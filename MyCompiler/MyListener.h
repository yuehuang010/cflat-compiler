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
	bool global_scope = true; // true when parsing an entry in the global scope.
	const bool debugPrint = false;

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
		if (debugPrint)
			return;

		auto func = ctx->functionDefinition();
		auto dataStruct = ctx->structClassUnionDefinition();
		auto decl = ctx->declaration();

		if (decl != nullptr)
		{
			auto globalDecl = ParseDeclaration(decl);
		}
		else if (func != nullptr)
		{
			global_scope = false;
			ParseFunctionDefinition(func);
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
				compilerLLVM->CreateReturnCall(right);
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

				auto blockStart = compilerLLVM->CreateBasicBlock("whileStart");
				auto blockInner = compilerLLVM->CreateBasicBlock("whileInner");
				auto blockResume = compilerLLVM->CreateBasicBlock("whileResume");

				compilerLLVM->CreateBlockBreak(blockStart, false);

				compilerLLVM->InitializeBlock(blockStart, true, blockStart, blockResume);
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

				auto blockIf = compilerLLVM->CreateBasicBlock("ifTrue");
				llvm::BasicBlock* blockElse = selectionStatement->Else() == nullptr ? nullptr : compilerLLVM->CreateBasicBlock("ifFalse");

				auto blockResume = compilerLLVM->CreateBasicBlock("ifResume");

				compilerLLVM->CreateConditionJump(condition, blockIf, blockElse ? blockElse : blockResume);

				compilerLLVM->InitializeBlock(blockIf, true);
				ParseStatement(innerStatement[0]);

				compilerLLVM->CreateBlockBreak(blockResume);

				if (blockElse != nullptr)
				{
					// else statement
					compilerLLVM->InitializeBlock(blockElse, true);
					ParseStatement(innerStatement[1]);
					compilerLLVM->CreateBlockBreak(blockResume);
				}

				// resume
				compilerLLVM->InitializeBlock(blockResume, false);
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

	void ParseFunctionDefinition(CParser::FunctionDefinitionContext* func, std::string structName = {})
	{
		// Create Function Definition
		auto name = this->getFunctionName(func);
		auto returnType = this->getFunctionReturnType(func);
		CParser::ParameterTypeListContext* paramTypeList = func->parameterTypeList();
		auto params = this->ParseParamaterTypeList(paramTypeList);

		if (!structName.empty())
		{
			MyCompilerLLVM::TypeAndValue typeValue{
			.TypeName = structName,
			.VariableName = structName + "__",
			.pointer = true };
			params.insert(params.begin(), typeValue);
		}

		auto fn = compilerLLVM->CreateFunctionDefinition(name, params, compilerLLVM->GetFunctionType(returnType, params, paramTypeList && paramTypeList->Ellipsis() != nullptr));

		compilerLLVM->InitializeBlock(&fn->front(), false, &fn->back(), &fn->back());

		auto blockItemList = func->compoundStatement()->blockItemList();

		if (blockItemList)
			ParseBlockItemList(blockItemList);

		// if return is void, then this might need a implicit return;
		if (returnType.TypeName == "void")
		{
			compilerLLVM->CreateReturnCall(nullptr);
		}

		// Pop the stack
		compilerLLVM->CreateBlockBreak(nullptr);
	}

	std::vector<MyCompilerLLVM::TypeAndValue> ParseDeclarationList(std::vector<CParser::DeclarationContext*> ctx)
	{
		std::vector<MyCompilerLLVM::TypeAndValue> result;

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
				auto ft = compilerLLVM->GetFunctionType(typeAndValue, params, ellipsis);
				compilerLLVM->CreateFunctionDeclaration(direct->getText(), ft);
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
					compilerLLVM->CreateGlobalVariable(typeAndValue, constant);
				}
				else
				{
					auto alloc = compilerLLVM->CreateLocalVariable(typeAndValue, right ? right->getType() : nullptr);
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

			auto destination = ParseUnaryExpression(unaryCtx, true);
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

			return compilerLLVM->CreateOperation(ctx->children[1]->getText(), left, right);
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

			return compilerLLVM->CreateOperation(ctx->children[1]->getText(), left, right);
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
				lvalue = compilerLLVM->CreateOperation(ctx->children[count * 2 + 1]->getText(), lvalue, rvalue);
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
				lvalue = compilerLLVM->CreateOperation(ctx->children[count * 2 + 1]->getText(), lvalue, rvalue);
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

		std::string fulltext = ctx->getText();

		if (auto primaryCtx = ctx->primaryExpression())
		{
			size_t prevRuleId = 0;
			size_t prevToken = 0;
			std::string primaryIdentifier;
			llvm::Value* primaryValue = nullptr;
			llvm::Value* primaryStorage = nullptr;
			llvm::StructType* structType = nullptr;
			llvm::Value* structValue = nullptr;
			llvm::Value* structStorage = nullptr;
			int functionArgCounter = 0;

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
					case CParser::RightParen:
					case CParser::Dot:
					case CParser::Arrow: { prevToken = tokenType; break; }
					case CParser::PlusPlus: { if (primaryStorage) { PlusPlus[primaryStorage]++; }  break; }
					case CParser::MinusMinus: { if (primaryStorage) { PlusPlus[primaryStorage]--; }break; }
					case CParser::Identifier:
					{
						if (structType)
						{
							primaryIdentifier = terminal->getText();
							auto datastrcture = compilerLLVM->GetDatastructure(structType);
							uint32_t fieldCount = 0;

							for (auto field : datastrcture.StructFields)
							{
								if (field.VariableName == primaryIdentifier)
								{
									break;
								}
								fieldCount++;
							}

							if (fieldCount < datastrcture.StructFields.size())
							{
								primaryStorage = compilerLLVM->CreateStructGEP(structType, structStorage, fieldCount);
								primaryValue = compilerLLVM->CreateLoad(primaryStorage);
							}
							else if (auto func = compilerLLVM->GetFunction(primaryIdentifier))
							{
								// Not a field, then try a function.
								primaryValue = func;
								primaryStorage = nullptr;
							}
							else
							{
								__debugbreak();
							}
						}
						else
						{
							std::tie(primaryValue, primaryStorage) = ParseIdentifier(primaryIdentifier);
						}

						if (primaryValue && primaryValue->getType()->isStructTy())
						{
							structType = llvm::dyn_cast<llvm::StructType>(primaryValue->getType());
							structValue = primaryValue;
							structStorage = primaryStorage;
						}
						//if (!primaryStorage)
						//{
						//	// TODO: verify use case when to clear.
						//	structType = nullptr;
						//	structValue = nullptr;
						//}

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
						std::tie(primaryValue, primaryStorage) = ParsePrimaryExpression(prevPrimary);

						if (primaryValue && primaryValue->getType()->isStructTy())
						{
							structType = llvm::dyn_cast<llvm::StructType>(primaryValue->getType());
							structValue = primaryValue;
							structStorage = primaryStorage;
						}
						if (!primaryStorage)
						{
							structType = nullptr;
							structValue = nullptr;
							structStorage = nullptr;
						}

						break;
					}
					case CParser::RuleExpression:
					{
						// TODO: bracket expression
						break;
					}
					case CParser::RuleArgumentExpressionList:
					{
						// Create Function Call
						std::string functionName = primaryIdentifier;
						auto func = compilerLLVM->GetFunction(functionName);

						std::vector<llvm::Value*> argVec;
						if (structType)
						{
							argVec.push_back(structStorage);
						}

						auto argumentList = ctx->argumentExpressionList();
						if (argumentList.size() > 0)
						{
							auto assignmentExpressionCtx = argumentList[functionArgCounter]->assignmentExpression();

							for (const auto& argument : assignmentExpressionCtx)
							{
								argVec.push_back(this->ParseAssignmentExpression(argument));
							}
						}

						primaryValue = compilerLLVM->CreateFunctionCall(func, argVec);
						primaryStorage = nullptr;

						if (primaryValue->getType()->isStructTy())
						{
							structType = llvm::dyn_cast<llvm::StructType>(primaryValue->getType());
							structStorage = compilerLLVM->CreateAlloca(structType->getPointerTo());
							compilerLLVM->CreateAssignment(primaryValue, structStorage);
							structValue = primaryValue;
						}

						functionArgCounter++;
						break;
					}

					default: { __debugbreak(); }
					}
				}
			}

			if (lValue)
				return primaryStorage;

			return primaryValue;
		}

		__debugbreak();
		return nullptr;
	}

	/// <summary>
	/// The Value and Storage if applicaple.
	/// </summary>
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
			return std::tuple(compilerLLVM->CreateGlobalString("", rawText), nullptr);
		}
		else if (constant)
		{
			if (constant->getText() == "true")
			{
				return std::tuple(compilerLLVM->CreateConstant("bool", constant->getText()), nullptr);
			}
			else if (constant->getText() == "false")
			{
				return std::tuple(compilerLLVM->CreateConstant("bool", constant->getText()), nullptr);
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
			return ParseIdentifier(name);
		}

		__debugbreak();
		return std::tuple(nullptr, nullptr);
	}

	std::tuple< llvm::Value*, llvm::Value*> ParseIdentifier(std::string name)
	{
		if (auto alloc = compilerLLVM->GetLocalVariable(name))
		{
			return std::tuple(compilerLLVM->CreateLoad(alloc), alloc);
		}

		if (auto funcArgument = compilerLLVM->GetFunctionArgument(name))
		{
			return std::tuple(funcArgument, nullptr);
		}

		if (auto memberVar = compilerLLVM->GetMemberVariable(name))
		{
			return std::tuple(compilerLLVM->CreateLoad(memberVar), memberVar);
		}

		// try getting global variable
		if (auto gVar = compilerLLVM->GetGlobalVariable(name))
		{
			return std::tuple(compilerLLVM->CreateLoad(gVar), gVar);
		}

		if (auto func = compilerLLVM->GetFunction(name))
		{
			return std::tuple(func, nullptr);
		}

		std::cout << "Undefined variable : " << name << "\n";
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
		auto declarationList = ctx->declaration();
		std::vector<llvm::Type*> types;

		auto declList = ParseDeclarationList(declarationList);

		// Create a opaqueStruct to initialize default constructor.
		auto structType = compilerLLVM->CreateStructType(structName, {});

		// Create default constructor
		{
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
			compilerLLVM->CreateBlockBreak(nullptr);
		}

		// Parse member functions
		auto functionList = ctx->functionDefinition();

		for (auto func : functionList)
		{
			global_scope = false;
			ParseFunctionDefinition(func, structName);
			global_scope = true;
		}
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
		if (debugPrint)
			PrintContext(ctx);
	}
};
