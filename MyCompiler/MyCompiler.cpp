// MyCompiler.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>

#include <llvm\IR\IRBuilder.h>
#include <llvm\IR\LLVMContext.h>
#include <llvm\IR\Module.h>
#include <llvm\IR\Verifier.h>

class MyCompilerLLVM {
private:
	std::unique_ptr<llvm::IRBuilder<>> builder;
	std::unique_ptr<llvm::Module> module;
	std::unique_ptr<llvm::LLVMContext> context;

	llvm::Function* CreateFunction(std::string name, llvm::FunctionType* returnType) {
		auto fn = module->getFunction(name);

		if (fn == nullptr) {
			fn = createFunctionProto(name, returnType);
		}

		createFunctionBlock(fn);

		return fn;
	}

	llvm::Function* createFunctionProto(std::string name, llvm::FunctionType* returnType) {
		auto fn = llvm::Function::Create(returnType, llvm::Function::ExternalLinkage, name, *module);

		llvm::verifyFunction(*fn);

		return fn;
	}

	void createFunctionBlock(llvm::Function* fn) {
		auto entry = CreateBasicBlock("entry", fn);
		builder->SetInsertPoint(entry);
	}

	llvm::BasicBlock* CreateBasicBlock(std::string name, llvm::Function* fn = nullptr) {
		return llvm::BasicBlock::Create(*context, name, fn);
	}

	void GenerateIR() {
		llvm::Function* func = CreateFunction("main", llvm::FunctionType::get(builder->getInt32Ty(), false));

		auto body = gen();
		auto i32Result = builder->CreateIntCast(body, builder->getInt32Ty(), true);

		builder->CreateRet(i32Result);
	}

	llvm::Value* gen() { return builder->getInt32(43); }

	void Init() {
		context = std::make_unique<llvm::LLVMContext>();
		module = std::make_unique<llvm::Module>("MyCompiler", *context);
		builder = std::make_unique<llvm::IRBuilder<>>(*context);
	}

	void SaveToFile(std::string filename) {
		std::error_code errorCode;
		llvm::raw_fd_ostream outLL(filename, errorCode);
		module->print(outLL, nullptr);
	}

public:
	MyCompilerLLVM() { Init(); }
	~MyCompilerLLVM() {
		builder.release();
		module.release();

		// context needs to be first so that objects can be free in the right order.
		context.release();
	}

	bool Compile(std::string program) {
		// parse
		// compile to IR
		// IR to bitcode.
		GenerateIR();
		SaveToFile(".\\out.ll");

		return true;
	}
};

int main() {
	std::cout << "MyCompilerLLVM\n";

	MyCompilerLLVM compiler;
	compiler.Compile("");
	std::cout << "Done.\n";

}

// Run program: Ctrl + F5 or Debug > Start Without Debugging menu
// Debug program: F5 or Debug > Start Debugging menu

// Tips for Getting Started: 
//   1. Use the Solution Explorer window to add/manage files
//   2. Use the Team Explorer window to connect to source control
//   3. Use the Output window to see build output and other messages
//   4. Use the Error List window to view errors
//   5. Go to Project > Add New Item to create new code files, or Project > Add Existing Item to add existing code files to the project
//   6. In the future, to open this project again, go to File > Open > Project and select the .sln file
