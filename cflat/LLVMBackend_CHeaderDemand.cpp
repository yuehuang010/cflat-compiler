#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include <bit>
#include <cctype>
#include <thread>
#include <fstream>
#include <iterator>
#include <llvm/Support/xxhash.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/Instrumentation/AddressSanitizer.h>
#include <llvm/Object/COFF.h>
#include <llvm/Object/Binary.h>
#include <llvm/Object/Archive.h>
#include <llvm/Object/COFFImportFile.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/TimeProfiler.h>
#include <llvm/Support/Process.h>
#include <llvm/Support/JSON.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticHandler.h>
#pragma warning(pop)
#include <antlr4-runtime.h>

#include "platform/GeneratedParser.h"
#include "LLVMBackend.h"
#include "CxxSpelling.h"
#include "CppStructName.h"
#include "CxxIncrementalGroup.h"
#include "MainListener.h"
#include "GrammarTreeListener.h"
#include "TypeMangling.h"
#include <filesystem>
#include <optional>
#include <algorithm>
#include <cctype>
#include <atomic>
#include <mutex>
#include <chrono>
#include <fstream>
#include <map>
#include <unordered_map>
#include <set>

#if defined(__APPLE__)
// Step 3 (macOS self-contained link): harvest libSystem's exported symbols from
// the live dyld shared cache to synthesize a linker stub, so -o needs no SDK.
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <dlfcn.h>
#include <cstring>
#include <sys/sysctl.h>
#endif

#include "LLVMBackend_CInteropShared.h"

bool LLVMBackend::ExtractCHeaderClang(const std::vector<std::string>& headerPaths,
                             std::vector<CSigEntry>& outSigs, std::vector<CEnumEntry>& outEnums,
                             std::vector<CRecordEntry>& outRecords,
                             std::vector<CMacroEntry>& outMacros,
                             std::vector<CFunctionMacroEntry>& outFuncMacros,
                             std::vector<CGlobalEntry>& outGlobals,
                             std::vector<std::pair<std::string, std::string>>& outAliases,
                             std::vector<CTypeAliasEntry>& outTypeAliases,
                             const std::vector<std::string>& extraDefines,
                             std::vector<std::string>* outIncludes,
                             bool* outPrereqFailure,
                             std::string* outPrereqMsg,
                             bool* outHeaderFailure,
                             bool cxxMode,
                             std::string* outCxxBitcode,
                             std::vector<cflat_cinterop::RawFunctionPointerAbi>* outFunctionPointerAbis,
                             uint64_t* outLongDoubleWidth,
                             bool* outLongDoubleIsIEEEDouble,
                             std::string* outTargetTriple,
                             std::vector<cflat_cinterop::RawFunctionTemplate>* outFunctionTemplates,
                             std::vector<std::pair<std::string, std::string>>* outUsingDirectives,
                             std::vector<std::pair<std::string, std::string>>* outNamespaceAliases,
                             std::vector<std::string>* outClassTemplateNames,
                             std::vector<std::string>* outVariableTemplateNames,
                             std::vector<std::pair<std::string, std::string>>* outClassUsings,
                             std::vector<std::string>* outClassTemplateSpecializations,
                             std::vector<cflat_cinterop::RawClassTemplate>* outClassTemplates,
                             std::vector<cflat_cinterop::RawDynamicInitializer>* outDynamicInitializers)
{
        if (headerPaths.empty()) return false;
        if (outHeaderFailure) *outHeaderFailure = false;

        // The first header's directory anchors the primary -I; also labels TimeTrace scopes
        // and attributes registered decls for LSP go-to-def.
        const std::string& headerPath = headerPaths.front();
        const std::string primaryDir = std::filesystem::path(headerPath).parent_path().string();
        /*
         * A system C++ header (libc++) is a TEMPLATE CATALOG, not a bound surface: walking it would
         * register thousands of implementation details and emit a whole standard library's inline
         * bodies. Nothing is bound at import time; the concrete types the program names are
         * requested one at a time (RequestCxxForeignType), which re-parses this same header set.
         */
        if (cxxMode && IsSystemCxxHeaderPath(headerPath))
        {
            cppInteropUsed_ = true;
            return true;
        }

        std::string source;
        for (const auto& h : headerPaths)
        {
            std::string fwd = h;
            std::replace(fwd.begin(), fwd.end(), '\\', '/');
            source += "#include \"" + fwd + "\"\n";
        }
        source += cxxMode ? "typedef int __cflat_header_scope_sentinel;\n"
                          : "int __cflat_header_scope_sentinel;\n";

        cflat_cinterop::ExtractRequest req;
        if (cxxMode) cppInteropUsed_ = true;
        req.mainFileName   = cxxMode ? "cflat_hdr_stub.cpp" : "cflat_hdr_stub.c";
        req.source         = source;
        req.cxxImportGroupKey = cxxMode && activeCxxRequestGroup_ != nullptr
            ? CxxIncrementalGroupKey(*activeCxxRequestGroup_) : source;
        req.args           = cxxMode && activeCxxRequestGroup_ != nullptr
            ? BuildCxxRequestClangArgs(*activeCxxRequestGroup_)
            : BuildClangDriverArgs(primaryDir, extraDefines, /*errorRecovery*/ true, cxxMode);
        req.cxxMode        = cxxMode;
        req.wantMacros     = true;
        req.requireInScope = true;
        req.checkHeaderScope = true;
        req.scopeHeaderPath = headerPaths.front();
        /*
         * M5 - a C++ header bind needs the inline BODIES: they are the only definition of an
         * inline function, an all-inline method or the key function of a vtable, and Clang emits
         * them into the companion module the backend links in. A C header still binds
         * declarations only (faster, and it avoids parsing system-header inline bodies).
         * LSP analysis never runs CodeGen: the symbol index wants declarations, not IR.
         */
        req.emitDefinitions    = cxxMode && symbolSink_ == nullptr;
        req.assumeInlineDefinitions = cxxMode && !req.emitDefinitions;
        req.skipFunctionBodies = !req.emitDefinitions;
        req.wantIncludes   = (outIncludes != nullptr) || cxxMode;
        req.verbose        = verbose;
        if (cxxMode && activeCxxRequestGroup_ != nullptr)
            AttachCxxHeaderParseGuard(req, *activeCxxRequestGroup_);

        // Expand um/<->shared/ siblings: the Windows SDK splits its surface across both and
        // code from MSDN fails without the sibling (ERROR_SUCCESS, MAX_PATH, etc.).
        auto addScopeDir = [&](const std::string& d) {
            for (const auto& e : req.inScopeDirs) if (e == d) return;
            req.inScopeDirs.push_back(d);
        };
        for (const auto& h : headerPaths)
        {
            // Direct C++ standard-library entries are request catalogs, not bound surfaces. Keep
            // them in the include set without widening extraction to every sibling implementation header.
            if (cxxMode && IsSystemCxxHeaderPath(h)) continue;
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
        const bool incrementalHeader = cxxMode && activeCxxRequestGroup_ != nullptr
            && UseCxxIncrementalRequests();
        req.demandOnlyDefinitions = incrementalHeader && !cflat_cinterop::CxxEagerBodies()
            && symbolSink_ == nullptr;  // LSP keeps the eager definition path
        if (req.demandOnlyDefinitions)
        {
            req.emitDefinitions = false;
            req.assumeInlineDefinitions = false;
        }
        bool extracted = false;
        std::vector<std::string_view> includedBuffers;   // parallel to raw.includedFiles
        if (incrementalHeader)
        {
            cflat_cinterop::ExtractRequest macroReq = req;
            macroReq.source = source;
            cflat_cinterop::ExtractRequest incrementalReq = req;
            incrementalReq.checkHeaderScope = false;
            std::string headerChunk = BuildCxxRequestIncludes(*activeCxxRequestGroup_);
            headerChunk += "typedef int __cflat_header_scope_sentinel;\n";
            CxxIncrementalGroup* incremental = GetCxxIncrementalGroup(
                *activeCxxRequestGroup_, err, headerChunk, /*tolerateDiagnostics*/ true,
                &macroReq, &raw);
            if (incremental != nullptr && raw.headerScopeOpen)
            {
                err = "header leaves a namespace or brace scope open";
                cxxIncrementalGroups_.erase(CxxIncrementalGroupKey(*activeCxxRequestGroup_));
            }
            else
            {
                extracted = incremental != nullptr
                    && incremental->HarvestHeader(incrementalReq, raw, err);
                if (extracted) includedBuffers = incremental->IncludedFileBuffers();
                if (extracted && raw.demandRecorded)
                {
                    pendingCxxDemandGroupKey_ = CxxIncrementalGroupKey(*activeCxxRequestGroup_);
                    auto& replay = pendingCxxDemandReplayChunk_;
                    replay = {};
                    replay.headerHarvest = true;
                    replay.source = incrementalReq.source;
                    replay.scopeHeaderPath = incrementalReq.scopeHeaderPath;
                    replay.inScopeDirs = incrementalReq.inScopeDirs;
                    replay.macroProbes = raw.macroProbes;
                    replay.wantMacros = incrementalReq.wantMacros;
                    replay.requireInScope = incrementalReq.requireInScope;
                    replay.checkHeaderScope = incrementalReq.checkHeaderScope;
                    NoteCxxDemandReplayChunk(*activeCxxRequestGroup_, replay);
                }
                if (extracted && !raw.macroProbes.empty())
                {
                    std::string probes;
                    for (size_t i = 0; i < raw.macroProbes.size(); ++i)
                    {
                        probes += "static const __auto_type __cflat_macro_";
                        probes += std::to_string(i);
                        probes += " = ";
                        probes += raw.macroProbes[i].name;
                        probes += ";\n";
                    }
                    cflat_cinterop::ExtractRequest probeReq = incrementalReq;
                    probeReq.cxxMacroProbes = raw.macroProbes;
                    probeReq.cxxWrapperBatch = true;
                    probeReq.emitDefinitions = false;
                    probeReq.demandOnlyDefinitions = false;
                    probeReq.assumeInlineDefinitions = true;
                    probeReq.skipFunctionBodies = true;
                    probeReq.wantMacros = false;
                    cflat_cinterop::ExtractResult probeRaw;
                    const bool probesExtracted = incremental->ParseRequest(
                        probeReq, probes, probeRaw, err);
                    if (probesExtracted)
                        raw.macros.insert(raw.macros.end(), probeRaw.macros.begin(),
                                          probeRaw.macros.end());
                    else
                        err.clear();
                }
            }
            if (!extracted)
            {
                if (verbose)
                    std::cout << std::format("[verbose]   incremental header extraction failed ({}); "
                                             "falling back to a full parse\n", err);
                req.source = source;
                req.cxxMacroProbes.clear();
                req.demandOnlyDefinitions = false;
                req.emitDefinitions = cxxMode && symbolSink_ == nullptr;
                req.assumeInlineDefinitions = cxxMode && !req.emitDefinitions;
                req.skipFunctionBodies = !req.emitDefinitions;
                raw = cflat_cinterop::ExtractResult();
                includedBuffers.clear();
                /*
                 * A header that fails this parse too must report its own clang diagnostic, not a
                 * parse-budget refusal. So the fallback's parses are recorded and counted only
                 * once it has produced a usable header.
                 */
                std::vector<const char*> fallbackStages;
                auto parseGuard = std::move(req.cxxHeaderParseGuard);
                req.cxxHeaderParseGuard = [&fallbackStages](const char* stage) {
                    fallbackStages.push_back(stage);
                    return true;
                };
                extracted = cflat_cinterop::ExtractCInterop(req, raw, err);
                req.cxxHeaderParseGuard = std::move(parseGuard);
                if (extracted && raw.headerErrors == 0 && raw.prereqErrors == 0
                    && req.cxxHeaderParseGuard != nullptr)
                    for (const char* stage : fallbackStages)
                        if (!req.cxxHeaderParseGuard(stage)) return false;
            }
        }
        else
            extracted = cflat_cinterop::ExtractCInterop(req, raw, err);
        if (!extracted)
        {
            if (verbose) std::cout << std::format("[verbose]   C header extraction failed: {}\n", err);
            return false;
        }
        if (outDynamicInitializers != nullptr)
            *outDynamicInitializers = raw.cxxDynamicInitializerSymbols;
        SetCInteropTargetFacts(raw);
        if (outIncludes) *outIncludes = raw.includedFiles;
        const bool captureClosure = cxxMode && activeCxxRequestGroup_ != nullptr
            && outIncludes != nullptr && !GetCHeaderCacheDir().empty();
        std::unordered_map<std::string, std::pair<uint64_t, int64_t>> closureHashes;
        auto* includeHashes = includeHashSink_;
        if (captureClosure && includeHashes == nullptr) includeHashes = &closureHashes;
        if (cxxMode && activeCxxRequestGroup_ != nullptr)
        {
            // Signature types are bound on first lookup now, so a namespace that only appears in
            // a signature spelling (c10 behind torch::ones) has no walked record to seed it. Seed
            // from the spellings and from the included headers, or a later on-demand request
            // cannot resolve the namespace.
            auto seedNamespace = [&](const std::string& spelling) {
                size_t end = spelling.find("::");
                // A leading '::' is the global scope qualifier; the namespace follows it.
                if (end == 0) end = spelling.find("::", 2);
                if (end == std::string::npos || end == 0) return;
                // Take the identifier immediately before the '::'. Clang's canonical spelling
                // carries cv-qualifiers and elaborated keywords ("const c10::Scalar &"), so a
                // plain prefix would yield "const c10" and never seed the real namespace.
                size_t begin = end;
                while (begin > 0
                       && (std::isalnum((unsigned char)spelling[begin - 1]) != 0
                           || spelling[begin - 1] == '_'))
                    --begin;
                std::string lead = spelling.substr(begin, end - begin);
                if (lead.empty() || std::isdigit((unsigned char)lead.front()) != 0) return;
                for (const char* keyword : { "const", "volatile", "struct", "class",
                                             "enum", "union" })
                    if (lead == keyword) return;
                cxxForeignNamespaces_.insert(lead);
                if (activeCxxRequestGroup_->primary < cxxImportGroups_.size())
                    cxxImportGroups_[activeCxxRequestGroup_->primary].namespaces.insert(lead);
            };
            for (const auto& sig : raw.sigs)
            {
                seedNamespace(sig.retType);
                for (const auto& param : sig.paramTypes) seedNamespace(param);
            }
            CollectHeaderNamespaceNamesParallel(
                raw.includedFiles, cxxForeignNamespaces_,
                activeCxxRequestGroup_->primary < cxxImportGroups_.size()
                    ? &cxxImportGroups_[activeCxxRequestGroup_->primary].namespaces : nullptr,
                includedBuffers.empty() ? nullptr : &includedBuffers, includeHashes);
        }
        /*
         * The group closure is part of every request key. Record it before the records register:
         * a projection during registration can complete special members, and a warm run (which
         * records the closure from its entry first) must compute the same key.
         */
        if (captureClosure)
        {
            llvm::TimeTraceScope closureScope("CxxEarlyClosureCapture");
            std::vector<CHeaderDep> deps;
            CollectCHeaderDependencies(raw.includedFiles, *includeHashes, deps);
            RememberCxxGroupClosureContent(activeCxxRequestGroup_->primary, deps);
            cxxEarlyClosureDeps_ = std::move(deps);
        }
        if (outLongDoubleWidth) *outLongDoubleWidth = raw.longDoubleWidth;
        if (outLongDoubleIsIEEEDouble)
            *outLongDoubleIsIEEEDouble = raw.longDoubleIsIEEEDouble;
        if (outTargetTriple) *outTargetTriple = raw.targetTriple;
        if (outFunctionTemplates) *outFunctionTemplates = raw.functionTemplates;
        if (outClassTemplates) *outClassTemplates = raw.classTemplates;
        if (outClassTemplateNames) *outClassTemplateNames = raw.classTemplateNames;
        if (outVariableTemplateNames) *outVariableTemplateNames = raw.variableTemplateNames;
        if (outClassTemplateSpecializations)
            *outClassTemplateSpecializations = raw.classTemplateSpecializations;
        if (outUsingDirectives) *outUsingDirectives = raw.usingDirectives;
        if (outNamespaceAliases) *outNamespaceAliases = raw.namespaceAliases;
        if (outClassUsings) *outClassUsings = raw.classUsings;
        if (cxxMode) RegisterCxxUsingDirectives(raw.usingDirectives);
        if (cxxMode) RegisterCxxNamespaceAliases(raw.namespaceAliases);

        // Default expressions stay on the harvested surface. Forwarding bodies are requested
        // by overload matching, and cached independently of this header entry.

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

        // Any other error inside a bound header leaves invalid declarations behind, and in
        // cxxMode no companion module at all (the harvest and CodeGen passes refuse to run over
        // them). Report the clang diagnostic rather than binding the error-recovered remnants.
        if (raw.headerErrors > 0)
        {
            if (outHeaderFailure) *outHeaderFailure = true;
            if (outPrereqMsg) *outPrereqMsg = raw.firstHeaderError;
            if (verbose)
                std::cout << std::format("[verbose]   header does not compile: {}\n",
                                         raw.firstHeaderError);
            return false;
        }

        if (outFunctionPointerAbis) *outFunctionPointerAbis = raw.functionPointerAbis;

        // Enum-backed widths are needed while mapping signatures and record fields, both of
        // which are processed before the enum constants are published below.
        for (const auto& re : raw.enums)
        {
            if (re.isScoped && !re.enumType.empty()) RegisterScopedEnumType(re.enumType);
            if (!re.enumType.empty()) importedEnumTypes_.insert(re.enumType);
            if (re.enumType.empty() || re.underlyingType.empty()) continue;
            TypeAndValue backing;
            if (MapCTypeToTypeAndValue(re.underlyingType, backing, cxxMode))
                RegisterEnumBackingType(re.enumType, backing.TypeName);
            if (TypeAndValue promoted; !re.promotedType.empty()
                && MapCTypeToTypeAndValue(re.promotedType, promoted, cxxMode))
                enumPromotedTypes_[re.enumType] = promoted.TypeName;
        }

        // Companion module: adopt it for this compile and hand it back so the header cache can
        // replay the identical bitcode on a warm run.
        if (!raw.bitcode.empty())
        {
            if (verbose)
                std::cout << std::format("[verbose]   C++ companion module: {} definition(s), {} bytes\n",
                                         raw.emittedDefinitions, raw.bitcode.size());
            AdoptCxxCompanionBitcode(raw.bitcode);
            if (outCxxBitcode != nullptr) *outCxxBitcode = std::move(raw.bitcode);
        }

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
            CxxExtractionStageTimer recordStage(verbose && cxxMode,
                                                "backend record/member registration");
            PruneRecordsToNeededClosure(raw);
            MapRawRecords(raw, outRecords);
            RegisterCRecords(outRecords, headerPath);
            // Surface `typedef struct Tag {...} Name;` as Name -> Tag aliases now that the tags
            // are registered, so the user can write MSG/RECT/WNDCLASSEXA, not tagMSG/...
            CollectRecordTypedefAliases(raw, outAliases);
            RegisterRecordAliases(outAliases);
            CollectTypeAliases(raw, outTypeAliases);
            if (cxxMode) RegisterTypeAliasSymbols(outTypeAliases, true);
            RegisterCxxFunctionPointerAbis(raw.functionPointerAbis);
        }

        {
            llvm::TimeTraceScope sigScope("MapSignatures", headerPath);
            CxxExtractionStageTimer sigStage(verbose && cxxMode,
                                             "backend signature registration");
            // Enums register after signatures; name them so a signature never requests one.
            std::unordered_set<std::string> localEnums;
            for (const auto& re : raw.enums)
                if (!re.enumType.empty()) localEnums.insert(CanonicalCxxForeignIdentity(re.enumType));
            if (cxxMode && activeCxxRequestGroup_ != nullptr)
            {
                // Every specialization this header's signatures name, instantiated by ONE pair of
                // Clang frontends instead of two per spelling. The loop below then replays each
                // request off the cache the batch filled.
                PublishCxxGroupNames(activeCxxRequestGroup_->primary, outRecords);
            }
            for (const auto& rs : raw.sigs)
            {
                if (cxxMode && rs.name.starts_with("__cflat_dflt_")) continue;
                cflat_cinterop::RawSig mappedRaw = rs;
                // A C++ import declares every free function the umbrella header reaches. Asking
                // for the types they name instantiates hundreds of specializations no CFlat line
                // ever mentions, each one a pair of full re-parses. Bind those signatures on
                // first LOOKUP instead (TryBindCxxFunction); the C path stays eager.
                bool requested = false;
                if (!cxxMode || activeCxxRequestGroup_ == nullptr)
                    requested = RequestCxxSignatureTypes(rs, &localEnums);
                if (requested && rs.isCxx && rs.abi.valid)
                    mappedRaw.bindRefusal.clear();
                CSigEntry e;
                if (MapRawSig(mappedRaw, e))
                {
                    if (!e.isCxx && e.isInline) e.inlineDefines = extraDefines;
                    outSigs.push_back(std::move(e));
                }
            }
        }

        {
            llvm::TimeTraceScope enumScope("MapEnums", headerPath);
        for (const auto& re : raw.enums)
        {
                CEnumEntry e;
                e.name = re.name; e.enumType = re.enumType; e.underlyingType = re.underlyingType;
                e.promotedType = re.promotedType;
                e.isScoped = re.isScoped;
                e.value = re.value;
                e.line = re.line ? re.line : 1; e.col = re.col < 0 ? 0 : re.col;
                outEnums.push_back(std::move(e));
            }
        }

        {
            llvm::TimeTraceScope macroScope("MapMacros", headerPath);
            for (const auto& rm : raw.macros)
            {
                CMacroEntry e;
                if (ClassifyRawMacro(rm, e, cxxMode)) outMacros.push_back(std::move(e));
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
                if (MapRawGlobal(rg, e, cxxMode)) outGlobals.push_back(std::move(e));
            }
        }

        if (verbose)
            std::cout << std::format("[verbose]   header bind: {} sig(s), {} enum(s), {} record(s), {} macro(s), {} func-macro(s), {} global(s)\n",
                outSigs.size(), outEnums.size(), outRecords.size(), outMacros.size(), outFuncMacros.size(), outGlobals.size());
        return true;
    }

/*
 * M5b - concrete C++ TYPE REQUESTS (class template specializations, and classes named through a
 * typedef such as `std::string`).
 *
 * Route: one synthetic "explicit instantiation" translation unit per requested type, made of the
 * import group's headers plus `template class std::vector<int>;` and a marker typedef. That needs
 * no long-lived Sema, keeps Clang confined to CClangExtract.cpp, and reuses the whole one-shot
 * extraction path (RawRecord / RawCxxMember / RawAbi, CodeGen into a companion module).
 *
 * The plan's design (internal/plan/cpp-direct-abi-interop.md M5) is a LIVE Sema session per import
 * group that instantiates through Sema::RequireCompleteType; that remains the eventual home. What
 * this route does NOT cover: function templates deduced at a call site (there is no type to name),
 * and a specialization requested from LSP analysis, which never runs CodeGen and therefore only
 * binds what an inline definition can supply.
 *
 * Two stages are needed because libc++ marks nearly every member
 * `__attribute__((exclude_from_explicit_instantiation))`: the explicit instantiation alone
 * instantiates the CLASS but no member BODY, and by the time our consumer runs Sema is past
 * end-of-TU, so nothing can be instantiated on demand. Stage 1 extracts the member list and their
 * canonical signatures; stage 2 re-parses the same TU with one ODR-USE per member appended (a
 * pointer-to-member cast for a method, a placement-new wrapper for a constructor, a
 * pseudo-destructor call for the destructor), which is what makes Sema instantiate the bodies that
 * CodeGen then emits into the companion module.
 */

bool LLVMBackend::IsCxxForeignTypeRegistered(const std::string& cflatName) const
{
        return cxxCflatToCxxSpelling_.find(cflatName) != cxxCflatToCxxSpelling_.end();
}

/*
 * An enumerator used as a C++ template argument. The registry keeps the value sign-extended from
 * its backing width, so an unsigned backing is re-read unsigned before it is spelled; and a SCOPED
 * enumeration has no implicit conversion from its integer, so the argument must carry the cast to
 * the qualified enumeration type or the request that reaches clang is ill-formed.
 */
