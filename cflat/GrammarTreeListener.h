#pragma once
#include <antlr4-runtime.h>
#include <format>
#include <iostream>
#include <string>

#include "CFlatParser.h"

// Prints the full parse-tree rule stack for `--grammar --verbose`. Each grammar
// rule is printed indented by its nesting depth, with its source position and the
// matched source text, so a developer can see exactly how the grammar matched a
// file. This mirrors the format of MainListener::PrintContext but is a standalone,
// codegen-free listener used only by LLVMBackend::CheckGrammar.
class GrammarTreeListener : public antlr4::tree::ParseTreeListener
{
public:
    explicit GrammarTreeListener(CFlatParser* parser) : parser_(parser) {}

    void enterEveryRule(antlr4::ParserRuleContext* ctx) override
    {
        const auto& ruleNames = parser_->getRuleNames();
        std::string indent(static_cast<size_t>(depth_) * 2, ' ');
        size_t line = ctx->getStart()->getLine();
        size_t col  = ctx->getStart()->getCharPositionInLine();

        // Keep one rule per line readable: clamp long matches and strip newlines.
        std::string text = ctx->getText();
        for (char& c : text)
            if (c == '\n' || c == '\r' || c == '\t')
                c = ' ';
        if (text.size() > 60)
            text = text.substr(0, 57) + "...";

        std::cout << std::format("{}{} [{},{}] \"{}\"\n",
                                 indent, ruleNames[ctx->getRuleIndex()], line, col, text);
        ++depth_;
    }

    void exitEveryRule(antlr4::ParserRuleContext*) override
    {
        if (depth_ > 0)
            --depth_;
    }

    void visitTerminal(antlr4::tree::TerminalNode*) override {}
    void visitErrorNode(antlr4::tree::ErrorNode*) override {}

private:
    CFlatParser* parser_;
    int depth_ = 0;
};
