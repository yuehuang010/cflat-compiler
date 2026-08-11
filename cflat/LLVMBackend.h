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
#include <llvm/IR/Operator.h>     // llvm::GEPOperator (constant-expr GEPs off a global)
#include <llvm/IR/ValueHandle.h>  // llvm::WeakVH (records that outlive their function)
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
#include "MoveDataflow.h"

#include "LspSymbolIndex.h"
#include "CFlatErrorListener.h"
#include "VcpkgResolver.h"
#include "NugetResolver.h"

struct ExpectedErrorReceived {};

// Thrown by LogError while suppressErrors_ is set, so a SPECULATIVE compile-time evaluation
// (e.g. owning-sink if-const probing) can bail without emitting a diagnostic or exiting; the
// real diagnostic still fires when the same construct is later evaluated for real.
struct SpeculativeEvalAbort {};

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
    // Mangled names known to name a generic INTERFACE instantiation (Container__int). Recorded
    // wherever such a use is seen so it lowers to a fat pointer before interfaceTable has it.
    // Revoked by RevokeGenericInterfaceInstances if a struct/class template of the same base
    // name later appears - the struct role owns the mangled name then.
    std::unordered_set<std::string>                                             genericInterfaceInstances;
    // One place a VALUE of a generic-interface type was actually materialised or converted, with the
    // location and role captured at that point. Resolved once, after every instantiation has been
    // drained - see LLVMBackend::ResolveMaterializedInterfaceUses for why the decision cannot be
    // made at the site.
    struct MaterializedInterfaceUse
    {
        std::string MangledName;
        std::string File;
        size_t Line = 0;
        size_t Column = 0;
        std::string Role;
    };
    std::vector<MaterializedInterfaceUse>                                       materializedInterfaceUses;
    // Bare template names of interface templates declared under an if-const condition the forward
    // scan could not FOLD. The only basis on which a diagnostic may mention `if const`. Deliberately
    // NOT every `certain == false` context: an expect_error block is also scanned with certain=false
    // (so a template inside it cannot veto or claim a name outside it) and has nothing to do with
    // `if const` - conflating the two made the diagnostic blame `if const` on files containing none.
    std::unordered_set<std::string>                                             ifConstUncertainInterfaceNames;
    // Bare template names the forward-ref scan saw declared as a generic INTERFACE, in a live
    // (non-dead-if-const) position. Answers "is Base<...> a fat pointer?" before the main pass
    // has registered the template. Deliberately NOT genericInterfaceTemplates: nothing may be
    // INSTANTIATED off this set, only routed.
    std::unordered_set<std::string>                                             scannedGenericInterfaceNames;
    // NOTE on the --init cache: genericInterfaceInstances and the two scannedGeneric*Names sets
    // below are deliberately NOT part of the LLVMBackend.cpp cache round-trip. They are rebuilt
    // from source by the forward-ref scan of every file that is actually compiled, and a warm
    // cache still restores interfaceTable + genericInterfaceTemplates, which is what a cached
    // core template needs. Do not "fix" this by serializing them.
    // Bare template names the forward-ref scan saw declared as a generic STRUCT or CLASS.
    // Over-inclusive: a namespace-qualified declaration contributes its bare name, so a core
    // template can veto a same-named user interface. That is NOT "safe" - it is master-parity,
    // i.e. the name keeps the ORIGINAL BUG - but it never yields a fat pointer, which is why the
    // veto is preferred over guessing the other way. Tracked in
    // internal/issue/generic-interface-name-vetoed-by-core-template.md.
    std::unordered_set<std::string>                                             scannedGenericStructNames;
    // Generic struct/class template names seen where `certain` is false - inside an unfoldable
    // `if const` arm or an expect_error block. Deliberately OUT of the key space (an invented key
    // is a false rejection, see CollectGenericTemplateDecls); recorded only so the opaque-shell
    // gate can tell "declared where we do not key names" from "no such name anywhere". Read by
    // AnyGenericTypeTemplateNamed and nothing else.
    std::unordered_set<std::string>                                             scannedGenericStructNamesUncertain;
    // Qualified names of every struct/class/interface DEFINITION the forward-ref scan saw, generic
    // or not. The accept set for resolving a generic type ARGUMENT's spelling (see
    // ResolveTypeArgBaseName): dataStructures/interfaceTable are not populated yet when
    // ScanGenericTypeUses runs, and are order-dependent while ScanExternalDeclaration walks. Like
    // the two scannedGeneric*Names sets it is deliberately NOT cached - a warm cache restores
    // dataStructures + interfaceTable, which is what a cached core type needs.
    std::unordered_set<std::string>                                             scannedTypeNames;
    // Template key -> the NAMESPACE it was declared in, recorded at registration. It CANNOT be
    // derived from the key: struct nesting and namespace nesting share one dotted key space, so a
    // template nested in `struct Outer` is keyed "Outer.Box" exactly like one in `namespace Outer`.
    // Deriving it with rfind('.') made a nested template's body resolve its bare names against a
    // same-named NAMESPACE and silently return that namespace's type.
    std::unordered_map<std::string, std::string>                                genericTemplateNamespace;
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

// Longest run of quoted-back user source allowed in one diagnostic. An `if const` condition may be
// arbitrarily long and may nest, so both a single condition and the composed nesting chain are
// bounded - a multi-kilobyte single-line error message helps nobody.
inline constexpr size_t kIfConstConditionTextLimit = 120;

// Cut `text` to at most `limit` bytes, appending "...". The cut backs off any UTF-8 continuation
// byte, so a sliced comment inside a condition cannot leave half a character behind.
inline std::string TruncateDiagnosticText(std::string text, size_t limit)
{
    if (text.size() <= limit) return text;
    size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) cut--;
    return text.substr(0, cut) + "...";
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

// Trim a version string to major.minor. LC_BUILD_VERSION's sdk field is a two-component
// SDK label by Apple convention, so an OS patch level ("26.5.2") must not leak into it.
inline std::string TwoComponentVersion(const std::string& v)
{
    size_t first = v.find('.');
    if (first == std::string::npos) return v;
    size_t second = v.find('.', first + 1);
    if (second == std::string::npos) return v;
    return v.substr(0, second);
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

// How a lock is held at a guarded-field access. Exclusive is `lock (m)` / `lock (rw.write)`;
// Shared is `lock (rw.read)`; Optimistic is a version-validated speculative read - it holds
// nothing at all. Only Exclusive permits a WRITE to a guarded field.
enum class LockMode
{
    Exclusive,
    Shared,
    Optimistic,
};

/*
 * Fold an alias SPELLING of a primitive onto its canonical name. `int` and `i32` are one type
 * ("freely interchangeable", doc/LANGUAGE.md), so every place that builds a TYPE IDENTITY out of
 * a spelling must funnel through here or the two spellings become two types. Two such places
 * exist: MangleTypeArg (generic instantiation) and ToUniqueString (overload symbol names).
 *
 * This is SOURCE identity, not the ABI canon. Signedness is PRESERVED: `u32` is a distinct type
 * from `i32`, not a spelling of it, and it divides, compares and prints differently. The
 * function-pointer comparison wants the ABI canon instead (FuncPtrScalarCanon, which drops
 * signedness) - see internal/plan/funcptr-type-mangling.md for why the two must stay separate.
 *
 * Two deliberate absences. `long`/`ulong` are target-native, so folding them onto `i64`/`u64`
 * would make `Box<long>` and `Box<i64>` one type on LP64 and two on Windows - platform-varying
 * symbol names. `char` carries a distinct DWARF encoding (DW_ATE_signed_char) that `i8` does not,
 * so folding it would make a debugger print numbers instead of characters, and which of the two
 * spellings won would depend on registration order.
 */
inline const std::string& CanonicalPrimitiveSpelling(const std::string& base)
{
    static const std::unordered_map<std::string, std::string> kCanonicalPrimitive = {
        { "int",   "i32" },
        { "short", "i16" },
    };
    auto it = kCanonicalPrimitive.find(base);
    return it == kCanonicalPrimitive.end() ? base : it->second;
}

class LLVMBackend
{
public:
    // Width of the `long` keyword, in bits. `long` follows the TARGET's native C ABI:
    // Windows is LLP64 (32-bit long), 64-bit POSIX is LP64 (64-bit long), 32-bit is 32.
    // Set by SetTargetLongWidth() when the target platform is resolved; a static because
    // the nested TypeAndValue struct and the static BitfieldStorageBits() both need it,
    // and one process compiles for exactly one target at a time. `long long` is always
    // i64 (spelled as such by ParseDeclarationSpecifiers), and `int` is always i32.
#if defined(_WIN32)
    static inline int longBits_ = 32;
#else
    static inline int longBits_ = 64;
#endif

    static void SetTargetLongWidth(bool targetWindows, int platformBits);

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
    void AddVectorizeLoopInfo(const VectorizeLoopInfo& info);

    int ArrayViewBufferFieldIndex(const std::string& typeName);

    void NoteVectorizeSpanAccessor(int loopLine, const std::string& accessor,
                                   const std::string& receiver, int line, int col);

    enum class CallingConv { Default, Stdcall, Cdecl };

    struct TypeAndValue
    {
        std::string TypeName;
        std::string VariableName;
        bool Pointer = false;
        bool ElemPointer = false; // true when this is T** (pointer to pointer), e.g. T* field where T is a pointer type
        // The '*' count POSITIVELY recorded for this value; 0 means NOT RECORDED, never "depth 0"
        // (same convention as FuncPtrParam::PointerDepth). The model caps at 2, and a declarator
        // OVER the cap records 0 - a clamped 2 stepped down by '*' would falsely prove depth 1.
        // Written at five producers only: both ParseDeclarationSpecifiers declarator branches, and
        // the '&' (+1), '*' (-1) and pointer-subscript (-1) steps over an ALREADY-RECORDED depth.
        // A new producer that CHANGES depth must step it too, or it falsely proves the old one.
        int PointerDepth = 0;
        bool IsInterface = false;
        bool IsInterfacePointer = false; // true when T is interface AND this is a pointer TO that fat-ptr (e.g. T* field where T=IMessage, or channel<IMessage*>)
        // True when this type IS one bare interface fat value ({vtable,data} by value). DERIVED,
        // never stored, so it cannot drift. An `IFace[]` array view is a thin POINTER to a run of
        // fat values, not one of them - it must take the pointer paths, so it is excluded here.
        // Prefer this over spelling `IsInterface && !IsInterfacePointer` at a new site.
        bool IsFatInterfaceValue() const { return IsInterface && !IsInterfacePointer && !IsArrayView; }
        bool IsNullable = false;
        bool IsMove = false;     // parameter declared with 'move' - function takes ownership
        bool IsAlias = false;    // return/decl declared with 'alias' - by-value borrow; caller must not free the interior
        // This type came from a generic type argument qualified with `unique`. It records only
        // that provenance - it is set on ANY declaration whose type substitutes to `unique X*`
        // (locals and fields included), not just parameters, because the substitution branch has
        // no parameter-context signal. The move-sink meaning lives in the CONSUMERS, which
        // iterate parameters only (ApplyMoveParamTransfer, DiagnoseExplicitMoveToBorrowParam).
        // A new consumer must therefore check it is looking at a parameter before treating it as
        // a sink; reading it off a local and nulling that source would free what nobody owns.
        // Distinct from IsUnique (the written field/local qualifier), which drives destructor
        // synthesis and must not be set from a substitution.
        bool IsUniqueTypeArg = false;
        // A buffer pointer (`T* _data`, substituted with T = `unique X*` / `unique IFace`) whose
        // indexed ELEMENT is an owning `unique` value. The pointer model strips the element's
        // `unique`-ness on a slot read (a read hands out a borrow, so IsUniqueTypeArg is cleared
        // by the explicit-pointer rule), so this out-of-band flag remembers the element ownership.
        // Read ONLY by the container-slot move-out recovery (ApplyMovedSlotOwnership): it lets
        // `_ = move _data[i]` (which has no destination type to re-derive from) release the element
        // exactly as `T tmp = move _data[i]` does. Plain reads ignore it, so a slot read still
        // demotes to a borrow. Propagated onto the element value by the subscript path.
        bool ElementOwningUnique = false;
        // An `alias` borrow (accessor return such as list's `alias T get`) whose type argument
        // substituted to a `unique X*` element - i.e. the CONTAINER owns the pointee and its
        // destructor frees it. Distinct from IsUniqueTypeArg (suppressed on alias borrows): this
        // records "borrow of an element the owner will free", so a later `delete` of a local bound
        // to it can be rejected as a double-free. Read ONLY by the delete-of-owned-element check.
        bool IsBorrowOfUniqueElement = false;
        // Set by the ForwardRefScanner body-scan on a plain by-value parameter the callee body
        // UNCONDITIONALLY moves (top-level `move <param>`): a synthesized move-sink whose caller
        // source is nulled at the call site. Consumers still gate on the concrete type owning a
        // resource (IsOwningValueType(T) || T=="string"), so a non-owning T stays harmless.
        bool IsOwningSink = false;
        // Set alongside IsOwningSink when the sink was inferred STRUCTURALLY from a CONSUMING STORE
        // (a plain `=`/slot store or a conditional `move`), not an unconditional top-level `move`.
        // The scanner cannot resolve a struct's owns-resource/copyability during the forward pass
        // (the full destructor / copy() may not exist yet), so the consume sink is recorded
        // structurally here and the copyability decision is deferred to OwningSinkConsumesConcrete,
        // evaluated at the call site / definition where the monomorphized type is fully known: such
        // a sink CONSUMES (and exit-drops) only a NON-COPYABLE owner; a copyable owner's store is a
        // COPY, so the param stays a borrow and the caller keeps its value. An unconditional-`move`
        // sink leaves this false and consumes any owner, exactly as before.
        bool IsConsumeInferredSink = false;
        // Pure-borrow element (list<alias T*>) whose owner lives elsewhere; a later delete of a
        // local bound to this accessor result double-frees when the real owner releases it.
        bool IsBorrowOfAliasElement = false;
        bool IsBond = false;     // parameter declared with 'bond' - return value borrows from this parameter; return must not outlive it
        bool IsUnique = false;   // struct/interface field declared with 'unique' - the owner owns this raw pointer; the synthesized destructor deletes it. On an interface it is contract: every implementor must agree (VerifyInterfaceFields)
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
        // The three signature fields below can be populated with IsFunctionPointer still FALSE:
        // the direct call-argument loop copies the SIGNATURE of a stored `function<>`/`Lambda<>`
        // argument without claiming the argument IS one, because setting IsFunctionPointer or
        // TypeName there would re-route unrelated calls through other branches of the scorer.
        // A reader of these three must therefore NOT assume IsFunctionPointer (FuncPtrSignatureOf
        // is the first such reader); a reader of IsFunctionPointer may still assume these.
        std::string FuncPtrReturnTypeName;
        bool FuncPtrReturnPointer = false;
        // Return ownership is semantic metadata carried by a stored function value. It is
        // recovered from the bound function symbol and makes indirect calls follow direct calls.
        bool FuncPtrReturnOwned = false;
        /*
         * Number of '*' on a signature component. 0 means NOT RECORDED, never "not a pointer" -
         * `Pointer` already answers that, and many producers (C-interop, WinRT, synthesized
         * signatures) set only `Pointer`. Readers must therefore treat 0 as unknown: the depth
         * proof is one-sided like every other part of the comparison, so `int**` vs an unrecorded
         * pointer accepts. Only the source-parse sites, which can count Star(), fill it in.
         *
         * Without it `*` and `**` collapsed: function<void(int**)> bound a void(int*) slot and
         * SIGSEGV'd, and the two spelled one overload symbol.
         */
        int FuncPtrReturnPointerDepth = 0;
        /*
         * The declaring-scope-RESOLVED key of a signature component, recorded ALONGSIDE the raw
         * spelling. The spelling still owns the mangled name (BuildEncodedClosureName), so this is
         * a separate field rather than a re-spelling of TypeName. "" means NOT RECORDED, never
         * "no type" - exactly the convention PointerDepth == 0 uses above. Only the source-parse
         * sites fill it, in BOTH passes (the scanner's copy is the one a call site reads); C
         * interop, WinRT and synthesized signatures leave it empty and keep binding. A reader may
         * only NARROW a candidate set the key is already a member of, so a stale key cannot invent
         * a rejection - a type not yet registered when the walk runs simply records nothing.
         */
        std::string FuncPtrReturnResolvedKey;
        struct FuncPtrParam
        {
            std::string TypeName;
            bool Pointer = false;
            bool IsMove = false;
            // Inferred owning sink (never spelled): lets the indirect call site transfer like
            // ApplyMoveParamTransfer. OwningSinkConsumesConcrete filters the structural half.
            bool IsOwningSink = false;
            bool IsConsumeInferredSink = false;
            int PointerDepth = 0;   // 0 = not recorded; see FuncPtrReturnPointerDepth
            std::string ResolvedTypeKey;  // "" = not recorded; see FuncPtrReturnResolvedKey
        };
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

        // Allocation-alignment clause: arg2 of `alignas(slot, alloc)`. Records that the heap BLOCK
        // this pointer/array-view owns is N-aligned so a field/param/return and the matching
        // `new T[n]` agree and the free site routes to __delete_aligned. 0 = unset. Power of two,
        // validated at parse time. NOT the field slot's alignment (that is UserAlignValue / arg1).
        uint64_t AllocAlignValue = 0;

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
        // The mode that grant is seeded in: lock(this) / lock(this.write) -> Exclusive,
        // lock(this.read) -> Shared, lock(this.optimistic) -> Optimistic (reads only).
        LockMode LockThisMode = LockMode::Exclusive;

        bool IsPrimitive() const
        {
            return IsInteger() != -1 || IsUnsignedInteger() != -1 || IsFloatingPoint() != -1
                || TypeName == "bool" || TypeName == "void";
        }

        /*
         * ASYMMETRIC by design: the sole caller (ComputeOverloadFunction) asks
         * `argument.IsTypeMatch(parameter)`, so `this` is the ARGUMENT and `other` the PARAMETER,
         * and the pointer gate below is one-sided on purpose. Do not add a caller that reverses
         * the operands without revisiting it.
         *
         * That gate: a pointer ARGUMENT does not bind a by-value parameter - the raw address would
         * land in the value slot and reach the LLVM verifier. `T` into a `T*` parameter is the
         * working implicit address-of (the caller passes the alloca) and stays a match.
         *
         * The two DEPTH gates below refuse only what BOTH sides PROVE. `ElemPointer == false` is
         * "not recorded", never "depth 1", so the reverse direction reads the positive
         * `PointerDepth` instead - which a `T*[N]` slot, an `&x` over an unrecorded operand and a
         * generic substitution all decline to claim, keeping those working spellings bound.
         * Arrays, views, simd, interfaces and function pointers spell depth about their ELEMENT
         * rather than their own value, so they are excluded from both.
         */
        bool IsTypeMatch(const TypeAndValue& other) const
        {
            if (Pointer && !other.Pointer)
                return false;

            if (PointerDepthRefuses(other))
                return false;

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
            if (TypeName == "i64" || TypeName == "u64")
                return 64;
            // `long`/`ulong` are target-native: 32 bits on Windows/LLP64, 64 on LP64.
            if (TypeName == "long" || TypeName == "ulong")
                return longBits_;

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
            // C's `unsigned long`: target-native width (u32 on Windows/LLP64, u64 on LP64).
            if (TypeName == "ulong") return longBits_;

            return -1;
        }

        int IsFloatingPoint() const
        {
            if (TypeName == "float")  return 32;
            if (TypeName == "double") return 64;
            return -1;
        }

        // '*' count this VALUE carries, for copying into a signature component's PointerDepth.
        // The model caps at 2, so that is the ceiling here.
        int ValuePointerDepth() const
        {
            return (ElemPointer || PointerDepth >= 2) ? 2 : (Pointer ? 1 : 0);
        }

        // A plain `T**` value, proven either by the ElemPointer bit or by a recorded depth >= 2
        // (which is how an inline `&x` over a `T*` proves itself). Both readings hold only when
        // nothing else on the type claims depth for an ELEMENT (array/view/simd) or a fat shape.
        bool IsProvenDoublePointer() const
        {
            return Pointer && (ElemPointer || PointerDepth >= 2)
                && DepthIsAboutThisValue();
        }

        // A POSITIVELY recorded depth-1 pointer. Unlike !ElemPointer (which is "not recorded"),
        // PointerDepth == 1 is a claim, so this is the proof the reverse-direction gate needs.
        bool IsProvenSinglePointerDepth() const
        {
            return Pointer && PointerDepth == 1 && !ElemPointer
                && TypeName != "void" && DepthIsAboutThisValue();
        }

        /*
         * A fixed `T[N]` ARGUMENT arrives as the element-0 address, so its decayed value is one
         * level deeper than the ELEMENT that Pointer/ElemPointer describe: `T*[N]` decays to a
         * proven `T**`. Plain `T[N]` decays to `T*` and is a sanctioned spelling, so only a
         * POINTER element claims anything here. Views claim nothing (a view is a fat value that
         * binds through its own gate), and `ElemPointer` on the element would mean depth 3.
         */
        bool IsProvenDecayedDoublePointer() const
        {
            return ConstArraySize > 0 && Pointer && !ElemPointer && !IsArrayView && !IsSimd
                && !IsInterface && !IsInterfacePointer && !IsFunctionPointer
                && TypeName != "void";
        }

        /*
         * The pointer-DEPTH half of IsTypeMatch, split out for the two judges that do not call
         * IsTypeMatch: the scorer's numeric fallback (which re-granted `int**` into `int*`,
         * because both spell the 32-bit "int") and its empty-TypeName branch, where opaque
         * pointers make every pointer pair identical. `this` is the ARGUMENT, `other` the
         * PARAMETER - the same asymmetry IsTypeMatch documents.
         */
        bool PointerDepthRefuses(const TypeAndValue& other) const
        {
            if ((IsProvenDoublePointer() || IsProvenDecayedDoublePointer())
                && other.IsProvenSinglePointer())
                return true;

            // The mirror: a POSITIVELY depth-1 argument does not bind a proven `T**` parameter.
            // Both sides must be proven, so an unrecorded depth (0) still accepts.
            return IsProvenSinglePointerDepth() && other.IsProvenDoublePointer();
        }

        // A plain `T*` value. `void*` is excluded because every pointer converts to it: without
        // that the overload dump claimed a `T**` cannot reach a `void*` param, which is false.
        bool IsProvenSinglePointer() const
        {
            return Pointer && !ElemPointer
                && TypeName != "void" && DepthIsAboutThisValue();
        }

        bool DepthIsAboutThisValue() const
        {
            return !IsArrayView && !IsSimd && ConstArraySize == 0 && AliasArraySize == 0
                && !IsInterface && !IsInterfacePointer && !IsFunctionPointer;
        }

        /*
         * The overload IDENTITY of this type: two parameters are the same overload slot exactly
         * when this string matches. Every spelling goes through CanonicalPrimitiveSpelling, so
         * `f(int)` and `f(i32)` are ONE overload rather than two - which is what the language
         * reference already promises and what monomorphization now does for `Box<int>`. Without
         * it the two coexisted and picked each other's arguments (`f(1)` reached the `i32` body
         * while an `i32` variable reached the `int` one).
         */
        std::string ToUniqueString() const
        {
            if (IsFunctionPointer)
            {
                // The indirection marker rides the generated PREFIX, not the tail: a tail suffix
                // would collide with a trailing pointer param (`function<int(int*)>` vs `function<int(int)>*`).
                std::string kind = IsThinFnPtr() ? "cfuncptr" : "funcptr";
                if (IsArrayView)   kind += "Arr";
                else if (Pointer)  kind += (ElemPointer ? "PtrPtr" : "Ptr");
                // One "Ptr" per level, so function<void(int*)> and function<void(int**)> are two
                // overloads. An unrecorded depth still yields exactly one "Ptr", which is the
                // string every producer emitted before depth existed.
                auto ptrTail = [](bool ptr, int depth) {
                    std::string t;
                    for (int i = 0, n = ptr ? std::max(depth, 1) : 0; i < n; i++) t += "Ptr";
                    return t;
                };
                std::string s = kind + "_"
                              + CanonicalPrimitiveSpelling(FuncPtrReturnTypeName)
                              + ptrTail(FuncPtrReturnPointer, FuncPtrReturnPointerDepth);
                for (const auto& p : FuncPtrParams)
                    s += "_" + CanonicalPrimitiveSpelling(p.TypeName)
                       + ptrTail(p.Pointer, p.PointerDepth) + (p.IsMove ? "M" : "");
                return s;
            }

            std::string type = CanonicalPrimitiveSpelling(TypeName);

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

        // The BARE brace spelling of a field default (`Inner i { x = 1 };`, no '='), which the
        // grammar hangs on initDeclarator itself so `Initializer` is null for it.
        CFlatParser::InitializerListContext* BraceInitializer = nullptr;

        // Used for array - first (outer) dimension; extra inner dimensions in ExtraArrayDims
        CFlatParser::AssignmentExpressionContext* ArraySize = nullptr;
        std::vector<CFlatParser::AssignmentExpressionContext*> ExtraArrayDims;

        // Used for default parameter values
        CFlatParser::InitializerContext* DefaultValue = nullptr;

        bool external = false;
        bool threadLocal = false;
        // `static` storage class as written. On a LOCAL it selects module-global storage with a
        // run-once initializer; on a file-scope global it is accepted and carries no meaning yet.
        bool staticStorage = false;

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
        // True on the synthesized `__padN` ([N x i8]) slots from PadFieldsForAlignment. Not a
        // user-visible member: skipped by reflection, JSON, DWARF, dtor/copy and LSP field lists.
        bool IsPadding = false;

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
        // compile-time: this argument was written 'move x' at a call site and is a VALUE type
        // (string/owning struct/closure). Zeroing is deferred to ApplyMoveParamTransfer so the
        // callee's parameter move-ness is known first. Not part of the --init cache round-trip.
        bool IsExplicitMove = false;
        // compile-time: the occurrence id (see currentCastOccurrence_) this argument's own value
        // was produced under, stamped when a call-argument's evaluation finishes. Lets a DEFERRED
        // gate (ArgumentIsCodeValue / ArgumentIsProvablyDataPointer, run after every sibling
        // argument in the same call is already evaluated) ask the launder "was THIS occurrence's
        // cast the one that produced this value", instead of "was this value ever cast anywhere in
        // the statement" - the latter is exactly the same-statement collision this field closes.
        // 0 means "not stamped" (never evaluated inside a call-argument slot) and can never match a
        // real registration (currentCastOccurrence_ is reset to 0 between statements too, so an
        // unstamped argument correctly falls back to the old whole-statement behaviour). Live
        // compile-time state derived from llvm::Value* identity - not part of the --init cache
        // round-trip, same rule as IsExplicitMove above.
        size_t CastOccurrenceId = 0;
        bool MovedIntoInterface = false; // compile-time: ownership was boxed into an interface local ('IFace x = ptr'); 'delete ptr' is a no-op that leaks - delete the interface instead
        // compile-time: this array-view LOCAL's DECLARATION bound it from fixed-array storage
        // (proven, never reassigned since); 'delete' would hand free() a non-heap address.
        bool ViewOfFixedArrayStorage = false;
        std::string ViewOfFixedArraySourceName; // name of the fixed array it was bound from, for the diagnostic
        // compile-time: the box this interface LOCAL was last bound to is PROVEN to be one a
        // different owner already frees. Cleared by any later binding. See SetInterfaceBoxIsBorrowed.
        bool BorrowedInterfaceBox = false;
        std::string BorrowedInterfaceBoxSource; // pre-rendered owner list, for the diagnostic
        // Sticky: set once any binding hands this local a box that is NOT proven, and never cleared.
        // From then on the local can never be rejected - walk order over the AST is not control flow.
        bool InterfaceBoxProvenanceUnknown = false;
        // compile-time: this POINTER binding was reassigned by a plain '='. Every "someone else
        // frees this" fact established at its declaration is stale from here on.
        bool PointerRebound = false;
        /*
         * PointerRebound means "was assigned to", NEVER "now holds an owner": the plain '=' sets it
         * for `b = q;` between two borrows and for a self-assign alike. A consumer that RETIRES a
         * safety fact must ask these two instead - the RHS was a provably OWNED value (the '=' path's
         * srcIsOwnedPtrRhs), and the store sat in the same BASIC BLOCK the consumer is reached from,
         * which is what keeps a never-taken `if (b == nullptr) { b = new T(); }` from retiring
         * anything (see internal/issue/p2/conditional-store-retires-borrow-facts-unconditionally.md).
         */
        bool ReboundToOwnedValue = false;
        llvm::BasicBlock* ReboundBlock = nullptr;
        llvm::Function* ReboundFunction = nullptr;   // paired with ReboundBlock; see BorrowProofRetiredByRebind
        // Set by that same '=' when the RHS binding itself proved another owner, so the proof is
        // carried across the store instead of retired. Refreshed on every '='; see MarkPointerRebound.
        bool InheritedKeepsOwner = false;
        std::string InheritedKeepsOwnerSource; // that proof's rendered owner name, for the diagnostic
        // compile-time: this POINTER binding was bound from a '?:' / '??' JOIN whose EVERY non-null
        // arm proved another owner. A join carries no source binding, so no source-keyed clause can
        // see it; recorded where the arms are in hand and re-asked at the delete / store sites.
        // Deliberately NOT InheritedKeepsOwner: that one is also set by the plain `p = c;` store,
        // which IMPLIED-MOVES the pointee out of `c`, so `delete p;` there is correct.
        bool JoinKeepsOwner = false;
        std::string JoinKeepsOwnerSource; // that join's rendered owner name, for the diagnostic
        // Every proving ARM's SLOT, so the consumers can re-ask whether they ALL still prove.
        // Without these the proof retires only when the JOIN end is rebound, and nulling or
        // rebinding an ARM leaves it stale - a false rejection whose remedy leaks.
        std::vector<llvm::Value*> JoinKeepsOwnerSlots;
        // The rebinding was a '??=', which stores CONDITIONALLY, so the element refresh takes the
        // JOIN and the declaration's element fact can survive. Suppresses the element clause of the
        // BOXING proof only - the raw-delete guard reads BorrowsOwnedElement directly and is
        // deliberately untouched.
        bool CoalesceRebound = false;
        // compile-time: explicitly 'move'd-out thin pointer local - null but plain-readable by
        // design; only a same-block '->'/'.'/'*'/'[]' DEREFERENCE is rejected (see MarkVariableExplicitlyMovedNull).
        bool ExplicitlyMovedNull = false;
        llvm::BasicBlock* ExplicitNullBlock = nullptr; // the block the move was recorded in - the deref guard only fires inside this same block
        bool AddressEscaped = false; // '&name' was taken - the pointee may have been rewritten through it, so the deref guard is latched off for good
        std::unordered_set<std::string> MovedFields; // compile-time: field names moved out of this variable via a 'move' of a sub-path (e.g. `node->left`) - the base stays usable
        // compile-time: `static` local - Storage is a module global with program lifetime, not an
        // alloca. Never destructed at scope exit (see DropValue); it outlives every call.
        bool IsStaticLocal = false;
        bool IsBonded = false;           // compile-time: true when this variable holds a bonded (borrowed) return value
        bool BondByAddress = false;      // bond originates from a by-address lambda capture; reassigning the source is safe
        std::vector<std::string> BondedSources; // names of bond parameters this value borrows from
        // Capture names for lambda literals, in capture order. Empty for non-capturing lambdas.
        // Used to diagnose capturing lambdas passed to C function-pointer params (C ABI can't carry state).
        std::vector<std::string> LambdaCaptureNames;
        bool IsBorrowed = false;         // compile-time: true for non-move pointer parameters and locals that alias one - 'delete' is forbidden
        // Set alongside IsBorrowed when the borrow was read out of a FIELD of the origin rather
        // than being the origin itself, so no diagnostic prescribes `move <origin>` (another object).
        bool BorrowedThroughField = false;
        // compile-time: true for a plain by-value OWNING-VALUE parameter (string / owning struct /
        // fat closure) that is NOT a sink (no `move` in body, not `move`/unique). Such a param
        // bitwise-aliases the caller's value - the caller keeps ownership - so feeding it into a
        // CONSUMING param (sink/`move`/unique) or moving it out would launder ownership and double-free.
        bool IsBorrowedOwningValue = false;
        bool IsAliasBorrow = false;      // compile-time: local bound from an `alias` return - shallow-aliases storage it does not own, so its scope-exit destructor is suppressed
        // The block/function the borrow BINDING was created in. A rebind emitted in that same block
        // runs on every path that reaches scope exit, which is the only case the borrow may retire.
        llvm::BasicBlock* AliasBorrowDeclBlock = nullptr;
        llvm::Function* AliasBorrowDeclFunction = nullptr;
        // compile-time: this local is a lambda body's unpacked BY-VALUE capture of an owning value
        // type. The closure ENV owns the buffer; this local only borrows it, so handing it to a
        // caller (a `return`) must hand over an independent copy, not the env's storage.
        bool IsClosureValueCapture = false;
        // The block a plain '=' recorded this borrow in (null when the borrow came from the
        // DECLARATION). An owned rebind in that same block retires it; see RetireAssignBorrow.
        llvm::BasicBlock* AssignBorrowBlock = nullptr;
        std::string BorrowedOrigin;      // name of the borrowed parameter this value transitively aliases (for diagnostics)
        // Set to "Struct.field" when the borrow originates from a `unique` field rather than a
        // borrowed parameter, so the delete/store diagnostics can name the real owner (Trap B).
        std::string BorrowedUniqueField;
        // True when BorrowedUniqueField was proven through a CALL rather than a direct field read.
        // The owner is then only nameable as "Struct.field", so `move <origin>` is not spellable
        // at this site and every diagnostic must offer a different remedy.
        bool BorrowedUniqueFieldViaCall = false;
        // True when this NamedVariable reads a `unique` field. A cast severs Storage and rewrites
        // TypeAndValue, so this out-of-band flag preserves the field provenance the borrow rules
        // key on (delete/move/store-into-field), keeping `(T*)b.p` a tracked alias.
        bool IsUniqueFieldAlias = false;
        // compile-time: this local is bound from an accessor return that borrows an element the
        // owning CONTAINER frees (list's `alias T get` with a `unique X*` element). A `delete` of
        // this local double-frees. Consulted ONLY by the delete-of-owned-element check; kept off
        // the general IsBorrowed path so it does not affect store/return/move diagnostics.
        bool BorrowsOwnedElement = false;
        // Set alongside BorrowsOwnedElement when the element is a list<alias T*> pure borrow (owner
        // elsewhere) rather than a container-owned unique element. Selects the delete message only.
        bool BorrowedElementExternallyOwned = false;
        std::string OwnedElementContainer; // container variable name, for the delete diagnostic
        // compile-time: this pointer local's DECLARATION plainly copied a live OWNING local
        // (`T* b = c;` / `alias T* b = c;`), which still frees the pointee at its own scope exit.
        // Retired by PointerRebound like the other declaration-time facts. Kept off the general
        // IsBorrowed path so it drives only the delete guards, not store/return/move diagnostics.
        bool BorrowsOwningLocal = false;
        std::string OwningLocalOrigin; // that owning binding's name, for the diagnostic
        // The owning binding's SLOT, so the delete sites can re-ask whether it STILL owns. Rebinding
        // the SOURCE (`c = new T();`) makes this copy the sole owner of what it holds.
        llvm::Value* OwningLocalStorage = nullptr;
        llvm::Value* RefCountStorage = nullptr; // lazy i32 alloca at function entry; non-null only when pointer escaped to a field
        std::string CallerName;          // the variable's name at the call site, for move tracking
        std::string OwningStructName;    // when this NamedVariable is a struct-field access, the field's owning struct
        std::string FieldName;           // when this NamedVariable is a struct-field access, the field name
        // Root VARIABLE of a field path ("w" for `w.a.b`). TypeAndValue.ParentVariableName names
        // only the IMMEDIATE parent, which on a nested path is an intermediate field, not a variable.
        std::string FieldPathRoot;
        // Answered at the field-access site, where the root's binding is RESOLVED: that root is the
        // current function's borrowed (non-`move`) by-value struct parameter. Re-asking downstream by
        // NAME cannot distinguish the parameter from an inner local that shadows it.
        bool RootIsBorrowedByValueParam = false;
        // POSITIVE provenance: this raw `T*` LOCAL holds a `new T[n]` allocation, whose elements
        // nothing ever frees. Only such a base takes the raw-heap borrow arms; every other binding
        // (a decayed fixed array, a join, a parameter, an unknown source) keeps the plain store.
        bool AllocatedByRawNewArray = false;
        // Runtime element count for a raw `new T[n]` pointer local. Used to destroy every
        // constructed element before releasing the raw block; null means scalar/unknown.
        llvm::Value* RawArrayLength = nullptr;
        // Same question for an `alias`-BORROW local root (`Box k = w.get(); move k.item;`): answered
        // where the root binding is RESOLVED, since a downstream name lookup cannot see a shadow.
        bool RootIsAliasBorrowLocal = false;
        // Field reached THROUGH an interface value (data ptr + the vtable's byte-offset slot). The
        // address is a byte GEP, not a 2-index struct GEP, so field-store rules must be told.
        bool IsInterfaceField = false;
        // Field extracted from a BY-VALUE owning-struct temp (`makeToken().text`): may be read, but
        // persisting it (store/bind/return) double-frees so those sites reject it. See FlushOwnedStructTemps.
        bool FromOwningTempField = false;
        // The parent temp OWNS this field (not an `alias` borrow return). Distinct from
        // MovableTempField below, which also requires the FIELD to be an owning value type.
        bool OwningTempParent = false;
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
        // True when a subscript '[i]' produced this NamedVariable (an array/pointer element).
        // Move-dataflow treats index/deref lvalues as untracked (permissive), so USE-recording
        // skips these - moving out of a container slot is neither checked nor flagged.
        bool IsElementAccess = false;
        // True when the subscript's BASE binding was a user-visible `T[]` array VIEW. The element
        // GEP has a container buffer's single-index shape, so this is the only positive signal
        // separating a view's LIVE slots from a container's internal `T*` slot.
        bool IsViewElement = false;
        // compile-time: Storage points at a SPILL of a by-value temporary (an inline array field
        // extracted out of a returned temp). The copy is shallow and dies with the full expression,
        // so reads are fine but 'move' and stores through it are rejected.
        bool IsTempSpillStorage = false;

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
        // Pointer DEPTH of the operand, carried so an operator's right operand can be judged:
        // the operator path reduces it to a raw llvm::Value and 0 means "not recorded".
        int          pointerDepth = 0;
        bool         elemPointer  = false;

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

        // Set when this scope was entered via a `lock` statement. unlock() is called
        // on scope exit (return, or normal block close). A multi-lock `lock(a, b)`
        // acquires several mutexes in one scope, so every acquired lock needs its own
        // cleanup entry - releasing only the first leaves the rest locked, and a still
        // -locked mutex trips its `unique` slot destructor into freeing a live lock.
        struct LockCleanup
        {
            llvm::Function* UnlockFn = nullptr;
            llvm::Value*    MutexPtr = nullptr; // pointer to the mutex struct
        };
        std::vector<LockCleanup> lockCleanups;

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
    // Set by ParseMoveExpression when the move source is a container element SLOT (`move _data[i]`,
    // a single-index-GEP subscript); consumed by ParseDeclaration to re-derive a dropped local's
    // ownership from the DEST type (the element read demoted `unique` away). Reset at the start of
    // ParseAssignmentExpressionNamed alongside lastOwningResult, so it reflects only THIS RHS.
    bool lastMovedFromContainerSlot = false;
    uint64_t lastAllocAlignment = 0;         // set by ParseNewExpression for `new T[n] alignas(N)` (>16); consumed by ParseDeclaration into NamedVariable.AllocAlignment
    // Inbound alloc-align channel (symmetric to lastAllocAlignment). Set from the target
    // declaration's AllocAlignValue BEFORE evaluating a DIRECT `new` initializer/RHS; a bare
    // `new` reads it to drive the aligned allocator. One-shot: consumed+cleared in ParseNewExpression.
    uint64_t pendingInitAllocAlign = 0;
    // Return-result alloc-align channel (mirror of lastCallReturnsOwned). Set from the callee's
    // return-type AllocAlignValue at the call; consumed in ParseDeclaration into NamedVariable.AllocAlignment.
    uint64_t lastCallReturnsAllocAlign = 0;
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

    // Discard-detection ledger: SSA results of owning-RETURN calls, keyed by value for identity.
    // This ledger answers only whether the no-discard diagnostic must fire. Release eligibility
    // lives in ownedReturnReleaseTemps_ so erasing a release entry cannot hide a diagnostic and
    // a diagnostic entry cannot accidentally authorize a free.
    struct OwnedReturnTemp
    {
        llvm::Value* Value;
        std::string FnName;
    };
    std::vector<OwnedReturnTemp> ownedReturnTemps_;

    // Release ledger for owning-return values. A release entry is removed when ownership is
    // adopted or suppressed; its presence never controls the no-discard diagnostic.
    struct OwnedReturnReleaseTemp
    {
        llvm::Value* Value;
        std::string TypeName;
        uint64_t AllocAlign = 0;
        bool IsOwningPtr = false;
    };
    std::vector<OwnedReturnReleaseTemp> ownedReturnReleaseTemps_;

    // Owning-POINTER SSA values produced by `new`, keyed by value identity, plus any value they
    // propagate onto (a '?:' phi/select whose arm is one). Lets a `unique T*` assignment ask
    // "does the RHS VALUE carry ownership" instead of "what does the RHS look like". A value
    // reaching a call's RESULT is a different value, so `b = addr(new R())` (borrow return) is
    // never in it. Retired each full expression.
    // TypeName/AllocAlign give a raw `new` the same free-site identity an owning RETURN has, so a
    // provably non-escaping consuming site can register it in pendingOwnedPtrTemps.
    struct OwnedNewTemp
    {
        llvm::Value* Value;
        std::string TypeName;
        uint64_t AllocAlign = 0;
    };
    std::vector<OwnedNewTemp> ownedNewTemps_;

    // Per-(callee, parameter) result of ParameterRetainsArgument. Cached only for callees whose
    // body is complete. Keyed on a raw llvm::Function*, so an entry MUST be dropped before that
    // function dies or LLVM could hand the same address to a later Function::Create: see
    // ForgetFunctionEscapeMemo (erasure) and ResetForReanalysis / DropModuleEscapeMemo (module
    // rebuilds - ResetForReanalysis is not the only one).
    static constexpr int kMaxRetainDepth = 8;
    static constexpr size_t kMaxRetainUses = 256;
    std::map<std::pair<const llvm::Function*, unsigned>, bool> paramRetainsMemo_;
    std::set<std::pair<const llvm::Function*, unsigned>> paramRetainsInProgress_;
    // Separate cycle guard for ParameterProvablyRetainsArgument: the two analyses answer opposite
    // questions, so a key left by one must never terminate the other's walk.
    std::set<std::pair<const llvm::Function*, unsigned>> provableRetainsInProgress_;
    // Third cycle guard, for ParameterMayReachReturn. Same reason: a key left by either escape
    // walk must never terminate the return walk, which asks a different question again.
    std::set<std::pair<const llvm::Function*, unsigned>> mayReachReturnInProgress_;
    // Cycle guard for the select/phi arm walk in MemoryOutlivesCall: a loop-carried phi reaches
    // itself, and re-entering it must not be read as an arm that names local memory.
    mutable std::set<const llvm::Instruction*> joinAddressInProgress_;
    // Functions whose emission is SUSPENDED while a nested one is emitted (lambda invoker,
    // generic instantiation, global-array initializer). Pushed/popped by Save/RestoreBuilderState.
    std::vector<const llvm::Function*> suspendedFunctions_;

    // Concrete element (class) TypeName behind a pointer SSA value produced by `new`, keyed by
    // value identity. A '?:' phi carries no NamedVariable TypeName, so the interface upcast
    // recovers each arm's class from here. Retired with ownedNewTemps_.
    std::vector<std::pair<llvm::Value*, std::string>> valueElementTypeNames_;

    // Interface name a fat interface value ({vtable,data}) was produced against, keyed by value
    // identity (see RegisterFatInterfaceValueTypeName). Retired with valueElementTypeNames_.
    std::vector<std::pair<llvm::Value*, std::string>> fatInterfaceValueTypeNames_;

    /*
     * What each class->interface boxing site actually boxed, recorded BY the boxing site instead
     * of recovered afterwards by walking emitted IR (which is how the return-path lifetime check
     * has to work today, and why it keeps missing shapes). Keyed by value identity on both the
     * produced fat value and its data half, so a '?:' join is answerable through either. Lives
     * for the whole function body - the guards that read it run in a LATER statement than the
     * boxing - and is parked/cleared with the other per-function ledgers.
     */
    enum class InterfaceBoxSource
    {
        Unknown,
        FrameStorage,  // an alloca in the boxing frame - the fat pointer dies with the frame
        Heap,          // a `new` result / owning pointer local - somebody must delete the box
        Parameter,     // a pointer parameter - the caller owns the lifetime
        Global         // a global - outlives everything
    };

    struct InterfaceBoxRecord
    {
        llvm::Value* FatValue = nullptr;
        llvm::Value* DataPointer = nullptr;
        std::string SourceClassName;
        std::string InterfaceName;
        InterfaceBoxSource Source = InterfaceBoxSource::Unknown;
        // The owning source was nulled and marked moved at this site, so the box is the sole owner.
        bool OwnershipTransferred = false;
        // PROVEN at the boxing site: a nameable OTHER owner frees this object anyway, so deleting
        // the box double-frees. See BindingKeepsOwnershipOfBoxedObject for what does and does not count.
        bool SourceKeepsOwner = false;
        // A join ARM boxed off a binding PROVABLY parked at null. It owns nothing, so the join-wide
        // ledger skips it as NEUTRAL instead of reading SourceKeepsOwner=false as a blocking arm.
        bool SourceProvablyNull = false;
        // The AST spelling of that other owner, resolved where the binding was still in hand.
        std::string SourceDisplayName;
    };

    /*
     * Holds raw llvm::Value* and is never retired mid-function, so it depends on an invariant:
     * any site that erases instructions mid-body must either be bracketed by SaveBuilderState /
     * RestoreBuilderState (which clear and then overwrite this ledger) or be followed by the
     * per-function clear before anything queries it. Every erasure site today satisfies that.
     * Adding an unbracketed mid-function erasure would let a freed Value* be recycled and match
     * a stale FrameStorage record - a spurious taint for RunInterfaceReturnDangleCheck.
     */
    std::vector<InterfaceBoxRecord> interfaceBoxRecords_;

    void RegisterInterfaceBox(const InterfaceBoxRecord& record);

    const InterfaceBoxRecord* FindInterfaceBoxByFatValue(const llvm::Value* value) const;

    // The only data-pointer lookup, deliberately provenance-filtered: a caller must say which
    // kind of box it means, so a record of another kind can never answer for it. Registration
    // dedupes only an identical (fat value, data pointer, source) tuple.
    const InterfaceBoxRecord* FindInterfaceBoxByDataPointer(const llvm::Value* value,
                                                            InterfaceBoxSource source) const;

    /*
     * The arms of a '??' join, recorded where the lowering still knows them. Unlike '?:' - whose
     * result is a PHI the boxing path can read arms off - '??' joins through a slot, so the joined
     * value is a plain load and the arms are unrecoverable downstream. Same lifetime and the same
     * park/clear points as interfaceBoxRecords_, and the same raw-Value* invariant.
     */
    struct NullCoalesceJoinArm
    {
        llvm::Value* Value = nullptr;
        llvm::BasicBlock* Block = nullptr;
    };

    struct NullCoalesceJoin
    {
        llvm::Value* Joined = nullptr;
        std::vector<NullCoalesceJoinArm> Arms;
    };

    std::vector<NullCoalesceJoin> nullCoalesceJoins_;

    void RegisterNullCoalesceJoin(llvm::Value* joined, std::vector<NullCoalesceJoinArm> arms);

    const NullCoalesceJoin* FindNullCoalesceJoin(const llvm::Value* value) const;

    /*
     * The cast occurrence (see codeValueDataCasts_) each ARM of a join was evaluated under, keyed
     * by the joined value and the arm's INDEX - not its value, since the two arms of one join are
     * routinely the same shared llvm::Function* / GlobalVariable constant. Both arms otherwise sit
     * inside the SAME argument slot and therefore the same occurrence, so a cast on one arm and a
     * bare mention on the other form one identical (value, occurrence) ledger key and the cast
     * launders both. Same lifetime and the same park/clear points as nullCoalesceJoins_.
     */
    struct JoinArmOccurrence
    {
        const llvm::Value* Joined = nullptr;
        unsigned Index = 0;
        size_t Occurrence = 0;
    };

    std::vector<JoinArmOccurrence> joinArmOccurrences_;

    void RegisterJoinArmCastOccurrence(const llvm::Value* joined, unsigned index, size_t occurrence);

    // The occurrence arm `index` of `joined` evaluated under, or `fallback` when unrecorded (a
    // join built by a path that does not scope its arms, or one whose record has been retired).
    size_t JoinArmCastOccurrence(const llvm::Value* joined, unsigned index, size_t fallback) const;

    /*
     * Re-register `value`'s launder from occurrence `from` under the CURRENT ambient occurrence.
     * An arm-scoped cast is invisible outside its arm by construction; when a ternary collapses to
     * one arm and hands that arm's value out directly there is no join left to consult the
     * per-arm record, so the launder has to be carried up to the slot the value now occupies.
     */
    void PromoteCastOccurrence(llvm::Value* value, size_t from);

    /*
     * Values PROVEN to be code - a function pointer or closure in its plain VALUE shape - keyed by
     * value identity and recorded at the read where the declared facts are still in hand
     * (LoadNamedVariable, beside the fat-interface ledger). A join carries none of those facts: the
     * '?:' spelling is a PHI and the '??' spelling a load out of a slot, and under opaque pointers
     * an arm is an indistinguishable `ptr`. Recording cannot reject, so a read this misses degrades
     * to no diagnostic. Same lifetime and park/clear points as nullCoalesceJoins_.
     */
    std::vector<llvm::Value*> codeValues_;

    /*
     * Values an EXPLICIT cast to a data type produced. A ptr->ptr cast is a no-op under opaque
     * pointers, so the cast result IS the ledgered arm value - without this, `(Rec*)w` and
     * `(Rec*)(c ? w : w2)` would be refused by the very escape hatch the rejection advises.
     *
     * STATEMENT-SCOPED, unlike codeValues_, and that scope is load-bearing: a NAMED FUNCTION is a
     * module-level llvm::Function constant, so every mention of `ro` in a function is the SAME
     * Value. Held for the whole function, one `(void*)ro` would launder `ro` for every later gate
     * in that function - measured, memory-unsafe. FlushOwnedTemps (the block-item boundary) retires
     * it, which is strictly later than every gate that reads it and no later than the next
     * statement.
     *
     * OCCURRENCE-KEYED: each entry pairs the value with the currentCastOccurrence_ id live when the
     * cast was registered, not the value alone. A statement-scoped, value-only ledger still let a
     * laundering cast and a BARE join of the SAME named function inside ONE statement launder each
     * other when they are two DIFFERENT call arguments - `two((void*)ro, c ? ro : n)` compiled clean
     * and exited 138, because argument 0's cast registered `ro`'s Function* and argument 1's join
     * arm (an unrelated, uncast mention of the same shared constant) matched it by value identity
     * alone. currentCastOccurrence_ is bumped fresh for the duration of evaluating each independently
     * -gated call argument (BeginCastOccurrence/EndCastOccurrence in the argument-evaluation loops),
     * so a cast in one argument's occurrence can never satisfy a lookup made under a sibling
     * argument's occurrence. A value evaluated OUTSIDE any call-argument slot (a plain assignment,
     * decl-init, return, ...) is never re-occurrence'd mid-statement, so it keeps the prior
     * whole-statement value-only behaviour (both sides of the comparison share the ambient id, 0
     * unless nested inside a call argument) - correct because a single-target store site never has a
     * sibling independently-gated expression to collide with in the same statement.
     *
     * NOT FULLY CLOSED: two residuals remain, filed rather than fixed here. (1) Both arms of ONE
     * join share a single occurrence (the argument/field slot that contains the join), so a cast on
     * one arm still launders its sibling arm - argument granularity is structurally one level too
     * coarse to separate them (see join-arm-cast-launders-sibling-arm.md). (2) Interface-method
     * dispatch never consults this ledger at all, cast or bare - it binds through the vtable slot's
     * declared signature, not the overload scorer this gate is wired into (see
     * interface-dispatch-argument-ungated-for-code-values.md).
     */
    std::vector<std::pair<llvm::Value*, size_t>> codeValueDataCasts_;

    // See codeValueDataCasts_'s occurrence-keying note. currentCastOccurrence_ is the AMBIENT
    // occurrence id: 0 between statements and for any expression evaluated outside a call-argument
    // slot, and a fresh id for the duration of evaluating one call argument (BeginCastOccurrence
    // saves the caller's id and returns a new one; EndCastOccurrence restores it, so nested calls
    // compose correctly without a real occurrence stack). nextCastOccurrence_ is the monotonic
    // generator; it is never reused and never needs resetting mid-compile, only at ResetForReanalysis
    // for hygiene between files. 0 is reserved and never handed out by BeginCastOccurrence, so an
    // un-stamped NamedVariable::CastOccurrenceId (default 0) can only ever match the ambient state,
    // never a real call-argument occurrence - the safe default.
    size_t nextCastOccurrence_ = 1;
    size_t currentCastOccurrence_ = 0;

public:
    // Bump to a fresh occurrence id for the duration of evaluating ONE independently-gated
    // expression (today: one call argument). Returns the PRIOR (caller's) occurrence id - save it
    // and pass it to EndCastOccurrence once that expression's evaluation is complete, to restore
    // the caller's (possibly ambient, possibly outer-call) occurrence so nested calls compose
    // correctly. Read CurrentCastOccurrence() right after calling this to get the fresh id to stamp
    // onto the resulting NamedVariable::CastOccurrenceId.
    size_t BeginCastOccurrence() { size_t saved = currentCastOccurrence_; currentCastOccurrence_ = nextCastOccurrence_++; return saved; }

    void EndCastOccurrence(size_t saved) { currentCastOccurrence_ = saved; }

    // RAII form of the pair above. LogError THROWS, so a hand-rolled restore is skipped on the
    // unwind path and the bumped id leaks into the next expression.
    struct CastOccurrenceScope
    {
        LLVMBackend* Backend = nullptr;
        size_t Saved = 0;
        size_t Id = 0;
        explicit CastOccurrenceScope(LLVMBackend* backend) : Backend(backend)
        {
            Saved = Backend->BeginCastOccurrence();
            Id = Backend->CurrentCastOccurrence();
        }
        ~CastOccurrenceScope() { Backend->EndCastOccurrence(Saved); }
        CastOccurrenceScope(const CastOccurrenceScope&) = delete;
        CastOccurrenceScope& operator=(const CastOccurrenceScope&) = delete;
    };

    size_t CurrentCastOccurrence() const { return currentCastOccurrence_; }

private:

    /*
     * Values read out of a `unique` field of an owning TEMPORARY this statement destructs
     * (`makeBox().t`), keyed by value identity and recorded at the read, where the temp
     * provenance (FromOwningTempField + OwningTempParent) is still on the NamedVariable. A CAST
     * rewrites TypeAndValue and a JOIN produces a PHI or a load out of a slot, so both spellings
     * lose every declared fact the persist-site guard reads. Recording cannot reject, so a read
     * this misses degrades to no diagnostic, never to a false rejection.
     *
     * STATEMENT-SCOPED, retired by FlushOwnedTemps at the block-item boundary - the same boundary
     * that runs the temp's destructor, so an entry never outlives the dangle it describes.
     */
    std::vector<llvm::Value*> owningTempUniqueFields_;

    void RegisterOwningTempUniqueField(llvm::Value* value);

    bool IsLedgeredOwningTempUniqueField(const llvm::Value* value) const;

    /*
     * Does this value, or any ARM of a '?:' / '??' join reaching it, carry a temp's `unique`
     * field? ANY arm answers yes - one dangling arm is enough to leave the destination pointing
     * at freed memory - and a join of two ordinary reads answers no because neither arm was ever
     * ledgered. Mirrors JoinCarriesCodeValue, including the depth cap that terminates a PHI cycle.
     *
     * BOTH '??' arms count. The fallback arm was once excluded because `nullcoal_null` got no
     * per-arm flush, so its temp was never destructed and rejecting it would have refused a
     * program that ran correctly. That arm now gets the same FlushOwnedTempsSince the '?:' arms
     * get (ParseConditionalExpression), so its temp really is destructed inside the arm and the
     * read really does dangle - the exclusion had to be deleted in the same change.
     */
    bool JoinCarriesOwningTempUniqueField(const llvm::Value* value, int depth = 0) const;

    /*
     * A call RESULT that was ledgered above because the call LAUNDERED a temp's `unique` field
     * through a borrowing callee that may hand its parameter back. Carries the provenance the
     * bare CallInst has none of, so the escape diagnostic can name the callee and the field
     * instead of the cast/join wording, which is false at a laundered site.
     */
    struct LaunderedTempUniqueField
    {
        llvm::Value* Result = nullptr;
        std::string CalleeName;
        std::string Access;
    };
    std::vector<LaunderedTempUniqueField> launderedTempUniqueFields_;

    void RegisterLaunderedTempUniqueField(llvm::Value* result, const std::string& calleeName,
                                          const std::string& access);

    // Look through '?:' / '??' arms the same way JoinCarriesOwningTempUniqueField does, so a
    // laundered result reached through a join still names its callee in the diagnostic.
    const LaunderedTempUniqueField* FindLaunderedTempUniqueField(const llvm::Value* value,
                                                                 int depth = 0) const;

    /*
     * The DEFERRED twin of launderedTempUniqueFields_. A callee defined BELOW its call site is
     * still a bare declaration when the call is emitted, so ParameterMayReachReturn correctly
     * reports "no proof" and the eager re-ledger above cannot run. The call result is instead
     * recorded as a CANDIDATE launder: the destination sites record where it was bound, and the
     * question is re-asked at end of module where every body is complete.
     *
     * Conds is a CONJUNCTION so a chain `f(g(x))` works with either callee below: an already
     * proven hop adds nothing, an unanswerable hop adds itself, and the escape holds only when
     * every hop really does hand its parameter back. Same statement scope as the eager ledger.
     */
    struct PendingLaunderTempUniqueField
    {
        llvm::Value* Result = nullptr;
        std::vector<std::pair<const llvm::Function*, unsigned>> Conds;
        std::string CalleeName;
        std::string Access;
    };
    std::vector<PendingLaunderTempUniqueField> pendingLaunderTempUniqueFields_;

    void RegisterPendingLaunderTempUniqueField(llvm::Value* result,
            const std::vector<std::pair<const llvm::Function*, unsigned>>& conds,
            const std::string& calleeName, const std::string& access);

    // Join-aware exactly as FindLaunderedTempUniqueField is, for the same reason.
    const PendingLaunderTempUniqueField* FindPendingLaunderTempUniqueField(const llvm::Value* value,
                                                                          int depth = 0) const;

    /*
     * A DESTINATION that bound a candidate launder. The fact the return half needs is not "did
     * the callee store" but "where did the RESULT get bound", which is known only here, at a
     * point the walk has already passed by end of module - so the SITE is what gets deferred.
     * Module lifetime, like tempUniqueFieldArgs_, and dropped at the same points.
     */
    struct DeferredTempUniqueFieldEscape
    {
        std::vector<std::pair<const llvm::Function*, unsigned>> Conds;
        std::string CalleeName;
        std::string Access;
        std::string DestDesc;
        // A sink PARAMETER destination has its own wording; empty means the ordinary store one.
        std::string SinkFunction;
        std::string SinkParam;
        // The file the destination was written in - by end of module sourceFileName is the main
        // file again, exactly as for TempUniqueFieldArg.
        std::string File;
        size_t Line = 0;
        size_t Column = 0;
    };
    std::vector<DeferredTempUniqueFieldEscape> deferredTempUniqueFieldEscapes_;

    bool RecordDeferredTempUniqueFieldEscape(const llvm::Value* value, const std::string& destDesc,
            const std::string& file, size_t line, size_t column);

    void RecordDeferredTempUniqueFieldSinkEscape(const llvm::Value* value,
            const std::string& functionName, const std::string& paramName);

    // The ONE formatter for the laundered-escape wording, so the eager site and the deferred
    // resolve cannot drift apart.
    static std::string DescribeLaunderedTempUniqueFieldEscape(const std::string& calleeName,
            const std::string& access, const std::string& destDesc);

    static std::string DescribeTempUniqueFieldSinkEscape(const std::string& functionName,
            const std::string& paramName);

    void ResolveDeferredTempUniqueFieldEscapes();

    // Every hop of a candidate launder really does hand its parameter back.
    bool LaunderCondsAllProve(const std::vector<std::pair<const llvm::Function*, unsigned>>& conds);

    /*
     * The MIRROR ledger of codeValues_: values PROVEN to be DATA - a pointer whose declared type
     * is not code - recorded at the same read where the facts are in hand. The closure-widen gate
     * asks the OPPOSITE question of the code-value gates, so it needs the opposite evidence: a
     * join of two data pointers must not widen into a fat closure's CODE slot. Recording cannot
     * reject, so a read this misses degrades to the pre-existing accept. Same lifetime and
     * park/clear points as codeValues_; deliberately NOT statement-scoped.
     */
    std::vector<llvm::Value*> dataValues_;

    void RegisterDataValue(llvm::Value* value);

    bool IsLedgeredDataValue(const llvm::Value* value) const;

    void RegisterCodeValue(llvm::Value* value);

    void RegisterCodeValueDataCast(llvm::Value* value);

    bool IsLedgeredCodeValue(const llvm::Value* value) const;

    // occurrence: the caller's CurrentCastOccurrence() (a synchronous check, e.g.
    // RejectCodeValueTernaryStringArm) or a deferred caller's own NamedVariable::CastOccurrenceId
    // (ArgumentIsCodeValue, run after every sibling argument has already been evaluated). See
    // codeValueDataCasts_'s comment for why value identity alone is unsound here.
    bool IsCodeValueDataCast(const llvm::Value* value, size_t occurrence) const;

    /*
     * The MIRROR launder: values an EXPLICIT cast to a CODE type produced. `(function<int(int)>)x`
     * is the escape hatch this gate's own message advises, and a ptr->ptr cast is a no-op under
     * opaque pointers, so the cast result IS the ledgered data value - without this, casting a
     * whole JOIN would be refused while casting each ARM (which carries the declared flag) is
     * accepted, an asymmetry of exactly the kind this fix removes. STATEMENT-SCOPED like
     * codeValueDataCasts_, retired by FlushOwnedTemps, so one cast cannot launder a later gate.
     *
     * OCCURRENCE-KEYED for the same reason as codeValueDataCasts_ (see its comment): a data pointer
     * mentioned bare twice in one statement, once inside a call argument that also casts a DIFFERENT
     * data value to code, must not have the bare mention misread as "cast to code" just because it
     * shares a statement with an unrelated cast. Ordinary (non-Function, non-null) pointer values are
     * usually distinct SSA values per read, but a raw pointer PARAMETER used bare twice without an
     * intervening load is not - same collision shape as a named function, just rarer to trigger.
     */
    std::vector<std::pair<llvm::Value*, size_t>> dataValueCodeCasts_;

    /*
     * A SHARED CONSTANT must never enter this ledger. `(function<>)nullptr` casts the one shared
     * ConstantPointerNull, so registering it let an unrelated join's null arm read as
     * user-asserted code and re-opened the widen - measured, memory-unsafe. Null needs no launder
     * anyway: the arm walk already treats null as neutral. llvm::Function is skipped for the same
     * reason, making an invariant explicit that today only holds by short-circuit order.
     */
    void RegisterDataValueCodeCast(llvm::Value* value);

    bool IsDataValueCodeCast(const llvm::Value* value, size_t occurrence) const;

    static constexpr int kMaxJoinArmDepth = 8;

    // One ARM of a join: code when it is a function symbol, a ledgered read, or itself a join
    // carrying one. An explicitly data-cast arm is laundered and stops the walk. occurrence: see
    // IsCodeValueDataCast - held fixed across the whole recursive arm walk of ONE gated expression.
    bool JoinArmCarriesCodeValue(const llvm::Value* value, size_t occurrence, int depth = 0) const;

    /*
     * Does a '?:' / '??' JOIN deliver a code value down at least one arm? The '?:' arms are the
     * PHI's incoming values; the '??' arms are unrecoverable from the IR and come from
     * nullCoalesceJoins_. ANY code arm answers yes - one arm is enough to write a code address
     * into the destination - and a join of two data pointers answers no.
     */
    bool JoinCarriesCodeValue(const llvm::Value* value, size_t occurrence, int depth = 0) const;

    // One ARM of a join, for the DATA question: 1 = proven data, 0 = neutral (a null constant can
    // never be code; an arm the user explicitly cast TO a code type is their own assertion about
    // that arm alone), -1 = unproven, which alone makes the whole join unproven.
    int JoinArmDataKind(const llvm::Value* value, size_t occurrence, int depth) const;

    /*
     * Does a '?:' / '??' JOIN deliver a value proven to be DATA? The mirror of
     * JoinCarriesCodeValue, over dataValues_ and with the OPPOSITE quantifier: no arm may be
     * unproven and at least one must be proven, because a single unproven arm can still be code at
     * runtime. ANY-arm here would false-reject every mixed code/data join - shapes master compiles
     * and runs correctly. A NEUTRAL arm (null, or one the user cast to a code type) neither proves
     * nor blocks: the cast excuses the arm it is written on, never its sibling, which is still
     * bound into the code slot on its own branch.
     */
    bool JoinDeliversDataValue(const llvm::Value* value, size_t occurrence, int depth = 0) const;

    /*
     * Deferred interface-return-dangle check (the "existential" attempt - see
     * interface-return-dangle-defeated-by-intermediate-local.md). The emission-time value
     * walk (FrameLocalDataOfFatValue in MainListener.h) deliberately stops at a load, so
     * `IShape r = loc as IShape; return r;` reaches the return as a plain load with nothing
     * to reject. Rather than ask "which store reaches this return" (unanswerable soundly at
     * emission time - see the issue's abandoned attempts), this asks an EXISTENTIAL question
     * once the function's CFG is complete: does the returned slot have any writer that is a
     * frame box, AND no writer/user that proves the slot is not exclusively frame-boxed.
     * Recorded per llvm::Function* (mirrors nullEventLog_), resolved at the same end-of-body
     * hook as RunNullDerefDataflow, and dropped (not analyzed) on abort or leftover sweep.
     */
    struct PendingReturnDangleCheck
    {
        llvm::Function* Fn = nullptr;
        llvm::AllocaInst* Slot = nullptr;
        int Line = 0;
        int Col = 0;
        std::string InterfaceName;
    };
    std::unordered_map<llvm::Function*, std::vector<PendingReturnDangleCheck>> pendingReturnDangleChecks_;

    void RecordPendingReturnDangleCheck(llvm::AllocaInst* slot, int line, int col,
                                        const std::string& ifaceName);

    // Drop a function's pending return-dangle checks without analyzing them (an aborted body
    // has a partial CFG - same rationale as DiscardNullDerefEvents).
    void DiscardPendingReturnDangleChecks(llvm::Function* F);

    /*
     * Deferred definitely-null interface dispatch check (see
     * interface-method-call-on-null-value-segfaults.md). `IFace lv = default;` zero-fills the
     * {vtable,data} fat pointer, and a plain `.` call then loads the method slot off a null
     * vtable and segfaults with no diagnostic. `?.` is the language's answer whenever the
     * compiler cannot prove liveness, so this rejects ONLY the straight-line case it can prove:
     * the last write to the slot BEFORE the dispatch, in the dispatch's OWN basic block, is a
     * null constant, and the slot's address never leaves the frame. Recorded at the dispatch
     * (which cannot reject), resolved at the same end-of-body hook as the return-dangle check,
     * where the block is complete. Anything unrecognized - a branch or loop between, an escaped
     * address, a global receiver, a `?.` spelling - is simply not proven and compiles.
     *
     * The receiver's storage is keyed as a frame-local alloca plus a constant index path, so a
     * struct FIELD (`h.c`) and an array ELEMENT (`a[0]`) of a frame-local are proven the same
     * way as a whole local (empty path). A `this`, heap, through-pointer or variable-index base
     * resolves to no alloca and is never proven - that is what keeps `this->c.Get()` compiling.
     *
     * Interface FIELD access (`lv.tag`, and the `lv.tag = ...` write form, which share one
     * lvalue) has the identical hazard and uses the identical proof: the field address is
     * data + vtable[slot], so a null vtable faults on the offset load itself.
     */
    struct NullIfaceDispatchSite
    {
        std::string VarName;
        // The receiver exactly as written in source ("h.c", "a[0]"). Only consulted for a
        // sub-object receiver, where VarName names the CONTAINER and would be factually false.
        std::string ReceiverText;
        std::string MemberName;
        bool IsField = false;
        int Line = 0;
        int Col = 0;
    };

    struct PendingNullIfaceDispatch
    {
        llvm::AllocaInst* Base = nullptr;      // frame-local alloca the receiver lives in
        llvm::SmallVector<uint64_t, 4> Path;   // constant GEP indices from Base to the fat slot
        llvm::Instruction* Anchor = nullptr;   // the access's own load off the slot
        std::string VarName;
        std::string MemberName;
        std::string IfaceName;
        bool IsField = false;
        int Line = 0;
        int Col = 0;
    };
    std::unordered_map<llvm::Function*, std::vector<PendingNullIfaceDispatch>> pendingNullIfaceDispatch_;

    /*
     * The same access with a module-level GLOBAL receiver. A global's null-ness is its
     * INITIALIZER, not a store - `PLive g = default;` emits no instruction anywhere - and the
     * "never assigned" fact is whole-module, so this cannot be answered at end-of-body. Resolved
     * in RunNullIfaceGlobalCheck at module end. Value handles because a body erased before then
     * (a temp global-init function) takes its instructions with it.
     */
    struct PendingNullIfaceGlobalAccess
    {
        llvm::WeakVH Global;   // the GlobalVariable the receiver lives in (whole global, or its base)
        llvm::WeakVH Anchor;   // the access's own load off the global
        std::string VarName;
        std::string MemberName;
        std::string IfaceName;
        bool IsField = false;
        int Line = 0;
        int Col = 0;
        // Constant GEP indices from Global to the fat slot; empty for a whole-global receiver.
        llvm::SmallVector<uint64_t, 4> Path;
    };
    std::vector<PendingNullIfaceGlobalAccess> pendingNullIfaceGlobal_;

    // WeakVH nulls itself when its value is deleted, so a null answer here means "gone".
    static llvm::Value* NullIfaceHandleValue(const llvm::WeakVH& h);

    /*
     * Resolve a storage address to (frame-local alloca, constant index path). An LLVM GEP's FIRST
     * index steps over whole pointee objects, so it must be zero here (anything else addresses a
     * different object) and is dropped; the remaining indices are the path. Each link's source
     * element type must match the type reached so far, which is what makes the path meaningful
     * against a constant stored at the base. Returns nullptr - i.e. "not proven, accept" - on a
     * non-constant index, a non-GEP link, or a base that is not an alloca.
     */
    static llvm::AllocaInst* ResolveIfaceStorageLoc(llvm::Value* slot,
                                                    llvm::SmallVectorImpl<uint64_t>& path);

    /*
     * Same resolution as ResolveIfaceStorageLoc, but for a sub-object of a GLOBAL rather than a
     * frame-local alloca. A field/element of a global is addressed through a GEP whose base and
     * indices are all constants, so IRBuilder folds it to a CONSTANT-EXPRESSION GEP
     * (`llvm::GEPOperator` covers both that form and the ordinary instruction form) instead of a
     * `GetElementPtrInst` - which is why the alloca resolver above never sees it. Same discipline:
     * bounded chain length, all-constant indices, a leading zero pointer-step index (dropped),
     * each link's source type must match the type reached so far, bounded path length. Returns
     * nullptr - i.e. "not proven, accept" - on anything that does not fit.
     */
    static llvm::GlobalVariable* ResolveIfaceStorageGlobal(llvm::Value* slot,
                                                            llvm::SmallVectorImpl<uint64_t>& path);

    void RecordPendingNullIfaceDispatch(const NullIfaceDispatchSite& site, llvm::Value* slot,
                                        llvm::Value* anchor, const std::string& ifaceName);

    // Same rationale as DiscardPendingReturnDangleChecks: an aborted body's blocks are partial.
    void DiscardPendingNullIfaceDispatch(llvm::Function* F);

    // Pointer SSA values detached by a `move` expression, keyed by value identity. A move nulls its
    // source, so nobody owns the value until a receiver adopts it - but it is not a `new` result and
    // carries no free-site type, so it belongs in neither ledger above. Lets a '?:' join ask "is
    // this arm owning" of the VALUE, so parens/casts around the move cannot change the answer.
    // Membership carries PROVENANCE: a move of a BORROWED source transfers nothing (the real owner
    // still frees the pointee), so ParseMoveExpression keeps it OUT and the join scores it mixed.
    // Retired with ownedNewTemps_.
    std::vector<llvm::Value*> movedOutPtrValues_;

    // The negative counterpart of movedOutPtrValues_: pointer SSA values a `move` of a BORROWED
    // source produced, with the borrow's origin name for the diagnostic. Such a move nulls only the
    // borrower's copy, so the value provably owns NOTHING and every owning destination rejects it -
    // a `unique` LOCAL (reassignment) and a `unique` FIELD (both the `=` and the brace-init store
    // paths). Carried through a '?:' join whose arms are all borrowed moves or null, so the
    // laundered spelling is rejected exactly like the direct one. Retired with ownedNewTemps_.
    std::vector<std::pair<llvm::Value*, std::string>> movedBorrowedPtrValues_;
    std::vector<llvm::Value*> movedBorrowedThroughFieldValues_;

    // '?:' joins of an OWNING-VALUE STRUCT whose arms did not all provably own, keyed by value
    // identity. Such a phi may carry a live borrow's bits, so every receiver that would otherwise
    // adopt it (a fresh local, an existing owning local, a struct field) must BORROW instead: the
    // untaken owning arm leaks, which beats destroying a pointee its real owner destroys again.
    // A struct value has no runtime owned bit, so this ledger is the only carrier. Retired with
    // ownedNewTemps_.
    std::vector<llvm::Value*> nonOwningStructJoins_;

    // Lambda literals with unclaimed heap envs (no named owner). Closure analog of
    // pendingOwnedStringTemps; freed at end-of-full-expression by FlushOwnedClosureTemps.
    std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> pendingOwnedClosureTemps;

    // Spilled allocas of unbound owning-struct `move`-return temps (`makeToken().text`), freed by
    // FlushOwnedStructTemps. Struct analog of the string/closure temp lists. Block = dominance guard.
    // LiveFlag (hoisted join-arm temps only): an i1 slot cleared at the branch and set in the arm,
    // so the later destructor runs ONLY when the arm ran - a user dtor body is not null-safe.
    struct PendingOwnedStructTemp { llvm::Value* Alloca; std::string TypeName; llvm::BasicBlock* Block; llvm::Value* LiveFlag = nullptr; };
    std::vector<PendingOwnedStructTemp> pendingOwnedStructTemps;

    // Owning-POINTER call results (`move R*`) consumed as a SUBEXPRESSION operand, so no named
    // local ever adopts them (`if (makePtr() != nullptr)`). Pointer analog of the string/closure/
    // struct temp lists; freed at end-of-full-expression by FlushOwnedPtrTemps. Registered only at
    // sites that provably consume-and-discard the pointer (see RegisterOwnedPtrTemp).
    struct PendingOwnedPtrTemp
    {
        llvm::Value* Value;
        std::string TypeName;
        uint64_t AllocAlign = 0;
        llvm::BasicBlock* Block = nullptr;
    };
    std::vector<PendingOwnedPtrTemp> pendingOwnedPtrTemps;

    // Per-function noalias metadata for T[] views. Proves pairwise disjointness for span<T> fields
    // where the noalias parameter attribute cannot reach. Reset in createFunctionBlock.
    llvm::MDNode* aliasDomain_ = nullptr;
    std::vector<llvm::MDNode*> aliasScopes_;
    std::map<std::string, int> viewScopeByOrigin_; // origin name -> index into aliasScopes_

    // Functions whose live `if const` branch hit compile_error("msg") during eager instantiation.
    // The error fires later only if the function is actually called (CheckPoisonedFunctionCalls).
    std::unordered_map<std::string, std::string> poisonedFunctions;

    // Source location of the FIRST emitted call to each function, by mangled name. Lets an
    // end-of-module diagnostic (CheckPoisonedFunctionCalls) point at the real call site.
    std::unordered_map<std::string, std::pair<size_t, size_t>> firstCallLocation_;

    /*
     * A call that handed a temp's `unique` field to a PLAIN `T*` parameter, recorded where the
     * ledger fact is still live and resolved at end of module, where every callee body is
     * complete. A callee defined BELOW its call site is still a declaration during the walk, so
     * an immediate-only check would silently accept it - the record-then-resolve shape
     * (codeValues_, owningTempUniqueFields_) is what makes the answer order-independent.
     */
    struct TempUniqueFieldArg
    {
        const llvm::Function* Callee = nullptr;
        unsigned ArgIndex = 0;
        std::string CalleeName;
        std::string Access;
        // The FILE the call was written in. By end of module sourceFileName is the main file
        // again, so a call in an imported module would otherwise be reported against the wrong
        // file at a line number that belongs to something else entirely.
        std::string File;
        size_t Line = 0;
        size_t Column = 0;
        // The value was not ledgered directly, so it arrived through a '?:' / '??' join. Only
        // then may the diagnostic say so - a plain read and a cast are both ledgered directly.
        bool ThroughJoin = false;
        // INTERFACE DISPATCH: there is no single Callee to ask, so the entry names the vtable
        // SLOT and is judged against EVERY implementor. Empty IfaceName = ordinary direct call.
        std::string IfaceName;
        std::string MethodName;
        std::string ParamName;
        size_t Arity = 0;
        // The argument was itself a CANDIDATE launder (`keep(g(makeBox().t))` with `g` below), so
        // this entry only escapes when every hop of that chain also proves.
        std::vector<std::pair<const llvm::Function*, unsigned>> LaunderConds;
    };
    std::vector<TempUniqueFieldArg> tempUniqueFieldArgs_;

    // Depth counter, not a bool: '?:' arms can nest (a ? (b ? c : d) : e), so a plain flag would
    // clear early on the inner ternary's exit. Non-zero while lowering either arm of a '?:' in the
    // EAGER constant-context form, where both arms execute unconditionally (CreateSelect, no
    // branch), so a deref inside either one that would be sound under the OTHER arm's condition
    // must not be guarded (see IsExplicitlyMovedNullHere). The normal in-function lowering
    // branches and does not raise this. Not part of the --init cache round-trip (transient state).
    int suppressExplicitNullDerefGuard_ = 0;
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

    void SetSourceLocation(size_t line, size_t column);

    void LogError(std::string message) const;

    // In batch/--check mode throws so the loop continues; otherwise exits with code 1.
    [[noreturn]] void FailCompilation(const std::string& message) const;

    void LogWarning(std::string message) const;

    friend class MainListener;
    friend class ForwardRefScanner;
    friend class CrossThreadEscapeScanner;

public:
    bool FieldSatisfiesThreadDiscipline(const TypeAndValue& field) const;

private:
    std::unique_ptr<llvm::IRBuilder<>> builder;
    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::LLVMContext> context;

    std::vector<StackState> stackNamedVariable;
    // Per-llvm::Function move-event log (move-dataflow). Appended in emission
    // order; consumed and cleared by RunMoveDataflow. Cleared by ResetForReanalysis.
    std::unordered_map<llvm::Function*, std::vector<movedf::Event>> moveEventLog_;
    // Per-llvm::Function explicit-move null-state event log. Separate stream from moveEventLog_
    // on purpose (see the nulldf namespace comment); consumed per function by
    // RunNullDerefDataflow and swept by RunMoveDataflow. Cleared by ResetForReanalysis.
    std::unordered_map<llvm::Function*, std::vector<nulldf::Event>> nullEventLog_;
    // Functions proven to never return, so a `if (cond) { die(); }` guard suppresses like an
    // inline exit() would. Populated as each body completes; cleared by ResetForReanalysis.
    nulldf::NoReturnSet provenNoReturn_;
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
    // Pure-rename `using` aliases (`using MyInt = int;`) - alias -> bare target name. Written ONLY
    // by ForwardRefScanner::PreRegisterRenameAliases, before either pass walks the file, and read
    // only by MangleTypeArg. Kept out of typeAliases because that map fills in progressively as
    // each pass reaches the `using`, which happens at different points in the two passes.
    std::unordered_map<std::string, std::string> manglingAliases_;
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
    // Interface name -> "file(line,col)" of the definition that registered it. The forward scan
    // and the codegen walk both register the SAME definition, so the guard compares sites rather
    // than mere presence; a second definition at a different site is a redefinition.
    std::unordered_map<std::string, std::string> interfaceDefSites;
    // A duplicate definition is visited by both passes. Remember rejected definition sites so
    // the same source definition emits its diagnostic only once.
    std::unordered_set<std::string> rejectedInterfaceDefSites_;
    std::unordered_map<std::string, std::string> rejectedInterfaceDefMessages_;
    // Subset of interfaceDefSites keys whose recorded definition came from a core library file
    // (currentSourceIsCore_ was true when the site was recorded). Lets the redefinition guard in
    // CreateInterfaceDefinition tell a core/user name collision apart from a user/user one.
    std::unordered_set<std::string> coreInterfaceDefs_;
    // Class name -> base-clause interface names, scanned before codegen so a conversion site sees
    // classes declared LATER. STATIC-CHECK ONLY: driving vtable emission off it would cache a
    // vtable built against a still-opaque StructType (null field offsets, empty destructor).
    std::unordered_map<std::string, std::vector<std::string>> scannedInterfaceImpls;
    // Interfaces whose real implementor set is a main-pass fact: named in a generic class's base
    // clause, or spelled generically. Never prove a conversion impossible against one of these.
    std::unordered_set<std::string> uncertainInterfaceImpls;
    // A class declared inside an `if const` arm that MainListener did NOT take, with a phrase
    // describing that arm. The scanner records every arm; the entries for the arm MainListener
    // takes are retracted as it takes them, so only genuinely absent classes are left. DIAGNOSTIC
    // ONLY: it never decides whether a program is accepted, it only explains a rejection.
    struct IfConstGuardedImpl
    {
        std::string ClassName;      // qualified: namespace and enclosing class included
        // Every `if const` arm or chained else path the class sits under, OUTERMOST first. A level
        // is peeled as it is taken, so GuardChain[0] is always one nobody was shown to take.
        std::vector<std::string> GuardChain;
        const void* Node = nullptr; // ClassDefinitionContext, the retraction/peel key
        // One CANDIDATE LIST per base-clause entry, in the resolver's own priority order. The
        // scanner cannot resolve a spelling yet, so the pick is deferred to the blame site.
        std::vector<std::vector<std::string>> InterfaceCandidates;
    };
    std::vector<IfConstGuardedImpl> ifConstGuardedImpls_;
    // Depth of nested CompileImportedFile scan/walk. Above zero the files that follow this import
    // have not been scanned yet, so the implementor registry is provably incomplete.
    int importCompileDepth_ = 0;
    // Type-level annotations keyed by type/interface name ([winrt] on a class, [uuid("...")] on an
    // interface, ...). Field annotations are separate (StructData.StructFields[].Annotations).
    std::unordered_map<std::string, std::vector<AnnotationValue>> typeAnnotations_;

    // Set (or, when empty, clear) the annotations recorded for a type/interface. Empty erases so a
    // re-analysis where a type loses its annotations does not keep the stale entry.
    void SetTypeAnnotations(const std::string& name, std::vector<AnnotationValue> anns);
    // Find a single named annotation on a type/interface, or nullptr if absent.
    const AnnotationValue* FindTypeAnnotation(const std::string& name, const std::string& annName) const;
    // The raw argument of a named type/interface annotation (e.g. the GUID of [uuid]); empty if absent.
    std::string GetTypeAnnotationArg(const std::string& name, const std::string& annName) const;

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
    // Canonical path of the root file handed to Compile()/Analyze() - under the LSP that is the
    // temp copy, which DefinitionSitePath() maps back to the real document.
    std::string analyzedRootPath_;

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

    // --sanitize=ownership (M1): debug-only runtime instrumentation. Off by default; when off,
    // codegen is byte-for-byte identical. ownOriginSlots_ maps an owning-pointer local's storage
    // alloca to a hidden i64 "move origin" slot (0 = live, ((i64)line<<32)|col = moved-at-site).
    // Module-bound values - cleared by ResetForReanalysis.
    bool sanitizeOwnership_ = false;
    std::unordered_map<llvm::Value*, llvm::Value*> ownOriginSlots_;

    struct UniqueFieldBorrowReturn
    {
        bool Failed = false;            // sticky: some return did NOT prove the field read
        bool SawProof = false;          // at least one return did prove it
        std::string FieldOwner;         // "Struct.field" - the owner named in a diagnostic
    };
    // The unique-field borrow provenance (see RecordUniqueFieldBorrowReturn). Both are keyed by
    // module-bound pointers - a CallInst is one value per CALL SITE, never a shared constant, so
    // value identity is safe here. Cleared by ResetForReanalysis with the module.
    std::unordered_map<const llvm::Function*, UniqueFieldBorrowReturn> uniqueFieldBorrowReturns_;
    std::unordered_map<const llvm::Value*, UniqueFieldBorrowReturn> uniqueFieldBorrowResults_;

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
    // A positional `.c` input, noted at arg-parse time because clang is only invoked for it
    // AFTER the module-end analyses run. Read by RunNullIfaceGlobalCheck.
    bool positionalCSource_ = false;
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
    // While set, LogError throws SpeculativeEvalAbort instead of emitting/exiting - used by
    // speculative compile-time probing (owning-sink if-const evaluation) to bail cleanly.
    bool suppressErrors_ = false;
    size_t expectedErrorScopeDepth = SIZE_MAX;  // SIZE_MAX = scoped block form (checked manually after block); else stackNamedVariable depth for bare-semicolon form
    // Armed before ProcessImports so import-time errors can match. An unmatched expectation
    // at end of compilation is a did-not-occur failure.
    std::string fileScopeExpectedError_;

    /*
     * A nested (scoped or statement-level) expect_error OVERWRITES the armed file-scope bare
     * expectation while it runs, so clearing it on completion would drop the file-scope one for
     * the rest of the compile. Re-arm instead. This is what lets one file combine the two forms,
     * which a MODULE-END diagnostic needs: only the bare file-scope form is still armed then.
     * Empty fileScopeExpectedError_ makes this exactly the old clear().
     */
    void RestoreFileScopeExpectedError();

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
    // One conversion site that needed a rebox thunk. Finalization has no parse context and no
    // meaningful scanner state of its own, so everything the zero-implementor diagnostic needs
    // is captured HERE, at the site, while it is still true.
    struct InterfaceReboxSite
    {
        std::string File;
        size_t Line = 0;
        size_t Column = 0;
        bool InImport = false;    // site is inside a file being imported (importCompileDepth_ > 0)
        bool Uncertain = false;   // destination's implementor set was unprovable at the site
    };

    // One deferred interface -> interface rebox thunk per destination interface (see
    // GetOrCreateInterfaceReboxThunk). The typedesc if-chain inside it is emitted at
    // finalization, when every implementor is registered. Every site that shares the thunk is
    // recorded, not just the first: the diagnostic must report the user's line rather than a
    // library's, and must stay silent if ANY sharer could not be reasoned about.
    struct DeferredInterfaceRebox
    {
        std::string DstInterface;
        llvm::Function* Thunk = nullptr;
        std::vector<InterfaceReboxSite> Sites;
    };
    std::vector<DeferredInterfaceRebox> deferredIfaceRebox_;
    std::unordered_map<std::string, size_t> deferredIfaceReboxIndex_;
    // Set once EmitDeferredInterfaceReboxBodies has run. A thunk handed out after that would
    // reach the verifier bodyless (internal linkage), so it is emitted on the spot instead.
    bool deferredIfaceReboxDrained_ = false;
    std::unordered_map<std::string, llvm::Function*> memberwiseCopyCache_;
    std::function<void(const std::string&, size_t, size_t, const std::string&, int)> diagnosticSink_;
    std::function<void(int, int, int, int, const std::string&)> hintRegionSink_;
    LspSymbolIndex* symbolSink_ = nullptr;

    llvm::Function* currentFunction;
    std::string sourceFileName;
    std::string currentSourceFilePath_;
    // True while the file being scanned/walked lives under runtimeDir/core (set by CompileImportedFile).
    bool currentSourceIsCore_ = false;
    // Where a function BODY was first attached, keyed on the MANGLED name: source file leaf and
    // line. Read only to tell a genuine redefinition apart from the same definition arriving twice.
    // Per-compile; cleared by ResetForReanalysis with the module it describes.
    std::unordered_map<std::string, std::pair<std::string, size_t>> functionBodyOrigin_;
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

    llvm::DIFile* GetDIFileForCurrentSource();


private:
    llvm::Function* createFunctionProto(const std::string& name, llvm::FunctionType* returnType);

    void SetVariableRefCountStorage(const std::string& varName, llvm::Value* refStorage);

    // Set a named local's ownership flag (e.g. a `unique` interface local adopting a new owned
    // value on reassignment, so its scope-exit teardown frees the current pointee).
    void SetVariableOwning(const std::string& varName, bool value);
    void SetVariableRawNewArray(const std::string& varName, bool value,
                                llvm::Value* rawArrayLength = nullptr);

    // True when the named variable currently owns its value (freed on scope exit). Used to decide
    // whether consuming it (into a move interface param) must disown the source.
    bool IsVariableOwning(const std::string& name) const;

    // True when `name` resolves to a plain by-value owning-value parameter that only BORROWS the
    // caller's value (see NamedVariable::IsBorrowedOwningValue). Used to reject laundering it into
    // a consuming param or moving it out.
    bool IsVariableBorrowedOwningValue(const std::string& name) const;

    struct VarStorageRef { llvm::Value* Storage = nullptr; llvm::Type* BaseType = nullptr; };
    VarStorageRef FindVariableStorage(const std::string& name) const;

    // Return the live NamedVariable a name binds to (namedVariable then functionArgument, inner
    // scope first), or nullptr. The `drop` statement needs the same object scope-exit cleanup
    // reads so a value is released identically both ways.
    NamedVariable* FindLiveNamedVariable(const std::string& name);

    bool IsVariableOwningString(const std::string& name) const;

    bool IsVariableBorrowingOwnedString(const std::string& name) const;

    void SetVariableBorrowsOwnedString(const std::string& name, bool value);

    // Refresh the container-owned-element borrow taint after a reassignment: `g = new B()`
    // over `g = l.get(0)` clears it so a later `delete g` is allowed (see BorrowsOwnedElement).
    void SetVariableBorrowsOwnedElement(const std::string& name, bool value,
        const std::string& container, bool externallyOwned = false);

    void EmitConditionalOwningPtrCleanup(const NamedVariable& namedVar, llvm::Value* refCount);

    void EmitOwningPtrCleanup(const NamedVariable& namedVar);

    // True when this variable owns a heap-boxed interface value freed at scope exit: a `unique`
    // interface local (IsOwning set from its `new` / move source) or a `unique` interface param.
    // IsUnique is load-bearing: the generic `list<T>::add(move T value)` is ALWAYS a move param,
    // but for a borrow `list<IShape>` (T not unique) `value` does not own and must NOT be freed -
    // only a genuinely `unique` interface value does. Its Storage is a fat-ptr {i8*,i8*} slot, so
    // EmitOwningPtrCleanup (single pointer) must not run on it - EmitOwningInterfaceCleanup handles
    // the fat-ptr teardown instead.
    // IsUniqueTypeArg is checked alongside IsUnique: generic substitution records a `unique` type
    // ARGUMENT there and never sets IsUnique, so `move V value` with V = `unique IFace` would
    // otherwise read as a borrow and leak (the `unique C*` spelling is owning unconditionally).
    bool IsOwningInterfaceValue(const NamedVariable& namedVar) const;

    // Free a `unique`/`move` interface VALUE (local or param): load the fat value from its slot
    // and route through DeleteInterfaceValue (vtable dtor slot + operator delete), nulling the
    // slot's data field so a prior explicit delete (which also nulls it) makes this a no-op. The
    // interface name is the value's TypeName.
    void EmitOwningInterfaceCleanup(const NamedVariable& namedVar);

    // True when a fixed-array local (`unique T*[N]` / `unique IFace[N]`) owns every element and
    // must be torn down slot by slot. A directly-declared unique array FIELD is rejected outright;
    // a LOCAL is legal, so its N elements need the per-element release the scalar path gives one.
    bool IsOwningUniqueArray(const NamedVariable& namedVar) const;

    // Release each element of an owning fixed-array local by reusing the scalar cleanup emitters
    // over a per-element GEP. Unrolled: N is a compile-time constant and stays small in practice.
    void EmitOwningUniqueArrayCleanup(const NamedVariable& namedVar);

    void RegisterOwnedStringTemp(llvm::Value* value);

    // True when `value` is a registered, still-unclaimed owned string temp (a produced value with
    // no named owner) rather than a local/field read whose own owner frees it.
    bool IsPendingOwnedStringTemp(llvm::Value* value) const;

    void UnregisterOwnedStringTemp(llvm::Value* value);

    // Record an owning-RETURN call result for the no-discard check (see ownedReturnTemps_).
    // `retType` also carries the pointee type/alignment the owned-pointer temp cleanup needs.
    void RegisterOwnedReturnTemp(llvm::Value* value, const std::string& fnName,
                                 const TypeAndValue& retType);

    // Propagate an existing ledger entry onto a derived value (e.g. a '?:' select result).
    // Detection and release entries propagate independently.
    void PropagateOwnedReturnTemp(llvm::Value* from, llvm::Value* to);

    /*
     * Ledger lookup for every OWNERSHIP question: the entry if `value` is a still-unconsumed
     * owning return, else nullptr. A CallerReleaseSuppressed entry reads as ABSENT here - it
     * survives only to keep the no-discard diagnostic firing, and treating its presence as
     * ownership is what re-admits a freed borrow (a mixed '?:' join). Flag-blind reads are
     * opt-in through FindOwnedReturnEntryForDiagnostic, so forgetting the qualifier now costs
     * a leak instead of a double free.
     */
    const OwnedReturnReleaseTemp* FindOwnedReturnEntry(llvm::Value* value) const;

    // Raw lookup, suppression included. ONLY the no-discard diagnostic and suppression-preserving
    // propagation may use this - a suppressed entry must never answer an ownership question.
    const OwnedReturnTemp* FindOwnedReturnEntryForDiagnostic(llvm::Value* value) const;

    // Returns the callee name if `value` is a still-unconsumed owning return, else nullptr.
    // Diagnostic-only, so deliberately flag-blind: a suppressed join is still a discard.
    const std::string* FindOwnedReturnTemp(llvm::Value* value) const;

    /*
     * Register an owning-POINTER call result that an enclosing expression consumes without
     * adopting it, and that the pointer provably cannot ESCAPE from: a comparison operand (the
     * result is a bool) or a scalar-field deref base (the read copies a self-contained value).
     * A CALL ARGUMENT is such a site only when the callee's matching parameter provably does not
     * RETAIN it (see ParameterRetainsArgument) - the call site checks that before calling here.
     * Gated on the owning-return / owning-`new` ledgers, so a BORROW-returning call (also a
     * CallInst) is never registered and cannot be double-freed. UnregisterOwnedPtrTemp guards the
     * sink invariant (no caller-side free may remain registered for an argument a callee took
     * ownership of).
     */
    void RegisterOwnedPtrTemp(llvm::Value* value);

    // True when `value` is a still-unadopted owning-POINTER temp: an owning-RETURN call result or
    // a raw `new` result. Exactly the set RegisterOwnedPtrTemp accepts.
    bool IsOwningPtrTempValue(llvm::Value* value) const;

    /*
     * Register every owning-pointer ARGUMENT of the just-emitted call whose matching parameter
     * provably does not RETAIN it, so the caller frees it at end-of-full-expression instead of
     * leaking it. Everything not proven safe is left alone (a bounded leak beats a use-after-free).
     */
    void RegisterNonEscapingOwningPtrArgs(llvm::Value* callResult);

    // Drop every escape-analysis answer recorded for `fn`. Must run before the function is
    // erased: the memo is keyed on the raw pointer, which LLVM may reuse for a new function.
    void ForgetFunctionEscapeMemo(const llvm::Function* fn);

    // Drop every escape-analysis answer: the whole module (and every Function* in it) is going away.
    void DropModuleEscapeMemo();

    // Keep `value` visible to the no-discard check but remove every release entry for it,
    // including one an earlier operand site already registered.
    void SuppressCallerRelease(llvm::Value* value);

    /*
     * True when a '?:' arm may join an OWNING result: a null constant (whose free is a no-op), a
     * value the caller could legally free, or a pointer a `move` detached from its source. The
     * free-eligibility core is deliberately THE SAME PREDICATE the release path uses
     * (IsOwningPtrTempValue is "exactly the set RegisterOwnedPtrTemp accepts"), so any condition
     * added to one is automatically honoured by the other; the other two legs own without being
     * freeable HERE (a null frees nothing, a moved-out value has no free-site type yet).
     * Every leg is answered by VALUE IDENTITY, never by the arm's syntax: parentheses or a cast
     * around a `move` must not change the answer, and a `new` sitting in ARGUMENT position inside
     * a borrow arm must not be mistaken for the arm owning its result. Everything else - a live
     * borrow, a suppressed join, an entry that is not a freeable owning pointer - fails here and
     * forces the join to be treated as mixed, i.e. suppressed and leaked rather than freed.
     */
    bool TernaryArmJoinsOwning(llvm::Value* arm);

    /*
     * True when `value` is a by-value struct of an OWNING-VALUE type (a `unique` field, directly or
     * transitively, so GetOrCreateFullDestructor synthesizes a field-deleting destructor). This is
     * the struct analog of IsInterfaceFatValue: such a join is adopted by its receiver (a fresh
     * local of that type destructs unconditionally at scope exit), so it needs the same strict
     * all-arms-owning join rule. `string` and the two fat-pointer shapes run their own release
     * paths and are excluded.
     */
    bool IsOwningValueStructValue(llvm::Value* value);

    // Ledger a '?:' join of an owning-value STRUCT whose arms did not all provably own: the joined
    // bits may be a live borrow, so no receiver may adopt (destruct) them. See ReceiverAdoptsJoin.
    void RegisterNonOwningStructJoin(llvm::Value* value);

    // True when `value` is a suppressed owning-value struct join (see RegisterNonOwningStructJoin).
    bool IsNonOwningStructJoin(llvm::Value* value) const;

    // True when `value` is an interface fat pointer ({vtable, data}), the STRUCT-typed shape an
    // interface-typed '?:' arm or join carries. The closure fat pointer is a different named type.
    bool IsInterfaceFatValue(const llvm::Value* value) const;

    // Drop every sticky per-expression "the result is owned" side-channel. These carry no value
    // identity, so a join that owns nothing must clear them or the receiver adopts anyway.
    void ClearOwnedResultChannels();

    /*
     * Carry "this value came from a `move` of a BORROW" out of a '?:' join, so a receiver that must
     * own the result rejects the laundered spelling exactly as it rejects the direct one. Only a
     * join whose arms are ALL borrowed moves or null qualifies: that join provably owns nothing on
     * every path. A MIXED join (one owning arm) keeps its existing borrow-and-suppress behaviour -
     * which arm runs is not knowable, so it is not provably non-owning and stays a plain borrow.
     */
    void PropagateMovedBorrowedPtrValue(llvm::Value* trueValue, llvm::Value* falseValue,
                                        llvm::Value* joined);

    /*
     * Ledger a '?:' phi/select result as owning, and report whether the join was MIXED. A ternary
     * is a transparent wrapper, so ownership rides out on the joined value. The two ledgers serve
     * different purposes and a MIXED pointer join (`cond ? makePtr() : borrowedPtr`) must split
     * them: DETECTION still applies, so a discarded mixed ternary is caught by the no-discard check
     * exactly as on master, while RELEASE is suppressed - a caller-side free would fire on whatever
     * the phi selected and destroy a pointee someone else still owns. The owning-`new` ledger drives
     * adoption and release only, so a mixed join does not enter it at all. The caller must also
     * ClearOwnedResultChannels on a mixed join: those side-channels have no value identity and
     * would otherwise let the receiver adopt anyway. An INTERFACE fat-pointer join runs the same
     * strict rule: it is a struct type, so it used to take the either-arm branch and let one
     * owning arm stamp a join whose other arm was a live borrow - the receiver then destroyed a
     * box its real owner destroyed again (use-after-free plus double free). A by-value OWNING
     * STRUCT join runs the same strict rule for the same reason, and additionally records the
     * suppression by value identity (see RegisterNonOwningStructJoin) because a struct carries no
     * runtime owned bit. `string` and the closure fat pointer keep the either-arm rule: their
     * runtime owned bit already makes a borrowed arm's release a no-op, and every string arm is
     * deep-copied into an independent buffer before the join (see AdoptTernaryStringArm).
     */
    bool PropagateTernaryOwnership(llvm::Value* trueValue, llvm::Value* falseValue, llvm::Value* joined);

    // Sentinel ledgered as a '?:' join's "interface" when its two arms disagree (e.g. a
    // derived-typed move joined with a parent-typed move): no single name is correct, so
    // ReboxInterfaceIfNeeded must rebox unconditionally instead of trusting either arm's name.
    static constexpr const char* kAmbiguousFatInterface = "<ambiguous>";

    // Symbol prefix of the deferred interface -> interface rebox thunks. The destination
    // interface name follows verbatim, so the pair round-trips through a bitcode module.
    static constexpr std::string_view kInterfaceReboxPrefix = "__iface_rebox.";

    // Stand-in file name for a rebox site recovered from the core bitcode cache, whose real
    // location died with the compile that built the cache.
    static constexpr std::string_view kCoreBitcodeCacheOrigin = "<core bitcode cache>";

    // Ledger a '?:' join's interface from whichever arm's own ledger entry is known - the joined
    // phi/select carries no NamedVariable of its own to read a TypeName from. When both arms are
    // known and DIFFER, ledger the ambiguous sentinel instead of picking one arbitrarily - the
    // unpicked arm's own interface would otherwise silently skip its rebox at the receiver.
    void PropagateFatInterfaceJoin(llvm::Value* trueValue, llvm::Value* falseValue, llvm::Value* joined);

    /*
     * Escape analysis: "can parameter `argIndex` of `fn` RETAIN its argument past the call?".
     * Conservative by construction - true (retains, caller must not free) for anything not proven
     * safe: an unseen body (extern / imported), an indirect or virtual-dispatch callee, a
     * store to a global or a field, a `return`, a ptrtoint, a hand-off into another retaining or
     * unanalyzable parameter slot, or a `delete` (which routes through the extern deallocator).
     * A variadic argument past the declared parameter list is a C boundary and answers false by
     * axiom; declared parameters of a variadic function are still walked normally.
     * A recursion cycle answers "retains", so mutual recursion terminates conservatively.
     * "Retain" also covers the pointee's CONTENTS: freeing the argument runs its destructor, so a
     * pointer the callee reads out of it (or parks into it) that outlives the call counts too.
     */
    bool ParameterRetainsArgument(const llvm::Function* fn, unsigned argIndex, int depth = 0);

    /*
     * The "does not retain PAST my call" half of the question above: same walk, but the callee's
     * OWN return is not an escape. Only ever asked about a NESTED callee, never about the
     * function being walked - a pointer handed back to my frame is still mine to judge, whereas
     * one handed back to MY caller has left. Used to keep following the call RESULT instead of
     * answering "retains", so a projection consumed inside the statement stops leaking.
     */
    bool ParameterRetainsArgumentPastCall(const llvm::Function* fn, unsigned argIndex, int depth);
    std::map<std::pair<const llvm::Function*, unsigned>, bool> paramRetainsPastCallMemo_;
    std::set<std::pair<const llvm::Function*, unsigned>> paramRetainsPastCallInProgress_;

    /*
     * The setTag discriminator: `tag = s.copy();` emits the FIELD's destructor through `this` and
     * then STORES the replacement into that same field. A callee that frees one field it goes on
     * to overwrite has not retained the object - unlike one that frees the object itself, which
     * keeps answering "retains" because the deallocation runs on the tracked pointer's own block.
     */
    bool CallIsOverwrittenFieldDestructor(const llvm::CallBase* call, const llvm::Value* tracked) const;

    /*
     * True only when `fn`'s body can no longer grow. cflat emits IR as it walks the tree, and it
     * emits functions NESTED (a lambda invoker, a generic instantiation or a global initializer
     * suspends its enclosing function through SaveBuilderState), so neither "has a body" nor
     * "is not currentFunction" proves completeness. Three tests, all required: not a declaration,
     * not the function being emitted, not an enclosing function whose emission is suspended, and
     * no block still open for appending (the belt-and-braces catch for any un-paired path).
     */
    bool FunctionBodyIsComplete(const llvm::Function* fn) const;

    // The permanent half of FunctionBodyIsComplete: a body that exists and has no block still
    // open for appending. Excludes the two TRANSIENT gates (being emitted / suspended), which an
    // end-of-module resolve must not consult - by then they say nothing about the body.
    bool FunctionBodyIsReadable(const llvm::Function* fn) const;

    /*
     * The OPPOSITE polarity of ParameterRetainsArgument, and a separate walk for that reason.
     * ParameterRetainsArgument answers "may this escape?" and reports TRUE for everything it
     * cannot model - a vararg callee, a declaration, recursion, a local struct field - which is
     * right for suppressing a free and wrong for raising a rejection. This one answers "is this
     * parameter PROVABLY stored into memory that outlives the call?" and reports FALSE for
     * everything unmodelled, so an unknown callee accepts. `destKind` receives a description of
     * the proof for the diagnostic; it is only written when the answer is true.
     */
    bool ParameterProvablyRetainsArgument(const llvm::Function* fn, unsigned argIndex,
                                          std::string& destKind, int depth = 0);

    // Worklist half of ParameterProvablyRetainsArgument. Deliberately NOT memoized: the query is
    // gated by the temp-unique-field ledger and so is rare, and a memo taken while a body was
    // still growing would be read back after it finished.
    bool OwningPtrProvablyEscapes(const llvm::Value* root, std::string& destKind, int depth);

    // True when `ptr` names memory that survives the call: a global, memory the CALLER supplied
    // through a parameter (`this` included), or memory reached by dereferencing either. A local
    // alloca, a fresh allocation and anything unrecognized answer false.
    bool MemoryOutlivesCall(const llvm::Value* ptr, std::string& destKind, int depth) const;

    // Does a load/store-only stack slot hold a pointer that outlives the call? This is the
    // parameter-prologue shape: `store ptr %this, ptr %this.addr` parks a caller pointer in a
    // slot every later dereference reads back.
    bool SlotHoldsOutlivingPointer(const llvm::Value* ptr, std::string& destKind, int depth) const;
    bool JoinAddressOutlivesCall(const llvm::Instruction* join, std::string& destKind,
                                 int depth) const;

    /*
     * Record every argument of the just-emitted call that reads a temp's `unique` field and lands
     * on a PLAIN `T*` parameter, and reject immediately when the callee body already proves the
     * store. `unique` / `move` parameters are NOT handled here - they state the claim at the call
     * site and are rejected by RejectOwningTempUniqueFieldIntoSinkParam.
     */
    void RecordTempUniqueFieldArgs(llvm::Value* callResult, const std::string& functionName,
                                   const std::vector<NamedVariable>& args);

    /*
     * The INTERFACE-DISPATCH twin of RecordTempUniqueFieldArgs. There is no `getCalledFunction()`
     * to ask, but the implementor set IS closed at end of module, so the slot is judged against
     * every implementor: EVERY one must provably store (store side) or may return (return side).
     */
    void RecordTempUniqueFieldInterfaceArgs(llvm::Value* callResult, const std::string& ifaceName,
                                            const InterfaceMethod& method,
                                            const std::vector<NamedVariable>& args);

    /*
     * ALL-of-implementors polarity, both directions. A diagnostic must be TRUE of the site it
     * fires at, and a single non-storing (resp. non-returning) implementor is a live dispatch on
     * which the claim is false - so one counter-example accepts. An implementor with no readable
     * body, or an untrustworthy implementor set, is a counter-example too: unknown accepts.
     */
    bool EveryImplementorRetainsInterfaceArg(const std::string& ifaceName,
                                             const std::string& methodName, size_t arity,
                                             unsigned paramIndex, std::string& destKind,
                                             std::string& implDetail);

    bool EveryImplementorMayReturnInterfaceArg(const std::string& ifaceName,
                                               const std::string& methodName, size_t arity,
                                               unsigned paramIndex);

    void RejectTempUniqueFieldInterfaceArgEscape(const TempUniqueFieldArg& entry,
                                                 const std::string& destKind,
                                                 const std::string& implDetail);

    /*
     * The DUAL of ParameterProvablyRetainsArgument: MAY this parameter come back out as the
     * call's result? A borrowing callee that returns its argument launders the temp-field
     * provenance off the value, so the existing declaration guard - which keys on ledger
     * identity - never sees it. Answering yes re-ledgers the call result at the call site.
     *
     * MAY, not MUST: an arm of a '?:', one side of a branch, or a slot the parameter was ever
     * parked in all count, because one path handing the pointer back is enough to dangle. The
     * over-approximation is safe only because the RECEIVING guard's reject set is narrow (a
     * binding that outlives the statement); a result consumed inside the statement stays legal.
     * Unknown - no callee, a declaration, an unreadable body, recursion - answers FALSE, which
     * is this guard family's ratified polarity.
     */
    bool ParameterMayReachReturn(const llvm::Function* fn, unsigned argIndex, int depth = 0);

    // Worklist half of ParameterMayReachReturn. Not memoized, for the same reason
    // OwningPtrProvablyEscapes is not: the query is rare and a body may still be growing.
    bool ValueMayReachReturn(const llvm::Value* root, int depth);

    /*
     * The RETURN-IDENTITY ALIAS PROOF: strictly stronger than ParameterMayReachReturn ("one path
     * MAY hand it back"). True only when EVERY return of `fn` hands back EXACTLY this argument
     * and the argument escapes the body nowhere else, so the result and the argument name one
     * object and the callee kept no second handle on it. That is what lets an owning destination
     * ADOPT a borrow-returning call's result instead of leaking the argument temp. Unknown - a
     * declaration, a body still growing (a callee defined BELOW its call site), a vararg,
     * recursion - answers FALSE, so the temp keeps leaking rather than being freed twice.
     */
    bool ParameterIsExactlyReturned(const llvm::Function* fn, unsigned argIndex, int depth = 0);

    // Half of the proof above: the returned value IS `arg`, seen through the no-op reshapes cflat
    // emits (bitcast, a parameter-prologue slot, a join whose every arm is the argument).
    bool ReturnedValueIsExactlyArgument(const llvm::Value* ret, const llvm::Argument* arg,
                                        int depth) const;

    // Re-ledger a laundered call RESULT as the owning temp its argument was, and retire the
    // argument's own entry so exactly one value in the statement carries the ownership.
    void AdoptLaunderedOwningTempResult(llvm::Value* callResult);

    /*
     * The BORROW-OF-A-LIVE-OWNER provenance, the REJECT-side twin of the adoption proof above.
     * Recorded per callee while its body is walked: EVERY pointer return of the function reads a
     * live `unique` FIELD (a `return nullptr;` is NEUTRAL - it owns nothing, exactly as a null
     * join arm proves and blocks nothing). The field's synthesized destructor still frees the
     * object, so the result is a borrow and an owning destination adopting it is a second owner.
     * Disjoint from ParameterIsExactlyReturned by construction: that proof needs the return value
     * to BE the argument, and a load of a field GEP never is.
     * Unknown ACCEPTS - a declaration, an opaque callee, or a callee walked below its call site
     * has no entry here and keeps compiling exactly as before.
     */
    void RecordUniqueFieldBorrowReturn(const llvm::Function* fn, bool proves,
                                       const std::string& fieldOwner);
    const UniqueFieldBorrowReturn* FindUniqueFieldBorrowReturn(const llvm::Function* fn) const;
    // An 'auto' return retype SPLICES the body into a new Function and erases the placeholder the
    // proof was recorded against, so carry the entry across or the accessor loses its provenance.
    void MigrateUniqueFieldBorrowReturn(const llvm::Function* oldFn, const llvm::Function* newFn);
    void RegisterUniqueFieldBorrowResult(llvm::Value* callResult, const UniqueFieldBorrowReturn& info);
    const UniqueFieldBorrowReturn* FindUniqueFieldBorrowResult(const llvm::Value* callResult) const;

    // Name a unique-field access for a diagnostic: "b.t", or the bare field / caller when a cast
    // or a join has already dropped half the provenance. MainListener delegates to this copy.
    static std::string DescribeUniqueFieldAccess(const NamedVariable& nv);

    /*
     * Parks `currentFunction` for the end-of-module resolves. FunctionBodyIsComplete refuses
     * `currentFunction`, so leaving the walk's last function set would exempt exactly that
     * callee. RAII because LogError THROWS out of the resolves.
     */
    struct NoCurrentFunctionScope
    {
        LLVMBackend* backend_;
        llvm::Function* saved_;
        explicit NoCurrentFunctionScope(LLVMBackend* backend)
            : backend_(backend), saved_(backend->currentFunction)
        { backend_->currentFunction = nullptr; }
        ~NoCurrentFunctionScope() { backend_->currentFunction = saved_; }
        NoCurrentFunctionScope(const NoCurrentFunctionScope&) = delete;
        NoCurrentFunctionScope& operator=(const NoCurrentFunctionScope&) = delete;
    };

    // Point diagnostics at a file/line recorded earlier, and put the compiler's own reporting
    // position back on the way out - LogError throws, so this cannot be a plain save/restore.
    struct ReportingFileScope
    {
        LLVMBackend* backend_;
        std::string file_;
        size_t line_;
        size_t column_;
        ReportingFileScope(LLVMBackend* backend, const std::string& file, size_t line, size_t column)
            : backend_(backend), file_(backend->sourceFileName), line_(backend->currentLine),
              column_(backend->currentColumn)
        {
            if (!file.empty()) backend_->sourceFileName = file;
            backend_->SetSourceLocation(line, column);
        }
        ~ReportingFileScope()
        {
            backend_->sourceFileName = file_;
            backend_->currentLine = line_;
            backend_->currentColumn = column_;
        }
        ReportingFileScope(const ReportingFileScope&) = delete;
        ReportingFileScope& operator=(const ReportingFileScope&) = delete;
    };

    // Re-ask the recorded questions now that every body is complete, and reject what is proven.
    void ResolveTempUniqueFieldArgEscapes();

    void RejectTempUniqueFieldArgEscape(const TempUniqueFieldArg& entry, const std::string& destKind);

    /*
     * Name one llvm argument of `fn` the way the SOURCE spells it: "the receiver object" for a
     * method's implicit `this`, "parameter 'x'" otherwise. ForwardRefScanner puts the receiver
     * INTO FunctionSymbol::Parameters (named `<Struct>__`), so the llvm arity and the cflat
     * parameter list line up 1:1 and the raw index would print an internal slot name.
     */
    std::string DescribeCalleeParameter(const llvm::Function* fn, unsigned argIndex) const;

    // True when llvm argument `argIndex` of `fn` is a method's implicit receiver.
    bool ArgumentIsMethodReceiver(const llvm::Function* fn, unsigned argIndex) const;

    // Worklist half of ParameterRetainsArgument: true when `root`, any pointer derived from it, or
    // any pointer read out of its pointee can outlive the call. An unrecognized user escapes.
    bool OwningPtrEscapes(const llvm::Value* root, int depth, bool returnIsEscape = true);

    /*
     * Backward origin walk for the store-through rule: can `val` name memory the CALLER still
     * owns? False only for something the callee provably produced itself - a non-global constant,
     * an owning-return call result (a fresh allocation), or a `move` parameter whose ownership the
     * caller has already given up. Every other origin, a plain parameter included, answers true.
     */
    bool StoredValueMayBeCallerOwned(const llvm::Value* val, int depth) const;

    // cflat-level facts about an emitted function, by llvm::Function identity. A miss answers the
    // conservative way for its caller (not owning / not `move`).
    const FunctionSymbol* FindSymbolForFunction(const llvm::Function* fn) const;

    bool CalleeReturnsOwned(const llvm::Function* fn) const;

    // True when llvm argument `argIndex` is a cflat `move` parameter. An ABI-lowered or otherwise
    // unmappable signature answers false, so the store-through rule stays conservative.
    bool ParameterIsMove(const llvm::Function* fn, unsigned argIndex) const;

    // True when a loaded value can carry a pointer INTO the pointee's ownership graph, so it
    // cannot be treated as a self-contained scalar copy.
    bool TypeHoldsPointer(const llvm::Type* t) const;

    // llvm.dbg.* / llvm.lifetime.* / llvm.mem* touch the POINTEE (or only debug info), never the
    // pointer value itself. Every other intrinsic is left to the general call path, whose
    // declaration-only body answers "retains"; an llvm.mem* READING the pointee is caught there.
    bool CallIsPointerOpaqueIntrinsic(const llvm::Function* callee) const;

    // True when a stack slot is only loaded from and stored into directly - its address never
    // leaves, so a pointer parked there cannot escape through the slot.
    bool AllocaIsLoadStoreOnly(const llvm::AllocaInst* slot) const;

    // Ledger a `new` result (see ownedNewTemps_).
    void RegisterOwnedNewTemp(llvm::Value* value, const std::string& typeName = {}, uint64_t allocAlign = 0);

    const OwnedNewTemp* FindOwnedNewTemp(llvm::Value* value) const;

    bool IsOwnedNewTemp(llvm::Value* value) const;

    // Carry the owning bit from an arm value onto a derived value ('?:' phi / select result).
    void PropagateOwnedNewTemp(llvm::Value* from, llvm::Value* to);

    // Ledger the interface a fat interface value ({vtable,data}) was produced against, keyed by
    // value identity. The fat struct is ONE shared LLVM type for every interface, so a '?:' arm
    // that mixes a fat arm with a thin arm needs this to learn which interface to box the thin
    // arm into (UnifyTernaryArmTypes). Retired with ownedNewTemps_ (see FlushOwnedTemps).
    void RegisterFatInterfaceValueTypeName(llvm::Value* value, const std::string& ifaceName);

    std::string FindFatInterfaceValueTypeName(const llvm::Value* value) const;

    // Best-effort source interface for ReboxInterfaceIfNeeded: prefer the caller's NamedVariable
    // TypeName; fall back to the ledger for a '?:' join, which carries no NamedVariable at all.
    std::string ResolveFatInterfaceSrcName(const llvm::Value* value, const std::string& declaredName) const;

    // Ledger the class a `new` result points at (see valueElementTypeNames_).
    void RegisterValueElementTypeName(llvm::Value* value, const std::string& typeName);

    std::string FindValueElementTypeName(llvm::Value* value) const;

    // Declared class of the live pointer binding whose storage slot is `storage`. Answers from the
    // DECLARED type, which a borrowed value has and the `new`-site ledger does not record.
    std::string FindDeclaredElementTypeNameForStorage(const llvm::Value* storage) const;

    /*
     * The DECLARED type of the binding whose storage slot is `storage` - a local, an argument, or
     * a global - or nullptr. Storage-keyed counterpart to FindLiveNamedVariable, for callers that
     * need the whole TypeAndValue rather than just a class name: FindDeclaredElementTypeNameForStorage
     * deliberately answers "" for the pointer-shaped bindings (a `T**`, a view, a const-array slot),
     * which is exactly what a shape DIAGNOSTIC needs to see. Read-only: never box off this, these
     * are the shapes that must NOT be boxed.
     *
     * A global keeps its declared type in globalVariableTypes, parallel to globalNamedVariable and
     * written with it, so the GlobalVariable is matched by identity and the type fetched by name.
     * Callers must COPY the result before anything can pop a scope or touch either map - the
     * pointer aliases live map storage and does not survive a rehash.
     */
    const TypeAndValue* FindDeclaredTypeAndValueForStorage(const llvm::Value* storage) const;

    /*
     * Resolve the concrete class a pointer VALUE points at. The `new`-site ledger answers an
     * owning temp, a direct call uses its registered pointer return type, and a BORROWED value is
     * a plain load of a typed local, which is in no ledger, so fall back to the binding's type.
     */
    std::string ResolvePointerElementTypeName(llvm::Value* value) const;

    // Ledger a value a `move` expression detached (see movedOutPtrValues_). An INTERFACE fat
    // pointer is accepted too - it is a struct, but it is detached by the same move contract.
    void RegisterMovedOutPtrValue(llvm::Value* value);

    bool IsMovedOutPtrValue(llvm::Value* value) const;

    // Ledger a pointer value a `move` of a BORROWED source produced (see movedBorrowedPtrValues_).
    void RegisterMovedBorrowedPtrValue(llvm::Value* value, const std::string& originName);
    // Same value, narrowed: the borrow was read out of a FIELD of that origin, so a diagnostic
    // must not prescribe `move <origin>` (see NamedVariable::BorrowedThroughField).
    void RegisterMovedBorrowedThroughField(llvm::Value* value);
    bool IsMovedBorrowedThroughField(llvm::Value* value) const;

    // True when `value` provably owns nothing because a `move` of a borrow produced it. `originOut`
    // receives the borrow's origin name so the diagnostic can name the real owner.
    bool IsMovedBorrowedPtrValue(llvm::Value* value, std::string* originOut = nullptr) const;

    // A named owner adopted the value; retire the entry so nothing else claims it.
    void ConsumeOwnedNewTemp(llvm::Value* value);

    void UnregisterOwnedPtrTemp(llvm::Value* value);

    // True when the insert block is active (non-null, no terminator yet).
    // Guards the Flush* functions: emitting into a terminated block is illegal IR.
    bool IsInsertBlockLive() const;

    // Free-safety check for a pending owned temp registered in block `bb`: the value
    // defined there may be freed at `curBlock` iff `bb` dominates `curBlock` (every
    // path to here passed through its definition). Same-block is the trivial case;
    // a plain `bb == curBlock` test is too conservative when an intermediate temp's
    // block precedes - and dominates - a later child's closure-built block in the
    // same full-expression. The DominatorTree is built lazily (only on the first
    // cross-block temp) and reused across one flush via the caller-owned `dt`.
    bool OwnedTempDominatesHere(llvm::BasicBlock* bb, llvm::BasicBlock* curBlock,
                                std::optional<llvm::DominatorTree>& dt) const;

    // Free one owned string temp at the current insert point. The dtor is owned-bit gated, so
    // running it on a borrow value is a no-op. Caller owns dominance safety.
    void EmitOwnedStringTempFree(llvm::Value* value);

    void FlushOwnedStringTemps();

    void RegisterOwnedClosureTemp(llvm::Value* value);

    void UnregisterOwnedClosureTemp(llvm::Value* value);

    // True when `value` is a freshly-lowered lambda-literal temp (registered, not yet claimed).
    // Used to gate the implicit non-capturing-lambda -> thin function<T> coercion: a stored
    // Lambda<T> value is NOT a temp, so it cannot implicitly narrow (must use .toFunction()).
    bool IsOwnedClosureTemp(llvm::Value* value) const;

    // Free one owned closure temp at the current insert point. Caller owns dominance safety.
    void EmitOwnedClosureTempFree(llvm::Value* value);

    void FlushOwnedClosureTemps();

    // Register a by-value owning-struct temp (already spilled to `alloca`) for full destruction at
    // the end of the current full expression. The dtor takes a T*, so no spill is needed at flush.
    void RegisterOwnedStructTemp(llvm::Value* alloca, const std::string& typeName);

    void FlushOwnedStructTemps();

    // Free one unowned owning-pointer temp: null guard, full destructor, then the matching
    // deallocator. Value-based twin of EmitOwningPtrCleanup (which loads from a named local's
    // storage); the temp is a bare SSA pointer with no slot to load from or null out.
    void EmitOwnedPtrTempFree(llvm::Value* ptrVal, const std::string& typeName, uint64_t allocAlign);

    // Free every owning-pointer temp nothing adopted. Each free opens new blocks, so the insert
    // block and the dominator tree are recomputed per temp instead of hoisted out of the loop.
    void FlushOwnedPtrTemps();

    // Register a by-value owning-struct RVALUE temp passed as a BORROW argument for destruction at
    // the end of the current full expression. A borrow param does not free it and it has no named
    // owner, so without this it leaks its owned field(s) (e.g. a string). The caller gates on the
    // param NOT being `move` (a move param transfers ownership to the callee). The temp is
    // identified exactly as the owned-STRING arg path (see RegisterOwnedStringTemp at the arg site):
    // a `CallInst` result is a freshly produced value with no named owner, whereas a named local
    // (loaded value) is freed by its own scope dtor - registering it would DOUBLE-FREE. That
    // distinction is essential because an operator operand reaches here with Storage cleared, so
    // Storage alone cannot tell a named operand from a temp. Alias/pointer/string/closure values and
    // fields of an already-registered owning temp (FromOwningTempField) run their own paths.
    void RegisterBorrowedOwningStructTemp(const NamedVariable& arg);

    // Free all unnamed owned temporaries (string, closure, struct) at an end-of-full-expression
    // boundary. The return path keeps the three explicit (it interleaves Unregister between them).
    // Sizes of the four pending owned-temp ledgers, so a nested lowering can free exactly what
    // IT registered (see FlushOwnedTempsSince).
    struct OwnedTempMark
    {
        size_t Strings  = 0;
        size_t Closures = 0;
        size_t Structs  = 0;
        size_t Ptrs     = 0;
    };

    // Drop the ledger entries added since index `from`, preserving only the entry for `keep`.
    template <typename ListT, typename GetT>
    static void TrimOwnedTempsSince(ListT& list, size_t from, llvm::Value* keep, GetT get)
    {
        size_t write = from;
        for (size_t i = from; i < list.size(); ++i)
            if (get(list[i]) == keep) list[write++] = list[i];
        list.resize(write);
    }

    OwnedTempMark MarkOwnedTemps() const;

    /*
     * Free every owned temp registered since `mark` at the CURRENT insert point, except `keep`
     * (the one value the caller hands onward). This is an end-of-SCOPE flush for a region that
     * the end-of-full-expression flush cannot reach: a '?:' arm registers its temps in the arm's
     * own block, which does not dominate the join, so FlushOwnedTemps would silently skip them
     * (OwnedTempDominatesHere) and every one of those buffers would leak.
     *
     * `hoistTo` (a block that dominates the join, i.e. the one holding the arm branch) opts the
     * ALLOCA-based struct temps out of that early free: each is zeroed there instead and re-keyed
     * to it, so the end-of-statement flush destructs it AFTER the joined value is consumed, and
     * the zeroed record makes that destructor a no-op on the path where the arm did not run.
     * Freeing them in the arm is a use-after-free whenever the join yields a pointer INTO the
     * temp. The string / closure / ptr ledgers hold SSA values that the resume block cannot name,
     * so they always take the early free.
     */
    void FlushOwnedTempsSince(const OwnedTempMark& mark, llvm::Value* keep,
                              llvm::BasicBlock* hoistTo = nullptr);

    // Zero `temp`'s storage in `hoistTo` (before its terminator) and re-key it there. False when
    // the temp is not an entry-block alloca of that function, i.e. cannot be hoisted.
    bool HoistOwnedStructTempTo(PendingOwnedStructTemp& temp, llvm::BasicBlock* hoistTo);

    // Emit one struct temp's destructor. A hoisted temp (LiveFlag set) is guarded on that flag,
    // which OPENS BLOCKS - after one, the caller must re-read the insert block and drop any
    // cached dominator tree before judging the next temp.
    void EmitOwnedStructTempFree(const PendingOwnedStructTemp& temp);

    // Drop the ledger entries registered since `mark` WITHOUT emitting any free. For an aborted
    // region (an arm whose lowering threw): those entries are keyed to blocks that no longer
    // reach the join, so leaving them would carry a stale key past this expression.
    void DiscardOwnedTempsSince(const OwnedTempMark& mark);

    void FlushOwnedTemps();

    // Release the resource a single named local owns, exactly as scope-exit cleanup does. Borrows,
    // primitives, and moved/aliased locals are no-ops. Shared by EmitDestructorsForScope and the
    // `drop` statement so a value is released identically at scope exit and on explicit drop.
    void DropValue(const NamedVariable& namedVar);

    // True when DropValue would release a real resource for this local (owning ptr/interface/
    // array/struct/string). Borrows, primitives, aliased/return-moved locals own nothing, so an
    // explicit `drop` on them is a harmless no-op. Mirrors DropValue's branch conditions exactly.
    bool OwnsDroppableResource(const NamedVariable& namedVar) const;

    void EmitDestructorsForScope(const StackState& frame);

    int MintAliasScope();

    void AttachViewNoalias(llvm::Instruction* memInst, int scopeId);

    int GetOrMintViewScope(const std::string& originKey);

    void createFunctionBlock(llvm::Function* fn, const std::string& friendlyName, std::vector<LLVMBackend::TypeAndValue> arguments, bool returnsOwned = false, bool returnIsArrayView = false, const std::string& returnTypeName = "");

    llvm::DIType* GetDIType(const TypeAndValue& tv);

    void RegisterBuiltinString();

    void RegisterBuiltinStrConcat();

    void Init();

    void RegisterBuiltinClosure();

    void EnsureClosureLifetimeRegistered();

    // Register a closure type used as a generic argument (e.g. list<Lambda<int(int)>>). `sig` is the
    // call descriptor: IsFunctionPointer + TypeName (__closure_fat_ptr | __c_fn_ptr) + FuncPtr*.
    // The encoded name is also made a real dataStructures value type so it stores, pointer-wraps,
    // and (fat) destructs like any owning element. Idempotent.
    void RegisterEncodedClosureType(const std::string& encodedName, const TypeAndValue& sig);

    bool IsEncodedClosureType(const std::string& name) const;
    // A THIN encoded closure is a bare code pointer with no struct backing, so it copies, stores
    // and passes exactly like a plain pointer value.
    bool IsThinEncodedClosureType(const std::string& name) const;

    // The source spelling an encoded closure type argument was written as. DIAGNOSTICS ONLY.
    std::string SpellEncodedClosureType(const TypeAndValue& enc) const;

    // A FAT encoded closure has the same representation as a spelled `Lambda<T>` - the
    // `__closure_fat_ptr` struct - so every ownership rule keyed on that struct applies to it.
    bool IsFatEncodedClosureType(const std::string& name) const;
    const TypeAndValue* GetEncodedClosureType(const std::string& name) const;

    // Called lazily the first time a string local's destructor needs to fire.
    // Must run after cruntime.cb is compiled so `free` is in the LLVM module.
    void EnsureStringDtorRegistered();

    llvm::Value* ClearStringOwnedBit(llvm::Value* value);

    // Recursively clear the runtime OWNED bit on every owning value-type field of a struct SSA
    // value, turning the value into a BORROW. Used when an accessor returns a struct taken from a
    // collection it still owns (e.g. list.get's `alias T get` returning `_data[i]`): the caller
    // gets a shallow copy whose always-run full destructor would otherwise free buffers the
    // container still holds. The `alias` compile-time machinery suppresses the destructor at the
    // immediate binding site, but a copy that escapes that tracking (e.g. a for-in loop variable)
    // is still destructed - clearing the runtime bit makes that destructor a safe no-op, mirroring
    // what ClearStringOwnedBit already does for a scalar string borrow return. Pointer/view/simd/
    // bitfield fields carry no value-level owned bit and are left untouched. An owning fixed-array
    // value field IS cleared element-by-element (FULLY-LIVE contract, in lockstep with the
    // destructor and memberwise-copy field selection).
    llvm::Value* ClearStructOwnedBits(llvm::Value* value, const std::string& typeName);

    // Above this element count a fixed array's per-element work (default-init, destruction) is
    // emitted as a runtime loop instead of unrolled, so a large `T[1000000]` stays O(1) IR.
    static constexpr uint64_t kMaxUnrolledArrayElements = 16;

    /*
     * Peel a fixed array's LLVM type down to its innermost element type and return the total
     * element count ([N x [M x T]] -> T, N*M). Multi-dimensional arrays are contiguous, so a flat
     * walk of that count reaches every element. Returns 0 for a non-array type, leaving `elemTy`
     * untouched. Derived from the LLVM type rather than the declaration's dimensions so an array
     * alias (`using Vec3 = float[3];`), whose dims never reach ConstArraySize, is covered too.
     */
    static uint64_t PeelFixedArrayType(llvm::Type* ty, llvm::Type*& elemTy);

    /*
     * Walk the `n` contiguous elements of the fixed array at `base`, invoking `emitElem` on each
     * element pointer. Small arrays are unrolled; above kMaxUnrolledArrayElements a runtime loop
     * keeps the IR O(1) in N. The loop counter is a phi, so no alloca lands in the loop body (see
     * AllocaAtEntry). Drives the caller's builder, so this serves both the main codegen builder
     * and the local builder inside a synthesized destructor.
     */
    void EmitFixedArrayElementWalk(llvm::IRBuilder<>& b, llvm::Value* base, llvm::Type* elemTy,
                                   uint64_t n, const std::function<void(llvm::Value*)>& emitElem);

    /*
     * Call `dtor` on `storage`: once when it holds a scalar, once per element when it holds a
     * fixed array. `T[N] a;` owns all N elements - calling the destructor on the base pointer
     * alone destructs element [0] and leaks the rest. Elements are destructed in declaration
     * order, matching the synthesized destructor's field order.
     */
    void EmitFullDestructorOverStorage(llvm::IRBuilder<>& b, llvm::Value* storage,
                                       llvm::Type* storageTy, llvm::Function* dtor);

    // `unique T* field`: null-checked delete of the pointee. Emitted into the synthesized
    // .dtorfull wrapper at teardown, and (with `replacement` set) ahead of a reassignment store,
    // so a field is freed identically on both paths. Mirrors EmitOwningPtrCleanup but drives the
    // caller's local builder.
    //
    // `replacement` is the pointer about to be stored: when the field already holds it, this is a
    // self-assign and the free is skipped (freeing would leave the store dangling). The test is at
    // runtime because the spellings that reach here - `h->slot = h->slot`, a bare `item = item`
    // inside a method (whose NamedVariable carries no FieldName), a source that merely aliases the
    // field - are not all distinguishable by name at the assignment site. Pass null at teardown,
    // where there is no incoming value.
    void EmitUniqueFieldDelete(llvm::IRBuilder<>& b, llvm::Value* fieldPtr,
                               llvm::Function* pointeeDtor, const std::string& typeName,
                               uint64_t allocAlign, llvm::Value* replacement = nullptr);

    // Release a `unique` fixed-array FIELD slot by slot from the synthesized destructor, reusing
    // the very walk a `unique` array LOCAL already gets. The member builder and currentFunction
    // are retargeted at the wrapper body because that walk drives them and creates basic blocks.
    void EmitUniqueArrayFieldRelease(llvm::IRBuilder<>& b, llvm::Value* fieldPtr,
                                     llvm::Type* fieldTy, const std::string& typeName,
                                     bool isIface, uint64_t allocAlign);

    // Release a scalar `unique IFace` FIELD (a {i8*,i8*} fat pointer) from the synthesized
    // destructor, reusing the same vtable-dtor-slot walk a `unique` interface LOCAL gets.
    void EmitUniqueInterfaceFieldRelease(llvm::IRBuilder<>& b, llvm::Value* fieldPtr,
                                         const std::string& ifaceName);

    llvm::Function* GetOrCreateFullDestructor(const std::string& typeName);


    // Resolve the destructor to call from a `delete` site. Differs from
    // GetOrCreateFullDestructor only when the eager resolve fails: a generic container
    // (e.g. `list<Stmt*>`) is monomorphized while its element type is still incomplete -
    // when Stmt itself has a `list<Stmt*>` field, registering Stmt forces the list
    // instantiation mid-registration, so `delete _data[i]` inside ~list resolves Stmt's
    // full dtor to null and the call is baked out permanently. To avoid that, bind the
    // delete to a stable forward-declared `<type>.dtordeferred` symbol whose body is
    // emitted at finalization (EmitDeferredFullDestructorBodies), once every type is
    // complete. The boolean callers of GetOrCreateFullDestructor are unaffected.
    llvm::Function* GetFullDestructorForDelete(const std::string& typeName);

    // Emit bodies for every deferred destructor wrapper handed out by
    // GetFullDestructorForDelete. Called once at finalization, when all types (and their
    // monomorphizations) are complete, so the real full destructor now resolves. The
    // wrapper simply forwards to it (null-guarded by the delete call site); a type that
    // turns out trivially-destructible gets an empty body, so the bound call is a no-op.
    void EmitDeferredFullDestructorBodies();

    bool HasUserCopyMethod(const std::string& typeName) const;

    bool TypeNeedsManagedCopy(const std::string& typeName);

    // A function whose unannotated by-value struct return always hands back a borrowed
    // parameter. The 'alias' inference cannot be applied during the forward-ref scan (the
    // returned struct's fields may not be registered yet), so it is queued here and resolved
    // once the scan of the translation unit has finished.
    struct PendingAliasReturn
    {
        std::string FunctionName;
        std::string ReturnTypeName;
        std::vector<std::string> ParamTypeNames;
    };
    std::vector<PendingAliasReturn> pendingAliasReturnInference;

    void QueueAliasReturnInference(const std::string& functionName, const std::string& returnTypeName,
                                   const std::vector<TypeAndValue>& params);

    // Apply the queued 'alias' return inference, now that every struct's fields are known.
    // Only a return type that transitively owns a 'unique' pointer is affected: a copyable
    // struct return duplicates nothing, so it keeps its plain by-value semantics.
    void ResolvePendingAliasReturnInference();

    // A type that owns a resource BY VALUE: `string`, an owning value struct, or a FAT closure
    // (Lambda<...>) which owns its captured env. A thin C function pointer owns nothing and is
    // excluded. Used to gate move-sink transfer / borrow-laundering diagnostics.
    bool IsOwningValueOrClosureType(const std::string& typeName);

    bool IsOwningValueType(const std::string& typeName);

    // True when the array-view element described by `elemField` owns nothing, so bit-copying it
    // (the get/set noalias fast path) is safe: no string, no owning-dtor struct/closure, no
    // interface fat value, no owning (`unique`) raw pointer. Anything unproven falls through to
    // the real method body, which already handles every ownership arm correctly.
    bool ArrayViewElementOwnsNothing(const TypeAndValue& elemField);

    // True when `typeName` owns a raw pointer via `unique` - directly, or through a by-value
    // member that does. Such a type has no memberwise copy: cloning a `unique` field would need a
    // generic deep-clone of the pointee, which does not exist, and a shallow copy would hand the
    // same pointer to two synthesized destructors. `outPath` receives the offending field path
    // (e.g. "h.slot") for diagnostics. Value-member cycles are impossible (infinite size), but
    // `seen` guards defensively so a malformed registry cannot recurse forever.
    bool TypeOwnsUniquePointer(const std::string& typeName, std::string* outPath = nullptr,
                               std::unordered_set<std::string>* seen = nullptr) const;

    bool HasCopyOverloadFor(const std::string& typeName) const;

    // True when a value of `typeName` can be COPIED (an independent duplicate) rather than
    // requiring a move. Mirrors the is_copyable intrinsic exactly: strip ownership qualifiers,
    // then a pointer or a non-struct type (string, primitives, closures) is copyable; a struct
    // is copyable iff it has a copy() overload or does not transitively own a `unique` pointer.
    // This is the predicate for the copy-on-assign flip (copyable owners copy, non-copyable move).
    bool IsCopyableType(const std::string& typeNameIn) const;

    // True when an owning-sink parameter actually CONSUMES its argument for the concrete
    // (monomorphized) type. An unconditional-`move` inferred sink consumes any owner (existing
    // behavior). A CONSUME-inferred sink (plain-store/conditional-move body, flagged structurally by
    // the scanner) consumes ONLY a non-copyable owner: a copyable owner's store is a COPY, so the
    // caller keeps its value and the param stays a borrow. This is the single copyability
    // discriminator every concrete-type consumer (call-site poison, scope-exit drop, rvalue-temp
    // ownership) consults, so the structural scanner flag and the full-info decision stay in sync.
    bool OwningSinkConsumesConcrete(const TypeAndValue& p);

    // Per-parameter 'move' agreement between a funcptr DESTINATION and its source. A literal that
    // INFERRED an owning sink satisfies a declared `move` - it already consumes - which is the one
    // way a lambda literal could not previously state the sink.
    bool FuncPtrParamMoveAgrees(const TypeAndValue::FuncPtrParam& dest,
                                const TypeAndValue::FuncPtrParam& src);
    // A closure whose parameter i is an owning sink crossing into a DECLARED closure type that
    // spells neither `move` nor a sink at i: the fact cannot ride the fat struct, so the callee
    // consumes and the caller frees again. Returns the 0-based index, or -1 when nothing is lost.
    int FindLostClosureSinkParam(const TypeAndValue& dest, const TypeAndValue& src);
    // The required spelling for the crossing FindLostClosureSinkParam rejected.
    std::string DescribeLostClosureSink(const TypeAndValue& dest, size_t index,
                                        const std::string& destDescription);

    // True when `typeName` has an AUTHOR-written (library/user) copy() - not the memberwise
    // synth generated on demand. The synth is registered under "<type>.copy.synth"; a real
    // copy is anything else. This is the deep-copy guarantee a container (list/dictionary)
    // gives via its own copy() vs. the shallow pointer-field byte copy the synth would do.
    bool HasRealCopyOverloadFor(const std::string& typeName) const;

    // True when an owning value type T can be safely DEEP-copied into a closure env by routing
    // through CreateOverloadedFunctionCall("copy", {T}) - i.e. capturing it by value behaves
    // exactly like `T x = src;`. Admitted when: T has a real copy() (list/dictionary/string/
    // hand-written user copy - authoritatively deep), OR T relies only on the memberwise synth
    // AND has no raw pointer/view field (the synth shallow-shares those while T's dtor frees
    // them -> double-free; such a type needs a hand-written copy() to be capturable).
    bool ClosureCaptureDeepCopyable(const std::string& typeName);

    // True when a value struct owns a raw resource that its memberwise synth would shallow-share:
    // the author wrote a destructor AND a field is a raw pointer/view. Copying such a type via the
    // synth bit-copies the pointer while the dtor frees it on both instances (double-free), so it is
    // NOT copyable without a hand-written copy(). `string`/closures have dedicated deep-copy/clone
    // paths and are excluded. Narrower than ClosureCaptureDeepCopyable's raw-pointer test by the
    // user-dtor gate: a raw pointer with NO destructor is a borrow the synth shallow-shares by design.
    bool StructSynthCopyUnsafe(const std::string& typeName) const;

    // True when `typeName` defines `operator->` (its implicit `this` is the sole parameter).
    // Drives the operator-> forward-on-miss path for both `.` and `->`.
    bool HasArrowOverloadFor(const std::string& typeName) const;

    // True when `memberName` is a plain SCALAR field of `typeName` (no pointer, array, string,
    // struct, interface or function pointer). Reading such a field yields a self-contained value
    // that cannot alias the object's heap storage - the property the owning-pointer temp cleanup
    // needs before it may free a pointee that was only dereferenced to reach a member.
    bool MemberIsScalarField(const std::string& typeName, const std::string& memberName) const;

    // True when `typeName` has a directly-accessible member `memberName`: a field, a
    // bitfield, a member function (any overload whose first param is this type), or a
    // winrt vtable slot. Own members take priority over operator-> forwarding, so this
    // is the miss test that gates forwarding (own member shadows a forwarded one).
    bool TypeHasMember(const std::string& typeName, const std::string& memberName) const;

    llvm::Function* GetOrCreateMemberwiseCopy(const std::string& typeName);

    // Generate a per-closure capture-cleanup function `void(i8* dst, i8* src, i32 mode)` for a
    // capture block `capTy` whose owning-value fields are listed in `owningFields` (fieldIndex,
    // typeName). mode 1 (CLONE): deep-copy each owning field FROM src INTO dst, overwriting the
    // shallow byte-copy the env clone left. mode 0 (FREE): destruct each owning field in dst.
    // This is what makes a closure OWN its captured strings/containers (independent lifetime),
    // so a captured owning value survives after its source is destructed and is freed exactly
    // once. Returns null when there are no owning fields (the scalar-only fast path).
    llvm::Function* GenerateClosureCaptureCleanup(const std::string& name, llvm::StructType* capTy,
        const std::vector<std::pair<unsigned, std::string>>& owningFields);

    // Called lazily from ParseFormatString after string concat is first needed.
    void EnsureStrConcatRegistered();

    // x86-64 System V va_list is `__va_list_tag[1]` (24 bytes: gp_offset i32,
    // fp_offset i32, overflow_arg_area ptr, reg_save_area ptr) passed by tag
    // pointer; Windows x64 va_list is just a pointer cursor. cflat carries `va_list`
    // as a `ptr` everywhere (GetType), which already matches a SysV tag *pointer*.
    // The only mismatch is what va_start fills: on SysV it must initialize a 24-byte
    // tag, not an 8-byte cursor slot. So on SysV we back the va_list slot with a real
    // tag buffer and point the slot at it; the slot's value (the tag pointer) then
    // forwards by value to libc vsnprintf/vsscanf unchanged. cflat never does va_arg
    // itself, so no SysV va_arg sequence is needed.
    llvm::StructType* VaListTagType();

    void CreateVaStart(llvm::Value* apAlloca);

    void CreateVaEnd(llvm::Value* apAlloca);

    llvm::Value* CreateFloatIntrinsic(const std::string& methodName, llvm::Value* floatVal);

    llvm::Value* CreateRdtscp();

    llvm::Value* CreateReadCycleCounter();

    void CreateLfence();

    void CreateFenceAcquire();

    void CreatePause();

    llvm::Value* CreatePopcount(llvm::Value* intVal);

    llvm::Value* CreateCtz(llvm::Value* intVal);

    llvm::Value* CreateClz(llvm::Value* intVal);

    void CreatePrefetch(llvm::Value* addr);

    llvm::Value* CreateFma(llvm::Value* a, llvm::Value* b, llvm::Value* c);

    llvm::Value* CreateExpect(llvm::Value* cond, bool expected);

    llvm::Value* CreateIntegerConvert(const std::string& methodName, llvm::Value* intVal);

    // Fire deferred compile_error() diagnostics: a poisoned function errors only if it has a real
    // call (a CallBase user). A vtable slot references the function without calling it, so a mere
    // declaration of a unique-element list (whose copy() is poisoned) stays legal.
    void CheckPoisonedFunctionCalls();

    bool VerifyModule();

    // Build a TargetLibraryInfoImpl for `triple` with cflat's reimplemented C stdio format
    // family (vsnprintf/vfprintf/vsscanf/vfscanf) marked unavailable on non-Windows targets.
    // Without this, instcombine (opt pipeline) and the codegen libcall simplifier fortify-fold
    // our __vsnprintf_chk / __vfprintf_chk libc calls back into plain vsnprintf / vfprintf, which
    // on ELF bind to cflat's OWN reimplementations and recurse forever. Windows keeps the real
    // libc names, so the unavailable marks only fire off-Windows.
    llvm::TargetLibraryInfoImpl MakeStdioSafeTLII(const llvm::Triple& triple) const;

    void RunModulePasses(llvm::ModulePassManager& MPM);
    void RunBaselinePasses();
    void RunGlobalDCE();
    void OptimizeModule(int optimizationLevel);

    bool SaveToFile(const std::string& filename);

    bool WriteBitcode(const std::string& filename);

    std::string FindClangCl() const;

#if defined(__APPLE__)
    // Bundled Mach-O linker (deployed next to cflat by the build), else PATH.
    // "" if absent, in which case the link falls back to the host clang driver.
    std::string FindBundledLd64Lld() const;
#endif

    // GCC-style C compiler driver for the ELF (non-Windows) target. Prefers clang to
    // match the LLVM the rest of the pipeline links, then falls back to cc/gcc.
    std::string FindCDriver() const;

    // Compile a .c input to an ELF object with a GCC-style driver and queue it for the
    // ELF link. Mirrors CompileCFile's MSVC path but with POSIX flags (-c/-o/-D/-fPIC).
    bool CompileCFileElf(const std::string& cSourcePath, const std::string& programAlias);

    // GetCflatCacheDir() / GetUserCacheDir() / SetCacheDirOverride() are declared in the
    // public section below (near RunInit) since main.cpp needs to call them for
    // --init/--init-local/--init-clear/--init-clear-local.
    // Records the running cflat.exe's full path to <cache dir>\compiler_path.txt
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

    bool CompileCFile(const std::string& cSourcePath, const std::string& programAlias = "");

    // NOT routed through CompileCFile: that path runs ExtractCSignatures and registers the
    // functions as CFlat externs, which we don't want for an internal handler.
    bool CompileCrashHandlerObject(const std::string& arch);

    // Compile core/cflat_builtins.c (freestanding memcpy/memset/memcmp) so those
    // compiler-emitted calls resolve locally instead of importing from VCRUNTIME140.dll.
    // Must pass -fno-builtin or LLVM's loop-idiom pass rewrites the byte loops back into
    // memcpy/memset calls, recursing into these very definitions. Returns false (and logs)
    // on any failure; the caller treats it as best-effort.
    bool CompileBuiltinsObject(const std::string& arch);

    // True if the Microsoft Visual C++ runtime (vcruntime140.dll) is present on this system.
    // Used only to phrase a helpful note for --asan builds, which are the one output kind that
    // still depends on it. Checks both System32 and SysWOW64 so 32-bit and 64-bit installs both
    // count; if SystemRoot is somehow unknown we assume installed rather than nag.
    static bool VcRuntimeInstalled();

    // Map a (preferably desugared) C type spelling onto a CFlat TypeAndValue.


    bool MapCTypeToTypeAndValue(std::string ctype, TypeAndValue& out);

    bool ParseCFunctionPointerSpelling(const std::string& s, TypeAndValue& out,
                                       std::unordered_set<std::string>& visited);

    static std::string StripFixedArrayDims(const std::string& ctype, std::vector<uint64_t>& dims);

    // For a pointer-to-aggregate spelling like "const struct Foo *", return the tag ("Foo")
    // and the indirection level via outPtr. Returns "" (and leaves outPtr at 0) when the
    // spelling is not a struct/union pointer. Used to keep `Foo*` record fields typed instead
    // of decaying to void* - so COM `lpVtbl` member access resolves.
    static std::string AggregatePointeeTag(const std::string& spelling, int& outPtr);

    bool MapCTypeToTypeAndValueImpl(std::string ctype, TypeAndValue& out,
                                    std::unordered_set<std::string>& visited);

    // FNV-1a 64-bit hash of a file's bytes. Returns false if the file can't be read.
    static bool HashFileFnv1a(const std::string& path, uint64_t& outHash);
    bool HashFileContents(const std::string& path, uint64_t& outHash) const;

    void RegisterCSignatures(const std::vector<CSigEntry>& sigs, const std::string& fileForLsp,
                             const std::string& programAlias = "");

    std::vector<std::string> BuildClangDriverArgs(const std::string& headerDir,
                                               const std::vector<std::string>& extraDefines,
                                               bool errorRecovery, bool asCxx = false) const;

    bool MapRawSig(const cflat_cinterop::RawSig& r, CSigEntry& e);

    bool MapRawGlobal(const cflat_cinterop::RawGlobalVar& r, CGlobalEntry& e);

    bool ClassifyRawMacro(const cflat_cinterop::RawMacro& r, CMacroEntry& e);

    void AdoptRawTypedefs(const cflat_cinterop::ExtractResult& raw);

    // Surface a C typedef that names a record as a CFlat type alias. Two shapes:
    //   by value  `typedef struct tagMSG MSG;`            -> MSG            = tagMSG
    //   handle    `typedef struct CGColorSpace *CGColorSpaceRef;` -> CGColorSpaceRef = CGColorSpace*
    // The handle form is the C opaque-pointer idiom: the tag is often only forward-declared, which
    // RegisterCRecords registers as an opaque shell, so the alias binds even with no struct body.
    // Requiring the tag to be a registered record keeps both shapes confined to the bound header's
    // own types. Trailing stars stay in the target string - GetType peels them into pointer depth.
    void CollectRecordTypedefAliases(const cflat_cinterop::ExtractResult& raw,
                                     std::vector<std::pair<std::string, std::string>>& out);

    // Register record-typedef aliases (from CollectRecordTypedefAliases or a cache entry). Never
    // shadows a real type or an existing alias (first-writer-wins).
    void RegisterRecordAliases(const std::vector<std::pair<std::string, std::string>>& aliases);

    // Keep the transitive closure of in-scope records over their by-value field deps, plus any
    // record referenced BY VALUE from an in-scope function signature or global variable (e.g.
    // CGRect/CGPoint: defined in a sibling out-of-scope header, but named by CGRectGetMinX's
    // param/return types). raw.sigs/enums/globals are already scope-filtered at extraction time
    // (LocOf in CClangExtract.cpp drops out-of-scope decls), so seeding from them cannot pull in
    // unrelated system structs - unlike records, which are collected regardless of scope.
    void PruneRecordsToNeededClosure(cflat_cinterop::ExtractResult& raw);

    void MapRawRecords(const cflat_cinterop::ExtractResult& raw, std::vector<CRecordEntry>& out);

    // A header-COM interface record is a struct whose lpVtbl field points at its <Iface>Vtbl.
    // Used to gate the (relatively costly) C++ uuid-harvest parse to headers that define COM.
    static bool HasComRecord(const std::vector<cflat_cinterop::RawRecord>& records);

    // Parse the same header(s) once more as C++ to read each COM interface's __declspec(uuid) GUID
    // (the MIDL_INTERFACE form), then stamp it onto the matching C-parse record by name. Failure is
    // non-fatal: records simply keep an empty uuid and iidof() falls back to its "no IID" error.
    void HarvestComUuids(const std::vector<std::string>& headerPaths, const std::string& primaryDir,
                         const std::vector<std::string>& extraDefines,
                         std::vector<cflat_cinterop::RawRecord>& records);

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
                             std::string* outPrereqMsg = nullptr);

    // Extract externally-linkable functions a .c file DEFINES, via the clang C++ API. Records
    // are registered up front (struct-by-value). Used by the .c auto-extern path.
    bool ExtractCFileClang(const std::string& cSourcePath,
                           std::vector<CSigEntry>& outSigs, std::vector<CRecordEntry>& outRecords,
                           std::vector<CGlobalEntry>& outGlobals);

    bool ExtractCSignatures(const std::string& cSourcePath, const std::string& programAlias = "");

    static std::string ConstIntValueSuffix(const std::string& typeName, long long value);

    void RegisterCEnums(const std::vector<CEnumEntry>& enums, const std::string& fileForLsp);

    void RegisterCGlobals(const std::vector<CGlobalEntry>& globals, const std::string& fileForLsp);

    void RegisterCRecords(const std::vector<CRecordEntry>& records, const std::string& fileForLsp);

    void RegisterCMacros(const std::vector<CMacroEntry>& macros);

    bool TranslateMacroBody(const CFunctionMacroEntry& m, std::string& out) const;

    void RegisterCFunctionMacros(const std::vector<CFunctionMacroEntry>& funcMacros,
                                 const std::string& fileForLsp);

    void ReportOrphanHeader(const std::vector<std::string>& headerPaths, const std::string& clangErr);

    // Single-header convenience wrapper - the common case (one `import "x.h";`).
    bool CompileCHeader(const std::string& headerPath, const std::vector<std::string>& extraDefines = {},
                        bool diskCache = false);

    bool CompileCHeaderGroup(const std::vector<std::string>& headerPaths,
                             const std::vector<std::string>& extraDefines = {},
                             bool diskCache = false);

    // Build a TargetMachine for the current target so the optimizer's PassBuilder
    // has TargetTransformInfo. Without a TM the loop vectorizer cannot cost vector
    // instructions and silently declines to vectorize (only memcpy/memset idioms
    // and SLP still fire). Mirrors the triple/CPU resolution in EmitExecutable.
    // Returns null on failure (the optimizer then runs target-agnostic, as before).
    std::unique_ptr<llvm::TargetMachine> CreateOptTargetMachine();

    // Native ELF code emission + link for Linux (and other ELF/Unix hosts).
    // Emits an x86-64 ELF object for the host triple and links it with the
    // system C compiler driver (cc/gcc/clang), which supplies crt1.o (_start ->
    // __libc_start_main -> main) and libc. cflat emits a C-ABI `main`, so no
    // custom entry point is needed. Windows/COFF/SEH/lld-link stay in
    // EmitExecutable; this path is the Stage-3 ELF target seam.
    bool EmitExecutableElf(const std::string& exePath, bool debugInfo,
                           const std::optional<std::string>& lliPath);

    // Cross-compile to a macOS arm64 Mach-O object (--platform macos). On a Mac
    // this would link via clang/ld64; on a non-Darwin host (the WSL cross-build)
    // no ld64 / macOS SDK is present, so this emits the relocatable Mach-O object
    // and stops, leaving <exePath>.o for a later link on a real Mac. The AArch64
    // backend must be registered in this LLVM build (apt llvm-18 on WSL has it;
    // the Windows vcpkg LLVM is X86-only, so this cleanly errors there).
    // Records a framework for the Mach-O link (`import framework "X"`). Dedups on
    // insert, preserving first-seen order. On a non-macOS target this is an error,
    // except in LSP analyze mode (symbolSink_ set) where it is recorded silently.
    bool AddFrameworkImport(const std::string& name);

    bool EmitExecutableMachO(const std::string& exePath, bool debugInfo,
                             const std::optional<std::string>& lliPath);

    bool EmitExecutable(const std::string& exePath, const std::string& platform, bool debugInfo = false,
                        const std::optional<std::string>& lliPath = std::nullopt);

    // BFS over static call graph to detect thread-spawn reachability from main (--run guard).
    // Indirect calls not followed; the spawn primitive is always reached via direct call from the wrapper.
    bool ReachesThreadSpawn();

    // --heap-audit instrumentation. Insert HeapAudit.enable() as main's first action and
    // HeapAudit.reportLeaks() immediately before every return (after the function's scope
    // destructors have run), so any program is audited for leaks/double-frees without source
    // edits. Report-only: reportLeaks prints still-live allocations to stderr but does not
    // alter the exit code; a double free is likewise printed (advisory) without aborting,
    // since a FREED-slot free can be a reuse artifact of a pre-enable() allocation.
    // enable() is idempotent, so a program that already audits itself just emits a duplicate
    // report. No-op when this module has no user main (e.g. a C-only or library compile).
    void InjectHeapAuditIntoMain();

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
    void EmitGlobalDestructorsInMain();

    bool JitRun(int& runExitCode);

    bool ResolveAsanRuntime(const std::string& arch, const std::string& msvcLibDir,
                            std::vector<std::string>& linkArgStrs, std::string& dllSrcOut);

    void CopyCRuntimeDlls(const std::string& exePath);

    Operation ParseOperation(const std::string& operationText);

public:
    LLVMBackend()
    {
        Init();
        // Annotations (including the WinRT/COM produce-side [winrt]/[uuid]) are declared in
        // source via `annotation X { ... }`; the registry is populated as those are scanned.
        CompilerManager::Instance().Register(this);
    }
    ~LLVMBackend();

    void DumpState() const;

    void InitDebugInfo(const std::string& filename, const std::string& directory);

    void FinalizeDebugInfo();

    void SetCurrentDebugLocation(size_t line, size_t col = 0);

    void ClearCurrentSubprogram();

    // Terminate all unterminated blocks in the module (lambdas mean outer function blocks may
    // also be unterminated), then pop stack frames to targetDepth without running destructors.
    void AbortFunctionBlocks(size_t targetDepth);

    struct BuilderState
    {
        llvm::IRBuilder<>::InsertPoint ip;
        llvm::Function* function = nullptr;
        llvm::DISubprogram* subprogram = nullptr;
        llvm::DebugLoc debugLoc;
        // Return-shape of the function being emitted. Emitting a nested function (e.g. a lambda
        // invoker) mid-body calls createFunctionBlock, which overwrites this trio with the nested
        // function's shape; snapshot it here so the enclosing function's return checks are restored.
        // All FOUR fields below are that shape - see the returnTV note.
        bool returnsOwned = false;
        bool returnIsArrayView = false;
        std::string returnTypeName;
        // Fourth member of the same return-shape group (set beside the trio in CreateFunction).
        // Leaving it unsaved let a nested emission's function<>/Lambda<> return type steer the
        // ENCLOSING function's `return` through CoerceToFuncPtrReturn.
        TypeAndValue returnTV;
        // The enclosing function's not-yet-flushed owned temps. A nested function emitted
        // mid-body (lambda invoker, global init, program shim) runs its OWN end-of-expression
        // flushes; those must not see - and drop - the outer function's temps, which are
        // registered in outer blocks and freed by the outer flush after we return here.
        std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> pendingStringTemps;
        std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> pendingClosureTemps;
        std::vector<PendingOwnedStructTemp> pendingStructTemps;
        std::vector<PendingOwnedPtrTemp> pendingPtrTemps;
        std::vector<OwnedReturnTemp> ownedReturnTemps;
        std::vector<OwnedReturnReleaseTemp> ownedReturnReleaseTemps;
        std::vector<OwnedNewTemp> ownedNewTemps;
        std::vector<std::pair<llvm::Value*, std::string>> valueElementTypeNames;
        std::vector<std::pair<llvm::Value*, std::string>> fatInterfaceValueTypeNames;
        std::vector<InterfaceBoxRecord> interfaceBoxRecords;
        std::vector<NullCoalesceJoin> nullCoalesceJoins;
        std::vector<JoinArmOccurrence> joinArmOccurrences;
        std::vector<llvm::Value*> codeValues;
        std::vector<llvm::Value*> dataValues;
        std::vector<std::pair<llvm::Value*, size_t>> codeValueDataCasts;
        std::vector<llvm::Value*> owningTempUniqueFields;
        std::vector<LaunderedTempUniqueField> launderedTempUniqueFields;
        std::vector<PendingLaunderTempUniqueField> pendingLaunderTempUniqueFields;
        std::vector<std::pair<llvm::Value*, size_t>> dataValueCodeCasts;
        // The enclosing statement's ambient occurrence id (see currentCastOccurrence_) - a nested
        // emission (lambda invoker, global init, program shim) starts its own from 0, same as the
        // per-function reset, and this restores the outer one when we return to it.
        size_t savedCastOccurrence = 0;
        std::vector<llvm::Value*> movedOutPtrValues;
        std::vector<std::pair<llvm::Value*, std::string>> movedBorrowedPtrValues;
        std::vector<llvm::Value*> movedBorrowedThroughFieldValues;
        std::vector<llvm::Value*> nonOwningStructJoins;
    };

    BuilderState SaveBuilderState();

    void RestoreBuilderState(const BuilderState& state);

    // True if `name` is a type projected from an imported .winmd (always fully qualified) or a
    // concrete instantiation of one.
    bool IsWinrtProjectedType(const std::string& name) const;

    // interfaceDefSites stores "canonicalPath(line,col)" - the full path is needed for identity
    // (two co-imported files sharing a basename must not compare equal), but an absolute,
    // machine-specific path is poor diagnostic output. A core def-site sits at a fixed offset
    // under the install (runtimeDir/core), so show it install-relative ("core/x.cb") - that is
    // stable across working directories, unlike cwd-relative which chains ".." from a deeply
    // nested cwd. A non-core (user) site is shown relative to the cwd when that stays inside the
    // tree; anything that would leak an absolute path or a cwd-dependent ".." chain (cross-drive
    // on Windows, or a canonicalize failure) falls back to the bare basename instead.
    // path::native() is a wstring on Windows; compare on the portable .string() form instead.
    static bool RelativePathEscapesUp(const std::filesystem::path& rel);

    // The path half of a "path(line,col)" def-site; empty if the site is not in that shape.
    static std::filesystem::path DefSitePath(const std::string& site);

    // The def-site path relative to the INSTALLED core tree (runtimeDir/core), or empty if it
    // does not live there.
    std::string InstalledCoreRelative(const std::filesystem::path& p) const;

    // The path tail below the last "core" directory component ("a/core/ui/x.cb" -> "ui/x.cb"),
    // or empty when no such component precedes the filename.
    static std::string TailBelowCoreDir(const std::filesystem::path& p);

    /*
     * A core .cb file exists twice on disk: the installed copy under runtimeDir/core that every
     * program implicitly imports, and the source-tree copy a maintainer edits. Compiling the
     * source copy directly - which the LSP does for every open file - registers the same logical
     * interface under two canonical paths, and the full-path identity key would call that a
     * redefinition. Treat the two as one file when exactly one side is the installed core copy
     * and the other names the same path below its own "core" directory. The residual hole is a
     * user file that sits at <anything>/core/<same relative name> and reuses a core interface
     * name; the far likelier collision - a user file anywhere else - still reports.
     */
    bool IsSameCoreFileDefSite(const std::string& siteA, const std::string& siteB) const;

    std::string ShortenDefSiteForDisplay(const std::string& site, bool isCore) const;

    void CreateInterfaceDefinition(const std::string& name, const std::vector<std::string>& parentNames,
                                   std::vector<InterfaceMethod> methods, std::vector<TypeAndValue> fields = {},
                                   const std::string& definitionSite = {});

    const std::vector<TypeAndValue>* GetInterfaceFields(const std::string& ifaceName) const;

    // Index of `fieldName` within the interface's field list, or -1. The vtable slot for it is
    // 1 + methodCount + index (see GetOrCreateVTable).
    int InterfaceFieldIndex(const std::string& ifaceName, const std::string& fieldName) const;

    size_t InterfaceFieldCount(const std::string& ifaceName) const;

    static bool IsPrimitiveTypeName(const std::string& name);

    void RegisterTypeAlias(const std::string& alias, const std::string& target);

    std::string ResolveTypeAlias(const std::string& name) const;

    // Look up a closure alias using the same innermost-namespace-first rules as named types.
    const TypeAndValue* FindFunctionTypeAlias(const std::string& name) const;

    // A pure-rename `using` alias is transparent the way a C typedef is, so it must not produce its
    // own monomorphization: list<MyInt> and list<int> are ONE instantiation. Registered ahead of
    // both passes (see PreRegisterRenameAliases) so the two mangle a type argument identically.
    void RegisterManglingAlias(const std::string& alias, const std::string& target);

    // Fold a type-arg base name through the pure-rename alias chain (`using MyInt2 = MyInt;` with
    // `using MyInt = int;` reaches "int"). The hop guard bounds a cycle; a cyclic alias is a
    // separate error the authoritative ParseUsingDeclaration owns.
    std::string ResolveManglingAlias(const std::string& name) const;

    // A `using` alias that names a GENERIC BASE rather than a concrete type - e.g.
    // `using IReference = Windows.Foundation.IReference;`, which then lets a body write the
    // familiar `IReference<int>`. Kept out of typeAliases: a base is not a usable type on its
    // own, and ResolveTypeAlias is consulted in places where expanding one would be wrong.
    void RegisterGenericBaseAlias(const std::string& alias, const std::string& target);

    bool IsGenericBaseAlias(const std::string& name) const;

    std::string ResolveGenericBaseAlias(const std::string& base) const;

    // Canonical interface lookup. Callers must not choose between a bare key and a qualified key
    // themselves: a namespace-local interface shadows a global interface of the same tail name.
    bool HasInterface(const std::string& name) const;
    const std::vector<InterfaceMethod>* FindInterface(const std::string& name) const;

    // True when `key` is a key some generic template kind is registered (or was scanned) under.
    // The single definition of "the generic template key space" - both the routing predicates and
    // the use-site base resolution below consult it, so they cannot drift apart.
    bool IsGenericTemplateKey(const std::string& key) const;

    /*
     * Resolve the SPELLED base of a generic instantiation to the key its template is registered
     * under. Templates declared in a namespace are keyed qualified ("NS.Box"), so both `NS.Box<int>`
     * and a bare `Box<int>` written inside `namespace NS` must land on that one key - otherwise the
     * mangled name never becomes a real type. The enclosing-namespace chain is walked INNERMOST
     * FIRST so a namespace-local template wins over a same-named global one; only a candidate that
     * actually names a template is accepted, so an unrelated sibling never hijacks the spelling.
     */
    std::string ResolveGenericTemplateBase(const std::string& base) const;

    /*
     * True when SOME generic TYPE template (struct / class / interface) by this spelling was seen
     * anywhere in this compile. The ACCEPT side of the opaque-shell gate in ScanGenericTypeUses,
     * and deliberately wider than IsGenericTemplateKey: it also accepts a bare spelling of a
     * namespace-qualified template (the `IV<int>` naming `NS.IV` that the namespaced-generic-
     * interface diagnostics are built on), a `using GB = Box;` base alias, an imported winmd
     * generic, and a template declared where the scan is not `certain` (an unfoldable `if const`
     * arm, an expect_error block) and so has no key at all. Accept-on-doubt: only a name with NO
     * evidence anywhere is refused, since the sole consequence of refusing is that the use falls
     * through to `unknown type`.
     * Generic FUNCTION templates are deliberately NOT consulted - a zero-argument `mk<int>()` call
     * parses as a type construction, and shelling it is exactly the bug this gate closes.
     */
    bool AnyGenericTypeTemplateNamed(const std::string& spelledBase) const;

    /*
     * True when `key` is a generic FUNCTION template that was declared DIRECTLY in namespace `ns`.
     * The declaring namespace is the one RECORDED at registration, never re-derived from the key:
     * "Outer.get" is equally the key of a free template in `namespace Outer` and of a method
     * template on `struct Outer`, and only the recorded value tells them apart.
     */
    bool IsGenericFunctionKeyInNamespace(const std::string& key, const std::string& ns) const;

    /*
     * Resolve the SPELLED base of a generic FUNCTION call to the key its template is registered
     * under - the function-side counterpart of ResolveGenericTemplateBase. A free generic function
     * declared in a namespace is keyed "NS.f", so a BARE call written inside that namespace has to
     * walk the enclosing-namespace chain (INNERMOST FIRST, so a namespace-local template wins over
     * a same-named global one) or it can never reach its own namespace's key.
     *
     * Deliberately NOT folded into IsGenericTemplateKey: that predicate also routes TYPE spellings
     * (an instantiation's base, a forward-scan shell), and a function key must never name a type.
     * A spelling that ALREADY contains a dot is root-anchored and returned untouched, matching the
     * type-argument rule - a qualified call site already spells its own key.
     */
    std::string ResolveGenericFunctionBase(const std::string& base) const;

    /*
     * True when `key` names a TYPE. The accept set for resolving a generic type ARGUMENT: only
     * type-shaped keys, so a same-named namespace sibling function or global never hijacks an
     * argument spelling (the mirror of IsGenericTemplateKey accepting only template keys).
     * All three legs are load-bearing and none is redundant: `scannedTypeNames` is the only one
     * populated while ScanGenericTypeUses runs and the only ORDER-INDEPENDENT one during the main
     * pass, but it is rebuilt per compile and so is empty for anything a WARM --init cache
     * restored; dataStructures and interfaceTable are the cached halves, and they are separate
     * registries - an interface has an interfaceTable entry, not a dataStructures one.
     */
    bool IsTypeArgTypeKey(const std::string& key) const;

    /*
     * Resolve the SPELLED bare name of a generic type ARGUMENT to the key its type is registered
     * under - the argument-side counterpart of ResolveGenericTemplateBase. Without it `Box<Item>`
     * written at global scope and inside `namespace A` (which declares its own `Item`) both mangle
     * to "Box__Item" and collapse onto ONE instantiation whose layout is decided by drain order.
     * Enclosing-namespace chain, INNERMOST FIRST, accepting only a candidate that names a type.
     * Callers must not apply it to an already-substituted argument.
     *
     * A spelling that ALREADY contains a dot is returned untouched, which is where this differs
     * from ResolveGenericTemplateBase. Ordinary type lookup resolves a dotted name from the top
     * (`ResolveQualifiedName` only walks outward for a name with NO dot), so relatively resolving
     * one here made a single spelling mean two types in one scope: `B.Item` as a plain local named
     * the top-level B.Item while `Box<B.Item>` named A.B.Item. For a template BASE there is no
     * second lookup to disagree with - the mangled name IS the identity - which is why the base
     * walk is deliberately left unguarded.
     */
    std::string ResolveTypeArgBaseName(const std::string& base) const;

    // True if `fullName` is a parameterized interface template from an imported .winmd.
    bool IsWinrtGenericTemplate(const std::string& fullName) const;

    // True if `fullName` is a PARAMETERIZED imported type - an interface template or a generic
    // delegate (AsyncOperationCompletedHandler`1). Both only become a type once <...> is supplied,
    // so both are aliasable as a generic BASE.
    bool IsWinrtGenericBase(const std::string& fullName) const;

    // The "uuid" type annotation for `name`, following the typedef-alias chain (ID3DBlob ->
    // ID3D10Blob) since a header-COM IID is registered on the struct tag, not its typedef name.
    std::string FindUuidAnnotationResolving(const std::string& name) const;

    // True when 'name' names a generic INTERFACE template and nothing else. A generic struct or
    // class template of the same name WINS - the two roles coexist under one mangled name
    // (Test/test_generics.cb emits both %Container__int and a Container__int vtable), and only the
    // struct role needs the mangled name to be a real struct type, so it takes it.
    bool IsGenericInterfaceTemplateName(const std::string& name) const;

    // A struct/class template named 'base' has appeared, so 'base__<args>' names a struct, not a
    // fat pointer. Drop any instance recorded under the interface role before it misroutes.
    void RevokeGenericInterfaceInstances(const std::string& base);

    bool IsInterfaceType(const std::string& name) const;

    bool HasInterfaceMethod(const std::string& ifaceName, const std::string& methodName) const;

    /*
     * The name-matching slot whose arity is argCount, or the first name match when no slot has
     * that arity (or argCount is npos). Name alone is not a key - an overloaded contract has
     * several slots under one name, and picking the first is what made dispatch miscompile.
     */
    const InterfaceMethod* FindInterfaceMethod(const std::string& ifaceName,
                                               const std::string& methodName,
                                               size_t argCount = (size_t)-1) const;

    // The declared parameters of an interface method (implicit 'this' excluded), or nullptr.
    // Lets the call site type a lambda-literal argument against the declared signature.
    const std::vector<TypeAndValue>* GetInterfaceMethodParams(const std::string& ifaceName,
                                                             const std::string& methodName,
                                                             size_t argCount = (size_t)-1) const;

    // The declared return type of an interface method, or nullptr. Lets a `?.` call site
    // build the null-path default value's type without invoking the method.
    // Name-only first match, unlike the signature-aware slot selection in
    // ResolveInterfaceMethodSlot: currently unused, so give it FindInterfaceMethod's
    // arity argument before wiring up any caller that can see an overload set.
    const TypeAndValue* GetInterfaceMethodReturnType(const std::string& ifaceName,
                                                    const std::string& methodName) const;

    llvm::StructType* GetFatPtrType() const;

    // {i8* fnptr, i8* envptr} - storage type for all in-function function<T> variables.
    llvm::StructType* GetClosureFatPtrType() const;

    // Creates __shim_<name>(original params..., i8* env) that ignores env and calls original.
    // Env-last ABI makes this bitcast-compatible with a bare C function pointer.
    llvm::Function* GetOrCreateFunctionShim(llvm::Function* original);

    // Wraps a bare C function pointer as a closure {thunk, env=cfnptr}. The env slot carries
    // the real C fn ptr; the thunk reads env back, bitcasts to C signature, and tail-calls through it.
    llvm::Function* GetOrCreateCFuncPtrThunk(llvm::FunctionType* cFnTy);

    llvm::Value* WrapCFuncPtrAsFatStruct(llvm::Value* cFnPtrValue, const TypeAndValue& fpTV);

    // Wraps a named function in a {shim_i8ptr, null} closure fat struct value.
    // Thin sibling of WrapBareValueAsFatStruct: a thin `function<T>` value is just the
    // bare function pointer, bitcast to the declared thin signature R(*)(Args). No shim,
    // no env, no fat struct. `fpTV` carries the target thin signature.
    llvm::Value* MakeThinFnPtrValue(llvm::Value* fn, const TypeAndValue& fpTV);

    // Extract the bare code pointer from a closure fat-struct value and present it as a thin
    // `function<T>`. Caller must ensure the closure does NOT capture (env null). Relies on the
    // trailing-env-ignored ABI tolerance (caller-cleanup conventions: Win x64, cdecl).
    llvm::Value* CoerceClosureFatToThin(llvm::Value* fatVal, const TypeAndValue& thinTV);

    // True iff a fat closure value is PROVABLY non-capturing: its env field is a compile-time
    // null constant (a non-capturing lambda literal or a named-fn wrap). A capturing lambda or a
    // stored/loaded Lambda<> value is not provable here and returns false. This is the static gate
    // for narrowing a fat closure to a thin `function<T>` - the same env-null signal the extern
    // function-pointer argument path uses.
    bool ClosureIsStaticallyNonCapturing(llvm::Value* fatVal);

    // Diagnostic for an illegal fat-closure -> thin `function<T>` narrowing. With capture names
    // (a capturing lambda literal) it lists them; with none (a stored Lambda<> value) it points at
    // .toFunction(). Shared by the declaration, assignment, and return coercion sites.
    std::string DescribeCapturingClosureToThin(const std::vector<std::string>& captureNames) const;

    // Lambda<T>.toFunction(): lower a fat closure value to a thin `function<T>` C pointer.
    // env == null (non-capturing) -> the bare code ptr; env != null (captures) -> a null thin
    // pointer. The env==null test is the entire contract: a capturing closure has no C-ABI
    // representation, so the lowering fails closed and the caller must null-check. No trap, no
    // diagnostic. A non-capturing closure's code ignores the trailing env arg under the
    // caller-cleanup ABI tolerance (Win x64, cdecl), so the returned bare ptr is C-callable.
    llvm::Value* EmitFuncToFunctionLowering(llvm::Value* fatVal, const TypeAndValue& thinTV);

    // Coerce a returned value to the declared function-pointer return type. Handles a named
    // function, a thin function<> value, and a fat closure value in either direction. `returnNV`
    // is the NamedVariable the return expression resolved to - the same shape of evidence the
    // argument-passing gates read - so the return site can share ArgumentIsProvablyDataPointer.
    llvm::Value* CoerceToFuncPtrReturn(llvm::Value* val, const TypeAndValue& retTV,
        const NamedVariable& returnNV);

    // The RETURN-flavoured provenance gate, sharing ArgumentIsProvablyDataPointer with the
    // argument-passing gates so the accept set cannot drift between "pass" and "return". `thin`
    // selects the 'function<>' wording vs the closure wording; reject only what is PROVEN data.
    void CheckClosureReturnProvenance(llvm::Value* val, const NamedVariable& returnNV, bool thin) const;

    // Widen a thin `function<T>` (bare C ptr) to a fat `Lambda<T>` value {code, null}. No thunk:
    // the thin ptr goes straight into the code slot, env is null. When the resulting Lambda is
    // invoked (env-last ABI), the trailing null env arg is harmlessly ignored by the bare C
    // function (caller-cleanup tolerance). The null env makes .toFunction() round-trip it back.
    llvm::Value* WidenThinToFat(llvm::Value* thinPtr);

    llvm::Value* WrapBareValueAsFatStruct(llvm::Function* original);

    bool InterfaceInheritsFrom(const std::string& child, const std::string& parent) const;

    bool StructImplementsInterface(const std::string& structName, const std::string& ifaceName) const;

    /*
     * Shape of a source that provably cannot be boxed into an interface fat pointer. A `T**`
     * (ElemPointer), a `T[]` view, a fixed `T[N]` slot and a simd lane vector all carry the
     * ELEMENT class as their TypeName while loading to a bare ptr, so the ordinary upcast would
     * attach that class's vtable to storage that is not an instance of it - silent type
     * confusion. Returns an empty string for every shape that stays on its normal path (a thin
     * `T*`, a `unique T*`, a borrowed class pointer, a by-value class, an already-fat interface).
     */
    std::string DescribePointerShapedInterfaceSource(const TypeAndValue& src) const;

    // Message for a rejected pointer-shaped class -> interface upcast. Shared by every site that
    // boxes into an interface so all spellings report the same diagnostic.
    std::string FormatPointerShapedInterfaceUpcastError(const std::string& shape,
                                                        const std::string& typeName,
                                                        const std::string& interfaceName) const;

    // Appends one vtable slot per interface field: the implementor's BYTE OFFSET of that field,
    // encoded as inttoptr(offset). Reads/writes through the interface GEP by it, so no thunk is
    // needed and the field stays an lvalue. A missing/mismatched field was already reported by
    // VerifyInterfaceFields at the implementor's definition; emit a null slot and keep going.
    void AppendInterfaceFieldOffsetSlots(const std::string& structName, const std::string& ifaceName,
                                         llvm::StructType* structTy,
                                         const std::vector<DeclTypeAndValue>& structFields,
                                         std::vector<llvm::Constant*>& entries);

    // The declared type of an interface field / implementor field, for diagnostics.
    static std::string InterfaceFieldTypeText(const TypeAndValue& f);

    // An interface method's parameter/return as declared, ownership qualifiers included, for
    // diagnostics - so a message can quote the exact spelling the fix needs.
    static std::string InterfaceMethodTypeText(const TypeAndValue& tv, const std::string& name = "");

    // Every field an interface declares must exist on the implementor with the same name and
    // type - the vtable slot carries that field's byte offset. Runs EAGERLY, at the class or
    // program definition, so the error points at the implementor and not at some later boxing
    // site (AppendInterfaceFieldOffsetSlots then trusts the layout it is handed).
    void VerifyInterfaceFields(const std::string& implName, const std::string& ifaceName,
                               const std::vector<DeclTypeAndValue>& implFields);

    /*
     * An interface method's ownership qualifiers are contract, not local sugar. Virtual dispatch
     * reads them from the INTERFACE - CallInterfaceMethod feeds the interface's Parameters to
     * ApplyMoveParamTransfer and classifies the result from the interface's ReturnType - so the
     * implementor's own spelling is never consulted at the call site. Both directions of
     * disagreement are heap bugs, so both are rejected; the same rule VerifyInterfaceFields
     * imposes on fields.
     *
     * Runs on the symbol overload resolution ALREADY selected (on TypeName/Pointer), so a
     * violation names the real mismatch instead of claiming the method is missing. 'Pointer' is
     * deliberately absent: it is a selection key in VerifyInterfaceImplementation's match loop,
     * so a pointer mismatch is already reported there as "does not implement".
     */
    /*
     * Non-diagnostic mirror of VerifyInterfaceMethodContract's ownership comparisons: true when
     * the class overload's qualifiers agree with the interface method on every axis the contract
     * check enforces (sink-ness, bond, alias - params and return). Lets the caller pick, from all
     * same-name same-signature overloads, any that conforms before deciding to report a mismatch.
     */
    bool InterfaceMethodContractConforms(const InterfaceMethod& method, const FunctionSymbol& sym) const;

    void VerifyInterfaceMethodContract(const std::string& implName, const std::string& ifaceName,
                                       const InterfaceMethod& method, const FunctionSymbol& sym);

    // The function a vtable slot calls for one concrete implementor. Shared by both vtable
    // builders and the interface-dispatch ownership walks so they can never disagree.
    llvm::Function* LookupInterfaceMethodImpl(const std::string& structName,
                                              const InterfaceMethod& method) const;

    // Every registered type that may provide this interface. False = the set is untrustworthy
    // (mid-import, uncertain template, or empty), which callers must read as "no proof".
    bool EnumerateInterfaceImplementors(const std::string& ifaceName,
                                        std::vector<std::string>& out) const;

    llvm::GlobalVariable* GetOrCreateProgramVTable(ProgramData& pd, const std::string& structName, const std::string& ifaceName);

    llvm::GlobalVariable* GetOrCreateVTable(const std::string& structName, const std::string& ifaceName);

    llvm::Value* BuildInterfaceFatValue(llvm::GlobalVariable* vtable, llvm::Value* dataPtr);

    // Coerce ONE call argument to an INTERFACE-typed parameter: box a concrete class/struct
    // (value or pointer) into a fat pointer, or upcast an already-boxed interface value.
    // Shared by the indirect (function<>/Lambda) call path so it cannot drift from the
    // direct-call path's inline boxing. Returns `val` unchanged when nothing applies.
    llvm::Value* CoerceArgToInterface(const NamedVariable& arg, llvm::Value* val,
                                      const std::string& ifaceName, const std::string& calleeDesc);

    // Record a class's base-clause interface names from the ForwardRefScanner pass, so the static
    // conversion check below sees the whole file instead of only textually earlier declarations.
    void RecordScannedStructInterfaces(const std::string& structName, std::vector<std::string> ifaceNames);

    // Mark an interface whose implementor set cannot be settled before codegen (a generic class
    // names it, or it is itself spelled generically), so the check never proves anything about it.
    void RecordUncertainInterfaceImpl(const std::string& ifaceName);

    // Remember a class the scanner found inside an `if const` arm, with the full chain of arms it
    // sits under. Read by the zero-implementor diagnostic only - it must never gate acceptance.
    void RecordIfConstGuardedInterfaceImpl(const std::string& className,
                                           std::vector<std::string> guardChain, const void* node,
                                           std::vector<std::vector<std::string>> ifaceCandidates);

    // Withdraw a class's entry because MainListener took the arm it sits in - it is live after all.
    void RetractIfConstGuardedInterfaceImpl(const void* node);

    // MainListener took an OUTER arm the class sits under, but the class is behind a further nested
    // `if const` that gets its own decision. Drop just that outer level, so whatever remains at the
    // front of the chain is still an arm nobody has been shown to take.
    void PeelIfConstGuardedInterfaceImpl(const void* node);

    /*
     * The ONE name a scanned base-clause spelling actually denotes, out of the candidates the
     * scanner could not choose between. Candidates arrive in the resolver's priority order (exact
     * spelling first, then the enclosing namespaces innermost-out, then a bare last component), so
     * the first that names a registered interface is the one the class would have registered
     * against. Empty when the entry names no interface at all - a base class, or an unknown name.
     * Called only from the finalization diagnostic, by which point every interface is registered.
     */
    std::string ResolveGuardedBaseCandidate(const std::vector<std::string>& candidates) const;

    // Look for classes that would implement `ifaceName` but sit inside an `if const` arm that this
    // build did not take. Returns the first such class plus how many distinct ones there are.
    bool FindIfConstGuardedImplementor(const std::string& ifaceName, std::string& classOut,
                                       std::vector<std::string>& guardChainOut,
                                       size_t& countOut) const;

    // RAII bracket for importCompileDepth_. LogError THROWS on several paths (SpeculativeEvalAbort,
    // CompilerAbortException, ExpectedErrorReceived) and FailCompilation throws in batch mode, so a
    // bare decrement would leak the depth and silently disable the check for the rest of the process.
    struct ImportedFileCompileScope
    {
        LLVMBackend* backend_;
        explicit ImportedFileCompileScope(LLVMBackend* backend) : backend_(backend)
        {
            backend_->importCompileDepth_++;
        }
        ~ImportedFileCompileScope()
        {
            if (backend_->importCompileDepth_ > 0) backend_->importCompileDepth_--;
        }
        ImportedFileCompileScope(const ImportedFileCompileScope&) = delete;
        ImportedFileCompileScope& operator=(const ImportedFileCompileScope&) = delete;
    };

    // True when nothing may be PROVEN about who implements `ifaceName`: a generic class could
    // still supply it, or later files simply have not been scanned yet (import-time codegen).
    bool InterfaceImplementorSetIsUncertain(const std::string& ifaceName) const;

    // Whether `typeName` may provide `ifaceName`, directly or by interface inheritance. Answers
    // from the codegen registry AND the scanner's forward registry, so it is declaration-order
    // independent - unlike RebuildInterfaceFatValue's case loop, which additionally needs a
    // typedesc global and so can only see classes already emitted.
    bool TypeMayProvideInterface(const std::string& typeName, const std::string& ifaceName) const;

    // Shared by the static check and RebuildInterfaceFatValue's zero-case backstop, so the two
    // can never disagree about whether an implementor exists.
    bool AnyTypeMayProvideInterface(const std::string& ifaceName) const;

    // True only when an interface -> interface conversion provably cannot succeed: both names are
    // real interfaces, neither inherits the other, neither implementor set is uncertain, and every
    // known implementor of `srcIface` lacks `dstIface`. Any doubt returns false: the check may only
    // reject what it can PROVE impossible, so a legal upcast is never rejected.
    bool InterfaceConversionIsProvablyImpossible(const std::string& srcIfaceIn,
                                                 const std::string& dstIfaceIn) const;

    // RECORD that a value of `name` was materialised or converted here. Cheap, and it CANNOT
    // reject - so a materialisation site this misses degrades to "no diagnostic", never to a false
    // rejection or a false cause. The decision is deferred to ResolveMaterializedInterfaceUses.
    //
    // Why the decision cannot be made here: "in genericInterfaceInstances but not yet in
    // interfaceTable" is a LEGITIMATE TRANSIENT state, not the bug. CreateStructType's field loop
    // runs during monomorphization, BEFORE ProcessPendingInstantiations drains the interface
    // instantiation queued by that very declaration, and GetType documents the same thing ("lowers
    // to a fat pointer even before its interfaceTable entry exists"). Rejecting at the site cannot
    // tell "never instantiated" from "not instantiated yet", which is exactly how an earlier
    // version of this check came to assert a false `if const` cause on programs containing no
    // `if const` at all.
    void RecordInterfaceMaterialization(const std::string& nameIn, const std::string& role);

    // Resolve the recorded materialisations, once, at a point where interfaceTable is COMPLETE.
    // A recorded name still absent from it here was never instantiated at all, so the value is a
    // fat pointer with no method table: a method call on it is already a clean error, but it can
    // still be assigned between two unrelated real interfaces and launder one's vtable into the
    // other, and `is` on it dereferences a null type descriptor.
    //
    // EVERY offender is aggregated into ONE diagnostic, because LogError does not return: on the CLI
    // path it reaches FailCompilation -> exit(1) (running no destructors), and on the batch/LSP path
    // it throws. A loop that called LogError per offender would therefore report exactly one and
    // silently drop the rest - which is what an earlier version of this did while claiming otherwise.
    //
    // The message states only what is KNOWN. The `if const` cause is appended ONLY for a name the
    // forward scan recorded as declared under a condition it could not fold.
    void ResolveMaterializedInterfaceUses();

    // "Container__int" -> "Container". The bare template name a mangled instantiation came from,
    // used only to look up the if-const hint.
    static std::string TemplateBaseOfMangledName(const std::string& mangled);

    /*
     * True when a "__" segment of an instantiation name is itself a TEMPLATE name, which makes the
     * flat rendering below AMBIGUOUS: the separator is the same for a nested argument and for a
     * sibling one, so "Box__Box__i32" (Box<Box<int>>) and a hypothetical two-argument Box read
     * identically. Deliberately over-broad - it also scans for a namespace-qualified key whose tail
     * matches, since the declaring namespace is not knowable here - because a false "ambiguous"
     * only costs the pretty rendering, while a false "unambiguous" prints a type that does not
     * exist. IsGenericTemplateKey is the one definition of the template key space.
     */
    bool MangledGenericNameIsAmbiguous(const std::string& mangled) const;

    /*
     * "Box__i32" -> "Box<i32>", "dictionary__string__int" -> "dictionary<string, int>". DIAGNOSTICS
     * ONLY - never a key. A message that quotes the raw mangled name gives advice the user cannot
     * write, so the rendered form is used when - and ONLY when - it is provably writable source
     * that binds the same instantiation ('Box<i32>*' and 'Box<int>*' mangle alike).
     *
     * A NESTED instantiation cannot be rendered from the string: "Box__Box__i32" would come out
     * "Box<Box, i32>", which is not what the user wrote and does not name any type. Those return
     * the RAW mangled name with *writable = false, and every caller must then DROP its "declare the
     * parameter as 'T*'" advice rather than hand back a spelling that will not compile. A name with
     * no "__" comes back unchanged and is always writable.
     */
    std::string DisplayNameOfMangledType(const std::string& mangled, bool* writable = nullptr) const;

    // Rebuild only when the source and destination interfaces actually differ (the common
    // same-interface case stays a plain by-value copy, with no if-chain emitted). The ambiguous
    // sentinel (a '?:' join whose two arms disagreed - see PropagateFatInterfaceJoin) always
    // rebuilds: neither arm's name can be trusted, but the runtime typedesc match can.
    llvm::Value* ReboxInterfaceIfNeeded(llvm::Value* fatVal, const std::string& srcIface,
                                        const std::string& dstIface);

    // Re-box an interface value as a DIFFERENT interface (a derived-to-parent upcast, e.g.
    // IButton -> IElement). A derived vtable is NOT layout-compatible with its parent's once
    // field-offset slots exist, so the fat pointer is rebuilt by matching the runtime typedesc
    // against each implementor of dstIface. That if-chain CANNOT be emitted here: the
    // implementor registry is only as complete as the walk has got, so a class declared later
    // in the file, monomorphized later, or defined in the file that imports this core library
    // would be missing and the conversion would silently yield a null vtable. Emit a call to a
    // per-destination thunk instead and fill its body at finalization.
    llvm::Value* RebuildInterfaceFatValue(llvm::Value* fatVal, const std::string& dstIface);

    // Get-or-create the empty `__iface_rebox.<dstIface>` thunk. One thunk per destination
    // interface: the lowering depends only on dstIface, so every site can share it.
    llvm::Function* GetOrCreateInterfaceReboxThunk(const std::string& dstIface);

    // Exception-safe bracket around SaveBuilderState/RestoreBuilderState. Needed wherever a
    // nested emission happens MID-BODY: it also parks the enclosing function's return contract,
    // its pending owned temps, and the suspended-function marker FunctionBodyIsComplete reads.
    struct FullBuilderStateScope
    {
        LLVMBackend* backend_;
        BuilderState state_;
        explicit FullBuilderStateScope(LLVMBackend* backend)
            : backend_(backend), state_(backend->SaveBuilderState()) {}
        ~FullBuilderStateScope() { backend_->RestoreBuilderState(state_); }
        FullBuilderStateScope(const FullBuilderStateScope&) = delete;
        FullBuilderStateScope& operator=(const FullBuilderStateScope&) = delete;
    };

    // RAII park of the member builder state around the finalization pass. GetOrCreateVTable can
    // LogError and LogError THROWS, so a plain save/restore pair would leave the builder pointing
    // into a half-built thunk (the hazard ImportedFileCompileScope exists to prevent). The
    // recorded-location fields are parked too: the diagnostic overwrites them to report.
    struct InterfaceReboxEmitScope
    {
        LLVMBackend* backend_;
        llvm::IRBuilderBase::InsertPoint ip_;
        llvm::Function* function_;
        llvm::DISubprogram* subprogram_;
        llvm::DebugLoc debugLoc_;
        std::string file_;
        size_t line_;
        size_t column_;
        explicit InterfaceReboxEmitScope(LLVMBackend* backend)
            : backend_(backend), ip_(backend->builder->saveIP()),
              function_(backend->currentFunction), subprogram_(backend->currentSubprogram),
              debugLoc_(backend->builder->getCurrentDebugLocation()),
              file_(backend->sourceFileName), line_(backend->currentLine),
              column_(backend->currentColumn)
        {
            // No DISubprogram covers a thunk, so any inherited debug location would name a
            // scope from another function and fail the verifier.
            backend_->currentSubprogram = nullptr;
            backend_->builder->SetCurrentDebugLocation(llvm::DebugLoc());
        }
        ~InterfaceReboxEmitScope()
        {
            backend_->currentFunction = function_;
            backend_->currentSubprogram = subprogram_;
            backend_->builder->restoreIP(ip_);
            backend_->builder->SetCurrentDebugLocation(debugLoc_);
            backend_->sourceFileName = file_;
            backend_->currentLine = line_;
            backend_->currentColumn = column_;
        }
        InterfaceReboxEmitScope(const InterfaceReboxEmitScope&) = delete;
        InterfaceReboxEmitScope& operator=(const InterfaceReboxEmitScope&) = delete;
    };

    // Re-adopt bodyless rebox thunks found in a module restored from the core bitcode cache.
    // The cache is built from a runtime.cb-only compile that never finalizes, so a thunk it
    // handed out would otherwise reach the verifier with no body.
    void AdoptInterfaceReboxThunksFromModule();

    // Emit the typedesc if-chain for every rebox thunk handed out during the walk. Runs once at
    // finalization, when every struct body, destructor, vtable and generic monomorphization
    // exists, so the chain sees the COMPLETE implementor set regardless of declaration order.
    void EmitDeferredInterfaceReboxBodies();

    // Body of one rebox thunk: match the source fat pointer's runtime typedesc against every
    // implementor of the destination interface and rebuild the pair from that implementor's
    // destination vtable. No match yields a zeroed fat pointer, like a failed `as <Interface>`.
    // Taken BY VALUE: emitting a body can append to deferredIfaceRebox_ and reallocate it.
    void EmitInterfaceReboxBody(DeferredInterfaceRebox site);

    // With no case to match, every call of the thunk returns a null vtable that the next method
    // call dispatches through. An empty case list is NOT by itself proof of that: a generic
    // implementor that was never monomorphized never reaches dataStructures, and a site inside
    // an import cannot see the importing file at all. Report only when nothing is left open -
    // a missed error is acceptable, a falsely rejected program is not.
    void ReportInterfaceReboxHasNoImplementor(const DeferredInterfaceRebox& site, bool noCases);

    // ===== WinRT / COM produce-side codegen (see internal/plan/winmd-projection.md) =====
    // A [winrt] class lowers to a thin COM object: a struct whose first field is a vtable
    // pointer, followed by a refcount and the user fields. The vtable holds IUnknown +
    // IInspectable slots plus the interface methods (raw 1:1 ABI for this milestone). All
    // runtime functions (QueryInterface/AddRef/Release/thunks) are generated here.

    bool IsWinrtClass(const std::string& name) const;

    // Parse a canonical GUID string into its 16-byte little-endian memory image (matching the
    // in-memory layout of core/guid.cb's Guid: u32 Data1, u16 Data2, u16 Data3, u8[8] Data4).
    // Non-hex characters (dashes, braces) are skipped. Returns false if fewer than 32 nibbles.
    static bool ParseUuidToBytes(const std::string& text, uint8_t out[16]);

    // Resolve `name` (a mangled generic instance like "Windows.Foundation.IReference__i32", a
    // [uuid] interface, or an imported non-generic interface FULL name) to a 16-byte GUID/PIID for
    // `iidof(...)` builtin. Returns nullptr if no IID is known. The result is a REFIID-shaped
    // pointer suitable for QueryInterface.
    llvm::GlobalVariable* EmitIidGlobalFor(const std::string& name);

    // Emit (or reuse) an internal-linkage [16 x i8] constant holding a GUID's memory image.
    // Deduplicated by content so IID_IUnknown/IID_IInspectable are shared across classes.
    llvm::GlobalVariable* EmitGuidGlobal(const uint8_t bytes[16]);

    // Build a single vtable slot: a thin `function<Ret(params)>` field with the given name.
    static DeclTypeAndValue MakeWinrtSlot(const std::string& name, const std::string& retType,
        bool retPtr, std::vector<TypeAndValue::FuncPtrParam> params);

    // Create the COM vtable struct for className implementing ifaceName. Flat IInspectable
    // layout: [QueryInterface, AddRef, Release, GetIids, GetRuntimeClassName, GetTrustLevel,
    // <interface methods in slot order>]. Returns the registered struct name.
    std::string CreateWinrtVtableStruct(const std::string& className, const std::string& ifaceName);

    // Find the user member function implementing interface method m on className. Mirrors the
    // overload match in GetOrCreateVTable (this-pointer + remaining params). Returns the full
    // symbol so the HRESULT-ABI thunk can see the impl's return type (plain T vs HResult<T>).
    // The impl's return type is NOT part of the match (an impl may return T or HResult<T> for
    // the same logical interface method).
    const FunctionSymbol* FindWinrtMethod(const std::string& className, const InterfaceMethod& m);

    // True if a type name is a monomorphized HResult<T> (the fallible method return form).
    static bool IsHResultType(const std::string& typeName);

    // Store &g_vtbl into lpVtbl (field 0) and 1 into __refcount (field 1) of a freshly
    // allocated [winrt] object. Called by `new` after the constructor runs.
    void WireWinrtObject(llvm::Value* objPtr, const std::string& className);

    // If `slotName` names a vtable slot of the [winrt] class `className` (a generated
    // QueryInterface/AddRef/Release/IInspectable method or an interface method), return its
    // field descriptor; else nullptr. Lets `recv->Slot(args)` be recognized as a COM call.
    const DeclTypeAndValue* GetWinrtSlot(const std::string& className, const std::string& slotName) const;

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
        const std::vector<llvm::Value*>& argVals, std::string& outResultType, bool& outResultPtr);

    // Emit all runtime functions and the static vtable instance for a [winrt] class. Must run
    // AFTER the object struct body, the vtable struct, and the user member functions exist.
    void EmitWinrtRuntime(const std::string& className, const std::string& ifaceName,
        const std::string& vtblName);

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
    static std::string WinrtSimpleName(const std::string& fullName);

    // Map a WinRT signature type to a (CFlat type name, pointer) pair for a vtable slot. Scalars
    // map precisely; String(HSTRING)/Object(IInspectable*)/interfaces/classes/arrays/generics/
    // by-ref all degrade to an opaque void* - ABI-correct under LLVM opaque pointers and exactly
    // what the hand-written COM demo uses. An imported enum collapses to its underlying integer;
    // an imported value struct we have registered passes by value.
    void MapWinrtTypeForSlot(const cflat_winmd::TypeRef& t, std::string& outName, bool& outPtr);

    // Build the COM vtable struct (`<thinName>Vtbl`, flat IInspectable layout) and the thin
    // pointer struct (`<thinName>` with a single `lpVtbl` field) for a non-generic interface or a
    // concrete generic instantiation, from already-resolved (substituted) method signatures.
    // Returns true if it registered, false if a struct of that name already exists.
    // True if `fullName`'s transitive `requires_` chain reaches an interface simple-named `target`
    // (e.g. "IUnknown" / "IInspectable"). `seen` guards against cycles. Used to tell classic-COM
    // (IUnknown-rooted) interfaces from WinRT (IInspectable) ones during consume registration.
    static bool WinrtRequiresReaches(const std::string& fullName,
        const std::unordered_map<std::string, const cflat_winmd::Interface*>& byName,
        const char* target, std::set<std::string>& seen);

    // Flatten a classic-COM single-inheritance method list into vtable-slot order: base-most
    // interface methods first, then this interface's own. IUnknown/IInspectable contribute no
    // methods (they are the synthetic header). `seen` guards against diamond duplication.
    static void CollectComBaseMethods(const cflat_winmd::Interface& iface,
        const std::unordered_map<std::string, const cflat_winmd::Interface*>& byName,
        std::set<std::string>& seen, std::vector<cflat_winmd::Method>& out);

    // `inspectable` selects the synthetic header: WinRT interfaces derive from IInspectable
    // (6 slots: IUnknown's 3 + GetIids/GetRuntimeClassName/GetTrustLevel); classic-COM interfaces
    // (IUnknown-rooted, e.g. Direct2D from Win32 metadata) use the 3-slot IUnknown header. Pass the
    // method list already flattened across the COM single-inheritance chain in vtable-slot order.
    bool BuildWinrtInterfaceStructs(const std::string& thinName,
        const std::vector<cflat_winmd::Method>& methods, const std::string& lspDesc,
        const std::string& fileForLsp, bool inspectable = true);

    // True if `name` is a thin COM interface pointer struct built from imported winmd (a
    // non-generic interface or a concrete generic instantiation like "IVector__int").
    bool IsWinrtThinInterface(const std::string& name) const;

    // Emit a raw COM dispatch `obj->lpVtbl->slot(obj, extraArgs...)` on a consume-side thin WinRT
    // interface pointer (built by BuildWinrtInterfaceStructs). Returns the i32 HRESULT. `extraArgs`
    // are passed verbatim after the receiver; out-params should already be void* (i8*).
    llvm::Value* EmitWinrtThinSlotCall(llvm::Value* objPtr, const std::string& thinName,
        const std::string& slotName, const std::vector<llvm::Value*>& extraArgs);

    // Substitute generic VAR placeholders in a template TypeRef with the concrete argument
    // TypeRefs (by VAR index), preserving any pointer/array decoration on the placeholder and
    // recursing into nested generic instantiations.
    cflat_winmd::TypeRef SubstWinrtVar(const cflat_winmd::TypeRef& t,
        const std::vector<cflat_winmd::TypeRef>& args);

    // Map a CFlat type-argument spelling (as written in `IVector<int>`) to the WinRT TypeRef used
    // for signature/PIID derivation. Only the unambiguous scalar set is accepted, plus named
    // imported winmd types; anything else is rejected rather than silently deriving a wrong PIID.
    bool CFlatArgToWinrtTypeRef(const std::string& cflatName, cflat_winmd::TypeRef& out, std::string& err);

    // True if `fullName` names an imported winmd type (any kind).
    bool IsWinrtFullName(const std::string& fullName) const;

    // Instantiate an imported generic WinRT interface (`base` = the FULL name, e.g.
    // "Windows.Foundation.Collections.IVector", `cflatArgs` = the concrete CFlat type arguments,
    // `mangledName` = base + "__" + args) into a concrete COM vtable + thin pointer struct and a
    // derived PIID. Returns false if `base` is not a registered winmd generic template (so the
    // caller can fall through to other resolution).
    bool InstantiateWinrtGenericInterface(const std::string& base,
        const std::vector<std::string>& cflatArgs, const std::string& mangledName);

    // Find an imported WinRT delegate template by its fully-qualified name.
    const cflat_winmd::Delegate* FindWinrtDelegate(const std::string& fullName) const;

    // The LLVM types for each Invoke parameter, mapped through the thin-slot ABI (interfaces /
    // objects / strings -> i8*, scalars/enums -> their scalar). This is exactly the surface a
    // consume-side handler sees, so the cflat closure's parameter list mirrors it 1:1.
    std::vector<llvm::Type*> WinrtDelegateInvokeParamTypes(const cflat_winmd::Method& invoke);

    // Build (once) the COM object type + static vtable {QI,AddRef,Release,Invoke} for a projected
    // delegate instantiation `mangled`. The object is { vtbl*, i32 refcount, __closure_fat_ptr };
    // Invoke forwards the WinRT ABI args to the stored closure (env-last), returning S_OK. QI
    // answers IUnknown, IAgileObject, and the delegate's own IID/PIID (`iidBytes`).
    void BuildWinrtDelegateType(const std::string& mangled, const cflat_winmd::Method& invoke,
        const uint8_t iidBytes[16]);

    // Lower winrtDelegate(DelegateType, closure): synthesize (or reuse) the COM-callable object
    // type for the delegate instantiation, then at the call site allocate it, wire lpVtbl and a
    // refcount of 1, clone the closure into it (clone-by-default; the object owns the clone and
    // destructs it on final Release), and return the object as an i8* the WinRT ABI accepts.
    llvm::Value* EmitWinrtDelegateObject(const std::string& base,
        const std::vector<std::string>& cflatArgs, llvm::Value* closureFat);

    // Register every projectable type in `model` as CFlat types: value structs, enums (named
    // constants + underlying), and interfaces (COM vtable struct + thin pointer struct). Runtime
    // classes, delegates, generics, and HSTRING/string ergonomics are deferred (counted + noted).
    //
    // TYPES are registered ONLY under their fully-qualified WinRT name (a `.winmd`'s IButton is
    // "Microsoft.UI.Xaml.Controls.IButton", never a bare "IButton"), so a projection can never
    // displace a CFlat interface/struct of the same short name. Source spells them qualified, or
    // aliases them (`using IButton = Microsoft.UI.Xaml.Controls.IButton;`).
    void RegisterWinrtModel(const cflat_winmd::Model& model, const std::string& fileForLsp);

    // Read a .winmd into the projection model and register its types. Entry point for the
    // import dispatch when it sees a `.winmd` extension.
    bool CompileWinmdFile(const std::string& path);

    // Diagnostic (M2 acceptance): import `path`, instantiate a few well-known parameterized
    // interfaces found in it, and check each derived PIID against the published reference IID plus
    // report the concrete vtable slot count. Drives the full reader -> template -> substitute ->
    // build -> PIID chain over REAL metadata. Returns true only if every present case matches.
    bool WinmdInstantiateSelfTest(const std::string& path, std::string& report);

    // ========================================================================================
    // WinMD PRODUCE (Phase 2): emit a .winmd from this compilation's [winrt] surface.
    // ========================================================================================

    // Convert a CFlat type to a WinRT logical signature type: fundamentals via the shared
    // typeMap; a [winrt] interface/class by name; anything else degrades to Object. A pointer
    // on a fundamental becomes one indirection level (interface/class refs carry no pointer in
    // the logical signature - the COM thinness is an ABI detail, not metadata).
    cflat_winmd::TypeRef CFlatTypeToWinrt(const TypeAndValue& tv);

    // Build a winmd model from the [winrt] interfaces (those carrying a [uuid]) and [winrt]
    // classes declared in this compilation, then write it to `path`. The interface methods come
    // from interfaceTable; the runtime classes from winrtClasses.
    bool EmitWinmd(const std::string& path, const std::string& assemblyName);

    // Parse-only verification of a .winmd (the `--check` path for metadata files): read it into
    // the model and report success/failure WITHOUT registering types or emitting anything. Used
    // by test_winmd.bat to batch-validate the whole SDK's metadata in one process.
    bool CheckWinmd(const std::string& path);

    // The vtable slot holding the concrete destructor: it trails the method and field-offset
    // slots, so it moves as either count grows. Single source of truth for the dtor index.
    int InterfaceDtorSlotIndex(const std::string& ifaceName) const;

    // The address of interface field `fieldName` inside the object behind fat value `fatVal`:
    // dataPtr + vtable[1 + methodCount + fieldIdx] (the implementor's byte offset). The result is
    // a true lvalue - reads and writes both go through it. Returns null when the name is no field.
    llvm::Value* EmitInterfaceFieldAddress(llvm::Value* fatVal, const std::string& ifaceName,
                                           const std::string& fieldName, llvm::Type* fieldType);

    // delete through an interface fat pointer: the operand is a {vtable, data} struct, not a
    // raw pointer. Extract the data pointer, run the concrete destructor via the vtable's
    // trailing dtor slot (runtime null-guarded), then free with
    // operator delete. A null data pointer (already deleted) makes the whole thing a no-op.
    // When fatStorage is non-null its data field is nulled so a second delete is also a no-op.
    void DeleteInterfaceValue(llvm::Value* fatVal, const std::string& ifaceName, llvm::Value* fatStorage);

    // Returns true if the given constant is a pooled string literal (length known at compile time).
    bool IsStringLiteralConstant(llvm::Constant* c) const;

    // Wraps a raw i8* string literal pointer in a string struct { _ptr, _len } by value.
    // Called automatically when assigning a string literal to a string-typed variable.
    // For a runtime (non-literal) char*, the length is not known at compile time: derive it
    // via `operator string(const char*)` if available (matches the runtime coercion used
    // elsewhere), else fall back to a direct strlen call. Never sets the OWNED bit - the
    // result is a non-owning borrow over the caller's buffer.
    llvm::Value* WrapStringLiteralAsString(llvm::Value* strLitPtr);

    llvm::Function* GetOrDeclareStrlen();

    // Deep-copy a string VALUE into a freshly heap-allocated, NUL-terminated owned buffer
    // (OWNED high-bit set on _len). Used when a string whose ownership the compiler cannot
    // transfer (e.g. a by-value parameter, which is a copy of an argument the CALLER still
    // frees) must be stored into a longer-lived slot such as a struct field - a shallow store
    // would alias the source buffer and dangle once the source is freed. Mirrors the inline
    // copy used for a non-owning string passed to a `move` parameter. Copies _len content bytes
    // then writes the terminator, so an empty/null-backed source never reads from a null pointer.
    llvm::Value* EmitOwnedStringDeepCopy(llvm::Value* value);


    // Transfer ownership for `move` parameters: null the caller's source storage and mark
    // it moved, so the caller's scope exit does not free what the callee now owns. Shared by
    // the normal call path (CreateOverloadedFunctionCall) and virtual dispatch
    // (CallInterfaceMethod) so the two cannot drift. Call AFTER the call is emitted.
    /*
     * An explicit 'move x' argument bound to a BORROWING parameter transfers nothing: the
     * callee never destructs it and the deferred zeroing never runs, so the write reads as
     * ownership transfer but is a no-op. Only diagnose when the value actually owns a
     * resource - a non-owning value type has nothing to orphan, so 'move' there is harmless.
     */
    void DiagnoseExplicitMoveToBorrowParam(const std::string& functionName,
        const std::string& paramName, const std::string& paramType, bool paramIsMove,
        const NamedVariable& arg);

    void DiagnoseExplicitMoveToBorrowParam(const std::string& functionName,
        const TypeAndValue& param, const NamedVariable& arg);

    /*
     * A `unique T*` / `move T*` PARAMETER states the ownership claim AT the call site, so passing
     * a temp's `unique` field there is decidable here: the temp's destructor frees the pointee at
     * the end of this statement and the callee would own - and later free - the same block.
     *
     * A PLAIN `T*` parameter is the undecidable remainder and is deliberately left alone: the
     * store happens in the CALLEE, and a read-only `int rd(Node* n) { return n->v; }` must keep
     * accepting `rd(makeBox().t)`. Tracked in
     * internal/issue/p2/temp-unique-field-escapes-through-a-plain-pointer-parameter.md.
     */
    void RejectOwningTempUniqueFieldIntoSinkParam(const std::string& functionName,
        const TypeAndValue& param, const NamedVariable& arg);

    // paramsCarryAllocAlign=false when the params were synthesized from a funcptr TYPE, which has
    // no `alignas` clause to record - see ApplyFuncPtrSinkTransfer.
    void ApplyMoveParamTransfer(const std::string& functionName,
        const std::vector<TypeAndValue>& params, const std::vector<NamedVariable>& args,
        bool paramsCarryAllocAlign = true);

    // Indirect-call twin: a lambda literal's inferred owning sinks ride the funcptr TYPE
    // (FuncPtrParam::IsOwningSink), so the caller's source must be transferred exactly as a direct
    // call does. A no-op unless some parameter carries the inferred flag.
    void ApplyFuncPtrSinkTransfer(const std::string& functionName,
        const std::vector<TypeAndValue::FuncPtrParam>& params,
        const std::vector<NamedVariable>& args);

    // The synthesized TypeAndValue a funcptr parameter's per-param facts stand for. Only the
    // fields the sink/ownership machinery reads are filled.
    static TypeAndValue FuncPtrParamAsTypeAndValue(const TypeAndValue::FuncPtrParam& p, size_t index);

    // Is this argument PROVABLY a data pointer being passed to a closure parameter? Deliberately
    // one-sided: it answers yes only when the frontend positively recorded a pointer that is not
    // a closure. Anything it cannot prove (a ternary join, a `??` load, a future spelling whose
    // provenance nothing records) must come back false, because the caller ACCEPTS on false. An
    // allowlist here would false-reject every shape nobody enumerated, which this project ranks
    // as the worse failure; a missed diagnostic only restores the pre-existing behaviour.
    bool ArgumentIsProvablyDataPointer(llvm::Value* value, const NamedVariable& arg) const;

    // Name a data-pointer argument for the closure-parameter rejection. The interface argument
    // loop propagates shape flags but NOT TypeName, so this often has to stay generic - that is
    // deliberate and must not be "fixed" by widening propagation into that loop.
    std::string DescribeNonFunctionArgument(const NamedVariable& arg) const;

    /*
     * The gate for widening a CALL ARGUMENT into a fat `Lambda<>` parameter. Under opaque pointers
     * a data pointer is indistinguishable from code, so reject only what is PROVABLY data and widen
     * everything else. The direct call path (CreateOverloadedFunctionCall), virtual dispatch
     * (LowerByValueArg), and the RETURN path (CoerceToFuncPtrReturn, via
     * CheckClosureReturnProvenance sharing ArgumentIsProvablyDataPointer) all route through this
     * one predicate so the three accept sets cannot drift - a divergence lets the same program
     * compile through one spelling and not another.
     */
    llvm::Value* WidenToClosureFatChecked(llvm::Value* val, const NamedVariable& arg,
        const std::string& paramName, const std::string& fieldDesc = {});

    /*
     * The THIN sibling of WidenToClosureFatChecked. A thin `function<>` parameter takes a bare
     * code address, so a provable data pointer bitcast into that slot is CALLED as code. Shares
     * the one provenance predicate with the fat gate, and is applied on both the direct call path
     * and virtual dispatch (LowerByValueArg) so all four combinations keep ONE accept set.
     */
    void CheckThinFnPtrArgProvenance(llvm::Value* val, const NamedVariable& arg,
        const std::string& paramName) const;

    /*
     * The ASSIGNMENT-flavoured thin provenance gate: covers a thin `function<>` destination
     * (spelled local/field/array-element, or a generic-encoded element) fed a raw pointer via
     * '=', decl-init, brace field-init, field default-init, or brace array-init (fixed or view).
     * The FAT destination is already caught by accident at MOST of these sites (a closure is a
     * struct, so a pointer source fails the generic aggregate-store cast); a thin slot is a bare
     * pointer, so nothing objected. The two DEFAULT-VALUE spellings (field default, parameter
     * default) are the exception - they never reach the aggregate-store cast, so they carry an
     * explicit fat sibling, CheckFatClosureAssignProvenance, below. Rejects only what
     * ArgumentIsProvablyDataPointer proves, shared with the argument/return gates so the accept
     * set cannot drift between "pass", "return", and "assign". `destDesc` is the destination as
     * the user would write it (already quoted), e.g. "'f'" or "'s.f'" - reused for every spelling,
     * the way WidenToClosureFatChecked's fieldDesc is.
     */
    void CheckThinFnPtrAssignProvenance(llvm::Value* val, const NamedVariable& arg,
        const std::string& destDesc) const;

    /*
     * The FAT sibling of CheckThinFnPtrAssignProvenance, check-only (no widen) like
     * CheckClosureReturnProvenance's fat arm: a parameter default never routes through
     * WidenToClosureFatChecked, so a provable data pointer needs its own reject here instead
     * of inheriting one from a widen call. The FIELD default site (ParseFieldDefaultInitializer)
     * calls this too, but follows it with an explicit WidenBareOrThinToClosureFat - see
     * [[fat-field-default-legal-source-not-widened]] in interface-issue-queue.md.
     */
    void CheckFatClosureAssignProvenance(llvm::Value* val, const NamedVariable& arg,
        const std::string& destDesc) const;

    // Mirror of LowerClosureFatToThinFnPtr: widen what a fat `Lambda<>` parameter expects.
    // A named function becomes {shim, null} and a thin `function<>` value becomes {code, null};
    // an already-fat value and a non-pointer are returned untouched. Shared by the normal call
    // path and virtual dispatch so the two cannot drift.
    llvm::Value* WidenBareOrThinToClosureFat(llvm::Value* val);

    // Lower a closure fat struct {code, env} to the bare code pointer that a thin
    // `function<>` / extern-C function-pointer parameter expects. The closure ABI is env-last,
    // so a NON-capturing invoker (compile-time-null env, set only by ParseLambdaExpression when
    // the lambda captured nothing) is directly callable as a bare C function pointer. Anything
    // else would silently drop its captured state across that ABI and is rejected instead.
    // LogError never returns (it throws, or calls the [[noreturn]] FailCompilation), so the two
    // `return nullptr` below are unreachable and no caller can observe a null result. They exist
    // only to satisfy the compiler's return-path analysis - do not add null checks against them.
    llvm::Value* LowerClosureFatToThinFnPtr(llvm::Value* val, llvm::Type* targetTy,
        const std::string& paramName, const std::vector<std::string>& captureNames);

    // Lower a by-value argument to match a declared (non-variadic) by-value parameter:
    // implicit char*/literal -> string coercion, scalar/struct upconvert, and the
    // non-owning-string -> move-param heap-copy. Shared by the normal call path
    // (CreateOverloadedFunctionCall) and virtual dispatch (CallInterfaceMethod) so the
    // two argument-lowering paths cannot drift. Returns the lowered call-arg value.
    // The function-pointer guards below are reachable ONLY from virtual dispatch:
    // CreateOverloadedFunctionCall handles an IsFunctionPointer parameter in an earlier branch,
    // and the one call it makes to this function is guarded by !inVariadicRange, so no direct
    // call can arrive here with such a parameter.
    llvm::Value* LowerByValueArg(llvm::Value* value, const TypeAndValue& param, const NamedVariable& arg);

    /*
     * Is this argument PROVABLY unusable for this parameter? Deliberately one-sided, and NOT
     * "the overload scorer failed to rank it" - the scorer has no int->floating-point rule, so
     * its silence is absence of a rule, not proof of incompatibility. The only proof taken here
     * is an integer or floating-point VALUE reaching a closure slot: a scalar can never be code,
     * and lowering it emits an `inttoptr` that virtual dispatch then CALLS. Everything else stays
     * accepted, which is the pre-existing permissive behaviour.
     *
     * One legal-looking program does change: `io.lam(0)`, integer 0 as a null function pointer,
     * now errors. That spelling is already rejected on the direct path and for a plain `int*`
     * parameter, so this brings the interface arm into parity; `nullptr` works on both.
     */
    bool ArgumentProvablyMismatchesParameter(const NamedVariable& arg, const TypeAndValue& param) const;

    /*
     * PROOF, not a heuristic: a pointer argument whose pointee type IS the by-value parameter's
     * type has no conversion to that slot - the raw address lowers into a struct slot and fails
     * module verification. An INTERFACE parameter is excluded: a 'Circle*' legitimately upcasts to
     * an 'IShape' value. Same question the direct-call scorer answers in TypeAndValue::IsTypeMatch;
     * virtual dispatch needs its own copy because the lone-slot arm picks by ARITY alone.
     */
    bool PointerArgIntoByValueParam(const NamedVariable& arg, const TypeAndValue& param) const;

    /*
     * The depth question at the INDIRECT-call door (`function<int(T*)> f; f(pp)`), which lowers
     * its own argument list and enters neither the scorer nor ResolveInterfaceMethodSlot. A
     * FuncPtrParam records no ElemPointer bit, only a PointerDepth whose 0 means "not recorded"
     * (C interop, WinRT and synthesized signatures never fill it), so a POSITIVE depth on the
     * parameter is required before anything is refused. Returns the diagnostic clause, or "".
     */
    std::string FuncPtrArgDepthMismatch(const NamedVariable& arg, const TypeAndValue::FuncPtrParam& p) const;

    /*
     * The same proof for a by-value PRIMITIVE parameter, which needs its own form: a primitive
     * pointer argument ('int* p') carries an EMPTY CFlat TypeName, so the same-name test above
     * cannot see it. Both the type flag and the LOWERED LLVM type must agree the argument is a
     * pointer - a by-value struct's Primary is an alloca address, so the LLVM type alone would
     * mistake one for a pointer. No pointer converts to an arithmetic slot, so nothing is proven
     * about the pointee: this is the same shape the direct path already rejects for 'int*'->'int'.
     */
    bool PointerArgIntoByValuePrimitiveParam(const NamedVariable& arg, const TypeAndValue& param) const;

    // Reject a virtual call whose argument provably cannot satisfy its parameter. Shared by both
    // slot-picking arms so a second same-arity overload cannot turn a clean error into a miscompile.
    bool DiagnoseProvableInterfaceArgMismatch(const std::string& ifaceName,
        const std::string& methodName, const std::vector<NamedVariable>& args,
        const std::vector<TypeAndValue>& params);

    /*
     * Pick the vtable slot for an interface method call by FULL SIGNATURE. Slots are gathered by
     * name, filtered by arity, then ranked with the same scorer the direct-call path uses
     * (ComputeOverloadFunction), so `io.go(a)` and `io.go(a, b)` reach different slots.
     * GetOrCreateVTable already populates each slot from the matching implementor overload, so
     * only the call site needed the signature match. Returns the slot index, or -1 after an error.
     * matchedArgs receives the arguments reordered against the winning slot's parameters.
     */
    int ResolveInterfaceMethodSlot(const std::string& ifaceName, const std::string& methodName,
        const std::vector<InterfaceMethod>& methods, const std::vector<NamedVariable>& args,
        std::vector<NamedVariable>& matchedArgs);

    llvm::Value* CallInterfaceMethod(llvm::Value* ifacePtr, const std::string& ifaceName,
        const std::string& methodName, const std::vector<NamedVariable>& extraArgNVs,
        const NullIfaceDispatchSite* site = nullptr);

    // Helper for building string NamedVariables for reflection visitor calls.
    // Wraps a string literal pointer into a string struct value.
    NamedVariable MakeStringLiteralNV(const std::string& text);

    void RegisterDestructor(const std::string& structName, llvm::Function* fn);

    // True if the named type has a destructor. Used by the LSP unused-locals check
    // to skip RAII locals, whose declaration alone is the point (e.g. a `lock`).
    bool TypeHasDestructor(const std::string& structName) const;

    /// Returns i64 sizeof(type) as a compile-time constant.
    llvm::Value* GetTypeSizeBytes(llvm::Type* type);

    /// Returns i64 alignof(type) as a compile-time constant.
    llvm::Value* GetTypeAlignBytes(llvm::Type* type);

    /// Effective alignment of a declaration: max of `alignas` on the decl,
    /// `alignas` on the struct type (if any), and the ABI alignment.
    uint64_t GetEffectiveAlignment(const DeclTypeAndValue& decl, llvm::Type* type);

    /// Same as above but starting from a type name (no DeclTypeAndValue handy).
    uint64_t GetEffectiveAlignmentForType(const std::string& typeName, llvm::Type* type);

    /// Alignment the default `operator new` already guarantees on x64. An allocation
    /// that needs more routes to `operator new(size, align)` / `__delete_aligned`.
    static constexpr uint64_t kDefaultNewAlign = 16;

    /// C++-style padded size: roundUp(allocSize, effectiveAlign).
    /// Used by sizeof so that arrays of over-aligned types stride correctly.
    uint64_t GetEffectiveAllocSize(llvm::Type* type, uint64_t effAlign);

    void RegisterStructInterfaces(const std::string& structName, const std::vector<std::string>& interfaces);

    std::vector<std::string> GetStructInterfaces(const std::string& structName) const;

    // Record the statically-conformed interfaces declared by [Capability(...)]. Kept apart from
    // Interfaces so the fat-pointer conversion sites never see them.
    void RegisterStructStaticInterfaces(const std::string& structName, const std::vector<std::string>& interfaces);

    std::vector<std::string> GetStructStaticInterfaces(const std::string& structName) const;

    // True when the type declares (or inherits, through interface parents) capability `ifaceName`.
    // Reads both the nominal and the static list, so a class ': ILockable' and a
    // '[Capability(ILockable)]' struct both answer yes.
    bool TypeHasCapability(const std::string& typeName, const std::string& ifaceName) const;

    bool TypeImplementsInterface(const std::string& typeName, const std::string& ifaceName) const;

    void VerifyInterfaceImplementation(const std::string& structName, const std::string& interfaceName);

    llvm::Function* SynthesizeReflectFunction(const std::string& structName);

    llvm::GlobalVariable* CreateGlobalVariable(TypeAndValue typeValue, llvm::Constant* initValue, bool threadLocal = false, uint64_t userAlign = 0, bool externalDecl = false);

    // Emit alloca in the function entry block - loop-body allocas would grow the stack unboundedly.
    // VLAs (non-null arraySize) must stay at the current point (dynamic size).
    llvm::AllocaInst* AllocaAtEntry(llvm::Type* type, llvm::Value* arraySize, const llvm::Twine& name = "", uint64_t align = 0);

    // --sanitize=ownership (M1): allocate the hidden i64 origin slot beside a pointer local's
    // storage and zero-init it at the declaration point (which dominates every later use). Keyed
    // on the local's alloca. Called from CreateLocalVariable; no-op unless the flag is on.
    void CreateOwnOriginSlot(llvm::Value* storage);

    // --sanitize=ownership (M1): record a move site into an owning-pointer local's origin slot,
    // encoded as ((i64)line<<32)|col (never 0 for a real 1-based line). No-op if untracked.
    void SetOwnMoveOrigin(llvm::Value* storage, size_t line, size_t col);

    // --sanitize=ownership (M1): reset an origin slot to live (0) when a moved-from local is
    // reassigned (revive). No-op if untracked.
    void ClearOwnMoveOrigin(llvm::Value* storage);

    // --sanitize=ownership: guard a DEREFERENCE (p->f, *p, p[i]). A null pointer here is a
    // use-after-move / use-after-free by definition - `move` and `delete` both null the slot,
    // and that null travels with the slot through container realloc, so it is the sound, relocation-
    // proof signal (no object shadow map, no address-reuse false positives). Trap on ptr==null. If
    // the base is a tracked owning local with a recorded move origin, report "moved at X"; otherwise
    // report a generic null deref. A null-COMPARE never reaches here (only real deref sites call this).
    void EmitOwnDerefGuard(llvm::Value* storage, llvm::Value* loadedPtr, size_t useLine, size_t useCol);

    // Returns the variable's STORAGE: an alloca normally, or an internal module global when the
    // pending-static request below matches this declaration (a `static` local).
    llvm::Value* CreateLocalVariable(TypeAndValue typeValue, llvm::Type* autoType = nullptr, llvm::Value* arraySize = nullptr, size_t line = 0, uint64_t userAlign = 0);

    // Set by the declaration path just before it creates a `static` local's storage; consumed by
    // the first CreateLocalVariable whose name, type, enclosing FUNCTION and scope depth all match.
    // The function+depth pair is what stops a local declared while the initializer is evaluated -
    // a lambda body's own local, an inlined return-block parameter - from claiming the storage.
    void RequestStaticLocalStorage(const std::string& varName, const std::string& typeName);
    void ClearStaticLocalRequest();
    bool MatchesStaticLocalRequest(const TypeAndValue& typeValue) const;
    std::string pendingStaticLocalName_;
    std::string pendingStaticLocalType_;
    llvm::Function* pendingStaticLocalFn_ = nullptr;
    size_t pendingStaticLocalDepth_ = 0;

    // Register a value directly as a named variable without an alloca.
    // Used for pointer-type params in return-block inlining so GetValue() returns the pointer itself.
    void RegisterPrimaryVariable(const TypeAndValue& typeValue, llvm::Value* value);

    llvm::AllocaInst* CreateAlloca(llvm::Type* type);

    void RegisterThisPointer(const TypeAndValue& tv, llvm::Value* storage, llvm::Type* baseType);

    llvm::Value* CreateIncrement(llvm::Value* destination, int amount, llvm::Type* elemType = nullptr,
                                 llvm::Type* loadType = nullptr);

    llvm::Value* CreateInsertValue(llvm::Value* structInstance, llvm::Value* newValue, unsigned int index);

    llvm::Value* CreateStructGEP(llvm::Type* structType, llvm::Value* structAlloc, unsigned int index, std::string variableName = "");

    llvm::Value* CreateGEP(llvm::Type* type, llvm::Value* ptr, llvm::Value* offset, std::string name = "");

    llvm::Value* CreateExtractValue(llvm::Value* structInstance, unsigned int index);

    llvm::StoreInst* CreateAssignment(llvm::Value* value, llvm::Value* destination, bool srcIsUnsigned = false, llvm::Type* explicitDestType = nullptr);

    llvm::LoadInst* CreateLoad(llvm::Value* value);

    llvm::LoadInst* CreateLoad(llvm::Type* type, llvm::Value* value);

    // Load an argument/receiver out of its Storage. A UNION member's Storage is the union alloca
    // (every arm aliases at offset 0), so the type must come from UnionFieldType, not from the
    // storage. Identical to CreateLoad(Storage) for every non-union NamedVariable.
    llvm::Value* LoadArgStorage(const NamedVariable& arg);

    llvm::Value* Upconvert(llvm::Value* value, llvm::Value* destination, bool srcIsUnsigned = false) const;

    llvm::Value* Upconvert(llvm::Value* value, llvm::Type* destType, bool srcIsUnsigned = false) const;

    // C integer promotion for compile-time constant folding: (i8)1 << 20 folds to 0 without this.
    // Cap at i32 - i16 still overflows, i64 is unnecessary. Gated on both operands being constants.
    llvm::Value* PromoteToInt(llvm::Value* value, bool isUnsigned = false) const;


    int CompareUpconvert(llvm::Type* srcType, llvm::Type* destType) const;

    llvm::Type* GetTypeFromStorage(llvm::Value* value) const;

    /*
     * Noun phrase describing aggregate storage for a diagnostic. Callers that know the CFlat
     * element-type name pass it and get "fixed array 'int[2][8]'". Where no name is available an
     * LLVM type cannot supply one (i8 is char and bool alike), so the phrase says the dimensions
     * in words rather than printing a placeholder letter that would read as a real type name.
     * The name must come from the declaration, never from a lowered artifact whose symbol may
     * carry an LLVM '.N' collision suffix.
     */
    static std::string DescribeAggregateStorageShape(llvm::Type* type,
                                                     const std::string& elementTypeName = "");

    llvm::Value* CreateCast(llvm::Value* value, llvm::Type* destType, bool isSigned = false);

    llvm::Value* CreateCast(llvm::Instruction::CastOps op, llvm::Value* value, llvm::Type* destType);

    // Returns the bit width of a bitfield's underlying integer type, or 0 if the
    // type is not a permitted bitfield base. C permits any integer type (plus
    // _Bool / bool); CFlat mirrors that and adds the sized integer aliases.
    static unsigned BitfieldStorageBits(const std::string& typeName);

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
        std::vector<BitfieldInfo>& outBitfields);

    // Effective alignment of a struct FIELD's slot: the max of the type's ABI alignment,
    // the field's own `alignas(N)`, and the field TYPE's `alignas` (an over-aligned struct
    // used by value keeps its alignment, as in C++). A pointer to an over-aligned type is
    // still just a pointer, so type-level over-alignment is only inherited by value fields.
    uint64_t GetFieldSlotAlignment(const DeclTypeAndValue& f, llvm::Type* t) const;

    // True when some field of `in` needs more alignment than its LLVM type's ABI alignment
    // (an `alignas(N)` member, or a by-value field of an over-aligned struct type).
    bool FieldsNeedAlignmentPadding(const std::vector<DeclTypeAndValue>& in) const;

    // Insert synthetic `__padN` ([N x i8]) slots so every over-aligned field starts on its
    // required boundary. Mirrors PackBitfields: the returned vector IS the storage list the
    // caller hands to CreateStructType, so StructFields and the LLVM element indices stay in
    // lockstep and no CreateStructGEP site needs a semantic->storage index map.
    //
    // outMaxAlign receives the strictest field alignment (0 when no field is over-aligned);
    // the caller folds it into the struct's `alignas`, which raises alignof and tail-pads
    // sizeof to a multiple of it. Unions must NOT be padded (their body is one array).
    //
    // `bitfields` (the side-table from a preceding PackBitfields) is remapped in place: an
    // inserted pad shifts the `__bfN` storage slots it precedes.
    std::vector<DeclTypeAndValue> PadFieldsForAlignment(
        const std::vector<DeclTypeAndValue>& in,
        uint64_t& outMaxAlign,
        std::vector<BitfieldInfo>* bitfields = nullptr);

    // True when a chain of `unique` pointer fields starting at `from` reaches `target`.
    // Pointee types not yet registered are skipped: a cycle needs every member present, so
    // the last one registered is the one that closes the chain and reports.
    bool UniqueChainReaches(const std::string& from, const std::string& target) const;

    // D2: a `unique` field whose pointee transitively reaches a `unique` field of the same
    // type would synthesize a self-recursive destructor that overflows the stack on a long chain.
    void RejectUniqueDestructionCycles(const std::string& name, const std::vector<LLVMBackend::DeclTypeAndValue>& fields);

    // Create StructType or OpaqueStruct
    //
    // Bitfield contract: if any entry in `typeAndValues` has IsBitfield=true,
    // the caller MUST have already run PackBitfields and passed the storage
    // entries (synthetic `__bfN` slots) here, together with the BitfieldInfo
    // side-table delivered via `bitfields`. CreateStructType itself does NOT
    // pack - the default-ctor path needs the packed list before this call to
    // emit one initializer per LLVM struct element.
    llvm::StructType* CreateStructType(std::string name, std::vector<LLVMBackend::DeclTypeAndValue> typeAndValues, uint64_t userAlign = 0, std::vector<BitfieldInfo>* bitfields = nullptr);

    // Creates a union type as a struct with a single [N x alignTy] body, where N and alignTy
    // are chosen to match the size and alignment of the largest/most-aligned member.
    // StructFields is preserved for metadata (field name lookup, type info).
    //
    // `userAlign` is `alignas(N)` on the union itself; `alignas(N)` on a MEMBER folds into the
    // same value (all union members start at offset 0, so an over-aligned member simply raises
    // the union's alignment - no padding slot, which a union body could not carry anyway).
    // The body is then grown to a multiple of it so getTypeAllocSize matches the padded sizeof.
    llvm::StructType* CreateUnionType(std::string name, std::vector<DeclTypeAndValue> typeAndValues, uint64_t userAlign = 0);

    llvm::Value* CreateConstant(ConstantVariant constantVariant);

    llvm::Constant* CreateConstant(std::string typeName, std::string initialValue);

    llvm::Value* CreateGlobalString(std::string name, std::string text);

    // Element-wise arithmetic and comparison on simd<T,N> values. Either operand may be a scalar,
    // which is splatted across all lanes (with element-type conversion). Both vector operands must
    // share the same lane count and element type. Arithmetic (+ - * /) yields a same-shape vector;
    // a comparison (== != < <= > >=) yields a `<N x i1>` mask (a simd<bool,N>) for use with
    // simd<T,N>.select - the branchless primitive that lets a masked kernel (e.g. LBM bounce-back)
    // stay straight-line and vectorize.
    llvm::Value* CreateVectorOperation(Operation op, llvm::Value* left, llvm::Value* right,
                                       bool leftIsUnsigned = false, bool rightIsUnsigned = false);

    // Splat a scalar across all lanes of a simd<T,N> value (converting element type as needed).
    // srcIsUnsigned steers the widening step so an unsigned source zero-extends into a wider lane.
    llvm::Value* SplatToSimd(llvm::Value* scalar, const TypeAndValue& tv, bool srcIsUnsigned = false);

    /*
     * Convert a scalar to an exact target scalar type, handling both widening (Upconvert)
     * and narrowing (e.g. a double literal into a float lane), which Upconvert alone won't do.
     * srcIsUnsigned only steers the WIDENING step; the CreateCast leg keeps its long-standing
     * default so threading the flag through cannot change a narrowing or an int<->float cast.
     */
    llvm::Value* ConvertScalarToType(llvm::Value* scalar, llvm::Type* target, bool srcIsUnsigned = false);

    // Reverse-map an IR function to its source-level name for diagnostics.
    // Only called on the error path, so the linear scan is fine.
    std::string FindFunctionSourceName(const llvm::Function* fn) const;

    // A bare llvm::Function reaching value context means the user referenced a
    // function without calling it (e.g. 'Math.PI' instead of 'Math.PI()').
    // Without this check the address either bitcasts into a garbage scalar or
    // trips an LLVM assert inside arithmetic codegen.
    void RejectBareFunctionValue(llvm::Value* value) const;

    // True for a struct-typed operand that is NOT one of the fat-pointer carriers, which
    // have their own field-extract comparison against nullptr.
    static bool IsPlainStructOperand(llvm::Value* v);

    /*
     * True only when `value` is PROVABLY an address that was never `new`-allocated: an alloca
     * (a stack slot), a global variable, or a GEP chain rooted at either (`&local`, `&arr[0]`,
     * `&s.f`, `&globalStruct.f`). Deliberately one-directional - anything not provable returns
     * false and is let through, so this can never reject a legal heap/borrowed source. Shared by
     * MainListener (a 'unique' store site) and the call-argument lowering here (a 'move' pointer
     * parameter) - both classes need it, so it lives on LLVMBackend rather than duplicated twice.
     * Named for what it proves ("non-heap"), not "stack": a global is exactly as un-free()-able.
     */
    static bool IsProvableNonHeapAddress(llvm::Value* value);

    llvm::Value* CreateOperation(std::string oper, llvm::Value* left, llvm::Value* right);

    llvm::Value* CreateOperation(Operation op, llvm::Value* left, llvm::Value* right);

    // Signedness-aware: chooses ZExt vs SExt, UDiv vs SDiv, ICMP_UGT vs ICMP_SGT, etc.
    llvm::Value* CreateOperation(Operation op, llvm::Value* left, llvm::Value* right,
                                  bool leftIsUnsigned, bool rightIsUnsigned);

    llvm::Value* CreateOperation(std::string oper, llvm::Value* left, llvm::Value* right,
                                  bool leftIsUnsigned, bool rightIsUnsigned);

    llvm::Value* CreateNot(llvm::Value* value);

    // Logical '!': zero-compare, always i1. Distinct from CreateNot ('~', bitwise xor -1),
    // which only coincides with logical negation on i1 - on a wider int every operand
    // negates to a nonzero value (~1 == -2, ~0 == -1), making '!x' unconditionally true.
    llvm::Value* CreateLogicalNot(llvm::Value* value);

    llvm::Value* CreateNeg(llvm::Value* value);

    llvm::BasicBlock* CreateBasicBlock(std::string name, llvm::Function* fn = nullptr);

    void SwitchToBlock(llvm::BasicBlock* block);

    llvm::BranchInst* CreateJump(llvm::BasicBlock* block);

    /// Returns the LLVM return type for the first overload of a function, or nullptr if not found.
    llvm::Type* GetFunctionReturnType(const std::string& functionName) const;

    TypeAndValue GetFunctionReturnTypeInfo(const std::string& functionName) const;

    std::string CreateAnonFunctionName();

    // Returns a TypeAndValue describing the function pointer type for the named function.
    TypeAndValue MakeFuncPtrTypeAndValue(const std::string& functionName) const;

    /*
     * A pointer VALUE left sitting in a non-pointer parameter slot of an indirect call. Checked
     * AFTER the conversion attempt, so it is a residue, not a prediction: Upconvert has no
     * ptr-to-struct or ptr-to-arithmetic arm and returns the value unchanged, and LLVM requires an
     * EXACT type match per call argument - so this shape always fails module verification, with no
     * source location. Rejecting it can therefore never refuse a program that builds. Only this one
     * direction is judged; every other residual mismatch is left exactly as it was.
     */
    void CheckIndirectCallArgShape(llvm::Value* arg, llvm::Type* destTy, size_t index,
                                   const std::string& paramTypeName);

    // Emits an indirect call through a closure fat struct {i8* fnptr, i8* envptr}.
    llvm::Value* CreateIndirectCall(const TypeAndValue& funcPtrType, llvm::Value* funcPtr, std::vector<llvm::Value*> args);

    llvm::SwitchInst* CreateSwitchInst(llvm::Value* cond, llvm::BasicBlock* defaultBlock, unsigned numCases);

    llvm::ConstantInt* CoerceCaseValue(llvm::ConstantInt* val, llvm::Type* switchType);

    llvm::Function* GetOrDeclareStrcmp();

    /// <summary>
    /// Lower an arbitrary scalar condition value to the i1 LLVM demands of a branch or a
    /// select. Shared by the if/while/for path (CreateConditionJump) and by '?:'.
    /// </summary>
    llvm::Value* CoerceToBoolCondition(llvm::Value* cond);

    // Short type name for a rejected condition operand.
    std::string DescribeConditionType(llvm::Type* t) const;

    llvm::BranchInst* CreateConditionJump(llvm::Value* cond, llvm::BasicBlock* trueBlock, llvm::BasicBlock* falseBlock);

    llvm::PHINode* CreatePHINode(std::string name, int reserve);

    llvm::Value* CreateSelect(llvm::Value* cond, llvm::Value* falseValue, llvm::Value* trueValue);

    /// <summary>
    /// Exit the current BasicBlock and then jump to resumeBlock.
    /// </summary>
    llvm::BranchInst* CreateBlockBreak(llvm::BasicBlock* resumeBlock, bool exitBlockStack);

    void InitializeBlock(llvm::BasicBlock* block, bool enterBlockStack, llvm::BasicBlock* continueBlock = nullptr, llvm::BasicBlock* resumeBlock = nullptr, llvm::BasicBlock* elseBlock = nullptr);

    // True when tv refers to a C-compatible struct (or auto-registered C struct) passed by
    // value (Pointer=false). Excludes interface fat-pointers, function pointers, strings,
    // and any TypeName not present in dataStructures with a complete (non-opaque) body.
    bool IsByValueStructTV(const TypeAndValue& tv) const;

    // Flatten an aggregate into its scalar leaf fields with absolute byte offsets, recursing
    // through nested structs and arrays. Used by the SysV eightbyte classifier.
    void CollectScalarFields(llvm::Type* ty, uint64_t base, const llvm::DataLayout& dl,
                             std::vector<std::pair<uint64_t, llvm::Type*>>& out) const;

    // SysV AMD64 eightbyte classification for a small aggregate. Returns the LLVM coercion
    // type per eightbyte (1 or 2 entries); empty means the struct is class MEMORY (size > 16
    // or a field straddling an eightbyte) and must be passed via ByVal / returned via SRet.
    // An eightbyte is SSE only if every field overlapping it is float/double; any integer/
    // pointer field makes the whole eightbyte INTEGER. Two packed floats -> <2 x float>.
    std::vector<llvm::Type*> ClassifySysVStruct(llvm::StructType* st) const;

    // Build a param/return slot from a classifier's coerce list (SysV eightbytes or AArch64
    // registers): empty -> MEMORY (memoryKind: ByVal for params, SRetReturn for returns);
    // one entry -> CoerceToInt; two -> CoercePair. Shared by the SysV and AArch64 classifiers.
    static AbiSlot MakeCoerceSlot(llvm::StructType* st, uint64_t align,
                                  const std::vector<llvm::Type*>& coerce, AbiSlot::Kind memoryKind);

    // SysV param classification: 1 eightbyte -> CoerceToInt; 2 -> CoercePair; MEMORY -> ByVal.
    AbiSlot ClassifySysVParam(llvm::StructType* st, uint64_t align);

    // SysV return classification: 1 eightbyte -> CoerceToInt; 2 -> CoercePair; MEMORY -> SRet.
    AbiSlot ClassifySysVReturn(llvm::StructType* st, uint64_t align);

    // True if `st` is an AArch64 Homogeneous Floating-point Aggregate: 1-4 leaf fields, all
    // the SAME floating type (all float OR all double), with no padding. HFAs are passed and
    // returned in consecutive SIMD/FP registers (V0..V3). On match, sets base + count.
    bool IsAArch64HFA(llvm::StructType* st, llvm::Type*& base, unsigned& count) const;

    // AArch64 AAPCS64 aggregate classification. Returns the LLVM coercion type(s):
    //   - HFA (1-4 same FP type)  -> one entry: the base type (count 1) or [count x base].
    //   - other <= 8 bytes        -> one entry: i(size*8)              (one X register).
    //   - other 9..16 bytes       -> two entries: i64 + i((size-8)*8)  (two X registers).
    //   - > 16 bytes              -> empty = MEMORY (indirect: ByVal param / SRet return).
    // Differs from SysV: AArch64 never splits a non-HFA struct into SSE eightbytes; small
    // mixed int/float aggregates go entirely in general registers.
    std::vector<llvm::Type*> ClassifyAArch64Struct(llvm::StructType* st) const;

    // AArch64 param classification: 1 coerce -> CoerceToInt; 2 -> CoercePair; MEMORY -> ByVal.
    AbiSlot ClassifyAArch64Param(llvm::StructType* st, uint64_t align);

    // AArch64 return classification: 1 coerce -> CoerceToInt; 2 -> CoercePair; MEMORY -> SRet.
    AbiSlot ClassifyAArch64Return(llvm::StructType* st, uint64_t align);

    // Win64 / Win32 / SysV / AArch64 ABI classification for a single param slot. Returns Direct
    // for scalars and pointers (the existing pipeline handles them). For struct-by-value:
    //   - AArch64 (macOS arm64): AAPCS64 (HFA in SIMD regs / 1-2 X regs / >16B indirect).
    //   - SysV (non-Windows x86): eightbyte classification (CoerceToInt / CoercePair / ByVal).
    //   - Win64: size in {1,2,4,8} -> CoerceToInt(iN); else ByVal(pointer + byval attr).
    //   - Win32: always ByVal (cdecl pushes the whole struct on the stack).
    AbiSlot ClassifyAbiParam(const TypeAndValue& tv);

    // Win64 / Win32 ABI classification for the return slot.
    //   - size in {1,2,4,8} -> CoerceToInt(iN) (returned in RAX / EDX:EAX as appropriate).
    //   - otherwise         -> SRetReturn: function returns void, caller passes hidden
    //                          pointer as arg 0 with the 'sret' attribute.
    AbiSlot ClassifyAbiReturn(const TypeAndValue& tv);

    // Build the full lowering recipe for an extern C function signature. hasLowering is
    // set if at least one slot is non-Direct so the call site knows whether to take the
    // fast path (existing CreateFunctionCall) or the ABI-rewriting path.
    AbiRecipe ComputeAbiRecipe(const TypeAndValue& retType,
                               const std::vector<TypeAndValue>& params);

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
                                                const AbiRecipe& recipe);

    // Attach byval / sret / alignment attributes on the function declaration per the recipe.
    // These are LLVM-level hints required for correct ABI lowering (the x86/x64 backend
    // uses them to decide register vs stack placement, byval copies, and sret semantics).
    void ApplyAbiAttributes(llvm::Function* fn, const AbiRecipe& recipe);

    // Number of LLVM params a param slot lowers to (CoercePair -> 2, everything else -> 1).
    static unsigned SlotLLVMParamCount(const AbiSlot& s);

    // linkageName: optional override of the emitted LLVM symbol for externs. A namespaced
    // extern (namespace os.windows { extern ... Sleep(...); }) registers in the function
    // table under the qualified lookup name but must link against the bare C symbol.
    void CreateFunctionDeclaration(std::string functionName, LLVMBackend::TypeAndValue returnType, std::vector<LLVMBackend::TypeAndValue> arguments, bool external = false, bool varargs = false, bool returnsOwned = false, bool isMethod = false, CallingConv callConv = CallingConv::Default, const std::string& linkageName = {});

    // Return the FunctionSymbol whose LLVM function pointer matches fn, or nullptr.
    const FunctionSymbol* GetFunctionSymbol(llvm::Function* fn) const;

    // Set RequiredLocks on the most-recently registered overload of functionName.
    void SetFunctionRequiredLocks(const std::string& functionName, std::vector<std::string> locks);

    // Builds the bare LLVM function-pointer type R(*)(Args) for a function-pointer TypeAndValue.
    // This is the wire type of a thin `function<T>` and of any C-ABI function pointer.
    llvm::Type* BuildThinFnPtrType(const TypeAndValue& tv) const;

    // Returns the C-compatible LLVM type: for IsFunctionPointer, bare fn ptr (not fat struct).
    // Used for extern function declarations to preserve C ABI compatibility.
    llvm::Type* GetCCompatibleType(const TypeAndValue& tv) const;

    llvm::FunctionType* GetFunctionType(const LLVMBackend::TypeAndValue& returnType, const std::vector<LLVMBackend::TypeAndValue>& arguments, bool varargs = false, bool externC = false);

    std::string ComputeMangledName(const std::string& functionName, const LLVMBackend::TypeAndValue& returnType, const std::vector<LLVMBackend::TypeAndValue>& arguments, bool varargs = false);

    static std::string SourceFileLeaf(const std::string& path);

    /*
     * A SECOND body for a mangled name - two definitions of one overload slot. Until now the first
     * one silently won and the second was unreachable, so `int f(int)` written twice compiled and
     * ran the first body. Since ToUniqueString became canonical, `f(int)` and `f(i32)` reach here
     * too: they are one overload, and before that they were two that picked each other's arguments.
     *
     * Reported ONLY when both origins carry a real source line AND the LINES differ. Line 0 is a
     * compiler-generated definition (lambda bodies, trampolines, thread shims) where re-entry is
     * routine, and a file processed twice - as the primary input and as its own import - re-attaches
     * the same body at the same line.
     *
     * The line alone is the discriminator; currentSourceFilePath_ is deliberately NOT part of it,
     * only of the message. It is not stable across the LSP re-analysis path: a bulk sweep attached
     * cocoa.cb's `setMenuHandler` twice under two different path values and the file comparison
     * reported the single definition as a redefinition of itself. Two DIFFERENT files holding the
     * same overload at the same line therefore go unreported - silence is the safe direction here,
     * being exactly what the compiler did before.
     */
    void DiagnoseDuplicateFunctionBody(const std::string& functionName,
        const std::string& mangledName, size_t line) const;

    /*
     * Is this overload slot already occupied by a BODY? Answers the exact question
     * CreateFunctionDefinition's `!fn->empty()` early return answers, but BEFORE the call, for a
     * caller that must not enter that early return at all: it pushes no function scope, so a
     * caller that goes on emitting pops a scope frame it never pushed.
     *
     * Out param 'originLine' is 0 for a COMPILER-SYNTHESIZED definition (created with no line),
     * which a synthesized caller should quietly yield to, and a real line for a user-written one,
     * which is a genuine clash worth reporting. Returns false when the slot is free or only declared.
     */
    bool OverloadSlotIsDefined(const std::string& functionName, const LLVMBackend::TypeAndValue& returnType,
        const std::vector<LLVMBackend::TypeAndValue>& arguments, bool varargs,
        std::string* originFile, size_t* originLine);

    llvm::Function* CreateFunctionDefinition(const std::string& functionName, LLVMBackend::TypeAndValue returnType, std::vector<LLVMBackend::TypeAndValue> arguments, bool external = false, bool varargs = false, size_t line = 0, bool returnsOwned = false, bool isMethod = false, CallingConv callConv = CallingConv::Default, size_t scopeLine = 0);

    // True when 'name' resolves to a type in the current compilation: a scalar
    // keyword, an enum, a type alias, an interface, or a registered struct. Used by
    // sizeof/alignof to disambiguate sizeof(Type) from sizeof(variable) - the type
    // meaning wins, matching C.
    bool IsKnownTypeName(const std::string& name) const;

    llvm::Type* GetType(const LLVMBackend::TypeAndValue& typeAndValue, llvm::Type* autoType = nullptr, bool allowPointer = true) const;

    /*
     * Tie-breaker between overloads that differ only in 'move': for each parameter, score +1
     * when the param's IsMove agrees with whether the caller-side argument is an owning lvalue
     * (so the borrow overload wins for non-owning args, the move overload for owning ones).
     * For function-pointer parameters, also score +1 when the named function arg has an
     * overload whose per-param IsMove flags match the candidate param's FuncPtrParams exactly
     * (so the move-typed start() wins for a move-typed fn).
     */
    int ScoreMoveAgreement(const std::vector<NamedVariable>& arguments, const FunctionSymbol& candidate) const;

    /*
     * Indirection shape of a function-pointer/closure parameter or argument:
     *   2 = array   ('function<T>[]' view, or a fixed 'function<T>[N]')
     *   1 = pointer ('function<T>*')
     *   0 = plain value ('function<T>' / 'Lambda<T>')
     * These are three distinct overloads, so the scorer compares shapes instead of accepting
     * every function-compatible argument as a perfect match. Pass `nv` on the ARGUMENT side
     * only: a fixed 'function<T>[N]' local reaches a call site with every TypeAndValue shape
     * flag cleared, so its array-ness survives only on the storage behind it. A subscripted
     * element ('arr[0]') has a GEP for Storage, not the alloca, and correctly scores a value.
     */
    static int FunctionPointerShapeOf(const LLVMBackend::TypeAndValue& tv, const NamedVariable* nv);

    /*
     * How to SPELL a function-pointer shape in a diagnostic. The shape itself is
     * FunctionPointerShapeOf's, re-derived here so the word and the verdict cannot drift. The
     * FAMILY comes from the closure TypeName markers (thin '__c_fn_ptr' is 'function<>', fat
     * '__closure_fat_ptr' is 'Lambda<>', an encoded generic closure carries its own flavour) and
     * from a bare llvm::Function argument, which is a named function and therefore thin. When
     * none of those says which family is in play the word stays a generic "closure": the call
     * argument loops copy a stored closure's SIGNATURE without its TypeName, and naming a
     * spelling the source may not have written is worse than naming less. Shape 2 likewise
     * distinguishes a fixed 'function<>[N]' array from a 'function<>[]' view.
     */
    std::string FuncPtrShapeWord(const TypeAndValue& tv, const NamedVariable* nv) const;

    /*
     * The shape gate shared by every site that lowers an ALREADY-SELECTED candidate: the direct
     * call (CreateOverloadedFunctionCall) and virtual dispatch (CallInterfaceMethod). 'function<T>',
     * 'function<T>*' and 'function<T>[]' are three distinct shapes with no implicit conversion
     * between them; the scorer ranks a disagreement non-perfect but with no better-shaped candidate
     * the mismatched arm still bound, so a bare VALUE landed in a pointer/view slot (or an ARRAY's
     * own address in a value slot) and the wrong bytes were called as code (SIGSEGV/SIGBUS, no
     * diagnostic). Returns true, having reported, when the argument must not bind.
     *
     * Two faces only. A VALUE into a pointer/view parameter, and an ARRAY/VIEW into a plain value
     * parameter. A POINTER argument into a value parameter is deliberately left to
     * ArgumentIsProvablyDataPointer, which owns its own (frozen) wording - widening this gate to
     * cover it would preempt that message. A literal 'nullptr' carries no shape flags at all and is
     * exempt, the same escape that gate grants it. The PARAMETER predicate mirrors the scorer's
     * ('IsFunctionPointer || IsEncodedClosureType'), so a monomorphized generic closure parameter -
     * 'list<function<int(int)>>::add(T value)' - is gated exactly like a spelled one.
     */
    bool RejectFuncPtrShapeMismatch(const NamedVariable& arg, const TypeAndValue& param);

    /*
     * The code-value gate for a call that binds through a DECLARED signature instead of overload
     * resolution - virtual dispatch (CallInterfaceMethod). The scorer refuses a code value against
     * a data parameter (LLVMBackend_Overloads.cpp), and every non-interface dispatch kind therefore
     * rejects `i.take(c ? fn : dataPtr)`; the vtable path consults no scorer, so the same crossing
     * bound and the callee wrote through a code address (SIGBUS, no diagnostic). Same predicate
     * (CodeValueIntoDataDestination) and same per-argument cast occurrence as every other site, so
     * an explicit '(Rec*)' cast still launders exactly what it launders on a direct call.
     * Returns true, having reported, when the argument must not bind.
     */
    bool RejectCodeValueIntoDataParam(const NamedVariable& arg, const TypeAndValue& param,
                                      const std::string& ifaceName, const std::string& methodName);

    /*
     * Canonical DESCRIPTOR of one component of a function-pointer signature. A function pointer's
     * type is tracked in the type system rather than inferred from the LLVM value, which under
     * opaque pointers carries no signature at all - so the proof below is made on the DECLARED
     * components, canonicalized so that a differing spelling of one type is never mistaken for
     * two types.
     *
     * `Known == false` means NO PROOF IS AVAILABLE: an interface, an unsubstituted generic
     * parameter, a struct the compiler has not registered. Such a component can never contribute
     * a rejection, so an unresolvable spelling keeps binding exactly as it did before.
     */
    struct FuncPtrComponent
    {
        bool Known = false;
        bool VoidPointee = false;   // `void*`: a wildcard over every pointee
        std::string Canon;          // "i32", "f64", "p:v", "s", "p:s", ...
        // Sorted candidate keys when Canon is "s"/"p:s". The SET is the answer, never one element.
        std::vector<std::string> StructKeys;
        // '*' count, or 0 for "not recorded" - one-sided like Known, so an unrecorded depth on
        // either side proves nothing. Both Canon and this must be known to reject on depth.
        int PointerDepth = 0;
    };

    // Resolve an alias chain (and an enum to its backing type) to whatever it ultimately names.
    std::string ResolveFuncPtrTypeSpelling(const std::string& typeName) const;

    /*
     * Canonical token of a SCALAR type, or empty when the name is not a known scalar. The token is
     * the LOWERED type, so the many spellings of one machine type collapse onto it: int/i32/u32
     * all give "i32", long/i64/ulong all give the host's native width. `long` is 32 bits on
     * Windows and 64 here, so `long` vs `i64` is one type on this host and two on Windows.
     *
     * SIGNEDNESS IS DELIBERATELY ABSENT. It is not part of an llvm::FunctionType - `u32(u32)` and
     * `i32(i32)` both lower to `i32(i32)` - so a signedness difference is zero ABI risk and
     * rejecting on it broke working programs. `bool` is NOT an integer here: it lowers to an `i1`
     * return (`define internal i1 @_isPos_bool_int_(i32)`), so calling it through an `int(int)`
     * pointer reads bits above bit 0 that the callee never defined.
     */
    std::string FuncPtrScalarCanon(const std::string& resolved) const;

    /*
     * ABI-canonical form of a registered struct key. Every `__`-separated token that names a
     * scalar is folded onto its LOWERED token (`Box__u32` and `Box__i32` both -> `Box__i32`);
     * every other token is emitted verbatim, so a plain `Circle` is untouched.
     *
     * Monomorphization canonicalizes SOURCE identity and deliberately keeps signedness, because
     * `u32` divides, compares and prints differently from `i32` - so `Box<i32>` and `Box<u32>`
     * really are two instantiations. But neither signedness nor a spelling changes LAYOUT, so
     * calling a `void(Box<i32>*)` through a `void(Box<u32>*)` pointer is safe, and rejecting it
     * took away a working program. `Box<double>` vs `Box<i32>` still differs here (f64 vs i32)
     * and is still caught - that is the memory-unsafe case, and it survives.
     *
     * `long` folds to the TARGET width, so `Box<long>` and `Box<i64>` are one key on LP64 and
     * two on Windows, which is the truth on each.
     *
     * STRICTLY RELAXING, which is why this is a MAP over the key set and never a filter: the map
     * is applied to both sides, so any key the two sets shared before still maps to a shared
     * value, and a rejection can only be removed, never invented. Filtering keys OUT would be a
     * tightening - the rule rejects on DISJOINT sets, so removing elements can only make two
     * sets more disjoint.
     */
    std::string FuncPtrAbiCanonKey(const std::string& key) const;

    /*
     * Every registered struct key a SPELLING could name: the key itself, plus any key whose
     * trailing dotted component is the spelling. A signature records the raw source spelling
     * (ResolveSigComponentCodegen keeps `ts->getText()` and no namespace resolution runs on that
     * path), so a bare `Pt` written inside `namespace NS` arrives here as "Pt" and could mean the
     * global `Pt` or `NS.Pt`.
     *
     * The caller compares SETS and never picks an element. Picking one would be a guess; and
     * treating a multi-candidate spelling as "unknown" would let a single unrelated same-named
     * struct - anywhere in the program or in anything it imports, referenced or not - silently
     * disarm the pointee proof. Set DISJOINTNESS guesses nothing and is not defeatable that way.
     */
    std::vector<std::string> FuncPtrStructCandidates(const std::string& spelling) const;

    /*
     * Descriptor of one component. `string` is normalized to `char*` BEFORE the pointer shape is
     * read: it carries Pointer == false yet interconverts with `char*`/`i8*` at every call site.
     */
    FuncPtrComponent FuncPtrComponentOf(const std::string& typeName, bool pointer,
        int pointerDepth = 0, const std::string& resolvedKey = "") const;

    /*
     * One-sided: true only when BOTH components resolve AND name provably different types.
     * A `void*` on either side is a wildcard over every other POINTER, matching the scorer's own
     * "any pointer is implicitly convertible to void*" rule; against a non-pointer it is proof.
     */
    static bool ComponentsProvablyDiffer(const FuncPtrComponent& a, const FuncPtrComponent& b);

    /*
     * Components of a function-pointer type (return first, then each parameter), WITHOUT the
     * thin/fat flavour ToUniqueString carries - a thin `function<>` value still widens into a fat
     * `Lambda<>` parameter of the same signature (WidenThinToFat), so the flavour must not
     * participate. Returns false only when there is no signature at all to read.
     */
    bool FuncPtrSignatureOf(const LLVMBackend::TypeAndValue& tv, std::vector<FuncPtrComponent>& out) const;

    /*
     * PROOF that two function-pointer signatures name different function TYPES - the one question
     * binding may reject on. One-sided throughout: a component unresolvable on either side
     * accepts, and only a differing arity or a provably differing component rejects.
     */
    bool FuncPtrSignaturesProvablyDiffer(const LLVMBackend::TypeAndValue& a,
        const LLVMBackend::TypeAndValue& b) const;

    // The function-pointer signature of ONE registered overload.
    TypeAndValue FuncPtrSigOfSymbol(const FunctionSymbol& sym) const;

    // The signature of the overload GetFunctionForFuncPtr actually BOUND. A by-NAME re-lookup
    // returns the first non-method entry, which is declaration-order dependent - use this instead
    // whenever per-parameter facts are read off a named-function initializer.
    TypeAndValue FuncPtrSigOfBoundFunction(const std::string& functionName,
        const llvm::Function* fn) const;

    /*
     * PROOF that a NAMED function cannot satisfy a function-pointer parameter. A named function
     * argument carries no signature on its own TypeAndValue - the value IS the function - so the
     * component proof was skipped entirely for it and `slot(fDbl)` silently entered a
     * `double(double)` body through an `int(int)` slot, returning a plausible wrong number.
     *
     * One-sided over the OVERLOAD SET, the same shape as the struct-candidate rule: proof requires
     * EVERY overload of the name to be provably different, because one that could bind is a
     * legitimate reading of the call. An unregistered name proves nothing.
     */
    bool NamedFunctionProvablyMismatchesFuncPtr(const std::string& functionName,
        const TypeAndValue& param) const;

    /*
     * Does this ARGUMENT name a function whose every overload is wrong for this parameter? Applies
     * only when the argument carries no signature of its own: a `function<>` VALUE is judged on its
     * declared type, and a local whose name shadows a function is that local, not the function.
     */
    bool NamedFunctionArgMismatches(const NamedVariable& arg, const TypeAndValue& param) const;

    // Source-shaped rendering of a function-pointer signature, for diagnostics: `double(double)`.
    std::string FuncPtrSpellingOf(const LLVMBackend::TypeAndValue& tv) const;

    // WHICH component of two signatures differs, as a diagnostic tail. Empty when nothing is
    // provably different. Shared, so the argument-site and bind-site messages cannot drift apart.
    std::string FuncPtrDifferenceOf(const std::vector<FuncPtrComponent>& sa,
        const std::vector<FuncPtrComponent>& sb) const;

    /*
     * Why this function-pointer argument cannot bind this parameter, naming the mechanism and the
     * component that differs. Empty when it can bind. Purely explanatory - it states what
     * FuncPtrSignaturesProvablyDiffer already decided and never widens the rejection.
     */
    std::string DescribeFuncPtrSignatureMismatch(const LLVMBackend::TypeAndValue& arg,
        const LLVMBackend::TypeAndValue& param) const;

    /*
     * Why a NAMED function cannot be entered through a destination function-pointer type. Empty
     * when it can be. Deliberately NOT the same rule as the argument path:
     *
     * A function taking FEWER parameters than the slot supplies is legal here and load-bearing.
     * The synthesized `onStdout` field is declared `void(char*, int)` and a `void(char*)` callback
     * is the form doc/LANGUAGE.md documents; under cdecl the caller cleans up, so the argument the
     * callee never reads is simply ignored. The MIRROR direction is not safe and stays proof - a
     * callee taking more parameters than the slot supplies reads one the caller never passed.
     * Component types must agree across the shared prefix either way.
     */
    std::string DescribeFuncPtrBindMismatch(const std::string& functionName,
        const LLVMBackend::TypeAndValue& fn, const LLVMBackend::TypeAndValue& dest) const;

    /*
     * An argument that IS code: a function pointer, a closure value, or a function symbol. One
     * predicate for the two readers that must not drift - the funcptr overload arm, which accepts
     * such an argument for a function-pointer parameter, and the void* implicit-conversion leg,
     * which refuses it. The trailing "any pointer" catch-all of the funcptr arm is deliberately
     * NOT here: that one is about the parameter being a closure slot, not about the argument.
     *
     * A recorded SIGNATURE counts, and is the form the direct call-argument loop produces: it
     * copies the three signature fields of a stored `function<>` without setting IsFunctionPointer
     * or TypeName (see their declaration), so that is the only evidence such an argument carries.
     */
    bool ArgumentIsFunctionPointerish(const NamedVariable& arg) const;

    /*
     * Is this ARGUMENT a code value - a function pointer or closure in its plain VALUE shape? The
     * shape check is load-bearing: a `function<T>*` is the ADDRESS of a slot and a `function<T>[N]`
     * decays to one, and both are real DATA pointers that must keep converting.
     */
    // occurrence: a stamped argument passes arg.CastOccurrenceId; LoadNamedVariable's
    // synchronous mid-evaluation check passes CurrentCastOccurrence() instead.
    bool ArgumentIsCodeValue(const NamedVariable& arg, size_t occurrence) const;

    /*
     * Is this ARGUMENT a value PROVEN to be data - a pointer whose declared type is not code? The
     * mirror of ArgumentIsCodeValue and the only positive evidence dataValues_ ever records.
     * Anything code-shaped or non-pointer answers no and stays unproven, so the closure-widen
     * gate keeps accepting it.
     */
    bool ArgumentIsDataValue(const NamedVariable& arg) const;

    // Does this PARAMETER store data rather than code? `string` counts: it is not a pointer but is
    // reached by the implicit char* -> string coercion, which lowers a code address the same way.
    bool ParameterStoresData(const TypeAndValue& param) const;

    /*
     * Does this PARAMETER accept a code value at all? The list mirrors the code-shape spellings
     * ArgumentIsFunctionPointerish accepts on the argument side, including the literal
     * `__closure_fat_ptr` a MONOMORPHIZED generic parameter carries - IsEncodedClosureType is a
     * map lookup of encoded names and does not contain it. Everything not listed here - pointer,
     * string, scalar - is a slot a function pointer or closure must not be poured into.
     */
    bool ParameterAcceptsCodeValue(const TypeAndValue& param) const;

    /*
     * DESTINATION-side reader for the conversion sites the overload scorer never sees: a declarator
     * initializer, an assignment (which also carries a field, element and global store), and a
     * `return`. Both predicates are the scorer's own, so the argument path and the store paths
     * cannot drift. Under opaque pointers a code pointer and a data pointer are one `ptr`, so the
     * proof has to be made on the DECLARED shapes: shape 0 on the source (a `function<T>*` is the
     * ADDRESS of a slot and a `function<T>[N]` decays to one - both genuinely data), and a
     * pointer-or-`string` destination that is not itself a function-pointer slot.
     */
    bool CodeValueIntoDataDestination(const NamedVariable& src, const TypeAndValue& dest) const;

    /*
     * One wording for every store site, matching the scorer's per-candidate line. `spelling` and
     * `castAdvice` are rendered by the CALLER (MainListener owns the declared-type spellers), so a
     * `Rec**` is never described as a `Rec*` and a cast is only ever advised when one COMPILES -
     * there is no cast that binds a code address to a `T[]` view, and `(string)` of a raw value is
     * itself rejected. `what` names the slot when it is not the declaration being reported on.
     */
    std::string DescribeCodeValueIntoData(const std::string& spelling, const std::string& role,
                                          const std::string& castAdvice,
                                          const std::string& what = {}) const;

    /*
     * A COMPOUND operator is a different question from a store. On a POINTER the code address is
     * consumed as an INTEGER OFFSET (`ptrtoint` of the callee, added to the pointer), proven from
     * --no-opt IR. On a non-pointer destination the operator is not pointer arithmetic at all -
     * `string +=` is concatenation - so the offset wording would be false there.
     */
    std::string DescribeCodeValueAsCompoundOperand(const std::string& spelling, const std::string& op,
                                                   bool destIsPointer) const;

    std::pair<std::vector<NamedVariable>, FunctionSymbol> ComputeOverloadFunction(std::vector<std::pair<std::vector<NamedVariable>, FunctionSymbol>> candidates) const;

    // How a call site's arguments bind to declared parameter slots (see ComputeArgumentPositions).
    struct ArgumentBinding
    {
        bool Ok = false;
        bool UnknownName = false;    // a named argument matched no parameter
        bool DuplicateName = false;  // two named arguments claimed the same parameter
        std::string FailedName;      // the offending name for the two flags above
        // PosMap[i] = declared parameter index for call argument i. A variadic overflow
        // argument gets an index at or past the last declared parameter.
        std::vector<int64_t> PosMap;
    };

    /*
     * Bind call-site arguments to declared parameter slots. Two passes: named arguments claim
     * their parameter by name, then the unnamed ones fill the remaining slots left to right
     * (for a variadic target, the overflow lands on trailing slots). `argNames[i]` is "" for a
     * positional argument; `firstTarget` skips leading parameters the call site does not supply
     * (an implicit 'this'). Pure - it reports failure through the result instead of diagnosing,
     * so a probing caller and a diagnosing one can share one implementation. This is also the
     * only way to learn an argument's DECLARED parameter before its value exists (lambda-literal
     * type seeding), which is why it is separate from MatchFunction.
     */
    static ArgumentBinding ComputeArgumentPositions(const std::vector<std::string>& argNames,
        const std::vector<TypeAndValue>& targetArguments, bool isVariadic, size_t firstTarget = 0);

    // `probe` = scoring one candidate of an overload set: a mismatch only disqualifies THIS
    // candidate, so it returns {} instead of reporting - a losing candidate must never error.
    std::vector<LLVMBackend::NamedVariable> MatchFunction(const std::vector<LLVMBackend::NamedVariable>& inputArguments, const std::vector<LLVMBackend::TypeAndValue>& targetArguments, bool isVariadic = false, bool probe = false);

    // Emit LLVM atomic IR for __atomic_* builtins called from atomic.cb.
    // Returns nullptr when name is not an atomic builtin (caller falls through to normal call).
    llvm::Value* TryEmitAtomicBuiltin(const std::string& name, const std::vector<llvm::Value*>& args);

    llvm::Value* CreateOverloadedFunctionCall(const std::string& functionNameIn, const std::vector<LLVMBackend::NamedVariable>& arguments, bool forceRoot = false);

    llvm::Function* GetFunction(const std::string& functionName);

    // Like GetFunction, but prefers non-method (top-level) overloads.
    // Used when assigning a named function to a function<T> variable to avoid
    // picking a struct method that shares the same plain name.
    // Returns true if any overload of `functionName` has the given param count AND its per-param IsMove
    // flags match `expectedParams`. Used to validate funcptr-assignment compatibility with `move` modifiers.
    bool HasFunctionWithMoveFlags(std::string functionName, const std::vector<TypeAndValue::FuncPtrParam>& expectedParams) const;

    /*
     * `destSig` is the DESTINATION's declared function-pointer type, when the caller has one. It
     * is proof: an overload the comparator proves is a different function type cannot be entered
     * through this pointer, because an indirect call has no conversion site. Until it was
     * consulted, this function selected on name, arity and `move` flags alone and returned its
     * candidate unconditionally - `function<int(int)> g = dd;` with `double dd(double)` entered
     * `dd` as an `int(int)` and printed a plausible wrong number, and a WIDER pointee
     * (`void(C2*)` into a `void(S2*)` slot) read past the end of the caller's object.
     *
     * One-sided over the overload set: refused only when EVERY overload is refuted, so a name the
     * comparator cannot resolve keeps binding exactly as before.
     */
    llvm::Function* GetFunctionForFuncPtr(std::string functionName, int expectedParamCount = -1,
                                          const std::vector<TypeAndValue::FuncPtrParam>* expectedParams = nullptr,
                                          const TypeAndValue* destSig = nullptr);

    NamedVariable GetLocalVariable(const std::string& name);

    // Resolve a bare identifier to a local or parameter with correct LEXICAL precedence:
    // innermost frame first, and within a single frame a local (namedVariable) shadows a
    // parameter (functionArgument). This is what lets a lambda / nested-function PARAMETER
    // shadow a same-named local of an ENCLOSING function. Consulting GetLocalVariable and
    // GetFunctionArgument in sequence cannot express this - the former walks EVERY frame's
    // namedVariable before any functionArgument is seen, so an enclosing local wrongly wins
    // over the inner param (a module-verification failure: the inner function loads the
    // enclosing alloca, which does not dominate). Returns an empty NamedVariable if unbound.
    NamedVariable GetScopedLocalOrArgument(const std::string& name);

    bool IsFunctionParameter(const std::string& name) const;

    // Find the implicit 'this' argument in a function-argument map.
    // The this parameter is named "<StructName>__" (trailing double-underscore).
    // Returns end() when not in a member function context.
    static auto FindThisArgIt(const std::map<std::string, NamedVariable>& args)
        -> std::map<std::string, NamedVariable>::const_iterator;

    /// <summary>
    /// Get the member variable from a member function.
    /// </summary>
    NamedVariable GetMemberVariable(const std::string& name);

    /// Returns the implicit 'this' NamedVariable when calling a bare member function
    /// from within a member function body of the same struct. Returns a default
    /// NamedVariable (Storage == nullptr) if not in a member context or not a method.
    NamedVariable GetCurrentMemberThis(const std::string& functionName);

    // Returns the implicit 'this' pointer NamedVariable for the innermost enclosing
    // member-function body, or a default (Storage == nullptr) when not in a method.
    // The result loads to the struct pointer (TypeName = struct, Pointer = true).
    NamedVariable GetThisPointer();

    NamedVariable GetFunctionArgument(std::string name);

    // Returns the 0-based stack depth where name is declared, SIZE_MAX if not found.
    // Searches both functionArgument and namedVariable in each frame (innermost first).
    size_t FindVariableScopeDepth(const std::string& name) const;

    // Returns the name of a live bonded local that currently borrows from sourceName, or "" if none.
    std::string FindActiveBondBorrower(const std::string& sourceName) const;

    // Clears the bond on a local variable (called when the bonded variable is reassigned).
    void ClearVariableBond(const std::string& name);

    void MarkVariableMoved(const std::string& name);

    // Mark a local whose ownership was boxed into an interface ('IFace x = ptr'). Distinct from
    // the generic moved flag: deleting such a source is a no-op that leaks (the interface owns the
    // object now and interface locals are not auto-destructed), so 'delete ptr' is rejected with a
    // targeted message. Plain moves (into 'move' params, view aliases) do NOT set this.
    void MarkVariableMovedIntoInterface(const std::string& name);

    // Record (declaration) or permanently clear (any later plain '=' reassignment) whether an
    // array-view LOCAL was bound from stack/global fixed-array storage - see
    // ViewOfFixedArrayStorage on NamedVariable. Deliberately NOT flow-sensitive: a reassignment
    // is walk-order over the AST, not control flow, so it cannot tell "this branch reassigns
    // and returns" from "this branch reassigns and falls through to the delete". Once a local
    // is EVER reassigned its declaration-time provenance can no longer be trusted on every
    // path, so it is cleared for good rather than recomputed from the new RHS - proven-reject
    // only, never proven-accept from a later assignment.
    void SetViewOfFixedArrayStorage(const std::string& name, bool value, const std::string& sourceName = {});

    /*
     * Record at a BINDING SITE whether an interface local now holds a box the frame only BORROWS
     * (see BorrowedInterfaceBox). Called with `borrowed == true` only when the boxing site PROVED
     * it; every unresolvable provenance arrives as false.
     *
     * Exactly ONE of the two flags is sticky, and only the ACCEPT-direction one. A not-proven
     * binding sets InterfaceBoxProvenanceUnknown, which is never cleared, so no later site can
     * re-arm the rejection - the same one-way rule SetViewOfFixedArrayStorage uses. BorrowedInterfaceBox
     * itself is NOT sticky: a later not-proven store clears it below, which is what keeps
     * `IShapeB s = p; s = new SqMove(); delete s;` compiling. The cost of the sticky half is the
     * reverse order - `IShapeB s = new Ci(); if (k) { s = p; } delete s;` stays accepted - and that
     * is deliberate: walk order over the AST is not control flow, so a binding seen earlier in the
     * text may not be the one that reaches the delete. A null box is neither proven nor unproven -
     * it owns nothing and deleting it is a no-op - so callers skip this call entirely for one.
     */
    void SetInterfaceBoxIsBorrowed(const std::string& name, bool borrowed,
                                   const std::string& sourceName = {});

    /*
     * A plain '=' into a POINTER binding retires the declaration-time facts about WHO frees what
     * it points at. "This is a borrowed parameter" and "this borrows a container's element" are
     * both true of the DECLARATION and false the moment the binding is pointed somewhere else -
     * `int g(Ci* p) { p = new Ci(); ... }` makes the frame the sole owner.
     *
     * Retirement is NOT unconditional: when the RHS is itself a binding that PROVES another owner
     * (`p = q;` between two borrowed parameters), the proof is PROPAGATED rather than dropped, or
     * the store would launder a borrow into an unblamable one. `inheritedOwner` carries that
     * proof's rendered owner name; empty means the RHS proved nothing and the facts really do go
     * stale. The inherited proof is refreshed on EVERY '=', so it never outlives its own store.
     *
     * `coalesceJoin` marks a '??=' rebinding, whose handler returns before the element-borrow
     * refresh. All three fields are written unconditionally so a later plain '=' clears whatever a
     * '??=' left behind.
     */
    void MarkPointerRebound(const std::string& name, const std::string& inheritedOwner = {},
                            bool coalesceJoin = false, bool reboundToOwnedValue = false);

    // Re-arm the join proof on a pointer binding a '?:' / '??' join was just stored into. Always
    // called AFTER MarkPointerRebound, which clears it; empty `owner` leaves it retired.
    void SetJoinKeepsOwner(const std::string& name, const std::string& owner,
                           const std::vector<llvm::Value*>& slots);

    /*
     * Record on a pointer binding that a plain '=' just stored a BORROW into it, so a later
     * `delete` (or a store into an owning slot) is rejected exactly as the declaration spelling
     * `T* d = p;` already is. The declaration path records the same four facts; this is its '='
     * counterpart, and the recording direction is the safe one - a conditional store may
     * over-record and reject, which is preferable to laundering a double free.
     */
    void RecordAssignBorrow(const std::string& name, const std::string& origin,
                            const std::string& uniqueField, bool throughField,
                            bool keepExistingOrigin = false,
                            bool uniqueFieldViaCall = false);

    /*
     * Drop a borrow that a plain '=' recorded (never a declaration-time one) when a later '='
     * stores a provably OWNED value into the same binding from the SAME basic block. Same-block is
     * the whole proof: any path that ran the borrow store also ran this one, so the binding cannot
     * still hold the borrow. Across blocks the borrow stands - `d = p; if (c) { d = new T(); }`
     * leaves a path where it does not, the hazard tracked as
     * conditional-store-retires-borrow-facts-unconditionally.
     */
    void RetireAssignBorrow(const std::string& name);

    // True when `slot` IS a function argument's own storage. Identity, never spelling: a local that
    // merely SHARES a parameter's name is a different binding and must not be classified as one.
    bool IsFunctionParameterStorage(const llvm::Value* slot) const;

    void MarkVariableUnmoved(const std::string& name);

    // Marks a thin pointer local as explicitly-moved-and-null in the CURRENT block (see
    // ExplicitlyMovedNull on NamedVariable). No-op once AddressEscaped latches - an escaped
    // address may have rewritten the pointee, so the deref guard can never be sound again.
    // Deliberately does not touch IsMoved: plain reads of this variable must stay legal.
    void MarkVariableExplicitlyMovedNull(const std::string& name);

    // Reassigning the local makes it live again - clears the deref guard set by an explicit move.
    void MarkVariableNotExplicitlyMovedNull(const std::string& name);

    // '&name' escaped the address - latch off the deref guard permanently (see AddressEscaped
    // on NamedVariable): the pointee may be rewritten through the escaped pointer from here on.
    void MarkVariableAddressEscaped(const std::string& name);

    // The full explicit-move-null deref predicate: fires only for a DEREFERENCE in the SAME
    // block the move was recorded in (never across a branch/merge/loop back-edge - see
    // ExplicitNullBlock), never once the address has escaped (see AddressEscaped), and never
    // while lowering a '?:' arm in the EAGER constant-context form (see
    // suppressExplicitNullDerefGuard_ - only there do both arms run unconditionally; the normal
    // in-function lowering branches, so its arms are separate blocks and need no suppression).
    // This is deliberately narrower than a real null-narrowing analysis - see the field comment.
    bool IsExplicitlyMovedNullHere(const NamedVariable& nv) const;

    // Cross-block half: log this dereference for the MAY-null fixpoint. Only whole-local owning
    // pointers/interfaces can ever receive a SetNull event, so nothing else is worth logging.
    void RecordNullDerefFor(const NamedVariable& nv, int line, int col);

    // Per-field move tracking. Moving a struct sub-path (e.g. `node->left`) into a 'move'
    // parameter must mark ONLY that field as moved, not the whole base variable - otherwise
    // a sibling access (`node->right`) or the base itself is wrongly rejected as use-after-
    // move. This is what lets legitimate recursive owning-pointer-tree code compile.
    void MarkVariableFieldMoved(const std::string& name, const std::string& field);

    // Reassigning a field (`node->left = ...`) makes it live again.
    void MarkVariableFieldUnmoved(const std::string& name, const std::string& field);

    // Compile-time use-after-move subject for a (possibly field-access) variable. Returns the
    // name to report if the variable - or, for a field access, the specific field - was moved,
    // or an empty string if it is still live. A field-access NamedVariable is built from the
    // base variable's scope lookup, so it inherits both IsMoved and MovedFields: a fully-moved
    // base poisons all field reads, while a single moved field poisons only that field.
    std::string MovedUseSubject(const NamedVariable& nv) const;

    // --- Move-dataflow event recording (Stage 1: engine, verify-only). ---
    // Each helper tags the event with the block currently being emitted into; when no block
    // is active (e.g. during the ForwardRefScanner pre-pass) the event is dropped.
    void RecordMoveEvent(movedf::EventKind kind, const std::string& name,
                         const std::string& field, int line, int col);
    void RecordMoveKill(const std::string& name);
    void RecordMoveKillField(const std::string& name, const std::string& field);
    void RecordMoveGenRevive(const std::string& name);
    void RecordMoveGenReviveField(const std::string& name, const std::string& field);
    // A fresh binding also re-initializes the null-state: a local declared inside a loop body is
    // live again on every iteration, so its previous iteration's explicit move must not carry.
    void RecordMoveGenBind(const std::string& name);
    void RecordMoveUse(const std::string& name, const std::string& field, int line, int col);

    // --- Explicit-move null-state event recording (cross-block deref diagnostic). ---
    // Same block-tagging discipline as RecordMoveEvent: dropped when no block is being
    // emitted into (e.g. the ForwardRefScanner pre-pass).
    void RecordNullEvent(nulldf::EventKind kind, const std::string& name, int line, int col);
    void RecordNullSet(const std::string& name);
    void RecordNullClear(const std::string& name);
    void RecordNullEscape(const std::string& name);
    // Any NON-dereference read. Recorded broadly on purpose: an unrecorded read is the only way
    // a guard could slip past the read-kill, and an extra one only costs a diagnostic.
    void RecordNullRead(const std::string& name);
    void RecordNullDeref(const std::string& name, int line, int col);

    // Drop a function's null-state log without analyzing it (an aborted body has a partial CFG).
    void DiscardNullDerefEvents(llvm::Function* F);

    // Solve the MAY-null fixpoint for ONE function as soon as its body is fully lowered, so the
    // resulting error still lands inside an enclosing scoped expect_error block. Reports the
    // earliest dereference of a maybe-null explicitly-moved local (LogError).
    void RunNullDerefDataflow(llvm::Function* F);

    /*
     * A store of null/undef/zero into the returned slot is ACCEPT evidence. Treating it as
     * merely NEUTRAL was tried and rejected in review: it false-rejects legal programs that
     * null the slot before returning (`r = probe as IShape; ...; r = nullptr; return r;`
     * always returns null, yet the slot's only non-null writer is a frame box). That is the
     * "does a frame box MAY-reach the return" question this design exists to avoid, arriving
     * through the back door - the null store IS the CFG edge the rule refuses to look at.
     * With this true the rule stays purely existential: reject only when EVERY writer of the
     * slot is a frame box. The cost is missing a dangle in any function that also nulls the
     * slot, which is today's behaviour and the acceptable side of the asymmetry.
     */
    static constexpr bool kNullStoreIsAcceptEvidence = true;

    // Resolve the pending interface-return-dangle checks for ONE function as soon as its body
    // is fully lowered (same hook as RunNullDerefDataflow, right beside it) - the CFG is
    // complete, so every store to the slot exists and every back-edge is wired.
    //
    // For each pending record, this is an EXISTENTIAL question over the slot's COMPLETE
    // use-list, never a reachability query: does the slot have a writer that is a frame box
    // (TAINT), and does it have NO user that proves otherwise (ACCEPT evidence - a non-frame
    // store, or any use this analysis does not explicitly recognize). Rejecting requires both
    // taint AND a total absence of accept evidence; any unrecognized shape is accept evidence,
    // so an unrecognized shape can only suppress a rejection, never manufacture one - a false
    // rejection cannot come from missing a case.
    void RunInterfaceReturnDangleCheck(llvm::Function* F);
    void RunDeferredEndOfBodyChecks(llvm::Function* F);

    /*
     * True when `slot`'s address provably never leaves the frame, so the only writes to it are
     * the direct stores this analysis can see. Every user must be a plain load, a store INTO the
     * slot, an all-constant GEP (walked RECURSIVELY, since a field or element receiver reaches
     * its fat pointer through two or more nested GEPs), or a debug / lifetime marker.
     * Deliberately NOT CallIsPointerOpaqueIntrinsic: that helper also admits llvm.mem*, and a
     * memcpy into the slot is a real write this walk would then miss.
     * Anything else answers false, which only ever suppresses a rejection.
     */
    bool InterfaceSlotIsFrameLocal(const llvm::Value* slot) const;

    /*
     * MAY-write test over the (Base, Path) location: true when this store's destination is Base
     * itself, or a constant GEP off Base whose path is a prefix of, equal to, or an extension of
     * Path. The destination's own path is returned so the caller can tell a covering write from a
     * partial one. A store through a NON-constant GEP off Base cannot reach here - the escape walk
     * above has already answered false for that base - so it simply does not match.
     */
    static bool StoreWritesInterfaceLoc(const llvm::StoreInst* st, const llvm::AllocaInst* base,
                                        llvm::ArrayRef<uint64_t> path,
                                        llvm::SmallVectorImpl<uint64_t>& storePath);

    /*
     * Metadata tag on the compiler-emitted zero splat of an uninitialized `unique` interface
     * local's slot. That store exists so the drop-old and the scope-exit release do not run
     * through garbage; it is NOT a user initialization, so every null-interface proof skips it
     * and the declaration keeps witnessing "never assigned an implementation".
     */
    static constexpr const char* kIfaceDeclSplatMD = "cflat.iface.declsplat";

    /*
     * The constant a store's value operand provably lands in memory: the value itself, or the
     * single constant `ret` of a directly-called DEFINED function - which is how a synthesized
     * default constructor supplies a struct's null interface field. Anything else answers null,
     * which accepts.
     */
    static const llvm::Constant* NullIfaceStoredConstant(const llvm::Value* v);

    /*
     * Does this store touch the (base, path) location, and does it leave a PROVABLE null there?
     * A write covering a prefix of the path, or the path exactly, is a full write of the
     * location and is classified from the constant it lands. A write at a LONGER path touches
     * only part of the fat pointer, so it leaves the location unproven - which accepts.
     */
    static bool NullIfaceStoreAffectsLoc(const llvm::StoreInst* st, const llvm::AllocaInst* base,
                                         llvm::ArrayRef<uint64_t> path, bool& leavesNull);

    // Per-block summary of one storage location: the LAST store in that block that writes it,
    // and whether that store leaves the location provably null.
    struct NullIfaceLocFacts
    {
        std::unordered_map<const llvm::BasicBlock*, std::pair<const llvm::StoreInst*, bool>> ByBlock;
        bool AnyNullWrite = false;
    };

    // Per-function CFG data shared by every record in one function, built lazily. The block
    // order costs O(B); the control-dependence closure costs O(B^2) and is built only once a
    // record has actually been proven definitely-null at its access, which correct code never
    // reaches - that is this analysis's counterpart of nulldf's 'haveSet' fast path.
    struct NullIfaceCfgInfo
    {
        llvm::Function* Fn = nullptr;
        std::unordered_map<llvm::BasicBlock*, int> Rpo;
        std::vector<llvm::BasicBlock*> Blocks;
        bool HaveCd = false;
        std::unordered_map<llvm::BasicBlock*, nulldf::CdSet> Cd;
    };

    /*
     * One pass over F classifying every store against every candidate location at once, so the
     * cost is O(instructions + stores * locations) rather than a full walk per record.
     */
    static std::vector<NullIfaceLocFacts> CollectNullIfaceLocFacts(
        llvm::Function* F, const std::vector<const PendingNullIfaceDispatch*>& live);

    void EnsureNullIfaceBlocks(NullIfaceCfgInfo& cfg, llvm::Function* F);

    /*
     * Cross-block half of the definitely-null interface proof. A forward MUST fixpoint over one
     * storage location: definitely-null at a block only when definitely-null on EVERY
     * predecessor, so a value assigned on any one path leaves the access accepted. This is
     * deliberately NOT nulldf's lattice - that one unions witness sets, which answers "was this
     * moved out on some path" and would reject a receiver assigned inside a branch or a loop.
     *
     * Blocks are seeded optimistically (null) except the entry, which starts unproven; the
     * greatest fixpoint of an AND-meet framework is the meet-over-all-paths answer here, since
     * every transfer function is a constant or the identity.
     *
     * On top of the lattice the control-dependence containment test still runs: a report needs
     * CD*(access) to be a subset of CD*(M) for EVERY block M establishing the null-ness, which
     * is what keeps a guarded or run-time-skippable access compiling. 'every' rather than
     * nulldf's 'some' is the conservative direction, since more suppression means more accepts.
     */
    bool CrossBlockProvesNullIface(llvm::Function* F, const PendingNullIfaceDispatch& rec,
                                   const NullIfaceLocFacts& facts, NullIfaceCfgInfo& cfg);

    /*
     * Straight-line half of the proof, unchanged: the last write covering the receiver's
     * location at or before the dispatch, within the dispatch's OWN block, lands a null
     * constant. A block has one entry and no branch inside it, so that write is exactly what
     * the dispatch reads.
     */
    bool SameBlockProvesNullIface(const PendingNullIfaceDispatch& rec) const;

    // LogError THROWS, so this never returns on the reporting path.
    void ReportNullIfaceAccess(const PendingNullIfaceDispatch& rec);

    /*
     * A DIFFERENT question from ReportNullIfaceAccess: not "proven null", but "this location has
     * no store anywhere in the function at all" - a bare interface local with no initializer
     * (`PLive lv;`), never assigned, never defaulted, so the alloca is genuinely uninitialised
     * memory rather than a proven-null fat pointer. Wording must say so honestly; the "last set to
     * null" phrasing of ReportNullIfaceAccess would be factually false here. LogError THROWS.
     */
    void ReportNullIfaceUninitAccess(const PendingNullIfaceDispatch& rec);

    /*
     * Resolve the pending definitely-null interface dispatches for ONE function, at the same
     * end-of-body hook as RunInterfaceReturnDangleCheck. Rejecting requires proving all three:
     * the base never escapes, a write covering the receiver's location reaches the access with
     * no assignment of an implementation on ANY path in between, and the value that write lands
     * at the location is a null constant. The straight-line proof answers within the access's
     * own block; the cross-block MUST fixpoint answers when control flow sits between the null
     * init and the access, and adds the control-dependence containment test so a guarded or
     * run-time-skippable access still compiles. Any gap anywhere leaves the program compiling.
     *
     * A second, separate check runs after: a receiver whose location has NO covering store
     * ANYWHERE in the function (facts.ByBlock empty) - not "proven null", genuinely never
     * initialised - and whose path is empty (the whole receiver, not a sub-object; a sub-object
     * of an aggregate is always reached through a synthesized constructor call and covered above
     * or left alone, per the do-not-widen list). MUST-uninit only: a store on ANY path (a branch,
     * a loop body, an unconditional assignment) disqualifies it, which is the conservative,
     * false-negative-safe direction for the same reason the null proof's cross-block half is a
     * MUST lattice, not a MAY one.
     */
    void RunNullIfaceDispatchCheck(llvm::Function* F);

    /*
     * True when NOTHING in this module can write `gv`: every user, transitively through GEPs, is
     * a plain load or a debug / lifetime marker. A store (into the global, or of its address
     * elsewhere), a call taking its address, an atomic, a constant referencing it, or any user
     * this walk does not recognise all answer false - which accepts. This is deliberately
     * stricter than InterfaceSlotIsFrameLocal, which permits stores INTO the slot: here a single
     * store anywhere in the module is exactly the fact that must not exist.
     */
    bool InterfaceGlobalNeverWritten(const llvm::GlobalVariable* gv) const;

    /*
     * Module-end half of the definitely-null interface proof: receivers that ARE a global.
     * Rejecting needs TWO independent facts, and neither alone is sufficient:
     *
     *  1. Whole-module "never assigned" - the initializer is null and no store anywhere in the
     *     module writes the global or any GEP off it, and its address never escapes.
     *  2. Control-dependence containment - the null witness is synthesised at the accessing
     *     function's ENTRY block, since the global is null on entry to every function. An access
     *     control-dependent on anything the entry block is not (a guard, a run-time-skippable
     *     branch) fails the subset test and compiles.
     *
     * Fact 1 alone is unsound: `if (g == nullptr) {} else { g.Get(); }` has no store anywhere
     * and the else arm is correct code. Fact 2 alone is unsound: a global assigned in another
     * translation unit has no in-module guard at the access.
     */
    void RunNullIfaceGlobalCheck();

    // Solve the MaybeMoved fixpoint per llvm::Function over the emitted module. This is the
    // source of truth for loop-carried / cross-block / switch use-after-move (the inline
    // linear checker owns straight-line + if/else and aborts before this runs). On any
    // divergence, report the globally earliest (line, col) as a real error via LogError.
    void RunMoveDataflow();

    // Mark a pre-declared string local as owning its heap buffer. Used when a plain
    // assignment (`s = expr`) stores an owned heap string into an already-declared
    // string local: the local now owns the buffer and must free it on scope exit
    // (mirrors the IsOwningString propagation in the declaration-with-initializer path).
    void MarkVariableOwningString(const std::string& name);

    // Snapshot of per-variable and per-field move state across active scopes (used to keep
    // move tracking sound across if/else branches).
    struct MovedStateSnapshot
    {
        std::map<std::string, bool> moved;
        std::map<std::string, std::unordered_set<std::string>> movedFields;
    };

    // Snapshots the IsMoved flag and per-field moved set for all variables in all active scopes.
    MovedStateSnapshot SaveMovedState() const;

    // Restores the move state from a snapshot (only for variables still in scope).
    void RestoreMovedState(const MovedStateSnapshot& state);

    // ORs a snapshot into the LIVE state: a variable (or field) moved in the snapshot becomes
    // moved now. Used to fold each break/continue path's moves back in at loop exit.
    void MergeMovedStateInto(const MovedStateSnapshot& state);

    // Merges two post-branch states: a variable (or field) is moved if it was moved in either branch.
    void MergeMovedStates(const MovedStateSnapshot& thenState,
                          const MovedStateSnapshot& elseState);

    llvm::GlobalVariable* GetGlobalVariable(const std::string& name);

    NamedVariable GetGlobalVariableNV(const std::string& name);

    llvm::Constant* GetPlatformConstant();

    void SetCompileTimeMacro(const std::string& name, llvm::Constant* value, const std::string& type);

    void SetPlatformMacros();

    CompileTimeMacro GetCompileTimeMacro(const std::string& name);

    StructData GetDataStructure(const std::string& structName);

    StructData GetDataStructure(llvm::StructType* structType);

    // Mirror of ApplyAbiAttributes but for a CallInst. LLVM's verifier requires that the
    // sret / byval attributes appear on both the function declaration AND every call site.
    void ApplyAbiCallAttributes(llvm::CallInst* ci, const AbiRecipe& recipe);

    // Emit a C-extern call when the resolved overload's signature contains struct-by-value
    // params or return. argList holds the CFlat-natural argument values (struct values are
    // passed as LLVM struct values). This rewrites them into the lowered ABI shape:
    //   - CoerceToInt param: alloca + store the struct, load back as iN, pass the iN.
    //   - ByVal param: alloca + store the struct, pass the alloca pointer (with byval attr at call site).
    //   - SRet return: alloca a return slot, prepend its pointer as arg 0, after the call
    //     load the struct from the slot.
    //   - CoerceToInt return: receive the iN, store into a temp alloca, reload as struct.
    llvm::Value* EmitAbiLoweredCall(const FunctionSymbol& candidate, std::vector<llvm::Value*>& argList);

    // Load a value of type coerceTy from byte offset byteOff within an alloca'd struct slot,
    // reinterpreting the underlying bytes (used to read SysV eightbytes out of a struct).
    llvm::Value* LoadCoerceAt(llvm::Value* structSlot, llvm::Type* coerceTy, uint64_t byteOff);

    // Store val into byte offset byteOff within an alloca'd struct slot, reinterpreting the
    // bytes (used to scatter SysV eightbytes returned in registers back into a struct).
    void StoreCoerceAt(llvm::Value* structSlot, llvm::Value* val, uint64_t byteOff);

    llvm::Value* CreateFunctionCall(llvm::Function* func, const std::vector<llvm::Value*>& arg);

    // Returns true if value is a load from an owning alloca in any live scope.
    bool IsOwningValue(llvm::Value* value) const;

    // The live binding whose Storage is `slot`, or empty. Recovers the NAME from the VALUE, so a
    // spelling that erased the binding can still reach the name-keyed move bookkeeping.
    // The binding behind a slot, for guards that need its ownership flags and not just its name.
    // Same search order as FindVariableNameByStorage; null when the slot is not a live binding.
    const NamedVariable* FindVariableByStorage(const llvm::Value* slot) const;

    /*
     * Has a plain `=` since the declaration made this binding the SOLE owner of what it now holds, so
     * the "someone else frees this" proof no longer applies? Two things must hold, and PointerRebound
     * on its own is NEITHER of them - it means "was assigned to", so it is equally set by `b = q;`
     * between two borrows and by a self-assign. (1) The store's RHS was a provably OWNED value, which
     * is the `=` path's own srcIsOwnedPtrRhs, recorded by MarkPointerRebound. (2) The store was in the
     * SAME basic block this move is being emitted into - within one block, walk order IS execution
     * order, so the store certainly ran. Across blocks it is unprovable (a never-taken
     * `if (b == nullptr) { b = new T(); }` is the standing counterexample, tracked as
     * conditional-store-retires-borrow-facts-unconditionally), so the proof is kept and the move stays
     * rejected - the safe direction, since declining to retire can only reject, never launder.
     */
    bool BorrowProofRetiredByRebind(const NamedVariable& nv) const;

    /*
     * True when a plain copy of an owning local (BorrowsOwningLocal) STILL aliases a live binding
     * that owns the object. Both ends retire the fact: rebinding the COPY makes it the sole owner
     * of what it now holds, and rebinding the SOURCE leaves the copy holding the only reference to
     * the original. An unresolvable or dead source answers false - the accept direction.
     */
    bool OwningLocalCopyStillAliases(const NamedVariable& nv) const;

    std::string FindVariableNameByStorage(const llvm::Value* slot) const;

    /*
     * The PROVABLE negative of IsOwningValue, for guards whose false positive is a false rejection.
     * IsOwningValue answers only a LoadInst, so its `false` conflates "this binding does not own"
     * with "this value is not a load at all" - keying a rejection off it rejects legal code (that
     * is exactly how a '?:' phi came to be rejected as not-owned). This answers true ONLY when the
     * loaded slot IS a live binding that declares itself non-owning. A call result, a phi, a field
     * GEP, an unresolvable slot: all answer false, i.e. "cannot tell", and are left accepted.
     */
    bool IsProvablyNonOwningPointerLoad(llvm::Value* value) const;

    // True when `storage` is the slot of a plain (borrow) string PARAMETER - a `string s`
    // argument this frame does NOT own. The slot holds a {ptr,len} copied by value from the
    // caller with the runtime OWNED bit intact (correct: the callee must not free a borrow),
    // so a passthrough `return s;` must hand back a BORROW, not a move: its Storage is an
    // alloca (looks like a movable whole-local), but the caller still owns the buffer.
    // A `move string s` param (IsOwningString) or any owning local is excluded.
    bool IsBorrowStringParamStorage(llvm::Value* storage);

    // `returnedLocalStorage`, when provided, is the alloca of the named local being
    // returned (the return expression's NamedVariable.Storage). It lets the struct-return
    // move detection below work even when the by-value return is materialized field-wise
    // (insertvalue) rather than as a single `load %Struct`, which dyn_cast<LoadInst> misses.
    void CreateReturnCall(llvm::Value* value, llvm::Value* returnedLocalStorage = nullptr, const std::string& interfaceReturnStructName = "");

    void BeginAutoReturnCapture();
    std::vector<AutoReturnSite> EndAutoReturnCapture();
    bool IsAutoReturnCaptureActive() const;

    // Best-effort reverse mapping from an LLVM type to a CFlat TypeName, used by
    // 'auto' return-type inference to populate the function table's ReturnType
    // after the unified return type is known. Falls back to "i64" so the entry
    // is never left empty.
    // Convenience: just the CFlat type name from an LLVM type. Useful for generic
    // argument inference where the argument's TypeName has been stripped but its
    // LLVM type still describes the underlying primitive.
    std::string LlvmTypeToTypeName(llvm::Type* t) const;

    TypeAndValue LlvmTypeToTypeAndValue(llvm::Type* t) const;

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
        bool isMethod);

    llvm::BasicBlock* GetElseBlock();

    // Swap the elseBlock in the innermost frame that owns one.
    // Returns the previous value so the caller can restore it.
    // Used by the '!' operator to prevent inner && from short-circuiting
    // directly to the outer false-branch (which would bypass the negation).
    llvm::BasicBlock* ExchangeElseBlock(llvm::BasicBlock* newBlock);

    bool IsBlockTerminated();

    // A `return` / `break` / `continue` inside a NESTED scope (a plain compound block or an
    // `if const` arm - both inline their statements into the ENCLOSING block) terminates the
    // block the caller is still writing to. Reopen emission in a fresh, predecessor-less block
    // so any statements that follow form valid - if unreachable - IR instead of instructions
    // after a terminator. Destructors already ran on the real return path (CreateReturnCall).
    void ReopenAfterTerminator();

    // True when v is a compile-time-constant non-zero integer - the guard of an
    // infinite loop such as `while (true)` / `while (1)`. Control can only leave
    // such a loop via `break`; it never falls through the condition.
    bool IsConstantTruthy(llvm::Value* v);

    // True when the current insert block is unreachable: a non-entry block with
    // no predecessors (nothing branches to it). Used by fall-through analysis so
    // that the dead exit of an infinite loop (e.g. `while (true)` with no break)
    // is not mistaken for a live path that must end in a return.
    bool IsCurrentBlockUnreachable();

    void CreateBreakCall();

    void CreateContinueCall();

    // Stamp `!llvm.loop !{!"llvm.loop.vectorize.enable", i1 true}` on the latch
    // branch of the loop just emitted, and record it for post-optimization
    // enforcement. Called for `vectorize` loops; the back-edge must be the
    // terminator of the current insert block (true for while/for latches here).
    // Forcing vectorize.enable makes LLVM emit an explicit failure diagnostic
    // when the loop cannot be vectorized, which OptimizeModule turns into an error.
    void AttachVectorizeHintToCurrentLatch(int sourceLine);

    std::string GetSourceFileName() const;
    std::string GetSourceFilePath() const;

    // The path that identifies the file currently being walked, for def-site identity. The LSP
    // analyzes a temp copy of the open document, so the root file's own path is a throwaway name
    // in %TEMP%; report the real document instead, or the copy reads as a second definition of
    // everything the real file already defines (it is imported under its real path).
    std::string DefinitionSitePath() const;

    std::string GetCurrentFunctionName() const;

    void RegisterReturnBlock(const std::string& name, CFlatParser::CompoundStatementContext* body, std::vector<DeclTypeAndValue> params, TypeAndValue returnType);

    const ReturnBlockEntry* GetReturnBlock(const std::string& name) const;

    void RegisterNamespace(const std::string& name);
    const std::string& GetCurrentNamespace() const;
    void SetCurrentNamespace(const std::string& name);

    // RAII holder for currentNamespace_. Hand-rolled save/restore pairs do NOT survive a
    // LogError, which THROWS on the batch (--check) and LSP paths: the restore is skipped and the
    // stale namespace steers the next file's generic-template key resolution.
    class NamespaceScope
    {
    public:
        NamespaceScope(LLVMBackend* c, const std::string& name)
            : compiler_(c), saved_(c->GetCurrentNamespace()) { c->SetCurrentNamespace(name); }
        ~NamespaceScope() { compiler_->SetCurrentNamespace(saved_); }
        NamespaceScope(const NamespaceScope&) = delete;
        NamespaceScope& operator=(const NamespaceScope&) = delete;
        const std::string& Saved() const { return saved_; }
    private:
        LLVMBackend* compiler_;
        std::string saved_;
    };
    void RegisterNamespaceAlias(const std::string& alias, const std::string& target);
    void RegisterLocalNamespaceAlias(const std::string& alias, const std::string& target);
    void RegisterEnumBackingType(const std::string& enumName, const std::string& backingType);
    std::string GetEnumBackingType(const std::string& enumName) const;
    bool IsNamespace(const std::string& name) const;
    bool IsImportAlias(const std::string& name) const;
    bool IsImportAliasMember(const std::string& alias, const std::string& member) const;
    bool IsDataStructure(const std::string& name) const;
    std::string ResolveNamespace(const std::string& name) const;

    // Candidate keys for scope-sensitive registries. The declaring scope is captured in the key at
    // registration time; lookup walks the active namespace outward and then the global scope.
    std::vector<std::string> ScopedNameCandidates(const std::string& name, bool forceRoot = false) const;

    /*
     * Resolve a base-clause / parent-list interface spelling to the name the interface is
     * registered under. A bare name walks outward through the enclosing namespaces ("IV" inside
     * "one" -> "one.IV"), a qualified name is canonicalized through namespace aliases. Only an
     * actual interface match is accepted, so a same-named sibling function never hijacks the
     * name; when nothing matches, the spelling is returned so the caller can report it verbatim.
     */
    std::string ResolveInterfaceName(const std::string& spelled) const;

    // Resolves a qualified name (e.g. "MathAdv.MyNumber") to its canonical registered name
    // by expanding namespace aliases on the leading component and then walking up parent namespaces.
    std::string ResolveQualifiedName(const std::string& name) const;

    // forceRoot: skip the enclosing-namespace outward walk and resolve the name starting at
    // the root (file/global) scope. Used by the `global::` scope-escape qualifier so a
    // namespace member that shadows a global (e.g. Math.tan over the CRT tan) can still
    // reach the root symbol.
    std::string ResolveQualifiedName(const std::string& name, bool forceRoot) const;

    std::string GetNameOfCurrentInsertionBlock();

    void DumpCurrentInsertionPoint(std::string prefix = "");

    // If set, disables auto-import of core/runtime.cb
    void SetSkipRuntimeImport(bool v);
    void SetRuntimeDir(const std::string& dir);
    void SetSourceFileDir(const std::string& dir);
    // Overrides the file name shown in diagnostics (and baked into __FILE__) for the next
    // Analyze(). Used by the LSP so errors point at the real document, not the temp copy.
    void SetSourceDisplayName(const std::string& name);
    void SetVerbose(bool v);
    bool IsVerbose() const;
    // Enable AddressSanitizer instrumentation + runtime linking. Best paired with -g.
    void SetAsan(bool v);
    // Enable the ownership sanitizer (M1). Implies -g (forced in Compile()).
    void SetSanitizeOwnership(bool v);
    bool IsSanitizeOwnership() const;
    // Instrument the program with the HeapAudit leak/double-free oracle without source edits.
    void SetHeapAudit(bool v);
    void SetRunMode(bool v);
    bool IsRunMode() const;
    void SetRunArgs(std::vector<std::string> a);
    int  GetJitExitCode() const;
    void SetBatchMode(bool v);
    void SetNoCache(bool v);
    // When true, headers opted into the disk cache (via the `cache` import clause) record and
    // validate every transitively-included file's mtime/hash rather than just the top header.
    void SetCHeaderCacheDeep(bool v);

    void SetXthreadScanLevel(int n);
    int  GetXthreadScanLevel() const;
    bool IsXthreadEscapedType(const std::string& typeName) const;
    void AddXthreadEscapedType(const std::string& typeName);
    void ScanCrossThreadEscapes(CFlatParser::CompilationUnitContext* cu);
    void ReportXthreadFieldAccess(const std::string& varName, const std::string& fieldName,
                                  const std::string& structType, const TypeAndValue& field);

    // severity: 1 = Error, 2 = Warning (matches the LSP DiagnosticSeverity codes).
    using DiagnosticSink = std::function<void(const std::string& file, size_t line, size_t col, const std::string& msg, int severity)>;
    void SetDiagnosticSink(DiagnosticSink sink);

    void SetSymbolSink(LspSymbolIndex* sink);
    LspSymbolIndex* GetSymbolSink() const;

    // LSP-only: grays unreachable/unused code spans. Null during real compiles.
    using HintRegionSink = std::function<void(int startLine, int startCol,
                                              int endLine, int endCol,
                                              const std::string& msg)>;
    void SetHintRegionSink(HintRegionSink sink);
    void ReportHintRegion(int startLine, int startCol, int endLine, int endCol, const std::string& msg);
    bool HasHintRegionSink() const;

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

    static std::string ResolveCLinkLib(const std::string& lib, const std::string& importingFilePath);

    void SetVcpkgExe(const std::string& path);
    void SetVcpkgManifest(const std::string& path);
    void SetVcpkgTriplet(const std::string& triplet);

    std::string RootVcpkgImportPath(const std::string& analyzedPath) const;

    static uint64_t VcpkgDiskCacheKey(const std::string& fileForLsp,
                                       const std::vector<std::string>& defines);

    // Identity of the running cflat binary (mtime + size), computed once. Folded into the
    // C-header disk-cache key so bindings do not survive a compiler that would remap them.
    static std::string CompilerBuildStamp();

    static std::string GetCHeaderCacheDir();

    static uint64_t CHeaderDiskCacheKey(const std::string& fileForLsp,
                                        const std::vector<std::string>& includeDirs,
                                        const std::vector<std::string>& defines,
                                        const std::vector<std::string>& extraDefines);

    static uint64_t CHeaderDiskCacheKey(const std::vector<std::string>& headerPaths,
                                        const std::vector<std::string>& includeDirs,
                                        const std::vector<std::string>& defines,
                                        const std::vector<std::string>& extraDefines);

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

    static nlohmann::json TvToJson(const TypeAndValue& tv);
    static TypeAndValue TvFromJson(const SjVal& j);

    static nlohmann::json SigToJson(const CSigEntry& e);
    static CSigEntry SigFromJson(const SjVal& j);

    static nlohmann::json EnumToJson(const CEnumEntry& e);
    static CEnumEntry EnumFromJson(const SjVal& j);

    static nlohmann::json GlobalToJson(const CGlobalEntry& g);
    static CGlobalEntry GlobalFromJson(const SjVal& j);

    static nlohmann::json FieldToJson(const CRecordFieldEntry& f);
    static CRecordFieldEntry FieldFromJson(const SjVal& j);

    static nlohmann::json RecordToJson(const CRecordEntry& r);
    static CRecordEntry RecordFromJson(const SjVal& j);

    static nlohmann::json MacroToJson(const CMacroEntry& m);
    static CMacroEntry MacroFromJson(const SjVal& j);

    static nlohmann::json FuncMacroToJson(const CFunctionMacroEntry& m);
    static CFunctionMacroEntry FuncMacroFromJson(const SjVal& j);

    static bool CHeaderDepFresh(const CHeaderDep& dep);

    static bool TryLoadCHeaderDiskCache(
        const std::filesystem::path& cacheDir,
        uint64_t diskKey,
        std::filesystem::file_time_type mtime,
        uint64_t contentHash,
        CFileSigCacheEntry& out);

    static void WriteCHeaderDiskCache(
        const std::filesystem::path& cacheDir,
        uint64_t diskKey,
        std::filesystem::file_time_type mtime,
        uint64_t contentHash,
        const CFileSigCacheEntry& entry);

    // Handle `import package-vcpkg "header" from "port[features]";`. Resolves the port
    // through the user-owned vcpkg.json, pushes the resulting include dir / libs / DLLs
    // into the per-backend accumulators, then routes the header through CompileCHeader
    // (the existing C-header binding path). Returns false on hard error.
    bool CompileVcpkgImport(const std::string& importingFilePath,
                            const std::string& header,
                            const std::string& portSpec,
                            const std::vector<std::string>& extraDefines = {});

    // Bind a resolved package header (vcpkg or nuget) through the C-header pipeline with a
    // disk cache. 'headerCanon' is the canonical header path; 'includeDir' locates the sibling
    // .cflat-cache dir. On a disk hit we preload the in-memory cache so CompileCHeader skips
    // clang entirely; on a miss we write after CompileCHeader so the next build is a hit.
    bool BindCanonicalCHeader(const std::filesystem::path& headerCanon,
                              const std::string& includeDir,
                              const std::vector<std::string>& extraDefines);

    // Locate the .pri named by a package-nuget `pri "..."` clause inside the resolved package
    // folder and record its absolute path in deployPriPath_ for deployment as <exe>.pri.
    // Probe runtimes-framework/win-<arch>/native/<name> first, then recursively search the
    // package folder for an exact filename match. Not-found is a hard error (LSP: silent skip).
    // A second pri that resolves to a DIFFERENT path is a conflict error; the same path is ok.
    bool ResolveNugetPri(const std::string& priName,
                         const std::string& packageFolder,
                         const std::string& packageSpec,
                         bool lspMode);

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
                            const std::string& priName = "");

    bool Analyze(const std::string& filePath, const std::vector<std::string>& importDirs, const std::string& runtimeDirPath);
    void ResetForReanalysis();

    /*
        Resolution order: (1) process override set via SetCacheDirOverride (used by
        --init-local), (2) CFLAT_CACHE_DIR env var, (3) <exeDir>/.cflat if it exists as a
        directory, (4) the per-user cache (GetUserCacheDir). Resolved once per process into
        a function-local static memo.
    */
    static std::string GetCflatCacheDir();
    // Which rule GetCflatCacheDir() matched ("override", "CFLAT_CACHE_DIR", "local",
    // "per-user"), for -v reporting. Empty until GetCflatCacheDir() has been called once.
    static std::string GetCflatCacheDirRule();
    // The per-user cache root (%USERPROFILE%\.cflat / ~/.cflat), independent of any
    // process override or local cache. Shared by the resolver and --init-clear.
    static std::string GetUserCacheDir();
    // Process-lifetime override for GetCflatCacheDir(), used by --init-local. Must be set
    // before the first GetCflatCacheDir() call in the process (resolution memoizes once).
    static void SetCacheDirOverride(std::string dir);

    // Populate %USERPROFILE%\.cflat\ with cached linker paths for x64 and x86.
    // Prints discovered paths to stdout. Returns false if the cache dir cannot be created.
    static bool RunInit(const std::string& runtimeDir, bool verbose);

    // Delete the compiler cache tree rooted at `root` (inverse of RunInit for that root).
    // Caller supplies the exact target - this never calls GetCflatCacheDir() itself, so
    // --init-clear (both roots) and --init-clear-local (local root only) stay unambiguous.
    // `label` (e.g. "local", "per-user") is used only in the printed status lines.
    // Prints what was removed. Returns false on a real failure; a missing root is success.
    static bool ClearCacheDir(const std::string& root, bool verbose, const std::string& label);

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
    // The running OS's product version (e.g. "26.5") via sysctlbyname("kern.osproductversion"),
    // or "" on failure. Never shells out - keeps the self-contained-build property.
    static std::string MacHostOsProductVersion();
    // Trimmed contents of <MacStubSyslibroot()>/SDKVersion (the stub's own provenance
    // record), or "" if the stub root or the file is absent.
    static std::string MacStubSdkVersion();
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
