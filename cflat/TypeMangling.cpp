#include "TypeMangling.h"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

struct TypeManglingAccess
{
    static void RememberArity(const LLVMBackend& compiler, std::string_view mangled,
                              size_t arity)
    {
        std::string_view base = mangled.substr(0, mangled.find('$'));
        if (base == "short" || base == "int" || base == "long"
            || base == "i8" || base == "u8" || base == "i16" || base == "u16"
            || base == "i32" || base == "u32" || base == "i64" || base == "u64"
            || base == "float" || base == "double" || base == "bool"
            || base == "char" || base == "string" || base == "void")
            return;
        compiler.gts.mangledArityHints[std::string(mangled)] = arity;
    }

    static std::optional<size_t> KnownMangledArity(const LLVMBackend& compiler,
                                                   std::string_view text, size_t position)
    {
        size_t bestLength = 0;
        std::optional<size_t> arity;
        for (const auto& [name, value] : compiler.gts.mangledArityHints)
        {
            if (name.size() > bestLength && text.substr(position).starts_with(name))
            {
                bestLength = name.size();
                arity = value;
            }
        }
        return arity;
    }

    static std::optional<size_t> TupleArity(const LLVMBackend& compiler,
                                             std::string_view text, size_t position)
    {
        size_t bestLength = 0;
        std::optional<size_t> arity;
        for (const auto& [name, args] : compiler.gts.tupleTypeArgs)
        {
            if (name.size() > bestLength && text.substr(position).starts_with(name))
            {
                bestLength = name.size();
                arity = args.size();
            }
        }
        return arity;
    }

    static std::optional<size_t> GenericArity(const LLVMBackend& compiler,
                                              std::string_view base)
    {
        std::string key(base);
        if (auto it = compiler.gts.genericStructTypeParams.find(key);
            it != compiler.gts.genericStructTypeParams.end())
            return it->second.size();
        if (auto it = compiler.gts.genericClassTemplates.find(key);
            it != compiler.gts.genericClassTemplates.end())
        {
            if (auto params = compiler.gts.genericStructTypeParams.find(key);
                params != compiler.gts.genericStructTypeParams.end())
                return params->second.size();
        }
        if (auto it = compiler.gts.genericInterfaceTypeParams.find(key);
            it != compiler.gts.genericInterfaceTypeParams.end())
            return it->second.size();
        if (auto it = compiler.gts.tupleTypeArgs.find(key);
            it != compiler.gts.tupleTypeArgs.end())
            return it->second.size();
        if (auto it = compiler.gts.genericFunctionTypeParams.find(key);
            it != compiler.gts.genericFunctionTypeParams.end())
            return it->second.size();
        if (key == "unique" && compiler.gts.coreGenericTemplates.count("unique") != 0)
            return 1;
        return std::nullopt;
    }
};

static std::string MangleGenericInstanceUnchecked(const LLVMBackend& compiler,
                                                  std::string_view base,
                                                  const std::vector<std::string>& args);

