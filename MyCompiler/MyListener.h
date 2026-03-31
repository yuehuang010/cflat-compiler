#pragma once
#include <iostream>
#include <unordered_map>
#include <format>
#include <variant>
#include <cstdlib>

#include "CParser.h"
#include "CLexer.h"
#include "CBaseListener.h"
#include "MyCompilerLLVM.h"


// Returns true when a function's entire body is a single 'return { ... };' statement,
// marking it as a return-block function (to be inlined at every call site).
static bool IsReturnBlockFunction(CParser::FunctionDefinitionContext* func)
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

// ForwardRefScanner performs a lightweight pre-pass over the AST to register
// all function signatures and struct type shells before the main code-gen walk.
// This allows functions and types to be used before their definition in source.
class ForwardRefScanner
{
private:
    MyCompilerLLVM* compilerLLVM;

    MyCompilerLLVM::DeclTypeAndValue ParseDeclarationSpecifiers(CParser::DeclarationSpecifiersContext* declSpecs)
    {
        MyCompilerLLVM::DeclTypeAndValue declType;
        for (auto declSpec : declSpecs->declarationSpecifier())
        {
            auto typeSpec = declSpec->typeSpecifier();
            auto storageSpec = declSpec->storageClassSpecifier();
            if (typeSpec != nullptr)
            {
                declType.TypeName = typeSpec->getText();
                declType.Pointer = declSpec->pointer() != nullptr;
                declType.ArraySize = declSpec->assignmentExpression();
                declType.IsInterface = compilerLLVM->IsInterfaceType(declType.TypeName);
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

    std::vector<MyCompilerLLVM::DeclTypeAndValue> ParseParameterTypeList(CParser::ParameterTypeListContext* paramTypeList)
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

    void ScanFunctionDefinition(CParser::FunctionDefinitionContext* func, const std::string& structName = {}, const std::string& namespaceName = {})
    {
        // Return-block functions are inlined at call sites — no LLVM proto needed.
        if (IsReturnBlockFunction(func))
            return;

        std::string name = func->directDeclarator()->getText();
        if (!namespaceName.empty())
            name = namespaceName + "." + name;

        auto returnType = ParseDeclarationSpecifiers(func->declarationSpecifiers());
        auto* paramTypeList = func->parameterTypeList();
        auto params = ParseParameterTypeList(paramTypeList);
        bool varargs = paramTypeList && paramTypeList->Ellipsis() != nullptr;

        if (!structName.empty())
        {
            MyCompilerLLVM::DeclTypeAndValue thisParam;
            thisParam.TypeName = structName;
            thisParam.VariableName = structName + "__";
            thisParam.Pointer = true;
            params.insert(params.begin(), thisParam);
        }

        std::vector<MyCompilerLLVM::TypeAndValue> allParams(params.begin(), params.end());
        compilerLLVM->CreateFunctionDeclaration(name, returnType, allParams, returnType.external, varargs);

        // Pre-declare overloads for default parameters
        int firstDefault = -1;
        for (int i = 0; i < (int)params.size(); i++)
        {
            if (params[i].DefaultValue != nullptr) { firstDefault = i; break; }
        }
        for (int cutoff = firstDefault; firstDefault >= 0 && cutoff < (int)params.size(); cutoff++)
        {
            std::vector<MyCompilerLLVM::TypeAndValue> wrapperParams(params.begin(), params.begin() + cutoff);
            compilerLLVM->CreateFunctionDeclaration(name, returnType, wrapperParams, false, false);
        }
    }

    void ScanInterfaceDefinition(CParser::InterfaceDefinitionContext* ctx)
    {
        auto identifiers = ctx->Identifier();
        std::string name = identifiers[0]->getText();

        std::vector<std::string> parentNames;
        for (size_t i = 1; i < identifiers.size(); i++)
            parentNames.push_back(identifiers[i]->getText());

        std::vector<MyCompilerLLVM::InterfaceMethod> methods;
        for (auto method : ctx->interfaceMethod())
        {
            MyCompilerLLVM::InterfaceMethod m;
            m.ReturnType = ParseDeclarationSpecifiers(method->declarationSpecifiers());
            m.Name = method->directDeclarator()->getText();
            auto declParams = ParseParameterTypeList(method->parameterTypeList());
            for (const auto& p : declParams)
                m.Parameters.push_back(p);
            methods.push_back(std::move(m));
        }

        compilerLLVM->CreateInterfaceDefinition(name, parentNames, methods);
    }

    void ScanStructDefinition(CParser::StructClassUnionDefinitionContext* ctx)
    {
        std::string structName = ctx->directDeclarator()->getText();

        // Register opaque struct so the type is known for pointer/field use
        compilerLLVM->CreateStructType(structName, {});

        // Pre-declare default constructor
        MyCompilerLLVM::TypeAndValue returnType{ .TypeName = structName };
        compilerLLVM->CreateFunctionDeclaration(structName, returnType, {});

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
            compilerLLVM->CreateFunctionDeclaration("~" + structName, voidReturn, { thisParam });
        }
    }

public:
    ForwardRefScanner(MyCompilerLLVM* compiler) : compilerLLVM(compiler) {}

    void ScanExternalDeclaration(CParser::ExternalDeclarationContext* ctx, const std::string& namespaceName = {})
    {
        if (auto ns = ctx->namespaceDefinition())
            ScanNamespace(ns, namespaceName);
        else if (auto func = ctx->functionDefinition())
            ScanFunctionDefinition(func, {}, namespaceName);
        else if (auto dataStruct = ctx->structClassUnionDefinition())
            ScanStructDefinition(dataStruct);
        else if (auto iface = ctx->interfaceDefinition())
            ScanInterfaceDefinition(iface);
    }

    void ScanNamespace(CParser::NamespaceDefinitionContext* ctx, const std::string& parentNamespace = {})
    {
        std::string namespaceName = ctx->Identifier()->getText();
        if (!parentNamespace.empty())
            namespaceName = parentNamespace + "." + namespaceName;

        for (auto* extDecl : ctx->externalDeclaration())
            ScanExternalDeclaration(extDecl, namespaceName);
    }
};


class MyListener : public CBaseListener
{
private:
    CParser* parser;
    MyCompilerLLVM* compilerLLVM;
    std::unordered_map<llvm::Value*, int> PlusPlus;
    bool global_scope = true; // true when parsing an entity in the global scope.
    constexpr static bool debugPrint = false;

    struct SwitchCaseEntry
    {
        llvm::ConstantInt* value;
        llvm::BasicBlock* block;
    };

    struct SwitchContext
    {
        std::unordered_map<CParser::LabeledStatementContext*, SwitchCaseEntry> caseMap;
        llvm::BasicBlock* defaultBlock = nullptr;
        llvm::BasicBlock* resumeBlock = nullptr;
    };

    std::vector<SwitchContext> switchStack;

    std::string getFunctionName(CParser::FunctionDefinitionContext* ctx)
    {
        std::string name;
        auto directDecl = ctx->directDeclarator();
        name = directDecl->getText();

        return name;
    }

    MyCompilerLLVM::DeclTypeAndValue ParseDeclarationSpecifiers(CParser::DeclarationSpecifiersContext* declSpecs)
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
                declType.TypeName = typeSpec->getText();
                declType.Pointer = declSpec->pointer() != nullptr;
                declType.ArraySize = declSpec->assignmentExpression();
                declType.IsInterface = compilerLLVM->IsInterfaceType(declType.TypeName);
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

    MyCompilerLLVM::DeclTypeAndValue getFunctionReturnType(CParser::FunctionDefinitionContext* ctx)
    {
        auto declSpecs = ctx->declarationSpecifiers();

        return ParseDeclarationSpecifiers(declSpecs);
    }

public:
    MyListener(CParser* parser, MyCompilerLLVM* compilerLLVM)
    {
        this->parser = parser;
        this->compilerLLVM = compilerLLVM;
    }

    void ParseInterfaceDefinition(CParser::InterfaceDefinitionContext* ctx)
    {
        auto identifiers = ctx->Identifier();
        std::string name = identifiers[0]->getText();

        std::vector<std::string> parentNames;
        for (size_t i = 1; i < identifiers.size(); i++)
            parentNames.push_back(identifiers[i]->getText());

        std::vector<MyCompilerLLVM::InterfaceMethod> methods;

        for (auto method : ctx->interfaceMethod())
        {
            MyCompilerLLVM::InterfaceMethod m;
            m.ReturnType = ParseDeclarationSpecifiers(method->declarationSpecifiers());
            m.Name = method->directDeclarator()->getText();

            auto declParams = ParseParameterTypeList(method->parameterTypeList());
            for (const auto& p : declParams)
            {
                MyCompilerLLVM::TypeAndValue tv = p;
                m.Parameters.push_back(tv);
            }

            methods.push_back(std::move(m));
        }

        compilerLLVM->CreateInterfaceDefinition(name, parentNames, methods);
    }

    void ParseUsingDeclaration(CParser::UsingDeclarationContext* ctx)
    {
        auto identifiers = ctx->Identifier();
        std::string alias = identifiers[0]->getText();
        std::string target;
        for (size_t i = 1; i < identifiers.size(); i++)
        {
            if (!target.empty()) target += ".";
            target += identifiers[i]->getText();
        }
        if (global_scope)
            compilerLLVM->RegisterNamespaceAlias(alias, target);
        else
            compilerLLVM->RegisterLocalNamespaceAlias(alias, target);
    }

    void ParseExternalDeclaration(CParser::ExternalDeclarationContext* ctx, const std::string& namespaceName = {})
    {
        auto func = ctx->functionDefinition();
        auto dataStruct = ctx->structClassUnionDefinition();
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
            ParseStructClassUnionDefinition(dataStruct);
        }
    }

    void ParseNamespaceDefinition(CParser::NamespaceDefinitionContext* ctx, const std::string& parentNamespace = {})
    {
        std::string namespaceName = ctx->Identifier()->getText();
        if (!parentNamespace.empty())
            namespaceName = parentNamespace + "." + namespaceName;
        compilerLLVM->RegisterNamespace(namespaceName);

        for (auto* extDecl : ctx->externalDeclaration())
            ParseExternalDeclaration(extDecl, namespaceName);
    }

    void enterExternalDeclaration(CParser::ExternalDeclarationContext* ctx) override
    {
        if constexpr (debugPrint)
            return;

        // Skip nodes nested inside a namespace — they are handled by ParseNamespaceDefinition.
        if (dynamic_cast<CParser::NamespaceDefinitionContext*>(ctx->parent))
            return;

        ParseExternalDeclaration(ctx);
    }

    void ParseBlockItemList(CParser::BlockItemListContext* ctx)
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
    void CollectCasesFromStatement(CParser::StatementContext* stmt, SwitchContext& ctx)
    {
        if (!stmt) return;
        auto labeled = stmt->labeledStatement();
        if (!labeled) return;

        if (labeled->Case())
        {
            auto val = llvm::dyn_cast<llvm::ConstantInt>(
                ParseConditionalExpression(labeled->constantExpression()->conditionalExpression()));
            if (!val)
                LogErrorContext(labeled, "case value must be a constant integer expression");
            ctx.caseMap[labeled] = { val, compilerLLVM->CreateBasicBlock("switchCase") };
            CollectCasesFromStatement(labeled->statement(), ctx);
        }
        else if (labeled->Default())
        {
            ctx.defaultBlock = compilerLLVM->CreateBasicBlock("switchDefault");
            CollectCasesFromStatement(labeled->statement(), ctx);
        }
    }

    void ParseStatement(CParser::StatementContext* statement)
    {
        compilerLLVM->SetCurrentDebugLocation(statement->getStart()->getLine());

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
                compilerLLVM->CreateJump(targetBlock);    // fallthrough if no terminator yet
                compilerLLVM->SwitchToBlock(targetBlock);
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
                    compilerLLVM->InitializeBlock(nullptr, true);
                    if (auto* blockItems = blockBody->blockItemList())
                        ParseBlockItemList(blockItems);
                    compilerLLVM->CreateBlockBreak(nullptr, true);
                }
                else
                {
                    auto express = jump->expression();
                    if (express != nullptr)
                    {
                        auto right = ParseExpression(express);
                        compilerLLVM->CreateReturnCall(right);
                    }
                    else
                    {
                        compilerLLVM->CreateReturnCall(nullptr);
                    }
                }
                return;
            }
            else if (jump->Continue())
            {
                compilerLLVM->CreateContinueCall();
                return;
            }
            else if (jump->Break())
            {
                compilerLLVM->CreateBreakCall();
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

                auto blockCondition = compilerLLVM->CreateBasicBlock("whileCondition");
                auto blockInner = compilerLLVM->CreateBasicBlock("whileInner");
                auto blockResume = compilerLLVM->CreateBasicBlock("whileResume");

                compilerLLVM->CreateBlockBreak(blockCondition, false);

                compilerLLVM->InitializeBlock(blockCondition, true, blockCondition, blockResume, blockResume);
                auto condition = ParseExpression(expression);
                compilerLLVM->CreateConditionJump(condition, blockInner, blockResume);

                compilerLLVM->InitializeBlock(blockInner, false);
                ParseStatement(innerStatement);
                compilerLLVM->CreateContinueCall();

                // resume
                compilerLLVM->InitializeBlock(blockResume, false);

                // pop the stack
                compilerLLVM->CreateBlockBreak(nullptr, true);

                return;
            }
            else if (iterationStatement->Do())
            {
                auto expression = iterationStatement->expression();
                auto innerStatement = iterationStatement->statement();

                auto blockInner = compilerLLVM->CreateBasicBlock("doWhileInner");
                auto blockCondition = compilerLLVM->CreateBasicBlock("doWhileCondition");
                auto blockResume = compilerLLVM->CreateBasicBlock("doWhileResume");

                compilerLLVM->CreateBlockBreak(blockInner, false);

                compilerLLVM->InitializeBlock(blockInner, true, blockCondition, blockResume, blockResume);
                ParseStatement(innerStatement);
                compilerLLVM->CreateContinueCall();

                compilerLLVM->InitializeBlock(blockCondition, false);
                auto condition = ParseExpression(expression);
                compilerLLVM->CreateConditionJump(condition, blockInner, blockResume);

                // resume
                compilerLLVM->InitializeBlock(blockResume, false);

                // pop the stack
                compilerLLVM->CreateBlockBreak(nullptr, true);
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

                auto blockInit = compilerLLVM->CreateBasicBlock("forInit");
                auto blockCondition = compilerLLVM->CreateBasicBlock("forCondition");
                auto blockInner = compilerLLVM->CreateBasicBlock("forInner");
                auto blockIncrement = compilerLLVM->CreateBasicBlock("forIncrement");
                auto blockResume = compilerLLVM->CreateBasicBlock("forResume");

                compilerLLVM->CreateBlockBreak(blockInit, false);

                // Init => (Condition => Inner => Increment =>Condition)

                // initialization
                compilerLLVM->InitializeBlock(blockInit, true, blockIncrement, blockResume, blockResume);
                if (declaration)
                    ParseForDeclaration(declaration);
                if (expressionCtx)
                    ParseExpression(expressionCtx);

                compilerLLVM->CreateContinueCall();

                // Condition
                compilerLLVM->InitializeBlock(blockCondition, false);
                auto condition = ParseAssignmentExpression(compareCtx);
                compilerLLVM->CreateConditionJump(condition, blockInner, blockResume);

                // Inner statement
                compilerLLVM->InitializeBlock(blockInner, false);
                ParseStatement(innerStatement);
                compilerLLVM->CreateContinueCall();

                // Increment
                compilerLLVM->InitializeBlock(blockIncrement, false);

                auto assignments = forIncrementCtx->assignmentExpression();
                for (auto assign : assignments)
                {
                    ParseAssignmentExpression(assign);
                    ProcessPlusPlus();
                }

                compilerLLVM->CreateBlockBreak(blockCondition, false);

                // resume
                compilerLLVM->InitializeBlock(blockResume, false);

                // pop the stack
                compilerLLVM->CreateBlockBreak(nullptr, true);

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
                auto blockCondition = compilerLLVM->CreateBasicBlock("ifCondition");
                auto blockTrue = compilerLLVM->CreateBasicBlock("ifTrue");
                auto blockResume = compilerLLVM->CreateBasicBlock("ifResume");
                llvm::BasicBlock* blockElse = selectionStatement->Else() == nullptr ? nullptr : compilerLLVM->CreateBasicBlock("ifFalse");
                auto blockFalse = blockElse ? blockElse : blockResume;

                compilerLLVM->CreateBlockBreak(blockCondition, false);

                compilerLLVM->InitializeBlock(blockCondition, true, nullptr, nullptr, blockFalse);
                auto condition = ParseExpression(expression);
                compilerLLVM->CreateConditionJump(condition, blockTrue, blockFalse);

                compilerLLVM->InitializeBlock(blockTrue, false);
                ParseStatement(innerStatement[0]);

                compilerLLVM->CreateBlockBreak(blockResume, true);

                if (blockElse != nullptr)
                {
                    // else statement
                    compilerLLVM->InitializeBlock(blockElse, true);
                    ParseStatement(innerStatement[1]);
                    compilerLLVM->CreateBlockBreak(blockResume, true);
                }

                // resume
                compilerLLVM->InitializeBlock(blockResume, false);
                return;
            }
            else if (selectionStatement->Switch())
            {
                auto expression = selectionStatement->expression();
                auto body = selectionStatement->statement(0)->compoundStatement();

                SwitchContext switchCtx;
                switchCtx.resumeBlock = compilerLLVM->CreateBasicBlock("switchResume");

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

                // Emit switch instruction
                auto condVal = ParseExpression(expression);
                auto switchInst = compilerLLVM->CreateSwitchInst(condVal, switchDefault, (unsigned)switchCtx.caseMap.size());
                for (auto& [labeledCtx, entry] : switchCtx.caseMap)
                    switchInst->addCase(compilerLLVM->CoerceCaseValue(entry.value, condVal->getType()), entry.block);

                // Push scope: break → resumeBlock, no continue (propagates to outer loop)
                compilerLLVM->InitializeBlock(nullptr, true, nullptr, switchCtx.resumeBlock, nullptr);

                switchStack.push_back(switchCtx);

                if (body && body->blockItemList())
                    ParseBlockItemList(body->blockItemList());

                // Fallthrough at end of switch body → resume
                compilerLLVM->CreateBlockBreak(switchCtx.resumeBlock, true);

                switchStack.pop_back();

                compilerLLVM->InitializeBlock(switchCtx.resumeBlock, false);
                return;
            }
        }
        else if (compoundStatement)
        {
            compilerLLVM->InitializeBlock(nullptr, true);
            auto blockList = compoundStatement->blockItemList();
            if (blockList)
                ParseBlockItemList(blockList);
            compilerLLVM->CreateBlockBreak(nullptr, true);
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
        int line)
    {
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

            auto wrapperFn = compilerLLVM->CreateFunctionDefinition(name, returnType, wrapperParams, false, false, line);
            compilerLLVM->InitializeBlock(&wrapperFn->front(), false);

            // Build the full argument list for the forwarding call
            std::vector<MyCompilerLLVM::NamedVariable> callArgs;

            for (int i = 0; i < cutoff; i++)
            {
                callArgs.push_back(compilerLLVM->GetFunctionArgument(params[i].VariableName));
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
                compilerLLVM->CreateFunctionCall2(name, callArgs);
                compilerLLVM->CreateReturnCall(nullptr);
            }
            else
            {
                auto result = compilerLLVM->CreateFunctionCall2(name, callArgs);
                compilerLLVM->CreateReturnCall(result);
            }

            compilerLLVM->CreateBlockBreak(nullptr, true);
            compilerLLVM->ClearCurrentSubprogram();
        }
    }

    void ParseFunctionDefinition(CParser::FunctionDefinitionContext* func, std::string structName = {}, std::string namespaceName = {})
    {
        // Create Function Definition
        auto name = this->getFunctionName(func);
        if (!namespaceName.empty())
            name = namespaceName + "." + name;
        auto returnType = this->getFunctionReturnType(func);
        CParser::ParameterTypeListContext* paramTypeList = func->parameterTypeList();
        auto params = this->ParseParameterTypeList(paramTypeList);
        int line = func->getStart()->getLine();
        bool varargs = paramTypeList && paramTypeList->Ellipsis() != nullptr;

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
            compilerLLVM->RegisterReturnBlock(name, blockBody, params, returnType);
            return;
        }

        std::vector<MyCompilerLLVM::TypeAndValue> allParams(params.begin(), params.end());

        auto fn = compilerLLVM->CreateFunctionDefinition(name, returnType, allParams, returnType.external, varargs, line);

        compilerLLVM->InitializeBlock(&fn->front(), false);

        auto blockItemList = func->compoundStatement()->blockItemList();

        if (blockItemList)
            ParseBlockItemList(blockItemList);

        if (returnType.TypeName != "void" && !compilerLLVM->IsBlockTerminated())
            LogErrorContext(func, std::format("Function '{}' with non-void return type is missing a return statement.", name));

        // if return is void, then this might need a implicit return;
        if (returnType.TypeName == "void")
        {
            compilerLLVM->CreateReturnCall(nullptr);
        }

        // Pop the stack
        compilerLLVM->CreateBlockBreak(nullptr, true);
        compilerLLVM->ClearCurrentSubprogram();

        GenerateDefaultParamOverloads(name, returnType, params, varargs, line);
    }

    std::vector<MyCompilerLLVM::DeclTypeAndValue> ParseDeclarationList(std::vector<CParser::DeclarationContext*> ctx)
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

    std::vector<std::pair<std::string, llvm::AllocaInst*>> ParseForDeclaration(CParser::ForDeclarationContext* ctx)
    {
        auto declSpec = ctx->declarationSpecifiers();
        auto initDecl = ctx->initDeclaratorList();
        return ParseDeclaration(declSpec, initDecl);
    }

    std::vector<std::pair<std::string, llvm::AllocaInst*>> ParseDeclaration(CParser::DeclarationContext* ctx)
    {
        auto declSpec = ctx->declarationSpecifiers();
        auto initDecl = ctx->initDeclaratorList();
        return ParseDeclaration(declSpec, initDecl);
    }

    std::vector<std::pair<std::string, llvm::AllocaInst*>> ParseDeclaration(CParser::DeclarationSpecifiersContext* declSpec, CParser::InitDeclaratorListContext* initDecl)
    {
        std::vector<std::pair<std::string, llvm::AllocaInst*>> allocList;

        int line = declSpec->getStart()->getLine();
        auto typeAndValue = ParseDeclarationSpecifiers(declSpec);
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
                compilerLLVM->CreateFunctionDeclaration(direct->getText(), typeAndValue, allParams, typeAndValue.external, ellipsis);

                // Declare overloads for each suffix of omitted default parameters
                int firstDefault = -1;
                for (int i = 0; i < (int)declParams.size(); i++)
                {
                    if (declParams[i].DefaultValue != nullptr) { firstDefault = i; break; }
                }
                for (int cutoff = firstDefault; firstDefault >= 0 && cutoff < (int)declParams.size(); cutoff++)
                {
                    std::vector<MyCompilerLLVM::TypeAndValue> wrapperParams(declParams.begin(), declParams.begin() + cutoff);
                    compilerLLVM->CreateFunctionDeclaration(direct->getText(), typeAndValue, wrapperParams, typeAndValue.external, false);
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
                        right = ParseAssignmentExpression(assignmentExpression);
                    }
                }

                if (right == nullptr && !typeAndValue.Pointer)
                {
                    auto structData = compilerLLVM->GetDataStructure(typeAndValue.TypeName);
                    if (structData.StructType != nullptr)
                    {
                        LogErrorContext(direct, std::format("({}) struct and class must be initialized on the stack.", typeAndValue.TypeName));
                    }
                }

                if (global_scope)
                {
                    auto constant = llvm::dyn_cast_or_null<llvm::Constant>(right);
                    compilerLLVM->CreateGlobalVariable(typeAndValue, constant);
                }
                else
                {
                    auto alloc = compilerLLVM->CreateLocalVariable(typeAndValue, right ? right->getType() : nullptr, arraySize, line);
                    allocList.push_back(std::pair(name, alloc));

                    if (right != nullptr)
                    {
                        compilerLLVM->CreateAssignment(right, alloc);
                    }
                }
            }
        }

        return allocList;
    }

    llvm::Value* ParseAssignmentExpression(CParser::AssignmentExpressionContext* ctx)
    {
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
            auto right = ParseAssignmentExpression(assignCtx);

            if (operatorText != "=")
            {
                auto left = compilerLLVM->CreateLoad(destination);
                right = compilerLLVM->CreateOperation(operatorText, left, right);
            }

            return compilerLLVM->CreateAssignment(right, destination);
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

    llvm::Value* ParseConditionalExpression(CParser::ConditionalExpressionContext* ctx)
    {
        auto logicCtx = ctx->logicalOrExpression();

        if (ctx->QuestionQuestion())
        {
            // Null-coalescing: lhs ?? rhs  →  (lhs != null) ? lhs : rhs
            auto* lhs = ParseLogicalOrExpression(logicCtx);
            if (!lhs) return nullptr;

            auto* resultAlloca = compilerLLVM->CreateAlloca(lhs->getType());

            auto* nullBlock    = compilerLLVM->CreateBasicBlock("nullcoal_null");
            auto* notNullBlock = compilerLLVM->CreateBasicBlock("nullcoal_notnull");
            auto* resumeBlock  = compilerLLVM->CreateBasicBlock("nullcoal_resume");

            compilerLLVM->CreateConditionJump(lhs, notNullBlock, nullBlock);
            // insert point is now notNullBlock (lhs is not null)
            compilerLLVM->CreateAssignment(lhs, resultAlloca);
            compilerLLVM->CreateJump(resumeBlock);

            compilerLLVM->SwitchToBlock(nullBlock);
            auto* rhs = ParseConditionalExpression(ctx->conditionalExpression());
            compilerLLVM->CreateAssignment(rhs, resultAlloca);
            compilerLLVM->CreateJump(resumeBlock);

            compilerLLVM->SwitchToBlock(resumeBlock);
            return compilerLLVM->CreateLoad(resultAlloca);
        }

        auto expressionFalse = ctx->expression();
        auto expressionTrue = ctx->conditionalExpression();

        if (logicCtx != nullptr)
        {
            auto expression = ParseLogicalOrExpression(logicCtx);

            // Both expression should exist or not exist.
            if ((expressionFalse != nullptr) != (expressionTrue != nullptr))
            {
                LogErrorContext(ctx, "Conditional expression requires both true and false branches.");
                return nullptr;
            }
            else if (expressionFalse != nullptr && (expressionTrue != nullptr))
            {
                auto falseValue = ParseExpression(expressionFalse);
                auto trueValue = ParseConditionalExpression(expressionTrue);

                auto selectValue = compilerLLVM->CreateSelect(expression, falseValue, trueValue);
                return selectValue;
            }

            return expression;
        }

        LogErrorContext(ctx, "Conditional expression has no logical-or sub-expression.");
        return nullptr;
    }

    llvm::Value* ParseLogicalOrExpression(CParser::LogicalOrExpressionContext* ctx)
    {
        auto logicCtxs = ctx->logicalAndExpression();

        if (logicCtxs.size() == 1)
        {
            return ParseLogicalAndExpression(logicCtxs[0]);
        }
        else if (logicCtxs.size() > 1)
        {
            llvm::Value* left = nullptr;
            auto elseBlock = compilerLLVM->GetElseBlock();

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
                        auto falseBlock = compilerLLVM->CreateBasicBlock("falseOR");
                        auto branch = compilerLLVM->CreateConditionJump(left, elseBlock, falseBlock);

                        compilerLLVM->InitializeBlock(falseBlock, false);
                        llvm::Value* right = ParseLogicalAndExpression(logicCtx);
                        left = compilerLLVM->CreateOperation(MyCompilerLLVM::Operation::LogicalOr, left, right);
                    }
                }
            }
            else
            {
                MyCompilerLLVM::TypeAndValue boolValue = { .TypeName = "bool",.VariableName = "", .Pointer = false };
                auto resultStorage = compilerLLVM->CreateAlloca(compilerLLVM->GetType(boolValue));
                auto resumeBlock = compilerLLVM->CreateBasicBlock("resumeOR");

                for (const auto& logicCtx : logicCtxs)
                {
                    if (left == nullptr)
                    {
                        left = ParseLogicalAndExpression(logicCtx);
                        compilerLLVM->CreateAssignment(left, resultStorage);
                    }
                    else
                    {
                        auto falseBlock = compilerLLVM->CreateBasicBlock("falseOR");
                        auto branch = compilerLLVM->CreateConditionJump(left, resumeBlock, falseBlock);

                        compilerLLVM->InitializeBlock(falseBlock, false);
                        llvm::Value* right = ParseLogicalAndExpression(logicCtx);
                        left = compilerLLVM->CreateOperation(MyCompilerLLVM::Operation::LogicalOr, left, right);
                        compilerLLVM->CreateAssignment(left, resultStorage);
                    }
                }

                compilerLLVM->CreateBlockBreak(resumeBlock, false);

                compilerLLVM->InitializeBlock(resumeBlock, false);
                return compilerLLVM->CreateLoad(resultStorage);
            }

            return left;
        }

        LogErrorContext(ctx, "Logical-OR expression has no operands.");
        return nullptr;
    }

    llvm::Value* ParseLogicalAndExpression(CParser::LogicalAndExpressionContext* ctx)
    {
        auto inclusiveCtxs = ctx->inclusiveOrExpression();

        if (inclusiveCtxs.size() == 1)
        {
            return ParseInclusiveOrExpression(inclusiveCtxs[0]);
        }
        else if (inclusiveCtxs.size() > 1)
        {
            llvm::Value* left = nullptr;
            auto elseBlock = compilerLLVM->GetElseBlock();

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
                        auto trueBlock = compilerLLVM->CreateBasicBlock("trueAND");
                        auto branch = compilerLLVM->CreateConditionJump(left, trueBlock, compilerLLVM->GetElseBlock());

                        compilerLLVM->InitializeBlock(trueBlock, false);
                        llvm::Value* right = ParseInclusiveOrExpression(inclusiveCtx);
                        left = compilerLLVM->CreateOperation(MyCompilerLLVM::Operation::LogicalAnd, left, right);
                    }
                }
            }
            else
            {
                MyCompilerLLVM::TypeAndValue boolValue = { .TypeName = "bool",.VariableName = "", .Pointer = false };
                auto resultStorage = compilerLLVM->CreateAlloca(compilerLLVM->GetType(boolValue));
                auto resumeBlock = compilerLLVM->CreateBasicBlock("resumeAND");

                for (const auto& inclusiveCtx : inclusiveCtxs)
                {
                    if (left == nullptr)
                    {
                        left = ParseInclusiveOrExpression(inclusiveCtx);
                        compilerLLVM->CreateAssignment(left, resultStorage);
                    }
                    else
                    {
                        auto trueBlock = compilerLLVM->CreateBasicBlock("trueAND");
                        auto branch = compilerLLVM->CreateConditionJump(left, trueBlock, resumeBlock);

                        compilerLLVM->InitializeBlock(trueBlock, false);
                        llvm::Value* right = ParseInclusiveOrExpression(inclusiveCtx);
                        left = compilerLLVM->CreateOperation(MyCompilerLLVM::Operation::LogicalAnd, left, right);
                        compilerLLVM->CreateAssignment(left, resultStorage);
                    }
                }

                compilerLLVM->CreateBlockBreak(resumeBlock, false);

                compilerLLVM->InitializeBlock(resumeBlock, false);
                return compilerLLVM->CreateLoad(resultStorage);
            }
            return left;
        }

        LogErrorContext(ctx, "Logical-AND expression has no operands.");
        return nullptr;
    }

    llvm::Value* ParseInclusiveOrExpression(CParser::InclusiveOrExpressionContext* ctx)
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

    llvm::Value* ParseExclusiveOrExpression(CParser::ExclusiveOrExpressionContext* ctx)
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

    llvm::Value* ParseAndExpression(CParser::AndExpressionContext* ctx)
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

    llvm::Value* ParseEqualityExpression(CParser::EqualityExpressionContext* ctx)
    {
        auto nextCtxs = ctx->relationalExpression();
        if (nextCtxs.size() == 1)
        {
            return ParseRelationalExpression(nextCtxs[0]);
        }
        else if (nextCtxs.size() == 2)
        {
            auto left = ParseRelationalExpression(nextCtxs[0]);
            auto right = ParseRelationalExpression(nextCtxs[1]);

            return compilerLLVM->CreateOperation(ctx->children[1]->getText(), left, right);
        }

        LogErrorContext(ctx, "Equality expression has unexpected operand count.");
        return nullptr;
    }

    llvm::Value* ParseRelationalExpression(CParser::RelationalExpressionContext* ctx)
    {
        auto nextCtxs = ctx->shiftExpression();
        if (nextCtxs.size() == 1)
        {
            return ParseShiftExpression(nextCtxs[0]);
        }
        else if (nextCtxs.size() == 2)
        {
            auto left = ParseShiftExpression(nextCtxs[0]);
            auto right = ParseShiftExpression(nextCtxs[1]);

            return compilerLLVM->CreateOperation(ctx->children[1]->getText(), left, right);
        }

        LogErrorContext(ctx, "Relational expression has unexpected operand count.");
        return nullptr;
    }

    llvm::Value* ParseShiftExpression(CParser::ShiftExpressionContext* ctx)
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

    llvm::Value* ParseAdditiveExpression(CParser::AdditiveExpressionContext* ctx)
    {
        auto nextCtxs = ctx->multiplicativeExpression();

        llvm::Value* lvalue = nullptr;
        llvm::Value* rvalue = nullptr;

        if (nextCtxs.size() == 1)
        {
            return ParseMultiplicativeExpression(nextCtxs[0]);
        }
        else if (nextCtxs.size() > 1)
        {
            int count = 0;
            for (const auto& nextCtx : nextCtxs)
            {
                rvalue = ParseMultiplicativeExpression(nextCtx);
                lvalue = compilerLLVM->CreateOperation(ctx->children[count * 2 + 1]->getText(), lvalue, rvalue);
            }

            return lvalue;
        }

        LogErrorContext(ctx, "Additive expression has no operands.");
        return nullptr;
    }

    llvm::Value* LoadNamedVariable(MyCompilerLLVM::NamedVariable& namedVar)
    {
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
                    return compilerLLVM->CreateLoad(namedVar.Storage);
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
                return compilerLLVM->CreateLoad(namedVar.Storage);
            }
        }

        // namedVar is empty — caller's block was already terminated (e.g. by a return-block inline).
        return nullptr;
    }

    llvm::Value* ParseMultiplicativeExpression(CParser::MultiplicativeExpressionContext* ctx)
    {
        auto nextCtxs = ctx->castExpression();

        if (nextCtxs.size() == 1)
        {
            auto namedVar = ParseCastExpression(nextCtxs[0]);
            return LoadNamedVariable(namedVar);;
        }
        else if (nextCtxs.size() > 1)
        {
            llvm::Value* lvalue = nullptr;
            llvm::Value* rvalue = nullptr;

            size_t count = 0;
            for (const auto& nextCtx : nextCtxs)
            {
                auto namedVar = ParseCastExpression(nextCtx);
                rvalue = LoadNamedVariable(namedVar);
                lvalue = compilerLLVM->CreateOperation(ctx->children[count * 2 + 1]->getText(), lvalue, rvalue);
            }

            return lvalue;
        }

        LogErrorContext(ctx, "Multiplicative expression has no operands.");
        return nullptr;
    }

    MyCompilerLLVM::NamedVariable ParseCastExpression(CParser::CastExpressionContext* ctx, bool lvalue = false)
    {
        /*
        castExpression : (int*)&num_f
        typeName : int*
        specifierQualifierList : int
        typeSpecifier : int
        abstractDeclarator : *
        pointer : *
        castExpression : &num_f
        unaryExpression : &num_f
        unaryOperator : &
        castExpression : num_f
        unaryExpression : num_f
        postfixExpression : num_f
        primaryExpression : num_f
        */

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
            auto type = compilerLLVM->GetType(destTypeName);

            if (namedVar.Storage)
            {
                // If storage is available, then load it with new type.
                namedVar.Primary = compilerLLVM->CreateLoad(type, namedVar.Storage);
            }
            else
            {
                // Otherwise cast it.
                namedVar.Primary = compilerLLVM->CreateCast(namedVar.Primary, type);
            }

            return namedVar;
        }

        LogErrorContext(ctx, "Cast expression has no recognized form.");
        return {};
    }

    MyCompilerLLVM::TypeAndValue ParseTypeName(CParser::TypeNameContext* ctx)
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

    MyCompilerLLVM::NamedVariable ParseUnaryExpression(CParser::UnaryExpressionContext* ctx)
    {
        auto postFixCtx = ctx->postfixExpression();
        auto castExpCtx = ctx->castExpression();
        auto unaryOperator = ctx->unaryOperator();

        if (postFixCtx != nullptr)
        {
            return ParsePostfixExpression(postFixCtx);
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

                namedVar.Primary = compilerLLVM->CreateLoad(namedVar.Storage);
            }
            else if (opText == "!")
            {
                auto newValue = this->LoadNamedVariable(namedVar);
                namedVar.Primary = compilerLLVM->CreateNot(newValue);
                namedVar.Storage = nullptr;
            }
            else if (opText == "-")
            {
                auto newValue = this->LoadNamedVariable(namedVar);
                namedVar.Primary = compilerLLVM->CreateNeg(newValue);
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
                namedVar.Primary = compilerLLVM->CreateNot(newValue);
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

    MyCompilerLLVM::NamedVariable ParsePostfixExpression(CParser::PostfixExpressionContext* ctx, bool lValue = false)
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
                    case CParser::LeftBracket:
                    case CParser::RightBracket:
                    case CParser::LeftParen:
                    case CParser::RightParen: { prevToken = tokenType; break; }
                    case CParser::Dot:
                    case CParser::Arrow: { prevToken = tokenType; nullConditionalPending = false; break; }
                    case CParser::QuestionDot: { prevToken = tokenType; nullConditionalPending = true; break; }
                    case CParser::PlusPlus: { if (namedVar.Storage) { PlusPlus[namedVar.Storage]++; } break; }
                    case CParser::MinusMinus: { if (namedVar.Storage) { PlusPlus[namedVar.Storage]--; } break; }
                    case CParser::Identifier:
                    {
                        if (!namespaceContext.empty())
                        {
                            std::string qualifiedName = namespaceContext + "." + terminal->getText();
                            if (compilerLLVM->IsNamespace(qualifiedName))
                            {
                                namespaceContext = compilerLLVM->ResolveNamespace(qualifiedName);
                                primaryIdentifier = namespaceContext;
                                namedVar = {};
                            }
                            else
                            {
                                primaryIdentifier = qualifiedName;
                                namespaceContext.clear();
                                namedVar = {};
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
                            auto dataStructure = compilerLLVM->GetDataStructure(llvm::dyn_cast<llvm::StructType>(structVar.BaseType));
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
                                    auto* fieldLLVMType  = compilerLLVM->GetType(fieldType);
                                    auto* resultAlloca   = compilerLLVM->CreateAlloca(fieldLLVMType);

                                    auto* nullBlock   = compilerLLVM->CreateBasicBlock("nc_null");
                                    auto* accessBlock = compilerLLVM->CreateBasicBlock("nc_access");
                                    auto* resumeBlock = compilerLLVM->CreateBasicBlock("nc_resume");

                                    compilerLLVM->CreateConditionJump(structVar.Storage, accessBlock, nullBlock);
                                    // insert point is now accessBlock

                                    auto* fieldGEP = compilerLLVM->CreateStructGEP(structVar.BaseType, structVar.Storage, fieldIndex);
                                    auto* fieldVal = compilerLLVM->CreateLoad(fieldGEP);
                                    compilerLLVM->CreateAssignment(fieldVal, resultAlloca);
                                    compilerLLVM->CreateJump(resumeBlock);

                                    compilerLLVM->SwitchToBlock(nullBlock);
                                    compilerLLVM->CreateAssignment(llvm::Constant::getNullValue(fieldLLVMType), resultAlloca);
                                    compilerLLVM->CreateJump(resumeBlock);

                                    compilerLLVM->SwitchToBlock(resumeBlock);
                                    auto* result = compilerLLVM->CreateLoad(resultAlloca);

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
                                        namedVar.Storage = compilerLLVM->CreateStructGEP(structVar.BaseType, structVar.Storage, fieldIndex);
                                        namedVar.Primary = compilerLLVM->CreateLoad(namedVar.Storage);
                                        namedVar.BaseType = namedVar.Primary->getType();
                                    }
                                    else if (structVar.Primary)
                                    {
                                        namedVar.Storage = nullptr;
                                        namedVar.Primary = compilerLLVM->CreateExtractValue(structVar.Primary, fieldIndex);
                                        namedVar.BaseType = namedVar.Primary->getType();
                                    }
                                    namedVar.TypeAndValue = fieldType;
                                }
                            }
                            else if (auto func = compilerLLVM->GetFunction(primaryIdentifier))
                            {
                                // Not a field, then it might be a function.
                                namedVar = {};
                            }
                            else
                            {
                                LogErrorContext(ctx, std::format("Unknown identifier '{}'.", primaryIdentifier));
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
                    case CParser::RulePrimaryExpression:
                    {
                        auto prevPrimary = dynamic_cast<CParser::PrimaryExpressionContext*>(parseTree);

                        primaryIdentifier = prevPrimary->getText();

                        if (prevPrimary->Identifier() && compilerLLVM->IsNamespace(prevPrimary->Identifier()->getText()))
                        {
                            namespaceContext = compilerLLVM->ResolveNamespace(prevPrimary->Identifier()->getText());
                            namedVar = {};
                            structVar = {};
                        }
                        else
                        {
                            namedVar.Primary = ParsePrimaryExpression(prevPrimary);
                            namedVar.Storage = nullptr;

                            if (namedVar.Primary == nullptr)
                            {
                                // Try identifier.
                                namedVar = ParseIdentifier(prevPrimary->Identifier());
                            }
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
                        else if (!namedVar.Storage)
                        {
                            structVar = {};
                            interfaceVar = {};
                        }

                        break;
                    }
                    case CParser::RuleExpression:
                    {
                        // Bracket [] operation

                        auto expressCtx = dynamic_cast<CParser::ExpressionContext*>(ruleContext);
                        auto rvalue = ParseExpression(expressCtx);

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
                            auto elementType = compilerLLVM->GetType(elementTypeAndValue);
                            auto ptrValue = LoadNamedVariable(namedVar);
                            namedVar.Storage = compilerLLVM->CreateGEP(elementType, ptrValue, rvalue);
                            namedVar.BaseType = elementType;
                            namedVar.TypeAndValue.Pointer = false;
                        }
                        else
                        {
                            namedVar.Storage = compilerLLVM->CreateGEP(namedVar.BaseType, namedVar.Storage, rvalue);
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
                    case CParser::RuleArgumentExpressionList:
                    {
                        // Create Function Call
                        std::string functionName = primaryIdentifier;

                        auto argumentList = ctx->argumentExpressionList();

                        // Check if this is a return-block function — inline it at the call site.
                        // A 'return' inside the block returns from the caller function.
                        if (const auto* rb = compilerLLVM->GetReturnBlock(functionName))
                        {
                            compilerLLVM->InitializeBlock(nullptr, true);

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
                                    thisVar = compilerLLVM->GetCurrentMemberThis(functionName);
                                }
                                if (thisVar.Storage != nullptr)
                                {
                                    const auto& thisParam = rb->Params[0];
                                    MyCompilerLLVM::TypeAndValue tv;
                                    tv.TypeName = thisParam.TypeName;
                                    tv.VariableName = thisParam.VariableName;
                                    tv.Pointer = thisParam.Pointer;
                                    auto* alloca = compilerLLVM->CreateLocalVariable(tv);
                                    compilerLLVM->CreateAssignment(thisVar.Storage, alloca);
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
                                        compilerLLVM->RegisterPrimaryVariable(tv, argValue);
                                    }
                                    else
                                    {
                                        auto* alloca = compilerLLVM->CreateLocalVariable(tv);
                                        compilerLLVM->CreateAssignment(argValue, alloca);
                                    }
                                }
                            }

                            if (auto* blockItems = rb->Body->blockItemList())
                                ParseBlockItemList(blockItems);

                            compilerLLVM->CreateBlockBreak(nullptr, true);

                            // The block's 'return' already terminated the caller's basic block.
                            // Return an empty namedVar — callers must tolerate null after a terminator.
                            namedVar = {};
                        }
                        else if (interfaceVar.TypeAndValue.IsInterface)
                        {
                            // Interface method dispatch via vtable
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
                                    extraArgs.emplace_back(argVar);
                                }
                            }

                            namedVar.Primary = compilerLLVM->CallInterfaceMethod(
                                interfaceVar.Storage,
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
                                auto thisVar = compilerLLVM->GetCurrentMemberThis(functionName);
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

                                    arguments.emplace_back(argVar);
                                }
                            }

                            if (nullConditionalPending && structVar.Storage != nullptr)
                            {
                                // Null-conditional method call: gate the call on a null check
                                auto* retType    = compilerLLVM->GetFunctionReturnType(functionName);
                                bool  hasResult  = retType && !retType->isVoidTy();
                                auto* resultAlloca = hasResult ? compilerLLVM->CreateAlloca(retType) : nullptr;

                                auto* nullBlock   = compilerLLVM->CreateBasicBlock("nc_null");
                                auto* accessBlock = compilerLLVM->CreateBasicBlock("nc_access");
                                auto* resumeBlock = compilerLLVM->CreateBasicBlock("nc_resume");

                                compilerLLVM->CreateConditionJump(structVar.Storage, accessBlock, nullBlock);
                                // insert point is now accessBlock

                                namedVar.Primary = compilerLLVM->CreateFunctionCall2(functionName, arguments);
                                namedVar.Storage = nullptr;
                                namedVar.BaseType = namedVar.Primary ? namedVar.Primary->getType() : nullptr;

                                if (hasResult && namedVar.Primary)
                                {
                                    compilerLLVM->CreateAssignment(namedVar.Primary, resultAlloca);
                                    compilerLLVM->CreateJump(resumeBlock);

                                    compilerLLVM->SwitchToBlock(nullBlock);
                                    compilerLLVM->CreateAssignment(llvm::Constant::getNullValue(retType), resultAlloca);
                                    compilerLLVM->CreateJump(resumeBlock);

                                    compilerLLVM->SwitchToBlock(resumeBlock);
                                    auto* result = compilerLLVM->CreateLoad(resultAlloca);
                                    namedVar.Primary  = result;
                                    namedVar.BaseType = result->getType();
                                }
                                else
                                {
                                    compilerLLVM->CreateJump(resumeBlock);
                                    compilerLLVM->SwitchToBlock(nullBlock);
                                    compilerLLVM->CreateJump(resumeBlock);
                                    compilerLLVM->SwitchToBlock(resumeBlock);
                                    namedVar = {};
                                }

                                nullConditionalPending = false;
                            }
                            else
                            {
                                namedVar.Primary = compilerLLVM->CreateFunctionCall2(functionName, arguments);
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
    CParser::UnaryExpressionContext* tryGetUnaryExpression(antlr4::RuleContext* ctx)
    {
        if (ctx->getRuleIndex() == CParser::RuleUnaryExpression)
            return dynamic_cast<CParser::UnaryExpressionContext*>(ctx);

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

    llvm::Value* ParsePrimaryExpression(CParser::PrimaryExpressionContext* ctx)
    {
        auto expressionCtx = ctx->expression();
        auto constant = ctx->Constant();
        auto stringLiteral = ctx->StringLiteral();

        if (ctx->TypeOf())
        {
            // typeof(int), typeof(bool), typeof(MyStruct) — type specifier used directly
            if (auto* ts = ctx->typeSpecifier())
                return compilerLLVM->CreateGlobalString("typeof", ts->getText());

            // typeof(expr) — navigate down to unaryExpression to read TypeAndValue
            std::string typeName;

            // ANTLR picks the expression alternative for user-defined type names (Identifier
            // matches both expression and typeSpecifier); catch them here before evaluating.
            if (compilerLLVM->GetDataStructure(expressionCtx->getText()).StructType != nullptr)
                return compilerLLVM->CreateGlobalString("typeof", expressionCtx->getText());

            if (auto* ue = tryGetUnaryExpression(expressionCtx))
            {
                auto namedVar = ParseUnaryExpression(ue);
                typeName = namedVar.TypeAndValue.TypeName;
                if (namedVar.TypeAndValue.Pointer && !typeName.empty())
                    typeName += "*";
            }

            return compilerLLVM->CreateGlobalString("typeof", typeName.empty() ? "unknown" : typeName);
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
            return compilerLLVM->CreateGlobalString("nameof", name);
        }
        else if (expressionCtx != nullptr)
        {
            return ParseExpression(expressionCtx);
        }
        else if (stringLiteral.size() > 0)
        {
            // TODO handle encoding u8,u,U,L
            std::string rawText = ctx->getText();
            rawText = ProcessRawText(rawText);
            return compilerLLVM->CreateGlobalString("", rawText);
        }
        else if (constant)
        {
            std::string constantText = constant->getText();
            if (constantText == "true")
            {
                return compilerLLVM->CreateConstant("bool", constantText);
            }
            else if (constantText == "false")
            {
                return compilerLLVM->CreateConstant("bool", constantText);
            }
            else if (constantText == "nullptr")
            {
                return compilerLLVM->CreateConstant("nullptr", constantText);
            }
            else if (constantText.front() == '\'' ||
                     (constantText.size() > 1 &&
                      (constantText[0] == 'L' || constantText[0] == 'u' || constantText[0] == 'U') &&
                      constantText[1] == '\''))
            {
                char c = ParseCharLiteral(constantText);
                return compilerLLVM->CreateConstant(MyCompilerLLVM::ConstantVariant(c));
            }
            else
            {
                std::string constantRaw = constant->getText();
                auto number = ParseNumberConstant(constantRaw);
                auto value = compilerLLVM->CreateConstant(number);
                return value;
            }
        }

        return nullptr;
    }

    MyCompilerLLVM::NamedVariable ParseIdentifier(antlr4::tree::TerminalNode* node)
    {
        if (!node)
            return {};

        std::string name = node->getText();
        MyCompilerLLVM::NamedVariable namedVar = {};

        if (name == "__FILE__")
        {
            auto str = compilerLLVM->CreateGlobalString("__FILE__", compilerLLVM->GetSourceFileName());
            namedVar.Primary = str;
            namedVar.BaseType = str->getType();
            return namedVar;
        }

        if (name == "__FUNCTION__")
        {
            auto str = compilerLLVM->CreateGlobalString("__FUNCTION__", compilerLLVM->GetCurrentFunctionName());
            namedVar.Primary = str;
            namedVar.BaseType = str->getType();
            return namedVar;
        }

        if (name == "__LINE__")
        {
            int line = (int)node->getSymbol()->getLine();
            auto val = compilerLLVM->CreateConstant("int", std::to_string(line));
            namedVar.Primary = val;
            namedVar.BaseType = val->getType();
            return namedVar;
        }

        namedVar = compilerLLVM->GetLocalVariable(name);
        if (namedVar.Storage != nullptr || namedVar.Primary != nullptr)
        {
            return namedVar;
        }

        auto funcArgument = compilerLLVM->GetFunctionArgument(name);
        if (funcArgument.GetValue() != nullptr)
        {
            return funcArgument;
        }

        auto memberVar = compilerLLVM->GetMemberVariable(name);
        if (memberVar.Storage != nullptr)
        {
            return memberVar;
        }

        // try getting global variable
        if (auto gVar = compilerLLVM->GetGlobalVariable(name))
        {
            namedVar.Storage = gVar;
            namedVar.BaseType = gVar->getType();
            return namedVar;
        }

        if (auto func = compilerLLVM->GetFunction(name))
        {
            namedVar.Primary = func;
            return namedVar;
        }

        // Return-block functions have no IR entry; they are inlined at the call site.
        if (compilerLLVM->GetReturnBlock(name) != nullptr)
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

    llvm::Value* ParseExpression(CParser::ExpressionContext* ctx)
    {
        auto assignCtxs = ctx->assignmentExpression();
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

                compilerLLVM->CreateIncrement(destination, amount);
            }

            PlusPlus.clear();
        }
    }

    void ParseStructClassUnionDefinition(CParser::StructClassUnionDefinitionContext* ctx)
    {
        auto decl = ctx->directDeclarator();
        std::string structName = decl->getText();
        auto declarationList = ctx->declaration();
        std::vector<llvm::Type*> types;

        auto declList = ParseDeclarationList(declarationList);

        // Create a opaqueStruct to initialize default constructor.
        auto structType = compilerLLVM->CreateStructType(structName, {});
        MyCompilerLLVM::TypeAndValue returnType{
            .TypeName = structName,
        };
        // Create default constructor
        {
            auto funcDef = compilerLLVM->CreateFunctionDefinition(structName, returnType, {});

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
                        }
                    }
                }
                else
                {
                    std::cout << "Uninitialize field \"" << structName << "::" << typeValue.VariableName << "\".\n";
                }

                initilizers.push_back(rvalue);
            }

            structType = compilerLLVM->CreateStructType(structName, declList);
            llvm::Value* structVal = llvm::UndefValue::get(structType);

            MyCompilerLLVM::TypeAndValue myStruct;
            myStruct.TypeName = structName;
            myStruct.VariableName = "_" + structName;

            unsigned int structIndex = 0;

            for (auto rvalue : initilizers)
            {
                if (rvalue != nullptr)
                {
                    rvalue = compilerLLVM->Upconvert(rvalue, structType->getTypeAtIndex(structIndex));
                    structVal = compilerLLVM->CreateInsertValue(structVal, rvalue, structIndex);
                }

                structIndex++;
            }

            // close constructor.
            compilerLLVM->CreateReturnCall(structVal);
            // Pop the stack
            compilerLLVM->CreateBlockBreak(nullptr, true);
        }

        // Parse member functions
        auto functionList = ctx->functionDefinition();

        for (auto func : functionList)
        {
            global_scope = false;
            ParseFunctionDefinition(func, structName);
            global_scope = true;
        }

        // Parse destructor
        for (auto dtor : ctx->destructorDefinition())
        {
            global_scope = false;
            ParseDestructorDefinition(dtor, structName);
            global_scope = true;
        }

        // Record interfaces and verify implementations
        std::vector<std::string> ifaceNames;
        for (auto interfaceIdentifier : ctx->Identifier())
            ifaceNames.push_back(interfaceIdentifier->getText());
        compilerLLVM->RegisterStructInterfaces(structName, ifaceNames);
        for (const auto& interfaceName : ifaceNames)
            compilerLLVM->VerifyInterfaceImplementation(structName, interfaceName);
    }

    void ParseDestructorDefinition(CParser::DestructorDefinitionContext* ctx, const std::string& structName)
    {
        MyCompilerLLVM::DeclTypeAndValue thisParam;
        thisParam.TypeName = structName;
        thisParam.VariableName = structName + "__";
        thisParam.Pointer = true;

        std::vector<MyCompilerLLVM::TypeAndValue> params = { thisParam };

        MyCompilerLLVM::TypeAndValue returnType;
        returnType.TypeName = "void";

        int line = ctx->getStart()->getLine();
        auto fn = compilerLLVM->CreateFunctionDefinition("~" + structName, returnType, params, false, false, line);
        compilerLLVM->RegisterDestructor(structName, fn);

        compilerLLVM->InitializeBlock(&fn->front(), false);

        auto blockItemList = ctx->compoundStatement()->blockItemList();
        if (blockItemList)
            ParseBlockItemList(blockItemList);

        compilerLLVM->CreateReturnCall(nullptr);
        compilerLLVM->CreateBlockBreak(nullptr, true);
        compilerLLVM->ClearCurrentSubprogram();
    }

    std::vector<MyCompilerLLVM::DeclTypeAndValue> ParseParameterTypeList(CParser::ParameterTypeListContext* paramTypeList)
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
        char lastChar = std::tolower(rawNumber.back());

        if (lastChar == 'f')
        {
            // Parse as float, excluding the 'f' suffix
            return std::stof(rawNumber.substr(0, rawNumber.size() - 1));
        }
        else if (lastChar == 'd' || lastChar == '.' || rawNumber.find('.') != std::string::npos)
        {
            // Parse as double, keeping the 'd' if present or removing nothing
            size_t end = (lastChar == 'd') ? rawNumber.size() - 1 : rawNumber.size();
            return std::stod(rawNumber.substr(0, end));
        }
        else if (lastChar == 'l')
        {
            size_t end = rawNumber.size() - 1;

            // capture ll
            if (rawNumber[end] == 'l' || rawNumber[end] == 'L')
                end--;

            return std::stoll(rawNumber.substr(0, end));
        }
        else
        {
            auto number = std::stoi(rawNumber);
            if (number < std::numeric_limits<int16_t>::max())
            {
                short value = number;
                return value;
            }
            else if (number < std::numeric_limits<int8_t>::max())
            {
                char value = number;
                return value;
            }

            return number;
        }

        return 0;
    }

    void LogErrorContext(antlr4::tree::TerminalNode* ctx, std::string errorMessage)
    {
        auto symbol = ctx->getSymbol();
        int line = symbol->getLine();
        int column = symbol->getCharPositionInLine();
        std::cout << std::format("[{}:{}] {} : {}\n", line, column, ctx->getText(), errorMessage);
        exit(1);
    }

    void LogErrorContext(antlr4::ParserRuleContext* ctx, std::string errorMessage)
    {
        int line = ctx->getStart()->getLine();
        int column = ctx->getStart()->getCharPositionInLine();
        std::cout << std::format("[{}:{}] {} : {}\n", line, column, ctx->getText(), errorMessage);
        exit(1);
    }

    void PrintContext(antlr4::ParserRuleContext* ctx, std::string suffix = "")
    {
        int line = ctx->getStart()->getLine();
        int column = ctx->getStart()->getCharPositionInLine();
        std::cout << std::format("[{}:{}] {} : {} : {}\n", line, column, parser->getRuleNames()[ctx->getRuleIndex()], ctx->getText(), suffix);
    }

    void enterEveryRule(antlr4::ParserRuleContext* ctx) override
    {
        if constexpr (debugPrint)
            PrintContext(ctx);
        compilerLLVM->SetSourceLocation(
            ctx->getStart()->getLine(),
            ctx->getStart()->getCharPositionInLine(),
            ctx->getText());
    }
};
