#include "LspSymbolIndex.h"
#include <algorithm>

void LspSymbolIndex::Register(SymbolKind kind, const std::string& name, const std::string& file,
                               int line, int col, const std::string& sig,
                               const std::vector<std::string>& members,
                               const std::string& docComment)
{
    SymbolDef def;
    def.name = name;
    def.kind = kind;
    def.file = file;
    def.line = line;
    def.column = col;
    def.signatureMarkdown = sig;
    def.docComment = docComment;
    def.memberNames = members;

    // Function overloads share one index slot. The new registration stays the
    // primary (last-writer-wins, as before), but the signatures it displaces
    // are carried forward so queries can report the full overload set.
    auto it = symbols_.find(name);
    if (it != symbols_.end() && kind == SymbolKind::Function && it->second.kind == SymbolKind::Function)
    {
        SymbolDef& prev = it->second;
        def.overloadSignatures = std::move(prev.overloadSignatures);
        if (!prev.signatureMarkdown.empty() && prev.signatureMarkdown != sig)
        {
            auto& sigs = def.overloadSignatures;
            if (std::find(sigs.begin(), sigs.end(), prev.signatureMarkdown) == sigs.end())
                sigs.push_back(prev.signatureMarkdown);
        }
        // The new primary may itself have been displaced earlier (re-registration).
        std::erase(def.overloadSignatures, sig);
        if (def.docComment.empty())
            def.docComment = prev.docComment;
    }

    symbols_[name] = std::move(def);
}

const SymbolDef* LspSymbolIndex::Lookup(const std::string& name) const
{
    auto it = symbols_.find(name);
    return (it != symbols_.end()) ? &it->second : nullptr;
}

std::vector<const SymbolDef*> LspSymbolIndex::LookupPrefix(const std::string& prefix) const
{
    std::vector<const SymbolDef*> results;
    for (const auto& [name, def] : symbols_)
    {
        if (name.size() >= prefix.size() && name.compare(0, prefix.size(), prefix) == 0)
            results.push_back(&def);
    }
    return results;
}

void LspSymbolIndex::Clear()
{
    symbols_.clear();
    variables_.clear();
    candidates_.clear();
}

void LspSymbolIndex::RegisterCandidate(const UnusedCandidate& cand)
{
    if (cand.name.empty()) return;
    candidates_.push_back(cand);
}

void LspSymbolIndex::RegisterVariable(const std::string& varName, const std::string& typeName)
{
    if (varName.empty() || typeName.empty()) return;
    // Preserve any previously-recorded location; only update the type.
    auto& info = variables_[varName];
    info.typeName = typeName;
}

void LspSymbolIndex::RegisterVariable(const std::string& varName, const std::string& typeName,
                                      const std::string& file, int line, int column)
{
    if (varName.empty()) return;
    auto& info = variables_[varName];
    if (!typeName.empty()) info.typeName = typeName;
    info.file = file;
    info.line = line;
    info.column = column;
}

const std::string* LspSymbolIndex::LookupVariableType(const std::string& varName) const
{
    auto it = variables_.find(varName);
    if (it == variables_.end() || it->second.typeName.empty()) return nullptr;
    return &it->second.typeName;
}

const VariableInfo* LspSymbolIndex::LookupVariable(const std::string& varName) const
{
    auto it = variables_.find(varName);
    return (it != variables_.end()) ? &it->second : nullptr;
}

void LspSymbolIndex::RemapFile(const std::string& fromFile, const std::string& toFile)
{
    for (auto& [name, def] : symbols_)
        if (def.file == fromFile)
            def.file = toFile;
    for (auto& [name, info] : variables_)
        if (info.file == fromFile)
            info.file = toFile;
    for (auto& cand : candidates_)
        if (cand.file == fromFile)
            cand.file = toFile;
}
