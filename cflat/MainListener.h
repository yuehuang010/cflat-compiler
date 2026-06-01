#pragma once
// ============================================================
// MainListener.h - CFlat front-end: ForwardRefScanner + MainListener
// ============================================================
// SECTION         LINE     DESCRIPTION
// ───────────────────────────────────────────────────────────
// §1              15-67    File-level helpers
// §2              72-195   ForwardRefScanner class (pre-pass)
// §3              200-517  MainListener class (code generation)
//   §3.1  591             ParseDeclarationSpecifiers (codegen)
//   §3.2  847             Interface/generic instantiation
//   §3.3 1020             Top-level declarations
//   §3.4 1206             Statement parsing
//   §3.5 1858             Function/parameter declarations
//   §3.6 2017             Expression parsing
//   §3.7 3476             ParsePostfixExpression (~797 lines)
//   §3.8 4838             Generic instantiation queue
//   §3.9 5011             Struct/Class definitions
//   §3.10 5571            Constructor/Destructor
//   §3.11 5686            Utilities
// ============================================================

#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <format>
#include <variant>
#include <optional>
#include <cstdlib>

#include "CFlatParser.h"
#include "CFlatLexer.h"
#include "CFlatBaseListener.h"
#include "LLVMBackend.h"
#include "LspSymbolIndex.h"

// Returns true when a function's entire body is a single 'return { ... };' statement,
// marking it as a return-block function (to be inlined at every call site).
static bool IsReturnBlockFunction(CFlatParser::FunctionDefinitionContext* func)
{
    auto* cs = func->compoundStatement();
    if (!cs) return false;
    auto* bil = cs->blockItemList();
    if (!bil) return false;
    auto items = bil->blockItem();
    if (items.size() != 1) return false;
    auto* stmt = items[0]->statement();
    if (!stmt) return false;
    auto* jump = stmt->jumpStatement();
    if (!jump) return false;
    return jump->Return() != nullptr && jump->compoundStatement() != nullptr;
}

inline std::string getOperatorName(CFlatParser::OperatorFunctionIdContext* opId)
{
    if (opId->New())          return "operator new";
    if (opId->Delete())       return "operator delete";
    if (opId->String())       return "operator string";
    if (opId->Plus())         return "operator+";
    if (opId->Minus())        return "operator-";
    if (opId->Star())         return "operator*";
    if (opId->Div())          return "operator/";
    if (opId->Mod())          return "operator%";
    if (opId->Equal())        return "operator==";
    if (opId->NotEqual())     return "operator!=";
    if (opId->Less())         return "operator<";
    if (opId->LessEqual())    return "operator<=";
    if (opId->GreaterEqual()) return "operator>=";
    if (opId->LeftShift())    return "operator<<";
    // '>>' is two Greater tokens; '>' is one. Check before single-Greater.
    if (opId->Greater().size() == 2) return "operator>>";
    if (opId->Greater().size() == 1) return "operator>";
    if (opId->LeftBracket())  return "operator[]";
    if (opId->Not())          return "operator!";
    if (opId->Tilde())        return "operator~";
    return "";
}

inline std::string getInterfaceMethodName(CFlatParser::InterfaceMethodContext* m)
{
    if (auto* opId = m->operatorFunctionId())
        return getOperatorName(opId);
    return m->directDeclarator()->getText();
}

// Extract the plain identifier from a directDeclarator (handles both bare names and C-style array syntax).
static std::string getDirectDeclName(CFlatParser::DirectDeclaratorContext* d)
{
    if (!d) return "";
    // C-style array form: (Identifier | Move) '[' assignmentExpression ']'
    if (d->assignmentExpression())
        return d->Identifier() ? d->Identifier()->getText() : "move";
    return d->getText();
}

// Normalize a generic type argument for use in mangled names (e.g. "Employee*" -> "Employeeptr").
static std::string MangleTypeArg(const std::string& typeName)
{
    std::string result;
    for (char c : typeName)
        if (c == '*') result += "ptr";
        else result += c;
    return result;
}

// Extract function name from FunctionDefinitionContext (handles operator overloads).
static std::string getFunctionName(CFlatParser::FunctionDefinitionContext* ctx)
{
    if (auto* opId = ctx->operatorFunctionId())
        return ::getOperatorName(opId);
    auto directDecl = ctx->directDeclarator();
    return directDecl->getText();
}

// Extract the leading doc comment block above the given declaration's start token.
// Tier 1 (preferred): contiguous /// or /** ... */ doc comments anchored to the declaration.
// Tier 2 (fallback): contiguous // or /* ... */ plain comments anchored to the declaration.
// "Anchored" means no blank line between the comment block and the declaration, and no
// blank line between adjacent comments in the chain.
// Returns markdown-ready text with comment markers stripped, or empty string on no match.
static std::string ExtractLeadingDoc(antlr4::BufferedTokenStream* tokens, antlr4::Token* declStart)
{
    if (!tokens || !declStart) return "";
    size_t idx = declStart->getTokenIndex();
    auto hidden = tokens->getHiddenTokensToLeft(idx);
    if (hidden.empty()) return "";

    struct Item { antlr4::Token* tok; int startLine; int endLine; bool isDoc; bool isLine; };
    auto countNewlines = [](const std::string& s) {
        int n = 0;
        for (char c : s) if (c == '\n') ++n;
        return n;
    };
    auto classify = [&](antlr4::Token* t) -> std::optional<Item> {
        const std::string& text = t->getText();
        if (text.size() < 2) return std::nullopt;
        Item it{};
        it.tok = t;
        it.startLine = (int)t->getLine();
        it.endLine = it.startLine + countNewlines(text);
        if (text.compare(0, 2, "//") == 0)
        {
            it.isLine = true;
            // /// is a doc comment; //// or longer slash run is not.
            it.isDoc = text.size() >= 3 && text[2] == '/' && (text.size() < 4 || text[3] != '/');
            return it;
        }
        if (text.compare(0, 2, "/*") == 0)
        {
            it.isLine = false;
            // /** ... */ is a doc comment; bare /**/ is not.
            it.isDoc = text.size() >= 3 && text[2] == '*' && text != "/**/";
            return it;
        }
        return std::nullopt;
    };

    // Walk hidden tokens right-to-left and collect comments that chain up to the
    // declaration with no blank line gap.
    std::vector<Item> chain;
    int nextExpectedEnd = (int)declStart->getLine() - 1;
    for (auto it = hidden.rbegin(); it != hidden.rend(); ++it)
    {
        auto opt = classify(*it);
        if (!opt) continue;
        Item item = *opt;
        if (item.endLine < nextExpectedEnd) break;     // blank-line gap broke the chain
        if (item.endLine > nextExpectedEnd) continue;  // comment trailing other code on this line; skip
        chain.push_back(item);
        nextExpectedEnd = item.startLine - 1;
    }
    if (chain.empty()) return "";
    std::reverse(chain.begin(), chain.end());

    // Tier 1: keep only the trailing contiguous run of doc comments (closest to decl).
    // Tier 2 (fallback): if no doc comments at all, keep the whole chain as plain.
    bool anyDoc = false;
    for (const auto& it : chain) if (it.isDoc) { anyDoc = true; break; }

    std::vector<Item> selected;
    if (anyDoc)
    {
        size_t first = chain.size();
        for (size_t i = chain.size(); i-- > 0; )
        {
            if (!chain[i].isDoc) break;
            first = i;
        }
        selected.assign(chain.begin() + first, chain.end());
    }
    else
    {
        selected = std::move(chain);
    }

    auto trimRight = [](std::string& s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    };

    std::vector<std::string> outLines;
    for (const auto& it : selected)
    {
        const std::string& text = it.tok->getText();
        if (it.isLine)
        {
            size_t off = it.isDoc ? 3 : 2;  // strip /// or //
            std::string ln = (text.size() > off) ? text.substr(off) : std::string();
            if (!ln.empty() && ln.front() == ' ') ln.erase(0, 1);
            trimRight(ln);
            outLines.push_back(ln);
        }
        else
        {
            // Strip leading /* or /** and trailing */.
            size_t startOff = it.isDoc ? 3 : 2;
            size_t endOff = (text.size() >= 2 && text.compare(text.size() - 2, 2, "*/") == 0) ? 2 : 0;
            if (text.size() <= startOff + endOff) continue;
            std::string body = text.substr(startOff, text.size() - startOff - endOff);
            std::vector<std::string> lines;
            std::string cur;
            for (char c : body)
            {
                if (c == '\n') { lines.push_back(cur); cur.clear(); }
                else cur.push_back(c);
            }
            lines.push_back(cur);
            for (auto& ln : lines)
            {
                size_t j = 0;
                while (j < ln.size() && (ln[j] == ' ' || ln[j] == '\t')) ++j;
                if (j < ln.size() && ln[j] == '*')
                {
                    ++j;
                    if (j < ln.size() && ln[j] == ' ') ++j;
                }
                std::string clean = (j < ln.size()) ? ln.substr(j) : std::string();
                trimRight(clean);
                outLines.push_back(clean);
            }
        }
    }

    while (!outLines.empty() && outLines.front().empty()) outLines.erase(outLines.begin());
    while (!outLines.empty() && outLines.back().empty()) outLines.pop_back();
    if (outLines.empty()) return "";

    std::string result;
    for (size_t i = 0; i < outLines.size(); ++i)
    {
        if (i > 0) result += '\n';
        result += outLines[i];
    }
    return result;
}

// Check if a function definition has the 'static' storage class.
static bool isFunctionStatic(CFlatParser::FunctionDefinitionContext* func)
{
    if (!func->declarationSpecifiers()) return false;
    for (auto* ds : func->declarationSpecifiers()->declarationSpecifier())
        if (ds->storageClassSpecifier() && ds->storageClassSpecifier()->Static() != nullptr)
            return true;
    return false;
}

// ForwardRefScanner performs a lightweight pre-pass over the AST to register
// all function signatures and struct type shells before the main code-gen walk.
// This allows functions and types to be used before their definition in source.
class ForwardRefScanner
{
private:
    LLVMBackend* compilerLLVM;
    antlr4::BufferedTokenStream* tokens_ = nullptr;

    LLVMBackend* Compiler(antlr4::ParserRuleContext* ctx)
    {
        compilerLLVM->SetSourceLocation(ctx->getStart()->getLine(), ctx->getStart()->getCharPositionInLine());
        return compilerLLVM;
    }

    LLVMBackend::DeclTypeAndValue ParseDeclarationSpecifiers(CFlatParser::DeclarationSpecifiersContext* declSpecs)
    {
        auto* compiler = Compiler(declSpecs);
        LLVMBackend::DeclTypeAndValue declType;
        for (auto declSpec : declSpecs->declarationSpecifier())
        {
            auto typeSpec = declSpec->typeSpecifier();
            auto storageSpec = declSpec->storageClassSpecifier();
                if (typeSpec != nullptr)
                {
                    // 'move' and 'bond' are soft keywords parsed as Identifiers in typeSpecifier context
                    if (typeSpec->getText() == "move")
                    {
                        declType.IsMove = true;
                        continue;  // not a type; look for the actual type in next specifier
                    }
                    if (typeSpec->getText() == "bond")
                    {
                        declType.IsBond = true;
                        continue;  // not a type; look for the actual type in next specifier
                    }
                    // tuple type sugar: (T1, T2) -> tuple<T1, T2>
                    if (typeSpec->tupleTypeSpecifier() != nullptr)
                    {
                        auto* tts = typeSpec->tupleTypeSpecifier();
                        // Pack-only form (T...) resolved during instantiation - skip forward-declare here
                        if (tts->tupleTypePackEntry() != nullptr)
                            break;
                        std::vector<std::string> typeArgs;
                        for (auto* entry : tts->tupleTypeEntry())
                        {
                            std::string argName = entry->typeSpecifier()->getText();
                            if (entry->pointer() != nullptr) argName += "*";
                            typeArgs.push_back(argName);
                        }
                        std::string mangledName = "tuple";
                        for (const auto& arg : typeArgs) mangledName += "__" + MangleTypeArg(arg);
                        compiler->CreateStructType(mangledName, {});
                        LLVMBackend::TypeAndValue rt{ .TypeName = mangledName };
                        compiler->CreateFunctionDeclaration(mangledName, rt, {});
                        declType.TypeName = mangledName;
                        declType.Pointer = declSpec->pointer() != nullptr;
                        declType.ArraySize = declSpec->arrayDimSpec() ? declSpec->arrayDimSpec()->assignmentExpression(0) : nullptr;
                        break;
                    }
                    // function pointer type: function<RetType(Params)> or bare 'function'
                    if (typeSpec->functionPointerSpecifier() != nullptr)
                    {
                        auto* fpSpec = typeSpec->functionPointerSpecifier();
                        declType.IsFunctionPointer = true;
                        if (fpSpec->typeSpecifier() != nullptr)
                        {
                            declType.FuncPtrReturnTypeName = fpSpec->typeSpecifier()->getText();
                            declType.FuncPtrReturnPointer = fpSpec->pointer() != nullptr;
                            if (fpSpec->functionPointerParamList() != nullptr)
                            {
                                for (auto* param : fpSpec->functionPointerParamList()->functionPointerParam())
                                {
                                    LLVMBackend::TypeAndValue::FuncPtrParam p;
                                    p.TypeName = param->typeSpecifier()->getText();
                                    p.Pointer = param->pointer() != nullptr;
                                    p.IsMove = param->Move() != nullptr;
                                    declType.FuncPtrParams.push_back(p);
                                }
                            }
                        }
                        // For bare 'function', signature inferred from initializer at declaration site
                        break;
                    }
                    // grammar: some Identifier occurrences were refactored into a genericIdentifier rule
                    if (typeSpec->genericIdentifier() != nullptr && typeSpec->genericIdentifier()->genericTypeParameters() != nullptr)
                    {
                        // Generic type instantiation: Box<MyType> -> Box__MyType
                        std::string baseName = typeSpec->genericIdentifier()->Identifier()->getText();
                        std::vector<std::string> typeArgs;
                    for (auto* entry : typeSpec->genericIdentifier()->genericTypeParameters()->typeParameterList()->typeParameterEntry())
                        typeArgs.push_back(entry->getText());
                    std::string mangledName = baseName;
                    for (const auto& arg : typeArgs) mangledName += "__" + MangleTypeArg(arg);
                    // Pre-declare opaque struct type and default constructor so that
                    // uses inside function bodies are resolvable before the full
                    // definition is emitted by ProcessPendingInstantiations().
                    compiler->CreateStructType(mangledName, {});
                    LLVMBackend::TypeAndValue returnType{ .TypeName = mangledName };
                    compiler->CreateFunctionDeclaration(mangledName, returnType, {});
                    declType.TypeName = mangledName;
                }
                else
                {
                    declType.TypeName = compiler->ResolveQualifiedName(typeSpec->getText());
                    // Resolve type aliases (e.g. user-defined aliases)
                    declType.TypeName = compiler->ResolveTypeAlias(declType.TypeName);
                }
                if (declSpec->pointer() != nullptr) {
                    declType.Pointer = true;
                    if (declSpec->pointer()->Star().size() >= 2)
                        declType.ElemPointer = true;
                }
                if (declSpec->arrayDimSpec())
                {
                    auto dims = declSpec->arrayDimSpec()->assignmentExpression();
                    declType.ArraySize = dims.empty() ? nullptr : dims[0];
                    for (size_t di = 1; di < dims.size(); di++)
                        declType.ExtraArrayDims.push_back(dims[di]);
                }
                declType.IsInterface = compiler->IsInterfaceType(declType.TypeName);
                if (declType.IsInterface && declSpec->pointer() != nullptr)
                    std::cerr << std::format("error: pointer '*' is not allowed on interface type '{}'\n", declType.TypeName);
                if (declType.IsInterface)
                {
                    declType.IsInterfacePointer = declSpec->pointer() != nullptr;
                    if (declType.IsInterfacePointer)
                        declType.Pointer = true;
                }
                if (declSpec->Question())
                {
                    if (declType.IsPrimitive())
                        std::cerr << std::format("error: nullable '?' is not allowed on primitive type '{}'\n", declType.TypeName);
                    else
                    {
                        declType.IsNullable = true;
                        declType.Pointer = true;
                    }
                }
                break;
            }
            else if (storageSpec)
            {
                declType.external = storageSpec->Extern() != nullptr;
                declType.threadLocal = storageSpec->ThreadLocal() != nullptr;
            }
            else if (auto funcSpec = declSpec->functionSpecifier())
            {
                if (funcSpec->getText() == "stdcall")
                    declType.IsStdcall = true;
                else if (funcSpec->getText() == "cdecl")
                    declType.IsCdecl = true;
            }
            else if (declSpec->alignmentSpecifier() != nullptr)
            {
                // ForwardRefScanner: alignment doesn't affect forward declarations.
                // The codegen pass captures the actual value.
            }
        }
        return declType;
    }

    std::vector<LLVMBackend::DeclTypeAndValue> ParseParameterTypeList(CFlatParser::ParameterTypeListContext* paramTypeList)
    {
        std::vector<LLVMBackend::DeclTypeAndValue> params;
        if (paramTypeList == nullptr)
            return params;

        auto paramList = paramTypeList->parameterList();
        for (auto paramDecl : paramList->parameterDeclaration())
        {
            LLVMBackend::DeclTypeAndValue paramType = ParseDeclarationSpecifiers(paramDecl->declarationSpecifiers());
            if (auto declarer = paramDecl->declarator())
                if (auto directDeclarer = declarer->directDeclarator())
                    paramType.VariableName = getDirectDeclName(directDeclarer);
            if (paramType.IsMove && paramType.IsBond)
                Compiler(paramDecl)->LogError(std::format("parameter '{}': 'bond' and 'move' are mutually exclusive", paramType.VariableName));
            if (auto* lc = paramDecl->lockClause())
            {
                auto args = lc->lockArgList()->expression();
                if (args.size() == 1 && args[0]->getText() == "this")
                    paramType.LockThis = true;
                else
                    Compiler(paramDecl)->LogError("lock(this) is the only supported form on function parameters");
            }
            paramType.DefaultValue = paramDecl->initializer();
            params.push_back(paramType);
        }
        return params;
    }

    void ScanFunctionDefinition(CFlatParser::FunctionDefinitionContext* func, const std::string& structName = {}, const std::string& namespaceName = {}, const std::vector<std::string>& extraRequiredLocks = {})
    {
        auto* compiler = Compiler(func);
        // Return-block functions are inlined at call sites - no LLVM proto needed.
        if (IsReturnBlockFunction(func))
            return;

        // Generic function templates are instantiated on demand - skip pre-declaration.
        if (func->genericTypeParameters() != nullptr)
            return;

        std::string name = getFunctionName(func);
        if (!namespaceName.empty())
            name = namespaceName + "." + name;

        auto returnType = ParseDeclarationSpecifiers(func->declarationSpecifiers());
        // 'auto' return type is only supported on generic function templates (already
        // skipped above) and return-block functions. Reaching this point with auto
        // means the user wrote a non-generic, non-return-block 'auto fn(...)' which
        // we cannot infer in v1. Diagnose here so GetType does not fire its less
        // helpful "unknown type 'auto'" later in this scan.
        if (returnType.TypeName == "auto")
        {
            Compiler(func)->LogError("'auto' return type is only supported on generic functions (e.g. auto f<T>(T x))");
            return;
        }
        auto* paramTypeList = func->parameterTypeList();
        auto params = ParseParameterTypeList(paramTypeList);
        bool varargs = paramTypeList && paramTypeList->Ellipsis() != nullptr;

        // Operator new/delete/string and static methods are scoped to struct but have no 'this' param
        bool isOperatorFunc = (getFunctionName(func) == "operator new"
                            || getFunctionName(func) == "operator delete"
                            || getFunctionName(func) == "operator string");
        bool isStaticFunc = isFunctionStatic(func);
        if (!structName.empty() && (isOperatorFunc || isStaticFunc))
        {
            name = structName + "." + getFunctionName(func);
        }
        else if (!structName.empty())
        {
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName = structName;
            thisParam.VariableName = structName + "__";
            thisParam.Pointer = true;
            params.insert(params.begin(), thisParam);
        }

        std::vector<LLVMBackend::TypeAndValue> allParams(params.begin(), params.end());

        // Detect functions that heap-allocate and return a new owned value.
        // operator+ always allocates; operator string(i32) uses malloc; user functions can
        // opt in by declaring 'move string' or 'move T*' as their return type.
        bool returnsOwned = false;
        if (returnType.TypeName == "string")
        {
            if (name == "operator+")
                returnsOwned = true;
            else if (name == "operator string" && allParams.size() == 1 && allParams[0].TypeName == "i32")
                returnsOwned = true;
            else if (returnType.IsMove)
                returnsOwned = true;
        }
        else if (returnType.IsMove && returnType.Pointer)
        {
            returnsOwned = true;
        }

        compiler->CreateFunctionDeclaration(name, returnType, allParams, returnType.external, varargs, returnsOwned, false, returnType.IsStdcall, returnType.IsCdecl);

        // Populate RequiredLocks from the function's lock clause and any extra locks
        // inherited from a positional lock group (extraRequiredLocks).
        {
            std::vector<std::string> locks;
            if (auto* lockClauseCtx = func->lockClause())
            {
                for (auto* exprCtx : lockClauseCtx->lockArgList()->expression())
                    locks.push_back(exprCtx->getText());
            }
            for (const auto& extra : extraRequiredLocks)
                locks.push_back(extra);
            if (!locks.empty())
                compiler->SetFunctionRequiredLocks(name, std::move(locks));
        }

        if (auto* s = compiler->GetSymbolSink())
        {
            std::string sig = returnType.TypeName + " " + name + "(";
            bool first = true;
            for (const auto& p : params)
            {
                // Skip implicit 'this' pointer (name convention: TypeName__)
                if (p.VariableName.size() >= 2 && p.VariableName.compare(p.VariableName.size() - 2, 2, "__") == 0)
                    continue;
                if (!first) sig += ", ";
                first = false;
                sig += p.TypeName;
                if (p.Pointer) sig += "*";
                if (!p.VariableName.empty()) sig += " " + p.VariableName;
            }
            sig += ")";
            std::string doc = ExtractLeadingDoc(tokens_, func->getStart());
            s->Register(SymbolKind::Function, name, compiler->GetSourceFilePath(),
                        (int)func->getStart()->getLine(), (int)func->getStart()->getCharPositionInLine(),
                        sig, {}, doc);

            // Also register under "TypeName.method" for dot-completion prefix lookup.
            // Operators and statics already have the qualified name; only instance methods need this.
            if (!structName.empty() && !isOperatorFunc && !isStaticFunc)
            {
                std::string qualName = structName + "." + getFunctionName(func);
                s->Register(SymbolKind::Function, qualName, compiler->GetSourceFilePath(),
                            (int)func->getStart()->getLine(), (int)func->getStart()->getCharPositionInLine(),
                            sig, {}, doc);
            }
        }

        // Pre-declare overloads for default parameters
        int firstDefault = -1;
        for (int i = 0; i < (int)params.size(); i++)
        {
            if (params[i].DefaultValue != nullptr) { firstDefault = i; break; }
        }
        for (int cutoff = firstDefault; firstDefault >= 0 && cutoff < (int)params.size(); cutoff++)
        {
            std::vector<LLVMBackend::TypeAndValue> wrapperParams(params.begin(), params.begin() + cutoff);
            compiler->CreateFunctionDeclaration(name, returnType, wrapperParams, false, false);
        }
    }

    void ScanInterfaceDefinition(CFlatParser::InterfaceDefinitionContext* ctx)
    {
        // Generic interface templates are not pre-declared; they are instantiated on demand.
        auto* nameGid = ctx->genericIdentifier();
        if (nameGid && nameGid->genericTypeParameters() != nullptr)
            return;

        if (!nameGid || !nameGid->Identifier()) return;
        std::string name = nameGid->Identifier()->getText();

        std::vector<std::string> parentNames;
        for (auto* term : ctx->Identifier())
            parentNames.push_back(term->getText());

        std::vector<LLVMBackend::InterfaceMethod> methods;
        for (auto method : ctx->interfaceMethod())
        {
            LLVMBackend::InterfaceMethod m;
            m.ReturnType = ParseDeclarationSpecifiers(method->declarationSpecifiers());
            m.Name = getInterfaceMethodName(method);
            auto declParams = ParseParameterTypeList(method->parameterTypeList());
            for (const auto& p : declParams)
                m.Parameters.push_back(p);
            methods.push_back(std::move(m));
        }

        Compiler(ctx)->CreateInterfaceDefinition(name, parentNames, methods);

        if (auto* s = Compiler(ctx)->GetSymbolSink())
        {
            std::string sig = "interface " + name;
            if (!parentNames.empty())
            {
                sig += " : ";
                for (size_t i = 0; i < parentNames.size(); ++i)
                {
                    if (i > 0) sig += ", ";
                    sig += parentNames[i];
                }
            }
            std::vector<std::string> memberNames;
            for (const auto& m : methods)
                memberNames.push_back(m.Name);
            s->Register(SymbolKind::Interface, name, Compiler(ctx)->GetSourceFilePath(),
                        (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine(),
                        sig, memberNames, ExtractLeadingDoc(tokens_, ctx->getStart()));
        }
    }

    // Pre-declare a struct or class type shell, member functions, and destructor.
    // Templated to handle both StructDefinitionContext and ClassDefinitionContext.
    template<typename TCtx>
    void ScanStructOrClassDefinition(TCtx* ctx, const std::string& namespaceName = {})
    {
        auto* compiler = Compiler(ctx);
        // Generic template definitions are not pre-declared; they are instantiated on demand.
        // However, we still register the template name and its method names in the LSP
        // symbol index so hover / go-to-definition can resolve uses through a variable
        // of the instantiated type (e.g. `g_explosions.getPtr` -> `array.getPtr`).
        if (ctx->genericTypeParameters() != nullptr)
        {
            if (auto* s = compiler->GetSymbolSink())
            {
                std::string typeName = ctx->directDeclarator()->getText();
                if (!namespaceName.empty())
                    typeName = namespaceName + "." + typeName;
                std::string keyword = ctx->getStart()->getText();
                std::string doc = ExtractLeadingDoc(tokens_, ctx->getStart());
                s->Register(SymbolKind::Struct, typeName, compiler->GetSourceFilePath(),
                            (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine(),
                            keyword + " " + typeName, {}, doc);
                for (auto* func : ctx->functionDefinition())
                {
                    std::string funcName = getFunctionName(func);
                    if (funcName == typeName || funcName.empty()) continue;
                    std::string qualName = typeName + "." + funcName;
                    std::string fdoc = ExtractLeadingDoc(tokens_, func->getStart());
                    s->Register(SymbolKind::Function, qualName, compiler->GetSourceFilePath(),
                                (int)func->getStart()->getLine(),
                                (int)func->getStart()->getCharPositionInLine(),
                                funcName, {}, fdoc);
                }
            }
            return;
        }

        std::string typeName = ctx->directDeclarator()->getText();
        if (!namespaceName.empty())
            typeName = namespaceName + "." + typeName;

        // Register opaque struct so the type is known for pointer/field use
        compiler->CreateStructType(typeName, {});

        if (auto* s = compiler->GetSymbolSink())
        {
            std::string keyword = ctx->getStart()->getText();
            std::string doc = ExtractLeadingDoc(tokens_, ctx->getStart());
            s->Register(SymbolKind::Struct, typeName, compiler->GetSourceFilePath(),
                        (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine(),
                        keyword + " " + typeName, {}, doc);
            // Also register under the unqualified name so Lookup("Point") finds "Geometry.Point".
            size_t dot = typeName.rfind('.');
            if (dot != std::string::npos)
                s->Register(SymbolKind::Struct, typeName.substr(dot + 1), compiler->GetSourceFilePath(),
                            (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine(),
                            keyword + " " + typeName, {}, doc);
        }

        // Pre-declare default constructor
        LLVMBackend::TypeAndValue returnType{ .TypeName = typeName };
        compiler->CreateFunctionDeclaration(typeName, returnType, {});

        // Pre-declare member functions (and detect constructor overloads)
        for (auto func : ctx->functionDefinition())
        {
            if (getFunctionName(func) == typeName)
            {
                // Constructor overload - no implicit this* parameter, returns the type
                if (!func->parameterTypeList()) continue; // no-arg already declared above
                auto ctorParams = ParseParameterTypeList(func->parameterTypeList());
                std::vector<LLVMBackend::TypeAndValue> allCtorParams(ctorParams.begin(), ctorParams.end());
                compiler->CreateFunctionDeclaration(typeName, returnType, allCtorParams);
            }
            else
            {
                ScanFunctionDefinition(func, typeName);
            }
        }

        // Pre-declare destructor
        for (auto dtor : ctx->destructorDefinition())
        {
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName = typeName;
            thisParam.VariableName = typeName + "__";
            thisParam.Pointer = true;
            LLVMBackend::TypeAndValue voidReturn{ .TypeName = "void" };
            compiler->CreateFunctionDeclaration("~" + typeName, voidReturn, { thisParam });
        }

        // Recursively pre-declare nested struct/class definitions
        for (auto* nestedStruct : ctx->structDefinition())
            ScanStructDefinition(nestedStruct, typeName);
        for (auto* nestedClass : ctx->classDefinition())
            ScanClassDefinition(nestedClass, typeName);

        // Pre-declare functions inside positional lock groups and mark their RequiredLocks.
        for (auto* lfg : ctx->lockFieldGroup())
        {
            auto groupArgs = lfg->lockClause()->lockArgList()->expression();
            if (groupArgs.empty()) continue;
            std::string guardianName = groupArgs[0]->getText();
            // Strip .read/.write rwlock suffix (same logic as GetLockArgCanonical in MainListener).
            if (guardianName.size() > 5 && guardianName.substr(guardianName.size()-5) == ".read")
                guardianName = guardianName.substr(0, guardianName.size()-5);
            else if (guardianName.size() > 6 && guardianName.substr(guardianName.size()-6) == ".write")
                guardianName = guardianName.substr(0, guardianName.size()-6);
            // Qualify bare names to "this.<name>" so call-site substitution works.
            std::string qualifiedLock = (guardianName.find('.') == std::string::npos)
                                        ? ("this." + guardianName) : guardianName;
            std::vector<std::string> groupLocks = { qualifiedLock };
            for (auto* func : lfg->functionDefinition())
                ScanFunctionDefinition(func, typeName, {}, groupLocks);
        }
    }

    void ScanStructDefinition(CFlatParser::StructDefinitionContext* ctx, const std::string& namespaceName = {})
    {
        ScanStructOrClassDefinition(ctx, namespaceName);
    }

    void ScanClassDefinition(CFlatParser::ClassDefinitionContext* ctx, const std::string& namespaceName = {})
    {
        ScanStructOrClassDefinition(ctx, namespaceName);
    }

public:
    ForwardRefScanner(LLVMBackend* compiler) : compilerLLVM(compiler) {}
    void SetTokens(antlr4::BufferedTokenStream* t) { tokens_ = t; }

    // Walk every typeSpecifier in the entire parse tree and pre-declare an opaque
    // struct type + default constructor for each generic instantiation found.
    // This ensures that Box<MyType> references inside function bodies resolve
    // correctly during the main pass, before ProcessPendingInstantiations runs.
    void ScanGenericTypeUses(antlr4::RuleContext* ctx)
    {
        for (auto* child : ctx->children)
        {
            auto* ruleCtx = dynamic_cast<antlr4::RuleContext*>(child);
            if (!ruleCtx) continue;

            auto tryPreDeclare = [&](const std::string& baseName, CFlatParser::GenericTypeParametersContext* genericParams)
            {
                auto* compiler = Compiler(genericParams);
                std::string mangledName = baseName;
                for (auto* entry : genericParams->typeParameterList()->typeParameterEntry())
                    mangledName += "__" + MangleTypeArg(entry->getText());
                compiler->CreateStructType(mangledName, {});
                LLVMBackend::TypeAndValue returnType{ .TypeName = mangledName };
                compiler->CreateFunctionDeclaration(mangledName, returnType, {});
            };

            // Use getRuleIndex() (integer compare) instead of dynamic_cast for each node type
            // to avoid the RTTI hierarchy walk (FindSITargetTypeInstance) on every tree node.
            switch (ruleCtx->getRuleIndex())
            {
            case CFlatParser::RuleStructDefinition:
                // Skip generic template definitions - bodies contain unbound type parameters (e.g. T).
                if (static_cast<CFlatParser::StructDefinitionContext*>(ruleCtx)->genericTypeParameters() != nullptr)
                    continue;
                break;
            case CFlatParser::RuleClassDefinition:
                if (static_cast<CFlatParser::ClassDefinitionContext*>(ruleCtx)->genericTypeParameters() != nullptr)
                    continue;
                break;
            case CFlatParser::RuleFunctionDefinition:
                // Skip generic function template definitions for the same reason.
                if (static_cast<CFlatParser::FunctionDefinitionContext*>(ruleCtx)->genericTypeParameters() != nullptr)
                    continue;
                break;
            case CFlatParser::RuleTypeSpecifier:
                {
                    auto* typeSpec = static_cast<CFlatParser::TypeSpecifierContext*>(ruleCtx);
                    if (typeSpec->genericIdentifier() != nullptr && typeSpec->genericIdentifier()->genericTypeParameters() != nullptr && typeSpec->genericIdentifier()->Identifier() != nullptr)
                        tryPreDeclare(typeSpec->genericIdentifier()->Identifier()->getText(), typeSpec->genericIdentifier()->genericTypeParameters());

                    // Tuple type sugar: (T1, T2) -> pre-declare tuple__T1__T2
                    if (typeSpec->tupleTypeSpecifier() != nullptr)
                    {
                        auto* tts = typeSpec->tupleTypeSpecifier();
                        // Pack-only form (T...) resolved during instantiation - skip forward-declare here
                        if (tts->tupleTypePackEntry() == nullptr)
                        {
                            std::string mangledName = "tuple";
                            for (auto* entry : tts->tupleTypeEntry())
                            {
                                std::string argName = entry->typeSpecifier()->getText();
                                if (entry->pointer() != nullptr) argName += "*";
                                mangledName += "__" + MangleTypeArg(argName);
                            }
                            auto* c = Compiler(tts);
                            c->CreateStructType(mangledName, {});
                            LLVMBackend::TypeAndValue rt{ .TypeName = mangledName };
                            c->CreateFunctionDeclaration(mangledName, rt, {});
                        }
                    }
                    break;
                }
            case CFlatParser::RulePrimaryExpression:
                {
                    auto* primaryExpr = static_cast<CFlatParser::PrimaryExpressionContext*>(ruleCtx);
                    if (primaryExpr->genericIdentifier() != nullptr && primaryExpr->genericIdentifier()->genericTypeParameters() != nullptr && primaryExpr->genericIdentifier()->Identifier() != nullptr)
                        tryPreDeclare(primaryExpr->genericIdentifier()->Identifier()->getText(), primaryExpr->genericIdentifier()->genericTypeParameters());
                    break;
                }
            }

            ScanGenericTypeUses(ruleCtx);
        }
    }

    void ScanUsingDeclaration(CFlatParser::UsingDeclarationContext* ctx)
    {
        auto* compiler = Compiler(ctx);

        std::string alias;
        if (ctx->String())
            alias = ctx->String()->getText();
        else if (ctx->Identifier() != nullptr)
            alias = ctx->Identifier()->getText();

        std::string target = ctx->typeSpecifier()->getText();

        if (compiler->IsInterfaceType(target) || compiler->dataStructures.count(target) > 0
            || LLVMBackend::IsPrimitiveTypeName(target))
            compiler->RegisterTypeAlias(alias, target);
    }

    void ScanProgramDefinition(CFlatParser::ProgramDefinitionContext* ctx)
    {
        auto* compiler = Compiler(ctx);
        std::string name = ctx->directDeclarator()->getText();

        // Register opaque struct shell and default constructor
        compiler->CreateStructType(name, {});
        LLVMBackend::TypeAndValue returnType{ .TypeName = name };
        compiler->CreateFunctionDeclaration(name, returnType, {});

        // Pre-register channel<IMessage> so the synthetic inbox field resolves during the main pass
        {
            const std::string channelMangledName = "channel__IMessage";
            if (!compiler->dataStructures.count(channelMangledName))
            {
                compiler->CreateStructType(channelMangledName, {});
                LLVMBackend::TypeAndValue chReturnType{ .TypeName = channelMangledName };
                compiler->CreateFunctionDeclaration(channelMangledName, chReturnType, {});
            }
        }

        // Pre-declare trampoline: int __program_run_Name(void*)
        {
            LLVMBackend::TypeAndValue intReturn{ .TypeName = "int" };
            LLVMBackend::DeclTypeAndValue ctxParam;
            ctxParam.TypeName = "void";
            ctxParam.VariableName = "ctx";
            ctxParam.Pointer = true;
            compiler->CreateFunctionDeclaration("__program_run_" + name, intReturn, { ctxParam }, false, false, false, false, false);
        }

        // Pre-declare run(Name* this, list__string args) -> bool
        {
            LLVMBackend::TypeAndValue boolReturn{ .TypeName = "bool" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer = true;
            LLVMBackend::DeclTypeAndValue argsParam;
            argsParam.TypeName = "list__string";
            argsParam.VariableName = "args";
            argsParam.IsMove = true;  // run() takes ownership; caller's list is zeroed after the call
            compiler->CreateFunctionDeclaration("run", boolReturn, { thisParam, argsParam });
        }

        // Pre-declare WaitForExit(Name* this) -> void
        {
            LLVMBackend::TypeAndValue voidReturn{ .TypeName = "void" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer = true;
            compiler->CreateFunctionDeclaration("WaitForExit", voidReturn, { thisParam });
        }

        // Pre-declare WaitForExit(Name* this, stop_token token) -> bool
        {
            LLVMBackend::TypeAndValue boolReturn{ .TypeName = "bool" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName     = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer      = true;
            LLVMBackend::DeclTypeAndValue tokenParam;
            tokenParam.TypeName     = "stop_token";
            tokenParam.VariableName = "token";
            compiler->CreateFunctionDeclaration("WaitForExit", boolReturn, { thisParam, tokenParam });
        }

        // Pre-declare WaitForExit(Name* this, int timeoutMs) -> bool
        {
            LLVMBackend::TypeAndValue boolReturn{ .TypeName = "bool" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName     = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer      = true;
            LLVMBackend::DeclTypeAndValue msParam;
            msParam.TypeName     = "int";
            msParam.VariableName = "timeoutMs";
            compiler->CreateFunctionDeclaration("WaitForExit", boolReturn, { thisParam, msParam });
        }

        // Pre-declare Kill(Name* this) -> void
        {
            LLVMBackend::TypeAndValue voidReturn{ .TypeName = "void" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName     = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer      = true;
            compiler->CreateFunctionDeclaration("Kill", voidReturn, { thisParam });
        }

        // Pre-declare RequestStop(Name* this) -> void
        {
            LLVMBackend::TypeAndValue voidReturn{ .TypeName = "void" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName     = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer      = true;
            compiler->CreateFunctionDeclaration("RequestStop", voidReturn, { thisParam });
        }

        // Pre-declare exitCode(Name* this) -> int (IProcess method)
        {
            LLVMBackend::TypeAndValue intReturn{ .TypeName = "int" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName     = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer      = true;
            compiler->CreateFunctionDeclaration("exitCode", intReturn, { thisParam });
        }

        // Store declared interfaces in programTable for VTable dispatch
        {
            std::vector<std::string> ifaces;
            for (auto* id : ctx->Identifier())
                ifaces.push_back(id->getText());
            compiler->programTable[name].Interfaces = ifaces;
        }

        // Pre-declare member functions (including user's main) and destructor
        for (auto func : ctx->functionDefinition())
            ScanFunctionDefinition(func, name);
        for (auto dtor : ctx->destructorDefinition())
        {
            LLVMBackend::TypeAndValue voidReturn{ .TypeName = "void" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer = true;
            compiler->CreateFunctionDeclaration("~" + name, voidReturn, { thisParam });
        }
    }

    void ScanImportedProgramDefinition(const std::string& name)
    {
        auto* compiler = compilerLLVM;

        compiler->CreateStructType(name, {});
        LLVMBackend::TypeAndValue returnType{ .TypeName = name };
        compiler->CreateFunctionDeclaration(name, returnType, {});

        // Pre-register channel<IMessage> so the synthetic inbox field resolves during the main pass
        {
            const std::string channelMangledName = "channel__IMessage";
            if (!compiler->dataStructures.count(channelMangledName))
            {
                compiler->CreateStructType(channelMangledName, {});
                LLVMBackend::TypeAndValue chReturnType{ .TypeName = channelMangledName };
                compiler->CreateFunctionDeclaration(channelMangledName, chReturnType, {});
            }
        }

        // Pre-declare trampoline: int __program_run_Name(void*)
        {
            LLVMBackend::TypeAndValue intReturn{ .TypeName = "int" };
            LLVMBackend::DeclTypeAndValue ctxParam;
            ctxParam.TypeName = "void";
            ctxParam.VariableName = "ctx";
            ctxParam.Pointer = true;
            compiler->CreateFunctionDeclaration("__program_run_" + name, intReturn, { ctxParam }, false, false, false, false, false);
        }

        // Pre-declare run(Name* this, list__string args) -> bool
        {
            LLVMBackend::TypeAndValue boolReturn{ .TypeName = "bool" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer = true;
            LLVMBackend::DeclTypeAndValue argsParam;
            argsParam.TypeName = "list__string";
            argsParam.VariableName = "args";
            argsParam.IsMove = true;
            compiler->CreateFunctionDeclaration("run", boolReturn, { thisParam, argsParam });
        }

        // Pre-declare WaitForExit(Name* this) -> void
        {
            LLVMBackend::TypeAndValue voidReturn{ .TypeName = "void" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer = true;
            compiler->CreateFunctionDeclaration("WaitForExit", voidReturn, { thisParam });
        }

        // Pre-declare WaitForExit(Name* this, stop_token token) -> bool
        {
            LLVMBackend::TypeAndValue boolReturn{ .TypeName = "bool" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName     = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer      = true;
            LLVMBackend::DeclTypeAndValue tokenParam;
            tokenParam.TypeName     = "stop_token";
            tokenParam.VariableName = "token";
            compiler->CreateFunctionDeclaration("WaitForExit", boolReturn, { thisParam, tokenParam });
        }

        // Pre-declare WaitForExit(Name* this, int timeoutMs) -> bool
        {
            LLVMBackend::TypeAndValue boolReturn{ .TypeName = "bool" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName     = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer      = true;
            LLVMBackend::DeclTypeAndValue msParam;
            msParam.TypeName     = "int";
            msParam.VariableName = "timeoutMs";
            compiler->CreateFunctionDeclaration("WaitForExit", boolReturn, { thisParam, msParam });
        }

        // Pre-declare Kill(Name* this) -> void
        {
            LLVMBackend::TypeAndValue voidReturn{ .TypeName = "void" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName     = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer      = true;
            compiler->CreateFunctionDeclaration("Kill", voidReturn, { thisParam });
        }

        // Pre-declare RequestStop(Name* this) -> void
        {
            LLVMBackend::TypeAndValue voidReturn{ .TypeName = "void" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName     = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer      = true;
            compiler->CreateFunctionDeclaration("RequestStop", voidReturn, { thisParam });
        }
        // Pre-declare exitCode(Name* this) -> int (IProcess method)
        {
            LLVMBackend::TypeAndValue intReturn{ .TypeName = "int" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName     = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer      = true;
            compiler->CreateFunctionDeclaration("exitCode", intReturn, { thisParam });
        }

        // All imported programs implicitly implement IProcess.
        compiler->programTable[name].Interfaces = { "IProcess" };
    }

    void ScanAnnotationDefinition(CFlatParser::AnnotationDefinitionContext* ctx)
    {
        auto* compiler = Compiler(ctx);
        std::string name = ctx->Identifier()->getText();

        std::vector<std::string> fields;
        for (auto* decl : ctx->declaration())
        {
            if (auto* initList = decl->initDeclaratorList())
                for (auto* initDecl : initList->initDeclarator())
                    if (auto* dir = initDecl->declarator())
                        fields.push_back(getDirectDeclName(dir->directDeclarator()));
        }
        compiler->annotationRegistry[name] = fields;
    }

    void ScanExternalDeclaration(CFlatParser::ExternalDeclarationContext* ctx, const std::string& namespaceName = {})
    {
        if (auto annDef = ctx->annotationDefinition())
            ScanAnnotationDefinition(annDef);
        else if (auto ns = ctx->namespaceDefinition())
            ScanNamespace(ns, namespaceName);
        else if (auto func = ctx->functionDefinition())
            ScanFunctionDefinition(func, {}, namespaceName);
        else if (auto dataStruct = ctx->structDefinition())
            ScanStructDefinition(dataStruct, namespaceName);
        else if (auto classDef = ctx->classDefinition())
            ScanClassDefinition(classDef, namespaceName);
        else if (auto iface = ctx->interfaceDefinition())
            ScanInterfaceDefinition(iface);
        else if (auto usingDecl = ctx->usingDeclaration())
            ScanUsingDeclaration(usingDecl);
        else if (auto progDef = ctx->programDefinition())
            ScanProgramDefinition(progDef);
        else if (auto imp = ctx->importDeclaration())
        {
            if (imp->children.size() >= 2 && imp->children[1]->getText() == "program")
            {
                std::string alias = imp->Identifier()->getText();
                ScanImportedProgramDefinition(alias);
            }
        }
        else if (auto expectErrDecl = ctx->expectErrorDeclaration())
        {
            // Set expectedError so errors during the scan are caught rather than aborting.
            std::string rawText = expectErrDecl->StringLiteral()->getText();
            rawText = rawText.substr(1, rawText.size() - 2); // strip surrounding quotes
            compilerLLVM->expectedError = rawText;
            try
            {
                for (auto* extDecl : expectErrDecl->externalDeclaration())
                    ScanExternalDeclaration(extDecl, namespaceName);
            }
            catch (const ExpectedErrorReceived&) {}
            compilerLLVM->expectedError.clear();
        }
        // if const declarations are skipped here; they are handled in MainListener
        // which has access to expression evaluation and can determine the taken branch
    }

    void ScanNamespace(CFlatParser::NamespaceDefinitionContext* ctx, const std::string& parentNamespace = {})
    {
        std::string namespaceName = ctx->Identifier()->getText();
        if (!parentNamespace.empty())
            namespaceName = parentNamespace + "." + namespaceName;

        for (auto* extDecl : ctx->externalDeclaration())
            ScanExternalDeclaration(extDecl, namespaceName);
    }
};


class MainListener : public CFlatBaseListener
{
private:
    CFlatParser* parser;
    LLVMBackend* compilerLLVM;
    std::string sourceFileName;
    std::string importNamespace_;

    LLVMBackend* Compiler(antlr4::ParserRuleContext* ctx)
    {
        if (ctx)
            compilerLLVM->SetSourceLocation(ctx->getStart()->getLine(), ctx->getStart()->getCharPositionInLine());
        return compilerLLVM;
    }
    inline LLVMBackend* Compiler() { return compilerLLVM; }

    std::unordered_map<llvm::Value*, std::pair<int, llvm::Type*>> PlusPlus;
    bool global_scope = true; // true when parsing an entity in the global scope.

    // Lambda state: expected type (set by ParseDeclaration before evaluating RHS)
    // and the last lambda's TypeAndValue (side-channel from ParsePrimaryExpression to ParsePostfixExpression).
    LLVMBackend::TypeAndValue lambdaExpectedType;
    LLVMBackend::TypeAndValue lastLambdaType;
    // Non-empty when the matched function parameter is declared lock(this): holds the canonical
    // receiver name (e.g. "d->ready") to seed currentLockSet during lambda body analysis.
    std::string lambdaLockThisReceiver;

    // Side-channel from ParsePrimaryExpression to ParsePostfixExpression:
    // carries the cast TypeAndValue when the primary is a parenthesized cast expression,
    // enabling ((Struct*)ptr)->field member access without a named intermediate variable.
    LLVMBackend::TypeAndValue lastParenExprType;

    // Variadic forwarding: true when the current function being codegen'd accepts '...'
    bool currentFunctionIsVariadic = false;

    // Lock-set analysis: set of canonical lock expressions currently held.
    // Cleared at function entry, seeded from the function's lock clause, and
    // pushed/popped around lock statement bodies.
    std::unordered_set<std::string> currentLockSet;

    // Generic template state lives on the backend so each LLVMBackend instance
    // owns its own copy. References below alias compilerLLVM->gts.<field> and are
    // bound in the constructor's member initializer list.
    using PendingInstantiation = GenericTemplateState::PendingInstantiation;

    std::unordered_map<std::string, CFlatParser::StructDefinitionContext*>&     genericStructTemplates;
    std::unordered_map<std::string, CFlatParser::ClassDefinitionContext*>&      genericClassTemplates;
    std::unordered_map<std::string, std::vector<std::string>>&                  genericStructTypeParams;
    std::unordered_set<std::string>&                                            instantiatedGenerics;
    // Constraints: templateName -> { typeParamName -> [requiredInterface, …] }
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>>& genericStructConstraints;
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>>& genericClassConstraints;
    // Active type parameter substitutions during generic instantiation (e.g. "T" -> "int")
    std::unordered_map<std::string, std::string> activeTypeSubstitutions;

    // Pack param index per template: index of the variadic param, or npos if not variadic
    std::unordered_map<std::string, size_t>& genericStructPackIndex;
    std::unordered_map<std::string, size_t>& genericClassPackIndex;
    std::unordered_map<std::string, size_t>& genericFunctionPackIndex;
    std::unordered_map<std::string, size_t>& genericInterfacePackIndex;

    // Active pack substitutions during instantiation: pack-param-name -> ["int", "float", "string"]
    std::unordered_map<std::string, std::vector<std::string>> activePackSubstitutions;

    // Struct scope stack: pushed when parsing fields/methods of a struct/class so that
    // unqualified nested type names (e.g. "Inner") resolve to "Outer.Inner".
    std::vector<std::string> structScopeStack;

    std::unordered_map<std::string, CFlatParser::InterfaceDefinitionContext*>&  genericInterfaceTemplates;
    std::unordered_map<std::string, std::vector<std::string>>&                  genericInterfaceTypeParams;
    std::unordered_set<std::string>&                                            instantiatedInterfaces;

    std::unordered_map<std::string, CFlatParser::FunctionDefinitionContext*>&   genericFunctionTemplates;
    std::unordered_map<std::string, std::vector<std::string>>&                  genericFunctionTypeParams;
    std::unordered_set<std::string>&                                            instantiatedGenericFunctions;
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>>& genericFunctionConstraints;

    // Queue for pending generic instantiations (delayed until safe to emit code)
    std::vector<PendingInstantiation>& pendingInstantiations;

    constexpr static bool debugPrint = false;

    struct SwitchCaseEntry
    {
        llvm::ConstantInt* value = nullptr;       // non-null for integer cases
        llvm::Constant* strLiteral = nullptr;     // non-null for string cases (i8* global)
        llvm::BasicBlock* block;
        bool isTypeCase = false;                  // non-null for type cases (struct or interface)
        std::string typeCaseName;                 // struct or interface name for type cases
        std::string boundVarName;                 // optional bound variable for arm-style type pointer cases
        bool isArmStyle = false;                  // true if this case uses => syntax
    };

    struct SwitchContext
    {
        std::unordered_map<CFlatParser::LabeledStatementContext*, SwitchCaseEntry> caseMap;
        llvm::BasicBlock* defaultBlock = nullptr;
        llvm::BasicBlock* resumeBlock = nullptr;
        bool isStringSwitch = false;
        bool isTypeSwitch = false;                // true if this switch contains type cases
        bool isArmStyle = false;                  // true if any case uses => syntax
        llvm::Value* condValue = nullptr;         // switch condition value (needed for bound var extraction)
    };

    std::vector<SwitchContext> switchStack;

    // Recursively resolve a typeParameterEntry to its mangled string,
    // applying activeTypeSubstitutions and handling nested generics like Box<Box<T>>.
    std::string ResolveTypeArgEntry(CFlatParser::TypeParameterEntryContext* entry)
    {
        auto* typeSpec = entry->typeSpecifier();
        bool hasPointer = entry->pointer() != nullptr;
        std::string resolved;

        if (typeSpec && typeSpec->genericIdentifier() && typeSpec->genericIdentifier()->genericTypeParameters())
        {
            // Nested generic (e.g., Box<T>): recurse into each type argument
            std::string innerBase = typeSpec->genericIdentifier()->Identifier()->getText();
            std::vector<std::string> innerArgs;
            for (auto* innerEntry : typeSpec->genericIdentifier()->genericTypeParameters()->typeParameterList()->typeParameterEntry())
                innerArgs.push_back(ResolveTypeArgEntry(innerEntry));
            resolved = MangledGenericName(innerBase, innerArgs);
        }
        else
        {
            // Simple type or type parameter: look up in activeTypeSubstitutions
            resolved = typeSpec ? typeSpec->getText() : entry->getText();
            auto substIt = activeTypeSubstitutions.find(resolved);
            if (substIt != activeTypeSubstitutions.end())
            {
                resolved = substIt->second;
                while (!resolved.empty() && resolved.back() == '*')
                {
                    resolved.pop_back();
                    hasPointer = true;
                }
            }
        }

        if (hasPointer)
        {
            if (Compiler(entry)->IsInterfaceType(resolved))
                LogErrorContext(entry, std::format("pointer '*' is not allowed on interface type '{}'", resolved));
            resolved += "*";
        }
        return resolved;
    }

    LLVMBackend::DeclTypeAndValue ParseDeclarationSpecifiers(CFlatParser::DeclarationSpecifiersContext* declSpecs)
    {
        LLVMBackend::DeclTypeAndValue declType;
        std::string typeName;
        auto declSpecList = declSpecs->declarationSpecifier();

        for (auto declSpec : declSpecList)
        {
            auto typeSpec = declSpec->typeSpecifier();
            auto storageSpec = declSpec->storageClassSpecifier();
            if (typeSpec != nullptr)
            {
                // 'move' and 'bond' are soft keywords parsed as Identifiers in typeSpecifier context
                if (typeSpec->getText() == "move")
                {
                    declType.IsMove = true;
                    continue;  // not a type; look for the actual type in next specifier
                }
                if (typeSpec->getText() == "bond")
                {
                    declType.IsBond = true;
                    continue;  // not a type; look for the actual type in next specifier
                }
                // tuple type sugar: (T1, T2) -> tuple<T1, T2>
                if (typeSpec->tupleTypeSpecifier() != nullptr)
                {
                    auto* tts = typeSpec->tupleTypeSpecifier();
                    std::vector<std::string> typeArgs;
                    if (tts->tupleTypePackEntry() != nullptr)
                    {
                        // (T...) - expand pack substitution
                        std::string packName = tts->tupleTypePackEntry()->typeSpecifier()->getText();
                        auto packIt = activePackSubstitutions.find(packName);
                        if (packIt != activePackSubstitutions.end())
                            typeArgs = packIt->second;
                        else
                            typeArgs.push_back(packName);
                    }
                    else
                    {
                        for (auto* entry : tts->tupleTypeEntry())
                        {
                            std::string argName = entry->typeSpecifier()->getText();
                            bool argPtr = entry->pointer() != nullptr;
                            // Apply active type substitutions (e.g. T -> int inside generic body)
                            auto substIt = activeTypeSubstitutions.find(argName);
                            if (substIt != activeTypeSubstitutions.end()) argName = substIt->second;
                            if (argPtr) argName += "*";
                            typeArgs.push_back(argName);
                        }
                    }
                    std::string mangledName = MangledGenericName("tuple", typeArgs);
                    declType.TypeName = mangledName;
                    // Queue instantiation if inside a generic context and not already done
                    if (!instantiatedGenerics.count(mangledName) &&
                        (genericStructTemplates.count("tuple") || genericClassTemplates.count("tuple")))
                    {
                        pendingInstantiations.push_back({"tuple", typeArgs, mangledName});
                        instantiatedGenerics.insert(mangledName);
                        auto* c = Compiler();
                        if (!c->GetDataStructure(mangledName).StructType)
                        {
                            c->CreateStructType(mangledName, {});
                            LLVMBackend::TypeAndValue rt{ .TypeName = mangledName };
                            c->CreateFunctionDeclaration(mangledName, rt, {});
                        }
                    }
                    declType.Pointer = declSpec->pointer() != nullptr;
                    declType.ArraySize = declSpec->arrayDimSpec() ? declSpec->arrayDimSpec()->assignmentExpression(0) : nullptr;
                    break;
                }
                // function pointer type: function<RetType(Params)> or bare 'function'
                if (typeSpec->functionPointerSpecifier() != nullptr)
                {
                    auto* fpSpec = typeSpec->functionPointerSpecifier();
                    declType.IsFunctionPointer = true;
                    if (fpSpec->typeSpecifier() != nullptr)
                    {
                        declType.FuncPtrReturnTypeName = fpSpec->typeSpecifier()->getText();
                        // Apply active generic type substitutions (e.g. T -> int inside list<int>)
                        {
                            auto substIt = activeTypeSubstitutions.find(declType.FuncPtrReturnTypeName);
                            if (substIt != activeTypeSubstitutions.end())
                            {
                                declType.FuncPtrReturnTypeName = substIt->second;
                                while (!declType.FuncPtrReturnTypeName.empty() && declType.FuncPtrReturnTypeName.back() == '*')
                                {
                                    declType.FuncPtrReturnTypeName.pop_back();
                                    declType.FuncPtrReturnPointer = true;
                                }
                            }
                        }
                        if (!declType.FuncPtrReturnPointer)
                            declType.FuncPtrReturnPointer = fpSpec->pointer() != nullptr;
                        if (fpSpec->functionPointerParamList() != nullptr)
                        {
                            for (auto* param : fpSpec->functionPointerParamList()->functionPointerParam())
                            {
                                LLVMBackend::TypeAndValue::FuncPtrParam p;
                                p.TypeName = param->typeSpecifier()->getText();
                                // Apply active generic type substitutions; strip trailing * into Pointer flag
                                {
                                    auto substIt = activeTypeSubstitutions.find(p.TypeName);
                                    if (substIt != activeTypeSubstitutions.end())
                                    {
                                        p.TypeName = substIt->second;
                                        while (!p.TypeName.empty() && p.TypeName.back() == '*')
                                        {
                                            p.TypeName.pop_back();
                                            p.Pointer = true;
                                        }
                                    }
                                }
                                if (!p.Pointer)
                                    p.Pointer = param->pointer() != nullptr;
                                p.IsMove = param->Move() != nullptr;
                                declType.FuncPtrParams.push_back(p);
                            }
                        }
                    }
                    // For bare 'function', signature inferred from initializer at declaration site
                    break;
                }
                if (typeSpec->genericIdentifier() != nullptr && typeSpec->genericIdentifier()->genericTypeParameters() != nullptr)
                {
                    // Generic type instantiation: Box<MyType> -> Box__MyType
                    std::string baseName = typeSpec->genericIdentifier()->Identifier()->getText();
                    std::vector<std::string> typeArgs;
                    for (auto* entry : typeSpec->genericIdentifier()->genericTypeParameters()->typeParameterList()->typeParameterEntry())
                        typeArgs.push_back(ResolveTypeArgEntry(entry));
                    std::string mangledName = MangledGenericName(baseName, typeArgs);
                    declType.TypeName = mangledName;
                    // Queue instantiation of nested generic types discovered during field/param parsing.
                    // Only do this when inside an active instantiation context (substitutions are set),
                    // to avoid treating unresolved type parameters (e.g. "T") as concrete types.
                    // Top-level explicit uses (e.g. hashset<int> in user code) are handled by
                    // ForwardRefScanner::ScanGenericTypeUses and ScanAndQueueGenericTypeUses.
                    if (!activeTypeSubstitutions.empty() && !instantiatedGenerics.count(mangledName))
                    {
                        bool isKnownTemplate = genericStructTemplates.count(baseName) || genericClassTemplates.count(baseName);
                        if (isKnownTemplate)
                        {
                            pendingInstantiations.push_back({baseName, typeArgs, mangledName});
                            instantiatedGenerics.insert(mangledName);
                            auto* c = Compiler();
                            if (!c->GetDataStructure(mangledName).StructType)
                            {
                                c->CreateStructType(mangledName, {});
                                LLVMBackend::TypeAndValue rt{ .TypeName = mangledName };
                                c->CreateFunctionDeclaration(mangledName, rt, {});
                            }
                        }
                    }
                }
                else
                {
                    typeName = typeSpec->getText();
                    // Apply active type parameter substitutions (e.g. T -> int inside a template body)
                    bool substPointer = false;
                    auto substIt = activeTypeSubstitutions.find(typeName);
                    if (substIt != activeTypeSubstitutions.end())
                    {
                        typeName = substIt->second;
                        // If substituted type includes pointer suffix (e.g. "Employee*"), extract it
                        while (!typeName.empty() && typeName.back() == '*')
                        {
                            typeName.pop_back();
                            substPointer = true;
                        }
                    }
                    // Resolve namespace-qualified type names (alias expansion + parent namespace search)
                    typeName = Compiler(declSpecs)->ResolveQualifiedName(typeName);
                    // If still unresolved, try qualifying with the enclosing struct scope (e.g. Inner -> Outer.Inner)
                    if (!structScopeStack.empty() && !Compiler(declSpecs)->IsDataStructure(typeName))
                    {
                        std::string qualified = structScopeStack.back() + "." + typeName;
                        if (Compiler(declSpecs)->IsDataStructure(qualified))
                            typeName = qualified;
                    }
                    // Resolve type aliases (e.g. user-defined aliases)
                    typeName = Compiler(declSpecs)->ResolveTypeAlias(typeName);
                    declType.TypeName = typeName;
                    if (substPointer) declType.Pointer = true;
                }
                bool hasExplicitPointer = declSpec->pointer() != nullptr;
                bool hasDblPointer = hasExplicitPointer && declSpec->pointer()->Star().size() >= 2;
                bool substPointer = declType.Pointer; // T was already a pointer (e.g. T=IMessage*)
                // Pointer-to-pointer: explicit ** OR (substituted type is a pointer AND explicit *)
                if (hasDblPointer || (declType.Pointer && hasExplicitPointer))
                    declType.ElemPointer = true;
                declType.Pointer = hasExplicitPointer || declType.Pointer;
                if (declSpec->arrayDimSpec())
                {
                    auto dims = declSpec->arrayDimSpec()->assignmentExpression();
                    declType.ArraySize = dims.empty() ? nullptr : dims[0];
                    for (size_t di = 1; di < dims.size(); di++)
                        declType.ExtraArrayDims.push_back(dims[di]);
                }
                declType.IsInterface = Compiler(declSpecs)->IsInterfaceType(declType.TypeName);
                if (declType.IsInterface && hasExplicitPointer && activeTypeSubstitutions.empty())
                    LogErrorContext(declSpec, std::format("pointer '*' is not allowed on interface type '{}'", declType.TypeName));
                if (declType.IsInterface)
                {
                    // IsInterfacePointer: this represents a pointer TO a fat-ptr, not the fat-ptr itself.
                    // True when T* where T=IFace (hasExplicitPointer), or T where T=IFace* (substPointer).
                    declType.IsInterfacePointer = hasExplicitPointer || substPointer;
                    if (declType.IsInterfacePointer)
                        declType.Pointer = true;
                }
                if (declSpec->Question())
                {
                    if (declType.IsPrimitive())
                        LogErrorContext(declSpec, std::format("nullable '?' is not allowed on primitive type '{}'", declType.TypeName));
                    else
                    {
                        declType.IsNullable = true;
                        declType.Pointer = true;
                    }
                }
                break;
            }
            else if (storageSpec)
            {
                declType.external = storageSpec->Extern() != nullptr;
                declType.threadLocal = storageSpec->ThreadLocal() != nullptr;
            }
            else if (auto funcSpec = declSpec->functionSpecifier())
            {
                if (funcSpec->getText() == "stdcall")
                    declType.IsStdcall = true;
                else if (funcSpec->getText() == "cdecl")
                    declType.IsCdecl = true;
            }
            else if (auto alignSpec = declSpec->alignmentSpecifier())
            {
                uint64_t alignVal = ParseAlignmentSpecifier(alignSpec);
                if (alignVal != 0)
                    declType.UserAlignValue = alignVal;
            }
        }

        return declType;
    }

    // Evaluate `alignas(N)` / `alignas(T)` into a power-of-two byte count.
    // Validates: power of two, 1 <= N <= 4096. Returns 0 on error (and logs).
    uint64_t ParseAlignmentSpecifier(CFlatParser::AlignmentSpecifierContext* alignSpec)
    {
        uint64_t alignVal = 0;
        if (auto* typeName = alignSpec->typeName())
        {
            LLVMBackend::DeclTypeAndValue dt;
            dt.TypeName = typeName->getText();
            llvm::Type* t = compilerLLVM->GetType(dt);
            if (t == nullptr || !t->isSized())
            {
                LogErrorContext(alignSpec, std::format("alignas: cannot resolve type '{}'", dt.TypeName));
                return 0;
            }
            alignVal = compilerLLVM->module->getDataLayout().getABITypeAlign(t).value();
        }
        else if (auto* cexpr = alignSpec->constantExpression())
        {
            auto* cond = cexpr->conditionalExpression();
            llvm::Value* condVal = ParseConditionalExpression(cond);
            auto* ci = llvm::dyn_cast_or_null<llvm::ConstantInt>(condVal);
            if (ci == nullptr)
            {
                LogErrorContext(alignSpec, "alignas: argument must be a constant integer expression");
                return 0;
            }
            int64_t signedVal = ci->getSExtValue();
            if (signedVal <= 0)
            {
                LogErrorContext(alignSpec, std::format("alignas: value must be positive (got {})", signedVal));
                return 0;
            }
            alignVal = (uint64_t)signedVal;
        }
        if (alignVal == 0) return 0;
        if (alignVal > 4096)
        {
            LogErrorContext(alignSpec, std::format("alignas: value {} exceeds maximum of 4096", alignVal));
            return 0;
        }
        if ((alignVal & (alignVal - 1)) != 0)
        {
            LogErrorContext(alignSpec, std::format("alignas: value {} is not a power of two", alignVal));
            return 0;
        }
        return alignVal;
    }

    LLVMBackend::DeclTypeAndValue getFunctionReturnType(CFlatParser::FunctionDefinitionContext* ctx)
    {
        auto declSpecs = ctx->declarationSpecifiers();

        return ParseDeclarationSpecifiers(declSpecs);
    }

    // Returns the default value for a type:
    //   - struct types (local scope): calls the default constructor.
    //   - everything else (or global scope): zero-initializes.
    llvm::Value* GenerateDefaultValue(const LLVMBackend::DeclTypeAndValue& typeValue)
    {
        auto* compiler = Compiler();
        // Apply active type-parameter substitutions as a fallback in case the caller
        // hasn't already resolved them (e.g. "V" inside a generic method body).
        auto resolved = typeValue;
        auto substIt = activeTypeSubstitutions.find(resolved.TypeName);
        if (substIt != activeTypeSubstitutions.end())
        {
            resolved.TypeName = substIt->second;
            while (!resolved.TypeName.empty() && resolved.TypeName.back() == '*')
            {
                resolved.TypeName.pop_back();
                resolved.Pointer = true;
            }
        }

        auto* llvmType = compiler->GetType(resolved);
        if (!llvmType) return nullptr;

        if (!resolved.Pointer && llvmType->isStructTy() && !global_scope)
        {
            auto structData = compiler->GetDataStructure(resolved.TypeName);
            if (structData.StructType != nullptr && compiler->GetFunction(resolved.TypeName))
                return compiler->CreateOverloadedFunctionCall(resolved.TypeName, {});
        }

        return llvm::Constant::getNullValue(llvmType);
    }

public:
    MainListener(CFlatParser* parser, LLVMBackend* compilerLLVM, const std::string& filename)
        : genericStructTemplates(compilerLLVM->gts.genericStructTemplates)
        , genericClassTemplates(compilerLLVM->gts.genericClassTemplates)
        , genericStructTypeParams(compilerLLVM->gts.genericStructTypeParams)
        , instantiatedGenerics(compilerLLVM->gts.instantiatedGenerics)
        , genericStructConstraints(compilerLLVM->gts.genericStructConstraints)
        , genericClassConstraints(compilerLLVM->gts.genericClassConstraints)
        , genericStructPackIndex(compilerLLVM->gts.genericStructPackIndex)
        , genericClassPackIndex(compilerLLVM->gts.genericClassPackIndex)
        , genericFunctionPackIndex(compilerLLVM->gts.genericFunctionPackIndex)
        , genericInterfacePackIndex(compilerLLVM->gts.genericInterfacePackIndex)
        , genericInterfaceTemplates(compilerLLVM->gts.genericInterfaceTemplates)
        , genericInterfaceTypeParams(compilerLLVM->gts.genericInterfaceTypeParams)
        , instantiatedInterfaces(compilerLLVM->gts.instantiatedInterfaces)
        , genericFunctionTemplates(compilerLLVM->gts.genericFunctionTemplates)
        , genericFunctionTypeParams(compilerLLVM->gts.genericFunctionTypeParams)
        , instantiatedGenericFunctions(compilerLLVM->gts.instantiatedGenericFunctions)
        , genericFunctionConstraints(compilerLLVM->gts.genericFunctionConstraints)
        , pendingInstantiations(compilerLLVM->gts.pendingInstantiations)
    {
        this->parser = parser;
        this->compilerLLVM = compilerLLVM;
        this->sourceFileName = filename;
    }

    void SetImportNamespace(const std::string& ns) { importNamespace_ = ns; }

    // Returns the BufferedTokenStream that backs this listener's parser, so that
    // helpers like ExtractLeadingDoc can reach the hidden-channel comment tokens.
    antlr4::BufferedTokenStream* GetTokens() const
    {
        return parser ? dynamic_cast<antlr4::BufferedTokenStream*>(parser->getTokenStream()) : nullptr;
    }

    void ParseInterfaceDefinition(CFlatParser::InterfaceDefinitionContext* ctx)
    {
        auto* nameGid = ctx->genericIdentifier();
        if (!nameGid || !nameGid->Identifier()) return;

        std::string name = nameGid->Identifier()->getText();

        // Collect parent interface names from direct Identifier terminals (after ':')
        std::vector<std::string> parentNames;
        for (auto* term : ctx->Identifier())
            parentNames.push_back(term->getText());

        // Generic interface template - store for on-demand instantiation
        if (nameGid->genericTypeParameters() != nullptr)
        {
            auto typeParams = ParseGenericTypeParameters(nameGid->genericTypeParameters());
            genericInterfaceTemplates[name] = ctx;
            genericInterfaceTypeParams[name] = typeParams;
            {
                auto entries = nameGid->genericTypeParameters()->typeParameterList()->typeParameterEntry();
                bool hasPack = !entries.empty() && entries.back()->Ellipsis() != nullptr;
                genericInterfacePackIndex[name] = hasPack ? (typeParams.size() - 1) : std::string::npos;
            }
            return;
        }

        std::vector<LLVMBackend::InterfaceMethod> methods;
        for (auto method : ctx->interfaceMethod())
        {
            LLVMBackend::InterfaceMethod m;
            m.ReturnType = ParseDeclarationSpecifiers(method->declarationSpecifiers());
            m.Name = getInterfaceMethodName(method);
            auto declParams = ParseParameterTypeList(method->parameterTypeList());
            for (const auto& p : declParams)
            {
                LLVMBackend::TypeAndValue tv = p;
                m.Parameters.push_back(tv);
            }
            methods.push_back(std::move(m));
        }

        Compiler(ctx)->CreateInterfaceDefinition(name, parentNames, methods);
    }

    void InstantiateGenericInterface(const std::string& baseName, const std::string& mangledName,
                                     const std::unordered_map<std::string, std::string>& substitutions,
                                     const std::unordered_map<std::string, std::vector<std::string>>& packSubstitutions = {})
    {
        if (instantiatedInterfaces.count(mangledName)) return;
        instantiatedInterfaces.insert(mangledName);

        auto templateIt = genericInterfaceTemplates.find(baseName);
        if (templateIt == genericInterfaceTemplates.end()) return;

        auto* ctx = templateIt->second;

        // Collect parent interface names
        std::vector<std::string> parentNames;
        for (auto* term : ctx->Identifier())
            parentNames.push_back(term->getText());

        // Apply substitutions to instantiate the interface methods
        auto savedSubst = activeTypeSubstitutions;
        auto savedPackSubst = activePackSubstitutions;
        for (const auto& [k, v] : substitutions)
            activeTypeSubstitutions[k] = v;
        for (const auto& [k, v] : packSubstitutions)
            activePackSubstitutions[k] = v;

        std::vector<LLVMBackend::InterfaceMethod> methods;
        for (auto method : ctx->interfaceMethod())
        {
            LLVMBackend::InterfaceMethod m;
            m.ReturnType = ParseDeclarationSpecifiers(method->declarationSpecifiers());
            m.Name = getInterfaceMethodName(method);
            auto declParams = ParseParameterTypeList(method->parameterTypeList());
            for (const auto& p : declParams)
            {
                LLVMBackend::TypeAndValue tv = p;
                m.Parameters.push_back(tv);
            }
            methods.push_back(std::move(m));
        }

        activeTypeSubstitutions = savedSubst;
        activePackSubstitutions = savedPackSubst;
        Compiler()->CreateInterfaceDefinition(mangledName, parentNames, methods);
    }

    // Instantiate a generic function template with concrete type arguments.
    // Returns the mangled name of the instantiated function, or empty string on failure.
    std::string InstantiateGenericFunction(const std::string& baseName, const std::vector<std::string>& typeArgs)
    {
        std::string mangledName = MangledGenericName(baseName, typeArgs);
        if (instantiatedGenericFunctions.count(mangledName)) return mangledName;
        instantiatedGenericFunctions.insert(mangledName);

        auto templateIt = genericFunctionTemplates.find(baseName);
        if (templateIt == genericFunctionTemplates.end()) return {};

        const auto& typeParams = genericFunctionTypeParams[baseName];
        if (typeParams.size() != typeArgs.size()) return {};

        if (!CheckConstraints(baseName, typeParams, typeArgs, genericFunctionConstraints, templateIt->second))
            return {};

        auto savedSubst = activeTypeSubstitutions;
        for (size_t i = 0; i < typeParams.size(); i++)
            activeTypeSubstitutions[typeParams[i]] = typeArgs[i];

        // Save the current IRBuilder insertion point so that emitting a new
        // function definition mid-block does not corrupt the caller's block.
        auto* instCompiler = Compiler(templateIt->second);
        auto savedState = instCompiler->SaveBuilderState();
        // Isolate the outer function's local-variable stack: if we are
        // instantiating mid-emission of another function, its frames are still
        // on stackNamedVariable, and the generic body's identifier lookup would
        // otherwise reach past its own frame and bind to the outer's locals
        // (e.g. parameter 'a' shadowed by outer 'int a' -> cross-function load
        // and "Instruction does not dominate all uses" verifier error).
        auto savedStack = std::move(instCompiler->stackNamedVariable);
        instCompiler->stackNamedVariable.clear();
        ParseFunctionDefinition(templateIt->second, {}, {}, mangledName);
        instCompiler->stackNamedVariable = std::move(savedStack);
        instCompiler->RestoreBuilderState(savedState);

        activeTypeSubstitutions = savedSubst;
        return mangledName;
    }

    // Infer the type arguments for a generic function template from the receiver's
    // interface type name (e.g. "Container__int" -> T="int") and instantiate it.
    // Returns the mangled name of the instantiated function, or empty string on failure.
    std::string InferAndInstantiateGenericFunction(const std::string& funcName, const std::string& receiverType)
    {
        auto templateIt = genericFunctionTemplates.find(funcName);
        if (templateIt == genericFunctionTemplates.end()) return {};

        auto* funcCtx = templateIt->second;
        const auto& typeParams = genericFunctionTypeParams[funcName];

        auto* paramTypeList = funcCtx->parameterTypeList();
        if (!paramTypeList || !paramTypeList->parameterList()) return {};

        auto paramDecls = paramTypeList->parameterList()->parameterDeclaration();
        if (paramDecls.empty()) return {};

        // Examine the first parameter's type specifier to determine the base interface name
        for (auto* declSpec : paramDecls[0]->declarationSpecifiers()->declarationSpecifier())
        {
            auto* typeSpec = declSpec->typeSpecifier();
            if (!typeSpec || !typeSpec->genericIdentifier()) continue;

            auto* genId = typeSpec->genericIdentifier();
            if (!genId->Identifier()) continue;

            std::string baseName = genId->Identifier()->getText();
            std::string prefix = baseName + "__";

            if (receiverType.size() <= prefix.size() || receiverType.substr(0, prefix.size()) != prefix)
                continue;

            std::string suffix = receiverType.substr(prefix.size());

            // With a single type parameter the entire suffix is the type argument.
            // With multiple type parameters split on "__" (works for simple non-generic args).
            std::vector<std::string> typeArgs;
            if (typeParams.size() == 1)
            {
                typeArgs = { suffix };
            }
            else
            {
                size_t pos = 0;
                while (pos < suffix.size())
                {
                    size_t next = suffix.find("__", pos);
                    if (next == std::string::npos)
                    {
                        typeArgs.push_back(suffix.substr(pos));
                        break;
                    }
                    typeArgs.push_back(suffix.substr(pos, next - pos));
                    pos = next + 2;
                }
            }

            if (typeArgs.size() == typeParams.size())
                return InstantiateGenericFunction(funcName, typeArgs);
        }
        return {};
    }

    // Infer type arguments for a generic function from call argument types and instantiate it.
    // Handles simple positional matching: where parameter type == type param name, bind to arg TypeName.
    std::string TryInferAndInstantiateFromArgs(const std::string& funcName,
                                               const std::vector<LLVMBackend::NamedVariable>& args)
    {
        auto templateIt = genericFunctionTemplates.find(funcName);
        if (templateIt == genericFunctionTemplates.end()) return {};

        auto* funcCtx = templateIt->second;
        const auto& typeParams = genericFunctionTypeParams[funcName];
        if (typeParams.empty()) return {};

        auto* paramTypeList = funcCtx->parameterTypeList();
        if (!paramTypeList || !paramTypeList->parameterList()) return {};
        auto paramDecls = paramTypeList->parameterList()->parameterDeclaration();

        std::unordered_map<std::string, std::string> inferred;
        for (size_t i = 0; i < paramDecls.size() && i < args.size(); i++)
        {
            std::string paramTypeName;
            for (auto* ds : paramDecls[i]->declarationSpecifiers()->declarationSpecifier())
            {
                auto* ts = ds->typeSpecifier();
                if (!ts || !ts->genericIdentifier()) continue;
                auto* gid = ts->genericIdentifier();
                if (gid->Identifier()) { paramTypeName = gid->Identifier()->getText(); break; }
            }
            for (const auto& tp : typeParams)
            {
                if (paramTypeName == tp)
                {
                    std::string argType = args[i].TypeAndValue.TypeName;
                    // Free-function calls drop TypeName for signed-int args (call site only
                    // preserves it for unsigned ints). Fall back to the LLVM BaseType so
                    // numeric literals like 3 and (i64)10 still infer cleanly.
                    if (argType.empty() && args[i].BaseType != nullptr)
                        argType = Compiler()->LlvmTypeToTypeName(args[i].BaseType);
                    if (!argType.empty()) inferred[tp] = argType;
                    break;
                }
            }
        }

        if (inferred.size() != typeParams.size()) return {};

        std::vector<std::string> typeArgs;
        for (const auto& tp : typeParams)
        {
            auto it = inferred.find(tp);
            if (it == inferred.end()) return {};
            typeArgs.push_back(it->second);
        }
        return InstantiateGenericFunction(funcName, typeArgs);
    }

    void ParseUsingDeclaration(CFlatParser::UsingDeclarationContext* ctx)
    {
        auto* compiler = Compiler(ctx);

        // The alias name may be a plain Identifier or the 'string' keyword token.
        std::string alias;
        if (ctx->String())
            alias = ctx->String()->getText();
        else if (ctx->Identifier() != nullptr)
            alias = ctx->Identifier()->getText();

        std::string target = ctx->typeSpecifier()->getText();

        // If the target names a known type (interface, struct, or primitive), register a type alias.
        // Otherwise treat it as a namespace alias.
        if (compiler->IsInterfaceType(target) || compiler->GetDataStructure(target).StructType != nullptr
            || LLVMBackend::IsPrimitiveTypeName(target))
        {
            compiler->RegisterTypeAlias(alias, target);
            if (auto* s = compiler->GetSymbolSink())
                s->Register(SymbolKind::TypeAlias, alias, compiler->GetSourceFilePath(),
                            (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine(),
                            "using " + alias + " = " + target, {},
                            ExtractLeadingDoc(GetTokens(), ctx->getStart()));
        }
        else if (global_scope)
            compiler->RegisterNamespaceAlias(alias, target);
        else
            compiler->RegisterLocalNamespaceAlias(alias, target);
    }

    void ParseAnnotationDefinition(CFlatParser::AnnotationDefinitionContext* ctx)
    {
        auto* compiler = Compiler(ctx);
        std::string name = ctx->Identifier()->getText();

        // Validate: no duplicate declaration
        if (compiler->annotationRegistry.count(name))
        {
            // Already registered by ForwardRefScanner - nothing to emit (no LLVM type).
            return;
        }

        // Fallback registration in case ForwardRefScanner missed it (shouldn't happen).
        std::vector<std::string> fields;
        for (auto* decl : ctx->declaration())
        {
            if (auto* initList = decl->initDeclaratorList())
                for (auto* initDecl : initList->initDeclarator())
                    if (auto* dir = initDecl->declarator())
                        fields.push_back(getDirectDeclName(dir->directDeclarator()));
        }
        compiler->annotationRegistry[name] = fields;
    }

    void ParseExternalDeclaration(CFlatParser::ExternalDeclarationContext* ctx, const std::string& namespaceName = {})
    {
        auto func = ctx->functionDefinition();
        auto dataStruct = ctx->structDefinition();
        auto classDef = ctx->classDefinition();
        auto decl = ctx->declaration();
        auto iface = ctx->interfaceDefinition();
        auto ns = ctx->namespaceDefinition();
        auto usingDecl = ctx->usingDeclaration();
        auto ifConst = ctx->ifConstDeclaration();

        if (auto annDef = ctx->annotationDefinition())
        {
            ParseAnnotationDefinition(annDef);
            return;
        }

        if (iface != nullptr)
        {
            ParseInterfaceDefinition(iface);
        }
        else if (ns != nullptr)
        {
            ParseNamespaceDefinition(ns, namespaceName);
        }
        else if (usingDecl != nullptr)
        {
            ParseUsingDeclaration(usingDecl);
        }
        else if (ifConst != nullptr)
        {
            ParseIfConstDeclaration(ifConst, namespaceName);
        }
        else if (decl != nullptr)
        {
            ParseDeclaration(decl);
        }
        else if (func != nullptr)
        {
            global_scope = false;
            ParseFunctionDefinition(func, {}, namespaceName);
            global_scope = true;
        }
        else if (dataStruct != nullptr)
        {
            ParseStructDefinition(dataStruct, {}, namespaceName);
        }
        else if (classDef != nullptr)
        {
            ParseClassDefinition(classDef, {}, namespaceName);
        }
        else if (auto progDef = ctx->programDefinition())
        {
            ParseProgramDefinition(progDef);
        }
        else if (auto imp = ctx->importDeclaration())
        {
            if (imp->children.size() >= 2 && imp->children[1]->getText() == "program")
            {
                std::string alias = imp->Identifier()->getText();
                ParseImportedProgramDefinition(alias);
            }
        }
        else if (auto expectErrDecl = ctx->expectErrorDeclaration())
        {
            // Scoped block form at file scope: expect_error("msg") { functionDef / structDef / ... }
            std::string rawText = expectErrDecl->StringLiteral()->getText();
            compilerLLVM->expectedError = ProcessRawText(rawText);
            compilerLLVM->expectedErrorScopeDepth = SIZE_MAX;

            bool errorReceived = false;
            try
            {
                for (auto* extDecl : expectErrDecl->externalDeclaration())
                    ParseExternalDeclaration(extDecl, namespaceName);
            }
            catch (const ExpectedErrorReceived&)
            {
                errorReceived = true;
                compilerLLVM->AbortFunctionBlocks(0);
                compilerLLVM->ClearCurrentSubprogram();
                compilerLLVM->expectedError.clear();
                compilerLLVM->expectedErrorScopeDepth = SIZE_MAX;
            }

            if (!errorReceived && !compilerLLVM->expectedError.empty())
            {
                std::cout << std::format("FAIL: expected error '{}' did not occur\n",
                                          compilerLLVM->expectedError);
                compilerLLVM->expectedError.clear();
                if (compilerLLVM->diagnosticSink_)
                    throw CompilerAbortException{ "expected error did not occur", compilerLLVM->sourceFileName, 0, 0 };
                else
                    compilerLLVM->FailCompilation("expected error did not occur");
            }
        }

        // Process any generic instantiations queued while parsing the above item.
        // This is the only safe point: the IRBuilder has no active function/block.
        ProcessPendingInstantiations();
    }

    void ParseIfConstDeclaration(CFlatParser::IfConstDeclarationContext* ctx, const std::string& namespaceName = {})
    {
        auto expression = ctx->expression();
        auto condition = ParseExpression(expression);
        auto constInt = llvm::dyn_cast<llvm::ConstantInt>(condition);
        if (!constInt)
        {
            LogErrorContext(ctx, "'if const' condition must be a compile-time constant expression");
            return;
        }

        bool taken = constInt->getZExtValue() != 0;
        auto ifBlocks = ctx->ifConstBlock();

        if (ifBlocks.empty())
            return;

        CFlatParser::IfConstBlockContext* branchBlock = nullptr;
        if (taken)
        {
            branchBlock = ifBlocks[0];
        }
        else if (ifBlocks.size() > 1)
        {
            branchBlock = ifBlocks[1];
        }

        if (!branchBlock)
            return;

        auto branchDecls = branchBlock->externalDeclaration();

        // Process any imports in the taken branch before forward-scanning it.
        for (auto* extDecl : branchDecls)
        {
            if (auto* imp = extDecl->importDeclaration())
            {
                std::string raw = imp->StringLiteral()->getText();
                std::string importFilename = raw.substr(1, raw.size() - 2);
                // `import package-vcpkg "header" from "port";` inside an if-const branch.
                if (imp->children.size() >= 2 && imp->children[1]->getText() == "package-vcpkg")
                {
                    std::string portSpec;
                    if (auto* fc = imp->fromClause())
                        if (fc->StringLiteral())
                        {
                            std::string fr = fc->StringLiteral()->getText();
                            if (fr.size() >= 2) portSpec = fr.substr(1, fr.size() - 2);
                        }
                    Compiler()->CompileVcpkgImport(Compiler()->RootVcpkgImportPath(Compiler()->currentSourceFilePath_), importFilename, portSpec);
                    continue;
                }
                std::string ns = imp->Identifier() ? imp->Identifier()->getText() : "";
                std::string explicitLib;
                if (auto* lc = imp->libClause())
                    if (lc->StringLiteral())
                    {
                        std::string lr = lc->StringLiteral()->getText();
                        if (lr.size() >= 2) explicitLib = lr.substr(1, lr.size() - 2);
                    }
                std::vector<std::string> extraDefines;
                for (auto* dc : imp->defineClause())
                    if (dc->StringLiteral())
                    {
                        std::string dr = dc->StringLiteral()->getText();
                        if (dr.size() >= 2) extraDefines.push_back(dr.substr(1, dr.size() - 2));
                    }
                Compiler()->CompileImportedFile(Compiler()->currentSourceFilePath_, importFilename, ns, "", explicitLib, extraDefines);
            }
        }

        // First pass: forward ref scan the taken branch to register symbols
        ForwardRefScanner scanner(Compiler());
        scanner.SetTokens(GetTokens());
        for (auto* extDecl : branchDecls)
            scanner.ScanExternalDeclaration(extDecl, namespaceName);

        // Second pass: generate code for the taken branch
        for (auto* extDecl : branchDecls)
            ParseExternalDeclaration(extDecl, namespaceName);
    }

    void ParseNamespaceDefinition(CFlatParser::NamespaceDefinitionContext* ctx, const std::string& parentNamespace = {})
    {
        std::string namespaceName = ctx->Identifier()->getText();
        if (!parentNamespace.empty())
            namespaceName = parentNamespace + "." + namespaceName;
        auto* compiler = Compiler(ctx);
        compiler->RegisterNamespace(namespaceName);

        if (auto* s = compiler->GetSymbolSink())
        {
            std::string doc = ExtractLeadingDoc(GetTokens(), ctx->getStart());
            s->Register(SymbolKind::Namespace, namespaceName, compiler->GetSourceFilePath(),
                        (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine(),
                        "namespace " + namespaceName, {}, doc);
            // Also register under the unqualified name so Lookup("Inner") finds "Outer.Inner".
            size_t dot = namespaceName.rfind('.');
            if (dot != std::string::npos)
                s->Register(SymbolKind::Namespace, namespaceName.substr(dot + 1), compiler->GetSourceFilePath(),
                            (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine(),
                            "namespace " + namespaceName, {}, doc);
        }

        for (auto* extDecl : ctx->externalDeclaration())
            ParseExternalDeclaration(extDecl, namespaceName);
    }

    void enterExternalDeclaration(CFlatParser::ExternalDeclarationContext* ctx) override
    {
        if constexpr (debugPrint)
            return;

        // Skip nodes nested inside a namespace - they are handled by ParseNamespaceDefinition.
        if (dynamic_cast<CFlatParser::NamespaceDefinitionContext*>(ctx->parent))
            return;

        // Skip nodes nested inside an if const block - they are handled by ParseIfConstDeclaration.
        if (dynamic_cast<CFlatParser::IfConstBlockContext*>(ctx->parent))
            return;

        // Skip nodes nested inside an expect_error block - handled by ParseExternalDeclaration's
        // expectErrorDeclaration branch, which processes them manually after setting expectedError.
        if (dynamic_cast<CFlatParser::ExpectErrorDeclarationContext*>(ctx->parent))
            return;

        ParseExternalDeclaration(ctx);
    }

    void ParseBlockItemList(CFlatParser::BlockItemListContext* ctx)
    {
        auto blockItems = ctx->blockItem();

        for (const auto& blockItem : blockItems)
        {
            auto decl = blockItem->declaration();
            auto statement = blockItem->statement();
            auto usingDecl = blockItem->usingDeclaration();
            auto destructuring = blockItem->destructuringDeclaration();

            auto* compiler = Compiler(blockItem);
            if (decl != nullptr)
            {
                compiler->SetCurrentDebugLocation(decl->getStart()->getLine());
                ParseDeclaration(decl);
            }
            else if (statement != nullptr)
            {
                ParseStatement(statement);
            }
            else if (usingDecl != nullptr)
            {
                compiler->SetCurrentDebugLocation(usingDecl->getStart()->getLine());
                ParseUsingDeclaration(usingDecl);
            }
            else if (destructuring != nullptr)
            {
                compiler->SetCurrentDebugLocation(destructuring->getStart()->getLine());
                ParseDestructuringDeclaration(destructuring);
            }
        }
    }

    void ParseDestructuringDeclaration(CFlatParser::DestructuringDeclarationContext* ctx)
    {
        auto* compiler = Compiler(ctx);

        // Evaluate the RHS once
        auto rhsNV = ParseAssignmentExpressionNamed(ctx->assignmentExpression());
        std::string rhsType = rhsNV.TypeAndValue.TypeName;

        // Verify the RHS is a tuple type (mangled name starts with "tuple__")
        if (rhsType.substr(0, 7) != "tuple__")
        {
            LogErrorContext(ctx, std::format("Destructuring requires a tuple type, got '{}'", rhsType));
            return;
        }

        auto* structType = compiler->GetDataStructure(rhsType).StructType;
        if (!structType)
        {
            LogErrorContext(ctx, std::format("Tuple type '{}' is not fully instantiated", rhsType));
            return;
        }

        // Load the tuple value into a temporary alloca so we can GEP its fields
        llvm::Value* tupleAlloca = rhsNV.Storage;
        if (!tupleAlloca)
        {
            // RHS was a value not stored - create a temp alloca
            tupleAlloca = compiler->CreateAlloca(structType);
            compiler->builder->CreateStore(LoadNamedVariable(rhsNV), tupleAlloca);
        }

        const auto& structData = compiler->GetDataStructure(rhsType);
        auto entries = ctx->destructuringEntry();

        if (entries.size() != structData.StructFields.size())
        {
            LogErrorContext(ctx, std::format("Destructuring arity mismatch: {} variables for {} fields",
                entries.size(), structData.StructFields.size()));
            return;
        }

        // Declare each variable and load from the corresponding item_i field
        for (size_t i = 0; i < entries.size(); i++)
        {
            auto* entry = entries[i];
            auto declType = ParseDeclarationSpecifiers(entry->declarationSpecifiers());
            std::string varName = entry->Identifier()->getText();

            std::string fieldName = "item_" + std::to_string(i);
            unsigned fieldIdx = 0;
            for (const auto& f : structData.StructFields)
            {
                if (f.VariableName == fieldName) break;
                fieldIdx++;
            }

            auto* gep = compiler->CreateStructGEP(structType, tupleAlloca, fieldIdx);
            auto* fieldLLVMType = compiler->GetType(structData.StructFields[fieldIdx]);
            auto* fieldVal = compiler->builder->CreateLoad(fieldLLVMType, gep);

            // Allocate and store into the new variable
            auto* alloca = compiler->CreateAlloca(fieldLLVMType);
            compiler->builder->CreateStore(fieldVal, alloca);

            declType.VariableName = varName;
            LLVMBackend::NamedVariable namedVar;
            namedVar.TypeAndValue = declType;
            namedVar.Storage = alloca;
            namedVar.BaseType = fieldLLVMType;
            compiler->stackNamedVariable.back().namedVariable[varName] = namedVar;
        }
    }

    // Recursively collects case/default labels from a statement (to handle `case 1: case 2: stmt` nesting).
    void CollectCasesFromStatement(CFlatParser::StatementContext* stmt, SwitchContext& ctx)
    {
        auto* compiler = Compiler(stmt);
        if (!stmt) return;
        auto labeled = stmt->labeledStatement();
        if (!labeled) return;

        bool hasArrow = labeled->FatArrow() != nullptr;

        // Reject mixing C-style (:) and arm-style (=>) cases in the same switch.
        if (hasArrow && !ctx.caseMap.empty() && !ctx.isArmStyle)
            LogErrorContext(labeled, "cannot mix ':' and '=>' cases in the same switch");
        if (!hasArrow && !ctx.caseMap.empty() && ctx.isArmStyle)
            LogErrorContext(labeled, "cannot mix ':' and '=>' cases in the same switch");
        if (hasArrow)
            ctx.isArmStyle = true;

        if (labeled->Case())
        {
            // Arm-style type pointer case: case TypeName* optVar => body
            if (labeled->typeSpecifier() && labeled->pointer())
            {
                std::string typeName = labeled->typeSpecifier()->getText();
                std::string boundVar = labeled->Identifier() ? labeled->Identifier()->getText() : "";
                bool isStruct = compiler->dataStructures.count(typeName) > 0;
                bool isInterface = compiler->interfaceTable.count(typeName) > 0;
                if (!isStruct && !isInterface)
                    LogErrorContext(labeled, std::format("'{}' is not a known struct or interface type", typeName));
                if (!ctx.isTypeSwitch && !ctx.caseMap.empty())
                    LogErrorContext(labeled, "cannot mix type cases with constant cases in a switch");
                ctx.isTypeSwitch = true;
                ctx.caseMap[labeled] = { nullptr, nullptr, compiler->CreateBasicBlock("switchTypeCase"), true, typeName, boundVar, true };
                return;
            }

            // C-style or arm-style value/type case via constantExpression
            auto constExpr = labeled->constantExpression();
            if (!constExpr)
            {
                LogErrorContext(labeled, "case must have an expression");
                return;
            }

            std::string exprText = constExpr->getText();
            bool isStruct = compiler->dataStructures.count(exprText) > 0;
            bool isInterface = compiler->interfaceTable.count(exprText) > 0;

            // _ is a soft wildcard in arm-style switches - register as both default and caseMap entry
            // so ParseStatement can locate this labeled node and emit the arm body with auto-jump.
            if (hasArrow && exprText == "_")
            {
                ctx.defaultBlock = compiler->CreateBasicBlock("switchDefault");
                ctx.caseMap[labeled] = { nullptr, nullptr, ctx.defaultBlock, false, "", "", true };
                return;
            }

            if (isStruct || isInterface)
            {
                if (!ctx.isTypeSwitch && !ctx.caseMap.empty())
                    LogErrorContext(labeled, "cannot mix type cases with constant cases in a switch");
                ctx.isTypeSwitch = true;
                ctx.caseMap[labeled] = { nullptr, nullptr, compiler->CreateBasicBlock("switchTypeCase"), true, exprText, "", hasArrow };
                if (!hasArrow) CollectCasesFromStatement(labeled->statement(), ctx);
            }
            else
            {
                if (ctx.isTypeSwitch)
                    LogErrorContext(labeled, "cannot mix constant cases with type cases in a switch");

                llvm::Value* rawVal = ParseConditionalExpression(constExpr->conditionalExpression());
                auto* val = llvm::dyn_cast<llvm::ConstantInt>(rawVal);
                llvm::Constant* strLit = nullptr;
                if (!val)
                {
                    strLit = llvm::dyn_cast<llvm::Constant>(rawVal);
                    if (strLit && compiler->IsStringLiteralConstant(strLit))
                        ctx.isStringSwitch = true;
                    else
                        LogErrorContext(labeled, "case value must be a constant integer or string literal");
                }
                ctx.caseMap[labeled] = { val, strLit, compiler->CreateBasicBlock("switchCase"), false, "", "", hasArrow };
                if (!hasArrow) CollectCasesFromStatement(labeled->statement(), ctx);
            }
        }
        else if (labeled->Default())
        {
            ctx.defaultBlock = compiler->CreateBasicBlock("switchDefault");
            if (!hasArrow) CollectCasesFromStatement(labeled->statement(), ctx);
        }
    }

    void ParseStatement(CFlatParser::StatementContext* statement)
    {
        auto* compiler = Compiler(statement);
        compiler->SetCurrentDebugLocation(statement->getStart()->getLine());

        auto jump = statement->jumpStatement();
        auto expressStatement = statement->expressionStatement();
        auto iterationStatement = statement->iterationStatement();
        auto selectionStatement = statement->selectionStatement();
        auto compoundStatement = statement->compoundStatement();
        auto labeledStatement = statement->labeledStatement();
        auto expectErrorStmt = statement->expectErrorStatement();

        if (labeledStatement != nullptr && !switchStack.empty())
        {
            auto& ctx = switchStack.back();
            llvm::BasicBlock* targetBlock = nullptr;

            if (labeledStatement->Case())
            {
                auto it = ctx.caseMap.find(labeledStatement);
                if (it != ctx.caseMap.end())
                    targetBlock = it->second.block;
            }
            else if (labeledStatement->Default())
            {
                targetBlock = ctx.defaultBlock;
            }

            if (targetBlock)
            {
                compiler->CreateJump(targetBlock);    // fallthrough if no terminator yet
                compiler->SwitchToBlock(targetBlock);
            }

            // Arm-style: inject bound variable, execute body in its own scope, auto-jump to resume.
            // `default =>` and `case _ =>` arms are detected via FatArrow token.
            auto it = ctx.caseMap.find(labeledStatement);
            bool isDefaultArm = labeledStatement->FatArrow() != nullptr
                             && (labeledStatement->Default()
                                 || (labeledStatement->Case()
                                     && labeledStatement->constantExpression()
                                     && labeledStatement->constantExpression()->getText() == "_"));
            bool isArm = (it != ctx.caseMap.end() && it->second.isArmStyle) || isDefaultArm;
            bool hasBound = isArm && it != ctx.caseMap.end() && it->second.isTypeCase && !it->second.boundVarName.empty();

            if (isArm)
            {
                compiler->InitializeBlock(nullptr, true);

                if (hasBound && ctx.condValue)
                {
                    // Extract the data pointer (field 1) from the interface fat ptr and bind it.
                    auto* dataPtr = compiler->builder->CreateExtractValue(ctx.condValue, { 1u }, "typecase_data");
                    auto* alloca = compiler->AllocaAtEntry(dataPtr->getType(), nullptr, it->second.boundVarName);
                    compiler->builder->CreateStore(dataPtr, alloca);

                    LLVMBackend::NamedVariable nv;
                    nv.Storage  = alloca;
                    nv.Primary  = dataPtr;
                    nv.TypeAndValue.TypeName = it->second.typeCaseName;
                    nv.TypeAndValue.Pointer  = true;
                    nv.IsOwning    = false;
                    compiler->stackNamedVariable.back().namedVariable[it->second.boundVarName] = nv;
                }

                ParseStatement(labeledStatement->statement());
                compiler->CreateBlockBreak(ctx.resumeBlock, true);
                return;
            }

            ParseStatement(labeledStatement->statement());
            return;
        }

        if (jump != nullptr)
        {
            if (jump->Return())
            {
                if (jump->Default() != nullptr)
                {
                    // return default; - zero-initialize the return type (null for pointers/interfaces, 0 for integers)
                    auto* retTy = compiler->currentFunction->getReturnType();
                    auto* defaultVal = retTy->isVoidTy() ? nullptr : llvm::Constant::getNullValue(retTy);
                    compiler->CreateReturnCall(defaultVal);
                }
                else if (auto* blockBody = jump->compoundStatement())
                {
                    compiler->InitializeBlock(nullptr, true);
                    if (auto* blockItems = blockBody->blockItemList())
                        ParseBlockItemList(blockItems);
                    compiler->CreateBlockBreak(nullptr, true);
                }
                else
                {
                    auto express = jump->expression();
                    if (express != nullptr)
                    {
                        // Evaluate via NV path so we can inspect bond info alongside ownership.
                        auto assignExpr = express->assignmentExpression();
                        LLVMBackend::NamedVariable returnNV;
                        if (assignExpr != nullptr)
                            returnNV = ParseAssignmentExpressionNamed(assignExpr);
                        auto right = LoadNamedVariable(returnNV);
                        ProcessPlusPlus();

                        if (compiler->currentFunctionReturnsOwned && right != nullptr
                            && !llvm::isa<llvm::Constant>(right)
                            && right->getType()->isPointerTy())
                        {
                            bool returnIsOwned = compiler->IsOwningValue(right)
                                || compiler->lastCallReturnsOwned
                                || compiler->lastOwningResult;
                            if (!returnIsOwned)
                                LogErrorContext(jump, "function declares 'move' return type but returned expression is not owned - value must come from 'new', a move parameter, or another move-returning function");
                        }

                        // Bond return check: bonded value may only be returned if all its sources
                        // are 'bond' parameters of the current function (not locals).
                        auto checkBondSources = [&](const std::vector<std::string>& sources) {
                            for (const auto& source : sources)
                            {
                                auto funcArg = compiler->GetFunctionArgument(source);
                                if (funcArg.GetValue() == nullptr || !funcArg.TypeAndValue.IsBond)
                                    LogErrorContext(jump, std::format("returning bonded value whose source '{}' is not a 'bond' parameter - bonded values cannot escape their source's scope", source));
                            }
                        };
                        if (returnNV.IsBonded)
                            checkBondSources(returnNV.BondedSources);
                        if (compiler->lastCallIsBonded)
                            checkBondSources(compiler->lastCallBondedSources);

                        // Clear flags consumed by the return check.
                        compiler->lastOwningResult = false;
                        compiler->lastCallReturnsOwned = false;
                        compiler->lastCallIsBonded = false;
                        compiler->lastCallBondedSources.clear();
                        compiler->CreateReturnCall(right);
                    }
                    else
                    {
                        compiler->CreateReturnCall(nullptr);
                    }
                }
                return;
            }
            else if (jump->Continue())
            {
                compiler->CreateContinueCall();
                return;
            }
            else if (jump->Break())
            {
                compiler->CreateBreakCall();
                return;
            }
        }
        else if (expressStatement != nullptr)
        {
            auto express = expressStatement->expression();
            if (express != nullptr)
            {
                ParseExpression(express);
                return;
            }
        }
        else if (iterationStatement != nullptr)
        {
            /*
            iterationStatement
                : While '(' expression ')' statement
                | Do statement While '(' expression ')' ';'
                | For '(' forCondition ')' statement
                ;
            forCondition
                : (forDeclaration | expression?) ';' forExpression? ';' forExpression?
                ;
            */

            // Check Do before While: do-while contains the 'while' keyword, so While() returns
            // non-null for both rules. Do() is unambiguous and must be tested first.
            if (iterationStatement->Do())
            {
                auto expression = iterationStatement->expression();
                auto innerStatement = iterationStatement->statement();

                auto blockInner = compiler->CreateBasicBlock("doWhileInner");
                auto blockCondition = compiler->CreateBasicBlock("doWhileCondition");
                auto blockResume = compiler->CreateBasicBlock("doWhileResume");

                compiler->CreateBlockBreak(blockInner, false);

                compiler->InitializeBlock(blockInner, true, blockCondition, blockResume, blockResume);
                ParseStatement(innerStatement);
                compiler->CreateContinueCall();

                compiler->InitializeBlock(blockCondition, false);
                auto condition = ParseExpression(expression);
                compiler->CreateConditionJump(condition, blockInner, blockResume);

                // resume
                compiler->InitializeBlock(blockResume, false);

                // pop the stack
                compiler->CreateBlockBreak(nullptr, true);
                return;
            }
            else if (iterationStatement->While())
            {
                auto expression = iterationStatement->expression();
                auto innerStatement = iterationStatement->statement();

                auto blockCondition = compiler->CreateBasicBlock("whileCondition");
                auto blockInner = compiler->CreateBasicBlock("whileInner");
                auto blockResume = compiler->CreateBasicBlock("whileResume");

                compiler->CreateBlockBreak(blockCondition, false);

                compiler->InitializeBlock(blockCondition, true, blockCondition, blockResume, blockResume);
                auto condition = ParseExpression(expression);
                compiler->CreateConditionJump(condition, blockInner, blockResume);

                compiler->InitializeBlock(blockInner, false);
                ParseStatement(innerStatement);
                compiler->CreateContinueCall();

                // resume
                compiler->InitializeBlock(blockResume, false);

                // pop the stack
                compiler->CreateBlockBreak(nullptr, true);

                return;
            }
            else if (iterationStatement->For())
            {
                // Classic for-loop: for (init; cond; inc) statement
                if (iterationStatement->forCondition())
                {
                    auto forCondition = iterationStatement->forCondition();
                    auto declaration = forCondition->forDeclaration();
                    auto expressionCtx = forCondition->expression();
                    auto forIncrementCtx = forCondition->forExpression();
                    auto compareCtx = forCondition->assignmentExpression();
                    auto innerStatement = iterationStatement->statement();

                    auto blockInit = compiler->CreateBasicBlock("forInit");
                    auto blockCondition = compiler->CreateBasicBlock("forCondition");
                    auto blockInner = compiler->CreateBasicBlock("forInner");
                    auto blockIncrement = compiler->CreateBasicBlock("forIncrement");
                    auto blockResume = compiler->CreateBasicBlock("forResume");

                    compiler->CreateBlockBreak(blockInit, false);

                    // Init => Condition => Inner => Increment => Condition

                    // initialization
                    compiler->InitializeBlock(blockInit, true, blockIncrement, blockResume, blockResume);
                    if (declaration)
                        ParseForDeclaration(declaration);
                    if (expressionCtx)
                        ParseExpression(expressionCtx);

                    compiler->CreateBlockBreak(blockCondition, false);

                    // Condition
                    compiler->InitializeBlock(blockCondition, false);
                    auto condition = ParseAssignmentExpression(compareCtx);
                    compiler->CreateConditionJump(condition, blockInner, blockResume);

                    // Inner statement
                    compiler->InitializeBlock(blockInner, false);
                    ParseStatement(innerStatement);
                    compiler->CreateContinueCall();

                    // Increment
                    compiler->InitializeBlock(blockIncrement, false);

                    auto assignments = forIncrementCtx->assignmentExpression();
                    for (auto assign : assignments)
                    {
                        ParseAssignmentExpression(assign);
                        ProcessPlusPlus();
                    }

                    compiler->CreateBlockBreak(blockCondition, false);

                    // resume
                    compiler->InitializeBlock(blockResume, false);

                    // pop the stack
                    compiler->CreateBlockBreak(nullptr, true);

                    return;
                }
                // Range-based for: for (T x in collection) statement
                else if (iterationStatement->declarationSpecifiers() && iterationStatement->In())
                {
                    auto declSpecCtx = iterationStatement->declarationSpecifiers();
                    auto varNameTok  = iterationStatement->Identifier();
                    auto collExprCtx = iterationStatement->expression();
                    auto bodyStmt    = iterationStatement->statement();

                    std::string varName = varNameTok->getText();

                    auto blockInit      = compiler->CreateBasicBlock("forRangeInit");
                    auto blockCond      = compiler->CreateBasicBlock("forRangeCond");
                    auto blockInner     = compiler->CreateBasicBlock("forRangeInner");
                    auto blockIncrement = compiler->CreateBasicBlock("forRangeIncrement");
                    auto blockResume    = compiler->CreateBasicBlock("forRangeResume");

                    compiler->CreateBlockBreak(blockInit, false);

                    // Push scope; continue -> increment, break/else -> resume
                    compiler->InitializeBlock(blockInit, true, blockIncrement, blockResume, blockResume);

                    // Evaluate the collection expression (needs typed NamedVariable for dispatch)
                    auto collNV = ParseAssignmentExpressionNamed(collExprCtx->assignmentExpression());

                    // Spill into alloca if the collection was returned by value (no storage)
                    if (collNV.Storage == nullptr && collNV.Primary != nullptr)
                    {
                        llvm::Type* ty = collNV.BaseType;
                        if (!ty) ty = compiler->GetType(collNV.TypeAndValue);
                        auto spill = compiler->CreateAlloca(ty);
                        compiler->CreateAssignment(collNV.Primary, spill);
                        collNV.Storage = spill;
                        collNV.Primary = nullptr;
                    }

                    bool isFaceType   = compiler->IsInterfaceType(collNV.TypeAndValue.TypeName);
                    bool isFixedArray = collNV.BaseType && llvm::isa<llvm::ArrayType>(collNV.BaseType);

                    // Call count() once and cache it
                    llvm::Value* countVal = nullptr;
                    llvm::Value* ifacePtr = nullptr;
                    if (isFixedArray)
                    {
                        auto* arrTy = llvm::cast<llvm::ArrayType>(collNV.BaseType);
                        countVal = compiler->builder->getInt32((uint32_t)arrTy->getNumElements());
                    }
                    else if (isFaceType)
                    {
                        ifacePtr = collNV.Storage;
                        if (!ifacePtr)
                        {
                            auto fatTy = compiler->GetFatPtrType();
                            ifacePtr = compiler->CreateAlloca(fatTy);
                            compiler->CreateAssignment(collNV.Primary, ifacePtr);
                        }
                        countVal = compiler->CallInterfaceMethod(ifacePtr, collNV.TypeAndValue.TypeName, "count", {});
                    }
                    else
                    {
                        LLVMBackend::NamedVariable selfArg = collNV;
                        selfArg.TypeAndValue.VariableName = "";
                        countVal = compiler->CreateOverloadedFunctionCall("count", { selfArg });
                    }

                    auto* i32Ty = compiler->builder->getInt32Ty();

                    auto countAlloca = compiler->CreateAlloca(i32Ty);
                    compiler->builder->CreateStore(countVal, countAlloca);

                    auto indexAlloca = compiler->CreateAlloca(i32Ty);
                    compiler->builder->CreateStore(compiler->builder->getInt32(0), indexAlloca);

                    // Pre-allocate the element variable in the init block (one alloca for all iterations)
                    auto elemType = ParseDeclarationSpecifiers(declSpecCtx);
                    elemType.VariableName = varName;
                    auto elemAlloca = compiler->CreateLocalVariable(elemType);

                    compiler->CreateBlockBreak(blockCond, false);

                    // Condition: i < count
                    compiler->InitializeBlock(blockCond, false);
                    auto iVal   = compiler->CreateLoad(indexAlloca);
                    auto cntVal = compiler->CreateLoad(countAlloca);
                    auto cond   = compiler->builder->CreateICmpSLT(iVal, cntVal);
                    compiler->CreateConditionJump(cond, blockInner, blockResume);

                    // Inner block: load element, run body
                    compiler->InitializeBlock(blockInner, false);

                    LLVMBackend::NamedVariable indexNV;
                    indexNV.Primary  = compiler->CreateLoad(indexAlloca);
                    indexNV.BaseType = i32Ty;
                    indexNV.TypeAndValue.TypeName = "int";

                    llvm::Value* elemVal = nullptr;
                    if (isFixedArray)
                    {
                        auto* arrTy = llvm::cast<llvm::ArrayType>(collNV.BaseType);
                        llvm::Value* zero   = compiler->builder->getInt64(0);
                        llvm::Value* idxI64 = compiler->builder->CreateZExt(
                            compiler->CreateLoad(indexAlloca), compiler->builder->getInt64Ty(), "idxi64");
                        auto* elemPtr = compiler->builder->CreateGEP(
                            arrTy, collNV.Storage, {zero, idxI64}, "arrayelemptr");
                        elemVal = compiler->builder->CreateLoad(arrTy->getElementType(), elemPtr, "arrayelem");
                    }
                    else if (isFaceType)
                    {
                        elemVal = compiler->CallInterfaceMethod(ifacePtr, collNV.TypeAndValue.TypeName, "get", { indexNV });
                    }
                    else
                    {
                        LLVMBackend::NamedVariable selfArg = collNV;
                        selfArg.TypeAndValue.VariableName = "";
                        elemVal = compiler->CreateOverloadedFunctionCall("get", { selfArg, indexNV });
                    }

                    if (elemVal)
                        compiler->CreateAssignment(elemVal, elemAlloca);

                    ParseStatement(bodyStmt);
                    compiler->CreateContinueCall();

                    // Increment block: i++
                    compiler->InitializeBlock(blockIncrement, false);
                    compiler->CreateIncrement(indexAlloca, 1);
                    compiler->CreateBlockBreak(blockCond, false);

                    // Resume
                    compiler->InitializeBlock(blockResume, false);
                    compiler->CreateBlockBreak(nullptr, true);

                    return;
                }
            }
        }
        else if (selectionStatement)
        {
            /*
            selectionStatement
                : 'if' '(' expression ')' statement ('else' statement)?
                | 'if' 'const' '(' expression ')' statement ('else' statement)?
                | 'switch' '(' expression ')' statement
                ;
            */

            if (selectionStatement->If() && selectionStatement->Const())
            {
                // if const (...) - compile-time conditional
                auto expression = selectionStatement->expression();
                auto innerStatement = selectionStatement->statement();

                auto condition = ParseExpression(expression);
                auto constInt = llvm::dyn_cast<llvm::ConstantInt>(condition);
                if (!constInt)
                {
                    LogErrorContext(selectionStatement, "'if const' condition must be a compile-time constant expression");
                    return;
                }

                bool taken = constInt->getZExtValue() != 0;
                if (taken)
                    ParseStatement(innerStatement[0]);
                else if (innerStatement.size() > 1)
                    ParseStatement(innerStatement[1]);
                return;
            }
            else if (selectionStatement->If())
            {
                auto expression = selectionStatement->expression();
                auto innerStatement = selectionStatement->statement();

                // Parse condition value before CreateBlock
                auto blockCondition = compiler->CreateBasicBlock("ifCondition");
                auto blockTrue = compiler->CreateBasicBlock("ifTrue");
                auto blockResume = compiler->CreateBasicBlock("ifResume");
                llvm::BasicBlock* blockElse = selectionStatement->Else() == nullptr ? nullptr : compiler->CreateBasicBlock("ifFalse");
                auto blockFalse = blockElse ? blockElse : blockResume;

                compiler->CreateBlockBreak(blockCondition, false);

                compiler->InitializeBlock(blockCondition, true, nullptr, nullptr, blockFalse);
                auto condition = ParseExpression(expression);
                compiler->CreateConditionJump(condition, blockTrue, blockFalse);

                auto preIfMovedState = compiler->SaveMovedState();

                compiler->InitializeBlock(blockTrue, false);
                ParseStatement(innerStatement[0]);
                auto thenMovedState = compiler->SaveMovedState();

                compiler->CreateBlockBreak(blockResume, true);

                if (blockElse != nullptr)
                {
                    // Restore pre-branch moved state so else doesn't inherit if-branch moves.
                    compiler->RestoreMovedState(preIfMovedState);

                    // else statement
                    compiler->InitializeBlock(blockElse, true);
                    ParseStatement(innerStatement[1]);
                    auto elseMovedState = compiler->SaveMovedState();
                    compiler->CreateBlockBreak(blockResume, true);

                    // Merge: variable is moved if moved in either branch.
                    compiler->MergeMovedStates(thenMovedState, elseMovedState);
                }

                // resume
                compiler->InitializeBlock(blockResume, false);
                return;
            }
            else if (selectionStatement->Switch())
            {
                auto expression = selectionStatement->expression();
                auto body = selectionStatement->statement(0)->compoundStatement();

                SwitchContext switchCtx;
                switchCtx.resumeBlock = compiler->CreateBasicBlock("switchResume");

                // Pre-scan: collect all case/default labels and create their blocks
                if (body && body->blockItemList())
                {
                    for (auto blockItem : body->blockItemList()->blockItem())
                    {
                        auto stmt = blockItem->statement();
                        if (stmt) CollectCasesFromStatement(stmt, switchCtx);
                    }
                }

                auto switchDefault = switchCtx.defaultBlock ? switchCtx.defaultBlock : switchCtx.resumeBlock;

                auto condVal = ParseExpression(expression);
                switchCtx.condValue = condVal;

                if (switchCtx.isTypeSwitch)
                {
                    // Type switch: dispatch on the concrete type behind an interface fat pointer
                    auto fatTy = compiler->GetFatPtrType();
                    auto ptrTy = compiler->builder->getInt8Ty()->getPointerTo();

                    // Validate: switch expression must be an interface-typed value (fat pointer)
                    if (condVal->getType() != fatTy && condVal->getType() != fatTy->getPointerTo())
                        LogErrorContext(expression, "type switch expression must be interface-typed (fat pointer)");

                    // Extract dataPtr (field 1 of fat pointer)
                    llvm::Value* dataPtr;
                    if (condVal->getType()->isStructTy())
                    {
                        dataPtr = compiler->builder->CreateExtractValue(condVal, {1u});
                    }
                    else
                    {
                        auto dpField = compiler->builder->CreateStructGEP(fatTy, condVal, 1);
                        dataPtr = compiler->builder->CreateLoad(ptrTy, dpField);
                    }

                    // Load type descriptor from vtable[0]
                    llvm::Value* loadedDesc = LoadTypeDescFromInterface(condVal, expression);

                    // Emit linear dispatch chain: for each type case, check if it matches
                    for (auto& [labeledCtx, entry] : switchCtx.caseMap)
                    {
                        if (!entry.isTypeCase) continue;  // skip non-type cases (shouldn't happen)

                        auto* nextCheck = compiler->CreateBasicBlock("typeswitch_next");

                        if (compiler->dataStructures.count(entry.typeCaseName))
                        {
                            // Concrete struct case: single type descriptor comparison
                            auto& sd = compiler->dataStructures[entry.typeCaseName];
                            if (!sd.typeDescriptor)
                            {
                                LogErrorContext(expression, std::format("struct '{}' has no type descriptor", entry.typeCaseName));
                                continue;
                            }
                            auto* cmp = compiler->builder->CreateICmpEQ(loadedDesc, sd.typeDescriptor);
                            compiler->builder->CreateCondBr(cmp, entry.block, nextCheck);
                        }
                        else if (compiler->programTable.count(entry.typeCaseName))
                        {
                            // Concrete program case: single type descriptor comparison
                            auto& pd = compiler->programTable[entry.typeCaseName];
                            if (!pd.typeDescriptor)
                            {
                                LogErrorContext(expression, std::format("program '{}' has no type descriptor", entry.typeCaseName));
                                continue;
                            }
                            auto* cmp = compiler->builder->CreateICmpEQ(loadedDesc, pd.typeDescriptor);
                            compiler->builder->CreateCondBr(cmp, entry.block, nextCheck);
                        }
                        else if (compiler->interfaceTable.count(entry.typeCaseName))
                        {
                            // Interface case: match if the concrete type implements this interface
                            // Emit: if (typedesc == any_implementing_struct_typedesc) goto case block
                            llvm::BasicBlock* anyMatchedBlock = entry.block;
                            auto* nextStruct = nextCheck;

                            // Enumerate all classes/structs that implement this interface
                            for (auto& [sName, sd] : compiler->dataStructures)
                            {
                                if (!compiler->StructImplementsInterface(sName, entry.typeCaseName)) continue;
                                if (!sd.typeDescriptor) continue;

                                auto* matchBlock = compiler->CreateBasicBlock("typeswitch_match");
                                auto* cmpNextStruct = compiler->CreateBasicBlock("typeswitch_cmp_next");

                                auto* cmp = compiler->builder->CreateICmpEQ(loadedDesc, sd.typeDescriptor);
                                compiler->builder->CreateCondBr(cmp, matchBlock, cmpNextStruct);

                                compiler->SwitchToBlock(matchBlock);
                                compiler->builder->CreateBr(anyMatchedBlock);

                                compiler->SwitchToBlock(cmpNextStruct);
                                nextStruct = cmpNextStruct;
                            }
                            // Also enumerate programs that implement this interface
                            for (auto& [pName, pd] : compiler->programTable)
                            {
                                if (!compiler->StructImplementsInterface(pName, entry.typeCaseName)) continue;
                                if (!pd.typeDescriptor) continue;

                                auto* matchBlock = compiler->CreateBasicBlock("typeswitch_match");
                                auto* cmpNextStruct = compiler->CreateBasicBlock("typeswitch_cmp_next");

                                auto* cmp = compiler->builder->CreateICmpEQ(loadedDesc, pd.typeDescriptor);
                                compiler->builder->CreateCondBr(cmp, matchBlock, cmpNextStruct);

                                compiler->SwitchToBlock(matchBlock);
                                compiler->builder->CreateBr(anyMatchedBlock);

                                compiler->SwitchToBlock(cmpNextStruct);
                                nextStruct = cmpNextStruct;
                            }
                            // Fall through nextStruct to nextCheck
                            compiler->builder->CreateBr(nextCheck);
                        }
                        else
                        {
                            LogErrorContext(expression, std::format("'{}' is not a known struct or interface type", entry.typeCaseName));
                        }

                        compiler->SwitchToBlock(nextCheck);
                    }
                    // Fall through to default case
                    compiler->CreateJump(switchDefault);
                }
                else if (switchCtx.isStringSwitch)
                {
                    // String switch: emit if-else chain using strcmp on _ptr (field 0)
                    auto* strPtr = compiler->builder->CreateExtractValue(condVal, { 0u });
                    auto* strcmpFn = compiler->GetOrDeclareStrcmp();
                    auto* i32Zero = compiler->builder->getInt32(0);

                    for (auto& [labeledCtx, entry] : switchCtx.caseMap)
                    {
                        if (!entry.strLiteral) continue;
                        auto* nextBlock = compiler->CreateBasicBlock("switchCmp");
                        auto* cmpResult = compiler->builder->CreateCall(strcmpFn, { strPtr, entry.strLiteral });
                        auto* isEqual = compiler->builder->CreateICmpEQ(cmpResult, i32Zero);
                        compiler->builder->CreateCondBr(isEqual, entry.block, nextBlock);
                        compiler->SwitchToBlock(nextBlock);
                    }
                    compiler->CreateJump(switchDefault);
                }
                else
                {
                    auto switchInst = compiler->CreateSwitchInst(condVal, switchDefault, (unsigned)switchCtx.caseMap.size());
                    for (auto& [labeledCtx, entry] : switchCtx.caseMap)
                        if (entry.value)  // null value = wildcard (_) arm, handled via defaultBlock
                            switchInst->addCase(compiler->CoerceCaseValue(entry.value, condVal->getType()), entry.block);
                }

                // Push scope: break -> resumeBlock, no continue (propagates to outer loop)
                compiler->InitializeBlock(nullptr, true, nullptr, switchCtx.resumeBlock, nullptr);

                switchStack.push_back(switchCtx);

                if (body && body->blockItemList())
                    ParseBlockItemList(body->blockItemList());

                // Fallthrough at end of switch body -> resume
                compiler->CreateBlockBreak(switchCtx.resumeBlock, true);

                switchStack.pop_back();

                compiler->InitializeBlock(switchCtx.resumeBlock, false);
                return;
            }
        }
        else if (compoundStatement)
        {
            compiler->InitializeBlock(nullptr, true);
            auto blockList = compoundStatement->blockItemList();
            if (blockList)
                ParseBlockItemList(blockList);
            compiler->CreateBlockBreak(nullptr, true);
            return;
        }
        else if (expectErrorStmt)
        {
            std::string rawText = expectErrorStmt->StringLiteral()->getText();
            compilerLLVM->expectedError = ProcessRawText(rawText);

            if (auto* cs = expectErrorStmt->compoundStatement())
            {
                // Scoped block form: expect_error("msg") { ... } - error must occur inside the braces.
                compilerLLVM->expectedErrorScopeDepth = SIZE_MAX;  // manual check after block
                size_t savedDepth = compilerLLVM->stackNamedVariable.size();
                compiler->InitializeBlock(nullptr, true);
                bool errorReceived = false;
                try
                {
                    if (auto* blockList = cs->blockItemList())
                        ParseBlockItemList(blockList);
                }
                catch (const ExpectedErrorReceived&)
                {
                    errorReceived = true;
                    // Pop any extra nested frames without destructors (error path).
                    while (compilerLLVM->stackNamedVariable.size() > savedDepth)
                        compilerLLVM->stackNamedVariable.pop_back();
                    // Terminate the current block and create a new one so the outer
                    // function's subsequent statements have a valid insertion point.
                    if (auto* bb = compilerLLVM->builder->GetInsertBlock())
                    {
                        if (!compiler->IsBlockTerminated())
                            compilerLLVM->builder->CreateUnreachable();
                        auto* resume = llvm::BasicBlock::Create(
                            *compilerLLVM->context, "after_expect_error", bb->getParent());
                        compilerLLVM->builder->SetInsertPoint(resume);
                    }
                    compilerLLVM->expectedError.clear();
                    compilerLLVM->expectedErrorScopeDepth = SIZE_MAX;
                }

                if (!errorReceived)
                {
                    compiler->CreateBlockBreak(nullptr, true);
                    if (!compilerLLVM->expectedError.empty())
                    {
                        std::cout << std::format("FAIL: expected error '{}' did not occur\n",
                                                  compilerLLVM->expectedError);
                        compilerLLVM->expectedError.clear();
                        if (compilerLLVM->diagnosticSink_)
                            throw CompilerAbortException{ "expected error did not occur", compilerLLVM->sourceFileName, 0, 0 };
                        else
                            compilerLLVM->FailCompilation("expected error did not occur");
                    }
                }
            }
            else
            {
                // Bare-semicolon form: expect_error("msg"); - error must occur before the enclosing scope exits.
                compilerLLVM->expectedErrorScopeDepth = compilerLLVM->stackNamedVariable.size();
            }
            return;
        }

        else if (auto* lockStmt = statement->lockStatement())
        {
            // lock (expr1, expr2, ...) { body }
            // Acquire all mutexes in order, parse body, release in reverse order on scope exit.
            auto* lockClauseCtx = lockStmt->lockClause();
            auto lockArgs = lockClauseCtx->lockArgList()->expression();

            struct AcquiredLock
            {
                std::string canonical;
                std::string acquireMethod;
                std::string releaseMethod;
                std::string typeName;
                llvm::Value* storage = nullptr;
            };
            std::vector<AcquiredLock> acquired;

            for (auto* exprCtx : lockArgs)
            {
                // Determine mode: strip trailing .read/.write soft keyword.
                std::string mode = GetLockArgMode(exprCtx);
                std::string canonical = GetLockArgCanonical(exprCtx);
                std::string acquireMethod = (mode == "read") ? "acquire_read" : "acquire";
                std::string releaseMethod = (mode == "read") ? "release_read" : "release";

                // Evaluate the base expression (without .read/.write suffix).
                // For .read/.write, we need the base expression's assignmentExpression context.
                // Since getText() strips whitespace, we reconstruct the base by truncating.
                // The simplest approach: evaluate the full expression but intercept if mode set.
                LLVMBackend::NamedVariable mutexNV;
                if (mode.empty())
                {
                    mutexNV = ParseAssignmentExpressionNamed(exprCtx->assignmentExpression());
                }
                else
                {
                    // For rw.read / rw.write: evaluate only the base (the postfix up to the mode suffix).
                    // The expression is `base.read` or `base.write`. We need to evaluate `base`.
                    // In ANTLR, exprCtx->assignmentExpression()->conditionalExpression()->...
                    // is the full expression. We strip the trailing member access by evaluating
                    // one level up: find the receiver of the final member access.
                    // Simple approach: evaluate the whole expression and use the struct var (receiver).
                    // The receiver has .Storage pointing to the rwlock; accessing .read/.write would fail
                    // at IR gen, so we intercept the postfix expression chain manually.
                    //
                    // For MVP: just use the full expression text as canonical and call acquire() directly.
                    // The mode is tracked in currentLockSet but acquire() always does exclusive for now.
                    mutexNV = ParseAssignmentExpressionNamed(exprCtx->assignmentExpression());
                    // If the last field access was .read or .write (non-existent field on rwlock),
                    // mutexNV.Storage might be null. Fall back to calling acquire() on the base.
                    // TODO: properly evaluate only the base sub-expression for rwlock.read/write.
                }

                // Spill into alloca if returned by value (no storage pointer).
                if (mutexNV.Storage == nullptr && mutexNV.Primary != nullptr)
                {
                    llvm::Type* ty = mutexNV.BaseType ? mutexNV.BaseType : compiler->GetType(mutexNV.TypeAndValue);
                    auto* spill = compiler->CreateAlloca(ty);
                    compiler->CreateAssignment(mutexNV.Primary, spill);
                    mutexNV.Storage = spill;
                    mutexNV.Primary = nullptr;
                }

                if (!mutexNV.Storage)
                {
                    LogErrorContext(lockStmt, std::format("lock: expression '{}' must be a mutex variable.", canonical));
                    return;
                }

                // Call acquire / acquire_read.
                LLVMBackend::NamedVariable selfArg = mutexNV;
                selfArg.TypeAndValue.VariableName = "";
                compiler->CreateOverloadedFunctionCall(acquireMethod, { selfArg });

                std::string mutexTypeName = mutexNV.TypeAndValue.TypeName;
                llvm::Function* unlockFn = FindMethodOf(releaseMethod, mutexTypeName);
                if (!unlockFn)
                {
                    // Fall back to plain release (e.g., mutex has no release_read).
                    unlockFn = FindMethodOf("release", mutexTypeName);
                }
                if (!unlockFn)
                {
                    LogErrorContext(lockStmt, std::format("lock: type '{}' has no 'release' method.", mutexTypeName));
                    return;
                }

                acquired.push_back({ canonical, acquireMethod, releaseMethod, mutexTypeName, mutexNV.Storage });
            }

            // Push lock scope - only supports single-mutex cleanup via StackState today.
            // For multi-lock, push one scope per mutex so each gets its own cleanup slot.
            compiler->InitializeBlock(nullptr, true);
            if (!acquired.empty())
            {
                compiler->stackNamedVariable.back().lockCleanup = LLVMBackend::StackState::LockCleanup{
                    .UnlockFn = FindMethodOf(acquired[0].releaseMethod, acquired[0].typeName),
                    .MutexPtr = acquired[0].storage,
                };
            }

            // Update lock-set for static analysis.
            for (const auto& lk : acquired)
                currentLockSet.insert(lk.canonical);

            // Parse the body.
            auto* blockList = lockStmt->compoundStatement()->blockItemList();
            if (blockList)
                ParseBlockItemList(blockList);

            // Remove from lock-set.
            for (const auto& lk : acquired)
                currentLockSet.erase(lk.canonical);

            // Close the scope - EmitDestructorsForScope will call unlock().
            compiler->CreateBlockBreak(nullptr, true);
            return;
        }

        LogErrorContext(statement, "Unhandled statement type.");
        return;
    }

    void GenerateDefaultParamOverloads(
        const std::string& name,
        const LLVMBackend::DeclTypeAndValue& returnType,
        const std::vector<LLVMBackend::DeclTypeAndValue>& params,
        bool varargs,
        size_t line)
    {
        auto* compiler = Compiler();
        int firstDefault = -1;
        for (int i = 0; i < (int)params.size(); i++)
        {
            if (params[i].DefaultValue != nullptr)
            {
                firstDefault = i;
                break;
            }
        }

        if (firstDefault < 0)
            return;

        // For each number of omitted trailing defaults, generate a wrapper that
        // fills them in and forwards to the full function.
        // e.g. f(int a, int b = 10, int c = 20):
        //   wrapper(int a, int b) -> f(a, b, 20)
        //   wrapper(int a)        -> f(a, 10, 20)
        for (int cutoff = firstDefault; cutoff < (int)params.size(); cutoff++)
        {
            std::vector<LLVMBackend::TypeAndValue> wrapperParams(params.begin(), params.begin() + cutoff);

            auto wrapperFn = compiler->CreateFunctionDefinition(name, returnType, wrapperParams, false, false, line);
            compiler->InitializeBlock(&wrapperFn->front(), false);

            // Build the full argument list for the forwarding call
            std::vector<LLVMBackend::NamedVariable> callArgs;

            for (int i = 0; i < cutoff; i++)
            {
                callArgs.push_back(compiler->GetFunctionArgument(params[i].VariableName));
            }

            for (int i = cutoff; i < (int)params.size(); i++)
            {
                auto* initCtx = params[i].DefaultValue;
                llvm::Value* defaultVal = nullptr;
                if (auto* ae = initCtx->assignmentExpression())
                {
                    defaultVal = ParseAssignmentExpression(ae);
                }
                else if (initCtx->Default())
                {
                    defaultVal = GenerateDefaultValue(params[i]);
                }
                else if (auto* initList = initCtx->initializerList())
                {
                    // Field initializer default: build the struct, apply overrides, pass by value.
                    defaultVal = GenerateDefaultValue(params[i]);
                    if (defaultVal)
                    {
                        auto* alloca = compiler->CreateAlloca(defaultVal->getType());
                        compiler->CreateAssignment(defaultVal, alloca);
                        EmitFieldInitializer(alloca, params[i].TypeName, initList);
                        defaultVal = compiler->CreateLoad(alloca);
                    }
                }
                LLVMBackend::NamedVariable namedVar;
                namedVar.Primary = defaultVal;
                namedVar.BaseType = defaultVal ? defaultVal->getType() : nullptr;
                namedVar.TypeAndValue.TypeName = params[i].TypeName;
                namedVar.TypeAndValue.Pointer = params[i].Pointer;
                callArgs.push_back(namedVar);
            }

            if (returnType.TypeName == "void")
            {
                compiler->CreateOverloadedFunctionCall(name, callArgs);
                compiler->CreateReturnCall(nullptr);
            }
            else
            {
                auto result = compiler->CreateOverloadedFunctionCall(name, callArgs);
                compiler->CreateReturnCall(result);
            }

            compiler->CreateBlockBreak(nullptr, true);
            compiler->ClearCurrentSubprogram();
        }
    }

    void ParseFunctionDefinition(CFlatParser::FunctionDefinitionContext* func, std::string structName = {}, std::string namespaceName = {}, const std::string& nameOverride = {})
    {
        auto* compiler = Compiler(func);
        // Create Function Definition
        auto name = nameOverride.empty() ? ::getFunctionName(func) : nameOverride;
        if (!namespaceName.empty())
            name = namespaceName + "." + name;
        auto returnType = this->getFunctionReturnType(func);
        CFlatParser::ParameterTypeListContext* paramTypeList = func->parameterTypeList();
        auto params = this->ParseParameterTypeList(paramTypeList);
        size_t line = func->getStart()->getLine();
        bool varargs = paramTypeList && paramTypeList->Ellipsis() != nullptr;

        // If this is a generic function template definition (not an instantiation), store it and return.
        if (nameOverride.empty() && func->genericTypeParameters() != nullptr)
        {
            auto typeParams = ParseGenericTypeParameters(func->genericTypeParameters());
            genericFunctionTemplates[name] = func;
            genericFunctionTypeParams[name] = typeParams;
            genericFunctionConstraints[name] = ParseWhereClause(func->whereClause());
            return;
        }

        if (!structName.empty())
        {
            LLVMBackend::DeclTypeAndValue typeValue;
            typeValue.TypeName = structName;
            typeValue.VariableName = structName + "__";
            typeValue.Pointer = true;
            params.insert(params.begin(), typeValue);
        }

        // Return-block function: store the inner block for inlining at call sites.
        if (IsReturnBlockFunction(func))
        {
            auto* blockBody = func->compoundStatement()->blockItemList()->blockItem()[0]
                ->statement()->jumpStatement()->compoundStatement();
            compiler->RegisterReturnBlock(name, blockBody, params, returnType);
            return;
        }

        std::vector<LLVMBackend::TypeAndValue> allParams(params.begin(), params.end());

        // 'auto' return type: only supported on generic instantiations, where param
        // types are concrete and the body can be emitted under an auto-capture pass
        // that records each 'return expr;' value type instead of emitting 'ret'.
        // After body emission the function is rebuilt with the unified return type.
        bool isAutoReturn = (returnType.TypeName == "auto");
        if (isAutoReturn)
        {
            if (nameOverride.empty())
            {
                LogErrorContext(func, "'auto' return type is only supported on generic functions (e.g. auto f<T>(T x))");
                return;
            }
            // Substitute a placeholder return type so CreateFunctionDefinition can build
            // an LLVM function. The placeholder is replaced after body emission.
            returnType.TypeName = "i64";
            returnType.Pointer  = false;
            returnType.IsMove   = false;
            returnType.IsNullable = false;
        }

        bool returnsOwned = false;
        if (returnType.TypeName == "string" && returnType.IsMove)
            returnsOwned = true;
        else if (returnType.IsMove && returnType.Pointer)
            returnsOwned = true;

        // Pre-scan parameter types, return type, and function body to queue and emit any
        // generic struct instantiations before the function's IR block is opened.
        // At this point no basic block is active, so it is safe to emit new functions.
        // Parameter types must be scanned so that e.g. list<string>* params instantiate
        // list__string before the body references its methods.
        if (auto* paramTypeList = func->parameterTypeList())
            ScanAndQueueGenericTypeUses(paramTypeList);
        if (auto* blockItemList = func->compoundStatement()->blockItemList())
            ScanAndQueueGenericTypeUses(blockItemList);
        ProcessPendingInstantiations();

        size_t bodyLine = 0;
        if (auto* body = func->compoundStatement())
            bodyLine = body->getStart()->getLine();
        auto fn = compiler->CreateFunctionDefinition(name, returnType, allParams, returnType.external, varargs, line, returnsOwned, !structName.empty(), returnType.IsStdcall, returnType.IsCdecl, bodyLine);

        // CreateFunctionDefinition returns the existing function (without setting up
        // a fresh entry block) when a matching definition was already emitted by a
        // transitive import. Detect that here and skip body emission - re-emitting
        // into the live function's blocks would corrupt its IR.
        if (!fn->empty() && fn->getEntryBlock().getTerminator() != nullptr)
            return;

        compiler->InitializeBlock(&fn->front(), false);

        currentFunctionIsVariadic = varargs;

        if (isAutoReturn)
        {
            // The placeholder mangled name encodes the placeholder return type, which
            // may happen to collide with the eventual post-inference mangled name
            // (e.g. if T=i64, both placeholder and inferred return are i64). Rename
            // the LLVM function to a guaranteed-unique pending name so finalization
            // can freely create the real function under the inferred mangled name.
            fn->setName(fn->getName().str() + "$auto_pending");
            compiler->BeginAutoReturnCapture();
        }

        // Seed the lock-set from the function's RequiredLocks (covers both lock clauses and
        // positional group membership). For this.X entries, also insert the bare form X so
        // self-access checks (which use the guardian bare name) pass inside the method body.
        currentLockSet.clear();
        if (const auto* sym = compiler->GetFunctionSymbol(fn))
        {
            for (const auto& rawLock : sym->RequiredLocks)
            {
                std::string canonical = StripLockModeSuffix(rawLock);
                currentLockSet.insert(canonical);
                if (canonical.size() > 5 && canonical.substr(0, 5) == "this.")
                    currentLockSet.insert(canonical.substr(5));
            }
        }

        // Record stack depth after createFunctionBlock pushed the function's frame.
        // Used to identify bare-semicolon expect_error that was set inside this function.
        size_t funcDepth = compilerLLVM->stackNamedVariable.size();

        auto blockItemList = func->compoundStatement()->blockItemList();

        bool expectErrorHandled = false;
        if (blockItemList)
        {
            try
            {
                ParseBlockItemList(blockItemList);
            }
            catch (const ExpectedErrorReceived&)
            {
                // Handle only if the expect_error was set at this function's entry depth
                // (bare-semicolon form inside this function body).
                // File-scope scoped-block form sets expectedErrorScopeDepth = SIZE_MAX - re-throw.
                if (!compilerLLVM->expectedError.empty() &&
                    compilerLLVM->expectedErrorScopeDepth == funcDepth)
                {
                    compilerLLVM->AbortFunctionBlocks(funcDepth - 1);
                    compilerLLVM->expectedError.clear();
                    compilerLLVM->expectedErrorScopeDepth = SIZE_MAX;
                    compiler->ClearCurrentSubprogram();
                    expectErrorHandled = true;
                }
                else
                {
                    throw;
                }
            }
        }

        if (!expectErrorHandled)
        {
            if (returnType.TypeName != "void" && !compiler->IsBlockTerminated())
                LogErrorContext(func, std::format("Function '{}' with non-void return type is missing a return statement.", name));

            // if return is void, then this might need a implicit return;
            if (returnType.TypeName == "void")
            {
                compiler->CreateReturnCall(nullptr);
            }

            // Pop the stack
            compiler->CreateBlockBreak(nullptr, true);
            compiler->ClearCurrentSubprogram();
        }

        if (isAutoReturn)
        {
            auto sites = compiler->EndAutoReturnCapture();
            compiler->FinalizeAutoReturnFunction(name, fn, sites, allParams, varargs, returnsOwned, !structName.empty());
            return;  // auto functions do not participate in default-param overload generation in v1
        }

        GenerateDefaultParamOverloads(name, returnType, params, varargs, line);
    }

    std::vector<LLVMBackend::AnnotationValue> ParseAnnotationList(CFlatParser::AnnotationListContext* annList)
    {
        std::vector<LLVMBackend::AnnotationValue> result;
        if (!annList) return result;

        auto* compiler = Compiler(annList);
        for (auto* ann : annList->annotation())
        {
            std::string annName = ann->Identifier()->getText();

            // Validate: annotation must be declared
            auto regIt = compiler->annotationRegistry.find(annName);
            if (regIt == compiler->annotationRegistry.end())
            {
                LogErrorContext(ann, "Unknown annotation '" + annName + "': no annotation declaration found");
                continue;
            }

            std::string argValue;
            bool hasArg = ann->annotationArg() != nullptr;
            bool expectsArg = !regIt->second.empty();

            if (hasArg && !expectsArg)
            {
                LogErrorContext(ann, "Annotation '" + annName + "' does not accept arguments");
                continue;
            }
            if (!hasArg && expectsArg)
            {
                LogErrorContext(ann, "Annotation '" + annName + "' requires an argument");
                continue;
            }

            if (hasArg)
            {
                // Strip surrounding quotes from string literals
                std::string raw = ann->annotationArg()->getText();
                if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
                    argValue = raw.substr(1, raw.size() - 2);
                else
                    argValue = raw;
            }

            result.push_back({ annName, argValue });
        }
        return result;
    }

    std::vector<LLVMBackend::DeclTypeAndValue> ParseDeclarationList(std::vector<CFlatParser::DeclarationContext*> ctx)
    {
        std::vector<LLVMBackend::DeclTypeAndValue> result;

        if (ctx.size() > 0)
        {
            for (auto decl : ctx)
            {
                auto direct = decl->declarationSpecifiers();
                auto typeAndValue = ParseDeclarationSpecifiers(direct);

                auto annotations = ParseAnnotationList(decl->annotationList());

                auto initDeclList = decl->initDeclaratorList()->initDeclarator();
                for (auto initDecl : initDeclList)
                {
                    auto declarator = initDecl->declarator();
                    auto initializer = initDecl->initializer();

                    auto* directDecl = declarator->directDeclarator();
                    std::string name = getDirectDeclName(directDecl);
                    typeAndValue.ConstArraySize = 0;  // reset per-declarator
                    if (directDecl->assignmentExpression())
                    {
                        // C-style fixed-size array field: char buf[N]
                        auto* sizeVal = ParseAssignmentExpression(directDecl->assignmentExpression());
                        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(sizeVal))
                            typeAndValue.ConstArraySize = ci->getZExtValue();
                        else
                            LogErrorContext(directDecl, "array size must be a compile-time constant");
                    }
                    typeAndValue.VariableName = name;
                    typeAndValue.Initializer = initializer;
                    typeAndValue.Annotations = annotations;

                    // Bitfield: `int flags : 3 = 0;` - the constantExpression after ':'
                    // is the declared width in bits. Set IsBitfield + BitWidth here;
                    // the packing pass (LayoutBitfields in LLVMBackend.h) fills in
                    // BitOffset and StorageFieldIndex before CreateStructType.
                    if (auto* widthExpr = initDecl->constantExpression())
                    {
                        llvm::Value* widthVal = ParseConditionalExpression(widthExpr->conditionalExpression());
                        auto* widthCi = llvm::dyn_cast_or_null<llvm::ConstantInt>(widthVal);
                        if (!widthCi)
                        {
                            LogErrorContext(widthExpr, "bitfield width must be a compile-time constant integer");
                        }
                        else
                        {
                            int64_t w = widthCi->getSExtValue();
                            if (w < 0)
                                LogErrorContext(widthExpr, "bitfield width must be non-negative");
                            else
                                typeAndValue.BitWidth = (unsigned)w;
                            typeAndValue.IsBitfield = true;
                        }
                    }

                    result.push_back(typeAndValue);  // Copy
                }
            }
        }

        return result;
    }

    std::vector<std::pair<std::string, llvm::AllocaInst*>> ParseForDeclaration(CFlatParser::ForDeclarationContext* ctx)
    {
        auto declSpec = ctx->declarationSpecifiers();
        auto initDecl = ctx->initDeclaratorList();
        return ParseDeclaration(declSpec, initDecl);
    }

    std::vector<std::pair<std::string, llvm::AllocaInst*>> ParseDeclaration(CFlatParser::DeclarationContext* ctx)
    {
        // Handle enum declarations which use the enumSpecifier alternative in the grammar
        if (auto enumSpec = ctx->enumSpecifier())
        {
            ParseEnumSpecifier(enumSpec);
            return {};
        }

        auto declSpec = ctx->declarationSpecifiers();
        auto initDecl = ctx->initDeclaratorList();
        return ParseDeclaration(declSpec, initDecl);
    }

    void ParseEnumSpecifier(CFlatParser::EnumSpecifierContext* ctx)
    {
        auto* compiler = Compiler(ctx);
        if (!ctx) return;

        // enum Identifier : typeSpecifier { enumeratorList }
        auto id = ctx->Identifier();
        std::string enumName = id ? id->getText() : "";
        auto typeSpec = ctx->typeSpecifier();
        std::string backingType = typeSpec ? typeSpec->getText() : "int";

        // Register enum name as a namespace so members can be referenced as EnumName.Member
        if (!enumName.empty())
            compiler->RegisterNamespace(enumName);

        long long current = 0;
        auto list = ctx->enumeratorList();
        if (!list) return;

        for (auto enumerator : list->enumerator())
        {
            auto enumConst = enumerator->enumerationConstant();
            std::string name = enumConst->getText();

            long long value = current;
            if (auto cexpr = enumerator->constantExpression())
            {
                auto cond = cexpr->conditionalExpression();
                llvm::Value* condVal = ParseConditionalExpression(cond);
                auto valLLVM = llvm::dyn_cast<llvm::ConstantInt>(condVal);
                if (!valLLVM)
                    LogErrorContext(enumerator, "enum value must be a constant integer expression");
                value = valLLVM->getSExtValue();
            }

            LLVMBackend::TypeAndValue tv;
            // Use the enum's declared name as the type for the enumerator variable so
            // overload resolution can consider enum type. The GetType call will resolve
            // the enum to its backing type when emitting IR.
            tv.TypeName = !enumName.empty() ? enumName : backingType;
            tv.VariableName = enumName.empty() ? name : (enumName + "." + name);
            tv.Pointer = false;

            // Register the enum's backing type so GetType and overload resolution can
            // resolve enum types to the underlying integral type.
            if (!enumName.empty())
                compiler->RegisterEnumBackingType(enumName, backingType);

            // Create a typed constant using the backing type
            llvm::Constant* c = compiler->CreateConstant(backingType, std::to_string(value));
            compiler->CreateGlobalVariable(tv, c);

            current = value + 1;
        }
    }

    std::vector<std::pair<std::string, llvm::AllocaInst*>> ParseDeclaration(CFlatParser::DeclarationSpecifiersContext* declSpec, CFlatParser::InitDeclaratorListContext* initDecl)
    {
        auto* compiler = Compiler(declSpec);
        std::vector<std::pair<std::string, llvm::AllocaInst*>> allocList;

        size_t line = declSpec->getStart()->getLine();
        auto typeAndValue = ParseDeclarationSpecifiers(declSpec);

        // Queue any pending generic instantiation for this declaration's type.
        // Actual instantiation happens later in ProcessPendingInstantiations() at top-level scope.
        QueueInstantiateGenericType(declSpec);

        auto initDeclarVec = initDecl->initDeclarator();

        if (typeAndValue.TypeName.empty() && !typeAndValue.IsFunctionPointer)
        {
            LogErrorContext(declSpec, "Declaration has an empty type name.");
            return allocList;
        }

        llvm::Value* arraySize = nullptr;
        if (typeAndValue.ArraySize)
        {
            arraySize = ParseAssignmentExpression(typeAndValue.ArraySize);
        }

        // Evaluate inner dimensions for multi-dim arrays: T[N][M] → ConstInnerDimensions = {M}
        typeAndValue.ConstInnerDimensions.clear();
        for (auto* dimExpr : typeAndValue.ExtraArrayDims)
        {
            auto* dimVal = ParseAssignmentExpression(dimExpr);
            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(dimVal))
                typeAndValue.ConstInnerDimensions.push_back(ci->getZExtValue());
            else
                LogErrorContext(dimExpr, "array dimension must be a compile-time constant");
        }

        for (auto initDecl : initDeclarVec)
        {
            /*
            declarator
            : directDeclarator
            | directDeclarator '[' typeQualifierList? assignmentExpression? ']'
            | directDeclarator '[' 'static' typeQualifierList? assignmentExpression ']'
            | directDeclarator '[' typeQualifierList 'static' assignmentExpression ']'
            | directDeclarator '[' typeQualifierList? '*' ']'
            | directDeclarator '(' parameterTypeList ')'
            | directDeclarator '(' identifierList? ')'
            ;
            */
            auto declarator = initDecl->declarator();
            auto direct = declarator->directDeclarator();
            auto paramTypeList = declarator->parameterTypeList();
            // A declarator with parens but no paramTypeList is a zero-parameter function:
            // matches grammar alternative `directDeclarator '(' identifierList? ')'`
            bool hasParens = declarator->children.size() > 1;

            if (paramTypeList != nullptr || hasParens)
            {
                // If there is parameter list (or empty parens), then it is a function.
                auto declParams = paramTypeList ? ParseParameterTypeList(paramTypeList) : std::vector<LLVMBackend::DeclTypeAndValue>{};
                std::vector<LLVMBackend::TypeAndValue> allParams(declParams.begin(), declParams.end());

                bool ellipsis = paramTypeList && paramTypeList->Ellipsis() != nullptr;
                compiler->CreateFunctionDeclaration(direct->getText(), typeAndValue, allParams, typeAndValue.external, ellipsis, false, false, typeAndValue.IsStdcall, typeAndValue.IsCdecl);

                // Declare overloads for each suffix of omitted default parameters
                int firstDefault = -1;
                for (int i = 0; i < (int)declParams.size(); i++)
                {
                    if (declParams[i].DefaultValue != nullptr) { firstDefault = i; break; }
                }
                for (int cutoff = firstDefault; firstDefault >= 0 && cutoff < (int)declParams.size(); cutoff++)
                {
                    std::vector<LLVMBackend::TypeAndValue> wrapperParams(declParams.begin(), declParams.begin() + cutoff);
                    compiler->CreateFunctionDeclaration(direct->getText(), typeAndValue, wrapperParams, typeAndValue.external, false, false, false, typeAndValue.IsStdcall, typeAndValue.IsCdecl);
                }
            }
            else if (direct != nullptr)
            {
                auto identList = declarator->identifierList();
                std::string name = getDirectDeclName(direct);
                // Fixed-size array local - two forms:
                //   C-style:    T arr[N]  - size in directDeclarator
                //   Type-first: T[N] arr  - size in declarationSpecifier (arraySize already evaluated)
                typeAndValue.ConstArraySize = 0;  // reset per-declarator
                if (direct->assignmentExpression() && typeAndValue.ArraySize == nullptr)
                {
                    // Pointer arrays (char* arr[N]) can't use type-first form - grammar
                    // doesn't support 'char*[N]' - so only error on non-pointer types.
                    if (!typeAndValue.Pointer)
                        LogErrorContext(direct, std::format(
                            "C-style array declaration '{}[{}]' is not allowed; use '[{}] {}'",
                            name, direct->assignmentExpression()->getText(),
                            direct->assignmentExpression()->getText(), name));
                    auto* sizeVal = ParseAssignmentExpression(direct->assignmentExpression());
                    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(sizeVal))
                        typeAndValue.ConstArraySize = ci->getZExtValue();
                }
                else if (arraySize != nullptr)
                {
                    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(arraySize))
                        typeAndValue.ConstArraySize = ci->getZExtValue();
                }
                typeAndValue.VariableName = name;

                if (identList != nullptr)
                {
                    // TODO
                    LogErrorContext(identList, "Not Yet Implemented.");
                }

                llvm::Value* right = nullptr;
                bool srcIsUnsigned = false;
                bool srcIsBorrowed = false;
                std::string srcBorrowedOrigin;
                bool srcIsOwningMove = false;       // RHS is an owning pointer (move param or alias thereof via cast)
                std::string srcOwningName;          // name of the original owning source, for nulling on transfer
                auto initializer = initDecl->initializer();

                // Bare-brace form: T[N] arr {} - LeftBrace/initializerList are on initDecl directly.
                // Equivalent to T[N] arr = {} / T[N] arr = {field=v,...}.
                bool barebraceInit = initDecl->LeftBrace() != nullptr;
                CFlatParser::InitializerListContext* barebraceList = barebraceInit ? initDecl->initializerList() : nullptr;

                // Array value-initializer: T[N] arr = {} / T[N] arr {} / T[N] arr = {field=v,...}
                // Seed-once pattern: construct one element, then memcpy into every slot.
                if (typeAndValue.ConstArraySize > 0 &&
                    ((initializer != nullptr && initializer->LeftBrace() != nullptr) || barebraceInit))
                {
                    if (global_scope)
                    {
                        LogErrorContext(initDecl, "array value-initializer '= {}' is not allowed at global scope");
                    }
                    else
                    {
                        auto structData = compiler->GetDataStructure(typeAndValue.TypeName);
                        if (structData.StructType == nullptr)
                        {
                            LogErrorContext(initDecl, std::format("array value-initializer '= {{}}' requires a struct element type, '{}' is not a struct", typeAndValue.TypeName));
                        }
                        else
                        {
                            // Build a single-element seed alloca and default-construct it.
                            auto* elemTy = structData.StructType;
                            auto* seedAlloc = compiler->AllocaAtEntry(elemTy, nullptr, "arrayseed");
                            if (compiler->GetFunction(typeAndValue.TypeName))
                            {
                                llvm::Value* seedVal = compiler->CreateOverloadedFunctionCall(typeAndValue.TypeName, {});
                                if (seedVal) compiler->CreateAssignment(seedVal, seedAlloc);
                            }

                            // Apply field overrides onto the seed (= {} or bare {} form).
                            auto* initList = barebraceInit ? barebraceList
                                           : initializer->initializerList();
                            if (initList)
                                EmitFieldInitializer(seedAlloc, typeAndValue.TypeName, initList);

                            // Create the array alloca and register it.
                            auto* arrAlloc = compiler->CreateLocalVariable(typeAndValue, nullptr, arraySize, line);
                            allocList.push_back(std::pair(name, arrAlloc));

                            // Memcpy seed into every element.
                            auto* arrTy = llvm::cast<llvm::ArrayType>(compiler->GetType(typeAndValue));
                            const auto& dl = compiler->module->getDataLayout();
                            uint64_t elemBytes = dl.getTypeAllocSize(elemTy);
                            llvm::Value* zero = compiler->builder->getInt32(0);
                            uint64_t n = typeAndValue.ConstArraySize;
                            for (uint64_t i = 0; i < n; i++)
                            {
                                llvm::Value* idx = compiler->builder->getInt32((uint32_t)i);
                                auto* elemPtr = compiler->builder->CreateInBoundsGEP(arrTy, arrAlloc, {zero, idx}, "arrelem");
                                compiler->builder->CreateMemCpy(elemPtr, llvm::MaybeAlign(), seedAlloc, llvm::MaybeAlign(), elemBytes);
                            }
                        }
                    }
                    continue;
                }

                // At global scope, any RHS evaluation emits IR into whatever block the builder
                // currently points at (the tail of the last generated function), corrupting it.
                // Guard against this by redirecting into a throwaway block; if the result isn't
                // a compile-time constant we emit an error and discard the temp IR.
                llvm::Function* globalInitTempFn = nullptr;
                LLVMBackend::BuilderState globalInitSavedState;
                if (global_scope && initializer != nullptr && initializer->assignmentExpression() != nullptr)
                {
                    globalInitSavedState = compiler->SaveBuilderState();
                    auto* voidTy = llvm::FunctionType::get(compiler->builder->getVoidTy(), false);
                    globalInitTempFn = llvm::Function::Create(
                        voidTy, llvm::Function::PrivateLinkage, "__global_init_tmp", compiler->module.get());
                    auto* tmpBB = llvm::BasicBlock::Create(*compiler->context, "entry", globalInitTempFn);
                    compiler->builder->SetInsertPoint(tmpBB);
                }

                if (initializer != nullptr)
                {
                    auto assignmentExpression = initializer->assignmentExpression();
                    if (assignmentExpression != nullptr)
                    {
                        if (typeAndValue.IsInterface)
                        {
                            // For interface declarations, preserve NamedVariable type info
                            // so we can do the class->interface fat-struct upcast when needed.
                            auto rightNV = ParseAssignmentExpressionNamed(assignmentExpression);
                            right = LoadNamedVariable(rightNV);
                            if (right && right->getType() != compiler->GetFatPtrType())
                            {
                                std::string structName = rightNV.TypeAndValue.TypeName;
                                // Catch the case where the RHS is a known type that doesn't implement the interface.
                                bool rhsIsKnownType = !structName.empty() &&
                                    (compiler->dataStructures.count(structName) || compiler->programTable.count(structName));
                                if (rhsIsKnownType && !compiler->StructImplementsInterface(structName, typeAndValue.TypeName))
                                {
                                    LogErrorContext(assignmentExpression,
                                        std::format("'{}' does not implement interface '{}'", structName, typeAndValue.TypeName));
                                }
                                else if (!structName.empty() && compiler->StructImplementsInterface(structName, typeAndValue.TypeName))
                                {
                                    auto vtable = compiler->GetOrCreateVTable(structName, typeAndValue.TypeName);
                                    // BuildInterfaceFatValue needs a *pointer* to the struct data,
                                    // not the loaded struct value.
                                    // - Pointer types (e.g. StringData*): the loaded value IS the pointer -> use it directly.
                                    // - Value types (e.g. Point by value): use the alloca address; spill if no storage.
                                    llvm::Value* dataPtr;
                                    if (rightNV.TypeAndValue.Pointer)
                                    {
                                        // RHS is already a pointer to the class (e.g. Circle* c)
                                        dataPtr = right;
                                    }
                                    else
                                    {
                                        dataPtr = rightNV.Storage;
                                        if (!dataPtr)
                                        {
                                            dataPtr = compiler->CreateAlloca(right->getType());
                                            compiler->CreateAssignment(right, dataPtr);
                                        }
                                    }
                                    right = compiler->BuildInterfaceFatValue(vtable, dataPtr);
                                }
                                else if (typeAndValue.TypeName == "string" &&
                                         right->getType() == compiler->builder->getInt8Ty()->getPointerTo())
                                {
                                    // A raw i8*/char* assigned to a string variable.
                                    // If it is a compile-time string literal constant (length known at
                                    // compile time), wrap it directly in a string struct on the caller's stack.
                                    // Otherwise call user-defined operator string(char*) for runtime values.
                                    auto* c = llvm::dyn_cast<llvm::Constant>(right);
                                    if (c && compiler->IsStringLiteralConstant(c))
                                    {
                                        right = compiler->WrapStringLiteralAsString(right);
                                    }
                                    else if (compiler->GetFunction("operator string"))
                                    {
                                        LLVMBackend::NamedVariable argNV;
                                        argNV.Primary = right;
                                        argNV.BaseType = right->getType();
                                        argNV.TypeAndValue.TypeName = "char";
                                        argNV.TypeAndValue.Pointer = true;
                                        right = compiler->CreateOverloadedFunctionCall("operator string", { argNV });
                                    }
                                    else
                                    {
                                        right = compiler->WrapStringLiteralAsString(right);
                                    }
                                }
                            }
                            // nullptr constant assigned to an interface variable - produce null fat pointer {null, null}
                            if (llvm::isa_and_nonnull<llvm::ConstantPointerNull>(right))
                                right = llvm::Constant::getNullValue(compiler->GetFatPtrType());
                        }
                        else
                        {
                            // Thread expected function-pointer type into lambda expression parsing.
                            if (typeAndValue.IsFunctionPointer)
                                lambdaExpectedType = typeAndValue;
                            std::string genericFuncCallerName;
                            bool rhsIsFuncPtr = false;
                            std::vector<LLVMBackend::TypeAndValue::FuncPtrParam> rhsFuncPtrParams;
                            {
                                auto rightNV = ParseAssignmentExpressionNamed(assignmentExpression);
                                right = LoadNamedVariable(rightNV);
                                srcIsUnsigned = rightNV.TypeAndValue.IsUnsignedInteger() != -1;
                                genericFuncCallerName = rightNV.CallerName;
                                srcIsBorrowed = rightNV.IsBorrowed;
                                srcBorrowedOrigin = rightNV.BorrowedOrigin.empty()
                                    ? rightNV.CallerName
                                    : rightNV.BorrowedOrigin;
                                // Trigger transfer only when the RHS is an owning pointer whose
                                // source link was severed (Storage cleared) - typically by a cast.
                                // Direct refs ('T* q = p') keep Storage set and follow today's
                                // alias semantics in ParseDeclaration; we don't disturb them.
                                srcIsOwningMove = rightNV.IsOwning
                                    && rightNV.TypeAndValue.Pointer
                                    && rightNV.Storage == nullptr
                                    && !rightNV.CallerName.empty();
                                srcOwningName = rightNV.CallerName;
                                rhsIsFuncPtr = rightNV.TypeAndValue.IsFunctionPointer;
                                rhsFuncPtrParams = rightNV.TypeAndValue.FuncPtrParams;
                            }
                            lambdaExpectedType = {};
                            // Implicit char* → string coercion: string s = "hello" or string s = charPtr.
                            if (right && typeAndValue.TypeName == "string" && !typeAndValue.Pointer
                                && right->getType() == compiler->builder->getInt8Ty()->getPointerTo())
                            {
                                auto* c = llvm::dyn_cast<llvm::Constant>(right);
                                if (c && compiler->IsStringLiteralConstant(c))
                                    right = compiler->WrapStringLiteralAsString(right);
                                else if (compiler->GetFunction("operator string"))
                                {
                                    LLVMBackend::NamedVariable argNV;
                                    argNV.Primary = right;
                                    argNV.BaseType = right->getType();
                                    argNV.TypeAndValue.TypeName = "char";
                                    argNV.TypeAndValue.Pointer = true;
                                    right = compiler->CreateOverloadedFunctionCall("operator string", { argNV });
                                }
                                else
                                    right = compiler->WrapStringLiteralAsString(right);
                            }
                            // Bare 'function' type inference: infer signature from the assigned function value.
                            if (right && typeAndValue.IsFunctionPointer && typeAndValue.FuncPtrReturnTypeName.empty())
                            {
                                std::string funcName = assignmentExpression->getText();
                                auto inferred = compiler->MakeFuncPtrTypeAndValue(funcName);
                                if (inferred.IsFunctionPointer)
                                {
                                    typeAndValue.FuncPtrReturnTypeName = inferred.FuncPtrReturnTypeName;
                                    typeAndValue.FuncPtrReturnPointer = inferred.FuncPtrReturnPointer;
                                    typeAndValue.FuncPtrParams = inferred.FuncPtrParams;
                                }
                            }
                            // Wrap named function in closure fat struct when declaring function<T>.
                            if (right && typeAndValue.IsFunctionPointer && !right->getType()->isStructTy())
                            {
                                // Re-resolve by name to avoid picking a struct method that shares the
                                // same plain key in functionTable (e.g. atomic_counter::add vs add).
                                std::string funcName = assignmentExpression->getText();
                                int expectedParams = (int)typeAndValue.FuncPtrParams.size();
                                if (auto* correctFn = compiler->GetFunctionForFuncPtr(funcName, expectedParams, &typeAndValue.FuncPtrParams))
                                    right = correctFn;
                                if (auto* fn = llvm::dyn_cast<llvm::Function>(right))
                                {
                                    VerifyFuncPtrAssignmentMoveFlags(funcName, typeAndValue, assignmentExpression);
                                    right = compiler->WrapBareValueAsFatStruct(fn);
                                }
                            }
                            // Fallback for generic function pointers (e.g. function<int(void*)> fp = wrap<int>):
                            // the RHS parses to an empty namedVar with CallerName set to the mangled function name.
                            if (!right && typeAndValue.IsFunctionPointer && !genericFuncCallerName.empty())
                            {
                                int expectedParams = (int)typeAndValue.FuncPtrParams.size();
                                llvm::Function* genericFn = compiler->GetFunctionForFuncPtr(genericFuncCallerName, expectedParams, &typeAndValue.FuncPtrParams);
                                if (genericFn)
                                {
                                    VerifyFuncPtrAssignmentMoveFlags(genericFuncCallerName, typeAndValue, assignmentExpression);
                                    right = compiler->WrapBareValueAsFatStruct(genericFn);
                                }
                            }

                            // Funcptr-to-funcptr declaration init: per-param IsMove flags must agree.
                            if (right && typeAndValue.IsFunctionPointer && rhsIsFuncPtr
                                && right->getType()->isStructTy()
                                && typeAndValue.FuncPtrParams.size() == rhsFuncPtrParams.size())
                            {
                                for (size_t i = 0; i < typeAndValue.FuncPtrParams.size(); i++)
                                {
                                    if (typeAndValue.FuncPtrParams[i].IsMove != rhsFuncPtrParams[i].IsMove)
                                    {
                                        LogErrorContext(assignmentExpression, std::format(
                                            "incompatible function pointer initializer: parameter {} differs in 'move' modifier - 'move' is part of the function-pointer type",
                                            i + 1));
                                        break;
                                    }
                                }
                            }

                            // Pointer variable assigned a struct value: catch the mismatch here
                            // with a clear message rather than letting LLVM assert inside CreateCast.
                            if (right && typeAndValue.Pointer && !typeAndValue.IsFunctionPointer
                                && right->getType()->isStructTy())
                            {
                                LogErrorContext(assignmentExpression, std::format(
                                    "cannot initialize pointer '{}' with a value of type '{}' - the right-hand side must be a pointer (call getPtr() or use '&')",
                                    name, typeAndValue.TypeName));
                                right = nullptr;
                            }

                        }
                    }
                    else if (initializer->Default() != nullptr)
                    {
                        right = GenerateDefaultValue(typeAndValue);
                    }
                    else if (initializer->initializerList() != nullptr)
                    {
                        // Field initializer: MyStruct s = { field=val, ... }
                        // Initialize with the default constructor first, then override named fields.
                        right = GenerateDefaultValue(typeAndValue);
                    }
                }

                if (globalInitTempFn != nullptr)
                {
                    // Check that the initializer reduced to a compile-time constant.
                    if (right != nullptr && llvm::dyn_cast_or_null<llvm::Constant>(right) == nullptr)
                    {
                        LogErrorContext(initializer->assignmentExpression(),
                            "global variable initializer must be a compile-time constant");
                        right = nullptr;
                    }
                    // Discard the temp function (and all IR emitted into it) then restore the builder.
                    globalInitTempFn->eraseFromParent();
                    globalInitTempFn = nullptr;
                    compiler->RestoreBuilderState(globalInitSavedState);
                }

                if (right == nullptr && typeAndValue.IsNullable)
                {
                    // Nullable pointer defaults to null when no initializer is provided.
                    llvm::Type* ptrTy = compiler->GetType(typeAndValue);
                    right = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
                }

                if (right == nullptr && !typeAndValue.Pointer && typeAndValue.ConstArraySize == 0)
                {
                    auto structData = compiler->GetDataStructure(typeAndValue.TypeName);
                    if (structData.StructType != nullptr)
                    {
                        // Auto-initialize using the default constructor when available.
                        if (!global_scope && compiler->GetFunction(typeAndValue.TypeName))
                            right = compiler->CreateOverloadedFunctionCall(typeAndValue.TypeName, {});
                        else
                            LogWarningContext(direct, std::format("({}) struct and class is not initialized on the stack.", typeAndValue.TypeName));
                    }
                }

                if (global_scope)
                {
                    auto constant = llvm::dyn_cast_or_null<llvm::Constant>(right);
                    compiler->CreateGlobalVariable(typeAndValue, constant, typeAndValue.threadLocal, typeAndValue.UserAlignValue);
                    // Record the variable's declaration site so LSP go-to-definition
                    // on the variable name lands on the global itself, not on its
                    // (possibly unregistered, e.g. generic-instantiation) type.
                    if (auto* s = compiler->GetSymbolSink())
                        s->RegisterVariable(name, typeAndValue.TypeName,
                                            compiler->GetSourceFilePath(),
                                            (int)direct->getStart()->getLine(),
                                            (int)direct->getStart()->getCharPositionInLine());
                }
                else
                {
                    if (typeAndValue.threadLocal)
                        LogErrorContext(direct, "thread_local is only allowed on global variables.");
                    auto alloc = compiler->CreateLocalVariable(typeAndValue, right ? right->getType() : nullptr, arraySize, line, typeAndValue.UserAlignValue);
                    allocList.push_back(std::pair(name, alloc));

                    if (right != nullptr)
                    {
                        compiler->CreateAssignment(right, alloc, srcIsUnsigned);

                        // Apply field initializer overrides after the default value is stored.
                        if (initializer && initializer->initializerList())
                            EmitFieldInitializer(alloc, typeAndValue.TypeName, initializer->initializerList());

                        // Propagate ownership: if the RHS was a heap-allocating string call,
                        // mark this local as owning so the destructor frees the buffer on scope exit.
                        if (typeAndValue.TypeName == "string" && compiler->lastCallReturnsOwned)
                        {
                            auto& nv = compiler->stackNamedVariable.back().namedVariable[name];
                            nv.IsOwningString = true;
                            compiler->lastCallReturnsOwned = false;
                        }

                        // Propagate pointer ownership: if the RHS was a move-returning pointer call,
                        // mark this local as owning so it is freed on scope exit.
                        if (typeAndValue.Pointer && compiler->lastCallReturnsOwned)
                        {
                            compiler->stackNamedVariable.back().namedVariable[name].IsOwning = true;
                            compiler->lastCallReturnsOwned = false;
                        }

                        // Propagate new-allocation: mark local as owning its heap pointer.
                        // IsNewAllocated is set alongside IsOwning to enable refcount-on-field-escape
                        // (move params have IsOwning but not IsNewAllocated).
                        if (compiler->lastOwningResult)
                        {
                            auto& nv = compiler->stackNamedVariable.back().namedVariable[name];
                            nv.IsOwning = true;
                            nv.IsNewAllocated = true;
                            compiler->lastOwningResult = false;
                        }

                        // Propagate bond: if the RHS was a bonded call result, tag this local.
                        if (compiler->lastCallIsBonded)
                        {
                            auto& nv = compiler->stackNamedVariable.back().namedVariable[name];
                            nv.IsBonded = true;
                            nv.BondedSources = compiler->lastCallBondedSources;
                            compiler->lastCallIsBonded = false;
                            compiler->lastCallBondedSources.clear();
                        }

                        // Propagate borrow: if the RHS is a borrowed pointer (non-move param or
                        // a local that already aliases one, possibly via cast), this local also
                        // aliases the borrowed origin and must not be deleted. IsOwning/IsNewAllocated
                        // sources override this (handled above by clearing in those branches).
                        if (srcIsBorrowed && typeAndValue.Pointer && !compiler->lastOwningResult)
                        {
                            auto& nv = compiler->stackNamedVariable.back().namedVariable[name];
                            if (!nv.IsOwning && !nv.IsNewAllocated)
                            {
                                nv.IsBorrowed = true;
                                nv.BorrowedOrigin = srcBorrowedOrigin;
                            }
                        }

                        // Propagate move ownership: if the RHS is a cast (or similar) over an
                        // owning pointer source - the source link was severed (Storage cleared)
                        // but CallerName still names the original - transfer ownership to this
                        // local: tag it owning, null the source alloca, mark source moved.
                        // Without this, both the source's scope-exit cleanup and any 'delete'
                        // on this local would free the same pointer (double-free).
                        // Move params are treated like new-allocated for this purpose.
                        if (srcIsOwningMove && typeAndValue.Pointer && !compiler->lastOwningResult)
                        {
                            auto& nv = compiler->stackNamedVariable.back().namedVariable[name];
                            if (!nv.IsOwning)
                            {
                                nv.IsOwning = true;
                                nv.IsNewAllocated = true;
                                auto ref = compiler->FindVariableStorage(srcOwningName);
                                if (ref.Storage != nullptr)
                                {
                                    if (auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(ref.BaseType))
                                        compiler->builder->CreateStore(
                                            llvm::ConstantPointerNull::get(ptrTy), ref.Storage);
                                }
                                compiler->MarkVariableMoved(srcOwningName);
                            }
                        }

                    }
                }
            }
        }

        return allocList;
    }

    // Returns a NamedVariable (preserving TypeName) for simple single-child expression chains.
    // Used by ParseDeclaration to get the struct TypeName for struct->interface upcasting.
    // Falls back to value-only for complex expressions (ternary, binary ops, etc.).
    LLVMBackend::NamedVariable ParseAssignmentExpressionNamed(CFlatParser::AssignmentExpressionContext* ctx)
    {
        auto* condCtx = ctx->conditionalExpression();
        if (condCtx && !ctx->assignmentOperator()
            && !condCtx->Question() && !condCtx->QuestionQuestion())
        {
            auto* lor = condCtx->logicalOrExpression();
            if (lor)
            {
                auto las = lor->logicalAndExpression();
                if (las.size() == 1)
                {
                    auto ios = las[0]->inclusiveOrExpression();
                    if (ios.size() == 1)
                    {
                        auto eos = ios[0]->exclusiveOrExpression();
                        if (eos.size() == 1)
                        {
                            auto ands = eos[0]->andExpression();
                            if (ands.size() == 1)
                            {
                                auto eqs = ands[0]->equalityExpression();
                                if (eqs.size() == 1)
                                {
                                    auto tcs = eqs[0]->typeCheckExpression();
                                    // Guard: skip the fast path if the typeCheckExpression has 'is'/'as'
                                    // operators - those must be handled by ParseConditionalExpression.
                                    if (tcs.size() == 1 && tcs[0]->typeSpecifier().empty())
                                    {
                                        auto* relCtx = tcs[0]->relationalExpression();
                                        auto rels = relCtx ? relCtx->shiftExpression() : std::vector<CFlatParser::ShiftExpressionContext*>{};
                                        if (rels.size() == 1)
                                        {
                                            auto shs = rels[0]->additiveExpression();
                                            if (shs.size() == 1)
                                            {
                                                auto adds = shs[0]->multiplicativeExpression();
                                                if (adds.size() == 1)
                                                {
                                                    auto muls = adds[0]->castExpression();
                                                    if (muls.size() == 1)
                                                    {
                                                        return ParseCastExpression(muls[0]);
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        // Fall back: call ParseConditionalExpression directly (when no assignment) so we can
        // recover the isUnsigned flag from TypedValue and synthesize the TypeName for Upconvert.
        {
            LLVMBackend::NamedVariable result;
            auto* condCtx = ctx->conditionalExpression();
            if (condCtx && !ctx->assignmentOperator())
            {
                auto tv = ParseConditionalExpression(condCtx);
                result.Primary = tv.value;
                if (result.Primary)
                {
                    result.BaseType = result.Primary->getType();
                    if (tv.isUnsigned && result.Primary->getType()->isIntegerTy())
                    {
                        unsigned bits = result.Primary->getType()->getIntegerBitWidth();
                        if      (bits == 8)  result.TypeAndValue.TypeName = "u8";
                        else if (bits == 16) result.TypeAndValue.TypeName = "u16";
                        else if (bits == 32) result.TypeAndValue.TypeName = "u32";
                        else if (bits == 64) result.TypeAndValue.TypeName = "u64";
                    }
                }
            }
            else
            {
                result.Primary = ParseAssignmentExpression(ctx);
                if (result.Primary) result.BaseType = result.Primary->getType();
            }
            return result;
        }
    }

    llvm::Value* ParseAssignmentExpression(CFlatParser::AssignmentExpressionContext* ctx)
    {
        auto* compiler = Compiler(ctx);
        auto condCtx = ctx->conditionalExpression();
        auto assignmentOp = ctx->assignmentOperator();
        auto unaryCtx = ctx->unaryExpression();

        if (condCtx != nullptr)
        {
            return ParseConditionalExpression(condCtx);
        }
        else if (assignmentOp != nullptr)
        {
            auto operatorText = ctx->assignmentOperator()->getText();
            auto assignCtx = ctx->assignmentExpression();

            auto namedVar = ParseUnaryExpression(unaryCtx);
            auto destination = namedVar.Storage;

            // For through-pointer dereferences (*p), Storage is a raw loaded ptr (not alloca/gep/global)
            // and BaseType holds the pointee type. All loads/stores through `destination` must use it.
            // UnionFieldType is set for union field access - it overrides the load/store type so that
            // reinterpret semantics work correctly even with LLVM opaque pointers.
            auto isDerefStorage = [&]() {
                return namedVar.BaseType
                    && !llvm::isa<llvm::AllocaInst>(destination)
                    && !llvm::isa<llvm::GlobalVariable>(destination)
                    && !llvm::isa<llvm::GetElementPtrInst>(destination);
            };
            auto derefLoad = [&]() -> llvm::Value* {
                // Bitfield: the extracted value was already computed at field-access time.
                if (namedVar.BitfieldStorage)
                    return namedVar.Primary;
                if (namedVar.UnionFieldType)
                    return compiler->CreateLoad(namedVar.UnionFieldType, destination);
                return isDerefStorage()
                    ? compiler->CreateLoad(namedVar.BaseType, destination)
                    : compiler->CreateLoad(destination);
            };
            // Bitfield read-modify-write: ((word & ~window) | ((val & valMask) << off)).
            // Returns the truncated/sign-extended bitfield value so the assignment
            // expression's result matches the bitfield's declared type.
            auto bitfieldAssign = [&](llvm::Value* val) -> llvm::Value* {
                auto* storageTy = namedVar.BitfieldStorageType;
                unsigned storageBits = (unsigned)storageTy->getIntegerBitWidth();
                unsigned w = namedVar.BitfieldWidth;
                unsigned off = namedVar.BitfieldOffset;
                uint64_t valMask = (w == 64) ? ~uint64_t(0) : ((uint64_t(1) << w) - 1);
                uint64_t windowMask = valMask << off;

                // Cast the incoming value to the storage word's integer width.
                // Third arg of CreateCast is isSigned (used only when widening); flip
                // BitfieldUnsigned to get sign-extension for signed bitfields.
                auto* valAsStorage = compiler->CreateCast(val, storageTy, !namedVar.BitfieldUnsigned);
                auto* valMasked = compiler->builder->CreateAnd(valAsStorage, llvm::ConstantInt::get(storageTy, valMask));
                auto* valShifted = compiler->builder->CreateShl(valMasked, llvm::ConstantInt::get(storageTy, off));
                auto* word = compiler->CreateLoad(storageTy, namedVar.BitfieldStorage);
                auto* cleared = compiler->builder->CreateAnd(word, llvm::ConstantInt::get(storageTy, ~windowMask));
                auto* newWord = compiler->builder->CreateOr(cleared, valShifted);
                compiler->builder->CreateStore(newWord, namedVar.BitfieldStorage);

                // Recompute the extracted bitfield value (post-store) so this expression's
                // result is the value actually stored, with width/sign correctly applied.
                llvm::Value* result;
                if (namedVar.BitfieldUnsigned)
                {
                    result = valMasked;  // already masked to width, zero-extended
                }
                else
                {
                    unsigned leftShift = storageBits - w;
                    auto* shl = compiler->builder->CreateShl(valMasked, llvm::ConstantInt::get(storageTy, leftShift));
                    result = compiler->builder->CreateAShr(shl, llvm::ConstantInt::get(storageTy, leftShift));
                }
                return result;
            };
            auto derefAssign = [&](llvm::Value* val, bool isUnsigned) -> llvm::Value* {
                if (namedVar.BitfieldStorage)
                    return bitfieldAssign(val);
                if (namedVar.UnionFieldType)
                    return compiler->CreateAssignment(val, destination, isUnsigned, namedVar.UnionFieldType);
                return isDerefStorage()
                    ? compiler->CreateAssignment(val, destination, isUnsigned, namedVar.BaseType)
                    : compiler->CreateAssignment(val, destination, isUnsigned);
            };

            // Thread expected function-pointer type into lambda RHS (for f = (x) => {...} reassignment)
            if (operatorText == "=" && namedVar.TypeAndValue.IsFunctionPointer)
                lambdaExpectedType = namedVar.TypeAndValue;

            if (operatorText == "??=")
            {
                // Null-coalescing assignment: x ??= rhs  ->  if (x == 0/null) x = rhs
                auto* lhs = derefLoad();

                auto* assignBlock  = compiler->CreateBasicBlock("nullcoalasgn_assign");
                auto* resumeBlock  = compiler->CreateBasicBlock("nullcoalasgn_resume");

                // Jump to assign block only when lhs is null/zero
                compiler->CreateConditionJump(lhs, resumeBlock, assignBlock);

                compiler->SwitchToBlock(assignBlock);
                auto* rhs = ParseAssignmentExpression(assignCtx);
                derefAssign(rhs, false);
                compiler->CreateJump(resumeBlock);

                compiler->SwitchToBlock(resumeBlock);
                return derefLoad();
            }

            // Bond source reassignment check: error if LHS is a live bond source.
            // Only fire when destination is the variable's own alloca/global - field assignments
            // (GEP destinations) mutate the struct in place and do not invalidate any bond.
            // Must run before RHS evaluation so the error fires on the assignment statement.
            if (operatorText == "=" && !namedVar.CallerName.empty()
                && (llvm::isa<llvm::AllocaInst>(destination) || llvm::isa<llvm::GlobalVariable>(destination)))
            {
                auto borrower = compiler->FindActiveBondBorrower(namedVar.CallerName);
                if (!borrower.empty())
                    LogErrorContext(ctx, std::format("cannot reassign '{}' while '{}' holds a bonded reference to it - assign null to '{}' first to break the bond", namedVar.CallerName, borrower, borrower));
            }

            auto rightNV = ParseAssignmentExpressionNamed(assignCtx);
            lambdaExpectedType = {};
            // An assignment to an existing variable (not a declaration) does not transfer
            // ownership - consuming lastOwningResult here would be wrong (the new-allocated
            // value is either moved via items.add(move p) or managed by the caller).
            // Reset the flag so it doesn't leak into the next declaration in this scope.
            compiler->lastOwningResult = false;
            auto right = LoadNamedVariable(rightNV);

            // Wrap named function in closure fat struct when assigning to a function<T> variable.
            if (operatorText == "=" && namedVar.TypeAndValue.IsFunctionPointer
                && right && !right->getType()->isStructTy())
            {
                // Re-resolve by name to avoid picking a struct method that shares the
                // same plain key in functionTable (e.g. atomic_counter::add vs add).
                std::string funcName = assignCtx->getText();
                int expectedParams = (int)namedVar.TypeAndValue.FuncPtrParams.size();
                if (auto* correctFn = compiler->GetFunctionForFuncPtr(funcName, expectedParams, &namedVar.TypeAndValue.FuncPtrParams))
                    right = correctFn;
                if (auto* fn = llvm::dyn_cast<llvm::Function>(right))
                {
                    VerifyFuncPtrAssignmentMoveFlags(funcName, namedVar.TypeAndValue, ctx);
                    right = compiler->WrapBareValueAsFatStruct(fn);
                }
            }
            // Fallback for generic function pointer reassignment (e.g. fn = wrap<int>):
            // the RHS produces a null value with CallerName set to the mangled function name.
            // This mirrors the same fallback in ParseDeclaration for the initial assignment.
            if (!right && operatorText == "=" && namedVar.TypeAndValue.IsFunctionPointer
                && !rightNV.CallerName.empty())
            {
                int expectedParams = (int)namedVar.TypeAndValue.FuncPtrParams.size();
                llvm::Function* genericFn = compiler->GetFunctionForFuncPtr(rightNV.CallerName, expectedParams, &namedVar.TypeAndValue.FuncPtrParams);
                if (genericFn)
                {
                    VerifyFuncPtrAssignmentMoveFlags(rightNV.CallerName, namedVar.TypeAndValue, ctx);
                    right = compiler->WrapBareValueAsFatStruct(genericFn);
                }
            }
            // Funcptr-to-funcptr assignment: per-param IsMove flags must agree on both sides.
            if (operatorText == "=" && namedVar.TypeAndValue.IsFunctionPointer
                && rightNV.TypeAndValue.IsFunctionPointer
                && right && right->getType()->isStructTy())
            {
                const auto& lhsP = namedVar.TypeAndValue.FuncPtrParams;
                const auto& rhsP = rightNV.TypeAndValue.FuncPtrParams;
                if (lhsP.size() == rhsP.size())
                {
                    for (size_t i = 0; i < lhsP.size(); i++)
                    {
                        if (lhsP[i].IsMove != rhsP[i].IsMove)
                        {
                            LogErrorContext(ctx, std::format(
                                "incompatible function pointer assignment: parameter {} differs in 'move' modifier - 'move' is part of the function-pointer type",
                                i + 1));
                            break;
                        }
                    }
                }
            }

            // Bond escape check: bonded value cannot be assigned to a variable in a wider scope
            // than its bond source, and cannot be stored into struct fields (GEP destinations).
            if (operatorText == "=" && (rightNV.IsBonded || compiler->lastCallIsBonded))
            {
                const auto& bondedSources = rightNV.IsBonded ? rightNV.BondedSources : compiler->lastCallBondedSources;
                if (!llvm::isa<llvm::AllocaInst>(destination) && !llvm::isa<llvm::GlobalVariable>(destination))
                {
                    // Struct field or heap dereference - bonded values cannot be stored there.
                    LogErrorContext(ctx, "bonded value cannot be stored in a struct field or through a pointer - bond lifetime would be untrackable");
                }
                else if (!namedVar.CallerName.empty())
                {
                    size_t lhsDepth = compiler->FindVariableScopeDepth(namedVar.CallerName);
                    for (const auto& source : bondedSources)
                    {
                        size_t srcDepth = compiler->FindVariableScopeDepth(source);
                        if (srcDepth != SIZE_MAX && lhsDepth < srcDepth)
                            LogErrorContext(ctx, std::format("bonded value cannot be assigned to '{}' - '{}' is in a wider scope than its bond source '{}'", namedVar.CallerName, namedVar.CallerName, source));
                    }
                }
                compiler->lastCallIsBonded = false;
                compiler->lastCallBondedSources.clear();
            }

            // Interface upcast: struct* -> fat pointer when assigning to an interface variable.
            // Mirrors the same logic in the declaration initializer path (ParseDeclaration).
            if (operatorText == "=" && namedVar.TypeAndValue.IsInterface
                && right && right->getType() != compiler->GetFatPtrType())
            {
                // nullptr assigned to a non-pointer interface variable - produce null fat pointer {null, null}.
                // IsInterfacePointer (IFoo*) stores a pointer-to-fat-ptr, not a fat-ptr; skip this conversion.
                if (!namedVar.TypeAndValue.IsInterfacePointer && llvm::isa_and_nonnull<llvm::ConstantPointerNull>(right))
                    right = llvm::Constant::getNullValue(compiler->GetFatPtrType());
                std::string structName = rightNV.TypeAndValue.TypeName;
                if (!structName.empty() && compiler->StructImplementsInterface(structName, namedVar.TypeAndValue.TypeName))
                {
                    auto vtable = compiler->GetOrCreateVTable(structName, namedVar.TypeAndValue.TypeName);
                    llvm::Value* dataPtr;
                    if (rightNV.TypeAndValue.Pointer)
                        dataPtr = right;
                    else
                    {
                        dataPtr = rightNV.Storage;
                        if (!dataPtr)
                        {
                            dataPtr = compiler->CreateAlloca(right->getType());
                            compiler->CreateAssignment(right, dataPtr);
                        }
                    }
                    right = compiler->BuildInterfaceFatValue(vtable, dataPtr);
                }
            }

            // Pointer variable assigned a struct value: catch the mismatch here
            // with a clear message rather than letting LLVM assert inside CreateCast.
            // Interface targets are exempt: an interface slot legitimately holds a fat-ptr
            // struct value (produced by the upcast above), even when Pointer is set.
            if (operatorText == "=" && right && namedVar.TypeAndValue.Pointer && !namedVar.TypeAndValue.IsFunctionPointer
                && !namedVar.TypeAndValue.IsInterface
                && right->getType()->isStructTy())
            {
                LogErrorContext(ctx, std::format(
                    "cannot assign a value of type '{}' to pointer variable '{}' - the right-hand side must be a pointer (call getPtr() or use '&')",
                    rightNV.TypeAndValue.TypeName, namedVar.CallerName));
                return right;
            }

            bool rhsUnsigned = rightNV.TypeAndValue.IsUnsignedInteger() != -1;
            if (operatorText != "=")
            {
                auto left = derefLoad();
                bool lhsUnsigned = namedVar.TypeAndValue.IsUnsignedInteger() != -1;

                // Pointer compound arithmetic: use element-typed GEP (C semantics).
                if (namedVar.TypeAndValue.Pointer
                    && (operatorText == "+=" || operatorText == "-=")
                    && right && right->getType()->isIntegerTy())
                {
                    auto elemTV = namedVar.TypeAndValue;
                    elemTV.ElemPointer ? (elemTV.ElemPointer = false) : (elemTV.Pointer = false, elemTV.IsInterfacePointer = false);
                    auto* et = compiler->GetType(elemTV);
                    auto* idx = compiler->Upconvert(right, compiler->builder->getInt64Ty());
                    if (operatorText == "-=")
                        idx = compiler->builder->CreateNeg(idx, "neg");
                    right = compiler->CreateGEP(et, left, idx, "ptrarith");
                }
                else
                {
                    right = compiler->CreateOperation(operatorText, left, right, lhsUnsigned, rhsUnsigned);
                }
            }

            auto* assignResult = derefAssign(right, rhsUnsigned);
            // Lazy refcount: if a new-allocated local is assigned to a non-local destination
            // (struct field, heap object), create a refcount on first escape and increment it.
            if (operatorText == "=" && rightNV.IsNewAllocated && rightNV.TypeAndValue.Pointer
                && !rightNV.CallerName.empty()
                && destination != nullptr
                && !llvm::isa<llvm::AllocaInst>(destination)
                && !llvm::isa<llvm::GlobalVariable>(destination))
            {
                // Fetch the live RefCountStorage (rightNV is a copy; look up the actual NV).
                llvm::Value* refAlloca = rightNV.RefCountStorage;
                if (refAlloca == nullptr)
                {
                    // First escape: emit the refcount alloca at function entry (initialized to 1).
                    auto savedIP = compiler->builder->saveIP();
                    auto* fn = compiler->builder->GetInsertBlock()->getParent();
                    auto* entryBB = &fn->getEntryBlock();
                    compiler->builder->SetInsertPoint(entryBB, entryBB->begin());
                    refAlloca = compiler->builder->CreateAlloca(compiler->builder->getInt32Ty(), nullptr, "refcount");
                    compiler->builder->CreateStore(compiler->builder->getInt32(1), refAlloca);
                    compiler->builder->restoreIP(savedIP);
                    compiler->SetVariableRefCountStorage(rightNV.CallerName, refAlloca);
                }
                // Increment for this escape.
                auto* cur = compiler->builder->CreateLoad(compiler->builder->getInt32Ty(), refAlloca);
                compiler->builder->CreateStore(
                    compiler->builder->CreateAdd(cur, compiler->builder->getInt32(1), "refinc"),
                    refAlloca);
            }
            // Transfer ownership: null the source alloca so EmitDestructorsForScope
            // won't free the pointer we just stored elsewhere.
            // Move params (IsOwning && !IsNewAllocated): null for any destination type.
            // New-allocated locals (IsOwning && IsNewAllocated): null only for local destinations;
            //   struct-field escapes use refcount above so both sides validly hold the pointer.
            // When the RHS came through a cast that cleared rightNV.Storage, fall back to
            // looking up the original variable by CallerName so the source is still nulled.
            if (operatorText == "=" && rightNV.IsOwning
                && rightNV.TypeAndValue.Pointer
                && (!rightNV.IsNewAllocated
                    || destination == nullptr
                    || llvm::isa<llvm::AllocaInst>(destination)
                    || llvm::isa<llvm::GlobalVariable>(destination)))
            {
                llvm::Value* srcStorage = rightNV.Storage;
                llvm::Type*  srcBaseTy  = rightNV.BaseType;
                if (srcStorage == nullptr && !rightNV.CallerName.empty())
                {
                    auto ref = compiler->FindVariableStorage(rightNV.CallerName);
                    srcStorage = ref.Storage;
                    srcBaseTy  = ref.BaseType;
                }
                if (srcStorage != nullptr)
                {
                    if (auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(srcBaseTy))
                    {
                        compiler->builder->CreateStore(
                            llvm::ConstantPointerNull::get(ptrTy), srcStorage);
                        compiler->MarkVariableMoved(rightNV.CallerName);
                    }
                }
            }
            // Transfer ownership for move string: null _ptr so string.dtor is a no-op
            // after the value has been moved to persistent storage (e.g. list::add).
            if (operatorText == "=" && rightNV.IsOwningString && rightNV.Storage != nullptr
                && rightNV.TypeAndValue.IsMove)
            {
                if (auto* strTy = llvm::StructType::getTypeByName(*compiler->context, "string"))
                {
                    auto* ptrField = compiler->builder->CreateStructGEP(strTy, rightNV.Storage, 0);
                    compiler->builder->CreateStore(
                        llvm::ConstantPointerNull::get(compiler->builder->getPtrTy()), ptrField);
                }
            }
            // Reassignment to a moved variable makes it live again.
            if (operatorText == "=" && !namedVar.CallerName.empty())
                compiler->MarkVariableUnmoved(namedVar.CallerName);
            // Reassignment to a bonded variable breaks the bond (per design: bond is to the instance).
            if (operatorText == "=" && !namedVar.CallerName.empty())
                compiler->ClearVariableBond(namedVar.CallerName);
            return assignResult;
        }
        else if (unaryCtx)
        {
            auto namedVar = ParseUnaryExpression(unaryCtx);
            auto destination = LoadNamedVariable(namedVar);
            return destination;
        }

        LogErrorContext(ctx, "Unhandled assignment expression form.");
        return nullptr;
    }

    LLVMBackend::TypedValue ParseConditionalExpression(CFlatParser::ConditionalExpressionContext* ctx)
    {
        auto* compiler = Compiler(ctx);
        auto logicCtx = ctx->logicalOrExpression();

        if (ctx->QuestionQuestion())
        {
            // Null-coalescing: lhs ?? rhs  ->  (lhs != null) ? lhs : rhs
            llvm::Value* lhs = ParseLogicalOrExpression(logicCtx);
            if (!lhs) return {};

            auto* resultAlloca = compiler->CreateAlloca(lhs->getType());

            auto* nullBlock = compiler->CreateBasicBlock("nullcoal_null");
            auto* notNullBlock = compiler->CreateBasicBlock("nullcoal_notnull");
            auto* resumeBlock = compiler->CreateBasicBlock("nullcoal_resume");

            compiler->CreateConditionJump(lhs, notNullBlock, nullBlock);
            // insert point is now notNullBlock (lhs is not null)
            compiler->CreateAssignment(lhs, resultAlloca);
            compiler->CreateJump(resumeBlock);

            compiler->SwitchToBlock(nullBlock);
            llvm::Value* rhs = ParseConditionalExpression(ctx->conditionalExpression());
            compiler->CreateAssignment(rhs, resultAlloca);
            compiler->CreateJump(resumeBlock);

            compiler->SwitchToBlock(resumeBlock);
            return { compiler->CreateLoad(resultAlloca), false };
        }

        // Grammar: logicalOrExpression ('?' expression ':' conditionalExpression)?
        // - so `expression` is the TRUE branch and `conditionalExpression` is the FALSE branch.
        auto expressionTrueCtx = ctx->expression();
        auto expressionFalseCtx = ctx->conditionalExpression();

        if (logicCtx != nullptr)
        {
            auto condTv = ParseLogicalOrExpression(logicCtx);

            // Both expression should exist or not exist.
            if ((expressionFalseCtx != nullptr) != (expressionTrueCtx != nullptr))
            {
                LogErrorContext(ctx, "Conditional expression requires both true and false branches.");
                return {};
            }
            else if (expressionFalseCtx != nullptr && (expressionTrueCtx != nullptr))
            {
                auto trueValue  = ParseExpression(expressionTrueCtx);
                llvm::Value* falseValue = ParseConditionalExpression(expressionFalseCtx);

                // Align branch types so LLVM select has matching operand types.
                if (falseValue && trueValue && falseValue->getType() != trueValue->getType())
                {
                    auto* ft = falseValue->getType();
                    auto* tt = trueValue->getType();
                    if (ft->isIntegerTy() && tt->isIntegerTy())
                    {
                        unsigned fb = ft->getIntegerBitWidth();
                        unsigned tb = tt->getIntegerBitWidth();
                        if (fb < tb) falseValue = compiler->Upconvert(falseValue, tt);
                        else         trueValue  = compiler->Upconvert(trueValue,  ft);
                    }
                }

                auto* selectValue = compiler->CreateSelect(condTv.value, falseValue, trueValue);
                return { selectValue, false };
            }

            return condTv;
        }

        LogErrorContext(ctx, "Conditional expression has no logical-or sub-expression.");
        return {};
    }

    LLVMBackend::TypedValue ParseLogicalOrExpression(CFlatParser::LogicalOrExpressionContext* ctx)
    {
        auto* compiler = Compiler(ctx);
        auto logicCtxs = ctx->logicalAndExpression();

        if (logicCtxs.size() == 1)
        {
            return ParseLogicalAndExpression(logicCtxs[0]);
        }
        else if (logicCtxs.size() > 1)
        {
            llvm::Value* left = nullptr;

            // Always use resultStorage path. The elseBlock optimization was broken:
            // when the first || operand is true it jumped to blockFalse instead of blockTrue,
            // because only the false-destination is stored in the block context.
            LLVMBackend::TypeAndValue boolValue = { .TypeName = "bool",.VariableName = "", .Pointer = false };
            auto resultStorage = compiler->CreateAlloca(compiler->GetType(boolValue));
            auto resumeBlock = compiler->CreateBasicBlock("resumeOR");

            for (const auto& logicCtx : logicCtxs)
            {
                if (left == nullptr)
                {
                    left = ParseLogicalAndExpression(logicCtx);
                    compiler->CreateAssignment(left, resultStorage);
                }
                else
                {
                    auto falseBlock = compiler->CreateBasicBlock("falseOR");
                    auto branch = compiler->CreateConditionJump(left, resumeBlock, falseBlock);

                    compiler->InitializeBlock(falseBlock, false);
                    llvm::Value* right = ParseLogicalAndExpression(logicCtx);
                    left = compiler->CreateOperation(LLVMBackend::Operation::LogicalOr, left, right);
                    compiler->CreateAssignment(left, resultStorage);
                }
            }

            compiler->CreateBlockBreak(resumeBlock, false);

            compiler->InitializeBlock(resumeBlock, false);
            return { compiler->CreateLoad(resultStorage), false };
        }

        LogErrorContext(ctx, "Logical-OR expression has no operands.");
        return {};
    }

    LLVMBackend::TypedValue ParseLogicalAndExpression(CFlatParser::LogicalAndExpressionContext* ctx)
    {
        auto* compiler = Compiler(ctx);
        auto inclusiveCtxs = ctx->inclusiveOrExpression();

        if (inclusiveCtxs.size() == 1)
        {
            return ParseInclusiveOrExpression(inclusiveCtxs[0]);
        }
        else if (inclusiveCtxs.size() > 1)
        {
            llvm::Value* left = nullptr;

            // Always use resultStorage path. The elseBlock optimization was broken:
            // in non-condition contexts (e.g. bool x = a && b inside a loop body) the
            // elseBlock from an enclosing scope (loop exit) was incorrectly used as the
            // false-branch target, causing the loop to exit instead of continuing.
            LLVMBackend::TypeAndValue boolValue = { .TypeName = "bool",.VariableName = "", .Pointer = false };
            auto resultStorage = compiler->CreateAlloca(compiler->GetType(boolValue));
            auto resumeBlock = compiler->CreateBasicBlock("resumeAND");

            for (const auto& inclusiveCtx : inclusiveCtxs)
            {
                if (left == nullptr)
                {
                    left = ParseInclusiveOrExpression(inclusiveCtx);
                    compiler->CreateAssignment(left, resultStorage);
                }
                else
                {
                    auto trueBlock = compiler->CreateBasicBlock("trueAND");
                    auto branch = compiler->CreateConditionJump(left, trueBlock, resumeBlock);

                    compiler->InitializeBlock(trueBlock, false);
                    llvm::Value* right = ParseInclusiveOrExpression(inclusiveCtx);
                    left = compiler->CreateOperation(LLVMBackend::Operation::LogicalAnd, left, right);
                    compiler->CreateAssignment(left, resultStorage);
                }
            }

            compiler->CreateBlockBreak(resumeBlock, false);

            compiler->InitializeBlock(resumeBlock, false);
            return { compiler->CreateLoad(resultStorage), false };
        }

        LogErrorContext(ctx, "Logical-AND expression has no operands.");
        return {};
    }

    LLVMBackend::TypedValue ParseInclusiveOrExpression(CFlatParser::InclusiveOrExpressionContext* ctx)
    {
        auto exclusiveCtxs = ctx->exclusiveOrExpression();
        if (exclusiveCtxs.size() == 1)
            return ParseExclusiveOrExpression(exclusiveCtxs[0]);

        if (exclusiveCtxs.size() > 1)
        {
            auto lv = ParseExclusiveOrExpression(exclusiveCtxs[0]);
            llvm::Value* acc = lv.value;
            for (size_t i = 1; i < exclusiveCtxs.size(); i++)
            {
                auto rv = ParseExclusiveOrExpression(exclusiveCtxs[i]);
                acc = Compiler(ctx)->CreateOperation(LLVMBackend::Operation::BitwiseOr, acc, rv.value);
            }
            return { acc, lv.isUnsigned };
        }

        LogErrorContext(ctx, "Inclusive-OR expression has no operands.");
        return {};
    }

    LLVMBackend::TypedValue ParseExclusiveOrExpression(CFlatParser::ExclusiveOrExpressionContext* ctx)
    {
        auto andCtxs = ctx->andExpression();
        if (andCtxs.size() == 1)
            return ParseAndExpression(andCtxs[0]);

        if (andCtxs.size() > 1)
        {
            auto lv = ParseAndExpression(andCtxs[0]);
            llvm::Value* acc = lv.value;
            for (size_t i = 1; i < andCtxs.size(); i++)
            {
                auto rv = ParseAndExpression(andCtxs[i]);
                acc = Compiler(ctx)->CreateOperation(LLVMBackend::Operation::BitwiseXor, acc, rv.value);
            }
            return { acc, lv.isUnsigned };
        }

        LogErrorContext(ctx, "Exclusive-OR expression has no operands.");
        return {};
    }

    LLVMBackend::TypedValue ParseAndExpression(CFlatParser::AndExpressionContext* ctx)
    {
        auto nextCtxs = ctx->equalityExpression();
        if (nextCtxs.size() == 1)
            return ParseEqualityExpression(nextCtxs[0]);

        if (nextCtxs.size() > 1)
        {
            auto lv = ParseEqualityExpression(nextCtxs[0]);
            llvm::Value* acc = lv.value;
            for (size_t i = 1; i < nextCtxs.size(); i++)
            {
                auto rv = ParseEqualityExpression(nextCtxs[i]);
                acc = Compiler(ctx)->CreateOperation(LLVMBackend::Operation::BitwiseAnd, acc, rv.value);
            }
            return { acc, lv.isUnsigned };
        }

        LogErrorContext(ctx, "Bitwise-AND expression has no operands.");
        return {};
    }

    LLVMBackend::TypedValue ParseEqualityExpression(CFlatParser::EqualityExpressionContext* ctx)
    {
        auto nextCtxs = ctx->typeCheckExpression();
        if (nextCtxs.size() == 1)
        {
            return ParseTypeCheckExpression(nextCtxs[0]);
        }
        else if (nextCtxs.size() == 2)
        {
            auto lv = ParseTypeCheckExpression(nextCtxs[0]);
            auto rv = ParseTypeCheckExpression(nextCtxs[1]);
            std::string op = ctx->children[1]->getText();

            auto* overload = TryBinaryOperatorOverload(lv, op, rv, ctx);
            llvm::Value* result = overload ? overload
                                           : Compiler(ctx)->CreateOperation(op, lv, rv, lv.isUnsigned, rv.isUnsigned);
            return { result, false };  // == != result is bool, not unsigned
        }

        LogErrorContext(ctx, "Equality expression has unexpected operand count.");
        return {};
    }

    LLVMBackend::TypedValue ParseTypeCheckExpression(CFlatParser::TypeCheckExpressionContext* ctx)
    {
        auto relCtx = ctx->relationalExpression();
        if (!relCtx)
        {
            LogErrorContext(ctx, "Type check expression has no operand.");
            return {};
        }

        auto tv = ParseRelationalExpression(relCtx);
        llvm::Value* result = tv.value;

        // Handle 'is' and 'as' operators
        auto typeSpecs = ctx->typeSpecifier();
        if (typeSpecs.size() > 0)
        {
            for (size_t i = 0; i < typeSpecs.size(); i++)
            {
                std::string op = ctx->children[2 * i + 1]->getText();  // 'is' or 'as' token
                std::string targetTypeName = ParseTypeSpecifierName(typeSpecs[i]);

                if (op == "is")
                {
                    result = GenerateIsCheck(result, targetTypeName, ctx);
                }
                else if (op == "as")
                {
                    result = GenerateSafeCast(result, targetTypeName, ctx);
                }
            }
            return { result, false };  // is/as result is bool or pointer, not unsigned
        }

        return tv;
    }

    // Returns the type descriptor pointer loaded from vtable[0].
    // Works whether interfaceValue is an aggregate {i8*,i8*} or a pointer to one.
    llvm::Value* LoadTypeDescFromInterface(llvm::Value* interfaceValue, antlr4::ParserRuleContext* ctx)
    {
        auto* compiler = Compiler(ctx);
        auto ptrTy = compiler->builder->getInt8Ty()->getPointerTo();

        llvm::Value* vtablePtr;
        if (interfaceValue->getType()->isStructTy())
        {
            vtablePtr = compiler->builder->CreateExtractValue(interfaceValue, {0u});
        }
        else
        {
            auto fatTy = compiler->GetFatPtrType();
            auto vtablePtrField = compiler->builder->CreateStructGEP(fatTy, interfaceValue, 0);
            vtablePtr = compiler->builder->CreateLoad(ptrTy, vtablePtrField);
        }

        // vtable[0] holds the type descriptor pointer
        auto typeDescField = compiler->builder->CreateGEP(ptrTy, vtablePtr, compiler->builder->getInt32(0));
        return compiler->builder->CreateLoad(ptrTy, typeDescField);
    }

    llvm::Value* GenerateIsCheck(llvm::Value* interfaceValue, const std::string& targetTypeName,
                                  antlr4::ParserRuleContext* ctx)
    {
        auto* compiler = Compiler(ctx);

        auto targetIt = compiler->dataStructures.find(targetTypeName);
        if (targetIt == compiler->dataStructures.end())
        {
            LogErrorContext(ctx, std::format("'{}' is not a known struct type for 'is' check", targetTypeName));
            return nullptr;
        }

        auto* typeDesc = targetIt->second.typeDescriptor;
        if (!typeDesc)
        {
            LogErrorContext(ctx, std::format("'{}' has no type descriptor", targetTypeName));
            return nullptr;
        }

        auto loadedDesc = LoadTypeDescFromInterface(interfaceValue, ctx);
        return compiler->builder->CreateICmpEQ(loadedDesc, typeDesc);
    }

    llvm::Value* GenerateSafeCast(llvm::Value* interfaceValue, const std::string& targetTypeName,
                                  antlr4::ParserRuleContext* ctx)
    {
        auto* compiler = Compiler(ctx);
        auto ptrTy = compiler->builder->getInt8Ty()->getPointerTo();

        // Check if target is an interface (interface-to-interface cast)
        if (compiler->interfaceTable.count(targetTypeName))
        {
            auto fatTy = compiler->GetFatPtrType();

            // Extract dataPtr from source fat pointer
            llvm::Value* dataPtr;
            if (interfaceValue->getType()->isStructTy())
                dataPtr = compiler->builder->CreateExtractValue(interfaceValue, {1u});
            else
            {
                auto dp = compiler->builder->CreateStructGEP(fatTy, interfaceValue, 1);
                dataPtr = compiler->builder->CreateLoad(ptrTy, dp);
            }

            // Load type descriptor from source
            llvm::Value* loadedDesc = LoadTypeDescFromInterface(interfaceValue, ctx);

            // Alloca for result: defaults to null fat ptr (aggregate zero)
            auto* resultAlloca = compiler->CreateAlloca(fatTy);
            compiler->builder->CreateStore(llvm::ConstantAggregateZero::get(fatTy), resultAlloca);

            auto* afterBlock = compiler->CreateBasicBlock("as_iface_after");

            // For each class that implements the target interface,
            // check if the concrete type matches and build the appropriate fat pointer
            for (auto& [sName, sd] : compiler->dataStructures)
            {
                if (!compiler->StructImplementsInterface(sName, targetTypeName)) continue;
                if (!sd.typeDescriptor) continue;

                auto* matchBlock = compiler->CreateBasicBlock("as_iface_match");
                auto* nextBlock = compiler->CreateBasicBlock("as_iface_next");

                auto* cmp = compiler->builder->CreateICmpEQ(loadedDesc, sd.typeDescriptor);
                compiler->builder->CreateCondBr(cmp, matchBlock, nextBlock);

                compiler->SwitchToBlock(matchBlock);
                auto* vtable = compiler->GetOrCreateVTable(sName, targetTypeName);
                auto fatVal = compiler->BuildInterfaceFatValue(vtable, dataPtr);
                compiler->builder->CreateStore(fatVal, resultAlloca);
                compiler->builder->CreateBr(afterBlock);

                compiler->SwitchToBlock(nextBlock);
            }
            // Also check programs that implement the target interface
            for (auto& [pName, pd] : compiler->programTable)
            {
                if (!compiler->StructImplementsInterface(pName, targetTypeName)) continue;
                if (!pd.typeDescriptor) continue;

                auto* matchBlock = compiler->CreateBasicBlock("as_iface_match");
                auto* nextBlock = compiler->CreateBasicBlock("as_iface_next");

                auto* cmp = compiler->builder->CreateICmpEQ(loadedDesc, pd.typeDescriptor);
                compiler->builder->CreateCondBr(cmp, matchBlock, nextBlock);

                compiler->SwitchToBlock(matchBlock);
                auto* vtable = compiler->GetOrCreateVTable(pName, targetTypeName);
                auto fatVal = compiler->BuildInterfaceFatValue(vtable, dataPtr);
                compiler->builder->CreateStore(fatVal, resultAlloca);
                compiler->builder->CreateBr(afterBlock);

                compiler->SwitchToBlock(nextBlock);
            }

            // No match: fall through (result stays null fat ptr)
            compiler->builder->CreateBr(afterBlock);
            compiler->SwitchToBlock(afterBlock);

            // Return the fat pointer value (aggregate)
            return compiler->builder->CreateLoad(fatTy, resultAlloca);
        }

        // Concrete struct cast (existing logic)
        auto targetIt = compiler->dataStructures.find(targetTypeName);
        if (targetIt == compiler->dataStructures.end())
        {
            LogErrorContext(ctx, std::format("'{}' is not a known struct or interface type for 'as' cast", targetTypeName));
            return nullptr;
        }

        auto* typeDesc = targetIt->second.typeDescriptor;
        if (!typeDesc)
        {
            LogErrorContext(ctx, std::format("'{}' has no type descriptor", targetTypeName));
            return nullptr;
        }

        // Extract data pointer (field 1)
        llvm::Value* dataPtr;
        if (interfaceValue->getType()->isStructTy())
            dataPtr = compiler->builder->CreateExtractValue(interfaceValue, {1u});
        else
        {
            auto fatTy = compiler->GetFatPtrType();
            auto dataPtrField = compiler->builder->CreateStructGEP(fatTy, interfaceValue, 1);
            dataPtr = compiler->builder->CreateLoad(ptrTy, dataPtrField);
        }

        auto loadedDesc = LoadTypeDescFromInterface(interfaceValue, ctx);
        auto typeMatches = compiler->builder->CreateICmpEQ(loadedDesc, typeDesc);

        // In opaque pointer mode, dataPtr is already the right pointer type - no bitcast needed
        auto nullPtr = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(dataPtr->getType()));
        return compiler->builder->CreateSelect(typeMatches, dataPtr, nullPtr);
    }

    LLVMBackend::TypedValue ParseRelationalExpression(CFlatParser::RelationalExpressionContext* ctx)
    {
        auto nextCtxs = ctx->shiftExpression();
        if (nextCtxs.size() == 1)
        {
            return ParseShiftExpression(nextCtxs[0]);
        }
        else if (nextCtxs.size() == 2)
        {
            auto lv = ParseShiftExpression(nextCtxs[0]);
            auto rv = ParseShiftExpression(nextCtxs[1]);
            std::string op = ctx->children[1]->getText();

            auto* overload = TryBinaryOperatorOverload(lv, op, rv, ctx);
            llvm::Value* result = overload ? overload
                                           : Compiler(ctx)->CreateOperation(op, lv, rv, lv.isUnsigned, rv.isUnsigned);
            return { result, false };  // comparison result is bool, not unsigned
        }

        LogErrorContext(ctx, "Relational expression has unexpected operand count.");
        return {};
    }

    // Walk a single-child expression chain to the leaf Identifier terminal.
    // Returns the identifier name, or "" if the expression is complex (e.g., arithmetic, member access).
    std::string TryGetSimpleIdentifier(antlr4::ParserRuleContext* ctx)
    {
        if (!ctx) return "";
        auto& children = ctx->children;
        if (children.size() == 1)
        {
            if (auto* term = dynamic_cast<antlr4::tree::TerminalNode*>(children[0]))
            {
                if (term->getSymbol()->getType() == CFlatLexer::Identifier)
                    return term->getText();
            }
            if (auto* child = dynamic_cast<antlr4::ParserRuleContext*>(children[0]))
                return TryGetSimpleIdentifier(child);
        }
        return "";
    }

    // Find the llvm::Function* for a method named `methodName` whose first parameter is `stream`.
    llvm::Function* FindStreamMethodFn(LLVMBackend* compiler, const std::string& methodName)
    {
        auto it = compiler->functionTable.find(methodName);
        if (it == compiler->functionTable.end()) return nullptr;
        for (const auto& sym : it->second)
        {
            if (!sym.Parameters.empty() && sym.Parameters[0].TypeName == "stream")
                return sym.Function;
        }
        return nullptr;
    }

    // Wire p1.onStdout to write into the stream. Called for `p1 >> s`.
    void EmitProgramToStreamWire(const std::string& progName,
        llvm::Value* progStorage, llvm::Value* streamStorage,
        antlr4::ParserRuleContext* ctx)
    {
        auto* compiler = Compiler(ctx);
        auto* i8PtrTy  = compiler->builder->getInt8Ty()->getPointerTo();

        auto* writeBytesFn = FindStreamMethodFn(compiler, "write_bytes");
        if (!writeBytesFn)
        {
            LogErrorContext(ctx, "stream::write_bytes not found - import \"stream.cb\" before using >>.");
            return;
        }

        // Create/reuse __stream_write_shim(i8* env, char* data, i32 len) - casts env to stream*, calls write_bytes.
        // The hook is non-owning: printf passes a thread-local buffer; the shim copies it via write_bytes.
        llvm::Function* shim = compiler->module->getFunction("__stream_write_shim");
        if (!shim)
        {
            auto* charPtrTy = i8PtrTy;  // char* and i8* are the same in LLVM
            auto* i32Ty     = llvm::Type::getInt32Ty(*compiler->context);
            auto* shimTy    = llvm::FunctionType::get(
                compiler->builder->getVoidTy(), {i8PtrTy, charPtrTy, i32Ty}, false);
            shim = llvm::Function::Create(shimTy, llvm::Function::InternalLinkage,
                "__stream_write_shim", *compiler->module);
            auto* bb = llvm::BasicBlock::Create(*compiler->context, "entry", shim);
            llvm::IRBuilder<> b(bb);
            auto* streamTy = compiler->GetDataStructure("stream").StructType;
            auto* selfPtr  = b.CreateBitCast(shim->getArg(0), streamTy->getPointerTo(), "stream_self");
            b.CreateCall(writeBytesFn->getFunctionType(), writeBytesFn,
                {selfPtr, shim->getArg(1), shim->getArg(2)});
            b.CreateRetVoid();
        }

        // Build fat closure {shim_i8*, stream_i8*} and store into prog.onStdout.
        auto* shimI8 = compiler->builder->CreateBitCast(shim, i8PtrTy, "write_shim_i8");
        auto* envI8  = compiler->builder->CreateBitCast(streamStorage, i8PtrTy, "stream_env_i8");
        auto* fatTy  = compiler->GetClosureFatPtrType();
        llvm::Value* fat = llvm::UndefValue::get(fatTy);
        fat = compiler->builder->CreateInsertValue(fat, shimI8, {0u});
        fat = compiler->builder->CreateInsertValue(fat, envI8,  {1u});

        auto& pd  = compiler->programTable[progName];
        auto* gep = compiler->builder->CreateStructGEP(
            pd.StructType, progStorage, pd.OnStdoutFieldIndex, "on_stdout_gep");
        compiler->builder->CreateStore(fat, gep);

        // Also store the stream pointer into _out so programs can call _out.write_bytes() directly.
        if (pd.OutFieldIndex != (unsigned)-1)
        {
            auto* outGep = compiler->builder->CreateStructGEP(
                pd.StructType, progStorage, pd.OutFieldIndex, "out_gep");
            compiler->builder->CreateStore(streamStorage, outGep);
        }
    }

    // Wire p2.onStdin to read from the stream. Called for `s >> p2`.
    void EmitStreamToProgramWire(llvm::Value* streamStorage,
        const std::string& progName, llvm::Value* progStorage,
        antlr4::ParserRuleContext* ctx)
    {
        auto* compiler = Compiler(ctx);
        auto* i8PtrTy  = compiler->builder->getInt8Ty()->getPointerTo();

        auto* readFn = FindStreamMethodFn(compiler, "read");
        if (!readFn)
        {
            LogErrorContext(ctx, "stream::read not found - import \"stream.cb\" before using >>.");
            return;
        }

        // Create/reuse __stream_read_shim(i8* env) -> char* - casts env to stream*, calls read.
        llvm::Function* shim = compiler->module->getFunction("__stream_read_shim");
        if (!shim)
        {
            auto* charPtrTy = i8PtrTy;
            auto* shimTy    = llvm::FunctionType::get(charPtrTy, {i8PtrTy}, false);
            shim = llvm::Function::Create(shimTy, llvm::Function::InternalLinkage,
                "__stream_read_shim", *compiler->module);
            auto* bb = llvm::BasicBlock::Create(*compiler->context, "entry", shim);
            llvm::IRBuilder<> b(bb);
            auto* streamTy = compiler->GetDataStructure("stream").StructType;
            auto* selfPtr  = b.CreateBitCast(shim->getArg(0), streamTy->getPointerTo(), "stream_self");
            auto* result   = b.CreateCall(readFn->getFunctionType(), readFn, {selfPtr}, "line");
            b.CreateRet(result);
        }

        auto* shimI8 = compiler->builder->CreateBitCast(shim, i8PtrTy, "read_shim_i8");
        auto* envI8  = compiler->builder->CreateBitCast(streamStorage, i8PtrTy, "stream_env_i8");
        auto* fatTy  = compiler->GetClosureFatPtrType();
        llvm::Value* fat = llvm::UndefValue::get(fatTy);
        fat = compiler->builder->CreateInsertValue(fat, shimI8, {0u});
        fat = compiler->builder->CreateInsertValue(fat, envI8,  {1u});

        auto& pd  = compiler->programTable[progName];
        auto* gep = compiler->builder->CreateStructGEP(
            pd.StructType, progStorage, pd.OnStdinFieldIndex, "on_stdin_gep");
        compiler->builder->CreateStore(fat, gep);

        // Wire return_buffer: when the consumer is done with a buffer, return it to the pool.
        auto* returnFn = FindStreamMethodFn(compiler, "return_buffer");
        if (returnFn)
        {
            // Create/reuse __stream_return_buffer_shim(i8* env, char* buf) - calls stream.return_buffer.
            llvm::Function* returnShim = compiler->module->getFunction("__stream_return_buffer_shim");
            if (!returnShim)
            {
                auto* charPtrTy = i8PtrTy;
                auto* shimTy    = llvm::FunctionType::get(
                    compiler->builder->getVoidTy(), {i8PtrTy, charPtrTy}, false);
                returnShim = llvm::Function::Create(shimTy, llvm::Function::InternalLinkage,
                    "__stream_return_buffer_shim", *compiler->module);
                auto* bb = llvm::BasicBlock::Create(*compiler->context, "entry", returnShim);
                llvm::IRBuilder<> b(bb);
                auto* streamTy = compiler->GetDataStructure("stream").StructType;
                auto* selfPtr  = b.CreateBitCast(returnShim->getArg(0), streamTy->getPointerTo(), "stream_self");
                b.CreateCall(returnFn->getFunctionType(), returnFn, {selfPtr, returnShim->getArg(1)});
                b.CreateRetVoid();
            }

            auto* returnShimI8 = compiler->builder->CreateBitCast(returnShim, i8PtrTy, "return_shim_i8");
            auto* returnEnvI8  = compiler->builder->CreateBitCast(streamStorage, i8PtrTy, "stream_env_i8_ret");
            llvm::Value* returnFat = llvm::UndefValue::get(fatTy);
            returnFat = compiler->builder->CreateInsertValue(returnFat, returnShimI8, {0u});
            returnFat = compiler->builder->CreateInsertValue(returnFat, returnEnvI8,  {1u});

            auto* retGep = compiler->builder->CreateStructGEP(
                pd.StructType, progStorage, pd.OnStdinReturnFieldIndex, "on_stdin_return_gep");
            compiler->builder->CreateStore(returnFat, retGep);
        }

        // Also store the stream pointer into _in so programs can call _in.read_buf() directly.
        if (pd.InStreamFieldIndex != (unsigned)-1)
        {
            auto* inGep = compiler->builder->CreateStructGEP(
                pd.StructType, progStorage, pd.InStreamFieldIndex, "in_gep");
            compiler->builder->CreateStore(streamStorage, inGep);
        }
    }

    // True if functionTable has an `opName` overload whose first parameter type matches
    // typeName. Used to decide whether a shift operator should dispatch to an overload
    // (member or free) instead of a primitive bit-shift.
    bool HasOperatorOverloadForFirstParam(const std::string& opName, const std::string& typeName)
    {
        auto* compiler = Compiler();
        auto it = compiler->functionTable.find(opName);
        if (it == compiler->functionTable.end()) return false;
        for (const auto& sym : it->second)
            if (!sym.Parameters.empty() && sym.Parameters[0].TypeName == typeName)
                return true;
        return false;
    }

    LLVMBackend::TypedValue ParseShiftExpression(CFlatParser::ShiftExpressionContext* ctx)
    {
        auto nextCtxs = ctx->additiveExpression();
        if (nextCtxs.size() == 1)
        {
            return ParseAdditiveExpression(nextCtxs[0]);
        }
        else if (nextCtxs.size() == 2)
        {
            auto lv = ParseAdditiveExpression(nextCtxs[0]);
            auto rv = ParseAdditiveExpression(nextCtxs[1]);
            // '>>' is two tokens in the grammar (('>' '>')), so children[1] = '>' and children[2] = '>'.
            // '<<' is a single token, so children[1] = '<<'.
            std::string op = ctx->children[1]->getText();
            if (op == ">" && ctx->children.size() > 2 && ctx->children[2]->getText() == ">")
                op = ">>";

            auto* compiler = Compiler(ctx);
            std::string lhsName = TryGetSimpleIdentifier(nextCtxs[0]);
            std::string rhsName = TryGetSimpleIdentifier(nextCtxs[1]);

            auto lhsNV = lhsName.empty() ? LLVMBackend::NamedVariable{} : compiler->GetLocalVariable(lhsName);
            if (!lhsName.empty() && lhsNV.Storage == nullptr)
                lhsNV = compiler->GetGlobalVariableNV(lhsName);
            auto rhsNV = rhsName.empty() ? LLVMBackend::NamedVariable{} : compiler->GetLocalVariable(rhsName);
            if (!rhsName.empty() && rhsNV.Storage == nullptr)
                rhsNV = compiler->GetGlobalVariableNV(rhsName);

            const std::string& lhsType = lhsNV.TypeAndValue.TypeName;
            const std::string& rhsType = rhsNV.TypeAndValue.TypeName;

            if (op == ">>")
            {
                llvm::Value* lhsStorage = lhsNV.Storage;
                llvm::Value* rhsStorage = rhsNV.Storage;

                bool lhsIsProgram = !lhsType.empty() && compiler->programTable.count(lhsType) > 0;
                bool rhsIsProgram = !rhsType.empty() && compiler->programTable.count(rhsType) > 0;
                bool lhsIsStream  = lhsType == "stream";
                bool rhsIsStream  = rhsType == "stream";

                if (lhsIsProgram && rhsIsStream && lhsStorage && rhsStorage)
                {
                    EmitProgramToStreamWire(lhsType, lhsStorage, rhsStorage, ctx);
                    return rv;  // return stream value so `p1 >> s >> p2` can chain
                }
                if (lhsIsStream && rhsIsProgram && rhsStorage)
                {
                    llvm::Value* streamPtr = lhsStorage;
                    if (!streamPtr)
                    {
                        // Spill loaded value (chain case: `(p1 >> s) >> p2` produces a loaded stream)
                        auto* streamTy = compiler->GetDataStructure("stream").StructType;
                        streamPtr = compiler->AllocaAtEntry(streamTy, nullptr, "stream_spill");
                        compiler->builder->CreateStore(lv.value, streamPtr);
                    }
                    EmitStreamToProgramWire(streamPtr, rhsType, rhsStorage, ctx);
                    return rv;  // return program value
                }
            }

            // General operator overloading for '>>' / '<<' (e.g. channel<T>::operator>>).
            // Only dispatched when the LHS is a struct/class type with a matching operator
            // overload, so primitive integer bit-shifts fall through to CreateOperation.
            if (op == ">>" || op == "<<")
            {
                std::string opName = "operator" + op;
                if (!lhsType.empty() && compiler->IsDataStructure(lhsType)
                    && HasOperatorOverloadForFirstParam(opName, lhsType))
                {
                    LLVMBackend::NamedVariable la;
                    la.TypeAndValue = lhsNV.TypeAndValue;
                    la.TypeAndValue.VariableName = "";   // positional, not a named arg
                    la.Primary      = lv.value;
                    la.BaseType     = lv.value ? lv.value->getType() : nullptr;
                    la.CallerName   = lhsName;
                    LLVMBackend::NamedVariable ra;
                    ra.TypeAndValue = rhsNV.TypeAndValue;
                    ra.TypeAndValue.VariableName = "";   // positional, not a named arg
                    ra.Primary      = rv.value;
                    ra.BaseType     = rv.value ? rv.value->getType() : nullptr;
                    ra.CallerName   = rhsName;
                    auto* res = compiler->CreateOverloadedFunctionCall(opName, { la, ra });
                    return { res, false };
                }
            }

            auto result = compiler->CreateOperation(op, lv, rv, lv.isUnsigned, rv.isUnsigned);
            return { result, false };
        }

        LogErrorContext(ctx, "Shift expression has no operands.");
        return {};
    }

    LLVMBackend::TypedValue ParseAdditiveExpression(CFlatParser::AdditiveExpressionContext* ctx)
    {
        auto nextCtxs = ctx->multiplicativeExpression();

        if (nextCtxs.size() == 1)
        {
            return ParseMultiplicativeExpression(nextCtxs[0]);
        }
        else if (nextCtxs.size() > 1)
        {
            auto lv = ParseMultiplicativeExpression(nextCtxs[0]);
            llvm::Value* lvalue = lv.value;
            bool lu = lv.isUnsigned;
            llvm::Type* elemType = lv.elemType;

            for (size_t i = 1; i < nextCtxs.size(); i++)
            {
                auto rv = ParseMultiplicativeExpression(nextCtxs[i]);
                llvm::Value* rvalue = rv.value;
                bool ru = rv.isUnsigned;
                std::string op = ctx->children[i * 2 - 1]->getText();

                if (lvalue->getType()->isPointerTy() && rvalue->getType()->isPointerTy() && op == "-")
                {
                    // ptr - ptr → element count (C ptrdiff_t semantics)
                    auto* i64Ty = Compiler(ctx)->builder->getInt64Ty();
                    auto* byteDiff = Compiler(ctx)->builder->CreateSub(
                        Compiler(ctx)->builder->CreatePtrToInt(lvalue, i64Ty),
                        Compiler(ctx)->builder->CreatePtrToInt(rvalue, i64Ty),
                        "ptrdiff");
                    if (elemType)
                    {
                        auto* elemSize = Compiler(ctx)->GetTypeSizeBytes(elemType);
                        lvalue = Compiler(ctx)->builder->CreateSDiv(byteDiff, elemSize, "ptrdiff_elem");
                    }
                    else
                    {
                        lvalue = byteDiff;
                    }
                    elemType = nullptr;
                }
                else if (elemType && lvalue->getType()->isPointerTy()
                    && rvalue && rvalue->getType()->isIntegerTy()
                    && (op == "+" || op == "-"))
                {
                    // Pointer arithmetic: ptr + int / ptr - int → GEP
                    if (op == "-")
                        rvalue = Compiler(ctx)->builder->CreateNeg(rvalue, "neg");
                    lvalue = Compiler(ctx)->CreateGEP(elemType, lvalue, rvalue, "ptrarith");
                    // elemType stays the same - result is still a pointer to the same element type
                }
                else
                {
                    auto* overload = TryBinaryOperatorOverload(lvalue, op, rvalue, ctx);
                    lvalue = overload ? overload : Compiler(ctx)->CreateOperation(op, lvalue, rvalue, lu, ru);
                    lu = lu || ru;
                    elemType = nullptr;  // arithmetic result is no longer a pointer
                }
            }

            LLVMBackend::TypedValue result{ lvalue, lu };
            result.elemType = elemType;
            return result;
        }

        LogErrorContext(ctx, "Additive expression has no operands.");
        return {};
    }

    // Diagnose move-flag mismatch when assigning a named function to a typed function pointer.
    // E.g. `function<void(move T*)> fp = borrowFn;` is rejected because borrowFn declares a borrow param.
    void VerifyFuncPtrAssignmentMoveFlags(const std::string& funcName,
                                          const LLVMBackend::TypeAndValue& funcPtrType,
                                          antlr4::ParserRuleContext* ctx)
    {
        if (!funcPtrType.IsFunctionPointer) return;
        // No move modifiers anywhere -> nothing to enforce.
        bool anyMove = false;
        for (const auto& p : funcPtrType.FuncPtrParams) if (p.IsMove) { anyMove = true; break; }
        if (!anyMove)
        {
            // Even when destination has no move flags, reject if every overload only has move params.
            // We only flag when the function table has *no* matching all-borrow overload.
        }
        if (!Compiler(ctx)->HasFunctionWithMoveFlags(funcName, funcPtrType.FuncPtrParams))
        {
            std::string expected;
            for (size_t i = 0; i < funcPtrType.FuncPtrParams.size(); i++)
            {
                if (i) expected += ", ";
                if (funcPtrType.FuncPtrParams[i].IsMove) expected += "move ";
                expected += funcPtrType.FuncPtrParams[i].TypeName;
                if (funcPtrType.FuncPtrParams[i].Pointer) expected += "*";
            }
            Compiler(ctx)->LogError(std::format(
                "function '{}' has no overload matching the 'move' modifiers of function pointer signature ({}) - 'move' is part of the function-pointer type and must agree on both sides",
                funcName, expected));
        }
    }

    llvm::Value* LoadNamedVariable(LLVMBackend::NamedVariable& namedVar)
    {
        auto* compiler = Compiler();
        if (namedVar.IsMoved && namedVar.IdentifierLine > 0)
        {
            compiler->currentLine = namedVar.IdentifierLine;
            compiler->currentColumn = namedVar.IdentifierColumn;
            compiler->LogError(std::format("use of moved variable '{}'", namedVar.CallerName));
        }
        if (namedVar.TypeAndValue.Pointer)
        {
            if (namedVar.Primary != nullptr)
                return namedVar.Primary;
            if (namedVar.Storage != nullptr)
            {
                // Pointer array decay: char*[3] used as char** → GEP to first element.
                // Must check before the alloca load path below.
                if (namedVar.BaseType && llvm::isa<llvm::ArrayType>(namedVar.BaseType))
                {
                    auto* arrTy = llvm::cast<llvm::ArrayType>(namedVar.BaseType);
                    auto* zero = compiler->builder->getInt64(0);
                    return compiler->builder->CreateGEP(arrTy, namedVar.Storage, {zero, zero}, "arrptr");
                }
                if (llvm::isa<llvm::AllocaInst>(namedVar.Storage) ||
                    llvm::isa<llvm::GlobalVariable>(namedVar.Storage) ||
                    llvm::isa<llvm::GetElementPtrInst>(namedVar.Storage))
                    return compiler->CreateLoad(namedVar.Storage);
                // Through-pointer dereference of a pointer type (e.g. *pp where pp is char**):
                // Storage is a raw loaded address - load again to get the pointer value.
                if (namedVar.BaseType && namedVar.BaseType->isPointerTy())
                    return compiler->CreateLoad(namedVar.BaseType, namedVar.Storage);
                return namedVar.Storage;
            }
            return nullptr;
        }
        else
        {
            if (namedVar.Primary != nullptr)
                return namedVar.Primary;

            if (namedVar.Storage != nullptr)
            {
                // Array-to-pointer decay: fixed-size array used as a value decays to pointer to first element
                if (namedVar.BaseType && llvm::isa<llvm::ArrayType>(namedVar.BaseType))
                {
                    auto* arrTy = llvm::cast<llvm::ArrayType>(namedVar.BaseType);
                    auto* zero = compiler->builder->getInt64(0);
                    return compiler->builder->CreateGEP(arrTy, namedVar.Storage, {zero, zero}, "arrptr");
                }
                // For through-pointer dereferences (Storage is a raw loaded ptr, not an alloca/gep/global),
                // use BaseType to emit the correctly-typed load (opaque pointers carry no type info).
                if (namedVar.BaseType
                    && !llvm::isa<llvm::AllocaInst>(namedVar.Storage)
                    && !llvm::isa<llvm::GlobalVariable>(namedVar.Storage)
                    && !llvm::isa<llvm::GetElementPtrInst>(namedVar.Storage))
                    return compiler->CreateLoad(namedVar.BaseType, namedVar.Storage);
                return compiler->CreateLoad(namedVar.Storage);
            }
        }

        // namedVar is empty - caller's block was already terminated (e.g. by a return-block inline).
        return nullptr;
    }

    // If lvalue is a struct type with a user-defined operator, dispatch to it.
    // Returns the result Value*, or nullptr to fall back to built-in CreateOperation.
    // Returns a non-null value if a user-defined operator overload was found and called,
    // nullptr if the operand type has no matching operator (fall back to primitive handling).
    llvm::Value* TryUnaryOperatorOverload(
        llvm::Value* operand, const std::string& op,
        antlr4::ParserRuleContext* ctx)
    {
        if (!operand) return nullptr;
        auto* compiler = Compiler(ctx);

        auto* ty = operand->getType();
        if (!ty->isStructTy()) return nullptr;
        auto* structTy = llvm::cast<llvm::StructType>(ty);
        if (structTy->isLiteral() || !structTy->hasName()) return nullptr;
        std::string typeName = structTy->getName().str();
        if (typeName == "__iface_fat_ptr") return nullptr;

        std::string opName = "operator" + op;
        if (!compiler->GetFunction(opName)) return nullptr;

        // Determine whether to pass the operand by pointer or by value.
        bool usePointer = false;
        {
            auto funcSym = compiler->functionTable.find(opName);
            if (funcSym != compiler->functionTable.end())
            {
                for (const auto& candidate : funcSym->second)
                {
                    if (!candidate.Parameters.empty()
                        && candidate.Parameters[0].TypeName == typeName
                        && candidate.Parameters[0].Pointer)
                    {
                        usePointer = true;
                        break;
                    }
                }
            }
        }

        if (usePointer)
        {
            auto* tempAlloca = compiler->CreateAlloca(structTy);
            compiler->CreateAssignment(operand, tempAlloca);

            LLVMBackend::NamedVariable thisNV;
            thisNV.TypeAndValue.TypeName = typeName;
            thisNV.TypeAndValue.Pointer  = true;
            thisNV.Primary = tempAlloca;

            return compiler->CreateOverloadedFunctionCall(opName, { thisNV });
        }
        else
        {
            LLVMBackend::NamedVariable thisNV;
            thisNV.TypeAndValue.TypeName = typeName;
            thisNV.TypeAndValue.Pointer  = false;
            thisNV.Primary  = operand;
            thisNV.BaseType = structTy;

            return compiler->CreateOverloadedFunctionCall(opName, { thisNV });
        }
    }

    llvm::Value* TryBinaryOperatorOverload(
        llvm::Value* lvalue, const std::string& op, llvm::Value* rvalue,
        antlr4::ParserRuleContext* ctx)
    {
        if (!lvalue) return nullptr;
        auto* compiler = Compiler(ctx);

        // If the LHS is a string literal (ptr to global constant), wrap it
        // as a %string struct so operator+(string, ...) can match.
        if (lvalue->getType()->isPointerTy())
        {
            if (auto* c = llvm::dyn_cast<llvm::Constant>(lvalue))
            {
                if (compiler->stringLiteralLenByPtr.count(c))
                    lvalue = compiler->WrapStringLiteralAsString(lvalue);
            }
        }

        auto* ty = lvalue->getType();
        if (!ty->isStructTy()) return nullptr;
        auto* structTy = llvm::cast<llvm::StructType>(ty);
        if (structTy->isLiteral() || !structTy->hasName()) return nullptr;
        std::string typeName = structTy->getName().str();
        // Fat-ptr types compare by extracted field - let CreateOperation handle it.
        if (typeName == "__iface_fat_ptr" || typeName == "__closure_fat_ptr") return nullptr;

        std::string opName = "operator" + op;
        if (!compiler->GetFunction(opName)) return nullptr;

        auto makeRightNV = [&]() {
            LLVMBackend::NamedVariable rightNV;
            rightNV.Primary  = rvalue;
            rightNV.BaseType = rvalue ? rvalue->getType() : nullptr;
            if (rvalue && rvalue->getType()->isStructTy())
            {
                auto* rst = llvm::cast<llvm::StructType>(rvalue->getType());
                if (!rst->isLiteral() && rst->hasName())
                    rightNV.TypeAndValue.TypeName = rst->getName().str();
            }
            else if (rvalue && rvalue->getType()->isPointerTy())
            {
                // If the RHS is a string literal (ptr to global constant), wrap it
                // as a %string struct so operator+(string, string) can match.
                if (auto* c = llvm::dyn_cast<llvm::Constant>(rvalue))
                {
                    if (compiler->stringLiteralLenByPtr.count(c))
                    {
                        rightNV.Primary  = compiler->WrapStringLiteralAsString(rvalue);
                        rightNV.BaseType = rightNV.Primary->getType();
                        rightNV.TypeAndValue.TypeName = "string";
                    }
                }
            }
            return rightNV;
        };

        // Determine whether to pass lvalue by pointer or by value by inspecting the
        // registered candidates - check if any candidate's first param is a pointer to
        // this struct type. If so use pointer dispatch; otherwise use value dispatch.
        bool usePointer = false;
        {
            auto funcSym = compiler->functionTable.find(opName);
            if (funcSym != compiler->functionTable.end())
            {
                for (const auto& candidate : funcSym->second)
                {
                    if (!candidate.Parameters.empty()
                        && candidate.Parameters[0].TypeName == typeName
                        && candidate.Parameters[0].Pointer)
                    {
                        usePointer = true;
                        break;
                    }
                }
            }
        }

        if (usePointer)
        {
            // By-pointer dispatch: conventional user-defined struct operators (T* this).
            auto* tempAlloca = compiler->CreateAlloca(structTy);
            compiler->CreateAssignment(lvalue, tempAlloca);

            LLVMBackend::NamedVariable thisNV;
            thisNV.TypeAndValue.TypeName = typeName;
            thisNV.TypeAndValue.Pointer  = true;
            thisNV.Primary = tempAlloca;

            return compiler->CreateOverloadedFunctionCall(opName, { thisNV, makeRightNV() });
        }
        else
        {
            // By-value dispatch: built-in value types like string whose operators
            // are defined as operator+(T a, ...) rather than operator+(T* a, ...).
            LLVMBackend::NamedVariable thisNV;
            thisNV.TypeAndValue.TypeName = typeName;
            thisNV.TypeAndValue.Pointer  = false;
            thisNV.Primary  = lvalue;
            thisNV.BaseType = structTy;

            return compiler->CreateOverloadedFunctionCall(opName, { thisNV, makeRightNV() });
        }

        return nullptr;
    }

    LLVMBackend::TypedValue ParseMultiplicativeExpression(CFlatParser::MultiplicativeExpressionContext* ctx)
    {
        auto nextCtxs = ctx->castExpression();

        if (nextCtxs.size() == 1)
        {
            auto namedVar = ParseCastExpression(nextCtxs[0]);
            bool isUnsigned = namedVar.TypeAndValue.IsUnsignedInteger() != -1;
            llvm::Type* elemType = nullptr;
            if (namedVar.TypeAndValue.Pointer)
            {
                auto elemTV = namedVar.TypeAndValue;
                elemTV.ElemPointer ? (elemTV.ElemPointer = false) : (elemTV.Pointer = false, elemTV.IsInterfacePointer = false);
                elemType = Compiler(nextCtxs[0])->GetType(elemTV);
            }
            LLVMBackend::TypedValue result{ LoadNamedVariable(namedVar), isUnsigned };
            result.elemType = elemType;
            return result;
        }
        else if (nextCtxs.size() > 1)
        {
            auto firstNV = ParseCastExpression(nextCtxs[0]);
            bool lu = firstNV.TypeAndValue.IsUnsignedInteger() != -1;
            llvm::Value* lvalue = LoadNamedVariable(firstNV);

            for (size_t i = 1; i < nextCtxs.size(); i++)
            {
                auto rightNV = ParseCastExpression(nextCtxs[i]);
                bool ru = rightNV.TypeAndValue.IsUnsignedInteger() != -1;
                llvm::Value* rvalue = LoadNamedVariable(rightNV);
                std::string op = ctx->children[i * 2 - 1]->getText();

                auto* overload = TryBinaryOperatorOverload(lvalue, op, rvalue, ctx);
                lvalue = overload ? overload : Compiler(ctx)->CreateOperation(op, lvalue, rvalue, lu, ru);
                lu = lu || ru;
            }

            return { lvalue, lu };
        }

        LogErrorContext(ctx, "Multiplicative expression has no operands.");
        return {};
    }

    LLVMBackend::NamedVariable ParseCastExpression(CFlatParser::CastExpressionContext* ctx, bool lvalue = false)
    {
        auto* compiler = Compiler(ctx);
        auto unaryCtx = ctx->unaryExpression();
        auto castExp = ctx->castExpression();
        auto typeName = ctx->typeName();

        if (unaryCtx != nullptr)
        {
            return ParseUnaryExpression(unaryCtx);
        }
        else if (castExp && typeName)
        {
            auto namedVar = ParseCastExpression(castExp);
            auto destTypeName = ParseTypeName(typeName);
            auto type = compiler->GetType(destTypeName);

            // If the destination is a struct type and an operator overload exists,
            // call it (e.g. (string)charPtr calls operator string(char*)).
            std::string opName = "operator " + destTypeName.TypeName;
            if (compiler->GetFunction(opName) != nullptr)
            {
                auto argNV = namedVar;
                argNV.TypeAndValue.VariableName = "";  // clear name so positional matching is used
                if (argNV.Primary == nullptr && argNV.Storage != nullptr)
                {
                    auto srcType = compiler->GetType(argNV.TypeAndValue);
                    argNV.Primary = compiler->CreateLoad(srcType, argNV.Storage);
                    argNV.Storage = nullptr;
                }
                auto result = compiler->CreateOverloadedFunctionCall(opName, { argNV });
                namedVar.Primary = result;
                namedVar.Storage = nullptr;
                namedVar.TypeAndValue = destTypeName;
                return namedVar;
            }

            // Load with the source type first so we read the correct number of bytes,
            // then cast to the destination type. The old code loaded directly with the
            // destination type, which reads too many bytes from narrower fields
            // (e.g., i64 load from a u32 field corrupts the adjacent field).
            if (namedVar.Primary == nullptr && namedVar.Storage != nullptr)
            {
                auto srcType = compiler->GetType(namedVar.TypeAndValue);
                namedVar.Primary = compiler->CreateLoad(srcType, namedVar.Storage);
                namedVar.Storage = nullptr;
            }

            bool srcIsSigned = namedVar.TypeAndValue.IsUnsignedInteger() == -1;
            namedVar.Primary = compiler->CreateCast(namedVar.Primary, type, srcIsSigned);
            namedVar.TypeAndValue = destTypeName;
            return namedVar;
        }

        LogErrorContext(ctx, "Cast expression has no recognized form.");
        return {};
    }

    LLVMBackend::TypeAndValue ParseTypeName(CFlatParser::TypeNameContext* ctx)
    {
        auto specCtx = ctx->specifierQualifierList();
        auto abstractDecl = ctx->abstractDeclarator();

        LLVMBackend::TypeAndValue typeValue;

        if (specCtx)
        {
            auto typeQualfier = specCtx->typeQualifier();
            auto typeSpecs = specCtx->typeSpecifier();

            if (typeSpecs.size() > 0)
            {
                // TODO Collect all of them.
                auto* typeSpec = typeSpecs[0];
                if (typeSpec->genericIdentifier() != nullptr && typeSpec->genericIdentifier()->genericTypeParameters() != nullptr)
                {
                    // Generic cast: (channel<int>*) -> mangle to channel__int
                    std::string baseName = typeSpec->genericIdentifier()->Identifier()->getText();
                    std::vector<std::string> typeArgs;
                    for (auto* entry : typeSpec->genericIdentifier()->genericTypeParameters()->typeParameterList()->typeParameterEntry())
                        typeArgs.push_back(ResolveTypeArgEntry(entry));
                    typeValue.TypeName = MangledGenericName(baseName, typeArgs);
                }
                else
                {
                    typeValue.TypeName = typeSpec->getText();
                    // Apply active type-parameter substitutions (e.g. T -> int inside a generic function body).
                    auto substIt = activeTypeSubstitutions.find(typeValue.TypeName);
                    if (substIt != activeTypeSubstitutions.end())
                    {
                        typeValue.TypeName = substIt->second;
                        while (!typeValue.TypeName.empty() && typeValue.TypeName.back() == '*')
                        {
                            typeValue.TypeName.pop_back();
                            typeValue.Pointer = true;
                        }
                    }
                }
            }
        }
        else
        {
            // undefined type.
            LogErrorContext(ctx, "Type name has no specifier-qualifier list.");
            return typeValue;
        }

        if (abstractDecl && abstractDecl->pointer())
        {
            typeValue.Pointer = true;
        }

        return typeValue;
    }

    LLVMBackend::NamedVariable ParseUnaryExpression(CFlatParser::UnaryExpressionContext* ctx)
    {
        auto* compiler = Compiler(ctx);
        auto postFixCtx = ctx->postfixExpression();
        auto castExpCtx = ctx->castExpression();
        auto unaryOperator = ctx->unaryOperator();
        auto typeNameCtx = ctx->typeName();

        // Handle sizeof/alignof as prefix: the parser may match "sizeof" and "(TypeName)"
        // separately, where the "(TypeName)" becomes a postfixExpression (function call syntax)
        if (postFixCtx != nullptr)
        {
            std::string text = ctx->getText();
            bool prefixSizeof = text.find("sizeof(") == 0;
            bool prefixAlignof = text.find("alignof(") == 0;

            if ((prefixSizeof || prefixAlignof) && typeNameCtx == nullptr)
            {
                // The parser matched sizeof/alignof as a prefix on a postfixExpression.
                // Extract the type name from the text (e.g., "sizeof(Point)" -> "Point")
                std::string postfixText = postFixCtx->getText();

                // Remove outer parentheses if present
                if (!postfixText.empty() && postfixText[0] == '(' && postfixText.back() == ')')
                {
                    postfixText = postfixText.substr(1, postfixText.length() - 2);
                }

                // Check if this looks like a type name (alphanumeric, dots, underscores, generics)
                bool likelyType = !postfixText.empty() && (std::isalpha(postfixText[0]) || postfixText[0] == '_');
                for (char c : postfixText)
                {
                    if (!std::isalnum(c) && c != '_' && c != '.' && c != '<' && c != '>' && c != '*')
                    {
                        likelyType = false;
                        break;
                    }
                }

                if (likelyType)
                {
                    // Try to parse as a type
                    LLVMBackend::TypeAndValue typeValue;
                    typeValue.TypeName = postfixText;

                    // Check for trailing * (pointer)
                    if (!typeValue.TypeName.empty() && typeValue.TypeName.back() == '*')
                    {
                        typeValue.Pointer = true;
                        typeValue.TypeName.pop_back();
                    }

                    // Apply active type-parameter substitutions (e.g. sizeof(T) inside a generic function body).
                    {
                        auto substIt = activeTypeSubstitutions.find(typeValue.TypeName);
                        if (substIt != activeTypeSubstitutions.end())
                        {
                            typeValue.TypeName = substIt->second;
                            while (!typeValue.TypeName.empty() && typeValue.TypeName.back() == '*')
                            {
                                typeValue.TypeName.pop_back();
                                typeValue.Pointer = true;
                            }
                        }
                    }

                    // sizeof(T) where T is a pack param returns the element count
                    if (prefixSizeof)
                    {
                        auto packIt = activePackSubstitutions.find(typeValue.TypeName);
                        if (packIt != activePackSubstitutions.end())
                        {
                            LLVMBackend::NamedVariable namedVar;
                            namedVar.Primary = llvm::ConstantInt::get(
                                llvm::Type::getInt32Ty(*compiler->context),
                                (int)packIt->second.size());
                            namedVar.TypeAndValue.TypeName = "int";
                            return namedVar;
                        }
                    }

                    auto* llvmType = compiler->GetType(typeValue, nullptr, true);
                    if (llvmType && !llvmType->isVoidTy())  // Void is a valid type but let's use basic validity check
                    {
                        llvm::Value* result;
                        uint64_t effAlign = typeValue.Pointer
                            ? 0
                            : compiler->GetEffectiveAlignmentForType(typeValue.TypeName, llvmType);
                        if (prefixSizeof)
                        {
                            if (effAlign > 1)
                            {
                                uint64_t paddedSize = compiler->GetEffectiveAllocSize(llvmType, effAlign);
                                result = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*compiler->context), paddedSize);
                            }
                            else
                            {
                                result = compiler->GetTypeSizeBytes(llvmType);
                            }
                        }
                        else
                        {
                            if (effAlign > 1)
                                result = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*compiler->context), effAlign);
                            else
                                result = compiler->GetTypeAlignBytes(llvmType);
                        }

                        if (result)
                        {
                            LLVMBackend::NamedVariable namedVar;
                            namedVar.Primary = result;
                            namedVar.TypeAndValue.TypeName = "i64";
                            namedVar.Storage = nullptr;
                            return namedVar;
                        }
                    }
                }
            }

            return ParsePostfixExpression(postFixCtx);
        }
        else if (auto* newCtx = ctx->newExpression())
        {
            return ParseNewExpression(newCtx);
        }
        else if (auto* delCtx = ctx->deleteExpression())
        {
            return ParseDeleteExpression(delCtx);
        }
        else if (auto* moveCtx = ctx->moveExpression())
        {
            return ParseMoveExpression(moveCtx);
        }
        else if (auto* opStrCtx = ctx->operatorStringExpression())
        {
            return ParseOperatorStringExpression(opStrCtx);
        }
        else if (unaryOperator && castExpCtx)
        {
            /* unaryOperator : '&' | '*'| '+'| '-'| '~'| '!'; */

            std::string opText = unaryOperator->getText();

            // For '!', suspend any active short-circuit else-block before
            // evaluating the operand.  Without this, !(A && B) in a while
            // condition would short-circuit the inner && directly to the
            // loop-exit block when A is false - bypassing the negation -
            // instead of continuing the loop as !(false && B) == true demands.
            llvm::BasicBlock* savedElse = nullptr;
            if (opText == "!")
                savedElse = compiler->ExchangeElseBlock(nullptr);

            auto namedVar = ParseCastExpression(castExpCtx);

            if (opText == "!")
                compiler->ExchangeElseBlock(savedElse);

            if (opText == "&")
            {
                if (namedVar.BitfieldStorage)
                {
                    LogErrorContext(ctx, std::format(
                        "cannot take the address of bitfield '{}' - bitfields are not byte-addressable",
                        namedVar.FieldName));
                }
                if (!namedVar.Storage)
                {
                    LogErrorContext(ctx, "Unable to get an Address-of an object without a Storage.");
                }

                namedVar.Primary = namedVar.Storage;
                namedVar.Storage = nullptr;
            }
            else if (opText == "*")
            {
                if (!namedVar.Storage)
                {
                    LogErrorContext(ctx, "Unable to dereference an object without a Storage.");
                }

                if (!namedVar.Storage->getType()->isPointerTy())
                {
                    LogErrorContext(ctx, "Unable to dereference a non-Pointer.");
                }

                // Strip one pointer level from the CFlat type to get the pointee type.
                // For char** (ElemPointer=true): deref gives char* (keep Pointer, clear ElemPointer).
                // For char* (ElemPointer=false): deref gives char (clear Pointer).
                if (namedVar.TypeAndValue.ElemPointer)
                    namedVar.TypeAndValue.ElemPointer = false;
                else
                {
                    namedVar.TypeAndValue.Pointer = false;
                    namedVar.TypeAndValue.IsInterfacePointer = false;
                }
                auto* pointeeType = compiler->GetType(namedVar.TypeAndValue);
                llvm::Value* loadedPtr = compiler->CreateLoad(namedVar.Storage);
                // The deref'd location: loadedPtr is the address; pointeeType is what it holds.
                // Storing it in Storage (not Primary) makes it usable as both lvalue and rvalue.
                namedVar.Storage  = loadedPtr;
                namedVar.Primary  = nullptr;
                namedVar.BaseType = pointeeType;
            }
            else if (opText == "!")
            {
                auto newValue = this->LoadNamedVariable(namedVar);
                if (auto* overload = TryUnaryOperatorOverload(newValue, "!", ctx))
                {
                    namedVar.Primary = overload;
                    namedVar.TypeAndValue = compiler->lastCallReturnType;
                }
                else
                {
                    namedVar.Primary = compiler->CreateNot(newValue);
                }
                namedVar.Storage = nullptr;
            }
            else if (opText == "-")
            {
                auto newValue = this->LoadNamedVariable(namedVar);
                if (auto* overload = TryUnaryOperatorOverload(newValue, "-", ctx))
                {
                    namedVar.Primary = overload;
                    namedVar.TypeAndValue = compiler->lastCallReturnType;
                    namedVar.Storage = nullptr;
                }
                else
                {
                    // Fold negation of integer constants into the smallest fitting type.
                    // e.g. 32768 is i32, but -32768 fits in i16 (INT16_MIN).
                    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(newValue))
                    {
                        int64_t neg = -(int64_t)ci->getSExtValue();
                        if (neg >= std::numeric_limits<int8_t>::min() && neg <= std::numeric_limits<int8_t>::max())
                            namedVar.Primary = compiler->builder->getInt8((int8_t)neg);
                        else if (neg >= std::numeric_limits<int16_t>::min() && neg <= std::numeric_limits<int16_t>::max())
                            namedVar.Primary = compiler->builder->getInt16((int16_t)neg);
                        else if (neg >= (int64_t)std::numeric_limits<int32_t>::min() && neg <= (int64_t)std::numeric_limits<int32_t>::max())
                            namedVar.Primary = compiler->builder->getInt32((int32_t)neg);
                        else
                            namedVar.Primary = compiler->builder->getInt64(neg);
                    }
                    else
                    {
                        namedVar.Primary = compiler->CreateNeg(newValue);
                    }
                    namedVar.Storage = nullptr;
                }
            }
            else if (opText == "+")
            {
                auto newValue = this->LoadNamedVariable(namedVar);
                if (auto* overload = TryUnaryOperatorOverload(newValue, "+", ctx))
                {
                    namedVar.Primary = overload;
                    namedVar.TypeAndValue = compiler->lastCallReturnType;
                }
                else
                {
                    // unary + is a no-op: just load the value
                    namedVar.Primary = newValue;
                }
                namedVar.Storage = nullptr;
            }
            else if (opText == "~")
            {
                auto newValue = this->LoadNamedVariable(namedVar);
                if (auto* overload = TryUnaryOperatorOverload(newValue, "~", ctx))
                {
                    namedVar.Primary = overload;
                    namedVar.TypeAndValue = compiler->lastCallReturnType;
                }
                else
                {
                    namedVar.Primary = compiler->CreateNot(newValue);
                }
                namedVar.Storage = nullptr;
            }
            else
            {
                LogErrorContext(ctx, std::format("{} operator is not yet implemented.", opText));
                return namedVar;
            }

            return namedVar;
        }
        else if (auto* typeNameCtx = ctx->typeName())
        {
            // Handle sizeof(typeName) or alignof(typeName)
            std::string text = ctx->getText();

            // Check if this is a sizeof or alignof expression
            bool isSizeof = text.find("sizeof(") != std::string::npos;
            bool isAlignof = text.find("alignof(") != std::string::npos;

            if (isSizeof || isAlignof)
            {
                // Parse the type name to get its LLVM type
                auto typeValue = ParseTypeName(typeNameCtx);
                if (typeValue.TypeName.empty())
                {
                    LogErrorContext(ctx, "sizeof/alignof: could not determine type");
                    return {};
                }

                // sizeof(T) where T is a pack param returns the element count, not byte size
                if (isSizeof)
                {
                    auto packIt = activePackSubstitutions.find(typeValue.TypeName);
                    if (packIt != activePackSubstitutions.end())
                    {
                        LLVMBackend::NamedVariable namedVar;
                        namedVar.Primary = llvm::ConstantInt::get(
                            llvm::Type::getInt32Ty(*compiler->context),
                            (int)packIt->second.size());
                        namedVar.TypeAndValue.TypeName = "int";
                        return namedVar;
                    }
                }

                auto* llvmType = compiler->GetType(typeValue, nullptr, true);
                if (!llvmType)
                {
                    LogErrorContext(ctx, "sizeof/alignof: could not resolve type to LLVM type");
                    return {};
                }

                // Get the size or alignment value, honoring any struct-level alignas.
                uint64_t effAlign = typeValue.Pointer
                    ? 0
                    : compiler->GetEffectiveAlignmentForType(typeValue.TypeName, llvmType);
                llvm::Value* result;
                if (isSizeof)
                {
                    if (effAlign > 1)
                    {
                        uint64_t paddedSize = compiler->GetEffectiveAllocSize(llvmType, effAlign);
                        result = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*compiler->context), paddedSize);
                    }
                    else
                    {
                        result = compiler->GetTypeSizeBytes(llvmType);
                    }
                }
                else
                {
                    if (effAlign > 1)
                        result = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*compiler->context), effAlign);
                    else
                        result = compiler->GetTypeAlignBytes(llvmType);
                }

                // Return as a named variable with i64 type
                LLVMBackend::NamedVariable namedVar;
                namedVar.Primary = result;
                namedVar.TypeAndValue.TypeName = "i64";
                namedVar.Storage = nullptr;
                return namedVar;
            }
        }

        LogErrorContext(ctx, "Unary expression has no recognized form.");
        return {};
    }

    std::string ParseTypeSpecifierName(CFlatParser::TypeSpecifierContext* ctx)
    {
        if (ctx->genericIdentifier() && ctx->genericIdentifier()->genericTypeParameters())
        {
            // Generic type: Box<int> -> Box__int
            // Also apply type substitutions to arguments (e.g. Box<T> with T=int -> Box__int)
            std::string base = ctx->genericIdentifier()->Identifier()->getText();
            std::vector<std::string> args;
            for (auto* entry : ctx->genericIdentifier()->genericTypeParameters()->typeParameterList()->typeParameterEntry())
            {
                std::string arg = entry->getText();
                // Apply active type substitutions to each type argument
                auto it = activeTypeSubstitutions.find(arg);
                if (it != activeTypeSubstitutions.end())
                    arg = it->second;
                args.push_back(arg);
            }
            return MangledGenericName(base, args);
        }
        std::string name = ctx->getText();
        // Apply active type substitutions (for generic templates)
        auto it = activeTypeSubstitutions.find(name);
        if (it != activeTypeSubstitutions.end()) name = it->second;
        return Compiler(ctx)->ResolveQualifiedName(name);
    }

    // Resolves the struct type expected at a call-site field initializer argument.
    // Pass effectiveParamIdx >= 0 for positional args (already offset by implicit 'this').
    // Pass effectiveParamIdx = -1 and non-empty namedParam for named-parameter form.
    std::string ResolveInitializerArgType(
        antlr4::ParserRuleContext* ctx,
        const std::string& functionName,
        int effectiveParamIdx,
        const std::string& namedParam)
    {
        auto* compiler = Compiler(ctx);
        auto it = compiler->functionTable.find(functionName);
        if (it == compiler->functionTable.end())
        {
            LogErrorContext(ctx, std::format("field initializer: unknown function '{}'", functionName));
            return "";
        }

        std::string resolved;
        bool ambiguous = false;
        for (const auto& sym : it->second)
        {
            const LLVMBackend::TypeAndValue* param = nullptr;
            if (!namedParam.empty())
            {
                for (const auto& p : sym.Parameters)
                {
                    if (p.VariableName == namedParam) { param = &p; break; }
                }
            }
            else if (effectiveParamIdx >= 0 && effectiveParamIdx < (int)sym.Parameters.size())
            {
                param = &sym.Parameters[effectiveParamIdx];
            }

            if (param && !param->TypeName.empty() && compiler->IsDataStructure(param->TypeName))
            {
                if (resolved.empty())
                    resolved = param->TypeName;
                else if (resolved != param->TypeName)
                    ambiguous = true;
            }
        }

        if (ambiguous)
        {
            LogErrorContext(ctx, namedParam.empty()
                ? std::format("field initializer: ambiguous struct type at argument position {}", effectiveParamIdx)
                : std::format("field initializer: ambiguous struct type for parameter '{}'", namedParam));
            return "";
        }
        if (resolved.empty())
        {
            LogErrorContext(ctx, namedParam.empty()
                ? std::format("field initializer: cannot infer struct type for argument position {}", effectiveParamIdx)
                : std::format("field initializer: parameter '{}' is not a struct type", namedParam));
            return "";
        }
        return resolved;
    }

    void EmitFieldInitializer(
        llvm::Value* structPtr,
        const std::string& typeName,
        CFlatParser::InitializerListContext* ctx)
    {
        auto* compiler = Compiler(ctx);
        auto it = compiler->dataStructures.find(typeName);
        if (it == compiler->dataStructures.end())
        {
            LogErrorContext(ctx, std::format("Field initializer: '{}' is not a known struct type", typeName));
            return;
        }
        const auto& sd = it->second;

        std::unordered_set<std::string> seen;
        for (auto* fi : ctx->fieldInit())
        {
            std::string fieldName = fi->Identifier()->getText();
            if (!seen.insert(fieldName).second)
            {
                LogErrorContext(fi, std::format("Duplicate field initializer for '{}'", fieldName));
                continue;
            }

            // Bitfield seeded-init: `Point p = {ready: 1}`. Resolve through the
            // side-table; emit a RMW into the storage slot. No address-of, no GEP
            // of the field name itself.
            const LLVMBackend::BitfieldInfo* bfHit = nullptr;
            for (const auto& b : sd.Bitfields)
                if (b.Name == fieldName) { bfHit = &b; break; }
            if (bfHit)
            {
                auto rightNV = ParseAssignmentExpressionNamed(fi->assignmentExpression());
                llvm::Value* val = LoadNamedVariable(rightNV);
                if (!val) continue;

                const auto& storageField = sd.StructFields[bfHit->StorageFieldIndex];
                auto* storageTy = compiler->GetType(storageField);
                auto* storagePtr = compiler->builder->CreateStructGEP(
                    sd.StructType, structPtr, bfHit->StorageFieldIndex, fieldName + "_bf");

                unsigned w = bfHit->BitWidth;
                unsigned off = bfHit->BitOffset;
                uint64_t valMask = (w == 64) ? ~uint64_t(0) : ((uint64_t(1) << w) - 1);
                uint64_t windowMask = valMask << off;
                auto* valAsStorage = compiler->CreateCast(val, storageTy, !bfHit->IsUnsigned);
                auto* valMasked = compiler->builder->CreateAnd(valAsStorage, llvm::ConstantInt::get(storageTy, valMask));
                auto* valShifted = compiler->builder->CreateShl(valMasked, llvm::ConstantInt::get(storageTy, off));
                auto* word = compiler->builder->CreateLoad(storageTy, storagePtr);
                auto* cleared = compiler->builder->CreateAnd(word, llvm::ConstantInt::get(storageTy, ~windowMask));
                auto* newWord = compiler->builder->CreateOr(cleared, valShifted);
                compiler->builder->CreateStore(newWord, storagePtr);
                continue;
            }

            int fieldIdx = -1;
            LLVMBackend::DeclTypeAndValue fieldType;
            for (int i = 0; i < (int)sd.StructFields.size(); i++)
            {
                if (sd.StructFields[i].VariableName == fieldName)
                {
                    fieldIdx = i;
                    fieldType = sd.StructFields[i];
                    break;
                }
            }
            if (fieldIdx < 0)
            {
                LogErrorContext(fi, std::format("'{}' has no field named '{}'", typeName, fieldName));
                continue;
            }

            auto rightNV = ParseAssignmentExpressionNamed(fi->assignmentExpression());
            llvm::Value* val = LoadNamedVariable(rightNV);
            if (!val) continue;

            // Coerce char* string literals to the string struct type
            if (fieldType.TypeName == "string" && !fieldType.Pointer
                && val->getType() == compiler->builder->getInt8Ty()->getPointerTo())
            {
                auto* c = llvm::dyn_cast<llvm::Constant>(val);
                if (c && compiler->IsStringLiteralConstant(c))
                    val = compiler->WrapStringLiteralAsString(val);
                else if (compiler->GetFunction("operator string"))
                {
                    LLVMBackend::NamedVariable argNV;
                    argNV.Primary = val;
                    argNV.BaseType = val->getType();
                    val = compiler->CreateOverloadedFunctionCall("operator string", { argNV });
                }
            }
            // Coerce struct pointer to interface fat pointer
            else if (fieldType.IsInterface && val->getType() != compiler->GetFatPtrType())
            {
                std::string srcName = rightNV.TypeAndValue.TypeName;
                if (!srcName.empty() && compiler->StructImplementsInterface(srcName, fieldType.TypeName))
                {
                    auto vtable = compiler->GetOrCreateVTable(srcName, fieldType.TypeName);
                    llvm::Value* dataPtr = rightNV.TypeAndValue.Pointer ? val : rightNV.Storage;
                    if (!dataPtr)
                    {
                        dataPtr = compiler->CreateAlloca(val->getType());
                        compiler->CreateAssignment(val, dataPtr);
                    }
                    val = compiler->BuildInterfaceFatValue(vtable, dataPtr);
                }
            }

            if (sd.IsUnion)
            {
                // Union: all fields alias at offset 0. Store through the raw pointer with the explicit field type.
                auto* fieldLLVMType = compiler->GetType(fieldType);
                compiler->CreateAssignment(val, structPtr, false, fieldLLVMType);
            }
            else
            {
                auto* gep = compiler->builder->CreateStructGEP(sd.StructType, structPtr, (unsigned)fieldIdx, fieldName + "_init");
                compiler->builder->CreateStore(val, gep);
            }
        }
    }

    LLVMBackend::NamedVariable ParseNewExpression(CFlatParser::NewExpressionContext* ctx)
    {
        auto* compiler = Compiler(ctx);
        std::string typeName = ParseTypeSpecifierName(ctx->typeSpecifier());
        bool isArray = ctx->assignmentExpression() != nullptr;

        // If type substitution produced a pointer type (e.g. T=Employee*), strip the '*'
        // and treat as pointer-to-element so GetType resolves the base type correctly.
        bool typeIsPtr = false;
        while (!typeName.empty() && typeName.back() == '*')
        {
            typeName.pop_back();
            typeIsPtr = true;
        }

        LLVMBackend::TypeAndValue typeInfo{ .TypeName = typeName, .Pointer = typeIsPtr };
        llvm::Type* elemType = compiler->GetType(typeInfo);

        // Compute allocation size
        if (!elemType || !elemType->isSized())
        {
            LogErrorContext(ctx, std::format("'new': cannot compute size of unsized or unresolved type '{}'", typeName));
            return {};
        }
        // Honor struct-level alignas: pad sizeof so arrays stride correctly.
        uint64_t effAlign = typeIsPtr
            ? 0
            : compiler->GetEffectiveAlignmentForType(typeName, elemType);
        llvm::Value* sizeVal;
        if (effAlign > 1)
        {
            uint64_t paddedSize = compiler->GetEffectiveAllocSize(elemType, effAlign);
            sizeVal = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*compiler->context), paddedSize);
        }
        else
        {
            sizeVal = compiler->GetTypeSizeBytes(elemType);
        }
        llvm::Value* count = nullptr;
        if (isArray)
        {
            count = ParseAssignmentExpression(ctx->assignmentExpression());
            count = compiler->Upconvert(count, compiler->builder->getInt64Ty());
            sizeVal = compiler->builder->CreateMul(sizeVal, count, "arraysz");
        }

        // Call operator new: class-specific -> global. When effective alignment
        // exceeds the default-new threshold (16 on x64), route to the 2-arg
        // overload `operator new(size, align)` so the allocator picks aligned
        // memory and the matching delete uses operator delete_aligned.
        constexpr uint64_t kDefaultNewAlign = 16;
        bool useAligned = effAlign > kDefaultNewAlign;
        llvm::Value* rawPtr = nullptr;
        std::string opNewName = typeName + ".operator new";
        LLVMBackend::NamedVariable szArg;
        szArg.Primary = sizeVal;
        szArg.BaseType = sizeVal->getType();
        std::vector<LLVMBackend::NamedVariable> newArgs = { szArg };
        if (useAligned)
        {
            LLVMBackend::NamedVariable alignArg;
            alignArg.Primary = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*compiler->context), effAlign);
            alignArg.BaseType = alignArg.Primary->getType();
            newArgs.push_back(alignArg);
        }
        if (!typeName.empty() && compiler->GetFunction(opNewName))
        {
            rawPtr = compiler->CreateOverloadedFunctionCall(opNewName, newArgs);
        }
        else if (compiler->GetFunction("operator new"))
        {
            rawPtr = compiler->CreateOverloadedFunctionCall("operator new", newArgs);
        }
        else
        {
            LogErrorContext(ctx, "'new' requires 'operator new' to be defined");
            return {};
        }

        llvm::Type* ptrTy = elemType->getPointerTo();
        llvm::Value* typedPtr = compiler->builder->CreateBitCast(rawPtr, ptrTy, "newptr");

        // For array new of a class type: call default constructor for each element (like C++).
        // Skip when typeIsPtr - the element is a pointer (e.g. Point*), not a struct; calling
        // the struct ctor would store sizeof(Point)=8 bytes into a sizeof(ptr)=4-byte slot on
        // Win32, corrupting the heap (on Win64 they happen to be equal so the bug is silent).
        if (isArray && count && !typeIsPtr && compiler->GetFunction(typeName))
        {
            auto* i64Ty = compiler->builder->getInt64Ty();
            auto* indexAlloca = compiler->builder->CreateAlloca(i64Ty, nullptr, "init_i");
            compiler->builder->CreateStore(compiler->builder->getInt64(0), indexAlloca);

            auto* condBB = compiler->CreateBasicBlock("new_ctor_cond");
            auto* bodyBB = compiler->CreateBasicBlock("new_ctor_body");
            auto* afterBB = compiler->CreateBasicBlock("new_ctor_after");
            compiler->builder->CreateBr(condBB);

            compiler->builder->SetInsertPoint(condBB);
            auto* idx = compiler->builder->CreateLoad(i64Ty, indexAlloca);
            auto* cmp = compiler->builder->CreateICmpSLT(idx, count);
            compiler->builder->CreateCondBr(cmp, bodyBB, afterBB);

            compiler->builder->SetInsertPoint(bodyBB);
            auto* idx2 = compiler->builder->CreateLoad(i64Ty, indexAlloca);
            auto* elemPtr = compiler->builder->CreateGEP(elemType, typedPtr, idx2, "new_elem");
            llvm::Value* structVal = compiler->CreateOverloadedFunctionCall(typeName, {});
            if (structVal)
                compiler->builder->CreateStore(structVal, elemPtr);
            auto* next = compiler->builder->CreateAdd(idx2, compiler->builder->getInt64(1));
            compiler->builder->CreateStore(next, indexAlloca);
            compiler->builder->CreateBr(condBB);

            compiler->builder->SetInsertPoint(afterBB);
        }

        // For non-array new of a class type: call constructor and store result
        if (!isArray && compiler->GetFunction(typeName))
        {
            std::vector<LLVMBackend::NamedVariable> ctorArgs;
            auto argList = ctx->argumentExpressionList();
            if (argList != nullptr)
            {
                for (auto* namedArg : argList->argumentNamedExpression())
                {
                    llvm::Value* argVal = ParseAssignmentExpression(namedArg->assignmentExpression());
                    if (!argVal) break;
                    LLVMBackend::NamedVariable argVar;
                    argVar.Primary = argVal;
                    argVar.BaseType = argVal->getType();
                    ctorArgs.push_back(argVar);
                }
            }
            llvm::Value* structVal = compiler->CreateOverloadedFunctionCall(typeName, ctorArgs);
            if (structVal)
                compiler->builder->CreateStore(structVal, typedPtr);
        }

        // Apply field initializer: new Type { field=val, ... }
        if (auto* initList = ctx->initializerList())
            EmitFieldInitializer(typedPtr, typeName, initList);

        LLVMBackend::NamedVariable result;
        result.TypeAndValue.TypeName = typeName;
        result.TypeAndValue.Pointer = true;
        result.Primary = typedPtr;
        result.BaseType = ptrTy;
        compiler->lastOwningResult = true;
        return result;
    }

    // If funcName is a method ("Type.method") or destructor ("~Type") whose owning
    // struct is registered, return the struct name. Returns "" for top-level
    // functions or unrecognized owners. Used by the delete-of-field rule to gate
    // 'delete obj->field' on "is the current function a method of obj's type."
    std::string SplitEnclosingStruct(const std::string& funcName, LLVMBackend* compiler) const
    {
        if (funcName.empty()) return "";
        if (funcName[0] == '~')
        {
            std::string typeName = funcName.substr(1);
            return compiler->IsDataStructure(typeName) ? typeName : std::string{};
        }
        auto dot = funcName.find('.');
        if (dot == std::string::npos) return "";
        std::string typeName = funcName.substr(0, dot);
        return compiler->IsDataStructure(typeName) ? typeName : std::string{};
    }

    LLVMBackend::NamedVariable ParseDeleteExpression(CFlatParser::DeleteExpressionContext* ctx)
    {
        auto* compiler = Compiler(ctx);
        bool isArray      = ctx->LeftBracket() != nullptr;
        bool hasSizeExpr  = ctx->deleteArraySize() != nullptr;
        // delete[_] ptr - free backing buffer only, no destructor calls.
        bool isRawFree    = hasSizeExpr && ctx->deleteArraySize()->expression()->getText() == "_";
        if (hasSizeExpr && ctx->deleteArraySize()->expression()->getText() == "0")
        {
            LogErrorContext(ctx, "'delete[0]' is not allowed - use 'delete[_]' to free a raw buffer without calling destructors");
            return {};
        }

        // Parse the pointer expression and determine the pointed-to struct type name.
        // getPointerElementType() is unavailable with LLVM opaque pointers; use the
        // AST-level type information from the unary expression instead.
        std::string typeName;
        bool elemIsPtr = false;         // true when deleting array of pointers (e.g. list<T*> buffer)
        llvm::Value* ptrVal = nullptr;
        llvm::Value* srcAlloca = nullptr;
        llvm::Type* srcAllocaElemType = nullptr;
        if (auto* ue = tryGetUnaryExpression(ctx->expression()))
        {
            auto namedVar = ParseUnaryExpression(ue);
            typeName  = namedVar.TypeAndValue.TypeName;
            elemIsPtr = namedVar.TypeAndValue.ElemPointer;

            // Error: deleting a borrowed (non-move) parameter directly. If the caller owns the
            // pointer, it will be freed again when the caller's scope exits - double-free.
            // Restrict to direct alloca storage (simple variable reference, not field access)
            // so 'delete param->field' is still allowed.
            if (!namedVar.CallerName.empty()
                && !namedVar.IsOwning
                && namedVar.Storage != nullptr
                && llvm::isa<llvm::AllocaInst>(namedVar.Storage)
                && compiler->IsFunctionParameter(namedVar.CallerName))
            {
                LogErrorContext(ctx, std::format(
                    "cannot delete borrowed parameter '{}' - caller may own this pointer and "
                    "will free it on scope exit. Declare the parameter 'move {}' to take ownership.",
                    namedVar.CallerName, namedVar.CallerName));
                return {};
            }

            // Error: deleting a local that aliases a borrowed parameter (e.g. via cast).
            // 'Payload* p = (Payload*)raw; delete p;' is the laundered form of the case above.
            // Exempt field accesses (storage is a GEP, not an alloca): 'delete param->field'
            // is intentionally allowed - the field's lifetime is the caller's contract, just
            // like the simple-variable check above.
            if (!namedVar.IsOwning
                && namedVar.IsBorrowed
                && !namedVar.BorrowedOrigin.empty()
                && namedVar.Storage != nullptr
                && llvm::isa<llvm::AllocaInst>(namedVar.Storage))
            {
                LogErrorContext(ctx, std::format(
                    "cannot delete '{}' - it aliases borrowed parameter '{}'. The caller may own "
                    "this pointer and will free it on scope exit. Declare the source parameter "
                    "'move {}' to take ownership.",
                    namedVar.CallerName.empty() ? namedVar.BorrowedOrigin : namedVar.CallerName,
                    namedVar.BorrowedOrigin, namedVar.BorrowedOrigin));
                return {};
            }

            // Error: deleting a struct field from outside the owning struct's own methods.
            // Encapsulation: only the owner is allowed to free its own pointer fields directly.
            // External callers must extract with 'T* p = move obj->field; delete p;' so the
            // ownership transfer is visible at the call site and the owner sees nullptr.
            if (!namedVar.OwningStructName.empty()
                && namedVar.Storage != nullptr
                && !llvm::isa<llvm::AllocaInst>(namedVar.Storage))
            {
                std::string enclosing = SplitEnclosingStruct(compiler->GetCurrentFunctionName(), compiler);
                if (enclosing != namedVar.OwningStructName)
                {
                    LogErrorContext(ctx, std::format(
                        "cannot delete field '{}.{}' from outside '{}'. Extract it first with "
                        "'T* p = move expr; delete p;' so the ownership transfer is explicit.",
                        namedVar.OwningStructName, namedVar.FieldName, namedVar.OwningStructName));
                    return {};
                }
            }

            if (namedVar.Storage)
            {
                ptrVal = compiler->CreateLoad(namedVar.Storage);
                if (llvm::isa<llvm::AllocaInst>(namedVar.Storage))
                {
                    srcAlloca = namedVar.Storage;
                    srcAllocaElemType = namedVar.BaseType;
                }
            }
            else
            {
                ptrVal = namedVar.Primary;
            }
        }
        else
        {
            ptrVal = ParseExpression(ctx->expression());
        }
        if (!ptrVal) return {};

        // Error: bare delete[] on a named struct array - the caller must supply the count.
        if (isArray && !hasSizeExpr && compiler->IsDataStructure(typeName) && !elemIsPtr)
        {
            LogErrorContext(ctx, std::format(
                "'delete[]' is not allowed for struct type '{}' - use 'delete[n] ptr' to call destructors or 'delete[_] ptr' to free the raw buffer", typeName));
            return {};
        }

        // 1. Call destructor if it exists (non-array only)
        if (!isArray && !typeName.empty())
        {
            auto structData = compiler->GetDataStructure(typeName);
            if (structData.Destructor)
                compiler->builder->CreateCall(structData.Destructor, { ptrVal });
        }

        // 1b. For delete[n]: call ~T() on each element using the caller-supplied count.
        // elemIsPtr suppresses destructor calls for pointer-element arrays (e.g. list<T*> buffer).
        llvm::Value* freeBase = ptrVal;
        if (hasSizeExpr && !isRawFree && compiler->IsDataStructure(typeName) && !elemIsPtr)
        {
            auto structData = compiler->GetDataStructure(typeName);
            if (structData.Destructor)
            {
                auto* i64Ty = compiler->builder->getInt64Ty();
                llvm::Value* arrCount = ParseExpression(ctx->deleteArraySize()->expression());
                arrCount = compiler->Upconvert(arrCount, i64Ty);

                LLVMBackend::TypeAndValue typeInfo{ .TypeName = typeName };
                llvm::Type* elemType = compiler->GetType(typeInfo);

                auto* indexAlloca = compiler->builder->CreateAlloca(i64Ty, nullptr, "del_i");
                compiler->builder->CreateStore(
                    compiler->builder->CreateSub(arrCount, compiler->builder->getInt64(1), "del_start"),
                    indexAlloca);

                auto* condBB  = compiler->CreateBasicBlock("del_dtor_cond");
                auto* bodyBB  = compiler->CreateBasicBlock("del_dtor_body");
                auto* afterBB = compiler->CreateBasicBlock("del_dtor_after");
                compiler->builder->CreateBr(condBB);

                compiler->builder->SetInsertPoint(condBB);
                auto* idx = compiler->builder->CreateLoad(i64Ty, indexAlloca);
                compiler->builder->CreateCondBr(
                    compiler->builder->CreateICmpSGE(idx, compiler->builder->getInt64(0)),
                    bodyBB, afterBB);

                compiler->builder->SetInsertPoint(bodyBB);
                auto* idx2    = compiler->builder->CreateLoad(i64Ty, indexAlloca);
                auto* elemPtr = compiler->builder->CreateGEP(elemType, ptrVal, idx2, "del_elem");
                compiler->builder->CreateCall(structData.Destructor, { elemPtr });
                compiler->builder->CreateStore(
                    compiler->builder->CreateSub(idx2, compiler->builder->getInt64(1)), indexAlloca);
                compiler->builder->CreateBr(condBB);

                compiler->builder->SetInsertPoint(afterBB);
            }
        }

        // 2. Convert free base to void*
        auto* voidPtrTy = compiler->builder->getInt8Ty()->getPointerTo();
        llvm::Value* voidPtr = compiler->builder->CreateBitCast(freeBase, voidPtrTy, "freeptr");

        // 3. Call operator delete: class-specific -> global. When the static
        // type carries effective alignment > 16 (the default-new threshold),
        // route to `operator delete_aligned` so it matches the aligned new path.
        std::string opDelName = typeName + ".operator delete";
        LLVMBackend::NamedVariable ptrArg;
        ptrArg.Primary = voidPtr;
        ptrArg.BaseType = voidPtrTy;
        uint64_t deleteEffAlign = 0;
        if (!typeName.empty() && !elemIsPtr)
        {
            LLVMBackend::TypeAndValue tv{ .TypeName = typeName };
            llvm::Type* t = compiler->GetType(tv);
            if (t != nullptr && t->isSized())
                deleteEffAlign = compiler->GetEffectiveAlignmentForType(typeName, t);
        }
        bool useAlignedDelete = deleteEffAlign > 16;
        if (!typeName.empty() && compiler->GetFunction(opDelName))
        {
            compiler->CreateOverloadedFunctionCall(opDelName, { ptrArg });
        }
        else if (useAlignedDelete && compiler->GetFunction("__delete_aligned"))
        {
            compiler->CreateOverloadedFunctionCall("__delete_aligned", { ptrArg });
        }
        else if (compiler->GetFunction("operator delete"))
        {
            compiler->CreateOverloadedFunctionCall("operator delete", { ptrArg });
        }
        else
        {
            LogErrorContext(ctx, "'delete' requires 'operator delete' to be defined");
        }

        // 4. Null the source alloca so scope-exit cleanup (IsNewAllocated) doesn't double-free.
        if (srcAlloca && srcAllocaElemType)
        {
            if (auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(srcAllocaElemType))
                compiler->builder->CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy), srcAlloca);
        }

        return {};
    }

    LLVMBackend::NamedVariable ParseMoveExpression(CFlatParser::MoveExpressionContext* ctx)
    {
        auto* compiler = Compiler(ctx);
        auto argNV = ParseUnaryExpression(ctx->unaryExpression());

        // Reject 'move param->field' (or any field whose parent traces to a borrowed
        // parameter): the caller still owns the parent struct and will see a nulled field
        // it never agreed to surrender. Require the parent parameter to be declared 'move'.
        if (!argNV.OwningStructName.empty()
            && argNV.IsBorrowed
            && !argNV.BorrowedOrigin.empty())
        {
            LogErrorContext(ctx, std::format(
                "cannot 'move' field '{}.{}' through borrowed parameter '{}'. Declare the "
                "source parameter 'move {}' to transfer ownership of its fields.",
                argNV.OwningStructName, argNV.FieldName, argNV.BorrowedOrigin, argNV.BorrowedOrigin));
            return {};
        }

        llvm::Value* ptrVal = LoadNamedVariable(argNV);

        // move on a named struct value type: capture the value, then zero the source storage
        // to leave it in a "moved-from" (default) state - enables safe delete[n] on the source.
        // Primitive value types (int, etc.) remain a no-op.
        if (!argNV.TypeAndValue.Pointer)
        {
            if (argNV.Storage && compiler->IsDataStructure(argNV.TypeAndValue.TypeName))
            {
                llvm::Type* structType = compiler->GetType(argNV.TypeAndValue);
                if (structType)
                    compiler->builder->CreateStore(
                        llvm::ConstantAggregateZero::get(structType), argNV.Storage);
                LLVMBackend::NamedVariable result;
                result.Primary      = ptrVal;       // original value captured before zeroing
                result.Storage      = nullptr;      // prevent re-load from zeroed storage
                result.BaseType     = argNV.BaseType;
                result.TypeAndValue = argNV.TypeAndValue;
                return result;
            }
            return argNV;
        }

        if (argNV.Storage == nullptr)
        {
            LogErrorContext(ctx, "'move' expression requires an addressable source (field or local).");
            return {};
        }

        // Null the source (field GEP or local alloca) to transfer ownership.
        if (auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(ptrVal->getType()))
            compiler->builder->CreateStore(llvm::ConstantPointerNull::get(ptrTy), argNV.Storage);

        // Signal ParseDeclaration to mark the target local as IsOwning.
        compiler->lastOwningResult = true;

        LLVMBackend::NamedVariable result;
        result.Primary      = ptrVal;
        result.Storage      = nullptr;
        result.BaseType     = ptrVal ? ptrVal->getType() : nullptr;
        result.TypeAndValue = argNV.TypeAndValue;
        return result;
    }

    LLVMBackend::NamedVariable ParseOperatorStringExpression(CFlatParser::OperatorStringExpressionContext* ctx)
    {
        auto* compiler = Compiler(ctx);

        // Collect arguments passed to operator string(...)
        std::vector<LLVMBackend::NamedVariable> arguments;
        if (auto* argList = ctx->argumentExpressionList())
        {
            for (auto* argExpr : argList->argumentNamedExpression())
            {
                llvm::Value* argVal = ParseAssignmentExpression(argExpr->assignmentExpression());
                if (!argVal) break;
                LLVMBackend::NamedVariable argVar;
                argVar.Primary = argVal;
                argVar.BaseType = argVal->getType();
                arguments.push_back(argVar);
            }
        }

        // Dispatch to the global "operator string" overload matching the argument types.
        auto result = compiler->CreateOverloadedFunctionCall("operator string", arguments);
        if (!result) { LogErrorContext(ctx, "'operator string' is not defined"); return {}; }

        LLVMBackend::NamedVariable ret;
        ret.Primary = result;
        ret.TypeAndValue.TypeName = "string";
        ret.TypeAndValue.IsInterface = false;
        ret.TypeAndValue.Pointer = false;
        return ret;
    }

    LLVMBackend::NamedVariable ParsePostfixExpression(CFlatParser::PostfixExpressionContext* ctx, bool lValue = false)
    {
        /*
        * postfixExpression
            : primaryExpression
            (
                '[' expression ']'
                | '(' argumentExpressionList? ')'
                | ('.' | '->') Identifier
                | '++'
                | '--'
            )*
        */

        if (auto primaryCtx = ctx->primaryExpression())
        {
            size_t prevRuleId = 0;
            size_t prevToken = 0;
            LLVMBackend::NamedVariable namedVar;
            LLVMBackend::NamedVariable structVar;
            LLVMBackend::NamedVariable interfaceVar;
            std::string primaryIdentifier;
            std::string namespaceContext;

            int functionArgCounter = 0;
            bool nullConditionalPending = false;

            for (auto parseTree : ctx->children)
            {
                if (parseTree->getTreeType() == antlr4::tree::ParseTreeType::TERMINAL)
                {
                    auto terminal = dynamic_cast<antlr4::tree::TerminalNode*>(parseTree);
                    auto tokenType = terminal->getSymbol()->getType();
                    // 'move' is now a keyword token; remap it to Identifier handling
                    // so it works as a member name (e.g. File.move(...)).
                    if (tokenType == CFlatParser::Move)
                        tokenType = CFlatParser::Identifier;
                    switch (tokenType)
                    {
                    case CFlatParser::LeftBracket:
                    case CFlatParser::RightBracket:
                    case CFlatParser::LeftParen:
                    case CFlatParser::RightParen: { prevToken = tokenType; break; }
                    case CFlatParser::Dot:
                    case CFlatParser::Arrow:
                    case CFlatParser::QuestionDot:
                    {
                        prevToken = tokenType;
                        nullConditionalPending = (tokenType == CFlatParser::QuestionDot);
                        // For any member access on a pointer to a known struct, load the pointer
                        // so subsequent field/method lookups work. '.' auto-deduces the dereference
                        // just like '->'; '?.' does the same but also arms the null-conditional check.
                        if (namedVar.TypeAndValue.Pointer
                            && !namedVar.TypeAndValue.TypeName.empty()
                            && !namedVar.TypeAndValue.IsInterface)
                        {
                            auto sd = Compiler(ctx)->GetDataStructure(namedVar.TypeAndValue.TypeName);
                            if (sd.StructType)
                            {
                                llvm::Value* ptrVal = LoadNamedVariable(namedVar);
                                structVar.Storage      = ptrVal;
                                structVar.Primary      = nullptr;
                                structVar.BaseType     = sd.StructType;
                                structVar.TypeAndValue = namedVar.TypeAndValue;
                                structVar.TypeAndValue.Pointer = false;
                                // Preserve borrow-origin across the auto-deref so 'move param->field'
                                // can detect that the parent pointer is a borrowed parameter.
                                structVar.IsBorrowed      = namedVar.IsBorrowed;
                                structVar.BorrowedOrigin  = namedVar.BorrowedOrigin;
                            }
                        }
                        else if (!namedVar.TypeAndValue.Pointer
                                 && !namedVar.TypeAndValue.TypeName.empty()
                                 && !namedVar.TypeAndValue.IsInterface
                                 && namedVar.Storage != nullptr)
                        {
                            // Embedded struct field (value, not pointer): namedVar.Storage is the GEP
                            // address of the embedded field - use it directly as the struct base so
                            // chained method calls (e.g. w.inbox.send()) pass the correct 'this'.
                            auto sd = Compiler(ctx)->GetDataStructure(namedVar.TypeAndValue.TypeName);
                            if (sd.StructType)
                            {
                                structVar.Storage      = namedVar.Storage;
                                structVar.Primary      = nullptr;
                                structVar.BaseType     = sd.StructType;
                                structVar.TypeAndValue = namedVar.TypeAndValue;
                                structVar.IsBorrowed      = namedVar.IsBorrowed;
                                structVar.BorrowedOrigin  = namedVar.BorrowedOrigin;
                            }
                        }
                        break;
                    }
                    case CFlatParser::Tilde:
                    {
                        // expr.~() - call destructor in place, no memory freed.
                        if (prevToken == CFlatParser::Dot)
                        {
                            auto* compiler = Compiler(ctx);
                            std::string typeName;
                            llvm::Value* thisPtr = nullptr;

                            if (!structVar.TypeAndValue.TypeName.empty() && structVar.Storage)
                            {
                                typeName = structVar.TypeAndValue.TypeName;
                                thisPtr  = structVar.Storage;
                            }
                            else if (!namedVar.TypeAndValue.TypeName.empty())
                            {
                                typeName = namedVar.TypeAndValue.TypeName;
                                thisPtr  = namedVar.TypeAndValue.Pointer
                                               ? LoadNamedVariable(namedVar)
                                               : namedVar.Storage;
                            }

                            if (typeName == "string")
                                compiler->EnsureStringDtorRegistered();

                            if (!typeName.empty() && thisPtr)
                            {
                                auto structData = compiler->GetDataStructure(typeName);
                                // No destructor = no-op (primitive or struct with no cleanup needed).
                                if (structData.Destructor)
                                    compiler->builder->CreateCall(structData.Destructor, { thisPtr });
                            }
                            else
                            {
                                LogErrorContext(ctx, "'.~()' requires a named struct type");
                            }
                            namedVar = {};
                            structVar = {};
                        }
                        break;
                    }
                    case CFlatParser::PlusPlus:
                    {
                        if (namedVar.Storage)
                        {
                            llvm::Type* et = nullptr;
                            if (namedVar.TypeAndValue.Pointer)
                            {
                                auto elemTV = namedVar.TypeAndValue;
                                elemTV.ElemPointer ? (elemTV.ElemPointer = false) : (elemTV.Pointer = false, elemTV.IsInterfacePointer = false);
                                et = Compiler(ctx)->GetType(elemTV);
                            }
                            PlusPlus[namedVar.Storage].first++;
                            PlusPlus[namedVar.Storage].second = et;
                        }
                        break;
                    }
                    case CFlatParser::MinusMinus:
                    {
                        if (namedVar.Storage)
                        {
                            llvm::Type* et = nullptr;
                            if (namedVar.TypeAndValue.Pointer)
                            {
                                auto elemTV = namedVar.TypeAndValue;
                                elemTV.ElemPointer ? (elemTV.ElemPointer = false) : (elemTV.Pointer = false, elemTV.IsInterfacePointer = false);
                                et = Compiler(ctx)->GetType(elemTV);
                            }
                            PlusPlus[namedVar.Storage].first--;
                            PlusPlus[namedVar.Storage].second = et;
                        }
                        break;
                    }
                    case CFlatParser::Identifier:
                    {
                        if (!namespaceContext.empty())
                        {
                            // "$global$:<alias>" sentinel: file-scoped import alias.
                            // Resolve Alias.Symbol → Symbol only if Symbol is a member of that alias.
                            bool isFileAlias = namespaceContext.starts_with("$global$:");
                            std::string memberName = terminal->getText();
                            std::string qualifiedName;
                            if (isFileAlias)
                            {
                                std::string aliasName = namespaceContext.substr(9);
                                qualifiedName = Compiler(ctx)->IsImportAliasMember(aliasName, memberName)
                                    ? memberName
                                    : namespaceContext + "." + memberName;  // will fail → error
                            }
                            else
                            {
                                qualifiedName = namespaceContext + "." + memberName;
                            }
                            if (Compiler(ctx)->IsNamespace(qualifiedName))
                            {
                                namespaceContext = Compiler(ctx)->ResolveNamespace(qualifiedName);
                                primaryIdentifier = namespaceContext;
                                namedVar = {};
                            }
                            else
                            {
                                // Qualified name (e.g. EnumName.Member) - try to resolve as a global
                                // variable (enum member) or a function. Fall back to leaving
                                // namedVar empty so later code can handle it.
                                primaryIdentifier = qualifiedName;
                                namespaceContext.clear();

                                auto globalNV = Compiler(ctx)->GetGlobalVariableNV(primaryIdentifier);
                                if (globalNV.Storage != nullptr)
                                {
                                    namedVar = globalNV;
                                }
                                else if (Compiler(ctx)->GetFunction(primaryIdentifier))
                                {
                                    namedVar.Primary = Compiler(ctx)->GetFunctionForFuncPtr(primaryIdentifier);
                                    namedVar.CallerName = primaryIdentifier;
                                }
                                else
                                {
                                    namedVar = {};
                                }
                            }
                        }
                        else if (interfaceVar.TypeAndValue.IsInterface)
                        {
                            // Method name on interface variable - just record it; dispatch at call site
                            primaryIdentifier = terminal->getText();
                            namedVar = {};
                        }
                        else if (structVar.BaseType)
                        {
                            primaryIdentifier = terminal->getText();
                            auto dataStructure = Compiler(ctx)->GetDataStructure(llvm::dyn_cast<llvm::StructType>(structVar.BaseType));

                            // Bitfield access path: side-table lookup, GEP to the storage word,
                            // emit shift+mask for the read, remember enough on the NamedVariable
                            // that write-side codegen can do a read-modify-write later.
                            const LLVMBackend::BitfieldInfo* bfHit = nullptr;
                            for (const auto& b : dataStructure.Bitfields)
                                if (b.Name == primaryIdentifier) { bfHit = &b; break; }
                            if (bfHit && !structVar.Storage)
                            {
                                LogErrorContext(ctx, std::format(
                                    "bitfield '{}' has no addressable storage in this context", primaryIdentifier));
                            }
                            if (bfHit && structVar.Storage)
                            {
                                auto* compiler = Compiler(ctx);
                                const auto& storageField = dataStructure.StructFields[bfHit->StorageFieldIndex];
                                auto* storageTy = compiler->GetType(storageField);
                                auto* storagePtr = compiler->CreateStructGEP(structVar.BaseType, structVar.Storage, bfHit->StorageFieldIndex);
                                auto* word = compiler->CreateLoad(storageTy, storagePtr);

                                unsigned w = bfHit->BitWidth;
                                unsigned off = bfHit->BitOffset;
                                unsigned storageBits = (unsigned)word->getType()->getIntegerBitWidth();
                                // Sign-aware extraction: ((word << (storageBits-w-off)) >> (storageBits-w))
                                // works for both signed (arithmetic shift) and unsigned (logical shift).
                                llvm::Value* shifted;
                                if (bfHit->IsUnsigned || bfHit->TypeName == "bool")
                                {
                                    // word >> off  &  ((1<<w)-1)
                                    auto* shr = compiler->builder->CreateLShr(word, llvm::ConstantInt::get(word->getType(), off));
                                    uint64_t mask = (w == 64) ? ~uint64_t(0) : ((uint64_t(1) << w) - 1);
                                    shifted = compiler->builder->CreateAnd(shr, llvm::ConstantInt::get(word->getType(), mask));
                                }
                                else
                                {
                                    // Signed: shift left to put the bitfield's MSB at the word MSB,
                                    // then arithmetic shift right by (storageBits-w) to sign-extend.
                                    unsigned leftShift = storageBits - w - off;
                                    auto* shl = compiler->builder->CreateShl(word, llvm::ConstantInt::get(word->getType(), leftShift));
                                    shifted = compiler->builder->CreateAShr(shl, llvm::ConstantInt::get(word->getType(), storageBits - w));
                                }

                                // Reconstruct a DeclTypeAndValue describing the bitfield as its declared type.
                                LLVMBackend::DeclTypeAndValue bfType{};
                                bfType.TypeName = bfHit->TypeName;
                                bfType.VariableName = bfHit->Name;
                                bfType.IsBitfield = true;
                                bfType.BitWidth = bfHit->BitWidth;
                                bfType.BitOffset = bfHit->BitOffset;
                                bfType.StorageFieldIndex = bfHit->StorageFieldIndex;

                                namedVar = {};
                                namedVar.Primary = shifted;
                                namedVar.BaseType = shifted->getType();
                                namedVar.Storage = nullptr;  // bitfields have no addressable storage
                                namedVar.TypeAndValue = bfType;
                                namedVar.TypeAndValue.ParentVariableName = structVar.TypeAndValue.VariableName;
                                namedVar.OwningStructName = structVar.TypeAndValue.TypeName;
                                namedVar.FieldName = primaryIdentifier;
                                namedVar.BitfieldStorage = storagePtr;
                                namedVar.BitfieldStorageType = storageTy;
                                namedVar.BitfieldOffset = bfHit->BitOffset;
                                namedVar.BitfieldWidth = bfHit->BitWidth;
                                namedVar.BitfieldUnsigned = bfHit->IsUnsigned || bfHit->TypeName == "bool";
                                continue;
                            }

                            uint32_t fieldIndex = 0;

                            for (const auto& field : dataStructure.StructFields)
                            {
                                if (field.VariableName == primaryIdentifier)
                                {
                                    break;
                                }
                                fieldIndex++;
                            }

                            if (fieldIndex < dataStructure.StructFields.size())
                            {
                                const auto& fieldType = dataStructure.StructFields[fieldIndex];

                                // Lock-set check: if this field is guarded, verify the lock is held.
                                if (!fieldType.GuardedBy.empty())
                                {
                                    std::string receiverName = structVar.TypeAndValue.VariableName;
                                    if (!receiverName.empty())
                                    {
                                        std::string requiredLock = receiverName + "." + fieldType.GuardedBy;
                                        if (currentLockSet.find(requiredLock) == currentLockSet.end())
                                        {
                                            LogErrorContext(ctx, std::format(
                                                "Field '{}' is guarded by '{}': must hold '{}' before accessing it.",
                                                primaryIdentifier, fieldType.GuardedBy, requiredLock));
                                        }
                                    }
                                }

                                if (nullConditionalPending && structVar.Storage != nullptr)
                                {
                                    // Null-conditional field access: emit a null check branch
                                    auto* fieldLLVMType = Compiler(ctx)->GetType(fieldType);
                                    auto* resultAlloca = Compiler(ctx)->CreateAlloca(fieldLLVMType);

                                    auto* nullBlock = Compiler(ctx)->CreateBasicBlock("nc_null");
                                    auto* accessBlock = Compiler(ctx)->CreateBasicBlock("nc_access");
                                    auto* resumeBlock = Compiler(ctx)->CreateBasicBlock("nc_resume");

                                    Compiler(ctx)->CreateConditionJump(structVar.Storage, accessBlock, nullBlock);
                                    // insert point is now accessBlock

                                    llvm::Value* fieldGEP;
                                    if (dataStructure.IsUnion)
                                        fieldGEP = structVar.Storage;  // union: all fields at offset 0
                                    else
                                        fieldGEP = Compiler(ctx)->CreateStructGEP(structVar.BaseType, structVar.Storage, fieldIndex);
                                    auto* fieldVal = dataStructure.IsUnion
                                        ? Compiler(ctx)->CreateLoad(fieldLLVMType, fieldGEP)
                                        : Compiler(ctx)->CreateLoad(fieldGEP);
                                    Compiler(ctx)->CreateAssignment(fieldVal, resultAlloca);
                                    Compiler(ctx)->CreateJump(resumeBlock);

                                    Compiler(ctx)->SwitchToBlock(nullBlock);
                                    Compiler(ctx)->CreateAssignment(llvm::Constant::getNullValue(fieldLLVMType), resultAlloca);
                                    Compiler(ctx)->CreateJump(resumeBlock);

                                    Compiler(ctx)->SwitchToBlock(resumeBlock);
                                    auto* result = Compiler(ctx)->CreateLoad(resultAlloca);

                                    namedVar.Storage = nullptr;
                                    namedVar.Primary = result;
                                    namedVar.BaseType = result->getType();
                                    namedVar.TypeAndValue = fieldType;
                                    namedVar.TypeAndValue.ParentVariableName = structVar.TypeAndValue.VariableName;
                                    nullConditionalPending = false;
                                }
                                else
                                {
                                    if (structVar.Storage)
                                    {
                                        auto* fieldLLVMType = Compiler(ctx)->GetType(fieldType);
                                        if (dataStructure.IsUnion)
                                        {
                                            // Union: all fields alias at offset 0. Store raw alloca pointer
                                            // and record the field type so derefLoad/derefAssign use it.
                                            namedVar.Storage = structVar.Storage;
                                            namedVar.UnionFieldType = fieldLLVMType;
                                            if (llvm::isa<llvm::ArrayType>(fieldLLVMType))
                                            {
                                                namedVar.Primary = nullptr;
                                                namedVar.BaseType = fieldLLVMType;
                                            }
                                            else
                                            {
                                                namedVar.Primary = Compiler(ctx)->CreateLoad(fieldLLVMType, namedVar.Storage);
                                                namedVar.BaseType = namedVar.Primary->getType();
                                            }
                                        }
                                        else
                                        {
                                            // Not a union field: clear any inherited UnionFieldType from a parent
                                            // union access in the chain (e.g. union.structField.subField).
                                            namedVar.UnionFieldType = nullptr;
                                            namedVar.Storage = Compiler(ctx)->CreateStructGEP(structVar.BaseType, structVar.Storage, fieldIndex);
                                            if (llvm::isa<llvm::ArrayType>(fieldLLVMType))
                                            {
                                                // Array field: keep GEP pointer; don't load the whole array
                                                namedVar.Primary = nullptr;
                                                namedVar.BaseType = fieldLLVMType;
                                            }
                                            else
                                            {
                                                namedVar.Primary = Compiler(ctx)->CreateLoad(namedVar.Storage);
                                                namedVar.BaseType = namedVar.Primary->getType();
                                            }
                                        }
                                    }
                                    else if (structVar.Primary)
                                    {
                                        namedVar.Storage = nullptr;
                                        namedVar.UnionFieldType = nullptr;
                                        // Unions with no backing storage can't reinterpret inline values.
                                        if (!dataStructure.IsUnion)
                                            namedVar.Primary = Compiler(ctx)->CreateExtractValue(structVar.Primary, fieldIndex);
                                        namedVar.BaseType = namedVar.Primary ? namedVar.Primary->getType() : nullptr;
                                    }
                                    namedVar.TypeAndValue = fieldType;
                                    namedVar.TypeAndValue.ParentVariableName = structVar.TypeAndValue.VariableName;
                                    // Mark this NamedVariable as a struct-field access so 'delete obj->field'
                                    // can reject it when invoked outside the owning struct's own methods.
                                    namedVar.OwningStructName = structVar.TypeAndValue.TypeName;
                                    namedVar.FieldName        = primaryIdentifier;
                                    // Propagate borrow-origin so 'move param->field' can detect
                                    // the parent pointer is a borrowed parameter.
                                    namedVar.IsBorrowed       = structVar.IsBorrowed;
                                    namedVar.BorrowedOrigin   = structVar.BorrowedOrigin;
                                }
                            }
                            else if (Compiler(ctx)->GetFunction(primaryIdentifier) || genericFunctionTemplates.count(primaryIdentifier))
                            {
                                // Not a field - could be a member function or an extension method template.
                                namedVar = {};
                            }
                            else
                            {
                                LogErrorContext(primaryCtx, std::format("Unknown identifier '{}'.", primaryIdentifier));
                            }
                        }
                        else if (prevToken == CFlatParser::Dot && [&]() -> bool {
                            // Named variables (alloca-backed): TypeName is reliable.
                            if (namedVar.TypeAndValue.IsFloatingPoint() >= 0) return true;
                            // Inline float expressions (e.g. (-2.5f)): TypeName is empty,
                            // but Primary holds the unloaded LLVM float value.
                            if (namedVar.Primary != nullptr
                                && namedVar.Primary->getType()->isFloatingPointTy()) return true;
                            // Integer conversion methods - matched by name so they work on any
                            // integer-typed base (named var, inline literal, call result).
                            static const std::unordered_set<std::string> intConvert = {
                                "to_i8","to_u8","to_i16","to_u16","to_i32","to_u32","to_i64","to_u64"
                            };
                            return intConvert.count(terminal->getText()) > 0;
                        }())
                        {
                            // Method name on a primitive float/double or integer conversion
                            // (e.g. f.round(), (-2.5f).abs(), x.to_i32(), x.to_u64()).
                            // Record the method name; the base value stays in namedVar.
                            // Actual dispatch happens when '()' is processed below.
                            primaryIdentifier = terminal->getText();
                        }
                        else
                        {
                            namedVar = ParseIdentifier(terminal);
                        }

                        if (namedVar.TypeAndValue.IsInterface)
                        {
                            interfaceVar = namedVar;
                            structVar = {};
                        }
                        else if (namedVar.BaseType && namedVar.BaseType->isStructTy())
                        {
                            structVar = namedVar;
                            interfaceVar = {};
                        }
                        else if (namedVar.BaseType && llvm::isa<llvm::ArrayType>(namedVar.BaseType))
                        {
                            // Fixed-array field: clear structVar so the subscript handler does not
                            // mistake the parent struct as the operator[] target.
                            structVar = {};
                        }

                        break;
                    }
                    }
                }
                else if (parseTree->getTreeType() == antlr4::tree::ParseTreeType::RULE)
                {
                    auto ruleContext = dynamic_cast<antlr4::RuleContext*>(parseTree);
                    auto ruleID = ruleContext->getRuleIndex();
                    switch (ruleID)
                    {
                    case CFlatParser::RulePrimaryExpression:
                    {
                        auto prevPrimary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(parseTree);

                        // If the primary is a generic instantiation (e.g. Box<MyInt>),
                        // map it to its mangled constructor name (e.g. Box__MyInt).
                        // Apply type substitutions for generic parameters.
                        if (prevPrimary->genericIdentifier() != nullptr && prevPrimary->genericIdentifier()->genericTypeParameters() != nullptr && prevPrimary->genericIdentifier()->Identifier() != nullptr)
                        {
                            std::string baseName = prevPrimary->genericIdentifier()->Identifier()->getText();
                            std::string mangledName = baseName;
                            std::vector<std::string> typeArgs;
                            for (auto* entry : prevPrimary->genericIdentifier()->genericTypeParameters()->typeParameterList()->typeParameterEntry())
                            {
                                std::string arg = entry->getText();
                                // Apply active type substitutions to each type argument
                                auto it = activeTypeSubstitutions.find(arg);
                                if (it != activeTypeSubstitutions.end())
                                    arg = it->second;
                                typeArgs.push_back(arg);
                                mangledName += "__" + MangleTypeArg(arg);
                            }
                            // If this is a generic function template (e.g. wrap<int>),
                            // instantiate it and record the mangled name as CallerName so
                            // ParseDeclaration can resolve the correct function pointer.
                            if (genericFunctionTemplates.count(baseName))
                            {
                                InstantiateGenericFunction(baseName, typeArgs);
                                primaryIdentifier = mangledName;
                                namedVar = {};
                                namedVar.CallerName = mangledName;
                                break;
                            }
                            primaryIdentifier = mangledName;
                            namedVar = {};
                            break;
                        }

                        primaryIdentifier = prevPrimary->getText();

                        if (prevPrimary->genericIdentifier() != nullptr && prevPrimary->genericIdentifier()->Identifier() != nullptr)
                        {
                            std::string idName = prevPrimary->genericIdentifier()->Identifier()->getText();
                            if (Compiler(ctx)->IsNamespace(idName))
                            {
                                namespaceContext = Compiler(ctx)->ResolveNamespace(idName);
                                namedVar = {};
                                structVar = {};
                            }
                            else if (Compiler(ctx)->IsDataStructure(idName)
                                     && Compiler(ctx)->GetLocalVariable(idName).Storage == nullptr
                                     && Compiler(ctx)->GetFunctionArgument(idName).GetValue() == nullptr)
                            {
                                // Type name used as qualifier for static method access: ClassName.Method()
                                // Constructor calls (ClassName()) still work because functionName = "ClassName".
                                namespaceContext = idName;
                                namedVar = {};
                                structVar = {};
                            }
                            else
                            {
                                namedVar.Primary = ParsePrimaryExpression(prevPrimary);
                                namedVar.Storage = nullptr;
                                if (namedVar.Primary == nullptr)
                                    namedVar = ParseIdentifier(prevPrimary->genericIdentifier()->Identifier());
                            }
                        }
                        else
                        {
                            namedVar.Primary = ParsePrimaryExpression(prevPrimary);
                            namedVar.Storage = nullptr;
                            // If the primary is a parenthesized cast expression, propagate its type
                            // so that chained member access (e.g. ((Struct*)ptr)->field) works.
                            if (prevPrimary->expression() != nullptr && !lastParenExprType.TypeName.empty())
                                namedVar.TypeAndValue = lastParenExprType;
                            lastParenExprType = {};
                        }

                        // If the primary was a lambda, propagate its function-pointer type.
                        if (prevPrimary->lambdaExpression() != nullptr)
                        {
                            namedVar.TypeAndValue = lastLambdaType;
                            lastLambdaType = {};
                        }

                        if (namedVar.TypeAndValue.IsInterface)
                        {
                            interfaceVar = namedVar;
                            structVar = {};
                        }
                        else if (namedVar.BaseType && namedVar.BaseType->isStructTy())
                        {
                            structVar = namedVar;
                            interfaceVar = {};
                        }
                        else if (!namedVar.TypeAndValue.TypeName.empty() && namedVar.TypeAndValue.Pointer
                                 && Compiler(ctx)->GetDataStructure(namedVar.TypeAndValue.TypeName).StructType != nullptr)
                        {
                            // Opaque pointer to a known struct (e.g. string* a) - track as structVar
                            // so member call dispatch can dereference it correctly.
                            structVar = namedVar;
                            interfaceVar = {};
                        }
                        else if (!namedVar.Storage)
                        {
                            structVar = {};
                            interfaceVar = {};
                        }

                        break;
                    }
                    case CFlatParser::RuleExpression:
                    {
                        // Bracket [] operation

                        auto expressCtx = dynamic_cast<CFlatParser::ExpressionContext*>(ruleContext);
                        auto rvalue = ParseExpression(expressCtx);

                        // If the base is a struct value with a user-defined operator[],
                        // dispatch to it (member call with 'this' as first arg).
                        if (rvalue && !namedVar.TypeAndValue.Pointer
                            && structVar.BaseType && structVar.BaseType->isStructTy()
                            && Compiler(ctx)->GetFunction("operator[]"))
                        {
                            LLVMBackend::NamedVariable thisNV = structVar;
                            thisNV.TypeAndValue.VariableName = "";
                            if (!thisNV.TypeAndValue.Pointer && thisNV.Storage == nullptr && thisNV.Primary != nullptr)
                            {
                                auto* temp = Compiler(ctx)->CreateAlloca(structVar.BaseType);
                                Compiler(ctx)->CreateAssignment(thisNV.Primary, temp);
                                thisNV.Storage = temp;
                            }

                            LLVMBackend::NamedVariable idxNV;
                            idxNV.Primary  = rvalue;
                            idxNV.BaseType = rvalue->getType();

                            auto* result = Compiler(ctx)->CreateOverloadedFunctionCall("operator[]", { thisNV, idxNV });
                            if (result)
                            {
                                namedVar.Primary  = result;
                                namedVar.Storage  = nullptr;
                                namedVar.BaseType = result->getType();
                                namedVar.TypeAndValue = Compiler(ctx)->lastCallReturnType;
                                if (namedVar.TypeAndValue.IsInterface)
                                {
                                    // operator[] returned an interface fat-ptr - expose as interfaceVar
                                    // so subsequent member accesses dispatch via vtable.
                                    interfaceVar = namedVar;
                                    structVar = {};
                                }
                                else if (result->getType()->isStructTy())
                                {
                                    if (auto* st = llvm::dyn_cast<llvm::StructType>(result->getType()))
                                        if (!st->isLiteral() && st->hasName())
                                            namedVar.TypeAndValue.TypeName = st->getName().str();
                                    structVar = namedVar;
                                    interfaceVar = {};
                                }
                                else if (!namedVar.TypeAndValue.TypeName.empty() && namedVar.TypeAndValue.Pointer
                                         && Compiler(ctx)->GetDataStructure(namedVar.TypeAndValue.TypeName).StructType != nullptr)
                                {
                                    structVar = namedVar;
                                    interfaceVar = {};
                                }
                                else
                                {
                                    structVar = {};
                                }
                                break;
                            }
                        }

                        if (!(rvalue && rvalue->getType()->isIntegerTy()))
                        {
                            LogErrorContext(expressCtx, "Expecting be an integer type.");
                        }

                        if (auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(namedVar.BaseType))
                        {
                            // Fixed-size array (char buf[N] or char* words[N]): two-index GEP {0, i}.
                            // Check BaseType first so pointer-element arrays (char*[N]) don't fall
                            // into the pointer-arithmetic branch below.
                            llvm::Value* zero = Compiler(ctx)->builder->getInt64(0);
                            namedVar.Storage = Compiler(ctx)->builder->CreateGEP(
                                arrTy, namedVar.Storage, {zero, rvalue}, "arrayelemptr");
                            namedVar.BaseType = arrTy->getElementType();
                            namedVar.TypeAndValue.ConstArraySize = 0;
                        }
                        else if (namedVar.TypeAndValue.Pointer)
                        {
                            // Indexing through a pointer (e.g. char* p; p[i]).
                            auto elementTypeAndValue = namedVar.TypeAndValue;
                            if (elementTypeAndValue.ElemPointer)
                            {
                                // Double-pointer (e.g. T* where T=Employee*): element type is T* (Employee*).
                                // Keep Pointer=true, clear ElemPointer - element is a pointer value.
                                elementTypeAndValue.ElemPointer = false;
                            }
                            else
                            {
                                elementTypeAndValue.Pointer = false;
                                // For interface arrays (T* where T=IFace), the element is a bare fat-ptr
                                // {i8*,i8*}, not a pointer-to-fat-ptr. Clear IsInterfacePointer so
                                // GetType returns {i8*,i8*} instead of {i8*,i8*}*.
                                elementTypeAndValue.IsInterfacePointer = false;
                            }
                            auto elementType = Compiler(ctx)->GetType(elementTypeAndValue);
                            auto ptrValue = LoadNamedVariable(namedVar);
                            namedVar.Storage = Compiler(ctx)->CreateGEP(elementType, ptrValue, rvalue);
                            namedVar.BaseType = elementType;
                            namedVar.TypeAndValue = elementTypeAndValue;
                        }
                        else
                        {
                            namedVar.Storage = Compiler(ctx)->CreateGEP(namedVar.BaseType, namedVar.Storage, rvalue);
                            namedVar.BaseType = namedVar.Storage->getType();
                        }
                        namedVar.Primary = nullptr;

                        if (namedVar.BaseType && namedVar.BaseType->isStructTy())
                        {
                            structVar = namedVar;
                        }
                        else if (!namedVar.Storage)
                        {
                            structVar = {};
                        }

                        break;
                    }
                    case CFlatParser::RuleGenericTypeParameters:
                    {
                        auto* genParams = dynamic_cast<CFlatParser::GenericTypeParametersContext*>(ruleContext);
                        if (!primaryIdentifier.empty() && genericFunctionTemplates.count(primaryIdentifier))
                        {
                            std::vector<std::string> typeArgs;
                            for (auto* entry : genParams->typeParameterList()->typeParameterEntry())
                            {
                                std::string arg = entry->getText();
                                auto it = activeTypeSubstitutions.find(arg);
                                if (it != activeTypeSubstitutions.end())
                                    arg = it->second;
                                typeArgs.push_back(arg);
                            }
                            std::string mangled = InstantiateGenericFunction(primaryIdentifier, typeArgs);
                            if (!mangled.empty())
                            {
                                primaryIdentifier = mangled;
                                namedVar.CallerName = mangled;  // expose mangled name for function<T> assignment
                            }
                        }
                        break;
                    }
                    case CFlatParser::RuleArgumentExpressionList:
                    {
                        // Create Function Call
                        std::string functionName = primaryIdentifier;

                        auto argumentList = ctx->argumentExpressionList();

                        // Compile-time intrinsic: annotationof(TypeName, "fieldName", "AnnotationName")
                        // Returns the annotation's argument value as a string constant, or "" if absent.
                        // For no-arg annotations, returns "1" if present or "" if absent.
                        // Usable with `if const` to branch on annotation presence.
                        if (functionName == "annotationof")
                        {
                            std::string annValue;
                            auto* compiler = Compiler(ctx);
                            if (argumentList.size() > 0)
                            {
                                auto allArgs = argumentList[0]->argumentNamedExpression();

                                auto getArgText = [&](int idx) -> std::string {
                                    if (idx >= (int)allArgs.size()) return {};
                                    std::string t = allArgs[idx]->assignmentExpression()->getText();
                                    // Strip surrounding quotes from string literal args
                                    if (t.size() >= 2 && t.front() == '"' && t.back() == '"')
                                        return t.substr(1, t.size() - 2);
                                    return t;
                                };

                                std::string typeName  = getArgText(0);
                                std::string fieldName = getArgText(1);
                                std::string annName   = getArgText(2);

                                // Resolve generic type substitutions
                                auto substIt = activeTypeSubstitutions.find(typeName);
                                if (substIt != activeTypeSubstitutions.end())
                                    typeName = substIt->second;

                                auto structData = compiler->GetDataStructure(typeName);
                                for (const auto& field : structData.StructFields)
                                {
                                    if (field.VariableName != fieldName) continue;
                                    for (const auto& ann : field.Annotations)
                                    {
                                        if (ann.Name != annName) continue;
                                        annValue = ann.Value.empty() ? "1" : ann.Value;
                                        break;
                                    }
                                    break;
                                }
                            }
                            namedVar.Primary = compiler->CreateGlobalString("annotationof", annValue);
                            namedVar.TypeAndValue.TypeName = "string";
                            break;
                        }

                        // Intrinsic: reflect(obj, visitor)
                        // Compile-time resolves obj's struct type T, synthesizes __reflect_T if needed,
                        // then emits: visitor.beginObject(""); __reflect_T(obj, visitor); visitor.endObject();
                        if (functionName == "reflect")
                        {
                            auto* compiler = Compiler(ctx);

                            // 1. Validate arity
                            if (argumentList.empty() || argumentList[0]->argumentNamedExpression().size() < 2)
                            {
                                LogErrorContext(ctx, "reflect() requires exactly two arguments: reflect(obj, visitor)");
                                break;
                            }
                            auto namedArgCtx = argumentList[0]->argumentNamedExpression();

                            // 2. Evaluate obj argument
                            auto objNV = ParseAssignmentExpressionNamed(namedArgCtx[0]->assignmentExpression());
                            std::string structTypeName = objNV.TypeAndValue.TypeName;
                            bool isPtr = objNV.TypeAndValue.Pointer;

                            // Validate struct type exists
                            auto sd = compiler->GetDataStructure(structTypeName);
                            if (!sd.StructType)
                            {
                                LogErrorContext(ctx, std::format("reflect(): first argument must be a struct type, got '{}'", structTypeName));
                                break;
                            }

                            // 3. Evaluate visitor argument
                            auto visitorNV = ParseAssignmentExpressionNamed(namedArgCtx[1]->assignmentExpression());
                            if (!visitorNV.TypeAndValue.IsInterface || visitorNV.TypeAndValue.TypeName != "IReflector")
                            {
                                LogErrorContext(ctx, "reflect(): second argument must be an IReflector interface value");
                                break;
                            }

                            // Get visitor alloca for interface method calls
                            llvm::Value* visitorAlloca = visitorNV.Storage;
                            if (!visitorAlloca)
                            {
                                // Visitor was a temporary - alloca it now
                                auto* fatTy = compiler->GetFatPtrType();
                                visitorAlloca = compiler->builder->CreateAlloca(fatTy, nullptr, "reflect_visitor_tmp");
                                llvm::Value* visitorVal = visitorNV.Primary;
                                if (!visitorVal)
                                    visitorVal = compiler->CreateLoad(visitorNV.Storage);
                                compiler->builder->CreateStore(visitorVal, visitorAlloca);
                            }

                            // 4. Inline reflection code with recursive lambda for nested structs
                            auto emptyNameNV = compiler->MakeStringLiteralNV("");
                            auto structData = compiler->GetDataStructure(structTypeName);
                            if (!structData.StructType)
                            {
                                LogErrorContext(ctx, std::format("reflect: cannot find struct '{}'", structTypeName));
                                break;
                            }
                            if (structData.IsUnion)
                            {
                                LogErrorContext(ctx, std::format("reflect is not supported on union type '{}'", structTypeName));
                                break;
                            }

                            // Define recursive lambda to emit fields for any struct
                            std::function<void(const LLVMBackend::StructData&, llvm::Value*)> emitFields;
                            emitFields = [&](const LLVMBackend::StructData& sd, llvm::Value* objPtr)
                            {
                                if (sd.IsUnion)
                                {
                                    compiler->LogError(std::format("JSON serialization is not supported on union type '{}'", sd.StructType->getName().str()));
                                    return;
                                }
                                LLVMBackend::NamedVariable intNV, boolNV, floatNV, strNV;
                                for (size_t i = 0; i < sd.StructFields.size(); i++)
                                {
                                    const auto& field = sd.StructFields[i];

                                    // Synthetic bitfield storage slots (`__bfN`) are not user-visible;
                                    // the named bitfields they pack are emitted from sd.Bitfields below.
                                    if (field.IsBitfieldStorage) continue;

                                    // Skip [Private] fields
                                    bool isPrivate = false;
                                    for (const auto& ann : field.Annotations)
                                        if (ann.Name == "Private") { isPrivate = true; break; }
                                    if (isPrivate) continue;

                                    const std::string& typeName = field.TypeName;
                                    std::string displayName = field.VariableName;
                                    for (const auto& ann : field.Annotations)
                                        if (ann.Name == "JsonName" && !ann.Value.empty()) { displayName = ann.Value; break; }
                                    auto* gep = compiler->builder->CreateStructGEP(sd.StructType, objPtr, (unsigned)i,
                                        field.VariableName + "_ptr");

                                    if ((typeName == "int" || typeName == "i8" || typeName == "i16" || typeName == "i32" || typeName == "i64"
                                         || typeName == "u8" || typeName == "u16" || typeName == "u32" || typeName == "u64")
                                        && !field.Pointer)
                                    {
                                        auto* val = compiler->builder->CreateLoad(compiler->GetType(field), gep);
                                        auto* widened = compiler->Upconvert(val, compiler->builder->getInt32Ty(), false);
                                        auto nameNV = compiler->MakeStringLiteralNV(displayName);
                                        intNV = {};
                                        intNV.Primary = widened;
                                        intNV.TypeAndValue.TypeName = "int";
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitInt", {nameNV, intNV});
                                    }
                                    else if (typeName == "string" && !field.Pointer)
                                    {
                                        auto nameNV = compiler->MakeStringLiteralNV(displayName);
                                        strNV = {};
                                        strNV.Storage = gep;
                                        strNV.TypeAndValue.TypeName = "string";
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitString", {nameNV, strNV});
                                    }
                                    else if (typeName == "bool" && !field.Pointer)
                                    {
                                        auto* val = compiler->builder->CreateLoad(compiler->GetType(field), gep);
                                        auto nameNV = compiler->MakeStringLiteralNV(displayName);
                                        boolNV = {};
                                        boolNV.Primary = val;
                                        boolNV.TypeAndValue.TypeName = "bool";
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitBool", {nameNV, boolNV});
                                    }
                                    else if ((typeName == "float" || typeName == "double") && !field.Pointer)
                                    {
                                        llvm::Value* val = compiler->builder->CreateLoad(compiler->GetType(field), gep);
                                        if (typeName == "double")
                                            val = compiler->builder->CreateFPCast(val, compiler->builder->getFloatTy());
                                        auto nameNV = compiler->MakeStringLiteralNV(displayName);
                                        floatNV = {};
                                        floatNV.Primary = val;
                                        floatNV.TypeAndValue.TypeName = "float";
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitFloat", {nameNV, floatNV});
                                    }
                                    // ── list<T> field (value type, not pointer) ──────────────
                                    // Check BEFORE nested struct to avoid treating list as a struct
                                    else if (typeName.rfind("list__", 0) == 0 && !field.Pointer)
                                    {
                                        std::string elemTypeName = typeName.substr(6); // strip "list__"
                                        auto nameNV = compiler->MakeStringLiteralNV(displayName);
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "beginArray", {nameNV});

                                        // Call count() on the list struct
                                        LLVMBackend::NamedVariable selfNV;
                                        selfNV.Storage = gep;
                                        selfNV.TypeAndValue.TypeName = typeName;
                                        auto* countVal = compiler->CreateOverloadedFunctionCall("count", {selfNV});

                                        // Loop: for (int i = 0; i < count; i++)
                                        auto* i32Ty = compiler->builder->getInt32Ty();
                                        auto* indexAlloca = compiler->builder->CreateAlloca(i32Ty, nullptr, "reflect_arr_idx");
                                        compiler->builder->CreateStore(compiler->builder->getInt32(0), indexAlloca);

                                        auto* condBB = compiler->CreateBasicBlock("reflect_arr_cond");
                                        auto* bodyBB = compiler->CreateBasicBlock("reflect_arr_body");
                                        auto* afterBB = compiler->CreateBasicBlock("reflect_arr_after");
                                        compiler->builder->CreateBr(condBB);

                                        // condition: i < count
                                        compiler->builder->SetInsertPoint(condBB);
                                        auto* idx = compiler->builder->CreateLoad(i32Ty, indexAlloca);
                                        auto* cmp = compiler->builder->CreateICmpSLT(idx, countVal);
                                        compiler->builder->CreateCondBr(cmp, bodyBB, afterBB);

                                        // loop body: get element and dispatch
                                        compiler->builder->SetInsertPoint(bodyBB);
                                        auto* idx2 = compiler->builder->CreateLoad(i32Ty, indexAlloca);
                                        LLVMBackend::NamedVariable idxNV;
                                        idxNV.Primary = idx2;
                                        idxNV.TypeAndValue.TypeName = "int";
                                        auto elemNV = compiler->CreateOverloadedFunctionCall("get", {selfNV, idxNV});
                                        auto emptyNV = compiler->MakeStringLiteralNV("");

                                        // Dispatch element by type
                                        if ((elemTypeName == "int" || elemTypeName == "i8" || elemTypeName == "i16" || elemTypeName == "i32" || elemTypeName == "i64"
                                             || elemTypeName == "u8" || elemTypeName == "u16" || elemTypeName == "u32" || elemTypeName == "u64"))
                                        {
                                            auto* widened = compiler->Upconvert(elemNV, compiler->builder->getInt32Ty(), false);
                                            LLVMBackend::NamedVariable elemIntNV;
                                            elemIntNV.Primary = widened;
                                            elemIntNV.TypeAndValue.TypeName = "int";
                                            compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitInt", {emptyNV, elemIntNV});
                                        }
                                        else if (elemTypeName == "bool")
                                        {
                                            LLVMBackend::NamedVariable elemBoolNV;
                                            elemBoolNV.Primary = elemNV;
                                            elemBoolNV.TypeAndValue.TypeName = "bool";
                                            compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitBool", {emptyNV, elemBoolNV});
                                        }
                                        else if (elemTypeName == "float" || elemTypeName == "double")
                                        {
                                            llvm::Value* val = elemNV;
                                            if (elemTypeName == "double")
                                                val = compiler->builder->CreateFPCast(val, compiler->builder->getFloatTy());
                                            LLVMBackend::NamedVariable elemFloatNV;
                                            elemFloatNV.Primary = val;
                                            elemFloatNV.TypeAndValue.TypeName = "float";
                                            compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitFloat", {emptyNV, elemFloatNV});
                                        }
                                        else if (elemTypeName == "string")
                                        {
                                            // For string, spill to alloca and set Storage
                                            LLVMBackend::TypeAndValue strTV;
                                            strTV.TypeName = "string";
                                            auto* strAlloca = compiler->builder->CreateAlloca(compiler->GetType(strTV), nullptr, "reflect_arr_elem");
                                            compiler->builder->CreateStore(elemNV, strAlloca);
                                            LLVMBackend::NamedVariable elemStrNV;
                                            elemStrNV.Storage = strAlloca;
                                            elemStrNV.TypeAndValue.TypeName = "string";
                                            compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitString", {emptyNV, elemStrNV});
                                        }
                                        else if (compiler->dataStructures.count(elemTypeName))
                                        {
                                            // Struct element - spill to alloca and recurse
                                            auto nestedData = compiler->GetDataStructure(elemTypeName);
                                            auto* elemAlloca = compiler->builder->CreateAlloca(nestedData.StructType, nullptr, "reflect_arr_elem");
                                            compiler->builder->CreateStore(elemNV, elemAlloca);
                                            compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "beginObject", {emptyNV});
                                            emitFields(nestedData, elemAlloca);
                                            compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "endObject", {});
                                        }

                                        // i++
                                        auto* nextIdx = compiler->builder->CreateAdd(idx2, compiler->builder->getInt32(1));
                                        compiler->builder->CreateStore(nextIdx, indexAlloca);
                                        compiler->builder->CreateBr(condBB);

                                        // after loop
                                        compiler->builder->SetInsertPoint(afterBB);
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "endArray", {});
                                    }
                                    // ── nested struct (value type) ────────────────────────────
                                    else if (!field.Pointer && compiler->dataStructures.count(typeName))
                                    {
                                        auto nestedData = compiler->GetDataStructure(typeName);
                                        auto nameNV = compiler->MakeStringLiteralNV(displayName);
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "beginObject", {nameNV});
                                        emitFields(nestedData, gep);
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "endObject", {});
                                    }
                                    // ── nested struct pointer ─────────────────────────────────
                                    else if (field.Pointer && compiler->dataStructures.count(typeName))
                                    {
                                        auto nameNV = compiler->MakeStringLiteralNV(displayName);
                                        auto* ptrVal = compiler->builder->CreateLoad(
                                            llvm::PointerType::getUnqual(*compiler->context), gep);
                                        auto* isNull = compiler->builder->CreateICmpEQ(ptrVal,
                                            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrVal->getType())));
                                        auto* thenBB = compiler->CreateBasicBlock("reflect_null_f");
                                        auto* elseBB = compiler->CreateBasicBlock("reflect_obj_f");
                                        auto* mergeBB = compiler->CreateBasicBlock("reflect_merge_f");
                                        compiler->builder->CreateCondBr(isNull, thenBB, elseBB);
                                        // null branch
                                        compiler->builder->SetInsertPoint(thenBB);
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitNull", {nameNV});
                                        compiler->builder->CreateBr(mergeBB);
                                        // non-null branch
                                        compiler->builder->SetInsertPoint(elseBB);
                                        auto nestedData = compiler->GetDataStructure(typeName);
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "beginObject", {nameNV});
                                        emitFields(nestedData, ptrVal);
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "endObject", {});
                                        compiler->builder->CreateBr(mergeBB);
                                        // merge
                                        compiler->builder->SetInsertPoint(mergeBB);
                                    }
                                }

                                // Named bitfields live in a side-table, not StructFields.
                                // Extract each one from its storage word with a sign-aware shift+mask.
                                for (const auto& bf : sd.Bitfields)
                                {
                                    bool bfPrivate = false;
                                    for (const auto& ann : bf.Annotations)
                                        if (ann.Name == "Private") { bfPrivate = true; break; }
                                    if (bfPrivate) continue;

                                    std::string displayName = bf.Name;
                                    for (const auto& ann : bf.Annotations)
                                        if (ann.Name == "JsonName" && !ann.Value.empty()) { displayName = ann.Value; break; }

                                    const auto& storageField = sd.StructFields[bf.StorageFieldIndex];
                                    auto* storageTy = compiler->GetType(storageField);
                                    auto* storagePtr = compiler->builder->CreateStructGEP(sd.StructType, objPtr,
                                        bf.StorageFieldIndex, bf.Name + "_bf_ptr");
                                    auto* word = compiler->builder->CreateLoad(storageTy, storagePtr);

                                    unsigned w = bf.BitWidth;
                                    unsigned off = bf.BitOffset;
                                    unsigned storageBits = (unsigned)word->getType()->getIntegerBitWidth();

                                    llvm::Value* extracted;
                                    if (bf.IsUnsigned || bf.TypeName == "bool")
                                    {
                                        auto* shr = compiler->builder->CreateLShr(word,
                                            llvm::ConstantInt::get(word->getType(), off));
                                        uint64_t mask = (w == 64) ? ~uint64_t(0) : ((uint64_t(1) << w) - 1);
                                        extracted = compiler->builder->CreateAnd(shr,
                                            llvm::ConstantInt::get(word->getType(), mask));
                                    }
                                    else
                                    {
                                        unsigned leftShift = storageBits - w - off;
                                        auto* shl = compiler->builder->CreateShl(word,
                                            llvm::ConstantInt::get(word->getType(), leftShift));
                                        extracted = compiler->builder->CreateAShr(shl,
                                            llvm::ConstantInt::get(word->getType(), storageBits - w));
                                    }

                                    auto nameNV = compiler->MakeStringLiteralNV(displayName);

                                    if (bf.TypeName == "bool")
                                    {
                                        auto* asBool = compiler->builder->CreateICmpNE(extracted,
                                            llvm::ConstantInt::get(extracted->getType(), 0));
                                        LLVMBackend::NamedVariable bNV;
                                        bNV.Primary = asBool;
                                        bNV.TypeAndValue.TypeName = "bool";
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitBool", {nameNV, bNV});
                                    }
                                    else
                                    {
                                        auto* widened = compiler->Upconvert(extracted,
                                            compiler->builder->getInt32Ty(), bf.IsUnsigned);
                                        LLVMBackend::NamedVariable iNV;
                                        iNV.Primary = widened;
                                        iNV.TypeAndValue.TypeName = "int";
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitInt", {nameNV, iNV});
                                    }
                                }
                            };

                            llvm::Value* objPtr = nullptr;
                            if (isPtr)
                            {
                                // Null-check for pointer obj
                                objPtr = objNV.Primary;
                                if (!objPtr)
                                    objPtr = compiler->CreateLoad(objNV.Storage);
                                auto ptrType = llvm::cast<llvm::PointerType>(objPtr->getType());
                                auto* isNull = compiler->builder->CreateICmpEQ(objPtr,
                                    llvm::ConstantPointerNull::get(ptrType));

                                auto* thenBB = compiler->CreateBasicBlock("reflect_null");
                                auto* elseBB = compiler->CreateBasicBlock("reflect_obj");
                                auto* mergeBB = compiler->CreateBasicBlock("reflect_merge");
                                compiler->builder->CreateCondBr(isNull, thenBB, elseBB);

                                // null branch: visitNull("")
                                compiler->builder->SetInsertPoint(thenBB);
                                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitNull", {emptyNameNV});
                                compiler->builder->CreateBr(mergeBB);

                                // non-null branch: beginObject + reflect fields + endObject
                                compiler->builder->SetInsertPoint(elseBB);
                                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "beginObject", {emptyNameNV});
                                emitFields(structData, objPtr);
                                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "endObject", {});
                                compiler->builder->CreateBr(mergeBB);

                                compiler->builder->SetInsertPoint(mergeBB);
                            }
                            else
                            {
                                // Value type: emit reflection code directly
                                objPtr = objNV.Storage;
                                if (!objPtr)
                                {
                                    LogErrorContext(ctx, "reflect(): cannot take address of temporary struct value");
                                    break;
                                }

                                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "beginObject", {emptyNameNV});
                                emitFields(structData, objPtr);
                                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "endObject", {});
                            }

                            // reflect() returns void
                            namedVar = {};
                            break;
                        }

                        // Compile-time intrinsic: reflect_set(obj, src)
                        // Symmetric dual of reflect(). Walks obj's struct fields at compile time,
                        // calls src.getXxx(fieldName) for each, and stores the result back into obj.
                        // src must be an IJSON interface value. Respects [Private] and [JsonName].
                        if (functionName == "reflect_set")
                        {
                            auto* compiler = Compiler(ctx);

                            // 1. Validate arity
                            if (argumentList.empty() || argumentList[0]->argumentNamedExpression().size() < 2)
                            {
                                LogErrorContext(ctx, "reflect_set() requires exactly two arguments: reflect_set(obj, src)");
                                break;
                            }
                            auto namedArgCtx = argumentList[0]->argumentNamedExpression();

                            // 2. Evaluate obj argument
                            auto objNV = ParseAssignmentExpressionNamed(namedArgCtx[0]->assignmentExpression());
                            std::string structTypeName = objNV.TypeAndValue.TypeName;
                            bool isPtr = objNV.TypeAndValue.Pointer;

                            auto sd = compiler->GetDataStructure(structTypeName);
                            if (!sd.StructType)
                            {
                                LogErrorContext(ctx, std::format("reflect_set(): first argument must be a struct type, got '{}'", structTypeName));
                                break;
                            }

                            // 3. Evaluate src argument - must be IJSON
                            auto srcNV = ParseAssignmentExpressionNamed(namedArgCtx[1]->assignmentExpression());
                            if (!srcNV.TypeAndValue.IsInterface || srcNV.TypeAndValue.TypeName != "IJSON")
                            {
                                LogErrorContext(ctx, "reflect_set(): second argument must be an IJSON interface value");
                                break;
                            }

                            // Ensure src is in an alloca for interface method dispatch
                            llvm::Value* srcAlloca = srcNV.Storage;
                            if (!srcAlloca)
                            {
                                auto* fatTy = compiler->GetFatPtrType();
                                srcAlloca = compiler->builder->CreateAlloca(fatTy, nullptr, "reflect_set_src");
                                llvm::Value* srcVal = srcNV.Primary ? srcNV.Primary : compiler->CreateLoad(srcNV.Storage);
                                compiler->builder->CreateStore(srcVal, srcAlloca);
                            }

                            // 4. Recursive lambda: populate fields of any struct from an IJSON alloca
                            std::function<void(const LLVMBackend::StructData&, llvm::Value*, llvm::Value*)> emitFieldSets;
                            emitFieldSets = [&](const LLVMBackend::StructData& sd, llvm::Value* objPtr, llvm::Value* srcA)
                            {
                                if (sd.IsUnion)
                                {
                                    compiler->LogError(std::format("JSON deserialization is not supported on union type '{}'", sd.StructType->getName().str()));
                                    return;
                                }
                                auto* fatTy = compiler->GetFatPtrType();

                                for (size_t i = 0; i < sd.StructFields.size(); i++)
                                {
                                    const auto& field = sd.StructFields[i];

                                    // Skip [Private] fields
                                    bool isPrivate = false;
                                    for (const auto& ann : field.Annotations)
                                        if (ann.Name == "Private") { isPrivate = true; break; }
                                    if (isPrivate) continue;

                                    const std::string& typeName = field.TypeName;
                                    std::string displayName = field.VariableName;
                                    for (const auto& ann : field.Annotations)
                                        if (ann.Name == "JsonName" && !ann.Value.empty()) { displayName = ann.Value; break; }

                                    auto* gep = compiler->builder->CreateStructGEP(sd.StructType, objPtr, (unsigned)i,
                                        field.VariableName + "_ptr");
                                    auto nameNV = compiler->MakeStringLiteralNV(displayName);

                                    // ── int / sized integer ──────────────────────────────────
                                    if ((typeName == "int" || typeName == "i8" || typeName == "i16" || typeName == "i32" || typeName == "i64"
                                         || typeName == "u8" || typeName == "u16" || typeName == "u32" || typeName == "u64")
                                        && !field.Pointer)
                                    {
                                        auto* intVal = compiler->CallInterfaceMethod(srcA, "IJSON", "getInt", {nameNV});
                                        auto* narrowed = compiler->Upconvert(intVal, compiler->GetType(field), false);
                                        compiler->builder->CreateStore(narrowed, gep);
                                    }
                                    // ── bool ─────────────────────────────────────────────────
                                    else if (typeName == "bool" && !field.Pointer)
                                    {
                                        auto* boolVal = compiler->CallInterfaceMethod(srcA, "IJSON", "getBool", {nameNV});
                                        compiler->builder->CreateStore(boolVal, gep);
                                    }
                                    // ── float / double ────────────────────────────────────────
                                    else if ((typeName == "float" || typeName == "double") && !field.Pointer)
                                    {
                                        auto* fVal = compiler->CallInterfaceMethod(srcA, "IJSON", "getFloat", {nameNV});
                                        if (typeName == "double")
                                            fVal = compiler->builder->CreateFPCast(fVal, compiler->builder->getDoubleTy());
                                        compiler->builder->CreateStore(fVal, gep);
                                    }
                                    // ── string ───────────────────────────────────────────────
                                    else if (typeName == "string" && !field.Pointer)
                                    {
                                        auto* strVal = compiler->CallInterfaceMethod(srcA, "IJSON", "getString", {nameNV});
                                        compiler->builder->CreateStore(strVal, gep);
                                    }
                                    // ── list<T> (value type) ──────────────────────────────────
                                    else if (typeName.rfind("list__", 0) == 0 && !field.Pointer)
                                    {
                                        std::string elemTypeName = typeName.substr(6);

                                        // arr = src.getArray(name) → alloca fat ptr
                                        auto* arrVal = compiler->CallInterfaceMethod(srcA, "IJSON", "getArray", {nameNV});
                                        auto* arrAlloca = compiler->builder->CreateAlloca(fatTy, nullptr, "reflect_set_arr");
                                        compiler->builder->CreateStore(arrVal, arrAlloca);

                                        // count = arr.count()
                                        auto* countVal = compiler->CallInterfaceMethod(arrAlloca, "IJSONArray", "count", {});

                                        // Loop i = 0..count
                                        auto* i32Ty = compiler->builder->getInt32Ty();
                                        auto* idxAlloca = compiler->builder->CreateAlloca(i32Ty, nullptr, "reflect_set_idx");
                                        compiler->builder->CreateStore(compiler->builder->getInt32(0), idxAlloca);

                                        auto* condBB = compiler->CreateBasicBlock("rset_arr_cond");
                                        auto* bodyBB = compiler->CreateBasicBlock("rset_arr_body");
                                        auto* afterBB = compiler->CreateBasicBlock("rset_arr_after");
                                        compiler->builder->CreateBr(condBB);

                                        compiler->builder->SetInsertPoint(condBB);
                                        auto* idx = compiler->builder->CreateLoad(i32Ty, idxAlloca);
                                        compiler->builder->CreateCondBr(
                                            compiler->builder->CreateICmpSLT(idx, countVal), bodyBB, afterBB);

                                        compiler->builder->SetInsertPoint(bodyBB);
                                        auto* idx2 = compiler->builder->CreateLoad(i32Ty, idxAlloca);
                                        LLVMBackend::NamedVariable idxNV;
                                        idxNV.Primary = idx2;
                                        idxNV.TypeAndValue.TypeName = "int";

                                        // list self NV for add()
                                        LLVMBackend::NamedVariable listNV;
                                        listNV.Storage = gep;
                                        listNV.TypeAndValue.TypeName = typeName;

                                        if (elemTypeName == "int" || elemTypeName == "i32")
                                        {
                                            auto* v = compiler->CallInterfaceMethod(arrAlloca, "IJSONArray", "getInt", {idxNV});
                                            LLVMBackend::NamedVariable elemNV;
                                            elemNV.Primary = v;
                                            elemNV.TypeAndValue.TypeName = "int";
                                            compiler->CreateOverloadedFunctionCall("add", {listNV, elemNV});
                                        }
                                        else if (elemTypeName == "bool")
                                        {
                                            auto* v = compiler->CallInterfaceMethod(arrAlloca, "IJSONArray", "getBool", {idxNV});
                                            LLVMBackend::NamedVariable elemNV;
                                            elemNV.Primary = v;
                                            elemNV.TypeAndValue.TypeName = "bool";
                                            compiler->CreateOverloadedFunctionCall("add", {listNV, elemNV});
                                        }
                                        else if (elemTypeName == "float" || elemTypeName == "double")
                                        {
                                            auto* v = compiler->CallInterfaceMethod(arrAlloca, "IJSONArray", "getFloat", {idxNV});
                                            LLVMBackend::NamedVariable elemNV;
                                            elemNV.Primary = v;
                                            elemNV.TypeAndValue.TypeName = "float";
                                            compiler->CreateOverloadedFunctionCall("add", {listNV, elemNV});
                                        }
                                        else if (elemTypeName == "string")
                                        {
                                            auto* v = compiler->CallInterfaceMethod(arrAlloca, "IJSONArray", "getString", {idxNV});
                                            LLVMBackend::TypeAndValue strTV;
                                            strTV.TypeName = "string";
                                            auto* strAlloca = compiler->builder->CreateAlloca(compiler->GetType(strTV), nullptr, "rset_arr_str");
                                            compiler->builder->CreateStore(v, strAlloca);
                                            LLVMBackend::NamedVariable elemNV;
                                            elemNV.Storage = strAlloca;
                                            elemNV.TypeAndValue.TypeName = "string";
                                            compiler->CreateOverloadedFunctionCall("add", {listNV, elemNV});
                                        }
                                        else if (compiler->dataStructures.count(elemTypeName))
                                        {
                                            auto* subVal = compiler->CallInterfaceMethod(arrAlloca, "IJSONArray", "getObject", {idxNV});
                                            auto* subAlloca = compiler->builder->CreateAlloca(fatTy, nullptr, "rset_arr_sub");
                                            compiler->builder->CreateStore(subVal, subAlloca);

                                            auto nestedData = compiler->GetDataStructure(elemTypeName);
                                            auto* elemAlloca = compiler->builder->CreateAlloca(nestedData.StructType, nullptr, "rset_arr_elem");
                                            compiler->builder->CreateStore(
                                                llvm::Constant::getNullValue(nestedData.StructType), elemAlloca);
                                            emitFieldSets(nestedData, elemAlloca, subAlloca);

                                            LLVMBackend::NamedVariable elemNV;
                                            elemNV.Storage = elemAlloca;
                                            elemNV.TypeAndValue.TypeName = elemTypeName;
                                            compiler->CreateOverloadedFunctionCall("add", {listNV, elemNV});
                                        }

                                        // i++
                                        compiler->builder->CreateStore(
                                            compiler->builder->CreateAdd(idx2, compiler->builder->getInt32(1)), idxAlloca);
                                        compiler->builder->CreateBr(condBB);
                                        compiler->builder->SetInsertPoint(afterBB);
                                    }
                                    // ── nested struct (value) ─────────────────────────────────
                                    else if (!field.Pointer && compiler->dataStructures.count(typeName))
                                    {
                                        auto* subVal = compiler->CallInterfaceMethod(srcA, "IJSON", "getObject", {nameNV});
                                        auto* subAlloca = compiler->builder->CreateAlloca(fatTy, nullptr, "reflect_set_sub");
                                        compiler->builder->CreateStore(subVal, subAlloca);
                                        auto nestedData = compiler->GetDataStructure(typeName);
                                        emitFieldSets(nestedData, gep, subAlloca);
                                    }
                                    // ── nested struct pointer ─────────────────────────────────
                                    else if (field.Pointer && compiler->dataStructures.count(typeName))
                                    {
                                        auto hasfieldNV = nameNV;
                                        auto* hasVal = compiler->CallInterfaceMethod(srcA, "IJSON", "hasField", {hasfieldNV});
                                        auto* thenBB = compiler->CreateBasicBlock("rset_ptr_then");
                                        auto* mergeBB = compiler->CreateBasicBlock("rset_ptr_merge");
                                        compiler->builder->CreateCondBr(hasVal, thenBB, mergeBB);

                                        compiler->builder->SetInsertPoint(thenBB);
                                        auto* subVal = compiler->CallInterfaceMethod(srcA, "IJSON", "getObject", {nameNV});
                                        auto* subAlloca = compiler->builder->CreateAlloca(fatTy, nullptr, "reflect_set_subp");
                                        compiler->builder->CreateStore(subVal, subAlloca);

                                        auto nestedData = compiler->GetDataStructure(typeName);
                                        auto* newObj = compiler->builder->CreateCall(
                                            compiler->module->getFunction("__alloc_" + typeName) ?
                                                compiler->module->getFunction("__alloc_" + typeName) : nullptr,
                                            {});
                                        // Simpler: just zero-init an alloca and store pointer
                                        auto* ptrAlloca = compiler->builder->CreateAlloca(nestedData.StructType, nullptr, "rset_ptr_obj");
                                        compiler->builder->CreateStore(llvm::Constant::getNullValue(nestedData.StructType), ptrAlloca);
                                        emitFieldSets(nestedData, ptrAlloca, subAlloca);
                                        compiler->builder->CreateStore(ptrAlloca, gep);
                                        compiler->builder->CreateBr(mergeBB);

                                        compiler->builder->SetInsertPoint(mergeBB);
                                    }
                                }
                            };

                            // 5. Get obj pointer
                            llvm::Value* objPtr = nullptr;
                            if (isPtr)
                                objPtr = objNV.Primary ? objNV.Primary : compiler->CreateLoad(objNV.Storage);
                            else
                            {
                                objPtr = objNV.Storage;
                                if (!objPtr)
                                {
                                    LogErrorContext(ctx, "reflect_set(): cannot take address of temporary struct value");
                                    break;
                                }
                            }

                            emitFieldSets(sd, objPtr, srcAlloca);

                            namedVar = {};
                            break;
                        }

                        // Compile-time intrinsic: is_pointer(T) - returns 1 if the type parameter T
                        // resolves to a pointer type in the current generic instantiation, 0 otherwise.
                        // Useful with `if const` to branch on pointer vs value element types.
                        if (functionName == "is_pointer")
                        {
                            bool isPtr = false;
                            if (argumentList.size() > 0)
                            {
                                auto namedArgCtx = argumentList[functionArgCounter]->argumentNamedExpression();
                                if (!namedArgCtx.empty())
                                {
                                    std::string argText = namedArgCtx[0]->assignmentExpression()->getText();
                                    auto substIt = activeTypeSubstitutions.find(argText);
                                    if (substIt != activeTypeSubstitutions.end())
                                    {
                                        const std::string& resolved = substIt->second;
                                        isPtr = !resolved.empty() && resolved.back() == '*';
                                    }
                                }
                            }
                            namedVar.Primary = llvm::ConstantInt::get(
                                llvm::Type::getInt1Ty(*Compiler(ctx)->context), isPtr ? 1 : 0);
                            namedVar.TypeAndValue.TypeName = "int";
                            break;
                        }

                        // Compile-time intrinsic: is_primitive(T) - returns 1 if T resolves to a primitive
                        // type (integer, float, bool, void), 0 otherwise. Use with `if const` to branch
                        // on primitive vs struct element types in generic data structures.
                        if (functionName == "is_primitive")
                        {
                            static const std::unordered_set<std::string> kPrimitiveTypes = {
                                "bool", "void",
                                "char", "i8", "i16", "i32", "i64",
                                "u8", "u16", "u32", "u64",
                                "short", "int", "long",
                                "float", "double",
                            };
                            bool isPrim = false;
                            if (argumentList.size() > 0)
                            {
                                auto namedArgCtx = argumentList[functionArgCounter]->argumentNamedExpression();
                                if (!namedArgCtx.empty())
                                {
                                    std::string argText = namedArgCtx[0]->assignmentExpression()->getText();
                                    auto substIt = activeTypeSubstitutions.find(argText);
                                    if (substIt != activeTypeSubstitutions.end())
                                        isPrim = kPrimitiveTypes.count(substIt->second) > 0;
                                }
                            }
                            namedVar.Primary = llvm::ConstantInt::get(
                                llvm::Type::getInt1Ty(*Compiler(ctx)->context), isPrim ? 1 : 0);
                            namedVar.TypeAndValue.TypeName = "int";
                            break;
                        }

                        // Compile-time intrinsic: is_string(T) - returns 1 if T resolves to the
                        // built-in `string` value type, 0 otherwise. Use with `if const` to deep-copy
                        // string elements in generic containers (string owns a heap buffer, so a
                        // shallow element copy would alias the buffer and double-free).
                        if (functionName == "is_string")
                        {
                            bool isStr = false;
                            if (argumentList.size() > 0)
                            {
                                auto namedArgCtx = argumentList[functionArgCounter]->argumentNamedExpression();
                                if (!namedArgCtx.empty())
                                {
                                    std::string argText = namedArgCtx[0]->assignmentExpression()->getText();
                                    auto substIt = activeTypeSubstitutions.find(argText);
                                    if (substIt != activeTypeSubstitutions.end())
                                        isStr = (substIt->second == "string");
                                }
                            }
                            namedVar.Primary = llvm::ConstantInt::get(
                                llvm::Type::getInt1Ty(*Compiler(ctx)->context), isStr ? 1 : 0);
                            namedVar.TypeAndValue.TypeName = "int";
                            break;
                        }

                        // Handle va_start / va_end - pass the va_list alloca address to the LLVM intrinsic.
                        if (functionName == "va_start" || functionName == "va_end")
                        {
                            if (argumentList.size() > 0)
                            {
                                auto namedArgCtx = argumentList[functionArgCounter]->argumentNamedExpression();
                                if (!namedArgCtx.empty())
                                {
                                    std::string varName = namedArgCtx[0]->assignmentExpression()->getText();
                                    auto vaVar = Compiler(ctx)->GetLocalVariable(varName);
                                    if (!vaVar.Storage) vaVar = Compiler(ctx)->GetFunctionArgument(varName);
                                    if (vaVar.Storage)
                                    {
                                        if (functionName == "va_start")
                                            Compiler(ctx)->CreateVaStart(vaVar.Storage);
                                        else
                                            Compiler(ctx)->CreateVaEnd(vaVar.Storage);
                                    }
                                }
                            }
                            namedVar = {};
                            break;
                        }

                        // Check if this is a function pointer variable - emit an indirect call.
                        if (namedVar.TypeAndValue.IsFunctionPointer)
                        {
                            llvm::Value* funcPtr = nullptr;
                            if (namedVar.Storage != nullptr)
                                funcPtr = Compiler(ctx)->CreateLoad(namedVar.Storage);
                            else if (namedVar.Primary != nullptr)
                                funcPtr = namedVar.Primary;

                            if (funcPtr != nullptr)
                            {
                                std::vector<llvm::Value*> callArgs;
                                std::vector<LLVMBackend::NamedVariable> argNVs;
                                if (argumentList.size() > 0)
                                {
                                    auto namedArgCtx = argumentList[functionArgCounter]->argumentNamedExpression();
                                    for (const auto& namedArgument : namedArgCtx)
                                    {
                                        auto argNV = this->ParseAssignmentExpressionNamed(namedArgument->assignmentExpression());
                                        auto argValue = argNV.Primary ? argNV.Primary : LoadNamedVariable(argNV);
                                        if (argValue) callArgs.push_back(argValue);
                                        argNVs.push_back(argNV);

                                        // Compile-time use-after-move check: arg is a moved variable.
                                        if (argNV.IsMoved && argNV.IdentifierLine > 0)
                                        {
                                            // (Same diagnostic shape as direct-call site; conservative — only fires when source variable was already moved.)
                                            Compiler(ctx)->LogError(std::format("use of moved variable '{}'", argNV.CallerName));
                                        }
                                    }
                                }
                                // Bond/move ownership-mismatch checks against the funcptr's per-param flags.
                                size_t pcount = std::min(argNVs.size(), namedVar.TypeAndValue.FuncPtrParams.size());
                                for (size_t i = 0; i < pcount; i++)
                                {
                                    if (namedVar.TypeAndValue.FuncPtrParams[i].IsMove && argNVs[i].IsBonded)
                                        Compiler(ctx)->LogError("cannot pass bonded value to 'move' parameter of function pointer - bonded values cannot be transferred out of their source's scope");
                                }
                                auto result = Compiler(ctx)->CreateIndirectCall(namedVar.TypeAndValue, funcPtr, callArgs);
                                // Null caller storage and mark as moved for params declared 'move' on the funcptr type.
                                for (size_t i = 0; i < pcount; i++)
                                {
                                    if (!namedVar.TypeAndValue.FuncPtrParams[i].IsMove) continue;
                                    auto& argNV = argNVs[i];
                                    if (argNV.Storage != nullptr && argNV.TypeAndValue.Pointer)
                                    {
                                        if (auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(argNV.BaseType))
                                            Compiler(ctx)->builder->CreateStore(llvm::ConstantPointerNull::get(ptrTy), argNV.Storage);
                                    }
                                    if (!argNV.CallerName.empty())
                                        Compiler(ctx)->MarkVariableMoved(argNV.CallerName);
                                }
                                namedVar.Primary = result;
                                namedVar.Storage = nullptr;
                                namedVar.BaseType = result ? result->getType() : nullptr;
                                namedVar.TypeAndValue = Compiler(ctx)->lastCallReturnType;
                                functionArgCounter++;
                                break;
                            }
                        }

                        // Check if this is a return-block function - inline it at the call site.
                        // A 'return' inside the block returns from the caller function.
                        if (const auto* rb = Compiler(ctx)->GetReturnBlock(functionName))
                        {
                            Compiler(ctx)->InitializeBlock(nullptr, true);

                            // Determine if the first param is an implicit 'this'
                            size_t paramOffset = (!rb->Params.empty() &&
                                rb->Params[0].VariableName.size() >= 2 &&
                                rb->Params[0].VariableName.substr(rb->Params[0].VariableName.size() - 2) == "__") ? 1 : 0;

                            // Bind 'this' if present
                            if (paramOffset > 0)
                            {
                                LLVMBackend::NamedVariable thisVar;
                                if (structVar.BaseType)
                                {
                                    thisVar = structVar;
                                    thisVar.TypeAndValue.VariableName = "";
                                }
                                else
                                {
                                    thisVar = Compiler(ctx)->GetCurrentMemberThis(functionName);
                                }
                                if (thisVar.Storage != nullptr)
                                {
                                    const auto& thisParam = rb->Params[0];
                                    LLVMBackend::TypeAndValue tv;
                                    tv.TypeName = thisParam.TypeName;
                                    tv.VariableName = thisParam.VariableName;
                                    tv.Pointer = thisParam.Pointer;
                                    auto* alloca = Compiler(ctx)->CreateLocalVariable(tv);
                                    // thisVar.Storage may be a promoted-param alloca holding a pointer;
                                    // load through it to get the actual pointer value to bind.
                                    llvm::Value* thisVal = thisVar.Storage;
                                    if (thisVar.TypeAndValue.Pointer
                                        && llvm::isa<llvm::AllocaInst>(thisVal))
                                        thisVal = Compiler(ctx)->CreateLoad(thisVal);
                                    Compiler(ctx)->CreateAssignment(thisVal, alloca);
                                }
                            }

                            // Parse and bind explicit parameters to local variables
                            if (argumentList.size() > 0)
                            {
                                auto namedArgCtx = argumentList[functionArgCounter]->argumentNamedExpression();
                                for (size_t i = 0; i < namedArgCtx.size() && (i + paramOffset) < rb->Params.size(); ++i)
                                {
                                    auto argValue = this->ParseAssignmentExpression(namedArgCtx[i]->assignmentExpression());
                                    const auto& param = rb->Params[i + paramOffset];
                                    LLVMBackend::TypeAndValue tv;
                                    tv.TypeName = param.TypeName;
                                    tv.VariableName = param.VariableName;
                                    tv.Pointer = param.Pointer;
                                    if (tv.Pointer)
                                    {
                                        // Pointer params: store value directly as Primary so GetValue()
                                        // returns the pointer itself rather than the alloca address.
                                        Compiler(ctx)->RegisterPrimaryVariable(tv, argValue);
                                    }
                                    else
                                    {
                                        auto* alloca = Compiler(ctx)->CreateLocalVariable(tv);
                                        Compiler(ctx)->CreateAssignment(argValue, alloca);
                                    }
                                }
                            }

                            if (auto* blockItems = rb->Body->blockItemList())
                                ParseBlockItemList(blockItems);

                            Compiler(ctx)->CreateBlockBreak(nullptr, true);

                            // The block's 'return' already terminated the caller's basic block.
                            // Return an empty namedVar - callers must tolerate null after a terminator.
                            namedVar = {};
                        }
                        else if (interfaceVar.TypeAndValue.IsInterface)
                        {
                            // Collect extra call arguments
                            std::vector<LLVMBackend::NamedVariable> extraArgs;
                            if (argumentList.size() > 0)
                            {
                                auto namedArgCtx = argumentList[functionArgCounter]->argumentNamedExpression();
                                for (const auto& namedArgument : namedArgCtx)
                                {
                                    auto argNV = this->ParseAssignmentExpressionNamed(namedArgument->assignmentExpression());
                                    auto argValue = argNV.Primary ? argNV.Primary : LoadNamedVariable(argNV);
                                    if (!argValue) break;
                                    LLVMBackend::NamedVariable argVar;
                                    argVar.Primary = argValue;
                                    argVar.BaseType = argValue->getType();
                                    argVar.Storage = argNV.Storage;
                                    argVar.IsOwning = argNV.IsOwning;
                                    argVar.IsOwningString = argNV.IsOwningString;
                                    argVar.TypeAndValue.Pointer = argNV.TypeAndValue.Pointer;
                                    argVar.CallerName = argNV.CallerName;
                                    argVar.TypeAndValue.IsInterface = argNV.TypeAndValue.IsInterface;
                                    argVar.IsBonded = argNV.IsBonded;
                                    argVar.BondedSources = argNV.BondedSources;

                                    // Extract struct name if this is a struct type
                                    if (auto* st = llvm::dyn_cast<llvm::StructType>(argValue->getType()))
                                    {
                                        auto structName = st->getName().str();
                                        if (!structName.empty())
                                            argVar.TypeAndValue.TypeName = structName;
                                    }

                                    // Propagate struct TypeName for pointer args so struct*->interface upcast works
                                    if (argVar.TypeAndValue.TypeName.empty() && argNV.TypeAndValue.Pointer
                                        && !argNV.TypeAndValue.TypeName.empty()
                                        && Compiler(ctx)->IsDataStructure(argNV.TypeAndValue.TypeName))
                                    {
                                        argVar.TypeAndValue.TypeName = argNV.TypeAndValue.TypeName;
                                    }

                                    extraArgs.emplace_back(argVar);
                                }
                            }

                            if (Compiler(ctx)->HasInterfaceMethod(interfaceVar.TypeAndValue.TypeName, primaryIdentifier))
                            {
                                // Interface method dispatch via vtable.
                                // Ensure we have a {i8*,i8*}* pointer (alloca address).
                                // If the interface value was produced inline (Primary set, no Storage),
                                // spill it into a temp alloca first.
                                llvm::Value* ifacePtr = interfaceVar.Storage;
                                if (ifacePtr == nullptr && interfaceVar.Primary != nullptr)
                                {
                                    auto fatTy = Compiler(ctx)->GetFatPtrType();
                                    ifacePtr = Compiler(ctx)->CreateAlloca(fatTy);
                                    Compiler(ctx)->CreateAssignment(interfaceVar.Primary, ifacePtr);
                                }
                                namedVar.Primary = Compiler(ctx)->CallInterfaceMethod(
                                    ifacePtr,
                                    interfaceVar.TypeAndValue.TypeName,
                                    primaryIdentifier,
                                    extraArgs
                                );
                                namedVar.Storage = nullptr;
                                namedVar.BaseType = namedVar.Primary ? namedVar.Primary->getType() : nullptr;
                                interfaceVar = {};
                                structVar = {};
                            }
                            else
                            {
                                // Extension method: find a standalone or generic function and
                                // pass the interface value as the first argument.
                                std::string extFuncName;
                                if (genericFunctionTemplates.count(primaryIdentifier))
                                    extFuncName = InferAndInstantiateGenericFunction(primaryIdentifier, interfaceVar.TypeAndValue.TypeName);
                                if (extFuncName.empty())
                                    extFuncName = primaryIdentifier;

                                std::vector<LLVMBackend::NamedVariable> allArgs;
                                LLVMBackend::NamedVariable ifaceArg = interfaceVar;
                                ifaceArg.TypeAndValue.VariableName = "";
                                allArgs.push_back(ifaceArg);
                                for (const auto& e : extraArgs)
                                    allArgs.push_back(e);

                                namedVar.Primary = Compiler(ctx)->CreateOverloadedFunctionCall(extFuncName, allArgs);
                                namedVar.Storage = nullptr;
                                namedVar.BaseType = namedVar.Primary ? namedVar.Primary->getType() : nullptr;
                                interfaceVar = {};
                                structVar = {};
                            }
                        }
                        else
                        {
                            // Intercept primitive method calls:
                            //   float/double: f.round(), (-2.5f).abs(), etc.
                            //   integer:      x.to_i32(), n.to_u64(), etc.
                            if (!structVar.BaseType && !primaryIdentifier.empty())
                            {
                                llvm::Value* primVal = namedVar.Storage
                                    ? Compiler(ctx)->CreateLoad(namedVar.Storage)
                                    : namedVar.Primary;

                                if (primVal != nullptr && primVal->getType()->isFloatingPointTy())
                                {
                                    auto* result = Compiler(ctx)->CreateFloatIntrinsic(primaryIdentifier, primVal);
                                    if (result)
                                    {
                                        namedVar.Primary  = result;
                                        namedVar.Storage  = nullptr;
                                        namedVar.BaseType = result->getType();
                                        structVar = {};
                                        break;
                                    }
                                }
                                else if (primVal != nullptr && primVal->getType()->isIntegerTy())
                                {
                                    auto* result = Compiler(ctx)->CreateIntegerConvert(primaryIdentifier, primVal);
                                    if (result)
                                    {
                                        namedVar.Primary  = result;
                                        namedVar.Storage  = nullptr;
                                        namedVar.BaseType = result->getType();
                                        // Strip the "to_" prefix to get the CFlat type name (e.g. "i32", "u64").
                                        namedVar.TypeAndValue = {};
                                        namedVar.TypeAndValue.TypeName = primaryIdentifier.substr(3);
                                        structVar = {};
                                        break;
                                    }
                                }
                            }

                            std::vector<LLVMBackend::NamedVariable> arguments;
                            if (structVar.BaseType)
                            {
                                // Use-after-move check for the method receiver.
                                // LoadNamedVariable is not called for receivers, so check IsMoved here.
                                if (structVar.IsMoved && structVar.IdentifierLine > 0)
                                {
                                    Compiler(ctx)->currentLine = structVar.IdentifierLine;
                                    Compiler(ctx)->currentColumn = structVar.IdentifierColumn;
                                    Compiler(ctx)->LogError(std::format("use of moved variable '{}'", structVar.CallerName));
                                }
                                LLVMBackend::NamedVariable argumentNamedVar = structVar; // Copy;
                                argumentNamedVar.TypeAndValue.VariableName = "";
                                arguments.push_back(argumentNamedVar);
                            }
                            else
                            {
                                // Bare call inside a member function - inject 'this' automatically
                                // if the callee is a method of the same struct.
                                auto thisVar = Compiler(ctx)->GetCurrentMemberThis(functionName);
                                if (thisVar.Storage != nullptr)
                                    arguments.push_back(thisVar);
                            }

                            if (argumentList.size() > 0)
                            {
                                auto namedArgCtx = argumentList[functionArgCounter]->argumentNamedExpression();

                                // Look up the function signature to set lambdaExpectedType for lambda arguments.
                                const LLVMBackend::FunctionSymbol* funcSym = nullptr;
                                {
                                    auto it = Compiler(ctx)->functionTable.find(functionName);
                                    if (it != Compiler(ctx)->functionTable.end() && !it->second.empty())
                                        funcSym = &it->second.front();
                                }
                                size_t paramOffset = arguments.empty() ? 0 : 1; // offset for implicit 'this'

                                for (size_t argIdx = 0; argIdx < namedArgCtx.size(); ++argIdx)
                                {
                                    const auto& namedArgument = namedArgCtx[argIdx];

                                    // '...' in call position: forward this function's variadic args as a va_list.
                                    if (namedArgument->Ellipsis())
                                    {
                                        if (!currentFunctionIsVariadic)
                                        {
                                            LogErrorContext(ctx, "'...' forwarding can only be used inside a variadic function");
                                            break;
                                        }
                                        if (!Compiler(ctx)->autoVaListAlloca)
                                        {
                                            LLVMBackend::TypeAndValue vaTv;
                                            vaTv.TypeName = "va_list";
                                            vaTv.VariableName = "__va_forward";
                                            Compiler(ctx)->autoVaListAlloca = Compiler(ctx)->CreateLocalVariable(vaTv);
                                            Compiler(ctx)->CreateVaStart(Compiler(ctx)->autoVaListAlloca);
                                        }
                                        llvm::Value* vaValue = Compiler(ctx)->CreateLoad(Compiler(ctx)->autoVaListAlloca);
                                        LLVMBackend::NamedVariable argVar;
                                        argVar.Primary = vaValue;
                                        argVar.BaseType = vaValue->getType();
                                        argVar.TypeAndValue.TypeName = "va_list";
                                        arguments.emplace_back(argVar);
                                        continue;
                                    }

                                    // Field initializer argument: { field=val, ... } or paramName: { field=val, ... }
                                    if (namedArgument->initializerList())
                                    {
                                        auto* argNameToken = namedArgument->Identifier();
                                        std::string namedParam = argNameToken ? argNameToken->getText() : "";
                                        int effectiveIdx = namedParam.empty() ? (int)(argIdx + paramOffset) : -1;
                                        std::string structType = ResolveInitializerArgType(ctx, functionName, effectiveIdx, namedParam);
                                        if (!structType.empty())
                                        {
                                            LLVMBackend::DeclTypeAndValue paramType;
                                            paramType.TypeName = structType;
                                            llvm::Value* defaultVal = GenerateDefaultValue(paramType);
                                            if (defaultVal)
                                            {
                                                auto* alloca = Compiler(ctx)->CreateAlloca(defaultVal->getType());
                                                Compiler(ctx)->CreateAssignment(defaultVal, alloca);
                                                EmitFieldInitializer(alloca, structType, namedArgument->initializerList());
                                                llvm::Value* loaded = Compiler(ctx)->CreateLoad(alloca);
                                                LLVMBackend::NamedVariable argVar;
                                                argVar.Primary = loaded;
                                                argVar.BaseType = loaded->getType();
                                                argVar.TypeAndValue.TypeName = structType;
                                                argVar.TypeAndValue.VariableName = namedParam;
                                                arguments.emplace_back(argVar);
                                            }
                                        }
                                        continue;
                                    }

                                    // Set expected type when function expects a function-pointer at this position.
                                    // If the parameter is lock(this), also seed lambdaLockThisReceiver so the
                                    // lambda body gets currentLockSet seeded with the call-site receiver guard.
                                    lambdaExpectedType = {};
                                    lambdaLockThisReceiver = {};
                                    if (funcSym && (argIdx + paramOffset) < funcSym->Parameters.size())
                                    {
                                        const auto& paramTv = funcSym->Parameters[argIdx + paramOffset];
                                        if (paramTv.IsFunctionPointer)
                                        {
                                            lambdaExpectedType = paramTv;
                                            // If the parameter is declared lock(this), build the qualified
                                            // guard name (e.g. "d.ready") to seed currentLockSet in the lambda body.
                                            if (paramTv.LockThis)
                                            {
                                                const std::string& parent = structVar.TypeAndValue.ParentVariableName;
                                                const std::string& field  = structVar.TypeAndValue.VariableName;
                                                if (!parent.empty() && !field.empty())
                                                    lambdaLockThisReceiver = parent + "." + field;
                                                else if (!field.empty())
                                                    lambdaLockThisReceiver = field;
                                            }
                                        }
                                    }
                                    auto argName = namedArgument->Identifier();
                                    auto argNV = this->ParseAssignmentExpressionNamed(namedArgument->assignmentExpression());
                                    // Load from storage if Primary isn't populated (simple variable reference)
                                    auto argValue = argNV.Primary ? argNV.Primary : LoadNamedVariable(argNV);
                                    lambdaExpectedType = {};
                                    if (!argValue) break; // caller's block was terminated (e.g. return-block inline)
                                    LLVMBackend::NamedVariable argVar;

                                    if (argName)
                                        argVar.TypeAndValue.VariableName = argName->getText();
                                    argVar.Primary = argValue;
                                    argVar.BaseType = argValue->getType();
                                    // Propagate caller variable name for compile-time move tracking.
                                    argVar.CallerName = argNV.CallerName;
                                    // Propagate storage and ownership so move-param zeroing works at the call site.
                                    argVar.Storage = argNV.Storage;
                                    argVar.IsOwning = argNV.IsOwning;
                                    argVar.IsOwningString = argNV.IsOwningString;
                                    argVar.TypeAndValue.Pointer = argNV.TypeAndValue.Pointer;
                                    // Propagate bond info so bond-to-move checks work at the call site.
                                    argVar.IsBonded = argNV.IsBonded;
                                    argVar.BondedSources = argNV.BondedSources;

                                    // Preserve unsigned-integer TypeName so Upconvert can choose ZExt over SExt.
                                    if (argNV.TypeAndValue.IsUnsignedInteger() != -1)
                                        argVar.TypeAndValue.TypeName = argNV.TypeAndValue.TypeName;

                                    // Propagate interface type so overload resolution matches IFoo->IFoo parameters.
                                    if (argNV.TypeAndValue.IsInterface)
                                    {
                                        argVar.TypeAndValue.IsInterface = true;
                                        argVar.TypeAndValue.TypeName = argNV.TypeAndValue.TypeName;
                                    }

                                    // Extract struct name if this is a struct type
                                    if (argVar.TypeAndValue.TypeName.empty())
                                    {
                                        if (auto* st = llvm::dyn_cast<llvm::StructType>(argValue->getType()))
                                        {
                                            auto structName = st->getName().str();
                                            if (!structName.empty())
                                                argVar.TypeAndValue.TypeName = structName;
                                        }
                                    }

                                    // Propagate struct TypeName for pointer args (e.g. move expressions, new)
                                    // so struct*->interface* upcast matching works in ComputeOverloadFunction.
                                    // Only propagate for known struct types - primitive TypeNames (char, bool,
                                    // int, …) must stay empty so LLVM-type comparison handles them correctly.
                                    if (argVar.TypeAndValue.TypeName.empty() && argNV.TypeAndValue.Pointer
                                        && !argNV.TypeAndValue.TypeName.empty()
                                        && Compiler(ctx)->IsDataStructure(argNV.TypeAndValue.TypeName))
                                    {
                                        argVar.TypeAndValue.TypeName = argNV.TypeAndValue.TypeName;
                                    }

                                    // Propagate function-pointer type for lambda arguments
                                    if (lastLambdaType.IsFunctionPointer && argValue != nullptr)
                                    {
                                        argVar.TypeAndValue = lastLambdaType;
                                        lastLambdaType = {};
                                    }

                                    arguments.emplace_back(argVar);
                                }
                            }

                            if (nullConditionalPending && structVar.Storage != nullptr)
                            {
                                // Null-conditional method call: gate the call on a null check
                                // Resolve generic extension method if needed
                                std::string resolvedFuncName = functionName;
                                if (!Compiler(ctx)->GetFunction(functionName) && genericFunctionTemplates.count(functionName))
                                {
                                    std::string structTypeName = structVar.TypeAndValue.TypeName;
                                    if (structTypeName.empty() && structVar.BaseType)
                                    {
                                        if (auto* st = llvm::dyn_cast<llvm::StructType>(structVar.BaseType))
                                            structTypeName = st->getName().str();
                                    }
                                    for (const auto& iface : Compiler(ctx)->GetStructInterfaces(structTypeName))
                                    {
                                        auto inst = InferAndInstantiateGenericFunction(functionName, iface);
                                        if (!inst.empty()) { resolvedFuncName = inst; break; }
                                    }
                                }
                                auto* retType = Compiler(ctx)->GetFunctionReturnType(resolvedFuncName);
                                bool  hasResult = retType && !retType->isVoidTy();
                                auto* resultAlloca = hasResult ? Compiler(ctx)->CreateAlloca(retType) : nullptr;

                                auto* nullBlock = Compiler(ctx)->CreateBasicBlock("nc_null");
                                auto* accessBlock = Compiler(ctx)->CreateBasicBlock("nc_access");
                                auto* resumeBlock = Compiler(ctx)->CreateBasicBlock("nc_resume");

                                Compiler(ctx)->CreateConditionJump(structVar.Storage, accessBlock, nullBlock);
                                // insert point is now accessBlock

                                namedVar.Primary = Compiler(ctx)->CreateOverloadedFunctionCall(resolvedFuncName, arguments);
                                {
                                    std::string rcvr = structVar.TypeAndValue.VariableName;
                                    if (rcvr.empty() && !compilerLLVM->lastCallParameterNames.empty())
                                    {
                                        const auto& fp = compilerLLVM->lastCallParameterNames[0];
                                        if (fp.size() >= 2 && fp.substr(fp.size() - 2) == "__")
                                            rcvr = "this";
                                    }
                                    CheckCallSiteLocks(ctx, rcvr, arguments);
                                }
                                namedVar.Storage = nullptr;
                                namedVar.BaseType = namedVar.Primary ? namedVar.Primary->getType() : nullptr;

                                if (hasResult && namedVar.Primary)
                                {
                                    Compiler(ctx)->CreateAssignment(namedVar.Primary, resultAlloca);
                                    Compiler(ctx)->CreateJump(resumeBlock);

                                    Compiler(ctx)->SwitchToBlock(nullBlock);
                                    Compiler(ctx)->CreateAssignment(llvm::Constant::getNullValue(retType), resultAlloca);
                                    Compiler(ctx)->CreateJump(resumeBlock);

                                    Compiler(ctx)->SwitchToBlock(resumeBlock);
                                    auto* result = Compiler(ctx)->CreateLoad(resultAlloca);
                                    namedVar.Primary = result;
                                    namedVar.BaseType = result->getType();
                                }
                                else
                                {
                                    Compiler(ctx)->CreateJump(resumeBlock);
                                    Compiler(ctx)->SwitchToBlock(nullBlock);
                                    Compiler(ctx)->CreateJump(resumeBlock);
                                    Compiler(ctx)->SwitchToBlock(resumeBlock);
                                    namedVar = {};
                                }

                                nullConditionalPending = false;
                            }
                            else
                            {
                                // Try generic extension method instantiation if this is a
                                // generic function template and the struct implements a matching interface.
                                std::string resolvedFuncName = functionName;
                                if (!Compiler(primaryCtx)->GetFunction(functionName) && genericFunctionTemplates.count(functionName))
                                {
                                    std::string structTypeName = structVar.TypeAndValue.TypeName;
                                    if (structTypeName.empty() && structVar.BaseType)
                                    {
                                        if (auto* st = llvm::dyn_cast<llvm::StructType>(structVar.BaseType))
                                            structTypeName = st->getName().str();
                                    }
                                    for (const auto& iface : Compiler(primaryCtx)->GetStructInterfaces(structTypeName))
                                    {
                                        auto inst = InferAndInstantiateGenericFunction(functionName, iface);
                                        if (!inst.empty()) { resolvedFuncName = inst; break; }
                                    }
                                    // If interface-based inference failed, try to infer from argument types.
                                    if (resolvedFuncName == functionName)
                                    {
                                        auto inst = TryInferAndInstantiateFromArgs(functionName, arguments);
                                        if (!inst.empty()) resolvedFuncName = inst;
                                    }
                                }
                                namedVar.Primary = Compiler(primaryCtx)->CreateOverloadedFunctionCall(resolvedFuncName, arguments);
                                {
                                    std::string rcvr = structVar.TypeAndValue.VariableName;
                                    if (rcvr.empty() && !compilerLLVM->lastCallParameterNames.empty())
                                    {
                                        const auto& fp = compilerLLVM->lastCallParameterNames[0];
                                        if (fp.size() >= 2 && fp.substr(fp.size() - 2) == "__")
                                            rcvr = "this";
                                    }
                                    CheckCallSiteLocks(primaryCtx, rcvr, arguments);
                                }
                                namedVar.Storage = nullptr;
                                namedVar.BaseType = namedVar.Primary ? namedVar.Primary->getType() : nullptr;
                                // Populate TypeAndValue from the resolved overload's return type
                                // so that subsequent member access (->field) can resolve the struct.
                                if (namedVar.Primary)
                                    namedVar.TypeAndValue = Compiler(primaryCtx)->lastCallReturnType;
                            }

                            if (namedVar.TypeAndValue.IsInterface)
                            {
                                interfaceVar = namedVar;
                                structVar = {};
                            }
                            else if (namedVar.BaseType && namedVar.BaseType->isStructTy())
                            {
                                structVar = namedVar;
                                interfaceVar = {};
                            }
                            else if (!namedVar.TypeAndValue.TypeName.empty() && namedVar.TypeAndValue.Pointer
                                     && Compiler(primaryCtx)->GetDataStructure(namedVar.TypeAndValue.TypeName).StructType != nullptr)
                            {
                                structVar = namedVar;
                                interfaceVar = {};
                            }
                        }

                        functionArgCounter++;
                        break;
                    }

                    default: { LogErrorContext(ctx, std::format("Unexpected token '{}' in postfix expression.", parseTree->getText())); return {}; }
                    }
                }
            }

            return namedVar;
        }

        LogErrorContext(ctx, "Postfix expression has no primary expression.");
        return {};
    }

    // Walk down single-child rule nodes to find a UnaryExpressionContext.
    // Returns nullptr if the path branches or never reaches a unaryExpression.
    CFlatParser::UnaryExpressionContext* tryGetUnaryExpression(antlr4::RuleContext* ctx)
    {
        if (ctx->getRuleIndex() == CFlatParser::RuleUnaryExpression)
            return dynamic_cast<CFlatParser::UnaryExpressionContext*>(ctx);

        antlr4::RuleContext* singleRuleChild = nullptr;
        for (auto* child : ctx->children)
        {
            if (child->getTreeType() == antlr4::tree::ParseTreeType::RULE)
            {
                if (singleRuleChild != nullptr)
                    return nullptr; // multiple rule children - complex expression
                singleRuleChild = dynamic_cast<antlr4::RuleContext*>(child);
            }
        }
        return singleRuleChild ? tryGetUnaryExpression(singleRuleChild) : nullptr;
    }

    // Returns true if rawText (the full StringLiteral token text including quotes)
    // contains at least one unescaped '{' that starts an interpolation expression.
    // '{{' is an escaped literal '{' and does NOT count as interpolation.
    bool HasInterpolation(const std::string& rawText)
    {
        bool inEscape = false;
        for (size_t i = 1; i + 1 < rawText.size(); i++) // skip opening/closing "
        {
            char c = rawText[i];
            if (inEscape) { inEscape = false; continue; }
            if (c == '\\') { inEscape = true; continue; }
            if (c == '{')
            {
                // {{ is an escaped literal '{', not an interpolation start.
                if (i + 1 < rawText.size() - 1 && rawText[i + 1] == '{')
                {
                    i++; // skip second '{'
                    continue;
                }
                return true;
            }
        }
        return false;
    }

    // Parses a format string literal with {expr} interpolation.
    // Splits the literal into alternating plain-text and expression segments,
    // coerces each expression segment to string via operator string,
    // stacks all (ptr, len) pairs on the stack and calls __strconcat.
    llvm::Value* ParseFormatString(CFlatParser::PrimaryExpressionContext* ctx, const std::string& rawText)
    {
        auto* compiler = Compiler(ctx);
        compiler->EnsureStrConcatRegistered();

        // Collect segment data: each entry is {i8* ptr, i32 len} stored in alloca arrays
        struct Segment { llvm::Value* ptr; llvm::Value* len; };
        std::vector<Segment> segments;

        auto* i8Ty  = compiler->builder->getInt8Ty();
        auto* i32Ty = compiler->builder->getInt32Ty();

        // Walk rawText between the outer quotes, splitting on unescaped { ... }
        // rawText format:  "...{expr}..."  (quotes included)
        size_t i = 1; // skip opening "
        size_t end = rawText.size() - 1; // stop before closing "
        std::string litAccum;

        auto flushLiteral = [&]()
        {
            if (litAccum.empty()) return;
            // Re-encode as a quoted string so ProcessRawText can decode escapes
            std::string quoted = "\"" + litAccum + "\"";
            std::string text = ProcessRawText(quoted);
            auto* gv  = compiler->CreateGlobalString("fmtlit", text);
            auto* len = compiler->builder->getInt32((int32_t)text.size());
            segments.push_back({ gv, len });
            litAccum.clear();
        };

        bool inEscape = false;
        while (i < end)
        {
            char c = rawText[i];
            if (inEscape)
            {
                litAccum += '\\';
                litAccum += c;
                inEscape = false;
                i++;
                continue;
            }
            if (c == '\\')
            {
                inEscape = true;
                i++;
                continue;
            }
            if (c == '{')
            {
                // {{ is a literal '{', not an interpolation start.
                if (i + 1 < end && rawText[i + 1] == '{')
                {
                    litAccum += '{';
                    i += 2;
                    continue;
                }

                // Find matching '}'
                size_t exprStart = i + 1;
                int depth = 1;
                size_t j = exprStart;
                while (j < end && depth > 0)
                {
                    if (rawText[j] == '{') depth++;
                    else if (rawText[j] == '}') depth--;
                    if (depth > 0) j++;
                }
                if (depth > 0)
                {
                    // No matching '}' within this string literal - treat '{' as a literal character.
                    litAccum += '{';
                    i++;
                    continue;
                }
                std::string exprText = rawText.substr(exprStart, j - exprStart);
                i = j + 1; // skip past '}'

                // Empty or non-expression content (e.g. JSON {"key": "value"}) - keep as literal.
                if (exprText.empty() || exprText[0] == '\\' || exprText[0] == '"')
                {
                    litAccum += '{';
                    litAccum += exprText;
                    litAccum += '}';
                    continue;
                }

                flushLiteral();

                // Re-parse the expression text
                antlr4::ANTLRInputStream exprInput(exprText);
                CFlatLexer exprLexer(&exprInput);
                antlr4::CommonTokenStream exprTokens(&exprLexer);
                CFlatParser exprParser(&exprTokens);
                auto* exprCtx = exprParser.assignmentExpression();

                auto nv = ParseAssignmentExpressionNamed(exprCtx);

                llvm::Value* ptr = nullptr;
                llvm::Value* len = nullptr;

                bool isString = nv.TypeAndValue.TypeName == "string";

                if (isString)
                {
                    // Already a string struct - extract _ptr (field 0) and _len (field 1)
                    llvm::Value* strVal = nv.Primary ? nv.Primary : compiler->CreateLoad(nv.Storage);
                    ptr = compiler->builder->CreateExtractValue(strVal, { 0u });
                    len = compiler->builder->CreateExtractValue(strVal, { 1u });
                }
                else
                {
                    // Call operator string to convert to string struct
                    LLVMBackend::NamedVariable arg = nv;
                    arg.TypeAndValue.VariableName = "";
                    auto* strVal = compiler->CreateOverloadedFunctionCall("operator string", { arg });
                    if (!strVal)
                    {
                        compiler->LogError("no operator string for expression in format string: " + exprText);
                        return nullptr;
                    }
                    ptr = compiler->builder->CreateExtractValue(strVal, { 0u });
                    len = compiler->builder->CreateExtractValue(strVal, { 1u });
                }

                segments.push_back({ ptr, len });
                continue;
            }
            // }} is a literal '}', not special on its own, but symmetric with {{.
            if (c == '}' && i + 1 < end && rawText[i + 1] == '}')
            {
                litAccum += '}';
                i += 2;
                continue;
            }
            litAccum += c;
            i++;
        }
        flushLiteral();

        if (segments.empty())
        {
            // Degenerate: no content at all - return empty string struct
            auto* gv = compiler->CreateGlobalString("fmtempty", "");
            return compiler->WrapStringLiteralAsString(gv);
        }

        int count = (int)segments.size();
        auto* i32ArrTy = llvm::ArrayType::get(i32Ty, count);
        auto* ptrArrTy = llvm::ArrayType::get(i8Ty->getPointerTo(), count);

        auto* ptrArr = compiler->builder->CreateAlloca(ptrArrTy, nullptr, "fmtptrs");
        auto* lenArr = compiler->builder->CreateAlloca(i32ArrTy, nullptr, "fmtlens");

        for (int k = 0; k < count; k++)
        {
            auto* ptrGep = compiler->builder->CreateConstInBoundsGEP2_32(ptrArrTy, ptrArr, 0, k);
            compiler->builder->CreateStore(segments[k].ptr, ptrGep);
            auto* lenGep = compiler->builder->CreateConstInBoundsGEP2_32(i32ArrTy, lenArr, 0, k);
            compiler->builder->CreateStore(segments[k].len, lenGep);
        }

        auto* ptrBase = compiler->builder->CreateConstInBoundsGEP2_32(ptrArrTy, ptrArr, 0, 0);
        auto* lenBase = compiler->builder->CreateConstInBoundsGEP2_32(i32ArrTy, lenArr, 0, 0);

        LLVMBackend::NamedVariable nvPtrs, nvLens, nvCount;
        nvPtrs.Primary  = ptrBase;
        nvPtrs.TypeAndValue = { "i8", "ptrs", true, false };
        nvLens.Primary  = lenBase;
        nvLens.TypeAndValue = { "i32", "lens", true, false };
        nvCount.Primary = compiler->builder->getInt32(count);
        nvCount.TypeAndValue = { "i32", "count", false, false };

        return compiler->CreateOverloadedFunctionCall("__strconcat", { nvPtrs, nvLens, nvCount });
    }

    struct CaptureInfo
    {
        std::string Name;
        LLVMBackend::TypeAndValue TV;
        bool ByReference;       // true for non-pointer struct types (capture by reference)
        llvm::Value* OuterStorage;
    };

    // Walk the lambda body AST and collect variables captured from the enclosing function scope.
    std::vector<CaptureInfo> CollectLambdaCaptures(
        antlr4::ParserRuleContext* bodyCtx,
        const std::set<std::string>& lambdaParamNames,
        LLVMBackend* compiler)
    {
        std::set<std::string> seenNames;
        std::vector<CaptureInfo> captures;

        std::function<void(antlr4::tree::ParseTree*)> walk = [&](antlr4::tree::ParseTree* node)
        {
            if (!node) return;

            // Stop at nested lambdas - they capture their own closures separately.
            if (dynamic_cast<CFlatParser::LambdaExpressionContext*>(node))
                return;

            // primaryExpression::genericIdentifier is the only AST node representing a
            // standalone identifier (not a member name after '.' or a named-arg label).
            if (auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node))
            {
                if (auto* gi = primary->genericIdentifier())
                {
                    std::string name = gi->Identifier()->getText();
                    if (!seenNames.count(name)
                        && !lambdaParamNames.count(name)
                        && !compiler->globalNamedVariable.count(name)
                        && !compiler->functionTable.count(name)
                        && !compiler->dataStructures.count(name))
                    {
                        for (const auto& frame : std::ranges::reverse_view(compiler->stackNamedVariable))
                        {
                            auto localIt = frame.namedVariable.find(name);
                            if (localIt != frame.namedVariable.end())
                            {
                                const auto& nv = localIt->second;
                                if (nv.Storage)
                                {
                                    seenNames.insert(name);
                                    CaptureInfo ci;
                                    ci.Name = name;
                                    ci.TV   = nv.TypeAndValue;
                                    ci.OuterStorage = nv.Storage;
                                    // string is a value type ({i8*,i32}): its methods take string by value,
                                    // so it must be value-captured like primitives to avoid pointer mismatch.
                                    ci.ByReference  = !ci.TV.Pointer
                                        && !ci.TV.IsFunctionPointer
                                        && !ci.TV.IsPrimitive()
                                        && ci.TV.TypeName != "string"
                                        && compiler->dataStructures.count(ci.TV.TypeName);
                                    captures.push_back(ci);
                                }
                                break;
                            }
                            auto argIt = frame.functionArgument.find(name);
                            if (argIt != frame.functionArgument.end())
                            {
                                const auto& nv = argIt->second;
                                if (nv.Storage)
                                {
                                    seenNames.insert(name);
                                    CaptureInfo ci;
                                    ci.Name = name;
                                    ci.TV   = nv.TypeAndValue;
                                    ci.OuterStorage = nv.Storage;
                                    ci.ByReference  = !ci.TV.Pointer
                                        && !ci.TV.IsFunctionPointer
                                        && !ci.TV.IsPrimitive()
                                        && ci.TV.TypeName != "string"
                                        && compiler->dataStructures.count(ci.TV.TypeName);
                                    captures.push_back(ci);
                                }
                                break;
                            }
                        }
                    }
                }
            }

            for (size_t i = 0; i < node->children.size(); i++)
                walk(node->children[i]);
        };

        if (bodyCtx) walk(bodyCtx);
        return captures;
    }

    LLVMBackend::NamedVariable ParseLambdaExpression(CFlatParser::LambdaExpressionContext* ctx)
    {
        auto* compiler = Compiler(ctx);

        // Parse lambda parameter list
        std::vector<LLVMBackend::DeclTypeAndValue> params;
        if (auto* paramList = ctx->lambdaParamList())
        {
            for (auto* param : paramList->lambdaParam())
            {
                LLVMBackend::DeclTypeAndValue p;
                p.TypeName = compiler->ResolveTypeAlias(param->typeSpecifier()->getText());
                p.Pointer = param->pointer() != nullptr;
                p.IsInterface = compiler->IsInterfaceType(p.TypeName);
                if (p.IsInterface)
                    p.IsInterfacePointer = param->pointer() != nullptr;
                p.VariableName = param->Identifier()->getText();
                params.push_back(p);
            }
        }

        // Return type from lambdaExpectedType (threaded from declaration or argument context)
        LLVMBackend::TypeAndValue returnType;
        returnType.TypeName = lambdaExpectedType.FuncPtrReturnTypeName;
        returnType.Pointer = lambdaExpectedType.FuncPtrReturnPointer;
        if (returnType.TypeName.empty())
            returnType.TypeName = "void";

        // Keep lambda counter in sync: closure name matches lambda name index.
        size_t lambdaIdx  = compiler->lambdaCounter;
        std::string lambdaName = compiler->CreateAnonFunctionName(); // post-increments lambdaCounter

        std::set<std::string> lambdaParamNames;
        for (const auto& p : params)
            lambdaParamNames.insert(p.VariableName);

        // Scan body for captures from the enclosing function scope BEFORE saving builder state.
        auto captures = CollectLambdaCaptures(ctx->lambdaBody(), lambdaParamNames, compiler);

        // Build closure struct alloca in the OUTER function before switching IR context.
        llvm::AllocaInst* closureAlloca = nullptr;
        llvm::StructType* closureStructTy = nullptr;
        auto* i8PtrTy = compiler->builder->getInt8Ty()->getPointerTo();

        if (!captures.empty())
        {
            std::string closureName = "__closure_" + std::to_string(lambdaIdx);
            std::vector<llvm::Type*> closureFields;
            for (const auto& cap : captures)
            {
                if (cap.ByReference)
                    closureFields.push_back(compiler->GetDataStructure(cap.TV.TypeName).StructType->getPointerTo());
                else
                    closureFields.push_back(compiler->GetType(cap.TV));
            }
            closureStructTy = llvm::StructType::create(*compiler->context, closureFields, closureName);
            closureAlloca   = compiler->AllocaAtEntry(closureStructTy, nullptr, closureName);

            for (size_t i = 0; i < captures.size(); i++)
            {
                auto* fieldGEP = compiler->builder->CreateStructGEP(closureStructTy, closureAlloca, (unsigned)i);
                if (captures[i].ByReference)
                {
                    // Store pointer to outer struct (alloca address) in closure field.
                    compiler->builder->CreateStore(captures[i].OuterStorage, fieldGEP);
                }
                else
                {
                    // Store a copy of the value in the closure field.
                    auto* val = compiler->CreateLoad(compiler->GetType(captures[i].TV), captures[i].OuterStorage);
                    compiler->builder->CreateStore(val, fieldGEP);
                }
            }
        }

        // Save builder position - invoker emits into a separate LLVM function.
        auto savedState = compiler->SaveBuilderState();

        // Invoker signature: (i8* __env, user_params...) -> RetType
        LLVMBackend::DeclTypeAndValue envParam;
        envParam.TypeName = "void"; envParam.Pointer = true; envParam.VariableName = "__env";
        std::vector<LLVMBackend::TypeAndValue> allParams;
        allParams.push_back(envParam);
        allParams.insert(allParams.end(), params.begin(), params.end());

        auto* fn = compiler->CreateFunctionDefinition(lambdaName, returnType, allParams);
        compiler->InitializeBlock(&fn->front(), false);

        // Unpack captured variables from env into the invoker's scope.
        if (!captures.empty() && closureStructTy)
        {
            auto* envArg    = fn->getArg(0);
            auto* closurePtr = compiler->builder->CreateBitCast(
                envArg, closureStructTy->getPointerTo(), "closure");

            for (size_t i = 0; i < captures.size(); i++)
            {
                auto* fieldGEP = compiler->builder->CreateStructGEP(
                    closureStructTy, closurePtr, (unsigned)i);
                const auto& cap = captures[i];
                auto& captureNV = compiler->stackNamedVariable.back().namedVariable[cap.Name];

                if (cap.ByReference)
                {
                    // Load pointer to outer struct; register as a pointer-type variable.
                    auto* structTy  = compiler->GetDataStructure(cap.TV.TypeName).StructType;
                    auto* outerPtr  = compiler->builder->CreateLoad(
                        structTy->getPointerTo(), fieldGEP, cap.Name + "_ref");
                    LLVMBackend::TypeAndValue captureTV = cap.TV;
                    captureTV.Pointer       = true;
                    captureTV.ConstArraySize = 0;  // inside the lambda this is a T*, not a T[N]
                    captureNV.Primary = outerPtr;
                    captureNV.TypeAndValue = captureTV;
                    captureNV.BaseType     = structTy->getPointerTo();
                }
                else
                {
                    // Load copied value; store into a local alloca so the body can modify it.
                    auto* capTy  = compiler->GetType(cap.TV);
                    auto* capVal = compiler->builder->CreateLoad(capTy, fieldGEP, cap.Name + "_val");
                    auto* capAlloca = compiler->builder->CreateAlloca(capTy, nullptr, cap.Name);
                    compiler->builder->CreateStore(capVal, capAlloca);
                    captureNV.Storage = capAlloca;
                    captureNV.TypeAndValue = cap.TV;
                    captureNV.BaseType     = capTy;
                }
            }
        }

        // If the lambda parameter was declared lock(this), seed currentLockSet with the
        // resolved receiver guard (e.g. "d.ready") so GuardedBy checks inside the body pass.
        std::string consumedLockThisReceiver = lambdaLockThisReceiver;
        lambdaLockThisReceiver = {};  // consumed - clear before body in case of nested lambdas
        std::unordered_set<std::string> savedLockSet;
        if (!consumedLockThisReceiver.empty())
        {
            savedLockSet = currentLockSet;
            currentLockSet.insert(consumedLockThisReceiver);
        }

        // Parse body
        if (auto* body = ctx->lambdaBody())
        {
            if (auto* block = body->compoundStatement())
            {
                if (auto* items = block->blockItemList())
                    ParseBlockItemList(items);
            }
            else if (auto* expr = body->assignmentExpression())
            {
                auto* val = ParseAssignmentExpression(expr);
                if (val && !compiler->IsBlockTerminated())
                    compiler->CreateReturnCall(val);
            }
        }

        if (!consumedLockThisReceiver.empty())
            currentLockSet = savedLockSet;

        if (returnType.TypeName == "void" && !compiler->IsBlockTerminated())
            compiler->CreateReturnCall(nullptr);
        else if (returnType.TypeName != "void" && !compiler->IsBlockTerminated())
            LogErrorContext(ctx, std::format("Lambda '{}' missing return statement.", lambdaName));

        compiler->CreateBlockBreak(nullptr, true);
        compiler->RestoreBuilderState(savedState);

        // Build user-visible function-pointer TypeAndValue (no __env param).
        LLVMBackend::TypeAndValue tv;
        tv.IsFunctionPointer = true;
        tv.FuncPtrReturnTypeName = returnType.TypeName;
        tv.FuncPtrReturnPointer  = returnType.Pointer;
        for (const auto& p : params)
        {
            LLVMBackend::TypeAndValue::FuncPtrParam fp;
            fp.TypeName = p.TypeName;
            fp.Pointer  = p.Pointer;
            tv.FuncPtrParams.push_back(fp);
        }

        // Build closure fat struct {invoker_as_i8ptr, closure_as_i8ptr_or_null}.
        auto* fnAsI8   = compiler->builder->CreateBitCast(fn, i8PtrTy, "lambda_fn_i8");
        llvm::Value* envForFat = closureAlloca
            ? compiler->builder->CreateBitCast(closureAlloca, i8PtrTy, "closure_i8")
            : static_cast<llvm::Value*>(llvm::ConstantPointerNull::get(i8PtrTy));

        auto* closureFatTy = compiler->GetClosureFatPtrType();
        llvm::Value* fat = llvm::UndefValue::get(closureFatTy);
        fat = compiler->builder->CreateInsertValue(fat, fnAsI8,     {0u});
        fat = compiler->builder->CreateInsertValue(fat, envForFat,  {1u});

        lastLambdaType = tv;
        LLVMBackend::NamedVariable result;
        result.Primary     = fat;
        result.TypeAndValue = tv;

        // Phase 6: Bond tracking - reference-captured variables are held by pointer.
        // The lambda borrows stack addresses that cannot outlive their source scope.
        for (const auto& cap : captures)
        {
            if (cap.ByReference)
            {
                result.IsBonded = true;
                result.BondedSources.push_back(cap.Name);
            }
        }
        if (result.IsBonded)
        {
            compiler->lastCallIsBonded       = true;
            compiler->lastCallBondedSources  = result.BondedSources;
        }

        return result;
    }

    // Map an LLVM type to its CFlat canonical type name for tuple construction.
    std::string LLVMTypeToTypeName(llvm::Type* ty, const std::string& structHint = "")
    {
        if (!ty) return "";
        if (ty->isIntegerTy(1))  return "bool";
        if (ty->isIntegerTy(8))  return "i8";
        if (ty->isIntegerTy(16)) return "i16";
        if (ty->isIntegerTy(32)) return "int";
        if (ty->isIntegerTy(64)) return "i64";
        if (ty->isFloatTy())     return "float";
        if (ty->isDoubleTy())    return "double";
        if (auto* st = llvm::dyn_cast<llvm::StructType>(ty))
            return st->hasName() ? st->getName().str() : structHint;
        if (!structHint.empty()) return structHint;
        return "";
    }

    // Build a tuple<T1,T2,...> value from a parenthesized expression list: (e1, e2, ...)
    LLVMBackend::NamedVariable ParseTupleExpression(CFlatParser::TupleExpressionContext* ctx)
    {
        auto* compiler = Compiler(ctx);
        auto entries = ctx->tupleConstructEntry();

        // Evaluate each element and collect its type name
        std::vector<llvm::Value*> elemValues;
        std::vector<std::string> typeArgs;
        for (auto* entry : entries)
        {
            auto nv = ParseAssignmentExpressionNamed(entry->assignmentExpression());
            auto* loaded = LoadNamedVariable(nv);
            elemValues.push_back(loaded);

            // Prefer TypeAndValue.TypeName (set for variables); fall back to LLVM type
            std::string typeName = nv.TypeAndValue.TypeName;
            bool typeNameInferred = typeName.empty();
            if (typeName.empty() && loaded)
                typeName = LLVMTypeToTypeName(loaded->getType(), nv.TypeAndValue.TypeName);
            // C integer promotion: untyped small-integer literals widen to int (i32)
            if (typeNameInferred && (typeName == "i8" || typeName == "i16"))
            {
                typeName = "int";
                loaded = compiler->builder->CreateSExt(loaded, llvm::Type::getInt32Ty(*compiler->context));
                elemValues.back() = loaded;
            }
            if (nv.TypeAndValue.Pointer && !typeName.empty() && typeName.back() != '*')
                typeName += "*";
            typeArgs.push_back(typeName);
        }

        std::string mangledName = MangledGenericName("tuple", typeArgs);

        // Ensure the tuple instantiation is processed before we use its struct layout
        if (!instantiatedGenerics.count(mangledName) &&
            (genericStructTemplates.count("tuple") || genericClassTemplates.count("tuple")))
        {
            pendingInstantiations.push_back({"tuple", typeArgs, mangledName});
            instantiatedGenerics.insert(mangledName);
            if (!compiler->GetDataStructure(mangledName).StructType)
            {
                compiler->CreateStructType(mangledName, {});
                LLVMBackend::TypeAndValue rt{ .TypeName = mangledName };
                compiler->CreateFunctionDeclaration(mangledName, rt, {});
            }
        }
        // ProcessPendingInstantiations calls ParseStructDefinition which calls
        // CreateFunctionDefinition and moves the builder into the new constructor.
        // Save and restore so we continue emitting into the caller's function body.
        {
            auto savedState = compiler->SaveBuilderState();
            ProcessPendingInstantiations();
            compiler->RestoreBuilderState(savedState);
        }

        // Allocate the tuple struct and store each element into item_i field
        LLVMBackend::TypeAndValue tupleType{ .TypeName = mangledName };
        auto* structType = compiler->GetDataStructure(mangledName).StructType;
        auto* alloca = compiler->CreateAlloca(structType);
        const auto& structData = compiler->GetDataStructure(mangledName);
        for (size_t i = 0; i < elemValues.size(); i++)
        {
            std::string fieldName = "item_" + std::to_string(i);
            unsigned fieldIdx = 0;
            for (const auto& f : structData.StructFields)
            {
                if (f.VariableName == fieldName) break;
                fieldIdx++;
            }
            auto* gep = compiler->CreateStructGEP(structType, alloca, fieldIdx);
            compiler->builder->CreateStore(elemValues[i], gep);
        }

        LLVMBackend::NamedVariable result;
        result.Storage = alloca;
        result.Primary = compiler->builder->CreateLoad(structType, alloca);
        result.TypeAndValue = tupleType;
        return result;
    }

    llvm::Value* ParsePrimaryExpression(CFlatParser::PrimaryExpressionContext* ctx)
    {
        auto* compiler = Compiler(ctx);

        if (auto* tupleCtx = ctx->tupleExpression())
            return ParseTupleExpression(tupleCtx).Primary;

        if (auto* lambdaCtx = ctx->lambdaExpression())
            return ParseLambdaExpression(lambdaCtx).Primary;

        auto expressionCtx = ctx->expression();
        auto constant = ctx->Constant();
        auto stringLiteral = ctx->StringLiteral();

        if (ctx->TypeOf())
        {
            // typeof(int), typeof(bool), typeof(MyStruct) - type specifier used directly
            if (auto* ts = ctx->typeSpecifier())
                return compiler->CreateGlobalString("typeof", ts->getText());

            // typeof(expr) - navigate down to unaryExpression to read TypeAndValue
            std::string typeName;

            // ANTLR picks the expression alternative for user-defined type names (Identifier
            // matches both expression and typeSpecifier); catch them here before evaluating.
            if (compiler->GetDataStructure(expressionCtx->getText()).StructType != nullptr)
                return compiler->CreateGlobalString("typeof", expressionCtx->getText());

            if (auto* ue = tryGetUnaryExpression(expressionCtx))
            {
                auto namedVar = ParseUnaryExpression(ue);
                typeName = namedVar.TypeAndValue.TypeName;
                if (namedVar.TypeAndValue.Pointer && !typeName.empty())
                    typeName += "*";
            }

            return compiler->CreateGlobalString("typeof", typeName.empty() ? "unknown" : typeName);
        }
        else if (ctx->NameOf())
        {
            std::string fullText = expressionCtx->getText();
            // Return just the last identifier after any '.' or '->'
            size_t dotPos = fullText.rfind('.');
            size_t arrowPos = fullText.rfind("->");
            size_t lastSep = 0;
            if (dotPos != std::string::npos) lastSep = std::max(lastSep, dotPos + 1);
            if (arrowPos != std::string::npos) lastSep = std::max(lastSep, arrowPos + 2);
            std::string name = fullText.substr(lastSep);
            return compiler->CreateGlobalString("nameof", name);
        }
        else if (expressionCtx != nullptr)
        {
            // Use ParseAssignmentExpressionNamed to preserve TypeAndValue (e.g. cast type)
            // for ((Struct*)ptr)->field member-access chains that follow this primary.
            auto nv = ParseAssignmentExpressionNamed(expressionCtx->assignmentExpression());
            ProcessPlusPlus();
            lastParenExprType = nv.TypeAndValue;
            return LoadNamedVariable(nv);
        }
        else if (stringLiteral.size() > 0)
        {
            // TODO handle encoding u8,u,U,L
            std::string rawText = ctx->getText();
            if (HasInterpolation(rawText))
                return ParseFormatString(ctx, rawText);
            std::string processed = ProcessRawText(rawText, /*foldBraces=*/true);
            return compiler->CreateGlobalString("", processed);
        }
        else if (constant)
        {
            std::string constantText = constant->getText();
            if (constantText == "true")
            {
                return compiler->CreateConstant("bool", constantText);
            }
            else if (constantText == "false")
            {
                return compiler->CreateConstant("bool", constantText);
            }
            else if (constantText == "nullptr")
            {
                return compiler->CreateConstant("nullptr", constantText);
            }
            else if (constantText.front() == '\'' ||
                (constantText.size() > 1 &&
                    (constantText[0] == 'L' || constantText[0] == 'u' || constantText[0] == 'U') &&
                    constantText[1] == '\''))
            {
                char c = ParseCharLiteral(constantText);
                return compiler->CreateConstant(LLVMBackend::ConstantVariant(c));
            }
            else
            {
                std::string constantRaw = constant->getText();
                auto number = ParseNumberConstant(constantRaw);
                auto value = compiler->CreateConstant(number);
                return value;
            }
        }

        return nullptr;
    }

    LLVMBackend::NamedVariable ParseIdentifier(antlr4::tree::TerminalNode* node)
    {
        auto* compiler = Compiler();
        if (!node)
            return {};

        std::string name = node->getText();
        LLVMBackend::NamedVariable namedVar = {};

        // Check compile-time macros (constant throughout compilation)
        auto macro = compiler->GetCompileTimeMacro(name);
        if (macro.value != nullptr)
        {
            namedVar.Primary = macro.value;
            namedVar.BaseType = macro.value->getType();
            return namedVar;
        }

        // Special case: __FUNCTION__ is context-dependent (changes per function)
        if (name == "__FUNCTION__")
        {
            auto str = compiler->CreateGlobalString("__FUNCTION__", compiler->GetCurrentFunctionName());
            namedVar.Primary = str;
            namedVar.BaseType = str->getType();
            return namedVar;
        }

        // Special case: __LINE__ is location-dependent (changes per location)
        if (name == "__LINE__")
        {
            int line = (int)node->getSymbol()->getLine();
            auto val = compiler->CreateConstant("int", std::to_string(line));
            namedVar.Primary = val;
            namedVar.BaseType = val->getType();
            return namedVar;
        }

        // 'this' inside a member-function body resolves to the implicit self pointer.
        if (name == "this")
        {
            auto thisVar = compiler->GetThisPointer();
            if (thisVar.Storage != nullptr || thisVar.Primary != nullptr)
            {
                thisVar.IdentifierLine = (int)node->getSymbol()->getLine();
                thisVar.IdentifierColumn = (int)node->getSymbol()->getCharPositionInLine();
                return thisVar;
            }
        }

        namedVar = compiler->GetLocalVariable(name);
        if (namedVar.Storage != nullptr || namedVar.Primary != nullptr)
        {
            namedVar.IdentifierLine = (int)node->getSymbol()->getLine();
            namedVar.IdentifierColumn = (int)node->getSymbol()->getCharPositionInLine();
            return namedVar;
        }

        auto funcArgument = compiler->GetFunctionArgument(name);
        if (funcArgument.GetValue() != nullptr)
        {
            funcArgument.IdentifierLine = (int)node->getSymbol()->getLine();
            funcArgument.IdentifierColumn = (int)node->getSymbol()->getCharPositionInLine();
            return funcArgument;
        }

        auto memberVar = compiler->GetMemberVariable(name);
        if (memberVar.Storage != nullptr)
        {
            // Lock-set check: self-access inside a struct method.
            if (!memberVar.TypeAndValue.GuardedBy.empty())
            {
                if (currentLockSet.find(memberVar.TypeAndValue.GuardedBy) == currentLockSet.end())
                {
                    LogErrorContext(node, std::format(
                        "Field '{}' is guarded by '{}': must hold '{}' before accessing it.",
                        name, memberVar.TypeAndValue.GuardedBy, memberVar.TypeAndValue.GuardedBy));
                }
            }
            return memberVar;
        }

        // try getting global variable
        {
            auto globalNV = compiler->GetGlobalVariableNV(name);
            if (globalNV.Storage != nullptr)
                return globalNV;
        }

        if (compiler->GetFunction(name))
        {
            // Use GetFunctionForFuncPtr so that a plain name resolves to the top-level
            // function rather than a struct method registered under the same key.
            namedVar.Primary = compiler->GetFunctionForFuncPtr(name);
            namedVar.CallerName = name;
            return namedVar;
        }

        // Return-block functions have no IR entry; they are inlined at the call site.
        if (compiler->GetReturnBlock(name) != nullptr)
            return {};

        // Generic function templates have no IR entry until instantiated.
        // The call-site dispatch (PostfixExpression's RuleArgumentExpressionList branch)
        // calls TryInferAndInstantiateFromArgs once the arguments are known.
        if (genericFunctionTemplates.count(name))
            return {};

        // Compiler intrinsics handled at the call site - not in the function table.
        static const std::unordered_set<std::string> kIntrinsics = {
            "va_start", "va_end", "is_pointer", "is_primitive", "is_string", "annotationof",
            "reflect", "reflect_set",
        };
        if (kIntrinsics.count(name))
            return {};

        LogErrorContext(node, std::format("Undefined variable {}.", name));
        return {};
    }

    // Consumes one escape sequence starting just after the leading '\'.
    // Advances itr past the consumed character(s) and returns the decoded char.
    char ProcessEscapeChar(std::string::const_iterator& itr, const std::string::const_iterator& end)
    {
        if (itr == end)
            return '\\';

        char esc = *itr++;
        if (esc == 'x' || esc == 'X')
        {
            std::string hex;
            while (itr != end && *itr != '\'' && *itr != '"')
                hex += *itr++;
            return static_cast<char>(std::stoi(hex, nullptr, 16));
        }
        // Octal escape: 1–3 octal digits (\0, \033, \177, etc.).
        if (esc >= '0' && esc <= '7')
        {
            int val = esc - '0';
            for (int i = 0; i < 2 && itr != end && *itr >= '0' && *itr <= '7'; i++)
            {
                val = val * 8 + (*itr - '0');
                ++itr;
            }
            return static_cast<char>(val);
        }
        switch (esc)
        {
        case 'n':  return '\n';
        case 't':  return '\t';
        case 'r':  return '\r';
        case '\\': return '\\';
        case '\'': return '\'';
        case '"':  return '"';
        case 'a':  return '\a';
        case 'b':  return '\b';
        case 'f':  return '\f';
        case 'v':  return '\v';
        case '{':  return '{';
        case '}':  return '}';
        default:   return esc;
        }
    }

    char ParseCharLiteral(const std::string& text)
    {
        auto itr = text.cbegin();

        // Skip encoding prefix (L, u, U)
        if (*itr == 'L' || *itr == 'u' || *itr == 'U')
            ++itr;

        ++itr; // skip opening '

        if (*itr == '\\')
        {
            ++itr;
            return ProcessEscapeChar(itr, text.cend());
        }

        return *itr;
    }

    // foldBraces: when true, {{ → { and }} → } (source-level escape for non-interpolated strings).
    // Pass false when decoding accumulated literal content inside ParseFormatString, because
    // that content may contain legitimate }} sequences (e.g. from JSON) that must not be folded.
    std::string ProcessRawText(const std::string& rawText, bool foldBraces = false)
    {
        std::string output;
        auto itr = rawText.cbegin();

        // Skip encoding prefix (u8, u, U, L)
        if (*itr == 'u' && *(itr + 1) == '8')
            itr += 2;
        else if (*itr == 'u' || *itr == 'U' || *itr == 'L')
            ++itr;

        ++itr; // skip opening "

        while (itr != rawText.cend() && *itr != '"')
        {
            if (*itr == '\\')
            {
                ++itr;
                output += ProcessEscapeChar(itr, rawText.cend());
            }
            else if (foldBraces && (*itr == '{' || *itr == '}') && (itr + 1) != rawText.cend() && *(itr + 1) == *itr)
            {
                // {{ → { and }} → } (only for source-level non-interpolated strings)
                output += *itr;
                itr += 2;
            }
            else
            {
                output += *itr++;
            }
        }

        return output;
    }

    llvm::Value* ParseExpression(CFlatParser::ExpressionContext* ctx)
    {
        auto assignCtxs = ctx->assignmentExpression();
        auto left = this->ParseAssignmentExpression(assignCtxs);
        ProcessPlusPlus();
        return left;

        /*
        // TODO: handle comma operator.
        if (assignCtxs.size() > 0)
        {
            llvm::Value* left = nullptr;
            for (const auto& assignCtx : assignCtxs)
            {
                left = this->ParseAssignmentExpression(assignCtx);
                ProcessPlusPlus();
            }

            return left;
        }
        */

        LogErrorContext(ctx, "Expression has no assignment sub-expressions.");
        return nullptr;
    }

    void ProcessPlusPlus()
    {
        if (PlusPlus.size() > 0)
        {
            for (auto& [destination, amountAndElem] : PlusPlus)
                Compiler()->CreateIncrement(destination, amountAndElem.first, amountAndElem.second);

            PlusPlus.clear();
        }
    }

    // Recursively walk any AST subtree and queue generic instantiations found anywhere
    // (declaration specifiers, initializer expressions, etc.).
    // Skips generic template struct definition bodies (contain unbound type parameters).
    void ScanAndQueueGenericTypeUses(antlr4::RuleContext* ctx)
    {
        if (!ctx) return;
        for (auto* child : ctx->children)
        {
            auto* ruleCtx = dynamic_cast<antlr4::RuleContext*>(child);
            if (!ruleCtx) continue;

            // Use getRuleIndex() (integer compare) instead of dynamic_cast for each node type.
            switch (ruleCtx->getRuleIndex())
            {
            case CFlatParser::RuleStructDefinition:
                // Skip generic template struct/class definition bodies (contain unbound T)
                if (static_cast<CFlatParser::StructDefinitionContext*>(ruleCtx)->genericTypeParameters() != nullptr)
                    continue;
                break;
            case CFlatParser::RuleClassDefinition:
                if (static_cast<CFlatParser::ClassDefinitionContext*>(ruleCtx)->genericTypeParameters() != nullptr)
                    continue;
                break;
            case CFlatParser::RuleTypeSpecifier:
                {
                    // typeSpecifier with generic params: e.g. the "Box<MyInt>" in "Box<MyInt> b"
                    // Apply type substitutions for generic parameters.
                    auto* typeSpec = static_cast<CFlatParser::TypeSpecifierContext*>(ruleCtx);
                    if (typeSpec->genericIdentifier() != nullptr && typeSpec->genericIdentifier()->genericTypeParameters() != nullptr && typeSpec->genericIdentifier()->Identifier() != nullptr)
                    {
                        std::string baseName = typeSpec->genericIdentifier()->Identifier()->getText();
                        std::vector<std::string> typeArgs;
                        for (auto* entry : typeSpec->genericIdentifier()->genericTypeParameters()->typeParameterList()->typeParameterEntry())
                        {
                            std::string arg = entry->getText();
                            // Apply active type substitutions to each type argument
                            auto it = activeTypeSubstitutions.find(arg);
                            if (it != activeTypeSubstitutions.end())
                                arg = it->second;
                            typeArgs.push_back(arg);
                        }
                        std::string mangledName = MangledGenericName(baseName, typeArgs);
                        if (!instantiatedGenerics.count(mangledName))
                        {
                            pendingInstantiations.push_back({baseName, typeArgs, mangledName});
                            instantiatedGenerics.insert(mangledName);
                        }
                    }
                    break;
                }
            case CFlatParser::RulePrimaryExpression:
                {
                    // primaryExpression with generic params: e.g. the "Box<MyInt>" in "Box<MyInt>()"
                    // Apply type substitutions for generic parameters.
                    auto* primaryExpr = static_cast<CFlatParser::PrimaryExpressionContext*>(ruleCtx);
                    if (primaryExpr->genericIdentifier() != nullptr && primaryExpr->genericIdentifier()->genericTypeParameters() != nullptr && primaryExpr->genericIdentifier()->Identifier() != nullptr)
                    {
                        std::string baseName = primaryExpr->genericIdentifier()->Identifier()->getText();
                        std::vector<std::string> typeArgs;
                        for (auto* entry : primaryExpr->genericIdentifier()->genericTypeParameters()->typeParameterList()->typeParameterEntry())
                        {
                            std::string arg = entry->getText();
                            // Apply active type substitutions to each type argument
                            auto it = activeTypeSubstitutions.find(arg);
                            if (it != activeTypeSubstitutions.end())
                                arg = it->second;
                            typeArgs.push_back(arg);
                        }
                        std::string mangledName = MangledGenericName(baseName, typeArgs);
                        if (!instantiatedGenerics.count(mangledName))
                        {
                            pendingInstantiations.push_back({baseName, typeArgs, mangledName});
                            instantiatedGenerics.insert(mangledName);
                        }
                    }
                    break;
                }
            }

            ScanAndQueueGenericTypeUses(ruleCtx);
        }
    }

    // Check if a declaration uses a generic type and queue it for instantiation if needed.
    // Instantiation is deferred to avoid interrupting code generation in the current context.
    void QueueInstantiateGenericType(CFlatParser::DeclarationSpecifiersContext* declSpec)
    {
        for (auto declSpecItem : declSpec->declarationSpecifier())
        {
            auto typeSpec = declSpecItem->typeSpecifier();
            if (!typeSpec) continue;

            // Tuple type sugar: (T1, T2) -> tuple<T1, T2>
            if (typeSpec->tupleTypeSpecifier() != nullptr)
            {
                auto* tts = typeSpec->tupleTypeSpecifier();
                // Pack-only form (T...) resolved during instantiation - skip queuing here
                if (tts->tupleTypePackEntry() != nullptr)
                    break;
                std::vector<std::string> typeArgs;
                for (auto* entry : tts->tupleTypeEntry())
                {
                    std::string argName = entry->typeSpecifier()->getText();
                    if (entry->pointer() != nullptr) argName += "*";
                    typeArgs.push_back(argName);
                }
                std::string mangledName = MangledGenericName("tuple", typeArgs);
                if (!instantiatedGenerics.count(mangledName))
                {
                    pendingInstantiations.push_back({"tuple", typeArgs, mangledName});
                    instantiatedGenerics.insert(mangledName);
                }
                break;
            }

            if (!typeSpec->genericIdentifier() || !typeSpec->genericIdentifier()->genericTypeParameters())
                continue;

            // This is a generic type instantiation
            std::string baseName = typeSpec->genericIdentifier()->Identifier()->getText();
            std::vector<std::string> typeArgs;
            // Resolve via ResolveTypeArgEntry so active type-parameter substitutions are
            // applied (e.g. channel<T> -> channel__int inside an instantiated generic body).
            // Without this, the literal "T" is queued (channel__T), which later fails to
            // instantiate with "unknown type 'T'". No-op for already-concrete args.
            for (auto* entry : typeSpec->genericIdentifier()->genericTypeParameters()->typeParameterList()->typeParameterEntry())
                typeArgs.push_back(ResolveTypeArgEntry(entry));

            std::string mangledName = MangledGenericName(baseName, typeArgs);

            // Queue the instantiation instead of doing it immediately
            if (!instantiatedGenerics.count(mangledName))
            {
                pendingInstantiations.push_back({baseName, typeArgs, mangledName});
                instantiatedGenerics.insert(mangledName);
            }
            break;
        }
    }

    // Compute the mangled name for a generic instantiation, e.g. Box<int, float> -> "Box__int__float".
    std::string MangledGenericName(const std::string& baseName, const std::vector<std::string>& typeArgs)
    {
        std::string name = baseName;
        for (const auto& arg : typeArgs)
            name += "__" + MangleTypeArg(arg);
        return name;
    }

    // Process all pending generic instantiations that were queued during parsing.
    // This is called when it's safe to emit code (e.g., after a declaration completes).
    void ProcessPendingInstantiations()
    {
        while (!pendingInstantiations.empty())
        {
            auto pending = pendingInstantiations.back();
            pendingInstantiations.pop_back();

            // Now safe to instantiate
            auto structIt = genericStructTemplates.find(pending.templateName);
            auto classIt = genericClassTemplates.find(pending.templateName);
            auto ifaceIt = genericInterfaceTemplates.find(pending.templateName);
            if (structIt == genericStructTemplates.end() && classIt == genericClassTemplates.end())
            {
                if (ifaceIt != genericInterfaceTemplates.end())
                {
                    // It's a generic interface - instantiate it (pack-aware)
                    std::unordered_map<std::string, std::string> ifaceSubst;
                    std::unordered_map<std::string, std::vector<std::string>> ifacePackSubst;
                    const auto& ifaceTypeParams = genericInterfaceTypeParams[pending.templateName];
                    auto packIdxIt = genericInterfacePackIndex.find(pending.templateName);
                    size_t packIdx = (packIdxIt != genericInterfacePackIndex.end()) ? packIdxIt->second : std::string::npos;
                    if (packIdx == std::string::npos)
                    {
                        for (size_t i = 0; i < ifaceTypeParams.size() && i < pending.typeArgs.size(); i++)
                            ifaceSubst[ifaceTypeParams[i]] = pending.typeArgs[i];
                    }
                    else
                    {
                        for (size_t i = 0; i < packIdx && i < pending.typeArgs.size(); i++)
                            ifaceSubst[ifaceTypeParams[i]] = pending.typeArgs[i];
                        ifacePackSubst[ifaceTypeParams[packIdx]] =
                            std::vector<std::string>(pending.typeArgs.begin() + packIdx, pending.typeArgs.end());
                    }
                    InstantiateGenericInterface(pending.templateName, pending.mangledName, ifaceSubst, ifacePackSubst);
                }
                else if (Compiler()->IsVerbose())
                {
                    std::cout << "[verbose]   skip instantiation '" << pending.mangledName
                              << "': template '" << pending.templateName << "' not found\n";
                }
                continue;
            }

            if (Compiler()->IsVerbose())
                std::cout << "[verbose]   instantiate generic: " << pending.mangledName << "\n";

            const auto& typeParams = genericStructTypeParams[pending.templateName];

            // Verify where-clause constraints before instantiating
            auto* ctxForError = structIt != genericStructTemplates.end()
                ? (antlr4::ParserRuleContext*)structIt->second
                : (antlr4::ParserRuleContext*)classIt->second;
            const auto& constraintMap = structIt != genericStructTemplates.end()
                ? genericStructConstraints : genericClassConstraints;
            if (!CheckConstraints(pending.templateName, typeParams, pending.typeArgs, constraintMap, ctxForError))
                continue;

            // Set up type substitutions for this instantiation
            auto savedSubst = activeTypeSubstitutions;
            auto savedPackSubst = activePackSubstitutions;

            auto packMapIt = genericStructPackIndex.find(pending.templateName);
            size_t packIdx = (packMapIt != genericStructPackIndex.end()) ? packMapIt->second : std::string::npos;

            if (packIdx == std::string::npos)
            {
                // Non-variadic: 1:1 mapping
                for (size_t i = 0; i < typeParams.size() && i < pending.typeArgs.size(); i++)
                    activeTypeSubstitutions[typeParams[i]] = pending.typeArgs[i];
            }
            else
            {
                // Fixed params before the pack
                for (size_t i = 0; i < packIdx && i < pending.typeArgs.size(); i++)
                    activeTypeSubstitutions[typeParams[i]] = pending.typeArgs[i];
                // Pack param absorbs remaining type args
                activePackSubstitutions[typeParams[packIdx]] =
                    std::vector<std::string>(pending.typeArgs.begin() + packIdx, pending.typeArgs.end());
            }

            if (structIt != genericStructTemplates.end())
                ParseStructDefinition(structIt->second, pending.mangledName);
            else
                ParseClassDefinition(classIt->second, pending.mangledName);

            activeTypeSubstitutions = savedSubst;
            activePackSubstitutions = savedPackSubst;
        }
    }

    void ParseStructDefinition(CFlatParser::StructDefinitionContext* ctx, const std::string& nameOverride = {}, const std::string& namespaceName = {})
    {
        auto* compiler = Compiler(ctx);
        auto decl = ctx->directDeclarator();
        std::string baseName = decl->getText();
        std::string structName;

        // Apply nameOverride first (for generic instantiations), then namespace
        if (!nameOverride.empty())
        {
            structName = nameOverride;
        }
        else if (!namespaceName.empty())
        {
            structName = namespaceName + "." + baseName;
        }
        else
        {
            structName = baseName;
        }

        // If this is a generic template definition (not an instantiation), store it and return.
        if (nameOverride.empty() && ctx->genericTypeParameters() != nullptr)
        {
            auto typeParams = ParseGenericTypeParameters(ctx->genericTypeParameters());
            genericStructTemplates[structName] = ctx;
            genericStructTypeParams[structName] = typeParams;
            genericStructConstraints[structName] = ParseWhereClause(ctx->whereClause());
            // Record which param (if any) is variadic - always the last one
            {
                auto entries = ctx->genericTypeParameters()->typeParameterList()->typeParameterEntry();
                bool hasPack = !entries.empty() && entries.back()->Ellipsis() != nullptr;
                genericStructPackIndex[structName] = hasPack ? (typeParams.size() - 1) : std::string::npos;
            }
            return;
        }

        // Re-emission guard: when the user's main file is also reached via a
        // transitive import under a different canonical path (e.g. the deployed
        // copy of a cflat/core/ file), the runtime chain may have already fully
        // emitted this struct. CreateFunctionDefinition's duplicate-skip leaves
        // the IR builder without the right scope to safely emit the bodies a
        // second time, so any field-or-method reference falls over. Detect
        // "already fully defined" here and skip the entire walk.
        {
            auto sd = compiler->GetDataStructure(structName);
            if (sd.StructType != nullptr && !sd.StructType->isOpaque())
            {
                if (auto* existing = compiler->GetFunction(structName);
                    existing != nullptr && !existing->empty())
                {
                    if (compiler->IsVerbose())
                        std::cout << "[verbose]     skipping duplicate struct definition: " << structName << "\n";
                    return;
                }
            }
        }

        if (compiler->IsVerbose())
            std::cout << "[verbose]     parse decl list: " << structName << "\n";

        // Process nested struct/class definitions before fields so their types are available
        for (auto* nestedStruct : ctx->structDefinition())
            ParseStructDefinition(nestedStruct, {}, structName);
        for (auto* nestedClass : ctx->classDefinition())
            ParseClassDefinition(nestedClass, {}, structName);

        // Push scope so unqualified nested type names resolve (e.g. Inner -> Outer.Inner)
        structScopeStack.push_back(structName);

        auto declarationList = ctx->declaration();
        std::vector<llvm::Type*> types;

        // Queue and instantiate generic types used in field declarations before
        // ParseDeclarationList resolves them to LLVM types. Only needed at top-level
        // (non-template) scope; template instantiations already have activeTypeSubstitutions
        // or activePackSubstitutions set and their generics are queued via ParseDeclarationSpecifiers.
        if (activeTypeSubstitutions.empty() && activePackSubstitutions.empty())
        {
            for (auto decl : declarationList)
                ScanAndQueueGenericTypeUses(decl);
            ProcessPendingInstantiations();
        }

        // Build field list, expanding pack fields (T... fieldName -> fieldName_0, fieldName_1, ...)
        std::vector<LLVMBackend::DeclTypeAndValue> declList;
        for (auto* decl : declarationList)
        {
            std::string packParamName;
            if (decl->declarationSpecifiers())
            {
                for (auto* ds : decl->declarationSpecifiers()->declarationSpecifier())
                {
                    auto* ts = ds->typeSpecifier();
                    if (!ts || !ts->genericIdentifier() || ts->genericIdentifier()->genericTypeParameters()) continue;
                    auto* gid = ts->genericIdentifier();
                    if (!gid->Identifier()) continue;
                    std::string n = gid->Identifier()->getText();
                    if (activePackSubstitutions.count(n)) { packParamName = n; break; }
                }
            }

            if (packParamName.empty())
            {
                for (auto& f : ParseDeclarationList({decl}))
                    declList.push_back(f);
                continue;
            }

            std::string baseFieldName;
            if (auto* idl = decl->initDeclaratorList())
                if (!idl->initDeclarator().empty())
                    if (auto* d = idl->initDeclarator()[0]->declarator())
                        if (auto* dd = d->directDeclarator())
                            baseFieldName = getDirectDeclName(dd);

            auto& packTypes = activePackSubstitutions.at(packParamName);
            auto savedPackItemSubst = activeTypeSubstitutions;
            for (size_t i = 0; i < packTypes.size(); i++)
            {
                activeTypeSubstitutions[packParamName] = packTypes[i];
                auto expanded = ParseDeclarationList({decl});
                for (auto& f : expanded)
                {
                    f.VariableName = baseFieldName + "_" + std::to_string(i);
                    declList.push_back(f);
                }
            }
            activeTypeSubstitutions = savedPackItemSubst;
        }

        // Process lock field groups: each group annotates its fields with GuardedBy,
        // and registers member functions with the group's lock as a RequiredLock.
        for (auto* lfg : ctx->lockFieldGroup())
        {
            // Also queue generic types used inside the group.
            if (activeTypeSubstitutions.empty() && activePackSubstitutions.empty())
                ScanAndQueueGenericTypeUses(lfg);

            // Extract the guardian name from the single lock arg expression.
            auto groupArgs = lfg->lockClause()->lockArgList()->expression();
            if (groupArgs.empty()) continue;
            std::string guardianName = GetLockArgCanonical(groupArgs[0]);

            for (auto* decl : lfg->declaration())
            {
                for (auto& f : ParseDeclarationList({decl}))
                {
                    f.GuardedBy = guardianName;
                    declList.push_back(f);
                }
            }
        }

        if (compiler->IsVerbose())
            std::cout << "[verbose]     decl list has " << declList.size() << " fields\n";

        // Bitfield packing: collapse runs of same-type bitfields into shared
        // storage slots BEFORE the constructor loop runs - the default ctor
        // emits one initializer per declList entry and the LLVM struct body
        // must match. The side-table goes into StructData.Bitfields.
        std::vector<LLVMBackend::BitfieldInfo> packedBitfields;
        bool anyBitfields = false;
        for (const auto& tv : declList) { if (tv.IsBitfield) { anyBitfields = true; break; } }
        if (anyBitfields)
            declList = compiler->PackBitfields(declList, packedBitfields);

        // Build the struct body before opening the constructor function so that
        // GetFunctionType can resolve the (sized) return type.  Initializer
        // expressions are evaluated later inside the constructor body.
        bool isUnion = (ctx->Union() != nullptr);

        if (compiler->IsVerbose())
            std::cout << "[verbose]     create struct type: " << structName << "\n";
        llvm::StructType* structType;
        if (isUnion)
        {
            structType = compiler->CreateUnionType(structName, declList);
        }
        else
        {
            // Capture `struct alignas(N) S { ... }` BEFORE struct layout so the
            // padding member can be appended atomically.
            uint64_t userAlign = 0;
            if (auto* alignSpec = ctx->alignmentSpecifier())
                userAlign = ParseAlignmentSpecifier(alignSpec);
            structType = compiler->CreateStructType(structName, declList, userAlign,
                anyBitfields ? &packedBitfields : nullptr);
            // A struct with zero fields still needs a sized (non-opaque) type
            // so that alloca/sizeof work correctly (e.g. when passed via interface).
            if (structType->isOpaque())
                structType->setBody(llvm::ArrayRef<llvm::Type*>());
        }
        if (compiler->IsVerbose())
            std::cout << "[verbose]     create default ctor: " << structName << "\n";
        LLVMBackend::TypeAndValue returnType{
            .TypeName = structName,
        };
        // Flush any nested generic instantiations queued while parsing field declarations,
        // so their constructors exist before this struct's default constructor calls them.
        {
            auto savedSubst = activeTypeSubstitutions;
            ProcessPendingInstantiations();
            activeTypeSubstitutions = savedSubst;
        }
        // If the user wrote an explicit no-arg constructor, skip the auto-generated one.
        // ParseConstructorDefinition will handle it later in the member function loop.
        bool hasExplicitNoArgCtor = !isUnion && [&]() {
            for (auto* f : ctx->functionDefinition())
                if (getFunctionName(f) == baseName && !f->parameterTypeList())
                    return true;
            return false;
        }();

        // Create default constructor (skipped when user provides an explicit no-arg ctor)
        if (!hasExplicitNoArgCtor)
        {
            auto funcDef = compiler->CreateFunctionDefinition(structName, returnType, {});

            if (isUnion)
            {
                // Union: zero-initialize all storage. Fields share the same memory, so we
                // can't insert individual field values - just return a zeroed union value.
                compiler->CreateReturnCall(llvm::Constant::getNullValue(structType));
            }
            else
            {
                std::vector<llvm::Value*> initializers;
                for (auto& typeValue : declList)
                {
                    auto initializer = typeValue.Initializer;
                    llvm::Value* rvalue = nullptr;
                    if (initializer != nullptr)
                    {
                        auto assignmentExpression = initializer->assignmentExpression();
                        if (assignmentExpression != nullptr)
                        {
                            rvalue = ParseAssignmentExpression(assignmentExpression);
                            if (typeValue.TypeName == "auto")
                            {
                                typeValue.TypeName = rvalue->getType()->getStructName();
                                // Re-finalise the struct body now that the auto field type is known.
                                structType = compiler->CreateStructType(structName, declList);
                            }
                        }
                        else if (initializer->Default() != nullptr)
                        {
                            rvalue = GenerateDefaultValue(typeValue);
                        }
                    }
                    initializers.push_back(rvalue);
                }

                llvm::Value* structVal = llvm::UndefValue::get(structType);

                LLVMBackend::TypeAndValue myStruct;
                myStruct.TypeName = structName;
                myStruct.VariableName = "_" + structName;

                unsigned int structIndex = 0;

                for (auto rvalue : initializers)
                {
                    auto* destType = structType->getTypeAtIndex(structIndex);
                    // No explicit initializer on a struct-typed field - call its default ctor.
                    if (rvalue == nullptr && destType->isStructTy())
                    {
                        std::string fieldTypeName = declList[structIndex].TypeName;
                        if (compiler->GetFunction(fieldTypeName))
                            rvalue = compiler->CreateOverloadedFunctionCall(fieldTypeName, {});
                        else
                            rvalue = llvm::Constant::getNullValue(destType);
                    }
                    // Synthesized bitfield storage slot: always zero so packed
                    // bitfields read as 0 after `S s = default;`.
                    if (rvalue == nullptr && declList[structIndex].IsBitfieldStorage)
                    {
                        rvalue = llvm::Constant::getNullValue(destType);
                    }
                    if (rvalue != nullptr)
                    {
                        rvalue = compiler->Upconvert(rvalue, destType);
                        if (rvalue->getType() != destType)
                        {
                            if (destType->isStructTy())
                            {
                                // Initializer type doesn't match struct field type (e.g. integer 0 used for
                                // a struct-typed generic field).  Call the field's default constructor when
                                // one is available; otherwise zero-initialize the aggregate.
                                std::string fieldTypeName = declList[structIndex].TypeName;
                                if (compiler->GetFunction(fieldTypeName))
                                    rvalue = compiler->CreateOverloadedFunctionCall(fieldTypeName, {});
                                else
                                    rvalue = llvm::Constant::getNullValue(destType);
                            }
                            else
                            {
                                // Narrowing field initializer (e.g. u8 r = 255 has i32 literal).
                                compiler->LogWarning(std::format(
                                    "implicit narrowing to '{}' in field '{}' - use an explicit cast",
                                    declList[structIndex].TypeName,
                                    declList[structIndex].VariableName));
                                rvalue = compiler->CreateCast(rvalue, destType);
                            }
                        }
                        structVal = compiler->CreateInsertValue(structVal, rvalue, structIndex);
                    }

                    structIndex++;
                }

                // close constructor.
                compiler->CreateReturnCall(structVal);
            }

            // Pop the stack
            compiler->CreateBlockBreak(nullptr, true);
        } // end if (!hasExplicitNoArgCtor)

        // Register struct fields in LSP index for dot-completion
        if (auto* s = compiler->GetSymbolSink())
        {
            auto sd = compiler->GetDataStructure(structName);
            for (const auto& field : sd.StructFields)
            {
                if (field.VariableName.empty()) continue;
                std::string annSig;
                for (const auto& ann : field.Annotations)
                {
                    annSig += "[" + ann.Name;
                    if (!ann.Value.empty()) annSig += "(" + ann.Value + ")";
                    annSig += "] ";
                }
                std::string typeSig = field.TypeName;
                if (field.Pointer) typeSig += "*";
                if (field.ElemPointer) typeSig += "*";
                s->Register(SymbolKind::Field, structName + "." + field.VariableName,
                            compiler->GetSourceFilePath(),
                            (int)ctx->getStart()->getLine(),
                            (int)ctx->getStart()->getCharPositionInLine(),
                            annSig + typeSig + " " + field.VariableName);
            }
        }

        // Parse member functions
        auto functionList = ctx->functionDefinition();

        // For generic instantiations, pre-declare all member function signatures
        // so they can forward-reference each other. (Non-generic structs already
        // get this via ForwardRefScanner::ScanStructDefinition.)
        if (!nameOverride.empty())
        {
            for (auto func : functionList)
            {
                if (IsReturnBlockFunction(func)) continue;
                if (func->genericTypeParameters() != nullptr) continue;

                std::string funcName = getFunctionName(func);

                // Constructor overload - pre-declare without this* parameter
                if (funcName == baseName && func->parameterTypeList())
                {
                    auto* declParamList = func->parameterTypeList();
                    auto declParams = this->ParseParameterTypeList(declParamList);
                    bool declVarargs = declParamList->Ellipsis() != nullptr;
                    std::vector<LLVMBackend::TypeAndValue> ctorAllParams(declParams.begin(), declParams.end());
                    compiler->CreateFunctionDeclaration(structName, returnType, ctorAllParams, false, declVarargs);
                    continue;
                }

                bool isOperatorFunc = (funcName == "operator new"
                                    || funcName == "operator delete"
                                    || funcName == "operator string");
                bool isStaticFunc = isFunctionStatic(func);
                bool isStaticLike = isOperatorFunc || isStaticFunc;
                std::string declName = isStaticLike ? (structName + "." + funcName) : funcName;

                auto declReturnType = this->getFunctionReturnType(func);
                auto* declParamList = func->parameterTypeList();
                auto declParams = this->ParseParameterTypeList(declParamList);
                bool declVarargs = declParamList && declParamList->Ellipsis() != nullptr;

                if (!isStaticLike)
                {
                    LLVMBackend::DeclTypeAndValue thisParam;
                    thisParam.TypeName = structName;
                    thisParam.VariableName = structName + "__";
                    thisParam.Pointer = true;
                    declParams.insert(declParams.begin(), thisParam);
                }

                std::vector<LLVMBackend::TypeAndValue> declAllParams(declParams.begin(), declParams.end());
                bool declReturnsOwned = false;
                if (declReturnType.TypeName == "string")
                {
                    if (declName == "operator+")
                        declReturnsOwned = true;
                    else if (declName == "operator string" && declAllParams.size() == 1 && declAllParams[0].TypeName == "i32")
                        declReturnsOwned = true;
                    else if (declReturnType.IsMove)
                        declReturnsOwned = true;
                }
                else if (declReturnType.IsMove && declReturnType.Pointer)
                {
                    declReturnsOwned = true;
                }
                compiler->CreateFunctionDeclaration(declName, declReturnType, declAllParams, declReturnType.external, declVarargs, declReturnsOwned, !isStaticLike);
            }
        }

        // Pre-register destructor so 'delete' inside static methods can call it.
        if (!ctx->destructorDefinition().empty())
        {
            if (auto* dtorFn = compiler->GetFunction("~" + structName))
                compiler->RegisterDestructor(structName, dtorFn);
        }

        {
            bool savedScope = global_scope;
            for (auto func : functionList)
            {
                global_scope = false;
                std::string funcName = getFunctionName(func);
                if (compiler->IsVerbose())
                    std::cout << "[verbose]     parse member: " << structName << "." << funcName << "\n";
                // Constructor - same name as struct (no-arg or with parameters)
                if (funcName == baseName)
                {
                    ParseConstructorDefinition(func, structName);
                    continue;
                }
                if (funcName == "operator new" || funcName == "operator delete" || isFunctionStatic(func))
                {
                    if (func->genericTypeParameters() != nullptr)
                    {
                        std::string qualifiedName = structName + "." + funcName;
                        auto typeParams = ParseGenericTypeParameters(func->genericTypeParameters());
                        genericFunctionTemplates[qualifiedName] = func;
                        genericFunctionTypeParams[qualifiedName] = typeParams;
                        genericFunctionConstraints[qualifiedName] = ParseWhereClause(func->whereClause());
                    }
                    else
                    {
                        ParseFunctionDefinition(func, {}, {}, structName + "." + funcName);
                    }
                }
                else
                    ParseFunctionDefinition(func, structName);
            }
            global_scope = savedScope;
        }

        // Parse functions declared inside positional lock groups.
        {
            bool savedScope = global_scope;
            for (auto* lfg : ctx->lockFieldGroup())
            {
                for (auto* func : lfg->functionDefinition())
                {
                    global_scope = false;
                    ParseFunctionDefinition(func, structName);
                }
            }
            global_scope = savedScope;
        }

        // Parse destructor
        {
            bool savedScope = global_scope;
            for (auto dtor : ctx->destructorDefinition())
            {
                global_scope = false;
                ParseDestructorDefinition(dtor, structName);
            }
            global_scope = savedScope;
        }

        // Process any generic instantiations that were queued during this struct definition
        // ProcessPendingInstantiations();

        structScopeStack.pop_back();
    }

    // Extract the canonical lock expression text from a single lock arg expression.
    // Strips a trailing '.read' or '.write' soft-keyword suffix and returns the base.
    // The mode suffix is handled by the caller via GetLockArgMode().
    std::string GetLockArgCanonical(CFlatParser::ExpressionContext* expr)
    {
        std::string text = expr->getText();
        // Strip trailing .read or .write (soft keywords for rwlock mode)
        if (text.size() > 5 && text.substr(text.size() - 5) == ".read")
            return text.substr(0, text.size() - 5);
        if (text.size() > 6 && text.substr(text.size() - 6) == ".write")
            return text.substr(0, text.size() - 6);
        return text;
    }

    // Returns "read", "write", or "" (mutex / default exclusive) for the lock arg expression.
    std::string GetLockArgMode(CFlatParser::ExpressionContext* expr)
    {
        std::string text = expr->getText();
        if (text.size() > 5 && text.substr(text.size() - 5) == ".read")
            return "read";
        if (text.size() > 6 && text.substr(text.size() - 6) == ".write")
            return "write";
        return "";
    }

    // Strip ".read" / ".write" rwlock mode suffix from a raw lock string.
    static std::string StripLockModeSuffix(const std::string& text)
    {
        if (text.size() > 5 && text.substr(text.size() - 5) == ".read")
            return text.substr(0, text.size() - 5);
        if (text.size() > 6 && text.substr(text.size() - 6) == ".write")
            return text.substr(0, text.size() - 6);
        return text;
    }

    // Verify that the current lock-set satisfies the RequiredLocks of the function just called.
    // Called immediately after CreateOverloadedFunctionCall; reads lastCallRequiredLocks and
    // lastCallParameterNames from the side-channel populated by that call.
    // receiverText: the name of the receiver object ("acct"), "this" for bare method calls, or "".
    // arguments: the NamedVariable vector passed to the call (includes implicit this at index 0 when present).
    void CheckCallSiteLocks(antlr4::ParserRuleContext* ctx,
                            const std::string& receiverText,
                            const std::vector<LLVMBackend::NamedVariable>& arguments)
    {
        const auto& requiredLocks = compilerLLVM->lastCallRequiredLocks;
        if (requiredLocks.empty()) return;

        const auto& paramNames = compilerLLVM->lastCallParameterNames;

        for (const auto& rawLock : requiredLocks)
        {
            std::string lock = StripLockModeSuffix(rawLock);

            // Split on the first '.' to get head (owner) and rest (field path).
            size_t dot = lock.find('.');
            std::string head = (dot == std::string::npos) ? lock : lock.substr(0, dot);
            std::string rest = (dot == std::string::npos) ? "" : lock.substr(dot + 1);

            std::string canonical;
            if (head == "this")
            {
                // this-relative lock: substitute with the receiver variable name.
                if (receiverText.empty()) continue; // no known receiver - skip
                canonical = rest.empty() ? receiverText : receiverText + "." + rest;
            }
            else
            {
                // Try to match head to a formal parameter name.
                bool found = false;
                for (size_t pi = 0; pi < paramNames.size(); pi++)
                {
                    if (paramNames[pi] != head) continue;
                    // arguments[pi] corresponds to paramNames[pi] (both include implicit this at index 0).
                    if (pi < arguments.size())
                    {
                        const std::string& argName = arguments[pi].CallerName;
                        if (argName.empty()) found = true; // complex expression - can't check
                        else
                            canonical = rest.empty() ? argName : argName + "." + rest;
                    }
                    found = true;
                    break;
                }
                if (!found)
                    canonical = lock; // global lock - no substitution
            }

            if (!canonical.empty() && currentLockSet.find(canonical) == currentLockSet.end())
            {
                LogErrorContext(ctx, std::format(
                    "must hold '{}' before calling this function.", canonical));
            }
        }
    }

    // Find the first registered overload of `methodName` whose first parameter type is `firstParamType`.
    llvm::Function* FindMethodOf(const std::string& methodName, const std::string& firstParamType)
    {
        auto it = compilerLLVM->functionTable.find(methodName);
        if (it == compilerLLVM->functionTable.end()) return nullptr;
        for (const auto& sym : it->second)
        {
            if (!sym.Parameters.empty() && sym.Parameters[0].TypeName == firstParamType)
                return sym.Function;
        }
        return nullptr;
    }

    void EmitProgramRunWrapper(const std::string& name)
    {
        auto* compiler = compilerLLVM;

        // Resolve types - all must be concrete by this point (ProcessPendingInstantiations ran)
        auto* progType        = compiler->dataStructures[name].StructType;
        auto* defAllocType    = compiler->dataStructures.count("MallocAllocator")
                                ? compiler->dataStructures["MallocAllocator"].StructType : nullptr;
        auto* listStringType  = compiler->dataStructures.count("list__string")
                                ? compiler->dataStructures["list__string"].StructType : nullptr;
        auto* stringStructType= compiler->dataStructures.count("string")
                                ? compiler->dataStructures["string"].StructType : nullptr;
        auto* threadType      = compiler->dataStructures.count("Thread")
                                ? compiler->dataStructures["Thread"].StructType : nullptr;
        auto* fatTy           = compiler->GetFatPtrType();   // {i8*, i8*}

        if (!progType || !defAllocType || !listStringType || !threadType)
        {
            compiler->LogError(std::format(
                "program '{}': missing required type (MallocAllocator={}, list__string={}, Thread={})",
                name,
                defAllocType  ? "ok" : "missing",
                listStringType ? "ok" : "missing",
                threadType    ? "ok" : "missing"));
            return;
        }

        auto* progPtrType    = progType->getPointerTo();
        auto* defAllocPtrTy  = defAllocType->getPointerTo();
        auto* voidPtrType    = compiler->builder->getInt8Ty()->getPointerTo();
        auto* i32Type        = llvm::Type::getInt32Ty(*compiler->context);
        auto* i64Type        = llvm::Type::getInt64Ty(*compiler->context);

        // __RunArgs_Name = { Name*, list__string }
        auto* runArgsType = llvm::StructType::create(
            *compiler->context, {progPtrType, listStringType}, "__RunArgs_" + name);
        compiler->programTable[name].RunArgsType = runArgsType;

        // Look up helper functions
        auto* mallocFn         = compiler->GetFunction("malloc");
        auto* freeFn           = compiler->GetFunction("free");
        auto* defAllocCtorFn   = compiler->GetFunction("MallocAllocator");
        auto* threadCtorFn     = compiler->GetFunction("Thread");
        auto* threadStartFn    = FindMethodOf("start", "Thread");
        auto* threadJoinFn     = FindMethodOf("join", "Thread");

        bool isImported = compiler->programTable[name].IsImportedProgram;

        // Find the 'main' function for this program.
        // For imported programs, MainFunction was already set by the pre-scan.
        // For regular programs, search the function table for a method with self as first param.
        llvm::Function* mainFn = nullptr;
        if (isImported)
        {
            mainFn = compiler->programTable[name].MainFunction;
        }
        else
        {
            auto it = compiler->functionTable.find("main");
            int mainCount = 0;
            if (it != compiler->functionTable.end())
            {
                for (const auto& sym : it->second)
                {
                    if (!sym.Parameters.empty() && sym.Parameters[0].TypeName == name)
                    {
                        ++mainCount;
                        mainFn = sym.Function;
                    }
                }
            }
            if (mainCount > 1)
            {
                compiler->LogError(std::format(
                    "program '{}': multiple 'main' methods defined; only one is allowed.", name));
                return;
            }
        }

        if (!mallocFn || !freeFn || !defAllocCtorFn
            || !threadCtorFn || !threadStartFn || !threadJoinFn || !mainFn)
        {
            compiler->LogError(std::format("program '{}': missing helper function for run() generation", name));
            return;
        }

        // Detect calling style from LLVM arg count.
        // Regular program methods have 'self' as the first arg, so the counts are offset by 1:
        //   1 arg  = NoArgs   (self - "int main()")
        //   2 args = ListArgs (self + list<string> - "int main(move list<string> args)")
        //   3 args = ArgcArgv (self + int + char** - "int main(int argc, char** argv)")
        // Imported programs are free functions (no self), so counts are:
        //   0 args = NoArgs   ("int main()")
        //   1 arg  = ListArgs ("int main(move list<string> args)")  [not yet supported]
        //   2 args = ArgcArgv ("int main(int argc, char** argv)")
        auto mainArgCount = static_cast<unsigned>(mainFn->arg_size());
        bool isNoArgs   = isImported ? (mainArgCount == 0) : (mainArgCount == 1);
        bool isListArgs = isImported ? (mainArgCount == 1) : (mainArgCount == 2);
        bool isArgcArgv = isImported ? (mainArgCount == 2) : (mainArgCount == 3);

        if (!isNoArgs && !isListArgs && !isArgcArgv)
        {
            compiler->LogError(std::format(
                "program '{}': 'main' has unsupported signature (expected 0, 1, or 2 user params).", name));
            return;
        }

        if (isArgcArgv && !stringStructType)
        {
            compiler->LogError(std::format(
                "program '{}': main(int,char**) style requires 'string' type to be available.", name));
            return;
        }

        unsigned exitCodeIdx      = compiler->programTable[name].ExitCodeFieldIndex;
        unsigned threadIdx        = compiler->programTable[name].ThreadFieldIndex;
        unsigned allocatorIdx     = compiler->programTable[name].AllocatorFieldIndex;
        unsigned onStdoutIdx         = compiler->programTable[name].OnStdoutFieldIndex;
        unsigned onStdinIdx          = compiler->programTable[name].OnStdinFieldIndex;
        unsigned onStdinReturnIdx    = compiler->programTable[name].OnStdinReturnFieldIndex;
        unsigned stopSrcIdx          = compiler->programTable[name].StopSourceFieldIndex;
        unsigned trackHandlesIdx     = compiler->programTable[name].TrackHandlesFieldIndex;

        auto* stopSrcInitFn        = FindMethodOf("init",           "stop_source");
        auto* stopSrcRequestStopFn = FindMethodOf("request_stop",   "stop_source");
        auto* stopSrcDisposeFn     = FindMethodOf("dispose",        "stop_source");
        auto* stopSrcType          = compiler->dataStructures.count("stop_source")
                                     ? compiler->dataStructures["stop_source"].StructType : nullptr;

        // __ProgramTLS field indices - must match struct __ProgramTLS in cruntime.cb
        constexpr int kPTLS_stdout_hook            = 0;
        constexpr int kPTLS_stdin_hook             = 4;
        constexpr int kPTLS_stdin_return_hook      = 8;
        constexpr int kPTLS_cached_stdin           = 9;
        constexpr int kPTLS_handle_tracker_enabled = 10;
        constexpr int kPTLS_handle_tracker_head    = 11;
        constexpr int kPTLS_stdin_active           = 12;

        // Look up the single thread-local TLS struct (declared in cruntime.cb)
        llvm::GlobalVariable* progTlsGlobal = nullptr;
        {
            auto it = compiler->globalNamedVariable.find("__prog_tls");
            if (it != compiler->globalNamedVariable.end()) progTlsGlobal = it->second;
        }
        if (!progTlsGlobal)
        {
            compiler->LogError(std::format(
                "program '{}': __prog_tls not found - cruntime.cb must be imported", name));
            return;
        }
        auto* progTlsType        = llvm::StructType::getTypeByName(*compiler->context, "__ProgramTLS");
        auto* hookFnPtrType      = progTlsType->getElementType(kPTLS_stdout_hook);
        auto* stdinHookFnPtrType = progTlsType->getElementType(kPTLS_stdin_hook);

        // Cast trampoline to the expected function pointer type: int(*)(void*)
        auto* trampolineFnTy = llvm::FunctionType::get(i32Type, {voidPtrType}, false);

        // SEH filter: always return EXCEPTION_EXECUTE_HANDLER (1) - catch everything.
        // Emitted once per module (deduped by name). Uses an isolated IRBuilder so the
        // main builder's insertion point is not disturbed.
        llvm::Function* sehFilterFn = compiler->module->getFunction("__cflat_seh_filter_always");
        if (!sehFilterFn)
        {
            auto* filterTy = llvm::FunctionType::get(i32Type, {voidPtrType, voidPtrType}, false);
            sehFilterFn = llvm::Function::Create(
                filterTy, llvm::Function::InternalLinkage,
                "__cflat_seh_filter_always", *compiler->module);
            auto* fEntry = llvm::BasicBlock::Create(*compiler->context, "entry", sehFilterFn);
            llvm::IRBuilder<> fb(fEntry);
            fb.CreateRet(fb.getInt32(1));
        }

        // ======================================================================
        // EMIT TRAMPOLINE: int __program_run_Name(void* ctx)
        // Runs on the spawned thread. Sets up allocator, calls main, stores
        // exitCode and allocator pointer into self, frees the args packet,
        // returns main's result. Allocator cleanup happens in ~Name().
        // ======================================================================
        {
            LLVMBackend::TypeAndValue intReturn;   intReturn.TypeName = "int";
            LLVMBackend::DeclTypeAndValue ctxParam;
            ctxParam.TypeName = "void";  ctxParam.VariableName = "ctx";  ctxParam.Pointer = true;
            auto* trampolineFn = compiler->CreateFunctionDefinition(
                "__program_run_" + name, intReturn, {ctxParam}, false, false, 0, false, false, false);
            compiler->programTable[name].TrampolineFunction = trampolineFn;

            // Install Windows SEH personality so hardware faults in main() are caught.
            // Win32 does NOT set a personality: LLVM's x86 backend drops the catch handler
            // body when using _except_handler3 + catchpad/catchret, leaving broken EH tables.
            // Win32 crash recovery is skipped in the test (guarded with if const __PLATFORM__).
            if (compiler->platformValue == 64)
            {
                llvm::Function* cshFn = compiler->module->getFunction("__C_specific_handler");
                if (!cshFn)
                {
                    auto* cshTy = llvm::FunctionType::get(i32Type, /*isVarArg=*/true);
                    cshFn = llvm::cast<llvm::Function>(
                        compiler->module->getOrInsertFunction("__C_specific_handler", cshTy).getCallee());
                    cshFn->setDLLStorageClass(llvm::GlobalValue::DLLImportStorageClass);
                }
                trampolineFn->setPersonalityFn(cshFn);
            }

            auto* ctxArg = trampolineFn->getArg(0);

            // Cast void* ctx to __RunArgs_Name*
            auto* argsPacket = compiler->builder->CreateBitCast(
                ctxArg, runArgsType->getPointerTo(), "args_packet");

            // Load self (Name*) from field 0
            auto* selfGEP  = compiler->builder->CreateStructGEP(runArgsType, argsPacket, 0, "self_gep");
            auto* self     = compiler->builder->CreateLoad(progPtrType, selfGEP, "self");

            // Pointer to list__string (field 1) - passed by value to main
            auto* argsGEP  = compiler->builder->CreateStructGEP(runArgsType, argsPacket, 1, "args_gep");

            // Load self->_allocator (IAllocator fat-ptr); user may have set it before run().
            auto* allocFieldGEP = compiler->builder->CreateStructGEP(
                progType, self, allocatorIdx, "alloc_field_gep");
            auto* existingFatPtr = compiler->builder->CreateLoad(fatTy, allocFieldGEP, "existing_alloc");

            // Check data ptr (field 1): null means user left _allocator unset → use default.
            auto* existingDataPtr = compiler->builder->CreateExtractValue(existingFatPtr, {1u}, "existing_data");
            auto* isNull = compiler->builder->CreateICmpEQ(
                existingDataPtr,
                llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(voidPtrType)),
                "alloc_is_null");

            auto* defaultBlock = llvm::BasicBlock::Create(*compiler->context, "alloc_default", trampolineFn);
            auto* useBlock     = llvm::BasicBlock::Create(*compiler->context, "alloc_use",     trampolineFn);
            compiler->builder->CreateCondBr(isNull, defaultBlock, useBlock);

            // Default path: create a MallocAllocator on the heap and build its IAllocator fat-ptr.
            compiler->builder->SetInsertPoint(defaultBlock);
            auto* defAllocSize = compiler->GetTypeSizeBytes(defAllocType);
            auto* defAllocRaw  = compiler->builder->CreateCall(
                mallocFn->getFunctionType(), mallocFn, {defAllocSize}, "def_alloc_raw");
            auto* defAllocPtr  = compiler->builder->CreateBitCast(defAllocRaw, defAllocPtrTy, "def_alloc_ptr");
            auto* defAllocInit = compiler->builder->CreateCall(
                defAllocCtorFn->getFunctionType(), defAllocCtorFn, {}, "def_alloc_init");
            compiler->builder->CreateStore(defAllocInit, defAllocPtr);
            auto* defVtable    = compiler->GetOrCreateVTable("MallocAllocator", "IAllocator");
            auto* defFatPtr    = compiler->BuildInterfaceFatValue(defVtable, defAllocPtr);
            compiler->builder->CreateStore(defFatPtr, allocFieldGEP);
            compiler->builder->CreateBr(useBlock);

            // Merge: load the (possibly just-written) fat-ptr from self->_allocator.
            compiler->builder->SetInsertPoint(useBlock);
            auto* activeFatPtr = compiler->builder->CreateLoad(fatTy, allocFieldGEP, "active_alloc");

            // Set thread-local __active_allocator to the IAllocator fat-ptr.
            auto* activeAllocGlobal = compiler->globalNamedVariable["__active_allocator"];
            compiler->builder->CreateStore(activeFatPtr, activeAllocGlobal);

            // Install stdout hook: load self->onStdout and store into __prog_tls.stdout_hook
            {
                auto* onStdoutGEP = compiler->builder->CreateStructGEP(
                    progType, self, onStdoutIdx, "on_stdout_gep");
                auto* onStdoutVal = compiler->builder->CreateLoad(
                    hookFnPtrType, onStdoutGEP, "on_stdout_fn");
                auto* stdoutHookGEP = compiler->builder->CreateStructGEP(
                    progTlsType, progTlsGlobal, kPTLS_stdout_hook, "stdout_hook_gep");
                compiler->builder->CreateStore(onStdoutVal, stdoutHookGEP);
            }

            // Install stdin hook: load self->onStdin and store into __prog_tls.stdin_hook
            {
                auto* onStdinGEP = compiler->builder->CreateStructGEP(
                    progType, self, onStdinIdx, "on_stdin_gep");
                auto* onStdinVal = compiler->builder->CreateLoad(
                    stdinHookFnPtrType, onStdinGEP, "on_stdin_fn");
                auto* stdinHookGEP = compiler->builder->CreateStructGEP(
                    progTlsType, progTlsGlobal, kPTLS_stdin_hook, "stdin_hook_gep");
                compiler->builder->CreateStore(onStdinVal, stdinHookGEP);
            }

            // Install stdin return hook: load self->onStdinReturn and store into __prog_tls.stdin_return_hook
            {
                auto* onStdinReturnGEP = compiler->builder->CreateStructGEP(
                    progType, self, onStdinReturnIdx, "on_stdin_return_gep");
                auto* stdinReturnHookType = progTlsType->getElementType(kPTLS_stdin_return_hook);
                auto* onStdinReturnVal = compiler->builder->CreateLoad(
                    stdinReturnHookType, onStdinReturnGEP, "on_stdin_return_fn");
                auto* stdinReturnHookGEP = compiler->builder->CreateStructGEP(
                    progTlsType, progTlsGlobal, kPTLS_stdin_return_hook, "stdin_return_hook_gep");
                compiler->builder->CreateStore(onStdinReturnVal, stdinReturnHookGEP);
            }

            // Eagerly init cached_stdin and activate the stdin fast path.
            // This pre-populates __prog_tls.cached_stdin so fgets avoids a lazy-init branch on every call,
            // and sets stdin_active so fgets can use a single-field guard instead of three separate checks.
            {
                auto* ioFuncTy  = llvm::FunctionType::get(voidPtrType, {i32Type}, false);
                auto* ioFuncFn  = compiler->module->getOrInsertFunction("__acrt_iob_func", ioFuncTy).getCallee();
                auto* stdinPtr  = compiler->builder->CreateCall(
                    llvm::cast<llvm::Function>(ioFuncFn)->getFunctionType(), ioFuncFn,
                    {llvm::ConstantInt::get(i32Type, 0)}, "stdin_ptr");
                auto* cachedStdinGEP = compiler->builder->CreateStructGEP(
                    progTlsType, progTlsGlobal, kPTLS_cached_stdin, "cached_stdin_gep");
                compiler->builder->CreateStore(stdinPtr, cachedStdinGEP);
                auto* stdinActiveGEP = compiler->builder->CreateStructGEP(
                    progTlsType, progTlsGlobal, kPTLS_stdin_active, "stdin_active_gep");
                compiler->builder->CreateStore(llvm::ConstantInt::getTrue(*compiler->context), stdinActiveGEP);
            }

            // Enable handle tracker: load self->trackHandles and store into __prog_tls.handle_tracker_enabled
            {
                auto* trackGEP = compiler->builder->CreateStructGEP(
                    progType, self, trackHandlesIdx, "track_handles_gep");
                // Field is int (i32) to avoid i1-in-ConstantStruct LLVM assertion; convert to i1 for the TLS field
                auto* trackI32 = compiler->builder->CreateLoad(
                    llvm::Type::getInt32Ty(*compiler->context), trackGEP, "track_handles_i32");
                auto* trackI1  = compiler->builder->CreateICmpNE(
                    trackI32,
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(*compiler->context), 0),
                    "track_handles_val");
                auto* enabledGEP = compiler->builder->CreateStructGEP(
                    progTlsType, progTlsGlobal, kPTLS_handle_tracker_enabled, "htrack_enabled_gep");
                compiler->builder->CreateStore(trackI1, enabledGEP);
            }

            auto* cleanupBB = llvm::BasicBlock::Create(*compiler->context, "seh_cleanup", trampolineFn);

            // Load list__string args by value from the packet
            llvm::Value* argsVal = compiler->builder->CreateLoad(listStringType, argsGEP, "args_val");

            // Re-home the args into the program allocator (which is now the active
            // allocator). The caller built the args under its own allocator (CRT on
            // the main thread); main(move list<string>) would otherwise free those
            // foreign buffers under the program allocator. __prog_adopt_args deep-
            // copies them into the program allocator and frees the originals under
            // the CRT, so main owns a list the program allocator actually allocated.
            // Only the list<string> form owns the list; the argc/argv form reads the
            // string pointers into a separate argv array, so it is left untouched.
            if (isListArgs)
            {
                auto* adoptFn = compiler->GetFunction("__prog_adopt_args");
                if (adoptFn)
                    argsVal = compiler->builder->CreateCall(
                        adoptFn->getFunctionType(), adoptFn, {argsVal}, "adopted_args");
                else
                    compiler->LogError(std::format(
                        "program '{}': __prog_adopt_args not found - program.cb must be imported", name));
            }

            // exitCodeGEP must dominate all paths - compute in the entry block.
            auto* exitCodeGEP = compiler->builder->CreateStructGEP(
                progType, self, exitCodeIdx, "exit_code_gep");

            // ---- ArgcArgv conversion: list<string> → argc + char** argv ----
            // Used only when main(int argc, char** argv) style is detected.
            // argvHolderAlloca stores the malloc'd argv array so cleanupBB can free it.
            llvm::AllocaInst* argvHolderAlloca = nullptr;
            llvm::Value* argc32Val  = nullptr;
            llvm::Value* argvPtrVal = nullptr;

            if (isArgcArgv)
            {
                // Alloca to hold argv ptr across SEH paths; initialized to null.
                argvHolderAlloca = compiler->AllocaAtEntry(voidPtrType, nullptr, "argv_holder");
                compiler->builder->CreateStore(
                    llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(voidPtrType)),
                    argvHolderAlloca);

                // list<string> layout: { string* _data, i32 _size, i32 _capacity }
                argc32Val        = compiler->builder->CreateExtractValue(argsVal, {1u}, "argc");
                auto* dataPtr    = compiler->builder->CreateExtractValue(argsVal, {0u}, "args_data");

                // malloc char*[argc+1]
                auto* argcPlus1    = compiler->builder->CreateAdd(argc32Val, compiler->builder->getInt32(1), "argc_p1");
                auto* argcPlus1_64 = compiler->builder->CreateZExt(argcPlus1, i64Type, "argc_p1_64");
                auto* ptrSize      = compiler->GetTypeSizeBytes(voidPtrType);
                auto* argvBytes    = compiler->builder->CreateMul(argcPlus1_64, ptrSize, "argv_bytes");
                auto* argvRaw      = compiler->builder->CreateCall(
                    mallocFn->getFunctionType(), mallocFn, {argvBytes}, "argv_raw");
                compiler->builder->CreateStore(argvRaw, argvHolderAlloca);
                argvPtrVal = compiler->builder->CreateBitCast(
                    argvRaw, voidPtrType->getPointerTo(), "argv_ptr");

                // Loop: argv[i] = data[i]._ptr  (string field 0 is i8* _ptr)
                auto* iAlloca   = compiler->AllocaAtEntry(i32Type, nullptr, "argv_i");
                compiler->builder->CreateStore(compiler->builder->getInt32(0), iAlloca);
                auto* loopCondBB = llvm::BasicBlock::Create(*compiler->context, "argv_cond", trampolineFn);
                auto* loopBodyBB = llvm::BasicBlock::Create(*compiler->context, "argv_body", trampolineFn);
                auto* loopDoneBB = llvm::BasicBlock::Create(*compiler->context, "argv_done", trampolineFn);
                compiler->builder->CreateBr(loopCondBB);

                compiler->builder->SetInsertPoint(loopCondBB);
                auto* iVal  = compiler->builder->CreateLoad(i32Type, iAlloca, "i");
                auto* check = compiler->builder->CreateICmpSLT(iVal, argc32Val, "loop_cond");
                compiler->builder->CreateCondBr(check, loopBodyBB, loopDoneBB);

                compiler->builder->SetInsertPoint(loopBodyBB);
                auto* i64Val    = compiler->builder->CreateZExt(iVal, i64Type, "i64");
                auto* elemPtr   = compiler->builder->CreateGEP(stringStructType, dataPtr, {i64Val}, "elem");
                auto* ptrFldGEP = compiler->builder->CreateStructGEP(stringStructType, elemPtr, 0, "elem_ptr");
                auto* charPtr   = compiler->builder->CreateLoad(voidPtrType, ptrFldGEP, "char_ptr");
                auto* argvSlot  = compiler->builder->CreateGEP(voidPtrType, argvPtrVal, {i64Val}, "argv_slot");
                compiler->builder->CreateStore(charPtr, argvSlot);
                auto* iNext = compiler->builder->CreateAdd(iVal, compiler->builder->getInt32(1), "i_next");
                compiler->builder->CreateStore(iNext, iAlloca);
                compiler->builder->CreateBr(loopCondBB);

                compiler->builder->SetInsertPoint(loopDoneBB);
                // Null-terminate: argv[argc] = nullptr
                auto* argc64    = compiler->builder->CreateZExt(argc32Val, i64Type, "argc64");
                auto* nullSlot  = compiler->builder->CreateGEP(voidPtrType, argvPtrVal, {argc64}, "null_slot");
                compiler->builder->CreateStore(
                    llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(voidPtrType)),
                    nullSlot);
            }

            // Build the argument list for the call/invoke based on main style.
            // Imported programs are free functions (no self pointer).
            std::vector<llvm::Value*> mainArgs;
            if (isImported)
            {
                if (isNoArgs)
                    mainArgs = {};
                else if (isListArgs)
                    mainArgs = {argsVal};
                else // ArgcArgv
                    mainArgs = {argc32Val, argvPtrVal};
            }
            else
            {
                if (isNoArgs)
                    mainArgs = {self};
                else if (isListArgs)
                    mainArgs = {self, argsVal};
                else // ArgcArgv
                    mainArgs = {self, argc32Val, argvPtrVal};
            }

            if (compiler->platformValue == 64)
            {
                // Win64: use SEH (invoke + catchswitch + catchpad) to catch hardware faults.
                auto* normalBB   = llvm::BasicBlock::Create(*compiler->context, "main_normal",  trampolineFn);
                auto* dispatchBB = llvm::BasicBlock::Create(*compiler->context, "seh_dispatch", trampolineFn);
                auto* catchBB    = llvm::BasicBlock::Create(*compiler->context, "seh_catch",    trampolineFn);

                // noinline: prevents the optimizer from inlining main() into this trampoline,
                // which would move null-dereference faults outside the invoke's protected region.
                mainFn->addFnAttr(llvm::Attribute::NoInline);

                // Invoke main() - normal return lands in normalBB, any fault unwinds to dispatchBB.
                auto* invokeInst = compiler->builder->CreateInvoke(
                    mainFn->getFunctionType(), mainFn,
                    normalBB, dispatchBB,
                    mainArgs, "main_result");

                // normalBB: main returned cleanly - store exit code, fall through to cleanup.
                compiler->builder->SetInsertPoint(normalBB);
                compiler->builder->CreateStore(invokeInst, exitCodeGEP);
                compiler->builder->CreateBr(cleanupBB);

                // dispatchBB: catchswitch routes all exceptions to catchBB.
                compiler->builder->SetInsertPoint(dispatchBB);
                auto* catchSwitch = compiler->builder->CreateCatchSwitch(
                    llvm::ConstantTokenNone::get(*compiler->context),
                    nullptr, 1, "cs");
                catchSwitch->addHandler(catchBB);

                // catchBB: catch everything (filter returns 1), store sentinel -1, rejoin cleanup.
                compiler->builder->SetInsertPoint(catchBB);
                auto* catchPad = compiler->builder->CreateCatchPad(
                    catchSwitch, {static_cast<llvm::Value*>(sehFilterFn)}, "cp");
                compiler->builder->CreateStore(
                    llvm::ConstantInt::get(i32Type, static_cast<uint64_t>(-1), /*isSigned=*/true),
                    exitCodeGEP);
                compiler->builder->CreateCatchRet(catchPad, cleanupBB);
            }
            else
            {
                // Win32: LLVM's x86 backend drops the catch-handler body when using
                // _except_handler3 + catchpad/catchret, producing broken EH tables.
                // Fall back to a plain call - crash recovery is not supported on Win32.
                auto* callResult = compiler->builder->CreateCall(
                    mainFn->getFunctionType(), mainFn, mainArgs, "main_result");
                compiler->builder->CreateStore(callResult, exitCodeGEP);
                compiler->builder->CreateBr(cleanupBB);
            }

            // cleanupBB: shared teardown - both normal and exception paths converge here.
            compiler->builder->SetInsertPoint(cleanupBB);
            compiler->builder->CreateStore(llvm::Constant::getNullValue(fatTy), activeAllocGlobal);
            {
                auto* stdoutHookGEP = compiler->builder->CreateStructGEP(
                    progTlsType, progTlsGlobal, kPTLS_stdout_hook, "stdout_hook_gep");
                compiler->builder->CreateStore(
                    llvm::Constant::getNullValue(hookFnPtrType), stdoutHookGEP);
                auto* stdinHookGEP = compiler->builder->CreateStructGEP(
                    progTlsType, progTlsGlobal, kPTLS_stdin_hook, "stdin_hook_gep");
                compiler->builder->CreateStore(
                    llvm::Constant::getNullValue(stdinHookFnPtrType), stdinHookGEP);
                auto* stdinActiveGEP = compiler->builder->CreateStructGEP(
                    progTlsType, progTlsGlobal, kPTLS_stdin_active, "stdin_active_gep");
                compiler->builder->CreateStore(
                    llvm::ConstantInt::getFalse(*compiler->context), stdinActiveGEP);
            }

            // Handle tracker cleanup: disable tracker first (so fclose won't re-enter the list),
            // then walk the linked list and fclose any handles still open from a crash.
            {
                auto* enabledGEP = compiler->builder->CreateStructGEP(
                    progTlsType, progTlsGlobal, kPTLS_handle_tracker_enabled, "htrack_enabled_gep");
                compiler->builder->CreateStore(
                    llvm::ConstantInt::getFalse(*compiler->context), enabledGEP);
                auto* headFieldGEP = compiler->builder->CreateStructGEP(
                    progTlsType, progTlsGlobal, kPTLS_handle_tracker_head, "htrack_head_field_gep");

                auto* fcloseTy = llvm::FunctionType::get(i32Type, {voidPtrType}, false);
                auto* fcloseFn = compiler->module->getOrInsertFunction("fclose", fcloseTy).getCallee();

                auto* nodeTy = llvm::StructType::getTypeByName(*compiler->context, "__HandleNode");

                auto* htrackLoopBB  = llvm::BasicBlock::Create(*compiler->context, "htrack_loop",  trampolineFn);
                auto* htrackBodyBB  = llvm::BasicBlock::Create(*compiler->context, "htrack_body",  trampolineFn);
                auto* htrackCloseBB = llvm::BasicBlock::Create(*compiler->context, "htrack_close", trampolineFn);
                auto* htrackSkipBB  = llvm::BasicBlock::Create(*compiler->context, "htrack_skip",  trampolineFn);
                auto* htrackDoneBB  = llvm::BasicBlock::Create(*compiler->context, "htrack_done",  trampolineFn);
                compiler->builder->CreateBr(htrackLoopBB);

                // Loop header: load head, exit if null
                compiler->builder->SetInsertPoint(htrackLoopBB);
                auto* headVal  = compiler->builder->CreateLoad(voidPtrType, headFieldGEP, "htrack_head");
                auto* headNull = compiler->builder->CreateICmpEQ(
                    headVal,
                    llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(voidPtrType)),
                    "htrack_head_null");
                compiler->builder->CreateCondBr(headNull, htrackDoneBB, htrackBodyBB);

                // Loop body: load handle and next from node
                compiler->builder->SetInsertPoint(htrackBodyBB);
                auto* handleGEP = compiler->builder->CreateStructGEP(nodeTy, headVal, 0, "htrack_handle_gep");
                auto* handleVal = compiler->builder->CreateLoad(voidPtrType, handleGEP, "htrack_handle");
                auto* nextGEP   = compiler->builder->CreateStructGEP(nodeTy, headVal, 1, "htrack_next_gep");
                auto* nextVal   = compiler->builder->CreateLoad(voidPtrType, nextGEP, "htrack_next");
                auto* handleNull = compiler->builder->CreateICmpEQ(
                    handleVal,
                    llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(voidPtrType)),
                    "htrack_handle_null");
                compiler->builder->CreateCondBr(handleNull, htrackSkipBB, htrackCloseBB);

                // Close the handle
                compiler->builder->SetInsertPoint(htrackCloseBB);
                compiler->builder->CreateCall(fcloseTy, fcloseFn, {handleVal});
                compiler->builder->CreateBr(htrackSkipBB);

                // Advance head, free current node, continue loop
                compiler->builder->SetInsertPoint(htrackSkipBB);
                compiler->builder->CreateStore(nextVal, headFieldGEP);
                compiler->builder->CreateCall(freeFn->getFunctionType(), freeFn, {headVal});
                compiler->builder->CreateBr(htrackLoopBB);

                compiler->builder->SetInsertPoint(htrackDoneBB);
            }

            // Free argv array if ArgcArgv style (may be null on exception path before allocation).
            if (isArgcArgv && argvHolderAlloca)
            {
                auto* argvToFree = compiler->builder->CreateLoad(voidPtrType, argvHolderAlloca, "argv_to_free");
                compiler->builder->CreateCall(freeFn->getFunctionType(), freeFn, {argvToFree});
            }

            compiler->builder->CreateCall(freeFn->getFunctionType(), freeFn, {ctxArg});
            auto* finalExitCode = compiler->builder->CreateLoad(i32Type, exitCodeGEP, "final_exit");
            compiler->CreateReturnCall(finalExitCode);
            compiler->CreateBlockBreak(nullptr, true);
        }

        // ======================================================================
        // EMIT run(): bool run(Name* this, list__string args)
        // Allocates args packet, spawns thread into self->_thread, returns
        // whether the thread started. Does NOT join - caller uses WaitForExit().
        // ======================================================================
        {
            LLVMBackend::TypeAndValue boolReturn;  boolReturn.TypeName = "bool";
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName = name;  thisParam.VariableName = name + "__";  thisParam.Pointer = true;
            LLVMBackend::DeclTypeAndValue argsParam;
            argsParam.TypeName = "list__string";  argsParam.VariableName = "args";
            argsParam.IsMove = true;  // run() takes ownership; caller's list is zeroed after the call

            auto* runFn = compiler->CreateFunctionDefinition("run", boolReturn, {thisParam, argsParam});
            compiler->programTable[name].RunFunction = runFn;

            auto* thisArg = runFn->getArg(0);   // Name*
            auto* argsArg = runFn->getArg(1);   // list__string by value (move)

            // Malloc the args packet (raw malloc - tracked alloc is per-thread)
            auto* pkgSize = compiler->GetTypeSizeBytes(runArgsType);
            auto* pkgRaw  = compiler->builder->CreateCall(
                mallocFn->getFunctionType(), mallocFn, {pkgSize}, "pkg_raw");
            auto* pkg = compiler->builder->CreateBitCast(pkgRaw, runArgsType->getPointerTo(), "pkg");

            // Store this -> pkg->self (field 0)
            auto* selfGEP = compiler->builder->CreateStructGEP(runArgsType, pkg, 0, "pkg_self_gep");
            compiler->builder->CreateStore(thisArg, selfGEP);

            // Store args -> pkg->args (field 1): use the original argument value (pre-alloca copy)
            auto* argsGEP = compiler->builder->CreateStructGEP(runArgsType, pkg, 1, "pkg_args_gep");
            compiler->builder->CreateStore(argsArg, argsGEP);

            // Zero run()'s args alloca so ~list__string is a no-op at scope exit.
            // Ownership of _data transfers to the packet; trampoline frees the packet on completion.
            {
                auto& runArgNV = compiler->stackNamedVariable.back().functionArgument["args"];
                if (runArgNV.Storage != nullptr)
                    compiler->builder->CreateStore(
                        llvm::ConstantAggregateZero::get(listStringType), runArgNV.Storage);
            }

            // Init _stop_source before spawning - gives main() a live token to check
            if (stopSrcInitFn && stopSrcType)
            {
                auto* stopSrcGEP = compiler->builder->CreateStructGEP(
                    progType, thisArg, stopSrcIdx, "stop_src_gep");
                compiler->builder->CreateCall(
                    stopSrcInitFn->getFunctionType(), stopSrcInitFn, {stopSrcGEP});
            }

            // Get &self->_thread (stored field; initialized by the program ctor)
            auto* threadFieldGEP = compiler->builder->CreateStructGEP(
                progType, thisArg, threadIdx, "thread_field");

            // Thread.start(&self->_thread, trampoline, pkg) -> bool
            // Thread.start() takes function<int(void*)> as a fat struct {i8*, i8*}.
            auto* trampolineFn  = compiler->programTable[name].TrampolineFunction;
            auto* trampolineFat = compiler->WrapBareValueAsFatStruct(trampolineFn);
            auto* startResult = compiler->builder->CreateCall(
                threadStartFn->getFunctionType(), threadStartFn,
                {threadFieldGEP, trampolineFat, pkgRaw}, "start_result");

            // On start failure: free pkg, return false
            auto* successBlock = llvm::BasicBlock::Create(*compiler->context, "start_ok",   runFn);
            auto* failBlock    = llvm::BasicBlock::Create(*compiler->context, "start_fail", runFn);
            compiler->builder->CreateCondBr(startResult, successBlock, failBlock);

            compiler->builder->SetInsertPoint(failBlock);
            compiler->builder->CreateCall(freeFn->getFunctionType(), freeFn, {pkgRaw});
            compiler->builder->CreateRet(compiler->builder->getFalse());

            // On start success: return true (trampoline owns pkg from here)
            compiler->builder->SetInsertPoint(successBlock);
            compiler->builder->CreateRet(compiler->builder->getTrue());

            compiler->CreateBlockBreak(nullptr, true);
        }

        // ======================================================================
        // EMIT WaitForExit(): void WaitForExit(Name* this)
        // Blocks until the program thread exits. exitCode field is readable after.
        // ======================================================================
        {
            LLVMBackend::TypeAndValue voidReturn;  voidReturn.TypeName = "void";
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName = name;  thisParam.VariableName = name + "__";  thisParam.Pointer = true;

            compiler->CreateFunctionDefinition("WaitForExit", voidReturn, {thisParam});

            auto* thisArg = compiler->builder->GetInsertBlock()->getParent()->getArg(0);

            // Get &self->_thread and join
            auto* threadFieldGEP = compiler->builder->CreateStructGEP(
                progType, thisArg, threadIdx, "thread_field");
            compiler->builder->CreateCall(
                threadJoinFn->getFunctionType(), threadJoinFn, {threadFieldGEP});

            compiler->CreateReturnCall(nullptr);
            compiler->CreateBlockBreak(nullptr, true);
        }

        // ======================================================================
        // EMIT WaitForExit(stop_token): bool WaitForExit(Name* this, stop_token token)
        // Polls until the thread exits or the token is cancelled.
        // Returns true if thread exited; false if cancelled (thread NOT joined).
        // ======================================================================
        {
            auto* stopTokenType = compiler->dataStructures.count("stop_token")
                                  ? compiler->dataStructures["stop_token"].StructType : nullptr;
            auto* waitOrStopFn  = stopTokenType
                                  ? FindMethodOf("__wait_thread_or_stop", "Thread") : nullptr;

            if (stopTokenType && waitOrStopFn)
            {
                LLVMBackend::TypeAndValue boolReturn;   boolReturn.TypeName = "bool";
                LLVMBackend::DeclTypeAndValue thisParam;
                thisParam.TypeName = name;  thisParam.VariableName = name + "__";  thisParam.Pointer = true;
                LLVMBackend::DeclTypeAndValue tokenParam;
                tokenParam.TypeName = "stop_token";  tokenParam.VariableName = "token";

                auto* waitFn = compiler->CreateFunctionDefinition("WaitForExit", boolReturn, {thisParam, tokenParam});

                auto* thisArg  = waitFn->getArg(0);
                auto* tokenArg = waitFn->getArg(1);

                auto* threadFieldGEP = compiler->builder->CreateStructGEP(
                    progType, thisArg, threadIdx, "thread_field");

                auto* result = compiler->builder->CreateCall(
                    waitOrStopFn->getFunctionType(), waitOrStopFn,
                    {threadFieldGEP, tokenArg}, "wait_result");

                compiler->builder->CreateRet(result);
                compiler->CreateBlockBreak(nullptr, true);
            }
        }

        // ======================================================================
        // EMIT WaitForExit(int): bool WaitForExit(Name* this, int timeoutMs)
        // Single try_join call with the given timeout. Returns true if the thread
        // exited within the timeout; false if still running (handles intact).
        // ======================================================================
        {
            auto* threadTryJoinFn = FindMethodOf("try_join", "Thread");
            if (threadTryJoinFn)
            {
                LLVMBackend::TypeAndValue boolReturn;  boolReturn.TypeName = "bool";
                LLVMBackend::DeclTypeAndValue thisParam;
                thisParam.TypeName = name;  thisParam.VariableName = name + "__";  thisParam.Pointer = true;
                LLVMBackend::DeclTypeAndValue msParam;
                msParam.TypeName = "int";  msParam.VariableName = "timeoutMs";

                auto* waitFn = compiler->CreateFunctionDefinition("WaitForExit", boolReturn, {thisParam, msParam});

                auto* thisArg = waitFn->getArg(0);
                auto* msArg   = waitFn->getArg(1);

                auto* threadFieldGEP = compiler->builder->CreateStructGEP(
                    progType, thisArg, threadIdx, "thread_field");

                auto* result = compiler->builder->CreateCall(
                    threadTryJoinFn->getFunctionType(), threadTryJoinFn,
                    {threadFieldGEP, msArg}, "try_join_result");

                compiler->builder->CreateRet(result);
                compiler->CreateBlockBreak(nullptr, true);
            }
        }

        // ======================================================================
        // EMIT RequestStop(): void RequestStop(Name* this)
        // Signals the program's _stop_source so main() can observe it via
        // _stop_source.get_token().stop_requested(). Cooperative - main() must check.
        // ======================================================================
        {
            if (stopSrcRequestStopFn && stopSrcType)
            {
                LLVMBackend::TypeAndValue voidReturn;  voidReturn.TypeName = "void";
                LLVMBackend::DeclTypeAndValue thisParam;
                thisParam.TypeName = name;  thisParam.VariableName = name + "__";  thisParam.Pointer = true;

                compiler->CreateFunctionDefinition("RequestStop", voidReturn, {thisParam});

                auto* thisArg = compiler->builder->GetInsertBlock()->getParent()->getArg(0);

                auto* stopSrcGEP = compiler->builder->CreateStructGEP(
                    progType, thisArg, stopSrcIdx, "stop_src_gep");
                compiler->builder->CreateCall(
                    stopSrcRequestStopFn->getFunctionType(), stopSrcRequestStopFn, {stopSrcGEP});

                compiler->CreateReturnCall(nullptr);
                compiler->CreateBlockBreak(nullptr, true);
            }
        }

        // ======================================================================
        // EMIT Kill(): void Kill(Name* this)
        // Signals RequestStop() first (cooperative), then forcibly terminates
        // via TerminateThread. Leaks allocator state and thread-held resources.
        // ======================================================================
        {
            auto* threadTerminateFn = FindMethodOf("terminate", "Thread");
            if (threadTerminateFn)
            {
                LLVMBackend::TypeAndValue voidReturn;  voidReturn.TypeName = "void";
                LLVMBackend::DeclTypeAndValue thisParam;
                thisParam.TypeName = name;  thisParam.VariableName = name + "__";  thisParam.Pointer = true;

                compiler->CreateFunctionDefinition("Kill", voidReturn, {thisParam});

                auto* thisArg = compiler->builder->GetInsertBlock()->getParent()->getArg(0);

                // Signal the stop token first - gives cooperative loops a chance to observe it.
                if (stopSrcRequestStopFn && stopSrcType)
                {
                    auto* stopSrcGEP = compiler->builder->CreateStructGEP(
                        progType, thisArg, stopSrcIdx, "stop_src_gep");
                    compiler->builder->CreateCall(
                        stopSrcRequestStopFn->getFunctionType(), stopSrcRequestStopFn, {stopSrcGEP});
                }

                auto* threadFieldGEP = compiler->builder->CreateStructGEP(
                    progType, thisArg, threadIdx, "thread_field");
                compiler->builder->CreateCall(
                    threadTerminateFn->getFunctionType(), threadTerminateFn, {threadFieldGEP});

                compiler->CreateReturnCall(nullptr);
                compiler->CreateBlockBreak(nullptr, true);
            }
        }

        // ======================================================================
        // EMIT ~Name(): void ~Name(Name* this)   [only if user didn't provide one]
        // Calls cleanup() and frees the IAllocator stored in _allocator.
        // Null-checks so it's safe to call even if run() was never called.
        // ======================================================================
        if (compiler->dataStructures[name].Destructor == nullptr)
        {
            LLVMBackend::TypeAndValue voidReturn;  voidReturn.TypeName = "void";
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName = name;  thisParam.VariableName = name + "__";  thisParam.Pointer = true;

            auto* dtorFn = compiler->CreateFunctionDefinition("~" + name, voidReturn, {thisParam});
            compiler->RegisterDestructor(name, dtorFn);

            auto* thisArg = dtorFn->getArg(0);

            // Load self->_allocator (IAllocator fat-ptr)
            auto* dtor_allocFieldGEP = compiler->builder->CreateStructGEP(
                progType, thisArg, allocatorIdx, "alloc_field_gep");
            auto* allocFatPtr = compiler->builder->CreateLoad(fatTy, dtor_allocFieldGEP, "alloc_fat_ptr");

            // if (data ptr != null) { cleanup(); free(data); zero _allocator; }
            auto* allocDataPtr = compiler->builder->CreateExtractValue(allocFatPtr, {1u}, "alloc_data");
            auto* isNotNull    = compiler->builder->CreateICmpNE(
                allocDataPtr,
                llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(voidPtrType)),
                "alloc_not_null");
            auto* cleanupBlock = llvm::BasicBlock::Create(*compiler->context, "alloc_cleanup", dtorFn);
            auto* doneBlock    = llvm::BasicBlock::Create(*compiler->context, "dtor_done",     dtorFn);
            compiler->builder->CreateCondBr(isNotNull, cleanupBlock, doneBlock);

            compiler->builder->SetInsertPoint(cleanupBlock);
            // alloca a fat-ptr slot so CallInterfaceMethod can GEP into it
            auto* allocFatPtrSlot = compiler->builder->CreateAlloca(fatTy, nullptr, "alloc_fat_slot");
            compiler->builder->CreateStore(allocFatPtr, allocFatPtrSlot);
            compiler->CallInterfaceMethod(allocFatPtrSlot, "IAllocator", "cleanup", {});
            // free the underlying memory (data ptr is already in allocDataPtr)
            compiler->builder->CreateCall(freeFn->getFunctionType(), freeFn, {allocDataPtr});
            compiler->builder->CreateStore(llvm::Constant::getNullValue(fatTy), dtor_allocFieldGEP);
            compiler->builder->CreateBr(doneBlock);

            compiler->builder->SetInsertPoint(doneBlock);

            // Dispose _stop_source (no-op if _state is null, safe to call after Kill())
            if (stopSrcDisposeFn && stopSrcType)
            {
                auto* stopSrcGEP = compiler->builder->CreateStructGEP(
                    progType, thisArg, stopSrcIdx, "stop_src_gep");
                compiler->builder->CreateCall(
                    stopSrcDisposeFn->getFunctionType(), stopSrcDisposeFn, {stopSrcGEP});
            }

            compiler->CreateReturnCall(nullptr);
            compiler->CreateBlockBreak(nullptr, true);
        }

        // ======================================================================
        // EMIT exitCode(): int exitCode(Name* this)
        // Returns the exitCode field - satisfies the IProcess interface contract.
        // ======================================================================
        {
            LLVMBackend::TypeAndValue intReturn;  intReturn.TypeName = "int";
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName = name;  thisParam.VariableName = name + "__";  thisParam.Pointer = true;

            auto* exitCodeFn = compiler->CreateFunctionDefinition("exitCode", intReturn, {thisParam});

            auto* thisArg = exitCodeFn->getArg(0);
            auto* exitCodeGEP = compiler->builder->CreateStructGEP(
                progType, thisArg, exitCodeIdx, "exit_code_gep");
            auto* exitCodeVal = compiler->builder->CreateLoad(i32Type, exitCodeGEP, "exit_code");
            compiler->builder->CreateRet(exitCodeVal);
            compiler->CreateBlockBreak(nullptr, true);
        }
    }

    void ParseImportedProgramDefinition(const std::string& name)
    {
        auto* compiler = compilerLLVM;

        if (compiler->IsVerbose())
            std::cout << "[verbose]     parse imported program: " << name << "\n";

        // Ensure channel<IMessage> is fully instantiated before CreateStructType uses its layout.
        if (!instantiatedGenerics.count("channel__IMessage"))
        {
            pendingInstantiations.push_back({"channel", {"IMessage"}, "channel__IMessage"});
            instantiatedGenerics.insert("channel__IMessage");
            ProcessPendingInstantiations();
        }

        if (!instantiatedInterfaces.count("IQueue__IMessage"))
        {
            pendingInstantiations.push_back({"IQueue", {"IMessage"}, "IQueue__IMessage"});
            ProcessPendingInstantiations();
        }

        // No user-declared fields - only synthetic fields
        std::vector<LLVMBackend::DeclTypeAndValue> declList;

        unsigned exitCodeFieldIndex     = (unsigned)declList.size();
        unsigned threadFieldIndex       = exitCodeFieldIndex + 1;
        unsigned allocatorFieldIndex    = threadFieldIndex + 1;
        unsigned onStdoutFieldIndex      = allocatorFieldIndex + 1;
        unsigned onStdinFieldIndex       = onStdoutFieldIndex + 1;
        unsigned onStdinReturnFieldIndex = onStdinFieldIndex + 1;
        unsigned inboxFieldIndex         = onStdinReturnFieldIndex + 1;
        unsigned stopSrcFieldIndex       = inboxFieldIndex + 1;
        unsigned trackHandlesFieldIndex  = stopSrcFieldIndex + 1;
        unsigned outFieldIndex           = (unsigned)-1;
        unsigned inStreamFieldIndex      = (unsigned)-1;
        bool     hasStreamType           = compiler->dataStructures.count("stream") > 0;
        {
            LLVMBackend::DeclTypeAndValue exitCodeField;
            exitCodeField.TypeName     = "int";
            exitCodeField.VariableName = "exitCode";
            declList.push_back(exitCodeField);

            LLVMBackend::DeclTypeAndValue threadField;
            threadField.TypeName     = "Thread";
            threadField.VariableName = "_thread";
            declList.push_back(threadField);

            LLVMBackend::DeclTypeAndValue allocatorField;
            allocatorField.TypeName     = "IAllocator";
            allocatorField.VariableName = "_allocator";
            allocatorField.IsInterface  = true;
            allocatorField.Pointer      = true;
            declList.push_back(allocatorField);

            LLVMBackend::DeclTypeAndValue onStdoutField;
            onStdoutField.VariableName          = "onStdout";
            onStdoutField.IsFunctionPointer     = true;
            onStdoutField.FuncPtrReturnTypeName = "void";
            onStdoutField.FuncPtrParams         = {{"char", true}, {"int", false}};
            declList.push_back(onStdoutField);

            LLVMBackend::DeclTypeAndValue onStdinField;
            onStdinField.VariableName          = "onStdin";
            onStdinField.IsFunctionPointer     = true;
            onStdinField.FuncPtrReturnTypeName = "char";
            onStdinField.FuncPtrReturnPointer  = true;
            onStdinField.FuncPtrParams         = {};
            declList.push_back(onStdinField);

            LLVMBackend::DeclTypeAndValue onStdinReturnField;
            onStdinReturnField.VariableName          = "onStdinReturn";
            onStdinReturnField.IsFunctionPointer     = true;
            onStdinReturnField.FuncPtrReturnTypeName = "void";
            onStdinReturnField.FuncPtrParams         = {{"char", true}};
            declList.push_back(onStdinReturnField);

            LLVMBackend::DeclTypeAndValue inboxField;
            inboxField.TypeName     = "IQueue__IMessage";
            inboxField.VariableName = "inbox";
            inboxField.IsInterface  = true;
            inboxField.Pointer      = true;
            declList.push_back(inboxField);

            LLVMBackend::DeclTypeAndValue stopSrcField;
            stopSrcField.TypeName     = "stop_source";
            stopSrcField.VariableName = "_stop_source";
            declList.push_back(stopSrcField);

            LLVMBackend::DeclTypeAndValue trackHandlesField;
            trackHandlesField.TypeName     = "int";
            trackHandlesField.VariableName = "trackHandles";
            declList.push_back(trackHandlesField);

            if (hasStreamType) {
                LLVMBackend::DeclTypeAndValue outField;
                outField.TypeName     = "stream";
                outField.VariableName = "_out";
                outField.Pointer      = true;
                outFieldIndex = (unsigned)declList.size();
                declList.push_back(outField);

                LLVMBackend::DeclTypeAndValue inField;
                inField.TypeName     = "stream";
                inField.VariableName = "_in";
                inField.Pointer      = true;
                inStreamFieldIndex = (unsigned)declList.size();
                declList.push_back(inField);
            }
        }

        auto* structType = compiler->CreateStructType(name, declList);
        if (structType->isOpaque())
            structType->setBody(llvm::ArrayRef<llvm::Type*>());

        // Create default constructor
        {
            LLVMBackend::TypeAndValue returnType;
            returnType.TypeName = name;
            compiler->CreateFunctionDefinition(name, returnType, {});

            std::vector<llvm::Value*> initializers;
            for (auto& typeValue : declList)
            {
                llvm::Value* rvalue = nullptr;
                auto* initializer = typeValue.Initializer;
                if (initializer)
                {
                    if (auto* ae = initializer->assignmentExpression())
                        rvalue = ParseAssignmentExpression(ae);
                    else if (initializer->Default())
                        rvalue = GenerateDefaultValue(typeValue);
                }
                initializers.push_back(rvalue);
            }

            llvm::Value* structVal = llvm::UndefValue::get(structType);
            unsigned int idx = 0;
            for (auto* rvalue : initializers)
            {
                if (rvalue)
                {
                    auto* destType = structType->getTypeAtIndex(idx);
                    rvalue = compiler->Upconvert(rvalue, destType);
                    if (rvalue->getType() != destType && destType->isStructTy())
                    {
                        std::string fieldTypeName = declList[idx].TypeName;
                        if (compiler->GetFunction(fieldTypeName))
                            rvalue = compiler->CreateOverloadedFunctionCall(fieldTypeName, {});
                        else
                            rvalue = llvm::Constant::getNullValue(destType);
                    }
                    structVal = compiler->CreateInsertValue(structVal, rvalue, idx);
                }
                idx++;
            }

            // exitCode = -1
            {
                auto* minusOne = llvm::ConstantInt::getSigned(
                    llvm::Type::getInt32Ty(*compiler->context), -1);
                structVal = compiler->CreateInsertValue(structVal, minusOne, exitCodeFieldIndex);
            }

            // _thread = Thread()
            if (auto* threadCtorFn = compiler->GetFunction("Thread"))
            {
                auto* threadInitVal = compiler->builder->CreateCall(
                    threadCtorFn->getFunctionType(), threadCtorFn, {}, "thread_init");
                structVal = compiler->CreateInsertValue(structVal, threadInitVal, threadFieldIndex);
            }

            // _allocator = zero (null fat-ptr)
            {
                auto* fatTy = compiler->GetFatPtrType();
                structVal = compiler->CreateInsertValue(
                    structVal, llvm::Constant::getNullValue(fatTy), allocatorFieldIndex);
            }

            // onStdout = nullptr
            {
                auto& onStdoutDecl = declList[onStdoutFieldIndex];
                auto* fieldType = compiler->GetType(onStdoutDecl);
                structVal = compiler->CreateInsertValue(
                    structVal, llvm::Constant::getNullValue(fieldType), onStdoutFieldIndex);
            }

            // onStdin = nullptr
            {
                auto& onStdinDecl = declList[onStdinFieldIndex];
                auto* fieldType = compiler->GetType(onStdinDecl);
                structVal = compiler->CreateInsertValue(
                    structVal, llvm::Constant::getNullValue(fieldType), onStdinFieldIndex);
            }

            // onStdinReturn = nullptr
            {
                auto& onStdinReturnDecl = declList[onStdinReturnFieldIndex];
                auto* fieldType = compiler->GetType(onStdinReturnDecl);
                structVal = compiler->CreateInsertValue(
                    structVal, llvm::Constant::getNullValue(fieldType), onStdinReturnFieldIndex);
            }

            // inbox = heap-allocated channel<IMessage> fat-ptr
            {
                auto* fatTy = compiler->GetFatPtrType();
                auto dsIt = compiler->dataStructures.find("channel__IMessage");
                auto* inboxCtorFn = compiler->GetFunction("channel__IMessage");
                auto* mallocInboxFn = compiler->GetFunction("malloc");
                if (dsIt != compiler->dataStructures.end() && inboxCtorFn && mallocInboxFn)
                {
                    auto* channelType    = dsIt->second.StructType;
                    auto* channelSize    = compiler->GetTypeSizeBytes(channelType);
                    auto* channelRaw     = compiler->builder->CreateCall(
                        mallocInboxFn->getFunctionType(), mallocInboxFn, {channelSize}, "inbox_raw");
                    auto* channelPtr     = compiler->builder->CreateBitCast(
                        channelRaw, channelType->getPointerTo(), "inbox_ptr");
                    auto* channelZeroVal = compiler->builder->CreateCall(
                        inboxCtorFn->getFunctionType(), inboxCtorFn, {}, "inbox_zero");
                    compiler->builder->CreateStore(channelZeroVal, channelPtr);
                    auto* inboxVtable    = compiler->GetOrCreateVTable("channel__IMessage", "IQueue__IMessage");
                    auto* inboxFatPtr    = compiler->BuildInterfaceFatValue(inboxVtable, channelPtr);
                    structVal = compiler->CreateInsertValue(structVal, inboxFatPtr, inboxFieldIndex);
                }
                else
                {
                    structVal = compiler->CreateInsertValue(
                        structVal, llvm::Constant::getNullValue(fatTy), inboxFieldIndex);
                }
            }

            // _stop_source = stop_source()
            if (auto* stopSrcCtorFn = compiler->GetFunction("stop_source"))
            {
                auto* stopSrcInitVal = compiler->builder->CreateCall(
                    stopSrcCtorFn->getFunctionType(), stopSrcCtorFn, {}, "stop_src_zero");
                structVal = compiler->CreateInsertValue(structVal, stopSrcInitVal, stopSrcFieldIndex);
            }

            // trackHandles = 0
            structVal = compiler->CreateInsertValue(
                structVal,
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*compiler->context), 0),
                trackHandlesFieldIndex);

            // _out / _in = nullptr (only when stream.cb is imported)
            if (outFieldIndex != (unsigned)-1)
            {
                auto* streamTy = compiler->GetDataStructure("stream").StructType;
                auto* nullStream = llvm::Constant::getNullValue(streamTy->getPointerTo());
                structVal = compiler->CreateInsertValue(structVal, nullStream, outFieldIndex);
                structVal = compiler->CreateInsertValue(structVal, nullStream, inStreamFieldIndex);
            }

            compiler->CreateReturnCall(structVal);
            compiler->CreateBlockBreak(nullptr, true);
        }

        ProcessPendingInstantiations();

        compiler->programTable[name].StructType          = structType;
        compiler->programTable[name].ConfigFields        = declList;
        compiler->programTable[name].ExitCodeFieldIndex  = exitCodeFieldIndex;
        compiler->programTable[name].ThreadFieldIndex    = threadFieldIndex;
        compiler->programTable[name].AllocatorFieldIndex = allocatorFieldIndex;
        compiler->programTable[name].OnStdoutFieldIndex       = onStdoutFieldIndex;
        compiler->programTable[name].OnStdinFieldIndex        = onStdinFieldIndex;
        compiler->programTable[name].OnStdinReturnFieldIndex  = onStdinReturnFieldIndex;
        compiler->programTable[name].InboxFieldIndex          = inboxFieldIndex;
        compiler->programTable[name].StopSourceFieldIndex    = stopSrcFieldIndex;
        compiler->programTable[name].TrackHandlesFieldIndex  = trackHandlesFieldIndex;
        compiler->programTable[name].OutFieldIndex           = outFieldIndex;
        compiler->programTable[name].InStreamFieldIndex      = inStreamFieldIndex;
        // IsImportedProgram and MainFunction were already set by LLVMBackend.cpp pre-scan

        // All imported programs implicitly implement IProcess.
        compiler->programTable[name].Interfaces = { "IProcess" };

        EmitProgramRunWrapper(name);
    }

    void ParseProgramDefinition(CFlatParser::ProgramDefinitionContext* ctx)
    {
        auto* compiler = Compiler(ctx);
        std::string name = ctx->directDeclarator()->getText();

        if (compiler->IsVerbose())
            std::cout << "[verbose]     parse program: " << name << "\n";

        // Queue generic types used in field declarations and function parameters
        if (activeTypeSubstitutions.empty())
        {
            for (auto decl : ctx->declaration())
                ScanAndQueueGenericTypeUses(decl);
            for (auto func : ctx->functionDefinition())
                ScanAndQueueGenericTypeUses(func);
            ProcessPendingInstantiations();
        }

        auto declList = ParseDeclarationList(ctx->declaration());

        // Ensure channel<IMessage> is fully instantiated before CreateStructType uses its layout.
        if (!instantiatedGenerics.count("channel__IMessage"))
        {
            pendingInstantiations.push_back({"channel", {"IMessage"}, "channel__IMessage"});
            instantiatedGenerics.insert("channel__IMessage");
            ProcessPendingInstantiations();
        }

        // Ensure IQueue<IMessage> interface is instantiated so the inbox fat-ptr vtable can be built.
        if (!instantiatedInterfaces.count("IQueue__IMessage"))
        {
            pendingInstantiations.push_back({"IQueue", {"IMessage"}, "IQueue__IMessage"});
            ProcessPendingInstantiations();
        }

        // Guard against user fields clashing with auto-injected synthetic fields
        for (auto& field : declList)
        {
            if (field.VariableName == "exitCode" || field.VariableName == "_thread"
                || field.VariableName == "_allocator" || field.VariableName == "onStdout"
                || field.VariableName == "onStdin" || field.VariableName == "onStdinReturn"
                || field.VariableName == "inbox" || field.VariableName == "_stop_source" || field.VariableName == "_inbox"
                || field.VariableName == "trackHandles" || field.VariableName == "_out" || field.VariableName == "_in")
                compiler->LogError(std::format(
                    "program '{}': field name '{}' is reserved", name, field.VariableName));
        }

        // Inject synthetic fields: exitCode (int), _thread (Thread), _allocator (IAllocator fat-ptr),
        // onStdout (function<void(char*)>), onStdin (function<char*()>),
        // inbox (IQueue<IMessage> fat-ptr), _stop_source (stop_source),
        // trackHandles (int - stored as i32 to avoid i1-in-ConstantStruct LLVM assertion),
        // _out / _in (stream* - only when stream.cb is imported; set by >> for direct write_bytes/read_buf access)
        unsigned exitCodeFieldIndex     = (unsigned)declList.size();
        unsigned threadFieldIndex       = exitCodeFieldIndex + 1;
        unsigned allocatorFieldIndex    = threadFieldIndex + 1;
        unsigned onStdoutFieldIndex      = allocatorFieldIndex + 1;
        unsigned onStdinFieldIndex       = onStdoutFieldIndex + 1;
        unsigned onStdinReturnFieldIndex = onStdinFieldIndex + 1;
        unsigned inboxFieldIndex         = onStdinReturnFieldIndex + 1;
        unsigned stopSrcFieldIndex       = inboxFieldIndex + 1;
        unsigned trackHandlesFieldIndex  = stopSrcFieldIndex + 1;
        unsigned outFieldIndex           = (unsigned)-1;  // set below if stream.cb is imported
        unsigned inStreamFieldIndex      = (unsigned)-1;  // set below if stream.cb is imported
        bool     hasStreamType           = compiler->dataStructures.count("stream") > 0;
        {
            LLVMBackend::DeclTypeAndValue exitCodeField;
            exitCodeField.TypeName     = "int";
            exitCodeField.VariableName = "exitCode";
            declList.push_back(exitCodeField);

            LLVMBackend::DeclTypeAndValue threadField;
            threadField.TypeName     = "Thread";
            threadField.VariableName = "_thread";
            declList.push_back(threadField);

            LLVMBackend::DeclTypeAndValue allocatorField;
            allocatorField.TypeName     = "IAllocator";
            allocatorField.VariableName = "_allocator";
            allocatorField.IsInterface  = true;
            allocatorField.Pointer      = true;
            declList.push_back(allocatorField);

            LLVMBackend::DeclTypeAndValue onStdoutField;
            onStdoutField.VariableName          = "onStdout";
            onStdoutField.IsFunctionPointer     = true;
            onStdoutField.FuncPtrReturnTypeName = "void";
            onStdoutField.FuncPtrParams         = {{"char", true}, {"int", false}};
            declList.push_back(onStdoutField);

            LLVMBackend::DeclTypeAndValue onStdinField;
            onStdinField.VariableName          = "onStdin";
            onStdinField.IsFunctionPointer     = true;
            onStdinField.FuncPtrReturnTypeName = "char";
            onStdinField.FuncPtrReturnPointer  = true;
            onStdinField.FuncPtrParams         = {};
            declList.push_back(onStdinField);

            LLVMBackend::DeclTypeAndValue onStdinReturnField;
            onStdinReturnField.VariableName          = "onStdinReturn";
            onStdinReturnField.IsFunctionPointer     = true;
            onStdinReturnField.FuncPtrReturnTypeName = "void";
            onStdinReturnField.FuncPtrParams         = {{"char", true}};
            declList.push_back(onStdinReturnField);

            // inbox: IQueue<IMessage> fat-ptr - swappable before run() just like _allocator.
            // By default the constructor pre-allocates a channel<IMessage> and builds the fat-ptr.
            // Users can replace it with any IQueue<IMessage> implementation via `p.inbox = &myQueue`.
            LLVMBackend::DeclTypeAndValue inboxField;
            inboxField.TypeName     = "IQueue__IMessage";
            inboxField.VariableName = "inbox";
            inboxField.IsInterface  = true;
            inboxField.Pointer      = true;
            declList.push_back(inboxField);

            LLVMBackend::DeclTypeAndValue stopSrcField;
            stopSrcField.TypeName     = "stop_source";
            stopSrcField.VariableName = "_stop_source";
            declList.push_back(stopSrcField);

            LLVMBackend::DeclTypeAndValue trackHandlesField;
            trackHandlesField.TypeName     = "int";
            trackHandlesField.VariableName = "trackHandles";
            declList.push_back(trackHandlesField);

            if (hasStreamType) {
                LLVMBackend::DeclTypeAndValue outField;
                outField.TypeName     = "stream";
                outField.VariableName = "_out";
                outField.Pointer      = true;
                outFieldIndex = (unsigned)declList.size();
                declList.push_back(outField);

                LLVMBackend::DeclTypeAndValue inField;
                inField.TypeName     = "stream";
                inField.VariableName = "_in";
                inField.Pointer      = true;
                inStreamFieldIndex = (unsigned)declList.size();
                declList.push_back(inField);
            }
        }

        // Build struct type with user fields + synthetic fields
        auto* structType = compiler->CreateStructType(name, declList);
        if (structType->isOpaque())
            structType->setBody(llvm::ArrayRef<llvm::Type*>());

        // Create default constructor (same pattern as ParseStructDefinition)
        {
            LLVMBackend::TypeAndValue returnType;
            returnType.TypeName = name;
            compiler->CreateFunctionDefinition(name, returnType, {});

            std::vector<llvm::Value*> initializers;
            for (auto& typeValue : declList)
            {
                llvm::Value* rvalue = nullptr;
                auto* initializer = typeValue.Initializer;
                if (initializer)
                {
                    if (auto* ae = initializer->assignmentExpression())
                        rvalue = ParseAssignmentExpression(ae);
                    else if (initializer->Default())
                        rvalue = GenerateDefaultValue(typeValue);
                }
                initializers.push_back(rvalue);
            }

            llvm::Value* structVal = llvm::UndefValue::get(structType);
            unsigned int idx = 0;
            for (auto* rvalue : initializers)
            {
                if (rvalue)
                {
                    auto* destType = structType->getTypeAtIndex(idx);
                    rvalue = compiler->Upconvert(rvalue, destType);
                    if (rvalue->getType() != destType && destType->isStructTy())
                    {
                        // Initializer type doesn't match struct field type (same fallback as ParseStructDefinition).
                        std::string fieldTypeName = declList[idx].TypeName;
                        if (compiler->GetFunction(fieldTypeName))
                            rvalue = compiler->CreateOverloadedFunctionCall(fieldTypeName, {});
                        else
                            rvalue = llvm::Constant::getNullValue(destType);
                    }
                    structVal = compiler->CreateInsertValue(structVal, rvalue, idx);
                }
                idx++;
            }

            // Synthetic field: exitCode = -1
            {
                auto* minusOne = llvm::ConstantInt::getSigned(
                    llvm::Type::getInt32Ty(*compiler->context), -1);
                structVal = compiler->CreateInsertValue(structVal, minusOne, exitCodeFieldIndex);
            }

            // Synthetic field: _thread = Thread()
            if (auto* threadCtorFn = compiler->GetFunction("Thread"))
            {
                auto* threadInitVal = compiler->builder->CreateCall(
                    threadCtorFn->getFunctionType(), threadCtorFn, {}, "thread_init");
                structVal = compiler->CreateInsertValue(structVal, threadInitVal, threadFieldIndex);
            }

            // Synthetic field: _allocator = zero (null IAllocator fat-ptr)
            {
                auto* fatTy = compiler->GetFatPtrType();
                structVal = compiler->CreateInsertValue(
                    structVal, llvm::Constant::getNullValue(fatTy), allocatorFieldIndex);
            }

            // Synthetic field: onStdout = nullptr (fat closure struct {i8*, i8*})
            {
                auto& onStdoutDecl = declList[onStdoutFieldIndex];
                auto* fieldType = compiler->GetType(onStdoutDecl);
                structVal = compiler->CreateInsertValue(
                    structVal, llvm::Constant::getNullValue(fieldType), onStdoutFieldIndex);
            }

            // Synthetic field: onStdin = nullptr (fat closure struct {i8*, i8*})
            {
                auto& onStdinDecl = declList[onStdinFieldIndex];
                auto* fieldType = compiler->GetType(onStdinDecl);
                structVal = compiler->CreateInsertValue(
                    structVal, llvm::Constant::getNullValue(fieldType), onStdinFieldIndex);
            }

            // Synthetic field: onStdinReturn = nullptr (fat closure struct {i8*, i8*})
            {
                auto& onStdinReturnDecl = declList[onStdinReturnFieldIndex];
                auto* fieldType = compiler->GetType(onStdinReturnDecl);
                structVal = compiler->CreateInsertValue(
                    structVal, llvm::Constant::getNullValue(fieldType), onStdinReturnFieldIndex);
            }

            // Synthetic field: inbox = IQueue<IMessage> fat-ptr pointing to a heap-allocated
            // channel<IMessage> (zero-init).  Pre-allocating ensures inbox.init(N) is safe to
            // call before run().  Users replace it with `p.inbox = &myQueue` to swap the impl.
            {
                auto* fatTy = compiler->GetFatPtrType();
                auto dsIt = compiler->dataStructures.find("channel__IMessage");
                auto* inboxCtorFn = compiler->GetFunction("channel__IMessage");
                auto* mallocInboxFn = compiler->GetFunction("malloc");
                if (dsIt != compiler->dataStructures.end() && inboxCtorFn && mallocInboxFn)
                {
                    auto* channelType    = dsIt->second.StructType;
                    auto* channelSize    = compiler->GetTypeSizeBytes(channelType);
                    auto* channelRaw     = compiler->builder->CreateCall(
                        mallocInboxFn->getFunctionType(), mallocInboxFn, {channelSize}, "inbox_raw");
                    auto* channelPtr     = compiler->builder->CreateBitCast(
                        channelRaw, channelType->getPointerTo(), "inbox_ptr");
                    auto* channelZeroVal = compiler->builder->CreateCall(
                        inboxCtorFn->getFunctionType(), inboxCtorFn, {}, "inbox_zero");
                    compiler->builder->CreateStore(channelZeroVal, channelPtr);
                    auto* inboxVtable    = compiler->GetOrCreateVTable("channel__IMessage", "IQueue__IMessage");
                    auto* inboxFatPtr    = compiler->BuildInterfaceFatValue(inboxVtable, channelPtr);
                    structVal = compiler->CreateInsertValue(structVal, inboxFatPtr, inboxFieldIndex);
                }
                else
                {
                    structVal = compiler->CreateInsertValue(
                        structVal, llvm::Constant::getNullValue(fatTy), inboxFieldIndex);
                }
            }

            // Synthetic field: _stop_source = stop_source() (zero-init; init() called in run())
            if (auto* stopSrcCtorFn = compiler->GetFunction("stop_source"))
            {
                auto* stopSrcInitVal = compiler->builder->CreateCall(
                    stopSrcCtorFn->getFunctionType(), stopSrcCtorFn, {}, "stop_src_zero");
                structVal = compiler->CreateInsertValue(structVal, stopSrcInitVal, stopSrcFieldIndex);
            }

            // Synthetic field: trackHandles = 0 (int, not bool - avoids i1-in-ConstantStruct assertion)
            structVal = compiler->CreateInsertValue(
                structVal,
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*compiler->context), 0),
                trackHandlesFieldIndex);

            // Synthetic fields: _out = nullptr, _in = nullptr (stream*; only present when stream.cb is imported)
            if (outFieldIndex != (unsigned)-1)
            {
                auto* streamTy = compiler->GetDataStructure("stream").StructType;
                auto* nullStream = llvm::Constant::getNullValue(streamTy->getPointerTo());
                structVal = compiler->CreateInsertValue(structVal, nullStream, outFieldIndex);
                structVal = compiler->CreateInsertValue(structVal, nullStream, inStreamFieldIndex);
            }

            compiler->CreateReturnCall(structVal);
            compiler->CreateBlockBreak(nullptr, true);
        }

        // Register class fields in LSP index for dot-completion
        if (auto* s = compiler->GetSymbolSink())
        {
            auto sd = compiler->GetDataStructure(name);
            for (const auto& field : sd.StructFields)
            {
                if (field.VariableName.empty()) continue;
                std::string annSig;
                for (const auto& ann : field.Annotations)
                {
                    annSig += "[" + ann.Name;
                    if (!ann.Value.empty()) annSig += "(" + ann.Value + ")";
                    annSig += "] ";
                }
                std::string typeSig = field.TypeName;
                if (field.Pointer) typeSig += "*";
                if (field.ElemPointer) typeSig += "*";
                s->Register(SymbolKind::Field, name + "." + field.VariableName,
                            compiler->GetSourceFilePath(),
                            (int)ctx->getStart()->getLine(),
                            (int)ctx->getStart()->getCharPositionInLine(),
                            annSig + typeSig + " " + field.VariableName);
            }
        }

        // Parse member functions (includes user's main)
        {
            bool savedScope = global_scope;
            for (auto func : ctx->functionDefinition())
            {
                global_scope = false;
                ParseFunctionDefinition(func, name);
            }
            global_scope = savedScope;
        }

        // Parse destructor if present
        {
            bool savedScope = global_scope;
            for (auto dtor : ctx->destructorDefinition())
            {
                global_scope = false;
                ParseDestructorDefinition(dtor, name);
            }
            global_scope = savedScope;
        }

        // Flush instantiations (e.g. list__string from main's params) before emitting run()
        ProcessPendingInstantiations();

        // Set programTable before EmitProgramRunWrapper so it can read field indices
        compiler->programTable[name].StructType          = structType;
        compiler->programTable[name].ConfigFields        = declList;
        compiler->programTable[name].ExitCodeFieldIndex  = exitCodeFieldIndex;
        compiler->programTable[name].ThreadFieldIndex    = threadFieldIndex;
        compiler->programTable[name].AllocatorFieldIndex = allocatorFieldIndex;
        compiler->programTable[name].OnStdoutFieldIndex       = onStdoutFieldIndex;
        compiler->programTable[name].OnStdinFieldIndex        = onStdinFieldIndex;
        compiler->programTable[name].OnStdinReturnFieldIndex  = onStdinReturnFieldIndex;
        compiler->programTable[name].InboxFieldIndex          = inboxFieldIndex;
        compiler->programTable[name].StopSourceFieldIndex    = stopSrcFieldIndex;
        compiler->programTable[name].TrackHandlesFieldIndex  = trackHandlesFieldIndex;
        compiler->programTable[name].OutFieldIndex           = outFieldIndex;
        compiler->programTable[name].InStreamFieldIndex      = inStreamFieldIndex;

        // Emit auto-generated run(), WaitForExit(), and __program_run_Name trampoline
        EmitProgramRunWrapper(name);
    }

    void ParseClassDefinition(CFlatParser::ClassDefinitionContext* ctx, const std::string& nameOverride = {}, const std::string& namespaceName = {})
    {
        auto* compiler = Compiler(ctx);
        auto decl = ctx->directDeclarator();
        std::string baseName = decl->getText();
        std::string structName;

        // Apply nameOverride first (for generic instantiations), then namespace
        if (!nameOverride.empty())
        {
            structName = nameOverride;
        }
        else if (!namespaceName.empty())
        {
            structName = namespaceName + "." + baseName;
        }
        else
        {
            structName = baseName;
        }

        // If this is a generic template definition (not an instantiation), store it and return.
        if (nameOverride.empty() && ctx->genericTypeParameters() != nullptr)
        {
            auto typeParams = ParseGenericTypeParameters(ctx->genericTypeParameters());
            genericClassTemplates[structName] = ctx;
            genericStructTypeParams[structName] = typeParams;
            genericClassConstraints[structName] = ParseWhereClause(ctx->whereClause());
            return;
        }

        // Re-emission guard: when the user's main file is also reached via a
        // transitive import under a different canonical path (e.g. the deployed
        // copy of a cflat/core/ file), the runtime chain may have already fully
        // emitted this struct. CreateFunctionDefinition's duplicate-skip leaves
        // the IR builder without the right scope to safely emit the bodies a
        // second time, so any field-or-method reference falls over. Detect
        // "already fully defined" here and skip the entire walk.
        {
            auto sd = compiler->GetDataStructure(structName);
            if (sd.StructType != nullptr && !sd.StructType->isOpaque())
            {
                if (auto* existing = compiler->GetFunction(structName);
                    existing != nullptr && !existing->empty())
                {
                    if (compiler->IsVerbose())
                        std::cout << "[verbose]     skipping duplicate struct definition: " << structName << "\n";
                    return;
                }
            }
        }

        if (compiler->IsVerbose())
            std::cout << "[verbose]     parse decl list: " << structName << "\n";

        // Process nested struct/class definitions before fields so their types are available
        for (auto* nestedStruct : ctx->structDefinition())
            ParseStructDefinition(nestedStruct, {}, structName);
        for (auto* nestedClass : ctx->classDefinition())
            ParseClassDefinition(nestedClass, {}, structName);

        // Push scope so unqualified nested type names resolve (e.g. Inner -> Outer.Inner)
        structScopeStack.push_back(structName);

        auto declarationList = ctx->declaration();
        std::vector<llvm::Type*> types;

        // Queue and instantiate generic types used in field declarations before
        // ParseDeclarationList resolves them to LLVM types. Only needed at top-level
        // (non-template) scope; template instantiations already have activeTypeSubstitutions
        // or activePackSubstitutions set and their generics are queued via ParseDeclarationSpecifiers.
        if (activeTypeSubstitutions.empty() && activePackSubstitutions.empty())
        {
            for (auto decl : declarationList)
                ScanAndQueueGenericTypeUses(decl);
            ProcessPendingInstantiations();
        }

        // Build field list, expanding pack fields (T... fieldName -> fieldName_0, fieldName_1, ...)
        std::vector<LLVMBackend::DeclTypeAndValue> declList;
        for (auto* decl : declarationList)
        {
            std::string packParamName;
            if (decl->declarationSpecifiers())
            {
                for (auto* ds : decl->declarationSpecifiers()->declarationSpecifier())
                {
                    auto* ts = ds->typeSpecifier();
                    if (!ts || !ts->genericIdentifier() || ts->genericIdentifier()->genericTypeParameters()) continue;
                    auto* gid = ts->genericIdentifier();
                    if (!gid->Identifier()) continue;
                    std::string n = gid->Identifier()->getText();
                    if (activePackSubstitutions.count(n)) { packParamName = n; break; }
                }
            }

            if (packParamName.empty())
            {
                for (auto& f : ParseDeclarationList({decl}))
                    declList.push_back(f);
                continue;
            }

            std::string baseFieldName;
            if (auto* idl = decl->initDeclaratorList())
                if (!idl->initDeclarator().empty())
                    if (auto* d = idl->initDeclarator()[0]->declarator())
                        if (auto* dd = d->directDeclarator())
                            baseFieldName = getDirectDeclName(dd);

            auto& packTypes = activePackSubstitutions.at(packParamName);
            auto savedPackItemSubst = activeTypeSubstitutions;
            for (size_t i = 0; i < packTypes.size(); i++)
            {
                activeTypeSubstitutions[packParamName] = packTypes[i];
                auto expanded = ParseDeclarationList({decl});
                for (auto& f : expanded)
                {
                    f.VariableName = baseFieldName + "_" + std::to_string(i);
                    declList.push_back(f);
                }
            }
            activeTypeSubstitutions = savedPackItemSubst;
        }

        // Process lock field groups: each group annotates its fields with GuardedBy.
        for (auto* lfg : ctx->lockFieldGroup())
        {
            if (activeTypeSubstitutions.empty() && activePackSubstitutions.empty())
                ScanAndQueueGenericTypeUses(lfg);

            auto groupArgs = lfg->lockClause()->lockArgList()->expression();
            if (groupArgs.empty()) continue;
            std::string guardianName = GetLockArgCanonical(groupArgs[0]);

            for (auto* decl : lfg->declaration())
            {
                for (auto& f : ParseDeclarationList({decl}))
                {
                    f.GuardedBy = guardianName;
                    declList.push_back(f);
                }
            }
        }

        if (compiler->IsVerbose())
            std::cout << "[verbose]     decl list has " << declList.size() << " fields\n";

        if (compiler->IsVerbose())
            std::cout << "[verbose]     create struct type: " << structName << "\n";
        // Capture `class alignas(N) Foo { ... }` before layout so trailing
        // padding can be inserted atomically by CreateStructType.
        uint64_t userAlign = 0;
        if (auto* alignSpec = ctx->alignmentSpecifier())
            userAlign = ParseAlignmentSpecifier(alignSpec);
        auto structType = compiler->CreateStructType(structName, declList, userAlign);
        // A class with zero fields still needs a sized (non-opaque) type
        // so that alloca/sizeof work correctly (e.g. when passed via interface).
        if (structType->isOpaque())
            structType->setBody(llvm::ArrayRef<llvm::Type*>());
        if (compiler->IsVerbose())
            std::cout << "[verbose]     create default ctor: " << structName << "\n";
        LLVMBackend::TypeAndValue returnType{
            .TypeName = structName,
        };
        // Flush any nested generic instantiations queued while parsing field declarations,
        // so their constructors exist before this class's default constructor calls them.
        {
            auto savedSubst = activeTypeSubstitutions;
            ProcessPendingInstantiations();
            activeTypeSubstitutions = savedSubst;
        }
        // If the user wrote an explicit no-arg constructor, skip the auto-generated one.
        bool hasExplicitNoArgCtor = [&]() {
            for (auto* f : ctx->functionDefinition())
                if (getFunctionName(f) == baseName && !f->parameterTypeList())
                    return true;
            return false;
        }();

        // Create default constructor (skipped when user provides an explicit no-arg ctor)
        if (!hasExplicitNoArgCtor)
        {
            auto funcDef = compiler->CreateFunctionDefinition(structName, returnType, {});

            std::vector<llvm::Value*> initializers;
            for (auto& typeValue : declList)
            {
                auto initializer = typeValue.Initializer;
                llvm::Value* rvalue = nullptr;
                if (initializer != nullptr)
                {
                    auto assignmentExpression = initializer->assignmentExpression();
                    if (assignmentExpression != nullptr)
                    {
                        rvalue = ParseAssignmentExpression(assignmentExpression);
                        if (typeValue.TypeName == "auto")
                        {
                            typeValue.TypeName = rvalue->getType()->getStructName();
                            structType = compiler->CreateStructType(structName, declList);
                        }
                    }
                    else if (initializer->Default() != nullptr)
                    {
                        rvalue = GenerateDefaultValue(typeValue);
                    }
                }
                initializers.push_back(rvalue);
            }

            llvm::Value* structVal = llvm::UndefValue::get(structType);

            LLVMBackend::TypeAndValue myStruct;
            myStruct.TypeName = structName;
            myStruct.VariableName = "_" + structName;

            unsigned int structIndex = 0;

            for (auto rvalue : initializers)
            {
                auto* destType = structType->getTypeAtIndex(structIndex);
                // No explicit initializer on a struct-typed field - call its default ctor.
                if (rvalue == nullptr && destType->isStructTy())
                {
                    std::string fieldTypeName = declList[structIndex].TypeName;
                    if (compiler->GetFunction(fieldTypeName))
                        rvalue = compiler->CreateOverloadedFunctionCall(fieldTypeName, {});
                    else
                        rvalue = llvm::Constant::getNullValue(destType);
                }
                if (rvalue != nullptr)
                {
                    rvalue = compiler->Upconvert(rvalue, destType);
                    if (rvalue->getType() != destType && destType->isStructTy())
                    {
                        std::string fieldTypeName = declList[structIndex].TypeName;
                        if (compiler->GetFunction(fieldTypeName))
                            rvalue = compiler->CreateOverloadedFunctionCall(fieldTypeName, {});
                        else
                            rvalue = llvm::Constant::getNullValue(destType);
                    }
                    structVal = compiler->CreateInsertValue(structVal, rvalue, structIndex);
                }

                structIndex++;
            }

            compiler->CreateReturnCall(structVal);
            compiler->CreateBlockBreak(nullptr, true);
        } // end if (!hasExplicitNoArgCtor)

        // Register class fields in LSP index for dot-completion
        if (auto* s = compiler->GetSymbolSink())
        {
            auto sd = compiler->GetDataStructure(structName);
            for (const auto& field : sd.StructFields)
            {
                if (field.VariableName.empty()) continue;
                std::string annSig;
                for (const auto& ann : field.Annotations)
                {
                    annSig += "[" + ann.Name;
                    if (!ann.Value.empty()) annSig += "(" + ann.Value + ")";
                    annSig += "] ";
                }
                std::string typeSig = field.TypeName;
                if (field.Pointer) typeSig += "*";
                if (field.ElemPointer) typeSig += "*";
                s->Register(SymbolKind::Field, structName + "." + field.VariableName,
                            compiler->GetSourceFilePath(),
                            (int)ctx->getStart()->getLine(),
                            (int)ctx->getStart()->getCharPositionInLine(),
                            annSig + typeSig + " " + field.VariableName);
            }
        }

        // Parse member functions
        auto functionList = ctx->functionDefinition();

        // For generic instantiations, pre-declare all member function signatures
        // so they can forward-reference each other.
        if (!nameOverride.empty())
        {
            for (auto func : functionList)
            {
                if (IsReturnBlockFunction(func)) continue;
                if (func->genericTypeParameters() != nullptr) continue;

                std::string funcName = getFunctionName(func);

                // Constructor overload - pre-declare without this* parameter
                if (funcName == baseName && func->parameterTypeList())
                {
                    auto* declParamList = func->parameterTypeList();
                    auto declParams = this->ParseParameterTypeList(declParamList);
                    bool declVarargs = declParamList->Ellipsis() != nullptr;
                    std::vector<LLVMBackend::TypeAndValue> ctorAllParams(declParams.begin(), declParams.end());
                    compiler->CreateFunctionDeclaration(structName, returnType, ctorAllParams, false, declVarargs);
                    continue;
                }

                bool isOperatorFunc = (funcName == "operator new"
                                    || funcName == "operator delete"
                                    || funcName == "operator string");
                bool isStaticFunc = isFunctionStatic(func);
                bool isStaticLike = isOperatorFunc || isStaticFunc;
                std::string declName = isStaticLike ? (structName + "." + funcName) : funcName;

                auto declReturnType = this->getFunctionReturnType(func);
                auto* declParamList = func->parameterTypeList();
                auto declParams = this->ParseParameterTypeList(declParamList);
                bool declVarargs = declParamList && declParamList->Ellipsis() != nullptr;

                if (!isStaticLike)
                {
                    LLVMBackend::DeclTypeAndValue thisParam;
                    thisParam.TypeName = structName;
                    thisParam.VariableName = structName + "__";
                    thisParam.Pointer = true;
                    declParams.insert(declParams.begin(), thisParam);
                }

                std::vector<LLVMBackend::TypeAndValue> declAllParams(declParams.begin(), declParams.end());
                bool declReturnsOwned = false;
                if (declReturnType.TypeName == "string")
                {
                    if (declName == "operator+")
                        declReturnsOwned = true;
                    else if (declName == "operator string" && declAllParams.size() == 1 && declAllParams[0].TypeName == "i32")
                        declReturnsOwned = true;
                    else if (declReturnType.IsMove)
                        declReturnsOwned = true;
                }
                else if (declReturnType.IsMove && declReturnType.Pointer)
                {
                    declReturnsOwned = true;
                }
                compiler->CreateFunctionDeclaration(declName, declReturnType, declAllParams, declReturnType.external, declVarargs, declReturnsOwned, !isStaticLike);
            }
        }

        // Pre-register destructor so 'delete' inside static methods can call it.
        if (!ctx->destructorDefinition().empty())
        {
            if (auto* dtorFn = compiler->GetFunction("~" + structName))
                compiler->RegisterDestructor(structName, dtorFn);
        }

        // Pre-register interfaces before compiling method bodies so that
        // StructImplementsInterface() returns true for assignments inside methods
        // of the class itself (e.g. `IJSON result = this;` inside a class : IJSON).
        // Helper: resolve concrete type args from an implements clause generic param list,
        // expanding pack params (T...) using activePackSubstitutions.
        auto resolveImplsTypeArgs = [&](CFlatParser::GenericTypeParametersContext* gtp) -> std::vector<std::string>
        {
            std::vector<std::string> args;
            for (auto* entry : gtp->typeParameterList()->typeParameterEntry())
            {
                std::string name = entry->typeSpecifier() ? entry->typeSpecifier()->getText() : entry->getText();
                bool isPack = entry->Ellipsis() != nullptr;
                if (isPack)
                {
                    // Expand pack substitution: T... -> [int, float, ...]
                    auto packIt = activePackSubstitutions.find(name);
                    if (packIt != activePackSubstitutions.end())
                        for (const auto& t : packIt->second)
                            args.push_back(t);
                    else
                        args.push_back(name);
                }
                else
                {
                    auto substIt = activeTypeSubstitutions.find(name);
                    if (substIt != activeTypeSubstitutions.end())
                        args.push_back(substIt->second);
                    else
                        args.push_back(name);
                }
            }
            return args;
        };

        {
            std::vector<std::string> earlyIfaceNames;
            for (auto* genId : ctx->genericIdentifier())
            {
                if (!genId->Identifier()) continue;
                std::string ifaceBaseName = genId->Identifier()->getText();
                std::string ifaceName = ifaceBaseName;
                if (genId->genericTypeParameters() != nullptr)
                {
                    auto concreteTypeArgs = resolveImplsTypeArgs(genId->genericTypeParameters());
                    ifaceName = MangledGenericName(ifaceBaseName, concreteTypeArgs);
                }
                earlyIfaceNames.push_back(ifaceName);
            }
            if (!earlyIfaceNames.empty())
                compiler->RegisterStructInterfaces(structName, earlyIfaceNames);
        }

        {
            bool savedScope = global_scope;
            for (auto func : functionList)
            {
                global_scope = false;
                std::string funcName = getFunctionName(func);
                if (compiler->IsVerbose())
                    std::cout << "[verbose]     parse member: " << structName << "." << funcName << "\n";
                // Constructor - same name as class (no-arg or with parameters)
                if (funcName == baseName)
                {
                    ParseConstructorDefinition(func, structName);
                    continue;
                }
                if (funcName == "operator new" || funcName == "operator delete" || isFunctionStatic(func))
                {
                    if (func->genericTypeParameters() != nullptr)
                    {
                        std::string qualifiedName = structName + "." + funcName;
                        auto typeParams = ParseGenericTypeParameters(func->genericTypeParameters());
                        genericFunctionTemplates[qualifiedName] = func;
                        genericFunctionTypeParams[qualifiedName] = typeParams;
                        genericFunctionConstraints[qualifiedName] = ParseWhereClause(func->whereClause());
                    }
                    else
                    {
                        ParseFunctionDefinition(func, {}, {}, structName + "." + funcName);
                    }
                }
                else
                    ParseFunctionDefinition(func, structName);
            }
            global_scope = savedScope;
        }

        // Parse functions declared inside positional lock groups.
        {
            bool savedScope = global_scope;
            for (auto* lfg : ctx->lockFieldGroup())
            {
                for (auto* func : lfg->functionDefinition())
                {
                    global_scope = false;
                    ParseFunctionDefinition(func, structName);
                }
            }
            global_scope = savedScope;
        }

        // Parse destructor
        {
            bool savedScope = global_scope;
            for (auto dtor : ctx->destructorDefinition())
            {
                global_scope = false;
                ParseDestructorDefinition(dtor, structName);
            }
            global_scope = savedScope;
        }

        // Record interfaces and verify implementations
        std::vector<std::string> ifaceNames;
        for (auto* genId : ctx->genericIdentifier())
        {
            if (!genId->Identifier()) continue;
            std::string ifaceBaseName = genId->Identifier()->getText();
            std::string ifaceName = ifaceBaseName;

            if (genId->genericTypeParameters() != nullptr)
            {
                auto concreteTypeArgs = resolveImplsTypeArgs(genId->genericTypeParameters());
                ifaceName = MangledGenericName(ifaceBaseName, concreteTypeArgs);

                // Build substitution maps for the interface template's type params (pack-aware)
                std::unordered_map<std::string, std::string> ifaceSubstitutions;
                std::unordered_map<std::string, std::vector<std::string>> ifacePackSubstitutions;
                const auto& ifaceTypeParams = genericInterfaceTypeParams[ifaceBaseName];
                auto ifacePackIdxIt = genericInterfacePackIndex.find(ifaceBaseName);
                size_t ifacePackIdx = (ifacePackIdxIt != genericInterfacePackIndex.end())
                                      ? ifacePackIdxIt->second : std::string::npos;
                if (ifacePackIdx == std::string::npos)
                {
                    for (size_t i = 0; i < ifaceTypeParams.size() && i < concreteTypeArgs.size(); i++)
                        ifaceSubstitutions[ifaceTypeParams[i]] = concreteTypeArgs[i];
                }
                else
                {
                    for (size_t i = 0; i < ifacePackIdx && i < concreteTypeArgs.size(); i++)
                        ifaceSubstitutions[ifaceTypeParams[i]] = concreteTypeArgs[i];
                    ifacePackSubstitutions[ifaceTypeParams[ifacePackIdx]] =
                        std::vector<std::string>(concreteTypeArgs.begin() + ifacePackIdx, concreteTypeArgs.end());
                }

                InstantiateGenericInterface(ifaceBaseName, ifaceName, ifaceSubstitutions, ifacePackSubstitutions);
            }

            ifaceNames.push_back(ifaceName);
        }
        compiler->RegisterStructInterfaces(structName, ifaceNames);
        for (const auto& interfaceName : ifaceNames)
            compiler->VerifyInterfaceImplementation(structName, interfaceName);

        // Process any generic instantiations that were queued during this class definition
        // ProcessPendingInstantiations();

        structScopeStack.pop_back();
    }

    std::vector<std::string> ParseGenericTypeParameters(CFlatParser::GenericTypeParametersContext* genericParams)
    {
        std::vector<std::string> typeParams;
        if (!genericParams)
            return typeParams;

        auto typeParamList = genericParams->typeParameterList();
        if (!typeParamList)
            return typeParams;

        bool seenPack = false;
        for (auto* entry : typeParamList->typeParameterEntry())
        {
            auto* typeSpec = entry->typeSpecifier();
            // Generic type parameters must be simple identifiers, not built-in types
            if (!typeSpec || !typeSpec->genericIdentifier() || !typeSpec->genericIdentifier()->Identifier())
            {
                LogErrorContext(entry, "Generic type parameter must be an identifier, not a built-in type");
                continue;
            }
            if (seenPack)
            {
                LogErrorContext(entry, "Only the last type parameter may be a pack (T...)");
                continue;
            }
            typeParams.push_back(typeSpec->genericIdentifier()->Identifier()->getText());
            if (entry->Ellipsis() != nullptr)
                seenPack = true;
        }

        return typeParams;
    }

    // Returns { typeParamName -> [requiredInterface, …] } from a whereClause context.
    std::unordered_map<std::string, std::vector<std::string>>
    ParseWhereClause(CFlatParser::WhereClauseContext* wc)
    {
        std::unordered_map<std::string, std::vector<std::string>> result;
        if (!wc) return result;
        for (auto* constraint : wc->typeParameterConstraint())
        {
            auto ids = constraint->Identifier();
            if (ids.size() < 2) continue;
            result[ids[0]->getText()].push_back(ids[1]->getText());
        }
        return result;
    }

    // Checks that each concrete type argument satisfies its where-clause constraints.
    // Logs an error and returns false on the first violation.
    bool CheckConstraints(
        const std::string& templateName,
        const std::vector<std::string>& typeParams,
        const std::vector<std::string>& typeArgs,
        const std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>>& constraintMap,
        antlr4::ParserRuleContext* ctx)
    {
        auto cit = constraintMap.find(templateName);
        if (cit == constraintMap.end()) return true;
        for (size_t i = 0; i < typeParams.size() && i < typeArgs.size(); i++)
        {
            auto pit = cit->second.find(typeParams[i]);
            if (pit == cit->second.end()) continue;
            for (const auto& iface : pit->second)
            {
                if (!Compiler(ctx)->TypeImplementsInterface(typeArgs[i], iface))
                {
                    Compiler(ctx)->LogError(std::format(
                        "type '{}' does not implement '{}', required by constraint 'where {} : {}'",
                        typeArgs[i], iface, typeParams[i], iface));
                    return false;
                }
            }
        }
        return true;
    }

    void ParseConstructorDefinition(CFlatParser::FunctionDefinitionContext* func, const std::string& structName)
    {
        auto* compiler = Compiler(func);
        auto params = ParseParameterTypeList(func->parameterTypeList());
        size_t line = func->getStart()->getLine();
        bool varargs = func->parameterTypeList() && func->parameterTypeList()->Ellipsis() != nullptr;

        // Pre-scan body for generic instantiations (same as ParseFunctionDefinition)
        if (auto* blockItemList = func->compoundStatement()->blockItemList())
        {
            ScanAndQueueGenericTypeUses(blockItemList);
            ProcessPendingInstantiations();
        }

        LLVMBackend::DeclTypeAndValue returnType;
        returnType.TypeName = structName;
        std::vector<LLVMBackend::TypeAndValue> allParams(params.begin(), params.end());

        // Open constructor function - no this* parameter; returns the struct by value
        auto fn = compiler->CreateFunctionDefinition(structName, returnType, allParams, false, varargs, line);
        compiler->InitializeBlock(&fn->front(), false);

        // Get the struct's LLVM type (without pointer)
        auto* structLLVMType = llvm::cast<llvm::StructType>(compiler->GetType(returnType, nullptr, false));

        // Alloca the struct so we can GEP into fields via 'this'
        auto* thisAlloca = compiler->AllocaAtEntry(structLLVMType, nullptr, structName + "__");

        if (allParams.empty())
        {
            // No-arg constructor: calling structName() would be self-recursive.
            // Zero-initialize the struct, then apply field defaults from the data structure.
            compiler->builder->CreateStore(llvm::Constant::getNullValue(structLLVMType), thisAlloca);
            auto structData = compiler->GetDataStructure(structName);
            unsigned fieldIdx = 0;
            for (const auto& field : structData.StructFields)
            {
                if (field.Initializer != nullptr)
                {
                    auto* assignExpr = field.Initializer->assignmentExpression();
                    if (assignExpr != nullptr)
                    {
                        llvm::Value* fieldVal = ParseAssignmentExpression(assignExpr);
                        if (fieldVal)
                        {
                            auto* destType = structLLVMType->getTypeAtIndex(fieldIdx);
                            fieldVal = compiler->Upconvert(fieldVal, destType);
                            if (fieldVal->getType() == destType)
                            {
                                auto* fieldPtr = compiler->builder->CreateStructGEP(
                                    structLLVMType, thisAlloca, fieldIdx, field.VariableName);
                                compiler->builder->CreateStore(fieldVal, fieldPtr);
                            }
                        }
                    }
                }
                fieldIdx++;
            }
        }
        else
        {
            // Parameterized constructor: delegate to the no-arg ctor for default initialization.
            auto* defaultVal = compiler->CreateOverloadedFunctionCall(structName, {});
            if (defaultVal)
                compiler->builder->CreateStore(defaultVal, thisAlloca);
        }

        // Register the alloca as the implicit 'this' pointer so member field access works
        LLVMBackend::TypeAndValue thisTv;
        thisTv.TypeName = structName;
        thisTv.VariableName = structName + "__";
        thisTv.Pointer = true;
        compiler->RegisterThisPointer(thisTv, thisAlloca, structLLVMType);

        // Parse user-written constructor body
        if (auto* blockItemList = func->compoundStatement()->blockItemList())
            ParseBlockItemList(blockItemList);

        // Load and return the (possibly mutated) struct by value
        auto* resultVal = compiler->CreateLoad(structLLVMType, thisAlloca);
        compiler->CreateReturnCall(resultVal);
        compiler->CreateBlockBreak(nullptr, true);
        compiler->ClearCurrentSubprogram();

        GenerateDefaultParamOverloads(structName, returnType, params, varargs, line);
    }

    void ParseDestructorDefinition(CFlatParser::DestructorDefinitionContext* ctx, const std::string& structName)
    {
        auto* compiler = Compiler(ctx);
        LLVMBackend::DeclTypeAndValue thisParam;
        thisParam.TypeName = structName;
        thisParam.VariableName = structName + "__";
        thisParam.Pointer = true;

        std::vector<LLVMBackend::TypeAndValue> params = { thisParam };

        LLVMBackend::TypeAndValue returnType;
        returnType.TypeName = "void";

        int line = static_cast<int>(ctx->getStart()->getLine());
        auto fn = compiler->CreateFunctionDefinition("~" + structName, returnType, params, false, false, line);
        compiler->RegisterDestructor(structName, fn);

        compiler->InitializeBlock(&fn->front(), false);

        auto blockItemList = ctx->compoundStatement()->blockItemList();
        if (blockItemList)
            ParseBlockItemList(blockItemList);

        compiler->CreateReturnCall(nullptr);
        compiler->CreateBlockBreak(nullptr, true);
        compiler->ClearCurrentSubprogram();
    }

    std::vector<LLVMBackend::DeclTypeAndValue> ParseParameterTypeList(CFlatParser::ParameterTypeListContext* paramTypeList)
    {
        std::vector<LLVMBackend::DeclTypeAndValue> params;

        if (paramTypeList == nullptr)
            return params;

        auto paramList = paramTypeList->parameterList();
        auto paramDeclList = paramList->parameterDeclaration();

        for (auto paramDecl : paramDeclList)
        {
            LLVMBackend::DeclTypeAndValue paramType = this->ParseDeclarationSpecifiers(paramDecl->declarationSpecifiers());
            if (auto declarer = paramDecl->declarator())
            {
                if (auto directDeclarer = declarer->directDeclarator())
                {
                    paramType.VariableName = directDeclarer->getText();
                }
            }

            if (paramType.IsMove && paramType.IsBond)
                LogErrorContext(paramDecl, std::format("parameter '{}': 'bond' and 'move' are mutually exclusive", paramType.VariableName));

            if (auto* lc = paramDecl->lockClause())
            {
                auto args = lc->lockArgList()->expression();
                if (args.size() == 1 && args[0]->getText() == "this")
                    paramType.LockThis = true;
                else
                    LogErrorContext(paramDecl, "lock(this) is the only supported form on function parameters");
            }

            paramType.DefaultValue = paramDecl->initializer();
            params.push_back(paramType);

            if (paramType.VariableName == "")
            {
                PrintContext(paramDecl);
                std::cout << "Function parameter name is missing.\n";
            }
        }

        return params;
    }

    LLVMBackend::ConstantVariant ParseNumberConstant(std::string rawNumber)
    {
        // Support suffixes: u/U, l/L, ll/LL, f/F, d/D and floating point forms with '.' or exponent.
        if (rawNumber.empty())
            return 0;

        // Handle optional leading sign
        bool negative = false;
        std::string s = rawNumber;
        if (s[0] == '+' || s[0] == '-')
        {
            negative = (s[0] == '-');
            s = s.substr(1);
        }

        // Determine if this is a hex literal (0x...)
        bool isHex = (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'));

        // Extract numeric part and suffix carefully.
        size_t suffixPos = s.size();
        if (isHex)
        {
            // For hex, scan forward from the "0x" prefix to include all hex digits
            size_t idx = 2; // skip 0x
            while (idx < s.size() && std::isxdigit(static_cast<unsigned char>(s[idx]))) ++idx;
            suffixPos = idx;
        }
        else
        {
            // For non-hex, strip trailing letters that belong to known suffix set (u,l,f,d)
            while (suffixPos > 0)
            {
                char c = s[suffixPos - 1];
                char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (lc == 'u' || lc == 'l' || lc == 'f' || lc == 'd')
                    --suffixPos;
                else
                    break;
            }
        }

        std::string numberPart = s.substr(0, suffixPos);
        std::string suffix = s.substr(suffixPos);
        for (auto& c : suffix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        bool hasU = suffix.find('u') != std::string::npos;
        int lCount = 0;
        for (char c : suffix) if (c == 'l') ++lCount;
        bool hasF = suffix.find('f') != std::string::npos;
        bool hasD = suffix.find('d') != std::string::npos;

        // For hex literals, 'e'/'E' are valid hex digits, not exponent markers.
        // Only treat e/E as a float indicator for decimal/non-hex numbers.
        bool looksFloat = (numberPart.find('.') != std::string::npos) ||
            (!isHex && numberPart.find('e') != std::string::npos) ||
            (!isHex && numberPart.find('E') != std::string::npos);

        // Floating point handling
        if (hasF || hasD || looksFloat)
        {
            try
            {
                if (hasF)
                    return std::stof(numberPart);
                else
                    return std::stod(numberPart);
            }
            catch (...) { return 0.0; }
        }

        // Integer handling. Support hex/octal/decimal by using base 0.
        unsigned long long uval = 0;
        try
        {
            if (numberPart.empty())
                uval = 0;
            else
                uval = std::stoull(numberPart, nullptr, 0);
        }
        catch (...) { uval = 0; }

        // If a long/long long suffix is present, prefer 64-bit result.
        if (lCount >= 1)
        {
            if (negative)
            {
                long long sval = -static_cast<long long>(uval);
                return static_cast<int64_t>(sval);
            }
            else
            {
                return static_cast<int64_t>(uval);
            }
        }

        // No explicit long suffix: pick smallest reasonable signed type unless 'u' forces unsigned semantics
        if (negative)
        {
            long long sval = -static_cast<long long>(uval);
            if (sval >= std::numeric_limits<int8_t>::min() && sval <= std::numeric_limits<int8_t>::max())
                return static_cast<char>(sval);
            if (sval >= std::numeric_limits<int16_t>::min() && sval <= std::numeric_limits<int16_t>::max())
                return static_cast<short>(sval);
            if (sval >= std::numeric_limits<int>::min() && sval <= std::numeric_limits<int>::max())
                return static_cast<int>(sval);
            return static_cast<int64_t>(sval);
        }

        if (hasU)
        {
            if (uval <= static_cast<unsigned long long>(std::numeric_limits<int>::max()))
                return static_cast<int>(uval);
            // u suffix on hex/value fitting in u32: reinterpret bits as i32 (same bit pattern).
            if (uval <= 0xFFFFFFFFull)
                return static_cast<int>(static_cast<uint32_t>(uval));
            return static_cast<int64_t>(uval);
        }

        if (uval <= static_cast<unsigned long long>(std::numeric_limits<int8_t>::max()))
            return static_cast<char>(uval);
        if (uval <= static_cast<unsigned long long>(std::numeric_limits<int16_t>::max()))
            return static_cast<short>(uval);
        if (uval <= static_cast<unsigned long long>(std::numeric_limits<int>::max()))
            return static_cast<int>(uval);
        // C++ rule: hex literals that exceed i32 positive range but fit in u32 are typed as
        // unsigned int. Since ConstantVariant has no uint32_t, reinterpret bits as i32.
        // This preserves the bit pattern: 0xBA63E001 → i32(-1168474111) with correct bits.
        if (isHex && uval <= 0xFFFFFFFFull)
            return static_cast<int>(static_cast<uint32_t>(uval));
        return static_cast<int64_t>(uval);
    }

    void LogErrorContext(antlr4::tree::TerminalNode* ctx, std::string errorMessage)
    {
        auto symbol = ctx->getSymbol();
        compilerLLVM->currentLine = static_cast<int>(symbol->getLine());
        compilerLLVM->currentColumn = static_cast<int>(symbol->getCharPositionInLine());
        compilerLLVM->LogError(std::move(errorMessage));
    }

    void LogErrorContext(antlr4::ParserRuleContext* ctx, std::string errorMessage)
    {
        compilerLLVM->currentLine = static_cast<int>(ctx->getStart()->getLine());
        compilerLLVM->currentColumn = static_cast<int>(ctx->getStart()->getCharPositionInLine());
        compilerLLVM->LogError(std::move(errorMessage));
    }

    void LogWarningContext(antlr4::ParserRuleContext* ctx, std::string warningMessage)
    {
        size_t line = ctx->getStart()->getLine();
        size_t column = ctx->getStart()->getCharPositionInLine();
        std::cout << std::format("{}({},{}): {}\n", sourceFileName, line, column, warningMessage);
    }

    void PrintContext(antlr4::ParserRuleContext* ctx, std::string suffix = "")
    {
        size_t line = ctx->getStart()->getLine();
        size_t column = ctx->getStart()->getCharPositionInLine();
        std::cout << std::format("[{},{}] {} : {} : {}\n", line, column, parser->getRuleNames()[ctx->getRuleIndex()], ctx->getText(), suffix);
    }

    void enterEveryRule(antlr4::ParserRuleContext* ctx) override
    {
        if constexpr (debugPrint)
            PrintContext(ctx);
    }
};
