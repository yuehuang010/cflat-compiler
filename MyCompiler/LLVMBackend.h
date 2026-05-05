#pragma once
// ============================================================
// LLVMBackend.h — LLVM IR backend, type system, symbol tables
// ============================================================
// SECTION      LINE     DESCRIPTION
// ───────────────────────────────────────────────────────────
// §1           40-370   Public enums/structs (Operation, TypeAndValue, ...)
// §2           372-414  Private member data
// §3           416-1134 Private methods
// §4           1136+    Public methods (~160+ methods)
//   §4.1               Debug info
//   §4.2               Block control
//   §4.3               Interface system
//   §4.4               Variable management
//   §4.5               IR emission
//   §4.6               Control flow
//   §4.7               Function system
//   §4.8               Lookup / name resolution
// ============================================================

#include <deque>
#include <functional>
#include <ranges>
#include <variant>
#include <format>
#include <unordered_set>
#include <cstdlib>
#include <io.h>

#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include <llvm\IR\IRBuilder.h>
#include <llvm\IR\Intrinsics.h>
#include <llvm\IR\LLVMContext.h>
#include <llvm\IR\Module.h>
#include <llvm\IR\Verifier.h>
#include <llvm\IR\DIBuilder.h>
#include <llvm\IR\LegacyPassManager.h>
#include <llvm\Bitcode\BitcodeWriter.h>
#include <llvm\Support\FileSystem.h>
#include <llvm\Support\MemoryBuffer.h>
#include <llvm\Support\Path.h>
#include <llvm\Support\Program.h>
#include <llvm\Support\TargetSelect.h>
#include <llvm\Target\TargetMachine.h>
#include <llvm\MC\TargetRegistry.h>
#include <llvm\TargetParser\Host.h>
#pragma warning(pop)
#include <antlr4-runtime.h>
#include <CFlatParser.h>
#include <CFlatLexer.h>
#include <fstream>
#include "ArgParser.h"
#include "CompilerManager.h"

#include "LspSymbolIndex.h"
#include "CFlatErrorListener.h"

struct ExpectedErrorReceived {};

struct CompilerAbortException {
    std::string message;
    std::string file;
    size_t line;
    size_t column;
};

class LLVMBackend
{
public:
    enum class Operation
    {
        None,
        Add,
        Subtract,
        Multiply,
        Divide,
        Modulo, // %
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
        ShiftLeft, // '<<'
        ShiftRight, // '>>'
        BitwiseAnd, // '&'
        BitwiseXor, // '^'
        BitwiseOr,  // '|'
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
        bool ElemPointer = false; // true when this is T** (pointer to pointer), e.g. T* field where T is a pointer type
        bool IsInterface = false;
        bool IsInterfacePointer = false; // true when T is interface AND this is a pointer TO that fat-ptr (e.g. T* field where T=IMessage, or channel<IMessage*>)
        bool IsNullable = false;
        bool IsMove = false;     // parameter declared with 'move' — function takes ownership
        bool IsBond = false;     // parameter declared with 'bond' — return value borrows from this parameter; return must not outlive it

        // Function pointer fields (IsFunctionPointer == true)
        bool IsFunctionPointer = false;
        std::string FuncPtrReturnTypeName;
        bool FuncPtrReturnPointer = false;
        struct FuncPtrParam { std::string TypeName; bool Pointer = false; };
        std::vector<FuncPtrParam> FuncPtrParams;

        uint64_t ConstArraySize = 0; // non-zero for C-style fixed arrays: char buf[N] in struct fields

        bool IsPrimitive() const
        {
            return IsInteger() != -1 || IsUnsignedInteger() != -1 || IsFloatingPoint() != -1
                || TypeName == "bool" || TypeName == "void";
        }

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
            if (IsFunctionPointer)
            {
                std::string s = "funcptr_" + FuncPtrReturnTypeName + (FuncPtrReturnPointer ? "Ptr" : "");
                for (const auto& p : FuncPtrParams)
                    s += "_" + p.TypeName + (p.Pointer ? "Ptr" : "");
                return s;
            }

            std::string type = TypeName;

            if (Pointer)
            {
                // Note: LLVM doesn't have void ptr, instead use i8 ptr.
                if (TypeName == "void")
                    return ElemPointer ? "U8PtrPtr" : "U8Ptr";
                return ElemPointer ? type + "PtrPtr" : type + "Ptr";
            }

            return type;
        }

    };

    struct AnnotationValue
    {
        std::string Name;   // e.g. "JsonName"
        std::string Value;  // raw arg text, empty for no-arg annotations
    };

    struct DeclTypeAndValue : public TypeAndValue
    {
        // Used for delayed Initialization
        CFlatParser::InitializerContext* Initializer = nullptr;

        // Used for array
        CFlatParser::AssignmentExpressionContext* ArraySize = nullptr;

        // Used for default parameter values
        CFlatParser::InitializerContext* DefaultValue = nullptr;

        bool external = false;
        bool threadLocal = false;

        std::vector<AnnotationValue> Annotations;
    };

    struct NamedVariable
    {
    public:
        LLVMBackend::TypeAndValue TypeAndValue;
        llvm::Type* BaseType = nullptr;  // The type of the value, even if it is a pointer.
        llvm::Value* Primary = nullptr;  // The value or result
        llvm::Value* Storage = nullptr;  // The container holding the value, used to load or store.
        bool IsOwning = false;           // true for move parameters, new-allocated locals, and any owned pointer — freed on scope exit
        bool IsNewAllocated = false;     // true only for 'new'-allocated locals — enables refcount on field escape (cleared on null-source transfer)
        bool IsOwningString = false;     // true when a string local owns its heap buffer — destructor called on scope exit
        bool IsOwningStruct = false;     // true for move parameters of struct types with destructors — destructor called on scope exit
        bool IsMoved = false;            // compile-time: true after this variable's ownership was transferred via a move call
        bool IsBonded = false;           // compile-time: true when this variable holds a bonded (borrowed) return value
        std::vector<std::string> BondedSources; // names of bond parameters this value borrows from
        llvm::Value* RefCountStorage = nullptr; // lazy i32 alloca at function entry; non-null only when pointer escaped to a field
        std::string CallerName;          // the variable's name at the call site, for move tracking
        int IdentifierLine = 0;          // source location for use-after-move error reporting
        int IdentifierColumn = 0;

        llvm::Value* GetValue() const
        {
            if (Primary)
                return Primary;

            return Storage;
        }
    };

    // Lightweight expression result: pairs an LLVM value with its signedness.
    // operator llvm::Value*() lets it substitute for Value* transparently at call sites.
    struct TypedValue
    {
        llvm::Value* value      = nullptr;
        bool         isUnsigned = false;
        llvm::Type*  elemType   = nullptr;  // non-null when value is a pointer (enables ptr+int GEP)

        TypedValue() = default;
        TypedValue(llvm::Value* v, bool u = false) : value(v), isUnsigned(u) {}

        operator llvm::Value*()   const { return value; }
        llvm::Value* operator->() const { return value; }
        explicit operator bool()  const { return value != nullptr; }
    };

    struct StructData
    {
        llvm::StructType* StructType;
        std::vector<DeclTypeAndValue> StructFields;
        llvm::Function* Destructor = nullptr;
        std::vector<std::string> Interfaces;      // Only used by classes (structs have empty list)
        std::unordered_map<std::string, llvm::GlobalVariable*> VTables; // Only used by classes
        llvm::GlobalVariable* typeDescriptor = nullptr; // unique per-struct global for type identity
    };

    struct ProgramData
    {
        llvm::StructType* StructType = nullptr;
        std::vector<DeclTypeAndValue> ConfigFields;
        llvm::Function* Destructor = nullptr;
        llvm::Function* MainFunction = nullptr;
        llvm::Function* RunFunction = nullptr;
        llvm::Function* TrampolineFunction = nullptr;  // __program_run_Name
        llvm::StructType* RunArgsType = nullptr;       // { Name*, list__string }
        unsigned ExitCodeFieldIndex = 0;               // struct field index of exitCode
        unsigned ThreadFieldIndex = 0;                 // struct field index of _thread
        unsigned AllocatorFieldIndex = 0;              // struct field index of _allocator (IAllocator fat-ptr)
        unsigned OnStdoutFieldIndex = 0;               // struct field index of onStdout (function<void(char*)>)
        unsigned OnStdinFieldIndex = 0;                // struct field index of onStdin (function<char*()>)
        unsigned OnStdinReturnFieldIndex = 0;          // struct field index of onStdinReturn (function<void(char*)>)
        unsigned InboxFieldIndex = 0;                  // struct field index of inbox (channel<IMessage>)
        unsigned StopSourceFieldIndex = 0;             // struct field index of _stop_source (stop_source)
        unsigned TrackHandlesFieldIndex = 0;           // struct field index of trackHandles (bool)
        unsigned OutFieldIndex      = (unsigned)-1;     // struct field index of _out (stream*); -1 when stream.cb not imported
        unsigned InStreamFieldIndex = (unsigned)-1;     // struct field index of _in  (stream*); -1 when stream.cb not imported
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

        // Set when this scope was entered via a `lock` statement.
        // unlock() is called on scope exit (return, or normal block close).
        struct LockCleanup
        {
            llvm::Function* UnlockFn = nullptr;
            llvm::Value*    MutexPtr = nullptr; // pointer to the mutex struct
        };
        std::optional<LockCleanup> lockCleanup;

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
        bool ReturnsOwned = false; // true when the function returns an owned value (heap string or owned pointer) — caller must free
        bool IsMethod = false;     // true when registered as a struct/class method (has implicit self pointer)
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
        CFlatParser::CompoundStatementContext* Body;
        std::vector<DeclTypeAndValue> Params;
        TypeAndValue ReturnType;
    };

    struct ReturnCaptureContext
    {
        llvm::AllocaInst* CaptureAlloca;   // nullptr for void return
        llvm::BasicBlock* ContinuationBlock;
    };

    private:
    size_t currentLine = 0;
    size_t currentColumn = 0;

    public:
    TypeAndValue lastCallReturnType;        // set by CreateOverloadedFunctionCall for post-call TypeAndValue queries
    bool lastCallReturnsOwned = false;       // set when the last call returned an owned heap string or pointer
    bool lastOwningResult = false;           // set by ParseNewExpression/ParseMoveExpression; consumed by ParseDeclaration
    bool currentFunctionReturnsOwned = false; // true when current function is declared with move T* or move string return type
    bool lastCallIsBonded = false;           // set when the last call returned a bonded (borrowed) value
    std::vector<std::string> lastCallBondedSources; // bond parameter names the last call's return borrows from

    private:

    void SetSourceLocation(size_t line, size_t column)
    {
        currentLine = line;
        currentColumn = column;
    }

    void LogError(std::string message) const
    {
        std::cout << std::format("{}({},{}): {}\n", sourceFileName, currentLine, currentColumn, message);
        if (diagnosticSink_)
        {
            diagnosticSink_(sourceFileName, currentLine, currentColumn, message);
            throw CompilerAbortException{ message, sourceFileName, currentLine, currentColumn };
        }
        if (!expectedError.empty())
        {
            if (message.find(expectedError) != std::string::npos)
            {
                std::cout << "PASS: expected error received\n";
                throw ExpectedErrorReceived{};
            }
            std::cout << std::format("FAIL: expected error '{}' but got '{}'\n", expectedError, message);
            exit(1);
        }
        exit(1);
    }

    friend class MainListener;
    friend class ForwardRefScanner;

    std::unique_ptr<llvm::IRBuilder<>> builder;
    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::LLVMContext> context;

    std::vector<StackState> stackNamedVariable;
    std::unordered_map<std::string, llvm::GlobalVariable*> globalNamedVariable;
    std::unordered_map<std::string, TypeAndValue> globalVariableTypes;
    std::unordered_map<std::string, StructData> dataStructures;

    // Maps annotation name -> field names declared in its body (empty vector = no-arg annotation).
    // Field name "" means the annotation accepts a single unnamed positional arg.
    std::unordered_map<std::string, std::vector<std::string>> annotationRegistry;

    // Tracks which __reflect_T functions have been synthesized (lazy, per struct type).
    // Pattern mirrors instantiatedGenerics.
    std::unordered_set<std::string> synthesizedReflectFunctions;

    std::unordered_map<std::string, ProgramData> programTable;
    std::unordered_map<std::string, std::string> enumBackingTypes;
    std::unordered_map<std::string, std::string> typeAliases;
    std::unordered_map<std::string, std::vector<FunctionSymbol>> functionTable;
    std::unordered_map<std::string, std::vector<InterfaceMethod>> interfaceTable;
    std::unordered_map<std::string, std::vector<std::string>> interfaceParents;
    std::unordered_map<std::string, llvm::Constant*> stringPool;
    std::unordered_set<std::string> namespaceTable;
    std::unordered_set<std::string> importedFiles;
    std::vector<std::string> importStack;  // DFS stack for circular import detection
    std::string importSearchDir;
    std::string runtimeDir;
    std::string sourceFileDir_;  // original source dir for LSP temp-file analysis
private:
    // When true, disables auto-import of core/runtime.cb
    bool skipRuntimeImport = false;
    bool verbose = false;
    int platformValue = 64;  // 64 for win64, 32 for win32
    int lambdaCounter = 0;
    std::string expectedError;
    size_t expectedErrorScopeDepth = SIZE_MAX;  // SIZE_MAX = scoped block form (checked manually after block); else stackNamedVariable depth for bare-semicolon form

    // Compile-time macros (constant throughout compilation, set early)
    struct CompileTimeMacro
    {
        std::string name;
        llvm::Constant* value;
        std::string type;  // "int", "string", etc.
    };
    std::unordered_map<std::string, CompileTimeMacro> compileTimeMacros;
    // Keep imported parse state alive: generic template ctx pointers point into
    // these ANTLR parse trees and are accessed later during main-file instantiation.
    struct ImportedParseState
    {
        std::string canonicalPath;   // absolute canonical path, used to identify core vs user imports
        std::unique_ptr<std::ifstream> stream;
        std::unique_ptr<antlr4::ANTLRInputStream> input;
        std::unique_ptr<CFlatLexer> lexer;
        std::unique_ptr<antlr4::CommonTokenStream> tokens;
        std::unique_ptr<CFlatParser> parser;
    };
    std::vector<ImportedParseState> importedParseStates;
    std::unordered_map<std::string, std::string> namespaceAliasTable;
    std::unordered_map<std::string, ReturnBlockEntry> returnBlockTable;
    std::optional<ReturnCaptureContext> returnCapture;
    std::unordered_map<llvm::Constant*, int32_t> stringLiteralLenByPtr;
    bool strConcatRegistered = false;
    bool stringDtorRegistered = false;
    std::function<void(const std::string&, size_t, size_t, const std::string&)> diagnosticSink_;
    LspSymbolIndex* symbolSink_ = nullptr;

    llvm::Function* currentFunction;
    std::string sourceFileName;
    std::string currentSourceFilePath_;
    llvm::AllocaInst* autoVaListAlloca = nullptr;

    std::unique_ptr<llvm::DIBuilder> diBuilder;
    llvm::DIFile* diFile = nullptr;
    llvm::DICompileUnit* compileUnit = nullptr;
    llvm::DISubprogram* currentSubprogram = nullptr;


