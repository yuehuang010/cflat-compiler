#pragma once
#include <string>
#include <vector>
#include <unordered_map>

enum class SymbolKind { Function, Struct, Interface, Namespace, TypeAlias, Field, Variable };

struct VariableInfo
{
    std::string typeName;
    std::string file;
    int line = 0;    // 1-based; 0 means no location recorded
    int column = 0;  // 0-based
};

struct SymbolDef
{
    std::string name;
    SymbolKind kind;
    std::string file;
    int line;      // 1-based (ANTLR convention)
    int column;    // 0-based
    std::string signatureMarkdown;
    std::string docComment;
    std::vector<std::string> memberNames;
    // Signatures of additional overloads sharing this name. The index is keyed
    // by name, so without this only the last-registered overload is visible to
    // --symbol queries and hover (e.g. Math.sin showed only the float form).
    std::vector<std::string> overloadSignatures;
};

// A definition that is a candidate for the "unused code" check. Recorded during
// analysis at each declaration site; consumed post-analysis by the LSP server,
// which cross-references it against an identifier-occurrence scan of the document.
struct UnusedCandidate
{
    std::string name;
    SymbolKind kind = SymbolKind::Variable;
    std::string file;
    int line = 0;   // 1-based
    int col = 0;    // 0-based
    bool isExported = false;     // function/global with external linkage - never flagged
    bool hasDestructor = false;  // local whose type has a destructor (RAII) - never flagged
};

class LspSymbolIndex
{
public:
    void Register(SymbolKind kind, const std::string& name, const std::string& file,
                  int line, int col, const std::string& sig,
                  const std::vector<std::string>& members = {},
                  const std::string& docComment = {});
    const SymbolDef* Lookup(const std::string& name) const;
    std::vector<const SymbolDef*> LookupPrefix(const std::string& prefix) const;
    void Clear();
    // Replace all occurrences of fromFile with toFile in registered symbol locations.
    void RemapFile(const std::string& fromFile, const std::string& toFile);

    void RegisterVariable(const std::string& varName, const std::string& typeName);
    // Overload: also records the variable's source location for go-to-definition.
    void RegisterVariable(const std::string& varName, const std::string& typeName,
                          const std::string& file, int line, int column);
    const std::string* LookupVariableType(const std::string& varName) const;
    const VariableInfo* LookupVariable(const std::string& varName) const;

    // Unused-code candidates (declarations that may turn out to be unreferenced).
    void RegisterCandidate(const UnusedCandidate& cand);
    const std::vector<UnusedCandidate>& Candidates() const { return candidates_; }

    // All registered symbols, keyed by name. Used by the unused-import check to map
    // a resolved import file to the set of symbol names it defines.
    const std::unordered_map<std::string, SymbolDef>& Symbols() const { return symbols_; }

    size_t SymbolCount() const { return symbols_.size(); }

private:
    std::unordered_map<std::string, SymbolDef> symbols_;
    std::unordered_map<std::string, VariableInfo> variables_;
    std::vector<UnusedCandidate> candidates_;
};
