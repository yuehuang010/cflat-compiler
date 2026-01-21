#pragma once
#include <iostream>
#include <unordered_map>

#include "CParser.h"
#include "CLexer.h"
#include "CBaseListener.h"
#include "MyCompilerLLVM.h"


class MyListener : public CBaseListener
{
private:
	std::vector<std::string> declaration;
	CParser* parser;
	MyCompilerLLVM* compilerLLVM;
	bool global_scope = true;

	struct Type
	{
		std::string Name;
		bool pointer : 1 = false;
	};

	bool isFunctionOrNamespace(antlr4::ParserRuleContext* ctx)
	{
		if (ctx->getRuleIndex() == CParser::RuleFunctionDefinition)
			return true;
		return false;
	}

	bool isStructClassUnion(antlr4::ParserRuleContext* ctx)
	{
		if (ctx->getRuleIndex() == CParser::RuleStructClassUnionDefinition)
			return true;
		return false;
	}

	std::string getFunctionName(CParser::FunctionDefinitionContext* ctx)
	{
		std::string name;
		auto directDecl = ctx->directDeclarator();
		name = directDecl->getText();

		return name;
	}

	std::string getDeclaratorName(CParser::DirectDeclaratorContext* directDecl)
	{
		return directDecl->getText();
	}

	Type getTypeSpecifier(CParser::DeclarationSpecifiersContext* declSpecs)
	{
		Type declType;
		std::string typeName;
		auto declSpecList = declSpecs->declarationSpecifier();

		for (auto declSpec : declSpecList)
		{
			auto typeSpec = declSpec->typeSpecifier();
			if (typeSpec != nullptr)
			{
				declType.Name = typeSpec->getText();
				declType.pointer = declSpec->pointer() != nullptr;
				break;
			}
		}

		return declType;
	}

	std::string getFunctionReturnType(CParser::FunctionDefinitionContext* ctx)
	{
		auto declSpecs = ctx->declarationSpecifiers();

		return getTypeSpecifier(declSpecs).Name;
	}

	std::string getStructClassUnionName(CParser::StructClassUnionDefinitionContext* ctx)
	{
		std::string name;
		auto directDecl1 = ctx->directDeclarator();
		auto directDecl2 = ctx->directDeclarator();

		if (directDecl2 != nullptr)
			name = directDecl2->getText();
		else
			name = directDecl1->getText();

		return name;
	}

	std::string getScopeName(antlr4::ParserRuleContext* ctx)
	{
		std::string name;

		while (ctx)
		{
			if (isFunctionOrNamespace(ctx))
			{
				auto funcName = getFunctionName((CParser::FunctionDefinitionContext*)ctx);
				name += "::";
				name += funcName;
			}
			else if (isStructClassUnion(ctx))
			{
				auto funcName = getStructClassUnionName((CParser::StructClassUnionDefinitionContext*)ctx);
				name += "::";
				name += funcName;
			}

			if (ctx->parent != nullptr)
				ctx = (antlr4::ParserRuleContext*)ctx->parent;
			else
				break;
		}

		//if (!name.empty())
		//	std::cout << "Function: " << name << "\n";

		return name;
	}

public:
	MyListener(CParser* parser, MyCompilerLLVM* compilerLLVM)
	{
		this->parser = parser;
		this->compilerLLVM = compilerLLVM;
	}

	void enterExternalDeclaration(CParser::ExternalDeclarationContext* ctx) override
	{
		auto func = ctx->functionDefinition();
		auto dataStruct = ctx->structClassUnionDefinition();
		auto decl = ctx->declaration();

		if (decl != nullptr)
		{
			auto globalDecl = ParseDeclaration(decl);
		}
		else if (func != nullptr)
		{
			// Create Function Definition
			auto name = this->getFunctionName(func);
			auto returnType = this->getFunctionReturnType(func);
			CParser::ParameterTypeListContext* paramTypeList = func->parameterTypeList();
			auto params = this->ParseParamaterTypeList(paramTypeList);
			auto ft = GetFunctionType(returnType, params, paramTypeList && paramTypeList->Ellipsis() != nullptr);
			auto fn = this->compilerLLVM->CreateFunctionDefinition(name, ft);

			auto blockItems = func->compoundStatement()->blockItemList()->blockItem();

			global_scope = false;

			for (const auto& blockItem : blockItems)
			{
				auto decl = blockItem->declaration();
				auto statement = blockItem->statement();

				if (decl != nullptr)
				{
					this->ParseDeclaration(decl);
				}
				else if (statement != nullptr)
				{
					std::cout << statement->getText() << std::endl;
					auto jump = statement->jumpStatement();
					auto expressStatement = statement->expressionStatement();
					if (jump != nullptr)
					{
						if (jump->Return())
						{
							auto express = jump->expression();
							auto right = ParseExpression(express);
							this->compilerLLVM->CreateReturnCall(right);
						}
					}
					else if (expressStatement != nullptr)
					{
						auto express = expressStatement->expression();
						if (express != nullptr)
						{
							ParseExpression(express);
						}
					}
				}
			}

			global_scope = true;
		}
	}

