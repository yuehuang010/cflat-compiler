#include "SymbolQuery.h"
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <vector>
#include <cctype>
#include <chrono>
#include <fstream>
#include <format>
#include <unordered_set>
#include <cstdlib>
#include <utility>

#include "LLVMBackend.h"
#include "TypeMangling.h"
#include "LspSymbolIndex.h"

// ---- --symbol query mode -------------------------------------------------
// A lightweight "IDE quick search" over the symbol index that a real analysis
// pass produces (the same index that drives LSP hover / go-to-definition). For
// each search term an exact (or case-insensitive) name match prints detailed
// symbol info; a miss falls back to substring / edit-distance matching and
// suggests the closest symbols. The point is to give an agent the API-discovery
// surface a human gets from an editor's symbol search.

static std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static const char* SymbolKindName(SymbolKind k)
{
    switch (k)
    {
        case SymbolKind::Function:  return "function";
        case SymbolKind::Struct:    return "struct";
        case SymbolKind::Interface: return "interface";
        case SymbolKind::Namespace: return "namespace";
        case SymbolKind::TypeAlias: return "type alias";
        case SymbolKind::Field:     return "field";
        case SymbolKind::Variable:  return "variable";
    }
    return "symbol";
}

static std::string SymbolDisplayName(const SymbolDef& def)
{
    return def.displayName.empty() ? def.name : def.displayName;
}

// Case-insensitive Levenshtein edit distance.
static int EditDistance(const std::string& a, const std::string& b)
{
    const size_t n = a.size(), m = b.size();
    std::vector<int> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = (int)j;
    for (size_t i = 1; i <= n; ++i)
    {
        cur[0] = (int)i;
        for (size_t j = 1; j <= m; ++j)
        {
            int cost = (std::tolower((unsigned char)a[i - 1]) ==
                        std::tolower((unsigned char)b[j - 1])) ? 0 : 1;
            cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost });
        }
        std::swap(prev, cur);
    }
    return prev[m];
}

// Print full detail for an exact symbol hit, including any members - methods and
// fields are registered under "<Type>.<member>", so we scan the index for that prefix.
static void PrintSymbolDetail(const LspSymbolIndex& index, const SymbolDef& def)
{
    const std::string displayName = SymbolDisplayName(def);
    std::cout << std::format("{}  ({})\n", displayName, SymbolKindName(def.kind));
    if (!def.signatureMarkdown.empty() && def.signatureMarkdown != displayName)
        std::cout << std::format("  {}\n", def.signatureMarkdown);
    for (const auto& sig : def.overloadSignatures)
        std::cout << std::format("  {}\n", sig);
    if (def.line > 0 && !def.file.empty())
        std::cout << std::format("  defined: {}:{}\n", def.file, def.line);
    if (!def.docComment.empty())
        std::cout << std::format("  doc: {}\n", def.docComment);

    const std::string prefix = def.name + ".";
    std::vector<const SymbolDef*> members;
    for (const auto& [name, m] : index.Symbols())
        if (name.starts_with(prefix) && name.size() > prefix.size())
            members.push_back(&m);
    std::sort(members.begin(), members.end(),
              [](const SymbolDef* a, const SymbolDef* b) { return a->name < b->name; });
    if (!members.empty())
    {
        std::cout << "  members:\n";
        for (const auto* m : members)
        {
            std::string shortName = m->name.substr(prefix.size());
            std::cout << "    " << SymbolDisplayName(*m);
            if (!m->signatureMarkdown.empty() && m->signatureMarkdown != shortName)
                std::cout << "  :  " << m->signatureMarkdown;
            std::cout << "\n";
            for (const auto& sig : m->overloadSignatures)
                std::cout << std::format("    {}  :  {}\n", SymbolDisplayName(*m), sig);
        }
    }
}

// Print "did you mean" suggestions for a term with no exact match. Substring hits
// rank ahead of edit-distance hits; prefix and shorter names rank best within each band.
static void PrintSymbolSuggestions(const LspSymbolIndex& index, const std::string& term)
{
    struct Suggestion { int score; const SymbolDef* def; };
    const std::string lcq = ToLower(term);
    const int threshold = std::max(2, (int)term.size() / 2);

    std::vector<Suggestion> hits;
    for (const auto& [name, def] : index.Symbols())
    {
        const std::string lcs = ToLower(name);
        int score;
        size_t pos = lcs.find(lcq);
        if (pos != std::string::npos)
            score = (int)pos * 2 + (int)(name.size() - term.size());  // substring band: 0..
        else
        {
            int ed = EditDistance(lcq, lcs);
            if (ed > threshold) continue;
            score = 1000 + ed;  // edit-distance band: ranks after every substring hit
        }
        hits.push_back({ score, &def });
    }

    if (hits.empty())
    {
        std::cout << "  (no similar symbols found)\n";
        return;
    }

    std::sort(hits.begin(), hits.end(), [](const Suggestion& a, const Suggestion& b) {
        if (a.score != b.score) return a.score < b.score;
        return a.def->name < b.def->name;
    });

    std::cout << "  did you mean:\n";
    const size_t maxShow = 10;
    for (size_t i = 0; i < hits.size() && i < maxShow; ++i)
    {
        const SymbolDef* d = hits[i].def;
        std::cout << std::format("    {}  ({})", SymbolDisplayName(*d), SymbolKindName(d->kind));
        if (d->line > 0 && !d->file.empty())
            std::cout << std::format("  {}:{}", d->file, d->line);
        std::cout << "\n";
    }
}

