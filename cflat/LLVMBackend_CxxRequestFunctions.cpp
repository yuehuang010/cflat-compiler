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

bool LLVMBackend::RequestGeneratedCxxWrapper(const CxxRequestGroup& group,
                                             const std::string& requestedSource,
                                             const std::string& wrapperName,
                                             const std::string& cacheTag,
                                             CSigEntry& signature,
                                             std::string& error,
                                             bool persistOnSuccess,
                                             bool allowIncremental)
{
        // A wrapper that hands a CFlat closure to C++ names __cflat_closure<>: define it first.
        std::string withClosurePreamble;
        if (requestedSource.find("__cflat_closure<") != std::string::npos)
        {
            withClosurePreamble = CxxClosureWrapperPreamble();
            if (requestedSource.find("__cflat_string_arg") != std::string::npos
                || requestedSource.find("__cflat_move_string_arg") != std::string::npos)
                withClosurePreamble += CxxClosureStringWrapperPreamble();
            withClosurePreamble += requestedSource;
        }
        const std::string& wrapperSource = withClosurePreamble.empty() ? requestedSource
                                                                       : withClosurePreamble;
        const bool emitDefinitions = symbolSink_ == nullptr;
        const std::string negativeSource = BuildCxxRequestPrologue(group, {}, false) + wrapperSource;
        const std::vector<std::string> negativeArgs = BuildCxxRequestClangArgs(group);
        const std::string negativeKey = CxxTypeRequestCacheKey(
            group, wrapperName, negativeSource, negativeArgs, emitDefinitions)
            // NEG2 leaves pre-fix text-only refusals unreadable: they carry no location proof.
            + "|" + cacheTag + "|NEG2";
        {
            CFileSigCacheEntry negative;
            std::string missReason;
            // LSP analysis also answers from a compile's remembered failure (see the positive key).
            if ((TryLoadCxxTypeRequestCache(group, negativeKey, /*emitDefinitions*/ false,
                                            negative, missReason, /*allowDisk*/ true)
                 || (!emitDefinitions
                     && TryLoadCxxTypeRequestCache(
                            group, CxxTypeRequestCacheKey(group, wrapperName, negativeSource,
                                                          negativeArgs, /*emitDefinitions*/ true)
                                       + "|" + cacheTag + "|NEG2",
                            /*emitDefinitions*/ false, negative, missReason, /*allowDisk*/ true)))
                && negative.macros.size() == 1
                && negative.macros.front().name == kNegativeCxxRequestMarker)
            {
                // A remembered failure carries its clang diagnostic after a 0x1e separator.
                const std::string& stored = negative.macros.front().file;
                const size_t split = stored.find('\x1e');
                error = stored.substr(0, split);
                lastCxxWrapperNoViableAtRequest_ = negative.macros.front().value == 1;
                lastCxxWrapperCause_ = split == std::string::npos ? std::string()
                                                                   : stored.substr(split + 1);
                return false;
            }
        }
        lastCxxWrapperCause_.clear();
        pendingCxxWrapperCause_.clear();
        lastCxxWrapperNoViableAtRequest_ = false;
        pendingCxxWrapperNoViableAtRequest_ = false;
        const unsigned parsesBefore = group.primary < cxxImportGroups_.size()
            ? cxxImportGroups_[group.primary].headerParseCount : 0;
        bool failureInsideWrapper = false;
        bool failureIsBodyRefusal = false;
        auto rememberFailure = [&] {
            CFileSigCacheEntry negative;
            CMacroEntry marker;
            marker.name = kNegativeCxxRequestMarker;
            marker.value = lastCxxWrapperNoViableAtRequest_ ? 1 : 0;
            marker.file = lastCxxWrapperCause_.empty() ? error
                                                       : error + '\x1e' + lastCxxWrapperCause_;
            negative.macros.push_back(std::move(marker));
            StoreCxxTypeRequestCache(group, negativeKey, /*emitDefinitions*/ false,
                                     std::move(negative), /*allowDisk*/ true, nullptr);
        };
        if (RequestGeneratedCxxWrapperUncached(group, wrapperSource, wrapperName, cacheTag,
                                               signature, error, persistOnSuccess,
                                               allowIncremental, &failureInsideWrapper))
        {
            // A callee body an earlier demand check already failed raises no error again here.
            const std::string demand = emitDefinitions ? CheckCxxDemand(wrapperName) : std::string();
            if (demand.empty()) return true;
            lastCxxWrapperCause_ = demand.starts_with("clang: ") ? demand.substr(7) : demand;
            error = lastCxxWrapperCause_;
            // The wrapper's harvest is on disk: remember the refusal too, or a warm compile would
            // reach the verdict without the group and lose the "clang: " form.
            if (const std::string refusal = CxxBodyRefusedWrapperError(group, cacheTag);
                !refusal.empty())
            {
                error = refusal;
                failureIsBodyRefusal = true;
            }
            if (persistOnSuccess && failureIsBodyRefusal) rememberFailure();
            return false;
        }
        lastCxxWrapperCause_ = std::move(pendingCxxWrapperCause_);
        pendingCxxWrapperCause_.clear();
        lastCxxWrapperNoViableAtRequest_ = pendingCxxWrapperNoViableAtRequest_;
        // The selected constructor's stored refusal is the error, so the remembered failure
        // (and every target's diagnostic wording) replays as the same "clang: ..." text.
        if (const std::string refusal = CxxBodyRefusedWrapperError(group, cacheTag);
            !refusal.empty())
        {
            error = refusal;
            lastCxxWrapperCause_ = refusal.substr(7);
            failureIsBodyRefusal = true;
        }
        // A budget refusal is about this compile, not the wrapper; never remember it.
        const std::optional<unsigned> parseBudget = TuParseBudget();
        const bool budgetRefused = parseBudget.has_value()
            && group.primary < cxxImportGroups_.size()
            && cxxImportGroups_[group.primary].headerParseCount > parsesBefore
            && cxxImportGroups_[group.primary].headerParseCount
                   - cxxImportGroups_[group.primary].lookupRecoveryParses > *parseBudget;
        if (persistOnSuccess && !error.empty() && !budgetRefused
            && (failureInsideWrapper || failureIsBodyRefusal))
            rememberFailure();
        return false;
}

bool LLVMBackend::RequestGeneratedCxxWrapperUncached(const CxxRequestGroup& group,
                                                     const std::string& wrapperSource,
                                                     const std::string& wrapperName,
                                                     const std::string& cacheTag,
                                                     CSigEntry& signature,
                                                     std::string& error,
                                                     bool persistOnSuccess,
                                                     bool allowIncremental,
                                                     bool* failureInsideWrapper)
{
        llvm::TimeTraceScope scope("RequestGeneratedCxxWrapper", wrapperName);
        error.clear();
        if (failureInsideWrapper != nullptr) *failureInsideWrapper = false;
        const bool emitDefinitions = symbolSink_ == nullptr;
        std::vector<CSigEntry> requestSigs;
        std::vector<CRecordEntry> requestRecords;
        std::string requestBitcode;
        bool cached = false;
        const std::string requestSource = BuildCxxRequestPrologue(group, {}, false) + wrapperSource;
        const std::vector<std::string> requestArgs = BuildCxxRequestClangArgs(group);
        const std::string requestKey =
            CxxTypeRequestCacheKey(group, wrapperName, requestSource, requestArgs, emitDefinitions)
            + "|" + cacheTag + "|FULL";
        CFileSigCacheEntry cachedEntry;
        std::string missReason;
        /*
         * LSP analysis never stores to disk, so its own key only hits memory. It then reads the
         * entry a compile stored for the same wrapper - the signature the compiler binds; the
         * companion bitcode is dropped below.
         */
        if (TryLoadCxxTypeRequestCache(group, requestKey, emitDefinitions, cachedEntry, missReason)
            || (!emitDefinitions
                && TryLoadCxxTypeRequestCache(
                       group,
                       CxxTypeRequestCacheKey(group, wrapperName, requestSource, requestArgs,
                                              /*emitDefinitions*/ true)
                           + "|" + cacheTag + "|FULL",
                       /*emitDefinitions*/ false, cachedEntry, missReason)))
        {
            requestSigs = cachedEntry.sigs;
            requestRecords = cachedEntry.records;
            if (!requestRecords.empty())
                RegisterCRecords(requestRecords, group.headers.front());
            requestBitcode = cachedEntry.cxxBitcode;
            SetCInteropTargetFacts(cachedEntry.longDoubleWidth,
                                   cachedEntry.longDoubleIsIEEEDouble, cachedEntry.targetTriple);
            cached = true;
        }
        if (!cached)
        {
            std::string requestDiagnostics;
            std::string failedSource;
            std::string failedSourcePath;
            std::string failedDiagnostics;
            bool incrementalLocationChecked = false;
            bool incrementalErrorsInsideWrapper = false;
            auto runRequest = [&](bool emit, cflat_cinterop::ExtractResult& out,
                                  std::string& runError) {
                incrementalLocationChecked = false;
                incrementalErrorsInsideWrapper = false;
                cflat_cinterop::ExtractRequest req;
                req.mainFileName = (std::filesystem::absolute("scratch")
                                    / (wrapperName + ".cpp")).string();
                req.source = BuildCxxRequestPrologue(group, {}, false) + wrapperSource;
                req.cxxMode = true;
                req.cxxImportGroupKey = CxxIncrementalGroupKey(group);
                req.emitDefinitions = emit;
                req.assumeInlineDefinitions = !emit;
                req.skipFunctionBodies = false;
                req.requireInScope = false;
                req.cxxFunctionWrapperNames = { wrapperName };
                req.args = requestArgs;
                AttachCxxHeaderParseGuard(req, group);
                if (UseCxxIncrementalRequests() && allowIncremental)
                {
                    CxxIncrementalGroup* incremental = GetCxxIncrementalGroup(group, runError);
                    if (incremental == nullptr) return false;
                    const std::string wrapperIncludes = BuildCxxRequestIncludes(group);
                    req.source = wrapperIncludes + incremental->UnseenPrefixSource(wrapperSource);
                    req.demandPrefixSource = wrapperSource;
                    req.demandPrefixOffset = wrapperIncludes.size();
                    out = cflat_cinterop::ExtractResult();
                    const bool parsed = incremental->ParseRequest(req, req.source, out, runError);
                    requestDiagnostics = incremental->LastRequestDiagnostics();
                    incrementalLocationChecked = true;
                    incrementalErrorsInsideWrapper = incremental->LastRequestErrorsInsideSourceRange(
                        req.source, req.source);
                    failedSource = req.source;
                    failedSourcePath = req.mainFileName;
                    failedDiagnostics = requestDiagnostics.empty() ? runError : requestDiagnostics;
                    if (!parsed) return false;
                    if (req.source.find(kCxxWrapperPidDecl) != std::string::npos)
                        incremental->RememberPrefixSource(kCxxWrapperPidDecl);
                    if (req.source.find(CxxClosureWrapperPreamble()) != std::string::npos)
                        incremental->RememberPrefixSource(CxxClosureWrapperPreamble());
                    if (req.source.find(CxxClosureStringWrapperPreamble()) != std::string::npos)
                        incremental->RememberPrefixSource(CxxClosureStringWrapperPreamble());
                    RememberGeneratedCxxRecords(*incremental, wrapperSource);
                    return true;
                }
                const std::string pch = EnsureCxxRequestPch(group, req.args);
                if (!pch.empty())
                {
                    req.source = wrapperSource;
                    req.args.push_back("-include-pch");
                    req.args.push_back(pch);
                }
                auto wrapperFound = [&]() {
                    for (const auto& sig : out.sigs)
                        if (sig.name == wrapperName || sig.linkageName == wrapperName) return true;
                    return false;
                };
                const bool ran = cflat_cinterop::ExtractCInterop(req, out, runError);
                failedSource = req.source;
                failedSourcePath = req.mainFileName;
                failedDiagnostics = runError;
                if (ran && (pch.empty() || wrapperFound())) return ran;
                if (pch.empty()) return false;
                // See RunCxxTypeRequests: a PCH may be recovered into an AST that harvests
                // nothing, so a missing wrapper is retried against the real includes - and the
                // PCH is dropped only if that retry proves the PCH was the reason.
                cflat_cinterop::ExtractRequest full = req;
                full.source = BuildCxxRequestPrologue(group, {}, false) + wrapperSource;
                full.args.resize(full.args.size() - 2);
                out = cflat_cinterop::ExtractResult();
                const bool retryRan = cflat_cinterop::ExtractCInterop(full, out, runError);
                failedSource = full.source;
                failedSourcePath = full.mainFileName;
                failedDiagnostics = runError;
                if (retryRan && (!ran || wrapperFound()))
                    DropCxxRequestPch(pch);
                return retryRan;
            };
            cflat_cinterop::ExtractResult probe;
            std::string probeError;
            const bool demandDefault = cacheTag == "DEFAULT";
            if (!runRequest(demandDefault && emitDefinitions, probe, probeError))
            {
                error = FirstCxxErrorLine(probeError);
                pendingCxxWrapperCause_ = requestDiagnostics;
                pendingCxxWrapperNoViableAtRequest_ = probe.firstErrorNoViableAtRequest;
                if (failureInsideWrapper != nullptr)
                {
                    *failureInsideWrapper = incrementalLocationChecked
                        ? incrementalErrorsInsideWrapper
                        : CxxErrorsAreInsideWrapper(
                            failedDiagnostics.empty() ? probeError : failedDiagnostics,
                            failedSourcePath, failedSource, wrapperSource);
                    // A generated ctor candidate is a closed request: an instantiated ctor-body
                    // error rejects this exact argument/type pair, and the key includes headers.
                    if (!*failureInsideWrapper && wrapperName.starts_with("__cflat_ctor_")
                        && !requestDiagnostics.empty())
                        *failureInsideWrapper = true;
                }
                return false;
            }
            auto findWrapper = [&](const cflat_cinterop::ExtractResult& raw)
                -> const cflat_cinterop::RawSig* {
                for (const auto& sig : raw.sigs)
                    if (sig.name == wrapperName || sig.linkageName == wrapperName) return &sig;
                return nullptr;
            };
            const auto* probeSig = findWrapper(probe);
            if (probeSig == nullptr || !probeSig->abi.valid || !probeSig->bindRefusal.empty()
                || !probe.firstError.empty())
            {
                error = FirstCxxErrorLine(!probe.firstError.empty() ? probe.firstError
                    : probeSig != nullptr && !probeSig->bindRefusal.empty()
                        ? probeSig->bindRefusal : probeError);
                pendingCxxWrapperCause_ = requestDiagnostics;
                pendingCxxWrapperNoViableAtRequest_ = !probe.firstError.empty()
                    && probe.firstErrorNoViableAtRequest;
                if (failureInsideWrapper != nullptr)
                {
                    *failureInsideWrapper = incrementalLocationChecked
                        ? incrementalErrorsInsideWrapper
                        : CxxErrorsAreInsideWrapper(
                            failedDiagnostics.empty() ? probe.firstError : failedDiagnostics,
                            failedSourcePath, failedSource, wrapperSource);
                    if (!*failureInsideWrapper && wrapperName.starts_with("__cflat_ctor_")
                        && !requestDiagnostics.empty())
                        *failureInsideWrapper = true;
                    // A default initializer whose re-read binds differently: remember the verdict.
                    if (wrapperName.starts_with("__cflat_dfi_") && probeSig != nullptr
                        && !probeSig->bindRefusal.empty())
                        *failureInsideWrapper = true;
                }
                if (probeSig != nullptr && !probeSig->refusalCause.empty())
                    pendingCxxWrapperCause_ += "\n" + probeSig->refusalCause;
                return false;
            }
            if (!probe.records.empty())
            {
                MapRawRecords(probe, requestRecords);
                RegisterCRecords(requestRecords, group.headers.front());
            }
            // An on-demand wrapper runs outside any extraction; its types resolve in its group.
            const bool signatureTypesBound = [&] {
                CxxRequestGroupScope signatureScope(*this, activeCxxRequestGroup_ != nullptr
                                                               ? activeCxxRequestGroup_ : &group);
                return RequestCxxSignatureTypes(*probeSig);
            }();
            if (!signatureTypesBound)
            {
                error = "the generated wrapper's return or parameter type is not supported by C++ interop";
                return false;
            }

            cflat_cinterop::ExtractResult raw = std::move(probe);
            if (emitDefinitions && !demandDefault)
            {
                cflat_cinterop::ExtractResult emitted;
                std::string emittedError;
                if (!runRequest(true, emitted, emittedError))
                {
                    error = FirstCxxErrorLine(emittedError);
                    return false;
                }
                const auto* emittedSig = findWrapper(emitted);
                if (emittedSig == nullptr || (emitted.bitcode.empty() && !emitted.demandRecorded))
                {
                    error = FirstCxxErrorLine(emitted.firstError.empty()
                        ? emittedError : emitted.firstError);
                    return false;
                }
                raw = std::move(emitted);
            }
            const auto* finalSig = findWrapper(raw);
            if (finalSig == nullptr || !finalSig->abi.valid)
            {
                error = FirstCxxErrorLine(raw.firstError);
                return false;
            }
            if (!raw.records.empty())
            {
                requestRecords.clear();
                MapRawRecords(raw, requestRecords);
                RegisterCRecords(requestRecords, group.headers.front());
            }
            CSigEntry mapped;
            if (!MapRawSig(*finalSig, mapped) || !mapped.bindRefusal.empty())
            {
                error = FirstCxxErrorLine(raw.firstError.empty() ? mapped.bindRefusal : raw.firstError);
                return false;
            }
            requestSigs.push_back(std::move(mapped));
            if (requestSigs.size() != 1 || !requestSigs.front().isCxx)
            {
                error = "the generated wrapper was not extracted";
                return false;
            }
            requestBitcode = raw.bitcode;
            SetCInteropTargetFacts(raw);
            CFileSigCacheEntry entry;
            entry.longDoubleWidth = raw.longDoubleWidth;
            entry.longDoubleIsIEEEDouble = raw.longDoubleIsIEEEDouble;
            entry.targetTriple = raw.targetTriple;
            entry.sigs = requestSigs;
            entry.records = requestRecords;
            entry.cxxBitcode = requestBitcode;
            if (raw.demandRecorded)
            {
                entry.cxxDemandGroupKey = CxxIncrementalGroupKey(group);
                entry.cxxDemandReplayChunk = raw.demandReplayChunk;
            }
            StoreCxxTypeRequestCache(group, requestKey, emitDefinitions, std::move(entry),
                                     /*allowDisk*/ raw.firstError.empty() && persistOnSuccess,
                                     /*allowDiskReason*/ !persistOnSuccess
                                         ? "incomplete generated prefix"
                                         : (raw.firstError.empty() ? nullptr
                                                                    : "rejected clang error"));
        }
        if (requestSigs.size() != 1 || !requestSigs.front().isCxx)
        {
            error = "the generated wrapper was not extracted";
            return false;
        }
        if (!emitDefinitions) requestBitcode.clear();
        RequestCxxSignatureTypes(requestSigs);
        if (cached)
            RebindCxxCachedSignatures(requestSigs);
        if (emitDefinitions) AdoptCxxCompanionBitcode(requestBitcode);
        signature = std::move(requestSigs.front());
        return true;
}

std::vector<LLVMBackend::CxxImplicitArgumentCandidate>
LLVMBackend::CollectCxxImplicitArgumentCandidates(
        const std::string& functionName, const std::vector<NamedVariable>& arguments)
{
        auto trim = [](std::string text) {
            while (!text.empty() && std::isspace((unsigned char)text.front())) text.erase(text.begin());
            while (!text.empty() && std::isspace((unsigned char)text.back())) text.pop_back();
            return text;
        };
        auto hasOuterReference = [&](const std::string& raw) {
            std::string text = trim(raw);
            return !text.empty() && (text.back() == '&' || text.back() == '*');
        };
        auto withoutOuterReference = [&](std::string raw) {
            raw = trim(std::move(raw));
            while (!raw.empty() && (raw.back() == '&' || raw.back() == '*'))
            {
                raw.pop_back();
                raw = trim(std::move(raw));
            }
            for (const char* prefix : { "const ", "volatile ", "class ", "struct ", "union " })
                if (raw.starts_with(prefix))
                {
                    raw.erase(0, std::strlen(prefix));
                    raw = trim(std::move(raw));
                }
            return raw;
        };
        auto squeeze = [](const std::string& text) {
            return SqueezeCxxSpelling(text);
        };

        auto argumentType = [&](const NamedVariable& arg) {
            TypeAndValue type = CxxFixedArrayDecayType(arg);
            if (!arg.LiteralIdentity.empty())
                type.TypeName = arg.LiteralIdentity;
            // The declared primitive identity outranks the machine-type guess below, which
            // cannot tell `char` from `i8` or `long` from `i64`.
            if (type.TypeName.empty())
                type.TypeName = DeclaredPrimitiveIdentityForCxxArgument(arg, arg.BaseType);
            if (type.TypeName.empty())
            {
                auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(arg.Primary);
                if (constant != nullptr && IsStringLiteralConstant(constant))
                {
                    type.TypeName = "char";
                    type.Pointer = true;
                }
                else if (arg.BaseType != nullptr && arg.BaseType->isFloatTy())
                    type.TypeName = "float";
                else if (arg.BaseType != nullptr && arg.BaseType->isDoubleTy())
                    type.TypeName = "double";
                else if (arg.BaseType != nullptr && arg.BaseType->isIntegerTy())
                {
                    const unsigned bits = arg.BaseType->getIntegerBitWidth();
                    type.TypeName = bits == 1 ? "bool" : bits <= 8 ? "i8" : bits <= 16 ? "short"
                        : bits <= 32 ? "int" : "i64";
                }
                else if (auto* st = llvm::dyn_cast_or_null<llvm::StructType>(arg.BaseType))
                    type.TypeName = st->getName().str();
                if (type.TypeName.empty()) type.TypeName = arg.InferSourceTypeName;
            }
            type.IsScopedEnum = type.IsScopedEnum || IsScopedEnumTypeName(type.TypeName);
            return type;
        };

        auto targetClassIdentity = [&](const std::string& raw) {
            TypeAndValue mapped;
            bool foreign = false;
            if (TryMapCxxForeignSpelling(raw, mapped, foreign) && foreign
                && IsCxxRecord(mapped.TypeName))
                return mapped.TypeName;
            const std::string identity = CanonicalCxxForeignIdentity(withoutOuterReference(raw));
            return IsCxxRecord(identity) ? identity : std::string();
        };
        auto isStringLike = [&](const std::string& raw) {
            if (hasOuterReference(raw)) return false;
            const std::string base = withoutOuterReference(raw);
            return base == "std::string" || base.ends_with("::string")
                || base.find("basic_string<") != std::string::npos;
        };
        auto sourceClassSpelling = [&](const NamedVariable& arg, TypeAndValue& type) {
            type = argumentType(arg);
            if (type.TypeName.empty() || type.Pointer || !IsCxxRecord(type.TypeName)) return std::string();
            std::string spelling;
            return CxxSpellingForCflatType(type.TypeName, spelling) ? spelling : std::string();
        };
        auto isAllowedConversion = [&](const NamedVariable& arg, const std::string& raw) {
            if (arg.TypeAndValue.TypeName.empty() && !arg.TypeAndValue.Pointer
                && arg.Primary != nullptr && arg.Primary->getType()->isPointerTy()
                && IsTopLevelStdFunctionSpelling(raw))
                return true;
            TypeAndValue source;
            const bool charPointer = [&] {
                source = argumentType(arg);
                auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(arg.Primary);
                return (constant != nullptr && IsStringLiteralConstant(constant))
                    || (source.Pointer && source.TypeName == "char");
            }();
            if (isStringLike(raw) && charPointer) return true;
            if (source.IsScopedEnum)
            {
                TypeAndValue targetType;
                if (!MapCTypeToTypeAndValue(raw, targetType, true)
                    || targetType.Pointer || !IsCxxRecord(targetType.TypeName))
                    return false;
                TryBindRefusedCxxMember(targetType.TypeName, "__ctor", 1);
                std::string why;
                return SelectCxxConstructor(targetType.TypeName, { source }, why,
                                            /*allowNumericConversions*/ true, nullptr,
                                            /*allowExplicit*/ false) != nullptr;
            }
            const std::string target = targetClassIdentity(raw);
            const std::string trimmedRaw = trim(raw);
            if (!target.empty() && source.TypeName != target && IsCxxRecord(source.TypeName)
                && (trimmedRaw.ends_with("&") || trimmedRaw.ends_with("*")))
            {
                uint64_t offset = 0;
                bool inaccessible = false;
                if (trimmedRaw.ends_with("&") && !trimmedRaw.ends_with("&&")
                    && !source.Pointer && FindCxxValueBaseOffset(source.TypeName, target,
                                                                   offset, inaccessible, nullptr))
                    return true;
                if (trimmedRaw.ends_with("*") && source.Pointer
                    && FindCxxBaseOffset(source.TypeName, target, offset, inaccessible))
                    return true;
            }
            if (hasOuterReference(raw)) return false;
            if (target.empty() || source.Pointer || source.TypeName.empty()
                || !IsCxxRecord(source.TypeName) || source.IsInterface)
                return false;
            std::string targetSpelling;
            std::string sourceSpelling = sourceClassSpelling(arg, source);
            if (sourceSpelling.empty()) return false;
            if (CxxSpellingForCflatType(target, targetSpelling))
                return squeeze(targetSpelling) != squeeze(sourceSpelling);
            return target != source.TypeName;
        };
        auto parameterMatches = [&](const NamedVariable& arg, const std::string& raw) {
            TypeAndValue source = argumentType(arg);
            TypeAndValue target;
            if (!MapCTypeToTypeAndValue(raw, target, true)) return false;
            if (source.TypeName == target.TypeName && source.Pointer == target.Pointer
                && source.ElemPointer == target.ElemPointer)
                return true;
            if (!source.Pointer && target.Pointer && hasOuterReference(raw)
                && IsCxxRecord(source.TypeName) && target.TypeName == source.TypeName)
                return true;
            if (IsCxxRecord(source.TypeName) && IsCxxRecord(target.TypeName))
            {
                std::string sourceSpelling;
                std::string targetSpelling;
                if (CxxSpellingForCflatType(source.TypeName, sourceSpelling)
                    && CxxSpellingForCflatType(target.TypeName, targetSpelling))
                    return squeeze(sourceSpelling) == squeeze(targetSpelling);
            }
            return false;
        };

        std::vector<CxxImplicitArgumentCandidate> candidates;
        auto candidateFits = [&](CxxImplicitArgumentCandidate& candidate) {
            if (candidate.parameterTypes.size() != arguments.size()) return false;
            if (candidate.instanceMember && !arguments.empty()
                && candidate.refQualifier != cflat_cinterop::CxxRefQualifierNone)
            {
                const bool receiverRvalue = IsCxxObjectRvalue(arguments.front());
                if ((candidate.refQualifier == cflat_cinterop::CxxRefQualifierLValue
                     && receiverRvalue
                     && (!candidate.constMember || candidate.volatileMember))
                    || (candidate.refQualifier == cflat_cinterop::CxxRefQualifierRValue
                        && !receiverRvalue))
                    return false;
            }
            bool hasConversion = false;
            for (size_t i = 0; i < arguments.size(); ++i)
            {
                // A C++ member's implicit object argument is already the receiver selected by
                // CFlat. User-defined conversions never apply to it.
                if (candidate.instanceMember && i == 0) continue;
                if (isAllowedConversion(arguments[i], candidate.parameterTypes[i]))
                {
                    hasConversion = true;
                    continue;
                }
                if (!parameterMatches(arguments[i], candidate.parameterTypes[i])) return false;
            }
            return hasConversion;
        };

        auto receiverType = [&] {
            if (arguments.empty()) return std::string();
            TypeAndValue type = argumentType(arguments.front());
            if (type.Pointer && !type.ElemPointer) return type.TypeName;
            return type.TypeName;
        };
        std::set<std::string> seenLinkage;
        auto addMembers = [&](const std::string& owner, const std::string& memberName,
                              bool staticOnly) {
            std::function<void(const std::string&, std::set<std::string>&)> visit;
            visit = [&](const std::string& typeName, std::set<std::string>& seen) {
                if (!seen.insert(typeName).second) return;
                auto record = cxxRecordEntries_.find(typeName);
                if (record == cxxRecordEntries_.end()) return;
                std::string ownerSpelling;
                if (!CxxSpellingForCflatType(owner, ownerSpelling)) return;
                for (const auto& member : record->second.members)
                {
                    // A using-declaration lists the base member here too; one candidate per member.
                    if (member.name == memberName && !member.linkageName.empty()
                        && !seenLinkage.insert(member.linkageName).second)
                        continue;
                    if (member.name != memberName
                        || member.access != cflat_cinterop::AccessPublic || member.isDeleted
                        || member.variadic || member.kind == cflat_cinterop::RawCxxMember::Constructor
                        || (staticOnly
                            ? member.kind != cflat_cinterop::RawCxxMember::StaticMethod
                            : member.kind != cflat_cinterop::RawCxxMember::Instance))
                        continue;
                    CxxImplicitArgumentCandidate candidate;
                    candidate.lookupName = staticOnly ? owner + "." + memberName : memberName;
                    candidate.ownerType = owner;
                    candidate.ownerSpelling = ownerSpelling;
                    candidate.targetName = member.name;
                    candidate.parameterTypes = member.paramTypes;
                    candidate.instanceMember = !staticOnly;
                    candidate.staticMember = staticOnly;
                    candidate.constMember = member.isConst;
                    candidate.volatileMember = member.isVolatile;
                    candidate.refQualifier = member.refQualifier;
                    candidate.isNoexcept = member.isNoexcept;
                    candidate.file = member.file;
                    candidate.line = member.line;
                    if (candidateFits(candidate)) candidates.push_back(std::move(candidate));
                }
                for (const auto& base : record->second.bases)
                    if (base.access == cflat_cinterop::AccessPublic)
                    {
                        const std::string baseName = ResolveCxxBaseIdentity(base);
                        if (!baseName.empty()) visit(baseName, seen);
                    }
            };
            std::set<std::string> seen;
            seenLinkage.clear();
            visit(owner, seen);
        };

        const std::string receiver = receiverType();
        if (!receiver.empty() && IsCxxRecord(receiver))
            addMembers(receiver, functionName, false);
        if (const size_t dot = functionName.rfind('.'); dot != std::string::npos)
        {
            const std::string owner = functionName.substr(0, dot);
            const std::string member = functionName.substr(dot + 1);
            if (IsCxxRecord(owner)) addMembers(owner, member, true);
        }

        if (auto freeIt = cxxFunctionSignatures_.find(functionName);
            freeIt != cxxFunctionSignatures_.end())
            for (const auto& stored : freeIt->second)
            {
                if (!stored.isCxx || stored.paramSpellings.size() != arguments.size()) continue;
                CxxImplicitArgumentCandidate candidate;
                candidate.lookupName = functionName;
                candidate.targetName = stored.name;
                candidate.parameterTypes = stored.paramSpellings;
                candidate.isNoexcept = stored.isNoexcept;
                candidate.file = stored.file;
                candidate.line = stored.line;
                if (candidateFits(candidate)) candidates.push_back(std::move(candidate));
            }
        return candidates;
}

bool LLVMBackend::EmitCxxImplicitArgumentConversions(
        const std::string& functionName, const std::vector<NamedVariable>& arguments,
        const std::vector<CxxImplicitArgumentCandidate>& candidates)
{
        struct ActualArgument
        {
            std::string spelling;
            std::string expression;
        };

        auto argumentType = [&](const NamedVariable& arg) {
            TypeAndValue type = CxxFixedArrayDecayType(arg);
            if (!arg.LiteralIdentity.empty())
                type.TypeName = arg.LiteralIdentity;
            // The declared primitive identity outranks the machine-type guess below, which
            // cannot tell `char` from `i8` or `long` from `i64`.
            if (type.TypeName.empty())
                type.TypeName = DeclaredPrimitiveIdentityForCxxArgument(arg, arg.BaseType);
            if (type.TypeName.empty())
            {
                auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(arg.Primary);
                if (constant != nullptr && IsStringLiteralConstant(constant))
                {
                    type.TypeName = "char";
                    type.Pointer = true;
                }
                else if (arg.BaseType != nullptr && arg.BaseType->isFloatTy())
                    type.TypeName = "float";
                else if (arg.BaseType != nullptr && arg.BaseType->isDoubleTy())
                    type.TypeName = "double";
                else if (arg.BaseType != nullptr && arg.BaseType->isIntegerTy())
                {
                    const unsigned bits = arg.BaseType->getIntegerBitWidth();
                    type.TypeName = bits == 1 ? "bool" : bits <= 8 ? "i8" : bits <= 16 ? "short"
                        : bits <= 32 ? "int" : "i64";
                }
                else if (auto* st = llvm::dyn_cast_or_null<llvm::StructType>(arg.BaseType))
                    type.TypeName = st->getName().str();
                if (type.TypeName.empty()) type.TypeName = arg.InferSourceTypeName;
            }
            return type;
        };
        auto actualArgument = [&](const NamedVariable& arg, size_t parameterIndex)
            -> std::optional<ActualArgument> {
            TypeAndValue type = argumentType(arg);
            if (type.TypeName.empty() && !type.Pointer && arg.Primary != nullptr
                && arg.Primary->getType()->isPointerTy())
                return ActualArgument{ "std::nullptr_t", "p" + std::to_string(parameterIndex) };
            auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(arg.Primary);
            const bool stringLiteral = constant != nullptr && IsStringLiteralConstant(constant);
            if (stringLiteral || (type.Pointer && type.TypeName == "char"))
                return ActualArgument{ "const char *", "p" + std::to_string(parameterIndex) };

            if (type.TypeName.empty()) return std::nullopt;
            std::string cflatType = type.TypeName;
            if (type.Pointer)
            {
                cflatType += "*";
                if (type.ElemPointer) cflatType += "*";
            }
            std::string spelling;
            if (!CxxSpellingForCflatType(cflatType, spelling)) return std::nullopt;
            const bool classValue = !type.Pointer && IsCxxRecord(type.TypeName)
                && !type.IsInterface;
            if (!classValue)
                return ActualArgument{ spelling, "p" + std::to_string(parameterIndex) };

            const bool rvalue = IsCxxRvalueReferenceArgument(arg);
            if (rvalue)
                return ActualArgument{ spelling + " &&",
                    "static_cast<" + spelling + " &&>(p" + std::to_string(parameterIndex) + ")" };
            return ActualArgument{ spelling + " &",
                "p" + std::to_string(parameterIndex) };
        };

        auto candidateDescription = [&](const CxxImplicitArgumentCandidate& candidate) {
            std::string result = candidate.lookupName + "(";
            for (size_t i = candidate.instanceMember ? 1u : 0u;
                 i < candidate.parameterTypes.size(); ++i)
                result += (i == (candidate.instanceMember ? 1u : 0u) ? "" : ", ")
                    + candidate.parameterTypes[i];
            return result + ")";
        };

        bool newlyBound = false;
        for (const CxxImplicitArgumentCandidate& candidate : candidates)
        {
            std::vector<ActualArgument> candidateActuals;
            candidateActuals.reserve(arguments.size());
            for (size_t i = 0; i < arguments.size(); ++i)
            {
                if (candidate.instanceMember && i == 0)
                {
                    candidateActuals.push_back({ std::string(candidate.constMember ? "const " : "")
                                                    + (candidate.volatileMember ? "volatile " : "")
                                                    + candidate.ownerSpelling + " *",
                                                "p0" });
                    continue;
                }
                auto actual = actualArgument(arguments[i], i);
                if (!actual.has_value())
                {
                    candidateActuals.clear();
                    break;
                }
                candidateActuals.push_back(std::move(*actual));
            }
            if (candidateActuals.size() != arguments.size()) continue;

            std::string target;
            // An && member is only reachable through an xvalue; `p0->` would name the & twin.
            if (candidate.instanceMember
                && candidate.refQualifier == cflat_cinterop::CxxRefQualifierRValue)
                target = "static_cast<" + std::string(candidate.constMember ? "const " : "")
                    + (candidate.volatileMember ? "volatile " : "") + candidate.ownerSpelling
                    + " &&>(*p0)." + candidate.targetName + "(";
            else if (candidate.instanceMember)
                target = "p0->" + candidate.targetName + "(";
            else if (candidate.staticMember)
                target = candidate.ownerSpelling + "::" + candidate.targetName + "(";
            else
                target = CxxNameFromCflat(candidate.targetName) + "(";
            for (size_t i = candidate.instanceMember ? 1u : 0u; i < candidateActuals.size(); ++i)
            {
                if (i != (candidate.instanceMember ? 1u : 0u)) target += ", ";
                target += candidateActuals[i].expression;
            }
            target += ")";

            std::string hashKey = "implicit-cxx-argument";
            hashKey += candidate.lookupName;
            hashKey += candidate.ownerSpelling;
            hashKey += target;
            for (const auto& param : candidate.parameterTypes) hashKey += param;
            for (const auto& actual : candidateActuals) hashKey += actual.spelling;
            const uint64_t hash = HashWrapperKey(hashKey);
            const std::string wrapperName = std::format("__cflat_udc_{:016x}", hash);
            bool alreadyBound = false;
            if (auto fit = functionTable.find(candidate.lookupName); fit != functionTable.end())
                for (const auto& symbol : fit->second)
                    if (symbol.External && symbol.UniqueName == wrapperName)
                    { alreadyBound = true; break; }
            if (alreadyBound) continue;

            std::string wrapperSource = "extern \"C\" __attribute__((weak)) decltype(auto) " + wrapperName + "(";
            for (size_t i = 0; i < candidateActuals.size(); ++i)
            {
                if (i != 0) wrapperSource += ", ";
                wrapperSource += candidateActuals[i].spelling + " p" + std::to_string(i);
            }
            wrapperSource += ")";
            if (candidate.isNoexcept) wrapperSource += " noexcept";
            wrapperSource += " { return " + target + "; }\n";

            size_t groupIndex = static_cast<size_t>(-1);
            if (candidate.instanceMember || candidate.staticMember)
            {
                auto groupIt = cxxTypeOwnerGroup_.find(candidate.ownerType);
                if (groupIt == cxxTypeOwnerGroup_.end()) continue;
                groupIndex = groupIt->second;
            }
            else
            {
                auto groupIt = cxxFunctionOwnerGroup_.find(candidate.lookupName);
                if (groupIt == cxxFunctionOwnerGroup_.end()) continue;
                groupIndex = groupIt->second;
            }
            CxxRequestGroup group = (candidate.instanceMember || candidate.staticMember)
                ? MakeCxxClassRequestGroup(groupIndex, candidate.ownerType)
                : MakeCxxRequestGroup(groupIndex, {});
            if (group.headers.empty()) continue;
            CxxRequestGroupScope groupScope(*this, &group);
            CSigEntry bound;
            std::string wrapperError;
            if (!RequestGeneratedCxxWrapper(group, wrapperSource, wrapperName,
                                             "IMPLICIT_ARGUMENT_CONVERSION", bound, wrapperError))
            {
                if (wrapperError.find("ambiguous") != std::string::npos)
                {
                    std::string names;
                    for (const auto& item : candidates)
                        names += (names.empty() ? "" : ", ") + candidateDescription(item);
                    LogError(std::format("ambiguous C++ call to '{}': candidates {}",
                                         functionName, names));
                }
                else
                {
                    if (verbose)
                        std::cout << std::format(
                            "[verbose]   C++ implicit conversion wrapper {} not bound: {}\n",
                            wrapperName, wrapperError);
                    if (std::string cause = CxxFirstDiagnosticLine(wrapperError); !cause.empty())
                        cxxImplicitConversionRefusal_ = std::format(
                            "clang rejected the call through '{}': {}",
                            candidateDescription(candidate), cause);
                }
                continue;
            }
            bound.name = candidate.lookupName;
            // The refusal map is keyed by name: park the callee's own entry so the wrapper's
            // reason is the one read back, then put it back.
            std::optional<std::string> calleeRefusal;
            if (auto rit = cxxBindingRefusals_.find(bound.name); rit != cxxBindingRefusals_.end())
            {
                calleeRefusal = std::move(rit->second);
                cxxBindingRefusals_.erase(rit);
            }
            RegisterCSignatures({ bound }, candidate.file.empty() ? group.headers.front() : candidate.file);
            std::string wrapperRefusal;
            if (auto rit = cxxBindingRefusals_.find(bound.name); rit != cxxBindingRefusals_.end())
                wrapperRefusal = rit->second;
            if (calleeRefusal.has_value()) cxxBindingRefusals_[bound.name] = std::move(*calleeRefusal);
            bool registered = false;
            if (auto fit = functionTable.find(candidate.lookupName); fit != functionTable.end())
                for (auto& symbol : fit->second)
                    if (symbol.External && symbol.UniqueName == wrapperName)
                    {
                        symbol.CxxRefQualifier = candidate.refQualifier;
                        // A member wrapper's p0 is the implicit object: rank it by the member's cv.
                        if (candidate.instanceMember && !symbol.Parameters.empty())
                        {
                            symbol.CxxConst = candidate.constMember;
                            symbol.CxxVolatile = candidate.volatileMember;
                            symbol.Parameters.front().VariableName = "this";
                        }
                        registered = true;
                        break;
                    }
            // A refused wrapper is invisible to overload resolution; keep its reason (last attempt
            // wins) for this call's "no overload" diagnostic instead of leaving it to -v.
            if (!registered && !wrapperRefusal.empty())
                cxxImplicitConversionRefusal_ = std::move(wrapperRefusal);
            newlyBound = newlyBound || registered;
        }
        return newlyBound;
}