namespace
{
const std::unordered_map<std::string, std::string>& PrimitiveCanonicalNames()
{
    static const std::unordered_map<std::string, std::string> names = {
        { "i16", "short" },
        { "i32", "int" },
    };
    return names;
}

std::string CanonicalPrimitiveSpelling(std::string_view base)
{
    auto it = PrimitiveCanonicalNames().find(std::string(base));
    return it == PrimitiveCanonicalNames().end() ? std::string(base) : it->second;
}

std::string CanonicalTypeBase(const LLVMBackend& compiler, std::string_view base)
{
    std::string resolved = compiler.ResolveManglingAlias(std::string(base));
    return CanonicalPrimitiveSpelling(resolved);
}

bool IsDecimalInteger(std::string_view text)
{
    if (text.empty()) return false;
    size_t start = text.front() == '-' ? 1 : 0;
    if (start == text.size()) return false;
    for (size_t i = start; i < text.size(); i++)
        if (text[i] < '0' || text[i] > '9') return false;
    return true;
}

std::string MangleTypeArgument(const LLVMBackend& compiler, std::string_view typeName);
std::string RemangleTypeSpelling(const LLVMBackend& compiler, const TypeSpelling& spelling);

struct TypeParseCandidate
{
    TypeSpelling spelling;
    size_t position = 0;
};

using TypeParseCandidates = std::vector<TypeParseCandidate>;

TypeParseCandidates ParseMangledCandidates(const LLVMBackend& compiler,
                                            std::string_view text, size_t position);

struct TypePrefix
{
    int pointerDepth = 0;
    bool view = false;
    bool alias = false;
};

static bool ParseCountToken(std::string_view token, size_t& value)
{
    if (token.size() < 2 || token.front() != '.') return false;
    auto result = std::from_chars(token.data() + 1, token.data() + token.size(), value);
    return result.ec == std::errc{} && result.ptr == token.data() + token.size();
}

static bool ParseTypePrefix(std::string_view text, size_t& position, TypePrefix& prefix)
{
    while (position < text.size() && text[position] == '.')
    {
        size_t separator = text.find('$', position);
        std::string_view token = separator == std::string_view::npos
            ? text.substr(position) : text.substr(position, separator - position);
        if (token == ".p")
            ++prefix.pointerDepth;
        else if (token == ".v")
            prefix.view = true;
        else if (token == ".a")
            prefix.alias = true;
        else
            return false;
        if (separator == std::string_view::npos) return false;
        position = separator + 1;
    }
    return true;
}

static void ApplyTypePrefix(TypeSpelling& spelling, const TypePrefix& prefix)
{
    spelling.alias = spelling.alias || prefix.alias;
    if (prefix.view)
    {
        spelling.view = true;
        spelling.pointerDepth = 0;
    }
    if (prefix.pointerDepth != 0)
    {
        spelling.view = false;
        spelling.pointerDepth += prefix.pointerDepth;
    }
}

static void AddTypeCandidate(TypeSpelling spelling, size_t position,
                             const TypePrefix& prefix, TypeParseCandidates& out)
{
    ApplyTypePrefix(spelling, prefix);
    out.push_back({ std::move(spelling), position });
}

void ParseMangledArguments(const LLVMBackend& compiler, std::string_view text,
                           size_t position, size_t remaining,
                           std::vector<TypeSpelling> args, TypeParseCandidates& out,
                           std::string_view base, const TypePrefix& prefix)
{
    if (remaining == 0)
    {
        TypeSpelling result;
        result.base = std::string(base);
        result.args = std::move(args);
        result.unique = result.base == "unique";
        AddTypeCandidate(std::move(result), position, prefix, out);
        return;
    }

    auto candidates = ParseMangledCandidates(compiler, text, position);
    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it)
    {
        size_t nextPosition = it->position;
        if (remaining > 1)
        {
            if (nextPosition >= text.size() || text[nextPosition] != '$') continue;
            nextPosition++;
        }
        auto nextArgs = args;
        nextArgs.push_back(std::move(it->spelling));
        ParseMangledArguments(compiler, text, nextPosition, remaining - 1,
                              std::move(nextArgs), out, base, prefix);
    }
}

