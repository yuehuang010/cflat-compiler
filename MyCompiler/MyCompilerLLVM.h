#pragma once

#include <deque>

#include <llvm\IR\IRBuilder.h>
#include <llvm\IR\LLVMContext.h>
#include <llvm\IR\Module.h>
#include <llvm\IR\Verifier.h>
#include <antlr4-runtime.h>



class MyCompilerLLVM
{
public:
	enum class operation
	{
		None,
		Add,
		Subtract,
		Multiply,
		Divide,
	};

private:
	std::unique_ptr<llvm::IRBuilder<>> builder;
	std::unique_ptr<llvm::Module> module;
	std::unique_ptr<llvm::LLVMContext> context;
	std::vector<std::unordered_map<std::string, llvm::AllocaInst*>> stackNamedVariable;
	std::unordered_map<std::string, llvm::GlobalVariable*> globalNamedVariable;

private:
	// Create Function Proto or Signature
	llvm::Function* createFunctionProto(std::string name, llvm::FunctionType* returnType)
	{
		auto fn = llvm::Function::Create(returnType, llvm::Function::ExternalLinkage, name, *module);

		llvm::verifyFunction(*fn);

		return fn;
	}

	void createFunctionBlock(llvm::Function* fn)
	{
		// all function starts at "entry" block
		auto entry = CreateBasicBlock("entry", fn);
		builder->SetInsertPoint(entry);
		stackNamedVariable.emplace_back();
	}