// A real compile reaches a platform backend only through its umbrella's `if const (__WINDOWS__)`
// dispatch (os.cb, network/socket.cb) or through an explicit user import (ui_native/win32.cb).
// The --symbol synthesized import-all-core unit has no such gate, so it would pull BOTH
// alternates in and collide on every shared name - and pull in backends that cannot even parse
// on this host (win32 needs windows.h). Keep only the host's, by file stem.
static bool IsForeignPlatformCoreFile(const std::string& rel, const std::string& stem)
{
    auto endsWith = [&](const std::string& hay, const char* suffix) {
        std::string s(suffix);
        return hay.size() > s.size() && hay.compare(hay.size() - s.size(), s.size(), s) == 0;
    };
    // ui_canvas/* and ui_native/* are MUTUALLY EXCLUSIVE UI backends selected by an explicit
    // user import, not by a host gate - and they define the same canvas* names as each other.
    // Only the host-native one can be indexed; the rest need `cflat yourapp.cb --symbol X`.
    if (rel.rfind("ui_canvas/", 0) == 0) return true;
#ifdef _WIN32
    if (rel.rfind("ui_native/", 0) == 0) return stem != "win32" && stem != "host";
    return endsWith(stem, ".posix");
#else
    // winrt.cb imports a .winmd, which only a Windows target can read.
    if (stem == "winrt") return true;
#ifdef __APPLE__
    if (rel.rfind("ui_native/", 0) == 0) return stem != "cocoa" && stem != "host";
#else
    if (rel.rfind("ui_native/", 0) == 0) return true;
#endif
    return endsWith(stem, ".windows");
#endif
}

int RunSymbolQuery(ArgParser& args, const std::string& runtimeDir, bool showProgress)
{
    const std::vector<std::string> terms = args.getMultiOption("symbol");
    const std::vector<std::string> importDirs = args.getMultiOption("import-dir");

    // What to index: an explicit positional source file (true IDE semantics - the
    // index reflects exactly what that file imports), or, when none is given, a
    // synthetic file that imports every core library so the whole standard library
    // is searchable with zero setup.
    std::string sourcePath;
    std::string tempPath;
    if (args.positionalCount() >= 1)
    {
        sourcePath = *args.getPositional(0);
    }
    else
    {
        std::string body;
        std::error_code ec;
        auto coreDir = std::filesystem::path(runtimeDir) / "core";
        if (std::filesystem::is_directory(coreDir, ec))
        {
            std::vector<std::string> names;
            // Recursive: subdirectory libraries (e.g. hpc/vecmath.cb) must be
            // searchable too, or --symbol silently hides whole library families.
            for (const auto& e : std::filesystem::recursive_directory_iterator(coreDir, ec))
            {
                if (e.path().extension() != ".cb" || e.path().filename() == "runtime.cb")
                    continue;
                std::string rel = std::filesystem::relative(e.path(), coreDir, ec).generic_string();
                if (IsForeignPlatformCoreFile(rel, e.path().stem().string()))
                    continue;
                names.push_back(rel);
            }
            std::sort(names.begin(), names.end());
            for (const auto& n : names)
                body += "import \"" + n + "\";\n";
        }
        body += "extern int main() { return 0; }\n";

        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        tempPath = (std::filesystem::temp_directory_path() /
                    ("cflat_symquery_" + std::to_string(stamp) + ".cb")).string();
        std::ofstream out(tempPath, std::ios::binary);
        out << body;
        out.close();
        sourcePath = tempPath;
    }

    LLVMBackend compiler;
    compiler.SetRuntimeDir(runtimeDir);
    compiler.SetVerbose(args.hasFlag("verbose"));
    compiler.SetAsan(args.hasFlag("asan"));
    compiler.SetSanitizeOwnership(args.hasFlag("sanitize-ownership"));
    const char* symbolLocale = std::getenv("CFLAT_LOCALE");
    compiler.SetLocale(args.getOption("locale").value_or(
        symbolLocale && *symbolLocale ? symbolLocale : "en"));
    compiler.SetLocaleDirectory(args.getOption("locale-dir").value_or(
        (std::filesystem::path(runtimeDir) / "locales").string()));
    compiler.LoadLocale(args.hasFlag("verbose"));

    LspSymbolIndex index;
    compiler.SetSymbolSink(&index);
    bool ok = compiler.Analyze(sourcePath, importDirs, runtimeDir);
    compiler.SetSymbolSink(nullptr);

    if (!tempPath.empty())
    {
        std::error_code ec;
        std::filesystem::remove(tempPath, ec);
    }

    if (index.SymbolCount() == 0)
    {
        std::cout << "Error: no symbols indexed";
        if (!ok) std::cout << std::format(" (analysis of '{}' failed)", sourcePath);
        std::cout << ".\n";
        return 1;
    }
    if (!ok && showProgress)
        std::cout << "(note: analysis reported errors; results may be incomplete)\n";
    // A failed analysis must be distinguishable from a clean lookup by exit code alone.
    int exitCode = ok ? 0 : 1;

    bool first = true;
    for (const auto& term : terms)
    {
        if (!first) std::cout << "\n";
        first = false;

        const SymbolDef* exact = index.Lookup(term);
        if (!exact)
        {
            const std::string lcq = ToLower(term);
            for (const auto& [name, def] : index.Symbols())
                if (ToLower(name) == lcq) { exact = &def; break; }
        }

        if (exact)
            PrintSymbolDetail(index, *exact);
        else
        {
            std::cout << std::format("'{}': no exact match.\n", term);
            if (!tempPath.empty())
                std::cout << "  (no source file given, only showing symbols from core libraries; "
                             "pass a .cb that imports your headers to search them)\n";
            PrintSymbolSuggestions(index, term);
        }
    }
    return exitCode;
}

struct SymbolLineToken
{
    std::string path;
    std::string receiver;
    std::string member;
    bool hasMember = false;
    size_t end = 0;
};

