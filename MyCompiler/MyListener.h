#pragma once
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
                    // grammar: some Identifier occurrences were refactored into a genericIdentifier rule
                    if (typeSpec->genericIdentifier() != nullptr && typeSpec->genericIdentifier()->genericTypeParameters() != nullptr)
                    {
                        // Generic type instantiation: Box<MyType> → Box__MyType
                        std::string baseName = typeSpec->genericIdentifier()->Identifier()->getText();
                        std::vector<std::string> typeArgs;
                    for (auto typeParamSpec : typeSpec->genericIdentifier()->genericTypeParameters()->typeParameterList()->typeSpecifier())
                    {
                        typeArgs.push_back(typeParamSpec->getText());
                    }
                    std::string mangledName = baseName;
                    for (const auto& arg : typeArgs) mangledName += "__" + arg;
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
                if (declType.IsInterface) declType.Pointer = true;
                break;
            }
            else if (storageSpec)
            {
                declType.external = storageSpec->Extern() != nullptr;
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

    std::string getFunctionName(CFlatParser::FunctionDefinitionContext* ctx)
    {
        if (auto* opId = ctx->operatorFunctionId())
            return ::getOperatorName(opId);
        auto directDecl = ctx->directDeclarator();
        return directDecl->getText();
    }

    bool isFunctionStatic(CFlatParser::FunctionDefinitionContext* func)
    {
        if (!func->declarationSpecifiers()) return false;
        for (auto* ds : func->declarationSpecifiers()->declarationSpecifier())
            if (ds->storageClassSpecifier() && ds->storageClassSpecifier()->Static() != nullptr)
                return true;
        return false;
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
        compiler->CreateFunctionDeclaration(name, returnType, allParams, returnType.external, varargs);

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
    }

    void ScanStructDefinition(CFlatParser::StructDefinitionContext* ctx, const std::string& namespaceName = {})
    {
        auto* compiler = Compiler(ctx);
        // Generic template definitions are not pre-declared; they are instantiated on demand.
        if (ctx->genericTypeParameters() != nullptr)
            return;

        std::string structName = ctx->directDeclarator()->getText();
        if (!namespaceName.empty())
            structName = namespaceName + "." + structName;

        // Register opaque struct so the type is known for pointer/field use
        compiler->CreateStructType(structName, {});

        // Pre-declare default constructor
        MyCompilerLLVM::TypeAndValue returnType{ .TypeName = structName };
        compiler->CreateFunctionDeclaration(structName, returnType, {});

        // Pre-declare member functions
        for (auto func : ctx->functionDefinition())
            ScanFunctionDefinition(func, structName);

        // Pre-declare destructor
        for (auto dtor : ctx->destructorDefinition())
        {
            MyCompilerLLVM::DeclTypeAndValue thisParam;
            thisParam.TypeName = structName;
            thisParam.VariableName = structName + "__";
            thisParam.Pointer = true;
            MyCompilerLLVM::TypeAndValue voidReturn{ .TypeName = "void" };
            compiler->CreateFunctionDeclaration("~" + structName, voidReturn, { thisParam });
        }
    }

    void ScanClassDefinition(CFlatParser::ClassDefinitionContext* ctx, const std::string& namespaceName = {})
    {
        auto* compiler = Compiler(ctx);
        // Generic template definitions are not pre-declared; they are instantiated on demand.
        if (ctx->genericTypeParameters() != nullptr)
            return;

        std::string className = ctx->directDeclarator()->getText();
        if (!namespaceName.empty())
            className = namespaceName + "." + className;

        // Register opaque struct so the type is known for pointer/field use
        compiler->CreateStructType(className, {});

        // Pre-declare default constructor
        MyCompilerLLVM::TypeAndValue returnType{ .TypeName = className };
        compiler->CreateFunctionDeclaration(className, returnType, {});

        // Pre-declare member functions
        for (auto func : ctx->functionDefinition())
            ScanFunctionDefinition(func, className);

        // Pre-declare destructor
        for (auto dtor : ctx->destructorDefinition())
        {
            MyCompilerLLVM::DeclTypeAndValue thisParam;
            thisParam.TypeName = className;
            thisParam.VariableName = className + "__";
            thisParam.Pointer = true;
            MyCompilerLLVM::TypeAndValue voidReturn{ .TypeName = "void" };
            compiler->CreateFunctionDeclaration("~" + className, voidReturn, { thisParam });
        }
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
                for (auto* typeParamSpec : genericParams->typeParameterList()->typeSpecifier())
                    mangledName += "__" + typeParamSpec->getText();
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

    MyCompilerLLVM* Compiler(antlr4::ParserRuleContext* ctx)
    {
        if (ctx)
            compilerLLVM->SetSourceLocation(ctx->getStart()->getLine(), ctx->getStart()->getCharPositionInLine());
        return compilerLLVM;
    }
    inline MyCompilerLLVM* Compiler() { return compilerLLVM; }

    std::unordered_map<llvm::Value*, int> PlusPlus;
    bool global_scope = true; // true when parsing an entity in the global scope.

    // Generic template state is shared across all MyListener instances so that
    // templates declared in an imported file remain visible when the importing
    // file needs to instantiate them.
    static inline std::unordered_map<std::string, CFlatParser::StructDefinitionContext*> genericStructTemplates;
    static inline std::unordered_map<std::string, CFlatParser::ClassDefinitionContext*> genericClassTemplates;
    static inline std::unordered_map<std::string, std::vector<std::string>> genericStructTypeParams;
    static inline std::unordered_set<std::string> instantiatedGenerics;
    // Active type parameter substitutions during generic instantiation (e.g. "T" -> "int")
    std::unordered_map<std::string, std::string> activeTypeSubstitutions;

    static inline std::unordered_map<std::string, CFlatParser::InterfaceDefinitionContext*> genericInterfaceTemplates;
    static inline std::unordered_map<std::string, std::vector<std::string>> genericInterfaceTypeParams;
    static inline std::unordered_set<std::string> instantiatedInterfaces;

    static inline std::unordered_map<std::string, CFlatParser::FunctionDefinitionContext*> genericFunctionTemplates;
    static inline std::unordered_map<std::string, std::vector<std::string>> genericFunctionTypeParams;
    static inline std::unordered_set<std::string> instantiatedGenericFunctions;

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
    };

    struct SwitchContext
    {
        std::unordered_map<CFlatParser::LabeledStatementContext*, SwitchCaseEntry> caseMap;
        llvm::BasicBlock* defaultBlock = nullptr;
        llvm::BasicBlock* resumeBlock = nullptr;
        bool isStringSwitch = false;
    };

    std::vector<SwitchContext> switchStack;

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
                if (typeSpec->genericIdentifier() != nullptr && typeSpec->genericIdentifier()->genericTypeParameters() != nullptr)
                {
                    // Generic type instantiation: Box<MyType> -> Box__MyType
                    std::string baseName = typeSpec->genericIdentifier()->Identifier()->getText();
                    std::vector<std::string> typeArgs;
                    for (auto typeParamSpec : typeSpec->genericIdentifier()->genericTypeParameters()->typeParameterList()->typeSpecifier())
                    {
                        std::string arg = typeParamSpec->getText();
                        // Apply active type parameter substitutions (e.g. T -> int inside a template body)
                        auto substIt = activeTypeSubstitutions.find(arg);
                        if (substIt != activeTypeSubstitutions.end())
                            arg = substIt->second;
                        typeArgs.push_back(arg);
                    }
                    std::string mangledName = MangledGenericName(baseName, typeArgs);
                    // Just store the mangled name; instantiation is done separately at declaration time
                    declType.TypeName = mangledName;
                }
                else
                {
                    typeName = typeSpec->getText();
                    // Apply active type parameter substitutions (e.g. T -> int inside a template body)
                    auto substIt = activeTypeSubstitutions.find(typeName);
                    if (substIt != activeTypeSubstitutions.end())
                        typeName = substIt->second;
                    // Resolve namespace-qualified type names (alias expansion + parent namespace search)
                    typeName = Compiler(declSpecs)->ResolveQualifiedName(typeName);
                    // Resolve type aliases (e.g. user-defined aliases)
                    typeName = Compiler(declSpecs)->ResolveTypeAlias(typeName);
                    declType.TypeName = typeName;
                }
                declType.Pointer = declSpec->pointer() != nullptr;
                declType.ArraySize = declSpec->assignmentExpression();
                declType.IsInterface = Compiler(declSpecs)->IsInterfaceType(declType.TypeName);
                if (declType.IsInterface) declType.Pointer = true;
                break;
            }
            else if (storageSpec)
            {
                declType.external = storageSpec->Extern() != nullptr;
            }
        }

        return declType;
    }

    MyCompilerLLVM::DeclTypeAndValue getFunctionReturnType(CFlatParser::FunctionDefinitionContext* ctx)
    {
        auto declSpecs = ctx->declarationSpecifiers();

        return ParseDeclarationSpecifiers(declSpecs);
    }

    std::string getFunctionName(CFlatParser::FunctionDefinitionContext* ctx)
    {
        if (auto* opId = ctx->operatorFunctionId())
            return ::getOperatorName(opId);
        auto directDecl = ctx->directDeclarator();
        return directDecl->getText();
    }

    bool isFunctionStatic(CFlatParser::FunctionDefinitionContext* func)
    {
        if (!func->declarationSpecifiers()) return false;
        for (auto* ds : func->declarationSpecifiers()->declarationSpecifier())
            if (ds->storageClassSpecifier() && ds->storageClassSpecifier()->Static() != nullptr)
                return true;
        return false;
    }

    // Returns the default value for a type:
    //   - struct types (local scope): calls the default constructor.
    //   - everything else (or global scope): zero-initializes.
    llvm::Value* GenerateDefaultValue(const MyCompilerLLVM::DeclTypeAndValue& typeValue)
    {
        auto* compiler = Compiler();
        auto* llvmType = compiler->GetType(typeValue);
        if (!llvmType) return nullptr;

        if (!typeValue.Pointer && llvmType->isStructTy() && !global_scope)
        {
            auto structData = compiler->GetDataStructure(typeValue.TypeName);
            if (structData.StructType != nullptr && compiler->GetFunction(typeValue.TypeName))
                return compiler->CreateOverloadedFunctionCall(typeValue.TypeName, {});
        }

        return llvm::Constant::getNullValue(llvmType);
    }