std::string LLVMBackend::CxxEnumeratorArgumentSpelling(const std::string& enumSpelled,
                                                       int64_t value) const
{
        const std::string enumKey = ResolveEnumTypeName(enumSpelled);
        std::string literal = std::to_string(value);
        if (value < 0)
        {
            TypeAndValue probe;
            probe.TypeName = GetEnumBackingType(enumKey);
            if (const int bits = probe.IsUnsignedInteger(); bits > 0)
                literal = std::to_string(bits >= 64
                                             ? (uint64_t)value
                                             : ((uint64_t)value & ((uint64_t(1) << bits) - 1)));
        }
        std::string enumSpelling;
        if (!enumKey.empty() && scopedEnumTypes_.count(enumKey) != 0
            && CxxSpellingForCflatType(enumKey, enumSpelling))
            return "(" + enumSpelling + ")" + literal;
        return literal;
}

// CFlat type argument -> C++ spelling. Primitives by width, a previously requested foreign type by
// its own spelling, one trailing pointer level as a pointer. Anything else (a CFlat struct, a CFlat
// generic instantiation) has no C++ identity and is refused by the caller.
/*
 * A C++ template argument is offered to deduction by SPELLING, so a primitive argument has to
 * keep its CFlat identity. The machine type cannot supply it: i8 is `char` and `i8` and `u8`
 * alike, i32 is `int` and `wchar`, i64 is `long` and `i64`. The call path drops TypeName for
 * primitives on purpose (implicit conversions must stay open in overload resolution) but keeps
 * the declared name in InferSourceTypeName, so that is the identity source here. The declared
 * name is accepted only when it lowers to EXACTLY the value's machine type, so an operand that
 * was widened or converted on the way in still falls back to the machine-type guess.
 */
std::string LLVMBackend::CxxArithIdentityForArgument(const NamedVariable& arg,
                                                     llvm::Type* valueType) const
{
        if (arg.CxxArithIdentity.empty() || valueType == nullptr || !arg.LiteralIdentity.empty())
            return std::string();
        if (arg.TypeAndValue.Pointer || arg.TypeAndValue.ElemPointer) return std::string();
        TypeAndValue declared;
        declared.TypeName = arg.CxxArithIdentity;
        llvm::Type* identityType = GetType(declared, nullptr, false);
        // An unsuffixed floating literal lowers as float but is a C++ double, so a floating
        // identity may name a wider floating type than the value; the call converts it.
        const bool floating = identityType != nullptr && identityType->isFloatingPointTy()
            && valueType->isFloatingPointTy()
            && identityType->getPrimitiveSizeInBits() > valueType->getPrimitiveSizeInBits();
        return identityType == valueType || floating ? arg.CxxArithIdentity : std::string();
}

/*
 * An unscoped enum lvalue or constant is spelled to clang by its PROMOTED type ([conv.prom]).
 * Allowlist: a NON-FIXED enum's promoted type differs from its backing only as an unsigned backing
 * (all values non-negative) promoting to the signed type of the same width (int, long). Every fixed underlying type
 * keeps its own spelling (clang matches it exactly), char-type and bool included.
 * Empty for a scoped enum, a non-enum, or any pair outside the allowlist.
 */
std::string LLVMBackend::CxxUnscopedEnumPromotedIdentity(const NamedVariable& arg) const
{
        const TypeAndValue& tv = arg.TypeAndValue;
        if (tv.Pointer || tv.ElemPointer || tv.TypeName.empty()) return std::string();
        const std::string key = ResolveEnumTypeName(tv.TypeName);
        if (key.empty() || IsScopedEnumTypeName(key)) return std::string();
        auto promoted = enumPromotedTypes_.find(key);
        auto backing = enumBackingTypes.find(key);
        if (promoted == enumPromotedTypes_.end() || backing == enumBackingTypes.end()) return std::string();
        static const std::unordered_set<std::string> unsignedBackings = { "u32", "uint", "u64", "ulong" };
        static const std::unordered_set<std::string> signedPromotions = { "int", "i32", "long", "i64" };
        if (unsignedBackings.count(backing->second) == 0 || signedPromotions.count(promoted->second) == 0)
            return std::string();
        const bool wide = backing->second == "u64" || backing->second == "ulong";
        const bool wideP = promoted->second == "long" || promoted->second == "i64";
        if (wide != wideP) return std::string();
        return promoted->second;
}

/*
 * A CFlat-declared enum has no C++ spelling, and clang never sees its declaration. Its values are
 * the backing integers C++ would read, so every wrapper spells the type as that integer. The
 * marker is the import: only a clang-read enum is in importedEnumTypes_. Scoped enums stay refused.
 */
bool LLVMBackend::IsCflatOnlyUnscopedEnum(const std::string& enumKey) const
{
        return !enumKey.empty() && enumBackingTypes.count(enumKey) != 0
            && importedEnumTypes_.count(enumKey) == 0 && scopedEnumTypes_.count(enumKey) == 0;
}

bool LLVMBackend::CxxCflatEnumBackingSpelling(const std::string& enumKey, std::string& spelling) const
{
        return IsCflatOnlyUnscopedEnum(enumKey) && CxxSpellingForCflatType(GetEnumBackingType(enumKey), spelling);
}

std::string LLVMBackend::CxxCflatEnumBackingIdentity(const NamedVariable& arg) const
{
        const TypeAndValue& tv = arg.TypeAndValue;
        if (tv.Pointer || tv.ElemPointer || tv.TypeName.empty()) return std::string();
        const std::string key = ResolveEnumTypeName(tv.TypeName);
        return IsCflatOnlyUnscopedEnum(key) ? GetEnumBackingType(key) : std::string();
}

const llvm::ConstantInt* LLVMBackend::CflatEnumeratorConstant(const llvm::Value* storage,
                                                              const std::string& enumKey) const
{
        auto* global = llvm::dyn_cast_or_null<llvm::GlobalVariable>(storage);
        if (global == nullptr || !global->hasInitializer() || !IsCflatOnlyUnscopedEnum(enumKey))
            return nullptr;
        // A member global is named `<enum>.<member>`; a variable of the enum type is not.
        const std::string name = global->getName().str();
        const std::string prefix = enumKey + ".";
        int64_t value = 0;
        if (name.compare(0, prefix.size(), prefix) != 0
            || !TryGetEnumMemberInt(enumKey, name.substr(prefix.size()), value))
            return nullptr;
        return llvm::dyn_cast<llvm::ConstantInt>(global->getInitializer());
}

bool LLVMBackend::CxxEnumPromotesToElement(const NamedVariable& source,
                                           const TypeAndValue& elementType)
{
        const std::string promoted = CxxUnscopedEnumPromotedIdentity(source);
        TypeAndValue promotedType;
        return !promoted.empty() && MapCTypeToTypeAndValue(promoted, promotedType, true)
            && promotedType.IsUnsignedInteger() == elementType.IsUnsignedInteger()
            && GetType(promotedType) == GetType(elementType);
}

std::string LLVMBackend::DeclaredPrimitiveIdentityForCxxArgument(const NamedVariable& arg,
                                                                 llvm::Type* valueType) const
{
        static const std::unordered_set<std::string> primitives = {
            "bool", "char", "i8", "u8", "short", "i16", "u16", "int", "i32", "uint", "u32",
            "long", "ulong", "i64", "u64", "c8", "c16", "c32", "wchar", "float", "double",
            "longdouble",
        };
        const std::string& name = arg.InferSourceTypeName;
        if (name.empty() || valueType == nullptr) return std::string();
        if (arg.TypeAndValue.Pointer || arg.TypeAndValue.ElemPointer) return std::string();
        if (primitives.count(name) == 0) return std::string();
        TypeAndValue declared;
        declared.TypeName = name;
        if (GetType(declared, nullptr, false) != valueType) return std::string();
        return name;
}

bool LLVMBackend::CxxSpellingForCflatType(const std::string& cflatType, std::string& out) const
{
        // `T[]` as a template argument is C++'s array of unknown bound (make_unique<int[]>).
        if (cflatType.size() > 2 && cflatType.ends_with("[]"))
        {
            std::string element;
            if (!CxxSpellingForCflatType(cflatType.substr(0, cflatType.size() - 2), element)
                || element.ends_with("]"))
                return false;
            out = element + "[]";
            return true;
        }
        std::string base = cflatType;
        int ptr = 0;
        while (!base.empty() && base.back() == '*') { base.pop_back(); ++ptr; }
        if (!base.empty() && (std::isdigit((unsigned char)base.front())
                              || (base.front() == '-' && base.size() > 1)))
        {
            bool integer = true;
            for (size_t i = base.front() == '-' ? 1 : 0; i < base.size(); ++i)
                if (!std::isdigit((unsigned char)base[i])) { integer = false; break; }
            if (integer) { out = base; for (int i = 0; i < ptr; ++i) out += " *"; return true; }
        }
        if (ptr == 0)
        {
            if (auto dot = base.rfind('.'); dot != std::string::npos)
            {
                const std::string enumSpelled = base.substr(0, dot);
                int64_t value = 0;
                if (TryGetEnumMemberInt(enumSpelled, base.substr(dot + 1), value))
                {
                    out = CxxEnumeratorArgumentSpelling(enumSpelled, value);
                    return true;
                }
            }
        }
        static const std::unordered_map<std::string, std::string> prims = {
            { "bool", "bool" }, { "char", "char" },
            { "i8", "signed char" }, { "u8", "unsigned char" },
            { "short", "short" }, { "i16", "short" }, { "u16", "unsigned short" },
            { "int", "int" }, { "i32", "int" }, { "uint", "unsigned int" }, { "u32", "unsigned int" },
            { "long", "long" }, { "i64", "long long" },
            { "ulong", "unsigned long" }, { "u64", "unsigned long long" },
            { "c8", "char8_t" }, { "c16", "char16_t" },
            { "c32", "char32_t" }, { "wchar", "wchar_t" },
            { "float", "float" }, { "double", "double" }, { "void", "void" },
            { "longdouble", "long double" },
        };
        // Prefer the exact reverse entry before decoding '$'. A nested class of a template
        // specialization uses the same delimiter as template arguments in the identity, so
        // decoding `Outer$A$B.Item` would otherwise turn the nested class into part of `B`, and
        // re-deriving a recorded spelling would drop defaulted arguments (allocators).
        if (auto fit = cxxCflatToCxxSpelling_.find(base);
            fit != cxxCflatToCxxSpelling_.end())
        {
            out = fit->second;
            for (int i = 0; i < ptr; ++i) out += " *";
            return true;
        }
        // Foreign identities encode nested C++ templates with '$', but that is not a C++
        // spelling. Decode the identity recursively when no exact reverse spelling is known.
        if (base.find('$') != std::string::npos)
        {
            TypeSpelling parsed;
            const bool parsedOk = DemangleType(*this, base, parsed);
            if (parsedOk && !parsed.args.empty())
            {
                auto spell = [&](auto&& self, const TypeSpelling& type,
                                 std::string& spelling) -> bool {
                    if (type.value)
                    {
                        spelling = type.base;
                        return true;
                    }
                    if (type.view || type.closure) return false;
                    std::string baseSpelling;
                    if (type.args.empty())
                    {
                        if (auto it = prims.find(type.base); it != prims.end())
                            baseSpelling = it->second;
                        else if (auto it = cxxCflatToCxxSpelling_.find(type.base);
                                 it != cxxCflatToCxxSpelling_.end())
                            baseSpelling = it->second;
                        else
                        {
                            const size_t dot = type.base.find('.');
                            if (dot == std::string::npos
                                || cxxForeignNamespaces_.count(type.base.substr(0, dot)) == 0)
                                return false;
                            baseSpelling = type.base;
                            for (size_t pos = 0;
                                 (pos = baseSpelling.find('.', pos)) != std::string::npos;
                                 pos += 2)
                                baseSpelling.replace(pos, 1, "::");
                        }
                    }
                    else
                    {
                        const size_t dot = type.base.find('.');
                        if (dot == std::string::npos
                            || cxxForeignNamespaces_.count(type.base.substr(0, dot)) == 0)
                            return false;
                        baseSpelling = type.base;
                        for (size_t pos = 0;
                             (pos = baseSpelling.find('.', pos)) != std::string::npos;
                             pos += 2)
                            baseSpelling.replace(pos, 1, "::");
                        baseSpelling += "<";
                        for (size_t i = 0; i < type.args.size(); ++i)
                        {
                            if (i != 0) baseSpelling += ", ";
                            std::string argSpelling;
                            if (!self(self, type.args[i], argSpelling)) return false;
                            baseSpelling += argSpelling;
                        }
                        baseSpelling += ">";
                    }
                    for (int i = 0; i < type.pointerDepth; ++i) baseSpelling += " *";
                    spelling = std::move(baseSpelling);
                    return true;
                };
                std::string decoded;
                if (spell(spell, parsed, decoded))
                {
                    out = std::move(decoded);
                    for (int i = 0; i < ptr; ++i) out += " *";
                    return true;
                }
            }
        }
        auto enumKey = ResolveEnumTypeName(base);
        if (!enumKey.empty())
        {
            if (scopedEnumTypes_.count(enumKey) != 0)
            {
                base = enumKey;
                for (size_t pos = 0; (pos = base.find('.', pos)) != std::string::npos; pos += 2)
                    base.replace(pos, 1, "::");
                out = base;
                for (int i = 0; i < ptr; ++i) out += " *";
                return true;
            }
            else
                base = GetEnumBackingType(enumKey);
        }
        auto closureIt = encodedClosureTypes_.find(base);
        if (closureIt != encodedClosureTypes_.end())
        {
            const auto& sig = closureIt->second;
            auto component = [&](const std::string& name, bool pointer, int depth,
                                 std::string& spelling) {
                if (!CxxSpellingForCflatType(name, spelling)) return false;
                int levels = pointer ? std::max(depth, 1) : 0;
                for (int i = 0; i < levels; ++i) spelling += " *";
                return true;
            };
            std::string ret;
            if (!component(sig.FuncPtrReturnTypeName, sig.FuncPtrReturnPointer,
                           sig.FuncPtrReturnPointerDepth, ret)) return false;
            out = ret + " (";
            for (size_t i = 0; i < sig.FuncPtrParams.size(); ++i)
            {
                if (i != 0) out += ", ";
                std::string param;
                const auto& p = sig.FuncPtrParams[i];
                if (!component(p.TypeName, p.Pointer, p.PointerDepth, param)) return false;
                out += param;
            }
            out += ")";
        }
        else if (auto it = prims.find(base); it != prims.end()) out = it->second;
        else if (auto lazy = cxxLazyAliasSpecializations_.find(base);
                 lazy != cxxLazyAliasSpecializations_.end())
            out = lazy->second;
        else if (IsCppStructName(base))
        {
            out = cflat_cinterop::GeneratedCflatStructCxxName(base);
        }
        else if (auto dot = base.find('.'); dot != std::string::npos
                 && cxxForeignNamespaces_.count(base.substr(0, dot)) != 0
                 && dataStructures.count(base) == 0)
        {
            // A dotted name under an imported C++ namespace that no request has registered yet:
            // spell it as clang would read it and let the request's TU resolve it. This is how a
            // NAMESPACE ALIAS (`simdjson::ondemand` for `simdjson::arm64::ondemand`) works as a
            // template argument; the canonical result aliases onto the registration.
            out = base;
            for (size_t pos = 0; (pos = out.find('.', pos)) != std::string::npos; pos += 2)
                out.replace(pos, 1, "::");
        }
        else return false;
        for (int i = 0; i < ptr; ++i) out += " *";
        return true;
}

bool LLVMBackend::CxxTemplateArgSpellingForCflatType(const std::string& cflatType,
                                                     std::string& out) const
{
        if (MangledBase(cflatType) == "barefn")
            return CxxSpellingForCflatType(cflatType, out);
        auto function = encodedClosureTypes_.find(cflatType);
        if (function == encodedClosureTypes_.end())
            return CxxSpellingForCflatType(cflatType, out);
        const TypeAndValue& signature = function->second;
        auto component = [&](const std::string& name, bool pointer, int depth,
                             std::string& spelling) {
            if (!CxxSpellingForCflatType(name, spelling)) return false;
            const int levels = pointer ? std::max(depth, 1) : 0;
            for (int i = 0; i < levels; ++i) spelling += " *";
            return true;
        };
        std::string result;
        if (!component(signature.FuncPtrReturnTypeName, signature.FuncPtrReturnPointer,
                       signature.FuncPtrReturnPointerDepth, result))
            return false;
        out = result + " (*) (";
        for (size_t i = 0; i < signature.FuncPtrParams.size(); ++i)
        {
            const auto& parameter = signature.FuncPtrParams[i];
            std::string spelling;
            if (!component(parameter.TypeName, parameter.Pointer, parameter.PointerDepth,
                           spelling))
                return false;
            if (i != 0) out += ", ";
            out += spelling;
        }
        out += ")";
        return true;
}

void LLVMBackend::RememberGeneratedCxxRecords(CxxIncrementalGroup& incremental,
                                              const std::string& source) const
{
        for (const auto& [name, record] : generatedCxxRecords_)
        {
            const std::string definition = record.source + '\n';  // as GeneratedCxxDefinitionsFor emits it
            if (source.find(definition) != std::string::npos)
                incremental.RememberPrefixSource(definition);
        }
        for (const std::string& selector : cxxBraceSelectorSources_)
            if (source.find(selector) != std::string::npos)
                incremental.RememberPrefixSource(selector);
}

void LLVMBackend::GeneratedCxxDefinitionsFor(
        const std::vector<std::string>& cflatTypeNames, std::string& outSource,
        std::set<size_t>& outDependencyGroups,
        std::unordered_set<std::string>& outIncompleteTypes) const
{
        std::string definitions;
        std::unordered_set<std::string> seenDefinitions;
        std::unordered_set<std::string> seenForwardDeclarations;
        std::unordered_set<std::string> visited;
        std::function<void(const std::string&)> visit = [&](const std::string& rawType) {
            std::string type = rawType;
            while (!type.empty() && type.back() == '*') type.pop_back();
            if (type.empty() || !visited.insert(type).second) return;

            if (auto generated = generatedCxxRecords_.find(type);
                generated != generatedCxxRecords_.end())
            {
                if (generated->second.ownerGroup != static_cast<size_t>(-1))
                    outDependencyGroups.insert(generated->second.ownerGroup);
                if (seenDefinitions.insert(type).second)
                    definitions += generated->second.source + "\n";
            }
            else if (IsCppStructName(type))
            {
                outIncompleteTypes.insert(type);
                if (seenForwardDeclarations.insert(type).second)
                {
                    const std::string cxxName =
                        cflat_cinterop::GeneratedCflatStructCxxName(type);
                    const size_t last = cxxName.rfind("::");
                    const std::string shortName = last == std::string::npos
                        ? cxxName : cxxName.substr(last + 2);
                    outSource += "namespace __cflat_user { struct " + shortName + "; }\n";
                }
            }

            TypeSpelling parsed;
            if (!DemangleType(*this, type, parsed)) return;
            // A pointer or view argument is prefix-mangled ('.p$BP'), so normalize it to the
            // pointee before checking generated records and incomplete [cpp] struct names.
            if (parsed.pointerDepth > 0 || parsed.view)
            {
                TypeSpelling pointee = parsed;
                pointee.pointerDepth = 0;
                pointee.view = false;
                const std::string pointeeName = MangleType(*this, pointee);
                if (pointeeName != type)
                {
                    visit(pointeeName);
                    return;
                }
            }
            for (const auto& arg : parsed.args) visit(MangleType(*this, arg));
        };
        for (const std::string& type : cflatTypeNames) visit(type);
        outSource += definitions;
}

std::vector<std::pair<std::string, std::string>> LLVMBackend::RetryTentativeCxxTypesFor(
    const std::string& generatedTypeName)
{
        std::vector<std::pair<std::string, std::string>> failures;
        std::vector<std::string> pending(cxxTentativeTypes_.begin(), cxxTentativeTypes_.end());
        for (const std::string& cflatName : pending)
        {
            auto spellingIt = cxxCflatToCxxSpelling_.find(cflatName);
            auto ownerIt = cxxTypeOwnerGroup_.find(cflatName);
            if (spellingIt == cxxCflatToCxxSpelling_.end()
                || ownerIt == cxxTypeOwnerGroup_.end())
                continue;

            TypeSpelling type;
            if (!DemangleType(*this, cflatName, type)) continue;
            std::function<bool(const TypeSpelling&)> containsGenerated =
                [&](const TypeSpelling& current) {
                    std::string name = MangleType(*this, current);
                    while (!name.empty() && name.back() == '*') name.pop_back();
                    if (name == generatedTypeName) return true;
                    return std::any_of(current.args.begin(), current.args.end(),
                                       containsGenerated);
                };
            if (!containsGenerated(type)) continue;

            std::vector<std::string> arguments;
            arguments.reserve(type.args.size());
            for (const auto& argument : type.args)
                arguments.push_back(MangleType(*this, argument));
            std::string generatedSource;
            std::set<size_t> dependencyGroups;
            std::unordered_set<std::string> incompleteTypes;
            GeneratedCxxDefinitionsFor(arguments, generatedSource, dependencyGroups,
                                       incompleteTypes);
            if (generatedSource.empty()) continue;

            std::vector<size_t> dependencies(dependencyGroups.begin(), dependencyGroups.end());
            // Same union as the first request: an argument nested at any depth keeps its own header.
            std::unordered_set<std::string> visitedArgs;
            for (const std::string& argument : arguments)
                CollectCxxTypeOwnerGroups(argument, dependencies, visitedArgs);
            CxxRequestGroup group = MakeCxxRequestGroup(ownerIt->second, dependencies);
            CxxRequestGroupScope groupScope(*this, &group);
            std::string error;
            auto previous = cxxForeignRequests_.find(cflatName);
            const bool hadPrevious = previous != cxxForeignRequests_.end();
            const std::string previousValue = hadPrevious ? previous->second : std::string();
            retryingTentativeCxxType_ = true;
            const bool ok = RequestCxxForeignType(cflatName, spellingIt->second, error,
                                                  /*needDefinitions*/ true,
                                                  /*explicitInstantiation*/ true,
                                                  /*tentative*/ true,
                                                  /*extraSource*/ {}, generatedSource,
                                                  /*persistOnSuccess*/ incompleteTypes.empty());
            retryingTentativeCxxType_ = false;
            if (!ok)
            {
                if (hadPrevious) cxxForeignRequests_[cflatName] = previousValue;
                else cxxForeignRequests_.erase(cflatName);
                failures.emplace_back(cflatName, error);
            }
            else
            {
                cxxTentativeTypes_.erase(cflatName);
                cxxForeignDefinitions_.insert(cflatName);
            }
        }
        return failures;
}

bool LLVMBackend::HasTentativeCxxTypeFor(const std::string& generatedTypeName) const
{
        for (const std::string& cflatName : cxxTentativeTypes_)
        {
            TypeSpelling type;
            if (!DemangleType(*this, cflatName, type)) continue;
            std::function<bool(const TypeSpelling&)> containsGenerated =
                [&](const TypeSpelling& current) {
                    std::string name = MangleType(*this, current);
                    while (!name.empty() && name.back() == '*') name.pop_back();
                    if (name == generatedTypeName) return true;
                    return std::any_of(current.args.begin(), current.args.end(),
                                       containsGenerated);
                };
            if (containsGenerated(type)) return true;
        }
        return false;
}

/*
 * The stub's include prologue, shared by both stages and by the group's PCH. It includes ONE
 * import group's headers, never every C++ header imported so far: an import that comes earlier
 * in the file must not change the translation unit a later import's request is instantiated in.
 */
std::string LLVMBackend::BuildCxxRequestIncludes(const CxxRequestGroup& group) const
{
        std::string src = "#include <new>\n";
        for (const auto& h : group.headers)
        {
            std::string fwd = h;
            std::replace(fwd.begin(), fwd.end(), '\\', '/');
            if (IsSystemCxxHeaderPath(fwd))
                src += "#include <" + std::filesystem::path(fwd).filename().string() + ">\n";
            else
                src += "#include \"" + fwd + "\"\n";
        }
        return src;
    }

