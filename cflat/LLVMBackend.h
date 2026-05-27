#pragma once
// ============================================================
// LLVMBackend.h - LLVM IR backend, type system, symbol tables
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
#include <filesystem>
#include <mutex>
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
#include <llvm\Support\JSON.h>
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
#include "VcpkgResolver.h"

struct ExpectedErrorReceived {};

// Per-backend generic template state. Holds the ANTLR contexts and metadata for
// all generic struct/class/interface/function templates seen during one analysis,
// plus the queue of pending instantiations. Lives on LLVMBackend so multiple
// backends can run in parallel without stomping on each other.
struct GenericTemplateState
{
    struct PendingInstantiation
    {
        std::string templateName;
        std::vector<std::string> typeArgs;
        std::string mangledName;
    };

    std::unordered_map<std::string, CFlatParser::StructDefinitionContext*>      genericStructTemplates;
    std::unordered_map<std::string, CFlatParser::ClassDefinitionContext*>       genericClassTemplates;
    std::unordered_map<std::string, std::vector<std::string>>                   genericStructTypeParams;
    std::unordered_set<std::string>                                             instantiatedGenerics;
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>> genericStructConstraints;
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>> genericClassConstraints;
    std::unordered_map<std::string, size_t>                                     genericStructPackIndex;
    std::unordered_map<std::string, size_t>                                     genericClassPackIndex;
    std::unordered_map<std::string, size_t>                                     genericFunctionPackIndex;
    std::unordered_map<std::string, size_t>                                     genericInterfacePackIndex;
    std::unordered_map<std::string, CFlatParser::InterfaceDefinitionContext*>   genericInterfaceTemplates;
    std::unordered_map<std::string, std::vector<std::string>>                   genericInterfaceTypeParams;
    std::unordered_set<std::string>                                             instantiatedInterfaces;
    std::unordered_map<std::string, CFlatParser::FunctionDefinitionContext*>    genericFunctionTemplates;
    std::unordered_map<std::string, std::vector<std::string>>                   genericFunctionTypeParams;
    std::unordered_set<std::string>                                             instantiatedGenericFunctions;
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>> genericFunctionConstraints;
    std::vector<PendingInstantiation>                                           pendingInstantiations;