private:
    // Create Function Proto or Signature
    llvm::Function* createFunctionProto(std::string name, llvm::FunctionType* returnType)
    {
        auto fn = llvm::Function::Create(returnType, llvm::Function::ExternalLinkage, name, *module);

        // CFlat treats null pointer dereferences as defined behavior (hardware fault → SEH).
        // This attribute prevents instcombine from removing loads/stores through null pointers
        // as UB, preserving the fault so the program construct's SEH handler can catch it.
        fn->addFnAttr(llvm::Attribute::NullPointerIsValid);

        llvm::verifyFunction(*fn);

        return fn;
    }

    void SetVariableRefCountStorage(const std::string& varName, llvm::Value* refStorage)
    {
        for (auto& frame : stackNamedVariable)
        {
            auto it = frame.namedVariable.find(varName);
            if (it != frame.namedVariable.end())
            {
                it->second.RefCountStorage = refStorage;
                return;
            }
        }
    }

    void EmitConditionalOwningPtrCleanup(const NamedVariable& namedVar, llvm::Value* refCount)
    {
        auto* zeroCond = builder->CreateICmpEQ(refCount, builder->getInt32(0), "refiszero");
        auto* freeBB = llvm::BasicBlock::Create(*context, "refcount.free", builder->GetInsertBlock()->getParent());
        auto* skipBB = llvm::BasicBlock::Create(*context, "refcount.skip", builder->GetInsertBlock()->getParent());
        builder->CreateCondBr(zeroCond, freeBB, skipBB);
        builder->SetInsertPoint(freeBB);
        EmitOwningPtrCleanup(namedVar);
        builder->CreateBr(skipBB);
        builder->SetInsertPoint(skipBB);
    }

    void EmitOwningPtrCleanup(const NamedVariable& namedVar)
    {
        // Load the current pointer value from the alloca
        auto* ptrVal = builder->CreateLoad(namedVar.BaseType, namedVar.Storage);

        // Skip if null (pointer may have been moved out)
        auto* isNull = builder->CreateICmpEQ(
            ptrVal,
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(namedVar.BaseType)));
        auto* cleanupBB = llvm::BasicBlock::Create(*context, "move.cleanup", builder->GetInsertBlock()->getParent());
        auto* afterBB   = llvm::BasicBlock::Create(*context, "move.after",   builder->GetInsertBlock()->getParent());
        builder->CreateCondBr(isNull, afterBB, cleanupBB);

        builder->SetInsertPoint(cleanupBB);

        // Call destructor if the type has one
        auto it = dataStructures.find(namedVar.TypeAndValue.TypeName);
        if (it != dataStructures.end() && it->second.Destructor != nullptr)
            builder->CreateCall(it->second.Destructor->getFunctionType(), it->second.Destructor, { ptrVal });

        // Call operator delete (global)
        auto* opDel = module->getFunction("operator delete");
        if (opDel)
        {
            auto* voidPtrTy = builder->getInt8Ty()->getPointerTo();
            auto* voidPtr = builder->CreateBitCast(ptrVal, voidPtrTy);
            builder->CreateCall(opDel->getFunctionType(), opDel, { voidPtr });
        }

        builder->CreateBr(afterBB);
        builder->SetInsertPoint(afterBB);
    }

    void EmitDestructorsForScope(const StackState& frame)
    {
        if (builder->GetInsertBlock()->getTerminator() != nullptr)
            return;

        for (const auto& [varName, namedVar] : frame.namedVariable)
        {
            if (namedVar.TypeAndValue.Pointer && namedVar.IsOwning)
            {
                if (namedVar.RefCountStorage == nullptr)
                {
                    EmitOwningPtrCleanup(namedVar);
                }
                else
                {
                    auto* cur = builder->CreateLoad(builder->getInt32Ty(), namedVar.RefCountStorage);
                    auto* dec = builder->CreateSub(cur, builder->getInt32(1), "refdec");
                    builder->CreateStore(dec, namedVar.RefCountStorage);
                    EmitConditionalOwningPtrCleanup(namedVar, dec);
                }
                continue;
            }
            if (namedVar.TypeAndValue.Pointer) continue;
            auto it = dataStructures.find(namedVar.TypeAndValue.TypeName);
            if (it != dataStructures.end())
            {
                // String destructor is only called when the local owns its buffer.
                if (namedVar.TypeAndValue.TypeName == "string")
                {
                    if (!namedVar.IsOwningString) continue;
                    EnsureStringDtorRegistered();
                }
                if (it->second.Destructor == nullptr) continue;
                auto* fn = it->second.Destructor;
                builder->CreateCall(fn->getFunctionType(), fn, { namedVar.Storage });
            }
        }

        // Clean up owning function parameters (move params)
        for (const auto& [varName, namedVar] : frame.functionArgument)
        {
            if (namedVar.IsOwning && namedVar.Storage != nullptr)
                EmitOwningPtrCleanup(namedVar);

            // Clean up move string parameters (move string param — non-pointer ownership)
            if (namedVar.IsOwningString && namedVar.Storage != nullptr)
            {
                EnsureStringDtorRegistered();
                auto it = dataStructures.find("string");
                if (it != dataStructures.end() && it->second.Destructor != nullptr)
                    builder->CreateCall(it->second.Destructor->getFunctionType(), it->second.Destructor, { namedVar.Storage });
            }

            // Clean up move struct parameters
            if (namedVar.IsOwningStruct && namedVar.Storage != nullptr)
            {
                auto it = dataStructures.find(namedVar.TypeAndValue.TypeName);
                if (it != dataStructures.end() && it->second.Destructor != nullptr)
                    builder->CreateCall(it->second.Destructor->getFunctionType(), it->second.Destructor, { namedVar.Storage });
            }
        }

        // Release lock held by this scope (lock statement).
        if (frame.lockCleanup.has_value())
        {
            auto& lc = frame.lockCleanup.value();
            builder->CreateCall(lc.UnlockFn->getFunctionType(), lc.UnlockFn, { lc.MutexPtr });
        }
    }

    void createFunctionBlock(llvm::Function* fn, const std::string& friendlyName, std::vector<LLVMBackend::TypeAndValue> arguments, bool returnsOwned = false)
    {
        // all function starts at "entry" block
        auto entry = CreateBasicBlock("entry", fn);
        builder->SetInsertPoint(entry);
        auto& stackState = stackNamedVariable.emplace_back();

        stackState.continueBlock = &fn->back();
        stackState.resumeBlock = &fn->back();
        stackState.isFunction = true;
        stackState.functionName = friendlyName;
        currentFunctionReturnsOwned = returnsOwned;

        // populate function arguments
        auto itr_nameArg = arguments.begin();
        for (auto& arg : fn->args())
        {
            arg.setName(itr_nameArg->VariableName);

            if (itr_nameArg->IsInterface && !itr_nameArg->IsInterfacePointer)
            {
                // Interface args arrive by value ({i8*,i8*}). Store in a temp alloca so
                // Storage is a {i8*,i8*}* pointer suitable for CallInterfaceMethod GEP.
                auto fatTy = GetFatPtrType();
                auto tmp = builder->CreateAlloca(fatTy, nullptr, itr_nameArg->VariableName);
                builder->CreateStore(&arg, tmp);
                NamedVariable namedVar{
                    .TypeAndValue = *itr_nameArg,
                    .BaseType = fatTy,
                    .Primary = nullptr,
                    .Storage = tmp,
                };
                stackState.functionArgument[itr_nameArg->VariableName] = namedVar;
            }
            else if (itr_nameArg->IsMove && itr_nameArg->Pointer)
            {
                // move parameter: alloca a slot so we can null it out on scope exit
                auto* ptrTy = GetType(*itr_nameArg, nullptr, true);
                auto* alloc = builder->CreateAlloca(ptrTy, nullptr, itr_nameArg->VariableName);
                builder->CreateStore(&arg, alloc);
                NamedVariable namedVar{
                    .TypeAndValue = *itr_nameArg,
                    .BaseType = ptrTy,
                    .Primary = nullptr,
                    .Storage = alloc,
                    .IsOwning = true,
                };
                stackState.functionArgument[itr_nameArg->VariableName] = namedVar;
            }
            else if (itr_nameArg->IsMove && itr_nameArg->TypeName == "string")
            {
                // move string parameter: alloca a slot so the destructor can free the buffer on scope exit
                auto* strTy = GetType(*itr_nameArg, nullptr, false);
                auto* alloc = builder->CreateAlloca(strTy, nullptr, itr_nameArg->VariableName);
                builder->CreateStore(&arg, alloc);
                NamedVariable namedVar{
                    .TypeAndValue = *itr_nameArg,
                    .BaseType = strTy,
                    .Primary = nullptr,
                    .Storage = alloc,
                    .IsOwningString = true,
                };
                stackState.functionArgument[itr_nameArg->VariableName] = namedVar;
            }
            else if (itr_nameArg->IsMove && !itr_nameArg->Pointer)
            {
                // move struct parameter: alloca a slot so the destructor runs on scope exit
                auto* structTy = GetType(*itr_nameArg, nullptr, false);
                auto* alloc = builder->CreateAlloca(structTy, nullptr, itr_nameArg->VariableName);
                builder->CreateStore(&arg, alloc);
                NamedVariable namedVar{
                    .TypeAndValue = *itr_nameArg,
                    .BaseType = structTy,
                    .Primary = nullptr,
                    .Storage = alloc,
                    .IsOwningStruct = true,
                };
                stackState.functionArgument[itr_nameArg->VariableName] = namedVar;
            }
            else if (!itr_nameArg->Pointer && GetType(*itr_nameArg)->isStructTy())
            {
                // Struct value parameter: store into an alloca so its address is available
                // (e.g. for reflect(), GEP field access, and taking &param).
                auto* structTy = GetType(*itr_nameArg);
                auto* alloc = builder->CreateAlloca(structTy, nullptr, itr_nameArg->VariableName);
                builder->CreateStore(&arg, alloc);
                NamedVariable namedVar{
                    .TypeAndValue = *itr_nameArg,
                    .BaseType = structTy,
                    .Storage = alloc,
                };
                stackState.functionArgument[itr_nameArg->VariableName] = namedVar;
            }
            else
            {
                // Alloca all remaining params (scalars and non-owning pointers) so they
                // are writable (CreateStore targets) just like local variables.
                auto* ty = GetType(*itr_nameArg, nullptr, itr_nameArg->Pointer);
                auto* alloc = builder->CreateAlloca(ty, nullptr, itr_nameArg->VariableName);
                builder->CreateStore(&arg, alloc);
                NamedVariable namedVar
                {
                    .TypeAndValue = *itr_nameArg,
                    .BaseType = ty,
                    .Storage = alloc,
                };
                stackState.functionArgument[itr_nameArg->VariableName] = namedVar;
            }
            if (symbolSink_ && !itr_nameArg->VariableName.empty())
                symbolSink_->RegisterVariable(itr_nameArg->VariableName, itr_nameArg->TypeName);

            itr_nameArg++;
        }

        currentFunction = fn;
        autoVaListAlloca = nullptr;
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

    // Registers the built-in `string` value type: { i8* _ptr, i32 _len }.
    // data() returns _ptr; length() returns _len.  No vtable — direct struct access.
    void RegisterBuiltinString()
    {
        auto* ptrTy = builder->getInt8Ty()->getPointerTo();
        auto* i32Ty = builder->getInt32Ty();

        auto* strTy = llvm::StructType::getTypeByName(*context, "string");
        if (!strTy)
            strTy = llvm::StructType::create(*context, { ptrTy, i32Ty }, "string");

        DeclTypeAndValue ptrField;
        ptrField.TypeName = "i8";
        ptrField.VariableName = "_ptr";
        ptrField.Pointer = true;

        DeclTypeAndValue lenField;
        lenField.TypeName = "i32";
        lenField.VariableName = "_len";

        dataStructures["string"].StructType = strTy;
        dataStructures["string"].StructFields = { ptrField, lenField };

        // Create string() default constructor -> string{nullptr, 0}
        {
            auto* fnTy = llvm::FunctionType::get(strTy, {}, false);
            auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "string.ctor", *module);
            auto* entry = llvm::BasicBlock::Create(*context, "entry", fn);
            llvm::IRBuilder<> b(entry);
            b.CreateRet(llvm::ConstantAggregateZero::get(strTy));

            FunctionSymbol sym;
            sym.UniqueName = "string.ctor";
            sym.Function = fn;
            sym.ReturnType = TypeAndValue{ "string", "", false };
            sym.Parameters = {};
            functionTable["string"].push_back(sym);
        }

        // Create string.data(string self) -> i8*  [by value, extract field 0]
        // Returns _ptr if non-null; otherwise returns a pointer to a static empty string.
        // This makes data() safe on default-initialized strings (ptr==null, len==0).
        {
            auto* emptyStrTy = llvm::ArrayType::get(builder->getInt8Ty(), 1);
            auto* emptyStrGlobal = new llvm::GlobalVariable(
                *module, emptyStrTy, /*isConstant=*/true,
                llvm::GlobalValue::PrivateLinkage,
                llvm::ConstantAggregateZero::get(emptyStrTy),
                "__string_empty");

            auto* fnTy = llvm::FunctionType::get(ptrTy, { strTy }, false);
            auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "string.data", *module);
            fn->arg_begin()->setName("self");
            auto* entry = llvm::BasicBlock::Create(*context, "entry", fn);
            llvm::IRBuilder<> b(entry);
            auto* ptr = b.CreateExtractValue(&*fn->arg_begin(), { 0u });
            auto* nullPtr = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
            auto* isNull = b.CreateICmpEQ(ptr, nullPtr);
            b.CreateRet(b.CreateSelect(isNull, emptyStrGlobal, ptr));

            TypeAndValue selfParam{ "string", "self", false };
            FunctionSymbol sym;
            sym.UniqueName = "string.data";
            sym.Function = fn;
            sym.ReturnType = TypeAndValue{ "i8", "", true };
            sym.Parameters = { selfParam };
            functionTable["data"].push_back(sym);
        }

        // Create string.length(string self) -> i32  [by value, extract field 1]
        {
            auto* fnTy = llvm::FunctionType::get(i32Ty, { strTy }, false);
            auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "string.length", *module);
            fn->arg_begin()->setName("self");
            auto* entry = llvm::BasicBlock::Create(*context, "entry", fn);
            llvm::IRBuilder<> b(entry);
            b.CreateRet(b.CreateExtractValue(&*fn->arg_begin(), { 1u }));

            TypeAndValue selfParam{ "string", "self", false };
            FunctionSymbol sym;
            sym.UniqueName = "string.length";
            sym.Function = fn;
            sym.ReturnType = TypeAndValue{ "i32", "", false };
            sym.Parameters = { selfParam };
            functionTable["length"].push_back(sym);
        }

        // Create string.hash(string self) -> i32  [FNV-1a over the bytes]
        {
            auto* i8Ty  = builder->getInt8Ty();
            auto* ptrTy = i8Ty->getPointerTo();
            auto* fnTy  = llvm::FunctionType::get(i32Ty, { strTy }, false);
            auto* fn    = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "string.hash", *module);
            fn->arg_begin()->setName("self");

            auto* entryBB  = llvm::BasicBlock::Create(*context, "entry",  fn);
            auto* loopBB   = llvm::BasicBlock::Create(*context, "loop",   fn);
            auto* bodyBB   = llvm::BasicBlock::Create(*context, "body",   fn);
            auto* exitBB   = llvm::BasicBlock::Create(*context, "exit",   fn);
            llvm::IRBuilder<> b(entryBB);

            auto* ptr = b.CreateExtractValue(&*fn->arg_begin(), { 0u }, "ptr");
            auto* len = b.CreateExtractValue(&*fn->arg_begin(), { 1u }, "len");
            auto* fnv_basis = b.getInt32(2166136261u);
            auto* fnv_prime = b.getInt32(16777619u);
            b.CreateBr(loopBB);

            // loop: phi nodes for index and hash
            b.SetInsertPoint(loopBB);
            auto* idxPhi  = b.CreatePHI(i32Ty, 2, "idx");
            auto* hashPhi = b.CreatePHI(i32Ty, 2, "h");
            idxPhi->addIncoming(b.getInt32(0), entryBB);
            hashPhi->addIncoming(fnv_basis, entryBB);
            auto* cond = b.CreateICmpSLT(idxPhi, len);
            b.CreateCondBr(cond, bodyBB, exitBB);

            // body: h = (h ^ byte) * prime; idx++
            b.SetInsertPoint(bodyBB);
            auto* gep    = b.CreateGEP(i8Ty, ptr, idxPhi);
            auto* byteVal = b.CreateLoad(i8Ty, gep, "byte");
            auto* byteExt = b.CreateZExt(byteVal, i32Ty);
            auto* xored  = b.CreateXor(hashPhi, byteExt);
            auto* mulled = b.CreateMul(xored, fnv_prime);
            auto* idxNext = b.CreateAdd(idxPhi, b.getInt32(1));
            idxPhi->addIncoming(idxNext, bodyBB);
            hashPhi->addIncoming(mulled, bodyBB);
            b.CreateBr(loopBB);

            b.SetInsertPoint(exitBB);
            b.CreateRet(hashPhi);

            TypeAndValue selfParam{ "string", "self", false };
            FunctionSymbol sym;
            sym.UniqueName = "string.hash";
            sym.Function = fn;
            sym.ReturnType = TypeAndValue{ "i32", "", false };
            sym.Parameters = { selfParam };
            functionTable["hash"].push_back(sym);
        }

        // String destructor is registered lazily via EnsureStringDtorRegistered()
        // so that the C `free` function is available in the function table first.
    }

    void RegisterBuiltinStrConcat()
    {
        auto* i8Ty     = builder->getInt8Ty();
        auto* ptrTy    = i8Ty->getPointerTo();
        auto* ptrPtrTy = ptrTy->getPointerTo();
        auto* i32Ty    = builder->getInt32Ty();
        auto* i32PtrTy = i32Ty->getPointerTo();
        auto* i64Ty    = builder->getInt64Ty();
        auto* strTy    = llvm::StructType::getTypeByName(*context, "string");

        auto* mallocTy = llvm::FunctionType::get(ptrTy, { i64Ty }, false);
        auto* mallocFn = llvm::dyn_cast<llvm::Function>(
            module->getOrInsertFunction("malloc", mallocTy).getCallee());

        auto* fnTy = llvm::FunctionType::get(strTy, { ptrPtrTy, i32PtrTy, i32Ty }, false);
        auto* fn   = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "__strconcat", *module);
        auto  argIt    = fn->arg_begin();
        auto* argPtrs  = &*argIt++;  argPtrs->setName("ptrs");
        auto* argLens  = &*argIt++;  argLens->setName("lens");
        auto* argCount = &*argIt;    argCount->setName("count");

        auto* entryBB = llvm::BasicBlock::Create(*context, "entry",    fn);
        auto* sumCond = llvm::BasicBlock::Create(*context, "sum.cond", fn);
        auto* sumBody = llvm::BasicBlock::Create(*context, "sum.body", fn);
        auto* allocBB = llvm::BasicBlock::Create(*context, "alloc",    fn);
        auto* cpyCond = llvm::BasicBlock::Create(*context, "cpy.cond", fn);
        auto* cpyBody = llvm::BasicBlock::Create(*context, "cpy.body", fn);
        auto* cpyNext = llvm::BasicBlock::Create(*context, "cpy.next", fn);
        auto* nullBB  = llvm::BasicBlock::Create(*context, "null",     fn);

        llvm::IRBuilder<> rb(entryBB);
        auto* totalA = rb.CreateAlloca(i32Ty, nullptr, "total");
        auto* bufA   = rb.CreateAlloca(ptrTy, nullptr, "buf");
        auto* dstA   = rb.CreateAlloca(ptrTy, nullptr, "dst");
        auto* idxA   = rb.CreateAlloca(i32Ty, nullptr, "idx");
        rb.CreateStore(rb.getInt32(0), totalA);
        rb.CreateStore(rb.getInt32(0), idxA);
        rb.CreateBr(sumCond);

        // Sum loop: total = sum of all segment lengths
        rb.SetInsertPoint(sumCond);
        rb.CreateCondBr(rb.CreateICmpSLT(rb.CreateLoad(i32Ty, idxA), argCount), sumBody, allocBB);
        rb.SetInsertPoint(sumBody);
        {
            auto* i   = rb.CreateLoad(i32Ty, idxA);
            auto* len = rb.CreateLoad(i32Ty, rb.CreateGEP(i32Ty, argLens, i));
            rb.CreateStore(rb.CreateAdd(rb.CreateLoad(i32Ty, totalA), len), totalA);
            rb.CreateStore(rb.CreateAdd(i, rb.getInt32(1)), idxA);
            rb.CreateBr(sumCond);
        }

        // Allocate buffer: total + 1 bytes
        rb.SetInsertPoint(allocBB);
        {
            auto* total   = rb.CreateLoad(i32Ty, totalA);
            auto* total64 = rb.CreateSExt(total, i64Ty);
            auto* buf     = rb.CreateCall(mallocFn, { rb.CreateAdd(total64, rb.getInt64(1)) }, "buf");
            rb.CreateStore(buf, bufA);
            rb.CreateStore(buf, dstA);
            rb.CreateStore(rb.getInt32(0), idxA);
            rb.CreateBr(cpyCond);
        }

        // Copy loop: iterate over segments
        rb.SetInsertPoint(cpyCond);
        rb.CreateCondBr(rb.CreateICmpSLT(rb.CreateLoad(i32Ty, idxA), argCount), cpyBody, nullBB);

        rb.SetInsertPoint(cpyBody);
        {
            auto* i      = rb.CreateLoad(i32Ty, idxA);
            auto* src    = rb.CreateLoad(ptrTy,  rb.CreateGEP(ptrTy,  argPtrs, i));
            auto* segLen = rb.CreateLoad(i32Ty,  rb.CreateGEP(i32Ty,  argLens, i));
            auto* jA     = rb.CreateAlloca(i32Ty, nullptr, "j");
            rb.CreateStore(rb.getInt32(0), jA);
            auto* bCond  = llvm::BasicBlock::Create(*context, "b.cond", fn);
            auto* bBody  = llvm::BasicBlock::Create(*context, "b.body", fn);
            rb.CreateBr(bCond);
            rb.SetInsertPoint(bCond);
            rb.CreateCondBr(rb.CreateICmpSLT(rb.CreateLoad(i32Ty, jA), segLen), bBody, cpyNext);
            rb.SetInsertPoint(bBody);
            {
                auto* j2     = rb.CreateLoad(i32Ty, jA);
                auto* dstNow = rb.CreateLoad(ptrTy, dstA);
                auto* byte   = rb.CreateLoad(i8Ty, rb.CreateGEP(i8Ty, src, j2));
                rb.CreateStore(byte, rb.CreateGEP(i8Ty, dstNow, j2));
                rb.CreateStore(rb.CreateAdd(j2, rb.getInt32(1)), jA);
                rb.CreateBr(bCond);
            }
        }

        rb.SetInsertPoint(cpyNext);
        {
            auto* i      = rb.CreateLoad(i32Ty, idxA);
            auto* segLen = rb.CreateLoad(i32Ty, rb.CreateGEP(i32Ty, argLens, i));
            auto* dst    = rb.CreateLoad(ptrTy, dstA);
            auto* sLen64 = rb.CreateSExt(segLen, i64Ty);
            rb.CreateStore(rb.CreateGEP(i8Ty, dst, sLen64), dstA);
            rb.CreateStore(rb.CreateAdd(i, rb.getInt32(1)), idxA);
            rb.CreateBr(cpyCond);
        }

        // Null-terminate the concatenated buffer, build and return a string struct by value.
        rb.SetInsertPoint(nullBB);
        {
            rb.CreateStore(rb.getInt8(0), rb.CreateLoad(ptrTy, dstA));
            llvm::Value* strVal = llvm::UndefValue::get(strTy);
            strVal = rb.CreateInsertValue(strVal, rb.CreateLoad(ptrTy,  bufA),   { 0u });
            strVal = rb.CreateInsertValue(strVal, rb.CreateLoad(i32Ty, totalA),  { 1u });
            rb.CreateRet(strVal);
        }

        FunctionSymbol sym;
        sym.UniqueName = "__strconcat";
        sym.Function   = fn;
        sym.ReturnType = TypeAndValue{ "string", "", false, false };
        sym.Parameters = {
            TypeAndValue{ "i8",  "ptrs",  true,  false },
            TypeAndValue{ "i32", "lens",  true,  false },
            TypeAndValue{ "i32", "count", false, false },
        };
        functionTable["__strconcat"].push_back(sym);
    }

    void Init()
    {
        context = std::make_unique<llvm::LLVMContext>();
        module = std::make_unique<llvm::Module>("MyCompiler", *context);
        builder = std::make_unique<llvm::IRBuilder<>>(*context);

        // Pre-register the built-in `string` value type { i8* _ptr, i32 _len }.
        RegisterBuiltinString();
    }

    // Called lazily the first time a string local's destructor needs to fire.
    // Must run after cruntime.cb is compiled so `free` is in the LLVM module.
    void EnsureStringDtorRegistered()
    {
        if (stringDtorRegistered) return;
        stringDtorRegistered = true;

        auto* strTy = llvm::StructType::getTypeByName(*context, "string");
        if (!strTy) return;

        // Look up free() — compiled from cruntime.cb, must already be in the module.
        auto* freeFn = module->getFunction("free");
        if (!freeFn) return;   // free not yet available; destructor cannot be created yet

        auto* voidTy   = llvm::Type::getVoidTy(*context);
        auto* ptrTy    = builder->getInt8Ty()->getPointerTo();
        auto* strPtrTy = strTy->getPointerTo();

        auto* dtorFnTy = llvm::FunctionType::get(voidTy, { strPtrTy }, false);
        auto* dtorFn   = llvm::Function::Create(dtorFnTy, llvm::Function::InternalLinkage, "string.dtor", *module);
        dtorFn->arg_begin()->setName("self");

        auto* entry = llvm::BasicBlock::Create(*context, "entry", dtorFn);
        llvm::IRBuilder<> b(entry);

        auto* self    = &*dtorFn->arg_begin();
        auto* ptrPtr  = b.CreateStructGEP(strTy, self, 0, "ptrfield");
        auto* ptr     = b.CreateLoad(ptrTy, ptrPtr, "ptr");
        auto* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
        auto* isNotNull = b.CreateICmpNE(ptr, nullPtr, "is_not_null");
        auto* freeBlock = llvm::BasicBlock::Create(*context, "free", dtorFn);
        auto* doneBlock = llvm::BasicBlock::Create(*context, "done", dtorFn);
        b.CreateCondBr(isNotNull, freeBlock, doneBlock);
        b.SetInsertPoint(freeBlock);
        b.CreateCall(freeFn->getFunctionType(), freeFn, { ptr });
        b.CreateBr(doneBlock);
        b.SetInsertPoint(doneBlock);
        b.CreateStore(nullPtr, ptrPtr);
        b.CreateRetVoid();

        RegisterDestructor("string", dtorFn);
    }

    // Called lazily from ParseFormatString after string concat is first needed.
    void EnsureStrConcatRegistered()
    {
        if (strConcatRegistered) return;
        strConcatRegistered = true;
        RegisterBuiltinStrConcat();
    }

    void CreateVaStart(llvm::Value* apAlloca)
    {
        auto* fn = llvm::Intrinsic::getDeclaration(module.get(), llvm::Intrinsic::vastart);
        builder->CreateCall(fn, {apAlloca});
    }

    void CreateVaEnd(llvm::Value* apAlloca)
    {
        auto* fn = llvm::Intrinsic::getDeclaration(module.get(), llvm::Intrinsic::vaend);
        builder->CreateCall(fn, {apAlloca});
    }

    // Emit a single-argument LLVM float intrinsic (round, floor, ceil, fabs, sqrt).
    // Returns nullptr if methodName is not a recognized float method.
    // Works for both float (f32) and double (f64) — type is inferred from floatVal.
    llvm::Value* CreateFloatIntrinsic(const std::string& methodName, llvm::Value* floatVal)
    {
        llvm::Intrinsic::ID id;
        if      (methodName == "round") id = llvm::Intrinsic::round;
        else if (methodName == "floor") id = llvm::Intrinsic::floor;
        else if (methodName == "ceil")  id = llvm::Intrinsic::ceil;
        else if (methodName == "abs")   id = llvm::Intrinsic::fabs;
        else if (methodName == "sqrt")  id = llvm::Intrinsic::sqrt;
        else return nullptr;

        auto* fn = llvm::Intrinsic::getDeclaration(module.get(), id, {floatVal->getType()});
        return builder->CreateCall(fn, {floatVal});
    }

    // Emit an integer narrowing/widening conversion (to_i8, to_u8, to_i16, to_u16,
    // to_i32, to_u32, to_i64, to_u64).  Returns nullptr for unrecognized names.
    // Narrowing always truncates; widening sign-extends for signed targets, zero-extends for unsigned.
    llvm::Value* CreateIntegerConvert(const std::string& methodName, llvm::Value* intVal)
    {
        struct Target { unsigned bits; bool isSigned; };
        static const std::unordered_map<std::string, Target> table = {
            {"to_i8",  {8,  true}},  {"to_u8",  {8,  false}},
            {"to_i16", {16, true}},  {"to_u16", {16, false}},
            {"to_i32", {32, true}},  {"to_u32", {32, false}},
            {"to_i64", {64, true}},  {"to_u64", {64, false}},
        };
        auto it = table.find(methodName);
        if (it == table.end()) return nullptr;

        unsigned srcBits  = intVal->getType()->getIntegerBitWidth();
        unsigned destBits = it->second.bits;
        auto* destTy = llvm::Type::getIntNTy(*context, destBits);

        if (srcBits > destBits)  return builder->CreateTrunc(intVal, destTy);
        if (srcBits < destBits)  return it->second.isSigned
                                     ? builder->CreateSExt(intVal, destTy)
                                     : builder->CreateZExt(intVal, destTy);
        return intVal; // same width — no-op
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

    void RunBaselinePasses();
    void OptimizeModule(int optimizationLevel);

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

    bool EmitExecutable(const std::string& exePath, const std::string& platform)
    {
        std::string triple;
        std::string clangTarget;
        std::string cpu;
        std::string clangBits;
        if (platform == "win32")
        {
            triple = "i686-pc-windows-msvc";
            clangTarget = "--target=i686-pc-windows-msvc";
            clangBits = "-m32";
            cpu = "i686";
        }
        else // win64 (default)
        {
            triple = "x86_64-pc-windows-msvc";
            clangTarget = "--target=x86_64-pc-windows-msvc";
            clangBits = "-m64";
            cpu = "x86-64";
        }

        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmPrinters();

        module->setTargetTriple(triple);

        std::string err;
        const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, err);
        if (!target)
        {
            std::cerr << "Error: no target for triple '" << triple << "': " << err << "\n";
            return false;
        }

        llvm::TargetOptions opt;
        auto TM = std::unique_ptr<llvm::TargetMachine>(
            target->createTargetMachine(triple, cpu, "", opt, llvm::Reloc::PIC_));
        module->setDataLayout(TM->createDataLayout());

        auto objPath = exePath + ".obj";
        std::error_code EC;
        llvm::raw_fd_ostream dest(objPath, EC, llvm::sys::fs::OF_None);
        if (EC)
        {
            std::cerr << "Error: could not write object file '" << objPath << "': " << EC.message() << "\n";
            return false;
        }

        llvm::legacy::PassManager pass;
        if (TM->addPassesToEmitFile(pass, dest, nullptr, llvm::CodeGenFileType::ObjectFile))
        {
            std::cerr << "Error: target does not support object file emission\n";
            return false;
        }
        pass.run(*module);
        dest.flush();
        dest.close();

        // ---- Find lld-link (prefer vcpkg tools next to clang) ----
        std::string lldLinkPath;
        {
            auto clangErr = llvm::sys::findProgramByName("clang-cl");
            if (clangErr)
            {
                llvm::SmallString<256> candidate(llvm::sys::path::parent_path(*clangErr));
                llvm::sys::path::append(candidate, "lld-link.exe");
                if (llvm::sys::fs::exists(candidate))
                    lldLinkPath = candidate.str().str();
            }
            if (lldLinkPath.empty())
            {
                auto err = llvm::sys::findProgramByName("lld-link");
                if (err) lldLinkPath = *err;
            }
        }
        if (lldLinkPath.empty())
        {
            llvm::sys::fs::remove(objPath);
            std::cerr << "Error: lld-link.exe not found\n";
            return false;
        }

        const std::string arch = (platform == "win32") ? "x86" : "x64";

        // ---- Find VS install path via vswhere ----
        std::string vsPath;
        {
            const char* vswhereFixed = "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe";
            if (llvm::sys::fs::exists(vswhereFixed))
            {
                llvm::SmallString<256> outFile;
                llvm::sys::path::system_temp_directory(true, outFile);
                int outFD;
                if (!llvm::sys::fs::createTemporaryFile("mycompiler_vswhere", "txt", outFD, outFile))
                {
                    _close(outFD);
                    std::string outFileStr = outFile.str().str();

                    std::vector<llvm::StringRef> vsArgs = { vswhereFixed, "-latest", "-property", "installationPath" };
                    std::optional<llvm::StringRef> vsRedirects[3] = { std::nullopt, llvm::StringRef(outFileStr), std::nullopt };
                    llvm::sys::ExecuteAndWait(vswhereFixed, vsArgs, std::nullopt, vsRedirects);

                    if (auto buf = llvm::MemoryBuffer::getFile(outFileStr))
                        vsPath = buf.get()->getBuffer().trim().str();
                    llvm::sys::fs::remove(outFile);
                }
            }
        }

        // ---- Find latest MSVC lib path ----
        std::string msvcLibPath;
        if (!vsPath.empty())
        {
            std::string msvcRoot = vsPath + "\\VC\\Tools\\MSVC";
            std::string latestVer;
            std::error_code ec;
            for (auto it = llvm::sys::fs::directory_iterator(msvcRoot, ec);
                 it != llvm::sys::fs::directory_iterator(); it.increment(ec))
            {
                if (ec) break;
                auto ver = llvm::sys::path::filename(it->path()).str();
                if (ver > latestVer) latestVer = ver;
            }
            if (!latestVer.empty())
                msvcLibPath = msvcRoot + "\\" + latestVer + "\\lib\\" + arch;
        }

        // ---- Find latest Windows SDK lib paths ----
        std::string ucrtLibPath, umLibPath;
        {
            std::string wkLib = "C:\\Program Files (x86)\\Windows Kits\\10\\Lib";
            std::string latestSDK;
            std::error_code ec;
            for (auto it = llvm::sys::fs::directory_iterator(wkLib, ec);
                 it != llvm::sys::fs::directory_iterator(); it.increment(ec))
            {
                if (ec) break;
                auto ver = llvm::sys::path::filename(it->path()).str();
                if (ver > latestSDK) latestSDK = ver;
            }
            if (!latestSDK.empty())
            {
                ucrtLibPath = wkLib + "\\" + latestSDK + "\\ucrt\\" + arch;
                umLibPath   = wkLib + "\\" + latestSDK + "\\um\\"   + arch;
            }
        }

        // ---- Invoke lld-link directly with explicit lib paths ----
        std::vector<std::string> linkArgStrs = {
            lldLinkPath,
            "/out:" + exePath,
            "/subsystem:console",
        };
        if (!msvcLibPath.empty()) linkArgStrs.push_back("/libpath:" + msvcLibPath);
        if (!ucrtLibPath.empty()) linkArgStrs.push_back("/libpath:" + ucrtLibPath);
        if (!umLibPath.empty())   linkArgStrs.push_back("/libpath:" + umLibPath);
        linkArgStrs.push_back("msvcrt.lib");
        linkArgStrs.push_back("ucrt.lib");
        linkArgStrs.push_back("vcruntime.lib");
        linkArgStrs.push_back("kernel32.lib");
        linkArgStrs.push_back(objPath);

        std::vector<llvm::StringRef> linkArgs;
        for (auto& s : linkArgStrs) linkArgs.push_back(s);

        std::cout << "Linking (" << arch << "): " << exePath << "\n";

        std::string linkErr;
        int rc = llvm::sys::ExecuteAndWait(lldLinkPath, linkArgs, std::nullopt, {}, 0, 0, &linkErr);
        llvm::sys::fs::remove(objPath);

        if (rc != 0)
        {
            std::cerr << "Error: linking failed (exit " << rc << "): " << linkErr << "\n";
            return false;
        }
        return true;
    }

    Operation ParseOperation(std::string operationText)
    {
        if (operationText == "+") { return Operation::Add; }
        else if (operationText == "*") { return Operation::Multiply; }
        else if (operationText == "-") { return Operation::Subtract; }
        else if (operationText == "/") { return Operation::Divide; }
        else if (operationText == "%") { return Operation::Modulo; }
        else if (operationText == "==") { return Operation::Equal; }
        else if (operationText == "!=") { return Operation::NotEqual; }
        else if (operationText == ">") { return Operation::Greater; }
        else if (operationText == ">=") { return Operation::GreaterEqual; }
        else if (operationText == "<") { return Operation::Less; }
        else if (operationText == "<=") { return Operation::LessEqual; }
        else if (operationText == "*=") { return Operation::MultiplyAssignment; }
        else if (operationText == "/=") { return Operation::DivideAssignment; }
        else if (operationText == "%=") { return Operation::ModAssignment; }
        else if (operationText == "+=") { return Operation::AddAssignment; }
        else if (operationText == "-=") { return Operation::MinusAssignment; }
        else if (operationText == "<<") { return Operation::ShiftLeft; }
        else if (operationText == ">>") { return Operation::ShiftRight; }
        else if (operationText == "&") { return Operation::BitwiseAnd; }
        else if (operationText == "^") { return Operation::BitwiseXor; }
        else if (operationText == "|") { return Operation::BitwiseOr; }
        else if (operationText == "<<=") { return Operation::LeftShiftAssignment; }
        else if (operationText == ">>=") { return Operation::RightShiftAssignment; }
        else if (operationText == "&=") { return Operation::AndAssignment; }
        else if (operationText == "^=") { return Operation::XorAssignment; }
        else if (operationText == "|=") { return Operation::OrAssignment; }

        LogError(std::format("unknown operation '{}'", operationText));
        return Operation::None;
    }