std::vector<std::string> LLVMBackend::BuildCxxRequestClangArgs(
    const CxxRequestGroup& group) const
{
        std::string primaryDir;
        for (const auto& h : group.headers)
            if (!IsSystemCxxHeaderPath(h))
            { primaryDir = std::filesystem::path(h).parent_path().string(); break; }
        std::vector<std::string> args = BuildClangDriverArgs(
            primaryDir, group.defines, /*errorRecovery*/ true, /*asCxx*/ true);
        const auto [cpu, features] = ProgramTargetCPUFeatures();
        if (!cpu.empty()) args.push_back("-mcpu=" + cpu);
        // -g reaches the companion's header inline bodies (CodeView on Windows, as the C path).
        // The args fold into the request and group keys, so -g and non-g never share an entry.
        if (cDebugInfo_) args.push_back("-g");
        if (cDebugInfo_ && targetWindows_) args.push_back("-gcodeview");
        size_t featureStart = 0;
        while (featureStart < features.size())
        {
            const size_t comma = features.find(',', featureStart);
            const std::string feature = features.substr(featureStart,
                comma == std::string::npos ? std::string::npos : comma - featureStart);
            if (!feature.empty())
            {
                args.push_back("-Xclang");
                args.push_back("-target-feature=" + feature);
            }
            if (comma == std::string::npos) break;
            featureStart = comma + 1;
        }
        // O0 gives plain inline definitions a synthetic noinline barrier. O1 emits the
        // source-authored inline/noinline/optnone attributes; disabling LLVM passes keeps the
        // harvested bodies unoptimized and reusable at every cflat optimization level.
        args.push_back("-O1");
        args.push_back("-Xclang");
        args.push_back("-disable-llvm-passes");
        return args;
    }

// The marker typedefs and explicit instantiations alone - everything the include prologue does
// not cover. This is the whole source of a request TU whose prologue rides in a PCH.
std::string LLVMBackend::BuildCxxRequestMarkers(const std::vector<CxxRequestItem>& items,
                                                bool instantiateAll,
                                                const std::string& markerPrefix,
                                                bool includeExplicitInstantiation) const
{
        std::string src;
        for (size_t i = 0; i < items.size(); ++i)
        {
            src += "typedef " + items[i].cxxSpelling + " " + markerPrefix
                + std::to_string(i) + ";\n";
            if (items[i].cxxSpelling.find('<') == std::string::npos) continue;
            if (items[i].instantiateMembers)
                src += "template class " + items[i].cxxSpelling + ";\n";
            else if (includeExplicitInstantiation
                     && (instantiateAll || items[i].explicitInstantiation))
                src += "static_assert(sizeof(" + items[i].cxxSpelling + ") > 0);\n";
        }
        return src;
    }

std::string LLVMBackend::BuildCxxRequestPrologue(const CxxRequestGroup& group,
                                                 const std::vector<CxxRequestItem>& items,
                                                 bool instantiateAll) const
{
        return BuildCxxRequestIncludes(group) + BuildCxxRequestMarkers(items, instantiateAll);
}

static std::optional<unsigned> CxxMaxHeaderParses()
{
        static const std::optional<unsigned> limit = []() -> std::optional<unsigned> {
            const char* value = std::getenv("CFLAT_CPP_MAX_HEADER_PARSES");
            if (value == nullptr || *value == '\0') return std::nullopt;
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(value, &end, 10);
            if (end == value || *end != '\0' || parsed > std::numeric_limits<unsigned>::max())
                return std::nullopt;
            return static_cast<unsigned>(parsed);
        }();
        return limit;
}

std::optional<unsigned> LLVMBackend::TuParseBudget() const
{
        return tuParseBudget_.has_value() ? tuParseBudget_ : CxxMaxHeaderParses();
}

bool LLVMBackend::NoteTuParse(const std::string& kind, const std::string& label, unsigned count,
                              const char* stage, unsigned exempt)
{
        // One line per counted parse, same shape for every TU kind: grep "clang parse:" to count.
        if (verbose)
            std::cout << std::format("[verbose] clang parse: {} '{}' #{} ({})\n", kind, label, count, stage);
        const std::optional<unsigned> limit = TuParseBudget();
        if (!limit.has_value() || count - exempt <= *limit) return true;
        LogErrorMessage("{} '{}' was parsed {} times in one compile; the budget is {} "
                        "(--error-on-cpp-reparse or CFLAT_CPP_MAX_HEADER_PARSES)",
                        { kind, label, std::to_string(count), std::to_string(*limit) });
        return false;
}

bool LLVMBackend::CountTuParse(const std::string& kind, const std::string& key, const std::string& label,
                               const char* stage)
{
        return NoteTuParse(kind, label, ++tuParseCounts_[kind + "|" + key], stage);
}

bool LLVMBackend::CountCxxHeaderParse(const CxxRequestGroup& group, const char* stage)
{
        if (group.primary >= cxxImportGroups_.size()) return true;
        CxxImportGroup& importGroup = cxxImportGroups_[group.primary];
        const unsigned count = ++importGroup.headerParseCount;
        // Only the one parse RecoverCxxLookupTaint armed is exempt; it disarms here.
        if (importGroup.lookupRecoveryArmed)
        {
            importGroup.lookupRecoveryArmed = false;
            ++importGroup.lookupRecoveryParses;
            if (verbose)
                std::cout << std::format("[verbose] clang parse: C++ header group '{}' #{} ({}, "
                                         "lookup recovery)\n", group.label, count, stage);
            return true;
        }
        return NoteTuParse("C++ header group", group.label, count, stage,
                           importGroup.lookupRecoveryParses);
}

void LLVMBackend::ReportCxxHeaderParseSummary(const CxxRequestGroup& group) const
{
        if (!verbose || group.primary >= cxxImportGroups_.size()) return;
        std::cout << std::format(
            "[verbose] C++ header group '{}' full header parse count: {} (summary)\n",
            group.label, cxxImportGroups_[group.primary].headerParseCount);
}

void LLVMBackend::AttachCxxHeaderParseGuard(cflat_cinterop::ExtractRequest& req,
                                             const CxxRequestGroup& group) const
{
        req.cxxHeaderParseGuard = [this, &group](const char* stage) {
            return const_cast<LLVMBackend*>(this)->CountCxxHeaderParse(group, stage);
        };
}


static void ReplaceCxxRequestText(std::string& source, const std::string& from,
                                  const std::string& to)
{
        size_t pos = 0;
        while ((pos = source.find(from, pos)) != std::string::npos)
        {
            source.replace(pos, from.size(), to);
            pos += to.size();
        }
}

// Appends `suffix` to every thunk name BuildCxxVirtualThunks defined in `source`.
static void AppendCxxThunkSuffix(std::string& source, const std::string& suffix)
{
        for (const std::string_view prefix :
             { std::string_view("__cflat_vthk_"), cflat_cinterop::kCxxVbaseCtorThunkPrefix })
        {
            size_t pos = 0;
            while ((pos = source.find(prefix, pos)) != std::string::npos)
            {
                pos += prefix.size();
                while (pos < source.size()
                       && (std::isalnum(static_cast<unsigned char>(source[pos])) || source[pos] == '_'))
                    ++pos;
                source.insert(pos, suffix);
                pos += suffix.size();
            }
        }
}

static void ReplaceCxxRequestUseText(std::string& source, const std::string& to)
{
        const std::string from = "__cflat_use";
        const std::string user = "__cflat_user::";
        size_t pos = 0;
        while ((pos = source.find(from, pos)) != std::string::npos)
        {
            if (source.compare(pos, user.size(), user) == 0)
            {
                ++pos;
                continue;
            }
            source.replace(pos, from.size(), to);
            pos += to.size();
        }
}

std::string LLVMBackend::CxxIncrementalGroupKey(const CxxRequestGroup& group) const
{
        std::string key;
        auto append = [&](const char prefix, const std::string& value) {
            key += prefix + std::to_string(value.size()) + ":" + value + "|";
        };
        append('T', CInteropTargetTriple());
        // One TU per import line: {a.h, b.h} and {a.h, c.h} share a first header but not a TU, so
        // key on the line's whole header set. Dependency-group headers appended after it do not split it.
        if (!group.ownerHeaders.empty())
            for (const auto& header : group.ownerHeaders) append('H', header);
        else if (!group.headers.empty())
            append('H', group.headers.front());
        for (const auto& define : group.defines) append('D', define);
        return key;
}

std::string LLVMBackend::CheckCxxDemand(const std::string& symbol)
{
        if (symbol.empty()) return {};
        // Layout-only imports have not registered these bodies with the demand plan yet.
        if (auto assumed = cxxAssumedBodyOwners_.find(symbol); assumed != cxxAssumedBodyOwners_.end())
        {
            const std::string owner = ResolveTypeAlias(assumed->second);
            std::erase_if(cxxAssumedBodyOwners_, [&](const auto& item) {
                return ResolveTypeAlias(item.second) == owner;
            });
            auto spelling = cxxCflatToCxxSpelling_.find(owner);
            auto owningGroup = cxxTypeOwnerGroup_.find(owner);
            if (spelling != cxxCflatToCxxSpelling_.end() && owningGroup != cxxTypeOwnerGroup_.end()
                && !cxxForeignDefinitions_.count(owner))
            {
                const std::string cxxSpelling = spelling->second;
                CxxRequestGroup group = MakeCxxRequestGroup(owningGroup->second, {});
                CxxRequestGroupScope groupScope(*this, &group);
                std::string error;
                if (!RequestCxxForeignType(owner, cxxSpelling, error, true, true))
                    return "clang: " + error;
            }
        }
        if (auto memo = cxxBodyChecks_.find(symbol); memo != cxxBodyChecks_.end())
            return memo->second == "ok" ? std::string{} : memo->second;
        llvm::TimeTraceScope scope("CxxDemandVerdict", symbol);
        // Program-specific: a [cpp] struct used as a template argument changes the verdict of a
        // symbol that keeps the same name in another program, so it is part of the file key.
        std::string generatedSource;
        if (!generatedCxxRecords_.empty())
            for (const auto& [name, record] : cxxRecordEntries_)
                if (std::any_of(record.members.begin(), record.members.end(), [&](const auto& member) {
                        return member.linkageName == symbol;
                    }))
                {
                    generatedSource = GeneratedCxxPrefixForSpelling(record.canonicalCtype);
                    break;
                }
        struct VerdictEntry
        {
            std::string groupKey;
            CxxVerdictFile* file;
        };
        std::vector<VerdictEntry> entries;
        const std::string cacheDir = noCache_ ? std::string{} : GetCHeaderCacheDir();
        auto known = [&](const CxxVerdictFile& file) {
            auto found = file.known.find(symbol);
            if (found == file.known.end() || found->second.empty()) return false;
            cxxBodyChecks_[symbol] = found->second;
            return true;
        };
        for (const auto& [key, use] : cxxDemandGroups_)
        {
            auto groupIt = cxxDemandRequestGroups_.find(key);
            if (groupIt == cxxDemandRequestGroups_.end()) continue;
            CxxVerdictFile* file = GetCxxVerdictFile(groupIt->second, false, key, generatedSource);
            if (file == nullptr) continue;
            for (const auto& bodyKey : file->lookupTaintedBodies)
                if (std::find(cxxDemandGroups_[key].noDeferBodyKeys.begin(),
                              cxxDemandGroups_[key].noDeferBodyKeys.end(), bodyKey)
                    == cxxDemandGroups_[key].noDeferBodyKeys.end())
                    cxxDemandGroups_[key].noDeferBodyKeys.push_back(bodyKey);
            if (known(*file))
            {
                const std::string& verdict = cxxBodyChecks_[symbol];
                return verdict == "ok" ? std::string{} : verdict;
            }
            entries.push_back({key, file});
        }
        // A group's latest request scope can lose dependency headers on warm registration.
        // Verdicts persist against the whole import cohort as well, independent of that order.
        CxxVerdictFile* cohortFile = GetCxxVerdictFile({}, true, {}, generatedSource);
        if (cohortFile != nullptr && known(*cohortFile))
        {
            const std::string& verdict = cxxBodyChecks_[symbol];
            return verdict == "ok" ? std::string{} : verdict;
        }
        const bool persist = !cacheDir.empty() && !runMode_ && symbolSink_ == nullptr && !batchMode_;
        // One file per group and one for the cohort hold every verdict of the compile; they are
        // written together by FlushCxxDemandVerdicts, not once per symbol.
        auto storeVerdict = [&](CxxVerdictFile* file, const std::string& verdict) {
            if (file == nullptr || !persist) return;
            file->known[symbol] = verdict;
            file->dirty = true;
        };
        for (const auto& entry : entries)
        {
            const auto& use = cxxDemandGroups_.at(entry.groupKey);
            auto live = cxxIncrementalGroups_.find(entry.groupKey);
            if (live == cxxIncrementalGroups_.end() || use.served)
            {
                std::string replayError;
                if (!ReplayCxxDemandChunks(entry.groupKey, replayError))
                {
                    // Reported once here; the use reads "ok" so its caller does not report again.
                    LogError("clang: " + replayError);
                    cxxBodyChecks_[symbol] = "ok";
                    return {};
                }
                live = cxxIncrementalGroups_.find(entry.groupKey);
            }
            if (live == cxxIncrementalGroups_.end()) continue;
            std::string error;
            const int result = live->second->CheckDemand(symbol, error);
            std::vector<std::string> tainted = live->second->TakeLookupTaintedBodyKeys();
            if (!tainted.empty())
            {
                auto& pending = cxxDemandGroups_[entry.groupKey].pendingLookupTaintBodyKeys;
                for (const auto& bodyKey : tainted)
                    if (std::find(pending.begin(), pending.end(), bodyKey) == pending.end())
                        pending.push_back(bodyKey);
            }
            if (verbose)
                llvm::errs() << std::format("[verbose] C++ demand verdict {}: {}\n", symbol, result);
            if (result == 0) continue;
            const std::string verdict = result < 0 ? error : "ok";
            cxxBodyChecks_[symbol] = verdict;
            storeVerdict(cohortFile, verdict);
            storeVerdict(entry.file, verdict);
            return result < 0 ? error : std::string{};
        }
        storeVerdict(cohortFile, "ok");
        cxxBodyChecks_[symbol] = "ok";
        return {};
}

// The verdict file for one request group (or the whole import cohort), loaded from the disk
// cache on first use. Keys carry no symbol, so it is built once per distinct group shape per
// compile instead of once per verdict. Null when the group's headers cannot be stamped.
LLVMBackend::CxxVerdictFile* LLVMBackend::GetCxxVerdictFile(const CxxRequestGroup& requested,
                                                            bool cohortFile,
                                                            const std::string& groupKey,
                                                            const std::string& generatedSource)
{
        // Everything the key reads besides files: shape of the group, and the closure contents
        // an import registers after its header parse.
        std::string signature = (cohortFile ? "C" : "G" + groupKey) + "|S" + generatedSource;
        auto addGroup = [&](const CxxRequestGroup& group) {
            signature += std::format("|{}", group.primary);
            for (const auto& header : group.headers) signature += "|H" + header;
            for (const auto& define : group.defines) signature += "|d" + define;
            for (const auto& owner : group.ownerHeaders) signature += "|O" + owner;
            auto addClosure = [&](size_t index) {
                if (index < cxxImportGroups_.size())
                    signature += std::format("|c{}:{}", index,
                                             cxxImportGroups_[index].closureContent.size());
            };
            addClosure(group.primary);
            for (size_t dep : group.dependencyGroups)
            {
                signature += std::format("|D{}", dep);
                addClosure(dep);
            }
        };
        if (cohortFile)
        {
            // Import groups never change once added (only their closure contents grow).
            for (size_t index = 0; index < cxxImportGroups_.size(); ++index)
                signature += std::format("|c{}", cxxImportGroups_[index].closureContent.size());
        }
        else
            addGroup(requested);
        if (auto memo = cxxVerdictFileMemo_.find(signature); memo != cxxVerdictFileMemo_.end())
            return memo->second;
        CxxRequestGroup cohort;
        std::string cohortSource = generatedSource;
        if (cohortFile)
            for (size_t index = 0; index < cxxImportGroups_.size(); ++index)
            {
                const auto group = MakeCxxRequestGroup(index, {});
                cohortSource += CxxIncrementalGroupKey(group);
                cohort.dependencyGroups.push_back(index);
                for (const auto& header : group.headers)
                    if (std::find(cohort.headers.begin(), cohort.headers.end(), header) == cohort.headers.end())
                        cohort.headers.push_back(header);
            }
        const CxxRequestGroup& group = cohortFile ? cohort : requested;
        CxxVerdictFile* result = nullptr;
        std::filesystem::file_time_type stamp;
        const std::string cacheDir = noCache_ ? std::string{} : GetCHeaderCacheDir();
        if (cohortFile && (cacheDir.empty() || group.headers.empty()))
            ;
        else if (CxxGroupHeaderStamp(group, stamp))
        {
            const std::string requestKey = CxxTypeRequestCacheKey(group,
                "|demand-verdicts-v3-helper-diagnostic",
                cohortFile ? cohortSource : generatedSource, BuildCxxRequestClangArgs(group), false)
                + (cohortFile ? "|BODY_COHORT_SET" : "|BODY_SET");
            auto [slot, fresh] = cxxVerdictFiles_.try_emplace(requestKey);
            result = &slot->second;
            if (fresh)
            {
                result->requestKey = requestKey;
                result->diskKey = HashWrapperKey(requestKey);
                result->hash = CxxGroupHeaderHash(group);
                result->stamp = stamp;
                result->group = group;
                CFileSigCacheEntry cached;
                if (!cacheDir.empty() && TryLoadCHeaderDiskCache(cacheDir, result->diskKey, stamp,
                        [&] { return result->hash; }, cached, requestKey, false))
                {
                    for (const auto& [checked, verdict] : cached.cxxBodyChecks)
                        if (!verdict.empty()) result->known.emplace(checked, verdict);
                    result->lookupTaintedBodies = cached.cxxLookupTaintedBodies;
                }
            }
        }
        cxxVerdictFileMemo_.emplace(signature, result);
        return result;
}

// Writes every verdict file that gained a verdict this compile (one write per file), merged
// with what it already held and with what is on disk now, so a concurrent compiler's verdicts
// written since this compile loaded the file are kept.
void LLVMBackend::FlushCxxDemandVerdicts()
{
        if (noCache_ || cxxVerdictFiles_.empty()) return;
        const std::string cacheDir = GetCHeaderCacheDir();
        if (cacheDir.empty()) return;
        for (auto& [key, file] : cxxVerdictFiles_)
        {
            if (!file.dirty) continue;
            file.dirty = false;
            CFileSigCacheEntry onDisk;
            if (TryLoadCHeaderDiskCache(cacheDir, file.diskKey, file.stamp,
                    [&] { return file.hash; }, onDisk, file.requestKey, false))
            {
                for (const auto& [checked, verdict] : onDisk.cxxBodyChecks)
                    if (!verdict.empty()) file.known.emplace(checked, verdict);
                for (const auto& bodyKey : onDisk.cxxLookupTaintedBodies)
                    if (std::find(file.lookupTaintedBodies.begin(), file.lookupTaintedBodies.end(),
                                  bodyKey) == file.lookupTaintedBodies.end())
                        file.lookupTaintedBodies.push_back(bodyKey);
            }
            CFileSigCacheEntry cached;
            for (const auto& [symbol, verdict] : file.known)
                cached.cxxBodyChecks.emplace_back(symbol, verdict);
            cached.cxxLookupTaintedBodies = file.lookupTaintedBodies;
            ScheduleCHeaderDiskCacheWrite(cacheDir, file.diskKey, file.stamp, file.hash,
                std::move(cached), file.requestKey, &file.group);
        }
}

void LLVMBackend::ValidateCxxDemand(const std::string& symbol)
{
        ValidateCxxDemandAtUseSite(symbol);
}

void LLVMBackend::ValidateCxxDemandAtUseSite(const std::string& symbol)
{
        if (std::string error = CheckCxxDemand(symbol); !error.empty())
            LogError(error.starts_with("clang: ") ? error : "clang: " + error);
}

void LLVMBackend::ValidateCxxStaticMemberDemandAtUseSite(const std::string& symbol)
{
        if (cxxStaticMemberSymbols_.count(symbol) != 0)
            ValidateCxxDemandAtUseSite(symbol);
}

std::string LLVMBackend::CxxBodyRefusedWrapperError(const CxxRequestGroup& group,
                                                     const std::string& cacheTag)
{
        // Only a constructor call's own wrapper carries the "clang: " refusal as its error; every
        // other wrapper's caller wraps the raw clang text itself.
        if (cacheTag != "VARIADIC_CTOR") return {};
        auto live = cxxIncrementalGroups_.find(CxxIncrementalGroupKey(group));
        if (live == cxxIncrementalGroups_.end()) return {};
        // CheckCxxDemand below resets the group's list: walk a copy.
        const std::vector<std::string> symbols = live->second->LastRefusedConstructors();
        for (const std::string& symbol : symbols)
        {
            std::string bodyError;
            if (!cxxConstructorBodyRefusals_.contains(symbol))
                if (bodyError = CheckCxxDemand(symbol); !bodyError.empty())
                    RefuseCxxDemandMember(symbol, bodyError);
            if (auto found = cxxConstructorBodyRefusals_.find(symbol);
                found != cxxConstructorBodyRefusals_.end())
                return found->second;
            if (!bodyError.empty())
                return bodyError.starts_with("clang: ") ? bodyError : "clang: " + bodyError;
        }
        return {};
}

std::string LLVMBackend::CxxConstructorBodyRefusalError(
        const CxxClassInfo::Structor* selected) const
{
        if (selected == nullptr) return {};
        auto found = cxxConstructorBodyRefusals_.find(selected->linkageName);
        return found == cxxConstructorBodyRefusals_.end() ? std::string{} : found->second;
}

std::string LLVMBackend::CxxConstructorCopySinkError(
        const CxxClassInfo::Structor* selected,
        const std::vector<NamedVariable>& arguments) const
{
        if (selected == nullptr) return {};
        for (size_t i = 0; i < arguments.size() && i + 1 < selected->params.size(); ++i)
        {
            const NamedVariable& argument = arguments[i];
            const TypeAndValue& parameter = selected->params[i + 1];
            if (!IsCopyDeletedCxxLvalue(argument) || !parameter.Pointer || parameter.ElemPointer
                || !parameter.CxxConstRef()
                || parameter.TypeName != argument.TypeAndValue.TypeName)
                continue;
            std::string name = std::format("parameter {}", i + 1);
            for (const auto& [recordName, record] : cxxRecordEntries_)
                for (const auto& member : record.members)
                    if (member.linkageName == selected->linkageName
                        && i + 1 < member.paramNames.size())
                        name = member.paramNames[i + 1];
            return std::format("cannot copy C++ class '{}' into constructor parameter '{}'",
                               DisplayCxxClassName(argument.TypeAndValue.TypeName), name);
        }
        return {};
}

void LLVMBackend::RefuseCxxDemandMember(const std::string& symbol, const std::string& error)
{
        const std::string reason = "cannot be instantiated for these template arguments "
            "(clang reported an error inside the body it generated)";
        const std::string cause = error.starts_with("clang: ") ? error.substr(7) : error;
        for (auto& [name, record] : cxxRecordEntries_)
            for (auto& member : record.members)
                if (member.linkageName == symbol
                    && member.kind == cflat_cinterop::RawCxxMember::Constructor)
                {
                    cxxConstructorBodyRefusals_[symbol] = error.starts_with("clang: ")
                        ? error : "clang: " + error;
                    // Stays bindable and projected: the use site that selects it refuses it.
                    member.refusalCause = cause;
                    return;
                }
        for (auto& [name, record] : cxxRecordEntries_)
            for (auto& member : record.members)
                if (member.linkageName == symbol)
                {
                    member.bindRefusal = reason;
                    member.refusalCause = cause;
                    member.linkageName.clear();
                    if (auto info = cxxClasses_.find(name); info != cxxClasses_.end())
                    {
                        bool sibling = false;
                        if (auto overloads = functionTable.find(member.name);
                            overloads != functionTable.end())
                            sibling = std::any_of(overloads->second.begin(), overloads->second.end(),
                                [&](const FunctionSymbol& other) {
                                    return other.IsCxx && other.Function != nullptr
                                        && other.Function->getName() != symbol
                                        && !other.Parameters.empty()
                                        && other.Parameters.front().TypeName == name;
                                });
                        if (sibling) info->second.refusedMembers.erase(member.name);
                        else info->second.refusedMembers[member.name] = reason
                            + " (clang: " + CxxFirstDiagnosticLine(cause) + ")";
                    }
                }
        for (auto& [name, overloads] : functionTable)
            std::erase_if(overloads, [&](const FunctionSymbol& candidate) {
                return candidate.IsCxx && candidate.Function != nullptr
                    && candidate.Function->getName() == symbol;
            });
        for (auto& [name, info] : cxxClasses_)
            std::erase_if(info.constructors, [&](const CxxClassInfo::Structor& ctor) {
                return ctor.linkageName == symbol;
            });
}