bool LLVMBackend::TryBindCxxImplicitArgumentConversions(
        const std::string& functionName, const std::vector<NamedVariable>& arguments)
{
        cxxImplicitConversionRefusal_.clear();
        // A const-twin group name (receiver cv redirect) hides the member name the implicit
        // conversion wrappers are looked up and registered under; bind under the bare name.
        const std::string bareName = StripCxxConstTwin(functionName);
        std::vector<CxxImplicitArgumentCandidate> candidates =
            CollectCxxImplicitArgumentCandidates(bareName, arguments);
        if (candidates.empty()) return false;
        return EmitCxxImplicitArgumentConversions(bareName, arguments, candidates);
}

bool LLVMBackend::TryBindCxxImplicitDefaultCtor(const std::string& typeName, std::string& error)
{
        error.clear();
        EnsureCxxRecordProjected(typeName, false);
        auto infoIt = cxxClasses_.find(typeName);
        auto recordIt = cxxRecordEntries_.find(typeName);
        if (infoIt == cxxClasses_.end() || recordIt == cxxRecordEntries_.end()) return false;
        if (CxxDefaultCtorAmbiguityError(typeName, error)) return false;
        if (FindCxxDefaultCtor(typeName) != nullptr) return true;

        const bool hasUserDeclaredCtor = std::any_of(recordIt->second.members.begin(),
                                                     recordIt->second.members.end(),
            [](const auto& member) {
                return member.kind == cflat_cinterop::RawCxxMember::Constructor
                    && !member.isImplicit;
            });
        const bool hasPublicDefaultCtor = std::any_of(recordIt->second.members.begin(),
                                                       recordIt->second.members.end(),
            [](const auto& member) {
                return member.kind == cflat_cinterop::RawCxxMember::Constructor
                    && member.isDefaultCtor && !member.isDeleted
                    && member.access == cflat_cinterop::AccessPublic;
            });
        // A user constructor callable with no arguments only via defaults cflat cannot pass
        // (`Buf(const Alloc& a = Alloc())`) is still the default ctor; clang applies them.
        const bool hasPublicDefaultedArgCtor = std::any_of(recordIt->second.members.begin(),
                                                           recordIt->second.members.end(),
            [](const auto& member) {
                return member.kind == cflat_cinterop::RawCxxMember::Constructor
                    && !member.isImplicit && member.isDefaultCtor && !member.isDeleted
                    && member.paramTypes.size() > 1
                    && member.access == cflat_cinterop::AccessPublic;
            });
        const bool hasNonPublicDefaultCtor = std::any_of(recordIt->second.members.begin(),
                                                         recordIt->second.members.end(),
            [](const auto& member) {
                return member.kind == cflat_cinterop::RawCxxMember::Constructor
                    && member.isDefaultCtor
                    && member.access != cflat_cinterop::AccessPublic;
            });
        if (!recordIt->second.hasDefaultCtor || recordIt->second.hasDeletedDefaultCtor
            || (hasUserDeclaredCtor && !hasPublicDefaultedArgCtor && !hasPublicDefaultCtor)
            || hasNonPublicDefaultCtor)
            return false;

        std::string ownerSpelling;
        if (!CxxSpellingForCflatType(typeName, ownerSpelling))
        {
            error = "the C++ class type is not registered";
            return false;
        }
        auto groupIt = cxxTypeOwnerGroup_.find(typeName);
        if (groupIt == cxxTypeOwnerGroup_.end())
        {
            error = "the C++ class's import group is unavailable";
            return false;
        }
        CxxRequestGroup group = MakeCxxClassRequestGroup(groupIt->second, typeName);
        if (group.headers.empty())
        {
            error = "the C++ class's import group is unavailable";
            return false;
        }

        const uint64_t hash = HashWrapperKey(typeName + "|implicit-default-ctor|" + ownerSpelling);
        const std::string wrapperName = std::format("__cflat_implicit_ctor_{:016x}", hash);
        const std::string wrapperSource = "#include <new>\nextern \"C\" __attribute__((weak)) void " + wrapperName + "("
            + ownerSpelling + " * p) { ::new (p) " + ownerSpelling + "(); }\n";
        // `::new`: a class-scope operator new hides the global placement form (C++ name lookup).
        CxxRequestGroupScope groupScope(*this, &group);
        CSigEntry signature;
        if (!RequestGeneratedCxxWrapper(group, wrapperSource, wrapperName,
                                        "IMPLICIT_DEFAULT_CTOR", signature, error))
        {
            // The wrapper's raw clang parse diagnostic keeps the "clang: " form (ruling 2026-09-27).
            // An already-demanded body reports its full multi-line text, so compare the first line.
            if (const std::string raw = CxxFirstDiagnosticLine(lastCxxWrapperCause_);
                !raw.empty() && !error.starts_with("clang: ") && CxxFirstDiagnosticLine(error) == raw)
                error = "clang: " + error;
            return false;
        }
        if (signature.params.size() != 1)
        {
            error = "the generated C++ default constructor wrapper has an unexpected signature";
            return false;
        }

        CxxClassInfo::Structor ctor;
        ctor.linkageName = wrapperName;
        ctor.params = signature.params;
        ctor.ret = signature.ret;
        ctor.isDefaultCtor = true;
        ctor.isNoexcept = signature.isNoexcept;
        ctor.access = cflat_cinterop::AccessPublic;
        ctor.abi = signature.abi;
        infoIt->second.constructors.push_back(std::move(ctor));
        return true;
}

bool LLVMBackend::HasUnavailableNestedCxxDefault(const std::string& typeName,
                                                  std::string* unavailableType,
                                                  bool checkCxxType)
{
        if (IsCxxRecord(typeName))
        {
            if (!checkCxxType) return false;
            if (!(CxxElementNeedsDefaultConstruction(typeName)
                  || HasNonPublicCxxDefaultCtor(typeName)))
                return false;
            std::string error;
            TryBindCxxImplicitDefaultCtor(typeName, error);
            if (!error.empty() || FindCxxDefaultCtor(typeName) == nullptr)
            {
                if (unavailableType != nullptr) *unavailableType = typeName;
                return true;
            }
            return false;
        }
        if (GetFunction(typeName) == nullptr)
            return false;
        for (const auto& field : GetDataStructure(typeName).StructFields)
        {
            if (field.Pointer || field.BraceInitializer != nullptr
                || field.HasEmptyBareBraceInitializer
                || (field.Initializer != nullptr && field.Initializer->Default() == nullptr))
                continue;
            if (HasUnavailableNestedCxxDefault(field.TypeName, unavailableType, true))
                return true;
        }
        return false;
}

/*
 * Move construction for a class whose record lists no move or copy constructor cflat can call.
 * A constructor TEMPLATE is never listed as one (MSVC STL spells `unique_ptr(unique_ptr&&)` as a
 * template constrained on the deleter), yet C++ picks it for an rvalue. Let clang do that overload
 * resolution in a generated wrapper; a class that really cannot be moved fails to compile there,
 * and the caller keeps its own diagnostic.
 */
const LLVMBackend::CxxClassInfo::Structor* LLVMBackend::TryBindCxxGeneratedMoveCtor(
        const std::string& typeName)
{
        auto infoIt = cxxClasses_.find(typeName);
        if (infoIt == cxxClasses_.end()) return nullptr;
        std::string ownerSpelling;
        if (!CxxSpellingForCflatType(typeName, ownerSpelling)) return nullptr;
        auto groupIt = cxxTypeOwnerGroup_.find(typeName);
        if (groupIt == cxxTypeOwnerGroup_.end()) return nullptr;
        CxxRequestGroup group = MakeCxxClassRequestGroup(groupIt->second, typeName);
        if (group.headers.empty()) return nullptr;

        const uint64_t hash = HashWrapperKey(typeName + "|generated-move-ctor|" + ownerSpelling);
        const std::string wrapperName = std::format("__cflat_move_ctor_{:016x}", hash);
        const std::string wrapperSource = "#include <new>\nextern \"C\" __attribute__((weak)) void " + wrapperName + "("
            + ownerSpelling + " * p0, " + ownerSpelling + " * p1) { new (p0) " + ownerSpelling
            + "(static_cast<" + ownerSpelling + " &&>(*p1)); }\n";
        CxxRequestGroupScope groupScope(*this, &group);
        CSigEntry signature;
        std::string error;
        if (!RequestGeneratedCxxWrapper(group, wrapperSource, wrapperName,
                                        "GENERATED_MOVE_CTOR", signature, error))
        {
            if (verbose)
                std::cout << std::format("[verbose]   C++ move constructor wrapper for {} not bound: {}\n",
                                         typeName, FirstCxxErrorLine(error));
            return nullptr;
        }
        if (signature.params.size() != 2) return nullptr;

        CxxClassInfo::Structor ctor;
        ctor.linkageName = wrapperName;
        ctor.params = signature.params;
        ctor.ret = signature.ret;
        ctor.isMoveCtor = true;
        ctor.isNoexcept = signature.isNoexcept;
        ctor.access = cflat_cinterop::AccessPublic;
        ctor.abi = signature.abi;
        infoIt->second.constructors.push_back(std::move(ctor));
        return &infoIt->second.constructors.back();
}

/*
 * Construction (or assignment) of a C++ class from the xvalue a `T&&` / `const T&&` call returns.
 * cflat never picks the special member itself: clang's overload resolution does, in a generated
 * wrapper over `static_cast<cv T&&>(*src)`, so a const xvalue binds `const T&&` / `const T&`, a
 * deleted-by-default move is skipped, and a selected deleted member fails there. Its first
 * diagnostic is the refusal, prefixed "clang: ". Construction is COPY-initialization (`T x =
 * xvalue;`, the return of a lambda whose prvalue is elided into the slot), so an `explicit`
 * move constructor loses to the copy constructor exactly as in C++.
 */
const LLVMBackend::CxxClassInfo::Structor* LLVMBackend::RequestCxxXvalueTransfer(
        const std::string& typeName, bool constSource, bool assignment, std::string& refusal)
{
        auto infoIt = cxxClasses_.find(typeName);
        std::string ownerSpelling;
        if (infoIt == cxxClasses_.end() || !CxxSpellingForCflatType(typeName, ownerSpelling))
        {
            refusal = "the C++ class type is not registered";
            return nullptr;
        }
        const std::string key = std::string(assignment ? "assign" : "init")
            + (constSource ? "|const" : "");
        if (auto known = infoIt->second.xvalueTransfers.find(key);
            known != infoIt->second.xvalueTransfers.end())
            return &known->second;
        auto groupIt = cxxTypeOwnerGroup_.find(typeName);
        CxxRequestGroup group = groupIt == cxxTypeOwnerGroup_.end()
            ? CxxRequestGroup{} : MakeCxxClassRequestGroup(groupIt->second, typeName);
        if (group.headers.empty())
        {
            refusal = "the C++ class's import group is unavailable";
            return nullptr;
        }
        const std::string source = (constSource ? "const " : "") + ownerSpelling;
        const std::string xvalue = "static_cast<" + source + " &&>(*p1)";
        const uint64_t hash
            = HashWrapperKey(typeName + "|xvalue-copyinit-" + key + "|" + ownerSpelling);
        const std::string wrapperName = std::format("__cflat_xvalue_{}_{:016x}",
                                                    assignment ? "assign" : "ctor", hash);
        const std::string wrapperSource = "#include <new>\nextern \"C\" __attribute__((weak)) void "
            + wrapperName + "(" + ownerSpelling + " * p0, " + source + " * p1) { "
            + (assignment ? "*p0 = " + xvalue + ";"
                          : "new (p0) " + ownerSpelling + "([&]() -> " + ownerSpelling
                                + " { return " + xvalue + "; }());")
            + " }\n";
        CxxRequestGroupScope groupScope(*this, &group);
        CSigEntry signature;
        std::string error;
        if (!RequestGeneratedCxxWrapper(group, wrapperSource, wrapperName, "XVALUE_TRANSFER",
                                        signature, error)
            || signature.params.size() != 2)
        {
            std::string cause = CxxFirstDiagnosticLine(lastCxxWrapperCause_);
            if (cause.empty()) cause = FirstCxxErrorLine(error);
            if (cause.empty()) cause = "the generated C++ wrapper has an unexpected signature";
            refusal = cause.starts_with("clang: ") ? cause : "clang: " + cause;
            return nullptr;
        }
        CxxClassInfo::Structor st;
        st.linkageName = wrapperName;
        st.params = signature.params;
        st.ret = signature.ret;
        st.isNoexcept = signature.isNoexcept;
        st.access = cflat_cinterop::AccessPublic;
        st.abi = signature.abi;
        return &(infoIt->second.xvalueTransfers[key] = std::move(st));
}

bool LLVMBackend::EmitCxxXvalueTransfer(const std::string& typeName, llvm::Value* dest,
                                        llvm::Value* src, bool constSource, bool assignment,
                                        std::string& refusal)
{
        std::string cause;
        const CxxClassInfo::Structor* transfer
            = RequestCxxXvalueTransfer(typeName, constSource, assignment, cause);
        if (transfer == nullptr)
        {
            refusal = CxxXvalueRefusal(typeName, constSource, assignment, cause);
            return false;
        }
        if (!EmitCxxStructorCall(typeName, *transfer, dest, { src })) return false;
        return ResetMovedStdFunctionSource(typeName, src);
}

std::string LLVMBackend::CxxXvalueRefusal(const std::string& typeName, bool constSource,
                                          bool assignment, const std::string& cause) const
{
        const std::string display = DisplayCxxClassName(typeName);
        return std::format("cannot {} C++ class '{}' from the rvalue reference '{}{}&&' this call "
                           "returns: {}", assignment ? "assign to" : "initialize", display,
                           constSource ? "const " : "", display, cause);
}

/*
 * Members cflat's direct pick cannot rank against, so clang must select even when the class is
 * trivially copyable (where cflat otherwise byte-copies, which IS the trivial copy / move): a
 * constructor / assignment template (it can win on the exact `T&&` binding), an `explicit` move
 * constructor (copy-initialization skips it), a deleted move constructor, a const-qualified
 * assignment (a non-const competitor can win).
 */
bool LLVMBackend::CxxTrivialMoveNeedsTransfer(const std::string& typeName, bool constSource,
                                              bool assignment)
{
        const CxxClassInfo* info = GetCxxClassInfo(typeName);
        if (info == nullptr || !cxxTypeOwnerGroup_.contains(typeName) || IsCppStructName(typeName))
            return false;
        if (assignment)
        {
            if (info->hasAssignTemplate) return true;
            for (const auto* list : { &info->moveAssignOverloads, &info->copyAssignOverloads })
                for (const auto& m : *list)
                    if (m.isConstReceiver) return true;
            return false;
        }
        info = GetCxxClassInfoWithConstructors(typeName);
        if (info->hasCtorTemplate || (!constSource && info->hasDeletedMoveCtor)) return true;
        for (const auto& c : info->constructors)
            if (c.isMoveCtor && !constSource && c.isExplicit) return true;
        return false;
}

/*
 * RULING 2026-10-01: CFlat `move x` of a C++ class object is `static_cast<[const] T&&>(x)`, the
 * member clang selects for `std::move(x)`. The direct member cflat already binds is that pick
 * for a non-const source with a bound move constructor / move assignment; anything else (const
 * source, implicit, deleted or absent move) goes through the clang-selected xvalue transfer, so
 * a deleted move is refused instead of silently copying.
 */
bool LLVMBackend::CxxExplicitMoveNeedsTransfer(const std::string& typeName, bool constSource,
                                               bool assignment)
{
        if (CxxTrivialMoveNeedsTransfer(typeName, constSource, assignment)) return true;
        const CxxClassInfo* info = GetCxxClassInfo(typeName);
        // A CFlat `[cpp]` struct keeps its own move rules and diagnostics.
        if (info == nullptr || !cxxTypeOwnerGroup_.contains(typeName) || IsCppStructName(typeName))
            return false;
        if (constSource) return true;
        // Exactly one bound `T&&` member is clang's pick; several (`T&&` beside `const T&&`) or
        // none leave the choice to clang.
        auto soleMutable = [](const auto& members) {
            int count = 0;
            bool constRef = false;
            for (const auto* m : members)
            {
                ++count;
                constRef = m->params.size() == 2 && m->params[1].CxxConstRef();
            }
            return count == 1 && !constRef;
        };
        std::vector<const CxxClassInfo::Structor*> members;
        if (assignment)
        {
            if (!info->hasMoveAssign) return true;
            for (const auto& m : info->moveAssignOverloads) members.push_back(&m);
            if (members.empty()) members.push_back(&info->moveAssign);
            return !soleMutable(members);
        }
        if (FindCxxMoveCtor(typeName) == nullptr) return true;
        for (const auto& c : info->constructors)
            if (c.isMoveCtor) members.push_back(&c);
        return !soleMutable(members);
}

bool LLVMBackend::EmitCxxExplicitMoveTransfer(const std::string& typeName, llvm::Value* dest,
                                              llvm::Value* src, bool constSource, bool assignment,
                                              const std::string& sourceName, std::string& refusal)
{
        std::string cause;
        const CxxClassInfo::Structor* transfer
            = RequestCxxXvalueTransfer(typeName, constSource, assignment, cause);
        if (transfer == nullptr)
        {
            refusal = std::format("cannot {} C++ class '{}' from 'move {}' (std::move of a{} '{}'): {}",
                                  assignment ? "move-assign" : "move-construct",
                                  DisplayCxxClassName(typeName), sourceName,
                                  constSource ? " const" : "", DisplayCxxClassName(typeName), cause);
            return false;
        }
        if (!EmitCxxStructorCall(typeName, *transfer, dest, { src })) return false;
        return ResetMovedStdFunctionSource(typeName, src, sourceName);
}

/*
 * `move x` of a TRIVIALLY COPYABLE C++ class reaching a sink that byte-copies it (an `auto`
 * local, a register return, a by-value argument): when clang's pick for the xvalue is not the
 * trivial move (CxxTrivialMoveNeedsTransfer), construct a temporary through that pick and let
 * the sink byte-copy the temporary, which is the elided C++ result. `nv` then names the
 * temporary. The caller has established that the source is `move x`. False when nothing
 * applies; a refusal is logged by the caller.
 */
bool LLVMBackend::MaterializeTrivialCxxExplicitMove(NamedVariable& nv,
                                                    const std::string& sourceName,
                                                    std::string& refusal)
{
        const std::string& typeName = nv.TypeAndValue.TypeName;
        if (nv.Storage == nullptr || nv.TypeAndValue.Pointer || nv.TypeAndValue.IsArrayView
            || IsForeignNontrivialCxxClass(typeName) || IsCxxRvalueRefClassResult(nv))
            return false;
        const bool constSource = CxxConstReceiverKind(nv) != 0;
        if (!CxxTrivialMoveNeedsTransfer(typeName, constSource, false)) return false;
        llvm::Type* recordType = GetType(TypeAndValue{ .TypeName = typeName });
        if (recordType == nullptr || !recordType->isSized()) return false;
        auto* temp = AllocaAtEntry(recordType, nullptr, "cxx.movetemp",
                                   module->getDataLayout().getABITypeAlign(recordType).value());
        if (!EmitCxxExplicitMoveTransfer(typeName, temp, nv.Storage, constSource, false,
                                         sourceName, refusal))
            return false;
        nv.Storage = temp;
        nv.Primary = builder->CreateLoad(recordType, temp);
        nv.IsCxxMoveSelected = true;
        return true;
}

// A call argument naming a function directly (not a pointer variable or call result).
bool LLVMBackend::IsCxxFunctionDesignatorArgument(const NamedVariable& arg) const
{
        return arg.Primary != nullptr
            && llvm::isa<llvm::Function>(arg.Primary->stripPointerCasts());
}

// A function-pointer reference and its cv forms: the referent `R (*)(A)` as clang canonicalizes it.

// The C++ type inside a `__typeof__( ... )` spelling (CxxThinFunctionPointerSpelling's form).

std::string LLVMBackend::CxxConstructorRvalueRefBindRefusal(
        const std::string& typeName, const std::vector<NamedVariable>& arguments) const
{
        auto record = cxxRecordEntries_.find(typeName);
        if (record == cxxRecordEntries_.end()) return {};
        // A constructor template may deduce `F&&` as `F&` for the lvalue: overload resolution decides.
        if (record->second.hasCtorTemplate || !record->second.ctorTemplates.empty()) return {};
        for (size_t index = 0; index < arguments.size(); ++index)
        {
            const NamedVariable& argument = arguments[index];
            if (IsCxxRvalueReferenceArgument(argument)) continue;
            TypeAndValue functionType = argument.TypeAndValue;
            if (IsCxxFunctionDesignatorArgument(argument))
                if (auto* function = llvm::dyn_cast<llvm::Function>(
                        argument.Primary->stripPointerCasts()))
                    if (const FunctionSymbol* symbol = FindSymbolForFunction(function))
                        functionType = FuncPtrSigOfSymbol(*symbol);
            if (functionType.TypeName.empty() && !functionType.FuncPtrReturnTypeName.empty())
                functionType.TypeName = "__c_fn_ptr";
            std::string spelling;
            if (!CxxThinFunctionPointerSpelling(functionType, spelling)) continue;
            spelling = CxxTypeofInner(spelling);
            if (spelling.empty()) continue;
            bool hasExactNonRvalueParameter = false;
            bool hasExactRvalueReference = false;
            std::string parameterName = "argument";
            for (const auto& member : record->second.members)
            {
                if (member.kind != cflat_cinterop::RawCxxMember::Constructor
                    || index + 1 >= member.paramTypes.size())
                    continue;
                bool arityFits = member.paramTypes.size() >= arguments.size() + 1;
                for (size_t n = arguments.size() + 1; arityFits && n < member.paramTypes.size(); ++n)
                    if ((n >= member.defaultArgs.size() || member.defaultArgs[n].kind.empty())
                        && !(n + 1 == member.paramTypes.size()
                            && member.paramTypes[n].ends_with("...")))
                        arityFits = false;
                if (!arityFits) continue;
                // Any other parameter type (by-value Fn, bool, ...) may still take the lvalue.
                if (member.paramTypes[index + 1].find("&&") == std::string::npos)
                {
                    hasExactNonRvalueParameter = true;
                    continue;
                }
                const std::string referent = CxxFunctionPointerReferentParameter(
                    member.paramTypes[index + 1]);
                if (referent.empty() || SqueezeCxxSpelling(referent)
                    != SqueezeCxxSpelling(spelling))
                    continue;
                if (member.paramTypes[index + 1].find("&&") != std::string::npos)
                {
                    hasExactRvalueReference = true;
                    if (index + 1 < member.paramNames.size()
                        && !member.paramNames[index + 1].empty())
                        parameterName = member.paramNames[index + 1];
                }
                else
                    hasExactNonRvalueParameter = true;
            }
            if (hasExactRvalueReference && !hasExactNonRvalueParameter)
                return std::format(
                    "parameter '{}' of '{}' is an rvalue reference; pass 'move <arg>' "
                    "or a temporary", parameterName,
                    SpellType(*this, TypeAndValue{ .TypeName = typeName }));
        }
        return {};
}

// `X *const &`: a C++ const reference to a pointer (a literal binds its decayed temporary).

/*
 * A string literal is an lvalue `const char[N]` with static storage. The wrapper takes its address
 * as `const char *` and rebuilds that array lvalue, so C++ deduces `T&&` / `const T&` as
 * `const char(&)[N]` (a forwarded reference refers to the literal itself, never a wrapper slot),
 * prefers an array-reference overload, and refuses a `const char *&` as clang does. Empty when
 * the argument is not a literal.
 */
std::string LLVMBackend::CxxStringLiteralArrayLvalue(const NamedVariable& arg,
                                                     size_t parameterIndex) const
{
        auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(arg.Primary);
        if (constant == nullptr) return {};
        auto length = stringLiteralLenByPtr.find(constant);
        if (length == stringLiteralLenByPtr.end()) return {};
        const char* elementType = arg.TypeAndValue.TypeName == "c8" ? "char8_t" : "char";
        return std::format("(*reinterpret_cast<const {} (*)[{}]>(p{}))", elementType,
                           length->second + 1, parameterIndex);
}

/*
 * A string literal crossed as its own array lvalue (literalParams: wrapper parameter indices) and
 * the callee clang selected binds `X *const &` to its DECAYED pointer (RawSig::paramTemporaryTypes
 * from WrapperLiteralPointerTemporaries): that temporary dies with the wrapper frame. Re-spell
 * those formals `X *const &` and pass them straight through, so the CFlat caller owns the slot
 * until its full-expression ends (cxx.refptr.arg), and require the re-spelled wrapper to select
 * the SAME callee. True with nothing to do or after a re-spelling (bound, calls, wrapperName
 * updated); false with `error` when clang refuses the re-spelled call or selects another callee.
 */
bool LLVMBackend::RetargetCxxLiteralPointerTemporaries(
        CSigEntry& bound, const std::vector<size_t>& literalParams,
        std::vector<std::string>& parameterSpellings, std::vector<std::string>& calls,
        const std::function<std::string(const std::vector<std::string>&, std::string&)>& makeInput,
        const std::function<bool(const std::string&, const std::string&, CSigEntry&,
                                 std::string&)>& request,
        const std::string& displayName, std::string& wrapperName, std::string& error)
{
        std::vector<std::pair<size_t, std::string>> plan;
        for (size_t k : literalParams)
            if (k < bound.paramTemporarySpellings.size() && k < parameterSpellings.size()
                && k < calls.size() && bound.paramTemporarySpellings[k].ends_with("*"))
                plan.emplace_back(k, bound.paramTemporarySpellings[k]);
        if (plan.empty()) return true;
        const std::vector<std::string> sourceSpellings = parameterSpellings;
        std::vector<std::string> retargetCalls = calls;
        for (const auto& [k, type] : plan)
        {
            parameterSpellings[k] = type + "const &";
            retargetCalls[k] = "p" + std::to_string(k);
        }
        std::string name;
        const std::string input = makeInput(retargetCalls, name);
        parameterSpellings = sourceSpellings;
        CSigEntry retargeted;
        std::string requestError;
        if (!request(name, input, retargeted, requestError)
            || bound.calleeIdentity.empty() || retargeted.calleeIdentity != bound.calleeIdentity)
        {
            error = std::format("C++ '{}' called with a string literal selects an overload that "
                                "binds a reference to the literal's decayed pointer, and CFlat "
                                "cannot keep that pointer alive in the calling frame for the "
                                "same overload; pass a named 'const char *' variable instead",
                                displayName);
            return false;
        }
        bound = std::move(retargeted);
        calls = std::move(retargetCalls);
        wrapperName = std::move(name);
        return true;
}

// A reference to an array (`const char (&)[N]`), which ends in `]`, not `&`.

// Every bound non-template C++ overload of `name` with `arity` parameters (none counts) takes
// `X *const &` at `index`: the cheap literal path needs no resolution by clang. A non-empty
// `owner` keeps instance members of that class only (keyed by bare name, `this` first).
bool LLVMBackend::CxxNonTemplateOverloadsTakePointerConstRef(const std::string& name,
                                                             const std::string& owner,
                                                             size_t arity, size_t index) const
{
        auto overloads = functionTable.find(name);
        if (overloads == functionTable.end()) return true;
        for (const auto& candidate : overloads->second)
        {
            if (!candidate.IsCxx || candidate.UniqueName.starts_with("__cflat_")
                || candidate.Parameters.size() != arity || index >= arity
                || (!owner.empty() && candidate.Parameters[0].TypeName != owner))
                continue;
            const TypeAndValue& parameter = candidate.Parameters[index];
            if (!(parameter.IsCxxRefToPointer && parameter.CxxConstRef())) return false;
        }
        return true;
}

/*
 * The C++ spelling of a non-interpolated CFlat string literal argument (escaped, NUL bytes kept),
 * for a wrapper retry that hands C++ the literal itself instead of a runtime `const char *`.
 */
bool LLVMBackend::CxxStringLiteralSpelling(const NamedVariable& arg, std::string& spelling) const
{
        auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(arg.Primary);
        if (constant == nullptr || !IsStringLiteralConstant(constant)) return false;
        auto* global = llvm::dyn_cast<llvm::GlobalVariable>(constant->stripPointerCasts());
        if (global == nullptr || !global->hasInitializer()) return false;
        auto* data = llvm::dyn_cast<llvm::ConstantDataArray>(global->getInitializer());
        if (data == nullptr || !data->isString()) return false;
        auto length = stringLiteralLenByPtr.find(global);
        if (length == stringLiteralLenByPtr.end()) return false;

        const std::string text = data->getAsString().take_front(length->second).str();
        spelling = "\"";
        for (unsigned char ch : text)
        {
            switch (ch)
            {
            case '\\': spelling += "\\\\"; break;
            case '"': spelling += "\\\""; break;
            case '?': spelling += "\\?"; break;   // no trigraph under any -std
            case '\n': spelling += "\\n"; break;
            case '\r': spelling += "\\r"; break;
            case '\t': spelling += "\\t"; break;
            default:
                // Three octal digits always, so a following digit never extends the escape.
                if (ch < 32 || ch >= 127) spelling += std::format("\\{:03o}", ch);
                else spelling += static_cast<char>(ch);
                break;
            }
        }
        if (arg.TypeAndValue.TypeName == "c8") spelling = "u8" + spelling;
        spelling += '"';
        return true;
}

bool LLVMBackend::PlanCxxOperatorScalarRetarget(const CSigEntry& bound,
                                                const std::vector<NamedVariable>& arguments,
                                                const std::string& infixOperator,
                                                std::vector<CxxScalarRetarget>& plan)
{
        plan.clear();
        for (size_t i = 0; i < bound.paramTemporarySpellings.size() && i < arguments.size(); ++i)
        {
            const std::string& spelling = bound.paramTemporarySpellings[i];
            const NamedVariable& arg = arguments[i];
            llvm::Type* source = arg.BaseType;
            if (spelling.empty() || arg.TypeAndValue.Pointer || source == nullptr
                || !(source->isIntegerTy() || source->isFloatingPointTy()))
                continue;
            CxxScalarRetarget entry;
            entry.index = i;
            entry.spelling = spelling;
            if (!MapCTypeToTypeAndValue(spelling, entry.target, true) || entry.target.Pointer)
                continue;
            llvm::Type* target = GetType(entry.target);
            if (target == nullptr || !(target->isIntegerTy() || target->isFloatingPointTy()))
                continue;
            // CFlat's call rules: widening only; an integer constant may narrow when it fits.
            bool legal = false;
            int refused = 0;  // 0 int <-> floating, 1 integer narrowing, 2 floating narrowing
            if (source->isIntegerTy() && target->isIntegerTy())
            {
                const unsigned from = source->getIntegerBitWidth();
                const unsigned to = target->getIntegerBitWidth();
                legal = to >= from || to == 1;
                if (auto* constant = llvm::dyn_cast_or_null<llvm::ConstantInt>(arg.Primary);
                    !legal && constant != nullptr)
                    legal = entry.target.IsUnsignedInteger() != -1
                        ? !constant->isNegative() && constant->getValue().getActiveBits() <= to
                        : constant->getValue().getSignificantBits() <= to;
                refused = 1;
            }
            else if (source->isFloatingPointTy() && target->isFloatingPointTy())
            {
                legal = target->getPrimitiveSizeInBits() >= source->getPrimitiveSizeInBits();
                refused = 2;
            }
            if (!legal)
            {
                std::string from = arg.LiteralIdentity;
                if (from.empty()) from = arg.TypeAndValue.TypeName;
                if (from.empty()) from = DeclaredPrimitiveIdentityForCxxArgument(arg, source);
                if (from.empty()) from = arg.InferSourceTypeName;
                if (from.empty()) from = source->isDoubleTy() ? "double"
                    : source->isFloatTy() ? "float"
                    : "i" + std::to_string(source->getPrimitiveSizeInBits());
                const std::string to = entry.target.TypeName;
                if (refused == 1)
                    LogErrorMessage("C++ operator{} cannot take a '{}' operand for its 'const {}&' "
                                    "parameter: integer narrowing is not allowed at a call "
                                    "argument - cast it explicitly with '({})'",
                                    { infixOperator, from, to, to });
                else if (refused == 2)
                    LogErrorMessage("C++ operator{} cannot take a '{}' operand for its 'const {}&' "
                                    "parameter: floating narrowing is not allowed at a call "
                                    "argument - cast it explicitly with '({})'",
                                    { infixOperator, from, to, to });
                else
                    LogErrorMessage("C++ operator{} cannot take a '{}' operand for its 'const {}&' "
                                    "parameter: an int <-> floating conversion is not allowed at a "
                                    "call argument - cast it explicitly with '({})'",
                                    { infixOperator, from, to, to });
            }
            plan.push_back(std::move(entry));
        }
        return !plan.empty();
}

