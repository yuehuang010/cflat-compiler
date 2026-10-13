#pragma once
#include "antlr4-runtime.h"

// Base of every generated CFlatParser context (CFlat.g4 option contextSuperClass). Overrides
// getText: the runtime's version builds a std::stringstream per subtree level, and each stream
// construction takes the CRT global locale lock, contended across LSP pool threads. Same result:
// concatenation of every terminal's text, depth-first.
class CFlatRuleContext : public antlr4::ParserRuleContext
{
public:
    using antlr4::ParserRuleContext::ParserRuleContext;

    std::string getText() override
    {
        std::string out;
        AppendText(*this, out);
        return out;
    }

private:
    static void AppendText(antlr4::tree::ParseTree& tree, std::string& out)
    {
        for (antlr4::tree::ParseTree* child : tree.children)
        {
            if (antlr4::RuleContext::is(*child))
                AppendText(*child, out);
            else
                out += child->getText();
        }
    }
};