public:
    LLVMBackend()
    {
        Init();
        CompilerManager::Instance().Register(this);
    }
    ~LLVMBackend()
    {
        CompilerManager::Instance().Unregister(this);

        builder.release();
        module.release();

        // context is last to be released.
        context.release();
    }

    void DumpState() const
    {
        std::cerr << "  File: " << (sourceFileName.empty() ? "<unknown>" : sourceFileName) << "\n";
        std::cerr << "  Location: " << currentLine << ":" << currentColumn << "\n";

        if (currentFunction)
            std::cerr << "  Function: " << currentFunction->getName().str() << "\n";
        else
            std::cerr << "  Function: <none>\n";

        std::cerr << "  Scope depth: " << stackNamedVariable.size() << "\n";
        if (!stackNamedVariable.empty())
        {
            const auto& top = stackNamedVariable.back();
            std::cerr << "  Top scope locals:";
            for (const auto& [name, _] : top.namedVariable)
                std::cerr << " " << name;
            std::cerr << "\n";
        }

        std::cerr << "  Structs registered: " << dataStructures.size() << "\n";
        std::cerr << "  Functions registered: " << functionTable.size() << "\n";
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

    void SetCurrentDebugLocation(size_t line, size_t col = 0)
    {
        if (!currentSubprogram || !diBuilder) return;
        builder->SetCurrentDebugLocation(llvm::DILocation::get(*context, (unsigned)line, (unsigned)col, currentSubprogram));
    }

    void ClearCurrentSubprogram()
    {
        currentSubprogram = nullptr;
        builder->SetCurrentDebugLocation(llvm::DebugLoc());
    }

    // Terminate all unterminated basic blocks in every function in the module, then pop stack
    // frames back down to targetDepth (without running destructors — used in error-skip paths).
    // Iterates the whole module because lambdas save/restore builder state: when an exception
    // fires inside a lambda the outer function's blocks may also be unterminated.
    void AbortFunctionBlocks(size_t targetDepth)
    {
        for (auto& fn : *module)
        {
            for (auto& bb : fn)
            {
                if (!bb.getTerminator())
                {
                    builder->SetInsertPoint(&bb);
                    builder->CreateUnreachable();
                }
            }
        }
        while (stackNamedVariable.size() > targetDepth)
            stackNamedVariable.pop_back();
    }

    struct BuilderState
    {
        llvm::IRBuilder<>::InsertPoint ip;
        llvm::Function* function = nullptr;
        llvm::DISubprogram* subprogram = nullptr;
    };

    BuilderState SaveBuilderState() const
    {
        return { builder->saveIP(), currentFunction, currentSubprogram };
    }

    void RestoreBuilderState(const BuilderState& state)
    {
        builder->restoreIP(state.ip);
        currentFunction = state.function;
        currentSubprogram = state.subprogram;
        if (state.subprogram)
            builder->SetCurrentDebugLocation(llvm::DILocation::get(*context, 0, 0, state.subprogram));
        else
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
                LogError(std::format("unknown parent interface: '{}'", parentName));
                continue;
            }
            for (const auto& m : it->second)
                inherited.push_back(m);
        }
        inherited.insert(inherited.end(), methods.begin(), methods.end());
        interfaceTable[name] = std::move(inherited);
        interfaceParents[name] = parentNames;
    }

    static bool IsPrimitiveTypeName(const std::string& name)
    {
        static const std::unordered_set<std::string> primitives = {
            "int", "char", "short", "long", "bool", "void",
            "float", "double",
            "i8", "i16", "i32", "i64",
            "u8", "u16", "u32", "u64",
        };
        return primitives.count(name) > 0;
    }

    void RegisterTypeAlias(const std::string& alias, const std::string& target)
    {
        typeAliases[alias] = target;
    }

    std::string ResolveTypeAlias(const std::string& name) const
    {
        auto it = typeAliases.find(name);
        return (it != typeAliases.end()) ? it->second : name;
    }

    bool IsInterfaceType(const std::string& name) const
    {
        return interfaceTable.count(ResolveTypeAlias(name)) > 0;
    }

    bool HasInterfaceMethod(const std::string& ifaceName, const std::string& methodName) const
    {
        auto it = interfaceTable.find(ifaceName);
        if (it == interfaceTable.end()) return false;
        for (const auto& m : it->second)
            if (m.Name == methodName) return true;
        return false;
    }

    llvm::StructType* GetFatPtrType() const
    {
        const char* fatPtrName = "__iface_fat_ptr";
        if (auto* existing = llvm::StructType::getTypeByName(*context, fatPtrName))
            return existing;
        auto ptrTy = builder->getInt8Ty()->getPointerTo();
        return llvm::StructType::create(*context, { ptrTy, ptrTy }, fatPtrName);
    }

    // {i8* fnptr, i8* envptr} — storage type for all in-function function<T> variables.
    llvm::StructType* GetClosureFatPtrType() const
    {
        const char* name = "__closure_fat_ptr";
        if (auto* existing = llvm::StructType::getTypeByName(*context, name)) return existing;
        auto ptrTy = builder->getInt8Ty()->getPointerTo();
        return llvm::StructType::create(*context, { ptrTy, ptrTy }, name);
    }

    // Creates __shim_<name>(i8* env, original params...) that ignores env and calls original.
    llvm::Function* GetOrCreateFunctionShim(llvm::Function* original)
    {
        std::string shimName = "__shim_" + original->getName().str();
        if (auto* existing = module->getFunction(shimName)) return existing;

        auto* origTy  = original->getFunctionType();
        auto* i8PtrTy = builder->getInt8Ty()->getPointerTo();

        std::vector<llvm::Type*> shimParamTypes = {i8PtrTy};
        for (auto* paramTy : origTy->params())
            shimParamTypes.push_back(paramTy);

        auto* shimTy = llvm::FunctionType::get(origTy->getReturnType(), shimParamTypes, false);
        auto* shim   = llvm::Function::Create(shimTy, llvm::Function::InternalLinkage, shimName, module.get());

        auto* entry = llvm::BasicBlock::Create(*context, "entry", shim);
        llvm::IRBuilder<> b(entry);

        std::vector<llvm::Value*> callArgs;
        auto argIt = shim->arg_begin();
        ++argIt; // skip env
        for (; argIt != shim->arg_end(); ++argIt)
            callArgs.push_back(&*argIt);

        if (origTy->getReturnType()->isVoidTy())
        {
            b.CreateCall(origTy, original, callArgs);
            b.CreateRetVoid();
        }
        else
        {
            b.CreateRet(b.CreateCall(origTy, original, callArgs));
        }
        return shim;
    }

    // Wraps a named function in a {shim_i8ptr, null} closure fat struct value.
    llvm::Value* WrapBareValueAsFatStruct(llvm::Function* original)
    {
        auto* i8PtrTy  = builder->getInt8Ty()->getPointerTo();
        auto* shim     = GetOrCreateFunctionShim(original);
        auto* shimAsI8 = builder->CreateBitCast(shim, i8PtrTy, "shim_i8");
        auto* nullEnv  = llvm::ConstantPointerNull::get(i8PtrTy);
        auto* closureTy = GetClosureFatPtrType();
        llvm::Value* fat = llvm::UndefValue::get(closureTy);
        fat = builder->CreateInsertValue(fat, shimAsI8, {0u});
        fat = builder->CreateInsertValue(fat, nullEnv,  {1u});
        return fat;
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
            LogError(std::format("GetOrCreateVTable: unknown interface '{}'", ifaceName));
            return nullptr;
        }

        auto ptrTy = builder->getInt8Ty()->getPointerTo();
        std::vector<llvm::Constant*> entries;

        // First entry: pointer to unique per-struct type descriptor global (for 'is'/'as' checks)
        // Lazily create the descriptor here if CreateStructType wasn't called first.
        if (sd.typeDescriptor == nullptr)
        {
            sd.typeDescriptor = new llvm::GlobalVariable(
                *module, builder->getInt8Ty(), true,
                llvm::GlobalValue::InternalLinkage,
                builder->getInt8(0), structName + "_typedesc");
        }
        entries.push_back(sd.typeDescriptor);

        for (const auto& method : ifaceIt->second)
        {
            llvm::Function* fn = nullptr;
            auto funcIt = functionTable.find(method.Name);
            if (funcIt != functionTable.end())
            {
                // method.Parameters excludes 'this'; sym.Parameters[0] is 'this'.
                // Match by struct this-pointer AND by the remaining parameter types/count,
                // so overloads like getInt(string) vs getInt(int) resolve correctly.
                size_t expectedParamCount = 1 + method.Parameters.size();
                for (const auto& sym : funcIt->second)
                {
                    if (sym.Parameters.size() != expectedParamCount) continue;
                    if (sym.Parameters[0].TypeName != structName || !sym.Parameters[0].Pointer) continue;
                    bool paramsMatch = true;
                    for (size_t pi = 0; pi < method.Parameters.size(); pi++)
                    {
                        if (sym.Parameters[1 + pi].TypeName != method.Parameters[pi].TypeName)
                        {
                            paramsMatch = false;
                            break;
                        }
                    }
                    if (paramsMatch)
                    {
                        fn = sym.Function;
                        break;
                    }
                }
            }
            if (fn == nullptr)
            {
                LogError(std::format("GetOrCreateVTable: '{}' does not implement '{}::{}'", structName, ifaceName, method.Name));
                entries.push_back(llvm::ConstantPointerNull::get(ptrTy));
            }
            else
            {
                entries.push_back(fn);
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

    llvm::Value* BuildInterfaceFatValue(llvm::GlobalVariable* vtable, llvm::Value* dataPtr)
    {
        auto fatTy = GetFatPtrType();
        auto ptrTy = builder->getInt8Ty()->getPointerTo();
        llvm::Value* v = llvm::UndefValue::get(fatTy);
        v = builder->CreateInsertValue(v, builder->CreateBitCast(vtable, ptrTy), { 0u });
        v = builder->CreateInsertValue(v, builder->CreateBitCast(dataPtr, ptrTy), { 1u });
        return v;
    }

    // Returns true if the given constant is a pooled string literal (length known at compile time).
    bool IsStringLiteralConstant(llvm::Constant* c) const
    {
        return stringLiteralLenByPtr.count(c) > 0;
    }

    // Wraps a raw i8* string literal pointer in a string struct { _ptr, _len } by value.
    // Called automatically when assigning a string literal to a string-typed variable.
    llvm::Value* WrapStringLiteralAsString(llvm::Value* strLitPtr)
    {
        auto* strTy = llvm::StructType::getTypeByName(*context, "string");
        if (!strTy) return strLitPtr;

        int32_t len = 0;
        if (auto* c = llvm::dyn_cast<llvm::Constant>(strLitPtr))
        {
            auto it = stringLiteralLenByPtr.find(c);
            if (it != stringLiteralLenByPtr.end())
                len = it->second;
        }

        llvm::Value* strVal = llvm::UndefValue::get(strTy);
        strVal = builder->CreateInsertValue(strVal, strLitPtr, { 0u });
        strVal = builder->CreateInsertValue(strVal, builder->getInt32(len), { 1u });
        return strVal;
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

        // Method indices start at 1 (slot 0 is type ID)
        auto fnPtrField = builder->CreateGEP(ptrTy, vtablePtr, builder->getInt32(methodIdx + 1));
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
            if (param.IsInterface && !nv.TypeAndValue.IsInterface)
            {
                // Concrete struct/pointer -> interface fat ptr upconversion.
                std::string structName = nv.TypeAndValue.TypeName;
                if (structName.empty() && nv.BaseType)
                {
                    if (auto* st = llvm::dyn_cast<llvm::StructType>(nv.BaseType))
                        structName = st->getName().str();
                }
                auto vtable = GetOrCreateVTable(structName, param.TypeName);
                llvm::Value* argDataPtr = nullptr;
                if (nv.TypeAndValue.Pointer)
                {
                    argDataPtr = nv.Primary != nullptr ? nv.Primary : CreateLoad(nv.Storage);
                }
                else if (nv.Storage != nullptr)
                {
                    argDataPtr = nv.Storage;
                }
                else
                {
                    auto structTy = nv.BaseType ? nv.BaseType : GetType(nv.TypeAndValue);
                    auto tempAlloca = builder->CreateAlloca(structTy);
                    builder->CreateStore(nv.Primary, tempAlloca);
                    argDataPtr = tempAlloca;
                }
                callArgs.push_back(BuildInterfaceFatValue(vtable, argDataPtr));
            }
            else if (param.IsInterface && nv.TypeAndValue.IsInterface)
            {
                // Interface -> interface: pass fat struct by value.
                llvm::Value* val = nv.Primary ? nv.Primary : CreateLoad(nv.Storage);
                callArgs.push_back(val);
            }
            else if (param.Pointer)
            {
                // Storage may be a promoted-param alloca holding the pointer; load to get the value.
                if (nv.Primary == nullptr && nv.Storage != nullptr && llvm::isa<llvm::AllocaInst>(nv.Storage))
                    callArgs.push_back(CreateLoad(nv.Storage));
                else
                    callArgs.push_back(nv.GetValue());
            }
            else
            {
                auto val = nv.Primary != nullptr ? nv.Primary : CreateLoad(nv.Storage);
                val = Upconvert(val, GetType(param), nv.TypeAndValue.IsUnsignedInteger() != -1);
                callArgs.push_back(val);
            }
        }

        return builder->CreateCall(fnTy, fnPtr, callArgs);
    }

    // Helper for building string NamedVariables for reflection visitor calls.
    // Wraps a string literal pointer into a string struct value.
    NamedVariable MakeStringLiteralNV(const std::string& text)
    {
        NamedVariable nv;
        auto* rawPtr = CreateGlobalString("reflect_str", text);
        nv.Primary = WrapStringLiteralAsString(rawPtr);
        nv.TypeAndValue.TypeName = "string";
        return nv;
    }

    void RegisterDestructor(const std::string& structName, llvm::Function* fn)
    {
        dataStructures[structName].Destructor = fn;
    }

    /// Returns i64 sizeof(type) as a compile-time constant.
    llvm::Value* GetTypeSizeBytes(llvm::Type* type)
    {
        if (!type)
        {
            LogError("GetTypeSizeBytes: null type pointer");
            return nullptr;
        }
        const llvm::DataLayout& dl = module->getDataLayout();
        uint64_t size = dl.getTypeAllocSize(type);
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), size);
    }

    /// Returns i64 alignof(type) as a compile-time constant.
    llvm::Value* GetTypeAlignBytes(llvm::Type* type)
    {
        if (!type)
        {
            LogError("GetTypeAlignBytes: null type pointer");
            return nullptr;
        }
        const llvm::DataLayout& dl = module->getDataLayout();
        uint64_t align = dl.getABITypeAlign(type).value();
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), align);
    }

    void RegisterStructInterfaces(const std::string& structName, const std::vector<std::string>& interfaces)
    {
        dataStructures[structName].Interfaces = interfaces;
    }

    std::vector<std::string> GetStructInterfaces(const std::string& structName) const
    {
        auto it = dataStructures.find(structName);
        if (it == dataStructures.end()) return {};
        return it->second.Interfaces;
    }

    bool TypeImplementsInterface(const std::string& typeName, const std::string& ifaceName) const
    {
        auto it = dataStructures.find(typeName);
        if (it == dataStructures.end()) return false;
        const auto& ifaces = it->second.Interfaces;
        return std::find(ifaces.begin(), ifaces.end(), ifaceName) != ifaces.end();
    }

    void VerifyInterfaceImplementation(const std::string& structName, const std::string& interfaceName)
    {
        auto ifaceIt = interfaceTable.find(interfaceName);
        if (ifaceIt == interfaceTable.end())
        {
            LogError(std::format("unknown interface: '{}'", interfaceName));
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
                LogError(std::format("class '{}' does not implement '{}::{}'", structName, interfaceName, method.Name));
            }
        }
    }

    // Lazily synthesizes void __reflect_T(T* obj, IReflector visitor) for the given struct type.
    // Returns the synthesized function, or nullptr if structName is not a known struct.
    // Safe to call mid-emission: uses SaveBuilderState/RestoreBuilderState.
    llvm::Function* SynthesizeReflectFunction(const std::string& structName)
    {
        auto compiler = this;

        // Guard: if already synthesized, return existing function
        if (compiler->synthesizedReflectFunctions.count(structName))
        {
            if (auto fn = compiler->module->getFunction("__reflect_" + structName))
                return fn;
        }

        // Insert into set before emitting body (handles A->B->A cycles)
        compiler->synthesizedReflectFunctions.insert(structName);

        // Lookup struct
        auto it = compiler->dataStructures.find(structName);
        if (it == compiler->dataStructures.end())
        {
            LogError(std::format("reflect: unknown struct '{}'", structName));
            return nullptr;
        }
        auto& sd = it->second;
        if (!sd.StructType)
        {
            LogError(std::format("reflect: struct '{}' has no LLVM type", structName));
            return nullptr;
        }

        // Save builder state (includes currentFunction, currentSubprogram)
        auto savedState = compiler->SaveBuilderState();

        // Build parameter types
        TypeAndValue objParam;
        objParam.TypeName = structName;
        objParam.Pointer = true;
        objParam.VariableName = "obj";

        TypeAndValue visitorParam;
        visitorParam.TypeName = "IReflector";
        visitorParam.IsInterface = true;
        visitorParam.IsInterfacePointer = false;
        visitorParam.Pointer = true;
        visitorParam.VariableName = "visitor";

        TypeAndValue voidReturn;
        voidReturn.TypeName = "void";

        // Create function definition
        std::string fnName = "__reflect_" + structName;
        auto* fn = compiler->CreateFunctionDefinition(fnName, voidReturn, {objParam, visitorParam});
        if (!fn)
        {
            LogError(std::format("reflect: failed to create function for '{}'", structName));
            compiler->RestoreBuilderState(savedState);
            return nullptr;
        }
        fn->setLinkage(llvm::Function::InternalLinkage);

        // Retrieve parameters
        // obj is a pointer argument
        llvm::Value* objPtr = fn->getArg(0);

        // visitor fat-ptr alloca (created by createFunctionBlock)
        llvm::Value* visitorAlloca = nullptr;
        if (!compiler->stackNamedVariable.empty())
        {
            auto& topFrame = compiler->stackNamedVariable.back();
            auto frameIt = topFrame.functionArgument.find("visitor");
            if (frameIt != topFrame.functionArgument.end())
                visitorAlloca = frameIt->second.Storage;
        }
        if (!visitorAlloca)
        {
            LogError("reflect: failed to retrieve visitor alloca");
            compiler->RestoreBuilderState(savedState);
            return fn;
        }

        // Emit per-field code
        for (size_t i = 0; i < sd.StructFields.size(); i++)
        {
            const auto& field = sd.StructFields[i];

            // Skip [Private] fields
            bool isPrivate = false;
            for (const auto& ann : field.Annotations)
            {
                if (ann.Name == "Private")
                {
                    isPrivate = true;
                    break;
                }
            }
            if (isPrivate) continue;

            std::string displayName = field.VariableName;

            // GEP to field
            auto* gep = compiler->builder->CreateStructGEP(sd.StructType, objPtr, (unsigned)i,
                field.VariableName + "_ptr");

            // Dispatch on field type
            std::string typeName = field.TypeName;
            bool isPtr = field.Pointer;
            bool isInterface = field.IsInterface;

            // Integer types
            if (typeName == "int" || typeName == "i8" || typeName == "i16" || typeName == "i32"
                || typeName == "i64" || typeName == "u8" || typeName == "u16" || typeName == "u32"
                || typeName == "u64")
            {
                if (!isPtr && !isInterface)
                {
                    auto* raw = compiler->builder->CreateLoad(compiler->GetType(field), gep);
                    auto* widened = compiler->Upconvert(raw, compiler->builder->getInt32Ty(),
                        field.IsUnsignedInteger() != -1);
                    auto nameNV = compiler->MakeStringLiteralNV(displayName);
                    NamedVariable intNV;
                    intNV.Primary = widened;
                    intNV.TypeAndValue.TypeName = "int";
                    compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitInt",
                        {nameNV, intNV});
                }
            }
            // Bool type
            else if (typeName == "bool" && !isPtr && !isInterface)
            {
                auto* raw = compiler->builder->CreateLoad(compiler->GetType(field), gep);
                auto nameNV = compiler->MakeStringLiteralNV(displayName);
                NamedVariable boolNV;
                boolNV.Primary = raw;
                boolNV.TypeAndValue.TypeName = "bool";
                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitBool",
                    {nameNV, boolNV});
            }
            // Float / double types
            else if ((typeName == "float" || typeName == "double") && !isPtr && !isInterface)
            {
                llvm::Value* raw = compiler->builder->CreateLoad(compiler->GetType(field), gep);
                // Cast to float if necessary
                if (typeName == "double")
                    raw = compiler->builder->CreateFPCast(raw, compiler->builder->getFloatTy());
                auto nameNV = compiler->MakeStringLiteralNV(displayName);
                NamedVariable floatNV;
                floatNV.Primary = raw;
                floatNV.TypeAndValue.TypeName = "float";
                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitFloat",
                    {nameNV, floatNV});
            }
            // String type
            else if (typeName == "string" && !isPtr && !isInterface)
            {
                auto nameNV = compiler->MakeStringLiteralNV(displayName);
                NamedVariable strNV;
                strNV.Storage = gep;
                strNV.TypeAndValue.TypeName = "string";
                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitString",
                    {nameNV, strNV});
            }
            // Struct value type (nested struct)
            else if (!isPtr && !isInterface && compiler->dataStructures.count(typeName))
            {
                compiler->SynthesizeReflectFunction(typeName);
                auto nameNV = compiler->MakeStringLiteralNV(displayName);
                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "beginObject",
                    {nameNV});

                // Call __reflect_FieldType(gep, visitor)
                auto* nestedFn = compiler->module->getFunction("__reflect_" + typeName);
                if (nestedFn)
                {
                    compiler->builder->CreateCall(nestedFn->getFunctionType(), nestedFn, {gep, visitorAlloca});
                }

                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "endObject", {});
            }
            // Struct pointer (nested struct reference)
            else if (isPtr && !isInterface && compiler->dataStructures.count(typeName))
            {
                compiler->SynthesizeReflectFunction(typeName);
                auto* ptrVal = compiler->builder->CreateLoad(compiler->GetType(field), gep);
                auto* ptrType = llvm::cast<llvm::PointerType>(ptrVal->getType());
                auto* isNull = compiler->builder->CreateICmpEQ(ptrVal,
                    llvm::ConstantPointerNull::get(ptrType));

                auto* thenBB = compiler->CreateBasicBlock("reflect_null", fn);
                auto* elseBB = compiler->CreateBasicBlock("reflect_obj", fn);
                auto* mergeBB = compiler->CreateBasicBlock("reflect_merge", fn);
                compiler->builder->CreateCondBr(isNull, thenBB, elseBB);

                // null branch: visitNull
                compiler->builder->SetInsertPoint(thenBB);
                auto nameNV = compiler->MakeStringLiteralNV(displayName);
                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitNull", {nameNV});
                compiler->builder->CreateBr(mergeBB);

                // non-null branch: beginObject + recurse + endObject
                compiler->builder->SetInsertPoint(elseBB);
                nameNV = compiler->MakeStringLiteralNV(displayName);
                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "beginObject", {nameNV});
                auto* nestedFn2 = compiler->module->getFunction("__reflect_" + typeName);
                if (nestedFn2)
                {
                    auto* visitorVal = compiler->builder->CreateLoad(compiler->GetFatPtrType(),
                        visitorAlloca);
                    compiler->builder->CreateCall(nestedFn2->getFunctionType(), nestedFn2,
                        {ptrVal, visitorVal});
                }
                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "endObject", {});
                compiler->builder->CreateBr(mergeBB);

                compiler->builder->SetInsertPoint(mergeBB);
            }
            // list<T>*, dictionary<K,V>*, void*, function, interface => skip (Phase 2+)
        }

        // Emit return
        compiler->builder->CreateRetVoid();

        // Pop stack frame
        if (!compiler->stackNamedVariable.empty())
            compiler->stackNamedVariable.pop_back();

        // Restore builder state
        compiler->RestoreBuilderState(savedState);

        return fn;
    }

    llvm::GlobalVariable* CreateGlobalVariable(TypeAndValue typeValue, llvm::Constant* initValue, bool threadLocal = false)
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
            // Coerce a null-value (e.g. nullptr) to the destination type when types differ.
            // Handles fat-ptr structs and function pointers initialized with = nullptr.
            else if (initValue->isNullValue() && initValue->getType() != destinationType)
            {
                initValue = llvm::Constant::getNullValue(destinationType);
            }
        }
        else
        {
            // Zero-initialize — works for all types: primitives, pointers, structs, fat-ptrs.
            initValue = llvm::Constant::getNullValue(destinationType);
        }

        auto gVar = new llvm::GlobalVariable(
            *module,
            destinationType,
            false, // isConstant
            llvm::GlobalValue::LinkageTypes::ExternalLinkage,
            initValue, // Initial value
            typeValue.VariableName // Name
        );

        if (threadLocal)
            gVar->setThreadLocalMode(llvm::GlobalVariable::GeneralDynamicTLSModel);

        globalNamedVariable[typeValue.VariableName] = gVar;
        globalVariableTypes[typeValue.VariableName] = typeValue;

        if (symbolSink_ && !typeValue.VariableName.empty())
            symbolSink_->RegisterVariable(typeValue.VariableName, typeValue.TypeName);

        return gVar;
    }

    llvm::AllocaInst* CreateLocalVariable(TypeAndValue typeValue, llvm::Type* autoType = nullptr, llvm::Value* arraySize = nullptr, size_t line = 0)
    {
        auto type = GetType(typeValue, autoType);
        auto alloc = builder->CreateAlloca(type, arraySize, typeValue.VariableName);
        auto& namedVariable = stackNamedVariable.back().namedVariable[typeValue.VariableName];
        namedVariable.Storage = alloc;
        namedVariable.TypeAndValue = typeValue;
        namedVariable.BaseType = type;

        if (symbolSink_ && !typeValue.VariableName.empty())
            symbolSink_->RegisterVariable(typeValue.VariableName, typeValue.TypeName);

        if (diBuilder && currentSubprogram && (unsigned)line > 0)
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

        if (symbolSink_ && !typeValue.VariableName.empty())
            symbolSink_->RegisterVariable(typeValue.VariableName, typeValue.TypeName);
    }

    llvm::AllocaInst* CreateAlloca(llvm::Type* type)
    {
        return builder->CreateAlloca(type, nullptr);
    }

    // Register an alloca of struct type as the implicit 'this' pointer in the current scope.
    // Used by parameterized constructors: the alloca IS the pointer (same layout as a method's
    // incoming this* argument), so GetMemberVariable can GEP through it.
    void RegisterThisPointer(const TypeAndValue& tv, llvm::Value* storage, llvm::Type* baseType)
    {
        NamedVariable namedVar{
            .TypeAndValue = tv,
            .BaseType = baseType,
            .Primary = nullptr,
            .Storage = storage,
        };
        stackNamedVariable.back().functionArgument[tv.VariableName] = namedVar;

        // Also register under "this" so explicit `this->field` resolves correctly.
        // Primary = storage (the alloca) so LoadNamedVariable returns it directly as a pointer
        // rather than loading through it (which would yield the struct by value, not a pointer).
        TypeAndValue thisTv = tv;
        thisTv.VariableName = "this";
        NamedVariable thisVar{
            .TypeAndValue = thisTv,
            .BaseType = baseType,
            .Primary = storage,
            .Storage = nullptr,
        };
        stackNamedVariable.back().functionArgument["this"] = thisVar;
    }

    llvm::Value* CreateIncrement(llvm::Value* destination, int amount)
    {
        llvm::LoadInst* loadInst = CreateLoad(destination);

        if (loadInst->getType()->isPointerTy())
        {
            // Pointer increment/decrement: byte arithmetic via i8 GEP.
            auto* step = llvm::ConstantInt::get(builder->getInt64Ty(), amount);
            auto* newPtr = builder->CreateGEP(builder->getInt8Ty(), loadInst, step, "ptrinc");
            return builder->CreateStore(newPtr, destination);
        }

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
        // When the base is a GlobalVariable (a Constant), IRBuilder's ConstantFolder
        // tries to fold the GEP into a ConstantExpr. In LLVM 18 opaque pointer mode
        // this internally calls ConstantExpr::getCast with an invalid type combination,
        // triggering an assertion. Bypass the folder by inserting a real GEP instruction.
        if (llvm::isa<llvm::GlobalVariable>(structAlloc))
        {
            llvm::Value* idxs[] = { builder->getInt32(0), builder->getInt32(index) };
            auto* gep = llvm::GetElementPtrInst::CreateInBounds(structType, structAlloc, idxs);
            builder->Insert(gep, variableName);
            return gep;
        }
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

    llvm::StoreInst* CreateAssignment(llvm::Value* value, llvm::Value* destination, bool srcIsUnsigned = false, llvm::Type* explicitDestType = nullptr)
    {
        auto destType = explicitDestType ? explicitDestType : GetTypeFromStorage(destination);
        if (destType == builder->getInt1Ty())
        {
            value = builder->CreateICmpNE(value, llvm::ConstantInt::get(value->getType(), 0), "tobool");
        }
        else
        {
            // When assigning a raw ptr (string literal) into a `string` struct variable,
            // wrap it into the { _ptr, _len } struct instead of attempting an invalid bitcast.
            auto* strTy = llvm::StructType::getTypeByName(*context, "string");
            if (strTy && destType == strTy && value->getType()->isPointerTy() && value->getType() != strTy)
            {
                value = WrapStringLiteralAsString(value);
            }
            else
            {
                // Upconvert handles integer widening with correct sign semantics (SExt for signed, ZExt for unsigned).
                // CreateCast handles all other conversions (truncation, int<->float, ptr<->int, etc.).
                value = Upconvert(value, destType, srcIsUnsigned);
                if (value->getType() != destType)
                    value = CreateCast(value, destType);
            }
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

    llvm::Value* Upconvert(llvm::Value* value, llvm::Value* destination, bool srcIsUnsigned = false) const
    {
        auto destType = GetTypeFromStorage(destination);
        return Upconvert(value, destType, srcIsUnsigned);
    }

    llvm::Value* Upconvert(llvm::Value* value, llvm::Type* destType, bool srcIsUnsigned = false) const
    {
        auto srcType = value->getType();
        if (srcType->isIntegerTy() && destType->isIntegerTy())
        {
            auto targetSize = destType->getIntegerBitWidth();
            auto srcSize = srcType->getIntegerBitWidth();

            if (srcSize < targetSize)
            {
                // i1 (bool) and unsigned types zero-extend; signed types sign-extend
                if (srcSize == 1 || srcIsUnsigned)
                    return builder->CreateZExt(value, destType);
                return builder->CreateSExt(value, destType);
            }
        }
        else if (srcType->isFloatingPointTy() && destType->isFloatingPointTy())
        {
            auto targetSize = destType->getScalarSizeInBits();
            auto srcSize = srcType->getScalarSizeInBits();

            // Widening only (e.g. float -> double). Narrowing (double -> float) is
            // not handled here — use a typed literal (0.0f) or an explicit cast at
            // the source level so types match before reaching this path.
            if (srcSize < targetSize)
                return builder->CreateFPExt(value, destType);
        }
        else if (srcType->isIntegerTy() && destType->isFloatingPointTy())
        {
            // Integer literal initializer for a float/double field (e.g. float x = 0)
            if (auto* constInt = llvm::dyn_cast<llvm::ConstantInt>(value))
                return llvm::ConstantFP::get(destType, (double)constInt->getSExtValue());
            return builder->CreateSIToFP(value, destType);
        }
        else if (srcType->isIntegerTy() && destType->isPointerTy())
        {
            // Integer 0 assigned to a pointer field — produce a proper null/ptr constant.
            if (auto* constInt = llvm::dyn_cast<llvm::ConstantInt>(value))
            {
                if (constInt->isZero())
                    return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(destType));
                return builder->CreateIntToPtr(constInt, destType);
            }
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
        if (srcType->isPointerTy() && destType->isPointerTy())
        {
            return 0;
        }
        if (srcType->isPointerTy() || destType->isPointerTy())
        {
            return -1;  // pointer vs non-pointer: incompatible
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
            // i1 (bool) and unsigned types zero-extend; signed types sign-extend
            if (srcBits == 1 || !isSigned)
                return builder->CreateZExt(value, destType);
            return builder->CreateSExt(value, destType);
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
            return builder->CreateSIToFP(value, destType);
        }

        // Float -> Integer
        if (srcType->isFloatingPointTy() && destType->isIntegerTy())
        {
            return builder->CreateFPToSI(value, destType);
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
    llvm::StructType* CreateStructType(std::string name, std::vector<LLVMBackend::DeclTypeAndValue> typeAndValues)
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
                dataStructures[name].typeDescriptor = new llvm::GlobalVariable(
                    *module, builder->getInt8Ty(), true,
                    llvm::GlobalValue::InternalLinkage,
                    builder->getInt8(0), name + "_typedesc");

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

            // Reuse the existing named type from the LLVM context if present — it survives
            // ResetForReanalysis() and would otherwise get renamed to "name.1" on re-analysis,
            // breaking TypeName-based overload matching in the LSP.
            llvm::StructType* opaqueStruct = llvm::StructType::getTypeByName(*context, name);
            if (!opaqueStruct)
                opaqueStruct = llvm::StructType::create(*context, name);
            dataStructures[name].StructType = opaqueStruct;
            dataStructures[name].typeDescriptor = new llvm::GlobalVariable(
                *module, builder->getInt8Ty(), true,
                llvm::GlobalValue::InternalLinkage,
                builder->getInt8(0), name + "_typedesc");
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
            LogError(std::format("unknown type '{}'", typeName));
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
        // In LLVM 18 opaque-pointer mode the GlobalVariable* is already a ptr —
        // no ConstantExpr GEP needed.
        stringPool[text] = gv;
        stringLiteralLenByPtr[gv] = (int32_t)text.size();
        return gv;
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

        // Pointer operations: must run before Upconvert, which would corrupt pointer/int pairs.
        if (left->getType()->isPointerTy() || right->getType()->isPointerTy())
        {
            auto* i64Ty = builder->getInt64Ty();
            bool leftIsPtr  = left->getType()->isPointerTy();
            bool rightIsPtr = right->getType()->isPointerTy();
            switch (op)
            {
            case Operation::Less:
            case Operation::Greater:
            case Operation::LessEqual:
            case Operation::GreaterEqual:
            {
                // Address comparison: convert both sides to i64 and compare as unsigned integers.
                if (leftIsPtr)  left  = builder->CreatePtrToInt(left,  i64Ty);
                else if (!left->getType()->isIntegerTy(64))  left  = builder->CreateSExt(left,  i64Ty);
                if (rightIsPtr) right = builder->CreatePtrToInt(right, i64Ty);
                else if (!right->getType()->isIntegerTy(64)) right = builder->CreateSExt(right, i64Ty);
                auto pred = op == Operation::Less      ? llvm::ICmpInst::ICMP_ULT :
                            op == Operation::Greater   ? llvm::ICmpInst::ICMP_UGT :
                            op == Operation::LessEqual ? llvm::ICmpInst::ICMP_ULE :
                                                         llvm::ICmpInst::ICMP_UGE;
                return builder->CreateICmp(pred, left, right);
            }
            case Operation::BitwiseAnd:
            case Operation::AndAssignment:
            case Operation::BitwiseOr:
            case Operation::OrAssignment:
            case Operation::BitwiseXor:
            case Operation::XorAssignment:
            {
                // Bitwise masking: ptrtoint, op, inttoptr.  Result type matches the pointer side.
                auto* ptrTy = leftIsPtr ? left->getType() : right->getType();
                if (leftIsPtr)  left  = builder->CreatePtrToInt(left,  i64Ty);
                else if (!left->getType()->isIntegerTy(64))  left  = builder->CreateZExtOrTrunc(left,  i64Ty);
                if (rightIsPtr) right = builder->CreatePtrToInt(right, i64Ty);
                else if (!right->getType()->isIntegerTy(64)) right = builder->CreateZExtOrTrunc(right, i64Ty);
                llvm::Value* res;
                switch (op)
                {
                case Operation::BitwiseAnd: case Operation::AndAssignment: res = builder->CreateAnd(left, right); break;
                case Operation::BitwiseOr:  case Operation::OrAssignment:  res = builder->CreateOr(left,  right); break;
                default:                                                    res = builder->CreateXor(left, right); break;
                }
                return builder->CreateIntToPtr(res, ptrTy);
            }
            case Operation::Add:
            case Operation::AddAssignment:
            case Operation::Subtract:
            case Operation::MinusAssignment:
            {
                if (leftIsPtr && rightIsPtr)
                {
                    // ptr - ptr: byte difference as i64.
                    left  = builder->CreatePtrToInt(left,  i64Ty);
                    right = builder->CreatePtrToInt(right, i64Ty);
                    return builder->CreateSub(left, right, "ptrdiff");
                }
                // ptr ± int (no element-type context here): byte arithmetic via i8 GEP.
                auto* ptrVal = leftIsPtr  ? left  : right;
                auto* intVal = leftIsPtr  ? right : left;
                if ((op == Operation::Subtract || op == Operation::MinusAssignment) && leftIsPtr)
                    intVal = builder->CreateNeg(intVal, "neg");
                intVal = Upconvert(intVal, i64Ty);
                return builder->CreateGEP(builder->getInt8Ty(), ptrVal, intVal, "ptrarith");
            }
            default:
                break;  // ==, !=, logical ops, etc. fall through to the integer path below
            }
        }

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
            case Operation::Modulo:
            {
                return builder->CreateFRem(left, right);
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
            case Operation::Modulo:
            {
                return builder->CreateSRem(left, right);
            }
            case Operation::Equal:
            {
                // Fat-ptr compared to nullptr: ICmp on a distinguishing field.
                // Closure fat ptr: compare fnptr (field 0); interface fat ptr: data ptr (field 1).
                if (left->getType() != right->getType() && left->getType()->isStructTy())
                {
                    auto* st = llvm::cast<llvm::StructType>(left->getType());
                    unsigned field = (st->getName() == "__closure_fat_ptr") ? 0u : 1u;
                    left  = builder->CreateExtractValue(left, {field});
                    right = llvm::Constant::getNullValue(left->getType());
                }
                return builder->CreateICmp(llvm::ICmpInst::ICMP_EQ, left, right);
            }
            case Operation::NotEqual:
            {
                if (left->getType() != right->getType() && left->getType()->isStructTy())
                {
                    auto* st = llvm::cast<llvm::StructType>(left->getType());
                    unsigned field = (st->getName() == "__closure_fat_ptr") ? 0u : 1u;
                    left  = builder->CreateExtractValue(left, {field});
                    right = llvm::Constant::getNullValue(left->getType());
                }
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
            {
                // Normalize both operands to i1 so bitwise AND behaves as logical AND.
                // Without this, isdigit()-style functions (returning 4, 8, etc.) would
                // AND to 0 with the i1-widened left side (1 & 4 == 0).
                if (!left->getType()->isIntegerTy(1))
                    left = builder->CreateICmpNE(left, llvm::ConstantInt::get(left->getType(), 0), "tobool");
                if (!right->getType()->isIntegerTy(1))
                    right = builder->CreateICmpNE(right, llvm::ConstantInt::get(right->getType(), 0), "tobool");
                return builder->CreateAnd(left, right);
            }
            case Operation::BitwiseAnd:
            case Operation::AndAssignment:
            {
                return builder->CreateAnd(left, right);
            }
            case Operation::LogicalOr:
            {
                if (!left->getType()->isIntegerTy(1))
                    left = builder->CreateICmpNE(left, llvm::ConstantInt::get(left->getType(), 0), "tobool");
                if (!right->getType()->isIntegerTy(1))
                    right = builder->CreateICmpNE(right, llvm::ConstantInt::get(right->getType(), 0), "tobool");
                return builder->CreateOr(left, right);
            }
            case Operation::BitwiseOr:
            case Operation::OrAssignment:
            {
                return builder->CreateOr(left, right);
            }
            case Operation::BitwiseXor:
            case Operation::XorAssignment:
                return builder->CreateXor(left, right);
            case Operation::ShiftLeft:
            case Operation::LeftShiftAssignment:
                return builder->CreateShl(left, right);
            case Operation::ShiftRight:
            case Operation::RightShiftAssignment:
                return builder->CreateAShr(left, right);
            }
        }

        LogError(std::format("unhandled operation {}", static_cast<int>(op)));
        return right;
    }

    // Signedness-aware: chooses ZExt vs SExt, UDiv vs SDiv, ICMP_UGT vs ICMP_SGT, etc.
    llvm::Value* CreateOperation(Operation op, llvm::Value* left, llvm::Value* right,
                                  bool leftIsUnsigned, bool rightIsUnsigned)
    {
        if (left == nullptr)
            return right;

        // Delegate pointer operations before Upconvert can corrupt pointer/int pairs.
        if (left->getType()->isPointerTy() || right->getType()->isPointerTy())
            return CreateOperation(op, left, right);

        left  = Upconvert(left,  right, leftIsUnsigned);
        right = Upconvert(right, left,  rightIsUnsigned);

        bool anyUnsigned = leftIsUnsigned || rightIsUnsigned;

        if (left->getType()->isFloatingPointTy() || right->getType()->isFloatingPointTy())
            return CreateOperation(op, left, right);

        switch (op)
        {
        case Operation::DivideAssignment:
        case Operation::Divide:
            return anyUnsigned ? builder->CreateUDiv(left, right) : builder->CreateSDiv(left, right);
        case Operation::Modulo:
            return anyUnsigned ? builder->CreateURem(left, right) : builder->CreateSRem(left, right);
        case Operation::Greater:
            return builder->CreateICmp(anyUnsigned ? llvm::ICmpInst::ICMP_UGT : llvm::ICmpInst::ICMP_SGT, left, right);
        case Operation::GreaterEqual:
            return builder->CreateICmp(anyUnsigned ? llvm::ICmpInst::ICMP_UGE : llvm::ICmpInst::ICMP_SGE, left, right);
        case Operation::Less:
            return builder->CreateICmp(anyUnsigned ? llvm::ICmpInst::ICMP_ULT : llvm::ICmpInst::ICMP_SLT, left, right);
        case Operation::LessEqual:
            return builder->CreateICmp(anyUnsigned ? llvm::ICmpInst::ICMP_ULE : llvm::ICmpInst::ICMP_SLE, left, right);
        case Operation::ShiftRight:
        case Operation::RightShiftAssignment:
            return anyUnsigned ? builder->CreateLShr(left, right) : builder->CreateAShr(left, right);
        default:
            return CreateOperation(op, left, right);
        }
    }

    llvm::Value* CreateOperation(std::string oper, llvm::Value* left, llvm::Value* right,
                                  bool leftIsUnsigned, bool rightIsUnsigned)
    {
        if (left == nullptr)
            return right;
        Operation op = ParseOperation(oper);
        return CreateOperation(op, left, right, leftIsUnsigned, rightIsUnsigned);
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

    TypeAndValue GetFunctionReturnTypeInfo(const std::string& functionName) const
    {
        auto it = functionTable.find(functionName);
        if (it == functionTable.end() || it->second.empty())
            return {};
        return it->second.front().ReturnType;
    }

    std::string CreateAnonFunctionName()
    {
        return "__lambda_" + std::to_string(lambdaCounter++);
    }

    // Returns a TypeAndValue describing the function pointer type for the named function.
    TypeAndValue MakeFuncPtrTypeAndValue(const std::string& functionName) const
    {
        auto it = functionTable.find(functionName);
        if (it == functionTable.end() || it->second.empty())
            return {};

        // Prefer non-method overloads so that a plain function name isn't
        // shadowed by a struct method registered under the same key.
        const FunctionSymbol* chosen = &it->second.front();
        for (const auto& sym : it->second)
        {
            if (!sym.IsMethod) { chosen = &sym; break; }
        }

        TypeAndValue tv;
        tv.IsFunctionPointer = true;
        tv.FuncPtrReturnTypeName = chosen->ReturnType.TypeName;
        tv.FuncPtrReturnPointer = chosen->ReturnType.Pointer;
        for (const auto& p : chosen->Parameters)
        {
            TypeAndValue::FuncPtrParam fp;
            fp.TypeName = p.TypeName;
            fp.Pointer = p.Pointer;
            tv.FuncPtrParams.push_back(fp);
        }
        return tv;
    }

    // Emits an indirect call through a closure fat struct {i8* fnptr, i8* envptr}.
    llvm::Value* CreateIndirectCall(const TypeAndValue& funcPtrType, llvm::Value* funcPtr, std::vector<llvm::Value*> args)
    {
        auto* i8PtrTy = builder->getInt8Ty()->getPointerTo();

        // Extract fn ptr (field 0) and env ptr (field 1) from the closure fat struct.
        auto* fnPtrI8 = builder->CreateExtractValue(funcPtr, {0u}, "fn_i8");
        auto* envPtr  = builder->CreateExtractValue(funcPtr, {1u}, "env_ptr");

        // Build invoker function type: (i8* env, user_params...) -> RetType
        std::vector<llvm::Type*> paramTypes = {i8PtrTy};
        for (const auto& p : funcPtrType.FuncPtrParams)
        {
            TypeAndValue pTV; pTV.TypeName = p.TypeName; pTV.Pointer = p.Pointer;
            paramTypes.push_back(GetType(pTV));
        }
        TypeAndValue retTV;
        retTV.TypeName = funcPtrType.FuncPtrReturnTypeName;
        retTV.Pointer  = funcPtrType.FuncPtrReturnPointer;
        auto* retTy     = GetType(retTV);
        auto* invokerTy = llvm::FunctionType::get(retTy, paramTypes, false);
        auto* fnPtr     = builder->CreateBitCast(fnPtrI8, invokerTy->getPointerTo(), "fn_ptr");

        // Upconvert user args to match declared param types (index 0 is env, skip it).
        // String literals arrive as i8* — wrap them into %string{ptr,len} when the
        // param expects a string value type.
        for (size_t i = 0; i < args.size() && i + 1 < paramTypes.size(); i++)
        {
            auto* destTy = paramTypes[i + 1];
            auto* strTy  = llvm::StructType::getTypeByName(*context, "string");
            if (strTy && destTy == strTy && args[i]->getType()->isPointerTy())
                args[i] = WrapStringLiteralAsString(args[i]);
            else
                args[i] = Upconvert(args[i], destTy);
        }

        // Prepend env to call args
        std::vector<llvm::Value*> fullArgs = {envPtr};
        fullArgs.insert(fullArgs.end(), args.begin(), args.end());

        lastCallReturnType = retTV;
        auto* result = builder->CreateCall(invokerTy, fnPtr, fullArgs);
        return retTy->isVoidTy() ? nullptr : result;
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

    llvm::Function* GetOrDeclareStrcmp()
    {
        if (auto* fn = module->getFunction("strcmp"))
            return fn;
        auto* i32Ty = builder->getInt32Ty();
        auto* ptrTy = builder->getInt8Ty()->getPointerTo();
        auto* fnTy = llvm::FunctionType::get(i32Ty, { ptrTy, ptrTy }, false);
        return llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage, "strcmp", module.get());
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
    llvm::BranchInst* CreateBlockBreak(llvm::BasicBlock* resumeBlock, bool exitBlockStack)
    {
        if (exitBlockStack)
        {
            // Bare-semicolon form: expect_error("msg"); — if the expected error never fired before this scope exits, report failure.
            if (!expectedError.empty() && expectedErrorScopeDepth == stackNamedVariable.size())
            {
                std::cout << std::format("FAIL: expected error '{}' did not occur\n", expectedError);
                expectedError.clear();
                expectedErrorScopeDepth = SIZE_MAX;
                exit(1);
            }
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

    void CreateFunctionDeclaration(std::string functionName, LLVMBackend::TypeAndValue returnType, std::vector<LLVMBackend::TypeAndValue> arguments, bool external = false, bool varargs = false, bool returnsOwned = false, bool isMethod = false)
    {
        auto functionType = GetFunctionType(returnType, arguments, varargs, external);
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
                .ReturnsOwned = returnsOwned,
                .IsMethod = isMethod,
            };

            for (const auto& arg : arguments)
            {
                funcSym.Parameters.push_back(arg);
            }

            symList.push_back(funcSym);
        }
    }

    // Returns the C-compatible LLVM type: for IsFunctionPointer, bare fn ptr (not fat struct).
    // Used for extern function declarations to preserve C ABI compatibility.
    llvm::Type* GetCCompatibleType(const TypeAndValue& tv) const
    {
        if (tv.IsFunctionPointer)
        {
            std::vector<llvm::Type*> paramTypes;
            for (const auto& p : tv.FuncPtrParams)
            {
                TypeAndValue pTV; pTV.TypeName = p.TypeName; pTV.Pointer = p.Pointer;
                paramTypes.push_back(GetType(pTV));
            }
            TypeAndValue retTV;
            retTV.TypeName = tv.FuncPtrReturnTypeName;
            retTV.Pointer  = tv.FuncPtrReturnPointer;
            return llvm::FunctionType::get(GetType(retTV), paramTypes, false)->getPointerTo();
        }
        return GetType(tv);
    }

    llvm::FunctionType* GetFunctionType(LLVMBackend::TypeAndValue returnType, std::vector<LLVMBackend::TypeAndValue> arguments, bool varargs = false, bool externC = false)
    {
        std::vector<llvm::Type*> types;
        types.reserve(arguments.size());

        for (const LLVMBackend::TypeAndValue& arg : arguments)
        {
            types.emplace_back(externC ? GetCCompatibleType(arg) : GetType(arg));
        }

        auto* retTy = externC ? GetCCompatibleType(returnType) : GetType(returnType);
        return llvm::FunctionType::get(retTy, types, varargs);
    }

    std::string ComputeMangledName(std::string functionName, LLVMBackend::TypeAndValue returnType, std::vector<LLVMBackend::TypeAndValue> arguments, bool varargs = false)
    {
        std::string argumentString = {};

        for (const auto& argument : arguments)
        {
            argumentString += argument.ToUniqueString();
        }

        std::string uniqueName = std::format("_{}_{}_{}_", functionName, returnType.ToUniqueString(), argumentString);

        return uniqueName;
    }

    llvm::Function* CreateFunctionDefinition(std::string functionName, LLVMBackend::TypeAndValue returnType, std::vector<LLVMBackend::TypeAndValue> arguments, bool external = false, bool varargs = false, size_t line = 0, bool returnsOwned = false, bool isMethod = false)
    {
        llvm::FunctionType* functionType = GetFunctionType(returnType, arguments, varargs, external);

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
                if (verbose) std::cerr << "[verbose] skipping duplicate definition of '" << functionName << "'\n";
                return fn;
            }
            // Pre-declared by ForwardRefScanner — reuse the declaration and attach a body.
            alreadyDeclared = true;
        }
        else
        {
            fn = createFunctionProto(mangledName, functionType);
        }

        // CFlat treats null pointer dereferences as defined behavior (hardware fault → SEH).
        // Ensure the attribute is set even on pre-declared functions that skipped createFunctionProto.
        fn->addFnAttr(llvm::Attribute::NullPointerIsValid);

        createFunctionBlock(fn, functionName, arguments, returnsOwned);

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
                .ReturnsOwned = returnsOwned,
                .IsMethod = isMethod,
            };

            for (const auto& arg : arguments)
            {
                funcSym.Parameters.push_back(arg);
            }

            symList.push_back(funcSym);
        }
        else if (isMethod)
        {
            // ForwardRefScanner registered this symbol; propagate IsMethod in case it wasn't set there.
            auto it = functionTable.find(functionName);
            if (it != functionTable.end())
            {
                for (auto& sym : it->second)
                {
                    if (sym.Function == fn) { sym.IsMethod = true; break; }
                }
            }
        }

        return fn;
    }

    llvm::Type* GetType(const LLVMBackend::TypeAndValue& typeAndValue, llvm::Type* autoType = nullptr, bool allowPointer = true) const
    {
        if (typeAndValue.IsFunctionPointer)
        {
            return GetClosureFatPtrType();
        }

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
        // Resolve user-defined type aliases
        if (!resolvedTypeName.empty())
        {
            auto it = typeAliases.find(resolvedTypeName);
            if (it != typeAliases.end())
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
        else if (resolvedTypeName == "va_list") { type = llvm::PointerType::getUnqual(*context); }
        else if (resolvedTypeName == "auto" && autoType != nullptr) { type = autoType; }
        else
        {
            // Check if it is an interface type first
            if (interfaceTable.count(resolvedTypeName) > 0)
            {
                auto* fatTy = GetFatPtrType();
                if (typeAndValue.ElemPointer)
                    return fatTy->getPointerTo()->getPointerTo(); // {i8*,i8*}** (T* where T=IFace*)
                if (allowPointer && typeAndValue.IsInterfacePointer)
                    return fatTy->getPointerTo();                 // {i8*,i8*}* (T* where T=IFace, or T=IFace*)
                return fatTy;                                     // {i8*,i8*}  (bare fat ptr)
            }
            else
            {
                auto result = dataStructures.find(resolvedTypeName);
                if (result != dataStructures.end())
                {
                    type = result->second.StructType;
                }
                else
                {
                    LogError(std::format("unknown type '{}'", resolvedTypeName));
                    type = builder->getVoidTy();
                }
            }
        }

        if (typeAndValue.ConstArraySize > 0)
            return llvm::ArrayType::get(type, typeAndValue.ConstArraySize);

        if (allowPointer && typeAndValue.Pointer)
        {
            // Note: LLVM doesn't have void ptr, instead use i8 ptr.
            if (type->isVoidTy())
            {
                auto p = builder->getInt8Ty()->getPointerTo();
                return typeAndValue.ElemPointer ? p->getPointerTo() : p;
            }
            auto p = type->getPointerTo();
            return typeAndValue.ElemPointer ? p->getPointerTo() : p;
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

                // function<T> parameter: accept any function-compatible argument (named function,
                // lambda fat struct, or stored function<T> variable). Type fidelity is checked at codegen.
                if (candidateParamItr->IsFunctionPointer
                    && (arg.TypeAndValue.IsFunctionPointer
                        || arg.TypeAndValue.TypeName == "__closure_fat_ptr"
                        || (arg.BaseType && arg.BaseType->isStructTy()
                            && llvm::isa<llvm::StructType>(arg.BaseType)
                            && llvm::cast<llvm::StructType>(arg.BaseType)->getName() == "__closure_fat_ptr")
                        || (arg.Primary && llvm::isa<llvm::Function>(arg.Primary))
                        || (arg.BaseType && arg.BaseType->isPointerTy())))
                {
                    result = 0;
                }
                else if (arg.TypeAndValue.TypeName != "")
                {
                    // Resolve enum types for comparison: if either arg or param is an enum, use its backing type
                    auto resolveName = [&](const std::string& tn) -> std::string
                        {
                            if (tn.empty()) return tn;
                            auto it = enumBackingTypes.find(tn);
                            return (it != enumBackingTypes.end()) ? it->second : tn;
                        };

                    LLVMBackend::TypeAndValue tmpArg = arg.TypeAndValue;
                    LLVMBackend::TypeAndValue tmpParam = *candidateParamItr;

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

                        // Unsigned source into a signed param at equal or greater width is a safe
                        // implicit conversion (e.g. u8 -> int, u64 -> i64); Upconvert zero-extends.
                        if (result < 0 && myBits != -1 && otherBits != -1 && myUnsigned && !otherUnsigned && myBits <= otherBits)
                            result = (myBits == otherBits) ? 1 : 1;

                        if (result < 0)
                        {
                            int myFP = tmpArg.IsFloatingPoint();
                            int otherFP = tmpParam.IsFloatingPoint();
                            if (myFP != -1 && otherFP != -1)
                                result = (myFP == otherFP) ? 0 : 1;
                        }

                        // Any pointer type is implicitly convertible to void*.
                        if (result < 0 && arg.TypeAndValue.Pointer &&
                            candidateParamItr->Pointer && candidateParamItr->TypeName == "void")
                        {
                            result = 0;
                        }

                        // Interface upcast using original names (interfaces are not enums).
                        // Handles both struct->interface (value) and struct*->interface (pointer).
                        if (result < 0 && candidateParamItr->IsInterface &&
                            !arg.TypeAndValue.IsInterface &&
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

                    // Interface upcast: struct value (TypeName empty, BaseType is struct) to interface param
                    if (result < 0 && candidateParamItr->IsInterface && arg.BaseType)
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

    std::vector<LLVMBackend::NamedVariable> MatchFunction(const std::vector<LLVMBackend::NamedVariable>& inputArguments, const std::vector<LLVMBackend::TypeAndValue>& targetArguments)
    {
        // Two pass match.
        // 1) Align named arguments
        // 2) Fill remaining argument fills in the blank.

        const size_t inputSize = inputArguments.size();
        if (inputSize != targetArguments.size())
            return {};

        // A map from input to target Argument
        std::vector<int64_t> posMap(inputSize, -1);

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
        for (int64_t pos : posMap)
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
        std::vector<LLVMBackend::NamedVariable> result(inputSize);
        for (int i = 0; i < inputSize; i++)
        {
            result[posMap[i]] = inputArguments[i];
        }
        return result;
    }

    // Emit LLVM atomic IR for __atomic_* builtins called from atomic.cb.
    // Returns nullptr when name is not an atomic builtin (caller falls through to normal call).
    llvm::Value* TryEmitAtomicBuiltin(const std::string& name, const std::vector<llvm::Value*>& args)
    {
        using namespace llvm;
        auto& ctx = *context;

        // arg[0] is always the pointer to the _value field (i64* or i32*)
        if (name == "__atomic_counter_increment" || name == "__atomic_counter_decrement" ||
            name == "__atomic_counter_add")
        {
            // atomicrmw add/sub relaxed ptr, delta -> returns old value; we return old+delta
            Value* ptr   = args[0];
            Value* delta = (name == "__atomic_counter_decrement")
                ? ConstantInt::get(Type::getInt64Ty(ctx), -1)
                : (args.size() > 1 ? args[1] : ConstantInt::get(Type::getInt64Ty(ctx), 1));
            auto op = (name == "__atomic_counter_decrement")
                ? AtomicRMWInst::Add  // add(-1) == sub
                : AtomicRMWInst::Add;
            auto* old = builder->CreateAtomicRMW(op, ptr, delta,
                MaybeAlign(), AtomicOrdering::Monotonic);
            // return old + delta (new value)
            return builder->CreateAdd(old, delta, "atomic_new");
        }
        if (name == "__atomic_counter_read")
        {
            Value* ptr = args[0];
            auto* li = builder->CreateLoad(Type::getInt64Ty(ctx), ptr, "atomic_load");
            li->setAtomic(AtomicOrdering::SequentiallyConsistent);
            li->setAlignment(Align(8));
            return li;
        }
        if (name == "__atomic_flag_test_and_set")
        {
            Value* ptr = args[0];
            auto* one = ConstantInt::get(Type::getInt32Ty(ctx), 1);
            // xchg acquire: returns old value; 0 means we acquired the flag
            auto* old = builder->CreateAtomicRMW(AtomicRMWInst::Xchg, ptr, one,
                MaybeAlign(), AtomicOrdering::Acquire);
            // return true if old was 1 (flag was already set = contention)
            return builder->CreateICmpNE(old, ConstantInt::get(Type::getInt32Ty(ctx), 0), "was_set");
        }
        if (name == "__atomic_flag_clear")
        {
            Value* ptr = args[0];
            auto* zero = ConstantInt::get(Type::getInt32Ty(ctx), 0);
            auto* si = builder->CreateStore(zero, ptr);
            si->setAtomic(AtomicOrdering::Release);
            si->setAlignment(Align(4));
            return ConstantInt::get(Type::getInt32Ty(ctx), 0); // void: unused
        }
        if (name == "__atomic_i32_load" || name == "__atomic_i64_load")
        {
            bool is64 = (name == "__atomic_i64_load");
            Value* ptr = args[0];
            auto* ty = is64 ? Type::getInt64Ty(ctx) : Type::getInt32Ty(ctx);
            auto* li = builder->CreateLoad(ty, ptr, "atomic_load");
            li->setAtomic(AtomicOrdering::SequentiallyConsistent);
            li->setAlignment(is64 ? Align(8) : Align(4));
            return li;
        }
        if (name == "__atomic_i32_store" || name == "__atomic_i64_store")
        {
            bool is64 = (name == "__atomic_i64_store");
            Value* ptr = args[0];
            Value* val = args[1];
            auto* si = builder->CreateStore(val, ptr);
            si->setAtomic(AtomicOrdering::SequentiallyConsistent);
            si->setAlignment(is64 ? Align(8) : Align(4));
            return ConstantInt::get(Type::getInt32Ty(ctx), 0); // void: unused
        }
        if (name == "__atomic_i32_cas" || name == "__atomic_i64_cas")
        {
            bool is64 = (name == "__atomic_i64_cas");
            Value* ptr      = args[0];
            Value* expected = args[1];
            Value* desired  = args[2];
            auto* result = builder->CreateAtomicCmpXchg(ptr, expected, desired,
                MaybeAlign(),
                AtomicOrdering::AcquireRelease,
                AtomicOrdering::Monotonic);
            // extract the success bit (second element of {T, i1})
            return builder->CreateExtractValue(result, 1, "cas_ok");
        }
        return nullptr; // not an atomic builtin
    }

    llvm::Value* CreateOverloadedFunctionCall(std::string functionName, std::vector<LLVMBackend::NamedVariable> arguments)
    {
        functionName = ResolveQualifiedName(functionName);
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
            std::string msg = std::format("no overload of '{}' matches the given arguments.\n", functionName);

            // Call arguments
            msg += std::format("  Call arguments ({}):\n", arguments.size());
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
                msg += std::format("    [{}] {}{} {}\n", i, typeName, arg.TypeAndValue.Pointer ? "*" : "", name);
            }

            // Candidates
            msg += std::format("  Candidates ({}):\n", candidates.size());
            for (const auto& c : candidates)
            {
                std::string paramList;
                for (size_t i = 0; i < c.Parameters.size(); i++)
                {
                    if (i > 0) paramList += ", ";
                    const auto& p = c.Parameters[i];
                    paramList += std::format("{}{} {}", p.TypeName, p.Pointer ? "*" : "", p.VariableName);
                }
                msg += std::format("    {}({})\n", c.UniqueName, paramList);
            }

            // If exactly one resolved candidate passed MatchFunction, show per-argument type comparison
            if (resolvedCandidate.size() == 1)
            {
                const auto& [resolvedArgs, resolvedSym] = resolvedCandidate.front();
                msg += std::format("  Argument mismatch detail (single resolved candidate: {}):\n", resolvedSym.UniqueName);
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
                    msg += std::format("    [{}] arg={}{}  param={}{}\n", i, argDesc, argPtr ? "*" : "", paramDesc, paramPtr ? "*" : "");
                }
            }

            LogError(msg);
            return nullptr;
        }

        // convert parameter to vector of llvm::value*
        std::vector<llvm::Value*> argList;
        auto candParamItr = candidate.Parameters.begin();
        size_t argIndex = 0;
        for (const auto& arg : matched)
        {
            // Variadic arguments past the declared parameter list must not be dispatched
            // through pointer-parameter logic (candParamItr points at the last declared
            // param, which for printf is 'ptr %fmt' — causing all variadic args to be
            // pushed as storage/GEP addresses instead of loaded values).
            bool inVariadicRange = candidate.Variadic && argIndex >= candidate.Parameters.size();

            if (!inVariadicRange && candParamItr->IsInterface && !arg.TypeAndValue.IsInterface)
            {
                // Derive struct name from TypeName if available, else from BaseType
                std::string structName = arg.TypeAndValue.TypeName;
                if (structName.empty() && arg.BaseType)
                {
                    if (auto* st = llvm::dyn_cast<llvm::StructType>(arg.BaseType))
                        structName = st->getName().str();
                }

                // Build fat value: vtable + data ptr -> {i8*, i8*} by value
                auto vtable = GetOrCreateVTable(structName, candParamItr->TypeName);
                llvm::Value* dataPtr = nullptr;
                if (arg.TypeAndValue.Pointer)
                {
                    // struct* -> interface*: data ptr IS the pointer value (not the alloca of the pointer).
                    dataPtr = arg.Primary != nullptr ? arg.Primary : CreateLoad(arg.Storage);
                }
                else if (arg.Storage != nullptr)
                {
                    dataPtr = arg.Storage;
                }
                else
                {
                    // Materialize a pointer to the struct value
                    auto structTy = arg.BaseType ? arg.BaseType : GetType(arg.TypeAndValue);
                    auto tempAlloca = builder->CreateAlloca(structTy);
                    builder->CreateStore(arg.Primary, tempAlloca);
                    dataPtr = tempAlloca;
                }
                argList.push_back(BuildInterfaceFatValue(vtable, dataPtr));
            }
            else if (!inVariadicRange && candParamItr->IsInterface && arg.TypeAndValue.IsInterface)
            {
                // Interface -> interface: pass fat struct by value
                llvm::Value* val = arg.Primary ? arg.Primary : CreateLoad(arg.Storage);
                argList.push_back(val);
            }
            else if (!inVariadicRange && candParamItr->Pointer)
            {
                // For a non-pointer value passed to a pointer parameter (e.g. a field access
                // used as the 'this' receiver), prefer Storage (the GEP address) over Primary
                // (the pre-loaded value). Primary holds the struct value itself, which would
                // be the wrong type for a pointer parameter.
                // Guard: if Primary is already a pointer value (e.g. loaded from a global ptr),
                // use Primary directly — Storage would be the wrong level of indirection.
                if (!arg.TypeAndValue.Pointer && arg.Storage != nullptr
                    && !(arg.Primary != nullptr && arg.Primary->getType()->isPointerTy()))
                    argList.push_back(arg.Storage);
                else if (!arg.TypeAndValue.Pointer && arg.Storage == nullptr
                         && arg.Primary != nullptr && arg.Primary->getType()->isStructTy())
                {
                    // By-value struct parameter passed to a pointer parameter (e.g. args.count()
                    // where args is a list<T> value param). Materialize on the stack first.
                    auto* tempAlloca = builder->CreateAlloca(arg.Primary->getType());
                    builder->CreateStore(arg.Primary, tempAlloca);
                    argList.push_back(tempAlloca);
                }
                else
                {
                    // arg is a pointer type; Storage may be an alloca holding the pointer
                    // (promoted param). Load through it to get the actual pointer value.
                    if (arg.Primary == nullptr && arg.Storage != nullptr
                        && llvm::isa<llvm::AllocaInst>(arg.Storage))
                        argList.push_back(CreateLoad(arg.Storage));
                    else
                        argList.push_back(arg.GetValue());
                }
            }
            else if (!inVariadicRange && candParamItr->IsFunctionPointer)
            {
                // function<T> parameter — dispatch depends on whether the callee is extern C.
                llvm::Value* val = arg.Primary ? arg.Primary : CreateLoad(arg.Storage);
                // Inspect the actual LLVM param type to distinguish fat struct vs C fn ptr.
                auto* llvmParamTy = candidate.Function->getFunctionType()->getParamType((unsigned)argList.size());
                if (llvmParamTy->isStructTy())
                {
                    // Internal function<T>: provide a closure fat struct {i8*, i8*}.
                    if (val && !val->getType()->isStructTy())
                    {
                        // Re-resolve by CallerName to skip method overloads sharing the same key.
                        if (!arg.CallerName.empty())
                        {
                            if (auto* correctFn = GetFunctionForFuncPtr(arg.CallerName, -1))
                                val = correctFn;
                        }
                        if (auto* fn = llvm::dyn_cast<llvm::Function>(val))
                            val = WrapBareValueAsFatStruct(fn);
                    }
                }
                else
                {
                    // Extern C-compatible parameter: provide a bare C function pointer.
                    if (val && val->getType()->isStructTy())
                    {
                        // Fat struct — extract the fn ptr (field 0) and cast to expected type.
                        auto* fnI8 = builder->CreateExtractValue(val, {0u});
                        val = builder->CreateBitCast(fnI8, llvmParamTy, "fn_for_extern");
                    }
                    else if (val && !val->getType()->isPointerTy())
                    {
                        val = builder->CreateBitCast(val, llvmParamTy, "fn_for_extern");
                    }
                    else if (val)
                    {
                        val = builder->CreateBitCast(val, llvmParamTy, "fn_for_extern");
                    }
                }
                argList.push_back(val);
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

                if (!inVariadicRange)
                {
                    // Upconvert to match the declared parameter type (e.g. i16 -> i32).
                    bool argIsUnsigned = arg.TypeAndValue.IsUnsignedInteger() != -1;
                    value = Upconvert(value, GetType(*candParamItr), argIsUnsigned);
                }
                argList.push_back(value);
            }
            if (!inVariadicRange && candParamItr != candidate.Parameters.end() - 1)
                ++candParamItr;
            argIndex++;
        }

        // Intercept __atomic_* stubs: emit LLVM atomic IR directly.
        if (candidate.Function->getName().starts_with("__atomic_"))
        {
            auto* atomicResult = TryEmitAtomicBuiltin(candidate.Function->getName().str(), argList);
            if (atomicResult != nullptr)
            {
                lastCallReturnType = candidate.ReturnType;
                lastCallReturnsOwned = false;
                return atomicResult;
            }
        }

        // Check: bonded value must not be passed to a move parameter (would transfer ownership out of scope).
        for (size_t i = 0; i < candidate.Parameters.size() && i < matched.size(); i++)
        {
            if (candidate.Parameters[i].IsMove && matched[i].IsBonded)
                LogError(std::format("parameter '{}': cannot pass bonded value to 'move' parameter — bonded values cannot be transferred out of their source's scope", candidate.Parameters[i].VariableName));
        }

        auto* result = CreateFunctionCall(candidate.Function, argList);

        // Cache the resolved return type so callers can populate TypeAndValue after the call.
        lastCallReturnType = candidate.ReturnType;
        lastCallReturnsOwned = candidate.ReturnsOwned;

        // Populate bond side-channel: collect source variable names for bond parameters.
        lastCallIsBonded = false;
        lastCallBondedSources.clear();
        for (size_t i = 0; i < candidate.Parameters.size() && i < matched.size(); i++)
        {
            if (candidate.Parameters[i].IsBond && !matched[i].CallerName.empty())
            {
                lastCallIsBonded = true;
                lastCallBondedSources.push_back(matched[i].CallerName);
            }
        }

        // Null out caller's storage for move parameters; mark variable as moved for compile-time checking.
        for (size_t i = 0; i < candidate.Parameters.size() && i < matched.size(); i++)
        {
            if (candidate.Parameters[i].IsMove)
            {
                // Interface fat-ptr parameters (non-pointer) borrow the caller's data — don't zero it.
                // The fat-ptr holds a reference to the caller's struct; zeroing would corrupt that data.
                bool isInterfaceBorrow = candidate.Parameters[i].IsInterface && !candidate.Parameters[i].IsInterfacePointer;
                if (matched[i].Storage != nullptr && !isInterfaceBorrow)
                {
                    if (auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(matched[i].BaseType))
                    {
                        // Pointer move param: null the caller's storage.
                        builder->CreateStore(llvm::ConstantPointerNull::get(ptrTy), matched[i].Storage);
                    }
                    else if (candidate.Parameters[i].TypeName == "string" && matched[i].IsOwningString)
                    {
                        // String move param: zero out _ptr in the caller's alloca so its destructor is a no-op.
                        auto* strTy = llvm::StructType::getTypeByName(*context, "string");
                        if (strTy)
                        {
                            auto* ptrField = builder->CreateStructGEP(strTy, matched[i].Storage, 0);
                            auto* i8ptrTy = builder->getInt8Ty()->getPointerTo();
                            builder->CreateStore(llvm::ConstantPointerNull::get(i8ptrTy), ptrField);
                        }
                    }
                    else if (auto* stTy = llvm::dyn_cast<llvm::StructType>(matched[i].BaseType))
                    {
                        // Struct move param: zero the caller's entire struct so its destructor is a no-op.
                        builder->CreateStore(llvm::ConstantAggregateZero::get(stTy), matched[i].Storage);
                    }
                }
                // Compile-time: mark the caller's variable as moved so subsequent reads are rejected.
                // Only applies to pointer types — move is a no-op for value types (int, etc.).
                if (!matched[i].CallerName.empty() &&
                    matched[i].BaseType != nullptr &&
                    llvm::isa<llvm::PointerType>(matched[i].BaseType))
                    MarkVariableMoved(matched[i].CallerName);
            }
        }

        return result;
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

    // Like GetFunction, but prefers non-method (top-level) overloads.
    // Used when assigning a named function to a function<T> variable to avoid
    // picking a struct method that shares the same plain name.
    llvm::Function* GetFunctionForFuncPtr(std::string functionName, int expectedParamCount = -1)
    {
        functionName = ResolveQualifiedName(functionName);
        auto it = functionTable.find(functionName);
        if (it == functionTable.end() || it->second.empty())
            return module->getFunction(functionName);

        const auto& overloads = it->second;
        if (overloads.size() == 1)
            return overloads.front().Function;

        // Prefer non-method overloads with matching param count.
        for (const auto& sym : overloads)
        {
            if (sym.IsMethod) continue;
            if (expectedParamCount < 0 || (int)sym.Parameters.size() == expectedParamCount)
                return sym.Function;
        }
        // Fallback: any overload whose effective (non-self) param count matches.
        for (const auto& sym : overloads)
        {
            int effectiveCount = sym.IsMethod ? (int)sym.Parameters.size() - 1 : (int)sym.Parameters.size();
            if (expectedParamCount < 0 || effectiveCount == expectedParamCount)
                return sym.Function;
        }
        return overloads.front().Function;
    }

    NamedVariable GetLocalVariable(std::string name)
    {
        for (const auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
        {
            auto& nameVal = stackFrame.namedVariable;
            auto result = nameVal.find(name);

            if (result != nameVal.end())
            {
                auto nv = result->second;
                nv.CallerName = name;
                return nv;
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
                // The implicit `this` parameter is named "<StructName>__" (trailing
                // double-underscore). functionArgument is a std::map sorted by key,
                // so `begin()` is not guaranteed to be `this` — find it explicitly.
                auto thisIt = functionArguments.end();
                for (auto it = functionArguments.begin(); it != functionArguments.end(); ++it)
                {
                    const auto& key = it->first;
                    if (key.size() >= 2 && key.substr(key.size() - 2) == "__")
                    {
                        thisIt = it;
                        break;
                    }
                }
                if (thisIt == functionArguments.end())
                    return {};

                const auto& memberStructName = thisIt->first;
                // Storage is either an alloca-of-struct (constructor) or an alloca-of-ptr (method param).
                // For the latter, load through it to get the actual struct pointer before GEP.
                llvm::Value* memberStructInstance = thisIt->second.Storage;
                if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(memberStructInstance))
                {
                    if (alloca->getAllocatedType()->isPointerTy())
                        memberStructInstance = CreateLoad(alloca);
                }
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

            // Find the implicit 'this' arg — its name ends with "__" (the struct
            // name followed by "__"). functionArgument is a std::map (sorted
            // alphabetically), so we can't rely on begin() here.
            auto thisIt = functionArguments.end();
            for (auto it = functionArguments.begin(); it != functionArguments.end(); ++it)
            {
                const auto& argName = it->first;
                if (argName.size() >= 2 && argName.substr(argName.size() - 2) == "__")
                {
                    thisIt = it;
                    break;
                }
            }
            if (thisIt == functionArguments.end())
                break;

            const auto& thisArgName = thisIt->first;
            std::string structName = thisArgName.substr(0, thisArgName.size() - 2);

            auto funcIt = functionTable.find(functionName);
            if (funcIt == functionTable.end())
                break;

            for (const auto& sym : funcIt->second)
            {
                if (!sym.Parameters.empty() &&
                    sym.Parameters[0].TypeName == structName &&
                    sym.Parameters[0].Pointer)
                {
                    NamedVariable thisVar = thisIt->second;
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
                auto nv = result->second;
                nv.CallerName = name;
                return nv;
            }
        }

        return {};
    }

    // Returns the 0-based stack depth where name is declared, SIZE_MAX if not found.
    // Searches both functionArgument and namedVariable in each frame (innermost first).
    size_t FindVariableScopeDepth(const std::string& name) const
    {
        size_t depth = stackNamedVariable.size();
        for (const auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            --depth;
            if (frame.functionArgument.count(name) || frame.namedVariable.count(name))
                return depth;
        }
        return SIZE_MAX;
    }

    // Returns the name of a live bonded local that currently borrows from sourceName, or "" if none.
    std::string FindActiveBondBorrower(const std::string& sourceName) const
    {
        for (const auto& frame : stackNamedVariable)
        {
            for (const auto& [name, nv] : frame.namedVariable)
            {
                if (nv.IsBonded)
                {
                    for (const auto& src : nv.BondedSources)
                    {
                        if (src == sourceName)
                            return name;
                    }
                }
            }
        }
        return {};
    }

    // Clears the bond on a local variable (called when the bonded variable is reassigned).
    void ClearVariableBond(const std::string& name)
    {
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
            {
                it->second.IsBonded = false;
                it->second.BondedSources.clear();
                return;
            }
        }
    }

    void MarkVariableMoved(const std::string& name)
    {
        if (name.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                { it->second.IsMoved = true; return; }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                { it->second.IsMoved = true; return; }
        }
    }

    void MarkVariableUnmoved(const std::string& name)
    {
        if (name.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                { it->second.IsMoved = false; return; }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                { it->second.IsMoved = false; return; }
        }
    }

    // Snapshots the IsMoved flag for all variables in all active scopes.
    std::map<std::string, bool> SaveMovedState() const
    {
        std::map<std::string, bool> state;
        for (const auto& frame : stackNamedVariable)
        {
            for (const auto& [name, nv] : frame.functionArgument)
                state[name] = nv.IsMoved;
            for (const auto& [name, nv] : frame.namedVariable)
                state[name] = nv.IsMoved;
        }
        return state;
    }

    // Restores the IsMoved flag from a snapshot (only for variables still in scope).
    void RestoreMovedState(const std::map<std::string, bool>& state)
    {
        for (auto& frame : stackNamedVariable)
        {
            for (auto& [name, nv] : frame.functionArgument)
                if (auto it = state.find(name); it != state.end())
                    nv.IsMoved = it->second;
            for (auto& [name, nv] : frame.namedVariable)
                if (auto it = state.find(name); it != state.end())
                    nv.IsMoved = it->second;
        }
    }

    // Merges two post-branch states: a variable is moved if it was moved in either branch.
    void MergeMovedStates(const std::map<std::string, bool>& thenState,
                          const std::map<std::string, bool>& elseState)
    {
        for (auto& frame : stackNamedVariable)
        {
            for (auto& [name, nv] : frame.functionArgument)
            {
                auto t = thenState.find(name);
                auto e = elseState.find(name);
                if (t != thenState.end() && e != elseState.end())
                    nv.IsMoved = t->second || e->second;
            }
            for (auto& [name, nv] : frame.namedVariable)
            {
                auto t = thenState.find(name);
                auto e = elseState.find(name);
                if (t != thenState.end() && e != elseState.end())
                    nv.IsMoved = t->second || e->second;
            }
        }
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

    NamedVariable GetGlobalVariableNV(const std::string& name)
    {
        auto gIt = globalNamedVariable.find(name);
        if (gIt == globalNamedVariable.end())
            return {};

        NamedVariable nv;
        nv.Storage  = gIt->second;
        nv.BaseType = gIt->second->getValueType();

        auto tvIt = globalVariableTypes.find(name);
        if (tvIt != globalVariableTypes.end())
            nv.TypeAndValue = tvIt->second;

        return nv;
    }

    llvm::Constant* GetPlatformConstant()
    {
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), platformValue);
    }

    void SetCompileTimeMacro(const std::string& name, llvm::Constant* value, const std::string& type)
    {
        compileTimeMacros[name] = {name, value, type};
    }

    CompileTimeMacro GetCompileTimeMacro(const std::string& name)
    {
        auto it = compileTimeMacros.find(name);
        if (it != compileTimeMacros.end())
            return it->second;
        return {"", nullptr, ""};
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
            size_t varArgStart = func->arg_size();
            for (auto value : arg)
            {
                if (varArgStart > 0)
                {
                    auto destArgument = func->getArg(static_cast<unsigned int>(func->arg_size() - varArgStart));
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

    // Returns true if value is a load from an owning alloca in any live scope.
    bool IsOwningValue(llvm::Value* value) const
    {
        if (!value) return false;
        auto* loadInst = llvm::dyn_cast<llvm::LoadInst>(value);
        if (!loadInst) return false;
        auto* srcAlloca = loadInst->getPointerOperand();
        for (const auto& frame : stackNamedVariable)
        {
            for (const auto& [varName, nv] : frame.namedVariable)
                if (nv.Storage == srcAlloca && nv.IsOwning) return true;
            for (const auto& [varName, nv] : frame.functionArgument)
                if (nv.Storage == srcAlloca && nv.IsOwning) return true;
        }
        return false;
    }

    void CreateReturnCall(llvm::Value* value)
    {
        // check if break has already been inserted.
        if (builder->GetInsertBlock()->getTerminator() != nullptr)
            return;

        // If returning an owned variable loaded from a local alloca, suppress its cleanup
        // so we don't free the value before the caller receives it.
        // The loaded snapshot in `value` already captures the pointer; the caller takes ownership.
        NamedVariable* ownedStringReturnVar = nullptr;
        NamedVariable* ownedPtrReturnVar    = nullptr;
        if (value)
        {
            if (auto* loadInst = llvm::dyn_cast<llvm::LoadInst>(value))
            {
                auto* srcAlloca = loadInst->getPointerOperand();
                [&]() {
                    for (auto& frame : stackNamedVariable)
                    {
                        for (auto& [varName, nv] : frame.namedVariable)
                        {
                            if (nv.Storage == srcAlloca && nv.IsOwningString)
                            {
                                ownedStringReturnVar = &nv;
                                nv.IsOwningString = false;  // suppress destructor
                                return;
                            }
                            if (nv.Storage == srcAlloca && nv.IsOwning)
                            {
                                ownedPtrReturnVar = &nv;
                                nv.IsOwning = false;  // suppress cleanup on return path
                                return;
                            }
                        }
                        for (auto& [varName, nv] : frame.functionArgument)
                        {
                            if (nv.Storage == srcAlloca && nv.IsOwning)
                            {
                                ownedPtrReturnVar = &nv;
                                nv.IsOwning = false;  // suppress cleanup on return path
                                return;
                            }
                        }
                    }
                }();
            }
        }

        // Emit destructors for all scopes from innermost out to the function boundary
        for (auto it = stackNamedVariable.rbegin(); it != stackNamedVariable.rend(); ++it)
        {
            EmitDestructorsForScope(*it);
            if (it->isFunction) break;
        }

        // Restore flags (clean state, though the scope is about to be popped anyway)
        if (ownedStringReturnVar) ownedStringReturnVar->IsOwningString = true;
        if (ownedPtrReturnVar)    ownedPtrReturnVar->IsOwning = true;

        // If we are inlining a return-block, capture the value and branch to continuation.
        if (returnCapture)
        {
            if (value != nullptr && returnCapture->CaptureAlloca != nullptr)
                builder->CreateStore(value, returnCapture->CaptureAlloca);
            builder->CreateBr(returnCapture->ContinuationBlock);
            return;
        }

        if (autoVaListAlloca)
            CreateVaEnd(autoVaListAlloca);

        if (value == nullptr)
            builder->CreateRetVoid();
        else
        {
            auto* retTy = currentFunction->getReturnType();
            // Wrap raw i8* string literals into string struct when returning string
            auto* strTy = llvm::StructType::getTypeByName(*context, "string");
            if (strTy && retTy == strTy && value->getType() != strTy)
            {
                auto* ptrTy = builder->getInt8Ty()->getPointerTo();
                if (value->getType() == ptrTy)
                {
                    if (auto* c = llvm::dyn_cast<llvm::Constant>(value); c && IsStringLiteralConstant(c))
                        value = WrapStringLiteralAsString(value);
                    else if (GetFunction("operator string"))
                    {
                        NamedVariable argNV;
                        argNV.Primary = value;
                        argNV.BaseType = ptrTy;
                        argNV.TypeAndValue = { "char", "", true, false };
                        value = CreateOverloadedFunctionCall("operator string", { argNV });
                    }
                }
            }
            value = Upconvert(value, retTy);
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
    std::string GetSourceFilePath() const { return currentSourceFilePath_; }

    std::string GetCurrentFunctionName() const
    {
        for (const auto& frame : std::ranges::reverse_view(stackNamedVariable))
            if (frame.isFunction) return frame.functionName;
        return "";
    }

    void RegisterReturnBlock(const std::string& name, CFlatParser::CompoundStatementContext* body, std::vector<DeclTypeAndValue> params, TypeAndValue returnType)
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
    bool IsDataStructure(const std::string& name) const { return dataStructures.count(name) > 0; }
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

    // Resolves a qualified name (e.g. "MathAdv.MyNumber") to its canonical registered name
    // by expanding namespace aliases on the leading component and then walking up parent namespaces.
    std::string ResolveQualifiedName(const std::string& name) const
    {
        if (dataStructures.count(name) || interfaceTable.count(name) || functionTable.count(name))
            return name;

        auto dotPos = name.rfind('.');
        if (dotPos == std::string::npos)
            return name;

        std::string lastName = name.substr(dotPos + 1);
        std::string nsPrefix = name.substr(0, dotPos);

        // Resolve an alias on the first namespace component
        {
            auto firstDot = nsPrefix.find('.');
            std::string firstComp = firstDot == std::string::npos ? nsPrefix : nsPrefix.substr(0, firstDot);
            std::string restComp  = firstDot == std::string::npos ? std::string{} : nsPrefix.substr(firstDot + 1);
            std::string resolvedFirst = ResolveNamespace(firstComp);
            if (resolvedFirst != firstComp)
                nsPrefix = restComp.empty() ? resolvedFirst : resolvedFirst + "." + restComp;
        }

        // Walk up from the (possibly expanded) prefix toward the root
        std::string prefix = nsPrefix;
        while (true)
        {
            std::string candidate = prefix + "." + lastName;
            if (dataStructures.count(candidate) || interfaceTable.count(candidate) || functionTable.count(candidate))
                return candidate;
            auto parentDot = prefix.rfind('.');
            if (parentDot == std::string::npos)
                break;
            prefix = prefix.substr(0, parentDot);
        }

        return name;
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

    // If set, disables auto-import of core/runtime.cb
    void SetSkipRuntimeImport(bool v) { skipRuntimeImport = v; }
    void SetRuntimeDir(const std::string& dir) { runtimeDir = dir; }
    void SetSourceFileDir(const std::string& dir) { sourceFileDir_ = dir; }
    void SetVerbose(bool v) { verbose = v; }
    bool IsVerbose() const { return verbose; }

    using DiagnosticSink = std::function<void(const std::string& file, size_t line, size_t col, const std::string& msg)>;
    void SetDiagnosticSink(DiagnosticSink sink) { diagnosticSink_ = std::move(sink); }

    void SetSymbolSink(LspSymbolIndex* sink) { symbolSink_ = sink; }
    LspSymbolIndex* GetSymbolSink() const { return symbolSink_; }

    void ReportParseErrors(const std::vector<ParseDiagnostic>& diagnostics,
                           const std::vector<std::string>& sourceLines);

    bool Compile(const ArgParser& args);
    bool CompileImportedFile(const std::string& importingFilePath, const std::string& importFilename);
    bool Analyze(const std::string& filePath, const std::string& importDir, const std::string& runtimeDirPath);
    void ResetForReanalysis();
};

// Defined here so MyCompilerLLVM is fully declared before DumpState() is called.
inline void CompilerManager::DumpAllState() const
{
    if (compilers_.empty())
    {
        std::cerr << "  (no compiler instances registered)\n";
        return;
    }
    for (size_t i = 0; i < compilers_.size(); ++i)
    {
        std::cerr << "  [Compiler " << i << "]\n";
        compilers_[i]->DumpState();
    }
}