void LLVMBackend::StageCxxOperatorScalarArguments(std::vector<NamedVariable>& arguments,
                                                  const std::vector<CxxScalarRetarget>& plan)
{
        for (const CxxScalarRetarget& entry : plan)
        {
            NamedVariable& arg = arguments[entry.index];
            llvm::Type* targetType = GetType(entry.target);
            llvm::Value* value = arg.Primary != nullptr ? arg.Primary : LoadArgStorage(arg);
            if (targetType == nullptr || value == nullptr) continue;
            // The caller's slot is the C++ full-expression temporary the wrapper's const S& binds.
            auto* slot = AllocaAtEntry(targetType, nullptr, "cxx.operator.scalar");
            CreateAssignment(value, slot, arg.TypeAndValue.IsUnsignedInteger() != -1);
            arg.Primary = builder->CreateLoad(targetType, slot);
            arg.BaseType = targetType;
            arg.Storage = slot;
            arg.TypeAndValue = entry.target;
            arg.LiteralIdentity.clear();
            arg.InferSourceTypeName.clear();
            arg.IsRvalue = true;
            arg.CallerName.clear();
        }
}

bool LLVMBackend::RequestCxxFunctionTemplate(const std::string& functionName,
                                             const std::string& ownerType,
                                             const std::vector<std::string>& explicitArgs,
                                             std::vector<NamedVariable>& arguments,
                                             const std::vector<CxxBraceArgument>& braceArguments,
                                             std::string& registeredName,
                                             std::string& error,
                                             const std::string& infixOperator,
                                             bool uniqueRegistration)
{
        error.clear();
        lastCxxRequestClangRejected_ = false;
        lastCxxRequestNoViableAtRequest_ = false;
        registeredName.clear();
        const bool hasDirectConstructionArgument = std::any_of(
            arguments.begin(), arguments.end(), [](const auto& argument) {
                return argument.IsCxxDirectConstruction;
            });
        const std::string lookupName = ownerType.empty()
            ? functionName : ownerType + "." + functionName;
        std::string templateName = lookupName;
        if (!ownerType.empty())
            if (std::string resolved = ResolveCxxFunctionTemplateName(ownerType, functionName);
                !resolved.empty())
                templateName = std::move(resolved);
        // A '#' prefix marks a NON-TYPE (integer) explicit template argument; the rest of this
        // function needs both the bare spelling for display and the tag for C++ emission.
        auto isValueArg = [](const std::string& a) { return a.starts_with("#"); };
        auto displayArg = [&](const std::string& a) {
            return isValueArg(a) ? a.substr(1) : a;
        };
        auto templatesIt = cxxFunctionTemplates_.find(templateName);
        if (templatesIt == cxxFunctionTemplates_.end())
        {
            if (!explicitArgs.empty())
            {
                std::string args;
                for (size_t i = 0; i < explicitArgs.size(); ++i)
                    args += (i == 0 ? "" : ", ") + displayArg(explicitArgs[i]);
                error = std::format("no C++ function template '{}' accepts explicit template "
                                    "arguments <{}>", lookupName, args);
            }
            return false;
        }
        for (NamedVariable& argument : arguments)
        {
            LowerNonCapturingClosureForCxx(argument);
            if (std::string refusal = CxxClosureArgumentRefusal(argument, lookupName,
                                                                nullptr, true);
                !refusal.empty())
            {
                error = std::move(refusal);
                return false;
            }
        }

        // A fixed array argument reaches C++ decayed as a prvalue pointer to element zero.
        auto cflatTypeOf = [&](const NamedVariable& arg) {
            if (IsCxxNullTypedArgument(arg)) return std::string("std.nullptr_t");
            std::string type = !arg.LiteralIdentity.empty()
                ? arg.LiteralIdentity : arg.TypeAndValue.TypeName;
            const bool arrayBaseType = llvm::isa_and_nonnull<llvm::ArrayType>(arg.BaseType);
            bool pointer = arg.TypeAndValue.Pointer && !arrayBaseType;
            llvm::Type* valueType = arg.Primary != nullptr ? arg.Primary->getType() : arg.BaseType;
            // Unscoped C++ enums lowered to integers keep their identity only at the source.
            if (!arg.InferSourceTypeName.empty()
                && (IsCxxForeignNamespace(arg.InferSourceTypeName)
                    || IsCxxForeignTypeRegistered(arg.InferSourceTypeName)
                    || (cxxTypeOwnerGroup_.count(arg.InferSourceTypeName) != 0
                        && cxxUnnamedEnumAliases_.count(arg.InferSourceTypeName) == 0))
                && (!ResolveEnumTypeName(arg.InferSourceTypeName).empty()
                    || enumBackingTypes.count(arg.InferSourceTypeName) != 0))
                type = arg.InferSourceTypeName;
            if (arg.LiteralIdentity.empty()
                && (arg.InferSourceTypeName == "float" || arg.InferSourceTypeName == "double"
                    || arg.InferSourceTypeName == "longdouble"))
                type = arg.InferSourceTypeName;
            // An arithmetic result keeps its C++ type (`l + 1` is `long`, not the i64 TypeName).
            if (std::string arith = CxxArithIdentityForArgument(arg, valueType); !arith.empty())
                type = arith;
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
                if (arg.TypeAndValue.ElemPointer
                    || (arg.IsCxxAddressOfPointerValue
                        && arg.TypeAndValue.PointerDepth >= 2))
                    type += "*";
            }
            if (IsCxxFixedArrayArgument(arg))
            {
                if (pointer && arg.TypeAndValue.ElemPointer) return std::string();
                type += "*";
            }
            return type;
        };
        auto cxxTemplateArgumentSpelling = [&](const std::string& type,
                                               std::string& spelling) {
            std::string base = type;
            int pointerDepth = 0;
            while (!base.empty() && base.back() == '*')
            {
                base.pop_back();
                ++pointerDepth;
            }
            const std::string enumKey = ResolveEnumTypeName(base);
            if (!enumKey.empty()
                && (IsCxxForeignNamespace(enumKey) || IsCxxForeignTypeRegistered(enumKey)
                    || (cxxTypeOwnerGroup_.count(enumKey) != 0
                        && cxxUnnamedEnumAliases_.count(enumKey) == 0
                        && cxxUnnamedEnumAliases_.count(base) == 0)))
            {
                spelling = enumKey;
                for (size_t pos = 0; (pos = spelling.find('.', pos)) != std::string::npos; pos += 2)
                    spelling.replace(pos, 1, "::");
                for (int i = 0; i < pointerDepth; ++i) spelling += " *";
                return true;
            }
            return CxxSpellingForCflatType(type, spelling);
        };
        auto displayTypeOf = [&](const NamedVariable& arg) {
            if (!arg.CxxClosureDisplay.empty()) return arg.CxxClosureDisplay;
            if (arg.TypeAndValue.IsFunctionPointer && arg.TypeAndValue.IsThinFnPtr())
                return "function<" + FuncPtrSpellingOf(arg.TypeAndValue) + ">";
            std::string type = cflatTypeOf(arg);
            return type.empty() ? std::string("<no C++ spelling>") : type;
        };
        auto braceForArgument = [&](size_t index) -> const CxxBraceArgument* {
            for (const auto& brace : braceArguments)
                if (brace.argumentIndex == index) return &brace;
            return nullptr;
        };
        auto braceElementSpelling = [&](const CxxBraceArgument& brace,
                                        const std::string& targetParameter) {
            std::string target = CxxBraceTargetElementSpelling(targetParameter);
            bool targetIsCxxRecord = false;
            if (!target.empty())
                for (const auto& [cflatName, cxxSpelling] : cxxCflatToCxxSpelling_)
                    if (cxxSpelling == target && IsCxxRecord(cflatName))
                    {
                        targetIsCxxRecord = true;
                        break;
                    }
            if (!target.empty() && !(targetIsCxxRecord && brace.allIntegerLiterals)) return target;
            std::string spelling;
            for (const auto& element : brace.elements)
            {
                std::string cflatType = cflatTypeOf(element);
                if (cflatType.empty()) return std::string();
                if (!element.TypeAndValue.Pointer
                    && (dataStructures.count(element.TypeAndValue.TypeName) != 0
                        || element.TypeAndValue.TypeName == "string"))
                    return std::string();
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
                    return std::string();
                if (spelling.empty()) spelling = one;
                else if (spelling != one) return std::string();
            }
            return spelling;
        };
        std::string argumentDisplay;
        for (size_t i = 0; i < arguments.size(); ++i)
        {
            if (i != 0) argumentDisplay += ", ";
            argumentDisplay += displayTypeOf(arguments[i]);
        }
        auto noMatch = [&](const std::string& extra = std::string()) {
            std::string displayedName = lookupName;
            if (!explicitArgs.empty())
            {
                displayedName += "<";
                for (size_t i = 0; i < explicitArgs.size(); ++i)
                    displayedName += (i == 0 ? "" : ", ") + displayArg(explicitArgs[i]);
                displayedName += ">";
            }
            error = std::format("no instantiation of C++ function template '{}' accepts these "
                                "argument types ({})", displayedName, argumentDisplay);
            if (!extra.empty()) error += " (clang: " + extra + ")";
            if (!ownerType.empty())
            {
                /*
                 * Stored causes are per member, not per call: attach one only when it cannot belong
                 * to a sibling overload - exactly one same-name same-arity member carries a cause,
                 * and the name-keyed refusal is used only for a name with a single member.
                 */
                const size_t dot = functionName.rfind('.');
                const std::string bareName = dot == std::string::npos
                    ? functionName : functionName.substr(dot + 1);
                size_t sameName = 0;
                if (auto record = cxxRecordEntries_.find(ownerType);
                    record != cxxRecordEntries_.end())
                {
                    const cflat_cinterop::RawCxxMember* only = nullptr;
                    size_t withCause = 0;
                    for (const auto& member : record->second.members)
                    {
                        if (member.name != bareName) continue;
                        ++sameName;
                        if (member.paramTypes.size() != arguments.size()
                            || !member.bindRefusal.starts_with("cannot be instantiated")
                            || member.refusalCause.empty())
                            continue;
                        ++withCause;
                        only = &member;
                    }
                    if (withCause == 1)
                    {
                        const std::string causeLine = CxxFirstDiagnosticLine(only->refusalCause);
                        if (!causeLine.empty() && error.find(causeLine) == std::string::npos)
                            error += std::format(" (clang: {})", causeLine);
                    }
                }
                const CxxClassInfo* info = sameName <= 1 ? GetCxxClassInfo(ownerType) : nullptr;
                if (info != nullptr)
                {
                    if (auto refusal = info->refusedMembers.find(bareName);
                        refusal != info->refusedMembers.end())
                    {
                        const size_t note = refusal->second.find(" (clang: ");
                        const size_t end = note == std::string::npos
                            ? std::string::npos : refusal->second.find(')', note + 9);
                        if (end != std::string::npos)
                        {
                            const std::string clause = refusal->second.substr(note, end - note + 1);
                            if (error.find(clause) == std::string::npos) error += clause;
                        }
                    }
                }
            }
            return false;
        };

        // How many explicit template arguments a candidate can take, and whether argument i may
        // be a non-type (integer) one. A template-parameter PACK makes the count unbounded.
        auto explicitCapacity = [](const cflat_cinterop::RawFunctionTemplate& c) -> size_t {
            const std::string& kinds = c.templateParameterKinds;
            if (kinds.empty()) return c.typeParameterCount;
            if (kinds.find('P') != std::string::npos) return (size_t)-1;
            return kinds.size();
        };
        auto kindsAccept = [&](const cflat_cinterop::RawFunctionTemplate& c) {
            const std::string& kinds = c.templateParameterKinds;
            if (kinds.empty()) return std::all_of(explicitArgs.begin(), explicitArgs.end(),
                                                  [&](const std::string& a) { return !isValueArg(a); });
            for (size_t i = 0; i < explicitArgs.size(); ++i)
            {
                if (i >= kinds.size()) return kinds.back() == 'P';
                const bool wantsValue = kinds[i] == 'N' || kinds[i] == 'd';
                if (wantsValue != isValueArg(explicitArgs[i])) return false;
            }
            return true;
        };
        const cflat_cinterop::RawFunctionTemplate* selected = nullptr;
        // Pass 0 prefers a candidate whose template-parameter kinds line up with what was
        // written; pass 1 falls back to arity alone (overloads of one C++ name share a spelling,
        // so the generated wrapper lets C++ overload resolution have the final say).
        for (int pass = 0; pass < 2 && selected == nullptr; ++pass)
        for (const auto& candidate : templatesIt->second)
        {
            const bool instance = candidate.kind == cflat_cinterop::RawFunctionTemplate::InstanceMember;
            const bool kindMatches = ownerType.empty()
                ? candidate.kind == cflat_cinterop::RawFunctionTemplate::Free
                    || candidate.kind == cflat_cinterop::RawFunctionTemplate::StaticMember
                : candidate.kind == cflat_cinterop::RawFunctionTemplate::InstanceMember
                    || candidate.kind == cflat_cinterop::RawFunctionTemplate::StaticMember;
            if (!kindMatches || explicitArgs.size() > explicitCapacity(candidate)) continue;
            if (pass == 0 && !kindsAccept(candidate)) continue;
            if (instance && arguments.empty()) continue;
            const unsigned arity = (unsigned)arguments.size() - (instance ? 1u : 0u);
            if (arity < candidate.minArity || arity > candidate.maxArity) continue;
            selected = &candidate;
            break;
        }
        if (selected == nullptr) return noMatch();

        const size_t functionArgumentOffset = selected->kind
            == cflat_cinterop::RawFunctionTemplate::InstanceMember ? 1u : 0u;
        for (size_t i = functionArgumentOffset; i < arguments.size(); ++i)
        {
            size_t parameterIndex = i - functionArgumentOffset;
            if (selected->hasParameterPack && !selected->parameterTypes.empty()
                && parameterIndex >= selected->parameterTypes.size() - 1)
                parameterIndex = selected->parameterTypes.size() - 1;
            if (parameterIndex >= selected->forwardingReferenceTemplateParameterIndices.size()
                || parameterIndex >= selected->forwardingReferenceParameters.size()
                || !selected->forwardingReferenceParameters[parameterIndex]
                || IsCxxRvalueReferenceArgument(arguments[i]))
                continue;

            const unsigned templateParameterIndex =
                selected->forwardingReferenceTemplateParameterIndices[parameterIndex];
            if (templateParameterIndex >= explicitArgs.size()
                || isValueArg(explicitArgs[templateParameterIndex])
                || explicitArgs[templateParameterIndex].find('&') != std::string::npos)
                continue;

            const std::string argumentType = cflatTypeOf(arguments[i]);
            if (argumentType.empty()) continue;
            std::string explicitTypeSpelling;
            std::string argumentTypeSpelling;
            const std::string explicitType = displayArg(explicitArgs[templateParameterIndex]);
            if (!CxxSpellingForCflatType(explicitType, explicitTypeSpelling))
                explicitTypeSpelling = explicitType;
            if (!CxxSpellingForCflatType(argumentType, argumentTypeSpelling))
                argumentTypeSpelling = argumentType;
            if (explicitTypeSpelling != argumentTypeSpelling) continue;

            const std::string parameterName = parameterIndex < selected->parameterNames.size()
                && !selected->parameterNames[parameterIndex].empty()
                    ? selected->parameterNames[parameterIndex] : "argument";
            error = std::format(
                "parameter '{}' of '{}' is an rvalue reference; pass 'move <arg>' or a temporary",
                parameterName, lookupName);
            return false;
        }

        std::string ownerSpelling;
        if (selected->kind != cflat_cinterop::RawFunctionTemplate::Free)
        {
            if (!CxxSpellingForCflatType(ownerType, ownerSpelling)
                && !CxxSpellingForCflatType(selected->owner, ownerSpelling))
                return noMatch("the C++ receiver type is not registered");
        }
        std::vector<std::string> parameterSpellings;
        std::vector<std::string> callArguments;
        std::vector<std::pair<size_t, size_t>> functionPointerCallerSlots;
        // (wrapper parameter, the direct-construction argument whose slot the wrapper takes).
        std::vector<std::pair<size_t, TypeAndValue>> constructionFunctionPointerSlots;
        // (call argument index, C++ literal spelling) for the literal retry below.
        std::vector<std::pair<size_t, std::string>> literalCallArguments;
        // A `const` CFlat's types cannot show was spelled into the wrapper: see uniqueForConst.
        bool constSpelled = false;
        if (selected->kind == cflat_cinterop::RawFunctionTemplate::InstanceMember)
        {
            const NamedVariable& receiver = arguments.front();
            std::string receiverSpelling = ownerSpelling;
            // `p->` keeps p's flags on the object: pointee const makes it const, while
            // CxxConstRef() on a pointer shape (`T *const &`) is the slot, not the object.
            const bool receiverIsConst = CxxConstReceiverKind(receiver) != 0;
            constSpelled = receiverIsConst;
            const bool receiverIsRvalue = IsCxxObjectRvalue(receiver);
            const std::string receiverReference = receiverIsRvalue ? " &&" : " &";
            if (receiver.TypeAndValue.Pointer)
            {
                receiverSpelling = (receiverIsConst ? "const " : "") + receiverSpelling + " *";
                callArguments.push_back("p0->" + selected->memberName);
            }
            else
            {
                receiverSpelling = (receiverIsConst ? "const " : "") + receiverSpelling
                    + receiverReference;
                callArguments.push_back(receiverIsRvalue
                    ? "static_cast<" + receiverSpelling.substr(0,
                        receiverSpelling.size() - receiverReference.size()) + "&&>(p0)."
                        + selected->memberName
                    : "p0." + selected->memberName);
            }
            parameterSpellings.push_back(std::move(receiverSpelling));
        }
        /*
         * A declared parameter that is a NON-CONST LVALUE REFERENCE ("U &", "int &", "U *&",
         * "A &..."), read off clang's spelling. A reference to const ("const U &", "U *const &")
         * and an rvalue/forwarding reference are not.
         */
        auto nonConstLvalueReference = [](std::string type) {
            if (type.ends_with("...")) type.resize(type.size() - 3);
            while (!type.empty() && type.back() == ' ') type.pop_back();
            if (!type.ends_with("&") || type.ends_with("&&")) return false;
            type.pop_back();
            while (!type.empty() && type.back() == ' ') type.pop_back();
            if (type.ends_with("const") || type.ends_with("volatile")) return false;
            return type.find('*') != std::string::npos || !type.starts_with("const ");
        };
        auto isReference = [](std::string type) {
            if (type.ends_with("...")) type.resize(type.size() - 3);
            while (!type.empty() && type.back() == ' ') type.pop_back();
            return type.ends_with("&") || IsCxxArrayReferenceSpelling(type);
        };
        auto templateParameterAt = [](const cflat_cinterop::RawFunctionTemplate& c,
                                      size_t index) -> const std::string* {
            if (c.hasParameterPack && !c.parameterTypes.empty()
                && index >= c.parameterTypes.size() - 1)
                index = c.parameterTypes.size() - 1;
            return index < c.parameterTypes.size() ? &c.parameterTypes[index] : nullptr;
        };
        // Every same-kind, arity-fitting template of this name, i.e. the set clang resolves over.
        auto fittingTemplates = [&](auto&& visit) {
            const bool wantInstance = selected->kind
                == cflat_cinterop::RawFunctionTemplate::InstanceMember;
            const unsigned arity = (unsigned)arguments.size() - (wantInstance ? 1u : 0u);
            for (const auto& c : templatesIt->second)
                if ((c.kind == cflat_cinterop::RawFunctionTemplate::InstanceMember) == wantInstance
                    && arity >= c.minArity && arity <= c.maxArity)
                    visit(c);
        };
        // A scalar at a reference position is spelled by value category; see the registration.
        bool categorySensitive = false;
        size_t flatParameterIndex = parameterSpellings.size();
        bool closureArgument = false;   // a CFlat closure crosses as a per-call __cflat_closure<>
        FunctionSymbol::CxxClosureResultKind closureResultKind =
            FunctionSymbol::CxxClosureResultKind::None;
        TypeAndValue closureResultType;
        bool literalArrayArgument = false;   // its `const char[N]` is in the call: one per N
        std::vector<size_t> literalArrayParams;   // wrapper parameters holding such a literal
        std::map<size_t, std::pair<size_t, std::string>> scalarBackedParams;
        std::map<size_t, std::pair<size_t, std::string>> classBackedParams;
        std::string wrapperBodyPrefix;
        bool wrapperNeedsInitializerList = false;
        auto appendDirectConstruction = [&](NamedVariable& construction,
                                             auto&& self,
                                             std::string& expression) -> bool {
            if (!construction.IsCxxDirectConstruction || !construction.CxxDeferredTernaryShape.empty()) return false;
            if (const std::string refusal = CxxConstructorRvalueRefBindRefusal(
                    construction.CxxDirectConstructionType,
                    construction.CxxDirectConstructionArguments);
                !refusal.empty())
            {
                error = refusal;
                return false;
            }
            std::string typeSpelling;
            if (!CxxSpellingForCflatType(construction.CxxDirectConstructionType, typeSpelling))
                return false;
            std::vector<std::string> constructorArguments;
            auto hasExactReferenceParameter = [&](size_t index,
                                                   const std::string& spelling,
                                                   bool functionPointer = false,
                                                   bool rvalueReference = false) {
                auto record = cxxRecordEntries_.find(construction.CxxDirectConstructionType);
                if (record == cxxRecordEntries_.end()) return false;
                for (const auto& member : record->second.members)
                {
                    if (member.kind != cflat_cinterop::RawCxxMember::Constructor
                        || member.paramTypes.size()
                            < construction.CxxDirectConstructionArguments.size() + 1)
                        continue;
                    bool fits = true;
                    for (size_t n = construction.CxxDirectConstructionArguments.size() + 1;
                         n < member.paramTypes.size(); ++n)
                        if ((n >= member.defaultArgs.size() || member.defaultArgs[n].kind.empty())
                            && !(n + 1 == member.paramTypes.size()
                                && member.paramTypes[n].ends_with("...")))
                        { fits = false; break; }
                    if (!fits || index + 1 >= member.paramTypes.size()) continue;
                    const std::string& parameter = member.paramTypes[index + 1];
                    const bool isRvalueReference = parameter.ends_with("&&");
                    const bool isLvalueReference = parameter.ends_with("&") && !isRvalueReference;
                    const std::string referent = functionPointer
                        ? CxxFunctionPointerReferentParameter(parameter)
                        : (rvalueReference ? isRvalueReference : isLvalueReference)
                            ? StripCxxRefAndCv(parameter) : std::string();
                    if (!referent.empty() && SqueezeCxxSpelling(referent)
                        == SqueezeCxxSpelling(spelling)) return true;
                }
                return false;
            };
            auto hasSelectedExactReferenceParameter = [&](size_t index,
                                                          const std::string& spelling,
                                                          bool functionPointer = false,
                                                          bool rvalueReference = false) {
                auto fallback = [&] {
                    bool selectedReference = false;
                    if (!functionPointer
                        && CxxConstructorThunkParameterIsReference(
                            construction.CxxDirectConstructionType,
                            construction.CxxDirectConstructionArguments,
                            index, rvalueReference, selectedReference))
                        return selectedReference;
                    return hasExactReferenceParameter(index, spelling, functionPointer,
                                                      rvalueReference);
                };
                const auto* classInfo = GetCxxClassInfoWithConstructors(
                    construction.CxxDirectConstructionType);
                auto record = cxxRecordEntries_.find(construction.CxxDirectConstructionType);
                if (classInfo == nullptr || classInfo->hasCtorTemplate
                    || record == cxxRecordEntries_.end() || !record->second.ctorTemplates.empty())
                    return fallback();
                for (const auto& member : record->second.members)
                    if (member.kind == cflat_cinterop::RawCxxMember::Constructor
                        && (member.variadic || member.requiresConstructorWrapper))
                        return fallback();
                std::vector<TypeAndValue> argumentTypes;
                std::vector<NamedVariable> argumentVars;
                for (const NamedVariable& argument : construction.CxxDirectConstructionArguments)
                {
                    TypeAndValue type = argument.TypeAndValue;
                    std::string typeName = cflatTypeOf(argument);
                    int pointerDepth = 0;
                    while (!typeName.empty() && typeName.back() == '*')
                    {
                        typeName.pop_back();
                        ++pointerDepth;
                    }
                    if (type.TypeName.empty()) type.TypeName = typeName;
                    if (pointerDepth > 0)
                    {
                        type.Pointer = true;
                        type.ElemPointer = pointerDepth > 1;
                        type.PointerDepth = pointerDepth;
                    }
                    NamedVariable selectedArgument = argument;
                    selectedArgument.TypeAndValue = type;
                    argumentTypes.push_back(type);
                    argumentVars.push_back(std::move(selectedArgument));
                }
                std::string why;
                const auto* selectedCtor = SelectCxxConstructorListed(
                    construction.CxxDirectConstructionType, argumentTypes, why,
                    /*allowNumericConversions*/ true, &argumentVars, /*allowExplicit*/ true);
                if (selectedCtor == nullptr && !functionPointer)
                {
                    bool selectedReference = false;
                    if (CxxConstructorThunkParameterIsReference(
                            construction.CxxDirectConstructionType,
                            construction.CxxDirectConstructionArguments,
                            index, rvalueReference, selectedReference, /*forceClangPick*/ true))
                        return selectedReference;
                    return hasExactReferenceParameter(index, spelling, functionPointer,
                                                      rvalueReference);
                }
                if (selectedCtor == nullptr || index + 1 >= selectedCtor->params.size())
                    return fallback();
                const TypeAndValue& selectedParam = selectedCtor->params[index + 1];
                const bool selectedReference = rvalueReference
                    ? selectedParam.IsRvalueRef
                    : selectedParam.IsAlias || selectedParam.CxxConstRef()
                        || selectedParam.IsRvalueRef;
                if (!selectedReference)
                    return false;
                for (const auto& member : record->second.members)
                {
                    if (member.kind != cflat_cinterop::RawCxxMember::Constructor
                        || member.linkageName != selectedCtor->linkageName
                        || index + 1 >= member.paramTypes.size())
                        continue;
                    const std::string& parameter = member.paramTypes[index + 1];
                    const bool isRvalueReference = parameter.ends_with("&&");
                    const bool isLvalueReference = parameter.ends_with("&") && !isRvalueReference;
                    const std::string referent = functionPointer
                        ? CxxFunctionPointerReferentParameter(parameter)
                        : (rvalueReference ? isRvalueReference : isLvalueReference)
                            ? StripCxxRefAndCv(parameter) : std::string();
                    return !referent.empty() && SqueezeCxxSpelling(referent)
                        == SqueezeCxxSpelling(spelling);
                }
                return fallback();
            };
            for (size_t ctorArgIndex = 0;
                 ctorArgIndex < construction.CxxDirectConstructionArguments.size();
                 ++ctorArgIndex)
            {
                NamedVariable& argument =
                    construction.CxxDirectConstructionArguments[ctorArgIndex];
                std::string child;
                if (argument.IsCxxDirectConstruction)
                {
                    if (!self(argument, self, child)) return false;
                }
                else
                {
                    std::string spelling;
                    const std::string type = cflatTypeOf(argument);
                    if (const std::string* unnamedEnum = CxxUnnamedEnumExpression(argument.Storage);
                        unnamedEnum != nullptr)
                    {
                        const std::string parameter = "p" + std::to_string(flatParameterIndex++);
                        parameterSpellings.push_back("decltype(" + *unnamedEnum + ")");
                        constructorArguments.push_back(parameter);
                        continue;
                    }
                    if (IsCxxNullTypedArgument(argument))
                    {
                        parameterSpellings.push_back("std::nullptr_t");
                        ++flatParameterIndex;
                        constructorArguments.push_back("nullptr");
                        continue;
                    }
                    TypeAndValue functionType = argument.TypeAndValue;
                    if (IsCxxFunctionDesignatorArgument(argument))
                        if (auto* fn = llvm::dyn_cast<llvm::Function>(
                                argument.Primary->stripPointerCasts()))
                            if (const FunctionSymbol* symbol = FindSymbolForFunction(fn))
                                functionType = FuncPtrSigOfSymbol(*symbol);
                    if (type == "__c_fn_ptr") functionType.TypeName = type;
                    if (!argument.CxxClosureSignature.empty())
                    {
                        const std::string parameter = "p" + std::to_string(flatParameterIndex++);
                        parameterSpellings.push_back("void *");
                        child = CxxClosureCallArgument(argument, parameter);
                        closureArgument = true;
                    }
                    else if (CxxThinFunctionPointerSpelling(functionType, spelling))
                    {
                        const std::string parameter = "p" + std::to_string(flatParameterIndex++);
                        if (IsCxxFunctionDesignatorArgument(argument))
                        {
                            parameterSpellings.push_back(spelling);
                            child = "(*" + parameter + ")";
                        }
                        else if (IsProvenCxxScalarLvalue(argument, true)
                                 && argument.Storage != nullptr)
                        {
                            // Keep a named function pointer an lvalue so clang ranks every ctor
                            // against its source category, including by-value and bool overloads.
                            parameterSpellings.push_back("void *");
                            constructionFunctionPointerSlots.emplace_back(
                                parameterSpellings.size() - 1, functionType);
                            argument.Primary = argument.Storage;
                            const std::string cv = CxxConstReceiverKind(argument) != 0
                                ? " const" : "";
                            child = "*static_cast<" + spelling + cv + " *>(" + parameter + ")";
                        }
                        else if (IsCxxRvalueReferenceArgument(argument)
                                 && argument.Storage != nullptr)
                        {
                            // Keep xvalue category and top-level const for clang's overload set.
                            parameterSpellings.push_back("void *");
                            constructionFunctionPointerSlots.emplace_back(
                                parameterSpellings.size() - 1, functionType);
                            argument.Primary = argument.Storage;
                            const std::string cv = CxxConstReceiverKind(argument) != 0
                                ? " const" : "";
                            child = "static_cast<" + spelling + cv + " &&>(*static_cast<"
                                + spelling + cv + " *>(" + parameter + "))";
                        }
                        else
                        {
                            // By value: the snapshot the wrapper took is the argument.
                            parameterSpellings.push_back(spelling);
                            child = "static_cast<" + spelling + " &&>(" + parameter + ")";
                        }
                    }
                    else
                    {
                        if (type.empty() || !cxxTemplateArgumentSpelling(type, spelling)) return false;
                        const bool classValue = !argument.TypeAndValue.Pointer
                            && IsCxxRecord(argument.TypeAndValue.TypeName);
                        const bool lvalue = classValue ? !IsCxxRvalueReferenceArgument(argument)
                            : IsProvenCxxScalarLvalue(argument);
                        const bool constObject = CxxConstReceiverKind(argument) != 0;
                        std::string parameterSpelling = constObject ? "const " + spelling : spelling;
                        if (classValue) parameterSpelling += lvalue ? " &" : " &&";
                        else if (lvalue && hasSelectedExactReferenceParameter(ctorArgIndex, spelling))
                            parameterSpelling += " &";
                        parameterSpellings.push_back(std::move(parameterSpelling));
                        const std::string parameter = "p" + std::to_string(flatParameterIndex++);
                        child = lvalue ? parameter
                            : "static_cast<" + spelling + " &&>(" + parameter + ")";
                    }
                }
                constructorArguments.push_back(std::move(child));
            }
            if (!error.empty()) return false;
            const char* open = construction.CxxDirectConstructionUsesBraces ? "({" : "(";
            const char* close = construction.CxxDirectConstructionUsesBraces ? "})" : ")";
            expression = typeSpelling + open;
            for (size_t i = 0; i < constructorArguments.size(); ++i)
                expression += (i == 0 ? "" : ", ") + constructorArguments[i];
            expression += close;
            return true;
        };
        for (size_t i = selected->kind == cflat_cinterop::RawFunctionTemplate::InstanceMember ? 1u : 0u;
             i < arguments.size(); ++i)
        {
            NamedVariable& arg = arguments[i];
            const size_t declaredIndex = i - (selected->kind
                == cflat_cinterop::RawFunctionTemplate::InstanceMember ? 1u : 0u);
            bool anyLvalueReference = false, allLvalueReference = true, anyReference = false;
            bool anyPointerConstRef = false, allPointerConstRef = true;
            fittingTemplates([&](const cflat_cinterop::RawFunctionTemplate& c) {
                const std::string* type = templateParameterAt(c, declaredIndex);
                const bool lvalueReference = type != nullptr && nonConstLvalueReference(*type);
                anyLvalueReference = anyLvalueReference || lvalueReference;
                allLvalueReference = allLvalueReference && lvalueReference;
                anyReference = anyReference || (type != nullptr && isReference(*type));
                const bool pointerConstRef = type != nullptr && IsCxxConstRefToPointerSpelling(*type);
                anyPointerConstRef = anyPointerConstRef || pointerConstRef;
                allPointerConstRef = allPointerConstRef && pointerConstRef;
            });
            const bool stringLiteralArgument = [&] {
                auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(arg.Primary);
                return constant != nullptr && IsStringLiteralConstant(constant);
            }();
            // A class keeps its storage-based category; a scalar needs positive lvalue proof.
            const bool decayedArray = IsCxxFixedArrayArgument(arg);
            const bool classArgument = !arg.TypeAndValue.Pointer && !decayedArray
                && IsCxxRecord(arg.TypeAndValue.TypeName);
            const bool provenArrayLvalue = decayedArray && !arg.IsExplicitMove
                && !arg.IsRvalue && !arg.TypeAndValue.IsRvalueRef
                && ((arg.CxxLvalueKind == 1 && arg.Storage != nullptr)
                    || (arg.CxxLvalueKind == 2 && arg.TypeAndValue.IsAlias));
            size_t forwardingIndex = declaredIndex;
            if (selected->hasParameterPack && !selected->parameterTypes.empty()
                && forwardingIndex >= selected->parameterTypes.size() - 1)
                forwardingIndex = selected->parameterTypes.size() - 1;
            const bool forwardingPosition = forwardingIndex
                < selected->forwardingReferenceParameters.size()
                && selected->forwardingReferenceParameters[forwardingIndex] != 0;
            // A named bitfield is an addressless lvalue: a `U&&` / `U&` deduces `T&` and the binding
            // is refused if selected; a const reference or a value parameter copies it as a prvalue.
            const bool bitfieldLvalue = !classArgument && arg.BitfieldStorage != nullptr
                && arg.CxxLvalueKind == 1 && !arg.IsExplicitMove
                && (forwardingPosition || anyLvalueReference);
            // A wrapped CFlat closure variable is its own caller-frame slot: an lvalue.
            const bool provenLvalue = !arg.CxxClosureSignature.empty() ? arg.CxxClosureBorrowed
                : decayedArray ? provenArrayLvalue
                : classArgument ? !IsCxxRvalueReferenceArgument(arg)
                : IsProvenCxxScalarLvalue(arg) || bitfieldLvalue;
            // Any reference (`const T&` included) binds the CALLER's object: a returned
            // reference then aliases it, never a wrapper copy.
            const bool bindsCallerLvalue = (anyReference || uniqueRegistration)
                && !braceForArgument(i)
                && !stringLiteralArgument && provenLvalue;
            // Read before the rvalue marking below drops the storage that identifies it.
            const std::string unnamedEnumType = classArgument ? std::string()
                                                              : CxxUnnamedEnumValueSpelling(arg);
            /*
             * A scalar without lvalue proof IS an rvalue at a reference position: say so on the
             * argument, so overload matching and the call never take a source variable's address.
             */
            if (!classArgument && (!arg.TypeAndValue.Pointer || anyReference) && !provenLvalue
                && !braceForArgument(i) && !stringLiteralArgument
                && (anyReference || forwardingPosition || uniqueRegistration))
            {
                arguments[i].IsRvalue = true;
                arguments[i].Storage = nullptr;
                arguments[i].CallerName.clear();
            }
            /*
             * A scalar lvalue whose C++ identity lowers to its own machine type (`auto a =
             * sizeof(int)`, an i64 slot that C++ sees as `unsigned long`) IS that object, so a
             * reference deduced as the identity binds the caller's slot under the identity name.
             */
            if (provenLvalue && !classArgument && !decayedArray && !arg.TypeAndValue.Pointer
                && (anyReference || forwardingPosition) && !braceForArgument(i))
                if (std::string arith = CxxArithIdentityForArgument(arg, arg.BaseType);
                    !arith.empty() && arith != arg.TypeAndValue.TypeName
                    && ResolveEnumTypeName(arg.TypeAndValue.TypeName).empty())
                {
                    if (!arguments[i].TypeAndValue.TypeName.empty())
                        arguments[i].TypeAndValue.TypeName = arith;
                    arguments[i].InferSourceTypeName = arith;
                }
            // A proven lvalue with no recorded slot (`ref()` of a `T&` result): the loaded address.
            if (bindsCallerLvalue && !classArgument && arguments[i].Storage == nullptr)
                if (auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(arg.Primary))
                    arguments[i].Storage = load->getPointerOperand();
            categorySensitive = categorySensitive
                || (!classArgument
                    && (anyReference || forwardingPosition || uniqueRegistration));
            /*
             * C++ refuses a temporary for a non-const lvalue reference, exactly as the non-template
             * form does. Only when EVERY candidate template demands one and no bound non-template
             * overload of the name could take the value instead.
             */
            if (anyLvalueReference && allLvalueReference && !braceForArgument(i)
                && !stringLiteralArgument && !provenLvalue)
            {
                bool nonTemplateSibling = false;
                if (auto bound = functionTable.find(functionName); bound != functionTable.end())
                    for (const auto& symbol : bound->second)
                        nonTemplateSibling = nonTemplateSibling
                            || (symbol.IsCxx && !symbol.UniqueName.starts_with("__cflat_tpl_"));
                if (!nonTemplateSibling)
                {
                    const auto& names = selected->parameterNames;
                    const std::string parameterName = names.empty() ? std::string("argument")
                        : declaredIndex < names.size() && !names[declaredIndex].empty()
                            ? names[declaredIndex]
                            : declaredIndex >= names.size() && !names.back().empty()
                                ? names.back() : std::string("argument");
                    // An assignment argument never gets here: DiagnoseAssignmentArguments refuses it
                    // at the call site for every callee (ruling 2026-09-27).
                    error = std::format(
                        "parameter '{}' of '{}' is a non-const lvalue reference and cannot bind an "
                        "rvalue; pass an lvalue", parameterName, lookupName);
                    return false;
                }
            }
            if (const auto* brace = braceForArgument(i))
            {
                const std::string* rawParameter = templateParameterAt(*selected, declaredIndex);
                if (rawParameter != nullptr
                    && rawParameter->find("initializer_list") != std::string::npos
                    && !brace->elements.empty())
                {
                    const bool classElements = brace->hasCxxClassElements
                        && !brace->hasMixedElements && !brace->cxxElementType.empty();
                    std::string element;
                    if (classElements)
                    {
                        if (!CxxSpellingForCflatType(brace->cxxElementType, element))
                            return noMatch("a C++ brace-list class element has no C++ spelling");
                    }
                    else element = braceElementSpelling(*brace, {});
                    if (element.empty())
                        return noMatch("brace arguments must contain scalar values of one type");
                    if (!classElements
                        && !CheckCxxBraceNarrowing(*brace, element, lookupName, "parameter", error))
                        return false;
                    const size_t dataIndex = flatParameterIndex++;
                    const std::string il = "__cflat_tpl_il_" + std::to_string(i);
                    const std::string listType = "std::initializer_list<" + element + ">";
                    parameterSpellings.push_back("const " + element + " *");
                    wrapperBodyPrefix += CxxInitializerListOverStorage(
                        listType, il, "p" + std::to_string(dataIndex), brace->elements.size());
                    callArguments.push_back(listType + "(" + il + ")");
                    if (classElements)
                        classBackedParams[brace->argumentIndex] = {
                            dataIndex, brace->cxxElementType };
                    else scalarBackedParams[brace->argumentIndex] = { dataIndex, element };
                    wrapperNeedsInitializerList = true;
                    continue;
                }
                std::string braceType = braceElementSpelling(*brace, {});
                if (brace->elements.empty())
                {
                    callArguments.push_back("{}");
                    continue;
                }
                if (braceType.empty()) return noMatch("brace arguments must contain scalar values of one type");
                std::string braceCall = "{";
                for (size_t element = 0; element < brace->elements.size(); ++element)
                {
                    if (element != 0) braceCall += ", ";
                    const std::string parameter = "p" + std::to_string(flatParameterIndex++);
                    parameterSpellings.push_back(braceType);
                    braceCall += parameter;
                }
                braceCall += "}";
                callArguments.push_back(std::move(braceCall));
                continue;
            }
            std::string literalSpelling;
            const bool inlineStringLiteral = CxxStringLiteralSpelling(arg, literalSpelling);
            std::string spelling;
            const bool stringLiteral = [&] {
                auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(arg.Primary);
                return constant != nullptr && IsStringLiteralConstant(constant);
            }();
            if (!arg.CxxClosureSignature.empty())
            {
                // A CFlat closure: F deduces to the owning __cflat_closure<> prvalue.
                parameterSpellings.push_back("void *");
                arg.CxxClosureParam = "p" + std::to_string(flatParameterIndex++);
                callArguments.push_back(CxxClosureCallArgument(arg, arg.CxxClosureParam));
                closureArgument = true;
                continue;
            }
            std::string cflatType = cflatTypeOf(arg);
            if (cflatType.empty()) return noMatch("an argument type cannot be spelled in C++");
            // The null type crosses by value (every value is the same null) and reaches C++ in
            // its own category, so `T` and `T&&` both deduce std::nullptr_t as clang does.
            if (cflatType == "std.nullptr_t")
            {
                parameterSpellings.push_back("decltype(nullptr)");
                const std::string parameter = "p" + std::to_string(flatParameterIndex++);
                callArguments.push_back(provenLvalue
                    ? parameter : "static_cast<decltype(nullptr) &&>(" + parameter + ")");
                continue;
            }
            /*
             * A literal at `const char *const &` binds its decayed pointer temporary, which C++
             * keeps until the CALLER's full-expression ends: the caller materializes that slot.
             * Only when EVERY candidate (non-templates too) takes that; a mixed set is resolved
             * by clang over the literal's array and re-spelled after (literalArrayParams).
             */
            if (stringLiteral && anyPointerConstRef && allPointerConstRef
                && CxxNonTemplateOverloadsTakePointerConstRef(lookupName, {},
                                                              arguments.size() - (i - declaredIndex),
                                                              declaredIndex)
                && (declaredIndex == i
                    || CxxNonTemplateOverloadsTakePointerConstRef(functionName, ownerType,
                                                                  arguments.size(), i)))
            {
                parameterSpellings.push_back("const char *const &");
                callArguments.push_back("p" + std::to_string(flatParameterIndex++));
                literalArrayArgument = true;
                continue;
            }
            if (std::string literalArray = stringLiteral
                    ? CxxStringLiteralArrayLvalue(arg, flatParameterIndex) : std::string();
                !literalArray.empty())
            {
                literalArrayParams.push_back(parameterSpellings.size());
                parameterSpellings.push_back("const char *");
                callArguments.push_back(std::move(literalArray));
                ++flatParameterIndex;
                literalArrayArgument = true;
                if (inlineStringLiteral)
                    literalCallArguments.emplace_back(callArguments.size() - 1,
                                                      std::move(literalSpelling));
                continue;
            }
            if ((arg.TypeAndValue.TypeName == "char" && arg.TypeAndValue.Pointer && arg.IsRvalue)
                || stringLiteral)
                spelling = "const char *";
            else if (CxxThinFunctionPointerSpelling(cflatType == "__c_fn_ptr"
                         ? [&] { TypeAndValue t = arg.TypeAndValue; t.TypeName = cflatType; return t; }()
                         : arg.TypeAndValue, spelling))
            {
                const size_t parameterIndex = flatParameterIndex++;
                const std::string parameter = "p" + std::to_string(parameterIndex);
                if (IsCxxFunctionDesignatorArgument(arg))
                {
                    parameterSpellings.push_back(spelling);
                    callArguments.push_back("(*" + parameter + ")");
                }
                else
                {
                    parameterSpellings.push_back("void *");
                    llvm::Type* pointerType = arg.BaseType != nullptr
                        ? arg.BaseType : GetType(arg.TypeAndValue);
                    if (pointerType == nullptr || !pointerType->isPointerTy())
                        return noMatch("a function-pointer slot has no pointer type");
                    llvm::Value* slot = arg.Storage;
                    if (slot == nullptr)
                    {
                        slot = AllocaAtEntry(pointerType, nullptr, "cxx.fptr.forward");
                        llvm::Value* value = arg.Primary != nullptr ? arg.Primary : LoadArgStorage(arg);
                        builder->CreateStore(value, slot);
                        arg.Storage = slot;
                    }
                    arg.Primary = slot;
                    functionPointerCallerSlots.emplace_back(parameterIndex, i);
                    const std::string referent = "*static_cast<" + spelling + " *>(" + parameter + ")";
                    callArguments.push_back(arg.IsRvalue
                        ? "static_cast<" + spelling + " &&>(" + referent + ")" : referent);
                }
                // A `const T*` callback parameter is invisible to CFlat's signature too.
                constSpelled = constSpelled || spelling.find("const ") != std::string::npos;
                continue;
            }
            else if (!cxxTemplateArgumentSpelling(cflatType, spelling))
                return noMatch("an argument type cannot be spelled in C++");
            // A volatile class lvalue keeps its volatile in the wrapper, so clang ranks the template
            // against the same cv as the source site (a plain reference overload must not win).
            if (classArgument && (CxxReceiverCv(arg) & 2) != 0) PrependCv(spelling, "volatile ");
            if (arg.TypeAndValue.CxxConstRef() && !arg.TypeAndValue.Pointer
                && !arg.TypeAndValue.IsCxxRefToPointer)
            {
                spelling = "const " + spelling;
                constSpelled = true;
            }
            // Preserve top-level const on both class lvalues and by-value prvalues.
            else if ((classArgument || decayedArray) && CxxConstReceiverKind(arg) != 0)
            {
                spelling = "const " + spelling;
                constSpelled = true;
            }
            else if (arg.TypeAndValue.PointeeConstBit() && arg.TypeAndValue.Pointer
                     && !arg.TypeAndValue.IsFunctionPointer && spelling.ends_with("*")
                     && !spelling.starts_with("const "))
            {
                spelling = "const " + spelling;
                constSpelled = true;
            }
            const bool classRvalue = classArgument && arg.IsRvalue;
            if (classRvalue && arg.IsCxxDirectConstruction)
            {
                std::string construction;
                if (!appendDirectConstruction(arg, appendDirectConstruction, construction))
                {
                    if (!error.empty()) return false;
                    return noMatch("a direct C++ construction argument cannot be forwarded");
                }
                callArguments.push_back(std::move(construction));
                continue;
            }
            /*
             * The wrapper takes the first-element pointer and rebuilds the caller's array lvalue,
             * so C++ deduces `T&` / `T(&)[N]` as the array and a by-value `T` decays on its own.
             */
            if (decayedArray && spelling.ends_with("*"))
            {
                uint64_t extent = arg.TypeAndValue.ConstArraySize;
                size_t innerDimensionStart = 0;
                if (extent == 0)
                {
                    if (auto* array = llvm::dyn_cast_or_null<llvm::ArrayType>(arg.BaseType))
                        extent = array->getNumElements();
                    else if (!arg.TypeAndValue.ConstInnerDimensions.empty())
                    {
                        // An indexed row carries its remaining extent as the first inner dimension.
                        extent = arg.TypeAndValue.ConstInnerDimensions.front();
                        innerDimensionStart = 1;
                    }
                }
                if (extent != 0)
                {
                    std::string element = spelling.substr(0, spelling.size() - 1);
                    while (!element.empty() && element.back() == ' ') element.pop_back();
                    std::string extents = std::format("[{}]", extent);
                    for (size_t dimension = innerDimensionStart;
                         dimension < arg.TypeAndValue.ConstInnerDimensions.size(); ++dimension)
                        extents += std::format("[{}]", arg.TypeAndValue.ConstInnerDimensions[dimension]);
                    parameterSpellings.push_back(spelling);
                    callArguments.push_back(std::format("(*reinterpret_cast<{} (*){}>(p{}))",
                                                        element, extents, flatParameterIndex++));
                    continue;
                }
            }
            size_t templateParameterIndex = i - (selected->kind
                == cflat_cinterop::RawFunctionTemplate::InstanceMember ? 1u : 0u);
            if (selected->hasParameterPack && !selected->parameterTypes.empty()
                && templateParameterIndex >= selected->parameterTypes.size() - 1)
                templateParameterIndex = selected->parameterTypes.size() - 1;
            const bool forwardingReference = templateParameterIndex
                < selected->forwardingReferenceParameters.size()
                && selected->forwardingReferenceParameters[templateParameterIndex] != 0;
            const bool cxxRvalue = !provenLvalue;
            if (forwardingReference)
            {
                if (cxxRvalue) spelling += " &&";
                else spelling += " &";
            }
            else if (classRvalue) spelling += " &&";
            if (!forwardingReference && classArgument
                     && arg.Storage != nullptr && !arg.IsRvalue)
                spelling += " &";
            // A non-const lvalue reference binds the CALLER's object, not a wrapper copy.
            else if (!forwardingReference && bindsCallerLvalue) spelling += " &";
            /*
             * A scalar temporary at a reference position lives in the CALLER's frame, as C++'s
             * full-expression temporary does, so a returned `const T&` never points into the
             * wrapper frame.
             */
            else if (!forwardingReference && anyReference && !classArgument && !provenLvalue
                     && !stringLiteralArgument && spelling != "const char *")
                spelling += " &&";
            /*
             * A string literal at a const reference lives in the caller too, but reaches C++ as a
             * NAMED `const char *` as on master, so overload ranking (`const U&` over `const U&&`)
             * is unchanged. A non-const `U&` keeps the by-value lvalue for the literal retry.
             */
            else if (!forwardingReference && anyReference && !classArgument && !provenLvalue
                     && !anyLvalueReference)
                spelling += " &";
            const std::string valueSpelling = spelling.ends_with(" &&")
                ? spelling.substr(0, spelling.size() - 3) : spelling;
            parameterSpellings.push_back(std::move(spelling));
            const std::string parameter = "p" + std::to_string(flatParameterIndex++);
            const bool packArgument = selected->hasParameterPack && i >= selected->minArity;
            if (forwardingReference && cxxRvalue)
                callArguments.push_back("static_cast<" + valueSpelling + "&&>(" + parameter + ")");
            else if (packArgument && cxxRvalue)
            {
                std::string packSpelling = valueSpelling;
                if (packSpelling.ends_with(" &")) packSpelling.resize(packSpelling.size() - 2);
                callArguments.push_back("static_cast<" + packSpelling + "&&>(" + parameter + ")");
            }
            else if (classRvalue)
                callArguments.push_back("static_cast<" + valueSpelling + "&&>(" + parameter + ")");
            // A temporary reaches C++ as an rvalue, so a `const U&` or non-template sibling wins.
            else if ((anyLvalueReference || uniqueRegistration
                      || parameterSpellings.back().ends_with(" &&"))
                     && !bindsCallerLvalue && !stringLiteralArgument && !braceForArgument(i))
                callArguments.push_back("static_cast<" + valueSpelling + "&&>(" + parameter + ")");
            else
                callArguments.push_back(parameter);
            // An unnamed-enum rvalue deduces the enum type in C++, not the int CFlat holds it as.
            if (!unnamedEnumType.empty())
                callArguments.back() = "static_cast<" + unnamedEnumType + ">("
                    + callArguments.back() + ")";
            if (inlineStringLiteral)
                literalCallArguments.emplace_back(callArguments.size() - 1,
                                                  std::move(literalSpelling));
            else if (IsNullPointerConstantArgument(arg, true))
            {
                const std::string& identity = arg.IsCxxMacroGlobal
                    ? arg.TypeAndValue.TypeName : arg.LiteralIdentity;
                if (std::string zero = CxxZeroLiteralSpelling(identity); !zero.empty())
                    literalCallArguments.emplace_back(callArguments.size() - 1, std::move(zero));
            }
        }

        std::vector<std::string> cxxExplicitArgs;
        for (const std::string& typeArg : explicitArgs)
        {
            // A non-type argument is already an integer constant: spell it literally.
            if (isValueArg(typeArg))
            {
                cxxExplicitArgs.push_back(typeArg.substr(1));
                continue;
            }
            std::string spelling;
            if (!CxxTemplateArgSpellingForCflatType(typeArg, spelling))
                return noMatch("an explicit type argument cannot be spelled in C++");
            cxxExplicitArgs.push_back(std::move(spelling));
        }

        std::string explicitSuffix;
        if (!cxxExplicitArgs.empty())
        {
            explicitSuffix = "<";
            for (size_t i = 0; i < cxxExplicitArgs.size(); ++i)
            {
                if (i != 0) explicitSuffix += ", ";
                explicitSuffix += cxxExplicitArgs[i];
            }
            explicitSuffix += ">";
        }
        const bool instance = selected->kind == cflat_cinterop::RawFunctionTemplate::InstanceMember;
        /*
         * A BINARY OPERATOR is asked for as an EXPRESSION, not as a named call: `p0 + p1` lets
         * C++ consider every candidate the operator has - the namespace's templates, a hidden
         * friend, a member, and the C++20 rewritten and reversed forms - where `ns::operator+(..)`
         * sees only the named overload set and loses all three.
         */
        const bool infixForm = !infixOperator.empty() && !instance
            && parameterSpellings.size() == 2;
        auto makeTargetCall = [&](const std::vector<std::string>& calls) {
            std::string targetCall;
            if (infixForm) targetCall = calls[0] + " " + infixOperator + " " + calls[1];
            else if (instance)
                targetCall = calls.front() + explicitSuffix + "(";
            else
            {
                const std::string staticTarget = selected->kind
                    == cflat_cinterop::RawFunctionTemplate::StaticMember
                    ? ownerSpelling + "::" + selected->memberName : selected->cxxSpelling;
                targetCall = staticTarget + explicitSuffix + "(";
            }
            if (!infixForm)
            {
                for (size_t i = instance ? 1u : 0u; i < calls.size(); ++i)
                {
                    if (i != (instance ? 1u : 0u)) targetCall += ", ";
                    targetCall += calls[i];
                }
                targetCall += ")";
            }
            return targetCall;
        };

        std::vector<size_t> dependencyGroups;
        auto addDependencyGroup = [&](size_t group) {
            if (std::find(dependencyGroups.begin(), dependencyGroups.end(), group)
                == dependencyGroups.end())
                dependencyGroups.push_back(group);
        };
        auto collectTypeDependencies = [&](auto&& self, const std::string& type) -> void {
            std::string base = type;
            while (!base.empty() && base.back() == '*') base.pop_back();
            // A pointer template argument demangles with its "ptr" suffix still in the name
            // ("cpptw.Twinptr"); the owner is registered under the pointee.
            while (!cxxTypeOwnerGroup_.contains(base) && base.size() > 3 && base.ends_with("ptr")
                   && base.find('$') == std::string::npos)
                base.resize(base.size() - 3);
            if (auto owner = cxxTypeOwnerGroup_.find(base);
                owner != cxxTypeOwnerGroup_.end()
                && owner->second < cxxImportGroups_.size()
                && !cxxImportGroups_[owner->second].headers.empty())
                addDependencyGroup(owner->second);
            TypeSpelling parsed;
            if (!DemangleType(*this, base, parsed)) return;
            for (const auto& arg : parsed.args)
                self(self, MangleType(*this, arg));
        };
        for (const auto& argument : arguments)
        {
            collectTypeDependencies(collectTypeDependencies, cflatTypeOf(argument));
            auto collectConstructionDependencies = [&](auto&& self,
                                                        const NamedVariable& construction) -> void {
                if (!construction.IsCxxDirectConstruction) return;
                collectTypeDependencies(collectTypeDependencies,
                                        construction.CxxDirectConstructionType);
                for (const NamedVariable& child : construction.CxxDirectConstructionArguments)
                {
                    collectTypeDependencies(collectTypeDependencies, cflatTypeOf(child));
                    self(self, child);
                }
            };
            collectConstructionDependencies(collectConstructionDependencies, argument);
        }
        for (const std::string& explicitArg : explicitArgs)
            if (!explicitArg.starts_with("#"))
                collectTypeDependencies(collectTypeDependencies, explicitArg);

        std::string generatedTypeSource;
        std::set<size_t> generatedDependencyGroups;
        std::unordered_set<std::string> incompleteTypes;
        std::vector<std::string> generatedTypeArguments;
        generatedTypeArguments.reserve(arguments.size() + explicitArgs.size());
        if (selected->kind == cflat_cinterop::RawFunctionTemplate::InstanceMember)
            generatedTypeArguments.push_back(ownerType);
        auto appendConstructionTypeArguments = [&](auto&& self,
                                                    const NamedVariable& construction) -> void {
            if (!construction.IsCxxDirectConstruction) return;
            generatedTypeArguments.push_back(construction.CxxDirectConstructionType);
            for (const NamedVariable& child : construction.CxxDirectConstructionArguments)
            {
                generatedTypeArguments.push_back(cflatTypeOf(child));
                self(self, child);
            }
        };
        for (const auto& argument : arguments)
        {
            generatedTypeArguments.push_back(cflatTypeOf(argument));
            appendConstructionTypeArguments(appendConstructionTypeArguments, argument);
        }
        for (const std::string& explicitArg : explicitArgs)
            if (!explicitArg.starts_with("#")) generatedTypeArguments.push_back(explicitArg);
        GeneratedCxxDefinitionsFor(generatedTypeArguments, generatedTypeSource,
                                   generatedDependencyGroups, incompleteTypes);
        for (size_t groupIndex : generatedDependencyGroups)
            addDependencyGroup(groupIndex);

        auto makeWrapperName = [&](const std::vector<std::string>& calls) {
            std::string hashKey = lookupName + std::to_string(selected->kind) + infixOperator;
            for (const auto& p : parameterSpellings) hashKey += p;
            for (const auto& a : calls) hashKey += "|call|" + a;
            for (const auto& a : cxxExplicitArgs) hashKey += a;
            for (const auto& brace : braceArguments)
            {
                hashKey += "|brace|";
                hashKey += std::to_string(brace.parameterIndex);
                hashKey += std::to_string(brace.elements.size());
                hashKey += braceElementSpelling(brace, {});
            }
            return std::format("__cflat_tpl_{:016x}", HashWrapperKey(hashKey));
        };
        auto makeWrapperInput = [&](const std::string& name, const std::vector<std::string>& calls) {
            std::string wrapperSource = wrapperNeedsInitializerList
                ? "#include <initializer_list>\n" : std::string();
            wrapperSource += "extern \"C\" __attribute__((weak)) decltype(auto) " + name + "(";
            for (size_t i = 0; i < parameterSpellings.size(); ++i)
            {
                if (i != 0) wrapperSource += ", ";
                wrapperSource += parameterSpellings[i] + " p" + std::to_string(i);
            }
            wrapperSource += ")";
            if (selected->isNoexcept && !infixForm) wrapperSource += " noexcept";
            wrapperSource += " { " + wrapperBodyPrefix + "return " + makeTargetCall(calls) + "; }\n";
            return generatedTypeSource + wrapperSource;
        };
        std::string wrapperName = makeWrapperName(callArguments);
        // An infix operator wrapper registers under its own unique name, never under the shared
        // "ns.operatorX" one: that name is the non-template free operators' candidate set, and a
        // wrapper built for ONE operand pair must not answer for a different pair.
        /*
         * So does a wrapper whose scalar parameters were spelled by value category (`int &` for an
         * lvalue, `int` for an rvalue): under the shared name, an lvalue call could resolve to an
         * rvalue sibling. Named operators keep the shared name; their callers collect by name.
         */
        const bool uniqueForCategory = categorySensitive && infixOperator.empty()
            && !functionName.starts_with("operator")
            && functionName.find(".operator") == std::string::npos;
        const bool uniqueForFunctionPointerSlot = !functionPointerCallerSlots.empty()
            || !constructionFunctionPointerSlots.empty();
        // So does one with a spelled `const`: its CFlat signature equals the non-const sibling's.
        const bool uniqueForConst = constSpelled && infixOperator.empty()
            && !functionName.starts_with("operator")
            && functionName.find(".operator") == std::string::npos;
        const bool uniqueForBraceBacking = !scalarBackedParams.empty() || !classBackedParams.empty();
        if (!explicitArgs.empty() || infixForm || uniqueForCategory || uniqueForConst
            || uniqueForFunctionPointerSlot || uniqueForBraceBacking || uniqueRegistration || closureArgument
            || literalArrayArgument || hasDirectConstructionArgument)
        {
            registeredName = wrapperName;
            if (functionTable.find(wrapperName) != functionTable.end()
                && scalarBackedParams.empty() && classBackedParams.empty())
            {
                ExpandCxxBraceArguments(arguments, braceArguments);
                return true;
            }
        }
        auto groupIt = cxxFunctionTemplateOwnerGroup_.find(selected->name);
        if (groupIt == cxxFunctionTemplateOwnerGroup_.end())
            return noMatch("the template's import group is unavailable");
        CxxRequestGroup group = MakeCxxRequestGroup(groupIt->second, dependencyGroups);
        if (group.headers.empty()) return noMatch("the template's import group is unavailable");
        CxxRequestGroupScope groupScope(*this, &group);

        CSigEntry bound;
        std::string wrapperError;
        const bool allowIncremental = true;
        bool wrapped = RequestGeneratedCxxWrapper(group, makeWrapperInput(wrapperName, callArguments),
                                                  wrapperName, "TPL", bound, wrapperError,
                                                  incompleteTypes.empty(), allowIncremental);
        // Only a result that IS a CFlat closure (or a reference to one) takes the closure-result
        // adapters. A record holding a closure (a holder, OwnedBox) is a plain record result.
        std::string baseReturnClosureSpelling = bound.retSpelling;
        const bool baseReturnsConstClosure = baseReturnClosureSpelling.starts_with("const ");
        if (baseReturnsConstClosure)
            baseReturnClosureSpelling.erase(0, 6);
        const bool baseWrapperReturnsClosure = wrapped
            && baseReturnClosureSpelling.starts_with("__cflat_closure<");
        const bool closureReferenceMayDangle = std::any_of(
            arguments.begin(), arguments.end(), [](const NamedVariable& argument) {
                return !argument.CxxClosureSignature.empty() && !argument.CxxClosureBorrowed;
            });
        // Refuse before the adapters: their address-of an xvalue would leak clang's text.
        if (closureArgument && closureReferenceMayDangle && wrapped
            && IsCxxClosureReferenceResult(bound.retSpelling))
        {
            error = CxxClosureReferenceResultRefusal(lookupName);
            return false;
        }
        if (closureArgument && (!wrapped || baseWrapperReturnsClosure))
        {
            for (const NamedVariable& argument : arguments)
                if (!argument.CxxClosureSignature.empty())
                {
                    closureResultType = argument.HasOriginalArgumentType
                        ? argument.OriginalArgumentType : argument.TypeAndValue;
                    closureResultType.TypeName = "__closure_fat_ptr";
                    closureResultType.Pointer = false;
                    closureResultType.ElemPointer = false;
                    closureResultType.PointerDepth = 0;
                    closureResultType.IsAlias = false;
                    closureResultType.IsRvalueRef = false;
                    closureResultType.IsCxxRefToPointer = false;
                    closureResultType.DropCxxRef();
                    closureResultType.IsFunctionPointer = true;
                    break;
                }
            auto makeValueAdapter = [&](const std::string& name,
                                        const std::vector<std::string>& calls) {
                std::string source = wrapperNeedsInitializerList
                    ? "#include <initializer_list>\n" : std::string();
                source += "extern \"C\" __attribute__((weak)) void " + name + "(";
                for (size_t i = 0; i < parameterSpellings.size(); ++i)
                {
                    if (i != 0) source += ", ";
                    source += parameterSpellings[i] + " p" + std::to_string(i);
                }
                source += ")";
                if (selected->isNoexcept && !infixForm) source += " noexcept";
                source += " { " + wrapperBodyPrefix + "auto result = " + makeTargetCall(calls)
                    + "; result.release_to(p" + std::to_string(parameterSpellings.size() - 1)
                    + "); }\n";
                return generatedTypeSource + source;
            };
            auto makeReferenceAdapter = [&](const std::string& name,
                                            const std::vector<std::string>& calls) {
                std::string source = wrapperNeedsInitializerList
                    ? "#include <initializer_list>\n" : std::string();
                source += "extern \"C\" __attribute__((weak)) void *" + name + "(";
                for (size_t i = 0; i < parameterSpellings.size(); ++i)
                {
                    if (i != 0) source += ", ";
                    source += parameterSpellings[i] + " p" + std::to_string(i);
                }
                source += ")";
                if (selected->isNoexcept && !infixForm) source += " noexcept";
                // A closure prvalue would take the address of a temporary that dies at the return:
                // the check fails the wrapper, so the value adapter takes it.
                const std::string targetCall = makeTargetCall(calls);
                const std::string resultType = "decltype(" + targetCall + ")";
                source += " { " + wrapperBodyPrefix + "static_assert(__is_lvalue_reference(" + resultType
                    + ") || __is_rvalue_reference(" + resultType + ") || !__cflat_is_closure_value<"
                    + resultType + ">::value); return const_cast<void *>(static_cast<const void *>(&"
                    + targetCall + ")); }\n";
                return generatedTypeSource + source;
            };

            CSigEntry referenceBound;
            std::string baseReturnSpelling = bound.retSpelling;
            while (!baseReturnSpelling.empty()
                   && std::isspace(static_cast<unsigned char>(baseReturnSpelling.back())))
                baseReturnSpelling.pop_back();
            const bool baseReturnsReference = baseReturnSpelling.ends_with('&');
            const std::string referenceName = std::format(
                "__cflat_tpl_closure_ref_{:016x}", HashWrapperKey(wrapperName));
            if (!closureResultType.TypeName.empty()
                && (!baseWrapperReturnsClosure || baseReturnsReference)
                && RequestGeneratedCxxWrapper(group, makeReferenceAdapter(referenceName, callArguments),
                    referenceName, "TPL_CLOSURE_REF",
                    referenceBound, wrapperError, incompleteTypes.empty(), allowIncremental))
            {
                wrapperName = referenceName;
                registeredName = referenceName;
                bound = std::move(referenceBound);
                closureResultKind = FunctionSymbol::CxxClosureResultKind::Reference;
            }
            else if (!closureResultType.TypeName.empty()
                     && (!baseWrapperReturnsClosure || !baseReturnsReference))
            {
                parameterSpellings.push_back("void *");
                const std::string valueName = makeWrapperName(callArguments);
                CSigEntry valueBound;
                const std::string valueSource = makeValueAdapter(valueName, callArguments);
                parameterSpellings.pop_back();
                if (RequestGeneratedCxxWrapper(group, valueSource, valueName, "TPL_CLOSURE_VALUE",
                                               valueBound, wrapperError,
                                               incompleteTypes.empty(), allowIncremental))
                {
                    wrapperName = valueName;
                    registeredName = valueName;
                    bound = std::move(valueBound);
                    closureResultKind = FunctionSymbol::CxxClosureResultKind::Value;
                }
            }
            wrapped = closureResultKind != FunctionSymbol::CxxClosureResultKind::None;
        }
        // A closure-free failure keeps the literal retry and the copy-sink probe below.
        if (!wrapped && closureArgument)
        {
            lastCxxRequestClangRejected_ = true;
            return noMatch(wrapperError);
        }
        if (wrapped)
            for (size_t i = functionArgumentOffset; i < arguments.size(); ++i)
            {
                const auto& argument = arguments[i];
                if (argument.CxxClosureSignature.empty()
                    || !(argument.IsBonded || argument.ContainsBondedClosure
                         || !argument.LambdaReferenceCaptureNames.empty()))
                    continue;
                size_t parameterIndex = i - functionArgumentOffset;
                if (selected->hasParameterPack && !selected->parameterTypes.empty()
                    && parameterIndex >= selected->parameterTypes.size() - 1)
                    parameterIndex = selected->parameterTypes.size() - 1;
                const std::string parameterName = parameterIndex < selected->parameterNames.size()
                    ? selected->parameterNames[parameterIndex] : std::string();
                if (!parameterName.empty()
                    && bound.closureKeptParams.find("," + parameterName + ",")
                        != std::string::npos)
                {
                    error = std::format(
                        "a by-reference closure passed to C++ '{}' cannot be retained because its "
                        "captured local may leave scope", lookupName);
                    return false;
                }
            }
        if (closureArgument && closureReferenceMayDangle && wrapped
            && (closureResultKind == FunctionSymbol::CxxClosureResultKind::Reference
                || IsCxxClosureReferenceResult(bound.retSpelling)))
        {
            error = CxxClosureReferenceResultRefusal(lookupName);
            return false;
        }
        if (closureResultKind == FunctionSymbol::CxxClosureResultKind::Reference)
        {
            closureResultType.IsAlias = true;
            closureResultType.MarkCxxRef();
            // `const F&` stays const: a write through the result is refused like any const result.
            closureResultType.SetCxxConstRef(baseReturnsConstClosure);
        }
        if (closureArgument && wrapped && !bound.ret.Pointer && !bound.ret.IsFunctionPointer
            && IsCxxRecord(bound.ret.TypeName))
        {
            EnsureCxxRecordProjected(bound.ret.TypeName, false);
            bool closureForwarded = false;
            for (size_t i = 0; i < arguments.size(); ++i)
            {
                // A closure variable crosses as its own caller-frame slot: a kept reference is fine.
                if (arguments[i].CxxClosureSignature.empty() || arguments[i].CxxClosureBorrowed) continue;
                size_t parameterIndex = i - functionArgumentOffset;
                if (i < functionArgumentOffset) continue;
                if (selected->hasParameterPack && !selected->parameterTypes.empty()
                    && parameterIndex >= selected->parameterTypes.size() - 1)
                    parameterIndex = selected->parameterTypes.size() - 1;
                if (parameterIndex < selected->forwardingReferenceParameters.size()
                    && selected->forwardingReferenceParameters[parameterIndex])
                    closureForwarded = true;
            }
            const bool returnedOpaqueReferenceSlot =
                CxxRecordStoresForwardedClosureReference(bound.ret.TypeName,
                    bound.retSpelling.find("__cflat_closure<") != std::string::npos);
            if (closureForwarded && returnedOpaqueReferenceSlot)
            {
                error = std::format("cannot forward a CFlat closure to C++ '{}': its result stores "
                    "a reference to the forwarding argument, and CFlat cannot provide the required "
                    "caller-owned C++ closure slot", lookupName);
                return false;
            }
        }
        std::vector<std::string> boundCalls = callArguments;
        /*
         * First try the ordinary forwarded argument. If clang cannot bind it, retry string and
         * integer-null literals with their original source category in a separately keyed wrapper.
         */
        if (!wrapped && !literalCallArguments.empty())
        {
            std::vector<std::string> literalCalls = callArguments;
            for (const auto& [index, spelling] : literalCallArguments) literalCalls[index] = spelling;
            const std::string literalName = makeWrapperName(literalCalls);
            if (functionTable.find(literalName) != functionTable.end())
            {
                registeredName = literalName;
                ExpandCxxBraceArguments(arguments, braceArguments);
                return true;
            }
            const std::string forwardedCause = lastCxxWrapperCause_;
            CSigEntry literalBound;
            std::string literalError;
            if (RequestGeneratedCxxWrapper(group, makeWrapperInput(literalName, literalCalls),
                                           literalName, "TPL", literalBound, literalError,
                                           incompleteTypes.empty(), allowIncremental))
            {
                wrapped = true;
                boundCalls = literalCalls;
                wrapperName = literalName;
                registeredName = literalName;
                bound = std::move(literalBound);
            }
            else if (wrapperError.find("consteval") != std::string::npos
                     && literalError.find("unavailable") != std::string::npos)
                wrapperError = std::move(literalError);
            else lastCxxWrapperCause_ = forwardedCause;
        }
        if (!wrapped)
        {
            lastCxxRequestClangRejected_ = true;
            lastCxxRequestNoViableAtRequest_ = lastCxxWrapperNoViableAtRequest_;
            // Clang's own errors and notes for this failure; the move probe below overwrites them.
            const std::string wrapperCause = lastCxxWrapperCause_ + "\n" + wrapperError;
            // A refused non-template overload taking the element by const reference (assign,
            // insert(pos, x)) is the copy the call needed, when clang named that copy.
            if (instance && arguments.size() > 1)
            {
                const std::vector<NamedVariable> userArgs(arguments.begin() + 1, arguments.end());
                const std::string bareName = functionName.substr(functionName.rfind('.') + 1);
                size_t sinkIndex = 0;
                std::string sinkParam, sinkCause;
                bool refusedRvalue = false;
                if (FindRefusedCxxCopySink(ownerType, bareName, userArgs, sinkIndex, sinkParam,
                                           sinkCause, refusedRvalue))
                {
                    // Suggest 'move x' only when a bound T&& or by-value overload takes it.
                    const std::string& sinkType = userArgs[sinkIndex].TypeAndValue.TypeName;
                    bool boundMoveSink = false;
                    if (auto bound = functionTable.find(functionName); bound != functionTable.end())
                        for (const auto& symbol : bound->second)
                        {
                            if (!symbol.IsCxx || symbol.Parameters.size() != arguments.size())
                                continue;
                            const auto& p = symbol.Parameters[sinkIndex + 1];
                            boundMoveSink = boundMoveSink || p.IsRvalueRef
                                || (!p.IsAlias && !p.CxxConstRef() && !p.Pointer
                                    && p.TypeName == sinkType);
                        }
                    error = CxxDeletedCopyMessage(userArgs[sinkIndex], sinkParam, bareName,
                                                  boundMoveSink);
                    const std::string causeLine = CxxFirstDiagnosticLine(sinkCause);
                    if (!causeLine.empty()) error += std::format(" (clang: {})", causeLine);
                    return false;
                }
                // The member access deferred this refusal for the arguments; they do not
                // explain it, so report it as it stands.
                if (IsCxxElementCopySinkRefusal(ownerType, bareName))
                    if (const CxxClassInfo* info = GetCxxClassInfo(ownerType))
                        if (auto refusal = info->refusedMembers.find(bareName);
                            refusal != info->refusedMembers.end())
                        {
                            error = std::format("member '{}' of C++ class '{}' {}", bareName,
                                                DisplayCxxClassName(ownerType), refusal->second);
                            return false;
                        }
            }
            /*
             * A forwarding pack binds an lvalue as T&. Blame its copy only when clang's own
             * diagnostic names the copy constructor of that argument's class.
             */
            for (size_t i = instance ? 1u : 0u; i < arguments.size(); ++i)
            {
                std::string spelling;
                if (!IsCopyDeletedCxxLvalue(arguments[i])
                    || !CxxSpellingForCflatType(arguments[i].TypeAndValue.TypeName, spelling)
                    || !CxxDiagnosticBlamesCopyOf(wrapperCause, spelling))
                    continue;
                const auto& names = selected->parameterNames;
                const size_t userIndex = instance ? i - 1 : i;
                const std::string paramName = names.empty() ? std::string("args")
                    : userIndex < names.size() ? names[userIndex] : names.back();
                /*
                 * The suffix only: offer 'move x' when the moved spelling really resolves. A
                 * bound non-template overload of the same arity takes a moved argument first (a
                 * harvested `emplace_back<T>(T&&)`), so it decides; otherwise the moved
                 * spelling must instantiate this template.
                 */
                bool siblingBound = false, siblingTakesMove = false;
                if (auto bound = functionTable.find(functionName); bound != functionTable.end())
                    for (const auto& symbol : bound->second)
                    {
                        if (!symbol.IsCxx || symbol.Parameters.size() != arguments.size()
                            || (instance && symbol.Parameters[0].TypeName != ownerType)
                            || symbol.UniqueName.starts_with("__cflat_tpl_"))
                            continue;
                        siblingBound = true;
                        siblingTakesMove = siblingTakesMove
                            || (symbol.Parameters[i].IsRvalueRef
                                && symbol.Parameters[i].TypeName
                                       == arguments[i].TypeAndValue.TypeName);
                    }
                bool moveRemedy = siblingBound && siblingTakesMove;
                if (!siblingBound)
                {
                    std::vector<NamedVariable> moved = arguments;
                    moved[i].IsExplicitMove = true;
                    std::string movedName, movedError;
                    moveRemedy = RequestCxxFunctionTemplate(functionName, ownerType, explicitArgs,
                                                            moved, braceArguments, movedName,
                                                            movedError, infixOperator);
                    lastCxxRequestClangRejected_ = true;
                }
                error = CxxDeletedCopyMessage(arguments[i], paramName, functionName, moveRemedy);
                return false;
            }
            return noMatch(wrapperError);
        }

        /*
         * Clang converted a scalar operand into a temporary of the WRAPPER's frame for the
         * selected operator's `const S&`: re-spell that formal as `const S&` so the caller's slot
         * is the temporary (see RequestCxxFreeFunction).
         */
        if (std::vector<CxxScalarRetarget> retarget;
            infixForm && PlanCxxOperatorScalarRetarget(bound, arguments, infixOperator, retarget))
        {
            const std::vector<std::string> sourceSpellings = parameterSpellings;
            for (const CxxScalarRetarget& entry : retarget)
            {
                parameterSpellings[entry.index] = "const " + entry.spelling + " &";
                boundCalls[entry.index] = "p" + std::to_string(entry.index);
            }
            const std::string retargetName = makeWrapperName(boundCalls);
            const std::string retargetInput = makeWrapperInput(retargetName, boundCalls);
            parameterSpellings = sourceSpellings;
            wrapperName = retargetName;
            registeredName = retargetName;
            if (functionTable.find(retargetName) != functionTable.end())
            {
                StageCxxOperatorScalarArguments(arguments, retarget);
                ExpandCxxBraceArguments(arguments, braceArguments);
                return true;
            }
            CSigEntry retargetBound;
            if (!RequestGeneratedCxxWrapper(group, retargetInput, retargetName, "TPL",
                                            retargetBound, wrapperError,
                                            incompleteTypes.empty(), allowIncremental))
            {
                lastCxxRequestClangRejected_ = true;
                return noMatch(wrapperError);
            }
            bound = std::move(retargetBound);
            StageCxxOperatorScalarArguments(arguments, retarget);
        }
        /*
         * Clang resolved over the literal's own array and the winner binds a `X *const &` to its
         * DECAYED pointer, a temporary of the wrapper frame: re-spell that formal so the CALLER's
         * slot is the temporary (full-expression lifetime), and require the re-spelled call to
         * select the same callee.
         */
        if (std::string retargetError; !RetargetCxxLiteralPointerTemporaries(
                bound, literalArrayParams, parameterSpellings, boundCalls,
                [&](const std::vector<std::string>& calls, std::string& name) {
                    name = makeWrapperName(calls);
                    return makeWrapperInput(name, calls);
                },
                [&](const std::string& name, const std::string& input, CSigEntry& out,
                    std::string& requestError) {
                    return RequestGeneratedCxxWrapper(group, input, name, "TPL", out, requestError,
                                                      incompleteTypes.empty(), allowIncremental);
                },
                lookupName, wrapperName, retargetError))
        {
            error = std::move(retargetError);
            return false;
        }
        else if (wrapperName != registeredName && literalArrayArgument
                 && functionTable.find(wrapperName) != functionTable.end())
        {
            registeredName = wrapperName;
            ExpandCxxBraceArguments(arguments, braceArguments);
            return true;
        }
        if (literalArrayArgument) registeredName = wrapperName;

        const bool instanceWrapper = selected->kind == cflat_cinterop::RawFunctionTemplate::InstanceMember;
        const std::string defaultRegisteredName = instanceWrapper ? functionName : lookupName;
        if (registeredName.empty()) registeredName = defaultRegisteredName;
        bound.name = registeredName;
        bound.isInstanceTemplateWrapper = instanceWrapper;
        for (const auto& [parameterIndex, argumentIndex] : functionPointerCallerSlots)
            if (parameterIndex < bound.params.size() && argumentIndex < arguments.size())
            {
                bound.params[parameterIndex] = arguments[argumentIndex].TypeAndValue;
                if (bound.params[parameterIndex].TypeName.empty())
                    bound.params[parameterIndex].TypeName = "__c_fn_ptr";
                bound.params[parameterIndex].IsFunctionPointer = true;
            }
        for (const auto& [parameterIndex, slotType] : constructionFunctionPointerSlots)
            if (parameterIndex < bound.params.size())
            {
                bound.params[parameterIndex] = slotType;
                if (bound.params[parameterIndex].TypeName.empty())
                    bound.params[parameterIndex].TypeName = "__c_fn_ptr";
                bound.params[parameterIndex].IsFunctionPointer = true;
            }
        RegisterCSignatures({ bound }, selected->file.empty() ? group.headers.front() : selected->file);
        if (closureResultKind != FunctionSymbol::CxxClosureResultKind::None)
            if (auto it = functionTable.find(registeredName); it != functionTable.end())
                for (auto& symbol : it->second)
                    // A repeat request registers the wrapper again: adapt every unadapted copy.
                    if (symbol.External && symbol.UniqueName == wrapperName
                        && symbol.CxxClosureResult == FunctionSymbol::CxxClosureResultKind::None)
                    {
                        if (closureResultKind == FunctionSymbol::CxxClosureResultKind::Value)
                        {
                            if (symbol.Parameters.empty())
                                return noMatch("the closure result adapter has no output slot");
                            symbol.Parameters.pop_back();
                            symbol.ReturnsOwned = true;
                            symbol.ReturnsAlias = false;
                        }
                        else
                        {
                            symbol.ReturnsOwned = false;
                            symbol.ReturnsAlias = true;
                        }
                        symbol.ReturnType = closureResultType;
                        symbol.CxxClosureResult = closureResultKind;
                    }
        bool registered = false;
        if (auto it = functionTable.find(registeredName); it != functionTable.end())
            for (const auto& symbol : it->second)
                if (symbol.External && symbol.UniqueName == wrapperName)
                { registered = true; break; }
        if (!registered) return noMatch("the generated wrapper could not be registered");
        cxxDeductionWrappers_.insert(wrapperName);
        if (auto* sink = GetSymbolSink())
            sink->Register(SymbolKind::Function, selected->name,
                           selected->file.empty() ? group.headers.front() : selected->file,
                           selected->line, selected->col < 0 ? 0 : selected->col,
                           "template <...> " + selected->name + "(...)");
        std::vector<CxxBraceArgument> backedBraceArguments = braceArguments;
        for (auto& brace : backedBraceArguments)
        {
            auto backed = scalarBackedParams.find(brace.argumentIndex);
            if (backed == scalarBackedParams.end()) continue;
            TypeAndValue elementType;
            llvm::Type* elementTy = MapCTypeToTypeAndValue(backed->second.second, elementType, true)
                ? GetType(elementType) : nullptr;
            if (elementTy == nullptr || (!elementTy->isIntegerTy() && !elementTy->isFloatingPointTy())
                || backed->second.first >= bound.params.size())
            {
                error = std::format("C++ brace-list element type '{}' has no scalar layout",
                                    backed->second.second);
                return false;
            }
            NamedVariable data;
            if (!EmitCxxBraceBacking(brace, elementType, elementTy, bound.params[backed->second.first],
                                     lookupName, "parameter", data, error))
                return false;
            brace.wrapperArguments.clear();
            brace.wrapperArguments.push_back(std::move(data));
        }
        for (auto& brace : backedBraceArguments)
        {
            auto backed = classBackedParams.find(brace.argumentIndex);
            if (backed == classBackedParams.end()) continue;
            TypeAndValue elementType;
            elementType.TypeName = backed->second.second;
            llvm::Type* elementTy = GetType(elementType);
            if (elementTy == nullptr || !elementTy->isSized()
                || backed->second.first >= bound.params.size())
            {
                error = std::format("C++ brace-list element type '{}' has no class layout",
                                    backed->second.second);
                return false;
            }
            const uint64_t align = GetEffectiveAlignmentForType(backed->second.second, elementTy);
            auto* arrayTy = llvm::ArrayType::get(elementTy, brace.elements.size());
            auto* array = AllocaAtEntry(arrayTy, nullptr, "cxx.tpl.brace.array", align);
            std::vector<llvm::Value*> slots;
            slots.reserve(brace.elements.size());
            for (size_t index = 0; index < brace.elements.size(); ++index)
            {
                auto* dst = builder->CreateInBoundsGEP(
                    arrayTy, array, { builder->getInt64(0), builder->getInt64(index) },
                    "cxx.tpl.brace.element");
                NamedVariable& source = brace.elements[index];
                auto* src = source.Storage;
                if (src == nullptr && source.IsRvalue && source.Primary != nullptr
                    && source.Primary->getType() == elementTy && elementTy->isStructTy()
                    && HasTrivialCxxDtor(backed->second.second))
                {
                    src = AllocaAtEntry(elementTy, nullptr, "cxx.tpl.brace.source", align);
                    builder->CreateStore(source.Primary, src);
                }
                if (src == nullptr
                    || !EmitCxxCopyOrMoveConstruct(
                        backed->second.second, dst, src, source.IsExplicitMove || source.IsRvalue,
                        "into a function-template initializer_list element"))
                {
                    if (src == nullptr)
                        error = std::format("cannot copy C++ class '{}' into a brace-list element: "
                                            "the element is not addressable",
                                            DisplayCxxClassName(backed->second.second));
                    return false;
                }
                slots.push_back(dst);
            }
            if (IsForeignNontrivialCxxClass(backed->second.second))
                for (auto it = slots.rbegin(); it != slots.rend(); ++it)
                    RegisterOwnedStructTemp(*it, backed->second.second);
            auto* dataPtr = builder->CreateInBoundsGEP(
                arrayTy, array, { builder->getInt64(0), builder->getInt64(0) },
                "cxx.tpl.brace.data");
            NamedVariable data;
            data.TypeAndValue = bound.params[backed->second.first];
            data.TypeAndValue.VariableName.clear();
            data.Primary = dataPtr;
            data.BaseType = dataPtr->getType();
            data.Storage = nullptr;
            data.IsRvalue = true;
            brace.wrapperArguments.clear();
            brace.wrapperArguments.push_back(std::move(data));
        }
        ExpandCxxBraceArguments(arguments, backedBraceArguments);
        return true;
}