	std::vector<std::pair<std::string, llvm::AllocaInst*>> ParseDeclaration(CParser::DeclarationContext* ctx)
	{
		std::vector<std::pair<std::string, llvm::AllocaInst*>> allocList;

		auto declSpec = ctx->declarationSpecifiers();
		std::string typeName = declSpec->getText();

		auto initDecl = ctx->initDeclaratorList();
		auto initDeclarVec = initDecl->initDeclarator();

		// auto fullname = getScopeName(ctx);
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
				std::vector<Type> params = ParseParamaterTypeList(paramTypeList);

				bool ellipsis = paramTypeList->Ellipsis() != nullptr;
				auto ft = GetFunctionType(typeName, params, ellipsis);
				this->compilerLLVM->CreateFunctionDeclaration(direct->getText(), ft);
			}
			else if (direct != nullptr)
			{
				auto identList = declarator->identifierList();
				std::string name = direct->getText();
				if (identList != nullptr)
				{
					// TODO
					std::cout << "declarator identList: " << typeName << " " << name << "\n";
				}

				llvm::Value* right = nullptr;
				auto initilizer = initDecl->initializer();
				if (initilizer != nullptr)
				{
					auto assignmentExpression = initilizer->assignmentExpression();
					if (assignmentExpression != nullptr)
					{
						right = ParseAssignmentExpression(assignmentExpression);
					}
				}

				if (global_scope)
				{
					auto constant = llvm::dyn_cast_or_null<llvm::ConstantInt>(right);
					this->compilerLLVM->CreateGlobalVariable(name, typeName, constant);
				}
				else
				{
					auto alloc = this->compilerLLVM->CreateVariable(name, typeName);
					allocList.push_back(std::pair(name, alloc));

					if (right != nullptr)
					{
						this->compilerLLVM->CreateAssignment(right, alloc);
					}
				}
			}
		}

		return allocList;
	}

	MyCompilerLLVM::operation ParseOperation(std::string operationText)
	{
		if (operationText == "+") { return MyCompilerLLVM::operation::Add; }
		else if (operationText == "*") { return MyCompilerLLVM::operation::Multiply; }
		else if (operationText == "-") { return MyCompilerLLVM::operation::Subtract; }
		else if (operationText == "/") { return MyCompilerLLVM::operation::Divide; }

		__debugbreak();
		return MyCompilerLLVM::operation::None;
	}

	llvm::Value* ParseAssignmentExpression(CParser::AssignmentExpressionContext* ctx)
	{
		auto condCtx = ctx->conditionalExpression();
		auto assignmentOp = ctx->assignmentOperator();
		if (condCtx != nullptr)
		{
			return ParseConditionalExpression(condCtx);
		}
		else if (assignmentOp != nullptr)
		{
			auto unaryCtx = ctx->unaryExpression();
			auto assignCtx = ctx->assignmentExpression();

			auto destination = ParseUnaryExpression(unaryCtx, true);
			auto right = ParseAssignmentExpression(assignCtx);

			if (auto alloc = llvm::dyn_cast<llvm::AllocaInst>(destination))
			{
				return compilerLLVM->CreateAssignment(right, alloc);
			}
			else if (auto gVar = llvm::dyn_cast<llvm::GlobalVariable>(destination))
			{
				return compilerLLVM->CreateAssignment(right, gVar);
			}
		}

		__debugbreak();
		return nullptr;
	}
	llvm::Value* ParseConditionalExpression(CParser::ConditionalExpressionContext* ctx)
	{
		auto logicCtx = ctx->logicalOrExpression();
		if (logicCtx != nullptr)
		{
			return ParseLogicalOrExpression(logicCtx);
		}

		__debugbreak();
		return nullptr;
	}
	llvm::Value* ParseLogicalOrExpression(CParser::LogicalOrExpressionContext* ctx)
	{
		auto logicCtxs = ctx->logicalAndExpression();
		if (logicCtxs.size())
		{
			for (const auto& logicCtx : logicCtxs)
			{
				return ParseLogicalAndExpression(logicCtx);
			}
		}

		__debugbreak();
		return nullptr;
	}
	llvm::Value* ParseLogicalAndExpression(CParser::LogicalAndExpressionContext* ctx)
	{
		auto incluiveCtxs = ctx->inclusiveOrExpression();
		if (incluiveCtxs.size())
		{
			for (const auto& incluCtx : incluiveCtxs)
			{
				return ParseInclusiveOrExpression(incluCtx);
			}
		}

		__debugbreak();
		return nullptr;
	}
	llvm::Value* ParseInclusiveOrExpression(CParser::InclusiveOrExpressionContext* ctx)
	{
		auto excluiveCtxs = ctx->exclusiveOrExpression();
		if (excluiveCtxs.size())
		{
			for (const auto& excluCtx : excluiveCtxs)
			{
				return ParseExclusiveOrExpression(excluCtx);
			}
		}

		__debugbreak();
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

		__debugbreak();
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

		__debugbreak();
		return nullptr;
	}
	llvm::Value* ParseEqualityExpression(CParser::EqualityExpressionContext* ctx)
	{
		auto nextCtxs = ctx->relationalExpression();
		if (nextCtxs.size())
		{
			for (const auto& nextCtx : nextCtxs)
			{
				return ParseRelationalExpression(nextCtx);
			}
		}

		__debugbreak();
		return nullptr;
	}
	llvm::Value* ParseRelationalExpression(CParser::RelationalExpressionContext* ctx)
	{
		auto nextCtxs = ctx->shiftExpression();
		if (nextCtxs.size())
		{
			for (const auto& nextCtx : nextCtxs)
			{
				return ParseShiftExpression(nextCtx);
			}
		}

		__debugbreak();
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

		__debugbreak();
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
				lvalue = this->compilerLLVM->CreateOperation(ParseOperation(ctx->children[count * 2 + 1]->getText()), lvalue, rvalue);
			}

			return lvalue;
		}

		__debugbreak();
		return nullptr;
	}
	llvm::Value* ParseMultiplicativeExpression(CParser::MultiplicativeExpressionContext* ctx)
	{
		auto nextCtxs = ctx->castExpression();

		llvm::Value* lvalue = nullptr;
		llvm::Value* rvalue = nullptr;

		if (nextCtxs.size() == 1)
		{
			return ParseCastExpression(nextCtxs[0]);
		}
		else if (nextCtxs.size() > 1)
		{
			int count = 0;
			for (const auto& nextCtx : nextCtxs)
			{
				rvalue = ParseCastExpression(nextCtx);
				lvalue = this->compilerLLVM->CreateOperation(ParseOperation(ctx->children[count * 2 + 1]->getText()), lvalue, rvalue);
			}

			return lvalue;
		}

		__debugbreak();
		return nullptr;
	}
	llvm::Value* ParseCastExpression(CParser::CastExpressionContext* ctx)
	{
		auto unaryCtx = ctx->unaryExpression();
		if (unaryCtx != nullptr)
		{
			return ParseUnaryExpression(unaryCtx);
		}

		__debugbreak();
		return nullptr;
	}
	llvm::Value* ParseUnaryExpression(CParser::UnaryExpressionContext* ctx, bool lValue = false)
	{
		auto postFixCtx = ctx->postfixExpression();
		if (postFixCtx != nullptr)
		{
			return ParsePostfixExpression(postFixCtx, lValue);
		}

		__debugbreak();
		return nullptr;
	}
	llvm::Value* ParsePostfixExpression(CParser::PostfixExpressionContext* ctx, bool lValue = false)
	{
		auto primaryCtx = ctx->primaryExpression();
		if (primaryCtx != nullptr)
		{
			auto argumentList = ctx->argumentExpressionList();
			if (argumentList.size() > 0)
			{
				// Function Callsite
				std::string functoinName = primaryCtx->getText();
				auto fn = this->compilerLLVM->GetFunction(functoinName);

				std::vector<llvm::Value*> argVec;

				auto assignmentExpressionCtx = argumentList[0]->assignmentExpression();

				for (const auto& argument : assignmentExpressionCtx)
				{
					argVec.push_back(this->ParseAssignmentExpression(argument));
				}

				return this->compilerLLVM->CreateFunctionCall(fn, argVec);
			}
			else
			{
				auto primaryExpression = ParsePrimaryExpression(primaryCtx, lValue);
				return primaryExpression;
			}
		}

		__debugbreak();
		return nullptr;
	}

	/// <summary>
	/// Get Primary Expression
	/// </summary>
	/// <param name="ctx">Context</param>
	/// <param name="lValue">If true, return the storage itself.</param>
	/// <returns></returns>
	llvm::Value* ParsePrimaryExpression(CParser::PrimaryExpressionContext* ctx, bool lValue = false)
	{
		auto expressionCtx = ctx->expression();
		auto identifier = ctx->Identifier();
		auto constant = ctx->Constant();
		auto stringLiteral = ctx->StringLiteral();

		if (expressionCtx != nullptr)
		{
			return ParseExpression(expressionCtx);
		}
		else if (stringLiteral.size() > 0)
		{
			return this->compilerLLVM->CreateGlobalString("", ctx->getText());
		}
		else if (constant)
		{
			return this->compilerLLVM->CreateConstant("int", constant->getText());
		}
		else if (identifier)
		{
			std::string name = identifier->getText();

			auto alloc = this->compilerLLVM->GetLocalVariable(name);

			if (alloc == nullptr)
			{
				// try getting global variable
				auto gVar = this->compilerLLVM->GetGlobalVariable(name);
				if (gVar == nullptr)
					__debugbreak();

				if (lValue)
					return gVar;
				else
					return this->compilerLLVM->CreateLoad(gVar);
			}

			if (lValue)
				return alloc;
			else
				return this->compilerLLVM->CreateLoad(alloc);
		}

		__debugbreak();
		return nullptr;
	}

	llvm::Value* ParseExpression(CParser::ExpressionContext* ctx)
	{
		/*
		expression : printf("Hello World")
assignmentExpression : printf("Hello World")
conditionalExpression : printf("Hello World")
logicalOrExpression : printf("Hello World")
logicalAndExpression : printf("Hello World")
inclusiveOrExpression : printf("Hello World")
exclusiveOrExpression : printf("Hello World")
andExpression : printf("Hello World")
equalityExpression : printf("Hello World")
relationalExpression : printf("Hello World")
shiftExpression : printf("Hello World")
additiveExpression : printf("Hello World")
multiplicativeExpression : printf("Hello World")
castExpression : printf("Hello World")
unaryExpression : printf("Hello World")
postfixExpression : printf("Hello World")
primaryExpression : printf
*/

		auto assignCtxs = ctx->assignmentExpression();
		if (assignCtxs.size() > 0)
		{
			llvm::Value* left = nullptr;
			for (const auto& assignCtx : assignCtxs)
			{
				left = this->ParseAssignmentExpression(assignCtx);
			}

			return left;
		}

		__debugbreak();
		return nullptr;
	}

	std::vector<MyListener::Type> ParseParamaterTypeList(CParser::ParameterTypeListContext* paramTypeList)
	{
		std::vector<MyListener::Type> params;

		if (paramTypeList == nullptr)
			return params;

		auto paramList = paramTypeList->parameterList();
		auto paramDeclList = paramList->parameterDeclaration();

		for (auto paramDecl : paramDeclList)
		{
			params.push_back(this->getTypeSpecifier(paramDecl->declarationSpecifiers()));
		}

		return params;
	}

	llvm::FunctionType* GetFunctionType(std::string returnType, std::vector<MyListener::Type> arguments, bool varargs = false)
	{
		std::vector<llvm::Type*> types;
		types.reserve(arguments.size());

		for (const MyListener::Type& arg : arguments)
		{
			types.push_back(this->compilerLLVM->GetType(arg.Name, arg.pointer));
		}

		return llvm::FunctionType::get(this->compilerLLVM->GetType(returnType), types, varargs);
	}

	void enterEveryRule(antlr4::ParserRuleContext* ctx) override
	{
		std::cout << parser->getRuleNames()[ctx->getRuleIndex()] << " : " << ctx->getText() << "\n";
	}
};
