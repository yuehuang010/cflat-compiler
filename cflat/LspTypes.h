#pragma once
#include <string>
#include <vector>
#include <optional>
#include "platform/JsonCompat.h"

namespace lsp {

struct Position
{
    int line;       // 0-based
    int character;  // 0-based
};

struct Range
{
    Position start;
    Position end;
};

struct Location
{
    std::string uri;
    Range range;
};

// LSP DiagnosticSeverity values.
enum DiagnosticSeverity
{
    DiagnosticSeverityError       = 1,
    DiagnosticSeverityWarning     = 2,
    DiagnosticSeverityInformation = 3,
    DiagnosticSeverityHint        = 4
};

// LSP DiagnosticTag values (textDocument/publishDiagnostics).
enum DiagnosticTag
{
    DiagnosticTagUnnecessary = 1,  // grayed-out / faded rendering for unused code
    DiagnosticTagDeprecated  = 2
};

// LSP CompletionItemKind values (subset used by this server).
enum CompletionItemKind
{
    CompletionItemKindFunction  = 3,
    CompletionItemKindField     = 5,
    CompletionItemKindInterface = 8,
    CompletionItemKindModule    = 9,
    CompletionItemKindStruct    = 22,
    CompletionItemKindTypeParam = 25
};

struct Diagnostic
{
    Range range;
    int severity = 1;  // 1=Error, 2=Warning, 3=Information, 4=Hint
    std::string message;
    std::string source = "cflat";
    std::vector<int> tags;  // DiagnosticTag values; empty when none
};

struct TextEdit
{
    Range range;
    std::string newText;
};

struct CompletionItem
{
    std::string label;
    std::optional<int> kind;  // CompletionItemKind
    std::optional<std::string> detail;
    std::optional<std::string> documentation;
    std::optional<std::string> insertText;
};

// --- JSON serialization helpers ---

inline nlohmann::json positionToJson(const Position& p)
{
    return { {"line", p.line}, {"character", p.character} };
}

inline nlohmann::json rangeToJson(const Range& r)
{
    return { {"start", positionToJson(r.start)}, {"end", positionToJson(r.end)} };
}

inline nlohmann::json locationToJson(const Location& loc)
{
    return { {"uri", loc.uri}, {"range", rangeToJson(loc.range)} };
}

inline nlohmann::json diagnosticToJson(const Diagnostic& d)
{
    nlohmann::json obj = {
        {"range", rangeToJson(d.range)},
        {"severity", d.severity},
        {"message", d.message},
        {"source", d.source}
    };
    if (!d.tags.empty())
        obj["tags"] = d.tags;
    return obj;
}

inline nlohmann::json completionItemToJson(const CompletionItem& item)
{
    nlohmann::json obj = { {"label", item.label} };
    if (item.kind)          obj["kind"]          = *item.kind;
    if (item.detail)        obj["detail"]        = *item.detail;
    if (item.documentation) obj["documentation"] = *item.documentation;
    if (item.insertText)    obj["insertText"]    = *item.insertText;
    return obj;
}

// --- JSON parsing helpers ---

inline Position parsePosition(const nlohmann::json& obj)
{
    return Position{
        obj.value("line",      0),
        obj.value("character", 0)
    };
}

inline Range parseRange(const nlohmann::json& obj)
{
    Range r{};
    if (obj.contains("start") && obj["start"].is_object()) r.start = parsePosition(obj["start"]);
    if (obj.contains("end")   && obj["end"].is_object())   r.end   = parsePosition(obj["end"]);
    return r;
}

} // namespace lsp
