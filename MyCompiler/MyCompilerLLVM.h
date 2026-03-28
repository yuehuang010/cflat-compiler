#pragma once

#include <deque>
#include <ranges>
#include <variant>
#include <format>
#include <unordered_set>

#include <llvm\IR\IRBuilder.h>
#include <llvm\IR\LLVMContext.h>
#include <llvm\IR\Module.h>
#include <llvm\IR\Verifier.h>
#include <llvm\IR\DIBuilder.h>
#include <llvm\Bitcode\BitcodeWriter.h>
#include <llvm\Support\FileSystem.h>
#include <antlr4-runtime.h>
#include <CParser.h>
#include "ArgParser.h"

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
		LogicalAnd, // '&&'
		AndAssignment, // '&='
		XorAssignment, // '^='
		LogicalOr, // '||'
		OrAssignment, // '|='

	};

	struct TypeAndValue
	{
		std::string TypeName;
		std::string VariableName;
		bool Pointer = false;

		bool IsTypeMatch(const TypeAndValue& other) const
		{
			if (TypeName == other.TypeName)
			{
				return true;
			}

			return false;
		}

		bool IsTypePromotion(const TypeAndValue& other) const
		{
			if (Pointer != other.Pointer)
				return false;

			int otherBitSize = other.IsInteger();
			if (otherBitSize != -1)
			{
				int myBitSize = IsInteger();
				if (myBitSize != -1)
					return myBitSize < otherBitSize;
			}

			otherBitSize = other.IsFloatingPoint();
			if (otherBitSize != -1)
			{
				int myBitSize = IsFloatingPoint();
				if (myBitSize != -1)
					return myBitSize < otherBitSize;
			}

			return false;
		}

		int IsInteger() const
		{
			if (TypeName == "char")
				return 8;
			if (TypeName == "short")
				return 16;
			if (TypeName == "int")
				return 32;
			if (TypeName == "long")
				return 64;

			return -1;
		}

		int IsFloatingPoint() const
		{
			if (TypeName == "float")
				return 32;

			if (TypeName == "double")
			{
				return 64;
			}

			return -1;
		}

		std::string ToUniqueString() const
		{
			std::string type = TypeName;

			if (Pointer)
			{
				// Note: LLVM doesn't have void ptr, instead use i8 ptr.
				if (TypeName == "void")
					return "U8Ptr";
				return type + "Ptr";
			}

			return type;
		}

	};

	struct DeclTypeAndValue : public TypeAndValue
	{
		// Used for delayed Initialization
		CParser::InitializerContext* Initializer = nullptr;

		// Used for array
		CParser::AssignmentExpressionContext* ArraySize = nullptr;

		// Used for default parameter values
		CParser::AssignmentExpressionContext* DefaultValue = nullptr;

		bool external = false;
	};

	struct NamedVariable
	{
	public:
		MyCompilerLLVM::TypeAndValue TypeAndValue;
		llvm::Type* BaseType = nullptr;  // The type of the value, even if it is a pointer.
		llvm::Value* Primary = nullptr;  // The value or result
		llvm::Value* Storage = nullptr;  // The container holding the value, used to load or store.

		llvm::Value* GetValue() const
		{
			if (Primary)
				return Primary;

			return Storage;
		}
	};

	struct StructData
	{
		llvm::StructType* StructType;
		std::vector<DeclTypeAndValue> StructFields;
		llvm::Function* Destructor = nullptr;
	};

	class StackState
	{
	public:
		std::unordered_map<std::string, NamedVariable> functionArgument;
		std::unordered_map<std::string, NamedVariable> namedVariable;
		std::unordered_map<std::string, std::string> namespaceAliases;
		llvm::BasicBlock* continueBlock = nullptr; // continue;
		llvm::BasicBlock* resumeBlock = nullptr; // break;
		llvm::BasicBlock* elseBlock = nullptr; // short-circuit condition.
		bool isFunction = false;
		std::string functionName;

		void ClearBlock()
		{
			continueBlock = nullptr;
			resumeBlock = nullptr;
			elseBlock = nullptr;
		}
	};

	class FunctionSymbol
	{
	public:
		std::string UniqueName;
		llvm::Function* Function;
		TypeAndValue ReturnType;
		std::vector<TypeAndValue> Parameters;
		bool Variadic = false;
	};

	struct InterfaceMethod
	{
		std::string Name;
		TypeAndValue ReturnType;
		std::vector<TypeAndValue> Parameters; // excludes the implicit 'this' pointer
	};

	using ConstantVariant = std::variant<bool, char, short, int, int64_t, float, double>;

