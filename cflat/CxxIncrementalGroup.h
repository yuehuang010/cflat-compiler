#pragma once

#include "CClangExtract.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

class CxxIncrementalGroup
{
public:
    static std::unique_ptr<CxxIncrementalGroup> Create(
        const std::vector<std::string>& args, const std::string& headerSource,
        bool verbose, std::string& error, bool tolerateDiagnostics = false,
        const cflat_cinterop::ExtractRequest* macroReq = nullptr,
        cflat_cinterop::ExtractResult* macroOut = nullptr);

    ~CxxIncrementalGroup();

    bool PrecheckSpelling(const std::string& spelling, std::string& error);
    bool HasPrefixSource(const std::string& source) const;
    std::string UnseenPrefixSource(const std::string& source) const;
    void RememberPrefixSource(const std::string& source);
    bool HasDemandChunkSource(const std::string& source) const;
    bool HasDemandHeaderHarvest() const;
    bool HarvestHeader(const cflat_cinterop::ExtractRequest& req,
                       cflat_cinterop::ExtractResult& out,
                       std::string& error);
    bool ParseRequest(const cflat_cinterop::ExtractRequest& req,
                      const std::string& source,
                      cflat_cinterop::ExtractResult& out,
                      std::string& error);
    // Every clang error and note the newest ParseRequest reported (empty on a clean parse).
    const std::string& LastRequestDiagnostics() const;
    // Linkage symbols of the constructors whose failed body refused the newest ParseRequest or
    // CheckDemand (the helper selected one), the extractor's key for the member.
    const std::vector<std::string>& LastRefusedConstructors() const;
    // Definitions harvests this group recorded for its one demand pass (0: nothing to emit).
    unsigned DemandChunks() const;
    // 0: another group owns the symbol, 1: valid demand, -1: clang body error.
    int CheckDemand(const std::string& symbol, std::string& error);
    void RestorePoisonedBodies(const std::vector<std::pair<std::string, std::string>>& verdicts);
    // The group's demand pass: bitcode defining what of `demand` this group can provide.
    bool EmitDemandCompanion(const std::vector<std::string>& demand, std::string& bitcode,
                             cflat_cinterop::CxxDemandStats& stats, std::string& error,
                             llvm::LLVMContext* targetContext = nullptr,
                             std::unique_ptr<llvm::Module>* moduleOut = nullptr);
    // The bytes clang read for each entry of the harvested includedFiles, same order; a null
    // data() marks a file whose buffer is unavailable. Valid while this group lives.
    std::vector<std::string_view> IncludedFileBuffers() const;
    // Of `files`, those an #include in this TU brought in that no header in `roots` reaches: what
    // a TU of `roots` alone would not declare. A root is a path, or `<name>` for an angled
    // include (the request prologue's `<new>`). Empty when a root is not a header of this TU.
    std::unordered_set<std::string> UnreachableFiles(const std::vector<std::string>& roots,
                                                     const std::vector<std::string>& files) const;

private:
    struct Impl;
    explicit CxxIncrementalGroup(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