void LLVMBackend::NoteCxxDemandEntry(const std::string& groupKey, uint64_t hash,
                                     uint64_t entryKey, bool served, bool demand)
{
        if (groupKey.empty()) return;
        CxxDemandGroupUse& use = cxxDemandGroups_[groupKey];
        if (activeCxxRequestGroup_ != nullptr
            && activeCxxRequestGroup_->primary < cxxImportGroups_.size())
        {
            const auto& order = cxxImportGroups_[activeCxxRequestGroup_->primary].importOrder;
            if (use.importOrder.empty() || order < use.importOrder) use.importOrder = order;
        }
        use.hashes.insert(HashWrapperKey(std::format("{:016x}:{:016x}", hash, entryKey)));
        use.served = use.served || served;
        use.demand = use.demand || demand;
}

void LLVMBackend::NoteCxxDynamicInitializers(
    const std::vector<cflat_cinterop::RawDynamicInitializer>& initializers)
{
        if (activeCxxRequestGroup_ == nullptr || initializers.empty()) return;
        const std::string groupKey = CxxIncrementalGroupKey(*activeCxxRequestGroup_);
        auto& use = cxxDemandGroups_[groupKey];
        cflat_cinterop::CxxImportOrder importOrder = activeCxxRequestGroup_->primary < cxxImportGroups_.size()
            ? cxxImportGroups_[activeCxxRequestGroup_->primary].importOrder
            : cflat_cinterop::CxxImportOrder{ static_cast<uint64_t>(-1) };
        if (use.importOrder.empty() || importOrder < use.importOrder) use.importOrder = importOrder;
        for (const auto& initializer : initializers)
        {
            if (initializer.symbol.empty()) continue;
            const std::string identity = initializer.identity.empty()
                ? initializer.symbol : initializer.identity;
            cflat_cinterop::NoteDynamicInitializerOccurrence(
                cxxDynamicInitializerOwners_, identity, groupKey);
            if (use.dynamicInitializerSeen.insert(identity).second)
                use.dynamicInitializerSymbols.push_back(initializer);
        }
}

void LLVMBackend::NoteCxxDemandReplayChunk(const CxxRequestGroup& group,
    const cflat_cinterop::ExtractResult::DemandReplayChunk& chunk)
{
        const std::string key = CxxIncrementalGroupKey(group);
        cxxDemandRequestGroups_[key] = group;
        auto& use = cxxDemandGroups_[key];
        if (group.primary < cxxImportGroups_.size())
        {
            const auto& order = cxxImportGroups_[group.primary].importOrder;
            if (use.importOrder.empty() || order < use.importOrder) use.importOrder = order;
        }
        for (const std::string& bodyKey : chunk.noDeferBodyKeys)
            if (std::find(use.noDeferBodyKeys.begin(), use.noDeferBodyKeys.end(), bodyKey)
                == use.noDeferBodyKeys.end())
                use.noDeferBodyKeys.push_back(bodyKey);
        // A served demand entry without its chunk (an older writer) makes replay unsound.
        if (chunk.source.empty() && !chunk.headerHarvest)
        {
            cxxDemandGroups_[key].replayIncomplete = true;
            return;
        }
        auto& chunks = cxxDemandGroups_[key].replayChunks;
        auto note = [&](const cflat_cinterop::ExtractResult::DemandReplayChunk& item) {
            if (std::none_of(chunks.begin(), chunks.end(), [&](const auto& old) {
                    return old.order == item.order && old.source == item.source
                        && old.headerHarvest == item.headerHarvest;
                }))
            {
                chunks.push_back(item);
                chunks.back().follow.clear();
            }
        };
        note(chunk);
        for (const auto& follow : chunk.follow) note(follow);
}

void LLVMBackend::PersistCxxLookupTaint(const std::string& groupKey,
                                        const std::vector<std::string>& bodyKeys)
{
        auto useIt = cxxDemandGroups_.find(groupKey);
        auto groupIt = cxxDemandRequestGroups_.find(groupKey);
        if (useIt == cxxDemandGroups_.end() || groupIt == cxxDemandRequestGroups_.end()) return;
        auto appendUnique = [](std::vector<std::string>& to,
                               const std::vector<std::string>& from) {
            for (const auto& key : from)
                if (std::find(to.begin(), to.end(), key) == to.end()) to.push_back(key);
            std::sort(to.begin(), to.end());
        };
        appendUnique(useIt->second.noDeferBodyKeys, bodyKeys);
        if (verbose)
            for (const auto& bodyKey : bodyKeys)
                std::cout << std::format("[verbose] C++ lookup taint: {}\n", bodyKey);
        for (auto& chunk : useIt->second.replayChunks)
            appendUnique(chunk.noDeferBodyKeys, useIt->second.noDeferBodyKeys);
        for (auto& [requestKey, file] : cxxVerdictFiles_)
        {
            if (CxxIncrementalGroupKey(file.group) != groupKey) continue;
            appendUnique(file.lookupTaintedBodies, useIt->second.noDeferBodyKeys);
            file.dirty = true;
        }

        std::vector<std::pair<std::string, CFileSigCacheEntry>> entries;
        {
            std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
            for (auto& [requestKey, entry] : cFileSigCache_)
            {
                if (entry.cxxDemandGroupKey != groupKey) continue;
                appendUnique(entry.cxxDemandReplayChunk.noDeferBodyKeys,
                             useIt->second.noDeferBodyKeys);
                entries.emplace_back(requestKey, entry);
            }
        }
        if (!groupIt->second.diskCache || runMode_ || batchMode_ || symbolSink_ != nullptr) return;
        const std::string cacheDir = GetCHeaderCacheDir();
        if (cacheDir.empty()) return;
        for (const auto& [requestKey, entry] : entries)
        {
            uint64_t diskKey = 14695981039346656037ULL;
            for (unsigned char byte : requestKey)
            {
                diskKey ^= byte;
                diskKey *= 1099511628211ULL;
            }
            WriteCHeaderDiskCache(cacheDir, diskKey, entry.mtime, entry.hash, entry,
                                  requestKey, &groupIt->second);
        }
}

bool LLVMBackend::RecoverCxxLookupTaint(const std::string& groupKey,
                                        const std::vector<std::string>& tainted,
                                        CxxIncrementalGroup*& group, std::string& error)
{
        auto requestIt = cxxDemandRequestGroups_.find(groupKey);
        if (requestIt == cxxDemandRequestGroups_.end()
            || requestIt->second.primary >= cxxImportGroups_.size())
        {
            error = "lookup-tainted C++ group has no import group";
            return false;
        }
        CxxImportGroup& importGroup = cxxImportGroups_[requestIt->second.primary];
        // Persisted first: even a refused recovery leaves the next compile parsing them in place.
        PersistCxxLookupTaint(groupKey, tainted);
        auto& use = cxxDemandGroups_[groupKey];
        use.pendingLookupTaintBodyKeys.clear();
        if (importGroup.lookupRecoveryParses != 0)
        {
            error = "inline bodies still lookup-tainted after the group's recovery parse";
            return false;
        }
        /*
         * The recovery parse keeps every body in place, not just the known tainted ones: a body
         * first reached after it could be tainted too, and a third parse is not allowed. Only the
         * keys persist, so the next compile parses lazily again with them in place.
         */
        use.forceEagerBodies = true;
        use.served = false;
        FlushCxxDemandVerdicts();
        cxxIncrementalGroups_.erase(groupKey);
        importGroup.lookupRecoveryArmed = true;
        std::string replayError;
        const bool replayed = ReplayCxxDemandChunks(groupKey, replayError);
        importGroup.lookupRecoveryArmed = false;
        if (!replayed)
        {
            error = replayError.empty() ? "group replay failed" : replayError;
            return false;
        }
        auto rebuilt = cxxIncrementalGroups_.find(groupKey);
        if (rebuilt == cxxIncrementalGroups_.end() || rebuilt->second == nullptr)
        {
            error = "lookup-tainted C++ group was not rebuilt";
            return false;
        }
        group = rebuilt->second.get();
        if (verbose)
            std::cout << std::format("[verbose] C++ lookup-tainted group '{}' reparsed in place: "
                                     "{} declaration(s)\n", requestIt->second.label, tainted.size());
        return true;
}

bool LLVMBackend::ReplayCxxDemandChunks(const std::string& groupKey, std::string& error)
{
        const auto replayStart = std::chrono::steady_clock::now();
        auto groupIt = cxxDemandRequestGroups_.find(groupKey);
        auto useIt = cxxDemandGroups_.find(groupKey);
        if (groupIt == cxxDemandRequestGroups_.end() || useIt == cxxDemandGroups_.end())
        {
            error = "missing request group for demand replay";
            return false;
        }
        if (useIt->second.replayIncomplete)
        {
            error = "a served demand entry stored no replay chunk";
            return false;
        }
        const bool groupWasLive = cxxIncrementalGroups_.contains(groupKey);
        const auto createStart = std::chrono::steady_clock::now();
        const bool wasReplaying = std::exchange(cxxDemandReplayInProgress_, true);
        struct ReplayScope
        {
            bool& value;
            bool previous;
            ~ReplayScope() { value = previous; }
        } replayScope{cxxDemandReplayInProgress_, wasReplaying};
        auto chunks = useIt->second.replayChunks;
        std::stable_sort(chunks.begin(), chunks.end(), [](const auto& left, const auto& right) {
            return left.order < right.order;
        });
        // Create the group the way the cold header harvest did, so its header root matches.
        const bool headerFirst = !groupWasLive && !chunks.empty() && chunks.front().headerHarvest;
        std::string headerChunk;
        if (headerFirst)
            headerChunk = BuildCxxRequestIncludes(groupIt->second)
                + "typedef int __cflat_header_scope_sentinel;\n";
        CxxIncrementalGroup* incremental = GetCxxIncrementalGroup(
            groupIt->second, error, headerChunk, /*tolerateDiagnostics*/ headerFirst);
        if (incremental == nullptr) return false;
        const double createMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - createStart).count();
        // Chunk-scoped names (__cflat_inc_<n>_) of a later live request must not repeat one a
        // replayed chunk already declared in this Interpreter.
        auto reserveChunkNumber = [this](const std::string& text) {
            static const std::string kChunk = "__cflat_inc_";
            size_t pos = 0;
            while ((pos = text.find(kChunk, pos)) != std::string::npos)
            {
                pos += kChunk.size();
                unsigned number = 0;
                bool digits = false;
                while (pos < text.size() && std::isdigit((unsigned char)text[pos]) != 0)
                {
                    number = number * 10 + unsigned(text[pos++] - '0');
                    digits = true;
                }
                if (digits) cxxDemandUsedChunkNumbers_.insert(number);
            }
        };
        auto replayOne = [&](const cflat_cinterop::ExtractResult::DemandReplayChunk& chunk,
                             std::string& error) -> bool {
            const auto chunkStart = std::chrono::steady_clock::now();
            incremental->RestorePoisonedBodies(chunk.poisonedBodies);
            if (chunk.headerHarvest && incremental->HasDemandHeaderHarvest()) return true;
            if (!chunk.headerHarvest && incremental->HasDemandChunkSource(chunk.source)) return true;
            cflat_cinterop::ExtractRequest req;
            req.mainFileName = "cflat_cpp_demand_replay.cpp";
            req.cxxMode = true;
            req.cxxImportGroupKey = CxxIncrementalGroupKey(groupIt->second);
            req.verbose = verbose;
            req.source = chunk.source;
            req.args = BuildCxxRequestClangArgs(groupIt->second);
            AttachCxxHeaderParseGuard(req, groupIt->second);
            cflat_cinterop::ExtractResult raw;
            bool parsed = false;
            if (chunk.headerHarvest)
            {
                req.mainFileName = "cflat_cpp_header_replay.cpp";
                req.emitDefinitions = cflat_cinterop::CxxEagerBodies();
                req.demandOnlyDefinitions = !req.emitDefinitions;
                req.assumeInlineDefinitions = false;
                req.skipFunctionBodies = false;
                req.wantMacros = chunk.wantMacros;
                req.requireInScope = chunk.requireInScope;
                req.checkHeaderScope = chunk.checkHeaderScope;
                req.scopeHeaderPath = chunk.scopeHeaderPath;
                req.inScopeDirs = chunk.inScopeDirs;
                req.cxxMacroProbes = chunk.macroProbes;
                parsed = incremental->HarvestHeader(req, raw, error);
            }
            else
            {
                req.emitDefinitions = true;
                req.assumeInlineDefinitions = false;
                req.skipFunctionBodies = false;
                req.requireInScope = false;
                req.demandPrefixSource = chunk.prefixSource;
                req.cxxTypeRequests = chunk.typeRequests;
                req.cxxRequestMarkerPrefix = chunk.markerPrefix;
                req.cxxThunkSuffix = chunk.thunkSuffix;
                req.cxxFunctionWrapperNames = chunk.wrapperNames;
                req.cxxWrapperBatch = chunk.wrapperBatch;
                req.autoInstantiateCxxTypes = chunk.autoInstantiate;
                req.completeCxxSpecialMembers = chunk.completeSpecialMembers;
                // Strip the stored whole prefix against THIS group, as the live path does: it
                // may lack declarations the storing compile's earlier chunks made.
                std::string text = chunk.source;
                const size_t at = chunk.prefixOffset;
                if (at != std::string::npos && !chunk.prefixSource.empty()
                    && chunk.source.compare(at, chunk.prefixSource.size(), chunk.prefixSource) == 0)
                {
                    text = chunk.source.substr(0, at)
                        + incremental->UnseenPrefixSource(chunk.prefixSource)
                        + chunk.source.substr(at + chunk.prefixSource.size());
                    req.demandPrefixOffset = at;
                }
                parsed = incremental->ParseRequest(req, text, raw, error);
            }
            if (!parsed || !raw.demandRecorded)
            {
                if (error.empty()) error = "replayed request did not record demand definitions";
                return false;
            }
            // As the live paths do: a type request remembers its whole prefix, a generated
            // wrapper request only the generated records inside it.
            if (!chunk.headerHarvest && !chunk.prefixSource.empty())
            {
                if (chunk.wrapperNames.empty())
                    incremental->RememberPrefixSource(chunk.prefixSource);
                RememberGeneratedCxxRecords(*incremental, chunk.prefixSource);
            }
            if (!chunk.headerHarvest && chunk.source.find(kCxxWrapperPidDecl) != std::string::npos)
                incremental->RememberPrefixSource(kCxxWrapperPidDecl);
            if (!chunk.headerHarvest
                && chunk.source.find(CxxClosureWrapperPreamble()) != std::string::npos)
                incremental->RememberPrefixSource(CxxClosureWrapperPreamble());
            reserveChunkNumber(chunk.markerPrefix);
            reserveChunkNumber(chunk.thunkSuffix);
            if (verbose)
                std::cout << std::format(
                    "[verbose] C++ demand replay chunk {}: {} source byte(s), {:.3f} ms\n",
                    chunk.order, chunk.source.size(),
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - chunkStart).count());
            return true;
        };
        // Stored orders mix compiles, so a request chunk may sort before its header's chunk: a
        // failed one (rolled back) is retried after the rest while that makes progress.
        std::vector<const cflat_cinterop::ExtractResult::DemandReplayChunk*> deferred;
        std::string deferredError;
        for (const auto& chunk : chunks)
        {
            std::string chunkError;
            if (replayOne(chunk, chunkError)) continue;
            if (chunk.headerHarvest)
            {
                error = chunkError;
                return false;
            }
            if (deferred.empty()) deferredError = chunkError;
            deferred.push_back(&chunk);
        }
        while (!deferred.empty())
        {
            std::vector<const cflat_cinterop::ExtractResult::DemandReplayChunk*> still;
            for (const auto* chunk : deferred)
            {
                std::string chunkError;
                if (!replayOne(*chunk, chunkError)) still.push_back(chunk);
            }
            if (still.size() == deferred.size())
            {
                error = deferredError;
                return false;
            }
            if (verbose)
                std::cout << std::format("[verbose] C++ demand replay: {} out-of-order chunk(s) "
                                         "replayed late\n", deferred.size() - still.size());
            deferred = std::move(still);
        }
        if (chunks.empty())
        {
            cflat_cinterop::ExtractRequest req;
            req.mainFileName = "cflat_cpp_header_reparse.cpp";
            req.cxxMode = true;
            req.cxxImportGroupKey = CxxIncrementalGroupKey(groupIt->second);
            req.verbose = verbose;
            req.source = BuildCxxRequestIncludes(groupIt->second)
                + "typedef int __cflat_header_scope_sentinel;\n";
            req.args = BuildCxxRequestClangArgs(groupIt->second);
            req.emitDefinitions = cflat_cinterop::CxxEagerBodies();
            req.demandOnlyDefinitions = !req.emitDefinitions;
            req.assumeInlineDefinitions = false;
            req.skipFunctionBodies = false;
            AttachCxxHeaderParseGuard(req, groupIt->second);
            cflat_cinterop::ExtractResult raw;
            if (!incremental->HarvestHeader(req, raw, error) || !raw.demandRecorded)
            {
                if (error.empty()) error = "header reparse did not record demand definitions";
                return false;
            }
            if (verbose)
                std::cout << std::format("[verbose] C++ demand replay synthesized header "
                                         "harvest: {} demand chunk(s)\n",
                                         incremental->DemandChunks());
        }
        if (verbose)
        {
            const double totalMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - replayStart).count();
            std::cout << std::format(
                "[verbose] C++ demand replay timing: group {} {:.3f} ms, {} chunk(s), total {:.3f} ms\n",
                groupWasLive ? "reuse" : "create/parse", createMs, chunks.size(), totalMs);
        }
        cxxDemandReplayed_ = true;
        return true;
}

CxxIncrementalGroup* LLVMBackend::GetCxxIncrementalGroup(const CxxRequestGroup& group,
                                                          std::string& error,
                                                          const std::string& initialSource,
                                                          bool tolerateDiagnostics,
                                                          const cflat_cinterop::ExtractRequest* macroReq,
                                                          cflat_cinterop::ExtractResult* macroOut)
{
        std::string key = CxxIncrementalGroupKey(group);
        cxxDemandRequestGroups_[key] = group;
        const cflat_cinterop::CxxImportOrder importOrder = group.primary < cxxImportGroups_.size()
            ? cxxImportGroups_[group.primary].importOrder
            : cflat_cinterop::CxxImportOrder{ static_cast<uint64_t>(-1) };
        auto found = cxxIncrementalGroups_.find(key);
        if (found != cxxIncrementalGroups_.end())
        {
            found->second->SetDynamicInitializerOwners(&cxxDynamicInitializerOwners_,
                importOrder);
            return found->second.get();
        }

        const std::string source = initialSource.empty()
            ? BuildCxxRequestIncludes(group) : initialSource;
        if (!CountCxxHeaderParse(group, "incremental chunk 0"))
            return nullptr;
        std::unique_ptr<CxxIncrementalGroup> created = CxxIncrementalGroup::Create(
            BuildCxxRequestClangArgs(group), source, verbose, error, tolerateDiagnostics,
            macroReq, macroOut,
            cxxDemandGroups_.contains(key) ? cxxDemandGroups_[key].noDeferBodyKeys
                                           : std::vector<std::string>{},
            cxxDemandGroups_.contains(key) && cxxDemandGroups_[key].forceEagerBodies);
        if (!created) return nullptr;
        CxxIncrementalGroup* result = created.get();
        result->SetDynamicInitializerOwners(&cxxDynamicInitializerOwners_,
            importOrder);
        result->RememberPrefixSource(BuildCxxRequestIncludes(group));
        cxxIncrementalGroups_.emplace(std::move(key), std::move(created));
        const std::string groupKey = CxxIncrementalGroupKey(group);
        auto demandIt = cxxDemandGroups_.find(groupKey);
        // A live header harvest (initialSource) comes first anyway; replay then runs at link.
        if (cxxDemandLinks_ && initialSource.empty() && !cxxDemandReplayInProgress_
            && !cxxDemandBypass_
            && demandIt != cxxDemandGroups_.end() && demandIt->second.served
            && !demandIt->second.replayChunks.empty())
        {
            std::string replayError;
            const bool replayed = ReplayCxxDemandChunks(groupKey, replayError);
            if (!replayed)
            {
                cxxDemandRetry_ = true;
                if (verbose)
                    std::cout << std::format(
                        "[verbose] C++ demand replay before live request failed ({}); whole compile retry required\n",
                        replayError);
            }
        }
        return result;
}

static std::string NameCxxParameter(const std::string& type, const std::string& name);

/*
 * Probe each exported declaration without instantiating ordinary member bodies.
 * Each use is its own top-level declaration: a member whose signature cannot be re-spelled (a
 * ref-qualified overload, a default argument that does not survive) errors on its own line, is
 * error-recovered, and leaves every other member emitted.
 */
std::string LLVMBackend::BuildCxxRequestOdrUses(const cflat_cinterop::RawRecord& rec,
                                               const std::string& marker,
                                               const std::string& tagPrefix,
                                               bool incrementalSource) const
{
        using Member = cflat_cinterop::RawCxxMember;
        // Over an incomplete [cpp] struct argument every body is instantiated, so the request
        // is refused here rather than after the struct is defined.
        const bool evaluated = !cxxIncompleteArgumentSpelling_.empty();
        std::string src;
        if (!rec.hasTrivialDtor && evaluated)
            src = std::string(incrementalSource ? "__attribute__((used, noinline)) " : "")
                + "static void __cflat_use_dtor" + tagPrefix + "(" + marker
                + "* p) { p->~" + marker + "(); }\n";
        else if (!rec.hasTrivialDtor)
            src = "using __cflat_use_dtor" + tagPrefix + " = decltype(((" + marker
                + "*)0)->~" + marker + "());\n";
        unsigned n = 0;
        for (const auto& m : rec.members)
        {
            if (m.access != cflat_cinterop::AccessPublic || m.isDeleted || m.variadic) continue;
            // `auto` is not a valid return type in a member-pointer cast. Its body cannot be
            // explicitly instantiated through this probe, so do not let it poison sibling uses.
            if (m.retType == "auto") continue;
            if (m.isImplicit && m.kind != Member::Constructor) continue;
            if (m.isDefaulted && m.kind != Member::Constructor) continue;
            if (m.kind == Member::Destructor) continue;   // covered above
            const std::string tag = tagPrefix + std::to_string(n++);
            if (m.kind == Member::Constructor)
            {
                if (rec.isAbstract) continue;
                // A constructor has no address; construct into raw storage instead. The wrapper's
                // own parameters carry the canonical spellings, so a reference parameter stays a
                // reference and an rvalue reference is re-cast on the way in.
                std::string params, args;
                for (size_t p = 1; p < m.paramTypes.size(); ++p)
                {
                    if (p > 1) { params += ", "; args += ", "; }
                    params += NameCxxParameter(m.paramTypes[p], "a" + std::to_string(p));
                    args += m.paramTypes[p].size() > 2
                            && m.paramTypes[p].compare(m.paramTypes[p].size() - 2, 2, "&&") == 0
                        ? "static_cast<" + m.paramTypes[p] + ">(a" + std::to_string(p) + ")"
                        : "a" + std::to_string(p);
                }
                if (evaluated)
                    src += std::string(m.isCopyCtor || m.isMoveCtor
                                           ? "__attribute__((used, noinline)) " : "")
                         + "static void __cflat_use_ctor" + tag + "(void* m"
                         + (params.empty() ? "" : ", " + params) + ") { (void)::new (m) "
                         + marker + "(" + args + "); }\n";
                else
                    src += "static auto __cflat_use_ctor" + tag + "(void* m"
                         + (params.empty() ? "" : ", " + params) + ") -> decltype(::new (m) "
                         + marker + "(" + args + "));\n";
                continue;
            }
            std::string params;
            for (size_t p = (m.kind == Member::StaticMethod ? 0u : 1u); p < m.paramTypes.size(); ++p)
            {
                if (!params.empty()) params += ", ";
                params += m.paramTypes[p];
            }
            const std::string ptrTo = m.kind == Member::StaticMethod
                ? "(*)" : "(" + marker + "::*)";
            const std::string memberName = incrementalSource && m.isConversion
                ? "operator " + m.retType : m.name;
            std::string functionQualifiers = m.isConst ? " const" : "";
            if (incrementalSource)
            {
                if (m.refQualifier == cflat_cinterop::CxxRefQualifierLValue)
                    functionQualifiers += " &";
                else if (m.refQualifier == cflat_cinterop::CxxRefQualifierRValue)
                    functionQualifiers += " &&";
                if (m.isNoexcept) functionQualifiers += " noexcept";
            }
            // Member-template specializations retain the evaluated surface-growing probe.
            const bool signatureOnly = !m.isTemplateSpecialization && !evaluated;
            src += (signatureOnly ? "using __cflat_use" : "static auto __cflat_use") + tag
                 + (signatureOnly ? " = decltype(static_cast<" : " = static_cast<")
                 + m.retType + " " + ptrTo + "(" + params + ")" + functionQualifiers
                 + ">(&" + marker + "::" + memberName
                 + (m.isTemplateSpecialization ? "<>" : "")
                 + (signatureOnly ? "));\n" : ");\n");
        }
        unsigned staticVarIndex = 0;
        for (const auto& var : rec.staticVars)
        {
            if (var.access != cflat_cinterop::AccessPublic) continue;
            src += "static void __cflat_use_static_var" + tagPrefix
                 + std::to_string(staticVarIndex++) + "() { (void)&" + marker + "::"
                 + var.name + "; }\n";
        }
        bool iteratorLike = false;
        for (const auto& m : rec.members)
            iteratorLike = iteratorLike || m.name == "operator++" || m.name == "operator*"
                                        || m.name == "operator->";
        if (iteratorLike)
        {
            // Iterator equality and ordering are commonly non-member function templates found by
            // ADL. The expressions instantiate those overloads so the ordinary free-function
            // extractor can publish them alongside the requested class.
            src += "static void __cflat_use_adl_ops" + tagPrefix + "(" + marker + "* a, "
                 + marker + "* b) { ";
            src += "(void)(*a == *b); (void)(*a != *b); (void)(*a < *b); ";
            src += "}\n";
        }
        // The SFINAE probes themselves live in the TU preamble (one copy per translation unit);
        // only the per-spelling entry points are emitted here.
        src += "static void __cflat_use_member_templates" + tagPrefix + "(" + marker + "* p) { "
               "__cflat_use_value_members_impl(p, 0); }\n";
        src += "static void __cflat_use_inherited_members" + tagPrefix + "(" + marker + "* p) { "
               "__cflat_use_optional_members_impl(p, 0); }\n";
        return src;
}

