#pragma once
#include "antlr4-runtime.h"
#include <vector>

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
        std::vector<antlr4::tree::ParseTree*> pending;
        for (auto it = tree.children.rbegin(); it != tree.children.rend(); ++it)
            pending.push_back(*it);
        while (!pending.empty())
        {
            auto* node = pending.back();
            pending.pop_back();
            if (antlr4::RuleContext::is(*node))
            {
                const auto& children = node->children;
                for (auto it = children.rbegin(); it != children.rend(); ++it)
                    pending.push_back(*it);
            }
            else
                out += node->getText();
        }
    }
};
