// SyntaxGenerator.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <unordered_map>
#include <antlr4-runtime.h>

#include "CParser.h"
#include "CLexer.h"
#include "CBaseListener.h"


class MyListener : public CBaseListener {
private:
	std::vector<std::string> declaration;

	bool isFunctionOrNamespace(antlr4::ParserRuleContext* ctx) {
		if (ctx->getRuleIndex() == CParser::RuleFunctionDefinition)
			return true;
		return false;
	}

	std::string getFunctionName(CParser::FunctionDefinitionContext* ctx) {
		std::string name;
		auto decl = ctx->declarator();
		auto directDecl = decl->directDeclarator();

		auto directDecl2 = directDecl->directDeclarator();
		if (directDecl2 != nullptr)
			name = directDecl2->getText();
		else
			name = directDecl->getText();

		return name;
	}

	std::string getParentFullName(antlr4::ParserRuleContext* ctx) {
		std::string name;

		while (ctx) {
			if (isFunctionOrNamespace(ctx)) {
				auto funcName = getFunctionName((CParser::FunctionDefinitionContext*)ctx);
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
	void enterDeclaration(CParser::DeclarationContext* ctx) override {
		std::cout << "enterDeclaration: " << ctx->getText() << std::endl;
		auto list = ctx->initDeclaratorList();
		auto spec = ctx->declarationSpecifiers();
		if (!list || list->isEmpty()) {

			if (spec) {
				std::cout << "spec: " << spec->getText() << std::endl;
			}
			return;
		}

		auto declarVec = list->initDeclarator();

		auto fullname = getParentFullName(ctx);
		for (auto decl : declarVec) {
			auto declarator = decl->declarator();
			auto direct = declarator->directDeclarator();
			auto text = fullname + "::" + direct->getText();
			declaration.push_back(text);
			std::cout << "Declaration: " << text << "\n";
		}
	}
	void exitDeclaration(CParser::DeclarationContext* ctx) override {
		// std::cout << "exitDeclaration: " << ctx->getText() << "\n";
	}
	void enterExternalDeclaration(CParser::ExternalDeclarationContext* ctx) override {
		// std::cout << "enterExternalDeclaration: " << ctx->getText() << "\n";
	}
	void exitExternalDeclaration(CParser::ExternalDeclarationContext* ctx) override {
		// std::cout << "exitExternalDeclaration: " << ctx->getText() << "\n";
	}
	void enterBlockItem(CParser::BlockItemContext* ctx) override {
		// std::cout << "enterBlockItem: " << ctx->getText() << "\n";
	}
	void exitBlockItem(CParser::BlockItemContext* ctx) override {
		// std::cout << "exitBlockItem: " << ctx->getText() << "\n";
	}
	void enterBlockItemList(CParser::BlockItemListContext* ctx) override {
		// std::cout << "enterBlockItemList: " << ctx->getText() << "\n";
	}
	void exitBlockItemList(CParser::BlockItemListContext* ctx) override {
		// std::cout << "exitBlockItemList: " << ctx->getText() << "\n";
	}
	void enterStatement(CParser::StatementContext* ctx) override {
		// std::cout << "enterStatement: " << ctx->getText() << "\n";
	}
	void exitStatement(CParser::StatementContext* ctx) override {
		// std::cout << "exitStatement: " << ctx->getText() << "\n";
	}

};

bool Parse(std::ifstream& stream) {
	antlr4::ANTLRInputStream input(stream);

	CLexer lexer(&input);
	antlr4::CommonTokenStream tokens(&lexer);
	CParser parser(&tokens);

	tokens.fill();
	//for (auto token : tokens.getTokens()) {
	//	std::cout << token->toString() << std::endl;
	//}

	auto computeUnit = parser.compilationUnit();
	// std::cout << "\nCompilation Unit\n";

	MyListener* mylistener = new MyListener();
	auto walker = antlr4::tree::ParseTreeWalker();
	walker.walk(mylistener, computeUnit);
	delete mylistener;

	stream.close();

	return true;
}


int main() {

	std::cout << "Parsing Starting.\n";
	std::ifstream stream;
	stream.open("testfile.cflat");

	if (Parse(stream)) {
		std::cout << "Parsing Complete.\n";
	}
	else {
		std::cout << "Parsing Error.\n";
	}
}