void ParseClosureCandidates(const LLVMBackend& compiler, std::string_view text,
                            size_t start, const TypePrefix& prefix,
                            TypeParseCandidates& out)
{
    size_t baseEnd = text.find('$', start);
    std::string_view base = baseEnd == std::string_view::npos
        ? text.substr(start) : text.substr(start, baseEnd - start);
    bool thin = base == "cfn";
    if (!thin && base != "fatfn") return;
    if (baseEnd == std::string_view::npos) return;

    size_t countStart = baseEnd + 1;
    size_t countEnd = text.find('$', countStart);
    std::string_view countToken = countEnd == std::string_view::npos
        ? text.substr(countStart) : text.substr(countStart, countEnd - countStart);
    size_t parameterCount = 0;
    if (!ParseCountToken(countToken, parameterCount) || parameterCount > 1024
        || countEnd == std::string_view::npos)
        return;
    size_t position = countEnd + 1;

    struct ClosureCandidate
    {
        std::vector<TypeSpelling> components;
        size_t position = 0;
    };
    std::vector<ClosureCandidate> partials;
    for (auto& ret : ParseMangledCandidates(compiler, text, position))
        partials.push_back({ { std::move(ret.spelling) }, ret.position });

    for (size_t i = 0; i < parameterCount; i++)
    {
        std::vector<ClosureCandidate> next;
        for (auto& partial : partials)
        {
            size_t parameterStart = partial.position;
            if (parameterStart >= text.size() || text[parameterStart] != '$') continue;
            parameterStart++;
            size_t moveEnd = text.find('$', parameterStart);
            if (moveEnd != std::string_view::npos
                && text.substr(parameterStart, moveEnd - parameterStart) == ".m")
                parameterStart = moveEnd + 1;
            auto candidates = ParseMangledCandidates(compiler, text, parameterStart);
            for (auto it = candidates.rbegin(); it != candidates.rend(); ++it)
            {
                auto components = partial.components;
                bool isMove = moveEnd != std::string_view::npos
                    && text.substr(partial.position + 1, moveEnd - partial.position - 1) == ".m";
                size_t paramPosition = it->position;
                it->spelling.move = isMove;
                components.push_back(std::move(it->spelling));
                next.push_back({ std::move(components), paramPosition });
            }
        }
        partials = std::move(next);
    }

    for (auto& partial : partials)
    {
        TypeSpelling closure;
        closure.base = thin ? "function" : "Lambda";
        closure.closure = true;
        closure.thinClosure = thin;
        closure.args = std::move(partial.components);
        AddTypeCandidate(std::move(closure), partial.position, prefix, out);
    }
}

TypeParseCandidates ParseMangledCandidates(const LLVMBackend& compiler,
                                            std::string_view text, size_t position)
{
    TypeParseCandidates out;
    if (position >= text.size()) return out;

    size_t firstEnd = text.find('$', position);
    std::string_view firstToken = firstEnd == std::string_view::npos
        ? text.substr(position) : text.substr(position, firstEnd - position);
    if (firstToken.size() > 1 && firstToken.front() == '.'
        && firstToken != ".p" && firstToken != ".v" && firstToken != ".a")
    {
        if (firstToken == ".p" || firstToken == ".v" || firstToken == ".a") return out;
        TypeSpelling value;
        if (firstToken[1] == 'n')
        {
            if (firstToken.size() < 3) return out;
            for (size_t i = 2; i < firstToken.size(); i++)
                if (firstToken[i] < '0' || firstToken[i] > '9') return out;
            value.base = "-" + std::string(firstToken.substr(2));
        }
        else
        {
            for (size_t i = 1; i < firstToken.size(); i++)
                if (firstToken[i] < '0' || firstToken[i] > '9') return out;
            value.base = std::string(firstToken.substr(1));
        }
        value.value = true;
        out.push_back({ std::move(value), firstEnd == std::string_view::npos
            ? text.size() : firstEnd });
        return out;
    }

    size_t basePosition = position;
    TypePrefix prefix;
    if (!ParseTypePrefix(text, basePosition, prefix)) return out;
    if (basePosition >= text.size()) return out;

    ParseClosureCandidates(compiler, text, basePosition, prefix, out);
    if (!out.empty()) return out;

    size_t separator = text.find('$', basePosition);
    std::string_view token = separator == std::string_view::npos
        ? text.substr(basePosition) : text.substr(basePosition, separator - basePosition);
    if (token.empty()) return out;
    if (token.front() == '.' || token.find_first_of("*[]") != std::string_view::npos) return out;

    TypeSpelling simple;
    simple.base = CanonicalPrimitiveSpelling(token);
    simple.canonicalPrimitive = simple.base;
    auto arity = TypeManglingAccess::KnownMangledArity(compiler, text, basePosition);
    if (!arity)
        arity = TypeManglingAccess::GenericArity(compiler, token);
    if (!arity && token == "tuple")
        arity = TypeManglingAccess::TupleArity(compiler, text, position);
    if (arity)
    {
        std::vector<TypeSpelling> args;
        if (separator == std::string_view::npos) return out;
        ParseMangledArguments(compiler, text, separator + 1, *arity,
                              std::move(args), out, simple.base, prefix);
        return out;
    }

    // Before templates are registered, a producer can still name a generic.
    // Preserve reversible parses until its arity becomes available.
    if (separator != std::string_view::npos && separator + 1 < text.size()
        && text[separator + 1] != '.')
    {
        std::vector<TypeSpelling> args;
        ParseMangledArguments(compiler, text, separator + 1, 1,
                              std::move(args), out, simple.base, prefix);
    }
    if (separator == std::string_view::npos)
        AddTypeCandidate(std::move(simple), text.size(), prefix, out);
    else
        AddTypeCandidate(std::move(simple), separator, prefix, out);
    return out;
}