static bool IsIdentifierStart(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool IsIdentifierChar(char c)
{
    return IsIdentifierStart(c) || (c >= '0' && c <= '9');
}

static std::vector<SymbolLineToken> ScanSymbolLine(const std::string& line)
{
    std::vector<SymbolLineToken> tokens;
    size_t pos = 0;
    while (pos < line.size())
    {
        if (line[pos] == '/' && pos + 1 < line.size() && line[pos + 1] == '/')
            break;
        if (line[pos] == '\'' || line[pos] == '"')
        {
            const char quote = line[pos++];
            while (pos < line.size())
            {
                if (line[pos] == '\\' && pos + 1 < line.size())
                {
                    pos += 2;
                    continue;
                }
                if (line[pos++] == quote) break;
            }
            continue;
        }
        if (!IsIdentifierStart(line[pos]))
        {
            ++pos;
            continue;
        }

        SymbolLineToken token;
        const size_t start = pos++;
        while (pos < line.size() && IsIdentifierChar(line[pos])) ++pos;
        token.path = line.substr(start, pos - start);
        token.member = token.path;
        token.receiver = token.path;

        while (true)
        {
            const size_t separatorStart = pos;
            while (pos < line.size() && std::isspace((unsigned char)line[pos])) ++pos;
            bool arrow = false;
            if (pos < line.size() && line[pos] == '.')
                ++pos;
            else if (pos + 1 < line.size() && line[pos] == '-' && line[pos + 1] == '>')
            {
                pos += 2;
                arrow = true;
            }
            else
            {
                pos = separatorStart;
                break;
            }

            while (pos < line.size() && std::isspace((unsigned char)line[pos])) ++pos;
            if (pos >= line.size() || !IsIdentifierStart(line[pos]))
            {
                pos = separatorStart;
                break;
            }
            const size_t memberStart = pos++;
            while (pos < line.size() && IsIdentifierChar(line[pos])) ++pos;
            token.path += "." + line.substr(memberStart, pos - memberStart);
            token.member = line.substr(memberStart, pos - memberStart);
            token.hasMember = true;
            (void)arrow;
        }
        token.end = pos;
        tokens.push_back(std::move(token));
    }
    return tokens;
}

static std::string TrimLineText(const std::string& line)
{
    size_t first = 0;
    while (first < line.size() && std::isspace((unsigned char)line[first])) ++first;
    size_t last = line.size();
    while (last > first && std::isspace((unsigned char)line[last - 1])) --last;
    return line.substr(first, last - first);
}

static std::string SymbolTypeLookupName(const std::string& typeName)
{
    std::string result;
    int templateDepth = 0;
    for (char c : typeName)
    {
        if (c == '<')
        {
            ++templateDepth;
            continue;
        }
        if (c == '>')
        {
            if (templateDepth > 0) --templateDepth;
            continue;
        }
        if (templateDepth == 0 && c != '*' && c != '&' && !std::isspace((unsigned char)c))
            result += c;
    }
    return result;
}

enum class SymbolArgumentFamily { Unknown, String, Character, Boolean, Integer, Floating, Pointer, Exact };

struct SymbolArgumentType
{
    SymbolArgumentFamily family = SymbolArgumentFamily::Unknown;
    std::string normalizedType;
};

static std::string NormalizeSymbolTypeName(const std::string& typeName)
{
    std::string result;
    for (char c : typeName)
        if (!std::isspace((unsigned char)c)) result += c;
    return result;
}

static SymbolArgumentFamily SymbolTypeFamily(const std::string& typeName)
{
    const std::string type = NormalizeSymbolTypeName(typeName);
    const std::string lower = ToLower(type);
    if (lower == "string" || lower == "char*" || lower == "constchar*" ||
        lower == "charconst*")
        return SymbolArgumentFamily::String;
    if (lower == "char") return SymbolArgumentFamily::Character;
    if (lower == "bool") return SymbolArgumentFamily::Boolean;
    if (lower == "int" || lower == "i8" || lower == "i16" || lower == "i32" ||
        lower == "i64" || lower == "u8" || lower == "u16" || lower == "u32" ||
        lower == "u64" || lower == "long" || lower == "short")
        return SymbolArgumentFamily::Integer;
    if (lower == "float" || lower == "double") return SymbolArgumentFamily::Floating;
    if (type.find('*') != std::string::npos) return SymbolArgumentFamily::Pointer;
    return SymbolArgumentFamily::Exact;
}

static bool IsQuotedSymbolLiteral(const std::string& text, char quote)
{
    if (text.size() < 2 || text.front() != quote || text.back() != quote) return false;
    bool escaped = false;
    for (size_t i = 1; i + 1 < text.size(); ++i)
    {
        if (escaped) escaped = false;
        else if (text[i] == '\\') escaped = true;
        else if (text[i] == quote) return false;
    }
    return !escaped;
}

static bool IsIntegerSymbolLiteral(const std::string& text)
{
    if (text.empty()) return false;
    size_t pos = (text[0] == '+' || text[0] == '-') ? 1 : 0;
    if (pos == text.size()) return false;
    bool hex = pos + 1 < text.size() && text[pos] == '0' &&
        (text[pos + 1] == 'x' || text[pos + 1] == 'X');
    if (hex) pos += 2;
    const size_t digitsStart = pos;
    while (pos < text.size())
    {
        const char c = text[pos];
        const bool digit = hex ? ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                                  (c >= 'A' && c <= 'F'))
                               : (c >= '0' && c <= '9');
        if (!digit) break;
        ++pos;
    }
    if (pos == digitsStart) return false;
    while (pos < text.size() && (text[pos] == 'u' || text[pos] == 'U' ||
                                 text[pos] == 'l' || text[pos] == 'L'))
        ++pos;
    return pos == text.size();
}

static bool IsFloatingSymbolLiteral(const std::string& text)
{
    if (text.empty()) return false;
    size_t pos = (text[0] == '+' || text[0] == '-') ? 1 : 0;
    bool digit = false;
    bool dot = false;
    while (pos < text.size() && ((text[pos] >= '0' && text[pos] <= '9') || text[pos] == '.'))
    {
        digit |= text[pos] >= '0' && text[pos] <= '9';
        dot |= text[pos] == '.';
        ++pos;
    }
    if (!digit) return false;
    if (pos < text.size() && (text[pos] == 'f' || text[pos] == 'F')) ++pos;
    return pos == text.size() && (dot || text.back() == 'f' || text.back() == 'F');
}

static bool IsPlainSymbolIdentifier(const std::string& text)
{
    if (text.empty() || !IsIdentifierStart(text[0])) return false;
    for (size_t i = 1; i < text.size(); ++i)
        if (!IsIdentifierChar(text[i])) return false;
    return true;
}

