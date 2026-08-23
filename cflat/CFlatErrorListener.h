#pragma once
#include "platform/GeneratedParser.h" // antlr runtime + generated parser + kTokenEOF
#include "DiagnosticLocalization.h"
#include <string>
#include <vector>
#include <set>
#include <sstream>
#include <functional>
#include <string_view>
#include <utility>
#include <cctype>

struct ParseDiagnostic
{
    std::string file;
    int line = 0;
    int col  = 0;
    std::string message;
    std::string hint;
};

class CFlatErrorListener : public antlr4::BaseErrorListener
{
public:
    CFlatErrorListener(
        std::string filename, std::vector<std::string> sourceLines,
        std::function<std::string(std::string, std::vector<std::string>)> localizeMessage = {})
        : filename_(std::move(filename)), sourceLines_(std::move(sourceLines))
    {
        if (localizeMessage)
            localizeMessage_ = std::move(localizeMessage);
        else
            localizeMessage_ = [](std::string englishTemplate,
                                  std::vector<std::string> arguments) {
                return DiagnosticLocalization::FormatSourceTemplate(englishTemplate, arguments);
            };
    }

    void syntaxError(antlr4::Recognizer* recognizer, antlr4::Token* offendingSymbol,
                     size_t line, size_t charPositionInLine,
                     const std::string& msg, std::exception_ptr e) override
    {
        if (!seenLines_.insert(static_cast<int>(line)).second)
            return;

        antlr4::Token* diagnosticToken = offendingSymbol;
        ParseDiagnostic d;
        d.file    = filename_;
        d.line    = diagnosticToken ? static_cast<int>(diagnosticToken->getLine())
                                    : static_cast<int>(line);
        d.col     = diagnosticToken
            ? static_cast<int>(diagnosticToken->getCharPositionInLine())
            : static_cast<int>(charPositionInLine);
        static constexpr std::string_view lexerPrefix = "token recognition error at: ";
        int sourceReservedCol = -1;
        std::string reserved = reservedWordFromSource(line, charPositionInLine, sourceReservedCol);
        if (sourceReservedCol >= 0)
            d.col = sourceReservedCol;
        if (msg.rfind(lexerPrefix, 0) == 0)
            d.message = localizeMessage_("unexpected character: {}",
                                         {msg.substr(lexerPrefix.size())});
        else if (!reserved.empty())
            d.message = reserved;
        else
            d.message = humanizeMessage(msg);
        d.hint    = reserved.empty() ? buildHint(recognizer, diagnosticToken, e, msg) : std::string();
        diagnostics_.push_back(std::move(d));
    }

    bool hasErrors() const { return !diagnostics_.empty(); }
    const std::vector<ParseDiagnostic>& getDiagnostics() const { return diagnostics_; }

private:
    std::string filename_;
    std::vector<std::string> sourceLines_;
    std::vector<ParseDiagnostic> diagnostics_;
    std::set<int> seenLines_;
    std::function<std::string(std::string, std::vector<std::string>)> localizeMessage_;

    static bool isKnownReservedWord(const std::string& word)
    {
        static constexpr const char* words[] = {
            "alignas", "alignof", "annotation", "as", "atomic", "bool", "break", "case",
            "char", "class", "const", "continue", "default", "delete", "do", "double",
            "else", "enum", "extern", "false", "float", "for", "function", "goto", "if",
            "import", "in", "inline", "int", "interface", "is", "long", "move", "namespace",
            "nullptr", "register", "return", "short", "signed", "sizeof", "static", "struct",
            "switch", "true", "typedef", "typeof", "union", "unsigned", "void", "volatile", "while"
        };
        for (const char* candidate : words)
            if (word == candidate) return true;
        return false;
    }

