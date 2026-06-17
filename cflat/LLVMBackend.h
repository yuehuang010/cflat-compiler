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
#include <set>
#include <iostream>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <io.h>

#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include <llvm\IR\CFG.h>          // llvm::pred_empty (MarkUnreachableIfNoPredecessors)
#include <llvm\IR\IRBuilder.h>
#include <llvm\IR\MDBuilder.h>    // llvm::MDBuilder (alias-scope/domain metadata for T[] noalias)
#include <llvm\IR\Intrinsics.h>
#include <llvm\IR\IntrinsicsX86.h>   // llvm::Intrinsic::x86_rdtscp (CreateRdtscp)
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
#include <llvm\Support\TimeProfiler.h>
#include <llvm\Support\TargetSelect.h>
#include <llvm\Target\TargetMachine.h>
#include <llvm\ExecutionEngine\Orc\LLJIT.h>          // --run JIT execution (JitRun)
#include <llvm\ExecutionEngine\Orc\ExecutionUtils.h> // DynamicLibrarySearchGenerator
#include <llvm\ExecutionEngine\Orc\ObjectLinkingLayer.h> // JITLink layer (--run threading/SEH)
#include <llvm\ExecutionEngine\JITLink\JITLink.h>        // LinkGraph (SEH .pdata registration)
#include <unordered_map>
#include <malloc.h>  // _aligned_malloc for the emutls shim
#include <llvm\MC\TargetRegistry.h>
#include <llvm\MC\MCSubtargetInfo.h>
#include <llvm\TargetParser\Host.h>
#pragma warning(pop)
#include <antlr4-runtime.h>
#include <CFlatParser.h>
#include <CFlatLexer.h>
#include <fstream>
#include "ArgParser.h"
#include "CClangExtract.h"
#include "CompilerManager.h"

#include "LspSymbolIndex.h"
#include "CFlatErrorListener.h"
#include "VcpkgResolver.h"

struct ExpectedErrorReceived {};

// Resolved paths for lld-link and the MSVC / Windows SDK lib directories.
// Persisted to %USERPROFILE%\.cflat\linker_paths_<arch>.json by --init and
// loaded from there by EmitExecutable before falling back to live discovery.
struct LinkerPaths {
    std::string lldLink;
    std::string msvcLib;
    std::string ucrtLib;
    std::string umLib;
    bool AllExist() const
    {
        auto e = [](const std::string& p) { return p.empty() || llvm::sys::fs::exists(p); };
        return !lldLink.empty() && llvm::sys::fs::exists(lldLink) && e(msvcLib) && e(ucrtLib) && e(umLib);
    }
};

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

// --run (JIT) emulated-TLS support. On x86_64-pc-windows-msvc LLVM lowers thread-locals to
// emulated TLS, which calls __emutls_get_address(control) to fetch this thread's copy of a
// thread-local. That helper normally lives in compiler-rt (which we do not link), so for the
// in-process JIT we provide our own, backed by a host C++ thread_local map. JitRun injects
// &CflatEmutlsGetAddress as an absolute symbol for "__emutls_get_address".
//
// Limitation: per-thread blocks are freed only at process exit, not thread exit (no
// destructor hook). Acceptable for the run-and-exit --run utility; revisit before relying on
// --run for long-lived multi-threaded programs.
namespace cflat_jit
{
    // Matches compiler-rt's __emutls_control: { size, align, object-union, value }. We only
    // read size/align/value; the object-union slot is managed per-thread by us.
    struct EmutlsControl { size_t size; size_t align; void* object; void* value; };

    inline void* CflatEmutlsGetAddress(void* control)
    {
        auto* c = static_cast<EmutlsControl*>(control);
        thread_local std::unordered_map<void*, void*> storage;
        auto it = storage.find(control);
        if (it != storage.end())
            return it->second;

        size_t align = c->align ? c->align : alignof(std::max_align_t);
        void* mem = _aligned_malloc(c->size, align);
        if (c->value) std::memcpy(mem, c->value, c->size);
        else          std::memset(mem, 0, c->size);
        storage.emplace(control, mem);
        return mem;
    }

    // JITLink plugin that registers Windows x64 unwind info for JIT'd code. JITLink fixes up
    // the .pdata section (an array of 12-byte RUNTIME_FUNCTION records, each three ADDR32NB
    // RVA fields: function begin, function end, unwind-info), but without ORC's COFFPlatform
    // (which needs the orc_rt runtime we do not link) nothing calls RtlAddFunctionTable - so
    // the OS cannot find unwind data for JIT'd frames and any SEH / C++ EH / FP-trap unwind on
    // a worker thread faults. This registers it manually.
    //
    // The RVAs in .pdata are relative to the image base JITLink picked. Rather than guess that
    // base, we recover it from a .pdata relocation: a stored RVA equals (target - imageBase),
    // so imageBase = target - storedRVA. We then hand the in-memory .pdata array straight to
    // RtlAddFunctionTable with that base.
    class SehRegistrationPlugin : public llvm::orc::ObjectLinkingLayer::Plugin
    {
    public:
        void modifyPassConfig(llvm::orc::MaterializationResponsibility&,
                              llvm::jitlink::LinkGraph&,
                              llvm::jitlink::PassConfiguration& Config) override
        {
            // PreFixup: memory is allocated and every node - including the resolved external
            // __ImageBase - has its final address, but fixups have not been applied yet. The
            // JITDylib defines __ImageBase as a placeholder (address 0, just to satisfy the
            // symbol lookup); here we rewrite it to the real image base so the ADDR32NB fixups
            // in .pdata/.xdata compute correct, in-range 32-bit RVAs.
            Config.PreFixupPasses.push_back(
                [](llvm::jitlink::LinkGraph& G) { return FixImageBase(G); });

            // PostFixup: relocations are applied and addresses are final (the in-process
            // memory manager links at the executor addresses), so .pdata holds real RVAs.
            // Unwind cannot fire until the code runs, which is after finalization, so
            // registering here is safe.
            Config.PostFixupPasses.push_back(
                [](llvm::jitlink::LinkGraph& G) { return RegisterUnwindInfo(G); });
        }

        // Pure-virtual ResourceManager hooks. --run is run-and-exit, so there is no teardown:
        // we never deregister the tables (the process owns them until it exits).
        llvm::Error notifyFailed(llvm::orc::MaterializationResponsibility&) override
        { return llvm::Error::success(); }
        llvm::Error notifyRemovingResources(llvm::orc::JITDylib&, llvm::orc::ResourceKey) override
        { return llvm::Error::success(); }
        void notifyTransferringResources(llvm::orc::JITDylib&, llvm::orc::ResourceKey,
                                         llvm::orc::ResourceKey) override {}

    private:
        // Lowest block address in the graph - the image base every ADDR32NB RVA is relative to.
        // The whole graph is one contiguous-ish allocation well under 4GB, so this base keeps
        // all RVAs non-negative and in 32-bit range.
        static llvm::orc::ExecutorAddr LowestBlockAddress(llvm::jitlink::LinkGraph& G)
        {
            llvm::orc::ExecutorAddr base;
            bool found = false;
            for (auto& Sec : G.sections())
                for (auto* B : Sec.blocks())
                    if (!found || B->getAddress() < base) { base = B->getAddress(); found = true; }
            return base; // default-constructed (0) if the graph has no blocks
        }

        // PreFixup: point the resolved __ImageBase symbol at the real image base (it is defined
        // as a placeholder in the JITDylib only to satisfy the lookup). After this, fixups see
        // the correct anchor. Matches the base LowestBlockAddress feeds RegisterUnwindInfo.
        static llvm::Error FixImageBase(llvm::jitlink::LinkGraph& G)
        {
            llvm::jitlink::Symbol* sym = nullptr;
            for (auto* S : G.external_symbols())
                if (S->hasName() && S->getName() == "__ImageBase") { sym = S; break; }
            if (!sym)
                for (auto* S : G.absolute_symbols())
                    if (S->hasName() && S->getName() == "__ImageBase") { sym = S; break; }
            if (sym)
                sym->getAddressable().setAddress(LowestBlockAddress(G));
            return llvm::Error::success();
        }

        // Defined out-of-line in LLVMBackend.cpp so the RtlAddFunctionTable forward-declaration
        // stays in a TU that does not include <winnt.h> (avoids an extern "C" overload clash
        // with the real prototype in TUs like LspServer.cpp that do include windows.h).
        static llvm::Error RegisterUnwindInfo(llvm::jitlink::LinkGraph& G);
    };
}

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

    // Structured facts about one `vectorize` loop, gathered at codegen time and
    // used to compose a precise failure diagnostic (see ComposeVectorizeFailure).
    struct VectorizeLoopInfo
    {
        int line = 0, col = 0;          // the `vectorize` keyword
        bool isWhile = false;           // while form (vs counted for)
        bool conditionCounted = true;   // false when a while condition is equality/sentinel (==,!=) - not a counted bound
        int condLine = 0, condCol = 0;  // the loop condition
        std::string condText;
        bool hasCall = false;           // a call appears in the loop body
        std::string callName;           // its callee text
        int callLine = 0, callCol = 0;  // the call site
        // A span<T> accessor (get/set/operator[]) used in the body that routes through the
        // method's `this` and so cannot carry the noalias contract - the precise cause when a
        // vectorized loop still keeps a runtime alias check (the Detection-B failure below).
        bool hasSpanAccessor = false;
        std::string spanAccessor;       // "get", "set", or "operator[]"
        std::string spanReceiver;       // receiver variable name (for the `.data()` hint), if known
        int spanLine = 0, spanCol = 0;  // the accessor site
    };
    void AddVectorizeLoopInfo(const VectorizeLoopInfo& info) { vectorizeLoops_.push_back(info); }

    // Field index of a noalias array-view buffer field named `_ptr` (the span<T> convention),
    // or -1 if `typeName` is not such a wrapper. Used to (a) lower a span subscript to the field
    // access that carries noalias and (b) recognize a span receiver for the vectorize hint. The
    // may-alias sibling view<T> has a raw-T* `_ptr` (IsArrayView=false) and so returns -1.
    int ArrayViewBufferFieldIndex(const std::string& typeName)
    {
        if (typeName.empty()) return -1;
        const auto& ds = GetDataStructure(typeName);
        for (int i = 0; i < (int)ds.StructFields.size(); ++i)
            if (ds.StructFields[i].VariableName == "_ptr" && ds.StructFields[i].IsArrayView)
                return i;
        return -1;
    }

    // Record (first-writer-wins) that a span accessor was used inside the vectorize loop whose
    // keyword is on `loopLine`, so a surviving runtime alias check can name it. No-op if the loop
    // is unknown or already has an accessor recorded.
    void NoteVectorizeSpanAccessor(int loopLine, const std::string& accessor,
                                   const std::string& receiver, int line, int col)
    {
        for (auto& vi : vectorizeLoops_)
            if (vi.line == loopLine && !vi.hasSpanAccessor)
            {
                vi.hasSpanAccessor = true;
                vi.spanAccessor = accessor;
                vi.spanReceiver = receiver;
                vi.spanLine = line;
                vi.spanCol = col;
                return;
            }
    }

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

        // Fixed-array sizes contributed by a TYPE ALIAS (using Vec3 = float[3]). The alias string
        // stores the folded sizes ("float[3]"); ParseDeclarationSpecifiers peels them here so the
        // per-declarator finalization can adopt them when the declarator carries no brackets of its
        // own (the size lives on the type, shared by every declarator). 0 / empty = no alias array.
        uint64_t AliasArraySize = 0;
        std::vector<uint64_t> AliasInnerDims;

        // simd<T,N>: builtin special-form vector type. IsSimd => TypeName is the element scalar (e.g. "float"),
        // SimdLanes is N (power of 2). Lowers to LLVM <N x T>. A primitive value (register-resident), NOT a
        // struct/array aggregate and NOT a pointer - never combined with Pointer/ConstArraySize.
        bool IsSimd = false;
        uint64_t SimdLanes = 0;

        // Thin `int[]` array-view: an `int*` representation (Pointer is also set) carrying a
        // noalias contract. Distinct `int[]` values point at distinct WHOLE allocations
        // (pointer arithmetic and the `int* -> int[]` cast are both forbidden, so an offset
        // sub-view is unconstructible). Drives: LLVM noalias on params, the arithmetic ban,
        // distinct mangling, and `int[] -> int*` implicit decay. See doc/LANGUAGE.md.
        bool IsArrayView = false;

        // Transient per-function noalias provenance: when >= 0, indexes into LLVMBackend::aliasScopes_
        // the alias-scope this view's element loads/stores should be tagged with. Keyed by ORIGIN
        // (parameter or struct field), not SSA value, so copies of the same view share a scope and are
        // therefore NOT treated as disjoint. NEVER serialized (it is meaningless across functions); it
        // is recomputed each function from the live aliasScopes_ registry. See MintAliasScope /
        // AttachViewNoalias.
        int NoaliasScopeId = -1;

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

            if (IsArrayView)
            {
                // Distinct from a bare pointer so `f(int[])` and `f(int*)` are separate overloads.
                return (TypeName == "void" ? std::string("U8") : type) + "Arr";
            }

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

        // Bitfield support (struct/union fields only; 0 = not a bitfield).
        // Layout (set by the packing pass before CreateStructType):
        //   BitWidth          - width in bits, as written (1..N where N = sizeof(underlying)*8)
        //                       0 with no name is the C "width-0 boundary" marker
        //   BitOffset         - LSB-first offset within the storage unit (MSVC ordering)
        //   StorageFieldIndex - LLVM struct element index of the underlying storage unit;
        //                       multiple packed bitfields share the same index.
        //   IsBitfield        - true for both named bitfields and width-0 padding markers;
        //                       width-0 entries are dropped from StructFields after packing.
        unsigned BitWidth = 0;
        unsigned BitOffset = 0;
        unsigned StorageFieldIndex = 0;
        bool IsBitfield = false;
        // True on the synthesized storage slot produced by PackBitfields. The
        // default constructor uses this to zero-initialize the slot even when
        // the user wrote no per-bitfield initializer.
        bool IsBitfieldStorage = false;

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
        bool BorrowsOwnedString = false; // true when a string local was initialized/assigned from an owning string FIELD (a non-owning alias of a heap buffer some struct still owns) - storing it into another field would double-free, so the field-store path rejects it
        bool IsOwningStruct = false;     // true for move parameters of struct types with destructors - destructor called on scope exit
        bool IsMoved = false;            // compile-time: true after this variable's ownership was transferred via a move call
        std::set<std::string> MovedFields; // compile-time: field names moved out of this variable via a 'move' of a sub-path (e.g. `node->left`) - the base stays usable
        bool IsBonded = false;           // compile-time: true when this variable holds a bonded (borrowed) return value
        bool BondByAddress = false;      // bond originates from a by-address lambda capture; reassigning the source is safe
        std::vector<std::string> BondedSources; // names of bond parameters this value borrows from
        // For a lambda literal: the (already de-duplicated) names of enclosing locals/params it
        // captures, in capture order. Empty for non-capturing lambdas and for stored function<>
        // values. Used to produce a precise diagnostic when a capturing lambda is illegally passed
        // to a C function-pointer parameter (the C ABI cannot carry captured state).
        std::vector<std::string> LambdaCaptureNames;
        bool IsBorrowed = false;         // compile-time: true for non-move pointer parameters and locals that alias one - 'delete' is forbidden
        std::string BorrowedOrigin;      // name of the borrowed parameter this value transitively aliases (for diagnostics)
        llvm::Value* RefCountStorage = nullptr; // lazy i32 alloca at function entry; non-null only when pointer escaped to a field
        std::string CallerName;          // the variable's name at the call site, for move tracking
        std::string OwningStructName;    // when this NamedVariable is a struct-field access, the field's owning struct
        std::string FieldName;           // when this NamedVariable is a struct-field access, the field name
        // Bitfield access metadata. Non-null BitfieldStorage means the variable
        // is a bitfield view onto a storage word; reads compute shift+mask, and
        // writes do a read-modify-write on this pointer.
        llvm::Value* BitfieldStorage = nullptr;   // GEP'd pointer to the storage word
        llvm::Type*  BitfieldStorageType = nullptr; // the storage word's LLVM type (e.g. i32)
        unsigned BitfieldOffset = 0;
        unsigned BitfieldWidth  = 0;
        bool BitfieldUnsigned   = false;
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
        bool         isArrayView = false;   // value came from a thin `int[]` view (pointer arithmetic is banned on it)

        TypedValue() = default;
        TypedValue(llvm::Value* v, bool u = false) : value(v), isUnsigned(u) {}

        operator llvm::Value*()   const { return value; }
        llvm::Value* operator->() const { return value; }
        explicit operator bool()  const { return value != nullptr; }
    };

    // One named bitfield. Multiple BitfieldInfo entries may share the same
    // StorageFieldIndex when the packing pass groups adjacent bitfields of the
    // same underlying type into a single storage unit (MSVC LSB-first layout).
    struct BitfieldInfo
    {
        std::string Name;
        std::string TypeName;        // declared underlying type ("int", "u8", ...)
        bool IsUnsigned = false;
        unsigned StorageFieldIndex = 0;  // index into StructType / StructFields (storage slot)
        unsigned BitOffset = 0;          // LSB-first offset within the storage unit
        unsigned BitWidth = 0;           // bits the user wrote after ':'
        std::vector<AnnotationValue> Annotations;
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
        // Bitfield side-table. Field-name lookup checks this BEFORE StructFields
        // so that a packed run of bitfields surfaces through their declared names.
        // StructFields itself contains the (synthetic) storage slots produced by
        // the packing pass; they have names like `__bf0` and are not user-visible.
        std::vector<BitfieldInfo> Bitfields;
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
        unsigned StopSourceFieldIndex = 0;             // struct field index of _stop_source (stop_source)
        unsigned TrackHandlesFieldIndex = 0;           // struct field index of trackHandles (bool)
        unsigned UseChannelFieldIndex = 0;             // struct field index of useChannel (int, 0/1). User opt-in: `p1 >> p2` only wires the arena channel when BOTH programs have useChannel set. Stream piping is always wired.
        unsigned FpConfigFieldIndex = 0;               // struct field index of _fpConfig (int). User-settable per-thread FP environment (traps + flush-to-zero) applied on the program thread before main(). 0 = no-op.
        unsigned OutFieldIndex      = (unsigned)-1;     // struct field index of _out (stream*); -1 when stream.cb not imported
        unsigned InStreamFieldIndex = (unsigned)-1;     // struct field index of _in  (stream*); -1 when stream.cb not imported
        unsigned InboxArenaFieldIndex = (unsigned)-1;   // struct field index of inbox (arena_channel*); -1 when arena_channel.cb not imported. Consumer owns it; lazily allocated by `>>`.
        unsigned OutboxFieldIndex     = (unsigned)-1;   // struct field index of outbox (arena_channel*); -1 when arena_channel.cb not imported. Producer handle bound by `>>` to a consumer's inbox.
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
    bool currentFunctionReturnIsArrayView = false; // true when the current function's return type is a `T[]` array-view
    std::string currentFunctionReturnTypeName; // declared return TypeName of the current function (e.g. an interface name); used to box a returned concrete pointer into the interface fat pointer
    TypeAndValue currentFunctionReturnTV; // full declared return TypeAndValue of the current function; used to thread a function<> return type into a returned lambda literal's expected type

    // When returning a struct VALUE local whose full-destructor frees members (e.g. owned
    // string fields), the by-value snapshot carries those member pointers to the caller, so
    // the local's destructor must be skipped on the return path or the returned value would
    // dangle (and double-free when the caller destroys it). CreateReturnCall sets this to the
    // returned local's alloca around the scope-exit destructor walk; EmitDestructorsForScope
    // skips the struct whose Storage matches. Null on every non-return scope exit.
    llvm::Value* returnedStructDtorSkipAlloca = nullptr;

    // Owned-string SSA temporaries produced by a ReturnsOwned call (e.g. the unnamed
    // (a + b) intermediate of a chained concat a + b + c) that are consumed as a
    // sub-expression rather than bound to a named local. They have no NamedVariable
    // and so are invisible to EmitDestructorsForScope; without explicit cleanup their
    // heap buffer leaks. Each entry pairs the SSA string value with the basic block it
    // was created in (for dominance safety at flush time). Registered by the additive /
    // operator lowering, removed when a named local or a `move string` parameter claims
    // ownership, and freed at end-of-full-expression (C++ temporary semantics) via
    // FlushOwnedStringTemps. See internal/string-concat-intermediate-leak.md.
    std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> pendingOwnedStringTemps;

    // Owned-closure temporaries (lambda Option A): a lambda literal owns a heap env. When it is
    // not bound to a named owner (e.g. passed by value directly as an argument), nothing frees
    // its env, so it is registered here and freed at end-of-full-expression - the closure analog
    // of pendingOwnedStringTemps. Unregistered when a decl-init / assignment / field store claims
    // it (that owner's destructor frees it). The flush is tag-gated (a non-capturing temp's null
    // env makes the free a no-op), and every closure STORE clones the env, so a callee that
    // retained a copy holds its OWN clone and never aliases the flushed temp.
    std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> pendingOwnedClosureTemps;

    // T[] array-view noalias metadata (per-function, reset in createFunctionBlock). aliasDomain_ is
    // the single anonymous alias domain for the current function; aliasScopes_ holds one anonymous
    // scope per distinct view ORIGIN (parameter / field) seen in the function body. A view's element
    // accesses are tagged !alias.scope {own scope} + !noalias {every other scope}, which proves
    // pairwise disjointness to the loop vectorizer without a runtime overlap check - the same effect
    // the `noalias` parameter attribute gives for a bare T[] arg, but reaching through a by-value
    // struct field (span<T>) where the attribute cannot. See MintAliasScope / AttachViewNoalias.
    llvm::MDNode* aliasDomain_ = nullptr;
    std::vector<llvm::MDNode*> aliasScopes_;
    std::map<std::string, int> viewScopeByOrigin_; // origin name -> index into aliasScopes_

    bool lastCallIsBonded = false;           // set when the last call returned a bonded (borrowed) value
    bool lastCallBondByAddress = false;      // set when the bond originates from a by-address lambda capture (kind A)
    std::vector<std::string> lastCallBondedSources; // bond parameter names the last call's return borrows from
    // De-duplicated capture names of the most recently evaluated lambda literal. Set in
    // ParseLambdaExpression and consumed when the lambda is bound as a call argument, so the
    // C function-pointer diagnostic can name exactly what a capturing lambda captured. Uses the
    // compiler-level channel (like lastCallBondedSources) because the lambda's NamedVariable is
    // reduced to its value before reaching the argument list - the MainListener-level
    // lastLambdaType side-channel is cleared too early by intervening postfix processing.
    std::vector<std::string> lastCallLambdaCaptureNames;
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
        if (diagnosticSink_)
        {
            // LSP mode: the error is delivered to the editor over the sink (JSON-RPC).
            // Don't also echo to cout - in LSP mode cout is redirected to stderr, so the
            // echo is pure noise that duplicates the diagnostic already sent to the client.
            diagnosticSink_(sourceFileName, currentLine, currentColumn, message, 1);
            throw CompilerAbortException{ message, sourceFileName, currentLine, currentColumn };
        }
        std::cout << std::format("{}({},{}): {}\n", sourceFileName, currentLine, currentColumn, message);
        if (!expectedError.empty())
        {
            if (message.find(expectedError) != std::string::npos)
            {
                std::cout << "PASS: expected error received\n";
                throw ExpectedErrorReceived{};
            }
            std::cout << std::format("FAIL: expected error '{}' but got '{}'\n", expectedError, message);
            FailCompilation(message);
        }
        FailCompilation(message);
    }

    // Terminate the current compilation. In batch / --check mode this throws so
    // the batch loop can record the failure and continue with the next file;
    // otherwise it preserves the historical hard exit(1).
    [[noreturn]] void FailCompilation(const std::string& message) const
    {
        if (batchMode_)
            throw CompilerAbortException{ message, sourceFileName, currentLine, currentColumn };
        exit(1);
    }

    void LogWarning(std::string message) const
    {
        if (diagnosticSink_)
        {
            // LSP mode: forward warnings to the editor as warning-severity diagnostics.
            // Previously these went only to cout (-> stderr), so they were both noise on
            // stderr and invisible in the editor. Routing through the sink fixes both.
            diagnosticSink_(sourceFileName, currentLine, currentColumn, message, 2);
            return;
        }
        std::cout << std::format("{}({},{}): warning: {}\n", sourceFileName, currentLine, currentColumn, message);
    }

    friend class MainListener;
    friend class ForwardRefScanner;
    friend class CrossThreadEscapeScanner;

public:
    // True if a field is thread-safe by construction so it should NOT be reported by the
    // cross-thread scan: explicitly guarded by a lock (GuardedBy) or an atomic wrapper
    // type (lowered struct name atomic__i32 / atomic_counter / atomic_flag / ...). At the
    // most aggressive level (--xthread-scan 3) atomics no longer suppress, since
    // default-ordering atomics are surveyed too.
    bool FieldSatisfiesThreadDiscipline(const TypeAndValue& field) const
    {
        if (!field.GuardedBy.empty())
            return true;
        if (xthreadScanLevel_ < 3 && field.TypeName.rfind("atomic", 0) == 0)
            return true;
        return false;
    }

private:
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
    // Function-type aliases (`using Cb = function<R(Args)>;`). A closure type carries a call
    // signature, not a plain type name, so it cannot live in the string-shaped typeAliases; the
    // resolved TypeAndValue (IsFunctionPointer + return + params, TypeName "__closure_fat_ptr") is
    // stored here and expanded wherever the alias name is used as a type (ParseDeclarationSpecifiers).
    std::unordered_map<std::string, TypeAndValue> functionTypeAliases;
    // Typedef name -> type spelling from C/header extraction. Populated by AdoptRawTypedefs
    // so the function-signature mapper can chase HANDLE -> void*, SOCKET -> uintptr_t, etc.
    // (a fallback; canonical spellings from the extractor already resolve most chains).
    // Process-wide; first-writer-wins.
    std::unordered_map<std::string, std::string> cTypedefMap_;
    std::unordered_map<std::string, std::vector<FunctionSymbol>> functionTable;
    std::unordered_map<std::string, std::vector<InterfaceMethod>> interfaceTable;
    std::unordered_map<std::string, std::vector<std::string>> interfaceParents;
    std::unordered_map<std::string, llvm::Constant*> stringPool;
    std::unordered_set<std::string> namespaceTable;
    // Namespace whose body is currently being code-generated (e.g. "N" or "Outer.Inner").
    // Empty at file scope. Set/restored around a namespace member's body so that an
    // unqualified sibling reference (e.g. bare "helper" inside "N") resolves to the
    // enclosing-namespace symbol "N.helper". See ResolveQualifiedName.
    std::string currentNamespace_;
    // Maps import alias name -> set of unqualified symbol names the file contributed.
    // Populated by CompileImportedFile when namespaceName is non-empty.
    std::unordered_map<std::string, std::unordered_set<std::string>> importAliasMembers;
    std::unordered_set<std::string> importedFiles;
    std::vector<std::string> importStack;  // DFS stack for circular import detection
    std::string importSearchDir;
    std::string runtimeDir;
    std::string sourceFileDir_;  // original source dir for LSP temp-file analysis
    // Display name to use in diagnostics in place of the analyzed file's own name.
    // The LSP analyzes a temp copy (cflat_lsp_*.cb); without this, every diagnostic and
    // the __FILE__ macro would carry the temp name instead of the real source file.
    std::string sourceDisplayName_;

    // Per-backend generic-template state, shared with MainListener via references.
    GenericTemplateState gts;