/*
 * One extern "C" THUNK per virtual member cflat cannot reach any other way - see
 * cflat_cinterop::CxxMemberNeedsVirtualThunk for exactly which those are. Under the MS ABI
 * MicrosoftVTableContext::getMethodVFTableLocation reports a vfptr at a non-zero offset (a
 * non-primary base) or one reached through a VIRTUAL base, and cflat models neither the vbtable
 * lookup nor a secondary vfptr; a covariant return needs an adjustment cflat cannot emit either.
 * Instead of reimplementing the ABI, let Clang do it: the thunk body is an ordinary C++ virtual
 * call, so Clang emits the vftable load, the vbtable adjustment and the return adjustment, and
 * cflat binds the resulting symbol like any other function.
 *
 * A member whose slot IS directly usable, or that is reachable through the record of the base
 * that declares it, keeps that path - a thunk is a real non-inlined call and an extra forwarding
 * move on a by-value class argument, and must not be paid where nothing was broken.
 */
std::string LLVMBackend::BuildCxxVirtualThunks(
    const std::vector<cflat_cinterop::RawRecord>& records) const
{
        using Member = cflat_cinterop::RawCxxMember;
        std::string src;
        std::set<std::string> emitted;
        unsigned recv = 0;
        bool anyCtorThunk = false;
        for (const auto& rec : records)
        {
            // A record whose layout is refused keeps every instance member refused before the
            // vtable slot is ever consulted, so a thunk for it would never be reached.
            if (rec.canonicalCtype.empty() || !rec.layoutRefusal.empty()) continue;
            std::string recordSrc;
            const std::string marker = "__cflat_vthk_recv" + std::to_string(recv);
            for (const auto& m : rec.members)
            {
                /*
                 * A CONSTRUCTOR of a class with virtual bases: a placement new inside the thunk
                 * lets Clang pass the implicit most-derived flag / VTT, and it passes 1 there
                 * because the thunk builds a COMPLETE object, never a base subobject.
                 */
                if (cflat_cinterop::CxxCtorNeedsVbaseThunk(rec, m) && !m.linkageName.empty())
                {
                    const std::string cname = cflat_cinterop::CxxVbaseCtorThunkName(m.linkageName);
                    if (!emitted.insert(cname).second) continue;
                    std::string params, args;
                    for (size_t p = 1; p < m.paramTypes.size(); ++p)
                    {
                        if (p > 1) { params += ", "; args += ", "; }
                        const std::string& t = m.paramTypes[p];
                        const std::string a = "a" + std::to_string(p);
                        params += t + " " + a;
                        args += t.ends_with("&&") ? "static_cast<" + t + ">(" + a + ")"
                              : t.ends_with("&")  ? a
                                                  : "static_cast<" + t + "&&>(" + a + ")";
                    }
                    recordSrc += "extern \"C\" __attribute__((weak)) void " + cname + "("
                               + marker + "* p" + (params.empty() ? "" : ", " + params)
                               + ") { ::new ((void*)p) " + marker + "(" + args + "); }\n";
                    anyCtorThunk = true;
                    continue;
                }
                if (!m.isVirtual || m.linkageName.empty()) continue;
                if (!CxxMemberNeedsVirtualThunk(m)) continue;
                if (m.access != cflat_cinterop::AccessPublic || m.isDeleted || m.variadic) continue;
                if (m.isTemplateSpecialization || !m.bindRefusal.empty()) continue;
                if (m.kind != Member::Instance && m.kind != Member::Destructor) continue;
                if (m.paramTypes.empty()) continue;   // no `this` slot: not an instance member
                const std::string name = cflat_cinterop::CxxVirtualThunkName(m.linkageName);
                if (!emitted.insert(name).second) continue;
                if (m.kind == Member::Destructor)
                {
                    recordSrc += "extern \"C\" __attribute__((weak)) void " + name + "(" + marker + "* p) { p->~"
                               + marker + "(); }\n";
                    continue;
                }
                std::string params, args;
                for (size_t p = 1; p < m.paramTypes.size(); ++p)
                {
                    if (p > 1) { params += ", "; args += ", "; }
                    const std::string& t = m.paramTypes[p];
                    const std::string a = "a" + std::to_string(p);
                    params += t + " " + a;
                    // A BY-VALUE parameter is forwarded as an rvalue, the way any forwarding
                    // wrapper does: one move instead of one copy, and a move-only type still binds.
                    args += t.ends_with("&&") ? "static_cast<" + t + ">(" + a + ")"
                          : t.ends_with("&")  ? a
                                              : "static_cast<" + t + "&&>(" + a + ")";
                }
                const std::string call = "p->" + m.name + "(" + args + ")";
                recordSrc += "extern \"C\" __attribute__((weak)) " + m.retType + " " + name + "(" + marker + "* p"
                           + (params.empty() ? "" : ", " + params) + ") { "
                           + (m.retType == "void" ? call : "return " + call) + "; }\n";
            }
            if (recordSrc.empty()) continue;
            src += "typedef " + rec.canonicalCtype + " " + marker + ";\n" + recordSrc;
            ++recv;
        }
        // Placement new needs the <new> declaration; nothing else emitted here does.
        if (anyCtorThunk) src = "\n#include <new>\n" + src;
        return src;
}

/*
 * ODR-uses for the PUBLIC members a requested class INHERITS. A library may keep part of the
 * member surface on a base template - the MSVC STL's std::function has its operator() on
 * _Func_class - so the request's own record carries nothing to instantiate, and inherited-member
 * registration then finds nothing bound on the base. Each base gets a marker typedef of its own,
 * spelled from the canonical base type the stage-1 walk recorded.
 */
std::string LLVMBackend::BuildCxxRequestInheritedOdrUses(
    const std::vector<cflat_cinterop::RawRecord>& records,
    const cflat_cinterop::RawRecord& rec, const std::string& marker,
    const std::string& tagPrefix, bool incrementalSource) const
{
        std::string src;
        std::set<std::string> visited{ rec.name };
        std::vector<const cflat_cinterop::RawRecord*> work{ &rec };
        unsigned n = 0;
        while (!work.empty())
        {
            const cflat_cinterop::RawRecord* current = work.back();
            work.pop_back();
            for (const auto& b : current->bases)
            {
                if (b.access != cflat_cinterop::AccessPublic || b.isVirtual) continue;
                if (b.canonicalType.empty() || !visited.insert(b.name).second) continue;
                const cflat_cinterop::RawRecord* base = nullptr;
                for (const auto& r : records)
                    if (r.name == b.name) { base = &r; break; }
                if (base == nullptr) continue;
                const std::string baseMarker = marker + "_base" + std::to_string(n);
                src += "typedef " + b.canonicalType + " " + baseMarker + ";\n";
                src += BuildCxxRequestOdrUses(*base, baseMarker,
                                              tagPrefix + "i" + std::to_string(n) + "_",
                                              incrementalSource);
                ++n;
                work.push_back(base);
            }
        }
        return src;
}

/*
 * Probes shared by every spelling in one request translation unit: optional-like member templates
 * reached through dependent SFINAE, so a requested class without value_type or has_value still
 * gets its emitted definitions and its cache entry.
 */


static std::string NameCxxParameter(const std::string& type, const std::string& name)
{
        for (size_t i = 0; i < type.size(); ++i)
        {
            if (type[i] != '(') continue;
            size_t symbol = i + 1;
            while (symbol < type.size() && std::isspace((unsigned char)type[symbol])) ++symbol;
            if (symbol >= type.size() || (type[symbol] != '*' && type[symbol] != '&')) continue;
            size_t close = symbol + 1;
            if (type[symbol] == '&' && close < type.size() && type[close] == '&') ++close;
            while (close < type.size() && type[close] != ')') ++close;
            if (close >= type.size()) continue;
            std::string declarator = type.substr(symbol, close - symbol);
            if (!declarator.empty() && !std::isspace((unsigned char)declarator.back()))
                declarator += ' ';
            return type.substr(0, i + 1) + declarator + name + type.substr(close);
        }
        return type + " " + name;
}

/*
 * Keep the PCH directory bounded. Each file is tens of MB and every cflat rebuild orphans the
 * whole set, because the build stamp is part of the key - a day of compiler work would otherwise
 * leave gigabytes behind. Pruning is by age, not count: every process stamps the files it uses,
 * so anything untouched for an hour is genuinely unreferenced, while a project needing twenty
 * live groups keeps all twenty instead of rebuilding them in a loop. The newest few always
 * survive, so a first build on a slow day cannot delete what it just made. Deleting one still in
 * use costs nothing but a reparse: the load fails and the caller falls back to the real includes.
 */
void LLVMBackend::PruneCxxRequestPchDir(const std::string& dir)
{
        constexpr size_t kAlwaysKeep = 8;
        constexpr auto kMaxAge = std::chrono::hours(1);
        std::error_code ec;
        std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (ec) return;
            if (entry.path().extension() != ".pch") continue;
            auto when = entry.last_write_time(ec);
            if (!ec) files.emplace_back(when, entry.path());
        }
        if (files.size() <= kAlwaysKeep) return;
        std::sort(files.begin(), files.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        const auto cutoff = std::filesystem::file_time_type::clock::now() - kMaxAge;
        for (size_t i = kAlwaysKeep; i < files.size(); ++i)
            if (files[i].first < cutoff) std::filesystem::remove(files[i].second, ec);
    }

// Forget a PCH that failed to load and delete it, so the next request rebuilds it.
void LLVMBackend::DropCxxRequestPch(const std::string& path)
{
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::lock_guard<std::mutex> lock(cxxRequestPchMutex_);
        for (auto it = cxxRequestPchCache_.begin(); it != cxxRequestPchCache_.end(); )
            it = it->second == path ? cxxRequestPchCache_.erase(it) : std::next(it);
    }

/*
 * A precompiled header for one import group's include prologue, shared by every request TU that
 * compiles against that group with the same driver args. Without it each of the dozens of type
 * requests in a file re-parses <vector>/<string>/<map>, which is where the wall clock goes.
 * The PCH lives in the compiler cache next to the C header cache, so it survives the process and
 * the LSP pool's slots share one copy. Returns the PCH path, or empty when it cannot be built.
 */
std::string LLVMBackend::EnsureCxxRequestPch(const CxxRequestGroup& group,
                                             const std::vector<std::string>& args)
{
        llvm::TimeTraceScope scope("EnsureCxxRequestPch", group.label);
        const std::string includes = BuildCxxRequestIncludes(group);
        uint64_t h = 14695981039346656037ULL;
        auto fold = [&h](const std::string& text) {
            for (unsigned char c : text) { h ^= c; h *= 1099511628211ULL; }
            h ^= '|'; h *= 1099511628211ULL;
        };
        fold(includes);
        for (const auto& a : args) fold(a);
        fold(CompilerBuildStamp());
        // Header edits are not folded in: clang records each input file's size and mtime in the
        // PCH and refuses to load a stale one, which the caller's fallback turns into a rebuild.
        const std::string key = std::to_string(h);

        {
            std::lock_guard<std::mutex> lock(cxxRequestPchMutex_);
            auto it = cxxRequestPchCache_.find(key);
            if (it != cxxRequestPchCache_.end()) return it->second;
        }

        std::string resolved;
        const std::string dir = GetCHeaderCacheDir();
        if (!dir.empty())
        {
            const std::string pchDir = dir + "/pch";
            const std::string path = pchDir + "/" + key + ".pch";
            std::error_code ec;
            std::filesystem::create_directories(pchDir, ec);
            if (std::filesystem::exists(path, ec))
            {
                // Stamp the hit so the age-based prune reads it as live, not abandoned.
                // Reusing one already on disk is nearly free, so it needs no amortization.
                resolved = path;
                std::filesystem::last_write_time(
                    path, std::filesystem::file_time_type::clock::now(), ec);
            }
            else if (std::filesystem::is_directory(pchDir, ec))
            {
                // Build to a process-unique name and rename, so a concurrent compile never reads
                // a half-written PCH (rename over an existing file is fine - same contents).
                static std::atomic<unsigned> tempCounter{ 0 };
                const std::string temp = path + "." + std::to_string(_getpid()) + "."
                                       + std::to_string(tempCounter.fetch_add(1)) + ".tmp";
                std::vector<std::string> pchArgs;
                pchArgs.reserve(args.size() + 2);
                for (size_t i = 0; i < args.size(); ++i)
                {
                    // -fsyntax-only would suppress the PCH; -x c++ is replaced by -x c++-header.
                    if (args[i] == "-fsyntax-only") continue;
                    if (args[i] == "-x" && i + 1 < args.size()) { ++i; continue; }
                    pchArgs.push_back(args[i]);
                }
                pchArgs.push_back("-x");
                pchArgs.push_back("c++-header");

                cflat_cinterop::ExtractRequest req;
                req.mainFileName = "cflat_cpp_prologue.h";
                req.source = includes;
                req.cxxMode = true;
                req.cxxImportGroupKey = CxxIncrementalGroupKey(group);
                req.pchOutputPath = temp;
                req.verbose = verbose;
                req.args = pchArgs;
                AttachCxxHeaderParseGuard(req, group);
                cflat_cinterop::ExtractResult ignored;
                std::string pchError;
                const bool built = cflat_cinterop::ExtractCInterop(req, ignored, pchError);
                if (verbose && (!built || !std::filesystem::exists(temp, ec)))
                    std::cout << std::format("[verbose]   C++ prologue PCH not built: {} {}\n",
                                             pchError, ignored.firstError);
                if (built && std::filesystem::exists(temp, ec))
                {
                    std::filesystem::rename(temp, path, ec);
                    if (ec) std::filesystem::remove(temp, ec);
                    if (std::filesystem::exists(path, ec))
                    {
                        resolved = path;
                        PruneCxxRequestPchDir(pchDir);
                    }
                }
                else std::filesystem::remove(temp, ec);
            }
        }
        std::lock_guard<std::mutex> lock(cxxRequestPchMutex_);
        cxxRequestPchCache_[key] = resolved;
        return resolved;
    }

bool LLVMBackend::RunCxxTypeRequests(const CxxRequestGroup& group,
                                     const std::vector<CxxRequestItem>& items,
                                     const std::string& extraSource, bool emitDefinitions,
                                     cflat_cinterop::ExtractResult& raw, std::string& error,
                                     const std::string& prefixSource,
                                     const cflat_cinterop::VariadicCallAbiRequest* variadicCallAbi)
{
        llvm::TimeTraceScope scope("RunCxxTypeRequests", group.label);
        cflat_cinterop::ExtractRequest req;
        req.mainFileName = "cflat_cpp_request.cpp";
        req.cxxMode = true;
        req.cxxImportGroupKey = CxxIncrementalGroupKey(group);
        req.source = BuildCxxRequestIncludes(group) + prefixSource
                   + BuildCxxRequestMarkers(items, /*instantiateAll*/ !emitDefinitions)
                   + extraSource;
        req.emitDefinitions = emitDefinitions;
        req.assumeInlineDefinitions = !emitDefinitions;
        req.skipFunctionBodies = false;
        req.requireInScope = false;
        req.completeCxxSpecialMembers = completingCxxSpecialMembers_;
        req.verbose = verbose;
        if (variadicCallAbi != nullptr)
            req.variadicCallAbiRequest = *variadicCallAbi;
        AttachCxxHeaderParseGuard(req, group);
        for (const CxxRequestItem& item : items)
            req.cxxTypeRequests.push_back({ item.cxxSpelling, item.cflatName });
        req.args = BuildCxxRequestClangArgs(group);
        const bool incrementalRequests = UseCxxIncrementalRequests();
        const bool incrementalSpellingSafe = std::all_of(items.begin(), items.end(),
            [](const CxxRequestItem& item) { return CxxIncrementalSpellingSafe(item.cxxSpelling); });
        if (incrementalRequests && incrementalSpellingSafe)
        {
            /*
             * No request-TU fallback: it would be a second parse of the group's headers, and a
             * request TU fails the same way. Report it like a request TU whose marker did not
             * parse - no records, the clang error as firstError - so callers word it the same.
             */
            auto requestFailed = [&]() {
                if (verbose)
                    std::cout << std::format("[verbose] C++ incremental request for '{}' failed: {}\n",
                                             items.empty() ? std::string() : items.front().cxxSpelling,
                                             error);
                raw = cflat_cinterop::ExtractResult();
                raw.firstError = error;
                error.clear();
                return true;
            };
            CxxIncrementalGroup* incremental = GetCxxIncrementalGroup(group, error);
            if (incremental == nullptr) return false;
            const bool instantiateAll = !emitDefinitions;
            /*
             * Numbered from the content, never from a process counter: a replay serves chunks
             * stored by several compiles into one interpreter, and two concurrent cold compiles
             * counting from the same start gave different chunks one name (a typedef redefinition
             * on replay, then a whole-compile cold retry). Deterministic, so a rerun names chunks
             * the same and its demand companion key is stable.
             */
            uint64_t chunkHash = 14695981039346656037ULL;
            for (unsigned char c : req.source + (emitDefinitions ? "|D" : "|S"))
                chunkHash = (chunkHash ^ c) * 1099511628211ULL;
            unsigned chunkNumber = 100000u + unsigned(chunkHash % 4000000000ULL);
            // Residual: two requests of one group colliding mod 4e9 in DIFFERENT compiles still clash
            // on replay (bypass retry); odds ~n^2/8e9 per group, accepted.
            while (!cxxDemandUsedChunkNumbers_.insert(chunkNumber).second) ++chunkNumber;
            const std::string chunkPrefix = "__cflat_inc_" + std::to_string(chunkNumber) + "_";
            const std::string markerPrefix = chunkPrefix + "req_";
            req.cxxRequestMarkerPrefix = markerPrefix;
            req.cxxThunkSuffix = chunkPrefix + "thk";
            req.autoInstantiateCxxTypes = emitDefinitions;
            std::string incrementalExtra = extraSource;
            ReplaceCxxRequestText(incrementalExtra, "__cflat_req_", markerPrefix);
            ReplaceCxxRequestText(incrementalExtra, "__cflat_pid", markerPrefix + "pid");
            ReplaceCxxRequestUseText(incrementalExtra, chunkPrefix + "use");
            ReplaceCxxRequestText(incrementalExtra, "__cflat_vthk_recv",
                                  chunkPrefix + "vthk_recv");
            AppendCxxThunkSuffix(incrementalExtra, req.cxxThunkSuffix);
            const std::string incrementalIncludes = BuildCxxRequestIncludes(group);
            const std::string incrementalPrefix = incrementalIncludes
                + incremental->UnseenPrefixSource(prefixSource);
            const bool includeExplicitInstantiation = false;
            req.source = incrementalPrefix + BuildCxxRequestMarkers(
                items, instantiateAll, markerPrefix, includeExplicitInstantiation) + incrementalExtra;
            req.demandPrefixSource = prefixSource;
            req.demandPrefixOffset = incrementalIncludes.size();
            raw = cflat_cinterop::ExtractResult();
            CxxExtractionStageTimer parseStage(verbose, "clang parse stage incremental");
            if (incremental->ParseRequest(req, req.source, raw, error))
            {
                if (raw.demandRecorded)
                    NoteCxxDemandReplayChunk(group, raw.demandReplayChunk);
                incremental->RememberPrefixSource(prefixSource);
                RememberGeneratedCxxRecords(*incremental, prefixSource);
                DropCxxDeclarationsOutsideRequestGroup(group, items, *incremental, raw);
                return true;
            }
            return requestFailed();
        }
        const std::string pch = EnsureCxxRequestPch(group, req.args);
        if (!pch.empty())
        {
            req.source = prefixSource + BuildCxxRequestMarkers(items, /*instantiateAll*/ !emitDefinitions)
                       + extraSource;
            req.args.push_back("-include-pch");
            req.args.push_back(pch);
        }
        // See CxxIncrementalGroup::ParseRequest: private nested names in the markers are aliased.
        if (std::any_of(items.begin(), items.end(), [](const CxxRequestItem& item) {
                return cflat_cinterop::IsCxxAccessFreeSpelling(item.cxxSpelling);
            }))
            req.source = cflat_cinterop::AliasCxxNestedSpecializationNames(req.source);
        bool ok = cflat_cinterop::ExtractCInterop(req, raw, error);
        // The PCH must only ever change the speed, never the answer. A stale or truncated one can
        // fail the frontend outright, but it can also be error-recovered into an AST where the
        // requested spellings resolve to nothing - the extractor swallows diagnostics, so that
        // reads as success. Treat an empty harvest the same as a hard failure and parse for real.
        // Harvesting no RECORD is not by itself a failed parse: a request whose spelling resolves
        // to free functions or member signatures alone (the std::function bridge is all of these)
        // legitimately brings back sigs and no records. Reading that as a broken PCH made every
        // such request pay a PCH TU and then a full reparse - measured, 289 request TUs where 37
        // were needed. Only a harvest that brought back NOTHING implicates the PCH.
        const bool emptyHarvest = ok && !items.empty() && raw.records.empty() && raw.sigs.empty();
        if ((!ok || emptyHarvest) && !pch.empty())
        {
            // Which answer is right is only known AFTER the retry, so the group's shared PCH is
            // judged on the retry's outcome rather than deleted on suspicion. An empty harvest is
            // equally the honest answer for a spelling that does not resolve (a dependent
            // std::chrono::duration<type-parameter-0-0, ...>, say), and dropping the PCH for that
            // makes every other request in the group re-parse the include prologue.
            llvm::TimeTraceScope retryScope("CxxRequestPchRetry", group.label);
            cflat_cinterop::ExtractRequest full = req;
            full.source = BuildCxxRequestIncludes(group) + prefixSource
                        + BuildCxxRequestMarkers(items, /*instantiateAll*/ !emitDefinitions)
                        + extraSource;
            full.args.resize(full.args.size() - 2);
            cflat_cinterop::ExtractResult retryRaw;
            std::string retryError;
            const bool retryOk = cflat_cinterop::ExtractCInterop(full, retryRaw, retryError);
            // Only a retry that does strictly better implicates the PCH.
            if (retryOk && (!ok || !retryRaw.records.empty()))
                DropCxxRequestPch(pch);
            raw = std::move(retryRaw);
            error = std::move(retryError);
            ok = retryOk;
        }
        return ok;
    }

