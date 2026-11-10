#include "MainListener.h"

std::string GenericFunctionTemplateSignature(
        CFlatParser::FunctionDefinitionContext* function, LLVMBackend* compiler)
{
    if (function == nullptr || function->genericTypeParameters() == nullptr) return {};
    auto entries = function->genericTypeParameters()->typeParameterList()->typeParameterEntry();
    std::string signature = std::to_string(entries.size()) + "<";
    std::vector<std::string> parameterNames;
    parameterNames.reserve(entries.size());
    for (auto* entry : entries)
    {
        if (auto* value = entry->valueParameterDeclaration())
        {
            signature += "V;";
            parameterNames.push_back(value->Identifier()->getText());
        }
        else
        {
            signature += "T;";
            parameterNames.push_back(entry->typeSpecifier() != nullptr
                ? entry->typeSpecifier()->getText() : entry->getText());
        }
    }
    signature += ">";
    auto replaceTypeToken = [](std::string& shape, const std::string& from,
                               const std::string& to)
    {
        if (from.empty() || from == to) return;
        const auto isIdent = [](char c) { return (c >= 'a' && c <= 'z')
            || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; };
        size_t at = 0;
        while ((at = shape.find(from, at)) != std::string::npos)
        {
            const bool left = at == 0 || !isIdent(shape[at - 1]);
            const bool right = at + from.size() == shape.size()
                || !isIdent(shape[at + from.size()]);
            if (left && right)
            {
                shape.replace(at, from.size(), to);
                at += to.size();
            }
            else at += from.size();
        }
    };
    if (function->trailingConst() != nullptr) signature += "const;";
    if (auto* list = function->parameterTypeList(); list != nullptr && list->parameterList() != nullptr)
        for (auto* parameter : list->parameterList()->parameterDeclaration())
        {
            auto* specs = parameter->declarationSpecifiers();
            std::string shape = specs != nullptr ? specs->getText() : std::string{};
            if (compiler != nullptr && specs != nullptr)
            {
                std::function<void(antlr4::tree::ParseTree*)> resolveAliases =
                    [&](antlr4::tree::ParseTree* node)
                {
                    if (auto* generic = dynamic_cast<CFlatParser::GenericIdentifierContext*>(node);
                        generic != nullptr && generic->Identifier() != nullptr)
                    {
                        const std::string name = generic->Identifier()->getText();
                        if (std::find(parameterNames.begin(), parameterNames.end(), name)
                            == parameterNames.end())
                            replaceTypeToken(shape, name, CanonicalPrimitiveTypeName(
                                compiler->ResolveTypeAlias(name)));
                    }
                    else if (auto* qualified =
                        dynamic_cast<CFlatParser::QualifiedGenericIdentifierContext*>(node);
                        qualified != nullptr)
                    {
                        const std::string name = qualified->getText();
                        replaceTypeToken(shape, name, CanonicalPrimitiveTypeName(
                            compiler->ResolveTypeAlias(name)));
                    }
                    else if (auto* type = dynamic_cast<CFlatParser::TypeSpecifierContext*>(node);
                        type != nullptr && type->genericIdentifier() == nullptr
                            && type->qualifiedGenericIdentifier() == nullptr
                            && type->tupleTypeSpecifier() == nullptr
                            && type->functionPointerSpecifier() == nullptr)
                    {
                        const std::string name = type->getText();
                        replaceTypeToken(shape, name, CanonicalPrimitiveTypeName(
                            compiler->ResolveTypeAlias(name)));
                    }
                    for (auto* child : node->children) resolveAliases(child);
                };
                resolveAliases(specs);
            }
            if (specs != nullptr)
                for (auto* spec : specs->declarationSpecifier())
                    if (spec->arrayTypeSuffix() != nullptr
                        && spec->arrayTypeSuffix()->getText() == "[]")
                    {
                        const size_t array = shape.rfind("[]");
                        if (array != std::string::npos) shape.replace(array, 2, "*");
                    }
            for (size_t i = 0; i < parameterNames.size(); ++i)
            {
                const auto& name = parameterNames[i];
                size_t at = 0;
                while ((at = shape.find(name, at)) != std::string::npos)
                {
                    const auto isIdent = [](char c) { return (c >= 'a' && c <= 'z')
                        || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; };
                    const bool left = at == 0 || !isIdent(shape[at - 1]);
                    const bool right = at + name.size() == shape.size()
                        || !isIdent(shape[at + name.size()]);
                    if (left && right)
                    {
                        shape.replace(at, name.size(), "#" + std::to_string(i));
                        at += 2;
                    }
                    else at += name.size();
                }
            }
            signature += shape + ",";
        }
    return signature;
}

std::string FlexibleArrayMemberLengthError(const LLVMBackend::TypeAndValue& type)
{
    if (!type.IsFlexibleArrayMember) return {};
    const std::string member = type.VariableName.empty() ? "<unknown>" : type.VariableName;
    return std::format("flexible array member '{}' has no known length", member);
}
std::vector<antlr4::ParserRuleContext*> DirectOperandPostfixes(antlr4::tree::ParseTree* node)
{
    std::vector<antlr4::ParserRuleContext*> layers;
    while (node != nullptr)
    {
        if (auto* postfix = dynamic_cast<CFlatParser::PostfixExpressionContext*>(node))
        {
            layers.push_back(postfix);
            auto* primary = postfix->primaryExpression();
            if (postfix->children.size() == 1 && primary != nullptr && primary->children.size() == 3
                && primary->expression() != nullptr && primary->children[0]->getText() == "(")
            {
                node = primary->expression();
                continue;
            }
            return layers;
        }
        if (auto* unary = dynamic_cast<CFlatParser::UnaryExpressionContext*>(node))
        {
            node = unary->postfixExpression();
            continue;
        }
        if (node->children.size() != 1) return layers;
        node = node->children[0];
    }
    return layers;
}
// `( expression )` as a whole postfix: parentheses keep the value of a flagged `*&X` / `&X` operand.
bool IsParenthesizedPostfix(CFlatParser::PostfixExpressionContext* postfix)
{
    auto* primary = postfix->primaryExpression();
    return postfix->children.size() == 1 && primary != nullptr && primary->children.size() == 3
        && primary->expression() != nullptr && primary->children[0]->getText() == "(";
}

// True when `node` is the whole operand of a prefix '*', reached through single-child rules and parentheses.
bool IsWholeDerefOperand(antlr4::tree::ParseTree* node)
{
    for (auto* p = node->parent; p != nullptr; p = p->parent)
    {
        if (auto* unary = dynamic_cast<CFlatParser::UnaryExpressionContext*>(p))
        {
            if (unary->unaryOperator() != nullptr) return unary->unaryOperator()->getText() == "*";
            continue;
        }
        const bool parenPrimary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(p) != nullptr
            && p->children.size() == 3 && p->children[0]->getText() == "(";
        if (p->children.size() != 1 && !parenPrimary) return false;
    }
    return false;
}

// Operator of the unary expression that is the whole operand (through parentheses); empty when none.
std::string LeadingUnaryOperator(antlr4::tree::ParseTree* castNode)
{
    antlr4::tree::ParseTree* node = castNode;
    while (node != nullptr)
    {
        if (auto* cast = dynamic_cast<CFlatParser::CastExpressionContext*>(node))
        {
            node = cast->unaryExpression();
            continue;
        }
        if (auto* unary = dynamic_cast<CFlatParser::UnaryExpressionContext*>(node))
        {
            if (unary->unaryOperator() != nullptr) return unary->unaryOperator()->getText();
            node = unary->postfixExpression();
            continue;
        }
        if (auto* postfix = dynamic_cast<CFlatParser::PostfixExpressionContext*>(node))
        {
            auto* primary = postfix->primaryExpression();
            if (postfix->children.size() == 1 && primary != nullptr && primary->children.size() == 3
                && primary->expression() != nullptr && primary->children[0]->getText() == "(")
            {
                node = primary->expression();
                continue;
            }
            return {};
        }
        if (node->children.size() != 1) return {};
        node = node->children[0];
    }
    return {};
}
LLVM_ATTRIBUTE_NOINLINE CFlatParser::AssignmentExpressionContext* UnwrapInitializerParens(
    CFlatParser::AssignmentExpressionContext* assign)
{
    while (assign != nullptr)
    {
        antlr4::tree::ParseTree* node = assign;
        while (auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(node))
        {
            if (rule->children.size() != 1) break;
            node = rule->children[0];
        }
        auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node);
        if (primary == nullptr || primary->children.size() != 3 || primary->expression() == nullptr
            || primary->NameOf() != nullptr || primary->TypeOf() != nullptr)
            break;
        auto* inner = primary->expression()->assignmentExpression();
        if (inner == nullptr) break;
        assign = inner;
    }
    return assign;
}
CFlatParser::PostfixExpressionContext* SolePostfixExpressionShape(
    antlr4::tree::ParseTree* node)
{
    if (node == nullptr) return nullptr;
    if (auto* postfix = dynamic_cast<CFlatParser::PostfixExpressionContext*>(node)) return postfix;
    auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(node);
    if (rule == nullptr) return nullptr;
    if (rule->children.size() == 1) return SolePostfixExpressionShape(rule->children[0]);
    if (auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(rule);
        primary != nullptr && primary->expression() != nullptr)
        return SolePostfixExpressionShape(primary->expression());
    return nullptr;
}
std::optional<StringLiteralExpressionInfo> MainListener::StringLiteralExpressionValue(
    antlr4::tree::ParseTree* node)
{
    if (auto* assign = dynamic_cast<CFlatParser::AssignmentExpressionContext*>(node))
        node = UnwrapInitializerParens(assign);
    auto* postfix = SolePostfixExpressionShape(node);
    if (postfix == nullptr || postfix->children.size() != 1
        || postfix->primaryExpression() == nullptr) return std::nullopt;
    auto literals = postfix->primaryExpression()->StringLiteral();
    if (literals.empty()) return std::nullopt;
    StringLiteralExpressionInfo value;
    for (auto* literal : literals)
    {
        const std::string spelling = literal->getText();
        if (!IsRawStringTokenSpelling(spelling))
        {
            const size_t quote = spelling.find('"');
            const std::string prefix = quote == std::string::npos
                ? std::string() : spelling.substr(0, quote);
            if (prefix != "") value.IsNarrow = false;
            if (prefix != "" && prefix != "u8") value.IsNarrowOrUtf8 = false;
            value.HasExpressionSegment = value.HasExpressionSegment
                || HasInterpolationSegment(spelling);
        }
        value.Value += DecodeStringLiteralToken(spelling, /*foldBraces=*/true);
    }
    return value;
}
CFlatParser::ArgumentExpressionListContext* SoleDirectNamedCallArguments(
    antlr4::tree::ParseTree* node, const std::string& functionName)
{
    auto* postfix = SolePostfixExpressionShape(node);
    if (postfix == nullptr || postfix->children.size() != 4
        || postfix->primaryExpression() == nullptr
        || postfix->argumentExpressionList().size() != 1) return nullptr;
    auto* identifier = postfix->primaryExpression()->genericIdentifier();
    if (identifier == nullptr || identifier->genericTypeParameters() != nullptr
        || identifier->Identifier() == nullptr
        || identifier->Identifier()->getText() != functionName) return nullptr;
    return postfix->argumentExpressionList().front();
}
std::string EnumMemberSourceSpelling(antlr4::tree::ParseTree* node)
{
    while (node != nullptr)
    {
        if (auto* postfix = dynamic_cast<CFlatParser::PostfixExpressionContext*>(node))
        {
            auto* primary = postfix->primaryExpression();
            size_t childCount = postfix->children.size();
            while (childCount > 1 && (postfix->children[childCount - 1]->getText() == "++"
                                      || postfix->children[childCount - 1]->getText() == "--"))
                --childCount;
            if (childCount == 1 && primary != nullptr && primary->expression() != nullptr)
            {
                node = primary->expression();
                continue;
            }
            std::string spelling;
            for (size_t i = 0; i < childCount; ++i) spelling += postfix->children[i]->getText();
            return spelling;
        }
        if (auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node);
            primary != nullptr && primary->expression() != nullptr)
        {
            node = primary->expression();
            continue;
        }
        auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(node);
        if (rule == nullptr) return node->getText();
        if (rule->children.size() != 1) return {};
        node = rule->children[0];
    }
    return {};
}
std::string EnumMemberDiagnosticName(const LLVMBackend::NamedVariable& member,
                                            antlr4::tree::ParseTree* expression,
                                            const llvm::GlobalVariable* foundMember)
{
    std::string sourceName = EnumMemberSourceSpelling(expression);
    if (!sourceName.empty()) return sourceName;
    return foundMember != nullptr ? foundMember->getName().str() : member.TypeAndValue.VariableName;
}
antlr4::tree::TerminalNode* BareIdentifierInExpression(
    antlr4::tree::ParseTree* expression)
{
    auto* postfix = SolePostfixExpressionShape(expression);
    if (postfix == nullptr || postfix->children.size() != 1) return nullptr;
    auto* primary = postfix->primaryExpression();
    auto* generic = primary != nullptr ? primary->genericIdentifier() : nullptr;
    return generic != nullptr ? generic->Identifier() : nullptr;
}
antlr4::tree::TerminalNode* GenericFunctionValueIdentifier(
    antlr4::tree::ParseTree* expression, bool allowAddressOf)
{
    if (auto* identifier = dynamic_cast<antlr4::tree::TerminalNode*>(expression))
    {
        auto bareArmIdentifier = [](antlr4::tree::ParseTree* arm)
            -> antlr4::tree::TerminalNode* {
            auto* postfix = SolePostfixExpressionShape(arm);
            if (postfix == nullptr || postfix->children.size() != 1) return nullptr;
            auto* primary = postfix->primaryExpression();
            auto* generic = primary != nullptr ? primary->genericIdentifier() : nullptr;
            return generic != nullptr && generic->genericTypeParameters() == nullptr
                ? generic->Identifier() : nullptr;
        };
        for (auto* ancestor = identifier->parent; ancestor != nullptr;
             ancestor = ancestor->parent)
            if (auto* conditional = dynamic_cast<CFlatParser::ConditionalExpressionContext*>(ancestor))
            {
                if ((conditional->expression() != nullptr
                        && bareArmIdentifier(conditional->expression()) == identifier)
                    || (conditional->conditionalExpression() != nullptr
                        && bareArmIdentifier(conditional->conditionalExpression()) == identifier))
                    return identifier;
            }

        auto* node = identifier->parent;
        while (node != nullptr
            && dynamic_cast<CFlatParser::AssignmentExpressionContext*>(node) == nullptr)
            node = node->parent;
        return node != nullptr && GenericFunctionValueIdentifier(node, allowAddressOf) == identifier
            ? identifier : nullptr;
    }

    auto bareIdentifier = [](antlr4::tree::ParseTree* node)
        -> antlr4::tree::TerminalNode* {
        auto* postfix = SolePostfixExpressionShape(node);
        if (postfix == nullptr || postfix->children.size() != 1) return nullptr;
        auto* primary = postfix->primaryExpression();
        auto* generic = primary != nullptr ? primary->genericIdentifier() : nullptr;
        return generic != nullptr && generic->genericTypeParameters() == nullptr
            ? generic->Identifier() : nullptr;
    };
    if (auto* identifier = bareIdentifier(expression))
        return identifier;

    auto* node = expression;
    while (node != nullptr)
    {
        if (auto* cast = dynamic_cast<CFlatParser::CastExpressionContext*>(node);
            cast != nullptr && cast->castTypeName() != nullptr && cast->castExpression() != nullptr
            && cast->castTypeName()->typeSpecifier().empty()
            && cast->castTypeName()->typeQualifier() == nullptr
            && cast->castTypeName()->abstractDeclarator() == nullptr)
        {
            node = cast->castExpression();
            continue;
        }
        if (auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node);
            primary != nullptr && primary->expression() != nullptr && primary->children.size() == 3)
        {
            node = primary->expression();
            continue;
        }
        auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(node);
        if (rule != nullptr && rule->children.size() == 1)
        {
            node = rule->children[0];
            continue;
        }
        break;
    }

    if (auto* identifier = bareIdentifier(node))
        return identifier;
    auto* unary = dynamic_cast<CFlatParser::UnaryExpressionContext*>(node);
    if (!allowAddressOf || unary == nullptr || unary->unaryOperator() == nullptr
        || unary->unaryOperator()->And() == nullptr || unary->castExpression() == nullptr)
        return nullptr;
    return GenericFunctionValueIdentifier(unary->castExpression(), false);
}
size_t CountCallParens(antlr4::tree::ParseTree* node)
{
    if (node == nullptr) return 0;
    size_t count = 0;
    if (auto* postfix = dynamic_cast<CFlatParser::PostfixExpressionContext*>(node))
        count += postfix->LeftParen().size();
    else if (auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node);
             primary != nullptr && primary->LeftParen() != nullptr)
        ++count;
    for (auto* child : node->children) count += CountCallParens(child);
    return count;
}
bool FieldInitializerIsSingleCall(CFlatParser::AssignmentExpressionContext* expr)
{
    if (expr == nullptr) return true;
    antlr4::tree::ParseTree* node = UnwrapInitializerParens(expr);
    while (auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(node))
    {
        if (rule->children.size() != 1) break;
        node = rule->children[0];
    }
    auto* postfix = dynamic_cast<CFlatParser::PostfixExpressionContext*>(node);
    if (postfix == nullptr || postfix->RightParen().empty()
        || postfix->children.back() != postfix->RightParen().back())
        return false;
    return postfix->LeftParen().size() == 1 && CountCallParens(postfix) == 1;
}
bool IsZeroLengthArrayMemberOperand(const LLVMBackend::TypeAndValue& type)
{
    return type.IsFlexibleArrayMember && type.IsZeroLengthArrayMember;
}
bool HasSoftDeclarationSpecifier(CFlatParser::DeclarationSpecifiersContext* specs,
                                        const std::string& name)
{
    if (specs == nullptr) return false;
    for (auto* spec : specs->declarationSpecifier())
        if (spec->typeSpecifier() != nullptr && spec->typeSpecifier()->getText() == name)
            return true;
    return false;
}
std::string ImportProgramAlias(CFlatParser::ImportDeclarationContext* imp)
{
    if (imp == nullptr || imp->As() == nullptr) return {};
    for (size_t i = 0; i + 1 < imp->children.size(); i++)
        if (imp->children[i] == imp->As())
            return imp->children[i + 1]->getText();
    return {};
}
std::string ImportNamespace(CFlatParser::ImportDeclarationContext* imp)
{
    if (imp == nullptr || imp->children.size() < 2) return {};
    std::string first = imp->children[1]->getText();
    // `cpp` selects C++ binding mode for the named file or import group, whatever its extension.
    if (first == "program" || first == "package" || first == "framework"
        || first == "cpp" || first == "package-vcpkg" || first == "package-nuget")
        return {};
    auto ids = imp->Identifier();
    return ids.empty() ? std::string{} : ids.front()->getText();
}
bool ImportClauseHasWord(antlr4::ParserRuleContext* clause, const char* word)
{
    return clause != nullptr && !clause->children.empty() && clause->children.front()->getText() == word;
}
std::string ValidateImportSoftKeywords(CFlatParser::ImportDeclarationContext* imp)
{
    std::string error;
    ForEachImportClause(imp, [&](antlr4::ParserRuleContext* clause) {
        if (!error.empty() || clause == nullptr || clause->children.empty()) return;
        std::string actual = clause->children.front()->getText();
        if (actual == "lib" || actual == "framework" || actual == "define"
            || actual == "cache" || actual == "from" || actual == "pri")
            return;
        error = std::format("unknown '{}' in import clause", actual);
    });
    return error;
}
std::string ValidateImportLeadingMarker(CFlatParser::ImportDeclarationContext* imp)
{
    if (imp == nullptr || imp->children.size() < 2) return "malformed import declaration";
    if (dynamic_cast<CFlatParser::ImportGroupContext*>(imp->children[1]) != nullptr)
        return {};
    std::string first = imp->children[1]->getText();
    if (auto* literal = dynamic_cast<antlr4::tree::TerminalNode*>(imp->children[1]);
        literal != nullptr && literal->getSymbol()->getType() == CFlatLexer::StringLiteral)
        return {};
    // `cpp` binds the named file as C++ - the marker, not the extension, selects the mode.
    if (first == "program" || first == "package" || first == "framework"
        || first == "cpp" || first == "package-vcpkg" || first == "package-nuget")
        return {};
    return std::format("unknown '{}' in import marker", first);
}
bool IsReturnBlockFunction(CFlatParser::FunctionDefinitionContext* func)
{
    auto* cs = func->compoundStatement();
    if (!cs) return false;
    auto* bil = cs->blockItemList();
    if (!bil) return false;
    auto items = bil->blockItem();
    if (items.size() != 1) return false;
    auto* stmt = items[0]->statement();
    if (!stmt) return false;
    auto* jump = stmt->jumpStatement();
    if (!jump) return false;
    return jump->Return() != nullptr && jump->compoundStatement() != nullptr;
}
std::string FixedArrayPointerTypeMessage(const std::string& typeName)
{
    return std::format(
        "pointer to fixed array '{}[N]*' is not a valid type; pass '{}*' (a fixed array decays to a pointer to its first element).",
        typeName, typeName);
}
std::string SimdPointerTypeMessage(const std::string& spelling, bool fixedArray)
{
    return std::format(
        "{} is not a valid type: a pointer to a simd vector is not supported. Pass the fixed "
        "array '{}[N]' by value, or use '{}.load(array, index)' / '{}.store(vector, array, "
        "index)' to move between memory and vectors.",
        fixedArray ? std::format("pointer to fixed array '{}[N]*'", spelling)
                   : std::format("'{}*'", spelling),
        spelling, spelling, spelling);
}
std::string JoinTypeSpecifierWords(const std::vector<std::string>& words)
{
    std::string spelling;
    for (const auto& word : words)
    {
        if (!spelling.empty()) spelling += " ";
        spelling += word;
    }
    return spelling;
}
bool HasPrimitiveTypeError(const PrimitiveTypeError& error)
{
    return error.kind != PrimitiveTypeErrorKind::None;
}
bool HasLongDoubleTypeArgument(const std::vector<std::string>& typeArgs)
{
    for (std::string arg : typeArgs)
    {
        while (!arg.empty() && (arg.back() == '*' || arg.back() == '[' || arg.back() == ']'))
            arg.pop_back();
        if (arg == "longdouble") return true;
    }
    return false;
}
std::string LocalizePrimitiveTypeError(const LLVMBackend* compiler,
                                              const PrimitiveTypeError& error)
{
    if (compiler == nullptr) return {};
    switch (error.kind)
    {
    case PrimitiveTypeErrorKind::ExpectedType:
        return compiler->LocalizeMessage("expected a type", {});
    case PrimitiveTypeErrorKind::InvalidSpelling:
        return compiler->LocalizeMessage("invalid type spelling '{}'", { error.spelling });
    case PrimitiveTypeErrorKind::InvalidSpellingSuggestion:
        return compiler->LocalizeMessage("invalid type spelling '{}': use '{}'",
            { error.spelling, error.suggestion });
    case PrimitiveTypeErrorKind::None:
        break;
    }
    return {};
}
bool CanonicalizePrimitiveTypeWords(const std::vector<std::string>& words,
                                           std::string& canonical, PrimitiveTypeError& error)
{
    static const std::unordered_map<std::string, std::string> aliases = {
        { "signed char", "i8" }, { "unsigned char", "u8" },
        { "short int", "short" }, { "signed short", "short" },
        { "signed short int", "short" }, { "unsigned short", "u16" },
        { "unsigned short int", "u16" }, { "signed", "int" },
        { "signed int", "int" }, { "unsigned", "u32" },
        { "unsigned int", "u32" }, { "long int", "long" },
        { "signed long", "long" }, { "signed long int", "long" },
        { "unsigned long", "ulong" }, { "unsigned long int", "ulong" },
        { "uint", "u32" }, { "longlong", "i64" }, { "ulonglong", "u64" },
        { "long long", "i64" }, { "long long int", "i64" },
        { "signed long long", "i64" }, { "signed long long int", "i64" },
        { "unsigned long long", "u64" }, { "unsigned long long int", "u64" },
        { "long double", "longdouble" },
        { "char8_t", "c8" }, { "char16_t", "c16" },
        { "char32_t", "c32" }, { "wchar_t", "wchar" },
    };
    if (words.empty())
    {
        error.kind = PrimitiveTypeErrorKind::ExpectedType;
        return false;
    }
    const std::string spelling = JoinTypeSpecifierWords(words);
    if (auto it = aliases.find(spelling); it != aliases.end())
    {
        canonical = it->second;
        return true;
    }
    if (words.size() == 1)
    {
        canonical = words[0];
        return true;
    }

    // A C sign modifier on a CFlat fixed-width word: name the word that has that signedness.
    static const std::unordered_map<std::string, std::string> signedWords = {
        { "i8", "i8" }, { "u8", "i8" }, { "i16", "i16" }, { "u16", "i16" },
        { "i32", "i32" }, { "u32", "i32" }, { "i64", "i64" }, { "u64", "i64" },
        { "i128", "i128" }, { "u128", "i128" },
    };
    static const std::unordered_map<std::string, std::string> unsignedWords = {
        { "i8", "u8" }, { "u8", "u8" }, { "i16", "u16" }, { "u16", "u16" },
        { "i32", "u32" }, { "u32", "u32" }, { "i64", "u64" }, { "u64", "u64" },
        { "i128", "u128" }, { "u128", "u128" },
    };
    if (words.size() == 2 && (words[0] == "signed" || words[0] == "unsigned"))
    {
        const auto& table = words[0] == "signed" ? signedWords : unsignedWords;
        if (auto it = table.find(words[1]); it != table.end())
        {
            error = { PrimitiveTypeErrorKind::InvalidSpellingSuggestion, spelling, it->second };
            return false;
        }
    }
    error = { PrimitiveTypeErrorKind::InvalidSpelling, spelling, {} };
    return false;
}
bool IsSoftKeywordTypeSpecifier(const std::string& text)
{
    return text == "move" || text == "adopt" || text == "alias" || text == "bond"
        || text == "unique" || text == "manifest" || text == "application"
        || text == "virtual" || text == "override";
}
std::vector<std::string> CollectDeclarationTypeSpecifierWords(
    const std::vector<CFlatParser::DeclarationSpecifierContext*>& declSpecs)
{
    std::vector<std::string> words;
    for (auto* declSpec : declSpecs)
    {
        auto* typeSpec = declSpec->typeSpecifier();
        if (typeSpec == nullptr) continue;
        const std::string rootSpelling = RootQualifiedTypeSpelling(typeSpec);
        const std::string text = rootSpelling.empty() ? typeSpec->getText() : rootSpelling;
        if (IsSoftKeywordTypeSpecifier(text)) continue;
        words.push_back(text);
    }
    return words;
}
CFlatParser::DeclarationSpecifierContext* MisplacedMultiWordTypeSuffix(
    const std::vector<CFlatParser::DeclarationSpecifierContext*>& declSpecs,
    std::string& suffixText)
{
    std::vector<CFlatParser::DeclarationSpecifierContext*> typeWordSpecs;
    for (auto* declSpec : declSpecs)
        if (declSpec->typeSpecifier() != nullptr
            && !IsSoftKeywordTypeSpecifier(declSpec->typeSpecifier()->getText()))
            typeWordSpecs.push_back(declSpec);
    for (size_t i = 0; i + 1 < typeWordSpecs.size(); ++i)
    {
        auto* declSpec = typeWordSpecs[i];
        std::string suffix;
        if (declSpec->Question() != nullptr) suffix += "?";
        if (declSpec->pointer() != nullptr) suffix += declSpec->pointer()->getText();
        if (declSpec->arrayTypeSuffix() != nullptr) suffix += declSpec->arrayTypeSuffix()->getText();
        if (!suffix.empty())
        {
            suffixText = suffix;
            return declSpec;
        }
    }
    return nullptr;
}
CFlatParser::DeclarationSpecifierContext* LastDeclarationTypeSpecifier(
    const std::vector<CFlatParser::DeclarationSpecifierContext*>& declSpecs)
{
    for (auto it = declSpecs.rbegin(); it != declSpecs.rend(); ++it)
        if ((*it)->typeSpecifier() != nullptr)
            return *it;
    return nullptr;
}
std::string CanonicalDeclarationTypeName(
    const std::vector<CFlatParser::DeclarationSpecifierContext*>& declSpecs,
    const std::string& fallback)
{
    std::string canonical;
    PrimitiveTypeError error;
    return CanonicalizePrimitiveTypeWords(CollectDeclarationTypeSpecifierWords(declSpecs), canonical, error)
        ? canonical : fallback;
}
bool IsGenericFunctionScalarDestination(LLVMBackend* compiler,
                                               LLVMBackend::TypeAndValue destination)
{
    if (compiler == nullptr || destination.TypeName.empty() || destination.TypeName == "auto"
        || destination.Pointer || destination.ElemPointer || destination.PointerDepth > 0
        || destination.IsArrayView || destination.IsFunctionPointer
        || !destination.FuncPtrReturnTypeName.empty())
        return false;
    destination.TypeName = compiler->ResolveTypeAlias(destination.TypeName);
    return destination.TypeName != "void" && destination.IsPrimitive();
}
std::string GenericFunctionScalarValueDiagnostic(LLVMBackend* compiler,
                                                        const std::string& functionName)
{
    std::string spelling = SpellFunctionSymbol(*compiler, functionName);
    if (spelling == functionName)
        spelling = SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = functionName });
    return std::format(
        "'{}' is a function used as a value - did you mean '{}()'? (a bare function name is only valid as a function<T> value)",
        spelling, spelling);
}
antlr4::tree::TerminalNode* BareGenericFunctionValueIdentifierInExpression(
    antlr4::tree::ParseTree* expression)
{
    auto* postfix = SolePostfixExpressionShape(expression);
    if (postfix == nullptr || postfix->children.size() != 1) return nullptr;
    auto* primary = postfix->primaryExpression();
    auto* generic = primary != nullptr ? primary->genericIdentifier() : nullptr;
    if (generic == nullptr || generic->genericTypeParameters() != nullptr) return nullptr;
    return generic->Identifier();
}
bool NamesVisibleVariable(LLVMBackend* compiler, const std::string& name)
{
    if (compiler->FindVariableScopeDepth(name) != SIZE_MAX
        || compiler->HasMemberVariable(name)
        || compiler->GetGlobalVariableNV(name).Storage != nullptr)
        return true;
    const std::string nsQualified = compiler->ResolveQualifiedName(name);
    return nsQualified != name && compiler->GetGlobalVariableNV(nsQualified).Storage != nullptr;
}
LLVMBackend::TypeAndValue VisibleVariableType(LLVMBackend* compiler, const std::string& name)
{
    if (compiler->FindVariableScopeDepth(name) != SIZE_MAX)
        return compiler->GetScopedLocalOrArgument(name).TypeAndValue;
    if (compiler->HasMemberVariable(name))
        return compiler->GetMemberVariable(name).TypeAndValue;
    // Same order as ParseIdentifier: an enclosing-namespace variable shadows the global of its name.
    if (const std::string nsQualified = compiler->ResolveQualifiedName(name);
        nsQualified != name && compiler->GetGlobalVariableNV(nsQualified).Storage != nullptr)
        return compiler->GetGlobalVariableNV(nsQualified).TypeAndValue;
    return compiler->GetGlobalVariableNV(name).TypeAndValue;
}
antlr4::tree::TerminalNode* BareTypeNameIdentifier(CFlatParser::TypeNameContext* typeName)
{
    if (typeName == nullptr || typeName->abstractDeclarator() != nullptr) return nullptr;
    auto* specCtx = typeName->specifierQualifierList();
    if (specCtx == nullptr) return nullptr;
    auto typeSpecs = specCtx->typeSpecifier();
    if (typeSpecs.size() != 1) return nullptr;
    auto* gid = typeSpecs[0]->genericIdentifier();
    if (gid == nullptr || gid->genericTypeParameters() != nullptr) return nullptr;
    return gid->Identifier();
}
antlr4::tree::TerminalNode* BareCastTypeNameIdentifier(CFlatParser::CastTypeNameContext* typeName)
{
    if (typeName == nullptr || typeName->typeQualifier() != nullptr
        || typeName->abstractDeclarator() != nullptr || typeName->typeSpecifier().size() != 1)
        return nullptr;
    auto* gid = typeName->typeSpecifier()[0]->genericIdentifier();
    if (gid == nullptr || gid->genericTypeParameters() != nullptr) return nullptr;
    return gid->Identifier();
}
std::vector<std::string> TypeSpecifierWords(
    CFlatParser::TypeSpecifierContext* typeSpec,
    CFlatParser::MultiWordTypeSuffixContext* suffix)
{
    std::vector<std::string> words;
    if (typeSpec != nullptr)
    {
        const std::string rootSpelling = RootQualifiedTypeSpelling(typeSpec);
        words.push_back(rootSpelling.empty() ? typeSpec->getText() : rootSpelling);
    }
    if (suffix != nullptr)
        for (auto* part : suffix->multiWordPrimitiveSpecifier())
            words.push_back(part->getText());
    return words;
}
bool IsGlobalScopeQualifiedType(CFlatParser::TypeSpecifierContext* typeSpec)
{
    auto* root = typeSpec != nullptr ? typeSpec->rootQualifiedTypeName() : nullptr;
    return root != nullptr && !root->Identifier().empty()
        && root->Identifier().front()->getText() == "global";
}
std::string RootQualifiedTypeSpelling(CFlatParser::TypeSpecifierContext* typeSpec)
{
    auto* root = typeSpec != nullptr ? typeSpec->rootQualifiedTypeName() : nullptr;
    if (root == nullptr || root->Identifier().size() < 2) return {};
    std::string name = root->Identifier()[1]->getText();
    for (auto* member : root->memberNameToken()) name += "." + member->getText();
    return name;
}
std::string ResolveQualifiedTypeSpecifierName(
    LLVMBackend* compiler, CFlatParser::TypeSpecifierContext* typeSpec,
    const std::string& spelling, bool resolveFromRoot)
{
    if (compiler == nullptr) return spelling;
    const bool forceRoot = resolveFromRoot || IsGlobalScopeQualifiedType(typeSpec);
    std::string name = spelling;
    if (name.empty() && typeSpec != nullptr)
        name = forceRoot ? RootQualifiedTypeSpelling(typeSpec) : typeSpec->getText();
    return compiler->ResolveQualifiedName(name, forceRoot);
}
std::string CanonicalTypeSpecifierText(
    CFlatParser::TypeSpecifierContext* typeSpec,
    CFlatParser::MultiWordTypeSuffixContext* suffix,
    bool,
    PrimitiveTypeError* errorOut)
{
    std::string canonical;
    PrimitiveTypeError error;
    if (!CanonicalizePrimitiveTypeWords(TypeSpecifierWords(typeSpec, suffix), canonical, error))
    {
        if (errorOut != nullptr) *errorOut = error;
        return typeSpec != nullptr ? typeSpec->getText() : std::string{};
    }
    return canonical;
}
std::string CanonicalTemplateTypeArgument(
    CFlatParser::TypeParameterEntryContext* entry, PrimitiveTypeError* errorOut)
{
    if (entry == nullptr) return {};
    return CanonicalTypeSpecifierText(entry->typeSpecifier(), entry->multiWordTypeSuffix(), true,
                                      errorOut);
}
bool DimSpecIsUnsizedMultiDim(CFlatParser::ArrayDimSpecContext* dims)
{
    return dims != nullptr && dims->LeftBracket().size() > 1
        && dims->assignmentExpression().size() < dims->LeftBracket().size();
}
std::string UnsizedMultiDimMessage(const std::string& typeName)
{
    return std::format("'{0}' with an empty '[]' among several dimensions ('{0}[][]', '{0}[][M]', "
        "'{0}[N][]') is not a valid type - a '{0}[]' array view is a thin pointer and carries no "
        "row stride. Write a single '{0}[]' for a flat view of the whole allocation, or size "
        "every dimension ('{0}[N][M]'); a row of a sized 2-D array binds to a '{0}[]' one at a "
        "time.", typeName);
}
int PointerDepthOf(CFlatParser::PointerContext* p)
{
    return p == nullptr ? 0 : (int)p->Star().size();
}
std::vector<CFlatParser::TypeQualifierContext*> ConstQualifiersAfterPointerStar(
    CFlatParser::PointerContext* pointer)
{
    std::vector<CFlatParser::TypeQualifierContext*> result;
    if (pointer == nullptr) return result;
    for (auto* qualifierList : pointer->typeQualifierList())
        for (auto* qualifier : qualifierList->typeQualifier())
            if (qualifier->getText() == "const") result.push_back(qualifier);
    return result;
}
std::string ConstAfterPointerDiagnostic(const std::string& spelledBaseType)
{
    return std::format("'const' after '*' is not allowed: const applies to the whole type. "
        "Write 'const {}' (pointer and target fixed) or '(const {})*' (target read-only)",
        spelledBaseType, spelledBaseType);
}
std::string SpelledTypeNameBase(CFlatParser::TypeNameContext* typeName)
{
    auto* spec = typeName == nullptr ? nullptr : typeName->specifierQualifierList();
    if (spec == nullptr) return "T";
    return SpelledTypeNameBase(spec->typeSpecifier());
}
std::string SpelledTypeNameBase(
    const std::vector<CFlatParser::TypeSpecifierContext*>& typeSpecs)
{
    if (typeSpecs.size() == 1 && typeSpecs.front()->typeName() != nullptr)
        return SpelledTypeNameBase(typeSpecs.front()->typeName());
    std::vector<std::string> words;
    words.reserve(words.size() + typeSpecs.size());
    for (auto* spec : typeSpecs) words.push_back(spec->getText());
    return words.empty() ? "T" : JoinTypeSpecifierWords(words);
}
std::string SpelledTypeNameBase(CFlatParser::TypeSpecifierContext* typeSpec,
                                       CFlatParser::MultiWordTypeSuffixContext* suffix)
{
    if (typeSpec != nullptr && typeSpec->typeName() != nullptr)
        return SpelledTypeNameBase(typeSpec->typeName());
    return JoinTypeSpecifierWords(TypeSpecifierWords(typeSpec, suffix));
}
int PeelAliasPointerStars(std::string& name)
{
    int depth = 0;
    while (!name.empty() && name.back() == '*') { name.pop_back(); depth++; }
    return depth;
}
std::string SignatureComponentAliasBase(LLVMBackend* compiler, std::string name)
{
    if (compiler != nullptr) name = compiler->ResolveTypeAlias(name);
    PeelAliasPointerStars(name);
    if (compiler != nullptr)
    {
        std::string resolved;
        std::string error;
        if (compiler->TryResolveDefaultedCxxGenericBaseAlias(name, resolved, error))
            return resolved;
    }
    return name;
}
bool FoldPointerAliasArg(const LLVMBackend* compiler, std::string& name, int& stars)
{
    if (compiler == nullptr) return false;
    if (compiler->FindFunctionTypeAlias(name) != nullptr) return false;
    std::string aliased = compiler->ResolveTypeAlias(name);
    // The scanner's pre-registered pointer aliases live in a mangling-only table, so that a
    // pointer alias stays order-DEPENDENT for ordinary declarations, exactly like a pure rename.
    if (aliased == name) aliased = compiler->ResolveManglingPointerAlias(name);
    if (aliased.empty() || aliased == name) return false;
    int aliasStars = PeelAliasPointerStars(aliased);
    if (aliasStars == 0 || !IsBareTypeName(aliased)) return false;
    name = compiler->ResolveManglingAlias(compiler->ResolveTypeArgBaseName(aliased));
    stars += aliasStars;
    return true;
}
std::string TupleElementBaseName(std::string argName)
{
    while (argName.ends_with("*") || argName.ends_with("[]"))
        argName.resize(argName.size() - (argName.back() == '*' ? 1 : 2));
    return argName;
}
bool TupleElementEntryNamesType(const LLVMBackend* compiler,
    CFlatParser::TupleTypeEntryContext* entry, const std::string& argName)
{
    if (auto* typeSpec = entry != nullptr ? entry->typeSpecifier() : nullptr; typeSpec != nullptr)
    {
        if (typeSpec->simdTypeSpecifier() != nullptr) return true;
        // `function<R(Args)>` / `Lambda<R(Args)>` carry their signature; a bare `function` does not.
        if (auto* fp = typeSpec->functionPointerSpecifier(); fp != nullptr && fp->typeSpecifier() != nullptr)
            return true;
        // A function-type alias element: the resolved argName is already its encoded closure.
        if (typeSpec->tupleTypeSpecifier() == nullptr)
        {
            PrimitiveTypeError spellError;
            const std::string spelled = CanonicalTypeSpecifierText(
                typeSpec, entry->multiWordTypeSuffix(), false, &spellError);
            if (compiler->FindFunctionTypeAlias(spelled) != nullptr) return true;
        }
    }
    if (compiler->FindFunctionTypeAlias(argName) != nullptr) return true;
    if (compiler->IsKnownTypeName(argName) || compiler->IsTypeArgTypeKey(argName)) return true;
    std::string base = argName;
    while (base.ends_with("*") || base.ends_with("[]"))
        base.resize(base.size() - (base.back() == '*' ? 1 : 2));
    return compiler->IsKnownTypeName(base) || compiler->IsTypeArgTypeKey(base);
}
std::string ResolveTupleTypeElementArgName(
    const TupleTypeElementResolveContext& context,
    CFlatParser::TupleTypeEntryContext* entry,
    PrimitiveTypeError* errorOut)
{
    PrimitiveTypeError error;
    auto* typeSpec = entry != nullptr ? entry->typeSpecifier() : nullptr;
    std::string writtenName = CanonicalTypeSpecifierText(
        typeSpec, entry != nullptr ? entry->multiWordTypeSuffix() : nullptr, false, &error);
    std::string argName;
    auto* compiler = context.Compiler;
    bool substituted = false;
    if (compiler != nullptr)
    {
        LLVMBackend::NamespaceScope namespaceScope(compiler, context.Namespace);
        auto substitution = context.Substitutions != nullptr
            ? context.Substitutions->find(writtenName)
            : std::unordered_map<std::string, std::string>::const_iterator{};
        if (context.Substitutions != nullptr && substitution != context.Substitutions->end())
        {
            argName = substitution->second;
            substituted = true;
        }
        else if (!HasPrimitiveTypeError(error) && context.ResolveType && typeSpec != nullptr)
            argName = context.ResolveType(typeSpec, entry->multiWordTypeSuffix());
        else
            argName = std::move(writtenName);
        // A function-type alias element (using Fn = function<int(int)>) keys like its spelled
        // signature, as a generic type argument does, so the instance field is the closure type.
        if (!substituted)
            if (auto* fit = compiler->FindFunctionTypeAlias(argName); fit != nullptr)
                argName = MainListener::EncodeClosureFromSig(compiler, *fit);
    }
    else
        argName = std::move(writtenName);
    if (errorOut != nullptr) *errorOut = error;

    // Substitutions and nested type names can already carry pointer/view suffixes.
    int pointerDepth = PointerDepthOf(entry != nullptr ? entry->pointer() : nullptr);
    bool isArrayView = entry != nullptr && IsArrayViewArg(entry);
    int elementPointerDepth = isArrayView ? pointerDepth : 0;
    if (isArrayView) pointerDepth = 0;
    bool resolvedArrayView = argName.ends_with("[]");
    if (resolvedArrayView) argName.resize(argName.size() - 2);
    const int resolvedPointerDepth = PeelAliasPointerStars(argName);
    if (resolvedArrayView)
    {
        isArrayView = true;
        elementPointerDepth += resolvedPointerDepth;
    }
    else if (isArrayView)
        elementPointerDepth += resolvedPointerDepth;
    else
        pointerDepth += resolvedPointerDepth;

    if (compiler != nullptr)
    {
        LLVMBackend::NamespaceScope namespaceScope(compiler, context.Namespace);
        if (!substituted)
        {
            const bool namesLocalType = context.LocalTypes.count(argName) != 0;
            std::string enumName = compiler->ResolveEnumTypeName(argName);
            if (!enumName.empty()) argName = std::move(enumName);
            else if (!namesLocalType)
            {
                std::string scannedEnum = compiler->ResolveScannedEnumTypeName(argName);
                if (!scannedEnum.empty()) argName = std::move(scannedEnum);
                else argName = compiler->ResolveTypeArgBaseName(argName);
            }
            FoldPointerAliasArg(compiler, argName,
                isArrayView ? elementPointerDepth : pointerDepth);
        }
    }
    if (isArrayView && entry != nullptr && !IsArrayViewArg(entry)
        && PointerDepthOf(entry->pointer()) > 0)
    {
        isArrayView = false;
        pointerDepth = elementPointerDepth + PointerDepthOf(entry->pointer());
        elementPointerDepth = 0;
    }
    if (isArrayView)
        argName += std::string(elementPointerDepth, '*') + "[]";
    else if (pointerDepth > 0)
        argName += std::string(pointerDepth, '*');

    if (entry != nullptr && entry->Identifier() != nullptr)
    {
        const std::string qualifier = entry->Identifier()->getText();
        if (qualifier != "unique")
        {
            if (context.ReportError != nullptr)
                context.ReportError(entry, std::format(
                    "unknown tuple element qualifier '{}'; only 'unique' is allowed", qualifier));
        }
        if (isArrayView || pointerDepth > 1
            || (pointerDepth == 0 && (compiler == nullptr || !compiler->IsInterfaceType(argName))))
        {
            if (context.ReportError != nullptr)
                context.ReportError(entry,
                    "'unique' tuple elements require one pointer or an interface type");
        }
        const std::string pointee = pointerDepth == 1
            ? argName.substr(0, argName.size() - 1) : argName;
        if (context.ResolveUniqueType != nullptr)
            return context.ResolveUniqueType(pointee, pointerDepth == 1, entry);
        if (compiler != nullptr)
        {
            std::string cxxError;
            const std::string cxxUnique = pointerDepth == 1
                ? compiler->CxxUniquePtrForUniqueKeyword(pointee, cxxError) : std::string();
            if (!cxxError.empty() && context.ReportError != nullptr)
                context.ReportError(entry, std::move(cxxError));
            if (!cxxUnique.empty()) return cxxUnique;
        }
        return compiler != nullptr
            ? MangleGenericInstance(*compiler, "unique", { pointee })
            : std::string("unique__") + pointee;
    }
    return argName;
}
std::string getOperatorName(CFlatParser::OperatorFunctionIdContext* opId)
{
    if (opId->New())          return "operator new";
    if (opId->Delete())       return "operator delete";
    if (opId->String())       return "operator string";
    if (opId->Bool())         return "operator bool";
    if (opId->Plus())         return "operator+";
    if (opId->Minus())        return "operator-";
    if (opId->PlusPlus())     return "operator++";
    if (opId->MinusMinus())   return "operator--";
    if (opId->Star())         return "operator*";
    if (opId->Div())          return "operator/";
    if (opId->Mod())          return "operator%";
    if (opId->And())          return "operator&";
    if (opId->Or())           return "operator|";
    if (opId->Caret())        return "operator^";
    if (opId->PlusAssign())       return "operator+=";
    if (opId->MinusAssign())      return "operator-=";
    if (opId->StarAssign())       return "operator*=";
    if (opId->DivAssign())        return "operator/=";
    if (opId->ModAssign())        return "operator%=";
    if (opId->LeftShiftAssign())  return "operator<<=";
    if (opId->RightShiftAssign()) return "operator>>=";
    if (opId->AndAssign())        return "operator&=";
    if (opId->XorAssign())        return "operator^=";
    if (opId->OrAssign())         return "operator|=";
    if (opId->Assign())           return "operator=";
    if (opId->Equal())        return "operator==";
    if (opId->NotEqual())     return "operator!=";
    if (opId->Spaceship())    return "operator<=>";
    if (opId->Less())         return "operator<";
    if (opId->LessEqual())    return "operator<=";
    if (opId->GreaterEqual()) return "operator>=";
    if (opId->LeftShift())    return "operator<<";
    // '>>' is two Greater tokens; '>' is one. Check before single-Greater.
    if (opId->Greater().size() == 2) return "operator>>";
    if (opId->Greater().size() == 1) return "operator>";
    if (opId->LeftBracket())  return "operator[]";
    if (opId->LeftParen())    return "operator()";
    if (opId->Arrow())        return "operator->";
    if (opId->Not())          return "operator!";
    if (opId->Tilde())        return "operator~";
    if (auto* target = opId->typeName())
        return "operator " + target->getText();
    return "";
}
std::string getOperatorName(CFlatParser::OperatorFunctionIdContext* opId,
                                  const LLVMBackend* compiler)
{
    std::string name = getOperatorName(opId);
    auto* target = opId->typeName();
    if (target == nullptr || compiler == nullptr) return name;

    LLVMBackend::TypeAndValue targetType;
    targetType.TypeName = compiler->ResolveTypeAlias(target->getText());
    if (targetType.TypeName == target->getText())
        targetType.TypeName = compiler->ResolveManglingPointerAlias(target->getText());
    if (targetType.TypeName.empty()) targetType.TypeName = target->getText();
    if (auto* abstractDecl = target->abstractDeclarator(); abstractDecl != nullptr
        && abstractDecl->pointer() != nullptr)
    {
        targetType.Pointer = true;
        targetType.PointerDepth = PointerDepthOf(abstractDecl->pointer());
        targetType.ElemPointer = targetType.PointerDepth >= 2;
    }
    std::string spelling = SpellType(*compiler, targetType);
    return "operator " + (spelling.empty() ? target->getText() : spelling);
}
std::string getInterfaceMethodName(CFlatParser::InterfaceMethodContext* m)
{
    if (auto* opId = m->operatorFunctionId())
        return getOperatorName(opId);
    return m->directDeclarator()->getText();
}
std::string AnnotationArgOne(CFlatParser::AnnotationArgContext* arg)
{
    if (arg != nullptr && arg->StringLiteral() != nullptr)
        return DecodeStringLiteralToken(arg->StringLiteral()->getText(), /*foldBraces=*/true);
    return arg == nullptr ? std::string{} : arg->getText();
}
std::vector<std::string> AnnotationArgTexts(CFlatParser::AnnotationContext* ann)
{
    std::vector<std::string> out;
    if (!ann->annotationArgList()) return out;
    for (auto* arg : ann->annotationArgList()->annotationArg())
        out.push_back(AnnotationArgOne(arg));
    return out;
}
std::string AnnotationArgText(CFlatParser::AnnotationContext* ann)
{
    auto args = AnnotationArgTexts(ann);
    return args.empty() ? std::string{} : args.front();
}
std::vector<LLVMBackend::AnnotationValue> ExtractAnnotations(CFlatParser::AnnotationListContext* annList)
{
    std::vector<LLVMBackend::AnnotationValue> result;
    if (!annList) return result;
    for (auto* ann : annList->annotation())
    {
        if (!ann->Identifier()) continue;
        auto args = AnnotationArgTexts(ann);
        LLVMBackend::AnnotationValue av;
        av.Name = ann->Identifier()->getText();
        av.Value = args.empty() ? std::string{} : args.front();
        av.Values = std::move(args);
        result.push_back(std::move(av));
    }
    return result;
}
bool HasNoTypedCast(antlr4::tree::ParseTree* node) {
    if (auto* cast = dynamic_cast<CFlatParser::CastExpressionContext*>(node);
        cast != nullptr && cast->castTypeName() != nullptr)
        return false;
    for (auto* child : node->children)
        if (!HasNoTypedCast(child)) return false;
    return true;
}
bool HasShortCircuit(antlr4::tree::ParseTree* node) {
    if (node == nullptr) return false;
    if (auto* a = dynamic_cast<CFlatParser::LogicalAndExpressionContext*>(node);
        a != nullptr && a->inclusiveOrExpression().size() > 1)
        return true;
    if (auto* o = dynamic_cast<CFlatParser::LogicalOrExpressionContext*>(node);
        o != nullptr && o->logicalAndExpression().size() > 1)
        return true;
    for (auto* child : node->children)
        if (HasShortCircuit(child)) return true;
    return false;
}
bool HasTernary(antlr4::tree::ParseTree* node) {
    if (node == nullptr) return false;
    if (auto* c = dynamic_cast<CFlatParser::ConditionalExpressionContext*>(node);
        c != nullptr && c->expression() != nullptr)
        return true;
    for (auto* child : node->children)
        if (HasTernary(child)) return true;
    return false;
}
bool IsStructurallyGrouped(antlr4::tree::ParseTree* node) {
    return HasShortCircuit(node) || HasTernary(node);
}
bool IsGroupedFoldCandidate(antlr4::tree::ParseTree* node) {
    return IsStructurallyGrouped(node) && HasNoTypedCast(node);
}
std::optional<int64_t> FoldCompileTimeInt(LLVMBackend* compiler, antlr4::tree::ParseTree* node) {
        if (node == nullptr) return std::nullopt;
        if (auto* c = dynamic_cast<CFlatParser::ConditionalExpressionContext*>(node))
        {
            if (c->expression() != nullptr)
            {
                auto cond = FoldCompileTimeInt(compiler, c->logicalOrExpression());
                if (!cond) return std::nullopt;
                return (*cond != 0) ? FoldCompileTimeInt(compiler, c->expression())
                                    : FoldCompileTimeInt(compiler, c->conditionalExpression());
            }
            if (c->children.size() > 1) return std::nullopt;  // `??` null-coalescing
            return FoldCompileTimeInt(compiler, c->logicalOrExpression());
        }
        if (auto* o = dynamic_cast<CFlatParser::LogicalOrExpressionContext*>(node))
        {
            auto operands = o->logicalAndExpression();
            if (operands.size() == 1) return FoldCompileTimeInt(compiler, operands[0]);
            bool allKnownFalse = true;
            for (auto* op : operands)
            {
                auto v = FoldCompileTimeInt(compiler, op);
                if (v && *v != 0) return (int64_t)1;
                if (!v) allKnownFalse = false;
            }
            return allKnownFalse ? std::optional<int64_t>(0) : std::nullopt;
        }
        if (auto* a = dynamic_cast<CFlatParser::LogicalAndExpressionContext*>(node))
        {
            auto operands = a->inclusiveOrExpression();
            if (operands.size() == 1) return FoldCompileTimeInt(compiler, operands[0]);
            bool allKnownTrue = true;
            for (auto* op : operands)
            {
                auto v = FoldCompileTimeInt(compiler, op);
                if (v && *v == 0) return (int64_t)0;
                if (!v) allKnownTrue = false;
            }
            return allKnownTrue ? std::optional<int64_t>(1) : std::nullopt;
        }
        if (auto* e = dynamic_cast<CFlatParser::ExpressionContext*>(node))
            return FoldCompileTimeInt(compiler, e->assignmentExpression());
        if (auto* asn = dynamic_cast<CFlatParser::AssignmentExpressionContext*>(node))
        {
            if (asn->conditionalExpression() != nullptr)
                return FoldCompileTimeInt(compiler, asn->conditionalExpression());
            return std::nullopt;  // an actual assignment is not a constant
        }
        return FoldCompileTimeIntLeaf(compiler, node);
    }

