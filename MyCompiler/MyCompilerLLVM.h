#pragma once
#include <llvm\IR\IRBuilder.h>
#include <llvm\IR\LLVMContext.h>
#include <llvm\IR\Module.h>
#include <llvm\IR\Verifier.h>
#include <antlr4-runtime.h>

class MyCompilerLLVM
{
private:
	std::unique_ptr<llvm::IRBuilder<>> builder;
	std::unique_ptr<llvm::Module> module;
	std::unique_ptr<llvm::LLVMContext> context;

private:
	llvm::Function* createFunctionProto(std::string name, llvm::FunctionType* returnType)
	{
		auto fn = llvm::Function::Create(returnType, llvm::Function::ExternalLinkage, name, *module);

		llvm::verifyFunction(*fn);

		return fn;
	}

	void createFunctionBlock(llvm::Function* fn)
	{
		auto entry = CreateBasicBlock("entry", fn);
		builder->SetInsertPoint(entry);
	}

	llvm::BasicBlock* CreateBasicBlock(std::string name, llvm::Function* fn = nullptr)
	{
		return llvm::BasicBlock::Create(*context, name, fn);
	}

	void GenerateIR()
	{
		llvm::Function* func = CreateFunction("main", llvm::FunctionType::get(builder->getInt32Ty(), false));

		auto body = gen();
		auto i32Result = builder->CreateIntCast(body, builder->getInt32Ty(), true);

		builder->CreateRet(i32Result);
	}

	llvm::Value* gen() { return builder->getInt32(43); }

	void Init()
	{
		context = std::make_unique<llvm::LLVMContext>();
		module = std::make_unique<llvm::Module>("MyCompiler", *context);
		builder = std::make_unique<llvm::IRBuilder<>>(*context);
	}

	void SaveToFile(std::string filename)
	{
		std::error_code errorCode;
		llvm::raw_fd_ostream outLL(filename, errorCode);
		module->print(outLL, nullptr);
	}

	bool Parse(std::ifstream& stream)
	{


		return true;
	}

public:
	MyCompilerLLVM() { Init(); }
	~MyCompilerLLVM()
	{
		builder.release();
		module.release();

		// context needs to be last so that objects can be free in the right order.
		context.release();
	}

	llvm::Function* CreateFunction(std::string name, llvm::FunctionType* returnType)
	{
		auto fn = module->getFunction(name);

		if (fn == nullptr)
		{
			fn = createFunctionProto(name, returnType);
		}

		createFunctionBlock(fn);

		return fn;
	}

	bool Compile(std::string filename);
};

