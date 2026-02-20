#pragma once

#include <deque>
#include <ranges>
#include <variant>

#include <llvm\IR\IRBuilder.h>
#include <llvm\IR\LLVMContext.h>
#include <llvm\IR\Module.h>
#include <llvm\IR\Verifier.h>
#include <antlr4-runtime.h>
#include <CParser.h>

class MyCompilerLLVM
{
public:
	enum class Operation
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
		Assignment, //     : '='
		MultiplyAssignment, // '*='
		DivideAssignment, // '/='
		ModAssignment, // '%='
		AddAssignment, // '+='
		MinusAssignment, // '-='
		LeftShiftAssignment, // '<<='
		RightShiftAssignment, // '>>='
		AndAssignment, // '&='
		XorAssignment, // '^='
		OrAssignment, // '|='

	};

	struct TypeAndValue
	{
		std::string TypeName;
		std::string VariableName;

		// Used for delayed Initialization
		CParser::InitializerContext* Initializer = nullptr;
		CParser::AssignmentExpressionContext* ArraySize = nullptr;
		bool Pointer = false;
	};

	struct NamedVariable
	{
		MyCompilerLLVM::TypeAndValue TypeAndValue;
		llvm::Type* BaseType = nullptr;
		llvm::Value* Primary = nullptr;
		llvm::Value* Storage = nullptr;
	};

	struct StructData
	{
		llvm::StructType* StructType;
		std::vector<TypeAndValue> StructFields;
	};

	class StackState
	{
	public:
		std::unordered_map<std::string, llvm::Argument*> functionArgument;
		std::unordered_map<std::string, NamedVariable> namedVariable;
		llvm::BasicBlock* continueBlock = nullptr; // continue;
		llvm::BasicBlock* resumeBlock = nullptr; // break;

		void ClearBlock()
		{
			continueBlock = nullptr;
			resumeBlock = nullptr;
		}
	};

	using ConstantVariant = std::variant<bool, char, short, int, int64_t, float, double>;

private:
	std::unique_ptr<llvm::IRBuilder<>> builder;
	std::unique_ptr<llvm::Module> module;
	std::unique_ptr<llvm::LLVMContext> context;

	std::vector<StackState> stackNamedVariable;
	std::unordered_map<std::string, llvm::GlobalVariable*> globalNamedVariable;
	std::unordered_map<std::string, StructData> dataStructures;

	llvm::Function* currentFunction;

