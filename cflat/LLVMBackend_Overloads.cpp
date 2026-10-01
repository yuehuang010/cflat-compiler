#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/Instrumentation/AddressSanitizer.h>
#include <llvm/Object/COFF.h>
#include <llvm/Object/Binary.h>
#include <llvm/Object/Archive.h>
#include <llvm/Object/COFFImportFile.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/TimeProfiler.h>
#include <llvm/Support/JSON.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticHandler.h>
#pragma warning(pop)
#include <antlr4-runtime.h>

#include "platform/GeneratedParser.h"
#include "LLVMBackend.h"
#include "MainListener.h"
#include "GrammarTreeListener.h"
#include <filesystem>
#include <optional>
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <charconv>

bool LLVMBackend::IsImplicitIntegerPointeePointerConversion(
    const TypeAndValue& from, const TypeAndValue& to) const
{
    // Array-view destinations use their own element-compatibility diagnostic. A view or fixed
    // array source decays only when the destination is an ordinary pointer.
    if (to.IsArrayView) return false;
    auto decayed = [](const TypeAndValue& value) {
        TypeAndValue result = value;
        if (result.IsArrayView || result.ConstArraySize > 0)
        {
            result.Pointer = true;
            result.IsArrayView = false;
            result.ConstArraySize = 0;
            result.ConstInnerDimensions.clear();
        }
        return result;
    };
    const TypeAndValue fromPointer = decayed(from);
    const TypeAndValue& toPointer = to;
    if (!fromPointer.Pointer || !toPointer.Pointer
        || fromPointer.IsCxxConstRef || toPointer.IsCxxConstRef
        || fromPointer.IsRvalueRef || toPointer.IsRvalueRef
        || fromPointer.IsCxxRefToPointer || toPointer.IsCxxRefToPointer)
        return false;

    auto resolvedPointee = [&](const TypeAndValue& value) {
        std::string resolved = ResolveTypeAlias(value.TypeName);
        std::string manglingAlias = ResolveManglingPointerAlias(resolved);
        if (!manglingAlias.empty()) resolved = std::move(manglingAlias);
        std::string backing = GetEnumBackingType(resolved);
        return backing.empty() ? resolved : ResolveTypeAlias(backing);
    };
    const std::string fromBase = resolvedPointee(fromPointer);
    const std::string toBase = resolvedPointee(toPointer);
    // `long` is the target's C long: C headers bind it as the same-width iN, so the two match.
    auto canonical = [](const std::string& name) {
        if (name == "long") return CanonicalPrimitiveTypeName(longBits_ == 64 ? "i64" : "i32");
        if (name == "ulong") return CanonicalPrimitiveTypeName(longBits_ == 64 ? "u64" : "u32");
        return CanonicalPrimitiveTypeName(name);
    };
    if (fromBase.empty() || toBase.empty() || fromBase == "void" || toBase == "void"
        || canonical(fromBase) == canonical(toBase))
        return false;

    // The pointee conversion applies only when both pointers have the same indirection depth.
    if ((fromPointer.ElemPointer || fromPointer.PointerDepth >= 2)
        != (toPointer.ElemPointer || toPointer.PointerDepth >= 2))
        return false;

    auto isInteger = [&](const std::string& name) {
        if (name == "bool") return true;
        TypeAndValue type;
        type.TypeName = name;
        return type.IsInteger() != -1 || type.IsUnsignedInteger() != -1;
    };
    if (!isInteger(fromBase) || !isInteger(toBase)) return false;

    const bool charFamily = (fromBase == "char" && (toBase == "i8" || toBase == "u8"))
        || (toBase == "char" && (fromBase == "i8" || fromBase == "u8"));
    return !charFamily;
}

void LLVMBackend::RejectImplicitIntegerPointeePointerConversion(
    const TypeAndValue& from, const TypeAndValue& to)
{
    if (!IsImplicitIntegerPointeePointerConversion(from, to)) return;
    if (from.IsSimd || to.IsSimd) return;

    auto decayed = [](const TypeAndValue& value) {
        TypeAndValue result = value;
        if (result.IsArrayView || result.ConstArraySize > 0)
        {
            result.Pointer = true;
            result.IsArrayView = false;
            result.ConstArraySize = 0;
            result.ConstInnerDimensions.clear();
        }
        return result;
    };
    const TypeAndValue fromPointer = decayed(from);
    const TypeAndValue& toPointer = to;

    auto resolvedPointee = [&](const TypeAndValue& value) {
        std::string resolved = ResolveTypeAlias(value.TypeName);
        std::string manglingAlias = ResolveManglingPointerAlias(resolved);
        if (!manglingAlias.empty()) resolved = std::move(manglingAlias);
        std::string backing = GetEnumBackingType(resolved);
        return backing.empty() ? resolved : ResolveTypeAlias(backing);
    };
    const std::string fromBase = resolvedPointee(fromPointer);
    const std::string toBase = resolvedPointee(toPointer);
    const bool fromDouble = fromPointer.ElemPointer || fromPointer.PointerDepth >= 2;
    const bool toDouble = toPointer.ElemPointer || toPointer.PointerDepth >= 2;
    const std::string fromType = fromBase + (fromDouble ? "**" : "*");
    const std::string toType = toBase + (toDouble ? "**" : "*");
    LogError(std::format("cannot convert '{}' to '{}' implicitly: the pointee types differ; "
                         "use an explicit cast '({})p'", fromType, toType, toType));
}

#if defined(__APPLE__)
// Step 3 (macOS self-contained link): harvest libSystem's exported symbols from
// the live dyld shared cache to synthesize a linker stub, so -o needs no SDK.
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <dlfcn.h>
#include <cstring>
#include <sys/sysctl.h>
#endif

// ---- Definitions moved out of LLVMBackend.h (Overloads) ----

// The '*' count a diagnostic prints for a value type. Depth caps at 2, and array/view/simd/fat
// shapes spell it about their ELEMENT rather than themselves.
static std::string PointerStars(const LLVMBackend::TypeAndValue& tv)
{
    if (!tv.Pointer) return "";
    bool deep = tv.ElemPointer || tv.PointerDepth >= 2;
    return (deep && tv.DepthIsAboutThisValue()) ? "**" : "*";
}

// C++ ranks `T*&` above `T*const&` for a modifiable lvalue pointer, and lets only `T*const&`
// bind a pointer rvalue. 0 = the overload C++ picks, 1 = viable but worse.
static int CxxRefToPointerScore(const LLVMBackend::NamedVariable& arg,
                                const LLVMBackend::TypeAndValue& param)
{
    const bool argIsLvalue = arg.Storage != nullptr && !arg.IsRvalue;
    return (argIsLvalue != param.IsCxxConstRef) ? 0 : 1;
}

static bool IsNullPointerConstantArgument(const LLVMBackend::NamedVariable& arg)
{
    if (arg.LiteralIdentity != "int")
        return false;
    auto* constant = llvm::dyn_cast_or_null<llvm::ConstantInt>(arg.Primary);
    return constant != nullptr && constant->isZero();
}

// CFlat spells an lvalue reference argument as `&value`; that address is the referred object,
// not a pointer value being converted to the reference's scalar type.
static bool IsCxxAddressOfObjectArgument(const LLVMBackend::NamedVariable& arg)
{
    if (!arg.TypeAndValue.Pointer || arg.TypeAndValue.PointerDepth != 0
        || arg.TypeAndValue.ElemPointer || arg.TypeAndValue.IsArrayView)
        return false;
    return llvm::isa_and_nonnull<llvm::AllocaInst>(arg.Primary)
        || llvm::isa_and_nonnull<llvm::GEPOperator>(arg.Primary)
        || llvm::isa_and_nonnull<llvm::GlobalVariable>(arg.Primary);
}

static bool IsCxxAddressOfReferent(const LLVMBackend::NamedVariable& arg,
                                   const std::string& referent)
{
    return IsCxxAddressOfObjectArgument(arg)
        && (arg.TypeAndValue.TypeName == referent || arg.InferSourceTypeName == referent);
}

// A declared signature in CFlat spelling, e.g. "int(char*, ...)". Spells each type the way the
// "no overload matches" candidate list does (SpellType, PointerStars as the fallback), so `const`
// and the calling convention are not shown - neither can be why two signatures conflict.
std::string LLVMBackend::SpellDeclaredSignature(const TypeAndValue& returnType,
                                               const std::vector<TypeAndValue>& parameters,
                                               bool varargs) const
{
        auto spell = [&](const TypeAndValue& tv) {
            std::string result = SpellType(*this, tv);
            if (result.empty()) result = tv.TypeName + PointerStars(tv);
            return result;
        };

        std::string signature = spell(returnType) + "(";
        for (size_t i = 0; i < parameters.size(); i++)
        {
            if (i > 0) signature += ", ";
            if (parameters[i].IsMove) signature += "move ";
            signature += spell(parameters[i]);
        }
        if (varargs) signature += parameters.empty() ? "..." : ", ...";
        return signature + ")";
    }

bool LLVMBackend::ArgumentNarrowsParameter(const NamedVariable& arg, const TypeAndValue& param) const
{
        if (arg.TypeAndValue.Pointer || param.Pointer) return false;

        auto resolve = [&](const std::string& tn) -> std::string {
            auto it = enumBackingTypes.find(tn);
            return (it != enumBackingTypes.end()) ? it->second : tn;
        };
        TypeAndValue argType = arg.TypeAndValue;
        TypeAndValue paramType = param;
        argType.TypeName = resolve(argType.TypeName);
        paramType.TypeName = resolve(paramType.TypeName);

        // A 'bool' parameter converts rather than narrows - see ArgumentConvertsToBoolParameter.
        if (paramType.TypeName == "bool") return false;

        int paramBits = paramType.IsInteger();
        if (paramBits == -1) return false;

        int argBits = argType.IsInteger();
        // An unnamed primitive argument carries its width only in the lowered type.
        if (argBits == -1 && argType.TypeName.empty() && arg.BaseType && arg.BaseType->isIntegerTy()
            && !arg.BaseType->isIntegerTy(1))
            argBits = (int)arg.BaseType->getIntegerBitWidth();
        if (argBits == -1) return false;

        return argBits > paramBits;
}

bool LLVMBackend::ArgumentConvertsToBoolParameter(const NamedVariable& arg, const TypeAndValue& param) const
{
        if (param.Pointer || param.TypeName != "bool") return false;
        if (arg.TypeAndValue.Pointer) return false;

        TypeAndValue argType = arg.TypeAndValue;
        auto it = enumBackingTypes.find(argType.TypeName);
        if (it != enumBackingTypes.end()) argType.TypeName = it->second;
        if (argType.IsInteger() != -1) return true;

        // An unnamed integer argument (a literal, a plain 'int' local) is proved by its lowered
        // type. A pointer or a floating point value is NOT accepted here.
        return argType.TypeName.empty() && arg.BaseType && arg.BaseType->isIntegerTy()
            && !arg.BaseType->isIntegerTy(1);
}

// Record C++ identities before literal values lose source spelling and narrow during lowering.
std::string LLVMBackend::LiteralIdentityForOverload(std::string_view text, bool* suffixedInteger)
{
        if (suffixedInteger != nullptr) *suffixedInteger = false;
        if (text.size() >= 3 && text.back() == '\'')
        {
            if (text.front() == '\'') return "char";
            if (text.size() >= 4 && text[1] == '\'')
            {
                if (text.front() == 'L') return "wchar";
                if (text.front() == 'u') return "c16";
                if (text.front() == 'U') return "c32";
            }
        }
        const bool negative = !text.empty() && text.front() == '-';
        const bool signedLiteral = negative || (!text.empty() && text.front() == '+');
        std::string_view digits = signedLiteral ? text.substr(1) : text;
        if (digits.empty())
            return "";
        if (!std::isdigit((unsigned char)digits.front()) && digits.front() != '.')
            return "";
        const bool hexadecimal = digits.size() > 2 && digits[0] == '0'
            && (digits[1] == 'x' || digits[1] == 'X');
        const bool hasFloatSuffix = digits.back() == 'f' || digits.back() == 'F';
        const bool hasLongFloatSuffix = digits.back() == 'l' || digits.back() == 'L';
        std::string_view floatingDigits = digits;
        if (hasFloatSuffix || hasLongFloatSuffix) floatingDigits.remove_suffix(1);
        const bool floatingLiteral = floatingDigits.find('.') != std::string_view::npos
            || (hexadecimal
                ? floatingDigits.find_first_of("pP") != std::string_view::npos
                : floatingDigits.find_first_of("eE") != std::string_view::npos);
        if (floatingLiteral)
            return hasLongFloatSuffix ? "" : hasFloatSuffix ? "float" : "double";
        if (hasFloatSuffix)
            return "";
        // Integer suffix (u/U, l/L, ll/LL in the grammar's orders): only a canonical spelling has one.
        bool unsignedSuffix = false;
        int longSuffix = 0;
        {
            char firstL = 0;
            while (!digits.empty())
            {
                const char c = digits.back();
                if (c == 'u' || c == 'U')
                {
                    if (unsignedSuffix) return "";
                    unsignedSuffix = true;
                }
                else if (c == 'l' || c == 'L')
                {
                    if (longSuffix > 0 && c != firstL) return "";
                    if (++longSuffix > 2) return "";
                    firstL = c;
                }
                else
                    break;
                digits.remove_suffix(1);
            }
        }
        const bool suffixed = unsignedSuffix || longSuffix > 0;
        int base = 10;
        bool hex = false;
        if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
        {
            base = 16;
            hex = true;
            digits.remove_prefix(2);
        }
        else if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'b' || digits[1] == 'B'))
        {
            base = 2;
            digits.remove_prefix(2);
        }
        else if (digits.size() > 1 && digits[0] == '0')
        {
            base = 8;
            digits.remove_prefix(1);
        }
        if (digits.empty())
            return "";

        uint64_t value = 0;
        auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
        if (parsed.ec != std::errc() || parsed.ptr != digits.data() + digits.size())
            return "";   // an unsupported suffix, an operator, or out of u64 range

        if (suffixed)
        {
            // C++ [lex.icon]: the first type of the suffix's list that holds the value. A decimal
            // literal without `u` never takes an unsigned type; the sign of `-1L` is an operator.
            struct Candidate { const char* name; int valueBits; bool isUnsigned; };
            const int longValueBits = longBits_ - 1;
            const Candidate intType{ "int", 31, false }, uintType{ "u32", 32, true };
            const Candidate longType{ "long", longValueBits, false };
            const Candidate ulongType{ "ulong", longBits_, true };
            const Candidate llType{ "i64", 63, false }, ullType{ "u64", 64, true };
            const bool decimal = base == 10;
            std::vector<Candidate> list;
            if (unsignedSuffix)
            {
                if (longSuffix == 0) list = { uintType, ulongType, ullType };
                else if (longSuffix == 1) list = { ulongType, ullType };
                else list = { ullType };
            }
            else if (decimal)
                list = longSuffix == 1 ? std::vector<Candidate>{ longType, llType }
                                       : std::vector<Candidate>{ llType };
            else
                list = longSuffix == 1
                    ? std::vector<Candidate>{ longType, ulongType, llType, ullType }
                    : std::vector<Candidate>{ llType, ullType };
            for (const Candidate& candidate : list)
                if (candidate.valueBits >= 64 || value < (uint64_t(1) << candidate.valueBits))
                {
                    if (suffixedInteger != nullptr) *suffixedInteger = true;
                    return candidate.name;
                }
            return "";
        }

        const uint64_t intMax = (uint64_t)std::numeric_limits<int32_t>::max();
        if (negative ? value <= intMax + 1 : value <= intMax)
            return "int";
        // A hex literal inside u32 range lowers as an i32 bit pattern; a 64-bit identity would
        // sign-extend that pattern into the wider parameter.
        if (hex && !negative && value <= 0xFFFFFFFFull)
            return "";
        const uint64_t i64Max = (uint64_t)std::numeric_limits<int64_t>::max();
        if (negative ? value > i64Max + 1 : value > i64Max)
            return "";
        return longBits_ == 64 ? "long" : "i64";
}

std::string LLVMBackend::IntegerParameterIdentity(const TypeAndValue& param) const
{
        if (param.Pointer || param.IsArrayView || param.ConstArraySize > 0 || param.IsSimd
            || param.IsFunctionPointer || param.IsInterface || param.IsAlias
            || param.IsRvalueRef || param.IsCxxRefToPointer)
            return "";
        std::string name = param.TypeName;
        if (auto it = enumBackingTypes.find(name); it != enumBackingTypes.end())
            name = it->second;
        TypeAndValue probe;
        probe.TypeName = name;
        if (name == "bool" || probe.IsInteger() == -1)
            return "";
        return name;
}

// C++ member ranking needs declared integer identity, which widths cannot distinguish on Windows.
std::string LLVMBackend::CxxIntegerParameterIdentity(const FunctionSymbol& candidate, size_t index,
                                                      const TypeAndValue& param) const
{
        if (param.Pointer || param.IsArrayView || param.ConstArraySize > 0 || param.IsSimd
            || param.IsFunctionPointer || param.IsInterface || param.IsCxxRefToPointer)
            return "";
        auto identityFromSpelling = [&](const std::string& spelling) -> std::string {
            std::string compact;
            compact.reserve(spelling.size());
            for (char c : spelling)
                if (!std::isspace((unsigned char)c)) compact += c;
            while (compact.ends_with("&&") || compact.ends_with('&'))
                compact.resize(compact.size() - (compact.ends_with("&&") ? 2 : 1));
            if (compact.starts_with("const")) compact.erase(0, 5);
            if (compact.starts_with("volatile")) compact.erase(0, 8);
            if (compact.find('*') != std::string::npos) return {};
            if (const char* identity = CxxCompactIntegerSpellingToCflat(compact)) return identity;
            return {};
        };
        const std::string declaredSpelling = CxxReferenceParameterSpelling(candidate, index);
        if (declaredSpelling.find('*') != std::string::npos) return "";
        if (std::string identity = identityFromSpelling(declaredSpelling); !identity.empty())
            return identity;

        const std::string integerIdentity = IntegerParameterIdentity(param);
        return integerIdentity.empty() ? param.TypeName : integerIdentity;
}

/*
 * A call argument keeps its integer identity in one of three places: TypeName (unsigned values and
 * enums keep it), InferSourceTypeName (the call site drops a signed primitive's TypeName on
 * purpose), or LiteralIdentity (an unsuffixed literal). A recorded name must agree with the
 * lowered width - a literal may lower narrower than the identity it ranks as, never wider.
 */
std::string LLVMBackend::IntegerArgumentIdentity(const NamedVariable& arg, bool cxxCandidate) const
{
        const TypeAndValue& tv = arg.TypeAndValue;
        // Native ulong/u64 and long/i64 are distinct names of one width: a C++ suffix identity would tie them.
        const std::string& literalIdentity = !cxxCandidate && arg.LiteralIdentitySuffixed
            && arg.LiteralIdentity != "long" ? std::string() : arg.LiteralIdentity;
        if (tv.Pointer || tv.IsArrayView || tv.ConstArraySize > 0 || tv.IsSimd
            || tv.IsFunctionPointer || tv.IsInterface)
            return "";
        if (arg.BaseType == nullptr || !arg.BaseType->isIntegerTy() || arg.BaseType->isIntegerTy(1))
            return "";
        const int loweredBits = (int)arg.BaseType->getIntegerBitWidth();

        auto resolved = [&](const std::string& name) -> std::string {
            auto it = enumBackingTypes.find(name);
            return it != enumBackingTypes.end() ? it->second : name;
        };
        auto integerBits = [](const std::string& name) {
            TypeAndValue probe;
            probe.TypeName = name;
            return name == "bool" ? -1 : probe.IsInteger();
        };

        // A suffixed unsigned literal carries a lowered name (`1UL` -> u64) that is not its C++
        // type (`unsigned long`); the exact spelling recorded from the source wins at equal width.
        if (cxxCandidate && !literalIdentity.empty() && (!tv.TypeName.empty() || !arg.InferSourceTypeName.empty())
            && integerBits(literalIdentity) == loweredBits)
            return literalIdentity;
        for (const std::string* recorded : { &tv.TypeName, &arg.InferSourceTypeName })
        {
            if (recorded->empty())
                continue;
            std::string name = resolved(*recorded);
            return integerBits(name) == loweredBits ? name : "";
        }
        if (!literalIdentity.empty() && integerBits(literalIdentity) >= loweredBits)
            return literalIdentity;
        return "";
}

// Cost of binding an integer argument to an integer parameter: 0 identity-exact, then
// value-preserving promotion (C++ integral promotion to `int` first, then the narrowest
// destination), then conversion (same width different identity, sign change, narrowing).
static constexpr int kIntegerConversionCost = 1000;   // above every promotion cost (1 + bits)

int LLVMBackend::RankIntegerConversion(const std::string& argIdentity, const std::string& paramIdentity,
                                       bool cxxCandidate)
{
        if (CanonicalPrimitiveTypeName(argIdentity) == CanonicalPrimitiveTypeName(paramIdentity))
            return 0;

        TypeAndValue argType;
        argType.TypeName = argIdentity;
        TypeAndValue paramType;
        paramType.TypeName = paramIdentity;
        const int argBits = argType.IsInteger();
        const int paramBits = paramType.IsInteger();
        const bool argUnsigned = argType.IsUnsignedInteger() != -1;
        const bool paramUnsigned = paramType.IsUnsignedInteger() != -1;

        // Same signedness, or unsigned into a STRICTLY wider signed type.
        const bool promotion = argBits < paramBits && (argUnsigned == paramUnsigned || argUnsigned);
        if (!promotion)
            return kIntegerConversionCost;
        if (CanonicalPrimitiveTypeName(paramIdentity) == "int")
            return 1;
        // C++ [conv.prom] promotes only to int: any other widening into a C++ parameter is a
        // conversion (LLP64 'long' -> 'long long' ties with 'long' -> 'unsigned long', as in clang).
        if (cxxCandidate)
            return kIntegerConversionCost;
        return 1 + paramBits;
}

/*
 * Ranks only what C++ ranks without further context: arithmetic to arithmetic (by value or
 * through a `const T&`), a class value to its own class, and a class value through a conversion
 * operator to an arithmetic parameter. Enums, pointers, views, other references and every
 * other class binding stay -1, so a candidate carrying one is never compared. Rank -2 marks the
 * implicit object parameter of a member, equal only to another member's.
 */
// Recover the original decorated declaration name from a generated default-argument wrapper.
std::string LLVMBackend::CxxDeclarationLinkageName(std::string name)
{
        constexpr std::string_view prefix = "__cflat_dflt_";
        if (!name.starts_with(prefix)) return name;
        name.erase(0, prefix.size());
        const size_t suffix = name.rfind('_');
        if (suffix == std::string::npos || suffix + 1 == name.size()
            || !std::all_of(name.begin() + suffix + 1, name.end(), [](char c) {
                   return c >= '0' && c <= '9'; }))
            return name;
        name.resize(suffix);
        std::string linkage;
        linkage.reserve(name.size());
        auto hexValue = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        for (size_t i = 0; i < name.size(); ++i)
        {
            if (name[i] == '_' && i + 2 < name.size())
            {
                const int high = hexValue(name[i + 1]);
                const int low = hexValue(name[i + 2]);
                if (high >= 0 && low >= 0)
                {
                    linkage += (char)((high << 4) | low);
                    i += 2;
                    continue;
                }
            }
            linkage += name[i];
        }
        return linkage;
}

/*
 * A class `source` converting to a class `target` both through an implicit `operator target()`
 * and an implicit `target(source&)` constructor whose object bindings are equally qualified:
 * C++ copy-initialization finds the two equally good (clang: "conversion ... is ambiguous").
 * Returns the two spellings, or nothing when either is missing or one binding is better.
 */
std::vector<std::string> LLVMBackend::CxxClassConversionTies(const std::string& source,
                                                             const std::string& target) const
{
        CxxClassConversionPair pair;
        if (!FindCxxClassConversionPair(source, target, pair) || pair.operatorConst != pair.ctorConst)
            return {};
        return { "operator " + pair.targetSpelling + "()" + (pair.operatorConst ? " const" : ""),
                 pair.targetSpelling + "(" + pair.ctorParam + ")" };
}

// A non-const `operator target()` against a `target(const source&)` constructor: the operator's
// implied object binds `source&`, less cv-qualified, so C++ calls the operator (3.2.6).
bool LLVMBackend::CxxConversionOperatorBindsBetter(const std::string& source,
                                                   const std::string& target) const
{
        CxxClassConversionPair pair;
        return FindCxxClassConversionPair(source, target, pair) && !pair.operatorConst && pair.ctorConst;
}

/*
 * The implicit `operator target()` of class `source` (no ref-qualifier) and the implicit
 * `target(source&)` / `target(const source&)` constructor, with each binding's constness.
 */
bool LLVMBackend::FindCxxClassConversionPair(const std::string& source, const std::string& target,
                                             CxxClassConversionPair& out) const
{
        std::string sourceSpelling;
        std::string targetSpelling;
        auto sourceIt = cxxRecordEntries_.find(source);
        auto targetIt = cxxRecordEntries_.find(target);
        if (sourceIt == cxxRecordEntries_.end() || targetIt == cxxRecordEntries_.end()
            || !CxxSpellingForCflatType(source, sourceSpelling)
            || !CxxSpellingForCflatType(target, targetSpelling))
            return false;
        sourceSpelling = SqueezeCxxSpelling(sourceSpelling);
        targetSpelling = SqueezeCxxSpelling(targetSpelling);
        int operatorConst = -1;
        for (const auto& m : sourceIt->second.members)
            if (m.isConversion && !m.isExplicit && !m.isDeleted
                && m.refQualifier == cflat_cinterop::CxxRefQualifierNone
                && SqueezeCxxSpelling(m.retType) == targetSpelling)
                operatorConst = m.isConst ? 1 : 0;
        if (operatorConst < 0)
            return false;
        // The equally qualified constructor first: it is the one that ties with the operator.
        for (int ctorConst : { operatorConst, 1 - operatorConst })
            for (const auto& m : targetIt->second.members)
            {
                if (m.kind != cflat_cinterop::RawCxxMember::Constructor || m.isExplicit
                    || m.isDeleted || m.paramTypes.size() != 2
                    || SqueezeCxxSpelling(m.paramTypes[1])
                           != SqueezeCxxSpelling((ctorConst ? "const " : "") + sourceSpelling + " &"))
                    continue;
                out.operatorConst = operatorConst;
                out.ctorConst = ctorConst;
                out.targetSpelling = targetSpelling;
                out.ctorParam = m.paramTypes[1];
                return true;
            }
        return false;
}

std::vector<LLVMBackend::CxxConversionRank> LLVMBackend::RankCxxConversionSequences(
        const std::vector<NamedVariable>& arguments, const FunctionSymbol& candidate)
{
        auto integerBits = [](const std::string& name) {
            TypeAndValue probe;
            probe.TypeName = name;
            return probe.IsInteger();
        };
        // Character types promote to their underlying type, not to `int`; leave them unjudged.
        auto arithmeticName = [&](const std::string& name) -> std::string {
            if (name == "bool" || name == "float" || name == "double") return name;
            if (name == "wchar" || name == "c16" || name == "c32" || name == "c8") return "";
            return integerBits(name) > 0 ? CanonicalPrimitiveTypeName(name) : "";
        };
        auto argumentIdentity = [&](const NamedVariable& arg) -> std::string {
            const TypeAndValue& tv = arg.TypeAndValue;
            if (tv.Pointer || tv.ElemPointer || tv.IsArrayView || tv.ConstArraySize > 0 || tv.IsSimd
                || tv.IsFunctionPointer || tv.IsInterface || tv.IsScopedEnum
                || enumBackingTypes.count(tv.TypeName) != 0 || IsScopedEnumTypeName(tv.TypeName)
                || arg.BaseType == nullptr)
                return "";
            if (arg.BaseType->isIntegerTy(1))
                return "bool";
            if (arg.BaseType->isIntegerTy())
                return arithmeticName(IntegerArgumentIdentity(arg));
            if (!arg.BaseType->isFloatTy() && !arg.BaseType->isDoubleTy())
                return "";
            // An unsuffixed floating literal is a C++ `double` even where it lowers as a float.
            for (const std::string* recorded : { &arg.LiteralIdentity, &tv.TypeName, &arg.InferSourceTypeName })
                if (*recorded == "float" || *recorded == "double")
                    return *recorded;
            if (!tv.TypeName.empty() || !arg.InferSourceTypeName.empty())
                return "";
            return arg.BaseType->isFloatTy() ? "float" : "double";
        };
        auto arithmeticRank = [&](const std::string& from, const std::string& to) {
            if (from == to) return 0;
            if (from == "float" && to == "double") return 1;
            // Integral promotion: bool and every integer type narrower than int, to int.
            if (to == "int" && (from == "bool" || (integerBits(from) > 0 && integerBits(from) < 32)))
                return 1;
            return 2;
        };

        std::vector<CxxConversionRank> ranks(arguments.size());
        for (size_t i = 0; i < arguments.size() && i < candidate.Parameters.size(); ++i)
        {
            const NamedVariable& arg = arguments[i];
            const TypeAndValue& param = candidate.Parameters[i];
            CxxConversionRank& out = ranks[i];
            // Rank the implicit object parameter for imported C++ members.
            if (candidate.IsMethod && i == 0
                && (param.VariableName.ends_with("__") || (candidate.IsCxx && param.VariableName == "this")))
            {
                // Judged for a receiver of the member's class or of a class derived from it; a
                // default-argument wrapper carries its declaration's qualifiers.
                const std::string& receiver = arg.TypeAndValue.TypeName;
                if (receiver == param.TypeName
                    || (IsCxxRecord(receiver) && IsCxxRecord(param.TypeName)
                        && IsCxxBaseOf(param.TypeName, receiver)))
                {
                    out.rank = -2;
                    const std::string linkage = CxxDeclarationLinkageName(candidate.UniqueName);
                    out.second = linkage.starts_with("_ZNK")
                            || linkage.find("@@QEBA") != std::string::npos
                            || linkage.find("@@UEBA") != std::string::npos
                        ? 1 : 0;
                    out.objectRef = candidate.CxxRefQualifier;
                    out.from = param.TypeName;
                    out.cxxViable = true;
                }
                continue;
            }
            // A pointer argument to a single-level pointer parameter: identity or qualification
            // (exact match), derived-to-base or to `void*` (conversion). Anything else unjudged.
            if (param.Pointer && !param.IsAlias && !param.IsRvalueRef && !param.ElemPointer
                && !param.IsArrayView && !param.IsFunctionPointer && !param.IsInterface
                && !param.IsCxxRefToPointer && param.PointerDepth <= 1)
            {
                const TypeAndValue& pt = arg.TypeAndValue;
                // `nullptr`: a null pointer conversion to any pointer type, no tie-breaker.
                if (!pt.Pointer && pt.TypeName.empty()
                    && llvm::isa_and_nonnull<llvm::ConstantPointerNull>(arg.Primary))
                {
                    out.rank = 2;
                    out.cxxViable = true;
                    out.from = "nullptr";
                    continue;
                }
                // A narrow string literal is `const char[N]`: `const char*` is its identity after
                // array-to-pointer, and the deprecated drop to `char*` binds worse.
                if (arg.IsStringLiteral && pt.TypeName.empty() && !pt.Pointer)
                {
                    if (param.TypeName == "char")
                    {
                        out.rank = 0;
                        out.cvBase = "char";
                        out.cv = CxxReferenceParameterSpelling(candidate, i).rfind("const ", 0) == 0 ? 0 : 1;
                        out.cxxViable = true;
                        out.from = "char";
                    }
                    continue;
                }
                // A primitive pointee is recorded only as the declared source name.
                const std::string pointee = CanonicalPrimitiveTypeName(
                    !pt.TypeName.empty() ? pt.TypeName : arg.InferSourceTypeName);
                if (!pt.Pointer || pt.ElemPointer || pt.IsArrayView || pt.ConstArraySize > 0
                    || pt.IsFunctionPointer || pt.IsInterface || pt.PointerDepth > 1
                    || param.TypeName.empty() || pointee == "void"
                    || IsNullPointerConstantArgument(arg))
                    continue;
                if (IsCxxRecord(pt.TypeName) && IsCxxRecord(param.TypeName)
                    && (pt.TypeName == param.TypeName
                        || IsCxxBaseOf(param.TypeName, pt.TypeName)))
                {
                    const bool derivedToBase = pt.TypeName != param.TypeName;
                    const bool addsOrDropsPointeeConst =
                        pt.IsCxxPointeeConst != param.IsCxxPointeeConst;
                    // Dropping pointee const is not viable in C++; ranked only so a lone such
                    // candidate reaches the lowering diagnostic that refuses it.
                    const bool dropsPointeeConst = pt.IsCxxPointeeConst && !param.IsCxxPointeeConst;
                    out.rank = dropsPointeeConst
                        ? 2 : (derivedToBase ? 2 : 0) + (addsOrDropsPointeeConst ? 1 : 0);
                    out.cxxViable = pt.PointerDepth == 1 && !dropsPointeeConst;
                    out.from = pt.TypeName;
                    out.toClass = param.TypeName;
                    continue;
                }
                if (pointee.empty())
                    continue;
                if (pointee == CanonicalPrimitiveTypeName(param.TypeName))
                {
                    out.rank = 0;
                    out.cvBase = pointee;
                    out.cv = CxxReferenceParameterSpelling(candidate, i).rfind("const ", 0) == 0 ? 1 : 0;
                }
                else if (param.TypeName == "void")
                    out.rank = 2;
                else if (IsCxxRecord(pointee) && IsCxxRecord(param.TypeName)
                         && IsCxxBaseOf(param.TypeName, pointee))
                    out.rank = 2;
                else
                    continue;
                // Qualification rides on any pointer sequence: 3.2.5 compares T* against const T*
                // for identity and for void* / derived-to-base conversions alike.
                if (out.rank == 2)
                {
                    out.toClass = param.TypeName;
                    out.cvBase = param.TypeName;
                    out.cv = CxxReferenceParameterSpelling(candidate, i).rfind("const ", 0) == 0 ? 1 : 0;
                }
                // C++ viability needs the recorded single level; CFlat's own match proved the rest.
                out.cxxViable = pt.PointerDepth == 1;
                out.from = pointee;
                continue;
            }
            // A C++ reference parameter is an address-passed alias; only its referent is ranked.
            const bool reference = param.IsAlias || param.IsRvalueRef;
            if ((param.Pointer && !reference) || param.ElemPointer || param.IsArrayView || param.ConstArraySize > 0
                || param.IsSimd || param.IsFunctionPointer || param.IsInterface
                || param.IsCxxRefToPointer || param.IsScopedEnum
                || enumBackingTypes.count(param.TypeName) != 0 || IsScopedEnumTypeName(param.TypeName))
                continue;
            const bool constReference = reference && !param.IsRvalueRef
                && (param.IsCxxConstRef
                    || CxxReferenceParameterSpelling(candidate, i).rfind("const ", 0) == 0);

            const TypeAndValue& at = arg.TypeAndValue;
            const size_t paramIndex = i;
            const bool cxxByValueParam = candidate.IsCxx
                && paramIndex < candidate.Recipe.paramSlots.size()
                && candidate.Recipe.paramSlots[paramIndex].kind == AbiSlot::ByVal;
            const bool cxxIndirectValueParam = candidate.IsCxx
                && candidate.CxxAbi.valid
                && paramIndex < candidate.CxxAbi.params.size()
                && candidate.CxxAbi.params[paramIndex].kind == cflat_cinterop::RawAbiSlot::Indirect;
            if (IsCxxRecord(at.TypeName) && IsCxxDerivedToBaseValue(at, param))
            {
                out.rank = 2;
                out.cxxViable = true;
                out.from = at.TypeName;
                out.toClass = param.TypeName;
                if (reference && !param.IsRvalueRef)
                {
                    out.cvBase = param.TypeName;   // 3.2.6 on a derived-to-base reference too
                    out.cv = constReference ? 1 : 0;
                }
                continue;
            }
            if (IsCxxRecord(param.TypeName)
                && CanImplicitlyConstructCxxClass(arg, param,
                                                  cxxByValueParam || cxxIndirectValueParam))
            {
                out.rank = 3;
                out.cxxViable = true;
                out.from = argumentIdentity(arg);
                if (reference)
                {
                    out.refTarget = param.TypeName;
                    out.refBind = param.IsRvalueRef ? 1 : constReference ? 2 : 0;
                }
                if (((!reference && !param.Pointer) || constReference) && !at.Pointer
                    && IsCxxRecord(at.TypeName))
                    out.ambiguousOperators = CxxClassConversionTies(at.TypeName, param.TypeName);
                continue;
            }
            if (!at.Pointer && !at.ElemPointer && !at.TypeName.empty() && IsCxxRecord(at.TypeName))
            {
                if (param.TypeName == at.TypeName)
                {
                    // Same class: an identity binding. C++ viability is proved only for a
                    // `const T&`, which binds any object of the class.
                    if (!param.IsRvalueRef)
                        out.rank = 0;
                    out.cxxViable = constReference;
                    if (reference && !param.IsRvalueRef)
                    {
                        out.cvBase = at.TypeName;
                        out.cv = constReference ? 1 : 0;
                    }
                    continue;
                }
                if (reference && !constReference)
                    continue;
                const std::string to = arithmeticName(param.TypeName);
                if (to.empty())
                    continue;
                TypeAndValue dest;
                dest.TypeName = param.TypeName;
                bool needsStandard = false;
                bool ambiguous = false;
                std::vector<std::string> ambiguousOperators;
                const std::string op = CxxConversionOperatorTo(at.TypeName, dest, false, &needsStandard,
                                                               &ambiguous, &ambiguousOperators);
                out.from = at.TypeName;
                if (ambiguous)
                {
                    // An ambiguous conversion sequence: user-defined, indistinguishable from any
                    // other user-defined sequence ([over.best.ics]/10).
                    out.rank = 3;
                    out.cxxViable = true;
                    out.ambiguousOperators = std::move(ambiguousOperators);
                }
                else if (!op.empty())
                {
                    out.rank = 3;
                    out.cxxViable = true;
                    out.userFunction = op;
                    const size_t spelled = op.rfind("operator ");
                    const std::string from = arithmeticName(op.substr(spelled + 9));
                    out.second = !needsStandard ? 0 : from.empty() ? 2 : arithmeticRank(from, to);
                }
                continue;
            }

            if (reference && !constReference)
                continue;
            const std::string to = arithmeticName(
                CxxIntegerParameterIdentity(candidate, i, param));
            // An unscoped enum promotes to its promotion type; every other arithmetic target is a
            // conversion. A fixed underlying type narrower than int is a better promotion (4.2),
            // ranked 0 here since no other candidate can match an enum argument exactly.
            if (auto promoted = enumPromotedTypes_.find(at.TypeName);
                !at.Pointer && !to.empty() && promoted != enumPromotedTypes_.end())
            {
                const std::string promotedName = arithmeticName(promoted->second);
                const auto backing = enumBackingTypes.find(at.TypeName);
                const std::string backingName = backing != enumBackingTypes.end()
                    ? arithmeticName(backing->second) : std::string();
                const bool narrowFixed = !backingName.empty() && integerBits(backingName) > 0
                    && integerBits(backingName) < 32;
                out.rank = narrowFixed && to == backingName ? 0
                    : !promotedName.empty() && to == promotedName ? 1 : 2;
                out.cxxViable = true;
                out.from = at.TypeName;
                continue;
            }
            const std::string from = argumentIdentity(arg);
            if (from.empty() || to.empty())
                continue;
            out.rank = arithmeticRank(from, to);
            out.cxxViable = true;
            out.from = from;
        }
        return ranks;
}

int LLVMBackend::CompareCxxConversionRanks(const std::vector<CxxConversionRank>& a,
                                           const std::vector<CxxConversionRank>& b, bool& crossing) const
{
        crossing = false;
        if (a.size() != b.size())
            return 2;
        // [over.ics.rank] tie-breakers between two sequences of the same rank.
        auto tieBreak = [&](const CxxConversionRank& x, const CxxConversionRank& y) {
            // 3.2.5 / 3.2.6: from one source to one target, the less cv-qualified is better.
            if (!x.cvBase.empty() && x.cvBase == y.cvBase && x.from == y.from)
                return x.cv - y.cv;
            // 4.4: from one class, to a more derived base is better, and any base beats void*.
            if (x.toClass.empty() || y.toClass.empty() || x.from != y.from || x.toClass == y.toClass)
                return 0;
            if (x.toClass == "void") return 1;
            if (y.toClass == "void") return -1;
            if (IsCxxBaseOf(y.toClass, x.toClass)) return -1;
            if (IsCxxBaseOf(x.toClass, y.toClass)) return 1;
            return 0;
        };
        bool aBetter = false;
        bool bBetter = false;
        for (size_t i = 0; i < a.size(); ++i)
        {
            const CxxConversionRank& x = a[i];
            const CxxConversionRank& y = b[i];
            int order = 0;
            // Implicit object parameters of one class and ref-qualifier: the non-const binding
            // is better (3.2.6); other pairs are not judged.
            if (x.rank == -2 || y.rank == -2)
            {
                if (x.rank != y.rank || x.from != y.from || x.objectRef != y.objectRef) return 2;
                order = x.second - y.second;
            }
            else if (x.rank < 0 || y.rank < 0)
                return 2;
            else if (x.rank < 3 || y.rank < 3)
                order = x.rank != y.rank ? x.rank - y.rank : tieBreak(x, y);
            // Two sequences through the SAME conversion function rank by the second standard
            // conversion; different functions (or an ambiguous one) are indistinguishable.
            else if (!x.userFunction.empty() && x.userFunction == y.userFunction)
                order = x.second - y.second;
            // One converting constructor to one class: only the reference binding differs.
            else if (x.userFunction.empty() && y.userFunction.empty() && !x.refTarget.empty()
                     && x.refTarget == y.refTarget && x.from == y.from && x.refBind != 0
                     && y.refBind != 0)
                order = x.refBind - y.refBind;
            aBetter |= order < 0;
            bBetter |= order > 0;
        }
        crossing = aBetter && bBetter;
        if (aBetter == bBetter)
            return 0;
        return aBetter ? -1 : 1;
}

std::pair<std::vector<LLVMBackend::NamedVariable>, LLVMBackend::FunctionSymbol> LLVMBackend::ComputeOverloadFunction(
        const std::vector<std::pair<std::vector<NamedVariable>, FunctionSymbol>>& candidates,
        std::vector<FunctionSymbol>* tiedOut, CxxPreferredOverload* preferredOut)
{
        // One viable candidate and the facts the tie-breaks below read.
        struct Ranked
        {
            const std::pair<std::vector<NamedVariable>, FunctionSymbol>* pair = nullptr;
            // Function-pointer arguments whose indirection shape disagrees with the parameter.
            int shapeMismatches = 0;
            // Integer -> bool coercions. Without it `sb.append(n)` picked append(bool) over
            // append(int) purely by declaration order.
            int boolCoercions = 0;
            // Parameters left to their defaults. An exact-arity overload (the C++ default
            // wrapper `f(a)` next to `f(a, b = expr)`) beats one that would fill defaults in.
            int omitted = 0;
            // Arguments bound through the `iterator -> const_iterator` conversion. Fewer wins, so
            // an overload declared over the argument's own specialization is never displaced.
            int constAddedConversions = 0;
            // Pointer arguments whose value category prefers the OTHER of a `T*&` / `T*const&`
            // overload pair. Fewer wins, so the binding C++ picks is taken.
            int refPtrConstMismatches = 0;
            // Aggregate fallback for user-defined conversions; full C++ sets use per-argument ranks.
            // The extra cost/name fields describe conversion operators when this fallback is used.
            int userConversions = 0;
            int userConversionCost = 0;
            std::string userConversionNames;
            int moveScore = 0;
            // Arguments bound by materializing a temporary for a C++ `const T&` scalar parameter.
            // A materialization is strictly worse than a by-value or rvalue-ref bind (ruling).
            int constRefMaterializations = 0;
            // Prefer matching C++ record-pointer pointee constness (adding const ranks worse).
            int cxxPointeeConstMismatches = 0;
            // Not viable in C++ (dropped pointee const); removed while any other candidate is
            // viable, else kept so the selected-candidate guard refuses it.
            bool cxxPointeeConstNotViable = false;
            // In a C++ overload set containing the same T& and const T& parameter, an
            // lvalue binds to T&. Count only this exact sibling comparison.
            int mutableRefPreference = 0;
            // Per argument: RankIntegerConversion cost, or -1 where no integer identity judged it.
            std::vector<int> integerCosts;
            // Per argument: standard conversion cost, including a literal zero to a pointer.
            std::vector<int> standardCosts;
            int nullPointerConversions = 0;
        };
        std::vector<Ranked> perfect;
        std::vector<Ranked> possible;   // the promotion/implicit tier
        const std::pair<std::vector<NamedVariable>, FunctionSymbol>* variadicFallback = nullptr;
        // A variadic candidate that drops a record pointee const: C++ keeps it only when
        // nothing else is viable, so it never displaces the tiers (see cxxPointeeConstNotViable).
        const std::pair<std::vector<NamedVariable>, FunctionSymbol>* variadicConstDropFallback = nullptr;
        auto isStringLiteralValue = [&](llvm::Value* value) {
            auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(
                value == nullptr ? nullptr : value->stripPointerCasts());
            return constant != nullptr && IsStringLiteralConstant(constant);
        };
        // Cost of one const-added binding, weighted so a value/reference pair does not tie into
        // declaration order: same polarity as the equal-spelling arm (reference wins an lvalue).
        auto constAddedBindingCost = [&](const NamedVariable& arg, const TypeAndValue& param,
                                         bool cxx) {
            const bool rvalue = cxx ? IsCxxRvalueReferenceArgument(arg)
                                    : IsRvalueReferenceArgument(arg);
            const bool preferred = param.IsAlias
                ? !rvalue : rvalue;
            return preferred ? 1 : 2;
        };
        auto receiverRefQualifierMatches = [&](const FunctionSymbol& candidate,
                                                const std::vector<NamedVariable>& arguments) {
            if (!candidate.IsCxx || !candidate.IsMethod
                || candidate.CxxRefQualifier == cflat_cinterop::CxxRefQualifierNone
                || candidate.Parameters.empty() || arguments.empty())
                return true;
            const bool receiverRvalue = IsCxxRvalueReferenceArgument(arguments.front());
            if (candidate.CxxRefQualifier == cflat_cinterop::CxxRefQualifierLValue)
                return !receiverRvalue;
            if (candidate.CxxRefQualifier == cflat_cinterop::CxxRefQualifierRValue)
                return receiverRvalue;
            return true;
        };

        /*
         * C++ converts a pointer to bool by a standard conversion, which beats any converting
         * constructor. Verify that a bool sibling is viable across all arguments before suppressing
         * the class conversion candidate, then let ordinary C++ ranking select the bool overload.
         */
        auto pointerShadowedByBool = [&](const NamedVariable& arg, size_t index) {
            if (!arg.TypeAndValue.Pointer || !arg.TypeAndValue.TypeName.empty()) return false;
            for (const auto& [otherArgs, other] : candidates)
            {
                if (index >= other.Parameters.size() || index >= otherArgs.size()
                    || !IsCxxBoolSiblingParameter(other.Parameters[index]))
                    continue;
                std::vector<NamedVariable> probeArgs = otherArgs;
                NamedVariable asBool;
                asBool.Primary = llvm::ConstantInt::getTrue(*context);
                asBool.BaseType = asBool.Primary->getType();
                asBool.InferSourceTypeName = "bool";
                asBool.IsRvalue = true;
                probeArgs[index] = std::move(asBool);
                const std::vector<std::pair<std::vector<NamedVariable>, FunctionSymbol>> one = {
                    { std::move(probeArgs), other } };
                if (!ComputeOverloadFunction(one).second.Parameters.empty()) return true;
            }
            return false;
        };

        for (const auto& pair : candidates)
        {
            const auto& [arguments, candidate] = pair;

            // Ref-qualified C++ members constrain the value category of the implicit receiver,
            // independently of the argument categories scored below.
            if (!receiverRefQualifierMatches(candidate, arguments)) continue;

            if (candidate.Variadic)
            {
                /*
                 * A variadic candidate is taken without per-argument scoring, so the code-value
                 * gate below never runs for it and `lam(Rec*, ...)` absorbed a function pointer
                 * exactly as the non-variadic sibling used to. Only the DECLARED parameters are
                 * judged: an argument in the `...` tail has no parameter to disagree with, and C
                 * passes function pointers through `...` routinely (`printf("%p", fn)`).
                 *
                 * Wider than the non-variadic sites: those judge only pointer/string parameters,
                 * and a DECLARED scalar here (`lam(int n, ...)`) absorbed the code address into an
                 * i32 slot and reached the LLVM verifier as a fatal "Call parameter type does not
                 * match function signature!". The non-variadic twin `lam(int n)` already rejects
                 * cleanly, so only this arm needs the wider question.
                 */
                bool declaredParamRefuses = false;
                bool variadicDropsPointeeConst = false;
                auto varParamItr = candidate.Parameters.begin();
                for (const auto& arg : arguments)
                {
                    if (varParamItr == candidate.Parameters.end()) break;
                    if (ArgumentIsCodeValue(arg, arg.CastOccurrenceId) && !ParameterAcceptsCodeValue(*varParamItr))
                        declaredParamRefuses = true;
                    // A DECLARED parameter of a variadic candidate is judged like any other:
                    // narrowing into it has no conversion and reached the module verifier.
                    if (ArgumentNarrowsParameter(arg, *varParamItr))
                        declaredParamRefuses = true;
                    const auto& originalArgType = arg.HasOriginalArgumentType
                        ? arg.OriginalArgumentType : arg.TypeAndValue;
                    if (!candidate.IsCxx
                        && IsImplicitIntegerPointeePointerConversion(originalArgType, *varParamItr))
                        declaredParamRefuses = true;
                    if (candidate.IsCxx
                        && CxxRecordPointeeConstRelation(arg.TypeAndValue, *varParamItr)
                               == CxxPointeeConstRelation::Drops)
                        variadicDropsPointeeConst = true;
                    ++varParamItr;
                }
                if (declaredParamRefuses)
                    continue;
                // C++ [over.ics.ellipsis]: only an argument the ellipsis receives has the worst
                // conversion. With none, the candidate ranks on its declared parameters below.
                const bool ellipsisUnused = candidate.IsCxx
                    && arguments.size() <= candidate.Parameters.size();
                if (!ellipsisUnused)
                {
                    if (variadicDropsPointeeConst)
                    {
                        if (variadicConstDropFallback == nullptr) variadicConstDropFallback = &pair;
                        continue;
                    }
                    // Variadic is a fallback: prefer any exact non-variadic match over it. For a
                    // native set it replaces the promotion tier seen so far; a later non-variadic
                    // candidate overrides it. A C++ ellipsis loses to every viable sibling.
                    variadicFallback = &pair;
                    if (!candidate.IsCxx) possible.clear();
                    continue;
                }
            }

            bool perfectMatch = true;
            bool promotionMatch = true;
            bool implicitMatch = true;
            // Function-pointer arguments whose indirection shape disagrees with the parameter.
            int shapeMismatches = 0;
            // Arguments bound to a 'bool' parameter through the integer -> bool coercion.
            int boolCoercions = 0;
            int constRefMaterializations = 0;
            int mutableRefPreference = 0;
            std::vector<int> standardCosts;
            int nullPointerConversions = 0;
            // Arguments bound through the `iterator -> const_iterator` conversion.
            int constAddedConversions = 0;
            int refPtrConstMismatches = 0;
            int cxxPointeeConstMismatches = 0;
            bool cxxPointeeConstNotViable = false;
            // Arguments bound through a user-defined conversion; the extra fields describe
            // conversion operators (see Ranked for the aggregate fallback).
            int userConversions = 0;
            int userConversionCost = 0;
            std::string userConversionNames;
            std::vector<int> integerCosts;
            integerCosts.reserve(arguments.size());

            auto candidateParamItr = candidate.Parameters.begin();
            for (const auto& arg : arguments)
            {
                int result = -1;
                if (candidate.IsCxx && candidateParamItr != candidate.Parameters.end())
                {
                    const auto relation =
                        CxxRecordPointeeConstRelation(arg.TypeAndValue, *candidateParamItr);
                    if (relation == CxxPointeeConstRelation::Drops)
                        cxxPointeeConstMismatches += 2;
                    else if (relation == CxxPointeeConstRelation::Adds)
                        cxxPointeeConstMismatches += 1;
                    // Not viable in C++: a dropped pointee const, or any inner-const change at **.
                    if (relation == CxxPointeeConstRelation::Drops
                        || (relation == CxxPointeeConstRelation::Adds
                            && arg.TypeAndValue.ElemPointer))
                        cxxPointeeConstNotViable = true;
                }
                // Set ONLY by the two `const T&` scalar arms below, so the integer-identity
                // ranking reads the REFERENT for candidates only those arms make viable.
                std::string constRefReferentIdentity;
                // True when one of those arms bound this argument, integer referent or not.
                bool constRefReferentArm = false;
                // Neither direction converts: a scoped enum to anything but itself, nor an integer to
                // any C++ enum parameter (scoped or unscoped).
                const bool scopedEnumMismatch = candidate.IsCxx
                    && (((arg.TypeAndValue.IsScopedEnum
                            || IsScopedEnumTypeName(arg.TypeAndValue.TypeName))
                        && !IsScopedEnumMatch(arg.TypeAndValue, *candidateParamItr))
                        || CxxEnumParameterRefusesArgument(arg, arg.TypeAndValue, *candidateParamItr));

                // A C++ rvalue-reference parameter is address-passed like an alias, but an
                // lvalue cannot bind it. Keep the candidate visible for the move diagnostic.
                const bool argIsCxxRvalue = candidate.IsCxx
                    && IsCxxRvalueReferenceArgument(arg);
                const size_t paramIndex = std::distance(candidate.Parameters.begin(), candidateParamItr);
                if (candidate.IsCxx && candidateParamItr->IsCxxConstRef
                    && !IsCxxRvalueReferenceArgument(arg))
                {
                    const bool hasMutableSibling = std::any_of(candidates.begin(), candidates.end(),
                        [&](const auto& sibling) {
                            const auto& symbol = sibling.second;
                            return symbol.IsCxx && symbol.SourceName == candidate.SourceName
                                && paramIndex < symbol.Parameters.size()
                                && symbol.Parameters.size() == candidate.Parameters.size()
                                && symbol.Parameters[paramIndex].TypeName == candidateParamItr->TypeName
                                && symbol.Parameters[paramIndex].IsAlias
                                && !symbol.Parameters[paramIndex].IsCxxConstRef
                                && !symbol.Parameters[paramIndex].IsRvalueRef;
                        });
                    if (hasMutableSibling) ++mutableRefPreference;
                }
                const bool cxxIndirectValueParam = candidate.IsCxx
                    && candidate.CxxAbi.valid
                    && paramIndex < candidate.CxxAbi.params.size()
                    && candidate.CxxAbi.params[paramIndex].kind == cflat_cinterop::RawAbiSlot::Indirect;
                const std::string cxxParamSpelling =
                    CxxReferenceParameterSpelling(candidate, paramIndex);
                const bool cxxRvalueReference = candidateParamItr->IsRvalueRef
                    || cxxParamSpelling.find("&&") != std::string::npos;
                const bool cxxConstReference = candidateParamItr->IsCxxConstRef
                    || cxxParamSpelling.rfind("const ", 0) == 0;
                if (candidateParamItr->IsRvalueRef
                    && !(candidate.IsCxx ? argIsCxxRvalue : IsRvalueReferenceArgument(arg)))
                {
                    perfectMatch = false;
                    promotionMatch = false;
                    implicitMatch = false;
                    break;
                }

                if (candidate.IsCxx && argIsCxxRvalue
                    && candidateParamItr->IsAlias && !candidateParamItr->IsRvalueRef
                    && !candidateParamItr->IsCxxConstRef
                    && !(cxxIndirectValueParam && !cxxConstReference && !cxxRvalueReference)
                    && !cxxRvalueReference
                    && !cxxConstReference
                    && !(arg.CxxLvalueKind == 0 && IsCxxAddressOfObjectArgument(arg))
                    && IsCxxReferenceParameter(candidate, paramIndex)
                    && (!arg.BaseType || !arg.BaseType->isStructTy()
                        || candidateParamItr->TypeName == arg.TypeAndValue.TypeName
                        || (arg.TypeAndValue.TypeName == "std.string"
                            && (candidateParamItr->TypeName == "string"
                                || candidateParamItr->TypeName == "void")))
                    && CxxReferenceArgumentMatches(*candidateParamItr, arg))
                {
                    perfectMatch = false;
                    promotionMatch = false;
                    implicitMatch = false;
                    break;
                }

                // function<T> parameter: accept any function-compatible argument (named function,
                // lambda fat struct, or stored function<T> variable). Type fidelity is checked at codegen.
                // An encoded closure param (list<Lambda<...>>::add's `T value`, gap a) accepts the same
                // arguments; an encoded closure arg satisfies a function<T> param likewise.
                if (candidate.IsCxx && candidateParamItr->IsFunctionPointer
                    && IsNullPointerConstantArgument(arg))
                {
                    result = 1;
                    ++nullPointerConversions;
                }
                else if ((candidateParamItr->IsFunctionPointer || IsEncodedClosureType(candidateParamItr->TypeName))
                    && (ArgumentIsFunctionPointerish(arg)
                        || (arg.BaseType && arg.BaseType->isPointerTy())))
                {
                    // `function<T>`, `function<T>*` and `function<T>[]` are three DISTINCT
                    // overloads; a disagreeing shape may still bind but never scores perfect.
                    result = (FunctionPointerShapeOf(arg.TypeAndValue, &arg)
                           == FunctionPointerShapeOf(*candidateParamItr, nullptr)) ? 0 : 1;
                    if (result != 0)
                        shapeMismatches++;

                    // Shapes agree: signatures that provably name different function types are
                    // proof. Only at equal shape - a shape mismatch stays score-1 bindable.
                    if (result == 0
                        && FuncPtrSignaturesProvablyDiffer(arg.TypeAndValue, *candidateParamItr))
                        result = -1;
                    // A NAMED function argument carries its signature in the function table, not
                    // on the argument, so it needs the overload-set form of the same proof.
                    if (result != -1 && NamedFunctionArgMismatches(arg, *candidateParamItr))
                        result = -1;
                }
                else if (arg.TypeAndValue.TypeName != "")
                {
                    // Resolve enum types for comparison: if either arg or param is an enum, use its backing type
                    auto resolveName = [&](const std::string& tn) -> std::string
                        {
                            if (tn.empty()) return tn;
                            if (IsScopedEnumTypeName(tn)) return tn;
                            auto it = enumBackingTypes.find(tn);
                            return (it != enumBackingTypes.end()) ? it->second : tn;
                        };

                    LLVMBackend::TypeAndValue tmpArg = arg.TypeAndValue;
                    LLVMBackend::TypeAndValue tmpParam = *candidateParamItr;

                    tmpArg.TypeName = resolveName(tmpArg.TypeName);
                    tmpParam.TypeName = resolveName(tmpParam.TypeName);

                    bool coreUniqueValueReceiver = !tmpArg.Pointer && tmpParam.Pointer
                        && tmpArg.TypeName == tmpParam.TypeName;
                    // A raw owning pointer binds by constructing the core value at the call site.
                    // The constructor is the ownership boundary and rejects borrowed pointers.
                    bool rawPointerToCoreUnique = IsRawPointerToCoreUnique(arg, tmpParam);
                    // A blessed core unique value is implicitly convertible to its raw pointee.
                    bool coreUniqueToRawPointer = !tmpArg.Pointer
                        && IsCoreUniqueToRawPointer(arg, tmpParam);
                    // A `unique<X>*` OUT-parameter accepts a pointer to X: the wrapper is a
                    // single-slot value of the same layout, so the callee's copy-out lands in
                    // the caller's slot as the borrow the builtin spelling handed back.
                    bool coreUniqueOutParam = tmpParam.Pointer && !tmpParam.ElemPointer
                        && tmpArg.Pointer && IsCoreUniqueType(tmpParam.TypeName)
                        && MangledGenericArgument(*this, tmpParam.TypeName) == tmpArg.TypeName;
                    const size_t paramIndex = std::distance(candidate.Parameters.begin(), candidateParamItr);
                    const bool cxxByValueParam = candidate.IsCxx
                        && paramIndex < candidate.Recipe.paramSlots.size()
                        && candidate.Recipe.paramSlots[paramIndex].kind == AbiSlot::ByVal;
                    const bool cxxIndirectValueParam = candidate.IsCxx
                        && candidate.CxxAbi.valid
                        && paramIndex < candidate.CxxAbi.params.size()
                        && candidate.CxxAbi.params[paramIndex].kind == cflat_cinterop::RawAbiSlot::Indirect;
                    // C++ never applies a USER-DEFINED conversion to the implicit object
                    // argument of a member call ([over.match.funcs]): `x.slice(0)` must not
                    // convert x into some other class that happens to have a `slice` member.
                    const bool cxxReceiverParam = candidate.IsCxx && candidate.IsMethod
                                               && paramIndex == 0;
                    if (!cxxReceiverParam && !pointerShadowedByBool(arg, paramIndex)
                        && CanImplicitlyConstructCxxClass(arg, *candidateParamItr,
                                                        cxxByValueParam || cxxIndirectValueParam))
                    {
                        result = 1;
                        if (candidate.IsCxx)
                            ++userConversions;
                    }
                    // Mirror of the line above in the conversion-OPERATOR direction: a class
                    // value with an implicit 'operator bool' / 'operator int' binds a scalar
                    // parameter. Ranked strictly below every standard sequence (see Ranked).
                    else if (bool needsStandard = false;
                        !cxxReceiverParam && !arg.TypeAndValue.Pointer
                        && !ScoreCxxConversionOperatorArgument(
                                arg, *candidateParamItr, !candidate.IsCxx, needsStandard, userConversions,
                                userConversionCost, userConversionNames).empty())
                        result = 1;
                    else if (coreUniqueValueReceiver || rawPointerToCoreUnique || coreUniqueToRawPointer
                        || coreUniqueOutParam
                        || IsStackValueToCoreUniqueInterface(arg, tmpParam))
                        result = 0;
                    else if (tmpParam.IsCxxRefToPointer && tmpArg.Pointer)
                    {
                        result = 0;
                        refPtrConstMismatches += CxxRefToPointerScore(arg, *candidateParamItr);
                    }
                    // `T *&&` takes the pointer VALUE through a temporary slot, so it binds a
                    // pointer rvalue whose pointee type matches the referred pointer.
                    else if (candidate.IsCxx && candidateParamItr->IsRvalueRef
                        && candidateParamItr->ElemPointer && tmpArg.Pointer
                        && IsCxxRvalueReferenceArgument(arg))
                    {
                        auto referent = *candidateParamItr;
                        referent.ElemPointer = false;
                        referent.Pointer = true;
                        referent.PointerDepth = 1;
                        referent.IsAlias = false;
                        referent.IsRvalueRef = false;
                        result = arg.BaseType != nullptr && GetType(referent) == arg.BaseType
                            ? 0 : -1;
                        if (result < 0 && arg.IsExplicitMove
                            && (tmpArg.TypeName.empty() || tmpArg.TypeName == referent.TypeName))
                            result = 0;
                    }
                    if (candidate.IsCxx && !tmpArg.Pointer
                        && !candidateParamItr->IsCxxRefToPointer)
                    {
                        std::string argSpelling;
                        std::string paramSpelling;
                        const bool sameCxxSpelling =
                            CxxSpellingForCflatType(tmpArg.TypeName, argSpelling)
                            && CxxSpellingForCflatType(tmpParam.TypeName, paramSpelling)
                            && SqueezeCxxSpelling(argSpelling) == SqueezeCxxSpelling(paramSpelling);
                        const bool sameCxxStdString = tmpArg.TypeName == "std.string"
                            && (tmpParam.TypeName == "string" || tmpParam.TypeName == "void");
                        if (sameCxxSpelling || sameCxxStdString)
                            result = candidateParamItr->IsAlias
                                ? (argIsCxxRvalue ? 1 : 0)
                                : (argIsCxxRvalue ? 0 : 1);
                        // `iterator -> const_iterator`: implicit (1), never perfect, so an
                        // exactly-typed overload of the same member still wins.
                        else if (IsCxxConstAddedPointerSpecialization(argSpelling, paramSpelling)
                            && HasIdenticalCxxRecordLayout(tmpArg.TypeName, tmpParam.TypeName))
                        {
                            result = 1;
                            constAddedConversions += constAddedBindingCost(
                                arg, *candidateParamItr, candidate.IsCxx);
                        }
                    }
                    // A contiguous C++ range converts to a CFlat view only after exact matches
                    // and native view bindings have had the chance to win.
                    if (result < 0 && !candidate.IsCxx
                        && IsCxxContiguousViewSource(arg, *candidateParamItr))
                        result = 1;
                    if (result < 0 && tmpArg.IsTypeMatch(tmpParam))
                        result = 0;
                    // A C++ lvalue reference is represented as an alias value in CFlat. A
                    // derived lvalue binds to a public base reference by a standard conversion;
                    // lower it from the derived object's storage with the base offset.
                    else if (tmpParam.IsAlias && !tmpParam.ElemPointer
                             && IsCxxDerivedToBaseValue(tmpArg, tmpParam))
                        result = 1;
                    // M6 - a pointer to a C++ class binds to a parameter typed as a PUBLIC base
                    // of it, with the base subobject offset added at the call. Scored as an
                    // implicit conversion so an exact-type overload always wins.
                    else if (IsCxxDerivedToBasePointer(tmpArg, tmpParam))
                        result = 1;
                    else if (tmpArg.IsTypePromotion(tmpParam))
                    {
                        // Positive = widening promotion (valid but non-perfect). Integer promotions
                        // report the source bit width; a floating-point promotion (float -> double)
                        // is not an integer, so IsInteger() returns -1 - use 1 so float -> double
                        // still scores as a valid promotion instead of a spurious no-match.
                        int bits = tmpArg.IsInteger();
                        result = (bits != -1) ? bits : 1;
                    }
                    else
                    {
                        // Same signedness group: int<->i32, long<->i64, char<->i8, etc.
                        // Same width  -> perfect match (result=0): int==i32, long==i64.
                        // Diff width  -> implicit conversion (result=1): i64->int, etc.
                        int myBits = tmpArg.IsInteger();
                        int otherBits = tmpParam.IsInteger();
                        bool myUnsigned = tmpArg.IsUnsignedInteger() != -1;
                        bool otherUnsigned = tmpParam.IsUnsignedInteger() != -1;
                        if (myBits != -1 && otherBits != -1 && myUnsigned == otherUnsigned)
                            result = (myBits == otherBits) ? 0 : 1;

                        // Unsigned source into a signed param at equal or greater width is a safe
                        // implicit conversion (e.g. u8 -> int, u64 -> i64); Upconvert zero-extends.
                        if (result < 0 && myBits != -1 && otherBits != -1 && myUnsigned && !otherUnsigned && myBits <= otherBits)
                            result = (myBits == otherBits) ? 1 : 1;

                        if (result < 0)
                        {
                            int myFP = tmpArg.IsFloatingPoint();
                            int otherFP = tmpParam.IsFloatingPoint();
                            if (myFP != -1 && otherFP != -1)
                                result = (myFP == otherFP) ? 0 : 1;
                        }

                        // Any pointer type is implicitly convertible to void*. Deliberately NOT
                        // gated on the argument's function-ness: `function<T>*` is the ADDRESS of a
                        // slot, i.e. data. The gate that refuses a function-pointer VALUE is in the
                        // empty-TypeName branch below - the shape such a value actually arrives in.
                        if (result < 0 && arg.TypeAndValue.Pointer &&
                            candidateParamItr->Pointer && candidateParamItr->TypeName == "void")
                        {
                            result = 0;
                        }

                        // Interface upcast using original names (interfaces are not enums).
                        // Handles both struct->interface (value) and struct*->interface (pointer).
                        if (result < 0 && candidateParamItr->IsInterface &&
                            !arg.TypeAndValue.IsInterface &&
                            StructImplementsInterface(arg.TypeAndValue.TypeName, candidateParamItr->TypeName))
                        {
                            result = 0;
                        }

                        // Derived interface -> parent interface (IButton arg to an IElement param).
                        // Implicit (1), not perfect, so an exact same-interface overload still wins.
                        if (result < 0 && candidateParamItr->IsInterface && arg.TypeAndValue.IsInterface &&
                            InterfaceInheritsFrom(arg.TypeAndValue.TypeName, candidateParamItr->TypeName))
                        {
                            result = 1;
                        }

                        // Lone view candidate refused on its element: let the pair through so the
                        // element gate at the call site spells the real reason (q13 sibling below).
                        if (result < 0 && candidates.size() == 1
                            && arg.TypeAndValue.IsArrayView && candidateParamItr->IsArrayView
                            && !candidateParamItr->IsInterface)
                            result = 0;

                        // A C++ `const T&` scalar parameter reached by a DIFFERENT scalar type
                        // (a `char` local into `const int&`) converts to the referent and binds it.
                        if (result < 0 && !tmpArg.Pointer)
                        {
                            TypeAndValue constRefReferent;
                            if (CxxConstScalarRefReferent(tmpParam, constRefReferent)
                                && CompareUpconvert(GetType(tmpArg), GetType(constRefReferent)) >= 0)
                            {
                                result = 1;
                                constRefReferentArm = true;
                                constRefReferentIdentity = IntegerParameterIdentity(constRefReferent);
                            }
                        }
                    }
                    // A TYPED class pointer binding 'void*' is a pointer conversion in C++, never
                    // an exact match, so the overload spelling the pointee wins the tie.
                    if (result == 0 && candidates.size() > 1 && candidateParamItr->Pointer
                        && !candidateParamItr->ElemPointer && candidateParamItr->TypeName == "void"
                        && tmpArg.Pointer && !tmpArg.ElemPointer && IsDataStructure(tmpArg.TypeName))
                        result = 1;
                }
                else
                {
                    const std::string inferredTypeName = arg.TypeAndValue.TypeName.empty()
                        ? arg.InferSourceTypeName : arg.TypeAndValue.TypeName;
                    std::string inferredSpelling;
                    std::string parameterSpelling;
                    const bool sameCxxSpelling = candidate.IsCxx
                        && !inferredTypeName.empty()
                        && CxxSpellingForCflatType(inferredTypeName, inferredSpelling)
                        && CxxSpellingForCflatType(candidateParamItr->TypeName, parameterSpelling)
                        && SqueezeCxxSpelling(inferredSpelling)
                            == SqueezeCxxSpelling(parameterSpelling);
                    const bool sameCxxValue = sameCxxSpelling && !candidateParamItr->IsAlias;
                    const bool sameCxxReference = sameCxxSpelling && candidateParamItr->IsAlias;
                    // Twin of the named-TypeName arm: same template, const added to a pointer
                    // template argument. Reached when the argument carries no CFlat TypeName.
                    const bool constAddedCxxSpelling = !sameCxxSpelling && candidate.IsCxx
                        && IsCxxConstAddedPointerSpecialization(inferredSpelling, parameterSpelling)
                        && HasIdenticalCxxRecordLayout(inferredTypeName,
                                                       candidateParamItr->TypeName);
                    const bool stringLiteralCharPointer = (candidateParamItr->Pointer
                        || candidateParamItr->PointerDepth > 0)
                        && candidateParamItr->TypeName == "char"
                        && arg.BaseType != nullptr && arg.BaseType->isPointerTy()
                        && (arg.IsRvalue || arg.IsStringLiteral || isStringLiteralValue(arg.Primary));
                    auto candidateParam = GetType(*candidateParamItr);
                    if (constAddedCxxSpelling)
                    {
                        result = 1;
                        constAddedConversions += constAddedBindingCost(
                            arg, *candidateParamItr, candidate.IsCxx);
                    }
                    else if (sameCxxValue)
                        result = IsCxxRvalueReferenceArgument(arg) ? 0 : 1;
                    else if (sameCxxReference)
                        result = IsCxxRvalueReferenceArgument(arg) ? 1 : 0;
                    else if (stringLiteralCharPointer)
                        result = 0;
                    else if (candidate.IsCxx && candidateParamItr->IsRvalueRef
                        && candidateParamItr->ElemPointer
                        && llvm::isa_and_nonnull<llvm::ConstantPointerNull>(arg.Primary))
                        result = 0;
                    else if (candidate.IsCxx && candidateParamItr->IsRvalueRef
                        && candidateParamItr->ElemPointer && arg.BaseType != nullptr
                        && arg.BaseType->isPointerTy()
                        && IsCxxRvalueReferenceArgument(arg))
                    {
                        auto referent = *candidateParamItr;
                        referent.ElemPointer = false;
                        referent.Pointer = true;
                        referent.PointerDepth = 1;
                        referent.IsAlias = false;
                        referent.IsRvalueRef = false;
                        result = GetType(referent) == arg.BaseType ? 0 : -1;
                        if (result < 0 && arg.IsExplicitMove)
                            result = 0;
                    }
                    else if (candidate.IsCxx && candidateParamItr->IsRvalueRef
                        && candidateParamItr->ElemPointer && arg.IsExplicitMove)
                        result = 0;
                    else if (candidateParamItr->IsRvalueRef && !arg.TypeAndValue.Pointer)
                    {
                        // A primitive foreign rvalue reference is represented as T* at the ABI
                        // boundary, but a call result may carry only its lowered scalar type.
                        auto valueParam = *candidateParamItr;
                        valueParam.Pointer = false;
                        valueParam.ElemPointer = false;
                        valueParam.PointerDepth = 0;
                        result = CompareUpconvert(arg.BaseType, GetType(valueParam));
                    }
                    else if (TypeAndValue constRefReferent;
                             !arg.TypeAndValue.Pointer
                             && CxxConstScalarRefReferent(*candidateParamItr, constRefReferent))
                    {
                        // A C++ `const T&` scalar parameter accepts an rvalue: score it against
                        // the REFERENT, and let argument lowering materialize the temporary.
                        result = CompareUpconvert(arg.BaseType, GetType(constRefReferent));
                        if (result >= 0)
                        {
                            constRefReferentArm = true;
                            constRefReferentIdentity = IntegerParameterIdentity(constRefReferent);
                        }
                    }
                    else
                    {
                        const bool refToPointerBind = candidateParamItr->IsCxxRefToPointer
                            && arg.TypeAndValue.Pointer;
                        if (refToPointerBind)
                            refPtrConstMismatches += CxxRefToPointerScore(arg, *candidateParamItr);
                        result = refToPointerBind
                            ? 0 : CompareUpconvert(arg.BaseType, candidateParam);
                    }
                    if (SpellType(*this, *candidateParamItr) == "char*"
                        && arg.BaseType != nullptr && arg.BaseType->isPointerTy()
                        && (arg.IsStringLiteral || isStringLiteralValue(arg.Primary)))
                        result = 0;
                    if (IsRawPointerToCoreUnique(arg, *candidateParamItr))
                        result = 0;
                    const size_t paramIndex = std::distance(candidate.Parameters.begin(), candidateParamItr);
                    const bool cxxByValueParam = candidate.IsCxx
                        && paramIndex < candidate.Recipe.paramSlots.size()
                        && candidate.Recipe.paramSlots[paramIndex].kind == AbiSlot::ByVal;
                    const bool cxxIndirectValueParam = candidate.IsCxx
                        && candidate.CxxAbi.valid
                        && paramIndex < candidate.CxxAbi.params.size()
                        && candidate.CxxAbi.params[paramIndex].kind == cflat_cinterop::RawAbiSlot::Indirect;
                    // C++ never applies a USER-DEFINED conversion to the implicit object
                    // argument of a member call ([over.match.funcs]): `x.slice(0)` must not
                    // convert x into some other class that happens to have a `slice` member.
                    const bool cxxReceiverParam = candidate.IsCxx && candidate.IsMethod
                                               && paramIndex == 0;
                    if (!cxxReceiverParam && !pointerShadowedByBool(arg, paramIndex)
                        && CanImplicitlyConstructCxxClass(arg, *candidateParamItr,
                                                        cxxByValueParam || cxxIndirectValueParam))
                    {
                        result = 1;
                        if (candidate.IsCxx)
                            ++userConversions;
                    }
                    // Opaque pointers scored the class REFERENCE a match above; with a `bool`
                    // sibling C++ takes the standard conversion, so this candidate is not it.
                    else if (!cxxReceiverParam && candidateParamItr->IsAlias
                        && IsCxxRecord(candidateParamItr->TypeName)
                        && pointerShadowedByBool(arg, paramIndex))
                        result = -1;
                    // Mirror of the line above in the conversion-OPERATOR direction: a class
                    // value with an implicit 'operator bool' / 'operator int' binds a scalar
                    // parameter. Ranked strictly below every standard sequence (see Ranked).
                    else if (bool needsStandard = false;
                        !cxxReceiverParam && !arg.TypeAndValue.Pointer
                        && !ScoreCxxConversionOperatorArgument(
                                arg, *candidateParamItr, !candidate.IsCxx, needsStandard, userConversions,
                                userConversionCost, userConversionNames).empty())
                        result = 1;

                    // A direct integer literal zero is a C++ null pointer constant. It is a
                    // conversion, below nullptr and pointer identity matches.
                    if (result < 0 && candidateParamItr->Pointer
                        && !candidateParamItr->IsArrayView
                        && IsNullPointerConstantArgument(arg))
                    {
                        result = 1;
                        ++nullPointerConversions;
                    }

                    /*
                     * A function pointer or closure VALUE does not implicitly convert to a DATA
                     * pointer of any pointee - it is code, not data, and ISO C has no
                     * function-to-object-pointer conversion either. Without this a candidate
                     * refuted on its SIGNATURE silently rebound onto a pointer-absorbing sibling.
                     * This branch is where such a value arrives: the call-argument loop copies its
                     * signature but deliberately leaves TypeName empty, and opaque pointers then
                     * make CompareUpconvert accept it against any pointer parameter alike.
                     *
                     * Only the plain-VALUE shape is refused, decided by the same helper the funcptr
                     * arm uses: a `function<T>*` is the ADDRESS of a slot and a `function<T>[N]`
                     * decays to one, and both are plain data pointers that must keep converting.
                     */
                    bool argIsCodeValue = ArgumentIsCodeValue(arg, arg.CastOccurrenceId);

                    // The pointee is never itself a function-pointer type here: the funcptr arm
                    // above claims every such parameter whenever the argument is code.
                    if (result >= 0 && candidateParamItr->Pointer && argIsCodeValue
                        && !arg.IsStringLiteral && !isStringLiteralValue(arg.Primary))
                        result = -1;

                    // Opaque pointers make every view look alike to CompareUpconvert; both sides
                    // record the ELEMENT star, so refusing 'int*[]' against 'int[]' is proven.
                    // With a lone candidate there is nothing to disambiguate, so let the pair
                    // through and let the element gate at the call site speak the real reason.
                    if (result >= 0 && candidates.size() > 1
                        && arg.TypeAndValue.IsArrayView && candidateParamItr->IsArrayView
                        && arg.TypeAndValue.ElemPointer != candidateParamItr->ElemPointer)
                        result = -1;

                    // Same blindness for the ELEMENT identity: an 'int[4]' argument scored perfect
                    // on 'double[]' too. Implicit only, so the matching-element overload wins.
                    if (result == 0 && candidates.size() > 1 && candidateParamItr->IsArrayView
                        && !candidateParamItr->IsInterface
                        && (arg.TypeAndValue.IsArrayView || arg.TypeAndValue.ConstArraySize > 0)
                        && !arg.InferSourceTypeName.empty() && !candidateParamItr->TypeName.empty()
                        && CanonicalPrimitiveTypeName(arg.InferSourceTypeName)
                            != CanonicalPrimitiveTypeName(candidateParamItr->TypeName))
                        result = 1;

                    // Opaque pointers make every pointer pair look identical to CompareUpconvert.
                    // An argument whose CFlat type is unknown (empty TypeName - primitive pointers
                    // like '&boolVar') binding to a pointer-to-struct parameter is only an IMPLICIT
                    // match, never a perfect one: a perfect match here would let a method's 'this'
                    // swallow any pointer in a free call (e.g. readLine(&eof) resolving to
                    // File.readLine with the bool* as 'this') and beat the exact free overload.
                    // nullptr stays a perfect match for any pointer parameter.
                    if (result == 0 && candidateParamItr->Pointer
                        && IsDataStructure(candidateParamItr->TypeName)
                        && arg.BaseType && arg.BaseType->isPointerTy()
                        && !(arg.Primary && llvm::isa<llvm::ConstantPointerNull>(arg.Primary)))
                        result = 1;

                    // Interface upcast: struct value (TypeName empty, BaseType is struct) to interface param
                    if (result < 0 && candidateParamItr->IsInterface && arg.BaseType)
                    {
                        if (auto* st = llvm::dyn_cast<llvm::StructType>(arg.BaseType))
                        {
                            auto structName = st->getName().str();
                            if (!structName.empty() && StructImplementsInterface(structName, candidateParamItr->TypeName))
                                result = 0;
                        }
                    }
                    // Implicit char* -> string coercion: string literal or char* passed to a string param.
                    // A code value is refused here for the same reason as the pointer gate above -
                    // it lowered to `operator string(char*)` reading the callee's machine code.
                    if (result < 0 && candidateParamItr->TypeName == "string" && !candidateParamItr->Pointer
                        && arg.BaseType && arg.BaseType->isPointerTy() && !argIsCodeValue)
                        result = 1;
                }

                // C++ converts any object pointer to bool by value, but does not bind that
                // pointer as a bool&. A const bool& instead binds a converted bool temporary.
                // Pointer-to-integer conversions remain forbidden at every C++ call site.
                bool pointerToBoolReference = false;
                bool pointerToNonConstClassReference = false;
                if (candidate.IsCxx && arg.TypeAndValue.Pointer)
                {
                    const bool addressOfObject = IsCxxAddressOfObjectArgument(arg);
                    TypeAndValue boolReferent;
                    const bool addressOfBool = IsCxxAddressOfReferent(arg, "bool");
                    const bool pointerValue = !addressOfBool;
                    const bool constBoolReference = CxxConstScalarRefReferent(
                        *candidateParamItr, boolReferent) && boolReferent.TypeName == "bool"
                        && pointerValue;
                    const bool boolByValue = candidateParamItr->TypeName == "bool"
                        && !candidateParamItr->Pointer && !candidateParamItr->IsAlias;
                    pointerToBoolReference = candidateParamItr->TypeName == "bool"
                        && candidateParamItr->Pointer && candidateParamItr->IsAlias
                        && !candidateParamItr->IsCxxConstRef && !constBoolReference
                        && pointerValue;
                    const bool primitivePointerArg = arg.TypeAndValue.Pointer
                        && arg.TypeAndValue.TypeName.empty()
                        && IsPrimitiveTypeName(arg.InferSourceTypeName);
                    pointerToNonConstClassReference = candidateParamItr->Pointer
                        && candidateParamItr->IsAlias && !candidateParamItr->IsCxxConstRef
                        && !candidateParamItr->IsRvalueRef
                        && !candidateParamItr->IsCxxRefToPointer
                        && IsCxxRecord(candidateParamItr->TypeName)
                        && primitivePointerArg;
                    if (constBoolReference)
                    {
                        result = 1;
                        constRefReferentArm = true;
                        constRefReferentIdentity = IntegerParameterIdentity(boolReferent);
                    }
                    else if (boolByValue)
                    {
                        result = 1;
                    }
                    else if (candidateParamItr->IsInteger() != -1
                        && candidateParamItr->TypeName != "bool"
                        && (!candidateParamItr->Pointer
                            || ((candidateParamItr->IsAlias || candidateParamItr->IsRvalueRef)
                                && !candidateParamItr->ElemPointer
                                && !candidateParamItr->IsCxxRefToPointer
                                && !IsCxxAddressOfReferent(arg, candidateParamItr->TypeName))))
                    {
                        result = -1;
                    }
                    if (pointerToBoolReference || pointerToNonConstClassReference)
                        result = -1;
                }

                /*
                 * Depth OVERRIDES both branches above, because both re-granted a pair the depth
                 * gates refuse: the named branch's numeric fallback scores `int**` against `int*`
                 * as one 32-bit "int", and the empty-TypeName branch sees only opaque pointers.
                 * Same predicate IsTypeMatch uses, so it refuses exactly what that refuses.
                 */
                // A reference-to-pointer parameter keeps the referred pointer's depth for
                // overload resolution; only its ABI lowering adds the reference slot.
                // A C++ `T*&&` carries the reference as its second level, so depth is judged
                // against the REFERENT; a flagged `T**&&` is already stored at its referent depth.
                TypeAndValue depthParam = *candidateParamItr;
                if (candidate.IsCxx && depthParam.IsRvalueRef && depthParam.ElemPointer
                    && !depthParam.IsCxxRefToPointer)
                {
                    depthParam.ElemPointer = false;
                    depthParam.PointerDepth = 1;
                }
                if (result >= 0 && arg.TypeAndValue.PointerDepthRefuses(depthParam))
                    result = -1;

                // Constructor offers and conversion operators cannot re-grant a standard
                // pointer/reference mismatch rejected above.
                if (pointerToBoolReference || pointerToNonConstClassReference)
                    result = -1;

                // Implicit integer NARROWING at a call argument is not legal (ruling 2026-09-04):
                // reject it here so the caller reports "no overload ... matches", the same answer
                // an 'int' argument to the same parameter already got.
                if (result >= 0 && ArgumentNarrowsParameter(arg, *candidateParamItr))
                    result = -1;

                const auto& originalArgType = arg.HasOriginalArgumentType
                    ? arg.OriginalArgumentType : arg.TypeAndValue;
                if (!candidate.IsCxx
                    && IsImplicitIntegerPointeePointerConversion(originalArgType,
                                                                  *candidateParamItr))
                    result = -1;
                // `long*` vs the same-width `iN*` is accepted but never perfect, so an exactly
                // spelled overload (f(long*) vs f(i64*)) still wins.
                if (result == 0 && candidates.size() > 1 && originalArgType.Pointer
                    && candidateParamItr->Pointer
                    && CanonicalPrimitiveTypeName(ResolveTypeAlias(originalArgType.TypeName))
                        != CanonicalPrimitiveTypeName(ResolveTypeAlias(candidateParamItr->TypeName))
                    && (ResolveTypeAlias(originalArgType.TypeName) == "long"
                        || ResolveTypeAlias(originalArgType.TypeName) == "ulong"
                        || ResolveTypeAlias(candidateParamItr->TypeName) == "long"
                        || ResolveTypeAlias(candidateParamItr->TypeName) == "ulong"))
                    result = 1;

                // Integer -> bool IS legal, and lowers through CoerceToBoolCondition. Implicit
                // (1), never perfect, so an exactly-typed overload still wins.
                if (!scopedEnumMismatch && result < 0
                    && ArgumentConvertsToBoolParameter(arg, *candidateParamItr))
                {
                    result = 1;
                    boolCoercions++;
                }
                if (scopedEnumMismatch) result = -1;

                // Integer identity ranking (ruling 2026-09-10): only an identity-exact integer is a
                // perfect match. Never widens the viable set - it re-ranks what already binds.
                int integerCost = -1;
                if (result >= 0)
                {
                    // LLVM opaque pointers erase primitive pointee types. Preserve that identity
                    // for overload ranking so an exact T* beats other integer pointers, and a
                    // same-width mismatch beats a different-width mismatch. Binding remains
                    // viable: this only moves mismatches out of the perfect tier. C++ candidates
                    // only; native overloads keep CFlat's own pointer rules (i8* into char*).
                    const TypeAndValue& pointerParam = *candidateParamItr;
                    const TypeAndValue& pointerArg = arg.TypeAndValue;
                    const std::string pointerArgName = !pointerArg.TypeName.empty()
                        ? pointerArg.TypeName : arg.InferSourceTypeName;
                    if (candidate.IsCxx && pointerArg.Pointer && !pointerArg.ElemPointer
                        && pointerArg.PointerDepth <= 1
                        && pointerParam.Pointer && !pointerParam.ElemPointer
                        && !pointerParam.IsAlias && !pointerParam.IsRvalueRef
                        && !pointerParam.IsCxxRefToPointer && pointerParam.PointerDepth <= 1
                        && !pointerArgName.empty() && !pointerParam.TypeName.empty())
                    {
                        TypeAndValue argPointee;
                        argPointee.TypeName = pointerArgName;
                        TypeAndValue paramPointee;
                        paramPointee.TypeName = pointerParam.TypeName;
                        const int argBits = argPointee.IsInteger();
                        const int paramBits = paramPointee.IsInteger();
                        if (argBits > 0 && paramBits > 0)
                        {
                            const bool samePointee = CanonicalPrimitiveTypeName(pointerArgName)
                                == CanonicalPrimitiveTypeName(pointerParam.TypeName);
                            const bool sameSignedness = (argPointee.IsUnsignedInteger() != -1)
                                == (paramPointee.IsUnsignedInteger() != -1);
                            integerCost = samePointee ? 0
                                : argBits != paramBits ? kIntegerConversionCost
                                : sameSignedness ? 1 : 2;
                            if (integerCost != 0 && result == 0)
                                result = 1;
                        }
                    }
                    const std::string argIdentity = IntegerArgumentIdentity(arg, candidate.IsCxx);
                    // A reference parameter has no identity of its own; the arms above supply the
                    // referent's, so `const int&` outranks `const long long&` for an int literal.
                    const std::string paramIdentity = constRefReferentIdentity.empty()
                        ? (candidate.IsCxx
                            ? CxxIntegerParameterIdentity(candidate, paramIndex, *candidateParamItr)
                            : IntegerParameterIdentity(*candidateParamItr))
                        : constRefReferentIdentity;
                    if (!argIdentity.empty() && !paramIdentity.empty())
                    {
                        integerCost = RankIntegerConversion(argIdentity, paramIdentity, candidate.IsCxx);
                        result = integerCost == 0 ? 0 : 1;
                    }
                }
                // RULING: a `const T&` materialization is strictly WORSE than a by-value or
                // rvalue-ref candidate at the same conversion, and never a perfect match.
                if (constRefReferentArm)
                {
                    if (result == 0) result = 1;
                    if (integerCost >= 0) integerCost += 1;
                    if (result >= 0) constRefMaterializations++;
                }
                if (result >= 0 && candidate.IsCxx
                    && candidateParamItr != candidate.Parameters.end())
                {
                    const auto relation =
                        CxxRecordPointeeConstRelation(arg.TypeAndValue, *candidateParamItr);
                    if (relation == CxxPointeeConstRelation::Adds
                        || relation == CxxPointeeConstRelation::Drops)
                        result = std::max(result, 1);
                }
                integerCosts.push_back(integerCost);
                standardCosts.push_back(integerCost);
                if (standardCosts.back() < 0 && IsNullPointerConstantArgument(arg)
                    && candidateParamItr->Pointer && !candidateParamItr->IsArrayView)
                    standardCosts.back() = kIntegerConversionCost;

                if (result != 0)
                {
                    perfectMatch = false;
                }

                if (result < 0)
                {
                    promotionMatch = false;
                    implicitMatch = false;
                }

                if (!(perfectMatch || promotionMatch || implicitMatch))
                {
                    // quick break if matches is no longer possible.
                    break;
                }

                ++candidateParamItr;
            }

            const int omitted = candidate.Parameters.size() > arguments.size()
                ? (int)(candidate.Parameters.size() - arguments.size()) : 0;
            if (perfectMatch || promotionMatch || implicitMatch)
            {
                Ranked ranked;
                ranked.pair = &pair;
                ranked.shapeMismatches = shapeMismatches;
                ranked.boolCoercions = boolCoercions;
                ranked.constRefMaterializations = constRefMaterializations;
                ranked.mutableRefPreference = mutableRefPreference;
                ranked.omitted = omitted;
                ranked.constAddedConversions = constAddedConversions;
                ranked.refPtrConstMismatches = refPtrConstMismatches;
                ranked.cxxPointeeConstMismatches = cxxPointeeConstMismatches;
                ranked.cxxPointeeConstNotViable = cxxPointeeConstNotViable;
                ranked.userConversions = userConversions;
                ranked.userConversionCost = userConversionCost;
                ranked.userConversionNames = userConversionNames;
                ranked.moveScore = ScoreMoveAgreement(arguments, candidate);
                ranked.integerCosts = std::move(integerCosts);
                ranked.standardCosts = std::move(standardCosts);
                ranked.nullPointerConversions = nullPointerConversions;
                (perfectMatch ? perfect : possible).push_back(std::move(ranked));
            }
        }

        // C++ viability: a const-dropping candidate leaves the set while any other is viable.
        auto cxxConstViable = [](const Ranked& r) { return !r.cxxPointeeConstNotViable; };
        if (std::any_of(perfect.begin(), perfect.end(), cxxConstViable)
            || std::any_of(possible.begin(), possible.end(), cxxConstViable))
        {
            std::erase_if(perfect, [](const Ranked& r) { return r.cxxPointeeConstNotViable; });
            std::erase_if(possible, [](const Ranked& r) { return r.cxxPointeeConstNotViable; });
        }

        using Result = std::pair<std::vector<NamedVariable>, FunctionSymbol>;

        // Keeps only the members of `set` whose key is lowest.
        auto keepLowest = [](std::vector<const Ranked*>& set, auto key) {
            int best = std::numeric_limits<int>::max();
            for (const Ranked* r : set)
                best = std::min(best, key(*r));
            std::erase_if(set, [&](const Ranked* r) { return key(*r) != best; });
        };

        // C++'s per-argument comparison: `a` is no worse at every position both sides judged by
        // integer identity, and strictly better at one.
        auto dominates = [](const Ranked& a, const Ranked& b) {
            bool strictlyBetter = false;
            const size_t count = std::min(a.integerCosts.size(), b.integerCosts.size());
            for (size_t i = 0; i < count; ++i)
            {
                if (a.integerCosts[i] < 0 || b.integerCosts[i] < 0)
                    continue;
                if (a.integerCosts[i] > b.integerCosts[i])
                    return false;
                if (a.integerCosts[i] < b.integerCosts[i])
                    strictlyBetter = true;
            }
            return strictlyBetter;
        };

        enum class TieKind { IdenticalParameters, RankedStandardOnly, InheritedBaseAmbiguity, Other };
        auto classifyTie = [&](const Ranked& a, const Ranked& b) {
            const auto& aParams = a.pair->second.Parameters;
            const auto& bParams = b.pair->second.Parameters;
            if (aParams.size() != bParams.size())
                return TieKind::Other;
            bool anyDiffers = false;
            for (size_t i = 0; i < aParams.size(); ++i)
            {
                if (aParams[i].IsMove == bParams[i].IsMove
                    && aParams[i].ToUniqueString(*this) == bParams[i].ToUniqueString(*this))
                    continue;
                anyDiffers = true;
                const bool integerJudged = i < a.integerCosts.size() && i < b.integerCosts.size()
                    && a.integerCosts[i] >= 0 && b.integerCosts[i] >= 0
                    && enumBackingTypes.count(aParams[i].TypeName) == 0
                    && enumBackingTypes.count(bParams[i].TypeName) == 0;
                const bool nullPointerJudged = i < a.integerCosts.size() && i < b.integerCosts.size()
                    && i < a.standardCosts.size() && i < b.standardCosts.size()
                    && a.integerCosts[i] < 0 && b.integerCosts[i] < 0
                    && a.standardCosts[i] >= 0 && b.standardCosts[i] >= 0;
                const bool judged = integerJudged || nullPointerJudged;
                if (!judged)
                    return TieKind::Other;
            }
            if (!anyDiffers)
            {
                if (a.pair->second.IsCxx && b.pair->second.IsCxx
                    && a.pair->second.IsMethod && b.pair->second.IsMethod
                    && !a.pair->second.CxxInheritedOwner.empty()
                    && !b.pair->second.CxxInheritedOwner.empty()
                    && a.pair->second.CxxInheritedOwner != b.pair->second.CxxInheritedOwner)
                    return TieKind::InheritedBaseAmbiguity;
                return (a.pair->second.IsCxx || b.pair->second.IsCxx) ? TieKind::Other
                                                                      : TieKind::IdenticalParameters;
            }
            return TieKind::RankedStandardOnly;
        };

        /*
         * Settles candidates every tie-break above left equal. Identical parameter lists: the later
         * registration shadows the earlier (a program's own `void WaitForExit(int)` over the
         * synthesized `bool WaitForExit(int)`). A tie that only integer identity or null-pointer
         * conversion ranking could have decided is a genuine ambiguity, reported through `tiedOut`.
         * A tie at any other kind of position is outside this ranking and keeps the legacy pick.
         */
        auto settle = [&](const std::vector<const Ranked*>& best, bool legacyLastWins) -> const Ranked* {
            if (best.size() == 1)
                return best.front();
            bool allIdentical = true;
            bool anyRankedOnly = false;
            bool anyInheritedBaseAmbiguity = false;
            bool anyOther = false;
            for (size_t i = 0; i < best.size(); ++i)
                for (size_t j = i + 1; j < best.size(); ++j)
                {
                    TieKind kind = classifyTie(*best[i], *best[j]);
                    allIdentical &= kind == TieKind::IdenticalParameters;
                    anyRankedOnly |= kind == TieKind::RankedStandardOnly;
                    anyInheritedBaseAmbiguity |= kind == TieKind::InheritedBaseAmbiguity;
                    anyOther |= kind == TieKind::Other;
                }
            if (allIdentical)
                return best.back();
            if (anyInheritedBaseAmbiguity && tiedOut != nullptr)
            {
                for (const Ranked* r : best)
                    tiedOut->push_back(r->pair->second);
                return nullptr;
            }
            if (anyRankedOnly && !anyOther && tiedOut != nullptr)
            {
                for (const Ranked* r : best)
                    tiedOut->push_back(r->pair->second);
                return nullptr;
            }
            return legacyLastWins ? best.back() : best.front();
        };

        /*
         * C++ [over.match.best] over an all-C++ candidate set, argument by argument: a candidate
         * no worse at every argument and better at one removes the other from BOTH tiers, and two
         * survivors each better at some argument are ambiguous. The tiers alone cannot see this:
         * they count conversions per candidate, and a floating position is never ranked in them.
         */
        const bool cxxRanking = tiedOut != nullptr && variadicFallback == nullptr
            && !candidates.empty()
            && std::all_of(candidates.begin(), candidates.end(), [](const auto& c) {
                   return c.second.IsCxx && !c.second.Variadic; });
        std::map<const Result*, std::vector<CxxConversionRank>> cxxRanks;
        auto fullyRanked = [](const std::vector<CxxConversionRank>& ranks) {
            return std::none_of(ranks.begin(), ranks.end(),
                                [](const CxxConversionRank& r) { return r.rank == -1; });
        };
        if (cxxRanking)
        {
            std::vector<const Ranked*> viable;
            for (const Ranked& r : perfect) viable.push_back(&r);
            for (const Ranked& r : possible) viable.push_back(&r);
            for (const Ranked* r : viable)
                cxxRanks[r->pair] = RankCxxConversionSequences(r->pair->first, r->pair->second);
            // [over.match.best] 2.4: on equal conversion sequences a non-template beats a function
            // template specialization (a deduction wrapper or a demangled `name<...>`).
            auto isTemplateCandidate = [&](const FunctionSymbol& f) {
                return f.UniqueName.starts_with("__cflat_tpl_") || IsCxxTemplateSpecializationSymbol(f);
            };
            std::set<const Result*> dominated;
            for (const Ranked* a : viable)
                for (const Ranked* b : viable)
                {
                    bool crossing = false;
                    if (a == b) continue;
                    const int order = CompareCxxConversionRanks(cxxRanks[a->pair], cxxRanks[b->pair],
                                                                crossing);
                    // CFlat has no volatile objects: a non-volatile member beats its volatile twin.
                    if (order == -1
                        || (order == 0 && !crossing && isTemplateCandidate(b->pair->second)
                            && !isTemplateCandidate(a->pair->second))
                        || (order == 0 && !crossing && b->pair->second.CxxVolatile
                            && !a->pair->second.CxxVolatile))
                        dominated.insert(b->pair);
                }
            std::vector<const Ranked*> survivors;
            for (const Ranked* r : viable)
                if (dominated.count(r->pair) == 0)
                    survivors.push_back(r);
            bool anyCrossing = false;
            for (size_t i = 0; i < survivors.size(); ++i)
                for (size_t j = i + 1; j < survivors.size(); ++j)
                {
                    bool crossing = false;
                    CompareCxxConversionRanks(cxxRanks[survivors[i]->pair],
                                              cxxRanks[survivors[j]->pair], crossing);
                    anyCrossing |= crossing;
                }
            // Refuse only when every survivor is fully judged: an unjudged one might beat both.
            if (anyCrossing && std::all_of(survivors.begin(), survivors.end(), [&](const Ranked* r) {
                    return fullyRanked(cxxRanks[r->pair]); }))
            {
                for (const Ranked* r : survivors)
                    tiedOut->push_back(r->pair->second);
                return Result{};
            }
            /*
             * Survivors no argument tells apart: C++ finds no best viable function. Refused only
             * for fully judged, distinct, non-template declarations (the template tie-breakers
             * and a second registration of one declaration are not ranked here). A by-value vs
             * reference pair stays with the tiers below (ruling: by-value beats a const T& temp).
             */
            auto referenceMix = [](const FunctionSymbol& x, const FunctionSymbol& y) {
                for (size_t k = 0; k < x.Parameters.size() && k < y.Parameters.size(); ++k)
                    if ((x.Parameters[k].IsAlias || x.Parameters[k].IsRvalueRef)
                        != (y.Parameters[k].IsAlias || y.Parameters[k].IsRvalueRef))
                        return true;
                return false;
            };
            auto isTemplateSpecialization = [](const std::string& linkage) {
                if (linkage.starts_with('?')) return linkage.starts_with("??$");
                llvm::ItaniumPartialDemangler demangler;
                if (demangler.partialDemangle(linkage.c_str())) return true;
                char* name = demangler.getFunctionName(nullptr, nullptr);
                const bool templated = name == nullptr || std::string_view(name).ends_with('>');
                std::free(name);
                return templated;
            };
            std::set<std::string> declarations;
            bool indistinguishable = !anyCrossing && survivors.size() > 1;
            for (size_t i = 0; indistinguishable && i < survivors.size(); ++i)
            {
                const std::string declared = CxxDeclarationLinkageName(survivors[i]->pair->second.UniqueName);
                indistinguishable = fullyRanked(cxxRanks[survivors[i]->pair])
                    && declarations.insert(declared).second && !isTemplateSpecialization(declared);
                for (size_t j = i + 1; indistinguishable && j < survivors.size(); ++j)
                {
                    bool crossing = false;
                    indistinguishable = CompareCxxConversionRanks(cxxRanks[survivors[i]->pair],
                                                                  cxxRanks[survivors[j]->pair], crossing) == 0
                        && !referenceMix(survivors[i]->pair->second, survivors[j]->pair->second);
                }
            }
            if (indistinguishable)
            {
                for (const Ranked* r : survivors)
                    tiedOut->push_back(r->pair->second);
                return Result{};
            }
            if (!survivors.empty())
            {
                std::erase_if(perfect, [&](const Ranked& r) { return dominated.count(r.pair) != 0; });
                std::erase_if(possible, [&](const Ranked& r) { return dominated.count(r.pair) != 0; });
            }
        }

        /*
         * A candidate CFlat's call rules refuse (a narrowing or an int <-> floating argument, an
         * ambiguous conversion operator) is still viable in C++. The pick stands only if it beats
         * every such candidate; otherwise C++ would have refused the call or picked the other.
         */
        // Does CFlat's own scoring bind argument `i` of `pair` alone? Probed on a one-parameter
        // copy, without `tiedOut`, so the C++ ranking above does not recurse.
        auto cflatAccepts = [&](const Result& pair, size_t i) {
            FunctionSymbol probe = pair.second;
            probe.Parameters = { pair.second.Parameters[i] };
            probe.IsMethod = false;
            probe.UniqueName = "__cflat_udc_rank_probe";   // no declared C++ spelling to look up
            probe.CxxAbi.valid = false;
            probe.Recipe.paramSlots.clear();
            const std::vector<Result> one = { Result{ { pair.first[i] }, probe } };
            return !ComputeOverloadFunction(one).second.Parameters.empty();
        };
        auto finish = [&](const Ranked* winner) -> Result {
            if (winner == nullptr)
                return Result{};
            if (!cxxRanking || !fullyRanked(cxxRanks[winner->pair]))
                return *winner->pair;
            const auto& won = cxxRanks[winner->pair];
            for (const auto& pair : candidates)
            {
                const auto defaultRequest = cxxDefaultWrapperRequests_.find(pair.second.UniqueName);
                const bool failedDefault = defaultRequest != cxxDefaultWrapperRequests_.end()
                    && defaultRequest->second.result < 0;
                if ((!failedDefault && cxxRanks.count(&pair) != 0)
                    || !receiverRefQualifierMatches(pair.second, pair.first))
                    continue;
                // A failed default remains viable even when CFlat's fallback ranks a rival first.
                // Its full declaration and its exact-arity wrapper are the same candidate.
                if (pair.second.UniqueName == winner->pair->second.UniqueName
                    || (failedDefault && CxxDeclarationLinkageName(pair.second.UniqueName)
                        == CxxDeclarationLinkageName(winner->pair->second.UniqueName)))
                    continue;
                const auto ranks = RankCxxConversionSequences(pair.first, pair.second);
                // A ranked by-value class parameter is viable even though cxxViable only proves
                // const-reference bindings for candidates CFlat could not itself match.
                if (failedDefault && cxxRanks.count(&pair) != 0 ? !fullyRanked(ranks)
                    : !std::all_of(ranks.begin(), ranks.end(),
                                  [](const CxxConversionRank& r) { return r.cxxViable; }))
                    continue;
                bool crossing = false;
                // A winner kept only because no tier member was C++-viable (it drops a record
                // pointee const) loses to any candidate C++ can call.
                const int order = winner->cxxPointeeConstNotViable
                    ? 1 : CompareCxxConversionRanks(won, ranks, crossing);
                if (order == -1 || order == 2)
                    continue;
                if (failedDefault && order == 0 && !crossing
                    && ((IsCxxTemplateSpecializationSymbol(pair.second)
                         && !IsCxxTemplateSpecializationSymbol(winner->pair->second))
                        || (pair.second.CxxVolatile && !winner->pair->second.CxxVolatile)))
                    continue;
                // Strictly better in C++: report it by name at the first argument CFlat refuses.
                if (order == 1 && preferredOut != nullptr)
                    for (size_t i = 0; i < ranks.size(); ++i)
                    {
                        if (ranks[i].rank < 0 || cflatAccepts(pair, i))
                            continue;
                        const auto& param = pair.second.Parameters[i];
                        preferredOut->set = true;
                        preferredOut->preferred = pair.second;
                        preferredOut->picked = winner->pair->second;
                        preferredOut->argument = pair.second.IsMethod ? i : i + 1;
                        preferredOut->from = ranks[i].from;
                        preferredOut->to = SpellType(*this, param);
                        if (preferredOut->to.empty())
                            preferredOut->to = param.TypeName;
                        return Result{};
                    }
                tiedOut->push_back(winner->pair->second);
                tiedOut->push_back(pair.second);
                return Result{};
            }
            return *winner->pair;
        };

        auto preferCxxDefaultWrapperForSameDeclaration = [&](std::vector<const Ranked*>& best) {
            if (!cxxRanking)
            {
                keepLowest(best, [](const Ranked& r) { return r.omitted; });
                return;
            }
            auto declarationName = [](const std::string& name) { return CxxDeclarationLinkageName(name); };
            std::map<std::string, int> minimumOmitted;
            for (const Ranked* ranked : best)
            {
                const std::string name = declarationName(ranked->pair->second.UniqueName);
                auto [found, inserted] = minimumOmitted.try_emplace(name, ranked->omitted);
                if (!inserted) found->second = std::min(found->second, ranked->omitted);
            }
            std::erase_if(best, [&](const Ranked* ranked) {
                const auto& name = declarationName(ranked->pair->second.UniqueName);
                return ranked->omitted != minimumOmitted[name];
            });
        };

        if (!perfect.empty())
        {
            std::vector<const Ranked*> best;
            for (const Ranked& r : perfect)
                best.push_back(&r);
            keepLowest(best, [](const Ranked& r) { return r.cxxPointeeConstMismatches; });
            keepLowest(best, [](const Ranked& r) { return r.refPtrConstMismatches; });
            keepLowest(best, [](const Ranked& r) { return r.mutableRefPreference; });
            keepLowest(best, [](const Ranked& r) { return -r.moveScore; });
            keepLowest(best, [](const Ranked& r) { return (int)r.pair->second.CxxVolatile; });
            preferCxxDefaultWrapperForSameDeclaration(best);
            return finish(settle(best, /*legacyLastWins=*/false));
        }

        if (!possible.empty())
        {
            std::vector<const Ranked*> best;
            for (const Ranked& r : possible)
                best.push_back(&r);
            keepLowest(best, [](const Ranked& r) { return r.cxxPointeeConstMismatches; });
            // Non-C++ sets keep the conversion-operator score; C++ sets use the per-argument
            // [over.match.best] sequences above, including converting constructors.
            if (!cxxRanking)
            {
                keepLowest(best, [](const Ranked& r) { return r.userConversions; });
                keepLowest(best, [](const Ranked& r) { return r.userConversionCost; });
            }
            // Prefer agreeing function-pointer shapes, then fewer integer -> bool coercions.
            keepLowest(best, [](const Ranked& r) { return r.shapeMismatches; });
            keepLowest(best, [](const Ranked& r) { return r.boolCoercions; });
            // An overload over the argument's own specialization beats a const-added one.
            keepLowest(best, [](const Ranked& r) { return r.constAddedConversions; });
            keepLowest(best, [](const Ranked& r) { return r.mutableRefPreference; });
            // `T*&` over `T*const&` for a modifiable lvalue pointer, and the reverse for a
            // pointer rvalue - the C++ ranking of the two reference bindings.
            keepLowest(best, [](const Ranked& r) { return r.refPtrConstMismatches; });
            // The candidate's tier is its WORST integer argument (0 identity, 1 promotion,
            // 2 conversion); the lowest tier wins before per-argument comparison.
            keepLowest(best, [](const Ranked& r) {
                int tier = 0;
                for (int cost : r.integerCosts)
                    if (cost > 0)
                        tier = std::max(tier, cost >= kIntegerConversionCost ? 2 : 1);
                if (r.nullPointerConversions > 0)
                    tier = 2;
                return tier;
            });
            std::vector<const Ranked*> undominated;
            for (const Ranked* r : best)
                if (std::none_of(best.begin(), best.end(),
                        [&](const Ranked* other) { return other != r && dominates(*other, *r); }))
                    undominated.push_back(r);
            // Dominance only compares positions both sides judged, so it can cycle; keep the set then.
            if (!undominated.empty())
                best = std::move(undominated);
            // CFlat has no volatile objects: a non-volatile member beats its volatile twin.
            keepLowest(best, [](const Ranked& r) { return (int)r.pair->second.CxxVolatile; });
            preferCxxDefaultWrapperForSameDeclaration(best);
            // Ruling: at the SAME conversion a by-value or rvalue-ref bind beats materializing a
            // temporary for a `const T&`. After dominance, so an identity match still wins first.
            keepLowest(best, [](const Ranked& r) { return r.constRefMaterializations; });
            // Same move tie-break as the perfect tier: `d.add(1, namedLvalue)` must not silently
            // keep the `move` overload and consume the caller's variable.
            keepLowest(best, [](const Ranked& r) { return -r.moveScore; });
            // Two user-defined sequences through DIFFERENT conversion functions are ambiguous in
            // C++, never ordered - report the tie instead of taking the legacy declaration-order
            // pick, which would make the answer depend on header order.
            if (best.size() > 1 && best.front()->userConversions > 0 && tiedOut != nullptr
                && std::any_of(best.begin(), best.end(), [&](const Ranked* r) {
                       return r->userConversionNames != best.front()->userConversionNames; }))
            {
                for (const Ranked* r : best)
                    tiedOut->push_back(r->pair->second);
                return Result{};
            }
            return finish(settle(best, /*legacyLastWins=*/true));
        }

        if (variadicFallback != nullptr)
            return *variadicFallback;
        // Nothing viable: the const-dropping variadic reaches the selected-candidate guard.
        if (variadicConstDropFallback != nullptr)
            return *variadicConstDropFallback;
        return {};
    }

LLVMBackend::ArgumentBinding LLVMBackend::ComputeArgumentPositions(const std::vector<std::string>& argNames,
        const std::vector<TypeAndValue>& targetArguments, bool isVariadic, size_t firstTarget,
        const std::vector<cflat_cinterop::RawDefaultArg>* defaults)
{
        ArgumentBinding binding;

        if (firstTarget > targetArguments.size())
            return binding;

        const size_t inputSize = argNames.size();
        const size_t paramSize = targetArguments.size() - firstTarget;

        if (isVariadic ? inputSize < paramSize : inputSize > paramSize)
            return binding;

        binding.PosMap.assign(inputSize, -1);
        std::vector<bool> usedTargetMap(paramSize);

        // Pass 1: named arguments - resolve to their fixed-param position by name.
        for (size_t posIndex = 0; posIndex < inputSize; posIndex++)
        {
            if (argNames[posIndex].empty())
                continue;

            auto it = std::find_if(targetArguments.begin() + firstTarget, targetArguments.end(),
                [&](const auto& typeAndName) { return argNames[posIndex] == typeAndName.VariableName; });

            if (it == targetArguments.end())
            {
                binding.UnknownName = true;
                binding.FailedName = argNames[posIndex];
                return binding;
            }

            size_t slot = (size_t)std::distance(targetArguments.begin() + firstTarget, it);
            // A second named argument for the same parameter would leave another slot unbound,
            // so the caller would read a default-constructed argument. Reject instead.
            if (usedTargetMap[slot])
            {
                binding.DuplicateName = true;
                binding.FailedName = argNames[posIndex];
                return binding;
            }
            usedTargetMap[slot] = true;
            binding.PosMap[posIndex] = (int64_t)(firstTarget + slot);
        }

        // Pass 2: unnamed arguments - assign to the next free fixed-param slot; for
        // variadic targets, arguments that overflow the fixed params go to trailing slots.
        size_t targetIndex = 0;
        size_t nextVariadicIdx = paramSize;

        for (size_t posIndex = 0; posIndex < inputSize; posIndex++)
        {
            if (!argNames[posIndex].empty())
                continue;

            bool assigned = false;
            while (targetIndex < paramSize)
            {
                if (!usedTargetMap[targetIndex])
                {
                    binding.PosMap[posIndex] = (int64_t)(firstTarget + targetIndex);
                    usedTargetMap[targetIndex] = true;
                    targetIndex++;
                    assigned = true;
                    break;
                }
                targetIndex++;
            }

            if (!assigned)
            {
                if (!isVariadic)
                    return binding;
                binding.PosMap[posIndex] = (int64_t)(firstTarget + nextVariadicIdx++);
            }
        }

        if (std::find(binding.PosMap.begin(), binding.PosMap.end(), -1) != binding.PosMap.end())
            return binding;
        if (!isVariadic && inputSize < paramSize)
        {
            if (defaults == nullptr || defaults->size() < targetArguments.size())
                return binding;
            for (size_t i = firstTarget; i < targetArguments.size(); ++i)
                if (!usedTargetMap[i - firstTarget] && (*defaults)[i].kind.empty())
                    return binding;
        }

        binding.Ok = true;
        return binding;
    }

std::vector<LLVMBackend::NamedVariable> LLVMBackend::MatchFunction(const std::vector<LLVMBackend::NamedVariable>& inputArguments, const std::vector<LLVMBackend::TypeAndValue>& targetArguments, bool isVariadic, bool probe, const std::vector<cflat_cinterop::RawDefaultArg>* defaults)
{
        std::vector<std::string> argNames;
        argNames.reserve(inputArguments.size());
        for (const auto& input : inputArguments)
            argNames.push_back(input.TypeAndValue.VariableName);

        auto binding = ComputeArgumentPositions(argNames, targetArguments, isVariadic, 0, defaults);
        if (!binding.Ok)
        {
            // LogError does not return, so a reported failure never falls through to the
            // reconstruction below - and an unbound slot can therefore never reach a caller.
            if (!probe && binding.UnknownName)
                LogErrorMessage("named argument '{}' does not match any parameter", { binding.FailedName });
            if (!probe && binding.DuplicateName)
                LogErrorMessage("duplicate named argument '{}'", { binding.FailedName });
            return {};
        }

        // Reconstruct arguments in matched order. firstTarget is 0 here, so every PosMap entry
        // indexes the result directly.
        std::vector<LLVMBackend::NamedVariable> result;
        result.reserve(inputArguments.size());
        for (size_t target = 0; target < targetArguments.size(); ++target)
            for (size_t input = 0; input < inputArguments.size(); ++input)
                if (binding.PosMap[input] == (int64_t)target)
                {
                    result.push_back(inputArguments[input]);
                    break;
                }
        if (isVariadic)
            for (size_t input = 0; input < inputArguments.size(); ++input)
                if ((size_t)binding.PosMap[input] >= targetArguments.size())
                    result.push_back(inputArguments[input]);
        return result;
    }

llvm::Value* LLVMBackend::TryEmitAtomicBuiltin(const std::string& name, const std::vector<llvm::Value*>& args)
{
        using namespace llvm;
        auto& ctx = *context;

        // MSVC's _Interlocked* names are clang builtins with no linkable library symbol.
        // Lower the Windows API's seq_cst operations directly to the equivalent LLVM atomic.
        if (name.starts_with("_Interlocked"))
        {
            if (args.empty() || args[0] == nullptr) return nullptr;
            // Exact <op><width> spellings only: _InterlockedCompareExchange128 (a 16-byte CAS with
            // four operands), _Interlockedbittestandset and friends keep the plain extern path.
            const std::string_view rest = std::string_view(name).substr(std::string_view("_Interlocked").size());
            std::string_view opName, suffix;
            for (std::string_view candidate : {"CompareExchange", "ExchangeAdd", "Exchange", "Increment",
                                               "Decrement", "And", "Or", "Xor"})
            {
                if (rest.starts_with(candidate)) { opName = candidate; suffix = rest.substr(candidate.size()); break; }
            }
            const bool pointer = suffix == "Pointer";
            const bool incDec = opName == "Increment" || opName == "Decrement";
            const bool exchangeOp = opName == "Exchange" || opName == "CompareExchange";
            unsigned width = 32;
            if (suffix == "8" && !incDec) width = 8;
            else if (suffix == "16") width = 16;
            else if (suffix == "64") width = 64;
            else if (opName.empty() || !(suffix.empty() || (pointer && exchangeOp))) return nullptr;
            if (pointer && args.size() < 2) return nullptr;
            Type* valueType = pointer ? args[1]->getType() : IntegerType::get(ctx, width);
            auto integerArg = [&](size_t i) -> Value* {
                if (i >= args.size()) return nullptr;
                if (args[i]->getType() == valueType) return args[i];
                return builder->CreateIntCast(args[i], valueType, true, "interlocked_arg");
            };
            const AtomicOrdering order = AtomicOrdering::SequentiallyConsistent;
            AtomicRMWInst::BinOp op;
            Value* operand = nullptr;
            bool returnsNew = false;
            const bool increment = opName == "Increment";
            const bool decrement = opName == "Decrement";
            if (increment || decrement)
            {
                op = AtomicRMWInst::Add;
                operand = ConstantInt::get(valueType, increment ? 1 : -1, true);
                returnsNew = true;
            }
            else if (opName == "ExchangeAdd")
            {
                op = AtomicRMWInst::Add;
                operand = integerArg(1);
            }
            else if (opName == "Exchange")
            {
                op = AtomicRMWInst::Xchg;
                operand = pointer ? args[1] : integerArg(1);
            }
            else if (opName == "And")
            {
                op = AtomicRMWInst::And;
                operand = integerArg(1);
            }
            else if (opName == "Or")
            {
                op = AtomicRMWInst::Or;
                operand = integerArg(1);
            }
            else if (opName == "Xor")
            {
                op = AtomicRMWInst::Xor;
                operand = integerArg(1);
            }
            else if (opName == "CompareExchange")
            {
                if (args.size() < 3) return nullptr;
                Value* exchange = pointer ? args[1] : integerArg(1);
                Value* comparand = pointer ? args[2] : integerArg(2);
                auto* pair = builder->CreateAtomicCmpXchg(args[0], comparand, exchange,
                    MaybeAlign(), order, order, SyncScope::System);
                return builder->CreateExtractValue(pair, 0, "interlocked_old");
            }
            else return nullptr;

            if (operand == nullptr) return nullptr;
            Value* old = builder->CreateAtomicRMW(op, args[0], operand, MaybeAlign(), order,
                                                   SyncScope::System);
            return returnsNew
                ? builder->CreateAdd(old, operand, "interlocked_new") : old;
        }

        // arg[0] is always the pointer to the _value field (i64* or i32*)
        if (name == "__atomic_counter_increment" || name == "__atomic_counter_decrement" ||
            name == "__atomic_counter_add")
        {
            // atomicrmw add/sub relaxed ptr, delta -> returns old value; we return old+delta
            Value* ptr   = args[0];
            Value* delta = (name == "__atomic_counter_decrement")
                ? ConstantInt::get(Type::getInt64Ty(ctx), -1)
                : (args.size() > 1 ? args[1] : ConstantInt::get(Type::getInt64Ty(ctx), 1));
            auto op = (name == "__atomic_counter_decrement")
                ? AtomicRMWInst::Add  // add(-1) == sub
                : AtomicRMWInst::Add;
            auto* old = builder->CreateAtomicRMW(op, ptr, delta,
                MaybeAlign(), AtomicOrdering::Monotonic);
            // return old + delta (new value)
            return builder->CreateAdd(old, delta, "atomic_new");
        }
        if (name == "__atomic_counter_read")
        {
            Value* ptr = args[0];
            auto* li = builder->CreateLoad(Type::getInt64Ty(ctx), ptr, "atomic_load");
            li->setAtomic(AtomicOrdering::SequentiallyConsistent);
            li->setAlignment(Align(8));
            return li;
        }
        if (name == "__atomic_flag_test_and_set")
        {
            Value* ptr = args[0];
            auto* one = ConstantInt::get(Type::getInt32Ty(ctx), 1);
            // xchg acquire: returns old value; 0 means we acquired the flag
            auto* old = builder->CreateAtomicRMW(AtomicRMWInst::Xchg, ptr, one,
                MaybeAlign(), AtomicOrdering::Acquire);
            // return true if old was 1 (flag was already set = contention)
            return builder->CreateICmpNE(old, ConstantInt::get(Type::getInt32Ty(ctx), 0), "was_set");
        }
        if (name == "__atomic_flag_clear")
        {
            Value* ptr = args[0];
            auto* zero = ConstantInt::get(Type::getInt32Ty(ctx), 0);
            auto* si = builder->CreateStore(zero, ptr);
            si->setAtomic(AtomicOrdering::Release);
            si->setAlignment(Align(4));
            return ConstantInt::get(Type::getInt32Ty(ctx), 0); // void: unused
        }
        if (name == "__atomic_i32_load" || name == "__atomic_i64_load")
        {
            bool is64 = (name == "__atomic_i64_load");
            Value* ptr = args[0];
            auto* ty = is64 ? Type::getInt64Ty(ctx) : Type::getInt32Ty(ctx);
            auto* li = builder->CreateLoad(ty, ptr, "atomic_load");
            li->setAtomic(AtomicOrdering::SequentiallyConsistent);
            li->setAlignment(is64 ? Align(8) : Align(4));
            return li;
        }
        if (name == "__atomic_i32_store" || name == "__atomic_i64_store")
        {
            bool is64 = (name == "__atomic_i64_store");
            Value* ptr = args[0];
            Value* val = args[1];
            auto* si = builder->CreateStore(val, ptr);
            si->setAtomic(AtomicOrdering::SequentiallyConsistent);
            si->setAlignment(is64 ? Align(8) : Align(4));
            return ConstantInt::get(Type::getInt32Ty(ctx), 0); // void: unused
        }
        if (name == "__atomic_i32_cas" || name == "__atomic_i64_cas")
        {
            bool is64 = (name == "__atomic_i64_cas");
            Value* ptr      = args[0];
            Value* expected = args[1];
            Value* desired  = args[2];
            auto* result = builder->CreateAtomicCmpXchg(ptr, expected, desired,
                MaybeAlign(),
                AtomicOrdering::AcquireRelease,
                AtomicOrdering::Monotonic);
            // extract the success bit (second element of {T, i1})
            return builder->CreateExtractValue(result, 1, "cas_ok");
        }
        if (name == "__atomic_release_store_i32" || name == "__atomic_release_store_i64" || name == "__atomic_release_store_flag")
        {
            bool is64   = (name == "__atomic_release_store_i64");
            bool isFlag = (name == "__atomic_release_store_flag");
            Value* ptr = args[0];
            Value* val = args[1];
            // bool is i1 in LLVM; atomic ops require byte-sized types - widen to i32.
            if (isFlag)
                val = builder->CreateZExt(val, Type::getInt32Ty(ctx), "flag_i32");
            auto* si = builder->CreateStore(val, ptr);
            si->setAtomic(AtomicOrdering::Release);
            si->setAlignment(is64 ? Align(8) : Align(4));
            return ConstantInt::get(Type::getInt32Ty(ctx), 0); // void: unused
        }
        if (name == "__atomic_acquire_load_i32" || name == "__atomic_acquire_load_i64" || name == "__atomic_acquire_load_flag")
        {
            bool is64   = (name == "__atomic_acquire_load_i64");
            bool isFlag = (name == "__atomic_acquire_load_flag");
            Value* ptr = args[0];
            auto* ty = is64 ? Type::getInt64Ty(ctx) : Type::getInt32Ty(ctx);
            auto* li = builder->CreateLoad(ty, ptr, "atomic_acq_load");
            li->setAtomic(AtomicOrdering::Acquire);
            li->setAlignment(is64 ? Align(8) : Align(4));
            // bool return: compare i32 result with zero.
            if (isFlag)
                return builder->CreateICmpNE(li, ConstantInt::get(Type::getInt32Ty(ctx), 0), "flag_bool");
            return li;
        }
        return nullptr; // not an atomic builtin
    }

bool LLVMBackend::RejectFuncPtrShapeMismatch(const NamedVariable& arg, const TypeAndValue& param)
{
        if (!param.IsFunctionPointer && !IsEncodedClosureType(param.TypeName))
            return false;

        int paramShape = FunctionPointerShapeOf(param, nullptr);
        int argShape = FunctionPointerShapeOf(arg.TypeAndValue, &arg);
        bool argIsNullLiteral = arg.Primary != nullptr
            && llvm::isa<llvm::ConstantPointerNull>(arg.Primary);

        const char* shapesAre = "'function<>', 'function<>*' and 'function<>[]' are three distinct "
            "shapes and none converts to another implicitly.";

        if (paramShape != 0 && argShape == 0 && !argIsNullLiteral)
        {
            // '&' produces a POINTER, so that advice is false for a view parameter.
            const char* advice = paramShape == 2
                ? "Pass a 'function<>[N]' array or another view to supply the shape it expects."
                : "Take the address with '&' to supply the shape it expects.";
            LogErrorMessage("cannot pass {} to closure parameter '{}', which expects {}: {} {}",
                { FuncPtrShapeWord(arg.TypeAndValue, &arg), param.VariableName,
                  FuncPtrShapeWord(param, nullptr), shapesAre, advice });
            return true;
        }

        if (paramShape == 0 && argShape == 2)
        {
            LogErrorMessage("cannot pass {} to closure parameter '{}', which expects {}: {} "
                "Index an element to supply the shape it expects.",
                { FuncPtrShapeWord(arg.TypeAndValue, &arg), param.VariableName,
                  FuncPtrShapeWord(param, nullptr), shapesAre });
            return true;
        }

        return false;
    }

bool LLVMBackend::RejectCodeValueIntoDataParam(const NamedVariable& arg, const TypeAndValue& param,
        const std::string& ifaceName, const std::string& methodName)
{
        // An interface parameter is left to the boxing path, exactly as the shape gate above is.
        if (param.IsInterface) return false;
        if (!CodeValueIntoDataDestination(arg, param)) return false;

        // Spelled from the DECLARED parameter, the only type the vtable slot knows.
        std::string spelling = SpellType(*this, param);
        // The cast escape is advised only where it compiles - a view rejects a raw 'T*' by its own
        // rule, and '(string)' of a raw value is itself refused.
        std::string advice = (param.Pointer && !param.IsArrayView && param.ConstArraySize == 0)
            ? spelling : std::string();
        // Unmangle the callee only on the instantiation REGISTRY, never on a bare '__' in the name.
        std::string callee = SpellType(*this, TypeAndValue{ .TypeName = ifaceName });
        callee += "." + methodName;
        std::string what = param.VariableName.empty()
            ? std::format("parameter of '{}'", callee)
            : std::format("parameter '{}' of '{}'", param.VariableName, callee);
        LogRawError(DescribeCodeValueIntoData(spelling, "pass", advice, what));
        return true;
    }

void LLVMBackend::LogUniqueCopyError(const std::string& typeName,
                                     const std::string& uniquePath) const
{
        const std::string displayType = SpellType(*this, TypeAndValue{ .TypeName = typeName });
        if (IsCoreUniqueType(typeName))
        {
            LogErrorMessage(
                "cannot copy '{}': unique<T> owns its pointee and has no deep-clone - 'move' it "
                "to transfer ownership, or clone the pointee yourself", { displayType });
            return;
        }
        if (HasTypeAnnotation(typeName, "unique"))
        {
            LogErrorMessage(
                "cannot copy '{}': it is declared '[unique]' (move-only). Write a 'copy()' method "
                "for '{}' or 'move' the value instead.", { displayType, displayType });
            return;
        }
        auto pathIsCoreUnique = [&](const auto& self, const std::string& owner,
                                    const std::string& path) -> bool {
            auto ownerIt = dataStructures.find(owner);
            if (ownerIt == dataStructures.end()) return false;
            size_t dot = path.find('.');
            std::string fieldName = dot == std::string::npos ? path : path.substr(0, dot);
            for (const auto& field : ownerIt->second.StructFields)
            {
                if (field.VariableName != fieldName) continue;
                if (dot == std::string::npos) return IsCoreUniqueType(field.TypeName);
                return self(self, field.TypeName, path.substr(dot + 1));
            }
            return false;
        };
        if (pathIsCoreUnique(pathIsCoreUnique, typeName, uniquePath))
            LogErrorMessage(
                "cannot copy '{}': its field '{}.{}' is 'unique' (a unique<T> wrapper), so it "
                "owns a raw pointer that has no generic deep-clone. A memberwise copy would share "
                "the pointer between two owners and double-free at teardown. Write a '{}' method "
                "for '{}' that clones the pointee itself, or '{}' the value to transfer ownership "
                "instead of copying it.",
                { displayType, displayType, uniquePath, "copy()", displayType, "move" });
        else
            LogErrorMessage(
                "cannot copy '{}': its field '{}.{}' is 'unique', so it owns a raw pointer that has "
                "no generic deep-clone. A memberwise copy would share the pointer between two owners "
                "and double-free at teardown. Write a '{}' method for '{}' that clones the pointee "
                "itself, or '{}' the value to transfer ownership instead of copying it.",
                { displayType, displayType, uniquePath, "copy()", displayType, "move" });
}

/*
 * Array-view PARAMETER gate, shared by the direct-call door (CreateOverloadedFunctionCall) and the
 * virtual-dispatch door (CallInterfaceMethod), which lowers a vtable slot's arguments by the same
 * ABI and so needs the same rejections. Three axes: a raw 'T*' would forge the whole-allocation
 * contract a view promises; a single VALUE is not one element of a view; a view whose
 * ELEMENT differs strides and loads with the wrong shape inside the callee.
 * Returns true when any axis logged.
 */
bool LLVMBackend::RejectArrayViewParamBinding(const NamedVariable& arg, const TypeAndValue& param,
                                              const std::string& diagnosticFunctionName)
{
        bool rejected = false;
        std::string destElement;
        std::string srcElement;
        // A view ARGUMENT may reach here as a loaded value whose TypeName is blank; recover
        // the declared element name from the named variable so the message can spell it.
        LLVMBackend::TypeAndValue argTV = arg.TypeAndValue;
        if (argTV.TypeName.empty() && !arg.CallerName.empty())
        {
            if (const NamedVariable* live = FindLiveNamedVariable(arg.CallerName))
            {
                const TypeAndValue* declared = &live->TypeAndValue;
                if (arg.FieldName.empty() && !argTV.IsArrayView && !argTV.IsSimd)
                {
                    argTV.TypeName = declared->TypeName;
                    argTV.Pointer = argTV.Pointer || declared->Pointer;
                }
                else if (arg.FieldName.empty() && declared->IsArrayView)
                    argTV.TypeName = declared->TypeName;
                // A FIELD read: CallerName names the base struct, so the element name
                // lives on the field's declaration, not on the base's TypeName.
                else if (auto base = dataStructures.find(declared->TypeName);
                         !arg.FieldName.empty() && base != dataStructures.end())
                    for (const auto& f : base->second.StructFields)
                        if (f.VariableName == arg.FieldName
                            && (f.IsArrayView
                                || (argTV.ConstArraySize != 0 && !argTV.IsArrayView
                                    && !argTV.IsSimd && f.ConstArraySize != 0 && !f.IsSimd)))
                        {
                            argTV.TypeName = f.TypeName;
                            if (!f.IsArrayView) argTV.Pointer = argTV.Pointer || f.Pointer;
                        }
            }
        }
        // A GLOBAL fixed array carries no CallerName at all, so reach its declaration through the
        // storage it was addressed from - the only handle on a global at this door.
        if (argTV.TypeName.empty() && argTV.ConstArraySize != 0 && !argTV.IsArrayView
            && !argTV.IsSimd)
            if (const TypeAndValue* declared = FindDeclaredTypeAndValueForStorage(arg.Storage);
                declared != nullptr && declared->ConstArraySize != 0 && !declared->IsArrayView
                && !declared->IsSimd)
            {
                argTV.TypeName = declared->TypeName;
                argTV.Pointer = argTV.Pointer || declared->Pointer;
            }
        // The reverse ('T[] -> T*' decay) is always safe; a view argument carries IsArrayView.
        // A fixed pointer-element array is an array source too; let the element gate reshape it.
        if (param.IsArrayView && argTV.Pointer && !argTV.IsArrayView
            && !(argTV.ConstArraySize != 0 && argTV.Pointer))
        {
            // The ARGUMENT's own recorded name wins: a cast ('(int*)q') leaves TypeName empty, so
            // the declaration recovery above would spell 'q' - the type BEFORE the cast.
            std::string sourceSpelling;
            if (!arg.InferSourceTypeName.empty())
            {
                TypeAndValue inferred;
                inferred.TypeName = arg.InferSourceTypeName;
                inferred.Pointer = argTV.Pointer;
                inferred.ElemPointer = argTV.ElemPointer;
                inferred.PointerDepth = argTV.PointerDepth;
                sourceSpelling = SpellType(*this, inferred);
            }
            if (sourceSpelling.empty()) sourceSpelling = SpellType(*this, argTV);
            if (sourceSpelling.empty()) sourceSpelling = "<unknown>";
            LogErrorMessage(
                "cannot pass a raw pointer '{}' as array-view parameter '{}' ('{}') - a view "
                "must span a whole allocation (it comes only from '{}' or another '{}'); "
                "the '{} -> {}' decay is one-way",
                { sourceSpelling, param.VariableName, SpellType(*this, param),
                  "new T[n]", "T[]", "T[]", "T*" });
            rejected = true;
        }

        rejected |= RejectValueIntoArrayViewParam(argTV, param);

        if (ArrayViewElementMismatch(param, argTV, destElement, srcElement))
        {
            LogErrorMessage(
                "cannot pass an array view of '{}' as parameter '{}' of '{}', whose element "
                "is '{}' - a view indexes by its own element, so the elements must match",
                { srcElement, param.VariableName, diagnosticFunctionName, destElement });
            rejected = true;
        }
        return rejected;
}

bool LLVMBackend::IsCxxTemplateSpecializationSymbol(const FunctionSymbol& symbol) const
{
        const std::string declared = CxxDeclarationLinkageName(symbol.UniqueName);
        if (declared.starts_with('?')) return declared.starts_with("??$");
        if (!declared.starts_with("_Z")) return false;
        llvm::ItaniumPartialDemangler demangler;
        if (demangler.partialDemangle(declared.c_str())) return false;
        char* name = demangler.getFunctionName(nullptr, nullptr);
        const bool templated = name != nullptr && std::string_view(name).ends_with('>');
        std::free(name);
        return templated;
}

bool LLVMBackend::CxxMemberIsOnlyNonConst(const std::string& recordName,
                                          const std::string& memberName,
                                          size_t argumentCount) const
{
        if (argumentCount == 0) return false;
        std::set<std::string> visited;
        auto inspect = [&](auto&& self, const std::string& typeName, bool& hasMember) -> bool {
            if (!visited.insert(typeName).second) return false;
            auto record = cxxRecordEntries_.find(typeName);
            if (record == cxxRecordEntries_.end()) return false;
            bool declaresName = false;
            bool hasConst = false, hasNonConst = false;
            for (const auto& member : record->second.members)
            {
                if (member.kind != cflat_cinterop::RawCxxMember::Instance
                    || member.name != memberName)
                    continue;
                declaresName = true;
                if (member.access != cflat_cinterop::AccessPublic) continue;
                if (member.paramTypes.size() < 1) continue;
                const size_t required = [&] {
                    size_t count = member.paramTypes.size();
                    while (count > 1 && member.defaultArgs.size() >= count
                           && !member.defaultArgs[count - 1].kind.empty())
                        --count;
                    return count;
                }();
                const bool arityFits = argumentCount >= required
                    && (member.variadic || argumentCount <= member.paramTypes.size());
                if (!arityFits) continue;
                (member.isConst ? hasConst : hasNonConst) = true;
            }
            const size_t explicitArgumentCount = argumentCount - 1;
            for (const auto& [templateName, templates] : cxxFunctionTemplates_)
                for (const auto& member : templates)
                {
                    if (member.kind != cflat_cinterop::RawFunctionTemplate::InstanceMember
                        || member.owner != typeName || member.memberName != memberName
                        || explicitArgumentCount < member.minArity
                        || explicitArgumentCount > member.maxArity)
                        continue;
                    declaresName = true;
                    (member.isConst ? hasConst : hasNonConst) = true;
                }
            if (declaresName)
            {
                hasMember = true;
                return hasNonConst && !hasConst;
            }
            bool inheritedOnlyNonConst = false;
            size_t inheritedMatches = 0;
            for (const auto& base : record->second.bases)
            {
                if (base.access != cflat_cinterop::AccessPublic || base.isVirtual) continue;
                bool baseHasMember = false;
                const bool baseOnlyNonConst = self(self, base.name, baseHasMember);
                if (baseHasMember)
                {
                    ++inheritedMatches;
                    inheritedOnlyNonConst = baseOnlyNonConst;
                }
            }
            hasMember = inheritedMatches != 0;
            return inheritedMatches == 1 && inheritedOnlyNonConst;
        };
        bool hasMember = false;
        return inspect(inspect, recordName, hasMember);
}

llvm::Value* LLVMBackend::CreateOverloadedFunctionCall(const std::string& functionNameIn, const std::vector<LLVMBackend::NamedVariable>& arguments, bool forceRoot,
        const std::string& displayName, const std::string& cxxMemberReceiver,
        bool postfixMemberCall, const std::string& enclosingFunctionName)
{
        // These describe only the call being lowered. Clear them before overload probing so a
        // later non-C++ call cannot make a chained result reuse an earlier sret temporary.
        lastCxxRetTemp_ = nullptr;
        lastCxxRetValue_ = nullptr;
        std::string functionName = ResolveQualifiedName(functionNameIn, forceRoot);
        const bool inGlobalInitThunk = currentFunction != nullptr
            && (currentFunction->getName().starts_with("__global")
                || currentFunction->getName().starts_with("__cflat_global"));
        if (!forceRoot && dataStructures.count(functionName) != 0
            && (currentFunction == nullptr || currentFunction->getName() != functionName)
            && !inGlobalInitThunk
            && !IsCxxRecord(functionName))
        {
            // A CFlat constructor is the first point where an unavailable C++ member default
            // becomes observable. The synthesized body was emitted at the type declaration and
            // intentionally leaves such a member zeroed until this real call is requested.
            for (const auto& field : GetDataStructure(functionName).StructFields)
            {
                if (field.Pointer || field.ConstArraySize != 0
                    || field.BraceInitializer != nullptr
                    || (field.Initializer != nullptr
                        && field.Initializer->Default() == nullptr))
                    continue;
                auto* fieldType = GetType(field);
                if (fieldType == nullptr || fieldType->isArrayTy()
                    || !fieldType->isStructTy() || !IsCxxRecord(field.TypeName)
                    || !(CxxElementNeedsDefaultConstruction(field.TypeName)
                        || HasNonPublicCxxDefaultCtor(field.TypeName)))
                    continue;
                std::string bindError;
                TryBindCxxImplicitDefaultCtor(field.TypeName, bindError);
                if (!bindError.empty()) LogErrorMessage("{}", { bindError });
                if (FindCxxDefaultCtor(field.TypeName) == nullptr)
                {
                    const auto* info = GetCxxClassInfo(field.TypeName);
                    LogErrorMessage(
                        "C++ class '{}' has no default constructor cflat can call{}",
                        { DisplayCxxClassName(field.TypeName),
                          info != nullptr && info->hasDeletedDefaultCtor
                              ? " (it is deleted)" : "" });
                }
            }
        }
        std::string shownFunctionName = displayName;
        // Set only when THIS call attempted an implicit-conversion wrapper that was refused.
        std::string implicitConversionRefusal;
        if (shownFunctionName.empty())
        {
            shownFunctionName = dataStructures.contains(functionName)
                ? SpellType(*this, TypeAndValue{ .TypeName = functionName })
                : SpellFunctionSymbol(*this, functionName);
            if (IsCoreUniqueType(functionName))
                shownFunctionName = SpellType(*this, TypeAndValue{ .TypeName = functionName });
        }

        // Implicit `copy()` synthesis: a value type with no copy() of its own gets a memberwise
        // one generated on demand (the "every value type has an implicit copy() if undefined"
        // rule). Fires for both user `x.copy()` and the compiler's internal copy calls; an
        // existing copy() always wins (HasCopyOverloadFor). Pointers/primitives are skipped.
        if (functionName == "copy" && arguments.size() == 1
            && !arguments[0].TypeAndValue.Pointer
            && !arguments[0].TypeAndValue.TypeName.empty()
            && dataStructures.count(arguments[0].TypeAndValue.TypeName)
            && !HasCopyOverloadFor(arguments[0].TypeAndValue.TypeName))
        {
            // The closure fat type gets an env-cloning copy (not a memberwise one - both its
            // fields are pointers, which a memberwise copy would shallow-share and double-free).
            const std::string& copyType = arguments[0].TypeAndValue.TypeName;
            if (copyType == "__closure_fat_ptr")
                EnsureClosureLifetimeRegistered();
            else
            {
                if (HasTypeAnnotation(copyType, "unique"))
                {
                    LogUniqueCopyError(copyType);
                    return nullptr;
                }
                // A synthesized destructor is the ownership boundary now that unique fields
                // desugar to the real unique<T> wrapper. Keep the old unique diagnostic for the
                // unsafe ownership cases, but key memberwise-copy suppression on the destructor.
                if (HasNonTrivialDestructor(copyType))
                {
                    std::string uniquePath;
                    bool uniqueOwner = TypeOwnsUniquePointer(copyType, &uniquePath);
                    if (uniqueOwner || StructSynthCopyUnsafe(copyType))
                    {
                        if (uniqueOwner)
                            LogUniqueCopyError(copyType, uniquePath);
                        else
                            LogError(std::format(
                                "cannot copy '{}': it has a non-trivial destructor and a shallow-copied "
                                "pointer/view field but no 'copy()' method. Write a 'copy()' method that "
                                "defines independent state, or 'move' the value instead of copying it.",
                                SpellType(*this, TypeAndValue{ .TypeName = copyType })));
                        return nullptr;
                    }
                    if (IsLoweredCFlatOnlyStruct(copyType))
                    {
                        auto* valueType = GetType(TypeAndValue{ .TypeName = copyType });
                        llvm::Value* sourceSlot = arguments[0].Storage;
                        if (sourceSlot == nullptr && arguments[0].Primary == lastLoweredRetValue_)
                            sourceSlot = lastLoweredRetTemp_;
                        if (sourceSlot == nullptr)
                            if (auto* loaded = llvm::dyn_cast_or_null<llvm::LoadInst>(
                                    arguments[0].Primary);
                                loaded != nullptr && loaded->getType() == valueType)
                                sourceSlot = loaded->getPointerOperand();
                        if (sourceSlot == nullptr)
                        {
                            LogError(std::format(
                                "cannot copy struct '{}' for synthesized copy operation: source has "
                                "no address", copyType));
                            return nullptr;
                        }
                        auto* resultSlot = AllocaAtEntry(valueType, nullptr, "lowered.copy");
                        if (!EmitLoweredMemberwiseCopy(copyType, valueType, resultSlot, sourceSlot,
                                                       "in synthesized copy"))
                            return nullptr;
                        lastCallReturnType = TypeAndValue{ copyType, "", false };
                        auto* resultValue = builder->CreateLoad(valueType, resultSlot,
                                                               "lowered.copy.value");
                        lastLoweredRetTemp_ = resultSlot;
                        lastLoweredRetValue_ = resultValue;
                        return resultValue;
                    }
                    GetOrCreateMemberwiseCopy(copyType);
                }
                // A POD struct has no destructor and uses the bitwise fallback below.
            }
        }

        // Bitwise-copy fallback. The copy is deep only for an OWNING value type (string, a struct
        // with owning fields, a container, a closure), all handled above by the memberwise synth /
        // closure copy / unique error, or by a real copy() overload found in resolution below. Every
        // OTHER value reaching copy() - a pointer (incl. an interface pointer), a thin function
        // value, an enum, a primitive, a POD struct - has no deep-copy: the copy is the same bits
        // (it shares any pointee). Return them bitwise here, mirroring how '.~()' gracefully no-ops
        // on these types (GetOrCreateFullDestructor returns null). Firing before resolution also lets
        // list<enum>/list<IShape> copy() resolve and keeps a thin function off __closure_fat_ptr.copy.
        // arguments[0] is read only after the size==1 short-circuit (a 0-arg call must not index it).
        // A pointer copy is always a bitwise share (it copies the address, not the pointee), even
        // when the pointee type owns resources - so a pointer bypasses the owning / real-copy
        // guards that only apply to a by-value receiver.
        if (functionName == "copy" && arguments.size() == 1
            && arguments[0].TypeAndValue.TypeName != "__closure_fat_ptr"
            && (arguments[0].TypeAndValue.Pointer
                || (!IsOwningValueType(arguments[0].TypeAndValue.TypeName)
                    && !HasRealCopyOverloadFor(arguments[0].TypeAndValue.TypeName))))
        {
            const auto& arg = arguments[0];
            // A bitwise copy has the receiver's own type; publish it so the caller classifies the
            // result correctly (e.g. keeps IsInterface, so it binds to an interface parameter).
            lastCallReturnType = arg.TypeAndValue;
            // Interface element: the slot holds a bare fat {vtable,data} value. Load that from
            // Storage (Primary is a mis-classified single pointer) so it binds an interface param.
            if (arg.TypeAndValue.IsInterface && arg.Storage != nullptr)
                return CreateLoad(GetFatPtrType(), arg.Storage);
            if (arg.Primary != nullptr)
                return arg.Primary;
            if (arg.Storage != nullptr)
                return arg.BaseType && arg.UnionFieldType == nullptr
                    ? static_cast<llvm::Value*>(CreateLoad(arg.BaseType, arg.Storage))
                    : LoadArgStorage(arg);
        }

        // Deferred C++ binding: complete this name's overload set before resolving against it.
        if (TryBindCxxFunction(functionName)) { /* bound now, or already was */ }
        std::string bareMemberName = functionName;
        if (const size_t dot = bareMemberName.find_last_of('.'); dot != std::string::npos)
            bareMemberName.erase(0, dot + 1);
        std::string receiverType = arguments.empty()
            ? std::string() : arguments.front().TypeAndValue.TypeName;
        if (receiverType.empty() && !arguments.empty() && arguments.front().BaseType)
            if (auto* receiverStruct = llvm::dyn_cast<llvm::StructType>(
                    arguments.front().BaseType))
                receiverType = receiverStruct->getName().str();
        // Compiler-synthesized member calls (view decay size/data) name lazy C++ members too.
        if ((postfixMemberCall || !cxxMemberReceiver.empty()) && IsCxxRecord(receiverType))
            EnsureCxxMemberProjected(receiverType, bareMemberName);
        /*
         * A const receiver (const namespace object, const reference result, pointee-const
         * pointer) calls the const twin of a const/non-const member pair like clang; the
         * non-const one would write read-only storage. MainListener::RefuseCxxConstReceiverCall
         * refuses a named call with no const overload at all.
         */
        const std::string constTwinPrefix = CxxConstTwinName("");
        if (!arguments.empty() && !bareMemberName.starts_with(constTwinPrefix)
            && IsCxxRecord(receiverType))
        {
            const int constKind = CxxConstReceiverKind(arguments.front());
            if (constKind != 0)
            {
                EnsureCxxMemberProjected(receiverType, bareMemberName);
                const std::string twinName = CxxConstTwinName(bareMemberName);
                /*
                 * Twins are registered (or cloned by inheritance) per owning class, and a class
                 * declaring the name hides the base's, so only the receiver's own twin counts.
                 * The const receiver ranks the whole const-callable set: its twins plus the
                 * receiver's unique-signature const members registered under the plain name.
                 */
                bool twinFits = false;
                if (auto twin = functionTable.find(twinName); twin != functionTable.end())
                {
                    for (const auto& candidate : twin->second)
                        if (candidate.IsMethod && !candidate.Parameters.empty()
                            && candidate.Parameters.front().TypeName == receiverType)
                            twinFits = true;
                    if (twinFits)
                        if (auto plain = functionTable.find(bareMemberName);
                            plain != functionTable.end())
                        {
                            std::vector<FunctionSymbol> constPlain;
                            for (const auto& candidate : plain->second)
                                if (candidate.IsCxx && candidate.IsMethod && candidate.CxxConst
                                    && !candidate.Parameters.empty()
                                    && candidate.Parameters.front().TypeName == receiverType
                                    && std::none_of(twin->second.begin(), twin->second.end(),
                                           [&](const FunctionSymbol& t) {
                                               return t.UniqueName == candidate.UniqueName
                                                   && !t.Parameters.empty()
                                                   && t.Parameters.front().TypeName == receiverType;
                                           }))
                                    constPlain.push_back(candidate);
                            for (auto& candidate : constPlain)
                                twin->second.push_back(std::move(candidate));
                        }
                }
                if (twinFits)
                    return CreateOverloadedFunctionCall(twinName, arguments, forceRoot,
                        displayName.empty() ? shownFunctionName : displayName,
                        cxxMemberReceiver, postfixMemberCall, enclosingFunctionName);
            }
        }
        const bool receiverHasCxxMember = (postfixMemberCall || !cxxMemberReceiver.empty())
            && !receiverType.empty()
            && CxxClassHasMemberNamed(receiverType, bareMemberName);
        auto originalFuncSym = functionTable.find(functionName);
        auto funcSym = originalFuncSym;
        std::vector<FunctionSymbol> receiverMemberCandidates;
        const auto bareMemberSet = functionTable.find(bareMemberName);
        if ((postfixMemberCall || receiverHasCxxMember) && !arguments.empty()
            && bareMemberSet != functionTable.end())
        {
            for (const auto& candidate : bareMemberSet->second)
            {
                if (!candidate.IsMethod || candidate.Parameters.empty()) continue;
                const std::string& ownerType = candidate.Parameters.front().TypeName;
                if (ownerType == receiverType
                    || (candidate.IsCxx && !receiverType.empty()
                        && IsCxxBaseOf(ownerType, receiverType)))
                    receiverMemberCandidates.push_back(candidate);
            }
            if (receiverHasCxxMember && !receiverMemberCandidates.empty())
                funcSym = bareMemberSet;
        }
        if (funcSym == functionTable.end())
        {
            if (TryBindCxxImplicitArgumentConversions(functionName, arguments))
                return CreateOverloadedFunctionCall(functionName, arguments, forceRoot, displayName,
                                                    cxxMemberReceiver, postfixMemberCall,
                                                    enclosingFunctionName);
            if (std::string refusal = GetCxxBindingRefusal(functionName); !refusal.empty())
                LogErrorMessage(refusal);
            else if (displayName.empty())
                LogErrorMessage("unknown function '{}'", { shownFunctionName });
            else
                LogErrorMessage("unknown generic function '{}'", { displayName });
            return nullptr;
        }

        const std::vector<FunctionSymbol>* candidateSet = &funcSym->second;
        const bool cxxReceiverHasMember = !cxxMemberReceiver.empty()
            && CxxClassHasMemberNamed(cxxMemberReceiver, bareMemberName);
        if ((postfixMemberCall || cxxReceiverHasMember) && !arguments.empty())
        {
            const auto& shadowCandidates = originalFuncSym != functionTable.end()
                ? originalFuncSym->second : funcSym->second;
            const bool currentFreeFunctionIsCandidate = std::any_of(
                shadowCandidates.begin(), shadowCandidates.end(), [&](const auto& candidate) {
                    return !candidate.IsMethod && candidate.Function != nullptr
                        && candidate.Function == currentFunction;
                });
            const std::string enclosingShortName = enclosingFunctionName.substr(
                enclosingFunctionName.find_last_of('.') == std::string::npos ? 0
                    : enclosingFunctionName.find_last_of('.') + 1);
            const bool enclosingFreeFunctionIsCandidate = !enclosingShortName.empty()
                && std::any_of(shadowCandidates.begin(), shadowCandidates.end(),
                    [&](const auto& candidate) {
                        const size_t sourceDot = candidate.SourceName.find_last_of('.');
                        const std::string sourceShortName = candidate.SourceName.substr(
                            sourceDot == std::string::npos ? 0 : sourceDot + 1);
                        return !candidate.IsMethod && sourceShortName == enclosingShortName;
                    });
            const bool receiverFirstFreeFunctionIsCandidate = !arguments.empty()
                && std::any_of(shadowCandidates.begin(), shadowCandidates.end(),
                    [&](const auto& candidate) {
                        return !candidate.IsMethod && !candidate.IsCxx
                            && !candidate.UniqueName.starts_with("__cflat_tpl_")
                            && !candidate.UniqueName.starts_with("__cflat_free_")
                            && !candidate.Parameters.empty()
                            && candidate.Parameters.front().TypeName == receiverType;
                    });
            if (!receiverMemberCandidates.empty()
                && (currentFreeFunctionIsCandidate || enclosingFreeFunctionIsCandidate
                    || (receiverHasCxxMember && receiverFirstFreeFunctionIsCandidate)))
                candidateSet = &receiverMemberCandidates;
        }
        std::vector<FunctionSymbol> cxxOperatorCandidates;
        // Unary operators too: a derived `operator-(int)` hides the base's unary `operator-()`.
        if (functionName.starts_with("operator") && !arguments.empty()
            && !arguments.front().TypeAndValue.TypeName.empty())
        {
            const std::string& operatorReceiver = arguments.front().TypeAndValue.TypeName;
            auto inheritedOperatorIsVisible = [&](const FunctionSymbol& candidate) {
                std::string inheritedOwner = candidate.CxxInheritedOwner;
                if (inheritedOwner.empty())
                    for (const auto& [owner, info] : cxxClasses_)
                    {
                        if (owner == operatorReceiver
                            || !IsCxxBaseOf(owner, operatorReceiver))
                            continue;
                        const bool declaresCandidate = std::any_of(
                            info.directMethods.begin(), info.directMethods.end(),
                            [&](const CxxClassInfo::Method& method) {
                                return method.raw.name == functionName
                                    && method.raw.linkageName == candidate.UniqueName;
                            });
                        if (declaresCandidate)
                        {
                            inheritedOwner = owner;
                            break;
                        }
                    }
                if (inheritedOwner.empty()) return true;
                std::unordered_set<std::string> visited;
                std::function<bool(const std::string&)> reachesOwner =
                    [&](const std::string& current) {
                    if (current == inheritedOwner) return true;
                    if (!visited.insert(current).second) return false;
                    const CxxClassInfo* info = GetCxxClassInfo(current);
                    if (info == nullptr) return false;
                    if (std::any_of(info->directMethods.begin(), info->directMethods.end(),
                                    [&](const CxxClassInfo::Method& method) {
                                        return method.raw.name == functionName;
                                    }))
                        return false;
                    for (const CxxClassInfo::BaseRef& base : info->bases)
                    {
                        if (base.access != cflat_cinterop::AccessPublic) continue;
                        const std::string baseName = ResolveCxxBaseIdentity(base);
                        if (!baseName.empty() && reachesOwner(baseName)) return true;
                    }
                    return false;
                };
                return reachesOwner(operatorReceiver);
            };
            const bool cxxOperatorReceiver = IsCxxRecord(operatorReceiver);
            for (const FunctionSymbol& candidate : *candidateSet)
            {
                if (candidate.IsCxx && candidate.IsMethod && !candidate.Parameters.empty()
                    && candidate.Parameters.front().TypeName != operatorReceiver)
                    continue;
                // A native member operator of another type is never a C++ receiver's operator; left
                // in, it still matched (list<string>::operator[]) and switched off the all-C++ ranking.
                if (cxxOperatorReceiver && !candidate.IsCxx && candidate.IsMethod
                    && !candidate.Parameters.empty()
                    && candidate.Parameters.front().TypeName != operatorReceiver)
                    continue;
                if (candidate.IsCxx && candidate.IsMethod
                    && !inheritedOperatorIsVisible(candidate))
                    continue;
                cxxOperatorCandidates.push_back(candidate);
            }
            candidateSet = &cxxOperatorCandidates;
        }
        std::vector<FunctionSymbol> defaultCandidates;
        if (!cxxDefaultWrapperRequests_.empty()
            && std::any_of(candidateSet->begin(), candidateSet->end(), [&](const auto& candidate) {
                return candidate.UniqueName.starts_with("__cflat_dflt_")
                    || (candidate.IsCxx && !candidate.DefaultArguments.empty()
                        && candidate.Parameters.size() > arguments.size());
            }))
        {
            defaultCandidates = *candidateSet;
            PrepareCxxDefaultCandidates(defaultCandidates, arguments.size());
            candidateSet = &defaultCandidates;
        }
        const auto& candidates = *candidateSet;

        std::vector<std::pair<std::vector<NamedVariable>, FunctionSymbol>> resolvedCandidate;

        for (const auto& candidate : candidates)
        {
            if (candidate.Variadic)
            {
                // Route through MatchFunction so named fixed params are reordered correctly.
                // probe=true: a LOSING candidate's named-arg mismatch must not hard-error
                // out of this loop while a later candidate might still match - see the
                // non-probed re-run below for how the diagnostic is recovered when nothing
                // scores at all.
                auto matched = MatchFunction(arguments, candidate.Parameters, true, true,
                                              &candidate.DefaultArguments);
                if (matched.size() > 0)
                {
                    resolvedCandidate.emplace_back(std::move(matched), candidate);
                    // A C++ variadic is one candidate of its overload set: its non-variadic
                    // siblings still score (ComputeOverloadFunction ranks the variadic last).
                    if (!candidate.IsCxx) break;
                }
            }
            else if (arguments.size() == 0)
            {
                auto binding = ComputeArgumentPositions({}, candidate.Parameters, false, 0,
                                                        &candidate.DefaultArguments);
                if (binding.Ok)
                {
                    // An exact wrapper for a class-typed default is needed for calls such as
                    // static methods with several C++ object defaults. Keep primitive-only
                    // nonconstant defaults on the diagnostic path.
                    if (candidate.Parameters.empty()
                        && candidate.UniqueName.starts_with("__cflat_dflt_"))
                    {
                        // With no declaration of its own in the set, the wrapper is the ONLY way
                        // to call this member: its full arity names a type cflat cannot spell
                        // (torch::optim::Optimizer::step takes a std::function), so registration
                        // bound the shorter arity alone. Keep the diagnostic path only while a
                        // real declaration is present to serve the call.
                        bool hasClassDefault = false;
                        bool haveDeclaration = false;
                        for (const auto& other : candidates)
                            if (!other.UniqueName.starts_with("__cflat_dflt_"))
                            {
                                haveDeclaration = true;
                                for (size_t i = 0; i < other.DefaultArguments.size()
                                     && i < other.Parameters.size(); ++i)
                                    if (other.DefaultArguments[i].kind == "nonconst"
                                        && dataStructures.count(other.Parameters[i].TypeName) != 0)
                                        hasClassDefault = true;
                            }
                        if (haveDeclaration && !hasClassDefault) continue;
                    }
                    resolvedCandidate.emplace_back(arguments, candidate);
                    // Native CFlat semantics: the first zero-argument-bindable candidate in
                    // registration order wins outright. Only C++ declarations and the generated
                    // default-argument wrappers need every candidate scored, because a class-typed
                    // default has to compete with its own zero-parameter wrapper.
                    if (!candidate.IsCxx
                        && !candidate.UniqueName.starts_with("__cflat_dflt_"))
                        break;
                }
            }
            else
            {
                auto matched = MatchFunction(arguments, candidate.Parameters, false, true,
                                              &candidate.DefaultArguments);
                if (matched.size() > 0)
                {
                    resolvedCandidate.emplace_back(std::move(matched), candidate);
                }
            }
        }

        std::vector<FunctionSymbol> tiedCandidates;
        CxxPreferredOverload cxxPreferred;
        auto [matched, candidate] = ComputeOverloadFunction(resolvedCandidate, &tiedCandidates,
                                                            &cxxPreferred);
        // C ABI `extern` definition with a CFlat body: the callee destroys its owning by-value
        // params, so every such param takes the argument over like a declared `move` param.
        if (candidate.External && candidate.HasCFlatBody)
            for (auto& param : candidate.Parameters)
                if (CFlatExternOwnsByValueParam(param))
                    param.IsMove = true;

        if (candidate.IsCxx && candidate.Function != nullptr && tiedCandidates.empty()
            && !cxxPreferred.set)
            if (std::string error = CheckCxxDemand(candidate.Function->getName().str());
                !error.empty())
            {
                // R4: the retry only sharpens the refusal (e.g. an rvalue sibling asking for
                // `move`); if it silently binds another overload, report the winner's body error.
                RefuseCxxDemandMember(candidate.Function->getName().str(), error);
                auto saved = std::exchange(cxxDemandRefusalRelay_,
                                           std::pair{functionName, error});
                llvm::Value* retried = CreateOverloadedFunctionCall(functionName, arguments,
                    forceRoot, displayName, cxxMemberReceiver, postfixMemberCall,
                    enclosingFunctionName);
                cxxDemandRefusalRelay_ = std::move(saved);
                // EmitError does not return: reaching here means the retry bound a sibling.
                LogError(error);
                return retried;
            }

        // A C++ reference-returning operator carries its referent address until the consumer is
        // known. For a selected by-value C++ parameter, pass the loaded class value with its
        // source address so the ordinary copy path can construct the argument.
        if (candidate.IsCxx)
            for (size_t i = 0; i < matched.size() && i < candidate.Parameters.size(); ++i)
            {
                auto& argument = matched[i];
                const auto& parameter = candidate.Parameters[i];
                if (argument.CxxRefValueType == nullptr || argument.Primary == nullptr
                    || !argument.Primary->getType()->isPointerTy()
                    || parameter.TypeName != argument.TypeAndValue.TypeName
                    || parameter.Pointer || parameter.IsAlias || parameter.IsRvalueRef)
                    continue;
                argument.Storage = argument.Primary;
                argument.BaseType = argument.CxxRefValueType;
                argument.Primary = CreateLoad(argument.CxxRefValueType, argument.Storage);
                argument.CxxRefValueType = nullptr;
                argument.TypeAndValue.IsAlias = false;
                argument.IsRvalue = false;
            }

        auto spellCandidate = [&](const FunctionSymbol& c) {
            std::string paramList;
            for (size_t i = 0; i < c.Parameters.size(); i++)
            {
                const auto& p = c.Parameters[i];
                if (i == 0 && c.IsMethod
                    && (p.VariableName.ends_with("__") || (c.IsCxx && p.VariableName == "this")))
                    continue;   // the implicit 'this'
                std::string spelled = SpellType(*this, p);
                // A free C++ function's reference parameter keeps its declared spelling.
                if (c.IsCxx && !c.IsMethod && (p.IsAlias || p.IsRvalueRef))
                    if (std::string declared = CxxReferenceParameterSpelling(c, i); !declared.empty())
                        spelled = declared;
                if (spelled.empty())
                    spelled = p.TypeName + PointerStars(p);
                // A C++ pointer-to-const parameter shows its const: it may be all that differs.
                if (c.IsCxx && p.Pointer && !p.IsAlias && !p.IsRvalueRef && !spelled.starts_with("const ")
                    && CxxReferenceParameterSpelling(c, i).starts_with("const "))
                    spelled = "const " + spelled;
                paramList += (paramList.empty() ? "" : ", ") + spelled;
            }
            const std::string name = StripCxxConstTwin(
                c.SourceName.empty() ? shownFunctionName : c.SourceName);
            return std::format("{}({})", name, paramList);
        };

        // C++ would call a candidate CFlat's call rules refuse: name it and the refused conversion.
        if (cxxPreferred.set)
        {
            LogErrorMessage("call to '{}' resolves in C++ to {}, which needs a conversion CFlat does not "
                            "make implicitly at argument {} ('{}' to '{}'); cast that argument to call it, "
                            "or cast to match {}.",
                            { shownFunctionName, spellCandidate(cxxPreferred.preferred),
                              std::to_string(cxxPreferred.argument), cxxPreferred.from, cxxPreferred.to,
                              spellCandidate(cxxPreferred.picked) });
            return nullptr;
        }

        // A tie only integer identity could have decided is ambiguous (ruling 2026-09-10).
        if (!tiedCandidates.empty())
        {
            std::string candidateList;
            std::set<std::string> spellings;
            std::set<std::string> withoutConst;
            for (const auto& c : tiedCandidates)
            {
                std::string spelled = spellCandidate(c);
                candidateList += (candidateList.empty() ? "" : ", ") + spelled;
                spellings.insert(spelled);
                for (size_t at; (at = spelled.find("const ")) != std::string::npos;)
                    spelled.erase(at, 6);
                withoutConst.insert(spelled);
            }
            // Candidates that differ only in const: CFlat has no const argument to cast to.
            if (withoutConst.size() == 1 && spellings.size() > 1)
            {
                LogErrorMessage("ambiguous call to '{}': no candidate ranks better than the others: {}. "
                                "They differ only in const, which a CFlat argument does not carry; "
                                "call it through a C++ helper that picks one.",
                                { shownFunctionName, candidateList });
                return nullptr;
            }
            LogErrorMessage("ambiguous call to '{}': no candidate ranks better than the others: {}. "
                            "Cast the argument to the parameter type you mean.",
                            { shownFunctionName, candidateList });
            return nullptr;
        }

        RejectFailedCxxDefaultCandidate(candidate, arguments.size());

        // The picked C++ candidate reaches a class parameter through a conversion operator and a
        // converting constructor that bind equally well: C++ refuses the conversion itself.
        if (candidate.IsCxx && !matched.empty())
        {
            const auto ranks = RankCxxConversionSequences(matched, candidate);
            for (size_t i = 0; i < ranks.size(); ++i)
                if (ranks[i].rank == 3 && ranks[i].userFunction.empty()
                    && ranks[i].ambiguousOperators.size() == 2)
                {
                    TypeAndValue source;
                    source.TypeName = matched[i].TypeAndValue.TypeName;
                    std::string sourceDisplay = SpellType(*this, source);
                    TypeAndValue target;   // the class itself, a const T& referent included
                    target.TypeName = candidate.Parameters[i].TypeName;
                    std::string targetDisplay = SpellType(*this, target);
                    LogErrorMessage("conversion from '{}' to '{}' is ambiguous: '{}' and '{}' bind the "
                                    "argument equally well. Call one of them explicitly.",
                                    { sourceDisplay.empty() ? source.TypeName : sourceDisplay,
                                      targetDisplay.empty() ? candidate.Parameters[i].TypeName : targetDisplay,
                                      ranks[i].ambiguousOperators[0], ranks[i].ambiguousOperators[1] });
                    return nullptr;
                }
        }

        // Blame a deleted copy at a T&& parameter only when a refused const T& sibling shows the
        // lvalue needed that copy; a lone T&& parameter keeps the rvalue-reference message.
        auto refusedCopySinkAt = [&](const std::vector<NamedVariable>& args, size_t index,
                                     std::string& cause) {
            if (cxxMemberReceiver.empty() || index == 0 || args.empty()
                || args.front().TypeAndValue.TypeName != cxxMemberReceiver)
                return false;
            const size_t dot = functionName.rfind('.');
            const std::string bare = dot == std::string::npos ? functionName
                                                              : functionName.substr(dot + 1);
            const std::vector<NamedVariable> userArgs(args.begin() + 1, args.end());
            size_t sinkIndex = 0;
            std::string sinkParam, sinkCause;
            bool refusedRvalue = false;
            // Any unbound const T& sibling counts: a bound one would have taken the lvalue.
            const bool found = FindRefusedCxxCopySink(cxxMemberReceiver, bare, userArgs,
                sinkIndex, sinkParam, sinkCause, refusedRvalue, /*requireRefusal*/ false);
            if (found && sinkIndex + 1 == index) cause = std::move(sinkCause);
            return found && sinkIndex + 1 == index;
        };
        // An unbound const T& sibling refused for its own reason: that reason explains why the
        // lvalue found only the T&& overload.
        auto refusedConstRefSibling = [&](const std::vector<NamedVariable>& args, size_t index) {
            std::string text;
            if (cxxMemberReceiver.empty() || index == 0 || index >= args.size()
                || args.front().TypeAndValue.TypeName != cxxMemberReceiver)
                return text;
            auto record = cxxRecordEntries_.find(cxxMemberReceiver);
            std::string spelling;
            if (record == cxxRecordEntries_.end()
                || !CxxSpellingForCflatType(args[index].TypeAndValue.TypeName, spelling))
                return text;
            const size_t dot = functionName.rfind('.');
            const std::string bare = dot == std::string::npos ? functionName
                                                              : functionName.substr(dot + 1);
            for (const auto& member : record->second.members)
                if (member.name == bare && !member.bindRefusal.empty()
                    && !member.refusalCause.starts_with("clang reported an error inside the body")
                    && member.paramTypes.size() == args.size()
                    && member.paramTypes[index] == "const " + spelling + " &")
                    return std::format("member '{}' of C++ class '{}' {}", bare,
                                       DisplayCxxClassName(cxxMemberReceiver), member.bindRefusal);
            return text;
        };
        if (candidate.Function == nullptr)
        {
            if (candidates.empty() && !cxxMemberReceiver.empty())
                if (auto info = cxxClasses_.find(cxxMemberReceiver); info != cxxClasses_.end())
                    if (auto refusal = info->second.refusedMembers.find(bareMemberName);
                        refusal != info->second.refusedMembers.end())
                        LogError(std::format("member '{}' of C++ class '{}' {}", bareMemberName,
                            DisplayCxxClassName(cxxMemberReceiver), refusal->second));
            for (const auto& c : candidates)
            {
                const bool arityFits = c.Variadic ? arguments.size() >= c.Parameters.size()
                                                  : arguments.size() == c.Parameters.size();
                if (!c.IsCxx || !arityFits) continue;
                for (size_t i = 0; i < arguments.size() && i < c.Parameters.size(); ++i)
                {
                    const auto& param = c.Parameters[i];
                    if (!IsCxxReferenceParameter(c, i)) continue;
                    if (!CxxReferenceArgumentMatches(param, arguments[i])) continue;
                    const bool rvalue = IsCxxRvalueReferenceArgument(arguments[i]);
                    if (param.IsRvalueRef && !rvalue && IsCopyDeletedCxxLvalue(arguments[i]))
                    {
                        const std::string sibling = refusedConstRefSibling(arguments, i);
                        std::string cause;
                        if (refusedCopySinkAt(arguments, i, cause))
                        {
                            std::string message = CxxDeletedCopyMessage(arguments[i],
                                param.VariableName, shownFunctionName, /*moveRemedy*/ true);
                            const std::string causeLine = CxxFirstDiagnosticLine(cause);
                            if (!causeLine.empty()) message += std::format(" (clang: {})", causeLine);
                            LogError(message);
                        }
                        else if (!sibling.empty())
                            LogError(sibling);
                    }
                    if (param.IsRvalueRef && !rvalue)
                        LogErrorMessage(
                            "parameter '{}' of '{}' is an rvalue reference; pass 'move <arg>' or a temporary",
                            { param.VariableName, shownFunctionName });
                    if (param.IsAlias && !param.IsRvalueRef && !param.IsCxxConstRef && rvalue)
                        LogErrorMessage(
                            "parameter '{}' of '{}' is a non-const lvalue reference and cannot bind an rvalue; pass an lvalue",
                            { param.VariableName, shownFunctionName });
                }
            }
            // MatchFunction intentionally keeps a ref-qualified candidate visible so the
            // diagnostic can distinguish a receiver-category error from an argument mismatch.
            if (!arguments.empty())
            {
                std::string receiverType = arguments.front().TypeAndValue.TypeName;
                if (receiverType.empty() && arguments.front().BaseType)
                    if (auto* receiverStruct = llvm::dyn_cast<llvm::StructType>(
                            arguments.front().BaseType))
                        receiverType = receiverStruct->getName().str();
                bool hasReceiverRefMismatch = false;
                bool hasReceiverCompatible = false;
                int rejectedRefQualifier = cflat_cinterop::CxxRefQualifierNone;
                for (const auto& c : candidates)
                {
                    if (!c.IsCxx || !c.IsMethod || c.Parameters.empty()
                        || c.Parameters[0].TypeName != receiverType)
                        continue;
                    const bool matches = c.CxxRefQualifier == cflat_cinterop::CxxRefQualifierNone
                        || (c.CxxRefQualifier == cflat_cinterop::CxxRefQualifierLValue
                            && !IsCxxRvalueReferenceArgument(arguments.front()))
                        || (c.CxxRefQualifier == cflat_cinterop::CxxRefQualifierRValue
                            && IsCxxRvalueReferenceArgument(arguments.front()));
                    if (matches) hasReceiverCompatible = true;
                    else
                    {
                        hasReceiverRefMismatch = true;
                        rejectedRefQualifier = c.CxxRefQualifier;
                    }
                }
                if (hasReceiverRefMismatch && !hasReceiverCompatible)
                {
                    const char* qualifier = rejectedRefQualifier
                        == cflat_cinterop::CxxRefQualifierLValue ? "&" : "&&";
                    const char* category = IsCxxRvalueReferenceArgument(arguments.front())
                        ? "rvalue" : "lvalue";
                    LogRawError(std::format(
                        "C++ member '{}' is {}-qualified and cannot be called on an {} receiver",
                        shownFunctionName, qualifier, category));
                    return nullptr;
                }
            }
            if (TryBindCxxImplicitArgumentConversions(functionName, arguments))
                return CreateOverloadedFunctionCall(functionName, arguments, forceRoot, displayName,
                                                    cxxMemberReceiver, postfixMemberCall,
                                                    enclosingFunctionName);
            implicitConversionRefusal = std::move(cxxImplicitConversionRefusal_);
            cxxImplicitConversionRefusal_.clear();
            for (const auto& failed : candidates)
            {
                if (failed.IsCxx) continue;
                for (size_t i = 0; i < arguments.size() && i < failed.Parameters.size(); ++i)
                {
                    const auto& arg = arguments[i];
                    const auto& param = failed.Parameters[i];
                    if (!param.IsArrayView || param.IsMove || param.IsUnique
                        || param.IsOwningSink || !IsCxxRecord(arg.TypeAndValue.TypeName))
                        continue;
                    const std::string element = SpellType(*this,
                        TypeAndValue{ .TypeName = param.TypeName });
                    const std::string container = SpellType(*this, arg.TypeAndValue);
                    LogError(std::format(
                        "cannot pass C++ class '{}' to array-view parameter '{}' of '{}': "
                        "contiguous view decay requires public data() and size(); data() must "
                        "return exactly '{}*' or 'const {}*'",
                        container, param.VariableName, shownFunctionName, element, element));
                }
            }
            /*
             * `obj.name(...)` on a C++ class that has no member `name` at all. Every candidate here
             * came from CFlat's own overload table by name only - core's atomic<T> load/store were
             * printed for std.atomic<int> - so report the C++ class's member set instead. A real
             * UFCS/extension call is exempt: one of its candidates takes the receiver type itself.
             */
            const size_t receiverMemberDot = functionName.rfind('.');
            const std::string bareMemberName = receiverMemberDot == std::string::npos
                ? functionName : functionName.substr(receiverMemberDot + 1);
            // A bound overload hides a refused sibling whose signature type no request had
            // registered yet (set::insert returning pair<iterator, bool>). Retry that bind once.
            // An operator call (`d[1]`, `a + b`) names no receiver, yet a member operator of the
            // left operand's C++ class may have been refused the same way; retry that bind too.
            std::string retryReceiver = cxxMemberReceiver;
            if (retryReceiver.empty() && bareMemberName.starts_with("operator")
                && !arguments.empty() && !arguments.front().TypeAndValue.ElemPointer)
                retryReceiver = arguments.front().TypeAndValue.TypeName;
            if (!retryReceiver.empty() && GetCxxClassInfo(retryReceiver) != nullptr)
            {
                const std::string retryKey = retryReceiver + "." + bareMemberName;
                if (cxxOverloadRebindInFlight_.insert(retryKey).second)
                {
                    llvm::Value* retried = nullptr;
                    const bool rebound = TryBindRefusedCxxMember(retryReceiver, bareMemberName);
                    if (rebound)
                        retried = CreateOverloadedFunctionCall(functionName, arguments, forceRoot,
                                                               displayName, cxxMemberReceiver,
                                                               postfixMemberCall,
                                                               enclosingFunctionName);
                    cxxOverloadRebindInFlight_.erase(retryKey);
                    if (rebound) return retried;
                }
            }
            if (!cxxMemberReceiver.empty() && GetCxxClassInfo(cxxMemberReceiver) != nullptr
                && !CxxClassHasMemberNamed(cxxMemberReceiver, bareMemberName)
                && std::none_of(candidates.begin(), candidates.end(), [&](const auto& c) {
                       return !c.Parameters.empty()
                           && c.Parameters[0].TypeName == cxxMemberReceiver;
                   }))
            {
                std::set<std::string> names, visited;
                CollectCxxMemberNames(cxxMemberReceiver, names, visited);
                // A library's own reserved names (libc++ writes 27 of them on vector) drown the
                // list; keep them out unless the call itself asked for one.
                const bool wantsReserved = bareMemberName.starts_with("__");
                std::string bound;
                size_t hidden = 0;
                for (const auto& name : names)
                {
                    if (name == "__ctor" || name == "__dtor") continue;
                    if (!wantsReserved && name.starts_with("__")) { ++hidden; continue; }
                    bound += (bound.empty() ? "" : ", ") + name;
                }
                if (bound.empty()) bound = "none";
                if (hidden != 0)
                    bound += std::format(" (plus {} reserved name(s) starting with '__')", hidden);
                TypeAndValue receiverType;
                receiverType.TypeName = cxxMemberReceiver;
                const std::string shownReceiver = DisplayCxxClassName(cxxMemberReceiver);
                LogRawError(std::format("C++ class '{}' has no member '{}'.\n  Members of '{}': {}",
                                        shownReceiver, bareMemberName, shownReceiver, bound));
                return nullptr;
            }
            for (const auto& c : candidates)
            {
                if (c.IsCxx || c.Parameters.size() != arguments.size()) continue;
                if (!std::all_of(arguments.begin(), arguments.end(), [](const auto& arg) {
                        return arg.TypeAndValue.VariableName.empty();
                    }))
                    continue;
                for (size_t i = 0; i < arguments.size(); ++i)
                    if (IsImplicitPrimitiveToPointer(c.Parameters[i], arguments[i], arguments[i].Primary))
                    {
                        LogError(DescribeImplicitPrimitiveToPointer(
                            c.Parameters[i], arguments[i], arguments[i].Primary, "pass",
                            std::format("parameter '{}' of '{}'", c.Parameters[i].VariableName,
                                        shownFunctionName)));
                        return nullptr;
                    }
            }

            // A copy-deleted lvalue whose const T& overload clang refused to instantiate: the
            // call needed the copy. Offer 'move x' only when an rvalue overload is bound.
            if (!cxxMemberReceiver.empty() && !arguments.empty()
                && arguments.front().TypeAndValue.TypeName == cxxMemberReceiver)
            {
                const std::vector<NamedVariable> userArgs(arguments.begin() + 1, arguments.end());
                size_t sinkIndex = 0;
                std::string sinkParam, sinkCause;
                bool refusedRvalue = false;
                if (FindRefusedCxxCopySink(cxxMemberReceiver, bareMemberName, userArgs, sinkIndex,
                                           sinkParam, sinkCause, refusedRvalue))
                {
                    // Suggest 'move x' only when a bound T&& or by-value overload takes it.
                    const std::string& sinkType = userArgs[sinkIndex].TypeAndValue.TypeName;
                    bool moveRemedy = std::any_of(candidates.begin(), candidates.end(),
                        [&](const auto& c) {
                            if (!c.IsCxx || c.Parameters.size() != arguments.size()) return false;
                            const auto& p = c.Parameters[sinkIndex + 1];
                            return p.IsRvalueRef
                                || (!p.IsAlias && !p.IsCxxConstRef && !p.Pointer
                                    && p.TypeName == sinkType);
                        });
                    if (!moveRemedy)
                    {
                        std::string spelling;
                        if (CxxSpellingForCflatType(sinkType, spelling))
                            if (auto record = cxxRecordEntries_.find(cxxMemberReceiver);
                                record != cxxRecordEntries_.end())
                                moveRemedy = std::any_of(record->second.members.begin(),
                                    record->second.members.end(), [&](const auto& member) {
                                        return member.name == bareMemberName && member.bindRefusal.empty()
                                            && !member.linkageName.empty()
                                            && member.paramTypes.size() == arguments.size()
                                            && member.paramTypes[sinkIndex + 1] == spelling;
                                    });
                    }
                    std::string message = CxxDeletedCopyMessage(userArgs[sinkIndex], sinkParam,
                                                                shownFunctionName, moveRemedy);
                    const std::string causeLine = CxxFirstDiagnosticLine(sinkCause);
                    if (!causeLine.empty()) message += std::format(" (clang: {})", causeLine);
                    LogError(message);
                    return nullptr;
                }
            }

            // A C++ candidate C++ accepts through an AMBIGUOUS conversion operator: spell that
            // ambiguity instead of a bare mismatch (clang refuses the call for the same reason).
            if (!resolvedCandidate.empty()
                && std::all_of(resolvedCandidate.begin(), resolvedCandidate.end(), [](const auto& c) {
                       return c.second.IsCxx && !c.second.Variadic; }))
                for (const auto& [ambiguousArgs, ambiguousSym] : resolvedCandidate)
                {
                    const auto ranks = RankCxxConversionSequences(ambiguousArgs, ambiguousSym);
                    if (!std::all_of(ranks.begin(), ranks.end(),
                                     [](const CxxConversionRank& r) { return r.cxxViable; }))
                        continue;
                    for (size_t i = 0; i < ranks.size(); ++i)
                        if (ranks[i].rank == 3 && ranks[i].userFunction.empty()
                            && ranks[i].ambiguousOperators.size() >= 2)
                        {
                            TypeAndValue dest;
                            dest.TypeName = ambiguousSym.Parameters[i].TypeName;
                            ReportAmbiguousCxxConversion(ambiguousArgs[i].TypeAndValue.TypeName, dest,
                                                         ranks[i].ambiguousOperators);
                            return nullptr;
                        }
                }

            // The best candidate's body failed and no sibling matches: relay clang's text.
            if (!cxxDemandRefusalRelay_.second.empty()
                && cxxDemandRefusalRelay_.first == functionNameIn)
            {
                LogError(cxxDemandRefusalRelay_.second);
                return nullptr;
            }
            if (candidates.size() == 1 && resolvedCandidate.size() == 1
                && !resolvedCandidate.front().second.IsCxx)
            {
                const auto& [resolvedArgs, resolvedSym] = resolvedCandidate.front();
                for (size_t i = 0; i < resolvedArgs.size()
                     && i < resolvedSym.Parameters.size(); ++i)
                {
                    const auto& arg = resolvedArgs[i];
                    const auto& from = arg.HasOriginalArgumentType
                        ? arg.OriginalArgumentType : arg.TypeAndValue;
                    if (IsImplicitIntegerPointeePointerConversion(
                            from, resolvedSym.Parameters[i]))
                        RejectImplicitIntegerPointeePointerConversion(
                            from, resolvedSym.Parameters[i]);
                }
            }
            std::string msg = std::format("no overload of '{}' matches the given arguments.\n", shownFunctionName);

            // Recover a named-argument diagnostic only from candidates whose parameter names
            // actually bind. A losing candidate with different names must not blame the call for
            // a name miss when another candidate owns those names and failed for its types.
            std::vector<std::string> argumentNames;
            argumentNames.reserve(arguments.size());
            for (const auto& arg : arguments)
                argumentNames.push_back(arg.TypeAndValue.VariableName);
            bool replayedNameMatch = false;
            for (const auto& c : candidates)
            {
                auto binding = ComputeArgumentPositions(argumentNames, c.Parameters, c.Variadic);
                if (!binding.Ok) continue;
                replayedNameMatch = true;
                if (c.Variadic)
                    MatchFunction(arguments, c.Parameters, true, false);
                else if (!(arguments.size() == 0 && c.Parameters.size() == 0))
                    MatchFunction(arguments, c.Parameters, false, false);
            }
            // If every candidate missed the names, preserve the specific unknown/duplicate-name
            // diagnostic by replaying the first candidate once.
            if (!replayedNameMatch && !candidates.empty())
            {
                const auto& c = candidates.front();
                if (c.Variadic)
                    MatchFunction(arguments, c.Parameters, true, false);
                else if (!(arguments.size() == 0 && c.Parameters.size() == 0))
                    MatchFunction(arguments, c.Parameters, false, false);
            }

            // Call arguments
            msg += std::format("  Call arguments ({}):\n", arguments.size());
            auto displayDiagnosticType = [&](const std::string& typeName) {
                if (typeName.empty()) return typeName;
                return SpellType(*this, TypeAndValue{ .TypeName = typeName });
            };
            auto displayParameterType = [&](const TypeAndValue& parameter) {
                std::string result = SpellType(*this, parameter);
                if (result.empty())
                    result = displayDiagnosticType(parameter.TypeName) + PointerStars(parameter);
                return result;
            };
            for (size_t i = 0; i < arguments.size(); i++)
            {
                const auto& arg = arguments[i];
                std::string typeName = arg.TypeAndValue.TypeName;
                // A string literal argument carries no CFlat type name. Spell the 'char*' it
                // actually is instead of the lowered 'ptr' its machine type prints as.
                auto* argConstant = llvm::dyn_cast_or_null<llvm::Constant>(arg.Primary);
                if (typeName.empty()
                    && (arg.IsStringLiteral
                        || (argConstant != nullptr && IsStringLiteralConstant(argConstant))))
                {
                    msg += std::format("    [{}] char* {}\n", i,
                        arg.TypeAndValue.VariableName.empty()
                            ? std::string("<unnamed>") : arg.TypeAndValue.VariableName);
                    continue;
                }
                if (typeName.empty() && arg.BaseType)
                {
                    std::string typeStr;
                    llvm::raw_string_ostream rso(typeStr);
                    arg.BaseType->print(rso);
                    typeName = typeStr;
                }
                typeName = displayDiagnosticType(typeName);
                std::string name;
                if (arg.TypeAndValue.VariableName.empty())
                {
                    bool isThis = i == 0 && !candidates.empty() &&
                                  !candidates[0].Parameters.empty() &&
                                  (candidates[0].Parameters[0].VariableName.ends_with("__")
                                   || (candidates[0].IsCxx && candidates[0].IsMethod
                                       && candidates[0].Parameters[0].VariableName == "this"));
                    name = isThis ? "<this>" : "<unnamed>";
                }
                else
                {
                    name = arg.TypeAndValue.VariableName;
                }
                msg += std::format("    [{}] {}{} {}\n", i, typeName, PointerStars(arg.TypeAndValue), name);
            }

            /*
             * A member call's candidate list is keyed by NAME alone, so an unrelated type's
             * member of the same name rides along (a std.vector<int> receiver listing CFlat
             * core `list<string>::insert`). Keep only the receiver's own members - never to
             * the point of an empty list, so a genuine free/other-type candidate still shows.
             */
            std::vector<const FunctionSymbol*> shownCandidates;
            for (const auto& c : candidates) shownCandidates.push_back(&c);
            if (!arguments.empty() && !candidates.empty()
                && !candidates.front().Parameters.empty()
                && candidates.front().Parameters.front().VariableName.ends_with("__"))
            {
                std::string receiverName = arguments.front().TypeAndValue.TypeName;
                if (receiverName.empty() && arguments.front().BaseType)
                    if (auto* st = llvm::dyn_cast<llvm::StructType>(arguments.front().BaseType))
                        receiverName = st->getName().str();
                if (!receiverName.empty())
                {
                    std::vector<const FunctionSymbol*> ownMembers;
                    for (const FunctionSymbol* c : shownCandidates)
                        if (!c->Parameters.empty()
                            && c->Parameters.front().TypeName == receiverName)
                            ownMembers.push_back(c);
                    if (!ownMembers.empty()) shownCandidates = std::move(ownMembers);
                }
            }

            // Candidates
            msg += std::format("  Candidates ({}):\n", shownCandidates.size());
            for (const FunctionSymbol* candidatePtr : shownCandidates)
            {
                const auto& c = *candidatePtr;
                std::string paramList;
                for (size_t i = 0; i < c.Parameters.size(); i++)
                {
                    if (i > 0) paramList += ", ";
                    const auto& p = c.Parameters[i];
                    std::string parameterType = displayParameterType(p);
                    if (p.IsMove) paramList += "move ";
                    paramList += parameterType;
                }
                const std::string candidateName = StripCxxConstTwin(
                    c.SourceName.empty() ? shownFunctionName : c.SourceName);
                msg += std::format("    {}({})\n", candidateName, paramList);
            }

            // If exactly one resolved candidate passed MatchFunction, show per-argument type comparison
            if (resolvedCandidate.size() == 1)
            {
                const auto& [resolvedArgs, resolvedSym] = resolvedCandidate.front();
                // A C++ candidate's UniqueName is its mangled linkage name (_ZN4cppi8pick_refERi),
                // which names nothing the user wrote. The registered lookup name is the dotted
                // spelling (cppi.pick_ref), so prefer it whenever it exists.
                std::string resolvedShown = StripCxxConstTwin(!resolvedSym.SourceName.empty()
                    ? resolvedSym.SourceName
                    : SpellFunctionSymbol(*this, resolvedSym.UniqueName));
                msg += std::format("  Argument mismatch detail (single resolved candidate: {}):\n",
                    displayName.empty() ? resolvedShown : shownFunctionName);
                size_t count = std::max(resolvedArgs.size(), resolvedSym.Parameters.size());
                for (size_t i = 0; i < count; i++)
                {
                    std::string argDesc = i < resolvedArgs.size() ? resolvedArgs[i].TypeAndValue.TypeName : "<missing>";
                    if (argDesc.empty() && i < resolvedArgs.size() && resolvedArgs[i].BaseType)
                    {
                        std::string typeStr;
                        llvm::raw_string_ostream rso(typeStr);
                        resolvedArgs[i].BaseType->print(rso);
                        argDesc = typeStr;
                    }
                    argDesc = displayDiagnosticType(argDesc);
                    std::string argPtr = i < resolvedArgs.size() ? PointerStars(resolvedArgs[i].TypeAndValue) : "";
                    std::string paramDesc = i < resolvedSym.Parameters.size()
                        ? displayDiagnosticType(resolvedSym.Parameters[i].TypeName) : "<missing>";
                    std::string paramPtr = i < resolvedSym.Parameters.size() ? PointerStars(resolvedSym.Parameters[i]) : "";
                    msg += std::format("    [{}] arg={}{}  param={}{}\n", i, argDesc, argPtr, paramDesc, paramPtr);
                }
            }

            /*
             * Name the mechanism when a C++ candidate was dropped because the one implicit
             * user-defined conversion its parameter needs runs through an `explicit`
             * constructor. The dump above prints only `arg=int param=cppexc.Ex`.
             */
            for (const auto& c : candidates)
            {
                auto pi = c.Parameters.begin();
                bool named = false;
                for (size_t i = 0; i < arguments.size() && pi != c.Parameters.end(); i++, ++pi)
                {
                    // The implicit object argument of a member call never takes a user-defined
                    // conversion, exactly as in the ranking loop.
                    if (c.IsCxx && c.IsMethod && i == 0) continue;
                    const bool byValueParam = c.IsCxx && i < c.Recipe.paramSlots.size()
                        && c.Recipe.paramSlots[i].kind == AbiSlot::ByVal;
                    const bool indirectValueParam = c.IsCxx && c.CxxAbi.valid
                        && i < c.CxxAbi.params.size()
                        && c.CxxAbi.params[i].kind == cflat_cinterop::RawAbiSlot::Indirect;
                    std::string note = DescribeCxxImplicitArgumentBlock(
                        arguments[i], *pi, byValueParam || indirectValueParam);
                    if (note.empty()) continue;
                    msg += std::format("  [{}] argument {}: {}\n",
                        StripCxxConstTwin(c.SourceName.empty() ? shownFunctionName : c.SourceName),
                        i, note);
                    named = true;
                    break;
                }
                if (named) break;
            }

            // A C++ implicit-conversion wrapper this call attempted and clang or registration refused.
            if (!implicitConversionRefusal.empty())
                msg += std::format("  implicit argument conversion refused: {}\n",
                                   implicitConversionRefusal);

            // Name the mechanism when a candidate was dropped for a function-pointer SIGNATURE:
            // the dump above prints only `arg=ptr param=__c_fn_ptr`, which points at no cause.
            for (const auto& c : candidates)
            {
                auto pi = c.Parameters.begin();
                for (size_t i = 0; i < arguments.size() && pi != c.Parameters.end(); i++, ++pi)
                {
                    if (!pi->IsFunctionPointer) continue;
                    std::string why = DescribeFuncPtrSignatureMismatch(arguments[i].TypeAndValue, *pi);
                    if (why.empty() && c.IsCxx
                        && NamedFunctionArgMismatches(arguments[i], *pi))
                    {
                        auto* function = llvm::dyn_cast_or_null<llvm::Function>(arguments[i].Primary);
                        const TypeAndValue actual = FuncPtrSigOfBoundFunction(
                            arguments[i].CallerName, function);
                        if (actual.IsFunctionPointer)
                            why = std::format("function-pointer signature mismatch: parameter takes '{}' "
                                "but function '{}' has '{}'", FuncPtrSpellingOf(*pi),
                                arguments[i].CallerName, FuncPtrSpellingOf(actual));
                    }
                    if (why.empty()) continue;
                    msg += std::format("  [{}] {}\n",
                        StripCxxConstTwin(c.SourceName.empty()
                            ? SpellFunctionSymbol(*this, c.UniqueName) : c.SourceName),
                        why);
                    break;
                }
            }

            /*
             * Same service for the parameters a refuted funcptr candidate used to rebind onto:
             * opaque pointers make the dump above print two indistinguishable `ptr`s. Conditioned to
             * fire ONLY where the code-value gate is what refused, and the gate's question differs
             * per arm, so this condition is per-arm too. The NON-VARIADIC sites judge only an
             * argument in their own empty-TypeName shape (without that it misdirects: an arity
             * mismatch or a `__closure_fat_ptr` argument, which the non-empty-TypeName branch
             * rejects and where no such gate exists, would claim the line) and only a data
             * parameter. The VARIADIC gate calls ArgumentIsCodeValue unconditionally against every
             * declared parameter, so both requirements are dropped for a variadic candidate - a fat
             * `Lambda<T>` value into `lam(Rec*, ...)` is refused there and otherwise got no line.
             */
            for (const auto& c : candidates)
            {
                bool arityFits = c.Variadic ? arguments.size() >= c.Parameters.size()
                                            : arguments.size() == c.Parameters.size();
                if (!arityFits) continue;
                auto pi = c.Parameters.begin();
                for (size_t i = 0; i < arguments.size() && pi != c.Parameters.end(); i++, ++pi)
                {
                    if (!c.Variadic && !arguments[i].TypeAndValue.TypeName.empty()) continue;
                    if (!ArgumentIsCodeValue(arguments[i], arguments[i].CastOccurrenceId)) continue;
                    bool refused = c.Variadic ? !ParameterAcceptsCodeValue(*pi) : ParameterStoresData(*pi);
                    if (!refused) continue;
                    // Per-shape wording: "data type" is false at the scalar cell the variadic arm
                    // also judges, and a rejection's message must be true where it fires.
                    if (ParameterStoresData(*pi))
                        msg += std::format("  [{}] parameter {} is a data type ('{}') and the argument is "
                            "a function-pointer or closure VALUE - code does not convert to a data "
                            "pointer.\n", SpellFunctionSymbol(*this, c.UniqueName), i,
                            displayParameterType(*pi));
                    else
                        msg += std::format("  [{}] parameter {} has type '{}' and the argument is a "
                            "function-pointer or closure VALUE - code does not convert to a "
                            "non-pointer type.\n", SpellFunctionSymbol(*this, c.UniqueName), i,
                            displayParameterType(*pi));
                    break;
                }
            }

            /*
             * Name the mechanism when a candidate was dropped for pointer DEPTH. Fires only where
             * the depth gate is what refused - both sides proven, which is the same condition
             * TypeAndValue::IsTypeMatch tests - so the wording is true wherever it appears.
             */
            for (const auto& c : candidates)
            {
                bool arityFits = c.Variadic ? arguments.size() >= c.Parameters.size()
                                            : arguments.size() == c.Parameters.size();
                if (!arityFits) continue;
                auto pi = c.Parameters.begin();
                for (size_t i = 0; i < arguments.size() && pi != c.Parameters.end(); i++, ++pi)
                {
                    bool tooDeep = (arguments[i].TypeAndValue.IsProvenDoublePointer()
                                 || arguments[i].TypeAndValue.IsProvenDecayedDoublePointer())
                                && pi->IsProvenSinglePointer();
                    bool tooShallow = arguments[i].TypeAndValue.IsProvenSinglePointerDepth()
                                   && pi->IsProvenDoublePointer();
                    if (!tooDeep && !tooShallow) continue;
                    // A mangled instantiation name is not writable source, so the advice clause is
                    // dropped rather than naming a type the user cannot spell.
                    TypeAndValue parameterBase = *pi;
                    parameterBase.Pointer = false;
                    parameterBase.ElemPointer = false;
                    parameterBase.PointerDepth = 0;
                    parameterBase.IsArrayView = false;
                    std::string shown = SpellType(*this, parameterBase);
                    std::string argShown = SpellType(*this, arguments[i].TypeAndValue);
                    const bool writable = !shown.empty();
                    // A PRIMITIVE pointer argument carries no CFlat TypeName at all, so the depth
                    // is all that is known about it - say only that, never "type '**'".
                    auto argType = [&](const char* stars, const char* unnamed) {
                        return argShown.empty() ? std::string(unnamed)
                                                : std::format("type '{}{}'", argShown, stars);
                    };
                    if (tooDeep)
                    {
                        std::string advice = writable
                            ? std::format(", or declare the parameter as '{}**'", shown) : std::string();
                        // A `T*[N]` argument decays to the element-0 ADDRESS, so the type the
                        // callee receives is `T**`; say so rather than naming the array.
                        std::string arrayElementShown = argShown;
                        if (arrayElementShown.ends_with("*"))
                            arrayElementShown.pop_back();
                        std::string how = arguments[i].TypeAndValue.IsProvenDecayedDoublePointer()
                            && !arrayElementShown.empty()
                            ? std::format(" (a '{}*[{}]' array decays to '{}**')", arrayElementShown,
                                arguments[i].TypeAndValue.ConstArraySize, arrayElementShown)
                            : std::string();
                        msg += std::format("  [{}] parameter {} '{}' has type '{}*' and the argument has "
                            "{}{} - there is no implicit dereference. Dereference it with '*' at "
                            "the call site{}.\n",
                            SpellFunctionSymbol(*this, c.UniqueName), i, pi->VariableName, shown,
                            argType("", "one more level of indirection"), how, advice);
                    }
                    else
                    {
                        std::string advice = writable
                            ? std::format(", or declare the parameter as '{}*'", shown) : std::string();
                        msg += std::format("  [{}] parameter {} '{}' has type '{}**' and the argument has "
                            "{} - there is no implicit address-of. Take its address with '&' at "
                            "the call site{}.\n",
                            SpellFunctionSymbol(*this, c.UniqueName), i, pi->VariableName, shown,
                            argType("", "one fewer level of indirection"), advice);
                    }
                    break;
                }
            }

            for (const auto& c : candidates)
            {
                const bool arityFits = c.Variadic ? arguments.size() >= c.Parameters.size()
                                                  : arguments.size() == c.Parameters.size();
                if (!c.IsCxx || !arityFits) continue;
                for (size_t i = 0; i < arguments.size() && i < c.Parameters.size(); ++i)
                {
                    const auto& arg = arguments[i].TypeAndValue;
                    if (CxxEnumParameterRefusesArgument(arguments[i], arg, c.Parameters[i]))
                    {
                        TypeAndValue shownEnum = c.Parameters[i];
                        shownEnum.Pointer = shownEnum.IsAlias = shownEnum.IsRvalueRef = false;
                        msg += std::format(
                            "  argument {} is not an enum; C++ has no implicit conversion to enum "
                            "'{}' - cast it explicitly.\n",
                            i, displayParameterType(shownEnum));
                        break;
                    }
                    if ((!arg.IsScopedEnum && !IsScopedEnumTypeName(arg.TypeName))
                        || IsScopedEnumMatch(arg, c.Parameters[i])) continue;
                    msg += std::format(
                        "  argument {} is scoped C++ enum '{}' and cannot be implicitly converted to '{}'.\n",
                        i, SpellType(*this, arg), displayParameterType(c.Parameters[i]));
                    break;
                }
            }

            if (resolvedCandidate.size() == 1)
            {
                const auto& [rvalueArgs, rvalueSym] = resolvedCandidate.front();
                for (size_t i = 0; i < rvalueArgs.size() && i < rvalueSym.Parameters.size(); ++i)
                {
                    if (!rvalueSym.Parameters[i].IsRvalueRef
                        || (rvalueSym.IsCxx ? IsCxxRvalueReferenceArgument(rvalueArgs[i])
                                            : IsRvalueReferenceArgument(rvalueArgs[i])))
                        continue;
                    if (rvalueSym.IsCxx && IsCopyDeletedCxxLvalue(rvalueArgs[i]))
                    {
                        const std::string sibling = refusedConstRefSibling(rvalueArgs, i);
                        std::string cause;
                        if (refusedCopySinkAt(rvalueArgs, i, cause))
                        {
                            std::string message = CxxDeletedCopyMessage(rvalueArgs[i],
                                rvalueSym.Parameters[i].VariableName, shownFunctionName,
                                /*moveRemedy*/ true);
                            const std::string causeLine = CxxFirstDiagnosticLine(cause);
                            if (!causeLine.empty()) message += std::format(" (clang: {})", causeLine);
                            LogError(message);
                        }
                        else if (!sibling.empty())
                            LogError(sibling);
                    }
                    LogErrorMessage(
                        "parameter '{}' of '{}' is an rvalue reference; pass 'move <arg>' or a temporary",
                        { rvalueSym.Parameters[i].VariableName, shownFunctionName });
                }
            }

            LogRawError(msg);
            return nullptr;
        }

        std::string diagnosticFunctionName = displayName.empty()
            ? SpellFunctionSymbol(*this, functionName) : displayName;
        if (displayName.empty() && diagnosticFunctionName == functionName)
            diagnosticFunctionName = SpellType(*this, TypeAndValue{ .TypeName = functionName });

        if (!candidate.Variadic && matched.size() < candidate.Parameters.size())
        {
            for (size_t i = matched.size(); i < candidate.Parameters.size(); ++i)
            {
                const auto& def = i < candidate.DefaultArguments.size()
                    ? candidate.DefaultArguments[i] : cflat_cinterop::RawDefaultArg{};
                if (def.kind == "nonconst" || def.kind.empty())
                    LogErrorMessage(
                        "call to '{}' omits parameter '{}' whose default argument is not a constant expression; pass it explicitly",
                        { diagnosticFunctionName, candidate.Parameters[i].VariableName });
                NamedVariable value;
                value.TypeAndValue = candidate.Parameters[i];
                value.TypeAndValue.VariableName = candidate.Parameters[i].VariableName;
                value.BaseType = GetType(candidate.Parameters[i]);
                if (value.BaseType == nullptr)
                    LogErrorMessage("cannot lower default argument for parameter '{}' of '{}'",
                                    { candidate.Parameters[i].VariableName, diagnosticFunctionName });
                if (def.kind == "nullptr")
                {
                    if (!value.BaseType->isPointerTy())
                        LogErrorMessage("default nullptr for parameter '{}' of '{}' is not a pointer",
                                        { candidate.Parameters[i].VariableName, diagnosticFunctionName });
                    value.Primary = llvm::ConstantPointerNull::get(
                        llvm::cast<llvm::PointerType>(value.BaseType));
                }
                else if (value.BaseType->isIntegerTy(1) && def.kind == "bool")
                {
                    value.Primary = llvm::ConstantInt::get(value.BaseType, def.value != "0");
                }
                else if (value.BaseType->isIntegerTy())
                {
                    llvm::APInt folded(value.BaseType->getIntegerBitWidth(), def.value, 10);
                    value.Primary = llvm::ConstantInt::get(*context, folded);
                }
                else if (value.BaseType->isFloatingPointTy())
                {
                    value.Primary = llvm::ConstantFP::get(value.BaseType, std::stod(def.value));
                }
                else
                    LogErrorMessage("default argument for parameter '{}' of '{}' has an unsupported type",
                                    { candidate.Parameters[i].VariableName, diagnosticFunctionName });
                value.IsRvalue = true;
                matched.push_back(std::move(value));
            }
        }

        // Materialize a borrowed pointer view only after this CFlat overload has won.
        for (size_t i = 0; i < matched.size() && i < candidate.Parameters.size(); ++i)
        {
            const auto& param = candidate.Parameters[i];
            auto& arg = matched[i];
            if (candidate.IsCxx || !param.IsArrayView || param.IsMove || param.IsUnique
                || param.IsOwningSink || !IsCxxContiguousViewSource(arg, param))
                continue;

            const std::string receiverType = arg.TypeAndValue.TypeName;
            bool useConstData = false;
            IsCxxContiguousViewSource(arg, param, &useConstData);
            // The const twin registers under a synthetic name only when 'data' is projected.
            if (IsCxxRecord(receiverType)) EnsureCxxMemberProjected(receiverType, "data");
            llvm::Value* count = CreateOverloadedFunctionCall(
                "size", { arg }, false, "size", receiverType, true, enclosingFunctionName);
            llvm::Value* data = CreateOverloadedFunctionCall(
                useConstData ? "__cflat_view_decay_const_data" : "data",
                { arg }, false, "data", useConstData ? "" : receiverType,
                !useConstData, enclosingFunctionName);
            if (count == nullptr || data == nullptr || !data->getType()->isPointerTy())
                LogError(std::format("cannot materialize contiguous view for parameter '{}' of '{}'",
                                     param.VariableName, diagnosticFunctionName));

            auto* countType = llvm::dyn_cast<llvm::IntegerType>(count->getType());
            if (countType == nullptr)
                LogError(std::format("size() for contiguous view parameter '{}' of '{}' must return an integer",
                                     param.VariableName, diagnosticFunctionName));
            llvm::Type* lengthType = llvm::Type::getInt64Ty(*context);
            if (countType->getBitWidth() < 64)
                count = builder->CreateZExt(count, lengthType, "view_size");
            else if (countType->getBitWidth() > 64)
                count = builder->CreateTrunc(count, lengthType, "view_size");

            arg.Primary = data;
            arg.Storage = nullptr;
            arg.BaseType = data->getType();
            arg.TypeAndValue = param;
            arg.IsOwning = false;
            arg.IsOwningStruct = false;
            arg.IsOwningString = false;
            arg.IsExplicitMove = false;
            arg.IsRvalue = true;
            arg.RawArrayLength = count;
        }

        // A scalar can bind to a C++ class reference through an implicit converting constructor.
        // Materialize those temporaries only after overload selection so the selected constructor
        // and the selected function agree on the same C++ conversion.
        for (size_t i = 0; i < matched.size() && i < candidate.Parameters.size(); ++i)
        {
            const auto& param = candidate.Parameters[i];
            auto& arg = matched[i];
            if (!param.Pointer || param.IsAlias || param.IsCxxRefToPointer || param.IsCxxConstRef
                || param.IsRvalueRef || arg.TypeAndValue.Pointer
                || !IsCxxRecord(arg.TypeAndValue.TypeName))
                continue;
            if (auto* referent = CxxReferenceResultAsPointer(
                    param, arg, std::format("parameter '{}' of '{}'", param.VariableName,
                                            diagnosticFunctionName)))
            {
                arg.Primary = referent;
                arg.Storage = nullptr;
                arg.BaseType = referent->getType();
                arg.TypeAndValue = param;
                arg.IsRvalue = false;
            }
        }
        for (size_t i = 0; i < matched.size() && i < candidate.Parameters.size(); ++i)
        {
            const bool cxxByValueParam = candidate.IsCxx
                && i < candidate.Recipe.paramSlots.size()
                && candidate.Recipe.paramSlots[i].kind == AbiSlot::ByVal;
            const bool cxxIndirectValueParam = candidate.IsCxx
                && candidate.CxxAbi.valid
                && i < candidate.CxxAbi.params.size()
                && candidate.CxxAbi.params[i].kind == cflat_cinterop::RawAbiSlot::Indirect;
            if (candidate.IsCxx && candidate.IsMethod && i == 0) continue;  // receiver: no UDC
            const auto& param = candidate.Parameters[i];
            const auto& arg = matched[i];
            if (param.Pointer && !param.IsAlias && !param.IsCxxRefToPointer && !param.IsCxxConstRef
                && !param.IsRvalueRef && !arg.TypeAndValue.Pointer
                && IsCxxRecord(param.TypeName) && IsCxxRecord(arg.TypeAndValue.TypeName))
                continue;
            if (!CanImplicitlyConstructCxxClass(matched[i], candidate.Parameters[i],
                                                cxxByValueParam || cxxIndirectValueParam))
            {
                // Conversion-OPERATOR direction, selected by the same rank above.
                if (!matched[i].TypeAndValue.Pointer
                    && !CxxConversionOperatorTo(matched[i].TypeAndValue.TypeName,
                                                candidate.Parameters[i], false).empty()
                    && !ApplyCxxConversionOperator(matched[i], candidate.Parameters[i], false))
                    LogErrorMessage("cannot materialize implicit C++ class argument for '{}'",
                                    { diagnosticFunctionName });
                continue;
            }
            if (!MaterializeImplicitCxxClassArgument(matched[i], candidate.Parameters[i]))
                LogErrorMessage("cannot materialize implicit C++ class argument for '{}'",
                                { diagnosticFunctionName });
        }

        // convert parameter to vector of llvm::value*
        for (size_t i = 0; i < arguments.size() && i < candidate.Parameters.size(); ++i)
        {
            const auto& param = candidate.Parameters[i];
            const auto& source = arguments[i];
            // An alias-return result may name a borrow or a temp at runtime, so an owning sink
            // cannot determine whether it owns the selected value.
            std::string aliasCallee;
            const std::string sourceBinding = !source.TypeAndValue.VariableName.empty()
                ? source.TypeAndValue.VariableName : source.CallerName;
            const auto* binding = FindLiveNamedVariable(sourceBinding);
            bool sourceIsAlias = false;
            if (binding != nullptr)
                sourceIsAlias = binding->IsAliasBorrow && binding->IsAliasReturnBorrow;
            else
                sourceIsAlias = (source.IsAliasBorrow && source.IsAliasReturnBorrow)
                    || IsAliasReturnResult(source.Primary, &aliasCallee);
            if (functionName != "operator=" && IsOwningValueType(param.TypeName)
                && (OwningSinkConsumesConcrete(param) || param.IsMove)
                && sourceIsAlias)
            {
                const std::string fallbackName = aliasCallee.empty() ? std::string("<expression>") : aliasCallee;
                const std::string sourceName = !source.CallerName.empty() ? source.CallerName
                    : !source.TypeAndValue.VariableName.empty() ? source.TypeAndValue.VariableName : fallbackName;
                LogErrorMessage(
                    "cannot store an 'alias' value '{}' into an owning parameter; it borrows storage it does not own and would dangle. Use '.copy()' for an independent owned copy.",
                    { sourceName });
            }
        }
        std::vector<llvm::Value*> argList;
        // A by-value param an 'alias' return can hand back is passed as a pointer to a caller-owned
        // copy slot (ParamIsAliasReturnSlot), so the result points into this frame. A slot holding
        // a temporary is that temporary's only owner; the post-call handoff settles who frees it.
        struct AliasSlotArg
        {
            llvm::Value* Slot;
            std::string TypeName;
            bool Temp;
            std::vector<llvm::Value*> AncestorSlots;
        };
        std::vector<AliasSlotArg> aliasSlots;
        auto candParamItr = candidate.Parameters.begin();
        size_t argIndex = 0;
        for (const auto& arg : matched)
        {
            // Variadic arguments past the declared parameter list must not be dispatched
            // through pointer-parameter logic (candParamItr points at the last declared
            // param, which for printf is 'ptr %fmt' - causing all variadic args to be
            // pushed as storage/GEP addresses instead of loaded values).
            bool inVariadicRange = candidate.Variadic && argIndex >= candidate.Parameters.size();
            if (candidate.IsCxx && !inVariadicRange
                && !(candidate.IsMethod && argIndex == 0 && candidate.CxxConst)
                && argIndex < candidate.Parameters.size())
            {
                const TypeAndValue& param = candidate.Parameters[argIndex];
                const TypeAndValue& source = arg.TypeAndValue;
                if (CxxRecordPointeeConstRelation(source, param) == CxxPointeeConstRelation::Drops)
                {
                    const std::string sourceName = !arg.CallerName.empty()
                        ? arg.CallerName : source.VariableName.empty()
                            ? "<argument>" : source.VariableName;
                    LogError(std::format(
                        "cannot pass C++ const pointer argument {} ('{}') to mutable C++ pointer "
                        "parameter '{}': it points to a const C++ object, so the parameter could "
                        "write through read-only storage",
                        argIndex + 1, sourceName, param.VariableName));
                }
            }

            // An owning value rvalue has no named owner that can survive the C vararg boundary.
            // The pointer ledger covers owning pointer returns/new; by-value owning returns use
            // the same value identity ledger plus the owning-struct type test. Keep string values
            // on the existing representation-based diagnostic below so its message is unchanged.
            bool argIsUnbound = arg.Storage == nullptr
                && FindVariableStorage(arg.CallerName).Storage == nullptr;
            bool argIsStringValue = arg.Primary != nullptr
                && arg.Primary->getType() == llvm::StructType::getTypeByName(*context, "string");
            bool argIsOwningValueRValue = argIsUnbound && arg.FieldName.empty()
                && !arg.TypeAndValue.Pointer
                && (arg.IsOwningStruct || IsOwningValueStructValue(arg.Primary)
                    || (arg.IsOwningString && !argIsStringValue
                        && arg.TypeAndValue.TypeName != "string"));

            std::string argCxxClassName = arg.TypeAndValue.TypeName;
            if (argCxxClassName.empty())
                if (auto* argStruct = llvm::dyn_cast_or_null<llvm::StructType>(
                        arg.Primary != nullptr ? arg.Primary->getType() : arg.BaseType))
                    argCxxClassName = argStruct->getName().str();
            if (inVariadicRange && !arg.TypeAndValue.Pointer
                && !argCxxClassName.empty()
                && IsForeignNontrivialCxxClass(argCxxClassName))
            {
                LogError(std::format(
                    "cannot pass non-trivial C++ class '{}' to the variadic '{}' slot of '{}'; "
                    "bind it to an owner before passing it",
                    DisplayCxxClassName(argCxxClassName), "...", diagnosticFunctionName));
                return nullptr;
            }

            if (inVariadicRange
                && (arg.IsExplicitMove || IsOwningPtrTempValue(arg.Primary)
                    || argIsOwningValueRValue))
            {
                LogErrorMessage(
                    "cannot pass an owning value to the variadic '{}' slot of '{}'; bind the "
                    "value to an owner first", { "...", diagnosticFunctionName });
                return nullptr;
            }

            // Array-view parameter gate (raw-pointer, interface-value and element axes), shared
            // with the virtual-dispatch door. Placed before the binding branches because an
            // array-view param has Pointer=true and is handled by the pointer branch below.
            if (!inVariadicRange)
                RejectArrayViewParamBinding(arg, *candParamItr, diagnosticFunctionName);

            if (!inVariadicRange && IsImplicitPrimitiveToPointer(
                    *candParamItr, arg, arg.Primary) && !candidate.IsCxx)
            {
                LogError(DescribeImplicitPrimitiveToPointer(
                    *candParamItr, arg, arg.Primary, "pass",
                    std::format("parameter '{}' of '{}'", candParamItr->VariableName,
                                diagnosticFunctionName)));
                return nullptr;
            }

            // Closure SHAPE gate (value vs pointer vs view), shared with virtual dispatch.
            // Hoisted above the binding branches: it judges the pair, not one binding arm.
            if (!inVariadicRange && !candParamItr->IsInterface
                && RejectFuncPtrShapeMismatch(arg, *candParamItr))
                return nullptr;

            // Desugared spelling of the same rule as the builtin `unique IFace` parameter below:
            // an owning wrapper would free a STACK address at scope exit.
            if (!inVariadicRange && IsStackValueToCoreUniqueInterface(arg, *candParamItr))
            {
                LogErrorMessage(
                    "call to '{}': cannot pass a stack value to {} interface parameter '{}' - "
                    "it takes ownership and frees the object at scope exit, so the source must be "
                    "a heap '{}' (or a '{}' of an owned interface value), not a stack value",
                    { diagnosticFunctionName, "unique", candParamItr->VariableName, "new", "move" });
                return nullptr;
            }

            if (!inVariadicRange && candParamItr->IsRvalueRef)
            {
                argList.push_back(LowerRvalueRefArg(arg, *candParamItr, candidate.IsCxx));
            }
            // A blessed unique<IFace> wrapper is not an implementor: borrow the fat value it
            // holds through get() rather than boxing the wrapper struct itself.
            else if (!inVariadicRange && candParamItr->IsInterface && !candParamItr->IsArrayView
                && !arg.TypeAndValue.IsInterface
                && IsCoreUniqueToRawPointer(arg, *candParamItr))
            {
                argList.push_back(CreateCoreUniqueRawPointerCall(arg, *candParamItr,
                                                                  candidate.IsCxx));
            }
            // An 'IA[]' parameter is a THIN view over fat elements, never a fat value itself, so
            // nothing binds to it by boxing - the declarator door excludes views the same way.
            // A null constant (or an all-unnamed join) then binds as the null view it is,
            // instead of boxing a class named ''.
            else if (!inVariadicRange && candParamItr->IsInterface && !candParamItr->IsArrayView
                     && !arg.TypeAndValue.IsInterface)
            {
                // A `unique` interface param takes ownership and frees its boxed object at scope
                // exit. A struct VALUE source would box a STACK address as the data pointer, so
                // that teardown would free a stack address (heap corruption). Only a heap pointer
                // (`new`) may transfer ownership here. Gated on IsUnique, not IsMove: a borrow
                // `list<IShape>::add(move T value)` is a move param but does not own, so a stack
                // value bound to it stays legal.
                if (candParamItr->IsUnique && !arg.TypeAndValue.Pointer)
                {
                    LogErrorMessage(
                        "call to '{}': cannot pass a stack value to {} interface parameter '{}' - "
                        "it takes ownership and frees the object at scope exit, so the source must be "
                        "a heap '{}' (or a '{}' of an owned interface value), not a stack value",
                        { diagnosticFunctionName, "unique", candParamItr->VariableName, "new", "move" });
                    return nullptr;
                }
                // A pointer-shaped source (`T**`, `T[]`, `T[N]`, simd) is not an instance of its
                // element class, so boxing it would attach that class's vtable to the wrong storage.
                std::string argShape = DescribePointerShapedInterfaceSource(arg.TypeAndValue);
                if (!argShape.empty())
                {
                    LogRawError(FormatPointerShapedInterfaceUpcastError(
                        argShape, SpellType(*this, arg.TypeAndValue), SpellType(*this, *candParamItr)));
                    return nullptr;
                }
                // Derive struct name from TypeName if available, else from BaseType
                std::string structName = arg.TypeAndValue.TypeName;
                if (structName.empty() && arg.BaseType)
                {
                    if (auto* st = llvm::dyn_cast<llvm::StructType>(arg.BaseType))
                        structName = st->getName().str();
                }

                // Build fat value: vtable + data ptr -> {i8*, i8*} by value
                auto vtable = GetOrCreateVTable(structName, candParamItr->TypeName);
                llvm::Value* dataPtr = nullptr;
                if (arg.TypeAndValue.Pointer)
                {
                    // struct* -> interface*: data ptr IS the pointer value (not the alloca of the pointer).
                    dataPtr = arg.Primary != nullptr ? arg.Primary : LoadArgStorage(arg);
                }
                else if (arg.Storage != nullptr)
                {
                    dataPtr = arg.Storage;
                }
                else
                {
                    // Materialize a pointer to the struct value
                    auto structTy = arg.BaseType ? arg.BaseType : GetType(arg.TypeAndValue);
                    // Defensive: a hand-built NamedVariable with no BaseType and no resolvable
                    // TypeName yields a null/void type here; allocating it would crash LLVM.
                    // Report the bad call instead of forging an invalid alloca.
                    if (structTy == nullptr || structTy->isVoidTy())
                    {
                        LogErrorMessage(
                            "call to '{}': argument {} (interface parameter '{}') has no resolved type",
                            { diagnosticFunctionName, std::to_string(argIndex), candParamItr->VariableName });
                        return nullptr;
                    }
                    auto tempAlloca = AllocaAtEntry(structTy, nullptr);
                    builder->CreateStore(arg.Primary, tempAlloca);
                    dataPtr = tempAlloca;
                }
                argList.push_back(BuildInterfaceFatValue(vtable, dataPtr));
            }
            else if (!inVariadicRange && candParamItr->IsInterface && arg.TypeAndValue.IsInterface)
            {
                // Interface -> interface: pass fat struct by value, re-boxing on an upcast
                llvm::Value* val = arg.Primary ? arg.Primary : LoadArgStorage(arg);
                argList.push_back(ReboxInterfaceIfNeeded(val, arg.TypeAndValue.TypeName, candParamItr->TypeName));
            }
            else if (!inVariadicRange && candParamItr->IsCxxRefToPointer)
            {
                argList.push_back(LowerAliasByPointerArg(arg, *candParamItr));
            }
            else if (TypeAndValue pointerBoolReferent;
                     !inVariadicRange && candidate.IsCxx && arg.TypeAndValue.Pointer
                     && !IsCxxAddressOfReferent(arg, "bool")
                     && CxxConstScalarRefReferent(*candParamItr, pointerBoolReferent)
                     && pointerBoolReferent.TypeName == "bool")
            {
                llvm::Value* pointerValue = arg.Primary != nullptr
                    ? arg.Primary : LoadArgStorage(arg);
                llvm::Value* boolValue = builder->CreateIsNotNull(pointerValue, "ptr.to.bool");
                llvm::AllocaInst* temporary = AllocaAtEntry(boolValue->getType(), nullptr);
                builder->CreateStore(boolValue, temporary);
                argList.push_back(temporary);
            }
            else if (!inVariadicRange && candidate.IsCxx && arg.TypeAndValue.Pointer
                     && candParamItr->TypeName == "bool" && !candParamItr->Pointer
                     && !candParamItr->IsAlias)
            {
                llvm::Value* pointerValue = arg.Primary != nullptr
                    ? arg.Primary : LoadArgStorage(arg);
                argList.push_back(builder->CreateIsNotNull(pointerValue, "ptr.to.bool"));
            }
            else if (TypeAndValue constRefReferent;
                     !inVariadicRange && !arg.TypeAndValue.Pointer
                     && CxxConstScalarRefReferent(*candParamItr, constRefReferent))
            {
                // A C++ `const T&` scalar parameter takes the address of a T-shaped object. An
                // exact-type lvalue hands over its own slot; anything else - a literal, a folded
                // constant, an expression, a narrower value - is converted to T and materialized
                // into a frame temporary. Passing the raw scalar made the callee read an integer
                // as an address.
                argList.push_back(LowerAliasByPointerArg(arg, constRefReferent,
                    /*strictCxxScalarType*/ functionName.starts_with("__cflat_ctor_")));
            }
            else if (!inVariadicRange && candParamItr->Pointer)
            {
                bool coreUniqueToRawPointer = IsCoreUniqueToRawPointer(arg, *candParamItr);
                if (coreUniqueToRawPointer)
                {
                    llvm::Value* rawPointer = CreateCoreUniqueRawPointerCall(
                        arg, *candParamItr, candidate.IsCxx);
                    if (candidate.IsCxx && rawPointer != nullptr)
                    {
                        TypeAndValue rawSource;
                        rawSource.TypeName = CxxUniquePtrPointee(arg.TypeAndValue.TypeName);
                        if (rawSource.TypeName.empty())
                            rawSource.TypeName = MangledGenericArgument(
                                *this, arg.TypeAndValue.TypeName);
                        rawSource.Pointer = !rawSource.TypeName.empty();
                        rawPointer = AdjustCxxPointerForStore(
                            *candParamItr, rawSource, rawPointer,
                            std::format("parameter '{}' of '{}'", candParamItr->VariableName,
                                        diagnosticFunctionName));
                    }
                    argList.push_back(rawPointer);
                }
                else
                {
                // A 'string' is a {ptr,len} value, not a char*: the lowering below would hand the
                // callee the PAIR's address. Only a variadic candidate reaches here with one (a
                // non-variadic candidate is rejected by overload scoring first).
                bool argIsStringValue = !arg.TypeAndValue.Pointer
                    && (arg.TypeAndValue.TypeName == "string"
                        || (arg.Primary != nullptr
                            && arg.Primary->getType() == llvm::StructType::getTypeByName(*context, "string")));
                if (argIsStringValue && (candParamItr->TypeName == "char" || candParamItr->TypeName == "i8"))
                {
                    LogErrorMessage(
                        "cannot pass '{}' to the '{}' parameter '{}' of '{}': a '{}' is a "
                        "{} value, not a '{}' - the callee would read the pair itself. Pass "
                        "the buffer explicitly with '{}'. An interpolated string literal is a "
                        "'{}': bind it first ({}; {}).",
                        { "string", "char*", candParamItr->VariableName, diagnosticFunctionName, "string",
                          "{ptr,len}", "char*", ".data()", "string", "string s = \"{{x}}\"",
                          "printf(\"%s\", s.data())" });
                    return nullptr;
                }

                // For a non-pointer value passed to a pointer parameter (e.g. a field access
                // used as the 'this' receiver), prefer Storage (the GEP address) over Primary
                // (the pre-loaded value). Primary holds the struct value itself, which would
                // be the wrong type for a pointer parameter.
                // Guard: if Primary is already a pointer value (e.g. loaded from a global ptr),
                // use Primary directly - Storage would be the wrong level of indirection.
                if (IsNullPointerConstantArgument(arg))
                {
                    auto* pointerType = GetType(*candParamItr);
                    if (pointerType == nullptr || !pointerType->isPointerTy())
                    {
                        LogErrorMessage("cannot lower null pointer argument for parameter '{}' of '{}'",
                                        { candParamItr->VariableName, diagnosticFunctionName });
                        return nullptr;
                    }
                    argList.push_back(llvm::ConstantPointerNull::get(
                        llvm::cast<llvm::PointerType>(pointerType)));
                }
                else if (!arg.TypeAndValue.Pointer && arg.Storage != nullptr
                    && !(arg.Primary != nullptr && arg.Primary->getType()->isPointerTy()))
                {
                    llvm::Value* address = arg.Storage;
                    if (!candParamItr->IsAlias && !candParamItr->IsCxxRefToPointer
                        && !candParamItr->IsCxxConstRef && !candParamItr->IsRvalueRef
                        && IsCxxRecord(candParamItr->TypeName)
                        && IsCxxRecord(arg.TypeAndValue.TypeName))
                    {
                        TypeAndValue sourcePointer = arg.TypeAndValue;
                        sourcePointer.Pointer = true;
                        address = AdjustCxxPointerForStore(
                            *candParamItr, sourcePointer, address,
                            std::format("parameter '{}' of '{}'", candParamItr->VariableName,
                                        diagnosticFunctionName));
                    }
                    argList.push_back(address);
                }
                else if (!arg.TypeAndValue.Pointer && arg.Storage == nullptr
                         && arg.Primary != nullptr && arg.Primary->getType()->isStructTy())
                {
                    // By-value struct parameter passed to a pointer parameter (e.g. args.count()
                    // where args is a list<T> value param). Materialize on the stack first.
                    auto* tempAlloca = AllocaAtEntry(arg.Primary->getType(), nullptr);
                    builder->CreateStore(arg.Primary, tempAlloca);
                    argList.push_back(tempAlloca);
                }
                else
                {
                    // arg is a pointer type; Storage may be an alloca holding the pointer
                    // (promoted param). Load through it to get the actual pointer value.
                    llvm::Value* pointerValue = nullptr;
                    if (arg.Primary == nullptr && arg.Storage != nullptr
                        && llvm::isa<llvm::AllocaInst>(arg.Storage))
                        pointerValue = LoadArgStorage(arg);
                    else
                        pointerValue = arg.GetValue();
                    argList.push_back(pointerValue);
                }

                // A `move` parameter is checked while the argument's origin is still visible.
                llvm::Value* loweredArg = argList.back();
                if (candParamItr->IsMove
                    && IsProvableNonHeapAddress(loweredArg))
                {
                    const std::string qualifier = candParamItr->IsMove ? "'move'" : "unique";
                    LogErrorMessage(
                        "call to '{}': cannot pass the address of a stack or global value to {} "
                        "parameter '{}' - it takes ownership and frees the pointee at scope exit, "
                        "but neither is heap-allocated and freeing it is undefined. Use '{}' to "
                        "allocate on the heap, or drop '{}' if the callee only borrows.",
                        { diagnosticFunctionName, qualifier, candParamItr->VariableName, "new", "new" });
                    return nullptr;
                }
                }
            }
            else if (!inVariadicRange && candParamItr->IsFunctionPointer)
            {
                // function<T> parameter - dispatch depends on whether the callee is extern C.
                llvm::Value* val = arg.Primary ? arg.Primary : LoadArgStorage(arg);
                // A named function reaching a funcptr slot: same alias-param door as a declared
                // `function<>` binding. The thin arm below never re-resolves, so check it here.
                if (auto* fnVal = llvm::dyn_cast_or_null<llvm::Function>(val))
                    if (const FunctionSymbol* fnSym = FindSymbolForFunction(fnVal))
                        if (RejectAliasParamFuncPtrBind(
                                arg.CallerName.empty() ? fnSym->UniqueName : arg.CallerName, *fnSym))
                            return nullptr;
                // Inspect the actual LLVM param type to distinguish fat struct vs C fn ptr.
                unsigned llvmParamIndex = (unsigned)argList.size();
                if (!candidate.External)
                {
                    llvmParamIndex = 0;
                    for (size_t i = 0; i < argIndex && i < candidate.Parameters.size(); i++)
                        llvmParamIndex += ParameterCarriesRawArrayCount(candidate.Parameters[i]) ? 2u : 1u;
                }
                auto* llvmParamTy = candidate.Function->getFunctionType()->getParamType(llvmParamIndex);
                if (llvmParamTy->isStructTy())
                {
                    // Internal function<T>: provide a closure fat struct {i8*, i8*}.
                    if (val && !val->getType()->isStructTy())
                    {
                        // Re-resolve a NAMED FUNCTION only: skips same-key method overloads and
                        // picks matching 'move' flags. On a call result CallerName is the CALLEE.
                        if (!arg.CallerName.empty() && llvm::isa<llvm::Function>(val))
                        {
                            int expectedCount = (int)candParamItr->FuncPtrParams.size();
                            if (auto* correctFn = GetFunctionForFuncPtr(arg.CallerName, expectedCount, &candParamItr->FuncPtrParams))
                                val = correctFn;
                            // Reject when no overload's IsMove flags match the destination signature.
                            if (!HasFunctionWithMoveFlags(arg.CallerName, candParamItr->FuncPtrParams))
                            {
                                LogErrorMessage(
                                    "function '{}' has no overload matching the '{}' modifiers required by parameter '{}' - '{}' is part of the function-pointer type",
                                    { arg.CallerName, "move", candParamItr->VariableName, "move" });
                            }
                        }
                        // Same provenance gate virtual dispatch applies (LowerByValueArg): under
                        // opaque pointers a data pointer would otherwise land in the code slot.
                        val = WidenToClosureFatChecked(val, arg, candParamItr->VariableName);
                    }
                }
                else
                {
                    // Extern C-compatible parameter: provide a bare C function pointer.
                    // The CFlat function's raw address escapes into separately-linked C code
                    // that may store and later call it by pointer, so restore external linkage
                    // (CreateFunctionDefinition defaults non-extern functions to internal) to
                    // keep the symbol's identity across the lld-link boundary.
                    bool madeCxxCallback = false;
                    if (auto* escFn = llvm::dyn_cast<llvm::Function>(val))
                    {
                        if (escFn->getLinkage() == llvm::Function::InternalLinkage)
                            escFn->setLinkage(llvm::Function::ExternalLinkage);
                        if (candidate.IsCxx)
                        {
                            val = MakeThinFnPtrValue(escFn, *candParamItr);
                            madeCxxCallback = true;
                        }
                    }
                    if (val && val->getType()->isStructTy())
                    {
                        // The argument is a CFlat closure fat struct {code, env} - a lambda or a
                        // `function<>` variable. A C function pointer is a bare code address with
                        // no env slot; the shared helper passes a provably non-capturing invoker
                        // and rejects anything that would lose captured state across the C ABI.
                        // A rejection does not return, so this never stores null.
                        val = LowerClosureFatToThinFnPtr(val, llvmParamTy,
                            candParamItr->VariableName, arg.LambdaCaptureNames);
                    }
                    else if (candidate.IsCxx && llvmParamTy->isPointerTy()
                             && IsNullPointerConstantArgument(arg))
                    {
                        // Literal 0 matched as a C++ null pointer constant: an integer cannot be
                        // bitcast to ptr (invalid constant cast), so pass the typed null.
                        val = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(llvmParamTy));
                    }
                    else if (val && !val->getType()->isPointerTy())
                    {
                        val = builder->CreateBitCast(val, llvmParamTy, "fn_for_extern");
                    }
                    else if (val && !madeCxxCallback)
                    {
                        // Same provenance gate virtual dispatch applies (LowerByValueArg): the
                        // bitcast below would otherwise make a data pointer callable as code.
                        CheckThinFnPtrArgProvenance(val, arg, candParamItr->VariableName);
                        val = builder->CreateBitCast(val, llvmParamTy, "fn_for_extern");
                    }
                }
                argList.push_back(val);
            }
            else if (!inVariadicRange && ParameterIsAliasByPointer(*candParamItr)
                     && IsRawPointerToCoreUnique(arg, *candParamItr))
            {
                // A raw pointer names no wrapper to borrow, so build one into a temporary and
                // borrow that - the same construction the by-value arm performs.
                llvm::Value* wrapped = CreateCoreUniqueFromRawPointerCall(arg, *candParamItr);
                auto* wrapperTy = GetType(*candParamItr);
                auto* tmp = AllocaAtEntry(wrapperTy, nullptr, "unique.aliasarg");
                builder->CreateStore(wrapped, tmp);
                argList.push_back(tmp);
            }
            else if (!inVariadicRange && ParameterIsAliasByPointer(*candParamItr))
            {
                argList.push_back(LowerAliasByPointerArg(arg, *candParamItr));
            }
            else
            {
                llvm::Value* value = nullptr;
                const bool aliasSlotParam = !inVariadicRange && !candidate.IsCxx
                    && ParamIsAliasReturnSlot(candidate.ReturnType, *candParamItr);
                if (arg.Primary == nullptr)
                {
                    value = LoadArgStorage(arg);
                }
                else
                {
                    value = arg.Primary;
                }

                if (!inVariadicRange)
                {
                    if (IsRawPointerToCoreUnique(arg, *candParamItr))
                        value = CreateCoreUniqueFromRawPointerCall(arg, *candParamItr);
                    else
                    {
                        // Canonical by-value arg lowering (string coercion + move heap-copy);
                        // shared with virtual dispatch via CallInterfaceMethod.
                        value = LowerByValueArg(value, *candParamItr, arg);
                    }

                    // An owning-struct RVALUE temp passed to a by-value BORROW param has no named
                    // owner and the callee will not free it; register it for end-of-full-expression
                    // destruction. A param that TAKES ownership is excluded (it frees the temp): a
                    // `move` param, or an inferred owning-value move-SINK - otherwise the sink slot
                    // AND this end-of-expr flush both free the temp -> double-free.
                    // A CONSUME-inferred sink of a copyable owner does NOT take ownership (its store
                    // is a copy), so an rvalue temp must still be registered for end-of-expr freeing.
                    const bool externByValueReturnParam = candidate.ReturnsAliasOfByValueParam
                        && !candidate.ReturnsAlias
                        && candParamItr->IsReturnInferredSink && !candParamItr->Pointer
                        && !candParamItr->IsAlias && !candParamItr->IsMove
                        && ResolveTypeAlias(candParamItr->TypeName)
                            == ResolveTypeAlias(candidate.ReturnType.TypeName);
                    bool aliasReturnConsumes = (aliasSlotParam || externByValueReturnParam)
                        && !IsCopyableType(candParamItr->TypeName);
                    bool paramTakesOwnership = candParamItr->IsMove || aliasReturnConsumes
                        || (!candParamItr->Pointer && IsCoreUniqueType(candParamItr->TypeName))
                        || (OwningSinkConsumesConcrete(*candParamItr)
                            && (candParamItr->TypeName == "string" || IsOwningValueType(candParamItr->TypeName)));
                    if (aliasSlotParam)
                    {
                        const bool phiArg = arg.Primary != nullptr && llvm::isa<llvm::PHINode>(arg.Primary);
                        std::vector<llvm::Value*> aliasAncestors;
                        if (arg.Primary != nullptr)
                            for (const auto& aliasTemp : aliasReturnTempSlots_)
                            {
                                auto* loaded = llvm::dyn_cast<llvm::LoadInst>(arg.Primary);
                                const bool carriesAliasResult = aliasTemp.Result == arg.Primary
                                    || (loaded != nullptr
                                        && loaded->getPointerOperand() == aliasTemp.Result);
                                if (carriesAliasResult)
                                {
                                    aliasAncestors.push_back(aliasTemp.Slot);
                                    for (auto* ancestor : aliasTemp.AncestorSlots)
                                        if (std::find(aliasAncestors.begin(), aliasAncestors.end(), ancestor)
                                            == aliasAncestors.end())
                                            aliasAncestors.push_back(ancestor);
                                }
                            }
                        bool temp = BorrowedOwningStructTempQualifies(
                            arg, phiArg && !arg.TernaryTempAlreadyRegistered);
                        // `f(c ? mk(1) : mk(2))`: each arm registered its own temp; the joined value
                        // moves into the slot, so the slot takes those entries over.
                        if (!temp && phiArg && arg.TernaryTempAlreadyRegistered)
                            temp = AdoptTernaryArmStructTemps(llvm::cast<llvm::PHINode>(arg.Primary));
                        auto* slot = AllocaAtEntry(value->getType(), nullptr, "aliasslot");
                        builder->CreateStore(value, slot);
                        aliasSlots.push_back({ slot, candParamItr->TypeName, temp,
                                               std::move(aliasAncestors) });
                        value = slot;
                    }
                    else if (!paramTakesOwnership)
                    {
                        bool ternaryJoinNeedsRegistration = arg.Primary != nullptr
                            && llvm::isa<llvm::PHINode>(arg.Primary)
                            && !arg.TernaryTempAlreadyRegistered;
                        const bool loweredSretPassedByValue =
                            IsLoweredCFlatOnlyStruct(candParamItr->TypeName)
                            && arg.TypeAndValue.TypeName == candParamItr->TypeName
                            && LoweredSretTempOf(arg.Primary,
                                                 arg.Primary != nullptr ? arg.Primary->getType()
                                                                        : nullptr) != nullptr;
                        if (!loweredSretPassedByValue)
                            RegisterBorrowedOwningStructTemp(arg, ternaryJoinNeedsRegistration);
                    }
                }
                else if (value->getType()->isIntegerTy(1))
                {
                    // C default argument promotion for a variadic slot: 'bool' (i1) widens to
                    // int and is ALWAYS zero-extended (true -> 1, never -1). Without this the
                    // vararg slot keeps the caller's garbage upper bits.
                    value = Upconvert(value, builder->getInt32Ty(), true);
                }
                else if (value->getType()->isIntegerTy(8) || value->getType()->isIntegerTy(16))
                {
                    // C default argument promotion for a variadic slot: widen a sub-int
                    // integer to int, choosing zero- vs sign-extension by the source type's
                    // signedness so that e.g. u8 255 promotes to 255, not -1. Signedness is
                    // only known here (CreateFunctionCall sees a bare llvm::Value).
                    value = PromoteToInt(value, arg.TypeAndValue.IsUnsignedInteger() != -1);
                }
                else if (!arg.TypeAndValue.Pointer
                         && value->getType() == llvm::StructType::getTypeByName(*context, "string"))
                {
                    // Keyed on the REPRESENTATION, not the spelling: an interpolated string
                    // literal reaches here as a string struct with no 'string' TypeName.
                    // A 'string' is a {ptr,len} value type, not a char*. A variadic slot is
                    // untyped, so the compiler cannot know the callee wants the char* (the
                    // format string is not visible here) - passing the struct silently feeds
                    // the LENGTH to the next slot and crashes on a second '%s'. cflat already
                    // rejects string -> char* at a typed parameter; be consistent and make the
                    // user state the lowering.
                    LogErrorMessage(
                        "cannot pass '{}' to the variadic '{}' of '{}': a variadic slot is "
                        "untyped and a '{}' is a {} value, not a '{}' - the length "
                        "field would be read as the next argument. Pass the buffer explicitly with "
                        "'{}' (e.g. {}).", { "string", "...", diagnosticFunctionName, "string", "{ptr,len}",
                                              "char*", ".data()", "printf(\"%s\", s.data())" });
                }

                argList.push_back(value);
            }
            if (!inVariadicRange && candParamItr != candidate.Parameters.end() - 1)
                ++candParamItr;
            argIndex++;
        }

        // Intercept __atomic_* stubs: emit LLVM atomic IR directly.
        if (candidate.Function->getName().starts_with("__atomic_")
            || candidate.Function->getName().starts_with("_Interlocked"))
        {
            auto* atomicResult = TryEmitAtomicBuiltin(candidate.Function->getName().str(), argList);
            if (atomicResult != nullptr)
            {
                lastCallReturnType = candidate.ReturnType;
                lastCallReturnsOwned = false;
                lastCallReturnsAllocAlign = 0;
                return atomicResult;
            }
        }


        // Check: bonded value must not be passed to a move parameter (would transfer ownership out of scope).
        for (size_t i = 0; i < candidate.Parameters.size() && i < matched.size(); i++)
        {
            bool isCoreUniqueConstructor = i == 0 && IsCoreUniqueType(functionName);
            bool isCoreUniqueReset = candidate.IsMethod && functionName == "reset" && i > 0
                && !candidate.Parameters.empty() && IsCoreUniqueType(candidate.Parameters[0].TypeName);
            if (candidate.Parameters[i].IsMove && candidate.Parameters[i].Pointer
                && (isCoreUniqueConstructor || isCoreUniqueReset))
            {
                const auto& arg = matched[i];
                bool isNull = arg.Primary != nullptr
                    && llvm::isa<llvm::ConstantPointerNull>(arg.Primary);
                bool owningLocalCopyWasRebound = false;
                if (arg.BorrowsOwningLocal && arg.OwningLocalStorage != nullptr)
                {
                    auto* owner = FindVariableByStorage(arg.OwningLocalStorage);
                    owningLocalCopyWasRebound = arg.PointerRebound
                        || (owner != nullptr && owner->PointerRebound);
                }
                bool isBorrowed = arg.IsBorrowed || arg.IsAliasBorrow
                    || arg.TypeAndValue.IsAlias || arg.BorrowsOwnedElement
                    || (arg.BorrowsOwningLocal && !owningLocalCopyWasRebound);
                bool isOwned = !isBorrowed && (arg.IsOwning || arg.IsOwningString
                    || arg.IsOwningStruct || arg.TypeAndValue.IsMove
                    || IsOwningPtrTempValue(arg.Primary)
                    || IsMovedOutPtrValue(arg.Primary)
                    || owningLocalCopyWasRebound
                    || (arg.FieldName.empty() && !arg.CallerName.empty()
                        && IsVariableOwning(arg.CallerName)));
                // Temporary blessing until the deferred general move-sink borrow rule lands.
                bool tempUniqueFieldSource = JoinCarriesOwningTempUniqueField(arg.Primary)
                    || (arg.FromOwningTempField && arg.OwningTempParent);
                if (!isNull && !isOwned && !tempUniqueFieldSource)
                {
                    const std::string sourceName = BorrowedSourceName(arg);
                    std::string uniqueType = isCoreUniqueConstructor
                        ? SpellType(*this, TypeAndValue{ .TypeName = functionName })
                        : [&] {
                            TypeAndValue base = candidate.Parameters[0];
                            base.Pointer = false;
                            base.ElemPointer = false;
                            base.PointerDepth = 0;
                            base.IsArrayView = false;
                            return SpellType(*this, base);
                        }();
                    std::string destinationName;
                    if (isCoreUniqueReset && !matched.empty())
                        destinationName = matched[0].CallerName.empty()
                            ? matched[0].TypeAndValue.VariableName : matched[0].CallerName;
                    if (sourceName.empty() && destinationName.empty())
                        LogErrorMessage(
                            "cannot {} {} from a borrowed value - the source still owns it; use 'new', "
                            "a 'move' expression, or a move-returning call",
                            { isCoreUniqueReset ? "reset" : "initialize", uniqueType });
                    else if (sourceName.empty())
                        LogErrorMessage(
                            "cannot {} {} '{}' from a borrowed value - the source still owns it; use 'new', "
                            "a 'move' expression, or a move-returning call",
                            { isCoreUniqueReset ? "reset" : "initialize", uniqueType, destinationName });
                    else if (destinationName.empty())
                        LogErrorMessage(
                            "cannot {} {} from borrowed value '{}' - the source still owns it; use 'new', "
                            "a 'move' expression, or a move-returning call",
                            { isCoreUniqueReset ? "reset" : "initialize", uniqueType, sourceName });
                    else
                        LogErrorMessage(
                            "cannot {} {} '{}' from borrowed value '{}' - the source still owns it; use 'new', "
                            "a 'move' expression, or a move-returning call",
                            { isCoreUniqueReset ? "reset" : "initialize", uniqueType, destinationName,
                              sourceName });
                }
            }

            /*
             * The general SLOT-MOVE rule. Binding a BORROWED PARAMETER of the enclosing function
             * (or a local that copied one) to a `move` sink parameter hands the callee a pointer
             * the caller still owns and will free again at its own scope exit - a double free,
             * whether the `move` is spelled or implicit. Moving out of the owning SLOT reached
             * THROUGH the borrow (`move parent->children[i]`, `move _root`) stays legal and is the
             * recommended fix: the slot is the one owner and the move nulls it. Storage must be a
             * direct alloca, so every field / element access is exempt by construction. Unknown
             * provenance ACCEPTS.
             */
            if (!isCoreUniqueConstructor && !isCoreUniqueReset)
            {
                const auto& sinkParam = candidate.Parameters[i];
                bool foreignCxxPointerSink = candidate.IsCxx && matched[i].IsExplicitMove
                    && sinkParam.Pointer
                    && !sinkParam.IsAlias && !sinkParam.IsRvalueRef
                    && !sinkParam.IsCxxRefToPointer && !sinkParam.IsCxxConstRef;
                bool paramIsSink = !sinkParam.IsAlias
                    && ((sinkParam.IsMove
                         && (sinkParam.Pointer || sinkParam.IsInterfacePointer
                             || sinkParam.IsFatInterfaceValue()))
                        // A `unique<T>` BY-VALUE parameter is a sink without the keyword: the
                        // type says the callee takes ownership.
                        || (!sinkParam.Pointer && IsCoreUniqueType(sinkParam.TypeName))
                        || foreignCxxPointerSink);
                const auto& arg = matched[i];
                bool storageIsLocalSlot = arg.Storage != nullptr
                    && llvm::isa<llvm::AllocaInst>(arg.Storage);
                // The argument NV is rebuilt by the expression walk, so the borrow bits live on
                // the binding itself - ask the live variable, not the copy.
                const NamedVariable* live = storageIsLocalSlot
                    ? FindVariableByStorage(arg.Storage) : nullptr;
                bool argOwns = arg.IsOwning || arg.IsOwningStruct || arg.IsOwningString
                    || arg.OwnsInterfaceBox
                    || (live != nullptr && (live->IsOwning || live->IsOwningStruct
                                            || live->IsOwningString || live->IsNewAllocated))
                    || (!arg.CallerName.empty() && IsVariableOwning(arg.CallerName));
                if (paramIsSink && storageIsLocalSlot && !argOwns && arg.FieldName.empty()
                    && !BorrowProofRetiredByRebind(arg)
                    && (live == nullptr || !BorrowProofRetiredByRebind(*live)))
                {
                    bool directBorrowParam = !arg.CallerName.empty()
                        && IsFunctionParameter(arg.CallerName);
                    // A local read out of a FIELD of the borrowed parameter copies an OWNING
                    // SLOT, not the parameter - that is exactly the legal idiom, so it is not
                    // reached by this rule.
                    bool throughField = arg.BorrowedThroughField
                        || (live != nullptr && live->BorrowedThroughField);
                    std::string borrowOrigin = throughField ? std::string()
                        : (!arg.BorrowedOrigin.empty()
                           ? arg.BorrowedOrigin
                           : (live != nullptr && live->IsBorrowed ? live->BorrowedOrigin
                                                                  : std::string()));
                    bool copiedFromBorrowParam = !directBorrowParam
                        && !borrowOrigin.empty() && IsFunctionParameter(borrowOrigin);
                    if (directBorrowParam || copiedFromBorrowParam)
                    {
                        std::string sourceName = directBorrowParam
                            ? std::format("borrowed parameter '{}'", arg.CallerName)
                            : std::format("borrowed pointer '{}' (copied from parameter '{}')",
                                          arg.CallerName.empty() ? borrowOrigin
                                                                 : arg.CallerName,
                                          borrowOrigin);
                        std::string declName = directBorrowParam ? arg.CallerName : borrowOrigin;
                        LogErrorMessage(
                            "cannot move {} into move parameter '{}' of '{}' - the caller may still "
                            "own this pointer and will free it on scope exit. Move it out of the "
                            "owning slot instead (e.g. 'move parent->children[i]'), or declare "
                            "'{}' as 'move {}' if this function owns it.",
                            { sourceName, sinkParam.VariableName,
                              SpellFunctionSymbol(*this, diagnosticFunctionName),
                              declName, declName });
                    }
                }
            }

            if (candidate.Parameters[i].IsMove && matched[i].IsBonded)
                LogErrorMessage("parameter '{}': cannot pass bonded value to '{}' parameter - bonded values cannot be transferred out of their source's scope",
                    { candidate.Parameters[i].VariableName, "move" });
            if ((OwningSinkConsumesConcrete(candidate.Parameters[i])
                    || (candidate.Parameters[i].IsConsumeInferredSink
                        && !candidate.Parameters[i].IsReturnInferredSink
                        && !candidate.Parameters[i].IsWriteInferredSink))
                && matched[i].IsBonded)
                LogErrorMessage("parameter '{}': cannot pass bonded value to '{}' parameter - bonded values cannot be transferred out of their source's scope",
                    { candidate.Parameters[i].VariableName, "consuming" });
            // A core unique<T> wrapper owns one T, never a raw new T[n] allocation. The source
            // provenance is visible here before move transfer; unknown sources fail open.
            const auto& arg = matched[i];
            if (!candidate.Parameters[i].Pointer
                && IsCoreUniqueType(candidate.Parameters[i].TypeName)
                && HasRawNewArrayProvenance(arg))
            {
                const std::string sourceName = arg.CallerName.empty()
                    ? (arg.TypeAndValue.VariableName.empty() ? std::string("<expression>")
                                                              : arg.TypeAndValue.VariableName)
                    : arg.CallerName;
                const auto* source = arg.CallerName.empty()
                    ? FindLiveNamedVariable(arg.TypeAndValue.VariableName)
                    : FindLiveNamedVariable(arg.CallerName);
                const bool isKnownRawArray = arg.IsNewAllocated
                    || (source != nullptr && source->IsNewAllocated);
                LogErrorMessage(
                    isKnownRawArray
                        ? "cannot pass owning heap array '{}' to parameter '{}' of '{}': "
                          "unique<T> does not own arrays"
                        : "cannot pass owning pointer '{}' to parameter '{}' of '{}': hold the "
                          "result in a 'unique T*' local",
                    { sourceName, candidate.Parameters[i].VariableName, diagnosticFunctionName });
            }
            // The element slot of a borrowing container is a legal `move` destination: the
            // container keeps the ONLY handle afterwards (manual-free idiom), so the generic
            // "move into a borrow parameter transfers nothing" diagnostic is wrong there.
            if (!IsBorrowingContainerElementSink(functionName, candidate.Parameters, i,
                                                 candidate.IsMethod))
                DiagnoseExplicitMoveToBorrowParam(functionName, candidate.Parameters[i], matched[i],
                                                   candidate.IsCxx);
            RejectOwningLocalIntoBorrowingContainer(functionName, candidate.Parameters, i,
                                                    candidate.IsMethod, matched[i]);
            RejectOwningLocalIntoBorrowingHelper(functionName, candidate, i, matched[i]);
            // A string LITERAL is a 'const char*', never a 'T*' - the ARGUMENT leg of the same
            // gate the declarator, `=`, brace-init, field-default and return sites apply.
            if (IsStringLiteralIntoStructPointer(candidate.Parameters[i], matched[i].Primary))
                LogError(DescribeStringLiteralIntoStructPointer(
                    candidate.Parameters[i],
                    std::format("parameter '{}' of '{}'",
                                candidate.Parameters[i].VariableName, diagnosticFunctionName)));
        }

        // C-extern ABI lowering: when the resolved candidate has struct-by-value params or
        // return, the LLVM Function was declared with the lowered signature (iN coerce /
        // byval ptr / sret). The current argList still holds CFlat-natural struct values -
        // EmitAbiLoweredCall rewrites it to match the recipe and reloads the struct return
        // for the caller. Otherwise fall through to the existing call path.
        // Remember where this callee was first called, so an end-of-module diagnostic
        // (CheckPoisonedFunctionCalls) can point at the real call site.
        // Refuse before any argument is committed: a potentially throwing C++ callee has no
        // landing pad on the CFlat side.
        RejectThrowingCxxFunction(candidate, diagnosticFunctionName);
        if (candidate.IsCxx && candidate.Function != nullptr)
            ValidateCxxDemand(candidate.Function->getName().str());

        if (candidate.Function != nullptr)
            firstCallLocation_.emplace(candidate.Function->getName().str(),
                std::make_pair(currentLine, currentColumn));

        // The pointer arm holds for a CFlat callee too: overload scoring accepts a derived C++
        // pointer for any public-base parameter, so the argument must arrive adjusted either way.
        if (!candidate.IsCxx && !candidate.Recipe.hasLowering)
            for (size_t i = 0; i < candidate.Parameters.size() && i < matched.size()
                            && i < argList.size(); ++i)
            {
                const TypeAndValue& at = matched[i].TypeAndValue;
                const TypeAndValue& pt = candidate.Parameters[i];
                uint64_t off = 0;
                bool inaccessible = false;
                if (IsCxxDerivedToBasePointer(at, pt)
                    && FindCxxBaseOffset(at.TypeName, pt.TypeName, off, inaccessible))
                    argList[i] = EmitCxxBaseAdjust(argList[i], off);
            }
        llvm::Value* rawReturnCountSlot = nullptr;
        if (!candidate.Recipe.hasLowering && !candidate.External)
        {
            std::vector<llvm::Value*> abiArgs;
            abiArgs.reserve(argList.size() + candidate.Parameters.size() + 1);
            size_t normalIndex = 0;
            for (size_t i = 0; i < candidate.Parameters.size() && normalIndex < argList.size(); i++)
            {
                abiArgs.push_back(argList[normalIndex++]);
                if (ParameterCarriesRawArrayCount(candidate.Parameters[i]))
                    abiArgs.push_back(RawArrayCountArgument(matched[i]));
            }
            while (normalIndex < argList.size()) abiArgs.push_back(argList[normalIndex++]);
            if (ReturnCarriesRawArrayCount(candidate.ReturnType))
            {
                rawReturnCountSlot = CreateRawArrayReturnCountSlot();
                abiArgs.push_back(rawReturnCountSlot);
            }
            argList = std::move(abiArgs);
        }

        std::vector<llvm::Value*> cflatRawArrayCounts;
        if (!candidate.External && candidate.Recipe.hasLowering)
        {
            cflatRawArrayCounts.resize(candidate.Parameters.size(), nullptr);
            for (size_t i = 0; i < candidate.Parameters.size() && i < matched.size(); ++i)
                if (ParameterCarriesRawArrayCount(candidate.Parameters[i]))
                    cflatRawArrayCounts[i] = RawArrayCountArgument(matched[i]);
        }

        // Null move sources before the callee can observe or reseat an aliased slot.
        moveTransferConsumedTemps_.clear();
        ApplyMoveParamTransfer(functionName, candidate.Parameters, matched, true,
                               candidate.IsMethod, true, candidate.IsCxx);
        std::vector<llvm::Value*> sinkConsumedTemps = std::move(moveTransferConsumedTemps_);
        moveTransferConsumedTemps_.clear();

        /*
         * M4b - foreign nontrivial C++ values crossing this call by value.
         *
         * Argument: clang arranges it Indirect-WITHOUT-byval, i.e. a bare pointer to storage the
         * CALLER owns. So copy-construct (or move-construct for `move x`) a temp here and hand
         * over its address; the byte copy the generic ByVal path would do is illegal for such a
         * type. Under Itanium the caller destroys that temp after the call, so it joins the
         * ordinary end-of-statement owned-temp list (whose destructor for this class IS the C++
         * complete-object destructor); under the MS ABI the CALLEE destroys its by-value
         * parameter, so the temp is handed over and never destroyed here.
         *
         * Result: an armed declaration slot (pendingCxxSretDest_) becomes the sret pointer, so
         * the callee constructs directly into the local and nothing is copied back out.
         */
        std::vector<llvm::Value*> cxxIndirectArgAddrs;
        llvm::Value* cxxSretDest = nullptr;
        llvm::Value* cxxRetTemp = nullptr;
        const bool cxxClassReturn = candidate.Recipe.hasLowering
            && candidate.Recipe.retSlot.kind == AbiSlot::SRetReturn
            && !candidate.ReturnType.Pointer
            && ReturnsViaCxxSret(candidate.ReturnType.TypeName);
        const bool cxxClassParam = std::any_of(candidate.Recipe.paramSlots.begin(),
            candidate.Recipe.paramSlots.end(), [&](const AbiSlot& slot) {
                return slot.kind == AbiSlot::ByVal && slot.structTy != nullptr;
            });
        if (candidate.Recipe.hasLowering && (candidate.IsCxx || cxxClassReturn || cxxClassParam))
        {
            for (size_t i = 0; i < candidate.Recipe.paramSlots.size()
                            && i < candidate.Parameters.size(); ++i)
            {
                if (candidate.Recipe.paramSlots[i].kind != AbiSlot::ByVal) continue;
                const std::string& pn = candidate.Parameters[i].TypeName;
                const bool cxxObject = IsForeignNontrivialCxxClass(pn);
                const bool cflatHolder = !cxxObject && HasForeignNontrivialCxxField(pn);
                if (!cxxObject && !cflatHolder) continue;
                if (cflatHolder && IsLoweredCFlatOnlyStruct(pn) && i < matched.size())
                {
                    auto* resultSlot = LoweredSretTempOf(matched[i].Primary,
                                                         candidate.Recipe.paramSlots[i].structTy);
                    if (resultSlot != nullptr)
                    {
                        // A CFlat sret result is already the storage for this by-value parameter.
                        // C++17 prvalue elision binds it directly; the callee owns its lifetime.
                        cxxIndirectArgAddrs.resize(candidate.Recipe.paramSlots.size(), nullptr);
                        cxxIndirectArgAddrs[i] = resultSlot;
                        if (matched[i].Primary == lastLoweredRetValue_)
                        {
                            lastLoweredRetTemp_ = nullptr;
                            lastLoweredRetValue_ = nullptr;
                        }
                        continue;
                    }
                }
                // `take(c ? a : b)` of a lowered struct: both arms are loads of addressable
                // objects, so copy from a join of their addresses, not a relocated value.
                if (cflatHolder && i < matched.size() && matched[i].Storage == nullptr)
                    if (auto* phi = llvm::dyn_cast_or_null<llvm::PHINode>(matched[i].Primary);
                        phi != nullptr && phi->getType() == candidate.Recipe.paramSlots[i].structTy)
                    {
                        bool allLoads = phi->getNumIncomingValues() > 0;
                        for (unsigned k = 0; k < phi->getNumIncomingValues() && allLoads; ++k)
                        {
                            auto* load = llvm::dyn_cast<llvm::LoadInst>(phi->getIncomingValue(k));
                            allLoads = load != nullptr && !IsProducedTempValue(load);
                        }
                        if (allLoads)
                        {
                            llvm::IRBuilder<> joinBuilder(phi->getParent(),
                                phi->getParent()->getFirstInsertionPt());
                            auto* addressJoin = joinBuilder.CreatePHI(builder->getPtrTy(),
                                phi->getNumIncomingValues(), "lowered.arg.addr");
                            for (unsigned k = 0; k < phi->getNumIncomingValues(); ++k)
                                addressJoin->addIncoming(llvm::cast<llvm::LoadInst>(
                                    phi->getIncomingValue(k))->getPointerOperand(),
                                    phi->getIncomingBlock(k));
                            matched[i].Storage = addressJoin;
                        }
                    }
                if (i >= matched.size() || matched[i].Storage == nullptr)
                {
                    if (cflatHolder)
                        LogError(std::format(
                            "cannot pass struct '{}' by value to parameter '{}' of '{}': the "
                            "argument must be a variable, a field or another addressable object "
                            "so the copy constructors of its C++ fields can run", pn,
                            candidate.Parameters[i].VariableName, diagnosticFunctionName));
                    else
                        LogError(std::format(
                            "cannot pass C++ class '{}' by value to parameter '{}' of '{}': the "
                            "argument must be a variable, a field or another addressable object so "
                            "its copy constructor can run", DisplayCxxClassName(pn),
                            candidate.Parameters[i].VariableName, diagnosticFunctionName));
                    continue;
                }
                auto* structTy = candidate.Recipe.paramSlots[i].structTy;
                auto* temp = AllocaAtEntry(structTy, nullptr, "cxx.argtemp",
                                           candidate.Recipe.paramSlots[i].align);
                const bool useMove = candidate.Parameters[i].IsMove
                    || matched[i].IsExplicitMove
                    || matched[i].CxxParamLastUse
                    || (!candidate.IsCxx
                        && IsForeignNontrivialCxxClass(candidate.Parameters[i].TypeName)
                        && candidate.Parameters[i].IsOwningSink
                        && OwningSinkConsumesConcrete(candidate.Parameters[i]))
                    || (matched[i].IsRvalue
                        && (cxxObject || !candidate.IsCxx));
                // A by-value parameter a 'move x' can fill: name the deleted copy and that remedy.
                if (!useMove && cxxObject && FindCxxCopyCtor(pn) == nullptr
                    && IsCopyDeletedCxxLvalue(matched[i])
                    && (FindCxxMoveCtor(pn) != nullptr || TryBindCxxGeneratedMoveCtor(pn) != nullptr))
                {
                    LogError(CxxDeletedCopyMessage(matched[i], candidate.Parameters[i].VariableName,
                                                   diagnosticFunctionName, /*moveRemedy*/ true));
                    continue;
                }
                if (!EmitCxxByValueParamConstruct(pn, temp, matched[i].Storage, useMove,
                                                  "into a by-value parameter")) continue;
                if (cxxObject && !IsCxxParamDestroyedInCallee(pn))
                    RegisterOwnedStructTemp(temp, pn);
                cxxIndirectArgAddrs.resize(candidate.Recipe.paramSlots.size(), nullptr);
                cxxIndirectArgAddrs[i] = temp;
                if (matched[i].CxxParamLastUse && !cflatHolder && !matched[i].IsElementAccess
                    && matched[i].FieldName.empty())
                {
                    const std::string sourceName = matched[i].CallerName.empty()
                        ? matched[i].TypeAndValue.VariableName : matched[i].CallerName;
                    MarkVariableMoved(sourceName);
                }
            }
            if (cxxClassReturn)
            {
                if (pendingCxxSretDest_ != nullptr
                    && candidate.ReturnType.TypeName == pendingCxxSretTypeName_)
                {
                    cxxSretDest = pendingCxxSretDest_;
                    pendingCxxSretDest_ = nullptr;
                    pendingCxxSretTypeName_.clear();
                }
                else
                {
                    // No declaration slot is waiting for this result, so the returned object is a
                    // TEMPORARY. It must still be destroyed: give it a named slot and hand that
                    // slot to the ordinary end-of-statement owned-temp list, whose destructor for
                    // this class is the C++ complete-object destructor.
                    cxxSretDest = AllocaAtEntry(candidate.Recipe.retSlot.structTy, nullptr,
                                                "cxx.rettemp", candidate.Recipe.retSlot.align);
                    cxxRetTemp = cxxSretDest;
                }
            }
        }

        /*
         * M6 - derived-to-base pointer adjustment, and virtual dispatch.
         *
         * Both are pure pointer arithmetic Clang told us the offsets for. An argument typed as a
         * class that publicly derives from the parameter's class is shifted to that base's
         * subobject; a member INHERITED from a non-primary base has its `this` shifted the same
         * way (the offset was recorded when the base's method was cloned onto this class); and a
         * VIRTUAL member is then reached through the pointer loaded out of the receiver's vptr,
         * indexed by the slot ItaniumVTableContext assigned it.
         */
        llvm::Value* cxxVirtualCallee = nullptr;
        if (candidate.IsCxx)
        {
            for (size_t i = 0; i < candidate.Parameters.size() && i < matched.size()
                            && i < argList.size(); ++i)
            {
                const TypeAndValue& pt = candidate.Parameters[i];
                const TypeAndValue& at = matched[i].TypeAndValue;
                uint64_t off = 0;
                bool inaccessible = false;
                if (IsCxxDerivedToBasePointer(at, pt))
                {
                    if (FindCxxBaseOffset(at.TypeName, pt.TypeName, off, inaccessible))
                        argList[i] = EmitCxxBaseAdjust(argList[i], off);
                }
                else if (!at.Pointer && pt.IsAlias && pt.Pointer && !pt.ElemPointer
                         && IsCxxDerivedToBaseValue(at, pt)
                         && FindCxxBaseOffset(at.TypeName, pt.TypeName, off, inaccessible))
                {
                    argList[i] = EmitCxxBaseAdjust(argList[i], off);
                }
            }
            if (candidate.IsMethod && !argList.empty() && !candidate.Parameters.empty())
            {
                auto adj = cxxThisAdjust_.find(
                    CxxThisAdjustKey(candidate.Parameters[0].TypeName, candidate.UniqueName));
                if (adj != cxxThisAdjust_.end())
                    argList[0] = EmitCxxBaseAdjust(argList[0], adj->second);
                llvm::Value* adjustedThis = argList[0];
                cxxVirtualCallee = EmitCxxVirtualCallee(candidate, argList[0], &adjustedThis);
                if (cxxVirtualCallee != nullptr) argList[0] = adjustedThis;
            }
        }

        const bool calleeMayUnwind = CalleeMayUnwind(candidate);
        // Only this call's own invoke treats the sink arguments as handed over; an unwind out of
        // an argument conversion emitted above still owns and frees them.
        struct ConsumedTempsScope
        {
            LLVMBackend& b;
            ~ConsumedTempsScope() { b.unwindCallConsumedTemps_.clear(); }
        } consumedTempsScope{ *this };
        unwindCallConsumedTemps_ = std::move(sinkConsumedTemps);
        // The call's landing pad frees only temps already on the ledger: register a direct C++
        // callee's gated owning-pointer args first (virtual dispatch has no body to prove).
        const bool cxxTempsRegisteredBeforeCall = candidate.IsCxx && !candidate.Recipe.hasLowering
            && cxxVirtualCallee == nullptr
            && RegisterCxxOwningPtrArgsBeforeCall(candidate.Function, argList, calleeMayUnwind);
        if (!candidate.IsCxx && !candidate.Recipe.hasLowering && cxxVirtualCallee == nullptr
            && calleeMayUnwind)
            PreserveRetainedJoinArmTempsBeforeCall(candidate.Function, argList);
        if (IsVerbose() && functionName.find("operator") != std::string::npos)
        {
            const std::string selectedName = candidate.SourceName.empty()
                ? functionName : candidate.SourceName;
            std::cout << std::format("[verbose] selected operator overload: {} ({})\n",
                                     selectedName, candidate.UniqueName);
        }
        llvm::Value* result = candidate.Recipe.hasLowering
            ? EmitAbiLoweredCall(candidate, argList, cxxSretDest,
                                 cxxIndirectArgAddrs.empty() ? nullptr : &cxxIndirectArgAddrs,
                                 cxxVirtualCallee,
                                 cflatRawArrayCounts.empty() ? nullptr : &cflatRawArrayCounts,
                                 calleeMayUnwind)
            : (cxxVirtualCallee != nullptr
                ? (llvm::Value*)CreateCallOrInvoke(candidate.Function->getFunctionType(),
                                                   cxxVirtualCallee, argList, calleeMayUnwind)
                : CreateFunctionCall(candidate.Function, argList, calleeMayUnwind));
        RecordOpaqueReturnCall(result, candidate.ReturnType.TypeName, functionName);
        unwindCallConsumedTemps_.clear();
        // Settle the copy slots of an alias-return call. When every possible source slot holds a
        // temporary, the result is one of them: it owns that value, and a slot the result does not
        // name is destroyed at the end of the full expression (runtime-compared with several).
        // A named source keeps today's non-owning alias; a mixed call frees its temps as before.
        bool resultIsAlias = candidate.ReturnsAlias;
        if (!aliasSlots.empty() && result != nullptr && result->getType()->isPointerTy())
        {
            const bool allTemps = std::all_of(aliasSlots.begin(), aliasSlots.end(),
                [](const AliasSlotArg& s) { return s.Temp; });
            if (allTemps && candidate.ReturnsAliasOfByValueParam)
            {
                auto* owned = builder->CreateLoad(GetType(candidate.ReturnType), result, "aliasret.owned");
                if (aliasSlots.size() > 1)
                    for (const auto& s : aliasSlots)
                    {
                        auto* live = AllocaAtEntry(builder->getInt1Ty(), nullptr, "aliasslot.live");
                        builder->CreateStore(builder->CreateICmpNE(result, s.Slot), live);
                        pendingOwnedStructTemps.push_back(
                            { s.Slot, s.TypeName, builder->GetInsertBlock(), live });
                    }
                PropagateProducedTempValue(result, owned);
                aliasTransferResults_.push_back(owned);
                result = owned;
                resultIsAlias = false;
            }
            else if (candidate.ReturnsAliasOfByValueParam)
            {
                // A mixed named/temp call cannot statically know whether the alias result names
                // a borrowed local or the temporary. Keep each temp in the full-expression
                // ledger, so branch and loop paths destroy only slots created by that call.
                for (const auto& s : aliasSlots)
                {
                    if (s.Temp) RegisterOwnedStructTemp(s.Slot, s.TypeName);
                    RegisterAliasReturnTempSlot(result, s.Slot, s.AncestorSlots, shownFunctionName);
                }
            }
            else
                for (const auto& s : aliasSlots)
                    if (s.Temp) RegisterOwnedStructTemp(s.Slot, s.TypeName);
        }
        if (cxxRetTemp != nullptr)
        {
            // A classified CFlat struct result keeps CFlat by-value ownership (the loaded value
            // is the owned temporary, as before sret); only C++ class results own the slot.
            if (!IsLoweredCFlatOnlyStruct(candidate.ReturnType.TypeName))
                RegisterOwnedStructTemp(cxxRetTemp, candidate.ReturnType.TypeName);
            result = builder->CreateLoad(candidate.Recipe.retSlot.structTy, cxxRetTemp);
            // The loaded value stands for the call result: a produced temporary, exactly like
            // the by-value CallInst it replaces (discard and ownership checks key on that).
            if (IsLoweredCFlatOnlyStruct(candidate.ReturnType.TypeName))
                nullConditionalTempResults_.push_back(result);
        }
        if (candidate.Recipe.hasLowering
            && (candidate.IsCxx || cxxClassReturn)
            && !candidate.ReturnType.Pointer && result != nullptr && result->getType()->isStructTy())
        {
            // Remembered for a declaration initialized by this call: the temp (sret) or null
            // when the ABI returned the object in registers (trivial for calls, a plain store).
            if (!candidate.IsCxx && IsLoweredCFlatOnlyStruct(candidate.ReturnType.TypeName))
            {
                lastLoweredRetTemp_ = cxxRetTemp;
                lastLoweredRetValue_ = result;
            }
            else
            {
                lastCxxRetTemp_ = cxxRetTemp;
                lastCxxRetValue_ = result;
            }
        }

        RegisterRawArrayCallResult(result, rawReturnCountSlot,
                                   candidate.ReturnType.AllocAlignValue);

        // Runs before ApplyMoveParamTransfer, whose UnregisterOwnedPtrTemp still has the
        // last word for a sink parameter.
        DropRetainedJoinArmPtrTemps(result);
        if (!cxxTempsRegisteredBeforeCall)
            RegisterNonEscapingOwningPtrArgs(result, candidate.IsCxx);

        // A bare interface element in the core list aliases the object; the caller's owning
        // handle must be retired so removeAt()+delete remains the one release path. Keep this
        // tied to the core list receiver - a user-defined `add` method is not an ownership API.
        if (functionName == "add" && candidate.IsMethod && candidate.Parameters.size() >= 2
            && candidate.Parameters[0].Pointer
            && MangledBase(candidate.Parameters[0].TypeName) == "list"
            && candidate.Parameters[1].IsFatInterfaceValue())
        {
            for (const auto& arg : matched)
            {
                if (!arg.TypeAndValue.IsFatInterfaceValue()
                    || !arg.OwnsInterfaceBox || arg.CallerName.empty() || !arg.FieldName.empty())
                    continue;
                SetVariableOwning(arg.CallerName, false);
                SetVariableOwnsInterfaceBox(arg.CallerName, false);
            }
        }

        // An `adopt` parameter publishes an owned interface box into callee-managed storage.
        // The source remains readable, but its local teardown ownership is retired.
        for (size_t i = 0; i < candidate.Parameters.size() && i < matched.size(); i++)
        {
            if (!candidate.Parameters[i].IsAdopt) continue;
            const auto& arg = matched[i];
            bool ownedMoveInterface = arg.TypeAndValue.IsFatInterfaceValue()
                && arg.TypeAndValue.IsMove && arg.Storage == nullptr && arg.FieldName.empty();
            bool ownedInterfaceLocal = arg.OwnsInterfaceBox
                || arg.IsAdoptable
                || (arg.TypeAndValue.IsFatInterfaceValue() && arg.IsOwning && arg.TypeAndValue.IsMove);
            if (arg.TypeAndValue.IsUnique
                || (!ownedInterfaceLocal && !ownedMoveInterface)
                || (!arg.CallerName.empty() && !arg.FieldName.empty()))
                LogErrorMessage(
                    "call to '{}': adopt parameter '{}' requires an owned interface local or "
                    "a move-returning interface result; "
                    "a unique local or borrowed interface cannot be adopted",
                    { diagnosticFunctionName, candidate.Parameters[i].VariableName });
            if (!arg.CallerName.empty())
            {
                SetVariableOwning(arg.CallerName, false);
                SetVariableOwnsInterfaceBox(arg.CallerName, false);
            }
        }

        // A callee that provably hands EXACTLY this owning argument back aliases it, so the
        // result carries the ownership and an owning destination can adopt it.
        AdoptLaunderedOwningTempResult(result);
        // The REJECT-side twin: a callee whose every return reads a live `unique` field hands back
        // an object that field still owns, so this result is a borrow, not something to adopt.
        const auto* uniqueFieldBorrow = !candidate.ReturnsOwned
            ? FindUniqueFieldBorrowReturn(candidate.Function) : nullptr;
        if (result != nullptr && uniqueFieldBorrow != nullptr)
            RegisterUniqueFieldBorrowResult(result, *uniqueFieldBorrow);

        // Extern C function returning a function pointer: the LLVM-level return type is a
        // bare ptr but CFlat function<T> variables hold the {fn, env} closure fat struct.
        // Wrap via a per-signature thunk so the indirect-call path (which always prepends
        // env to the args) reaches the real C function correctly.
        if (candidate.ReturnType.IsFunctionPointer
            && !candidate.ReturnType.IsThinFnPtr()
            && result != nullptr
            && result->getType()->isPointerTy())
        {
            result = WrapCFuncPtrAsFatStruct(result, candidate.ReturnType);
        }

        // Cache the resolved return type so callers can populate TypeAndValue after the call.
        lastCallReturnType = candidate.ReturnType;
        lastCallReturnType.IsAlias = resultIsAlias; // mark borrow-return result; inert until consumed
        if (resultIsAlias) RegisterAliasValue(result);
        // The call RESULT is now the current expression value, so a `new`/`move` that ran only in
        // the ARGUMENT list describes a different value: retire its sticky channels here.
        lastOwningResult = false;
        lastAllocAlignment = 0;
        // A core unique return owns like `move`.
        lastCallReturnsOwned = uniqueFieldBorrow == nullptr
            && (candidate.ReturnsOwned
                || (!resultIsAlias && !candidate.ReturnType.Pointer
                    && IsCoreUniqueType(candidate.ReturnType.TypeName)));
        // Ledger the owning-return result by value for the no-discard check: string / pointer /
        // interface via lastCallReturnsOwned, plus a by-value owning-value STRUCT return (move S).
        bool ownedValueStructReturn = uniqueFieldBorrow == nullptr && !resultIsAlias
            && !candidate.ReturnType.Pointer
            && candidate.ReturnType.TypeName != "string"
            && IsOwningValueType(candidate.ReturnType.TypeName);
        if (lastCallReturnsOwned || ownedValueStructReturn)
        {
            RegisterOwnedReturnTemp(result, functionName, candidate.ReturnType);
            // An `alias` return hands back a BORROW the callee still owns. Keep the entry VISIBLE
            // to the no-discard check, but never let it answer an ownership question - otherwise a
            // '?:' arm scores it owning and the receiving local destroys the callee's live value.
            if (resultIsAlias) SuppressCallerRelease(result);
        }
        // Return-type `alignas(_, N)`: the callee hands back an N-aligned heap block. Stamp the
        // side-channel so the receiving local frees via __delete_aligned (consumed in ParseDeclaration).
        lastCallReturnsAllocAlign = candidate.ReturnType.AllocAlignValue;

        // Populate lock-check side-channels for call-site RequiredLocks verification.
        lastCallRequiredLocks = candidate.RequiredLocks;
        lastCallParameterNames.clear();
        for (const auto& p : candidate.Parameters)
            lastCallParameterNames.push_back(p.VariableName);

        // Populate bond side-channel: collect source variable names for bond parameters.
        lastCallIsBonded = false;
        lastCallBondByAddress = false;
        lastCallBondedSources.clear();
        for (size_t i = 0; i < candidate.Parameters.size() && i < matched.size(); i++)
        {
            if (candidate.Parameters[i].IsBond && !matched[i].CallerName.empty())
            {
                lastCallIsBonded = true;
                lastCallBondedSources.push_back(matched[i].CallerName);
            }
        }

        // A bonded argument the callee hands back keeps its bond: the result still borrows the
        // caller's frame, so the escape checks must see the sources on the call result.
        if (candidate.Function != nullptr && !candidate.Function->isVarArg())
        {
            size_t expandedParams = 0;
            const bool useRecipeSlots = candidate.External && candidate.Recipe.hasLowering
                && candidate.Recipe.paramSlots.size() == candidate.Parameters.size();
            if (useRecipeSlots)
            {
                for (const auto& slot : candidate.Recipe.paramSlots)
                    expandedParams += SlotLLVMParamCount(slot);
            }
            else
            {
                for (const auto& param : candidate.Parameters)
                    expandedParams += ParameterCarriesRawArrayCount(param) ? 2u : 1u;
            }
            size_t trailingRawCount = (!candidate.External
                && ReturnCarriesRawArrayCount(candidate.ReturnType)) ? 1u : 0u;
            if (candidate.Function->arg_size() >= expandedParams + trailingRawCount)
            {
                unsigned llvmParamIndex = (unsigned)(candidate.Function->arg_size()
                    - expandedParams - trailingRawCount);
                for (size_t i = 0; i < candidate.Parameters.size() && i < matched.size(); i++)
                {
                    const unsigned slotCount = useRecipeSlots
                        ? SlotLLVMParamCount(candidate.Recipe.paramSlots[i])
                        : (ParameterCarriesRawArrayCount(candidate.Parameters[i]) ? 2u : 1u);
                    if (matched[i].IsBonded && !matched[i].BondedSources.empty()
                        && ParameterMayReachReturn(candidate.Function, llvmParamIndex))
                    {
                        lastCallIsBonded = true;
                        for (const auto& source : matched[i].BondedSources)
                            if (std::find(lastCallBondedSources.begin(), lastCallBondedSources.end(),
                                          source) == lastCallBondedSources.end())
                                lastCallBondedSources.push_back(source);
                    }
                    llvmParamIndex += slotCount;
                }
            }
        }

        // A consuming closure bound to a DECLARED closure PARAMETER: the parameter's registered
        // type cannot adopt the inferred sink, so the callee consumes and the caller frees again.
        for (size_t i = 0; i < candidate.Parameters.size() && i < matched.size(); i++)
        {
            int lostSink = FindLostClosureSinkParam(candidate.Parameters[i],
                                                    matched[i].TypeAndValue);
            if (lostSink < 0) continue;
            LogRawError(DescribeLostClosureSink(candidate.Parameters[i], (size_t)lostSink,
                std::format("parameter '{}' of '{}'",
                            candidate.Parameters[i].VariableName, diagnosticFunctionName)));
        }

        // Retire move temporaries and mark the source moved after the call.
        std::vector<TypeAndValue> transferParamsStorage;
        const auto* transferParams = &candidate.Parameters;
        if (candidate.ReturnsAliasOfByValueParam && !candidate.ReturnsAlias && !candidate.External)
        {
            transferParamsStorage = candidate.Parameters;
            transferParams = &transferParamsStorage;
            for (auto& param : transferParamsStorage)
                if (param.IsReturnInferredSink && !param.Pointer
                    && ResolveTypeAlias(param.TypeName) == ResolveTypeAlias(candidate.ReturnType.TypeName))
                    param.IsReturnInferredSink = false;
        }
        ApplyMoveParamTransfer(functionName, *transferParams, matched, true,
                               candidate.IsMethod, false, candidate.IsCxx);

        // A temp's `unique` field handed to a PLAIN `T*` parameter. Runs AFTER the sink reject
        // above, so `unique` / `move` parameters never reach the callee-side question.
        RecordTempUniqueFieldArgs(result, functionName, matched);

        // Register a closure-returning call RESULT as an owned closure temp (lambda Option A),
        // mirroring how a lambda LITERAL is tracked at creation. A binding site (decl-init /
        // assignment / field store / return) calls UnregisterOwnedClosureTemp so only the owner
        // frees it; a result used INLINE (invoked directly, or passed by value as an argument) and
        // never bound is freed by FlushOwnedClosureTemps at end-of-full-expression. Exclude the
        // `copy` clone - its result is always consumed by an owner or stored into a struct field by
        // the synthesized memberwise copy, so flushing it would double-free a now-owned field.
        // A monomorphized generic `T` return (e.g. queue<Lambda>::dequeue) has a bare-T static
        // ReturnType, not IsFunctionPointer, yet the runtime value IS a closure fat struct. Gate on
        // the concrete LLVM type so the returned env temp is cleaned up at end-of-expr (no leak).
        // An `alias T` BORROW return (e.g. queue<Lambda>::peek) must NOT be registered - freeing a
        // borrowed env would double-free the slot the container still owns.
        if (result != nullptr
            && functionName != "copy"
            && !resultIsAlias
            && result->getType() == GetClosureFatPtrType())
            RegisterOwnedClosureTemp(result);

        // Register an owned-string-returning call RESULT as an owned string temp, mirroring the
        // closure case above and TrackOwnedStringOperatorResult (operator+). A binding site
        // (decl-init / assignment / field store / move-param / return) calls
        // UnregisterOwnedStringTemp so only the owner frees it; a result used INLINE - passed by
        // value as a borrow (non-move 'string') argument, or as an expression statement - is never
        // bound and would otherwise leak, so it is freed by FlushOwnedStringTemps at end-of-full-
        // expression. Exclude the 'copy' clone: the synthesized memberwise copy stores its result
        // straight into a struct field (GetOrCreateMemberwiseCopy), bypassing the assignment-path
        // unregister, so flushing it would double-free a now-owned field - same reasoning as closures.
        if (result != nullptr
            && candidate.ReturnsOwned
            && functionName != "copy"
            && result->getType() == llvm::StructType::getTypeByName(*context, "string"))
            RegisterOwnedStringTemp(result);

        return result;
    }

llvm::Function* LLVMBackend::GetFunction(const std::string& functionName)
{
        // A C++ free function whose signature binding was deferred to first use.
        TryBindCxxFunction(functionName);
        auto functionSym = functionTable.find(functionName);

        if (functionSym != functionTable.end() && !functionSym->second.empty())
        {
            return functionSym->second.front().Function;
        }

        return module->getFunction(functionName);
    }

bool LLVMBackend::HasFunctionWithMoveFlags(std::string functionName, const std::vector<TypeAndValue::FuncPtrParam>& expectedParams) const
{
        functionName = ResolveQualifiedName(functionName);
        auto it = functionTable.find(functionName);
        if (it == functionTable.end()) return true;  // unknown - leave to other mechanisms
        bool sawCountMatch = false;
        for (const auto& sym : it->second)
        {
            if (sym.IsMethod) continue;
            if (sym.Parameters.size() != expectedParams.size()) continue;
            sawCountMatch = true;
            bool ok = true;
            for (size_t i = 0; i < sym.Parameters.size(); i++)
                if (sym.Parameters[i].IsMove != expectedParams[i].IsMove) { ok = false; break; }
            if (ok) return true;
        }
        // No exact-count overload exists -> not a 'move-modifier' problem; let other type-check paths handle.
        return !sawCountMatch;
    }

bool LLVMBackend::RejectAliasParamFuncPtrBind(const std::string& functionName,
                                              const FunctionSymbol& sym)
{
        // A non-pointer `alias` param is passed as a pointer to the caller's object, but a
        // function-pointer type has no spelling for that, so an indirect call would pass the
        // value itself and the callee would read it as an address.
        for (const auto& p : sym.Parameters)
        {
            if (!ParameterIsAliasByPointer(p)) continue;
            LogErrorMessage(
                "function '{}' cannot be used as a function pointer: its parameter '{}' is "
                "'{}', which is passed as a reference to the caller's object and has no "
                "function-pointer spelling. Declare the parameter '{}' instead, or drop '{}'",
                { SpellFunctionSymbol(*this, functionName), p.VariableName,
                  "alias " + SpellType(*this, p), SpellType(*this, p) + "*", "alias" });
            return true;
        }
        // A C-linkage definition with a CFlat body destroys its owning by-value params, but only a
        // direct call hands the argument over; an indirect call would leave the caller freeing it too.
        if (sym.External && sym.HasCFlatBody)
            for (const auto& p : sym.Parameters)
            {
                if (!CFlatExternOwnsByValueParam(p)) continue;
                LogErrorMessage(
                    "function '{}' cannot be used as a function pointer: its 'extern' definition "
                    "owns and destroys its by-value parameter '{}' of type '{}', which only a direct "
                    "call hands over. Declare the parameter '{}', or call '{}' directly",
                    { SpellFunctionSymbol(*this, functionName), p.VariableName, SpellType(*this, p),
                      "move " + SpellType(*this, p), SpellFunctionSymbol(*this, functionName) });
                return true;
            }
        return false;
    }

llvm::Function* LLVMBackend::GetFunctionForFuncPtr(std::string functionName, int expectedParamCount,
                                          const std::vector<TypeAndValue::FuncPtrParam>* expectedParams,
                                          const TypeAndValue* destSig)
{
        functionName = ResolveQualifiedName(functionName);
        auto it = functionTable.find(functionName);
        if (it == functionTable.end() || it->second.empty())
            return module->getFunction(functionName);

        std::vector<const FunctionSymbol*> viable;
        for (const auto& sym : it->second) viable.push_back(&sym);

        if (destSig != nullptr && !destSig->FuncPtrReturnTypeName.empty())
        {
            // The DESCRIBE function is the rule: asking it keeps the verdict and the message from
            // ever disagreeing about which overloads were refused and why.
            std::vector<const FunctionSymbol*> bindable;
            for (auto* sym : viable)
                if (DescribeFuncPtrBindMismatch(functionName, FuncPtrSigOfSymbol(*sym), *destSig).empty())
                    bindable.push_back(sym);
            if (bindable.empty())
            {
                LogRawError(DescribeFuncPtrBindMismatch(functionName,
                    FuncPtrSigOfSymbol(*viable.front()), *destSig));
            }
            else
            {
                viable = std::move(bindable);
            }
        }

        const auto& overloads = viable;
        // A bare name resolves through here on the ORDINARY call path too, so the alias-param
        // rejection fires only when a function-pointer destination is actually being bound.
        const bool bindingFuncPtr = expectedParams != nullptr
            || (destSig != nullptr && !destSig->FuncPtrReturnTypeName.empty());
        auto chosen = [&](const FunctionSymbol* sym) -> llvm::Function* {
            if (bindingFuncPtr) RejectAliasParamFuncPtrBind(functionName, *sym);
            // Taking the address is as unsafe as calling it - the eventual indirect call has
            // no landing pad either.
            if (bindingFuncPtr) RejectThrowingCxxFunction(*sym, functionName);
            if (bindingFuncPtr && sym->IsCxx && sym->Function != nullptr)
                ValidateCxxDemand(sym->Function->getName().str());
            return sym->Function;
        };
        if (overloads.size() == 1)
            return chosen(overloads.front());

        // When expectedParams is provided, prefer the overload whose per-param IsMove flags match exactly.
        auto moveFlagsMatch = [&](const FunctionSymbol& sym) -> bool {
            if (!expectedParams) return true;
            if (sym.Parameters.size() != expectedParams->size()) return false;
            for (size_t i = 0; i < sym.Parameters.size(); i++)
                if (sym.Parameters[i].IsMove != (*expectedParams)[i].IsMove) return false;
            return true;
        };

        // Pass 1: non-method, param count + move flags match.
        for (const auto* sym : overloads)
        {
            if (sym->IsMethod) continue;
            if (expectedParamCount >= 0 && (int)sym->Parameters.size() != expectedParamCount) continue;
            if (!moveFlagsMatch(*sym)) continue;
            return chosen(sym);
        }
        // Pass 2: non-method, count only (legacy behavior when no expectedParams).
        for (const auto* sym : overloads)
        {
            if (sym->IsMethod) continue;
            if (expectedParamCount < 0 || (int)sym->Parameters.size() == expectedParamCount)
                return chosen(sym);
        }
        // Fallback: any overload whose effective (non-self) param count matches.
        for (const auto* sym : overloads)
        {
            int effectiveCount = sym->IsMethod ? (int)sym->Parameters.size() - 1 : (int)sym->Parameters.size();
            if (expectedParamCount < 0 || effectiveCount == expectedParamCount)
                return chosen(sym);
        }
        return chosen(overloads.front());
    }

LLVMBackend::NamedVariable LLVMBackend::GetLocalVariable(const std::string& name)
{
        for (const auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
        {
            auto& nameVal = stackFrame.namedVariable;
            auto result = nameVal.find(name);

            if (result != nameVal.end())
            {
                auto nv = result->second;
                nv.CallerName = name;
                return nv;
            }
        }

        return {};
    }

LLVMBackend::NamedVariable LLVMBackend::GetScopedLocalOrArgument(const std::string& name)
{
        for (const auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = stackFrame.namedVariable.find(name); it != stackFrame.namedVariable.end())
            {
                auto nv = it->second;
                nv.CallerName = name;
                return nv;
            }
            if (auto it = stackFrame.functionArgument.find(name); it != stackFrame.functionArgument.end())
            {
                auto nv = it->second;
                nv.CallerName = name;
                return nv;
            }
        }

        return {};
    }

bool LLVMBackend::IsFunctionParameter(const std::string& name) const
{
        if (name.empty()) return false;
        for (const auto& frame : stackNamedVariable)
            if (frame.functionArgument.find(name) != frame.functionArgument.end())
                return true;
        return false;
    }

auto LLVMBackend::FindThisArgIt(const std::map<std::string, NamedVariable>& args)
        -> std::map<std::string, NamedVariable>::const_iterator
{
        for (auto it = args.begin(); it != args.end(); ++it)
            if (it->first.ends_with("__"))
                return it;
        return args.end();
    }

const LLVMBackend::NamedVariable* LLVMBackend::FindImplicitThisField(
    const std::string& name, const StructData** outStruct, int* outIndex,
    const BitfieldInfo** outBitfield)
{
        for (const auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
        {
            const auto& functionArguments = stackFrame.functionArgument;
            if (functionArguments.empty())
                continue;

            auto thisIt = FindThisArgIt(functionArguments);
            if (thisIt == functionArguments.end())
                return nullptr;

            const auto& memberStructName = thisIt->first;
            auto truncName = memberStructName.substr(0, memberStructName.size() - 2);
            auto findResult = dataStructures.find(truncName);
            if (findResult == dataStructures.end())
                return nullptr;

            int count = 0;
            for (const auto& structField : findResult->second.StructFields)
            {
                if (structField.VariableName == name)
                {
                    if (outStruct != nullptr) *outStruct = &findResult->second;
                    if (outIndex != nullptr)  *outIndex = count;
                    return &thisIt->second;
                }
                count++;
            }
            // Bitfields are not in StructFields (PackBitfields folds them into synthesized
            // storage slots), so the declared name only appears in the Bitfields side-table.
            for (const auto& bf : findResult->second.Bitfields)
            {
                if (bf.Name == name)
                {
                    if (outStruct != nullptr)   *outStruct = &findResult->second;
                    if (outBitfield != nullptr) *outBitfield = &bf;
                    return &thisIt->second;
                }
            }
            return nullptr;
        }

        return nullptr;
    }

bool LLVMBackend::HasMemberVariable(const std::string& name)
{
        return FindImplicitThisField(name, nullptr, nullptr) != nullptr;
    }

LLVMBackend::NamedVariable LLVMBackend::GetMemberVariable(const std::string& name)
{
        const StructData* structData = nullptr;
        int count = 0;
        const BitfieldInfo* bitfield = nullptr;
        const NamedVariable* thisArg = FindImplicitThisField(name, &structData, &count, &bitfield);
        if (thisArg == nullptr)
            return {};

        // Storage is either an alloca-of-struct (constructor) or an alloca-of-ptr (method param).
        // For the latter, load through it to get the actual struct pointer before GEP.
        llvm::Value* memberStructInstance = thisArg->Storage;
        if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(memberStructInstance))
        {
            if (alloca->getAllocatedType()->isPointerTy())
                memberStructInstance = CreateLoad(alloca);
        }

        const auto& sd = *structData;
        // Bare bitfield read through the implicit 'this': GEP to the packed storage word and
        // reuse the same emitter the explicit `this.f` door uses, so read and write agree.
        if (bitfield != nullptr)
        {
            const auto& storageField = sd.StructFields[bitfield->StorageFieldIndex];
            auto* storagePtr = sd.IsUnion ? memberStructInstance
                : CreateStructGEP(sd.StructType, memberStructInstance,
                                  bitfield->StorageFieldIndex);
            return EmitBitfieldRead(storagePtr, GetType(storageField), *bitfield, "", "");
        }

        const auto& structField = sd.StructFields[count];
        NamedVariable namedVar;
        auto* fieldLLVMType = GetType(structField);
        if (sd.IsUnion)
        {
            // Promoted fields from an anonymous struct can sit beyond offset zero.
            namedVar.Storage = sd.CxxOffsetLayout && (unsigned)count < sd.CxxFieldOffsets.size()
                && sd.CxxFieldOffsets[count] != 0
                ? CreateCxxFieldGEP(sd, memberStructInstance, (unsigned)count)
                : memberStructInstance;
            namedVar.UnionFieldType = fieldLLVMType;
            namedVar.Primary = CreateLoad(fieldLLVMType, namedVar.Storage);
        }
        else
        {
            namedVar.Storage = CreateCxxFieldGEP(sd, memberStructInstance, (unsigned)count);
            namedVar.Primary = CreateLoad(fieldLLVMType, namedVar.Storage);
        }
        namedVar.BaseType = namedVar.Primary->getType();
        namedVar.TypeAndValue = structField;
        // Preserve unique-field provenance across a later cast so a bare
        // self-field read (`(Res*)p` inside a method) stays a tracked alias.
        bool isUniqueField = (structField.IsUnique && structField.Pointer)
            || (IsCoreUniqueType(structField.TypeName) && !structField.Pointer);
        if (isUniqueField)
        {
            namedVar.IsUniqueFieldAlias = true;
            RegisterUniqueFieldRead(namedVar.Primary, namedVar.Storage);
        }
        // Field declared `alignas(_, N)`: stamp the block alignment so a bare
        // `delete field` inside a member (e.g. the destructor) frees via
        // __delete_aligned. GetMemberVariable purposely omits OwningStructName.
        namedVar.AllocAlignment = structField.AllocAlignValue;
        return namedVar;
    }

LLVMBackend::NamedVariable LLVMBackend::GetCurrentMemberThis(const std::string& functionName)
{
        for (const auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
        {
            const auto& functionArguments = stackFrame.functionArgument;
            if (functionArguments.empty())
                continue;

            auto thisIt = FindThisArgIt(functionArguments);
            if (thisIt == functionArguments.end())
                break;

            const auto& thisArgName = thisIt->first;
            std::string structName = thisArgName.substr(0, thisArgName.size() - 2);

            auto funcIt = functionTable.find(functionName);
            if (funcIt == functionTable.end())
                break;

            for (const auto& sym : funcIt->second)
            {
                if (!sym.Parameters.empty() &&
                    sym.Parameters[0].TypeName == structName &&
                    sym.Parameters[0].Pointer)
                {
                    NamedVariable thisVar = thisIt->second;
                    thisVar.TypeAndValue.VariableName = "";
                    return thisVar;
                }
            }
            break;
        }
        return {};
    }

LLVMBackend::NamedVariable LLVMBackend::GetThisPointer()
{
        for (const auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
        {
            const auto& functionArguments = stackFrame.functionArgument;
            if (functionArguments.empty())
                continue;

            auto thisIt = FindThisArgIt(functionArguments);
            if (thisIt == functionArguments.end())
                break;  // first frame with arguments but no 'this' -> not a member body
            const auto& [key, nv] = *thisIt;
            std::string structName = key.substr(0, key.size() - 2);
            if (!IsDataStructure(structName))
                break;
            // Only a *method* self qualifies here: its storage is an alloca-of-pointer
            // (the incoming this* param), so loading it yields the struct pointer.
            // A constructor's self is an alloca-of-struct (value under construction);
            // leave that to the existing `this`-handling path.
            auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(nv.Storage);
            if (!alloca || !alloca->getAllocatedType()->isPointerTy())
                break;
            NamedVariable thisVar = nv;
            thisVar.CallerName = "this";
            thisVar.TypeAndValue.TypeName = structName;
            thisVar.TypeAndValue.Pointer = true;
            return thisVar;
        }
        return {};
    }

bool LLVMBackend::IsCoreUniqueType(const std::string& typeName) const
{
        std::string_view base = MangledBase(typeName);
        if (base == typeName || base != "unique" || typeName.size() <= base.size() + 1)
            return false;
        // The parser and generic-instantiation queue ask this before the wrapper struct is
        // materialized. The reserved mangling plus the imported core template is sufficient.
        return gts.coreGenericTemplates.count("unique") != 0;
}

bool LLVMBackend::IsCoreArrayType(const std::string& typeName) const
{
        std::string_view base = MangledBase(typeName);
        if (base == typeName || base != "array" || typeName.size() <= base.size() + 1)
            return false;
        return gts.coreGenericTemplates.count("array") != 0;
}

bool LLVMBackend::IsMoveOrCoreUniqueValue(const TypeAndValue& t) const
{
        return t.IsMove || (!t.Pointer && IsCoreUniqueType(t.TypeName));
}

bool LLVMBackend::IsCoreUniqueToRawPointer(const NamedVariable& arg, const TypeAndValue& param) const
{
        if (arg.TypeAndValue.Pointer) return false;
        // R5: a `unique T*` on a C++ class is std::unique_ptr<T>; it borrows to T* the same way.
        const std::string cxxPointee = CxxUniquePtrPointee(arg.TypeAndValue.TypeName);
        if (cxxPointee.empty() && (!IsCoreUniqueType(arg.TypeAndValue.TypeName)
            || MangledGenericArgument(*this, arg.TypeAndValue.TypeName).empty()))
            return false;
        const std::string argPointee = !cxxPointee.empty() ? cxxPointee
            : MangledGenericArgument(*this, arg.TypeAndValue.TypeName);
        // Interface arm: unique<IShape> borrows to a plain IShape parameter through get().
        if (!param.Pointer)
            return param.IsFatInterfaceValue()
                && argPointee == param.TypeName;
        TypeAndValue uniquePointee;
        uniquePointee.TypeName = argPointee;
        uniquePointee.Pointer = true;
        if (param.ElemPointer) return false;
        if (uniquePointee.TypeName == param.TypeName) return true;

        // Releasing an owner yields its pointee pointer first; that raw pointer then takes the
        // same public derived-to-base standard conversion as a raw pointer argument.
        if (IsCxxDerivedToBasePointer(uniquePointee, param)) return true;

        // Generic substitutions may spell the same C-equivalent pointee as `i32` in
        // unique<T> and `int` in the instantiated method parameter. Compare their value
        // identities through the normal integer-width rules before applying the pointer
        // adaptation, so all generic methods share this borrow path.
        TypeAndValue uniquePointeeValue;
        uniquePointeeValue.TypeName = uniquePointee.TypeName;
        TypeAndValue paramPointee = param;
        paramPointee.Pointer = false;
        paramPointee.ElemPointer = false;
        return uniquePointeeValue.IsTypeMatch(paramPointee);
}

bool LLVMBackend::IsRawPointerToCoreUnique(const NamedVariable& arg, const TypeAndValue& param) const
{
        if (param.Pointer || !IsCoreUniqueType(param.TypeName)
            || MangledGenericArgument(*this, param.TypeName).empty())
            return false;
        const std::string paramPointee = MangledGenericArgument(*this, param.TypeName);
        bool nullPointer = arg.Primary != nullptr && llvm::isa<llvm::ConstantPointerNull>(arg.Primary);
        if (!nullPointer && arg.Primary == nullptr && arg.TypeAndValue.TypeName.empty()
            && arg.Storage != nullptr && arg.BaseType != nullptr && arg.BaseType->isPointerTy())
            nullPointer = true;
        if (nullPointer)
            return true;
        // Interface arm: unique<IShape> is constructed from any implementor pointer or from an
        // IShape value, exactly as the explicit `unique<IShape>(new Sq())` spelling resolves.
        if (IsInterfaceType(paramPointee))
        {
            const std::string ifaceName = paramPointee;
            if (arg.TypeAndValue.TypeName == ifaceName) return true;
            if (arg.TypeAndValue.Pointer
                && StructImplementsInterface(arg.TypeAndValue.TypeName, ifaceName))
                return true;
            std::string elemName = FindValueElementTypeName(arg.Primary);
            if (!elemName.empty()
                && (elemName == ifaceName || StructImplementsInterface(elemName, ifaceName)))
                return true;
            if (const auto* owned = FindOwnedNewTemp(arg.Primary);
                owned != nullptr && StructImplementsInterface(owned->TypeName, ifaceName))
                return true;
            return false;
        }
        const TypeAndValue uniquePointee = [&]() {
            TypeAndValue value;
            value.TypeName = paramPointee;
            value.Pointer = true;
            return value;
        }();
        auto matchesRawPointer = [&](llvm::Value* value) {
            if (value == nullptr) return false;
            if (FindValueElementTypeName(value) == uniquePointee.TypeName) return true;
            if (const auto* owned = FindOwnedNewTemp(value);
                owned != nullptr && owned->TypeName == uniquePointee.TypeName)
                return true;
            auto* expectedType = GetType(uniquePointee);
            return expectedType != nullptr && value->getType() == expectedType;
        };
        const auto* nullJoin = FindNullCoalesceJoin(arg.Primary);
        if (!arg.TypeAndValue.Pointer && nullJoin == nullptr)
            return false;
        if (!arg.TypeAndValue.Pointer)
        {
            for (const auto& arm : nullJoin->Arms)
                if (!llvm::isa_and_nonnull<llvm::ConstantPointerNull>(arm.Value)
                    && matchesRawPointer(arm.Value))
                    return true;
            return false;
        }
        if (uniquePointee.IsTypeMatch(arg.TypeAndValue)) return true;
        if (matchesRawPointer(arg.Primary)) return true;
        // Fresh `new T()` expressions have no source spelling on their NamedVariable. Their LLVM
        // pointer type still proves the same pointee shape for this ownership constructor.
        auto* expectedType = GetType(uniquePointee);
        return expectedType != nullptr
            && ((arg.BaseType != nullptr && arg.BaseType == expectedType)
                || (arg.Primary != nullptr && arg.Primary->getType() == expectedType));
}

bool LLVMBackend::IsStackValueToCoreUniqueInterface(const NamedVariable& arg,
                                                    const TypeAndValue& param) const
{
        if (param.Pointer || arg.TypeAndValue.Pointer || arg.TypeAndValue.IsInterface)
            return false;
        if (!IsCoreUniqueType(param.TypeName) || param.TypeName.size() <= 8)
            return false;
        const std::string ifaceName = MangledGenericArgument(*this, param.TypeName);
        if (!IsInterfaceType(ifaceName) || arg.TypeAndValue.TypeName.empty())
            return false;
        return StructImplementsInterface(arg.TypeAndValue.TypeName, ifaceName);
}

llvm::Value* LLVMBackend::CreateCoreUniqueFromRawPointerCall(
    const NamedVariable& arg, const TypeAndValue& param)
{
        if (arg.IsExplicitMove && !arg.BorrowedUniqueField.empty())
            LogErrorMessage(
                "cannot 'move' '{}' - it was returned as a borrow of unique field '{}', and moving "
                "the alias does not null the field, so the field's synthesized destructor still "
                "frees the pointee. Pass it to a plain (non-'move') parameter, which borrows, or "
                "pass a value the receiver can own.",
                { arg.CallerName.empty() ? arg.BorrowedOrigin : arg.CallerName,
                  arg.BorrowedUniqueField });
        auto receiver = arg;
        if (receiver.Primary != nullptr && receiver.Primary->getType()->isPointerTy())
            receiver.Primary = AdjustCxxPointerForUniqueAdoption(
                receiver, param, receiver.Primary, "unique pointer adoption");
        // A plain factory result has no owner marker; treat it as the ownership handoff while
        // still rejecting named or explicitly borrowed pointers.
        if (receiver.Storage == nullptr && !receiver.IsBorrowed && !receiver.IsAliasBorrow
            && !receiver.TypeAndValue.IsAlias)
            receiver.IsOwning = true;
        receiver.TypeAndValue.VariableName.clear();
        receiver.IsExplicitMove = false;
        return CreateOverloadedFunctionCall(param.TypeName, { receiver }, true);
}

llvm::Value* LLVMBackend::CreateCoreUniqueRawPointerCall(
    const NamedVariable& arg, const TypeAndValue& param, bool calleeIsCxx)
{
        bool foreignCxxPointerSink = calleeIsCxx && param.Pointer && !param.IsAlias
            && !param.IsRvalueRef && !param.IsCxxRefToPointer && !param.IsCxxConstRef;
        bool consumesCoreUnique = arg.IsExplicitMove
            && (IsMoveOrCoreUniqueValue(param) || foreignCxxPointerSink);
        bool carriesTempUniqueField = JoinCarriesOwningTempUniqueField(arg.Primary)
            || (arg.FromOwningTempField && arg.OwningTempParent);
        if (consumesCoreUnique && !arg.CallerName.empty())
        {
            RecordNullSet(arg.CallerName);
            MarkVariableExplicitlyMovedNull(arg.CallerName);
        }
        auto receiver = arg;
        // Resolve the wrapper observer/release method through a pointer receiver so a pointee
        // method named `get` cannot win overload scoring.
        auto* wrapperTy = GetType(arg.TypeAndValue);
        if (receiver.Storage != nullptr)
        {
            receiver.Primary = receiver.Storage;
            receiver.Storage = nullptr;
        }
        else if (receiver.Primary != nullptr && wrapperTy != nullptr
                 && receiver.Primary->getType() == wrapperTy)
        {
            auto* wrapperStorage = AllocaAtEntry(wrapperTy, nullptr, "unique.receiver");
            builder->CreateStore(receiver.Primary, wrapperStorage);
            receiver.Primary = wrapperStorage;
        }
        receiver.BaseType = wrapperTy == nullptr ? receiver.BaseType
            : cflat_llvm::PointerTo(wrapperTy);
        receiver.TypeAndValue.Pointer = true;
        receiver.TypeAndValue.VariableName.clear();
        receiver.IsExplicitMove = false;
        EnsureCxxMemberProjected(arg.TypeAndValue.TypeName,
                                 consumesCoreUnique ? "release" : "get");
        auto* result = CreateOverloadedFunctionCall(consumesCoreUnique ? "release" : "get", { receiver });
        // The getter is an ABI adapter, not a new ownership boundary. Preserve the temporary
        // field ledger across it so a later call/return still rejects the escaping raw pointer.
        if (carriesTempUniqueField)
            RegisterOwningTempUniqueField(result);
        // Keep the provenance link so a diagnostic can still name the source field.
        if (result != nullptr && arg.Primary != nullptr)
            coreUniqueGetterSource_[result] = arg.Primary;
        return result;
}

LLVMBackend::NamedVariable LLVMBackend::GetFunctionArgument(std::string name)
{
        for (const auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
        {
            const auto& nameVal = stackFrame.functionArgument;
            auto result = nameVal.find(name);

            if (result != nameVal.end())
            {
                auto nv = result->second;
                nv.CallerName = name;
                return nv;
            }
        }

        return {};
    }

size_t LLVMBackend::FindVariableScopeDepth(const std::string& name) const
{
        size_t depth = stackNamedVariable.size();
        for (const auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            --depth;
            if (frame.functionArgument.count(name) || frame.namedVariable.count(name))
                return depth;
        }
        return SIZE_MAX;
    }

std::string LLVMBackend::FindActiveBondBorrower(const std::string& sourceName) const
{
        for (const auto& frame : stackNamedVariable)
        {
            for (const auto& [name, nv] : frame.namedVariable)
            {
                if (nv.IsBonded && !nv.BondByAddress)
                {
                    for (const auto& src : nv.BondedSources)
                    {
                        if (src == sourceName)
                            return name;
                    }
                }
            }
        }
        return {};
    }

void LLVMBackend::ClearVariableBond(const std::string& name)
{
        auto* here = builder != nullptr ? builder->GetInsertBlock() : nullptr;
        auto* function = here != nullptr ? here->getParent() : nullptr;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
            {
                // A rebind in a nested control-flow block is not known to run before every later
                // use of the bonded value. Retire only a same-block store.
                if (it->second.BondDeclBlock == nullptr
                    || it->second.BondDeclBlock != here
                    || it->second.BondDeclFunction != function)
                    return;
                it->second.IsBonded = false;
                it->second.BondedSources.clear();
                it->second.BondDeclBlock = nullptr;
                it->second.BondDeclFunction = nullptr;
                return;
            }
        }
    }

bool LLVMBackend::IsBorrowingContainerElementSink(const std::string& functionName,
        const std::vector<TypeAndValue>& params, size_t paramIndex, bool isMethod) const
{
        // The receiver occupies params[0], so an element slot is never index 0. In every core
        // element-storing method the element is the LAST parameter (`set`/`insert`/dictionary
        // `add` take the index/key first), which pins the slot without a per-method table.
        if (!isMethod || paramIndex == 0 || params.size() < 2) return false;
        if (paramIndex != params.size() - 1) return false;

        std::string receiver = params[0].TypeName;
        if (!params[0].Pointer) return false;
        std::string_view qualifiedView = MangledBase(receiver);
        if (qualifiedView == receiver) return false;   // not a generic instantiation
        // The template key keeps its namespace ("mylib.list"); the method table below is keyed
        // on the bare name.
        std::string qualified(qualifiedView);
        std::string base = qualified;
        if (size_t dot = base.rfind('.'); dot != std::string::npos)
            base = base.substr(dot + 1);
        // Origin gate: only a template DECLARED in a core library file has the borrow semantics
        // this predicate encodes. A user-defined `stack<T>` (or `mylib.list<T>`) owns whatever
        // its own code says it owns and is none of this rule's business.
        if (gts.coreGenericTemplates.count(qualified) == 0) return false;

        // The element-storing methods, exactly as core/list.cb, dictionary.cb, queue.cb and
        // stack.cb declare them. hashset's `add(alias T value)` is deliberately absent: it
        // declares its parameter a borrow and offers no `move` overload for a pointer element,
        // so there would be no remedy to name.
        bool storing =
            (base == "list"       && (functionName == "add" || functionName == "set"
                                      || functionName == "insert"))
            || (base == "dictionary" && (functionName == "add" || functionName == "set"))
            || (base == "queue"      && functionName == "enqueue")
            || (base == "stack"      && functionName == "push");
        if (!storing) return false;

        // Bare pointer element only. `unique` (a real sink), `alias` (the opt-in borrow
        // spelling), an interface fat value and any by-value element all answer false.
        const TypeAndValue& elem = params[paramIndex];
        if (!elem.Pointer || elem.IsArrayView) return false;
        if (elem.IsInterfacePointer || elem.IsFatInterfaceValue() || elem.IsFunctionPointer)
            return false;
        if (elem.IsMove || elem.IsAlias || elem.IsUnique) return false;
        if (elem.IsBorrowOfAliasElement) return false;
        return true;
    }

bool LLVMBackend::IsProvenOwningNamedLocal(const NamedVariable& arg) const
{
        if (arg.IsExplicitMove || arg.TypeAndValue.IsMove) return false;
        if (!arg.TypeAndValue.Pointer || arg.TypeAndValue.IsArrayView) return false;
        if (arg.TypeAndValue.IsUnique) return false;
        if (arg.IsBorrowed || arg.IsAliasBorrow || arg.BorrowsOwnedElement) return false;
        if (!arg.IsOwning) return false;
        // Not live any more: already transferred, or handed to an interface box.
        if (arg.IsMoved || arg.ExplicitlyMovedNull || arg.MovedIntoInterface) return false;
        // A NAMED local of this frame: an rvalue (`new B()`) has no name, a global/static has
        // non-alloca storage, and a parameter (borrowed or `move`) is the caller's business.
        if (arg.CallerName.empty() || !arg.FieldName.empty()) return false;
        if (arg.IsStaticLocal) return false;
        if (arg.Storage == nullptr || !llvm::isa<llvm::AllocaInst>(arg.Storage)) return false;
        if (IsFunctionParameter(arg.CallerName)) return false;
        // The binding must still be the owning one the flags describe.
        if (!IsVariableOwning(arg.CallerName)) return false;
        // A `unique T*` local is an owner with a DECLARED policy: it is the spelling the ruling
        // keeps for "this object is owned here and the container borrows it", so a borrow-add
        // from one is the sanctioned borrow-collection shape and stays legal. The written
        // qualifier lives on the DECLARATION - a read hands out a plain pointer value.
        if (const NamedVariable* decl = FindVariableByStorage(arg.Storage))
            if (decl->TypeAndValue.IsUnique) return false;

        return true;
    }

bool LLVMBackend::IsBarePointerParameter(const TypeAndValue& param) const
{
        if (!param.Pointer || param.IsArrayView) return false;
        if (param.IsInterfacePointer || param.IsFatInterfaceValue() || param.IsFunctionPointer)
            return false;
        if (param.IsMove || param.IsAlias || param.IsUnique) return false;
        if (param.IsBorrowOfAliasElement) return false;
        return true;
    }

bool LLVMBackend::RejectOwningLocalIntoBorrowingContainer(const std::string& functionName,
        const std::vector<TypeAndValue>& params, size_t paramIndex, bool isMethod,
        const NamedVariable& arg)
{
        if (!IsBorrowingContainerElementSink(functionName, params, paramIndex, isMethod))
            return false;
        if (!IsProvenOwningNamedLocal(arg)) return false;

        LogErrorMessage(
            "call to '{}': '{}' still owns the object it was given, and frees it when it goes out "
            "of scope - but this container only BORROWS its elements and never frees them, so the "
            "stored element would dangle. Transfer the object with '{}', or declare the container's "
            "element '{}' so the container owns it.",
            { SpellFunctionSymbol(*this, functionName), arg.CallerName,
              std::format("{}({} {})", SpellFunctionSymbol(*this, functionName), "move", arg.CallerName),
              "unique T*" });
        return true;
    }

void LLVMBackend::RejectOwningLocalIntoBorrowingHelper(const std::string& functionName,
        const FunctionSymbol& callee, size_t paramIndex, const NamedVariable& arg)
{
        if (callee.Function == nullptr || paramIndex >= callee.Parameters.size()) return;
        if (callee.IsMethod && paramIndex == 0) return;
        if (!IsBarePointerParameter(callee.Parameters[paramIndex])
            || !IsProvenOwningNamedLocal(arg)) return;
        auto llvmIndex = CFlatParameterLLVMIndex(callee, (unsigned)paramIndex);
        if (!llvmIndex.has_value()) return;
        if (!FunctionBodyIsComplete(callee.Function))
        {
            owningLocalBorrowingHelperArgs_.push_back({
                callee.Function, *llvmIndex, functionName,
                callee.Parameters[paramIndex].VariableName, arg.CallerName,
                sourceFileName, currentLine, currentColumn });
            return;
        }
        if (!ParameterMayReachBorrowingContainerSink(callee.Function, *llvmIndex)) return;
        LogErrorMessage(
            "call to '{}': '{}' still owns the object it was given, and the helper parameter '{}' "
            "stores it into a container that only BORROWS its elements and never frees them, so "
            "the stored element would dangle. Declare parameter '{}' as 'move' and call '{}(move "
            "{})', or declare the container's element 'unique T*'.",
            { SpellFunctionSymbol(*this, functionName), arg.CallerName,
              callee.Parameters[paramIndex].VariableName, callee.Parameters[paramIndex].VariableName,
              SpellFunctionSymbol(*this, functionName), arg.CallerName });
    }

void LLVMBackend::MarkVariableMoved(const std::string& name)
{
        if (name.empty()) return;
        RecordMoveKill(name);
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                { it->second.IsMoved = true; return; }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                { it->second.IsMoved = true; return; }
        }
    }

void LLVMBackend::MarkVariableMovedIntoInterface(const std::string& name)
{
        if (name.empty()) return;
        RecordMoveKill(name);
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                { it->second.MovedIntoInterface = true; return; }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                { it->second.MovedIntoInterface = true; return; }
        }
    }

void LLVMBackend::SetViewOfFixedArrayStorage(const std::string& name, bool value, const std::string& sourceName)
{
        if (name.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
            {
                it->second.ViewOfFixedArrayStorage = value;
                it->second.ViewOfFixedArraySourceName = value ? sourceName : std::string();
                return;
            }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
            {
                it->second.ViewOfFixedArrayStorage = value;
                it->second.ViewOfFixedArraySourceName = value ? sourceName : std::string();
                return;
            }
        }
    }

void LLVMBackend::SetInterfaceBoxIsBorrowed(const std::string& name, bool borrowed,
                                   const std::string& sourceName)
{
        if (name.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            NamedVariable* nv = nullptr;
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                nv = &it->second;
            else if (auto it2 = frame.functionArgument.find(name); it2 != frame.functionArgument.end())
                nv = &it2->second;
            if (nv == nullptr) continue;
            if (!borrowed)
            {
                nv->InterfaceBoxProvenanceUnknown = true;
                nv->BorrowedInterfaceBox = false;
                nv->BorrowedInterfaceBoxSource.clear();
                nv->BorrowedInterfaceBoxSlots.clear();
                return;
            }
            if (nv->InterfaceBoxProvenanceUnknown) return;
            nv->BorrowedInterfaceBox = true;
            nv->BorrowedInterfaceBoxSource = sourceName;
            return;
        }
    }

void LLVMBackend::SetInterfaceBoxBorrowSlots(const std::string& name,
                                              const std::vector<llvm::Value*>& slots)
{
        if (name.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            NamedVariable* nv = nullptr;
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                nv = &it->second;
            else if (auto it2 = frame.functionArgument.find(name); it2 != frame.functionArgument.end())
                nv = &it2->second;
            if (nv == nullptr) continue;
            nv->BorrowedInterfaceBoxSlots = slots;
            return;
        }
    }

void LLVMBackend::MarkPointerRebound(const std::string& name, const std::string& inheritedOwner,
                            bool coalesceJoin, bool reboundToOwnedValue)
{
        if (name.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            NamedVariable* nv = nullptr;
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                nv = &it->second;
            else if (auto it2 = frame.functionArgument.find(name); it2 != frame.functionArgument.end())
                nv = &it2->second;
            if (nv == nullptr) continue;
            nv->PointerRebound = true;
            // Recorded, not consulted, here: a retiring consumer must also prove the store was
            // reached (same basic block), which is why the block travels with the bit.
            nv->ReboundToOwnedValue = reboundToOwnedValue;
            nv->ReboundBlock = reboundToOwnedValue ? builder->GetInsertBlock() : nullptr;
            nv->ReboundFunction = nv->ReboundBlock != nullptr ? nv->ReboundBlock->getParent() : nullptr;
            nv->InheritedKeepsOwner = !inheritedOwner.empty();
            nv->InheritedKeepsOwnerSource = inheritedOwner;
            nv->CoalesceRebound = coalesceJoin;
            // Written unconditionally so a plain '=' retires whatever join a declaration (or an
            // earlier store) recorded; SetJoinKeepsOwner re-arms it when THIS RHS is such a join.
            nv->JoinKeepsOwner = false;
            nv->JoinKeepsOwnerSource.clear();
            nv->JoinKeepsOwnerSlots.clear();
            return;
        }
    }

void LLVMBackend::SetJoinKeepsOwner(const std::string& name, const std::string& owner,
                           const std::vector<llvm::Value*>& slots)
{
        if (name.empty() || owner.empty() || slots.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            NamedVariable* nv = nullptr;
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                nv = &it->second;
            else if (auto it2 = frame.functionArgument.find(name); it2 != frame.functionArgument.end())
                nv = &it2->second;
            if (nv == nullptr) continue;
            nv->JoinKeepsOwner = true;
            nv->JoinKeepsOwnerSource = owner;
            nv->JoinKeepsOwnerSlots = slots;
            return;
        }
    }

void LLVMBackend::RecordAssignBorrow(const std::string& name, const std::string& origin,
                           const std::string& uniqueField, bool throughField, bool keepExistingOrigin,
                           bool uniqueFieldViaCall)
{
        if (name.empty() || origin.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            NamedVariable* nv = nullptr;
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                nv = &it->second;
            else if (auto it2 = frame.functionArgument.find(name); it2 != frame.functionArgument.end())
                nv = &it2->second;
            if (nv == nullptr) continue;
            // A binding that already owns what it holds is not made a borrow by this store; the
            // declaration path skips the same two cases for the same reason.
            if (nv->IsOwning || nv->IsNewAllocated) return;
            // A '??=' keeps the OLD referent when its arm is not taken, so it may not overwrite the
            // origin an existing borrow names - only supply one where there was none.
            if (keepExistingOrigin && nv->IsBorrowed && !nv->BorrowedOrigin.empty()) return;
            nv->BorrowsOwningLocal = false;
            nv->OwningLocalOrigin.clear();
            nv->OwningLocalStorage = nullptr;
            nv->OwningLocalBorrowAfterRebind = false;
            nv->IsBorrowed = true;
            nv->BorrowedOrigin = origin;
            nv->BorrowedUniqueField = uniqueField;
            nv->BorrowedUniqueFieldViaCall = uniqueFieldViaCall;
            nv->BorrowedThroughField = throughField;
            nv->AssignBorrowBlock = builder->GetInsertBlock();
            return;
        }
    }

void LLVMBackend::RecordAssignOwningLocalBorrow(const std::string& name,
                                       const std::string& origin,
                                       llvm::Value* ownerStorage)
{
        if (name.empty() || origin.empty() || ownerStorage == nullptr) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            NamedVariable* nv = nullptr;
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                nv = &it->second;
            else if (auto it2 = frame.functionArgument.find(name); it2 != frame.functionArgument.end())
                nv = &it2->second;
            if (nv == nullptr) continue;
            if (nv->IsOwning || nv->IsNewAllocated) return;
            nv->IsBorrowed = false;
            nv->BorrowedOrigin.clear();
            nv->BorrowedUniqueField.clear();
            nv->BorrowedUniqueFieldViaCall = false;
            nv->BorrowedThroughField = false;
            nv->BorrowsOwningLocal = true;
            nv->OwningLocalOrigin = origin;
            nv->OwningLocalStorage = ownerStorage;
            const auto* owner = FindVariableByStorage(ownerStorage);
            nv->OwningLocalBorrowAfterRebind = owner != nullptr && owner->PointerRebound;
            nv->AssignBorrowBlock = builder->GetInsertBlock();
            return;
        }
    }

void LLVMBackend::SetPointsToBorrowedByValueParam(const std::string& name, bool value)
{
        if (name.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            NamedVariable* nv = nullptr;
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                nv = &it->second;
            else if (auto it2 = frame.functionArgument.find(name); it2 != frame.functionArgument.end())
                nv = &it2->second;
            if (nv == nullptr) continue;
            nv->PointsToBorrowedByValueParam = value;
            return;
        }
    }

void LLVMBackend::SetPointsToBorrowedAddress(const std::string& name, bool value)
{
        if (name.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            NamedVariable* nv = nullptr;
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                nv = &it->second;
            else if (auto it2 = frame.functionArgument.find(name); it2 != frame.functionArgument.end())
                nv = &it2->second;
            if (nv == nullptr) continue;
            nv->PointsToBorrowedAddress = value;
            return;
    }
}

void LLVMBackend::SetStackCharBufferBorrow(const std::string& name, bool value,
                                           const std::string& source, size_t scopeDepth)
{
        if (name.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            NamedVariable* nv = nullptr;
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                nv = &it->second;
            else if (auto it2 = frame.functionArgument.find(name); it2 != frame.functionArgument.end())
                nv = &it2->second;
            if (nv == nullptr) continue;
            nv->StackCharBufferBorrow = value;
            nv->StackCharBufferSource = value ? source : std::string();
            nv->StackCharBufferScopeDepth = value ? scopeDepth : 0;
            return;
        }
}

void LLVMBackend::RetireAssignBorrow(const std::string& name)
{
        if (name.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            NamedVariable* nv = nullptr;
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                nv = &it->second;
            else if (auto it2 = frame.functionArgument.find(name); it2 != frame.functionArgument.end())
                nv = &it2->second;
            if (nv == nullptr) continue;
            if (nv->AssignBorrowBlock == nullptr
                || nv->AssignBorrowBlock != builder->GetInsertBlock())
                return;
            nv->IsBorrowed = false;
            nv->BorrowedOrigin.clear();
            nv->BorrowedUniqueField.clear();
            nv->BorrowedUniqueFieldViaCall = false;
            nv->BorrowedThroughField = false;
            nv->BorrowsOwningLocal = false;
            nv->OwningLocalOrigin.clear();
            nv->OwningLocalStorage = nullptr;
            nv->OwningLocalBorrowAfterRebind = false;
            nv->AssignBorrowBlock = nullptr;
            return;
        }
    }

bool LLVMBackend::IsFunctionParameterStorage(const llvm::Value* slot) const
{
        if (slot == nullptr) return false;
        for (const auto& frame : stackNamedVariable)
            for (const auto& [varName, nv] : frame.functionArgument)
                if (nv.Storage == slot) return true;
        return false;
    }

void LLVMBackend::MarkVariableUnmoved(const std::string& name)
{
        if (name.empty()) return;
        RecordMoveGenRevive(name);
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                { it->second.IsMoved = false; return; }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                { it->second.IsMoved = false; return; }
        }
    }

void LLVMBackend::MarkVariableExplicitlyMovedNull(const std::string& name)
{
        if (name.empty() || !builder) return;
        llvm::BasicBlock* bb = builder->GetInsertBlock();
        if (!bb) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
            {
                if (it->second.AddressEscaped) return;
                it->second.ExplicitlyMovedNull = true;
                it->second.ExplicitNullBlock = bb;
                EnsureConditionalDropFlag(it->second);
                if (it->second.ConditionalDropFlag != nullptr)
                    builder->CreateStore(builder->getInt1(false), it->second.ConditionalDropFlag);
                RecordNullSet(name);
                return;
            }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
            {
                if (it->second.AddressEscaped) return;
                it->second.ExplicitlyMovedNull = true;
                it->second.ExplicitNullBlock = bb;
                EnsureConditionalDropFlag(it->second);
                if (it->second.ConditionalDropFlag != nullptr)
                    builder->CreateStore(builder->getInt1(false), it->second.ConditionalDropFlag);
                RecordNullSet(name);
                return;
            }
        }
    }

void LLVMBackend::MarkVariableNotExplicitlyMovedNull(const std::string& name)
{
        if (name.empty()) return;
        RecordNullClear(name);
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                {
                    it->second.ExplicitlyMovedNull = false;
                    it->second.ExplicitNullBlock = nullptr;
                    RearmConditionalDropFlag(it->second);
                    return;
                }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                {
                    it->second.ExplicitlyMovedNull = false;
                    it->second.ExplicitNullBlock = nullptr;
                    RearmConditionalDropFlag(it->second);
                    return;
                }
        }
    }

void LLVMBackend::MarkVariableAddressEscaped(const std::string& name)
{
        if (name.empty()) return;
        RecordNullEscape(name);
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
            {
                it->second.AddressEscaped = true;
                it->second.ExplicitlyMovedNull = false;
                it->second.ExplicitNullBlock = nullptr;
                return;
            }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
            {
                it->second.AddressEscaped = true;
                it->second.ExplicitlyMovedNull = false;
                it->second.ExplicitNullBlock = nullptr;
                return;
            }
        }
    }

bool LLVMBackend::IsExplicitlyMovedNullHere(const NamedVariable& nv) const
{
        return nv.ExplicitlyMovedNull && !nv.AddressEscaped && suppressExplicitNullDerefGuard_ == 0
            && builder && nv.ExplicitNullBlock == builder->GetInsertBlock();
    }

void LLVMBackend::RecordNullDerefFor(const NamedVariable& nv, int line, int col)
{
        if (nv.CallerName.empty() || !nv.FieldName.empty() || nv.AddressEscaped) return;
        if (!(nv.ExplicitlyMovedNull || nv.IsOwning
              || nv.TypeAndValue.IsUnique
              || IsCoreUniqueType(nv.TypeAndValue.TypeName)))
            return;
        RecordNullDeref(nv.CallerName, line, col);
    }

void LLVMBackend::MarkVariableFieldMoved(const std::string& name, const std::string& field)
{
        if (name.empty() || field.empty()) return;
        RecordMoveKillField(name, field);
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                { it->second.MovedFields.insert(field); return; }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                { it->second.MovedFields.insert(field); return; }
        }
    }

void LLVMBackend::MarkVariableFieldUnmoved(const std::string& name, const std::string& field)
{
        if (name.empty() || field.empty()) return;
        RecordMoveGenReviveField(name, field);
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                { it->second.MovedFields.erase(field); return; }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                { it->second.MovedFields.erase(field); return; }
        }
    }

std::string LLVMBackend::MovedUseSubject(const NamedVariable& nv) const
{
        if (nv.IsMoved) return nv.CallerName;
        if (!nv.FieldName.empty() && nv.MovedFields.count(nv.FieldName))
            return nv.CallerName + "." + nv.FieldName;
        if (!nv.CallerName.empty())
        {
            for (const auto& frame : std::ranges::reverse_view(stackNamedVariable))
            {
                const NamedVariable* binding = nullptr;
                if (auto it = frame.namedVariable.find(nv.CallerName); it != frame.namedVariable.end())
                    binding = &it->second;
                else if (auto it = frame.functionArgument.find(nv.CallerName);
                         it != frame.functionArgument.end())
                    binding = &it->second;
                if (binding == nullptr) continue;
                if (binding->IsMoved) return nv.CallerName;
                if (!nv.FieldName.empty() && binding->MovedFields.count(nv.FieldName))
                    return nv.CallerName + "." + nv.FieldName;
                break;
            }
        }
        return "";
}

/*
 * The sret temporary holding a lowered CFlat struct call result, when `value` is that result:
 * the most recent one, or any earlier result of the same full expression (a load from its own
 * sret slot that is still a produced temporary). Null otherwise.
 */
llvm::AllocaInst* LLVMBackend::LoweredSretTempOf(llvm::Value* value, llvm::Type* type) const
{
        if (value == nullptr || type == nullptr) return nullptr;
        llvm::AllocaInst* slot = nullptr;
        if (value == lastLoweredRetValue_)
            slot = llvm::dyn_cast_or_null<llvm::AllocaInst>(lastLoweredRetTemp_);
        else if (auto* load = llvm::dyn_cast<llvm::LoadInst>(value);
                 load != nullptr && IsProducedTempValue(value))
            slot = llvm::dyn_cast<llvm::AllocaInst>(load->getPointerOperand());
        if (slot == nullptr || slot->getAllocatedType() != type) return nullptr;
        return slot;
}