    std::string reservedWordFromSource(size_t line, size_t col, int& wordCol) const
    {
        if (line == 0 || line > sourceLines_.size()) return {};
        const std::string& source = sourceLines_[line - 1];
        static constexpr const char* typeWords[] = {
            "auto", "bool", "char", "double", "float", "int", "long", "short", "string",
            "signed", "unsigned", "void", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64"
        };
        auto isTypeWord = [&](const std::string& candidate) {
            for (const char* typeWord : typeWords)
                if (candidate == typeWord) return true;
            return false;
        };
        std::string previousWord;
        size_t previousEnd = 0;
        // Only a declaration-shaped gap ("int class", "int* class") counts. A ',' or ')'
        // between them means the keyword is a legal use ("(int)true", "int, move Buf b").
        auto gapIsDeclarationLike = [&](size_t from, size_t to) {
            if (to <= from) return false;
            for (size_t k = from; k < to; ++k)
                if (source[k] != ' ' && source[k] != '\t' && source[k] != '*'
                    && source[k] != '&' && source[k] != '?')
                    return false;
            return true;
        };
        for (size_t i = 0; i < source.size(); )
        {
            if (source[i] == '"' || source[i] == '\'')
            {
                char quote = source[i++];
                while (i < source.size())
                {
                    if (source[i] == '\\' && i + 1 < source.size()) { i += 2; continue; }
                    if (source[i++] == quote) break;
                }
                continue;
            }
            if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '/') break;
            while (i < source.size()
                   && !(std::isalnum(static_cast<unsigned char>(source[i])) || source[i] == '_')
                   && source[i] != '"' && source[i] != '\'')
                ++i;
            if (i == source.size()) break;
            size_t begin = i++;
            while (i < source.size()
                   && (std::isalnum(static_cast<unsigned char>(source[i])) || source[i] == '_'))
                ++i;
            std::string word = source.substr(begin, i - begin);
            if (isKnownReservedWord(word) && !isTypeWord(word) && isTypeWord(previousWord)
                && gapIsDeclarationLike(previousEnd, begin))
            {
                wordCol = static_cast<int>(begin);
                return localizeMessage_("'{}' is a reserved word in CFlat and cannot be used as an identifier",
                                        {word});
            }
            previousWord = word;
            previousEnd = i;
        }
        (void)col;
        return {};
    }

    static std::string humanizeMessage(const std::string& msg)
    {
        // Replace ANTLR internal names with readable tokens.
        static const std::pair<std::string, std::string> replacements[] = {
            { "<EOF>",      "end of file" },
            { "token recognition error at:", "unexpected character:" },
        };
        std::string result = msg;
        for (auto& [from, to] : replacements)
        {
            size_t pos = 0;
            while ((pos = result.find(from, pos)) != std::string::npos)
            {
                result.replace(pos, from.size(), to);
                pos += to.size();
            }
        }
        return result;
    }

    // True when the offending token's line already carries a ';' at or after the error column.
    bool statementIsTerminated(antlr4::Token* offendingSymbol) const
    {
        if (offendingSymbol == nullptr) return false;
        int lineIdx = static_cast<int>(offendingSymbol->getLine()) - 1;
        if (lineIdx < 0 || lineIdx >= static_cast<int>(sourceLines_.size())) return false;
        const std::string& srcLine = sourceLines_[lineIdx];
        int col = static_cast<int>(offendingSymbol->getCharPositionInLine());
        if (col < 0 || col > static_cast<int>(srcLine.size())) return false;
        return srcLine.find(';', static_cast<size_t>(col)) != std::string::npos;
    }

    std::string buildHint(antlr4::Recognizer* recognizer,
                          antlr4::Token* offendingSymbol,
                          std::exception_ptr /*e*/,
                          const std::string& msg)
    {
        // Check for struct-implements-interface before walking the context chain.
        // The parse error fires from ExternalDeclarationContext with the offending
        // token being the interface name (not ':'). Detect by checking whether the
        // source line starts with 'struct' and contains ':' before the error column.
        if (offendingSymbol)
        {
            int lineIdx = static_cast<int>(offendingSymbol->getLine()) - 1;
            int col     = static_cast<int>(offendingSymbol->getCharPositionInLine());
            if (lineIdx >= 0 && lineIdx < static_cast<int>(sourceLines_.size()))
            {
                const std::string& srcLine = sourceLines_[lineIdx];
                size_t firstNonSpace = srcLine.find_first_not_of(" \t");
                if (firstNonSpace != std::string::npos &&
                    srcLine.substr(firstNonSpace, 6) == "struct")
                {
                    std::string before = srcLine.substr(0, std::min(col, static_cast<int>(srcLine.size())));
                    if (before.find(':') != std::string::npos)
                        return localizeMessage_(
                            "structs cannot implement interfaces; use 'class' instead", {});
                }
            }
        }

        // Walk the rule context chain looking for known grammar rules.
        auto* parser = dynamic_cast<antlr4::Parser*>(recognizer);
        if (!parser) return {};
        antlr4::RuleContext* ctx = parser->getContext();

        bool offendingIsEof = offendingSymbol &&
                              offendingSymbol->getType() == cflat::kTokenEOF;

        while (ctx)
        {
            // Check inner (more specific) rules before outer ones.

            if (dynamic_cast<CFlatParser::ExpressionStatementContext*>(ctx) ||
                dynamic_cast<CFlatParser::StatementContext*>(ctx)           ||
                dynamic_cast<CFlatParser::DeclarationContext*>(ctx)         ||
                dynamic_cast<CFlatParser::BlockItemContext*>(ctx))
            {
                // Only when a ';' really is missing: ANTLR says so itself when its recovery
                // inserted one. Otherwise a line that already carries a ';' would send the
                // reader to the previous line looking for a semicolon that is not missing.
                if (msg.find("missing ';'") == std::string::npos
                    && statementIsTerminated(offendingSymbol)) return {};
                return localizeMessage_("missing ';' at end of statement", {});
            }

            if (dynamic_cast<CFlatParser::ParameterTypeListContext*>(ctx))
                return localizeMessage_("check parameter list - missing type or closing ')'?", {});

            if (offendingIsEof &&
                (dynamic_cast<CFlatParser::CompoundStatementContext*>(ctx) ||
                 dynamic_cast<CFlatParser::BlockItemListContext*>(ctx)))
                return localizeMessage_("unclosed '{' - check for a missing closing brace", {});

            if (dynamic_cast<CFlatParser::StructDefinitionContext*>(ctx)       ||
                dynamic_cast<CFlatParser::StructOrUnionSpecifierContext*>(ctx) ||
                dynamic_cast<CFlatParser::ClassDefinitionContext*>(ctx))
                return localizeMessage_("struct/class definitions require a trailing ';'", {});

            if (dynamic_cast<CFlatParser::ImportDeclarationContext*>(ctx))
                return localizeMessage_("import statements require a trailing ';'", {});

            if (dynamic_cast<CFlatParser::ProgramDefinitionContext*>(ctx))
                return localizeMessage_("program definitions require a trailing ';'", {});

            if (dynamic_cast<CFlatParser::FunctionDefinitionContext*>(ctx))
                return localizeMessage_(
                    "check the function signature - missing return type or parameter type?", {});

            ctx = dynamic_cast<antlr4::RuleContext*>(ctx->parent);
        }

        return {};
    }
};