private:
    // When true, disables auto-import of core/runtime.cb
    bool skipRuntimeImport = false;
    bool verbose = false;
    // When true (--check / batch mode), a fatal compile error throws instead of
    // calling exit(1), so the batch loop can record the failure and move on.
    bool batchMode_ = false;
    bool noCache_ = false;
    bool cHeaderCacheDeep_ = false;  // --c-header-cache-deep: transitive validation of cached C headers
    // --cpu: target CPU passed to createTargetMachine (the -mcpu-style knob: sets default
    // ISA features + scheduling model). Empty means the platform default (x86-64 / i686).
    // --tune: tune-only CPU (the -mtune knob: scheduling model only, no change to the
    // legal instruction set); applied as the per-function "tune-cpu" attribute.
    // Both are resolved ("native" -> host CPU) and validated in Compile, so EmitExecutable
    // and the C interop path can use them verbatim.
    std::string targetCpu_;
    std::string tuneCpu_;
    int platformValue = 64;  // 64 for win64, 32 for win32
    // C interop: temp .obj files produced by clang-cl from .c inputs, linked
    // into the final image by EmitExecutable and then deleted.
    std::vector<std::string> cObjectFiles_;
    int cOptLevel_ = 0;        // optimization level applied to clang C compiles
    bool cDebugInfo_ = false;  // emit CodeView for clang C compiles
    // --asan: instrument with AddressSanitizer and link the compiler-rt asan runtime.
    // Intended to be paired with -g so the runtime report can name CFlat source lines.
    // Off by default; when off, codegen/linking is byte-for-byte unchanged.
    bool asan_ = false;

    // --run: JIT and execute in-process instead of emitting an exe. jitExitCode_ carries the
    // program's exit status back to main() (Compile returns bool for compile success only).
    // runArgs_ are the program arguments (everything after a `--` on the command line),
    // forwarded to an 'int main(int argc, char** argv)' entry as argv[1..]; argv[0] is the
    // source file name. Empty for an 'int main()' entry.
    bool runMode_ = false;
    int  jitExitCode_ = 0;
    std::vector<std::string> runArgs_;

    // Cross-thread sharing scan state. xthreadScanLevel_ is 0 unless --xthread-scan N
    // (N in 1..3) was passed; threadSharedTypes_ holds the struct type names found
    // escaping across a thread-spawn boundary (populated by ScanCrossThreadEscapes before
    // the codegen walk; consumed at the two field-access check sites). Findings are printed
    // to stdout with an [xthread] prefix (not the diagnostic sink). Cleared by ResetForReanalysis.
    int xthreadScanLevel_ = 0;
    std::unordered_set<std::string> threadSharedTypes_;

    // Structured facts about each `vectorize` loop, gathered at codegen time (when
    // the AST is available) and consulted by the post-optimization enforcement scan
    // to produce a precise, well-located failure message. Matched to a failed loop
    // by source line (which also travels in the loop's `!llvm.loop` metadata).
    // Non-empty gates enforcement at -O2. Cleared by ResetForReanalysis.
    std::vector<VectorizeLoopInfo> vectorizeLoops_;
    // Dedupe set for the [xthread] cross-thread sharing report (see xthreadScanLevel_
    // / threadSharedTypes_ above): each distinct finding line prints at most once.
    // Cleared by ResetForReanalysis.
    std::unordered_set<std::string> xthreadReported_;
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
    // An externally-linkable C global variable - a header `extern int x;` declaration or a
    // .c-defined global. Bound as a declaration-only, mutable CFlat global (external reference
    // resolved by the linker against the C library / object). ret carries the mapped CFlat type.
    struct CGlobalEntry
    {
        std::string name;
        TypeAndValue type;
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
        double floatValue = 0.0;
        std::string file;
        int line = 1;
        int col = 0;
        // True when the macro's natural type (recovered by Pass B's __typeof__ probe)
        // is a void* (directly or via a typedef chain like HANDLE -> void *). Such
        // macros are sentinel pointers (INVALID_HANDLE_VALUE, MAP_FAILED, ...) and
        // RegisterCMacros emits them as void* globals so they can be compared against
        // pointer-returning C APIs.
        bool isPointer = false;
        // True when the macro's natural type is float / double / long double. The
        // double-fold probe (__cflat_f_NAME) supplies the value; RegisterCMacros
        // emits the macro as a CFlat `double` global. Routing on type (not on a
        // successful double-fold alone) is intentional: `(double)(0x100000000)`
        // folds fine but the macro is an integer constant, not a float.
        bool isFloat = false;
        // True when the macro expands to a string literal (e.g. #define VERSION "1.2.3").
        // stringValue holds the decoded characters; RegisterCMacros interns it via
        // CreateGlobalString and emits a `char*` global. Routed via Pass B's
        // __typeof__ probe so `(int)"x"` stays on the integer path.
        bool isString = false;
        std::string stringValue;
        // CFlat integer type recovered from the macro's natural C type (e.g.
        // ((DWORD)-10) -> "u32"). Empty when the natural type is unknown or not a
        // plain integer; RegisterCMacros then falls back to width-guessing from the
        // value. Carrying the real type lets a call site match the header's
        // parameter type without an explicit cast (GetStdHandle(STD_INPUT_HANDLE)).
        std::string intTypeName;
        // True when the macro's natural type is a C function-pointer (e.g.
        // #define SIG_IGN ((void(*)(int))1)). value carries the folded integer
        // bit pattern; funcPtrTV carries the parsed signature. RegisterCMacros
        // emits a function<R(P...)> global initialized to a constant fat struct
        // {thunk, env=intToPtr(value)} - same wire as a C-returned fn ptr.
        bool isFuncPtr = false;
        TypeAndValue funcPtrTV;
    };
    // A C struct/union decl extracted from a bound header or source. Field types are kept as
    // raw C type spellings (qualType) so they can be re-resolved against dataStructures after
    // all records in the same TU are registered (handles forward references between structs).
    struct CRecordFieldEntry
    {
        std::string name;
        std::string ctype;
        // Bitfield support. When isBitfield is true, the C field is a bitfield
        // and bitWidth holds the user-written width (1..N). bitOffset is the
        // LSB-first offset within the containing storage unit, as resolved by
        // RegisterCRecords using MSVC ABI rules; we do NOT rely on clang's
        // reported offset since CFlat replicates the layout itself.
        bool isBitfield = false;
        unsigned bitWidth = 0;
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
    // An object-like macro name discovered during the header parse, awaiting value resolution
    // (the resolve pass folds each name's integer / float / string / pointer value).
    struct CMacroNameCand
    {
        std::string name;
        std::string file;
        int line = 1;
    };
    // One transitively-included file recorded for deep (transitive) disk-cache validation.
    struct CHeaderDep
    {
        std::string path;
        int64_t  mtime = 0;  // file_time_type::time_since_epoch().count()
        uint64_t hash  = 0;  // FNV-1a of file contents (checked only on mtime drift)
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
        std::vector<CGlobalEntry> globals;
        // `typedef struct Tag {...} Name;` aliases (Name -> Tag) surfaced from a header bind so the
        // user can write the typedef name (MSG) instead of the bare tag (tagMSG). Cached so a
        // disk-cache hit replays them; empty for the .c path.
        std::vector<std::pair<std::string, std::string>> recordAliases;
        // Populated only for deep-mode disk-cache entries; empty otherwise (shallow validation).
        std::vector<CHeaderDep> deps;
    };
    static inline std::mutex cFileSigCacheMutex_;
    // Key: canonical .c path, or for bound headers "<canonical .h>|<include dirs>" so the
    // same header under different --c-include roots does not collide.
    static inline std::unordered_map<std::string, CFileSigCacheEntry> cFileSigCache_;
    int lambdaCounter = 0;
    int pipeStreamCounter = 0;   // uniquifies synthesized hidden `stream` locals for `producer >> consumer` piping
    std::string expectedError;
    size_t expectedErrorScopeDepth = SIZE_MAX;  // SIZE_MAX = scoped block form (checked manually after block); else stackNamedVariable depth for bare-semicolon form
    // File-scope bare-semicolon expect_error("..."); armed before ProcessImports so it can
    // catch import-time diagnostics (e.g. the orphan-header error). Non-empty while pending;
    // a match throws ExpectedErrorReceived (caught in Compile), an unmatched one at end of
    // compilation is the did-not-occur failure. See Compile() in LLVMBackend.cpp.
    std::string fileScopeExpectedError_;

    // Compile-time macros (constant throughout compilation, set early)
    struct CompileTimeMacro
    {
        std::string name;
        llvm::Constant* value;
        std::string type;  // "int", "string", etc.
    };
    std::unordered_map<std::string, CompileTimeMacro> compileTimeMacros;
    // Parse-tree cache for imported files. The ANTLR ecosystem (input/lexer/tokens/
    // parser) is kept alive so generic-template ctx pointers stay valid, AND the whole
    // entry is reused across compiles when the file's content is unchanged - parsing the
    // standard-library closure (runtime.cb + transitive core imports) is by far the most
    // expensive part of a small compile, so amortizing it across batch --check files and
    // LSP re-analyses is a large win. The cache deliberately survives ResetForReanalysis.
    //
    // Per-backend (not process-global): an LSP backend slot is only ever used by one
    // worker thread at a time, so no locking is needed and walked trees are never shared
    // across threads.
    struct CachedParseTree
    {
        std::string canonicalPath;                 // absolute canonical path
        std::filesystem::file_time_type writeTime; // for staleness validation
        std::unique_ptr<antlr4::ANTLRInputStream> input;
        std::unique_ptr<CFlatLexer> lexer;
        std::unique_ptr<antlr4::CommonTokenStream> tokens;
        std::unique_ptr<CFlatParser> parser;
        CFlatParser::CompilationUnitContext* unit = nullptr;  // owned by `parser`
    };
    // Persistent cache, keyed by canonical path - populated ONLY for implicit core-library
    // imports (files under runtimeDir/core), whose content is stable for the process. User
    // imports are deliberately excluded: they change during editing, and bounding the cache
    // to core keeps staleness handling trivial.
    std::unordered_map<std::string, std::unique_ptr<CachedParseTree>> parseTreeCache_;
    // Per-compile lifetime anchor for NON-core imported parse trees (generic-template ctx
    // pointers point into them). Cleared by ResetForReanalysis; parseTreeCache_ is not.
    std::vector<std::unique_ptr<CachedParseTree>> importedParseStates;

    // Parse `canonicalPath` into a CFlat tree. When `isCore` is true the tree is reused
    // from / stored in parseTreeCache_ (validated by last-write time); otherwise it is
    // parsed fresh and owned by importedParseStates for this compile only. Returns nullptr
    // (and reports diagnostics) on a parse error; a failed parse is never cached.
    // `displayName` is used in TimeTraceScope/logs.
    CachedParseTree* GetOrParseFile(const std::string& canonicalPath, const std::string& displayName, bool isCore);
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
    bool closureLifetimeRegistered = false;   // closure dtor + copy registered (lazy; needs function.cb)
    // Memoized "full destructor" per type: user dtor + recursive member destruction.
    // Presence in the map means "computed"; the mapped value may be nullptr (type needs
    // no destruction), the plain user dtor (no dtor-bearing members), or a synthesized
    // wrapper. See GetOrCreateFullDestructor.
    std::unordered_map<std::string, llvm::Function*> fullDestructorCache_;
    std::unordered_set<std::string> fullDestructorInProgress_;
    std::unordered_map<std::string, llvm::Function*> memberwiseCopyCache_;
    std::function<void(const std::string&, size_t, size_t, const std::string&, int)> diagnosticSink_;
    std::function<void(int, int, int, int, const std::string&)> hintRegionSink_;
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

        // CFlat treats null pointer dereferences as defined behavior (hardware fault -> SEH).
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

    // True when the named variable is a string local/param that currently OWNS its heap buffer
    // (its scope-exit destructor will free it). Lets the call path tell a plain owned-string
    // VARIABLE read apart from a borrowed one, so passing it as a `move string` argument
    // transfers the buffer instead of triggering the defensive heap-copy in
    // CreateOverloadedFunctionCall (which would clone the buffer and orphan the source's - a leak).
    bool IsVariableOwningString(const std::string& name) const
    {
        if (name.empty()) return false;
        for (const auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                return it->second.IsOwningString;
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                return it->second.IsOwningString;
        }
        return false;
    }

    // True when a string local was tainted as borrowing an owning string FIELD (e.g.
    // `string t = b.name`). Storing such an alias into another field launders a
    // field-to-field copy and double-frees, so the field-store path rejects it.
    bool IsVariableBorrowingOwnedString(const std::string& name) const
    {
        if (name.empty()) return false;
        for (const auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                return it->second.BorrowsOwnedString;
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                return it->second.BorrowsOwnedString;
        }
        return false;
    }

    // Set/clear the field-borrow taint on a string local (used at declaration-with-initializer
    // and on reassignment, mirroring MarkVariableOwningString).
    void SetVariableBorrowsOwnedString(const std::string& name, bool value)
    {
        if (name.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                { it->second.BorrowsOwnedString = value; return; }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                { it->second.BorrowsOwnedString = value; return; }
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

        // Call the full destructor (user dtor + member fields) if the type needs one.
        if (auto* dtor = GetOrCreateFullDestructor(namedVar.TypeAndValue.TypeName))
            builder->CreateCall(dtor->getFunctionType(), dtor, { ptrVal });

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

    // Record an owned-string SSA temporary for end-of-full-expression cleanup. Called
    // by the operator lowering when an operator+ (ReturnsOwned string) result is NOT
    // bound to a named local. `value` must be the %string struct value the call returned.
    void RegisterOwnedStringTemp(llvm::Value* value)
    {
        if (value == nullptr) return;
        pendingOwnedStringTemps.emplace_back(value, builder->GetInsertBlock());
    }

    // Remove an owned-string temporary from the pending-cleanup list because some other
    // construct has taken ownership of it: a named local (its destructor will free it on
    // scope exit) or a `move string` parameter (the callee frees it). Prevents a double
    // free of the same buffer.
    void UnregisterOwnedStringTemp(llvm::Value* value)
    {
        if (value == nullptr) return;
        std::erase_if(pendingOwnedStringTemps,
            [&](const std::pair<llvm::Value*, llvm::BasicBlock*>& e) { return e.first == value; });
    }

    // Free every owned-string temporary still pending at the end of a full expression,
    // mirroring C++ temporary lifetime (destroyed at the semicolon). Only temporaries
    // created in the CURRENT basic block are freed - one created across a branch (e.g. in
    // a loop condition) would not dominate this insert point, so it is dropped rather than
    // emitted as invalid IR (a rare leak, documented in the bug note). The list is always
    // cleared so temporaries never carry across statements. No-op if the current block is
    // already terminated (e.g. after a return emitted the frees itself).
    void FlushOwnedStringTemps()
    {
        if (pendingOwnedStringTemps.empty()) return;

        auto* curBlock = builder->GetInsertBlock();
        bool terminated = curBlock != nullptr && curBlock->getTerminator() != nullptr;
        if (!terminated)
        {
            auto* strTy = llvm::StructType::getTypeByName(*context, "string");
            for (auto& [value, bb] : pendingOwnedStringTemps)
            {
                if (value == nullptr || bb != curBlock) continue;     // dominance safety
                if (strTy == nullptr || value->getType() != strTy) continue;
                EnsureStringDtorRegistered();
                auto it = dataStructures.find("string");
                if (it == dataStructures.end() || it->second.Destructor == nullptr) continue;
                // The destructor takes a string*; spill the SSA value to an entry-block
                // alloca (never a loop body - see AllocaAtEntry) and free through it.
                auto* tmp = AllocaAtEntry(strTy, nullptr, "concat.tmp");
                builder->CreateStore(value, tmp);
                builder->CreateCall(it->second.Destructor->getFunctionType(),
                                    it->second.Destructor, { tmp });
            }
        }
        pendingOwnedStringTemps.clear();
    }

    void RegisterOwnedClosureTemp(llvm::Value* value)
    {
        if (value == nullptr) return;
        pendingOwnedClosureTemps.emplace_back(value, builder->GetInsertBlock());
    }

    void UnregisterOwnedClosureTemp(llvm::Value* value)
    {
        if (value == nullptr) return;
        std::erase_if(pendingOwnedClosureTemps,
            [&](const std::pair<llvm::Value*, llvm::BasicBlock*>& e) { return e.first == value; });
    }

    // Free every owned-closure temporary still pending at end-of-full-expression (mirrors
    // FlushOwnedStringTemps): a lambda literal whose env was never claimed by a named owner.
    // Only temporaries created in the CURRENT block are freed (dominance safety); the list is
    // always cleared so nothing carries across statements. The closure dtor is tag-gated, so a
    // non-capturing temp (null env) frees nothing.
    void FlushOwnedClosureTemps()
    {
        if (pendingOwnedClosureTemps.empty()) return;

        auto* curBlock = builder->GetInsertBlock();
        bool terminated = curBlock != nullptr && curBlock->getTerminator() != nullptr;
        if (!terminated)
        {
            auto* closureTy = GetClosureFatPtrType();
            auto* dtor = GetOrCreateFullDestructor("__closure_fat_ptr");
            if (dtor != nullptr)
            {
                for (auto& [value, bb] : pendingOwnedClosureTemps)
                {
                    if (value == nullptr || bb != curBlock) continue;     // dominance safety
                    if (value->getType() != closureTy) continue;
                    // The dtor takes a __closure_fat_ptr*; spill the SSA value to an entry-block
                    // alloca (never a loop body - see AllocaAtEntry) and free through it.
                    auto* tmp = AllocaAtEntry(closureTy, nullptr, "closure.tmp");
                    builder->CreateStore(value, tmp);
                    builder->CreateCall(dtor->getFunctionType(), dtor, { tmp });
                }
            }
        }
        pendingOwnedClosureTemps.clear();
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
                // String destructor is only called when the local owns its buffer; it has
                // no dtor-bearing members so the registered string dtor is the full dtor.
                if (namedVar.TypeAndValue.TypeName == "string")
                {
                    if (!namedVar.IsOwningString) continue;
                    EnsureStringDtorRegistered();
                    if (it->second.Destructor == nullptr) continue;
                    auto* fn = it->second.Destructor;
                    builder->CreateCall(fn->getFunctionType(), fn, { namedVar.Storage });
                    continue;
                }
                // Non-string struct local: run the full destructor (user dtor + members).
                // Skip the struct value being moved out via `return` - the caller now owns it.
                if (namedVar.Storage == returnedStructDtorSkipAlloca) continue;
                if (auto* fn = GetOrCreateFullDestructor(namedVar.TypeAndValue.TypeName))
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
                if (auto* dtor = GetOrCreateFullDestructor(namedVar.TypeAndValue.TypeName))
                    builder->CreateCall(dtor->getFunctionType(), dtor, { namedVar.Storage });
            }
        }

        // Release lock held by this scope (lock statement).
        if (frame.lockCleanup.has_value())
        {
            auto& lc = frame.lockCleanup.value();
            builder->CreateCall(lc.UnlockFn->getFunctionType(), lc.UnlockFn, { lc.MutexPtr });
        }
    }

    // Mint a fresh alias scope for one view origin in the current function and return its index into
    // aliasScopes_. The anonymous alias domain is created lazily on first use. The returned index is
    // stored on the view's TypeAndValue::NoaliasScopeId so every access derived from the same origin
    // (and any copy that shares the id) tags the same scope.
    int MintAliasScope()
    {
        llvm::MDBuilder mdb(*context);
        if (aliasDomain_ == nullptr)
            aliasDomain_ = mdb.createAnonymousAliasScopeDomain("cflat.view");
        auto* scope = mdb.createAnonymousAliasScope(aliasDomain_, "cflat.view.scope");
        aliasScopes_.push_back(scope);
        return (int)aliasScopes_.size() - 1;
    }

    // Tag a load/store with the noalias provenance of a view: !alias.scope = { its own scope } and
    // !noalias = { every OTHER live scope in this function }. With each origin in its own scope, the
    // optimizer can prove any two distinct-origin views do not alias - dropping the runtime overlap
    // check the loop vectorizer would otherwise insert.
    //
    // The alias.scope is set unconditionally (even when no sibling exists yet): scopes are minted
    // lazily as origins are first indexed, so an access emitted before its sibling's scope exists
    // would otherwise be invisible to that later sibling's !noalias list. Always stamping the
    // alias.scope makes the disambiguation order-independent - a later access whose !noalias names
    // this scope proves disjointness against this access regardless of which was emitted first. The
    // !noalias list is only attached when at least one other scope currently exists.
    void AttachViewNoalias(llvm::Instruction* memInst, int scopeId)
    {
        if (memInst == nullptr || scopeId < 0 || scopeId >= (int)aliasScopes_.size())
            return;
        std::vector<llvm::Metadata*> others;
        for (int i = 0; i < (int)aliasScopes_.size(); ++i)
            if (i != scopeId)
                others.push_back(aliasScopes_[i]);
        memInst->setMetadata(llvm::LLVMContext::MD_alias_scope,
            llvm::MDNode::get(*context, { aliasScopes_[scopeId] }));
        if (!others.empty())
            memInst->setMetadata(llvm::LLVMContext::MD_noalias, llvm::MDNode::get(*context, others));
    }

    // Return the alias-scope index for a view ORIGIN (a parameter name, local name, or
    // struct-instance name for a field view), minting one on first sight. Keyed by the origin
    // STRING - so every access derived from the same origin (and any copy that names the same
    // origin) shares a scope and is therefore correctly treated as possibly-aliasing itself, while
    // two distinct origins land in distinct scopes and are proven disjoint. An empty key returns -1
    // (no stable provenance -> no metadata; a bare T[] parameter still relies on its noalias attr).
    int GetOrMintViewScope(const std::string& originKey)
    {
        if (originKey.empty())
            return -1;
        auto it = viewScopeByOrigin_.find(originKey);
        if (it != viewScopeByOrigin_.end())
            return it->second;
        int id = MintAliasScope();
        viewScopeByOrigin_.emplace(originKey, id);
        return id;
    }

    void createFunctionBlock(llvm::Function* fn, const std::string& friendlyName, std::vector<LLVMBackend::TypeAndValue> arguments, bool returnsOwned = false, bool returnIsArrayView = false, const std::string& returnTypeName = "")
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
        currentFunctionReturnIsArrayView = returnIsArrayView;
        currentFunctionReturnTypeName = returnTypeName;
        // Reset the per-function alias-scope registry; scopes from a prior function must never leak.
        aliasDomain_ = nullptr;
        aliasScopes_.clear();
        viewScopeByOrigin_.clear();

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

        // string.data() / string.length() / string.hash() are now LIBRARY functions defined
        // in core/string.cb (string-redesign: `string` as a library type). The compiler keeps
        // only the bare {i8*,i32} layout + default ctor here for bootstrap (literal typing /
        // __FILE__) and lowers literals via WrapStringLiteralAsString; the observable methods
        // live in cflat. The compiler never calls these three internally (it inlines field
        // access in the interpolation/%s path), so they resolve fine for every consumer, all
        // of which import string.cb.

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
            // The concatenated buffer is freshly malloc'd: set _len's high OWNED bit so the
            // result owns it (string-redesign FINAL MODEL). length() masks the bit back off.
            auto* ownedLen = rb.CreateOr(rb.CreateLoad(i32Ty, totalA), rb.getInt32(0x80000000));
            strVal = rb.CreateInsertValue(strVal, ownedLen, { 1u });
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

        // Pre-register the closure fat type { i8* code, i8* env } as an owning value type
        // (lambda Option A). The dtor + copy are registered lazily once function.cb's env
        // primitives are available (EnsureClosureLifetimeRegistered).
        RegisterBuiltinClosure();
    }

    // Register the closure backing type `__closure_fat_ptr` { i8* code, i8* env } as a
    // dataStructures entry so a `function<R(Args)>` value (which carries this TypeName)
    // is recognized as an owning value type. The destructor (frees the heap env) and the
    // env-cloning copy() are added lazily by EnsureClosureLifetimeRegistered() once the
    // core/function.cb primitives have been compiled.
    void RegisterBuiltinClosure()
    {
        auto* closureTy = GetClosureFatPtrType();

        DeclTypeAndValue codeField;
        codeField.TypeName = "i8";
        codeField.VariableName = "code";
        codeField.Pointer = true;

        DeclTypeAndValue envField;
        envField.TypeName = "i8";
        envField.VariableName = "env";
        envField.Pointer = true;

        dataStructures["__closure_fat_ptr"].StructType = closureTy;
        dataStructures["__closure_fat_ptr"].StructFields = { codeField, envField };
    }

    // Lazily register the closure destructor and the env-cloning copy() once the
    // core/function.cb env primitives are in the module. Mirrors EnsureStringDtorRegistered:
    // a capturing closure owns its heap env, and these two functions are what the value-type
    // machinery calls to free it at scope exit / struct teardown (dtor) and to clone it on
    // copy (copy). A borrowed/null env is a runtime no-op inside the primitives.
    void EnsureClosureLifetimeRegistered()
    {
        if (closureLifetimeRegistered) return;

        // Resolve the library primitives. If function.cb has not been compiled yet (e.g. a
        // closure appears in a core file imported before it), defer - try again on next use.
        auto* freeFn  = GetFunction("__closure_env_free");
        auto* cloneFn = GetFunction("__closure_env_clone");
        if (!freeFn || !cloneFn) return;
        closureLifetimeRegistered = true;

        auto* closureTy = GetClosureFatPtrType();
        auto* i8PtrTy   = builder->getInt8Ty()->getPointerTo();
        auto* voidTy    = llvm::Type::getVoidTy(*context);

        // Destructor: free the heap env (no-op if borrowed/null) and null the field.
        {
            auto* dtorTy = llvm::FunctionType::get(voidTy, { closureTy->getPointerTo() }, false);
            auto* dtorFn = llvm::Function::Create(dtorTy, llvm::Function::InternalLinkage,
                                                  "__closure_fat_ptr.dtor", *module);
            dtorFn->arg_begin()->setName("self");
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(*context, "entry", dtorFn));
            auto* self   = &*dtorFn->arg_begin();
            auto* envPtr = b.CreateStructGEP(closureTy, self, 1, "envfield");
            auto* env    = b.CreateLoad(i8PtrTy, envPtr, "env");
            b.CreateCall(freeFn->getFunctionType(), freeFn, { env });
            b.CreateStore(llvm::ConstantPointerNull::get(i8PtrTy), envPtr);
            b.CreateRetVoid();
            RegisterDestructor("__closure_fat_ptr", dtorFn);
        }

        // copy(__closure_fat_ptr self): clone the heap env so the copy owns an independent
        // block (code pointer unchanged). Registered in functionTable["copy"] so the value-
        // type copy machinery resolves it (and HasCopyOverloadFor short-circuits the
        // memberwise synth, which would shallow-copy the env pointer and double-free).
        {
            auto* copyTy = llvm::FunctionType::get(closureTy, { closureTy }, false);
            auto* copyFn = llvm::Function::Create(copyTy, llvm::Function::InternalLinkage,
                                                  "__closure_fat_ptr.copy", *module);
            copyFn->arg_begin()->setName("self");
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(*context, "entry", copyFn));
            auto* self   = &*copyFn->arg_begin();
            auto* code   = b.CreateExtractValue(self, { 0u }, "code");
            auto* env    = b.CreateExtractValue(self, { 1u }, "env");
            auto* newEnv = b.CreateCall(cloneFn->getFunctionType(), cloneFn, { env }, "clonedenv");
            llvm::Value* fat = llvm::UndefValue::get(closureTy);
            fat = b.CreateInsertValue(fat, code,   { 0u });
            fat = b.CreateInsertValue(fat, newEnv, { 1u });
            b.CreateRet(fat);

            FunctionSymbol sym;
            sym.UniqueName = "__closure_fat_ptr.copy";
            sym.Function   = copyFn;
            sym.ReturnType = TypeAndValue{ "__closure_fat_ptr", "", false };
            sym.ReturnType.IsMove = true;   // the fresh clone is owned by the caller
            sym.Parameters = { TypeAndValue{ "__closure_fat_ptr", "self", false } };
            functionTable["copy"].push_back(sym);
        }
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

        auto* i32Ty   = b.getInt32Ty();
        auto* self    = &*dtorFn->arg_begin();
        auto* ptrPtr  = b.CreateStructGEP(strTy, self, 0, "ptrfield");
        auto* lenPtr  = b.CreateStructGEP(strTy, self, 1, "lenfield");
        auto* ptr     = b.CreateLoad(ptrTy, ptrPtr, "ptr");
        auto* len     = b.CreateLoad(i32Ty, lenPtr, "len");
        auto* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
        // Free iff this string OWNS its buffer (string-redesign FINAL MODEL): _len's high
        // bit is the OWNED flag. Borrowed strings (literals, char* wraps, views; owned bit
        // clear) are never freed. This makes the dtor safe to call on ANY string - including
        // a struct/container `string` member, where the compiler has no static ownership info
        // and must rely on the runtime bit.
        auto* isOwned   = b.CreateICmpNE(
            b.CreateAnd(len, b.getInt32(0x80000000)), b.getInt32(0), "is_owned");
        auto* isNotNull = b.CreateICmpNE(ptr, nullPtr, "is_not_null");
        auto* shouldFree = b.CreateAnd(isOwned, isNotNull, "should_free");
        auto* freeBlock = llvm::BasicBlock::Create(*context, "free", dtorFn);
        auto* doneBlock = llvm::BasicBlock::Create(*context, "done", dtorFn);
        b.CreateCondBr(shouldFree, freeBlock, doneBlock);
        b.SetInsertPoint(freeBlock);
        b.CreateCall(freeFn->getFunctionType(), freeFn, { ptr });
        b.CreateBr(doneBlock);
        b.SetInsertPoint(doneBlock);
        b.CreateStore(nullPtr, ptrPtr);
        b.CreateStore(b.getInt32(0), lenPtr);
        b.CreateRetVoid();

        RegisterDestructor("string", dtorFn);
    }

    // Return the function that fully destroys an instance of `typeName`: the user
    // destructor (if any) followed by destruction of every destructor-bearing member
    // field, recursively. Returns nullptr when the type needs no destruction at all
    // (no user dtor and no dtor-bearing members). Result is memoized.
    //
    // Member scope: a member is destructed only when it is a VALUE field (not a pointer,
    // array-view, simd, bitfield, or fixed-array) whose type is a struct with its own
    // full destructor (list<T>, dictionary<T>, array<T>, user structs with `~T()`, and
    // nested aggregates of these), INCLUDING bare `string` members. `string` now carries a
    // runtime owned flag (the high bit of _len, string-redesign FINAL MODEL), so the string
    // dtor frees an owning member and leaves a borrowed one (literal/view) untouched - it is
    // safe to auto-destruct. Raw pointer fields are never auto-freed.
    //
    // When a type has a user dtor but no dtor-bearing members, the user dtor IS the full
    // destructor (no wrapper is synthesized) - the common case stays byte-identical.
    llvm::Function* GetOrCreateFullDestructor(const std::string& typeName)
    {
        // `string` has no dtor-bearing members, so its full dtor is just the lazily
        // registered string dtor. Resolve it live (never cache) so a call before the
        // lazy registration does not poison the cache with a null.
        if (typeName == "string")
        {
            EnsureStringDtorRegistered();
            auto it = dataStructures.find("string");
            return it != dataStructures.end() ? it->second.Destructor : nullptr;
        }

        // The closure fat type's full dtor is the lazily-registered env-freeing dtor (lambda
        // Option A). Resolve live (never cache a pre-registration null), like `string`.
        if (typeName == "__closure_fat_ptr")
        {
            EnsureClosureLifetimeRegistered();
            auto it = dataStructures.find("__closure_fat_ptr");
            return it != dataStructures.end() ? it->second.Destructor : nullptr;
        }

        // Only synthesized wrappers are cached - they are stable once built. The
        // no-member-work and unknown-type results are resolved LIVE on every call so a
        // destructor that is still a forward declaration when first queried (e.g. a
        // generic ~list instantiation whose body is emitted later) is not frozen as a
        // stale null. This mirrors the pre-existing direct `Destructor` reads at each site.
        if (auto it = fullDestructorCache_.find(typeName); it != fullDestructorCache_.end())
            return it->second;

        auto dsIt = dataStructures.find(typeName);
        if (dsIt == dataStructures.end())
            return nullptr;

        // Value-type member cycles are impossible (infinite size), but guard defensively
        // so a malformed registry cannot recurse forever - fall back to the user dtor.
        if (!fullDestructorInProgress_.insert(typeName).second)
            return dsIt->second.Destructor;

        llvm::Function* userDtor = dsIt->second.Destructor;

        // Collect member fields that need destruction.
        struct MemberWork { unsigned Index; llvm::Function* Dtor; };
        std::vector<MemberWork> work;
        for (unsigned i = 0; i < dsIt->second.StructFields.size(); ++i)
        {
            const auto& f = dsIt->second.StructFields[i];
            if (f.Pointer || f.ElemPointer || f.IsArrayView || f.IsSimd || f.IsBitfield)
                continue;
            if (f.ConstArraySize > 0)   // fixed-array members: out of scope (rare), documented
                continue;
            // `string` members ARE destructed now (string-redesign FINAL MODEL): the owned
            // bit in _len tells the string dtor at runtime whether to free, so a borrowed
            // string member (literal/view; bit clear) is safely left alone while an owning
            // one is freed. This is exactly what the owned bit was added to enable.
            if (llvm::Function* childDtor = GetOrCreateFullDestructor(f.TypeName))
                work.push_back({ i, childDtor });
        }

        fullDestructorInProgress_.erase(typeName);

        if (work.empty())
        {
            // No member work: full destruction is exactly the user dtor (possibly null).
            // Resolved live (not cached) so a later-registered dtor is picked up.
            return userDtor;
        }

        // Synthesize a wrapper: user dtor first (so hand-written free-and-null logic runs
        // before member teardown), then each member's full destructor.
        auto* structTy = dsIt->second.StructType;
        auto* voidTy   = llvm::Type::getVoidTy(*context);
        auto* selfPtrTy = structTy->getPointerTo();
        auto* fnTy = llvm::FunctionType::get(voidTy, { selfPtrTy }, false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                          typeName + ".dtorfull", *module);
        fn->arg_begin()->setName("self");
        fullDestructorCache_[typeName] = fn;   // memoize before body emission

        auto* entry = llvm::BasicBlock::Create(*context, "entry", fn);
        llvm::IRBuilder<> b(entry);
        auto* self = &*fn->arg_begin();

        if (userDtor)
            b.CreateCall(userDtor->getFunctionType(), userDtor, { self });

        for (const auto& w : work)
        {
            auto* fieldPtr = b.CreateStructGEP(structTy, self, w.Index, "fld");
            b.CreateCall(w.Dtor->getFunctionType(), w.Dtor, { fieldPtr });
        }

        b.CreateRetVoid();
        return fn;
    }

    // True if `typeName` defines a no-arg instance method `copy()` (self only). This is
    // the universal `.copy()` copy-constructor hook (string-redesign Phase 1): when a
    // managed value is copied by value at a copy point (decl-init / assignment /
    // field-store), the compiler invokes copy() to produce an independent, ownership-
    // correct clone instead of a shallow struct copy that would alias owned buffers and
    // double-free at teardown. The method-receiver convention is Parameters[0] == self
    // (see the existing `Parameters[0].TypeName == typeName` checks in MainListener).
    bool HasUserCopyMethod(const std::string& typeName) const
    {
        auto it = functionTable.find("copy");
        if (it == functionTable.end())
            return false;
        for (const auto& sym : it->second)
            if (sym.IsMethod && sym.Parameters.size() == 1
                && sym.Parameters[0].TypeName == typeName)
                return true;
        return false;
    }

    // Phase-1 classifier: should a by-value copy of this type at a copy point be routed
    // through `.copy()` rather than a plain shallow store? True for a struct/class that
    // owns resources (has a full destructor) AND exposes a no-arg copy() to clone them.
    // `string` and function<> stay on their existing static-ownership path until later
    // phases, so they are excluded here.
    bool TypeNeedsManagedCopy(const std::string& typeName)
    {
        if (typeName.empty() || typeName == "string")
            return false;
        if (dataStructures.find(typeName) == dataStructures.end())
            return false;
        if (!HasUserCopyMethod(typeName))
            return false;
        return GetOrCreateFullDestructor(typeName) != nullptr;
    }

    // A struct/class VALUE type that owns resources: it has a full destructor that frees
    // at scope exit (string, list<T>, dictionary<K,V>, user classes with a `~T()`...). Per
    // the string-redesign FINAL MODEL, `b = a` of such a type MOVES by default (transfer
    // the bits + mark the source moved, use-after-move enforced) rather than aliasing (which
    // double-frees) or deep-copying; `.copy()` is the explicit way to duplicate. `string` is
    // included (it owns its buffer via the _len owned bit).
    bool IsOwningValueType(const std::string& typeName)
    {
        if (typeName.empty())
            return false;
        if (dataStructures.find(typeName) == dataStructures.end())
            return false;
        return GetOrCreateFullDestructor(typeName) != nullptr;
    }

    // True when functionTable already has a `copy` overload whose first parameter is `typeName`
    // (a user/library free `copy(T self)`, a method, or an already-synthesized memberwise copy).
    // This is the gate for implicit copy synthesis: an existing copy() always wins.
    bool HasCopyOverloadFor(const std::string& typeName) const
    {
        auto it = functionTable.find("copy");
        if (it == functionTable.end()) return false;
        for (const auto& sym : it->second)
            if (!sym.Parameters.empty() && sym.Parameters[0].TypeName == typeName)
                return true;
        return false;
    }

    // Synthesize the implicit `copy()` for a value type that does not define one: a MEMBERWISE
    // copy (the "every value type has an implicit copy() if undefined" rule). The result starts
    // as a shallow copy of `self`, then every value field whose type has its own copy() (managed
    // members - string, list<T>, dictionary, nested owning structs) is replaced with an
    // independent deep copy via that field's copy(); primitives and raw pointers stay
    // shallow-copied (the pointee is shared, matching list<T>.copy()). The result is owned
    // (`move`). Registered in functionTable["copy"] so overload resolution finds it; memoized.
    llvm::Function* GetOrCreateMemberwiseCopy(const std::string& typeName)
    {
        if (auto it = memberwiseCopyCache_.find(typeName); it != memberwiseCopyCache_.end())
            return it->second;
        auto dsIt = dataStructures.find(typeName);
        if (dsIt == dataStructures.end()) return nullptr;
        auto* structTy = dsIt->second.StructType;
        if (structTy == nullptr) return nullptr;

        // Signature: T copy(T self) - self by value (a borrow; not destructed), result by value.
        auto* fnTy = llvm::FunctionType::get(structTy, { structTy }, false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                          typeName + ".copy.synth", *module);
        fn->arg_begin()->setName("self");
        memberwiseCopyCache_[typeName] = fn;   // memoize before body (re-entrancy / recursion safe)

        // Register so overload resolution (user `x.copy()` + internal copy calls) finds it.
        {
            FunctionSymbol sym;
            sym.UniqueName = typeName + ".copy.synth";
            sym.Function   = fn;
            sym.ReturnType = TypeAndValue{ typeName, "", false };
            sym.ReturnType.IsMove = true;       // the fresh copy is owned by the caller
            sym.Parameters = { TypeAndValue{ typeName, "self", false } };
            functionTable["copy"].push_back(sym);
        }

        auto savedIP = builder->saveIP();
        auto* entry = llvm::BasicBlock::Create(*context, "entry", fn);
        builder->SetInsertPoint(entry);

        // result = self  (shallow copy of every field; managed fields are fixed up below)
        auto* resultSlot = builder->CreateAlloca(structTy, nullptr, "result");
        builder->CreateStore(&*fn->arg_begin(), resultSlot);

        // Deep-copy each managed value field, overwriting the aliased shallow handle.
        for (unsigned i = 0; i < dsIt->second.StructFields.size(); ++i)
        {
            const auto& f = dsIt->second.StructFields[i];
            if (f.Pointer || f.ElemPointer || f.IsArrayView || f.IsSimd || f.IsBitfield)
                continue;                       // pointer/view/simd/bitfield: shallow (pointee shared)
            if (f.ConstArraySize > 0)
                continue;                       // fixed-array members: out of scope (matches the dtor)
            if (!HasCopyOverloadFor(f.TypeName) && !IsOwningValueType(f.TypeName))
                continue;                       // POD field: the shallow copy is already correct
            auto* fieldPtr = builder->CreateStructGEP(structTy, resultSlot, i, "fld");
            NamedVariable argNV;
            argNV.Storage  = fieldPtr;
            argNV.BaseType = structTy->getElementType(i);
            argNV.TypeAndValue.TypeName = f.TypeName;
            if (auto* copied = CreateOverloadedFunctionCall("copy", { argNV }))
                builder->CreateStore(copied, fieldPtr);
        }

        auto* resultVal = builder->CreateLoad(structTy, resultSlot, "copyresult");
        builder->CreateRet(resultVal);
        builder->restoreIP(savedIP);
        return fn;
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

    // Emit a single-argument LLVM float intrinsic.  Works for both float (f32)
    // and double (f64) - type is inferred from floatVal.  Returns nullptr if
    // methodName is not a recognized float method.
    //
    // Supported methods and their LLVM counterparts:
    //   round      -> llvm.round       ceil       -> llvm.ceil
    //   floor      -> llvm.floor       trunc      -> llvm.trunc
    //   abs        -> llvm.fabs        rint       -> llvm.rint
    //   sqrt       -> llvm.sqrt        nearbyint  -> llvm.nearbyint
    //   sin        -> llvm.sin         exp        -> llvm.exp
    //   cos        -> llvm.cos         exp2       -> llvm.exp2
    //   log        -> llvm.log         log2       -> llvm.log2
    //   log10      -> llvm.log10
    llvm::Value* CreateFloatIntrinsic(const std::string& methodName, llvm::Value* floatVal)
    {
        llvm::Intrinsic::ID id;
        if      (methodName == "round")     id = llvm::Intrinsic::round;
        else if (methodName == "floor")     id = llvm::Intrinsic::floor;
        else if (methodName == "ceil")      id = llvm::Intrinsic::ceil;
        else if (methodName == "trunc")     id = llvm::Intrinsic::trunc;
        else if (methodName == "abs")       id = llvm::Intrinsic::fabs;
        else if (methodName == "rint")      id = llvm::Intrinsic::rint;
        else if (methodName == "nearbyint") id = llvm::Intrinsic::nearbyint;
        else if (methodName == "sqrt")      id = llvm::Intrinsic::sqrt;
        else if (methodName == "sin")       id = llvm::Intrinsic::sin;
        else if (methodName == "cos")       id = llvm::Intrinsic::cos;
        else if (methodName == "exp")       id = llvm::Intrinsic::exp;
        else if (methodName == "exp2")      id = llvm::Intrinsic::exp2;
        else if (methodName == "log")       id = llvm::Intrinsic::log;
        else if (methodName == "log2")      id = llvm::Intrinsic::log2;
        else if (methodName == "log10")     id = llvm::Intrinsic::log10;
        else return nullptr;

        auto* fn = llvm::Intrinsic::getDeclaration(module.get(), id, {floatVal->getType()});
        return builder->CreateCall(fn, {floatVal});
    }

    // Emit the x86 RDTSCP instruction - a serializing read of the CPU timestamp
    // counter.  Returns the 64-bit cycle count; the TSC_AUX processor id (the
    // intrinsic's second result) is discarded.  x86/Intel target only - CFlat
    // callers should guard with `if const (__X86__)`.
    llvm::Value* CreateRdtscp()
    {
        // llvm.x86.rdtscp returns { i64 cycles, i32 aux } and takes no arguments.
        auto* fn   = llvm::Intrinsic::getDeclaration(module.get(), llvm::Intrinsic::x86_rdtscp);
        auto* call = builder->CreateCall(fn, {});
        return builder->CreateExtractValue(call, { 0u }, "rdtscp");
    }

    // Emit the x86 LFENCE instruction - a load fence that also serializes the
    // instruction stream: it does not retire until all prior instructions have
    // completed, and no later instruction starts before it.  Pairs with RDTSC so
    // out-of-order execution cannot smear a measured region's start/end.  Returns
    // nothing.  x86/Intel target only - guard callers with `if const (__X86__)`.
    void CreateLfence()
    {
        // llvm.x86.sse2.lfence returns void and takes no arguments.
        auto* fn = llvm::Intrinsic::getDeclaration(module.get(), llvm::Intrinsic::x86_sse2_lfence);
        builder->CreateCall(fn, {});
    }

    // Emit the x86 PAUSE instruction - a spin-loop hint that yields the SMT
    // sibling and lowers the power/throughput cost of a busy-wait.  Returns
    // nothing.  x86/Intel target only - guard callers with `if const (__X86__)`.
    void CreatePause()
    {
        // llvm.x86.sse2.pause returns void and takes no arguments.
        auto* fn = llvm::Intrinsic::getDeclaration(module.get(), llvm::Intrinsic::x86_sse2_pause);
        builder->CreateCall(fn, {});
    }

    // Population count (llvm.ctpop) - number of set bits.  Target-independent:
    // lowers to POPCNT on x86, CNT on ARM, etc.  Result has the same integer
    // type as the input.
    llvm::Value* CreatePopcount(llvm::Value* intVal)
    {
        auto* fn = llvm::Intrinsic::getDeclaration(module.get(), llvm::Intrinsic::ctpop, { intVal->getType() });
        return builder->CreateCall(fn, { intVal }, "popcount");
    }

    // Count trailing zeros (llvm.cttz).  Target-independent.  The second argument
    // is_zero_poison is false, so cttz(0) is the bit width (well-defined) rather
    // than poison.  Result has the same integer type as the input.
    llvm::Value* CreateCtz(llvm::Value* intVal)
    {
        auto* fn = llvm::Intrinsic::getDeclaration(module.get(), llvm::Intrinsic::cttz, { intVal->getType() });
        return builder->CreateCall(fn, { intVal, llvm::ConstantInt::getFalse(*context) }, "ctz");
    }

    // Count leading zeros (llvm.ctlz).  Target-independent.  is_zero_poison is
    // false, so ctlz(0) is the bit width.  Result matches the input's type.
    llvm::Value* CreateClz(llvm::Value* intVal)
    {
        auto* fn = llvm::Intrinsic::getDeclaration(module.get(), llvm::Intrinsic::ctlz, { intVal->getType() });
        return builder->CreateCall(fn, { intVal, llvm::ConstantInt::getFalse(*context) }, "clz");
    }

    // Software prefetch (llvm.prefetch).  Target-independent.  Hints the memory
    // subsystem to pull `addr` toward the cache.  Hardcoded to a read prefetch
    // (rw=0) with high temporal locality (3) into the data cache (cache type=1) -
    // the common streaming/strided-loop case.  Returns nothing.
    void CreatePrefetch(llvm::Value* addr)
    {
        auto* i32ty = llvm::Type::getInt32Ty(*context);
        auto* fn = llvm::Intrinsic::getDeclaration(module.get(), llvm::Intrinsic::prefetch, { addr->getType() });
        builder->CreateCall(fn, {
            addr,
            llvm::ConstantInt::get(i32ty, 0),   // rw: 0 = read
            llvm::ConstantInt::get(i32ty, 3),   // locality: 3 = high temporal
            llvm::ConstantInt::get(i32ty, 1) }); // cache type: 1 = data cache
    }

    // Fused multiply-add (llvm.fma): a * b + c with a single rounding step.
    // Target-independent; lowers to a hardware FMA when available.  All three
    // operands and the result share the same floating-point type.
    llvm::Value* CreateFma(llvm::Value* a, llvm::Value* b, llvm::Value* c)
    {
        auto* fn = llvm::Intrinsic::getDeclaration(module.get(), llvm::Intrinsic::fma, { a->getType() });
        return builder->CreateCall(fn, { a, b, c }, "fma");
    }

    // Branch-probability hint (llvm.expect): tells the optimizer the i1 condition
    // `cond` is expected to equal `expected`.  Target-independent; affects block
    // layout only, not the computed value.  Result is the same i1 value.
    llvm::Value* CreateExpect(llvm::Value* cond, bool expected)
    {
        auto* fn = llvm::Intrinsic::getDeclaration(module.get(), llvm::Intrinsic::expect, { cond->getType() });
        return builder->CreateCall(fn, { cond, llvm::ConstantInt::get(cond->getType(), expected ? 1 : 0) }, "expect");
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

    // --- Linker path helpers (cache-first; static so --init can call without a compile) ---

    // Returns %USERPROFILE%\.cflat, or "" if USERPROFILE is unset.
    static std::string GetCflatCacheDir();

    // Run vswhere + directory scan and return the resolved paths for the given arch ("x64"/"x86").
    // runtimeDir is the directory of cflat.exe, used to locate the deployed lld-link/clang-cl.
    static LinkerPaths DiscoverLinkerPaths(const std::string& arch, const std::string& runtimeDir);

    // Try to load previously cached paths from ~/.cflat/linker_paths_<arch>.json.
    // Returns nullopt if the file is missing, malformed, or any stored path no longer exists.
    static std::optional<LinkerPaths> LoadLinkerPathsFromCache(const std::string& arch);

    // Persist paths to ~/.cflat/linker_paths_<arch>.json (creates the directory if needed).
    static bool SaveLinkerPathsToCache(const std::string& arch, const LinkerPaths& paths);

    // Cache-first lookup: load from disk, fall back to DiscoverLinkerPaths, and write the
    // cache on a miss so subsequent compiles skip discovery.
    static LinkerPaths FindLinkerPaths(const std::string& arch, const std::string& runtimeDir);

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
        // Match the native object's tuning for C interop objects. --cpu sets both ISA and
        // tune (clang -march); --tune overrides tune only (clang -mtune). clang-cl needs the
        // /clang: prefix to forward these clang-driver options. Pre-resolved/validated in Compile.
        if (!targetCpu_.empty()) argStrs.push_back("/clang:-march=" + targetCpu_);
        if (!tuneCpu_.empty())   argStrs.push_back("/clang:-mtune=" + tuneCpu_);
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
        int rc;
        {
            llvm::TimeTraceScope spawnScope("ClangCompileC", cSourcePath);
            rc = llvm::sys::ExecuteAndWait(clangPath, args, std::nullopt, {}, 0, 0, &clangCompileErr);
        }
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

    // Compile the in-process crash-handler (core/diagnostic/crashdump.c) into a temporary object
    // recorded in cObjectFiles_ for EmitExecutable to link. Used only under -g, where a
    // PDB is produced and DbgHelp can symbolize CFlat frames at runtime.
    //
    // Deliberately NOT routed through CompileCFile: that path runs ExtractCSignatures
    // (libclang) and registers the C functions as CFlat externs, which we do not want for
    // an internal handler. This is a minimal clang-cl spawn. arch is "x64" or "x86".
    // Failures are reported via LogError and return false without aborting the compile.
    bool CompileCrashHandlerObject(const std::string& arch)
    {
        if (runtimeDir.empty())
        {
            LogError("cannot locate crash handler source: runtime directory is unset.");
            return false;
        }

        llvm::SmallString<256> srcPath(runtimeDir);
        llvm::sys::path::append(srcPath, "core", "diagnostic", "crashdump.c");
        if (!llvm::sys::fs::exists(srcPath))
        {
            LogError(std::format("crash handler source not found: '{}'.", srcPath.str().str()));
            return false;
        }

        const std::string clangPath = FindClangCl();
        if (clangPath.empty())
        {
            LogError("clang-cl.exe not found - cannot compile crash handler.");
            return false;
        }

        llvm::SmallString<256> objFile;
        if (auto ec = llvm::sys::fs::createTemporaryFile("cflat_crashdump", "obj", objFile))
        {
            LogError(std::format("could not create temp object for crash handler: {}", ec.message()));
            return false;
        }
        std::string objPath = objFile.str().str();

        const std::string target = (arch == "x86")
            ? "--target=i686-pc-windows-msvc"
            : "--target=x86_64-pc-windows-msvc";
        const std::string foArg = "/Fo" + objPath;

        // /Z7 puts CodeView in the object so the handler's own frames are symbolizable too.
        std::vector<std::string> argStrs = {
            clangPath, "/c", "/Z7", "/nologo", target, srcPath.str().str(), foArg
        };

        std::vector<llvm::StringRef> args;
        for (auto& s : argStrs) args.push_back(s);

        if (verbose)
        {
            std::cout << "[verbose] compiling crash handler: " << srcPath.str().str()
                      << " -> " << objPath << "\n";
            std::cout << "[verbose]   clang-cl";
            for (size_t i = 1; i < argStrs.size(); ++i) std::cout << " " << argStrs[i];
            std::cout << "\n";
        }

        std::string clangCompileErr;
        int rc = llvm::sys::ExecuteAndWait(clangPath, args, std::nullopt, {}, 0, 0, &clangCompileErr);
        if (rc != 0)
        {
            llvm::sys::fs::remove(objPath);
            LogError(std::format("clang-cl failed to compile crash handler (exit {}){}{}",
                rc, clangCompileErr.empty() ? "" : ": ", clangCompileErr));
            return false;
        }

        cObjectFiles_.push_back(objPath);
        return true;
    }

    // Map a (preferably desugared) C type spelling onto a CFlat TypeAndValue.


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

    // Recognises clang's spelling for a bare function-pointer type:
    //   "int (*)(const char *, int)"   ->   IsFunctionPointer with return=int, params=[char*, int]
    // The leading return type is whatever appears before the literal "(*)" token; the
    // parenthesised argument list follows. Nested function-pointer arguments are rejected
    // (depth-counted top-level commas only) - matches the >2 pointer-levels limit.
    bool ParseCFunctionPointerSpelling(const std::string& s, TypeAndValue& out,
                                       std::unordered_set<std::string>& visited)
    {
        // Locate "(*)" possibly with whitespace around the star.
        size_t markerPos = std::string::npos;
        for (size_t i = 0; i + 2 < s.size(); ++i)
        {
            if (s[i] != '(') continue;
            size_t j = i + 1;
            while (j < s.size() && std::isspace((unsigned char)s[j])) ++j;
            if (j >= s.size() || s[j] != '*') continue;
            ++j;
            while (j < s.size() && std::isspace((unsigned char)s[j])) ++j;
            if (j < s.size() && s[j] == ')') { markerPos = i; break; }
        }
        if (markerPos == std::string::npos) return false;

        std::string retSpelling = s.substr(0, markerPos);
        while (!retSpelling.empty() && std::isspace((unsigned char)retSpelling.back()))
            retSpelling.pop_back();

        // After "(*)" the next non-space char must be '('.
        size_t after = s.find(')', markerPos) + 1;
        while (after < s.size() && std::isspace((unsigned char)s[after])) ++after;
        if (after >= s.size() || s[after] != '(') return false;
        size_t argOpen = after;
        size_t argClose = std::string::npos;
        int depth = 0;
        for (size_t i = argOpen; i < s.size(); ++i)
        {
            if (s[i] == '(') ++depth;
            else if (s[i] == ')') { --depth; if (depth == 0) { argClose = i; break; } }
        }
        if (argClose == std::string::npos) return false;

        std::string argList = s.substr(argOpen + 1, argClose - argOpen - 1);

        // Resolve the return type via the same recursive resolver.
        TypeAndValue retTV;
        if (!MapCTypeToTypeAndValueImpl(retSpelling, retTV, visited)) return false;
        // Function pointers returning function pointers are not supported here.
        if (retTV.IsFunctionPointer) return false;

        out = TypeAndValue();
        out.IsFunctionPointer = true;
        out.FuncPtrReturnTypeName = retTV.TypeName;
        out.FuncPtrReturnPointer = retTV.Pointer;

        // Split argList on top-level commas. Bail on nested fn-ptr arg or variadic.
        if (argList.find("...") != std::string::npos) return false;

        auto trim = [](std::string v) {
            size_t a = 0; while (a < v.size() && std::isspace((unsigned char)v[a])) ++a;
            size_t b = v.size(); while (b > a && std::isspace((unsigned char)v[b-1])) --b;
            return v.substr(a, b - a);
        };

        // Empty arg list or "void" -> zero params.
        std::string normArgs = trim(argList);
        if (normArgs.empty() || normArgs == "void")
            return true;

        std::vector<std::string> parts;
        {
            int d = 0;
            std::string cur;
            for (char c : argList)
            {
                if (c == '(') { ++d; cur += c; }
                else if (c == ')') { --d; cur += c; }
                else if (c == ',' && d == 0) { parts.push_back(cur); cur.clear(); }
                else cur += c;
            }
            if (!cur.empty()) parts.push_back(cur);
        }
        for (auto& p : parts)
        {
            TypeAndValue ptv;
            if (!MapCTypeToTypeAndValueImpl(trim(p), ptv, visited)) return false;
            if (ptv.IsFunctionPointer) return false; // nested fn-ptr arg not supported
            TypeAndValue::FuncPtrParam fp;
            fp.TypeName = ptv.TypeName;
            fp.Pointer = ptv.Pointer;
            out.FuncPtrParams.push_back(fp);
        }
        return true;
    }

    // Extract fixed-array dimensions ("[N]") from a C field type spelling, returning the
    // element spelling with those groups removed and the dimensions in declaration order.
    // Only positive integer extents are treated as fixed arrays; an incomplete "[]" or a
    // non-numeric extent is left verbatim (the caller's mapper then applies the usual
    // array->pointer decay). Used by RegisterCRecords so a struct field like
    // `char szExeFile[260]` lays out as an inline 260-byte array (correct sizeof), NOT a
    // decayed pointer - which would silently shrink the struct.
    static std::string StripFixedArrayDims(const std::string& ctype, std::vector<uint64_t>& dims)
    {
        std::string elem;
        size_t i = 0;
        while (i < ctype.size())
        {
            if (ctype[i] == '[')
            {
                size_t close = ctype.find(']', i);
                if (close == std::string::npos) { elem += ctype.substr(i); break; }
                std::string inner = ctype.substr(i + 1, close - i - 1);
                size_t a = inner.find_first_not_of(" \t");
                size_t b = inner.find_last_not_of(" \t");
                std::string num = (a == std::string::npos) ? std::string{} : inner.substr(a, b - a + 1);
                bool allDigits = !num.empty() &&
                    std::all_of(num.begin(), num.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
                if (allDigits)
                {
                    dims.push_back(std::strtoull(num.c_str(), nullptr, 10));
                    i = close + 1;
                    continue;
                }
                elem += ctype.substr(i, close - i + 1);  // keep non-numeric extent verbatim
                i = close + 1;
                continue;
            }
            elem += ctype[i++];
        }
        while (!elem.empty() && (elem.back() == ' ' || elem.back() == '\t')) elem.pop_back();
        return elem;
    }

    bool MapCTypeToTypeAndValueImpl(std::string ctype, TypeAndValue& out,
                                    std::unordered_set<std::string>& visited)
    {
        // Detect function-pointer spelling before the generic '*'-strip path mangles it.
        // clang spells these as "R (*)(args)" - the "(*)" token disambiguates from a
        // function declaration (which would have a name between the parens).
        if (ctype.find("(*)") != std::string::npos)
        {
            if (ParseCFunctionPointerSpelling(ctype, out, visited))
                return true;
            // Fall through only if the parse failed - lets unknown shapes hit the normal
            // "return false" path below instead of being silently accepted.
            return false;
        }

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

        // Three-or-more levels of indirection (e.g. `char***`, `void****`) collapse to an
        // opaque `void**`. CFlat's TypeAndValue exposes at most two pointer levels
        // (Pointer + ElemPointer), and any pointer is the same ABI size on x64/x86, so
        // the call still links correctly. Direct dereference of the third+ level is not
        // available from CFlat - the user holds the handle and threads it back through C
        // (e.g. allocate / free helpers on the C side that hide the inner indirection).
        if (ptr > 2)
        {
            out.TypeName = "void";
            out.Pointer = true;
            out.ElemPointer = true;
            return true;
        }

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
                // Final fallback: chase a user typedef recorded by AdoptRawTypedefs. We
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
    static bool HashFileFnv1a(const std::string& path, uint64_t& outHash)
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
    bool HashFileContents(const std::string& path, uint64_t& outHash) const
    {
        return HashFileFnv1a(path, outHash);
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

    // Build the clang driver args shared by the C-interop extraction paths. headerDir, when
    // non-empty, is added as the first -I (the bound header's own directory). errorRecovery
    // enables per-decl recovery so one bad probe / decl does not poison the rest.
    std::vector<std::string> BuildClangDriverArgs(const std::string& headerDir,
                                               const std::vector<std::string>& extraDefines,
                                               bool errorRecovery) const
    {
        std::vector<std::string> args;
        args.push_back(platformValue == 32 ? "--target=i686-pc-windows-msvc"
                                           : "--target=x86_64-pc-windows-msvc");
        args.push_back("-fsyntax-only");
        args.push_back("-x");
        args.push_back("c");
        if (errorRecovery)
        {
            args.push_back("-ferror-limit=0");
            args.push_back("-Wno-everything");
        }
        if (!headerDir.empty()) args.push_back("-I" + headerDir);
        for (const auto& inc : cIncludeDirs_) args.push_back("-I" + inc);
        for (const auto& def : cDefines_)      args.push_back("-D" + def);
        for (const auto& def : extraDefines)   args.push_back("-D" + def);
        return args;
    }

    // ---- clang C++ API C-interop extraction (CClangExtract.cpp) -------------------------
    // The extractor (clang C++ API, single full parse + cheap PP prepass) emits plain-data
    // canonical-C-type spellings; these helpers map them to the C* structs the existing
    // Register*/cache machinery consumes. Canonical spellings already resolve typedef chains
    // (HANDLE -> void *), so the string-based MapCTypeToTypeAndValue stays unchanged.

    // Map one extracted signature to a CSigEntry. Returns false (skips the function) when a
    // return / parameter type is outside the extern ABI subset (skips the whole function).
    bool MapRawSig(const cflat_cinterop::RawSig& r, CSigEntry& e)
    {
        e = CSigEntry();
        e.name     = r.name;
        e.variadic = r.variadic;
        e.line     = r.line ? r.line : 1;
        e.col      = r.col < 0 ? 0 : r.col;
        if (!MapCTypeToTypeAndValue(r.retType, e.ret))
        {
            if (verbose) std::cout << "[verbose]   skipping '" << r.name
                                   << "': unsupported return type '" << r.retType << "'\n";
            return false;
        }
        for (size_t i = 0; i < r.paramTypes.size(); ++i)
        {
            TypeAndValue ptv;
            if (!MapCTypeToTypeAndValue(r.paramTypes[i], ptv))
            {
                if (verbose) std::cout << "[verbose]   skipping '" << r.name
                                       << "': unsupported parameter type '" << r.paramTypes[i] << "'\n";
                return false;
            }
            if (i < r.paramNames.size()) ptv.VariableName = r.paramNames[i];
            e.params.push_back(std::move(ptv));
        }
        return true;
    }

    // Map one extracted global variable to a CGlobalEntry. Returns false (skips the global) when
    // its type is outside the extern ABI subset. Array-typed globals are skipped: the string
    // mapper decays `T[N]` to a pointer, which would mistype the symbol's storage as holding a
    // pointer rather than being the array data - a silent miscompile. The user threads such
    // symbols through C-side accessors instead.
    bool MapRawGlobal(const cflat_cinterop::RawGlobalVar& r, CGlobalEntry& e)
    {
        if (r.ctype.find('[') != std::string::npos)
        {
            if (verbose) std::cout << "[verbose]   skipping global '" << r.name
                                   << "': array type '" << r.ctype << "' is not bindable\n";
            return false;
        }
        e = CGlobalEntry();
        e.name = r.name;
        e.line = r.line ? r.line : 1;
        e.col  = r.col < 0 ? 0 : r.col;
        if (!MapCTypeToTypeAndValue(r.ctype, e.type))
        {
            if (verbose) std::cout << "[verbose]   skipping global '" << r.name
                                   << "': unsupported type '" << r.ctype << "'\n";
            return false;
        }
        e.type.VariableName = r.name;
        return true;
    }

    // Classify a folded object-like macro into a CMacroEntry. An int macro whose natural type
    // chases (via the mapper) to a function pointer or void* becomes a funcptr / pointer
    // sentinel; otherwise it is a plain int / float / string. Returns false for Skip macros.
    bool ClassifyRawMacro(const cflat_cinterop::RawMacro& r, CMacroEntry& e)
    {
        using K = cflat_cinterop::RawMacro;
        if (r.kind == K::Skip) return false;
        e = CMacroEntry();
        e.name = r.name; e.file = r.file; e.line = r.line ? r.line : 1; e.col = 0;
        if (r.kind == K::String) { e.isString = true; e.stringValue = r.stringValue; return true; }
        if (r.kind == K::Float)  { e.isFloat = true;  e.floatValue  = r.floatValue;  return true; }

        e.value = r.intValue;
        if (!r.naturalType.empty())
        {
            TypeAndValue tv;
            if (MapCTypeToTypeAndValue(r.naturalType, tv))
            {
                if (tv.IsFunctionPointer) { e.isFuncPtr = true; e.funcPtrTV = std::move(tv); }
                else if (tv.Pointer && !tv.ElemPointer && tv.TypeName == "void") e.isPointer = true;
                else if (!tv.Pointer && BitfieldStorageBits(tv.TypeName) != 0 && tv.TypeName != "bool")
                    e.intTypeName = tv.TypeName;   // known plain integer scalar
            }
        }
        return true;
    }

    // Adopt typedef aliases into cTypedefMap_ (first-writer-wins). Canonical spellings already
    // resolve most chains; this only feeds the rare bare-alias fallback in the type mapper.
    void AdoptRawTypedefs(const cflat_cinterop::ExtractResult& raw)
    {
        for (const auto& t : raw.typedefs)
            if (!t.name.empty() && !t.underlying.empty() && t.underlying != t.name)
                cTypedefMap_.emplace(t.name, t.underlying);
    }

    // Surface `typedef struct Tag {...} Name;` (a by-value record typedef) as a CFlat type alias
    // Name -> Tag, so a user can write the familiar typedef name (MSG, RECT, WNDCLASSEXA) rather
    // than the bare clang tag (tagMSG, ...). MUST run after RegisterCRecords so the target tag is
    // already in dataStructures. Pointer / array / scalar typedefs are left to the C type mapper.
    // Fills `out` with the (alias, target) pairs so the disk cache can replay them on a hit.
    void CollectRecordTypedefAliases(const cflat_cinterop::ExtractResult& raw,
                                     std::vector<std::pair<std::string, std::string>>& out)
    {
        auto trim = [](std::string& x) {
            size_t a = x.find_first_not_of(" \t");
            size_t b = x.find_last_not_of(" \t");
            x = (a == std::string::npos) ? std::string{} : x.substr(a, b - a + 1);
        };
        for (const auto& t : raw.typedefs)
        {
            if (t.name.empty() || t.underlying.empty() || t.name == t.underlying) continue;
            if (t.underlying.find('*') != std::string::npos) continue;   // pointer typedef (LPMSG)
            if (t.underlying.find('[') != std::string::npos) continue;   // array typedef
            std::string tag = t.underlying;
            trim(tag);
            if (tag.rfind("const ", 0) == 0)    { tag.erase(0, 6); trim(tag); }
            if (tag.rfind("struct ", 0) == 0)   tag.erase(0, 7);
            else if (tag.rfind("union ", 0) == 0) tag.erase(0, 6);
            trim(tag);
            if (tag.empty() || tag == t.name) continue;
            if (dataStructures.find(tag) == dataStructures.end()) continue;  // not a registered record
            out.emplace_back(t.name, tag);
        }
    }

    // Register record-typedef aliases (from CollectRecordTypedefAliases or a cache entry). Never
    // shadows a real type or an existing alias (first-writer-wins).
    void RegisterRecordAliases(const std::vector<std::pair<std::string, std::string>>& aliases)
    {
        for (const auto& [alias, target] : aliases)
        {
            if (dataStructures.find(alias) != dataStructures.end()) continue;  // real type wins
            if (typeAliases.find(alias) != typeAliases.end()) continue;        // first-writer-wins
            RegisterTypeAlias(alias, target);
        }
    }

    // For a header bind, the extractor collects records both in and out of scope (see
    // RawRecord::inScope). Registering every out-of-scope SDK struct would flood the type table,
    // but an in-scope struct is useless if a struct it embeds BY VALUE was dropped (the in-scope
    // struct then fails to size and is itself skipped - the cascade that made MSG/WNDCLASSEXA
    // unavailable from windows.h). This keeps the transitive closure of in-scope records over
    // their by-value field dependencies and drops the rest, in place.
    void PruneRecordsToNeededClosure(std::vector<cflat_cinterop::RawRecord>& records)
    {
        // Last definition wins on a duplicate tag (forward decls are not definitions, so this is
        // rare); the index just needs to resolve a referenced tag to some record we can keep.
        std::unordered_map<std::string, size_t> byName;
        for (size_t i = 0; i < records.size(); ++i)
            if (!records[i].name.empty()) byName[records[i].name] = i;

        // Extract the referenced record tag from a field's canonical C type spelling, but ONLY
        // when the field embeds the aggregate by value (a pointer field is pointer-sized whether
        // or not the pointee is registered, so it creates no sizing dependency). Returns "" for
        // pointer / scalar / enum fields.
        auto byValueDep = [](const std::string& ctype) -> std::string {
            if (ctype.find('*') != std::string::npos) return {};   // pointer: no sizing dependency
            std::string s = ctype;
            if (auto br = s.find('['); br != std::string::npos) s = s.substr(0, br);  // drop array suffix
            auto trim = [](std::string& x) {
                size_t a = x.find_first_not_of(" \t");
                size_t b = x.find_last_not_of(" \t");
                x = (a == std::string::npos) ? std::string{} : x.substr(a, b - a + 1);
            };
            trim(s);
            // Strip leading qualifiers / tag keywords to reach the bare tag name.
            for (;;)
            {
                if (s.rfind("const ", 0) == 0)    { s.erase(0, 6); trim(s); continue; }
                if (s.rfind("volatile ", 0) == 0) { s.erase(0, 9); trim(s); continue; }
                if (s.rfind("struct ", 0) == 0)   { s.erase(0, 7); trim(s); continue; }
                if (s.rfind("union ", 0) == 0)    { s.erase(0, 6); trim(s); continue; }
                if (s.rfind("enum ", 0) == 0)     return {};   // enum is scalar (int-sized)
                break;
            }
            return s;
        };

        std::vector<bool> needed(records.size(), false);
        std::vector<size_t> work;
        for (size_t i = 0; i < records.size(); ++i)
            if (records[i].inScope) { needed[i] = true; work.push_back(i); }

        while (!work.empty())
        {
            size_t i = work.back(); work.pop_back();
            for (const auto& f : records[i].fields)
            {
                std::string dep = byValueDep(f.ctype);
                if (dep.empty()) continue;
                auto it = byName.find(dep);
                if (it == byName.end() || needed[it->second]) continue;
                needed[it->second] = true;
                work.push_back(it->second);
            }
        }

        std::vector<cflat_cinterop::RawRecord> kept;
        kept.reserve(records.size());
        for (size_t i = 0; i < records.size(); ++i)
            if (needed[i]) kept.push_back(std::move(records[i]));
        records.swap(kept);
    }

    // Translate extracted records to CRecordEntry (field type spellings carried through).
    void MapRawRecords(const cflat_cinterop::ExtractResult& raw, std::vector<CRecordEntry>& out)
    {
        for (const auto& r : raw.records)
        {
            CRecordEntry rec;
            rec.name = r.name; rec.isUnion = r.isUnion;
            rec.line = r.line ? r.line : 1; rec.col = r.col < 0 ? 0 : r.col;
            for (const auto& f : r.fields)
            {
                CRecordFieldEntry fe;
                fe.name = f.name; fe.ctype = f.ctype;
                fe.isBitfield = f.isBitfield; fe.bitWidth = f.bitWidth;
                rec.fields.push_back(std::move(fe));
            }
            out.push_back(std::move(rec));
        }
    }

    // Parse a C *header* via the clang C++ API and produce the bind data (functions, enums,
    // records, object-like macro values, function-like macros). Records are registered up
    // front so struct-by-value signatures resolve. (Formerly a libclang decl+name parse plus a
    // separate value-fold parse; now a single full parse via the clang C++ API.) Returns false
    // on a hard parse failure.
    // Extract decls from one or more C headers that share a single translation unit. The stub
    // `#include`s each header in `headerPaths` order, so an earlier entry can satisfy a later
    // one's prerequisites - this is how a grouped import `import {"windows.h","tlhelp32.h"};`
    // makes tlhelp32.h's HANDLE/DWORD resolve without the compiler knowing any header by name.
    // A plain single-header import passes a one-element list. The in-scope registration set is
    // the union, over every header, of its own directory plus the Windows SDK um/<->shared/
    // sibling expansion (and the --c-include roots), so each header contributes only decls that
    // live under those roots. If the parse hits a missing-prerequisite error inside a header,
    // nothing is registered and *outPrereqFailure is set so the caller can fail loudly.
    bool ExtractCHeaderClang(const std::vector<std::string>& headerPaths,
                             std::vector<CSigEntry>& outSigs, std::vector<CEnumEntry>& outEnums,
                             std::vector<CRecordEntry>& outRecords,
                             std::vector<CMacroEntry>& outMacros,
                             std::vector<CFunctionMacroEntry>& outFuncMacros,
                             std::vector<CGlobalEntry>& outGlobals,
                             std::vector<std::pair<std::string, std::string>>& outAliases,
                             const std::vector<std::string>& extraDefines = {},
                             std::vector<std::string>* outIncludes = nullptr,
                             bool* outPrereqFailure = nullptr,
                             std::string* outPrereqMsg = nullptr)
    {
        if (headerPaths.empty()) return false;

        // The first header's directory anchors the clang driver's primary -I; every header's
        // directory (plus its SDK sibling) is added to the in-scope set below. The primary header
        // also labels the TimeTrace scopes and attributes registered decls for LSP go-to-def.
        const std::string& headerPath = headerPaths.front();
        const std::string primaryDir = std::filesystem::path(headerPath).parent_path().string();

        std::string source;
        for (const auto& h : headerPaths)
        {
            std::string fwd = h;
            std::replace(fwd.begin(), fwd.end(), '\\', '/');
            source += "#include \"" + fwd + "\"\n";
        }

        cflat_cinterop::ExtractRequest req;
        req.mainFileName   = "cflat_hdr_stub.c";
        req.source         = source;
        req.args           = BuildClangDriverArgs(primaryDir, extraDefines, /*errorRecovery*/ true);
        req.wantMacros     = true;
        req.requireInScope = true;
        req.skipFunctionBodies = true;   // headers: declarations only - skip inline bodies
        req.wantIncludes   = (outIncludes != nullptr);

        // In-scope set = union over the group of each header's dir + the Windows SDK um/<->shared/
        // sibling expansion. The Windows SDK splits its API surface across um/ (functions, most
        // structs) and shared/ (winerror.h ERROR_*, minwindef.h MAX_PATH, ntdef.h, ...); code
        // copied from MSDN fails on the very first ERROR_SUCCESS without the sibling. This is a
        // generic directory-layout rule, not knowledge of any specific header.
        auto addScopeDir = [&](const std::string& d) {
            for (const auto& e : req.inScopeDirs) if (e == d) return;
            req.inScopeDirs.push_back(d);
        };
        for (const auto& h : headerPaths)
        {
            std::filesystem::path hdrDirPath = std::filesystem::path(h).parent_path();
            addScopeDir(hdrDirPath.string());
            std::string dirLeaf = hdrDirPath.filename().string();
            std::transform(dirLeaf.begin(), dirLeaf.end(), dirLeaf.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            if (dirLeaf == "um")
                addScopeDir((hdrDirPath.parent_path() / "shared").string());
            else if (dirLeaf == "shared")
                addScopeDir((hdrDirPath.parent_path() / "um").string());
        }
        for (const auto& inc : cIncludeDirs_) addScopeDir(inc);

        if (verbose)
        {
            std::cout << "[verbose] extracting C header"
                      << (headerPaths.size() > 1 ? " group" : "") << ":";
            for (const auto& h : headerPaths) std::cout << " " << h;
            std::cout << " (clang C++ API)\n";
        }

        cflat_cinterop::ExtractResult raw;
        std::string err;
        if (!cflat_cinterop::ExtractCInterop(req, raw, err))
        {
            if (verbose) std::cout << "[verbose]   C header extraction failed: " << err << "\n";
            return false;
        }

        // A missing-prerequisite error inside a header (unknown type 'HANDLE', ...) means clang
        // dropped the dependent declarations; registering the error-recovered remnants would
        // silently expose a partial, wrong-sized API. Refuse and let the caller suggest a group.
        if (raw.prereqErrors > 0)
        {
            if (outPrereqFailure) *outPrereqFailure = true;
            if (outPrereqMsg) *outPrereqMsg = raw.firstPrereqError;
            if (verbose)
                std::cout << "[verbose]   header is not self-contained: " << raw.firstPrereqError << "\n";
            return false;
        }

        if (outIncludes) *outIncludes = std::move(raw.includedFiles);

        {
            llvm::TimeTraceScope adoptScope("AdoptTypedefs", headerPath);
            AdoptRawTypedefs(raw);
        }

        // Records first, then register, so struct-by-value param/return types resolve. Prune to
        // the in-scope set plus the by-value dependency closure before mapping, so the cache and
        // the type table carry the dependency structs (e.g. POINT for MSG) but not every
        // unrelated out-of-scope SDK struct.
        {
            llvm::TimeTraceScope recordScope("RegisterCRecords", headerPath);
            PruneRecordsToNeededClosure(raw.records);
            MapRawRecords(raw, outRecords);
            RegisterCRecords(outRecords, headerPath);
            // Surface `typedef struct Tag {...} Name;` as Name -> Tag aliases now that the tags
            // are registered, so the user can write MSG/RECT/WNDCLASSEXA, not tagMSG/...
            CollectRecordTypedefAliases(raw, outAliases);
            RegisterRecordAliases(outAliases);
        }

        {
            llvm::TimeTraceScope sigScope("MapSignatures", headerPath);
            for (const auto& rs : raw.sigs)
            {
                CSigEntry e;
                if (MapRawSig(rs, e)) outSigs.push_back(std::move(e));
            }
        }

        {
            llvm::TimeTraceScope enumScope("MapEnums", headerPath);
            for (const auto& re : raw.enums)
            {
                CEnumEntry e;
                e.name = re.name; e.value = re.value;
                e.line = re.line ? re.line : 1; e.col = re.col < 0 ? 0 : re.col;
                outEnums.push_back(std::move(e));
            }
        }

        {
            llvm::TimeTraceScope macroScope("MapMacros", headerPath);
            for (const auto& rm : raw.macros)
            {
                CMacroEntry e;
                if (ClassifyRawMacro(rm, e)) outMacros.push_back(std::move(e));
            }
            for (const auto& rf : raw.funcMacros)
            {
                CFunctionMacroEntry e;
                e.name = rf.name; e.params = rf.params; e.body = rf.body;
                e.file = rf.file; e.line = rf.line ? rf.line : 1; e.col = rf.col < 0 ? 0 : rf.col;
                outFuncMacros.push_back(std::move(e));
            }
        }

        {
            llvm::TimeTraceScope globalScope("MapGlobals", headerPath);
            for (const auto& rg : raw.globals)
            {
                CGlobalEntry e;
                if (MapRawGlobal(rg, e)) outGlobals.push_back(std::move(e));
            }
        }

        if (verbose)
            std::cout << "[verbose]   header bind: " << outSigs.size() << " sig(s), "
                      << outEnums.size() << " enum(s), " << outRecords.size() << " record(s), "
                      << outMacros.size() << " macro(s), " << outFuncMacros.size() << " func-macro(s), "
                      << outGlobals.size() << " global(s)\n";
        return true;
    }

    // Extract externally-linkable functions a .c file DEFINES, via the clang C++ API. Records
    // are registered up front (struct-by-value). Used by the .c auto-extern path.
    bool ExtractCFileClang(const std::string& cSourcePath,
                           std::vector<CSigEntry>& outSigs, std::vector<CRecordEntry>& outRecords,
                           std::vector<CGlobalEntry>& outGlobals)
    {
        llvm::TimeTraceScope extractScope("CFileExtract", cSourcePath);

        cflat_cinterop::ExtractRequest req;
        req.realPath        = cSourcePath;     // parsed from disk
        req.args            = BuildClangDriverArgs(/*headerDir*/ "", /*extraDefines*/ {}, /*errorRecovery*/ true);
        req.wantMacros      = false;
        req.requireInScope  = false;
        req.definitionsOnly = true;

        if (verbose)
            std::cout << "[verbose] extracting C signatures: " << cSourcePath << " (clang C++ API)\n";

        cflat_cinterop::ExtractResult raw;
        std::string err;
        if (!cflat_cinterop::ExtractCInterop(req, raw, err))
        {
            if (verbose) std::cout << "[verbose]   C extraction failed: " << err << "\n";
            return false;
        }

        {
            llvm::TimeTraceScope adoptScope("AdoptTypedefs", cSourcePath);
            AdoptRawTypedefs(raw);
        }
        {
            llvm::TimeTraceScope recordScope("RegisterCRecords", cSourcePath);
            MapRawRecords(raw, outRecords);
            RegisterCRecords(outRecords, cSourcePath);
        }
        {
            llvm::TimeTraceScope sigScope("MapSignatures", cSourcePath);
            for (const auto& rs : raw.sigs)
            {
                CSigEntry e;
                if (MapRawSig(rs, e)) outSigs.push_back(std::move(e));
            }
        }
        {
            llvm::TimeTraceScope globalScope("MapGlobals", cSourcePath);
            for (const auto& rg : raw.globals)
            {
                CGlobalEntry e;
                if (MapRawGlobal(rg, e)) outGlobals.push_back(std::move(e));
            }
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
        std::vector<CGlobalEntry> hitGlobals;
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
                    hitGlobals = entry.globals;
                    hit = true;
                }
                // Timestamp moved but content may be identical - only now pay for a hash.
                else if (hashNow() == entry.hash)
                {
                    if (verbose) std::cout << "[verbose] C signatures cache hit (hash) for " << fileForLsp << "\n";
                    entry.mtime = currentMtime; // refresh so the next check short-circuits on mtime
                    hitSigs = entry.sigs;
                    hitRecords = entry.records;
                    hitGlobals = entry.globals;
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
            RegisterCGlobals(hitGlobals, fileForLsp);
            return true;
        }

        // Cache miss - parse and extract (outside the lock; concurrent first-time misses for
        // the same file just redo the work harmlessly, last writer wins). Extraction is in-
        // process via the clang C++ API and needs no clang-cl; the object compile (CompileCFile)
        // is what requires clang-cl, and it reports its own error if the toolchain is missing.
        // This also lets LSP .c indexing work when clang-cl is unavailable.
        std::vector<CSigEntry> sigs;
        std::vector<CRecordEntry> records;
        std::vector<CGlobalEntry> globals;
        if (!ExtractCFileClang(cSourcePath, sigs, records, globals))
            return false;

        if (!mtEc)
        {
            CFileSigCacheEntry entry;
            entry.mtime = currentMtime;
            entry.hash  = hashNow();
            entry.sigs  = sigs;
            entry.records = records;
            entry.globals = globals;
            std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
            cFileSigCache_[cacheKey] = std::move(entry);
        }

        // Records were already registered inside ExtractCFileClang (so it could map
        // struct-by-value parameter types); do not re-register here.
        RegisterCSignatures(sigs, fileForLsp, programAlias);
        RegisterCGlobals(globals, fileForLsp);
        return true;
    }

    // Render a "= value" suffix for a folded C integer constant's --symbol / hover
    // signature, so the discoverable type carries its compile-time value too
    // (GENERIC_READ is `u32 GENERIC_READ = 0x80000000`, not just `u32 GENERIC_READ`).
    // Unsigned integer types render in hex masked to their width - the flag/mask idiom
    // these constants almost always express; signed integer types render in decimal.
    static std::string ConstIntValueSuffix(const std::string& typeName, long long value)
    {
        bool isUnsigned = !typeName.empty() && typeName[0] == 'u';  // u8/u16/u32/u64
        if (isUnsigned)
        {
            unsigned bits = BitfieldStorageBits(typeName);
            uint64_t mask = (bits == 0 || bits >= 64) ? ~0ull : ((1ull << bits) - 1);
            return std::format(" = 0x{:x}", (uint64_t)value & mask);
        }
        return std::format(" = {}", value);
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
                            tv.TypeName + " " + e.name + ConstIntValueSuffix(tv.TypeName, e.value));
        }
        if (verbose)
            std::cout << "[verbose]   registered " << enums.size() << " C enum constant(s) from " << fileForLsp << "\n";
    }

    // Register externally-linkable C globals as declaration-only, mutable CFlat globals (external
    // references the linker resolves against the C library / object). First-writer-wins against an
    // existing global, matching RegisterCEnums.
    void RegisterCGlobals(const std::vector<CGlobalEntry>& globals, const std::string& fileForLsp)
    {
        for (const CGlobalEntry& e : globals)
        {
            if (e.name.empty()) continue;
            if (globalNamedVariable.count(e.name)) continue;  // first writer wins

            TypeAndValue tv = e.type;
            tv.VariableName = e.name;
            CreateGlobalVariable(tv, /*initValue*/ nullptr, /*threadLocal*/ false,
                                 /*userAlign*/ 0, /*externalDecl*/ true);

            if (auto* s = GetSymbolSink())
                s->Register(SymbolKind::Variable, e.name, fileForLsp, e.line, e.col < 0 ? 0 : e.col,
                            tv.TypeName + (tv.Pointer ? "*" : "") + " " + e.name);
        }
        if (verbose)
            std::cout << "[verbose]   registered " << globals.size() << " C global(s) from " << fileForLsp << "\n";
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
        // a CFlat-defined struct/union with C metadata. Anonymous records (no tag) are
        // skipped - they have no name to bind to, and clang inlines their fields into the
        // enclosing struct at the JSON layer anyway.
        std::vector<const CRecordEntry*> ours;
        ours.reserve(records.size());
        for (const auto& r : records)
        {
            if (r.name.empty()) continue;
            if (dataStructures.find(r.name) != dataStructures.end()) continue;
            // Create the opaque shell now so a later field can refer to the record via
            // `struct r.name *` / `union r.name *` or to another record being registered
            // in the same batch. CreateUnionType handles the opaque-shell case identically
            // to CreateStructType, so the union branch can also start from an opaque shell.
            CreateStructType(r.name, /*typeAndValues*/{});
            ours.push_back(&r);
        }

        // Pass 2: bodies. If any field type cannot be mapped, abandon this record and leave
        // the opaque shell in place - a later reference will then surface a clear error
        // rather than crash on a partially-typed struct.
        for (const CRecordEntry* rp : ours)
        {
            const CRecordEntry& r = *rp;
            std::vector<DeclTypeAndValue> fields;
            fields.reserve(r.fields.size());
            bool ok = true;
            for (const auto& f : r.fields)
            {
                TypeAndValue tv;
                // Fixed-size array field (e.g. `char szExeFile[260]`): strip the [N] extents
                // and map the element type, then re-apply the dimensions as an inline array.
                // The shared mapper decays any `[...]` to a pointer (correct for a function
                // parameter, wrong for a struct field), so the extents are peeled here first.
                std::vector<uint64_t> arrDims;
                std::string elemSpelling = StripFixedArrayDims(f.ctype, arrDims);
                if (!MapCTypeToTypeAndValue(elemSpelling, tv))
                {
                    if (verbose) std::cout << "[verbose]   skipping C " << (r.isUnion ? "union" : "struct")
                                           << " '" << r.name
                                           << "': unsupported field '" << f.name
                                           << "' of type '" << f.ctype << "'\n";
                    ok = false;
                    break;
                }
                if (!arrDims.empty())
                {
                    tv.ConstArraySize = arrDims[0];
                    tv.ConstInnerDimensions.assign(arrDims.begin() + 1, arrDims.end());
                }
                // A C function-pointer field (e.g. WNDPROC lpfnWndProc in WNDCLASSEXA) occupies a
                // single bare pointer in the C ABI. CFlat's function<T> is a 16-byte {code, env}
                // fat closure, so keeping IsFunctionPointer here would oversize the struct and
                // shift every following field's offset (sizeof(WNDCLASSEXA) -> 88 instead of 80).
                // Collapse to an opaque void*; the user stores `(void*)namedFunction` into it.
                if (tv.IsFunctionPointer)
                {
                    tv = TypeAndValue{};
                    tv.TypeName = "void";
                    tv.Pointer  = true;
                }
                DeclTypeAndValue d;
                static_cast<TypeAndValue&>(d) = tv;
                d.VariableName = f.name;
                if (f.isBitfield)
                {
                    d.IsBitfield = true;
                    d.BitWidth = f.bitWidth;
                }
                fields.push_back(std::move(d));
            }
            if (!ok || fields.empty())
            {
                // Leave the shell in place for !ok with no body so we don't crash on later
                // references; the user will get a clear error if they try to use it.
                continue;
            }
            // A by-value aggregate field whose LLVM type is still an opaque shell (an inner
            // record we abandoned above, or a forward declaration never completed) has no
            // size. Sizing it in CreateStructType/CreateUnionType would assert ("Cannot
            // getTypeInfo() on a type that is unsized"). Abandon this record too, leaving the
            // opaque shell - same graceful path as an unmapped field.
            for (const auto& d : fields)
            {
                if (d.Pointer) continue;            // pointers are always sized
                auto* ft = GetType(d);
                if (ft && !ft->isSized())
                {
                    if (verbose) std::cout << "[verbose]   skipping C " << (r.isUnion ? "union" : "struct")
                                           << " '" << r.name << "': field '" << d.VariableName
                                           << "' has incomplete (unsized) type '" << d.TypeName << "'\n";
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;
            // Bitfield packing for C structs uses the same MSVC LSB-first layout
            // as native CFlat bitfields. The packing pass produces synthetic
            // storage slots; CreateStructType consumes them and stores the
            // BitfieldInfo side-table on the StructData.
            std::vector<BitfieldInfo> packedBitfields;
            bool anyBitfields = false;
            for (const auto& tv : fields) { if (tv.IsBitfield) { anyBitfields = true; break; } }
            // Save the semantic field list (original names + CFlat-mapped types) before
            // PackBitfields replaces individual bitfield entries with __bfN storage slots.
            // Used only for per-field symbol registration below; skipped when there are no
            // bitfields because fields itself is unchanged in that case.
            std::vector<DeclTypeAndValue> prePackFields;
            if (anyBitfields)
            {
                prePackFields = fields;
                fields = PackBitfields(fields, packedBitfields);
            }
            if (r.isUnion)
                CreateUnionType(r.name, fields);
            else
                CreateStructType(r.name, fields, 0,
                    anyBitfields ? &packedBitfields : nullptr);
            if (auto* s = GetSymbolSink())
            {
                s->Register(SymbolKind::Struct, r.name, fileForLsp, r.line, r.col < 0 ? 0 : r.col,
                            (r.isUnion ? "union " : "struct ") + r.name);
                // Register per-field symbols so --symbol detail and LSP hover list members.
                // For bitfield records use prePackFields (semantic names before packing);
                // for plain records fields itself is unchanged.
                const auto& symFields = anyBitfields ? prePackFields : fields;
                for (const auto& f : symFields)
                {
                    if (f.VariableName.empty()) continue;  // skip unnamed padding markers
                    std::string annSig;
                    for (uint64_t d : f.ConstInnerDimensions)
                        annSig += "[" + std::to_string(d) + "] ";
                    if (f.ConstArraySize > 0)
                        annSig = "[" + std::to_string(f.ConstArraySize) + "] " + annSig;
                    std::string typeSig = f.TypeName;
                    if (f.Pointer) typeSig += "*";
                    if (f.ElemPointer) typeSig += "*";
                    std::string fieldSig = annSig + typeSig + " " + f.VariableName;
                    if (f.IsBitfield && f.BitWidth > 0)
                        fieldSig += ":" + std::to_string(f.BitWidth);
                    s->Register(SymbolKind::Field, r.name + "." + f.VariableName,
                                fileForLsp, r.line, 0, fieldSig);
                }
            }
        }

        if (verbose)
            std::cout << "[verbose]   registered " << ours.size() << " C record(s) from " << fileForLsp << "\n";
    }

    // Register object-like C macros (as classified by ClassifyRawMacro) as bare global int
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
            // Compile-time value rendered into the --symbol / hover signature so the
            // folded constant is discoverable at the use site (no defensive casts).
            std::string valSuffix;
            if (m.isString)
            {
                // String-literal macro (e.g. #define VERSION "1.2.3"). Intern the literal
                // in the string pool and register a `char*` global pointing at it. CFlat's
                // `char*` matches C's `const char*` ABI; callers can pass directly to any C
                // function taking const char* without an explicit cast.
                tv.TypeName = "char";
                tv.Pointer  = true;
                llvm::Value* strGv = CreateGlobalString(".cmacro." + m.name, m.stringValue);
                c = llvm::cast<llvm::Constant>(strGv);
                valSuffix = std::format(" = \"{}\"", m.stringValue);
            }
            else if (m.isFloat)
            {
                // Float/double macro (e.g. M_PI). Always register as `double` - CFlat
                // narrows at use site if assigned to a `float`, and double matches C's
                // default FP promotion in expressions.
                tv.TypeName = "double";
                tv.Pointer  = false;
                c = llvm::ConstantFP::get(builder->getDoubleTy(), m.floatValue);
                valSuffix = std::format(" = {}", m.floatValue);
            }
            else if (m.isFuncPtr)
            {
                // Function-pointer constant macro (e.g. SIG_IGN). Register as a
                // function<R(P...)> global initialized to a constant fat struct
                // {thunk_const, env=intToPtr(value)} - same wire shape as a
                // C-returned function pointer, just frozen at link time.
                tv = m.funcPtrTV;
                tv.VariableName = m.name;

                // Build the C-side LLVM FunctionType for the thunk lookup.
                std::vector<llvm::Type*> paramTypes;
                for (const auto& p : tv.FuncPtrParams)
                {
                    TypeAndValue pTV; pTV.TypeName = p.TypeName; pTV.Pointer = p.Pointer;
                    paramTypes.push_back(GetType(pTV));
                }
                TypeAndValue retTV;
                retTV.TypeName = tv.FuncPtrReturnTypeName;
                retTV.Pointer  = tv.FuncPtrReturnPointer;
                auto* cFnTy = llvm::FunctionType::get(GetType(retTV), paramTypes, false);
                auto* thunk = GetOrCreateCFuncPtrThunk(cFnTy);

                auto* i8PtrTy = builder->getInt8Ty()->getPointerTo();
                auto* closureTy = GetClosureFatPtrType();
                llvm::Constant* thunkC = llvm::ConstantExpr::getBitCast(thunk, i8PtrTy);
                llvm::Constant* envC = llvm::ConstantExpr::getIntToPtr(
                    builder->getInt64((uint64_t)m.value), i8PtrTy);
                c = llvm::ConstantStruct::get(closureTy, { thunkC, envC });
            }
            else if (m.isPointer)
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
                // Sentinel pointers are bit patterns (INVALID_HANDLE_VALUE = -1); show
                // the full 64-bit pattern in hex, matching how the C macro reads.
                valSuffix = std::format(" = 0x{:x}", (uint64_t)m.value);
            }
            else if (!m.intTypeName.empty())
            {
                // The macro's natural C type is a known integer: register with it, so
                // ((DWORD)-10) is a u32 and a call site taking u32 matches without a
                // cast (GetStdHandle(STD_INPUT_HANDLE)). Build the constant at the
                // type's own width - truncation keeps the C bit pattern exact.
                tv.TypeName = m.intTypeName;
                tv.Pointer  = false;
                unsigned bits = BitfieldStorageBits(m.intTypeName);
                c = llvm::ConstantInt::get(llvm::IntegerType::get(*context, bits),
                                           (uint64_t)m.value, /*isSigned*/ true);
                valSuffix = ConstIntValueSuffix(tv.TypeName, m.value);
            }
            else
            {
                // Natural type unknown: width-guess from the folded value.
                bool wide = (m.value < INT32_MIN || m.value > INT32_MAX);
                tv.TypeName = wide ? "i64" : "int";
                tv.Pointer  = false;
                c = wide
                    ? static_cast<llvm::Constant*>(builder->getInt64((uint64_t)m.value))
                    : static_cast<llvm::Constant*>(builder->getInt32((uint32_t)(int32_t)m.value));
                valSuffix = ConstIntValueSuffix(tv.TypeName, m.value);
            }
            CreateGlobalVariable(tv, c);
            ++registered;

            if (auto* s = GetSymbolSink())
                s->Register(SymbolKind::Variable, m.name, m.file, m.line, m.col < 0 ? 0 : m.col,
                            tv.TypeName + (tv.Pointer ? "* " : " ") + m.name + valSuffix);
        }
        if (verbose && !macros.empty())
            std::cout << "[verbose]   registered " << registered << " C macro constant(s) (of "
                      << macros.size() << " object-like candidates)\n";
    }

    // Tokenize a function-like macro body and verify every token is on a safe allowlist:
    // integer + decimal-float literals (int suffix stripped, float suffix kept), char
    // literals, parameter references, parens, the operators + - * / % & | ^ ~ ! << >> <
    // <= > >= == != && || ? : , calls into already-known C functions, and bare references
    // to known global / enum constants. The translated body is returned via 'out' on
    // success. Unknown identifiers, strings, member access ('.', '->'), hex floats, casts,
    // '#'/'##', '['/']', and the comma operator outside a call return false and the macro
    // is dropped (silently, so binding a header never spews errors for macros cflat cannot
    // express). Identifier resolution is validated against functionTable / globalNamedVariable
    // here so the generated source only ever references symbols that already resolve.
    bool TranslateMacroBody(const CFunctionMacroEntry& m, std::string& out) const
    {
        std::unordered_set<std::string> paramSet(m.params.begin(), m.params.end());
        out.clear();
        out.reserve(m.body.size());
        const std::string& s = m.body;
        size_t i = 0;
        bool hasContent = false;

        // Paren-context stack: 'c' = argument list of a validated call (',' allowed),
        // 'g' = a grouping paren (',' would be the comma operator - rejected). A call
        // identifier sets pendingCallParen so the next '(' becomes a 'c' context.
        std::vector<char> parenCtx;
        bool pendingCallParen = false;

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

            // Identifier: a parameter, a call into a known function, or a bare reference to
            // a known global / enum constant. Anything else (unknown name) drops the macro.
            if (std::isalpha((unsigned char)c) || c == '_')
            {
                size_t start = i;
                while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_')) ++i;
                std::string ident = s.substr(start, i - start);
                if (paramSet.count(ident)) { out += ident; hasContent = true; continue; }

                // Peek past whitespace: an identifier followed by '(' is a call.
                size_t j = i;
                while (j < s.size() && std::isspace((unsigned char)s[j])) ++j;
                bool isCall = (j < s.size() && s[j] == '(');
                if (isCall)
                {
                    if (!functionTable.count(ident)) return false;   // unknown callee: drop
                    pendingCallParen = true;
                }
                else if (!globalNamedVariable.count(ident))
                {
                    return false;   // unknown bare identifier (not a constant): drop
                }
                out += ident;
                hasContent = true;
                continue;
            }

            // Numeric literal. Decimal integers and decimal floats are supported; integer
            // suffixes (u/U/l/L) are stripped (cflat's lexer rejects them) while float
            // suffixes (f/F/l/L) are kept (cflat accepts them). Hex integers are supported;
            // hex floats (a '.'/'p'/'P' after the hex digits) are dropped.
            if (std::isdigit((unsigned char)c) ||
                (c == '.' && i + 1 < s.size() && std::isdigit((unsigned char)s[i + 1])))
            {
                size_t start = i;
                bool isHex = (c == '0' && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X'));
                if (isHex)
                {
                    i += 2;
                    while (i < s.size() && std::isxdigit((unsigned char)s[i])) ++i;
                    if (i < s.size() && (s[i] == '.' || s[i] == 'p' || s[i] == 'P'))
                        return false;   // hex float: drop
                    out += s.substr(start, i - start);
                    while (i < s.size() && (s[i] == 'u' || s[i] == 'U' || s[i] == 'l' || s[i] == 'L')) ++i;
                    hasContent = true;
                    continue;
                }

                bool isFloat = false;
                while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
                if (i < s.size() && s[i] == '.')
                {
                    isFloat = true;
                    ++i;
                    while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
                }
                if (i < s.size() && (s[i] == 'e' || s[i] == 'E'))
                {
                    size_t save = i;
                    ++i;
                    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
                    if (i < s.size() && std::isdigit((unsigned char)s[i]))
                    {
                        isFloat = true;
                        while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
                    }
                    else { i = save; }   // a stray 'e' that is not an exponent
                }
                out += s.substr(start, i - start);
                if (isFloat)
                    while (i < s.size() && (s[i] == 'f' || s[i] == 'F' || s[i] == 'l' || s[i] == 'L'))
                        { out += s[i]; ++i; }
                else
                    while (i < s.size() && (s[i] == 'u' || s[i] == 'U' || s[i] == 'l' || s[i] == 'L')) ++i;
                hasContent = true;
                continue;
            }

            // Char literal: 'x' / '\n' etc. - cflat accepts the same C escape forms. The
            // closing quote must be found (respecting backslash escapes) or the macro drops.
            if (c == '\'')
            {
                size_t start = i;
                ++i;
                while (i < s.size() && s[i] != '\'')
                {
                    if (s[i] == '\\' && i + 1 < s.size()) i += 2;
                    else ++i;
                }
                if (i >= s.size()) return false;   // unterminated
                ++i;                                // closing quote
                out += s.substr(start, i - start);
                hasContent = true;
                continue;
            }

            // String literals and disallowed punctuation.
            if (c == '"')                                        return false;
            if (c == '#' || c == '[' || c == ']' || c == '.' || c == ';') return false;
            if (c == '-' && i + 1 < s.size() && s[i + 1] == '>') return false; // ->

            // Parens: track call vs grouping context so ',' is only allowed in a call's
            // argument list.
            if (c == '(')
            {
                parenCtx.push_back(pendingCallParen ? 'c' : 'g');
                pendingCallParen = false;
                out += c; ++i; hasContent = true; continue;
            }
            if (c == ')')
            {
                if (!parenCtx.empty()) parenCtx.pop_back();
                out += c; ++i; hasContent = true; continue;
            }
            if (c == ',')
            {
                if (parenCtx.empty() || parenCtx.back() != 'c') return false; // comma operator: drop
                out += c; ++i; hasContent = true; continue;
            }

            // Two-char operators kept intact.
            if (i + 1 < s.size())
            {
                std::string two = s.substr(i, 2);
                static const std::unordered_set<std::string> twoChar = {
                    "<=", ">=", "==", "!=", "&&", "||", "<<", ">>"
                };
                if (twoChar.count(two)) { out += two; i += 2; hasContent = true; continue; }
            }

            // Single-character operators.
            static const std::string allowedSingle = "+-*/%&|^~!<>?:";
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
        // A '(' immediately preceded by an identifier char is a CALL's argument paren
        // (e.g. ml_add(x)); never strip those or the call would be mangled.
        size_t pos = 0;
        while ((pos = out.find('(', pos)) != std::string::npos)
        {
            if (pos > 0)
            {
                char prev = out[pos - 1];
                if (std::isalnum((unsigned char)prev) || prev == '_') { ++pos; continue; }
            }
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

    // Bind a prebuilt C library by extracting its header's declarations + enum constants.
    // No object is produced (unlike CompileCFile) - the library is linked via --c-lib in
    // EmitExecutable. Results are cached per header+include-dir set, mirroring
    // ExtractCSignatures. Runs in LSP mode too (registration only), so headers contribute
    // hover / completion / go-to-definition.
    // Loud, actionable diagnostic for a header that does not compile on its own because it
    // relies on a prerequisite header having been included first (the classic Windows SDK leaf
    // case: tlhelp32.h uses HANDLE/DWORD without including the header that defines them). The
    // compiler does not know which header is the prerequisite - that belongs in the user's
    // source - so the message teaches the grouped-import fix generically. Only fires for a
    // standalone single-header import; a header already in a group that still fails names the
    // group so the user can fix the order or add the missing prerequisite.
    void ReportOrphanHeader(const std::vector<std::string>& headerPaths, const std::string& clangErr)
    {
        std::string name = std::filesystem::path(headerPaths.front()).filename().string();
        std::string detail = clangErr.empty() ? "a required type is undefined" : clangErr;
        if (headerPaths.size() > 1)
        {
            std::string grp;
            for (size_t i = 0; i < headerPaths.size(); ++i)
                grp += (i ? ", \"" : "\"") + std::filesystem::path(headerPaths[i]).filename().string() + "\"";
            LogError(std::format(
                "C header '{}' did not compile in this group ({}). Reorder the group so the "
                "prerequisite header comes first, or add the missing one: import {{ {} }};",
                name, detail, grp));
            return;
        }
        LogError(std::format(
            "C header '{}' does not compile on its own ({}). It likely needs a prerequisite "
            "header included first. Import them together as one group so they share a single "
            "translation unit, e.g. import {{ \"prerequisite.h\", \"{}\" }};",
            name, detail, name));
    }

    // Single-header convenience wrapper - the common case (one `import "x.h";`).
    bool CompileCHeader(const std::string& headerPath, const std::vector<std::string>& extraDefines = {},
                        bool diskCache = false)
    {
        return CompileCHeaderGroup(std::vector<std::string>{ headerPath }, extraDefines, diskCache);
    }

    // Bind one or more C headers that share a single translation unit (a grouped import
    // `import {"windows.h","tlhelp32.h"};`). The headers are extracted together so an earlier
    // entry satisfies a later one's prerequisites; registration scope stays per the union of
    // their directories (see ExtractCHeaderClang). The cache key folds the full ordered list,
    // so a header extracted standalone never collides with the same header extracted after a
    // prerequisite. Freshness folds every group member's mtime/hash.
    bool CompileCHeaderGroup(const std::vector<std::string>& headerPaths,
                             const std::vector<std::string>& extraDefines = {},
                             bool diskCache = false)
    {
        if (headerPaths.empty()) return true;

        // Real path per header for cache identity + LSP attribution; the first is the primary
        // (labels diagnostics / attributes registered decls, mirroring the single-header path).
        std::vector<std::string> realPaths;
        realPaths.reserve(headerPaths.size());
        for (const auto& h : headerPaths)
        {
            llvm::SmallString<256> rp;
            realPaths.push_back(!llvm::sys::fs::real_path(h, rp) ? rp.str().str() : h);
        }
        const std::string& fileForLsp = realPaths.front();

        // Fold the ordered header list, the include-dir set and the defines into the cache key:
        // the same header under different --c-include roots / --c-define values, or extracted
        // standalone vs. after a prerequisite header, can expose different decls and must not
        // collide on a stale entry.
        std::string cacheKey;
        for (const auto& rp : realPaths)       cacheKey += "|H" + rp;
        for (const auto& inc : cIncludeDirs_)  cacheKey += "|I" + inc;
        for (const auto& def : cDefines_)      cacheKey += "|D" + def;
        for (const auto& def : extraDefines)   cacheKey += "|d" + def;

        // Combined freshness across the group: max mtime + a fold of every member's content
        // hash, so a change to any header in the group invalidates the entry.
        uint64_t currentHash = 0;
        bool haveHash = false;
        auto hashNow = [&]() -> uint64_t {
            if (!haveHash)
            {
                currentHash = 14695981039346656037ULL;
                for (const auto& rp : realPaths)
                {
                    uint64_t h = 0;
                    HashFileContents(rp, h);
                    currentHash ^= h; currentHash *= 1099511628211ULL;
                }
                haveHash = true;
            }
            return currentHash;
        };

        std::error_code mtEc;
        std::filesystem::file_time_type currentMtime{};
        for (const auto& rp : realPaths)
        {
            std::error_code ec;
            auto mt = std::filesystem::last_write_time(rp, ec);
            if (ec) { mtEc = ec; break; }
            if (mt > currentMtime) currentMtime = mt;
        }

        std::vector<CSigEntry> hitSigs;
        std::vector<CEnumEntry> hitEnums;
        std::vector<CRecordEntry> hitRecords;
        std::vector<CMacroEntry> hitMacros;
        std::vector<CFunctionMacroEntry> hitFuncMacros;
        std::vector<CGlobalEntry> hitGlobals;
        std::vector<std::pair<std::string, std::string>> hitAliases;
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
                    hitMacros = entry.macros; hitFuncMacros = entry.funcMacros; hitGlobals = entry.globals;
                    hitAliases = entry.recordAliases; hit = true;
                }
                else if (hashNow() == entry.hash)
                {
                    if (verbose) std::cout << "[verbose] C header cache hit (hash) for " << fileForLsp << "\n";
                    entry.mtime = currentMtime;
                    hitSigs = entry.sigs; hitEnums = entry.enums; hitRecords = entry.records;
                    hitMacros = entry.macros; hitFuncMacros = entry.funcMacros; hitGlobals = entry.globals;
                    hitAliases = entry.recordAliases; hit = true;
                }
            }
        }
        if (hit)
        {
            // Records before sigs so struct-by-value signatures resolve to the same types.
            RegisterCRecords(hitRecords, fileForLsp);
            RegisterRecordAliases(hitAliases);
            RegisterCSignatures(hitSigs, fileForLsp);
            RegisterCEnums(hitEnums, fileForLsp);
            RegisterCMacros(hitMacros);
            RegisterCFunctionMacros(hitFuncMacros, fileForLsp);
            RegisterCGlobals(hitGlobals, fileForLsp);
            return true;
        }

        // Persistent disk cache (opt-in via the `cache` import clause). On hit we preload the
        // in-memory cache and register the decls, skipping the clang header parse entirely.
        // The disk key folds the same path/include/define set as the in-memory key above.
        std::filesystem::path cHeaderCacheDir = GetCHeaderCacheDir();
        uint64_t diskKey = 0;
        if (diskCache && !mtEc && !cHeaderCacheDir.empty())
        {
            diskKey = CHeaderDiskCacheKey(realPaths, cIncludeDirs_, cDefines_, extraDefines);
            CFileSigCacheEntry diskEntry;
            if (TryLoadCHeaderDiskCache(cHeaderCacheDir, diskKey, currentMtime, hashNow(), diskEntry))
            {
                if (verbose) std::cout << "[verbose] C header disk cache hit for " << fileForLsp << "\n";
                {
                    std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
                    cFileSigCache_[cacheKey] = diskEntry;
                }
                RegisterCRecords(diskEntry.records, fileForLsp);
                RegisterRecordAliases(diskEntry.recordAliases);
                RegisterCSignatures(diskEntry.sigs, fileForLsp);
                RegisterCEnums(diskEntry.enums, fileForLsp);
                RegisterCMacros(diskEntry.macros);
                RegisterCFunctionMacros(diskEntry.funcMacros, fileForLsp);
                RegisterCGlobals(diskEntry.globals, fileForLsp);
                return true;
            }
        }

        std::vector<CSigEntry> sigs;
        std::vector<CEnumEntry> enums;
        std::vector<CRecordEntry> records;
        std::vector<CMacroEntry> macros;
        std::vector<CFunctionMacroEntry> funcMacros;
        std::vector<CGlobalEntry> globals;
        std::vector<std::pair<std::string, std::string>> aliases;
        // Deep mode: collect the transitive include set so the disk entry can validate it.
        bool wantDeps = diskCache && cHeaderCacheDeep_ && !cHeaderCacheDir.empty();
        std::vector<std::string> includes;
        {
            // Functions / enums / records / object-like macro values / function-like macros are
            // all produced by the clang C++ API extractor in one full parse (+ a cheap
            // preprocess-only prepass for macro names). No clang-cl, no libclang.
            llvm::TimeTraceScope extractScope("CHeaderExtract", fileForLsp);
            bool prereqFailure = false;
            std::string prereqMsg;
            if (!ExtractCHeaderClang(realPaths, sigs, enums, records, macros, funcMacros, globals,
                                     aliases, extraDefines, wantDeps ? &includes : nullptr,
                                     &prereqFailure, &prereqMsg))
            {
                if (prereqFailure)
                    ReportOrphanHeader(headerPaths, prereqMsg);
                return false;
            }
        }

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
            entry.globals = globals;
            entry.recordAliases = aliases;
            // Build the transitive dependency list (deep mode): keep only paths that resolve
            // to a real file on disk - this drops the virtual stub and clang pseudo-files,
            // and dedups. A non-existent dep would otherwise poison every later validation.
            if (wantDeps)
            {
                std::unordered_set<std::string> seen;
                for (const auto& inc : includes)
                {
                    std::error_code dec;
                    auto dm = std::filesystem::last_write_time(inc, dec);
                    if (dec) continue;
                    if (!seen.insert(inc).second) continue;
                    CHeaderDep dep;
                    dep.path  = inc;
                    dep.mtime = (int64_t)dm.time_since_epoch().count();
                    HashFileFnv1a(inc, dep.hash);
                    entry.deps.push_back(std::move(dep));
                }
            }
            // --run is read-only: never persist the header cache to disk under run mode, even
            // when the import opted in with a 'cache' clause (the in-memory entry below still
            // serves this compile).
            if (diskCache && !runMode_ && !cHeaderCacheDir.empty())
                WriteCHeaderDiskCache(cHeaderCacheDir, diskKey, currentMtime, hashNow(), entry);
            std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
            cFileSigCache_[cacheKey] = std::move(entry);
        }

        // Records were already registered inside ExtractCHeaderClang.
        RegisterCSignatures(sigs, fileForLsp);
        RegisterCEnums(enums, fileForLsp);
        RegisterCMacros(macros);
        RegisterCFunctionMacros(funcMacros, fileForLsp);
        RegisterCGlobals(globals, fileForLsp);
        return true;
    }

    // Build a TargetMachine for the current target so the optimizer's PassBuilder
    // has TargetTransformInfo. Without a TM the loop vectorizer cannot cost vector
    // instructions and silently declines to vectorize (only memcpy/memset idioms
    // and SLP still fire). Mirrors the triple/CPU resolution in EmitExecutable.
    // Returns null on failure (the optimizer then runs target-agnostic, as before).
    std::unique_ptr<llvm::TargetMachine> CreateOptTargetMachine()
    {
        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmPrinters();

        std::string triple = (platformValue == 32)
            ? "i686-pc-windows-msvc" : "x86_64-pc-windows-msvc";
        std::string cpu = !targetCpu_.empty()
            ? targetCpu_ : (platformValue == 32 ? std::string("i686") : std::string("x86-64"));

        std::string err;
        const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, err);
        if (!target)
            return nullptr;

        llvm::TargetOptions opt;
        return std::unique_ptr<llvm::TargetMachine>(
            target->createTargetMachine(triple, cpu, "", opt, llvm::Reloc::PIC_));
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

        // --cpu overrides the platform default. The value was already resolved ("native"
        // -> host CPU) and validated in Compile, so it can be used verbatim here.
        if (!targetCpu_.empty())
            cpu = targetCpu_;

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

        {
            llvm::TimeTraceScope codegenScope("ObjectCodegen", exePath);
            llvm::legacy::PassManager pass;
            if (TM->addPassesToEmitFile(pass, dest, nullptr, llvm::CodeGenFileType::ObjectFile))
            {
                std::cerr << "Error: target does not support object file emission\n";
                return false;
            }
            pass.run(*module);
            dest.flush();
            dest.close();
        }

        // ---- Find lld-link and MSVC / Windows SDK lib paths (cache-first) ----
        const std::string arch = (platform == "win32") ? "x86" : "x64";

        // Under -g a PDB is produced beside the exe, so DbgHelp can symbolize CFlat
        // frames at runtime. Compile the in-process crash handler and link it in (with
        // dbghelp.lib below). Best-effort: if the handler compile fails (e.g. core/diagnostic/crashdump.c
        // not deployed or clang-cl missing), CompileCrashHandlerObject logs via LogError and
        // returns false; we then skip the handler-specific link args so the link still
        // succeeds and still produces the PDB - just without the in-process backtrace.
        bool crashHandlerLinked = debugInfo && CompileCrashHandlerObject(arch);

        LinkerPaths linkerPaths_;
        {
            llvm::TimeTraceScope pathScope("FindLinkerPaths", exePath);
            linkerPaths_ = FindLinkerPaths(arch, runtimeDir);
        }
        const std::string& lldLinkPath = linkerPaths_.lldLink;
        const std::string& msvcLibPath = linkerPaths_.msvcLib;
        const std::string& ucrtLibPath = linkerPaths_.ucrtLib;
        const std::string& umLibPath   = linkerPaths_.umLib;

        if (lldLinkPath.empty())
        {
            llvm::sys::fs::remove(objPath);
            std::cerr << "Error: lld-link.exe not found\n";
            return false;
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
        if (crashHandlerLinked)
        {
            // DbgHelp (StackWalk64/SymFromAddr/SymGetLineFromAddr64) for the in-process
            // crash handler. dbghelp.lib lives in the Windows SDK um lib dir (umLibPath,
            // already on the libpath below). Gated on crashHandlerLinked so a failed
            // handler compile does not /INCLUDE a symbol that was never emitted.
            linkArgStrs.push_back("dbghelp.lib");
            // Force-retain the crash handler's .CRT$XCU initializer: it has no external
            // references, so lld-link's /OPT:REF (on by default, even with /DEBUG) would
            // garbage-collect it. x86 decorates the data symbol with a leading underscore.
            linkArgStrs.push_back(arch == "x86"
                ? "/INCLUDE:_cflat_crash_init_"
                : "/INCLUDE:cflat_crash_init_");
        }
        if (!msvcLibPath.empty()) linkArgStrs.push_back("/libpath:" + msvcLibPath);
        if (!ucrtLibPath.empty()) linkArgStrs.push_back("/libpath:" + ucrtLibPath);
        if (!umLibPath.empty())   linkArgStrs.push_back("/libpath:" + umLibPath);

        // AddressSanitizer runtime. The compiler-rt asan import libs ship in the same MSVC
        // lib/<arch> dir as the CRT (already on the libpath above) and the runtime DLL lives
        // in the sibling bin/Host*/<arch> dir. We link the dynamic asan runtime (matches the
        // dynamic CRT, /MD-style msvcrt.lib used below) and force-include the thunk via
        // /wholearchive so its CRT$XI* interceptor-init records are retained. The asan DLL is
        // copied next to the exe after a successful link (see below). Gated on asan_.
        std::string asanDllToCopy;
        if (asan_)
        {
            if (!ResolveAsanRuntime(arch, msvcLibPath, linkArgStrs, asanDllToCopy))
            {
                llvm::sys::fs::remove(objPath);
                for (auto& cObj : cObjectFiles_) llvm::sys::fs::remove(cObj);
                return false;
            }
        }

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

        {
            llvm::TimeTraceScope linkScope("Link", exePath);
            std::string linkErr;
            int rc = llvm::sys::ExecuteAndWait(lldLinkPath, linkArgs, std::nullopt, {}, 0, 0, &linkErr);
            llvm::sys::fs::remove(objPath);
            for (auto& cObj : cObjectFiles_) llvm::sys::fs::remove(cObj);

            if (rc != 0)
            {
                std::cerr << "Error: linking failed (exit " << rc << "): " << linkErr << "\n";
                return false;
            }
        }

        // Self-contained run: copy each --c-lib's sibling runtime DLL next to the exe so
        // the program launches without the user staging DLLs. Conan puts the DLL either
        // beside the import lib or in a ../bin sibling; check both. Best-effort - a static
        // lib has no DLL, which is fine.
        {
            llvm::TimeTraceScope dllScope("CopyCRuntimeDlls", exePath);
            CopyCRuntimeDlls(exePath);
        }

        // The asan runtime is a DLL, so it must sit next to the exe to launch (the
        // instrumented program imports __asan_* from it). Copy it after a successful link.
        if (asan_ && !asanDllToCopy.empty())
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            fs::path dest = fs::path(exePath).parent_path() / fs::path(asanDllToCopy).filename();
            fs::copy_file(asanDllToCopy, dest, fs::copy_options::overwrite_existing, ec);
            if (ec)
                LogError(std::format("--asan: failed to copy asan runtime DLL '{}' next to '{}': {}",
                                     asanDllToCopy, exePath, ec.message()));
            else if (verbose)
                std::cout << "[verbose]   copied asan runtime DLL: " << asanDllToCopy
                          << " -> " << dest.string() << "\n";
        }
        return true;
    }

    // True if a thread-spawn call is reachable from 'main', following direct calls only (a
    // worklist BFS over the static call graph). Used by --run to reject multi-threaded programs
    // up front. Indirect/virtual calls are not followed, but the spawn primitive itself
    // (CreateThread / _beginthreadex) is always invoked by a direct call from the thread
    // wrapper, so the chain main -> ... -> wrapper -> CreateThread is fully direct and caught.
    // Reachability (not mere presence) means importing thread.cb without spawning stays allowed.
    bool ReachesThreadSpawn()
    {
        auto isSpawnPrimitive = [](llvm::StringRef name) {
            return name == "CreateThread" || name == "_beginthreadex" || name == "_beginthread";
        };

        llvm::Function* mainFn = module->getFunction("main");
        if (!mainFn) return false;

        llvm::SmallVector<llvm::Function*, 32> worklist{mainFn};
        llvm::SmallPtrSet<llvm::Function*, 32> visited{mainFn};

        while (!worklist.empty())
        {
            llvm::Function* fn = worklist.pop_back_val();
            for (auto& bb : *fn)
            {
                for (auto& inst : bb)
                {
                    auto* call = llvm::dyn_cast<llvm::CallBase>(&inst);
                    if (!call) continue;
                    llvm::Function* callee = call->getCalledFunction();
                    if (!callee) continue; // indirect/virtual call - not followed
                    if (isSpawnPrimitive(callee->getName()))
                        return true;
                    if (callee->isDeclaration()) continue; // no body to walk into
                    if (visited.insert(callee).second)
                        worklist.push_back(callee);
                }
            }
        }
        return false;
    }

    // --run: JIT the in-memory module and execute its 'main' in-process, writing nothing to
    // disk. The host process (cflat.exe) already links and initialized the dynamic CRT, so
    // the JIT'd code resolves printf/malloc/kernel32 etc. from the DLLs already loaded into
    // this process via a process-wide symbol generator - no CRT startup of our own is needed.
    //
    // Scope (prototype): single-module, host-target programs whose entry is 'int main()'.
    // Not yet handled - C interop objects (cObjectFiles_), --c-lib import libs, and programs
    // whose main takes argv (the 'program' construct / 'int main(move list<string>)'). Those
    // need object/library injection and an argv marshaller respectively. 'runExitCode' returns
    // the program's exit status; the function returns false only on a JIT setup failure.
    bool JitRun(int& runExitCode)
    {
        runExitCode = 0;

        // ORC needs the native target + asm printer registered (EmitExecutable registers all
        // targets for cross-codegen; for in-process JIT only the host target is required).
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();

        if (!module->getFunction("main"))
        {
            LogError("--run: no 'main' function to execute.");
            return false;
        }

        // Determine the entry signature now, before the module is handed to the JIT (which
        // consumes it). Two prototypes are supported: 'int main()' and
        // 'int main(int argc, char** argv)'. The second form receives the program arguments
        // passed after '--' on the command line.
        llvm::FunctionType* mainTy = module->getFunction("main")->getFunctionType();
        unsigned mainParamCount = mainTy->getNumParams();
        bool mainTakesArgv = false;
        if (mainParamCount == 0)
        {
            // int main()
        }
        else if (mainParamCount == 2 && mainTy->getParamType(0)->isIntegerTy() &&
                 mainTy->getParamType(1)->isPointerTy())
        {
            mainTakesArgv = true; // int main(int argc, char** argv)
        }
        else
        {
            LogError("--run: 'main' must be 'int main()' or 'int main(int argc, char** argv)' "
                     "to be executed in-process.");
            return false;
        }
        if (!mainTakesArgv && !runArgs_.empty())
        {
            LogError("--run: program arguments were supplied after '--', but 'main' takes no "
                     "parameters. Declare 'int main(int argc, char** argv)' to receive them.");
            return false;
        }

        // Guard: in-process --run is single-threaded only. The 'program' construct and raw
        // thread<T> both spawn worker threads via Win32 CreateThread, and on a JIT'd module
        // those workers run an SEH-wrapped trampoline (__C_specific_handler) whose unwind
        // tables are not registered by the JIT's memory manager - the worker corrupts/crashes
        // the moment it unwinds. Rather than fault at runtime, detect a reachable spawn at the
        // source and refuse with a helpful error. Reachability from 'main' means importing
        // thread.cb without actually spawning stays allowed.
        if (ReachesThreadSpawn())
        {
            LogError(
                "--run cannot execute a multi-threaded program in-process: this program spawns a "
                "thread (via the 'program' construct or thread<T>), which the in-process JIT does "
                "not support - worker threads need Windows SEH unwind tables that the JIT cannot "
                "register. Compile to an exe instead: cflat <input> -o <output.exe>.");
            return false;
        }

        // Build an LLJIT for the host. JITTargetMachineBuilder::detectHost picks up the host
        // triple, CPU, and features so the JIT'd code matches this machine.
        auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
        if (!jtmb)
        {
            LogError(std::format("--run: could not detect host machine: {}",
                                 llvm::toString(jtmb.takeError())));
            return false;
        }

        auto jitOrErr = llvm::orc::LLJITBuilder()
                            .setJITTargetMachineBuilder(std::move(*jtmb))
                            .create();
        if (!jitOrErr)
        {
            LogError(std::format("--run: failed to create JIT: {}",
                                 llvm::toString(jitOrErr.takeError())));
            return false;
        }
        std::unique_ptr<llvm::orc::LLJIT> jit = std::move(*jitOrErr);

        // The module's data layout must match the JIT's. Set both layout and triple to the
        // JIT's host values before handing the module over.
        module->setDataLayout(jit->getDataLayout());
        module->setTargetTriple(jit->getTargetTriple().str());

        // Resolve external symbols (CRT, kernel32, ws2_32, ...) from the symbols already
        // loaded into this process. GlobalPrefix from the data layout keeps name mangling
        // consistent with the JIT (no prefix on win64, '_' on win32).
        auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            jit->getDataLayout().getGlobalPrefix());
        if (!gen)
        {
            LogError(std::format("--run: failed to install process symbol resolver: {}",
                                 llvm::toString(gen.takeError())));
            return false;
        }
        jit->getMainJITDylib().addGenerator(std::move(*gen));

        // Supply __emutls_get_address (see cflat_jit::CflatEmutlsGetAddress). Emulated TLS is
        // how the host target lowers thread-locals, and runtime.cb's allocator hooks are
        // thread-local, so essentially every program needs this helper.
        {
            llvm::orc::SymbolMap syms;
            syms[jit->mangleAndIntern("__emutls_get_address")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&cflat_jit::CflatEmutlsGetAddress),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            if (auto err = jit->getMainJITDylib().define(
                    llvm::orc::absoluteSymbols(std::move(syms))))
            {
                LogError(std::format("--run: failed to define __emutls_get_address: {}",
                                     llvm::toString(std::move(err))));
                return false;
            }
        }

        // Hand the module (and its context) to the JIT. After this the backend's module is
        // consumed - fine, since --run executes and the process exits.
        llvm::orc::ThreadSafeContext tsc(std::move(context));
        if (auto err = jit->addIRModule(
                llvm::orc::ThreadSafeModule(std::move(module), std::move(tsc))))
        {
            LogError(std::format("--run: failed to add module to JIT: {}",
                                 llvm::toString(std::move(err))));
            return false;
        }

        // Run static initializers (llvm.global_ctors / .CRT$XCU) before main.
        if (auto err = jit->initialize(jit->getMainJITDylib()))
        {
            LogError(std::format("--run: static initializer execution failed: {}",
                                 llvm::toString(std::move(err))));
            return false;
        }

        auto mainSym = jit->lookup("main");
        if (!mainSym)
        {
            LogError(std::format("--run: could not find 'main': {}",
                                 llvm::toString(mainSym.takeError())));
            return false;
        }

        // Dispatch on the entry signature detected above.
        if (mainTakesArgv)
        {
            // Build a C-style argv: argv[0] is the program (source file) name, argv[1..] are the
            // user's program arguments, and argv[argc] is NULL per the C convention. The backing
            // std::strings outlive the call; std::string::data() is NUL-terminated (C++11+), so
            // each element is a valid C string.
            std::vector<std::string> argvStorage;
            argvStorage.reserve(runArgs_.size() + 1);
            argvStorage.push_back(sourceFileName.empty() ? std::string("program") : sourceFileName);
            for (const auto& a : runArgs_)
                argvStorage.push_back(a);

            std::vector<char*> argv;
            argv.reserve(argvStorage.size() + 1);
            for (auto& s : argvStorage)
                argv.push_back(s.data());
            argv.push_back(nullptr);

            auto mainPtr = mainSym->toPtr<int (*)(int, char**)>();
            runExitCode = mainPtr(static_cast<int>(argvStorage.size()), argv.data());
        }
        else
        {
            auto mainPtr = mainSym->toPtr<int (*)()>();
            runExitCode = mainPtr();
        }

        // Run static destructors (atexit-style) registered via the JIT.
        if (auto err = jit->deinitialize(jit->getMainJITDylib()))
            llvm::consumeError(std::move(err)); // best-effort; program already ran

        return true;
    }

    // Resolve the compiler-rt AddressSanitizer runtime for --asan and add it to the link.
    // The dynamic asan import libs (clang_rt.asan_dynamic-<suffix>.lib and the matching
    // runtime-thunk) ship in the MSVC lib/<arch> dir (msvcLibDir, already on the libpath),
    // and the runtime DLL lives in the sibling bin/Host{x64,x86}/<arch> dir. Appends the two
    // libs plus a /wholearchive for the thunk to linkArgStrs and returns the absolute DLL
    // path to deploy in dllSrcOut. On any missing piece, reports via LogError and returns
    // false (the runtime ABI is version-pinned, so a missing/mismatched runtime is fatal -
    // we never silently fall back). suffix: x86_64 for x64, i386 for x86.
    bool ResolveAsanRuntime(const std::string& arch, const std::string& msvcLibDir,
                            std::vector<std::string>& linkArgStrs, std::string& dllSrcOut)
    {
        namespace fs = std::filesystem;
        if (msvcLibDir.empty())
        {
            LogError("--asan: MSVC lib directory not found, cannot locate the asan runtime "
                     "(clang_rt.asan_dynamic). Ensure Visual Studio with the C++ "
                     "AddressSanitizer component is installed.");
            return false;
        }

        const std::string suffix = (arch == "x86") ? "i386" : "x86_64";
        const std::string dynLib   = "clang_rt.asan_dynamic-" + suffix + ".lib";
        const std::string thunkLib = "clang_rt.asan_dynamic_runtime_thunk-" + suffix + ".lib";

        fs::path libDir(msvcLibDir);
        for (const std::string& lib : { dynLib, thunkLib })
        {
            std::error_code ec;
            if (!fs::exists(libDir / lib, ec))
            {
                LogError(std::format("--asan: asan runtime import library '{}' not found in "
                                     "'{}'. Install the 'C++ AddressSanitizer' component for "
                                     "this MSVC toolset.", lib, msvcLibDir));
                return false;
            }
        }

        // libDir is already on the /libpath, so reference the libs by name. The thunk must be
        // pulled in whole so its interceptor init records are not GC'd by /OPT:REF.
        linkArgStrs.push_back(dynLib);
        linkArgStrs.push_back(thunkLib);
        linkArgStrs.push_back("/wholearchive:" + thunkLib);

        // The DLL lives under the MSVC version root: msvcLibDir is <ver>/lib/<arch>, so the
        // version root is two levels up; the DLL is at <ver>/bin/Host{x64,x86}/<arch>/.
        const std::string dllName = "clang_rt.asan_dynamic-" + suffix + ".dll";
        fs::path verRoot = libDir.parent_path().parent_path(); // <ver>/lib/<arch> -> <ver>
        for (const char* host : { "Hostx64", "Hostx86" })
        {
            fs::path candidate = verRoot / "bin" / host / arch / dllName;
            std::error_code ec;
            if (fs::exists(candidate, ec))
            {
                dllSrcOut = candidate.string();
                return true;
            }
        }

        LogError(std::format("--asan: asan runtime DLL '{}' not found under '{}\\bin'. The "
                             "import library was present but the matching runtime DLL is "
                             "missing; reinstall the 'C++ AddressSanitizer' component.",
                             dllName, verRoot.string()));
        return false;
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

    // Creates __shim_<name>(original params..., i8* env) that ignores env and calls original.
    // Env is the trailing param (the closure ABI is env-last) so a non-capturing lambda's
    // invoker is bitcast-compatible with a bare C function pointer; this shim mirrors that layout.
    llvm::Function* GetOrCreateFunctionShim(llvm::Function* original)
    {
        std::string shimName = "__shim_" + original->getName().str();
        if (auto* existing = module->getFunction(shimName)) return existing;

        auto* origTy  = original->getFunctionType();
        auto* i8PtrTy = builder->getInt8Ty()->getPointerTo();

        std::vector<llvm::Type*> shimParamTypes;
        for (auto* paramTy : origTy->params())
            shimParamTypes.push_back(paramTy);
        shimParamTypes.push_back(i8PtrTy); // env (trailing, ignored)

        auto* shimTy = llvm::FunctionType::get(origTy->getReturnType(), shimParamTypes, false);
        auto* shim   = llvm::Function::Create(shimTy, llvm::Function::InternalLinkage, shimName, module.get());

        auto* entry = llvm::BasicBlock::Create(*context, "entry", shim);
        llvm::IRBuilder<> b(entry);

        // Forward every param except the trailing env to the original.
        std::vector<llvm::Value*> callArgs;
        for (unsigned i = 0; i + 1 < shim->arg_size(); ++i)
            callArgs.push_back(shim->getArg(i));

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

    // Thunk used to wrap a bare C function pointer (returned from an extern C call) as a
    // closure {thunk, env=cfnptr}. The thunk's `env` slot carries the real C function
    // pointer at runtime; CreateIndirectCall passes env as the trailing arg, so the thunk
    // reads env back, bitcasts it to the C signature, and tail-calls through it.
    // One thunk per signature - keyed by the C-compatible LLVM FunctionType.
    llvm::Function* GetOrCreateCFuncPtrThunk(llvm::FunctionType* cFnTy)
    {
        // Build a stable signature key (mangled function type) so we share thunks across
        // identical signatures (e.g. multiple `int(int,int)` callbacks).
        std::string key;
        llvm::raw_string_ostream os(key);
        os << "__c_fnptr_thunk_";
        cFnTy->getReturnType()->print(os);
        for (auto* pt : cFnTy->params()) { os << "_"; pt->print(os); }
        os.flush();
        // LLVM may emit punctuation; sanitize to identifier-friendly chars.
        for (char& c : key) if (!std::isalnum((unsigned char)c)) c = '_';

        if (auto* existing = module->getFunction(key)) return existing;

        auto* i8PtrTy = builder->getInt8Ty()->getPointerTo();
        std::vector<llvm::Type*> thunkParams;
        for (auto* pt : cFnTy->params()) thunkParams.push_back(pt);
        thunkParams.push_back(i8PtrTy); // env (trailing) carries the real C fn ptr at runtime
        auto* thunkTy = llvm::FunctionType::get(cFnTy->getReturnType(), thunkParams, false);

        auto* thunk = llvm::Function::Create(thunkTy, llvm::Function::InternalLinkage, key, *module);

        llvm::IRBuilder<> b(llvm::BasicBlock::Create(*context, "entry", thunk));
        auto* envArg = thunk->getArg((unsigned)thunk->arg_size() - 1);
        auto* fnPtr  = b.CreateBitCast(envArg, cFnTy->getPointerTo(), "cfn");
        std::vector<llvm::Value*> callArgs;
        for (unsigned i = 0; i + 1 < thunk->arg_size(); ++i) callArgs.push_back(thunk->getArg(i));
        if (cFnTy->getReturnType()->isVoidTy())
        {
            b.CreateCall(cFnTy, fnPtr, callArgs);
            b.CreateRetVoid();
        }
        else
        {
            b.CreateRet(b.CreateCall(cFnTy, fnPtr, callArgs));
        }
        return thunk;
    }

    // Wraps a C-returned bare function pointer in a {thunk, cfnptr-as-env} fat struct
    // so CFlat's indirect-call machinery (which passes env as the trailing arg) routes through
    // the thunk and reaches the real C function with the right argument layout.
    llvm::Value* WrapCFuncPtrAsFatStruct(llvm::Value* cFnPtrValue, const TypeAndValue& fpTV)
    {
        auto* i8PtrTy = builder->getInt8Ty()->getPointerTo();
        // Build the C-side LLVM FunctionType from the TypeAndValue (no env arg).
        std::vector<llvm::Type*> paramTypes;
        for (const auto& p : fpTV.FuncPtrParams)
        {
            TypeAndValue pTV; pTV.TypeName = p.TypeName; pTV.Pointer = p.Pointer;
            paramTypes.push_back(GetType(pTV));
        }
        TypeAndValue retTV;
        retTV.TypeName = fpTV.FuncPtrReturnTypeName;
        retTV.Pointer  = fpTV.FuncPtrReturnPointer;
        auto* cFnTy = llvm::FunctionType::get(GetType(retTV), paramTypes, false);

        auto* thunk    = GetOrCreateCFuncPtrThunk(cFnTy);
        auto* thunkI8  = builder->CreateBitCast(thunk, i8PtrTy, "thunk_i8");
        auto* envI8    = builder->CreateBitCast(cFnPtrValue, i8PtrTy, "cfnret_i8");
        auto* closureTy = GetClosureFatPtrType();
        llvm::Value* fat = llvm::UndefValue::get(closureTy);
        fat = builder->CreateInsertValue(fat, thunkI8, {0u});
        fat = builder->CreateInsertValue(fat, envI8,   {1u});
        return fat;
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

    // True if the named type has a destructor. Used by the LSP unused-locals check
    // to skip RAII locals, whose declaration alone is the point (e.g. a `lock`).
    bool TypeHasDestructor(const std::string& structName) const
    {
        auto it = dataStructures.find(structName);
        return it != dataStructures.end() && it->second.Destructor != nullptr;
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
        // An interface trivially satisfies a constraint to itself: `T : IMessage`
        // is met by T == IMessage. This lets a generic constrained to a marker
        // interface be instantiated with that interface as the default payload
        // (e.g. arena_channel<IMessage>).
        if (typeName == ifaceName && interfaceTable.count(typeName)) return true;

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

            // Synthetic bitfield storage slots (`__bfN`) are not user-visible;
            // the named bitfields they pack are emitted from sd.Bitfields below.
            if (field.IsBitfieldStorage) continue;

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

        // Emit named bitfields from the side-table. These are not in StructFields
        // (only their __bfN storage slots are), so they need their own pass.
        for (const auto& bf : sd.Bitfields)
        {
            // Skip [Private] bitfields
            bool isPrivate = false;
            for (const auto& ann : bf.Annotations)
            {
                if (ann.Name == "Private") { isPrivate = true; break; }
            }
            if (isPrivate) continue;

            const auto& storageField = sd.StructFields[bf.StorageFieldIndex];
            auto* storageTy = compiler->GetType(storageField);
            auto* storagePtr = compiler->builder->CreateStructGEP(sd.StructType, objPtr,
                bf.StorageFieldIndex, bf.Name + "_bf_ptr");
            auto* word = compiler->builder->CreateLoad(storageTy, storagePtr);

            unsigned w = bf.BitWidth;
            unsigned off = bf.BitOffset;
            unsigned storageBits = (unsigned)word->getType()->getIntegerBitWidth();

            llvm::Value* extracted;
            if (bf.IsUnsigned || bf.TypeName == "bool")
            {
                auto* shr = compiler->builder->CreateLShr(word,
                    llvm::ConstantInt::get(word->getType(), off));
                uint64_t mask = (w == 64) ? ~uint64_t(0) : ((uint64_t(1) << w) - 1);
                extracted = compiler->builder->CreateAnd(shr,
                    llvm::ConstantInt::get(word->getType(), mask));
            }
            else
            {
                unsigned leftShift = storageBits - w - off;
                auto* shl = compiler->builder->CreateShl(word,
                    llvm::ConstantInt::get(word->getType(), leftShift));
                extracted = compiler->builder->CreateAShr(shl,
                    llvm::ConstantInt::get(word->getType(), storageBits - w));
            }

            auto nameNV = compiler->MakeStringLiteralNV(bf.Name);

            if (bf.TypeName == "bool")
            {
                // Truncate to i1 for visitBool.
                auto* asBool = compiler->builder->CreateICmpNE(extracted,
                    llvm::ConstantInt::get(extracted->getType(), 0));
                NamedVariable boolNV;
                boolNV.Primary = asBool;
                boolNV.TypeAndValue.TypeName = "bool";
                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitBool",
                    {nameNV, boolNV});
            }
            else
            {
                auto* widened = compiler->Upconvert(extracted,
                    compiler->builder->getInt32Ty(), bf.IsUnsigned);
                NamedVariable intNV;
                intNV.Primary = widened;
                intNV.TypeAndValue.TypeName = "int";
                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitInt",
                    {nameNV, intNV});
            }
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

    // externalDecl=true emits a declaration-only global (null initializer + ExternalLinkage):
    // an external reference the linker resolves to a definition elsewhere (a C library symbol,
    // or a CFlat `extern` global defined in another translation unit). Mutable, C-style - the
    // global is NOT zero-initialized here (that would emit a conflicting definition).
    llvm::GlobalVariable* CreateGlobalVariable(TypeAndValue typeValue, llvm::Constant* initValue, bool threadLocal = false, uint64_t userAlign = 0, bool externalDecl = false)
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
                // Widen a narrower FP constant to the global's type. A bare float literal
                // (1.0 is typed float) widens to a double global - float -> double is an
                // implicit widening conversion. Narrowing is left alone so it still errors.
                if (destinationType->isFloatingPointTy() &&
                    fpValue->getType()->getScalarSizeInBits() < destinationType->getScalarSizeInBits())
                {
                    llvm::APFloat widened = fpValue->getValueAPF();
                    bool losesInfo = false;
                    widened.convert(destinationType->getFltSemantics(),
                        llvm::APFloat::rmNearestTiesToEven, &losesInfo);
                    initValue = llvm::ConstantFP::get(destinationType->getContext(), widened);
                }
            }
            // Coerce a null-value (e.g. nullptr) to the destination type when types differ.
            // Handles fat-ptr structs and function pointers initialized with = nullptr.
            else if (initValue->isNullValue() && initValue->getType() != destinationType)
            {
                initValue = llvm::Constant::getNullValue(destinationType);
            }
        }
        else if (!externalDecl)
        {
            // Zero-initialize - works for all types: primitives, pointers, structs, fat-ptrs.
            initValue = llvm::Constant::getNullValue(destinationType);
        }
        // externalDecl with no initValue: leave initValue null so the GlobalVariable below is a
        // declaration (external reference), not a definition.

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

        if (diBuilder && diFile && !typeValue.VariableName.empty() && !externalDecl)
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
        // An abandoned C-imported record (a field type the extractor could not map,
        // e.g. INPUT_RECORD's inline anonymous union) is registered as an unsized
        // opaque shell. It cannot live on the stack - diagnose instead of emitting
        // IR that fails module verification.
        if (!typeValue.Pointer && type != nullptr && type->isStructTy() && !type->isSized())
        {
            LogError(std::format(
                "type '{}' has an incomplete layout (a field type C interop could not import); "
                "it can only be used through a pointer", typeValue.TypeName));
            type = builder->getInt8Ty();  // sized placeholder; the error already aborts the compile
        }
        // A fixed-size array's dimension is already encoded in `type` (GetType
        // returns [N x T], or [N x [M x T]] for multi-dim). Passing that same N
        // again as the alloca element-count would allocate N copies of [N x T] -
        // an N x N over-allocation (e.g. int[1024] -> 4MB instead of 4KB, which
        // both wastes stack and defeats the loop vectorizer's bounds analysis).
        if (typeValue.ConstArraySize > 0)
            arraySize = nullptr;
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
                return llvm::ConstantFP::get(destType, (double)(int64_t)constInt->getZExtValue());
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

    // C integer promotion for the compile-time constant-fold path: a sub-int
    // operand is promoted to int (i32) before the two constants are folded.
    // Without this, binary folding of small literals overflows in the literal's
    // minimal storage width - the literal 1 is stored as i8, so (i8)1 << 20 folds
    // to 0 and (i8)100 << 1 to -56. Callers gate this on both operands being
    // constants, so runtime narrow arithmetic keeps its width (and still matches
    // narrow return/storage types). The folded constant is truncated back by the
    // receiving storage (CreateCast) when assigned to a narrower type.
    //
    // i32 is the smallest width that actually resolves the overflow: i16 still
    // loses 1 << 20 and 4 * 1024 * 1024, and going wider than int (to i64) is
    // unnecessary - so we cap at exactly i32 (C's int) and never promote i32/i64.
    // i1 (bool) is left alone; promotion targets only the i8/i16 char/short widths.
    llvm::Value* PromoteToInt(llvm::Value* value, bool isUnsigned = false) const
    {
        auto* type = value->getType();
        if (type->isIntegerTy())
        {
            unsigned width = type->getIntegerBitWidth();
            if (width == 8 || width == 16)
                return Upconvert(value, builder->getInt32Ty(), isUnsigned);
        }
        return value;
    }


    /// <summary>
    /// Compare bit width.
    /// </summary>
    /// <returns>Returns -1 for in compatible type, 0 for same, positive number for possible Upconvert.</returns>
    int CompareUpconvert(llvm::Type* srcType, llvm::Type* destType) const
    {
        // A typeless operand (e.g. a bare literal cast through an operator overload,
        // whose NamedVariable carries only Primary - no BaseType, no TypeName) reaches
        // here with a null srcType. Treat it as incompatible (-1) rather than
        // dereferencing null: the caller then reports a clean "no overload matches"
        // diagnostic instead of segfaulting. See internal/string-literal-cast-crash.md.
        if (srcType == nullptr || destType == nullptr)
            return -1;

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

        // A function address converted to a scalar is the forgot-the-parens bug
        // ('double pi = Math.PI;') - reject before it bitcasts into garbage.
        if (destType->isIntegerTy() || destType->isFloatingPointTy())
            RejectBareFunctionValue(value);

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

        // Aggregate operand: the frontend should have decayed a fixed array to a
        // pointer (or rejected the cast) before reaching here. Emit a diagnostic
        // instead of a bitcast LLVM's verifier would reject.
        if (srcType->isAggregateType())
        {
            LogError("cannot cast an aggregate value - a fixed array decays to a pointer to its first element");
            return value;
        }

        // Fallback: BitCast for same-size reinterpretation
        return builder->CreateBitCast(value, destType);
    }

    llvm::Value* CreateCast(llvm::Instruction::CastOps op, llvm::Value* value, llvm::Type* destType)
    {
        return builder->CreateCast(op, value, destType);
    }

    // Returns the bit width of a bitfield's underlying integer type, or 0 if the
    // type is not a permitted bitfield base. C permits any integer type (plus
    // _Bool / bool); CFlat mirrors that and adds the sized integer aliases.
    static unsigned BitfieldStorageBits(const std::string& typeName)
    {
        if (typeName == "bool")  return 8;   // CFlat bool is i8 in storage
        if (typeName == "char" || typeName == "i8"  || typeName == "u8")  return 8;
        if (typeName == "short"|| typeName == "i16" || typeName == "u16") return 16;
        if (typeName == "int"  || typeName == "i32" || typeName == "u32") return 32;
        if (typeName == "long" || typeName == "i64" || typeName == "u64") return 64;
        return 0;
    }

    // MSVC LSB-first bitfield packing. Consumes the user's declList; groups
    // consecutive bitfields with the same underlying type into one storage
    // slot each, populates outBitfields, and returns the storage-slot list
    // that CreateStructType uses to emit the LLVM struct body.
    //
    // Rules (matching MSVC ABI):
    // - A bitfield of width W with the same TypeName as the current run fits if
    //   bitOffset + W <= storageBits; otherwise it starts a new storage unit.
    // - A bitfield with a different TypeName always starts a new unit.
    // - Width-0 unnamed bitfield closes the current unit (next bitfield, even of
    //   the same type, starts a fresh unit).
    // - A bitfield wider than the underlying type is a hard error.
    // - Non-bitfield fields end any open run and pass through as their own slot.
    std::vector<DeclTypeAndValue> PackBitfields(
        const std::vector<DeclTypeAndValue>& in,
        std::vector<BitfieldInfo>& outBitfields)
    {
        std::vector<DeclTypeAndValue> out;
        outBitfields.clear();
        int synthIdx = 0;
        size_t i = 0;
        while (i < in.size())
        {
            const auto& cur = in[i];
            if (!cur.IsBitfield)
            {
                out.push_back(cur);
                i++;
                continue;
            }

            unsigned storageBits = BitfieldStorageBits(cur.TypeName);
            if (storageBits == 0)
            {
                LogError("bitfield '" + cur.VariableName + "' has unsupported underlying type '" + cur.TypeName + "' (must be an integer or bool type)");
                i++;
                continue;
            }
            if (cur.BitWidth > storageBits)
            {
                LogError("bitfield '" + cur.VariableName + "' width " + std::to_string(cur.BitWidth)
                       + " exceeds underlying type '" + cur.TypeName + "' width " + std::to_string(storageBits));
                i++;
                continue;
            }

            // Width-0 acts as a boundary marker; close any open run (here means
            // do not start one). A name is permitted but inaccessible (it has
            // zero bits) - mirrors MSVC's lenient interpretation rather than the
            // strict C standard which only allows the unnamed form.
            if (cur.BitWidth == 0)
            {
                i++;
                continue;
            }

            // Open a new storage unit using the underlying type of the first bitfield.
            unsigned storageIdx = (unsigned)out.size();
            unsigned bitOffset = 0;
            DeclTypeAndValue storage = cur;
            storage.VariableName = "__bf" + std::to_string(synthIdx++);
            storage.IsBitfield = false;     // the storage slot itself isn't a bitfield
            storage.IsBitfieldStorage = true;
            storage.BitWidth = 0;
            storage.BitOffset = 0;
            storage.StorageFieldIndex = 0;
            storage.Initializer = nullptr;  // zeroed by default; per-bitfield init handled at field-init time
            storage.Annotations.clear();
            storage.GuardedBy.clear();
            out.push_back(storage);

            // Greedily attach this and subsequent same-type bitfields that fit.
            while (i < in.size() && in[i].IsBitfield && in[i].TypeName == cur.TypeName)
            {
                const auto& bf = in[i];
                if (bf.BitWidth == 0)
                {
                    // Width-0 marker closes the current unit; consume and stop.
                    i++;
                    break;
                }
                if (bf.BitWidth > storageBits)
                {
                    LogError("bitfield '" + bf.VariableName + "' width " + std::to_string(bf.BitWidth)
                           + " exceeds underlying type '" + bf.TypeName + "' width " + std::to_string(storageBits));
                    i++;
                    continue;
                }
                if (bitOffset + bf.BitWidth > storageBits)
                {
                    // Doesn't fit - leave it for the outer loop to start a new unit.
                    break;
                }
                if (!bf.VariableName.empty())
                {
                    BitfieldInfo info;
                    info.Name = bf.VariableName;
                    info.TypeName = bf.TypeName;
                    info.IsUnsigned = (bf.IsUnsignedInteger() != -1) || bf.TypeName == "bool";
                    info.StorageFieldIndex = storageIdx;
                    info.BitOffset = bitOffset;
                    info.BitWidth = bf.BitWidth;
                    info.Annotations = bf.Annotations;
                    outBitfields.push_back(info);
                }
                // Anonymous (no name) bitfield reserves bits but is unreachable.
                bitOffset += bf.BitWidth;
                i++;
            }
            // Outer-loop continue: next iteration handles whatever didn't fit.
        }
        return out;
    }

    // Create StructType or OpaqueStruct
    //
    // Bitfield contract: if any entry in `typeAndValues` has IsBitfield=true,
    // the caller MUST have already run PackBitfields and passed the storage
    // entries (synthetic `__bfN` slots) here, together with the BitfieldInfo
    // side-table delivered via `bitfields`. CreateStructType itself does NOT
    // pack - the default-ctor path needs the packed list before this call to
    // emit one initializer per LLVM struct element.
    llvm::StructType* CreateStructType(std::string name, std::vector<LLVMBackend::DeclTypeAndValue> typeAndValues, uint64_t userAlign = 0, std::vector<BitfieldInfo>* bitfields = nullptr)
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
                if (bitfields && !bitfields->empty())
                    dataStructures[name].Bitfields = *bitfields;
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
            if (bitfields && !bitfields->empty())
                structData.Bitfields = *bitfields;
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
            // An unsized member (opaque shell / incomplete type) has no layout; sizing it
            // would assert. Callers (RegisterCRecords) abandon such records before reaching
            // here, but guard defensively so this never crashes the compiler.
            if (!t || !t->isSized()) continue;
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

        // Pass module explicitly so CreateGlobalString doesn't dereference the builder's
        // (possibly null) insertion point to find the module.
        auto* gv = builder->CreateGlobalString(text, name, 0, module.get());
        stringPool[text] = gv;
        stringLiteralLenByPtr[gv] = (int32_t)text.size();
        return gv;
    }

    // Element-wise arithmetic and comparison on simd<T,N> values. Either operand may be a scalar,
    // which is splatted across all lanes (with element-type conversion). Both vector operands must
    // share the same lane count and element type. Arithmetic (+ - * /) yields a same-shape vector;
    // a comparison (== != < <= > >=) yields a `<N x i1>` mask (a simd<bool,N>) for use with
    // simd<T,N>.select - the branchless primitive that lets a masked kernel (e.g. LBM bounce-back)
    // stay straight-line and vectorize.
    llvm::Value* CreateVectorOperation(Operation op, llvm::Value* left, llvm::Value* right, bool isUnsigned = false)
    {
        auto* lvt = llvm::dyn_cast<llvm::FixedVectorType>(left->getType());
        auto* rvt = llvm::dyn_cast<llvm::FixedVectorType>(right->getType());
        llvm::FixedVectorType* vt = lvt ? lvt : rvt;

        auto splat = [&](llvm::Value* scalar) -> llvm::Value*
        {
            scalar = ConvertScalarToType(scalar, vt->getElementType());
            return builder->CreateVectorSplat(vt->getElementCount(), scalar);
        };
        if (!lvt) left = splat(left);
        if (!rvt) right = splat(right);

        if (left->getType() != right->getType())
        {
            LogError("simd operands must have the same lane count and element type");
            return left;
        }

        bool isFloat = vt->getElementType()->isFloatingPointTy();
        switch (op)
        {
        case Operation::Add:      case Operation::AddAssignment:
            return isFloat ? builder->CreateFAdd(left, right) : builder->CreateAdd(left, right);
        case Operation::Subtract: case Operation::MinusAssignment:
            return isFloat ? builder->CreateFSub(left, right) : builder->CreateSub(left, right);
        case Operation::Multiply: case Operation::MultiplyAssignment:
            return isFloat ? builder->CreateFMul(left, right) : builder->CreateMul(left, right);
        case Operation::Divide:   case Operation::DivideAssignment:
            return isFloat ? builder->CreateFDiv(left, right)
                 : isUnsigned ? builder->CreateUDiv(left, right) : builder->CreateSDiv(left, right);
        // Comparisons -> <N x i1> mask. FP uses ordered predicates (a NaN lane compares false);
        // integers honour the operands' signedness for the relational ops.
        case Operation::Equal:
            return isFloat ? builder->CreateFCmpOEQ(left, right) : builder->CreateICmpEQ(left, right);
        case Operation::NotEqual:
            return isFloat ? builder->CreateFCmpONE(left, right) : builder->CreateICmpNE(left, right);
        case Operation::Greater:
            return isFloat ? builder->CreateFCmpOGT(left, right)
                 : isUnsigned ? builder->CreateICmpUGT(left, right) : builder->CreateICmpSGT(left, right);
        case Operation::GreaterEqual:
            return isFloat ? builder->CreateFCmpOGE(left, right)
                 : isUnsigned ? builder->CreateICmpUGE(left, right) : builder->CreateICmpSGE(left, right);
        case Operation::Less:
            return isFloat ? builder->CreateFCmpOLT(left, right)
                 : isUnsigned ? builder->CreateICmpULT(left, right) : builder->CreateICmpSLT(left, right);
        case Operation::LessEqual:
            return isFloat ? builder->CreateFCmpOLE(left, right)
                 : isUnsigned ? builder->CreateICmpULE(left, right) : builder->CreateICmpSLE(left, right);
        default:
            LogError("simd supports + - * / and comparisons == != < <= > >= (other operators are not yet supported)");
            return left;
        }
    }

    // Splat a scalar across all lanes of a simd<T,N> value (converting element type as needed).
    // Used to initialize/assign a simd variable from a scalar (e.g. simd<float,8> v = 1.0;).
    llvm::Value* SplatToSimd(llvm::Value* scalar, const TypeAndValue& tv)
    {
        auto* vecTy = llvm::cast<llvm::FixedVectorType>(GetType(tv));
        scalar = ConvertScalarToType(scalar, vecTy->getElementType());
        return builder->CreateVectorSplat(vecTy->getElementCount(), scalar);
    }

    // Convert a scalar to an exact target scalar type, handling both widening (Upconvert)
    // and narrowing (e.g. a double literal into a float lane), which Upconvert alone won't do.
    llvm::Value* ConvertScalarToType(llvm::Value* scalar, llvm::Type* target)
    {
        scalar = Upconvert(scalar, target);
        if (scalar->getType() != target)
            scalar = CreateCast(scalar, target);
        return scalar;
    }

    // Reverse-map an IR function to its source-level name for diagnostics.
    // Only called on the error path, so the linear scan is fine.
    std::string FindFunctionSourceName(const llvm::Function* fn) const
    {
        for (const auto& [name, overloads] : functionTable)
            for (const auto& sym : overloads)
                if (sym.Function == fn)
                    return name;
        return fn->getName().str();
    }

    // A bare llvm::Function reaching value context means the user referenced a
    // function without calling it (e.g. 'Math.PI' instead of 'Math.PI()').
    // Without this check the address either bitcasts into a garbage scalar or
    // trips an LLVM assert inside arithmetic codegen.
    void RejectBareFunctionValue(llvm::Value* value) const
    {
        if (value == nullptr)
            return;
        if (auto* fn = llvm::dyn_cast<llvm::Function>(value))
        {
            std::string name = FindFunctionSourceName(fn);
            LogError(std::format(
                "'{}' is a function used as a value - did you mean '{}()'? (a bare function name is only valid as a function<T> value)",
                name, name));
        }
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

        RejectBareFunctionValue(left);
        RejectBareFunctionValue(right);

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

        // simd<T,N> operands: element-wise vector ops. A <N x float> answers false to the scalar
        // isFloatingPointTy() check below, so vectors must be handled before the scalar paths.
        if (left->getType()->isVectorTy() || right->getType()->isVectorTy())
            return CreateVectorOperation(op, left, right);

        // C integer promotion, scoped to the compile-time fold case (both operands
        // are constants). Widening sub-int constants to i32 stops expressions over
        // small literals (e.g. 1 << 20, 4 * 1024 * 1024) from overflowing in the
        // literals' minimal storage width. Runtime narrow arithmetic is left alone
        // so its result keeps the narrow type and still matches narrow return/storage.
        if (llvm::isa<llvm::ConstantInt>(left) && llvm::isa<llvm::ConstantInt>(right))
        {
            left = PromoteToInt(left);
            right = PromoteToInt(right);
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

        // simd<T,N> operands: element-wise ops. A <N x iX>/<N x float> answers false to the scalar
        // isFloatingPointTy() check below, so vectors must be handled before the integer switch.
        if (left->getType()->isVectorTy() || right->getType()->isVectorTy())
            return CreateVectorOperation(op, left, right, leftIsUnsigned || rightIsUnsigned);

        // C integer promotion (see PromoteToInt), scoped to the compile-time fold
        // case (both operands constant) so runtime narrow arithmetic is unaffected.
        // Signedness is preserved so unsigned narrow constants zero-extend.
        if (llvm::isa<llvm::ConstantInt>(left) && llvm::isa<llvm::ConstantInt>(right))
        {
            left  = PromoteToInt(left,  leftIsUnsigned);
            right = PromoteToInt(right, rightIsUnsigned);
        }

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

        // Strip the OWNED tag (low bit) off the env before the env-last call: an owning heap
        // env carries the tag (lambda Option A), and the invoker expects a clean pointer to the
        // captures block. Borrowed/null env (tag clear) is unchanged by the mask.
        {
            auto* envInt = builder->CreatePtrToInt(envPtr, builder->getInt64Ty());
            auto* masked = builder->CreateAnd(envInt, builder->getInt64(~(uint64_t)1));
            envPtr = builder->CreateIntToPtr(masked, i8PtrTy, "env_untagged");
        }

        // Build invoker function type: (user_params..., i8* env) -> RetType (env-last ABI).
        std::vector<llvm::Type*> paramTypes;
        for (const auto& p : funcPtrType.FuncPtrParams)
        {
            TypeAndValue pTV; pTV.TypeName = p.TypeName; pTV.Pointer = p.Pointer;
            paramTypes.push_back(GetType(pTV));
        }
        paramTypes.push_back(i8PtrTy); // env (trailing)
        TypeAndValue retTV;
        retTV.TypeName = funcPtrType.FuncPtrReturnTypeName;
        retTV.Pointer  = funcPtrType.FuncPtrReturnPointer;
        auto* retTy     = GetType(retTV);
        auto* invokerTy = llvm::FunctionType::get(retTy, paramTypes, false);
        auto* fnPtr     = builder->CreateBitCast(fnPtrI8, invokerTy->getPointerTo(), "fn_ptr");

        // Upconvert user args to match declared param types (the trailing slot is env, skip it).
        // String literals arrive as i8* - wrap them into %string{ptr,len} when the
        // param expects a string value type.
        for (size_t i = 0; i < args.size() && i + 1 < paramTypes.size(); i++)
        {
            auto* destTy = paramTypes[i];
            auto* strTy  = llvm::StructType::getTypeByName(*context, "string");
            if (strTy && destTy == strTy && args[i]->getType()->isPointerTy())
                args[i] = WrapStringLiteralAsString(args[i]);
            else
                args[i] = Upconvert(args[i], destTy);
        }

        // Append env to call args (env-last)
        std::vector<llvm::Value*> fullArgs(args.begin(), args.end());
        fullArgs.push_back(envPtr);

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
        // getZExtValue avoids the isRepresentableByInt64 assert for constants > INT64_MAX.
        return llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(switchType), val->getZExtValue(), false);
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
                FailCompilation("expected error did not occur");
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

    // linkageName: optional override of the emitted LLVM symbol for externs. A namespaced
    // extern (namespace os.windows { extern ... Sleep(...); }) registers in the function
    // table under the qualified lookup name but must link against the bare C symbol.
    void CreateFunctionDeclaration(std::string functionName, LLVMBackend::TypeAndValue returnType, std::vector<LLVMBackend::TypeAndValue> arguments, bool external = false, bool varargs = false, bool returnsOwned = false, bool isMethod = false, bool isStdcall = false, bool isCdecl = false, const std::string& linkageName = {})
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
        std::string mangledName = external ? (linkageName.empty() ? functionName : linkageName)
                                           : ComputeMangledName(functionName, returnType, arguments, varargs);

        if (module->getFunction(mangledName) != nullptr)
        {
            // A repeat declaration under the same lookup name is a no-op. A *different*
            // lookup name for an already-emitted linkage symbol (core's os.windows.Sleep
            // and a header-imported bare Sleep) still registers below, reusing the
            // existing llvm::Function via getOrInsertFunction.
            for (const auto& sym : functionTable[functionName])
                if (sym.UniqueName == mangledName)
                    return;
        }

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

        // CFlat treats null pointer dereferences as defined behavior (hardware fault -> SEH).
        // Ensure the attribute is set even on pre-declared functions that skipped createFunctionProto.
        fn->addFnAttr(llvm::Attribute::NullPointerIsValid);

        // The whole program (user files + core libraries) is compiled into a single module
        // and optimized with the per-module pipeline before lld-link merges any separately
        // compiled C objects. A non-extern CFlat function therefore never needs to be visible
        // by name to anything outside this module, so give it internal linkage. That unlocks
        // interprocedural optimization the optimizer must otherwise withhold from symbols it
        // assumes are externally referenced (dead-function elimination, inline-then-delete,
        // argument promotion / specialization). Exceptions stay external:
        //   - `external` (extern C) functions are the ABI surface for the linked C objects.
        //   - `main` is the linker entry symbol.
        //   - A function whose raw address is handed to a bare C callback is promoted back to
        //     external at the call site (see CreateOverloadedFunctionCall) so the linker keeps
        //     its identity. We only internalize here, where a body is being attached - never on
        //     declaration-only protos (an internal declaration with no body is invalid IR).
        if (!external && functionName != "main")
            fn->setLinkage(llvm::Function::InternalLinkage);

        if (isStdcall && platformValue == 32)
            fn->setCallingConv(llvm::CallingConv::X86_StdCall);
        else if (isCdecl)
            fn->setCallingConv(llvm::CallingConv::C);

        // Thin `int[]` array-view params carry a noalias contract (distinct views point at
        // distinct whole allocations). Stamp LLVM noalias so the loop vectorizer can drop its
        // runtime alias check. Native params map 1:1 to fn args (no sret/byval reordering);
        // skipped for extern C, where pointers alias freely.
        if (!external)
        {
            unsigned ai = 0;
            for (const auto& a : arguments)
            {
                if (a.IsArrayView && ai < fn->arg_size())
                    fn->addParamAttr(ai, llvm::Attribute::NoAlias);
                ++ai;
            }
        }

        createFunctionBlock(fn, functionName, arguments, returnsOwned, returnType.IsArrayView, returnType.TypeName);
        // Sibling of the currentFunctionReturn* fields set inside createFunctionBlock: retain the
        // full return TypeAndValue so a returned lambda literal can adopt a function<> return type.
        currentFunctionReturnTV = returnType;

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

    // True when 'name' resolves to a type in the current compilation: a scalar
    // keyword, an enum, a type alias, an interface, or a registered struct. Used by
    // sizeof/alignof to disambiguate sizeof(Type) from sizeof(variable) - the type
    // meaning wins, matching C.
    bool IsKnownTypeName(const std::string& name) const
    {
        static const std::unordered_set<std::string> scalars = {
            "void", "char", "i8", "u8", "short", "i16", "u16", "int", "i32", "u32",
            "long", "i64", "u64", "float", "double", "bool", "va_list", "auto" };
        if (scalars.count(name)) return true;
        return enumBackingTypes.count(name) > 0 || typeAliases.count(name) > 0
            || interfaceTable.count(name) > 0 || dataStructures.count(name) > 0;
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
        // Resolve user-defined type aliases. A pointer alias (using Handle = void*) keeps its
        // trailing stars in the stored string; peel them into a local pointer-depth count and
        // OR it onto the typeAndValue pointer flags below (storage stays string-shaped).
        int aliasPtrDepth = 0;
        std::vector<uint64_t> aliasArrayDims;  // outer dimension first, from an array alias
        if (!resolvedTypeName.empty())
        {
            auto it = typeAliases.find(resolvedTypeName);
            if (it != typeAliases.end())
            {
                resolvedTypeName = it->second;
                // Array brackets are outermost in the stored string ("int*[3]"); peel them first.
                std::vector<uint64_t> rev;
                while (!resolvedTypeName.empty() && resolvedTypeName.back() == ']')
                {
                    size_t open = resolvedTypeName.rfind('[');
                    if (open == std::string::npos) break;
                    std::string num = resolvedTypeName.substr(open + 1, resolvedTypeName.size() - open - 2);
                    if (num.empty() || num.find_first_not_of("0123456789") != std::string::npos) break;
                    rev.push_back(std::strtoull(num.c_str(), nullptr, 10));
                    resolvedTypeName.erase(open);
                }
                for (auto rit = rev.rbegin(); rit != rev.rend(); ++rit) aliasArrayDims.push_back(*rit);
                while (!resolvedTypeName.empty() && resolvedTypeName.back() == '*')
                {
                    resolvedTypeName.pop_back();
                    aliasPtrDepth++;
                }
            }
        }
        // Namespace-relative type reference: a bare "_SystemInfo" inside namespace
        // os.windows resolves to "os.windows._SystemInfo". Only accept a resolution
        // that names a type, so a sibling function never hijacks a type name.
        if (!resolvedTypeName.empty()
            && dataStructures.find(resolvedTypeName) == dataStructures.end()
            && interfaceTable.count(resolvedTypeName) == 0)
        {
            std::string nsResolved = ResolveQualifiedName(resolvedTypeName);
            if (nsResolved != resolvedTypeName
                && (dataStructures.count(nsResolved) || interfaceTable.count(nsResolved)))
                resolvedTypeName = nsResolved;
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

        // simd<T,N> -> LLVM <N x T> vector primitive. Wrap before pointer wrapping so
        // simd<float,8>* lowers to <8 x float>*. Element must be a numeric scalar.
        if (typeAndValue.IsSimd)
        {
            if (typeAndValue.SimdLanes == 0)
                return type;  // malformed lane count already reported; avoid a 0-width vector
            bool numeric = type->isFloatTy() || type->isDoubleTy()
                || (type->isIntegerTy() && !type->isIntegerTy(1));
            // simd<bool,N> (i1 lanes) is the mask type: the result of a vector comparison and the
            // first argument of simd<T,N>.select. It is a valid simd element even though it is not
            // an arithmetic scalar (you cannot + - * / a mask, but you can store and select with it).
            bool isMask = type->isIntegerTy(1);
            if (!numeric && !isMask)
            {
                LogError(std::format("simd element type must be a numeric scalar "
                    "(i8..i64, u8..u64, float, double) or bool for a mask, got '{}'", resolvedTypeName));
                return type;
            }
            type = llvm::FixedVectorType::get(type, static_cast<unsigned>(typeAndValue.SimdLanes));
        }

        // Apply pointer wrapping to get the element type before array wrapping.
        // This ensures char*[3] -> [3 x ptr] not [3 x i8].
        // aliasPtrDepth folds in a pointer alias's stars (using Handle = void* -> 1) so a call
        // site that passes the un-stripped alias name still lowers correctly.
        bool wantPointer = typeAndValue.Pointer || aliasPtrDepth >= 1;
        bool wantElemPointer = typeAndValue.ElemPointer || aliasPtrDepth >= 2
            || (typeAndValue.Pointer && aliasPtrDepth >= 1);
        if (allowPointer && wantPointer)
        {
            // Note: LLVM doesn't have void ptr, instead use i8 ptr.
            if (type->isVoidTy())
                type = builder->getInt8Ty()->getPointerTo();
            else
                type = type->getPointerTo();
            if (wantElemPointer)
                type = type->getPointerTo();
        }

        // Prefer the explicit ConstArraySize set by the declaration handler; fall back to an
        // array alias's dims (using Vec3 = float[3]) for call sites that pass only the alias name
        // (sizeof(Vec3), a parameter of type Vec3) and never run the per-declarator finalization.
        uint64_t outerDim = typeAndValue.ConstArraySize;
        const std::vector<uint64_t>* innerDims = &typeAndValue.ConstInnerDimensions;
        std::vector<uint64_t> aliasInner;
        if (outerDim == 0 && !aliasArrayDims.empty())
        {
            outerDim = aliasArrayDims[0];
            aliasInner.assign(aliasArrayDims.begin() + 1, aliasArrayDims.end());
            innerDims = &aliasInner;
        }
        if (outerDim > 0)
        {
            // Build from innermost to outermost: T[N1][N2] -> [N1 x [N2 x T]]
            llvm::Type* inner = type;
            for (int i = (int)innerDims->size() - 1; i >= 0; i--)
                inner = llvm::ArrayType::get(inner, (*innerDims)[i]);
            return llvm::ArrayType::get(inner, outerDim);
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

                    // Opaque pointers make every pointer pair look identical to CompareUpconvert.
                    // An argument whose CFlat type is unknown (empty TypeName - primitive pointers
                    // like '&boolVar') binding to a pointer-to-struct parameter is only an IMPLICIT
                    // match, never a perfect one: a perfect match here would let a method's 'this'
                    // swallow any pointer in a free call (e.g. readLine(&eof) resolving to
                    // File.readLine with the bool* as 'this') and beat the exact free overload.
                    // nullptr stays a perfect match for any pointer parameter.
                    if (result == 0 && candidateParamItr->Pointer
                        && IsDataStructure(candidateParamItr->TypeName)
                        && arg.BaseType && arg.BaseType->isPointerTy()
                        && !(arg.Primary && llvm::isa<llvm::ConstantPointerNull>(arg.Primary)))
                        result = 1;

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
                    // Implicit char* -> string coercion: string literal or char* passed to a string param.
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

    llvm::Value* CreateOverloadedFunctionCall(std::string functionName, std::vector<LLVMBackend::NamedVariable> arguments, bool forceRoot = false)
    {
        functionName = ResolveQualifiedName(functionName, forceRoot);

        // Implicit `copy()` synthesis: a value type with no copy() of its own gets a memberwise
        // one generated on demand (the "every value type has an implicit copy() if undefined"
        // rule). Fires for both user `x.copy()` and the compiler's internal copy calls; an
        // existing copy() always wins (HasCopyOverloadFor). Pointers/primitives are skipped.
        if (functionName == "copy" && arguments.size() == 1
            && !arguments[0].TypeAndValue.Pointer
            && !arguments[0].TypeAndValue.TypeName.empty()
            && dataStructures.count(arguments[0].TypeAndValue.TypeName)
            && !HasCopyOverloadFor(arguments[0].TypeAndValue.TypeName))
        {
            // The closure fat type gets an env-cloning copy (not a memberwise one - both its
            // fields are pointers, which a memberwise copy would shallow-share and double-free).
            if (arguments[0].TypeAndValue.TypeName == "__closure_fat_ptr")
                EnsureClosureLifetimeRegistered();
            else
                GetOrCreateMemberwiseCopy(arguments[0].TypeAndValue.TypeName);
        }

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

            // Array-view parameter gate: a raw `T*` must not bind to a `T[]` parameter - that
            // would forge the noalias contract the view promises (a whole, distinct allocation).
            // Placed before the binding branches because an array-view param has Pointer=true and
            // is handled by the pointer-parameter branch below. The reverse (`T[] -> T*` decay)
            // is always safe; a view argument carries IsArrayView and passes.
            if (!inVariadicRange && candParamItr->IsArrayView
                && arg.TypeAndValue.Pointer && !arg.TypeAndValue.IsArrayView)
                LogError(std::format(
                    "cannot pass a raw pointer 'T*' as array-view parameter '{}' ('T[]') - a view "
                    "must span a whole allocation (it comes only from 'new T[n]' or another 'T[]'); "
                    "the 'T[] -> T*' decay is one-way", candParamItr->VariableName));

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
                    // Defensive: a hand-built NamedVariable with no BaseType and no resolvable
                    // TypeName yields a null/void type here; allocating it would crash LLVM.
                    // Report the bad call instead of forging an invalid alloca.
                    if (structTy == nullptr || structTy->isVoidTy())
                    {
                        LogError(std::format(
                            "call to '{}': argument {} (interface parameter '{}') has no resolved type",
                            functionName, argIndex, candParamItr->VariableName));
                        return nullptr;
                    }
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
                    // The CFlat function's raw address escapes into separately-linked C code
                    // that may store and later call it by pointer, so restore external linkage
                    // (CreateFunctionDefinition defaults non-extern functions to internal) to
                    // keep the symbol's identity across the lld-link boundary.
                    if (auto* escFn = llvm::dyn_cast<llvm::Function>(val))
                        if (escFn->getLinkage() == llvm::Function::InternalLinkage)
                            escFn->setLinkage(llvm::Function::ExternalLinkage);
                    if (val && val->getType()->isStructTy())
                    {
                        // The argument is a CFlat closure fat struct {code, env} - a lambda or a
                        // `function<>` variable. A C function pointer is a bare code address with no
                        // env slot. The closure ABI is env-last, so the invoker of a *non-capturing*
                        // lambda (env is the trailing param and is never read) is directly callable
                        // as a C function pointer: the C caller simply never supplies the trailing
                        // env arg. Detect non-capturing by a compile-time-null env field (set only
                        // when the lambda captured nothing - see ParseLambdaExpression) and pass the
                        // bare invoker (field 0). A capturing lambda or a `function<>` variable
                        // cannot be statically proven non-capturing, so it loses captured state
                        // across the C ABI and is rejected instead of silently miscompiling.
                        auto* envField = builder->CreateExtractValue(val, {1u}, "closure_env");
                        if (llvm::isa<llvm::ConstantPointerNull>(envField))
                        {
                            auto* codeField = builder->CreateExtractValue(val, {0u}, "closure_code");
                            val = builder->CreateBitCast(codeField, llvmParamTy, "noncap_lambda_cfn");
                        }
                        else if (!arg.LambdaCaptureNames.empty())
                        {
                            // A capturing lambda literal - name what it captures so the user can see
                            // exactly which variables to drop (or replace with a named function).
                            // Names are already de-duplicated; show at most 5, then summarize the rest.
                            const auto& caps = arg.LambdaCaptureNames;
                            const size_t count = caps.size();
                            const size_t shown = count < 5 ? count : 5;
                            std::string list;
                            for (size_t k = 0; k < shown; k++)
                            {
                                if (k != 0) list += ", ";
                                list += caps[k];
                            }
                            std::string more = count > shown
                                ? std::format(", ... (and {} more)", count - shown) : "";
                            LogError(std::format(
                                "cannot pass to C function-pointer parameter '{}': this lambda captured {} {} [{}{}]. "
                                "A C callback is a bare code pointer and cannot carry captured state - "
                                "pass a non-capturing lambda or a named function.",
                                candParamItr->VariableName, count, (count == 1 ? "variable" : "variables"), list, more));
                        }
                        else
                        {
                            // A stored function<> value - its captures are not known at the call site.
                            LogError(std::format(
                                "cannot pass to C function-pointer parameter '{}': this 'function<>' value may store captured state. "
                                "A C callback is a bare code pointer - pass a non-capturing lambda or a named function.",
                                candParamItr->VariableName));
                        }
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
                        // Implicit char* -> string coercion: string literal or char* passed to a string param.
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
                        // Mask off _len's high OWNED bit before using it as a byte count
                        // (string-redesign FINAL MODEL) - the bit would inflate allocSize to
                        // ~2GB and AV the memcpy when the source is an owned string.
                        auto* srcLen = builder->CreateAnd(
                            builder->CreateExtractValue(value, { 1u }, "litlenraw"),
                            builder->getInt32(0x7FFFFFFF), "litlen");
                        auto* allocSize = builder->CreateAdd(
                            builder->CreateZExt(srcLen, builder->getInt64Ty()),
                            builder->getInt64(1), "litbufsz");
                        NamedVariable szArg;
                        szArg.Primary  = allocSize;
                        szArg.BaseType = builder->getInt64Ty();
                        auto* rawPtr  = CreateOverloadedFunctionCall("operator new", { szArg });
                        auto* heapPtr = builder->CreateBitCast(rawPtr, builder->getInt8Ty()->getPointerTo(), "litbuf");
                        builder->CreateMemCpy(heapPtr, llvm::MaybeAlign(1), srcPtr, llvm::MaybeAlign(1), allocSize);
                        // The callee receives a freshly allocated buffer it owns: set the OWNED bit.
                        auto* ownedLen = builder->CreateOr(srcLen, builder->getInt32(0x80000000), "ownedlen");
                        value = llvm::UndefValue::get(strTy);
                        value = builder->CreateInsertValue(value, heapPtr, { 0u });
                        value = builder->CreateInsertValue(value, ownedLen, { 1u });
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

        // Extern C function returning a function pointer: the LLVM-level return type is a
        // bare ptr but CFlat function<T> variables hold the {fn, env} closure fat struct.
        // Wrap via a per-signature thunk so the indirect-call path (which always prepends
        // env to the args) reaches the real C function correctly.
        if (candidate.ReturnType.IsFunctionPointer
            && result != nullptr
            && result->getType()->isPointerTy())
        {
            result = WrapCFuncPtrAsFatStruct(result, candidate.ReturnType);
        }

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
        lastCallBondByAddress = false;
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
                // A `move string` argument transfers ownership to the callee, which frees
                // it on return. If the argument is an unnamed owned-string temporary (e.g.
                // a chained-concat result passed directly), drop it from the end-of-
                // expression cleanup list so it is not also freed by the caller.
                if (candidate.Parameters[i].TypeName == "string")
                    UnregisterOwnedStringTemp(matched[i].Primary);

                // Interface fat-ptr parameters (non-pointer) borrow the caller's data - don't zero it.
                // The fat-ptr holds a reference to the caller's struct; zeroing would corrupt that data.
                bool isInterfaceBorrow = candidate.Parameters[i].IsInterface && !candidate.Parameters[i].IsInterfacePointer;
                // When the arg expression went through a cast (or similar), matched[i].Storage
                // may be cleared even though CallerName still names the original owning variable.
                // Look up the source by name so we still null its alloca and prevent a double-free.
                llvm::Value* srcStorage = matched[i].Storage;
                llvm::Type*  srcBaseTy  = matched[i].BaseType;
                bool isFieldAccess = !matched[i].FieldName.empty();
                // The CallerName fallback resolves the BASE variable's storage; do NOT use it
                // for a field access or it would null the base pointer instead of the field.
                // A field access already carries its own (GEP) Storage.
                if (srcStorage == nullptr && !isFieldAccess && !matched[i].CallerName.empty())
                {
                    auto ref = FindVariableStorage(matched[i].CallerName);
                    srcStorage = ref.Storage;
                    if (srcBaseTy == nullptr) srcBaseTy = ref.BaseType;
                }
                if (srcStorage != nullptr && !isInterfaceBorrow)
                {
                    // dyn_cast_or_null: a hand-built NamedVariable may carry storage but a null
                    // BaseType. A bare dyn_cast on a null type dereferences null and segfaults
                    // the compiler (the pre-existing list<string>-json crash); _or_null treats a
                    // missing type as "no match" and falls through to the diagnostic below.
                    if (auto* ptrTy = llvm::dyn_cast_or_null<llvm::PointerType>(srcBaseTy))
                    {
                        // Pointer move param: null the caller's storage.
                        builder->CreateStore(llvm::ConstantPointerNull::get(ptrTy), srcStorage);
                    }
                    else if (candidate.Parameters[i].TypeName == "string" && matched[i].IsOwningString)
                    {
                        // String move param: zero out _ptr in the caller's alloca so its destructor is a no-op.
                        // (Does not need srcBaseTy - the string layout is looked up by name.)
                        auto* strTy = llvm::StructType::getTypeByName(*context, "string");
                        if (strTy)
                        {
                            auto* ptrField = builder->CreateStructGEP(strTy, srcStorage, 0);
                            auto* i8ptrTy = builder->getInt8Ty()->getPointerTo();
                            builder->CreateStore(llvm::ConstantPointerNull::get(i8ptrTy), ptrField);
                        }
                    }
                    else if (auto* stTy = llvm::dyn_cast_or_null<llvm::StructType>(srcBaseTy))
                    {
                        // Struct move param: zero the caller's entire struct so its destructor is a no-op.
                        builder->CreateStore(llvm::ConstantAggregateZero::get(stTy), srcStorage);
                    }
                    else if (srcBaseTy == nullptr && candidate.Parameters[i].TypeName != "string")
                    {
                        // We have caller storage to clear but no type telling us how. This can only
                        // arise from a malformed (hand-built) argument; emit a clear diagnostic
                        // rather than leaving a stale value that a later destructor would double-free.
                        LogError(std::format(
                            "call to '{}': 'move' argument {} has no resolved type, so its source "
                            "storage cannot be cleared after the move", functionName, i));
                    }
                }
                // Compile-time: mark the caller's storage as moved so subsequent reads are rejected.
                // Covers pointer, owning-string, and struct move params - all cases where caller storage was zeroed.
                // Moving a FIELD (`node->left`) marks only that field, not the whole base variable.
                if (!matched[i].CallerName.empty() && srcStorage != nullptr &&
                    !isInterfaceBorrow && srcBaseTy != nullptr)
                {
                    bool isPtr = llvm::isa<llvm::PointerType>(srcBaseTy);
                    bool isOwningStr = candidate.Parameters[i].TypeName == "string" && matched[i].IsOwningString;
                    bool isStruct = llvm::isa<llvm::StructType>(srcBaseTy);
                    if (isPtr || isOwningStr || isStruct)
                    {
                        if (isFieldAccess)
                            MarkVariableFieldMoved(matched[i].CallerName, matched[i].FieldName);
                        else
                            MarkVariableMoved(matched[i].CallerName);
                    }
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
                if (nv.IsBonded && !nv.BondByAddress)
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

    // Per-field move tracking. Moving a struct sub-path (e.g. `node->left`) into a 'move'
    // parameter must mark ONLY that field as moved, not the whole base variable - otherwise
    // a sibling access (`node->right`) or the base itself is wrongly rejected as use-after-
    // move. This is what lets legitimate recursive owning-pointer-tree code compile.
    void MarkVariableFieldMoved(const std::string& name, const std::string& field)
    {
        if (name.empty() || field.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                { it->second.MovedFields.insert(field); return; }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                { it->second.MovedFields.insert(field); return; }
        }
    }

    // Reassigning a field (`node->left = ...`) makes it live again.
    void MarkVariableFieldUnmoved(const std::string& name, const std::string& field)
    {
        if (name.empty() || field.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                { it->second.MovedFields.erase(field); return; }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                { it->second.MovedFields.erase(field); return; }
        }
    }

    // Compile-time use-after-move subject for a (possibly field-access) variable. Returns the
    // name to report if the variable - or, for a field access, the specific field - was moved,
    // or an empty string if it is still live. A field-access NamedVariable is built from the
    // base variable's scope lookup, so it inherits both IsMoved and MovedFields: a fully-moved
    // base poisons all field reads, while a single moved field poisons only that field.
    std::string MovedUseSubject(const NamedVariable& nv) const
    {
        if (nv.IsMoved) return nv.CallerName;
        if (!nv.FieldName.empty() && nv.MovedFields.count(nv.FieldName))
            return nv.CallerName + "." + nv.FieldName;
        return "";
    }

    // Mark a pre-declared string local as owning its heap buffer. Used when a plain
    // assignment (`s = expr`) stores an owned heap string into an already-declared
    // string local: the local now owns the buffer and must free it on scope exit
    // (mirrors the IsOwningString propagation in the declaration-with-initializer path).
    void MarkVariableOwningString(const std::string& name)
    {
        if (name.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                { it->second.IsOwningString = true; return; }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                { it->second.IsOwningString = true; return; }
        }
    }

    // Snapshot of per-variable and per-field move state across active scopes (used to keep
    // move tracking sound across if/else branches).
    struct MovedStateSnapshot
    {
        std::map<std::string, bool> moved;
        std::map<std::string, std::set<std::string>> movedFields;
    };

    // Snapshots the IsMoved flag and per-field moved set for all variables in all active scopes.
    MovedStateSnapshot SaveMovedState() const
    {
        MovedStateSnapshot state;
        for (const auto& frame : stackNamedVariable)
        {
            for (const auto& [name, nv] : frame.functionArgument)
                { state.moved[name] = nv.IsMoved; state.movedFields[name] = nv.MovedFields; }
            for (const auto& [name, nv] : frame.namedVariable)
                { state.moved[name] = nv.IsMoved; state.movedFields[name] = nv.MovedFields; }
        }
        return state;
    }

    // Restores the move state from a snapshot (only for variables still in scope).
    void RestoreMovedState(const MovedStateSnapshot& state)
    {
        auto restoreOne = [&](const std::string& name, NamedVariable& nv) {
            if (auto it = state.moved.find(name); it != state.moved.end())
                nv.IsMoved = it->second;
            if (auto it = state.movedFields.find(name); it != state.movedFields.end())
                nv.MovedFields = it->second;
        };
        for (auto& frame : stackNamedVariable)
        {
            for (auto& [name, nv] : frame.functionArgument) restoreOne(name, nv);
            for (auto& [name, nv] : frame.namedVariable) restoreOne(name, nv);
        }
    }

    // Merges two post-branch states: a variable (or field) is moved if it was moved in either branch.
    void MergeMovedStates(const MovedStateSnapshot& thenState,
                          const MovedStateSnapshot& elseState)
    {
        auto mergeOne = [&](const std::string& name, NamedVariable& nv) {
            auto t = thenState.moved.find(name);
            auto e = elseState.moved.find(name);
            if (t != thenState.moved.end() && e != elseState.moved.end())
                nv.IsMoved = t->second || e->second;
            auto tf = thenState.movedFields.find(name);
            auto ef = elseState.movedFields.find(name);
            if (tf != thenState.movedFields.end() && ef != elseState.movedFields.end())
            {
                std::set<std::string> merged = tf->second;
                merged.insert(ef->second.begin(), ef->second.end());
                nv.MovedFields = std::move(merged);
            }
        };
        for (auto& frame : stackNamedVariable)
        {
            for (auto& [name, nv] : frame.functionArgument) mergeOne(name, nv);
            for (auto& [name, nv] : frame.namedVariable) mergeOne(name, nv);
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

    // `returnedLocalStorage`, when provided, is the alloca of the named local being
    // returned (the return expression's NamedVariable.Storage). It lets the struct-return
    // move detection below work even when the by-value return is materialized field-wise
    // (insertvalue) rather than as a single `load %Struct`, which dyn_cast<LoadInst> misses.
    void CreateReturnCall(llvm::Value* value, llvm::Value* returnedLocalStorage = nullptr, const std::string& interfaceReturnStructName = "")
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
                            if (nv.Storage == srcAlloca && nv.IsOwningString
                                && nv.TypeAndValue.TypeName == "string")
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

        // Returning a struct VALUE local whose full-destructor frees members (owned string
        // fields, value containers, ...): the by-value snapshot in `value` carries those member
        // pointers to the caller, so the local's destructor must be skipped here or the returned
        // value dangles (and double-frees when the caller destroys it). Suppress only for the
        // return-path destructor walk. Mirrors the owned-string / owned-pointer cases above.
        llvm::Value* prevStructSkip = returnedStructDtorSkipAlloca;
        if (value && !ownedStringReturnVar && !ownedPtrReturnVar)
        {
            llvm::Value* srcAlloca = returnedLocalStorage;
            if (srcAlloca == nullptr)
                if (auto* loadInst = llvm::dyn_cast<llvm::LoadInst>(value))
                    srcAlloca = loadInst->getPointerOperand();
            if (srcAlloca != nullptr)
            {
                bool found = false;
                for (auto& frame : stackNamedVariable)
                {
                    for (auto& [varName, nv] : frame.namedVariable)
                    {
                        if (nv.Storage == srcAlloca && !nv.TypeAndValue.Pointer
                            && nv.TypeAndValue.TypeName != "string"
                            && GetOrCreateFullDestructor(nv.TypeAndValue.TypeName) != nullptr)
                        {
                            returnedStructDtorSkipAlloca = srcAlloca;
                            found = true;
                            break;
                        }
                    }
                    if (found) break;
                }
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
        returnedStructDtorSkipAlloca = prevStructSkip;

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

        // Interface return: a returned concrete-implementer POINTER reaches here as a bare
        // pointer (the loaded owning local). Its scope-exit free was already suppressed by the
        // owned-pointer block above (the load made the variable look returned), so ownership
        // moves to the caller exactly as a plain `return ptr;` would. Box it into the interface
        // fat pointer { vtable, data } now, after the destructor walk. The value-operand and
        // already-boxed-interface cases are handled at the return site (MainListener).
        if (!interfaceReturnStructName.empty() && value)
        {
            auto* fatTy = GetFatPtrType();
            if (currentFunction->getReturnType() == fatTy && value->getType() != fatTy)
            {
                auto* vtable = GetOrCreateVTable(interfaceReturnStructName, currentFunctionReturnTypeName);
                value = BuildInterfaceFatValue(vtable, value);
            }
        }

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

    // True when v is a compile-time-constant non-zero integer - the guard of an
    // infinite loop such as `while (true)` / `while (1)`. Control can only leave
    // such a loop via `break`; it never falls through the condition.
    bool IsConstantTruthy(llvm::Value* v)
    {
        if (auto* ci = llvm::dyn_cast_or_null<llvm::ConstantInt>(v))
            return !ci->isZero();
        return false;
    }

    // True when the current insert block is unreachable: a non-entry block with
    // no predecessors (nothing branches to it). Used by fall-through analysis so
    // that the dead exit of an infinite loop (e.g. `while (true)` with no break)
    // is not mistaken for a live path that must end in a return.
    bool IsCurrentBlockUnreachable()
    {
        auto* bb = builder->GetInsertBlock();
        if (bb == nullptr || bb->getParent() == nullptr)
            return false;
        if (bb == &bb->getParent()->getEntryBlock())
            return false;
        return llvm::pred_empty(bb);
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

    // Stamp `!llvm.loop !{!"llvm.loop.vectorize.enable", i1 true}` on the latch
    // branch of the loop just emitted, and record it for post-optimization
    // enforcement. Called for `vectorize` loops; the back-edge must be the
    // terminator of the current insert block (true for while/for latches here).
    // Forcing vectorize.enable makes LLVM emit an explicit failure diagnostic
    // when the loop cannot be vectorized, which OptimizeModule turns into an error.
    void AttachVectorizeHintToCurrentLatch(int sourceLine)
    {
        auto* term = builder->GetInsertBlock()->getTerminator();
        auto* br = llvm::dyn_cast_or_null<llvm::BranchInst>(term);
        if (!br)
            return;  // body did not fall through to a back-edge (e.g. ended in return)

        auto* i1True = llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 1);
        auto* enableMD = llvm::MDNode::get(*context, {
            llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
            llvm::ConstantAsMetadata::get(i1True),
        });
        // Stamp the source line into the loop ID so post-optimization enforcement
        // can report the exact `vectorize` loop without relying on debug info or
        // diagnostic-handler correlation.
        auto* lineMD = llvm::MDNode::get(*context, {
            llvm::MDString::get(*context, "cflat.vectorize.line"),
            llvm::ConstantAsMetadata::get(
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), sourceLine)),
        });

        // Loop metadata is a self-referential node: operand 0 points at itself.
        llvm::SmallVector<llvm::Metadata*, 3> ops{ nullptr, enableMD, lineMD };
        auto* loopID = llvm::MDNode::getDistinct(*context, ops);
        loopID->replaceOperandWith(0, loopID);
        br->setMetadata(llvm::LLVMContext::MD_loop, loopID);
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
    const std::string& GetCurrentNamespace() const { return currentNamespace_; }
    void SetCurrentNamespace(const std::string& name) { currentNamespace_ = name; }
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
        return ResolveQualifiedName(name, false);
    }

    // forceRoot: skip the enclosing-namespace outward walk and resolve the name starting at
    // the root (file/global) scope. Used by the `global::` scope-escape qualifier so a
    // namespace member that shadows a global (e.g. Math.tan over the CRT tan) can still
    // reach the root symbol.
    std::string ResolveQualifiedName(const std::string& name, bool forceRoot) const
    {
        // Bare name referenced inside a namespace body: prefer an enclosing-namespace
        // sibling (e.g. inside "N", a bare "helper" resolves to "N.helper") before
        // falling back to a top-level/global symbol. Walk outward through parent
        // namespaces so a nested "Outer.Inner" also sees "Outer" members. A more-local
        // match wins; if no qualified sibling exists the bare name resolves below.
        if (!forceRoot && !currentNamespace_.empty() && name.find('.') == std::string::npos)
        {
            std::string prefix = currentNamespace_;
            while (true)
            {
                std::string candidate = prefix + "." + name;
                if (dataStructures.count(candidate) || interfaceTable.count(candidate)
                    || functionTable.count(candidate) || globalNamedVariable.count(candidate))
                    return candidate;
                auto parentDot = prefix.rfind('.');
                if (parentDot == std::string::npos)
                    break;
                prefix = prefix.substr(0, parentDot);
            }
        }

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
    // Overrides the file name shown in diagnostics (and baked into __FILE__) for the next
    // Analyze(). Used by the LSP so errors point at the real document, not the temp copy.
    void SetSourceDisplayName(const std::string& name) { sourceDisplayName_ = name; }
    void SetVerbose(bool v) { verbose = v; }
    bool IsVerbose() const { return verbose; }
    // Enable AddressSanitizer instrumentation + runtime linking. Best paired with -g.
    void SetAsan(bool v) { asan_ = v; }
    void SetRunMode(bool v) { runMode_ = v; }
    void SetRunArgs(std::vector<std::string> a) { runArgs_ = std::move(a); }
    int  GetJitExitCode() const { return jitExitCode_; }
    void SetBatchMode(bool v) { batchMode_ = v; }
    void SetNoCache(bool v) { noCache_ = v; }
    // When true, headers opted into the disk cache (via the `cache` import clause) record and
    // validate every transitively-included file's mtime/hash rather than just the top header.
    void SetCHeaderCacheDeep(bool v) { cHeaderCacheDeep_ = v; }

    // ---- Cross-thread sharing diagnostic (--xthread-scan N) ----------------
    // Opt-in, default OFF (level 0). A pre-pass (ScanCrossThreadEscapes, run before the
    // codegen walk) collects the set of struct TYPES whose instances escape to a spawned
    // thread into threadSharedTypes_; the two field-access reporting sites in MainListener
    // then print a one-line [xthread] report to stdout for any access to a field of such a
    // type that is neither atomic nor guarded by a lock. Higher levels widen which
    // spawn/escape patterns the pre-pass recognizes:
    //   1 - address-of a local struct passed to a thread spawn (&ctx; low noise)
    //   2 - level 1 + a heap struct-pointer local handed to a spawn (ptr handoff)
    //   3 - level 2 + any struct pointer passed to ANY call, and default-ordering
    //       atomics no longer suppress (most aggressive / most false positives)
    // This is a plain compiler stdout report, NOT a diagnostic-sink warning, so it
    // is never routed to the LSP and never affects the exit code.
    void SetXthreadScanLevel(int n) { xthreadScanLevel_ = n; }
    int  GetXthreadScanLevel() const { return xthreadScanLevel_; }
    bool IsXthreadEscapedType(const std::string& typeName) const
    {
        return threadSharedTypes_.find(typeName) != threadSharedTypes_.end();
    }
    void AddXthreadEscapedType(const std::string& typeName)
    {
        if (!typeName.empty())
            threadSharedTypes_.insert(typeName);
    }
    // Pre-pass: walk the main translation unit's parse tree (before the codegen walk) and
    // populate threadSharedTypes_ with struct types that escape to a spawned thread.
    // No-op when xthreadScanLevel_ == 0. Defined in LLVMBackend.cpp.
    void ScanCrossThreadEscapes(CFlatParser::CompilationUnitContext* cu);
    // Emit one [xthread] line for a field access on an escaped struct type, unless the
    // field is thread-safe by construction (atomic wrapper or lock-guarded). Deduped so
    // each distinct finding prints once. Plain stdout - NOT a diagnostic-sink warning.
    void ReportXthreadFieldAccess(const std::string& varName, const std::string& fieldName,
                                  const std::string& structType, const TypeAndValue& field)
    {
        if (xthreadScanLevel_ <= 0)
            return;
        if (!IsXthreadEscapedType(structType))
            return;
        if (FieldSatisfiesThreadDiscipline(field))
            return;                         // atomic wrapper or GuardedBy lock -> safe
        std::string line = std::format(
            "[xthread] field '{}.{}' ({}) shared across spawn, not atomic/guarded",
            varName.empty() ? "?" : varName, fieldName, structType);
        if (xthreadReported_.insert(line).second)
            std::cout << line << "\n";
    }

    // severity: 1 = Error, 2 = Warning (matches the LSP DiagnosticSeverity codes).
    using DiagnosticSink = std::function<void(const std::string& file, size_t line, size_t col, const std::string& msg, int severity)>;
    void SetDiagnosticSink(DiagnosticSink sink) { diagnosticSink_ = std::move(sink); }

    void SetSymbolSink(LspSymbolIndex* sink) { symbolSink_ = sink; }
    LspSymbolIndex* GetSymbolSink() const { return symbolSink_; }

    // LSP-only sink for "faded" hint regions (unused/unreachable code). Carries a full
    // range (start..end, 1-based line / 0-based col) so the client can gray a whole span
    // rather than a single caret. Set only in LSP mode; null during real compiles.
    using HintRegionSink = std::function<void(int startLine, int startCol,
                                              int endLine, int endCol,
                                              const std::string& msg)>;
    void SetHintRegionSink(HintRegionSink sink) { hintRegionSink_ = std::move(sink); }
    void ReportHintRegion(int startLine, int startCol, int endLine, int endCol, const std::string& msg)
    {
        if (hintRegionSink_)
            hintRegionSink_(startLine, startCol, endLine, endCol, msg);
    }
    bool HasHintRegionSink() const { return (bool)hintRegionSink_; }

    void ReportParseErrors(const std::vector<ParseDiagnostic>& diagnostics,
                           const std::vector<std::string>& sourceLines);

    // inputOverride, when non-empty, replaces positional(0) as the file to compile.
    // Used by batch / --check mode to compile each positional file independently.
    bool Compile(const ArgParser& args, const std::string& inputOverride = {});

    // --grammar: parse a single source file in isolation to validate its syntax
    // (no imports, no forward-ref scan, no codegen). Syntax errors are reported
    // with the same humanized hints as a normal compile. When verbose is set, the
    // full parse-tree rule stack is also printed. Returns true if the file parsed
    // without syntax errors.
    bool CheckGrammar(const std::string& filename);

    bool CompileImportedFile(const std::string& importingFilePath, const std::string& importFilename, const std::string& namespaceName = {}, const std::string& programAlias = {}, const std::vector<std::string>& explicitLibs = {}, const std::vector<std::string>& extraDefines = {}, bool cacheHeader = false);

    // Resolve an import filename against the same search order CompileImportedFile uses
    // (importing dir, LSP source dir, -i dir, --c-include roots, runtime core, Windows SDK).
    // Returns true with the canonical path in `outCanonical`; on failure logs the not-found
    // error (unless `quiet`) and returns false.
    bool ResolveImportPath(const std::string& importingFilePath, const std::string& importFilename,
                           std::string& outCanonical, bool quiet = false);

    // Handle a grouped import `import { "a", "b" } lib {...} define ...;`. Non-header entries
    // (.cb/.c) route individually in listed order; every header entry (.h/.hpp/.hh) is bound as
    // ONE translation unit so an earlier header satisfies a later one's prerequisites. Group
    // `lib`/`define` clauses apply to the whole group.
    bool CompileImportGroup(const std::string& importingFilePath,
                            const std::vector<std::string>& entries,
                            const std::vector<std::string>& groupLibs,
                            const std::vector<std::string>& groupDefines,
                            bool cacheGroup);

    // Resolve an inline `lib "..."` clause to a link argument for cLinkLibs_. A library that
    // ships beside the importing .cb (the Conan/vcpkg layout) is used by its full path; a
    // bare system lib name like "user32.lib" that is not found there is passed through
    // unqualified, so lld-link resolves it against the system lib search paths - the Windows
    // SDK `um` dir that cflat already discovered to resolve <windows.h> is on that path.
    static std::string ResolveCLinkLib(const std::string& lib, const std::string& importingFilePath)
    {
        std::filesystem::path lp(lib);
        if (lp.is_absolute())
            return lp.string();
        std::filesystem::path beside = (std::filesystem::path(importingFilePath).parent_path() / lp).lexically_normal();
        std::error_code ec;
        if (std::filesystem::exists(beside, ec))
            return beside.string();
        // A relative path that names a subdirectory is a real (possibly mistyped) location -
        // keep it normalized so lld-link's "not found" points at the resolved path. A bare
        // filename is a system lib - pass it through for the linker's lib-path search.
        if (lp.has_parent_path())
            return beside.string();
        return lib;
    }

    // Vcpkg integration setters - wired from CLI flags in main.cpp.
    void SetVcpkgExe(const std::string& path)        { vcpkg_.SetExeOverride(path); }
    void SetVcpkgManifest(const std::string& path)   { vcpkg_.SetManifestOverride(path); }
    void SetVcpkgTriplet(const std::string& triplet) { vcpkg_.SetTripletOverride(triplet); }

    // Manifest-walk origin for a `import package-vcpkg` on the ROOT file under compilation.
    // In LSP mode the analyzed file is a temp copy in %TEMP% (see LspServer's
    // RunAnalysisOnSlot), so walking up from it would never find the project's vcpkg.json;
    // sourceFileDir_ carries the file's real on-disk directory and is used as the origin.
    // In CLI mode sourceFileDir_ is empty and the real analyzed path is returned unchanged.
    // Only for the root file - transitive .cb imports pass their own real path and must keep
    // walking from there (nearest vcpkg.json wins). The returned path is synthetic (only its
    // parent directory is consulted by the resolver), so the basename is irrelevant.
    std::string RootVcpkgImportPath(const std::string& analyzedPath) const
    {
        if (sourceFileDir_.empty())
            return analyzedPath;
        return (std::filesystem::path(sourceFileDir_) /
                std::filesystem::path(analyzedPath).filename()).string();
    }

    // ---- Vcpkg header disk cache ----
    // Cache lives in vcpkg_installed/.cflat-cache/<key>.json, co-located with the
    // installed packages so it invalidates naturally when vcpkg rewrites them.
    // Key: FNV-1a of the canonical header path + --c-define values.
    // Concurrent-write safety: atomic temp-file + rename; treat any read failure
    // as a cache miss (benign - just re-runs clang).

    static uint64_t VcpkgDiskCacheKey(const std::string& fileForLsp,
                                       const std::vector<std::string>& defines)
    {
        uint64_t h = 14695981039346656037ULL;
        for (unsigned char c : fileForLsp) { h ^= c; h *= 1099511628211ULL; }
        for (const auto& d : defines) for (unsigned char c : d) { h ^= c; h *= 1099511628211ULL; }
        return h;
    }

    // Disk-cache directory for plain `import "x.h" cache;` header binds. Sits beside the
    // core-bitcode and linker-path caches under %USERPROFILE%\.cflat. Empty if unavailable.
    static std::string GetCHeaderCacheDir()
    {
        std::string base = GetCflatCacheDir();
        if (base.empty()) return {};
        return base + "\\cheaders";
    }

    // FNV-1a key for the plain-header disk cache. Folds the same inputs as the in-memory
    // cache key in CompileCHeader (canonical path, every --c-include dir, every --c-define,
    // and every per-import inline define) so a header exposed differently under different
    // roots/defines never collides on a stale entry. The version-stamped SDK include dirs
    // here are what catch an SDK upgrade in shallow mode.
    static uint64_t CHeaderDiskCacheKey(const std::string& fileForLsp,
                                        const std::vector<std::string>& includeDirs,
                                        const std::vector<std::string>& defines,
                                        const std::vector<std::string>& extraDefines)
    {
        return CHeaderDiskCacheKey(std::vector<std::string>{ fileForLsp },
                                   includeDirs, defines, extraDefines);
    }

    // Group form: folds the full ordered header list so a header bound standalone never shares
    // a disk-cache slot with the same header bound after a prerequisite (different decl set).
    static uint64_t CHeaderDiskCacheKey(const std::vector<std::string>& headerPaths,
                                        const std::vector<std::string>& includeDirs,
                                        const std::vector<std::string>& defines,
                                        const std::vector<std::string>& extraDefines)
    {
        uint64_t h = 14695981039346656037ULL;
        auto fold = [&h](const std::string& s) {
            for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
        };
        for (const auto& hp : headerPaths)   { fold("|H"); fold(hp); }
        for (const auto& inc : includeDirs)  { fold("|I"); fold(inc); }
        for (const auto& def : defines)      { fold("|D"); fold(def); }
        for (const auto& def : extraDefines) { fold("|d"); fold(def); }
        return h;
    }

    static nlohmann::json TvToJson(const TypeAndValue& tv)
    {
        nlohmann::json j;
        j["t"] = tv.TypeName;
        if (tv.Pointer)        j["p"]   = true;
        if (tv.ElemPointer)    j["ep"]  = true;
        if (tv.IsMove)         j["mv"]  = true;
        if (tv.IsStdcall)      j["sc"]  = true;
        if (tv.IsCdecl)        j["cd"]  = true;
        if (tv.ConstArraySize) j["arr"] = tv.ConstArraySize;
        if (!tv.ConstInnerDimensions.empty()) j["idims"] = tv.ConstInnerDimensions;
        if (tv.IsSimd) { j["sd"] = true; j["sdl"] = tv.SimdLanes; }
        if (tv.IsArrayView) j["av"] = true;
        if (tv.IsFunctionPointer)
        {
            j["fp"]  = true;
            j["fpr"] = tv.FuncPtrReturnTypeName;
            if (tv.FuncPtrReturnPointer) j["fprp"] = true;
            nlohmann::json fps = nlohmann::json::array();
            for (const auto& p : tv.FuncPtrParams)
            {
                nlohmann::json pj;
                pj["t"] = p.TypeName;
                if (p.Pointer) pj["p"]  = true;
                if (p.IsMove)  pj["mv"] = true;
                fps.push_back(pj);
            }
            j["fps"] = fps;
        }
        return j;
    }
    static TypeAndValue TvFromJson(const nlohmann::json& j)
    {
        TypeAndValue tv;
        tv.TypeName       = j.value("t",   std::string{});
        tv.Pointer        = j.value("p",   false);
        tv.ElemPointer    = j.value("ep",  false);
        tv.IsMove         = j.value("mv",  false);
        tv.IsStdcall      = j.value("sc",  false);
        tv.IsCdecl        = j.value("cd",  false);
        tv.ConstArraySize = j.value("arr", uint64_t{0});
        if (j.contains("idims")) tv.ConstInnerDimensions = j["idims"].get<std::vector<uint64_t>>();
        tv.IsSimd   = j.value("sd",  false);
        tv.SimdLanes = j.value("sdl", uint64_t{0});
        tv.IsArrayView = j.value("av", false);
        tv.IsFunctionPointer = j.value("fp", false);
        if (tv.IsFunctionPointer)
        {
            tv.FuncPtrReturnTypeName = j.value("fpr",  std::string{});
            tv.FuncPtrReturnPointer  = j.value("fprp", false);
            if (j.contains("fps"))
                for (const auto& pj : j["fps"])
                {
                    TypeAndValue::FuncPtrParam p;
                    p.TypeName = pj.value("t",  std::string{});
                    p.Pointer  = pj.value("p",  false);
                    p.IsMove   = pj.value("mv", false);
                    tv.FuncPtrParams.push_back(p);
                }
        }
        return tv;
    }

    static nlohmann::json SigToJson(const CSigEntry& e)
    {
        nlohmann::json ps = nlohmann::json::array();
        for (const auto& p : e.params) ps.push_back(TvToJson(p));
        return {{"n", e.name}, {"r", TvToJson(e.ret)}, {"ps", ps},
                {"va", e.variadic}, {"ln", e.line}, {"co", e.col}};
    }
    static CSigEntry SigFromJson(const nlohmann::json& j)
    {
        CSigEntry e;
        e.name     = j.value("n",  std::string{});
        e.ret      = TvFromJson(j.at("r"));
        e.variadic = j.value("va", false);
        e.line     = j.value("ln", 1);
        e.col      = j.value("co", 0);
        if (j.contains("ps")) for (const auto& p : j["ps"]) e.params.push_back(TvFromJson(p));
        return e;
    }

    static nlohmann::json EnumToJson(const CEnumEntry& e)
    {
        return {{"n", e.name}, {"v", e.value}, {"ln", e.line}, {"co", e.col}};
    }
    static CEnumEntry EnumFromJson(const nlohmann::json& j)
    {
        return {j.value("n", std::string{}), j.value("v", 0LL), j.value("ln", 1), j.value("co", 0)};
    }

    static nlohmann::json GlobalToJson(const CGlobalEntry& g)
    {
        return {{"n", g.name}, {"t", TvToJson(g.type)}, {"ln", g.line}, {"co", g.col}};
    }
    static CGlobalEntry GlobalFromJson(const nlohmann::json& j)
    {
        CGlobalEntry g;
        g.name = j.value("n", std::string{});
        g.type = TvFromJson(j.at("t"));
        g.line = j.value("ln", 1);
        g.col  = j.value("co", 0);
        return g;
    }

    static nlohmann::json FieldToJson(const CRecordFieldEntry& f)
    {
        nlohmann::json j = {{"n", f.name}, {"ct", f.ctype}};
        if (f.isBitfield) { j["bf"] = true; j["bw"] = f.bitWidth; }
        return j;
    }
    static CRecordFieldEntry FieldFromJson(const nlohmann::json& j)
    {
        CRecordFieldEntry f;
        f.name      = j.value("n",  std::string{});
        f.ctype     = j.value("ct", std::string{});
        f.isBitfield = j.value("bf", false);
        f.bitWidth   = j.value("bw", 0u);
        return f;
    }

    static nlohmann::json RecordToJson(const CRecordEntry& r)
    {
        nlohmann::json fs = nlohmann::json::array();
        for (const auto& f : r.fields) fs.push_back(FieldToJson(f));
        nlohmann::json j = {{"n", r.name}, {"fs", fs}, {"ln", r.line}, {"co", r.col}};
        if (r.isUnion) j["u"] = true;
        return j;
    }
    static CRecordEntry RecordFromJson(const nlohmann::json& j)
    {
        CRecordEntry r;
        r.name    = j.value("n", std::string{});
        r.isUnion = j.value("u", false);
        r.line    = j.value("ln", 1);
        r.col     = j.value("co", 0);
        if (j.contains("fs")) for (const auto& f : j["fs"]) r.fields.push_back(FieldFromJson(f));
        return r;
    }

    static nlohmann::json MacroToJson(const CMacroEntry& m)
    {
        nlohmann::json j = {{"n", m.name}, {"v", m.value}, {"f", m.file},
                            {"ln", m.line}, {"co", m.col}};
        if (m.isPointer) j["isp"]  = true;
        // Store the float as its raw IEEE-754 bit pattern, not as a JSON number:
        // nlohmann serializes inf/NaN as JSON `null` (math.h's INFINITY/NAN/HUGE_VAL),
        // which then throws type_error.302 on reload. Bits round-trip every value exactly.
        if (m.isFloat)
        {
            uint64_t fvbits = 0;
            std::memcpy(&fvbits, &m.floatValue, sizeof(double));
            j["isf"] = true; j["fvb"] = fvbits;
        }
        if (m.isString) { j["iss"]  = true; j["sv"]   = m.stringValue; }
        if (m.isFuncPtr){ j["isfp"] = true; j["fptv"] = TvToJson(m.funcPtrTV); }
        if (!m.intTypeName.empty()) j["ity"] = m.intTypeName;
        return j;
    }
    static CMacroEntry MacroFromJson(const nlohmann::json& j)
    {
        CMacroEntry m;
        m.name      = j.value("n",   std::string{});
        m.value     = j.value("v",   0LL);
        m.file      = j.value("f",   std::string{});
        m.line      = j.value("ln",  1);
        m.col       = j.value("co",  0);
        m.isPointer = j.value("isp", false);
        m.isFloat   = j.value("isf", false);
        if (m.isFloat)
        {
            uint64_t fvbits = j.value("fvb", uint64_t{0});
            std::memcpy(&m.floatValue, &fvbits, sizeof(double));
        }
        m.isString  = j.value("iss",  false);
        if (m.isString) m.stringValue = j.value("sv", std::string{});
        m.isFuncPtr = j.value("isfp", false);
        if (m.isFuncPtr && j.contains("fptv")) m.funcPtrTV = TvFromJson(j["fptv"]);
        m.intTypeName = j.value("ity", std::string{});
        return m;
    }

    static nlohmann::json FuncMacroToJson(const CFunctionMacroEntry& m)
    {
        return {{"n", m.name}, {"ps", m.params}, {"b", m.body},
                {"f", m.file}, {"ln", m.line},   {"co", m.col}};
    }
    static CFunctionMacroEntry FuncMacroFromJson(const nlohmann::json& j)
    {
        CFunctionMacroEntry m;
        m.name = j.value("n",  std::string{});
        m.body = j.value("b",  std::string{});
        m.file = j.value("f",  std::string{});
        m.line = j.value("ln", 1);
        m.col  = j.value("co", 0);
        if (j.contains("ps")) m.params = j["ps"].get<std::vector<std::string>>();
        return m;
    }

    // Validate one transitively-included dependency: accept on mtime match (fast) or, on
    // mtime drift, content-hash match. A vanished file or unreadable hash is a miss.
    static bool CHeaderDepFresh(const CHeaderDep& dep)
    {
        std::error_code ec;
        auto mt = std::filesystem::last_write_time(dep.path, ec);
        if (ec) return false;
        if ((int64_t)mt.time_since_epoch().count() == dep.mtime) return true;
        uint64_t h = 0;
        return HashFileFnv1a(dep.path, h) && h == dep.hash;
    }

    static bool TryLoadCHeaderDiskCache(
        const std::filesystem::path& cacheDir,
        uint64_t diskKey,
        std::filesystem::file_time_type mtime,
        uint64_t contentHash,
        CFileSigCacheEntry& out)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        auto cachePath = cacheDir / std::format("{:016x}.json", diskKey);
        if (!fs::exists(cachePath, ec)) return false;

        std::ifstream f(cachePath);
        if (!f.is_open()) return false;
        nlohmann::json j;
        try { f >> j; } catch (...) { return false; }

        int version = j.value("version", 0);
        // v1/v2 stored float macro values as JSON numbers, which encode inf/NaN as `null`
        // and throw on reload; v3 used the raw-bits float encoding but cached only in-scope
        // records (so an entry written before the by-value dependency closure was added would
        // still be missing the dependency structs). v4 caches the in-scope + dependency set.
        // v5 widened the in-scope filter to the Windows SDK shared/ sibling dir (ERROR_*,
        // MAX_PATH, ...), so a v4 entry would silently lack those constants.
        // v6 synthesizes a tag/record for a named field of an unnamed record type (the
        // _LARGE_INTEGER `u` shape), so a v5 entry would still carry the unmappable
        // "struct X::(unnamed at ...)" ctype that abandons the whole record.
        // v7 extends that synthesis to an *array* of an unnamed record element
        // (RETRIEVAL_POINTERS_BUFFER.Extents[1]); a v6 entry would still have dropped that
        // field and left the enclosing record an incomplete shell.
        if (version != 7) return false;

        // Accept on mtime match (fast) or content hash match (authoritative on mtime drift).
        auto storedMtime = j.value("mtime", int64_t{-1});
        auto storedHash  = j.value("hash",  uint64_t{0});
        bool mtimeOk = (storedMtime == (int64_t)mtime.time_since_epoch().count());
        bool hashOk  = (storedHash  == contentHash);
        if (!mtimeOk && !hashOk) return false;

        // Any malformed/incompatible field must degrade to a cache miss (reparse), never abort
        // the compiler: the nlohmann accessors throw on a type mismatch, so guard the whole build.
        CFileSigCacheEntry entry;
        entry.mtime = mtime;
        entry.hash  = contentHash;
        try
        {
            if (j.contains("sigs"))       for (const auto& s : j["sigs"])       entry.sigs.push_back(SigFromJson(s));
            if (j.contains("enums"))      for (const auto& e : j["enums"])      entry.enums.push_back(EnumFromJson(e));
            if (j.contains("records"))    for (const auto& r : j["records"])    entry.records.push_back(RecordFromJson(r));
            if (j.contains("macros"))     for (const auto& m : j["macros"])     entry.macros.push_back(MacroFromJson(m));
            if (j.contains("funcMacros")) for (const auto& m : j["funcMacros"]) entry.funcMacros.push_back(FuncMacroFromJson(m));
            if (j.contains("globals"))    for (const auto& g : j["globals"])    entry.globals.push_back(GlobalFromJson(g));
            if (j.contains("recordAliases"))
                for (const auto& a : j["recordAliases"])
                    entry.recordAliases.emplace_back(a.value("a", std::string{}), a.value("t", std::string{}));

            // A deep (transitive) entry is only fresh if every recorded include is unchanged.
            // Shallow entries (no "deps") skip this and rely on the top-header check above.
            if (j.contains("deps"))
            {
                for (const auto& dj : j["deps"])
                {
                    CHeaderDep dep;
                    dep.path  = dj.value("f", std::string{});
                    dep.mtime = dj.value("mt", int64_t{0});
                    dep.hash  = dj.value("h",  uint64_t{0});
                    if (!CHeaderDepFresh(dep)) return false;
                    entry.deps.push_back(std::move(dep));
                }
            }
        }
        catch (...) { return false; }
        out = std::move(entry);
        return true;
    }

    static void WriteCHeaderDiskCache(
        const std::filesystem::path& cacheDir,
        uint64_t diskKey,
        std::filesystem::file_time_type mtime,
        uint64_t contentHash,
        const CFileSigCacheEntry& entry)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(cacheDir, ec);
        if (ec) return;

        nlohmann::json j;
        j["version"] = 7;
        j["mtime"]   = (int64_t)mtime.time_since_epoch().count();
        j["hash"]    = contentHash;

        nlohmann::json sigs = nlohmann::json::array();
        for (const auto& s : entry.sigs) sigs.push_back(SigToJson(s));
        j["sigs"] = sigs;
        nlohmann::json enums = nlohmann::json::array();
        for (const auto& e : entry.enums) enums.push_back(EnumToJson(e));
        j["enums"] = enums;
        nlohmann::json records = nlohmann::json::array();
        for (const auto& r : entry.records) records.push_back(RecordToJson(r));
        j["records"] = records;
        nlohmann::json macros = nlohmann::json::array();
        for (const auto& m : entry.macros) macros.push_back(MacroToJson(m));
        j["macros"] = macros;
        nlohmann::json funcMacros = nlohmann::json::array();
        for (const auto& m : entry.funcMacros) funcMacros.push_back(FuncMacroToJson(m));
        j["funcMacros"] = funcMacros;
        nlohmann::json globals = nlohmann::json::array();
        for (const auto& g : entry.globals) globals.push_back(GlobalToJson(g));
        j["globals"] = globals;
        nlohmann::json recordAliases = nlohmann::json::array();
        for (const auto& a : entry.recordAliases)
            recordAliases.push_back({{"a", a.first}, {"t", a.second}});
        j["recordAliases"] = recordAliases;

        // Deep mode only: the transitive include set for strict (transitive) validation.
        if (!entry.deps.empty())
        {
            nlohmann::json deps = nlohmann::json::array();
            for (const auto& d : entry.deps)
                deps.push_back({{"f", d.path}, {"mt", d.mtime}, {"h", d.hash}});
            j["deps"] = deps;
        }

        // Atomic write: PID-stamped temp file renamed over the target.
        auto tmpPath  = cacheDir / std::format("{:016x}.{}.tmp", diskKey, _getpid());
        auto destPath = cacheDir / std::format("{:016x}.json", diskKey);
        {
            std::ofstream f(tmpPath);
            if (!f.is_open()) return;
            f << j;
        }
        fs::rename(tmpPath, destPath, ec);
        if (ec) fs::remove(tmpPath, ec);
    }

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
            // In LSP mode `vcpkg install` is skipped (RunVcpkgInstall is a no-op), so if the
            // package has not been built yet the whole vcpkg_installed/<triplet>/include tree
            // is absent. Flagging the import line then would put a spurious error on a file
            // that compiles cleanly once the user runs a build. Degrade to a silent skip: the
            // C symbols just stay unindexed until the package is installed. The CLI build
            // (which actually ran the install) still reports the precise error. A header
            // missing *under an existing* include dir is a real mistake (typo / wrong port)
            // and is surfaced even in LSP mode.
            const bool lspMode = symbolSink_ != nullptr;
            std::error_code dirEc;
            const bool includeDirPresent =
                !res.includeDir.empty() && std::filesystem::exists(res.includeDir, dirEc);
            if (lspMode && !includeDirPresent)
            {
                if (verbose)
                    std::cout << "[verbose] vcpkg: package not installed (no '" << res.includeDir
                              << "'); skipping header '" << header << "' for LSP analysis\n";
                return true;
            }
            // CLI: a missing header is a hard error. If `vcpkg install` was suppressed
            // (--vcpkg-no-install) and the include tree is absent, say so precisely instead
            // of the generic "install is incomplete" hint.
            if (vcpkg_.InstallSuppressed() && !includeDirPresent)
            {
                LogError(std::format(
                    "import package-vcpkg: port for header '{}' is not installed (no '{}'), "
                    "and 'vcpkg install' is disabled (--vcpkg-no-install).\n"
                    "  Run 'vcpkg install' yourself, or drop --vcpkg-no-install to let cflat install it.",
                    header, res.includeDir));
                return false;
            }
            LogError(std::format(
                "import package-vcpkg: header '{}' not found under '{}'.\n"
                "  The port may not own this header, or the install is incomplete.",
                header, res.includeDir));
            return false;
        }
        // Disk cache for vcpkg headers, stored in vcpkg_installed/.cflat-cache/.
        // On hit we preload the in-memory cache so CompileCHeader skips clang entirely.
        // On miss we write after CompileCHeader so the next build is a hit.
        std::filesystem::path vcpkgCacheDir =
            std::filesystem::path(res.includeDir).parent_path().parent_path() / ".cflat-cache";

        // Derive the same in-memory key CompileCHeader builds internally.
        llvm::SmallString<256> realPathBuf;
        std::string fileForLsp = headerCanon.string();
        if (!llvm::sys::fs::real_path(fileForLsp, realPathBuf))
            fileForLsp = realPathBuf.str().str();
        // Mirror the in-memory cache key CompileCHeaderGroup builds (single-header form).
        std::string inMemKey = "|H" + fileForLsp;
        for (const auto& inc : cIncludeDirs_) inMemKey += "|I" + inc;
        for (const auto& def : cDefines_)     inMemKey += "|D" + def;

        uint64_t diskKey     = VcpkgDiskCacheKey(fileForLsp, cDefines_);
        uint64_t contentHash = 0;
        bool haveHash        = HashFileContents(fileForLsp, contentHash);
        std::error_code mtEc;
        auto headerMtime     = std::filesystem::last_write_time(fileForLsp, mtEc);

        bool diskHit = false;
        if (haveHash && !mtEc)
        {
            CFileSigCacheEntry diskEntry;
            if (TryLoadCHeaderDiskCache(vcpkgCacheDir, diskKey, headerMtime, contentHash, diskEntry))
            {
                std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
                cFileSigCache_[inMemKey] = std::move(diskEntry);
                diskHit = true;
                if (verbose)
                    std::cout << "[verbose] vcpkg header disk cache hit: " << fileForLsp << "\n";
            }
        }

        bool ok = CompileCHeader(headerCanon.string(), {});

        // --run is read-only: skip persisting the vcpkg header cache to disk under run mode
        // (the in-memory cache entry from CompileCHeader still serves this compile).
        if (ok && !diskHit && !runMode_ && haveHash && !mtEc)
        {
            CFileSigCacheEntry entryToWrite;
            bool haveEntry = false;
            {
                std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
                auto it = cFileSigCache_.find(inMemKey);
                if (it != cFileSigCache_.end()) { entryToWrite = it->second; haveEntry = true; }
            }
            if (haveEntry)
                WriteCHeaderDiskCache(vcpkgCacheDir, diskKey, headerMtime, contentHash, entryToWrite);
        }

        if (ok) ProcessPendingMacroSources();
        return ok;
    }

    bool Analyze(const std::string& filePath, const std::string& importDir, const std::string& runtimeDirPath);
    void ResetForReanalysis();

    // Populate %USERPROFILE%\.cflat\ with cached linker paths for x64 and x86.
    // Prints discovered paths to stdout. Returns false if the cache dir cannot be created.
    static bool RunInit(const std::string& runtimeDir, bool verbose);

    // List the target CPUs supported on the currently supported platforms
    // (Windows x86/x64, which both use LLVM's X86 backend and share one CPU table).
    // Prints the sorted CPU names to stdout. Returns false on failure.
    static bool PrintSupportedCpus();

    // Print the LLVM name of the host CPU (what --cpu native resolves to), e.g.
    // "znver4". Returns false if the host CPU cannot be determined.
    static bool PrintHostCpu();

    // Resolve a --cpu/--tune value for the given triple: map "native" to the host CPU
    // and validate the result against the target's CPU table. 'label' names the flag in
    // the error message. Returns false with a diagnostic if the name is unknown.
    static bool ResolveCpuName(const std::string& requested, const std::string& triple,
                               const char* label, bool verbose, std::string& resolved);

    // Core bitcode cache: returns %USERPROFILE%\.cflat\runtime\<hash> or "" on failure.
    // The hash is derived from the modification times of all .cb files in runtimeDir/core.
    static std::string GetRuntimeBitcodeDir(const std::string& runtimeDir);

    // Initialize the module for the given platform and run RuntimeImport.
    // Used by RunInit to pre-compile core libraries for the bitcode cache.
    bool CompileCoreOnly(const std::string& platform);

    // Serialize the compiled core module and symbol tables to cacheDir/core_<platform>.{bc,meta.json}.
    bool SaveCoreBitcode(const std::string& cacheDir, const std::string& platform) const;

    // Load the core bitcode cache from cacheDir/core_<platform>.{bc,meta.json}.
    // Populates module, symbol tables, and generic templates. Returns false if absent or stale.
    bool LoadCoreBitcodeIfFresh(const std::string& cacheDir, const std::string& platform);
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
