#pragma once

#include "platform/GeneratedParser.h"
#include <DefaultErrorStrategy.h>
#include <exception>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#pragma push_macro("EOF")
#undef EOF

class CFlatErrorStrategy : public antlr4::DefaultErrorStrategy
{
public:
    explicit CFlatErrorStrategy(
        std::function<std::string(std::string, std::vector<std::string>)> localizeMessage)
    {
        localizeMessage_ = std::move(localizeMessage);
    }

protected:
    void reportInputMismatch(antlr4::Parser* recognizer,
                             const antlr4::InputMismatchException& e) override
    {
        antlr4::Token* offendingToken = e.getOffendingToken();
        std::string msg = localizeMessage_(
            "mismatched input {} expecting {}",
            {displayToken(offendingToken),
             e.getExpectedTokens().toString(recognizer->getVocabulary())});
        recognizer->notifyErrorListeners(offendingToken, msg, std::make_exception_ptr(e));
    }

    void reportUnwantedToken(antlr4::Parser* recognizer) override
    {
        if (inErrorRecoveryMode(recognizer))
            return;
        beginErrorCondition(recognizer);

        antlr4::Token* token = recognizer->getCurrentToken();
        std::string msg = localizeMessage_(
            "extraneous input {} expecting {}",
            {displayToken(token),
             getExpectedTokens(recognizer).toString(recognizer->getVocabulary())});
        recognizer->notifyErrorListeners(token, msg, nullptr);
    }

    void reportMissingToken(antlr4::Parser* recognizer) override
    {
        if (inErrorRecoveryMode(recognizer))
            return;
        beginErrorCondition(recognizer);

        antlr4::Token* token = recognizer->getCurrentToken();
        std::string msg = localizeMessage_(
            "missing {} at {}",
            {getExpectedTokens(recognizer).toString(recognizer->getVocabulary()),
             displayToken(token)});
        recognizer->notifyErrorListeners(token, msg, nullptr);
    }

    void reportNoViableAlternative(antlr4::Parser* recognizer,
                                   const antlr4::NoViableAltException& e) override
    {
        antlr4::TokenStream* tokens = recognizer->getTokenStream();
        std::string input;
        if (tokens != nullptr)
        {
            if (e.getStartToken()->getType() == antlr4::Token::EOF)
                input = "<EOF>";
            else
                input = tokens->getText(e.getStartToken(), e.getOffendingToken());
        }
        else
            input = "<unknown input>";

        std::string msg = localizeMessage_(
            "no viable alternative at input {}", {escapeWSAndQuote(input)});
        recognizer->notifyErrorListeners(e.getOffendingToken(), msg,
                                         std::make_exception_ptr(e));
    }

private:
    std::function<std::string(std::string, std::vector<std::string>)> localizeMessage_;

    std::string displayToken(antlr4::Token* token)
    {
        std::string display = getTokenErrorDisplay(token);
        if (display == "'<EOF>'")
            return "'" + localizeMessage_("end of file", {}) + "'";
        if (display == "<EOF>")
            return localizeMessage_("end of file", {});
        return display;
    }
};

#pragma pop_macro("EOF")
