#pragma once
#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

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

struct Diagnostic
{
    Range range;
    int severity = 1;  // 1=Error, 2=Warning, 3=Information, 4=Hint
    std::string message;
    std::string source = "mycompiler";
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
    return {
        {"range", rangeToJson(d.range)},
        {"severity", d.severity},
        {"message", d.message},
        {"source", d.source}
    };
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