static std::optional<LLVMBackend::TypeAndValue> ScanTimeGlobalVariableType(
    LLVMBackend* compiler, const std::string& name)
{
    if (compiler == nullptr || name.empty()) return std::nullopt;
    const std::string resolved = compiler->ResolveQualifiedName(name);
    if (auto scanType = compiler->GetScanTimeGlobalVariableType(resolved)) return scanType;
    auto global = compiler->GetGlobalVariableNV(resolved);
    if (global.Storage != nullptr || !global.TypeAndValue.TypeName.empty())
        return global.TypeAndValue;
    if (resolved != name)
    {
        if (auto scanType = compiler->GetScanTimeGlobalVariableType(name)) return scanType;
        global = compiler->GetGlobalVariableNV(name);
        if (global.Storage != nullptr || !global.TypeAndValue.TypeName.empty())
            return global.TypeAndValue;
    }
    return std::nullopt;
}

static std::optional<LLVMBackend::TypeAndValue> ScanTimeVariableType(
    LLVMBackend* compiler, const std::string& name)
{
    if (compiler == nullptr || name.empty()) return std::nullopt;
    auto local = compiler->GetScopedLocalOrArgument(name);
    if (local.Storage != nullptr || !local.TypeAndValue.TypeName.empty())
        return local.TypeAndValue;
    auto argument = compiler->GetFunctionArgument(name);
    if (argument.GetValue() != nullptr || !argument.TypeAndValue.TypeName.empty())
        return argument.TypeAndValue;
    if (name == "this")
    {
        auto thisValue = compiler->GetThisPointer();
        if (!thisValue.TypeAndValue.TypeName.empty()) return thisValue.TypeAndValue;
    }
    if (const auto* field = compiler->FindImplicitThisField(name, nullptr, nullptr))
        return field->TypeAndValue;
    if (auto scanType = compiler->GetScanTimeVariableType(name)) return scanType;
    return ScanTimeGlobalVariableType(compiler, name);
}

static std::optional<LLVMBackend::TypeAndValue> ScanTimeCountofOperandType(
    LLVMBackend* compiler, antlr4::tree::ParseTree* node)
{
    if (node == nullptr) return std::nullopt;
    if (auto* postfix = dynamic_cast<CFlatParser::PostfixExpressionContext*>(node))
    {
        auto* primary = postfix->primaryExpression();
        if (primary == nullptr) return std::nullopt;
        if (primary->expression() != nullptr)
        {
            if (postfix->children.size() != 1) return std::nullopt;
            return ScanTimeCountofOperandType(compiler, primary->expression());
        }
        LLVMBackend::TypeAndValue type;
        if (auto* generic = primary->genericIdentifier(); generic != nullptr
            && generic->Identifier() != nullptr)
        {
            const std::string baseName = generic->Identifier()->getText();
            auto resolved = ScanTimeVariableType(compiler, baseName);
            if (!resolved)
            {
                std::string qualifiedName = baseName;
                bool hasMember = false;
                bool isQualifiedPath = true;
                const auto& children = postfix->children;
                for (size_t i = 1; i < children.size();)
                {
                    auto* dot = dynamic_cast<antlr4::tree::TerminalNode*>(children[i]);
                    auto* member = i + 1 < children.size()
                        ? dynamic_cast<CFlatParser::MemberNameTokenContext*>(children[i + 1]) : nullptr;
                    if (dot == nullptr || dot->getSymbol()->getType() != CFlatParser::Dot
                        || member == nullptr)
                    {
                        isQualifiedPath = false;
                        break;
                    }
                    qualifiedName += "." + member->getText();
                    hasMember = true;
                    i += 2;
                }
                if (isQualifiedPath && hasMember)
                    return ScanTimeGlobalVariableType(compiler, qualifiedName);
                return std::nullopt;
            }
            type = std::move(*resolved);
        }
        else return std::nullopt;

        const auto& children = postfix->children;
        for (size_t i = 1; i < children.size(); ++i)
        {
            auto* terminal = dynamic_cast<antlr4::tree::TerminalNode*>(children[i]);
            if (terminal == nullptr) return std::nullopt;
            const std::string suffix = terminal->getText();
            if (suffix == "[" && i + 2 < children.size()
                && dynamic_cast<CFlatParser::ExpressionContext*>(children[i + 1]) != nullptr
                && children[i + 2]->getText() == "]")
            {
                if (type.IsArrayView || (type.ConstArraySize == 0 && type.AliasArraySize == 0))
                    return std::nullopt;
                if (type.ConstArraySize != 0)
                {
                    type.ConstArraySize = type.ConstInnerDimensions.empty()
                        ? 0 : type.ConstInnerDimensions.front();
                    if (!type.ConstInnerDimensions.empty())
                        type.ConstInnerDimensions.erase(type.ConstInnerDimensions.begin());
                }
                else
                {
                    type.ConstArraySize = type.AliasInnerDims.empty()
                        ? 0 : type.AliasInnerDims.front();
                    type.AliasArraySize = 0;
                    if (!type.AliasInnerDims.empty())
                        type.AliasInnerDims.erase(type.AliasInnerDims.begin());
                }
                i += 2;
                continue;
            }
            if ((suffix == "." || suffix == "->") && i + 1 < children.size())
            {
                auto* member = dynamic_cast<CFlatParser::MemberNameTokenContext*>(children[i + 1]);
                if (member == nullptr) return std::nullopt;
                const std::string owner = compiler->ResolveTypeAlias(type.TypeName);
                auto structure = compiler->GetDataStructure(owner);
                if (structure.StructType == nullptr)
                    structure = compiler->GetDataStructure(type.TypeName);
                if (structure.StructType == nullptr) return std::nullopt;
                bool matched = false;
                for (const auto& field : structure.StructFields)
                    if (field.VariableName == member->getText())
                    {
                        type = static_cast<const LLVMBackend::TypeAndValue&>(field);
                        matched = true;
                        break;
                    }
                if (!matched) return std::nullopt;
                ++i;
                continue;
            }
            return std::nullopt;
        }
        return type;
    }
    auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(node);
    if (rule != nullptr && rule->children.size() == 1)
        return ScanTimeCountofOperandType(compiler, rule->children.front());
    return std::nullopt;
}

static std::optional<int64_t> FoldScanTimeCountof(
    LLVMBackend* compiler, CFlatParser::PostfixExpressionContext* call)
{
    if (compiler == nullptr || call == nullptr || call->children.size() != 4
        || call->primaryExpression() == nullptr
        || call->primaryExpression()->genericIdentifier() == nullptr
        || call->primaryExpression()->genericIdentifier()->Identifier() == nullptr
        || call->primaryExpression()->genericIdentifier()->Identifier()->getText() != "countof"
        || call->argumentExpressionList().size() != 1)
        return std::nullopt;

    auto local = compiler->GetScopedLocalOrArgument("countof");
    auto argument = compiler->GetFunctionArgument("countof");
    if (local.Storage != nullptr || !local.TypeAndValue.TypeName.empty()
        || argument.GetValue() != nullptr || !argument.TypeAndValue.TypeName.empty()
        || compiler->FindImplicitThisField("countof", nullptr, nullptr) != nullptr)
        return std::nullopt;
    const std::string resolvedName = compiler->ResolveQualifiedName("countof");
    auto global = compiler->GetGlobalVariableNV(resolvedName);
    if (global.Storage != nullptr || !global.TypeAndValue.TypeName.empty()) return std::nullopt;
    if (compiler->GetScanTimeGlobalVariableType(resolvedName)
        || compiler->GetScanTimeVariableType("countof")
        || compiler->GetScanTimeGlobalVariableType("countof"))
        return std::nullopt;
    const std::string genericBase = compiler->ResolveGenericFunctionBase(resolvedName);
    if (compiler->GetFunction(resolvedName) != nullptr
        || compiler->GetReturnBlock(resolvedName) != nullptr
        || compiler->IsGenericFunctionKeyInNamespace(genericBase, compiler->GetCurrentNamespace()))
        return std::nullopt;

    const auto* currentFunction = compiler->GetCurrentFunctionSymbol();
    if (currentFunction != nullptr && currentFunction->IsMethod
        && !currentFunction->Parameters.empty())
    {
        const std::string owner = compiler->ResolveTypeAlias(currentFunction->Parameters.front().TypeName);
        if (compiler->GetFunction(owner + ".countof") != nullptr
            || compiler->IsGenericFunctionKeyInNamespace(owner + ".countof", compiler->GetCurrentNamespace()))
            return std::nullopt;
    }

    auto args = call->argumentExpressionList(0)->argumentNamedExpression();
    if (args.size() != 1 || args[0]->assignmentExpression() == nullptr)
        return std::nullopt;
    auto type = ScanTimeCountofOperandType(compiler, args[0]->assignmentExpression());
    if (!type) return std::nullopt;
    const uint64_t extent = type->ConstArraySize != 0
        ? type->ConstArraySize : type->AliasArraySize;
    if (extent == 0 || type->IsArrayView || type->IsUnique
        || compiler->IsCoreUniqueType(type->TypeName)
        || compiler->IsCoreUniqueType(compiler->ResolveTypeAlias(type->TypeName)))
        return std::nullopt;
    return extent <= INT64_MAX ? std::optional<int64_t>(static_cast<int64_t>(extent)) : std::nullopt;
}

