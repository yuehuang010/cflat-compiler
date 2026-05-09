#pragma once
#include <antlr4-runtime.h>
#include <string>
#include <vector>
#include <set>
#include <sstream>

#include "CFlatParser.h"

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
    CFlatErrorListener(std::string filename, std::vector<std::string> sourceLines)
        : filename_(std::move(filename)), sourceLines_(std::move(sourceLines))
    {}

    void syntaxError(antlr4::Recognizer* recognizer, antlr4::Token* offendingSymbol,
                     size_t line, size_t charPositionInLine,
                     const std::string& msg, std::exception_ptr e) override
    {
        if (!seenLines_.insert(static_cast<int>(line)).second)
            return;

        ParseDiagnostic d;
        d.file    = filename_;
        d.line    = static_cast<int>(line);
        d.col     = static_cast<int>(charPositionInLine);
        d.message = humanizeMessage(msg);
        d.hint    = buildHint(recognizer, offendingSymbol, e);
        diagnostics_.push_back(std::move(d));
    }

    bool hasErrors() const { return !diagnostics_.empty(); }
    const std::vector<ParseDiagnostic>& getDiagnostics() const { return diagnostics_; }

private:
    std::string filename_;
    std::vector<std::string> sourceLines_;
    std::vector<ParseDiagnostic> diagnostics_;
    std::set<int> seenLines_;

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

    std::string buildHint(antlr4::Recognizer* recognizer,
                          antlr4::Token* offendingSymbol,
                          std::exception_ptr /*e*/)
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
                        return "structs cannot implement interfaces; use 'class' instead";
                }
            }
        }

        // Walk the rule context chain looking for known grammar rules.
        auto* parser = dynamic_cast<antlr4::Parser*>(recognizer);
        if (!parser) return {};
        antlr4::RuleContext* ctx = parser->getContext();

        bool offendingIsEof = offendingSymbol &&
                              offendingSymbol->getType() == antlr4::Token::EOF;

        while (ctx)
        {
            // Check inner (more specific) rules before outer ones.

            if (dynamic_cast<CFlatParser::ExpressionStatementContext*>(ctx) ||
                dynamic_cast<CFlatParser::StatementContext*>(ctx)           ||
                dynamic_cast<CFlatParser::DeclarationContext*>(ctx)         ||
                dynamic_cast<CFlatParser::BlockItemContext*>(ctx))
                return "missing ';' at end of statement";

            if (dynamic_cast<CFlatParser::ParameterTypeListContext*>(ctx))
                return "check parameter list — missing type or closing ')'?";

            if (offendingIsEof &&
                (dynamic_cast<CFlatParser::CompoundStatementContext*>(ctx) ||
                 dynamic_cast<CFlatParser::BlockItemListContext*>(ctx)))
                return "unclosed '{' — check for a missing closing brace";

            if (dynamic_cast<CFlatParser::StructDefinitionContext*>(ctx)       ||
                dynamic_cast<CFlatParser::StructOrUnionSpecifierContext*>(ctx) ||
                dynamic_cast<CFlatParser::ClassDefinitionContext*>(ctx))
                return "struct/class definitions require a trailing ';'";

            if (dynamic_cast<CFlatParser::ImportDeclarationContext*>(ctx))
                return "import statements require a trailing ';'";

            if (dynamic_cast<CFlatParser::ProgramDefinitionContext*>(ctx))
                return "program definitions require a trailing ';'";

            if (dynamic_cast<CFlatParser::FunctionDefinitionContext*>(ctx))
                return "check the function signature — missing return type or parameter type?";

            ctx = dynamic_cast<antlr4::RuleContext*>(ctx->parent);
        }

        return {};
    }
};