/* Lazily query Clang's call-site ABI only for an aggregate in a selected C++ vararg tail. */
bool LLVMBackend::RequestCxxVariadicCallAbi(
        const FunctionSymbol& candidate, const std::vector<NamedVariable>& arguments,
        cflat_cinterop::RawAbi& abi, std::string& error)
{
        error.clear();
        if (!candidate.IsCxx || !candidate.Variadic || candidate.UniqueName.empty()
            || arguments.size() <= candidate.Parameters.size())
            return false;

        size_t primary = static_cast<size_t>(-1);
        if (candidate.IsMethod && !candidate.Parameters.empty())
        {
            auto owner = cxxTypeOwnerGroup_.find(candidate.Parameters.front().TypeName);
            if (owner != cxxTypeOwnerGroup_.end()) primary = owner->second;
        }
        else
        {
            const std::string& name = candidate.SourceName.empty()
                ? candidate.UniqueName : candidate.SourceName;
            auto owner = cxxFunctionOwnerGroup_.find(name);
            if (owner != cxxFunctionOwnerGroup_.end()) primary = owner->second;
            else if (const size_t dot = name.rfind('.'); dot != std::string::npos)
                if (auto memberOwner = cxxTypeOwnerGroup_.find(name.substr(0, dot));
                    memberOwner != cxxTypeOwnerGroup_.end())
                    primary = memberOwner->second;
        }
        if (primary >= cxxImportGroups_.size()) return false;

        std::string receiverTypeSpelling;
        if (candidate.IsMethod && !candidate.Parameters.empty())
            CxxSpellingForCflatType(candidate.Parameters.front().TypeName,
                                    receiverTypeSpelling);

        std::vector<size_t> dependencies;
        std::unordered_set<std::string> visited;
        std::vector<std::string> tailSpellings;
        tailSpellings.reserve(arguments.size() - candidate.Parameters.size());
        uint64_t requestHash = 14695981039346656037ULL;
        auto hashText = [&](const std::string& text) {
            for (unsigned char byte : text)
                requestHash = (requestHash ^ byte) * 1099511628211ULL;
        };
        hashText(candidate.UniqueName);
        if (!receiverTypeSpelling.empty()) hashText(receiverTypeSpelling);
        for (size_t i = candidate.Parameters.size(); i < arguments.size(); ++i)
        {
            const NamedVariable& argument = arguments[i];
            const TypeAndValue& type = argument.TypeAndValue;
            if (type.IsFunctionPointer || type.IsArrayView || type.IsInterface || type.ElemPointer)
                return false;

            llvm::Type* valueType = argument.Primary != nullptr
                ? argument.Primary->getType() : GetType(type);
            std::string cflatType = type.TypeName;
            if (cflatType.empty())
                if (auto* record = llvm::dyn_cast_or_null<llvm::StructType>(valueType))
                    cflatType = record->getName().str();
            std::string spelling;
            if (valueType != nullptr && (valueType->isFloatTy() || valueType->isHalfTy()))
            {
                spelling = "double";
            }
            else if (valueType != nullptr && valueType->isIntegerTy()
                     && valueType->getIntegerBitWidth() < 32)
            {
                spelling = "int";
            }
            else
            {
                if (cflatType.empty())
                {
                    if (valueType == nullptr || !valueType->isIntegerTy()) return false;
                    const unsigned width = valueType->getIntegerBitWidth();
                    spelling = width <= 32 ? "int" : width <= 64 ? "long long" : "__int128";
                }
                else if (!CxxSpellingForCflatType(cflatType, spelling)) return false;
                if (type.Pointer)
                {
                    size_t pointerDepth = type.PointerDepth == 0 ? 1 : type.PointerDepth;
                    std::string base = cflatType;
                    while (base.ends_with("*")) { base.pop_back(); if (pointerDepth > 1) --pointerDepth; }
                    if (base == cflatType)
                        for (size_t depth = 0; depth < pointerDepth; ++depth) spelling += " *";
                }
            }
            tailSpellings.push_back(spelling);
            hashText(spelling);
            if (!cflatType.empty())
                CollectCxxTypeOwnerGroups(cflatType, dependencies, visited);
        }
        if (tailSpellings.empty()) return false;

        CxxRequestGroup group = MakeCxxRequestGroup(primary, dependencies);
        if (group.headers.empty()) return false;

        const std::string aliasStem = std::format("__cflat_t204_{:016x}_", requestHash);
        std::string aliasSource;
        std::vector<std::string> aliases;
        aliases.reserve(tailSpellings.size());
        std::string receiverAlias;
        if (!receiverTypeSpelling.empty())
        {
            receiverAlias = aliasStem + "receiver";
            aliasSource += "using " + receiverAlias + " = " + receiverTypeSpelling + ";\n";
            aliasSource += "static_assert(sizeof(" + receiverAlias + ") > 0);\n";
        }
        for (size_t i = 0; i < tailSpellings.size(); ++i)
        {
            std::string alias = aliasStem + std::to_string(i);
            aliasSource += "using " + alias + " = " + tailSpellings[i] + ";\n";
            aliases.push_back(std::move(alias));
        }

        cflat_cinterop::VariadicCallAbiRequest callRequest;
        callRequest.targetLinkageName = candidate.UniqueName;
        callRequest.instanceMethod = candidate.IsMethod;
        callRequest.receiverTypeAlias = receiverAlias;
        callRequest.argumentTypeAliases = aliases;
        const std::string requestSource = BuildCxxRequestIncludes(group) + aliasSource;
        const std::vector<std::string> requestArgs = BuildCxxRequestClangArgs(group);

        const std::string cacheName = "T204 variadic call ABI " + candidate.UniqueName;
        const std::string cacheKey = CxxTypeRequestCacheKey(
            group, cacheName, requestSource, requestArgs, /*emitDefinitions*/ false) + "|T204VARABI";
        CFileSigCacheEntry cached;
        std::string missReason;
        if (TryLoadCxxTypeRequestCache(group, cacheKey, /*emitDefinitions*/ false,
                                       cached, missReason))
        {
            if (!cached.sigs.empty() && cached.sigs.front().abi.valid)
            {
                abi = cached.sigs.front().abi;
                return true;
            }
        }

        cflat_cinterop::ExtractResult raw;
        if (!RunCxxTypeRequests(group, {}, aliasSource, /*emitDefinitions*/ false,
                                raw, error, {}, &callRequest)
            || !raw.hasVariadicCallAbi)
        {
            if (error.empty()) error = raw.firstError;
            return false;
        }
        CFileSigCacheEntry entry;
        entry.sigs.emplace_back();
        entry.sigs.back().abi = raw.variadicCallAbi;
        StoreCxxTypeRequestCache(group, cacheKey, /*emitDefinitions*/ false,
                                 std::move(entry), group.diskCache);
        abi = std::move(raw.variadicCallAbi);
        return true;
}

/*
 * Identity of a C++ type request: the OWNING import group's headers and defines (never every C++
 * header imported so far), the C++ include dirs, the CFlat and C++ instantiation spellings, the
 * emit mode (an LSP bind carries no bodies and an empty companion module, which a compile must
 * never reuse), the bitfield packing mode of the resolved target, and the cache schema version.
 * The compiler build stamp is never keyed; CacheBuildStampEnabled() checks it inside the stored
 * entry. Shares the C header signature cache, so it shares its row budget and its LRU/root pinning.
 */
std::string LLVMBackend::CxxTypeRequestCacheKey(const CxxRequestGroup& group,
                                                const std::string& cxxSpelling,
                                                const std::string& requestSource,
                                                const std::vector<std::string>& clangArgs,
                                                bool emitDefinitions) const
{
        std::string key = "|RQ" + cxxSpelling;
        key += "|STD" + cppStandard_;
        for (const auto& h : group.headers)     key += "|H" + h;
        for (const auto& inc : cIncludeDirs_)   key += "|I" + inc;
        for (const auto& def : cDefines_)       key += "|D" + def;
        for (const auto& def : group.defines)   key += "|d" + def;
        std::filesystem::file_time_type stamp{};
        if (CxxGroupHeaderStamp(group, stamp))
            key += "|T" + std::to_string((long long)stamp.time_since_epoch().count());
        key += std::format("|C{:016x}", CxxGroupClosureHash(group));
        key += emitDefinitions ? "|EDEF" : "|EDECL";
        // Imported records pack bitfields by the MSVC rule on a Windows target and the Itanium
        // rule elsewhere, so a cached layout from the other target is not reusable.
        key += targetWindows_ ? "|BFMS" : "|BFIT";
        uint64_t sourceHash = 14695981039346656037ULL;
        for (unsigned char byte : requestSource)
        {
            sourceHash ^= byte;
            sourceHash *= 1099511628211ULL;
        }
        key += std::format("|S{:016x}-{}", sourceHash, requestSource.size());
        for (const auto& arg : clangArgs) key += "|A" + arg;
        // Schema version, NOT the compiler build stamp: the build stamp is checked inside the entry
        // (CacheBuildStampEnabled, on by default), so a rebuild rewrites this key's file in place.
        // Under CFLAT_CACHE_BUILD_STAMP=0 a rebuilt cflat reuses it; kCHeaderCacheVersion is what
        // makes an incompatible entry miss - bump it whenever the harvest changes.
        key += "|V" + std::to_string(kCHeaderCacheVersion);
        if (cflat_cinterop::CxxEagerBodies()) key += "|BEAGER";
        return key;
    }

std::string LLVMBackend::CxxTypeRequestCacheKey(const CxxRequestGroup& group,
                                                const CxxRequestItem& item,
                                                const std::string& requestSource,
                                                const std::vector<std::string>& clangArgs,
                                                bool emitDefinitions) const
{
        return CxxTypeRequestCacheKey(group, item.cxxSpelling, requestSource, clangArgs,
                                      emitDefinitions)
             + "|F" + item.cflatName
             + (item.needDefinitions ? "|FULL" : "|LAYOUT")
             + (item.explicitInstantiation ? "|INST" : "|NOINST")
             + (item.instantiateMembers ? "|MEMBERS" : "");
    }

// Newest mtime over the group's headers; false when any of them cannot be stat'ed.
bool LLVMBackend::CxxGroupHeaderStamp(const CxxRequestGroup& group,
                                      std::filesystem::file_time_type& newest) const
{
        newest = std::filesystem::file_time_type{};
        for (const auto& h : group.headers)
        {
            auto [memo, fresh] = cxxGroupHeaderStampMemo_.try_emplace(h);
            if (fresh)
            {
                std::error_code ec;
                auto mt = std::filesystem::last_write_time(h, ec);
                if (!ec) memo->second = mt;
            }
            if (!memo->second.has_value()) return false;
            if (*memo->second > newest) newest = *memo->second;
        }
        return true;
    }

/*
 * Content of everything the request group's import groups transitively include (`|C` in the
 * request key, and folded into the entry's content hash). A request's entry and the group's
 * cached demand companion carry bodies from included headers too (a struct defined in `a.h`,
 * requested through `import cpp "b.h"` that includes it), so an edit to `a.h` alone must miss
 * them - keyed on the top-level headers only, a warm compile replayed the old bodies: stale
 * values, or a definition that was strong before the edit colliding with the one now emitted
 * inline. Content, not mtime: a touch keeps hitting, and an edit rewrites the key's entry
 * instead of stranding it. The per-file hashes are the harvest entry's dependency hashes,
 * already checked fresh (mtime equal, else content equal) when the harvest loaded, so this
 * stats and reads nothing. 0 for a group whose closure is unknown (no cache directory).
 */
uint64_t LLVMBackend::CxxGroupClosureHash(const CxxRequestGroup& group) const
{
        uint64_t combined = 14695981039346656037ULL;
        auto fold = [&](size_t index) {
            if (index >= cxxImportGroups_.size()) return;
            const CxxImportGroup& g = cxxImportGroups_[index];
            if (g.closureHashFiles != g.closureContent.size())
            {
                uint64_t h = 14695981039346656037ULL;
                for (const auto& [path, content] : g.closureContent)
                {
                    for (unsigned char byte : path) { h ^= byte; h *= 1099511628211ULL; }
                    h ^= content; h *= 1099511628211ULL;
                }
                g.closureHash = h;
                g.closureHashFiles = g.closureContent.size();
            }
            combined ^= g.closureHash; combined *= 1099511628211ULL;
        };
        fold(group.primary);
        for (size_t d : group.dependencyGroups) fold(d);
        return combined;
    }

/*
 * Share the primary import's fresh hash with later type requests. Imports force a read even
 * when the stamp matches: their hash also invalidates the demand companion on same-stamp edits.
 * Type requests retain their existing per-process (path, mtime, size) memo.
 */
bool LLVMBackend::HashCxxHeaderFileContents(const std::string& path, uint64_t& outHash,
                                          bool force)
{
        struct StampedHash
        {
            std::filesystem::file_time_type mtime;
            uintmax_t size = 0;
            uint64_t hash = 0;
        };
        static std::mutex mutex;
        static std::unordered_map<std::string, StampedHash> hashes;

        std::error_code timeError, sizeError;
        const auto mtime = std::filesystem::last_write_time(path, timeError);
        const uintmax_t size = std::filesystem::file_size(path, sizeError);
        const bool stamped = !timeError && !sizeError;
        if (stamped && !force)
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = hashes.find(path);
            if (it != hashes.end() && it->second.mtime == mtime && it->second.size == size)
            {
                outHash = it->second.hash;
                return true;
            }
        }
        if (!HashFileFnv1a(path, outHash)) return false;
        if (stamped)
        {
            std::lock_guard<std::mutex> lock(mutex);
            hashes[path] = StampedHash{ mtime, size, outHash };
        }
        return true;
}

uint64_t LLVMBackend::CxxGroupHeaderHash(const CxxRequestGroup& group) const
{
        uint64_t combined = 14695981039346656037ULL;
        for (const auto& h : group.headers)
        {
            auto [memo, fresh] = cxxGroupHeaderHashMemo_.try_emplace(h, 0);
            if (fresh) HashCxxHeaderFileContents(h, memo->second);
            const uint64_t hash = memo->second;
            combined ^= hash; combined *= 1099511628211ULL;
        }
        combined ^= CxxGroupClosureHash(group); combined *= 1099511628211ULL;
        return combined;
    }

 cflat_cinterop::RawRecord LLVMBackend::RawRecordFromCxxCache(const CRecordEntry& cached)
{
        cflat_cinterop::RawRecord raw;
        raw.name = cached.name;
        raw.isUnion = cached.isUnion;
        raw.isCxx = cached.isCxx;
        raw.specialMembersPending = cached.specialMembersPending;
        raw.isPacked = cached.isPacked;
        raw.sizeBytes = cached.sizeBytes;
        raw.alignBytes = cached.alignBytes;
        raw.isTrivial = cached.isTrivial;
        raw.isTriviallyCopyable = cached.isTriviallyCopyable;
        raw.isTriviallyRelocatable = cached.isTriviallyRelocatable;
        raw.scopeNames = cached.scopeNames;
        raw.isPolymorphic = cached.isPolymorphic;
        raw.vtableDeleteVerdict = cached.vtableDeleteVerdict;
        raw.hasBases = cached.hasBases;
        raw.hasVirtualBases = cached.hasVirtualBases;
        raw.isFinal = cached.isFinal;
        raw.isAbstract = cached.isAbstract;
        raw.hasFriendOperators = cached.hasFriendOperators;
        raw.friendNames = cached.friendNames;
        raw.bases = cached.bases;
        raw.virtualBases = cached.virtualBases;
        raw.layoutRefusal = cached.layoutRefusal;
        raw.canonicalCtype = cached.canonicalCtype;
        raw.hasTrivialDefaultCtor = cached.hasTrivialDefaultCtor;
        raw.hasTrivialCopyCtor = cached.hasTrivialCopyCtor;
        raw.hasTrivialCopyAssign = cached.hasTrivialCopyAssign;
        raw.hasTrivialMoveAssign = cached.hasTrivialMoveAssign;
        raw.hasTrivialDtor = cached.hasTrivialDtor;
        raw.paramDestroyedInCallee = cached.paramDestroyedInCallee;
        raw.hasDeletedDefaultCtor = cached.hasDeletedDefaultCtor;
        raw.hasDeletedCopyCtor = cached.hasDeletedCopyCtor;
        raw.hasDefaultCtor = cached.hasDefaultCtor;
        raw.hasCopyCtor = cached.hasCopyCtor;
        raw.hasCtorTemplate = cached.hasCtorTemplate;
        raw.hasAssignTemplate = cached.hasAssignTemplate;
        raw.moveAssignBindsConstRef = cached.moveAssignBindsConstRef;
        raw.moveAssignConstSourceBindsConstRef = cached.moveAssignConstSourceBindsConstRef;
        raw.isAggregate = cached.isAggregate;
        raw.members = cached.members;
        raw.ctorTemplates = cached.ctorTemplates;
        raw.staticVars = cached.staticVars;
        raw.qualifiedName = cached.qualifiedName.empty() ? cached.name : cached.qualifiedName;
        raw.file = cached.file;
        raw.inScope = cached.inScope;
        raw.uuid = cached.uuid;
        raw.line = cached.line;
        raw.col = cached.col;
        for (const CRecordFieldEntry& field : cached.fields)
        {
            cflat_cinterop::RawField rawField;
            rawField.name = field.name;
            rawField.ctype = field.ctype;
            rawField.access = field.access;
            rawField.isBitfield = field.isBitfield;
            rawField.isPromoted = field.isPromoted;
            rawField.isZeroSize = field.isZeroSize;
            rawField.isConst = field.isConst;
            rawField.isMutable = field.isMutable;
            rawField.inherited = field.inherited;
            rawField.bitWidth = field.bitWidth;
            rawField.offsetBytes = field.offsetBytes;
            rawField.sizeBytes = field.sizeBytes;
            rawField.alignBytes = field.alignBytes;
            rawField.bitOffset = field.bitOffset;
            raw.fields.push_back(std::move(rawField));
        }
        return raw;
    }

bool LLVMBackend::TryLoadCxxTypeRequestCache(const CxxRequestGroup& group,
                                             const std::string& requestKey,
                                             bool emitDefinitions,
                                             CFileSigCacheEntry& out,
                                             std::string& missReason,
                                             bool allowDisk)
{
        llvm::TimeTraceScope scope("TryLoadCxxTypeRequestCache", requestKey);
        missReason = "missing entry";
        const size_t labelEnd = requestKey.find("|H");
        const std::string requestLabel = requestKey.starts_with("|RQ")
            ? requestKey.substr(3, labelEnd == std::string::npos ? std::string::npos : labelEnd - 3)
            : requestKey;
        std::filesystem::file_time_type headerMtime{};
        if (!CxxGroupHeaderStamp(group, headerMtime))
        {
            missReason = "header stamp unavailable";
            if (verbose && group.diskCache)
                llvm::errs() << std::format(
                    "[verbose] C++ type request cache MISS for {} ({})\n",
                    requestLabel, missReason);
            return false;
        }
        const uint64_t headerHash = CxxGroupHeaderHash(group);
        {
            std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
            auto it = cFileSigCache_.find(requestKey);
            if (it != cFileSigCache_.end())
            {
                if ((it->second.mtime == headerMtime || it->second.hash == headerHash)
                    && (!emitDefinitions || !it->second.cxxBitcode.empty()
                        || !it->second.cxxDemandGroupKey.empty())
                    && CxxDemandEntryUsable(/*cxxEntry*/ true))
                {
                    it->second.mtime = headerMtime;
                    TouchCFileSigEntry(requestKey, it->second);
                    out = it->second;
                    NoteCxxDemandEntry(CxxIncrementalGroupKey(group), out.hash,
                                       HashWrapperKey(requestKey),
                                       cxxDemandLiveKeys_.count(requestKey) == 0,
                                       !out.cxxDemandGroupKey.empty());
                    if (!out.cxxDemandGroupKey.empty())
                        NoteCxxDemandReplayChunk(group, out.cxxDemandReplayChunk);
                    if (verbose) llvm::errs() << std::format(
                        "[verbose] C++ type request cache HIT for {} (memory)\n", requestLabel);
                    return true;
                }
                missReason = it->second.mtime == headerMtime || it->second.hash == headerHash
                    ? "missing sidecar" : "header stamp";
            }
        }

        if (!allowDisk) return false;
        const std::string cacheDir = GetCHeaderCacheDir();
        if (!group.diskCache || cacheDir.empty())
        {
            if (verbose && group.diskCache)
            {
                missReason = "cache directory unavailable";
                llvm::errs() << std::format(
                    "[verbose] C++ type request cache MISS for {} ({})\n",
                    requestLabel, missReason);
            }
            return false;
        }
        uint64_t diskKey = 14695981039346656037ULL;
        for (unsigned char byte : requestKey)
        {
            diskKey ^= byte;
            diskKey *= 1099511628211ULL;
        }
        CFileSigCacheEntry diskEntry;
        std::string diskReason;
        if (cxxTypeRequestDiskAbsent_.contains(diskKey))
        {
            missReason = "missing entry";
            if (verbose) llvm::errs() << std::format(
                "[verbose] C++ type request cache MISS for {} ({})\n", requestLabel, missReason);
            return false;
        }
        {
            llvm::TimeTraceScope loadScope("CxxTypeRequestJsonLoad", requestKey);
            if (!TryLoadCHeaderDiskCache(cacheDir, diskKey, headerMtime,
                                         [&] { return headerHash; }, diskEntry,
                                         requestKey, emitDefinitions, &diskReason,
                                         !batchMode_ && !runMode_ && symbolSink_ == nullptr))
            {
                if (diskReason == "missing entry") cxxTypeRequestDiskAbsent_.insert(diskKey);
                missReason = diskReason.empty() ? missReason : diskReason;
                if (verbose) llvm::errs() << std::format(
                    "[verbose] C++ type request cache MISS for {} ({})\n",
                    requestLabel, missReason);
                return false;
            }
        }
        if (!CxxDemandEntryUsable(/*cxxEntry*/ true))
        {
            missReason = "demand-group retry";
            return false;
        }
        // Any served entry, shape-only included, means the live group never parsed its chunk.
        NoteCxxDemandEntry(CxxIncrementalGroupKey(group), diskEntry.hash, diskKey,
                           /*served*/ true, !diskEntry.cxxDemandGroupKey.empty());
        if (!diskEntry.cxxDemandGroupKey.empty())
            NoteCxxDemandReplayChunk(group, diskEntry.cxxDemandReplayChunk);
        /*
         * Only a process that analyzes again (a --check batch, symbol queries, the LSP) can hit
         * this entry in memory later. A one-shot compile would copy every loaded entry - the
         * group's whole signature surface - to serve a handful of repeats, which cost less
         * re-read from disk.
         */
        if (batchMode_ || symbolSink_ != nullptr)
        {
            std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
            InsertCFileSigEntry(requestKey, CFileSigCacheEntry(diskEntry), verbose);
        }
        out = std::move(diskEntry);
        if (verbose) llvm::errs() << std::format(
            "[verbose] C++ type request cache HIT for {} (disk)\n", requestLabel);
        return true;
    }

