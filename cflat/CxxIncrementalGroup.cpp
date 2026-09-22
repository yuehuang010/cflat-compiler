#include "CxxIncrementalGroup.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Interpreter/Interpreter.h"
#include "clang/Interpreter/PartialTranslationUnit.h"
#include "clang/Sema/Sema.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/TargetSelect.h"

#include <algorithm>
#include <format>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace
{
    class CountingDiagnosticConsumer : public clang::DiagnosticConsumer
    {
    public:
        unsigned errors = 0;
        std::string firstError;

        void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                              const clang::Diagnostic& info) override
        {
            if (level < clang::DiagnosticsEngine::Error) return;
            ++errors;
            if (firstError.empty())
            {
                llvm::SmallString<256> text;
                info.FormatDiagnostic(text);
                firstError = text.str().str();
            }
        }
    };

    struct DiagnosticScope
    {
        clang::DiagnosticsEngine& diagnostics;
        clang::DiagnosticConsumer* previous;
        CountingDiagnosticConsumer consumer;

        explicit DiagnosticScope(clang::DiagnosticsEngine& d)
            : diagnostics(d), previous(d.getClient())
        {
            diagnostics.Reset(/*soft*/ true);
            diagnostics.setSuppressAllDiagnostics(false);
            diagnostics.setClient(&consumer, /*ShouldOwnClient*/ false);
        }

        ~DiagnosticScope()
        {
            diagnostics.setClient(previous, /*ShouldOwnClient*/ false);
            diagnostics.setSuppressAllDiagnostics(true);
            diagnostics.Reset(/*soft*/ true);
        }
    };

    std::string ErrorText(llvm::Error error)
    {
        return llvm::toString(std::move(error));
    }

    std::vector<std::string> InterpreterArgs(const std::vector<std::string>& args)
    {
        std::vector<std::string> result;
        result.reserve(args.size());
        for (size_t i = 0; i < args.size(); ++i)
        {
            if (args[i] == "-fsyntax-only") continue;
            if (args[i] == "-x" && i + 1 < args.size())
            {
                ++i;
                continue;
            }
            result.push_back(args[i]);
        }
        return result;
    }
}

struct CxxIncrementalGroup::Impl
{
    std::unique_ptr<clang::Interpreter> interpreter;
    clang::TranslationUnitDecl* headerRoot = nullptr;
    std::unordered_set<std::string> prefixSources;
    bool verbose = false;
    std::unordered_map<std::string, cflat_cinterop::ExtractResult> wrapperResults;
    std::unordered_map<std::string, cflat_cinterop::ExtractResult> typeResults;
    std::unordered_map<std::string, clang::TranslationUnitDecl*> typeRoots;
};