bool LLVMBackend::RequestCxxFreeFunction(const std::string& functionName,
                                         const std::vector<std::string>& explicitArgs,
                                         std::vector<NamedVariable>& arguments,
                                         std::string& registeredName,
                                         std::string& error,
                                         const std::string& infixOperator,
                                         const std::string& infixLhsName,
                                         const std::string& infixRhsName,
                                         bool adlCall,
                                         const std::vector<std::pair<size_t, size_t>>& braceGroups,
                                         const std::string& memberOwnerType)
{
        lastCxxFreeWrapperLinkage_.clear();
        if (!RequestCxxFreeFunctionImpl(functionName, explicitArgs, arguments, registeredName, error,
                                        infixOperator, infixLhsName, infixRhsName, adlCall,
                                        braceGroups, memberOwnerType))
            return false;
        if (!infixOperator.empty() || adlCall || !braceGroups.empty() || !memberOwnerType.empty())
            return true;
        return SwitchCxxDirectByValueCall(functionName, arguments, registeredName, error);
}

/*
 * T518: C++ copy-initializes a by-value class parameter at its argument's own position, before
 * the later arguments run. Through the generated wrapper (`T &pN` parameters) that copy would be
 * made inside the wrapper, after every argument. So a non-trivially-copyable class lvalue that a
 * later argument names is copy-constructed at its position behind a decision (the call site, see
 * CxxEarlyCopySlot), and when clang's selected callee takes it by value the call bypasses the
 * wrapper: the selected callee is called through its address (a generated getter), on the
 * by-value C++ ABI path, with that copy as the owned prvalue parameter - one copy, in order.
 */
