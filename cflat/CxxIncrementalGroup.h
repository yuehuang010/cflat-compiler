#pragma once

#include "CClangExtract.h"

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

class CxxIncrementalGroup
{
public:
    static std::unique_ptr<CxxIncrementalGroup> Create(
        const std::vector<std::string>& args, const std::string& headerSource,
        bool verbose, std::string& error, bool tolerateDiagnostics = false);

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
    // Definitions harvests this group recorded for its one demand pass (0: nothing to emit).
    unsigned DemandChunks() const;
    // The group's demand pass: bitcode defining what of `demand` this group can provide.
    bool EmitDemandCompanion(const std::vector<std::string>& demand, std::string& bitcode,
                             cflat_cinterop::CxxDemandStats& stats, std::string& error);
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
