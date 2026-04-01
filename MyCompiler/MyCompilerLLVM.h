#pragma once

#include <deque>
#include <ranges>
#include <variant>
#include <format>
#include <unordered_set>
#include <cstdlib>

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
        bool IsInterface = false;

        bool IsTypeMatch(const TypeAndValue& other) const
        {
            if (TypeName == other.TypeName)
                return true;

            // C-equivalent signed integer types: char=i8, short=i16, int=i32, long=i64
            int myBits = IsInteger();
            int otherBits = other.IsInteger();
            if (myBits != -1 && myBits == otherBits)
            {
                bool myUnsigned = (IsUnsignedInteger() != -1);
                bool otherUnsigned = (other.IsUnsignedInteger() != -1);
                if (myUnsigned == otherUnsigned)
                    return true;
            }

            return false;
        }

        bool IsTypePromotion(const TypeAndValue& other) const
        {
            if (Pointer != other.Pointer)
                return false;

            // Unsigned integer promotion: u8 < u16 < u32 < u64
            int myUnsigned = IsUnsignedInteger();
            int otherUnsigned = other.IsUnsignedInteger();
            if (myUnsigned != -1 && otherUnsigned != -1)
                return myUnsigned < otherUnsigned;

            // Signed integer promotion: char/i8 < short/i16 < int/i32 < long/i64
            // Do not allow signed <-> unsigned implicit promotion.
            int otherBitSize = other.IsInteger();
            if (otherBitSize != -1 && other.IsUnsignedInteger() == -1)
            {
                int myBitSize = IsInteger();
                if (myBitSize != -1 && IsUnsignedInteger() == -1)
                    return myBitSize < otherBitSize;
            }

            int otherFPSize = other.IsFloatingPoint();
            if (otherFPSize != -1)
            {
                int myFPSize = IsFloatingPoint();
                if (myFPSize != -1)
                    return myFPSize < otherFPSize;
            }

            return false;
        }

        int IsInteger() const
        {
            if (TypeName == "char" || TypeName == "i8" || TypeName == "u8")
                return 8;
            if (TypeName == "short" || TypeName == "i16" || TypeName == "u16")
                return 16;
            if (TypeName == "int" || TypeName == "i32" || TypeName == "u32")
                return 32;
            if (TypeName == "long" || TypeName == "i64" || TypeName == "u64")
                return 64;

            return -1;
        }

        // Returns the bit width if this is an unsigned integer type, -1 otherwise.
        // C equivalents: u8=uint8_t, u16=uint16_t, u32=uint32_t, u64=uint64_t
        int IsUnsignedInteger() const
        {
            if (TypeName == "u8")  return 8;
            if (TypeName == "u16") return 16;
            if (TypeName == "u32") return 32;
            if (TypeName == "u64") return 64;

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
        std::vector<std::string> Interfaces;
        std::unordered_map<std::string, llvm::GlobalVariable*> VTables;
    };

    class StackState
    {
    public:
        std::map<std::string, NamedVariable> functionArgument;
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

    struct ReturnBlockEntry
    {
        CParser::CompoundStatementContext* Body;
        std::vector<DeclTypeAndValue> Params;
        TypeAndValue ReturnType;
    };

    struct ReturnCaptureContext
    {
        llvm::AllocaInst* CaptureAlloca;   // nullptr for void return
        llvm::BasicBlock* ContinuationBlock;
    };

    void SetSourceLocation(int line, int column)
    {
        currentLine = line;
        currentColumn = column;
    }

private:
    int currentLine = 0;
    int currentColumn = 0;

    void LogError(std::string message) const
    {
        std::cout << std::format("[{}:{}] : {}\n", currentLine, currentColumn, message);
        exit(1);
    }

    std::unique_ptr<llvm::IRBuilder<>> builder;
    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::LLVMContext> context;

    std::vector<StackState> stackNamedVariable;
    std::unordered_map<std::string, llvm::GlobalVariable*> globalNamedVariable;
    std::unordered_map<std::string, StructData> dataStructures;
    std::unordered_map<std::string, std::string> enumBackingTypes;
    std::unordered_map<std::string, std::vector<FunctionSymbol>> functionTable;
    std::unordered_map<std::string, std::vector<InterfaceMethod>> interfaceTable;
    std::unordered_map<std::string, std::vector<std::string>> interfaceParents;
    std::unordered_map<std::string, llvm::Constant*> stringPool;
    std::unordered_set<std::string> namespaceTable;
    std::unordered_set<std::string> importedFiles;
    std::string importSearchDir;
    std::unordered_map<std::string, std::string> namespaceAliasTable;
    std::unordered_map<std::string, ReturnBlockEntry> returnBlockTable;
    std::optional<ReturnCaptureContext> returnCapture;

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
        // Explicit-width signed integer types (C equivalents: int8_t, int16_t, int32_t, int64_t)
        else if (typeValue.TypeName == "i8")
            baseType = diBuilder->createBasicType("i8", 8, DW_ATE_signed);
        else if (typeValue.TypeName == "i16")
            baseType = diBuilder->createBasicType("i16", 16, DW_ATE_signed);
        else if (typeValue.TypeName == "i32")
            baseType = diBuilder->createBasicType("i32", 32, DW_ATE_signed);
        else if (typeValue.TypeName == "i64")
            baseType = diBuilder->createBasicType("i64", 64, DW_ATE_signed);
        // Explicit-width unsigned integer types (C equivalents: uint8_t, uint16_t, uint32_t, uint64_t)
        else if (typeValue.TypeName == "u8")
            baseType = diBuilder->createBasicType("u8", 8, DW_ATE_unsigned);
        else if (typeValue.TypeName == "u16")
            baseType = diBuilder->createBasicType("u16", 16, DW_ATE_unsigned);
        else if (typeValue.TypeName == "u32")
            baseType = diBuilder->createBasicType("u32", 32, DW_ATE_unsigned);
        else if (typeValue.TypeName == "u64")
            baseType = diBuilder->createBasicType("u64", 64, DW_ATE_unsigned);
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

        LogError(std::format("unknown operation '{}'", operationText));
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

    void CreateInterfaceDefinition(const std::string& name, const std::vector<std::string>& parentNames, std::vector<InterfaceMethod> methods)
    {
        // Prepend inherited methods from parent interfaces (in order)
        std::vector<InterfaceMethod> inherited;
        for (const auto& parentName : parentNames)
        {
            auto it = interfaceTable.find(parentName);
            if (it == interfaceTable.end())
            {
                std::cout << std::format("Unknown parent interface: '{}'\n", parentName);
                continue;
            }
            for (const auto& m : it->second)
                inherited.push_back(m);
        }
        inherited.insert(inherited.end(), methods.begin(), methods.end());
        interfaceTable[name] = std::move(inherited);
        interfaceParents[name] = parentNames;
    }

    bool IsInterfaceType(const std::string& name) const
    {
        return interfaceTable.count(name) > 0;
    }

    llvm::StructType* GetFatPtrType() const
    {
        const char* fatPtrName = "__iface_fat_ptr";
        if (auto* existing = llvm::StructType::getTypeByName(*context, fatPtrName))
            return existing;
        auto ptrTy = builder->getInt8Ty()->getPointerTo();
        return llvm::StructType::create(*context, { ptrTy, ptrTy }, fatPtrName);
    }

    bool InterfaceInheritsFrom(const std::string& child, const std::string& parent) const
    {
        auto it = interfaceParents.find(child);
        if (it == interfaceParents.end()) return false;
        for (const auto& p : it->second)
        {
            if (p == parent) return true;
            if (InterfaceInheritsFrom(p, parent)) return true;
        }
        return false;
    }

    bool StructImplementsInterface(const std::string& structName, const std::string& ifaceName) const
    {
        auto structIt = dataStructures.find(structName);
        if (structIt == dataStructures.end()) return false;
        for (const auto& iface : structIt->second.Interfaces)
        {
            if (iface == ifaceName) return true;
            if (InterfaceInheritsFrom(iface, ifaceName)) return true;
        }
        return false;
    }

    llvm::GlobalVariable* GetOrCreateVTable(const std::string& structName, const std::string& ifaceName)
    {
        auto& sd = dataStructures[structName];
        auto it = sd.VTables.find(ifaceName);
        if (it != sd.VTables.end()) return it->second;

        auto ifaceIt = interfaceTable.find(ifaceName);
        if (ifaceIt == interfaceTable.end())
        {
            std::cout << std::format("GetOrCreateVTable: unknown interface '{}'\n", ifaceName);
            return nullptr;
        }

        auto ptrTy = builder->getInt8Ty()->getPointerTo();
        std::vector<llvm::Constant*> entries;
        for (const auto& method : ifaceIt->second)
        {
            llvm::Function* fn = nullptr;
            auto funcIt = functionTable.find(method.Name);
            if (funcIt != functionTable.end())
            {
                for (const auto& sym : funcIt->second)
                {
                    if (sym.Parameters.size() > 0 &&
                        sym.Parameters[0].TypeName == structName &&
                        sym.Parameters[0].Pointer)
                    {
                        fn = sym.Function;
                        break;
                    }
                }
            }
            if (fn == nullptr)
            {
                std::cout << std::format("GetOrCreateVTable: '{}' does not implement '{}::{}'\n", structName, ifaceName, method.Name);
                entries.push_back(llvm::ConstantPointerNull::get(ptrTy));
            }
            else
            {
                entries.push_back(llvm::ConstantExpr::getBitCast(fn, ptrTy));
            }
        }

        auto arrTy = llvm::ArrayType::get(ptrTy, entries.size());
        auto vtableConst = llvm::ConstantArray::get(arrTy, entries);
        auto vtableGlobal = new llvm::GlobalVariable(
            *module,
            arrTy,
            true,
            llvm::GlobalValue::InternalLinkage,
            vtableConst,
            structName + "_" + ifaceName + "_vtable"
        );

        sd.VTables[ifaceName] = vtableGlobal;
        return vtableGlobal;
    }

    llvm::Value* BuildInterfaceFatPtr(llvm::GlobalVariable* vtable, llvm::Value* dataPtr)
    {
        auto fatTy = GetFatPtrType();
        auto alloca = builder->CreateAlloca(fatTy);
        auto ptrTy = builder->getInt8Ty()->getPointerTo();

        auto vtableField = builder->CreateStructGEP(fatTy, alloca, 0);
        auto vtableAsPtr = builder->CreateBitCast(vtable, ptrTy);
        builder->CreateStore(vtableAsPtr, vtableField);

        auto dataField = builder->CreateStructGEP(fatTy, alloca, 1);
        auto dataCast = builder->CreateBitCast(dataPtr, ptrTy);
        builder->CreateStore(dataCast, dataField);

        return alloca;
    }

    llvm::Value* CallInterfaceMethod(llvm::Value* ifacePtr, const std::string& ifaceName,
        const std::string& methodName, const std::vector<NamedVariable>& extraArgNVs)
    {
        auto fatTy = GetFatPtrType();
        auto ptrTy = builder->getInt8Ty()->getPointerTo();

        auto vtablePtrField = builder->CreateStructGEP(fatTy, ifacePtr, 0);
        auto vtablePtr = builder->CreateLoad(ptrTy, vtablePtrField);

        auto dataPtrField = builder->CreateStructGEP(fatTy, ifacePtr, 1);
        auto dataPtr = builder->CreateLoad(ptrTy, dataPtrField);

        auto ifaceIt = interfaceTable.find(ifaceName);
        if (ifaceIt == interfaceTable.end())
        {
            LogError(std::format("unknown interface '{}'", ifaceName));
            return nullptr;
        }

        int methodIdx = -1;
        const InterfaceMethod* methodInfo = nullptr;
        for (int i = 0; i < (int)ifaceIt->second.size(); i++)
        {
            if (ifaceIt->second[i].Name == methodName)
            {
                methodIdx = i;
                methodInfo = &ifaceIt->second[i];
                break;
            }
        }
        if (methodIdx < 0)
        {
            LogError(std::format("interface '{}' has no method '{}'", ifaceName, methodName));
            return nullptr;
        }

        auto fnPtrField = builder->CreateGEP(ptrTy, vtablePtr, builder->getInt32(methodIdx));
        auto fnPtr = builder->CreateLoad(ptrTy, fnPtrField);

        llvm::Type* retTy = GetType(methodInfo->ReturnType);
        std::vector<llvm::Type*> paramTypes = { ptrTy };
        for (const auto& p : methodInfo->Parameters)
            paramTypes.push_back(GetType(p));
        auto fnTy = llvm::FunctionType::get(retTy, paramTypes, false);

        std::vector<llvm::Value*> callArgs = { dataPtr };
        for (size_t i = 0; i < extraArgNVs.size() && i < methodInfo->Parameters.size(); i++)
        {
            const auto& nv = extraArgNVs[i];
            const auto& param = methodInfo->Parameters[i];
            if (param.Pointer)
            {
                callArgs.push_back(nv.GetValue());
            }
            else
            {
                auto val = nv.Primary != nullptr ? nv.Primary : CreateLoad(nv.Storage);
                val = Upconvert(val, GetType(param));
                callArgs.push_back(val);
            }
        }

        return builder->CreateCall(fnTy, fnPtr, callArgs);
    }

    void RegisterDestructor(const std::string& structName, llvm::Function* fn)
    {
        dataStructures[structName].Destructor = fn;
    }

    void RegisterStructInterfaces(const std::string& structName, const std::vector<std::string>& interfaces)
    {
        dataStructures[structName].Interfaces = interfaces;
    }

    void VerifyInterfaceImplementation(const std::string& structName, const std::string& interfaceName)
    {
        auto ifaceIt = interfaceTable.find(interfaceName);
        if (ifaceIt == interfaceTable.end())
        {
            std::cout << std::format("Unknown interface: '{}'\n", interfaceName);
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
                            sym.Parameters[i + 1].Pointer != method.Parameters[i].Pointer)
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
                std::cout << std::format("Class '{}' does not implement '{}::{}'\n", structName, interfaceName, method.Name);
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

    // Register a value directly as a named variable without an alloca.
    // Used for pointer-type params in return-block inlining so GetValue() returns the pointer itself.
    void RegisterPrimaryVariable(const TypeAndValue& typeValue, llvm::Value* value)
    {
        auto& namedVariable = stackNamedVariable.back().namedVariable[typeValue.VariableName];
        namedVariable.Primary = value;
        namedVariable.Storage = nullptr;
        namedVariable.TypeAndValue = typeValue;
        namedVariable.BaseType = value->getType();
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
            value = CreateCast(value, destType);
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
        else if (srcType->isIntegerTy() && destType->isPointerTy())
        {
            // Integer 0 assigned to a pointer field — produce a proper null/ptr constant.
            if (auto* constInt = llvm::dyn_cast<llvm::ConstantInt>(value))
                return llvm::ConstantExpr::getIntToPtr(constInt, destType);
            return builder->CreateIntToPtr(value, destType);
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
            LogError(std::format("GetTypeFromStorage could not resolve type for value '{}'", value->getName().str()));
        }

        return type;
    }

    llvm::Value* CreateCast(llvm::Value* value, llvm::Type* destType, bool isSigned = false)
    {
        auto srcType = value->getType();

        if (srcType == destType)
            return value;

        // Integer <-> Integer
        if (srcType->isIntegerTy() && destType->isIntegerTy())
        {
            unsigned srcBits = srcType->getIntegerBitWidth();
            unsigned dstBits = destType->getIntegerBitWidth();
            if (dstBits < srcBits)
                return builder->CreateTrunc(value, destType);
            else if (isSigned)
                return builder->CreateSExt(value, destType);
            else
                return builder->CreateZExt(value, destType);
        }

        // Float <-> Float
        if (srcType->isFloatingPointTy() && destType->isFloatingPointTy())
        {
            if (destType->getScalarSizeInBits() < srcType->getScalarSizeInBits())
                return builder->CreateFPTrunc(value, destType);
            else
                return builder->CreateFPExt(value, destType);
        }

        // Integer -> Float
        if (srcType->isIntegerTy() && destType->isFloatingPointTy())
        {
            if (isSigned)
                return builder->CreateSIToFP(value, destType);
            else
                return builder->CreateUIToFP(value, destType);
        }

        // Float -> Integer
        if (srcType->isFloatingPointTy() && destType->isIntegerTy())
        {
            if (isSigned)
                return builder->CreateFPToSI(value, destType);
            else
                return builder->CreateFPToUI(value, destType);
        }

        // Pointer -> Integer
        if (srcType->isPointerTy() && destType->isIntegerTy())
            return builder->CreatePtrToInt(value, destType);

        // Integer -> Pointer
        if (srcType->isIntegerTy() && destType->isPointerTy())
            return builder->CreateIntToPtr(value, destType);

        // Pointer -> Pointer (reinterpret)
        if (srcType->isPointerTy() && destType->isPointerTy())
            return builder->CreateBitCast(value, destType);

        // Fallback: BitCast for same-size reinterpretation
        return builder->CreateBitCast(value, destType);
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
            if (structData.StructType->isOpaque())
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
            LogError(std::format("CreateConstant encountered unsupported variant type (index {})", constantVariant.index()));
        }

        return value;
    }

    llvm::Constant* CreateConstant(std::string typeName, std::string initialValue)
    {
        llvm::Constant* value = nullptr;

        if (typeName == "char" || typeName == "i8" || typeName == "u8")
        {
            int initValue = 0;
            if (!initialValue.empty())
            {
                initValue = std::stoi(initialValue);
            }

            value = builder->getInt8(initValue);
        }
        else if (typeName == "short" || typeName == "i16" || typeName == "u16")
        {
            int initValue = 0;
            if (!initialValue.empty())
            {
                initValue = std::stoi(initialValue);
            }

            value = builder->getInt16(initValue);
        }
        else if (typeName == "int" || typeName == "i32" || typeName == "u32")
        {
            int initValue = 0;
            if (!initialValue.empty())
            {
                initValue = std::stoi(initialValue);
            }

            value = builder->getInt32(initValue);
        }
        else if (typeName == "long" || typeName == "i64" || typeName == "u64")
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
            std::cout << std::format("Unknown value: {}\n", typeName);
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

        LogError(std::format("unhandled operation {}", static_cast<int>(op)));
        return right;
    }

    llvm::Value* CreateNot(llvm::Value* value)
    {
        return builder->CreateNot(value);
    }

    llvm::Value* CreateNeg(llvm::Value* value)
    {
        if (value->getType()->isFloatingPointTy())
            return builder->CreateFNeg(value);
        return builder->CreateNeg(value);
    }

    llvm::BasicBlock* CreateBasicBlock(std::string name, llvm::Function* fn = nullptr)
    {
        if (fn == nullptr)
            fn = currentFunction;

        return llvm::BasicBlock::Create(*context, name, fn);
    }

    void SwitchToBlock(llvm::BasicBlock* block)
    {
        builder->SetInsertPoint(block);
    }

    llvm::BranchInst* CreateJump(llvm::BasicBlock* block)
    {
        if (block && builder->GetInsertBlock()->getTerminator() == nullptr)
            return builder->CreateBr(block);
        return nullptr;
    }

    /// Returns the LLVM return type for the first overload of a function, or nullptr if not found.
    llvm::Type* GetFunctionReturnType(const std::string& functionName) const
    {
        auto it = functionTable.find(functionName);
        if (it == functionTable.end() || it->second.empty())
            return nullptr;
        return GetType(it->second.front().ReturnType);
    }

    llvm::SwitchInst* CreateSwitchInst(llvm::Value* cond, llvm::BasicBlock* defaultBlock, unsigned numCases)
    {
        if (!cond->getType()->isIntegerTy())
            LogError("switch expression must be an integer type");
        return builder->CreateSwitch(cond, defaultBlock, numCases);
    }

    llvm::ConstantInt* CoerceCaseValue(llvm::ConstantInt* val, llvm::Type* switchType)
    {
        return llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(switchType), val->getSExtValue(), true);
    }

    llvm::BranchInst* CreateConditionJump(llvm::Value* cond, llvm::BasicBlock* trueBlock, llvm::BasicBlock* falseBlock)
    {
        if (cond->getType()->isPointerTy())
        {
            cond = builder->CreateIsNotNull(cond);
        }
        else if (!cond->getType()->isIntegerTy(1))
        {
            // Convert non-boolean integer to i1 (nonzero = true)
            cond = builder->CreateICmpNE(cond, llvm::ConstantInt::get(cond->getType(), 0), "tobool");
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
                LogError(std::format("function already exists: '{}'", functionName));
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

        // Resolve enum type names to their backing type if registered
        std::string resolvedTypeName = typeName;
        if (!resolvedTypeName.empty())
        {
            auto it = enumBackingTypes.find(resolvedTypeName);
            if (it != enumBackingTypes.end())
                resolvedTypeName = it->second;
        }

        if (resolvedTypeName == "void") { type = builder->getVoidTy(); }
        else if (resolvedTypeName == "char" || resolvedTypeName == "i8" || resolvedTypeName == "u8") { type = builder->getInt8Ty(); }
        else if (resolvedTypeName == "short" || resolvedTypeName == "i16" || resolvedTypeName == "u16") { type = builder->getInt16Ty(); }
        else if (resolvedTypeName == "int" || resolvedTypeName == "i32" || resolvedTypeName == "u32") { type = builder->getInt32Ty(); }
        else if (resolvedTypeName == "long" || resolvedTypeName == "i64" || resolvedTypeName == "u64") { type = builder->getInt64Ty(); }
        else if (resolvedTypeName == "float") { type = builder->getFloatTy(); }
        else if (resolvedTypeName == "double") { type = builder->getDoubleTy(); }
        else if (resolvedTypeName == "bool") { type = builder->getInt1Ty(); }
        else if (resolvedTypeName == "auto" && autoType != nullptr) { type = autoType; }
        else
        {
            // Check if it is an interface type first
            if (interfaceTable.count(typeName) > 0)
            {
                // Interface type: represented as a fat pointer {i8*, i8*}
                // Return the fat ptr struct type (pointer applied below if needed)
                type = GetFatPtrType();
            }
            else
            {
                auto result = dataStructures.find(typeName);
                if (result != dataStructures.end())
                {
                    type = result->second.StructType;
                }
                else
                {
                    std::cout << std::format("Unknown value: {}\n", typeName);
                    type = builder->getVoidTy();
                }
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
                    // Resolve enum types for comparison: if either arg or param is an enum, use its backing type
                    auto resolveName = [&](const std::string& tn) -> std::string
                        {
                            if (tn.empty()) return tn;
                            auto it = enumBackingTypes.find(tn);
                            return (it != enumBackingTypes.end()) ? it->second : tn;
                        };

                    MyCompilerLLVM::TypeAndValue tmpArg = arg.TypeAndValue;
                    MyCompilerLLVM::TypeAndValue tmpParam = *candidateParamItr;

                    tmpArg.TypeName = resolveName(tmpArg.TypeName);
                    tmpParam.TypeName = resolveName(tmpParam.TypeName);

                    if (tmpArg.IsTypeMatch(tmpParam))
                        result = 0;
                    else if (tmpArg.IsTypePromotion(tmpParam))
                        result = tmpArg.IsInteger();  // positive: widening
                    else
                    {
                        // Same signedness group: int<->i32, long<->i64, char<->i8, etc.
                        // Same width  -> perfect match (result=0): int==i32, long==i64.
                        // Diff width  -> implicit conversion (result=1): i64->int, etc.
                        int myBits = tmpArg.IsInteger();
                        int otherBits = tmpParam.IsInteger();
                        bool myUnsigned = tmpArg.IsUnsignedInteger() != -1;
                        bool otherUnsigned = tmpParam.IsUnsignedInteger() != -1;
                        if (myBits != -1 && otherBits != -1 && myUnsigned == otherUnsigned)
                            result = (myBits == otherBits) ? 0 : 1;

                        if (result < 0)
                        {
                            int myFP = tmpArg.IsFloatingPoint();
                            int otherFP = tmpParam.IsFloatingPoint();
                            if (myFP != -1 && otherFP != -1)
                                result = (myFP == otherFP) ? 0 : 1;
                        }

                        // Interface upcast using original names (interfaces are not enums)
                        if (result < 0 && candidateParamItr->IsInterface && candidateParamItr->Pointer &&
                            !arg.TypeAndValue.IsInterface && !arg.TypeAndValue.Pointer &&
                            StructImplementsInterface(arg.TypeAndValue.TypeName, candidateParamItr->TypeName))
                        {
                            result = 0;
                        }
                    }
                }
                else
                {
                    auto candidateParam = GetType(*candidateParamItr);
                    result = CompareUpconvert(arg.BaseType, candidateParam);

                    // Interface upcast: struct value (TypeName empty, BaseType is struct) to interface* param
                    if (result < 0 && candidateParamItr->IsInterface && candidateParamItr->Pointer && arg.BaseType)
                    {
                        if (auto* st = llvm::dyn_cast<llvm::StructType>(arg.BaseType))
                        {
                            auto structName = st->getName().str();
                            if (!structName.empty() && StructImplementsInterface(structName, candidateParamItr->TypeName))
                                result = 0;
                        }
                    }
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
                    LogError(std::format("named argument '{}' does not match any parameter", input.TypeAndValue.VariableName));
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
        if (funcSym == functionTable.end())
        {
            LogError(std::format("unknown function '{}'", functionName));
            return nullptr;
        }

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

        const auto& [matched, candidate] = ComputeOverloadFunction(resolvedCandidate);

        if (candidate.Function == nullptr)
        {
            std::cout << std::format("[{}:{}] : no overload of '{}' matches the given arguments.\n", currentLine, currentColumn, functionName);

            // Print the input arguments
            std::cout << std::format("  Call arguments ({}):\n", arguments.size());
            for (size_t i = 0; i < arguments.size(); i++)
            {
                const auto& arg = arguments[i];
                std::string typeName = arg.TypeAndValue.TypeName;
                if (typeName.empty() && arg.BaseType)
                {
                    std::string typeStr;
                    llvm::raw_string_ostream rso(typeStr);
                    arg.BaseType->print(rso);
                    typeName = typeStr;
                }
                std::string name = arg.TypeAndValue.VariableName.empty() ? "<unnamed>" : arg.TypeAndValue.VariableName;
                std::cout << std::format("    [{}] {}{} {}\n", i, typeName, arg.TypeAndValue.Pointer ? "*" : "", name);
            }

            // Print all candidates so the user can see what was available
            std::cout << std::format("  Candidates ({}):\n", candidates.size());
            for (const auto& c : candidates)
            {
                std::string paramList;
                for (size_t i = 0; i < c.Parameters.size(); i++)
                {
                    if (i > 0) paramList += ", ";
                    const auto& p = c.Parameters[i];
                    paramList += std::format("{}{} {}", p.TypeName, p.Pointer ? "*" : "", p.VariableName);
                }
                std::cout << std::format("    {}({})\n", c.UniqueName, paramList);
            }

            // If exactly one resolved candidate passed MatchFunction, show per-argument type comparison
            if (resolvedCandidate.size() == 1)
            {
                const auto& [resolvedArgs, resolvedSym] = resolvedCandidate.front();
                std::cout << std::format("  Argument mismatch detail (single resolved candidate: {}):\n", resolvedSym.UniqueName);
                size_t count = std::max(resolvedArgs.size(), resolvedSym.Parameters.size());
                for (size_t i = 0; i < count; i++)
                {
                    std::string argDesc = i < resolvedArgs.size() ? resolvedArgs[i].TypeAndValue.TypeName : "<missing>";
                    if (argDesc.empty() && i < resolvedArgs.size() && resolvedArgs[i].BaseType)
                    {
                        std::string typeStr;
                        llvm::raw_string_ostream rso(typeStr);
                        resolvedArgs[i].BaseType->print(rso);
                        argDesc = typeStr;
                    }
                    bool argPtr = i < resolvedArgs.size() && resolvedArgs[i].TypeAndValue.Pointer;
                    std::string paramDesc = i < resolvedSym.Parameters.size() ? resolvedSym.Parameters[i].TypeName : "<missing>";
                    bool paramPtr = i < resolvedSym.Parameters.size() && resolvedSym.Parameters[i].Pointer;
                    std::cout << std::format("    [{}] arg={}{}  param={}{}\n", i, argDesc, argPtr ? "*" : "", paramDesc, paramPtr ? "*" : "");
                }
            }

            exit(1);
            return nullptr;
        }

        // convert parameter to vector of llvm::value*
        std::vector<llvm::Value*> argList;
        auto candParamItr = candidate.Parameters.begin();
        for (const auto& arg : matched)
        {
            if (candParamItr->IsInterface && candParamItr->Pointer && !arg.TypeAndValue.IsInterface)
            {
                // Derive struct name from TypeName if available, else from BaseType
                std::string structName = arg.TypeAndValue.TypeName;
                if (structName.empty() && arg.BaseType)
                {
                    if (auto* st = llvm::dyn_cast<llvm::StructType>(arg.BaseType))
                        structName = st->getName().str();
                }

                // Build fat pointer: vtable + data ptr
                auto vtable = GetOrCreateVTable(structName, candParamItr->TypeName);
                llvm::Value* dataPtr = arg.Storage;
                if (dataPtr == nullptr)
                {
                    // Materialize a pointer to the struct value
                    auto structTy = arg.BaseType ? arg.BaseType : GetType(arg.TypeAndValue);
                    auto tempAlloca = builder->CreateAlloca(structTy);
                    builder->CreateStore(arg.Primary, tempAlloca);
                    dataPtr = tempAlloca;
                }
                argList.push_back(BuildInterfaceFatPtr(vtable, dataPtr));
            }
            else if (candParamItr->Pointer)
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

                // Upconvert to match the declared parameter type (e.g. i16 -> i32).
                value = Upconvert(value, GetType(*candParamItr));
                argList.push_back(value);
            }
            if (candParamItr != candidate.Parameters.end() - 1)
                ++candParamItr;
        }

        return CreateFunctionCall(candidate.Function, argList);
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
    NamedVariable GetMemberVariable(std::string name)
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
                            NamedVariable namedVar;
                            namedVar.Storage = CreateStructGEP(findResult->second.StructType, memberStructInstance, count);
                            namedVar.Primary = CreateLoad(namedVar.Storage);
                            namedVar.BaseType = namedVar.Primary->getType();
                            namedVar.TypeAndValue = structField;
                            return namedVar;
                        }
                        count++;
                    }
                }
                return {};
            }
        }

        return {};
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
                if (valueType->isIntegerTy(8) || valueType->isIntegerTy(16))
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

        // If we are inlining a return-block, capture the value and branch to continuation.
        if (returnCapture)
        {
            if (value != nullptr && returnCapture->CaptureAlloca != nullptr)
                builder->CreateStore(value, returnCapture->CaptureAlloca);
            builder->CreateBr(returnCapture->ContinuationBlock);
            return;
        }

        if (value == nullptr)
            builder->CreateRetVoid();
        else
        {
            value = Upconvert(value, currentFunction->getReturnType());
            builder->CreateRet(value);
        }
    }

    void BeginReturnCapture(llvm::AllocaInst* captureAlloca, llvm::BasicBlock* continuationBlock)
    {
        returnCapture = { captureAlloca, continuationBlock };
    }

    void EndReturnCapture()
    {
        returnCapture.reset();
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

    bool IsBlockTerminated()
    {
        return builder->GetInsertBlock()->getTerminator() != nullptr;
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

    void RegisterReturnBlock(const std::string& name, CParser::CompoundStatementContext* body, std::vector<DeclTypeAndValue> params, TypeAndValue returnType)
    {
        returnBlockTable[name] = { body, std::move(params), returnType };
    }

    const ReturnBlockEntry* GetReturnBlock(const std::string& name) const
    {
        auto it = returnBlockTable.find(name);
        return it != returnBlockTable.end() ? &it->second : nullptr;
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
    void RegisterEnumBackingType(const std::string& enumName, const std::string& backingType)
    {
        enumBackingTypes[enumName] = backingType;
    }
    std::string GetEnumBackingType(const std::string& enumName) const
    {
        auto it = enumBackingTypes.find(enumName);
        return it != enumBackingTypes.end() ? it->second : std::string();
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
    bool CompileImportedFile(const std::string& importingFilePath, const std::string& importFilename);
};
