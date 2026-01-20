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
		auto directDecl1 = ctx->directDeclarator();
		auto directDecl2 = directDecl1->directDeclarator();

		if (directDecl2 != nullptr)
			name = directDecl2->getText();
		else
			name = directDecl1->getText();

		return name;
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

	void enterDeclaration(CParser::DeclarationContext* ctx) override
	{
		// std::cout << "enterDeclaration: " << ctx->getText() << std::endl;
		auto list = ctx->initDeclaratorList();
		auto spec = ctx->declarationSpecifiers();
		std::string specText = "";

		if (spec)
		{
			specText = spec->getText();
		}

		auto declarVec = list->initDeclarator();

		auto fullname = getScopeName(ctx);
		for (auto decl : declarVec)
		{
			auto declarator = decl->declarator();
			auto direct = declarator->directDeclarator();
			auto text = fullname + "::" + direct->getText();
			declaration.push_back(text);
			std::cout << "declarator: " << specText << " " << text << "\n";
		}
	}
	void enterExternalDeclaration(CParser::ExternalDeclarationContext* ctx) override
	{
		// if (false)
		{
			auto decl = ctx->declaration();
			auto func = ctx->functionDefinition();
			auto dataStruct = ctx->structClassUnionDefinition();
			std::string text;
			text += (decl == nullptr) ? "" : "declaration";
			text += (func == nullptr) ? "" : "function";
			text += (dataStruct == nullptr) ? "" : "struct";
			std::cout << "enterExternalDeclaration: " << ctx->getText() << " : " << text << "\n";
		}
	}

	void enterEveryRule(antlr4::ParserRuleContext* ctx) override
	{
		// std::cout << parser->getRuleNames()[ctx->getRuleIndex()] << " : " << ctx->getText() << "\n";
	}
};