bool LLVMBackend::SwitchCxxDirectByValueCall(const std::string& functionName,
                                             std::vector<NamedVariable>& arguments,
                                             std::string& registeredName, std::string& error)
{
        if (lastCxxFreeWrapperLinkage_.empty()
            || std::none_of(arguments.begin(), arguments.end(), [&](const NamedVariable& a) {
                   return a.CxxEarlyCopySlot != nullptr
                       || (a.IsCxxPrvalueTemp && a.Storage != nullptr && !a.TypeAndValue.Pointer
                           && IsCxxRecord(a.TypeAndValue.TypeName));
               }))
            return true;
        const std::string linkage = lastCxxFreeWrapperLinkage_;
        const CSigEntry* wrapperSig = nullptr;
        for (const std::string& key : { linkage, registeredName })
            if (auto sigs = cxxFunctionSignatures_.find(key);
                wrapperSig == nullptr && sigs != cxxFunctionSignatures_.end())
                for (auto sig = sigs->second.rbegin(); sig != sigs->second.rend(); ++sig)
                    if (sig->linkageName == linkage)
                    {
                        wrapperSig = &*sig;
                        break;
                    }
        if (wrapperSig == nullptr) return true;
        const std::string bindKinds = wrapperSig->paramBindKinds;
        const std::string calleeType = wrapperSig->selectedCalleeType;
        const std::string calleeTemplateArgs = wrapperSig->selectedCalleeTemplateArgs;
        const std::vector<cflat_cinterop::RawDefaultArg> calleeDefaults = wrapperSig->selectedCalleeDefaults;
        // A deferred `?:` / construction argument is flattened into several wrapper parameters.
        auto leaves = [](auto&& self, const NamedVariable& value) -> size_t {
            if (!value.IsCxxDirectConstruction) return 1;
            size_t count = 0;
            for (const NamedVariable& child : value.CxxDirectConstructionArguments) count += self(self, child);
            return count;
        };
        std::vector<size_t> byValue;
        bool wrapperBuilt = false;
        size_t flat = 0;
        for (size_t i = 0; i < arguments.size(); ++i)
        {
            const size_t index = flat;
            flat += leaves(leaves, arguments[i]);
            wrapperBuilt = wrapperBuilt || arguments[i].IsCxxDirectConstruction;
            if (index < bindKinds.size() && bindKinds[index] == 'v'
                && (arguments[i].CxxEarlyCopySlot != nullptr
                    || (arguments[i].IsCxxPrvalueTemp && arguments[i].Storage != nullptr
                        && !arguments[i].TypeAndValue.Pointer
                        && IsCxxRecord(arguments[i].TypeAndValue.TypeName))))
                byValue.push_back(i);
        }
        auto clearMarkers = [&] {
            for (NamedVariable& a : arguments)
            {
                a.CxxEarlyCopySlot = nullptr;
                a.CxxEarlyCopyJoin = nullptr;
                a.CxxEarlyCopyDecision = nullptr;
                a.CxxEarlyCopyNamedLaterWrite = false;
            }
        };
        if (byValue.empty())
        {
            // Reference selections keep the object; the copies stay unconstructed.
            for (NamedVariable& a : arguments)
                if (a.CxxEarlyCopySlot != nullptr)
                {
                    a.CxxEarlyCopySlot = nullptr;
                    a.CxxEarlyCopyJoin = nullptr;
                    a.CxxEarlyCopyDecision = nullptr;
                    a.CxxEarlyCopyNamedLaterWrite = false;
                }
            return true;
        }
        const bool hasNamedEarlyCopyByValue = std::any_of(byValue.begin(), byValue.end(), [&](size_t i) {
            return arguments[i].CxxEarlyCopySlot != nullptr
                && arguments[i].CxxEarlyCopyNamedLaterWrite;
        });
        const size_t first = byValue.front();
        auto refuse = [&](const std::string& reason) {
            if (!hasNamedEarlyCopyByValue)
            {
                // Prvalues already use the wrapper's established move path; these refusals only
                // apply when an early lvalue copy needs the selected callee's by-value ABI.
                clearMarkers();
                return true;
            }
            error = std::format("argument {} of C++ '{}' initializes a by-value '{}' parameter, which C++ "
                                "copies before the later arguments run; this call cannot make that copy "
                                "in place because {}, so copy it into a local first and pass that",
                                first + 1, functionName, arguments[first].TypeAndValue.TypeName, reason);
            clearMarkers();
            return false;
        };
        if (wrapperBuilt)
            return refuse("another argument is a '?:' or construction that only the call's wrapper can build");
        if (calleeType.empty())
            return refuse("the selected callee is not a plain function whose address can be taken");
        if (calleeDefaults.size() < arguments.size())
            return refuse("the selected callee is variadic");
        for (size_t k = arguments.size(); k < calleeDefaults.size(); ++k)
            if (calleeDefaults[k].kind.empty() || calleeDefaults[k].kind == "nonconst"
                || calleeDefaults[k].kind == "unsupported")
                return refuse(std::format("default argument {} of the selected callee is not a constant", k + 1));
        auto owner = cxxFreeWrapperGroups_.find(linkage);
        if (owner == cxxFreeWrapperGroups_.end())
            return refuse("the call's C++ import group is not known");
        std::string cxxBase = functionName;
        for (size_t pos = 0; (pos = cxxBase.find('.', pos)) != std::string::npos; pos += 2)
            cxxBase.replace(pos, 1, "::");
        const std::string name = std::format("__cflat_byval_{:016x}", HashWrapperKey(
            std::format("{}{}|{}|{}", cxxBase, calleeTemplateArgs, calleeType, owner->second.first)));
        const std::string getter = name + "_fp";
        bool known = false;
        if (auto fit = functionTable.find(name); fit != functionTable.end())
            for (const auto& symbol : fit->second)
                known = known || (symbol.External && symbol.UniqueName == name);
        if (!known)
        {
            CxxRequestGroup group = MakeCxxRequestGroup(owner->second.first, owner->second.second);
            if (group.headers.empty()) return refuse("the call's C++ import group is not known");
            CxxRequestGroupScope groupScope(*this, &group);
            const std::string source = "using " + name + "_t = " + calleeType + ";\n"
                + "extern \"C\" " + name + "_t " + name + ";\n"
                + "extern \"C\" __attribute__((weak)) void* " + getter + "() { return reinterpret_cast<void*>("
                + "static_cast<" + name + "_t*>(&" + cxxBase + calleeTemplateArgs + ")); }\n";
            CSigEntry direct;
            std::string requestError;
            if (!RequestGeneratedCxxWrapper(group, source, name, "BYVAL", direct, requestError))
            {
                const std::string cause = CxxFirstDiagnosticLine(lastCxxWrapperCause_);
                return refuse("the selected callee's address cannot be taken"
                              + (cause.empty() ? std::string() : " (clang: " + cause + ")"));
            }
            direct.defaultArgs = calleeDefaults;
            direct.name = name;
            RegisterCSignatures({ direct }, group.headers.front());
            known = false;
            if (auto fit = functionTable.find(name); fit != functionTable.end())
                for (const auto& symbol : fit->second)
                    known = known || (symbol.External && symbol.UniqueName == name);
            if (!known) return refuse("the selected callee could not be registered");
        }
        cxxDirectCalleeGetters_[name] = getter;
        for (size_t i = 0; i < arguments.size(); ++i)
        {
            NamedVariable& a = arguments[i];
            const bool direct = std::find(byValue.begin(), byValue.end(), i) != byValue.end();
            if (a.CxxEarlyCopyDecision != nullptr)
                a.CxxEarlyCopyDecision->setOperand(0, builder->getInt1(
                    direct || (a.CxxEarlyCopySlot == nullptr && i < bindKinds.size() && bindKinds[i] == 'v')));
            if (direct && a.CxxEarlyCopySlot != nullptr)
            {
                // The early copy is the by-value parameter: an owned prvalue the call elides into.
                llvm::AllocaInst* slot = a.CxxEarlyCopySlot;
                a.Storage = a.CxxEarlyCopyJoin != nullptr ? static_cast<llvm::Value*>(a.CxxEarlyCopyJoin) : slot;
                a.BaseType = slot->getAllocatedType();
                a.Primary = builder->CreateLoad(slot->getAllocatedType(), a.Storage);
                a.IsRvalue = true;
                a.IsCxxPrvalueTemp = true;
                a.TypeAndValue.VariableName.clear();
            }
        }
        clearMarkers();
        registeredName = name;
        return true;
}

