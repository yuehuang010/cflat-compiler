#pragma once
#include <string>
#include <vector>
#include <unordered_map>

enum class SymbolKind { Function, Struct, Interface, Namespace, TypeAlias, Field };

struct SymbolDef
{
    std::string name;
    SymbolKind kind;
    std::string file;
    int line;      // 1-based (ANTLR convention)
    int column;    // 0-based
    std::string signatureMarkdown;
    std::vector<std::string> memberNames;
};

class LspSymbolIndex
{
public:
    void Register(SymbolKind kind, const std::string& name, const std::string& file,
                  int line, int col, const std::string& sig,
                  const std::vector<std::string>& members = {});
    const SymbolDef* Lookup(const std::string& name) const;
    std::vector<const SymbolDef*> LookupPrefix(const std::string& prefix) const;
    void Clear();
    // Replace all occurrences of fromFile with toFile in registered symbol locations.
    void RemapFile(const std::string& fromFile, const std::string& toFile);

    void RegisterVariable(const std::string& varName, const std::string& typeName);
    const std::string* LookupVariableType(const std::string& varName) const;

    size_t SymbolCount() const { return symbols_.size(); }

private:
    std::unordered_map<std::string, SymbolDef> symbols_;
    std::unordered_map<std::string, std::string> variableTypes_;
};