private:
	std::unique_ptr<llvm::IRBuilder<>> builder;
	std::unique_ptr<llvm::Module> module;
	std::unique_ptr<llvm::LLVMContext> context;

	std::vector<StackState> stackNamedVariable;
	std::unordered_map<std::string, llvm::GlobalVariable*> globalNamedVariable;
	std::unordered_map<std::string, StructData> dataStructures;
	std::unordered_map<std::string, std::vector<FunctionSymbol>> functionTable;
	std::unordered_map<std::string, std::vector<InterfaceMethod>> interfaceTable;
	std::unordered_map<std::string, llvm::Constant*> stringPool;
	std::unordered_set<std::string> namespaceTable;
	std::unordered_map<std::string, std::string> namespaceAliasTable;

	llvm::Function* currentFunction;
	std::string sourceFileName;

	std::unique_ptr<llvm::DIBuilder> diBuilder;
	llvm::DIFile* diFile = nullptr;
	llvm::DICompileUnit* compileUnit = nullptr;
	llvm::DISubprogram* currentSubprogram = nullptr;

private:
	// Create Function Proto or Signature
	llvm::Function* createFunctionProto(std::string name, llvm::FunctionType* returnType)
	{
		auto fn = llvm::Function::Create(returnType, llvm::Function::ExternalLinkage, name, *module);

		llvm::verifyFunction(*fn);

		return fn;
	}

	void EmitDestructorsForScope(const StackState& frame)
	{
		if (builder->GetInsertBlock()->getTerminator() != nullptr)
			return;

		for (const auto& [varName, namedVar] : frame.namedVariable)
		{
			if (namedVar.TypeAndValue.Pointer) continue;
			auto it = dataStructures.find(namedVar.TypeAndValue.TypeName);
			if (it != dataStructures.end() && it->second.Destructor != nullptr)
			{
				auto* fn = it->second.Destructor;
				builder->CreateCall(fn->getFunctionType(), fn, { namedVar.Storage });
			}
		}
	}

	void createFunctionBlock(llvm::Function* fn, const std::string& friendlyName, std::vector<MyCompilerLLVM::TypeAndValue> arguments)
	{
		// all function starts at "entry" block
		auto entry = CreateBasicBlock("entry", fn);
		builder->SetInsertPoint(entry);
		auto& stackState = stackNamedVariable.emplace_back();

		stackState.continueBlock = &fn->back();
		stackState.resumeBlock = &fn->back();
		stackState.isFunction = true;
		stackState.functionName = friendlyName;

		// populate function arguments
		auto itr_nameArg = arguments.begin();
		for (auto& arg : fn->args())
		{
			arg.setName(itr_nameArg->VariableName);

			NamedVariable namedVar
			{
				.TypeAndValue = *itr_nameArg,
				.BaseType = GetType(*itr_nameArg, nullptr, false),
				.Primary = itr_nameArg->Pointer ? nullptr : &arg,
				.Storage = itr_nameArg->Pointer ? &arg : nullptr,
			};

			stackState.functionArgument[itr_nameArg->VariableName] = namedVar;
			itr_nameArg++;
		}

		currentFunction = fn;
	}

	llvm::DIType* GetDIType(const TypeAndValue& typeValue)
	{
		using namespace llvm::dwarf;
		llvm::DIType* baseType = nullptr;

		if (typeValue.TypeName == "int")
			baseType = diBuilder->createBasicType("int", 32, DW_ATE_signed);
		else if (typeValue.TypeName == "char")
			baseType = diBuilder->createBasicType("char", 8, DW_ATE_signed_char);
		else if (typeValue.TypeName == "short")
			baseType = diBuilder->createBasicType("short", 16, DW_ATE_signed);
		else if (typeValue.TypeName == "long")
			baseType = diBuilder->createBasicType("long", 64, DW_ATE_signed);
		else if (typeValue.TypeName == "float")
			baseType = diBuilder->createBasicType("float", 32, DW_ATE_float);
		else if (typeValue.TypeName == "double")
			baseType = diBuilder->createBasicType("double", 64, DW_ATE_float);
		else if (typeValue.TypeName == "bool")
			baseType = diBuilder->createBasicType("bool", 1, DW_ATE_boolean);
		else
			baseType = diBuilder->createUnspecifiedType(typeValue.TypeName);

		if (typeValue.Pointer)
			return diBuilder->createPointerType(baseType, 64);

		return baseType;
	}

	void Init()
	{
		context = std::make_unique<llvm::LLVMContext>();
		module = std::make_unique<llvm::Module>("MyCompiler", *context);
		builder = std::make_unique<llvm::IRBuilder<>>(*context);
	}

	bool VerifyModule()
	{
		std::string errors;
		llvm::raw_string_ostream errorStream(errors);
		if (llvm::verifyModule(*module, &errorStream))
		{
			std::cerr << "Module verification failed:\n" << errorStream.str() << "\n";
			return false;
		}
		return true;
	}

	bool SaveToFile(std::string filename)
	{
		std::error_code errorCode;
		llvm::raw_fd_ostream outLL(filename, errorCode);
		if (errorCode)
		{
			std::cerr << "Error: could not write IR to '" << filename << "': " << errorCode.message() << "\n";
			return false;
		}
		module->print(outLL, nullptr);
		return true;
	}

	bool WriteBitcode(std::string filename)
	{
		std::error_code errorCode;
		llvm::raw_fd_ostream outBC(filename, errorCode, llvm::sys::fs::OF_None);
		if (errorCode)
		{
			std::cerr << "Error: could not write bitcode to '" << filename << "': " << errorCode.message() << "\n";
			return false;
		}
		llvm::WriteBitcodeToFile(*module, outBC);
		return true;
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

	void InitDebugInfo(const std::string& filename, const std::string& directory)
	{
		diBuilder = std::make_unique<llvm::DIBuilder>(*module);
		module->addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
		module->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 4);
		diFile = diBuilder->createFile(filename, directory);
		compileUnit = diBuilder->createCompileUnit(llvm::dwarf::DW_LANG_C99, diFile, "MyCompiler", false, "", 0);
	}

	void FinalizeDebugInfo()
	{
		if (diBuilder)
			diBuilder->finalize();
	}

	void SetCurrentDebugLocation(int line, int col = 0)
	{
		if (!currentSubprogram || !diBuilder) return;
		builder->SetCurrentDebugLocation(llvm::DILocation::get(*context, (unsigned)line, (unsigned)col, currentSubprogram));
	}

	void ClearCurrentSubprogram()
	{
		currentSubprogram = nullptr;
		builder->SetCurrentDebugLocation(llvm::DebugLoc());
	}

	void CreateInterfaceDefinition(const std::string& name, std::vector<InterfaceMethod> methods)
	{
		interfaceTable[name] = std::move(methods);
	}

	void RegisterDestructor(const std::string& structName, llvm::Function* fn)
	{
		dataStructures[structName].Destructor = fn;
	}

	void VerifyInterfaceImplementation(const std::string& structName, const std::string& interfaceName)
	{
		auto ifaceIt = interfaceTable.find(interfaceName);
		if (ifaceIt == interfaceTable.end())
		{
			std::cout << "Unknown interface: '" << interfaceName << "'\n";
			return;
		}

		for (const auto& method : ifaceIt->second)
		{
			bool found = false;
			auto funcIt = functionTable.find(method.Name);
			if (funcIt != functionTable.end())
			{
				for (const auto& sym : funcIt->second)
				{
					// Expect: first param = structName*, remaining params match the interface method's params
					if (sym.Parameters.size() != method.Parameters.size() + 1)
						continue;
					if (sym.Parameters[0].TypeName != structName || !sym.Parameters[0].Pointer)
						continue;

					bool paramsMatch = true;
					for (int i = 0; i < (int)method.Parameters.size(); i++)
					{
						if (sym.Parameters[i + 1].TypeName != method.Parameters[i].TypeName ||
							sym.Parameters[i + 1].Pointer  != method.Parameters[i].Pointer)
						{
							paramsMatch = false;
							break;
						}
					}

					if (paramsMatch) { found = true; break; }
				}
			}

			if (!found)
			{
				std::cout << "Class '" << structName << "' does not implement '"
					<< interfaceName << "::" << method.Name << "'\n";
			}
		}
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

	llvm::AllocaInst* CreateLocalVariable(TypeAndValue typeValue, llvm::Type* autoType = nullptr, llvm::Value* arraySize = nullptr, int line = 0)
	{
		auto type = GetType(typeValue, autoType);
		auto alloc = builder->CreateAlloca(type, arraySize, typeValue.VariableName);
		auto& namedVariable = stackNamedVariable.back().namedVariable[typeValue.VariableName];
		namedVariable.Storage = alloc;
		namedVariable.TypeAndValue = typeValue;
		namedVariable.BaseType = type;

		if (diBuilder && currentSubprogram && line > 0)
		{
			auto diType = GetDIType(typeValue);
			auto diVar = diBuilder->createAutoVariable(currentSubprogram, typeValue.VariableName, diFile, (unsigned)line, diType);
			diBuilder->insertDeclare(alloc, diVar, diBuilder->createExpression(),
				llvm::DILocation::get(*context, (unsigned)line, 0, currentSubprogram),
				builder->GetInsertBlock());
		}

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
		auto destType = GetTypeFromStorage(destination);
		if (destType == builder->getInt1Ty())
		{
			value = builder->CreateICmpNE(value, llvm::ConstantInt::get(value->getType(), 0), "tobool");
		}
		else
		{
			value = Upconvert(value, destType);
		}
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

	llvm::Value* Upconvert(llvm::Value* value, llvm::Value* destination) const
	{
		auto destType = GetTypeFromStorage(destination);
		return Upconvert(value, destType);
	}

	llvm::Value* Upconvert(llvm::Value* value, llvm::Type* destType) const
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

	
	/// <summary>
	/// Compare bit width.
	/// </summary>
	/// <returns>Returns -1 for in compatible type, 0 for same, positive number for possible Upconvert.</returns>
	int CompareUpconvert(llvm::Type* srcType, llvm::Type* destType) const
	{
		if (srcType->isPointerTy() || destType->isPointerTy())
		{
			return 0;
		}

		// auto srcType = value->getType();
		if (srcType->isIntegerTy() && destType->isIntegerTy())
		{
			auto targetSize = destType->getIntegerBitWidth();
			auto srcSize = srcType->getIntegerBitWidth();

			// upconvert is needed
			if (srcSize <= targetSize)
			{
				return targetSize - srcSize;
			}
		}
		else if (srcType->isFloatingPointTy() && destType->isFloatingPointTy())
		{
			auto targetSize = destType->getScalarSizeInBits();
			auto srcSize = srcType->getScalarSizeInBits();

			// upconvert is needed
			if (srcSize <= targetSize)
			{
				return targetSize - srcSize;
			}
		}

		return -1;
	}

	llvm::Type* GetTypeFromStorage(llvm::Value* value) const
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
	llvm::StructType* CreateStructType(std::string name, std::vector<MyCompilerLLVM::DeclTypeAndValue> typeAndValues)
	{
		if (typeAndValues.size() > 0)
		{
			std::vector<llvm::Type*> types;

			for (const auto& typeValue : typeAndValues)
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
			auto existing = dataStructures.find(name);
			if (existing != dataStructures.end())
				return existing->second.StructType;

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
		else if (typeName == "nullptr")
		{
			// create a i8 null pointer
			value = llvm::ConstantPointerNull::get(builder->getInt8Ty()->getPointerTo());
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
		auto it = stringPool.find(text);
		if (it != stringPool.end())
			return it->second;

		auto* gv = builder->CreateGlobalString(text, name);
		auto* ptr = llvm::ConstantExpr::getInBoundsGetElementPtr(
			gv->getValueType(), gv,
			llvm::ArrayRef<llvm::Constant*>{
				llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0),
				llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0)
			});
		stringPool[text] = ptr;
		return ptr;
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
			case Operation::Add:
			{
				return builder->CreateFAdd(left, right);
			}
			case Operation::MinusAssignment:
			case Operation::Subtract:
			{
				return builder->CreateFSub(left, right);
			}
			case Operation::MultiplyAssignment:
			case Operation::Multiply:
			{
				return builder->CreateFMul(left, right);
			}
			case Operation::DivideAssignment:
			case Operation::Divide:
			{
				return builder->CreateFDiv(left, right);
			}
			case Operation::Equal:
			{
				return builder->CreateFCmp(llvm::ICmpInst::FCMP_OEQ, left, right);
			}
			case Operation::NotEqual:
			{
				return builder->CreateFCmp(llvm::ICmpInst::FCMP_ONE, left, right);
			}
			case Operation::Greater:
			{
				return builder->CreateFCmp(llvm::ICmpInst::FCMP_OGT, left, right);
			}
			case Operation::GreaterEqual:
			{
				return builder->CreateFCmp(llvm::ICmpInst::FCMP_OGE, left, right);
			}
			case Operation::Less:
			{
				return builder->CreateFCmp(llvm::ICmpInst::FCMP_OLT, left, right);
			}
			case Operation::LessEqual:
			{
				return builder->CreateFCmp(llvm::ICmpInst::FCMP_OLE, left, right);
			}
			}
		}
		else
		{
			switch (op)
			{
			case Operation::AddAssignment:
			case Operation::Add:
			{
				return builder->CreateAdd(left, right);
			}
			case Operation::MinusAssignment:
			case Operation::Subtract:
			{
				return builder->CreateSub(left, right);
			}
			case Operation::MultiplyAssignment:
			case Operation::Multiply:
			{
				return builder->CreateMul(left, right);
			}
			case Operation::DivideAssignment:
			case Operation::Divide:
			{
				return builder->CreateSDiv(left, right);
			}
			case Operation::Equal:
			{
				return builder->CreateICmp(llvm::ICmpInst::ICMP_EQ, left, right);
			}
			case Operation::NotEqual:
			{
				return builder->CreateICmp(llvm::ICmpInst::ICMP_NE, left, right);
			}
			case Operation::Greater:
			{
				return builder->CreateICmp(llvm::ICmpInst::ICMP_SGT, left, right);
			}
			case Operation::GreaterEqual:
			{
				return builder->CreateICmp(llvm::ICmpInst::ICMP_SGE, left, right);
			}
			case Operation::Less:
			{
				return builder->CreateICmp(llvm::ICmpInst::ICMP_SLT, left, right);
			}
			case Operation::LessEqual:
			{
				return builder->CreateICmp(llvm::ICmpInst::ICMP_SLE, left, right);
			}
			case Operation::LogicalAnd:
			case Operation::AndAssignment:
			{
				return builder->CreateAnd(left, right);
			}
			case Operation::LogicalOr:
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

	llvm::Value* CreateNot(llvm::Value* value)
	{
		return builder->CreateNot(value);
	}

	llvm::BasicBlock* CreateBasicBlock(std::string name, llvm::Function* fn = nullptr)
	{
		if (fn == nullptr)
			fn = currentFunction;

		return llvm::BasicBlock::Create(*context, name, fn);
	}

	llvm::BranchInst* CreateConditionJump(llvm::Value* cond, llvm::BasicBlock* trueBlock, llvm::BasicBlock* falseBlock)
	{
		if (cond->getType()->isPointerTy())
		{
			cond = builder->CreateIsNotNull(cond);
		}

		auto branchInst = builder->CreateCondBr(cond, trueBlock, falseBlock);
		builder->SetInsertPoint(trueBlock);
		return branchInst;
	}

	llvm::PHINode* CreatePHINode(std::string name, int reserve)
	{
		return builder->CreatePHI(builder->getInt1Ty(), reserve, name);
	}

	llvm::Value* CreateSelect(llvm::Value* cond, llvm::Value* falseValue, llvm::Value* trueValue)
	{
		return builder->CreateSelect(cond, trueValue, falseValue);
	}

	/// <summary>
	/// Exit the current BasicBlock and then jump to resumeBlock.
	/// </summary>
	llvm::BranchInst* CreateBlockBreak(llvm::BasicBlock* resumeBlock, bool exitBlackStack)
	{
		if (exitBlackStack)
		{
			EmitDestructorsForScope(stackNamedVariable.back());
			stackNamedVariable.pop_back();
		}

		if (resumeBlock)
		{
			// check if break has already been inserted.
			if (builder->GetInsertBlock()->getTerminator() == nullptr)
				return builder->CreateBr(resumeBlock);
		}

		return nullptr;
	}

	void InitializeBlock(llvm::BasicBlock* block, bool enterBlockStack, llvm::BasicBlock* continueBlock = nullptr, llvm::BasicBlock* resumeBlock = nullptr, llvm::BasicBlock* elseBlock = nullptr)
	{
		if (enterBlockStack)
		{
			auto& stack = stackNamedVariable.emplace_back();
			stack.continueBlock = continueBlock;
			stack.resumeBlock = resumeBlock;
			stack.elseBlock = elseBlock;
		}

		if (block)
		{
			builder->SetInsertPoint(block);
		}
	}

	void CreateFunctionDeclaration(std::string functionName, MyCompilerLLVM::TypeAndValue returnType, std::vector<MyCompilerLLVM::TypeAndValue> arguments, bool external = false, bool varargs = false)
	{
		auto functionType = GetFunctionType(returnType, arguments, varargs);
		std::string mangledName = external ? functionName : ComputeMangledName(functionName, returnType, arguments, varargs);

		if (module->getFunction(mangledName) != nullptr)
			return;

		auto funcCallee = module->getOrInsertFunction(mangledName, functionType);
		llvm::Value* calleeValue = funcCallee.getCallee();

		if (llvm::Function* fn = llvm::dyn_cast<llvm::Function>(calleeValue))
		{
			auto& symList = functionTable[functionName];
			FunctionSymbol funcSym = {
				.UniqueName = mangledName,
				.Function = fn,
				.ReturnType = returnType,
				.Variadic = fn->isVarArg(),
			};

			for (const auto& arg : arguments)
			{
				funcSym.Parameters.push_back(arg);
			}

			symList.push_back(funcSym);
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

	std::string ComputeMangledName(std::string functionName, MyCompilerLLVM::TypeAndValue returnType, std::vector<MyCompilerLLVM::TypeAndValue> arguments, bool varargs = false)
	{
		std::string argumentString = {};

		for (const auto& argument : arguments)
		{
			argumentString += argument.ToUniqueString();
		}

		std::string uniqueName = std::format("_{}_{}_{}_", functionName, returnType.ToUniqueString(), argumentString);

		return uniqueName;
	}

	llvm::Function* CreateFunctionDefinition(std::string functionName, MyCompilerLLVM::TypeAndValue returnType, std::vector<MyCompilerLLVM::TypeAndValue> arguments, bool external = false, bool varargs = false, int line = 0)
	{
		llvm::FunctionType* functionType = GetFunctionType(returnType, arguments, varargs);

		std::string mangledName = external ? functionName : ComputeMangledName(functionName, returnType, arguments, varargs);

		if (functionType == nullptr)
		{
			functionType = llvm::FunctionType::get(builder->getVoidTy(), false);
		}

		auto fn = module->getFunction(mangledName);
		bool alreadyDeclared = false;

		if (fn != nullptr)
		{
			if (!fn->empty())
			{
				std::cout << "Function already exists : " << functionName << "\n";
				__debugbreak();
				return fn;
			}
			// Pre-declared by ForwardRefScanner — reuse the declaration and attach a body.
			alreadyDeclared = true;
		}
		else
		{
			fn = createFunctionProto(mangledName, functionType);
		}

		createFunctionBlock(fn, functionName, arguments);

		if (diBuilder && diFile && line > 0)
		{
			auto funcDIType = diBuilder->createSubroutineType(diBuilder->getOrCreateTypeArray({}));
			auto sp = diBuilder->createFunction(
				diFile, functionName, fn->getName(),
				diFile, (unsigned)line, funcDIType, (unsigned)line,
				llvm::DINode::FlagPrototyped,
				llvm::DISubprogram::SPFlagDefinition
			);
			fn->setSubprogram(sp);
			currentSubprogram = sp;
			builder->SetCurrentDebugLocation(llvm::DILocation::get(*context, (unsigned)line, 0, sp));
		}

		if (!alreadyDeclared)
		{
			auto& symList = functionTable[functionName];
			FunctionSymbol funcSym = {
				.UniqueName = mangledName,
				.Function = fn,
				.ReturnType = returnType,
				.Variadic = fn->isVarArg(),
			};

			for (const auto& arg : arguments)
			{
				funcSym.Parameters.push_back(arg);
			}

			symList.push_back(funcSym);
		}

		return fn;
	}

	llvm::Type* GetType(const MyCompilerLLVM::TypeAndValue& typeAndValue, llvm::Type* autoType = nullptr, bool allowPointer = true) const
	{
		llvm::Type* type = nullptr;
		const auto& typeName = typeAndValue.TypeName;

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

		if (allowPointer && typeAndValue.Pointer)
		{
			// Note: LLVM doesn't have void ptr, instead use i8 ptr.
			if (type->isVoidTy())
				return builder->getInt8Ty()->getPointerTo();
			return type->getPointerTo();
		}

		return type;
	}

	std::pair<std::vector<NamedVariable>, FunctionSymbol> ComputeOverloadFunction(std::vector<std::pair<std::vector<NamedVariable>, FunctionSymbol>> candidates) const
	{
		std::pair<std::vector<NamedVariable>, FunctionSymbol> possibleResult;
		// int score = 0; // 2 for promotionMatch, 1 for implicitMatch

		for (const auto& pair : candidates)
		{
			const auto& [arguments, candidate] = pair;

			if (candidate.Variadic)
			{
				// TODO: Support overload for variadic.
				return pair;
			}

			bool perfectMatch = true;
			bool promotionMatch = true;
			bool implicitMatch = true;

			auto candidateParamItr = candidate.Parameters.begin();
			for (const auto& arg : arguments)
			{
				int result = -1;
				if (arg.TypeAndValue.TypeName != "")
				{
					result = arg.TypeAndValue.IsTypeMatch(*candidateParamItr) ? 0 : -1;
				}
				else
				{
					auto candidateParam = GetType(*candidateParamItr);
					result = CompareUpconvert(arg.BaseType, candidateParam);
				}

				if (result != 0)
				{
					perfectMatch = false;
				}

				if (result < 0)
				{
					promotionMatch = false;
					implicitMatch = false;
				}

				if (!(perfectMatch || promotionMatch || implicitMatch))
				{
					// quick break if matches is no longer possible.
					break;
				}

				++candidateParamItr;
			}

			if (perfectMatch)
				return pair;

			if (promotionMatch || implicitMatch)
			{
				possibleResult = pair;
			}
		}

		return possibleResult;
	}

	std::vector<MyCompilerLLVM::NamedVariable> MatchFunction(const std::vector<MyCompilerLLVM::NamedVariable>& inputArguments, const std::vector<MyCompilerLLVM::TypeAndValue>& targetArguments)
	{
		// Two pass match.
		// 1) Align named arguments
		// 2) Fill remaining argument fills in the blank.

		const size_t inputSize = inputArguments.size();
		if (inputSize != targetArguments.size())
			return {};

		// A map from input to target Argument
		std::vector<int> posMap(inputSize, -1);

		int posIndex = 0;
		for (const auto& input : inputArguments)
		{
			if (input.TypeAndValue.VariableName != "")
			{
				auto it = std::find_if(targetArguments.begin(), targetArguments.end(), [&](const auto& typeAndName)
					{
						return input.TypeAndValue.VariableName == typeAndName.VariableName;
					});

				if (it != targetArguments.end())
				{
					posMap[posIndex] = std::distance(targetArguments.begin(), it);
				}
				else
				{
					// named but none matched.
					__debugbreak();
				}
			}

			posIndex++;
		}

		// Create a used map for the target parameters
		std::vector<bool> usedTargetMap(inputSize);
		for (int pos : posMap)
		{
			if (pos >= 0)
			{
				usedTargetMap[pos] = true;
			}
		}

		// iterate through non-named variables and assign to the next available posMap.
		bool successful = true;
		posIndex = 0;
		int inputIndex = 0;
		int targetIndex = 0;

		for (const auto& input : inputArguments)
		{
			if (posIndex >= inputSize)
			{
				// run out of possible positions
				// TODO: handle variadic arguments.
				successful = false;
				break;
			}

			if (input.TypeAndValue.VariableName == "")
			{
				// search for next empty pos
				while (targetIndex < inputSize)
				{
					if (!usedTargetMap[targetIndex])
					{
						posMap[posIndex] = targetIndex;
						targetIndex++;
						break;
					}
					targetIndex++;
				}
			}

			posIndex++;
		}

		if (successful && std::find(posMap.begin(), posMap.end(), -1) != posMap.end())
		{
			successful = false;
		}

		if (!successful)
		{
			return {};
		}

		// recombine
		std::vector<MyCompilerLLVM::NamedVariable> result(inputSize);
		for (int i = 0; i < inputSize; i++)
		{
			result[posMap[i]] = inputArguments[i];
		}
		return result;
	}

	llvm::Value* CreateFunctionCall2(std::string functionName, std::vector<MyCompilerLLVM::NamedVariable> arguments)
	{
		auto funcSym = functionTable.find(functionName);
		if (funcSym != functionTable.end())
		{
			const auto& candidates = funcSym->second;

			std::vector<std::pair<std::vector<NamedVariable>, FunctionSymbol>> resolvedCandidate;

			for (const auto& candidate : candidates)
			{
				if (candidate.Variadic || (arguments.size() == 0 && candidate.Parameters.size() == 0))
				{
					// TODO: support named parameters variadic
					resolvedCandidate.emplace_back(arguments, candidate);
					break;
				}
				else
				{
					auto matched = MatchFunction(arguments, candidate.Parameters);
					if (matched.size() > 0)
					{
						resolvedCandidate.emplace_back(matched, candidate);
					}
				}
			}

			if (resolvedCandidate.size() > 0)
			{
				const auto& [matched, candidate] = ComputeOverloadFunction(resolvedCandidate);

				// convert parameter to vector of llvm::value*
				std::vector<llvm::Value*> argList;
				auto candParamItr = candidate.Parameters.begin();
				for (const auto& arg : matched)
				{
					if (candParamItr->Pointer)
					{
						argList.push_back(arg.GetValue());
					}
					else
					{
						llvm::Value* value = nullptr;
						if (arg.Primary == nullptr)
						{
							value = CreateLoad(arg.Storage);
						}
						else
						{
							value = arg.Primary;
						}

						argList.push_back(value);
					}
				}

				return CreateFunctionCall(candidate.Function, argList);
			}
		}

		__debugbreak();
		return nullptr;
	}

	llvm::Function* GetFunction(std::string functionName)
	{
		auto functionSym = functionTable.find(functionName);

		if (functionSym != functionTable.end())
		{
			return functionSym->second.front().Function;
		}

		return module->getFunction(functionName);
	}

	NamedVariable GetLocalVariable(std::string name)
	{
		for (const auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
		{
			auto& nameVal = stackFrame.namedVariable;
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
		for (const auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
		{
			const auto& functionArguments = stackFrame.functionArgument;

			if (functionArguments.size() > 0)
			{
				const auto& memberStructName = functionArguments.begin()->first;
				const auto& memberStructInstance = functionArguments.begin()->second.Storage;
				auto truncName = memberStructName.substr(0, memberStructName.size() - 2);
				auto findResult = dataStructures.find(truncName);
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

	/// Returns the implicit 'this' NamedVariable when calling a bare member function
	/// from within a member function body of the same struct. Returns a default
	/// NamedVariable (Storage == nullptr) if not in a member context or not a method.
	NamedVariable GetCurrentMemberThis(const std::string& functionName)
	{
		for (const auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
		{
			const auto& functionArguments = stackFrame.functionArgument;
			if (functionArguments.empty())
				continue;

			const auto& firstArgName = functionArguments.begin()->first;
			if (firstArgName.size() < 2 || firstArgName.substr(firstArgName.size() - 2) != "__")
				break;

			std::string structName = firstArgName.substr(0, firstArgName.size() - 2);

			auto funcIt = functionTable.find(functionName);
			if (funcIt == functionTable.end())
				break;

			for (const auto& sym : funcIt->second)
			{
				if (!sym.Parameters.empty() &&
					sym.Parameters[0].TypeName == structName &&
					sym.Parameters[0].Pointer)
				{
					NamedVariable thisVar = functionArguments.begin()->second;
					thisVar.TypeAndValue.VariableName = "";
					return thisVar;
				}
			}
			break;
		}
		return {};
	}

	NamedVariable GetFunctionArgument(std::string name)
	{
		for (const auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
		{
			const auto& nameVal = stackFrame.functionArgument;
			auto result = nameVal.find(name);

			if (result != nameVal.end())
			{
				return result->second;
			}
		}

		return {};
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

	StructData GetDataStructure(std::string structName)
	{
		auto result = dataStructures.find(structName);
		if (result != dataStructures.end())
		{
			return result->second;
		}

		return {};
	}

	StructData GetDataStructure(llvm::StructType* structType)
	{
		for (const auto& [_, structData] : dataStructures)
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

		// Emit destructors for all scopes from innermost out to the function boundary
		for (auto it = stackNamedVariable.rbegin(); it != stackNamedVariable.rend(); ++it)
		{
			EmitDestructorsForScope(*it);
			if (it->isFunction) break;
		}

		if (value == nullptr)
			builder->CreateRetVoid();
		else
		{
			value = Upconvert(value, currentFunction->getReturnType());
			builder->CreateRet(value);
		}
	}

	llvm::BasicBlock* GetElseBlock()
	{
		for (const auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
		{
			auto elseBlock = stackFrame.elseBlock;
			if (elseBlock || stackFrame.isFunction)
			{
				return elseBlock;
			}
		}

		return nullptr;
	}

	void CreateBreakCall()
	{
		// check if break has already been inserted.
		if (builder->GetInsertBlock()->getTerminator() != nullptr)
			return;

		for (const auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
		{
			auto resumeBlock = stackFrame.resumeBlock;
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

		for (const auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
		{
			auto continueBlock = stackFrame.continueBlock;
			if (continueBlock)
			{
				auto result = builder->CreateBr(continueBlock);
				break;
			}
		}
	}

	std::string GetSourceFileName() const { return sourceFileName; }

	std::string GetCurrentFunctionName() const
	{
		for (const auto& frame : std::ranges::reverse_view(stackNamedVariable))
			if (frame.isFunction) return frame.functionName;
		return "";
	}

	void RegisterNamespace(const std::string& name) { namespaceTable.insert(name); }
	void RegisterNamespaceAlias(const std::string& alias, const std::string& target) { namespaceAliasTable[alias] = target; }
	void RegisterLocalNamespaceAlias(const std::string& alias, const std::string& target)
	{
		if (!stackNamedVariable.empty())
			stackNamedVariable.back().namespaceAliases[alias] = target;
		else
			namespaceAliasTable[alias] = target;
	}
	bool IsNamespace(const std::string& name) const
	{
		for (const auto& frame : std::ranges::reverse_view(stackNamedVariable))
			if (frame.namespaceAliases.count(name)) return true;
		return namespaceTable.count(name) > 0 || namespaceAliasTable.count(name) > 0;
	}
	std::string ResolveNamespace(const std::string& name) const
	{
		for (const auto& frame : std::ranges::reverse_view(stackNamedVariable))
		{
			auto it = frame.namespaceAliases.find(name);
			if (it != frame.namespaceAliases.end()) return it->second;
		}
		auto it = namespaceAliasTable.find(name);
		return it != namespaceAliasTable.end() ? it->second : name;
	}

	std::string GetNameOfCurrentInsertionBlock()
	{
		llvm::BasicBlock* currentBlock = builder->GetInsertBlock();
		return std::format("{}::{}", currentBlock->getParent()->getName().str(), currentBlock->getName().str());
	}

	void DumpCurrentInsertionPoint(std::string prefix = "")
	{
		llvm::BasicBlock* currentBlock = builder->GetInsertBlock();
		llvm::outs() << prefix << "Current insertion block: " << currentBlock->getParent()->getName() << "::" << currentBlock->getName() << "\n";
	}

	bool Compile(const ArgParser& args);
};