bool LLVMBackend::RequestCxxFreeFunctionImpl(const std::string& functionName,
                                             const std::vector<std::string>& explicitArgs,
                                             std::vector<NamedVariable>& arguments,
                                             std::string& registeredName,
                                             std::string& error,
                                             const std::string& infixOperator,
                                             const std::string& infixLhsName,
                                             const std::string& infixRhsName,
                                             bool adlCall,
                                             const std::vector<std::pair<size_t, size_t>>& braceGroups,
                                             const std::string& memberOwnerType)
{
        static constexpr std::string_view kArrayReferenceParameterMarker =
            "(&cflat_array_argument_marker)";
        const bool memberCall = !memberOwnerType.empty();
        const std::string memberName = memberCall
            ? functionName.substr(functionName.rfind('.') == std::string::npos
                ? 0 : functionName.rfind('.') + 1)
            : std::string();
        lastCxxRequestClangRejected_ = false;
        lastCxxRequestNoViableAtRequest_ = false;
        /*
         * An INFIX request (`p0 << p1`) lets C++ pick among members, free operators and ADL
         * candidates; it registers under its own wrapper name, never the shared operator name.
         * An operand given as a C++ NAME (`std.cout`, `std.endl`) is spelled into the expression
         * itself instead of becoming a parameter.
         */
        const size_t infixNames = (infixLhsName.empty() ? 0 : 1) + (infixRhsName.empty() ? 0 : 1);
        const bool infix = !infixOperator.empty() && arguments.size() + infixNames == 2
            && explicitArgs.empty();
        const size_t infixRhsIndex = infixLhsName.empty() ? 1 : 0;
        error.clear();
        registeredName.clear();
        const bool hasDirectConstructionArgument = std::any_of(
            arguments.begin(), arguments.end(), [](const auto& argument) {
                return argument.IsCxxDirectConstruction;
            });
        if (!memberCall && !functionName.starts_with("std.") && !adlCall)
            if (std::string refusal = GetCxxBindingRefusal(functionName); !refusal.empty())
            {
                auto fit = functionTable.find(functionName);
                if (fit == functionTable.end() || fit->second.empty())
                {
                    error = refusal;
                    return false;
                }
            }
        const bool functionMacro = cxxFunctionMacros_.count(functionName) != 0;
        if ((functionName.find('.') == std::string::npos && !functionMacro && !memberCall && !adlCall)
            || !HasCxxImportGroup())
            return false;
        for (NamedVariable& argument : arguments)
        {
            LowerNonCapturingClosureForCxx(argument);
            if (std::string refusal = CxxClosureArgumentRefusal(argument, functionName);
                !refusal.empty())
            {
                error = std::move(refusal);
                return false;
            }
        }

        auto argumentType = [&](const NamedVariable& arg) {
            TypeAndValue type = CxxFixedArrayDecayType(arg);
            if (IsCxxNullTypedArgument(arg))
            {
                type = TypeAndValue{};
                type.TypeName = "std.nullptr_t";
                return type;
            }
            if (!arg.LiteralIdentity.empty())
                type.TypeName = arg.LiteralIdentity;
            // An int-backed unscoped C++ enum (every MSVC one) arrives with TypeName dropped like a
            // primitive; an IMPORTED enum keeps its identity, a CFlat enum stays its backing.
            else if (type.TypeName.empty() && !type.Pointer && !arg.InferSourceTypeName.empty()
                     && (IsCxxForeignNamespace(arg.InferSourceTypeName)
                         || cxxTypeOwnerGroup_.count(arg.InferSourceTypeName) != 0)
                     && (!ResolveEnumTypeName(arg.InferSourceTypeName).empty()
                         || enumBackingTypes.count(arg.InferSourceTypeName) != 0))
                type.TypeName = arg.InferSourceTypeName;
            // Preserve a floating source type for template deduction even if argument setup
            // widened the LLVM value to match a candidate wrapper.
            else if (arg.InferSourceTypeName == "float" || arg.InferSourceTypeName == "double"
                     || arg.InferSourceTypeName == "longdouble")
                type.TypeName = arg.InferSourceTypeName;
            // An arithmetic result keeps its C++ type (`l + 1` is `long`, not the i64 TypeName).
            if (std::string arith = CxxArithIdentityForArgument(arg, arg.BaseType); !arith.empty())
                type.TypeName = arith;
            // The declared primitive identity outranks the machine-type guess below, which
            // cannot tell `char` from `i8` or `long` from `i64`.
            if (type.TypeName.empty())
                type.TypeName = DeclaredPrimitiveIdentityForCxxArgument(arg, arg.BaseType);
            if (type.TypeName.empty())
            {
                auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(arg.Primary);
                if (constant != nullptr && IsStringLiteralConstant(constant))
                {
                    type.TypeName = "char";
                    type.Pointer = true;
                }
                else if (arg.BaseType != nullptr && arg.BaseType->isFloatTy())
                    type.TypeName = "float";
                else if (arg.BaseType != nullptr && arg.BaseType->isDoubleTy())
                    type.TypeName = "double";
                else if (arg.BaseType != nullptr && arg.BaseType->isIntegerTy())
                {
                    const unsigned bits = arg.BaseType->getIntegerBitWidth();
                    type.TypeName = bits == 1 ? "bool" : bits <= 8 ? "i8" : bits <= 16 ? "short"
                        : bits <= 32 ? "int" : "i64";
                }
                else if (auto* st = llvm::dyn_cast_or_null<llvm::StructType>(arg.BaseType))
                    type.TypeName = st->getName().str();
                if (type.TypeName.empty()) type.TypeName = arg.InferSourceTypeName;
            }
            if (type.TypeName.empty() && arg.Primary != nullptr)
            {
                if (auto* gep = llvm::dyn_cast<llvm::GEPOperator>(
                        arg.Primary->stripPointerCasts()))
                {
                    type.TypeName = ResolvePointerElementTypeName(gep->getPointerOperand());
                    type.Pointer = !type.TypeName.empty();
                }
            }
            if (type.TypeName.empty() && type.Pointer)
                type.TypeName = arg.InferSourceTypeName;
            return type;
        };
        auto cflatTypeOf = [&](const NamedVariable& arg) {
            TypeAndValue type = argumentType(arg);
            std::string result = type.TypeName;
            for (int i = 0; i < type.ValuePointerDepth(); ++i) result += "*";
            return result;
        };
        auto enumSpelling = [&](const TypeAndValue& type, std::string& spelling) {
            std::string key = ResolveEnumTypeName(type.TypeName);
            if (key.empty() && enumBackingTypes.count(type.TypeName) != 0)
                key = type.TypeName;
            if (key.empty() && type.IsScopedEnum) key = type.TypeName;
            if (key.empty()) return false;
            if (!type.Pointer && !type.ElemPointer && CxxCflatEnumBackingSpelling(key, spelling))
                return true;
            if (auto it = cxxCflatToCxxSpelling_.find(key); it != cxxCflatToCxxSpelling_.end())
                spelling = it->second;
            else
            {
                spelling = key;
                for (size_t pos = 0; (pos = spelling.find('.', pos)) != std::string::npos; pos += 2)
                    spelling.replace(pos, 1, "::");
            }
            return true;
        };

        // A const C++ object argument binds its own wrapper, never the mutable one's name.
        bool constSpelled = false;
        // So does a literal's: its `const char[N]` is in the call, one wrapper per N.
        bool literalArrayArgument = false;
        std::vector<size_t> literalArrayParams;   // wrapper parameters holding such a literal
        /*
         * EVERY fitting overload (templates and non-templates) takes a `T *const &` there: the
         * literal crosses as that pointer without asking clang. A mixed set keeps the literal's
         * array, and RetargetCxxLiteralPointerTemporaries moves a bound temporary to the caller.
         */
        auto literalBindsPointerConstRef = [&](size_t index) {
            bool any = false;
            if (memberCall)
            {
                auto record = cxxRecordEntries_.find(memberOwnerType);
                if (record == cxxRecordEntries_.end() || index == 0) return false;
                const size_t userIndex = index - 1;
                for (const auto& member : record->second.members)
                {
                    if (member.name != memberName
                        || member.kind != cflat_cinterop::RawCxxMember::Instance
                        || member.paramTypes.size() + 1 != arguments.size()
                        || userIndex >= member.paramTypes.size())
                        continue;
                    if (!IsCxxConstRefToPointerSpelling(member.paramTypes[userIndex])) return false;
                    any = true;
                }
                return any;
            }
            if (auto templates = cxxFunctionTemplates_.find(functionName);
                templates != cxxFunctionTemplates_.end())
                for (const auto& candidate : templates->second)
                {
                    if (candidate.kind != cflat_cinterop::RawFunctionTemplate::Free
                        || arguments.size() < candidate.minArity
                        || arguments.size() > candidate.maxArity
                        || candidate.parameterTypes.empty())
                        continue;
                    const std::string& type = candidate.parameterTypes[
                        std::min(index, candidate.parameterTypes.size() - 1)];
                    if (!IsCxxConstRefToPointerSpelling(type)) return false;
                    any = true;
                }
            if (auto overloads = functionTable.find(functionName); overloads != functionTable.end())
                for (const auto& candidate : overloads->second)
                    if (candidate.IsCxx && !candidate.UniqueName.starts_with("__cflat_")
                        && candidate.Parameters.size() == arguments.size())
                        any = true;
            return any && CxxNonTemplateOverloadsTakePointerConstRef(functionName, {},
                                                                     arguments.size(), index);
        };
        std::vector<std::string> parameterSpellings;
        std::vector<std::string> callArguments;
        std::vector<size_t> functionPointerCallerSlots;
        std::vector<std::pair<size_t, TypeAndValue>> constructionFunctionPointerSlots;
        parameterSpellings.reserve(arguments.size());
        callArguments.reserve(arguments.size());
        // (call argument index, C++ literal spelling) for the literal retry below.
        std::vector<std::pair<size_t, std::string>> literalCallArguments;
        bool hasDeferredMoveLeaf = false;
        // T518: argument `index` must bind a parameter of class `spelling` (by value or reference).
        struct DeferredPlaceGuard { size_t index; std::string spelling; std::string cflatType; };
        std::vector<DeferredPlaceGuard> deferredPlaceGuards;
        size_t currentArgumentIndex = 0;
        auto appendDirectConstruction = [&](NamedVariable& construction,
                                             auto&& self,
                                             std::string& expression) -> bool {
            if (!construction.IsCxxDirectConstruction) return false;
            /*
             * T418: a deferred `?:` argument: the selector is an int, each place leaf the CALLER's
             * object (lvalue), and the wrapper spells the conditional so clang types and elides it.
             */
            if (!construction.CxxDeferredTernaryShape.empty())
            {
                std::string defaultSpelling;
                if (!CxxSpellingForCflatType(construction.CxxDirectConstructionType, defaultSpelling))
                    return false;
                std::vector<std::string> childNames;
                for (size_t c = 0; c < construction.CxxDirectConstructionArguments.size(); ++c)
                {
                    const NamedVariable& child = construction.CxxDirectConstructionArguments[c];
                    std::string spelling = "int";
                    if (c != 0 && IsCxxFunctionDesignatorArgument(child))
                    {
                        // T518: a call arm's CFlat function, called only in its selected arm.
                        if (!CxxThinFunctionPointerSpelling(child.TypeAndValue, spelling)) return false;
                    }
                    else if (c != 0)
                    {
                        if (!CxxSpellingForCflatType(child.TypeAndValue.TypeName, spelling)) return false;
                        // A const local stays const: `CV(const CV&)` over `CV(CV&)`, and a distinct wrapper key.
                        spelling = (child.TypeAndValue.IsConstObject() ? "const " : "") + spelling + " &";
                    }
                    childNames.push_back("p" + std::to_string(parameterSpellings.size()));
                    parameterSpellings.push_back(std::move(spelling));
                }
                const std::string& shape = construction.CxxDeferredTernaryShape;
                expression.clear();
                for (size_t at = 0; at < shape.size(); )
                {
                    if (shape[at] != '@') { expression += shape[at++]; continue; }
                    ++at;
                    if (at < shape.size() && shape[at] == 'S') { expression += childNames.front(); ++at; continue; }
                    if (at < shape.size() && shape[at] == 'D') { expression += defaultSpelling + "()"; ++at; continue; }
                    // T518: the callee is unbound, so clang must prove the parameter is `default`'s class.
                    if (at < shape.size() && shape[at] == 'G')
                    {
                        deferredPlaceGuards.push_back({ currentArgumentIndex, defaultSpelling,
                            construction.CxxDirectConstructionType });
                        ++at;
                        continue;
                    }
                    const bool callLeaf = at < shape.size() && shape[at] == 'F';
                    if (callLeaf) ++at;
                    const bool movedLeaf = at < shape.size() && shape[at] == 'M';
                    if (movedLeaf) ++at;
                    size_t end = at;
                    while (end < shape.size() && std::isdigit(static_cast<unsigned char>(shape[end]))) ++end;
                    const size_t child = static_cast<size_t>(std::stoul(shape.substr(at, end - at)));
                    if (child >= childNames.size()) return false;
                    if (callLeaf)
                    {
                        if (movedLeaf) return false;
                        expression += childNames[child] + "()";
                        at = end;
                        continue;
                    }
                    if (movedLeaf)
                    {
                        if (construction.CxxDirectConstructionArguments[child].Storage == nullptr)
                            return false;
                        expression += "std::move(" + childNames[child] + ")";
                        hasDeferredMoveLeaf = true;
                    }
                    else expression += childNames[child];
                    at = end;
                }
                return true;
            }
            if (const std::string refusal = CxxConstructorRvalueRefBindRefusal(
                    construction.CxxDirectConstructionType,
                    construction.CxxDirectConstructionArguments);
                !refusal.empty())
            {
                error = refusal;
                return false;
            }
            std::string typeSpelling;
            if (!CxxSpellingForCflatType(construction.CxxDirectConstructionType, typeSpelling))
                return false;
            std::vector<std::string> constructorArguments;
            auto hasExactReferenceParameter = [&](size_t index,
                                                   const std::string& spelling,
                                                   bool functionPointer = false,
                                                   bool rvalueReference = false) {
                auto record = cxxRecordEntries_.find(construction.CxxDirectConstructionType);
                if (record == cxxRecordEntries_.end()) return false;
                for (const auto& member : record->second.members)
                {
                    if (member.kind != cflat_cinterop::RawCxxMember::Constructor
                        || member.paramTypes.size()
                            < construction.CxxDirectConstructionArguments.size() + 1)
                        continue;
                    bool fits = true;
                    for (size_t n = construction.CxxDirectConstructionArguments.size() + 1;
                         n < member.paramTypes.size(); ++n)
                        if ((n >= member.defaultArgs.size() || member.defaultArgs[n].kind.empty())
                            && !(n + 1 == member.paramTypes.size()
                                && member.paramTypes[n].ends_with("...")))
                        { fits = false; break; }
                    if (!fits || index + 1 >= member.paramTypes.size()) continue;
                    const std::string& parameter = member.paramTypes[index + 1];
                    const bool isRvalueReference = parameter.ends_with("&&");
                    const bool isLvalueReference = parameter.ends_with("&") && !isRvalueReference;
                    const std::string referent = functionPointer
                        ? CxxFunctionPointerReferentParameter(parameter)
                        : (rvalueReference ? isRvalueReference : isLvalueReference)
                            ? StripCxxRefAndCv(parameter) : std::string();
                    if (!referent.empty() && SqueezeCxxSpelling(referent)
                        == SqueezeCxxSpelling(spelling)) return true;
                }
                return false;
            };
            auto hasSelectedExactReferenceParameter = [&](size_t index,
                                                          const std::string& spelling,
                                                          bool functionPointer = false,
                                                          bool rvalueReference = false) {
                auto fallback = [&] {
                    bool selectedReference = false;
                    if (!functionPointer
                        && CxxConstructorThunkParameterIsReference(
                            construction.CxxDirectConstructionType,
                            construction.CxxDirectConstructionArguments,
                            index, rvalueReference, selectedReference))
                        return selectedReference;
                    return hasExactReferenceParameter(index, spelling, functionPointer,
                                                      rvalueReference);
                };
                const auto* classInfo = GetCxxClassInfoWithConstructors(
                    construction.CxxDirectConstructionType);
                auto record = cxxRecordEntries_.find(construction.CxxDirectConstructionType);
                if (classInfo == nullptr || classInfo->hasCtorTemplate
                    || record == cxxRecordEntries_.end() || !record->second.ctorTemplates.empty())
                    return fallback();
                for (const auto& member : record->second.members)
                    if (member.kind == cflat_cinterop::RawCxxMember::Constructor
                        && (member.variadic || member.requiresConstructorWrapper))
                        return fallback();
                std::vector<TypeAndValue> argumentTypes;
                std::vector<NamedVariable> argumentVars;
                for (const NamedVariable& argument : construction.CxxDirectConstructionArguments)
                {
                    TypeAndValue type = argumentType(argument);
                    if (IsCxxFunctionDesignatorArgument(argument))
                        if (auto* fn = llvm::dyn_cast_or_null<llvm::Function>(
                                argument.Primary->stripPointerCasts()))
                            if (const FunctionSymbol* symbol = FindSymbolForFunction(fn))
                                type = FuncPtrSigOfSymbol(*symbol);
                    NamedVariable selectedArgument = argument;
                    selectedArgument.TypeAndValue = type;
                    argumentTypes.push_back(type);
                    argumentVars.push_back(std::move(selectedArgument));
                }
                std::string why;
                const auto* selectedCtor = SelectCxxConstructorListed(
                    construction.CxxDirectConstructionType, argumentTypes, why,
                    /*allowNumericConversions*/ true, &argumentVars, /*allowExplicit*/ true);
                if (selectedCtor == nullptr && !functionPointer)
                {
                    bool selectedReference = false;
                    if (CxxConstructorThunkParameterIsReference(
                            construction.CxxDirectConstructionType,
                            construction.CxxDirectConstructionArguments,
                            index, rvalueReference, selectedReference, /*forceClangPick*/ true))
                        return selectedReference;
                    return hasExactReferenceParameter(index, spelling, functionPointer,
                                                      rvalueReference);
                }
                if (selectedCtor == nullptr || index + 1 >= selectedCtor->params.size())
                    return fallback();
                const TypeAndValue& selectedParam = selectedCtor->params[index + 1];
                const bool selectedReference = rvalueReference
                    ? selectedParam.IsRvalueRef
                    : selectedParam.IsAlias || selectedParam.CxxConstRef()
                        || selectedParam.IsRvalueRef;
                if (!selectedReference)
                    return false;
                for (const auto& member : record->second.members)
                {
                    if (member.kind != cflat_cinterop::RawCxxMember::Constructor
                        || member.linkageName != selectedCtor->linkageName
                        || index + 1 >= member.paramTypes.size())
                        continue;
                    const std::string& parameter = member.paramTypes[index + 1];
                    const bool isRvalueReference = parameter.ends_with("&&");
                    const bool isLvalueReference = parameter.ends_with("&") && !isRvalueReference;
                    const std::string referent = functionPointer
                        ? CxxFunctionPointerReferentParameter(parameter)
                        : (rvalueReference ? isRvalueReference : isLvalueReference)
                            ? StripCxxRefAndCv(parameter) : std::string();
                    return !referent.empty() && SqueezeCxxSpelling(referent)
                        == SqueezeCxxSpelling(spelling);
                }
                return fallback();
            };
            for (size_t ctorArgIndex = 0;
                 ctorArgIndex < construction.CxxDirectConstructionArguments.size();
                 ++ctorArgIndex)
            {
                NamedVariable& argument =
                    construction.CxxDirectConstructionArguments[ctorArgIndex];
                std::string child;
                if (argument.IsCxxDirectConstruction)
                {
                    if (!self(argument, self, child)) return false;
                }
                else
                {
                    if (const std::string* unnamedEnum = CxxUnnamedEnumExpression(argument.Storage);
                        unnamedEnum != nullptr)
                    {
                        const std::string parameter = "p"
                            + std::to_string(parameterSpellings.size());
                        parameterSpellings.push_back("decltype(" + *unnamedEnum + ")");
                        constructorArguments.push_back(parameter);
                        continue;
                    }
                    if (IsCxxNullTypedArgument(argument))
                    {
                        parameterSpellings.push_back("std::nullptr_t");
                        constructorArguments.push_back("nullptr");
                        continue;
                    }
                    TypeAndValue childType = argumentType(argument);
                    if (IsCxxFunctionDesignatorArgument(argument))
                        if (auto* fn = llvm::dyn_cast<llvm::Function>(
                                argument.Primary->stripPointerCasts()))
                            if (const FunctionSymbol* symbol = FindSymbolForFunction(fn))
                                childType = FuncPtrSigOfSymbol(*symbol);
                    std::string spelling;
                    if (!argument.CxxClosureSignature.empty())
                    {
                        const std::string parameter = "p"
                            + std::to_string(parameterSpellings.size());
                        parameterSpellings.push_back("void *");
                        child = CxxClosureCallArgument(argument, parameter);
                    }
                    else if (CxxThinFunctionPointerSpelling(childType, spelling))
                    {
                        const std::string parameter = "p"
                            + std::to_string(parameterSpellings.size());
                        if (IsCxxFunctionDesignatorArgument(argument))
                        {
                            parameterSpellings.push_back(spelling);
                            child = "(*" + parameter + ")";
                        }
                        else if (IsProvenCxxScalarLvalue(argument, true)
                                 && argument.Storage != nullptr)
                        {
                            // Keep a named function pointer an lvalue so clang ranks every ctor
                            // against its source category, including by-value and bool overloads.
                            parameterSpellings.push_back("void *");
                            constructionFunctionPointerSlots.emplace_back(
                                parameterSpellings.size() - 1, childType);
                            argument.Primary = argument.Storage;
                            const std::string cv = CxxConstReceiverKind(argument) != 0
                                ? " const" : "";
                            child = "*static_cast<" + spelling + cv + " *>(" + parameter + ")";
                        }
                        else if (IsCxxRvalueReferenceArgument(argument)
                                 && argument.Storage != nullptr)
                        {
                            // Keep xvalue category and top-level const for clang's overload set.
                            parameterSpellings.push_back("void *");
                            constructionFunctionPointerSlots.emplace_back(
                                parameterSpellings.size() - 1, childType);
                            argument.Primary = argument.Storage;
                            const std::string cv = CxxConstReceiverKind(argument) != 0
                                ? " const" : "";
                            child = "static_cast<" + spelling + cv + " &&>(*static_cast<"
                                + spelling + cv + " *>(" + parameter + "))";
                        }
                        else
                        {
                            // By value: the snapshot the wrapper took is the argument.
                            parameterSpellings.push_back(spelling);
                            child = "static_cast<" + spelling + " &&>(" + parameter + ")";
                        }
                    }
                    else
                    {
                        if (!enumSpelling(childType, spelling)
                            && !CxxSpellingForCflatType(cflatTypeOf(argument), spelling))
                            return false;
                        const bool classValue = !childType.Pointer
                            && IsCxxRecord(childType.TypeName) && !childType.IsInterface;
                        const bool lvalue = classValue ? !IsCxxRvalueReferenceArgument(argument)
                            : IsProvenCxxScalarLvalue(argument);
                        const bool constObject = CxxConstReceiverKind(argument) != 0;
                        std::string parameterSpelling = constObject ? "const " + spelling : spelling;
                        if (classValue) parameterSpelling += lvalue ? " &" : " &&";
                        else if (lvalue && hasSelectedExactReferenceParameter(ctorArgIndex, spelling))
                            parameterSpelling += " &";
                        const std::string parameter = "p"
                            + std::to_string(parameterSpellings.size());
                        parameterSpellings.push_back(std::move(parameterSpelling));
                        child = lvalue ? parameter
                            : "static_cast<" + spelling + " &&>(" + parameter + ")";
                    }
                }
                constructorArguments.push_back(std::move(child));
            }
            if (!error.empty()) return false;
            const char* open = construction.CxxDirectConstructionUsesBraces ? "({" : "(";
            const char* close = construction.CxxDirectConstructionUsesBraces ? "})" : ")";
            expression = typeSpelling + open;
            for (size_t i = 0; i < constructorArguments.size(); ++i)
                expression += (i == 0 ? "" : ", ") + constructorArguments[i];
            expression += close;
            return true;
        };
        for (size_t i = 0; i < arguments.size(); ++i)
        {
            NamedVariable& arg = arguments[i];
            currentArgumentIndex = i;
            const size_t parameterIndex = parameterSpellings.size();
            TypeAndValue type = argumentType(arg);
            auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(arg.Primary);
            const bool stringLiteral = constant != nullptr && IsStringLiteralConstant(constant);
            // Only a literal is `const char *`; a char* VALUE stays writable, as C++ sees it
            // (to_chars(first, last, v), format_to_n(out, n, ...) write through it).
            // At `const char *const &` the decayed pointer lives in the caller (as the template path).
            if (stringLiteral && literalBindsPointerConstRef(i))
            {
                parameterSpellings.push_back("const char *const &");
                callArguments.push_back("p" + std::to_string(parameterIndex));
                literalArrayArgument = true;
                continue;
            }
            // A literal is its own `const char[N]` lvalue (see CxxStringLiteralArrayLvalue).
            if (std::string literalArray = stringLiteral
                    ? CxxStringLiteralArrayLvalue(arg, i) : std::string();
                !literalArray.empty())
            {
                literalArrayParams.push_back(parameterIndex);
                parameterSpellings.push_back("const char *");
                callArguments.push_back(std::move(literalArray));
                literalArrayArgument = true;
                if (std::string literal; stringLiteral && CxxStringLiteralSpelling(arg, literal))
                    literalCallArguments.emplace_back(i, std::move(literal));
                continue;
            }
            const std::string cflatType = cflatTypeOf(arg);
            if (type.TypeName.empty())
            {
                error = std::format("C++ free function '{}' has an argument with no C++ type", functionName);
                return false;
            }
            std::string spelling;
            if (!arg.CxxClosureSignature.empty())
            {
                // A CFlat closure: the wrapper builds the owning __cflat_closure<> prvalue.
                parameterSpellings.push_back("void *");
                arg.CxxClosureParam = "p" + std::to_string(parameterIndex);
                callArguments.push_back(CxxClosureCallArgument(arg, arg.CxxClosureParam));
                continue;
            }
            // The null type needs no header: C++ deduces std::nullptr_t from this spelling.
            if (type.TypeName == "std.nullptr_t" && !type.Pointer)
            {
                parameterSpellings.push_back("decltype(nullptr)");
                callArguments.push_back("p" + std::to_string(parameterIndex));
                continue;
            }
            if (const std::string* unnamedEnum = CxxUnnamedEnumExpression(arg.Storage);
                unnamedEnum != nullptr)
            {
                parameterSpellings.push_back("decltype(" + *unnamedEnum + ")");
                callArguments.push_back("p" + std::to_string(parameterIndex));
                continue;
            }
            if (CxxThinFunctionPointerSpelling(type, spelling))
            {
                if (IsCxxFunctionDesignatorArgument(arg))
                {
                    parameterSpellings.push_back(spelling);
                    callArguments.push_back("(*p" + std::to_string(parameterIndex) + ")");
                }
                else
                {
                    parameterSpellings.push_back("void *");
                    llvm::Type* pointerType = arg.BaseType != nullptr
                        ? arg.BaseType : GetType(arg.TypeAndValue);
                    if (pointerType == nullptr || !pointerType->isPointerTy())
                    {
                        error = std::format("C++ free function '{}' has a function-pointer argument "
                                            "without a pointer type", functionName);
                        return false;
                    }
                    llvm::Value* slot = arg.Storage;
                    if (slot == nullptr)
                    {
                        slot = AllocaAtEntry(pointerType, nullptr, "cxx.fptr.forward");
                        llvm::Value* value = arg.Primary != nullptr ? arg.Primary : LoadArgStorage(arg);
                        builder->CreateStore(value, slot);
                        arg.Storage = slot;
                    }
                    arg.Primary = slot;
                    functionPointerCallerSlots.push_back(parameterIndex);
                    const std::string referent = "*static_cast<" + spelling + " *>(p"
                        + std::to_string(parameterIndex) + ")";
                    // `move f` keeps its caller slot here, so the xvalue is spelled from the move.
                    callArguments.push_back(arg.IsRvalue || arg.IsExplicitMove
                        ? "static_cast<" + spelling + " &&>(" + referent + ")" : referent);
                }
                // A `const T*` callback parameter is invisible to CFlat's signature too.
                constSpelled = constSpelled || spelling.find("const ") != std::string::npos;
                continue;
            }
            if (!enumSpelling(type, spelling) && !CxxSpellingForCflatType(cflatType, spelling))
            {
                error = std::format("C++ free function '{}' has an argument type that cannot be spelled in C++",
                                    functionName);
                return false;
            }
            if (IsCxxFixedArrayArgument(arg))
            {
                std::vector<uint64_t> dimensions;
                if (arg.TypeAndValue.ConstArraySize != 0)
                {
                    dimensions.push_back(arg.TypeAndValue.ConstArraySize);
                    dimensions.insert(dimensions.end(), arg.TypeAndValue.ConstInnerDimensions.begin(),
                                      arg.TypeAndValue.ConstInnerDimensions.end());
                }
                else
                {
                    llvm::Type* arrayType = arg.BaseType;
                    while (auto* array = llvm::dyn_cast_or_null<llvm::ArrayType>(arrayType))
                    {
                        dimensions.push_back(array->getNumElements());
                        arrayType = array->getElementType();
                    }
                }
                if (dimensions.empty() || !spelling.ends_with("*"))
                {
                    error = std::format("C++ free function '{}' has a fixed-array argument that cannot be spelled",
                                        functionName);
                    return false;
                }
                spelling.pop_back();
                std::string parameter = spelling + " "
                    + std::string(kArrayReferenceParameterMarker);
                for (uint64_t dimension : dimensions)
                    parameter += std::format("[{}]", dimension);
                parameterSpellings.push_back(std::move(parameter));
                callArguments.push_back("p" + std::to_string(parameterIndex));
                continue;
            }
            if (arg.IsCxxDirectConstruction)
            {
                std::string construction;
                if (!appendDirectConstruction(arg, appendDirectConstruction, construction))
                {
                    if (error.empty())
                        error = std::format("C++ free function '{}' has a direct class construction "
                                            "argument that cannot be forwarded", functionName);
                    return false;
                }
                callArguments.push_back(std::move(construction));
                continue;
            }
            // An imported `const T*` (c_str()) keeps its const pointee, as in the template path.
            if (type.PointeeConstBit() && type.Pointer
                && !arg.TypeAndValue.IsFunctionPointer && spelling.ends_with("*")
                && !spelling.starts_with("const "))
            {
                spelling = "const " + spelling;
                // CFlat's signature drops this const: keep the wrapper off the shared name.
                constSpelled = constSpelled || IsCxxRecord(type.TypeName);
            }
            const bool classValue = !type.Pointer && IsCxxRecord(type.TypeName)
                && !type.IsInterface;
            if (classValue)
            {
                if (IsCxxRvalueReferenceArgument(arg))
                {
                    const bool constObject = CxxConstReceiverKind(arg) != 0;
                    constSpelled = constSpelled || constObject;
                    const std::string valueSpelling = constObject && !spelling.starts_with("const ")
                        ? "const " + spelling : spelling;
                    parameterSpellings.push_back(valueSpelling + " &&");
                    callArguments.push_back("static_cast<" + valueSpelling + " &&>(p"
                                            + std::to_string(parameterIndex) + ")");
                }
                else
                {
                    // A const C++ object lvalue binds as one: `std::move` of it is `const T&&`.
                    const bool constObject = !arg.IsRvalue && CxxConstReceiverKind(arg) != 0;
                    constSpelled = constSpelled || constObject;
                    parameterSpellings.push_back((constObject ? "const " : "") + spelling + " &");
                    callArguments.push_back("p" + std::to_string(parameterIndex));
                }
            }
            else
            {
                // `stream >> v` writes v: a scalar lvalue operand of an infix `>>` stays one.
                if (infix && infixOperator == ">>" && infixRhsName.empty() && i == infixRhsIndex
                    && !type.Pointer
                    && arg.Storage != nullptr && !arg.IsRvalue)
                    spelling += " &";
                /*
                 * The wrapper hands C++ the argument's own value category, as the class branch
                 * does: a PROVEN lvalue as the CALLER's object (so `std::swap(x, y)` swaps x and y,
                 * not wrapper copies), anything else as an xvalue a non-const `T&` refuses.
                 */
                const bool cxxRvalue = !IsProvenCxxScalarLvalue(arg);
                if (!cxxRvalue && !spelling.ends_with(" &")) spelling += " &";
                const bool xvalue = cxxRvalue && !spelling.ends_with(" &");
                const std::string parameter = "p" + std::to_string(parameterIndex);
                callArguments.push_back(xvalue
                    ? "static_cast<" + spelling + " &&>(" + parameter + ")" : parameter);
                /*
                 * The temporary lives in the CALLER's frame, as C++'s full-expression temporary
                 * does: a `const T&` result bound to it must not point into the wrapper frame.
                 * The argument says rvalue too, so the call materializes it instead of passing a
                 * source variable's slot (as RequestCxxFunctionTemplate does).
                 */
                if (xvalue)
                {
                    spelling += " &&";
                    arguments[i].IsRvalue = true;
                    arguments[i].Storage = nullptr;
                    arguments[i].CallerName.clear();
                }
                parameterSpellings.push_back(std::move(spelling));
            }
        }
        /*
         * A numeric LITERAL list element is written into the list as the literal itself: list
         * initialization accepts a constant that fits (`max<short>({3, 9})`) and refuses one that
         * narrows, which a forwarded parameter cannot express. The parameter stays, unused.
         */
        std::map<size_t, std::string> braceLiteralElements;
        for (const auto& [first, count] : braceGroups)
            for (size_t i = first; i < first + count && i < arguments.size(); ++i)
            {
                const NamedVariable& element = arguments[i];
                const std::string& identity = element.LiteralIdentity;
                // An imported enumerator or integer macro is a constant too: its typed constant text
                // goes in the list (clang applies the constant-expression rule to it).
                if (identity.empty() && element.CxxImportedLiteral.empty()) continue;
                const std::string forwarded = callArguments[i];
                // Recorded after the literal spelling below replaced the forwarded parameter.
                struct RecordLiteral
                {
                    std::map<size_t, std::string>& literals;
                    const std::vector<std::string>& calls;
                    const std::string& forwarded;
                    size_t index;
                    ~RecordLiteral()
                    {
                        if (calls[index] != forwarded) literals[index] = calls[index];
                    }
                } recordLiteral{ braceLiteralElements, callArguments, forwarded, i };
                if (identity.empty())
                {
                    callArguments[i] = element.CxxImportedLiteral;
                    continue;
                }
                if (auto* constant = llvm::dyn_cast_or_null<llvm::ConstantInt>(element.Primary))
                {
                    static const std::map<std::string, std::pair<std::string, unsigned>> suffixes = {
                        { "int", { "", 32 } }, { "long", { "L", 64 } }, { "i64", { "LL", 64 } },
                        { "uint", { "U", 32 } }, { "ulong", { "UL", 64 } }, { "u64", { "ULL", 64 } },
                    };
                    auto suffix = suffixes.find(identity);
                    if (suffix == suffixes.end()) continue;
                    const bool isUnsigned = identity.front() == 'u';
                    const llvm::APInt value = isUnsigned
                        ? constant->getValue().zextOrTrunc(suffix->second.second)
                        : constant->getValue().sextOrTrunc(suffix->second.second);
                    if (!isUnsigned && value.isMinSignedValue())
                        callArguments[i] = "(" + llvm::toString(value + 1, 10, true)
                            + suffix->second.first + " - 1)";
                    else
                        callArguments[i] = llvm::toString(value, 10, !isUnsigned)
                            + suffix->second.first;
                }
                else if (auto* real = llvm::dyn_cast_or_null<llvm::ConstantFP>(element.Primary);
                         real != nullptr && real->getValueAPF().isFinite()
                             && (identity == "double" || identity == "float"))
                {
                    llvm::APFloat value = real->getValueAPF();
                    bool lossy = false;
                    value.convert(llvm::APFloat::IEEEdouble(), llvm::APFloat::rmNearestTiesToEven,
                                  &lossy);
                    callArguments[i] = std::format("{:a}", value.convertToDouble())
                        + (identity == "float" ? "f" : "");
                    if (!callArguments[i].starts_with("-")) callArguments[i] = "0x" + callArguments[i];
                    else callArguments[i] = "-0x" + callArguments[i].substr(1);
                }
            }

        std::vector<std::string> cxxExplicitArgs;
        cxxExplicitArgs.reserve(explicitArgs.size());
        for (const std::string& typeArg : explicitArgs)
        {
            if (typeArg.starts_with("#"))
            {
                cxxExplicitArgs.push_back(typeArg.substr(1));
                continue;
            }
            std::string spelling;
            if (!CxxSpellingForCflatType(typeArg, spelling))
            {
                error = std::format("C++ free function '{}' has an explicit type argument that cannot be spelled in C++",
                                    functionName);
                return false;
            }
            cxxExplicitArgs.push_back(std::move(spelling));
        }

        // Each brace group (first expanded argument, element count) is written back as one list,
        // or, for a caller-backed wrapper, as the std::initializer_list over the caller's array.
        const std::vector<std::string>* backedLists = nullptr;
        auto groupBraceCalls = [&](const std::vector<std::string>& calls) {
            if (braceGroups.empty()) return calls;
            std::vector<std::string> grouped;
            size_t i = 0;
            size_t g = 0;
            for (const auto& [first, count] : braceGroups)  // ascending, in argument order
            {
                while (i < first && i < calls.size()) grouped.push_back(calls[i++]);
                std::string list = "{";
                for (size_t k = 0; k < count && i < calls.size(); ++k, ++i)
                    list += (k == 0 ? "" : ", ") + calls[i];
                grouped.push_back(backedLists != nullptr && g < backedLists->size()
                                      ? (*backedLists)[g] : list + "}");
                ++g;
            }
            while (i < calls.size()) grouped.push_back(calls[i++]);
            return grouped;
        };
        auto makeTarget = [&](const std::vector<std::string>& ungroupedCalls) {
            const std::vector<std::string> calls = groupBraceCalls(ungroupedCalls);
            if (memberCall)
            {
                std::string target = calls.empty() ? std::string("p0") : calls.front();
                target = arguments.front().TypeAndValue.Pointer
                    ? target + "->" + memberName
                    : "(" + target + ")." + memberName;
                target += "(";
                for (size_t i = 1; i < calls.size(); ++i)
                    target += (i == 1 ? "" : ", ") + calls[i];
                return target + ")";
            }
            // An ADL call is written unqualified so C++ searches the arguments' associated
            // namespaces and hidden friends, as a range-for's free begin/end lookup does.
            std::string target = adlCall ? functionName.substr(functionName.rfind('.') + 1)
                : functionMacro ? functionName   // `::assert(...)` would not expand the macro
                                : CxxNameFromCflat(functionName);
            if (infix)
                target = (infixLhsName.empty() ? calls[0] : CxxNameFromCflat(infixLhsName))
                    + " " + infixOperator + " "
                    + (infixRhsName.empty() ? calls[infixRhsIndex]
                                            : CxxNameFromCflat(infixRhsName));
            if (infix) {}
            else if (!cxxExplicitArgs.empty())
            {
                target += "<";
                for (size_t i = 0; i < cxxExplicitArgs.size(); ++i)
                {
                    if (i != 0) target += ", ";
                    target += cxxExplicitArgs[i];
                }
                target += ">";
            }
            if (!infix)
            {
                target += "(";
                for (size_t i = 0; i < calls.size(); ++i)
                {
                    if (i != 0) target += ", ";
                    target += calls[i];
                }
                target += ")";
            }
            return target;
        };

        std::vector<size_t> dependencyGroups;
        auto addDependencyGroup = [&](size_t group) {
            if (std::find(dependencyGroups.begin(), dependencyGroups.end(), group)
                == dependencyGroups.end())
                dependencyGroups.push_back(group);
        };
        auto collectTypeDependencies = [&](const std::string& type) {
            std::string base = type;
            while (!base.empty() && base.back() == '*') base.pop_back();
            if (auto owner = cxxTypeOwnerGroup_.find(base);
                owner != cxxTypeOwnerGroup_.end()
                && owner->second < cxxImportGroups_.size()
                && !cxxImportGroups_[owner->second].headers.empty())
                addDependencyGroup(owner->second);
        };
        auto collectConstructionDependencies = [&](auto&& self,
                                                    const NamedVariable& construction) -> void {
            if (!construction.IsCxxDirectConstruction) return;
            collectTypeDependencies(construction.CxxDirectConstructionType);
            for (const NamedVariable& child : construction.CxxDirectConstructionArguments)
            {
                collectTypeDependencies(cflatTypeOf(child));
                self(self, child);
            }
        };
        for (const auto& argument : arguments)
        {
            collectTypeDependencies(cflatTypeOf(argument));
            collectConstructionDependencies(collectConstructionDependencies, argument);
        }
        for (const std::string& explicitArg : explicitArgs)
            if (!explicitArg.starts_with("#")) collectTypeDependencies(explicitArg);

        // T518: the place guard re-forms a plain free call only; other call forms refuse.
        std::string placeGuardError;
        if (!deferredPlaceGuards.empty()
            && (memberCall || infix || adlCall || !braceGroups.empty() || !explicitArgs.empty()))
        {
            error = std::format("a '?:' argument of C++ '{}' cannot be checked against its parameter "
                                "type in this call form", functionName);
            return false;
        }
        const std::string cxxBase = [&] {
            if (memberCall)
                return CxxNameFromCflat(memberOwnerType) + "::" + memberName;
            std::string result = functionName;
            for (size_t pos = 0; (pos = result.find('.', pos)) != std::string::npos; pos += 2)
                result.replace(pos, 1, "::");
            return result;
        }();
        auto namedParameter = [&](std::string spelling, const std::string& name) {
            const size_t marker = spelling.find(kArrayReferenceParameterMarker);
            if (marker != std::string::npos)
            {
                spelling.replace(marker, kArrayReferenceParameterMarker.size(),
                                 "(&" + name + ")");
                return spelling;
            }
            return spelling + " " + name;
        };
        struct FreeWrapper
        {
            std::string name;
            std::string bindName;
            std::string source;
            std::vector<std::string> calls;
        };
        auto makeWrapper = [&](const std::vector<std::string>& calls, bool literal) {
            const std::string target = makeTarget(calls);
            std::string guardKey;
            for (const DeferredPlaceGuard& guard : deferredPlaceGuards)
                guardKey += std::format("|place{}{}", guard.index, guard.spelling);
            const uint64_t hash = HashWrapperKey(functionName + target + guardKey
                                                 + std::format("|{}", parameterSpellings.size())
                                                 + (braceGroups.empty() ? "" : "|list-probe")
                                                 + [&] {
                                                       std::string result;
                                                       for (const auto& p : parameterSpellings) result += "|" + p;
                                                       for (const auto& a : cxxExplicitArgs) result += "|" + a;
                                                       return result;
            }());
            FreeWrapper wrapper;
            wrapper.calls = calls;
            wrapper.name = std::format("__cflat_free_{:016x}", hash);
            // Literal, infix, and explicit wrappers register under their unique wrapper name. So
            // does an enum's value-construction (`std.errc()`): the enum name must stay a type.
            wrapper.bindName = (infix || adlCall || !explicitArgs.empty() || literal
                                || constSpelled || literalArrayArgument
                                || !functionPointerCallerSlots.empty()
                                || !ResolveEnumTypeName(functionName).empty()
                                || !braceGroups.empty() || hasDirectConstructionArgument)
                ? wrapper.name : functionName;
            std::string parameters;
            for (size_t i = 0; i < parameterSpellings.size(); ++i)
            {
                if (i != 0) parameters += ", ";
                parameters += namedParameter(parameterSpellings[i], "p" + std::to_string(i));
            }
            std::string call = target;
            if (adlCall)
            {
                /*
                 * ADL ONLY ([stmt.ranged]): the call is made inside a namespace whose deleted
                 * zero-argument overload ends ordinary unqualified lookup there, so a global or
                 * non-associated function of that name is never a candidate.
                 */
                const std::string scope = wrapper.name + "_adl";
                std::string forward;
                // Each parameter is forwarded as its declared type: an rvalue-reference one binds a literal.
                for (size_t i = 0; i < parameterSpellings.size(); ++i)
                {
                    const std::string pi = "p" + std::to_string(i);
                    forward += (i != 0 ? ", " : "") + std::string("static_cast<decltype(") + pi
                        + ") &&>(" + pi + ")";
                }
                // An unqualified name is an ordinary call plus ADL: the global overloads stay visible.
                const std::string hideGlobal = functionName.find('.') == std::string::npos
                    ? std::string() : "void " + target.substr(0, target.find('(')) + "() = delete; ";
                wrapper.source = "namespace " + scope + " { " + hideGlobal
                    + "inline decltype(auto) call(" + parameters + ") noexcept(noexcept("
                    + target + ")) { return " + target + "; } }\n";
                call = scope + "::call(" + forward + ")";
            }
            /*
             * List narrowing is a warning-as-error that the wrappers' -Wno-everything silences, but
             * in a SFINAE context it is a substitution failure. A declaration-only probe template
             * re-forms the call with every element dependent (literals through a dependent comma)
             * and the wrapper names it after its return, so a narrowing list does not compile.
             */
            std::string narrowingCheck;
            if (!braceGroups.empty())
            {
                std::vector<std::string> probeCalls = calls;
                for (const auto& [index, literalSpelling] : braceLiteralElements)
                    if (index < probeCalls.size())
                        probeCalls[index] = "(void(Z()), " + literalSpelling + ")";
                const std::string probe = wrapper.name + "_list_narrowing";
                std::string probeParameters = "Z";
                std::string probeArguments = "0";
                std::string templateParameters = "class Z";
                for (size_t i = 0; i < parameterSpellings.size(); ++i)
                {
                    const std::string index = std::to_string(i);
                    templateParameters += ", class A" + index;
                    probeParameters += ", A" + index + " && p" + index;
                    probeArguments += ", static_cast<decltype(p" + index + ") &&>(p" + index + ")";
                }
                wrapper.source += "template <" + templateParameters + "> auto " + probe + "("
                    + probeParameters + ") -> decltype(" + makeTarget(probeCalls) + ");\n";
                narrowingCheck = " using " + probe + "_t = decltype(" + probe + "(" + probeArguments
                    + "));";
            }
            /*
             * T518: an unbound callee's parameter is only known here. The probe re-forms the call with
             * the `?:` replaced by a proxy whose only conversion yields a P prvalue: the overload clang
             * selects (templates and default arguments included) must take P there - by value or by a
             * reference a prvalue binds. A parameter of another class would need a second user-defined
             * conversion, so the probe, and the wrapper with it, does not compile.
             */
            std::string placeCheck;
            for (const DeferredPlaceGuard& guard : deferredPlaceGuards)
            {
                if (guard.index >= calls.size()) continue;
                const std::string probe = wrapper.name + "_place" + std::to_string(guard.index);
                std::vector<std::string> probeCalls = calls;
                probeCalls[guard.index] = "Z()";
                std::string templateParameters = "class Z";
                std::string probeParameters = "Z";
                std::string probeArguments = probe + "_proxy()";
                for (size_t i = 0; i < parameterSpellings.size(); ++i)
                {
                    const std::string index = std::to_string(i);
                    templateParameters += ", class A" + index;
                    probeParameters += ", A" + index + " && p" + index;
                    probeArguments += ", static_cast<decltype(p" + index + ") &&>(p" + index + ")";
                }
                wrapper.source += "struct " + probe + "_proxy { operator " + guard.spelling + "() const; };\n";
                wrapper.source += "template <" + templateParameters + "> auto " + probe + "("
                    + probeParameters + ") -> decltype(" + makeTarget(probeCalls) + ");\n";
                placeCheck += " using " + probe + "_t = decltype(" + probe + "(" + probeArguments + "));";
            }
            wrapper.source += "extern \"C\" __attribute__((weak)) decltype(auto) " + wrapper.name + "("
                + parameters + ") noexcept(noexcept(" + call + ")) { return " + call + ";"
                + narrowingCheck + placeCheck + " }\n";
            if (hasDeferredMoveLeaf) wrapper.source.insert(0, "#include <utility>\n");
            return wrapper;
        };
        /*
         * Re-binds a brace-list call whose first wrapper's result can carry an address over
         * CALLER-frame list storage (C++ keeps the list to the end of the full-expression). The
         * second wrapper takes one `E *` per list after the forwarded parameters, constructs the
         * elements there, and passes an initializer_list over them; the caller allocates each
         * `E[N]` and destroys its elements as statement temporaries. 1 bound, 0 not applicable
         * (the caller refuses), -1 clang refused the second wrapper (backedError says why).
         */
        auto bindBraceBacked = [&](const CxxRequestGroup& group, const CSigEntry& first,
                                   const FreeWrapper& firstWrapper, std::string& backedError,
                                   bool onlyNontrivial) -> int {
            // Each list must reach the callee directly as its std::initializer_list argument (an
            // aggregate, array, or converted container temporary would still die in the wrapper).
            std::vector<std::string> elements;
            size_t grouped = 0;
            for (const auto& [firstIndex, count] : braceGroups)
            {
                const size_t argument = firstIndex - grouped;
                grouped += count - 1;
                if (argument >= first.braceListElementSpellings.size()
                    || first.braceListElementSpellings[argument].empty())
                    return 0;
                elements.push_back(first.braceListElementSpellings[argument]);
            }
            if (infix || adlCall || builder == nullptr || builder->GetInsertBlock() == nullptr)
                return 0;
            std::vector<TypeAndValue> elementTypes(elements.size());
            std::vector<llvm::Type*> elementTys(elements.size(), nullptr);
            for (size_t g = 0; g < elements.size(); ++g)
            {
                if (!MapCTypeToTypeAndValue(elements[g], elementTypes[g], true)) return 0;
                elementTys[g] = GetType(elementTypes[g]);
                if (elementTys[g] == nullptr || !elementTys[g]->isSized()) return 0;
            }
            if (onlyNontrivial
                && std::none_of(elementTypes.begin(), elementTypes.end(), [&](const TypeAndValue& e) {
                       return !e.Pointer && IsForeignNontrivialCxxClass(e.TypeName);
                   }))
                return 0;
            const std::string name = firstWrapper.name + "_backed";
            std::string typedefs = "#include <initializer_list>\n#include <new>\n";
            std::string parameters;
            for (size_t i = 0; i < parameterSpellings.size(); ++i)
                parameters += (i != 0 ? ", " : "")
                    + namedParameter(parameterSpellings[i], "p" + std::to_string(i));
            std::string body;
            std::vector<std::string> lists;
            for (size_t g = 0; g < braceGroups.size(); ++g)
            {
                const auto& [firstIndex, count] = braceGroups[g];
                const std::string element = name + "_e" + std::to_string(g);
                const std::string storage = "s" + std::to_string(g);
                const std::string il = "__cflat_list_" + std::to_string(g);
                const std::string listType = "std::initializer_list<" + element + ">";
                typedefs += "typedef " + elements[g] + " " + element + ";\n";
                parameters += (parameters.empty() ? "" : ", ") + element + " * " + storage;
                for (size_t k = 0; k < count && firstIndex + k < firstWrapper.calls.size(); ++k)
                    body += "::new ((void*)(" + storage + " + " + std::to_string(k) + ")) "
                        + element + "(" + firstWrapper.calls[firstIndex + k] + "); ";
                body += CxxInitializerListOverStorage(listType, il, storage, count);
                lists.push_back(listType + "(" + il + ")");
            }
            backedLists = &lists;
            const std::string target = makeTarget(firstWrapper.calls);
            backedLists = nullptr;
            const std::string source = typedefs + "extern \"C\" __attribute__((weak)) decltype(auto) "
                + name + "(" + parameters + ") noexcept(noexcept(" + makeTarget(firstWrapper.calls)
                + ")) { " + body + " return " + target + "; }\n";
            CSigEntry backed;
            if (!RequestGeneratedCxxWrapper(group, source, name, "FREE", backed, backedError))
            {
                backedError = FirstCxxErrorLine(backedError);
                return -1;
            }
            const size_t storageBase = parameterSpellings.size();
            if (backed.params.size() != storageBase + braceGroups.size())
            {
                backedError = "the caller-backed wrapper has an unexpected signature";
                return -1;
            }
            auto known = [&] {
                if (auto fit = functionTable.find(name); fit != functionTable.end())
                    for (const auto& symbol : fit->second)
                        if (symbol.External && symbol.UniqueName == name) return true;
                return false;
            };
            if (!known())
            {
                backed.name = name;
                RegisterCSignatures({ backed }, group.headers.front());
                if (!known())
                {
                    backedError = "the caller-backed wrapper could not be registered";
                    return -1;
                }
            }
            for (size_t g = 0; g < braceGroups.size(); ++g)
            {
                const size_t count = braceGroups[g].second;
                const uint64_t align = std::max<uint64_t>(
                    GetEffectiveAlignmentForType(elementTypes[g].TypeName, elementTys[g]),
                    module->getDataLayout().getABITypeAlign(elementTys[g]).value());
                auto* arrayTy = llvm::ArrayType::get(elementTys[g], count);
                auto* array = AllocaAtEntry(arrayTy, nullptr, "cxx.brace.list", align);
                if (!elementTypes[g].Pointer && IsForeignNontrivialCxxClass(elementTypes[g].TypeName))
                    RegisterOwnedBraceListTemp(array, elementTypes[g].TypeName, count);
                auto* dataPtr = builder->CreateInBoundsGEP(
                    arrayTy, array, { builder->getInt64(0), builder->getInt64(0) },
                    "cxx.brace.list.data");
                NamedVariable data;
                data.TypeAndValue = backed.params[storageBase + g];
                data.TypeAndValue.VariableName.clear();
                data.Primary = dataPtr;
                data.BaseType = dataPtr->getType();
                data.Storage = nullptr;
                data.IsRvalue = true;
                arguments.push_back(std::move(data));
            }
            registeredName = name;
            return 1;
        };
        /*
         * A string literal is forwarded as `const char *` first; only when clang refuses that
         * does a second wrapper spell the literal itself (see RequestCxxFunctionTemplate).
         */
        std::vector<FreeWrapper> wrappers{ makeWrapper(callArguments, false) };
        if (!literalCallArguments.empty())
        {
            std::vector<std::string> literalCalls = callArguments;
            for (const auto& [index, spelling] : literalCallArguments) literalCalls[index] = spelling;
            wrappers.push_back(makeWrapper(literalCalls, true));
        }
        for (const FreeWrapper& wrapper : wrappers)
            if (auto fit = functionTable.find(wrapper.bindName); fit != functionTable.end())
                for (const auto& symbol : fit->second)
                    if (symbol.External && symbol.UniqueName == wrapper.name)
                    {
                        registeredName = wrapper.bindName;
                        lastCxxFreeWrapperLinkage_ = wrapper.name;
                        return true;
                    }

        std::string lastError;
        int lastErrorScopeScore = -1;
        /*
         * Rank a rejected probe by the deepest path segment clang names as missing (or, for a
         * non-leaf, as not a scope): a group that resolved the scope outranks every scope miss,
         * and one that found the leaf outranks a leaf miss. Ties: first-segment ownership.
         */
        auto cxxProbeScopeScore = [&](size_t primary, const std::string& probeError) {
            std::vector<std::string> path;
            for (size_t start = 0;;)
            {
                const size_t sep = cxxBase.find("::", start);
                path.push_back(cxxBase.substr(start, sep == std::string::npos
                    ? std::string::npos : sep - start));
                if (sep == std::string::npos) break;
                start = sep + 2;
            }
            const bool ownsLead = primary < cxxImportGroups_.size()
                && cxxImportGroups_[primary].namespaces.count(path.front()) != 0;
            const std::string diagnostic = FirstCxxErrorLine(probeError);
            int missDepth = -1;
            std::string qualifiedScope;
            for (size_t i = 0; i < path.size(); ++i)
            {
                const std::string& segment = path[i];
                const bool namesScope = i == 0
                    ? diagnostic.find("in the global namespace") != std::string::npos
                        || diagnostic.find(" in ") == std::string::npos
                    : diagnostic.find("in namespace '" + qualifiedScope + "'") != std::string::npos
                        || diagnostic.find("in '" + qualifiedScope + "'") != std::string::npos;
                if ((diagnostic.find("no member named '" + segment + "'") != std::string::npos
                        && namesScope)
                    || (i + 1 < path.size() && diagnostic.find("'" + segment
                            + "' is not a class, namespace, or enumeration") != std::string::npos)
                    || (i == 0 && diagnostic.find("use of undeclared identifier '" + segment + "'")
                        != std::string::npos))
                    missDepth = static_cast<int>(i);
                if (!qualifiedScope.empty()) qualifiedScope += "::";
                qualifiedScope += segment;
            }
            if (missDepth >= 0) return missDepth * 2 + (ownsLead ? 1 : 0);
            return static_cast<int>(path.size()) * 2 + (ownsLead ? 1 : 0);
        };
        auto shouldReplaceCxxProbeError = [&](int score, size_t attempt,
                                               const std::string& probeError) {
            return score > lastErrorScopeScore
                || (score == lastErrorScopeScore
                    && (attempt == 0 || (lastError.find("consteval") != std::string::npos
                        && probeError.find("unavailable") != std::string::npos)));
        };
        std::vector<size_t> candidateGroups;
        // An infix request is keyed on its operands, not on the operator's name: try the groups
        // that own the operand types first, then every group declaring the namespace. The
        // per-name owner memo is neither read nor written - one pair must not steer another.
        if (infix || adlCall)
        {
            candidateGroups = dependencyGroups;
            const std::string lead = !infixLhsName.empty() ? infixLhsName
                : !infixRhsName.empty() ? infixRhsName : functionName;
            for (size_t group : CandidateCxxGroupsFor(CxxNameFromCflat(lead) + "::"))
                if (std::find(candidateGroups.begin(), candidateGroups.end(), group)
                    == candidateGroups.end())
                    candidateGroups.push_back(group);
        }
        else if (memberCall)
        {
            if (auto owner = cxxTypeOwnerGroup_.find(memberOwnerType);
                owner != cxxTypeOwnerGroup_.end() && owner->second < cxxImportGroups_.size())
                candidateGroups.push_back(owner->second);
            else
                candidateGroups = CandidateCxxGroupsFor(cxxBase);
        }
        else if (auto owner = cxxFunctionOwnerGroup_.find(functionName);
            owner != cxxFunctionOwnerGroup_.end() && owner->second < cxxImportGroups_.size())
            candidateGroups.push_back(owner->second);
        else if (auto owner = cxxFunctionTemplateOwnerGroup_.find(functionName);
                 owner != cxxFunctionTemplateOwnerGroup_.end()
                     && owner->second < cxxImportGroups_.size())
            candidateGroups.push_back(owner->second);
        else
            candidateGroups = CandidateCxxGroupsFor(cxxBase);
        for (size_t primary : candidateGroups)
        {
            CxxRequestGroup group = MakeCxxRequestGroup(primary, dependencyGroups);
            if (group.headers.empty()) continue;
            CxxRequestGroupScope groupScope(*this, &group);
            for (size_t attempt = 0; attempt < wrappers.size(); ++attempt)
            {
                const FreeWrapper& wrapper = wrappers[attempt];
                CSigEntry bound;
                std::string wrapperError;
                if (!RequestGeneratedCxxWrapper(group, wrapper.source, wrapper.name, "FREE",
                                                 bound, wrapperError))
                {
                    // A literal retry can expose availability hidden by forwarding's consteval error.
                    // A group missing a scope segment of the path ranks below one that resolved it.
                    const int scopeScore = cxxProbeScopeScore(group.primary, wrapperError);
                    if (shouldReplaceCxxProbeError(scopeScore, attempt, wrapperError))
                    {
                        lastError = FirstCxxErrorLine(wrapperError);
                        lastErrorScopeScore = scopeScore;
                        if (lastError.find("_list_narrowing") != std::string::npos)
                            lastError = "a brace-list element narrows in list-initialization";
                        for (const DeferredPlaceGuard& guard : deferredPlaceGuards)
                            if (lastError.find(wrapper.name + "_place" + std::to_string(guard.index))
                                != std::string::npos)
                                placeGuardError = std::format("'default' in the '?:' argument {} of C++ '{}' takes "
                                        "the class of its other arm ('{}'), but the '{}' overload clang "
                                        "selects does not take that class there; the parameter is not known "
                                        "before the call, so give 'default' its type explicitly",
                                        guard.index + 1, functionName, guard.cflatType, functionName);
                        lastCxxRequestClangRejected_ = true;
                        lastCxxRequestNoViableAtRequest_ = lastCxxWrapperNoViableAtRequest_;
                    }
                    continue;
                }
                // T518: the selected callee's parameter must be `default`'s class, not a conversion target.
                for (const DeferredPlaceGuard& guard : deferredPlaceGuards)
                    if (guard.index >= bound.conditionalArgumentKinds.size()
                        || bound.conditionalArgumentKinds[guard.index] != '=')
                    {
                        error = std::format("'default' in the '?:' argument {} of C++ '{}' takes the class of "
                                            "its other arm ('{}'), but the '{}' overload clang selects does not "
                                            "take that class there; the parameter is not known before the call, "
                                            "so give 'default' its type explicitly",
                                            guard.index + 1, functionName, guard.cflatType, functionName);
                        return false;
                    }
                if (IsCxxClosureReferenceResult(bound.retSpelling)
                    && std::any_of(arguments.begin(), arguments.end(), [](const NamedVariable& argument) {
                           return !argument.CxxClosureSignature.empty();
                       }))
                {
                    error = CxxClosureReferenceResultRefusal(functionName);
                    return false;
                }
                const bool hasClosureArgument = std::any_of(arguments.begin(), arguments.end(),
                    [](const NamedVariable& argument) {
                        // A closure variable is its own caller-frame slot; only a temporary dangles.
                        return !argument.CxxClosureSignature.empty() && !argument.CxxClosureBorrowed;
                    });
                if (hasClosureArgument && HasCxxForwardingReferenceTemplate(functionName)
                    && !bound.ret.Pointer && !bound.ret.IsFunctionPointer
                    && IsCxxRecord(bound.ret.TypeName))
                {
                    EnsureCxxRecordProjected(bound.ret.TypeName, false);
                    const bool storesClosureReference =
                        CxxRecordStoresForwardedClosureReference(bound.ret.TypeName,
                            bound.retSpelling.find("__cflat_closure<") != std::string::npos);
                    if (storesClosureReference)
                    {
                        error = std::format("cannot forward a CFlat closure to C++ '{}': its result "
                            "stores a reference to the forwarding argument, and CFlat cannot provide "
                            "the required caller-owned C++ closure slot", functionName);
                        return false;
                    }
                }
                /*
                 * Clang converted a scalar operand into a temporary of the WRAPPER's frame for
                 * the selected operator's `const S&`: re-spell that formal as `const S&` so the
                 * caller's slot is the temporary (an expression template keeps the reference).
                 */
                std::vector<CxxScalarRetarget> retarget;
                FreeWrapper retargeted;
                if (infix && PlanCxxOperatorScalarRetarget(bound, arguments, infixOperator, retarget))
                {
                    const std::vector<std::string> sourceSpellings = parameterSpellings;
                    std::vector<std::string> calls = wrapper.calls;
                    for (const CxxScalarRetarget& entry : retarget)
                    {
                        parameterSpellings[entry.index] = "const " + entry.spelling + " &";
                        calls[entry.index] = "p" + std::to_string(entry.index);
                    }
                    retargeted = makeWrapper(calls, attempt != 0);
                    parameterSpellings = sourceSpellings;
                    bool known = false;
                    if (auto fit = functionTable.find(retargeted.bindName); fit != functionTable.end())
                        for (const auto& symbol : fit->second)
                            known = known || (symbol.External && symbol.UniqueName == retargeted.name);
                    if (known)
                    {
                        StageCxxOperatorScalarArguments(arguments, retarget);
                        registeredName = retargeted.bindName;
                        lastCxxFreeWrapperLinkage_ = retargeted.name;
                        cxxFreeWrapperGroups_[retargeted.name] = { group.primary, group.dependencyGroups };
                        return true;
                    }
                    CSigEntry retargetBound;
                    if (!RequestGeneratedCxxWrapper(group, retargeted.source, retargeted.name,
                                                     "FREE", retargetBound, wrapperError))
                    {
                        const int scopeScore = cxxProbeScopeScore(group.primary, wrapperError);
                        if (shouldReplaceCxxProbeError(scopeScore, attempt, wrapperError))
                        {
                            lastError = FirstCxxErrorLine(wrapperError);
                            lastErrorScopeScore = scopeScore;
                            lastCxxRequestClangRejected_ = true;
                            lastCxxRequestNoViableAtRequest_ = lastCxxWrapperNoViableAtRequest_;
                        }
                        continue;
                    }
                    bound = std::move(retargetBound);
                    StageCxxOperatorScalarArguments(arguments, retarget);
                }
                // A literal's decayed pointer bound by the winner's `X *const &` moves to the caller.
                FreeWrapper literalRetargeted;
                bool literalRetarget = false;
                if (retarget.empty() && !literalArrayParams.empty())
                {
                    std::vector<std::string> calls = wrapper.calls;
                    std::string boundName = wrapper.name;
                    std::string retargetError;
                    if (!RetargetCxxLiteralPointerTemporaries(
                            bound, literalArrayParams, parameterSpellings, calls,
                            [&](const std::vector<std::string>& retargetCalls, std::string& name) {
                                literalRetargeted = makeWrapper(retargetCalls, attempt != 0);
                                name = literalRetargeted.name;
                                return literalRetargeted.source;
                            },
                            [&](const std::string& name, const std::string& input, CSigEntry& out,
                                std::string& requestError) {
                                return RequestGeneratedCxxWrapper(group, input, name, "FREE", out,
                                                                  requestError);
                            },
                            functionName, boundName, retargetError))
                    {
                        error = std::move(retargetError);
                        return false;
                    }
                    literalRetarget = boundName != wrapper.name;
                    if (literalRetarget)
                        if (auto fit = functionTable.find(literalRetargeted.bindName);
                            fit != functionTable.end())
                            for (const auto& symbol : fit->second)
                                if (symbol.External && symbol.UniqueName == literalRetargeted.name)
                                {
                                    registeredName = literalRetargeted.bindName;
                                    lastCxxFreeWrapperLinkage_ = literalRetargeted.name;
                                    cxxFreeWrapperGroups_[literalRetargeted.name] =
                                        { group.primary, group.dependencyGroups };
                                    return true;
                                }
                }
                const FreeWrapper& chosen = literalRetarget ? literalRetargeted
                    : retarget.empty() ? wrapper : retargeted;
                /*
                 * A list materialized in the wrapper dies when it returns, where C++ keeps it to
                 * the end of the caller's full-expression. When the result can carry an address
                 * (WrapperResultBorrowsBraceList) the call is re-bound through a second wrapper
                 * over CALLER-frame arrays: the caller allocates each list's `E[N]`, the wrapper
                 * constructs the elements there and passes an initializer_list over them, and
                 * the caller destroys them as a statement temporary. Refused if that fails.
                 */
                if (!braceGroups.empty() && bound.resultBorrowsBraceList)
                {
                    std::string ret = bound.retSpelling;
                    while (!ret.empty() && std::isspace((unsigned char)ret.back())) ret.pop_back();
                    if (retarget.empty())
                    {
                        std::string backedError;
                        switch (bindBraceBacked(group, bound, wrapper, backedError, false))
                        {
                        case 1: return true;
                        case -1:
                            error = std::format("C++ '{}' called with a brace list could not be "
                                                "bound over caller-frame list storage (clang: {})",
                                                functionName, backedError);
                            return false;
                        default: break;
                        }
                    }
                    error = std::format("C++ '{}' called with a brace list returns '{}', which "
                                        "can hold an address and so may refer into the list's "
                                        "temporary storage; CFlat can keep a brace list alive in "
                                        "the calling frame only when it reaches the callee as its "
                                        "std::initializer_list argument of a CFlat-mappable element type",
                                        functionName, ret);
                    lastCxxBraceLifetimeRefused_ = true;
                    return false;
                }
                /*
                 * A result that cannot refer into the list still lets a NON-TRIVIAL element's
                 * destruction be observed by the rest of the full-expression: back such lists
                 * in the caller too when possible, else keep the single wrapper.
                 */
                if (!braceGroups.empty() && retarget.empty()
                    && !bound.braceListElementSpellings.empty())
                {
                    std::string backedError;
                    if (bindBraceBacked(group, bound, wrapper, backedError, true) == 1) return true;
                }
                // Marker from CClangExtract's BraceTemporaryHasNontrivialElements: the list became
                // an array/aggregate/container temporary of non-trivial elements in the wrapper.
                if (!braceGroups.empty()
                    && std::find(bound.braceListElementSpellings.begin(),
                                 bound.braceListElementSpellings.end(),
                                 "#nontrivial-brace-temporary")
                           != bound.braceListElementSpellings.end())
                {
                    error = std::format("C++ '{}' called with a brace list that becomes an array, "
                                        "aggregate or container temporary of non-trivially "
                                        "destructible or copyable elements: CFlat would destroy "
                                        "them before the end of the expression; pass a named "
                                        "local array or aggregate instead", functionName);
                    lastCxxBraceLifetimeRefused_ = true;
                    return false;
                }
                for (size_t argumentIndex : functionPointerCallerSlots)
                    if (argumentIndex < bound.params.size() && argumentIndex < arguments.size())
                    {
                        bound.params[argumentIndex] = arguments[argumentIndex].TypeAndValue;
                        if (bound.params[argumentIndex].TypeName.empty())
                            bound.params[argumentIndex].TypeName = "__c_fn_ptr";
                        bound.params[argumentIndex].IsFunctionPointer = true;
                    }
                for (const auto& [parameterIndex, slotType] : constructionFunctionPointerSlots)
                    if (parameterIndex < bound.params.size())
                    {
                        bound.params[parameterIndex] = slotType;
                        if (bound.params[parameterIndex].TypeName.empty())
                            bound.params[parameterIndex].TypeName = "__c_fn_ptr";
                        bound.params[parameterIndex].IsFunctionPointer = true;
                    }
                bound.name = chosen.bindName;
                RegisterCSignatures({ bound }, group.headers.front());
                if (auto fit = functionTable.find(chosen.bindName); fit != functionTable.end())
                    for (const auto& symbol : fit->second)
                        if (symbol.External && symbol.UniqueName == chosen.name)
                        {
                            registeredName = chosen.bindName;
                            lastCxxFreeWrapperLinkage_ = chosen.name;
                            cxxFreeWrapperGroups_[chosen.name] = { group.primary, group.dependencyGroups };
                            if (!infix && !adlCall)
                                cxxTemplateOwnerGroup_[cxxBase] = primary;
                            return true;
                        }
                lastError = "the generated wrapper could not be registered";
                lastCxxRequestClangRejected_ = false;
                break;
            }
        }

        if (infix || adlCall)
        {
            error = lastError.empty() ? std::string("no C++ import declares it")
                                      : "clang: " + lastError;
            return false;
        }
        if (functionMacro)
        {
            // `offsetof(S, m)`: a function-like macro whose arguments are no values.
            error = std::format("C++ macro '{}' cannot be called with these arguments", functionName);
            if (!lastError.empty()) error += " (clang: " + lastError + ")";
            return false;
        }
        if (!placeGuardError.empty())
        {
            error = placeGuardError;
            return false;
        }
        // clang found the name but could not bind it (a nested ctor or call argument refused):
        // its reason is the cause, not "not a member of namespace".
        if (!lastError.empty() && !lastError.starts_with("no member named "))
        {
            error = std::format("C++ free function '{}' could not be bound (clang: {})",
                                functionName, lastError);
            return false;
        }
        const size_t missingDot = functionName.rfind('.');
        const std::string missingMember = functionName.substr(missingDot + 1);
        const std::string missingNamespace = memberCall ? memberOwnerType
            : functionName.substr(0, missingDot);
        error = std::format("'{}' is not a member of namespace '{}' (C++ free function '{}' "
                            "could not be bound", missingMember, missingNamespace, functionName);
        if (!lastError.empty()) error += " (clang: " + lastError + ")";
        error += ")";
        return false;
}