std::string MangleTypeArgument(const LLVMBackend& compiler, std::string_view typeName)
{
    if (IsDecimalInteger(typeName))
        return typeName.front() == '-'
            ? ".n" + std::string(typeName.substr(1)) : "." + std::string(typeName);

    // Already encoded nested types are reparsed so primitive aliases inside them
    // are canonicalized as well.
    if (typeName.find('$') != std::string_view::npos)
    {
        TypeSpelling spelling;
        if (DemangleType(compiler, typeName, spelling))
            return RemangleTypeSpelling(compiler, spelling);
    }

    std::string result;
    size_t start = 0;
    if (typeName.starts_with("alias "))
    {
        result = ".a$";
        start = 6;
    }

    std::string decorations;
    size_t baseEnd = start;
    while (baseEnd < typeName.size() && typeName[baseEnd] != '*' && typeName[baseEnd] != '[')
        baseEnd++;
    for (size_t i = baseEnd; i < typeName.size(); i++)
    {
        if (typeName[i] == '*')
            decorations += ".p$";
        else if (typeName[i] == '[' && i + 1 < typeName.size() && typeName[i + 1] == ']')
        {
            decorations += ".v$";
            i++;
        }
        else
            decorations += typeName[i];
    }
    return result + decorations + CanonicalTypeBase(compiler, typeName.substr(start, baseEnd - start));
}

std::string MangleFunctionComponent(const LLVMBackend& compiler, std::string_view name,
                                    bool pointer, int pointerDepth, bool alias = false)
{
    LLVMBackend::TypeAndValue component;
    component.TypeName = name.empty() ? "void" : std::string(name);
    component.Pointer = pointer;
    component.ElemPointer = pointer && pointerDepth >= 2;
    component.PointerDepth = pointer ? std::max(pointerDepth, 1) : 0;
    component.IsAlias = alias;
    return MangleType(compiler, component);
}

std::string MangleFunctionType(const LLVMBackend& compiler,
                               const LLVMBackend::TypeAndValue& type)
{
    std::string result;
    if (type.IsArrayView)
        result = ".v$";
    else if (type.Pointer)
        for (int i = 0; i < std::max(type.ValuePointerDepth(), 1); i++) result += ".p$";
    result += type.IsThinFnPtr() ? "cfn" : "fatfn";
    result += "$." + std::to_string(type.FuncPtrParams.size()) + "$"
           + MangleFunctionComponent(compiler, type.FuncPtrReturnTypeName,
                                     type.FuncPtrReturnPointer,
                                     type.FuncPtrReturnPointerDepth,
                                     type.FuncPtrReturnAlias);
    for (const auto& param : type.FuncPtrParams)
    {
        result += "$";
        if (param.IsMove) result += ".m$";
        result += MangleFunctionComponent(compiler, param.TypeName,
                                          param.Pointer, param.PointerDepth);
    }
#ifndef NDEBUG
    TypeSpelling spelling;
    assert(DemangleType(compiler, result, spelling));
#endif
    return result;
}