void LLVMBackend::StoreCxxTypeRequestCache(const CxxRequestGroup& group,
                                           const std::string& requestKey,
                                           bool emitDefinitions,
                                           CFileSigCacheEntry&& entry,
                                           bool allowDisk,
                                           const char* allowDiskReason)
{
        llvm::TimeTraceScope scope("StoreCxxTypeRequestCache", requestKey);
        // |ENUM and |CONST request results carry only enums or globals; they persist too.
        if (entry.sigs.empty() && entry.records.empty() && entry.macros.empty()
            && entry.enums.empty() && entry.globals.empty()) return;
        std::filesystem::file_time_type headerMtime{};
        if (!CxxGroupHeaderStamp(group, headerMtime)) return;
        const uint64_t headerHash = CxxGroupHeaderHash(group);
        entry.mtime = headerMtime;
        entry.hash = headerHash;
        NoteCxxDemandEntry(CxxIncrementalGroupKey(group), entry.hash, HashWrapperKey(requestKey),
                           /*served*/ false, !entry.cxxDemandGroupKey.empty());
        if (!entry.cxxDemandGroupKey.empty()
            && (!entry.cxxDemandReplayChunk.source.empty()
                || entry.cxxDemandReplayChunk.headerHarvest))
            NoteCxxDemandReplayChunk(group, entry.cxxDemandReplayChunk);
        cxxDemandLiveKeys_.insert(requestKey);
        {
            std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
            InsertCFileSigEntry(requestKey, CFileSigCacheEntry(entry), verbose);
        }
        const bool generatedRequest = requestKey.find("__cflat_user") != std::string::npos;
        auto reportRefusal = [&](const char* reason) {
            if (verbose && generatedRequest)
                std::cout << std::format(
                    "[verbose] C++ type request cache STORE REFUSED for {} ({})\n",
                    requestKey, reason);
        };
        if (!allowDisk)
        {
            reportRefusal(allowDiskReason != nullptr ? allowDiskReason : "caller disallowed");
            return;
        }
        if (!group.diskCache)
        {
            reportRefusal("no import group supplies a header");
            return;
        }
        if (runMode_)
        {
            reportRefusal("run mode");
            return;
        }
        if (batchMode_)
        {
            reportRefusal("batch mode");
            return;
        }
        if (symbolSink_ != nullptr)
        {
            reportRefusal("symbol lookup");
            return;
        }
        if (emitDefinitions && entry.cxxBitcode.empty() && entry.cxxDemandGroupKey.empty())
        {
            reportRefusal("missing definitions sidecar");
            return;
        }
        const std::string cacheDir = GetCHeaderCacheDir();
        if (cacheDir.empty())
        {
            reportRefusal("cache directory unavailable");
            return;
        }
        uint64_t diskKey = 14695981039346656037ULL;
        for (unsigned char byte : requestKey)
        {
            diskKey ^= byte;
            diskKey *= 1099511628211ULL;
        }
        ScheduleCHeaderDiskCacheWrite(cacheDir, diskKey, headerMtime, headerHash,
                                      std::move(entry), requestKey, &group);
        if (verbose) llvm::errs() << std::format(
            "[verbose] C++ type request cache STORE {} ({})\n",
            requestKey, emitDefinitions ? "definitions" : "declarations");
    }

/*
 * One `import cpp` statement, identified by its ordered headers plus its defines - the same tuple
 * the header disk cache hashes. A second import of the same headers with the same defines is the
 * same group, so a repeated import does not split the request cache.
 */
cflat_cinterop::CxxImportOrder LLVMBackend::CxxImportOrderAt(uint64_t sourceOffset) const
{
        auto order = currentCxxImportOrderPath_;
        order.push_back(sourceOffset);
        return order;
}

size_t LLVMBackend::FindOrAddCxxImportGroup(const std::vector<std::string>& headers,
                                            const std::vector<std::string>& defines)
{
        for (size_t i = 0; i < cxxImportGroups_.size(); ++i)
            if (cxxImportGroups_[i].headers == headers && cxxImportGroups_[i].defines == defines)
            {
                const auto& order = currentCxxImportOrderPath_.empty()
                    ? cflat_cinterop::CxxImportOrder{ static_cast<uint64_t>(-1) }
                    : currentCxxImportOrderPath_;
                if (cxxImportGroups_[i].importOrder.empty()
                    || order < cxxImportGroups_[i].importOrder)
                    cxxImportGroups_[i].importOrder = order;
                return i;
            }
        CxxImportGroup group;
        group.importOrder = currentCxxImportOrderPath_.empty()
            ? cflat_cinterop::CxxImportOrder{ static_cast<uint64_t>(-1) }
            : currentCxxImportOrderPath_;
        group.headers = headers;
        group.defines = defines;
        cxxImportGroups_.push_back(std::move(group));
        return cxxImportGroups_.size() - 1;
    }

// A class specialization request needs its owner and nested template-argument headers.
LLVMBackend::CxxRequestGroup LLVMBackend::MakeCxxClassRequestGroup(
        size_t owner, const std::string& typeName, const std::vector<size_t>& deps) const
{
        std::vector<size_t> groups = deps;
        std::unordered_set<std::string> visited;
        CollectCxxTypeOwnerGroups(typeName, groups, visited);
        return MakeCxxRequestGroup(owner, groups);
}

/*
 * The primary group's headers first, then the headers of the groups that own the requested
 * spelling's template arguments, appended in a canonical (sorted) order so the prologue and the
 * cache key do not depend on the order the file imported them in.
 */
LLVMBackend::CxxRequestGroup LLVMBackend::MakeCxxRequestGroup(size_t primary,
                                                              const std::vector<size_t>& deps) const
{
        CxxRequestGroup out;
        if (primary >= cxxImportGroups_.size()) return out;
        out.primary = primary;
        out.headers = cxxImportGroups_[primary].headers;
        out.defines = cxxImportGroups_[primary].defines;
        out.ownerHeaders = out.headers;
        out.ownerDefines = out.defines;
        const bool primaryHasHeaders = !cxxImportGroups_[primary].headers.empty();
        bool hasDependencyGroup = false;
        std::vector<size_t> sorted;
        for (size_t d : deps)
            if (d != primary && d < cxxImportGroups_.size()
                && std::find(sorted.begin(), sorted.end(), d) == sorted.end())
                sorted.push_back(d);
        std::sort(sorted.begin(), sorted.end(), [&](size_t a, size_t b) {
            const auto& ha = cxxImportGroups_[a].headers;
            const auto& hb = cxxImportGroups_[b].headers;
            if (ha.empty() || hb.empty()) return hb.empty() && !ha.empty();
            return ha.front() < hb.front();
        });
        out.dependencyGroups = sorted;
        for (size_t d : sorted)
        {
            hasDependencyGroup = true;
            for (const auto& h : cxxImportGroups_[d].headers)
                if (std::find(out.headers.begin(), out.headers.end(), h) == out.headers.end())
                    out.headers.push_back(h);
            for (const auto& def : cxxImportGroups_[d].defines)
                if (std::find(out.defines.begin(), out.defines.end(), def) == out.defines.end())
                    out.defines.push_back(def);
        }
        // Every import is disk-cached. A request with no header at all (a CFlat struct handed to
        // C++ that needs nothing but itself) is keyed by its whole generated source, and is
        // cacheable as soon as the compile imports any C++ header.
        out.diskCache = primaryHasHeaders || hasDependencyGroup
            || std::any_of(cxxImportGroups_.begin(), cxxImportGroups_.end(),
                           [](const CxxImportGroup& g) { return !g.headers.empty(); });
        out.pruneScope = "STD" + cppStandard_ + (targetWindows_ ? "|W" : "|N");
        out.pruneScope += "|T" + CInteropTargetTriple();
        for (const auto& h : out.headers)     out.pruneScope += "|H" + h;
        for (const auto& def : out.defines)   out.pruneScope += "|d" + def;
        for (const auto& inc : cIncludeDirs_) out.pruneScope += "|I" + inc;
        for (const auto& def : cDefines_)     out.pruneScope += "|D" + def;
        out.label = out.headers.empty()
            ? std::string("<no header>")
            : std::filesystem::path(out.headers.front()).filename().string();
        return out;
    }

// Qualified C++ names an import group brought in, so a later CFlat spelling resolves without a probe.
void LLVMBackend::PublishCxxGroupNames(size_t group, const std::vector<CRecordEntry>& records)
{
        if (group >= cxxImportGroups_.size()) return;
        CxxImportGroup& g = cxxImportGroups_[group];
        for (const CRecordEntry& record : records)
        {
            if (record.name.empty()) continue;
            std::string dotted = record.name;
            const size_t mangled = dotted.find('$');
            if (mangled != std::string::npos) dotted.erase(mangled);
            std::string cxxName = dotted;
            size_t pos = 0;
            while ((pos = cxxName.find('.', pos)) != std::string::npos)
            { cxxName.replace(pos, 1, "::"); pos += 2; }
            g.publishedNames.insert(cxxName);
            cxxTypeOwnerGroup_.emplace(record.name, group);
    }
}

void LLVMBackend::RegisterCxxFunctionTemplates(
        const std::vector<cflat_cinterop::RawFunctionTemplate>& templates, size_t group,
        const std::string& fileForLsp)
{
        for (const auto& t : templates)
        {
            if (t.name.empty() || t.cxxSpelling.empty()) continue;
            auto& entries = cxxFunctionTemplates_[t.name];
            const bool duplicate = std::any_of(entries.begin(), entries.end(), [&](const auto& old) {
                return old.kind == t.kind && old.cxxSpelling == t.cxxSpelling
                    && old.minArity == t.minArity && old.maxArity == t.maxArity
                    && old.typeParameterCount == t.typeParameterCount
                    && old.hasParameterPack == t.hasParameterPack
                    && old.parameterTypes == t.parameterTypes;
            });
            if (duplicate) continue;
            entries.push_back(t);
            if (group < cxxImportGroups_.size())
                cxxFunctionTemplateOwnerGroup_.emplace(t.name, group);
            if (!t.owner.empty())
            {
                std::string ownerCxx = "::" + t.owner;
                for (size_t pos = 0; (pos = ownerCxx.find('.', pos)) != std::string::npos; )
                {
                    ownerCxx.replace(pos, 1, "::");
                    pos += 2;
                }
                cxxCflatToCxxSpelling_.emplace(t.owner, ownerCxx);
                cxxForeignTypeSpellings_.emplace(SqueezeCxxSpelling(ownerCxx), t.owner);
            }
            NoteCxxForeignNamespace(t.name);
            if (auto* sink = GetSymbolSink())
            {
                const std::string& declFile = t.file.empty() ? fileForLsp : t.file;
                sink->Register(SymbolKind::Function, t.name, declFile,
                               t.line, t.col < 0 ? 0 : t.col,
                               "template <...> " + t.name + "(...)");
            }
        }
}

bool LLVMBackend::HasCxxFunctionTemplate(const std::string& qualifiedName) const
{
        auto it = cxxFunctionTemplates_.find(qualifiedName);
        if (it != cxxFunctionTemplates_.end() && !it->second.empty()) return true;
        const size_t dot = qualifiedName.rfind('.');
        return dot != std::string::npos
            && !ResolveCxxFunctionTemplateName(qualifiedName.substr(0, dot),
                                               qualifiedName.substr(dot + 1)).empty();
}

bool LLVMBackend::HasCxxFunctionTemplateForRecordArgument(
        const std::string& qualifiedName, const std::string& recordType, size_t arity) const
{
        std::string resolvedName = qualifiedName;
        auto templates = cxxFunctionTemplates_.find(resolvedName);
        if (templates == cxxFunctionTemplates_.end())
        {
            const size_t dot = qualifiedName.rfind('.');
            if (dot == std::string::npos) return false;
            resolvedName = ResolveCxxFunctionTemplateName(
                qualifiedName.substr(0, dot), qualifiedName.substr(dot + 1));
            templates = cxxFunctionTemplates_.find(resolvedName);
            if (templates == cxxFunctionTemplates_.end()) return false;
        }

        auto stripSpecialization = [](const std::string& type) {
            const size_t separator = type.find('$');
            return type.substr(0, separator);
        };
        const std::string recordBase = stripSpecialization(recordType);
        static const std::set<std::string> builtins = {
            "void", "bool", "char", "wchar", "c8", "c16", "c32", "short", "u16",
            "int", "u32", "long", "ulong", "i64", "u64", "float", "double",
            "longdouble", "i8", "u8"
        };
        for (const auto& candidate : templates->second)
        {
            if (candidate.kind != cflat_cinterop::RawFunctionTemplate::Free
                || arity < candidate.minArity || arity > candidate.maxArity
                || candidate.parameterTypes.empty())
                continue;

            std::string parameterType = candidate.parameterTypes.front();
            if (parameterType.find('*') != std::string::npos) continue;
            for (const char* tag : { "class ", "struct ", "union " })
                if (parameterType.starts_with(tag))
                {
                    parameterType.erase(0, std::strlen(tag));
                    break;
                }
            for (const char* qualifier : { "const", "volatile" })
                for (size_t pos = 0; (pos = parameterType.find(qualifier, pos)) != std::string::npos; )
                {
                    const size_t end = pos + std::strlen(qualifier);
                    const bool leftBoundary = pos == 0
                        || std::isspace(static_cast<unsigned char>(parameterType[pos - 1]));
                    const bool rightBoundary = end == parameterType.size()
                        || std::isspace(static_cast<unsigned char>(parameterType[end]));
                    if (leftBoundary && rightBoundary) parameterType.erase(pos, end - pos);
                    else pos = end;
                }
            parameterType.erase(std::remove(parameterType.begin(), parameterType.end(), '&'),
                                parameterType.end());
            const size_t first = parameterType.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) continue;
            parameterType.erase(0, first);
            const size_t last = parameterType.find_last_not_of(" \t\r\n");
            parameterType.erase(last + 1);

            const std::string parameterIdentity = CanonicalCxxForeignIdentity(parameterType);
            if (parameterIdentity == recordType || parameterIdentity == recordBase
                || IsCxxBaseOf(parameterIdentity, recordType))
                return true;

            const bool simpleTypeName = !parameterType.empty()
                && std::all_of(parameterType.begin(), parameterType.end(), [](unsigned char c) {
                    return std::isalnum(c) || c == '_';
                });
            if (candidate.typeParameterCount != 0 && simpleTypeName
                && !builtins.count(parameterIdentity)
                && !IsCxxRecord(parameterIdentity)
                && !IsCxxForeignTypeRegistered(parameterIdentity))
                return true;
        }
        return false;
}

bool LLVMBackend::HasArityViableCxxFunctionTemplate(
        const std::string& functionName, const std::string& ownerType, size_t arity) const
{
        const std::string resolved = ownerType.empty()
            ? functionName : ResolveCxxFunctionTemplateName(ownerType, functionName);
        auto templates = cxxFunctionTemplates_.find(resolved);
        if (templates == cxxFunctionTemplates_.end()) return false;
        return std::any_of(templates->second.begin(), templates->second.end(),
            [&](const cflat_cinterop::RawFunctionTemplate& candidate) {
                const bool kindMatches = ownerType.empty()
                    ? candidate.kind == cflat_cinterop::RawFunctionTemplate::Free
                        || candidate.kind == cflat_cinterop::RawFunctionTemplate::StaticMember
                    : candidate.kind == cflat_cinterop::RawFunctionTemplate::InstanceMember
                        || candidate.kind == cflat_cinterop::RawFunctionTemplate::StaticMember;
                return kindMatches && arity >= candidate.minArity
                    && arity <= candidate.maxArity;
            });
}

bool LLVMBackend::IsCxxFunctionTemplateParameterByValue(const std::string& functionName,
                                                        const std::string& ownerType,
                                                        size_t parameterIndex,
                                                        size_t arity) const
{
        const std::string resolved = ownerType.empty()
            ? functionName
            : ResolveCxxFunctionTemplateName(ownerType, functionName);
        auto templates = cxxFunctionTemplates_.find(resolved);
        if (templates == cxxFunctionTemplates_.end()) return false;
        bool sawCandidate = false;
        for (const auto& candidate : templates->second)
        {
            const bool kindMatches = ownerType.empty()
                ? candidate.kind == cflat_cinterop::RawFunctionTemplate::Free
                    || candidate.kind == cflat_cinterop::RawFunctionTemplate::StaticMember
                : candidate.kind == cflat_cinterop::RawFunctionTemplate::InstanceMember
                    || candidate.kind == cflat_cinterop::RawFunctionTemplate::StaticMember;
            if (!kindMatches || arity < candidate.minArity || arity > candidate.maxArity)
                continue;
            sawCandidate = true;
            size_t index = parameterIndex;
            if (candidate.hasParameterPack && !candidate.parameterTypes.empty()
                && index >= candidate.parameterTypes.size() - 1)
                index = candidate.parameterTypes.size() - 1;
            if (index >= candidate.parameterTypes.size()
                || candidate.parameterTypes[index].find('&') != std::string::npos)
                return false;
        }
        return sawCandidate;
}

bool LLVMBackend::IsCxxFreeFunctionParameterByValue(const std::string& functionName,
                                                    size_t parameterIndex,
                                                    size_t arity) const
{
        auto signatures = cxxFunctionSignatures_.find(functionName);
        if (signatures == cxxFunctionSignatures_.end()) return false;
        bool sawCandidate = false;
        for (const auto& signature : signatures->second)
        {
            if (!signature.isCxx || signature.paramSpellings.size() != arity) continue;
            sawCandidate = true;
            if (parameterIndex >= signature.paramSpellings.size()
                || signature.paramSpellings[parameterIndex].find('&') != std::string::npos)
                return false;
        }
        return sawCandidate;
}

bool LLVMBackend::IsCxxMemberFunctionParameterByValue(const std::string& ownerType,
                                                      const std::string& memberName,
                                                      size_t parameterIndex,
                                                      size_t arity) const
{
        auto record = cxxRecordEntries_.find(ownerType);
        if (record == cxxRecordEntries_.end()) return false;
        bool sawCandidate = false;
        for (const auto& member : record->second.members)
        {
            if (member.name != memberName
                || member.kind != cflat_cinterop::RawCxxMember::Instance
                || member.variadic
                || member.paramTypes.size() < arity + 1
                || parameterIndex + 1 >= member.paramTypes.size())
                continue;
            bool arityFits = true;
            for (size_t index = arity + 1; index < member.paramTypes.size(); ++index)
                if (index >= member.defaultArgs.size() || member.defaultArgs[index].kind.empty())
                {
                    arityFits = false;
                    break;
                }
            if (!arityFits) continue;
            sawCandidate = true;
            if (member.paramTypes[parameterIndex + 1].find('&') != std::string::npos)
                return false;
        }
        return sawCandidate;
}

bool LLVMBackend::IsCxxConstructorParameterByValue(const std::string& typeName,
                                                   size_t parameterIndex,
                                                   size_t arity) const
{
        auto record = cxxRecordEntries_.find(typeName);
        if (record == cxxRecordEntries_.end() || !record->second.ctorTemplates.empty()) return false;
        bool sawCandidate = false;
        for (const auto& member : record->second.members)
        {
            if (member.kind != cflat_cinterop::RawCxxMember::Constructor
                || member.variadic || member.requiresConstructorWrapper
                || member.paramTypes.size() < parameterIndex + 2
                || member.paramTypes.size() < arity + 1)
                continue;
            bool arityFits = true;
            for (size_t index = arity + 1; index < member.paramTypes.size(); ++index)
                if (index >= member.defaultArgs.size() || member.defaultArgs[index].kind.empty())
                {
                    arityFits = false;
                    break;
                }
            if (!arityFits) continue;
            sawCandidate = true;
            if (member.paramTypes[parameterIndex + 1].find('&') != std::string::npos)
                return false;
        }
        return sawCandidate;
}

bool LLVMBackend::GetUniqueCxxConstructorValueParameter(const std::string& typeName,
                                                        size_t parameterIndex,
                                                        size_t arity,
                                                        TypeAndValue& out, bool constRef) const
{
        const auto* info = GetCxxClassInfoWithConstructors(typeName);
        if (info == nullptr || info->hasCtorTemplate) return false;
        const CxxClassInfo::Structor* match = nullptr;
        for (const auto& ctor : info->constructors)
        {
            if (ctor.isDeleted || ctor.access != cflat_cinterop::AccessPublic
                || ctor.isCopyCtor || ctor.isMoveCtor
                || ctor.params.size() < arity + 1
                || parameterIndex + 1 >= ctor.params.size())
                continue;
            const size_t parameterCount = ctor.params.size() - 1;
            bool arityFits = true;
            for (size_t index = arity; index < parameterCount; ++index)
                if (index + 1 >= ctor.defaultArgs.size()
                    || ctor.defaultArgs[index + 1].kind.empty())
                {
                    arityFits = false;
                    break;
                }
            if (!arityFits) continue;
            const auto& parameter = ctor.params[parameterIndex + 1];
            // A const reference is an alias too; the class's own copy ctor is no target.
            if ((parameter.IsAlias && !constRef) || parameter.IsRvalueRef
                || parameter.CxxConstRef() != constRef
                || (constRef && parameter.TypeName == typeName))
                continue;
            if (match != nullptr) return false;
            match = &ctor;
        }
        if (match == nullptr) return false;
        out = match->params[parameterIndex + 1];
        // A const reference is spelled as a pointer here; the caller reads only its class name.
        return !out.TypeName.empty() && (constRef || !out.Pointer);
}

static bool CxxTemplateHasForwardingReferenceParameter(
        const cflat_cinterop::RawFunctionTemplate& candidate)
{
        return std::any_of(candidate.forwardingReferenceParameters.begin(),
                           candidate.forwardingReferenceParameters.end(),
                           [](uint8_t value) { return value != 0; });
}

bool LLVMBackend::HasCxxForwardingReferenceTemplate(const std::string& qualifiedName) const
{
        auto it = cxxFunctionTemplates_.find(qualifiedName);
        if (it == cxxFunctionTemplates_.end()) return false;
        return std::any_of(it->second.begin(), it->second.end(),
                           CxxTemplateHasForwardingReferenceParameter);
}

// Every dotted segment of `name` was harvested as a C++ namespace (`std.numbers`): a namespace
// path, not a class, an enum or an alias of one.
bool LLVMBackend::IsCxxNamespacePath(const std::string& name) const
{
        if (name.empty()) return false;
        for (size_t start = 0; start < name.size(); )
        {
            const size_t dot = name.find('.', start);
            const std::string segment = name.substr(start,
                dot == std::string::npos ? std::string::npos : dot - start);
            if (start == 0 ? cxxForeignNamespaces_.count(segment) == 0
                           : cxxNestedNamespaceNames_.count(segment) == 0)
                return false;
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        return true;
}

bool LLVMBackend::IsCxxNamespacePathOrScanned(const std::string& name) const
{
        if (IsCxxNamespacePath(name)) return true;
        if (name.empty()) return false;
        for (size_t start = 0; start < name.size(); )
        {
            const size_t dot = name.find('.', start);
            const std::string segment = name.substr(start,
                dot == std::string::npos ? std::string::npos : dot - start);
            if (cxxForeignNamespaces_.count(segment) == 0
                && (start == 0 || cxxNestedNamespaceNames_.count(segment) == 0))
                return false;
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        return true;
}

bool LLVMBackend::IsCxxNamespace(const std::string& name) const
{
        const size_t dot = name.find('.');
        const std::string lead = dot == std::string::npos ? name : name.substr(0, dot);
        return !lead.empty() && cxxForeignNamespaces_.count(lead) != 0;
}

std::string LLVMBackend::ResolveCxxFunctionTemplateName(const std::string& owner,
                                                        const std::string& memberName) const
{
        auto direct = [&](const std::string& candidate) -> std::string {
            const std::string exact = candidate + "." + memberName;
            auto exactIt = cxxFunctionTemplates_.find(exact);
            if (exactIt != cxxFunctionTemplates_.end() && !exactIt->second.empty()) return exact;

            std::string ownerSpelling;
            if (!CxxSpellingForCflatType(candidate, ownerSpelling))
                if (auto lazy = cxxLazyAliasSpecializations_.find(candidate);
                    lazy != cxxLazyAliasSpecializations_.end())
                    ownerSpelling = lazy->second;
            if (ownerSpelling.empty()) return {};
            const size_t templateStart = ownerSpelling.find('<');
            if (templateStart != std::string::npos) ownerSpelling.resize(templateStart);
            while (ownerSpelling.starts_with("::")) ownerSpelling.erase(0, 2);
            for (size_t pos = 0; (pos = ownerSpelling.find("::", pos)) != std::string::npos; )
            {
                ownerSpelling.replace(pos, 2, ".");
                ++pos;
            }
            const std::string resolved = ownerSpelling + "." + memberName;
            auto resolvedIt = cxxFunctionTemplates_.find(resolved);
            if (resolvedIt != cxxFunctionTemplates_.end() && !resolvedIt->second.empty())
                return resolved;

            // Class-template member templates may be recorded under `Owner<>.member` even
            // when the CFlat owner is a concrete specialization such as `Owner$void`.
            const size_t specialization = candidate.find('$');
            const std::string ownerBase = candidate.substr(0, specialization);
            for (const auto& [templateName, entries] : cxxFunctionTemplates_)
                if (std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
                        return entry.owner == ownerBase && entry.memberName == memberName;
                    }))
                    return templateName;
            if (ownerBase == "std.weak_ptr" && memberName == "owner_before")
                for (const auto& [templateName, entries] : cxxFunctionTemplates_)
                    if (std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
                            return entry.owner == "std.shared_ptr"
                                && entry.memberName == memberName;
                        }))
                        return templateName;
            return {};
        };

        std::set<std::string> seen;
        std::function<std::string(const std::string&)> find = [&](const std::string& candidate) {
            if (!seen.insert(candidate).second) return std::string{};
            if (std::string found = direct(candidate); !found.empty()) return found;
            auto record = cxxRecordEntries_.find(candidate);
            if (record == cxxRecordEntries_.end()) return std::string{};
            for (const auto& base : record->second.bases)
            {
                if (base.access != cflat_cinterop::AccessPublic) continue;
                const std::string baseName = ResolveCxxBaseIdentity(base);
                if (baseName.empty()) continue;
                if (std::string found = find(baseName); !found.empty()) return found;
            }
            return std::string{};
        };
        return find(owner);
}

