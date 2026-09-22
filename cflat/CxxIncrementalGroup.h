#pragma once

#include "CClangExtract.h"

#include <memory>
#include <string>
#include <vector>

class CxxIncrementalGroup
{
public:
    static std::unique_ptr<CxxIncrementalGroup> Create(
        const std::vector<std::string>& args, const std::string& headerSource,
        bool verbose, std::string& error);

    ~CxxIncrementalGroup();

    bool PrecheckSpelling(const std::string& spelling, std::string& error);
    bool HasPrefixSource(const std::string& source) const;
    std::string UnseenPrefixSource(const std::string& source) const;
    void RememberPrefixSource(const std::string& source);
    bool ParseRequest(const cflat_cinterop::ExtractRequest& req,
                      const std::string& source,
                      cflat_cinterop::ExtractResult& out,
                      std::string& error);

private:
    struct Impl;
    explicit CxxIncrementalGroup(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