std::string PrintTypeSpelling(const LLVMBackend& compiler, const TypeSpelling& spelling)
{
    if (spelling.closure)
    {
        std::string result = spelling.thinClosure ? "function<" : "Lambda<";
        if (!spelling.args.empty())
        {
            result += PrintTypeSpelling(compiler, spelling.args.front()) + "(";
            for (size_t i = 1; i < spelling.args.size(); i++)
            {
                if (i != 1) result += ", ";
                if (spelling.args[i].move) result += "move ";
                result += PrintTypeSpelling(compiler, spelling.args[i]);
            }
            result += ")>";
        }
        else
            result += ")>";
        if (spelling.view)
            result += "[]";
        else
            result += std::string(std::max(spelling.pointerDepth, 0), '*');
        return result;
    }
    if (spelling.encodedName == "__c_fn_ptr") return "<c_fn_ptr>";
    if (spelling.encodedName == "__closure_fat_ptr") return "<closure_fat_ptr>";
    if (spelling.value) return spelling.base;

    std::string result = spelling.alias ? "alias " : "";
    result += spelling.base;
    if (!spelling.args.empty())
    {
        result += "<";
        for (size_t i = 0; i < spelling.args.size(); i++)
        {
            if (i != 0) result += ", ";
            result += PrintTypeSpelling(compiler, spelling.args[i]);
        }
        result += ">";
    }
    if (spelling.view)
        result += "[]";
    else
        result += std::string(std::max(spelling.pointerDepth, 0), '*');
    return result;
}

std::string RemangleTypeSpelling(const LLVMBackend& compiler, const TypeSpelling& spelling)
{
    if (!spelling.encodedName.empty()) return spelling.encodedName;
    std::string result;
    if (spelling.alias) result += ".a$";
    if (spelling.view)
        result += ".v$";
    else
        for (int i = 0; i < std::max(spelling.pointerDepth, 0); i++) result += ".p$";
    if (spelling.value)
        return IsDecimalInteger(spelling.base) && spelling.base.front() == '-'
            ? result + ".n" + spelling.base.substr(1) : result + "." + spelling.base;
    if (spelling.closure)
    {
        result += spelling.thinClosure ? "cfn" : "fatfn";
        size_t parameterCount = spelling.args.empty() ? 0 : spelling.args.size() - 1;
        result += "$." + std::to_string(parameterCount);
        if (!spelling.args.empty())
        {
            result += "$" + RemangleTypeSpelling(compiler, spelling.args.front());
            for (size_t i = 1; i < spelling.args.size(); i++)
            {
                result += "$";
                if (spelling.args[i].move) result += ".m$";
                result += RemangleTypeSpelling(compiler, spelling.args[i]);
            }
        }
        return result;
    }
    result += CanonicalTypeBase(compiler, spelling.base);
    for (const auto& arg : spelling.args)
        result += "$" + RemangleTypeSpelling(compiler, arg);
    return result;
}

std::string MangleTypeFromSpelling(const LLVMBackend& compiler, TypeSpelling spelling)
{
    return RemangleTypeSpelling(compiler, spelling);
}
}

std::string_view MangledBase(std::string_view mangled)
{
    size_t position = 0;
    while (position < mangled.size() && mangled[position] == '.')
    {
        size_t separator = mangled.find('$', position);
        if (separator == std::string_view::npos) return {};
        position = separator + 1;
    }
    size_t separator = mangled.find('$', position);
    return separator == std::string_view::npos
        ? mangled.substr(position) : mangled.substr(position, separator - position);
}

bool IsThinMangledClosure(std::string_view mangled)
{
    return MangledBase(mangled) == "cfn";
}

static std::string MangleGenericInstanceUnchecked(const LLVMBackend& compiler,
                                                  std::string_view base,
                                                  const std::vector<std::string>& args)
{
    std::string result = CanonicalTypeBase(compiler, base);
    for (const auto& arg : args)
        result += "$" + MangleTypeArgument(compiler, arg);
    return result;
}

std::string MangleGenericInstance(const LLVMBackend& compiler, std::string_view base,
                                  const std::vector<std::string>& args)
{
    std::string result = MangleGenericInstanceUnchecked(compiler, base, args);
    TypeManglingAccess::RememberArity(compiler, result, args.size());
#ifndef NDEBUG
    TypeSpelling spelling;
    assert(DemangleType(compiler, result, spelling));
    std::vector<std::string> demangledArgs;
    demangledArgs.reserve(spelling.args.size());
    for (const auto& arg : spelling.args)
        demangledArgs.push_back(MangleTypeFromSpelling(compiler, arg));
    assert(MangleGenericInstanceUnchecked(compiler, spelling.base, demangledArgs) == result);
#endif
    return result;
}