	llvm::BasicBlock* CreateBasicBlock(std::string name, llvm::Function* fn = nullptr)
	{
		return llvm::BasicBlock::Create(*context, name, fn);
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

public:
	MyCompilerLLVM() { Init(); }
	~MyCompilerLLVM()
	{
		builder.release();
		module.release();

		// context is last to be released.
		context.release();
	}

	llvm::GlobalVariable* CreateGlobalVariable(std::string name, std::string typeName, llvm::ConstantInt* initValue)
	{
		auto gVar = new llvm::GlobalVariable(
			*module,
			GetType(typeName), // Type: int32
			false, // isConstant
			llvm::GlobalValue::ExternalLinkage,
			initValue, // Initial value
			name // Name
		);

		globalNamedVariable[name] = gVar;

		return gVar;
	}

	llvm::AllocaInst* CreateVariable(std::string name, std::string typeName)
	{
		auto alloc = builder->CreateAlloca(GetType(typeName), nullptr, name);
		stackNamedVariable.back()[name] = alloc;
		return alloc;
	}

	llvm::StoreInst* CreateAssignment(llvm::Value* value, llvm::AllocaInst* destination)
	{
		return builder->CreateStore(value, destination);
	}

	llvm::StoreInst* CreateAssignment(llvm::Value* value, llvm::GlobalVariable* destination)
	{
		return builder->CreateStore(value, destination);
	}

	llvm::LoadInst* CreateLoad(llvm::AllocaInst* alloc)
	{
		return builder->CreateLoad(alloc->getAllocatedType(), alloc);
	}

	llvm::LoadInst* CreateLoad(llvm::GlobalVariable* gVar)
	{
		return builder->CreateLoad(gVar->getType(), gVar);
	}

	llvm::Value* CreateConstant(std::string typeName, std::string initialValue)
	{
		llvm::Value* value = nullptr;

		if (typeName == "char")
		{
			int initValue = 0;
			if (!initialValue.empty())
			{
				initValue = std::stoi(initialValue);
			}

			value = builder->getInt8(initValue);
		}
		else if (typeName == "short")
		{
			int initValue = 0;
			if (!initialValue.empty())
			{
				initValue = std::stoi(initialValue);
			}

			value = builder->getInt16(initValue);
		}
		else if (typeName == "int")
		{
			int initValue = 0;
			if (!initialValue.empty())
			{
				initValue = std::stoi(initialValue);
			}

			value = builder->getInt32(initValue);
		}
		else if (typeName == "long")
		{
			int initValue = 0;
			if (!initialValue.empty())
			{
				initValue = std::stoi(initialValue);
			}

			value = builder->getInt64(initValue);
		}
		//else if (typeName == "float")
		//{
		//	float initValue = 0;
		//	if (!initialValue.empty())
		//	{
		//		initValue = std::stof(initialValue);
		//	}

		//	value = builder->getFP(initValue);
		//}
		//else if (typeName == "double")
		//{
		//	double initValue = 0;
		//	if (!initialValue.empty())
		//	{
		//		initValue = std::stoi(initialValue);
		//	}

		//	value = builder->getInt8(initValue);
		//}
		else if (typeName == "bool")
		{
			if (!initialValue.empty() && initialValue == "true")
			{
				value = builder->getTrue();
			}
			else
			{

				value = builder->getFalse();
			}
		}
		else
		{
			std::cout << "Unknown value: " << typeName << "\n";
			return nullptr;
		}

		return value;
	}

	llvm::Value* CreateGlobalString(std::string name, std::string text)
	{
		return builder->CreateGlobalStringPtr(text, name);
	}

	llvm::Value* CreateOperation(operation op, llvm::Value* left, llvm::Value* right)
	{
		if (left == nullptr)
			return right;

		// NSW (No Signed Wrap) and NUS(No Unsigned Wrap)
		switch (op)
		{
		case operation::Add: {
			return builder->CreateAdd(left, right);
		}
		case operation::Subtract: {
			return builder->CreateSub(left, right);
		}
		case operation::Multiply: {
			return builder->CreateMul(left, right);
		}
		case operation::Divide: {
			return builder->CreateSDiv(left, right);
		}
		}

		__debugbreak();
		return right;
	}

	void CreateFunctionDeclaration(std::string name, llvm::FunctionType* functionType = nullptr)
	{
		module->getOrInsertFunction(name, functionType);
	}

	llvm::Function* CreateFunctionDefinition(std::string name, llvm::FunctionType* returnType = nullptr)
	{
		auto fn = module->getFunction(name);
		if (returnType == nullptr)
		{
			returnType = llvm::FunctionType::get(builder->getVoidTy(), false);
		}

		if (fn == nullptr)
		{
			fn = createFunctionProto(name, returnType);
		}

		createFunctionBlock(fn);

		return fn;
	}

	llvm::Type* GetType(std::string typeName, bool pointer = false)
	{
		llvm::Type* type = nullptr;

		if (typeName == "void") { type = builder->getVoidTy(); }
		else if (typeName == "char") { type = builder->getInt8Ty(); }
		else if (typeName == "short") { type = builder->getInt16Ty(); }
		else if (typeName == "int") { type = builder->getInt32Ty(); }
		else if (typeName == "long") { type = builder->getInt64Ty(); }
		else if (typeName == "float") { type = builder->getFloatTy(); }
		else if (typeName == "double") { type = builder->getDoubleTy(); }
		else if (typeName == "bool") { type = builder->getInt1Ty(); }
		else
		{
			std::cout << "Unknown value: " << typeName << "\n";
			type = builder->getVoidTy();
		}

		if (pointer)
			return type->getPointerTo();

		return type;
	}

	llvm::Function* GetFunction(std::string functionName)
	{
		return module->getFunction(functionName);
	}

	llvm::AllocaInst* GetLocalVariable(std::string name)
	{
		auto lastBlock = stackNamedVariable.back();
		auto result = lastBlock.find(name);
		if (result != lastBlock.end())
		{
			return result->second;
		}

		return nullptr;
	}

	llvm::GlobalVariable* GetGlobalVariable(std::string name)
	{
		auto result = globalNamedVariable.find(name);
		if (result != globalNamedVariable.end())
		{
			return result->second;
		}

		return nullptr;
	}

	llvm::Value* CreateFunctionCall(llvm::Function* func, std::vector<llvm::Value*> arg)
	{
		return builder->CreateCall(func, arg);
	}

	llvm::Value* CreateReturnCall(llvm::Value* value)
	{
		auto result = builder->CreateRet(value);
		stackNamedVariable.pop_back();
		return result;
	}

	bool Compile(std::string filename);
};