static SymbolArgumentType InferSymbolArgumentType(const LspSymbolIndex& index,
                                                  const std::string& argument)
{
    SymbolArgumentType result;
    const std::string text = TrimLineText(argument);
    if (IsQuotedSymbolLiteral(text, '"'))
    {
        result.normalizedType = "char*";
        result.family = SymbolArgumentFamily::String;
    }
    else if (IsQuotedSymbolLiteral(text, '\''))
    {
        result.normalizedType = "char";
        result.family = SymbolArgumentFamily::Character;
    }
    else if (IsIntegerSymbolLiteral(text))
    {
        result.normalizedType = "int";
        result.family = SymbolArgumentFamily::Integer;
    }
    else if (IsFloatingSymbolLiteral(text))
    {
        result.normalizedType = text.back() == 'f' || text.back() == 'F' ? "float" : "double";
        result.family = SymbolArgumentFamily::Floating;
    }
    else if (text == "true" || text == "false")
    {
        result.normalizedType = "bool";
        result.family = SymbolArgumentFamily::Boolean;
    }
    else if (text == "nullptr")
        result.family = SymbolArgumentFamily::Pointer;
    else if (text == "__FILE__")
    {
        result.normalizedType = "char*";
        result.family = SymbolArgumentFamily::String;
    }
    else if (text == "__LINE__")
    {
        result.normalizedType = "int";
        result.family = SymbolArgumentFamily::Integer;
    }
    else if (IsPlainSymbolIdentifier(text))
    {
        if (const std::string* typeName = index.LookupVariableType(text))
        {
            result.normalizedType = NormalizeSymbolTypeName(*typeName);
            result.family = SymbolTypeFamily(result.normalizedType);
        }
    }
    return result;
}