bool LLVMBackend::RequestCxxFreeBraceCall(const std::string& functionName,
                                          const std::vector<std::string>& explicitArgs,
                                          std::vector<NamedVariable>& arguments,
                                          const std::vector<CxxBraceArgument>& braceArguments,
                                          std::string& registeredName,
                                          std::string& error)
{
        error.clear();
        registeredName.clear();
        lastCxxBraceLifetimeRefused_ = false;
        if (braceArguments.empty()) return false;
        std::vector<const CxxBraceArgument*> ordered;
        ordered.reserve(ordered.size() + braceArguments.size());
        for (const auto& brace : braceArguments) ordered.push_back(&brace);
        std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
            return a->argumentIndex < b->argumentIndex;
        });
        std::vector<std::pair<size_t, size_t>> groups;
        size_t shift = 0;
        for (const auto* brace : ordered)
        {
            groups.emplace_back(brace->argumentIndex + shift, brace->elements.size());
            shift += brace->elements.size();
            shift -= 1;
        }
        std::vector<NamedVariable> expanded = arguments;
        std::vector<CxxBraceArgument> elementsOnly = braceArguments;
        for (auto& brace : elementsOnly) brace.wrapperArguments.clear();
        ExpandCxxBraceArguments(expanded, elementsOnly);
        if (!RequestCxxFreeFunction(functionName, explicitArgs, expanded, registeredName, error,
                                    {}, {}, {}, false, groups))
            return false;
        arguments = std::move(expanded);
        return true;
}

