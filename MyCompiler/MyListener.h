#pragma once
#include <iostream>
#include <unordered_map>
#include <format>
#include <variant>

#include "CParser.h"
#include "CLexer.h"
#include "CBaseListener.h"
#include "MyCompilerLLVM.h"


class MyListener : public CBaseListener
{
private:
	class DecrementTracker
	{
	public:
		llvm::AllocaInst* storage;
		int change = 0;
	};

	CParser* parser;
	MyCompilerLLVM* compilerLLVM;
	std::unordered_map<llvm::Value*, int> PlusPlus;
	bool global_scope = true;

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

	MyCompilerLLVM::TypeAndValue ParseDeclarationSpecifiers(CParser::DeclarationSpecifiersContext* declSpecs)
	{
		MyCompilerLLVM::TypeAndValue declType;
		std::string typeName;
		auto declSpecList = declSpecs->declarationSpecifier();

		for (auto declSpec : declSpecList)
		{
			auto typeSpec = declSpec->typeSpecifier();
			if (typeSpec != nullptr)
			{
				declType.TypeName = typeSpec->getText();
				declType.pointer = declSpec->pointer() != nullptr;
				break;
			}
		}

		return declType;
	}

	MyCompilerLLVM::TypeAndValue getFunctionReturnType(CParser::FunctionDefinitionContext* ctx)
	{
		auto declSpecs = ctx->declarationSpecifiers();

		return ParseDeclarationSpecifiers(declSpecs);
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
			auto fn = this->compilerLLVM->CreateFunctionDefinition(name, params, this->compilerLLVM->GetFunctionType(returnType, params, paramTypeList && paramTypeList->Ellipsis() != nullptr));

			global_scope = false;

			this->compilerLLVM->InitializeBlock(&fn->front(), false, &fn->back(), &fn->back());

			auto blockItemList = func->compoundStatement()->blockItemList();

			if (blockItemList)
				ParseBlockItemList(blockItemList);

			// if return is void, then this might need a implicit return;
			if (returnType.TypeName == "void")
			{
				this->compilerLLVM->CreateReturnCall(nullptr);
			}

			// Pop the stack
			this->compilerLLVM->CreateBlockBreak(nullptr);

			global_scope = true;
		}
		else if (dataStruct != nullptr)
		{
			ParseStructClassUnionDefinition(dataStruct);
		}
	}

	void ParseBlockItemList(CParser::BlockItemListContext* ctx)
	{
		auto blockItems = ctx->blockItem();

		for (const auto& blockItem : blockItems)
		{
			auto decl = blockItem->declaration();
			auto statement = blockItem->statement();

			if (decl != nullptr)
			{
				ParseDeclaration(decl);
			}
			else if (statement != nullptr)
			{
				ParseStatement(statement);
			}
		}
	}

	void ParseStatement(CParser::StatementContext* statement)
	{
		auto jump = statement->jumpStatement();
		auto expressStatement = statement->expressionStatement();
		auto iterationStatement = statement->iterationStatement();
		auto selectionStatement = statement->selectionStatement();
		auto compoundStatement = statement->compoundStatement();

		if (jump != nullptr)
		{
			if (jump->Return())
			{
				auto express = jump->expression();
				auto right = ParseExpression(express);
				this->compilerLLVM->CreateReturnCall(right);
				return;
			}
			else if (jump->Continue())
			{
				this->compilerLLVM->CreateContinueCall();
				return;
			}
			else if (jump->Break())
			{
				this->compilerLLVM->CreateBreakCall();
				return;
			}
		}
		else if (expressStatement != nullptr)
		{
			auto express = expressStatement->expression();
			if (express != nullptr)
			{
				ParseExpression(express);
				if (PlusPlus.size() > 0)
				{
					for (auto increment : PlusPlus)
					{
						auto destination = increment.first;
						auto amount = increment.second;

						this->compilerLLVM->CreateIncrement(destination, amount);
					}

					PlusPlus.clear();
				}

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
			*/

			if (iterationStatement->While())
			{
				auto expression = iterationStatement->expression();
				auto innerStatement = iterationStatement->statement();

				auto blockStart = this->compilerLLVM->CreateBasicBlock("whileStart");
				auto blockInner = this->compilerLLVM->CreateBasicBlock("whileInner");
				auto blockResume = this->compilerLLVM->CreateBasicBlock("whileResume");

				this->compilerLLVM->CreateBlockBreak(blockStart, false);

				this->compilerLLVM->InitializeBlock(blockStart, true, blockStart, blockResume);
				auto condition = ParseExpression(expression);
				this->compilerLLVM->CreateConditionJump(condition, blockInner, blockResume);

				this->compilerLLVM->InitializeBlock(blockInner, false);
				ParseStatement(innerStatement);
				this->compilerLLVM->CreateContinueCall();

				// resume
				this->compilerLLVM->InitializeBlock(blockResume, false);

				// pop the stack
				this->compilerLLVM->CreateBlockBreak(nullptr, true);

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
				auto condition = ParseExpression(expression);

				auto blockIf = this->compilerLLVM->CreateBasicBlock("ifTrue");
				llvm::BasicBlock* blockElse = selectionStatement->Else() == nullptr ? nullptr : this->compilerLLVM->CreateBasicBlock("ifFalse");

				auto blockResume = this->compilerLLVM->CreateBasicBlock("ifResume");

				this->compilerLLVM->CreateConditionJump(condition, blockIf, blockElse ? blockElse : blockResume);

				this->compilerLLVM->InitializeBlock(blockIf, true);
				ParseStatement(innerStatement[0]);

				this->compilerLLVM->CreateBlockBreak(blockResume);

				if (blockElse != nullptr)
				{
					// else statement
					this->compilerLLVM->InitializeBlock(blockElse, true);
					ParseStatement(innerStatement[1]);
					this->compilerLLVM->CreateBlockBreak(blockResume);
				}

				// resume
				this->compilerLLVM->InitializeBlock(blockResume, false);
				return;
			}
		}
		else if (compoundStatement)
		{
			auto blockList = compoundStatement->blockItemList();
			if (blockList)
				ParseBlockItemList(blockList);
			return;
		}

		__debugbreak();
	}

	std::vector<MyCompilerLLVM::TypeAndValue> ParseDeclarationList(CParser::DeclarationListContext* ctx)
	{
		std::vector<MyCompilerLLVM::TypeAndValue> result;

		if (ctx)
		{
			auto declList = ctx->declaration();

			for (auto decl : declList)
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

					result.push_back(typeAndValue);  // Should copy?
				}
			}
		}

		return result;
	}

	std::vector<std::pair<std::string, llvm::AllocaInst*>> ParseDeclaration(CParser::DeclarationContext* ctx)
	{
		std::vector<std::pair<std::string, llvm::AllocaInst*>> allocList;

		auto declSpec = ctx->declarationSpecifiers();
		auto typeAndValue = ParseDeclarationSpecifiers(declSpec);

		auto initDecl = ctx->initDeclaratorList();
		auto initDeclarVec = initDecl->initDeclarator();

		if (typeAndValue.TypeName.empty())
		{
			__debugbreak();
		}

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
				std::vector<MyCompilerLLVM::TypeAndValue> params = ParseParamaterTypeList(paramTypeList);

				bool ellipsis = paramTypeList->Ellipsis() != nullptr;
				auto ft = this->compilerLLVM->GetFunctionType(typeAndValue, params, ellipsis);
				this->compilerLLVM->CreateFunctionDeclaration(direct->getText(), ft);
			}
			else if (direct != nullptr)
			{
				auto identList = declarator->identifierList();
				std::string name = direct->getText();
				typeAndValue.VariableName = name;

				if (identList != nullptr)
				{
					// TODO
					std::cout << "declarator identList: " << typeAndValue.TypeName << " " << name << "\n";
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
					auto constant = llvm::dyn_cast_or_null<llvm::Constant>(right);
					this->compilerLLVM->CreateGlobalVariable(typeAndValue, constant);
				}
				else
				{
					auto alloc = this->compilerLLVM->CreateLocalVariable(typeAndValue, right ? right->getType() : nullptr);
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
			auto assignCtx = ctx->assignmentExpression();

			auto destination = ParseUnaryExpression(unaryCtx, true);
			auto right = ParseAssignmentExpression(assignCtx);

			return compilerLLVM->CreateAssignment(right, destination);
		}
		else if (unaryCtx)
		{
			return ParseUnaryExpression(unaryCtx, false);
		}

		__debugbreak();
		return nullptr;
	}

	llvm::Value* ParseConditionalExpression(CParser::ConditionalExpressionContext* ctx)
	{
		auto logicCtx = ctx->logicalOrExpression();
		auto expressionFalse = ctx->expression();
		auto expressionTrue = ctx->conditionalExpression();

		if (logicCtx != nullptr)
		{
			auto expression = ParseLogicalOrExpression(logicCtx);

			// Both expression should exist or not exist.
			if ((expressionFalse != nullptr) != (expressionTrue != nullptr))
			{
				std::cout << "Conditional Expression require both true and false parts.\n";
				__debugbreak();
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
		if (nextCtxs.size() == 1)
		{
			return ParseRelationalExpression(nextCtxs[0]);
		}
		else if (nextCtxs.size() == 2)
		{
			auto left = ParseRelationalExpression(nextCtxs[0]);
			auto right = ParseRelationalExpression(nextCtxs[1]);

			return this->compilerLLVM->CreateOperation(ctx->children[1]->getText(), left, right);
		}

		__debugbreak();
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

			return this->compilerLLVM->CreateOperation(ctx->children[1]->getText(), left, right);
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
				lvalue = this->compilerLLVM->CreateOperation(ctx->children[count * 2 + 1]->getText(), lvalue, rvalue);
			}

			return lvalue;
		}

		__debugbreak();
		return nullptr;
	}

	llvm::Value* ParseMultiplicativeExpression(CParser::MultiplicativeExpressionContext* ctx)
	{
		auto nextCtxs = ctx->castExpression();

		if (nextCtxs.size() == 1)
		{
			return ParseCastExpression(nextCtxs[0]);
		}
		else if (nextCtxs.size() > 1)
		{
			llvm::Value* lvalue = nullptr;
			llvm::Value* rvalue = nullptr;

			int count = 0;
			for (const auto& nextCtx : nextCtxs)
			{
				rvalue = ParseCastExpression(nextCtx);
				lvalue = this->compilerLLVM->CreateOperation(ctx->children[count * 2 + 1]->getText(), lvalue, rvalue);
			}

			return lvalue;
		}

		__debugbreak();
		return nullptr;
	}

	llvm::Value* ParseCastExpression(CParser::CastExpressionContext* ctx)
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
			auto rvalue = ParseCastExpression(castExp);
			auto destTypeName = ParseTypeName(typeName);
			auto type = compilerLLVM->GetType(destTypeName);

			return compilerLLVM->CreateCast(rvalue, type);
		}

		__debugbreak();
		return nullptr;
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
			__debugbreak();
			return typeValue;
		}

		if (abstractDecl && abstractDecl->pointer())
		{
			typeValue.pointer = true;
		}

		return typeValue;
	}


	llvm::Value* ParseUnaryExpression(CParser::UnaryExpressionContext* ctx, bool lValue = false)
	{
		auto postFixCtx = ctx->postfixExpression();
		auto castExpCtx = ctx->castExpression();
		auto unaryOperator = ctx->unaryOperator();

		if (postFixCtx != nullptr)
		{
			return ParsePostfixExpression(postFixCtx, lValue);
		}
		else if (unaryOperator && castExpCtx)
		{
			/* unaryOperator : '&' | '*'| '+'| '-'| '~'| '!'; */

			auto castExpression = ParseCastExpression(castExpCtx);

			if (unaryOperator->getText() == "&")
			{
				// llvm will convert alloc into pointer type.
			}

			// TODO, unaryOperator
			return castExpression;
		}

		__debugbreak();
		return nullptr;
	}

	llvm::Value* ParsePostfixExpression(CParser::PostfixExpressionContext* ctx, bool lValue = false)
	{
		auto primaryCtx = ctx->primaryExpression();
		if (primaryCtx != nullptr)
		{
			auto plusplus = ctx->PlusPlus().size();
			auto minusminus = ctx->MinusMinus().size();
			auto argumentList = ctx->argumentExpressionList();
			if (argumentList.size() > 0 || (ctx->LeftParen().size() && ctx->RightParen().size()))
			{
				// Function Callsite
				std::string functoinName = primaryCtx->getText();
				auto fn = this->compilerLLVM->GetFunction(functoinName);

				std::vector<llvm::Value*> argVec;

				if (argumentList.size() > 0)
				{
					auto assignmentExpressionCtx = argumentList[0]->assignmentExpression();

					for (const auto& argument : assignmentExpressionCtx)
					{
						argVec.push_back(this->ParseAssignmentExpression(argument));
					}
				}

				return this->compilerLLVM->CreateFunctionCall(fn, argVec);
			}
			else
			{
				auto [primaryExpression, storage] = ParsePrimaryExpression(primaryCtx);
				uint32_t count = 0;

				if (primaryExpression->getType()->isStructTy())
				{
					auto ident = ctx->Identifier(count);
					while ((ident = ctx->Identifier(count)) != nullptr)
					{
						auto structType = llvm::dyn_cast<llvm::StructType>(primaryExpression->getType());
						auto datastrcture = compilerLLVM->GetDatastructure(structType);
						uint32_t fieldCount = -1;
						std::string identName = ident->getText();
						for (auto field : datastrcture.StructFields)
						{
							fieldCount++;
							if (field.VariableName == identName)
							{
								break;
							}
						}

						if (fieldCount == -1)
						{
							__debugbreak();
						}

						auto destAlloc = compilerLLVM->CreateStructGEP(structType, storage, fieldCount);

						if (storage != nullptr)
							storage = destAlloc;

						primaryExpression = compilerLLVM->CreateLoad(destAlloc);
						count++;
					}
				}

				if (storage != nullptr)
				{
					// Disable increment on lValue.
					if (lValue)
						return storage;

					if (plusplus > 0)
					{
						PlusPlus[storage] += plusplus;
					}

					if (minusminus > 0)
					{
						PlusPlus[storage] -= minusminus;
					}
				}

				return primaryExpression;
			}
		}

		__debugbreak();
		return nullptr;
	}

	/// <summary>
	/// The Value and Storage if applicaple.
	/// </summary>
	/// <param name="ctx"></param>
	/// <returns>Returns the load instruction and storage if writable.</returns>
	std::tuple< llvm::Value*, llvm::Value*> ParsePrimaryExpression(CParser::PrimaryExpressionContext* ctx)
	{
		auto expressionCtx = ctx->expression();
		auto identifier = ctx->Identifier();
		auto constant = ctx->Constant();
		auto stringLiteral = ctx->StringLiteral();

		if (expressionCtx != nullptr)
		{
			return std::tuple(ParseExpression(expressionCtx), nullptr);
		}
		else if (stringLiteral.size() > 0)
		{
			// TODO handle encoding u8,u,U,L
			std::string rawText = ctx->getText();
			rawText = ProcessRawText(rawText);
			return std::tuple(this->compilerLLVM->CreateGlobalString("", rawText), nullptr);
		}
		else if (constant)
		{
			if (constant->getText() == "true")
			{
				return std::tuple(this->compilerLLVM->CreateConstant("bool", constant->getText()), nullptr);
			}
			else if (constant->getText() == "false")
			{
				return std::tuple(this->compilerLLVM->CreateConstant("bool", constant->getText()), nullptr);
			}
			else
			{
				std::string constantRaw = constant->getText();
				auto number = ParseNumberConstant(constantRaw);

				auto value = compilerLLVM->CreateConstant(number);
				return { value, nullptr };
			}
		}
		else if (identifier)
		{
			std::string name = identifier->getText();

			auto alloc = this->compilerLLVM->GetLocalVariable(name);

			if (alloc == nullptr)
			{
				auto funcArgument = this->compilerLLVM->GetFunctionArgument(name);

				if (funcArgument == nullptr)
				{
					// try getting global variable
					auto gVar = this->compilerLLVM->GetGlobalVariable(name);
					if (gVar == nullptr)
					{
						std::cout << "Undefined variable : " << name << "\n";
						__debugbreak();
						return std::tuple(nullptr, nullptr);
					}

					return std::tuple(this->compilerLLVM->CreateLoad(gVar), gVar);
				}

				return std::tuple(funcArgument, nullptr);
			}

			return std::tuple(this->compilerLLVM->CreateLoad(alloc), alloc);
		}

		__debugbreak();
		return std::tuple(nullptr, nullptr);
	}

	std::string ProcessRawText(std::string rawText)
	{
		std::string output;
		bool escape = false;
		auto itr = rawText.begin();

		if (*itr == '"')
		{
			// skip the first quote
			itr++;
		}

		while (itr != rawText.end())
		{
			char c = *itr;
			if (escape)
			{
				if (c == 'n')
					output += '\n';

				escape = false;
			}
			else if (c == '\\')
			{
				escape = true;
			}
			else
			{
				output += c;
			}

			itr++;
		}

		// Remove the last quote
		output.pop_back();

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
			}

			return left;
		}

		__debugbreak();
		return nullptr;
	}

	void ParseStructClassUnionDefinition(CParser::StructClassUnionDefinitionContext* ctx)
	{
		auto decl = ctx->directDeclarator();
		std::string structName = decl->getText();
		auto declarationList = ctx->declarationList();
		std::vector<llvm::Type*> types;

		auto declList = ParseDeclarationList(declarationList);

		// Create a opaqueStruct to initialize default constructor.
		auto structType = compilerLLVM->CreateStructType(structName, {});

		// Create default constructor
		auto funcDef = compilerLLVM->CreateFunctionDefinition(structName, {}, llvm::FunctionType::get(structType, {}, false));

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

			initilizers.push_back(rvalue);
		}

		structType = compilerLLVM->CreateStructType(structName, declList);
		llvm::Value* structVal = llvm::UndefValue::get(structType);

		MyCompilerLLVM::TypeAndValue myStruct;
		myStruct.TypeName = structName;
		myStruct.VariableName = "_" + structName;

		auto myStructAlloc = this->compilerLLVM->CreateLocalVariable(myStruct);
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
		this->compilerLLVM->CreateBlockBreak(nullptr);
	}

	std::vector<MyCompilerLLVM::TypeAndValue> ParseParamaterTypeList(CParser::ParameterTypeListContext* paramTypeList)
	{
		std::vector<MyCompilerLLVM::TypeAndValue> params;

		if (paramTypeList == nullptr)
			return params;

		auto paramList = paramTypeList->parameterList();
		auto paramDeclList = paramList->parameterDeclaration();

		for (auto paramDecl : paramDeclList)
		{
			MyCompilerLLVM::TypeAndValue paramType = this->ParseDeclarationSpecifiers(paramDecl->declarationSpecifiers());
			paramType.VariableName = paramDecl->declarator()->directDeclarator()->getText();
			params.push_back(paramType);
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

	void PrintContext(antlr4::ParserRuleContext* ctx)
	{
		int line = ctx->getStart()->getLine();
		int column = ctx->getStart()->getCharPositionInLine();
		std::cout << std::format("[{}:{}] {} : {}\n", line, column, parser->getRuleNames()[ctx->getRuleIndex()], ctx->getText());
	}

	void enterEveryRule(antlr4::ParserRuleContext* ctx) override
	{
		// PrintContext(ctx);
	}
};