bool HasUnfoldableCountofCall(LLVMBackend* compiler, antlr4::tree::ParseTree* node)
{
    if (node == nullptr) return false;
    if (auto* call = dynamic_cast<CFlatParser::PostfixExpressionContext*>(node))
    {
        auto* primary = call->primaryExpression();
        auto* identifier = primary != nullptr && primary->genericIdentifier() != nullptr
            ? primary->genericIdentifier()->Identifier() : nullptr;
        if (identifier != nullptr && identifier->getText() == "countof"
            && call->children.size() == 4 && call->argumentExpressionList().size() == 1
            && !FoldScanTimeCountof(compiler, call))
            return true;
    }
    for (auto* child : node->children)
        if (HasUnfoldableCountofCall(compiler, child)) return true;
    return false;
}

void SeedScanTimeLocalVariableTypes(
    LLVMBackend* compiler, CFlatParser::DeclarationContext* declaration)
{
    if (compiler == nullptr || declaration == nullptr) return;
    auto* specs = declaration->declarationSpecifiers();
    auto* initList = declaration->initDeclaratorList();
    auto* lastSpec = specs != nullptr
        ? LastDeclarationTypeSpecifier(specs->declarationSpecifier()) : nullptr;
    if (specs == nullptr || initList == nullptr || lastSpec == nullptr
        || lastSpec->typeSpecifier() == nullptr)
        return;

    std::string baseName = CanonicalTypeSpecifierText(lastSpec->typeSpecifier());
    baseName = compiler->ResolveQualifiedName(baseName);
    std::string aliasBase = compiler->ResolveTypeAlias(baseName);
    std::vector<uint64_t> aliasDims;
    PeelAliasArrayDims(aliasBase, aliasDims);

    LLVMBackend::TypeAndValue value{ .TypeName = aliasBase };
    value.IsUnique = HasSoftDeclarationSpecifier(specs, "unique");
    for (auto* spec : specs->declarationSpecifier())
    {
        if (spec->pointer() != nullptr) value.Pointer = true;
    }
    if (auto* dimSpec = ArrayDimsOf(lastSpec); dimSpec != nullptr)
    {
        auto dimensions = dimSpec->assignmentExpression();
        if (dimensions.empty())
        {
            value.IsArrayView = true;
            value.Pointer = true;
        }
        else
        {
            std::vector<uint64_t> extents;
            for (auto* dimension : dimensions)
            {
                auto folded = FoldCompileTimeInt(compiler, dimension);
                if (!folded || *folded <= 0) return;
                extents.push_back(static_cast<uint64_t>(*folded));
            }
            value.ConstArraySize = extents.front();
            value.ConstInnerDimensions.assign(extents.begin() + 1, extents.end());
        }
    }
    if (!aliasDims.empty())
    {
        value.AliasArraySize = aliasDims.front();
        value.AliasInnerDims.assign(aliasDims.begin() + 1, aliasDims.end());
    }

    for (auto* init : initList->initDeclarator())
    {
        auto* declarator = init->declarator();
        auto* direct = declarator != nullptr ? declarator->directDeclarator() : nullptr;
        const std::string name = getDirectDeclName(direct);
        if (name.empty()) continue;
        compiler->SetScanTimeVariableType(name, value);
    }
}

std::optional<int64_t> FoldCompileTimeIntLeaf(LLVMBackend* compiler, antlr4::tree::ParseTree* node) {
        if (node == nullptr) return std::nullopt;

        // Left-associative binary chains: fold pairwise using the operator token between operands.
        auto foldChain = [&](const std::vector<antlr4::tree::ParseTree*>& ops,
                            antlr4::RuleContext* parent) -> std::optional<int64_t>
        {
            if (ops.empty()) return std::nullopt;
            auto acc = FoldCompileTimeIntLeaf(compiler, ops[0]);
            if (ops.size() == 1 || !acc) return acc;
            // Walk the parent's children to recover the operator tokens in source order.
            size_t opIdx = 0;
            for (size_t i = 0; i < parent->children.size(); i++)
            {
                auto* term = dynamic_cast<antlr4::tree::TerminalNode*>(parent->children[i]);
                if (term == nullptr) continue;
                std::string op = term->getText();
                if (op == "<" || op == ">")
                {
                    // '>' '>' is a shift written as two tokens; join it when adjacent.
                    if (i + 1 < parent->children.size())
                        if (auto* n2 = dynamic_cast<antlr4::tree::TerminalNode*>(parent->children[i + 1]))
                            if (op == ">" && n2->getText() == ">") { op = ">>"; i++; }
                }
                if (++opIdx > ops.size() - 1) break;
                auto rhs = FoldCompileTimeIntLeaf(compiler, ops[opIdx]);
                if (!rhs) return std::nullopt;
                int64_t l = *acc, r = *rhs, out = 0;
                // Every operand is already inside the 32-bit range (ParseScannerIntegerLiteral and
                // the macro leaf both guarantee it), so computing in int64 and rejecting an
                // out-of-range RESULT means no fold can differ from codegen's i32 arithmetic by a
                // wraparound - it becomes undecidable instead.
                if (op == "|")       out = l | r;
                else if (op == "^")  out = l ^ r;
                else if (op == "&")  out = l & r;
                else if (op == "==") out = (l == r) ? 1 : 0;
                else if (op == "!=") out = (l != r) ? 1 : 0;
                else if (op == "<")  out = (l < r) ? 1 : 0;
                else if (op == ">")  out = (l > r) ? 1 : 0;
                else if (op == "<=") out = (l <= r) ? 1 : 0;
                else if (op == ">=") out = (l >= r) ? 1 : 0;
                else if (op == "<<") { if (r < 0 || r > 31) return std::nullopt; out = l << r; }
                else if (op == ">>") { if (r < 0 || r > 31) return std::nullopt; out = l >> r; }
                else if (op == "+")  { if (ScannerAddOverflow(l, r, &out)) return std::nullopt; }
                else if (op == "-")  { if (ScannerSubOverflow(l, r, &out)) return std::nullopt; }
                else if (op == "*")  { if (ScannerMulOverflow(l, r, &out)) return std::nullopt; }
                else if (op == "/")  { if (r == 0) return std::nullopt; out = l / r; }
                else if (op == "%")  { if (r == 0) return std::nullopt; out = l % r; }
                else return std::nullopt;
                if (!InScannerInt32Range(out)) return std::nullopt;
                acc = out;
            }
            return acc;
        };

        if (auto* n = dynamic_cast<CFlatParser::InclusiveOrExpressionContext*>(node))
        {
            auto ops = n->exclusiveOrExpression();
            return foldChain({ops.begin(), ops.end()}, n);
        }
        if (auto* n = dynamic_cast<CFlatParser::ExclusiveOrExpressionContext*>(node))
        {
            auto ops = n->andExpression();
            return foldChain({ops.begin(), ops.end()}, n);
        }
        if (auto* n = dynamic_cast<CFlatParser::AndExpressionContext*>(node))
        {
            auto ops = n->equalityExpression();
            return foldChain({ops.begin(), ops.end()}, n);
        }
        if (auto* n = dynamic_cast<CFlatParser::EqualityExpressionContext*>(node))
        {
            auto ops = n->typeCheckExpression();
            return foldChain({ops.begin(), ops.end()}, n);
        }
        if (auto* n = dynamic_cast<CFlatParser::TypeCheckExpressionContext*>(node))
        {
            // `is` / `as` are not integer constants; a bare relational passes through.
            if (n->children.size() != 1) return std::nullopt;
            return FoldCompileTimeIntLeaf(compiler, n->relationalExpression());
        }
        if (auto* n = dynamic_cast<CFlatParser::RelationalExpressionContext*>(node))
        {
            auto ops = n->spaceshipExpression();
            return foldChain({ops.begin(), ops.end()}, n);
        }
        if (auto* n = dynamic_cast<CFlatParser::SpaceshipExpressionContext*>(node))
        {
            if (n->Spaceship() != nullptr) return std::nullopt;
            auto ops = n->shiftExpression();
            return foldChain({ops.begin(), ops.end()}, n);
        }
        if (auto* n = dynamic_cast<CFlatParser::ShiftExpressionContext*>(node))
        {
            auto ops = n->additiveExpression();
            return foldChain({ops.begin(), ops.end()}, n);
        }
        if (auto* n = dynamic_cast<CFlatParser::AdditiveExpressionContext*>(node))
        {
            auto ops = n->multiplicativeExpression();
            return foldChain({ops.begin(), ops.end()}, n);
        }
        if (auto* n = dynamic_cast<CFlatParser::MultiplicativeExpressionContext*>(node))
        {
            auto ops = n->castExpression();
            return foldChain({ops.begin(), ops.end()}, n);
        }
        if (auto* n = dynamic_cast<CFlatParser::CastExpressionContext*>(node))
        {
            if (n->castTypeName() != nullptr)
            {
                // A C-cast to an integer (or enum) target folds as the main pass's truncation;
                // every other target leaves the condition undecidable.
                if (compiler == nullptr) return std::nullopt;
                auto v = FoldCompileTimeIntLeaf(compiler, n->castExpression());
                if (!v) return std::nullopt;
                std::string targetTypeName = ResolveCastTypeNameForScanner(compiler, n->castTypeName());
                if (targetTypeName.empty()) return std::nullopt;
                int64_t out = 0;
                if (!compiler->TryGetScanTimeIntegerCast(targetTypeName, *v, out))
                    return std::nullopt;
                return InScannerInt32Range(out) ? std::optional<int64_t>(out) : std::nullopt;
            }
            if (n->unaryExpression() != nullptr) return FoldCompileTimeIntLeaf(compiler, n->unaryExpression());
            return ParseScannerIntegerLiteral(n->getText());
        }
        if (auto* n = dynamic_cast<CFlatParser::UnaryExpressionContext*>(node))
        {
            if (n->offsetofFieldPath() != nullptr
                && n->getStart()->getText() == "__builtin_offsetof")
            {
                if (compiler == nullptr) return std::nullopt;
                std::vector<std::string> path;
                for (auto* part : n->offsetofFieldPath()->Identifier()) path.push_back(part->getText());
                int64_t offset = 0;
                if (!compiler->TryGetScanTimeFieldOffset(n->typeName()->getText(), path, offset))
                    return std::nullopt;
                return InScannerInt32Range(offset) ? std::optional<int64_t>(offset) : std::nullopt;
            }
            // `sizeof '(' typeName ')'` - exactly four children, so `sizeof alignof(T)` and the
            // expression form are not mistaken for it. Aggregates answer undecidable.
            if (n->typeName() != nullptr && n->children.size() == 4 && !n->Sizeof().empty())
            {
                int64_t bytes = 0;
                if (compiler == nullptr
                    || !compiler->TryGetScanTimeTypeSize(n->typeName()->getText(), bytes))
                    return std::nullopt;
                return InScannerInt32Range(bytes) ? std::optional<int64_t>(bytes) : std::nullopt;
            }
            // Any other `sizeof` measures its operand; folding the operand's VALUE would be wrong.
            if (!n->Sizeof().empty()) return std::nullopt;
            if (n->postfixExpression() != nullptr) return FoldCompileTimeIntLeaf(compiler, n->postfixExpression());
            if (n->unaryOperator() != nullptr && n->castExpression() != nullptr)
            {
                auto v = FoldCompileTimeIntLeaf(compiler, n->castExpression());
                if (!v) return std::nullopt;
                std::string op = n->unaryOperator()->getText();
                if (op == "!") return (*v == 0) ? (int64_t)1 : (int64_t)0;
                if (op == "+") return *v;
                int64_t out = 0;
                if (op == "-")      { if (ScannerSubOverflow((int64_t)0, *v, &out)) return std::nullopt; }
                else if (op == "~") out = ~*v;
                else return std::nullopt;  // '&' / '*' are not integer constants
                return InScannerInt32Range(out) ? std::optional<int64_t>(out) : std::nullopt;
            }
            return std::nullopt;      // sizeof/alignof/new/delete/move/operator
        }
        if (auto* n = dynamic_cast<CFlatParser::PostfixExpressionContext*>(node))
        {
            if (auto count = FoldScanTimeCountof(compiler, n)) return count;
            if (compiler != nullptr && n->primaryExpression() != nullptr
                && n->primaryExpression()->genericIdentifier() != nullptr
                && n->primaryExpression()->genericIdentifier()->Identifier() != nullptr
                && n->primaryExpression()->genericIdentifier()->Identifier()->getText() == "offsetof"
                && n->children.size() == 4 && n->argumentExpressionList().size() == 1
                && compiler->GetScopedLocalOrArgument("offsetof").Storage == nullptr
                && compiler->GetFunctionArgument("offsetof").GetValue() == nullptr
                && compiler->GetMemberVariable("offsetof").Storage == nullptr
                && compiler->GetGlobalVariableNV("offsetof").Storage == nullptr
                && compiler->GetFunction("offsetof") == nullptr)
            {
                auto args = n->argumentExpressionList(0)->argumentNamedExpression();
                if (args.size() == 2 && args[0]->assignmentExpression() != nullptr
                    && args[1]->assignmentExpression() != nullptr)
                {
                    const std::string typeText = args[0]->assignmentExpression()->getText();
                    const std::string pathText = args[1]->assignmentExpression()->getText();
                    std::vector<std::string> path;
                    size_t begin = 0;
                    while (begin < pathText.size())
                    {
                        const size_t dot = pathText.find('.', begin);
                        path.push_back(pathText.substr(begin,
                            dot == std::string::npos ? std::string::npos : dot - begin));
                        if (dot == std::string::npos) break;
                        begin = dot + 1;
                    }
                    int64_t offset = 0;
                    if (compiler->TryGetScanTimeFieldOffset(typeText, path, offset))
                        return InScannerInt32Range(offset) ? std::optional<int64_t>(offset) : std::nullopt;
                }
            }
            // Only a bare primary folds - a call, index or non-enum member access does not.
            if (n->children.size() == 1) return FoldCompileTimeIntLeaf(compiler, n->primaryExpression());
            // `Dir.Back`: an enum member is a compile-time constant, registered in both passes.
            if (n->children.size() == 3 && n->Dot().size() == 1 && compiler != nullptr
                && n->children[1] == n->Dot(0))
            {
                auto* prim = n->primaryExpression();
                auto* member = dynamic_cast<CFlatParser::MemberNameTokenContext*>(n->children[2]);
                auto* gid = prim != nullptr ? prim->genericIdentifier() : nullptr;
                if (member != nullptr && gid != nullptr && gid->genericTypeParameters() == nullptr
                    && gid->Identifier() != nullptr)
                {
                    int64_t ev = 0;
                    if (compiler->TryGetEnumMemberInt(gid->Identifier()->getText(),
                                                      member->getText(), ev))
                        return InScannerInt32Range(ev) ? std::optional<int64_t>(ev) : std::nullopt;
                }
            }
            // C++ enum members keep their fully qualified spelling in the global registry, so
            // use that key for nested owners and unscoped namespace members.
            if (compiler != nullptr)
            {
                auto global = compiler->GetGlobalVariableNV(n->getText());
                if (global.Storage != nullptr && !global.TypeAndValue.EnumBacking.empty())
                {
                    auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(global.Storage);
                    auto* ci = gv != nullptr && gv->hasInitializer()
                        ? llvm::dyn_cast<llvm::ConstantInt>(gv->getInitializer()) : nullptr;
                    if (ci != nullptr && ci->getBitWidth() <= 64)
                        return InScannerInt32Range(ci->getSExtValue())
                            ? std::optional<int64_t>(ci->getSExtValue()) : std::nullopt;
                }
            }
            return std::nullopt;
        }
        if (auto* n = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node))
        {
            if (n->expression() != nullptr && n->children.size() == 3)
                return FoldCompileTimeIntLeaf(compiler, n->expression());   // redundant parens
            if (n->expression() != nullptr) return std::nullopt;  // nameof/typeof(expr)
            if (auto* gid = n->genericIdentifier())
                return FoldCompileTimeIdentifier(compiler, gid);
            return ParseScannerIntegerLiteral(n->getText());
        }
        // A bare `genericIdentifier` node, which is what the generic-argument funnels hand us for
        // `Buf<int, CAP>` - it never reaches this folder wrapped in a primaryExpression.
        if (auto* gid = dynamic_cast<CFlatParser::GenericIdentifierContext*>(node))
            return FoldCompileTimeIdentifier(compiler, gid);
        if (auto* e = dynamic_cast<CFlatParser::ExpressionContext*>(node))
            return FoldCompileTimeInt(compiler, e);
        return std::nullopt;
    }
bool ConstLocalFoldFits(llvm::IntegerType* type, int64_t value)
{
    if (type == nullptr || type->getBitWidth() < 2 || !InScannerInt32Range(value)) return false;
    return type->getBitWidth() >= 32
        || (value >= 0 && value < (int64_t(1) << (type->getBitWidth() - 1)));
}
bool LexicalIsUnder(antlr4::tree::ParseTree* node, antlr4::tree::ParseTree* ancestor)
{
    for (; node != nullptr; node = node->parent)
        if (node == ancestor) return true;
    return false;
}
bool LexicalGenericParamNamed(CFlatParser::GenericTypeParametersContext* g, const std::string& name)
{
    if (g == nullptr || g->typeParameterList() == nullptr) return false;
    for (auto* e : g->typeParameterList()->typeParameterEntry())
    {
        if (auto* v = e->valueParameterDeclaration(); v != nullptr && v->Identifier() != nullptr
            && v->Identifier()->getText() == name)
            return true;
        if (e->typeSpecifier() != nullptr && e->typeSpecifier()->getText() == name) return true;
    }
    return false;
}
CFlatParser::InitDeclaratorContext* LexicalVisibleDeclarator(antlr4::tree::ParseTree* owner,
    CFlatParser::InitDeclaratorListContext* list, const std::string& name, antlr4::tree::ParseTree* use,
    bool* self)
{
    if (list == nullptr) return nullptr;
    const bool inside = LexicalIsUnder(use, owner);
    if (inside && !LexicalIsUnder(use, list)) return nullptr;
    CFlatParser::InitDeclaratorContext* match = nullptr;
    for (auto* init : list->initDeclarator())
    {
        const bool named = init->declarator() != nullptr && init->declarator()->directDeclarator() != nullptr
            && getDirectDeclName(init->declarator()->directDeclarator()) == name;
        if (inside && LexicalIsUnder(use, init))
        {
            if (named) *self = true;
            break;
        }
        if (named) match = init;
    }
    return match;
}
bool LexicalDeclaresTypeNamed(CFlatParser::DeclarationContext* decl, const std::string& name)
{
    if (auto* en = decl->enumSpecifier(); en != nullptr && en->Identifier() != nullptr)
        return en->Identifier()->getText() == name;
    std::vector<antlr4::tree::ParseTree*> pending{ decl->declarationSpecifiers() };
    while (!pending.empty())
    {
        auto* tree = pending.back();
        pending.pop_back();
        if (tree == nullptr) continue;
        if (auto* su = dynamic_cast<CFlatParser::StructOrUnionSpecifierContext*>(tree);
            su != nullptr && su->LeftBrace() != nullptr && su->genericIdentifier() != nullptr
            && su->genericIdentifier()->Identifier() != nullptr
            && su->genericIdentifier()->Identifier()->getText() == name)
            return true;
        for (auto* child : tree->children) pending.push_back(child);
    }
    return false;
}
bool LexicalBlockHasPriorEnum(CFlatParser::BlockItemListContext* list,
    CFlatParser::BlockItemContext* item, const std::string& name)
{
    if (list == nullptr || item == nullptr || name.empty()) return false;
    for (auto* prior : list->blockItem())
    {
        if (prior == item) return false;
        auto* decl = prior->declaration();
        auto* en = decl != nullptr ? decl->enumSpecifier() : nullptr;
        if (en != nullptr && en->Identifier() != nullptr && en->Identifier()->getText() == name)
            return true;
    }
    return false;
}
antlr4::tree::TerminalNode* LexicalDirectDeclaratorIdentifier(
    CFlatParser::DirectDeclaratorContext* direct)
{
    if (direct == nullptr) return nullptr;
    if (direct->Identifier() != nullptr) return direct->Identifier();
    auto* nested = direct->declarator();
    return nested != nullptr
        ? LexicalDirectDeclaratorIdentifier(nested->directDeclarator()) : nullptr;
}
LexicalBlockBinding FindLexicalBlockBinding(CFlatParser::BlockItemListContext* list,
    antlr4::tree::ParseTree* use, const std::string& name)
{
    LexicalBlockBinding binding;
    if (list == nullptr) return binding;
    for (auto* item : list->blockItem())
    {
        auto* decl = item->declaration();
        bool self = false;
        auto* visible = decl != nullptr
            ? LexicalVisibleDeclarator(decl, decl->initDeclaratorList(), name, use, &self) : nullptr;
        if (self)
        {
            binding.Kind = LexicalBlockBindingKind::Value;
            binding.Specifiers = nullptr;
            binding.Declarator = nullptr;
            binding.HasVisibleValue = true;
            return binding;
        }
        if (visible != nullptr)
        {
            binding.Kind = LexicalBlockBindingKind::Value;
            binding.Specifiers = decl->declarationSpecifiers();
            binding.Declarator = visible;
            binding.HasVisibleValue = true;
        }
        if (item == use) break;
        if (decl != nullptr && LexicalDeclaresTypeNamed(decl, name))
        {
            binding.Kind = LexicalBlockBindingKind::Type;
            binding.Specifiers = nullptr;
            binding.Declarator = nullptr;
        }
        if (auto* destructure = item->destructuringDeclaration())
            for (auto* entry : destructure->destructuringEntry())
                if (entry->Identifier() != nullptr && entry->Identifier()->getText() == name)
                {
                    binding.Kind = LexicalBlockBindingKind::Value;
                    binding.Specifiers = nullptr;
                    binding.Declarator = nullptr;
                    binding.HasVisibleValue = true;
                }
        if (auto* alias = item->usingDeclaration(); alias != nullptr && alias->Identifier() != nullptr
            && alias->Identifier()->getText() == name)
        {
            binding.Kind = LexicalBlockBindingKind::Type;
            binding.Specifiers = nullptr;
            binding.Declarator = nullptr;
        }
    }
    return binding;
}
bool LexicalFunctionHasParameter(CFlatParser::FunctionDefinitionContext* fn,
    const std::string& name)
{
    auto* types = fn != nullptr ? fn->parameterTypeList() : nullptr;
    auto* parameters = types != nullptr ? types->parameterList() : nullptr;
    if (parameters == nullptr) return false;
    for (auto* param : parameters->parameterDeclaration())
        if (param->declarator() != nullptr && param->declarator()->directDeclarator() != nullptr
            && getDirectDeclName(param->declarator()->directDeclarator()) == name)
            return true;
    return false;
}
bool LexicalLambdaHasParameter(CFlatParser::LambdaExpressionContext* lambda,
    const std::string& name)
{
    if (lambda == nullptr || lambda->lambdaParamList() == nullptr) return false;
    for (auto* param : lambda->lambdaParamList()->lambdaParam())
        if (param->Identifier() != nullptr && param->Identifier()->getText() == name)
            return true;
    return false;
}
bool LexicalBodyHasValueParameter(CFlatParser::BlockItemListContext* list,
    const std::string& name)
{
    auto* compound = list != nullptr
        ? dynamic_cast<CFlatParser::CompoundStatementContext*>(list->parent) : nullptr;
    if (compound == nullptr) return false;
    if (auto* fn = dynamic_cast<CFlatParser::FunctionDefinitionContext*>(compound->parent))
        return LexicalFunctionHasParameter(fn, name);
    auto* lambdaBody = dynamic_cast<CFlatParser::LambdaBodyContext*>(compound->parent);
    return LexicalLambdaHasParameter(
        lambdaBody != nullptr
            ? dynamic_cast<CFlatParser::LambdaExpressionContext*>(lambdaBody->parent) : nullptr,
        name);
}
bool LexicalLocalTypeShadowsValue(LLVMBackend* compiler, antlr4::tree::ParseTree* use,
    const std::string& name)
{
    if (compiler == nullptr || !compiler->HasLexicalLocalTypeName(name)) return false;
    bool shadows = false;
    WalkLexicalBindings(use, name, [&](const LexicalBindingEvent& event) {
        switch (event.Kind)
        {
        case LexicalBindingEventKind::Block:
            if (event.BlockBinding.Kind == LexicalBlockBindingKind::Type)
            {
                shadows = !event.BlockBinding.HasVisibleValue && !event.HasSameScopeParameter;
                return true;
            }
            if (event.BlockBinding.Kind == LexicalBlockBindingKind::Value)
            {
                shadows = false;
                return true;
            }
            return false;
        case LexicalBindingEventKind::FunctionParameter:
        case LexicalBindingEventKind::LambdaParameter:
        case LexicalBindingEventKind::CaseBinding:
        case LexicalBindingEventKind::RangeBinding:
        case LexicalBindingEventKind::ForDeclaration:
            shadows = false;
            return true;
        case LexicalBindingEventKind::GenericParameter:
            shadows = false;
            return true;
        }
        return false;
    });
    return shadows;
}
bool LexicalHasFunctionScope(antlr4::tree::ParseTree* node)
{
    for (; node != nullptr; node = node->parent)
        if (dynamic_cast<CFlatParser::FunctionDefinitionContext*>(node) != nullptr
            || dynamic_cast<CFlatParser::LambdaExpressionContext*>(node) != nullptr
            || dynamic_cast<CFlatParser::DestructorDefinitionContext*>(node) != nullptr)
            return true;
    return false;
}
void RegisterLexicalLocalTypeName(LLVMBackend* compiler,
    antlr4::tree::ParseTree* declaration, const std::string& name)
{
    if (compiler != nullptr && !name.empty() && LexicalHasFunctionScope(declaration))
        compiler->RegisterLexicalLocalTypeName(name);
}
void RegisterLexicalStructNamesInDeclaration(LLVMBackend* compiler,
    CFlatParser::DeclarationContext* declaration)
{
    if (compiler == nullptr || declaration == nullptr || !LexicalHasFunctionScope(declaration)) return;
    std::vector<antlr4::tree::ParseTree*> pending;
    if (declaration->declarationSpecifiers() != nullptr)
        pending.push_back(declaration->declarationSpecifiers());
    while (!pending.empty())
    {
        auto* tree = pending.back();
        pending.pop_back();
        if (auto* su = dynamic_cast<CFlatParser::StructOrUnionSpecifierContext*>(tree);
            su != nullptr && su->LeftBrace() != nullptr && su->genericIdentifier() != nullptr
            && su->genericIdentifier()->Identifier() != nullptr)
            compiler->RegisterLexicalLocalTypeName(su->genericIdentifier()->Identifier()->getText());
        for (auto* child : tree->children) pending.push_back(child);
    }
}
std::optional<int64_t> FoldLexicalConstLocal(LLVMBackend* compiler, antlr4::tree::ParseTree* use,
    const std::string& name, const LexicalConstTypeParser& parseType, bool* found)
{
    if (found != nullptr) *found = false;
    auto shadow = [&]() -> std::optional<int64_t> {
        if (found != nullptr) *found = true;
        return std::nullopt;
    };
    auto fold = [&](CFlatParser::DeclarationSpecifiersContext* specs,
                    CFlatParser::InitDeclaratorContext* match) -> std::optional<int64_t> {
        if (found != nullptr) *found = true;
        auto* initializer = match->initializer();
        if (specs == nullptr || initializer == nullptr || initializer->assignmentExpression() == nullptr)
            return std::nullopt;
        auto type = parseType(specs);
        if (type.Pointer || type.ConstArraySize != 0 || !compiler->IsNativeConstObject(type))
            return std::nullopt;
        auto* intTy = llvm::dyn_cast_or_null<llvm::IntegerType>(compiler->GetType(type));
        LexicalConstParserScope parserScope(&parseType);
        auto folded = FoldCompileTimeInt(compiler, initializer->assignmentExpression());
        if (!folded || !ConstLocalFoldFits(intTy, *folded)) return std::nullopt;
        return folded;
    };
    std::optional<int64_t> result;
    WalkLexicalBindings(use, name, [&](const LexicalBindingEvent& event) {
        switch (event.Kind)
        {
        case LexicalBindingEventKind::Block:
            if (event.BlockBinding.Kind == LexicalBlockBindingKind::Type)
            {
                result = shadow();
                return true;
            }
            if (event.BlockBinding.Kind == LexicalBlockBindingKind::Value)
            {
                result = event.BlockBinding.Declarator != nullptr
                    ? fold(event.BlockBinding.Specifiers, event.BlockBinding.Declarator)
                    : shadow();
                return true;
            }
            return false;
        case LexicalBindingEventKind::ForDeclaration:
            result = event.Self ? shadow() : fold(event.Specifiers, event.Declarator);
            return true;
        case LexicalBindingEventKind::FunctionParameter:
        case LexicalBindingEventKind::GenericParameter:
        case LexicalBindingEventKind::LambdaParameter:
        case LexicalBindingEventKind::CaseBinding:
        case LexicalBindingEventKind::RangeBinding:
            result = shadow();
            return true;
        }
        return false;
    });
    return result;
}
std::optional<int64_t> FoldCompileTimeIdentifier(LLVMBackend* compiler,
                                                        CFlatParser::GenericIdentifierContext* gid)
{
    if (compiler == nullptr || gid == nullptr) return std::nullopt;
    if (gid->genericTypeParameters() != nullptr || gid->Identifier() == nullptr) return std::nullopt;
    const std::string name = gid->Identifier()->getText();
    if (auto local = compiler->GetScopedLocalOrArgument(name); local.Storage != nullptr)
        return local.ConstFoldInt;
    // Inside a lexical const initializer a name resolves in that declaration's own scope first.
    if (activeLexicalConstTypeParser != nullptr)
    {
        bool lexicalBinding = false;
        auto lexical = FoldLexicalConstLocal(compiler, gid, name, *activeLexicalConstTypeParser,
                                             &lexicalBinding);
        if (lexicalBinding) return lexical;
    }
    // An implicit-this field shadows a global constant of the same name.
    if (compiler->FindImplicitThisField(name, nullptr, nullptr) != nullptr) return std::nullopt;
    auto macro = compiler->GetCompileTimeMacro(name);
    if (auto* ci = llvm::dyn_cast_or_null<llvm::ConstantInt>(macro.value); ci != nullptr && macro.type == "int")
    {
        int64_t mv = (int64_t)ci->getSExtValue();
        return InScannerInt32Range(mv) ? std::optional<int64_t>(mv) : std::nullopt;
    }
    // Innermost enclosing namespace first (C++ lookup): a seeded "N.name" const folds to its own value,
    // while a non-const global or function of that qualified name hides the file-scope const.
    for (std::string scope = compiler->GetCurrentNamespace(); !scope.empty();)
    {
        const std::string key = scope + "." + name;
        int64_t nsValue = 0;
        if (compiler->TryGetConstGlobalInt(key, nsValue))
            return InScannerInt32Range(nsValue) ? std::optional<int64_t>(nsValue) : std::nullopt;
        if (compiler->GetGlobalVariableNV(key).Storage != nullptr || compiler->GetFunction(key))
            return std::nullopt;
        const size_t dot = scope.rfind('.');
        scope = dot == std::string::npos ? std::string{} : scope.substr(0, dot);
    }
    int64_t cv = 0;
    if (!compiler->TryGetConstGlobalInt(name, cv))
    {
        // Inside an array dimension only, a C macro or an unscoped imported enumerator (`enum { HE = 2 }`)
        // folds like a constant, so `(DIMQ > 2) && (HE == 2) ? 2 : 1` is a constant size. Elsewhere it
        // stays undecided, which keeps C++ null-pointer-constant classification unchanged.
        if (!compiler->IsFoldingArrayDim()) return std::nullopt;
        // A CFlat global this name resolves to from the current or an enclosing namespace (`N.Q` in
        // `namespace N`) is the binding the macro would shadow; the emission path folds a const one.
        if (compiler->ResolveQualifiedName(name) != name) return std::nullopt;
        auto global = compiler->GetGlobalVariableNV(name);
        auto* gv = llvm::dyn_cast_or_null<llvm::GlobalVariable>(global.Storage);
        auto* ci = gv != nullptr && gv->hasInitializer()
            ? llvm::dyn_cast<llvm::ConstantInt>(gv->getInitializer()) : nullptr;
        if (ci == nullptr || ci->getBitWidth() > 64)
            return std::nullopt;
        const std::string gvName(gv->getName());
        if (!compiler->IsCMacroGlobal(gvName) && compiler->ImportedEnumeratorGlobals().count(gvName) == 0)
            return std::nullopt;
        // An unsigned macro keeps its zero-extended value (0xFFFFFFFFu is 4294967295, not -1).
        // Returned unclamped so TryFoldArrayDim reports an oversized value as "exceeds the maximum".
        if (global.TypeAndValue.IsUnsignedInteger() != -1)
        {
            const uint64_t z = ci->getZExtValue();
            return z > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)z;
        }
        return ci->getSExtValue();
    }
    return InScannerInt32Range(cv) ? std::optional<int64_t>(cv) : std::nullopt;
}
bool InScannerInt32Range(int64_t v) {
        return v >= (int64_t)INT32_MIN && v <= (int64_t)INT32_MAX;
    }