    void Clear()
    {
        genericStructTemplates.clear();
        genericClassTemplates.clear();
        genericStructTypeParams.clear();
        instantiatedGenerics.clear();
        genericStructConstraints.clear();
        genericClassConstraints.clear();
        genericInterfaceTemplates.clear();
        genericInterfaceTypeParams.clear();
        instantiatedInterfaces.clear();
        genericFunctionTemplates.clear();
        genericFunctionTypeParams.clear();
        genericFunctionConstraints.clear();
        instantiatedGenericFunctions.clear();
        pendingInstantiations.clear();
    }
};

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
        bool IsMove = false;     // parameter declared with 'move' - function takes ownership
        bool IsBond = false;     // parameter declared with 'bond' - return value borrows from this parameter; return must not outlive it
        bool IsStdcall = false;  // function declared with 'stdcall' - uses __stdcall calling convention on Win32
        bool IsCdecl = false;   // function declared with 'cdecl'   - explicit C calling convention (default; recognized for clarity)

        // Function pointer fields (IsFunctionPointer == true)
        bool IsFunctionPointer = false;
        std::string FuncPtrReturnTypeName;
        bool FuncPtrReturnPointer = false;
        struct FuncPtrParam { std::string TypeName; bool Pointer = false; bool IsMove = false; };
        std::vector<FuncPtrParam> FuncPtrParams;

        uint64_t ConstArraySize = 0;                   // outer (first) dimension; non-zero for fixed arrays
        std::vector<uint64_t> ConstInnerDimensions;   // inner dimensions for multi-dim arrays (e.g. [M] in T[N][M])

        // Lock-set analysis: non-empty when this variable's field is inside a lock(...) { } group.
        std::string GuardedBy;
        // The VariableName of the struct that contains this field (e.g. "d" when this field was accessed as d->field).
        // Used to reconstruct the qualified lock name (e.g. "d.ready") for lock(this) parameter seeding.
        std::string ParentVariableName;
        // True when this function parameter is declared lock(this): the lambda/callback passed here
        // executes with the call-site receiver seeded into currentLockSet for GuardedBy checks.
        bool LockThis = false;

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
                    s += "_" + p.TypeName + (p.Pointer ? "Ptr" : "") + (p.IsMove ? "M" : "");
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

        // Used for array - first (outer) dimension; extra inner dimensions in ExtraArrayDims
        CFlatParser::AssignmentExpressionContext* ArraySize = nullptr;
        std::vector<CFlatParser::AssignmentExpressionContext*> ExtraArrayDims;

        // Used for default parameter values
        CFlatParser::InitializerContext* DefaultValue = nullptr;

        bool external = false;
        bool threadLocal = false;

        // User-requested alignment from `alignas(N)`. 0 means unset; honored only
        // when greater than the type's ABI alignment. Power of two, validated at
        // parse time.
        uint64_t UserAlignValue = 0;

        std::vector<AnnotationValue> Annotations;
    };

    struct NamedVariable
    {
    public:
        LLVMBackend::TypeAndValue TypeAndValue;
        llvm::Type* BaseType = nullptr;  // The type of the value, even if it is a pointer.
        llvm::Value* Primary = nullptr;  // The value or result
        llvm::Value* Storage = nullptr;  // The container holding the value, used to load or store.
        llvm::Type* UnionFieldType = nullptr;  // When non-null: load/store this storage as this type (union field access).
        bool IsOwning = false;           // true for move parameters, new-allocated locals, and any owned pointer - freed on scope exit
        bool IsNewAllocated = false;     // true only for 'new'-allocated locals - enables refcount on field escape (cleared on null-source transfer)
        bool IsOwningString = false;     // true when a string local owns its heap buffer - destructor called on scope exit
        bool IsOwningStruct = false;     // true for move parameters of struct types with destructors - destructor called on scope exit
        bool IsMoved = false;            // compile-time: true after this variable's ownership was transferred via a move call
        bool IsBonded = false;           // compile-time: true when this variable holds a bonded (borrowed) return value
        std::vector<std::string> BondedSources; // names of bond parameters this value borrows from
        bool IsBorrowed = false;         // compile-time: true for non-move pointer parameters and locals that alias one - 'delete' is forbidden
        std::string BorrowedOrigin;      // name of the borrowed parameter this value transitively aliases (for diagnostics)
        llvm::Value* RefCountStorage = nullptr; // lazy i32 alloca at function entry; non-null only when pointer escaped to a field
        std::string CallerName;          // the variable's name at the call site, for move tracking
        std::string OwningStructName;    // when this NamedVariable is a struct-field access, the field's owning struct
        std::string FieldName;           // when this NamedVariable is a struct-field access, the field name
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
        bool IsUnion = false;
        // User-requested alignment from `alignas(N)` on the struct definition.
        // 0 means unset. When set, the struct's effective alignment is max of
        // this and the ABI alignment LLVM derived from the field types.
        uint64_t UserRequestedAlignment = 0;
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
        bool IsImportedProgram      = false;            // true when created via 'import program "file.cb" as Name'
        std::vector<std::string> Interfaces;            // interfaces declared with ': IFoo, IBar'
        std::unordered_map<std::string, llvm::GlobalVariable*> VTables; // cached vtables keyed by interface name
        llvm::GlobalVariable* typeDescriptor = nullptr; // unique type-identity global for 'is'/'as' checks
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

    // ABI lowering recipe for one parameter or return value when calling a C extern with
    // struct-by-value in its signature. Each slot tells the call-site emitter how to bridge
    // a CFlat struct value to the platform ABI:
    //   - Direct       : no lowering (scalar / pointer); pass the value as-is
    //   - CoerceToInt  : load the struct as an iN integer (size in {1,2,4,8} bytes) and pass
    //                    that. For the return slot, the iN result is stored back to a temp
    //                    alloca and reinterpreted as the struct.
    //   - ByVal        : pass a pointer to a caller-allocated copy of the struct. The LLVM
    //                    'byval' attribute carries the pointee struct type + alignment.
    //   - SRetReturn   : (return only) the function returns void; the caller passes a hidden
    //                    pointer as arg 0 with the 'sret' attribute, callee writes through it.
    struct AbiSlot
    {
        enum Kind { Direct, CoerceToInt, ByVal, SRetReturn };
        Kind kind = Direct;
        llvm::Type* coerceTy = nullptr;     // iN type for CoerceToInt
        llvm::StructType* structTy = nullptr; // pointee for ByVal / SRetReturn (and source of byval/sret type attrs)
        uint64_t align = 0;                  // byval/sret alignment hint
    };
    struct AbiRecipe
    {
        bool hasLowering = false;            // true if at least one slot is non-Direct
        AbiSlot retSlot;
        std::vector<AbiSlot> paramSlots;
    };

    class FunctionSymbol
    {
    public:
        std::string UniqueName;
        llvm::Function* Function;
        TypeAndValue ReturnType;
        std::vector<TypeAndValue> Parameters;
        bool Variadic = false;
        bool ReturnsOwned = false; // true when the function returns an owned value (heap string or owned pointer) - caller must free
        bool IsMethod = false;     // true when registered as a struct/class method (has implicit self pointer)
        std::vector<std::string> RequiredLocks; // canonical lock-set that the caller must hold (from lock clause)
        AbiRecipe Recipe;          // populated for extern (cdecl) functions whose signature contains struct-by-value
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

    // Capture mode for 'auto' return-type inference. While active, CreateReturnCall
    // does NOT emit a 'ret' instruction; instead it terminates the current BB with
    // an UnreachableInst placeholder and records (BB, value) onto AutoReturnSites.
    // After body emission the caller unifies the value types, builds a new function
    // with the inferred signature, splices the BBs over, replaces each placeholder
    // terminator with the real ret, and RAUWs the old function.
    struct AutoReturnSite
    {
        llvm::BasicBlock* Block;
        llvm::Value* Value;      // nullptr for bare 'return;'
        llvm::Instruction* Placeholder; // unreachable inst we inserted; will be replaced by ret
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
    std::vector<std::string> lastCallRequiredLocks;  // RequiredLocks of the last resolved overload (for call-site lock checking)
    std::vector<std::string> lastCallParameterNames; // VariableName of each parameter of the last resolved overload

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

    void LogWarning(std::string message) const
    {
        std::cout << std::format("{}({},{}): warning: {}\n", sourceFileName, currentLine, currentColumn, message);
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
    // Typedef name -> qualType string from C/header AST dumps. Populated by
    // CollectCTypedefs as a pre-pass before signature/struct walks so the
    // function-signature mapper can chase HANDLE -> void*, SOCKET -> uintptr_t,
    // etc. Process-wide; first-writer-wins.
    std::unordered_map<std::string, std::string> cTypedefMap_;
    std::unordered_map<std::string, std::vector<FunctionSymbol>> functionTable;
    std::unordered_map<std::string, std::vector<InterfaceMethod>> interfaceTable;
    std::unordered_map<std::string, std::vector<std::string>> interfaceParents;
    std::unordered_map<std::string, llvm::Constant*> stringPool;
    std::unordered_set<std::string> namespaceTable;
    // Maps import alias name → set of unqualified symbol names the file contributed.
    // Populated by CompileImportedFile when namespaceName is non-empty.
    std::unordered_map<std::string, std::unordered_set<std::string>> importAliasMembers;
    std::unordered_set<std::string> importedFiles;
    std::vector<std::string> importStack;  // DFS stack for circular import detection
    std::string importSearchDir;
    std::string runtimeDir;
    std::string sourceFileDir_;  // original source dir for LSP temp-file analysis

    // Per-backend generic-template state, shared with MainListener via references.
    GenericTemplateState gts;
private:
    // When true, disables auto-import of core/runtime.cb
    bool skipRuntimeImport = false;
    bool verbose = false;
    int platformValue = 64;  // 64 for win64, 32 for win32
    // C interop: temp .obj files produced by clang-cl from .c inputs, linked
    // into the final image by EmitExecutable and then deleted.
    std::vector<std::string> cObjectFiles_;
    int cOptLevel_ = 0;        // optimization level applied to clang C compiles
    bool cDebugInfo_ = false;  // emit CodeView for clang C compiles
    // C interop (prebuilt libraries): header search dirs (--c-include) used when
    // AST-dumping a bound header, and import libraries (--c-lib) added to the
    // lld-link line by EmitExecutable. A sibling runtime DLL of each lib is copied
    // next to the output exe after a successful link.
    std::vector<std::string> cIncludeDirs_;
    std::vector<std::string> cLinkLibs_;
    // Preprocessor defines (--c-define) passed as /D to every clang-cl invocation:
    // the .c object compile, the .c auto-extern AST dump, and the header AST dump.
    std::vector<std::string> cDefines_;
    // Runtime DLLs sourced from `import package-vcpkg`. Populated by the vcpkg resolver
    // with the authoritative list from vcpkg_installed/<triplet>/bin and copied next to
    // the exe by CopyCRuntimeDlls. Kept separate from cLinkLibs_-based DLL probing.
    std::vector<std::string> vcpkgRuntimeDlls_;
    // Vcpkg integration state. Owns manifest discovery, port validation, and the one-shot
    // `vcpkg install` invocation. See VcpkgResolver.h.
    VcpkgResolver vcpkg_;

    // Cache of C function signatures extracted from a .c via clang's AST dump, so the
    // (slow) clang spawn is skipped when the file is unchanged - important for LSP,
    // which re-analyzes the importing .cb on every debounced edit. Process-global and
    // mutex-guarded: the LSP runs analyses concurrently on a pool of backends, and a
    // document is not pinned to a slot, so a per-instance cache would miss across slots.
    // Entries hold only plain data (no LLVM/context-bound objects), so sharing is safe.
    struct CSigEntry
    {
        std::string name;
        TypeAndValue ret;
        std::vector<TypeAndValue> params;
        bool variadic = false;
        int line = 1;
        int col = 0;
    };
    // A C enum constant extracted from a bound header (registered as a bare global int).
    struct CEnumEntry
    {
        std::string name;
        long long value = 0;
        int line = 1;
        int col = 0;
    };
    // An object-like C macro extracted from a bound header. Function-like macros are skipped
    // (cflat has no preprocessor to expand them). The value is resolved by feeding the macro
    // through an enum stub - the original source location (file/line/col) is preserved from
    // the -dD discovery pass so the LSP can go-to-definition into the real header.
    struct CMacroEntry
    {
        std::string name;
        long long value = 0;
        std::string file;
        int line = 1;
        int col = 0;
        // True when the macro's natural type (recovered by Pass B's __typeof__ probe)
        // is a void* (directly or via a typedef chain like HANDLE -> void *). Such
        // macros are sentinel pointers (INVALID_HANDLE_VALUE, MAP_FAILED, ...) and
        // RegisterCMacros emits them as void* globals so they can be compared against
        // pointer-returning C APIs.
        bool isPointer = false;
    };
    // A C struct/union decl extracted from a bound header or source. Field types are kept as
    // raw C type spellings (qualType) so they can be re-resolved against dataStructures after
    // all records in the same TU are registered (handles forward references between structs).
    struct CRecordFieldEntry
    {
        std::string name;
        std::string ctype;
    };
    struct CRecordEntry
    {
        std::string name;                       // tag name (e.g. "Point")
        bool isUnion = false;                   // tagUsed == "union"
        std::vector<CRecordFieldEntry> fields;
        int line = 1;
        int col = 0;
    };
    // A function-like C macro extracted from a bound header. Translated to a CFlat 'auto'
    // generic function so that callers can use macro-defined helpers like MIN/MAX/KB without
    // hand-writing them. Body is the raw replacement text; the translation pass tokenizes,
    // filters against an allowlist of safe expression operators, and rejects anything else.
    struct CFunctionMacroEntry
    {
        std::string name;
        std::vector<std::string> params;
        std::string body;
        std::string file;
        int line = 1;
        int col = 0;
    };
    struct CFileSigCacheEntry
    {
        std::filesystem::file_time_type mtime{};
        uint64_t hash = 0;
        std::vector<CSigEntry> sigs;
        std::vector<CEnumEntry> enums;
        std::vector<CRecordEntry> records;
        std::vector<CMacroEntry> macros;
        std::vector<CFunctionMacroEntry> funcMacros;
    };
    static inline std::mutex cFileSigCacheMutex_;
    // Key: canonical .c path, or for bound headers "<canonical .h>|<include dirs>" so the
    // same header under different --c-include roots does not collide.
    static inline std::unordered_map<std::string, CFileSigCacheEntry> cFileSigCache_;
    // Serializes clang-cl subprocess spawns. The LSP analyzes on a pool of worker
    // threads; concurrent llvm::sys::ExecuteAndWait calls (CreateProcess with
    // handle-inheriting redirects) race and crash the process on Windows. Spawns are
    // rare (cache absorbs the rest), so a process-wide lock costs effectively nothing.
    static inline std::mutex cClangSpawnMutex_;
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
    // Synthesized CFlat source (from C function-like macro translation) lives here.
    // The parser ecosystem must outlive instantiation since genericFunctionTemplates
    // stores raw FunctionDefinitionContext pointers into these trees.
    struct SyntheticParseState
    {
        std::string label;                                          // for diagnostics
        std::unique_ptr<antlr4::ANTLRInputStream> input;
        std::unique_ptr<CFlatLexer> lexer;
        std::unique_ptr<antlr4::CommonTokenStream> tokens;
        std::unique_ptr<CFlatParser> parser;
    };
    std::vector<SyntheticParseState> syntheticParseStates_;
    // Queued CFlat source generated from C function-like macros, drained after each
    // C-header import by ProcessPendingMacroSources (defined in LLVMBackend.cpp).
    struct PendingMacroSource { std::string label; std::string source; };
    std::vector<PendingMacroSource> pendingMacroSources_;
    void ProcessPendingMacroSources();
    std::unordered_map<std::string, std::string> namespaceAliasTable;
    std::unordered_map<std::string, ReturnBlockEntry> returnBlockTable;
    std::optional<ReturnCaptureContext> returnCapture;
    std::optional<std::vector<AutoReturnSite>> autoReturnCapture; // active when emitting an 'auto' generic instantiation
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
    // Cache: base type name -> DIType (no pointer/array wrapper). Pointer / array /
    // function-pointer / interface fat-ptr wrappers are built on demand so the cache
    // key stays simple.
    std::unordered_map<std::string, llvm::DIType*> diTypeCache;
    // Cache: canonical source path -> DIFile. Without this, all functions / globals
    // emitted from imported files would be attributed to the primary diFile, causing
    // line-number collisions across files (breakpoint at one file's line N fires on
    // any imported function whose actual line happens to be N).
    std::unordered_map<std::string, llvm::DIFile*> diFileCache_;

    // Globals queued for deferred DI emission. We defer because a global's struct type
    // may still be an opaque shell (typical for generic instantiations) at the moment
    // the global is declared - emitting DI immediately would produce an unspecified
    // type with no fields. By the time FinalizeDebugInfo runs, all struct layouts are
    // finished and GetDIType returns the real composite.
    struct PendingGlobalDI
    {
        llvm::GlobalVariable* gVar;
        TypeAndValue typeValue;
        llvm::DIFile* file;
        unsigned line;
    };
    std::vector<PendingGlobalDI> pendingGlobalDI_;

    llvm::DIFile* GetDIFileForCurrentSource()
    {
        if (!diBuilder) return nullptr;
        const std::string& p = currentSourceFilePath_;
        if (p.empty()) return diFile;
        auto it = diFileCache_.find(p);
        if (it != diFileCache_.end()) return it->second;
        // Split path into directory + filename manually to avoid needing <filesystem>
        // in this header.
        size_t slash = p.find_last_of("/\\");
        std::string fname = (slash == std::string::npos) ? p : p.substr(slash + 1);
        std::string dir   = (slash == std::string::npos) ? std::string() : p.substr(0, slash);
        auto* f = diBuilder->createFile(fname, dir);
        diFileCache_[p] = f;
        return f;
    }


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

    // Returns the Storage alloca and BaseType for a named variable in any active
    // scope (innermost first). Searches both functionArgument (for move params)
    // and namedVariable. Used by the transfer-ownership path to find the source
    // alloca when an expression (e.g. a cast) has cleared rightNV.Storage but
    // CallerName still names the original variable.
    struct VarStorageRef { llvm::Value* Storage = nullptr; llvm::Type* BaseType = nullptr; };
    VarStorageRef FindVariableStorage(const std::string& name) const
    {
        if (name.empty()) return {};
        for (const auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                return { it->second.Storage, it->second.BaseType };
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                return { it->second.Storage, it->second.BaseType };
        }
        return {};
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

        // Call operator delete (allocator-aware) to free the pointer.
        auto* opDel = GetFunction("operator delete");
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

        // Cleanup calls (destructors, operator delete, lock release) emitted here
        // belong to "the end of the scope" rather than any user statement. If we
        // have a subprogram but the builder lost its debug location (e.g. just
        // after a branch / return / lambda body), the verifier rejects untagged
        // inlinable calls in -g builds. Pin a synthetic location to the function
        // line so every cleanup instruction is properly attributed.
        if (currentSubprogram && !builder->getCurrentDebugLocation())
        {
            builder->SetCurrentDebugLocation(llvm::DILocation::get(
                *context, currentSubprogram->getLine(), 0, currentSubprogram));
        }

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

            // Clean up move string parameters (move string param - non-pointer ownership)
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
        // Drop any debug location lingering from the caller's emission context
        // before emitting parameter allocas/stores: otherwise those instructions
        // get tagged with the outer function's DISubprogram, which trips the
        // LLVM verifier (`!dbg attachment points at wrong subprogram`).
        // CreateFunctionDefinition will re-set the location once it attaches the
        // new function's subprogram below.
        builder->SetCurrentDebugLocation(llvm::DebugLoc());
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
                // Non-move pointer parameters borrow from the caller. Track this so we can
                // reject 'delete' on the param or any local that aliases it via assignment/cast.
                if (itr_nameArg->Pointer && !itr_nameArg->IsMove)
                {
                    namedVar.IsBorrowed = true;
                    namedVar.BorrowedOrigin = itr_nameArg->VariableName;
                }
                stackState.functionArgument[itr_nameArg->VariableName] = namedVar;
            }
            if (symbolSink_ && !itr_nameArg->VariableName.empty())
                symbolSink_->RegisterVariable(itr_nameArg->VariableName, itr_nameArg->TypeName);

            itr_nameArg++;
        }

        currentFunction = fn;
        autoVaListAlloca = nullptr;
    }

    llvm::DIType* GetDIType(const TypeAndValue& tv)
    {
        using namespace llvm::dwarf;
        const llvm::DataLayout& DL = module->getDataLayout();
        unsigned ptrBits = DL.getPointerSizeInBits();

        // Fixed-size array: T[N] - wrap element DI in an array DIType.
        if (tv.ConstArraySize > 0)
        {
            TypeAndValue elem = tv;
            elem.ConstArraySize = 0;
            elem.ConstInnerDimensions.clear();
            auto* elemDI = GetDIType(elem);
            auto* elemTy = GetType(elem, nullptr, elem.Pointer);
            uint64_t total = DL.getTypeAllocSizeInBits(elemTy) * tv.ConstArraySize;
            llvm::SmallVector<llvm::Metadata*, 1> subs;
            subs.push_back(diBuilder->getOrCreateSubrange(0, (int64_t)tv.ConstArraySize));
            return diBuilder->createArrayType(total, 0, elemDI,
                diBuilder->getOrCreateArray(subs));
        }

        // Function pointer: build subroutine + pointer wrapper.
        if (tv.IsFunctionPointer)
        {
            std::vector<llvm::Metadata*> types;
            TypeAndValue retTV;
            retTV.TypeName = tv.FuncPtrReturnTypeName;
            retTV.Pointer = tv.FuncPtrReturnPointer;
            types.push_back((retTV.TypeName == "void" && !retTV.Pointer)
                ? nullptr : (llvm::Metadata*)GetDIType(retTV));
            for (const auto& p : tv.FuncPtrParams)
            {
                TypeAndValue pTV; pTV.TypeName = p.TypeName; pTV.Pointer = p.Pointer;
                types.push_back(GetDIType(pTV));
            }
            auto srt = diBuilder->createSubroutineType(
                diBuilder->getOrCreateTypeArray(types));
            return diBuilder->createPointerType(srt, ptrBits);
        }

        // Pointer wrapper: strip and recurse on base.
        if (tv.Pointer)
        {
            TypeAndValue base = tv;
            base.Pointer = false;
            auto* baseDI = GetDIType(base);
            return diBuilder->createPointerType(baseDI, ptrBits);
        }

        // Interface fat pointer {i8* vtable, i8* data} - cached once.
        if (tv.IsInterface && !tv.IsInterfacePointer)
        {
            auto it = diTypeCache.find("__interface_fatptr");
            if (it != diTypeCache.end()) return it->second;
            auto i8DI = diBuilder->createBasicType("i8", 8, DW_ATE_unsigned_char);
            auto i8Ptr = diBuilder->createPointerType(i8DI, ptrBits);
            std::vector<llvm::Metadata*> members = {
                diBuilder->createMemberType(compileUnit, "vtable", diFile, 0,
                    ptrBits, ptrBits, 0, llvm::DINode::FlagZero, i8Ptr),
                diBuilder->createMemberType(compileUnit, "data", diFile, 0,
                    ptrBits, ptrBits, ptrBits, llvm::DINode::FlagZero, i8Ptr),
            };
            auto fatTy = diBuilder->createStructType(compileUnit, "__interface",
                diFile, 0, ptrBits * 2, ptrBits, llvm::DINode::FlagZero, nullptr,
                diBuilder->getOrCreateArray(members));
            diTypeCache["__interface_fatptr"] = fatTy;
            return fatTy;
        }

        // Cache lookup for basic + struct types.
        auto cacheIt = diTypeCache.find(tv.TypeName);
        if (cacheIt != diTypeCache.end()) return cacheIt->second;

        llvm::DIType* basic = nullptr;
        if      (tv.TypeName == "int")    basic = diBuilder->createBasicType("int", 32, DW_ATE_signed);
        else if (tv.TypeName == "char")   basic = diBuilder->createBasicType("char", 8, DW_ATE_signed_char);
        else if (tv.TypeName == "short")  basic = diBuilder->createBasicType("short", 16, DW_ATE_signed);
        else if (tv.TypeName == "long")   basic = diBuilder->createBasicType("long", 64, DW_ATE_signed);
        else if (tv.TypeName == "float")  basic = diBuilder->createBasicType("float", 32, DW_ATE_float);
        else if (tv.TypeName == "double") basic = diBuilder->createBasicType("double", 64, DW_ATE_float);
        else if (tv.TypeName == "bool")   basic = diBuilder->createBasicType("bool", 1, DW_ATE_boolean);
        else if (tv.TypeName == "i8")     basic = diBuilder->createBasicType("i8", 8, DW_ATE_signed);
        else if (tv.TypeName == "i16")    basic = diBuilder->createBasicType("i16", 16, DW_ATE_signed);
        else if (tv.TypeName == "i32")    basic = diBuilder->createBasicType("i32", 32, DW_ATE_signed);
        else if (tv.TypeName == "i64")    basic = diBuilder->createBasicType("i64", 64, DW_ATE_signed);
        else if (tv.TypeName == "u8")     basic = diBuilder->createBasicType("u8", 8, DW_ATE_unsigned);
        else if (tv.TypeName == "u16")    basic = diBuilder->createBasicType("u16", 16, DW_ATE_unsigned);
        else if (tv.TypeName == "u32")    basic = diBuilder->createBasicType("u32", 32, DW_ATE_unsigned);
        else if (tv.TypeName == "u64")    basic = diBuilder->createBasicType("u64", 64, DW_ATE_unsigned);
        else if (tv.TypeName == "void")   basic = diBuilder->createUnspecifiedType("void");

        if (basic)
        {
            diTypeCache[tv.TypeName] = basic;
            return basic;
        }

        // Struct (including generic instantiations like Box__int and the built-in `string`).
        auto sdIt = dataStructures.find(tv.TypeName);
        if (sdIt != dataStructures.end() && sdIt->second.StructType != nullptr
            && !sdIt->second.StructType->isOpaque())
        {
            auto* st = sdIt->second.StructType;
            uint64_t sizeBits = DL.getTypeAllocSizeInBits(st);
            uint64_t alignBits = (uint64_t)DL.getABITypeAlign(st).value() * 8;
            const llvm::StructLayout* SL = DL.getStructLayout(st);

            // Insert a forward declaration into the cache before recursing into
            // fields - this lets a struct that contains a pointer to itself (or
            // a mutually-recursive struct) resolve without infinite recursion.
            auto fwd = diBuilder->createReplaceableCompositeType(
                llvm::dwarf::DW_TAG_structure_type, tv.TypeName, compileUnit, diFile, 0);
            diTypeCache[tv.TypeName] = fwd;

            std::vector<llvm::Metadata*> members;
            const auto& fields = sdIt->second.StructFields;
            unsigned n = st->getNumElements();
            for (size_t i = 0; i < fields.size() && i < n; ++i)
            {
                const auto& f = fields[i];
                auto* fieldTy = st->getElementType((unsigned)i);
                uint64_t fSize = DL.getTypeAllocSizeInBits(fieldTy);
                uint64_t fAlign = (uint64_t)DL.getABITypeAlign(fieldTy).value() * 8;
                uint64_t fOffset = SL->getElementOffsetInBits((unsigned)i);
                auto* fDI = GetDIType(f);
                members.push_back(diBuilder->createMemberType(
                    fwd, f.VariableName, diFile, 0,
                    fSize, (uint32_t)fAlign, fOffset, llvm::DINode::FlagZero, fDI));
            }

            auto real = diBuilder->createStructType(
                compileUnit, tv.TypeName, diFile, 0,
                sizeBits, (uint32_t)alignBits, llvm::DINode::FlagZero, nullptr,
                diBuilder->getOrCreateArray(members));

            fwd->replaceAllUsesWith(real);
            diTypeCache[tv.TypeName] = real;
            return real;
        }

        // Unknown / opaque - leave as unspecified so the debugger at least knows the name.
        // Do NOT cache: a struct may be opaque now (e.g. generic instantiation processed
        // before its body is laid out) and become a real composite later. Caching the
        // unspecified version would poison every subsequent lookup with no struct fields.
        return diBuilder->createUnspecifiedType(tv.TypeName);
    }

    // Registers the built-in `string` value type: { i8* _ptr, i32 _len }.
    // data() returns _ptr; length() returns _len.  No vtable - direct struct access.
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

        // Use the function-table overload of malloc so that on Win32 we get the
        // i64-wrapper (_malloc_U8Ptr_i64_) rather than the raw extern malloc(i32).
        auto* mallocFn = GetFunction("malloc");
        if (!mallocFn)
        {
            auto* mallocTy = llvm::FunctionType::get(ptrTy, { i64Ty }, false);
            mallocFn = llvm::dyn_cast<llvm::Function>(
                module->getOrInsertFunction("malloc", mallocTy).getCallee());
        }

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
        module = std::make_unique<llvm::Module>("cflat", *context);
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

        // Prefer operator delete (allocator-aware) over raw free.
        // operator delete is defined in memory.cb which is imported before string.cb,
        // so it should be available by the time the first string dtor is needed.
        auto* freeFn  = GetFunction("operator delete");
        if (!freeFn) freeFn = module->getFunction("free");
        if (!freeFn) return;   // neither available yet; destructor cannot be created

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
    // Works for both float (f32) and double (f64) - type is inferred from floatVal.
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
        return intVal; // same width - no-op
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

    // Resolve clang-cl.exe, preferring the copy deployed next to cflat.exe (runtimeDir)
    // over whatever may be on PATH - this keeps the toolchain self-contained and mirrors
    // how lld-link is located. Returns "" if not found anywhere.
    std::string FindClangCl() const
    {
        if (!runtimeDir.empty())
        {
            llvm::SmallString<256> candidate(runtimeDir);
            llvm::sys::path::append(candidate, "clang-cl.exe");
            if (llvm::sys::fs::exists(candidate))
                return candidate.str().str();
        }
        if (auto p = llvm::sys::findProgramByName("clang-cl"))
            return *p;
        return "";
    }

    // Compile a real C source file with clang-cl into a temporary object, recorded
    // in cObjectFiles_ for EmitExecutable to link. clang-cl auto-detects the Windows
    // SDK / MSVC, so no explicit include-dir discovery is needed here. Headers come
    // from the .cb's own 'extern' declarations; this object supplies the definitions.
    bool CompileCFile(const std::string& cSourcePath, const std::string& programAlias = "")
    {
        // Auto-discover the C function signatures so the importing .cb does not need
        // hand-written 'extern' declarations. Best-effort: a failure here just falls
        // back to the old behaviour (caller must declare the functions manually).
        // When programAlias is set ('import program "x.c" as Alias'), the C `main` is
        // registered as `__imported_main_<Alias>` and wired into programTable here.
        ExtractCSignatures(cSourcePath, programAlias);

        // In LSP / analysis mode we only need the signatures (already extracted above)
        // for hover, completion and go-to-definition - not a linkable object file.
        // symbolSink_ is set only by the LSP server, so it is a reliable mode flag.
        if (symbolSink_ != nullptr)
            return true;

        const std::string clangPath = FindClangCl();
        if (clangPath.empty())
        {
            LogError(std::format("clang-cl.exe not found - cannot compile C source '{}'.", cSourcePath));
            return false;
        }

        // Temp object next to the system temp dir; removed after linking.
        llvm::SmallString<256> objFile;
        if (auto ec = llvm::sys::fs::createTemporaryFile("cflat_c", "obj", objFile))
        {
            LogError(std::format("could not create temp object for C source '{}': {}", cSourcePath, ec.message()));
            return false;
        }
        std::string objPath = objFile.str().str();

        const std::string target = (platformValue == 32)
            ? "--target=i686-pc-windows-msvc"
            : "--target=x86_64-pc-windows-msvc";
        const std::string foArg = "/Fo" + objPath;

        std::vector<std::string> argStrs = { clangPath, "/c", "/nologo", target, cSourcePath, foArg };
        if (cOptLevel_ >= 2)      argStrs.push_back("/O2");
        else if (cOptLevel_ == 1) argStrs.push_back("/O1");
        if (cDebugInfo_)          argStrs.push_back("/Z7"); // CodeView in the obj -> PDB via /DEBUG
        for (const auto& def : cDefines_) argStrs.push_back("/D" + def);
        // For an imported program, rename the object's `main` symbol so it matches the
        // `__imported_main_<Alias>` declaration (above) and does not collide with the
        // CFlat program's own entry `main` at link time.
        if (!programAlias.empty())
            argStrs.push_back("/Dmain=__imported_main_" + programAlias);

        std::vector<llvm::StringRef> args;
        for (auto& s : argStrs) args.push_back(s);

        if (verbose)
        {
            std::cout << "[verbose] compiling C source: " << cSourcePath << " -> " << objPath << "\n";
            std::cout << "[verbose]   clang-cl";
            for (size_t i = 1; i < argStrs.size(); ++i) std::cout << " " << argStrs[i];
            std::cout << "\n";
        }

        std::string clangCompileErr;
        int rc = llvm::sys::ExecuteAndWait(clangPath, args, std::nullopt, {}, 0, 0, &clangCompileErr);
        if (rc != 0)
        {
            llvm::sys::fs::remove(objPath);
            LogError(std::format("clang-cl failed to compile C source '{}' (exit {}){}{}",
                cSourcePath, rc, clangCompileErr.empty() ? "" : ": ", clangCompileErr));
            return false;
        }

        cObjectFiles_.push_back(objPath);
        return true;
    }

    // Map a (preferably desugared) C type spelling onto a CFlat TypeAndValue.
    // Pre-pass over a clang JSON AST: collect top-level user typedefs so the signature
    // mapper can later chase `HANDLE` -> `void *`, `SOCKET` -> `unsigned long long`, etc.
    // Implicit typedefs (compiler-injected like __int128_t, size_t aliases) are skipped -
    // they pull in noise without helping resolution. First-writer-wins so a later header
    // doesn't clobber an earlier one's spelling. Safe to call multiple times.
    void CollectCTypedefs(const llvm::json::Array* topLevel)
    {
        if (!topLevel) return;
        for (const auto& v : *topLevel)
        {
            const llvm::json::Object* node = v.getAsObject();
            if (!node) continue;
            auto kind = node->getString("kind");
            if (!kind || *kind != "TypedefDecl") continue;
            if (auto imp = node->getBoolean("isImplicit"); imp && *imp) continue;
            auto name = node->getString("name");
            if (!name) continue;
            const llvm::json::Object* typeObj = node->getObject("type");
            if (!typeObj) continue;
            std::string target;
            if (auto d = typeObj->getString("desugaredQualType")) target = d->str();
            else if (auto q = typeObj->getString("qualType"))     target = q->str();
            if (target.empty()) continue;
            cTypedefMap_.emplace(name->str(), target);
        }
    }

    // Returns false for anything the extern ABI cannot faithfully pass - struct/union
    // by value, > 2 pointer levels, or an unknown scalar - so the caller can skip the
    // whole function rather than emit a wrong signature. This intentionally mirrors the
    // scalar+pointer subset that GetCCompatibleType already supports for hand-written
    // extern declarations.
    bool MapCTypeToTypeAndValue(std::string ctype, TypeAndValue& out)
    {
        std::unordered_set<std::string> visited;
        return MapCTypeToTypeAndValueImpl(std::move(ctype), out, visited);
    }

    bool MapCTypeToTypeAndValueImpl(std::string ctype, TypeAndValue& out,
                                    std::unordered_set<std::string>& visited)
    {
        // Arrays decay to a pointer; drop the '[...]' and bump the pointer level.
        int ptr = 0;
        if (auto br = ctype.find('['); br != std::string::npos)
        {
            ptr++;
            ctype = ctype.substr(0, br);
        }
        ptr += (int)std::count(ctype.begin(), ctype.end(), '*');
        ctype.erase(std::remove(ctype.begin(), ctype.end(), '*'), ctype.end());

        // Strip cv / nullability qualifiers - they do not affect the ABI here.
        auto stripWord = [&](const char* w)
        {
            std::string word = w;
            for (size_t pos; (pos = ctype.find(word)) != std::string::npos; )
                ctype.erase(pos, word.size());
        };
        for (const char* q : { "const", "volatile", "restrict", "__restrict", "__restrict__",
                               "_Nonnull", "_Nullable", "_Null_unspecified" })
            stripWord(q);

        // Collapse runs of whitespace and trim - so "unsigned   long  long" normalizes.
        std::string base;
        bool prevSpace = true; // leading -> skip
        for (char c : ctype)
        {
            bool isSpace = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
            if (isSpace) { if (!prevSpace) base += ' '; prevSpace = true; }
            else { base += c; prevSpace = false; }
        }
        while (!base.empty() && base.back() == ' ') base.pop_back();

        if (ptr > 2)
            return false;

        // enum decays to int; struct/union pointers become opaque void* (the call only
        // needs a pointer-sized slot). For struct/union *by value* we look up the tag in
        // dataStructures - the auto-registered C struct gets a CFlat TypeAndValue with that
        // tag name and Pointer=false, so call-site ABI lowering can see the layout.
        std::string mapped;
        if (base.rfind("enum ", 0) == 0)
        {
            mapped = "int";
        }
        else if (ptr == 0 && (base.rfind("struct ", 0) == 0 || base.rfind("union ", 0) == 0))
        {
            std::string tag = (base.rfind("struct ", 0) == 0)
                ? base.substr(7)
                : base.substr(6);
            // Trim any trailing whitespace (shouldn't happen post-normalize but be defensive).
            while (!tag.empty() && tag.back() == ' ') tag.pop_back();
            if (tag.empty() || dataStructures.find(tag) == dataStructures.end())
                return false;
            out.TypeName = tag;
            out.Pointer = false;
            out.ElemPointer = false;
            return true;
        }
        else
        {
            static const std::unordered_map<std::string, std::string> scalarMap = {
                { "void", "void" }, { "_Bool", "bool" }, { "bool", "bool" },
                { "char", "char" }, { "signed char", "i8" }, { "unsigned char", "u8" },
                { "short", "short" }, { "short int", "short" }, { "signed short", "short" },
                { "unsigned short", "u16" }, { "unsigned short int", "u16" },
                { "int", "int" }, { "signed", "int" }, { "signed int", "int" },
                { "unsigned", "u32" }, { "unsigned int", "u32" },
                // Windows is LLP64: 'long' is 32-bit, 'long long' is 64-bit.
                { "long", "i32" }, { "long int", "i32" }, { "signed long", "i32" },
                { "unsigned long", "u32" }, { "unsigned long int", "u32" },
                { "long long", "i64" }, { "long long int", "i64" }, { "signed long long", "i64" },
                { "unsigned long long", "u64" }, { "unsigned long long int", "u64" },
                { "float", "float" }, { "double", "double" }, { "long double", "double" },
            };
            auto it = scalarMap.find(base);
            if (it != scalarMap.end())
                mapped = it->second;
            else if (ptr > 0)
                mapped = "void"; // unknown pointee (struct*, function ptr, ...) -> opaque ptr
            else if (dataStructures.find(base) != dataStructures.end())
            {
                // Bare typedef-style spelling resolved to a registered C struct (e.g. clang
                // emitted "Point" for a `typedef struct Point Point;` without the tag prefix).
                out.TypeName = base;
                out.Pointer = false;
                out.ElemPointer = false;
                return true;
            }
            else
            {
                // Final fallback: chase a user typedef recorded by CollectCTypedefs. We
                // append the original pointer-star count so HANDLE (ptr=0) recursing into
                // "void *" lands at void with ptr=1. Visited-set prevents pathological
                // self-referential typedefs from looping (clang would have errored, but
                // be defensive).
                auto td = cTypedefMap_.find(base);
                if (td != cTypedefMap_.end() && visited.insert(base).second)
                {
                    std::string substituted = td->second;
                    if (ptr > 0) substituted += std::string(ptr, '*');
                    return MapCTypeToTypeAndValueImpl(std::move(substituted), out, visited);
                }
                return false;    // struct/union by value or unknown scalar
            }
        }

        out.TypeName = mapped;
        out.Pointer = ptr >= 1;
        out.ElemPointer = ptr == 2;
        return true;
    }

    // FNV-1a 64-bit hash of a file's bytes. Returns false if the file can't be read.
    bool HashFileContents(const std::string& path, uint64_t& outHash) const
    {
        auto bufOrErr = llvm::MemoryBuffer::getFile(path);
        if (!bufOrErr) return false;
        uint64_t h = 1469598103934665603ULL; // FNV offset basis
        for (unsigned char c : (*bufOrErr)->getBuffer())
        {
            h ^= c;
            h *= 1099511628211ULL; // FNV prime
        }
        outHash = h;
        return true;
    }

    // Register a set of extracted C signatures into the function table (as unmangled,
    // C-compatible externs) and into the LSP symbol sink. Shared by the cache-hit and
    // cache-miss paths so both behave identically.
    // programAlias non-empty: this .c is being brought in via `import program "x.c" as Alias`.
    // The C `main` is registered under `__imported_main_<Alias>` (matching the renamed object
    // symbol from CompileCFile's /Dmain define) and wired into programTable so the imported-
    // program synthesis (struct + run wrapper) treats it exactly like a .cb imported program.
    void RegisterCSignatures(const std::vector<CSigEntry>& sigs, const std::string& fileForLsp,
                             const std::string& programAlias = "")
    {
        for (const CSigEntry& e : sigs)
        {
            std::string regName  = e.name;
            bool        isProgMain = (!programAlias.empty() && e.name == "main");
            if (isProgMain)
                regName = "__imported_main_" + programAlias;

            // external=true: unmangled name + C-compatible types; cdecl on the call.
            CreateFunctionDeclaration(regName, e.ret, e.params, /*external=*/true, e.variadic,
                                      /*returnsOwned=*/false, /*isMethod=*/false,
                                      /*isStdcall=*/false, /*isCdecl=*/true);

            if (isProgMain)
            {
                programTable[programAlias].MainFunction     = module->getFunction(regName);
                programTable[programAlias].IsImportedProgram = true;
                continue; // not a user-facing symbol; skip the LSP sink registration below
            }

            if (auto* s = GetSymbolSink())
            {
                std::string sig = e.ret.TypeName + (e.ret.Pointer ? "*" : "") + " " + e.name + "(";
                bool first = true;
                for (const auto& p : e.params)
                {
                    if (!first) sig += ", ";
                    first = false;
                    sig += p.TypeName;
                    if (p.Pointer) sig += "*";
                    if (!p.VariableName.empty()) sig += " " + p.VariableName;
                }
                sig += ")";
                s->Register(SymbolKind::Function, e.name, fileForLsp, e.line, e.col < 0 ? 0 : e.col, sig);
            }
        }

        if (verbose)
            std::cout << "[verbose]   registered " << sigs.size() << " C function(s) from " << fileForLsp << "\n";
    }

    // Spawn a clang JSON AST dump (argStrs[0] is the clang path) and return stdout as a
    // string. All three std streams are redirected to temp files (stdin from an empty
    // temp) so a spawned child never references the LSP server's JSON-RPC pipes - leaving
    // a redirect inherited (STARTF_USESTDHANDLES) breaks the protocol channel and kills
    // the server. The spawn is serialized because concurrent CreateProcess-with-redirects
    // calls race on Windows and crash the LSP worker pool.
    bool SpawnClangJson(const std::vector<std::string>& argStrs, std::string& outJson)
    {
        llvm::SmallString<256> inFile, jsonFile, errFile;
        if (llvm::sys::fs::createTemporaryFile("cflat_ast", "in",  inFile))  return false;
        if (llvm::sys::fs::createTemporaryFile("cflat_ast", "json", jsonFile))
        {
            llvm::sys::fs::remove(inFile);
            return false;
        }
        if (llvm::sys::fs::createTemporaryFile("cflat_ast", "err", errFile))
        {
            llvm::sys::fs::remove(inFile);
            llvm::sys::fs::remove(jsonFile);
            return false;
        }
        const std::string inPath   = inFile.str().str();
        const std::string jsonPath = jsonFile.str().str();
        const std::string errPath  = errFile.str().str();
        struct TmpRemover {
            std::string a, b, c;
            ~TmpRemover() { llvm::sys::fs::remove(a); llvm::sys::fs::remove(b); llvm::sys::fs::remove(c); }
        } tmpRemover{ inPath, jsonPath, errPath };

        std::vector<llvm::StringRef> args;
        for (auto& s : argStrs) args.push_back(s);

        std::optional<llvm::StringRef> redirects[3] = {
            llvm::StringRef(inPath), llvm::StringRef(jsonPath), llvm::StringRef(errPath)
        };

        std::string execErr;
        int rc;
        {
            std::lock_guard<std::mutex> spawnLock(cClangSpawnMutex_);
            rc = llvm::sys::ExecuteAndWait(argStrs[0], args, std::nullopt, redirects, 0, 0, &execErr);
        }
        if (rc != 0)
        {
            if (verbose) std::cout << "[verbose]   AST dump failed (exit " << rc << ")\n";
            return false;
        }

        auto bufOrErr = llvm::MemoryBuffer::getFile(jsonPath);
        if (!bufOrErr) return false;
        outJson = (*bufOrErr)->getBuffer().str();
        return true;
    }

    // Variant of SpawnClangJson that captures stdout regardless of exit code. Needed for the
    // macro-resolution stub: one bad-apple macro produces clang errors (non-zero exit), but
    // the other anonymous enums still land in the AST and remain usable.
    bool SpawnClangCapture(const std::vector<std::string>& argStrs, std::string& outStdout)
    {
        llvm::SmallString<256> inFile, outFile, errFile;
        if (llvm::sys::fs::createTemporaryFile("cflat_cap", "in",  inFile))  return false;
        if (llvm::sys::fs::createTemporaryFile("cflat_cap", "out", outFile))
        {
            llvm::sys::fs::remove(inFile);
            return false;
        }
        if (llvm::sys::fs::createTemporaryFile("cflat_cap", "err", errFile))
        {
            llvm::sys::fs::remove(inFile);
            llvm::sys::fs::remove(outFile);
            return false;
        }
        const std::string inPath  = inFile.str().str();
        const std::string outPath = outFile.str().str();
        const std::string errPath = errFile.str().str();
        struct TmpRemover {
            std::string a, b, c;
            ~TmpRemover() { llvm::sys::fs::remove(a); llvm::sys::fs::remove(b); llvm::sys::fs::remove(c); }
        } tmpRemover{ inPath, outPath, errPath };

        std::vector<llvm::StringRef> args;
        for (auto& s : argStrs) args.push_back(s);

        std::optional<llvm::StringRef> redirects[3] = {
            llvm::StringRef(inPath), llvm::StringRef(outPath), llvm::StringRef(errPath)
        };

        std::string execErr;
        {
            std::lock_guard<std::mutex> spawnLock(cClangSpawnMutex_);
            (void)llvm::sys::ExecuteAndWait(argStrs[0], args, std::nullopt, redirects, 0, 0, &execErr);
        }
        // Intentionally ignore rc - the caller wants whatever clang managed to produce.

        auto bufOrErr = llvm::MemoryBuffer::getFile(outPath);
        if (!bufOrErr) return false;
        outStdout = (*bufOrErr)->getBuffer().str();
        return true;
    }

    // Spawn clang's JSON AST dump for a C source and parse out every externally-linkable
    // function it *defines*. Fills outSigs; returns false on any clang / parse failure.
    bool RunClangAstExtract(const std::string& clangPath, const std::string& cSourcePath,
                            std::vector<CSigEntry>& outSigs,
                            std::vector<CRecordEntry>& outRecords)
    {
        const std::string target = (platformValue == 32)
            ? "--target=i686-pc-windows-msvc"
            : "--target=x86_64-pc-windows-msvc";

        std::vector<std::string> argStrs = {
            clangPath, "/nologo", target, "-Xclang", "-ast-dump=json",
            "-fsyntax-only"
        };
        for (const auto& def : cDefines_) argStrs.push_back("/D" + def);
        argStrs.push_back(cSourcePath);

        if (verbose)
            std::cout << "[verbose] extracting C signatures: " << cSourcePath << " (clang AST dump)\n";

        std::string jsonText;
        if (!SpawnClangJson(argStrs, jsonText))
            return false;

        llvm::Expected<llvm::json::Value> root = llvm::json::parse(jsonText);
        if (!root) { llvm::consumeError(root.takeError()); return false; }

        const llvm::json::Object* tu = root->getAsObject();
        if (!tu) return false;
        const llvm::json::Array* topLevel = tu->getArray("inner");
        if (!topLevel) return true; // parsed fine, nothing to register

        // Collect typedefs first so the signature mapper can chase pointer aliases
        // (HANDLE -> void *, SOCKET -> uintptr_t, etc.) during the function walk.
        CollectCTypedefs(topLevel);

        auto qualTypeOf = [](const llvm::json::Object* typeObj) -> std::string
        {
            if (!typeObj) return "";
            if (auto d = typeObj->getString("desugaredQualType")) return d->str();
            if (auto q = typeObj->getString("qualType")) return q->str();
            return "";
        };

        // First pass: collect record (struct/union) decls. Recording them up front
        // (before function signatures) lets MapCTypeToTypeAndValue resolve struct-by-
        // value parameter / return types against the registered structs.
        for (const llvm::json::Value& v : *topLevel)
        {
            const llvm::json::Object* node = v.getAsObject();
            if (!node) continue;
            auto kind = node->getString("kind");
            if (!kind || *kind != "RecordDecl") continue;
            auto nm = node->getString("name");
            if (!nm || nm->empty()) continue; // skip anonymous records (handled via typedef path later, if ever)
            auto cd = node->getBoolean("completeDefinition");
            if (!cd || !*cd) continue; // forward decls give us no layout
            auto tag = node->getString("tagUsed");
            CRecordEntry rec;
            rec.name = nm->str();
            rec.isUnion = tag && *tag == "union";
            if (auto* inner = node->getArray("inner"))
            {
                for (const auto& c : *inner)
                {
                    const llvm::json::Object* co = c.getAsObject();
                    if (!co) continue;
                    auto ck = co->getString("kind");
                    if (!ck || *ck != "FieldDecl") continue;
                    auto fname = co->getString("name");
                    if (!fname) continue;
                    CRecordFieldEntry fe;
                    fe.name = fname->str();
                    fe.ctype = qualTypeOf(co->getObject("type"));
                    rec.fields.push_back(std::move(fe));
                }
            }
            if (auto* loc = node->getObject("loc"))
            {
                if (auto l = loc->getInteger("line")) rec.line = (int)*l;
                if (auto cc = loc->getInteger("col")) rec.col = (int)*cc - 1;
            }
            outRecords.push_back(std::move(rec));
        }

        // Register the records now so MapCTypeToTypeAndValue can resolve struct-by-value
        // parameter / return types in the function-decl pass below.
        RegisterCRecords(outRecords, cSourcePath);

        for (const llvm::json::Value& v : *topLevel)
        {
            const llvm::json::Object* node = v.getAsObject();
            if (!node) continue;
            auto kind = node->getString("kind");
            if (!kind || *kind != "FunctionDecl") continue;

            // static functions are not externally linkable - skip.
            if (auto sc = node->getString("storageClass"); sc && *sc == "static") continue;

            const llvm::json::Array* fnInner = node->getArray("inner");

            // Only functions *defined* in this translation unit end up as object
            // symbols we can link. A body means a CompoundStmt child; header-only
            // prototypes (e.g. from <stdio.h>) have none and are skipped.
            bool hasBody = false;
            if (fnInner)
                for (const auto& c : *fnInner)
                    if (auto* co = c.getAsObject())
                        if (auto ck = co->getString("kind"); ck && *ck == "CompoundStmt")
                        { hasBody = true; break; }
            if (!hasBody) continue;

            auto nm = node->getString("name");
            if (!nm || nm->empty()) continue;

            CSigEntry entry;
            entry.name = nm->str();

            std::string funcQual = qualTypeOf(node->getObject("type"));
            entry.variadic = funcQual.find("...") != std::string::npos;

            // Return type = the spelling before the parameter list's '('.
            std::string retStr = funcQual;
            if (auto paren = funcQual.find('('); paren != std::string::npos)
                retStr = funcQual.substr(0, paren);

            if (!MapCTypeToTypeAndValue(retStr, entry.ret))
            {
                if (verbose) std::cout << "[verbose]   skipping '" << entry.name
                                       << "': unsupported return type '" << retStr << "'\n";
                continue;
            }

            bool paramsOk = true;
            if (fnInner)
            {
                for (const auto& c : *fnInner)
                {
                    const llvm::json::Object* co = c.getAsObject();
                    if (!co) continue;
                    auto ck = co->getString("kind");
                    if (!ck || *ck != "ParmVarDecl") continue;

                    TypeAndValue ptv;
                    std::string pQual = qualTypeOf(co->getObject("type"));
                    if (!MapCTypeToTypeAndValue(pQual, ptv))
                    {
                        if (verbose) std::cout << "[verbose]   skipping '" << entry.name
                                               << "': unsupported parameter type '" << pQual << "'\n";
                        paramsOk = false;
                        break;
                    }
                    if (auto pn = co->getString("name")) ptv.VariableName = pn->str();
                    entry.params.push_back(ptv);
                }
            }
            if (!paramsOk) continue;

            if (auto* loc = node->getObject("loc"))
            {
                if (auto l = loc->getInteger("line")) entry.line = (int)*l;
                if (auto cc = loc->getInteger("col")) entry.col = (int)*cc - 1;
            }

            outSigs.push_back(std::move(entry));
        }

        return true;
    }

    // Auto-discover C function signatures so `import "foo.c";` needs no hand-written
    // 'extern'. Results are cached per .c file: the (slow) clang AST dump is skipped
    // when the file is unchanged - the timestamp is checked first, and the content hash
    // is consulted only when the timestamp differs (e.g. touch / checkout with no real
    // change). Feeds both the function table and the LSP symbol sink.
    bool ExtractCSignatures(const std::string& cSourcePath, const std::string& programAlias = "")
    {
        // Canonical path: stable cache key + the real .c for LSP go-to-definition.
        llvm::SmallString<256> realPath;
        std::string fileForLsp = cSourcePath;
        if (!llvm::sys::fs::real_path(cSourcePath, realPath))
            fileForLsp = realPath.str().str();

        // Defines can gate which functions a .c defines, so fold them into the cache key
        // (the file path alone is the LSP identity; the key is path + defines).
        std::string cacheKey = fileForLsp;
        for (const auto& def : cDefines_) cacheKey += "|D" + def;

        // Hash the file at most once per call, and only when actually needed.
        uint64_t currentHash = 0;
        bool haveHash = false;
        auto hashNow = [&]() -> uint64_t
        {
            if (!haveHash) { HashFileContents(fileForLsp, currentHash); haveHash = true; }
            return currentHash;
        };

        std::error_code mtEc;
        auto currentMtime = std::filesystem::last_write_time(fileForLsp, mtEc);

        // --- Cache lookup under lock. Copy the signatures out, then register after the
        //     lock is released, so the global cache never serializes per-backend work. ---
        std::vector<CSigEntry> hitSigs;
        std::vector<CRecordEntry> hitRecords;
        bool hit = false;
        {
            std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
            auto cacheIt = cFileSigCache_.find(cacheKey);
            if (!mtEc && cacheIt != cFileSigCache_.end())
            {
                CFileSigCacheEntry& entry = cacheIt->second;
                if (entry.mtime == currentMtime)
                {
                    if (verbose) std::cout << "[verbose] C signatures cache hit (mtime) for " << fileForLsp << "\n";
                    hitSigs = entry.sigs;
                    hitRecords = entry.records;
                    hit = true;
                }
                // Timestamp moved but content may be identical - only now pay for a hash.
                else if (hashNow() == entry.hash)
                {
                    if (verbose) std::cout << "[verbose] C signatures cache hit (hash) for " << fileForLsp << "\n";
                    entry.mtime = currentMtime; // refresh so the next check short-circuits on mtime
                    hitSigs = entry.sigs;
                    hitRecords = entry.records;
                    hit = true;
                }
            }
        }
        if (hit)
        {
            // Records must be registered before sigs so signatures referencing struct-by-
            // value resolve to the same dataStructures entries on cache hits.
            RegisterCRecords(hitRecords, fileForLsp);
            RegisterCSignatures(hitSigs, fileForLsp, programAlias);
            return true;
        }

        // Cache miss - run clang and parse (outside the lock; concurrent first-time
        // misses for the same file just redo the work harmlessly, last writer wins).
        const std::string clangPath = FindClangCl();
        if (clangPath.empty())
            return false;

        std::vector<CSigEntry> sigs;
        std::vector<CRecordEntry> records;
        if (!RunClangAstExtract(clangPath, cSourcePath, sigs, records))
            return false;

        if (!mtEc)
        {
            CFileSigCacheEntry entry;
            entry.mtime = currentMtime;
            entry.hash  = hashNow();
            entry.sigs  = sigs;
            entry.records = records;
            std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
            cFileSigCache_[cacheKey] = std::move(entry);
        }

        // Records were already registered inside RunClangAstExtract (so it could map
        // struct-by-value parameter types); do not re-register here.
        RegisterCSignatures(sigs, fileForLsp, programAlias);
        return true;
    }

    // Depth-first search for the first node carrying an evaluated integer "value" string
    // (clang puts the evaluated enum value on the wrapping ConstantExpr / IntegerLiteral).
    // Returns false when the enumerator has no initializer (caller uses the running count).
    static bool FindConstantValue(const llvm::json::Object* node, long long& out)
    {
        if (!node) return false;
        if (auto v = node->getString("value"))
        {
            const std::string s = v->str();
            try {
                size_t consumed = 0;
                long long parsed = std::stoll(s, &consumed);
                if (consumed == s.size()) { out = parsed; return true; }
            } catch (...) { /* not a plain decimal (e.g. a string literal) - keep searching */ }
        }
        if (auto* inner = node->getArray("inner"))
            for (const auto& c : *inner)
                if (FindConstantValue(c.getAsObject(), out)) return true;
        return false;
    }

    // Register C enum constants as bare global int constants - matching C's flat enum
    // scope (the user writes CURLOPT_URL, not CURLoption.CURLOPT_URL). Built directly via
    // builder->getIntN to sidestep CreateConstant's stoi limitation on wide values.
    void RegisterCEnums(const std::vector<CEnumEntry>& enums, const std::string& fileForLsp)
    {
        for (const CEnumEntry& e : enums)
        {
            if (e.name.empty()) continue;
            // First writer wins: a hand-written declaration or an earlier header takes
            // precedence over a duplicate constant name.
            if (globalNamedVariable.count(e.name)) continue;

            bool wide = (e.value < INT32_MIN || e.value > INT32_MAX);
            TypeAndValue tv;
            tv.TypeName     = wide ? "i64" : "int";
            tv.VariableName = e.name;
            tv.Pointer      = false;
            llvm::Constant* c = wide
                ? static_cast<llvm::Constant*>(builder->getInt64((uint64_t)e.value))
                : static_cast<llvm::Constant*>(builder->getInt32((uint32_t)(int32_t)e.value));
            CreateGlobalVariable(tv, c);

            if (auto* s = GetSymbolSink())
                s->Register(SymbolKind::Variable, e.name, fileForLsp, e.line, e.col < 0 ? 0 : e.col,
                            tv.TypeName + " " + e.name);
        }
        if (verbose)
            std::cout << "[verbose]   registered " << enums.size() << " C enum constant(s) from " << fileForLsp << "\n";
    }

    // Auto-register C struct/union decls extracted from a header or .c source as CFlat
    // struct types. Two passes: first creates an opaque shell for every record so fields
    // that reference siblings (or self-pointers) can resolve, then fills each body via
    // MapCTypeToTypeAndValue (which now accepts "struct Name" once the shell exists).
    // First-writer-wins on dataStructures: a hand-written CFlat struct with the same name,
    // or an earlier header's contribution, takes precedence over later C-side definitions.
    void RegisterCRecords(const std::vector<CRecordEntry>& records, const std::string& fileForLsp)
    {
        if (records.empty()) return;

        // Pass 1: opaque shells. Skip ones already in dataStructures so we never overwrite
        // a CFlat-defined struct with C metadata.
        std::vector<const CRecordEntry*> ours;
        ours.reserve(records.size());
        for (const auto& r : records)
        {
            if (r.name.empty()) continue;
            if (r.isUnion) continue; // unions deferred - layout is non-trivial (anonymous union member of struct, etc.)
            if (dataStructures.find(r.name) != dataStructures.end()) continue;
            // Create the opaque shell now so a later field can refer to `struct r.name *`
            // or to another record being registered in the same batch.
            CreateStructType(r.name, /*typeAndValues*/{});
            ours.push_back(&r);
        }

        // Pass 2: bodies. If any field type cannot be mapped, abandon this record and erase
        // the shell - leaving an opaque struct around would silently miscompile any caller
        // that tried to instantiate it.
        for (const CRecordEntry* rp : ours)
        {
            const CRecordEntry& r = *rp;
            std::vector<DeclTypeAndValue> fields;
            fields.reserve(r.fields.size());
            bool ok = true;
            for (const auto& f : r.fields)
            {
                TypeAndValue tv;
                if (!MapCTypeToTypeAndValue(f.ctype, tv))
                {
                    if (verbose) std::cout << "[verbose]   skipping C struct '" << r.name
                                           << "': unsupported field '" << f.name
                                           << "' of type '" << f.ctype << "'\n";
                    ok = false;
                    break;
                }
                DeclTypeAndValue d;
                static_cast<TypeAndValue&>(d) = tv;
                d.VariableName = f.name;
                fields.push_back(std::move(d));
            }
            if (!ok || fields.empty())
            {
                // Leave the shell in place for !ok with no body so we don't crash on later
                // references; the user will get a clear error if they try to use it.
                continue;
            }
            CreateStructType(r.name, fields);
            if (auto* s = GetSymbolSink())
                s->Register(SymbolKind::Struct, r.name, fileForLsp, r.line, r.col < 0 ? 0 : r.col,
                            "struct " + r.name);
        }

        if (verbose)
            std::cout << "[verbose]   registered " << ours.size() << " C record(s) from " << fileForLsp << "\n";
    }

    // Register object-like C macros (as resolved by ExtractCHeaderMacros) as bare global int
    // constants - same flat-scope rule as RegisterCEnums so the user writes CURLOPT_URL, not
    // qualified. First-writer-wins against an existing global. The LSP location points at the
    // original #define in the real header, not the synthesized resolution stub.
    void RegisterCMacros(const std::vector<CMacroEntry>& macros)
    {
        size_t registered = 0;
        for (const CMacroEntry& m : macros)
        {
            if (m.name.empty()) continue;
            if (globalNamedVariable.count(m.name)) continue;

            TypeAndValue tv;
            tv.VariableName = m.name;
            llvm::Constant* c = nullptr;
            if (m.isPointer)
            {
                // Sentinel pointer macro (e.g. INVALID_HANDLE_VALUE). Register as a
                // void* global initialized by reinterpreting the folded bit pattern
                // as a pointer - matches the C macro's natural type so direct compares
                // against a HANDLE-returning API succeed without an explicit cast.
                tv.TypeName = "void";
                tv.Pointer  = true;
                llvm::Type* i8Ptr = llvm::PointerType::get(*context, 0);
                llvm::Constant* bits = builder->getInt64((uint64_t)m.value);
                c = llvm::ConstantExpr::getIntToPtr(bits, i8Ptr);
            }
            else
            {
                bool wide = (m.value < INT32_MIN || m.value > INT32_MAX);
                tv.TypeName = wide ? "i64" : "int";
                tv.Pointer  = false;
                c = wide
                    ? static_cast<llvm::Constant*>(builder->getInt64((uint64_t)m.value))
                    : static_cast<llvm::Constant*>(builder->getInt32((uint32_t)(int32_t)m.value));
            }
            CreateGlobalVariable(tv, c);
            ++registered;

            if (auto* s = GetSymbolSink())
                s->Register(SymbolKind::Variable, m.name, m.file, m.line, m.col < 0 ? 0 : m.col,
                            tv.TypeName + (tv.Pointer ? "* " : " ") + m.name);
        }
        if (verbose && !macros.empty())
            std::cout << "[verbose]   registered " << registered << " C macro constant(s) (of "
                      << macros.size() << " object-like candidates)\n";
    }

    // Tokenize a function-like macro body and verify every token is on a safe allowlist:
    // integer literals (suffix stripped), parameter references, parens, and the operators
    // + - * / % & | ^ ~ ! << >> < <= > >= == != && || ? : . The translated body is
    // returned via 'out' on success. Rejections (calls into other macros, string/char
    // literals, member access, casts, '#'/'##', float literals, the comma operator, ...)
    // return false and the macro is dropped.
    bool TranslateMacroBody(const CFunctionMacroEntry& m, std::string& out) const
    {
        std::unordered_set<std::string> paramSet(m.params.begin(), m.params.end());
        out.clear();
        out.reserve(m.body.size());
        const std::string& s = m.body;
        size_t i = 0;
        bool hasContent = false;

        while (i < s.size())
        {
            char c = s[i];
            if (std::isspace((unsigned char)c)) { out += c; ++i; continue; }

            // Block comment: /* ... */ allowed (and stripped); line comments rejected.
            if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') return false;
            if (c == '/' && i + 1 < s.size() && s[i + 1] == '*')
            {
                i += 2;
                while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) ++i;
                if (i + 1 < s.size()) i += 2;
                continue;
            }

            // Identifier: must be a parameter (no calls, casts, or references to other macros).
            if (std::isalpha((unsigned char)c) || c == '_')
            {
                size_t start = i;
                while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_')) ++i;
                std::string ident = s.substr(start, i - start);
                if (!paramSet.count(ident)) return false;
                out += ident;
                hasContent = true;
                continue;
            }

            // Numeric literal: integers only (decimal or hex). Suffixes U/L/UL/LL/ULL are stripped
            // because cflat's lexer does not accept them. Float literals (presence of '.') are
            // rejected via the general '.' reject below.
            if (std::isdigit((unsigned char)c))
            {
                size_t start = i;
                if (c == '0' && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X'))
                {
                    i += 2;
                    while (i < s.size() && std::isxdigit((unsigned char)s[i])) ++i;
                }
                else
                {
                    while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
                }
                out += s.substr(start, i - start);
                while (i < s.size() && (s[i] == 'u' || s[i] == 'U' || s[i] == 'l' || s[i] == 'L')) ++i;
                hasContent = true;
                continue;
            }

            // String/char literals and disallowed punctuation.
            if (c == '"' || c == '\'')                     return false;
            if (c == '#' || c == '[' || c == ']' || c == '.' || c == ',' || c == ';') return false;
            if (c == '-' && i + 1 < s.size() && s[i + 1] == '>') return false; // ->

            // Two-char operators kept intact.
            if (i + 1 < s.size())
            {
                std::string two = s.substr(i, 2);
                static const std::unordered_set<std::string> twoChar = {
                    "<=", ">=", "==", "!=", "&&", "||", "<<", ">>"
                };
                if (twoChar.count(two)) { out += two; i += 2; hasContent = true; continue; }
            }

            // Single-character operators / grouping.
            static const std::string allowedSingle = "+-*/%&|^~!<>?:()";
            if (allowedSingle.find(c) != std::string::npos)
            {
                out += c;
                ++i;
                hasContent = true;
                continue;
            }

            return false; // anything else: reject
        }

        if (!hasContent) return false;
        while (!out.empty() && std::isspace((unsigned char)out.back())) out.pop_back();
        if (out.empty()) return false;

        // Defensive parens like '(n)' in macro bodies become C-style casts in CFlat
        // (`castExpression : '(' typeName ')' castExpression`), e.g. '(n) * 1024'
        // parses as 'cast n of *1024'. Strip parens around bare identifiers - they
        // are redundant in a real function and unblock the multiply/binary-op cases.
        size_t pos = 0;
        while ((pos = out.find('(', pos)) != std::string::npos)
        {
            size_t inner = pos + 1;
            while (inner < out.size() && std::isspace((unsigned char)out[inner])) ++inner;
            if (inner >= out.size() ||
                !(std::isalpha((unsigned char)out[inner]) || out[inner] == '_'))
            {
                ++pos; continue;
            }
            size_t identEnd = inner;
            while (identEnd < out.size() &&
                   (std::isalnum((unsigned char)out[identEnd]) || out[identEnd] == '_'))
                ++identEnd;
            size_t after = identEnd;
            while (after < out.size() && std::isspace((unsigned char)out[after])) ++after;
            if (after >= out.size() || out[after] != ')') { ++pos; continue; }
            out.replace(pos, after + 1 - pos, out.substr(inner, identEnd - inner));
            // Stay at pos: the rewrite may expose another stripping opportunity.
        }
        return true;
    }

    // Translate function-like C macros into CFlat 'auto' generic functions and queue
    // the synthesized source for parsing. Rejected macros are dropped silently (verbose
    // log emits the reason). Generated source shape per macro:
    //     auto NAME<T0,T1,...>(T0 p0, T1 p1, ...) { return (BODY); }
    void RegisterCFunctionMacros(const std::vector<CFunctionMacroEntry>& funcMacros,
                                 const std::string& fileForLsp)
    {
        if (funcMacros.empty()) return;

        std::string generated;
        size_t accepted = 0, rejected = 0, skipped = 0;

        for (const auto& m : funcMacros)
        {
            // Skip if a real symbol with this name already exists (extern function,
            // existing generic template, or even a bound C macro constant). First writer
            // wins, matching the existing object-like macro path.
            if (functionTable.count(m.name) ||
                globalNamedVariable.count(m.name))
            {
                ++skipped;
                if (verbose) std::cout << "[verbose]   skip macro " << m.name << ": name already defined\n";
                continue;
            }

            std::string translatedBody;
            if (!TranslateMacroBody(m, translatedBody))
            {
                ++rejected;
                if (verbose) std::cout << "[verbose]   reject macro " << m.name << ": body uses unsupported tokens\n";
                continue;
            }

            generated += "auto " + m.name + "<";
            for (size_t i = 0; i < m.params.size(); ++i)
            {
                if (i) generated += ", ";
                generated += "T" + std::to_string(i);
            }
            generated += ">(";
            for (size_t i = 0; i < m.params.size(); ++i)
            {
                if (i) generated += ", ";
                generated += "T" + std::to_string(i) + " " + m.params[i];
            }
            generated += ") { return (" + translatedBody + "); }\n";
            ++accepted;
        }

        if (verbose)
            std::cout << "[verbose]   function-like C macros: " << accepted << " translated, "
                      << rejected << " rejected, " << skipped << " skipped (already defined) from "
                      << fileForLsp << "\n";

        if (!generated.empty())
            pendingMacroSources_.push_back({ fileForLsp + "@cmacros", std::move(generated) });
    }

    // Discover object-like macros contributed by a bound C header and resolve their integer
    // values. Two clang spawns:
    //   Pass A (-E -dD): preprocessed text with #define directives preserved and `# line "file"`
    //     markers; we walk the lines, track the current header file, and collect every
    //     #define NAME <replacement> whose source file is in scope. Function-like macros
    //     (#define NAME(args) ...) and empty replacements are skipped.
    //   Pass B (-ast-dump=json on a synthesized stub): for each candidate, emit an anonymous
    //     enum { __cflat_m_NAME = (long long)(NAME) }. Per-macro isolation means a bad apple
    //     (non-constant, string, or compound-literal macro) only invalidates its own enum;
    //     the others still appear as EnumConstantDecls in the AST and get harvested.
    // String/float macros and ones whose value clang cannot fold to a constant integer are
    // dropped silently (verbose log emits a count). Caller passes the same -I/-D args used
    // for the main header extraction so resolution sees the same view of the header.
    bool ExtractCHeaderMacros(const std::string& clangPath, const std::string& headerPath,
                              const std::vector<std::string>& extraDefines,
                              std::vector<CMacroEntry>& outMacros,
                              std::vector<CFunctionMacroEntry>& outFuncMacros)
    {
        const std::string target = (platformValue == 32)
            ? "--target=i686-pc-windows-msvc"
            : "--target=x86_64-pc-windows-msvc";

        const std::string hdrDir = std::filesystem::path(headerPath).parent_path().string();

        std::string fwdHeader = headerPath;
        std::replace(fwdHeader.begin(), fwdHeader.end(), '\\', '/');

        auto norm = [](std::string p) {
            std::replace(p.begin(), p.end(), '\\', '/');
            std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            return p;
        };
        std::vector<std::string> scopeRoots;
        scopeRoots.push_back(norm(hdrDir));
        for (const auto& inc : cIncludeDirs_) scopeRoots.push_back(norm(inc));
        auto inScope = [&](const std::string& file) -> bool {
            if (file.empty()) return false;
            std::string nf = norm(file);
            for (const auto& r : scopeRoots)
                if (!r.empty() && nf.rfind(r, 0) == 0) return true;
            return false;
        };

        // --- Pass A: -dD discovery ---
        struct MacroCand { std::string name; std::string file; int line; };
        std::vector<MacroCand> candidates;
        std::unordered_map<std::string, size_t> byName; // name -> index in candidates (dedupe; last #define wins, matching C)
        std::vector<CFunctionMacroEntry> funcCandidates;
        std::unordered_map<std::string, size_t> funcByName; // dedupe for function-like macros

        {
            llvm::SmallString<256> stubFile;
            if (llvm::sys::fs::createTemporaryFile("cflat_mhdr", "c", stubFile)) return false;
            const std::string stubPath = stubFile.str().str();
            struct StubRemover { std::string p; ~StubRemover() { llvm::sys::fs::remove(p); } } stubRemover{ stubPath };
            {
                std::error_code ec;
                llvm::raw_fd_ostream os(stubPath, ec, llvm::sys::fs::OF_Text);
                if (ec) return false;
                os << "#include \"" << fwdHeader << "\"\n";
                os.close();
            }

            // clang-cl silently drops the bare driver flag `-dD`; passing it through to cc1
            // via -Xclang is the only form that actually preserves #define directives.
            std::vector<std::string> argStrs = {
                clangPath, "/nologo", target, "-E", "-Xclang", "-dD"
            };
            argStrs.push_back("-I" + hdrDir);
            for (const auto& inc : cIncludeDirs_) argStrs.push_back("-I" + inc);
            for (const auto& def : cDefines_)     argStrs.push_back("/D" + def);
            for (const auto& def : extraDefines)  argStrs.push_back("/D" + def);
            argStrs.push_back(stubPath);

            if (verbose)
                std::cout << "[verbose] discovering C macros: " << headerPath << " (clang -E -dD)\n";

            std::string preText;
            if (!SpawnClangCapture(argStrs, preText)) return false;

            std::string curFile;
            int curLine = 0;
            std::string line;
            line.reserve(256);
            for (size_t i = 0; i <= preText.size(); ++i)
            {
                char ch = (i < preText.size()) ? preText[i] : '\n';
                if (ch != '\n') { line.push_back(ch); continue; }

                if (!line.empty() && line[0] == '#')
                {
                    // Line marker: `# <num> "<file>" [flags]`. Parse the number and the
                    // double-quoted file name; ignore everything else.
                    size_t p = 1;
                    while (p < line.size() && line[p] == ' ') ++p;
                    if (p < line.size() && std::isdigit((unsigned char)line[p]))
                    {
                        int parsed = 0;
                        while (p < line.size() && std::isdigit((unsigned char)line[p]))
                        { parsed = parsed * 10 + (line[p] - '0'); ++p; }
                        while (p < line.size() && line[p] == ' ') ++p;
                        if (p < line.size() && line[p] == '"')
                        {
                            size_t q = ++p;
                            while (q < line.size() && line[q] != '"') ++q;
                            curFile = line.substr(p, q - p);
                            // Un-escape \\ -> \ (clang quotes Windows paths with escaped backslashes).
                            std::string unescaped;
                            unescaped.reserve(curFile.size());
                            for (size_t k = 0; k < curFile.size(); ++k)
                            {
                                if (curFile[k] == '\\' && k + 1 < curFile.size() && curFile[k + 1] == '\\')
                                { unescaped.push_back('\\'); ++k; }
                                else unescaped.push_back(curFile[k]);
                            }
                            curFile = std::move(unescaped);
                            curLine = parsed;
                        }
                    }
                    else if (line.rfind("#define ", 0) == 0)
                    {
                        // #define NAME[(args)] [replacement]
                        size_t p2 = 8; // past "#define "
                        while (p2 < line.size() && (line[p2] == ' ' || line[p2] == '\t')) ++p2;
                        size_t nameStart = p2;
                        while (p2 < line.size() &&
                               (std::isalnum((unsigned char)line[p2]) || line[p2] == '_'))
                            ++p2;
                        if (p2 > nameStart && p2 < line.size())
                        {
                            std::string name = line.substr(nameStart, p2 - nameStart);
                            // Function-like macro: name immediately followed by '(' (no space).
                            if (line[p2] == '(')
                            {
                                if (!inScope(curFile) || name.rfind("__", 0) == 0) { /* drop */ }
                                else
                                {
                                    // Parse comma-separated param list until matching ')'. Variadic
                                    // (`...`) or empty-name params disqualify the macro.
                                    ++p2; // past '('
                                    std::vector<std::string> params;
                                    bool ok = true;
                                    while (p2 < line.size() && line[p2] != ')')
                                    {
                                        while (p2 < line.size() && (line[p2] == ' ' || line[p2] == '\t')) ++p2;
                                        size_t s = p2;
                                        while (p2 < line.size() &&
                                               (std::isalnum((unsigned char)line[p2]) || line[p2] == '_'))
                                            ++p2;
                                        if (p2 == s) { ok = false; break; } // empty/variadic
                                        params.push_back(line.substr(s, p2 - s));
                                        while (p2 < line.size() && (line[p2] == ' ' || line[p2] == '\t')) ++p2;
                                        if (p2 < line.size() && line[p2] == ',') ++p2;
                                        else break;
                                    }
                                    if (ok && p2 < line.size() && line[p2] == ')')
                                    {
                                        ++p2;
                                        while (p2 < line.size() && (line[p2] == ' ' || line[p2] == '\t')) ++p2;
                                        std::string body = (p2 < line.size()) ? line.substr(p2) : "";
                                        // Body-less macros (no replacement text) are useless to translate.
                                        if (!body.empty())
                                        {
                                            auto it = funcByName.find(name);
                                            if (it == funcByName.end())
                                            {
                                                funcByName[name] = funcCandidates.size();
                                                funcCandidates.push_back({ name, std::move(params), std::move(body), curFile, curLine, 0 });
                                            }
                                            else
                                            {
                                                // Redefined - keep latest definition.
                                                funcCandidates[it->second].params = std::move(params);
                                                funcCandidates[it->second].body   = std::move(body);
                                                funcCandidates[it->second].file   = curFile;
                                                funcCandidates[it->second].line   = curLine;
                                            }
                                        }
                                    }
                                }
                            }
                            else
                            {
                                while (p2 < line.size() && (line[p2] == ' ' || line[p2] == '\t')) ++p2;
                                if (p2 < line.size() && inScope(curFile) && name.rfind("__", 0) != 0)
                                {
                                    auto it = byName.find(name);
                                    if (it == byName.end())
                                    {
                                        byName[name] = candidates.size();
                                        candidates.push_back({ name, curFile, curLine });
                                    }
                                    else
                                    {
                                        // Redefined - keep latest location.
                                        candidates[it->second].file = curFile;
                                        candidates[it->second].line = curLine;
                                    }
                                }
                            }
                        }
                    }
                    // Other directives (#pragma, #undef, etc.) ignored.
                }
                else
                {
                    // Plain source line - advances the line counter within curFile.
                    ++curLine;
                }
                line.clear();
            }
        }

        // Function-like macros need no AST resolution; emit them now so an early return
        // from the object-like Pass B does not drop them.
        outFuncMacros = funcCandidates;

        if (candidates.empty()) return true;

        // --- Pass B: AST resolution. One anonymous enum per macro so per-decl errors don't
        //     cascade. Bad apples (non-constant, string, compound) are dropped silently. ---
        llvm::SmallString<256> resolveFile;
        if (llvm::sys::fs::createTemporaryFile("cflat_mres", "c", resolveFile)) return false;
        const std::string resolvePath = resolveFile.str().str();
        struct ResolveRemover { std::string p; ~ResolveRemover() { llvm::sys::fs::remove(p); } } resolveRemover{ resolvePath };
        {
            std::error_code ec;
            llvm::raw_fd_ostream os(resolvePath, ec, llvm::sys::fs::OF_Text);
            if (ec) return false;
            os << "#include \"" << fwdHeader << "\"\n";
            for (const auto& m : candidates)
            {
                // Value probe: folds the macro to a (long long) integer constant for value
                // extraction. Type probe: declares a global of the macro's natural type via
                // __typeof__ so the AST dump reports the unfolded type. The type-probe line
                // can fail in isolation (e.g. when the macro is not an expression) without
                // affecting the value-probe enum thanks to -ferror-limit=0 error recovery.
                os << "enum { __cflat_m_" << m.name << " = (long long)(" << m.name << ") };\n";
                os << "__typeof__(" << m.name << ") __cflat_t_" << m.name << ";\n";
            }
            os.close();
        }

        std::vector<std::string> argStrs = {
            clangPath, "/nologo", target, "-Xclang", "-ast-dump=json", "-fsyntax-only",
            "-ferror-limit=0", "-Wno-everything"
        };
        argStrs.push_back("-I" + hdrDir);
        for (const auto& inc : cIncludeDirs_) argStrs.push_back("-I" + inc);
        for (const auto& def : cDefines_)     argStrs.push_back("/D" + def);
        for (const auto& def : extraDefines)  argStrs.push_back("/D" + def);
        argStrs.push_back(resolvePath);

        std::string jsonText;
        if (!SpawnClangCapture(argStrs, jsonText)) return false;
        if (jsonText.empty()) return false;

        llvm::Expected<llvm::json::Value> root = llvm::json::parse(jsonText);
        if (!root) { llvm::consumeError(root.takeError()); return false; }
        const llvm::json::Object* tu = root->getAsObject();
        if (!tu) return false;
        const llvm::json::Array* topLevel = tu->getArray("inner");
        if (!topLevel) return true;

        // Walk the AST once and gather both pieces per macro:
        //   __cflat_m_NAME (EnumConstantDecl) -> folded long long value
        //   __cflat_t_NAME (VarDecl)          -> recovered qualType (HANDLE, void *, ...)
        // EnumConstantDecls are nested inside EnumDecls; VarDecls live at top level.
        // Both can fail independently (per-decl recovery via -ferror-limit=0).
        const std::string mPrefix = "__cflat_m_";
        const std::string tPrefix = "__cflat_t_";
        std::unordered_map<std::string, long long>   resolvedVal;
        std::unordered_map<std::string, std::string> resolvedType;
        std::function<void(const llvm::json::Value&)> visit = [&](const llvm::json::Value& v) {
            const llvm::json::Object* node = v.getAsObject();
            if (!node) return;
            if (auto kind = node->getString("kind"))
            {
                if (*kind == "EnumConstantDecl")
                {
                    if (auto nm = node->getString("name"))
                    {
                        std::string n = nm->str();
                        if (n.rfind(mPrefix, 0) == 0)
                        {
                            long long val = 0;
                            if (FindConstantValue(node, val))
                                resolvedVal[n.substr(mPrefix.size())] = val;
                        }
                    }
                }
                else if (*kind == "VarDecl")
                {
                    if (auto nm = node->getString("name"))
                    {
                        std::string n = nm->str();
                        if (n.rfind(tPrefix, 0) == 0)
                        {
                            if (auto* typeObj = node->getObject("type"))
                            {
                                std::string t;
                                if (auto d = typeObj->getString("desugaredQualType")) t = d->str();
                                else if (auto q = typeObj->getString("qualType"))     t = q->str();
                                if (!t.empty())
                                    resolvedType[n.substr(tPrefix.size())] = std::move(t);
                            }
                        }
                    }
                }
            }
            if (auto* inner = node->getArray("inner"))
                for (const auto& c : *inner) visit(c);
        };
        for (const auto& v : *topLevel) visit(v);

        outMacros.reserve(resolvedVal.size());
        for (const auto& cand : candidates)
        {
            auto it = resolvedVal.find(cand.name);
            if (it == resolvedVal.end()) continue; // dropped (non-int, non-constant, string, etc.)
            CMacroEntry e;
            e.name  = cand.name;
            e.value = it->second;
            e.file  = cand.file;
            e.line  = cand.line;
            e.col   = 0;
            // If the type probe also succeeded, ask the signature mapper whether the
            // recovered type chases (through cTypedefMap_) down to void*. Only that
            // shape qualifies as a pointer sentinel under the current scope.
            auto tt = resolvedType.find(cand.name);
            if (tt != resolvedType.end())
            {
                TypeAndValue tv;
                if (MapCTypeToTypeAndValue(tt->second, tv)
                    && tv.Pointer && !tv.ElemPointer && tv.TypeName == "void")
                {
                    e.isPointer = true;
                }
            }
            outMacros.push_back(std::move(e));
        }

        if (verbose)
            std::cout << "[verbose]   resolved " << outMacros.size() << " of "
                      << candidates.size() << " object-like macro candidate(s) from " << headerPath << "\n";
        return true;
    }

    // Spawn clang's JSON AST dump for a C *header* (via a stub TU that #includes it) and
    // parse out the externally-linkable function *declarations* and enum constants the
    // header itself contributes. A header pulls in the whole SDK/CRT transitively, so a
    // source-location filter keeps only decls whose file lives under the header's own dir
    // or one of the --c-include roots. Fills outSigs/outEnums; false on clang/parse error.
    bool RunClangHeaderExtract(const std::string& clangPath, const std::string& headerPath,
                               std::vector<CSigEntry>& outSigs, std::vector<CEnumEntry>& outEnums,
                               std::vector<CRecordEntry>& outRecords,
                               const std::vector<std::string>& extraDefines = {})
    {
        const std::string target = (platformValue == 32)
            ? "--target=i686-pc-windows-msvc"
            : "--target=x86_64-pc-windows-msvc";

        const std::string hdrDir = std::filesystem::path(headerPath).parent_path().string();

        // Stub TU: #include the header by absolute path (forward slashes for the lexer).
        llvm::SmallString<256> stubFile;
        if (llvm::sys::fs::createTemporaryFile("cflat_hdr", "c", stubFile)) return false;
        const std::string stubPath = stubFile.str().str();
        struct StubRemover { std::string p; ~StubRemover() { llvm::sys::fs::remove(p); } } stubRemover{ stubPath };
        {
            std::error_code ec;
            llvm::raw_fd_ostream os(stubPath, ec, llvm::sys::fs::OF_Text);
            if (ec) return false;
            std::string fwd = headerPath;
            std::replace(fwd.begin(), fwd.end(), '\\', '/');
            os << "#include \"" << fwd << "\"\n";
            os.close();
        }

        std::vector<std::string> argStrs = {
            clangPath, "/nologo", target, "-Xclang", "-ast-dump=json", "-fsyntax-only"
        };
        argStrs.push_back("-I" + hdrDir);
        for (const auto& inc : cIncludeDirs_) argStrs.push_back("-I" + inc);
        for (const auto& def : cDefines_) argStrs.push_back("/D" + def);
        // Per-import `define` clauses, appended after the process-wide --c-define.
        for (const auto& def : extraDefines) argStrs.push_back("/D" + def);
        argStrs.push_back(stubPath);

        if (verbose)
            std::cout << "[verbose] extracting C header: " << headerPath << " (clang AST dump)\n";

        std::string jsonText;
        if (!SpawnClangJson(argStrs, jsonText))
            return false;

        llvm::Expected<llvm::json::Value> root = llvm::json::parse(jsonText);
        if (!root) { llvm::consumeError(root.takeError()); return false; }
        const llvm::json::Object* tu = root->getAsObject();
        if (!tu) return false;
        const llvm::json::Array* topLevel = tu->getArray("inner");
        if (!topLevel) return true;

        // Collect typedefs first so the signature mapper can chase pointer aliases
        // (HANDLE -> void *, SOCKET -> uintptr_t, etc.) during the function walk.
        CollectCTypedefs(topLevel);

        // Normalize a path for prefix comparison: forward slashes, lowercase (Windows).
        auto norm = [](std::string p) {
            std::replace(p.begin(), p.end(), '\\', '/');
            std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            return p;
        };
        std::vector<std::string> scopeRoots;
        scopeRoots.push_back(norm(hdrDir));
        for (const auto& inc : cIncludeDirs_) scopeRoots.push_back(norm(inc));

        auto qualTypeOf = [](const llvm::json::Object* typeObj) -> std::string {
            if (!typeObj) return "";
            if (auto d = typeObj->getString("desugaredQualType")) return d->str();
            if (auto q = typeObj->getString("qualType")) return q->str();
            return "";
        };

        // clang emits loc.file only when it changes between sibling nodes; a node with no
        // file inherits the previous node's. Track it so the scope filter is accurate.
        std::string lastFile;
        auto fileOfNode = [&](const llvm::json::Object* node) -> std::string {
            auto pick = [&](const llvm::json::Object* o) {
                if (o)
                    if (auto f = o->getString("file")) lastFile = f->str();
            };
            if (auto* loc = node->getObject("loc"))
            {
                pick(loc);
                if (auto* exp = loc->getObject("expansionLoc")) pick(exp);
                if (auto* spel = loc->getObject("spellingLoc")) pick(spel);
            }
            if (auto* range = node->getObject("range"))
                if (auto* begin = range->getObject("begin"))
                    pick(begin);
            return lastFile;
        };
        auto inScope = [&](const std::string& file) -> bool {
            if (file.empty()) return false;
            std::string nf = norm(file);
            for (const auto& r : scopeRoots)
                if (!r.empty() && nf.rfind(r, 0) == 0) return true;
            return false;
        };

        // Pre-pass: collect record (struct/union) decls so MapCTypeToTypeAndValue can resolve
        // struct-by-value parameter/return types when the function-decl pass runs below. Uses
        // its own sticky lastFile so we don't perturb the main loop's tracker.
        {
            std::string preLastFile;
            auto preFileOfNode = [&](const llvm::json::Object* node) -> std::string {
                auto pick = [&](const llvm::json::Object* o) {
                    if (o)
                        if (auto f = o->getString("file")) preLastFile = f->str();
                };
                if (auto* loc = node->getObject("loc"))
                {
                    pick(loc);
                    if (auto* exp = loc->getObject("expansionLoc")) pick(exp);
                    if (auto* spel = loc->getObject("spellingLoc")) pick(spel);
                }
                if (auto* range = node->getObject("range"))
                    if (auto* begin = range->getObject("begin"))
                        pick(begin);
                return preLastFile;
            };
            for (const llvm::json::Value& v : *topLevel)
            {
                const llvm::json::Object* node = v.getAsObject();
                if (!node) continue;
                const std::string curFile = preFileOfNode(node);
                auto kind = node->getString("kind");
                if (!kind || *kind != "RecordDecl") continue;
                if (!inScope(curFile)) continue;
                auto nm = node->getString("name");
                if (!nm || nm->empty()) continue;
                auto cd = node->getBoolean("completeDefinition");
                if (!cd || !*cd) continue;
                auto tag = node->getString("tagUsed");
                CRecordEntry rec;
                rec.name = nm->str();
                rec.isUnion = tag && *tag == "union";
                if (auto* inner = node->getArray("inner"))
                {
                    for (const auto& c : *inner)
                    {
                        const llvm::json::Object* co = c.getAsObject();
                        if (!co) continue;
                        auto ck = co->getString("kind");
                        if (!ck || *ck != "FieldDecl") continue;
                        auto fname = co->getString("name");
                        if (!fname) continue;
                        CRecordFieldEntry fe;
                        fe.name = fname->str();
                        fe.ctype = qualTypeOf(co->getObject("type"));
                        rec.fields.push_back(std::move(fe));
                    }
                }
                if (auto* loc = node->getObject("loc"))
                {
                    if (auto l = loc->getInteger("line")) rec.line = (int)*l;
                    if (auto cc = loc->getInteger("col")) rec.col = (int)*cc - 1;
                }
                outRecords.push_back(std::move(rec));
            }
            RegisterCRecords(outRecords, headerPath);
        }

        for (const llvm::json::Value& v : *topLevel)
        {
            const llvm::json::Object* node = v.getAsObject();
            if (!node) continue;
            const std::string curFile = fileOfNode(node); // updates lastFile (sticky)
            auto kind = node->getString("kind");
            if (!kind) continue;
            if (!inScope(curFile)) continue; // drop SDK / CRT / builtin decls

            if (*kind == "FunctionDecl")
            {
                if (auto sc = node->getString("storageClass"); sc && *sc == "static") continue;
                auto nm = node->getString("name");
                if (!nm || nm->empty()) continue;

                CSigEntry entry;
                entry.name = nm->str();

                std::string funcQual = qualTypeOf(node->getObject("type"));
                entry.variadic = funcQual.find("...") != std::string::npos;

                std::string retStr = funcQual;
                if (auto paren = funcQual.find('('); paren != std::string::npos)
                    retStr = funcQual.substr(0, paren);
                if (!MapCTypeToTypeAndValue(retStr, entry.ret))
                {
                    if (verbose) std::cout << "[verbose]   skipping '" << entry.name
                                           << "': unsupported return type '" << retStr << "'\n";
                    continue;
                }

                bool paramsOk = true;
                if (auto* fnInner = node->getArray("inner"))
                {
                    for (const auto& c : *fnInner)
                    {
                        const llvm::json::Object* co = c.getAsObject();
                        if (!co) continue;
                        auto ck = co->getString("kind");
                        if (!ck || *ck != "ParmVarDecl") continue;

                        TypeAndValue ptv;
                        std::string pQual = qualTypeOf(co->getObject("type"));
                        if (!MapCTypeToTypeAndValue(pQual, ptv))
                        {
                            if (verbose) std::cout << "[verbose]   skipping '" << entry.name
                                                   << "': unsupported parameter type '" << pQual << "'\n";
                            paramsOk = false;
                            break;
                        }
                        if (auto pn = co->getString("name")) ptv.VariableName = pn->str();
                        entry.params.push_back(ptv);
                    }
                }
                if (!paramsOk) continue;

                if (auto* loc = node->getObject("loc"))
                {
                    if (auto l = loc->getInteger("line")) entry.line = (int)*l;
                    if (auto cc = loc->getInteger("col")) entry.col = (int)*cc - 1;
                }
                outSigs.push_back(std::move(entry));
            }
            else if (*kind == "EnumDecl")
            {
                long long current = 0;
                if (auto* enumInner = node->getArray("inner"))
                {
                    for (const auto& c : *enumInner)
                    {
                        const llvm::json::Object* co = c.getAsObject();
                        if (!co) continue;
                        auto ck = co->getString("kind");
                        if (!ck || *ck != "EnumConstantDecl") continue;
                        auto en = co->getString("name");
                        if (!en || en->empty()) continue;

                        long long value = current;
                        if (auto* eci = co->getArray("inner"))
                            for (const auto& e : *eci)
                            {
                                long long parsed = 0;
                                if (FindConstantValue(e.getAsObject(), parsed)) { value = parsed; break; }
                            }

                        CEnumEntry ee;
                        ee.name  = en->str();
                        ee.value = value;
                        if (auto* loc = co->getObject("loc"))
                        {
                            if (auto l = loc->getInteger("line")) ee.line = (int)*l;
                            if (auto cc = loc->getInteger("col")) ee.col = (int)*cc - 1;
                        }
                        outEnums.push_back(std::move(ee));
                        current = value + 1;
                    }
                }
            }
        }
        return true;
    }

    // Bind a prebuilt C library by extracting its header's declarations + enum constants.
    // No object is produced (unlike CompileCFile) - the library is linked via --c-lib in
    // EmitExecutable. Results are cached per header+include-dir set, mirroring
    // ExtractCSignatures. Runs in LSP mode too (registration only), so headers contribute
    // hover / completion / go-to-definition.
    bool CompileCHeader(const std::string& headerPath, const std::vector<std::string>& extraDefines = {})
    {
        llvm::SmallString<256> realPath;
        std::string fileForLsp = headerPath;
        if (!llvm::sys::fs::real_path(headerPath, realPath))
            fileForLsp = realPath.str().str();

        // Fold the include-dir set and defines into the cache key: the same header under
        // different --c-include roots or --c-define values can expose different decls.
        // Per-import `define` clauses are folded too, so two imports of the same header
        // with different inline defines do not collide on a stale cache entry.
        std::string cacheKey = fileForLsp;
        for (const auto& inc : cIncludeDirs_)  cacheKey += "|I" + inc;
        for (const auto& def : cDefines_)      cacheKey += "|D" + def;
        for (const auto& def : extraDefines)   cacheKey += "|d" + def;

        uint64_t currentHash = 0;
        bool haveHash = false;
        auto hashNow = [&]() -> uint64_t {
            if (!haveHash) { HashFileContents(fileForLsp, currentHash); haveHash = true; }
            return currentHash;
        };

        std::error_code mtEc;
        auto currentMtime = std::filesystem::last_write_time(fileForLsp, mtEc);

        std::vector<CSigEntry> hitSigs;
        std::vector<CEnumEntry> hitEnums;
        std::vector<CRecordEntry> hitRecords;
        std::vector<CMacroEntry> hitMacros;
        std::vector<CFunctionMacroEntry> hitFuncMacros;
        bool hit = false;
        {
            std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
            auto cacheIt = cFileSigCache_.find(cacheKey);
            if (!mtEc && cacheIt != cFileSigCache_.end())
            {
                CFileSigCacheEntry& entry = cacheIt->second;
                if (entry.mtime == currentMtime)
                {
                    if (verbose) std::cout << "[verbose] C header cache hit (mtime) for " << fileForLsp << "\n";
                    hitSigs = entry.sigs; hitEnums = entry.enums; hitRecords = entry.records;
                    hitMacros = entry.macros; hitFuncMacros = entry.funcMacros; hit = true;
                }
                else if (hashNow() == entry.hash)
                {
                    if (verbose) std::cout << "[verbose] C header cache hit (hash) for " << fileForLsp << "\n";
                    entry.mtime = currentMtime;
                    hitSigs = entry.sigs; hitEnums = entry.enums; hitRecords = entry.records;
                    hitMacros = entry.macros; hitFuncMacros = entry.funcMacros; hit = true;
                }
            }
        }
        if (hit)
        {
            // Records before sigs so struct-by-value signatures resolve to the same types.
            RegisterCRecords(hitRecords, fileForLsp);
            RegisterCSignatures(hitSigs, fileForLsp);
            RegisterCEnums(hitEnums, fileForLsp);
            RegisterCMacros(hitMacros);
            RegisterCFunctionMacros(hitFuncMacros, fileForLsp);
            return true;
        }

        const std::string clangPath = FindClangCl();
        if (clangPath.empty())
        {
            LogError(std::format("clang-cl.exe not found - cannot bind C header '{}'.", headerPath));
            return false;
        }

        std::vector<CSigEntry> sigs;
        std::vector<CEnumEntry> enums;
        std::vector<CRecordEntry> records;
        std::vector<CMacroEntry> macros;
        std::vector<CFunctionMacroEntry> funcMacros;
        if (!RunClangHeaderExtract(clangPath, headerPath, sigs, enums, records, extraDefines))
            return false;
        // Macro discovery is best-effort: failure here (clang missing a header path, OOM,
        // etc.) should not invalidate the function / enum / record bindings we already got.
        (void)ExtractCHeaderMacros(clangPath, headerPath, extraDefines, macros, funcMacros);

        if (!mtEc)
        {
            CFileSigCacheEntry entry;
            entry.mtime = currentMtime;
            entry.hash  = hashNow();
            entry.sigs  = sigs;
            entry.enums = enums;
            entry.records = records;
            entry.macros = macros;
            entry.funcMacros = funcMacros;
            std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
            cFileSigCache_[cacheKey] = std::move(entry);
        }

        // Records were already registered inside RunClangHeaderExtract.
        RegisterCSignatures(sigs, fileForLsp);
        RegisterCEnums(enums, fileForLsp);
        RegisterCMacros(macros);
        RegisterCFunctionMacros(funcMacros, fileForLsp);
        return true;
    }

    bool EmitExecutable(const std::string& exePath, const std::string& platform, bool debugInfo = false)
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

        // ---- Find lld-link (prefer the copy next to clang-cl, i.e. next to cflat.exe) ----
        std::string lldLinkPath;
        {
            std::string clangPath = FindClangCl();
            if (!clangPath.empty())
            {
                llvm::SmallString<256> candidate(llvm::sys::path::parent_path(clangPath));
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
                if (!llvm::sys::fs::createTemporaryFile("cflat_vswhere", "txt", outFD, outFile))
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
        if (debugInfo)
        {
            // /DEBUG embeds CodeView from the object into a PDB next to the EXE.
            // Without this, the DI metadata we emitted is dropped at link time.
            linkArgStrs.push_back("/DEBUG");
            // Derive <exe>.pdb from the exe path by swapping the extension.
            std::string pdbPath = exePath;
            auto dot = pdbPath.find_last_of('.');
            auto sep = pdbPath.find_last_of("/\\");
            if (dot != std::string::npos && (sep == std::string::npos || dot > sep))
                pdbPath.replace(dot, std::string::npos, ".pdb");
            else
                pdbPath += ".pdb";
            linkArgStrs.push_back("/PDB:" + pdbPath);
        }
        if (!msvcLibPath.empty()) linkArgStrs.push_back("/libpath:" + msvcLibPath);
        if (!ucrtLibPath.empty()) linkArgStrs.push_back("/libpath:" + ucrtLibPath);
        if (!umLibPath.empty())   linkArgStrs.push_back("/libpath:" + umLibPath);
        linkArgStrs.push_back("msvcrt.lib");
        linkArgStrs.push_back("ucrt.lib");
        linkArgStrs.push_back("vcruntime.lib");
        linkArgStrs.push_back("kernel32.lib");
        linkArgStrs.push_back("ws2_32.lib");
        linkArgStrs.push_back(objPath);
        // Merge any C objects compiled by clang-cl from .c inputs.
        for (auto& cObj : cObjectFiles_) linkArgStrs.push_back(cObj);
        // Prebuilt C import libraries (--c-lib): add each lib's directory as a search
        // path, then the lib itself. lld-link accepts an explicit path too, but the
        // /libpath + name form keeps behaviour uniform with the system libs above.
        for (const auto& lib : cLinkLibs_)
        {
            auto libDir = std::filesystem::path(lib).parent_path().string();
            if (!libDir.empty()) linkArgStrs.push_back("/libpath:" + libDir);
            linkArgStrs.push_back(std::filesystem::path(lib).filename().string());
        }

        std::vector<llvm::StringRef> linkArgs;
        for (auto& s : linkArgStrs) linkArgs.push_back(s);

        std::cout << "Linking (" << arch << "): " << exePath << "\n";

        std::string linkErr;
        int rc = llvm::sys::ExecuteAndWait(lldLinkPath, linkArgs, std::nullopt, {}, 0, 0, &linkErr);
        llvm::sys::fs::remove(objPath);
        for (auto& cObj : cObjectFiles_) llvm::sys::fs::remove(cObj);

        if (rc != 0)
        {
            std::cerr << "Error: linking failed (exit " << rc << "): " << linkErr << "\n";
            return false;
        }

        // Self-contained run: copy each --c-lib's sibling runtime DLL next to the exe so
        // the program launches without the user staging DLLs. Conan puts the DLL either
        // beside the import lib or in a ../bin sibling; check both. Best-effort - a static
        // lib has no DLL, which is fine.
        CopyCRuntimeDlls(exePath);
        return true;
    }

    // For each --c-lib, find a matching runtime DLL (lib dir, or a ../bin sibling) and copy
    // it next to the output exe. Tries <stem>.dll and the common lib<stem> / <stem>_imp
    // import-lib naming so the right DLL is found for either layout.
    void CopyCRuntimeDlls(const std::string& exePath)
    {
        namespace fs = std::filesystem;
        auto exeDir = fs::path(exePath).parent_path();
        // The legacy probe path (below) and the vcpkg authoritative list often resolve to
        // the same DLL when --c-lib is set by the vcpkg resolver. Track dest filenames so
        // we don't copy + log the same file twice.
        std::unordered_set<std::string> copiedDestNames;
        for (const auto& lib : cLinkLibs_)
        {
            fs::path libPath(lib);
            std::string stem = libPath.stem().string(); // e.g. "libcurl" or "libcurl_imp"
            // Strip a trailing "_imp" (MSVC import-lib convention) to recover the DLL stem.
            if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, "_imp") == 0)
                stem = stem.substr(0, stem.size() - 4);

            std::vector<fs::path> dirs = { libPath.parent_path() };
            auto binSibling = libPath.parent_path().parent_path() / "bin";
            dirs.push_back(binSibling);

            std::vector<std::string> stems = { stem };
            if (stem.rfind("lib", 0) == 0) stems.push_back(stem.substr(3)); // libcurl -> curl

            bool copied = false;
            for (const auto& dir : dirs)
            {
                for (const auto& s : stems)
                {
                    fs::path dll = dir / (s + ".dll");
                    std::error_code ec;
                    if (fs::exists(dll, ec))
                    {
                        fs::path dest = exeDir / dll.filename();
                        fs::copy_file(dll, dest, fs::copy_options::overwrite_existing, ec);
                        if (!ec)
                        {
                            copiedDestNames.insert(dest.filename().string());
                            if (verbose) std::cout << "[verbose]   copied runtime DLL: " << dll.string()
                                                   << " -> " << dest.string() << "\n";
                            copied = true;
                        }
                        break;
                    }
                }
                if (copied) break;
            }
            if (!copied && verbose)
                std::cout << "[verbose]   no runtime DLL found for " << lib << " (static lib?)\n";
        }

        // vcpkg-resolved DLLs: paths are authoritative (taken from the .list under
        // vcpkg_installed/<triplet>/bin) - no probing needed, just copy. Skip ones the
        // legacy probe above already copied.
        for (const auto& dll : vcpkgRuntimeDlls_)
        {
            fs::path src(dll);
            std::error_code ec;
            if (!fs::exists(src, ec)) continue;
            fs::path dest = exeDir / src.filename();
            if (copiedDestNames.count(dest.filename().string())) continue;
            fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
            if (!ec && verbose)
                std::cout << "[verbose]   copied vcpkg DLL: " << src.string() << " -> " << dest.string() << "\n";
        }
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
        // Target is *-pc-windows-msvc: emit CodeView so lld-link can produce a PDB.
        // DWARF would be ignored by VS / WinDbg / cppvsdbg on Windows.
        module->addModuleFlag(llvm::Module::Warning, "CodeView", 1);
        diFile = diBuilder->createFile(filename, directory);
        compileUnit = diBuilder->createCompileUnit(llvm::dwarf::DW_LANG_C99, diFile, "cflat", false, "", 0);
        // Seed cache so the primary translation unit's path resolves to the same
        // DIFile node that the compile unit references (avoids duplicates).
        if (!currentSourceFilePath_.empty())
            diFileCache_[currentSourceFilePath_] = diFile;
    }

    void FinalizeDebugInfo()
    {
        if (!diBuilder) return;
        // Emit DI for all globals now that struct types are fully laid out.
        for (auto& p : pendingGlobalDI_)
        {
            auto* diType = GetDIType(p.typeValue);
            auto* diGVE = diBuilder->createGlobalVariableExpression(
                compileUnit, p.typeValue.VariableName, p.gVar->getName(),
                p.file, p.line, diType,
                /*isLocalToUnit*/ false, /*isDefined*/ true);
            p.gVar->addDebugInfo(diGVE);
        }
        pendingGlobalDI_.clear();
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
    // frames back down to targetDepth (without running destructors - used in error-skip paths).
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
        llvm::DebugLoc debugLoc;
    };

    BuilderState SaveBuilderState() const
    {
        return { builder->saveIP(), currentFunction, currentSubprogram, builder->getCurrentDebugLocation() };
    }

    void RestoreBuilderState(const BuilderState& state)
    {
        builder->restoreIP(state.ip);
        currentFunction = state.function;
        currentSubprogram = state.subprogram;
        builder->SetCurrentDebugLocation(state.debugLoc);
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

    // {i8* fnptr, i8* envptr} - storage type for all in-function function<T> variables.
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
        // Programs exist in both dataStructures (empty shell) and programTable (real data).
        // Check programTable first so program-declared interfaces are found correctly.
        auto progIt = programTable.find(structName);
        if (progIt != programTable.end())
        {
            for (const auto& iface : progIt->second.Interfaces)
            {
                if (iface == ifaceName) return true;
                if (InterfaceInheritsFrom(iface, ifaceName)) return true;
            }
            return false;
        }
        auto structIt = dataStructures.find(structName);
        if (structIt != dataStructures.end())
        {
            for (const auto& iface : structIt->second.Interfaces)
            {
                if (iface == ifaceName) return true;
                if (InterfaceInheritsFrom(iface, ifaceName)) return true;
            }
        }
        return false;
    }

    llvm::GlobalVariable* GetOrCreateProgramVTable(ProgramData& pd, const std::string& structName, const std::string& ifaceName)
    {
        auto it = pd.VTables.find(ifaceName);
        if (it != pd.VTables.end()) return it->second;

        auto ifaceIt = interfaceTable.find(ifaceName);
        if (ifaceIt == interfaceTable.end())
        {
            LogError(std::format("GetOrCreateProgramVTable: unknown interface '{}'", ifaceName));
            return nullptr;
        }

        auto ptrTy = builder->getInt8Ty()->getPointerTo();
        std::vector<llvm::Constant*> entries;

        if (pd.typeDescriptor == nullptr)
        {
            pd.typeDescriptor = new llvm::GlobalVariable(
                *module, builder->getInt8Ty(), true,
                llvm::GlobalValue::InternalLinkage,
                builder->getInt8(0), structName + "_typedesc");
        }
        entries.push_back(pd.typeDescriptor);

        for (const auto& method : ifaceIt->second)
        {
            llvm::Function* fn = nullptr;
            auto funcIt = functionTable.find(method.Name);
            if (funcIt != functionTable.end())
            {
                size_t expectedParamCount = 1 + method.Parameters.size();
                for (const auto& sym : funcIt->second)
                {
                    if (sym.Parameters.size() != expectedParamCount) continue;
                    if (sym.Parameters[0].TypeName != structName || !sym.Parameters[0].Pointer) continue;
                    bool paramsMatch = true;
                    for (size_t pi = 0; pi < method.Parameters.size(); pi++)
                    {
                        if (sym.Parameters[1 + pi].TypeName != method.Parameters[pi].TypeName)
                        { paramsMatch = false; break; }
                    }
                    if (paramsMatch) { fn = sym.Function; break; }
                }
            }
            if (fn == nullptr)
            {
                LogError(std::format("GetOrCreateProgramVTable: '{}' does not implement '{}::{}'", structName, ifaceName, method.Name));
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
            *module, arrTy, true,
            llvm::GlobalValue::InternalLinkage,
            vtableConst, structName + "_" + ifaceName + "_vtable");

        pd.VTables[ifaceName] = vtableGlobal;
        return vtableGlobal;
    }

    llvm::GlobalVariable* GetOrCreateVTable(const std::string& structName, const std::string& ifaceName)
    {
        // Programs have their own VTable cache separate from dataStructures.
        auto progIt = programTable.find(structName);
        if (progIt != programTable.end())
            return GetOrCreateProgramVTable(progIt->second, structName, ifaceName);

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
                    auto tempAlloca = AllocaAtEntry(structTy, nullptr);
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

    /// Effective alignment of a declaration: max of `alignas` on the decl,
    /// `alignas` on the struct type (if any), and the ABI alignment.
    uint64_t GetEffectiveAlignment(const DeclTypeAndValue& decl, llvm::Type* type)
    {
        uint64_t a = 1;
        if (type && type->isSized())
            a = module->getDataLayout().getABITypeAlign(type).value();
        if (decl.UserAlignValue > a) a = decl.UserAlignValue;
        auto it = dataStructures.find(decl.TypeName);
        if (it != dataStructures.end() && it->second.UserRequestedAlignment > a)
            a = it->second.UserRequestedAlignment;
        return a;
    }

    /// Same as above but starting from a type name (no DeclTypeAndValue handy).
    uint64_t GetEffectiveAlignmentForType(const std::string& typeName, llvm::Type* type)
    {
        uint64_t a = 1;
        if (type && type->isSized())
            a = module->getDataLayout().getABITypeAlign(type).value();
        auto it = dataStructures.find(typeName);
        if (it != dataStructures.end() && it->second.UserRequestedAlignment > a)
            a = it->second.UserRequestedAlignment;
        return a;
    }

    /// C++-style padded size: roundUp(allocSize, effectiveAlign).
    /// Used by sizeof so that arrays of over-aligned types stride correctly.
    uint64_t GetEffectiveAllocSize(llvm::Type* type, uint64_t effAlign)
    {
        if (!type || !type->isSized()) return 0;
        uint64_t sz = module->getDataLayout().getTypeAllocSize(type);
        if (effAlign <= 1) return sz;
        return (sz + effAlign - 1) / effAlign * effAlign;
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
        if (sd.IsUnion)
        {
            LogError(std::format("reflect is not supported on union type '{}'", structName));
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

    llvm::GlobalVariable* CreateGlobalVariable(TypeAndValue typeValue, llvm::Constant* initValue, bool threadLocal = false, uint64_t userAlign = 0)
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
            // Zero-initialize - works for all types: primitives, pointers, structs, fat-ptrs.
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

        // Apply effective alignment (decl-level alignas, struct alignas, or ABI).
        uint64_t effAlign = GetEffectiveAlignmentForType(typeValue.TypeName, destinationType);
        if (userAlign > effAlign) effAlign = userAlign;
        uint64_t abiAlign = (destinationType && destinationType->isSized())
            ? module->getDataLayout().getABITypeAlign(destinationType).value() : 0;
        if (effAlign > abiAlign)
            gVar->setAlignment(llvm::Align(effAlign));

        globalNamedVariable[typeValue.VariableName] = gVar;
        globalVariableTypes[typeValue.VariableName] = typeValue;

        if (symbolSink_ && !typeValue.VariableName.empty())
            symbolSink_->RegisterVariable(typeValue.VariableName, typeValue.TypeName);

        if (diBuilder && diFile && !typeValue.VariableName.empty())
        {
            llvm::DIFile* gvFile = GetDIFileForCurrentSource();
            if (!gvFile) gvFile = diFile;
            pendingGlobalDI_.push_back({gVar, typeValue, gvFile, (unsigned)currentLine});
        }

        return gVar;
    }

    // Emit an alloca in the function entry block so that loop-body declarations
    // don't grow the stack unboundedly across iterations.  VLAs (non-null arraySize)
    // must stay at the current point because their size is computed dynamically.
    // When `align` is nonzero, the alloca is annotated with that alignment.
    llvm::AllocaInst* AllocaAtEntry(llvm::Type* type, llvm::Value* arraySize, const llvm::Twine& name = "", uint64_t align = 0)
    {
        llvm::AllocaInst* a;
        if (arraySize != nullptr)
        {
            a = builder->CreateAlloca(type, arraySize, name);
        }
        else
        {
            auto* currentBlock = builder->GetInsertBlock();
            auto* entryBlock = &currentBlock->getParent()->getEntryBlock();
            if (currentBlock == entryBlock)
            {
                a = builder->CreateAlloca(type, nullptr, name);
            }
            else
            {
                llvm::IRBuilder<> eb(entryBlock, entryBlock->begin());
                a = eb.CreateAlloca(type, nullptr, name);
            }
        }
        if (align != 0)
            a->setAlignment(llvm::Align(align));
        return a;
    }

    llvm::AllocaInst* CreateLocalVariable(TypeAndValue typeValue, llvm::Type* autoType = nullptr, llvm::Value* arraySize = nullptr, size_t line = 0, uint64_t userAlign = 0)
    {
        auto type = GetType(typeValue, autoType);
        // Effective alignment: max(decl-level alignas, struct-level alignas, ABI).
        uint64_t effAlign = GetEffectiveAlignmentForType(typeValue.TypeName, type);
        if (userAlign > effAlign) effAlign = userAlign;
        // Only annotate when above the natural ABI alignment - otherwise LLVM's
        // default is already correct and we avoid noisy IR.
        uint64_t abiAlign = (type && type->isSized())
            ? module->getDataLayout().getABITypeAlign(type).value() : 0;
        uint64_t allocaAlign = (effAlign > abiAlign) ? effAlign : 0;
        auto alloc = AllocaAtEntry(type, arraySize, typeValue.VariableName, allocaAlign);
        auto& namedVariable = stackNamedVariable.back().namedVariable[typeValue.VariableName];
        namedVariable.Storage = alloc;
        namedVariable.TypeAndValue = typeValue;
        namedVariable.BaseType = type;

        if (symbolSink_ && !typeValue.VariableName.empty())
            symbolSink_->RegisterVariable(typeValue.VariableName, typeValue.TypeName);

        if (diBuilder && currentSubprogram && (unsigned)line > 0)
        {
            auto diType = GetDIType(typeValue);
            llvm::DIFile* locFile = currentSubprogram->getFile();
            if (!locFile) locFile = diFile;
            auto diVar = diBuilder->createAutoVariable(currentSubprogram, typeValue.VariableName, locFile, (unsigned)line, diType);
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
        return AllocaAtEntry(type, nullptr);
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

    llvm::Value* CreateIncrement(llvm::Value* destination, int amount, llvm::Type* elemType = nullptr)
    {
        llvm::LoadInst* loadInst = CreateLoad(destination);

        if (loadInst->getType()->isPointerTy())
        {
            // Pointer increment/decrement: step by elemType-sized strides (C semantics).
            // Fall back to i8 (byte stride) only when element type is unknown.
            auto* stepTy = elemType ? elemType : builder->getInt8Ty();
            auto* step = llvm::ConstantInt::get(builder->getInt64Ty(), amount);
            auto* newPtr = builder->CreateGEP(stepTy, loadInst, step, "ptrinc");
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
            // not handled here - use a typed literal (0.0f) or an explicit cast at
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
            // Integer 0 assigned to a pointer field - produce a proper null/ptr constant.
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

        // Struct -> Pointer: invalid; caller should have emitted an error already.
        // Return the value unchanged rather than letting LLVM assert.
        if (srcType->isStructTy() && destType->isPointerTy())
        {
            LogError("cannot assign a struct value to a pointer variable - use getPtr() or take the address with '&'");
            return value;
        }

        // Fallback: BitCast for same-size reinterpretation
        return builder->CreateBitCast(value, destType);
    }

    llvm::Value* CreateCast(llvm::Instruction::CastOps op, llvm::Value* value, llvm::Type* destType)
    {
        return builder->CreateCast(op, value, destType);
    }

    // Create StructType or OpaqueStruct
    llvm::StructType* CreateStructType(std::string name, std::vector<LLVMBackend::DeclTypeAndValue> typeAndValues, uint64_t userAlign = 0)
    {
        if (typeAndValues.size() > 0)
        {
            std::vector<llvm::Type*> types;

            for (const auto& typeValue : typeAndValues)
            {
                types.emplace_back(GetType(typeValue));
            }

            // alignas(N) on the struct: append a trailing [padBytes x i8] member
            // so getTypeAllocSize matches the padded sizeof. Without this, arrays
            // of the struct stride at the natural size and elements lose alignment.
            // The padding is NOT added to StructFields metadata - user code never
            // sees the synthetic trailing field. Computed BEFORE any setBody call
            // so we only set the body once (LLVM asserts on resetting non-opaque
            // structs).
            if (userAlign > 1)
            {
                auto* tmp = llvm::StructType::get(*context, types);
                uint64_t natural = module->getDataLayout().getTypeAllocSize(tmp);
                uint64_t padded = (natural + userAlign - 1) / userAlign * userAlign;
                if (padded > natural)
                    types.push_back(llvm::ArrayType::get(builder->getInt8Ty(), padded - natural));
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

                if (userAlign > 1)
                    dataStructures[name].UserRequestedAlignment = userAlign;

                return myStruct;
            }

            // existing struct;
            auto& structData = mystuct->second;
            structData.StructFields = typeAndValues;
            if (structData.StructType->isOpaque())
                structData.StructType->setBody(types);
            if (userAlign > 1)
                structData.UserRequestedAlignment = userAlign;

            return structData.StructType;
        }
        else
        {
            auto existing = dataStructures.find(name);
            if (existing != dataStructures.end())
                return existing->second.StructType;

            llvm::StructType* opaqueStruct = llvm::StructType::create(*context, name);
            dataStructures[name].StructType = opaqueStruct;
            dataStructures[name].typeDescriptor = new llvm::GlobalVariable(
                *module, builder->getInt8Ty(), true,
                llvm::GlobalValue::InternalLinkage,
                builder->getInt8(0), name + "_typedesc");
            return opaqueStruct;
        }
    }

    // Creates a union type as a struct with a single [N x alignTy] body, where N and alignTy
    // are chosen to match the size and alignment of the largest/most-aligned member.
    // StructFields is preserved for metadata (field name lookup, type info).
    llvm::StructType* CreateUnionType(std::string name, std::vector<DeclTypeAndValue> typeAndValues)
    {
        uint64_t maxSize = 1;
        llvm::Align maxAlign(1);
        for (const auto& tv : typeAndValues)
        {
            auto* t = GetType(tv);
            uint64_t sz = module->getDataLayout().getTypeAllocSize(t);
            llvm::Align al = module->getDataLayout().getABITypeAlign(t);
            if (sz > maxSize) maxSize = sz;
            if (al > maxAlign) maxAlign = al;
        }

        // Pick an integer element type that satisfies maxAlign so the LLVM struct
        // inherits the correct ABI alignment (LLVM sets struct align = max(element aligns)).
        llvm::Type* alignTy;
        switch (maxAlign.value())
        {
            case 8:  alignTy = builder->getInt64Ty(); break;
            case 4:  alignTy = builder->getInt32Ty(); break;
            case 2:  alignTy = builder->getInt16Ty(); break;
            default: alignTy = builder->getInt8Ty();  break;
        }
        uint64_t elemSize = module->getDataLayout().getTypeAllocSize(alignTy);
        uint64_t numElems = (maxSize + elemSize - 1) / elemSize;
        auto* bodyTy = llvm::ArrayType::get(alignTy, numElems);

        auto it = dataStructures.find(name);
        llvm::StructType* unionTy;
        if (it != dataStructures.end() && it->second.StructType != nullptr)
        {
            unionTy = it->second.StructType;
            if (unionTy->isOpaque())
                unionTy->setBody({bodyTy});
        }
        else
        {
            unionTy = llvm::StructType::create(*context, {bodyTy}, name);
        }

        auto& sd = dataStructures[name];
        sd.StructType = unionTy;
        sd.StructFields = typeAndValues;
        sd.IsUnion = true;
        if (sd.typeDescriptor == nullptr)
        {
            sd.typeDescriptor = new llvm::GlobalVariable(
                *module, builder->getInt8Ty(), true,
                llvm::GlobalValue::InternalLinkage,
                builder->getInt8(0), name + "_typedesc");
        }
        return unionTy;
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
        // In LLVM 18 opaque-pointer mode the GlobalVariable* is already a ptr -
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
            fp.IsMove = p.IsMove;
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
        // String literals arrive as i8* - wrap them into %string{ptr,len} when the
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
            // Bare-semicolon form: expect_error("msg"); - if the expected error never fired before this scope exits, report failure.
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

    // True when tv refers to a C-compatible struct (or auto-registered C struct) passed by
    // value (Pointer=false). Excludes interface fat-pointers, function pointers, strings,
    // and any TypeName not present in dataStructures with a complete (non-opaque) body.
    bool IsByValueStructTV(const TypeAndValue& tv) const
    {
        if (tv.Pointer) return false;
        if (tv.IsInterface || tv.IsFunctionPointer) return false;
        if (tv.TypeName.empty()) return false;
        auto it = dataStructures.find(tv.TypeName);
        if (it == dataStructures.end()) return false;
        auto* st = it->second.StructType;
        return st != nullptr && !st->isOpaque();
    }

    // Win64 / Win32 ABI classification for a single param slot. Returns Direct for scalars
    // and pointers (the existing pipeline handles them). For struct-by-value:
    //   - Win64: size in {1,2,4,8} -> CoerceToInt(iN); else ByVal(pointer + byval attr).
    //   - Win32: always ByVal (cdecl pushes the whole struct on the stack).
    AbiSlot ClassifyAbiParam(const TypeAndValue& tv)
    {
        AbiSlot slot;
        if (!IsByValueStructTV(tv)) return slot; // Direct
        auto* st = dataStructures.at(tv.TypeName).StructType;
        const llvm::DataLayout& dl = module->getDataLayout();
        uint64_t size  = dl.getTypeAllocSize(st);
        uint64_t align = dl.getABITypeAlign(st).value();
        if (platformValue == 64 && (size == 1 || size == 2 || size == 4 || size == 8))
        {
            slot.kind     = AbiSlot::CoerceToInt;
            slot.coerceTy = llvm::Type::getIntNTy(*context, (unsigned)(size * 8));
            slot.structTy = st;
            slot.align    = align;
        }
        else
        {
            slot.kind     = AbiSlot::ByVal;
            slot.structTy = st;
            // On Win32 cdecl the parameter slot is 4-byte aligned (the stack itself is
            // S32-aligned), so byval must report align 4 even if the struct's natural
            // alignment is larger - mirroring clang. Otherwise the callee will use
            // aligned loads against an actually-misaligned stack slot.
            slot.align    = (platformValue == 32) ? std::min<uint64_t>(align, 4) : align;
        }
        return slot;
    }

    // Win64 / Win32 ABI classification for the return slot.
    //   - size in {1,2,4,8} -> CoerceToInt(iN) (returned in RAX / EDX:EAX as appropriate).
    //   - otherwise         -> SRetReturn: function returns void, caller passes hidden
    //                          pointer as arg 0 with the 'sret' attribute.
    AbiSlot ClassifyAbiReturn(const TypeAndValue& tv)
    {
        AbiSlot slot;
        if (!IsByValueStructTV(tv)) return slot; // Direct
        auto* st = dataStructures.at(tv.TypeName).StructType;
        const llvm::DataLayout& dl = module->getDataLayout();
        uint64_t size  = dl.getTypeAllocSize(st);
        uint64_t align = dl.getABITypeAlign(st).value();
        if (size == 1 || size == 2 || size == 4 || size == 8)
        {
            slot.kind     = AbiSlot::CoerceToInt;
            slot.coerceTy = llvm::Type::getIntNTy(*context, (unsigned)(size * 8));
            slot.structTy = st;
            slot.align    = align;
        }
        else
        {
            slot.kind     = AbiSlot::SRetReturn;
            slot.structTy = st;
            slot.align    = align;
        }
        return slot;
    }

    // Build the full lowering recipe for an extern C function signature. hasLowering is
    // set if at least one slot is non-Direct so the call site knows whether to take the
    // fast path (existing CreateFunctionCall) or the ABI-rewriting path.
    AbiRecipe ComputeAbiRecipe(const TypeAndValue& retType,
                               const std::vector<TypeAndValue>& params)
    {
        AbiRecipe recipe;
        recipe.retSlot = ClassifyAbiReturn(retType);
        recipe.paramSlots.reserve(params.size());
        for (const auto& p : params)
            recipe.paramSlots.push_back(ClassifyAbiParam(p));
        recipe.hasLowering = (recipe.retSlot.kind != AbiSlot::Direct);
        if (!recipe.hasLowering)
            for (const auto& s : recipe.paramSlots)
                if (s.kind != AbiSlot::Direct) { recipe.hasLowering = true; break; }
        return recipe;
    }

    // Build the LLVM FunctionType for an extern C function with the given recipe applied.
    // - SRetReturn ret: function returns void, prepend a ptr param for the hidden sret slot.
    // - CoerceToInt ret: function returns iN.
    // - Direct ret: GetCCompatibleType(retType).
    // - ByVal param: ptr (callee sees a pointer; LLVM x86/x64 backend lowers byval correctly).
    // - CoerceToInt param: iN.
    // - Direct param: GetCCompatibleType(p).
    llvm::FunctionType* BuildExternFunctionType(const TypeAndValue& retType,
                                                const std::vector<TypeAndValue>& params,
                                                bool varargs,
                                                const AbiRecipe& recipe)
    {
        std::vector<llvm::Type*> ptypes;
        ptypes.reserve(params.size() + (recipe.retSlot.kind == AbiSlot::SRetReturn ? 1 : 0));

        llvm::Type* loweredRet = nullptr;
        if (recipe.retSlot.kind == AbiSlot::SRetReturn)
        {
            loweredRet = builder->getVoidTy();
            // Hidden sret pointer goes first.
            ptypes.push_back(recipe.retSlot.structTy->getPointerTo());
        }
        else if (recipe.retSlot.kind == AbiSlot::CoerceToInt)
            loweredRet = recipe.retSlot.coerceTy;
        else
            loweredRet = GetCCompatibleType(retType);

        for (size_t i = 0; i < params.size(); ++i)
        {
            const AbiSlot& s = recipe.paramSlots[i];
            if (s.kind == AbiSlot::CoerceToInt)
                ptypes.push_back(s.coerceTy);
            else if (s.kind == AbiSlot::ByVal)
                ptypes.push_back(s.structTy->getPointerTo());
            else
                ptypes.push_back(GetCCompatibleType(params[i]));
        }
        return llvm::FunctionType::get(loweredRet, ptypes, varargs);
    }

    // Attach byval / sret / alignment attributes on the function declaration per the recipe.
    // These are LLVM-level hints required for correct ABI lowering (the x86/x64 backend
    // uses them to decide register vs stack placement, byval copies, and sret semantics).
    void ApplyAbiAttributes(llvm::Function* fn, const AbiRecipe& recipe)
    {
        unsigned attrIdx = 0; // LLVM param attribute indices are 0-based on the function's actual param list
        if (recipe.retSlot.kind == AbiSlot::SRetReturn)
        {
            fn->addParamAttr(attrIdx, llvm::Attribute::getWithStructRetType(*context, recipe.retSlot.structTy));
            fn->addParamAttr(attrIdx, llvm::Attribute::NoAlias);
            if (recipe.retSlot.align > 0)
                fn->addParamAttr(attrIdx, llvm::Attribute::getWithAlignment(*context, llvm::Align(recipe.retSlot.align)));
            ++attrIdx;
        }
        for (size_t i = 0; i < recipe.paramSlots.size(); ++i, ++attrIdx)
        {
            const AbiSlot& s = recipe.paramSlots[i];
            if (s.kind == AbiSlot::ByVal)
            {
                fn->addParamAttr(attrIdx, llvm::Attribute::getWithByValType(*context, s.structTy));
                if (s.align > 0)
                    fn->addParamAttr(attrIdx, llvm::Attribute::getWithAlignment(*context, llvm::Align(s.align)));
            }
        }
    }

    void CreateFunctionDeclaration(std::string functionName, LLVMBackend::TypeAndValue returnType, std::vector<LLVMBackend::TypeAndValue> arguments, bool external = false, bool varargs = false, bool returnsOwned = false, bool isMethod = false, bool isStdcall = false, bool isCdecl = false)
    {
        // For extern C declarations, compute an ABI recipe so struct-by-value params/returns
        // are lowered (coerce-to-int / byval / sret) per the Win64 or Win32 MSVC ABI. If the
        // recipe has no lowering (scalar/pointer only) the existing GetFunctionType path is used.
        AbiRecipe recipe;
        bool useRecipe = false;
        if (external)
        {
            recipe = ComputeAbiRecipe(returnType, arguments);
            useRecipe = recipe.hasLowering;
        }

        llvm::FunctionType* functionType = useRecipe
            ? BuildExternFunctionType(returnType, arguments, varargs, recipe)
            : GetFunctionType(returnType, arguments, varargs, external);
        std::string mangledName = external ? functionName : ComputeMangledName(functionName, returnType, arguments, varargs);

        if (module->getFunction(mangledName) != nullptr)
            return;

        auto funcCallee = module->getOrInsertFunction(mangledName, functionType);
        llvm::Value* calleeValue = funcCallee.getCallee();

        if (llvm::Function* fn = llvm::dyn_cast<llvm::Function>(calleeValue))
        {
            if (isStdcall && platformValue == 32)
                fn->setCallingConv(llvm::CallingConv::X86_StdCall);
            else if (isCdecl)
                fn->setCallingConv(llvm::CallingConv::C);

            if (useRecipe)
                ApplyAbiAttributes(fn, recipe);

            auto& symList = functionTable[functionName];
            FunctionSymbol funcSym = {
                .UniqueName = mangledName,
                .Function = fn,
                .ReturnType = returnType,
                .Variadic = fn->isVarArg(),
                .ReturnsOwned = returnsOwned,
                .IsMethod = isMethod,
                .Recipe = recipe,
            };

            for (const auto& arg : arguments)
            {
                funcSym.Parameters.push_back(arg);
            }

            symList.push_back(funcSym);
        }
    }

    // Return the FunctionSymbol whose LLVM function pointer matches fn, or nullptr.
    const FunctionSymbol* GetFunctionSymbol(llvm::Function* fn) const
    {
        for (const auto& [key, syms] : functionTable)
            for (const auto& sym : syms)
                if (sym.Function == fn)
                    return &sym;
        return nullptr;
    }

    // Set RequiredLocks on the most-recently registered overload of functionName.
    void SetFunctionRequiredLocks(const std::string& functionName, std::vector<std::string> locks)
    {
        auto it = functionTable.find(functionName);
        if (it != functionTable.end() && !it->second.empty())
            it->second.back().RequiredLocks = std::move(locks);
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
            if (argument.IsMove)
                argumentString += "M";
        }

        std::string uniqueName = std::format("_{}_{}_{}_", functionName, returnType.ToUniqueString(), argumentString);

        return uniqueName;
    }

    llvm::Function* CreateFunctionDefinition(std::string functionName, LLVMBackend::TypeAndValue returnType, std::vector<LLVMBackend::TypeAndValue> arguments, bool external = false, bool varargs = false, size_t line = 0, bool returnsOwned = false, bool isMethod = false, bool isStdcall = false, bool isCdecl = false, size_t scopeLine = 0)
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
            // Pre-declared by ForwardRefScanner - reuse the declaration and attach a body.
            alreadyDeclared = true;
        }
        else
        {
            fn = createFunctionProto(mangledName, functionType);
        }

        // CFlat treats null pointer dereferences as defined behavior (hardware fault → SEH).
        // Ensure the attribute is set even on pre-declared functions that skipped createFunctionProto.
        fn->addFnAttr(llvm::Attribute::NullPointerIsValid);

        if (isStdcall && platformValue == 32)
            fn->setCallingConv(llvm::CallingConv::X86_StdCall);
        else if (isCdecl)
            fn->setCallingConv(llvm::CallingConv::C);

        createFunctionBlock(fn, functionName, arguments, returnsOwned);

        if (diBuilder && diFile && line > 0)
        {
            // Reuse an existing subprogram if the function was previously processed
            // (e.g. constraint-instantiation re-entered this path). Creating a new
            // distinct DISubprogram and overwriting setSubprogram() leaves the prior
            // node orphaned, which the LLVM verifier rejects.
            auto sp = fn->getSubprogram();
            llvm::DIFile* fnFile = GetDIFileForCurrentSource();
            if (!fnFile) fnFile = diFile;
            if (!sp)
            {
                // DWARF / CodeView subroutine type: element 0 is the return type
                // (nullptr means void), followed by parameter types in order.
                std::vector<llvm::Metadata*> diTypes;
                diTypes.reserve(1 + arguments.size());
                if (returnType.TypeName == "void" && !returnType.Pointer)
                    diTypes.push_back(nullptr);
                else
                    diTypes.push_back(GetDIType(returnType));
                for (const auto& a : arguments)
                    diTypes.push_back(GetDIType(a));

                auto funcDIType = diBuilder->createSubroutineType(
                    diBuilder->getOrCreateTypeArray(diTypes));
                unsigned effectiveScopeLine = (unsigned)(scopeLine != 0 ? scopeLine : line);
                sp = diBuilder->createFunction(
                    fnFile, functionName, fn->getName(),
                    fnFile, (unsigned)line, funcDIType, effectiveScopeLine,
                    llvm::DINode::FlagPrototyped,
                    llvm::DISubprogram::SPFlagDefinition
                );
                fn->setSubprogram(sp);
            }
            currentSubprogram = sp;
            builder->SetCurrentDebugLocation(llvm::DILocation::get(*context, (unsigned)line, 0, sp));

            // Attach parameter DI to the allocas createFunctionBlock just emitted.
            // argNo is 1-based and follows declaration order.
            auto& frame = stackNamedVariable.back();
            unsigned argNo = 1;
            for (const auto& a : arguments)
            {
                auto it = frame.functionArgument.find(a.VariableName);
                if (it != frame.functionArgument.end() && it->second.Storage != nullptr)
                {
                    auto diParam = diBuilder->createParameterVariable(
                        sp, a.VariableName, argNo, fnFile,
                        (unsigned)line, GetDIType(a), /*alwaysPreserve*/ true);
                    diBuilder->insertDeclare(
                        it->second.Storage, diParam, diBuilder->createExpression(),
                        llvm::DILocation::get(*context, (unsigned)line, 0, sp),
                        builder->GetInsertBlock());
                }
                ++argNo;
            }
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

        // Apply pointer wrapping to get the element type before array wrapping.
        // This ensures char*[3] → [3 x ptr] not [3 x i8].
        if (allowPointer && typeAndValue.Pointer)
        {
            // Note: LLVM doesn't have void ptr, instead use i8 ptr.
            if (type->isVoidTy())
                type = builder->getInt8Ty()->getPointerTo();
            else
                type = type->getPointerTo();
            if (typeAndValue.ElemPointer)
                type = type->getPointerTo();
        }

        if (typeAndValue.ConstArraySize > 0)
        {
            // Build from innermost to outermost: T[N1][N2] → [N1 x [N2 x T]]
            llvm::Type* inner = type;
            for (int i = (int)typeAndValue.ConstInnerDimensions.size() - 1; i >= 0; i--)
                inner = llvm::ArrayType::get(inner, typeAndValue.ConstInnerDimensions[i]);
            return llvm::ArrayType::get(inner, typeAndValue.ConstArraySize);
        }

        return type;
    }

    std::pair<std::vector<NamedVariable>, FunctionSymbol> ComputeOverloadFunction(std::vector<std::pair<std::vector<NamedVariable>, FunctionSymbol>> candidates) const
    {
        std::pair<std::vector<NamedVariable>, FunctionSymbol> possibleResult;
        std::pair<std::vector<NamedVariable>, FunctionSymbol> bestPerfect;
        int bestPerfectScore = -1;  // moveScore is always >= 0; -1 means "no perfect match yet"
        // int score = 0; // 2 for promotionMatch, 1 for implicitMatch

        for (const auto& pair : candidates)
        {
            const auto& [arguments, candidate] = pair;

            if (candidate.Variadic)
            {
                // Variadic is a fallback: prefer any exact non-variadic match over it.
                possibleResult = pair;
                continue;
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
                    // Implicit char* → string coercion: string literal or char* passed to a string param.
                    if (result < 0 && candidateParamItr->TypeName == "string" && !candidateParamItr->Pointer
                        && arg.BaseType && arg.BaseType->isPointerTy())
                        result = 1;
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
            {
                // Tie-breaker between perfect-match overloads that differ only in 'move':
                // for each parameter, score +1 when the param's IsMove agrees with whether
                // the caller-side argument is an owning lvalue (so the borrow overload wins
                // for non-owning args, and the move overload wins for owning ones).
                // For function-pointer parameters, also score +1 when the named function arg
                // has an overload whose per-param IsMove flags match the candidate param's
                // FuncPtrParams exactly (so the move-typed start() wins for a move-typed fn).
                int moveScore = 0;
                auto pi = candidate.Parameters.begin();
                for (const auto& arg : arguments)
                {
                    if (pi == candidate.Parameters.end()) break;
                    bool argOwning = !arg.CallerName.empty() &&
                        (arg.IsOwning || arg.IsOwningString || arg.IsOwningStruct) &&
                        !arg.IsMoved;
                    if (pi->IsMove == argOwning)
                        moveScore++;
                    if (pi->IsFunctionPointer && !arg.CallerName.empty()
                        && HasFunctionWithMoveFlags(arg.CallerName, pi->FuncPtrParams))
                    {
                        moveScore += 2;  // weight funcptr-IsMove match heavily so it can break a tie
                    }
                    ++pi;
                }
                if (moveScore > bestPerfectScore)
                {
                    bestPerfectScore = moveScore;
                    bestPerfect = pair;
                }
                continue;
            }

            if (promotionMatch || implicitMatch)
            {
                possibleResult = pair;
            }
        }

        if (bestPerfectScore >= 0)
            return bestPerfect;

        return possibleResult;
    }

    std::vector<LLVMBackend::NamedVariable> MatchFunction(const std::vector<LLVMBackend::NamedVariable>& inputArguments, const std::vector<LLVMBackend::TypeAndValue>& targetArguments, bool isVariadic = false)
    {
        // Two pass match.
        // 1) Align named arguments to fixed parameters by name.
        // 2) Fill remaining unnamed arguments into the next available fixed-param slot;
        //    for variadic functions, overflow args are assigned to trailing variadic positions.

        const size_t inputSize = inputArguments.size();
        const size_t paramSize = targetArguments.size();

        if (isVariadic ? inputSize < paramSize : inputSize != paramSize)
            return {};

        // posMap[i] = destination index in the result vector for inputArguments[i].
        // Fixed-param slots: 0..paramSize-1; variadic slots: paramSize, paramSize+1, ...
        std::vector<int64_t> posMap(inputSize, -1);

        // Pass 1: named arguments - resolve to their fixed-param position by name.
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

        // Mark fixed-param slots claimed by named arguments so pass 2 skips them.
        std::vector<bool> usedTargetMap(paramSize);
        for (int64_t pos : posMap)
        {
            if (pos >= 0)
                usedTargetMap[pos] = true;
        }

        // Pass 2: unnamed arguments - assign to the next free fixed-param slot; for
        // variadic functions, arguments that overflow the fixed params go to trailing slots.
        bool successful = true;
        posIndex = 0;
        int targetIndex = 0;
        size_t nextVariadicIdx = paramSize;

        for (const auto& input : inputArguments)
        {
            if (input.TypeAndValue.VariableName == "")
            {
                bool assigned = false;
                while (targetIndex < (int)paramSize)
                {
                    if (!usedTargetMap[targetIndex])
                    {
                        posMap[posIndex] = targetIndex;
                        usedTargetMap[targetIndex] = true;
                        targetIndex++;
                        assigned = true;
                        break;
                    }
                    targetIndex++;
                }

                if (!assigned)
                {
                    if (isVariadic)
                        posMap[posIndex] = (int64_t)nextVariadicIdx++;
                    else
                    {
                        successful = false;
                        break;
                    }
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

        // Reconstruct arguments in matched order.
        std::vector<LLVMBackend::NamedVariable> result(inputSize);
        for (int i = 0; i < (int)inputSize; i++)
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
        if (name == "__atomic_release_store_i32" || name == "__atomic_release_store_i64" || name == "__atomic_release_store_flag")
        {
            bool is64   = (name == "__atomic_release_store_i64");
            bool isFlag = (name == "__atomic_release_store_flag");
            Value* ptr = args[0];
            Value* val = args[1];
            // bool is i1 in LLVM; atomic ops require byte-sized types - widen to i32.
            if (isFlag)
                val = builder->CreateZExt(val, Type::getInt32Ty(ctx), "flag_i32");
            auto* si = builder->CreateStore(val, ptr);
            si->setAtomic(AtomicOrdering::Release);
            si->setAlignment(is64 ? Align(8) : Align(4));
            return ConstantInt::get(Type::getInt32Ty(ctx), 0); // void: unused
        }
        if (name == "__atomic_acquire_load_i32" || name == "__atomic_acquire_load_i64" || name == "__atomic_acquire_load_flag")
        {
            bool is64   = (name == "__atomic_acquire_load_i64");
            bool isFlag = (name == "__atomic_acquire_load_flag");
            Value* ptr = args[0];
            auto* ty = is64 ? Type::getInt64Ty(ctx) : Type::getInt32Ty(ctx);
            auto* li = builder->CreateLoad(ty, ptr, "atomic_acq_load");
            li->setAtomic(AtomicOrdering::Acquire);
            li->setAlignment(is64 ? Align(8) : Align(4));
            // bool return: compare i32 result with zero.
            if (isFlag)
                return builder->CreateICmpNE(li, ConstantInt::get(Type::getInt32Ty(ctx), 0), "flag_bool");
            return li;
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
            if (candidate.Variadic)
            {
                // Route through MatchFunction so named fixed params are reordered correctly.
                auto matched = MatchFunction(arguments, candidate.Parameters, true);
                if (matched.size() > 0)
                {
                    resolvedCandidate.emplace_back(matched, candidate);
                    break;
                }
            }
            else if (arguments.size() == 0 && candidate.Parameters.size() == 0)
            {
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
                std::string name;
                if (arg.TypeAndValue.VariableName.empty())
                {
                    bool isThis = i == 0 && !candidates.empty() &&
                                  !candidates[0].Parameters.empty() &&
                                  candidates[0].Parameters[0].VariableName.ends_with("__");
                    name = isThis ? "<this>" : "<unnamed>";
                }
                else
                {
                    name = arg.TypeAndValue.VariableName;
                }
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
            // param, which for printf is 'ptr %fmt' - causing all variadic args to be
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
                    auto tempAlloca = AllocaAtEntry(structTy, nullptr);
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
                // use Primary directly - Storage would be the wrong level of indirection.
                if (!arg.TypeAndValue.Pointer && arg.Storage != nullptr
                    && !(arg.Primary != nullptr && arg.Primary->getType()->isPointerTy()))
                    argList.push_back(arg.Storage);
                else if (!arg.TypeAndValue.Pointer && arg.Storage == nullptr
                         && arg.Primary != nullptr && arg.Primary->getType()->isStructTy())
                {
                    // By-value struct parameter passed to a pointer parameter (e.g. args.count()
                    // where args is a list<T> value param). Materialize on the stack first.
                    auto* tempAlloca = AllocaAtEntry(arg.Primary->getType(), nullptr);
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
                // function<T> parameter - dispatch depends on whether the callee is extern C.
                llvm::Value* val = arg.Primary ? arg.Primary : CreateLoad(arg.Storage);
                // Inspect the actual LLVM param type to distinguish fat struct vs C fn ptr.
                auto* llvmParamTy = candidate.Function->getFunctionType()->getParamType((unsigned)argList.size());
                if (llvmParamTy->isStructTy())
                {
                    // Internal function<T>: provide a closure fat struct {i8*, i8*}.
                    if (val && !val->getType()->isStructTy())
                    {
                        // Re-resolve by CallerName to skip method overloads sharing the same key,
                        // and pick the overload whose per-param 'move' flags match the destination.
                        if (!arg.CallerName.empty())
                        {
                            int expectedCount = (int)candParamItr->FuncPtrParams.size();
                            if (auto* correctFn = GetFunctionForFuncPtr(arg.CallerName, expectedCount, &candParamItr->FuncPtrParams))
                                val = correctFn;
                            // Reject when no overload's IsMove flags match the destination signature.
                            if (!HasFunctionWithMoveFlags(arg.CallerName, candParamItr->FuncPtrParams))
                            {
                                LogError(std::format(
                                    "function '{}' has no overload matching the 'move' modifiers required by parameter '{}' - 'move' is part of the function-pointer type",
                                    arg.CallerName, candParamItr->VariableName));
                            }
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
                        // Fat struct - extract the fn ptr (field 0) and cast to expected type.
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
                    if (candParamItr->TypeName == "string" && !candParamItr->Pointer
                        && value && value->getType() == builder->getInt8Ty()->getPointerTo())
                    {
                        // Implicit char* → string coercion: string literal or char* passed to a string param.
                        auto* c = llvm::dyn_cast<llvm::Constant>(value);
                        if (c && IsStringLiteralConstant(c))
                            value = WrapStringLiteralAsString(value);
                        else if (GetFunction("operator string"))
                        {
                            NamedVariable argNV2;
                            argNV2.Primary = value;
                            argNV2.BaseType = value->getType();
                            argNV2.TypeAndValue.TypeName = "char";
                            argNV2.TypeAndValue.Pointer = true;
                            value = CreateOverloadedFunctionCall("operator string", { argNV2 });
                        }
                        else
                            value = WrapStringLiteralAsString(value);
                    }
                    else
                    {
                        // Upconvert to match the declared parameter type (e.g. i16 -> i32).
                        bool argIsUnsigned = arg.TypeAndValue.IsUnsignedInteger() != -1;
                        value = Upconvert(value, GetType(*candParamItr), argIsUnsigned);
                    }
                }

                // Non-owning string (literal or view) passed to a move parameter - heap-copy it
                // so the callee receives an owned buffer it can safely free.
                if (!inVariadicRange &&
                    candParamItr->IsMove &&
                    candParamItr->TypeName == "string" &&
                    !arg.IsOwningString)
                {
                    auto* strTy = llvm::StructType::getTypeByName(*context, "string");
                    if (strTy && GetFunction("operator new"))
                    {
                        auto* srcPtr = builder->CreateExtractValue(value, { 0u }, "litptr");
                        auto* srcLen = builder->CreateExtractValue(value, { 1u }, "litlen");
                        auto* allocSize = builder->CreateAdd(
                            builder->CreateZExt(srcLen, builder->getInt64Ty()),
                            builder->getInt64(1), "litbufsz");
                        NamedVariable szArg;
                        szArg.Primary  = allocSize;
                        szArg.BaseType = builder->getInt64Ty();
                        auto* rawPtr  = CreateOverloadedFunctionCall("operator new", { szArg });
                        auto* heapPtr = builder->CreateBitCast(rawPtr, builder->getInt8Ty()->getPointerTo(), "litbuf");
                        builder->CreateMemCpy(heapPtr, llvm::MaybeAlign(1), srcPtr, llvm::MaybeAlign(1), allocSize);
                        value = llvm::UndefValue::get(strTy);
                        value = builder->CreateInsertValue(value, heapPtr, { 0u });
                        value = builder->CreateInsertValue(value, srcLen,  { 1u });
                    }
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
                LogError(std::format("parameter '{}': cannot pass bonded value to 'move' parameter - bonded values cannot be transferred out of their source's scope", candidate.Parameters[i].VariableName));
        }

        // C-extern ABI lowering: when the resolved candidate has struct-by-value params or
        // return, the LLVM Function was declared with the lowered signature (iN coerce /
        // byval ptr / sret). The current argList still holds CFlat-natural struct values -
        // EmitAbiLoweredCall rewrites it to match the recipe and reloads the struct return
        // for the caller. Otherwise fall through to the existing call path.
        llvm::Value* result = candidate.Recipe.hasLowering
            ? EmitAbiLoweredCall(candidate, argList)
            : CreateFunctionCall(candidate.Function, argList);

        // Cache the resolved return type so callers can populate TypeAndValue after the call.
        lastCallReturnType = candidate.ReturnType;
        lastCallReturnsOwned = candidate.ReturnsOwned;

        // Populate lock-check side-channels for call-site RequiredLocks verification.
        lastCallRequiredLocks = candidate.RequiredLocks;
        lastCallParameterNames.clear();
        for (const auto& p : candidate.Parameters)
            lastCallParameterNames.push_back(p.VariableName);

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
                // Interface fat-ptr parameters (non-pointer) borrow the caller's data - don't zero it.
                // The fat-ptr holds a reference to the caller's struct; zeroing would corrupt that data.
                bool isInterfaceBorrow = candidate.Parameters[i].IsInterface && !candidate.Parameters[i].IsInterfacePointer;
                // When the arg expression went through a cast (or similar), matched[i].Storage
                // may be cleared even though CallerName still names the original owning variable.
                // Look up the source by name so we still null its alloca and prevent a double-free.
                llvm::Value* srcStorage = matched[i].Storage;
                llvm::Type*  srcBaseTy  = matched[i].BaseType;
                if (srcStorage == nullptr && !matched[i].CallerName.empty())
                {
                    auto ref = FindVariableStorage(matched[i].CallerName);
                    srcStorage = ref.Storage;
                    if (srcBaseTy == nullptr) srcBaseTy = ref.BaseType;
                }
                if (srcStorage != nullptr && !isInterfaceBorrow)
                {
                    if (auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(srcBaseTy))
                    {
                        // Pointer move param: null the caller's storage.
                        builder->CreateStore(llvm::ConstantPointerNull::get(ptrTy), srcStorage);
                    }
                    else if (candidate.Parameters[i].TypeName == "string" && matched[i].IsOwningString)
                    {
                        // String move param: zero out _ptr in the caller's alloca so its destructor is a no-op.
                        auto* strTy = llvm::StructType::getTypeByName(*context, "string");
                        if (strTy)
                        {
                            auto* ptrField = builder->CreateStructGEP(strTy, srcStorage, 0);
                            auto* i8ptrTy = builder->getInt8Ty()->getPointerTo();
                            builder->CreateStore(llvm::ConstantPointerNull::get(i8ptrTy), ptrField);
                        }
                    }
                    else if (auto* stTy = llvm::dyn_cast<llvm::StructType>(srcBaseTy))
                    {
                        // Struct move param: zero the caller's entire struct so its destructor is a no-op.
                        builder->CreateStore(llvm::ConstantAggregateZero::get(stTy), srcStorage);
                    }
                }
                // Compile-time: mark the caller's variable as moved so subsequent reads are rejected.
                // Covers pointer, owning-string, and struct move params - all cases where caller storage was zeroed.
                if (!matched[i].CallerName.empty() && srcStorage != nullptr &&
                    !isInterfaceBorrow && srcBaseTy != nullptr)
                {
                    bool isPtr = llvm::isa<llvm::PointerType>(srcBaseTy);
                    bool isOwningStr = candidate.Parameters[i].TypeName == "string" && matched[i].IsOwningString;
                    bool isStruct = llvm::isa<llvm::StructType>(srcBaseTy);
                    if (isPtr || isOwningStr || isStruct)
                        MarkVariableMoved(matched[i].CallerName);
                }
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
    // Returns true if any overload of `functionName` has the given param count AND its per-param IsMove
    // flags match `expectedParams`. Used to validate funcptr-assignment compatibility with `move` modifiers.
    bool HasFunctionWithMoveFlags(std::string functionName, const std::vector<TypeAndValue::FuncPtrParam>& expectedParams) const
    {
        functionName = ResolveQualifiedName(functionName);
        auto it = functionTable.find(functionName);
        if (it == functionTable.end()) return true;  // unknown - leave to other mechanisms
        bool sawCountMatch = false;
        for (const auto& sym : it->second)
        {
            if (sym.IsMethod) continue;
            if (sym.Parameters.size() != expectedParams.size()) continue;
            sawCountMatch = true;
            bool ok = true;
            for (size_t i = 0; i < sym.Parameters.size(); i++)
                if (sym.Parameters[i].IsMove != expectedParams[i].IsMove) { ok = false; break; }
            if (ok) return true;
        }
        // No exact-count overload exists -> not a 'move-modifier' problem; let other type-check paths handle.
        return !sawCountMatch;
    }

    llvm::Function* GetFunctionForFuncPtr(std::string functionName, int expectedParamCount = -1,
                                          const std::vector<TypeAndValue::FuncPtrParam>* expectedParams = nullptr)
    {
        functionName = ResolveQualifiedName(functionName);
        auto it = functionTable.find(functionName);
        if (it == functionTable.end() || it->second.empty())
            return module->getFunction(functionName);

        const auto& overloads = it->second;
        if (overloads.size() == 1)
            return overloads.front().Function;

        // When expectedParams is provided, prefer the overload whose per-param IsMove flags match exactly.
        auto moveFlagsMatch = [&](const FunctionSymbol& sym) -> bool {
            if (!expectedParams) return true;
            if (sym.Parameters.size() != expectedParams->size()) return false;
            for (size_t i = 0; i < sym.Parameters.size(); i++)
                if (sym.Parameters[i].IsMove != (*expectedParams)[i].IsMove) return false;
            return true;
        };

        // Pass 1: non-method, param count + move flags match.
        for (const auto& sym : overloads)
        {
            if (sym.IsMethod) continue;
            if (expectedParamCount >= 0 && (int)sym.Parameters.size() != expectedParamCount) continue;
            if (!moveFlagsMatch(sym)) continue;
            return sym.Function;
        }
        // Pass 2: non-method, count only (legacy behavior when no expectedParams).
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

    bool IsFunctionParameter(const std::string& name) const
    {
        if (name.empty()) return false;
        for (const auto& frame : stackNamedVariable)
            if (frame.functionArgument.find(name) != frame.functionArgument.end())
                return true;
        return false;
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
                // so `begin()` is not guaranteed to be `this` - find it explicitly.
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
                    const auto& sd = findResult->second;
                    for (const auto& structField : sd.StructFields)
                    {
                        if (structField.VariableName == name)
                        {
                            NamedVariable namedVar;
                            auto* fieldLLVMType = GetType(structField);
                            if (sd.IsUnion)
                            {
                                // Union: all fields alias at offset 0; load with explicit field type.
                                namedVar.Storage = memberStructInstance;
                                namedVar.UnionFieldType = fieldLLVMType;
                                namedVar.Primary = CreateLoad(fieldLLVMType, memberStructInstance);
                            }
                            else
                            {
                                namedVar.Storage = CreateStructGEP(sd.StructType, memberStructInstance, count);
                                namedVar.Primary = CreateLoad(namedVar.Storage);
                            }
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

            // Find the implicit 'this' arg - its name ends with "__" (the struct
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

    // Returns the implicit 'this' pointer NamedVariable for the innermost enclosing
    // member-function body, or a default (Storage == nullptr) when not in a method.
    // The result loads to the struct pointer (TypeName = struct, Pointer = true).
    NamedVariable GetThisPointer()
    {
        for (const auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
        {
            const auto& functionArguments = stackFrame.functionArgument;
            if (functionArguments.empty())
                continue;

            // The implicit 'this' arg is named "<StructName>__" (trailing "__").
            for (const auto& [key, nv] : functionArguments)
            {
                if (key.size() < 2 || key.substr(key.size() - 2) != "__")
                    continue;
                std::string structName = key.substr(0, key.size() - 2);
                if (!IsDataStructure(structName))
                    continue;
                // Only a *method* self qualifies here: its storage is an alloca-of-pointer
                // (the incoming this* param), so loading it yields the struct pointer.
                // A constructor's self is an alloca-of-struct (value under construction);
                // leave that to the existing `this`-handling path.
                auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(nv.Storage);
                if (!alloca || !alloca->getAllocatedType()->isPointerTy())
                    break;
                NamedVariable thisVar = nv;
                thisVar.CallerName = "this";
                thisVar.TypeAndValue.TypeName = structName;
                thisVar.TypeAndValue.Pointer = true;
                return thisVar;
            }
            // First frame with arguments but no 'this' -> not a member body.
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

    // Mirror of ApplyAbiAttributes but for a CallInst. LLVM's verifier requires that the
    // sret / byval attributes appear on both the function declaration AND every call site.
    void ApplyAbiCallAttributes(llvm::CallInst* ci, const AbiRecipe& recipe)
    {
        unsigned attrIdx = 0;
        if (recipe.retSlot.kind == AbiSlot::SRetReturn)
        {
            ci->addParamAttr(attrIdx, llvm::Attribute::getWithStructRetType(*context, recipe.retSlot.structTy));
            ci->addParamAttr(attrIdx, llvm::Attribute::NoAlias);
            if (recipe.retSlot.align > 0)
                ci->addParamAttr(attrIdx, llvm::Attribute::getWithAlignment(*context, llvm::Align(recipe.retSlot.align)));
            ++attrIdx;
        }
        for (size_t i = 0; i < recipe.paramSlots.size(); ++i, ++attrIdx)
        {
            const AbiSlot& s = recipe.paramSlots[i];
            if (s.kind == AbiSlot::ByVal)
            {
                ci->addParamAttr(attrIdx, llvm::Attribute::getWithByValType(*context, s.structTy));
                if (s.align > 0)
                    ci->addParamAttr(attrIdx, llvm::Attribute::getWithAlignment(*context, llvm::Align(s.align)));
            }
        }
    }

    // Emit a C-extern call when the resolved overload's signature contains struct-by-value
    // params or return. argList holds the CFlat-natural argument values (struct values are
    // passed as LLVM struct values). This rewrites them into the lowered ABI shape:
    //   - CoerceToInt param: alloca + store the struct, load back as iN, pass the iN.
    //   - ByVal param: alloca + store the struct, pass the alloca pointer (with byval attr at call site).
    //   - SRet return: alloca a return slot, prepend its pointer as arg 0, after the call
    //     load the struct from the slot.
    //   - CoerceToInt return: receive the iN, store into a temp alloca, reload as struct.
    llvm::Value* EmitAbiLoweredCall(const FunctionSymbol& candidate, std::vector<llvm::Value*>& argList)
    {
        const AbiRecipe& recipe = candidate.Recipe;
        std::vector<llvm::Value*> loweredArgs;
        loweredArgs.reserve(argList.size() + (recipe.retSlot.kind == AbiSlot::SRetReturn ? 1 : 0));

        llvm::AllocaInst* sretSlot = nullptr;
        if (recipe.retSlot.kind == AbiSlot::SRetReturn)
        {
            sretSlot = AllocaAtEntry(recipe.retSlot.structTy, nullptr, "sret", recipe.retSlot.align);
            loweredArgs.push_back(sretSlot);
        }

        for (size_t i = 0; i < argList.size() && i < recipe.paramSlots.size(); ++i)
        {
            const AbiSlot& s = recipe.paramSlots[i];
            llvm::Value* v = argList[i];
            if (s.kind == AbiSlot::Direct)
            {
                loweredArgs.push_back(v);
            }
            else if (s.kind == AbiSlot::CoerceToInt)
            {
                // v is a struct value. Place in an alloca, then load as iN through a
                // bitcast - portable across all element layouts and lets LLVM coalesce.
                auto* slot = AllocaAtEntry(s.structTy, nullptr, "abi.coerce", s.align);
                builder->CreateStore(v, slot);
                auto* intPtr = builder->CreateBitCast(slot, s.coerceTy->getPointerTo());
                loweredArgs.push_back(builder->CreateLoad(s.coerceTy, intPtr));
            }
            else // ByVal
            {
                auto* slot = AllocaAtEntry(s.structTy, nullptr, "abi.byval", s.align);
                builder->CreateStore(v, slot);
                loweredArgs.push_back(slot);
            }
        }

        auto* ci = builder->CreateCall(candidate.Function, loweredArgs);
        ci->setCallingConv(candidate.Function->getCallingConv());
        ApplyAbiCallAttributes(ci, recipe);

        if (recipe.retSlot.kind == AbiSlot::SRetReturn)
            return builder->CreateLoad(recipe.retSlot.structTy, sretSlot);
        if (recipe.retSlot.kind == AbiSlot::CoerceToInt)
        {
            auto* slot = AllocaAtEntry(recipe.retSlot.structTy, nullptr, "abi.ret", recipe.retSlot.align);
            auto* intPtr = builder->CreateBitCast(slot, recipe.retSlot.coerceTy->getPointerTo());
            builder->CreateStore(ci, intPtr);
            return builder->CreateLoad(recipe.retSlot.structTy, slot);
        }
        return ci; // Direct return
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

        auto* ci = builder->CreateCall(func, arg);
        ci->setCallingConv(func->getCallingConv());
        return ci;
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

        // Auto return-type inference: record the (BB, value) pair so the caller can
        // unify types after body emission. Terminate the BB with a placeholder
        // 'unreachable' that will be replaced with a real 'ret' once the inferred
        // signature is known and BBs are spliced onto the new function.
        if (autoReturnCapture)
        {
            auto* placeholder = builder->CreateUnreachable();
            autoReturnCapture->push_back({ builder->GetInsertBlock(), value, placeholder });
            return;
        }

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
            // Upconvert only widens; handle narrowing int -> bool explicitly (same as CreateAssignment).
            // Warn: CFlat requires explicit narrowing - write "return expr != 0;" instead.
            if (retTy == builder->getInt1Ty() && value->getType()->isIntegerTy() && value->getType() != retTy)
            {
                LogError("implicit int-to-bool conversion on return - use '!= 0' to make narrowing explicit");
                value = builder->CreateICmpNE(value, llvm::ConstantInt::get(value->getType(), 0), "tobool");
            }
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

    void BeginAutoReturnCapture() { autoReturnCapture.emplace(); }
    std::vector<AutoReturnSite> EndAutoReturnCapture()
    {
        std::vector<AutoReturnSite> sites;
        if (autoReturnCapture) sites = std::move(*autoReturnCapture);
        autoReturnCapture.reset();
        return sites;
    }
    bool IsAutoReturnCaptureActive() const { return autoReturnCapture.has_value(); }

    // Best-effort reverse mapping from an LLVM type to a CFlat TypeName, used by
    // 'auto' return-type inference to populate the function table's ReturnType
    // after the unified return type is known. Falls back to "i64" so the entry
    // is never left empty.
    // Convenience: just the CFlat type name from an LLVM type. Useful for generic
    // argument inference where the argument's TypeName has been stripped but its
    // LLVM type still describes the underlying primitive.
    std::string LlvmTypeToTypeName(llvm::Type* t) const
    {
        return LlvmTypeToTypeAndValue(t).TypeName;
    }

    TypeAndValue LlvmTypeToTypeAndValue(llvm::Type* t) const
    {
        TypeAndValue tv;
        if (!t) { tv.TypeName = "void"; return tv; }
        if (t->isVoidTy())              { tv.TypeName = "void"; return tv; }
        if (t->isIntegerTy(1))          { tv.TypeName = "bool"; return tv; }
        if (t->isIntegerTy(8))          { tv.TypeName = "i8";   return tv; }
        if (t->isIntegerTy(16))         { tv.TypeName = "i16";  return tv; }
        if (t->isIntegerTy(32))         { tv.TypeName = "int";  return tv; }
        if (t->isIntegerTy(64))         { tv.TypeName = "i64";  return tv; }
        if (t->isFloatTy())             { tv.TypeName = "float";  return tv; }
        if (t->isDoubleTy())            { tv.TypeName = "double"; return tv; }
        if (t->isPointerTy())
        {
            tv.TypeName = "i8";  // opaque pointer in modern LLVM; pointee is unknown here
            tv.Pointer  = true;
            return tv;
        }
        if (t->isStructTy())
        {
            auto* st = llvm::cast<llvm::StructType>(t);
            if (st->hasName())
            {
                tv.TypeName = st->getName().str();
                return tv;
            }
        }
        tv.TypeName = "i64";
        return tv;
    }

    // After an 'auto' generic instantiation has emitted its body under
    // BeginAutoReturnCapture, replace the placeholder function with one whose
    // signature uses the unified return type. Splices basic blocks from the
    // placeholder over, rewrites each captured 'unreachable' placeholder into a
    // real 'ret', remaps argument uses, updates the function table, and erases
    // the old function. Returns the replacement function (or the original if
    // unification failed - caller should treat that as already-diagnosed).
    llvm::Function* FinalizeAutoReturnFunction(
        const std::string& functionName,
        llvm::Function* oldFn,
        std::vector<AutoReturnSite>& sites,
        std::vector<TypeAndValue> arguments,
        bool varargs,
        bool returnsOwned,
        bool isMethod)
    {
        if (sites.empty())
        {
            LogError(std::format("'auto' return: function '{}' has no return statement; explicit return required for type inference", functionName));
            return oldFn;
        }

        // Unify return value types. v1 rule: identical types accepted; otherwise
        // pick the strictly-wider one if CompareUpconvert says it widens. No
        // bidirectional promotion (no int+long -> long folding yet); reject with
        // a clear message so the user knows to cast at the source level.
        llvm::Type* unifiedTy = sites[0].Value ? sites[0].Value->getType() : builder->getVoidTy();
        for (size_t i = 1; i < sites.size(); i++)
        {
            llvm::Type* siteTy = sites[i].Value ? sites[i].Value->getType() : builder->getVoidTy();
            if (siteTy == unifiedTy) continue;
            // Both must be non-void (cannot mix 'return;' with 'return expr;').
            if (siteTy->isVoidTy() || unifiedTy->isVoidTy())
            {
                LogError(std::format("'auto' return: cannot mix 'return;' and 'return <expr>;' in function '{}'", functionName));
                return oldFn;
            }
            int srcToCur = CompareUpconvert(siteTy, unifiedTy);   // widens to current?
            int curToSrc = CompareUpconvert(unifiedTy, siteTy);   // current widens to new?
            if (srcToCur > 0)      { /* keep unifiedTy, siteTy widens up */ }
            else if (curToSrc > 0) { unifiedTy = siteTy; }
            else
            {
                LogError(std::format("'auto' return: cannot unify return types in function '{}'", functionName));
                return oldFn;
            }
        }

        // Build the new function type with the same params and the unified return.
        std::vector<llvm::Type*> paramTypes(oldFn->getFunctionType()->params().begin(),
                                            oldFn->getFunctionType()->params().end());
        auto* newFnTy = llvm::FunctionType::get(unifiedTy, paramTypes, varargs);

        TypeAndValue newReturnType = LlvmTypeToTypeAndValue(unifiedTy);
        std::string newMangledName = ComputeMangledName(functionName, newReturnType, arguments, varargs);

        // If a function with the new mangled name already exists (e.g. another
        // instantiation of the same template hit the same inferred type), reuse it.
        if (auto* existing = module->getFunction(newMangledName); existing && !existing->empty())
        {
            // Discard the placeholder; the existing definition wins.
            if (!oldFn->use_empty())
                LogError(std::format("'auto' return: recursive call in function '{}' is not yet supported", functionName));
            oldFn->eraseFromParent();
            return existing;
        }

        auto* newFn = llvm::Function::Create(newFnTy, llvm::Function::ExternalLinkage, newMangledName, *module);
        newFn->addFnAttr(llvm::Attribute::NullPointerIsValid);
        newFn->setCallingConv(oldFn->getCallingConv());

        // Splice basic blocks from old to new and remap argument uses.
        std::vector<llvm::BasicBlock*> bbs;
        for (auto& bb : *oldFn) bbs.push_back(&bb);
        for (auto* bb : bbs)
        {
            bb->removeFromParent();
            bb->insertInto(newFn);
        }
        // Remap argument uses by value pointer. Do NOT setName on the new args:
        // the spliced entry block already contains allocas named after the params
        // (e.g. "a"), which would collide and trigger LLVM auto-suffixing in a way
        // that misaligns subsequent load/store operands relative to their displayed
        // names. Arg display becomes %0, %1, ... which is fine for IR validity.
        auto oldArgIt = oldFn->arg_begin();
        auto newArgIt = newFn->arg_begin();
        for (; oldArgIt != oldFn->arg_end() && newArgIt != newFn->arg_end(); ++oldArgIt, ++newArgIt)
        {
            oldArgIt->replaceAllUsesWith(&*newArgIt);
        }

        // Replace each captured 'unreachable' placeholder with the real ret.
        for (auto& site : sites)
        {
            auto* ph = site.Placeholder;
            builder->SetInsertPoint(ph);
            if (site.Value != nullptr)
            {
                llvm::Value* retVal = site.Value;
                if (retVal->getType() != unifiedTy)
                    retVal = Upconvert(retVal, unifiedTy);
                builder->CreateRet(retVal);
            }
            else
            {
                builder->CreateRetVoid();
            }
            ph->eraseFromParent();
        }

        // Transfer the DI subprogram so debug info stays attached.
        if (auto* sp = oldFn->getSubprogram())
        {
            oldFn->setSubprogram(nullptr);
            newFn->setSubprogram(sp);
        }

        // Update the function table entry that pointed at the placeholder. We do
        // this before erasing so the dangling pointer compare is well-defined.
        auto it = functionTable.find(functionName);
        if (it != functionTable.end())
        {
            for (auto& sym : it->second)
            {
                if (sym.Function == oldFn)
                {
                    sym.Function    = newFn;
                    sym.UniqueName  = newMangledName;
                    sym.ReturnType  = newReturnType;
                    sym.ReturnsOwned = returnsOwned;
                    sym.IsMethod    = isMethod;
                    sym.Parameters  = arguments;
                    sym.Variadic    = varargs;
                    break;
                }
            }
        }

        // Recursion would leave uses behind (call to oldFn from inside its own body).
        // Diagnose explicitly rather than letting LLVM's verifier complain later.
        if (!oldFn->use_empty())
            LogError(std::format("'auto' return: recursive call in function '{}' is not yet supported - declare the return type explicitly", functionName));

        oldFn->eraseFromParent();
        return newFn;
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

    // Swap the elseBlock in the innermost frame that owns one.
    // Returns the previous value so the caller can restore it.
    // Used by the '!' operator to prevent inner && from short-circuiting
    // directly to the outer false-branch (which would bypass the negation).
    llvm::BasicBlock* ExchangeElseBlock(llvm::BasicBlock* newBlock)
    {
        for (auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (stackFrame.elseBlock || stackFrame.isFunction)
            {
                auto old = stackFrame.elseBlock;
                stackFrame.elseBlock = newBlock;
                return old;
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
        if (builder->GetInsertBlock()->getTerminator() != nullptr)
            return;

        for (auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
        {
            EmitDestructorsForScope(stackFrame);
            if (stackFrame.resumeBlock)
            {
                builder->CreateBr(stackFrame.resumeBlock);
                break;
            }
        }
    }

    void CreateContinueCall()
    {
        if (builder->GetInsertBlock()->getTerminator() != nullptr)
            return;

        for (auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (stackFrame.continueBlock)
            {
                builder->CreateBr(stackFrame.continueBlock);
                break;
            }
            EmitDestructorsForScope(stackFrame);
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
    bool IsImportAlias(const std::string& name) const { return importAliasMembers.count(name) > 0; }
    bool IsImportAliasMember(const std::string& alias, const std::string& member) const
    {
        auto it = importAliasMembers.find(alias);
        return it != importAliasMembers.end() && it->second.count(member) > 0;
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

        // "$global$:<alias>" sentinel: file-scoped import alias (import "x.cb" as Alias).
        // Resolve to unqualified lastName only if it was contributed by that file.
        if (nsPrefix.starts_with("$global$:"))
        {
            std::string aliasName = nsPrefix.substr(9);
            if (IsImportAliasMember(aliasName, lastName))
                return lastName;
            return name;
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
    bool CompileImportedFile(const std::string& importingFilePath, const std::string& importFilename, const std::string& namespaceName = {}, const std::string& programAlias = {}, const std::string& explicitLib = {}, const std::vector<std::string>& extraDefines = {});

    // Vcpkg integration setters - wired from CLI flags in main.cpp.
    void SetVcpkgExe(const std::string& path)        { vcpkg_.SetExeOverride(path); }
    void SetVcpkgManifest(const std::string& path)   { vcpkg_.SetManifestOverride(path); }
    void SetVcpkgTriplet(const std::string& triplet) { vcpkg_.SetTripletOverride(triplet); }

    // Handle `import package-vcpkg "header" from "port[features]";`. Resolves the port
    // through the user-owned vcpkg.json, pushes the resulting include dir / libs / DLLs
    // into the per-backend accumulators, then routes the header through CompileCHeader
    // (the existing C-header binding path). Returns false on hard error.
    bool CompileVcpkgImport(const std::string& importingFilePath,
                            const std::string& header,
                            const std::string& portSpec)
    {
        // Mirror LSP/non-LSP mode and verbosity into the resolver.
        vcpkg_.SetVerbose(verbose);
        vcpkg_.SetLspMode(symbolSink_ != nullptr);
        vcpkg_.SetPlatform(platformValue == 32 ? "win32" : "win64");

        VcpkgResolver::Resolution res;
        std::string err;
        if (!vcpkg_.Resolve(importingFilePath, portSpec, res, err))
        {
            LogError(err);
            return false;
        }

        // Push the resolved paths into the existing accumulators. Idempotent: a second
        // package-vcpkg import re-pushes the same include dir, which is harmless (clang-cl
        // dedupes -I, lld-link dedupes libs).
        if (!res.includeDir.empty())
        {
            bool dup = false;
            for (const auto& d : cIncludeDirs_) if (d == res.includeDir) { dup = true; break; }
            if (!dup) cIncludeDirs_.push_back(res.includeDir);
        }
        for (const auto& lib : res.libs)
        {
            bool dup = false;
            for (const auto& l : cLinkLibs_) if (l == lib) { dup = true; break; }
            if (!dup) cLinkLibs_.push_back(lib);
        }
        for (const auto& dll : res.dlls)
        {
            bool dup = false;
            for (const auto& d : vcpkgRuntimeDlls_) if (d == dll) { dup = true; break; }
            if (!dup) vcpkgRuntimeDlls_.push_back(dll);
        }

        // The header path in source is relative to the include dir (e.g. "curl/curl.h").
        // Resolve it against the vcpkg include dir explicitly so CompileCHeader sees an
        // absolute path; the source-location filter in clang's AST dump already keeps
        // only decls under our --c-include roots, so unrelated SDK/CRT decls stay out.
        std::filesystem::path headerAbs = std::filesystem::path(res.includeDir) / header;
        std::error_code ec;
        auto headerCanon = std::filesystem::canonical(headerAbs, ec);
        if (ec)
        {
            LogError(std::format(
                "import package-vcpkg: header '{}' not found under '{}'.\n"
                "  The port may not own this header, or the install is incomplete.",
                header, res.includeDir));
            return false;
        }
        bool ok = CompileCHeader(headerCanon.string(), {});
        if (ok) ProcessPendingMacroSources();
        return ok;
    }
    bool Analyze(const std::string& filePath, const std::string& importDir, const std::string& runtimeDirPath);
    void ResetForReanalysis();
};

// Defined here so LLVMBackend is fully declared before DumpState() is called.
inline void CompilerManager::DumpAllState() const
{
    std::lock_guard<std::mutex> lock(mutex_);
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