static bool SplitSymbolTopLevel(const std::string& text, std::vector<std::string>& parts)
{
    size_t start = 0;
    int parens = 0;
    int brackets = 0;
    int braces = 0;
    int angles = 0;
    char quote = 0;
    bool escaped = false;
    for (size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (quote)
        {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '\'' || c == '"') { quote = c; continue; }
        if (c == '(') ++parens;
        else if (c == ')' && --parens < 0) return false;
        else if (c == '[') ++brackets;
        else if (c == ']' && --brackets < 0) return false;
        else if (c == '{') ++braces;
        else if (c == '}' && --braces < 0) return false;
        else if (c == '<') ++angles;
        else if (c == '>' && angles > 0) --angles;
        else if (c == ',' && parens == 0 && brackets == 0 && braces == 0 && angles == 0)
        {
            parts.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    if (quote || parens || brackets || braces || angles) return false;
    parts.push_back(text.substr(start));
    return true;
}

static bool ExtractSymbolCallArguments(const std::string& line, size_t openParen,
                                       std::string& arguments)
{
    if (openParen >= line.size() || line[openParen] != '(') return false;
    int depth = 0;
    char quote = 0;
    bool escaped = false;
    for (size_t i = openParen; i < line.size(); ++i)
    {
        const char c = line[i];
        if (quote)
        {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '\'' || c == '"') { quote = c; continue; }
        if (c == '(') ++depth;
        else if (c == ')' && --depth == 0)
        {
            arguments = line.substr(openParen + 1, i - openParen - 1);
            return true;
        }
    }
    return false;
}

static bool ParseSymbolSignatureParameters(const std::string& signature,
                                           std::vector<std::string>& parameters)
{
    const size_t openParen = signature.find('(');
    if (openParen == std::string::npos) return false;
    std::string body;
    if (!ExtractSymbolCallArguments(signature, openParen, body)) return false;
    std::vector<std::string> rawParameters;
    if (!SplitSymbolTopLevel(body, rawParameters)) return false;
    if (rawParameters.size() == 1 && TrimLineText(rawParameters[0]).empty())
        rawParameters.clear();
    for (const std::string& raw : rawParameters)
    {
        std::string parameter = TrimLineText(raw);
        if (parameter.empty()) return false;
        size_t nameEnd = parameter.size();
        while (nameEnd > 0 && IsIdentifierChar(parameter[nameEnd - 1])) --nameEnd;
        if (nameEnd == parameter.size()) return false;
        const std::string type = TrimLineText(parameter.substr(0, nameEnd));
        if (type.empty()) return false;
        parameters.push_back(NormalizeSymbolTypeName(type));
    }
    return true;
}

static bool TryNarrowSymbolOverloads(const LspSymbolIndex& index, const SymbolDef& def,
                                     const std::string& line, const SymbolLineToken& token,
                                     std::vector<std::string>& selected)
{
    size_t openParen = token.end;
    while (openParen < line.size() && std::isspace((unsigned char)line[openParen])) ++openParen;
    if (openParen >= line.size() || line[openParen] != '(') return false;

    std::string argumentText;
    if (!ExtractSymbolCallArguments(line, openParen, argumentText)) return false;
    std::vector<std::string> rawArguments;
    if (!SplitSymbolTopLevel(argumentText, rawArguments)) return false;
    if (rawArguments.size() == 1 && TrimLineText(rawArguments[0]).empty()) rawArguments.clear();
    std::vector<SymbolArgumentType> arguments;
    for (const auto& raw : rawArguments)
        arguments.push_back(InferSymbolArgumentType(index, raw));

    std::vector<std::string> signatures;
    if (!def.signatureMarkdown.empty() && def.signatureMarkdown != SymbolDisplayName(def))
        signatures.push_back(def.signatureMarkdown);
    signatures.insert(signatures.end(), def.overloadSignatures.begin(), def.overloadSignatures.end());
    if (signatures.empty()) return false;

    struct ScoredSignature { int score; size_t index; };
    std::vector<ScoredSignature> scored;
    for (size_t i = 0; i < signatures.size(); ++i)
    {
        std::vector<std::string> parameters;
        if (!ParseSymbolSignatureParameters(signatures[i], parameters)) return false;
        if (parameters.size() != arguments.size()) continue;
        int score = 0;
        for (size_t j = 0; j < arguments.size(); ++j)
        {
            const SymbolArgumentType& argument = arguments[j];
            const std::string parameter = NormalizeSymbolTypeName(parameters[j]);
            if (!argument.normalizedType.empty() && argument.normalizedType == parameter)
                score += 2;
            else if (argument.family == SymbolArgumentFamily::Pointer &&
                     parameter.find('*') != std::string::npos)
                ++score;
            else if (argument.family != SymbolArgumentFamily::Unknown &&
                     argument.family != SymbolArgumentFamily::Exact &&
                     argument.family == SymbolTypeFamily(parameter))
                ++score;
            else if (argument.family != SymbolArgumentFamily::Unknown)
                score -= 100;
        }
        scored.push_back({ score, i });
    }
    if (scored.empty()) return false;
    const int bestScore = std::max_element(scored.begin(), scored.end(),
        [](const ScoredSignature& a, const ScoredSignature& b) { return a.score < b.score; })->score;
    for (const auto& candidate : scored)
        if (candidate.score == bestScore) selected.push_back(signatures[candidate.index]);
    return !selected.empty();
}

static bool IsSymbolLineKeyword(const std::string& name)
{
    static const std::unordered_set<std::string> keywords = {
        "if", "else", "for", "while", "do", "switch", "case", "default", "break",
        "continue", "return", "int", "float", "double", "bool", "char", "void",
        "const", "struct", "class", "interface", "enum", "true", "false", "nullptr",
        "new", "move", "import", "extern", "namespace", "static", "public", "private",
        "protected", "sizeof", "as", "is", "in", "where"
    };
    return keywords.contains(name);
}

static void PrintCompactSymbolDetail(const SymbolDef& def,
                                     const std::vector<std::string>* signatures = nullptr)
{
    const std::string displayName = SymbolDisplayName(def);
    std::cout << std::format("  {}  ({})\n", displayName, SymbolKindName(def.kind));
    if (!def.signatureMarkdown.empty() && def.signatureMarkdown != displayName)
    {
        if (!signatures) std::cout << std::format("    {}\n", def.signatureMarkdown);
    }
    if (signatures)
        for (const auto& sig : *signatures) std::cout << std::format("    {}\n", sig);
    else
        for (const auto& sig : def.overloadSignatures) std::cout << std::format("    {}\n", sig);
    if (def.line > 0 && !def.file.empty())
        std::cout << std::format("    defined: {}:{}\n", def.file, def.line);
    if (!def.docComment.empty())
    {
        const size_t newline = def.docComment.find('\n');
        std::cout << std::format("    doc: {}\n", def.docComment.substr(0, newline));
    }
}

static void PrintDumpSymbolDetail(const LspSymbolIndex& index, const SymbolDef& def,
                                  const std::string& line, const SymbolLineToken& token)
{
    std::vector<std::string> selected;
    if (def.kind == SymbolKind::Function && TryNarrowSymbolOverloads(index, def, line, token, selected))
        PrintCompactSymbolDetail(def, &selected);
    else
        PrintCompactSymbolDetail(def);
}

static bool TryParsePositiveLine(const std::string& text, size_t& line)
{
    if (text.empty()) return false;
    for (char c : text)
        if (c < '0' || c > '9') return false;
    try
    {
        line = std::stoull(text);
    }
    catch (...)
    {
        return false;
    }
    return line > 0;
}

enum class SymbolDumpSelectorKind { Line, Function };

struct SymbolDumpSelector
{
    SymbolDumpSelectorKind kind;
    size_t firstLine = 0;
    size_t lastLine = 0;
    std::string functionName;
};

static bool ParseSymbolDumpSelector(const std::string& text, SymbolDumpSelector& selector)
{
    const size_t colon = text.find(':');
    const auto printSyntaxError = [&]() {
        std::cout << std::format(
            "Error: --symbol-dump selector must be line:<n>, line:<a>-<b>, or function:<name>: '{}'\n",
            text);
    };
    if (colon == std::string::npos)
    {
        printSyntaxError();
        return false;
    }

    const std::string prefix = text.substr(0, colon);
    const std::string value = text.substr(colon + 1);
    if (prefix == "function")
    {
        if (value.empty())
        {
            printSyntaxError();
            return false;
        }
        selector.kind = SymbolDumpSelectorKind::Function;
        selector.functionName = value;
        return true;
    }

    if (prefix != "line")
    {
        printSyntaxError();
        return false;
    }

    const size_t dash = value.find('-');
    if (dash == std::string::npos)
    {
        if (!TryParsePositiveLine(value, selector.firstLine))
        {
            std::cout << std::format(
                "Error: --symbol-dump line selector requires a positive integer: '{}'\n", text);
            return false;
        }
        selector.kind = SymbolDumpSelectorKind::Line;
        selector.lastLine = selector.firstLine;
        return true;
    }

    if (!TryParsePositiveLine(value.substr(0, dash), selector.firstLine) ||
        !TryParsePositiveLine(value.substr(dash + 1), selector.lastLine))
    {
        std::cout << std::format(
            "Error: --symbol-dump line range requires positive integers: '{}'\n", text);
        return false;
    }
    if (selector.firstLine > selector.lastLine)
    {
        std::cout << std::format(
            "Error: --symbol-dump line range is reversed: '{}'\n", text);
        return false;
    }
    selector.kind = SymbolDumpSelectorKind::Line;
    return true;
}

enum class SymbolDumpIrSelectorKind { Line, Function, Module };

struct SymbolDumpIrSelector
{
    SymbolDumpIrSelectorKind kind;
    size_t line = 0;
    std::string functionName;
};

static bool ParseSymbolDumpIrSelector(const std::string& text,
                                      const std::string& optionName,
                                      SymbolDumpIrSelector& selector)
{
    if (text == "module")
    {
        selector.kind = SymbolDumpIrSelectorKind::Module;
        return true;
    }

    const size_t colon = text.find(':');
    const auto printSyntaxError = [&]() {
        std::cout << std::format(
            "Error: {} selector must be module, line:<n>, or function:<name>: '{}'\n",
            optionName, text);
    };
    if (colon == std::string::npos)
    {
        printSyntaxError();
        return false;
    }

    const std::string prefix = text.substr(0, colon);
    const std::string value = text.substr(colon + 1);
    if (prefix == "function")
    {
        if (value.empty())
        {
            printSyntaxError();
            return false;
        }
        selector.kind = SymbolDumpIrSelectorKind::Function;
        selector.functionName = value;
        return true;
    }

    if (prefix != "line")
    {
        printSyntaxError();
        return false;
    }

    if (value.find('-') != std::string::npos)
    {
        std::cout << std::format(
            "Error: {} does not support line ranges: '{}'\n", optionName, text);
        return false;
    }
    if (!TryParsePositiveLine(value, selector.line))
    {
        std::cout << std::format(
            "Error: {} line selector requires a positive integer: '{}'\n", optionName, text);
        return false;
    }
    selector.kind = SymbolDumpIrSelectorKind::Line;
    return true;
}

static bool ReadSourceLines(const std::string& path, std::vector<std::string>& lines)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::string line;
    while (std::getline(input, line)) lines.push_back(line);
    return true;
}

static void DumpSymbolLine(const LspSymbolIndex& index,
                           const std::vector<std::string>& sourceLines,
                           size_t lineNumber, bool& firstLine,
                           const SymbolDef* preferredFunction = nullptr)
{
    if (!firstLine) std::cout << "\n";
    firstLine = false;
    if (lineNumber > sourceLines.size())
    {
        std::cout << std::format("line {}: out of range (file has {} lines)\n",
                                 lineNumber, sourceLines.size());
        return;
    }

    const std::string& sourceLine = sourceLines[lineNumber - 1];
    std::cout << std::format("line {}: {}\n", lineNumber, TrimLineText(sourceLine));
    std::unordered_set<std::string> printed;
    for (const auto& token : ScanSymbolLine(sourceLine))
    {
        const SymbolDef* def = index.Lookup(token.path);
        if (preferredFunction && lineNumber == (size_t)preferredFunction->line)
        {
            const std::string preferredName = SymbolDisplayName(*preferredFunction);
            const size_t dot = preferredName.rfind('.');
            const std::string preferredShort = dot == std::string::npos
                ? preferredName : preferredName.substr(dot + 1);
            if (token.path == preferredShort)
                def = preferredFunction;
        }
        if (def)
        {
            if (printed.insert(def->name).second)
                PrintDumpSymbolDetail(index, *def, sourceLine, token);
            continue;
        }

        if (!token.hasMember)
        {
            const VariableInfo* variable = index.LookupVariable(token.path);
            const std::string* typeName = index.LookupVariableType(token.path);
            if (IsSymbolLineKeyword(token.path) && !variable && !typeName)
                continue;
            if (variable || typeName)
            {
                const std::string type = variable && !variable->typeName.empty()
                    ? variable->typeName : (typeName ? *typeName : "");
                if (!printed.insert(token.path).second) continue;
                const std::string displayType = variable && !variable->displayTypeName.empty()
                    ? variable->displayTypeName : type;
                std::cout << std::format("  {} (variable) : {}\n", token.path, displayType);
                if (variable && variable->line > 0 && !variable->file.empty())
                    std::cout << std::format("    defined: {}:{}\n", variable->file, variable->line);
                const SymbolDef* typeDef = type.empty() ? nullptr : index.Lookup(SymbolTypeLookupName(type));
                if (typeDef && !typeDef->signatureMarkdown.empty() &&
                    typeDef->signatureMarkdown != SymbolDisplayName(*typeDef))
                    std::cout << std::format("    {}\n", typeDef->signatureMarkdown);
            }
            continue;
        }

        const std::string* receiverType = index.LookupVariableType(token.receiver);
        if (!receiverType) continue;
        const std::string type = SymbolTypeLookupName(*receiverType);
        std::string typeBase = type;
        typeBase = std::string(MangledBase(typeBase));
        def = index.Lookup(type + "." + token.member);
        if (!def && typeBase != type)
            def = index.Lookup(typeBase + "." + token.member);
        if (def && printed.insert(def->name).second)
            PrintDumpSymbolDetail(index, *def, sourceLine, token);
    }
}

static std::string StripGenericOwner(const std::string& name)
{
    const size_t open = name.find('<');
    if (open == std::string::npos) return name;
    int depth = 0;
    for (size_t i = open; i < name.size(); i++)
    {
        if (name[i] == '<')
            ++depth;
        else if (name[i] == '>' && --depth == 0)
        {
            if (i + 1 < name.size() && name[i + 1] == '.')
                return name.substr(0, open) + name.substr(i + 1);
            return name;
        }
    }
    return name;
}

static const SymbolDef* LookupFunction(const LspSymbolIndex& index,
                                       const LLVMBackend& compiler,
                                       const std::string& name)
{
    if (const SymbolDef* def = index.Lookup(name); def && def->kind == SymbolKind::Function)
        return def;

    const size_t selectorDot = name.rfind('.');
    if (selectorDot != std::string::npos && selectorDot > 0 && selectorDot + 1 < name.size())
    {
        const std::string normalizedSelector = NormalizeSymbolTypeName(StripGenericOwner(name));
        for (const auto& [symbolName, def] : index.Symbols())
        {
            if (def.kind != SymbolKind::Function) continue;
            if (NormalizeSymbolTypeName(def.name) == normalizedSelector
                || NormalizeSymbolTypeName(SymbolDisplayName(def)) == normalizedSelector)
                return &def;
        }
    }

    const std::string baseName = StripGenericOwner(name);
    if (baseName != name)
        if (const SymbolDef* def = index.Lookup(baseName); def && def->kind == SymbolKind::Function)
            return def;

    // A raw LLVM function symbol has no index entry of its own. Its source function name,
    // and where possible its receiver type, identify the source definition to dump.
    FunctionSymbolSpelling raw;
    std::string rawFunctionName;
    if (DemangleFunctionSymbol(compiler, name, raw))
        rawFunctionName = raw.name;
    else if (name.size() > 2 && name.front() == '_')
    {
        const size_t end = name.find('$', 1);
        if (end != std::string::npos)
            rawFunctionName = name.substr(1, end - 1);
    }
    if (!rawFunctionName.empty())
    {
        if (const SymbolDef* def = index.Lookup(rawFunctionName);
            def && def->kind == SymbolKind::Function)
            return def;

        std::string receiver;
        const std::string sourceSymbol = SpellFunctionSymbol(compiler, name);
        const size_t openParen = sourceSymbol.find('(');
        if (openParen != std::string::npos && !raw.parameters.empty())
        {
            const size_t firstEnd = sourceSymbol.find(',', openParen + 1);
            receiver = sourceSymbol.substr(openParen + 1,
                (firstEnd == std::string::npos ? sourceSymbol.find(')', openParen + 1) : firstEnd)
                - openParen - 1);
            while (receiver.starts_with("move ")) receiver.erase(0, 5);
            while (receiver.ends_with("*")) receiver.pop_back();
            if (receiver.ends_with("[]")) receiver.resize(receiver.size() - 2);
            receiver = StripGenericOwner(receiver);
        }
        if (!receiver.empty())
            if (const SymbolDef* def = index.Lookup(receiver + "." + rawFunctionName);
                def && def->kind == SymbolKind::Function)
                return def;

        for (const auto& [symbolName, def] : index.Symbols())
            if (def.kind == SymbolKind::Function
                && (def.displayName == rawFunctionName
                    || def.displayName.ends_with("." + rawFunctionName)))
                return &def;
    }

    const std::string lowerName = ToLower(name);
    for (const auto& [symbolName, def] : index.Symbols())
        if (def.kind == SymbolKind::Function && ToLower(symbolName) == lowerName)
            return &def;
    const std::string lowerBaseName = ToLower(baseName);
    if (baseName != name)
        for (const auto& [symbolName, def] : index.Symbols())
            if (def.kind == SymbolKind::Function && ToLower(symbolName) == lowerBaseName)
                return &def;
    if (name.find('.') == std::string::npos)
        for (const auto& [symbolName, def] : index.Symbols())
            if (def.kind == SymbolKind::Function
                && (symbolName.ends_with("." + name)
                    || def.displayName.ends_with("." + name)))
                return &def;
    return nullptr;
}

static size_t FindFunctionEndLine(const std::vector<std::string>& sourceLines, size_t startLine)
{
    if (startLine == 0 || startLine > sourceLines.size()) return 0;

    bool inBlockComment = false;
    char quote = 0;
    int depth = 0;
    bool foundBody = false;
    for (size_t lineNumber = startLine; lineNumber <= sourceLines.size(); ++lineNumber)
    {
        const std::string& line = sourceLines[lineNumber - 1];
        for (size_t i = 0; i < line.size(); ++i)
        {
            const char c = line[i];
            if (inBlockComment)
            {
                if (c == '*' && i + 1 < line.size() && line[i + 1] == '/')
                {
                    inBlockComment = false;
                    ++i;
                }
                continue;
            }
            if (quote)
            {
                if (c == '\\' && i + 1 < line.size())
                    ++i;
                else if (c == quote)
                    quote = 0;
                continue;
            }
            if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') break;
            if (c == '/' && i + 1 < line.size() && line[i + 1] == '*')
            {
                inBlockComment = true;
                ++i;
                continue;
            }
            if (c == '\'' || c == '"')
            {
                quote = c;
                continue;
            }
            if (c == '{')
            {
                foundBody = true;
                ++depth;
            }
            else if (c == '}' && foundBody && --depth == 0)
                return lineNumber;
        }
    }
    return foundBody ? sourceLines.size() : startLine;
}

int RunSymbolDumpQuery(ArgParser& args, const std::string& runtimeDir, bool showProgress)
{
    const auto selectorArguments = args.getMultiOption("symbol-dump");
    auto source = args.getPositional(0);
    if (!source)
    {
        std::cout << "Error: --symbol-dump requires an input source file\n";
        return 1;
    }

    std::vector<SymbolDumpSelector> selectors;
    for (const auto& argument : selectorArguments)
    {
        SymbolDumpSelector selector;
        if (!ParseSymbolDumpSelector(argument, selector))
            return 1;
        selectors.push_back(std::move(selector));
    }

    const std::vector<std::string> importDirs = args.getMultiOption("import-dir");
    LLVMBackend compiler;
    compiler.SetRuntimeDir(runtimeDir);
    compiler.SetVerbose(args.hasFlag("verbose"));
    compiler.SetAsan(args.hasFlag("asan"));
    compiler.SetSanitizeOwnership(args.hasFlag("sanitize-ownership"));
    const char* symbolLocale = std::getenv("CFLAT_LOCALE");
    compiler.SetLocale(args.getOption("locale").value_or(
        symbolLocale && *symbolLocale ? symbolLocale : "en"));
    compiler.SetLocaleDirectory(args.getOption("locale-dir").value_or(
        (std::filesystem::path(runtimeDir) / "locales").string()));
    compiler.LoadLocale(args.hasFlag("verbose"));

    LspSymbolIndex index;
    compiler.SetSymbolSink(&index);
    bool ok = compiler.Analyze(*source, importDirs, runtimeDir);
    compiler.SetSymbolSink(nullptr);

    if (index.SymbolCount() == 0)
    {
        std::cout << "Error: no symbols indexed";
        if (!ok) std::cout << std::format(" (analysis of '{}' failed)", *source);
        std::cout << ".\n";
        return 1;
    }
    if (!ok && showProgress)
        std::cout << "(note: analysis reported errors; results may be incomplete)\n";

    bool firstLine = true;
    std::vector<std::string> inputLines;
    bool inputLinesRead = false;
    for (const auto& selector : selectors)
    {
        if (selector.kind == SymbolDumpSelectorKind::Line)
        {
            if (!inputLinesRead)
            {
                if (!ReadSourceLines(*source, inputLines))
                {
                    std::cout << std::format("Error: could not read input source file '{}'.\n", *source);
                    return 1;
                }
                inputLinesRead = true;
            }
            for (size_t lineNumber = selector.firstLine;; ++lineNumber)
            {
                DumpSymbolLine(index, inputLines, lineNumber, firstLine);
                if (lineNumber == selector.lastLine) break;
            }
            continue;
        }

        const SymbolDef* def = LookupFunction(index, compiler, selector.functionName);
        if (!def)
        {
            std::cout << std::format("'{}': no exact match.\n", selector.functionName);
            PrintSymbolSuggestions(index, selector.functionName);
            return 1;
        }

        std::vector<std::string> functionLines;
        if (!ReadSourceLines(def->file, functionLines))
        {
            std::cout << std::format("Error: could not read definition source file '{}'.\n", def->file);
            return 1;
        }
        const size_t startLine = def->line > 0 ? (size_t)def->line : 0;
        const size_t endLine = FindFunctionEndLine(functionLines, startLine);
        if (startLine == 0 || endLine == 0)
        {
            std::cout << std::format("Error: function '{}' has no source definition.\n",
                                     selector.functionName);
            return 1;
        }
        for (size_t lineNumber = startLine;; ++lineNumber)
        {
            DumpSymbolLine(index, functionLines, lineNumber, firstLine, def);
            if (lineNumber == endLine) break;
        }
    }
    return 0;
}

int RunSymbolDumpIrQuery(ArgParser& args, const std::string& runtimeDir)
{
    const auto irArguments = args.getMultiOption("symbol-dump-ir");
    const auto optArguments = args.getMultiOption("symbol-dump-opt");
    if (args.positionalCount() == 0)
    {
        std::cout << "Error: --symbol-dump-ir and --symbol-dump-opt require an input source file\n";
        return 1;
    }

    struct Query
    {
        std::string optionName;
        int optLevel;
        const std::vector<std::string>* arguments;
    };
    std::vector<Query> queries;
    if (!irArguments.empty())
        queries.push_back({ "--symbol-dump-ir", 0, &irArguments });
    if (!optArguments.empty())
    {
        const int optLevel = args.hasFlag("O2") ? 2 : args.hasFlag("O1") ? 1
                                                                    : args.hasFlag("O0") ? 0 : 2;
        queries.push_back({ "--symbol-dump-opt", optLevel, &optArguments });
    }

    struct ParsedQuery
    {
        Query query;
        std::vector<SymbolDumpIrSelector> selectors;
    };
    std::vector<ParsedQuery> parsedQueries;
    for (const auto& query : queries)
    {
        ParsedQuery parsed{ query, {} };
        for (const auto& argument : *query.arguments)
        {
            SymbolDumpIrSelector selector;
            if (!ParseSymbolDumpIrSelector(argument, query.optionName, selector))
                return 1;
            parsed.selectors.push_back(std::move(selector));
        }
        parsedQueries.push_back(std::move(parsed));
    }

    const std::vector<std::string> importDirs = args.getMultiOption("import-dir");
    LLVMBackend compiler;
    compiler.SetRuntimeDir(runtimeDir);
    compiler.SetVerbose(args.hasFlag("verbose"));
    compiler.SetBatchMode(true);
    // The dump must see the sanitizer-gated IR the real compile would emit, the same way the
    // -O level already reaches --symbol-dump-opt.
    compiler.SetAsan(args.hasFlag("asan"));
    compiler.SetSanitizeOwnership(args.hasFlag("sanitize-ownership"));
    const char* symbolLocale = std::getenv("CFLAT_LOCALE");
    compiler.SetLocale(args.getOption("locale").value_or(
        symbolLocale && *symbolLocale ? symbolLocale : "en"));
    compiler.SetLocaleDirectory(args.getOption("locale-dir").value_or(
        (std::filesystem::path(runtimeDir) / "locales").string()));
    compiler.LoadLocale(args.hasFlag("verbose"));

    int failures = 0;
    for (size_t fileIndex = 0; fileIndex < args.positionalCount(); ++fileIndex)
    {
        const std::string file = *args.getPositional(fileIndex);
        if (fileIndex > 0)
            compiler.ResetForReanalysis();
        compiler.SetSourceDisplayName(std::filesystem::path(file).filename().string());
        if (args.positionalCount() > 1)
            std::cout << "; ==== " << file << " ====\n";

        LspSymbolIndex index;
        compiler.SetSymbolSink(&index);
        const bool analysisOk = compiler.Analyze(file, importDirs, runtimeDir);
        compiler.SetSymbolSink(nullptr);
        const std::string analyzedFile = compiler.GetSourceFilePath();
        if (!analysisOk)
            ++failures;

        for (const auto& parsed : parsedQueries)
        {
            for (size_t selectorIndex = 0; selectorIndex < parsed.selectors.size(); ++selectorIndex)
            {
                const auto& selector = parsed.selectors[selectorIndex];
                const std::string& selectorText = (*parsed.query.arguments)[selectorIndex];
                std::string functionName;
                bool wholeModule = false;
                if (selector.kind == SymbolDumpIrSelectorKind::Module)
                    wholeModule = true;
                else if (selector.kind == SymbolDumpIrSelectorKind::Function)
                    functionName = selector.functionName;
                else
                {
                    auto ranges = index.FunctionsEnclosing(analyzedFile, (int)selector.line);
                    if (ranges.empty())
                    {
                        std::cout << std::format(
                            "; {}:{} has no function\n", file, selector.line);
                        continue;
                    }
                    functionName = ranges.front()->name;
                }

                std::string output;
                if (!compiler.PrintModuleView(output, "ir", parsed.query.optLevel,
                                              functionName, wholeModule))
                {
                    std::cout << std::format(
                        "Error: failed to emit IR for '{}' selector '{}'\n", file, selectorText);
                    ++failures;
                    continue;
                }
                std::cout << output;
            }
        }
    }
    return failures == 0 ? 0 : 1;
}