public:
    MyListener(CFlatParser* parser, MyCompilerLLVM* compilerLLVM)
    {
        this->parser = parser;
        this->compilerLLVM = compilerLLVM;
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
            compiler->RegisterTypeAlias(alias, target);
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

        // Process any generic instantiations queued while parsing the above item.
        // This is the only safe point: the IRBuilder has no active function/block.
        ProcessPendingInstantiations();
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
            auto* rawVal = ParseConditionalExpression(labeled->constantExpression()->conditionalExpression());
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
            ctx.caseMap[labeled] = { val, strLit, compiler->CreateBasicBlock("switchCase") };
            CollectCasesFromStatement(labeled->statement(), ctx);
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
                /*
                forCondition
                    : (forDeclaration | expression ? ) ';' forExpression ? ';' forExpression ?
                */

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
            else if (iterationStatement->Foreach())
            {
                /*
                iterationStatement
                    : Foreach '(' declarationSpecifiers Identifier In expression ')' statement
                    ;

                Lowers to:
                    init: eval collection, cache count(), alloc element var and index
                    cond: i < count
                    inner: element = collection.get(i); body
                    increment: i++
                    resume: pop scope
                */

                auto declSpecCtx = iterationStatement->declarationSpecifiers();
                auto varNameTok  = iterationStatement->Identifier();
                auto collExprCtx = iterationStatement->expression();
                auto bodyStmt    = iterationStatement->statement();

                std::string varName = varNameTok->getText();

                auto blockInit      = compiler->CreateBasicBlock("foreachInit");
                auto blockCond      = compiler->CreateBasicBlock("foreachCond");
                auto blockInner     = compiler->CreateBasicBlock("foreachInner");
                auto blockIncrement = compiler->CreateBasicBlock("foreachIncrement");
                auto blockResume    = compiler->CreateBasicBlock("foreachResume");

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
        else if (selectionStatement)
        {
            /*
            selectionStatement
                : 'if' '(' expression ')' statement ('else' statement)?
                | 'switch' '(' expression ')' statement
                ;
            */

            if (selectionStatement->If())
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

                if (switchCtx.isStringSwitch)
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
        auto name = nameOverride.empty() ? this->getFunctionName(func) : nameOverride;
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

        auto blockItemList = func->compoundStatement()->blockItemList();

        if (blockItemList)
            ParseBlockItemList(blockItemList);

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
                auto valLLVM = llvm::dyn_cast<llvm::ConstantInt>(ParseConditionalExpression(cond));
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

        if (typeAndValue.TypeName.empty())
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

            if (paramTypeList != nullptr)
            {
                // If there is parameter list, then it is a function.
                auto declParams = ParseParameterTypeList(paramTypeList);
                std::vector<MyCompilerLLVM::TypeAndValue> allParams(declParams.begin(), declParams.end());

                bool ellipsis = paramTypeList->Ellipsis() != nullptr;
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
                            // If RHS is a struct (not already a fat struct), upcast it to the interface.
                            if (right && right->getType() != compiler->GetFatPtrType())
                            {
                                std::string structName = rightNV.TypeAndValue.TypeName;
                                if (!structName.empty())
                                {
                                    auto vtable = compiler->GetOrCreateVTable(structName, typeAndValue.TypeName);
                                    // BuildInterfaceFatValue needs a *pointer* to the struct data,
                                    // not the loaded struct value.
                                    // - Pointer types (e.g. StringData*): the loaded value IS the pointer → use it directly.
                                    // - Value types (e.g. Point by value): use the alloca address; spill if no storage.
                                    llvm::Value* dataPtr;
                                    if (rightNV.TypeAndValue.Pointer)
                                    {
                                        // RHS is already a pointer to the struct (e.g. StringData* sd)
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
                            right = ParseAssignmentExpression(assignmentExpression);
                        }
                    }
                    else if (initializer->Default() != nullptr)
                    {
                        right = GenerateDefaultValue(typeAndValue);
                    }
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
                    compiler->CreateGlobalVariable(typeAndValue, constant);
                }
                else
                {
                    auto alloc = compiler->CreateLocalVariable(typeAndValue, right ? right->getType() : nullptr, arraySize, line);
                    allocList.push_back(std::pair(name, alloc));

                    if (right != nullptr)
                    {
                        compiler->CreateAssignment(right, alloc);
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
                                    if (tcs.size() == 1)
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
        // Fall back: no TypeName info available
        MyCompilerLLVM::NamedVariable result;
        result.Primary = ParseAssignmentExpression(ctx);
        if (result.Primary) result.BaseType = result.Primary->getType();
        return result;
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

            if (operatorText == "??=")
            {
                // Null-coalescing assignment: x ??= rhs  →  if (x == 0/null) x = rhs
                auto* lhs = compiler->CreateLoad(destination);

                auto* assignBlock  = compiler->CreateBasicBlock("nullcoalasgn_assign");
                auto* resumeBlock  = compiler->CreateBasicBlock("nullcoalasgn_resume");

                // Jump to assign block only when lhs is null/zero
                compiler->CreateConditionJump(lhs, resumeBlock, assignBlock);

                compiler->SwitchToBlock(assignBlock);
                auto* rhs = ParseAssignmentExpression(assignCtx);
                compiler->CreateAssignment(rhs, destination);
                compiler->CreateJump(resumeBlock);

                compiler->SwitchToBlock(resumeBlock);
                return compiler->CreateLoad(destination);
            }

            auto right = ParseAssignmentExpression(assignCtx);

            if (operatorText != "=")
            {
                auto left = compiler->CreateLoad(destination);
                right = compiler->CreateOperation(operatorText, left, right);
            }

            return compiler->CreateAssignment(right, destination);
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

    llvm::Value* ParseConditionalExpression(CFlatParser::ConditionalExpressionContext* ctx)
    {
        auto* compiler = Compiler(ctx);
        auto logicCtx = ctx->logicalOrExpression();

        if (ctx->QuestionQuestion())
        {
            // Null-coalescing: lhs ?? rhs  →  (lhs != null) ? lhs : rhs
            auto* lhs = ParseLogicalOrExpression(logicCtx);
            if (!lhs) return nullptr;

            auto* resultAlloca = compiler->CreateAlloca(lhs->getType());

            auto* nullBlock = compiler->CreateBasicBlock("nullcoal_null");
            auto* notNullBlock = compiler->CreateBasicBlock("nullcoal_notnull");
            auto* resumeBlock = compiler->CreateBasicBlock("nullcoal_resume");

            compiler->CreateConditionJump(lhs, notNullBlock, nullBlock);
            // insert point is now notNullBlock (lhs is not null)
            compiler->CreateAssignment(lhs, resultAlloca);
            compiler->CreateJump(resumeBlock);

            compiler->SwitchToBlock(nullBlock);
            auto* rhs = ParseConditionalExpression(ctx->conditionalExpression());
            compiler->CreateAssignment(rhs, resultAlloca);
            compiler->CreateJump(resumeBlock);

            compiler->SwitchToBlock(resumeBlock);
            return compiler->CreateLoad(resultAlloca);
        }

        // Grammar: logicalOrExpression ('?' expression ':' conditionalExpression)?
        // — so `expression` is the TRUE branch and `conditionalExpression` is the FALSE branch.
        auto expressionTrueCtx = ctx->expression();
        auto expressionFalseCtx = ctx->conditionalExpression();

        if (logicCtx != nullptr)
        {
            auto expression = ParseLogicalOrExpression(logicCtx);

            // Both expression should exist or not exist.
            if ((expressionFalseCtx != nullptr) != (expressionTrueCtx != nullptr))
            {
                LogErrorContext(ctx, "Conditional expression requires both true and false branches.");
                return nullptr;
            }
            else if (expressionFalseCtx != nullptr && (expressionTrueCtx != nullptr))
            {
                auto trueValue = ParseExpression(expressionTrueCtx);
                auto falseValue = ParseConditionalExpression(expressionFalseCtx);

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

                auto selectValue = compiler->CreateSelect(expression, falseValue, trueValue);
                return selectValue;
            }

            return expression;
        }

        LogErrorContext(ctx, "Conditional expression has no logical-or sub-expression.");
        return nullptr;
    }

    llvm::Value* ParseLogicalOrExpression(CFlatParser::LogicalOrExpressionContext* ctx)
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
            auto elseBlock = compiler->GetElseBlock();

            if (elseBlock)
            {
                for (const auto& logicCtx : logicCtxs)
                {
                    if (left == nullptr)
                    {
                        left = ParseLogicalAndExpression(logicCtx);
                    }
                    else
                    {
                        auto falseBlock = compiler->CreateBasicBlock("falseOR");
                        auto branch = compiler->CreateConditionJump(left, elseBlock, falseBlock);

                        compiler->InitializeBlock(falseBlock, false);
                        llvm::Value* right = ParseLogicalAndExpression(logicCtx);
                        left = compiler->CreateOperation(MyCompilerLLVM::Operation::LogicalOr, left, right);
                    }
                }
            }
            else
            {
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
                return compiler->CreateLoad(resultStorage);
            }

            return left;
        }

        LogErrorContext(ctx, "Logical-OR expression has no operands.");
        return nullptr;
    }

    llvm::Value* ParseLogicalAndExpression(CFlatParser::LogicalAndExpressionContext* ctx)
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
                return compiler->CreateLoad(resultStorage);
            }
            return left;
        }

        LogErrorContext(ctx, "Logical-AND expression has no operands.");
        return nullptr;
    }

    llvm::Value* ParseInclusiveOrExpression(CFlatParser::InclusiveOrExpressionContext* ctx)
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
        return nullptr;
    }

    llvm::Value* ParseExclusiveOrExpression(CFlatParser::ExclusiveOrExpressionContext* ctx)
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
        return nullptr;
    }

    llvm::Value* ParseAndExpression(CFlatParser::AndExpressionContext* ctx)
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
        return nullptr;
    }

    llvm::Value* ParseEqualityExpression(CFlatParser::EqualityExpressionContext* ctx)
    {
        auto nextCtxs = ctx->typeCheckExpression();
        if (nextCtxs.size() == 1)
        {
            return ParseTypeCheckExpression(nextCtxs[0]);
        }
        else if (nextCtxs.size() == 2)
        {
            auto left  = ParseTypeCheckExpression(nextCtxs[0]);
            auto right = ParseTypeCheckExpression(nextCtxs[1]);
            std::string op = ctx->children[1]->getText();

            auto* overload = TryBinaryOperatorOverload(left, op, right, ctx);
            return overload ? overload : Compiler(ctx)->CreateOperation(op, left, right);
        }

        LogErrorContext(ctx, "Equality expression has unexpected operand count.");
        return nullptr;
    }

    llvm::Value* ParseTypeCheckExpression(CFlatParser::TypeCheckExpressionContext* ctx)
    {
        auto relCtx = ctx->relationalExpression();
        if (!relCtx)
        {
            LogErrorContext(ctx, "Type check expression has no operand.");
            return nullptr;
        }

        auto result = ParseRelationalExpression(relCtx);

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
        }

        return result;
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

        auto targetIt = compiler->dataStructures.find(targetTypeName);
        if (targetIt == compiler->dataStructures.end())
        {
            LogErrorContext(ctx, std::format("'{}' is not a known struct type for 'as' cast", targetTypeName));
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

    llvm::Value* ParseRelationalExpression(CFlatParser::RelationalExpressionContext* ctx)
    {
        auto nextCtxs = ctx->shiftExpression();
        if (nextCtxs.size() == 1)
        {
            return ParseShiftExpression(nextCtxs[0]);
        }
        else if (nextCtxs.size() == 2)
        {
            auto left  = ParseShiftExpression(nextCtxs[0]);
            auto right = ParseShiftExpression(nextCtxs[1]);
            std::string op = ctx->children[1]->getText();

            auto* overload = TryBinaryOperatorOverload(left, op, right, ctx);
            return overload ? overload : Compiler(ctx)->CreateOperation(op, left, right);
        }

        LogErrorContext(ctx, "Relational expression has unexpected operand count.");
        return nullptr;
    }

    llvm::Value* ParseShiftExpression(CFlatParser::ShiftExpressionContext* ctx)
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
        return nullptr;
    }

    llvm::Value* ParseAdditiveExpression(CFlatParser::AdditiveExpressionContext* ctx)
    {
        auto nextCtxs = ctx->multiplicativeExpression();

        if (nextCtxs.size() == 1)
        {
            return ParseMultiplicativeExpression(nextCtxs[0]);
        }
        else if (nextCtxs.size() > 1)
        {
            llvm::Value* lvalue = ParseMultiplicativeExpression(nextCtxs[0]);

            for (size_t i = 1; i < nextCtxs.size(); i++)
            {
                llvm::Value* rvalue = ParseMultiplicativeExpression(nextCtxs[i]);
                std::string op = ctx->children[i * 2 - 1]->getText();

                auto* overload = TryBinaryOperatorOverload(lvalue, op, rvalue, ctx);
                lvalue = overload ? overload : Compiler(ctx)->CreateOperation(op, lvalue, rvalue);
            }

            return lvalue;
        }

        LogErrorContext(ctx, "Additive expression has no operands.");
        return nullptr;
    }

    llvm::Value* LoadNamedVariable(MyCompilerLLVM::NamedVariable& namedVar)
    {
        auto* compiler = Compiler();
        if (namedVar.TypeAndValue.Pointer)
        {
            if (namedVar.Primary != nullptr)
                return namedVar.Primary;
            if (namedVar.Storage != nullptr)
            {
                // Local variables and globals store the pointer value inside an alloca/global.
                // Function arguments have the pointer value as the argument itself — return directly.
                if (llvm::isa<llvm::AllocaInst>(namedVar.Storage) ||
                    llvm::isa<llvm::GlobalVariable>(namedVar.Storage))
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

    llvm::Value* ParseMultiplicativeExpression(CFlatParser::MultiplicativeExpressionContext* ctx)
    {
        auto nextCtxs = ctx->castExpression();

        if (nextCtxs.size() == 1)
        {
            auto namedVar = ParseCastExpression(nextCtxs[0]);
            return LoadNamedVariable(namedVar);
        }
        else if (nextCtxs.size() > 1)
        {
            auto firstNV = ParseCastExpression(nextCtxs[0]);
            llvm::Value* lvalue = LoadNamedVariable(firstNV);

            for (size_t i = 1; i < nextCtxs.size(); i++)
            {
                auto rightNV = ParseCastExpression(nextCtxs[i]);
                llvm::Value* rvalue = LoadNamedVariable(rightNV);
                std::string op = ctx->children[i * 2 - 1]->getText();

                auto* overload = TryBinaryOperatorOverload(lvalue, op, rvalue, ctx);
                lvalue = overload ? overload : Compiler(ctx)->CreateOperation(op, lvalue, rvalue);
            }

            return lvalue;
        }

        LogErrorContext(ctx, "Multiplicative expression has no operands.");
        return nullptr;
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

            if (namedVar.Storage)
            {
                // If storage is available, then load it with new type.
                namedVar.Primary = compiler->CreateLoad(type, namedVar.Storage);
            }
            else
            {
                // Otherwise cast it.
                namedVar.Primary = compiler->CreateCast(namedVar.Primary, type);
            }

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
                typeValue.TypeName = typeSpecs[0]->getText();
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

        if (postFixCtx != nullptr)
        {
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

                namedVar.Primary = compiler->CreateLoad(namedVar.Storage);
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
                namedVar.Primary = compiler->CreateNeg(newValue);
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

        LogErrorContext(ctx, "Unary expression has no recognized form.");
        return {};
    }

    std::string ParseTypeSpecifierName(CFlatParser::TypeSpecifierContext* ctx)
    {
        if (ctx->genericIdentifier() && ctx->genericIdentifier()->genericTypeParameters())
        {
            // Generic type: Box<int> → Box__int
            std::string base = ctx->genericIdentifier()->Identifier()->getText();
            std::vector<std::string> args;
            for (auto* tp : ctx->genericIdentifier()->genericTypeParameters()->typeParameterList()->typeSpecifier())
                args.push_back(tp->getText());
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

        MyCompilerLLVM::TypeAndValue typeInfo{ .TypeName = typeName };
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
        if (auto* ue = tryGetUnaryExpression(ctx->expression()))
        {
            auto namedVar = ParseUnaryExpression(ue);
            typeName = namedVar.TypeAndValue.TypeName;
            ptrVal = namedVar.Storage ? compiler->CreateLoad(namedVar.Storage) : namedVar.Primary;
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
                        if (prevPrimary->genericIdentifier() != nullptr && prevPrimary->genericIdentifier()->genericTypeParameters() != nullptr && prevPrimary->genericIdentifier()->Identifier() != nullptr)
                        {
                            std::string mangledName = prevPrimary->genericIdentifier()->Identifier()->getText();
                            for (auto* typeParamSpec : prevPrimary->genericIdentifier()->genericTypeParameters()->typeParameterList()->typeSpecifier())
                                mangledName += "__" + typeParamSpec->getText();
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
                                namedVar.TypeAndValue = {};
                                if (result->getType()->isStructTy())
                                {
                                    if (auto* st = llvm::dyn_cast<llvm::StructType>(result->getType()))
                                        if (!st->isLiteral() && st->hasName())
                                            namedVar.TypeAndValue.TypeName = st->getName().str();
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
                            // Use the loaded pointer as the GEP base and the
                            // element type (TypeName without the pointer flag) as element type.
                            auto elementTypeAndValue = namedVar.TypeAndValue;
                            elementTypeAndValue.Pointer = false;
                            auto elementType = Compiler(ctx)->GetType(elementTypeAndValue);
                            auto ptrValue = LoadNamedVariable(namedVar);
                            namedVar.Storage = Compiler(ctx)->CreateGEP(elementType, ptrValue, rvalue);
                            namedVar.BaseType = elementType;
                            namedVar.TypeAndValue.Pointer = false;
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

                                for (const auto& namedArgument : namedArgCtx)
                                {
                                    auto argName = namedArgument->Identifier();
                                    auto argValue = this->ParseAssignmentExpression(namedArgument->assignmentExpression());
                                    if (!argValue) break; // caller's block was terminated (e.g. return-block inline)
                                    MyCompilerLLVM::NamedVariable argVar;

                                    if (argName)
                                        argVar.TypeAndValue.VariableName = argName->getText();
                                    argVar.Primary = argValue;
                                    argVar.BaseType = argValue->getType();

                                    // Extract struct name if this is a struct type
                                    if (auto* st = llvm::dyn_cast<llvm::StructType>(argValue->getType()))
                                    {
                                        auto structName = st->getName().str();
                                        if (!structName.empty())
                                            argVar.TypeAndValue.TypeName = structName;
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
                                namedVar.BaseType = namedVar.Primary->getType();
                            }

                            if (namedVar.BaseType && namedVar.BaseType->isStructTy())
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

    llvm::Value* ParsePrimaryExpression(CFlatParser::PrimaryExpressionContext* ctx)
    {
        auto* compiler = Compiler(ctx);
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

        if (name == "__FILE__")
        {
            auto str = compiler->CreateGlobalString("__FILE__", compiler->GetSourceFileName());
            namedVar.Primary = str;
            namedVar.BaseType = str->getType();
            return namedVar;
        }

        if (name == "__FUNCTION__")
        {
            auto str = compiler->CreateGlobalString("__FUNCTION__", compiler->GetCurrentFunctionName());
            namedVar.Primary = str;
            namedVar.BaseType = str->getType();
            return namedVar;
        }

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
            return namedVar;
        }

        auto funcArgument = compiler->GetFunctionArgument(name);
        if (funcArgument.GetValue() != nullptr)
        {
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
            if (auto* typeSpec = dynamic_cast<CFlatParser::TypeSpecifierContext*>(ruleCtx))
            {
                if (typeSpec->genericIdentifier() != nullptr && typeSpec->genericIdentifier()->genericTypeParameters() != nullptr && typeSpec->genericIdentifier()->Identifier() != nullptr)
                {
                    std::string baseName = typeSpec->genericIdentifier()->Identifier()->getText();
                    std::vector<std::string> typeArgs;
                    for (auto* p : typeSpec->genericIdentifier()->genericTypeParameters()->typeParameterList()->typeSpecifier())
                        typeArgs.push_back(p->getText());
                    std::string mangledName = MangledGenericName(baseName, typeArgs);
                    if (!instantiatedGenerics.count(mangledName))
                    {
                        pendingInstantiations.push_back({baseName, typeArgs, mangledName});
                        instantiatedGenerics.insert(mangledName);
                    }
                }
            }

            // primaryExpression with generic params: e.g. the "Box<MyInt>" in "Box<MyInt>()"
            if (auto* primaryExpr = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(ruleCtx))
            {
                if (primaryExpr->genericIdentifier() != nullptr && primaryExpr->genericIdentifier()->genericTypeParameters() != nullptr && primaryExpr->genericIdentifier()->Identifier() != nullptr)
                {
                    std::string baseName = primaryExpr->genericIdentifier()->Identifier()->getText();
                    std::vector<std::string> typeArgs;
                    for (auto* p : primaryExpr->genericIdentifier()->genericTypeParameters()->typeParameterList()->typeSpecifier())
                        typeArgs.push_back(p->getText());
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
            for (auto typeParamSpec : typeSpec->genericIdentifier()->genericTypeParameters()->typeParameterList()->typeSpecifier())
                typeArgs.push_back(typeParamSpec->getText());

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
            name += "__" + arg;
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
            if (structIt == genericStructTemplates.end() && classIt == genericClassTemplates.end())
            {
                if (Compiler()->IsVerbose())
                    std::cout << "[verbose]   skip instantiation '" << pending.mangledName
                              << "': template '" << pending.templateName << "' not found\n";
                continue; // not a generic template
            }

            if (Compiler()->IsVerbose())
                std::cout << "[verbose]   instantiate generic: " << pending.mangledName << "\n";

            const auto& typeParams = genericStructTypeParams[pending.templateName];

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
            return;
        }

        if (compiler->IsVerbose())
            std::cout << "[verbose]     parse decl list: " << structName << "\n";
        auto declarationList = ctx->declaration();
        std::vector<llvm::Type*> types;

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
        // Create default constructor
        {
            auto funcDef = compiler->CreateFunctionDefinition(structName, returnType, {});

            std::vector<llvm::Value*> initilizers;
            for (auto& typeValue : declList)
            {
                auto initilizer = typeValue.Initializer;
                llvm::Value* rvalue = nullptr;
                if (initilizer != nullptr)
                {
                    auto assignmentExpression = initilizer->assignmentExpression();
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
                    else if (initilizer->Default() != nullptr)
                    {
                        rvalue = GenerateDefaultValue(typeValue);
                    }
                }
                else
                {
                    std::cout << "Uninitialize field \"" << structName << "::" << typeValue.VariableName << "\".\n";
                }

                initilizers.push_back(rvalue);
            }

            llvm::Value* structVal = llvm::UndefValue::get(structType);

            MyCompilerLLVM::TypeAndValue myStruct;
            myStruct.TypeName = structName;
            myStruct.VariableName = "_" + structName;

            unsigned int structIndex = 0;

            for (auto rvalue : initilizers)
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

        // Structs cannot implement interfaces — only classes can.
        compiler->RegisterStructInterfaces(structName, {});

        // Process any generic instantiations that were queued during this struct definition
        // ProcessPendingInstantiations();
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
            return;
        }

        if (compiler->IsVerbose())
            std::cout << "[verbose]     parse decl list: " << structName << "\n";
        auto declarationList = ctx->declaration();
        std::vector<llvm::Type*> types;

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
        // Create default constructor
        {
            auto funcDef = compiler->CreateFunctionDefinition(structName, returnType, {});

            std::vector<llvm::Value*> initilizers;
            for (auto& typeValue : declList)
            {
                auto initilizer = typeValue.Initializer;
                llvm::Value* rvalue = nullptr;
                if (initilizer != nullptr)
                {
                    auto assignmentExpression = initilizer->assignmentExpression();
                    if (assignmentExpression != nullptr)
                    {
                        rvalue = ParseAssignmentExpression(assignmentExpression);
                        if (typeValue.TypeName == "auto")
                        {
                            typeValue.TypeName = rvalue->getType()->getStructName();
                            structType = compiler->CreateStructType(structName, declList);
                        }
                    }
                    else if (initilizer->Default() != nullptr)
                    {
                        rvalue = GenerateDefaultValue(typeValue);
                    }
                }
                else
                {
                    std::cout << "Uninitialize field \"" << structName << "::" << typeValue.VariableName << "\".\n";
                }

                initilizers.push_back(rvalue);
            }

            llvm::Value* structVal = llvm::UndefValue::get(structType);

            MyCompilerLLVM::TypeAndValue myStruct;
            myStruct.TypeName = structName;
            myStruct.VariableName = "_" + structName;

            unsigned int structIndex = 0;

            for (auto rvalue : initilizers)
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
                for (auto* typeSpec : genId->genericTypeParameters()->typeParameterList()->typeSpecifier())
                {
                    std::string typeArg = typeSpec->getText();
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

        for (auto typeSpec : typeParamList->typeSpecifier())
        {
            // Generic type parameters must be simple identifiers, not built-in types
            if (!typeSpec->genericIdentifier() || !typeSpec->genericIdentifier()->Identifier())
            {
                LogErrorContext(typeSpec, "Generic type parameter must be an identifier, not a built-in type");
                continue;
            }
            typeParams.push_back(typeSpec->genericIdentifier()->Identifier()->getText());
        }

        return typeParams;
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
        int line = static_cast<int>(symbol->getLine());
        int column = static_cast<int>(symbol->getCharPositionInLine());
        std::cout << std::format("[{}:{}] {} : {}\n", line, column, ctx->getText(), errorMessage);
        exit(1);
    }

    void LogErrorContext(antlr4::ParserRuleContext* ctx, std::string errorMessage)
    {
        int line = static_cast<int>(ctx->getStart()->getLine());
        int column = static_cast<int>(ctx->getStart()->getCharPositionInLine());
        std::cout << std::format("[{}:{}] {} : {}\n", line, column, ctx->getText(), errorMessage);
        exit(1);
    }

    void LogWarningContext(antlr4::ParserRuleContext* ctx, std::string warningMessage)
    {
        size_t line = ctx->getStart()->getLine();
        size_t column = ctx->getStart()->getCharPositionInLine();
        std::cout << std::format("[{}:{}] {} : {}\n", line, column, ctx->getText(), warningMessage);
    }

    void PrintContext(antlr4::ParserRuleContext* ctx, std::string suffix = "")
    {
        size_t line = ctx->getStart()->getLine();
        size_t column = ctx->getStart()->getCharPositionInLine();
        std::cout << std::format("[{}:{}] {} : {} : {}\n", line, column, parser->getRuleNames()[ctx->getRuleIndex()], ctx->getText(), suffix);
    }

    void enterEveryRule(antlr4::ParserRuleContext* ctx) override
    {
        if constexpr (debugPrint)
            PrintContext(ctx);
    }
};