private:
	// Create Function Proto or Signature
	llvm::Function* createFunctionProto(std::string name, llvm::FunctionType* returnType)
	{
		auto fn = llvm::Function::Create(returnType, llvm::Function::ExternalLinkage, name, *module);

		llvm::verifyFunction(*fn);

		return fn;
	}

	void createFunctionBlock(llvm::Function* fn, std::vector<MyCompilerLLVM::TypeAndValue> arguments)
	{
		// all function starts at "entry" block
		auto entry = CreateBasicBlock("entry", fn);
		builder->SetInsertPoint(entry);
		auto& stackState = stackNamedVariable.emplace_back();

		stackState.continueBlock = &fn->back();
		stackState.resumeBlock = &fn->back();

		// populate function arguments
		auto itr_nameArg = arguments.begin();
		for (auto& arg : fn->args())
		{
			arg.setName(itr_nameArg->VariableName);
			stackState.functionArgument[itr_nameArg->VariableName] = &arg;
			itr_nameArg++;
		}

		currentFunction = fn;
	}

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
	Operation ParseOperation(std::string operationText)
	{
		if (operationText == "+") { return Operation::Add; }
		else if (operationText == "*") { return Operation::Multiply; }
		else if (operationText == "-") { return Operation::Subtract; }
		else if (operationText == "/") { return Operation::Divide; }
		else if (operationText == "==") { return Operation::Equal; }
		else if (operationText == "!=") { return Operation::NotEqual; }
		else if (operationText == ">") { return Operation::Greater; }
		else if (operationText == ">=") { return Operation::GreaterEqual; }
		else if (operationText == "<") { return Operation::Less; }
		else if (operationText == "*=") { return Operation::MultiplyAssignment; }
		else if (operationText == "/=") { return Operation::DivideAssignment; }
		else if (operationText == "%=") { return Operation::ModAssignment; }
		else if (operationText == "+=") { return Operation::AddAssignment; }
		else if (operationText == "-=") { return Operation::MinusAssignment; }
		else if (operationText == "<<=") { return Operation::LeftShiftAssignment; }
		else if (operationText == ">>=") { return Operation::RightShiftAssignment; }
		else if (operationText == "&=") { return Operation::AndAssignment; }
		else if (operationText == "^=") { return Operation::XorAssignment; }
		else if (operationText == "|=") { return Operation::OrAssignment; }

		__debugbreak();
		return Operation::None;
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

	llvm::GlobalVariable* CreateGlobalVariable(TypeAndValue typeValue, llvm::Constant* initValue)
	{
		llvm::Type* destinationType = GetType(typeValue);
		if (initValue)
		{
			if (auto intValue = llvm::dyn_cast<llvm::ConstantInt>(initValue))
			{
				if (intValue->getIntegerType()->getBitWidth() != destinationType->getIntegerBitWidth())
				{
					initValue = builder->getIntN(destinationType->getIntegerBitWidth(), intValue->getZExtValue());
				}
			}
			else if (auto fpValue = llvm::dyn_cast<llvm::ConstantFP>(initValue))
			{
				if (fpValue->getType()->getScalarSizeInBits() == destinationType->getScalarSizeInBits())
				{
					initValue = llvm::ConstantFP::get(destinationType, fpValue->getValueAPF());
				}
			}
		}
		else
		{
			// initialize to 0
			initValue = CreateConstant(typeValue.TypeName, "0");
		}

		auto gVar = new llvm::GlobalVariable(
			*module,
			destinationType,
			false, // isConstant
			llvm::GlobalValue::LinkageTypes::ExternalLinkage,
			initValue, // Initial value
			typeValue.VariableName // Name
		);

		globalNamedVariable[typeValue.VariableName] = gVar;

		return gVar;
	}

	llvm::AllocaInst* CreateLocalVariable(TypeAndValue typeValue, llvm::Type* autoType = nullptr, llvm::Value* arraySize = nullptr)
	{
		auto type = GetType(typeValue, autoType);
		auto alloc = builder->CreateAlloca(type, arraySize, typeValue.VariableName);
		auto& namedVariable = stackNamedVariable.back().namedVariable[typeValue.VariableName];
		namedVariable.Storage = alloc;
		namedVariable.TypeAndValue = typeValue;
		namedVariable.BaseType = type;

		return alloc;
	}

	llvm::AllocaInst* CreateAlloca(llvm::Type* type)
	{
		return builder->CreateAlloca(type, nullptr);
	}

	llvm::Value* CreateIncrement(llvm::Value* destination, int amount)
	{
		llvm::LoadInst* loadInst = CreateLoad(destination);

		auto value = llvm::ConstantInt::get(loadInst->getType(), amount);
		auto newValue = CreateOperation(Operation::Add, loadInst, value);
		return builder->CreateStore(newValue, destination);
	}

	/// <summary>
	/// Initialize a constant in the structInstance;
	/// </summary>
	llvm::Value* CreateInsertValue(llvm::Value* structInstance, llvm::Value* newValue, unsigned int index)
	{
		return builder->CreateInsertValue(structInstance, newValue, index);
	}

	/// <summary>
	/// Get the Storage position in a struct.
	/// </summary>
	llvm::Value* CreateStructGEP(llvm::Type* structType, llvm::Value* structAlloc, unsigned int index, std::string variableName = "")
	{
		return builder->CreateStructGEP(structType, structAlloc, index, variableName);
	}

	llvm::Value* CreateGEP(llvm::Type* type, llvm::Value* ptr, llvm::Value* offset, std::string name = "")
	{
		return builder->CreateGEP(type, ptr, offset, name);
	}

	llvm::Value* CreateExtractValue(llvm::Value* structInstance, unsigned int index)
	{
		return builder->CreateExtractValue(structInstance, index);
	}

	llvm::StoreInst* CreateAssignment(llvm::Value* value, llvm::Value* destination)
	{
		value = Upconvert(value, destination);
		return builder->CreateStore(value, destination);
	}

	llvm::LoadInst* CreateLoad(llvm::Value* value)
	{
		llvm::Type* type = GetTypeFromStorage(value);
		return builder->CreateLoad(type, value);
	}

	llvm::LoadInst* CreateLoad(llvm::Type* type, llvm::Value* value)
	{
		return builder->CreateLoad(type, value);
	}

	llvm::Value* Upconvert(llvm::Value* value, llvm::Value* destination)
	{
		auto destType = GetTypeFromStorage(destination);
		return Upconvert(value, destType);
	}

	llvm::Value* Upconvert(llvm::Value* value, llvm::Type* destType)
	{
		auto srcType = value->getType();
		if (srcType->isIntegerTy() && destType->isIntegerTy())
		{
			auto targetSize = destType->getIntegerBitWidth();
			auto srcSize = srcType->getIntegerBitWidth();

			// upconvert is needed
			if (srcSize < targetSize)
			{
				return builder->CreateZExt(value, destType);
			}
		}
		else if (srcType->isFloatingPointTy() && destType->isFloatingPointTy())
		{
			auto targetSize = destType->getScalarSizeInBits();
			auto srcSize = srcType->getScalarSizeInBits();

			// upconvert is needed
			if (srcSize < targetSize)
			{
				return builder->CreateFPExt(value, destType);
			}
		}

		return value;
	}

	llvm::Type* GetTypeFromStorage(llvm::Value* value)
	{
		llvm::Type* type = nullptr;

		if (auto* allocaInst = llvm::dyn_cast<llvm::AllocaInst>(value))
		{
			// If it's a direct alloca, we know the type it was created with
			type = allocaInst->getAllocatedType();
		}
		else if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(value))
		{
			// If it's a GEP (e.g., from CreateStructGEP), get the element type
			type = gep->getResultElementType();

			if (type == nullptr)
			{
				type = gep->getSourceElementType();
			}
		}
		else if (auto* global = llvm::dyn_cast<llvm::GlobalVariable>(value))
		{
			// If it's a global variable
			type = global->getValueType();
		}
		else
		{
			type = value->getType();
		}

		if (type == nullptr)
		{
			__debugbreak();
		}

		return type;
	}

	llvm::Value* CreateCast(llvm::Value* value, llvm::Type* destType)
	{
		/*
		HANDLE_CAST_INST(38, Trunc   , TruncInst   )  // Truncate integers
		HANDLE_CAST_INST(39, ZExt    , ZExtInst    )  // Zero extend integers
		HANDLE_CAST_INST(40, SExt    , SExtInst    )  // Sign extend integers
		HANDLE_CAST_INST(41, FPToUI  , FPToUIInst  )  // floating point -> UInt
		HANDLE_CAST_INST(42, FPToSI  , FPToSIInst  )  // floating point -> SInt
		HANDLE_CAST_INST(43, UIToFP  , UIToFPInst  )  // UInt -> floating point
		HANDLE_CAST_INST(44, SIToFP  , SIToFPInst  )  // SInt -> floating point
		HANDLE_CAST_INST(45, FPTrunc , FPTruncInst )  // Truncate floating point
		HANDLE_CAST_INST(46, FPExt   , FPExtInst   )  // Extend floating point
		HANDLE_CAST_INST(47, PtrToInt, PtrToIntInst)  // Pointer -> Integer
		HANDLE_CAST_INST(48, IntToPtr, IntToPtrInst)  // Integer -> Pointer
		HANDLE_CAST_INST(49, BitCast , BitCastInst )  // Type cast
		HANDLE_CAST_INST(50, AddrSpaceCast, AddrSpaceCastInst)  // addrspace cast
		*/

		/*
		builder->CreateBitCast(value, dest_type, name): Converts a pointer value to a pointer type with a different pointee type. This is the most common use case for "reinterpreting" a pointer.
		builder->CreatePtrToInt(value, dest_int_type, name): Converts a pointer to an integer type. The integer type should typically be intptr_t or uintptr_t to ensure it is large enough to hold the address.
		builder->CreateIntToPtr(value, dest_ptr_type, name): Converts an integer value back to a pointer type.
		builder->CreateAddrSpaceCast(value, dest_ptr_type, name): Converts a pointer in one address space to a pointer in a different address space.
		*/

		auto op = llvm::Instruction::CastOps::IntToPtr;
		return CreateCast(op, value, destType);
	}

	llvm::Value* CreateCast(llvm::Instruction::CastOps op, llvm::Value* value, llvm::Type* destType)
	{
		return builder->CreateCast(op, value, destType);
	}

	// Create StructType or OpaqueStruct
	llvm::StructType* CreateStructType(std::string name, std::vector<MyCompilerLLVM::TypeAndValue> typeAndValues)
	{
		if (typeAndValues.size() > 0)
		{

			std::vector<llvm::Type*> types;

			for (auto typeValue : typeAndValues)
			{
				types.emplace_back(GetType(typeValue));
			}

			auto mystuct = dataStructures.find(name);
			if (mystuct == dataStructures.end())
			{
				llvm::StructType* myStruct = llvm::StructType::create(types, name);
				dataStructures[name].StructType = myStruct;
				dataStructures[name].StructFields = typeAndValues;

				return myStruct;
			}
			
			// existing struct;
			auto& structData = mystuct->second;
			structData.StructFields = typeAndValues;
			structData.StructType->setBody(types);

			return structData.StructType;
		}
		else
		{
			llvm::StructType* opaqueStruct = llvm::StructType::create(*context, name);
			dataStructures[name].StructType = opaqueStruct;
			return opaqueStruct;
		}
	}

	llvm::Value* CreateConstant(ConstantVariant constantVariant)
	{
		llvm::Value* value = nullptr;

		if (auto* v = std::get_if<bool>(&constantVariant))
		{
			if (*v) { value = builder->getTrue(); }
			else { value = builder->getFalse(); }
		}
		else if (auto* v = std::get_if<char>(&constantVariant))
		{
			value = builder->getInt8(*v);
		}
		else if (auto* v = std::get_if<short>(&constantVariant))
		{
			value = builder->getInt16(*v);
		}
		else if (auto* v = std::get_if<int>(&constantVariant))
		{
			value = builder->getInt32(*v);
		}
		else if (auto* v = std::get_if<int64_t>(&constantVariant))
		{
			value = builder->getInt64(*v);
		}
		else if (auto* v = std::get_if<float>(&constantVariant))
		{
			auto floatType = builder->getFloatTy();
			value = llvm::ConstantFP::get(builder->getFloatTy(), *v);
			// auto doubleVal = llvm::ConstantFP::get(builder->getDoubleTy(), *v);
			// value = builder->CreateFPTrunc(doubleVal, floatType);
		}
		else  if (auto* v = std::get_if<double>(&constantVariant))
		{
			double upConvert = *v;
			value = llvm::ConstantFP::get(builder->getDoubleTy(), *v);
		}
		else
		{
			__debugbreak;
		}

		return value;
	}

	llvm::Constant* CreateConstant(std::string typeName, std::string initialValue)
	{
		llvm::Constant* value = nullptr;

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
		else if (typeName == "float")
		{
			float initValue = 0;
			if (!initialValue.empty())
			{
				initValue = std::stof(initialValue);
			}
			value = llvm::ConstantFP::get(builder->getFloatTy(), initValue);
		}
		else if (typeName == "double")
		{
			double initValue = 0;
			if (!initialValue.empty())
			{
				initValue = std::stoi(initialValue);
			}
			value = llvm::ConstantFP::get(builder->getDoubleTy(), initValue);
		}
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

		Operation op = ParseOperation(oper);

		return CreateOperation(op, left, right);
	}

	llvm::Value* CreateOperation(Operation op, llvm::Value* left, llvm::Value* right)
	{
		if (left == nullptr)
			return right;

		// Upconvert both
		left = Upconvert(left, right);
		right = Upconvert(right, left);

		// Note: NSW (No Signed Wrap) and NUS(No Unsigned Wrap)
		if (left->getType()->isFloatingPointTy() || right->getType()->isFloatingPointTy())
		{
			switch (op)
			{
			case Operation::AddAssignment:
			case Operation::Add: {
				return builder->CreateFAdd(left, right);
			}
			case Operation::MinusAssignment:
			case Operation::Subtract: {
				return builder->CreateFSub(left, right);
			}
			case Operation::MultiplyAssignment:
			case Operation::Multiply: {
				return builder->CreateFMul(left, right);
			}
			case Operation::DivideAssignment:
			case Operation::Divide: {
				return builder->CreateFDiv(left, right);
			}
			case Operation::Equal: {
				return builder->CreateFCmp(llvm::ICmpInst::ICMP_EQ, left, right);
			}
			case Operation::NotEqual: {
				return builder->CreateFCmp(llvm::ICmpInst::ICMP_NE, left, right);
			}
			case Operation::Greater: {
				return builder->CreateFCmp(llvm::ICmpInst::ICMP_SGT, left, right);
			}
			case Operation::GreaterEqual: {
				return builder->CreateFCmp(llvm::ICmpInst::ICMP_SGE, left, right);
			}
			case Operation::Less: {
				return builder->CreateFCmp(llvm::ICmpInst::ICMP_SLT, left, right);
			}
			case Operation::LessEqual: {
				return builder->CreateFCmp(llvm::ICmpInst::ICMP_SLE, left, right);
			}
			}
		}
		else
		{
			switch (op)
			{
			case Operation::AddAssignment:
			case Operation::Add: {
				return builder->CreateAdd(left, right);
			}
			case Operation::MinusAssignment:
			case Operation::Subtract: {
				return builder->CreateSub(left, right);
			}
			case Operation::MultiplyAssignment:
			case Operation::Multiply: {
				return builder->CreateMul(left, right);
			}
			case Operation::DivideAssignment:
			case Operation::Divide: {
				return builder->CreateSDiv(left, right);
			}
			case Operation::Equal: {
				return builder->CreateICmp(llvm::ICmpInst::ICMP_EQ, left, right);
			}
			case Operation::NotEqual: {
				return builder->CreateICmp(llvm::ICmpInst::ICMP_NE, left, right);
			}
			case Operation::Greater: {
				return builder->CreateICmp(llvm::ICmpInst::ICMP_SGT, left, right);
			}
			case Operation::GreaterEqual: {
				return builder->CreateICmp(llvm::ICmpInst::ICMP_SGE, left, right);
			}
			case Operation::Less: {
				return builder->CreateICmp(llvm::ICmpInst::ICMP_SLT, left, right);
			}
			case Operation::LessEqual: {
				return builder->CreateICmp(llvm::ICmpInst::ICMP_SLE, left, right);
			}
			case Operation::AndAssignment:
			{
				return builder->CreateAnd(left, right);
			}
			case Operation::OrAssignment:
			{
				return builder->CreateOr(left, right);
			}
			case Operation::XorAssignment:
				return builder->CreateXor(left, right);
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

	llvm::Value* CreateSelect(llvm::Value* cond, llvm::Value* falseValue, llvm::Value* trueValue)
	{
		return builder->CreateSelect(cond, trueValue, falseValue);
	}

	/// <summary>
	/// Exit the current BasicBlock and then jump to resumeBlock.
	/// </summary>
	llvm::BranchInst* CreateBlockBreak(llvm::BasicBlock* resumeBlock, bool exitBlackStack = true)
	{
		if (exitBlackStack)
			stackNamedVariable.pop_back();

		if (resumeBlock)
		{
			// check if break has already been inserted.
			if (builder->GetInsertBlock()->getTerminator() == nullptr)
				return builder->CreateBr(resumeBlock);
		}

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

	void CreateFunctionDeclaration(std::string functionName, llvm::FunctionType* functionType = nullptr)
	{
		if (module->getFunction(functionName) == nullptr)
		{
			module->getOrInsertFunction(functionName, functionType);
		}
		else
		{
			std::cout << "Function already exists:" << functionName << "\n";
			__debugbreak();
		}
	}

	llvm::FunctionType* GetFunctionType(MyCompilerLLVM::TypeAndValue returnType, std::vector<MyCompilerLLVM::TypeAndValue> arguments, bool varargs = false)
	{
		std::vector<llvm::Type*> types;
		types.reserve(arguments.size());

		for (const MyCompilerLLVM::TypeAndValue& arg : arguments)
		{
			types.emplace_back(GetType(arg));
		}

		auto ft = llvm::FunctionType::get(GetType(returnType), types, varargs);
		return ft;
	}

	llvm::Function* CreateFunctionDefinition(std::string functionName, std::vector<MyCompilerLLVM::TypeAndValue> arguments, llvm::FunctionType* functionType)
	{
		auto fn = module->getFunction(functionName);
		if (functionType == nullptr)
		{
			functionType = llvm::FunctionType::get(builder->getVoidTy(), false);
		}

		if (fn == nullptr)
		{
			fn = createFunctionProto(functionName, functionType);
		}
		else
		{
			std::cout << "Function already exists : " << functionName << "\n";
			__debugbreak();
		}

		createFunctionBlock(fn, arguments);

		return fn;
	}

	llvm::Type* GetType(MyCompilerLLVM::TypeAndValue typeAndValue, llvm::Type* autoType = nullptr)
	{
		llvm::Type* type = nullptr;
		auto typeName = typeAndValue.TypeName;

		if (typeName == "void") { type = builder->getVoidTy(); }
		else if (typeName == "char") { type = builder->getInt8Ty(); }
		else if (typeName == "short") { type = builder->getInt16Ty(); }
		else if (typeName == "int") { type = builder->getInt32Ty(); }
		else if (typeName == "long") { type = builder->getInt64Ty(); }
		else if (typeName == "float") { type = builder->getFloatTy(); }
		else if (typeName == "double") { type = builder->getDoubleTy(); }
		else if (typeName == "bool") { type = builder->getInt1Ty(); }
		else if (typeName == "auto" && autoType != nullptr) { type = autoType; }
		else
		{
			auto result = dataStructures.find(typeName);
			if (result != dataStructures.end())
			{
				type = result->second.StructType;
			}
			else
			{
				std::cout << "Unknown value: " << typeName << "\n";
				type = builder->getVoidTy();
			}
		}

		if (typeAndValue.Pointer)
		{
			// Note: LLVM doesn't have void ptr, instead use i8 ptr.
			if (type->isVoidTy())
				return builder->getInt8Ty()->getPointerTo();
			return type->getPointerTo();
		}

		return type;
	}

	llvm::Function* GetFunction(std::string functionName)
	{
		return module->getFunction(functionName);
	}

	NamedVariable GetLocalVariable(std::string name)
	{
		for (const auto& stackframe : std::ranges::reverse_view(stackNamedVariable))
		{
			auto& nameVal = stackframe.namedVariable;
			auto result = nameVal.find(name);

			if (result != nameVal.end())
			{
				return result->second;
			}
		}

		return {};
	}

	/// <summary>
	/// Get the member variable from a member function.
	/// </summary>
	llvm::Value* GetMemberVariable(std::string name)
	{
		for (const auto& stackframe : std::ranges::reverse_view(stackNamedVariable))
		{
			auto functionArguments = stackframe.functionArgument;
			
			if (functionArguments.size() > 0)
			{
				auto memberStructName = functionArguments.begin()->first;
				auto memberStructInstance = functionArguments.begin()->second;
				auto trunName = memberStructName.substr(0, memberStructName.size() - 2);
				auto findResult = dataStructures.find(trunName);
				if (findResult != dataStructures.end())
				{
					int count = 0;
					for (const auto& structField : findResult->second.StructFields)
					{
						if (structField.VariableName == name)
						{
							return CreateStructGEP(findResult->second.StructType, memberStructInstance, count);
						}
						count++;
					}
				}
				return nullptr;
			}
		}

		return nullptr;
	}

	llvm::Argument* GetFunctionArgument(std::string name)
	{
		for (const auto& stackframe : std::ranges::reverse_view(stackNamedVariable))
		{
			auto nameVal = stackframe.functionArgument;
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

	StructData GetDatastructure(std::string structName)
	{
		auto result = dataStructures.find(structName);
		if (result != dataStructures.end())
		{
			return result->second;
		}

		return {};
	}

	StructData GetDatastructure(llvm::StructType* structType)
	{
		for (auto [_, structData] : dataStructures)
		{
			if (structData.StructType == structType)
			{
				return structData;
			}
		}

		return {};
	}

	llvm::Value* CreateFunctionCall(llvm::Function* func, std::vector<llvm::Value*> arg)
	{
		// Perform Default Argument Promotions for Variadic arguments
		if (func->isVarArg())
		{
			std::vector<llvm::Value*> newArg;
			int varArgStart = func->arg_size();
			for (auto value : arg)
			{
				if (varArgStart > 0)
				{
					auto destArgument = func->getArg(func->arg_size() - varArgStart);
					auto srcArg = Upconvert(value, destArgument);
					newArg.push_back(srcArg);
					varArgStart--;
					continue;
				}

				auto valueType = value->getType();

				// Convert 16bit 32bit float to double and non-32bit int to 64bit.
				if (valueType->isIntegerTy(8) /*|| valueType->isIntegerTy(16)*/)
				{
					auto newValue = builder->CreateSExt(value, builder->getInt32Ty(), "conv");
					newArg.push_back(newValue);
				}
				else if (valueType->is16bitFPTy() || valueType->isFloatTy())
				{
					auto newValue = builder->CreateFPExt(value, builder->getDoubleTy(), "conv");
					newArg.push_back(newValue);
				}
				else
				{
					newArg.push_back(value);
				}
			}

			arg = newArg;
		}
		else
		{
			std::vector<llvm::Value*> newArg;

			for (int i = 0; i < arg.size(); i++)
			{
				auto srcArgument = arg[i];
				auto destArgument = func->getArg(i);
				newArg.push_back(Upconvert(srcArgument, destArgument));
			}

			arg = newArg;
		}

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
		{
			value = Upconvert(value, currentFunction->getReturnType());
			builder->CreateRet(value);
		}
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
				break;
			}
		}
	}

	bool Compile(std::string filename);
};