CxxIncrementalGroup::CxxIncrementalGroup(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

CxxIncrementalGroup::~CxxIncrementalGroup()
{
    // Clang's interpreter teardown is unsafe after CodeGen has visited its live AST.
    if (impl_ != nullptr) (void)impl_->interpreter.release();
}

std::unique_ptr<CxxIncrementalGroup> CxxIncrementalGroup::Create(
    const std::vector<std::string>& args, const std::string& headerSource,
    bool verbose, std::string& error)
{
    std::vector<std::string> storage = InterpreterArgs(args);
    std::vector<const char*> cargs;
    cargs.reserve(storage.size());
    for (const std::string& arg : storage) cargs.push_back(arg.c_str());

    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmPrinters();
    llvm::InitializeAllAsmParsers();

    clang::IncrementalCompilerBuilder builder;
    builder.SetCompilerArgs(cargs);
    auto compiler = builder.CreateCpp();
    if (!compiler)
    {
        error = ErrorText(compiler.takeError());
        return nullptr;
    }
    auto interpreter = clang::Interpreter::create(std::move(*compiler));
    if (!interpreter)
    {
        error = ErrorText(interpreter.takeError());
        return nullptr;
    }

    auto impl = std::make_unique<Impl>();
    impl->interpreter = std::move(*interpreter);
    impl->verbose = verbose;
    {
        DiagnosticScope diagnostics(impl->interpreter->getCompilerInstance()->getDiagnostics());
        auto ptu = impl->interpreter->Parse(headerSource);
        if (!ptu)
        {
            error = ErrorText(ptu.takeError());
            if (error.empty()) error = diagnostics.consumer.firstError;
            return nullptr;
        }
        if (diagnostics.consumer.errors != 0)
        {
            error = diagnostics.consumer.firstError;
            return nullptr;
        }
        impl->headerRoot = (*ptu).TUPart;
    }
    return std::unique_ptr<CxxIncrementalGroup>(
        new CxxIncrementalGroup(std::move(impl)));
}

bool CxxIncrementalGroup::PrecheckSpelling(const std::string& spelling, std::string& error)
{
    static unsigned nextId = 0;
    const std::string typedefName = "__cflat_precheck_" + std::to_string(nextId++);
    DiagnosticScope diagnostics(impl_->interpreter->getCompilerInstance()->getDiagnostics());
    auto ptu = impl_->interpreter->Parse(
        "typedef " + spelling + " " + typedefName + ";\n");
    if (!ptu)
    {
        // Consume the Expected either way; an unchecked one aborts under LLVM assertions.
        const std::string parseError = ErrorText(ptu.takeError());
        error = diagnostics.consumer.firstError;
        if (error.empty()) error = parseError;
        return false;
    }

    clang::TypedefNameDecl* typedefDecl = nullptr;
    for (clang::Decl* decl : (*ptu).TUPart->decls())
    {
        auto* candidate = llvm::dyn_cast<clang::TypedefNameDecl>(decl);
        if (candidate != nullptr && candidate->getNameAsString() == typedefName)
        {
            typedefDecl = candidate;
            break;
        }
    }
    if (typedefDecl == nullptr)
    {
        error = std::format("C++ type '{}' could not be prechecked", spelling);
        return false;
    }

    clang::QualType type = typedefDecl->getUnderlyingType().getCanonicalType();
    clang::Sema& sema = impl_->interpreter->getCompilerInstance()->getSema();
    clang::Sema::SFINAETrap trap(sema);
    const bool incomplete = sema.RequireCompleteType(
        typedefDecl->getLocation(), type, clang::diag::err_incomplete_type);
    if (incomplete || trap.hasErrorOccurred() || type->isIncompleteType())
    {
        error = diagnostics.consumer.firstError;
        if (error.empty()) error = std::format("C++ type '{}' is incomplete", spelling);
        return false;
    }
    return diagnostics.consumer.errors == 0;
}

bool CxxIncrementalGroup::HasPrefixSource(const std::string& source) const
{
    return !source.empty() && impl_->prefixSources.count(source) != 0;
}

std::string CxxIncrementalGroup::UnseenPrefixSource(const std::string& source) const
{
    std::string result = source;
    std::vector<std::string> known(impl_->prefixSources.begin(), impl_->prefixSources.end());
    std::sort(known.begin(), known.end(), [](const auto& a, const auto& b) {
        return a.size() > b.size();
    });
    for (const std::string& prefix : known)
    {
        size_t pos = 0;
        while (!prefix.empty() && (pos = result.find(prefix, pos)) != std::string::npos)
            result.erase(pos, prefix.size());
    }
    return result;
}

void CxxIncrementalGroup::RememberPrefixSource(const std::string& source)
{
    if (!source.empty()) impl_->prefixSources.insert(source);
}

bool CxxIncrementalGroup::ParseRequest(const cflat_cinterop::ExtractRequest& req,
                                       const std::string& source,
                                       cflat_cinterop::ExtractResult& out,
                                       std::string& error)
{
    std::string typeKey;
    for (const auto& request : req.cxxTypeRequests)
        typeKey += request.cflatName + "\n";
    const std::string wrapperName = req.cxxFunctionWrapperNames.size() == 1
        ? req.cxxFunctionWrapperNames.front() : std::string{};
    if (!wrapperName.empty())
    {
        auto found = impl_->wrapperResults.find(wrapperName);
        if (found != impl_->wrapperResults.end())
        {
            out = found->second;
            return true;
        }
    }
    DiagnosticScope diagnostics(impl_->interpreter->getCompilerInstance()->getDiagnostics());
    auto ptu = impl_->interpreter->Parse(source);
    if (!ptu)
    {
        // Consume the Expected either way; an unchecked one aborts under LLVM assertions.
        const std::string parseError = ErrorText(ptu.takeError());
        error = diagnostics.consumer.firstError;
        if (error.empty()) error = parseError;
        return false;
    }
    cflat_cinterop::ExtractRequest effective = req;
    if (!wrapperName.empty())
    {
        effective.emitDefinitions = true;
        effective.assumeInlineDefinitions = false;
    }
    std::vector<clang::TranslationUnitDecl*> extraRoots;
    if (req.emitDefinitions && !typeKey.empty())
    {
        auto found = impl_->typeRoots.find(typeKey);
        if (found != impl_->typeRoots.end()) extraRoots.push_back(found->second);
    }
    const bool harvested = cflat_cinterop::ExtractCxxIncremental(
        effective, *impl_->interpreter->getCompilerInstance(), (*ptu).TUPart,
        impl_->headerRoot, extraRoots,
        (*ptu).TheModule.get(), out, error);
    if (harvested && !typeKey.empty() && !req.emitDefinitions)
    {
        impl_->typeResults[typeKey] = out;
        impl_->typeRoots[typeKey] = (*ptu).TUPart;
    }
    if (harvested && !typeKey.empty() && req.emitDefinitions)
    {
        auto prior = impl_->typeResults.find(typeKey);
        if (prior != impl_->typeResults.end())
        {
            cflat_cinterop::ExtractResult merged = prior->second;
            auto appendRecords = [&](const auto& source) {
                for (const auto& record : source)
                {
                    const bool exists = std::any_of(merged.records.begin(), merged.records.end(),
                        [&](const auto& old) {
                            return (!record.name.empty() && old.name == record.name)
                                || (!record.canonicalCtype.empty()
                                    && old.canonicalCtype == record.canonicalCtype);
                        });
                    if (exists)
                    {
                        for (auto& old : merged.records)
                            if ((!record.name.empty() && old.name == record.name)
                                || (!record.canonicalCtype.empty()
                                    && old.canonicalCtype == record.canonicalCtype))
                            {
                                old = record;
                                break;
                            }
                    }
                    else merged.records.push_back(record);
                }
            };
            auto appendSigs = [&](const auto& source) {
                for (const auto& sig : source)
                {
                    const bool exists = std::any_of(merged.sigs.begin(), merged.sigs.end(),
                        [&](const auto& old) {
                            return old.name == sig.name && old.linkageName == sig.linkageName;
                        });
                    if (!exists) merged.sigs.push_back(sig);
                }
            };
            auto appendTemplates = [&](const auto& source) {
                for (const auto& templ : source)
                {
                    const bool exists = std::any_of(merged.functionTemplates.begin(),
                                                    merged.functionTemplates.end(),
                        [&](const auto& old) {
                            return old.name == templ.name
                                && old.cxxSpelling == templ.cxxSpelling;
                        });
                    if (!exists) merged.functionTemplates.push_back(templ);
                }
            };
            appendRecords(out.records);
            appendSigs(out.sigs);
            appendTemplates(out.functionTemplates);
            merged.bitcode = std::move(out.bitcode);
            merged.emittedDefinitions = out.emittedDefinitions;
            if (!out.firstError.empty()) merged.firstError = std::move(out.firstError);
            if (!out.invalidCxxTypeRequestError.empty())
                merged.invalidCxxTypeRequestError = std::move(out.invalidCxxTypeRequestError);
            merged.droppedCxxDefaultWrappers.insert(
                merged.droppedCxxDefaultWrappers.end(),
                out.droppedCxxDefaultWrappers.begin(), out.droppedCxxDefaultWrappers.end());
            merged.weakPromoteSymbols.insert(merged.weakPromoteSymbols.end(),
                                             out.weakPromoteSymbols.begin(),
                                             out.weakPromoteSymbols.end());
            out = std::move(merged);
        }
    }
    if (harvested && !wrapperName.empty()) impl_->wrapperResults.emplace(wrapperName, out);
    return harvested;
}
