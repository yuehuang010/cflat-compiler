#include <llvm\IR\IRBuilder.h>
#include <llvm\IR\LLVMContext.h>
#include <llvm\IR\Module.h>
#include <llvm\IR\Verifier.h>
#include <antlr4-runtime.h>

#include "CParser.h"
#include "CLexer.h"
#include "CBaseListener.h"
#include "MyCompilerLLVM.h"
#include "MyListener.h"

bool MyCompilerLLVM::Compile(std::string filename)
{
	std::ifstream stream;
	stream.open(filename);
	antlr4::ANTLRInputStream input(stream);

	CLexer lexer(&input);
	antlr4::CommonTokenStream tokens(&lexer);
	CParser parser(&tokens);

	tokens.fill();
	//for (auto token : tokens.getTokens()) {
	//	std::cout << token->toString() << std::endl;
	//}

	auto computeUnit = parser.compilationUnit();

	MyListener* mylistener = new MyListener(&parser, this);
	auto walker = antlr4::tree::ParseTreeWalker();
	walker.walk(mylistener, computeUnit);
	delete mylistener;

	stream.close();

	SaveToFile(".\\out.ll");

	return true;
}