bool LLVMBackend::HasCxxFunctionTemplateMember(const std::string& owner,
                                               const std::string& memberName) const
{
        return !ResolveCxxFunctionTemplateName(owner, memberName).empty();
}

bool LLVMBackend::HasCxxFunctionTemplateMemberName(const std::string& memberName) const
{
        for (const auto& item : cxxFunctionTemplates_)
            if (std::any_of(item.second.begin(), item.second.end(), [&](const auto& entry) {
                    return entry.kind != cflat_cinterop::RawFunctionTemplate::Free
                        && entry.memberName == memberName;
                }))
                return true;
        return false;
}

bool LLVMBackend::IsCxxStaticOnlyFunctionTemplateMember(const std::string& owner,
                                                         const std::string& memberName) const
{
        const std::string resolved = ResolveCxxFunctionTemplateName(owner, memberName);
        auto templates = cxxFunctionTemplates_.find(resolved);
        // Every candidate static: an instance overload of the same name keeps the receiver.
        return templates != cxxFunctionTemplates_.end() && !templates->second.empty()
            && std::all_of(templates->second.begin(), templates->second.end(),
                [](const cflat_cinterop::RawFunctionTemplate& candidate) {
                    return candidate.kind == cflat_cinterop::RawFunctionTemplate::StaticMember;
                });
}

bool LLVMBackend::HasCxxForwardingReferenceTemplateMember(const std::string& owner,
                                                          const std::string& memberName) const
{
        const std::string resolved = ResolveCxxFunctionTemplateName(owner, memberName);
        if (resolved.empty()) return false;
        return HasCxxForwardingReferenceTemplate(resolved);
}

// The headers a request's TU includes itself: the prologue's `#include <new>`
// (BuildCxxRequestIncludes) and the group's headers.







// `const long (&)[N]` / `long (&&)[2]`: a reference to an array a braced list binds. Yields the
// cv-stripped element spelling and the extent text (empty for an unbounded `(&)[]`).

std::string LLVMBackend::CxxBraceContainerElementSpelling(const std::string& parameter)
{
        if (std::string element = CxxBraceTargetElementSpelling(parameter); !element.empty())
            return element;

        // ArrayRef-like APIs are often a named class with pointer+size and initializer-list
        // constructors rather than a template whose element is visible in the parameter spelling.
        // Recover that element from the class's constructor signatures so a wrapper can construct
        // the right destination class in C++ (for example Ref(Index*) from Slice and int).
        std::string container = StripCxxRefAndCv(parameter);
        while (!container.empty() && container.back() == '*')
        {
            container.pop_back();
            while (!container.empty() && std::isspace((unsigned char)container.back()))
                container.pop_back();
        }
        TypeAndValue containerType;
        if (!MapCTypeToTypeAndValue(container, containerType, true) || containerType.Pointer)
        {
            auto known = cxxForeignTypeSpellings_.find(SqueezeCxxSpelling(container));
            if (known == cxxForeignTypeSpellings_.end()) return {};
            containerType = TypeAndValue{};
            containerType.TypeName = known->second;
        }
        auto record = cxxRecordEntries_.find(containerType.TypeName);
        if (record == cxxRecordEntries_.end()) return {};

        for (const auto& member : record->second.members)
        {
            if (member.kind != cflat_cinterop::RawCxxMember::Constructor) continue;
            for (size_t i = 1; i < member.paramTypes.size(); ++i)
            {
                if (std::string element = CxxBraceTargetElementSpelling(member.paramTypes[i]);
                    !element.empty())
                    return element;

                TypeAndValue argumentType;
                if (!MapCTypeToTypeAndValue(member.paramTypes[i], argumentType, true)
                    || !argumentType.Pointer || argumentType.ElemPointer)
                    continue;
                std::string element;
                if (CxxSpellingForCflatType(argumentType.TypeName, element))
                    return element;
            }
        }
        return {};
}

bool LLVMBackend::IsCxxScalarBraceElement(const std::string& element)
{
        TypeAndValue elementType;
        return !element.empty() && MapCTypeToTypeAndValue(element, elementType, true)
            && !elementType.Pointer && !IsCxxRecord(elementType.TypeName)
            && (elementType.IsInteger() != -1 || elementType.IsFloatingPoint() != -1);
}

std::string LLVMBackend::CxxClassBraceListElement(const std::string& parameter)
{
        if (CxxBraceParameterIsPointer(parameter)) return {};
        const std::string container = StripCxxRefAndCv(parameter);
        TypeAndValue containerType;
        if (!MapCTypeToTypeAndValue(container, containerType, true) || containerType.Pointer)
        {
            auto known = cxxForeignTypeSpellings_.find(SqueezeCxxSpelling(container));
            if (known == cxxForeignTypeSpellings_.end()) return {};
            containerType = TypeAndValue{};
            containerType.TypeName = known->second;
        }
        auto record = cxxRecordEntries_.find(containerType.TypeName);
        if (record == cxxRecordEntries_.end()) return {};
        std::string found;
        for (const auto& member : record->second.members)
        {
            if (member.kind != cflat_cinterop::RawCxxMember::Constructor
                || member.paramTypes.size() < 2
                || CxxBraceContainerKindOf(member.paramTypes[1]) != CxxBraceContainerKind::InitializerList)
                continue;
            bool defaulted = true;
            for (size_t n = 2; n < member.paramTypes.size(); ++n)
                defaulted = defaulted && n < member.defaultArgs.size()
                    && !member.defaultArgs[n].kind.empty();
            if (!defaulted) continue;
            const std::string element = CxxBraceTargetElementSpelling(member.paramTypes[1]);
            // Two list constructors: clang ranks them by the list's element types; not backed.
            if (element.empty() || (!found.empty() && found != element)) return {};
            found = element;
        }
        return IsCxxScalarBraceElement(found) ? found : std::string();
}

// A std::initializer_list pointed at existing storage: no portable spelling exists, so its known
// two-word layout is set; an unknown standard library is refused.

// Every element is a scalar value (a pointer element picks a non-list constructor instead).

// True when a scalar brace element cannot initialize an initializer-list element of
// `elementTy` without narrowing ([dcl.init.list]).
static bool CxxBraceElementNarrows(const LLVMBackend::NamedVariable& source, llvm::Type* elementTy,
                                   bool dstUnsigned)
{
        llvm::Value* value = source.Primary;
        llvm::Type* have = value->getType();
        // A C++ initializer list forbids narrowing ([dcl.init.list]); a constant the
        // target represents exactly is exempt, floating -> integer never is.
        const bool srcUnsigned = have->isIntegerTy(1)
            || source.TypeAndValue.IsUnsignedInteger() != -1;
        auto* constInt = source.CxxConstantValue != nullptr ? source.CxxConstantValue
                                                            : llvm::dyn_cast<llvm::ConstantInt>(value);
        auto* constFp = llvm::dyn_cast<llvm::ConstantFP>(value);
        bool narrows = false;
        // `int` and `unsigned` share one LLVM type: same type is exempt only with equal sign.
        if (have == elementTy && (!have->isIntegerTy() || srcUnsigned == dstUnsigned))
            narrows = false;
        else if (have->isFloatingPointTy() && elementTy->isIntegerTy())
            narrows = true;
        else if (have->isIntegerTy() && elementTy->isFloatingPointTy())
        {
            llvm::APFloat exact(elementTy->getFltSemantics());
            narrows = constInt == nullptr
                || exact.convertFromAPInt(constInt->getValue(), !srcUnsigned,
                       llvm::APFloat::rmNearestTiesToEven) != llvm::APFloat::opOK;
        }
        else if (have->isFloatingPointTy())
        {
            bool lossy = false;
            llvm::APFloat narrowed = constFp != nullptr ? constFp->getValueAPF()
                                                        : llvm::APFloat(0.0);
            narrows = elementTy->getPrimitiveSizeInBits().getFixedValue()
                    < have->getPrimitiveSizeInBits().getFixedValue()
                && (constFp == nullptr
                    || narrowed.convert(elementTy->getFltSemantics(),
                           llvm::APFloat::rmNearestTiesToEven, &lossy) != llvm::APFloat::opOK
                    || lossy);
        }
        else
        {
            const unsigned srcBits = have->getIntegerBitWidth();
            const unsigned dstBits = elementTy->getIntegerBitWidth();
            const bool represented = srcUnsigned == dstUnsigned ? dstBits >= srcBits
                                   : srcUnsigned && dstBits > srcBits;
            if (!represented)
            {
                const llvm::APInt v = constInt != nullptr ? constInt->getValue()
                                                          : llvm::APInt();
                const bool negative = !srcUnsigned && constInt != nullptr && v.isNegative();
                narrows = constInt == nullptr || (negative && dstUnsigned)
                    || (negative ? v.getSignificantBits() > dstBits
                                 : v.getActiveBits() > (dstUnsigned ? dstBits : dstBits - 1));
            }
        }
        return narrows;
}

bool LLVMBackend::CheckCxxBraceNarrowing(const CxxBraceArgument& brace, const std::string& element,
                                         const std::string& owner, const char* role,
                                         std::string& error)
{
        TypeAndValue elementType;
        if (!IsCxxScalarBraceElement(element)
            || !MapCTypeToTypeAndValue(element, elementType, true))
            return true;
        llvm::Type* elementTy = GetType(elementType);
        if (elementTy == nullptr || (!elementTy->isIntegerTy() && !elementTy->isFloatingPointTy()))
            return true;
        const bool enumTarget = !ResolveEnumTypeName(elementType.TypeName).empty();
        for (size_t i = 0; i < brace.elements.size(); ++i)
        {
            const NamedVariable& source = brace.elements[i];
            llvm::Type* have = source.Primary != nullptr ? source.Primary->getType() : nullptr;
            if (source.TypeAndValue.Pointer || have == nullptr
                || (!have->isIntegerTy() && !have->isFloatingPointTy()))
                continue;
            if (CxxEnumPromotesToElement(source, elementType))
                continue;
            // A scoped enum converts to no non-enum scalar (clang): refused as narrowing.
            if ((source.TypeAndValue.IsScopedEnum && !enumTarget)
                || CxxBraceElementNarrows(source, elementTy,
                    elementTy->isIntegerTy(1) || elementType.IsUnsignedInteger() != -1))
            {
                error = std::format("brace-list element {} of '{}' narrows to the C++ "
                                    "{}'s '{}'; a C++ initializer list forbids that",
                                    i + 1, owner, role, elementType.TypeName);
                return false;
            }
        }
        return true;
}

bool LLVMBackend::EmitCxxBraceBacking(const CxxBraceArgument& brace, const TypeAndValue& elementType,
                                      llvm::Type* elementTy, const TypeAndValue& dataParameter,
                                      const std::string& owner, const char* role,
                                      NamedVariable& data, std::string& error)
{
        auto* arrayTy = llvm::ArrayType::get(elementTy, brace.elements.size());
        auto* array = AllocaAtEntry(arrayTy, nullptr, "cxx.brace.backing");
        for (size_t element = 0; element < brace.elements.size(); ++element)
        {
            const NamedVariable& source = brace.elements[element];
            llvm::Value* value = source.Primary;
            llvm::Type* have = value != nullptr ? value->getType() : nullptr;
            if (source.TypeAndValue.Pointer || have == nullptr
                || (!have->isIntegerTy() && !have->isFloatingPointTy()))
            {
                error = "brace arguments must contain scalar values of one type";
                return false;
            }
            const bool enumTarget = !ResolveEnumTypeName(elementType.TypeName).empty();
            // A scoped enum converts to no non-enum scalar (clang): refused as narrowing.
            const bool narrows = !CxxEnumPromotesToElement(source, elementType)
                && ((source.TypeAndValue.IsScopedEnum && !enumTarget)
                    || CxxBraceElementNarrows(source, elementTy,
                        elementTy->isIntegerTy(1) || elementType.IsUnsignedInteger() != -1));
            if (narrows)
            {
                error = std::format("brace-list element {} of '{}' narrows to the C++ "
                                    "{}'s '{}'; a C++ initializer list forbids that",
                                    element + 1, owner, role, elementType.TypeName);
                return false;
            }
            // Converted like the list element: bool as 0/1, floating into unsigned as fptoui.
            if (have != elementTy)
            {
                if (have->isFloatingPointTy() && elementTy->isIntegerTy()
                    && !elementTy->isIntegerTy(1) && elementType.IsUnsignedInteger() != -1)
                    value = builder->CreateFPToUI(value, elementTy);
                else
                    value = CreateCast(value, elementTy, !have->isIntegerTy(1)
                        && source.TypeAndValue.IsUnsignedInteger() == -1);
            }
            builder->CreateStore(value, builder->CreateInBoundsGEP(
                arrayTy, array, { builder->getInt64(0), builder->getInt64(element) },
                "cxx.brace.slot"));
        }
        data = NamedVariable{};
        data.TypeAndValue = dataParameter;
        data.TypeAndValue.VariableName.clear();
        data.Primary = builder->CreateInBoundsGEP(
            arrayTy, array, { builder->getInt64(0), builder->getInt64(0) }, "cxx.brace.data");
        data.BaseType = data.Primary->getType();
        data.Storage = nullptr;
        data.IsRvalue = true;
        return true;
}

std::string LLVMBackend::CxxBraceElementSpelling(const CxxBraceArgument& brace,
                                                 const std::string& targetParameter) const
{
        std::string target = CxxBraceTargetElementSpelling(targetParameter);
        if (brace.hasMixedElements) return {};
        if (!brace.cxxElementType.empty())
        {
            std::string spelling;
            if (!CxxSpellingForCflatType(brace.cxxElementType, spelling)) return {};
            if (!target.empty() && target != spelling) return {};
            return spelling;
        }
        bool targetIsCxxRecord = false;
        if (!target.empty())
            for (const auto& [cflatName, cxxSpelling] : cxxCflatToCxxSpelling_)
                if (cxxSpelling == target && IsCxxRecord(cflatName))
                {
                    targetIsCxxRecord = true;
                    break;
                }
        // Keep scalar brace arguments matchable when C++ converts them into a record element.
        if (!target.empty() && !(targetIsCxxRecord && brace.allIntegerLiterals)) return target;
        auto cflatTypeOf = [&](const NamedVariable& arg) {
            if (IsCxxNullTypedArgument(arg)) return std::string("std.nullptr_t");
            std::string type = !arg.LiteralIdentity.empty()
                ? arg.LiteralIdentity : arg.TypeAndValue.TypeName;
            bool pointer = arg.TypeAndValue.Pointer;
            llvm::Type* valueType = arg.Primary != nullptr ? arg.Primary->getType() : arg.BaseType;
            // An arithmetic result keeps its C++ type (`l + 1` is `long`, not the i64 TypeName).
            if (std::string arith = CxxArithIdentityForArgument(arg, valueType); !arith.empty())
                type = arith;
            if (std::string promoted = CxxUnscopedEnumPromotedIdentity(arg); !promoted.empty())
                type = promoted;
            if (std::string backing = CxxCflatEnumBackingIdentity(arg); !backing.empty())
                type = backing;
            // The declared primitive identity outranks the machine-type guess below, which
            // cannot tell `char` from `i8` or `long` from `i64`.
            if (type.empty()) type = DeclaredPrimitiveIdentityForCxxArgument(arg, valueType);
            if (type.empty())
            {
                if (auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(arg.Primary);
                    constant != nullptr && IsStringLiteralConstant(constant))
                {
                    type = "char";
                    pointer = true;
                }
                else if (valueType != nullptr && valueType->isFloatTy()) type = "float";
                else if (valueType != nullptr && valueType->isDoubleTy()) type = "double";
                else if (valueType != nullptr && valueType->isIntegerTy())
                {
                    const unsigned bits = valueType->getIntegerBitWidth();
                    if (bits == 1) type = "bool";
                    else if (llvm::dyn_cast_or_null<llvm::ConstantInt>(arg.Primary) != nullptr)
                        type = bits > 32 ? "i64" : "int";
                    else type = bits <= 8 ? "i8" : bits <= 16 ? "short"
                        : bits <= 32 ? "int" : "i64";
                }
                else if (auto* st = llvm::dyn_cast_or_null<llvm::StructType>(valueType))
                    type = st->getName().str();
                if (type.empty()) type = arg.InferSourceTypeName;
            }
            if (pointer)
            {
                type += "*";
                if (arg.TypeAndValue.ElemPointer) type += "*";
            }
            return type;
        };
        std::string spelling;
        for (const auto& element : brace.elements)
        {
            std::string cflatType = cflatTypeOf(element);
            if (cflatType.empty()) return {};
            if (!element.TypeAndValue.Pointer
                && (dataStructures.count(element.TypeAndValue.TypeName) != 0
                    || element.TypeAndValue.TypeName == "string"))
                return {};
            std::string one;
            const bool stringLiteral = [&] {
                auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(element.Primary);
                return constant != nullptr && IsStringLiteralConstant(constant);
            }();
            if ((element.TypeAndValue.TypeName == "char"
                    && element.TypeAndValue.Pointer && element.IsRvalue)
                || stringLiteral)
                one = "const char *";
            else if (brace.allIntegerLiterals && !element.TypeAndValue.Pointer)
                one = targetIsCxxRecord ? "int" : "long";
            else if (!CxxSpellingForCflatType(cflatType, one))
                return {};
            if (spelling.empty()) spelling = one;
            else if (spelling != one) return {};
        }
        return spelling;
}

// C++ spelling of a literal zero by its CFlat literal identity (suffix kept: `0L` is a long zero),
// empty for any other identity. Callers first check IsNullPointerConstantArgument.

// One brace element as a C++ expression of its exact source type, for an overload selector: a
// constant stays a constant expression (narrowing reads as at the source), anything else is a
// declval. Empty when the element is not a spellable scalar.
std::string LLVMBackend::CxxBraceElementIdentity(const NamedVariable& element) const
{
        llvm::Type* valueType = element.Primary != nullptr ? element.Primary->getType()
                                                           : element.BaseType;
        std::string type = !element.LiteralIdentity.empty() ? element.LiteralIdentity
                                                            : element.TypeAndValue.TypeName;
        // Same identity CxxBraceElementSpelling keys the wrapper by (`l + 4` is `long`), so two
        // wrappers never share one selector namespace while ranking different element types.
        if (std::string arith = CxxArithIdentityForArgument(element, valueType); !arith.empty())
            type = arith;
        if (std::string promoted = CxxUnscopedEnumPromotedIdentity(element); !promoted.empty())
            type = promoted;
        if (std::string backing = CxxCflatEnumBackingIdentity(element); !backing.empty())
            type = backing;
        if (type.empty()) type = DeclaredPrimitiveIdentityForCxxArgument(element, valueType);
        return type;
}

std::string LLVMBackend::CxxBraceSelectorElement(
        const NamedVariable& element, const std::vector<std::string>& candidateElements)
{
        llvm::Type* valueType = element.Primary != nullptr ? element.Primary->getType()
                                                           : element.BaseType;
        if (element.TypeAndValue.Pointer || valueType == nullptr
            || (!valueType->isIntegerTy() && !valueType->isFloatingPointTy()))
            return {};
        if (IsNullPointerConstantArgument(element, true))
        {
            if (element.IsCxxMacroGlobal && !element.CallerName.empty())
                return element.CallerName;
            if (std::string zero = CxxZeroLiteralSpelling(element.LiteralIdentity); !zero.empty())
                return zero;
        }
        std::string type = CxxBraceElementIdentity(element);
        auto* constantInt = llvm::dyn_cast_or_null<llvm::ConstantInt>(element.Primary);
        auto* constantFp = llvm::dyn_cast_or_null<llvm::ConstantFP>(element.Primary);
        // An untyped integer literal is spelled as itself, so clang gives it its own type.
        if (constantInt != nullptr && element.LiteralIdentity.empty()
            && element.TypeAndValue.TypeName.empty())
            return "(" + std::to_string(constantInt->getSExtValue()) + ")";
        std::string spelling;
        // A machine-named value (`l + 4` is `i64`, its `long` identity gone) takes the one
        // candidate element of the same machine type and signedness; clang decides otherwise.
        const bool machineName = type.size() >= 2 && (type[0] == 'i' || type[0] == 'u')
            && std::all_of(type.begin() + 1, type.end(),
                           [](char c) { return std::isdigit((unsigned char)c) != 0; });
        if (machineName && constantInt == nullptr)
        {
            std::string match;
            int matches = 0;
            for (const std::string& candidate : candidateElements)
            {
                TypeAndValue candidateType;
                if (!MapCTypeToTypeAndValue(candidate, candidateType, true)
                    || candidateType.Pointer || GetType(candidateType) != valueType
                    || (candidateType.IsUnsignedInteger() != -1) != (type[0] == 'u'))
                    continue;
                if (match != candidate) ++matches;
                match = candidate;
            }
            if (matches == 1) return "declval<" + match + ">()";
        }
        if (element.LiteralIdentity.empty() && constantInt != nullptr && valueType->isIntegerTy(32))
            spelling = "int";      // an unsuffixed integer literal, as clang types it
        else if (element.LiteralIdentity.empty() && constantInt != nullptr
                 && valueType->isIntegerTy(64) && (type.empty() || type == "int"))
            spelling = "long long";
        else if (type.empty())
            spelling = valueType->isFloatTy() ? "float" : valueType->isDoubleTy() ? "double"
                     : valueType->isIntegerTy(1) ? "bool" : std::string();
        else if (!CxxSpellingForCflatType(type, spelling))
            return {};
        if (spelling.empty()) return {};
        if (constantInt != nullptr)
        {
            const bool isUnsigned = element.TypeAndValue.IsUnsignedInteger() != -1
                || valueType->isIntegerTy(1);
            const std::string value = isUnsigned
                ? std::to_string(constantInt->getZExtValue()) + "ULL"
                : (constantInt->isMinValue(true) && valueType->getIntegerBitWidth() == 64)
                    ? std::string("(-9223372036854775807LL - 1)")
                    : std::to_string(constantInt->getSExtValue()) + "LL";
            return "static_cast<" + spelling + ">(" + value + ")";
        }
        if (constantFp != nullptr && constantFp->getValueAPF().isFinite())
        {
            bool lossy = false;
            llvm::APFloat value = constantFp->getValueAPF();
            value.convert(llvm::APFloat::IEEEdouble(), llvm::APFloat::rmNearestTiesToEven, &lossy);
            const std::string literal = std::format("{:a}", value.convertToDouble())
                .insert(value.isNegative() ? 1 : 0, "0x");
            if (element.LiteralIdentity.empty() && valueType->isFloatTy()) return literal;
            return "static_cast<" + spelling + ">(" + literal + ")";
        }
        return "declval<" + spelling + ">()";
}

void LLVMBackend::ExpandCxxBraceArguments(
        std::vector<NamedVariable>& arguments,
        const std::vector<CxxBraceArgument>& braceArguments)
{
        std::vector<size_t> order;
        order.reserve(braceArguments.size());
        for (size_t i = 0; i < braceArguments.size(); ++i) order.push_back(i);
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return braceArguments[a].argumentIndex > braceArguments[b].argumentIndex;
        });
        for (size_t n : order)
        {
            const auto& brace = braceArguments[n];
            if (brace.argumentIndex >= arguments.size()) continue;
            auto at = arguments.begin() + brace.argumentIndex;
            at = arguments.erase(at);
            const auto& replacement = brace.wrapperArguments.empty()
                ? brace.elements : brace.wrapperArguments;
            arguments.insert(at, replacement.begin(), replacement.end());
        }
}

void LLVMBackend::DiscardCxxBraceArguments(
        std::vector<NamedVariable>& arguments,
        const std::vector<CxxBraceArgument>& braceArguments)
{
        std::vector<size_t> order;
        order.reserve(braceArguments.size());
        for (size_t i = 0; i < braceArguments.size(); ++i) order.push_back(i);
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return braceArguments[a].argumentIndex > braceArguments[b].argumentIndex;
        });
        for (size_t n : order)
            if (braceArguments[n].argumentIndex < arguments.size())
                arguments.erase(arguments.begin() + braceArguments[n].argumentIndex);
}

/*
 * Cache a clang rejection only when every error is located in this wrapper's source range.
 * Context-dependent or unmapped errors are retried so another TU state can decide them.
 */