bool ScannerAddOverflow(int64_t a, int64_t b, int64_t* out) {
        *out = (int64_t)((uint64_t)a + (uint64_t)b);
        return ((a ^ *out) & (b ^ *out)) < 0;
    }
bool ScannerSubOverflow(int64_t a, int64_t b, int64_t* out) {
        *out = (int64_t)((uint64_t)a - (uint64_t)b);
        return ((a ^ b) & (a ^ *out)) < 0;
    }
bool ScannerMulOverflow(int64_t a, int64_t b, int64_t* out) {
        *out = (int64_t)((uint64_t)a * (uint64_t)b);
        if (a == 0) return false;
        if (a == -1 && b == INT64_MIN) return true;
        return *out / a != b;
    }
std::optional<int64_t> ParseScannerIntegerLiteral(const std::string& textIn) {
        if (textIn == "true") return (int64_t)1;
        if (textIn == "false") return (int64_t)0;
        std::string text = textIn;
        if (!text.empty() && (text.back() == 'u' || text.back() == 'U'
                           || text.back() == 'l' || text.back() == 'L'))
            return std::nullopt;   // suffixed literal - codegen's type differs from ours
        bool neg = false;
        size_t pos = 0;
        if (!text.empty() && text[0] == '-') { neg = true; pos = 1; }
        if (pos >= text.size()) return std::nullopt;
        int base = 10;
        if (text.size() > pos + 1 && text[pos] == '0')
        {
            char c = text[pos + 1];
            if (c == 'x' || c == 'X')      { base = 16; pos += 2; }
            else if (c == 'b' || c == 'B') { base = 2;  pos += 2; }
            else return std::nullopt;      // C octal (leading 0) - not folded here
        }
        if (pos >= text.size()) return std::nullopt;
        uint64_t v = 0;
        auto res = std::from_chars(text.data() + pos, text.data() + text.size(), v, base);
        if (res.ec != std::errc() || res.ptr != text.data() + text.size()) return std::nullopt;
        if (v > (uint64_t)INT32_MAX) return std::nullopt;
        int64_t out = neg ? -(int64_t)v : (int64_t)v;
        return InScannerInt32Range(out) ? std::optional<int64_t>(out) : std::nullopt;
    }
