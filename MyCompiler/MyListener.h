#pragma once
// ============================================================
// MyListener.h — CFlat front-end: ForwardRefScanner + MyListener
// ============================================================
// SECTION         LINE     DESCRIPTION
// ───────────────────────────────────────────────────────────
// §1              15-67    File-level helpers
// §2              72-195   ForwardRefScanner class (pre-pass)
// §3              200-517  MyListener class (code generation)
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
#include <cstdlib>

#include "CFlatParser.h"
#include "CFlatLexer.h"
#include "CFlatBaseListener.h"
#include "MyCompilerLLVM.h"
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
    if (opId->Greater())      return "operator>";
    if (opId->GreaterEqual()) return "operator>=";
    if (opId->LeftBracket())  return "operator[]";
    return "";
}

inline std::string getInterfaceMethodName(CFlatParser::InterfaceMethodContext* m)
{
    if (auto* opId = m->operatorFunctionId())
        return getOperatorName(opId);
    return m->directDeclarator()->getText();
}

// Normalize a generic type argument for use in mangled names (e.g. "Employee*" → "Employeeptr").
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
    MyCompilerLLVM* compilerLLVM;

    MyCompilerLLVM* Compiler(antlr4::ParserRuleContext* ctx)
    {
        compilerLLVM->SetSourceLocation(ctx->getStart()->getLine(), ctx->getStart()->getCharPositionInLine());
        return compilerLLVM;
    }

    MyCompilerLLVM::DeclTypeAndValue ParseDeclarationSpecifiers(CFlatParser::DeclarationSpecifiersContext* declSpecs)
    {
        auto* compiler = Compiler(declSpecs);
        MyCompilerLLVM::DeclTypeAndValue declType;
        for (auto declSpec : declSpecs->declarationSpecifier())
        {
            auto typeSpec = declSpec->typeSpecifier();
            auto storageSpec = declSpec->storageClassSpecifier();
                if (typeSpec != nullptr)
                {
                    // 'move' is a soft keyword — recognized as a parameter ownership qualifier
                    if (typeSpec->getText() == "move")
                    {
                        declType.IsMove = true;
                        continue;  // not a type; look for the actual type in next specifier
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
                                    MyCompilerLLVM::TypeAndValue::FuncPtrParam p;
                                    p.TypeName = param->typeSpecifier()->getText();
                                    p.Pointer = param->pointer() != nullptr;
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
                        // Generic type instantiation: Box<MyType> → Box__MyType
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
                    MyCompilerLLVM::TypeAndValue returnType{ .TypeName = mangledName };
                    compiler->CreateFunctionDeclaration(mangledName, returnType, {});
                    declType.TypeName = mangledName;
                }
                else
                {
                    declType.TypeName = compiler->ResolveQualifiedName(typeSpec->getText());
                    // Resolve type aliases (e.g. user-defined aliases)
                    declType.TypeName = compiler->ResolveTypeAlias(declType.TypeName);
                }
                declType.Pointer = declSpec->pointer() != nullptr;
                declType.ArraySize = declSpec->assignmentExpression();
                declType.IsInterface = compiler->IsInterfaceType(declType.TypeName);
                if (declType.IsInterface && declSpec->pointer() != nullptr)
                    std::cerr << std::format("error: pointer '*' is not allowed on interface type '{}'\n", declType.TypeName);
                if (declType.IsInterface) declType.Pointer = true;
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
        }
        return declType;
    }

    std::vector<MyCompilerLLVM::DeclTypeAndValue> ParseParameterTypeList(CFlatParser::ParameterTypeListContext* paramTypeList)
    {
        std::vector<MyCompilerLLVM::DeclTypeAndValue> params;
        if (paramTypeList == nullptr)
            return params;

        auto paramList = paramTypeList->parameterList();
        for (auto paramDecl : paramList->parameterDeclaration())
        {
            MyCompilerLLVM::DeclTypeAndValue paramType = ParseDeclarationSpecifiers(paramDecl->declarationSpecifiers());
            if (auto declarer = paramDecl->declarator())
                if (auto directDeclarer = declarer->directDeclarator())
                    paramType.VariableName = directDeclarer->getText();
            paramType.DefaultValue = paramDecl->assignmentExpression();
            params.push_back(paramType);
        }
        return params;
    }

    void ScanFunctionDefinition(CFlatParser::FunctionDefinitionContext* func, const std::string& structName = {}, const std::string& namespaceName = {})
    {
        auto* compiler = Compiler(func);
        // Return-block functions are inlined at call sites — no LLVM proto needed.
        if (IsReturnBlockFunction(func))
            return;

        // Generic function templates are instantiated on demand — skip pre-declaration.
        if (func->genericTypeParameters() != nullptr)
            return;

        std::string name = getFunctionName(func);
        if (!namespaceName.empty())
            name = namespaceName + "." + name;

        auto returnType = ParseDeclarationSpecifiers(func->declarationSpecifiers());
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
            MyCompilerLLVM::DeclTypeAndValue thisParam;
            thisParam.TypeName = structName;
            thisParam.VariableName = structName + "__";
            thisParam.Pointer = true;
            params.insert(params.begin(), thisParam);
        }

        std::vector<MyCompilerLLVM::TypeAndValue> allParams(params.begin(), params.end());

        // Detect functions that heap-allocate and return a new string buffer.
        // operator+ always allocates; operator string(i32) uses malloc; user functions can
        // opt in by declaring 'move string' as their return type.
        bool returnsOwnedString = false;
        if (returnType.TypeName == "string")
        {
            if (name == "operator+")
                returnsOwnedString = true;
            else if (name == "operator string" && allParams.size() == 1 && allParams[0].TypeName == "i32")
                returnsOwnedString = true;
            else if (returnType.IsMove)
                returnsOwnedString = true;
        }

        compiler->CreateFunctionDeclaration(name, returnType, allParams, returnType.external, varargs, returnsOwnedString);

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
            s->Register(SymbolKind::Function, name, compiler->GetSourceFileName(),
                        (int)func->getStart()->getLine(), (int)func->getStart()->getCharPositionInLine(), sig);

            // Also register under "TypeName.method" for dot-completion prefix lookup.
            // Operators and statics already have the qualified name; only instance methods need this.
            if (!structName.empty() && !isOperatorFunc && !isStaticFunc)
            {
                std::string qualName = structName + "." + getFunctionName(func);
                s->Register(SymbolKind::Function, qualName, compiler->GetSourceFileName(),
                            (int)func->getStart()->getLine(), (int)func->getStart()->getCharPositionInLine(), sig);
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
            std::vector<MyCompilerLLVM::TypeAndValue> wrapperParams(params.begin(), params.begin() + cutoff);
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

        std::vector<MyCompilerLLVM::InterfaceMethod> methods;
        for (auto method : ctx->interfaceMethod())
        {
            MyCompilerLLVM::InterfaceMethod m;
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
            s->Register(SymbolKind::Interface, name, Compiler(ctx)->GetSourceFileName(),
                        (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine(),
                        sig, memberNames);
        }
    }

    // Pre-declare a struct or class type shell, member functions, and destructor.
    // Templated to handle both StructDefinitionContext and ClassDefinitionContext.
    template<typename TCtx>
    void ScanStructOrClassDefinition(TCtx* ctx, const std::string& namespaceName = {})
    {
        auto* compiler = Compiler(ctx);
        // Generic template definitions are not pre-declared; they are instantiated on demand.
        if (ctx->genericTypeParameters() != nullptr)
            return;

        std::string typeName = ctx->directDeclarator()->getText();
        if (!namespaceName.empty())
            typeName = namespaceName + "." + typeName;

        // Register opaque struct so the type is known for pointer/field use
        compiler->CreateStructType(typeName, {});

        if (auto* s = compiler->GetSymbolSink())
            s->Register(SymbolKind::Struct, typeName, compiler->GetSourceFileName(),
                        (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine(),
                        "struct " + typeName);

        // Pre-declare default constructor
        MyCompilerLLVM::TypeAndValue returnType{ .TypeName = typeName };
        compiler->CreateFunctionDeclaration(typeName, returnType, {});

        // Pre-declare member functions (and detect constructor overloads)
        for (auto func : ctx->functionDefinition())
        {
            if (getFunctionName(func) == typeName)
            {
                // Constructor overload — no implicit this* parameter, returns the type
                if (!func->parameterTypeList()) continue; // no-arg already declared above
                auto ctorParams = ParseParameterTypeList(func->parameterTypeList());
                std::vector<MyCompilerLLVM::TypeAndValue> allCtorParams(ctorParams.begin(), ctorParams.end());
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
            MyCompilerLLVM::DeclTypeAndValue thisParam;
            thisParam.TypeName = typeName;
            thisParam.VariableName = typeName + "__";
            thisParam.Pointer = true;
            MyCompilerLLVM::TypeAndValue voidReturn{ .TypeName = "void" };
            compiler->CreateFunctionDeclaration("~" + typeName, voidReturn, { thisParam });
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
    ForwardRefScanner(MyCompilerLLVM* compiler) : compilerLLVM(compiler) {}

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

            // Skip generic template definitions entirely — their bodies contain
            // unbound type parameters (e.g. T) that are not valid type names.
            if (auto* structDef = dynamic_cast<CFlatParser::StructDefinitionContext*>(ruleCtx))
            {
                if (structDef->genericTypeParameters() != nullptr)
                    continue;
            }
            if (auto* classDef = dynamic_cast<CFlatParser::ClassDefinitionContext*>(ruleCtx))
            {
                if (classDef->genericTypeParameters() != nullptr)
                    continue;
            }

            // Skip generic function template definitions for the same reason.
            if (auto* funcDef = dynamic_cast<CFlatParser::FunctionDefinitionContext*>(ruleCtx))
            {
                if (funcDef->genericTypeParameters() != nullptr)
                    continue;
            }

            auto tryPreDeclare = [&](const std::string& baseName, CFlatParser::GenericTypeParametersContext* genericParams)
            {
        auto* compiler = Compiler(genericParams);
                std::string mangledName = baseName;
                for (auto* entry : genericParams->typeParameterList()->typeParameterEntry())
                    mangledName += "__" + MangleTypeArg(entry->getText());
                compiler->CreateStructType(mangledName, {});
                MyCompilerLLVM::TypeAndValue returnType{ .TypeName = mangledName };
                compiler->CreateFunctionDeclaration(mangledName, returnType, {});
            };

            if (auto* typeSpec = dynamic_cast<CFlatParser::TypeSpecifierContext*>(ruleCtx))
            {
                if (typeSpec->genericIdentifier() != nullptr && typeSpec->genericIdentifier()->genericTypeParameters() != nullptr && typeSpec->genericIdentifier()->Identifier() != nullptr)
                    tryPreDeclare(typeSpec->genericIdentifier()->Identifier()->getText(), typeSpec->genericIdentifier()->genericTypeParameters());
            }

            if (auto* primaryExpr = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(ruleCtx))
            {
                if (primaryExpr->genericIdentifier() != nullptr && primaryExpr->genericIdentifier()->genericTypeParameters() != nullptr && primaryExpr->genericIdentifier()->Identifier() != nullptr)
                    tryPreDeclare(primaryExpr->genericIdentifier()->Identifier()->getText(), primaryExpr->genericIdentifier()->genericTypeParameters());
            }

            ScanGenericTypeUses(ruleCtx);
        }
    }

    void ScanUsingDeclaration(CFlatParser::UsingDeclarationContext* ctx)
    {
        auto* compiler = Compiler(ctx);
        auto identifiers = ctx->Identifier();

        std::string alias;
        if (ctx->String())
            alias = ctx->String()->getText();
        else if (!identifiers.empty())
            alias = identifiers[0]->getText();

        std::string target;
        size_t start = ctx->String() ? 0 : 1;
        for (size_t i = start; i < identifiers.size(); i++)
        {
            if (!target.empty()) target += ".";
            target += identifiers[i]->getText();
        }

        if (compiler->IsInterfaceType(target) || compiler->dataStructures.count(target) > 0)
            compiler->RegisterTypeAlias(alias, target);
    }

    void ScanProgramDefinition(CFlatParser::ProgramDefinitionContext* ctx)
    {
        auto* compiler = Compiler(ctx);
        std::string name = ctx->directDeclarator()->getText();

        // Register opaque struct shell and default constructor
        compiler->CreateStructType(name, {});
        MyCompilerLLVM::TypeAndValue returnType{ .TypeName = name };
        compiler->CreateFunctionDeclaration(name, returnType, {});

        // Pre-declare trampoline: int __program_run_Name(void*)
        {
            MyCompilerLLVM::TypeAndValue intReturn{ .TypeName = "int" };
            MyCompilerLLVM::DeclTypeAndValue ctxParam;
            ctxParam.TypeName = "void";
            ctxParam.VariableName = "ctx";
            ctxParam.Pointer = true;
            compiler->CreateFunctionDeclaration("__program_run_" + name, intReturn, { ctxParam });
        }

        // Pre-declare run(Name* this, list__string args) -> bool
        {
            MyCompilerLLVM::TypeAndValue boolReturn{ .TypeName = "bool" };
            MyCompilerLLVM::DeclTypeAndValue thisParam;
            thisParam.TypeName = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer = true;
            MyCompilerLLVM::DeclTypeAndValue argsParam;
            argsParam.TypeName = "list__string";
            argsParam.VariableName = "args";
            compiler->CreateFunctionDeclaration("run", boolReturn, { thisParam, argsParam });
        }

        // Pre-declare WaitForExit(Name* this) -> void
        {
            MyCompilerLLVM::TypeAndValue voidReturn{ .TypeName = "void" };
            MyCompilerLLVM::DeclTypeAndValue thisParam;
            thisParam.TypeName = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer = true;
            compiler->CreateFunctionDeclaration("WaitForExit", voidReturn, { thisParam });
        }

        // Pre-declare WaitForExit(Name* this, stop_token token) -> bool
        {
            MyCompilerLLVM::TypeAndValue boolReturn{ .TypeName = "bool" };
            MyCompilerLLVM::DeclTypeAndValue thisParam;
            thisParam.TypeName     = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer      = true;
            MyCompilerLLVM::DeclTypeAndValue tokenParam;
            tokenParam.TypeName     = "stop_token";
            tokenParam.VariableName = "token";
            compiler->CreateFunctionDeclaration("WaitForExit", boolReturn, { thisParam, tokenParam });
        }

        // Pre-declare member functions (including user's main) and destructor
        for (auto func : ctx->functionDefinition())
            ScanFunctionDefinition(func, name);
        for (auto dtor : ctx->destructorDefinition())
        {
            MyCompilerLLVM::TypeAndValue voidReturn{ .TypeName = "void" };
            MyCompilerLLVM::DeclTypeAndValue thisParam;
            thisParam.TypeName = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer = true;
            compiler->CreateFunctionDeclaration("~" + name, voidReturn, { thisParam });
        }
    }

    void ScanExternalDeclaration(CFlatParser::ExternalDeclarationContext* ctx, const std::string& namespaceName = {})
    {
        if (auto ns = ctx->namespaceDefinition())
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
        else if (auto expectErrDecl = ctx->expectErrorDeclaration())
        {
            // Scan function/struct definitions inside the expect_error block so forward refs work.
            for (auto* extDecl : expectErrDecl->externalDeclaration())
                ScanExternalDeclaration(extDecl, namespaceName);
        }
        // if const declarations are skipped here; they are handled in MyListener
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


class MyListener : public CFlatBaseListener
{
private:
    CFlatParser* parser;
    MyCompilerLLVM* compilerLLVM;
    std::string sourceFileName;

    MyCompilerLLVM* Compiler(antlr4::ParserRuleContext* ctx)
    {
        if (ctx)
            compilerLLVM->SetSourceLocation(ctx->getStart()->getLine(), ctx->getStart()->getCharPositionInLine());
        return compilerLLVM;
    }
    inline MyCompilerLLVM* Compiler() { return compilerLLVM; }

    std::unordered_map<llvm::Value*, int> PlusPlus;
    bool global_scope = true; // true when parsing an entity in the global scope.

    // Lambda state: expected type (set by ParseDeclaration before evaluating RHS)
    // and the last lambda's TypeAndValue (side-channel from ParsePrimaryExpression to ParsePostfixExpression).
    MyCompilerLLVM::TypeAndValue lambdaExpectedType;
    MyCompilerLLVM::TypeAndValue lastLambdaType;

    // Variadic forwarding: true when the current function being codegen'd accepts '...'
    bool currentFunctionIsVariadic = false;

    // Generic template state is shared across all MyListener instances so that
    // templates declared in an imported file remain visible when the importing
    // file needs to instantiate them.
    static inline std::unordered_map<std::string, CFlatParser::StructDefinitionContext*> genericStructTemplates;
    static inline std::unordered_map<std::string, CFlatParser::ClassDefinitionContext*> genericClassTemplates;
    static inline std::unordered_map<std::string, std::vector<std::string>> genericStructTypeParams;
    static inline std::unordered_set<std::string> instantiatedGenerics;
    // Constraints: templateName → { typeParamName → [requiredInterface, …] }
    static inline std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>> genericStructConstraints;
    static inline std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>> genericClassConstraints;
    // Active type parameter substitutions during generic instantiation (e.g. "T" -> "int")
    std::unordered_map<std::string, std::string> activeTypeSubstitutions;

    static inline std::unordered_map<std::string, CFlatParser::InterfaceDefinitionContext*> genericInterfaceTemplates;
    static inline std::unordered_map<std::string, std::vector<std::string>> genericInterfaceTypeParams;
    static inline std::unordered_set<std::string> instantiatedInterfaces;

    static inline std::unordered_map<std::string, CFlatParser::FunctionDefinitionContext*> genericFunctionTemplates;
    static inline std::unordered_map<std::string, std::vector<std::string>> genericFunctionTypeParams;
    static inline std::unordered_set<std::string> instantiatedGenericFunctions;
    static inline std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>> genericFunctionConstraints;

    // Queue for pending generic instantiations (delayed until safe to emit code)
    struct PendingInstantiation
    {
        std::string templateName;
        std::vector<std::string> typeArgs;
        std::string mangledName;
    };
    static inline std::vector<PendingInstantiation> pendingInstantiations;

    constexpr static bool debugPrint = false;

    struct SwitchCaseEntry
    {
        llvm::ConstantInt* value = nullptr;       // non-null for integer cases
        llvm::Constant* strLiteral = nullptr;     // non-null for string cases (i8* global)
        llvm::BasicBlock* block;
        bool isTypeCase = false;                  // non-null for type cases (struct or interface)
        std::string typeCaseName;                 // struct or interface name for type cases
    };

    struct SwitchContext
    {
        std::unordered_map<CFlatParser::LabeledStatementContext*, SwitchCaseEntry> caseMap;
        llvm::BasicBlock* defaultBlock = nullptr;
        llvm::BasicBlock* resumeBlock = nullptr;
        bool isStringSwitch = false;
        bool isTypeSwitch = false;                // true if this switch contains type cases
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

    MyCompilerLLVM::DeclTypeAndValue ParseDeclarationSpecifiers(CFlatParser::DeclarationSpecifiersContext* declSpecs)
    {
        MyCompilerLLVM::DeclTypeAndValue declType;
        std::string typeName;
        auto declSpecList = declSpecs->declarationSpecifier();

        for (auto declSpec : declSpecList)
        {
            auto typeSpec = declSpec->typeSpecifier();
            auto storageSpec = declSpec->storageClassSpecifier();
            if (typeSpec != nullptr)
            {
                // 'move' is a soft keyword — recognized as a parameter ownership qualifier
                if (typeSpec->getText() == "move")
                {
                    declType.IsMove = true;
                    continue;  // not a type; look for the actual type in next specifier
                }
                // function pointer type: function<RetType(Params)> or bare 'function'
                if (typeSpec->functionPointerSpecifier() != nullptr)
                {
                    auto* fpSpec = typeSpec->functionPointerSpecifier();
                    declType.IsFunctionPointer = true;
                    if (fpSpec->typeSpecifier() != nullptr)
                    {
                        declType.FuncPtrReturnTypeName = fpSpec->typeSpecifier()->getText();
                        // Apply active generic type substitutions (e.g. T → int inside list<int>)
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
                                MyCompilerLLVM::TypeAndValue::FuncPtrParam p;
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
                                MyCompilerLLVM::TypeAndValue rt{ .TypeName = mangledName };
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
                    // Resolve type aliases (e.g. user-defined aliases)
                    typeName = Compiler(declSpecs)->ResolveTypeAlias(typeName);
                    declType.TypeName = typeName;
                    if (substPointer) declType.Pointer = true;
                }
                bool hasExplicitPointer = declSpec->pointer() != nullptr;
                // When the substituted type is already a pointer AND there is an explicit '*',
                // the result is pointer-to-pointer (e.g. T* where T=Employee* → Employee**).
                if (declType.Pointer && hasExplicitPointer)
                    declType.ElemPointer = true;
                declType.Pointer = hasExplicitPointer || declType.Pointer;
                declType.ArraySize = declSpec->assignmentExpression();
                declType.IsInterface = Compiler(declSpecs)->IsInterfaceType(declType.TypeName);
                if (declType.IsInterface && hasExplicitPointer)
                    LogErrorContext(declSpec, std::format("pointer '*' is not allowed on interface type '{}'", declType.TypeName));
                if (declType.IsInterface) declType.Pointer = true;
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
        }

        return declType;
    }

    MyCompilerLLVM::DeclTypeAndValue getFunctionReturnType(CFlatParser::FunctionDefinitionContext* ctx)
    {
        auto declSpecs = ctx->declarationSpecifiers();

        return ParseDeclarationSpecifiers(declSpecs);
    }

    // Returns the default value for a type:
    //   - struct types (local scope): calls the default constructor.
    //   - everything else (or global scope): zero-initializes.
    llvm::Value* GenerateDefaultValue(const MyCompilerLLVM::DeclTypeAndValue& typeValue)
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
    MyListener(CFlatParser* parser, MyCompilerLLVM* compilerLLVM, const std::string& filename)
    {
        this->parser = parser;
        this->compilerLLVM = compilerLLVM;
        this->sourceFileName = filename;
    }

    static void ClearGenericCaches()
    {
        genericStructTemplates.clear();
        genericClassTemplates.clear();
        genericStructTypeParams.clear();
        instantiatedGenerics.clear();
        genericStructConstraints.clear();
        genericClassConstraints.clear();
        genericInterfaceTemplates.clear();
        genericInterfaceTypeParams.clear();
        instantiatedInterfaces.clear();
        genericFunctionTemplates.clear();
        genericFunctionTypeParams.clear();
        genericFunctionConstraints.clear();
        instantiatedGenericFunctions.clear();
        pendingInstantiations.clear();
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

        // Generic interface template — store for on-demand instantiation
        if (nameGid->genericTypeParameters() != nullptr)
        {
            auto typeParams = ParseGenericTypeParameters(nameGid->genericTypeParameters());
            genericInterfaceTemplates[name] = ctx;
            genericInterfaceTypeParams[name] = typeParams;
            return;
        }

        std::vector<MyCompilerLLVM::InterfaceMethod> methods;
        for (auto method : ctx->interfaceMethod())
        {
            MyCompilerLLVM::InterfaceMethod m;
            m.ReturnType = ParseDeclarationSpecifiers(method->declarationSpecifiers());
            m.Name = getInterfaceMethodName(method);
            auto declParams = ParseParameterTypeList(method->parameterTypeList());
            for (const auto& p : declParams)
            {
                MyCompilerLLVM::TypeAndValue tv = p;
                m.Parameters.push_back(tv);
            }
            methods.push_back(std::move(m));
        }

        Compiler(ctx)->CreateInterfaceDefinition(name, parentNames, methods);
    }

    void InstantiateGenericInterface(const std::string& baseName, const std::string& mangledName,
                                     const std::unordered_map<std::string, std::string>& substitutions)
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
        for (const auto& [k, v] : substitutions)
            activeTypeSubstitutions[k] = v;

        std::vector<MyCompilerLLVM::InterfaceMethod> methods;
        for (auto method : ctx->interfaceMethod())
        {
            MyCompilerLLVM::InterfaceMethod m;
            m.ReturnType = ParseDeclarationSpecifiers(method->declarationSpecifiers());
            m.Name = getInterfaceMethodName(method);
            auto declParams = ParseParameterTypeList(method->parameterTypeList());
            for (const auto& p : declParams)
            {
                MyCompilerLLVM::TypeAndValue tv = p;
                m.Parameters.push_back(tv);
            }
            methods.push_back(std::move(m));
        }

        activeTypeSubstitutions = savedSubst;
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
        auto savedState = Compiler(templateIt->second)->SaveBuilderState();
        ParseFunctionDefinition(templateIt->second, {}, {}, mangledName);
        Compiler(templateIt->second)->RestoreBuilderState(savedState);

        activeTypeSubstitutions = savedSubst;
        return mangledName;
    }

    // Infer the type arguments for a generic function template from the receiver's
    // interface type name (e.g. "Container__int" → T="int") and instantiate it.
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

    void ParseUsingDeclaration(CFlatParser::UsingDeclarationContext* ctx)
    {
        auto* compiler = Compiler(ctx);
        auto identifiers = ctx->Identifier();

        // The alias name may be a plain Identifier or the 'string' keyword token.
        std::string alias;
        if (ctx->String())
            alias = ctx->String()->getText();
        else if (!identifiers.empty())
            alias = identifiers[0]->getText();

        // Build the target from the remaining identifiers.
        std::string target;
        size_t start = ctx->String() ? 0 : 1;
        for (size_t i = start; i < identifiers.size(); i++)
        {
            if (!target.empty()) target += ".";
            target += identifiers[i]->getText();
        }

        // If the target names a known type (interface or struct), register a type alias.
        // Otherwise treat it as a namespace alias.
        if (compiler->IsInterfaceType(target) || compiler->GetDataStructure(target).StructType != nullptr)
        {
            compiler->RegisterTypeAlias(alias, target);
            if (auto* s = compiler->GetSymbolSink())
                s->Register(SymbolKind::TypeAlias, alias, compiler->GetSourceFileName(),
                            (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine(),
                            "using " + alias + " = " + target);
        }
        else if (global_scope)
            compiler->RegisterNamespaceAlias(alias, target);
        else
            compiler->RegisterLocalNamespaceAlias(alias, target);
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
                    exit(1);
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

        // First pass: forward ref scan the taken branch to register symbols
        ForwardRefScanner scanner(Compiler());
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
        Compiler(ctx)->RegisterNamespace(namespaceName);

        for (auto* extDecl : ctx->externalDeclaration())
            ParseExternalDeclaration(extDecl, namespaceName);
    }

    void enterExternalDeclaration(CFlatParser::ExternalDeclarationContext* ctx) override
    {
        if constexpr (debugPrint)
            return;

        // Skip nodes nested inside a namespace — they are handled by ParseNamespaceDefinition.
        if (dynamic_cast<CFlatParser::NamespaceDefinitionContext*>(ctx->parent))
            return;

        // Skip nodes nested inside an if const block — they are handled by ParseIfConstDeclaration.
        if (dynamic_cast<CFlatParser::IfConstBlockContext*>(ctx->parent))
            return;

        // Skip nodes nested inside an expect_error block — handled by ParseExternalDeclaration's
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

            if (decl != nullptr)
            {
                ParseDeclaration(decl);
            }
            else if (statement != nullptr)
            {
                ParseStatement(statement);
            }
            else if (usingDecl != nullptr)
            {
                ParseUsingDeclaration(usingDecl);
            }
        }
    }

    // Recursively collects case/default labels from a statement (to handle `case 1: case 2: stmt` nesting).
    void CollectCasesFromStatement(CFlatParser::StatementContext* stmt, SwitchContext& ctx)
    {
        auto* compiler = Compiler(stmt);
        if (!stmt) return;
        auto labeled = stmt->labeledStatement();
        if (!labeled) return;

        if (labeled->Case())
        {
            // Check if this is a type case (struct or interface name) before evaluating as constant
            auto constExpr = labeled->constantExpression();
            if (!constExpr)
            {
                LogErrorContext(labeled, "case must have an expression");
                return;
            }

            std::string exprText = constExpr->getText();
            bool isStruct = compiler->dataStructures.count(exprText) > 0;
            bool isInterface = compiler->interfaceTable.count(exprText) > 0;

            if (isStruct || isInterface)
            {
                // Type case detected
                if (!ctx.isTypeSwitch && !ctx.caseMap.empty())
                    LogErrorContext(labeled, "cannot mix type cases with constant cases in a switch");
                ctx.isTypeSwitch = true;
                ctx.caseMap[labeled] = { nullptr, nullptr, compiler->CreateBasicBlock("switchTypeCase"), true, exprText };
                CollectCasesFromStatement(labeled->statement(), ctx);
            }
            else
            {
                // Constant case (integer or string)
                if (ctx.isTypeSwitch)
                    LogErrorContext(labeled, "cannot mix constant cases with type cases in a switch");

                llvm::Value* rawVal = ParseConditionalExpression(labeled->constantExpression()->conditionalExpression());
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
                ctx.caseMap[labeled] = { val, strLit, compiler->CreateBasicBlock("switchCase"), false, "" };
                CollectCasesFromStatement(labeled->statement(), ctx);
            }
        }
        else if (labeled->Default())
        {
            ctx.defaultBlock = compiler->CreateBasicBlock("switchDefault");
            CollectCasesFromStatement(labeled->statement(), ctx);
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

            ParseStatement(labeledStatement->statement());
            return;
        }

        if (jump != nullptr)
        {
            if (jump->Return())
            {
                if (auto* blockBody = jump->compoundStatement())
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
                        auto right = ParseExpression(express);
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

            if (iterationStatement->While())
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
            else if (iterationStatement->Do())
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

                    // Push scope; continue → increment, break/else → resume
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

                    bool isFaceType = compiler->IsInterfaceType(collNV.TypeAndValue.TypeName);

                    // Call count() once and cache it
                    llvm::Value* countVal = nullptr;
                    llvm::Value* ifacePtr = nullptr;
                    if (isFaceType)
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
                        MyCompilerLLVM::NamedVariable selfArg = collNV;
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

                    MyCompilerLLVM::NamedVariable indexNV;
                    indexNV.Primary  = compiler->CreateLoad(indexAlloca);
                    indexNV.BaseType = i32Ty;
                    indexNV.TypeAndValue.TypeName = "int";

                    llvm::Value* elemVal = nullptr;
                    if (isFaceType)
                    {
                        elemVal = compiler->CallInterfaceMethod(ifacePtr, collNV.TypeAndValue.TypeName, "get", { indexNV });
                    }
                    else
                    {
                        MyCompilerLLVM::NamedVariable selfArg = collNV;
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

                compiler->InitializeBlock(blockTrue, false);
                ParseStatement(innerStatement[0]);

                compiler->CreateBlockBreak(blockResume, true);

                if (blockElse != nullptr)
                {
                    // else statement
                    compiler->InitializeBlock(blockElse, true);
                    ParseStatement(innerStatement[1]);
                    compiler->CreateBlockBreak(blockResume, true);
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
                        else if (compiler->interfaceTable.count(entry.typeCaseName))
                        {
                            // Interface case: match if the concrete type implements this interface
                            // Emit: if (typedesc == any_implementing_struct_typedesc) goto case block
                            llvm::BasicBlock* anyMatchedBlock = entry.block;
                            auto* nextStruct = nextCheck;

                            // Enumerate all classes that implement this interface
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
                        switchInst->addCase(compiler->CoerceCaseValue(entry.value, condVal->getType()), entry.block);
                }

                // Push scope: break → resumeBlock, no continue (propagates to outer loop)
                compiler->InitializeBlock(nullptr, true, nullptr, switchCtx.resumeBlock, nullptr);

                switchStack.push_back(switchCtx);

                if (body && body->blockItemList())
                    ParseBlockItemList(body->blockItemList());

                // Fallthrough at end of switch body → resume
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
                // Scoped block form: expect_error("msg") { ... } — error must occur inside the braces.
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
                            exit(1);
                    }
                }
            }
            else
            {
                // Bare-semicolon form: expect_error("msg"); — error must occur before the enclosing scope exits.
                compilerLLVM->expectedErrorScopeDepth = compilerLLVM->stackNamedVariable.size();
            }
            return;
        }

        else if (auto* lockStmt = statement->lockStatement())
        {
            // lock (expr) { body }
            // 1. Evaluate the mutex expression — get the NamedVariable so we have its Storage.
            auto* exprCtx = lockStmt->expression();
            auto mutexNV = ParseAssignmentExpressionNamed(exprCtx->assignmentExpression());

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
                LogErrorContext(lockStmt, "lock: expression must be a mutex variable.");
                return;
            }

            // 2. Call mutex.acquire().
            MyCompilerLLVM::NamedVariable selfArg = mutexNV;
            selfArg.TypeAndValue.VariableName = "";
            compiler->CreateOverloadedFunctionCall("acquire", { selfArg });

            // 3. Find the release function for cleanup on scope exit.
            std::string mutexTypeName = mutexNV.TypeAndValue.TypeName;
            llvm::Function* unlockFn = FindMethodOf("release", mutexTypeName);
            if (!unlockFn)
            {
                LogErrorContext(lockStmt, std::format("lock: type '{}' has no 'release' method.", mutexTypeName));
                return;
            }

            // 4. Push a new scope with lockCleanup set so any exit (return, scope-close)
            //    automatically calls unlock().
            compiler->InitializeBlock(nullptr, true);
            compiler->stackNamedVariable.back().lockCleanup = MyCompilerLLVM::StackState::LockCleanup{
                .UnlockFn  = unlockFn,
                .MutexPtr  = mutexNV.Storage,
            };

            // 5. Parse the body.
            auto* blockList = lockStmt->compoundStatement()->blockItemList();
            if (blockList)
                ParseBlockItemList(blockList);

            // 6. Close the scope — EmitDestructorsForScope will call unlock().
            compiler->CreateBlockBreak(nullptr, true);
            return;
        }

        LogErrorContext(statement, "Unhandled statement type.");
        return;
    }

    void GenerateDefaultParamOverloads(
        const std::string& name,
        const MyCompilerLLVM::DeclTypeAndValue& returnType,
        const std::vector<MyCompilerLLVM::DeclTypeAndValue>& params,
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
            std::vector<MyCompilerLLVM::TypeAndValue> wrapperParams(params.begin(), params.begin() + cutoff);

            auto wrapperFn = compiler->CreateFunctionDefinition(name, returnType, wrapperParams, false, false, line);
            compiler->InitializeBlock(&wrapperFn->front(), false);

            // Build the full argument list for the forwarding call
            std::vector<MyCompilerLLVM::NamedVariable> callArgs;

            for (int i = 0; i < cutoff; i++)
            {
                callArgs.push_back(compiler->GetFunctionArgument(params[i].VariableName));
            }

            for (int i = cutoff; i < (int)params.size(); i++)
            {
                auto defaultVal = ParseAssignmentExpression(params[i].DefaultValue);
                MyCompilerLLVM::NamedVariable namedVar;
                namedVar.Primary = defaultVal;
                namedVar.BaseType = defaultVal ? defaultVal->getType() : nullptr;
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
            MyCompilerLLVM::DeclTypeAndValue typeValue;
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

        std::vector<MyCompilerLLVM::TypeAndValue> allParams(params.begin(), params.end());

        // Pre-scan declarations in the function body to queue and emit any generic
        // struct instantiations before the function's IR block is opened.
        // At this point no basic block is active, so it is safe to emit new functions.
        if (auto* blockItemList = func->compoundStatement()->blockItemList())
        {
            ScanAndQueueGenericTypeUses(blockItemList);
            ProcessPendingInstantiations();
        }

        auto fn = compiler->CreateFunctionDefinition(name, returnType, allParams, returnType.external, varargs, line);

        compiler->InitializeBlock(&fn->front(), false);

        currentFunctionIsVariadic = varargs;

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
                // File-scope scoped-block form sets expectedErrorScopeDepth = SIZE_MAX — re-throw.
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

        GenerateDefaultParamOverloads(name, returnType, params, varargs, line);
    }

    std::vector<MyCompilerLLVM::DeclTypeAndValue> ParseDeclarationList(std::vector<CFlatParser::DeclarationContext*> ctx)
    {
        std::vector<MyCompilerLLVM::DeclTypeAndValue> result;

        if (ctx.size() > 0)
        {
            for (auto decl : ctx)
            {
                auto direct = decl->declarationSpecifiers();
                auto typeAndValue = ParseDeclarationSpecifiers(direct);

                auto initDeclList = decl->initDeclaratorList()->initDeclarator();
                for (auto initDecl : initDeclList)
                {
                    auto declarator = initDecl->declarator();
                    auto initializer = initDecl->initializer();

                    std::string name = declarator->directDeclarator()->getText();
                    typeAndValue.VariableName = name;
                    typeAndValue.Initializer = initializer;

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

            MyCompilerLLVM::TypeAndValue tv;
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
                auto declParams = paramTypeList ? ParseParameterTypeList(paramTypeList) : std::vector<MyCompilerLLVM::DeclTypeAndValue>{};
                std::vector<MyCompilerLLVM::TypeAndValue> allParams(declParams.begin(), declParams.end());

                bool ellipsis = paramTypeList && paramTypeList->Ellipsis() != nullptr;
                compiler->CreateFunctionDeclaration(direct->getText(), typeAndValue, allParams, typeAndValue.external, ellipsis);

                // Declare overloads for each suffix of omitted default parameters
                int firstDefault = -1;
                for (int i = 0; i < (int)declParams.size(); i++)
                {
                    if (declParams[i].DefaultValue != nullptr) { firstDefault = i; break; }
                }
                for (int cutoff = firstDefault; firstDefault >= 0 && cutoff < (int)declParams.size(); cutoff++)
                {
                    std::vector<MyCompilerLLVM::TypeAndValue> wrapperParams(declParams.begin(), declParams.begin() + cutoff);
                    compiler->CreateFunctionDeclaration(direct->getText(), typeAndValue, wrapperParams, typeAndValue.external, false);
                }
            }
            else if (direct != nullptr)
            {
                auto identList = declarator->identifierList();
                std::string name = direct->getText();
                typeAndValue.VariableName = name;

                if (identList != nullptr)
                {
                    // TODO
                    LogErrorContext(identList, "Not Yet Implemented.");
                }

                llvm::Value* right = nullptr;
                bool srcIsUnsigned = false;
                auto initializer = initDecl->initializer();
                if (initializer != nullptr)
                {
                    auto assignmentExpression = initializer->assignmentExpression();
                    if (assignmentExpression != nullptr)
                    {
                        if (typeAndValue.IsInterface)
                        {
                            // For interface (string) declarations, preserve NamedVariable type info
                            // so we can do the struct→interface fat-struct upcast when needed.
                            auto rightNV = ParseAssignmentExpressionNamed(assignmentExpression);
                            right = LoadNamedVariable(rightNV);
                            // Classes that implement interfaces can be upcast. Structs cannot.
                            if (right && right->getType() != compiler->GetFatPtrType())
                            {
                                std::string structName = rightNV.TypeAndValue.TypeName;
                                if (!structName.empty() && compiler->StructImplementsInterface(structName, typeAndValue.TypeName))
                                {
                                    // Only classes (not structs) can implement interfaces, so this is a class
                                    auto vtable = compiler->GetOrCreateVTable(structName, typeAndValue.TypeName);
                                    // BuildInterfaceFatValue needs a *pointer* to the struct data,
                                    // not the loaded struct value.
                                    // - Pointer types (e.g. StringData*): the loaded value IS the pointer → use it directly.
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
                                        MyCompilerLLVM::NamedVariable argNV;
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
                        }
                        else
                        {
                            // Thread expected function-pointer type into lambda expression parsing.
                            if (typeAndValue.IsFunctionPointer)
                                lambdaExpectedType = typeAndValue;
                            {
                                auto rightNV = ParseAssignmentExpressionNamed(assignmentExpression);
                                right = LoadNamedVariable(rightNV);
                                srcIsUnsigned = rightNV.TypeAndValue.IsUnsignedInteger() != -1;
                            }
                            lambdaExpectedType = {};
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
                        }
                    }
                    else if (initializer->Default() != nullptr)
                    {
                        right = GenerateDefaultValue(typeAndValue);
                    }
                }

                if (right == nullptr && typeAndValue.IsNullable)
                {
                    // Nullable pointer defaults to null when no initializer is provided.
                    llvm::Type* ptrTy = compiler->GetType(typeAndValue);
                    right = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
                }

                if (right == nullptr && !typeAndValue.Pointer)
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
                    compiler->CreateGlobalVariable(typeAndValue, constant, typeAndValue.threadLocal);
                }
                else
                {
                    if (typeAndValue.threadLocal)
                        LogErrorContext(direct, "thread_local is only allowed on global variables.");
                    auto alloc = compiler->CreateLocalVariable(typeAndValue, right ? right->getType() : nullptr, arraySize, line);
                    allocList.push_back(std::pair(name, alloc));

                    if (right != nullptr)
                    {
                        compiler->CreateAssignment(right, alloc, srcIsUnsigned);

                        // Propagate ownership: if the RHS was a heap-allocating string call,
                        // mark this local as owning so the destructor frees the buffer on scope exit.
                        if (typeAndValue.TypeName == "string" && compiler->lastCallReturnsOwnedString)
                        {
                            auto& nv = compiler->stackNamedVariable.back().namedVariable[name];
                            nv.IsOwningString = true;
                            compiler->lastCallReturnsOwnedString = false;
                        }

                        // Propagate new-allocation: mark local as owning its heap pointer.
                        if (compiler->lastNewAllocated)
                        {
                            compiler->stackNamedVariable.back().namedVariable[name].IsNewAllocated = true;
                            compiler->lastNewAllocated = false;
                        }
                    }
                }
            }
        }

        return allocList;
    }

    // Returns a NamedVariable (preserving TypeName) for simple single-child expression chains.
    // Used by ParseDeclaration to get the struct TypeName for struct→interface upcasting.
    // Falls back to value-only for complex expressions (ternary, binary ops, etc.).
    MyCompilerLLVM::NamedVariable ParseAssignmentExpressionNamed(CFlatParser::AssignmentExpressionContext* ctx)
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
                                    // operators — those must be handled by ParseConditionalExpression.
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
            MyCompilerLLVM::NamedVariable result;
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
            auto isDerefStorage = [&]() {
                return namedVar.BaseType
                    && !llvm::isa<llvm::AllocaInst>(destination)
                    && !llvm::isa<llvm::GlobalVariable>(destination)
                    && !llvm::isa<llvm::GetElementPtrInst>(destination);
            };
            auto derefLoad = [&]() -> llvm::Value* {
                return isDerefStorage()
                    ? compiler->CreateLoad(namedVar.BaseType, destination)
                    : compiler->CreateLoad(destination);
            };
            auto derefAssign = [&](llvm::Value* val, bool isUnsigned) {
                return isDerefStorage()
                    ? compiler->CreateAssignment(val, destination, isUnsigned, namedVar.BaseType)
                    : compiler->CreateAssignment(val, destination, isUnsigned);
            };

            // Thread expected function-pointer type into lambda RHS (for f = (x) => {...} reassignment)
            if (operatorText == "=" && namedVar.TypeAndValue.IsFunctionPointer)
                lambdaExpectedType = namedVar.TypeAndValue;

            if (operatorText == "??=")
            {
                // Null-coalescing assignment: x ??= rhs  →  if (x == 0/null) x = rhs
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

            auto rightNV = ParseAssignmentExpressionNamed(assignCtx);
            lambdaExpectedType = {};
            auto right = LoadNamedVariable(rightNV);

            bool rhsUnsigned = rightNV.TypeAndValue.IsUnsignedInteger() != -1;
            if (operatorText != "=")
            {
                auto left = derefLoad();
                bool lhsUnsigned = namedVar.TypeAndValue.IsUnsignedInteger() != -1;
                right = compiler->CreateOperation(operatorText, left, right, lhsUnsigned, rhsUnsigned);
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
            // Transfer ownership: if RHS was an owning move param, null its alloca so
            // EmitDestructorsForScope won't free the pointer we just stored elsewhere.
            if (operatorText == "=" && rightNV.IsOwning && rightNV.Storage != nullptr
                && rightNV.TypeAndValue.Pointer)
            {
                if (auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(rightNV.BaseType))
                    compiler->builder->CreateStore(
                        llvm::ConstantPointerNull::get(ptrTy), rightNV.Storage);
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

    MyCompilerLLVM::TypedValue ParseConditionalExpression(CFlatParser::ConditionalExpressionContext* ctx)
    {
        auto* compiler = Compiler(ctx);
        auto logicCtx = ctx->logicalOrExpression();

        if (ctx->QuestionQuestion())
        {
            // Null-coalescing: lhs ?? rhs  →  (lhs != null) ? lhs : rhs
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
        // — so `expression` is the TRUE branch and `conditionalExpression` is the FALSE branch.
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

    MyCompilerLLVM::TypedValue ParseLogicalOrExpression(CFlatParser::LogicalOrExpressionContext* ctx)
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
            MyCompilerLLVM::TypeAndValue boolValue = { .TypeName = "bool",.VariableName = "", .Pointer = false };
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
                    left = compiler->CreateOperation(MyCompilerLLVM::Operation::LogicalOr, left, right);
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

    MyCompilerLLVM::TypedValue ParseLogicalAndExpression(CFlatParser::LogicalAndExpressionContext* ctx)
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
            auto elseBlock = compiler->GetElseBlock();

            if (elseBlock)
            {
                for (const auto& inclusiveCtx : inclusiveCtxs)
                {
                    if (left == nullptr)
                    {
                        left = ParseInclusiveOrExpression(inclusiveCtx);
                    }
                    else
                    {
                        auto trueBlock = compiler->CreateBasicBlock("trueAND");
                        auto branch = compiler->CreateConditionJump(left, trueBlock, compiler->GetElseBlock());

                        compiler->InitializeBlock(trueBlock, false);
                        llvm::Value* right = ParseInclusiveOrExpression(inclusiveCtx);
                        left = compiler->CreateOperation(MyCompilerLLVM::Operation::LogicalAnd, left, right);
                    }
                }
            }
            else
            {
                MyCompilerLLVM::TypeAndValue boolValue = { .TypeName = "bool",.VariableName = "", .Pointer = false };
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
                        left = compiler->CreateOperation(MyCompilerLLVM::Operation::LogicalAnd, left, right);
                        compiler->CreateAssignment(left, resultStorage);
                    }
                }

                compiler->CreateBlockBreak(resumeBlock, false);

                compiler->InitializeBlock(resumeBlock, false);
                return { compiler->CreateLoad(resultStorage), false };
            }
            return { left, false };  // && produces bool
        }

        LogErrorContext(ctx, "Logical-AND expression has no operands.");
        return {};
    }

    MyCompilerLLVM::TypedValue ParseInclusiveOrExpression(CFlatParser::InclusiveOrExpressionContext* ctx)
    {
        auto exclusiveCtxs = ctx->exclusiveOrExpression();
        if (exclusiveCtxs.size())
        {
            for (const auto& exclusiveCtx : exclusiveCtxs)
            {
                return ParseExclusiveOrExpression(exclusiveCtx);
            }
        }

        LogErrorContext(ctx, "Inclusive-OR expression has no operands.");
        return {};
    }

    MyCompilerLLVM::TypedValue ParseExclusiveOrExpression(CFlatParser::ExclusiveOrExpressionContext* ctx)
    {
        auto andCtxs = ctx->andExpression();
        if (andCtxs.size())
        {
            for (const auto& andCtx : andCtxs)
            {
                return ParseAndExpression(andCtx);
            }
        }

        LogErrorContext(ctx, "Exclusive-OR expression has no operands.");
        return {};
    }

    MyCompilerLLVM::TypedValue ParseAndExpression(CFlatParser::AndExpressionContext* ctx)
    {
        auto nextCtxs = ctx->equalityExpression();
        if (nextCtxs.size())
        {
            for (const auto& nextCtx : nextCtxs)
            {
                return ParseEqualityExpression(nextCtx);
            }
        }

        LogErrorContext(ctx, "Bitwise-AND expression has no operands.");
        return {};
    }

    MyCompilerLLVM::TypedValue ParseEqualityExpression(CFlatParser::EqualityExpressionContext* ctx)
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

    MyCompilerLLVM::TypedValue ParseTypeCheckExpression(CFlatParser::TypeCheckExpressionContext* ctx)
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

        // In opaque pointer mode, dataPtr is already the right pointer type — no bitcast needed
        auto nullPtr = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(dataPtr->getType()));
        return compiler->builder->CreateSelect(typeMatches, dataPtr, nullPtr);
    }

    MyCompilerLLVM::TypedValue ParseRelationalExpression(CFlatParser::RelationalExpressionContext* ctx)
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

    MyCompilerLLVM::TypedValue ParseShiftExpression(CFlatParser::ShiftExpressionContext* ctx)
    {
        auto nextCtxs = ctx->additiveExpression();
        if (nextCtxs.size())
        {
            for (const auto& nextCtx : nextCtxs)
            {
                return ParseAdditiveExpression(nextCtx);
            }
        }

        LogErrorContext(ctx, "Shift expression has no operands.");
        return {};
    }

    MyCompilerLLVM::TypedValue ParseAdditiveExpression(CFlatParser::AdditiveExpressionContext* ctx)
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

            for (size_t i = 1; i < nextCtxs.size(); i++)
            {
                auto rv = ParseMultiplicativeExpression(nextCtxs[i]);
                llvm::Value* rvalue = rv.value;
                bool ru = rv.isUnsigned;
                std::string op = ctx->children[i * 2 - 1]->getText();

                auto* overload = TryBinaryOperatorOverload(lvalue, op, rvalue, ctx);
                lvalue = overload ? overload : Compiler(ctx)->CreateOperation(op, lvalue, rvalue, lu, ru);
                lu = lu || ru;
            }

            return { lvalue, lu };
        }

        LogErrorContext(ctx, "Additive expression has no operands.");
        return {};
    }

    llvm::Value* LoadNamedVariable(MyCompilerLLVM::NamedVariable& namedVar)
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
                // Local variables and globals store the pointer value inside an alloca/global.
                // Function arguments have the pointer value as the argument itself — return directly.
                if (llvm::isa<llvm::AllocaInst>(namedVar.Storage) ||
                    llvm::isa<llvm::GlobalVariable>(namedVar.Storage) ||
                    llvm::isa<llvm::GetElementPtrInst>(namedVar.Storage))
                    return compiler->CreateLoad(namedVar.Storage);
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

        // namedVar is empty — caller's block was already terminated (e.g. by a return-block inline).
        return nullptr;
    }

    // If lvalue is a struct type with a user-defined operator, dispatch to it.
    // Returns the result Value*, or nullptr to fall back to built-in CreateOperation.
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

        std::string opName = "operator" + op;
        if (!compiler->GetFunction(opName)) return nullptr;

        auto makeRightNV = [&]() {
            MyCompilerLLVM::NamedVariable rightNV;
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
        // registered candidates — check if any candidate's first param is a pointer to
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

            MyCompilerLLVM::NamedVariable thisNV;
            thisNV.TypeAndValue.TypeName = typeName;
            thisNV.TypeAndValue.Pointer  = true;
            thisNV.Primary = tempAlloca;

            return compiler->CreateOverloadedFunctionCall(opName, { thisNV, makeRightNV() });
        }
        else
        {
            // By-value dispatch: built-in value types like string whose operators
            // are defined as operator+(T a, ...) rather than operator+(T* a, ...).
            MyCompilerLLVM::NamedVariable thisNV;
            thisNV.TypeAndValue.TypeName = typeName;
            thisNV.TypeAndValue.Pointer  = false;
            thisNV.Primary  = lvalue;
            thisNV.BaseType = structTy;

            return compiler->CreateOverloadedFunctionCall(opName, { thisNV, makeRightNV() });
        }

        return nullptr;
    }

    MyCompilerLLVM::TypedValue ParseMultiplicativeExpression(CFlatParser::MultiplicativeExpressionContext* ctx)
    {
        auto nextCtxs = ctx->castExpression();

        if (nextCtxs.size() == 1)
        {
            auto namedVar = ParseCastExpression(nextCtxs[0]);
            bool isUnsigned = namedVar.TypeAndValue.IsUnsignedInteger() != -1;
            return { LoadNamedVariable(namedVar), isUnsigned };
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

    MyCompilerLLVM::NamedVariable ParseCastExpression(CFlatParser::CastExpressionContext* ctx, bool lvalue = false)
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

            // Load with the source type first so we read the correct number of bytes,
            // then cast to the destination type. The old code loaded directly with the
            // destination type, which reads too many bytes from narrower fields
            // (e.g., i64 load from a u32 field corrupts the adjacent field).
            if (namedVar.Primary == nullptr && namedVar.Storage != nullptr)
            {
                // Pointer parameters use Storage = &arg (the parameter register value),
                // not an alloca. Loading from the parameter register would dereference
                // the pointer, not read the pointer itself. Use the register value directly.
                if (llvm::isa<llvm::Argument>(namedVar.Storage))
                    namedVar.Primary = namedVar.Storage;
                else
                {
                    auto srcType = compiler->GetType(namedVar.TypeAndValue);
                    namedVar.Primary = compiler->CreateLoad(srcType, namedVar.Storage);
                }
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

    MyCompilerLLVM::TypeAndValue ParseTypeName(CFlatParser::TypeNameContext* ctx)
    {
        auto specCtx = ctx->specifierQualifierList();
        auto abstractDecl = ctx->abstractDeclarator();

        MyCompilerLLVM::TypeAndValue typeValue;

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
                    // Generic cast: (channel<int>*) → mangle to channel__int
                    std::string baseName = typeSpec->genericIdentifier()->Identifier()->getText();
                    std::vector<std::string> typeArgs;
                    for (auto* entry : typeSpec->genericIdentifier()->genericTypeParameters()->typeParameterList()->typeParameterEntry())
                        typeArgs.push_back(ResolveTypeArgEntry(entry));
                    typeValue.TypeName = MangledGenericName(baseName, typeArgs);
                }
                else
                {
                    typeValue.TypeName = typeSpec->getText();
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

    MyCompilerLLVM::NamedVariable ParseUnaryExpression(CFlatParser::UnaryExpressionContext* ctx)
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
                    MyCompilerLLVM::TypeAndValue typeValue;
                    typeValue.TypeName = postfixText;

                    // Check for trailing * (pointer)
                    if (!typeValue.TypeName.empty() && typeValue.TypeName.back() == '*')
                    {
                        typeValue.Pointer = true;
                        typeValue.TypeName.pop_back();
                    }

                    auto* llvmType = compiler->GetType(typeValue, nullptr, true);
                    if (llvmType && !llvmType->isVoidTy())  // Void is a valid type but let's use basic validity check
                    {
                        llvm::Value* result;
                        if (prefixSizeof)
                        {
                            result = compiler->GetTypeSizeBytes(llvmType);
                        }
                        else
                        {
                            result = compiler->GetTypeAlignBytes(llvmType);
                        }

                        if (result)
                        {
                            MyCompilerLLVM::NamedVariable namedVar;
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
        else if (auto* opStrCtx = ctx->operatorStringExpression())
        {
            return ParseOperatorStringExpression(opStrCtx);
        }
        else if (unaryOperator && castExpCtx)
        {
            /* unaryOperator : '&' | '*'| '+'| '-'| '~'| '!'; */

            std::string opText = unaryOperator->getText();
            auto namedVar = ParseCastExpression(castExpCtx);

            if (opText == "&")
            {
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
                namedVar.TypeAndValue.Pointer = false;
                auto* pointeeType = compiler->GetType(namedVar.TypeAndValue);
                // Pointer parameters use Storage = argument register (not an alloca), so the
                // register IS the pointer value — loading from it would add an extra indirection.
                llvm::Value* loadedPtr;
                if (llvm::isa<llvm::Argument>(namedVar.Storage))
                    loadedPtr = namedVar.Storage;
                else
                    loadedPtr = compiler->CreateLoad(namedVar.Storage);
                // The deref'd location: loadedPtr is the address; pointeeType is what it holds.
                // Storing it in Storage (not Primary) makes it usable as both lvalue and rvalue.
                namedVar.Storage  = loadedPtr;
                namedVar.Primary  = nullptr;
                namedVar.BaseType = pointeeType;
            }
            else if (opText == "!")
            {
                auto newValue = this->LoadNamedVariable(namedVar);
                namedVar.Primary = compiler->CreateNot(newValue);
                namedVar.Storage = nullptr;
            }
            else if (opText == "-")
            {
                auto newValue = this->LoadNamedVariable(namedVar);
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
            else if (opText == "+")
            {
                // unary + is a no-op: just load the value
                namedVar.Primary = this->LoadNamedVariable(namedVar);
                namedVar.Storage = nullptr;
            }
            else if (opText == "~")
            {
                auto newValue = this->LoadNamedVariable(namedVar);
                namedVar.Primary = compiler->CreateNot(newValue);
                namedVar.Storage = nullptr;
            }
            else
            {
                LogErrorContext(ctx, std::format("{} operator is not yet implemented.", opText));
                return namedVar;
            }

            // TODO, unaryOperator
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

                auto* llvmType = compiler->GetType(typeValue, nullptr, true);
                if (!llvmType)
                {
                    LogErrorContext(ctx, "sizeof/alignof: could not resolve type to LLVM type");
                    return {};
                }

                // Get the size or alignment value
                llvm::Value* result;
                if (isSizeof)
                {
                    result = compiler->GetTypeSizeBytes(llvmType);
                }
                else
                {
                    result = compiler->GetTypeAlignBytes(llvmType);
                }

                // Return as a named variable with i64 type
                MyCompilerLLVM::NamedVariable namedVar;
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
            // Generic type: Box<int> → Box__int
            // Also apply type substitutions to arguments (e.g. Box<T> with T=int → Box__int)
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

    MyCompilerLLVM::NamedVariable ParseNewExpression(CFlatParser::NewExpressionContext* ctx)
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

        MyCompilerLLVM::TypeAndValue typeInfo{ .TypeName = typeName, .Pointer = typeIsPtr };
        llvm::Type* elemType = compiler->GetType(typeInfo);

        // Compute allocation size
        if (!elemType || !elemType->isSized())
        {
            LogErrorContext(ctx, std::format("'new': cannot compute size of unsized or unresolved type '{}'", typeName));
            return {};
        }
        llvm::Value* sizeVal = compiler->GetTypeSizeBytes(elemType);
        if (isArray)
        {
            llvm::Value* count = ParseAssignmentExpression(ctx->assignmentExpression());
            count = compiler->Upconvert(count, compiler->builder->getInt64Ty());
            sizeVal = compiler->builder->CreateMul(sizeVal, count, "arraysz");
        }

        // Call operator new: class-specific → global
        llvm::Value* rawPtr = nullptr;
        std::string opNewName = typeName + ".operator new";
        MyCompilerLLVM::NamedVariable szArg;
        szArg.Primary = sizeVal;
        szArg.BaseType = sizeVal->getType();
        if (!typeName.empty() && compiler->GetFunction(opNewName))
        {
            rawPtr = compiler->CreateOverloadedFunctionCall(opNewName, { szArg });
        }
        else if (compiler->GetFunction("operator new"))
        {
            rawPtr = compiler->CreateOverloadedFunctionCall("operator new", { szArg });
        }
        else
        {
            LogErrorContext(ctx, "'new' requires 'operator new' to be defined");
            return {};
        }

        // Bitcast void* → T*
        llvm::Type* ptrTy = elemType->getPointerTo();
        llvm::Value* typedPtr = compiler->builder->CreateBitCast(rawPtr, ptrTy, "newptr");

        // For non-array new of a class type: call constructor and store result
        if (!isArray && compiler->GetFunction(typeName))
        {
            std::vector<MyCompilerLLVM::NamedVariable> ctorArgs;
            auto argList = ctx->argumentExpressionList();
            if (argList != nullptr)
            {
                for (auto* namedArg : argList->argumentNamedExpression())
                {
                    llvm::Value* argVal = ParseAssignmentExpression(namedArg->assignmentExpression());
                    if (!argVal) break;
                    MyCompilerLLVM::NamedVariable argVar;
                    argVar.Primary = argVal;
                    argVar.BaseType = argVal->getType();
                    ctorArgs.push_back(argVar);
                }
            }
            llvm::Value* structVal = compiler->CreateOverloadedFunctionCall(typeName, ctorArgs);
            if (structVal)
                compiler->builder->CreateStore(structVal, typedPtr);
        }

        MyCompilerLLVM::NamedVariable result;
        result.TypeAndValue.TypeName = typeName;
        result.TypeAndValue.Pointer = true;
        result.Primary = typedPtr;
        result.BaseType = ptrTy;
        compiler->lastNewAllocated = true;
        return result;
    }

    MyCompilerLLVM::NamedVariable ParseDeleteExpression(CFlatParser::DeleteExpressionContext* ctx)
    {
        auto* compiler = Compiler(ctx);
        bool isArray = ctx->LeftBracket() != nullptr;

        // Parse the pointer expression and determine the pointed-to struct type name.
        // getPointerElementType() is unavailable with LLVM opaque pointers; use the
        // AST-level type information from the unary expression instead.
        std::string typeName;
        llvm::Value* ptrVal = nullptr;
        llvm::Value* srcAlloca = nullptr;
        llvm::Type* srcAllocaElemType = nullptr;
        if (auto* ue = tryGetUnaryExpression(ctx->expression()))
        {
            auto namedVar = ParseUnaryExpression(ue);
            typeName = namedVar.TypeAndValue.TypeName;
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

        // 1. Call destructor if it exists (non-array only)
        if (!isArray && !typeName.empty())
        {
            auto structData = compiler->GetDataStructure(typeName);
            if (structData.Destructor)
                compiler->builder->CreateCall(structData.Destructor, { ptrVal });
        }

        // 2. Convert to void*
        auto* voidPtrTy = compiler->builder->getInt8Ty()->getPointerTo();
        llvm::Value* voidPtr = compiler->builder->CreateBitCast(ptrVal, voidPtrTy, "freeptr");

        // 3. Call operator delete: class-specific → global
        std::string opDelName = typeName + ".operator delete";
        MyCompilerLLVM::NamedVariable ptrArg;
        ptrArg.Primary = voidPtr;
        ptrArg.BaseType = voidPtrTy;
        if (!typeName.empty() && compiler->GetFunction(opDelName))
        {
            compiler->CreateOverloadedFunctionCall(opDelName, { ptrArg });
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

    MyCompilerLLVM::NamedVariable ParseOperatorStringExpression(CFlatParser::OperatorStringExpressionContext* ctx)
    {
        auto* compiler = Compiler(ctx);

        // Collect arguments passed to operator string(...)
        std::vector<MyCompilerLLVM::NamedVariable> arguments;
        if (auto* argList = ctx->argumentExpressionList())
        {
            for (auto* argExpr : argList->argumentNamedExpression())
            {
                llvm::Value* argVal = ParseAssignmentExpression(argExpr->assignmentExpression());
                if (!argVal) break;
                MyCompilerLLVM::NamedVariable argVar;
                argVar.Primary = argVal;
                argVar.BaseType = argVal->getType();
                arguments.push_back(argVar);
            }
        }

        // Dispatch to the global "operator string" overload matching the argument types.
        auto result = compiler->CreateOverloadedFunctionCall("operator string", arguments);
        if (!result) { LogErrorContext(ctx, "'operator string' is not defined"); return {}; }

        MyCompilerLLVM::NamedVariable ret;
        ret.Primary = result;
        ret.TypeAndValue.TypeName = "string";
        ret.TypeAndValue.IsInterface = false;
        ret.TypeAndValue.Pointer = false;
        return ret;
    }

    MyCompilerLLVM::NamedVariable ParsePostfixExpression(CFlatParser::PostfixExpressionContext* ctx, bool lValue = false)
    {
        /*
        * postfixExpression
            : (primaryExpression | '(' typeName ')' '{' initializerList ','? '}')
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
            MyCompilerLLVM::NamedVariable namedVar;
            MyCompilerLLVM::NamedVariable structVar;
            MyCompilerLLVM::NamedVariable interfaceVar;
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
                            }
                        }
                        break;
                    }
                    case CFlatParser::PlusPlus: { if (namedVar.Storage) { PlusPlus[namedVar.Storage]++; } break; }
                    case CFlatParser::MinusMinus: { if (namedVar.Storage) { PlusPlus[namedVar.Storage]--; } break; }
                    case CFlatParser::Identifier:
                    {
                        if (!namespaceContext.empty())
                        {
                            std::string qualifiedName = namespaceContext + "." + terminal->getText();
                            if (Compiler(ctx)->IsNamespace(qualifiedName))
                            {
                                namespaceContext = Compiler(ctx)->ResolveNamespace(qualifiedName);
                                primaryIdentifier = namespaceContext;
                                namedVar = {};
                            }
                            else
                            {
                                // Qualified name (e.g. EnumName.Member) — try to resolve as a global
                                // variable (enum member) or a function. Fall back to leaving
                                // namedVar empty so later code can handle it.
                                primaryIdentifier = qualifiedName;
                                namespaceContext.clear();

                                if (auto gVar = Compiler(ctx)->GetGlobalVariable(primaryIdentifier))
                                {
                                    namedVar.Storage = gVar;
                                    namedVar.BaseType = gVar->getType();
                                }
                                else if (auto func = Compiler(ctx)->GetFunction(primaryIdentifier))
                                {
                                    namedVar.Primary = func;
                                }
                                else
                                {
                                    namedVar = {};
                                }
                            }
                        }
                        else if (interfaceVar.TypeAndValue.IsInterface)
                        {
                            // Method name on interface variable — just record it; dispatch at call site
                            primaryIdentifier = terminal->getText();
                            namedVar = {};
                        }
                        else if (structVar.BaseType)
                        {
                            primaryIdentifier = terminal->getText();
                            auto dataStructure = Compiler(ctx)->GetDataStructure(llvm::dyn_cast<llvm::StructType>(structVar.BaseType));
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

                                    auto* fieldGEP = Compiler(ctx)->CreateStructGEP(structVar.BaseType, structVar.Storage, fieldIndex);
                                    auto* fieldVal = Compiler(ctx)->CreateLoad(fieldGEP);
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
                                    nullConditionalPending = false;
                                }
                                else
                                {
                                    if (structVar.Storage)
                                    {
                                        namedVar.Storage = Compiler(ctx)->CreateStructGEP(structVar.BaseType, structVar.Storage, fieldIndex);
                                        namedVar.Primary = Compiler(ctx)->CreateLoad(namedVar.Storage);
                                        namedVar.BaseType = namedVar.Primary->getType();
                                    }
                                    else if (structVar.Primary)
                                    {
                                        namedVar.Storage = nullptr;
                                        namedVar.Primary = Compiler(ctx)->CreateExtractValue(structVar.Primary, fieldIndex);
                                        namedVar.BaseType = namedVar.Primary->getType();
                                    }
                                    namedVar.TypeAndValue = fieldType;
                                }
                            }
                            else if (Compiler(ctx)->GetFunction(primaryIdentifier) || genericFunctionTemplates.count(primaryIdentifier))
                            {
                                // Not a field — could be a member function or an extension method template.
                                namedVar = {};
                            }
                            else
                            {
                                LogErrorContext(primaryCtx, std::format("Unknown identifier '{}'.", primaryIdentifier));
                            }
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
                            // If this is a generic function template call (e.g. MaxScore<Player>(...)),
                            // instantiate the template now with the explicit type arguments.
                            if (genericFunctionTemplates.count(baseName))
                                InstantiateGenericFunction(baseName, typeArgs);
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
                        }

                        // If the primary was a lambda, propagate its function-pointer type.
                        if (prevPrimary->lambdaExpression() != nullptr)
                            namedVar.TypeAndValue = lastLambdaType;

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
                            // Opaque pointer to a known struct (e.g. string* a) — track as structVar
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
                            MyCompilerLLVM::NamedVariable thisNV = structVar;
                            thisNV.TypeAndValue.VariableName = "";
                            if (!thisNV.TypeAndValue.Pointer && thisNV.Storage == nullptr && thisNV.Primary != nullptr)
                            {
                                auto* temp = Compiler(ctx)->CreateAlloca(structVar.BaseType);
                                Compiler(ctx)->CreateAssignment(thisNV.Primary, temp);
                                thisNV.Storage = temp;
                            }

                            MyCompilerLLVM::NamedVariable idxNV;
                            idxNV.Primary  = rvalue;
                            idxNV.BaseType = rvalue->getType();

                            auto* result = Compiler(ctx)->CreateOverloadedFunctionCall("operator[]", { thisNV, idxNV });
                            if (result)
                            {
                                namedVar.Primary  = result;
                                namedVar.Storage  = nullptr;
                                namedVar.BaseType = result->getType();
                                namedVar.TypeAndValue = Compiler(ctx)->lastCallReturnType;
                                if (result->getType()->isStructTy())
                                {
                                    if (auto* st = llvm::dyn_cast<llvm::StructType>(result->getType()))
                                        if (!st->isLiteral() && st->hasName())
                                            namedVar.TypeAndValue.TypeName = st->getName().str();
                                    structVar = namedVar;
                                }
                                else if (!namedVar.TypeAndValue.TypeName.empty() && namedVar.TypeAndValue.Pointer
                                         && Compiler(ctx)->GetDataStructure(namedVar.TypeAndValue.TypeName).StructType != nullptr)
                                {
                                    structVar = namedVar;
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

                        if (namedVar.TypeAndValue.Pointer)
                        {
                            // Indexing through a pointer (e.g. char* p; p[i]).
                            auto elementTypeAndValue = namedVar.TypeAndValue;
                            if (elementTypeAndValue.ElemPointer)
                            {
                                // Double-pointer (e.g. T* where T=Employee*): element type is T* (Employee*).
                                // Keep Pointer=true, clear ElemPointer — element is a pointer value.
                                elementTypeAndValue.ElemPointer = false;
                            }
                            else
                            {
                                elementTypeAndValue.Pointer = false;
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
                    case CFlatParser::RuleArgumentExpressionList:
                    {
                        // Create Function Call
                        std::string functionName = primaryIdentifier;

                        auto argumentList = ctx->argumentExpressionList();

                        // Handle va_start / va_end — pass the va_list alloca address to the LLVM intrinsic.
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

                        // Check if this is a function pointer variable — emit an indirect call.
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
                                if (argumentList.size() > 0)
                                {
                                    auto namedArgCtx = argumentList[functionArgCounter]->argumentNamedExpression();
                                    for (const auto& namedArgument : namedArgCtx)
                                    {
                                        auto argValue = this->ParseAssignmentExpression(namedArgument->assignmentExpression());
                                        if (argValue) callArgs.push_back(argValue);
                                    }
                                }
                                auto result = Compiler(ctx)->CreateIndirectCall(namedVar.TypeAndValue, funcPtr, callArgs);
                                namedVar.Primary = result;
                                namedVar.Storage = nullptr;
                                namedVar.BaseType = result ? result->getType() : nullptr;
                                namedVar.TypeAndValue = Compiler(ctx)->lastCallReturnType;
                                functionArgCounter++;
                                break;
                            }
                        }

                        // Check if this is a return-block function — inline it at the call site.
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
                                MyCompilerLLVM::NamedVariable thisVar;
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
                                    MyCompilerLLVM::TypeAndValue tv;
                                    tv.TypeName = thisParam.TypeName;
                                    tv.VariableName = thisParam.VariableName;
                                    tv.Pointer = thisParam.Pointer;
                                    auto* alloca = Compiler(ctx)->CreateLocalVariable(tv);
                                    Compiler(ctx)->CreateAssignment(thisVar.Storage, alloca);
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
                                    MyCompilerLLVM::TypeAndValue tv;
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
                            // Return an empty namedVar — callers must tolerate null after a terminator.
                            namedVar = {};
                        }
                        else if (interfaceVar.TypeAndValue.IsInterface)
                        {
                            // Collect extra call arguments
                            std::vector<MyCompilerLLVM::NamedVariable> extraArgs;
                            if (argumentList.size() > 0)
                            {
                                auto namedArgCtx = argumentList[functionArgCounter]->argumentNamedExpression();
                                for (const auto& namedArgument : namedArgCtx)
                                {
                                    auto argValue = this->ParseAssignmentExpression(namedArgument->assignmentExpression());
                                    if (!argValue) break;
                                    MyCompilerLLVM::NamedVariable argVar;
                                    argVar.Primary = argValue;
                                    argVar.BaseType = argValue->getType();

                                    // Extract struct name if this is a struct type
                                    if (auto* st = llvm::dyn_cast<llvm::StructType>(argValue->getType()))
                                    {
                                        auto structName = st->getName().str();
                                        if (!structName.empty())
                                            argVar.TypeAndValue.TypeName = structName;
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

                                std::vector<MyCompilerLLVM::NamedVariable> allArgs;
                                MyCompilerLLVM::NamedVariable ifaceArg = interfaceVar;
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
                            std::vector<MyCompilerLLVM::NamedVariable> arguments;
                            if (structVar.BaseType)
                            {
                                MyCompilerLLVM::NamedVariable argumentNamedVar = structVar; // Copy;
                                argumentNamedVar.TypeAndValue.VariableName = "";
                                arguments.push_back(argumentNamedVar);
                            }
                            else
                            {
                                // Bare call inside a member function — inject 'this' automatically
                                // if the callee is a method of the same struct.
                                auto thisVar = Compiler(ctx)->GetCurrentMemberThis(functionName);
                                if (thisVar.Storage != nullptr)
                                    arguments.push_back(thisVar);
                            }

                            if (argumentList.size() > 0)
                            {
                                auto namedArgCtx = argumentList[functionArgCounter]->argumentNamedExpression();

                                // Look up the function signature to set lambdaExpectedType for lambda arguments.
                                const MyCompilerLLVM::FunctionSymbol* funcSym = nullptr;
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
                                            MyCompilerLLVM::TypeAndValue vaTv;
                                            vaTv.TypeName = "va_list";
                                            vaTv.VariableName = "__va_forward";
                                            Compiler(ctx)->autoVaListAlloca = Compiler(ctx)->CreateLocalVariable(vaTv);
                                            Compiler(ctx)->CreateVaStart(Compiler(ctx)->autoVaListAlloca);
                                        }
                                        llvm::Value* vaValue = Compiler(ctx)->CreateLoad(Compiler(ctx)->autoVaListAlloca);
                                        MyCompilerLLVM::NamedVariable argVar;
                                        argVar.Primary = vaValue;
                                        argVar.BaseType = vaValue->getType();
                                        argVar.TypeAndValue.TypeName = "va_list";
                                        arguments.emplace_back(argVar);
                                        continue;
                                    }

                                    // Set expected type when function expects a function-pointer at this position
                                    lambdaExpectedType = {};
                                    if (funcSym && (argIdx + paramOffset) < funcSym->Parameters.size())
                                    {
                                        const auto& paramTv = funcSym->Parameters[argIdx + paramOffset];
                                        if (paramTv.IsFunctionPointer) lambdaExpectedType = paramTv;
                                    }
                                    auto argName = namedArgument->Identifier();
                                    auto argNV = this->ParseAssignmentExpressionNamed(namedArgument->assignmentExpression());
                                    // Load from storage if Primary isn't populated (simple variable reference)
                                    auto argValue = argNV.Primary ? argNV.Primary : LoadNamedVariable(argNV);
                                    lambdaExpectedType = {};
                                    if (!argValue) break; // caller's block was terminated (e.g. return-block inline)
                                    MyCompilerLLVM::NamedVariable argVar;

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

                                    // Preserve unsigned-integer TypeName so Upconvert can choose ZExt over SExt.
                                    if (argNV.TypeAndValue.IsUnsignedInteger() != -1)
                                        argVar.TypeAndValue.TypeName = argNV.TypeAndValue.TypeName;

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
                                }
                                namedVar.Primary = Compiler(primaryCtx)->CreateOverloadedFunctionCall(resolvedFuncName, arguments);
                                namedVar.Storage = nullptr;
                                namedVar.BaseType = namedVar.Primary ? namedVar.Primary->getType() : nullptr;
                                // Populate TypeAndValue from the resolved overload's return type
                                // so that subsequent member access (->field) can resolve the struct.
                                if (namedVar.Primary)
                                    namedVar.TypeAndValue = Compiler(primaryCtx)->lastCallReturnType;
                            }

                            if (namedVar.BaseType && namedVar.BaseType->isStructTy())
                                structVar = namedVar;
                            else if (!namedVar.TypeAndValue.TypeName.empty() && namedVar.TypeAndValue.Pointer
                                     && Compiler(primaryCtx)->GetDataStructure(namedVar.TypeAndValue.TypeName).StructType != nullptr)
                                structVar = namedVar;
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
                    return nullptr; // multiple rule children — complex expression
                singleRuleChild = dynamic_cast<antlr4::RuleContext*>(child);
            }
        }
        return singleRuleChild ? tryGetUnaryExpression(singleRuleChild) : nullptr;
    }

    // Returns true if rawText (the full StringLiteral token text including quotes)
    // contains at least one unescaped '{' that starts an interpolation expression.
    bool HasInterpolation(const std::string& rawText)
    {
        bool inEscape = false;
        for (size_t i = 1; i + 1 < rawText.size(); i++) // skip opening/closing "
        {
            char c = rawText[i];
            if (inEscape) { inEscape = false; continue; }
            if (c == '\\') { inEscape = true; continue; }
            if (c == '{') return true;
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
                std::string exprText = rawText.substr(exprStart, j - exprStart);
                i = j + 1; // skip past '}'

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
                    // Already a string struct — extract _ptr (field 0) and _len (field 1)
                    llvm::Value* strVal = nv.Primary ? nv.Primary : compiler->CreateLoad(nv.Storage);
                    ptr = compiler->builder->CreateExtractValue(strVal, { 0u });
                    len = compiler->builder->CreateExtractValue(strVal, { 1u });
                }
                else
                {
                    // Call operator string to convert to string struct
                    MyCompilerLLVM::NamedVariable arg = nv;
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
            litAccum += c;
            i++;
        }
        flushLiteral();

        if (segments.empty())
        {
            // Degenerate: no content at all — return empty string struct
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

        MyCompilerLLVM::NamedVariable nvPtrs, nvLens, nvCount;
        nvPtrs.Primary  = ptrBase;
        nvPtrs.TypeAndValue = { "i8", "ptrs", true, false };
        nvLens.Primary  = lenBase;
        nvLens.TypeAndValue = { "i32", "lens", true, false };
        nvCount.Primary = compiler->builder->getInt32(count);
        nvCount.TypeAndValue = { "i32", "count", false, false };

        return compiler->CreateOverloadedFunctionCall("__strconcat", { nvPtrs, nvLens, nvCount });
    }

    MyCompilerLLVM::NamedVariable ParseLambdaExpression(CFlatParser::LambdaExpressionContext* ctx)
    {
        auto* compiler = Compiler(ctx);

        // Parse lambda parameter list
        std::vector<MyCompilerLLVM::DeclTypeAndValue> params;
        if (auto* paramList = ctx->lambdaParamList())
        {
            for (auto* param : paramList->lambdaParam())
            {
                MyCompilerLLVM::DeclTypeAndValue p;
                p.TypeName = param->typeSpecifier()->getText();
                p.Pointer = param->pointer() != nullptr;
                p.VariableName = param->Identifier()->getText();
                params.push_back(p);
            }
        }

        // Return type from lambdaExpectedType (threaded from declaration or argument context)
        MyCompilerLLVM::TypeAndValue returnType;
        returnType.TypeName = lambdaExpectedType.FuncPtrReturnTypeName;
        returnType.Pointer = lambdaExpectedType.FuncPtrReturnPointer;
        if (returnType.TypeName.empty())
            returnType.TypeName = "void";

        std::string lambdaName = compiler->CreateAnonFunctionName();

        // Save builder position — lambda body emits a separate LLVM function
        auto savedState = compiler->SaveBuilderState();

        std::vector<MyCompilerLLVM::TypeAndValue> allParams(params.begin(), params.end());
        auto* fn = compiler->CreateFunctionDefinition(lambdaName, returnType, allParams);
        compiler->InitializeBlock(&fn->front(), false);

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

        if (returnType.TypeName == "void" && !compiler->IsBlockTerminated())
            compiler->CreateReturnCall(nullptr);
        else if (returnType.TypeName != "void" && !compiler->IsBlockTerminated())
            LogErrorContext(ctx, std::format("Lambda '{}' missing return statement.", lambdaName));

        compiler->CreateBlockBreak(nullptr, true);
        compiler->RestoreBuilderState(savedState);

        // Build the function-pointer TypeAndValue describing this lambda
        MyCompilerLLVM::TypeAndValue tv;
        tv.IsFunctionPointer = true;
        tv.FuncPtrReturnTypeName = returnType.TypeName;
        tv.FuncPtrReturnPointer = returnType.Pointer;
        for (const auto& p : params)
        {
            MyCompilerLLVM::TypeAndValue::FuncPtrParam fp;
            fp.TypeName = p.TypeName;
            fp.Pointer = p.Pointer;
            tv.FuncPtrParams.push_back(fp);
        }

        lastLambdaType = tv;
        MyCompilerLLVM::NamedVariable result;
        result.Primary = fn;
        result.TypeAndValue = tv;
        return result;
    }

    llvm::Value* ParsePrimaryExpression(CFlatParser::PrimaryExpressionContext* ctx)
    {
        auto* compiler = Compiler(ctx);

        if (auto* lambdaCtx = ctx->lambdaExpression())
            return ParseLambdaExpression(lambdaCtx).Primary;

        auto expressionCtx = ctx->expression();
        auto constant = ctx->Constant();
        auto stringLiteral = ctx->StringLiteral();

        if (ctx->TypeOf())
        {
            // typeof(int), typeof(bool), typeof(MyStruct) — type specifier used directly
            if (auto* ts = ctx->typeSpecifier())
                return compiler->CreateGlobalString("typeof", ts->getText());

            // typeof(expr) — navigate down to unaryExpression to read TypeAndValue
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
            return ParseExpression(expressionCtx);
        }
        else if (stringLiteral.size() > 0)
        {
            // TODO handle encoding u8,u,U,L
            std::string rawText = ctx->getText();
            if (HasInterpolation(rawText))
                return ParseFormatString(ctx, rawText);
            std::string processed = ProcessRawText(rawText);
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
                return compiler->CreateConstant(MyCompilerLLVM::ConstantVariant(c));
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

    MyCompilerLLVM::NamedVariable ParseIdentifier(antlr4::tree::TerminalNode* node)
    {
        auto* compiler = Compiler();
        if (!node)
            return {};

        std::string name = node->getText();
        MyCompilerLLVM::NamedVariable namedVar = {};

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
            return memberVar;
        }

        // try getting global variable
        if (auto gVar = compiler->GetGlobalVariable(name))
        {
            namedVar.Storage = gVar;
            namedVar.BaseType = gVar->getType();
            return namedVar;
        }

        if (auto func = compiler->GetFunction(name))
        {
            namedVar.Primary = func;
            return namedVar;
        }

        // Return-block functions have no IR entry; they are inlined at the call site.
        if (compiler->GetReturnBlock(name) != nullptr)
            return {};

        // Compiler intrinsics handled at the call site — not in the function table.
        if (name == "va_start" || name == "va_end")
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
        switch (esc)
        {
        case 'n':  return '\n';
        case 't':  return '\t';
        case 'r':  return '\r';
        case '\\': return '\\';
        case '\'': return '\'';
        case '"':  return '"';
        case '0':  return '\0';
        case 'a':  return '\a';
        case 'b':  return '\b';
        case 'f':  return '\f';
        case 'v':  return '\v';
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

    std::string ProcessRawText(const std::string& rawText)
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
            for (auto increment : PlusPlus)
            {
                auto destination = increment.first;
                auto amount = increment.second;

                Compiler()->CreateIncrement(destination, amount);
            }

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

            // Skip generic template struct/class definition bodies (contain unbound T)
            if (auto* structDef = dynamic_cast<CFlatParser::StructDefinitionContext*>(ruleCtx))
            {
                if (structDef->genericTypeParameters() != nullptr)
                    continue;
            }
            if (auto* classDef = dynamic_cast<CFlatParser::ClassDefinitionContext*>(ruleCtx))
            {
                if (classDef->genericTypeParameters() != nullptr)
                    continue;
            }

            // typeSpecifier with generic params: e.g. the "Box<MyInt>" in "Box<MyInt> b"
            // Apply type substitutions for generic parameters.
            if (auto* typeSpec = dynamic_cast<CFlatParser::TypeSpecifierContext*>(ruleCtx))
            {
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
            }

            // primaryExpression with generic params: e.g. the "Box<MyInt>" in "Box<MyInt>()"
            // Apply type substitutions for generic parameters.
            if (auto* primaryExpr = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(ruleCtx))
            {
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
            if (!typeSpec || !typeSpec->genericIdentifier() || !typeSpec->genericIdentifier()->genericTypeParameters())
                continue;

            // This is a generic type instantiation
            std::string baseName = typeSpec->genericIdentifier()->Identifier()->getText();
            std::vector<std::string> typeArgs;
            for (auto* entry : typeSpec->genericIdentifier()->genericTypeParameters()->typeParameterList()->typeParameterEntry())
                typeArgs.push_back(entry->getText());

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

    // Compute the mangled name for a generic instantiation, e.g. Box<int, float> → "Box__int__float".
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
                    // It's a generic interface — instantiate it
                    std::unordered_map<std::string, std::string> ifaceSubst;
                    const auto& ifaceTypeParams = genericInterfaceTypeParams[pending.templateName];
                    for (size_t i = 0; i < ifaceTypeParams.size() && i < pending.typeArgs.size(); i++)
                        ifaceSubst[ifaceTypeParams[i]] = pending.typeArgs[i];
                    InstantiateGenericInterface(pending.templateName, pending.mangledName, ifaceSubst);
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
            for (size_t i = 0; i < typeParams.size() && i < pending.typeArgs.size(); i++)
                activeTypeSubstitutions[typeParams[i]] = pending.typeArgs[i];

            if (structIt != genericStructTemplates.end())
                ParseStructDefinition(structIt->second, pending.mangledName);
            else
                ParseClassDefinition(classIt->second, pending.mangledName);

            activeTypeSubstitutions = savedSubst;
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
            return;
        }

        if (compiler->IsVerbose())
            std::cout << "[verbose]     parse decl list: " << structName << "\n";
        auto declarationList = ctx->declaration();
        std::vector<llvm::Type*> types;

        // Queue and instantiate generic types used in field declarations before
        // ParseDeclarationList resolves them to LLVM types. Only needed at top-level
        // (non-template) scope; template instantiations already have activeTypeSubstitutions
        // set and their generics are queued via ParseDeclarationSpecifiers.
        if (activeTypeSubstitutions.empty())
        {
            for (auto decl : declarationList)
                ScanAndQueueGenericTypeUses(decl);
            ProcessPendingInstantiations();
        }

        auto declList = ParseDeclarationList(declarationList);
        if (compiler->IsVerbose())
            std::cout << "[verbose]     decl list has " << declList.size() << " fields\n";

        // Build the struct body before opening the constructor function so that
        // GetFunctionType can resolve the (sized) return type.  Initializer
        // expressions are evaluated later inside the constructor body.
        if (compiler->IsVerbose())
            std::cout << "[verbose]     create struct type: " << structName << "\n";
        auto structType = compiler->CreateStructType(structName, declList);
        // A struct with zero fields still needs a sized (non-opaque) type
        // so that alloca/sizeof work correctly (e.g. when passed via interface).
        if (structType->isOpaque())
            structType->setBody(llvm::ArrayRef<llvm::Type*>());
        if (compiler->IsVerbose())
            std::cout << "[verbose]     create default ctor: " << structName << "\n";
        MyCompilerLLVM::TypeAndValue returnType{
            .TypeName = structName,
        };
        // Flush any nested generic instantiations queued while parsing field declarations,
        // so their constructors exist before this struct's default constructor calls them.
        {
            auto savedSubst = activeTypeSubstitutions;
            ProcessPendingInstantiations();
            activeTypeSubstitutions = savedSubst;
        }
        // Create default constructor
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
                            // Re-finalise the struct body now that the auto field type is known.
                            structType = compiler->CreateStructType(structName, declList);
                        }
                    }
                    else if (initializer->Default() != nullptr)
                    {
                        rvalue = GenerateDefaultValue(typeValue);
                    }
                }
                else
                {
                    std::cout << "Uninitialize field \"" << structName << "::" << typeValue.VariableName << "\".\n";
                }

                initializers.push_back(rvalue);
            }

            llvm::Value* structVal = llvm::UndefValue::get(structType);

            MyCompilerLLVM::TypeAndValue myStruct;
            myStruct.TypeName = structName;
            myStruct.VariableName = "_" + structName;

            unsigned int structIndex = 0;

            for (auto rvalue : initializers)
            {
                if (rvalue != nullptr)
                {
                    auto* destType = structType->getTypeAtIndex(structIndex);
                    rvalue = compiler->Upconvert(rvalue, destType);
                    if (rvalue->getType() != destType && destType->isStructTy())
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
                    structVal = compiler->CreateInsertValue(structVal, rvalue, structIndex);
                }

                structIndex++;
            }

            // close constructor.
            compiler->CreateReturnCall(structVal);
            // Pop the stack
            compiler->CreateBlockBreak(nullptr, true);
        }

        // Register struct fields in LSP index for dot-completion
        if (auto* s = compiler->GetSymbolSink())
        {
            auto sd = compiler->GetDataStructure(structName);
            for (const auto& field : sd.StructFields)
            {
                if (field.VariableName.empty()) continue;
                std::string typeSig = field.TypeName;
                if (field.Pointer) typeSig += "*";
                if (field.ElemPointer) typeSig += "*";
                s->Register(SymbolKind::Field, structName + "." + field.VariableName,
                            compiler->GetSourceFileName(),
                            (int)ctx->getStart()->getLine(),
                            (int)ctx->getStart()->getCharPositionInLine(),
                            typeSig + " " + field.VariableName);
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

                // Constructor overload — pre-declare without this* parameter
                if (funcName == baseName && func->parameterTypeList())
                {
                    auto* declParamList = func->parameterTypeList();
                    auto declParams = this->ParseParameterTypeList(declParamList);
                    bool declVarargs = declParamList->Ellipsis() != nullptr;
                    std::vector<MyCompilerLLVM::TypeAndValue> ctorAllParams(declParams.begin(), declParams.end());
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
                    MyCompilerLLVM::DeclTypeAndValue thisParam;
                    thisParam.TypeName = structName;
                    thisParam.VariableName = structName + "__";
                    thisParam.Pointer = true;
                    declParams.insert(declParams.begin(), thisParam);
                }

                std::vector<MyCompilerLLVM::TypeAndValue> declAllParams(declParams.begin(), declParams.end());
                compiler->CreateFunctionDeclaration(declName, declReturnType, declAllParams, declReturnType.external, declVarargs);
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
                // Constructor overload — same name as struct, with parameters
                if (funcName == baseName && func->parameterTypeList())
                {
                    ParseConstructorDefinition(func, structName);
                    continue;
                }
                if (funcName == "operator new" || funcName == "operator delete" || isFunctionStatic(func))
                    ParseFunctionDefinition(func, {}, {}, structName + "." + funcName);
                else
                    ParseFunctionDefinition(func, structName);
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

        // Resolve types — all must be concrete by this point (ProcessPendingInstantiations ran)
        auto* progType       = compiler->dataStructures[name].StructType;
        auto* blockAllocType = compiler->dataStructures.count("BlockAllocator")
                               ? compiler->dataStructures["BlockAllocator"].StructType : nullptr;
        auto* listStringType = compiler->dataStructures.count("list__string")
                               ? compiler->dataStructures["list__string"].StructType : nullptr;
        auto* threadType     = compiler->dataStructures.count("Thread")
                               ? compiler->dataStructures["Thread"].StructType : nullptr;

        if (!progType || !blockAllocType || !listStringType || !threadType)
        {
            compiler->LogError(std::format(
                "program '{}': missing required type (BlockAllocator={}, list__string={}, Thread={})",
                name,
                blockAllocType ? "ok" : "missing",
                listStringType ? "ok" : "missing",
                threadType     ? "ok" : "missing"));
            return;
        }

        auto* progPtrType       = progType->getPointerTo();
        auto* blockAllocPtrType = blockAllocType->getPointerTo();
        auto* voidPtrType       = compiler->builder->getInt8Ty()->getPointerTo();
        auto* i32Type           = llvm::Type::getInt32Ty(*compiler->context);
        auto* i64Type           = llvm::Type::getInt64Ty(*compiler->context);

        // __RunArgs_Name = { Name*, list__string }
        auto* runArgsType = llvm::StructType::create(
            *compiler->context, {progPtrType, listStringType}, "__RunArgs_" + name);
        compiler->programTable[name].RunArgsType = runArgsType;

        // Look up helper functions
        auto* mallocFn           = compiler->GetFunction("malloc");
        auto* freeFn             = compiler->GetFunction("free");
        auto* blockAllocCtorFn   = compiler->GetFunction("BlockAllocator");
        auto* blockAllocCleanupFn = FindMethodOf("cleanup", "BlockAllocator");
        auto* threadCtorFn       = compiler->GetFunction("Thread");
        auto* threadStartFn      = FindMethodOf("start", "Thread");
        auto* threadJoinFn       = FindMethodOf("join", "Thread");
        auto* mainFn             = FindMethodOf("main", name);

        if (!mallocFn || !freeFn || !blockAllocCtorFn || !blockAllocCleanupFn
            || !threadCtorFn || !threadStartFn || !threadJoinFn || !mainFn)
        {
            compiler->LogError(std::format("program '{}': missing helper function for run() generation", name));
            return;
        }

        unsigned exitCodeIdx = compiler->programTable[name].ExitCodeFieldIndex;
        unsigned threadIdx   = compiler->programTable[name].ThreadFieldIndex;

        // Cast trampoline to the expected function pointer type: int(*)(void*)
        auto* trampolineFnTy = llvm::FunctionType::get(i32Type, {voidPtrType}, false);

        // ======================================================================
        // EMIT TRAMPOLINE: int __program_run_Name(void* ctx)
        // Runs on the spawned thread. Sets up allocator, calls main, stores
        // exitCode into self, frees the args packet, returns main's result.
        // ======================================================================
        {
            MyCompilerLLVM::TypeAndValue intReturn;   intReturn.TypeName = "int";
            MyCompilerLLVM::DeclTypeAndValue ctxParam;
            ctxParam.TypeName = "void";  ctxParam.VariableName = "ctx";  ctxParam.Pointer = true;
            auto* trampolineFn = compiler->CreateFunctionDefinition(
                "__program_run_" + name, intReturn, {ctxParam});
            compiler->programTable[name].TrampolineFunction = trampolineFn;

            auto* ctxArg = trampolineFn->getArg(0);

            // Cast void* ctx to __RunArgs_Name*
            auto* argsPacket = compiler->builder->CreateBitCast(
                ctxArg, runArgsType->getPointerTo(), "args_packet");

            // Load self (Name*) from field 0
            auto* selfGEP  = compiler->builder->CreateStructGEP(runArgsType, argsPacket, 0, "self_gep");
            auto* self     = compiler->builder->CreateLoad(progPtrType, selfGEP, "self");

            // Pointer to list__string (field 1) — passed by value to main
            auto* argsGEP  = compiler->builder->CreateStructGEP(runArgsType, argsPacket, 1, "args_gep");

            // Allocate BlockAllocator on heap using raw malloc (tracked alloc not active yet)
            auto* blockAllocSize = compiler->GetTypeSizeBytes(blockAllocType);
            auto* allocRaw = compiler->builder->CreateCall(
                mallocFn->getFunctionType(), mallocFn, {blockAllocSize}, "alloc_raw");
            auto* allocPtr = compiler->builder->CreateBitCast(allocRaw, blockAllocPtrType, "alloc_ptr");

            // Initialize BlockAllocator via its default constructor (returns struct by value)
            auto* allocInitVal = compiler->builder->CreateCall(
                blockAllocCtorFn->getFunctionType(), blockAllocCtorFn, {}, "alloc_init");
            compiler->builder->CreateStore(allocInitVal, allocPtr);

            // Set thread-local __active_allocator = allocPtr
            auto* activeAllocGlobal = compiler->globalNamedVariable["__active_allocator"];
            compiler->builder->CreateStore(allocPtr, activeAllocGlobal);

            // Load list__string args by value from the packet
            auto* argsVal = compiler->builder->CreateLoad(listStringType, argsGEP, "args_val");

            // Call Name.main(self, args)
            auto* mainResult = compiler->builder->CreateCall(
                mainFn->getFunctionType(), mainFn, {self, argsVal}, "main_result");

            // Store main's return code into self->exitCode
            auto* exitCodeGEP = compiler->builder->CreateStructGEP(progType, self, exitCodeIdx, "exit_code_gep");
            compiler->builder->CreateStore(mainResult, exitCodeGEP);

            // Cleanup: call BlockAllocator.cleanup(allocPtr)
            compiler->builder->CreateCall(
                blockAllocCleanupFn->getFunctionType(), blockAllocCleanupFn, {allocPtr});

            // Free the BlockAllocator
            compiler->builder->CreateCall(freeFn->getFunctionType(), freeFn, {allocRaw});

            // Clear __active_allocator
            compiler->builder->CreateStore(
                llvm::ConstantPointerNull::get(blockAllocPtrType), activeAllocGlobal);

            // Free the args packet (trampoline owns it; run() transferred ownership on start success)
            compiler->builder->CreateCall(freeFn->getFunctionType(), freeFn, {ctxArg});

            compiler->CreateReturnCall(mainResult);
            compiler->CreateBlockBreak(nullptr, true);
        }

        // ======================================================================
        // EMIT run(): bool run(Name* this, list__string args)
        // Allocates args packet, spawns thread into self->_thread, returns
        // whether the thread started. Does NOT join — caller uses WaitForExit().
        // ======================================================================
        {
            MyCompilerLLVM::TypeAndValue boolReturn;  boolReturn.TypeName = "bool";
            MyCompilerLLVM::DeclTypeAndValue thisParam;
            thisParam.TypeName = name;  thisParam.VariableName = name + "__";  thisParam.Pointer = true;
            MyCompilerLLVM::DeclTypeAndValue argsParam;
            argsParam.TypeName = "list__string";  argsParam.VariableName = "args";
            argsParam.IsMove = true;  // run() takes ownership; caller's list is zeroed after the call

            auto* runFn = compiler->CreateFunctionDefinition("run", boolReturn, {thisParam, argsParam});
            compiler->programTable[name].RunFunction = runFn;

            auto* thisArg = runFn->getArg(0);   // Name*
            auto* argsArg = runFn->getArg(1);   // list__string by value (move)

            // Malloc the args packet (raw malloc — tracked alloc is per-thread)
            auto* pkgSize = compiler->GetTypeSizeBytes(runArgsType);
            auto* pkgRaw  = compiler->builder->CreateCall(
                mallocFn->getFunctionType(), mallocFn, {pkgSize}, "pkg_raw");
            auto* pkg = compiler->builder->CreateBitCast(pkgRaw, runArgsType->getPointerTo(), "pkg");

            // Store this → pkg->self (field 0)
            auto* selfGEP = compiler->builder->CreateStructGEP(runArgsType, pkg, 0, "pkg_self_gep");
            compiler->builder->CreateStore(thisArg, selfGEP);

            // Store args → pkg->args (field 1): use the original argument value (pre-alloca copy)
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

            // Get &self->_thread (stored field; initialized by the program ctor)
            auto* threadFieldGEP = compiler->builder->CreateStructGEP(
                progType, thisArg, threadIdx, "thread_field");

            // Thread.start(&self->_thread, trampoline, pkg) → bool
            auto* trampolineFn  = compiler->programTable[name].TrampolineFunction;
            auto* trampolineFnPtr = compiler->builder->CreateBitCast(
                trampolineFn, trampolineFnTy->getPointerTo());
            auto* startResult = compiler->builder->CreateCall(
                threadStartFn->getFunctionType(), threadStartFn,
                {threadFieldGEP, trampolineFnPtr, pkgRaw}, "start_result");

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
            MyCompilerLLVM::TypeAndValue voidReturn;  voidReturn.TypeName = "void";
            MyCompilerLLVM::DeclTypeAndValue thisParam;
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
                MyCompilerLLVM::TypeAndValue boolReturn;   boolReturn.TypeName = "bool";
                MyCompilerLLVM::DeclTypeAndValue thisParam;
                thisParam.TypeName = name;  thisParam.VariableName = name + "__";  thisParam.Pointer = true;
                MyCompilerLLVM::DeclTypeAndValue tokenParam;
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

        // Guard against user fields clashing with auto-injected synthetic fields
        for (auto& field : declList)
        {
            if (field.VariableName == "exitCode" || field.VariableName == "_thread")
                compiler->LogError(std::format(
                    "program '{}': field name '{}' is reserved", name, field.VariableName));
        }

        // Inject synthetic fields: exitCode (int = -1) and _thread (Thread)
        unsigned exitCodeFieldIndex = (unsigned)declList.size();
        unsigned threadFieldIndex   = exitCodeFieldIndex + 1;
        {
            MyCompilerLLVM::DeclTypeAndValue exitCodeField;
            exitCodeField.TypeName     = "int";
            exitCodeField.VariableName = "exitCode";
            declList.push_back(exitCodeField);

            MyCompilerLLVM::DeclTypeAndValue threadField;
            threadField.TypeName     = "Thread";
            threadField.VariableName = "_thread";
            declList.push_back(threadField);
        }

        // Build struct type with user fields + synthetic fields
        auto* structType = compiler->CreateStructType(name, declList);
        if (structType->isOpaque())
            structType->setBody(llvm::ArrayRef<llvm::Type*>());

        // Create default constructor (same pattern as ParseStructDefinition)
        {
            MyCompilerLLVM::TypeAndValue returnType;
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
                std::string typeSig = field.TypeName;
                if (field.Pointer) typeSig += "*";
                if (field.ElemPointer) typeSig += "*";
                s->Register(SymbolKind::Field, name + "." + field.VariableName,
                            compiler->GetSourceFileName(),
                            (int)ctx->getStart()->getLine(),
                            (int)ctx->getStart()->getCharPositionInLine(),
                            typeSig + " " + field.VariableName);
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
        compiler->programTable[name].StructType        = structType;
        compiler->programTable[name].ConfigFields      = declList;
        compiler->programTable[name].ExitCodeFieldIndex = exitCodeFieldIndex;
        compiler->programTable[name].ThreadFieldIndex   = threadFieldIndex;

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

        if (compiler->IsVerbose())
            std::cout << "[verbose]     parse decl list: " << structName << "\n";
        auto declarationList = ctx->declaration();
        std::vector<llvm::Type*> types;

        // Queue and instantiate generic types used in field declarations before
        // ParseDeclarationList resolves them to LLVM types. Only needed at top-level
        // (non-template) scope; template instantiations already have activeTypeSubstitutions
        // set and their generics are queued via ParseDeclarationSpecifiers.
        if (activeTypeSubstitutions.empty())
        {
            for (auto decl : declarationList)
                ScanAndQueueGenericTypeUses(decl);
            ProcessPendingInstantiations();
        }

        auto declList = ParseDeclarationList(declarationList);
        if (compiler->IsVerbose())
            std::cout << "[verbose]     decl list has " << declList.size() << " fields\n";

        if (compiler->IsVerbose())
            std::cout << "[verbose]     create struct type: " << structName << "\n";
        auto structType = compiler->CreateStructType(structName, declList);
        // A class with zero fields still needs a sized (non-opaque) type
        // so that alloca/sizeof work correctly (e.g. when passed via interface).
        if (structType->isOpaque())
            structType->setBody(llvm::ArrayRef<llvm::Type*>());
        if (compiler->IsVerbose())
            std::cout << "[verbose]     create default ctor: " << structName << "\n";
        MyCompilerLLVM::TypeAndValue returnType{
            .TypeName = structName,
        };
        // Flush any nested generic instantiations queued while parsing field declarations,
        // so their constructors exist before this class's default constructor calls them.
        {
            auto savedSubst = activeTypeSubstitutions;
            ProcessPendingInstantiations();
            activeTypeSubstitutions = savedSubst;
        }
        // Create default constructor
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
                else
                {
                    std::cout << "Uninitialized field \"" << structName << "::" << typeValue.VariableName << "\".\n";
                }

                initializers.push_back(rvalue);
            }

            llvm::Value* structVal = llvm::UndefValue::get(structType);

            MyCompilerLLVM::TypeAndValue myStruct;
            myStruct.TypeName = structName;
            myStruct.VariableName = "_" + structName;

            unsigned int structIndex = 0;

            for (auto rvalue : initializers)
            {
                if (rvalue != nullptr)
                {
                    auto* destType = structType->getTypeAtIndex(structIndex);
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

                // Constructor overload — pre-declare without this* parameter
                if (funcName == baseName && func->parameterTypeList())
                {
                    auto* declParamList = func->parameterTypeList();
                    auto declParams = this->ParseParameterTypeList(declParamList);
                    bool declVarargs = declParamList->Ellipsis() != nullptr;
                    std::vector<MyCompilerLLVM::TypeAndValue> ctorAllParams(declParams.begin(), declParams.end());
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
                    MyCompilerLLVM::DeclTypeAndValue thisParam;
                    thisParam.TypeName = structName;
                    thisParam.VariableName = structName + "__";
                    thisParam.Pointer = true;
                    declParams.insert(declParams.begin(), thisParam);
                }

                std::vector<MyCompilerLLVM::TypeAndValue> declAllParams(declParams.begin(), declParams.end());
                compiler->CreateFunctionDeclaration(declName, declReturnType, declAllParams, declReturnType.external, declVarargs);
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
                // Constructor overload — same name as class, with parameters
                if (funcName == baseName && func->parameterTypeList())
                {
                    ParseConstructorDefinition(func, structName);
                    continue;
                }
                if (funcName == "operator new" || funcName == "operator delete" || isFunctionStatic(func))
                    ParseFunctionDefinition(func, {}, {}, structName + "." + funcName);
                else
                    ParseFunctionDefinition(func, structName);
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
                // Compute concrete type args by applying active substitutions
                std::vector<std::string> concreteTypeArgs;
                for (auto* entry : genId->genericTypeParameters()->typeParameterList()->typeParameterEntry())
                {
                    std::string typeArg = entry->getText();
                    auto substIt = activeTypeSubstitutions.find(typeArg);
                    if (substIt != activeTypeSubstitutions.end())
                        typeArg = substIt->second;
                    concreteTypeArgs.push_back(typeArg);
                }

                ifaceName = MangledGenericName(ifaceBaseName, concreteTypeArgs);

                // Build substitution map for the interface template's type params
                std::unordered_map<std::string, std::string> ifaceSubstitutions;
                const auto& ifaceTypeParams = genericInterfaceTypeParams[ifaceBaseName];
                for (size_t i = 0; i < ifaceTypeParams.size() && i < concreteTypeArgs.size(); i++)
                    ifaceSubstitutions[ifaceTypeParams[i]] = concreteTypeArgs[i];

                InstantiateGenericInterface(ifaceBaseName, ifaceName, ifaceSubstitutions);
            }

            ifaceNames.push_back(ifaceName);
        }
        compiler->RegisterStructInterfaces(structName, ifaceNames);
        for (const auto& interfaceName : ifaceNames)
            compiler->VerifyInterfaceImplementation(structName, interfaceName);

        // Process any generic instantiations that were queued during this class definition
        // ProcessPendingInstantiations();
    }

    std::vector<std::string> ParseGenericTypeParameters(CFlatParser::GenericTypeParametersContext* genericParams)
    {
        std::vector<std::string> typeParams;
        if (!genericParams)
            return typeParams;

        auto typeParamList = genericParams->typeParameterList();
        if (!typeParamList)
            return typeParams;

        for (auto* entry : typeParamList->typeParameterEntry())
        {
            auto* typeSpec = entry->typeSpecifier();
            // Generic type parameters must be simple identifiers, not built-in types
            if (!typeSpec || !typeSpec->genericIdentifier() || !typeSpec->genericIdentifier()->Identifier())
            {
                LogErrorContext(entry, "Generic type parameter must be an identifier, not a built-in type");
                continue;
            }
            typeParams.push_back(typeSpec->genericIdentifier()->Identifier()->getText());
        }

        return typeParams;
    }

    // Returns { typeParamName → [requiredInterface, …] } from a whereClause context.
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

        MyCompilerLLVM::DeclTypeAndValue returnType;
        returnType.TypeName = structName;
        std::vector<MyCompilerLLVM::TypeAndValue> allParams(params.begin(), params.end());

        // Open constructor function — no this* parameter; returns the struct by value
        auto fn = compiler->CreateFunctionDefinition(structName, returnType, allParams, false, varargs, line);
        compiler->InitializeBlock(&fn->front(), false);

        // Get the struct's LLVM type (without pointer)
        auto* structLLVMType = compiler->GetType(returnType, nullptr, false);

        // Call the default constructor to get a fully default-initialized instance
        auto* defaultVal = compiler->CreateOverloadedFunctionCall(structName, {});

        // Alloca the struct so we can GEP into fields via 'this'
        auto* thisAlloca = compiler->builder->CreateAlloca(structLLVMType, nullptr, structName + "__");
        if (defaultVal)
            compiler->builder->CreateStore(defaultVal, thisAlloca);

        // Register the alloca as the implicit 'this' pointer so member field access works
        MyCompilerLLVM::TypeAndValue thisTv;
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
        MyCompilerLLVM::DeclTypeAndValue thisParam;
        thisParam.TypeName = structName;
        thisParam.VariableName = structName + "__";
        thisParam.Pointer = true;

        std::vector<MyCompilerLLVM::TypeAndValue> params = { thisParam };

        MyCompilerLLVM::TypeAndValue returnType;
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

    std::vector<MyCompilerLLVM::DeclTypeAndValue> ParseParameterTypeList(CFlatParser::ParameterTypeListContext* paramTypeList)
    {
        std::vector<MyCompilerLLVM::DeclTypeAndValue> params;

        if (paramTypeList == nullptr)
            return params;

        auto paramList = paramTypeList->parameterList();
        auto paramDeclList = paramList->parameterDeclaration();

        for (auto paramDecl : paramDeclList)
        {
            MyCompilerLLVM::DeclTypeAndValue paramType = this->ParseDeclarationSpecifiers(paramDecl->declarationSpecifiers());
            if (auto declarer = paramDecl->declarator())
            {
                if (auto directDeclarer = declarer->directDeclarator())
                {
                    paramType.VariableName = directDeclarer->getText();
                }
            }

            paramType.DefaultValue = paramDecl->assignmentExpression();
            params.push_back(paramType);

            if (paramType.VariableName == "")
            {
                PrintContext(paramDecl);
                std::cout << "Function parameter name is missing.\n";
            }
        }

        return params;
    }

    MyCompilerLLVM::ConstantVariant ParseNumberConstant(std::string rawNumber)
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

        bool looksFloat = (numberPart.find('.') != std::string::npos) ||
            (numberPart.find('e') != std::string::npos) ||
            (numberPart.find('E') != std::string::npos);

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
            return static_cast<int64_t>(uval);
        }

        if (uval <= static_cast<unsigned long long>(std::numeric_limits<int8_t>::max()))
            return static_cast<char>(uval);
        if (uval <= static_cast<unsigned long long>(std::numeric_limits<int16_t>::max()))
            return static_cast<short>(uval);
        if (uval <= static_cast<unsigned long long>(std::numeric_limits<int>::max()))
            return static_cast<int>(uval);
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