std::string MangleClosureType(const LLVMBackend& compiler, bool isThin,
                              const std::string& ret, int retDepth,
                              const std::vector<std::pair<std::string, int>>& params)
{
    std::string result = isThin ? "cfn$." : "fatfn$.";
    result += std::to_string(params.size()) + "$"
           + MangleFunctionComponent(compiler, ret, retDepth > 0, retDepth);
    for (const auto& param : params)
        result += "$" + MangleFunctionComponent(compiler, param.first, param.second > 0,
                                                  param.second);
#ifndef NDEBUG
    TypeSpelling spelling;
    assert(DemangleType(compiler, result, spelling));
#endif
    return result;
}

std::string MangleType(const LLVMBackend& compiler,
                       const LLVMBackend::TypeAndValue& type)
{
    if (type.IsFunctionPointer) return MangleFunctionType(compiler, type);

    TypeSpelling spelling;
    if (!DemangleType(compiler, type.TypeName, spelling))
    {
        spelling.base = CanonicalTypeBase(compiler, type.TypeName);
    }
    spelling.alias = spelling.alias || type.IsAlias;
    if (type.IsArrayView)
    {
        spelling.view = true;
        spelling.pointerDepth = 0;
    }
    else if (type.Pointer)
        spelling.pointerDepth = type.ValuePointerDepth();
    return MangleTypeFromSpelling(compiler, std::move(spelling));
}

std::string MangleType(const LLVMBackend& compiler, const TypeSpelling& spelling)
{
    return MangleTypeFromSpelling(compiler, spelling);
}

bool DemangleType(const LLVMBackend& compiler, std::string_view mangled, TypeSpelling& out)
{
    out = {};
    if (mangled.empty()) return false;
    if (mangled == "__c_fn_ptr" || mangled == "__closure_fat_ptr")
    {
        out.base = std::string(mangled.substr(2));
        out.encodedName = std::string(mangled);
        return true;
    }

    for (auto& candidate : ParseMangledCandidates(compiler, mangled, 0))
    {
        if (candidate.position == mangled.size())
        {
            out = std::move(candidate.spelling);
            return true;
        }
    }
    return false;
}

std::string MangledGenericArgument(const LLVMBackend& compiler, std::string_view mangled,
                                   size_t index)
{
    TypeSpelling spelling;
    if (!DemangleType(compiler, mangled, spelling) || index >= spelling.args.size())
        return {};
    return MangleTypeFromSpelling(compiler, spelling.args[index]);
}

std::string SpellType(const LLVMBackend& compiler,
                      const LLVMBackend::TypeAndValue& type)
{
    TypeSpelling spelling;
    if (!DemangleType(compiler, type.TypeName, spelling))
        return type.TypeName;
    if (type.IsArrayView)
    {
        spelling.view = true;
        spelling.pointerDepth = 0;
    }
    else if (type.Pointer)
        spelling.pointerDepth = type.ValuePointerDepth();
    return PrintTypeSpelling(compiler, spelling);
}

std::string MangleFunctionSymbol(const LLVMBackend& compiler, std::string_view name,
                                 const LLVMBackend::TypeAndValue& returnType,
                                 const std::vector<LLVMBackend::TypeAndValue>& parameters,
                                 bool varargs)
{
    std::string result = "_" + std::string(name) + "$" + MangleType(compiler, returnType)
                       + "$." + std::to_string(parameters.size());
    for (const auto& parameter : parameters)
    {
        result += "$";
        if (parameter.IsMove) result += ".m$";
        result += MangleType(compiler, parameter);
    }
    if (varargs) result += "$.va";
    return result;
}

