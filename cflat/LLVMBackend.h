#pragma once
// ============================================================
// LLVMBackend.h - LLVM IR backend, type system, symbol tables
// ============================================================
// SECTION      LINE       DESCRIPTION               FUNCTION
// ───────────────────────────────────────────────────────────
// §1           235-845    Public enums/structs (Operation, TypeAndValue, ...)
// §2           847-1161   Private member data
// §3           1162-4806  Private methods
// §4           4807+      Public methods (>230 methods)
//   §4.1       4848       Debug info                InitDebugInfo, FinalizeDebugInfo
//   §4.2       4894       Block control             AbortFunctionBlocks, SaveBuilderState
//   §4.3       4932       Interface system          CreateInterfaceDefinition, IsInterfaceType, GetFatPtrType
//   §4.4       5915       Variable management       CreateGlobalVariable, AllocaAtEntry, CreateLocalVariable
//   §4.5       6128       IR emission               CreateInsertValue, CreateStructGEP, CreateAssignment
//   §4.6       7308       Control flow              CreateBasicBlock, SwitchToBlock, CreateJump
//   §4.7       7690       Function system           CreateFunctionDeclaration, CreateFunctionDefinition
//   §4.8       7991       Lookup / name resolution  IsKnownTypeName, GetType
// ============================================================

#include <algorithm>
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
#if defined(_WIN32)
#include <io.h>
#endif
#include "platform/PlatformCompat.h"

#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include <llvm/IR/CFG.h>          // llvm::pred_empty (MarkUnreachableIfNoPredecessors)
#include <llvm/Analysis/TargetLibraryInfo.h>  // stdio-safe TLI for ELF codegen (no chk->plain fold)
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/MDBuilder.h>    // llvm::MDBuilder (alias-scope/domain metadata for T[] noalias)
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicsX86.h>   // llvm::Intrinsic::x86_rdtscp (CreateRdtscp)
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Dominators.h>   // llvm::DominatorTree (dominance-aware owned-temp flush)
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/JSON.h>
#include <simdjson.h>   // fast read path for the C-header disk cache (parse only; writes stay nlohmann)
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/TimeProfiler.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>          // --run JIT execution (JitRun)
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h> // DynamicLibrarySearchGenerator
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h> // JITLink layer (--run threading/SEH)
#include <llvm/ExecutionEngine/JITLink/JITLink.h>        // LinkGraph (SEH .pdata registration)
#include <llvm/ExecutionEngine/JITLink/x86_64.h>          // jump-stub helpers for the SEH handler thunk
#include <llvm/ExecutionEngine/Orc/Shared/MemoryFlags.h>  // orc::MemProt for synthesized stub sections
#include <unordered_map>
// _aligned_malloc/_aligned_free (emutls shim) come from platform/PlatformCompat.h
// (included above): <malloc.h> on Windows, a posix_memalign shim on POSIX. macOS
// has no <malloc.h>, so do not include it directly here.
#include <llvm/MC/TargetRegistry.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/TargetParser/Host.h>
#pragma warning(pop)
#include "platform/GeneratedParser.h" // antlr runtime + generated parser/lexer/listener + kTokenEOF
#include <fstream>
#include "ArgParser.h"
#include "CClangExtract.h"
#include "WinmdExtract.h"
#include "WinmdEmit.h"
#include "WinmdSignature.h"
#include <array>
#include "CompilerManager.h"

#include "LspSymbolIndex.h"
#include "CFlatErrorListener.h"
#include "VcpkgResolver.h"
#include "NugetResolver.h"

struct ExpectedErrorReceived {};

// Resolved lld-link and MSVC/Windows SDK lib paths.
// Persisted to %USERPROFILE%\.cflat\linker_paths_<arch>.json; loaded before falling back to live discovery.
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

// Per-backend generic template state. Lives on LLVMBackend so concurrent
// LSP-pool backends don't stomp each other.
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
    // Mangled tuple name -> its element type args. Recorded wherever a tuple shell is named
    // (including the forward-ref pre-scan) so a destructure can lazily instantiate the fields
    // when the tuple is reached only via a return type before its producer was code-generated.
    std::unordered_map<std::string, std::vector<std::string>>                   tupleTypeArgs;

    // Templates loaded from the core cache but not yet parsed: name -> source text. The
    // genericX Templates maps hold a null context placeholder until MaterializeGeneric* parses
    // the source on first instantiation. Lives here so Clear() drops it with the maps it backs.
    std::unordered_map<std::string, std::string>                                lazyTemplateSource;

    void Clear() { *this = GenericTemplateState{}; }
};

struct CompilerAbortException {
    std::string message;
    std::string file;
    size_t line;
    size_t column;
};

// --run JIT: compiler-rt is not linked, so we provide __emutls_get_address backed by a host
// thread_local map. Per-thread blocks are freed only at process exit (no thread-exit dtor hook).
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

        // Floor at 16 bytes: JIT'd code initializes some thread-locals with aligned SIMD
        // stores (vmovaps), which require 16-byte alignment. Native .tls over-aligns the whole
        // section to 16, so this only bites under emutls, where each var is allocated on its
        // own declared alignment. Match the native guarantee here as a safety net.
        size_t align = c->align ? c->align : alignof(std::max_align_t);
        if (align < 16) align = 16;
        void* mem = _aligned_malloc(c->size, align);
        if (c->value) std::memcpy(mem, c->value, c->size);
        else          std::memset(mem, 0, c->size);
        storage.emplace(control, mem);
        return mem;
    }

    // Without ORC's COFFPlatform (needs orc_rt, not linked), nothing calls RtlAddFunctionTable.
    // Registers .pdata manually so the OS can unwind JIT'd SEH/C++EH frames.
    class SehRegistrationPlugin : public llvm::orc::ObjectLinkingLayer::Plugin
    {
    public:
        void modifyPassConfig(llvm::orc::MaterializationResponsibility&,
                              llvm::jitlink::LinkGraph&,
                              llvm::jitlink::PassConfiguration& Config) override
        {
            // PostPrune (before allocation, so synthesized blocks get memory): redirect the
            // SEH language handler (__C_specific_handler, which resolves to ntdll >4GB away)
            // through an in-image jump stub so its image-relative .xdata reference fits in 32 bits.
            Config.PostPrunePasses.push_back(
                [](llvm::jitlink::LinkGraph& G) { return LowerSEHHandler(G); });

            // PostAllocation (addresses final, before the PreFixup ADDR32NB lowering runs):
            // define __ImageBase ourselves. COFF/x86_64 lowering resolves the image base by
            // scanning defined_symbols() for "__ImageBase"; only if absent does it fall back to
            // an external process lookup that fails (nothing provides __ImageBase -> "Symbols
            // not found"). By defining it at the lowest block, the lowering finds it and bakes
            // .pdata/.xdata RVAs relative to a base inside the JIT'd region (so deltas fit in 32
            // bits), and no external lookup is attempted.
            Config.PostAllocationPasses.push_back(
                [](llvm::jitlink::LinkGraph& G) { return FixImageBase(G); });

            // PostFixup: addresses are final and .pdata holds real RVAs; registering here
            // is safe because unwind cannot fire until after finalization.
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
        // The block with the lowest final address across all sections (the base of the JIT'd
        // image for this graph). Null only if the graph has no blocks.
        static llvm::jitlink::Block* LowestBlock(llvm::jitlink::LinkGraph& G)
        {
            llvm::jitlink::Block* lowest = nullptr;
            for (auto& Sec : G.sections())
                for (auto* B : Sec.blocks())
                    if (!lowest || B->getAddress() < lowest->getAddress()) lowest = B;
            return lowest;
        }

        static llvm::Error LowerSEHHandler(llvm::jitlink::LinkGraph& G)
        {
            llvm::jitlink::Symbol* handler = nullptr;
            for (auto* S : G.external_symbols())
                if (S->hasName() && S->getName() == "__C_specific_handler") { handler = S; break; }
            if (!handler)
                return llvm::Error::success(); // no SEH personality in this graph

            // Collect existing references to the handler FIRST (these are the .xdata image-
            // relative RVAs). Doing this before synthesizing the pointer slot below ensures the
            // slot's own Pointer64 edge - which must hold the true ntdll address - is excluded;
            // repointing it would make the stub jump to itself.
            std::vector<llvm::jitlink::Edge*> toRepoint;
            for (auto& Sec : G.sections())
                for (auto* B : Sec.blocks())
                    for (auto& E : B->edges())
                        if (&E.getTarget() == handler)
                            toRepoint.push_back(&E);

            // 8-byte slot holding the real 64-bit handler address (Pointer64 -> ntdll), plus a
            // 6-byte `jmp *slot(%rip)` stub. Both live in the JIT'd image next to the code, so
            // an image-relative reference to the stub fits in 32 bits.
            // MemProt's bitmask operator| is not exported into llvm::orc here; OR the bits by value.
            auto memProt = [](unsigned bits) { return static_cast<llvm::orc::MemProt>(bits); };
            constexpr unsigned R = (unsigned)llvm::orc::MemProt::Read;
            constexpr unsigned W = (unsigned)llvm::orc::MemProt::Write;
            constexpr unsigned X = (unsigned)llvm::orc::MemProt::Exec;
            auto& ptrSec  = G.createSection("$__cflat_sehptr",  memProt(R | W));
            auto& stubSec = G.createSection("$__cflat_sehstub", memProt(R | X));
            auto& ptrSym  = llvm::jitlink::x86_64::createAnonymousPointer(G, ptrSec, handler);
            auto& stubSym = llvm::jitlink::x86_64::createAnonymousPointerJumpStub(G, stubSec, ptrSym);

            for (auto* E : toRepoint)
                E->setTarget(stubSym);
            return llvm::Error::success();
        }

        static llvm::Error FixImageBase(llvm::jitlink::LinkGraph& G)
        {
            for (auto* S : G.defined_symbols())
                if (S->hasName() && S->getName() == "__ImageBase")
                    return llvm::Error::success(); // graph already supplies one
            llvm::jitlink::Block* lowest = LowestBlock(G);
            if (!lowest)
                return llvm::Error::success(); // empty graph - nothing to anchor to
            // Local scope: consumed by the lowering pass within this graph; not exported.
            G.addDefinedSymbol(*lowest, 0, "__ImageBase", 0, llvm::jitlink::Linkage::Strong,
                               llvm::jitlink::Scope::Local, /*IsCallable=*/false, /*IsLive=*/true);
            return llvm::Error::success();
        }

        // Out-of-line in LLVMBackend.cpp to avoid an extern "C" clash with <winnt.h> in TUs
        // that include windows.h (e.g. LspServer.cpp).
        static llvm::Error RegisterUnwindInfo(llvm::jitlink::LinkGraph& G);
    };
}

// Strip surrounding quotes from an ANTLR StringLiteral token text (e.g. `"foo"` -> `foo`).
// Returns the raw text unchanged if it is shorter than 2 characters.
inline std::string DequoteStringLiteral(const std::string& raw)
{
    return raw.size() >= 2 ? raw.substr(1, raw.size() - 2) : raw;
}

#if defined(__APPLE__)
// Run a shell command and return its trimmed first stdout line ("" on failure).
// Used for xcrun SDK discovery and the clang compiler-rt resource dir.
inline std::string CaptureToolLine(const char* cmd)
{
    std::string out;
    if (FILE* p = popen(cmd, "r"))
    {
        char buf[1024];
        if (fgets(buf, sizeof(buf), p)) out = buf;
        pclose(p);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    }
    return out;
}

// Cached real SDK path for macOS C-header binding ($SDKROOT, else `xcrun --show-sdk-path`).
// The harvested ~/.cflat/macsdk carries link stubs but no headers, so it is never used here.
// Empty when no SDK is available (no Xcode / Command Line Tools and no $SDKROOT).
inline const std::string& MacSdkPathCached()
{
    static const std::string sdk = [] {
        std::string s;
        if (const char* env = std::getenv("SDKROOT")) if (env[0]) s = env;
        if (s.empty()) s = CaptureToolLine("xcrun --show-sdk-path 2>/dev/null");
        return s;
    }();
    return sdk;
}
#endif

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
        // A span<T> accessor routes through `this` and cannot carry the noalias contract,
        // causing a surviving runtime alias check even with vectorize.
        bool hasSpanAccessor = false;
        std::string spanAccessor;       // "get", "set", or "operator[]"
        std::string spanReceiver;       // receiver variable name (for the `.data()` hint), if known
        int spanLine = 0, spanCol = 0;  // the accessor site
    };
    void AddVectorizeLoopInfo(const VectorizeLoopInfo& info) { vectorizeLoops_.push_back(info); }

    int ArrayViewBufferFieldIndex(const std::string& typeName)
    {
        if (typeName.empty()) return -1;
        const auto& ds = GetDataStructure(typeName);
        for (int i = 0; i < (int)ds.StructFields.size(); ++i)
            if (ds.StructFields[i].VariableName == "_ptr" && ds.StructFields[i].IsArrayView)
                return i;
        return -1;
    }

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

    enum class CallingConv { Default, Stdcall, Cdecl };

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
        bool IsAlias = false;    // return/decl declared with 'alias' - by-value borrow; caller must not free the interior
        bool IsBond = false;     // parameter declared with 'bond' - return value borrows from this parameter; return must not outlive it
        CallingConv CallConv = CallingConv::Default; // calling convention for extern declarations (Stdcall/Cdecl; Default = compiler-chosen)

        // Function pointer fields (IsFunctionPointer == true)
        bool IsFunctionPointer = false;
        // When IsFunctionPointer is set, the closure TypeName distinguishes the two flavours:
        //   thin  (`function<T>`): a bare C function pointer R(*)(Args), 8 bytes, no env,
        //                          non-capturing, C-ABI compatible. TypeName == "__c_fn_ptr".
        //   fat   (`Lambda<T>`):     the owning closure {code, env}, 16 bytes. TypeName == "__closure_fat_ptr".
        // IsThinFnPtr() is DERIVED from TypeName (not stored) so the two can never drift; set
        // TypeName to one of the two names above and this follows.
        bool IsThinFnPtr() const { return TypeName == "__c_fn_ptr"; }
        std::string FuncPtrReturnTypeName;
        bool FuncPtrReturnPointer = false;
        struct FuncPtrParam { std::string TypeName; bool Pointer = false; bool IsMove = false; };
        std::vector<FuncPtrParam> FuncPtrParams;

        uint64_t ConstArraySize = 0;                   // outer (first) dimension; non-zero for fixed arrays
        std::vector<uint64_t> ConstInnerDimensions;   // inner dimensions for multi-dim arrays (e.g. [M] in T[N][M])

        // Alias-provided array sizes (using Vec3 = float[3]). Peeled by ParseDeclarationSpecifiers
        // so the per-declarator step can adopt them when the declarator has no brackets. 0 = none.
        uint64_t AliasArraySize = 0;
        std::vector<uint64_t> AliasInnerDims;

        // simd<T,N>: lowers to LLVM <N x T>. Register-resident primitive - never combined with
        // Pointer or ConstArraySize.
        bool IsSimd = false;
        uint64_t SimdLanes = 0;

        // Thin `int[]` array-view: like `int*` but carries a noalias contract. Pointer arithmetic
        // and `int* -> int[]` casts are forbidden so sub-views are unconstructible. See doc/LANGUAGE.md.
        bool IsArrayView = false;

        // Keyed by ORIGIN (not SSA value) so copies of the same view share a scope and are not
        // treated as disjoint. Never serialized; recomputed per-function from aliasScopes_.
        int NoaliasScopeId = -1;

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
            if (TypeName == "float")  return 32;
            if (TypeName == "double") return 64;
            return -1;
        }

        std::string ToUniqueString() const
        {
            if (IsFunctionPointer)
            {
                std::string s = (IsThinFnPtr() ? "cfuncptr_" : "funcptr_")
                              + FuncPtrReturnTypeName + (FuncPtrReturnPointer ? "Ptr" : "");
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
        std::string Value;  // FIRST arg's raw text, empty for no-arg annotations
        // All args in source order ([Capability(ILockable, ICvWaitable)] -> two entries).
        // Value mirrors Values[0]; single-arg consumers keep reading Value.
        std::vector<std::string> Values;
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

        // User-requested alignment from `alignas(N)`. 0 means unset; honored only when
        // greater than the type's ABI alignment. Power of two, validated at parse time.
        uint64_t UserAlignValue = 0;

        // Bitfield support (struct/union fields only; 0 = not a bitfield).
        // BitOffset is LSB-first within the storage unit (MSVC ordering); StorageFieldIndex is the LLVM struct element index.
        unsigned BitWidth = 0;
        unsigned BitOffset = 0;
        unsigned StorageFieldIndex = 0;
        bool IsBitfield = false;
        // True on the synthesized storage slot from PackBitfields. Used to zero-initialize
        // the slot even when no per-bitfield initializer was written.
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
        uint64_t AllocAlignment = 0;     // per-allocation alignment from `new T[n] alignas(N)` (>16 = over-aligned); frees via __delete_aligned
        bool IsOwningString = false;     // true when a string local owns its heap buffer - destructor called on scope exit
        bool BorrowsOwnedString = false; // true when a string local was initialized/assigned from an owning string FIELD (a non-owning alias of a heap buffer some struct still owns) - storing it into another field would double-free, so the field-store path rejects it
        bool IsOwningStruct = false;     // true for move parameters of struct types with destructors - destructor called on scope exit
        bool IsMoved = false;            // compile-time: true after this variable's ownership was transferred via a move call
        bool MovedIntoInterface = false; // compile-time: ownership was boxed into an interface local ('IFace x = ptr'); 'delete ptr' is a no-op that leaks - delete the interface instead
        std::set<std::string> MovedFields; // compile-time: field names moved out of this variable via a 'move' of a sub-path (e.g. `node->left`) - the base stays usable
        bool IsBonded = false;           // compile-time: true when this variable holds a bonded (borrowed) return value
        bool BondByAddress = false;      // bond originates from a by-address lambda capture; reassigning the source is safe
        std::vector<std::string> BondedSources; // names of bond parameters this value borrows from
        // Capture names for lambda literals, in capture order. Empty for non-capturing lambdas.
        // Used to diagnose capturing lambdas passed to C function-pointer params (C ABI can't carry state).
        std::vector<std::string> LambdaCaptureNames;
        bool IsBorrowed = false;         // compile-time: true for non-move pointer parameters and locals that alias one - 'delete' is forbidden
        bool IsAliasBorrow = false;      // compile-time: local bound from an `alias` return - shallow-aliases storage it does not own, so its scope-exit destructor is suppressed
        std::string BorrowedOrigin;      // name of the borrowed parameter this value transitively aliases (for diagnostics)
        llvm::Value* RefCountStorage = nullptr; // lazy i32 alloca at function entry; non-null only when pointer escaped to a field
        std::string CallerName;          // the variable's name at the call site, for move tracking
        std::string OwningStructName;    // when this NamedVariable is a struct-field access, the field's owning struct
        std::string FieldName;           // when this NamedVariable is a struct-field access, the field name
        // Field reached THROUGH an interface value (data ptr + the vtable's byte-offset slot). The
        // address is a byte GEP, not a 2-index struct GEP, so field-store rules must be told.
        bool IsInterfaceField = false;
        // Field extracted from a BY-VALUE owning-struct temp (`makeToken().text`): may be read, but
        // persisting it (store/bind/return) double-frees so those sites reject it. See FlushOwnedStructTemps.
        bool FromOwningTempField = false;
        // Parent was a `move`-return temp (the temp OWNS this field): a persist site may MOVE the field
        // out (store + zero the source) instead of forcing `.copy()`. An alias temp leaves these unset.
        bool MovableTempField = false;
        llvm::Value* MoveTempStructAlloca = nullptr;  // the spilled `owntemp` holding the parent struct
        llvm::Type*  MoveTempStructType = nullptr;    // the parent struct's LLVM type, for the field GEP
        unsigned     MoveTempFieldIndex = 0;          // this field's index within the parent struct
        // Bitfield access: non-null BitfieldStorage means this is a bitfield view onto a storage word.
        // Reads compute shift+mask; writes do a read-modify-write on BitfieldStorage.
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

    // Named bitfield. Multiple entries may share StorageFieldIndex when the packing
    // pass groups adjacent bitfields into one storage unit (MSVC LSB-first layout).
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
        // Statically-conformed interfaces from [Capability(...)]. The shape is checked at compile
        // time; the type is NEVER convertible to an interface fat pointer through this list.
        std::vector<std::string> StaticInterfaces;
        std::unordered_map<std::string, llvm::GlobalVariable*> VTables; // Only used by classes
        llvm::GlobalVariable* typeDescriptor = nullptr; // unique per-struct global for type identity
        bool IsUnion = false;
        // User-requested alignment from `alignas(N)` on the struct definition. 0 = unset.
        // Effective alignment is max of this and ABI alignment from LLVM field types.
        uint64_t UserRequestedAlignment = 0;
        // Bitfield side-table: field-name lookup checks this BEFORE StructFields.
        // StructFields has synthetic storage slots (`__bf0` etc.) that are not user-visible.
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

    // ABI lowering for C extern struct-by-value params/returns; Kind selects the lowering
    // strategy. MSVC x64 uses Direct / CoerceToInt / ByVal / SRetReturn. The SysV AMD64 ABI
    // (Linux/macOS) additionally uses CoercePair: a <=16-byte struct that classifies into two
    // "eightbyte" registers, passed as two scalar params (coerceTy + coerceTy2) and returned
    // as a { coerceTy, coerceTy2 } literal aggregate. For SysV, coerceTy may be a float/double/
    // <2 x float> (SSE eightbyte), not only an integer.
    struct AbiSlot
    {
        enum Kind { Direct, CoerceToInt, ByVal, SRetReturn, CoercePair };
        Kind kind = Direct;
        llvm::Type* coerceTy = nullptr;     // eightbyte 0 type for CoerceToInt / CoercePair
        llvm::Type* coerceTy2 = nullptr;    // eightbyte 1 type for CoercePair
        llvm::StructType* structTy = nullptr; // pointee for ByVal / SRetReturn / coerce source
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
        bool ReturnsAlias = false; // true when the function returns an 'alias' by-value borrow - caller must not free the interior
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

    // 'auto' return-type inference: CreateReturnCall emits UnreachableInst placeholders instead
    // of ret; the caller splices BBs and replaces placeholders after unifying all return types.
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
    uint64_t lastAllocAlignment = 0;         // set by ParseNewExpression for `new T[n] alignas(N)` (>16); consumed by ParseDeclaration into NamedVariable.AllocAlignment
    bool currentFunctionReturnsOwned = false; // true when current function is declared with move T* or move string return type
    bool currentFunctionReturnIsArrayView = false; // true when the current function's return type is a `T[]` array-view
    std::string currentFunctionReturnTypeName; // declared return TypeName of the current function (e.g. an interface name); used to box a returned concrete pointer into the interface fat pointer
    TypeAndValue currentFunctionReturnTV; // full declared return TypeAndValue of the current function; used to thread a function<> return type into a returned lambda literal's expected type

    // Returning a struct by value hands its member pointers to the caller; running the local's
    // dtor on the return path would dangle them. EmitDestructorsForScope skips this alloca.
    llvm::Value* returnedStructDtorSkipAlloca = nullptr;

    // Unnamed ReturnsOwned string intermediates (e.g. a+b in a+b+c). Invisible to
    // EmitDestructorsForScope; freed at end-of-full-expression by FlushOwnedStringTemps.
    std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> pendingOwnedStringTemps;

    // Lambda literals with unclaimed heap envs (no named owner). Closure analog of
    // pendingOwnedStringTemps; freed at end-of-full-expression by FlushOwnedClosureTemps.
    std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> pendingOwnedClosureTemps;

    // Spilled allocas of unbound owning-struct `move`-return temps (`makeToken().text`), freed by
    // FlushOwnedStructTemps. Struct analog of the string/closure temp lists. Block = dominance guard.
    struct PendingOwnedStructTemp { llvm::Value* Alloca; std::string TypeName; llvm::BasicBlock* Block; };
    std::vector<PendingOwnedStructTemp> pendingOwnedStructTemps;

    // Per-function noalias metadata for T[] views. Proves pairwise disjointness for span<T> fields
    // where the noalias parameter attribute cannot reach. Reset in createFunctionBlock.
    llvm::MDNode* aliasDomain_ = nullptr;
    std::vector<llvm::MDNode*> aliasScopes_;
    std::map<std::string, int> viewScopeByOrigin_; // origin name -> index into aliasScopes_

    bool lastCallIsBonded = false;           // set when the last call returned a bonded (borrowed) value
    bool lastCallBondByAddress = false;      // set when the bond originates from a by-address lambda capture (kind A)
    std::vector<std::string> lastCallBondedSources; // bond parameter names the last call's return borrows from
    // Capture names of the last lambda literal, for the C function-pointer diagnostic. Uses the
    // compiler-level channel because lastLambdaType is cleared too early by postfix processing.
    std::vector<std::string> lastCallLambdaCaptureNames;
    std::vector<std::string> lastCallRequiredLocks;  // RequiredLocks of the last resolved overload (for call-site lock checking)
    std::vector<std::string> lastCallParameterNames; // VariableName of each parameter of the last resolved overload
    // Set while parsing the declarations of a file-scope lock group; CreateGlobalVariable
    // stamps it onto the global's TypeAndValue.GuardedBy. Empty outside such a group.
    std::string pendingGlobalGuardedBy;

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
            // LSP mode: cout is redirected to stderr and would duplicate the diagnostic already
            // sent to the client over the sink. Don't echo.
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

    // In batch/--check mode throws so the loop continues; otherwise exits with code 1.
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
            // LSP mode: route through the sink so warnings appear in the editor,
            // not just on stderr where they were invisible.
            diagnosticSink_(sourceFileName, currentLine, currentColumn, message, 2);
            return;
        }
        std::cout << std::format("{}({},{}): warning: {}\n", sourceFileName, currentLine, currentColumn, message);
    }

    friend class MainListener;
    friend class ForwardRefScanner;
    friend class CrossThreadEscapeScanner;

public:
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
    // Declaration LINE of each global, for same-scope redeclaration detection. A true in-source
    // duplicate is the same name at DIFFERENT lines (`int g=1; int g=2;`). The same name at the
    // SAME line is benign re-registration of one declaration: a core file analyzed as a root (via a
    // temp-copy path, as the LSP does) is also pulled in via the auto-import graph, so its globals
    // register twice from the same source line - the temp copy preserves line numbers, but its path
    // differs from the real file, so line (not path) is the stable identity. Cleared by
    // ResetForReanalysis like the maps above.
    std::unordered_map<std::string, int> globalDeclSite;
    // Definition order of the globals that end-of-main destruction covers (see
    // EmitGlobalDestructorsInMain). Excludes externs, thread-locals and core-library globals.
    std::vector<std::string> globalDtorOrder_;
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
    // `using IReference = Windows.Foundation.IReference;` - alias -> generic BASE name. Separate
    // from typeAliases because a base is not a type until its <...> arguments are supplied.
    std::unordered_map<std::string, std::string> genericBaseAliases_;
    // Closure type aliases (`using Cb = function<R(Args)>;`). Cannot live in string-shaped
    // typeAliases because a closure type carries a full call signature, not a plain type name.
    std::unordered_map<std::string, TypeAndValue> functionTypeAliases;
    // Closure types used as generic arguments (e.g. list<Lambda<int(int)>>) are encoded into a
    // symbol-safe name (see BuildEncodedClosureName in MainListener.h) and registered here so the
    // call descriptor (signature + fat/thin) is recoverable at the invoke site. The encoded name
    // is also a real dataStructures entry (fat -> owning closure struct; thin -> {i8*} POD) so it
    // behaves as an ordinary value-type element for storage / pointer-wrap / element destruction.
    std::unordered_map<std::string, TypeAndValue> encodedClosureTypes_;
    // Fallback typedef map (HANDLE->void*, etc.) for the type mapper when canonical spellings
    // don't resolve. Process-wide, first-writer-wins.
    std::unordered_map<std::string, std::string> cTypedefMap_;
    std::unordered_map<std::string, std::vector<FunctionSymbol>> functionTable;
    std::unordered_map<std::string, std::vector<InterfaceMethod>> interfaceTable;
    // Interface fields (parents' fields first, then own), parallel to interfaceTable. Each entry's
    // VariableName is the field name; every implementor must expose a field of the same name/type.
    std::unordered_map<std::string, std::vector<TypeAndValue>> interfaceFields;
    std::unordered_map<std::string, std::vector<std::string>> interfaceParents;
    // Type-level annotations keyed by type/interface name ([winrt] on a class, [uuid("...")] on an
    // interface, ...). Field annotations are separate (StructData.StructFields[].Annotations).
    std::unordered_map<std::string, std::vector<AnnotationValue>> typeAnnotations_;

    // Set (or, when empty, clear) the annotations recorded for a type/interface. Empty erases so a
    // re-analysis where a type loses its annotations does not keep the stale entry.
    void SetTypeAnnotations(const std::string& name, std::vector<AnnotationValue> anns)
    {
        if (anns.empty()) typeAnnotations_.erase(name);
        else typeAnnotations_[name] = std::move(anns);
    }
    // Find a single named annotation on a type/interface, or nullptr if absent.
    const AnnotationValue* FindTypeAnnotation(const std::string& name, const std::string& annName) const
    {
        auto it = typeAnnotations_.find(name);
        if (it == typeAnnotations_.end()) return nullptr;
        for (const auto& a : it->second)
            if (a.Name == annName) return &a;
        return nullptr;
    }
    // The raw argument of a named type/interface annotation (e.g. the GUID of [uuid]); empty if absent.
    std::string GetTypeAnnotationArg(const std::string& name, const std::string& annName) const
    {
        auto* a = FindTypeAnnotation(name, annName);
        return a ? a->Value : std::string{};
    }

    // Bookkeeping for a [winrt] class lowered to a COM object (thin-ptr ABI). Records the
    // implemented interface, its generated vtable struct type, and the static vtable instance
    // wired into every object's lpVtbl field by `new`.
    struct WinrtClassInfo
    {
        std::string InterfaceName;
        llvm::StructType* VtableType = nullptr;
        llvm::GlobalVariable* VtableInstance = nullptr;
    };
    std::unordered_map<std::string, WinrtClassInfo> winrtClasses;
    // Mangled HResult<T> type name for each value-returning [winrt] vtable slot, keyed by
    // "className::methodName". Populated when the [winrt] class is parsed (MainListener primes the
    // HResult<T> instantiation there); read by EmitWinrtSlotCall to build the sugar's result.
    std::unordered_map<std::string, std::string> winrtSlotHResultType_;
    // Consume-side (imported .winmd) bookkeeping. Keyed by WinRT full name. Underlying scalar
    // for each imported enum (so an enum-typed param maps to its integer); the set of imported
    // value structs (so a struct-typed field/param can pass by value when we have registered it).
    std::unordered_map<std::string, std::string> winrtEnumUnderlying_;
    std::unordered_set<std::string> winrtValueStructs_;
    // Parameterized (generic) interface templates from imported .winmd, keyed by WinRT FULL name
    // (Windows.Foundation.Collections.IVector, ...). Kept so a qualified `IVector<int>` can be
    // instantiated on demand into a concrete COM vtable + thin pointer + derived PIID.
    std::unordered_map<std::string, cflat_winmd::Interface> winrtGenericTemplates_;
    // Derived per-instantiation IID (PIID) keyed by mangled name (e.g. "IVector__int"), used for
    // QueryInterface identity and the `iidof(...)` builtin.
    std::unordered_map<std::string, std::array<uint8_t, 16>> winrtInstanceIid_;
    // Thin COM interface pointer structs built by BuildWinrtInterfaceStructs (both non-generic
    // imports and concrete generic instantiations). Used to route foreach over a winmd interface
    // through the IIterable<T>/IIterator<T> protocol instead of the count()/get() index path.
    std::unordered_set<std::string> winrtThinInterfaces_;
    // Projected delegate objects built by EmitWinrtDelegateObject, keyed by mangled instance name
    // (e.g. "AsyncOperationCompletedHandler__string"). Caches the COM object struct type and the
    // static vtable so repeated winrtDelegate(...) sites reuse one type/vtable per instantiation.
    std::unordered_map<std::string, llvm::StructType*> winrtDelegateObjTy_;
    std::unordered_map<std::string, llvm::GlobalVariable*> winrtDelegateVtbl_;
    // All imported winmd types accumulated across every imported file, so the signature encoder
    // can resolve nested named type args (enums/structs/interfaces) when deriving a PIID.
    cflat_winmd::Model winrtConsumedModel_;
    std::string winrtConsumedLspFile_;
    std::unordered_map<std::string, llvm::Constant*> stringPool;
    std::unordered_set<std::string> namespaceTable;
    // Set/restored around a namespace member's body so unqualified sibling references
    // (e.g. "helper" inside "N") resolve to the enclosing-namespace symbol "N.helper".
    std::string currentNamespace_;
    std::unordered_map<std::string, std::unordered_set<std::string>> importAliasMembers;
    std::unordered_set<std::string> importedFiles;
    std::vector<std::string> importStack;  // DFS stack for circular import detection
    std::vector<std::string> importSearchDirs;  // -i dirs, searched in order (first match wins)
    std::string runtimeDir;
    std::string sourceFileDir_;  // original source dir for LSP temp-file analysis
    // Override name for diagnostics: LSP analyzes a temp copy, so this carries the real
    // source path so diagnostics and __FILE__ don't show the temp name.
    std::string sourceDisplayName_;

    // Per-backend generic-template state, shared with MainListener via references.
    GenericTemplateState gts;
private:
    // When true, disables auto-import of core/runtime.cb
    bool skipRuntimeImport = false;
    bool verbose = false;
    bool batchMode_ = false;
    bool noCache_ = false;
    bool cHeaderCacheDeep_ = false;  // --c-header-cache-deep: transitive validation of cached C headers
    // --cpu/-mcpu (ISA + scheduling) and --tune/-mtune (scheduling only). Resolved and
    // validated in Compile so EmitExecutable and C interop can use them verbatim.
    std::string targetCpu_;
    std::string tuneCpu_;
    int platformValue = 64;  // 64 for win64, 32 for win32
    // Target OS: drives __WINDOWS__/__POSIX__ macros and core OS-library selection.
    // Defaults to the host OS (no cross-OS compilation yet).
#if defined(_WIN32)
    bool targetWindows_ = true;
#else
    bool targetWindows_ = false;
#endif
    // Darwin/Mach-O target (--platform macos). Implies !targetWindows_ + POSIX.
    // arm64 is the only supported macOS arch; the architecture is tracked
    // separately because every prior target was x86 (the eightbyte struct ABI,
    // va_list lowering, and triple all assume x86-64 unless this is set).
    // Defaults to the host OS (the CLI overrides via --platform; the LSP Analyze
    // path does not, so the raw default must match the host or if-const(__MACOS__)
    // branches and os.macos.cb selection go the wrong way during LSP analysis).
#if defined(__APPLE__)
    bool targetMacOS_ = true;
  #if defined(__aarch64__) || defined(__arm64__)
    bool targetArm64_ = true;
  #else
    bool targetArm64_ = false;
  #endif
#else
    bool targetMacOS_ = false;
    bool targetArm64_ = false;
#endif
    std::vector<std::string> cObjectFiles_;
    int cOptLevel_ = 0;        // optimization level applied to clang C compiles
    bool cDebugInfo_ = false;  // emit CodeView for clang C compiles
    // Off by default; when off, codegen/linking is byte-for-byte identical (no overhead).
    bool asan_ = false;

    // --heap-audit: when set, force-import diagnostic/heap_audit.cb and instrument main
    // with HeapAudit.enable()/reportLeaks() (see InjectHeapAuditIntoMain). Report-only.
    bool heapAudit_ = false;

    // --run: jitExitCode_ carries the program's exit status (Compile returns compile success only).
    // runArgs_ are forwarded as argv[1..] to main(int argc, char** argv); empty for main().
    bool runMode_ = false;
    int  jitExitCode_ = 0;
    std::vector<std::string> runArgs_;

    // --xthread-scan N state. Findings go to stdout (not the diagnostic sink) and never
    // affect the exit code or LSP. Cleared by ResetForReanalysis.
    int xthreadScanLevel_ = 0;
    std::unordered_set<std::string> threadSharedTypes_;

    // Gathered at codegen time (AST available); matched to failed loops by source line in
    // `!llvm.loop` metadata to produce precise diagnostics. Cleared by ResetForReanalysis.
    std::vector<VectorizeLoopInfo> vectorizeLoops_;
    // Dedupe set so each [xthread] finding prints at most once. Cleared by ResetForReanalysis.
    std::unordered_set<std::string> xthreadReported_;
    // --c-include dirs and --c-lib import libraries. A sibling runtime DLL of each lib
    // is copied next to the output exe after a successful link.
    std::vector<std::string> cIncludeDirs_;
    std::vector<std::string> cLinkLibs_;
    std::vector<std::string> cDefines_;
    // macOS frameworks requested via `import framework "X"` / --framework. Each becomes
    // a `-framework X` pair on the Mach-O link. Deduped, first-seen order preserved.
    std::vector<std::string> cFrameworks_;
    // Authoritative DLL list from vcpkg_installed/<triplet>/bin, copied next to the exe.
    // Kept separate from cLinkLibs_-based DLL probing.
    std::vector<std::string> vcpkgRuntimeDlls_;
    // Absolute path of a .pri harvested from a package-nuget `pri "..."` clause, deployed
    // next to the exe as <exe>.pri (WinUI MRT probes resources.pri and <exe>.pri).
    std::string deployPriPath_;
    VcpkgResolver vcpkg_;
    NugetResolver nuget_;

    // Process-global mutex-guarded cache of C signatures from clang AST dumps. Global because
    // LSP backends run concurrently and a document is not pinned to a slot.
    struct CSigEntry
    {
        std::string name;
        TypeAndValue ret;
        std::vector<TypeAndValue> params;
        bool variadic = false;
        std::string file;  // declaring header (presumed loc), for go-to-definition
        int line = 1;
        int col = 0;
    };
    struct CEnumEntry
    {
        std::string name;
        long long value = 0;
        int line = 1;
        int col = 0;
    };
    struct CGlobalEntry
    {
        std::string name;
        TypeAndValue type;
        int line = 1;
        int col = 0;
    };
    // Object-like C macro. Function-like macros are skipped (no preprocessor). Source location
    // preserved from the -dD pass so LSP can go-to-definition into the real header.
    struct CMacroEntry
    {
        std::string name;
        long long value = 0;
        double floatValue = 0.0;
        std::string file;
        int line = 1;
        int col = 0;
        // Natural type resolves to void* (sentinel pointer like INVALID_HANDLE_VALUE).
        // Emitted as a void* global so it can be compared against pointer-returning APIs.
        bool isPointer = false;
        // Natural type is float/double. Routes on type, not fold success: `(double)(0x100000000)`
        // folds fine but is an integer constant - type routing prevents misclassifying it.
        bool isFloat = false;
        // Natural type is a string literal. Routed via __typeof__ so `(int)"x"` stays integer.
        bool isString = false;
        std::string stringValue;
        // CFlat integer type from the macro's natural C type (e.g. DWORD -> "u32"). Lets callers
        // match the header's parameter type without an explicit cast (e.g. GetStdHandle).
        std::string intTypeName;
        // Natural type is a C function-pointer. Emitted as function<R(P...)> initialized to a
        // fat struct {thunk, env=intToPtr(value)}, same wire format as a C-returned fn ptr.
        bool isFuncPtr = false;
        TypeAndValue funcPtrTV;
    };
    // Field types kept as raw C spellings so they can be re-resolved after all records in the
    // TU are registered (handles forward references between structs).
    struct CRecordFieldEntry
    {
        std::string name;
        std::string ctype;
        // CFlat replicates MSVC ABI layout itself; bitOffset is NOT taken from clang's
        // reported offset - RegisterCRecords computes it from MSVC ABI rules.
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
        // Header-COM interface IID (hyphenated GUID) from __declspec(uuid), or empty. Registered
        // as the type's "uuid" annotation so iidof()/ComPtr<T> resolve a header-COM IID.
        std::string uuid;
    };
    // Function-like C macro translated to a CFlat 'auto' generic. The translation pass filters
    // the body against an allowlist of safe operators; anything else is rejected.
    struct CFunctionMacroEntry
    {
        std::string name;
        std::vector<std::string> params;
        std::string body;
        std::string file;
        int line = 1;
        int col = 0;
    };
    struct CMacroNameCand
    {
        std::string name;
        std::string file;
        int line = 1;
    };
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
        // typedef-name -> tag aliases (MSG -> tagMSG). Cached so disk-cache hits replay them.
        std::vector<std::pair<std::string, std::string>> recordAliases;
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
    // Armed before ProcessImports so import-time errors can match. An unmatched expectation
    // at end of compilation is a did-not-occur failure.
    std::string fileScopeExpectedError_;

    // Compile-time macros (constant throughout compilation, set early)
    struct CompileTimeMacro
    {
        std::string name;
        llvm::Constant* value;
        std::string type;  // "int", "string", etc.
    };
    std::unordered_map<std::string, CompileTimeMacro> compileTimeMacros;
    // ANTLR ecosystem kept alive so generic-template ctx pointers remain valid. Core-library
    // entries reused across compiles; deliberately survives ResetForReanalysis.
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
    // Core-library imports only (stable for the process lifetime). User imports deliberately
    // excluded: they change during editing, so bounding to core keeps staleness trivial.
    std::unordered_map<std::string, std::unique_ptr<CachedParseTree>> parseTreeCache_;
    // Per-compile lifetime anchor for NON-core imported parse trees (generic-template ctx
    // pointers point into them). Cleared by ResetForReanalysis; parseTreeCache_ is not.
    std::vector<std::unique_ptr<CachedParseTree>> importedParseStates;

    CachedParseTree* GetOrParseFile(const std::string& canonicalPath, const std::string& displayName, bool isCore);
    // Parser ecosystem must outlive instantiation: genericFunctionTemplates holds raw
    // FunctionDefinitionContext pointers into these trees.
    struct SyntheticParseState
    {
        std::string label;                                          // for diagnostics
        std::unique_ptr<antlr4::ANTLRInputStream> input;
        std::unique_ptr<CFlatLexer> lexer;
        std::unique_ptr<antlr4::CommonTokenStream> tokens;
        std::unique_ptr<CFlatParser> parser;
    };
    std::vector<SyntheticParseState> syntheticParseStates_;
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
    // nullptr entry means "type needs no destruction"; non-null may be the plain user dtor
    // or a synthesized wrapper that also runs dtor-bearing members.
    std::unordered_map<std::string, llvm::Function*> fullDestructorCache_;
    std::unordered_set<std::string> fullDestructorInProgress_;
    // Deferred `delete`-site destructor wrappers (see GetFullDestructorForDelete): bound at
    // a delete site when the element type is not yet complete; bodies emitted at finalization.
    std::unordered_map<std::string, llvm::Function*> deferredFullDtor_;
    std::vector<std::string> deferredFullDtorOrder_;
    std::unordered_map<std::string, llvm::Function*> memberwiseCopyCache_;
    std::function<void(const std::string&, size_t, size_t, const std::string&, int)> diagnosticSink_;
    std::function<void(int, int, int, int, const std::string&)> hintRegionSink_;
    LspSymbolIndex* symbolSink_ = nullptr;

    llvm::Function* currentFunction;
    std::string sourceFileName;
    std::string currentSourceFilePath_;
    // True while the file being scanned/walked lives under runtimeDir/core (set by CompileImportedFile).
    bool currentSourceIsCore_ = false;
    llvm::AllocaInst* autoVaListAlloca = nullptr;

    std::unique_ptr<llvm::DIBuilder> diBuilder;
    llvm::DIFile* diFile = nullptr;
    llvm::DICompileUnit* compileUnit = nullptr;
    llvm::DISubprogram* currentSubprogram = nullptr;
    // Base type name -> DIType (no pointer/array wrapper). Wrappers built on demand so
    // the key stays simple.
    std::unordered_map<std::string, llvm::DIType*> diTypeCache;
    // Without this, all imported-file functions are attributed to the primary diFile, causing
    // line-number collisions (a breakpoint at line N fires on any imported function at line N).
    std::unordered_map<std::string, llvm::DIFile*> diFileCache_;

    // Deferred so a generic struct is not opaque when DI is emitted; FinalizeDebugInfo
    // runs after all layouts are complete.
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
    llvm::Function* createFunctionProto(const std::string& name, llvm::FunctionType* returnType)
    {
        auto fn = llvm::Function::Create(returnType, llvm::Function::ExternalLinkage, name, *module);

        // CFlat treats null pointer dereferences as defined behavior (hardware fault -> SEH).
        // NullPointerIsValid prevents instcombine from removing null loads/stores as UB.
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

        // Free the pointer. An over-aligned buffer (`new T[n] alignas(N)`, N>16) came
        // from the aligned allocator, so it must be freed via __delete_aligned to match;
        // otherwise use the allocator-aware operator delete.
        auto* voidPtrTy = builder->getInt8Ty()->getPointerTo();
        auto* voidPtr = builder->CreateBitCast(ptrVal, voidPtrTy);
        llvm::Function* alignedDel = namedVar.AllocAlignment > 16 ? GetFunction("__delete_aligned") : nullptr;
        if (alignedDel)
            builder->CreateCall(alignedDel->getFunctionType(), alignedDel, { voidPtr });
        else if (auto* opDel = GetFunction("operator delete"))
            builder->CreateCall(opDel->getFunctionType(), opDel, { voidPtr });

        builder->CreateBr(afterBB);
        builder->SetInsertPoint(afterBB);
    }

    void RegisterOwnedStringTemp(llvm::Value* value)
    {
        if (value == nullptr) return;
        // Idempotent: an SSA value owns exactly one buffer, so it must be freed once.
        // A comparison operand that is an operator+ result is registered here both by
        // TryBinaryOperatorOverload and by the comparison operand pass - dedup avoids a
        // double free at flush.
        for (const auto& e : pendingOwnedStringTemps)
            if (e.first == value) return;
        pendingOwnedStringTemps.emplace_back(value, builder->GetInsertBlock());
    }

    void UnregisterOwnedStringTemp(llvm::Value* value)
    {
        if (value == nullptr) return;
        std::erase_if(pendingOwnedStringTemps,
            [&](const std::pair<llvm::Value*, llvm::BasicBlock*>& e) { return e.first == value; });
    }

    // True when the insert block is active (non-null, no terminator yet).
    // Guards the Flush* functions: emitting into a terminated block is illegal IR.
    bool IsInsertBlockLive() const
    {
        auto* b = builder->GetInsertBlock();
        return b != nullptr && b->getTerminator() == nullptr;
    }

    // Free-safety check for a pending owned temp registered in block `bb`: the value
    // defined there may be freed at `curBlock` iff `bb` dominates `curBlock` (every
    // path to here passed through its definition). Same-block is the trivial case;
    // a plain `bb == curBlock` test is too conservative when an intermediate temp's
    // block precedes - and dominates - a later child's closure-built block in the
    // same full-expression. The DominatorTree is built lazily (only on the first
    // cross-block temp) and reused across one flush via the caller-owned `dt`.
    bool OwnedTempDominatesHere(llvm::BasicBlock* bb, llvm::BasicBlock* curBlock,
                                std::optional<llvm::DominatorTree>& dt) const
    {
        if (bb == nullptr || curBlock == nullptr) return false;
        if (bb == curBlock) return true;
        if (bb->getParent() != curBlock->getParent()) return false;
        if (!dt) dt.emplace(*curBlock->getParent());
        return dt->dominates(bb, curBlock);
    }

    void FlushOwnedStringTemps()
    {
        if (pendingOwnedStringTemps.empty()) return;

        auto* curBlock = builder->GetInsertBlock();
        if (IsInsertBlockLive())
        {
            std::optional<llvm::DominatorTree> domTree;
            auto* strTy = llvm::StructType::getTypeByName(*context, "string");
            for (auto& [value, bb] : pendingOwnedStringTemps)
            {
                if (value == nullptr || !OwnedTempDominatesHere(bb, curBlock, domTree)) continue; // dominance safety
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

    // True when `value` is a freshly-lowered lambda-literal temp (registered, not yet claimed).
    // Used to gate the implicit non-capturing-lambda -> thin function<T> coercion: a stored
    // Lambda<T> value is NOT a temp, so it cannot implicitly narrow (must use .toFunction()).
    bool IsOwnedClosureTemp(llvm::Value* value) const
    {
        for (const auto& e : pendingOwnedClosureTemps)
            if (e.first == value) return true;
        return false;
    }

    void FlushOwnedClosureTemps()
    {
        if (pendingOwnedClosureTemps.empty()) return;

        auto* curBlock = builder->GetInsertBlock();
        if (IsInsertBlockLive())
        {
            auto* closureTy = GetClosureFatPtrType();
            auto* dtor = GetOrCreateFullDestructor("__closure_fat_ptr");
            if (dtor != nullptr)
            {
                std::optional<llvm::DominatorTree> domTree;
                for (auto& [value, bb] : pendingOwnedClosureTemps)
                {
                    if (value == nullptr || !OwnedTempDominatesHere(bb, curBlock, domTree)) continue; // dominance safety
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

    // Register a by-value owning-struct temp (already spilled to `alloca`) for full destruction at
    // the end of the current full expression. The dtor takes a T*, so no spill is needed at flush.
    void RegisterOwnedStructTemp(llvm::Value* alloca, const std::string& typeName)
    {
        if (alloca == nullptr || typeName.empty()) return;
        pendingOwnedStructTemps.push_back({ alloca, typeName, builder->GetInsertBlock() });
    }

    void FlushOwnedStructTemps()
    {
        if (pendingOwnedStructTemps.empty()) return;

        auto* curBlock = builder->GetInsertBlock();
        if (IsInsertBlockLive())
        {
            std::optional<llvm::DominatorTree> domTree;
            for (auto& t : pendingOwnedStructTemps)
            {
                if (t.Alloca == nullptr || !OwnedTempDominatesHere(t.Block, curBlock, domTree)) continue; // dominance safety
                if (auto* dtor = GetOrCreateFullDestructor(t.TypeName))
                    builder->CreateCall(dtor->getFunctionType(), dtor, { t.Alloca });
            }
        }
        pendingOwnedStructTemps.clear();
    }

    // Free all unnamed owned temporaries (string, closure, struct) at an end-of-full-expression
    // boundary. The return path keeps the three explicit (it interleaves Unregister between them).
    void FlushOwnedTemps()
    {
        FlushOwnedStringTemps();
        FlushOwnedClosureTemps();
        FlushOwnedStructTemps();
    }

    void EmitDestructorsForScope(const StackState& frame)
    {
        if (!IsInsertBlockLive())
            return;

        // Cleanup destructors have no user location. Pin a synthetic location to the function
        // line to avoid the -g verifier rejecting untagged inlinable calls after a branch/return.
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
                // String dtor is emitted unconditionally - the runtime OWNED bit (_len high bit) decides.
                // Legacy IsOwningString skipped genuinely-owned strings the compiler couldn't prove owned, leaking their buffer.
                if (namedVar.TypeAndValue.TypeName == "string")
                {
                    if (namedVar.BorrowsOwnedString || namedVar.IsAliasBorrow) continue;
                    EnsureStringDtorRegistered();
                    if (it->second.Destructor == nullptr) continue;
                    auto* fn = it->second.Destructor;
                    builder->CreateCall(fn->getFunctionType(), fn, { namedVar.Storage });
                    continue;
                }
                // Non-string struct local: run the full destructor (user dtor + members).
                // Skip an `alias`-bound local - it borrows storage it does not own (double-free).
                if (namedVar.IsAliasBorrow) continue;
                // Skip the struct value being moved out via `return` - the caller now owns it.
                if (namedVar.Storage == returnedStructDtorSkipAlloca) continue;
                if (auto* fn = GetOrCreateFullDestructor(namedVar.TypeAndValue.TypeName))
                    builder->CreateCall(fn->getFunctionType(), fn, { namedVar.Storage });
            }
        }

        // Clean up owning function parameters (move params)
        for (const auto& [varName, namedVar] : frame.functionArgument)
        {
            // A `move` interface fat-ptr param is owning, but its Storage is a {i8*,i8*} slot,
            // not a single pointer - so EmitOwningPtrCleanup must not run on it. Interface
            // ownership is released by an explicit `delete` (interface locals are likewise not
            // auto-destructed at scope exit); the move flag only authorizes that delete.
            if (namedVar.IsOwning && namedVar.Storage != nullptr
                && namedVar.TypeAndValue.IsInterface && !namedVar.TypeAndValue.IsInterfacePointer)
                continue;
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

    int MintAliasScope()
    {
        llvm::MDBuilder mdb(*context);
        if (aliasDomain_ == nullptr)
            aliasDomain_ = mdb.createAnonymousAliasScopeDomain("cflat.view");
        auto* scope = mdb.createAnonymousAliasScope(aliasDomain_, "cflat.view.scope");
        aliasScopes_.push_back(scope);
        return (int)aliasScopes_.size() - 1;
    }

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
        // Drop any debug location before emitting parameter allocas/stores: instructions tagged
        // with the outer DISubprogram trip the verifier (`!dbg attachment points at wrong subprogram`).
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
                    // A `move` interface param takes ownership: mark it owning so 'delete x' is
                    // permitted (not a borrow) and scope exit destructs it if not deleted.
                    .IsOwning = itr_nameArg->IsMove,
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

            // Forward-declare into cache before recursing into fields so that self-referential
            // or mutually-recursive structs resolve without infinite recursion.
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
        // Do NOT cache: a struct may be opaque now and become real later (e.g. generic before body layout).
        return diBuilder->createUnspecifiedType(tv.TypeName);
    }

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

        // string.data/length/hash are library functions in core/string.cb. Only the bare
        // {i8*,i32} layout + default ctor live here for bootstrap (literal typing, __FILE__).

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

        // Pre-register the closure fat type { i8* code, i8* env } (lambda Option A).
        // Dtor + copy registered lazily when function.cb env primitives are available.
        RegisterBuiltinClosure();
    }

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
        auto* i64Ty     = builder->getInt64Ty();
        auto* i32Ty     = builder->getInt32Ty();

        // A per-closure capture-cleanup fn (`void(i8* dst, i8* src, i32 mode)`) is stashed in the
        // env header at (captures - 8) by __closure_env_new, or null for a scalar-only closure.
        // Load it from a TAGGED, non-null owned env; returns null if env is null/borrowed/no-cleanup.
        auto* cleanupFnTy = llvm::FunctionType::get(voidTy, { i8PtrTy, i8PtrTy, i32Ty }, false);
        auto loadCleanup = [&](llvm::IRBuilder<>& b, llvm::Value* env) -> llvm::Value* {
            auto* envInt  = b.CreatePtrToInt(env, i64Ty);
            auto* capAddr = b.CreateAnd(envInt, b.getInt64(~(uint64_t)1));
            auto* slotAddr = b.CreateSub(capAddr, b.getInt64(8));
            auto* slotPtr = b.CreateIntToPtr(slotAddr, cleanupFnTy->getPointerTo()->getPointerTo());
            return b.CreateLoad(cleanupFnTy->getPointerTo(), slotPtr, "cleanupfn");
        };
        auto envIsOwned = [&](llvm::IRBuilder<>& b, llvm::Value* env) -> llvm::Value* {
            auto* envInt = b.CreatePtrToInt(env, i64Ty);
            return b.CreateICmpNE(b.CreateAnd(envInt, b.getInt64(1)), b.getInt64(0));
        };
        auto capPtrOf = [&](llvm::IRBuilder<>& b, llvm::Value* env) -> llvm::Value* {
            auto* envInt  = b.CreatePtrToInt(env, i64Ty);
            auto* capAddr = b.CreateAnd(envInt, b.getInt64(~(uint64_t)1));
            return b.CreateIntToPtr(capAddr, i8PtrTy);
        };

        // Destructor: destruct owning captures (via the header cleanup fn), free the heap env
        // (no-op if borrowed/null), and null the field.
        {
            auto* dtorTy = llvm::FunctionType::get(voidTy, { closureTy->getPointerTo() }, false);
            auto* dtorFn = llvm::Function::Create(dtorTy, llvm::Function::InternalLinkage,
                                                  "__closure_fat_ptr.dtor", *module);
            dtorFn->arg_begin()->setName("self");
            auto* entry = llvm::BasicBlock::Create(*context, "entry", dtorFn);
            auto* ownBB = llvm::BasicBlock::Create(*context, "owned", dtorFn);
            auto* callBB = llvm::BasicBlock::Create(*context, "callcleanup", dtorFn);
            auto* freeBB = llvm::BasicBlock::Create(*context, "freeenv", dtorFn);
            llvm::IRBuilder<> b(entry);
            auto* self   = &*dtorFn->arg_begin();
            auto* envPtr = b.CreateStructGEP(closureTy, self, 1, "envfield");
            auto* env    = b.CreateLoad(i8PtrTy, envPtr, "env");
            b.CreateCondBr(envIsOwned(b, env), ownBB, freeBB);

            b.SetInsertPoint(ownBB);
            auto* cleanup = loadCleanup(b, env);
            b.CreateCondBr(b.CreateICmpNE(cleanup,
                llvm::ConstantPointerNull::get(cleanupFnTy->getPointerTo())), callBB, freeBB);

            b.SetInsertPoint(callBB);
            b.CreateCall(cleanupFnTy, cleanup,
                { capPtrOf(b, env), llvm::ConstantPointerNull::get(i8PtrTy), b.getInt32(0) });
            b.CreateBr(freeBB);

            b.SetInsertPoint(freeBB);
            b.CreateCall(freeFn->getFunctionType(), freeFn, { env });
            b.CreateStore(llvm::ConstantPointerNull::get(i8PtrTy), envPtr);
            b.CreateRetVoid();
            RegisterDestructor("__closure_fat_ptr", dtorFn);
        }

        // Registered so HasCopyOverloadFor short-circuits memberwise synth, which would
        // shallow-copy the env pointer and double-free.
        {
            auto* copyTy = llvm::FunctionType::get(closureTy, { closureTy }, false);
            auto* copyFn = llvm::Function::Create(copyTy, llvm::Function::InternalLinkage,
                                                  "__closure_fat_ptr.copy", *module);
            copyFn->arg_begin()->setName("self");
            auto* entry  = llvm::BasicBlock::Create(*context, "entry", copyFn);
            auto* ownBB  = llvm::BasicBlock::Create(*context, "owned", copyFn);
            auto* callBB = llvm::BasicBlock::Create(*context, "callcleanup", copyFn);
            auto* doneBB = llvm::BasicBlock::Create(*context, "done", copyFn);
            llvm::IRBuilder<> b(entry);
            auto* self   = &*copyFn->arg_begin();
            auto* code   = b.CreateExtractValue(self, { 0u }, "code");
            auto* env    = b.CreateExtractValue(self, { 1u }, "env");
            auto* newEnv = b.CreateCall(cloneFn->getFunctionType(), cloneFn, { env }, "clonedenv");
            // Deep-copy owning captures into the fresh env (the byte-clone left them aliasing src).
            b.CreateCondBr(envIsOwned(b, newEnv), ownBB, doneBB);

            b.SetInsertPoint(ownBB);
            auto* cleanup = loadCleanup(b, newEnv);
            b.CreateCondBr(b.CreateICmpNE(cleanup,
                llvm::ConstantPointerNull::get(cleanupFnTy->getPointerTo())), callBB, doneBB);

            b.SetInsertPoint(callBB);
            b.CreateCall(cleanupFnTy, cleanup, { capPtrOf(b, newEnv), capPtrOf(b, env), b.getInt32(1) });
            b.CreateBr(doneBB);

            b.SetInsertPoint(doneBB);
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

    // Register a closure type used as a generic argument (e.g. list<Lambda<int(int)>>). `sig` is the
    // call descriptor: IsFunctionPointer + TypeName (__closure_fat_ptr | __c_fn_ptr) + FuncPtr*.
    // The encoded name is also made a real dataStructures value type so it stores, pointer-wraps,
    // and (fat) destructs like any owning element. Idempotent.
    void RegisterEncodedClosureType(const std::string& encodedName, const TypeAndValue& sig)
    {
        if (encodedClosureTypes_.count(encodedName)) return;
        encodedClosureTypes_[encodedName] = sig;
        EnsureClosureLifetimeRegistered();

        if (!sig.IsThinFnPtr())
        {
            // Fat owning closure: alias the closure fat struct + its dtor so the encoded name is an
            // owning value type (container frees envs; GetType resolves; pointer-wrap works).
            auto& base = dataStructures["__closure_fat_ptr"];
            auto& ds = dataStructures[encodedName];
            ds.StructType   = base.StructType;
            ds.StructFields = base.StructFields;
            ds.Destructor   = base.Destructor;
            // Bind clone-by-default to the closure copy so a copy request deep-clones the env rather
            // than falling back to the memberwise synth (which shallow-shares env -> double-free).
            if (auto* copyFn = module->getFunction("__closure_fat_ptr.copy"))
            {
                FunctionSymbol sym;
                sym.UniqueName = encodedName + ".copy";
                sym.Function   = copyFn;
                sym.ReturnType = TypeAndValue{ encodedName, "", false };
                sym.ReturnType.IsMove = true;
                sym.Parameters = { TypeAndValue{ encodedName, "self", false } };
                functionTable["copy"].push_back(sym);
            }
        }
        else
        {
            // Thin C fn ptr: an 8-byte POD. Represent as a { i8* } struct so it stores/copies like a
            // normal value-type element; no destructor.
            auto& ds = dataStructures[encodedName];
            if (!ds.StructType)
            {
                auto* i8PtrTy = builder->getInt8Ty()->getPointerTo();
                ds.StructType = llvm::StructType::create(*context, { i8PtrTy }, encodedName);
                DeclTypeAndValue fnField;
                fnField.TypeName    = "i8";
                fnField.VariableName = "fn";
                fnField.Pointer     = true;
                ds.StructFields = { fnField };
            }
        }
    }

    bool IsEncodedClosureType(const std::string& name) const
    {
        return encodedClosureTypes_.count(name) != 0;
    }
    const TypeAndValue* GetEncodedClosureType(const std::string& name) const
    {
        auto it = encodedClosureTypes_.find(name);
        return it == encodedClosureTypes_.end() ? nullptr : &it->second;
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
        // operator delete is from memory.cb which is imported before string.cb.
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
        // Free iff this string OWNS its buffer (_len high bit = OWNED flag).
        // Borrowed strings (literals, char* wraps) have bit clear - safe to call dtor on ANY string.
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

    llvm::Value* ClearStringOwnedBit(llvm::Value* value)
    {
        if (value == nullptr) return value;
        auto* strTy = llvm::StructType::getTypeByName(*context, "string");
        if (strTy == nullptr || value->getType() != strTy) return value;
        auto* len    = builder->CreateExtractValue(value, { 1 }, "borrow.len");
        auto* masked = builder->CreateAnd(len, builder->getInt32(0x7FFFFFFF), "borrow.len.noown");
        return builder->CreateInsertValue(value, masked, { 1 }, "borrow.str");
    }

    // Recursively clear the runtime OWNED bit on every owning value-type field of a struct SSA
    // value, turning the value into a BORROW. Used when an accessor returns a struct taken from a
    // collection it still owns (e.g. list.get's `alias T get` returning `_data[i]`): the caller
    // gets a shallow copy whose always-run full destructor would otherwise free buffers the
    // container still holds. The `alias` compile-time machinery suppresses the destructor at the
    // immediate binding site, but a copy that escapes that tracking (e.g. a for-in loop variable)
    // is still destructed - clearing the runtime bit makes that destructor a safe no-op, mirroring
    // what ClearStringOwnedBit already does for a scalar string borrow return. Pointer/view/simd/
    // bitfield/fixed-array fields carry no value-level owned bit and are left untouched (matches the
    // destructor and memberwise-copy field selection).
    llvm::Value* ClearStructOwnedBits(llvm::Value* value, const std::string& typeName)
    {
        if (value == nullptr || typeName.empty()) return value;
        auto dsIt = dataStructures.find(typeName);
        if (dsIt == dataStructures.end()) return value;
        auto* structTy = dsIt->second.StructType;
        if (structTy == nullptr || value->getType() != structTy) return value;

        for (unsigned i = 0; i < dsIt->second.StructFields.size(); ++i)
        {
            const auto& f = dsIt->second.StructFields[i];
            if (f.Pointer || f.ElemPointer || f.IsArrayView || f.IsSimd || f.IsBitfield)
                continue;
            if (f.ConstArraySize > 0)
                continue;
            if (f.TypeName == "string")
            {
                auto* len    = builder->CreateExtractValue(value, { i, 1u }, "fborrow.len");
                auto* masked = builder->CreateAnd(len, builder->getInt32(0x7FFFFFFF), "fborrow.noown");
                value = builder->CreateInsertValue(value, masked, { i, 1u }, "fborrow.str");
            }
            else if (IsOwningValueType(f.TypeName))
            {
                auto* sub = builder->CreateExtractValue(value, { i }, "fborrow.sub");
                sub = ClearStructOwnedBits(sub, f.TypeName);
                value = builder->CreateInsertValue(value, sub, { i }, "fborrow.subset");
            }
        }
        return value;
    }

    llvm::Function* GetOrCreateFullDestructor(const std::string& typeName)
    {
        // `string` full dtor is the lazily registered string dtor. Resolve live (never cache)
        // so a call before lazy registration doesn't poison the cache with null.
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

        // Only synthesized wrappers are cached - they are stable once built. Direct lookups
        // are resolved live so forward-declared types (e.g. generic ~list) aren't frozen as null.
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
            // `string` members ARE destructed: the owned bit in _len tells the dtor whether to free.
            // A borrowed string (literal/view; bit clear) is safely left alone by the dtor.
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

    // Resolve the destructor to call from a `delete` site. Differs from
    // GetOrCreateFullDestructor only when the eager resolve fails: a generic container
    // (e.g. `list<Stmt*>`) is monomorphized while its element type is still incomplete -
    // when Stmt itself has a `list<Stmt*>` field, registering Stmt forces the list
    // instantiation mid-registration, so `delete _data[i]` inside ~list resolves Stmt's
    // full dtor to null and the call is baked out permanently. To avoid that, bind the
    // delete to a stable forward-declared `<type>.dtordeferred` symbol whose body is
    // emitted at finalization (EmitDeferredFullDestructorBodies), once every type is
    // complete. The boolean callers of GetOrCreateFullDestructor are unaffected.
    llvm::Function* GetFullDestructorForDelete(const std::string& typeName)
    {
        if (llvm::Function* eager = GetOrCreateFullDestructor(typeName))
            return eager;
        // Eager resolve failed: trivially-destructible OR not-yet-complete. Only data
        // structures can need a (possibly recursive) destructor; anything else is null.
        if (dataStructures.find(typeName) == dataStructures.end())
            return nullptr;
        if (auto it = deferredFullDtor_.find(typeName); it != deferredFullDtor_.end())
            return it->second;
        auto* voidTy   = llvm::Type::getVoidTy(*context);
        auto* selfPtrTy = llvm::PointerType::get(*context, 0);
        auto* fnTy = llvm::FunctionType::get(voidTy, { selfPtrTy }, false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                          typeName + ".dtordeferred", *module);
        fn->arg_begin()->setName("self");
        deferredFullDtor_[typeName] = fn;
        deferredFullDtorOrder_.push_back(typeName);
        return fn;
    }

    // Emit bodies for every deferred destructor wrapper handed out by
    // GetFullDestructorForDelete. Called once at finalization, when all types (and their
    // monomorphizations) are complete, so the real full destructor now resolves. The
    // wrapper simply forwards to it (null-guarded by the delete call site); a type that
    // turns out trivially-destructible gets an empty body, so the bound call is a no-op.
    void EmitDeferredFullDestructorBodies()
    {
        for (const std::string& typeName : deferredFullDtorOrder_)
        {
            llvm::Function* fn = deferredFullDtor_[typeName];
            if (!fn->empty())
                continue;
            auto* entry = llvm::BasicBlock::Create(*context, "entry", fn);
            llvm::IRBuilder<> b(entry);
            auto* self = &*fn->arg_begin();
            if (llvm::Function* real = GetOrCreateFullDestructor(typeName))
                b.CreateCall(real->getFunctionType(), real, { self });
            b.CreateRetVoid();
        }
    }

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

    bool IsOwningValueType(const std::string& typeName)
    {
        if (typeName.empty())
            return false;
        if (dataStructures.find(typeName) == dataStructures.end())
            return false;
        return GetOrCreateFullDestructor(typeName) != nullptr;
    }

    bool HasCopyOverloadFor(const std::string& typeName) const
    {
        auto it = functionTable.find("copy");
        if (it == functionTable.end()) return false;
        for (const auto& sym : it->second)
            if (!sym.Parameters.empty() && sym.Parameters[0].TypeName == typeName)
                return true;
        return false;
    }

    // True when `typeName` has an AUTHOR-written (library/user) copy() - not the memberwise
    // synth generated on demand. The synth is registered under "<type>.copy.synth"; a real
    // copy is anything else. This is the deep-copy guarantee a container (list/dictionary)
    // gives via its own copy() vs. the shallow pointer-field byte copy the synth would do.
    bool HasRealCopyOverloadFor(const std::string& typeName) const
    {
        auto it = functionTable.find("copy");
        if (it == functionTable.end()) return false;
        const std::string synthName = typeName + ".copy.synth";
        for (const auto& sym : it->second)
            if (!sym.Parameters.empty() && sym.Parameters[0].TypeName == typeName
                && sym.UniqueName != synthName)
                return true;
        return false;
    }

    // True when an owning value type T can be safely DEEP-copied into a closure env by routing
    // through CreateOverloadedFunctionCall("copy", {T}) - i.e. capturing it by value behaves
    // exactly like `T x = src;`. Admitted when: T has a real copy() (list/dictionary/string/
    // hand-written user copy - authoritatively deep), OR T relies only on the memberwise synth
    // AND has no raw pointer/view field (the synth shallow-shares those while T's dtor frees
    // them -> double-free; such a type needs a hand-written copy() to be capturable).
    bool ClosureCaptureDeepCopyable(const std::string& typeName)
    {
        if (!IsOwningValueType(typeName)) return false;
        if (HasRealCopyOverloadFor(typeName)) return true;
        auto ds = dataStructures.find(typeName);
        if (ds == dataStructures.end()) return false;
        for (const auto& f : ds->second.StructFields)
            if (f.Pointer || f.ElemPointer || f.IsArrayView)
                return false;
        return true;
    }

    // True when `typeName` defines `operator->` (its implicit `this` is the sole parameter).
    // Drives the operator-> forward-on-miss path for both `.` and `->`.
    bool HasArrowOverloadFor(const std::string& typeName) const
    {
        auto it = functionTable.find("operator->");
        if (it == functionTable.end()) return false;
        for (const auto& sym : it->second)
            if (!sym.Parameters.empty() && sym.Parameters[0].TypeName == typeName)
                return true;
        return false;
    }

    // True when `typeName` has a directly-accessible member `memberName`: a field, a
    // bitfield, a member function (any overload whose first param is this type), or a
    // winrt vtable slot. Own members take priority over operator-> forwarding, so this
    // is the miss test that gates forwarding (own member shadows a forwarded one).
    bool TypeHasMember(const std::string& typeName, const std::string& memberName) const
    {
        if (auto ds = dataStructures.find(typeName); ds != dataStructures.end())
        {
            for (const auto& f : ds->second.StructFields)
                if (f.VariableName == memberName) return true;
            for (const auto& b : ds->second.Bitfields)
                if (b.Name == memberName) return true;
        }
        if (auto fn = functionTable.find(memberName); fn != functionTable.end())
            for (const auto& sym : fn->second)
                if (!sym.Parameters.empty() && sym.Parameters[0].TypeName == typeName)
                    return true;
        return GetWinrtSlot(typeName, memberName) != nullptr;
    }

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

    // Generate a per-closure capture-cleanup function `void(i8* dst, i8* src, i32 mode)` for a
    // capture block `capTy` whose owning-value fields are listed in `owningFields` (fieldIndex,
    // typeName). mode 1 (CLONE): deep-copy each owning field FROM src INTO dst, overwriting the
    // shallow byte-copy the env clone left. mode 0 (FREE): destruct each owning field in dst.
    // This is what makes a closure OWN its captured strings/containers (independent lifetime),
    // so a captured owning value survives after its source is destructed and is freed exactly
    // once. Returns null when there are no owning fields (the scalar-only fast path).
    llvm::Function* GenerateClosureCaptureCleanup(const std::string& name, llvm::StructType* capTy,
        const std::vector<std::pair<unsigned, std::string>>& owningFields)
    {
        if (owningFields.empty()) return nullptr;
        auto* i8PtrTy = builder->getInt8Ty()->getPointerTo();
        auto* i32Ty   = builder->getInt32Ty();
        auto* voidTy  = llvm::Type::getVoidTy(*context);
        auto* fnTy = llvm::FunctionType::get(voidTy, { i8PtrTy, i8PtrTy, i32Ty }, false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, name, module.get());

        auto savedIP  = builder->saveIP();
        auto* entry   = llvm::BasicBlock::Create(*context, "entry", fn);
        auto* cloneBB = llvm::BasicBlock::Create(*context, "clone", fn);
        auto* freeBB  = llvm::BasicBlock::Create(*context, "free", fn);
        builder->SetInsertPoint(entry);
        auto* dstCaps = builder->CreateBitCast(fn->getArg(0), capTy->getPointerTo(), "dstcaps");
        auto* isClone = builder->CreateICmpEQ(fn->getArg(2), builder->getInt32(1));
        builder->CreateCondBr(isClone, cloneBB, freeBB);

        // CLONE: dst currently aliases src's owning handles (byte copy); replace each with a
        // deep copy so dst owns independent buffers.
        builder->SetInsertPoint(cloneBB);
        auto* srcCaps = builder->CreateBitCast(fn->getArg(1), capTy->getPointerTo(), "srccaps");
        for (const auto& [idx, tn] : owningFields)
        {
            NamedVariable argNV;
            argNV.Storage  = builder->CreateStructGEP(capTy, srcCaps, idx);
            argNV.BaseType = capTy->getElementType(idx);
            argNV.TypeAndValue.TypeName = tn;
            if (auto* copied = CreateOverloadedFunctionCall("copy", { argNV }))
                builder->CreateStore(copied, builder->CreateStructGEP(capTy, dstCaps, idx));
        }
        builder->CreateRetVoid();

        // FREE: destruct dst's owning fields.
        builder->SetInsertPoint(freeBB);
        for (const auto& [idx, tn] : owningFields)
        {
            auto* fldPtr = builder->CreateStructGEP(capTy, dstCaps, idx);
            if (auto* dtor = GetOrCreateFullDestructor(tn))
                builder->CreateCall(dtor->getFunctionType(), dtor,
                    { builder->CreateBitCast(fldPtr, dtor->getArg(0)->getType()) });
        }
        builder->CreateRetVoid();
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

    // x86-64 System V va_list is `__va_list_tag[1]` (24 bytes: gp_offset i32,
    // fp_offset i32, overflow_arg_area ptr, reg_save_area ptr) passed by tag
    // pointer; Windows x64 va_list is just a pointer cursor. cflat carries `va_list`
    // as a `ptr` everywhere (GetType), which already matches a SysV tag *pointer*.
    // The only mismatch is what va_start fills: on SysV it must initialize a 24-byte
    // tag, not an 8-byte cursor slot. So on SysV we back the va_list slot with a real
    // tag buffer and point the slot at it; the slot's value (the tag pointer) then
    // forwards by value to libc vsnprintf/vsscanf unchanged. cflat never does va_arg
    // itself, so no SysV va_arg sequence is needed.
    llvm::StructType* VaListTagType()
    {
        auto* i32Ty = llvm::Type::getInt32Ty(*context);
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        return llvm::StructType::get(*context, { i32Ty, i32Ty, ptrTy, ptrTy });
    }

    void CreateVaStart(llvm::Value* apAlloca)
    {
        auto* fn = llvm::Intrinsic::getDeclaration(module.get(), llvm::Intrinsic::vastart);
        // macOS arm64 (Darwin): va_list is a plain char* (Apple passes every variadic
        // argument on the stack), so the slot IS the va_list - init it directly, exactly
        // like the Windows path. NOT the x86-64 SysV __va_list_tag indirection below.
        if (!targetWindows_ && !targetMacOS_)
        {
            // x86-64 SysV: allocate the 24-byte tag, point the va_list slot at it, init the tag.
            auto* tag = AllocaAtEntry(VaListTagType(), nullptr, "va.tag", 16);
            builder->CreateStore(tag, apAlloca);
            builder->CreateCall(fn, {tag});
            return;
        }
        builder->CreateCall(fn, {apAlloca});
    }

    void CreateVaEnd(llvm::Value* apAlloca)
    {
        auto* fn = llvm::Intrinsic::getDeclaration(module.get(), llvm::Intrinsic::vaend);
        if (!targetWindows_ && !targetMacOS_)
        {
            // x86-64 SysV: the slot holds the tag pointer (set by CreateVaStart); va_end takes the tag.
            auto* tag = builder->CreateLoad(llvm::PointerType::getUnqual(*context), apAlloca, "va.tag");
            builder->CreateCall(fn, {tag});
            return;
        }
        builder->CreateCall(fn, {apAlloca});
    }

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

    llvm::Value* CreateRdtscp()
    {
        // llvm.x86.rdtscp returns { i64 cycles, i32 aux } and takes no arguments.
        auto* fn   = llvm::Intrinsic::getDeclaration(module.get(), llvm::Intrinsic::x86_rdtscp);
        auto* call = builder->CreateCall(fn, {});
        return builder->CreateExtractValue(call, { 0u }, "rdtscp");
    }

    llvm::Value* CreateReadCycleCounter()
    {
        // llvm.readcyclecounter is target-independent: it returns an i64 cycle
        // count and takes no arguments. Lowers to RDTSC on x86, the cycle-count
        // register elsewhere (e.g. mftb/CNTVCT), or 0 where unsupported.
        auto* fn = llvm::Intrinsic::getDeclaration(module.get(), llvm::Intrinsic::readcyclecounter);
        return builder->CreateCall(fn, {}, "cyclecount");
    }

    void CreateLfence()
    {
        // llvm.x86.sse2.lfence returns void and takes no arguments.
        auto* fn = llvm::Intrinsic::getDeclaration(module.get(), llvm::Intrinsic::x86_sse2_lfence);
        builder->CreateCall(fn, {});
    }

    void CreatePause()
    {
        // llvm.x86.sse2.pause returns void and takes no arguments.
        auto* fn = llvm::Intrinsic::getDeclaration(module.get(), llvm::Intrinsic::x86_sse2_pause);
        builder->CreateCall(fn, {});
    }

    llvm::Value* CreatePopcount(llvm::Value* intVal)
    {
        auto* fn = llvm::Intrinsic::getDeclaration(module.get(), llvm::Intrinsic::ctpop, { intVal->getType() });
        return builder->CreateCall(fn, { intVal }, "popcount");
    }

    llvm::Value* CreateCtz(llvm::Value* intVal)
    {
        auto* fn = llvm::Intrinsic::getDeclaration(module.get(), llvm::Intrinsic::cttz, { intVal->getType() });
        return builder->CreateCall(fn, { intVal, llvm::ConstantInt::getFalse(*context) }, "ctz");
    }

    llvm::Value* CreateClz(llvm::Value* intVal)
    {
        auto* fn = llvm::Intrinsic::getDeclaration(module.get(), llvm::Intrinsic::ctlz, { intVal->getType() });
        return builder->CreateCall(fn, { intVal, llvm::ConstantInt::getFalse(*context) }, "clz");
    }

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

    llvm::Value* CreateFma(llvm::Value* a, llvm::Value* b, llvm::Value* c)
    {
        auto* fn = llvm::Intrinsic::getDeclaration(module.get(), llvm::Intrinsic::fma, { a->getType() });
        return builder->CreateCall(fn, { a, b, c }, "fma");
    }

    llvm::Value* CreateExpect(llvm::Value* cond, bool expected)
    {
        auto* fn = llvm::Intrinsic::getDeclaration(module.get(), llvm::Intrinsic::expect, { cond->getType() });
        return builder->CreateCall(fn, { cond, llvm::ConstantInt::get(cond->getType(), expected ? 1 : 0) }, "expect");
    }

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
            std::cout << std::format("Module verification failed:\n{}\n", errorStream.str());
            return false;
        }
        return true;
    }

    // Build a TargetLibraryInfoImpl for `triple` with cflat's reimplemented C stdio format
    // family (vsnprintf/vfprintf/vsscanf/vfscanf) marked unavailable on non-Windows targets.
    // Without this, instcombine (opt pipeline) and the codegen libcall simplifier fortify-fold
    // our __vsnprintf_chk / __vfprintf_chk libc calls back into plain vsnprintf / vfprintf, which
    // on ELF bind to cflat's OWN reimplementations and recurse forever. Windows keeps the real
    // libc names, so the unavailable marks only fire off-Windows.
    llvm::TargetLibraryInfoImpl MakeStdioSafeTLII(const llvm::Triple& triple) const
    {
        llvm::TargetLibraryInfoImpl tlii{ triple };
        if (!targetWindows_)
        {
            tlii.setUnavailable(llvm::LibFunc_vsnprintf);
            tlii.setUnavailable(llvm::LibFunc_vfprintf);
            tlii.setUnavailable(llvm::LibFunc_vsscanf);
            tlii.setUnavailable(llvm::LibFunc_vfscanf);
        }
        return tlii;
    }

    void RunModulePasses(llvm::ModulePassManager& MPM);
    void RunBaselinePasses();
    void RunGlobalDCE();
    void OptimizeModule(int optimizationLevel);

    bool SaveToFile(const std::string& filename)
    {
        std::error_code errorCode;
        llvm::raw_fd_ostream outLL(filename, errorCode);
        if (errorCode)
        {
            std::cout << std::format("Error: could not write IR to '{}': {}\n", filename, errorCode.message());
            return false;
        }
        module->print(outLL, nullptr);
        return true;
    }

    bool WriteBitcode(const std::string& filename)
    {
        std::error_code errorCode;
        llvm::raw_fd_ostream outBC(filename, errorCode, llvm::sys::fs::OF_None);
        if (errorCode)
        {
            std::cout << std::format("Error: could not write bitcode to '{}': {}\n", filename, errorCode.message());
            return false;
        }
        llvm::WriteBitcodeToFile(*module, outBC);
        return true;
    }

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

#if defined(__APPLE__)
    // Bundled Mach-O linker (deployed next to cflat by the build), else PATH.
    // "" if absent, in which case the link falls back to the host clang driver.
    std::string FindBundledLd64Lld() const
    {
        if (!runtimeDir.empty())
        {
            llvm::SmallString<256> cand(runtimeDir);
            llvm::sys::path::append(cand, "ld64.lld");
            if (llvm::sys::fs::exists(cand)) return cand.str().str();
        }
        if (auto p = llvm::sys::findProgramByName("ld64.lld")) return *p;
        return "";
    }
#endif

    // GCC-style C compiler driver for the ELF (non-Windows) target. Prefers clang to
    // match the LLVM the rest of the pipeline links, then falls back to cc/gcc.
    std::string FindCDriver() const
    {
        for (const char* cand : { "clang", "clang-18", "cc", "gcc" })
            if (auto p = llvm::sys::findProgramByName(cand)) return *p;
        return "";
    }

    // Compile a .c input to an ELF object with a GCC-style driver and queue it for the
    // ELF link. Mirrors CompileCFile's MSVC path but with POSIX flags (-c/-o/-D/-fPIC).
    bool CompileCFileElf(const std::string& cSourcePath, const std::string& programAlias)
    {
        const std::string cc = FindCDriver();
        if (cc.empty())
        {
            LogError(std::format("no C compiler driver (clang/cc/gcc) found - cannot compile C source '{}'.", cSourcePath));
            return false;
        }

        llvm::SmallString<256> objFile;
        if (auto ec = llvm::sys::fs::createTemporaryFile("cflat_c", "o", objFile))
        {
            LogError(std::format("could not create temp object for C source '{}': {}", cSourcePath, ec.message()));
            return false;
        }
        std::string objPath = objFile.str().str();

        // -fPIC so the object links into the position-independent image the ELF path emits.
        std::vector<std::string> argStrs = { cc, "-c", "-fPIC", cSourcePath, "-o", objPath };
        if (cOptLevel_ >= 2)      argStrs.push_back("-O2");
        else if (cOptLevel_ == 1) argStrs.push_back("-O1");
        if (cDebugInfo_)          argStrs.push_back("-g");
        if (!targetCpu_.empty()) argStrs.push_back("-march=" + targetCpu_);
        if (!tuneCpu_.empty())   argStrs.push_back("-mtune=" + tuneCpu_);
        for (const auto& def : cDefines_) argStrs.push_back("-D" + def);
        if (!programAlias.empty())
            argStrs.push_back("-Dmain=__imported_main_" + programAlias);

        std::vector<llvm::StringRef> args;
        for (auto& s : argStrs) args.push_back(s);

        if (verbose)
        {
            std::cout << std::format("[verbose] compiling C source: {} -> {}\n", cSourcePath, objPath);
            std::cout << "[verbose]   cc";
            for (size_t i = 1; i < argStrs.size(); ++i) std::cout << " " << argStrs[i];
            std::cout << "\n";
        }

        std::string compileErr;
        int rc;
        {
            llvm::TimeTraceScope spawnScope("ClangCompileC", cSourcePath);
            rc = llvm::sys::ExecuteAndWait(cc, args, std::nullopt, {}, 0, 0, &compileErr);
        }
        if (rc != 0)
        {
            llvm::sys::fs::remove(objPath);
            LogError(std::format("C compiler failed to compile C source '{}' (exit {}){}{}",
                cSourcePath, rc, compileErr.empty() ? "" : ": ", compileErr));
            return false;
        }

        cObjectFiles_.push_back(objPath);
        return true;
    }

    static std::string GetCflatCacheDir();
    // Records the running cflat.exe's full path to %USERPROFILE%\.cflat\compiler_path.txt
    // so the VS Code extension can auto-detect the compiler without manual configuration.
    static bool WriteCompilerPathToCache();
    static LinkerPaths DiscoverLinkerPaths(const std::string& arch, const std::string& runtimeDir, bool verbose = false);
    static std::optional<LinkerPaths> LoadLinkerPathsFromCache(const std::string& arch);
    static bool SaveLinkerPathsToCache(const std::string& arch, const LinkerPaths& paths);
    static LinkerPaths FindLinkerPaths(const std::string& arch, const std::string& runtimeDir, bool verbose = false);

    // SDK-free system import libs synthesized from the OS-resident DLLs (Phase B/C).
    static std::string GetSyntheticLibDir(const std::string& arch);
    static bool SynthesizeSystemImportLibs(const std::string& arch, const std::string& lldLink);
    static bool SynthesizeX86SystemImportLibs(const LinkerPaths& paths);

    bool CompileCFile(const std::string& cSourcePath, const std::string& programAlias = "")
    {
        // Auto-discover C function signatures so the importing .cb needs no hand-written extern declarations.
        // When programAlias is set, registers C `main` as `__imported_main_<Alias>` in programTable.
        ExtractCSignatures(cSourcePath, programAlias);

        if (symbolSink_ != nullptr)
            return true;

        // Non-Windows targets compile to an ELF object with a GCC-style driver and link
        // via EmitExecutableElf; clang-cl + MSVC flags only apply to the COFF path.
        if (!targetWindows_)
            return CompileCFileElf(cSourcePath, programAlias);

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

        // /MD (dynamic UCRT) so this object's CRT /defaultlib directives are the dynamic set,
        // not clang-cl's /MT default (libcmt) which the freestanding link cannot satisfy. Covers
        // both user .c interop and the imported diagnostic/heap_audit.c.
        std::vector<std::string> argStrs = { clangPath, "/c", "/MD", "/nologo", target, cSourcePath, foArg };
        // cflat's own bundled runtime .c files (e.g. diagnostic/heap_audit.c) are compiled
        // freestanding like crashdump.c/cflat_builtins.c: /GS- so they emit no __security_check_
        // cookie reference (that symbol lives in msvcrt.lib, which the freestanding link drops).
        // User .c interop keeps default /GS - its hardening is the user's call.
        if (!runtimeDir.empty())
        {
            std::error_code pec;
            auto canonSrc  = std::filesystem::weakly_canonical(cSourcePath, pec);
            auto canonCore = std::filesystem::weakly_canonical(std::filesystem::path(runtimeDir) / "core", pec);
            if (!pec)
            {
                std::string s = canonSrc.string(), c = canonCore.string();
                if (s.size() >= c.size() && _strnicmp(s.c_str(), c.c_str(), c.size()) == 0)
                    argStrs.push_back("/GS-");
            }
        }
        if (cOptLevel_ >= 2)      argStrs.push_back("/O2");
        else if (cOptLevel_ == 1) argStrs.push_back("/O1");
        if (cDebugInfo_)          argStrs.push_back("/Z7"); // CodeView in the obj -> PDB via /DEBUG
        if (!targetCpu_.empty()) argStrs.push_back("/clang:-march=" + targetCpu_);
        if (!tuneCpu_.empty())   argStrs.push_back("/clang:-mtune=" + tuneCpu_);
        for (const auto& def : cDefines_) argStrs.push_back("/D" + def);
        if (!programAlias.empty())
            argStrs.push_back("/Dmain=__imported_main_" + programAlias);

        std::vector<llvm::StringRef> args;
        for (auto& s : argStrs) args.push_back(s);

        if (verbose)
        {
            std::cout << std::format("[verbose] compiling C source: {} -> {}\n", cSourcePath, objPath);
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

    // NOT routed through CompileCFile: that path runs ExtractCSignatures and registers the
    // functions as CFlat externs, which we don't want for an internal handler.
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
        // /MD selects the dynamic-CRT /defaultlib directives (suppressed at link time) instead
        // of clang-cl's /MT default (libcmt), which the freestanding link cannot satisfy. /GS-
        // so the buffers here emit no __security_check_cookie reference: that symbol comes from
        // msvcrt.lib, which the freestanding (non-asan) link drops. See Phase A.
        std::vector<std::string> argStrs = {
            clangPath, "/c", "/Z7", "/MD", "/GS-", "/nologo", target, srcPath.str().str(), foArg
        };

        std::vector<llvm::StringRef> args;
        for (auto& s : argStrs) args.push_back(s);

        if (verbose)
        {
            std::cout << std::format("[verbose] compiling crash handler: {} -> {}\n", srcPath.str().str(), objPath);
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

    // Compile core/cflat_builtins.c (freestanding memcpy/memset/memcmp) so those
    // compiler-emitted calls resolve locally instead of importing from VCRUNTIME140.dll.
    // Must pass -fno-builtin or LLVM's loop-idiom pass rewrites the byte loops back into
    // memcpy/memset calls, recursing into these very definitions. Returns false (and logs)
    // on any failure; the caller treats it as best-effort.
    bool CompileBuiltinsObject(const std::string& arch)
    {
        if (runtimeDir.empty())
        {
            LogError("cannot locate cflat_builtins.c: runtime directory is unset.");
            return false;
        }

        llvm::SmallString<256> srcPath(runtimeDir);
        llvm::sys::path::append(srcPath, "core", "cflat_builtins.c");
        if (!llvm::sys::fs::exists(srcPath))
        {
            LogError(std::format("builtins source not found: '{}'.", srcPath.str().str()));
            return false;
        }

        const std::string clangPath = FindClangCl();
        if (clangPath.empty())
        {
            LogError("clang-cl.exe not found - cannot compile cflat_builtins.c.");
            return false;
        }

        llvm::SmallString<256> objFile;
        if (auto ec = llvm::sys::fs::createTemporaryFile("cflat_builtins", "obj", objFile))
        {
            LogError(std::format("could not create temp object for builtins: {}", ec.message()));
            return false;
        }
        std::string objPath = objFile.str().str();

        const std::string target = (arch == "x86")
            ? "--target=i686-pc-windows-msvc"
            : "--target=x86_64-pc-windows-msvc";
        const std::string foArg = "/Fo" + objPath;

        // /MD so the object's CRT /defaultlib directives are the dynamic set (msvcrt/vcruntime/
        // oldnames - all suppressed at link time) rather than clang-cl's /MT default (libcmt),
        // which the freestanding link cannot satisfy. /GS- so cflat_start and friends emit no
        // __security_check_cookie reference; that symbol lives in msvcrt.lib, which this object's
        // whole point is to let us drop.
        std::vector<std::string> argStrs = {
            clangPath, "/c", "/O2", "/MD", "/GS-", "/nologo", "/clang:-fno-builtin",
            target, srcPath.str().str(), foArg
        };

        std::vector<llvm::StringRef> args;
        for (auto& s : argStrs) args.push_back(s);

        if (verbose)
        {
            std::cout << std::format("[verbose] compiling builtins: {} -> {}\n", srcPath.str().str(), objPath);
            std::cout << "[verbose]   clang-cl";
            for (size_t i = 1; i < argStrs.size(); ++i) std::cout << " " << argStrs[i];
            std::cout << "\n";
        }

        std::string clangCompileErr;
        int rc = llvm::sys::ExecuteAndWait(clangPath, args, std::nullopt, {}, 0, 0, &clangCompileErr);
        if (rc != 0)
        {
            llvm::sys::fs::remove(objPath);
            LogError(std::format("clang-cl failed to compile cflat_builtins.c (exit {}){}{}",
                rc, clangCompileErr.empty() ? "" : ": ", clangCompileErr));
            return false;
        }

        cObjectFiles_.push_back(objPath);
        return true;
    }

    // True if the Microsoft Visual C++ runtime (vcruntime140.dll) is present on this system.
    // Used only to phrase a helpful note for --asan builds, which are the one output kind that
    // still depends on it. Checks both System32 and SysWOW64 so 32-bit and 64-bit installs both
    // count; if SystemRoot is somehow unknown we assume installed rather than nag.
    static bool VcRuntimeInstalled()
    {
        char buf[260] = {};
        size_t len = 0;
        if (getenv_s(&len, buf, sizeof(buf), "SystemRoot") != 0 || len == 0)
            return true;
        std::filesystem::path root(buf);
        for (const char* sub : { "System32", "SysWOW64" })
            if (std::filesystem::exists(root / sub / "vcruntime140.dll"))
                return true;
        return false;
    }

    // Map a (preferably desugared) C type spelling onto a CFlat TypeAndValue.


    bool MapCTypeToTypeAndValue(std::string ctype, TypeAndValue& out)
    {
        std::unordered_set<std::string> visited;
        return MapCTypeToTypeAndValueImpl(std::move(ctype), out, visited);
    }

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
        out.TypeName = "__c_fn_ptr";       // thin: a C function pointer is the thin `function<T>`
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

    // For a pointer-to-aggregate spelling like "const struct Foo *", return the tag ("Foo")
    // and the indirection level via outPtr. Returns "" (and leaves outPtr at 0) when the
    // spelling is not a struct/union pointer. Used to keep `Foo*` record fields typed instead
    // of decaying to void* - so COM `lpVtbl` member access resolves.
    static std::string AggregatePointeeTag(const std::string& spelling, int& outPtr)
    {
        outPtr = 0;
        std::string s = spelling;
        for (const char* w : { "const", "volatile", "restrict", "__restrict", "__restrict__",
                               "_Nonnull", "_Nullable", "_Null_unspecified" })
            for (size_t pos; (pos = s.find(w)) != std::string::npos; ) s.erase(pos, std::strlen(w));
        outPtr = (int)std::count(s.begin(), s.end(), '*');
        if (outPtr == 0) return std::string();
        s.erase(std::remove(s.begin(), s.end(), '*'), s.end());
        size_t a = s.find_first_not_of(" \t");
        size_t b = s.find_last_not_of(" \t");
        if (a == std::string::npos) return std::string();
        s = s.substr(a, b - a + 1);
        if (s.rfind("struct ", 0) == 0) return s.substr(7);
        if (s.rfind("union ", 0) == 0)  return s.substr(6);
        return std::string();
    }

    bool MapCTypeToTypeAndValueImpl(std::string ctype, TypeAndValue& out,
                                    std::unordered_set<std::string>& visited)
    {
        // Detect function-pointer spelling before the '*'-strip path mangles it.
        // clang spells these as "R (*)(args)"; the "(*)" token disambiguates from a declaration.
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

        // 3+ levels of indirection collapse to void** - CFlat TypeAndValue has at most two pointer
        // levels. Pointers are the same ABI size on x64/x86, so calls still link correctly.
        if (ptr > 2)
        {
            out.TypeName = "void";
            out.Pointer = true;
            out.ElemPointer = true;
            return true;
        }

        // enum decays to int. struct/union by-value: look up in dataStructures for ABI lowering.
        // struct/union pointers become opaque void* (only a pointer-sized slot is needed).
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
                // Chase a user typedef from AdoptRawTypedefs; append pointer stars so HANDLE (ptr=0)->void* works.
                // visited-set prevents pathological self-referential typedefs from looping.
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
                                      CallingConv::Cdecl);

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
                // Prefer the declaration's own header (presumed loc) so go-to-definition
                // lands on the real prototype, not the umbrella header that was imported.
                const std::string& declFile = e.file.empty() ? fileForLsp : e.file;
                s->Register(SymbolKind::Function, e.name, declFile, e.line, e.col < 0 ? 0 : e.col, sig);
            }
        }

        if (verbose)
            std::cout << std::format("[verbose]   registered {} C function(s) from {}\n", sigs.size(), fileForLsp);
    }

    std::vector<std::string> BuildClangDriverArgs(const std::string& headerDir,
                                               const std::vector<std::string>& extraDefines,
                                               bool errorRecovery, bool asCxx = false) const
    {
        std::vector<std::string> args;
        // Parse the C against the target OS so predefined macros (_WIN32 vs __linux__)
        // and the system header search behave like the real compile would. On non-Windows
        // the MSVC triple would pull in MSVC predefines and break libc header resolution.
        if (targetWindows_)
            args.push_back(platformValue == 32 ? "--target=i686-pc-windows-msvc"
                                               : "--target=x86_64-pc-windows-msvc");
        else if (targetMacOS_)
        {
            // Match the codegen triple from EmitExecutableMachO so __APPLE__ and the Apple
            // system-header search are active. Headers need a REAL SDK (the harvested
            // ~/.cflat/macsdk carries link stubs only), so isysroot points at $SDKROOT/xcrun.
            args.push_back("--target=arm64-apple-macosx11.0.0");
            std::string sdk;
#if defined(__APPLE__)
            sdk = MacSdkPathCached();
#else
            if (const char* env = std::getenv("SDKROOT")) if (env[0]) sdk = env;
#endif
            if (!sdk.empty())
            {
                args.push_back("-isysroot");
                args.push_back(sdk);
            }
            else if (symbolSink_ == nullptr)
            {
                // Do not fall through to a Linux triple - Apple headers would misparse.
                // In LSP/analyze mode (symbolSink_ set) degrade silently like other binds.
                LogError("C header import targeting macOS requires an SDK: set $SDKROOT or install "
                         "Xcode / Command Line Tools so 'xcrun --show-sdk-path' resolves");
            }
        }
        else
            args.push_back(platformValue == 32 ? "--target=i686-pc-linux-gnu"
                                               : "--target=x86_64-pc-linux-gnu");
        args.push_back("-fsyntax-only");
        args.push_back("-x");
        // C++ is used only by the focused uuid-harvest pass, so the SDK's MIDL_INTERFACE form
        // (which carries the __declspec(uuid) the C form omits) is parsed. The normal bind is C.
        args.push_back(asCxx ? "c++" : "c");
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

    bool MapRawSig(const cflat_cinterop::RawSig& r, CSigEntry& e)
    {
        e = CSigEntry();
        e.name     = r.name;
        e.variadic = r.variadic;
        e.file     = r.file;
        e.line     = r.line ? r.line : 1;
        e.col      = r.col < 0 ? 0 : r.col;
        if (!MapCTypeToTypeAndValue(r.retType, e.ret))
        {
            if (verbose) std::cout << std::format("[verbose]   skipping '{}': unsupported return type '{}'\n", r.name, r.retType);
            return false;
        }
        for (size_t i = 0; i < r.paramTypes.size(); ++i)
        {
            TypeAndValue ptv;
            if (!MapCTypeToTypeAndValue(r.paramTypes[i], ptv))
            {
                if (verbose) std::cout << std::format("[verbose]   skipping '{}': unsupported parameter type '{}'\n", r.name, r.paramTypes[i]);
                return false;
            }
            if (i < r.paramNames.size()) ptv.VariableName = r.paramNames[i];
            e.params.push_back(std::move(ptv));
        }
        return true;
    }

    bool MapRawGlobal(const cflat_cinterop::RawGlobalVar& r, CGlobalEntry& e)
    {
        if (r.ctype.find('[') != std::string::npos)
        {
            if (verbose) std::cout << std::format("[verbose]   skipping global '{}': array type '{}' is not bindable\n", r.name, r.ctype);
            return false;
        }
        e = CGlobalEntry();
        e.name = r.name;
        e.line = r.line ? r.line : 1;
        e.col  = r.col < 0 ? 0 : r.col;
        if (!MapCTypeToTypeAndValue(r.ctype, e.type))
        {
            if (verbose) std::cout << std::format("[verbose]   skipping global '{}': unsupported type '{}'\n", r.name, r.ctype);
            return false;
        }
        e.type.VariableName = r.name;
        return true;
    }

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

    void AdoptRawTypedefs(const cflat_cinterop::ExtractResult& raw)
    {
        for (const auto& t : raw.typedefs)
            if (!t.name.empty() && !t.underlying.empty() && t.underlying != t.name)
                cTypedefMap_.emplace(t.name, t.underlying);
    }

    // Surface a C typedef that names a record as a CFlat type alias. Two shapes:
    //   by value  `typedef struct tagMSG MSG;`            -> MSG            = tagMSG
    //   handle    `typedef struct CGColorSpace *CGColorSpaceRef;` -> CGColorSpaceRef = CGColorSpace*
    // The handle form is the C opaque-pointer idiom: the tag is often only forward-declared, which
    // RegisterCRecords registers as an opaque shell, so the alias binds even with no struct body.
    // Requiring the tag to be a registered record keeps both shapes confined to the bound header's
    // own types. Trailing stars stay in the target string - GetType peels them into pointer depth.
    void CollectRecordTypedefAliases(const cflat_cinterop::ExtractResult& raw,
                                     std::vector<std::pair<std::string, std::string>>& out)
    {
        static const std::unordered_set<std::string> qualifiers = {
            "const", "volatile", "restrict", "__restrict", "__restrict__",
            "_Nonnull", "_Nullable", "_Null_unspecified", "struct", "union" };
        for (const auto& t : raw.typedefs)
        {
            if (t.name.empty() || t.underlying.empty() || t.name == t.underlying) continue;
            if (t.underlying.find('(') != std::string::npos) continue;   // function pointer typedef
            if (t.underlying.find('[') != std::string::npos) continue;   // array typedef

            // Split off the pointer depth, then tokenize what remains: the tag is the one word
            // left after dropping cv/nullability qualifiers and the struct/union keyword.
            std::string spelling = t.underlying;
            int ptr = (int)std::count(spelling.begin(), spelling.end(), '*');
            if (ptr > 2) continue;   // TypeAndValue carries at most two pointer levels
            std::replace(spelling.begin(), spelling.end(), '*', ' ');

            std::string tag;
            bool ambiguous = false;
            std::istringstream words(spelling);
            for (std::string w; words >> w; )
            {
                if (qualifiers.count(w)) continue;
                if (!tag.empty()) { ambiguous = true; break; }   // compound spelling we do not model
                tag = w;
            }
            if (ambiguous || tag.empty() || tag == t.name) continue;
            if (dataStructures.find(tag) == dataStructures.end()) continue;  // not a registered record
            out.emplace_back(t.name, tag + std::string(ptr, '*'));
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

            // Surface the typedef name itself as a navigable LSP symbol. Type resolution already
            // follows the alias, but the symbol index only knew the underlying tag, so --symbol /
            // hover / go-to-def on the alias name (e.g. ID3DBlob -> ID3D10Blob) found nothing.
            // Inherit the target struct's location (registered just before us by RegisterCRecords)
            // so go-to-def jumps to the aliased definition.
            if (auto* s = GetSymbolSink())
            {
                std::string file;
                int line = 0, col = 0;
                // A handle alias carries pointer stars (CGColorSpaceRef -> CGColorSpace*); the
                // symbol index is keyed on the bare tag, so peel them before looking it up.
                std::string targetTag = target;
                while (!targetTag.empty() && targetTag.back() == '*') targetTag.pop_back();
                if (const SymbolDef* td = s->Lookup(targetTag))
                {
                    file = td->file;
                    line = td->line;
                    col = td->column;
                }
                s->Register(SymbolKind::TypeAlias, alias, file, line, col,
                            "typedef " + target + " " + alias);
            }
        }
    }

    // Keep the transitive closure of in-scope records over their by-value field deps, plus any
    // record referenced BY VALUE from an in-scope function signature or global variable (e.g.
    // CGRect/CGPoint: defined in a sibling out-of-scope header, but named by CGRectGetMinX's
    // param/return types). raw.sigs/enums/globals are already scope-filtered at extraction time
    // (LocOf in CClangExtract.cpp drops out-of-scope decls), so seeding from them cannot pull in
    // unrelated system structs - unlike records, which are collected regardless of scope.
    void PruneRecordsToNeededClosure(cflat_cinterop::ExtractResult& raw)
    {
        std::vector<cflat_cinterop::RawRecord>& records = raw.records;

        // Last definition wins on a duplicate tag (forward decls are not definitions, so this is
        // rare); the index just needs to resolve a referenced tag to some record we can keep.
        std::unordered_map<std::string, size_t> byName;
        for (size_t i = 0; i < records.size(); ++i)
            if (!records[i].name.empty()) byName[records[i].name] = i;

        // Extract the by-value dependency tag from a type spelling (field, param, return, or
        // global var). Pointer types are pointer-sized regardless of pointee registration, so skip them.
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

        auto seed = [&](const std::string& ctype) {
            std::string dep = byValueDep(ctype);
            if (dep.empty()) return;
            auto it = byName.find(dep);
            if (it == byName.end() || needed[it->second]) return;
            needed[it->second] = true;
            work.push_back(it->second);
        };
        for (const auto& sig : raw.sigs)
        {
            seed(sig.retType);
            for (const auto& pt : sig.paramTypes) seed(pt);
        }
        for (const auto& g : raw.globals) seed(g.ctype);

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

    void MapRawRecords(const cflat_cinterop::ExtractResult& raw, std::vector<CRecordEntry>& out)
    {
        for (const auto& r : raw.records)
        {
            CRecordEntry rec;
            rec.name = r.name; rec.isUnion = r.isUnion;
            rec.line = r.line ? r.line : 1; rec.col = r.col < 0 ? 0 : r.col;
            rec.uuid = r.uuid;
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

    // A header-COM interface record is a struct whose lpVtbl field points at its <Iface>Vtbl.
    // Used to gate the (relatively costly) C++ uuid-harvest parse to headers that define COM.
    static bool HasComRecord(const std::vector<cflat_cinterop::RawRecord>& records)
    {
        for (const auto& r : records)
            for (const auto& f : r.fields)
                if (f.name == "lpVtbl") return true;
        return false;
    }

    // Parse the same header(s) once more as C++ to read each COM interface's __declspec(uuid) GUID
    // (the MIDL_INTERFACE form), then stamp it onto the matching C-parse record by name. Failure is
    // non-fatal: records simply keep an empty uuid and iidof() falls back to its "no IID" error.
    void HarvestComUuids(const std::vector<std::string>& headerPaths, const std::string& primaryDir,
                         const std::vector<std::string>& extraDefines,
                         std::vector<cflat_cinterop::RawRecord>& records)
    {
        std::string source;
        for (const auto& h : headerPaths)
        {
            std::string fwd = h;
            std::replace(fwd.begin(), fwd.end(), '\\', '/');
            source += "#include \"" + fwd + "\"\n";
        }

        cflat_cinterop::ExtractRequest req;
        req.mainFileName      = "cflat_uuid_stub.cpp";
        req.source            = source;
        req.args              = BuildClangDriverArgs(primaryDir, extraDefines, /*errorRecovery*/ true, /*asCxx*/ true);
        req.uuidHarvestCxx    = true;
        req.skipFunctionBodies = true;

        cflat_cinterop::ExtractResult uuidRaw;
        std::string err;
        if (!cflat_cinterop::ExtractCInterop(req, uuidRaw, err))
        {
            if (verbose) std::cout << std::format("[verbose]   COM uuid harvest failed: {}\n", err);
            return;
        }

        std::unordered_map<std::string, std::string> byName;
        for (const auto& r : uuidRaw.records)
            if (!r.name.empty() && !r.uuid.empty()) byName.emplace(r.name, r.uuid);
        size_t stamped = 0;
        for (auto& r : records)
        {
            if (!r.uuid.empty()) continue;
            if (auto it = byName.find(r.name); it != byName.end()) { r.uuid = it->second; ++stamped; }
        }
        if (verbose)
            std::cout << std::format("[verbose]   COM uuid harvest: {} interface IID(s) captured, {} stamped\n",
                byName.size(), stamped);
    }

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

        // The first header's directory anchors the primary -I; also labels TimeTrace scopes
        // and attributes registered decls for LSP go-to-def.
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

        // Expand um/<->shared/ siblings: the Windows SDK splits its surface across both and
        // code from MSDN fails without the sibling (ERROR_SUCCESS, MAX_PATH, etc.).
        auto addScopeDir = [&](const std::string& d) {
            for (const auto& e : req.inScopeDirs) if (e == d) return;
            req.inScopeDirs.push_back(d);
        };
        for (const auto& h : headerPaths)
        {
            std::filesystem::path hdrDirPath = std::filesystem::path(h).parent_path();
            addScopeDir(hdrDirPath.string());
            // macOS framework: sibling headers are spelled `.../X.framework/Headers/...` but the
            // umbrella's real_path is `.../X.framework/Versions/A/Headers/...`. Scope the whole
            // X.framework bundle so both spellings pass the in-scope filter.
            {
                auto pos = h.find(".framework/");
                if (pos != std::string::npos)
                    addScopeDir(h.substr(0, pos + std::string(".framework").size()));
            }
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
            std::cout << std::format("[verbose] extracting C header{}:", headerPaths.size() > 1 ? " group" : "");
            for (const auto& h : headerPaths) std::cout << " " << h;
            std::cout << " (clang C++ API)\n";
        }

        cflat_cinterop::ExtractResult raw;
        std::string err;
        if (!cflat_cinterop::ExtractCInterop(req, raw, err))
        {
            if (verbose) std::cout << std::format("[verbose]   C header extraction failed: {}\n", err);
            return false;
        }

        // clang drops dependent decls on prereq errors, so registering the remnants would expose
        // a partial/wrong-sized API. Refuse and let the caller suggest a grouped import.
        if (raw.prereqErrors > 0)
        {
            if (outPrereqFailure) *outPrereqFailure = true;
            if (outPrereqMsg) *outPrereqMsg = raw.firstPrereqError;
            if (verbose)
                std::cout << std::format("[verbose]   header is not self-contained: {}\n", raw.firstPrereqError);
            return false;
        }

        if (outIncludes) *outIncludes = std::move(raw.includedFiles);

        // Header-COM interfaces (a record carrying an `lpVtbl` field) hide their IID in the C++
        // MIDL_INTERFACE form the C parse above cannot see. When any are present, harvest the GUIDs
        // with a focused C++ parse and stamp them onto the matching C records so iidof() resolves.
        if (HasComRecord(raw.records))
            HarvestComUuids(headerPaths, primaryDir, extraDefines, raw.records);

        {
            llvm::TimeTraceScope adoptScope("AdoptTypedefs", headerPath);
            AdoptRawTypedefs(raw);
        }

        // Records first so struct-by-value param/return types resolve; pruned to the by-value
        // closure so dependency structs (e.g. POINT for MSG) are included but unrelated ones aren't.
        {
            llvm::TimeTraceScope recordScope("RegisterCRecords", headerPath);
            PruneRecordsToNeededClosure(raw);
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
            std::cout << std::format("[verbose]   header bind: {} sig(s), {} enum(s), {} record(s), {} macro(s), {} func-macro(s), {} global(s)\n",
                outSigs.size(), outEnums.size(), outRecords.size(), outMacros.size(), outFuncMacros.size(), outGlobals.size());
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
            std::cout << std::format("[verbose] extracting C signatures: {} (clang C++ API)\n", cSourcePath);

        cflat_cinterop::ExtractResult raw;
        std::string err;
        if (!cflat_cinterop::ExtractCInterop(req, raw, err))
        {
            if (verbose) std::cout << std::format("[verbose]   C extraction failed: {}\n", err);
            return false;
        }

        {
            llvm::TimeTraceScope adoptScope("AdoptTypedefs", cSourcePath);
            AdoptRawTypedefs(raw);
        }
        {
            llvm::TimeTraceScope recordScope("RegisterCRecords", cSourcePath);
            // No prune here: the .c path has no scope filter, so every top-level record is wanted.
            // Pruning would drop the synthesized nested records (inScope=false) a pointer field names.
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
                    if (verbose) std::cout << std::format("[verbose] C signatures cache hit (mtime) for {}\n", fileForLsp);
                    hitSigs = entry.sigs;
                    hitRecords = entry.records;
                    hitGlobals = entry.globals;
                    hit = true;
                }
                // Timestamp moved but content may be identical - only now pay for a hash.
                else if (hashNow() == entry.hash)
                {
                    if (verbose) std::cout << std::format("[verbose] C signatures cache hit (hash) for {}\n", fileForLsp);
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

        // Cache miss - parse outside the lock; concurrent misses redo work harmlessly.
        // Extraction uses the clang C++ API in-process (no clang-cl needed), so LSP works too.
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
            std::cout << std::format("[verbose]   registered {} C enum constant(s) from {}\n", enums.size(), fileForLsp);
    }

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
            std::cout << std::format("[verbose]   registered {} C global(s) from {}\n", globals.size(), fileForLsp);
    }

    void RegisterCRecords(const std::vector<CRecordEntry>& records, const std::string& fileForLsp)
    {
        if (records.empty()) return;

        // Pass 1: opaque shells. CFlat-defined types win; anonymous records are skipped
        // (clang inlines their fields at the JSON layer).
        std::vector<const CRecordEntry*> ours;
        ours.reserve(records.size());
        for (const auto& r : records)
        {
            if (r.name.empty()) continue;
            if (dataStructures.find(r.name) != dataStructures.end()) continue;
            // Create the opaque shell so later fields/records in the same batch can refer to it.
            CreateStructType(r.name, /*typeAndValues*/{});
            ours.push_back(&r);
        }

        // Pass 2: bodies. On unmappable fields leave the opaque shell in place so a later
        // reference surfaces a clear error rather than crashing on a partial struct.
        for (const CRecordEntry* rp : ours)
        {
            const CRecordEntry& r = *rp;
            std::vector<DeclTypeAndValue> fields;
            fields.reserve(r.fields.size());
            bool ok = true;
            for (const auto& f : r.fields)
            {
                TypeAndValue tv;
                // Strip fixed-array dims before mapping: the shared mapper decays `[N]` to a
                // pointer (right for params, wrong for fields), so peel them here first.
                std::vector<uint64_t> arrDims;
                std::string elemSpelling = StripFixedArrayDims(f.ctype, arrDims);
                if (!MapCTypeToTypeAndValue(elemSpelling, tv))
                {
                    if (verbose) std::cout << std::format("[verbose]   skipping C {} '{}': unsupported field '{}' of type '{}'\n",
                        r.isUnion ? "union" : "struct", r.name, f.name, f.ctype);
                    ok = false;
                    break;
                }
                if (!arrDims.empty())
                {
                    tv.ConstArraySize = arrDims[0];
                    tv.ConstInnerDimensions.assign(arrDims.begin() + 1, arrDims.end());
                }
                // A C fn-ptr field maps to a THIN function<T> ("__c_fn_ptr") - a bare,
                // pointer-sized C function pointer, same size as the void* it replaces, so the
                // struct layout is unchanged. Keeping the real signature makes MIDL COM vtable
                // slots (e.g. ID3D12DeviceVtbl) callable as `obj->lpVtbl->Method(obj, ...)`
                // through the existing thin-call path, instead of an opaque void* the user
                // must reinterpret by hand.

                // A pointer field to a KNOWN aggregate keeps its pointee type instead of decaying
                // to opaque void* (the shared mapper's default for struct pointers). This is what
                // makes a COM object's `lpVtbl` typed as `<Interface>Vtbl*` so the member-access
                // chain resolves; unknown/opaque pointees still fall back to void*.
                if (!tv.IsFunctionPointer && tv.Pointer && tv.TypeName == "void")
                {
                    int ptrLevels = 0;
                    std::string tag = AggregatePointeeTag(elemSpelling, ptrLevels);
                    if (!tag.empty() && ptrLevels <= 2 && dataStructures.find(tag) != dataStructures.end())
                        tv.TypeName = tag;   // keep Pointer / ElemPointer as the mapper set them
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
                continue;
            }
            // An opaque-shell by-value aggregate field has no size; CreateStructType would
            // assert "Cannot getTypeInfo() on unsized". Abandon and leave the shell.
            for (const auto& d : fields)
            {
                if (d.Pointer) continue;            // pointers are always sized
                auto* ft = GetType(d);
                if (ft && !ft->isSized())
                {
                    if (verbose) std::cout << std::format("[verbose]   skipping C {} '{}': field '{}' has incomplete (unsized) type '{}'\n",
                        r.isUnion ? "union" : "struct", r.name, d.VariableName, d.TypeName);
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;
            // Bitfield packing uses the same MSVC LSB-first layout as native CFlat bitfields;
            // the packing pass produces synthetic slots and CreateStructType stores BitfieldInfo.
            std::vector<BitfieldInfo> packedBitfields;
            bool anyBitfields = false;
            for (const auto& tv : fields) { if (tv.IsBitfield) { anyBitfields = true; break; } }
            // Save semantic fields before PackBitfields replaces them with __bfN slots;
            // used only for LSP symbol registration below.
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
                // For bitfield records use prePackFields (semantic names before packing).
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

        // Register each header-COM interface's IID as its "uuid" type annotation (over ALL records,
        // not just freshly-created `ours`, so a cache hit re-annotates already-registered types).
        // EmitIidGlobalFor then resolves iidof(<HeaderComType>) through the existing uuid path.
        for (const auto& r : records)
        {
            if (r.uuid.empty()) continue;
            std::vector<AnnotationValue> anns;
            if (auto it = typeAnnotations_.find(r.name); it != typeAnnotations_.end()) anns = it->second;
            bool had = false;
            for (auto& a : anns) if (a.Name == "uuid") { a.Value = r.uuid; had = true; }
            if (!had) anns.push_back(AnnotationValue{ "uuid", r.uuid });
            SetTypeAnnotations(r.name, std::move(anns));
        }

        if (verbose)
            std::cout << std::format("[verbose]   registered {} C record(s) from {}\n", ours.size(), fileForLsp);
    }

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
            std::string valSuffix;
            if (m.isString)
            {
                // Intern the string literal and register a char* global (char* matches const char* ABI).
                tv.TypeName = "char";
                tv.Pointer  = true;
                llvm::Value* strGv = CreateGlobalString(".cmacro." + m.name, m.stringValue);
                c = llvm::cast<llvm::Constant>(strGv);
                valSuffix = std::format(" = \"{}\"", m.stringValue);
            }
            else if (m.isFloat)
            {
                // Float/double macro (e.g. M_PI). Always registered as `double` - CFlat narrows at
                // use site; double matches C's default FP promotion in variadic/unprototyped calls.
                tv.TypeName = "double";
                tv.Pointer  = false;
                c = llvm::ConstantFP::get(builder->getDoubleTy(), m.floatValue);
                valSuffix = std::format(" = {}", m.floatValue);
            }
            else if (m.isFuncPtr)
            {
                // A C function-pointer macro (e.g. ((int(*)(int,int))0)) is the THIN
                // function<R(P...)>: a bare C function pointer frozen at link time. The
                // constant is just the bit pattern reinterpreted as the thin signature.
                // Force the thin marker: funcPtrTV can arrive with an empty TypeName (e.g.
                // from a cached extraction), which would make GetType pick the fat closure
                // type {ptr,ptr} for the global while the initializer below is a thin ptr -
                // a definition the verifier rejects. A C macro fn-ptr is always thin.
                tv = m.funcPtrTV;
                tv.TypeName = "__c_fn_ptr";
                tv.VariableName = m.name;
                c = llvm::ConstantExpr::getIntToPtr(
                    builder->getInt64((uint64_t)m.value), BuildThinFnPtrType(tv));
            }
            else if (m.isPointer)
            {
                // Sentinel pointer: reinterpret the bit pattern as a void* so comparisons
                // against HANDLE-returning APIs work without an explicit cast.
                tv.TypeName = "void";
                tv.Pointer  = true;
                llvm::Type* i8Ptr = llvm::PointerType::get(*context, 0);
                llvm::Constant* bits = builder->getInt64((uint64_t)m.value);
                c = llvm::ConstantExpr::getIntToPtr(bits, i8Ptr);
                valSuffix = std::format(" = 0x{:x}", (uint64_t)m.value);
            }
            else if (!m.intTypeName.empty())
            {
                // Register with the macro's natural C type so call sites match without casts;
                // build at the type's width so truncation keeps the bit pattern exact.
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
            std::cout << std::format("[verbose]   registered {} C macro constant(s) (of {} object-like candidates)\n",
                registered, macros.size());
    }

    bool TranslateMacroBody(const CFunctionMacroEntry& m, std::string& out) const
    {
        std::unordered_set<std::string> paramSet(m.params.begin(), m.params.end());
        out.clear();
        out.reserve(m.body.size());
        const std::string& s = m.body;
        size_t i = 0;
        bool hasContent = false;

        // 'c' = argument list of a validated call (',' allowed);
        // 'g' = grouping paren (',' would be the comma operator, rejected).
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

            // Integer suffixes (u/U/l/L) are stripped; float suffixes kept; hex floats dropped.
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

        // Strip parens around bare identifiers: '(n)' is a C-style cast in CFlat grammar,
        // but never strip call-argument parens (preceded by identifier char).
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

    void RegisterCFunctionMacros(const std::vector<CFunctionMacroEntry>& funcMacros,
                                 const std::string& fileForLsp)
    {
        if (funcMacros.empty()) return;

        std::string generated;
        generated.reserve(funcMacros.size() * 60);
        size_t accepted = 0, rejected = 0, skipped = 0;

        for (const auto& m : funcMacros)
        {
            if (functionTable.count(m.name) ||
                globalNamedVariable.count(m.name))
            {
                ++skipped;
                if (verbose) std::cout << std::format("[verbose]   skip macro {}: name already defined\n", m.name);
                continue;
            }

            std::string translatedBody;
            if (!TranslateMacroBody(m, translatedBody))
            {
                ++rejected;
                if (verbose) std::cout << std::format("[verbose]   reject macro {}: body uses unsupported tokens\n", m.name);
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
            std::cout << std::format("[verbose]   function-like C macros: {} translated, {} rejected, {} skipped (already defined) from {}\n",
                accepted, rejected, skipped, fileForLsp);

        if (!generated.empty())
            pendingMacroSources_.push_back({ fileForLsp + "@cmacros", std::move(generated) });
    }

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

    bool CompileCHeaderGroup(const std::vector<std::string>& headerPaths,
                             const std::vector<std::string>& extraDefines = {},
                             bool diskCache = false)
    {
        if (headerPaths.empty()) return true;

        std::vector<std::string> realPaths;
        realPaths.reserve(headerPaths.size());
        for (const auto& h : headerPaths)
        {
            llvm::SmallString<256> rp;
            realPaths.push_back(!llvm::sys::fs::real_path(h, rp) ? rp.str().str() : h);
        }
        const std::string& fileForLsp = realPaths.front();

        // Fold headers, include-dirs, and defines: same header under different roots/defines or
        // standalone vs. grouped must not share a stale cache entry.
        std::string cacheKey;
        for (const auto& rp : realPaths)       cacheKey += "|H" + rp;
        for (const auto& inc : cIncludeDirs_)  cacheKey += "|I" + inc;
        for (const auto& def : cDefines_)      cacheKey += "|D" + def;
        for (const auto& def : extraDefines)   cacheKey += "|d" + def;

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
                    if (verbose) std::cout << std::format("[verbose] C header cache hit (mtime) for {}\n", fileForLsp);
                    hitSigs = entry.sigs; hitEnums = entry.enums; hitRecords = entry.records;
                    hitMacros = entry.macros; hitFuncMacros = entry.funcMacros; hitGlobals = entry.globals;
                    hitAliases = entry.recordAliases; hit = true;
                }
                else if (hashNow() == entry.hash)
                {
                    if (verbose) std::cout << std::format("[verbose] C header cache hit (hash) for {}\n", fileForLsp);
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

        // Persistent disk cache (opt-in via `cache` import clause). On hit, preloads the
        // in-memory cache and registers decls, skipping the clang header parse entirely.
        std::filesystem::path cHeaderCacheDir = GetCHeaderCacheDir();
        uint64_t diskKey = 0;
        if (diskCache && !mtEc && !cHeaderCacheDir.empty())
        {
            diskKey = CHeaderDiskCacheKey(realPaths, cIncludeDirs_, cDefines_, extraDefines);
            CFileSigCacheEntry diskEntry;
            bool diskHit;
            {
                llvm::TimeTraceScope loadScope("CHeaderJsonLoad", fileForLsp);
                diskHit = TryLoadCHeaderDiskCache(cHeaderCacheDir, diskKey, currentMtime, hashNow(), diskEntry);
            }
            if (diskHit)
            {
                if (verbose) std::cout << std::format("[verbose] C header disk cache hit for {}\n", fileForLsp);
                {
                    std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
                    cFileSigCache_[cacheKey] = diskEntry;
                }
                llvm::TimeTraceScope registerScope("CHeaderRegister", fileForLsp);
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
            // All C entities are extracted in one full parse (plus a cheap preprocess-only prepass
            // for macro names). Uses clang C++ API, not clang-cl or libclang.
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
            // Keep only real on-disk paths in the transitive dependency list (deep mode).
            // Non-existent deps would otherwise poison every later cache validation.
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
            // --run is read-only: never persist header cache to disk even with 'cache' clause.
            // The in-memory entry still serves this compile.
            if (diskCache && !runMode_ && !cHeaderCacheDir.empty())
                WriteCHeaderDiskCache(cHeaderCacheDir, diskKey, currentMtime, hashNow(), entry);
            std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
            cFileSigCache_[cacheKey] = std::move(entry);
        }

        // Records were already registered inside ExtractCHeaderClang.
        {
            llvm::TimeTraceScope registerScope("CHeaderRegister", fileForLsp);
            RegisterCSignatures(sigs, fileForLsp);
            RegisterCEnums(enums, fileForLsp);
            RegisterCMacros(macros);
            RegisterCFunctionMacros(funcMacros, fileForLsp);
            RegisterCGlobals(globals, fileForLsp);
        }
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

        std::string triple = targetMacOS_
            ? std::string("arm64-apple-macosx")
            : (platformValue == 32 ? "i686-pc-windows-msvc" : "x86_64-pc-windows-msvc");
        std::string cpu = !targetCpu_.empty()
            ? targetCpu_
            : (targetMacOS_ ? std::string("apple-m1")
                            : (platformValue == 32 ? std::string("i686") : std::string("x86-64")));

        std::string err;
        const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, err);
        if (!target)
            return nullptr;

        llvm::TargetOptions opt;
        return std::unique_ptr<llvm::TargetMachine>(
            target->createTargetMachine(triple, cpu, "", opt, llvm::Reloc::PIC_));
    }

    // Native ELF code emission + link for Linux (and other ELF/Unix hosts).
    // Emits an x86-64 ELF object for the host triple and links it with the
    // system C compiler driver (cc/gcc/clang), which supplies crt1.o (_start ->
    // __libc_start_main -> main) and libc. cflat emits a C-ABI `main`, so no
    // custom entry point is needed. Windows/COFF/SEH/lld-link stay in
    // EmitExecutable; this path is the Stage-3 ELF target seam.
    bool EmitExecutableElf(const std::string& exePath, bool debugInfo,
                           const std::optional<std::string>& lliPath)
    {
        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmPrinters();

        const std::string triple = llvm::sys::getProcessTriple(); // e.g. x86_64-unknown-linux-gnu
        module->setTargetTriple(triple);

        std::string err;
        const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, err);
        if (!target)
        {
            std::cout << std::format("Error: no target for triple '{}': {}\n", triple, err);
            return false;
        }

        std::string cpu = targetCpu_.empty() ? std::string("x86-64") : targetCpu_;
        llvm::TargetOptions opt;
        opt.FunctionSections = true;
        opt.DataSections     = true;
        // PIC so the object links into a position-independent executable (the
        // default on modern Linux toolchains).
        auto TM = std::unique_ptr<llvm::TargetMachine>(
            target->createTargetMachine(triple, cpu, "", opt, llvm::Reloc::PIC_));
        module->setDataLayout(TM->createDataLayout());

        // Keep cflat's runtime DEFINITIONS out of the dynamic symbol table so they do
        // not interpose libc. cflat reuses libc names (printf/vsnprintf/malloc/...);
        // an executable that defines a symbol libc also exports would preempt libc's
        // OWN internal calls to it (e.g. glibc __vsnprintf_chk -> vsnprintf -> cflat's
        // vsnprintf), recursing forever. Hidden visibility makes each definition local
        // to the image while still callable from cflat code; external declarations
        // (the libc imports) are left untouched so they still bind to libc.
        for (llvm::Function& F : module->functions())
            if (!F.isDeclaration())
                F.setVisibility(llvm::GlobalValue::HiddenVisibility);
        for (llvm::GlobalVariable& G : module->globals())
            if (!G.isDeclaration())
                G.setVisibility(llvm::GlobalValue::HiddenVisibility);

        if (lliPath)
        {
            if (verbose) std::cout << std::format("[verbose] writing IR to {}\n", *lliPath);
            if (!SaveToFile(*lliPath))
            {
                std::cout << std::format("Error: failed to save IR to '{}'.\n", *lliPath);
                return false;
            }
        }

        const std::string objPath = exePath + ".o";
        std::error_code EC;
        llvm::raw_fd_ostream dest(objPath, EC, llvm::sys::fs::OF_None);
        if (EC)
        {
            std::cout << std::format("Error: could not write object file '{}': {}\n", objPath, EC.message());
            return false;
        }
        {
            llvm::legacy::PassManager pass;
            // Codegen runs its own libcall simplification with a default TargetLibraryInfo
            // (where vsnprintf/vfprintf are "available"), which would re-fold our
            // __vsnprintf_chk/__vfprintf_chk calls back into cflat's own vsnprintf/vfprintf
            // and recurse forever - even though the IR-level opt pipeline already kept them
            // intact. Seed the codegen PM with the same stdio-safe TLI so the fold stays off.
            pass.add(new llvm::TargetLibraryInfoWrapperPass(MakeStdioSafeTLII(llvm::Triple(triple))));
            if (TM->addPassesToEmitFile(pass, dest, nullptr, llvm::CodeGenFileType::ObjectFile))
            {
                std::cout << "Error: target does not support object file emission\n";
                return false;
            }
            pass.run(*module);
            dest.flush();
            dest.close();
        }

        // Link with the system C compiler driver: it pulls in the C runtime
        // startup and libc and resolves cflat's `main`.
        std::string cc;
        for (const char* cand : { "cc", "gcc", "clang", "clang-18" })
        {
            if (auto p = llvm::sys::findProgramByName(cand)) { cc = *p; break; }
        }
        if (cc.empty())
        {
            llvm::sys::fs::remove(objPath);
            std::cout << "Error: no C compiler driver (cc/gcc/clang) found to link the executable\n";
            return false;
        }

        std::vector<std::string> argStrs = { cc, objPath, "-o", exePath };
        // GC unreferenced sections: the auto-imported core runtime defines many
        // functions (stdio/stdout shims) that still reference Windows CRT symbols
        // until the core libs are ported. Emitting one section per function (set
        // above) lets the linker drop the ones this program does not call.
        argStrs.push_back("-Wl,--gc-sections");
        // -no-pie keeps global symbols out of .dynsym so --gc-sections can drop
        // unreferenced runtime functions (otherwise PIE exports them as GC roots).
        argStrs.push_back("-no-pie");
        // Merge any C objects compiled from .c inputs.
        for (auto& cObj : cObjectFiles_) argStrs.push_back(cObj);
        // Prebuilt C libraries (--c-lib).
        for (const auto& lib : cLinkLibs_) argStrs.push_back(lib);
        // Common runtime deps for cflat programs (math, threads, dlopen).
        argStrs.push_back("-lm");
        argStrs.push_back("-lpthread");
        argStrs.push_back("-ldl");
        if (debugInfo) argStrs.push_back("-g");

        std::vector<llvm::StringRef> args;
        for (auto& s : argStrs) args.push_back(s);

        std::cout << std::format("Linking (elf): {}\n", exePath);
        std::string linkErr;
        int rc = llvm::sys::ExecuteAndWait(cc, args, std::nullopt, {}, 0, 0, &linkErr);
        llvm::sys::fs::remove(objPath);
        for (auto& cObj : cObjectFiles_) llvm::sys::fs::remove(cObj);
        if (rc != 0)
        {
            std::cout << std::format("Error: linking failed (exit {}): {}\n", rc, linkErr);
            return false;
        }
        return true;
    }

    // Cross-compile to a macOS arm64 Mach-O object (--platform macos). On a Mac
    // this would link via clang/ld64; on a non-Darwin host (the WSL cross-build)
    // no ld64 / macOS SDK is present, so this emits the relocatable Mach-O object
    // and stops, leaving <exePath>.o for a later link on a real Mac. The AArch64
    // backend must be registered in this LLVM build (apt llvm-18 on WSL has it;
    // the Windows vcpkg LLVM is X86-only, so this cleanly errors there).
    // Records a framework for the Mach-O link (`import framework "X"`). Dedups on
    // insert, preserving first-seen order. On a non-macOS target this is an error,
    // except in LSP analyze mode (symbolSink_ set) where it is recorded silently.
    bool AddFrameworkImport(const std::string& name)
    {
        if (!targetMacOS_ && symbolSink_ == nullptr)
        {
            LogError(std::format("import framework '{}' is only supported when targeting macOS", name));
            return false;
        }
        for (const auto& f : cFrameworks_) if (f == name) return true;
        cFrameworks_.push_back(name);
        return true;
    }

    bool EmitExecutableMachO(const std::string& exePath, bool debugInfo,
                             const std::optional<std::string>& lliPath)
    {
        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmPrinters();

        // Versioned triple (min macOS 11.0, the Apple Silicon baseline) so the
        // emitted Mach-O carries an LC_BUILD_VERSION load command; without a
        // version ld64 warns "no platform load command found" on every link.
        const std::string triple = "arm64-apple-macosx11.0.0";
        module->setTargetTriple(triple);

        std::string err;
        const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, err);
        if (!target)
        {
            std::cout << std::format("Error: no target for triple '{}': {}. The macOS arm64 "
                                     "target needs an LLVM built with the AArch64 backend "
                                     "(apt llvm-18 on Linux/WSL has it).\n", triple, err);
            return false;
        }

        // Apple Silicon baseline. --cpu overrides (e.g. apple-m2).
        std::string cpu = targetCpu_.empty() ? std::string("apple-m1") : targetCpu_;
        llvm::TargetOptions opt;
        opt.FunctionSections = true;
        opt.DataSections     = true;
        // Darwin code is always PIC.
        auto TM = std::unique_ptr<llvm::TargetMachine>(
            target->createTargetMachine(triple, cpu, "", opt, llvm::Reloc::PIC_));
        module->setDataLayout(TM->createDataLayout());

        if (lliPath)
        {
            if (verbose) std::cout << std::format("[verbose] writing IR to {}\n", *lliPath);
            if (!SaveToFile(*lliPath))
            {
                std::cout << std::format("Error: failed to save IR to '{}'.\n", *lliPath);
                return false;
            }
        }

        const std::string objPath = exePath + ".o";
        std::error_code EC;
        llvm::raw_fd_ostream dest(objPath, EC, llvm::sys::fs::OF_None);
        if (EC)
        {
            std::cout << std::format("Error: could not write object file '{}': {}\n", objPath, EC.message());
            return false;
        }
        {
            llvm::legacy::PassManager pass;
            // Same stdio-safe TLI as EmitExecutableElf: codegen runs its own libcall
            // simplification, which would fortify-fold our __vsnprintf_chk call back
            // into a bare vsnprintf - and cflat DEFINES vsnprintf, so that recurses
            // forever. Mark the format libcalls unavailable so the fold stays off.
            pass.add(new llvm::TargetLibraryInfoWrapperPass(MakeStdioSafeTLII(llvm::Triple(triple))));
            if (TM->addPassesToEmitFile(pass, dest, nullptr, llvm::CodeGenFileType::ObjectFile))
            {
                std::cout << "Error: target does not support object file emission\n";
                return false;
            }
            pass.run(*module);
            dest.flush();
            dest.close();
        }

        // Link only if a Darwin-capable linker exists (a real Mac, or osxcross).
        // ld64 / the macOS SDK is absent on the WSL cross-host, so the object IS
        // the deliverable there; report it and stop without claiming an exe.
        const bool darwinHost =
            llvm::Triple(llvm::sys::getProcessTriple()).isOSDarwin();

#if defined(__APPLE__)
        // Prefer the bundled ld64.lld (deployed next to cflat), invoked directly -
        // mirroring the Windows lld-link path. The SDK still supplies libSystem via
        // -syslibroot; harvesting our own libSystem.tbd (step 3) is what drops that.
        if (darwinHost)
        {
            const std::string ld64 = FindBundledLd64Lld();
            // Prefer the SDK-free stub harvested by `cflat --init` (no CLT needed);
            // fall back to $SDKROOT / xcrun when it hasn't been generated.
            const std::string stubRoot = MacStubSyslibroot();
            std::string sdk = stubRoot;
            std::string sdkVer = "11.0";
            if (sdk.empty())
            {
                if (const char* env = std::getenv("SDKROOT")) sdk = env;
                if (sdk.empty()) sdk = CaptureToolLine("xcrun --show-sdk-path 2>/dev/null");
                std::string v = CaptureToolLine("xcrun --show-sdk-version 2>/dev/null");
                if (!v.empty()) sdkVer = v;
            }
            if (!ld64.empty() && !sdk.empty())
            {
                std::vector<std::string> argStrs = {
                    ld64, "-arch", "arm64",
                    "-platform_version", "macos", "11.0.0", sdkVer,
                    "-syslibroot", sdk, "-o", exePath, objPath };
                for (auto& cObj : cObjectFiles_) argStrs.push_back(cObj);
                for (const auto& lib : cLinkLibs_) argStrs.push_back(lib);
                // macOS frameworks (`import framework`). When the SDK-free harvested
                // root is in use, point -F at its Frameworks dir so ld64 finds the
                // harvested stubs; the SDK/xcrun syslibroot already carries the default path.
                if (!cFrameworks_.empty() && !stubRoot.empty())
                    argStrs.push_back("-F"), argStrs.push_back(stubRoot + "/System/Library/Frameworks");
                // Frameworks not in the harvested root (e.g. CoreGraphics from a header
                // bind) resolve from the real SDK: add its Frameworks dir as an extra -F.
                // ld64 tries syslibroot+path then path, so this coexists with the stub root.
                if (!cFrameworks_.empty())
                {
                    const std::string& fwSdk = MacSdkPathCached();
                    if (!fwSdk.empty() && fwSdk != stubRoot)
                        argStrs.push_back("-F"), argStrs.push_back(fwSdk + "/System/Library/Frameworks");
                }
                for (const auto& fw : cFrameworks_)
                    argStrs.push_back("-framework"), argStrs.push_back(fw);
                // The AArch64 backend can emit compiler-rt libcalls (e.g. __multi3);
                // clang always links libclang_rt.osx.a, so mirror it when locatable.
                // Also the source of the asan runtime dylib below (--asan) - hoisted
                // so we only shell out to `clang -print-resource-dir` once.
                const std::string rtDir = CaptureToolLine("clang -print-resource-dir 2>/dev/null");
                if (asan_)
                {
                    const std::string asanDylib = rtDir + "/lib/darwin/libclang_rt.asan_osx_dynamic.dylib";
                    if (rtDir.empty() || !llvm::sys::fs::exists(asanDylib))
                    {
                        LogError("--asan on macOS requires the AddressSanitizer runtime "
                                 "from Xcode or the Command Line Tools (the SDK-free "
                                 "harvested-stub link cannot supply it); install one and retry.");
                        return false;
                    }
                    // Install name is @rpath/libclang_rt.asan_osx_dynamic.dylib.
                    // Listed before -lSystem to mirror clang's own
                    // -fsanitize=address link order (link order does not by
                    // itself affect interception - see the __asan_default_options
                    // override in OptimizeModule for the real fix needed here).
                    argStrs.push_back(asanDylib);
                    argStrs.push_back("-rpath");
                    argStrs.push_back(rtDir + "/lib/darwin");
                }
                argStrs.push_back("-lSystem");
                if (!rtDir.empty())
                {
                    const std::string rt = rtDir + "/lib/darwin/libclang_rt.osx.a";
                    if (llvm::sys::fs::exists(rt)) argStrs.push_back(rt);
                }
                std::vector<llvm::StringRef> args;
                for (auto& s : argStrs) args.push_back(s);
                if (verbose)
                {
                    std::string joined;
                    for (auto& s : argStrs) joined += (joined.empty() ? "" : " ") + s;
                    std::cout << std::format("[verbose] ld64.lld link line: {}\n", joined);
                }
                std::cout << std::format("Linking (ld64.lld{}): {}\n",
                                         stubRoot.empty() ? "" : ", SDK-free", exePath);
                std::string linkErr;
                int rc = llvm::sys::ExecuteAndWait(ld64, args, std::nullopt, {}, 0, 0, &linkErr);
                llvm::sys::fs::remove(objPath);
                for (auto& cObj : cObjectFiles_) llvm::sys::fs::remove(cObj);
                if (rc != 0)
                {
                    std::cout << std::format("Error: linking failed (exit {}): {}\n", rc, linkErr);
                    return false;
                }
                return true;
            }
        }
#endif

        // Fallback: host clang driver (bundled ld64.lld/SDK missing, or an osxcross
        // cross-link from a non-Darwin host). Same behavior as before this change.
        std::string cc;
        for (const char* cand : { "o64-clang", "oa64-clang", "clang" })
        {
            if (auto p = llvm::sys::findProgramByName(cand)) { cc = *p; break; }
        }
        if (cc.empty() || !darwinHost)
        {
            (void)debugInfo;
            std::cout << std::format("Emitted Mach-O arm64 object (no Darwin linker on this host; "
                                     "link on macOS): {}\n", objPath);
            return true;
        }

        std::vector<std::string> argStrs = { cc, "-target", "arm64-apple-macosx11.0.0",
                                             objPath, "-o", exePath };
        for (auto& cObj : cObjectFiles_) argStrs.push_back(cObj);
        for (const auto& lib : cLinkLibs_) argStrs.push_back(lib);
        // macOS frameworks (`import framework`); the clang driver accepts -framework.
        for (const auto& fw : cFrameworks_)
            argStrs.push_back("-framework"), argStrs.push_back(fw);
        if (debugInfo) argStrs.push_back("-g");
        if (asan_) argStrs.push_back("-fsanitize=address");

        std::vector<llvm::StringRef> args;
        for (auto& s : argStrs) args.push_back(s);

        std::cout << std::format("Linking (mach-o): {}\n", exePath);
        std::string linkErr;
        int rc = llvm::sys::ExecuteAndWait(cc, args, std::nullopt, {}, 0, 0, &linkErr);
        llvm::sys::fs::remove(objPath);
        for (auto& cObj : cObjectFiles_) llvm::sys::fs::remove(cObj);
        if (rc != 0)
        {
            std::cout << std::format("Error: linking failed (exit {}): {}\n", rc, linkErr);
            return false;
        }
        return true;
    }

    bool EmitExecutable(const std::string& exePath, const std::string& platform, bool debugInfo = false,
                        const std::optional<std::string>& lliPath = std::nullopt)
    {
        // macOS arm64 cross-target: Mach-O object emission, independent of host OS
        // (handled before the host-specific COFF/ELF split below).
        if (targetMacOS_)
            return EmitExecutableMachO(exePath, debugInfo, lliPath);
#if !defined(_WIN32)
        // Linux/Unix host: emit a native ELF executable. The Windows/COFF path
        // below is unreachable here (but still compiles).
        (void)platform;
        return EmitExecutableElf(exePath, debugInfo, lliPath);
#endif
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
            std::cout << std::format("Error: no target for triple '{}': {}\n", triple, err);
            return false;
        }

        // --cpu overrides the platform default. The value was already resolved ("native"
        // -> host CPU) and validated in Compile, so it can be used verbatim here.
        if (!targetCpu_.empty())
            cpu = targetCpu_;

        llvm::TargetOptions opt;
        // Emit one section per function/global so lld-link's /OPT:REF can garbage-collect
        // unreferenced symbols. This is pure object layout - it adds no optimization and does
        // not touch the IR, so it has no effect on the --out-lli dump written just below.
        opt.FunctionSections = true;
        opt.DataSections     = true;
        auto TM = std::unique_ptr<llvm::TargetMachine>(
            target->createTargetMachine(triple, cpu, "", opt, llvm::Reloc::PIC_));
        module->setDataLayout(TM->createDataLayout());

        // --out-lli for an -o build: dump the IR here, after the target triple and data layout
        // are finalized and immediately before codegen, so the .ll is exactly what gets
        // instruction-selected into the object. (Standalone --out-lli without -o is written in
        // Compile, since EmitExecutable does not run there.)
        if (lliPath)
        {
            llvm::TimeTraceScope irScope("WriteIR", *lliPath);
            if (verbose) std::cout << std::format("[verbose] writing IR to {}\n", *lliPath);
            if (!SaveToFile(*lliPath))
            {
                std::cout << std::format("Error: failed to save IR to '{}'.\n", *lliPath);
                return false;
            }
        }

        auto objPath = exePath + ".obj";
        std::error_code EC;
        llvm::raw_fd_ostream dest(objPath, EC, llvm::sys::fs::OF_None);
        if (EC)
        {
            std::cout << std::format("Error: could not write object file '{}': {}\n", objPath, EC.message());
            return false;
        }

        {
            llvm::TimeTraceScope codegenScope("ObjectCodegen", exePath);
            llvm::legacy::PassManager pass;
            if (TM->addPassesToEmitFile(pass, dest, nullptr, llvm::CodeGenFileType::ObjectFile))
            {
                std::cout << "Error: target does not support object file emission\n";
                return false;
            }
            pass.run(*module);
            dest.flush();
            dest.close();
        }

        const std::string arch = (platform == "win32") ? "x86" : "x64";

        // Under -g, compile and link the in-process crash handler (DbgHelp symbolizes CFlat frames).
        // If CompileCrashHandlerObject fails, skip handler link args but still produce the PDB.
        bool crashHandlerLinked = debugInfo && CompileCrashHandlerObject(arch);

        // Keep the stock VC CRT (msvcrt.lib + vcruntime140.dll + mainCRTStartup) when:
        //   - ASan: its runtime expects the stock CRT and would clash with our mem*; or
        //   - x86 (win32): freestanding x86 additionally needs the x86 64-bit compiler-runtime
        //     intrinsics (__alldiv/__aulldiv/__allrem/__aullrem/__allmul/shifts), __tls_array,
        //     and __fltused - normally from the VC CRT and not available to link as compiler-rt
        //     here. Until those are ported, x86 stays on the proven stock path. The x86 import
        //     libs are already synthesized (--init) and the cflat_builtins.c decoration guards
        //     are in place, so finishing x86 is just that intrinsic set. See Phase D of
        //     internal/plan/remove-vcruntime-dependency.md.
        // Otherwise we drop vcruntime entirely: cflat_builtins.c supplies our own CRT entry, the
        // mem*/str* intrinsics and (on x64) a real __C_specific_handler, so even the `program`
        // construct's SEH crash isolation (catchpad personality, MainListener.h) works without it.
        const bool keepVcRuntime = asan_ || arch == "x86";

        if (!keepVcRuntime)
            CompileBuiltinsObject(arch);

        LinkerPaths linkerPaths_;
        {
            llvm::TimeTraceScope pathScope("FindLinkerPaths", exePath);
            linkerPaths_ = FindLinkerPaths(arch, runtimeDir, verbose);
        }
        const std::string& lldLinkPath = linkerPaths_.lldLink;
        const std::string& msvcLibPath = linkerPaths_.msvcLib;
        const std::string& ucrtLibPath = linkerPaths_.ucrtLib;
        const std::string& umLibPath   = linkerPaths_.umLib;

        if (lldLinkPath.empty())
        {
            llvm::sys::fs::remove(objPath);
            std::cout << "Error: lld-link.exe not found\n";
            return false;
        }

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
            // DbgHelp for the in-process crash handler. Gated on crashHandlerLinked so a failed
            // handler compile doesn't /INCLUDE a symbol that was never emitted.
            // (heap_audit.c pulls in dbghelp.lib itself via #pragma comment(lib) so its leak-site
            // symbolization works whether or not the crash handler is linked.)
            linkArgStrs.push_back("dbghelp.lib");
            // Force-retain the crash handler's .CRT$XCU initializer - lld-link's /OPT:REF would
            // garbage-collect it since it has no external references. x86 uses a leading underscore.
            linkArgStrs.push_back(arch == "x86"
                ? "/INCLUDE:_cflat_crash_init_"
                : "/INCLUDE:cflat_crash_init_");
        }
        // SDK-free path: if --init synthesized the system import libs (from the OS-resident
        // DLLs) and this is a freestanding x64 build, link against those instead of the Windows
        // SDK / VS lib directories - so the user needs neither installed. ASan still needs the
        // VS libs (clang_rt.asan + the stock CRT), so it keeps the SDK paths. /nodefaultlib:
        // oldnames suppresses the /defaultlib:oldnames directive the clang-cl (/MD) objects emit;
        // oldnames.lib lives in the VS lib dir we are deliberately not adding, and nothing here
        // needs its POSIX-name aliases.
        // x64 only for now: x86 also reaches here as keepVcRuntime (stock path), and finishing
        // x86 freestanding is gated on the x86 compiler-runtime intrinsics (see keepVcRuntime).
        std::string syntheticLibDir = GetSyntheticLibDir(arch);
        const bool useSyntheticLibs = !keepVcRuntime && arch == "x64"
            && !syntheticLibDir.empty()
            && std::filesystem::exists(std::filesystem::path(syntheticLibDir) / "ucrt.lib")
            && std::filesystem::exists(std::filesystem::path(syntheticLibDir) / "kernel32.lib");

        if (useSyntheticLibs)
        {
            // Synthetic dir first: ucrt/kernel32/ws2_32/ntdll/dbghelp resolve here (first match
            // wins), so basic code links with no Windows SDK present. The SDK dirs are added
            // after as a fallback chain for advanced cases that genuinely still need the SDK:
            //   - 'um':       long-tail system import libs we do not synthesize (user32/gdi32/
            //                 uuid/... pulled by system-header `#pragma comment(lib,...)`).
            //   - 'msvc-lib': libcmt/msvcrt - reached only by a prebuilt user lib built /MT, a
            //                 user .c using /GS (__security_cookie/check), or a CRT header-inline
            //                 we have not shimmed. A basic build requests none of these, so it
            //                 never consults this dir and stays SDK-free; lld pulls only the
            //                 specific archive members an advanced build leaves undefined.
            linkArgStrs.push_back("/libpath:" + syntheticLibDir);
            linkArgStrs.push_back("/nodefaultlib:oldnames");
            if (!umLibPath.empty())   linkArgStrs.push_back("/libpath:" + umLibPath);
            if (!msvcLibPath.empty()) linkArgStrs.push_back("/libpath:" + msvcLibPath);
        }
        else
        {
            if (!msvcLibPath.empty()) linkArgStrs.push_back("/libpath:" + msvcLibPath);
            if (!ucrtLibPath.empty()) linkArgStrs.push_back("/libpath:" + ucrtLibPath);
            if (!umLibPath.empty())   linkArgStrs.push_back("/libpath:" + umLibPath);
        }

        // No source for the OS CRT import libs: neither the --init cache (synthetic libs)
        // nor a Windows SDK ucrt.lib is present. Fail early with an actionable message
        // instead of letting lld-link emit a raw "could not open ucrt.lib".
        if (!useSyntheticLibs && ucrtLibPath.empty())
        {
            llvm::sys::fs::remove(objPath);
            for (auto& cObj : cObjectFiles_) llvm::sys::fs::remove(cObj);
            if (!keepVcRuntime && arch == "x64")
                LogError("Run cflat --init for the first time to finish setting up.");
            else
                LogError("Windows SDK / Visual Studio build tools are required for this build "
                         "configuration but were not found.");
            return false;
        }

        // AddressSanitizer: link the dynamic asan runtime (matches /MD-style CRT), force-include
        // the thunk via /wholearchive to retain CRT$XI* interceptors. DLL copied next to exe.
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

        // ucrt.lib is always needed - it is the import lib for the OS-resident Universal CRT.
        linkArgStrs.push_back("ucrt.lib");
        if (keepVcRuntime)
        {
            // ASan path: keep the stock VC CRT. msvcrt.lib provides mainCRTStartup (the default
            // entry) and vcruntime140.dll provides the mem*/EH symbols ASan's runtime expects.
            linkArgStrs.push_back("msvcrt.lib");
            linkArgStrs.push_back("vcruntime.lib");
        }
        else
        {
            // Freestanding path: cflat_builtins.c supplies our own entry (cflat_start) plus the
            // mem*/str* intrinsics and a real x64 __C_specific_handler. So we need neither the VC
            // startup lib (msvcrt.lib, a Visual Studio component) nor VCRUNTIME140.dll (the VC++
            // redistributable). /nodefaultlib suppresses the /defaultlib directives the clang-cl
            // (/MD) builtins object emits that would otherwise re-pull both.
            linkArgStrs.push_back("/entry:cflat_start");
            linkArgStrs.push_back("/nodefaultlib:msvcrt.lib");
            linkArgStrs.push_back("/nodefaultlib:vcruntime.lib");
            // _fltused (the MSVC FP-usage marker) is dropped with the VC CRT. The synthetic
            // ntdll.lib re-exports it, but the Windows SDK's ntdll.lib does not - so on the
            // SDK-fallback path (no --init) it is unresolved. Redirect it to our private
            // definition only when nothing else supplies it; /alternatename is a no-op when
            // _fltused already resolves (synthetic path), avoiding a duplicate symbol.
            linkArgStrs.push_back("/alternatename:_fltused=__cflat_fltused");
            // __chkstk (the MSVC stack-probe stub, emitted for >4KB frames) lives in the static
            // CRT, which the freestanding link drops. kernel32.dll exports it (forwarded to ntdll),
            // so the synthetic-lib path resolves it and this redirect is a no-op; on the SDK
            // fallback (whose kernel32.lib omits __chkstk) it supplies our private definition.
            linkArgStrs.push_back("/alternatename:__chkstk=__cflat_chkstk");
        }
        linkArgStrs.push_back("kernel32.lib");
        linkArgStrs.push_back("ws2_32.lib");
        // ntdll.lib provides RtlUnwindEx, used by cflat_builtins.c's __C_specific_handler.
        // Harmless when unreferenced (no DLL import is added unless a symbol is pulled).
        linkArgStrs.push_back("ntdll.lib");
        // advapi32.lib provides OpenProcessToken/LookupPrivilegeValueA/AdjustTokenPrivileges
        // (core/os.windows.cb's huge-page privilege dance). Harmless when unreferenced.
        linkArgStrs.push_back("advapi32.lib");
        linkArgStrs.push_back(objPath);
        // Merge any C objects compiled by clang-cl from .c inputs.
        for (auto& cObj : cObjectFiles_) linkArgStrs.push_back(cObj);
        // Prebuilt C import libraries (--c-lib): add each lib's dir as libpath, then name.
        // Keeps behavior uniform with system libs above.
        for (const auto& lib : cLinkLibs_)
        {
            auto libDir = std::filesystem::path(lib).parent_path().string();
            if (!libDir.empty()) linkArgStrs.push_back("/libpath:" + libDir);
            linkArgStrs.push_back(std::filesystem::path(lib).filename().string());
        }

        std::vector<llvm::StringRef> linkArgs;
        for (auto& s : linkArgStrs) linkArgs.push_back(s);

        std::cout << std::format("Linking ({}): {}\n", arch, exePath);

        {
            llvm::TimeTraceScope linkScope("Link", exePath);
            std::string linkErr;
            int rc = llvm::sys::ExecuteAndWait(lldLinkPath, linkArgs, std::nullopt, {}, 0, 0, &linkErr);
            llvm::sys::fs::remove(objPath);
            for (auto& cObj : cObjectFiles_) llvm::sys::fs::remove(cObj);

            if (rc != 0)
            {
                std::cout << std::format("Error: linking failed (exit {}): {}\n", rc, linkErr);
                return false;
            }
        }

        // Copy each --c-lib's sibling runtime DLL next to the exe for self-contained launch.
        // Conan puts the DLL beside the import lib or in ../bin - check both. Best-effort.
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
                std::cout << std::format("[verbose]   copied asan runtime DLL: {} -> {}\n", asanDllToCopy, dest.string());
        }

        // Normal builds are vcruntime-free, but an --asan build links the VC++ runtime (and its
        // asan runtime DLL needs it too). If it is not installed the program fails to start with
        // a cryptic loader error, so point the user at the redistributable - they may not know.
        if (keepVcRuntime && !VcRuntimeInstalled())
        {
            const char* redist = (arch == "x86") ? "vc_redist.x86.exe" : "vc_redist.x64.exe";
            std::cout << std::format(
                "Note: this --asan build depends on the Microsoft Visual C++ runtime "
                "(vcruntime140.dll),\n"
                "      which was not found on this system. The program will not start until the\n"
                "      Microsoft Visual C++ Redistributable is installed:\n"
                "        https://aka.ms/vs/17/release/{}\n"
                "      (Normal builds do not depend on it; only --asan does.)\n", redist);
        }
        return true;
    }

    // BFS over static call graph to detect thread-spawn reachability from main (--run guard).
    // Indirect calls not followed; the spawn primitive is always reached via direct call from the wrapper.
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

    // --heap-audit instrumentation. Insert HeapAudit.enable() as main's first action and
    // HeapAudit.reportLeaks() immediately before every return (after the function's scope
    // destructors have run), so any program is audited for leaks/double-frees without source
    // edits. Report-only: reportLeaks prints still-live allocations to stderr but does not
    // alter the exit code; a double free is likewise printed (advisory) without aborting,
    // since a FREED-slot free can be a reuse artifact of a pre-enable() allocation.
    // enable() is idempotent, so a program that already audits itself just emits a duplicate
    // report. No-op when this module has no user main (e.g. a C-only or library compile).
    void InjectHeapAuditIntoMain()
    {
        llvm::Function* mainFn = module->getFunction("main");
        if (!mainFn || mainFn->isDeclaration())
            return;

        llvm::Function* enableFn = module->getFunction("_HeapAudit.enable_void__");
        llvm::Function* reportFn = module->getFunction("_HeapAudit.reportLeaks_u64__");
        if (!enableFn || !reportFn)
        {
            LogError("--heap-audit: HeapAudit.enable/reportLeaks were not linked - "
                     "diagnostic/heap_audit.cb failed to import; cannot instrument main.");
            return;
        }

        // Leave a self-auditing program untouched: if it already calls enable()/reportLeaks()
        // it audits at its own quiescent points, and injecting an earlier enable() would widen
        // the tracked set and break its own reportLeaks()==0 assertions. The flag is for
        // programs that do not instrument themselves.
        if (enableFn->getNumUses() > 0 || reportFn->getNumUses() > 0)
        {
            if (verbose)
                std::cout << "[verbose] --heap-audit: program already audits itself; "
                             "leaving main uninstrumented\n";
            return;
        }

        // Under -g every call in a function with debug info needs a !dbg location or the
        // verifier rejects it. Reuse a nearby instruction's location; fall back to the
        // subprogram scope (the prologue) when none is available. Empty when not -g.
        llvm::DISubprogram* sp = mainFn->getSubprogram();
        auto debugLocNear = [&](llvm::Instruction* anchor) -> llvm::DebugLoc {
            if (anchor && anchor->getDebugLoc())
                return anchor->getDebugLoc();
            if (sp)
                return llvm::DILocation::get(*context, sp->getLine(), 0, sp);
            return llvm::DebugLoc();
        };

        // enable() before the first real instruction of the entry block.
        llvm::BasicBlock& entry = mainFn->getEntryBlock();
        llvm::BasicBlock::iterator entryIp = entry.getFirstInsertionPt();
        {
            llvm::IRBuilder<> b(&entry, entryIp);
            b.SetCurrentDebugLocation(debugLocNear(entryIp != entry.end() ? &*entryIp : nullptr));
            b.CreateCall(enableFn);
        }

        // reportLeaks() right before each return - the leak count is intentionally ignored
        // (report-only); reportLeaks itself prints every live allocation to stderr.
        for (llvm::BasicBlock& bb : *mainFn)
        {
            auto* ret = llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator());
            if (!ret) continue;
            llvm::IRBuilder<> b(ret);
            b.SetCurrentDebugLocation(debugLocNear(ret));
            b.CreateCall(reportFn);
        }
    }

    // Destruct global owning values (list, dictionary, string, closure, ...) on the normal return
    // path out of main, in REVERSE definition order - the global-scope analog of a scope's
    // destructors. Emitted as a post-pass once every return in main is lowered, so it covers every
    // `return` and the implicit fall-off return alike.
    //
    // Safety notes:
    // - A moved-from global was zeroed at the move site (ApplyMoveParamTransfer stores a zeroed
    //   aggregate into the source), so its destructor is a no-op - no double free.
    // - Extern globals (not ours), thread-locals (main owns only its own copy) and core-library
    //   globals are excluded at registration time (see globalDtorOrder_). Core globals are
    //   process-lifetime infrastructure (page pools, allocator registries, their mutexes) that
    //   threads and the allocator may still touch as the process winds down.
    // - Pointer globals are skipped: a raw `T*` global has no owning-value destructor contract.
    // - An early exit()/abort() bypasses main's return and therefore skips these. Accepted: there
    //   is no atexit hook, and a hard exit leaks nothing the OS does not reclaim.
    void EmitGlobalDestructorsInMain()
    {
        llvm::Function* mainFn = module->getFunction("main");
        if (!mainFn || mainFn->isDeclaration())
            return;

        std::vector<std::pair<llvm::GlobalVariable*, llvm::Function*>> work;
        for (auto it = globalDtorOrder_.rbegin(); it != globalDtorOrder_.rend(); ++it)
        {
            auto gIt = globalNamedVariable.find(*it);
            auto tIt = globalVariableTypes.find(*it);
            if (gIt == globalNamedVariable.end() || tIt == globalVariableTypes.end()) continue;

            const TypeAndValue& tv = tIt->second;
            if (tv.Pointer || tv.IsArrayView || tv.IsInterface || tv.ConstArraySize > 0) continue;
            if (!IsOwningValueType(tv.TypeName)) continue;
            if (auto* dtor = GetOrCreateFullDestructor(tv.TypeName))
                work.emplace_back(gIt->second, dtor);
        }
        if (work.empty())
            return;

        // Under -g every call needs a !dbg location or the verifier rejects it; reuse the
        // return's own location, falling back to the subprogram scope.
        llvm::DISubprogram* sp = mainFn->getSubprogram();
        for (llvm::BasicBlock& bb : *mainFn)
        {
            auto* ret = llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator());
            if (!ret) continue;
            llvm::IRBuilder<> b(ret);
            if (ret->getDebugLoc())
                b.SetCurrentDebugLocation(ret->getDebugLoc());
            else if (sp)
                b.SetCurrentDebugLocation(llvm::DILocation::get(*context, sp->getLine(), 0, sp));
            for (const auto& [gVar, dtor] : work)
                b.CreateCall(dtor->getFunctionType(), dtor, { gVar });
        }
    }

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

        // Determine entry signature before handing the module to the JIT (which consumes it).
        // Two prototypes: int main() and int main(int argc, char** argv).
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

        // Multi-threaded --run is supported: the JITLink object-linking layer plus
        // SehRegistrationPlugin (see the LLJITBuilder setup below) register .pdata unwind tables
        // for the JIT'd image, so hardware faults on worker threads dispatch correctly (e.g. the
        // 'program' construct's SEH crash isolation). ReachesThreadSpawn() is retained for
        // diagnostics but no longer gates execution.

        // Build an LLJIT for the host. JITTargetMachineBuilder::detectHost picks up the host
        // triple, CPU, and features so the JIT'd code matches this machine.
        auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
        if (!jtmb)
        {
            LogError(std::format("--run: could not detect host machine: {}",
                                 llvm::toString(jtmb.takeError())));
            return false;
        }

#if defined(__APPLE__)
        // Force emulated TLS for the in-process JIT on Darwin. The host arm64-apple target
        // otherwise lowers `thread_local` to a native Mach-O TLV descriptor whose resolver thunk
        // is only bootstrapped by dyld (or an ORC MachOPlatform + orc_rt, which we do not link).
        // In the bare LLJIT that thunk is unresolved, so the first thread-local access (e.g.
        // __prog_tls inside printf's format path) recurses through the stub and overflows the
        // stack. Emulated TLS routes every access through __emutls_get_address instead, which we
        // define below - matching how the Windows --run path resolves thread-locals.
        jtmb->getOptions().EmulatedTLS = true;
#endif

        // On Windows, force the JITLink object-linking layer (instead of the default RuntimeDyld)
        // and attach SehRegistrationPlugin. RuntimeDyld does not register .pdata in a way the OS
        // exception dispatcher finds, so a hardware fault on a JIT'd WORKER thread (e.g. the
        // 'program' construct's SEH crash-isolation catchpad) is never dispatched and the host
        // process hard-crashes. JITLink gives the plugin the LinkGraph's .pdata so we can call
        // RtlAddFunctionTable ourselves - this is what makes multi-threaded --run unwind-safe.
        //
        // These object-flag overrides and the SEH plugin are COFF-specific. On macOS (Mach-O)
        // setAutoClaimResponsibilityForObjectSymbols makes JITLink claim the object's *undefined*
        // reference to __emutls_get_address as if the object defined it, binding its GOT slot to a
        // self-referential stub cycle instead of our absolute host hook - the emutls call then
        // recurses until the stack overflows. Mach-O/ELF need none of this, so use the default
        // LLJIT object layer there (which is already JITLink on macOS).
        llvm::orc::LLJITBuilder jitBuilder;
        jitBuilder.setJITTargetMachineBuilder(std::move(*jtmb));
#if defined(_WIN32)
        jitBuilder.setObjectLinkingLayerCreator(
            [](llvm::orc::ExecutionSession& ES, const llvm::Triple&)
                -> llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>> {
                auto ol = std::make_unique<llvm::orc::ObjectLinkingLayer>(ES);
                // Under LLJIT, the IR layer pre-computes symbol flags from the IR; JITLink
                // recomputes them from the linked COFF object and they can disagree
                // (weak/COMDAT constants), tripping ORC's "Resolving symbol with incorrect
                // flags" assert. Defer to the promised flags and claim any object-only symbols.
                ol->setOverrideObjectFlagsWithResponsibilityFlags(true);
                ol->setAutoClaimResponsibilityForObjectSymbols(true);
                ol->addPlugin(std::make_unique<cflat_jit::SehRegistrationPlugin>());
                return ol;
            });
#endif
        auto jitOrErr = jitBuilder.create();
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

        // Disable builtin libcall recognition for the in-process JIT (equivalent to -fno-builtin).
        // cflat DEFINES its own hook-aware libc functions (printf/vsnprintf/memcpy/...). LLVM's
        // libcall optimizer otherwise treats those names as the standard builtins and applies
        // folds that assume standard semantics - most damagingly the _FORTIFY fold that rewrites
        // __vsnprintf_chk(buf,size,0,size,fmt,ap) into vsnprintf(buf,size,fmt,ap). Since cflat's
        // vsnprintf routes back through __vsnprintf_chk, that fold makes vsnprintf_libc recurse
        // infinitely and overflow the stack under --run. The linked -o path does not fold these,
        // so match that behavior by marking every function "no-builtins".
        for (llvm::Function& fn : *module)
            fn.addFnAttr("no-builtins");

        // Resolve external symbols (CRT, kernel32, ws2_32) from already-loaded process symbols.
        // GlobalPrefix from the data layout keeps name mangling consistent (no prefix on win64).
        auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            jit->getDataLayout().getGlobalPrefix());
        if (!gen)
        {
            LogError(std::format("--run: failed to install process symbol resolver: {}",
                                 llvm::toString(gen.takeError())));
            return false;
        }
        jit->getMainJITDylib().addGenerator(std::move(*gen));

        // Supply __emutls_get_address - emulated TLS is how the host target lowers thread-locals.
        // runtime.cb allocator hooks are thread-local, so essentially every program needs this.
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

        char jitDiagBuf[16]; size_t jitDiagLen = 0;
        if (getenv_s(&jitDiagLen, jitDiagBuf, sizeof(jitDiagBuf), "CFLAT_JIT_DIAG") == 0 && jitDiagLen > 0)
            fprintf(stderr, "[jitdiag] invoking JIT main @%p\n", (void*)mainSym->getValue());

        // Dispatch on the entry signature detected above.
        if (mainTakesArgv)
        {
            // Build a C-style argv: argv[0] = source file name, argv[1..] = user args, argv[argc] = NULL.
            // std::string::data() is NUL-terminated (C++11+), so each element is a valid C string.
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

    void CopyCRuntimeDlls(const std::string& exePath)
    {
        namespace fs = std::filesystem;
        auto exeDir = fs::path(exePath).parent_path();
        // Track dest filenames to avoid duplicate copies when the legacy probe and vcpkg
        // authoritative list resolve to the same DLL for a given --c-lib.
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
                            if (verbose) std::cout << std::format("[verbose]   copied runtime DLL: {} -> {}\n", dll.string(), dest.string());
                            copied = true;
                        }
                        break;
                    }
                }
                if (copied) break;
            }
            if (!copied && verbose)
                std::cout << std::format("[verbose]   no runtime DLL found for {} (static lib?)\n", lib);
        }

        // vcpkg-resolved DLL paths are authoritative (from vcpkg_installed/<triplet>/bin).
        // Skip ones the legacy probe already copied.
        for (const auto& dll : vcpkgRuntimeDlls_)
        {
            fs::path src(dll);
            std::error_code ec;
            if (!fs::exists(src, ec)) continue;
            fs::path dest = exeDir / src.filename();
            if (copiedDestNames.count(dest.filename().string())) continue;
            fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
            if (!ec && verbose)
                std::cout << std::format("[verbose]   copied vcpkg DLL: {} -> {}\n", src.string(), dest.string());
        }

        // Deploy a package-nuget `pri "..."` file as <exe>.pri (WinUI MRT probes resources.pri
        // and <exe>.pri beside an unpackaged exe; <exe>.pri lets several exes share a folder).
        if (!deployPriPath_.empty())
        {
            std::error_code ec;
            fs::path src(deployPriPath_);
            fs::path dest = exeDir / (fs::path(exePath).stem().string() + ".pri");
            if (!fs::exists(src, ec))
                LogError(std::format("failed to deploy pri: source '{}' no longer exists", deployPriPath_));
            else
            {
                fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
                if (ec)
                    LogError(std::format("failed to deploy pri '{}' to '{}': {}",
                                         deployPriPath_, dest.string(), ec.message()));
                else if (verbose)
                    std::cout << std::format("[verbose]   copied pri: {} -> {}\n", src.string(), dest.string());
            }
        }
    }

    Operation ParseOperation(const std::string& operationText)
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
        // Annotations (including the WinRT/COM produce-side [winrt]/[uuid]) are declared in
        // source via `annotation X { ... }`; the registry is populated as those are scanned.
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
        std::cout << std::format("  File: {}\n", sourceFileName.empty() ? "<unknown>" : sourceFileName);
        std::cout << std::format("  Location: {}:{}\n", currentLine, currentColumn);

        if (currentFunction)
            std::cout << std::format("  Function: {}\n", currentFunction->getName().str());
        else
            std::cout << "  Function: <none>\n";

        std::cout << std::format("  Scope depth: {}\n", stackNamedVariable.size());
        if (!stackNamedVariable.empty())
        {
            const auto& top = stackNamedVariable.back();
            std::cout << "  Top scope locals:";
            for (const auto& [name, _] : top.namedVariable)
                std::cout << " " << name;
            std::cout << "\n";
        }

        std::cout << std::format("  Structs registered: {}\n", dataStructures.size());
        std::cout << std::format("  Functions registered: {}\n", functionTable.size());
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

    // Terminate all unterminated blocks in the module (lambdas mean outer function blocks may
    // also be unterminated), then pop stack frames to targetDepth without running destructors.
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
        // Return-shape of the function being emitted. Emitting a nested function (e.g. a lambda
        // invoker) mid-body calls createFunctionBlock, which overwrites this trio with the nested
        // function's shape; snapshot it here so the enclosing function's return checks are restored.
        bool returnsOwned = false;
        bool returnIsArrayView = false;
        std::string returnTypeName;
        // The enclosing function's not-yet-flushed owned temps. A nested function emitted
        // mid-body (lambda invoker, global init, program shim) runs its OWN end-of-expression
        // flushes; those must not see - and drop - the outer function's temps, which are
        // registered in outer blocks and freed by the outer flush after we return here.
        std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> pendingStringTemps;
        std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> pendingClosureTemps;
        std::vector<PendingOwnedStructTemp> pendingStructTemps;
    };

    BuilderState SaveBuilderState()
    {
        BuilderState s{ builder->saveIP(), currentFunction, currentSubprogram, builder->getCurrentDebugLocation(),
                        currentFunctionReturnsOwned, currentFunctionReturnIsArrayView, currentFunctionReturnTypeName };
        // Park the outer function's pending owned temps in the saved state and start the
        // nested emission with empty lists (see the BuilderState field comment).
        s.pendingStringTemps  = std::move(pendingOwnedStringTemps);
        s.pendingClosureTemps = std::move(pendingOwnedClosureTemps);
        s.pendingStructTemps  = std::move(pendingOwnedStructTemps);
        pendingOwnedStringTemps.clear();
        pendingOwnedClosureTemps.clear();
        pendingOwnedStructTemps.clear();
        return s;
    }

    void RestoreBuilderState(const BuilderState& state)
    {
        builder->restoreIP(state.ip);
        currentFunction = state.function;
        currentSubprogram = state.subprogram;
        builder->SetCurrentDebugLocation(state.debugLoc);
        currentFunctionReturnsOwned = state.returnsOwned;
        currentFunctionReturnIsArrayView = state.returnIsArrayView;
        currentFunctionReturnTypeName = state.returnTypeName;
        pendingOwnedStringTemps  = state.pendingStringTemps;
        pendingOwnedClosureTemps = state.pendingClosureTemps;
        pendingOwnedStructTemps  = state.pendingStructTemps;
    }

    // True if `name` is a type projected from an imported .winmd (always fully qualified) or a
    // concrete instantiation of one.
    bool IsWinrtProjectedType(const std::string& name) const
    {
        return winrtThinInterfaces_.count(name) != 0 || winrtValueStructs_.count(name) != 0
            || IsWinrtFullName(name);
    }

    void CreateInterfaceDefinition(const std::string& name, const std::vector<std::string>& parentNames,
                                   std::vector<InterfaceMethod> methods, std::vector<TypeAndValue> fields = {})
    {
        // A CFlat interface value is a fat pointer; a WinMD interface is a COM struct with an
        // lpVtbl. If a name meant both, the use site would GEP the fat pointer as a COM struct and
        // emit invalid IR. WinMD types are registered fully qualified so they can never take a bare
        // name - the only way back into that state is a `using` alias claiming this one. Reject it.
        if (auto it = typeAliases.find(name); it != typeAliases.end() && IsWinrtProjectedType(it->second))
        {
            LogError(std::format(
                "interface '{}' collides with the WinMD alias 'using {} = {};' - rename one of them "
                "(a WinMD type and a CFlat interface cannot share a name)", name, name, it->second));
            return;
        }

        // Prepend inherited methods and fields from parent interfaces (in order)
        std::vector<InterfaceMethod> inherited;
        std::vector<TypeAndValue> inheritedFields;
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
            if (auto fit = interfaceFields.find(parentName); fit != interfaceFields.end())
                for (const auto& f : fit->second)
                    inheritedFields.push_back(f);
        }
        inherited.insert(inherited.end(), methods.begin(), methods.end());
        inheritedFields.insert(inheritedFields.end(), fields.begin(), fields.end());
        interfaceTable[name] = std::move(inherited);
        interfaceFields[name] = std::move(inheritedFields);
        interfaceParents[name] = parentNames;
    }

    const std::vector<TypeAndValue>* GetInterfaceFields(const std::string& ifaceName) const
    {
        auto it = interfaceFields.find(ResolveTypeAlias(ifaceName));
        return it == interfaceFields.end() ? nullptr : &it->second;
    }

    // Index of `fieldName` within the interface's field list, or -1. The vtable slot for it is
    // 1 + methodCount + index (see GetOrCreateVTable).
    int InterfaceFieldIndex(const std::string& ifaceName, const std::string& fieldName) const
    {
        const auto* fields = GetInterfaceFields(ifaceName);
        if (fields == nullptr) return -1;
        for (int i = 0; i < (int)fields->size(); i++)
            if ((*fields)[i].VariableName == fieldName) return i;
        return -1;
    }

    size_t InterfaceFieldCount(const std::string& ifaceName) const
    {
        const auto* fields = GetInterfaceFields(ifaceName);
        return fields == nullptr ? 0 : fields->size();
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

    // A `using` alias that names a GENERIC BASE rather than a concrete type - e.g.
    // `using IReference = Windows.Foundation.IReference;`, which then lets a body write the
    // familiar `IReference<int>`. Kept out of typeAliases: a base is not a usable type on its
    // own, and ResolveTypeAlias is consulted in places where expanding one would be wrong.
    void RegisterGenericBaseAlias(const std::string& alias, const std::string& target)
    {
        genericBaseAliases_[alias] = target;
    }

    bool IsGenericBaseAlias(const std::string& name) const
    {
        return genericBaseAliases_.count(name) != 0;
    }

    std::string ResolveGenericBaseAlias(const std::string& base) const
    {
        auto it = genericBaseAliases_.find(base);
        return (it != genericBaseAliases_.end()) ? it->second : base;
    }

    // True if `fullName` is a parameterized interface template from an imported .winmd.
    bool IsWinrtGenericTemplate(const std::string& fullName) const
    {
        return winrtGenericTemplates_.count(fullName) != 0;
    }

    // True if `fullName` is a PARAMETERIZED imported type - an interface template or a generic
    // delegate (AsyncOperationCompletedHandler`1). Both only become a type once <...> is supplied,
    // so both are aliasable as a generic BASE.
    bool IsWinrtGenericBase(const std::string& fullName) const
    {
        if (IsWinrtGenericTemplate(fullName)) return true;
        for (const auto& d : winrtConsumedModel_.delegates)
            if (d.fullName == fullName) return !d.genericParams.empty();
        return false;
    }

    // The "uuid" type annotation for `name`, following the typedef-alias chain (ID3DBlob ->
    // ID3D10Blob) since a header-COM IID is registered on the struct tag, not its typedef name.
    std::string FindUuidAnnotationResolving(const std::string& name) const
    {
        std::string cur = name;
        for (int guard = 0; guard < 8; ++guard)
        {
            if (std::string u = GetTypeAnnotationArg(cur, "uuid"); !u.empty()) return u;
            std::string next = ResolveTypeAlias(cur);
            if (next == cur) break;
            cur = next;
        }
        return std::string{};
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

    // The declared parameters of an interface method (implicit 'this' excluded), or nullptr.
    // Lets the call site type a lambda-literal argument against the declared signature.
    const std::vector<TypeAndValue>* GetInterfaceMethodParams(const std::string& ifaceName,
                                                             const std::string& methodName) const
    {
        auto it = interfaceTable.find(ifaceName);
        if (it == interfaceTable.end()) return nullptr;
        for (const auto& m : it->second)
            if (m.Name == methodName) return &m.Parameters;
        return nullptr;
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
    // Env-last ABI makes this bitcast-compatible with a bare C function pointer.
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

    // Wraps a bare C function pointer as a closure {thunk, env=cfnptr}. The env slot carries
    // the real C fn ptr; the thunk reads env back, bitcasts to C signature, and tail-calls through it.
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
    // Thin sibling of WrapBareValueAsFatStruct: a thin `function<T>` value is just the
    // bare function pointer, bitcast to the declared thin signature R(*)(Args). No shim,
    // no env, no fat struct. `fpTV` carries the target thin signature.
    llvm::Value* MakeThinFnPtrValue(llvm::Value* fn, const TypeAndValue& fpTV)
    {
        return builder->CreateBitCast(fn, BuildThinFnPtrType(fpTV), "thinfn");
    }

    // Extract the bare code pointer from a closure fat-struct value and present it as a thin
    // `function<T>`. Caller must ensure the closure does NOT capture (env null). Relies on the
    // trailing-env-ignored ABI tolerance (caller-cleanup conventions: Win x64, cdecl).
    llvm::Value* CoerceClosureFatToThin(llvm::Value* fatVal, const TypeAndValue& thinTV)
    {
        auto* code = builder->CreateExtractValue(fatVal, {0u}, "clo_code");
        return builder->CreateBitCast(code, BuildThinFnPtrType(thinTV), "thinfn");
    }

    // True iff a fat closure value is PROVABLY non-capturing: its env field is a compile-time
    // null constant (a non-capturing lambda literal or a named-fn wrap). A capturing lambda or a
    // stored/loaded Lambda<> value is not provable here and returns false. This is the static gate
    // for narrowing a fat closure to a thin `function<T>` - the same env-null signal the extern
    // function-pointer argument path uses.
    bool ClosureIsStaticallyNonCapturing(llvm::Value* fatVal)
    {
        auto* envField = builder->CreateExtractValue(fatVal, {1u}, "closure_env_probe");
        return llvm::isa<llvm::ConstantPointerNull>(envField);
    }

    // Diagnostic for an illegal fat-closure -> thin `function<T>` narrowing. With capture names
    // (a capturing lambda literal) it lists them; with none (a stored Lambda<> value) it points at
    // .toFunction(). Shared by the declaration, assignment, and return coercion sites.
    std::string DescribeCapturingClosureToThin(const std::vector<std::string>& captureNames) const
    {
        if (captureNames.empty())
            return "a closure that may carry captured state cannot become a C function pointer "
                   "'function<T>'; call .toFunction() instead (it returns the code pointer when the "
                   "closure does not capture, or null when it does), or use Lambda<T> to keep the captures.";
        const size_t count = captureNames.size();
        const size_t shown = count < 5 ? count : 5;
        std::string list;
        for (size_t k = 0; k < shown; k++) { if (k != 0) list += ", "; list += captureNames[k]; }
        std::string more = count > shown ? std::format(", ... (and {} more)", count - shown) : "";
        return std::format(
            "a capturing lambda cannot become a C function pointer 'function<T>': it captured {} {} "
            "[{}{}]. Use Lambda<T> to keep the captures, or call .toFunction() (which returns null when "
            "the closure captures).",
            count, (count == 1 ? "variable" : "variables"), list, more);
    }

    // Lambda<T>.toFunction(): lower a fat closure value to a thin `function<T>` C pointer.
    // env == null (non-capturing) -> the bare code ptr; env != null (captures) -> a null thin
    // pointer. The env==null test is the entire contract: a capturing closure has no C-ABI
    // representation, so the lowering fails closed and the caller must null-check. No trap, no
    // diagnostic. A non-capturing closure's code ignores the trailing env arg under the
    // caller-cleanup ABI tolerance (Win x64, cdecl), so the returned bare ptr is C-callable.
    llvm::Value* EmitFuncToFunctionLowering(llvm::Value* fatVal, const TypeAndValue& thinTV)
    {
        auto* code     = builder->CreateExtractValue(fatVal, {0u}, "clo_code");
        auto* env      = builder->CreateExtractValue(fatVal, {1u}, "clo_env");
        auto* thinTy   = llvm::cast<llvm::PointerType>(BuildThinFnPtrType(thinTV));
        auto* codeThin = builder->CreateBitCast(code, thinTy, "thinfn");
        auto* nullThin = llvm::ConstantPointerNull::get(thinTy);
        auto* i8PtrTy  = builder->getInt8Ty()->getPointerTo();
        auto* envNull  = builder->CreateICmpEQ(env, llvm::ConstantPointerNull::get(i8PtrTy), "env_isnull");
        return builder->CreateSelect(envNull, codeThin, nullThin, "tofn");
    }

    // Coerce a returned value to the declared function-pointer return type. Handles a named
    // function, a thin function<> value, and a fat closure value in either direction.
    llvm::Value* CoerceToFuncPtrReturn(llvm::Value* val, const TypeAndValue& retTV)
    {
        bool valIsStruct = val->getType()->isStructTy();   // fat closure value
        if (retTV.IsThinFnPtr())
        {
            if (auto* fn = llvm::dyn_cast<llvm::Function>(val)) return MakeThinFnPtrValue(fn, retTV);
            if (valIsStruct)
            {
                // Fat closure returned as thin function<T>: only safe when provably non-capturing.
                // A capturing closure has no C-ABI representation, so reject it instead of dropping
                // the env (which yields a thin ptr that reads freed/absent captures when called).
                if (ClosureIsStaticallyNonCapturing(val)) return CoerceClosureFatToThin(val, retTV);
                LogError(DescribeCapturingClosureToThin({}));
                return llvm::UndefValue::get(BuildThinFnPtrType(retTV));
            }
            return builder->CreateBitCast(val, BuildThinFnPtrType(retTV), "thinret");
        }
        if (auto* fn = llvm::dyn_cast<llvm::Function>(val)) return WrapBareValueAsFatStruct(fn);
        if (!valIsStruct && val->getType()->isPointerTy()) return WidenThinToFat(val);   // thin -> fat
        return val;   // already a fat closure value
    }

    // Widen a thin `function<T>` (bare C ptr) to a fat `Lambda<T>` value {code, null}. No thunk:
    // the thin ptr goes straight into the code slot, env is null. When the resulting Lambda is
    // invoked (env-last ABI), the trailing null env arg is harmlessly ignored by the bare C
    // function (caller-cleanup tolerance). The null env makes .toFunction() round-trip it back.
    llvm::Value* WidenThinToFat(llvm::Value* thinPtr)
    {
        auto* i8PtrTy   = builder->getInt8Ty()->getPointerTo();
        auto* codeI8    = builder->CreateBitCast(thinPtr, i8PtrTy, "thin_code_i8");
        auto* nullEnv   = llvm::ConstantPointerNull::get(i8PtrTy);
        auto* closureTy = GetClosureFatPtrType();
        llvm::Value* fat = llvm::UndefValue::get(closureTy);
        fat = builder->CreateInsertValue(fat, codeI8,  {0u});
        fat = builder->CreateInsertValue(fat, nullEnv, {1u});
        return fat;
    }

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

    // Appends one vtable slot per interface field: the implementor's BYTE OFFSET of that field,
    // encoded as inttoptr(offset). Reads/writes through the interface GEP by it, so no thunk is
    // needed and the field stays an lvalue. A missing/mismatched field was already reported by
    // VerifyInterfaceFields at the implementor's definition; emit a null slot and keep going.
    void AppendInterfaceFieldOffsetSlots(const std::string& structName, const std::string& ifaceName,
                                         llvm::StructType* structTy,
                                         const std::vector<DeclTypeAndValue>& structFields,
                                         std::vector<llvm::Constant*>& entries)
    {
        const auto* ifields = GetInterfaceFields(ifaceName);
        if (ifields == nullptr || ifields->empty()) return;

        auto ptrTy = builder->getInt8Ty()->getPointerTo();
        const llvm::StructLayout* layout = (structTy != nullptr && !structTy->isOpaque())
            ? module->getDataLayout().getStructLayout(structTy) : nullptr;

        for (const auto& f : *ifields)
        {
            size_t idx = 0;
            for (; idx < structFields.size(); idx++)
                if (structFields[idx].VariableName == f.VariableName) break;

            if (idx >= structFields.size() || layout == nullptr)
            {
                entries.push_back(llvm::ConstantPointerNull::get(ptrTy));
                continue;
            }

            uint64_t offset = layout->getElementOffset((unsigned)idx);
            entries.push_back(llvm::ConstantExpr::getIntToPtr(builder->getInt64(offset), ptrTy));
        }
    }

    // The declared type of an interface field / implementor field, for diagnostics.
    static std::string InterfaceFieldTypeText(const TypeAndValue& f)
    {
        std::string s = f.TypeName;
        if (f.Pointer) s += "*";
        return s;
    }

    // Every field an interface declares must exist on the implementor with the same name and
    // type - the vtable slot carries that field's byte offset. Runs EAGERLY, at the class or
    // program definition, so the error points at the implementor and not at some later boxing
    // site (AppendInterfaceFieldOffsetSlots then trusts the layout it is handed).
    void VerifyInterfaceFields(const std::string& implName, const std::string& ifaceName,
                               const std::vector<DeclTypeAndValue>& implFields)
    {
        const auto* ifields = GetInterfaceFields(ifaceName);
        if (ifields == nullptr) return;

        for (const auto& f : *ifields)
        {
            const DeclTypeAndValue* impl = nullptr;
            for (const auto& sf : implFields)
                if (sf.VariableName == f.VariableName) { impl = &sf; break; }

            if (impl == nullptr)
            {
                LogError(std::format(
                    "class '{}' does not implement interface field '{}::{}' (expected type '{}')",
                    implName, ifaceName, f.VariableName, InterfaceFieldTypeText(f)));
                continue;
            }
            if (impl->TypeName != f.TypeName || impl->Pointer != f.Pointer
                || impl->IsInterface != f.IsInterface)
            {
                LogError(std::format(
                    "class '{}' field '{}' has type '{}' but interface '{}' declares it as '{}'",
                    implName, f.VariableName, InterfaceFieldTypeText(*impl), ifaceName,
                    InterfaceFieldTypeText(f)));
            }
        }
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

        AppendInterfaceFieldOffsetSlots(structName, ifaceName, pd.StructType, pd.ConfigFields, entries);

        // Trailing slot: full concrete destructor (or null) for delete-through-interface.
        // Layout is [typedesc, method0..N-1, fieldOff0..M-1, fullDtor].
        if (auto* dtor = GetOrCreateFullDestructor(structName))
            entries.push_back(llvm::ConstantExpr::getBitCast(dtor, ptrTy));
        else
            entries.push_back(llvm::ConstantPointerNull::get(ptrTy));

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
                // Match by struct this-pointer and remaining params to distinguish overloads.
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

        AppendInterfaceFieldOffsetSlots(structName, ifaceName, sd.StructType, sd.StructFields, entries);

        // Trailing slot: full concrete destructor (or null) for delete-through-interface.
        // Layout is [typedesc, method0..N-1, fieldOff0..M-1, fullDtor].
        if (auto* dtor = GetOrCreateFullDestructor(structName))
            entries.push_back(llvm::ConstantExpr::getBitCast(dtor, ptrTy));
        else
            entries.push_back(llvm::ConstantPointerNull::get(ptrTy));

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

    // Coerce ONE call argument to an INTERFACE-typed parameter: box a concrete class/struct
    // (value or pointer) into a fat pointer, or upcast an already-boxed interface value.
    // Shared by the indirect (function<>/Lambda) call path so it cannot drift from the
    // direct-call path's inline boxing. Returns `val` unchanged when nothing applies.
    llvm::Value* CoerceArgToInterface(const NamedVariable& arg, llvm::Value* val,
                                      const std::string& ifaceName, const std::string& calleeDesc)
    {
        if (val == nullptr || ifaceName.empty() || !IsInterfaceType(ifaceName)) return val;
        if (arg.TypeAndValue.IsInterface)
            return ReboxInterfaceIfNeeded(val, arg.TypeAndValue.TypeName, ifaceName);
        if (val->getType() == GetFatPtrType()) return val;   // already a fat pointer

        std::string structName = arg.TypeAndValue.TypeName;
        if (structName.empty() && arg.BaseType)
            if (auto* st = llvm::dyn_cast<llvm::StructType>(arg.BaseType))
                structName = st->getName().str();

        if (structName.empty() || !StructImplementsInterface(structName, ifaceName))
        {
            LogError(std::format(
                "cannot pass '{}' as interface parameter '{}' of {} - it does not implement '{}'",
                structName.empty() ? "<unknown>" : structName, ifaceName, calleeDesc, ifaceName));
            return val;
        }

        auto* vtable = GetOrCreateVTable(structName, ifaceName);
        llvm::Value* dataPtr = nullptr;
        if (arg.TypeAndValue.Pointer)
            dataPtr = arg.Primary != nullptr ? arg.Primary : CreateLoad(arg.Storage);
        else if (arg.Storage != nullptr)
            dataPtr = arg.Storage;
        else
        {
            auto* structTy = arg.BaseType ? arg.BaseType : GetType(arg.TypeAndValue);
            if (structTy == nullptr || structTy->isVoidTy() || arg.Primary == nullptr)
            {
                LogError(std::format(
                    "argument for interface parameter '{}' of {} has no resolved storage",
                    ifaceName, calleeDesc));
                return val;
            }
            auto* tempAlloca = AllocaAtEntry(structTy, nullptr);
            builder->CreateStore(arg.Primary, tempAlloca);
            dataPtr = tempAlloca;
        }
        return BuildInterfaceFatValue(vtable, dataPtr);
    }

    // Rebuild only when the source and destination interfaces actually differ (the common
    // same-interface case stays a plain by-value copy, with no if-chain emitted).
    llvm::Value* ReboxInterfaceIfNeeded(llvm::Value* fatVal, const std::string& srcIface,
                                        const std::string& dstIface)
    {
        if (fatVal == nullptr || fatVal->getType() != GetFatPtrType()) return fatVal;
        if (srcIface.empty() || dstIface.empty() || srcIface == dstIface) return fatVal;
        if (!IsInterfaceType(srcIface) || !IsInterfaceType(dstIface)) return fatVal;
        return RebuildInterfaceFatValue(fatVal, dstIface);
    }

    // Re-box an interface value as a DIFFERENT interface (a derived-to-parent upcast, e.g.
    // IButton -> IElement). A derived vtable is NOT layout-compatible with its parent's once
    // field-offset slots exist, so the fat pointer is rebuilt: match the runtime typedesc
    // against each implementor of dstIface and pick that implementor's dstIface vtable.
    // No match yields a zeroed fat pointer, exactly like a failed `as <Interface>`.
    llvm::Value* RebuildInterfaceFatValue(llvm::Value* fatVal, const std::string& dstIface)
    {
        auto fatTy = GetFatPtrType();
        auto ptrTy = builder->getInt8Ty()->getPointerTo();

        llvm::Value* dataPtr  = builder->CreateExtractValue(fatVal, { 1u }, "up_data");
        llvm::Value* vtablePtr = builder->CreateExtractValue(fatVal, { 0u }, "up_vtable");

        auto* resultAlloca = CreateAlloca(fatTy);
        builder->CreateStore(llvm::ConstantAggregateZero::get(fatTy), resultAlloca);
        auto* afterBlock = CreateBasicBlock("upcast_after");

        // A null (failed-cast) source has no vtable to read a typedesc from: stay zeroed.
        auto* liveBlock = CreateBasicBlock("upcast_live");
        builder->CreateCondBr(
            builder->CreateICmpEQ(vtablePtr, llvm::ConstantPointerNull::get(ptrTy)), afterBlock, liveBlock);
        SwitchToBlock(liveBlock);

        llvm::Value* loadedDesc = builder->CreateLoad(ptrTy,
            builder->CreateGEP(ptrTy, vtablePtr, builder->getInt32(0)), "up_typedesc");

        auto emitCase = [&](const std::string& implName, llvm::GlobalVariable* typeDesc)
        {
            auto* matchBlock = CreateBasicBlock("upcast_match");
            auto* nextBlock  = CreateBasicBlock("upcast_next");
            builder->CreateCondBr(builder->CreateICmpEQ(loadedDesc, typeDesc), matchBlock, nextBlock);
            SwitchToBlock(matchBlock);
            builder->CreateStore(BuildInterfaceFatValue(GetOrCreateVTable(implName, dstIface), dataPtr), resultAlloca);
            builder->CreateBr(afterBlock);
            SwitchToBlock(nextBlock);
        };

        for (auto& [sName, sd] : dataStructures)
            if (sd.typeDescriptor && StructImplementsInterface(sName, dstIface))
                emitCase(sName, sd.typeDescriptor);
        for (auto& [pName, pd] : programTable)
            if (pd.typeDescriptor && StructImplementsInterface(pName, dstIface))
                emitCase(pName, pd.typeDescriptor);

        builder->CreateBr(afterBlock);
        SwitchToBlock(afterBlock);
        return builder->CreateLoad(fatTy, resultAlloca);
    }

    // ===== WinRT / COM produce-side codegen (see internal/plan/winmd-projection.md) =====
    // A [winrt] class lowers to a thin COM object: a struct whose first field is a vtable
    // pointer, followed by a refcount and the user fields. The vtable holds IUnknown +
    // IInspectable slots plus the interface methods (raw 1:1 ABI for this milestone). All
    // runtime functions (QueryInterface/AddRef/Release/thunks) are generated here.

    bool IsWinrtClass(const std::string& name) const { return winrtClasses.count(name) > 0; }

    // Parse a canonical GUID string into its 16-byte little-endian memory image (matching the
    // in-memory layout of core/guid.cb's Guid: u32 Data1, u16 Data2, u16 Data3, u8[8] Data4).
    // Non-hex characters (dashes, braces) are skipped. Returns false if fewer than 32 nibbles.
    static bool ParseUuidToBytes(const std::string& text, uint8_t out[16])
    {
        uint8_t textBytes[16] = {};
        int count = 0;
        for (char c : text)
        {
            int v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else continue;
            if (count >= 32) break;
            int bi = count / 2;
            if ((count & 1) == 0) textBytes[bi] = (uint8_t)(v << 4);
            else                  textBytes[bi] = (uint8_t)(textBytes[bi] | v);
            count++;
        }
        if (count < 32) return false;
        // Data1 (u32) and Data2/Data3 (u16) are stored little-endian; Data4 is raw byte order.
        out[0] = textBytes[3]; out[1] = textBytes[2]; out[2] = textBytes[1]; out[3] = textBytes[0];
        out[4] = textBytes[5]; out[5] = textBytes[4];
        out[6] = textBytes[7]; out[7] = textBytes[6];
        for (int i = 8; i < 16; i++) out[i] = textBytes[i];
        return true;
    }

    // Resolve `name` (a mangled generic instance like "Windows.Foundation.IReference__i32", a
    // [uuid] interface, or an imported non-generic interface FULL name) to a 16-byte GUID/PIID for
    // `iidof(...)` builtin. Returns nullptr if no IID is known. The result is a REFIID-shaped
    // pointer suitable for QueryInterface.
    llvm::GlobalVariable* EmitIidGlobalFor(const std::string& name)
    {
        if (auto it = winrtInstanceIid_.find(name); it != winrtInstanceIid_.end())
            return EmitGuidGlobal(it->second.data());
        uint8_t bytes[16];
        // A header-COM interface is often a typedef alias of its real tag (ID3DBlob -> ID3D10Blob);
        // the uuid annotation lives on the tag, so resolve the alias chain before giving up.
        if (std::string u = FindUuidAnnotationResolving(name); !u.empty() && ParseUuidToBytes(u, bytes))
            return EmitGuidGlobal(bytes);
        for (const auto& i : winrtConsumedModel_.interfaces)
            if (i.fullName == name && !i.iid.empty() && ParseUuidToBytes(i.iid, bytes))
                return EmitGuidGlobal(bytes);
        return nullptr;
    }

    // Emit (or reuse) an internal-linkage [16 x i8] constant holding a GUID's memory image.
    // Deduplicated by content so IID_IUnknown/IID_IInspectable are shared across classes.
    llvm::GlobalVariable* EmitGuidGlobal(const uint8_t bytes[16])
    {
        std::string name = "__winrt_iid_";
        for (int i = 0; i < 16; i++)
        {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", bytes[i]);
            name += buf;
        }
        if (auto* existing = module->getNamedGlobal(name)) return existing;

        std::vector<llvm::Constant*> elems;
        for (int i = 0; i < 16; i++) elems.push_back(builder->getInt8(bytes[i]));
        auto* arrTy = llvm::ArrayType::get(builder->getInt8Ty(), 16);
        auto* init = llvm::ConstantArray::get(arrTy, elems);
        return new llvm::GlobalVariable(*module, arrTy, true,
            llvm::GlobalValue::InternalLinkage, init, name);
    }

    // Build a single vtable slot: a thin `function<Ret(params)>` field with the given name.
    static DeclTypeAndValue MakeWinrtSlot(const std::string& name, const std::string& retType,
        bool retPtr, std::vector<TypeAndValue::FuncPtrParam> params)
    {
        DeclTypeAndValue f;
        f.VariableName = name;
        f.TypeName = "__c_fn_ptr";
        f.IsFunctionPointer = true;
        f.FuncPtrReturnTypeName = retType;
        f.FuncPtrReturnPointer = retPtr;
        f.FuncPtrParams = std::move(params);
        return f;
    }

    // Create the COM vtable struct for className implementing ifaceName. Flat IInspectable
    // layout: [QueryInterface, AddRef, Release, GetIids, GetRuntimeClassName, GetTrustLevel,
    // <interface methods in slot order>]. Returns the registered struct name.
    std::string CreateWinrtVtableStruct(const std::string& className, const std::string& ifaceName)
    {
        std::string vtblName = className + "__" + ifaceName + "_comvtbl";
        if (dataStructures.count(vtblName)) return vtblName;

        auto vp = []() { TypeAndValue::FuncPtrParam p; p.TypeName = "void"; p.Pointer = true; return p; };
        std::vector<DeclTypeAndValue> slots;
        slots.push_back(MakeWinrtSlot("QueryInterface", "i32", false, { vp(), vp(), vp() }));
        slots.push_back(MakeWinrtSlot("AddRef", "u32", false, { vp() }));
        slots.push_back(MakeWinrtSlot("Release", "u32", false, { vp() }));
        slots.push_back(MakeWinrtSlot("GetIids", "i32", false, { vp(), vp(), vp() }));
        slots.push_back(MakeWinrtSlot("GetRuntimeClassName", "i32", false, { vp(), vp() }));
        slots.push_back(MakeWinrtSlot("GetTrustLevel", "i32", false, { vp(), vp() }));

        auto it = interfaceTable.find(ifaceName);
        if (it != interfaceTable.end())
        {
            for (const auto& m : it->second)
            {
                std::vector<TypeAndValue::FuncPtrParam> params = { vp() };  // this
                for (const auto& mp : m.Parameters)
                {
                    TypeAndValue::FuncPtrParam p;
                    p.TypeName = mp.TypeName;
                    p.Pointer = mp.Pointer;
                    params.push_back(p);
                }
                // WinRT ABI: the slot returns HRESULT (i32); a non-void logical return is passed
                // back through a trailing [out,retval] pointer (opaque void* in the slot type).
                bool voidRet = (m.ReturnType.TypeName == "void" && !m.ReturnType.Pointer);
                if (!voidRet) params.push_back(vp());
                slots.push_back(MakeWinrtSlot(m.Name, "i32", false, params));
            }
        }

        CreateStructType(vtblName, slots);
        return vtblName;
    }

    // Find the user member function implementing interface method m on className. Mirrors the
    // overload match in GetOrCreateVTable (this-pointer + remaining params). Returns the full
    // symbol so the HRESULT-ABI thunk can see the impl's return type (plain T vs HResult<T>).
    // The impl's return type is NOT part of the match (an impl may return T or HResult<T> for
    // the same logical interface method).
    const FunctionSymbol* FindWinrtMethod(const std::string& className, const InterfaceMethod& m)
    {
        auto it = functionTable.find(m.Name);
        if (it == functionTable.end()) return nullptr;
        size_t expected = 1 + m.Parameters.size();
        for (const auto& sym : it->second)
        {
            if (sym.Parameters.size() != expected) continue;
            if (sym.Parameters[0].TypeName != className || !sym.Parameters[0].Pointer) continue;
            bool ok = true;
            for (size_t pi = 0; pi < m.Parameters.size(); pi++)
                if (sym.Parameters[1 + pi].TypeName != m.Parameters[pi].TypeName) { ok = false; break; }
            if (ok) return &sym;
        }
        return nullptr;
    }

    // True if a type name is a monomorphized HResult<T> (the fallible method return form).
    static bool IsHResultType(const std::string& typeName)
    {
        return typeName.rfind("HResult__", 0) == 0;
    }

    // Store &g_vtbl into lpVtbl (field 0) and 1 into __refcount (field 1) of a freshly
    // allocated [winrt] object. Called by `new` after the constructor runs.
    void WireWinrtObject(llvm::Value* objPtr, const std::string& className)
    {
        auto wi = winrtClasses.at(className);
        auto* objTy = dataStructures[className].StructType;
        auto* vtblFieldPtr = builder->CreateStructGEP(objTy, objPtr, 0, "lpVtbl");
        auto* vtblPtrTy = objTy->getStructElementType(0);
        builder->CreateStore(builder->CreateBitCast(wi.VtableInstance, vtblPtrTy), vtblFieldPtr);
        auto* rcFieldPtr = builder->CreateStructGEP(objTy, objPtr, 1, "__refcount");
        builder->CreateStore(builder->getInt32(1), rcFieldPtr);
    }

    // If `slotName` names a vtable slot of the [winrt] class `className` (a generated
    // QueryInterface/AddRef/Release/IInspectable method or an interface method), return its
    // field descriptor; else nullptr. Lets `recv->Slot(args)` be recognized as a COM call.
    const DeclTypeAndValue* GetWinrtSlot(const std::string& className, const std::string& slotName) const
    {
        auto wi = winrtClasses.find(className);
        if (wi == winrtClasses.end()) return nullptr;
        std::string vtblName = className + "__" + wi->second.InterfaceName + "_comvtbl";
        auto ds = dataStructures.find(vtblName);
        if (ds == dataStructures.end()) return nullptr;
        for (const auto& f : ds->second.StructFields)
            if (f.VariableName == slotName) return &f;
        return nullptr;
    }

    // Emit a COM dispatch `recv->lpVtbl->slot(args)`: load the vtable pointer (field 0), load the
    // named slot, and indirect-call it. argVals[0] is the receiver `this` (also used to reach the
    // vtable); argVals[1..] are the user arguments. `outResultType`/`outResultPtr` receive the
    // CFlat type of the produced value so the caller can type the result.
    //
    // Slots 0-5 are the IUnknown+IInspectable infrastructure (QI/AddRef/Release/...), dispatched
    // raw. Interface-method slots (>=6) use the HRESULT ABI `i32(this, ...in, RetType* retval)`:
    //  - void logical return -> the call yields the raw HRESULT (i32).
    //  - non-void -> allocate a retval out-slot, call, and package {hr, *retval} into an
    //    HResult<T> (primed at [winrt] class parse time); result type is "HResult__<T>".
    llvm::Value* EmitWinrtSlotCall(const std::string& className, const std::string& slotName,
        const std::vector<llvm::Value*>& argVals, std::string& outResultType, bool& outResultPtr)
    {
        auto wi = winrtClasses.at(className);
        auto* objTy = dataStructures[className].StructType;
        auto* vtblTy = wi.VtableType;
        std::string vtblName = className + "__" + wi.InterfaceName + "_comvtbl";
        auto& vtblSD = dataStructures[vtblName];

        unsigned slotIdx = 0;
        const DeclTypeAndValue* slot = nullptr;
        for (unsigned i = 0; i < vtblSD.StructFields.size(); i++)
            if (vtblSD.StructFields[i].VariableName == slotName) { slotIdx = i; slot = &vtblSD.StructFields[i]; break; }
        if (!slot || argVals.empty())
        {
            LogError(std::format("EmitWinrtSlotCall: bad slot '{}' on '{}'", slotName, className));
            return nullptr;
        }

        // A value-returning interface method: build the HResult<T> wrapper around the ABI call.
        const InterfaceMethod* im = nullptr;
        if (slotIdx >= 6)
            if (auto it = interfaceTable.find(wi.InterfaceName); it != interfaceTable.end())
                for (const auto& m : it->second) if (m.Name == slotName) { im = &m; break; }
        bool nonVoidIface = im && !(im->ReturnType.TypeName == "void" && !im->ReturnType.Pointer);

        llvm::Value* retvalAlloca = nullptr;
        std::string hresultTypeName;
        if (nonVoidIface)
        {
            auto rt = winrtSlotHResultType_.find(className + "::" + slotName);
            if (rt == winrtSlotHResultType_.end() || !dataStructures.count(rt->second))
            {
                LogError(std::format("[winrt] '{}::{}' sugar needs HResult<{}> instantiated "
                    "(import \"com.cb\")", className, slotName, im->ReturnType.TypeName));
                return nullptr;
            }
            hresultTypeName = rt->second;
            auto* retElemTy = GetType(im->ReturnType);
            retvalAlloca = AllocaAtEntry(retElemTy, nullptr, "winrt.retval");
        }

        std::vector<llvm::Value*> callArgs = argVals;
        if (nonVoidIface)
            callArgs.push_back(builder->CreateBitCast(retvalAlloca, builder->getInt8Ty()->getPointerTo()));

        auto* objPtr = builder->CreateBitCast(argVals[0], objTy->getPointerTo());
        auto* vtblFieldPtr = builder->CreateStructGEP(objTy, objPtr, 0);
        auto* vtblPtr = builder->CreateLoad(vtblTy->getPointerTo(), vtblFieldPtr);
        auto* slotPtr = builder->CreateStructGEP(vtblTy, vtblPtr, slotIdx);
        auto* fnPtr = builder->CreateLoad(BuildThinFnPtrType(*slot), slotPtr);
        llvm::Value* callRes = CreateIndirectCall(*slot, fnPtr, callArgs);

        if (!nonVoidIface)
        {
            // Infra method or void interface method: the raw return (slot's declared type).
            outResultType = slot->FuncPtrReturnTypeName;
            outResultPtr = slot->FuncPtrReturnPointer;
            return callRes;
        }

        // Package {hr = callRes, value = *retval} into the primed HResult<T>.
        auto* hrTy = dataStructures[hresultTypeName].StructType;
        auto* hrAlloca = AllocaAtEntry(hrTy, nullptr, "winrt.hr");
        builder->CreateStore(callRes, builder->CreateStructGEP(hrTy, hrAlloca, 0));
        auto* loadedVal = builder->CreateLoad(GetType(im->ReturnType), retvalAlloca);
        builder->CreateStore(loadedVal, builder->CreateStructGEP(hrTy, hrAlloca, 1));
        outResultType = hresultTypeName;
        outResultPtr = false;
        return builder->CreateLoad(hrTy, hrAlloca);
    }

    // Emit all runtime functions and the static vtable instance for a [winrt] class. Must run
    // AFTER the object struct body, the vtable struct, and the user member functions exist.
    void EmitWinrtRuntime(const std::string& className, const std::string& ifaceName,
        const std::string& vtblName)
    {
        auto* objTy = dataStructures[className].StructType;
        auto* vtblTy = dataStructures[vtblName].StructType;
        auto* i8PtrTy = builder->getInt8Ty()->getPointerTo();
        auto* i32Ty = builder->getInt32Ty();

        // GUID constants: IUnknown, IInspectable, IAgileObject (every free-threaded WinRT object
        // answers to it), and this class's interface IID.
        uint8_t unkB[16], inspB[16], agileB[16], ifaceB[16];
        ParseUuidToBytes("00000000-0000-0000-C000-000000000046", unkB);
        ParseUuidToBytes("AF86E2E0-B12D-4C6A-9C5A-D7AA65101E90", inspB);
        ParseUuidToBytes("94EA2B94-E9CC-49E0-C0FF-EE64CA8F5B90", agileB);
        std::string ifaceUuid = GetTypeAnnotationArg(ifaceName, "uuid");
        bool haveIfaceIid = ParseUuidToBytes(ifaceUuid, ifaceB);
        auto* gUnk = EmitGuidGlobal(unkB);
        auto* gInsp = EmitGuidGlobal(inspB);
        auto* gAgile = EmitGuidGlobal(agileB);
        auto* gIface = haveIfaceIid ? EmitGuidGlobal(ifaceB) : nullptr;

        auto makeFn = [&](const std::string& nm, llvm::FunctionType* ty) {
            return llvm::Function::Create(ty, llvm::Function::InternalLinkage,
                "__winrt_" + className + "_" + nm, module.get());
        };
        std::string prefix = "__winrt_" + className + "_";

        // QueryInterface(i8* self, i8* riid, i8* ppv) -> i32
        auto* qiTy = llvm::FunctionType::get(i32Ty, { i8PtrTy, i8PtrTy, i8PtrTy }, false);
        auto* qiFn = makeFn("QueryInterface", qiTy);
        {
            auto* entry = llvm::BasicBlock::Create(*context, "entry", qiFn);
            auto* matchBB = llvm::BasicBlock::Create(*context, "match", qiFn);
            auto* noBB = llvm::BasicBlock::Create(*context, "nomatch", qiFn);
            llvm::IRBuilder<> b(entry);
            auto* self = qiFn->getArg(0);
            auto* riid = qiFn->getArg(1);
            auto* ppv = qiFn->getArg(2);

            // Compare two 8-byte halves of the 16-byte GUID (unaligned loads are fine on x64).
            auto guidEq = [&](llvm::Value* a, llvm::GlobalVariable* g) -> llvm::Value* {
                auto* i64Ty = b.getInt64Ty();
                auto* i64PtrTy = i64Ty->getPointerTo();
                auto* aLo = b.CreateAlignedLoad(i64Ty, b.CreateBitCast(a, i64PtrTy), llvm::MaybeAlign(1));
                auto* aHiPtr = b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), a, 8);
                auto* aHi = b.CreateAlignedLoad(i64Ty, b.CreateBitCast(aHiPtr, i64PtrTy), llvm::MaybeAlign(1));
                auto* gI8 = b.CreateBitCast(g, i8PtrTy);
                auto* bLo = b.CreateAlignedLoad(i64Ty, b.CreateBitCast(gI8, i64PtrTy), llvm::MaybeAlign(1));
                auto* bHiPtr = b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), gI8, 8);
                auto* bHi = b.CreateAlignedLoad(i64Ty, b.CreateBitCast(bHiPtr, i64PtrTy), llvm::MaybeAlign(1));
                return b.CreateAnd(b.CreateICmpEQ(aLo, bLo), b.CreateICmpEQ(aHi, bHi));
            };

            llvm::Value* match = b.CreateOr(guidEq(riid, gUnk), guidEq(riid, gInsp));
            match = b.CreateOr(match, guidEq(riid, gAgile));
            if (gIface) match = b.CreateOr(match, guidEq(riid, gIface));
            b.CreateCondBr(match, matchBB, noBB);

            b.SetInsertPoint(matchBB);
            auto* objP = b.CreateBitCast(self, objTy->getPointerTo());
            auto* rcP = b.CreateStructGEP(objTy, objP, 1);
            b.CreateAtomicRMW(llvm::AtomicRMWInst::Add, rcP, b.getInt32(1),
                llvm::MaybeAlign(), llvm::AtomicOrdering::Monotonic);
            b.CreateStore(self, b.CreateBitCast(ppv, i8PtrTy->getPointerTo()));
            b.CreateRet(b.getInt32(0));

            b.SetInsertPoint(noBB);
            b.CreateStore(llvm::ConstantPointerNull::get(i8PtrTy),
                b.CreateBitCast(ppv, i8PtrTy->getPointerTo()));
            b.CreateRet(b.getInt32((uint32_t)0x80004002));  // E_NOINTERFACE
        }

        // AddRef(i8* self) -> i32
        auto* refTy = llvm::FunctionType::get(i32Ty, { i8PtrTy }, false);
        auto* addRefFn = makeFn("AddRef", refTy);
        {
            auto* entry = llvm::BasicBlock::Create(*context, "entry", addRefFn);
            llvm::IRBuilder<> b(entry);
            auto* objP = b.CreateBitCast(addRefFn->getArg(0), objTy->getPointerTo());
            auto* rcP = b.CreateStructGEP(objTy, objP, 1);
            auto* old = b.CreateAtomicRMW(llvm::AtomicRMWInst::Add, rcP, b.getInt32(1),
                llvm::MaybeAlign(), llvm::AtomicOrdering::Monotonic);
            b.CreateRet(b.CreateAdd(old, b.getInt32(1)));
        }

        // Release(i8* self) -> i32: atomic dec; free at zero (full dtor + operator delete).
        auto* releaseFn = makeFn("Release", refTy);
        {
            auto* entry = llvm::BasicBlock::Create(*context, "entry", releaseFn);
            auto* freeBB = llvm::BasicBlock::Create(*context, "free", releaseFn);
            auto* doneBB = llvm::BasicBlock::Create(*context, "done", releaseFn);
            llvm::IRBuilder<> b(entry);
            auto* self = releaseFn->getArg(0);
            auto* objP = b.CreateBitCast(self, objTy->getPointerTo());
            auto* rcP = b.CreateStructGEP(objTy, objP, 1);
            auto* old = b.CreateAtomicRMW(llvm::AtomicRMWInst::Sub, rcP, b.getInt32(1),
                llvm::MaybeAlign(), llvm::AtomicOrdering::Monotonic);
            auto* nw = b.CreateSub(old, b.getInt32(1));
            b.CreateCondBr(b.CreateICmpEQ(nw, b.getInt32(0)), freeBB, doneBB);

            b.SetInsertPoint(freeBB);
            if (auto* dtor = GetOrCreateFullDestructor(className))
                b.CreateCall(dtor->getFunctionType(), dtor,
                    { b.CreateBitCast(self, objTy->getPointerTo()) });
            if (auto* del = GetFunction("operator delete"))
                b.CreateCall(del->getFunctionType(), del,
                    { b.CreateBitCast(self, del->getArg(0)->getType()) });
            b.CreateBr(doneBB);

            b.SetInsertPoint(doneBB);
            b.CreateRet(nw);
        }

        // IInspectable stubs: store null/zero out-params and return S_OK.
        auto emitInspStub = [&](const std::string& nm, unsigned outParams) {
            std::vector<llvm::Type*> ps(1 + outParams, i8PtrTy);
            auto* ty = llvm::FunctionType::get(i32Ty, ps, false);
            auto* fn = makeFn(nm, ty);
            auto* entry = llvm::BasicBlock::Create(*context, "entry", fn);
            llvm::IRBuilder<> b(entry);
            b.CreateRet(b.getInt32(0));
            return fn;
        };
        auto* getIidsFn = emitInspStub("GetIids", 2);
        auto* getNameFn = emitInspStub("GetRuntimeClassName", 1);
        auto* getTrustFn = emitInspStub("GetTrustLevel", 1);

        // Per-method thunks: bitcast i8* self to the object pointer and forward to the member fn.
        std::vector<llvm::Function*> thunks;
        auto ifIt = interfaceTable.find(ifaceName);
        if (ifIt != interfaceTable.end())
        {
            for (const auto& m : ifIt->second)
            {
                const FunctionSymbol* implSym = FindWinrtMethod(className, m);
                if (!implSym)
                {
                    LogError(std::format("[winrt] class '{}' does not implement '{}::{}'",
                        className, ifaceName, m.Name));
                    thunks.push_back(nullptr);
                    continue;
                }
                llvm::Function* impl = implSym->Function;
                auto* implTy = impl->getFunctionType();
                bool voidRet = (m.ReturnType.TypeName == "void" && !m.ReturnType.Pointer);
                bool implHResult = IsHResultType(implSym->ReturnType.TypeName);

                // A struct returned via the sret ABI (hidden pointer param) is not yet handled
                // by the wrapping thunk; diagnose rather than miscompile.
                if (impl->hasStructRetAttr())
                {
                    LogError(std::format("[winrt] '{}::{}' returns a struct via the sret ABI, "
                        "which the WinRT thunk does not support yet", className, m.Name));
                    thunks.push_back(nullptr);
                    continue;
                }

                // WinRT thunk: i32 (i8* this, <in-params>, [RetType* retval]). It forwards to the
                // user method and adapts the result to (HRESULT, *retval).
                std::vector<llvm::Type*> ps;
                ps.push_back(i8PtrTy);  // this
                for (unsigned i = 1; i < implTy->getNumParams(); i++)
                    ps.push_back(implTy->getParamType(i));
                if (!voidRet) ps.push_back(i8PtrTy);  // retval out-pointer
                auto* ty = llvm::FunctionType::get(i32Ty, ps, false);
                auto* fn = makeFn(m.Name, ty);
                auto* entry = llvm::BasicBlock::Create(*context, "entry", fn);
                llvm::IRBuilder<> b(entry);

                std::vector<llvm::Value*> args;
                args.push_back(b.CreateBitCast(fn->getArg(0), implTy->getParamType(0)));
                for (unsigned i = 1; i < implTy->getNumParams(); i++) args.push_back(fn->getArg(i));
                auto* call = b.CreateCall(implTy, impl, args);

                if (voidRet)
                {
                    // void logical return: forward the impl's hr if it is HResult<void>, else S_OK.
                    b.CreateRet(implHResult ? b.CreateExtractValue(call, { 0u }) : b.getInt32(0));
                }
                else
                {
                    llvm::Value* hr;
                    llvm::Value* val;
                    if (implHResult) { hr = b.CreateExtractValue(call, { 0u }); val = b.CreateExtractValue(call, { 1u }); }
                    else             { hr = b.getInt32(0); val = call; }
                    auto* retPtr = b.CreateBitCast(fn->getArg(static_cast<unsigned>(fn->arg_size() - 1)), val->getType()->getPointerTo());
                    b.CreateStore(val, retPtr);
                    b.CreateRet(hr);
                }
                thunks.push_back(fn);
            }
        }

        // Static vtable instance: a ConstantStruct of the generated functions, each bitcast to
        // the exact thin-fn-ptr type of its slot field.
        std::vector<llvm::Function*> slotFns = {
            qiFn, addRefFn, releaseFn, getIidsFn, getNameFn, getTrustFn };
        for (auto* t : thunks) slotFns.push_back(t);

        const auto& slotFields = dataStructures[vtblName].StructFields;
        std::vector<llvm::Constant*> entries;
        for (size_t i = 0; i < slotFields.size(); i++)
        {
            auto* slotTy = GetType(slotFields[i]);
            llvm::Constant* fnC = slotFns[i]
                ? (llvm::Constant*)llvm::ConstantExpr::getBitCast(slotFns[i], slotTy)
                : llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(slotTy));
            entries.push_back(fnC);
        }
        auto* init = llvm::ConstantStruct::get(vtblTy, entries);
        auto* vtblGlobal = new llvm::GlobalVariable(*module, vtblTy, true,
            llvm::GlobalValue::InternalLinkage, init, prefix + "vtbl");

        winrtClasses[className] = WinrtClassInfo{ ifaceName, vtblTy, vtblGlobal };
    }

    // ========================================================================================
    // WinMD CONSUME (Phase 1): register an imported .winmd's types as CFlat types so a program
    // can drive WinRT objects by hand through their COM vtable (raw HRESULT / out-param ABI),
    // exactly like the hand-written example/COM demos.
    // ========================================================================================

    // The only WinRT type names the COMPILER itself spells (foreach lowers to this protocol).
    // Everything else is named by source, fully qualified.
    static constexpr const char* kWinrtIIterable = "Windows.Foundation.Collections.IIterable";
    static constexpr const char* kWinrtIIterator = "Windows.Foundation.Collections.IIterator";

    // Last dotted segment of a WinRT full name ("Windows.Foundation.IFoo" -> "IFoo").
    static std::string WinrtSimpleName(const std::string& fullName)
    {
        auto dot = fullName.rfind('.');
        return dot == std::string::npos ? fullName : fullName.substr(dot + 1);
    }

    // Map a WinRT signature type to a (CFlat type name, pointer) pair for a vtable slot. Scalars
    // map precisely; String(HSTRING)/Object(IInspectable*)/interfaces/classes/arrays/generics/
    // by-ref all degrade to an opaque void* - ABI-correct under LLVM opaque pointers and exactly
    // what the hand-written COM demo uses. An imported enum collapses to its underlying integer;
    // an imported value struct we have registered passes by value.
    void MapWinrtTypeForSlot(const cflat_winmd::TypeRef& t, std::string& outName, bool& outPtr)
    {
        if (t.pointerDepth > 0 || t.isArray || t.isGenericVar || !t.genericArgs.empty())
        {
            outName = "void"; outPtr = true; return;
        }
        std::string fund = cflat_winmd::WinrtFundamentalToCFlat(t.fullName);
        if (!fund.empty())
        {
            if (fund == "string" || fund == "object") { outName = "void"; outPtr = true; return; }
            // Guid maps to core/guid.cb's 16-byte `Guid` by value when that type is in scope;
            // without it (guid.cb not imported) degrade to an opaque pointer (REFGUID-style).
            if (fund == "Guid" && !dataStructures.count("Guid")) { outName = "void"; outPtr = true; return; }
            // The typeMap yields user-facing CFlat spellings; the backend's direct type
            // resolution uses internal names for floats (i32/u32/... already match).
            if (fund == "f32") fund = "float";
            else if (fund == "f64") fund = "double";
            outName = fund; outPtr = false; return;
        }
        if (auto e = winrtEnumUnderlying_.find(t.fullName); e != winrtEnumUnderlying_.end())
        {
            outName = e->second; outPtr = false; return;
        }
        if (winrtValueStructs_.count(t.fullName) && dataStructures.count(t.fullName))
        {
            outName = t.fullName; outPtr = false; return;
        }
        outName = "void"; outPtr = true;   // interface / class / forward -> opaque thin pointer
    }

    // Build the COM vtable struct (`<thinName>Vtbl`, flat IInspectable layout) and the thin
    // pointer struct (`<thinName>` with a single `lpVtbl` field) for a non-generic interface or a
    // concrete generic instantiation, from already-resolved (substituted) method signatures.
    // Returns true if it registered, false if a struct of that name already exists.
    // True if `fullName`'s transitive `requires_` chain reaches an interface simple-named `target`
    // (e.g. "IUnknown" / "IInspectable"). `seen` guards against cycles. Used to tell classic-COM
    // (IUnknown-rooted) interfaces from WinRT (IInspectable) ones during consume registration.
    static bool WinrtRequiresReaches(const std::string& fullName,
        const std::unordered_map<std::string, const cflat_winmd::Interface*>& byName,
        const char* target, std::set<std::string>& seen)
    {
        auto it = byName.find(fullName);
        if (it == byName.end()) return false;
        for (const std::string& req : it->second->requires_)
        {
            if (WinrtSimpleName(req) == target) return true;
            if (seen.insert(req).second && WinrtRequiresReaches(req, byName, target, seen)) return true;
        }
        return false;
    }

    // Flatten a classic-COM single-inheritance method list into vtable-slot order: base-most
    // interface methods first, then this interface's own. IUnknown/IInspectable contribute no
    // methods (they are the synthetic header). `seen` guards against diamond duplication.
    static void CollectComBaseMethods(const cflat_winmd::Interface& iface,
        const std::unordered_map<std::string, const cflat_winmd::Interface*>& byName,
        std::set<std::string>& seen, std::vector<cflat_winmd::Method>& out)
    {
        for (const std::string& req : iface.requires_)
        {
            std::string rs = WinrtSimpleName(req);
            if (rs == "IUnknown" || rs == "IInspectable") continue;
            if (!seen.insert(req).second) continue;
            auto it = byName.find(req);
            if (it != byName.end()) CollectComBaseMethods(*it->second, byName, seen, out);
        }
        for (const cflat_winmd::Method& m : iface.methods) out.push_back(m);
    }

    // `inspectable` selects the synthetic header: WinRT interfaces derive from IInspectable
    // (6 slots: IUnknown's 3 + GetIids/GetRuntimeClassName/GetTrustLevel); classic-COM interfaces
    // (IUnknown-rooted, e.g. Direct2D from Win32 metadata) use the 3-slot IUnknown header. Pass the
    // method list already flattened across the COM single-inheritance chain in vtable-slot order.
    bool BuildWinrtInterfaceStructs(const std::string& thinName,
        const std::vector<cflat_winmd::Method>& methods, const std::string& lspDesc,
        const std::string& fileForLsp, bool inspectable = true)
    {
        std::string vtblName = thinName + "Vtbl";
        // A forward/opaque shell for the thin pointer may already exist (created by type
        // resolution); fill it rather than bail. Only skip when it is already fully built.
        auto filled = [&](const std::string& n) {
            auto it = dataStructures.find(n);
            return it != dataStructures.end() && !it->second.StructFields.empty();
        };
        if (filled(thinName)) return false;

        auto vp = []() { TypeAndValue::FuncPtrParam p; p.TypeName = "void"; p.Pointer = true; return p; };
        std::vector<DeclTypeAndValue> slots;
        slots.push_back(MakeWinrtSlot("QueryInterface", "i32", false, { vp(), vp(), vp() }));
        slots.push_back(MakeWinrtSlot("AddRef", "u32", false, { vp() }));
        slots.push_back(MakeWinrtSlot("Release", "u32", false, { vp() }));
        if (inspectable)
        {
            slots.push_back(MakeWinrtSlot("GetIids", "i32", false, { vp(), vp(), vp() }));
            slots.push_back(MakeWinrtSlot("GetRuntimeClassName", "i32", false, { vp(), vp() }));
            slots.push_back(MakeWinrtSlot("GetTrustLevel", "i32", false, { vp(), vp() }));
        }
        for (const cflat_winmd::Method& m : methods)
        {
            std::vector<TypeAndValue::FuncPtrParam> params = { vp() };   // this
            for (const cflat_winmd::Param& mp : m.params)
            {
                TypeAndValue::FuncPtrParam p;
                MapWinrtTypeForSlot(mp.type, p.TypeName, p.Pointer);
                params.push_back(p);
            }
            // A non-void logical return is passed back through a trailing [out,retval] pointer
            // ONLY for the WinRT implicit-HRESULT projection. Raw-COM methods (Win32 metadata)
            // return HRESULT explicitly and already list every out-param, so nothing is appended.
            bool voidRet = (m.returnType.fullName == "Void" && m.returnType.pointerDepth == 0);
            if (m.hresultImplicit && !voidRet) params.push_back(vp());
            slots.push_back(MakeWinrtSlot(m.name, "i32", false, params));
        }
        if (!filled(vtblName)) CreateStructType(vtblName, slots);

        DeclTypeAndValue lp;
        lp.VariableName = "lpVtbl";
        lp.TypeName = vtblName;
        lp.Pointer = true;
        CreateStructType(thinName, { lp });
        winrtThinInterfaces_.insert(thinName);
        if (auto* s = GetSymbolSink())
        {
            s->Register(SymbolKind::Struct, thinName, fileForLsp, 0, 0, lspDesc);
            // Register each real interface method as a "<Interface>.<Method>" member so --symbol
            // and dot-completion expose the WinRT call surface. `methods` is already the flattened
            // vtable-order list with the synthetic IUnknown/IInspectable base slots excluded, so
            // QueryInterface/AddRef/Release never appear here. The signature uses the logical WinRT
            // spelling (declared return + in/out params), matching how the docs describe the call.
            for (const cflat_winmd::Method& m : methods)
            {
                std::string sig = m.returnType.Spelling() + " " + m.name + "(";
                bool first = true;
                for (const cflat_winmd::Param& p : m.params)
                {
                    if (!first) sig += ", ";
                    first = false;
                    if (p.dir == cflat_winmd::ParamDir::Out) sig += "out ";
                    sig += p.type.Spelling();
                    if (!p.name.empty()) sig += " " + p.name;
                }
                sig += ")";
                s->Register(SymbolKind::Function, thinName + "." + m.name, fileForLsp, 0, 0, sig);
            }
        }
        return true;
    }

    // True if `name` is a thin COM interface pointer struct built from imported winmd (a
    // non-generic interface or a concrete generic instantiation like "IVector__int").
    bool IsWinrtThinInterface(const std::string& name) const
    {
        return winrtThinInterfaces_.count(name) != 0;
    }

    // Emit a raw COM dispatch `obj->lpVtbl->slot(obj, extraArgs...)` on a consume-side thin WinRT
    // interface pointer (built by BuildWinrtInterfaceStructs). Returns the i32 HRESULT. `extraArgs`
    // are passed verbatim after the receiver; out-params should already be void* (i8*).
    llvm::Value* EmitWinrtThinSlotCall(llvm::Value* objPtr, const std::string& thinName,
        const std::string& slotName, const std::vector<llvm::Value*>& extraArgs)
    {
        auto dsIt = dataStructures.find(thinName);
        std::string vtblName = thinName + "Vtbl";
        auto vtblIt = dataStructures.find(vtblName);
        if (dsIt == dataStructures.end() || vtblIt == dataStructures.end())
        {
            LogError(std::format("EmitWinrtThinSlotCall: '{}' is not a built WinRT interface", thinName));
            return nullptr;
        }
        auto* objTy = dsIt->second.StructType;     // { <vtbl>* }
        auto* vtblTy = vtblIt->second.StructType;
        const auto& slots = vtblIt->second.StructFields;

        unsigned slotIdx = 0;
        const DeclTypeAndValue* slot = nullptr;
        for (unsigned i = 0; i < slots.size(); i++)
            if (slots[i].VariableName == slotName) { slotIdx = i; slot = &slots[i]; break; }
        if (!slot)
        {
            LogError(std::format("EmitWinrtThinSlotCall: no slot '{}' on '{}'", slotName, thinName));
            return nullptr;
        }

        auto* i8PtrTy = builder->getInt8Ty()->getPointerTo();
        auto* typedObj = builder->CreateBitCast(objPtr, objTy->getPointerTo());
        auto* vtblFieldPtr = builder->CreateStructGEP(objTy, typedObj, 0);
        auto* vtblPtr = builder->CreateLoad(vtblTy->getPointerTo(), vtblFieldPtr);
        auto* slotPtr = builder->CreateStructGEP(vtblTy, vtblPtr, slotIdx);
        auto* fnPtr = builder->CreateLoad(BuildThinFnPtrType(*slot), slotPtr);

        std::vector<llvm::Value*> callArgs;
        callArgs.push_back(builder->CreateBitCast(objPtr, i8PtrTy));
        for (auto* a : extraArgs) callArgs.push_back(a);
        return CreateIndirectCall(*slot, fnPtr, callArgs);
    }

    // Substitute generic VAR placeholders in a template TypeRef with the concrete argument
    // TypeRefs (by VAR index), preserving any pointer/array decoration on the placeholder and
    // recursing into nested generic instantiations.
    cflat_winmd::TypeRef SubstWinrtVar(const cflat_winmd::TypeRef& t,
        const std::vector<cflat_winmd::TypeRef>& args)
    {
        if (t.isGenericVar)
        {
            cflat_winmd::TypeRef r =
                (t.genericVarIndex >= 0 && (size_t)t.genericVarIndex < args.size()) ? args[t.genericVarIndex] : t;
            r.pointerDepth += t.pointerDepth;
            r.isArray = r.isArray || t.isArray;
            return r;
        }
        if (!t.genericArgs.empty())
        {
            cflat_winmd::TypeRef r = t;
            for (auto& g : r.genericArgs) g = SubstWinrtVar(g, args);
            return r;
        }
        return t;
    }

    // Map a CFlat type-argument spelling (as written in `IVector<int>`) to the WinRT TypeRef used
    // for signature/PIID derivation. Only the unambiguous scalar set is accepted, plus named
    // imported winmd types; anything else is rejected rather than silently deriving a wrong PIID.
    bool CFlatArgToWinrtTypeRef(const std::string& cflatName, cflat_winmd::TypeRef& out, std::string& err)
    {
        // Normalize the few safe C aliases onto the explicit-width family.
        std::string n = cflatName;
        if (n == "int") n = "i32";
        else if (n == "uint") n = "u32";
        else if (n == "float") n = "f32";
        else if (n == "double") n = "f64";

        std::string w = cflat_winmd::CFlatToWinrtFundamental(n);
        if (!w.empty()) { out.fullName = w; return true; }

        // Named imported winmd type (interface / struct / enum / delegate / runtime class). WinMD
        // types are registered under their fully-qualified name, so this is an exact match - after
        // expanding a `using` alias, which is how a type ARGUMENT names one (the mangled
        // instantiation name keeps the alias spelling; only the WinRT signature needs the real one).
        std::string resolved = ResolveTypeAlias(cflatName);
        if (IsWinrtFullName(resolved)) { out.fullName = resolved; return true; }

        err = "cannot map type argument '" + cflatName + "' to a WinRT type (use an explicit-width "
              "scalar like i32/u32/f32/f64/bool/string/object, or the FULLY-QUALIFIED name of an "
              "imported winmd type such as Windows.Foundation.IStringable)";
        return false;
    }

    // True if `fullName` names an imported winmd type (any kind).
    bool IsWinrtFullName(const std::string& fullName) const
    {
        for (const auto& i : winrtConsumedModel_.interfaces)   if (i.fullName == fullName) return true;
        for (const auto& s : winrtConsumedModel_.structs)      if (s.fullName == fullName) return true;
        for (const auto& e : winrtConsumedModel_.enums)        if (e.fullName == fullName) return true;
        for (const auto& d : winrtConsumedModel_.delegates)    if (d.fullName == fullName) return true;
        for (const auto& rc : winrtConsumedModel_.runtimeClasses) if (rc.fullName == fullName) return true;
        return false;
    }

    // Instantiate an imported generic WinRT interface (`base` = the FULL name, e.g.
    // "Windows.Foundation.Collections.IVector", `cflatArgs` = the concrete CFlat type arguments,
    // `mangledName` = base + "__" + args) into a concrete COM vtable + thin pointer struct and a
    // derived PIID. Returns false if `base` is not a registered winmd generic template (so the
    // caller can fall through to other resolution).
    bool InstantiateWinrtGenericInterface(const std::string& base,
        const std::vector<std::string>& cflatArgs, const std::string& mangledName)
    {
        auto it = winrtGenericTemplates_.find(base);
        if (it == winrtGenericTemplates_.end()) return false;
        const cflat_winmd::Interface& tpl = it->second;
        if (auto ds = dataStructures.find(mangledName);
            ds != dataStructures.end() && !ds->second.StructFields.empty())
            return true;   // already fully instantiated
        if (verbose) std::cout << "[winmd] instantiate parameterized interface " << mangledName << "\n";

        if (cflatArgs.size() != tpl.genericParams.size())
        {
            LogError(std::format("'{}<...>' expects {} type argument(s), got {}",
                base, tpl.genericParams.size(), cflatArgs.size()));
            return true;   // handled (it IS a winmd generic), just mis-arity
        }

        std::vector<cflat_winmd::TypeRef> argRefs;
        for (const auto& a : cflatArgs)
        {
            cflat_winmd::TypeRef r;
            std::string err;
            if (!CFlatArgToWinrtTypeRef(a, r, err)) { LogError(base + "<...>: " + err); return true; }
            argRefs.push_back(r);
        }

        std::vector<cflat_winmd::Method> methods;
        for (const auto& m : tpl.methods)
        {
            cflat_winmd::Method nm = m;
            nm.returnType = SubstWinrtVar(m.returnType, argRefs);
            for (auto& p : nm.params) p.type = SubstWinrtVar(p.type, argRefs);
            methods.push_back(std::move(nm));
        }
        BuildWinrtInterfaceStructs(mangledName, methods, "interface " + tpl.fullName, winrtConsumedLspFile_);

        // Derive and stash the parameterized IID (shared algorithm; see WinmdSignature).
        cflat_winmd::TypeRef inst;
        inst.fullName = tpl.fullName;
        inst.genericArgs = argRefs;
        uint8_t img[16];
        std::string err;
        if (cflat_winmd::DerivePiid(inst, winrtConsumedModel_, img, err))
        {
            std::array<uint8_t, 16> a;
            for (int i = 0; i < 16; i++) a[i] = img[i];
            winrtInstanceIid_[mangledName] = a;
        }
        else
        {
            LogError("PIID derivation for '" + mangledName + "': " + err);
        }
        return true;
    }

    // Find an imported WinRT delegate template by its fully-qualified name.
    const cflat_winmd::Delegate* FindWinrtDelegate(const std::string& fullName) const
    {
        for (const auto& d : winrtConsumedModel_.delegates)
            if (d.fullName == fullName) return &d;
        return nullptr;
    }

    // The LLVM types for each Invoke parameter, mapped through the thin-slot ABI (interfaces /
    // objects / strings -> i8*, scalars/enums -> their scalar). This is exactly the surface a
    // consume-side handler sees, so the cflat closure's parameter list mirrors it 1:1.
    std::vector<llvm::Type*> WinrtDelegateInvokeParamTypes(const cflat_winmd::Method& invoke)
    {
        auto* i8PtrTy = builder->getInt8Ty()->getPointerTo();
        std::vector<llvm::Type*> out;
        for (const cflat_winmd::Param& p : invoke.params)
        {
            std::string nm; bool ptr = false;
            MapWinrtTypeForSlot(p.type, nm, ptr);
            if (ptr) { out.push_back(i8PtrTy); continue; }
            TypeAndValue tv; tv.TypeName = nm; tv.Pointer = false;
            out.push_back(GetType(tv));
        }
        return out;
    }

    // Build (once) the COM object type + static vtable {QI,AddRef,Release,Invoke} for a projected
    // delegate instantiation `mangled`. The object is { vtbl*, i32 refcount, __closure_fat_ptr };
    // Invoke forwards the WinRT ABI args to the stored closure (env-last), returning S_OK. QI
    // answers IUnknown, IAgileObject, and the delegate's own IID/PIID (`iidBytes`).
    void BuildWinrtDelegateType(const std::string& mangled, const cflat_winmd::Method& invoke,
        const uint8_t iidBytes[16])
    {
        EnsureClosureLifetimeRegistered();
        auto* i8PtrTy = builder->getInt8Ty()->getPointerTo();
        auto* i32Ty = builder->getInt32Ty();
        auto* closureTy = GetClosureFatPtrType();

        // vtbl = 4 opaque code pointers (WinRT reinterprets it as the delegate C vtable).
        auto* vtblTy = llvm::StructType::create(*context,
            { i8PtrTy, i8PtrTy, i8PtrTy, i8PtrTy }, "winrtdel_" + mangled + "_vtbl");
        auto* objTy = llvm::StructType::create(*context,
            { vtblTy->getPointerTo(), i32Ty, closureTy }, "winrtdel_" + mangled);

        // GUID globals: IUnknown, IAgileObject, and this delegate's IID.
        uint8_t unkB[16], agileB[16];
        ParseUuidToBytes("00000000-0000-0000-C000-000000000046", unkB);
        ParseUuidToBytes("94EA2B94-E9CC-49E0-C0FF-EE64CA8F5B90", agileB);
        auto* gUnk = EmitGuidGlobal(unkB);
        auto* gAgile = EmitGuidGlobal(agileB);
        auto* gIid = EmitGuidGlobal(iidBytes);

        auto makeFn = [&](const std::string& nm, llvm::FunctionType* ty) {
            return llvm::Function::Create(ty, llvm::Function::InternalLinkage,
                "__winrtdel_" + mangled + "_" + nm, module.get());
        };

        // QueryInterface(i8* self, i8* riid, i8* ppv) -> i32
        auto* qiFn = makeFn("QueryInterface",
            llvm::FunctionType::get(i32Ty, { i8PtrTy, i8PtrTy, i8PtrTy }, false));
        {
            auto* entry = llvm::BasicBlock::Create(*context, "entry", qiFn);
            auto* matchBB = llvm::BasicBlock::Create(*context, "match", qiFn);
            auto* noBB = llvm::BasicBlock::Create(*context, "nomatch", qiFn);
            llvm::IRBuilder<> b(entry);
            auto* self = qiFn->getArg(0);
            auto* riid = qiFn->getArg(1);
            auto* ppv = qiFn->getArg(2);
            auto guidEq = [&](llvm::Value* a, llvm::GlobalVariable* g) -> llvm::Value* {
                auto* i64Ty = b.getInt64Ty();
                auto* i64PtrTy = i64Ty->getPointerTo();
                auto* aLo = b.CreateAlignedLoad(i64Ty, b.CreateBitCast(a, i64PtrTy), llvm::MaybeAlign(1));
                auto* aHiPtr = b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), a, 8);
                auto* aHi = b.CreateAlignedLoad(i64Ty, b.CreateBitCast(aHiPtr, i64PtrTy), llvm::MaybeAlign(1));
                auto* gI8 = b.CreateBitCast(g, i8PtrTy);
                auto* bLo = b.CreateAlignedLoad(i64Ty, b.CreateBitCast(gI8, i64PtrTy), llvm::MaybeAlign(1));
                auto* bHiPtr = b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), gI8, 8);
                auto* bHi = b.CreateAlignedLoad(i64Ty, b.CreateBitCast(bHiPtr, i64PtrTy), llvm::MaybeAlign(1));
                return b.CreateAnd(b.CreateICmpEQ(aLo, bLo), b.CreateICmpEQ(aHi, bHi));
            };
            llvm::Value* match = b.CreateOr(guidEq(riid, gUnk), guidEq(riid, gAgile));
            match = b.CreateOr(match, guidEq(riid, gIid));
            b.CreateCondBr(match, matchBB, noBB);

            b.SetInsertPoint(matchBB);
            auto* objP = b.CreateBitCast(self, objTy->getPointerTo());
            auto* rcP = b.CreateStructGEP(objTy, objP, 1);
            b.CreateAtomicRMW(llvm::AtomicRMWInst::Add, rcP, b.getInt32(1),
                llvm::MaybeAlign(), llvm::AtomicOrdering::Monotonic);
            b.CreateStore(self, b.CreateBitCast(ppv, i8PtrTy->getPointerTo()));
            b.CreateRet(b.getInt32(0));

            b.SetInsertPoint(noBB);
            b.CreateStore(llvm::ConstantPointerNull::get(i8PtrTy),
                b.CreateBitCast(ppv, i8PtrTy->getPointerTo()));
            b.CreateRet(b.getInt32((uint32_t)0x80004002));   // E_NOINTERFACE
        }

        // AddRef(i8* self) -> i32
        auto* addRefFn = makeFn("AddRef", llvm::FunctionType::get(i32Ty, { i8PtrTy }, false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(*context, "entry", addRefFn));
            auto* objP = b.CreateBitCast(addRefFn->getArg(0), objTy->getPointerTo());
            auto* rcP = b.CreateStructGEP(objTy, objP, 1);
            auto* old = b.CreateAtomicRMW(llvm::AtomicRMWInst::Add, rcP, b.getInt32(1),
                llvm::MaybeAlign(), llvm::AtomicOrdering::Monotonic);
            b.CreateRet(b.CreateAdd(old, b.getInt32(1)));
        }

        // Release(i8* self) -> i32: destruct the owned closure and free at zero.
        auto* releaseFn = makeFn("Release", llvm::FunctionType::get(i32Ty, { i8PtrTy }, false));
        {
            auto* entry = llvm::BasicBlock::Create(*context, "entry", releaseFn);
            auto* freeBB = llvm::BasicBlock::Create(*context, "free", releaseFn);
            auto* doneBB = llvm::BasicBlock::Create(*context, "done", releaseFn);
            llvm::IRBuilder<> b(entry);
            auto* self = releaseFn->getArg(0);
            auto* objP = b.CreateBitCast(self, objTy->getPointerTo());
            auto* rcP = b.CreateStructGEP(objTy, objP, 1);
            auto* old = b.CreateAtomicRMW(llvm::AtomicRMWInst::Sub, rcP, b.getInt32(1),
                llvm::MaybeAlign(), llvm::AtomicOrdering::Monotonic);
            auto* nw = b.CreateSub(old, b.getInt32(1));
            b.CreateCondBr(b.CreateICmpEQ(nw, b.getInt32(0)), freeBB, doneBB);

            b.SetInsertPoint(freeBB);
            if (auto* cdtor = module->getFunction("__closure_fat_ptr.dtor"))
                b.CreateCall(cdtor->getFunctionType(), cdtor, { b.CreateStructGEP(objTy, objP, 2) });
            if (auto* del = GetFunction("operator delete"))
                b.CreateCall(del->getFunctionType(), del, { b.CreateBitCast(self, del->getArg(0)->getType()) });
            b.CreateBr(doneBB);

            b.SetInsertPoint(doneBB);
            b.CreateRet(nw);
        }

        // Invoke(i8* self, <mapped invoke params...>) -> i32. Forward to the closure (env-last).
        std::vector<llvm::Type*> paramTys = WinrtDelegateInvokeParamTypes(invoke);
        std::vector<llvm::Type*> invokeSig = { i8PtrTy };
        for (auto* t : paramTys) invokeSig.push_back(t);
        auto* invokeFn = makeFn("Invoke", llvm::FunctionType::get(i32Ty, invokeSig, false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(*context, "entry", invokeFn));
            auto* objP = b.CreateBitCast(invokeFn->getArg(0), objTy->getPointerTo());
            auto* cloP = b.CreateStructGEP(objTy, objP, 2);
            auto* code = b.CreateLoad(i8PtrTy, b.CreateStructGEP(closureTy, cloP, 0), "clo_code");
            llvm::Value* env = b.CreateLoad(i8PtrTy, b.CreateStructGEP(closureTy, cloP, 1), "clo_env");

            // Strip the OWNED tag (low bit) off the env before invoking - an owning heap env
            // carries the tag (lambda Option A) but the closure code expects a clean pointer.
            auto* envInt = b.CreatePtrToInt(env, b.getInt64Ty());
            auto* masked = b.CreateAnd(envInt, b.getInt64(~(uint64_t)1));
            env = b.CreateIntToPtr(masked, i8PtrTy, "env_untagged");

            std::vector<llvm::Type*> cloSig = paramTys;
            cloSig.push_back(i8PtrTy);   // trailing env
            auto* cloFnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(*context), cloSig, false);
            auto* cloFn = b.CreateBitCast(code, cloFnTy->getPointerTo(), "clo_fn");
            std::vector<llvm::Value*> cloArgs;
            for (size_t i = 0; i < paramTys.size(); i++) cloArgs.push_back(invokeFn->getArg(1 + (unsigned)i));
            cloArgs.push_back(env);
            b.CreateCall(cloFnTy, cloFn, cloArgs);
            b.CreateRet(b.getInt32(0));   // S_OK
        }

        std::vector<llvm::Constant*> entries = {
            llvm::ConstantExpr::getBitCast(qiFn, i8PtrTy),
            llvm::ConstantExpr::getBitCast(addRefFn, i8PtrTy),
            llvm::ConstantExpr::getBitCast(releaseFn, i8PtrTy),
            llvm::ConstantExpr::getBitCast(invokeFn, i8PtrTy) };
        auto* init = llvm::ConstantStruct::get(vtblTy, entries);
        auto* vtblGlobal = new llvm::GlobalVariable(*module, vtblTy, true,
            llvm::GlobalValue::InternalLinkage, init, "__winrtdel_" + mangled + "_vtbl_inst");

        winrtDelegateObjTy_[mangled] = objTy;
        winrtDelegateVtbl_[mangled] = vtblGlobal;
    }

    // Lower winrtDelegate(DelegateType, closure): synthesize (or reuse) the COM-callable object
    // type for the delegate instantiation, then at the call site allocate it, wire lpVtbl and a
    // refcount of 1, clone the closure into it (clone-by-default; the object owns the clone and
    // destructs it on final Release), and return the object as an i8* the WinRT ABI accepts.
    llvm::Value* EmitWinrtDelegateObject(const std::string& base,
        const std::vector<std::string>& cflatArgs, llvm::Value* closureFat)
    {
        const cflat_winmd::Delegate* tpl = FindWinrtDelegate(base);
        if (!tpl)
        {
            LogError("winrtDelegate: '" + base + "' is not an imported WinRT delegate type "
                     "(import the .winmd that declares it)");
            return nullptr;
        }
        if (cflatArgs.size() != tpl->genericParams.size())
        {
            LogError(std::format("winrtDelegate: '{}' expects {} type argument(s), got {}",
                base, tpl->genericParams.size(), cflatArgs.size()));
            return nullptr;
        }
        if (!(tpl->invoke.returnType.fullName == "Void" && tpl->invoke.returnType.pointerDepth == 0))
        {
            LogError("winrtDelegate: '" + base + "' has a non-void Invoke return, which the "
                     "projection does not support yet (only void-returning handlers)");
            return nullptr;
        }

        std::string mangled = base;
        for (const auto& a : cflatArgs) mangled += "__" + a;

        // Substitute the generic Invoke signature with the concrete type arguments.
        std::vector<cflat_winmd::TypeRef> argRefs;
        for (const auto& a : cflatArgs)
        {
            cflat_winmd::TypeRef r; std::string err;
            if (!CFlatArgToWinrtTypeRef(a, r, err)) { LogError("winrtDelegate " + base + "<...>: " + err); return nullptr; }
            argRefs.push_back(r);
        }
        cflat_winmd::Method invoke = tpl->invoke;
        for (auto& p : invoke.params) p.type = SubstWinrtVar(p.type, argRefs);

        // Derive the delegate IID: stored (non-generic) or PIID (parameterized).
        uint8_t iidBytes[16];
        if (cflatArgs.empty())
        {
            if (!ParseUuidToBytes(tpl->iid, iidBytes))
            {
                LogError("winrtDelegate: delegate '" + base + "' has no IID in metadata");
                return nullptr;
            }
        }
        else
        {
            cflat_winmd::TypeRef inst; inst.fullName = tpl->fullName; inst.genericArgs = argRefs;
            std::string err;
            if (!cflat_winmd::DerivePiid(inst, winrtConsumedModel_, iidBytes, err))
            {
                LogError("winrtDelegate PIID for '" + mangled + "': " + err);
                return nullptr;
            }
        }

        if (!winrtDelegateObjTy_.count(mangled))
            BuildWinrtDelegateType(mangled, invoke, iidBytes);
        auto* objTy = winrtDelegateObjTy_[mangled];
        auto* vtblGlobal = winrtDelegateVtbl_[mangled];

        if (!GetFunction("operator new"))
        {
            LogError("winrtDelegate: 'operator new' unavailable (import a core library first)");
            return nullptr;
        }
        auto* i8PtrTy = builder->getInt8Ty()->getPointerTo();
        auto* closureTy = GetClosureFatPtrType();

        // Allocate the object.
        uint64_t sz = module->getDataLayout().getTypeAllocSize(objTy);
        NamedVariable szArg;
        szArg.Primary = builder->getInt64(sz);
        szArg.BaseType = builder->getInt64Ty();
        auto* raw = CreateOverloadedFunctionCall("operator new", { szArg });
        auto* objP = builder->CreateBitCast(raw, objTy->getPointerTo(), "winrtdel.obj");

        // lpVtbl = &vtbl, refcount = 1.
        builder->CreateStore(builder->CreateBitCast(vtblGlobal, objTy->getStructElementType(0)),
            builder->CreateStructGEP(objTy, objP, 0));
        builder->CreateStore(builder->getInt32(1), builder->CreateStructGEP(objTy, objP, 1));

        // Clone the closure into the object so it owns an independent env (destructed on Release).
        llvm::Value* owned = closureFat;
        if (auto* copyFn = module->getFunction("__closure_fat_ptr.copy"))
            owned = builder->CreateCall(copyFn->getFunctionType(), copyFn, { closureFat }, "clo_clone");
        builder->CreateStore(owned, builder->CreateStructGEP(objTy, objP, 2));

        return builder->CreateBitCast(objP, i8PtrTy, "winrtdel.i8");
    }

    // Register every projectable type in `model` as CFlat types: value structs, enums (named
    // constants + underlying), and interfaces (COM vtable struct + thin pointer struct). Runtime
    // classes, delegates, generics, and HSTRING/string ergonomics are deferred (counted + noted).
    //
    // TYPES are registered ONLY under their fully-qualified WinRT name (a `.winmd`'s IButton is
    // "Microsoft.UI.Xaml.Controls.IButton", never a bare "IButton"), so a projection can never
    // displace a CFlat interface/struct of the same short name. Source spells them qualified, or
    // aliases them (`using IButton = Microsoft.UI.Xaml.Controls.IButton;`).
    void RegisterWinrtModel(const cflat_winmd::Model& model, const std::string& fileForLsp)
    {
        using namespace cflat_winmd;

        // Retain everything imported so the signature encoder can resolve nested named type args
        // (an enum/struct/interface used as a generic argument) when deriving a PIID later.
        winrtConsumedLspFile_ = fileForLsp;
        for (const auto& i : model.interfaces)    winrtConsumedModel_.interfaces.push_back(i);
        for (const auto& s : model.structs)       winrtConsumedModel_.structs.push_back(s);
        for (const auto& e : model.enums)         winrtConsumedModel_.enums.push_back(e);
        for (const auto& d : model.delegates)     winrtConsumedModel_.delegates.push_back(d);
        for (const auto& rc : model.runtimeClasses) winrtConsumedModel_.runtimeClasses.push_back(rc);

        // Pass A: enums. Record the underlying scalar (for type mapping) and expose members as
        // named constants "<Simple>_<Member>" (first-writer-wins). Deliberately NOT qualified: a
        // member is a VALUE (a global constant, never a type), so it cannot displace a type and
        // cannot reach the bad-IR class that qualification exists to kill; and a dotted name in
        // expression position is member access, so "Ns.Enum_Member" would not even be spellable.
        for (const Enum& e : model.enums)
        {
            std::string under = WinrtFundamentalToCFlat(e.underlying);
            if (under != "i32" && under != "u32" && under != "i64" && under != "u64") under = "i32";
            winrtEnumUnderlying_[e.fullName] = under;
            std::string simple = WinrtSimpleName(e.fullName);
            bool wide = (under == "i64" || under == "u64");
            for (const EnumMember& m : e.members)
            {
                std::string name = simple + "_" + m.name;
                if (globalNamedVariable.count(name)) continue;
                TypeAndValue tv; tv.TypeName = wide ? "i64" : "i32"; tv.VariableName = name;
                llvm::Constant* c = wide
                    ? (llvm::Constant*)builder->getInt64((uint64_t)m.value)
                    : (llvm::Constant*)builder->getInt32((uint32_t)(int32_t)m.value);
                CreateGlobalVariable(tv, c);
                if (auto* s = GetSymbolSink())
                    s->Register(SymbolKind::Variable, name, fileForLsp, 0, 0, tv.TypeName + " " + name);
            }
        }

        // Pass B: value structs, under their full name. Shells first so a field can reference a
        // peer struct by value. Nothing to skip - a fully-qualified name cannot collide with a
        // program's own `struct Rect`, so both types coexist.
        for (const Struct& st : model.structs)
        {
            winrtValueStructs_.insert(st.fullName);
            if (!dataStructures.count(st.fullName)) CreateStructType(st.fullName, {});
        }
        for (const Struct& st : model.structs)
        {
            std::vector<DeclTypeAndValue> fields;
            for (const Field& f : st.fields)
            {
                DeclTypeAndValue d;
                d.VariableName = f.name;
                MapWinrtTypeForSlot(f.type, d.TypeName, d.Pointer);
                fields.push_back(d);
            }
            CreateStructType(st.fullName, fields);   // fills the shell created above
            if (auto* s = GetSymbolSink())
                s->Register(SymbolKind::Struct, st.fullName, fileForLsp, 0, 0, "struct " + st.fullName);
        }

        // Pass C: interfaces -> COM vtable struct (flat IInspectable layout) + thin pointer struct.
        // Generic interfaces (IVector<T>, IMap<K,V>, ...) cannot be lowered until their type args
        // are known, so they are stashed as templates (keyed by full name) and instantiated on
        // demand by InstantiateWinrtGenericInterface.
        std::unordered_map<std::string, const cflat_winmd::Interface*> byName;
        for (const Interface& iface : model.interfaces) byName[iface.fullName] = &iface;

        size_t registered = 0, deferredGeneric = 0;
        for (const Interface& iface : model.interfaces)
        {
            const std::string& regName = iface.fullName;
            if (!iface.genericParams.empty())
            {
                deferredGeneric++;
                if (!winrtGenericTemplates_.count(regName)) winrtGenericTemplates_[regName] = iface;
                continue;
            }
            // Classic-COM (IUnknown-rooted, not IInspectable) interfaces from Win32 metadata use the
            // 3-slot IUnknown header and inline their single-inheritance base methods into the vtable.
            // WinRT interfaces (implicit IInspectable base) keep the 6-slot header + own methods only.
            std::set<std::string> s1, s2;
            bool unknownRooted = WinrtRequiresReaches(iface.fullName, byName, "IUnknown", s1)
                              && !WinrtRequiresReaches(iface.fullName, byName, "IInspectable", s2);
            bool ok;
            if (unknownRooted)
            {
                std::vector<cflat_winmd::Method> flat;
                std::set<std::string> seen;
                CollectComBaseMethods(iface, byName, seen, flat);
                ok = BuildWinrtInterfaceStructs(regName, flat, "interface " + iface.fullName, fileForLsp, false);
            }
            else
            {
                ok = BuildWinrtInterfaceStructs(regName, iface.methods, "interface " + iface.fullName, fileForLsp);
            }
            if (ok) registered++;
        }

        // Deferrals are surfaced under -v, not silently dropped (per the projection plan).
        // Runtime-class activation, delegates, and generic interfaces are not yet projected.
        if (verbose)
            std::cout << std::format(
                "[winmd] {}: registered {} interface(s), {} struct(s), {} enum(s); deferred {} generic interface(s), {} runtime class(es), {} delegate(s)\n",
                fileForLsp, registered, model.structs.size(), model.enums.size(),
                deferredGeneric, model.runtimeClasses.size(), model.delegates.size());
    }

    // Read a .winmd into the projection model and register its types. Entry point for the
    // import dispatch when it sees a `.winmd` extension.
    bool CompileWinmdFile(const std::string& path)
    {
        cflat_winmd::Model model;
        std::string err;
        if (!cflat_winmd::ReadWinmd(path, model, err))
        {
            LogError("Failed to read WinRT metadata '" + path + "': " + err);
            return false;
        }
        RegisterWinrtModel(model, path);
        return true;
    }

    // Diagnostic (M2 acceptance): import `path`, instantiate a few well-known parameterized
    // interfaces found in it, and check each derived PIID against the published reference IID plus
    // report the concrete vtable slot count. Drives the full reader -> template -> substitute ->
    // build -> PIID chain over REAL metadata. Returns true only if every present case matches.
    bool WinmdInstantiateSelfTest(const std::string& path, std::string& report)
    {
        // The constructor already brought up context/module/builder; no Init() here.
        cflat_winmd::Model model;
        std::string err;
        if (!cflat_winmd::ReadWinmd(path, model, err)) { report = "read failed: " + err + "\n"; return false; }
        RegisterWinrtModel(model, path);

        struct Case { const char* base; std::vector<std::string> args; const char* iid; };
        std::vector<Case> cases = {
            { "Windows.Foundation.IReference",                  { "i32" }, "548cefbd-bc8a-5fa0-8df2-957440fc8bf4" },
            { "Windows.Foundation.Collections.IVector",         { "i32" }, "b939af5b-b45d-5489-9149-61442c1905fe" },
            { "Windows.Foundation.Collections.IVectorView",     { "i32" }, "8d720cdf-3934-5d3f-9a55-40e8063b086a" },
            { "Windows.Foundation.Collections.IIterable",       { "i32" }, "81a643fb-f51c-5565-83c4-f96425777b66" },
        };

        bool allOk = true;
        for (const auto& c : cases)
        {
            if (!winrtGenericTemplates_.count(c.base)) { report += std::string("[skip] ") + c.base + " (not in winmd)\n"; continue; }
            std::string mangled = c.base;
            for (const auto& a : c.args) mangled += "__" + a;
            InstantiateWinrtGenericInterface(c.base, c.args, mangled);

            auto iidIt = winrtInstanceIid_.find(mangled);
            std::string got = iidIt != winrtInstanceIid_.end()
                ? cflat_winmd::FormatGuidImage(iidIt->second.data()) : "(none)";
            auto vtbl = dataStructures.find(mangled + "Vtbl");
            size_t slots = vtbl != dataStructures.end() ? vtbl->second.StructFields.size() : 0;
            bool ok = (got == c.iid) && slots >= 6;
            allOk = allOk && ok;
            report += std::format("[{}] {} -> {} ({} vtbl slots){}\n", ok ? " ok " : "FAIL",
                mangled, got, slots, ok ? "" : std::string("  want ") + c.iid);
        }
        report += allOk ? "\nALL PASS\n" : "\nFAILURES PRESENT\n";
        return allOk;
    }

    // ========================================================================================
    // WinMD PRODUCE (Phase 2): emit a .winmd from this compilation's [winrt] surface.
    // ========================================================================================

    // Convert a CFlat type to a WinRT logical signature type: fundamentals via the shared
    // typeMap; a [winrt] interface/class by name; anything else degrades to Object. A pointer
    // on a fundamental becomes one indirection level (interface/class refs carry no pointer in
    // the logical signature - the COM thinness is an ABI detail, not metadata).
    cflat_winmd::TypeRef CFlatTypeToWinrt(const TypeAndValue& tv)
    {
        cflat_winmd::TypeRef t;
        std::string w = cflat_winmd::CFlatToWinrtFundamental(tv.TypeName);
        if (!w.empty())
        {
            t.fullName = w;
            if (tv.Pointer && w != "string" && w != "object") t.pointerDepth = 1;
        }
        else if (FindTypeAnnotation(tv.TypeName, "uuid") || winrtClasses.count(tv.TypeName))
            t.fullName = tv.TypeName;
        else
            t.fullName = "Object";
        return t;
    }

    // Build a winmd model from the [winrt] interfaces (those carrying a [uuid]) and [winrt]
    // classes declared in this compilation, then write it to `path`. The interface methods come
    // from interfaceTable; the runtime classes from winrtClasses.
    bool EmitWinmd(const std::string& path, const std::string& assemblyName)
    {
        cflat_winmd::Model model;

        for (const auto& [name, methods] : interfaceTable)
        {
            std::string iid = GetTypeAnnotationArg(name, "uuid");
            if (iid.empty()) continue;   // only [uuid]-bearing interfaces describe a winmd type
            cflat_winmd::Interface iface;
            iface.fullName = name;
            iface.iid = iid;
            for (const auto& m : methods)
            {
                cflat_winmd::Method wm;
                wm.name = m.Name;
                wm.returnType = CFlatTypeToWinrt(m.ReturnType);
                for (const auto& p : m.Parameters)
                {
                    cflat_winmd::Param wp;
                    wp.type = CFlatTypeToWinrt(p);
                    wm.params.push_back(std::move(wp));
                }
                iface.methods.push_back(std::move(wm));
            }
            model.interfaces.push_back(std::move(iface));
        }

        for (const auto& [cls, info] : winrtClasses)
        {
            cflat_winmd::RuntimeClass rc;
            rc.fullName = cls;
            rc.defaultInterface = info.InterfaceName;
            rc.interfaces.push_back(info.InterfaceName);
            rc.activatable = true;
            model.runtimeClasses.push_back(std::move(rc));
        }

        if (model.interfaces.empty() && model.runtimeClasses.empty())
        {
            LogError("--emit-winmd: no [winrt] interfaces or classes found to emit");
            return false;
        }

        std::string err;
        if (!cflat_winmd::WriteWinmd(model, assemblyName, path, err))
        {
            LogError("Failed to emit winmd '" + path + "': " + err);
            return false;
        }
        if (verbose)
            std::cout << std::format("[winmd] wrote {} ({} interface(s), {} runtime class(es))\n",
                path, model.interfaces.size(), model.runtimeClasses.size());
        return true;
    }

    // Parse-only verification of a .winmd (the `--check` path for metadata files): read it into
    // the model and report success/failure WITHOUT registering types or emitting anything. Used
    // by test_winmd.bat to batch-validate the whole SDK's metadata in one process.
    bool CheckWinmd(const std::string& path)
    {
        cflat_winmd::Model model;
        std::string err;
        if (!cflat_winmd::ReadWinmd(path, model, err))
        {
            LogError("Failed to read WinRT metadata '" + path + "': " + err);
            return false;
        }
        if (verbose)
            std::cout << std::format("[winmd] {}: {} interfaces, {} structs, {} enums, {} delegates, {} runtime classes\n",
                path, model.interfaces.size(), model.structs.size(), model.enums.size(),
                model.delegates.size(), model.runtimeClasses.size());
        return true;
    }

    // The vtable slot holding the concrete destructor: it trails the method and field-offset
    // slots, so it moves as either count grows. Single source of truth for the dtor index.
    int InterfaceDtorSlotIndex(const std::string& ifaceName) const
    {
        size_t methodCount = 0;
        if (auto it = interfaceTable.find(ifaceName); it != interfaceTable.end())
            methodCount = it->second.size();
        return (int)(1 + methodCount + InterfaceFieldCount(ifaceName));
    }

    // The address of interface field `fieldName` inside the object behind fat value `fatVal`:
    // dataPtr + vtable[1 + methodCount + fieldIdx] (the implementor's byte offset). The result is
    // a true lvalue - reads and writes both go through it. Returns null when the name is no field.
    llvm::Value* EmitInterfaceFieldAddress(llvm::Value* fatVal, const std::string& ifaceName,
                                           const std::string& fieldName, llvm::Type* fieldType)
    {
        int fieldIdx = InterfaceFieldIndex(ifaceName, fieldName);
        if (fieldIdx < 0) return nullptr;

        size_t methodCount = 0;
        if (auto it = interfaceTable.find(ifaceName); it != interfaceTable.end())
            methodCount = it->second.size();

        auto ptrTy = builder->getInt8Ty()->getPointerTo();
        llvm::Value* vtablePtr = builder->CreateExtractValue(fatVal, { 0u }, "iface_vtable");
        llvm::Value* dataPtr   = builder->CreateExtractValue(fatVal, { 1u }, "iface_data");

        auto* slot = builder->CreateGEP(ptrTy, vtablePtr,
            builder->getInt32((int)(1 + methodCount + fieldIdx)), "iface_fld_slot");
        auto* offPtr = builder->CreateLoad(ptrTy, slot, "iface_fld_offptr");
        auto* off    = builder->CreatePtrToInt(offPtr, builder->getInt64Ty(), "iface_fld_off");
        auto* bytePtr = builder->CreateGEP(builder->getInt8Ty(), dataPtr, off, "iface_fld_bytes");
        // Re-GEP at the field type (a zero-index no-op) so GetTypeFromStorage - which reads a
        // GEP's element type - sees the field type rather than i8, and loads/stores get it right.
        return builder->CreateGEP(fieldType, bytePtr, builder->getInt32(0), "iface_fld_addr");
    }

    // delete through an interface fat pointer: the operand is a {vtable, data} struct, not a
    // raw pointer. Extract the data pointer, run the concrete destructor via the vtable's
    // trailing dtor slot (runtime null-guarded), then free with
    // operator delete. A null data pointer (already deleted) makes the whole thing a no-op.
    // When fatStorage is non-null its data field is nulled so a second delete is also a no-op.
    void DeleteInterfaceValue(llvm::Value* fatVal, const std::string& ifaceName, llvm::Value* fatStorage)
    {
        auto fatTy = GetFatPtrType();
        auto ptrTy = builder->getInt8Ty()->getPointerTo();

        llvm::Value* vtablePtr = builder->CreateExtractValue(fatVal, { 0u }, "iface_vtable");
        llvm::Value* dataPtr   = builder->CreateExtractValue(fatVal, { 1u }, "iface_data");

        auto* nullPtr   = llvm::ConstantPointerNull::get(ptrTy);
        auto* dataIsNull = builder->CreateICmpEQ(dataPtr, nullPtr, "iface_data_isnull");
        auto* liveBB  = CreateBasicBlock("iface_del_live");
        auto* afterBB = CreateBasicBlock("iface_del_after");
        builder->CreateCondBr(dataIsNull, afterBB, liveBB);

        // Live: dispatch the destructor through the vtable, guarding a null slot.
        builder->SetInsertPoint(liveBB);
        auto* dtorSlot = builder->CreateGEP(ptrTy, vtablePtr,
            builder->getInt32(InterfaceDtorSlotIndex(ifaceName)), "iface_dtor_slot");
        auto* dtorFn   = builder->CreateLoad(ptrTy, dtorSlot, "iface_dtor");
        auto* dtorIsNull = builder->CreateICmpEQ(dtorFn, nullPtr, "iface_dtor_isnull");
        auto* callBB = CreateBasicBlock("iface_del_dtor");
        auto* freeBB = CreateBasicBlock("iface_del_free");
        builder->CreateCondBr(dtorIsNull, freeBB, callBB);

        builder->SetInsertPoint(callBB);
        auto* dtorTy = llvm::FunctionType::get(builder->getVoidTy(), { ptrTy }, false);
        builder->CreateCall(dtorTy, dtorFn, { dataPtr });
        builder->CreateBr(freeBB);

        builder->SetInsertPoint(freeBB);
        NamedVariable ptrArg;
        ptrArg.Primary  = dataPtr;
        ptrArg.BaseType = ptrTy;
        if (GetFunction("operator delete"))
            CreateOverloadedFunctionCall("operator delete", { ptrArg });
        builder->CreateBr(afterBB);

        builder->SetInsertPoint(afterBB);
        // Null the operand's data field so a second delete sees a null pointer and no-ops.
        if (fatStorage != nullptr)
        {
            auto* dataField = builder->CreateStructGEP(fatTy, fatStorage, 1);
            builder->CreateStore(nullPtr, dataField);
        }
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

    // Deep-copy a string VALUE into a freshly heap-allocated, NUL-terminated owned buffer
    // (OWNED high-bit set on _len). Used when a string whose ownership the compiler cannot
    // transfer (e.g. a by-value parameter, which is a copy of an argument the CALLER still
    // frees) must be stored into a longer-lived slot such as a struct field - a shallow store
    // would alias the source buffer and dangle once the source is freed. Mirrors the inline
    // copy used for a non-owning string passed to a `move` parameter. Copies _len content bytes
    // then writes the terminator, so an empty/null-backed source never reads from a null pointer.
    llvm::Value* EmitOwnedStringDeepCopy(llvm::Value* value)
    {
        auto* strTy = llvm::StructType::getTypeByName(*context, "string");
        if (strTy == nullptr || value == nullptr || value->getType() != strTy) return value;
        if (!GetFunction("operator new")) return value;

        auto* srcPtr = builder->CreateExtractValue(value, { 0u }, "cpyptr");
        // Mask off the OWNED high bit before using _len as a byte count.
        auto* srcLen = builder->CreateAnd(
            builder->CreateExtractValue(value, { 1u }, "cpylenraw"),
            builder->getInt32(0x7FFFFFFF), "cpylen");
        auto* len64 = builder->CreateZExt(srcLen, builder->getInt64Ty());
        auto* allocSize = builder->CreateAdd(len64, builder->getInt64(1), "cpybufsz");

        NamedVariable szArg;
        szArg.Primary  = allocSize;
        szArg.BaseType = builder->getInt64Ty();
        auto* rawPtr  = CreateOverloadedFunctionCall("operator new", { szArg });
        auto* heapPtr = builder->CreateBitCast(rawPtr, builder->getInt8Ty()->getPointerTo(), "cpybuf");
        // Copy exactly _len bytes (0 is a safe no-op for an empty source), then NUL-terminate.
        builder->CreateMemCpy(heapPtr, llvm::MaybeAlign(1), srcPtr, llvm::MaybeAlign(1), len64);
        auto* termPtr = builder->CreateInBoundsGEP(builder->getInt8Ty(), heapPtr, { len64 }, "cpyterm");
        builder->CreateStore(builder->getInt8(0), termPtr);

        auto* ownedLen = builder->CreateOr(srcLen, builder->getInt32(0x80000000), "cpyownedlen");
        llvm::Value* out = llvm::UndefValue::get(strTy);
        out = builder->CreateInsertValue(out, heapPtr, { 0u });
        out = builder->CreateInsertValue(out, ownedLen, { 1u });
        return out;
    }


    // Transfer ownership for `move` parameters: null the caller's source storage and mark
    // it moved, so the caller's scope exit does not free what the callee now owns. Shared by
    // the normal call path (CreateOverloadedFunctionCall) and virtual dispatch
    // (CallInterfaceMethod) so the two cannot drift. Call AFTER the call is emitted.
    void ApplyMoveParamTransfer(const std::string& functionName,
        const std::vector<TypeAndValue>& params, const std::vector<NamedVariable>& args)
    {
        for (size_t i = 0; i < params.size() && i < args.size(); i++)
        {
            if (params[i].IsMove)
            {
                // A `move string` argument transfers ownership to the callee, which frees
                // it on return. If the argument is an unnamed owned-string temporary (e.g.
                // a chained-concat result passed directly), drop it from the end-of-
                // expression cleanup list so it is not also freed by the caller.
                if (params[i].TypeName == "string")
                    UnregisterOwnedStringTemp(args[i].Primary);
                // A `move` closure argument (fat Lambda<T> or an encoded closure element type, gap a)
                // transfers env ownership to the callee. Drop the unnamed lambda temp from the end-
                // of-expression closure cleanup so it is not also freed by the caller (double-free).
                else if (params[i].TypeName == "__closure_fat_ptr"
                         || IsEncodedClosureType(params[i].TypeName))
                    UnregisterOwnedClosureTemp(args[i].Primary);

                // An interface fat-ptr param built from a caller STRUCT VALUE points AT the caller's
                // alloca, so zeroing that alloca would corrupt the data the callee sees - keep it a
                // borrow. A POINTER argument is different: the fat ptr carries the pointer VALUE, so
                // boxing an owning pointer into a `move` interface param transfers ownership (like
                // `IFace x = ptr`). Its source must be nulled, or the local is freed at scope exit
                // while the callee (e.g. list<IFace>) still holds it - a double free.
                bool isInterfaceBorrow = params[i].IsInterface
                    && !params[i].IsInterfacePointer
                    && !args[i].TypeAndValue.Pointer;
                // When the arg expression went through a cast (or similar), args[i].Storage
                // may be cleared even though CallerName still names the original owning variable.
                // Look up the source by name so we still null its alloca and prevent a double-free.
                llvm::Value* srcStorage = args[i].Storage;
                llvm::Type*  srcBaseTy  = args[i].BaseType;
                bool isFieldAccess = !args[i].FieldName.empty();
                // The CallerName fallback resolves the BASE variable's storage; do NOT use it
                // for a field access or it would null the base pointer instead of the field.
                // A field access already carries its own (GEP) Storage.
                if (srcStorage == nullptr && !isFieldAccess && !args[i].CallerName.empty())
                {
                    auto ref = FindVariableStorage(args[i].CallerName);
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
                    else if (params[i].TypeName == "string" && args[i].IsOwningString)
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
                    else if (srcBaseTy == nullptr && params[i].TypeName != "string")
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
                if (!args[i].CallerName.empty() && srcStorage != nullptr &&
                    !isInterfaceBorrow && srcBaseTy != nullptr)
                {
                    bool isPtr = llvm::isa<llvm::PointerType>(srcBaseTy);
                    bool isOwningStr = params[i].TypeName == "string" && args[i].IsOwningString;
                    bool isStruct = llvm::isa<llvm::StructType>(srcBaseTy);
                    if (isPtr || isOwningStr || isStruct)
                    {
                        if (isFieldAccess)
                            MarkVariableFieldMoved(args[i].CallerName, args[i].FieldName);
                        else
                            MarkVariableMoved(args[i].CallerName);
                    }
                }
            }
        }
    }

    // Lower a by-value argument to match a declared (non-variadic) by-value parameter:
    // implicit char*/literal -> string coercion, scalar/struct upconvert, and the
    // non-owning-string -> move-param heap-copy. Shared by the normal call path
    // (CreateOverloadedFunctionCall) and virtual dispatch (CallInterfaceMethod) so the
    // two argument-lowering paths cannot drift. Returns the lowered call-arg value.
    llvm::Value* LowerByValueArg(llvm::Value* value, const TypeAndValue& param, const NamedVariable& arg)
    {
        // Encoded thin closure param (list<function<...>>::add's `T value`, gap a): the element is a
        // { i8* } POD wrapper. A bare C fn ptr / thin function<> value arrives as a pointer - wrap it
        // into the struct so it stores as the element type. The invoke site unwraps field 0.
        if (const TypeAndValue* enc = GetEncodedClosureType(param.TypeName);
            enc && enc->IsThinFnPtr() && value && value->getType()->isPointerTy() && !param.Pointer)
        {
            auto* wrapTy  = dataStructures[param.TypeName].StructType;
            auto* i8PtrTy = builder->getInt8Ty()->getPointerTo();
            auto* asI8    = builder->CreateBitCast(value, i8PtrTy, "thinfn_i8");
            llvm::Value* wrapped = llvm::UndefValue::get(wrapTy);
            return builder->CreateInsertValue(wrapped, asI8, { 0u });
        }
        if (param.TypeName == "string" && !param.Pointer
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
            // Upconvert to match the declared parameter type (e.g. i16 -> i32; struct identity).
            bool argIsUnsigned = arg.TypeAndValue.IsUnsignedInteger() != -1;
            value = Upconvert(value, GetType(param), argIsUnsigned);
        }

        // A string not statically known to own its buffer, passed to a move parameter.
        // It may still carry the runtime OWNED bit (a plain-'string' call result, or an
        // owned local read bare) - transferring that as-is lets the callee/list adopt it.
        // A literal or view (owned bit clear) must instead be heap-copied so the callee
        // receives an independent, freeable buffer that cannot dangle or leak. The static
        // flag cannot tell these apart, so branch on the owned bit at runtime.
        if (param.IsMove && param.TypeName == "string" && !arg.IsOwningString)
        {
            auto* strTy = llvm::StructType::getTypeByName(*context, "string");
            if (strTy && GetFunction("operator new"))
            {
                auto* srcPtr = builder->CreateExtractValue(value, { 0u }, "litptr");
                auto* rawLen = builder->CreateExtractValue(value, { 1u }, "litlenraw");
                // Owned bit is _len's high bit - already-owned strings transfer without a copy.
                auto* alreadyOwned = builder->CreateICmpSLT(rawLen, builder->getInt32(0), "movearg.owned");
                auto* transferBB = builder->GetInsertBlock();
                auto* fn = transferBB->getParent();
                auto* copyBB  = llvm::BasicBlock::Create(*context, "movearg.copy",  fn);
                auto* mergeBB = llvm::BasicBlock::Create(*context, "movearg.merge", fn);
                builder->CreateCondBr(alreadyOwned, mergeBB, copyBB);

                // Non-owning source: heap-copy the bytes (plus the null terminator) and set
                // the OWNED bit. Mask off _len's high bit before using it as a byte count -
                // it would otherwise inflate allocSize to ~2GB and AV the memcpy.
                builder->SetInsertPoint(copyBB);
                auto* srcLen = builder->CreateAnd(rawLen, builder->getInt32(0x7FFFFFFF), "litlen");
                auto* allocSize = builder->CreateAdd(
                    builder->CreateZExt(srcLen, builder->getInt64Ty()),
                    builder->getInt64(1), "litbufsz");
                NamedVariable szArg;
                szArg.Primary  = allocSize;
                szArg.BaseType = builder->getInt64Ty();
                auto* rawPtr  = CreateOverloadedFunctionCall("operator new", { szArg });
                auto* heapPtr = builder->CreateBitCast(rawPtr, builder->getInt8Ty()->getPointerTo(), "litbuf");
                builder->CreateMemCpy(heapPtr, llvm::MaybeAlign(1), srcPtr, llvm::MaybeAlign(1), allocSize);
                auto* ownedLen = builder->CreateOr(srcLen, builder->getInt32(0x80000000), "ownedlen");
                llvm::Value* copyVal = llvm::UndefValue::get(strTy);
                copyVal = builder->CreateInsertValue(copyVal, heapPtr, { 0u });
                copyVal = builder->CreateInsertValue(copyVal, ownedLen, { 1u });
                auto* copyEndBB = builder->GetInsertBlock();
                builder->CreateBr(mergeBB);

                // Merge: the original owned value, or the fresh owned copy.
                builder->SetInsertPoint(mergeBB);
                auto* phi = builder->CreatePHI(strTy, 2, "movearg.val");
                phi->addIncoming(value, transferBB);
                phi->addIncoming(copyVal, copyEndBB);
                value = phi;
            }
        }

        return value;
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

        // Record the return type so the call site can populate the result's TypeAndValue
        // (needed for chaining a method on the result, e.g. `e.toJson().data()`).
        lastCallReturnType = methodInfo->ReturnType;

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
                // Interface -> interface: pass the fat struct by value, re-boxing on an upcast.
                llvm::Value* val = nv.Primary ? nv.Primary : CreateLoad(nv.Storage);
                callArgs.push_back(ReboxInterfaceIfNeeded(val, nv.TypeAndValue.TypeName, param.TypeName));
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
                // Scalar or by-value struct (string / user value struct). Reuse the normal
                // call path's lowering so a string literal becomes a %string by value rather
                // than a bare ptr. See LowerByValueArg (canonical path: CreateOverloadedFunctionCall).
                auto val = nv.Primary != nullptr ? nv.Primary : CreateLoad(nv.Storage);
                val = LowerByValueArg(val, param, nv);
                callArgs.push_back(val);
            }
        }

        auto* callResult = builder->CreateCall(fnTy, fnPtr, callArgs);

        // A `move` parameter on an INTERFACE method transfers ownership just as it does on a
        // direct call, so the caller's source must be nulled/marked-moved here too. Without this
        // the source keeps its owning flag and scope exit frees what the callee now owns.
        ApplyMoveParamTransfer(ifaceName + "." + methodName, methodInfo->Parameters, extraArgNVs);

        // Classify the virtual result's ownership exactly like the direct-call path
        // (CreateOverloadedFunctionCall): a 'move string' / 'move T*' / 'move <interface>'
        // return hands an owned value to the caller. Set the side-channel so a binding site
        // (decl-init / assignment / move-param / return) re-homes it and clears the flag; an
        // inline-used owned STRING result is registered for end-of-full-expression cleanup
        // (mirrors the direct path's RegisterOwnedStringTemp) so e.g. `tree.toJson().data()`
        // does not leak. Without this the virtual path left the result classified as a borrow.
        const auto& rt = methodInfo->ReturnType;
        lastCallReturnsOwned = rt.IsMove && (rt.TypeName == "string" || rt.Pointer || rt.IsInterface);
        if (lastCallReturnsOwned
            && callResult->getType() == llvm::StructType::getTypeByName(*context, "string"))
            RegisterOwnedStringTemp(callResult);

        return callResult;
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

    // Record the statically-conformed interfaces declared by [Capability(...)]. Kept apart from
    // Interfaces so the fat-pointer conversion sites never see them.
    void RegisterStructStaticInterfaces(const std::string& structName, const std::vector<std::string>& interfaces)
    {
        dataStructures[structName].StaticInterfaces = interfaces;
    }

    std::vector<std::string> GetStructStaticInterfaces(const std::string& structName) const
    {
        auto it = dataStructures.find(structName);
        if (it == dataStructures.end()) return {};
        return it->second.StaticInterfaces;
    }

    // True when the type declares (or inherits, through interface parents) capability `ifaceName`.
    // Reads both the nominal and the static list, so a class ': ILockable' and a
    // '[Capability(ILockable)]' struct both answer yes.
    bool TypeHasCapability(const std::string& typeName, const std::string& ifaceName) const
    {
        auto it = dataStructures.find(typeName);
        if (it == dataStructures.end()) return false;
        auto declares = [&](const std::vector<std::string>& list)
        {
            for (const auto& i : list)
                if (i == ifaceName || InterfaceInheritsFrom(i, ifaceName)) return true;
            return false;
        };
        return declares(it->second.StaticInterfaces) || declares(it->second.Interfaces);
    }

    bool TypeImplementsInterface(const std::string& typeName, const std::string& ifaceName) const
    {
        // An interface trivially satisfies a constraint to itself (e.g. arena_channel<IMessage>
        // uses IMessage as its payload type when constrained to IMessage).
        if (typeName == ifaceName && interfaceTable.count(typeName)) return true;

        auto it = dataStructures.find(typeName);
        if (it == dataStructures.end()) return false;
        auto has = [&](const std::vector<std::string>& list)
        {
            return std::find(list.begin(), list.end(), ifaceName) != list.end();
        };
        // Static ([Capability]) conformance satisfies a `where T : I` constraint too; only the
        // fat-pointer CONVERSION sites are restricted to the nominal Interfaces list.
        return has(it->second.Interfaces) || has(it->second.StaticInterfaces);
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

        auto sdIt = dataStructures.find(structName);
        if (sdIt != dataStructures.end())
            VerifyInterfaceFields(structName, interfaceName, sdIt->second.StructFields);
    }

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

    llvm::GlobalVariable* CreateGlobalVariable(TypeAndValue typeValue, llvm::Constant* initValue, bool threadLocal = false, uint64_t userAlign = 0, bool externalDecl = false)
    {
        llvm::Type* destinationType = GetType(typeValue);
        if (initValue)
        {
            if (auto intValue = llvm::dyn_cast<llvm::ConstantInt>(initValue))
            {
                if (intValue->getIntegerType()->getBitWidth() != destinationType->getIntegerBitWidth())
                {
                    // Sign-extend, not zero-extend: a negative literal (e.g. the unary-minus
                    // fold that narrows -150 to i16) must widen via its signed value or the
                    // sign bit is lost and the constant reads back as a large positive number.
                    initValue = builder->getIntN(destinationType->getIntegerBitWidth(),
                        (uint64_t)intValue->getSExtValue());
                }
            }
            else if (auto fpValue = llvm::dyn_cast<llvm::ConstantFP>(initValue))
            {
                // Widen a narrower FP constant to the global's type (float -> double is implicit).
                // Narrowing is left alone so it still errors at use.
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

        // Extern globals link to the bare C symbol: a namespaced declaration like
        // os.posix's `stdout` must resolve to libc `stdout`, not a symbol literally
        // named "os.posix.stdout". This mirrors extern functions (whose linkage name
        // is the un-namespaced name). The full namespaced name stays the cflat lookup
        // key below. Non-namespaced externs (no '.') and definitions are unaffected.
        std::string symbolName = typeValue.VariableName;
        if (externalDecl)
        {
            auto dot = symbolName.find_last_of('.');
            if (dot != std::string::npos) symbolName = symbolName.substr(dot + 1);
        }

        auto gVar = new llvm::GlobalVariable(
            *module,
            destinationType,
            false, // isConstant
            llvm::GlobalValue::LinkageTypes::ExternalLinkage,
            initValue, // Initial value
            symbolName // Name (bare C symbol for externs)
        );

        if (threadLocal)
            gVar->setThreadLocalMode(llvm::GlobalVariable::GeneralDynamicTLSModel);

        // Apply effective alignment (decl-level alignas, struct alignas, or ABI).
        uint64_t effAlign = GetEffectiveAlignmentForType(typeValue.TypeName, destinationType);
        if (userAlign > effAlign) effAlign = userAlign;
        // Floor thread-locals at 16: the store vectorizer pairs adjacent 8-byte field stores into
        // a 16-byte aligned store (vmovaps), and emutls (--run) allocates each TLS block on the
        // global's declared alignment. Without an explicit 16 here the control records 8 and the
        // aligned store faults. Native .tls over-aligns the section to 16, so this only bites --run.
        if (threadLocal && effAlign < 16) effAlign = 16;
        uint64_t abiAlign = (destinationType && destinationType->isSized())
            ? module->getDataLayout().getABITypeAlign(destinationType).value() : 0;
        if (effAlign > abiAlign)
            gVar->setAlignment(llvm::Align(effAlign));

        // File-scope lock group: stamp the guardian onto the global so identifier
        // resolution can enforce the lock-set on every access.
        if (!pendingGlobalGuardedBy.empty())
            typeValue.GuardedBy = pendingGlobalGuardedBy;

        globalNamedVariable[typeValue.VariableName] = gVar;
        globalVariableTypes[typeValue.VariableName] = typeValue;

        // Record definition order for end-of-main destruction (see EmitGlobalDestructorsInMain).
        // Externs (not ours to free), thread-locals (main destroys only its own copy) and
        // core-library globals (process-lifetime infrastructure) are excluded.
        if (!externalDecl && !threadLocal && !currentSourceIsCore_ && !typeValue.VariableName.empty()
            && std::find(globalDtorOrder_.begin(), globalDtorOrder_.end(), typeValue.VariableName)
               == globalDtorOrder_.end())
            globalDtorOrder_.push_back(typeValue.VariableName);

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

    // Emit alloca in the function entry block - loop-body allocas would grow the stack unboundedly.
    // VLAs (non-null arraySize) must stay at the current point (dynamic size).
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
        // No enclosing scope means a file-scope declaration reached the local path (a stale
        // global_scope). back() on the empty scope stack is UB - diagnose instead of corrupting.
        if (stackNamedVariable.empty())
            LogError(std::format("internal: local variable '{}' declared with no enclosing scope",
                                 typeValue.VariableName));

        auto type = GetType(typeValue, autoType);
        // An abandoned C-imported record (field type the extractor couldn't map, e.g.
        // INPUT_RECORD's anonymous union) is an unsized opaque shell - diagnose, don't emit invalid IR.
        if (!typeValue.Pointer && type != nullptr && type->isStructTy() && !type->isSized())
        {
            LogError(std::format(
                "type '{}' has an incomplete layout (a field type C interop could not import); "
                "it can only be used through a pointer", typeValue.TypeName));
            type = builder->getInt8Ty();  // sized placeholder; the error already aborts the compile
        }
        // GetType returns [N x T] for fixed-size arrays; passing N again as element-count
        // would allocate N x [N x T] - an N x N over-allocation (int[1024] -> 4MB not 4KB).
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
            symbolSink_->RegisterVariable(typeValue.VariableName, typeValue.TypeName,
                                          GetSourceFilePath(), (int)line, 0);

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

    void RegisterThisPointer(const TypeAndValue& tv, llvm::Value* storage, llvm::Type* baseType)
    {
        NamedVariable namedVar{
            .TypeAndValue = tv,
            .BaseType = baseType,
            .Primary = nullptr,
            .Storage = storage,
        };
        stackNamedVariable.back().functionArgument[tv.VariableName] = namedVar;

        // Also register under "this" so `this->field` resolves correctly.
        // Primary=storage makes LoadNamedVariable return the pointer, not load through it.
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

    llvm::Value* CreateInsertValue(llvm::Value* structInstance, llvm::Value* newValue, unsigned int index)
    {
        return builder->CreateInsertValue(structInstance, newValue, index);
    }

    llvm::Value* CreateStructGEP(llvm::Type* structType, llvm::Value* structAlloc, unsigned int index, std::string variableName = "")
    {
        // LLVM 18 opaque pointer mode: ConstantFolder folds GlobalVariable GEPs into ConstantExpr,
        // which calls ConstantExpr::getCast with an invalid type combination and asserts. Insert directly.
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

            // Widening only (float -> double). Narrowing not handled here -
            // use a typed literal (0.0f) or explicit cast so types match.
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
            }
            // Widen to pointer width with sign semantics before inttoptr (which
            // zero-extends), so a negative sentinel keeps its high bits.
            unsigned ptrBits = module->getDataLayout().getPointerSizeInBits();
            if (srcType->getIntegerBitWidth() < ptrBits)
            {
                auto* ptrInt = llvm::Type::getIntNTy(*context, ptrBits);
                value = (srcIsUnsigned || srcType->getIntegerBitWidth() == 1)
                    ? builder->CreateZExt(value, ptrInt)
                    : builder->CreateSExt(value, ptrInt);
            }
            return builder->CreateIntToPtr(value, destType);
        }

        return value;
    }

    // C integer promotion for compile-time constant folding: (i8)1 << 20 folds to 0 without this.
    // Cap at i32 - i16 still overflows, i64 is unnecessary. Gated on both operands being constants.
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


    int CompareUpconvert(llvm::Type* srcType, llvm::Type* destType) const
    {
        // Null srcType (e.g. bare literal cast through operator overload, no TypeName): return -1
        // so caller reports "no overload matches" instead of segfaulting on null dereference.
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
        {
            // inttoptr zero-extends a narrow source; widen with the source's sign
            // first so (void*)(-2) yields 0xff...fe (RTLD_DEFAULT-style sentinels).
            unsigned ptrBits = module->getDataLayout().getPointerSizeInBits();
            if (srcType->getIntegerBitWidth() < ptrBits)
                value = CreateCast(value, llvm::Type::getIntNTy(*context, ptrBits), isSigned);
            return builder->CreateIntToPtr(value, destType);
        }

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
            value = llvm::ConstantFP::get(builder->getFloatTy(), *v);
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
                // ptr +/- int (no element-type context here): byte arithmetic via i8 GEP.
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
                // Normalize to i1 so AND behaves logically. isdigit() returns 4/8, which
                // would AND to 0 against an i1-widened left side (1 & 4 == 0).
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

        // C integer promotion for compile-time constants only (see PromoteToInt).
        // Signedness preserved so unsigned narrow constants zero-extend.
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

    // Logical '!': zero-compare, always i1. Distinct from CreateNot ('~', bitwise xor -1),
    // which only coincides with logical negation on i1 - on a wider int every operand
    // negates to a nonzero value (~1 == -2, ~0 == -1), making '!x' unconditionally true.
    llvm::Value* CreateLogicalNot(llvm::Value* value)
    {
        auto* type = value->getType();
        if (type->isIntegerTy(1))
            return builder->CreateNot(value);
        if (type->isPointerTy())
            return builder->CreateIsNull(value);
        if (type->isFloatingPointTy())
            return builder->CreateFCmpOEQ(value, llvm::ConstantFP::get(type, 0.0));
        if (type->isIntegerTy())
            return builder->CreateICmpEQ(value, llvm::ConstantInt::get(type, 0));
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
        if (block && IsInsertBlockLive())
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

        // Thin `function<T>`: a bare C function pointer. Direct call, no env, exact C signature.
        if (funcPtrType.IsThinFnPtr())
        {
            std::vector<llvm::Type*> paramTypes;
            for (const auto& p : funcPtrType.FuncPtrParams)
            {
                TypeAndValue pTV; pTV.TypeName = p.TypeName; pTV.Pointer = p.Pointer;
                paramTypes.push_back(GetType(pTV));
            }
            TypeAndValue retTV;
            retTV.TypeName = funcPtrType.FuncPtrReturnTypeName;
            retTV.Pointer  = funcPtrType.FuncPtrReturnPointer;
            auto* retTy   = GetType(retTV);
            auto* cFnTy   = llvm::FunctionType::get(retTy, paramTypes, false);
            auto* fnPtr   = builder->CreateBitCast(funcPtr, cFnTy->getPointerTo(), "cfn_ptr");
            for (size_t i = 0; i < args.size() && i < paramTypes.size(); i++)
            {
                auto* destTy = paramTypes[i];
                auto* strTy  = llvm::StructType::getTypeByName(*context, "string");
                if (strTy && destTy == strTy && args[i]->getType()->isPointerTy())
                    args[i] = WrapStringLiteralAsString(args[i]);
                else
                    args[i] = Upconvert(args[i], destTy);
            }
            lastCallReturnType = retTV;
            auto* result = builder->CreateCall(cFnTy, fnPtr, args);
            return retTy->isVoidTy() ? nullptr : result;
        }

        // Extract fn ptr (field 0) and env ptr (field 1) from the closure fat struct.
        auto* fnPtrI8 = builder->CreateExtractValue(funcPtr, {0u}, "fn_i8");
        auto* envPtr  = builder->CreateExtractValue(funcPtr, {1u}, "env_ptr");

        // Strip the OWNED tag (low bit) off the env - an owning heap env carries the tag (lambda
        // Option A) but the invoker expects a clean pointer. Borrowed/null env is unchanged.
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
            if (IsInsertBlockLive())
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

    // Flatten an aggregate into its scalar leaf fields with absolute byte offsets, recursing
    // through nested structs and arrays. Used by the SysV eightbyte classifier.
    void CollectScalarFields(llvm::Type* ty, uint64_t base, const llvm::DataLayout& dl,
                             std::vector<std::pair<uint64_t, llvm::Type*>>& out) const
    {
        if (auto* st = llvm::dyn_cast<llvm::StructType>(ty))
        {
            const llvm::StructLayout* sl = dl.getStructLayout(st);
            for (unsigned i = 0; i < st->getNumElements(); ++i)
                CollectScalarFields(st->getElementType(i), base + sl->getElementOffset(i), dl, out);
        }
        else if (auto* at = llvm::dyn_cast<llvm::ArrayType>(ty))
        {
            llvm::Type* el = at->getElementType();
            uint64_t esz = dl.getTypeAllocSize(el);
            for (uint64_t i = 0; i < at->getNumElements(); ++i)
                CollectScalarFields(el, base + i * esz, dl, out);
        }
        else
        {
            out.push_back({ base, ty });
        }
    }

    // SysV AMD64 eightbyte classification for a small aggregate. Returns the LLVM coercion
    // type per eightbyte (1 or 2 entries); empty means the struct is class MEMORY (size > 16
    // or a field straddling an eightbyte) and must be passed via ByVal / returned via SRet.
    // An eightbyte is SSE only if every field overlapping it is float/double; any integer/
    // pointer field makes the whole eightbyte INTEGER. Two packed floats -> <2 x float>.
    std::vector<llvm::Type*> ClassifySysVStruct(llvm::StructType* st) const
    {
        const llvm::DataLayout& dl = module->getDataLayout();
        uint64_t size = dl.getTypeAllocSize(st);
        if (size == 0 || size > 16) return {}; // MEMORY
        unsigned nEB = (unsigned)((size + 7) / 8);

        std::vector<std::pair<uint64_t, llvm::Type*>> fields;
        CollectScalarFields(st, 0, dl, fields);

        enum Cls { NoClass, Integer, Sse };
        Cls cls[2]      = { NoClass, NoClass };
        bool hasDouble[2] = { false, false };
        int  floatLanes[2] = { 0, 0 };
        for (const auto& f : fields)
        {
            uint64_t off = f.first;
            llvm::Type* t = f.second;
            uint64_t fsz = dl.getTypeAllocSize(t);
            // A scalar straddling an eightbyte boundary only happens with packing/misalignment;
            // bail to MEMORY rather than mis-coerce.
            if (off / 8 != (off + (fsz ? fsz - 1 : 0)) / 8) return {};
            unsigned eb = (unsigned)(off / 8);
            if (eb >= 2) return {};
            bool isSse = t->isFloatTy() || t->isDoubleTy();
            if (isSse)
            {
                if (cls[eb] == NoClass) cls[eb] = Sse;
                if (t->isDoubleTy()) hasDouble[eb] = true;
                else                 floatLanes[eb]++;
            }
            else
            {
                cls[eb] = Integer; // INTEGER wins over SSE in the same eightbyte
            }
        }

        std::vector<llvm::Type*> coerce;
        for (unsigned eb = 0; eb < nEB; ++eb)
        {
            uint64_t ebBytes = std::min<uint64_t>(8, size - (uint64_t)eb * 8);
            if (cls[eb] == Sse)
            {
                if (hasDouble[eb])
                    coerce.push_back(llvm::Type::getDoubleTy(*context));
                else if (floatLanes[eb] >= 2)
                    coerce.push_back(llvm::FixedVectorType::get(llvm::Type::getFloatTy(*context), 2));
                else
                    coerce.push_back(llvm::Type::getFloatTy(*context));
            }
            else // Integer or NoClass (empty eightbyte) -> integer of the covered bytes
            {
                coerce.push_back(llvm::Type::getIntNTy(*context, (unsigned)(ebBytes * 8)));
            }
        }
        return coerce;
    }

    // Build a param/return slot from a classifier's coerce list (SysV eightbytes or AArch64
    // registers): empty -> MEMORY (memoryKind: ByVal for params, SRetReturn for returns);
    // one entry -> CoerceToInt; two -> CoercePair. Shared by the SysV and AArch64 classifiers.
    static AbiSlot MakeCoerceSlot(llvm::StructType* st, uint64_t align,
                                  const std::vector<llvm::Type*>& coerce, AbiSlot::Kind memoryKind)
    {
        AbiSlot slot;
        slot.structTy = st;
        slot.align    = align;
        if (coerce.empty()) { slot.kind = memoryKind; return slot; }
        if (coerce.size() == 1) { slot.kind = AbiSlot::CoerceToInt; slot.coerceTy = coerce[0]; return slot; }
        slot.kind = AbiSlot::CoercePair; slot.coerceTy = coerce[0]; slot.coerceTy2 = coerce[1];
        return slot;
    }

    // SysV param classification: 1 eightbyte -> CoerceToInt; 2 -> CoercePair; MEMORY -> ByVal.
    AbiSlot ClassifySysVParam(llvm::StructType* st, uint64_t align)
    {
        return MakeCoerceSlot(st, align, ClassifySysVStruct(st), AbiSlot::ByVal);
    }

    // SysV return classification: 1 eightbyte -> CoerceToInt; 2 -> CoercePair; MEMORY -> SRet.
    AbiSlot ClassifySysVReturn(llvm::StructType* st, uint64_t align)
    {
        return MakeCoerceSlot(st, align, ClassifySysVStruct(st), AbiSlot::SRetReturn);
    }

    // True if `st` is an AArch64 Homogeneous Floating-point Aggregate: 1-4 leaf fields, all
    // the SAME floating type (all float OR all double), with no padding. HFAs are passed and
    // returned in consecutive SIMD/FP registers (V0..V3). On match, sets base + count.
    bool IsAArch64HFA(llvm::StructType* st, llvm::Type*& base, unsigned& count) const
    {
        const llvm::DataLayout& dl = module->getDataLayout();
        std::vector<std::pair<uint64_t, llvm::Type*>> fields;
        CollectScalarFields(st, 0, dl, fields);
        if (fields.empty() || fields.size() > 4) return false;
        llvm::Type* b = fields[0].second;
        if (!b->isFloatTy() && !b->isDoubleTy()) return false; // HVA (vectors) not handled
        for (const auto& f : fields)
            if (f.second != b) return false;
        // Reject padding/misalignment: a clean HFA is exactly count * sizeof(base).
        // Cast the (fixed) TypeSize values to uint64_t so the multiply is unambiguous
        // (AppleClang/libc++ rejects size_type * TypeSize otherwise).
        if ((uint64_t)dl.getTypeAllocSize(st) != fields.size() * (uint64_t)dl.getTypeAllocSize(b)) return false;
        base = b;
        count = (unsigned)fields.size();
        return true;
    }

    // AArch64 AAPCS64 aggregate classification. Returns the LLVM coercion type(s):
    //   - HFA (1-4 same FP type)  -> one entry: the base type (count 1) or [count x base].
    //   - other <= 8 bytes        -> one entry: i(size*8)              (one X register).
    //   - other 9..16 bytes       -> two entries: i64 + i((size-8)*8)  (two X registers).
    //   - > 16 bytes              -> empty = MEMORY (indirect: ByVal param / SRet return).
    // Differs from SysV: AArch64 never splits a non-HFA struct into SSE eightbytes; small
    // mixed int/float aggregates go entirely in general registers.
    std::vector<llvm::Type*> ClassifyAArch64Struct(llvm::StructType* st) const
    {
        const llvm::DataLayout& dl = module->getDataLayout();
        uint64_t size = dl.getTypeAllocSize(st);
        if (size == 0) return {};

        llvm::Type* base = nullptr;
        unsigned hfaCount = 0;
        if (IsAArch64HFA(st, base, hfaCount))
        {
            llvm::Type* coerce = (hfaCount == 1)
                ? base
                : (llvm::Type*)llvm::ArrayType::get(base, hfaCount);
            return { coerce };
        }

        if (size > 16) return {}; // MEMORY
        if (size <= 8)
            return { llvm::Type::getIntNTy(*context, (unsigned)(size * 8)) };
        return { llvm::Type::getInt64Ty(*context),
                 llvm::Type::getIntNTy(*context, (unsigned)((size - 8) * 8)) };
    }

    // AArch64 param classification: 1 coerce -> CoerceToInt; 2 -> CoercePair; MEMORY -> ByVal.
    AbiSlot ClassifyAArch64Param(llvm::StructType* st, uint64_t align)
    {
        return MakeCoerceSlot(st, align, ClassifyAArch64Struct(st), AbiSlot::ByVal);
    }

    // AArch64 return classification: 1 coerce -> CoerceToInt; 2 -> CoercePair; MEMORY -> SRet.
    AbiSlot ClassifyAArch64Return(llvm::StructType* st, uint64_t align)
    {
        return MakeCoerceSlot(st, align, ClassifyAArch64Struct(st), AbiSlot::SRetReturn);
    }

    // Win64 / Win32 / SysV / AArch64 ABI classification for a single param slot. Returns Direct
    // for scalars and pointers (the existing pipeline handles them). For struct-by-value:
    //   - AArch64 (macOS arm64): AAPCS64 (HFA in SIMD regs / 1-2 X regs / >16B indirect).
    //   - SysV (non-Windows x86): eightbyte classification (CoerceToInt / CoercePair / ByVal).
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
        if (targetArm64_)
            return ClassifyAArch64Param(st, align);
        if (!targetWindows_)
            return ClassifySysVParam(st, align);
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
        if (targetArm64_)
            return ClassifyAArch64Return(st, align);
        if (!targetWindows_)
            return ClassifySysVReturn(st, align);
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
        else if (recipe.retSlot.kind == AbiSlot::CoercePair)
            loweredRet = llvm::StructType::get(*context, { recipe.retSlot.coerceTy, recipe.retSlot.coerceTy2 });
        else
            loweredRet = GetCCompatibleType(retType);

        for (size_t i = 0; i < params.size(); ++i)
        {
            const AbiSlot& s = recipe.paramSlots[i];
            if (s.kind == AbiSlot::CoerceToInt)
                ptypes.push_back(s.coerceTy);
            else if (s.kind == AbiSlot::CoercePair)
            {
                ptypes.push_back(s.coerceTy);
                ptypes.push_back(s.coerceTy2);
            }
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
        for (size_t i = 0; i < recipe.paramSlots.size(); ++i)
        {
            const AbiSlot& s = recipe.paramSlots[i];
            if (s.kind == AbiSlot::ByVal)
            {
                fn->addParamAttr(attrIdx, llvm::Attribute::getWithByValType(*context, s.structTy));
                if (s.align > 0)
                    fn->addParamAttr(attrIdx, llvm::Attribute::getWithAlignment(*context, llvm::Align(s.align)));
            }
            attrIdx += SlotLLVMParamCount(s); // CoercePair expands to two LLVM params
        }
    }

    // Number of LLVM params a param slot lowers to (CoercePair -> 2, everything else -> 1).
    static unsigned SlotLLVMParamCount(const AbiSlot& s)
    {
        return s.kind == AbiSlot::CoercePair ? 2u : 1u;
    }

    // linkageName: optional override of the emitted LLVM symbol for externs. A namespaced
    // extern (namespace os.windows { extern ... Sleep(...); }) registers in the function
    // table under the qualified lookup name but must link against the bare C symbol.
    void CreateFunctionDeclaration(std::string functionName, LLVMBackend::TypeAndValue returnType, std::vector<LLVMBackend::TypeAndValue> arguments, bool external = false, bool varargs = false, bool returnsOwned = false, bool isMethod = false, CallingConv callConv = CallingConv::Default, const std::string& linkageName = {})
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

        if (llvm::Function* existing = module->getFunction(mangledName))
        {
            // A repeat declaration under the same lookup name is a no-op. A *different*
            // lookup name for an already-emitted linkage symbol (core's os.windows.Sleep
            // and a header-imported bare Sleep) still registers below, reusing the
            // existing llvm::Function via getOrInsertFunction.
            for (const auto& sym : functionTable[functionName])
                if (sym.UniqueName == mangledName)
                    return;

            // The linkage symbol already exists with a DIFFERENT signature. This happens
            // when a user `extern` collides with a core-library extern of the same name
            // (e.g. fwrite, declared in os.windows with 32-bit params). getOrInsertFunction
            // would hand back the existing function, so the new overload's calls coerce
            // args to the user's types but dispatch to the old callee - an LLVM "bad
            // signature" assert at codegen. Reject with a clear diagnostic instead.
            if (external && existing->getFunctionType() != functionType)
            {
                LogError(std::format(
                    "conflicting declaration of extern '{}': a function with this linkage "
                    "name already exists with a different signature (e.g. in a core library "
                    "such as os.windows). Rename your extern, or call the existing one "
                    "(for file I/O use os.windows.fopen/fread/fwrite/fclose).",
                    functionName));
                return;
            }
        }

        auto funcCallee = module->getOrInsertFunction(mangledName, functionType);
        llvm::Value* calleeValue = funcCallee.getCallee();

        if (llvm::Function* fn = llvm::dyn_cast<llvm::Function>(calleeValue))
        {
            if (callConv == CallingConv::Stdcall && platformValue == 32)
                fn->setCallingConv(llvm::CallingConv::X86_StdCall);
            else if (callConv == CallingConv::Cdecl)
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
                .ReturnsAlias = returnType.IsAlias, // 'alias' return: caller must not free the interior
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

    // Builds the bare LLVM function-pointer type R(*)(Args) for a function-pointer TypeAndValue.
    // This is the wire type of a thin `function<T>` and of any C-ABI function pointer.
    llvm::Type* BuildThinFnPtrType(const TypeAndValue& tv) const
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

    // Returns the C-compatible LLVM type: for IsFunctionPointer, bare fn ptr (not fat struct).
    // Used for extern function declarations to preserve C ABI compatibility.
    llvm::Type* GetCCompatibleType(const TypeAndValue& tv) const
    {
        if (tv.IsFunctionPointer)
            return BuildThinFnPtrType(tv);
        return GetType(tv);
    }

    llvm::FunctionType* GetFunctionType(const LLVMBackend::TypeAndValue& returnType, const std::vector<LLVMBackend::TypeAndValue>& arguments, bool varargs = false, bool externC = false)
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

    std::string ComputeMangledName(const std::string& functionName, const LLVMBackend::TypeAndValue& returnType, const std::vector<LLVMBackend::TypeAndValue>& arguments, bool varargs = false)
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

    llvm::Function* CreateFunctionDefinition(const std::string& functionName, LLVMBackend::TypeAndValue returnType, std::vector<LLVMBackend::TypeAndValue> arguments, bool external = false, bool varargs = false, size_t line = 0, bool returnsOwned = false, bool isMethod = false, CallingConv callConv = CallingConv::Default, size_t scopeLine = 0)
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
                if (verbose) std::cout << std::format("[verbose] skipping duplicate definition of '{}'\n", functionName);
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

        // Apple's arm64 ABI requires x29 (the frame pointer) to form a valid linked list of
        // frame records for every non-leaf function; "non-leaf" matches Apple clang's own
        // default (leaf functions may still omit it). This also lets Darwin's backtrace()
        // (frame-pointer walk, unlike Windows' unwind-table-based CaptureStackBackTrace) and
        // native tools (Instruments/sample/debuggers) unwind cflat-generated stacks.
        if (targetMacOS_)
            fn->addFnAttr("frame-pointer", "non-leaf");

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

        if (callConv == CallingConv::Stdcall && platformValue == 32)
            fn->setCallingConv(llvm::CallingConv::X86_StdCall);
        else if (callConv == CallingConv::Cdecl)
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
                .ReturnsAlias = returnType.IsAlias, // 'alias' return: caller must not free the interior
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
            if (typeAndValue.IsThinFnPtr())
                return BuildThinFnPtrType(typeAndValue);
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

        // resolvedTypeName is final; hoist the struct/interface map lookups once.
        auto dsIt = dataStructures.find(resolvedTypeName);
        bool isInterface = interfaceTable.count(resolvedTypeName) > 0;

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
            if (isInterface)
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
                if (dsIt != dataStructures.end())
                {
                    type = dsIt->second.StructType;
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
                // An encoded closure param (list<Lambda<...>>::add's `T value`, gap a) accepts the same
                // arguments; an encoded closure arg satisfies a function<T> param likewise.
                if ((candidateParamItr->IsFunctionPointer || IsEncodedClosureType(candidateParamItr->TypeName))
                    && (arg.TypeAndValue.IsFunctionPointer
                        || arg.TypeAndValue.TypeName == "__closure_fat_ptr"
                        || IsEncodedClosureType(arg.TypeAndValue.TypeName)
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
                    {
                        // Positive = widening promotion (valid but non-perfect). Integer promotions
                        // report the source bit width; a floating-point promotion (float -> double)
                        // is not an integer, so IsInteger() returns -1 - use 1 so float -> double
                        // still scores as a valid promotion instead of a spurious no-match.
                        int bits = tmpArg.IsInteger();
                        result = (bits != -1) ? bits : 1;
                    }
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

                        // Derived interface -> parent interface (IButton arg to an IElement param).
                        // Implicit (1), not perfect, so an exact same-interface overload still wins.
                        if (result < 0 && candidateParamItr->IsInterface && arg.TypeAndValue.IsInterface &&
                            InterfaceInheritsFrom(arg.TypeAndValue.TypeName, candidateParamItr->TypeName))
                        {
                            result = 1;
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

    llvm::Value* CreateOverloadedFunctionCall(const std::string& functionNameIn, const std::vector<LLVMBackend::NamedVariable>& arguments, bool forceRoot = false)
    {
        std::string functionName = ResolveQualifiedName(functionNameIn, forceRoot);

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
                // Interface -> interface: pass fat struct by value, re-boxing on an upcast
                llvm::Value* val = arg.Primary ? arg.Primary : CreateLoad(arg.Storage);
                argList.push_back(ReboxInterfaceIfNeeded(val, arg.TypeAndValue.TypeName, candParamItr->TypeName));
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
                        else if (val && val->getType()->isPointerTy())
                            val = WidenThinToFat(val);   // thin function<T> -> fat Lambda<T> {code, null}
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
                    // Canonical by-value arg lowering (string coercion + move heap-copy);
                    // shared with virtual dispatch via CallInterfaceMethod.
                    value = LowerByValueArg(value, *candParamItr, arg);
                }
                else if (value->getType()->isIntegerTy(1))
                {
                    // C default argument promotion for a variadic slot: 'bool' (i1) widens to
                    // int and is ALWAYS zero-extended (true -> 1, never -1). Without this the
                    // vararg slot keeps the caller's garbage upper bits.
                    value = Upconvert(value, builder->getInt32Ty(), true);
                }
                else if (value->getType()->isIntegerTy(8) || value->getType()->isIntegerTy(16))
                {
                    // C default argument promotion for a variadic slot: widen a sub-int
                    // integer to int, choosing zero- vs sign-extension by the source type's
                    // signedness so that e.g. u8 255 promotes to 255, not -1. Signedness is
                    // only known here (CreateFunctionCall sees a bare llvm::Value).
                    value = PromoteToInt(value, arg.TypeAndValue.IsUnsignedInteger() != -1);
                }
                else if (arg.TypeAndValue.TypeName == "string" && !arg.TypeAndValue.Pointer
                         && value->getType()->isStructTy())
                {
                    // A 'string' is a {ptr,len} value type, not a char*. A variadic slot is
                    // untyped, so the compiler cannot know the callee wants the char* (the
                    // format string is not visible here) - passing the struct silently feeds
                    // the LENGTH to the next slot and crashes on a second '%s'. cflat already
                    // rejects string -> char* at a typed parameter; be consistent and make the
                    // user state the lowering.
                    LogError(std::format(
                        "cannot pass 'string' to the variadic '...' of '{}': a variadic slot is "
                        "untyped and a 'string' is a {{ptr,len}} value, not a 'char*' - the length "
                        "field would be read as the next argument. Pass the buffer explicitly with "
                        "'.data()' (e.g. printf(\"%s\", s.data())).", functionName));
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
            && !candidate.ReturnType.IsThinFnPtr()
            && result != nullptr
            && result->getType()->isPointerTy())
        {
            result = WrapCFuncPtrAsFatStruct(result, candidate.ReturnType);
        }

        // Cache the resolved return type so callers can populate TypeAndValue after the call.
        lastCallReturnType = candidate.ReturnType;
        lastCallReturnType.IsAlias = candidate.ReturnsAlias; // mark borrow-return result; inert until consumed
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

        // Null out caller's storage for move parameters; mark the source moved (shared helper).
        ApplyMoveParamTransfer(functionName, candidate.Parameters, matched);

        // Register a closure-returning call RESULT as an owned closure temp (lambda Option A),
        // mirroring how a lambda LITERAL is tracked at creation. A binding site (decl-init /
        // assignment / field store / return) calls UnregisterOwnedClosureTemp so only the owner
        // frees it; a result used INLINE (invoked directly, or passed by value as an argument) and
        // never bound is freed by FlushOwnedClosureTemps at end-of-full-expression. Exclude the
        // `copy` clone - its result is always consumed by an owner or stored into a struct field by
        // the synthesized memberwise copy, so flushing it would double-free a now-owned field.
        if (result != nullptr
            && candidate.ReturnType.IsFunctionPointer
            && functionName != "copy"
            && result->getType() == GetClosureFatPtrType())
            RegisterOwnedClosureTemp(result);

        // Register an owned-string-returning call RESULT as an owned string temp, mirroring the
        // closure case above and TrackOwnedStringOperatorResult (operator+). A binding site
        // (decl-init / assignment / field store / move-param / return) calls
        // UnregisterOwnedStringTemp so only the owner frees it; a result used INLINE - passed by
        // value as a borrow (non-move 'string') argument, or as an expression statement - is never
        // bound and would otherwise leak, so it is freed by FlushOwnedStringTemps at end-of-full-
        // expression. Exclude the 'copy' clone: the synthesized memberwise copy stores its result
        // straight into a struct field (GetOrCreateMemberwiseCopy), bypassing the assignment-path
        // unregister, so flushing it would double-free a now-owned field - same reasoning as closures.
        if (result != nullptr
            && candidate.ReturnsOwned
            && functionName != "copy"
            && result->getType() == llvm::StructType::getTypeByName(*context, "string"))
            RegisterOwnedStringTemp(result);

        return result;
    }

    llvm::Function* GetFunction(const std::string& functionName)
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

    NamedVariable GetLocalVariable(const std::string& name)
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

    // Find the implicit 'this' argument in a function-argument map.
    // The this parameter is named "<StructName>__" (trailing double-underscore).
    // Returns end() when not in a member function context.
    static auto FindThisArgIt(const std::map<std::string, NamedVariable>& args)
        -> std::map<std::string, NamedVariable>::const_iterator
    {
        for (auto it = args.begin(); it != args.end(); ++it)
            if (it->first.ends_with("__"))
                return it;
        return args.end();
    }

    /// <summary>
    /// Get the member variable from a member function.
    /// </summary>
    NamedVariable GetMemberVariable(const std::string& name)
    {
        for (const auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
        {
            const auto& functionArguments = stackFrame.functionArgument;

            if (functionArguments.size() > 0)
            {
                auto thisIt = FindThisArgIt(functionArguments);
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

            auto thisIt = FindThisArgIt(functionArguments);
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

            auto thisIt = FindThisArgIt(functionArguments);
            if (thisIt == functionArguments.end())
                break;  // first frame with arguments but no 'this' -> not a member body
            const auto& [key, nv] = *thisIt;
            std::string structName = key.substr(0, key.size() - 2);
            if (!IsDataStructure(structName))
                break;
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

    // Mark a local whose ownership was boxed into an interface ('IFace x = ptr'). Distinct from
    // the generic moved flag: deleting such a source is a no-op that leaks (the interface owns the
    // object now and interface locals are not auto-destructed), so 'delete ptr' is rejected with a
    // targeted message. Plain moves (into 'move' params, view aliases) do NOT set this.
    void MarkVariableMovedIntoInterface(const std::string& name)
    {
        if (name.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                { it->second.MovedIntoInterface = true; return; }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                { it->second.MovedIntoInterface = true; return; }
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

    llvm::GlobalVariable* GetGlobalVariable(const std::string& name)
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

    void SetPlatformMacros()
    {
        auto i32 = llvm::Type::getInt32Ty(*context);
        SetCompileTimeMacro("__PLATFORM__", llvm::ConstantInt::get(i32, platformValue),            "int");
        SetCompileTimeMacro("__WIN64__",    llvm::ConstantInt::get(i32, platformValue == 64 ? 1 : 0), "int");
        SetCompileTimeMacro("__WIN32__",    llvm::ConstantInt::get(i32, platformValue == 32 ? 1 : 0), "int");
        SetCompileTimeMacro("__WINDOWS__",  llvm::ConstantInt::get(i32, targetWindows_ ? 1 : 0),   "int");
        // POSIX/Linux/macOS counterparts for the non-Windows targets. Both Linux
        // and macOS are POSIX; __LINUX__ and __MACOS__ are mutually exclusive and
        // select the os.posix.cb vs os.macos.cb core library in os.cb.
        const bool macos = targetMacOS_;
        const bool linux = !targetWindows_ && !macos;
        SetCompileTimeMacro("__POSIX__",    llvm::ConstantInt::get(i32, targetWindows_ ? 0 : 1),   "int");
        SetCompileTimeMacro("__LINUX__",    llvm::ConstantInt::get(i32, linux ? 1 : 0),            "int");
        SetCompileTimeMacro("__MACOS__",    llvm::ConstantInt::get(i32, macos ? 1 : 0),            "int");
        SetCompileTimeMacro("__DARWIN__",   llvm::ConstantInt::get(i32, macos ? 1 : 0),            "int");
        // Target architecture: every prior target was x86; arm64 is macOS-only for now.
        SetCompileTimeMacro("__X86__",      llvm::ConstantInt::get(i32, targetArm64_ ? 0 : 1),     "int");
        SetCompileTimeMacro("__ARM64__",    llvm::ConstantInt::get(i32, targetArm64_ ? 1 : 0),     "int");
    }

    CompileTimeMacro GetCompileTimeMacro(const std::string& name)
    {
        auto it = compileTimeMacros.find(name);
        if (it != compileTimeMacros.end())
            return it->second;
        return {"", nullptr, ""};
    }

    StructData GetDataStructure(const std::string& structName)
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
        for (size_t i = 0; i < recipe.paramSlots.size(); ++i)
        {
            const AbiSlot& s = recipe.paramSlots[i];
            if (s.kind == AbiSlot::ByVal)
            {
                ci->addParamAttr(attrIdx, llvm::Attribute::getWithByValType(*context, s.structTy));
                if (s.align > 0)
                    ci->addParamAttr(attrIdx, llvm::Attribute::getWithAlignment(*context, llvm::Align(s.align)));
            }
            attrIdx += SlotLLVMParamCount(s); // CoercePair expands to two LLVM params
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
                // v is a struct value. Place in an alloca, then load the eightbyte through a
                // bitcast - portable across all element layouts and lets LLVM coalesce. The
                // coerce type may be integer or SSE (float/double/<2 x float>) under SysV.
                auto* slot = AllocaAtEntry(s.structTy, nullptr, "abi.coerce", s.align);
                builder->CreateStore(v, slot);
                loweredArgs.push_back(LoadCoerceAt(slot, s.coerceTy, 0));
            }
            else if (s.kind == AbiSlot::CoercePair)
            {
                // SysV: two eightbytes passed as two separate scalar params (eightbyte 0 at
                // byte offset 0, eightbyte 1 at byte offset 8).
                auto* slot = AllocaAtEntry(s.structTy, nullptr, "abi.coerce", s.align);
                builder->CreateStore(v, slot);
                loweredArgs.push_back(LoadCoerceAt(slot, s.coerceTy, 0));
                loweredArgs.push_back(LoadCoerceAt(slot, s.coerceTy2, 8));
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
            StoreCoerceAt(slot, ci, 0);
            return builder->CreateLoad(recipe.retSlot.structTy, slot);
        }
        if (recipe.retSlot.kind == AbiSlot::CoercePair)
        {
            // SysV: result is a { eightbyte0, eightbyte1 } aggregate; scatter each half back
            // into the struct slot at byte offsets 0 and 8, then reload the natural struct.
            auto* slot = AllocaAtEntry(recipe.retSlot.structTy, nullptr, "abi.ret", recipe.retSlot.align);
            StoreCoerceAt(slot, builder->CreateExtractValue(ci, 0), 0);
            StoreCoerceAt(slot, builder->CreateExtractValue(ci, 1), 8);
            return builder->CreateLoad(recipe.retSlot.structTy, slot);
        }
        return ci; // Direct return
    }

    // Load a value of type coerceTy from byte offset byteOff within an alloca'd struct slot,
    // reinterpreting the underlying bytes (used to read SysV eightbytes out of a struct).
    llvm::Value* LoadCoerceAt(llvm::Value* structSlot, llvm::Type* coerceTy, uint64_t byteOff)
    {
        llvm::Value* p = structSlot;
        if (byteOff != 0)
        {
            auto* i8p = builder->CreateBitCast(structSlot, builder->getInt8Ty()->getPointerTo());
            p = builder->CreateInBoundsGEP(builder->getInt8Ty(), i8p, builder->getInt64(byteOff));
        }
        auto* cp = builder->CreateBitCast(p, coerceTy->getPointerTo());
        return builder->CreateLoad(coerceTy, cp);
    }

    // Store val into byte offset byteOff within an alloca'd struct slot, reinterpreting the
    // bytes (used to scatter SysV eightbytes returned in registers back into a struct).
    void StoreCoerceAt(llvm::Value* structSlot, llvm::Value* val, uint64_t byteOff)
    {
        llvm::Value* p = structSlot;
        if (byteOff != 0)
        {
            auto* i8p = builder->CreateBitCast(structSlot, builder->getInt8Ty()->getPointerTo());
            p = builder->CreateInBoundsGEP(builder->getInt8Ty(), i8p, builder->getInt64(byteOff));
        }
        auto* cp = builder->CreateBitCast(p, val->getType()->getPointerTo());
        builder->CreateStore(val, cp);
    }

    llvm::Value* CreateFunctionCall(llvm::Function* func, const std::vector<llvm::Value*>& arg)
    {
        // Perform Default Argument Promotions for Variadic arguments
        std::vector<llvm::Value*> callArgs;
        if (func->isVarArg())
        {
            size_t varArgStart = func->arg_size();
            for (auto value : arg)
            {
                if (varArgStart > 0)
                {
                    auto destArgument = func->getArg(static_cast<unsigned int>(func->arg_size() - varArgStart));
                    callArgs.push_back(Upconvert(value, destArgument));
                    varArgStart--;
                    continue;
                }
                auto valueType = value->getType();
                // Convert 8/16-bit int to 32-bit and 16-bit/float to double (default argument promotions).
                // bool (i1) is zero-extended: sign-extending it would pass true as -1.
                if (valueType->isIntegerTy(1))
                    callArgs.push_back(builder->CreateZExt(value, builder->getInt32Ty(), "conv"));
                else if (valueType->isIntegerTy(8) || valueType->isIntegerTy(16))
                    callArgs.push_back(builder->CreateSExt(value, builder->getInt32Ty(), "conv"));
                else if (valueType->is16bitFPTy() || valueType->isFloatTy())
                    callArgs.push_back(builder->CreateFPExt(value, builder->getDoubleTy(), "conv"));
                else
                    callArgs.push_back(value);
            }
        }
        else
        {
            for (int i = 0; i < (int)arg.size(); i++)
                callArgs.push_back(Upconvert(arg[i], func->getArg(i)));
        }

        auto* ci = builder->CreateCall(func, callArgs);
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

    // True when `storage` is the slot of a plain (borrow) string PARAMETER - a `string s`
    // argument this frame does NOT own. The slot holds a {ptr,len} copied by value from the
    // caller with the runtime OWNED bit intact (correct: the callee must not free a borrow),
    // so a passthrough `return s;` must hand back a BORROW, not a move: its Storage is an
    // alloca (looks like a movable whole-local), but the caller still owns the buffer.
    // A `move string s` param (IsOwningString) or any owning local is excluded.
    bool IsBorrowStringParamStorage(llvm::Value* storage)
    {
        if (storage == nullptr) return false;
        for (auto& frame : stackNamedVariable)
        {
            for (auto& [varName, nv] : frame.functionArgument)
                if (nv.Storage == storage)
                    return nv.TypeAndValue.TypeName == "string"
                        && !nv.TypeAndValue.IsMove
                        && !nv.IsOwningString && !nv.IsOwning;
        }
        return false;
    }

    // `returnedLocalStorage`, when provided, is the alloca of the named local being
    // returned (the return expression's NamedVariable.Storage). It lets the struct-return
    // move detection below work even when the by-value return is materialized field-wise
    // (insertvalue) rather than as a single `load %Struct`, which dyn_cast<LoadInst> misses.
    void CreateReturnCall(llvm::Value* value, llvm::Value* returnedLocalStorage = nullptr, const std::string& interfaceReturnStructName = "")
    {
        if (!IsInsertBlockLive())
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
                            // Returning a whole string LOCAL is a MOVE to the caller: zero the
                            // source so its always-run scope-exit destructor is a no-op on the
                            // moved-out value (the runtime owned bit rides along in `value`, so
                            // the caller frees iff the local owned its buffer) - the same
                            // zero-the-source discipline every other value type uses. A local
                            // that merely BORROWS an owned field (BorrowsOwnedString) is left
                            // alone: the owning struct frees that buffer, and the borrow's bit
                            // was already cleared at the return site.
                            if (nv.Storage == srcAlloca && nv.TypeAndValue.TypeName == "string"
                                && !nv.BorrowsOwnedString)
                            {
                                ownedStringReturnVar = &nv;
                                builder->CreateStore(
                                    llvm::ConstantAggregateZero::get(loadInst->getType()), srcAlloca);
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

        // Restore flags (clean state, though the scope is about to be popped anyway).
        // The returned string local was moved out by zeroing its storage (not by toggling
        // IsOwningString), so nothing to restore for it - its always-run dtor now no-ops on
        // the zeroed value.
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
                    sym.ReturnsAlias = newReturnType.IsAlias; // 'alias' return: caller must not free the interior
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
        return !IsInsertBlockLive();
    }

    // A `return` / `break` / `continue` inside a NESTED scope (a plain compound block or an
    // `if const` arm - both inline their statements into the ENCLOSING block) terminates the
    // block the caller is still writing to. Reopen emission in a fresh, predecessor-less block
    // so any statements that follow form valid - if unreachable - IR instead of instructions
    // after a terminator. Destructors already ran on the real return path (CreateReturnCall).
    void ReopenAfterTerminator()
    {
        auto* bb = builder->GetInsertBlock();
        if (bb == nullptr || bb->getParent() == nullptr || bb->getTerminator() == nullptr)
            return;
        builder->SetInsertPoint(CreateBasicBlock("unreachable", bb->getParent()));
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
        if (!IsInsertBlockLive())
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
        if (!IsInsertBlockLive())
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
    // Instrument the program with the HeapAudit leak/double-free oracle without source edits.
    void SetHeapAudit(bool v) { heapAudit_ = v; }
    void SetRunMode(bool v) { runMode_ = v; }
    bool IsRunMode() const { return runMode_; }
    void SetRunArgs(std::vector<std::string> a) { runArgs_ = std::move(a); }
    int  GetJitExitCode() const { return jitExitCode_; }
    void SetBatchMode(bool v) { batchMode_ = v; }
    void SetNoCache(bool v) { noCache_ = v; }
    // When true, headers opted into the disk cache (via the `cache` import clause) record and
    // validate every transitively-included file's mtime/hash rather than just the top header.
    void SetCHeaderCacheDeep(bool v) { cHeaderCacheDeep_ = v; }

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
    void ScanCrossThreadEscapes(CFlatParser::CompilationUnitContext* cu);
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

    // LSP-only: grays unreachable/unused code spans. Null during real compiles.
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

    bool Compile(const ArgParser& args, const std::string& inputOverride = {});

    bool CheckGrammar(const std::string& filename);

    bool CompileImportedFile(const std::string& importingFilePath, const std::string& importFilename, const std::string& namespaceName = {}, const std::string& programAlias = {}, const std::vector<std::string>& explicitLibs = {}, const std::vector<std::string>& extraDefines = {}, bool cacheHeader = false);

    bool ResolveImportPath(const std::string& importingFilePath, const std::string& importFilename,
                           std::string& outCanonical, bool quiet = false);

    bool CompileImportGroup(const std::string& importingFilePath,
                            const std::vector<std::string>& entries,
                            const std::vector<std::string>& groupLibs,
                            const std::vector<std::string>& groupDefines,
                            bool cacheGroup);

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

    void SetVcpkgExe(const std::string& path)        { vcpkg_.SetExeOverride(path); }
    void SetVcpkgManifest(const std::string& path)   { vcpkg_.SetManifestOverride(path); }
    void SetVcpkgTriplet(const std::string& triplet) { vcpkg_.SetTripletOverride(triplet); }

    std::string RootVcpkgImportPath(const std::string& analyzedPath) const
    {
        if (sourceFileDir_.empty())
            return analyzedPath;
        return (std::filesystem::path(sourceFileDir_) /
                std::filesystem::path(analyzedPath).filename()).string();
    }

    static uint64_t VcpkgDiskCacheKey(const std::string& fileForLsp,
                                       const std::vector<std::string>& defines)
    {
        uint64_t h = 14695981039346656037ULL;
        for (unsigned char c : fileForLsp) { h ^= c; h *= 1099511628211ULL; }
        for (const auto& d : defines) for (unsigned char c : d) { h ^= c; h *= 1099511628211ULL; }
        return h;
    }

    static std::string GetCHeaderCacheDir()
    {
        std::string base = GetCflatCacheDir();
        if (base.empty()) return {};
        return base + "\\cheaders";
    }

    static uint64_t CHeaderDiskCacheKey(const std::string& fileForLsp,
                                        const std::vector<std::string>& includeDirs,
                                        const std::vector<std::string>& defines,
                                        const std::vector<std::string>& extraDefines)
    {
        return CHeaderDiskCacheKey(std::vector<std::string>{ fileForLsp },
                                   includeDirs, defines, extraDefines);
    }

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

    // Read-only adapter exposing the nlohmann subset the *FromJson converters use, backed by a
    // simdjson DOM element. Keeps converter bodies unchanged while parsing with simdjson.
    // Lifetime: the backing simdjson::dom::parser + padded_string must outlive every SjVal
    // (all use is inside TryLoadCHeaderDiskCache). String reads return owning std::string copies.
    struct SjVal
    {
        simdjson::dom::element e{};
        bool ok = false;                 // false => missing/errored element, behaves as JSON null
        mutable std::vector<SjVal> kids_;
        mutable bool kidsBuilt_ = false;

        SjVal() = default;
        explicit SjVal(simdjson::dom::element el) : e(el), ok(true) {}

        bool contains(std::string_view key) const
        {
            if (!ok) return false;
            simdjson::dom::element t;
            return e.at_key(key).get(t) == simdjson::SUCCESS;
        }
        SjVal operator[](std::string_view key) const
        {
            simdjson::dom::element t;
            if (ok && e.at_key(key).get(t) == simdjson::SUCCESS) return SjVal{t};
            return SjVal{};
        }
        SjVal at(std::string_view key) const { return (*this)[key]; }

        // String read: copy out of the parser buffer so the result can outlive the parser.
        std::string value(std::string_view key, std::string_view defv) const
        {
            if (ok)
            {
                simdjson::dom::element t; std::string_view sv;
                if (e.at_key(key).get(t) == simdjson::SUCCESS && t.get(sv) == simdjson::SUCCESS)
                    return std::string(sv);
            }
            return std::string(defv);
        }
        bool value(std::string_view key, bool defv) const
        {
            if (ok)
            {
                simdjson::dom::element t; bool b;
                if (e.at_key(key).get(t) == simdjson::SUCCESS && t.get(b) == simdjson::SUCCESS)
                    return b;
            }
            return defv;
        }
        // Integer read with the coercion nlohmann did implicitly: simdjson categorizes a number
        // at parse time, so a value that fits int64 is stored as int64 and get_uint64 would reject
        // it. Try uint64 -> int64 -> double so float-bits (fvb), array sizes, lanes, etc. survive.
        template <class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
        T value(std::string_view key, T defv) const
        {
            if (!ok) return defv;
            simdjson::dom::element t;
            if (e.at_key(key).get(t) != simdjson::SUCCESS) return defv;
            uint64_t u; if (t.get(u) == simdjson::SUCCESS) return static_cast<T>(u);
            int64_t  i; if (t.get(i) == simdjson::SUCCESS) return static_cast<T>(i);
            double   d; if (t.get(d) == simdjson::SUCCESS) return static_cast<T>(d);
            return defv;
        }
        // Replaces the single nlohmann j["idims"].get<std::vector<uint64_t>>() site.
        std::vector<uint64_t> to_u64_vector() const
        {
            std::vector<uint64_t> out;
            simdjson::dom::array arr;
            if (ok && e.get(arr) == simdjson::SUCCESS)
                for (auto x : arr)
                {
                    uint64_t u; int64_t i; double d;
                    if      (x.get(u) == simdjson::SUCCESS) out.push_back(u);
                    else if (x.get(i) == simdjson::SUCCESS) out.push_back(static_cast<uint64_t>(i));
                    else if (x.get(d) == simdjson::SUCCESS) out.push_back(static_cast<uint64_t>(d));
                }
            return out;
        }
        // Replaces the nlohmann j["ps"].get<std::vector<std::string>>() site (func-macro params).
        std::vector<std::string> to_string_vector() const
        {
            std::vector<std::string> out;
            simdjson::dom::array arr;
            if (ok && e.get(arr) == simdjson::SUCCESS)
                for (auto x : arr)
                {
                    std::string_view sv;
                    if (x.get(sv) == simdjson::SUCCESS) out.emplace_back(sv);
                }
            return out;
        }
        // Range-for support: materialize array children as SjVal once, so iterators stay valid for
        // the loop (the range temporary's lifetime is extended). Non-arrays yield an empty range.
        void buildKids() const
        {
            if (kidsBuilt_) return;
            kidsBuilt_ = true;
            simdjson::dom::array arr;
            if (ok && e.get(arr) == simdjson::SUCCESS)
                for (auto x : arr) kids_.push_back(SjVal{x});
        }
        std::vector<SjVal>::const_iterator begin() const { buildKids(); return kids_.begin(); }
        std::vector<SjVal>::const_iterator end()   const { buildKids(); return kids_.end(); }
    };

    static nlohmann::json TvToJson(const TypeAndValue& tv)
    {
        nlohmann::json j;
        j["t"] = tv.TypeName;
        if (tv.Pointer)        j["p"]   = true;
        if (tv.ElemPointer)    j["ep"]  = true;
        if (tv.IsMove)         j["mv"]  = true;
        if (tv.CallConv != CallingConv::Default) j["cc"] = static_cast<int>(tv.CallConv);
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
    static TypeAndValue TvFromJson(const SjVal& j)
    {
        TypeAndValue tv;
        tv.TypeName       = j.value("t",   std::string{});
        tv.Pointer        = j.value("p",   false);
        tv.ElemPointer    = j.value("ep",  false);
        tv.IsMove         = j.value("mv",  false);
        tv.CallConv = static_cast<CallingConv>(j.value("cc", 0));
        tv.ConstArraySize = j.value("arr", uint64_t{0});
        if (j.contains("idims")) tv.ConstInnerDimensions = j["idims"].to_u64_vector();
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
        nlohmann::json j = {{"n", e.name}, {"r", TvToJson(e.ret)}, {"ps", ps},
                            {"va", e.variadic}, {"ln", e.line}, {"co", e.col}};
        if (!e.file.empty()) j["f"] = e.file;
        return j;
    }
    static CSigEntry SigFromJson(const SjVal& j)
    {
        CSigEntry e;
        e.name     = j.value("n",  std::string{});
        e.ret      = TvFromJson(j.at("r"));
        e.variadic = j.value("va", false);
        e.file     = j.value("f",  std::string{});
        e.line     = j.value("ln", 1);
        e.col      = j.value("co", 0);
        if (j.contains("ps")) for (const auto& p : j["ps"]) e.params.push_back(TvFromJson(p));
        return e;
    }

    static nlohmann::json EnumToJson(const CEnumEntry& e)
    {
        return {{"n", e.name}, {"v", e.value}, {"ln", e.line}, {"co", e.col}};
    }
    static CEnumEntry EnumFromJson(const SjVal& j)
    {
        return {j.value("n", std::string{}), j.value("v", 0LL), j.value("ln", 1), j.value("co", 0)};
    }

    static nlohmann::json GlobalToJson(const CGlobalEntry& g)
    {
        return {{"n", g.name}, {"t", TvToJson(g.type)}, {"ln", g.line}, {"co", g.col}};
    }
    static CGlobalEntry GlobalFromJson(const SjVal& j)
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
    static CRecordFieldEntry FieldFromJson(const SjVal& j)
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
        if (!r.uuid.empty()) j["id"] = r.uuid;
        return j;
    }
    static CRecordEntry RecordFromJson(const SjVal& j)
    {
        CRecordEntry r;
        r.name    = j.value("n", std::string{});
        r.isUnion = j.value("u", false);
        r.line    = j.value("ln", 1);
        r.col     = j.value("co", 0);
        r.uuid    = j.value("id", std::string{});
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
    static CMacroEntry MacroFromJson(const SjVal& j)
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
    static CFunctionMacroEntry FuncMacroFromJson(const SjVal& j)
    {
        CFunctionMacroEntry m;
        m.name = j.value("n",  std::string{});
        m.body = j.value("b",  std::string{});
        m.file = j.value("f",  std::string{});
        m.line = j.value("ln", 1);
        m.col  = j.value("co", 0);
        if (j.contains("ps")) m.params = j["ps"].to_string_vector();
        return m;
    }

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

        // parser + jsonBuf own the storage that doc/SjVal reference; keep them alive for the
        // whole function. Reads run through SjVal (simdjson DOM); writes still use nlohmann.
        simdjson::dom::parser parser;
        simdjson::padded_string jsonBuf;
        simdjson::dom::element doc;
        SjVal j;
        {
            llvm::TimeTraceScope parseScope("CHeaderJsonParse", cachePath.string());
            auto loaded = simdjson::padded_string::load(cachePath.string());
            if (loaded.error()) return false;
            jsonBuf = std::move(loaded.value());
            if (parser.parse(jsonBuf).get(doc) != simdjson::SUCCESS) return false;
            j = SjVal{doc};
        }

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
        // v8 records each C function's own declaring header (CSigEntry.file) so
        // go-to-definition lands on the real prototype, not the imported umbrella header.
        // v9 adopts pointer-to-record typedefs as handle aliases (CGColorSpaceRef ->
        // CGColorSpace*); a v8 entry's recordAliases dropped every one of them.
        // v10 also seeds the needed-record closure from in-scope function-signature and
        // global-variable by-value types, not just in-scope record fields; a v9 entry cached
        // the narrower record set, so CGPoint/CGRect-style signature-only dependency records
        // (defined in a sibling out-of-scope header, named only by a function's params/return)
        // would still be missing.
        if (version != 10) return false;

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
        // DOM walk -> structs (plus deep-mode deps freshness check). Distinct from the parse
        // span above so the allocation-bound conversion cost can be tracked separately.
        llvm::TimeTraceScope convertScope("CHeaderJsonConvert", cachePath.string());
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
        j["version"] = 10;
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
                            const std::string& portSpec,
                            const std::vector<std::string>& extraDefines = {})
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
                    std::cout << std::format("[verbose] vcpkg: package not installed (no '{}'); skipping header '{}' for LSP analysis\n",
                        res.includeDir, header);
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
        return BindCanonicalCHeader(headerCanon, res.includeDir, extraDefines);
    }

    // Bind a resolved package header (vcpkg or nuget) through the C-header pipeline with a
    // disk cache. 'headerCanon' is the canonical header path; 'includeDir' locates the sibling
    // .cflat-cache dir. On a disk hit we preload the in-memory cache so CompileCHeader skips
    // clang entirely; on a miss we write after CompileCHeader so the next build is a hit.
    bool BindCanonicalCHeader(const std::filesystem::path& headerCanon,
                              const std::string& includeDir,
                              const std::vector<std::string>& extraDefines)
    {
        std::filesystem::path pkgCacheDir =
            std::filesystem::path(includeDir).parent_path().parent_path() / ".cflat-cache";

        // Derive the same in-memory key CompileCHeader builds internally.
        llvm::SmallString<256> realPathBuf;
        std::string fileForLsp = headerCanon.string();
        if (!llvm::sys::fs::real_path(fileForLsp, realPathBuf))
            fileForLsp = realPathBuf.str().str();
        // Mirror the in-memory cache key CompileCHeaderGroup builds (single-header form).
        std::string inMemKey = "|H" + fileForLsp;
        for (const auto& inc : cIncludeDirs_)  inMemKey += "|I" + inc;
        for (const auto& def : cDefines_)      inMemKey += "|D" + def;
        for (const auto& def : extraDefines)   inMemKey += "|d" + def;

        // Fold the inline `define` clauses into the disk key so a build with a different
        // set of defines does not load a stale cached header bind.
        std::vector<std::string> diskKeyDefines = cDefines_;
        diskKeyDefines.insert(diskKeyDefines.end(), extraDefines.begin(), extraDefines.end());
        uint64_t diskKey     = VcpkgDiskCacheKey(fileForLsp, diskKeyDefines);
        uint64_t contentHash = 0;
        bool haveHash        = HashFileContents(fileForLsp, contentHash);
        std::error_code mtEc;
        auto headerMtime     = std::filesystem::last_write_time(fileForLsp, mtEc);

        bool diskHit = false;
        if (haveHash && !mtEc)
        {
            CFileSigCacheEntry diskEntry;
            if (TryLoadCHeaderDiskCache(pkgCacheDir, diskKey, headerMtime, contentHash, diskEntry))
            {
                std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
                cFileSigCache_[inMemKey] = std::move(diskEntry);
                diskHit = true;
                if (verbose)
                    std::cout << std::format("[verbose] package header disk cache hit: {}\n", fileForLsp);
            }
        }

        bool ok = CompileCHeader(headerCanon.string(), extraDefines);

        // --run is read-only: skip persisting the header cache to disk under run mode
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
                WriteCHeaderDiskCache(pkgCacheDir, diskKey, headerMtime, contentHash, entryToWrite);
        }

        if (ok) ProcessPendingMacroSources();
        return ok;
    }

    // Locate the .pri named by a package-nuget `pri "..."` clause inside the resolved package
    // folder and record its absolute path in deployPriPath_ for deployment as <exe>.pri.
    // Probe runtimes-framework/win-<arch>/native/<name> first, then recursively search the
    // package folder for an exact filename match. Not-found is a hard error (LSP: silent skip).
    // A second pri that resolves to a DIFFERENT path is a conflict error; the same path is ok.
    bool ResolveNugetPri(const std::string& priName,
                         const std::string& packageFolder,
                         const std::string& packageSpec,
                         bool lspMode)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        std::string arch = (platformValue == 32) ? "x86" : "x64";

        fs::path primary = fs::path(packageFolder) / "runtimes-framework" /
                           ("win-" + arch) / "native" / priName;
        fs::path found;
        if (fs::exists(primary, ec))
            found = primary;
        else
        {
            for (auto it = fs::recursive_directory_iterator(packageFolder, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                if (it->is_regular_file(ec) && it->path().filename().string() == priName)
                {
                    found = it->path();
                    break;
                }
            }
        }

        if (found.empty())
        {
            // LSP never deploys; a missing pri must not paint an error on a clean file.
            if (lspMode) return true;
            LogError(std::format(
                "import package-nuget: pri file '{}' was not found in package '{}' (probed '{}' "
                "and a recursive search of '{}').",
                priName, packageSpec, primary.string(), packageFolder));
            return false;
        }

        std::string abs = fs::absolute(found, ec).string();
        if (!deployPriPath_.empty() && deployPriPath_ != abs)
        {
            LogError(std::format(
                "import package-nuget: conflicting pri deployment - both '{}' and '{}' were "
                "requested as <exe>.pri. Only one pri may be deployed per output.",
                deployPriPath_, abs));
            return false;
        }
        deployPriPath_ = abs;
        if (verbose)
            std::cout << std::format("[verbose] nuget: pri '{}' -> deploy as <exe>.pri from {}\n", priName, abs);
        return true;
    }

    // Handle `import package-nuget importGroup from "id[/version]";`. Resolves the package
    // through the NuGet global packages folder (acquiring on a miss unless suppressed), pushes
    // the resulting include dirs / libs / DLLs into the per-backend accumulators, then binds
    // the imported files. A single entry routes by extension: .h/.hpp/.hh through the C-header
    // binding path (shared with package-vcpkg via BindCanonicalCHeader), .winmd through the
    // WinRT metadata pipeline. A multi-entry group is STRICT package-only and header-only: every
    // entry must resolve under the package include dirs (system headers may not ride in a group)
    // and is bound as ONE translation unit / one disk-cache entry via CompileCHeaderGroup.
    // Returns false on hard error. In LSP mode an unresolved package (or a not-found header)
    // degrades to a silent skip.
    bool CompileNugetImport(const std::vector<std::string>& files,
                            const std::string& packageSpec,
                            const std::vector<std::string>& extraDefines = {},
                            const std::string& priName = "")
    {
        if (files.empty()) return true;
        const bool multi = files.size() > 1;
        const bool lspMode = symbolSink_ != nullptr;

        // Lowercased extension of a path (leading '.' included, e.g. ".winmd").
        auto lowerExt = [](const std::string& f) {
            std::string e = std::filesystem::path(f).extension().string();
            std::transform(e.begin(), e.end(), e.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return e;
        };

        // Shape check BEFORE package resolution (needs no network/cache): a multi-entry group is
        // one C translation unit, so it may hold only .h/.hpp/.hh headers. A .winmd must be
        // imported on its own line; anything else is unsupported inside a group.
        if (multi)
        {
            for (const auto& f : files)
            {
                std::string ext = lowerExt(f);
                if (ext == ".winmd")
                {
                    LogError(std::format(
                        "import package-nuget: '{}' - a .winmd cannot appear in a multi-entry group; "
                        "group a .winmd import on its own line.", f));
                    return false;
                }
                if (ext != ".h" && ext != ".hpp" && ext != ".hh")
                {
                    LogError(std::format(
                        "import package-nuget: '{}' - a multi-entry group may contain only "
                        ".h/.hpp/.hh headers.", f));
                    return false;
                }
            }
        }

        // Mirror LSP/non-LSP mode and verbosity into the resolver.
        nuget_.SetVerbose(verbose);
        nuget_.SetLspMode(symbolSink_ != nullptr);
        nuget_.SetPlatform(platformValue == 32 ? "win32" : "win64");

        NugetResolver::Resolution res;
        std::string err;
        if (!nuget_.Resolve(packageSpec, res, err))
        {
            // LSP mode never downloads; an unresolved package should not paint an error on a
            // file that compiles cleanly once the package is restored. Degrade to a silent skip.
            if (lspMode)
            {
                if (verbose)
                    std::cout << std::format("[verbose] nuget: package '{}' not resolved; skipping for LSP analysis\n",
                        packageSpec);
                return true;
            }
            LogError(err);
            return false;
        }

        // Push the resolved paths into the existing accumulators (deduped, same as vcpkg).
        // vcpkgRuntimeDlls_ is the generic "runtime DLLs to copy next to the exe" list; reuse it.
        for (const auto& inc : res.includeDirs)
        {
            if (inc.empty()) continue;
            bool dup = false;
            for (const auto& d : cIncludeDirs_) if (d == inc) { dup = true; break; }
            if (!dup) cIncludeDirs_.push_back(inc);
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

        // Optional `pri "..."` clause: locate the named .pri inside the resolved package and
        // record it for deployment as <exe>.pri. Probe the arch-specific framework runtimes
        // dir first, then fall back to a recursive filename match over the package folder.
        if (!priName.empty() && !ResolveNugetPri(priName, res.packageFolder, packageSpec, lspMode))
            return false;

        // Multi-entry group: STRICT package-only. Resolve every header under the package
        // include dirs (a header not found there is an error - system headers may not ride in a
        // package-nuget group), then bind them all as one TU / one disk-cache entry.
        if (multi)
        {
            std::vector<std::string> headerCanonicals;
            for (const auto& f : files)
            {
                bool found = false;
                for (const auto& inc : res.includeDirs)
                {
                    std::error_code ec;
                    auto headerCanon = std::filesystem::canonical(std::filesystem::path(inc) / f, ec);
                    if (!ec) { headerCanonicals.push_back(headerCanon.string()); found = true; break; }
                }
                if (found) continue;
                // Not found under any resolved include dir. In LSP mode degrade to a silent skip.
                if (lspMode)
                {
                    if (verbose)
                        std::cout << std::format("[verbose] nuget: header '{}' not found in package '{}'; skipping for LSP analysis\n",
                            f, packageSpec);
                    return true;
                }
                LogError(std::format(
                    "import package-nuget: header '{}' was not found in the include dirs of package '{}'.\n"
                    "  Only package-owned headers may appear in a package-nuget group; a system header "
                    "(e.g. windows.h) may not ride in a package-nuget group.",
                    f, packageSpec));
                return false;
            }
            bool ok = CompileCHeaderGroup(headerCanonicals, extraDefines, /*diskCache=*/true);
            if (ok) ProcessPendingMacroSources();
            return ok;
        }

        // Single entry: route by extension of the imported file.
        const std::string& file = files[0];
        std::string ext = std::filesystem::path(file).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (ext == ".h" || ext == ".hpp" || ext == ".hh")
        {
            // The source header path is relative to a package include dir (e.g. "WebView2.h").
            // Find the first include dir under which it exists, canonicalize, then reuse the
            // shared disk-cache + CompileCHeader flow.
            for (const auto& inc : res.includeDirs)
            {
                std::error_code ec;
                std::filesystem::path headerAbs = std::filesystem::path(inc) / file;
                auto headerCanon = std::filesystem::canonical(headerAbs, ec);
                if (!ec)
                    return BindCanonicalCHeader(headerCanon, inc, extraDefines);
            }
            // Not found under any resolved include dir. In LSP mode the package may not be
            // restored yet - degrade to a silent skip rather than flagging the import line.
            if (lspMode)
            {
                if (verbose)
                    std::cout << std::format("[verbose] nuget: header '{}' not found in package '{}'; skipping for LSP analysis\n",
                        file, packageSpec);
                return true;
            }
            LogError(std::format(
                "import package-nuget: header '{}' not found in the include dirs of package '{}'.\n"
                "  The package may not own this header, or the layout is unexpected.",
                file, packageSpec));
            return false;
        }

        if (ext == ".winmd")
        {
            // WinRT metadata is a Windows-only feature - reject early off-Windows with a
            // guarded-import hint, mirroring CompileImportedFile's .winmd guard.
            if (!targetWindows_)
            {
                LogError(std::format("import package-nuget '{}': WinRT metadata (.winmd) is only supported when "
                                     "targeting Windows; guard the import with "
                                     "'if const (__WINDOWS__) {{ import ...; }}'.", file));
                return false;
            }
            // Search the resolved metadata dirs for the exact filename and route the absolute
            // path through the existing .winmd import pipeline.
            for (const auto& dir : res.winmdDirs)
            {
                std::error_code ec;
                std::filesystem::path cand = std::filesystem::path(dir) / file;
                if (std::filesystem::exists(cand, ec) && !ec)
                {
                    auto canon = std::filesystem::canonical(cand, ec);
                    return CompileWinmdFile(ec ? cand.string() : canon.string());
                }
            }
            if (lspMode)
            {
                if (verbose)
                    std::cout << std::format("[verbose] nuget: winmd '{}' not found in package '{}'; skipping for LSP analysis\n",
                        file, packageSpec);
                return true;
            }
            LogError(std::format(
                "import package-nuget: metadata '{}' not found in the winmd dirs of package '{}'.",
                file, packageSpec));
            return false;
        }

        LogError(std::format(
            "import package-nuget: '{}': only .h/.hpp/.hh headers and .winmd metadata are supported.",
            file));
        return false;
    }

    bool Analyze(const std::string& filePath, const std::vector<std::string>& importDirs, const std::string& runtimeDirPath);
    void ResetForReanalysis();

    // Populate %USERPROFILE%\.cflat\ with cached linker paths for x64 and x86.
    // Prints discovered paths to stdout. Returns false if the cache dir cannot be created.
    static bool RunInit(const std::string& runtimeDir, bool verbose);

#if defined(__APPLE__)
    // Harvest libSystem's reexported symbols from the live dyld shared cache and
    // write a flattened tbd stub to <cacheDir>/macsdk/usr/lib/libSystem.tbd, so the
    // -o link needs no macOS SDK / Command Line Tools. Called by RunInit on Darwin.
    static bool HarvestMacSystemStub(const std::string& cacheDir, bool verbose);
    // Harvest one dyld-shared-cache image (framework or dylib) into a tbd v4 stub at
    // <cacheDir>/macsdk/<relTbdPath>, using the image's real install-name. Best-effort;
    // enables SDK-free `import framework` linking. Called by RunInit on Darwin.
    static bool HarvestMacImageStub(const std::string& cacheDir, const std::string& dlopenPath,
                                    const std::string& relTbdPath, bool verbose);
    // The harvested stub's syslibroot (<cacheDir>/macsdk) if present, else "".
    static std::string MacStubSyslibroot();
#endif

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

    // Lazy generic-template materialization. The core cache stores each generic template's
    // source text instead of an eagerly re-parsed ANTLR tree; templates are parsed on first
    // use so a compile only pays the ANTLR cost for the generics it actually instantiates.
    // Returns nullptr if the name is unknown. See LoadCoreBitcodeIfFresh for the load side.
    CFlatParser::StructDefinitionContext*    MaterializeGenericStruct(const std::string& name);
    CFlatParser::ClassDefinitionContext*     MaterializeGenericClass(const std::string& name);
    CFlatParser::InterfaceDefinitionContext* MaterializeGenericInterface(const std::string& name);
    CFlatParser::FunctionDefinitionContext*  MaterializeGenericFunction(const std::string& name);

private:
    // Parse one lazily-stored template's source and patch its context pointer into `map` in
    // place. The key already exists (inserted with a null value at cache load) so .count/.find
    // existence checks stay valid; assigning the value does not invalidate other iterators.
    template <typename CtxT, typename Extract>
    CtxT* MaterializeGenericTemplate(std::unordered_map<std::string, CtxT*>& map,
                                     const std::string& name, Extract extract)
    {
        auto it = map.find(name);
        if (it == map.end()) return nullptr;
        if (it->second) return it->second;            // user-defined or already materialized
        auto srcIt = gts.lazyTemplateSource.find(name);
        if (srcIt == gts.lazyTemplateSource.end()) return nullptr;

        llvm::TimeTraceScope mat("GenericMaterialize", name);
        SyntheticParseState state;
        state.label  = "cached:" + name;
        state.input  = std::make_unique<antlr4::ANTLRInputStream>(srcIt->second);
        state.lexer  = std::make_unique<CFlatLexer>(state.input.get());
        state.tokens = std::make_unique<antlr4::CommonTokenStream>(state.lexer.get());
        state.parser = std::make_unique<CFlatParser>(state.tokens.get());
        state.parser->removeErrorListeners();
        state.tokens->fill();
        auto* cu = state.parser->compilationUnit();
        CtxT* ctx = extract(cu);
        it->second = ctx;
        syntheticParseStates_.push_back(std::move(state));
        gts.lazyTemplateSource.erase(srcIt);
        return ctx;
    }
};

// Defined here so LLVMBackend is fully declared before DumpState() is called.
inline void CompilerManager::DumpAllState() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (compilers_.empty())
    {
        std::cout << "  (no compiler instances registered)\n";
        return;
    }
    for (size_t i = 0; i < compilers_.size(); ++i)
    {
        std::cout << std::format("  [Compiler {}]\n", i);
        compilers_[i]->DumpState();
    }
}