std::string getDirectDeclName(CFlatParser::DirectDeclaratorContext* d)
{
    if (!d) return "";
    // C-style array form: (Identifier | Move) '[' assignmentExpression ']'
    if (d->assignmentExpression())
        return d->Identifier() ? d->Identifier()->getText() : "move";
    return d->getText();
}
bool HasParameterArrayDeclarator(CFlatParser::DeclarationSpecifiersContext* specs)
{
    auto* param = specs != nullptr
        ? dynamic_cast<CFlatParser::ParameterDeclarationContext*>(specs->parent) : nullptr;
    if (param == nullptr || param->declarator() == nullptr) return false;
    auto* direct = param->declarator()->directDeclarator();
    return direct != nullptr && direct->assignmentExpression() != nullptr;
}
bool TypeArgHasUnique(CFlatParser::TypeParameterEntryContext* entry)
{
    return entry != nullptr && entry->Identifier() != nullptr
        && entry->Identifier()->getText() == "unique";
}
std::string LegacyUniqueSubstitutionSpelling(const LLVMBackend* compiler,
                                                    const std::string& resolved)
{
    if (compiler == nullptr || !compiler->IsCoreUniqueType(resolved)) return resolved;
    std::string pointee = MangledGenericArgument(*compiler, resolved);
    if (compiler->IsInterfaceType(pointee))
        return pointee;
    return pointee + "*";
}
uint64_t CoreUniqueDeclaredAllocAlign(const LLVMBackend* compiler,
                                             const std::string& typeName)
{
    if (compiler == nullptr || !compiler->IsCoreUniqueType(typeName)) return 0;
    std::string alignArg = MangledGenericArgument(*compiler, typeName, 1);
    if (!alignArg.empty() && alignArg.front() == '.') alignArg.erase(0, 1);
    if (alignArg.empty()) return 0;
    for (char c : alignArg)
        if (c < '0' || c > '9') return 0;
    return std::strtoull(alignArg.c_str(), nullptr, 10);
}
void ShapeCoreUniqueCtorArg(const LLVMBackend* compiler,
                                   LLVMBackend::NamedVariable& ctorArg,
                                   const std::string& uniqueTypeName, llvm::Value* value)
{
    const std::string pointee = MangledGenericArgument(*compiler, uniqueTypeName);
    if (compiler != nullptr && compiler->IsInterfaceType(pointee))
    {
        bool fat = value != nullptr && value->getType()->isStructTy();
        std::string argType = fat ? pointee : ctorArg.TypeAndValue.TypeName;
        if (argType.empty()) argType = pointee;
        ctorArg.TypeAndValue.TypeName = argType;
        ctorArg.TypeAndValue.Pointer = !fat;
        ctorArg.TypeAndValue.IsInterface = compiler->IsInterfaceType(argType);
        ctorArg.TypeAndValue.IsInterfacePointer = false;
    }
    else
    {
        ctorArg.TypeAndValue.TypeName = pointee;
        ctorArg.TypeAndValue.Pointer = true;
    }
    ctorArg.TypeAndValue.ElemPointer = false;
    ctorArg.TypeAndValue.PointerDepth = 0;
}
void CanonicalizeUniqueSigComponent(LLVMBackend* compiler, std::string& typeName,
                                           bool& pointer, int& pointerDepth,
                                           std::string* cxxUniqueError)
{
    if (compiler == nullptr || pointer || !compiler->IsCoreUniqueType(typeName)) return;
    std::string pointee = MangledGenericArgument(*compiler, typeName);
    if (compiler->IsInterfaceType(pointee)) return;
    if (compiler->IsCxxUniquePtrPointee(pointee))
    {
        std::string localError;
        std::string& error = cxxUniqueError != nullptr ? *cxxUniqueError : localError;
        std::string cxxUniqueType = compiler->CxxUniquePtrForUniqueKeyword(pointee, error);
        if (!cxxUniqueType.empty())
        {
            typeName = std::move(cxxUniqueType);
            return;
        }
    }
    typeName = pointee;
    pointer = true;
    pointerDepth = 1;
}
bool CanDesugarUniqueTypeArg(const LLVMBackend* compiler, const std::string& base,
                                    bool hasPointer, int pointerDepth, bool hasArrayView,
                                    bool baseIsGenericInstantiation)
{
    if (compiler == nullptr || hasArrayView || base.empty() || base == "void") return false;
    if (base.find('*') != std::string::npos || base.find('[') != std::string::npos) return false;
    // The interface answer must be the SAME in both passes: interfaceTable is still empty during
    // the forward scan, so the scan's own interface-definition set stands in for it there.
    if (compiler->IsInterfaceTypeArgKey(base)) return !hasPointer;
    if (!hasPointer || pointerDepth != 1) return false;
    // IsTypeArgTypeKey also answers for a name the FORWARD SCAN has only seen, so the scanner's
    // opaque shell is named the same as the instantiation the codegen pass builds. An unbound
    // template parameter is in neither set and keeps the qualifier prefix.
    // baseIsGenericInstantiation: the caller MANGLED this base itself from a nested generic spec,
    // so no registry holds it yet - the other pass registers it before it asks. Without it the two
    // passes disagreed on `unique dictionary<string, double>*`.
    return compiler->IsKnownTypeName(base) || compiler->IsTypeArgTypeKey(base)
        || baseIsGenericInstantiation;
}
bool NamesEnclosingTypeParameter(antlr4::tree::ParseTree* node, const std::string& name)
{
    for (auto* cur = node; cur != nullptr; cur = cur->parent)
    {
        CFlatParser::GenericTypeParametersContext* generic = nullptr;
        if (auto* fn = dynamic_cast<CFlatParser::FunctionDefinitionContext*>(cur))
            generic = fn->genericTypeParameters();
        else if (auto* sd = dynamic_cast<CFlatParser::StructDefinitionContext*>(cur))
            generic = sd->genericTypeParameters();
        else if (auto* cd = dynamic_cast<CFlatParser::ClassDefinitionContext*>(cur))
            generic = cd->genericTypeParameters();
        if (generic == nullptr || generic->typeParameterList() == nullptr) continue;
        for (auto* entry : generic->typeParameterList()->typeParameterEntry())
        {
            if (entry->valueParameterDeclaration() != nullptr || entry->typeSpecifier() == nullptr)
                continue;
            auto* gid = entry->typeSpecifier()->genericIdentifier();
            if (gid != nullptr && gid->Identifier() != nullptr
                && gid->genericTypeParameters() == nullptr
                && gid->Identifier()->getText() == name)
                return true;
        }
    }
    return false;
}
bool UniqueDeclPointeeIsUnbound(const LLVMBackend* compiler, antlr4::tree::ParseTree* node,
                                       const std::string& name)
{
    if (compiler == nullptr || name.empty()) return false;
    if (compiler->IsKnownTypeName(name) || compiler->IsTypeArgTypeKey(name)) return false;
    return NamesEnclosingTypeParameter(node, name);
}
bool TypeArgHasAlias(CFlatParser::TypeParameterEntryContext* entry)
{
    return entry != nullptr && entry->Identifier() != nullptr
        && entry->Identifier()->getText() == "alias";
}
bool IsValueParameterDeclaration(CFlatParser::TypeParameterEntryContext* entry)
{
    return entry != nullptr && entry->valueParameterDeclaration() != nullptr;
}
std::string ValueParameterTypeSpelling(CFlatParser::TypeParameterEntryContext* entry)
{
    auto* decl = entry != nullptr ? entry->valueParameterDeclaration() : nullptr;
    return decl != nullptr && decl->valueParameterType() != nullptr
        ? decl->valueParameterType()->getText() : std::string{};
}
bool IsDecimalIntegerSpelling(const std::string& text)
{
    if (text.empty()) return false;
    size_t start = text[0] == '-' ? 1 : 0;
    if (start == text.size()) return false;
    for (size_t i = start; i < text.size(); i++)
        if (text[i] < '0' || text[i] > '9') return false;
    return true;
}
int64_t ParseValueArg(const std::string& text)
{
    int64_t out = 0;
    size_t start = (!text.empty() && text[0] == '-') ? 1 : 0;
    auto res = std::from_chars(text.data() + start, text.data() + text.size(), out);
    if (res.ec != std::errc() || res.ptr != text.data() + text.size()) return 0;
    return start == 1 ? -out : out;
}
CFlatParser::NewExpressionContext* RawNewArrayValueOf(antlr4::tree::ParseTree* node)
{
    if (node == nullptr) return nullptr;
    if (auto* newExpr = dynamic_cast<CFlatParser::NewExpressionContext*>(node))
        return newExpr->assignmentExpression() != nullptr ? newExpr : nullptr;
    if (auto* conditional = dynamic_cast<CFlatParser::ConditionalExpressionContext*>(node))
    {
        if (conditional->Question() != nullptr)
        {
            if (auto* v = RawNewArrayValueOf(conditional->expression())) return v;
            return RawNewArrayValueOf(conditional->conditionalExpression());
        }
        if (conditional->QuestionQuestion() != nullptr)
        {
            if (auto* v = RawNewArrayValueOf(conditional->logicalOrExpression())) return v;
            return RawNewArrayValueOf(conditional->conditionalExpression());
        }
        return RawNewArrayValueOf(conditional->logicalOrExpression());
    }
    if (auto* assignment = dynamic_cast<CFlatParser::AssignmentExpressionContext*>(node))
        return RawNewArrayValueOf(assignment->conditionalExpression());
    if (auto* ctx = dynamic_cast<antlr4::ParserRuleContext*>(node))
    {
        // A single-child pass-through link in the expression chain (or a parenthesized
        // expression) carries the same value; anything wider is not the value side.
        if (ctx->children.size() == 1)
            return RawNewArrayValueOf(ctx->children[0]);
        if (auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(ctx);
            primary != nullptr && primary->expression() != nullptr)
            return RawNewArrayValueOf(primary->expression());
    }
    return nullptr;
}
std::string MainListener::NewExpressionElementTypeName(CFlatParser::NewExpressionContext* ctx,
    uint32_t* aliasConstMask)
{
    if (ctx == nullptr) return {};
    std::string typeName;
    uint32_t mask = 0;
    if (auto* typeSpec = ctx->typeSpecifier(); typeSpec != nullptr && typeSpec->typeName() != nullptr)
    {
        // Inline `(const T)` / `const(T)`: the same whole-word resolution as a cast or declaration.
        auto nested = ParseTypeName(typeSpec->typeName(), false, ctx);
        nested.ApplyWholeWordConst();
        typeName = nested.TypeName + std::string(nested.ValuePointerDepth(), '*');
        mask = nested.ConstMask;
    }
    else
    {
        typeName = ParseTypeSpecifierName(ctx->typeSpecifier(), ctx->multiWordTypeSuffix());
        mask = Compiler(ctx)->ResolveTypeAliasConstMask(typeName)
            | LLVMBackend::TypeAndValue::ConstBitsBelow(
                SubstitutedConstDepth(ctx->typeSpecifier(), ctx->multiWordTypeSuffix()));
        typeName = Compiler(ctx)->ResolveTypeAlias(typeName);
    }
    if (aliasConstMask != nullptr) *aliasConstMask = mask;
    if (ctx->pointer() != nullptr)
        typeName.append(ctx->pointer()->Star().size(), '*');
    if (aliasConstMask == nullptr)
        return JoinConstTypeArgument(typeName, ContiguousLowConstLevels(mask));
    return typeName;
}
bool IsChainedAssignmentShape(antlr4::tree::ParseTree* node)
{
    if (node == nullptr) return false;
    if (auto* assign = dynamic_cast<CFlatParser::AssignmentExpressionContext*>(node);
        assign != nullptr && assign->assignmentOperator() != nullptr)
        return true;
    if (auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node);
        primary != nullptr)
        return primary->expression() != nullptr && primary->children.size() == 3
            && IsChainedAssignmentShape(primary->expression());
    if (auto* ctxRule = dynamic_cast<antlr4::ParserRuleContext*>(node);
        ctxRule != nullptr && ctxRule->children.size() == 1)
        return IsChainedAssignmentShape(ctxRule->children[0]);
    return false;
}
bool FillGenericValueDefaults(const LLVMBackend& compiler, const std::string& templateName,
                                     size_t paramCount, std::vector<std::string>& typeArgs)
{
    const auto* defaults = GenericValueDefaultsFor(compiler, templateName);
    if (defaults == nullptr) return typeArgs.size() >= paramCount;
    while (typeArgs.size() < paramCount && typeArgs.size() < defaults->size()
           && !(*defaults)[typeArgs.size()].empty())
        typeArgs.push_back((*defaults)[typeArgs.size()]);
    return typeArgs.size() >= paramCount;
}
bool TemplateHasGenericDefaults(const LLVMBackend& compiler, const std::string& templateName)
{
    const auto* defaults = GenericValueDefaultsFor(compiler, templateName);
    if (defaults == nullptr) return false;
    for (const auto& d : *defaults)
        if (!d.empty()) return true;
    return false;
}
bool StripAliasQualifier(std::string& name)
{
    if (name.compare(0, kAliasQualifierPrefixLen, kAliasQualifierPrefix) == 0)
    {
        name.erase(0, kAliasQualifierPrefixLen);
        return true;
    }
    return false;
}
void StripOwnershipQualifiers(std::string& name)
{
    StripAliasQualifier(name);
}
std::string MangleTypeArg(const LLVMBackend* compiler, const std::string& typeName)
{
    if (compiler == nullptr) return typeName;
    LLVMBackend::TypeAndValue type;
    type.TypeName = typeName;
    return MangleType(*compiler, type);
}
std::string SpellFunctionParameterValueType(
    const LLVMBackend& compiler, const LLVMBackend::TypeAndValue& type)
{
    std::string spelling = SpellType(compiler, type);
    const int depth = type.ValuePointerDepth();
    const uint32_t pointeeConst = static_cast<uint32_t>(type.PointeeConstMask());
    int constDepth = 0;
    while (constDepth < depth && (pointeeConst & (1u << constDepth)) != 0)
        ++constDepth;
    return JoinConstTypeArgument(spelling, constDepth);
}
uint32_t UsingWholeWordConstMask(LLVMBackend* compiler, const std::string& target, size_t suffixStars)
{
    const std::string resolved = compiler->ResolveTypeAlias(target);
    size_t depth = suffixStars;
    for (auto it = resolved.rbegin(); it != resolved.rend() && *it == '*'; ++it) ++depth;
    return LLVMBackend::TypeAndValue::ConstBitsThrough(static_cast<int>(depth));
}
void RegisterPureRenameAlias(LLVMBackend* compiler, CFlatParser::UsingDeclarationContext* ctx)
{
    if (compiler == nullptr || ctx == nullptr) return;
    if (ctx->Identifier() == nullptr) return;  // `using "name" = T;` keys on a string literal
    // A pointer/array suffix means the alias is not a pure rename.
    if (ctx->pointer() != nullptr || ctx->arrayTypeSuffix() != nullptr) return;
    auto* typeSpec = ctx->typeSpecifier();
    if (typeSpec == nullptr) return;
    PrimitiveTypeError targetError;
    std::string targetSpelling = CanonicalTypeSpecifierText(
        typeSpec, ctx->multiWordTypeSuffix(), false, &targetError);
    if (HasPrimitiveTypeError(targetError)) return;
    std::string target = compiler->ResolveQualifiedName(targetSpelling);
    if (IsUnparameterizedNamedTypeSpecifier(typeSpec))
    {
        const std::string contextual = ResolveContextualTypeName(compiler,
            typeSpec->getText(), target, typeSpec,
            EnclosingAggregateNamesFromContext(compiler, typeSpec),
            NamespaceNameFromContext(compiler, typeSpec));
        if (!contextual.empty()) target = contextual;
    }
    std::string alias = ctx->Identifier()->getText();
    if (alias == target || !IsBareTypeName(target)) return;
    compiler->RegisterManglingAlias(alias, target);
}
std::string ResolveTypeArgSpelling(const LLVMBackend* compiler, const std::string& arg)
{
    if (compiler == nullptr || arg.empty()) return arg;
    bool isView = arg.ends_with("[]");
    size_t end = isView ? arg.size() - 2 : arg.size();
    int stars = 0;
    while (end > 0 && arg[end - 1] == '*') { end--; stars++; }
    if (end == 0) return arg;
    std::string core = arg.substr(0, end);
    std::string resolved = compiler->ResolveTypeArgBaseName(core);
    if (FoldPointerAliasArg(compiler, resolved, stars))
        return resolved + std::string(stars, '*') + (isView ? "[]" : "");
    if (resolved == core) return arg;
    return resolved + arg.substr(end);
}
CFlatParser::GenericTypeParametersContext* GenericSpecOf(
    CFlatParser::TypeSpecifierContext* ts, std::string& base)
{
    base.clear();
    if (ts == nullptr) return nullptr;
    if (auto* q = ts->qualifiedGenericIdentifier())
    {
        // The lexer token for a soft-keyword component such as `std.function` is Function,
        // not Identifier, so reconstruct the qualified base from the text rather than dropping
        // that component from the C++ template identity.
        std::string text = q->getText();
        size_t lt = text.find('<');
        base = text.substr(0, lt == std::string::npos ? text.size() : lt);
        return q->genericTypeParameters();
    }
    if (auto* g = ts->genericIdentifier())
    {
        if (g->Identifier() == nullptr) return nullptr;
        base = g->Identifier()->getText();
        return g->genericTypeParameters();
    }
    return nullptr;
}
std::string NamespaceNameFromContext(
    LLVMBackend* compiler, antlr4::tree::ParseTree* node)
{
    std::vector<CFlatParser::NamespaceDefinitionContext*> namespaces;
    for (auto* parent = node; parent != nullptr; parent = parent->parent)
        if (auto* ns = dynamic_cast<CFlatParser::NamespaceDefinitionContext*>(parent))
            namespaces.push_back(ns);
    std::string name;
    for (auto it = namespaces.rbegin(); it != namespaces.rend(); ++it)
        for (auto* id : (*it)->Identifier())
            name += (name.empty() ? "" : ".") + id->getText();
    return name.empty() && compiler != nullptr ? compiler->GetCurrentNamespace() : name;
}
bool IsUnparameterizedNamedTypeSpecifier(CFlatParser::TypeSpecifierContext* typeSpec)
{
    if (typeSpec == nullptr) return false;
    if (typeSpec->rootQualifiedTypeName() != nullptr) return true;
    if (auto* qualified = typeSpec->qualifiedGenericIdentifier(); qualified != nullptr)
        return qualified->genericTypeParameters() == nullptr;
    if (auto* generic = typeSpec->genericIdentifier(); generic != nullptr)
        return generic->genericTypeParameters() == nullptr;
    return false;
}
std::vector<std::string> NamedTypePathFromContext(CFlatParser::TypeSpecifierContext* typeSpec)
{
    std::vector<std::string> path;
    if (!IsUnparameterizedNamedTypeSpecifier(typeSpec)) return path;
    if (auto* qualified = typeSpec->qualifiedGenericIdentifier())
    {
        path.push_back(qualified->Identifier()->getText());
        for (auto* member : qualified->memberNameToken()) path.push_back(member->getText());
    }
    else if (auto* generic = typeSpec->genericIdentifier())
        path.push_back(generic->Identifier()->getText());
    else if (typeSpec->rootQualifiedTypeName() != nullptr)
    {
        std::string name = RootQualifiedTypeSpelling(typeSpec);
        size_t start = 0;
        while (start <= name.size())
        {
            const size_t dot = name.find('.', start);
            path.push_back(name.substr(start, dot == std::string::npos ? dot : dot - start));
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
    }
    return path;
}
namespace
{
std::vector<antlr4::ParserRuleContext*> EnclosingAggregateDefinitionsFromContext(
    antlr4::tree::ParseTree* node)
{
    std::vector<antlr4::ParserRuleContext*> definitions;
    for (auto* parent = node != nullptr ? node->parent : nullptr;
         parent != nullptr; parent = parent->parent)
    {
        if (dynamic_cast<CFlatParser::NestedAggregateFieldContext*>(parent) != nullptr
            || dynamic_cast<CFlatParser::StructDefinitionContext*>(parent) != nullptr
            || dynamic_cast<CFlatParser::ClassDefinitionContext*>(parent) != nullptr
            || dynamic_cast<CFlatParser::ProgramDefinitionContext*>(parent) != nullptr)
            definitions.push_back(dynamic_cast<antlr4::ParserRuleContext*>(parent));
    }
    return definitions;
}

std::vector<CFlatParser::AggregateMemberContext*> AggregateMembersFromDefinition(
    antlr4::ParserRuleContext* definition)
{
    if (auto* nested = dynamic_cast<CFlatParser::NestedAggregateFieldContext*>(definition))
        return nested->aggregateMember();
    if (auto* structure = dynamic_cast<CFlatParser::StructDefinitionContext*>(definition))
        return structure->aggregateMember();
    if (auto* classDefinition = dynamic_cast<CFlatParser::ClassDefinitionContext*>(definition))
        return classDefinition->aggregateMember();
    return {};
}
}

bool HasNestedAggregatePathInContext(antlr4::tree::ParseTree* useSite,
    size_t aggregateIndex, const std::vector<std::string>& path)
{
    if (path.empty()) return false;
    const auto definitions = EnclosingAggregateDefinitionsFromContext(useSite);
    if (aggregateIndex >= definitions.size()) return false;
    auto* owner = definitions[aggregateIndex];
    auto hasPath = [&](auto&& self, antlr4::tree::ParseTree* aggregate, size_t part) -> bool
    {
        auto* definition = dynamic_cast<antlr4::ParserRuleContext*>(aggregate);
        for (auto* member : AggregateMembersFromDefinition(definition))
        {
            if (part + 1 == path.size())
                if (auto* alias = member->usingDeclaration(); alias != nullptr
                    && alias->Identifier() != nullptr
                    && alias->Identifier()->getText() == path[part])
                    return true;
            antlr4::tree::ParseTree* nested = member->nestedAggregateField();
            if (nested == nullptr) nested = member->structDefinition();
            if (nested == nullptr) nested = member->classDefinition();
            if (nested == nullptr) continue;
            auto* nestedDefinition = dynamic_cast<antlr4::ParserRuleContext*>(nested);
            const std::string nestedName = AggregateDefinitionName(nestedDefinition);
            if (nestedName != path[part]) continue;
            if (part + 1 == path.size() || self(self, nested, part + 1)) return true;
        }
        return false;
    };
    return hasPath(hasPath, owner, 0);
}
std::vector<EnclosingAggregateLookupScope> EnclosingAggregateScopesFromContext(
    LLVMBackend* compiler, antlr4::tree::ParseTree* node)
{
    if (compiler == nullptr) return {};
    auto scopes = EnclosingAggregateTypeScopesFromContext(compiler, node);
    if (!compiler->GetCurrentNamespace().empty())
        scopes.push_back({ compiler->GetCurrentNamespace(), nullptr });
    return scopes;
}
std::vector<EnclosingAggregateLookupScope> EnclosingAggregateTypeScopesFromContext(
    LLVMBackend* compiler, antlr4::tree::ParseTree* node)
{
    if (compiler == nullptr) return {};
    const auto definitions = EnclosingAggregateDefinitionsFromContext(node);
    std::string scope = NamespaceNameFromContext(compiler, node);
    std::vector<EnclosingAggregateLookupScope> scopes;
    for (auto it = definitions.rbegin(); it != definitions.rend(); ++it)
    {
        const std::string name = AggregateDefinitionName(*it);
        if (name.empty()) continue;
        scope += (scope.empty() ? "" : ".") + name;
        scopes.push_back({ scope, *it });
    }
    std::reverse(scopes.begin(), scopes.end());
    return scopes;
}
std::vector<std::string> EnclosingAggregateNamesFromContext(
    LLVMBackend* compiler, antlr4::tree::ParseTree* node)
{
    if (compiler == nullptr) return {};
    const auto scopes = EnclosingAggregateTypeScopesFromContext(compiler, node);
    std::vector<std::string> aggregateNames;
    aggregateNames.reserve(scopes.size());
    for (const auto& scope : scopes) aggregateNames.push_back(scope.Name);
    return aggregateNames;
}
std::string AggregateDefinitionName(antlr4::ParserRuleContext* definition)
{
    if (auto* nested = dynamic_cast<CFlatParser::NestedAggregateFieldContext*>(definition))
    {
        const auto names = nested->directDeclarator();
        if (!names.empty()) return names[0]->getText();
    }
    if (auto* structure = dynamic_cast<CFlatParser::StructDefinitionContext*>(definition))
        return structure->directDeclarator()->getText();
    if (auto* classDefinition = dynamic_cast<CFlatParser::ClassDefinitionContext*>(definition))
        return classDefinition->directDeclarator()->getText();
    if (auto* program = dynamic_cast<CFlatParser::ProgramDefinitionContext*>(definition))
        return program->directDeclarator()->getText();
    return {};
}
std::string ResolveContextualTypeName(LLVMBackend* compiler,
    const std::string& spelled, const std::string& resolvedName,
    CFlatParser::TypeSpecifierContext* typeSpec,
    const std::vector<std::string>& aggregateScopes, const std::string& namespaceName)
{
    if (compiler == nullptr || spelled.empty()) return {};
    auto isKnownType = [compiler](const std::string& candidate)
    {
        return compiler->IsKnownTypeName(candidate) || compiler->IsTypeArgTypeKey(candidate);
    };
    const auto typePath = NamedTypePathFromContext(typeSpec);
    const auto contextScopes = EnclosingAggregateTypeScopesFromContext(compiler, typeSpec);
    for (size_t i = 0; i < contextScopes.size(); i++)
    {
        if (i >= aggregateScopes.size()) break;
        const std::string& candidateScope = aggregateScopes[i];
        if (!candidateScope.empty())
        {
            const bool declaredInAggregate =
                HasNestedAggregatePathInContext(typeSpec, i, typePath);
            if (AggregateDefinitionName(contextScopes[i].Definition) == spelled)
                return candidateScope;
            const std::string candidate = candidateScope + "." + spelled;
            const bool ambiguousScope = compiler->IsNamespace(candidateScope);
            if ((!ambiguousScope || declaredInAggregate)
                && (isKnownType(candidate) || declaredInAggregate))
                return candidate;
        }
    }
    const size_t memberDot = spelled.find('.');
    if (memberDot != std::string::npos)
    {
        const std::string owner = spelled.substr(0, memberDot);
        for (std::string scope = namespaceName; !scope.empty(); )
        {
            const std::string ownerCandidate = scope + "." + owner;
            if (isKnownType(ownerCandidate))
            {
                const std::string candidate = scope + "." + spelled;
                if (isKnownType(candidate)) return candidate;
            }
            const size_t dot = scope.rfind('.');
            if (dot == std::string::npos) break;
            scope.resize(dot);
        }
    }
    if (isKnownType(resolvedName)) return {};
    return {};
}
std::vector<EnclosingAggregateLookupScope> EnclosingAggregateScopesForCall(
    LLVMBackend* compiler, antlr4::tree::ParseTree* node)
{
    return EnclosingAggregateScopesFromContext(compiler, node);
}
std::vector<EnclosingAggregateLookupScope> EnclosingAggregateScopesFromName(
    const std::string& scopeName)
{
    std::vector<EnclosingAggregateLookupScope> scopes;
    std::string prefix;
    size_t begin = 0;
    while (begin < scopeName.size())
    {
        const size_t end = scopeName.find('.', begin);
        const std::string part = scopeName.substr(begin,
            end == std::string::npos ? std::string::npos : end - begin);
        prefix += (prefix.empty() ? "" : ".") + part;
        scopes.push_back({ prefix, nullptr });
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    std::reverse(scopes.begin(), scopes.end());
    return scopes;
}
std::string EnclosingAggregateScopeFromContext(
    LLVMBackend* compiler, antlr4::tree::ParseTree* node)
{
    const auto scopes = EnclosingAggregateScopesFromContext(compiler, node);
    return scopes.empty() ? std::string{} : scopes.front().Name;
}
bool IsCompleteClassBodyUse(antlr4::tree::ParseTree* node)
{
    for (auto* parent = node != nullptr ? node->parent : nullptr; parent != nullptr; parent = parent->parent)
    {
        if (dynamic_cast<CFlatParser::CompoundStatementContext*>(parent) != nullptr) return true;
        if (dynamic_cast<CFlatParser::AggregateMemberContext*>(parent) != nullptr) return false;
    }
    return false;
}
bool HasVisibleNonGenericAggregateName(
    const EnclosingAggregateLookupScope& scope, const std::string& name,
    antlr4::tree::ParseTree* useSite)
{
    auto* use = dynamic_cast<antlr4::ParserRuleContext*>(useSite);
    if (scope.Definition == nullptr || use == nullptr || use->getStart() == nullptr) return false;
    const size_t useToken = use->getStart()->getTokenIndex();
    const bool completeClass = IsCompleteClassBodyUse(useSite);
    auto matches = [&](antlr4::ParserRuleContext* definition,
                       CFlatParser::DirectDeclaratorContext* declarator,
                       bool isGeneric)
    {
        return definition != nullptr && definition->getStart() != nullptr && declarator != nullptr
            && !isGeneric && declarator->getText() == name
            && (completeClass || definition->getStart()->getTokenIndex() < useToken);
    };
    if (auto* structure = dynamic_cast<CFlatParser::StructDefinitionContext*>(scope.Definition))
    {
        for (auto* member : structure->aggregateMember())
        {
            if (auto* nested = member->structDefinition(); nested != nullptr
                && matches(nested, nested->directDeclarator(), nested->genericTypeParameters() != nullptr))
                return true;
            if (auto* nested = member->classDefinition(); nested != nullptr
                && matches(nested, nested->directDeclarator(), nested->genericTypeParameters() != nullptr))
                return true;
        }
    }
    else if (auto* classDefinition = dynamic_cast<CFlatParser::ClassDefinitionContext*>(scope.Definition))
    {
        for (auto* member : classDefinition->aggregateMember())
        {
            if (auto* nested = member->structDefinition(); nested != nullptr
                && matches(nested, nested->directDeclarator(), nested->genericTypeParameters() != nullptr))
                return true;
            if (auto* nested = member->classDefinition(); nested != nullptr
                && matches(nested, nested->directDeclarator(), nested->genericTypeParameters() != nullptr))
                return true;
        }
    }
    return false;
}
// Namespace-scope analogue of HasVisibleNonGenericAggregateName. A non-template type `name` declared
// directly in the use's namespace or an enclosing one hides a generic template of that name further
// out, wherever it is declared in the file (CFlat namespace lookup is order-independent, as QUALORDER).
// Walks the namespace chain innermost first.
bool NonGenericNamespaceTypeHides(LLVMBackend* compiler, antlr4::tree::ParseTree* useSite,
    const std::string& name)
{
    if (compiler == nullptr || name.empty() || name.find('.') != std::string::npos) return false;
    std::string ns = NamespaceNameFromContext(compiler, useSite);
    while (!ns.empty())
    {
        const std::string key = ns + "." + name;
        if (compiler->IsGenericTemplateKey(key)) return false;
        if (compiler->IsNonGenericNamespaceTypeKey(key)) return true;
        const size_t dot = ns.rfind('.');
        ns = dot == std::string::npos ? std::string{} : ns.substr(0, dot);
    }
    return false;
}
bool HasEnclosingAggregateContext(antlr4::tree::ParseTree* node)
{
    for (auto* parent = node != nullptr ? node->parent : nullptr;
         parent != nullptr; parent = parent->parent)
        if (dynamic_cast<CFlatParser::StructDefinitionContext*>(parent) != nullptr
            || dynamic_cast<CFlatParser::ClassDefinitionContext*>(parent) != nullptr)
            return true;
    return false;
}
bool IsAggregateMemberDeclarationTypeUse(antlr4::tree::ParseTree* node)
{
    for (auto* parent = node != nullptr ? node->parent : nullptr;
         parent != nullptr; parent = parent->parent)
    {
        if (dynamic_cast<CFlatParser::FunctionDefinitionContext*>(parent) != nullptr)
            return false;
        if (auto* member = dynamic_cast<CFlatParser::AggregateMemberContext*>(parent))
            return member->declaration() != nullptr;
    }
    return false;
}
bool GenericTemplateDeclaredBeforeUse(
    LLVMBackend* compiler, const std::string& base, antlr4::tree::ParseTree* useSite)
{
    auto* useContext = dynamic_cast<antlr4::ParserRuleContext*>(useSite);
    if (compiler == nullptr || useContext == nullptr || useContext->getStart() == nullptr) return true;
    auto* definition = compiler->GetGenericTemplateDefinitionContext(base);
    return definition == nullptr || definition->getStart() == nullptr
        || definition->getStart()->getTokenIndex() < useContext->getStart()->getTokenIndex();
}
bool GenericTemplateKeyNestedInScope(const std::string& key, const std::string& scope)
{
    return !scope.empty() && key.size() > scope.size()
        && key.compare(0, scope.size(), scope) == 0 && key[scope.size()] == '.';
}
bool GenericTemplateKeyNestedInAnyScope(
    const std::string& key, const std::vector<EnclosingAggregateLookupScope>& scopes)
{
    for (const auto& scope : scopes)
        if (GenericTemplateKeyNestedInScope(key, scope.Name)) return true;
    return false;
}
std::string ResolveGenericTemplateBaseAtScope(
    LLVMBackend* compiler, const std::string& spelledBase,
    const std::vector<EnclosingAggregateLookupScope>& enclosingScopes,
    antlr4::tree::ParseTree* useSite, const std::string& lookupName,
    NonGenericRefusal* nonGenericRefusal)
{
    if (compiler == nullptr || spelledBase.empty()) return spelledBase;
    const std::string& lexicalName = lookupName.empty() ? spelledBase : lookupName;
    const bool lexicalBareName = lexicalName.find('.') == std::string::npos;
    const bool hasExactKey = compiler->IsGenericTemplateKey(spelledBase);
    if (lexicalBareName)
    {
        for (const auto& scope : enclosingScopes)
        {
            std::string nested = scope.Name + "." + lexicalName;
            if (compiler->IsGenericTemplateKey(nested))
            {
                if (IsAggregateMemberDeclarationTypeUse(useSite)
                    && !GenericTemplateDeclaredBeforeUse(compiler, nested, useSite))
                {
                    std::string fallback = hasExactKey ? spelledBase
                        : compiler->ResolveGenericBaseAlias(spelledBase);
                    if (!compiler->IsGenericTemplateKey(fallback)
                        || GenericTemplateKeyNestedInAnyScope(fallback, enclosingScopes)) return {};
                    return fallback;
                }
                return nested;
            }
            if (HasVisibleNonGenericAggregateName(scope, lexicalName, useSite))
            {
                if (nonGenericRefusal != nullptr) *nonGenericRefusal = NonGenericRefusal::EnclosingAggregate;
                return {};
            }
            // Namespace entry: the check walks the whole chain, so it runs HERE, before an outer
            // namespace's template is returned by a later loop entry.
            if (scope.Definition == nullptr && lexicalBareName
                && NonGenericNamespaceTypeHides(compiler, useSite, lexicalName))
            {
                if (nonGenericRefusal != nullptr) *nonGenericRefusal = NonGenericRefusal::EnclosingNamespace;
                return {};
            }
        }
    }
    if (lexicalBareName && NonGenericNamespaceTypeHides(compiler, useSite, lexicalName))
    {
        if (nonGenericRefusal != nullptr) *nonGenericRefusal = NonGenericRefusal::EnclosingNamespace;
        return {};
    }
    const std::string qualifiedSpelling = compiler->ResolveQualifiedName(spelledBase);
    if (!compiler->HasCxxAliasTemplate(qualifiedSpelling)
        && compiler->IsGenericBaseAlias(qualifiedSpelling))
        return compiler->ResolveGenericBaseAlias(qualifiedSpelling);
    if (compiler->HasCxxAliasTemplate(lexicalName)) return spelledBase;
    if (compiler->IsGenericBaseAlias(lexicalName))
        return compiler->ResolveGenericBaseAlias(lexicalName);
    if (hasExactKey)
    {
        if (IsAggregateMemberDeclarationTypeUse(useSite)
            && GenericTemplateKeyNestedInAnyScope(spelledBase, enclosingScopes)
            && !GenericTemplateDeclaredBeforeUse(compiler, spelledBase, useSite)) return {};
        return spelledBase;
    }
    std::string base = compiler->ResolveGenericBaseAlias(spelledBase);
    if (compiler->IsGenericTemplateKey(base))
    {
        if (IsAggregateMemberDeclarationTypeUse(useSite)
            && GenericTemplateKeyNestedInAnyScope(base, enclosingScopes)
            && !GenericTemplateDeclaredBeforeUse(compiler, base, useSite)) return {};
        return base;
    }
    for (const auto& scope : enclosingScopes)
    {
        std::string nested = scope.Name + "." + base;
        if (compiler->IsGenericTemplateKey(nested))
        {
            if (IsAggregateMemberDeclarationTypeUse(useSite)
                && !GenericTemplateDeclaredBeforeUse(compiler, nested, useSite))
            {
                std::string fallback = compiler->ResolveGenericTemplateBase(base);
                if (!compiler->IsGenericTemplateKey(fallback)
                    || (GenericTemplateKeyNestedInAnyScope(fallback, enclosingScopes)
                        && !GenericTemplateDeclaredBeforeUse(compiler, fallback, useSite))) return {};
                return fallback;
            }
            return nested;
        }
        if (HasVisibleNonGenericAggregateName(scope, base, useSite))
        {
            if (nonGenericRefusal != nullptr) *nonGenericRefusal = NonGenericRefusal::EnclosingAggregate;
            return {};
        }
    }
    return compiler->ResolveGenericTemplateBase(base);
}
std::string ResolveGenericTemplateSpellingAtScope(
    LLVMBackend* compiler, const std::string& spelledBase, std::vector<std::string>& args,
    const std::vector<EnclosingAggregateLookupScope>& enclosingScopes,
    antlr4::tree::ParseTree* useSite,
    const std::string& lookupName, bool report, NonGenericRefusal* nonGenericRefusal)
{
    if (compiler == nullptr || spelledBase.empty()) return spelledBase;
    std::string base = ResolveGenericTemplateBaseAtScope(
        compiler, spelledBase, enclosingScopes, useSite, lookupName, nonGenericRefusal);
    if (base.empty() || base == spelledBase)
    {
        base = spelledBase;
        compiler->ResolveGenericAliasSpelling(base, args, report);
        base = ResolveGenericTemplateBaseAtScope(
            compiler, base, enclosingScopes, useSite, lookupName, nonGenericRefusal);
    }
    else if (compiler->HasCxxAliasTemplate(base))
        compiler->ResolveGenericAliasSpelling(base, args, report);
    else if (base == "unique" && compiler->IsGenericBaseAlias(spelledBase))
        compiler->ResolveGenericAliasSpelling(base, args, report);
    return base;
}
std::vector<EnclosingAggregateLookupScope> EnclosingScopesForLegacyScope(
    LLVMBackend* compiler, const std::string& scopeName, antlr4::tree::ParseTree* useSite)
{
    auto scopes = EnclosingAggregateScopesFromContext(compiler, useSite);
    if (scopeName.empty()) return scopes;
    // The legacy string names the scope being lowered (an instantiation may spell it differently
    // from the template's parse tree); the parse tree only supplies each level's definition.
    auto named = EnclosingAggregateScopesFromName(scopeName);
    for (auto& scope : named)
        for (const auto& candidate : scopes)
            if (candidate.Name == scope.Name) { scope.Definition = candidate.Definition; break; }
    return named;
}
std::string ResolveGenericTemplateBaseAtScope(
    LLVMBackend* compiler, const std::string& spelledBase, const std::string& enclosingScope,
    antlr4::tree::ParseTree* useSite, const std::string& lookupName,
    NonGenericRefusal* nonGenericRefusal)
{
    return ResolveGenericTemplateBaseAtScope(compiler, spelledBase,
        EnclosingScopesForLegacyScope(compiler, enclosingScope, useSite), useSite, lookupName,
        nonGenericRefusal);
}
std::string ResolveGenericTemplateSpellingAtScope(
    LLVMBackend* compiler, const std::string& spelledBase, std::vector<std::string>& args,
    const std::string& enclosingScope, antlr4::tree::ParseTree* useSite,
    const std::string& lookupName, bool report, NonGenericRefusal* nonGenericRefusal)
{
    return ResolveGenericTemplateSpellingAtScope(compiler, spelledBase, args,
        EnclosingScopesForLegacyScope(compiler, enclosingScope, useSite), useSite, lookupName, report,
        nonGenericRefusal);
}
LLVM_ATTRIBUTE_NOINLINE static bool ResolveDefaultedCxxAliasTypeName(
    LLVMBackend* compiler, std::string& typeName, antlr4::ParserRuleContext* useSite,
    CxxTypeErrorReporter reportCxxTypeError, void* reportContext)
{
    if (compiler == nullptr || !compiler->IsGenericBaseAlias(typeName)) return false;
    std::string resolvedType;
    std::string error;
    if (compiler->TryResolveDefaultedCxxGenericBaseAlias(typeName, resolvedType, error))
    {
        typeName = std::move(resolvedType);
        return true;
    }
    if (!error.empty() && useSite != nullptr && reportCxxTypeError != nullptr)
        reportCxxTypeError(reportContext, useSite, error);
    return false;
}

bool ResolveCanonicalTypeSpecifier(
    LLVMBackend* compiler, const std::vector<CFlatParser::TypeSpecifierContext*>& specs,
    const std::unordered_map<std::string, std::string>* substitutions,
    const TypeNameArgumentResolver& resolveTypeArg,
    const TypeNameMemberResolver& resolveMemberType,
    bool reportGenericAlias, CanonicalTypeSpecifier& out,
    PrimitiveTypeError* errorOut,
    const TypeNameTupleResolver& resolveTupleType,
    CxxTypeErrorReporter reportCxxTypeError, void* reportContext)
{
    if (compiler == nullptr || specs.empty()) return false;
    PrimitiveTypeError typeError;
    if (specs.size() > 1)
    {
        std::vector<std::string> words;
        words.reserve(words.size() + specs.size());
        for (auto* spec : specs) words.push_back(spec->getText());
        if (!CanonicalizePrimitiveTypeWords(words, out.TypeName, typeError))
        {
            if (errorOut != nullptr) *errorOut = typeError;
            return false;
        }
    }
    else
    {
        auto* typeSpec = specs.front();
        if (typeSpec == nullptr || typeSpec->typeName() != nullptr
            || typeSpec->functionPointerSpecifier() != nullptr)
            return false;

        if (typeSpec->rootQualifiedTypeName() != nullptr)
            out.TypeName = RootQualifiedTypeSpelling(typeSpec);
        else if (auto* tuple = typeSpec->tupleTypeSpecifier())
        {
            if (!resolveTupleType) return false;
            out.TypeName = resolveTupleType(tuple);
        }
        else if (auto* generic = GenericSpecOf(typeSpec, out.GenericBase))
        {
            out.IsGeneric = true;
            for (auto* entry : generic->typeParameterList()->typeParameterEntry())
                out.GenericArgs.push_back(resolveTypeArg
                    ? resolveTypeArg(entry) : entry->getText());
            for (auto& arg : out.GenericArgs)
                ResolveEnumTypeArgumentBase(arg, [compiler](const std::string& base) {
                    return compiler->ResolveEnumTypeName(base);
                });
            const std::string spelledBase = out.GenericBase;
            out.GenericBase = ResolveGenericTemplateSpellingAtScope(
                compiler, spelledBase, out.GenericArgs,
                EnclosingAggregateScopesFromContext(compiler, typeSpec), typeSpec,
                spelledBase, reportGenericAlias);
            compiler->CompleteCxxTemplateTypeArgs(out.GenericBase, out.GenericArgs);
            out.TypeName = MangleGenericInstance(*compiler, out.GenericBase, out.GenericArgs);
        }
        else if (auto* memberType = typeSpec->memberTypeSpecifier())
            out.TypeName = resolveMemberType ? resolveMemberType(memberType) : memberType->getText();
        else
        {
            if (!CanonicalizePrimitiveTypeWords({ typeSpec->getText() }, out.TypeName, typeError))
            {
                if (errorOut != nullptr) *errorOut = typeError;
                return false;
            }
        }
    }

    if (substitutions != nullptr)
        if (auto it = substitutions->find(out.TypeName); it != substitutions->end())
            out.TypeName = it->second;
    out.TypeName = ResolveQualifiedTypeSpecifierName(compiler,
        specs.size() == 1 ? specs.front() : nullptr, out.TypeName);
    out.ConstMask |= compiler->ResolveTypeAliasConstMask(out.TypeName);
    out.TypeName = compiler->ResolveTypeAlias(out.TypeName);
    ResolveDefaultedCxxAliasTypeName(compiler, out.TypeName,
        specs.size() == 1 ? specs.front() : nullptr, reportCxxTypeError, reportContext);
    out.PointerDepth = PeelAliasPointerStars(out.TypeName);
    return true;
}
void ApplyConstMaskThroughDepth(uint32_t& mask, int depth)
{
    depth = std::clamp(depth, 0, 32);
    mask = LLVMBackend::TypeAndValue::ConstBitsThrough(depth);
}
bool ResolveConstTypeSyntax(
    LLVMBackend* compiler, CFlatParser::TypeNameContext* ctx, ConstTypeSyntax& out,
    const std::unordered_map<std::string, std::string>* substitutions,
    const TypeNameArgumentResolver& resolveTypeArg,
    const TypeNameMemberResolver& resolveMemberType,
    const TypeNameTupleResolver& resolveTupleType,
    CxxTypeErrorReporter reportCxxTypeError, void* reportContext)
{
    if (compiler == nullptr || ctx == nullptr) return false;
    auto* spec = ctx->specifierQualifierList();
    if (spec == nullptr || spec->typeSpecifier().empty()) return false;
    auto specs = spec->typeSpecifier();
    auto* typeSpec = specs.front();
    if (auto* nested = typeSpec->typeName())
    {
        if (specs.size() != 1) return false;
        if (!ResolveConstTypeSyntax(compiler, nested, out, substitutions,
                resolveTypeArg, resolveMemberType, resolveTupleType,
                reportCxxTypeError, reportContext)) return false;
        ApplyConstMaskThroughDepth(out.ConstMask, out.PointerDepth);
    }
    else
    {
        CanonicalTypeSpecifier resolved;
        if (!ResolveCanonicalTypeSpecifier(compiler, specs, substitutions, resolveTypeArg,
                resolveMemberType, false, resolved, nullptr, resolveTupleType,
                reportCxxTypeError, reportContext))
            return false;
        out.TypeName = resolved.TypeName;
        out.ConstMask = resolved.ConstMask;
        out.PointerDepth = resolved.PointerDepth;
    }

    if (auto* abstractDecl = ctx->abstractDeclarator())
    {
        if (abstractDecl->pointer() != nullptr)
            out.PointerDepth += PointerDepthOf(abstractDecl->pointer());
        if (auto* dims = ArrayDimsOf(abstractDecl); dims != nullptr
            && dims->assignmentExpression().empty())
            out.IsArrayView = true;
    }
    if (spec->typeQualifier() != nullptr && spec->typeQualifier()->getText() == "const")
        ApplyConstMaskThroughDepth(out.ConstMask, out.PointerDepth);
    return true;
}
bool ResolveConstTypeSpecifierSyntax(
    LLVMBackend* compiler, CFlatParser::TypeSpecifierContext* typeSpec, ConstTypeSyntax& out,
    const std::unordered_map<std::string, std::string>* substitutions,
    const TypeNameArgumentResolver& resolveTypeArg,
    const TypeNameMemberResolver& resolveMemberType,
    const TypeNameTupleResolver& resolveTupleType,
    CxxTypeErrorReporter reportCxxTypeError, void* reportContext)
{
    if (typeSpec == nullptr || typeSpec->typeName() == nullptr) return false;
    if (!ResolveConstTypeSyntax(compiler, typeSpec->typeName(), out, substitutions,
            resolveTypeArg, resolveMemberType, resolveTupleType,
            reportCxxTypeError, reportContext)) return false;
    ApplyConstMaskThroughDepth(out.ConstMask, out.PointerDepth);
    return true;
}
bool ResolveConstCastTypeSyntax(
    LLVMBackend* compiler, CFlatParser::CastTypeNameContext* ctx, ConstTypeSyntax& out,
    const std::unordered_map<std::string, std::string>* substitutions,
    const TypeNameArgumentResolver& resolveTypeArg,
    const TypeNameMemberResolver& resolveMemberType,
    const TypeNameTupleResolver& resolveTupleType,
    CxxTypeErrorReporter reportCxxTypeError, void* reportContext)
{
    if (compiler == nullptr || ctx == nullptr || ctx->typeSpecifier().empty()) return false;
    auto specs = ctx->typeSpecifier();
    if (specs.size() == 1 && specs.front()->typeName() != nullptr)
    {
        if (!ResolveConstTypeSpecifierSyntax(compiler, specs.front(), out, substitutions,
                resolveTypeArg, resolveMemberType, resolveTupleType,
                reportCxxTypeError, reportContext)) return false;
    }
    else
    {
        CanonicalTypeSpecifier resolved;
        if (!ResolveCanonicalTypeSpecifier(compiler, specs, substitutions, resolveTypeArg,
                resolveMemberType, false, resolved, nullptr, resolveTupleType,
                reportCxxTypeError, reportContext))
            return false;
        out.ConstMask = resolved.ConstMask;
        out.PointerDepth = resolved.PointerDepth;
        out.TypeName = resolved.TypeName;
    }
    if (ctx->abstractDeclarator() != nullptr)
    {
        out.PointerDepth += PointerDepthOf(ctx->abstractDeclarator()->pointer());
        if (auto* dims = ArrayDimsOf(ctx->abstractDeclarator()); dims != nullptr
            && dims->assignmentExpression().empty())
            out.IsArrayView = true;
    }
    if (ctx->typeQualifier() != nullptr && ctx->typeQualifier()->getText() == "const")
        ApplyConstMaskThroughDepth(out.ConstMask, out.PointerDepth);
    return true;
}
std::string ResolveCastTypeNameForScanner(
    LLVMBackend* compiler, CFlatParser::CastTypeNameContext* castTypeName)
{
    ConstTypeSyntax shape;
    return ResolveConstCastTypeSyntax(compiler, castTypeName, shape) ? shape.TypeName : std::string();
}
std::unordered_set<std::string> CollectFunctionLocalEnumNames(
    CFlatParser::FunctionDefinitionContext* function)
{
    std::unordered_set<std::string> names;
    if (function == nullptr || function->compoundStatement() == nullptr) return names;
    std::function<void(antlr4::tree::ParseTree*)> visit = [&](antlr4::tree::ParseTree* node)
    {
        if (auto* enumSpec = dynamic_cast<CFlatParser::EnumSpecifierContext*>(node);
            enumSpec != nullptr && enumSpec->Identifier() != nullptr)
            names.insert(enumSpec->Identifier()->getText());
        if (!node->children.empty())
            for (auto* child : node->children) visit(child);
    };
    visit(function->compoundStatement());
    return names;
}
std::string SimpleTypeSpecifierName(CFlatParser::TypeSpecifierContext* typeSpec)
{
    if (typeSpec == nullptr) return {};
    auto* generic = typeSpec->genericIdentifier();
    if (generic == nullptr || generic->genericTypeParameters() != nullptr
        || generic->Identifier() == nullptr)
        return {};
    return generic->Identifier()->getText();
}
bool TypeSpecifierNamesFunctionLocalEnum(
    CFlatParser::TypeSpecifierContext* typeSpec,
    const std::unordered_set<std::string>& localEnumNames)
{
    if (typeSpec == nullptr) return false;
    if (auto* typeName = typeSpec->typeName())
        if (auto* spec = typeName->specifierQualifierList())
            for (auto* nested : spec->typeSpecifier())
                if (TypeSpecifierNamesFunctionLocalEnum(nested, localEnumNames)) return true;
    if (auto* tuple = typeSpec->tupleTypeSpecifier())
        for (auto* entry : tuple->tupleTypeEntry())
            if (TypeSpecifierNamesFunctionLocalEnum(entry->typeSpecifier(), localEnumNames)) return true;
    std::string nestedBase;
    if (auto* nested = GenericSpecOf(typeSpec, nestedBase))
        for (auto* entry : nested->typeParameterList()->typeParameterEntry())
            if (TypeArgumentNamesFunctionLocalEnum(entry, localEnumNames)) return true;
    if (auto* memberType = typeSpec->memberTypeSpecifier())
        if (auto* ownerParams = memberType->memberTypeOwner()->genericTypeParameters())
            for (auto* entry : ownerParams->typeParameterList()->typeParameterEntry())
                if (TypeArgumentNamesFunctionLocalEnum(entry, localEnumNames)) return true;
    const std::string simpleName = SimpleTypeSpecifierName(typeSpec);
    return !simpleName.empty() && localEnumNames.count(simpleName) != 0;
}
bool TypeArgumentNamesFunctionLocalEnum(
    CFlatParser::TypeParameterEntryContext* entry,
    const std::unordered_set<std::string>& localEnumNames)
{
    return entry != nullptr
        && TypeSpecifierNamesFunctionLocalEnum(entry->typeSpecifier(), localEnumNames);
}
bool GenericArgumentsNameFunctionLocalEnum(
    CFlatParser::GenericTypeParametersContext* params,
    const std::unordered_set<std::string>* localEnumNames)
{
    if (params == nullptr || localEnumNames == nullptr || localEnumNames->empty()) return false;
    for (auto* entry : params->typeParameterList()->typeParameterEntry())
        if (TypeArgumentNamesFunctionLocalEnum(entry, *localEnumNames)) return true;
    return false;
}
bool TypeSpecifierNeedsFunctionLocalEnumDeferral(
    CFlatParser::TypeSpecifierContext* typeSpec,
    const std::unordered_set<std::string>& localEnumNames,
    LLVMBackend* compiler,
    const std::unordered_set<std::string>* scannedNonLocalEnumNames,
    const std::unordered_set<std::string>* scannedTypeNames)
{
    if (typeSpec == nullptr || compiler == nullptr) return false;
    if (auto* typeName = typeSpec->typeName())
        if (auto* spec = typeName->specifierQualifierList())
            for (auto* nested : spec->typeSpecifier())
                if (TypeSpecifierNeedsFunctionLocalEnumDeferral(nested, localEnumNames, compiler,
                        scannedNonLocalEnumNames, scannedTypeNames)) return true;
    if (auto* tuple = typeSpec->tupleTypeSpecifier())
        for (auto* tupleEntry : tuple->tupleTypeEntry())
            if (TypeSpecifierNeedsFunctionLocalEnumDeferral(tupleEntry->typeSpecifier(),
                    localEnumNames, compiler, scannedNonLocalEnumNames, scannedTypeNames)) return true;
    std::string nestedBase;
    if (auto* nested = GenericSpecOf(typeSpec, nestedBase))
        for (auto* nestedEntry : nested->typeParameterList()->typeParameterEntry())
            if (TypeArgumentNeedsFunctionLocalEnumDeferral(nestedEntry, localEnumNames, compiler,
                    scannedNonLocalEnumNames, scannedTypeNames))
                return true;
    if (auto* memberType = typeSpec->memberTypeSpecifier())
        if (auto* ownerParams = memberType->memberTypeOwner()->genericTypeParameters())
            for (auto* nestedEntry : ownerParams->typeParameterList()->typeParameterEntry())
                if (TypeArgumentNeedsFunctionLocalEnumDeferral(
                        nestedEntry, localEnumNames, compiler,
                        scannedNonLocalEnumNames, scannedTypeNames))
                    return true;

    const std::string spelling = SimpleTypeSpecifierName(typeSpec);
    if (spelling.empty() || localEnumNames.count(spelling) == 0) return false;
    const std::string resolved = compiler->ResolveTypeArgBaseName(spelling);
    const std::string enumKey = compiler->ResolveEnumTypeName(spelling);
    if (!enumKey.empty())
        return enumKey.find('@') != std::string::npos || compiler->IsGenericLocalEnumKey(enumKey);
    const std::string alias = compiler->ResolveTypeAlias(spelling);
    if (alias.find('@') != std::string::npos || compiler->IsGenericLocalEnumKey(alias)) return true;
    if (compiler->IsDataStructure(spelling) || compiler->IsDataStructure(resolved)
        || compiler->HasInterface(spelling) || compiler->HasInterface(resolved)
        || (!alias.empty() && alias != spelling && compiler->IsKnownTypeName(alias)))
        return false;
    const auto isScannedNonLocalType = [scannedNonLocalEnumNames, scannedTypeNames](
        const std::string& candidate) {
        return (scannedNonLocalEnumNames != nullptr
                && scannedNonLocalEnumNames->count(candidate) != 0)
            || (scannedTypeNames != nullptr && scannedTypeNames->count(candidate) != 0);
    };
    return compiler->FirstVisibleScopedKey(resolved.empty() ? spelling : resolved,
        isScannedNonLocalType, ScopedLookupOptions{ .ResolveFirstComponentAlias = false }).empty();
}
bool TypeArgumentNeedsFunctionLocalEnumDeferral(
    CFlatParser::TypeParameterEntryContext* entry,
    const std::unordered_set<std::string>& localEnumNames,
    LLVMBackend* compiler,
    const std::unordered_set<std::string>* scannedNonLocalEnumNames,
    const std::unordered_set<std::string>* scannedTypeNames)
{
    return entry != nullptr
        && TypeSpecifierNeedsFunctionLocalEnumDeferral(entry->typeSpecifier(), localEnumNames,
            compiler, scannedNonLocalEnumNames, scannedTypeNames);
}
bool GenericArgumentsNeedFunctionLocalEnumDeferral(
    CFlatParser::GenericTypeParametersContext* params,
    const std::unordered_set<std::string>* localEnumNames,
    LLVMBackend* compiler,
    const std::unordered_set<std::string>* scannedNonLocalEnumNames,
    const std::unordered_set<std::string>* scannedTypeNames)
{
    if (params == nullptr || localEnumNames == nullptr || localEnumNames->empty()) return false;
    for (auto* entry : params->typeParameterList()->typeParameterEntry())
        if (TypeArgumentNeedsFunctionLocalEnumDeferral(entry, *localEnumNames, compiler,
                scannedNonLocalEnumNames, scannedTypeNames))
            return true;
    return false;
}
bool TypeSpecifierUsesRegisteredLocalEnum(
    CFlatParser::TypeSpecifierContext* typeSpec,
    LLVMBackend* compiler,
    const std::unordered_map<std::string, std::string>* substitutions)
{
    if (typeSpec == nullptr || compiler == nullptr) return false;
    if (auto* typeName = typeSpec->typeName())
        if (auto* spec = typeName->specifierQualifierList())
            for (auto* nested : spec->typeSpecifier())
                if (TypeSpecifierUsesRegisteredLocalEnum(nested, compiler, substitutions)) return true;
    if (auto* tuple = typeSpec->tupleTypeSpecifier())
        for (auto* tupleEntry : tuple->tupleTypeEntry())
            if (TypeSpecifierUsesRegisteredLocalEnum(tupleEntry->typeSpecifier(), compiler,
                    substitutions)) return true;
    std::string nestedBase;
    if (auto* nested = GenericSpecOf(typeSpec, nestedBase))
        for (auto* nestedEntry : nested->typeParameterList()->typeParameterEntry())
            if (TypeArgumentUsesRegisteredLocalEnum(nestedEntry, compiler, substitutions)) return true;
    if (auto* memberType = typeSpec->memberTypeSpecifier())
        if (auto* ownerParams = memberType->memberTypeOwner()->genericTypeParameters())
            for (auto* nestedEntry : ownerParams->typeParameterList()->typeParameterEntry())
                if (TypeArgumentUsesRegisteredLocalEnum(nestedEntry, compiler, substitutions))
                    return true;

    std::string spelling = SimpleTypeSpecifierName(typeSpec);
    if (spelling.empty()) return false;
    if (substitutions != nullptr)
        if (auto it = substitutions->find(spelling); it != substitutions->end()) spelling = it->second;
    const std::string enumKey = compiler->ResolveEnumTypeName(spelling);
    return !enumKey.empty() && compiler->IsGenericLocalEnumKey(enumKey);
}
bool TypeArgumentUsesRegisteredLocalEnum(
    CFlatParser::TypeParameterEntryContext* entry,
    LLVMBackend* compiler,
    const std::unordered_map<std::string, std::string>* substitutions)
{
    if (entry == nullptr || entry->typeSpecifier() == nullptr || compiler == nullptr) return false;
    auto* typeSpec = entry->typeSpecifier();
    std::string genericBase;
    if (typeSpec->tupleTypeSpecifier() != nullptr || typeSpec->typeName() != nullptr
        || GenericSpecOf(typeSpec, genericBase) != nullptr
        || typeSpec->memberTypeSpecifier() != nullptr)
        return TypeSpecifierUsesRegisteredLocalEnum(typeSpec, compiler, substitutions);
    std::string spelling = CanonicalTemplateTypeArgument(entry);
    if (substitutions != nullptr)
        if (auto it = substitutions->find(spelling); it != substitutions->end()) spelling = it->second;
    const std::string enumKey = compiler->ResolveEnumTypeName(spelling);
    return !enumKey.empty() && compiler->IsGenericLocalEnumKey(enumKey);
}
bool GenericArgumentsUseRegisteredLocalEnum(
    CFlatParser::GenericTypeParametersContext* params,
    LLVMBackend* compiler,
    const std::unordered_map<std::string, std::string>* substitutions)
{
    if (params == nullptr) return false;
    for (auto* entry : params->typeParameterList()->typeParameterEntry())
        if (TypeArgumentUsesRegisteredLocalEnum(entry, compiler, substitutions)) return true;
    return false;
}
void MainListener::ValidateDeclaratorPointerDepth(antlr4::ParserRuleContext* site, int depth)
{
    if (depth > MaxDeclaratorPointerDepth)
        LogErrorContext(site, std::format("pointer depth {} exceeds the supported {} levels",
            depth, MaxDeclaratorPointerDepth));
}
int ReconcilePointerDepth(bool pointer, int stars)
{
    if (!pointer) return 0;
    return stars > 0 ? stars : 1;
}
uint32_t PointeeConstOfComponent(uint32_t mask, int depth, bool preserveAliasReferentConst)
{
    if (mask == 0) return 0;
    if (preserveAliasReferentConst && depth <= 0) return mask & 1;
    return mask & LLVMBackend::TypeAndValue::ConstBitsBelow(depth);
}
std::string BuildEncodedClosureName(const LLVMBackend* compiler, bool isThin,
    const std::string& ret, int retDepth,
    const std::vector<std::pair<std::string, int>>& params,
    int64_t retConstMask, const std::vector<int64_t>* paramConstMasks,
    const std::vector<bool>* paramArrayViews,
    const std::vector<bool>* paramAliases, bool retAlias)
{
    if (compiler == nullptr) return ret;
    return MangleClosureType(*compiler, isThin, ret, retDepth, params, retConstMask,
        paramConstMasks, paramArrayViews, paramAliases, retAlias);
}
void PeelTypeArgSuffix(std::string& name, int& pointerDepth, bool& arrayView,
    bool* aliasOut, int* elemPointerDepth)
{
    // The `alias` qualifier is a leading prefix; peel it before the suffix loop so the underlying
    // type remains available to the resolver. It is re-added by callers that mangle alias args.
    bool hadAlias = StripAliasQualifier(name);
    if (aliasOut != nullptr) *aliasOut = hadAlias;
    int stars = 0;
    bool view = false;
    for (;;)
    {
        if (name.size() >= 2 && name.compare(name.size() - 2, 2, "[]") == 0)
        {
            view = true;
            name.erase(name.size() - 2);
        }
        else if (!name.empty() && name.back() == '*')
        {
            stars++;
            name.pop_back();
        }
        else
            break;
    }
    // "[]*" is rejected upstream, so every star sits LEFT of the brackets and decorates the
    // ELEMENT: `int*[]` is a view of `int*`, not a deeper pointer.
    // A view written by the CALLER (`T[]` with T bound to "int*") owns the peeled stars too.
    if (view || arrayView)
    {
        arrayView = true;
        if (pointerDepth < 1) pointerDepth = 1;
        if (elemPointerDepth != nullptr) *elemPointerDepth += stars;
    }
    else
        pointerDepth += stars;
}
bool PeelAliasArrayDims(std::string& name, std::vector<uint64_t>& dims)
{
    std::vector<uint64_t> rev;  // collected inner-to-outer (rightmost bracket first)
    while (!name.empty() && name.back() == ']')
    {
        size_t open = name.rfind('[');
        if (open == std::string::npos) break;
        std::string num = name.substr(open + 1, name.size() - open - 2);
        if (num.empty() || num.find_first_not_of("0123456789") != std::string::npos) break;
        rev.push_back(std::strtoull(num.c_str(), nullptr, 10));
        name.erase(open);
    }
    for (auto it = rev.rbegin(); it != rev.rend(); ++it) dims.push_back(*it);
    return !dims.empty();
}
std::vector<CFlatParser::BaseSpecifierContext*> BaseClauseIdentifiers(CFlatParser::ClassDefinitionContext* ctx) { return ctx->baseSpecifier(); }
std::vector<CFlatParser::BaseSpecifierContext*> BaseClauseIdentifiers(CFlatParser::StructDefinitionContext* ctx) { return ctx->baseSpecifier(); }
std::string BaseSpecifierName(CFlatParser::BaseSpecifierContext* spec)
{
    if (spec == nullptr) return {};
    std::string name;
    for (auto* id : spec->Identifier())
        name += (name.empty() ? "" : ".") + id->getText();
    return name;
}
void AppendInterfaceNameCandidates(LLVMBackend* compiler, const std::string& namespaceName,
                                          const std::string& spelled, std::vector<std::string>& out)
{
    if (spelled.empty()) return;
    std::string resolved = compiler->ResolveInterfaceName(spelled);
    out.push_back(resolved);
    if (spelled.find('.') != std::string::npos || namespaceName.empty()) return;
    // Every enclosing-namespace spelling, not just the first visible one - the bare spelling is
    // deliberately left out, it is not a name the interface could be registered under here.
    for (const auto& candidate : compiler->ScopedNameCandidatesIn(namespaceName, spelled))
        if (candidate != resolved && candidate != spelled) out.push_back(candidate);
}
std::string DefinitionSiteText(LLVMBackend* compiler, antlr4::ParserRuleContext* ctx)
{
    if (compiler == nullptr || ctx == nullptr || ctx->getStart() == nullptr) return {};
    return std::format("{}({},{})", compiler->DefinitionSitePath(),
                       (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine());
}
std::string CollapsedSourceText(antlr4::ParserRuleContext* ctx)
{
    if (ctx == nullptr) return {};
    auto* startTok = ctx->getStart();
    auto* stopTok = ctx->getStop();
    if (startTok == nullptr || stopTok == nullptr) return {};
    auto* input = startTok->getInputStream();
    if (input == nullptr) return ctx->getText();
    std::string raw = input->getText(
        antlr4::misc::Interval(startTok->getStartIndex(), stopTok->getStopIndex()));
    std::string out;
    bool pendingSpace = false;
    for (char c : raw)
    {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { pendingSpace = true; continue; }
        if (pendingSpace && !out.empty()) out += ' ';
        pendingSpace = false;
        out += c;
    }
    if (out.empty()) return ctx->getText();
    return TruncateDiagnosticText(std::move(out), kIfConstConditionTextLimit);
}
std::string IfConstConditionText(antlr4::tree::ParseTree* node)
{
    if (auto* d = dynamic_cast<CFlatParser::IfConstDeclarationContext*>(node))
        return CollapsedSourceText(d->expression());
    if (auto* m = dynamic_cast<CFlatParser::IfConstMemberContext*>(node))
        return CollapsedSourceText(m->expression());
    if (auto* i = dynamic_cast<CFlatParser::IfConstInterfaceMemberContext*>(node))
        return CollapsedSourceText(i->expression());
    return {};
}
bool IsIfConstArmBlock(antlr4::tree::ParseTree* node)
{
    return dynamic_cast<CFlatParser::IfConstBlockContext*>(node) != nullptr
        || dynamic_cast<CFlatParser::IfConstMemberBlockContext*>(node) != nullptr
        || dynamic_cast<CFlatParser::IfConstInterfaceBlockContext*>(node) != nullptr;
}
std::string NamespaceDefinitionName(CFlatParser::NamespaceDefinitionContext* ctx)
{
    std::string name;
    for (auto* id : ctx->Identifier())
        name += (name.empty() ? "" : ".") + id->getText();
    return name;
}
std::string JoinQualified(const std::string& outer, const std::string& inner)
{
    if (inner.empty()) return outer;
    return outer.empty() ? inner : outer + "." + inner;
}
void MarkIfConstClassImplsUncertain(LLVMBackend* compiler, antlr4::tree::ParseTree* node,
                                           const std::string& namespaceName,
                                           const std::vector<std::string>& guardChain,
                                           const std::string& qualifier,
                                           bool inGenericTemplate)
{
    if (compiler == nullptr || node == nullptr) return;
    std::string scope = qualifier;
    std::string nsScope = namespaceName;
    if (auto* ns = dynamic_cast<CFlatParser::NamespaceDefinitionContext*>(node))
    {
        scope = JoinQualified(scope, NamespaceDefinitionName(ns));
        nsScope = JoinQualified(nsScope, NamespaceDefinitionName(ns));
    }
    bool childGeneric = inGenericTemplate;
    if (auto* cls = dynamic_cast<CFlatParser::ClassDefinitionContext*>(node))
    {
        // Suppression takes EVERY candidate (a surplus one only weakens a proof), while blame keeps
        // them grouped per base entry so the blame site can pick the one name each entry resolves to.
        std::vector<std::vector<std::string>> named;
        for (auto* spec : BaseClauseIdentifiers(cls))
        {
            std::string spelled = BaseSpecifierName(spec);
            if (spelled.empty()) continue;
            // Candidates come from the NAMESPACE-only scope: the resolver never walks class-name
            // prefixes, so a class-rung candidate is a name the class could never register against.
            std::vector<std::string> candidates;
            AppendInterfaceNameCandidates(compiler, nsScope, spelled, candidates);
            for (const auto& c : candidates)
                compiler->RecordUncertainInterfaceImpl(c);
            // Last component of a qualified spelling: SUPPRESSION only - resolving blame through
            // it fabricates an implements-claim against an unrelated same-named interface.
            if (auto dot = spelled.rfind('.'); dot != std::string::npos)
                compiler->RecordUncertainInterfaceImpl(spelled.substr(dot + 1));
            named.push_back(std::move(candidates));
        }
        // A class inside a generic TEMPLATE body is reconciled once per instantiation - zero times
        // when the template is never used - and the peel below is not idempotent, so never blame it.
        if (auto* dd = cls->directDeclarator())
        {
            scope = JoinQualified(scope, dd->getText());
            if (!inGenericTemplate)
                compiler->RecordIfConstGuardedInterfaceImpl(scope, guardChain, (const void*)cls,
                                                            std::move(named));
        }
        if (cls->genericTypeParameters() != nullptr) childGeneric = true;
    }
    else if (auto* st = dynamic_cast<CFlatParser::StructDefinitionContext*>(node))
    {
        if (auto* dd = st->directDeclarator()) scope = JoinQualified(scope, dd->getText());
        if (st->genericTypeParameters() != nullptr) childGeneric = true;
    }

    std::string cond = IfConstConditionText(node);
    size_t armIndex = 0;
    for (auto* child : node->children)
    {
        // APPEND, never replace: a class under `if const (A) { if const (B) { ... } }` sits under
        // both arms, and dropping A would leave the message blaming a guard that may well hold.
        std::vector<std::string> childChain = guardChain;
        if (!cond.empty() && IsIfConstArmBlock(child))
            childChain.push_back(armIndex++ == 0
                ? std::format("an 'if const ({})' branch", cond)
                : std::format("the else arm of an 'if const ({})'", cond));
        // A chained `else if const` is a nested node, not an arm block, but it is still a LEVEL:
        // reaching it means this node's own arm was rejected, and that fact must stay visible.
        else if (!cond.empty() && !IfConstConditionText(child).empty())
            childChain.push_back(std::format("the else path of an 'if const ({})'", cond));
        MarkIfConstClassImplsUncertain(compiler, child, nsScope, childChain, scope, childGeneric);
    }
}
void RetractIfConstArmGuardedImpls(LLVMBackend* compiler, antlr4::tree::ParseTree* node,
                                          bool nested)
{
    if (compiler == nullptr || node == nullptr) return;
    if (auto* cls = dynamic_cast<CFlatParser::ClassDefinitionContext*>(node))
    {
        if (nested) compiler->PeelIfConstGuardedInterfaceImpl((const void*)cls);
        else        compiler->RetractIfConstGuardedInterfaceImpl((const void*)cls);
    }
    for (auto* child : node->children)
        RetractIfConstArmGuardedImpls(compiler, child,
                                      nested || !IfConstConditionText(child).empty());
}
void ForgetIfConstGuardedImpls(LLVMBackend* compiler, antlr4::tree::ParseTree* node)
{
    if (compiler == nullptr || node == nullptr) return;
    if (auto* cls = dynamic_cast<CFlatParser::ClassDefinitionContext*>(node))
        compiler->RetractIfConstGuardedInterfaceImpl((const void*)cls);
    for (auto* child : node->children)
        ForgetIfConstGuardedImpls(compiler, child);
}
bool TryParseSimdLaneCount(LLVMBackend* compiler,
                                  CFlatParser::AssignmentExpressionContext* expr,
                                  uint64_t& lanesOut, std::string& errorOut)
{
    auto folded = FoldCompileTimeInt(compiler, expr);
    if (!folded)
    {
        const std::string text = expr != nullptr ? expr->getText() : "";
        errorOut = std::format("simd lane count must be an integer literal (got '{}')", text);
        return false;
    }
    // A value that FOLDED is a lane count, however unusable - 0 and negatives belong in the
    // range diagnostic below, not in the "not an integer literal" one. Signed throughout so a
    // negative cannot wrap into a huge unsigned and skip the hint.
    int64_t v = *folded;
    if (v < 2 || v > 64 || (v & (v - 1)) != 0)
    {
        int64_t up = 2; while (up < v && up < 64) up <<= 1;
        int64_t down = (up > 2) ? (up >> 1) : 2;
        if (down == up)
            errorOut = std::format("simd lane count must be a power of 2 in [2,64] (got {}); did you mean simd<...,{}>?", v, up);
        else
            errorOut = std::format("simd lane count must be a power of 2 in [2,64] (got {}); did you mean simd<...,{}> or simd<...,{}>?", v, down, up);
        return false;
    }
    lanesOut = v;
    return true;
}
void RecordSimdPointerAndDims(LLVMBackend::DeclTypeAndValue& declType,
                                     CFlatParser::DeclarationSpecifierContext* declSpec)
{
    declType.Pointer = declSpec->pointer() != nullptr;
    if (declType.Pointer && declSpec->pointer()->Star().size() >= 2)
        declType.ElemPointer = true;
    // An empty '[]' on a simd type is left alone: simd array views are unimplemented, and
    // deducing one here would change a shape that currently compiles.
    if (auto* dimSpec = ArrayDimsOf(declSpec))
    {
        auto dims = dimSpec->assignmentExpression();
        if (!dims.empty())
        {
            declType.ArraySize = dims[0];
            for (size_t di = 1; di < dims.size(); di++)
                declType.ExtraArrayDims.push_back(dims[di]);
        }
    }
}
std::string getFunctionName(CFlatParser::FunctionDefinitionContext* ctx)
{
    if (auto* opId = ctx->operatorFunctionId())
        return ::getOperatorName(opId);
    auto directDecl = ctx->directDeclarator();
    return directDecl->getText();
}
std::string getFunctionName(CFlatParser::FunctionDefinitionContext* ctx,
                                   const LLVMBackend* compiler)
{
    if (auto* opId = ctx->operatorFunctionId())
        return ::getOperatorName(opId, compiler);
    return getFunctionName(ctx);
}
std::string getFunctionSignatureText(CFlatParser::FunctionDefinitionContext* ctx)
{
    auto* body = ctx->compoundStatement();
    auto* startTok = ctx->getStart();
    if (!body || !startTok) return getFunctionName(ctx);
    auto* input = startTok->getInputStream();
    size_t a = startTok->getStartIndex();
    size_t b = body->getStart()->getStartIndex();
    if (!input || b <= a) return getFunctionName(ctx);

    std::string raw = input->getText(antlr4::misc::Interval(a, b - 1));
    std::string out;
    bool pendingSpace = false;
    for (char c : raw)
    {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { pendingSpace = true; continue; }
        if (pendingSpace && !out.empty()) out += ' ';
        pendingSpace = false;
        out += c;
    }
    return out.empty() ? getFunctionName(ctx) : out;
}
std::string ExtractLeadingDoc(antlr4::BufferedTokenStream* tokens, antlr4::Token* declStart)
{
    if (!tokens || !declStart) return "";
    size_t idx = declStart->getTokenIndex();
    auto hidden = tokens->getHiddenTokensToLeft(idx);
    if (hidden.empty()) return "";

    struct Item { antlr4::Token* tok; int startLine; int endLine; bool isDoc; bool isLine; };
    auto countNewlines = [](const std::string& s) {
        int n = 0;
        for (char c : s) if (c == '\n') ++n;
        return n;
    };
    auto classify = [&](antlr4::Token* t) -> std::optional<Item> {
        const std::string& text = t->getText();
        if (text.size() < 2) return std::nullopt;
        Item it{};
        it.tok = t;
        it.startLine = (int)t->getLine();
        it.endLine = it.startLine + countNewlines(text);
        if (text.starts_with("//"))
        {
            it.isLine = true;
            // /// is a doc comment; //// or longer slash run is not.
            it.isDoc = text.size() >= 3 && text[2] == '/' && (text.size() < 4 || text[3] != '/');
            return it;
        }
        if (text.starts_with("/*"))
        {
            it.isLine = false;
            // /** ... */ is a doc comment; bare /**/ is not.
            it.isDoc = text.size() >= 3 && text[2] == '*' && text != "/**/";
            return it;
        }
        return std::nullopt;
    };

    // Walk hidden tokens right-to-left and collect comments that chain up to the
    // declaration with no blank line gap.
    std::vector<Item> chain;
    int nextExpectedEnd = (int)declStart->getLine() - 1;
    for (auto it = hidden.rbegin(); it != hidden.rend(); ++it)
    {
        auto opt = classify(*it);
        if (!opt) continue;
        Item item = *opt;
        if (item.endLine < nextExpectedEnd) break;     // blank-line gap broke the chain
        if (item.endLine > nextExpectedEnd) continue;  // comment trailing other code on this line; skip
        chain.push_back(item);
        nextExpectedEnd = item.startLine - 1;
    }
    if (chain.empty()) return "";
    std::reverse(chain.begin(), chain.end());

    // Tier 1: keep only the trailing contiguous run of doc comments (closest to decl).
    // Tier 2 (fallback): if no doc comments at all, keep the whole chain as plain.
    bool anyDoc = false;
    for (const auto& it : chain) if (it.isDoc) { anyDoc = true; break; }

    std::vector<Item> selected;
    if (anyDoc)
    {
        size_t first = chain.size();
        for (size_t i = chain.size(); i-- > 0; )
        {
            if (!chain[i].isDoc) break;
            first = i;
        }
        selected.assign(chain.begin() + first, chain.end());
    }
    else
    {
        selected = std::move(chain);
    }

    auto trimRight = [](std::string& s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    };

    std::vector<std::string> outLines;
    for (const auto& it : selected)
    {
        const std::string& text = it.tok->getText();
        if (it.isLine)
        {
            size_t off = it.isDoc ? 3 : 2;  // strip /// or //
            std::string ln = (text.size() > off) ? text.substr(off) : std::string();
            if (!ln.empty() && ln.front() == ' ') ln.erase(0, 1);
            trimRight(ln);
            outLines.push_back(ln);
        }
        else
        {
            // Strip leading /* or /** and trailing */.
            size_t startOff = it.isDoc ? 3 : 2;
            size_t endOff = (text.size() >= 2 && text.compare(text.size() - 2, 2, "*/") == 0) ? 2 : 0;
            if (text.size() <= startOff + endOff) continue;
            std::string body = text.substr(startOff, text.size() - startOff - endOff);
            std::vector<std::string> lines;
            std::string cur;
            for (char c : body)
            {
                if (c == '\n') { lines.push_back(cur); cur.clear(); }
                else cur.push_back(c);
            }
            lines.push_back(cur);
            for (auto& ln : lines)
            {
                size_t j = 0;
                while (j < ln.size() && (ln[j] == ' ' || ln[j] == '\t')) ++j;
                if (j < ln.size() && ln[j] == '*')
                {
                    ++j;
                    if (j < ln.size() && ln[j] == ' ') ++j;
                }
                std::string clean = (j < ln.size()) ? ln.substr(j) : std::string();
                trimRight(clean);
                outLines.push_back(clean);
            }
        }
    }

    // Drop decorative divider lines (e.g. "-----", "=====", box-drawing rules) so a
    // description framed by dividers does not render as an oversized, contentless hover.
    auto isSeparatorLine = [](const std::string& s) {
        int visible = 0, sep = 0;
        for (unsigned char c : s)
        {
            if (c == ' ' || c == '\t') continue;
            ++visible;
            if (std::isalnum(c)) return false;  // any real text -> keep the line
            if (c == '-' || c == '=' || c == '_' || c == '~' || c == '*' || c == '#' || c >= 0x80) ++sep;
        }
        return visible >= 3 && sep >= 3;
    };
    std::erase_if(outLines, [&](const std::string& ln) { return isSeparatorLine(ln); });

    while (!outLines.empty() && outLines.front().empty()) outLines.erase(outLines.begin());
    while (!outLines.empty() && outLines.back().empty()) outLines.pop_back();
    if (outLines.empty()) return "";

    std::string result;
    for (size_t i = 0; i < outLines.size(); ++i)
    {
        if (i > 0) result += '\n';
        result += outLines[i];
    }
    return result;
}
bool isFunctionStatic(CFlatParser::FunctionDefinitionContext* func)
{
    if (!func->declarationSpecifiers()) return false;
    for (auto* ds : func->declarationSpecifiers()->declarationSpecifier())
        if (ds->storageClassSpecifier() && ds->storageClassSpecifier()->Static() != nullptr)
            return true;
    return false;
}
bool AllParametersDefaulted(CFlatParser::ParameterTypeListContext* paramTypeList)
{
    if (paramTypeList == nullptr) return false;
    if (paramTypeList->Ellipsis() != nullptr) return false;
    auto* list = paramTypeList->parameterList();
    if (list == nullptr) return false;
    auto decls = list->parameterDeclaration();
    if (decls.empty()) return false;
    for (auto* d : decls)
        if (d->initializer() == nullptr)
            return false;
    return true;
}
std::string BareSourceText(antlr4::tree::ParseTree* node,
                                  std::vector<std::string>* outWrapperTypes)
{
    while (node != nullptr)
    {
        if (node->children.size() == 1)
        {
            node = node->children[0];
            continue;
        }
        auto* prim = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node);
        // 3 children with a leading '(' is exactly `'(' expression ')'` - nameof/typeof/sizeof
        // spell 4, and a tuple `(a, b)` is its own context reached as a single child.
        if (prim != nullptr && prim->children.size() == 3 && prim->expression() != nullptr
            && prim->children[0]->getText() == "(")
        {
            node = prim->expression();
            continue;
        }
        if (outWrapperTypes != nullptr)
        {
            if (auto* cast = dynamic_cast<CFlatParser::CastExpressionContext*>(node))
                if (cast->castTypeName() != nullptr && cast->castExpression() != nullptr)
                {
                    outWrapperTypes->push_back(cast->castTypeName()->getText());
                    node = cast->castExpression();
                    continue;
                }
            // `x as T` - a single trailing operator, and it must be 'as' (an `is` yields a bool).
            if (auto* tc = dynamic_cast<CFlatParser::TypeCheckExpressionContext*>(node))
                if (tc->typeSpecifier().size() == 1 && tc->children.size() == 3
                    && tc->children[1]->getText() == "as" && tc->relationalExpression() != nullptr)
                {
                    outWrapperTypes->push_back(tc->typeSpecifier(0)->getText());
                    node = tc->relationalExpression();
                    continue;
                }
        }
        break;
    }
    return node == nullptr ? std::string() : node->getText();
}
std::vector<std::string> SplitTopLevelTypeArgs(const std::string& args)
{
    std::vector<std::string> out;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < args.size(); i++)
    {
        if (args[i] == '<') depth++;
        else if (args[i] == '>') depth--;
        else if (args[i] == ',' && depth == 0)
        {
            out.push_back(args.substr(start, i - start));
            start = i + 1;
        }
    }
    out.push_back(args.substr(start));
    return out;
}
std::string CanonicalWrapperTypeName(const LLVMBackend* compiler, const std::string& spelled)
{
    if (compiler == nullptr || spelled.empty()) return spelled;
    if (size_t lt = spelled.find('<');
        lt != std::string::npos && lt > 0 && spelled.back() == '>')
    {
        std::string base = compiler->ResolveTypeAlias(
            compiler->ResolveQualifiedName(spelled.substr(0, lt)));
        std::vector<std::string> args;
        for (const auto& arg : SplitTopLevelTypeArgs(spelled.substr(lt + 1, spelled.size() - lt - 2)))
        {
            if (!arg.empty() && arg.back() == '>' && arg.find('<') != std::string::npos)
                args.push_back(CanonicalWrapperTypeName(compiler, arg));
            else
                args.push_back(MangleTypeArg(compiler, ResolveTypeArgSpelling(compiler, arg)));
        }
        compiler->ResolveGenericAliasSpelling(base, args);
        return MangleGenericInstance(*compiler, base, args);
    }
    return compiler->ResolveManglingAlias(
        compiler->ResolveTypeAlias(compiler->ResolveQualifiedName(spelled)));
}
bool AllWrapperTypesName(const LLVMBackend* compiler,
                                const std::vector<std::string>& wrapperTypes,
                                const std::string& typeName)
{
    if (wrapperTypes.empty() || typeName.empty()) return false;
    const std::string want = CanonicalWrapperTypeName(compiler, typeName);
    for (const auto& t : wrapperTypes)
        if (t != typeName && CanonicalWrapperTypeName(compiler, t) != want) return false;
    return true;
}
void RecordConsumeSourceName(antlr4::tree::ParseTree* node,
                                    std::unordered_set<std::string>& out,
                                    WrappedSourceNames* wrapped)
{
    if (wrapped == nullptr)
    {
        out.insert(BareSourceText(node));
        return;
    }
    std::vector<std::string> wrapperTypes;
    std::string name = BareSourceText(node, &wrapperTypes);
    if (wrapperTypes.empty())
        out.insert(name);
    else
    {
        auto& slot = (*wrapped)[name];
        slot.insert(slot.end(), wrapperTypes.begin(), wrapperTypes.end());
    }
}
bool ComputeReturnsOwned(const LLVMBackend::DeclTypeAndValue& returnType,
                                CFlatParser::FunctionDefinitionContext* func,
                                const std::vector<LLVMBackend::TypeAndValue>& allParams)
{
    if (returnType.TypeName == "string")
    {
        auto* opId = func != nullptr ? func->operatorFunctionId() : nullptr;
        const bool isPlusOperator = opId != nullptr && opId->Plus() != nullptr;
        const bool isStringConversion = opId != nullptr && opId->String() != nullptr;
        const auto isStringValue = [](const LLVMBackend::TypeAndValue& p) {
            return p.TypeName == "string" && !p.Pointer && !p.ElemPointer;
        };
        const auto isCString = [](const LLVMBackend::TypeAndValue& p) {
            return p.TypeName == "char" && p.Pointer && !p.ElemPointer;
        };
        const auto isStringOperand = [&](const LLVMBackend::TypeAndValue& p) {
            return isStringValue(p) || isCString(p);
        };
        const bool coreStringConcat = isPlusOperator && allParams.size() == 2
            && ((isStringValue(allParams[0])
                    && (isStringOperand(allParams[1])
                        || (allParams[1].TypeName == "IString" && !allParams[1].Pointer)))
                || (isCString(allParams[0]) && isStringOperand(allParams[1])));
        if (coreStringConcat)
            return true;
        if (isStringConversion && allParams.size() == 1 && allParams[0].TypeName == "i32")
            return true;
        if (returnType.IsMove)
            return true;
        return false;
    }
    // 'move <interface>' transfers ownership of the boxed heap object to the caller (the
    // caller is then responsible for 'delete'). An interface return type is not Pointer
    // (its LLVM type is a { vtable, data } fat pointer), so it needs its own case.
    if (returnType.IsMove && (returnType.Pointer || returnType.IsInterface))
        return true;
    // NOTE: a 'move S' struct-VALUE return is deliberately NOT returns-owned. The returned
    // aggregate already carries its owning fields' bits, so the receiving local frees them;
    // also registering it as an owned temp at the call site double-frees (e.g. list<T>.copy()).
    return false;
}
bool IsLastUseOfForeignCxxParam(
    LLVMBackend* compiler, antlr4::ParserRuleContext* useCtx,
    const LLVMBackend::NamedVariable& source, bool allowMoveOnlyReturn)
{
    if (compiler == nullptr || useCtx == nullptr || source.FieldName.size() != 0
        || source.TypeAndValue.Pointer || source.TypeAndValue.IsAlias
        || compiler->IsCoreUniqueType(source.TypeAndValue.TypeName)
        || !(compiler->IsForeignNontrivialCxxClass(source.TypeAndValue.TypeName)
             || compiler->HasForeignNontrivialCxxFieldForAnalysis(source.TypeAndValue.TypeName)))
        return false;
    if (compiler->IsForeignNontrivialCxxClass(source.TypeAndValue.TypeName))
    {
        if (const auto* info = compiler->GetCxxClassInfo(source.TypeAndValue.TypeName);
            info == nullptr
            || ((!info->hasCopyCtor || info->hasDeletedCopyCtor)
                && !(allowMoveOnlyReturn
                     && compiler->FindCxxMoveCtor(source.TypeAndValue.TypeName) != nullptr)))
            return false;
    }
    else if (!compiler->IsCopyableTypeForAnalysis(source.TypeAndValue.TypeName)) return false;
    const std::string name = source.CallerName.empty()
        ? source.TypeAndValue.VariableName : source.CallerName;
    if (name.empty() || !compiler->IsFunctionParameter(name) || useCtx->getText() != name)
        return false;
    const auto parameter = compiler->GetFunctionArgument(name);
    if (parameter.Storage == nullptr || source.Storage != parameter.Storage)
        return false;

    CFlatParser::FunctionDefinitionContext* function = nullptr;
    for (antlr4::tree::ParseTree* node = useCtx; node != nullptr; node = node->parent)
    {
        if (dynamic_cast<CFlatParser::LambdaExpressionContext*>(node) != nullptr)
            return false;
        if (auto* fn = dynamic_cast<CFlatParser::FunctionDefinitionContext*>(node))
        {
            function = fn;
            break;
        }
    }
    antlr4::tree::ParseTree* body = function != nullptr
        ? static_cast<antlr4::tree::ParseTree*>(function->compoundStatement()) : nullptr;
    if (body == nullptr) return false;

    struct Occurrence
    {
        size_t token = 0;
        bool inLoop = false;
        bool inNestedLambda = false;
    };
    std::vector<Occurrence> occurrences;
    // Declaration contexts supply lexical slot identity for later uses not visited yet.
    auto isShadowedByLocal = [&](CFlatParser::PrimaryExpressionContext* primary) {
        const size_t token = (size_t)primary->getStart()->getTokenIndex();
        for (auto* parent = primary->parent; parent != nullptr && parent != body;
             parent = parent->parent)
        {
            auto* block = dynamic_cast<CFlatParser::CompoundStatementContext*>(parent);
            if (block == nullptr || block->blockItemList() == nullptr) continue;
            for (auto* item : block->blockItemList()->blockItem())
            {
                if (item->getStart() == nullptr
                    || (size_t)item->getStart()->getTokenIndex() >= token)
                    continue;
                if (auto* declaration = item->declaration())
                {
                    if (declaration->initDeclaratorList() == nullptr) continue;
                    for (auto* init : declaration->initDeclaratorList()->initDeclarator())
                    {
                        auto* declarator = init->declarator();
                        auto* direct = declarator != nullptr ? declarator->directDeclarator() : nullptr;
                        if (direct != nullptr && direct->Identifier() != nullptr
                            && direct->Identifier()->getText() == name)
                            return true;
                    }
                }
                if (auto* destructuring = item->destructuringDeclaration())
                    for (auto* entry : destructuring->destructuringEntry())
                        if (entry->Identifier() != nullptr
                            && entry->Identifier()->getText() == name)
                            return true;
            }
        }
        return false;
    };
    auto collect = [&](auto&& self, antlr4::tree::ParseTree* node) -> void {
        if (node == nullptr) return;
        if (auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node))
        {
            if (primary->genericIdentifier() != nullptr && primary->getText() == name)
            {
                if (isShadowedByLocal(primary)) return;
                bool inLoop = false;
                bool inNestedLambda = false;
                for (auto* parent = primary->parent; parent != nullptr && parent != body;
                     parent = parent->parent)
                {
                    if (dynamic_cast<CFlatParser::IterationStatementContext*>(parent) != nullptr)
                        inLoop = true;
                    if (dynamic_cast<CFlatParser::LambdaExpressionContext*>(parent) != nullptr)
                        inNestedLambda = true;
                }
                occurrences.push_back({ (size_t)primary->getStart()->getTokenIndex(),
                                        inLoop, inNestedLambda });
            }
            return;
        }
        for (auto* child : node->children) self(self, child);
    };
    collect(collect, body);

    const size_t useToken = (size_t)useCtx->getStart()->getTokenIndex();
    bool found = false;
    for (const auto& occurrence : occurrences)
    {
        if (occurrence.token == useToken && !found)
        {
            found = true;
            if (occurrence.inLoop || occurrence.inNestedLambda) return false;
            continue;
        }
        if (occurrence.inNestedLambda) return false;
        if (occurrence.token > useToken) return false;
    }
    return found;
}
void CollectOwnReturnExpressions(antlr4::tree::ParseTree* node,
                                        std::vector<CFlatParser::ExpressionContext*>& out,
                                        const IfConstEvaluator& evalIfConst)
{
    if (node == nullptr) return;
    if (AsRuleCtx<CFlatParser::LambdaExpressionContext>(node) != nullptr) return;
    if (auto* sel = AsRuleCtx<CFlatParser::SelectionStatementContext>(node);
        sel != nullptr && sel->If() != nullptr && sel->Const() != nullptr && evalIfConst)
    {
        const int decision = evalIfConst(sel->expression());
        if (decision >= 0)
        {
            auto branches = sel->statement();
            const size_t index = decision == 1 ? 0 : 1;
            if (index < branches.size())
                CollectOwnReturnExpressions(branches[index], out, evalIfConst);
            return;
        }
    }
    if (auto* jump = AsRuleCtx<CFlatParser::JumpStatementContext>(node))
        if (jump->expression() != nullptr) out.push_back(jump->expression());
    for (auto* child : node->children)
        CollectOwnReturnExpressions(child, out, evalIfConst);
}
void CollectOwnReturnStatements(antlr4::tree::ParseTree* node,
                                       std::vector<CFlatParser::JumpStatementContext*>& out)
{
    if (node == nullptr) return;
    if (AsRuleCtx<CFlatParser::LambdaExpressionContext>(node) != nullptr) return;
    if (auto* jump = AsRuleCtx<CFlatParser::JumpStatementContext>(node))
        if (jump->Return() != nullptr) out.push_back(jump);
    for (auto* child : node->children)
        CollectOwnReturnStatements(child, out);
}
bool HasNonAutomaticStorageClass(CFlatParser::DeclarationSpecifiersContext* declSpecs)
{
    if (declSpecs == nullptr) return false;
    for (auto* spec : declSpecs->declarationSpecifier())
        if (auto* storage = spec->storageClassSpecifier(); storage != nullptr)
            if (storage->Static() != nullptr || storage->ThreadLocal() != nullptr
                || storage->Extern() != nullptr)
                return true;
    return false;
}
bool IsBareReturnIdentifier(const std::string& text)
{
    if (text.empty()) return false;
    auto initial = [](unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; };
    auto subsequent = [&](unsigned char c) { return initial(c) || (c >= '0' && c <= '9'); };
    if (!initial(static_cast<unsigned char>(text.front()))) return false;
    return std::all_of(text.begin() + 1, text.end(), [&](char c) {
        return subsequent(static_cast<unsigned char>(c));
    });
}
void NamedLocalReturnVisit(antlr4::tree::ParseTree* node, NamedLocalReturnScan& scan)
{
    if (node == nullptr) return;
    if (AsRuleCtx<CFlatParser::LambdaExpressionContext>(node) != nullptr) return;
    auto visitChildren = [&]() {
        for (auto* child : node->children) NamedLocalReturnVisit(child, scan);
    };
    if (dynamic_cast<CFlatParser::CompoundStatementContext*>(node) != nullptr)
    {
        scan.scopes.emplace_back();
        visitChildren();
        scan.scopes.pop_back();
        return;
    }
    if (auto* loop = AsRuleCtx<CFlatParser::IterationStatementContext>(node))
    {
        scan.scopes.emplace_back();
        if (loop->In() != nullptr && loop->Identifier() != nullptr)
            scan.Bind(loop->Identifier()->getText(), nullptr);
        visitChildren();
        scan.scopes.pop_back();
        return;
    }
    if (auto* labeled = AsRuleCtx<CFlatParser::LabeledStatementContext>(node))
    {
        // A pattern arm binds its identifier; a plain label (starts with the identifier) does not.
        scan.scopes.emplace_back();
        if (labeled->Identifier() != nullptr
            && labeled->getStart()->getType() != CFlatParser::Identifier)
            scan.Bind(labeled->Identifier()->getText(), nullptr);
        visitChildren();
        scan.scopes.pop_back();
        return;
    }
    if (auto* destructuring = dynamic_cast<CFlatParser::DestructuringDeclarationContext*>(node))
    {
        for (auto* entry : destructuring->destructuringEntry())
            if (entry->Identifier() != nullptr) scan.Bind(entry->Identifier()->getText(), nullptr);
        return;
    }
    if (auto* forDecl = dynamic_cast<CFlatParser::ForDeclarationContext*>(node))
    {
        if (forDecl->initDeclaratorList() != nullptr)
            for (auto* init : forDecl->initDeclaratorList()->initDeclarator())
                if (init->declarator() != nullptr)
                    scan.Bind(getDirectDeclName(init->declarator()->directDeclarator()), nullptr);
        return;
    }
    if (auto* declaration = dynamic_cast<CFlatParser::DeclarationContext*>(node))
    {
        const bool automatic = !HasNonAutomaticStorageClass(declaration->declarationSpecifiers());
        if (declaration->initDeclaratorList() != nullptr)
            for (auto* init : declaration->initDeclaratorList()->initDeclarator())
                if (init->declarator() != nullptr && init->declarator()->directDeclarator() != nullptr)
                    scan.Bind(getDirectDeclName(init->declarator()->directDeclarator()),
                              automatic ? init->declarator()->directDeclarator() : nullptr);
        return;
    }
    if (auto* jump = AsRuleCtx<CFlatParser::JumpStatementContext>(node))
    {
        if (jump->Return() == nullptr) { visitChildren(); return; }
        // Only a bare identifier (no cast or wrapper) names a local; anything else names none.
        size_t named = NamedLocalReturnScan::kNoCandidate;
        if (jump->expression() != nullptr && jump->compoundStatement() == nullptr)
        {
            std::vector<std::string> wrapperTypes;
            const std::string returned = BareSourceText(jump->expression(), &wrapperTypes);
            if (wrapperTypes.empty() && IsBareReturnIdentifier(returned))
                named = scan.Resolve(returned);
        }
        scan.Return(named);
        return;
    }
    visitChildren();
}
std::vector<CFlatParser::DirectDeclaratorContext*> ClassifyNamedLocalReturnCandidates(
    CFlatParser::FunctionDefinitionContext* func)
{
    std::vector<CFlatParser::DirectDeclaratorContext*> out;
    if (func == nullptr || func->compoundStatement() == nullptr
        || func->compoundStatement()->blockItemList() == nullptr)
        return out;

    NamedLocalReturnScan scan;
    scan.scopes.emplace_back();
    if (func->parameterTypeList() != nullptr && func->parameterTypeList()->parameterList() != nullptr)
        for (auto* param : func->parameterTypeList()->parameterList()->parameterDeclaration())
            if (param->declarator() != nullptr && param->declarator()->directDeclarator() != nullptr)
                scan.Bind(getDirectDeclName(param->declarator()->directDeclarator()), nullptr);
    NamedLocalReturnVisit(func->compoundStatement(), scan);
    for (const auto& candidate : scan.candidates)
        if (candidate.named && !candidate.killed) out.push_back(candidate.declarator);
    return out;
}
ValueStructReturnKind ClassifyValueStructReturns(
    const LLVMBackend* compiler,
    CFlatParser::FunctionDefinitionContext* func,
    const LLVMBackend::DeclTypeAndValue& returnType,
    const std::vector<LLVMBackend::TypeAndValue>& allParams,
    std::string* outBorrowedParamName)
{
    if (func == nullptr || func->compoundStatement() == nullptr) return ValueStructReturnKind::NotApplicable;
    if (returnType.TypeName.empty() || returnType.Pointer || returnType.ElemPointer) return ValueStructReturnKind::NotApplicable;
    if (returnType.IsMove || returnType.IsAlias) return ValueStructReturnKind::NotApplicable;

    std::vector<CFlatParser::ExpressionContext*> returns;
    CollectOwnReturnExpressions(func->compoundStatement(), returns);
    if (returns.empty()) return ValueStructReturnKind::NotApplicable;

    int borrowed = 0;
    int other = 0;
    for (auto* expr : returns)
    {
        // `return (p);` / `return (T)p;` hand back the same borrow `return p;` does. Peel the
        // wrappers; a peeled CAST counts only when it names the return type itself (below).
        std::vector<std::string> wrapperTypes;
        const std::string text = BareSourceText(expr, &wrapperTypes);
        if (!wrapperTypes.empty() && !AllWrapperTypesName(compiler, wrapperTypes, returnType.TypeName))
        {
            ++other;
            continue;
        }
        bool isBorrowedParam = false;
        for (const auto& p : allParams)
        {
            if (p.VariableName != text) continue;
            if (p.TypeName != returnType.TypeName) continue;
            if (p.Pointer || p.ElemPointer || p.IsMove || p.IsAlias) continue;
            // A param the body also consumes (`w += k; return w;`) is owned by the callee, so its
            // return hands out that value; only a return-only sink is still a borrow here.
            if (p.IsOwningSink && (!p.IsReturnInferredSink || p.IsWriteInferredSink)) continue;
            isBorrowedParam = true;
            if (outBorrowedParamName && outBorrowedParamName->empty()) *outBorrowedParamName = p.VariableName;
            break;
        }
        if (isBorrowedParam) ++borrowed; else ++other;
    }
    if (borrowed == 0) return ValueStructReturnKind::NotApplicable;
    return other == 0 ? ValueStructReturnKind::AllBorrowedParam : ValueStructReturnKind::Mixed;
}
bool IsDeclScopeNode(antlr4::tree::ParseTree* node)
{
    return AsRuleCtx<CFlatParser::CompoundStatementContext>(node) != nullptr
        || AsRuleCtx<CFlatParser::IterationStatementContext>(node) != nullptr
        || AsRuleCtx<CFlatParser::SelectionStatementContext>(node) != nullptr
        || AsRuleCtx<CFlatParser::FunctionDefinitionContext>(node) != nullptr
        || AsRuleCtx<CFlatParser::LambdaExpressionContext>(node) != nullptr
        || AsRuleCtx<CFlatParser::ParameterDeclarationContext>(node) != nullptr
        || AsRuleCtx<CFlatParser::StructDefinitionContext>(node) != nullptr
        || AsRuleCtx<CFlatParser::ClassDefinitionContext>(node) != nullptr;
}
void CollectFieldNames(antlr4::tree::ParseTree* node, std::unordered_set<std::string>& out)
{
    if (node == nullptr) return;
    if (auto* d = dynamic_cast<CFlatParser::DirectDeclaratorContext*>(node))
        out.insert(getDirectDeclName(d));
    for (auto* child : node->children)
        if (!IsDeclScopeNode(child)) CollectFieldNames(child, out);
}
void CollectVisibleLocals(antlr4::tree::ParseTree* node,
                                 const std::unordered_set<antlr4::tree::ParseTree*>& enclosing,
                                 size_t usePos, std::unordered_set<std::string>& out)
{
    if (node == nullptr) return;
    if (auto* d = dynamic_cast<CFlatParser::DirectDeclaratorContext*>(node))
    {
        bool visible = d->getStart()->getTokenIndex() < usePos;
        for (auto* p = d->parent; visible && p != nullptr; p = p->parent)
            if (IsDeclScopeNode(p) && enclosing.count(p) == 0) visible = false;
        if (visible) out.insert(getDirectDeclName(d));
    }
    if (auto* loop = AsRuleCtx<CFlatParser::IterationStatementContext>(node))
        if (loop->Identifier() != nullptr && enclosing.count(loop) != 0)
            out.insert(loop->Identifier()->getText());
    for (auto* child : node->children)
        CollectVisibleLocals(child, enclosing, usePos, out);
}
bool IsIdentifierText(const std::string& s)
{
    return !s.empty() && (std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')
        && std::all_of(s.begin(), s.end(), [](char ch) {
            return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'; });
}
std::string DottedPathHead(const std::string& text)
{
    std::string head;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i)
    {
        if (i < text.size() && text[i] != '.') continue;
        const std::string segment = text.substr(start, i - start);
        if (!IsIdentifierText(segment)) return {};
        if (start == 0) head = segment;
        start = i + 1;
    }
    return head;
}
bool ReturnsOnlyModuleGlobals(CFlatParser::FunctionDefinitionContext* func,
                                     const std::vector<LLVMBackend::TypeAndValue>& allParams,
                                     const IfConstEvaluator& evalIfConst)
{
    if (func == nullptr || func->compoundStatement() == nullptr) return false;
    std::vector<CFlatParser::ExpressionContext*> returns;
    CollectOwnReturnExpressions(func->compoundStatement(), returns, evalIfConst);
    if (returns.empty()) return false;

    antlr4::tree::ParseTree* structNode = nullptr;
    for (antlr4::tree::ParseTree* p = func->parent; p != nullptr; p = p->parent)
    {
        if (auto* s = dynamic_cast<CFlatParser::StructDefinitionContext*>(p))
        {
            if (!s->baseSpecifier().empty()) return false;
            structNode = s;
            break;
        }
        if (auto* c = dynamic_cast<CFlatParser::ClassDefinitionContext*>(p))
        {
            if (!c->baseSpecifier().empty()) return false;
            structNode = c;
            break;
        }
    }
    std::unordered_set<std::string> bound;
    if (structNode != nullptr) CollectFieldNames(structNode, bound);
    for (const auto& p : allParams)
        bound.insert(p.VariableName);
    for (auto* expr : returns)
    {
        const std::string head = DottedPathHead(BareSourceText(expr));
        if (head.empty() || head == "nullptr" || bound.count(head) != 0) return false;
        std::unordered_set<antlr4::tree::ParseTree*> enclosing;
        for (antlr4::tree::ParseTree* p = expr; p != nullptr; p = p->parent)
            enclosing.insert(p);
        std::unordered_set<std::string> locals;
        CollectVisibleLocals(func, enclosing, expr->getStart()->getTokenIndex(), locals);
        if (locals.count(head) != 0) return false;
    }
    return true;
}
std::string DirectReturnIdentifier(CFlatParser::ExpressionContext* expr)
{
    antlr4::tree::ParseTree* node = expr;
    while (node != nullptr)
    {
        if (auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node))
        {
            if (primary->genericIdentifier() != nullptr && node->children.size() == 1
                && primary->genericIdentifier()->genericTypeParameters() == nullptr)
                return primary->genericIdentifier()->Identifier()->getText();
            if (primary->expression() != nullptr)
            {
                node = primary->expression();
                continue;
            }
            return {};
        }
        if (auto* cast = dynamic_cast<CFlatParser::CastExpressionContext*>(node);
            cast != nullptr && cast->castExpression() != nullptr)
        {
            node = cast->castExpression();
            continue;
        }
        if (node->children.size() != 1) return {};
        node = node->children[0];
    }
    return {};
}
int ReturnedPointerParameterIndex(
    CFlatParser::FunctionDefinitionContext* func,
    CFlatParser::ExpressionContext* expr,
    const std::vector<LLVMBackend::TypeAndValue>& allParams)
{
    if (func == nullptr || expr == nullptr) return -1;
    const std::string name = DirectReturnIdentifier(expr);
    if (name.empty()) return -1;
    std::unordered_set<antlr4::tree::ParseTree*> enclosing;
    for (antlr4::tree::ParseTree* p = expr; p != nullptr; p = p->parent)
        enclosing.insert(p);
    std::unordered_set<std::string> locals;
    CollectVisibleLocals(func, enclosing, expr->getStart()->getTokenIndex(), locals);
    if (locals.count(name) != 0) return -1;
    for (size_t i = 0; i < allParams.size(); ++i)
        if (allParams[i].VariableName == name && allParams[i].Pointer
            && !allParams[i].IsMove && !allParams[i].IsAlias)
            return static_cast<int>(i);
    return -1;
}
bool ReturnsParameterBorrow(CFlatParser::FunctionDefinitionContext* func,
                                   const std::vector<LLVMBackend::TypeAndValue>& allParams,
                                   const IfConstEvaluator& evalIfConst)
{
    if (func == nullptr || func->compoundStatement() == nullptr) return false;
    std::vector<CFlatParser::ExpressionContext*> returns;
    CollectOwnReturnExpressions(func->compoundStatement(), returns, evalIfConst);
    for (auto* expr : returns)
        if (ReturnedPointerParameterIndex(func, expr, allParams) >= 0) return true;
    return false;
}
int ReturnsSingleParameterIndex(
    CFlatParser::FunctionDefinitionContext* func,
    const std::vector<LLVMBackend::TypeAndValue>& allParams,
    const IfConstEvaluator& evalIfConst)
{
    if (func == nullptr || func->compoundStatement() == nullptr) return -1;
    std::vector<CFlatParser::ExpressionContext*> returns;
    CollectOwnReturnExpressions(func->compoundStatement(), returns, evalIfConst);
    if (returns.empty()) return -1;
    const int index = ReturnedPointerParameterIndex(func, returns.front(), allParams);
    if (index < 0) return -1;
    for (size_t i = 1; i < returns.size(); ++i)
        if (ReturnedPointerParameterIndex(func, returns[i], allParams) != index)
            return -1;
    return index;
}
void SetPointerBorrowReturnSummaries(
    std::vector<LLVMBackend::FunctionSymbol>& overloads,
    const std::vector<LLVMBackend::TypeAndValue>& allParams,
    bool returnsGlobal, bool returnsParameter, int singleParameterIndex)
{
    for (auto& sym : overloads)
        if (sym.ReturnType.Pointer && sym.Parameters.size() == allParams.size()
            && std::equal(allParams.begin(), allParams.end(), sym.Parameters.begin(),
                [](const auto& a, const auto& b) { return a.TypeName == b.TypeName && a.Pointer == b.Pointer; }))
        {
            sym.ReturnsGlobalBorrow = returnsGlobal;
            sym.ReturnsParameterBorrow = returnsParameter;
            sym.ReturnsSingleParameterIndex = singleParameterIndex;
        }
}
bool UniquePointerSourceIsOwned(const LLVMBackend::NamedVariable& source)
{
    return source.IsOwning || source.IsNewAllocated;
}
std::string ArrowNormalizeLockText(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); )
    {
        if (text[i] == '-' && i + 1 < text.size() && text[i + 1] == '>')
        {
            out += '.';
            i += 2;
        }
        else
        {
            out += text[i];
            i++;
        }
    }
    return out;
}
std::string NormalizeLockText(const std::string& text)
{
    std::string out = ArrowNormalizeLockText(text);
    if (out.ends_with(".read"))
        out.resize(out.size() - 5);
    else if (out.ends_with(".write"))
        out.resize(out.size() - 6);
    else if (out.ends_with(".optimistic"))
        out.resize(out.size() - 11);
    return out;
}
bool IsSimpleLockPath(const std::string& text)
{
    if (text.empty()) return false;
    unsigned char first = (unsigned char)text[0];
    if (!(std::isalpha(first) || first == '_')) return false;
    for (char c : text)
    {
        unsigned char u = (unsigned char)c;
        if (!(std::isalnum(u) || c == '_' || c == '.' || c == '[' || c == ']')) return false;
    }
    return true;
}
std::string LockPathForOperandText(const std::string& operandText)
{
    std::string text = operandText;
    if (!text.empty() && text.front() == '&') text.erase(0, 1);
    text = ArrowNormalizeLockText(text);
    return IsSimpleLockPath(text) ? text : std::string();
}
std::string GuardKeyForPath(const std::string& receiverPath, const std::string& guard)
{
    if (receiverPath.empty() || receiverPath == "this") return guard;
    return receiverPath + "." + guard;
}
std::string LockTextMode(const std::string& text)
{
    // Arrow-normalize first so 'p->rw.read' (and 'rw->read') still detect the mode suffix.
    std::string dotted = ArrowNormalizeLockText(text);
    if (dotted.ends_with(".read"))
        return "read";
    if (dotted.ends_with(".write"))
        return "write";
    if (dotted.ends_with(".optimistic"))
        return "optimistic";
    return "";
}
LockMode LockModeFromSuffix(const std::string& mode)
{
    if (mode == "read")       return LockMode::Shared;
    if (mode == "optimistic") return LockMode::Optimistic;
    return LockMode::Exclusive;
}
const char* CapabilityForLockMode(const std::string& mode)
{
    if (mode == "read")       return "ISharedLockable";
    if (mode == "optimistic") return "IOptimisticLockable";
    return "ILockable";
}
bool SubtreeContainsFunctionReturn(antlr4::tree::ParseTree* node, ReturnScanMemo* memo)
{
    if (node == nullptr) return false;
    if (memo != nullptr)
    {
        auto it = memo->find(node);
        if (it != memo->end()) return it->second;
    }
    if (AsRuleCtx<CFlatParser::LambdaExpressionContext>(node))
    {
        if (memo != nullptr) (*memo)[node] = false;
        return false;
    }
    if (auto* jump = AsRuleCtx<CFlatParser::JumpStatementContext>(node))
        if (jump->Return() != nullptr)
        {
            if (memo != nullptr) (*memo)[node] = true;
            return true;
        }
    for (auto* child : node->children)
        if (SubtreeContainsFunctionReturn(child, memo))
        {
            if (memo != nullptr) (*memo)[node] = true;
            return true;
        }
    if (memo != nullptr) (*memo)[node] = false;
    return false;
}
bool TryGetKnownIfConstBranch(CFlatParser::SelectionStatementContext* sel,
                                     const IfConstEvaluator& evalIfConst,
                                     antlr4::tree::ParseTree*& liveBranch)
{
    liveBranch = nullptr;
    if (sel == nullptr || !evalIfConst || !sel->If() || !sel->Const()) return false;
    const int taken = evalIfConst(sel->expression());
    if (taken < 0) return false;
    auto inner = sel->statement();
    if (taken == 1 && inner.size() > 0) liveBranch = inner[0];
    else if (taken == 0 && inner.size() > 1) liveBranch = inner[1];
    return true;
}
void CollectUnconditionalMovedNames(antlr4::tree::ParseTree* node, std::unordered_set<std::string>& out,
                                           const IfConstEvaluator& evalIfConst,
                                           ReturnScanMemo* returnScanMemo)
{
    if (node == nullptr) return;
    if (auto* sel = AsRuleCtx<CFlatParser::SelectionStatementContext>(node))
    {
        antlr4::tree::ParseTree* liveBranch = nullptr;
        if (TryGetKnownIfConstBranch(sel, evalIfConst, liveBranch) && liveBranch != nullptr)
            CollectUnconditionalMovedNames(liveBranch, out, evalIfConst, returnScanMemo);
        // A runtime `if`/`switch` (or an undecidable if-const) is conditional - stop descending.
        return;
    }
    // A move reached only through one of these executes conditionally (or in a captured
    // scope), so it cannot make the parameter a sink - stop descending here.
    if (AsRuleCtx<CFlatParser::IterationStatementContext>(node)
        || AsRuleCtx<CFlatParser::LabeledStatementContext>(node)
        || AsRuleCtx<CFlatParser::LambdaExpressionContext>(node))
        return;
    // A ternary `a ? b : c` (or `a ?? b`) makes its branches conditional.
    if (auto* cond = AsRuleCtx<CFlatParser::ConditionalExpressionContext>(node))
        if (cond->children.size() > 1) return;
    if (auto* mv = AsRuleCtx<CFlatParser::MoveExpressionContext>(node))
        if (auto* u = mv->unaryExpression())
            out.insert(BareSourceText(u));
    for (auto* child : node->children)
    {
        CollectUnconditionalMovedNames(child, out, evalIfConst, returnScanMemo);
        // A statement that can return from the function makes every LATER sibling conditional,
        // so a `move` after an early exit (e.g. `if (b) return; g = move v;`) is not a sink -
        // consuming the caller there would leak on the not-moved path.
        if (SubtreeContainsFunctionReturn(child, returnScanMemo)) break;
    }
}
void CollectPositionalBraceElementNames(CFlatParser::InitializerListContext* list,
                                               std::unordered_set<std::string>& out,
                                               WrappedSourceNames* wrapped)
{
    if (list == nullptr) return;
    for (auto* fi : list->fieldInit())
        if (fi != nullptr && fi->Identifier() == nullptr && fi->assignmentExpression().size() == 1)
            RecordConsumeSourceName(fi->assignmentExpression(0), out, wrapped);
}
bool NameShadowedByEnclosingLocal(antlr4::tree::ParseTree* use, const std::string& name,
        const CFlatParser::LambdaExpressionContext* functionLambda,
        const antlr4::tree::ParseTree* stopAt)
{
    auto* useCtx = dynamic_cast<antlr4::ParserRuleContext*>(use);
    if (useCtx == nullptr || useCtx->getStart() == nullptr || name.empty()) return false;
    const size_t token = (size_t)useCtx->getStart()->getTokenIndex();
    auto declares = [&](CFlatParser::InitDeclaratorListContext* list) {
        if (list == nullptr) return false;
        for (auto* init : list->initDeclarator())
        {
            auto* declarator = init->declarator();
            auto* direct = declarator != nullptr ? declarator->directDeclarator() : nullptr;
            if (direct != nullptr && direct->Identifier() != nullptr
                && direct->Identifier()->getText() == name)
                return true;
        }
        return false;
    };
    for (auto* parent = use->parent; parent != nullptr && parent != stopAt; parent = parent->parent)
    {
        if (auto* lambda = dynamic_cast<CFlatParser::LambdaExpressionContext*>(parent))
        {
            if (lambda == functionLambda) break;
            auto* params = lambda->lambdaParamList();
            if (params != nullptr)
                for (auto* param : params->lambdaParam())
                    if (param->Identifier() != nullptr && param->Identifier()->getText() == name)
                        return true;
            continue;
        }
        if (dynamic_cast<CFlatParser::FunctionDefinitionContext*>(parent) != nullptr)
            break;
        if (auto* loop = dynamic_cast<CFlatParser::IterationStatementContext*>(parent))
        {
            if (loop->Identifier() != nullptr && loop->Identifier()->getText() == name)
                return true;
            if (auto* cond = loop->forCondition())
                if (cond->forDeclaration() != nullptr
                    && declares(cond->forDeclaration()->initDeclaratorList()))
                    return true;
            continue;
        }
        auto* block = dynamic_cast<CFlatParser::CompoundStatementContext*>(parent);
        if (block == nullptr || block->blockItemList() == nullptr) continue;
        for (auto* item : block->blockItemList()->blockItem())
        {
            if (item->getStart() == nullptr || (size_t)item->getStart()->getTokenIndex() >= token)
                continue;
            if (auto* declaration = item->declaration())
                if (declares(declaration->initDeclaratorList()))
                    return true;
            if (auto* destructuring = item->destructuringDeclaration())
                for (auto* entry : destructuring->destructuringEntry())
                    if (entry->Identifier() != nullptr && entry->Identifier()->getText() == name)
                        return true;
        }
    }
    return false;
}
void CollectConsumedStoreNames(antlr4::tree::ParseTree* node, std::unordered_set<std::string>& out,
                                      WrappedSourceNames* wrapped,
                                      std::unordered_set<std::string>* returned,
                                      WrappedSourceNames* returnedWrapped,
                                      std::unordered_set<std::string>* written,
                                      const CFlatParser::LambdaExpressionContext* functionLambda,
                                      std::unordered_set<std::string>* explicitMoves,
                                      const IfConstEvaluator& evalIfConst)
{
    if (node == nullptr) return;
    if (AsRuleCtx<CFlatParser::LambdaExpressionContext>(node)) return;
    if (AsRuleCtx<CFlatParser::FunctionDefinitionContext>(node)) return;
    if (auto* sel = AsRuleCtx<CFlatParser::SelectionStatementContext>(node))
    {
        antlr4::tree::ParseTree* liveBranch = nullptr;
        if (TryGetKnownIfConstBranch(sel, evalIfConst, liveBranch))
        {
            if (liveBranch != nullptr)
                CollectConsumedStoreNames(liveBranch, out, wrapped, returned, returnedWrapped, written,
                                          functionLambda, explicitMoves, evalIfConst);
            return;
        }
    }
    if (auto* asn = AsRuleCtx<CFlatParser::AssignmentExpressionContext>(node))
        if (asn->assignmentOperator() != nullptr && asn->assignmentExpression() != nullptr)
        {
            if (asn->assignmentOperator()->getText() == "=")
                RecordConsumeSourceName(asn->assignmentExpression(), out, wrapped);
            // A store INTO the whole name (`w = v`, `w += k`) drops the value it held, so the body
            // takes that value over exactly as `T t = w; t = v;` does. A shadowing local is not w.
            if (asn->unaryExpression() != nullptr)
            {
                std::string dest = BareSourceText(asn->unaryExpression());
                if (!NameShadowedByEnclosingLocal(asn, dest, functionLambda))
                    (written != nullptr ? *written : out).insert(dest);
            }
        }
    // `return w;` hands the whole value to the caller - the same consume as `T t = w; return t;`.
    // Kept apart when asked: a unique-owning passthrough is an inferred 'alias' return instead.
    if (auto* jump = AsRuleCtx<CFlatParser::JumpStatementContext>(node))
        if (jump->expression() != nullptr
            && !NameShadowedByEnclosingLocal(jump, BareSourceText(jump->expression()), functionLambda))
            RecordConsumeSourceName(jump->expression(), returned != nullptr ? *returned : out,
                                    returned != nullptr ? returnedWrapped : wrapped);
    // `construct_at(slot, value)` is the explicit raw-slot spelling of the same native
    // owning sink as `_data[i] = value`; its second argument must participate in inference.
    if (auto* call = dynamic_cast<CFlatParser::PostfixExpressionContext*>(node))
        if (call->primaryExpression() != nullptr
            && call->primaryExpression()->getText() == "construct_at"
            && call->argumentExpressionList().size() == 1)
        {
            auto args = call->argumentExpressionList(0)->argumentNamedExpression();
            if (args.size() == 2 && args[1]->assignmentExpression() != nullptr)
                RecordConsumeSourceName(args[1]->assignmentExpression(), out, wrapped);
        }
    if (auto* init = AsRuleCtx<CFlatParser::InitDeclaratorContext>(node))
    {
        if (auto* iz = init->initializer(); iz != nullptr)
        {
            if (iz->assignmentExpression() != nullptr)
                RecordConsumeSourceName(iz->assignmentExpression(), out, wrapped);
            CollectPositionalBraceElementNames(iz->initializerList(), out, wrapped);
        }
        // `T[N] d { p };` - the brace-ctor alternative hangs the list off the declarator itself.
        CollectPositionalBraceElementNames(init->initializerList(), out, wrapped);
    }
    if (auto* moved = AsRuleCtx<CFlatParser::MoveExpressionContext>(node))
        if (auto* source = moved->unaryExpression())
        {
            std::string name = BareSourceText(source);
            if (!NameShadowedByEnclosingLocal(moved, name, functionLambda))
            {
                out.insert(name);
                if (explicitMoves != nullptr) explicitMoves->insert(name);
            }
        }
    for (auto* child : node->children)
        CollectConsumedStoreNames(child, out, wrapped, returned, returnedWrapped, written,
                                  functionLambda, explicitMoves, evalIfConst);
}
void CollectWholeNameStoreDestinations(antlr4::tree::ParseTree* node,
                                              std::unordered_set<std::string>& out,
                                              bool skipLambdas,
                                              const CFlatParser::LambdaExpressionContext* functionLambda)
{
    if (node == nullptr) return;
    if (AsRuleCtx<CFlatParser::FunctionDefinitionContext>(node)) return;
    if (skipLambdas && AsRuleCtx<CFlatParser::LambdaExpressionContext>(node)) return;
    if (auto* asn = AsRuleCtx<CFlatParser::AssignmentExpressionContext>(node))
        if (asn->assignmentOperator() != nullptr && asn->unaryExpression() != nullptr)
        {
            std::string dest = BareSourceText(asn->unaryExpression());
            if (!NameShadowedByEnclosingLocal(asn, dest, functionLambda))
                out.insert(dest);
        }
    for (auto* child : node->children)
        CollectWholeNameStoreDestinations(child, out, skipLambdas, functionLambda);
}
char FirstSourceChar(antlr4::ParserRuleContext* ctx)
{
    if (ctx == nullptr || ctx->getStart() == nullptr) return '\0';
    const std::string first = ctx->getStart()->getText();
    return first.empty() ? '\0' : first[0];
}
void CollectAddressAliasWholeNameStores(
        antlr4::tree::ParseTree* body,
        const std::vector<LLVMBackend::TypeAndValue>& params,
        std::unordered_set<std::string>& writtenNames,
        bool addressMayWrite,
        const CFlatParser::LambdaExpressionContext* functionLambda)
{
    if (std::none_of(params.begin(), params.end(), [](const auto& p) { return !p.VariableName.empty(); }))
        return;
    struct AddressedParam { std::string name; size_t token; };
    std::unordered_map<std::string, AddressedParam> pointerRoots;
    auto collectAddressAliases = [&](auto&& self, antlr4::tree::ParseTree* node) -> void {
        if (node == nullptr) return;
        if (auto* init = dynamic_cast<CFlatParser::InitDeclaratorContext*>(node))
        {
            auto* direct = init->declarator() != nullptr ? init->declarator()->directDeclarator() : nullptr;
            auto* value = init->initializer() != nullptr ? init->initializer()->assignmentExpression() : nullptr;
            if (direct != nullptr && direct->Identifier() != nullptr && value != nullptr
                && FirstSourceChar(value) == '&')
            {
                const std::string source = value->getText();
                if (source.size() > 1 && source[0] == '&')
                {
                    const std::string target = source.substr(1);
                    for (const auto& p : params)
                        if (p.VariableName == target
                            && !NameShadowedByEnclosingLocal(init, target, functionLambda))
                            pointerRoots[direct->Identifier()->getText()] = {
                                target, (size_t)init->getStart()->getTokenIndex() };
                }
            }
        }
        if (auto* asn = AsRuleCtx<CFlatParser::AssignmentExpressionContext>(node))
        {
            auto* dest = asn->unaryExpression();
            auto* value = asn->assignmentExpression();
            if (dest != nullptr && value != nullptr && FirstSourceChar(value) == '&')
            {
                const std::string source = value->getText();
                if (source.size() > 1 && source[0] == '&')
                {
                    const std::string target = source.substr(1);
                    const std::string alias = BareSourceText(dest);
                    for (const auto& p : params)
                        if (p.VariableName == target && !alias.empty()
                            && !NameShadowedByEnclosingLocal(asn, target, functionLambda))
                            pointerRoots[alias] = {
                                target, (size_t)asn->getStart()->getTokenIndex() };
                }
            }
        }
        for (auto* child : node->children) self(self, child);
    };
    collectAddressAliases(collectAddressAliases, body);
    auto collectDerefWrites = [&](auto&& self, antlr4::tree::ParseTree* node) -> void {
        if (node == nullptr) return;
        if (auto* asn = AsRuleCtx<CFlatParser::AssignmentExpressionContext>(node))
        {
            auto* dest = asn->unaryExpression();
            if (dest != nullptr && FirstSourceChar(dest) == '*')
            {
                const std::string text = dest->getText();
                if (text.size() > 1 && text[0] == '*')
                    if (auto it = pointerRoots.find(text.substr(1)); it != pointerRoots.end()
                        && (size_t)asn->getStart()->getTokenIndex() > it->second.token)
                        writtenNames.insert(it->second.name);
            }
        }
        for (auto* child : node->children) self(self, child);
    };
    collectDerefWrites(collectDerefWrites, body);
    if (!addressMayWrite) return;
    // `&param...` that is the whole source of a local pointer alias is tracked by the alias
    // passes (deref stores above, container mutators in the caller); any other `&param...` use
    // escapes to code this pass cannot see, so it counts as a write.
    auto isSimpleAliasVariable = [](CFlatParser::UnaryExpressionContext* target) {
        if (target == nullptr || target->postfixExpression() == nullptr
            || target->postfixExpression()->children.size() != 1) return false;
        auto* primary = target->postfixExpression()->primaryExpression();
        return primary != nullptr && primary->genericIdentifier() != nullptr
            && primary->genericIdentifier()->Identifier() != nullptr
            && primary->genericIdentifier()->genericTypeParameters() == nullptr;
    };
    auto isAliasSource = [&](CFlatParser::UnaryExpressionContext* unary) {
        const size_t startToken = (size_t)unary->getStart()->getTokenIndex();
        const size_t stopToken = (size_t)unary->getStop()->getTokenIndex();
        for (auto* parent = unary->parent; parent != nullptr; parent = parent->parent)
        {
            if ((size_t)parent->getSourceInterval().a != startToken
                || (size_t)parent->getSourceInterval().b != stopToken) return false;
            if (dynamic_cast<CFlatParser::InitializerContext*>(parent) != nullptr)
            {
                auto* init = dynamic_cast<CFlatParser::InitDeclaratorContext*>(parent->parent);
                auto* direct = init != nullptr && init->declarator() != nullptr
                    ? init->declarator()->directDeclarator() : nullptr;
                if (direct != nullptr && direct->Identifier() != nullptr
                    && direct->assignmentExpression() == nullptr) return true;
            }
            if (auto* asn = dynamic_cast<CFlatParser::AssignmentExpressionContext*>(parent->parent))
                if (asn->assignmentExpression() == parent
                    && isSimpleAliasVariable(asn->unaryExpression())) return true;
        }
        return false;
    };
    auto collectAddressedParams = [&](auto&& self, antlr4::tree::ParseTree* node) -> void {
        if (node == nullptr) return;
        if (auto* arg = dynamic_cast<CFlatParser::ArgumentNamedExpressionContext*>(node))
        {
            // A tracked alias handed to a callee may be written there.
            std::string text = BareSourceText(arg);
            if (!text.empty() && (text[0] == '*' || text[0] == '&')) text = text.substr(1);
            if (auto it = pointerRoots.find(text); it != pointerRoots.end()
                && (size_t)arg->getStart()->getTokenIndex() > it->second.token)
                writtenNames.insert(it->second.name);
        }
        auto* unary = dynamic_cast<CFlatParser::UnaryExpressionContext*>(node);
        if (unary != nullptr && FirstSourceChar(unary) == '&')
        {
            const std::string text = unary->getText();
            if (!text.empty() && text[0] == '&' && !isAliasSource(unary))
            {
                for (const auto& p : params)
                    if (!p.VariableName.empty()
                        && text.size() > p.VariableName.size() + 1
                        && text.compare(1, p.VariableName.size(), p.VariableName) == 0
                        && (text[p.VariableName.size() + 1] == '.'
                            || text[p.VariableName.size() + 1] == '>'
                            || text[p.VariableName.size() + 1] == '[')
                        && !NameShadowedByEnclosingLocal(unary, p.VariableName, functionLambda))
                        writtenNames.insert(p.VariableName);
                    else if (!p.VariableName.empty() && text == "&" + p.VariableName
                        && !NameShadowedByEnclosingLocal(unary, p.VariableName, functionLambda))
                        writtenNames.insert(p.VariableName);
            }
        }
        for (auto* child : node->children) self(self, child);
    };
    collectAddressedParams(collectAddressedParams, body);
}
bool ParamIsOwningSinkEligible(const LLVMBackend::TypeAndValue& p)
{
    if (p.Pointer || p.ElemPointer) return false;
    if (p.IsInterface || p.IsInterfacePointer) return false;
    // A THIN C function pointer (function<...> / __c_fn_ptr, or an encoded cfn$...) owns NOTHING,
    // so it can never be an owning sink. A FAT closure (Lambda<...> / __closure_fat_ptr, or an
    // encoded fatfn$...) is an OWNING VALUE like string - it owns its captured env - so it IS
    // sink-eligible: moved into the slot on insert, freed on teardown. The concrete
    // owns-a-resource gate still runs at the call site on the monomorphized type.
    if (p.IsThinFnPtr() || IsThinMangledClosure(p.TypeName)) return false;
    if (p.IsArrayView || p.IsSimd) return false;
    if (p.ConstArraySize > 0) return false;
    if (p.IsMove || MangledBase(p.TypeName) == "unique"
        || p.IsAlias) return false;
    if (p.IsBorrowOfAliasElement) return false;
    return true;
}
std::string SimpleIdentifierExpressionName(antlr4::tree::ParseTree* expression)
{
    auto* postfix = SolePostfixExpressionShape(expression);
    if (postfix == nullptr || postfix->children.size() != 1) return {};
    auto* primary = postfix->primaryExpression();
    auto* generic = primary != nullptr ? primary->genericIdentifier() : nullptr;
    if (primary == nullptr || primary->children.size() != 1 || generic == nullptr
        || generic->genericTypeParameters() != nullptr || generic->Identifier() == nullptr)
        return {};
    return generic->Identifier()->getText();
}
bool NameIsImplicitThisField(antlr4::tree::ParseTree* bodyRoot,
        const std::string& name)
{
    if (bodyRoot == nullptr || name.empty()) return false;
    antlr4::tree::ParseTree* aggregate = nullptr;
    for (auto* parent = bodyRoot; parent != nullptr; parent = parent->parent)
        if (dynamic_cast<CFlatParser::StructDefinitionContext*>(parent) != nullptr
            || dynamic_cast<CFlatParser::ClassDefinitionContext*>(parent) != nullptr)
        {
            aggregate = parent;
            break;
        }
    if (aggregate == nullptr) return false;
    bool found = false;
    auto scan = [&](auto&& self, antlr4::tree::ParseTree* node) -> void {
        if (found || node == nullptr) return;
        if (node != aggregate
            && (AsRuleCtx<CFlatParser::FunctionDefinitionContext>(node)
                || AsRuleCtx<CFlatParser::StructDefinitionContext>(node)
                || AsRuleCtx<CFlatParser::ClassDefinitionContext>(node))) return;
        if (auto* declaration = dynamic_cast<CFlatParser::DeclarationContext*>(node))
            if (auto* list = declaration->initDeclaratorList())
                for (auto* init : list->initDeclarator())
                {
                    auto* declarator = init->declarator();
                    auto* direct = declarator != nullptr ? declarator->directDeclarator() : nullptr;
                    if (direct != nullptr && direct->Identifier() != nullptr
                        && direct->Identifier()->getText() == name)
                    {
                        found = true;
                        return;
                    }
                }
        for (auto* child : node->children) self(self, child);
    };
    scan(scan, aggregate);
    return found;
}
bool ViewFieldAliasProvesParam(antlr4::tree::ParseTree* bodyRoot,
        antlr4::tree::ParseTree* use, const std::string& aliasName,
        const LLVMBackend::TypeAndValue& param,
        const CFlatParser::LambdaExpressionContext* functionLambda)
{
    if (bodyRoot == nullptr || use == nullptr || aliasName.empty()
        || aliasName == param.VariableName
        || !NameShadowedByEnclosingLocal(use, aliasName, functionLambda))
        return false;
    const size_t useToken = static_cast<size_t>(use->getSourceInterval().a);
    std::vector<CFlatParser::InitDeclaratorContext*> declarations;
    auto collectDeclarations = [&](auto&& self, antlr4::tree::ParseTree* node) -> void {
        if (node == nullptr) return;
        if (node != bodyRoot
            && (AsRuleCtx<CFlatParser::LambdaExpressionContext>(node)
                || AsRuleCtx<CFlatParser::FunctionDefinitionContext>(node))) return;
        if (auto* init = AsRuleCtx<CFlatParser::InitDeclaratorContext>(node))
        {
            auto* declarator = init->declarator();
            auto* direct = declarator != nullptr ? declarator->directDeclarator() : nullptr;
            if (direct != nullptr && direct->Identifier() != nullptr
                && direct->Identifier()->getText() == aliasName
                && static_cast<size_t>(init->getStart()->getTokenIndex()) < useToken)
                declarations.push_back(init);
        }
        for (auto* child : node->children) self(self, child);
    };
    collectDeclarations(collectDeclarations, bodyRoot);
    if (declarations.size() != 1) return false;
    auto* init = declarations.front();
    auto* declaration = dynamic_cast<CFlatParser::DeclarationContext*>(init->parent);
    for (auto* parent = init->parent; declaration == nullptr && parent != nullptr;
         parent = parent->parent)
        declaration = dynamic_cast<CFlatParser::DeclarationContext*>(parent);
    auto* specs = declaration != nullptr ? declaration->declarationSpecifiers() : nullptr;
    bool pointerType = false;
    if (specs != nullptr)
        for (auto* spec : specs->declarationSpecifier())
            pointerType = pointerType || (spec->typeSpecifier() != nullptr && spec->pointer() != nullptr);
    auto* initializer = init->initializer();
    auto* initialValue = initializer != nullptr ? initializer->assignmentExpression() : nullptr;
    if (!pointerType || SimpleIdentifierExpressionName(initialValue) != param.VariableName
        || NameShadowedByEnclosingLocal(initialValue, param.VariableName, functionLambda))
        return false;

    bool everyStoreAgrees = true;
    auto verifyStores = [&](auto&& self, antlr4::tree::ParseTree* node) -> void {
        if (!everyStoreAgrees || node == nullptr) return;
        if (node != bodyRoot
            && (AsRuleCtx<CFlatParser::LambdaExpressionContext>(node)
                || AsRuleCtx<CFlatParser::FunctionDefinitionContext>(node))) return;
        if (auto* assignment = AsRuleCtx<CFlatParser::AssignmentExpressionContext>(node))
        {
            auto* lhs = assignment->unaryExpression();
            if (lhs != nullptr && SimpleIdentifierExpressionName(lhs) == aliasName
                && assignment->assignmentOperator() != nullptr)
            {
                auto* rhs = assignment->assignmentExpression();
                if (assignment->assignmentOperator()->getText() != "="
                    || SimpleIdentifierExpressionName(rhs) != param.VariableName
                    || NameShadowedByEnclosingLocal(rhs, param.VariableName, functionLambda))
                    everyStoreAgrees = false;
            }
        }
        if (auto* unary = dynamic_cast<CFlatParser::UnaryExpressionContext*>(node))
            if (unary->unaryOperator() != nullptr && unary->unaryOperator()->getText() == "&"
                && SimpleIdentifierExpressionName(unary->castExpression()) == aliasName)
                everyStoreAgrees = false;
        for (auto* child : node->children) self(self, child);
    };
    verifyStores(verifyStores, bodyRoot);
    return everyStoreAgrees;
}
bool MoveIsLocalDeclInitializer(antlr4::tree::ParseTree* move)
{
    // Climb single-child expression wrappers; any other shape is not a bare initializer.
    antlr4::tree::ParseTree* node = move != nullptr ? move->parent : nullptr;
    while (node != nullptr && node->children.size() == 1
        && dynamic_cast<CFlatParser::InitializerContext*>(node) == nullptr)
        node = node->parent;
    auto* initializer = dynamic_cast<CFlatParser::InitializerContext*>(node);
    if (initializer == nullptr) return false;
    auto* declarator = dynamic_cast<CFlatParser::InitDeclaratorContext*>(initializer->parent);
    if (declarator == nullptr || declarator->parent == nullptr) return false;
    auto* list = dynamic_cast<CFlatParser::InitDeclaratorListContext*>(declarator->parent);
    if (list == nullptr || list->parent == nullptr) return false;
    if (dynamic_cast<CFlatParser::DeclarationContext*>(list->parent) == nullptr) return false;
    // Local only: a function or lambda body encloses it before any struct or class does.
    for (auto* scope = list->parent; scope != nullptr; scope = scope->parent)
    {
        if (dynamic_cast<CFlatParser::FunctionDefinitionContext*>(scope) != nullptr
            || dynamic_cast<CFlatParser::LambdaExpressionContext*>(scope) != nullptr)
            return true;
        if (dynamic_cast<CFlatParser::StructDefinitionContext*>(scope) != nullptr
            || dynamic_cast<CFlatParser::ClassDefinitionContext*>(scope) != nullptr)
            return false;
    }
    return false;
}
std::string ViewFieldMovePath(CFlatParser::UnaryExpressionContext* source,
        antlr4::tree::ParseTree* bodyRoot,
        const LLVMBackend::TypeAndValue& param,
        const std::vector<LLVMBackend::TypeAndValue>& allParams,
        const CFlatParser::LambdaExpressionContext* functionLambda)
{
    if (source == nullptr || !param.Pointer || param.VariableName.empty()) return {};
    auto* postfix = SolePostfixExpressionShape(source);
    auto* primary = postfix != nullptr ? postfix->primaryExpression() : nullptr;
    auto* generic = primary != nullptr ? primary->genericIdentifier() : nullptr;
    auto* root = generic != nullptr ? generic->Identifier() : nullptr;
    if (primary == nullptr || primary->children.size() != 1 || generic == nullptr
        || generic->genericTypeParameters() != nullptr || root == nullptr)
        return {};
    const std::string rootName = root->getText();
    const bool isImplicitThisParam = param.VariableName == param.TypeName + "__";
    const bool isImplicitThis = isImplicitThisParam && rootName == "this";
    const bool isParam = rootName == param.VariableName
        && !NameShadowedByEnclosingLocal(primary, param.VariableName, functionLambda);
    const bool isParamAlias = rootName != param.VariableName
        && ViewFieldAliasProvesParam(bodyRoot, primary, rootName, param, functionLambda);
    const bool shadowsExplicitParam = std::any_of(allParams.begin(), allParams.end(),
        [&](const auto& candidate) {
            return candidate.VariableName == rootName
                && candidate.VariableName != candidate.TypeName + "__";
        });
    const bool isImplicitThisField = isImplicitThisParam && rootName != "this"
        && !shadowsExplicitParam
        && !NameShadowedByEnclosingLocal(primary, rootName, functionLambda)
        && NameIsImplicitThisField(bodyRoot, rootName);
    if (!isImplicitThis && !isParam && !isParamAlias && !isImplicitThisField) return {};

    std::string path = isImplicitThisField ? rootName : std::string{};
    bool sawArrow = false;
    for (size_t i = 1; i < postfix->children.size();)
    {
        auto* token = dynamic_cast<antlr4::tree::TerminalNode*>(postfix->children[i]);
        if (token == nullptr) return {};
        const size_t tokenType = token->getSymbol()->getType();
        if (tokenType == CFlatParser::Arrow || tokenType == CFlatParser::Dot)
        {
            if (i + 1 >= postfix->children.size()) return {};
            auto* member = dynamic_cast<CFlatParser::MemberNameTokenContext*>(postfix->children[i + 1]);
            if (member == nullptr || (i + 2 < postfix->children.size()
                && dynamic_cast<CFlatParser::GenericTypeParametersContext*>(postfix->children[i + 2]) != nullptr))
                return {};
            if (tokenType == CFlatParser::Arrow)
            {
                if (sawArrow || !path.empty()) return {};
                sawArrow = true;
                path = member->getText();
            }
            else
            {
                if (!sawArrow && !isImplicitThisField) return {};
                path += "." + member->getText();
            }
            i += 2;
            continue;
        }
        if (tokenType == CFlatParser::LeftBracket)
        {
            if ((!sawArrow && !isImplicitThisField) || i + 2 >= postfix->children.size()) return {};
            auto* indexExpression = dynamic_cast<CFlatParser::ExpressionContext*>(postfix->children[i + 1]);
            auto* close = dynamic_cast<antlr4::tree::TerminalNode*>(postfix->children[i + 2]);
            auto* indexPostfix = SolePostfixExpressionShape(indexExpression);
            auto* indexPrimary = indexPostfix != nullptr
                ? indexPostfix->primaryExpression() : nullptr;
            auto* literal = indexPrimary != nullptr ? indexPrimary->Constant() : nullptr;
            if (indexPostfix == nullptr || close == nullptr
                || close->getSymbol()->getType() != CFlatParser::RightBracket)
                return {};
            if (indexPostfix->children.size() == 1 && indexPrimary != nullptr
                && indexPrimary->children.size() == 1 && literal != nullptr)
            {
                const std::string digits = literal->getText();
                uint64_t index = 0;
                const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), index);
                if (parsed.ec == std::errc{} && parsed.ptr == digits.data() + digits.size())
                    path += "[" + std::to_string(index) + "]";
                else path += "[*]";
            }
            else path += "[*]";
            i += 3;
            continue;
        }
        return {};
    }
    return sawArrow || isImplicitThisField ? path : std::string{};
}
void InferViewFieldMovePaths(antlr4::tree::ParseTree* node,
        antlr4::tree::ParseTree* bodyRoot,
        const LLVMBackend* compiler,
        std::vector<LLVMBackend::TypeAndValue>& allParams,
        const IfConstEvaluator& evalIfConst,
        const CFlatParser::LambdaExpressionContext* functionLambda)
{
    if (node == nullptr) return;
    if (auto* selection = AsRuleCtx<CFlatParser::SelectionStatementContext>(node))
    {
        antlr4::tree::ParseTree* liveBranch = nullptr;
        if (TryGetKnownIfConstBranch(selection, evalIfConst, liveBranch))
        {
            if (liveBranch != nullptr)
                InferViewFieldMovePaths(liveBranch, bodyRoot, compiler, allParams,
                                        evalIfConst, functionLambda);
            return;
        }
    }
    if (node != bodyRoot
        && (AsRuleCtx<CFlatParser::LambdaExpressionContext>(node)
            || AsRuleCtx<CFlatParser::FunctionDefinitionContext>(node)))
        return;
    // A local-declaration move copies a view (no sink), so it names no sink path.
    if (auto* move = AsRuleCtx<CFlatParser::MoveExpressionContext>(node);
        move != nullptr && !MoveIsLocalDeclInitializer(move))
        for (auto& param : allParams)
        {
            const std::string path = ViewFieldMovePath(
                move->unaryExpression(), bodyRoot, param, allParams, functionLambda);
            if (!path.empty()
                && std::find(param.ViewFieldMovePaths.begin(), param.ViewFieldMovePaths.end(), path)
                    == param.ViewFieldMovePaths.end())
                param.ViewFieldMovePaths.push_back(path);
        }
    if (compiler != nullptr)
        if (auto* postfix = dynamic_cast<CFlatParser::PostfixExpressionContext*>(node);
            postfix != nullptr && postfix->children.size() == 4)
        {
            auto* primary = postfix->primaryExpression();
            auto* generic = primary != nullptr ? primary->genericIdentifier() : nullptr;
            auto* name = generic != nullptr ? generic->Identifier() : nullptr;
            auto* open = dynamic_cast<antlr4::tree::TerminalNode*>(postfix->children[1]);
            auto* args = dynamic_cast<CFlatParser::ArgumentExpressionListContext*>(postfix->children[2]);
            auto* close = dynamic_cast<antlr4::tree::TerminalNode*>(postfix->children[3]);
            if (primary != nullptr && primary->children.size() == 1 && generic != nullptr
                && generic->genericTypeParameters() == nullptr && name != nullptr
                && open != nullptr && open->getSymbol()->getType() == CFlatParser::LeftParen
                && args != nullptr && !args->argumentNamedExpression().empty()
                && close != nullptr && close->getSymbol()->getType() == CFlatParser::RightParen)
            {
                const std::string calleeName = name->getText();
                const bool shadowsParam = std::any_of(allParams.begin(), allParams.end(),
                    [&](const auto& param) { return param.VariableName == calleeName; });
                if (!shadowsParam
                    && !NameShadowedByEnclosingLocal(primary, calleeName, functionLambda)
                    && !NameIsImplicitThisField(bodyRoot, calleeName)
                    && compiler->GetGlobalVariableNV(calleeName).Storage == nullptr)
                {
                    for (auto& callerParam : allParams)
                    {
                        if (!callerParam.Pointer || callerParam.VariableName.empty()) continue;
                        for (size_t argumentIndex = 0;
                             argumentIndex < args->argumentNamedExpression().size(); ++argumentIndex)
                        {
                            auto* argument = args->argumentNamedExpression(argumentIndex);
                            if (argument == nullptr || argument->assignmentExpression() == nullptr)
                                continue;
                            auto* forwarded = argument->assignmentExpression();
                            const std::string argName = SimpleIdentifierExpressionName(forwarded);
                            const bool directParam = argName == callerParam.VariableName
                                && !NameShadowedByEnclosingLocal(forwarded,
                                    callerParam.VariableName, functionLambda);
                            const bool localAlias = argName != callerParam.VariableName
                                && ViewFieldAliasProvesParam(bodyRoot, forwarded, argName,
                                    callerParam, functionLambda);
                            if (!directParam && !localAlias) continue;
                            const std::string namedParameter = argument->Identifier() != nullptr
                                ? argument->Identifier()->getText() : std::string{};
                            const auto calleeParams = compiler->FindCFlatViewFieldMoveParams(
                                calleeName, callerParam.TypeName,
                                args->argumentNamedExpression().size(), argumentIndex,
                                namedParameter);
                            for (const auto* calleeParam : calleeParams)
                                for (const auto& path : calleeParam->ViewFieldMovePaths)
                                    if (std::find(callerParam.ViewFieldMovePaths.begin(),
                                                  callerParam.ViewFieldMovePaths.end(), path)
                                        == callerParam.ViewFieldMovePaths.end())
                                        callerParam.ViewFieldMovePaths.push_back(path);
                        }
                    }
                }
            }
        }
    for (auto* child : node->children)
        InferViewFieldMovePaths(child, bodyRoot, compiler, allParams, evalIfConst, functionLambda);
}
void ApplyOwningSinkInferenceToBody(const LLVMBackend* compiler,
                                     antlr4::tree::ParseTree* body,
                                     std::vector<LLVMBackend::TypeAndValue>& allParams,
                                     const IfConstEvaluator& evalIfConst,
                                     const CFlatParser::LambdaExpressionContext* functionLambda)
{
    if (body == nullptr) return;
    if (std::any_of(allParams.begin(), allParams.end(), [](const auto& p) {
            return p.Pointer && !p.VariableName.empty(); }))
        InferViewFieldMovePaths(body, body, compiler, allParams, evalIfConst, functionLambda);
    // The scans only feed eligible parameters; a body with none (`main()`) skips them - their
    // shadowing checks are quadratic in the statement count of a large function.
    if (std::none_of(allParams.begin(), allParams.end(), [](const auto& p) {
            return !p.VariableName.empty() && ParamIsOwningSinkEligible(p); }))
        return;
    std::unordered_set<std::string> movedNames;
    ReturnScanMemo returnScanMemo;
    CollectUnconditionalMovedNames(body, movedNames, evalIfConst, &returnScanMemo);
    std::unordered_set<std::string> consumedNames;
    WrappedSourceNames wrappedConsumed;
    std::unordered_set<std::string> explicitMoves;
    std::unordered_set<std::string> returnedNames;
    WrappedSourceNames wrappedReturned;
    std::unordered_set<std::string> writtenNames;
    CollectConsumedStoreNames(body, consumedNames, &wrappedConsumed, &returnedNames, &wrappedReturned,
                              &writtenNames, functionLambda, &explicitMoves, evalIfConst);
    CollectWholeNameStoreDestinations(body, writtenNames, false, functionLambda);

    CollectAddressAliasWholeNameStores(body, allParams, writtenNames, false, functionLambda);
    for (auto& p : allParams)
    {
        if (p.VariableName.empty() || !ParamIsOwningSinkEligible(p)) continue;
        // An imported C++ class param follows the C++ by-value rule (the caller copy- or
        // move-constructs it), so a write or return in the body never consumes the caller.
        const bool cxxClass = compiler != nullptr
            && (compiler->IsCxxRecord(p.TypeName) || compiler->IsForeignNontrivialCxxClass(p.TypeName));
        // A cast-wrapped source counts only when every peeled wrapper names the parameter's own
        // type - a TYPE-CHANGING cast is not a whole-value consume of the parameter.
        bool wrappedConsume = false;
        if (auto it = wrappedConsumed.find(p.VariableName); it != wrappedConsumed.end())
            wrappedConsume = AllWrapperTypesName(compiler, it->second, p.TypeName);
        bool wrappedReturn = false;
        if (auto it = wrappedReturned.find(p.VariableName); it != wrappedReturned.end())
            wrappedReturn = AllWrapperTypesName(compiler, it->second, p.TypeName);
        const bool returnsParam = returnedNames.count(p.VariableName) || wrappedReturn;
        const bool isStringParam = compiler != nullptr
            && compiler->ResolveTypeAlias(p.TypeName) == "string";
        if (movedNames.count(p.VariableName))
        {
            p.IsOwningSink = true;
            // An explicit string move consumes; copy-only string uses stay borrowed.
            if (isStringParam) p.IsConsumeInferredSink = true;
            if (returnsParam)
            {
                p.IsReturnInferredSink = true;
                p.IsWriteInferredSink = true;
                if (isStringParam) p.IsConsumeInferredSink = true;
            }
        }
        else if (consumedNames.count(p.VariableName) || wrappedConsume)
        {
            p.IsOwningSink = true;
            p.IsConsumeInferredSink = !isStringParam
                || explicitMoves.count(p.VariableName) != 0;
            if (returnsParam)
            {
                p.IsReturnInferredSink = true;
                p.IsWriteInferredSink = true;
            }
        }
        else if (cxxClass)
            continue;
        else if (writtenNames.count(p.VariableName))
        {
            // A whole write is structural consume for non-strings; strings copy on entry, so the
            // caller keeps its buffer. Concrete copyability is decided at the call site.
            p.IsOwningSink = true;
            p.IsConsumeInferredSink = true;
            p.IsWriteInferredSink = true;
        }
        else if (returnsParam)
        {
            // Only `return p` consumes it: OwningSinkConsumesConcrete leaves a unique-owning
            // type to the inferred 'alias' return (ClassifyValueStructReturns) as before.
            p.IsOwningSink = true;
            p.IsConsumeInferredSink = true;
            p.IsReturnInferredSink = true;
        }
    }
}
void AdoptInferredParamSinks(LLVMBackend::TypeAndValue& dest,
                                    const std::vector<LLVMBackend::TypeAndValue::FuncPtrParam>& src)
{
    if (dest.FuncPtrParams.size() != src.size()) return;
    for (size_t i = 0; i < src.size(); i++)
    {
        auto& d = dest.FuncPtrParams[i];
        if (d.AllocAlignValue == 0)
            d.AllocAlignValue = src[i].AllocAlignValue;
        d.IsOwningSink = d.IsOwningSink || src[i].IsOwningSink;
        d.IsConsumeInferredSink = d.IsConsumeInferredSink || src[i].IsConsumeInferredSink;
        d.IsReturnInferredSink = d.IsReturnInferredSink || src[i].IsReturnInferredSink;
        d.IsWriteInferredSink = d.IsWriteInferredSink || src[i].IsWriteInferredSink;
    }
}
void ApplyOwningSinkInference(const LLVMBackend* compiler,
                                     CFlatParser::FunctionDefinitionContext* func,
                                     std::vector<LLVMBackend::TypeAndValue>& allParams,
                                     const IfConstEvaluator& evalIfConst)
{
    ApplyOwningSinkInferenceToBody(compiler, func->compoundStatement(), allParams, evalIfConst);
}
std::string RecordReceiverScan(LLVMBackend* compiler, CFlatParser::FunctionDefinitionContext* func,
                                      const std::string& name, const LLVMBackend::DeclTypeAndValue& returnType,
                                      const std::vector<LLVMBackend::TypeAndValue>& allParams, bool varargs,
                                      const IfConstEvaluator& eval)
{
    if (compiler == nullptr || func == nullptr || func->compoundStatement() == nullptr || allParams.empty())
        return {};
    const auto& self = allParams.front();
    if (!self.Pointer || self.VariableName != self.TypeName + "__" || returnType.external) return {};
    const std::string unique = compiler->ComputeMangledName(name, returnType, allParams, varargs);
    auto facts = ReceiverScan(allParams, eval).Run(func->compoundStatement());
    facts.Line = func->getStart()->getLine();
    facts.Column = func->getStart()->getCharPositionInLine();
    compiler->RecordReceiverScanFacts(unique, std::move(facts));
    return unique;
}
bool FunctionDeclaresReturnType(CFlatParser::FunctionDefinitionContext* func)
{
    auto* specs = func->declarationSpecifiers();
    if (specs == nullptr) return false;
    for (auto* spec : specs->declarationSpecifier())
        if (spec->typeSpecifier() != nullptr) return true;
    return false;
}
std::string DescribeParameterTypes(const std::vector<LLVMBackend::TypeAndValue>& params)
{
    std::string text;
    for (const auto& p : params)
    {
        if (!text.empty()) text += ", ";
        text += p.TypeName;
        // An array-view param carries Pointer too, but spells as 'T[]', never 'T[]*'.
        if (p.IsArrayView) text += "[]";
        else if (p.Pointer) text += std::string(p.ValuePointerDepth(), '*');
    }
    return text;
}
