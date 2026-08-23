#pragma once

#include "CFlatErrorListener.h"
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
        std::vector<std::string> localizedArguments{
            displayToken(offendingToken, true),
            e.getExpectedTokens().toString(recognizer->getVocabulary())};
        std::vector<std::string> sourceArguments{
            displayToken(offendingToken, false),
            e.getExpectedTokens().toString(recognizer->getVocabulary())};
        std::string msg = localizeMessage_("mismatched input {} expecting {}", localizedArguments);
        std::string sourceMessage = DiagnosticLocalization::FormatSourceTemplate(
            "found {} but expected {}", sourceArguments);
        recognizer->notifyErrorListeners(
            offendingToken, msg,
            std::make_exception_ptr(ParseDiagnosticSourceMessage{std::move(sourceMessage)}));
    }

    void reportUnwantedToken(antlr4::Parser* recognizer) override
    {
        if (inErrorRecoveryMode(recognizer))
            return;
        beginErrorCondition(recognizer);

        antlr4::Token* token = recognizer->getCurrentToken();
        std::vector<std::string> localizedArguments{
            displayToken(token, true),
            getExpectedTokens(recognizer).toString(recognizer->getVocabulary())};
        std::vector<std::string> sourceArguments{
            displayToken(token, false),
            getExpectedTokens(recognizer).toString(recognizer->getVocabulary())};
        std::string msg = localizeMessage_("extraneous input {} expecting {}", localizedArguments);
        std::string sourceMessage = DiagnosticLocalization::FormatSourceTemplate(
            "unexpected {} here; expected {}", sourceArguments);
        recognizer->notifyErrorListeners(
            token, msg,
            std::make_exception_ptr(ParseDiagnosticSourceMessage{std::move(sourceMessage)}));
    }

    void reportMissingToken(antlr4::Parser* recognizer) override
    {
        if (inErrorRecoveryMode(recognizer))
            return;
        beginErrorCondition(recognizer);

        antlr4::Token* token = recognizer->getCurrentToken();
        std::vector<std::string> localizedArguments{
            getExpectedTokens(recognizer).toString(recognizer->getVocabulary()),
            displayToken(token, true)};
        std::vector<std::string> sourceArguments{
            getExpectedTokens(recognizer).toString(recognizer->getVocabulary()),
            displayToken(token, false)};
        std::string msg = localizeMessage_("missing {} at {}", localizedArguments);
        std::string sourceMessage = DiagnosticLocalization::FormatSourceTemplate(
            "missing {} at {}", sourceArguments);
        recognizer->notifyErrorListeners(
            token, msg,
            std::make_exception_ptr(ParseDiagnosticSourceMessage{std::move(sourceMessage)}));
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

        std::vector<std::string> arguments{escapeWSAndQuote(input)};
        std::string msg = localizeMessage_("no viable alternative at input {}", arguments);
        std::string sourceMessage = DiagnosticLocalization::FormatSourceTemplate(
            "cannot understand the code at {}", arguments);
        recognizer->notifyErrorListeners(
            e.getOffendingToken(), msg,
            std::make_exception_ptr(ParseDiagnosticSourceMessage{std::move(sourceMessage)}));
    }

private:
    std::function<std::string(std::string, std::vector<std::string>)> localizeMessage_;

    std::string displayToken(antlr4::Token* token, bool localized)
    {
        std::string display = getTokenErrorDisplay(token);
        if (display == "'<EOF>'")
        {
            std::string endOfFile = localized
                ? localizeMessage_("end of file", {})
                : DiagnosticLocalization::FormatSourceTemplate("end of file", {});
            return "'" + endOfFile + "'";
        }
        if (display == "<EOF>")
            return localized
                ? localizeMessage_("end of file", {})
                : DiagnosticLocalization::FormatSourceTemplate("end of file", {});
        return display;
    }
};

#pragma pop_macro("EOF")
