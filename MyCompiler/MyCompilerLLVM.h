#pragma once

#include <deque>
#include <ranges>

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
		Equal, // ==
		NotEqual, // !=
		Greater, // >
		GreaterEqual, // >=
		Less, // <
		LessEqual, // <=
	};

	class StackState
	{
	public:
		std::unordered_map<std::string, llvm::AllocaInst*> namedVariable;
		llvm::BasicBlock* continueBlock = nullptr; // continue;
		llvm::BasicBlock* resumeBlock = nullptr; // break;

		void ClearBlock()
		{
			continueBlock = nullptr;
			resumeBlock = nullptr;
		}	
	};

private:
	std::unique_ptr<llvm::IRBuilder<>> builder;
	std::unique_ptr<llvm::Module> module;
	std::unique_ptr<llvm::LLVMContext> context;
	// std::vector<std::unordered_map<std::string, llvm::AllocaInst*>> stackNamedVariable;
	std::vector<StackState> stackNamedVariable;
	std::unordered_map<std::string, llvm::GlobalVariable*> globalNamedVariable;
	llvm::Function* currentFunction;

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
		currentFunction = fn;
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
	operation ParseOperation(std::string operationText)
	{
		if (operationText == "+") { return operation::Add; }
		else if (operationText == "*") { return operation::Multiply; }
		else if (operationText == "-") { return operation::Subtract; }
		else if (operationText == "/") { return operation::Divide; }
		else if (operationText == "==") { return operation::Equal; }
		else if (operationText == "!=") { return operation::NotEqual; }
		else if (operationText == ">") { return operation::Greater; }
		else if (operationText == ">=") { return operation::GreaterEqual; }
		else if (operationText == "<") { return operation::Less; }
		else if (operationText == "<=") { return operation::LessEqual; }

		__debugbreak();
		return operation::None;
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
		stackNamedVariable.back().namedVariable[name] = alloc;
		return alloc;
	}

	llvm::Value* CreateIncrement(llvm::Value* destination, int amount)
	{
		llvm::LoadInst* loadInst;
		if (auto alloc = llvm::dyn_cast<llvm::AllocaInst>(destination))
		{
			loadInst = CreateLoad(alloc);
		}
		else if (auto gVar = llvm::dyn_cast<llvm::GlobalVariable>(destination))
		{
			loadInst = CreateLoad(gVar);
		}
		else
		{
			__debugbreak();
			return nullptr;
		}

		auto value = llvm::ConstantInt::get(loadInst->getType(), amount);
		// auto value = builder->getInt32(amount);
		auto newValue = CreateOperation(operation::Add, loadInst, value);
		return builder->CreateStore(newValue, destination);
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

	llvm::Value* CreateOperation(std::string oper, llvm::Value* left, llvm::Value* right)
	{
		if (left == nullptr)
			return right;

		operation op = ParseOperation(oper);
		return CreateOperation(op, left, right);
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
		case operation::Equal: {
			return builder->CreateICmp(llvm::ICmpInst::ICMP_EQ, left, right);
		}
		case operation::NotEqual: {
			return builder->CreateICmp(llvm::ICmpInst::ICMP_NE, left, right);
		}
		case operation::Greater: {
			return builder->CreateICmp(llvm::ICmpInst::ICMP_SGT, left, right);
		}
		case operation::GreaterEqual: {
			return builder->CreateICmp(llvm::ICmpInst::ICMP_SGE, left, right);
		}
		case operation::Less: {
			return builder->CreateICmp(llvm::ICmpInst::ICMP_SLT, left, right);
		}
		case operation::LessEqual: {
			return builder->CreateICmp(llvm::ICmpInst::ICMP_SLE, left, right);
		}
		}

		__debugbreak();
		return right;
	}

	llvm::BasicBlock* CreateBasicBlock(std::string name, llvm::Function* fn = nullptr)
	{
		if (fn == nullptr)
			fn = currentFunction;

		return llvm::BasicBlock::Create(*context, name, fn);
	}

	llvm::BranchInst* CreateConditionJump(llvm::Value* cond, llvm::BasicBlock* trueBlock, llvm::BasicBlock* falseBlock)
	{
		auto branchInst = builder->CreateCondBr(cond, trueBlock, falseBlock);
		builder->SetInsertPoint(trueBlock);
		return branchInst;
	}

	/// <summary>
	/// Exit the current BasicBlock and then jump to resumeBlock.
	/// </summary>
	/// <param name="resumeBlock"></param>
	/// <returns></returns>
	llvm::BranchInst* CreateBlockBreak(llvm::BasicBlock* resumeBlock, bool exitBlackStack = true)
	{
		if (exitBlackStack)
			stackNamedVariable.pop_back();

		if (resumeBlock)
			return builder->CreateBr(resumeBlock);

		return nullptr;
	}

	void InitializeBlock(llvm::BasicBlock* block, bool enterBlockStack = true, llvm::BasicBlock* continueBlock = nullptr, llvm::BasicBlock* resumeBlock = nullptr)
	{
		if (enterBlockStack)
		{
			auto& stack = stackNamedVariable.emplace_back();
			stack.continueBlock = continueBlock;
			stack.resumeBlock = resumeBlock;
		}

		builder->SetInsertPoint(block);
	}

	void CreateFunctionDeclaration(std::string name, llvm::FunctionType* functionType = nullptr)
	{
		module->getOrInsertFunction(name, functionType);
	}

	llvm::Function* CreateFunctionDefinition(std::string name, llvm::FunctionType* functionType = nullptr)
	{
		auto fn = module->getFunction(name);
		if (functionType == nullptr)
		{
			functionType = llvm::FunctionType::get(builder->getVoidTy(), false);
		}

		if (fn == nullptr)
		{
			fn = createFunctionProto(name, functionType);
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
		for (const auto& stackframe : std::ranges::reverse_view(stackNamedVariable))
		{
			auto nameVal = stackframe.namedVariable;
			auto result = nameVal.find(name);

			if (result != nameVal.end())
			{
				return result->second;
			}
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

	void CreateReturnCall(llvm::Value* value)
	{
		// check if break has already been inserted.
		if (builder->GetInsertBlock()->getTerminator() != nullptr)
			return;

		if (value == nullptr)
			builder->CreateRetVoid();
		else
			builder->CreateRet(value);
	}

	void CreateBreakCall()
	{
		// check if break has already been inserted.
		if (builder->GetInsertBlock()->getTerminator() != nullptr)
			return;

		for (const auto& stackframe : std::ranges::reverse_view(stackNamedVariable))
		{
			auto resumeBlock = stackframe.resumeBlock;
			if (resumeBlock)
			{
				auto result = builder->CreateBr(resumeBlock);
				// stackNamedVariable.back().ClearBlock();
				break;
			}
		}
	}

	void CreateContinueCall()
	{
		// check if break has already been inserted.
		if (builder->GetInsertBlock()->getTerminator() != nullptr)
			return;

		for (const auto& stackframe : std::ranges::reverse_view(stackNamedVariable))
		{
			auto continueBlock = stackframe.continueBlock;
			if (continueBlock)
			{
				auto result = builder->CreateBr(continueBlock);
				/// stackNamedVariable.back().ClearBlock();
				break;
			}
		}
	}

	bool Compile(std::string filename);
};

