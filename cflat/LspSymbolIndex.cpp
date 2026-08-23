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

void LspSymbolIndex::RemoveFunctionAliases(const std::string& name)
{
    auto it = symbols_.find(name);
    if (it == symbols_.end() || it->second.kind != SymbolKind::Function) return;
    SymbolDef& def = it->second;
    auto isAlias = [](const std::string& sig) { return sig.starts_with("#define "); };
    if (isAlias(def.signatureMarkdown))
    {
        auto real = std::find_if(def.overloadSignatures.begin(), def.overloadSignatures.end(),
                                 [&](const std::string& sig) { return !isAlias(sig); });
        if (real == def.overloadSignatures.end())
        {
            symbols_.erase(it);
            return;
        }
        def.signatureMarkdown = *real;
        def.overloadSignatures.erase(real);
    }
    std::erase_if(def.overloadSignatures, isAlias);
}

void LspSymbolIndex::RegisterDefinition(const SymbolDef& def)
{
    if (def.name.empty()) return;
    symbols_[def.name] = def;
}

void LspSymbolIndex::RegisterFunctionRange(const std::string& name, const std::string& file,
                                           int startLine, int endLine)
{
    if (name.empty() || file.empty() || startLine <= 0 || endLine < startLine)
        return;
    functionRanges_.push_back({name, file, startLine, endLine});
}

void LspSymbolIndex::MergeFrom(const LspSymbolIndex& other)
{
    for (const auto& [name, def] : other.symbols_)
        if (!symbols_.contains(name))
            symbols_.emplace(name, def);
    for (const auto& [name, info] : other.variables_)
        if (!variables_.contains(name))
            variables_.emplace(name, info);
    functionRanges_.insert(functionRanges_.end(), other.functionRanges_.begin(), other.functionRanges_.end());
    candidates_.insert(candidates_.end(), other.candidates_.begin(), other.candidates_.end());
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
        if (name.starts_with(prefix))
            results.push_back(&def);
    }
    return results;
}

void LspSymbolIndex::Clear()
{
    symbols_.clear();
    variables_.clear();
    functionRanges_.clear();
    candidates_.clear();
}

std::vector<const FunctionRange*> LspSymbolIndex::FunctionsEnclosing(const std::string& file, int line) const
{
    std::vector<const FunctionRange*> results;
    for (const auto& range : functionRanges_)
        if (range.file == file && line >= range.startLine && line <= range.endLine)
            results.push_back(&range);
    return results;
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

void LspSymbolIndex::MergeVariablesFrom(const LspSymbolIndex& other)
{
    // Union, new-wins: a partial parse truncates registration early, so wholesale
    // replacement would drop variables the cached index legitimately still has.
    for (const auto& [name, info] : other.variables_)
        variables_[name] = info;
}

void LspSymbolIndex::RemapFile(const std::string& fromFile, const std::string& toFile)
{
    for (auto& [name, def] : symbols_)
        if (def.file == fromFile)
            def.file = toFile;
    for (auto& [name, info] : variables_)
        if (info.file == fromFile)
            info.file = toFile;
    for (auto& range : functionRanges_)
        if (range.file == fromFile)
            range.file = toFile;
    for (auto& cand : candidates_)
        if (cand.file == fromFile)
            cand.file = toFile;
}
