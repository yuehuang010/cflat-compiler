#include "LspSymbolIndex.h"
#include <algorithm>

void LspSymbolIndex::Register(SymbolKind kind, const std::string& name, const std::string& file,
                               int line, int col, const std::string& sig,
                               const std::vector<std::string>& members)
{
    SymbolDef def;
    def.name = name;
    def.kind = kind;
    def.file = file;
    def.line = line;
    def.column = col;
    def.signatureMarkdown = sig;
    def.memberNames = members;
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
}

void LspSymbolIndex::RemapFile(const std::string& fromFile, const std::string& toFile)
{
    for (auto& [name, def] : symbols_)
        if (def.file == fromFile)
            def.file = toFile;
}