std::string MangleFunctionSymbol(const LLVMBackend& compiler,
                                 const FunctionSymbolSpelling& spelling)
{
    std::string result = "_" + spelling.name + "$"
                       + RemangleTypeSpelling(compiler, spelling.returnType) + "$"
                       + "." + std::to_string(spelling.parameters.size());
    for (size_t i = 0; i < spelling.parameters.size(); i++)
    {
        result += "$";
        if (i < spelling.move.size() && spelling.move[i]) result += ".m$";
        result += RemangleTypeSpelling(compiler, spelling.parameters[i]);
    }
    if (spelling.varargs) result += "$.va";
    return result;
}

bool DemangleFunctionSymbol(const LLVMBackend& compiler, std::string_view symbol,
                            FunctionSymbolSpelling& out)
{
    out = {};
    if (symbol.size() < 3 || symbol.front() != '_') return false;
    size_t nameEnd = symbol.find('$', 1);
    if (nameEnd == std::string_view::npos) return false;
    for (; nameEnd != std::string_view::npos;
         nameEnd = symbol.find('$', nameEnd + 1))
    {
        std::string candidateName(symbol.substr(1, nameEnd - 1));
        for (auto& ret : ParseMangledCandidates(compiler, symbol, nameEnd + 1))
        {
            if (ret.position >= symbol.size() || symbol[ret.position] != '$') continue;
            size_t countStart = ret.position + 1;
            size_t countEnd = symbol.find('$', countStart);
            std::string_view countToken = countEnd == std::string_view::npos
                ? symbol.substr(countStart) : symbol.substr(countStart, countEnd - countStart);
            size_t parameterCount = 0;
            if (!ParseCountToken(countToken, parameterCount) || parameterCount > 1024) continue;

            FunctionSymbolSpelling candidate;
            candidate.name = candidateName;
            candidate.returnType = std::move(ret.spelling);
            std::vector<TypeSpelling> params;
            std::vector<bool> moves;
            std::function<bool(size_t, size_t)> parseParams =
                [&](size_t index, size_t position) {
                    if (index == parameterCount)
                    {
                        if (position == symbol.size())
                        {
                            candidate.parameters = std::move(params);
                            candidate.move = std::move(moves);
                            return true;
                        }
                        if (symbol.substr(position) == "$.va")
                        {
                            candidate.parameters = std::move(params);
                            candidate.move = std::move(moves);
                            candidate.varargs = true;
                            return true;
                        }
                        return false;
                    }
                    if (position >= symbol.size() || symbol[position] != '$') return false;
                    size_t parameterStart = position + 1;
                    bool isMove = false;
                    size_t moveEnd = symbol.find('$', parameterStart);
                    if (moveEnd != std::string_view::npos
                        && symbol.substr(parameterStart, moveEnd - parameterStart) == ".m")
                    {
                        isMove = true;
                        parameterStart = moveEnd + 1;
                    }
                    for (auto& param : ParseMangledCandidates(compiler, symbol, parameterStart))
                    {
                        params.push_back(std::move(param.spelling));
                        moves.push_back(isMove);
                        if (parseParams(index + 1, param.position)) return true;
                        params.pop_back();
                        moves.pop_back();
                    }
                    return false;
                };
            size_t parameterPosition = countEnd == std::string_view::npos ? symbol.size() : countEnd;
            if (parseParams(0, parameterPosition))
            {
                out = std::move(candidate);
                return true;
            }
        }
    }
    return false;
}

std::string SpellFunctionSymbol(const LLVMBackend& compiler, std::string_view symbol)
{
    FunctionSymbolSpelling spelling;
    if (!DemangleFunctionSymbol(compiler, symbol, spelling))
    {
        TypeSpelling type;
        if (DemangleType(compiler, symbol, type))
            return PrintTypeSpelling(compiler, type);
        return std::string(symbol);
    }
    std::string result = spelling.name + "(";
    for (size_t i = 0; i < spelling.parameters.size(); i++)
    {
        if (i != 0) result += ", ";
        if (i < spelling.move.size() && spelling.move[i]) result += "move ";
        result += PrintTypeSpelling(compiler, spelling.parameters[i]);
    }
    return result + ")";
}

std::string LLVMBackend::TypeAndValue::ToUniqueString(const LLVMBackend& compiler) const
{
    return MangleType(compiler, *this);
}