bool LLVMBackend::RequestCxxBraceFunction(const std::string& functionName,
                                          const std::string& ownerType,
                                          const std::string& memberName,
                                          std::vector<NamedVariable>& arguments,
                                          std::vector<CxxBraceArgument>& braceArguments,
                                          std::string& error)
{
        error.clear();
        if (braceArguments.empty()) return false;

        for (auto& brace : braceArguments)
            for (auto& element : brace.elements)
            {
                const std::string enumName = ResolveEnumTypeName(element.TypeAndValue.TypeName);
                const bool enumElement = element.TypeAndValue.IsScopedEnum
                    || !element.TypeAndValue.EnumBacking.empty() || !enumName.empty();
                if (enumElement && IsImportedEnumeratorStorage(element.Storage))
                {
                    TypeAndValue scalarType = element.TypeAndValue;
                    scalarType.Pointer = false;
                    scalarType.ElemPointer = false;
                    scalarType.PointerDepth = 0;
                    llvm::Type* valueType = GetType(scalarType);
                    if (valueType != nullptr && valueType->isIntegerTy())
                    {
                        element.Primary = builder->CreateLoad(valueType, element.Storage,
                                                              "cxx.brace.enum.constant");
                        element.BaseType = valueType;
                        element.TypeAndValue = std::move(scalarType);
                        element.IsRvalue = true;
                        element.CxxImportedLiteral = CxxImportedConstantLiteral(element);
                        RecordCxxConstantBraceElement(element, element.Primary);
                        continue;
                    }
                }
                if (element.TypeAndValue.Pointer || element.Primary == nullptr
                    || !element.Primary->getType()->isPointerTy() || !enumElement)
                    continue;
                llvm::Type* valueType = GetType(element.TypeAndValue);
                if (valueType == nullptr || !valueType->isIntegerTy()) continue;
                element.Primary = builder->CreateLoad(valueType, element.Primary, "cxx.brace.enum");
                element.BaseType = valueType;
            }

        struct Candidate
        {
            std::vector<std::string> paramTypes;
            std::string retType;
            std::string target;
            std::vector<std::string> paramNames;
            std::string file;
            int line = 1;
            int col = 0;
            bool isNoexcept = false;
            bool instance = false;
            bool isConst = false;
            bool found = false;
        } selected;

        auto fitsBraces = [&](const std::vector<std::string>& params) {
            for (const auto& brace : braceArguments)
                if (brace.parameterIndex >= params.size()) return false;
            return true;
        };
        const bool hasMixedBraceElements = std::any_of(
            braceArguments.begin(), braceArguments.end(),
            [](const CxxBraceArgument& brace) { return brace.hasMixedElements; });
        auto isBareCxxClass = [&](const std::string& spelling) {
            std::string type = TrimCxxBraceType(spelling);
            if (!type.empty() && type.back() == '&')
            {
                type.pop_back();
                while (!type.empty() && std::isspace((unsigned char)type.back())) type.pop_back();
            }
            TypeAndValue mapped;
            return MapCTypeToTypeAndValue(type, mapped, true)
                && !mapped.Pointer && IsCxxRecord(mapped.TypeName);
        };
        auto targetElementSpelling = [&](const std::string& parameter) {
            return CxxBraceContainerElementSpelling(parameter);
        };
        auto classElementsNeedConversion = [&](const CxxBraceArgument& brace,
                                                const std::string& parameter) {
            if (!brace.hasCxxClassElements) return false;
            if (brace.hasMixedElements) return true;
            const std::string target = targetElementSpelling(parameter);
            if (target.empty() || brace.cxxElementType.empty()) return false;
            std::string source;
            return !CxxSpellingForCflatType(brace.cxxElementType, source)
                || source != target;
        };
        auto nonBraceArgumentsFit = [&](const std::vector<std::string>& params,
                                        bool instance) {
            const size_t first = instance ? 1u : 0u;
            if (params.size() < arguments.size()) return false;
            for (size_t i = first; i < arguments.size(); ++i)
            {
                bool isBrace = false;
                for (const auto& brace : braceArguments)
                    if (brace.argumentIndex == i) { isBrace = true; break; }
                if (isBrace || !isBareCxxClass(params[i])) continue;
                const auto& arg = arguments[i];
                if (!arg.TypeAndValue.Pointer && IsCxxRecord(arg.TypeAndValue.TypeName)) continue;
                return false;
            }
            return true;
        };
        auto isCxxBraceListClass = [&](const std::string& parameter) {
            if (CxxBraceParameterIsPointer(parameter)) return false;
            if (isBareCxxClass(parameter)) return true;
            const std::string container = StripCxxRefAndCv(parameter);
            auto known = cxxForeignTypeSpellings_.find(SqueezeCxxSpelling(container));
            return known != cxxForeignTypeSpellings_.end()
                && cxxRecordEntries_.contains(known->second);
        };
        std::map<std::string, std::string> classBraceListElements;
        auto classBraceListElementFor = [&](const std::string& parameter,
                                             CxxBraceContainerKind kind) {
            if (kind != CxxBraceContainerKind::ArrayRef
                && kind != CxxBraceContainerKind::PointerPairClass
                && !isCxxBraceListClass(parameter))
                return std::string();
            const std::string key = StripCxxRefAndCv(parameter);
            auto [it, inserted] = classBraceListElements.try_emplace(key);
            if (inserted) it->second = CxxClassBraceListElement(parameter);
            return it->second;
        };
        // A scalar list the callee can keep a view of - a std::initializer_list parameter, or a
        // class it copy-list-initializes through its own initializer_list<E> constructor - is
        // backed by CALLER-frame storage (a thunk-local list dangles once the thunk returns).
        // Yields that E; empty for any other target, a vector (it copies) included.
        auto scalarBackedElement = [&](const CxxBraceArgument& brace, const std::string& parameter) {
            if (brace.hasCxxClassElements || brace.elements.empty()) return std::string();
            const CxxBraceContainerKind kind = CxxBraceContainerKindOf(parameter);
            if (kind == CxxBraceContainerKind::Vector) return std::string();
            std::string element;
            if (kind == CxxBraceContainerKind::InitializerList)
                element = CxxBraceTargetElementSpelling(parameter);
            else if (kind == CxxBraceContainerKind::Unsupported
                || kind == CxxBraceContainerKind::ArrayRef
                || kind == CxxBraceContainerKind::PointerPairClass)
                element = classBraceListElementFor(parameter, kind);
            else
                return std::string();
            return IsCxxScalarOrBoolBraceElement(element) && CxxBraceElementsAllScalar(brace)
                ? element : std::string();
        };
        std::string initializerListRefusal;
        auto bracePriorityFor = [&](const std::vector<std::string>& params,
                                    const std::vector<std::string>& names) -> std::optional<int> {
            int priority = 0;
            for (const auto& brace : braceArguments)
            {
                if (brace.parameterIndex >= params.size()) return std::nullopt;
                const std::string targetParameter = brace.parameterIndex < params.size()
                    ? params[brace.parameterIndex] : std::string();
                const CxxBraceContainerKind kind = CxxBraceContainerKindOf(targetParameter);
                CxxBraceContainerKind effectiveKind = kind;
                if (effectiveKind == CxxBraceContainerKind::Unsupported
                    && isBareCxxClass(targetParameter))
                    effectiveKind = CxxBraceContainerKind::PointerPairClass;
                if (effectiveKind == CxxBraceContainerKind::Unsupported)
                    return std::nullopt;
                if (effectiveKind == CxxBraceContainerKind::InitializerList)
                {
                    if (!brace.elements.empty() && !brace.hasCxxClassElements)
                    {
                        // An enum element converts to no other type (clang): a list of enum E
                        // takes only E elements. A scoped element into a 32-bit scalar is refused
                        // here; other scalar targets report it through the narrowing check.
                        const std::string element = CxxBraceTargetElementSpelling(targetParameter);
                        TypeAndValue elementType;
                        if (!element.empty() && MapCTypeToTypeAndValue(element, elementType, true)
                            && !elementType.Pointer)
                        {
                            const std::string targetEnum = ResolveEnumTypeName(elementType.TypeName);
                            const bool scalarTarget = targetEnum.empty()
                                && elementType.IsInteger() == 32;
                            for (const auto& source : brace.elements)
                            {
                                const std::string sourceEnum = source.TypeAndValue.Pointer
                                    || source.TypeAndValue.ElemPointer ? std::string()
                                    : ResolveEnumTypeName(source.TypeAndValue.TypeName);
                                const bool refused = !targetEnum.empty()
                                    ? sourceEnum != targetEnum
                                    : scalarTarget && (source.TypeAndValue.IsScopedEnum
                                        || IsScopedEnumTypeName(sourceEnum));
                                if (!refused) continue;
                                if (initializerListRefusal.empty())
                                    initializerListRefusal = std::format(
                                        "no overload of '{}' matches the given arguments.",
                                        functionName);
                                return std::nullopt;
                            }
                        }
                    }
                    if (brace.hasCxxClassElements
                        && !classElementsNeedConversion(brace, targetParameter))
                    {
                        if (initializerListRefusal.empty())
                        {
                            const std::string parameterName = brace.parameterIndex < names.size()
                                && !names[brace.parameterIndex].empty()
                                ? names[brace.parameterIndex]
                                : std::format("#{}", brace.parameterIndex);
                            const std::string element = CxxBraceTargetElementSpelling(targetParameter);
                            initializerListRefusal = std::format(
                                "C++ parameter '{}' of '{}' takes std::initializer_list<{}>, "
                                "which cannot be formed from a materialized CFlat brace list; "
                                "provide an ArrayRef, IListRef or vector overload",
                                parameterName, functionName,
                                element.empty() ? brace.cxxElementType : element);
                        }
                        return std::nullopt;
                    }
                }
                // A backed list converts each element itself, so mixed scalars are fine.
                if (!brace.elements.empty()
                    && !classElementsNeedConversion(brace, targetParameter)
                    && CxxBraceElementSpelling(brace, targetParameter).empty()
                    && scalarBackedElement(brace, targetParameter).empty())
                    return std::nullopt;
                priority = std::max(priority,
                    effectiveKind == CxxBraceContainerKind::ArrayRef ? 0
                    : effectiveKind == CxxBraceContainerKind::Vector ? 1 : 2);
            }
            return priority;
        };
        // The CFlat primitive a C++ element spelling or identity names ("unsigned int" is u32).
        auto primitiveOf = [&](const std::string& spelling) {
            const std::string compact = SqueezeCxxSpelling(StripCxxRefAndCv(spelling));
            const char* integer = CxxCompactIntegerSpellingToCflat(compact);
            return CanonicalPrimitiveTypeName(integer != nullptr ? std::string(integer) : compact);
        };
        auto integerBitsOf = [](const std::string& name) {
            TypeAndValue probe;
            probe.TypeName = name;
            return probe.IsInteger();
        };
        // One braced element against a candidate's element type, ranked as RankCxxConversionSequences
        // ranks an argument: an unscoped enum by its fixed or promoted type, anything else by identity.
        // 0 exact, 1 integral or floating promotion, 2 any other conversion. `certain` drops to false
        // when the rank rests on a spelling this model does not classify (such a tie is never refused).
        auto isArithmeticName = [&](const std::string& name) {
            return name == "bool" || name == "float" || name == "double" || integerBitsOf(name) > 0;
        };
        auto elementRank = [&](const NamedVariable& e, const std::string& to, bool& certain) -> size_t {
            const TypeAndValue& tv = e.TypeAndValue;
            const bool importedEnumValue = IsImportedEnumeratorStorage(e.Storage);
            const std::string key = (tv.Pointer || tv.ElemPointer) && !importedEnumValue
                ? std::string() : ResolveEnumTypeName(tv.TypeName);
            TypeAndValue mappedTarget;
            const bool targetMapped = MapCTypeToTypeAndValue(to, mappedTarget, true);
            const std::string targetEnum = ResolveEnumTypeName(
                targetMapped ? mappedTarget.TypeName : to);
            if (!key.empty() && !targetEnum.empty() && key == targetEnum) return 0;
            const std::string target = primitiveOf(to);
            if (!isArithmeticName(target)) certain = false;
            auto promoted = enumPromotedTypes_.find(key);
            if (!key.empty() && !IsScopedEnumTypeName(key) && promoted != enumPromotedTypes_.end())
            {
                const std::string promotedName = primitiveOf(promoted->second);
                auto backing = enumBackingTypes.find(key);
                const std::string backingName = backing != enumBackingTypes.end()
                    ? primitiveOf(backing->second) : std::string();
                const bool narrowFixed = !backingName.empty()
                    && (backingName == "bool" || (integerBitsOf(backingName) > 0
                                                  && integerBitsOf(backingName) < 32));
                if (narrowFixed && target == backingName) return 0;
                return target == promotedName ? 1 : 2;
            }
            const std::string identity = primitiveOf(CxxBraceElementIdentity(e));
            if (identity.empty() || !isArithmeticName(identity)) certain = false;
            if (identity.empty()) return 2;
            if (identity == target) return 0;
            if (target == "int" && (identity == "bool" || (integerBitsOf(identity) > 0
                                                      && integerBitsOf(identity) < 32)))
                return 1;
            if (target == "double" && identity == "float") return 1;
            return 2;
        };
        // Candidates tied on priority and arity: clang ranks a braced list by its WORST element
        // conversion ([over.ics.list]); the candidate whose worst element ranks lowest wins. A tie
        // between two candidates that the model classifies exactly is ambiguous in clang, so the
        // caller refuses it (the old first-declared pick silently chose one). `certain` is false when
        // the rank is not decidable here: several braced arguments (per-argument dominance), or an
        // element/parameter spelling outside the arithmetic model.
        auto braceElementRank = [&](const std::vector<std::string>& params, bool& certain) {
            size_t worst = 0;
            certain = certain && braceArguments.size() == 1;
            for (const auto& brace : braceArguments)
            {
                if (brace.parameterIndex >= params.size() || brace.hasCxxClassElements)
                {
                    certain = false;
                    continue;
                }
                const std::string& target = params[brace.parameterIndex];
                std::string element;
                std::string extent;
                if (CxxBraceContainerKindOf(target) == CxxBraceContainerKind::InitializerList)
                    element = CxxBraceTargetElementSpelling(target);
                else if (!CxxArrayReferenceParameter(target, element, extent))
                {
                    certain = false;
                    continue;
                }
                if (element.empty())
                {
                    certain = false;
                    continue;
                }
                for (const auto& e : brace.elements)
                    worst = std::max(worst, elementRank(e, element, certain));
            }
            return worst;
        };
        // Display of one candidate for the ambiguity refusal: name(params), C++ types shown as CFlat.
        auto displayCandidate = [&](const std::string& name, const std::vector<std::string>& params) {
            std::string shown = name + "(";
            for (size_t p = 0; p < params.size(); ++p)
                shown += (p == 0 ? "" : ", ") + DisplayCxxClassName(params[p]);
            return shown + ")";
        };
        if (ownerType.empty())
        {
            auto it = cxxFunctionSignatures_.find(functionName);
            if (it == cxxFunctionSignatures_.end()) return false;
            int selectedBracePriority = 100;
            size_t selectedExtraParameters = static_cast<size_t>(-1);
            size_t selectedRank = static_cast<size_t>(-1);
            bool selectedCertain = false;
            bool bracesTied = false;
            std::vector<std::string> tiedCandidates;
            for (const auto& sig : it->second)
            {
                // A brace wrapper registered by an earlier call is not a C++ overload: its
                // backed-list `const E *` parameter would take a later list in the wrong slot.
                if (sig.linkageName.starts_with("__cflat_tpl_")) continue;
                if (sig.paramSpellings.size() < arguments.size()
                    || !fitsBraces(sig.paramSpellings)
                    || !nonBraceArgumentsFit(sig.paramSpellings, false)) continue;
                const auto bracePriority = bracePriorityFor(sig.paramSpellings, sig.paramNames);
                if (!bracePriority) continue;
                const size_t extraParameters = sig.paramSpellings.size() - arguments.size();
                // A plain argument is not ranked here (arguments hold the braced slots too).
                bool certain = arguments.size() == braceArguments.size();
                const size_t rank = braceElementRank(sig.paramSpellings, certain);
                const bool better = !selected.found
                    || *bracePriority < selectedBracePriority
                    || (*bracePriority == selectedBracePriority
                        && (extraParameters < selectedExtraParameters
                            || (extraParameters == selectedExtraParameters
                                && rank < selectedRank)));
                if (!better)
                {
                    if (selected.found && *bracePriority == selectedBracePriority
                        && extraParameters == selectedExtraParameters && rank == selectedRank)
                    {
                        if (certain && selectedCertain) bracesTied = true;
                        else bracesTied = selectedCertain = false;
                        if (bracesTied) tiedCandidates.push_back(displayCandidate(functionName, sig.paramSpellings));
                    }
                    continue;
                }
                // A strictly better candidate replaces any earlier tie.
                bracesTied = false;
                tiedCandidates.clear();
                selected.paramTypes = sig.paramSpellings;
                selected.paramNames = sig.paramNames;
                selected.retType = sig.retSpelling;
                selected.target = CxxNameFromCflat(functionName);
                selected.file = sig.file;
                selected.line = sig.line;
                selected.col = sig.col;
                selected.isNoexcept = sig.isNoexcept;
                selectedBracePriority = *bracePriority;
                selectedExtraParameters = extraParameters;
                selectedRank = rank;
                selectedCertain = certain;
                selected.found = true;
                tiedCandidates = {displayCandidate(functionName, sig.paramSpellings)};
            }
            if (bracesTied)
            {
                std::string candidateList;
                for (const auto& c : tiedCandidates)
                    candidateList += (candidateList.empty() ? "" : ", ") + c;
                error = std::format("ambiguous call to '{}': no candidate ranks better than the others: "
                                    "{}. Cast the argument to the parameter type you mean.",
                                    functionName, candidateList);
                return false;
            }
            if (!selected.found)
            {
                if (!initializerListRefusal.empty()) error = initializerListRefusal;
                else if (hasMixedBraceElements)
                    error = "brace arguments must contain scalar values of one type";
                return false;
            }
        }
        else
        {
            auto record = cxxRecordEntries_.find(ownerType);
            if (record == cxxRecordEntries_.end()) return false;
            const cflat_cinterop::RawCxxMember* fallback = nullptr;
            int fallbackBracePriority = 100;
            size_t fallbackExtraParameters = static_cast<size_t>(-1);
            size_t fallbackRank = static_cast<size_t>(-1);
            bool fallbackCertain = false;
            bool fallbackConst = false;
            bool fallbackQualified = false;
            // An instance member lists its implicit object parameter first; the display omits it.
            auto memberShownParams = [](const cflat_cinterop::RawCxxMember& member) {
                const size_t skip = member.kind == cflat_cinterop::RawCxxMember::Instance
                    && !member.paramTypes.empty() ? 1 : 0;
                return std::vector<std::string>(member.paramTypes.begin() + skip, member.paramTypes.end());
            };
            bool bracesTied = false;
            std::vector<std::string> tiedCandidates;
            for (const auto& member : record->second.members)
            {
                if (member.name != memberName
                    || (member.kind != cflat_cinterop::RawCxxMember::Instance
                        && member.kind != cflat_cinterop::RawCxxMember::StaticMethod))
                    continue;
                const bool instance = member.kind == cflat_cinterop::RawCxxMember::Instance;
                if (member.paramTypes.size() < arguments.size()
                    || !fitsBraces(member.paramTypes)
                    || !nonBraceArgumentsFit(member.paramTypes, member.kind == cflat_cinterop::RawCxxMember::Instance)) continue;
                const bool receiverLooksPresent = !arguments.empty()
                    && arguments.front().TypeAndValue.TypeName == ownerType;
                if (instance != receiverLooksPresent) continue;
                const auto bracePriority = bracePriorityFor(member.paramTypes, member.paramNames);
                if (!bracePriority) continue;
                const size_t extraParameters = member.paramTypes.size() - arguments.size();
                // Only the receiver is ranked here; a plain argument beyond it makes a tie undecidable.
                bool certain = arguments.size() == braceArguments.size() + (instance ? 1u : 0u);
                const size_t rank = braceElementRank(member.paramTypes, certain);
                const bool better = fallback == nullptr
                    || *bracePriority < fallbackBracePriority
                    || (*bracePriority == fallbackBracePriority
                        && (extraParameters < fallbackExtraParameters
                            || (extraParameters == fallbackExtraParameters
                                && rank < fallbackRank)));
                if (!better)
                {
                    // clang ranks an implicit object parameter by const: tied overloads that differ in
                    // const are not a tie there, so only same-const candidates are refused.
                    if (*bracePriority == fallbackBracePriority
                        && extraParameters == fallbackExtraParameters && rank == fallbackRank)
                    {
                        // A ref- or volatile-qualified receiver changes viability: never a plain tie.
                        const bool sameConst = member.isConst == fallbackConst && !fallbackQualified
                            && member.refQualifier == cflat_cinterop::CxxRefQualifierNone && !member.isVolatile;
                        if (certain && fallbackCertain && sameConst) bracesTied = true;
                        else bracesTied = fallbackCertain = false;
                        if (bracesTied)
                            tiedCandidates.push_back(displayCandidate(memberName, memberShownParams(member)));
                    }
                    continue;
                }
                bracesTied = false;
                tiedCandidates = {displayCandidate(memberName, memberShownParams(member))};
                fallback = &member;
                fallbackBracePriority = *bracePriority;
                fallbackExtraParameters = extraParameters;
                fallbackRank = rank;
                fallbackCertain = certain;
                fallbackConst = member.isConst;
                fallbackQualified = member.refQualifier != cflat_cinterop::CxxRefQualifierNone || member.isVolatile;
            }
            if (bracesTied)
            {
                std::string candidateList;
                for (const auto& c : tiedCandidates)
                    candidateList += (candidateList.empty() ? "" : ", ") + c;
                error = std::format("ambiguous call to '{}': no candidate ranks better than the others: "
                                    "{}. Cast the argument to the parameter type you mean.",
                                    memberName, candidateList);
                return false;
            }
            if (fallback == nullptr)
            {
                if (!initializerListRefusal.empty()) error = initializerListRefusal;
                else if (hasMixedBraceElements)
                    error = "brace arguments must contain scalar values of one type";
                return false;
            }
            selected.paramTypes = fallback->paramTypes;
            selected.paramNames = fallback->paramNames;
            selected.retType = fallback->retType;
            selected.file = fallback->file;
            selected.line = fallback->line;
            selected.col = fallback->col;
            selected.isNoexcept = fallback->isNoexcept;
            selected.instance = fallback->kind == cflat_cinterop::RawCxxMember::Instance;
            selected.isConst = fallback->isConst;
            selected.found = true;
        }

        std::string ownerSpelling;
        if (selected.instance || !ownerType.empty())
            if (!CxxSpellingForCflatType(ownerType, ownerSpelling))
            {
                error = "the C++ receiver type is not registered";
                return false;
            }

        auto braceForArgument = [&](size_t index) -> const CxxBraceArgument* {
            for (const auto& brace : braceArguments)
                if (brace.argumentIndex == index) return &brace;
            return nullptr;
        };
        auto cflatTypeOf = [&](const NamedVariable& arg) {
            std::string type = !arg.LiteralIdentity.empty()
                ? arg.LiteralIdentity : arg.TypeAndValue.TypeName;
            bool pointer = arg.TypeAndValue.Pointer;
            const bool fixedArray = IsCxxFixedArrayArgument(arg);
            llvm::Type* valueType = arg.Primary != nullptr ? arg.Primary->getType() : arg.BaseType;
            // An arithmetic result keeps its C++ type (`l + 1` is `long`, not the i64 TypeName).
            if (std::string arith = CxxArithIdentityForArgument(arg, valueType); !arith.empty())
                type = arith;
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
                // `&p` over a `T*` is a `T**` prvalue: its recorded depth is the second star.
                if (arg.TypeAndValue.ElemPointer
                    || (arg.IsCxxAddressOfPointerValue && arg.TypeAndValue.PointerDepth >= 2))
                    type += "*";
            }
            if (fixedArray) type += "*";
            return type;
        };
        auto braceElementParameterSpelling = [&](const NamedVariable& element) {
            const std::string cflatType = cflatTypeOf(element);
            if (cflatType.empty()) return std::string();
            const bool stringLiteral = [&] {
                auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(element.Primary);
                return constant != nullptr && IsStringLiteralConstant(constant);
            }();
            if ((element.TypeAndValue.TypeName == "char"
                    && element.TypeAndValue.Pointer && element.IsRvalue)
                || stringLiteral)
                return std::string("const char *");

            std::string spelling;
            // LiteralIdentity records the C++ rank of an unsuffixed literal even when its LLVM
            // value uses a narrower storage type. This lets a converting constructor choose the
            // natural `int` overload while named CFlat integers retain their declared width.
            const std::string& spellingType = !element.TypeAndValue.Pointer
                && !element.LiteralIdentity.empty() ? element.LiteralIdentity : cflatType;
            if (!CxxSpellingForCflatType(spellingType, spelling)) return std::string();
            if (!element.TypeAndValue.Pointer && IsCxxRecord(element.TypeAndValue.TypeName))
            {
                if (!element.IsRvalue) return "const " + spelling + " &";
                return spelling + " &&";
            }
            return spelling;
        };
        std::vector<std::string> parameterSpellings;
        std::vector<std::string> callArguments;
        std::map<size_t, std::pair<size_t, size_t>> classWrapperParams;
        // Backed scalar lists: argument index -> (thunk parameter index, element spelling).
        std::map<size_t, std::pair<size_t, std::string>> scalarBackedParams;
        // Converted class lists: argument index -> (caller array parameter index, element type).
        std::map<size_t, std::pair<size_t, std::string>> convertedClassBackedParams;
        std::string wrapperBodyPrefix;
        bool wrapperNeedsInitializerList = false;
        bool wrapperNeedsPlacementNew = false;
        size_t flatParameterIndex = 0;
        if (selected.instance)
        {
            const NamedVariable& receiver = arguments.front();
            std::string receiverSpelling = selected.isConst ? "const " : "";
            receiverSpelling += ownerSpelling;
            if (receiver.TypeAndValue.Pointer)
            {
                receiverSpelling += " *";
                callArguments.push_back("p0->" + memberName);
            }
            else
            {
                receiverSpelling += " &";
                callArguments.push_back("p0." + memberName);
            }
            parameterSpellings.push_back(std::move(receiverSpelling));
            flatParameterIndex = 1;
        }
        for (size_t i = selected.instance ? 1u : 0u; i < arguments.size(); ++i)
        {
            const auto* brace = braceForArgument(i);
            if (brace != nullptr)
            {
                const std::string targetParameter = i < selected.paramTypes.size()
                    ? selected.paramTypes[i] : std::string();
                if (brace->hasCxxClassElements)
                {
                    const CxxBraceContainerKind kind = CxxBraceContainerKindOf(targetParameter);
                    const CxxBraceContainerKind effectiveKind = kind == CxxBraceContainerKind::Unsupported
                        && isBareCxxClass(targetParameter)
                        ? CxxBraceContainerKind::PointerPairClass : kind;
                    if (effectiveKind == CxxBraceContainerKind::Unsupported)
                    {
                        error = "C++ brace-list parameter is not a supported pointer-pair container";
                        return false;
                    }
                    const bool convertElements = classElementsNeedConversion(*brace, targetParameter);
                    if (convertElements)
                    {
                        const std::string elementType = targetElementSpelling(targetParameter);
                        if (elementType.empty())
                        {
                            error = "C++ brace-list parameter has no recoverable element class type";
                            return false;
                        }
                        std::string braceContainer = StripCxxRefAndCv(targetParameter);
                        while (!braceContainer.empty() && braceContainer.back() == '*')
                        {
                            braceContainer.pop_back();
                            while (!braceContainer.empty()
                                   && std::isspace((unsigned char)braceContainer.back()))
                                braceContainer.pop_back();
                        }
                        std::string convertedElements;
                        std::string callerArray;
                        size_t callerArrayIndex = static_cast<size_t>(-1);
                        const bool callerBacked = effectiveKind == CxxBraceContainerKind::InitializerList
                            || effectiveKind == CxxBraceContainerKind::ArrayRef
                            || effectiveKind == CxxBraceContainerKind::PointerPairClass;
                        if (callerBacked)
                        {
                            wrapperNeedsPlacementNew = true;
                            callerArrayIndex = flatParameterIndex++;
                            callerArray = "p" + std::to_string(callerArrayIndex);
                            parameterSpellings.push_back(elementType + " *");
                            convertedClassBackedParams[brace->argumentIndex] = {
                                callerArrayIndex, elementType };
                        }
                        for (size_t element = 0; element < brace->elements.size(); ++element)
                        {
                            const auto& source = brace->elements[element];
                            const std::string sourceParameter = braceElementParameterSpelling(source);
                            if (sourceParameter.empty())
                            {
                                error = "a C++ brace-list element has no C++ spelling";
                                return false;
                            }
                            const size_t parameterIndex = flatParameterIndex++;
                            parameterSpellings.push_back(sourceParameter);
                            std::string sourceType = cflatTypeOf(source);
                            if (!source.TypeAndValue.Pointer && IsCxxRecord(sourceType))
                            {
                                std::string sourceSpelling;
                                if (!CxxSpellingForCflatType(sourceType, sourceSpelling))
                                {
                                    error = "a C++ brace-list class element has no C++ spelling";
                                    return false;
                                }
                                sourceType = sourceSpelling;
                            }
                            std::string expression = "p" + std::to_string(parameterIndex);
                            if (!source.TypeAndValue.Pointer && IsCxxRecord(cflatTypeOf(source))
                                && source.IsRvalue)
                                expression = "static_cast<" + sourceType + " &&>(" + expression + ")";
                            if (callerBacked)
                                wrapperBodyPrefix += "::new (" + callerArray + " + "
                                    + std::to_string(element) + ") " + elementType + "(" + expression
                                    + "); ";
                            else
                            {
                                if (!convertedElements.empty()) convertedElements += ", ";
                                convertedElements += elementType + "(" + expression + ")";
                            }
                        }
                        if (effectiveKind == CxxBraceContainerKind::InitializerList)
                        {
                            wrapperNeedsInitializerList = true;
                            const std::string listType = "std::initializer_list<" + elementType + ">";
                            if (callerBacked)
                            {
                                const std::string il = "__cflat_converted_il_"
                                    + std::to_string(brace->argumentIndex);
                                wrapperBodyPrefix += CxxInitializerListOverStorage(
                                    listType, il, callerArray, brace->elements.size());
                                callArguments.push_back(listType + "(" + il + ")");
                            }
                            else
                                callArguments.push_back(listType + "{" + convertedElements + "}");
                        }
                        else if (effectiveKind == CxxBraceContainerKind::Vector)
                        {
                            callArguments.push_back(braceContainer + "{" + convertedElements + "}");
                        }
                        else
                        {
                            const std::string arrayName = callerBacked ? callerArray
                                : "__cflat_brace_array_" + std::to_string(brace->argumentIndex);
                            if (!callerBacked)
                                wrapperBodyPrefix += elementType + " " + arrayName + "[] = {"
                                    + convertedElements + "}; ";
                            callArguments.push_back(braceContainer + "(" + arrayName + ", "
                                                    + std::to_string(brace->elements.size()) + ")");
                        }
                        continue;
                    }
                    if (brace->hasMixedElements || brace->cxxElementType.empty())
                    {
                        error = "brace arguments must contain values of one C++ class type";
                        return false;
                    }
                    if (effectiveKind == CxxBraceContainerKind::InitializerList)
                    {
                        const std::string parameterName = i < selected.paramNames.size()
                            && !selected.paramNames[i].empty()
                            ? selected.paramNames[i] : std::format("#{}", i);
                        error = std::format(
                            "C++ parameter '{}' of '{}' takes std::initializer_list, which "
                            "cannot be formed from a materialized CFlat brace list; provide an "
                            "ArrayRef, IListRef or vector overload", parameterName, functionName);
                        return false;
                    }
                    std::string braceType = CxxBraceElementSpelling(*brace, targetParameter);
                    if (braceType.empty())
                    {
                        error = "brace arguments must contain values of one C++ class type";
                        return false;
                    }
                    std::string braceContainer = StripCxxRefAndCv(targetParameter);
                    while (!braceContainer.empty() && braceContainer.back() == '*')
                    {
                        braceContainer.pop_back();
                        while (!braceContainer.empty()
                               && std::isspace((unsigned char)braceContainer.back()))
                            braceContainer.pop_back();
                    }
                    const size_t dataIndex = flatParameterIndex++;
                    parameterSpellings.push_back("const " + braceType + " *");
                    const size_t countIndex = flatParameterIndex++;
                    parameterSpellings.push_back("size_t");
                    classWrapperParams[brace->argumentIndex] = { dataIndex, countIndex };
                    std::string braceCall = braceContainer + "(p" + std::to_string(dataIndex) + ", ";
                    if (effectiveKind == CxxBraceContainerKind::Vector)
                        braceCall += "p" + std::to_string(dataIndex) + " + p"
                            + std::to_string(countIndex);
                    else
                        braceCall += "p" + std::to_string(countIndex);
                    braceCall += ")";
                    callArguments.push_back(std::move(braceCall));
                    continue;
                }
                if (const std::string element = scalarBackedElement(*brace, targetParameter);
                    !element.empty())
                {
                    const size_t dataIndex = flatParameterIndex++;
                    const std::string il = "__cflat_il_" + std::to_string(i);
                    const bool list = CxxBraceContainerKindOf(targetParameter)
                        == CxxBraceContainerKind::InitializerList;
                    const std::string container = StripCxxRefAndCv(targetParameter);
                    const std::string listType = list ? container
                                                      : "std::initializer_list<" + element + ">";
                    wrapperNeedsInitializerList = true;
                    parameterSpellings.push_back("const " + element + " *");
                    wrapperBodyPrefix += CxxInitializerListOverStorage(
                        listType, il, "p" + std::to_string(dataIndex), brace->elements.size());
                    // A prvalue list binds any list parameter; a class keeps the list's own
                    // copy-list-initialization (`{il}` reaches its initializer_list constructor).
                    if (list)
                        callArguments.push_back(listType + "(" + il + ")");
                    else
                        callArguments.push_back((CxxBraceTargetElementSpelling(container).empty()
                            ? std::string() : container) + "{" + il + "}");
                    scalarBackedParams[brace->argumentIndex] = { dataIndex, element };
                    continue;
                }
                std::string braceType = CxxBraceElementSpelling(
                    *brace, targetParameter);
                if (!brace->elements.empty() && braceType.empty())
                {
                    error = "brace arguments must contain scalar values of one type";
                    return false;
                }
                // The thunk list is formed from parameters already converted to the element type,
                // so C++ never sees a narrowing the source list has: check it here.
                if (braceType == CxxBraceTargetElementSpelling(targetParameter)
                    && !CheckCxxBraceNarrowing(*brace, braceType,
                           ownerType.empty() ? functionName : ownerType + "." + memberName,
                           "parameter", error))
                    return false;
                std::string braceContainer = i < selected.paramTypes.size()
                    ? StripCxxRefAndCv(selected.paramTypes[i]) : std::string();
                while (!braceContainer.empty() && braceContainer.back() == '*')
                {
                    braceContainer.pop_back();
                    while (!braceContainer.empty()
                           && std::isspace((unsigned char)braceContainer.back()))
                        braceContainer.pop_back();
                }
                std::string braceCall = CxxBraceTargetElementSpelling(braceContainer).empty()
                    ? "{" : braceContainer + "{";
                for (size_t element = 0; element < brace->elements.size(); ++element)
                {
                    if (element != 0) braceCall += ", ";
                    parameterSpellings.push_back(braceType);
                    braceCall += "p" + std::to_string(flatParameterIndex++);
                }
                braceCall += "}";
                callArguments.push_back(std::move(braceCall));
            }
            else
            {
                if (i >= selected.paramTypes.size())
                {
                    error = "the C++ declaration has too few parameters for this call";
                    return false;
                }
                parameterSpellings.push_back(selected.paramTypes[i]);
                callArguments.push_back("p" + std::to_string(flatParameterIndex++));
            }
        }

        std::string targetCall = selected.instance
            ? callArguments.front() + "("
            : (ownerType.empty() ? selected.target : ownerSpelling + "::" + memberName) + "(";
        const size_t firstUserArgument = selected.instance ? 1u : 0u;
        for (size_t i = firstUserArgument; i < callArguments.size(); ++i)
        {
            if (i != firstUserArgument) targetCall += ", ";
            targetCall += callArguments[i];
        }
        targetCall += ")";

        std::string hashKey = functionName + ownerType + targetCall;
        for (const auto& param : parameterSpellings) hashKey += param;
        for (const auto& brace : braceArguments)
        {
            hashKey += "|brace|";
            hashKey += std::to_string(brace.parameterIndex);
            hashKey += std::to_string(brace.elements.size());
            hashKey += CxxBraceElementSpelling(brace, brace.parameterIndex < selected.paramTypes.size()
                                                       ? selected.paramTypes[brace.parameterIndex] : std::string());
        }
        const std::string wrapperName = std::format("__cflat_tpl_{:016x}", HashWrapperKey(hashKey));
        std::string wrapperSource = classWrapperParams.empty() && wrapperBodyPrefix.empty()
            && !wrapperNeedsInitializerList && !wrapperNeedsPlacementNew
            ? std::string() : "#include <cstddef>\n";
        if (wrapperNeedsInitializerList || wrapperNeedsPlacementNew)
            wrapperSource = "#include <cstddef>\n#include <initializer_list>\n";
        if (wrapperNeedsPlacementNew) wrapperSource += "#include <new>\n";
        const bool returnsVoid = TrimCxxBraceType(selected.retType) == "void";
        wrapperSource += "extern \"C\" " + std::string(returnsVoid ? "void" : "auto")
            + " " + wrapperName + "(";
        for (size_t i = 0; i < parameterSpellings.size(); ++i)
        {
            if (i != 0) wrapperSource += ", ";
            wrapperSource += parameterSpellings[i] + " p" + std::to_string(i);
        }
        wrapperSource += ")";
        if (selected.isNoexcept) wrapperSource += " noexcept";
        wrapperSource += " { " + wrapperBodyPrefix;
        if (!returnsVoid) wrapperSource += "return ";
        wrapperSource += targetCall + "; }\n";

        size_t groupIndex = static_cast<size_t>(-1);
        if (ownerType.empty())
        {
            auto it = cxxFunctionOwnerGroup_.find(functionName);
            if (it != cxxFunctionOwnerGroup_.end()) groupIndex = it->second;
        }
        else
        {
            auto it = cxxTypeOwnerGroup_.find(ownerType);
            if (it != cxxTypeOwnerGroup_.end()) groupIndex = it->second;
        }
        if (groupIndex == static_cast<size_t>(-1))
        {
            error = "the C++ declaration's import group is unavailable";
            return false;
        }
        CxxRequestGroup group = ownerType.empty()
            ? MakeCxxRequestGroup(groupIndex, {})
            : MakeCxxClassRequestGroup(groupIndex, ownerType);
        if (group.headers.empty())
        {
            error = "the C++ declaration's import group is unavailable";
            return false;
        }
        CxxRequestGroupScope groupScope(*this, &group);
        CSigEntry bound;
        std::string wrapperError;
        if (!RequestGeneratedCxxWrapper(group, wrapperSource, wrapperName, "BRACE",
                                         bound, wrapperError))
        {
            error = std::format("C++ brace argument call '{}' does not match (clang: {})",
                                ownerType.empty() ? functionName : ownerType + "." + memberName,
                                FirstCxxErrorLine(wrapperError));
            return false;
        }
        const std::string registeredName = ownerType.empty()
            ? functionName : (selected.instance ? memberName : functionName);
        bound.name = registeredName;
        RegisterCSignatures({ bound }, selected.file.empty() ? group.headers.front() : selected.file);
        bool registered = false;
        if (auto it = functionTable.find(registeredName); it != functionTable.end())
            for (const auto& symbol : it->second)
                if (symbol.External && symbol.UniqueName == wrapperName)
                { registered = true; break; }
        if (!registered)
        {
            error = "the generated C++ brace wrapper could not be registered";
            return false;
        }
        for (auto& brace : braceArguments)
        {
            auto backed = scalarBackedParams.find(brace.argumentIndex);
            if (backed == scalarBackedParams.end()) continue;
            TypeAndValue elementType;
            llvm::Type* elementTy = MapCTypeToTypeAndValue(backed->second.second, elementType, true)
                ? GetType(elementType) : nullptr;
            if (elementTy == nullptr || (!elementTy->isIntegerTy() && !elementTy->isFloatingPointTy())
                || backed->second.first >= bound.params.size())
            {
                error = std::format("C++ brace-list element type '{}' has no scalar layout",
                                    backed->second.second);
                return false;
            }
            NamedVariable data;
            if (!EmitCxxBraceBacking(brace, elementType, elementTy, bound.params[backed->second.first],
                                     ownerType.empty() ? functionName : ownerType + "." + memberName,
                                     "parameter", data, error))
                return false;
            brace.wrapperArguments.clear();
            brace.wrapperArguments.push_back(std::move(data));
        }
        for (auto& brace : braceArguments)
        {
            const std::string targetParameter = brace.parameterIndex < selected.paramTypes.size()
                ? selected.paramTypes[brace.parameterIndex] : std::string();
            if (!brace.hasCxxClassElements
                || classElementsNeedConversion(brace, targetParameter))
                continue;
            auto params = classWrapperParams.find(brace.argumentIndex);
            if (params == classWrapperParams.end()
                || params->second.first >= bound.params.size()
                || params->second.second >= bound.params.size())
            {
                error = "the generated C++ brace wrapper has an unexpected parameter layout";
                return false;
            }
            TypeAndValue elementTv;
            elementTv.TypeName = brace.cxxElementType;
            llvm::Type* elementTy = GetType(elementTv);
            if (elementTy == nullptr || !elementTy->isSized())
            {
                error = std::format("C++ brace-list element type '{}' has no sized layout",
                                    brace.cxxElementType);
                return false;
            }
            const uint64_t align = GetEffectiveAlignmentForType(brace.cxxElementType, elementTy);
            auto* arrayTy = llvm::ArrayType::get(elementTy, brace.elements.size());
            auto* array = AllocaAtEntry(arrayTy, nullptr, "cxx.brace.array", align);
            std::vector<llvm::Value*> slots;
            slots.reserve(brace.elements.size());
            for (size_t element = 0; element < brace.elements.size(); ++element)
            {
                auto* dst = builder->CreateInBoundsGEP(
                    arrayTy, array, { builder->getInt64(0), builder->getInt64(element) },
                    "cxx.brace.element");
                auto* src = brace.elements[element].Storage;
                if (src == nullptr && brace.elements[element].IsRvalue
                    && brace.elements[element].Primary != nullptr
                    && brace.elements[element].Primary->getType() == elementTy
                    && elementTy->isStructTy()
                    && HasTrivialCxxDtor(brace.cxxElementType))
                {
                    // Direct-ABI class results have a value but no sret temp. Spill them so
                    // the ordinary C++ copy constructor still receives an address.
                    src = AllocaAtEntry(elementTy, nullptr, "cxx.brace.source", align);
                    builder->CreateStore(brace.elements[element].Primary, src);
                }
                if (src == nullptr)
                {
                    error = std::format(
                        "cannot copy C++ class '{}' into a brace-list element: the element is "
                        "not an addressable lvalue or C++ call result",
                        DisplayCxxClassName(brace.cxxElementType));
                    return false;
                }
                if (!EmitCxxCopyOrMoveConstruct(
                        brace.cxxElementType, dst, src, brace.elements[element].IsExplicitMove,
                        "into a brace-list element"))
                    return false;
                slots.push_back(dst);
            }
            if (IsForeignNontrivialCxxClass(brace.cxxElementType))
                for (auto it = slots.rbegin(); it != slots.rend(); ++it)
                    RegisterOwnedStructTemp(*it, brace.cxxElementType);

            auto* dataPtr = builder->CreateInBoundsGEP(
                arrayTy, array, { builder->getInt64(0), builder->getInt64(0) },
                "cxx.brace.data");
            NamedVariable data;
            data.TypeAndValue = bound.params[params->second.first];
            data.TypeAndValue.VariableName.clear();
            data.Primary = dataPtr;
            data.BaseType = dataPtr->getType();
            data.Storage = nullptr;
            data.IsRvalue = true;

            TypeAndValue countTv = bound.params[params->second.second];
            llvm::Type* countTy = GetType(countTv);
            if (countTy == nullptr || !countTy->isIntegerTy())
            {
                error = "the generated C++ brace wrapper's size parameter is not an integer";
                return false;
            }
            NamedVariable count;
            count.TypeAndValue = countTv;
            count.TypeAndValue.VariableName.clear();
            count.Primary = llvm::ConstantInt::get(countTy, brace.elements.size());
            count.BaseType = countTy;
            count.Storage = nullptr;
            count.IsRvalue = true;
            brace.wrapperArguments.clear();
            brace.wrapperArguments.push_back(std::move(data));
            brace.wrapperArguments.push_back(std::move(count));
        }
        for (auto& brace : braceArguments)
        {
            auto backed = convertedClassBackedParams.find(brace.argumentIndex);
            if (backed == convertedClassBackedParams.end()) continue;
            TypeAndValue elementType;
            if (!MapCTypeToTypeAndValue(backed->second.second, elementType, true))
            {
                error = std::format("C++ brace-list element type '{}' has no class layout",
                                    backed->second.second);
                return false;
            }
            llvm::Type* elementTy = GetType(elementType);
            if (elementTy == nullptr || !elementTy->isSized()
                || backed->second.first >= bound.params.size())
            {
                error = std::format("C++ brace-list element type '{}' has no class layout",
                                    backed->second.second);
                return false;
            }
            const uint64_t align = GetEffectiveAlignmentForType(backed->second.second, elementTy);
            auto* arrayTy = llvm::ArrayType::get(elementTy, brace.elements.size());
            auto* array = AllocaAtEntry(arrayTy, nullptr, "cxx.brace.converted", align);
            std::vector<llvm::Value*> slots;
            slots.reserve(brace.elements.size());
            for (size_t element = 0; element < brace.elements.size(); ++element)
            {
                auto* dst = builder->CreateInBoundsGEP(
                    arrayTy, array, { builder->getInt64(0), builder->getInt64(element) },
                    "cxx.brace.converted.element");
                slots.push_back(dst);
            }
            if (IsForeignNontrivialCxxClass(elementType.TypeName))
                for (auto it = slots.rbegin(); it != slots.rend(); ++it)
                    RegisterOwnedStructTemp(*it, elementType.TypeName);
            auto* dataPtr = builder->CreateInBoundsGEP(
                arrayTy, array, { builder->getInt64(0), builder->getInt64(0) },
                "cxx.brace.converted.data");
            NamedVariable data;
            data.TypeAndValue = bound.params[backed->second.first];
            data.TypeAndValue.VariableName.clear();
            data.Primary = dataPtr;
            data.BaseType = dataPtr->getType();
            data.Storage = nullptr;
            data.IsRvalue = true;
            brace.wrapperArguments.clear();
            brace.wrapperArguments.push_back(std::move(data));
            brace.wrapperArguments.insert(brace.wrapperArguments.end(), brace.elements.begin(),
                                          brace.elements.end());
        }
        ExpandCxxBraceArguments(arguments, braceArguments);
        return true;
}

