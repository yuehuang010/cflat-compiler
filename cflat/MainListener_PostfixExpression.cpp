#include "MainListener.h"

static bool JsonConstIdentifier(const std::string& text)
{
    if (text.empty() || (!std::isalpha((unsigned char)text[0]) && text[0] != '_')) return false;
    for (char c : text)
        if (!std::isalnum((unsigned char)c) && c != '_') return false;
    return true;
}

static bool JsonConstStringToken(const std::string& text)
{
    size_t quote = 0;
    if (text.starts_with("u8")) quote = 2;
    else if (!text.empty() && (text[0] == 'u' || text[0] == 'U' || text[0] == 'L')) quote = 1;
    if (quote >= text.size() || text[quote] != '"') return false;

    bool escaped = false;
    for (size_t i = quote + 1; i < text.size(); ++i)
    {
        char c = text[i];
        if (!escaped && c == '"') return i + 1 == text.size();
        if (!escaped && c == '\\') escaped = true;
        else escaped = false;
    }
    return false;
}

static bool JsonConstIntegerToken(const std::string& text)
{
    if (text.empty()) return false;
    size_t i = text[0] == '-' ? 1 : 0;
    if (i == text.size()) return false;
    for (; i < text.size(); ++i)
    {
        if (!std::isdigit((unsigned char)text[i])) return false;
    }
    return true;
}

static bool JsonConstFloatToken(const std::string& text)
{
    if (text.empty() || text[0] == '+') return false;
    size_t digitCount = 0;
    bool dot = false;
    bool exponent = false;
    for (size_t i = text[0] == '-' ? 1 : 0; i < text.size(); ++i)
    {
        char c = text[i];
        if (std::isdigit((unsigned char)c)) { ++digitCount; continue; }
        if (c == '.' && !dot && !exponent) { dot = true; continue; }
        if ((c == 'e' || c == 'E') && !exponent && digitCount > 0)
        {
            exponent = true;
            if (i + 1 < text.size() && (text[i + 1] == '+' || text[i + 1] == '-')) ++i;
            continue;
        }
        if ((c == 'f' || c == 'F') && i + 1 == text.size()) continue;
        return false;
    }
    return digitCount > 0 && (dot || exponent);
}

static std::string JsonConstEscape(const std::string& text)
{
    std::string result;
    for (unsigned char c : text)
    {
        switch (c)
        {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (c < 0x20)
            {
                static constexpr char hex[] = "0123456789abcdef";
                result += "\\u00";
                result += hex[c >> 4];
                result += hex[c & 0xf];
            }
            else result += (char)c;
        }
    }
    return result;
}

static std::string XmlConstEscape(const std::string& text)
{
    std::string result;
    for (char c : text)
    {
        switch (c)
        {
        case '&': result += "&amp;"; break;
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '"': result += "&quot;"; break;
        case '\'': result += "&apos;"; break;
        default: result += c; break;
        }
    }
    return result;
}

std::optional<std::string> MainListener::FoldConstLiteral(
    antlr4::ParserRuleContext* ctx, const std::string& intrinsicName,
    const std::string& typeName, CFlatParser::InitializerListContext* init,
    std::vector<LLVMBackend::ManifestFragment::Leaf>* manifestLeaves)
{
    auto* compiler = Compiler(ctx);
    auto spellTypeName = [&](const std::string& name) {
        return SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = name });
    };
    const bool xmlConst = intrinsicName == "xml_const";
    auto rootData = compiler->GetDataStructure(typeName);
    if (!rootData.StructType)
    {
        LogErrorContext(ctx, std::format(
            "{}(): unknown struct type '{}'", intrinsicName, spellTypeName(typeName)));
        return std::nullopt;
    }
    if (rootData.IsUnion)
    {
        LogErrorContext(ctx, std::format(
            "{}() is not supported on union type '{}'", intrinsicName, spellTypeName(typeName)));
        return std::nullopt;
    }
    if (init == nullptr)
    {
        LogErrorContext(ctx, std::format(
            "{}() requires a type name and a brace initializer", intrinsicName));
        return std::nullopt;
    }

    bool valid = true;
    auto literalError = [&](antlr4::ParserRuleContext* valueCtx) {
        LogErrorContext(valueCtx, std::format(
            "{} initializer must be a compile-time literal", intrinsicName));
        valid = false;
    };
    auto isIntegerType = [](const std::string& name) {
        return name == "char" || name == "short" || name == "int" || name == "long"
            || name == "i8" || name == "i16" || name == "i32" || name == "i64"
            || name == "u8" || name == "u16" || name == "u32" || name == "u64"
            || name == "ulong";
    };
    auto isScalarType = [&](const std::string& name) {
        return isIntegerType(name) || name == "float" || name == "double"
            || name == "bool" || name == "string"
            || !compiler->GetEnumBackingType(name).empty();
    };
    auto hasAnnotation = [](const LLVMBackend::DeclTypeAndValue& field, const std::string& name) {
        for (const auto& ann : field.Annotations)
            if (ann.Name == name) return true;
        return false;
    };
    auto fieldDisplayName = [](const LLVMBackend::DeclTypeAndValue& field) {
        for (const auto& ann : field.Annotations)
            if (ann.Name == "JsonName" && !ann.Value.empty()) return ann.Value;
        return field.VariableName;
    };
    auto isEmptyNestedBrace = [](CFlatParser::FieldInitContext* fieldInit) {
        return fieldInit != nullptr && fieldInit->initializerList() == nullptr
            && fieldInit->assignmentExpression().empty() && fieldInit->getText().ends_with("{}");
    };

    std::function<std::string(const std::string&, CFlatParser::AssignmentExpressionContext*,
                              antlr4::ParserRuleContext*)> foldScalar;
    std::function<std::string(const std::string&, CFlatParser::FieldInitContext*, const std::string&)> foldValue;
    std::function<std::string(const std::string&, CFlatParser::InitializerListContext*, bool)> foldStruct;
    std::function<std::string(const std::string&, const std::string&, CFlatParser::InitializerListContext*)> emitXmlElement;

    foldScalar = [&](const std::string& fieldType, CFlatParser::AssignmentExpressionContext* expression,
                     antlr4::ParserRuleContext* errorCtx) -> std::string {
        if (expression == nullptr) { literalError(errorCtx); return {}; }
        std::string raw = expression->getText();
        if (isIntegerType(fieldType))
        {
            if (!JsonConstIntegerToken(raw)) { literalError(expression); return {}; }
            return raw;
        }
        if (fieldType == "float" || fieldType == "double")
        {
            if (!JsonConstFloatToken(raw)) { literalError(expression); return {}; }
            if (!raw.empty() && (raw.back() == 'f' || raw.back() == 'F')) raw.pop_back();
            size_t lead = raw[0] == '-' ? 1 : 0;
            if (raw.size() > lead && raw[lead] == '.') raw.insert(lead, "0");
            if (raw.back() == '.') raw += "0";
            return raw;
        }
        if (fieldType == "bool")
        {
            if (raw != "true" && raw != "false") { literalError(expression); return {}; }
            return raw;
        }
        if (fieldType == "string")
        {
            std::string value;
            if (JsonConstStringToken(raw) && !HasInterpolation(raw))
                value = ProcessRawText(raw);
            else if (auto constant = compiler->GetCompileTimeStringConstant(raw))
                value = *constant;
            else
            {
                literalError(expression);
                return {};
            }
            return xmlConst ? value : "\"" + JsonConstEscape(value) + "\"";
        }
        auto backingType = compiler->GetEnumBackingType(fieldType);
        if (!backingType.empty())
        {
            size_t dot = raw.find('.');
            if (dot == std::string::npos || raw.find('.', dot + 1) != std::string::npos
                || raw.substr(0, dot) != fieldType || !JsonConstIdentifier(raw.substr(dot + 1)))
            {
                LogErrorContext(expression, std::format(
                    "{} enum initializer must name a member of '{}', got '{}'",
                    intrinsicName, fieldType, raw));
                valid = false;
                return {};
            }
            std::string member = raw.substr(dot + 1);
            auto memberNV = compiler->GetGlobalVariableNV(fieldType + "." + member);
            if (!memberNV.Storage)
            {
                LogErrorContext(expression, std::format(
                    "{}: unknown enum member '{}.{}'", intrinsicName, fieldType, member));
                valid = false;
                return {};
            }
            return xmlConst ? member : "\"" + JsonConstEscape(member) + "\"";
        }
        literalError(errorCtx);
        return {};
    };

    foldValue = [&](const std::string& fieldType, CFlatParser::FieldInitContext* fieldInit,
                    const std::string& elementName) -> std::string {
        if (fieldInit == nullptr) { literalError(ctx); return {}; }
        auto values = fieldInit->assignmentExpression();
        auto* nested = fieldInit->initializerList();
        if (nested == nullptr && values.empty() && isEmptyNestedBrace(fieldInit))
        {
            if (MangledBase(fieldType) == "list") return xmlConst ? "" : "[]";
            if (compiler->GetDataStructure(fieldType).StructType != nullptr)
                return xmlConst ? emitXmlElement(elementName, fieldType, nullptr)
                                : foldStruct(fieldType, nullptr, false);
            literalError(fieldInit);
            return {};
        }
        if (nested != nullptr)
        {
            if (MangledBase(fieldType) == "list")
            {
                TypeSpelling listSpelling;
                if (!DemangleType(*compiler, fieldType, listSpelling)
                    || listSpelling.args.size() != 1)
                {
                    literalError(fieldInit);
                    return {};
                }
                std::string elementType = MangleType(*compiler, listSpelling.args[0]);
                if (xmlConst && compiler->GetDataStructure(elementType).StructType == nullptr)
                {
                    LogErrorContext(fieldInit, std::format(
                        "xml_const list field '{}' must contain struct elements",
                        fieldInit->Identifier() == nullptr ? "<unnamed>" : fieldInit->Identifier()->getText()));
                    valid = false;
                    return {};
                }
                std::string result = xmlConst ? "" : "[";
                bool first = true;
                for (auto* element : nested->fieldInit())
                {
                    if (xmlConst)
                    {
                        if (element->Identifier() != nullptr || element->Colon() != nullptr
                            || !element->assignmentExpression().empty() || element->initializerList() == nullptr)
                        {
                            literalError(element);
                            continue;
                        }
                        result += emitXmlElement(elementName, elementType, element->initializerList());
                    }
                    else
                    {
                        if (element->Identifier() != nullptr || element->Colon() != nullptr
                            || element->assignmentExpression().size() > 1)
                        {
                            literalError(element);
                            continue;
                        }
                        if (!first) result += ",";
                        first = false;
                        result += foldValue(elementType, element, "");
                    }
                }
                if (!xmlConst) result += "]";
                return result;
            }
            if (compiler->GetDataStructure(fieldType).StructType != nullptr)
                return xmlConst ? emitXmlElement(elementName, fieldType, nested)
                                : foldStruct(fieldType, nested, false);
            literalError(fieldInit);
            return {};
        }
        if (fieldInit->Colon() != nullptr || values.size() != 1)
        {
            literalError(fieldInit);
            return {};
        }
        return foldScalar(fieldType, values[0], fieldInit);
    };

    emitXmlElement = [&](const std::string& elementName, const std::string& structName,
                         CFlatParser::InitializerListContext* nestedInit) {
        std::string content = foldStruct(structName, nestedInit, false);
        size_t child = content.find('<');
        if (child == std::string::npos) return "<" + elementName + content + "/>";
        return "<" + elementName + content.substr(0, child) + ">"
            + content.substr(child) + "</" + elementName + ">";
    };

    foldStruct = [&](const std::string& structName, CFlatParser::InitializerListContext* nestedInit,
                     bool rootTransparent) {
        auto data = compiler->GetDataStructure(structName);
        std::unordered_map<std::string, CFlatParser::FieldInitContext*> named;
        if (nestedInit != nullptr) for (auto* fieldInit : nestedInit->fieldInit())
        {
            if (fieldInit->Identifier() == nullptr || fieldInit->assignmentExpression().size() > 1
                || (fieldInit->initializerList() == nullptr && fieldInit->assignmentExpression().empty()
                    && !isEmptyNestedBrace(fieldInit)))
            {
                LogErrorContext(fieldInit, std::format(
                    "{} struct initializer for '{}' requires named fields", intrinsicName, spellTypeName(structName)));
                valid = false;
                continue;
            }
            std::string fieldName = fieldInit->Identifier()->getText();
            if (!named.emplace(fieldName, fieldInit).second)
            {
                LogErrorContext(fieldInit, std::format(
                    "{}: duplicate field initializer '{}'", intrinsicName, fieldName));
                valid = false;
            }
        }

        std::unordered_map<std::string, std::string> xmlNamespaceBindings;
        if (xmlConst)
            for (const auto& field : data.StructFields)
            {
                if (field.IsBitfieldStorage || field.IsPadding || hasAnnotation(field, "Private"))
                    continue;
                std::string displayName = fieldDisplayName(field);
                if (field.TypeName != "string"
                    || !(displayName == "xmlns" || displayName.starts_with("xmlns:")))
                    continue;
                std::string raw;
                if (auto it = named.find(field.VariableName); it != named.end())
                {
                    auto* source = it->second;
                    if (source->initializerList() != nullptr || source->Colon() != nullptr
                        || source->assignmentExpression().size() != 1)
                        continue;
                    raw = source->assignmentExpression(0)->getText();
                }
                else if (field.Initializer != nullptr
                    && field.Initializer->assignmentExpression() != nullptr)
                    raw = field.Initializer->assignmentExpression()->getText();
                else
                    continue;
                if (JsonConstStringToken(raw) && !HasInterpolation(raw))
                    xmlNamespaceBindings[displayName] = ProcessRawText(raw);
            }

        auto recordXmlTextLeaf = [&](const std::string& displayName, const std::string& value) {
            if (manifestLeaves == nullptr) return;
            size_t colon = displayName.find(':');
            std::string prefix = colon == std::string::npos ? "" : displayName.substr(0, colon);
            std::string binding = prefix.empty() ? "xmlns" : "xmlns:" + prefix;
            auto ns = xmlNamespaceBindings.find(binding);
            manifestLeaves->push_back({ ns == xmlNamespaceBindings.end() ? "" : ns->second,
                colon == std::string::npos ? displayName : displayName.substr(colon + 1),
                value, compiler->GetSourceFilePath(),
                ctx != nullptr && ctx->getStart() != nullptr ? ctx->getStart()->getLine() : 0 });
        };

        std::string result = xmlConst ? "" : "{";
        std::string xmlAttributes;
        std::string xmlChildren;
        bool first = true;
        for (const auto& field : data.StructFields)
        {
            if (field.IsBitfieldStorage || field.IsPadding) continue;
            if (hasAnnotation(field, "Private")) continue;
            const bool isList = !field.Pointer && MangledBase(field.TypeName) == "list";
            const bool isStruct = !field.Pointer && compiler->GetDataStructure(field.TypeName).StructType != nullptr;
            const bool isScalar = !field.Pointer && isScalarType(field.TypeName);
            const bool jsonText = hasAnnotation(field, "JsonText");
            const std::string displayName = fieldDisplayName(field);

            if (xmlConst && rootTransparent && !isStruct && !isList)
            {
                LogErrorContext(nestedInit, std::format(
                    "xml_const root fields must be elements (struct or list<struct>), field '{}' is scalar",
                    field.VariableName));
                valid = false;
                continue;
            }
            if (xmlConst && isList)
            {
                TypeSpelling listSpelling;
                if (!DemangleType(*compiler, field.TypeName, listSpelling)
                    || listSpelling.args.size() != 1
                    || compiler->GetDataStructure(
                        MangleType(*compiler, listSpelling.args[0])).StructType == nullptr)
                {
                    LogErrorContext(nestedInit, std::format(
                        "xml_const list field '{}' must contain struct elements", field.VariableName));
                    valid = false;
                    continue;
                }
            }
            if (xmlConst && jsonText && (isStruct || isList))
            {
                LogErrorContext(nestedInit, std::format(
                    "xml_const [JsonText] requires a scalar field '{}'", field.VariableName));
                valid = false;
                continue;
            }

            auto it = named.find(field.VariableName);
            if (it == named.end())
            {
                if (!xmlConst || !isScalar) continue;
                if (field.Initializer != nullptr && field.Initializer->assignmentExpression() != nullptr)
                {
                    std::string value = foldScalar(field.TypeName,
                        field.Initializer->assignmentExpression(), field.Initializer);
                    if (!valid) continue;
                    if (jsonText)
                    {
                        recordXmlTextLeaf(displayName, value);
                        xmlChildren += "<" + displayName + ">" + XmlConstEscape(value)
                            + "</" + displayName + ">";
                    }
                    else
                        xmlAttributes += " " + displayName + "=\"" + XmlConstEscape(value) + "\"";
                }
                else if (field.Initializer != nullptr && field.Initializer->Default() == nullptr
                    && field.BraceInitializer == nullptr)
                {
                    LogErrorContext(nestedInit, std::format(
                        "xml_const default for field '{}' must be a compile-time literal", field.VariableName));
                    valid = false;
                }
                continue;
            }

            std::string value = foldValue(field.TypeName, it->second, displayName);
            if (!xmlConst)
            {
                if (!first) result += ",";
                first = false;
                result += "\"" + JsonConstEscape(displayName) + "\":" + value;
            }
            else if (isScalar)
            {
                if (jsonText)
                {
                    recordXmlTextLeaf(displayName, value);
                    xmlChildren += "<" + displayName + ">" + XmlConstEscape(value)
                        + "</" + displayName + ">";
                }
                else
                    xmlAttributes += " " + displayName + "=\"" + XmlConstEscape(value) + "\"";
            }
            else if (!value.empty())
                xmlChildren += value;
        }
        for (const auto& [fieldName, fieldInit] : named)
        {
            bool found = false;
            for (const auto& field : data.StructFields)
                if (field.VariableName == fieldName) { found = true; break; }
            if (!found)
            {
                LogErrorContext(fieldInit, std::format(
                    "{}: no field named {} in {}", intrinsicName, fieldName, spellTypeName(structName)));
                valid = false;
            }
        }
        if (!xmlConst) result += "}";
        else result = xmlAttributes + xmlChildren;
        return result;
    };

    std::string foldedText = foldStruct(typeName, init, xmlConst);
    if (!valid) return std::nullopt;
    return foldedText;
}

/*
 * Every direct call - free function, class method, interface vtable slot, generic
 * instantiation, namespace-qualified, aliased return type - finishes here, so this wrapper
 * is the one gate the void-result rule needs. The body has six exits; gating them
 * individually is the site enumeration this repo has paid for before.
 */
LLVMBackend::NamedVariable MainListener::ParsePostfixExpression(CFlatParser::PostfixExpressionContext* ctx, bool lValue,
                                                       size_t dropTrailingChildren, ResultUse use) {
        auto namedVar = ParsePostfixExpressionInner(ctx, lValue, dropTrailingChildren, use);
        if (namedVar.FromOwningTempField && !namedVar.OwningTempParent)
            Compiler(ctx)->RegisterTempFieldValue(namedVar.Primary);
        // Name the callee from the spelling: the chain up to its LAST top-level '(', so a call
        // on another call's result names the callee consumed here, not the first one in the chain.
        std::string text = ctx->getText();
        size_t paren = std::string::npos;
        int depth = 0;
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '(' || text[i] == '[')
            {
                if (depth == 0 && text[i] == '(') paren = i;
                depth++;
            }
            else if ((text[i] == ')' || text[i] == ']') && depth > 0) depth--;
        }
        std::string name = paren == std::string::npos ? text : text.substr(0, paren);
        std::string subject = (name.empty() || name.size() > 40)
            ? std::string("the call") : std::format("'{}'", name);
        DiagnoseVoidResultConsumed(ctx, namedVar, use, subject);
        return namedVar;
    }

void MainListener::DiagnoseVoidResultConsumed(antlr4::ParserRuleContext* ctx,
                                              const LLVMBackend::NamedVariable& nv,
                                              ResultUse use, const std::string& subject) {
        // Discard positions want exactly this result, and `return <call>;` is settled by
        // EmitReturnExpression (a void crossing out of a void function is legal) - both defer.
        if (use != ResultUse::Value || nv.TypeAndValue.Pointer) return;
        // Resolve through `using V = void;` - the spelling is aliasable, so matching the
        // literal "void" would leave the alias reaching the verifier.
        if (Compiler(ctx)->ResolveTypeAlias(nv.TypeAndValue.TypeName) != "void") return;
        // The NAME alone is not proof: a stale lastCallReturnType can spell 'void' over a
        // result that really carries a value (an i1 from a bool method). Require value-lessness.
        if (nv.Storage != nullptr || (nv.Primary != nullptr && !nv.Primary->getType()->isVoidTy()))
            return;
        LogErrorContext(ctx, std::format(
            "call to {} returns 'void', so it produces no value to consume - "
            "call it as a statement", subject));
    }

LLVMBackend::NamedVariable MainListener::ParsePostfixExpressionInner(CFlatParser::PostfixExpressionContext* ctx, bool lValue,
                                                       size_t dropTrailingChildren, ResultUse use) {
        /*
        * postfixExpression
            : primaryExpression
            (
                '[' expression ']'
                | '(' argumentExpressionList? ')'
                | ('.' | '->') Identifier
                | '++'
                | '--'
            )*
        */

        // Static methods on the simd type: `simd<T,N>.load(arr, i)` and `simd<T,N>.store(vec, arr, i)`.
        // The type args make these self-describing (T,N come from the spelling, not the LHS), so
        // they work with `auto` and compose inside larger expressions. Intercept here before the
        // general member-access walker, since the primary is a *type*, not a value.
        if (auto* primaryCtx = ctx->primaryExpression())
            if (auto* simdSpec = primaryCtx->simdTypeSpecifier())
                return ParseSimdStaticMethod(ctx, simdSpec);

        if (auto primaryCtx = ctx->primaryExpression())
        {
            size_t prevRuleId = 0;
            size_t prevToken = 0;
            LLVMBackend::NamedVariable namedVar;
            LLVMBackend::NamedVariable structVar;
            LLVMBackend::NamedVariable interfaceVar;
            bool receiverWasFixedArray = false;
            auto hasFixedArrayShape = [](const LLVMBackend::NamedVariable& value) {
                return value.TypeAndValue.ConstArraySize > 0
                    || (value.BaseType && llvm::isa<llvm::ArrayType>(value.BaseType));
            };
            std::string primaryIdentifier;
            std::string callDisplayName;
            std::string namespaceContext;

            int functionArgCounter = 0;
            bool nullConditionalPending = false;
            // HResult `?.`/`?->` chaining: armed by [PFX-1] when the receiver is an HResult<T*>.
            // structVar is redirected to the unwrapped `.value` object; hresultChainStorage holds
            // the source HResult so the call dispatch can branch on failed() and propagate hr.
            bool hresultChainPending = false;
            llvm::Value* hresultChainStorage = nullptr;
            std::string hresultChainType;
            // The unwrapped `.value` pointer to Release once the chain completes, or null to leave
            // it alone. Only set for a provably-owned (+1) temporary - see the arming site [PFX-1].
            llvm::Value* hresultChainReleasePtr = nullptr;
            std::string hresultChainReleaseType;
            // SSA result of the most recent [winrt] vtable slot call in THIS postfix chain. COM
            // requires an [out,retval] interface pointer to be AddRef'd, so only such a result is
            // a provably-owned HResult<T*>; a plain function's HResult may hold a borrowed pointer.
            llvm::Value* lastWinrtSlotCallResult = nullptr;
            // Set when the primary was a `global::name` form whose base resolved to a dot-less
            // root function: the following call must resolve at root (skip the enclosing-namespace
            // walk). Consumed and cleared by the call dispatch so chained calls do not inherit it.
            bool globalScopeCall = false;
            // Storage restored from a parenthesized primary must remain available to ++/--
            // while the suffix walk consumes the restored value.
            llvm::Value* parenthesizedPostfixStorage = nullptr;
            // Armed by [PFX-1] when a bare-pointer BORROW receiver is followed by `.copy` - the next
            // call is a pointer-identity copy (see [PFX-copy-ptr]). The pointer value/type are stashed
            // because the auto-deref mutates namedVar. Consumed and cleared by the call.
            bool pendingPtrCopyIdentity = false;
            llvm::Value* pendingPtrCopyValue = nullptr;
            LLVMBackend::TypeAndValue pendingPtrCopyType;
            // Consumed-COM sugar receiver: when [PFX-2a] redirects a thin interface pointer through
            // its lpVtbl, this holds the original object pointer so the following call ([PFX-5])
            // can inject it as the implicit `this` first argument - `rs->Release()` dispatches as
            // `rs->lpVtbl->Release(rs)`. Set on redirect, consumed (and cleared) by the call.
            llvm::Value* pendingThinComReceiver = nullptr;
            // [PFX-2-dangle] Armed by [PFX-2] for a `.`/`->` member name; cleared by the next suffix
            // token (a call `(` above all). Still armed at the end of the chain means the member was
            // never called - `obj.method` without '()' - which otherwise returns an empty
            // NamedVariable that callers dereference (SIGSEGV). See the exit check below.
            std::string danglingMemberName;
            std::string danglingMemberOwner;
            bool danglingIsMethod = false;

            // Whole-chain '?.' short-circuit: links after the first '?.' run in a shared "access"
            // block, so a null anywhere upstream skips the REST of the chain (merged at the end).
            llvm::BasicBlock* ncChainNullBlock = nullptr;
            std::optional<LLVMBackend::OwnedTempMark> ncTempMark;
            // The block holding the FIRST '?.' branch: it dominates the merge, so owned temps
            // created inside the chain are re-homed there instead of freed at the merge.
            llvm::BasicBlock* ncHoistBlock = nullptr;
            auto ncEnterGuard = [&](llvm::Value* testPtr)
            {
                auto* compiler = Compiler(ctx);
                if (!ncTempMark.has_value())
                    ncTempMark = compiler->MarkOwnedTemps();
                if (ncHoistBlock == nullptr)
                    ncHoistBlock = compiler->builder->GetInsertBlock();
                if (ncChainNullBlock == nullptr)
                    ncChainNullBlock = compiler->CreateBasicBlock("nc_null");
                auto* accessBlock = compiler->CreateBasicBlock("nc_access");
                // CreateConditionJump already leaves the insert point at accessBlock.
                compiler->CreateConditionJump(testPtr, accessBlock, ncChainNullBlock);
                nullConditionalPending = false;
            };
            // A guarded chain merges into a phi, so it yields a VALUE and has no single
            // address - read-modify-write operators have nothing to write back through.
            auto NullConditionalNotWritable = [&](const char* op)
            {
                // LogErrorContext throws, so the chain's half-built blocks must be closed HERE
                // or the aborted walk leaves an unterminated 'nc_null' behind (same guard the
                // return-block bailouts below use).
                auto* compiler = Compiler(ctx);
                if (!compiler->IsBlockTerminated())
                    compiler->builder->CreateUnreachable();
                if (ncChainNullBlock != nullptr && cflat_llvm::GetTerminatorOrNull(ncChainNullBlock) == nullptr)
                {
                    compiler->SwitchToBlock(ncChainNullBlock);
                    compiler->builder->CreateUnreachable();
                }
                return std::format(
                    "'{}' cannot be applied to a null-conditional '?.' access - the guarded chain "
                    "yields a value, not an addressable location. Null-check the receiver and use "
                    "a plain '.' access instead.", op);
            };

            // dropTrailingChildren lets a caller (e.g. the lock statement, for `rw.read`)
            // evaluate only the base of the postfix chain, ignoring a trailing suffix.
            size_t childLimit = ctx->children.size();
            childLimit -= std::min(dropTrailingChildren, childLimit);
            size_t childIndex = 0;

            // A suffix consumes the primary as a receiver/callee, so the enclosing destination
            // belongs to the final postfix result, not to that primary. The one exception is a
            // direct call whose only suffix is its immediate call; that call result is the whole
            // expression and may use the destination below.
            bool directCallWhole = false;
            if (childLimit > 1 && ctx->children[1]->getText() == "(")
            {
                size_t firstRightParen = childLimit;
                for (size_t i = 1; i < childLimit; i++)
                {
                    auto* terminal = dynamic_cast<antlr4::tree::TerminalNode*>(ctx->children[i]);
                    if (terminal != nullptr && terminal->getSymbol()->getType() == CFlatParser::RightParen)
                    {
                        firstRightParen = i;
                        break;
                    }
                }
                directCallWhole = firstRightParen == childLimit - 1;
            }
            std::optional<DeclExpectedTypeScope> postfixBaseExpectedScope;
            if (childLimit > 1 && !directCallWhole)
                postfixBaseExpectedScope.emplace(&declExpectedType, LLVMBackend::TypeAndValue{});

            // The receiver of the member name currently being walked, spelled as in source
            // ("h.c", "a[0]"): every child before the '.'/'->' that introduced the name. Used
            // only to NAME a sub-object receiver in the definitely-null diagnostic, where the
            // NamedVariable's own name is the container's. childIndex is already past the name.
            auto ReceiverSourceText = [&]() -> std::string
            {
                if (childIndex < 3) return std::string();
                std::string text;
                for (size_t i = 0; i + 2 < childIndex; ++i)
                    text += ctx->children[i]->getText();
                return text;
            };
            auto isFixedArrayReceiver = [&]() {
                bool result = receiverWasFixedArray
                    || hasFixedArrayShape(structVar)
                    || hasFixedArrayShape(namedVar)
                    || hasFixedArrayShape(interfaceVar);
                if (!result)
                {
                    std::string receiverText = ReceiverSourceText();
                    auto original = Compiler(ctx)->GetScopedLocalOrArgument(receiverText);
                    if (!hasFixedArrayShape(original))
                        original = Compiler(ctx)->GetGlobalVariableNV(receiverText);
                    result = hasFixedArrayShape(original);
                }
                return result;
            };
            // Captured at the member name so the call suffix below can still name the receiver.
            std::string nullIfaceRecvText;
            for (auto parseTree : ctx->children)
            {
                if (childIndex++ >= childLimit) break;
                if (parseTree->getTreeType() == antlr4::tree::ParseTreeType::TERMINAL)
                {
                    auto terminal = dynamic_cast<antlr4::tree::TerminalNode*>(parseTree);
                    auto tokenType = terminal->getSymbol()->getType();
                    // 'move' is now a keyword token; remap it to Identifier handling
                    // so it works as a member name (e.g. File.move(...)).
                    if (tokenType == CFlatParser::Move)
                        tokenType = CFlatParser::Identifier;
                    // Any suffix other than the member name itself consumes the pending member
                    // access ('(' calls it, '[' / '++' / '--' / '.' start a new link).
                    if (tokenType != CFlatParser::Identifier)
                    {
                        danglingMemberName.clear();
                        danglingMemberOwner.clear();
                        danglingIsMethod = false;
                    }
                    switch (tokenType)
                    {
                    case CFlatParser::LeftBracket:
                    case CFlatParser::RightBracket:
                    case CFlatParser::LeftParen:
                    case CFlatParser::RightParen: { prevToken = tokenType; break; }
                    // [PFX-1] member-access operator (. -> ?.): auto-deref the receiver (pointer or
                    // embedded struct) into structVar so the next identifier resolves against it.
                    case CFlatParser::Dot:
                    case CFlatParser::Arrow:
                    case CFlatParser::QuestionDot:
                    {
                        if (namedVar.Primary != nullptr && namedVar.Primary->getType()->isVoidTy())
                            LogErrorContext(ctx, "a void call result cannot be used as a chain receiver");
                        prevToken = tokenType;
                        nullConditionalPending = (tokenType == CFlatParser::QuestionDot);
                        if (!namedVar.TypeAndValue.Pointer
                            && Compiler(ctx)->IsCoreUniqueType(namedVar.TypeAndValue.TypeName)
                            && namedVar.CallerName != "this")
                        {
                            if (!namedVar.FieldName.empty())
                                CheckMovedReceiver(namedVar);
                            if (!nullConditionalPending)
                                Compiler(ctx)->RecordNullDerefFor(namedVar,
                                    ctx->getStart()->getLine(), ctx->getStart()->getCharPositionInLine());
                            if (!nullConditionalPending
                                && Compiler(ctx)->IsExplicitlyMovedNullHere(namedVar))
                                LogErrorContext(ctx, std::format(
                                    "dereference of moved variable '{}' (it is null after the move)",
                                    namedVar.CallerName));
                        }
                        // [PFX-1b] Unadopted owning-POINTER receiver (`makePtr()->v = 1;`): free it
                        // at end-of-expression. SCALAR member only - see MemberIsScalarField.
                        if ((tokenType == CFlatParser::Dot || tokenType == CFlatParser::Arrow)
                            && namedVar.TypeAndValue.Pointer && namedVar.Primary != nullptr)
                        {
                            std::string scalarMember = NextMemberName(ctx, parseTree);
                            if (!scalarMember.empty()
                                && Compiler(ctx)->MemberIsScalarField(
                                       namedVar.TypeAndValue.TypeName, scalarMember))
                                Compiler(ctx)->RegisterOwnedPtrTemp(namedVar.Primary);
                        }
                        // [PFX-1c] blessed core unique<IFace> : forward-on-miss to the owned fat
                        // value. The pointer arm reaches its pointee through operator->; the
                        // interface arm has no pointer to hand out, so unwrap the value in place
                        // and let the normal virtual-dispatch path below run on it.
                        if ((tokenType == CFlatParser::Dot || tokenType == CFlatParser::Arrow
                             || tokenType == CFlatParser::QuestionDot)
                            && !namedVar.TypeAndValue.Pointer
                            && !namedVar.TypeAndValue.IsInterface
                            && Compiler(ctx)->IsCoreUniqueType(namedVar.TypeAndValue.TypeName))
                        {
                            auto* compiler = Compiler(ctx);
                            std::string memberName = NextMemberName(ctx, parseTree);
                            auto sd = compiler->GetDataStructure(namedVar.TypeAndValue.TypeName);
                            bool coreUniqueInterfaceMember = !memberName.empty()
                                && sd.StructType != nullptr && sd.StructFields.size() == 1
                                && sd.StructFields[0].IsFatInterfaceValue()
                                && compiler->HasInterfaceMethod(
                                    sd.StructFields[0].TypeName, memberName);
                            if (!memberName.empty() && sd.StructType != nullptr
                                && sd.StructFields.size() == 1
                                && sd.StructFields[0].IsFatInterfaceValue()
                                && (coreUniqueInterfaceMember
                                    || !compiler->TypeHasMember(namedVar.TypeAndValue.TypeName, memberName)))
                            {
                                if (memberName == "copy" && !coreUniqueInterfaceMember)
                                    compiler->LogUniqueCopyError(namedVar.TypeAndValue.TypeName);
                                llvm::Value* storage = namedVar.Storage;
                                if (storage == nullptr && namedVar.Primary != nullptr)
                                {
                                    storage = compiler->CreateAlloca(sd.StructType);
                                    compiler->CreateAssignment(namedVar.Primary, storage);
                                }
                                if (storage != nullptr)
                                {
                                    auto fieldType = sd.StructFields[0];
                                    fieldType.VariableName.clear();
                                    auto* fieldPtr = compiler->CreateStructGEP(sd.StructType, storage, 0);
                                    LLVMBackend::NamedVariable ifaceNV;
                                    ifaceNV.Storage = fieldPtr;
                                    ifaceNV.Primary = compiler->CreateLoad(fieldPtr);
                                    ifaceNV.BaseType = ifaceNV.Primary->getType();
                                    ifaceNV.TypeAndValue = fieldType;
                                    ifaceNV.TypeAndValue.VariableName = namedVar.TypeAndValue.VariableName;
                                    ifaceNV.CallerName = namedVar.CallerName;
                                    ifaceNV.IsBorrowed = true;
                                    namedVar = ifaceNV;
                                    interfaceVar = namedVar;
                                    structVar = {};
                                }
                            }
                        }
                        // [PFX-1a] user operator-> : forward-on-miss. CFlat's `.` and `->` are flexible
                        // (either token works on a value or a pointer). When the member that follows is
                        // NOT a member of a value-type receiver but that type defines `operator->`, forward
                        // member access to the returned pointer; chain through value wrappers until a raw
                        // pointer (or a wrapper that DOES have the member) is reached. The terminal pointer
                        // then flows through the normal pointer handling below (regular struct or thin-COM
                        // lpVtbl dispatch), so the wrapper composes with existing dispatch. Own members
                        // shadow forwarded ones (the miss test gates forwarding). Applies to `.`, `->`,
                        // and `?.`: for `?.` the `?` is recognized first (nullConditionalPending set above),
                        // then operator-> forwards on a miss and the null-conditional guard applies to the
                        // forwarded pointer.
                        if ((tokenType == CFlatParser::Dot || tokenType == CFlatParser::Arrow
                             || tokenType == CFlatParser::QuestionDot)
                            && !namedVar.TypeAndValue.Pointer
                            && !namedVar.TypeAndValue.IsInterface
                            && !namedVar.TypeAndValue.TypeName.empty()
                            && Compiler(ctx)->GetDataStructure(namedVar.TypeAndValue.TypeName).StructType != nullptr)
                        {
                            auto* compiler = Compiler(ctx);
                            std::string memberName = NextMemberName(ctx, parseTree);
                            int arrowGuard = 0;
                            while (!memberName.empty()
                                   && !namedVar.TypeAndValue.Pointer
                                   && !namedVar.TypeAndValue.TypeName.empty()
                                   && !compiler->TypeHasMember(namedVar.TypeAndValue.TypeName, memberName)
                                   && compiler->HasArrowOverloadFor(namedVar.TypeAndValue.TypeName))
                            {
                                auto sd = compiler->GetDataStructure(namedVar.TypeAndValue.TypeName);
                                if (sd.StructType == nullptr) break;

                                // operator-> takes `this`; materialize a slot if we only hold an rvalue.
                                LLVMBackend::NamedVariable thisNV = namedVar;
                                thisNV.TypeAndValue.VariableName = "";
                                if (thisNV.Storage == nullptr && thisNV.Primary != nullptr)
                                {
                                    auto* temp = compiler->CreateAlloca(sd.StructType);
                                    compiler->CreateAssignment(thisNV.Primary, temp);
                                    thisNV.Storage = temp;
                                }

                                if (!compiler->IsCoreUniqueType(namedVar.TypeAndValue.TypeName))
                                    CheckMovedReceiver(namedVar);
                                auto* arrowResult = compiler->CreateOverloadedFunctionCall("operator->", { thisNV });
                                if (arrowResult == nullptr) break;

                                // operator-> is an ABI adapter, not an ownership boundary: keep the
                                // owning-temp-field ledger on the pointer it hands back.
                                if (compiler->IsLedgeredOwningTempUniqueField(namedVar.Primary))
                                    compiler->RegisterOwningTempUniqueField(arrowResult);
                                bool throughCoreUniqueField =
                                    compiler->IsCoreUniqueType(namedVar.TypeAndValue.TypeName)
                                    && (!namedVar.FieldName.empty() || namedVar.IsUniqueFieldAlias);
                                bool throughPointer = namedVar.FieldPathThroughPointer
                                    || throughCoreUniqueField;
                                std::string pathRoot = namedVar.FieldPathRoot;
                                namedVar = {};
                                namedVar.Primary      = arrowResult;
                                namedVar.BaseType     = arrowResult->getType();
                                namedVar.TypeAndValue = compiler->lastCallReturnType;
                                namedVar.FieldPathThroughPointer = throughPointer;
                                namedVar.FieldPathRoot = pathRoot;
                                PrepareAliasCallResult(ctx, namedVar);

                                if (++arrowGuard > 32)
                                {
                                    LogErrorContext(ctx, "operator-> chain did not resolve to a pointer (possible cycle)");
                                    break;
                                }
                            }
                        }
                        // Total .copy() over a bare-pointer BORROW receiver: the following `copy()` is a
                        // pointer-identity copy (shares the pointee), handled by [PFX-copy-ptr]. The
                        // auto-deref below still runs (a harmless load) and leaves namedVar as the pointer,
                        // so the arm keys off namedVar; this flag only records that the receiver is a
                        // non-owning pointer so a `unique`/owning pointer keeps its actionable error.
                        // A thin encoded closure element joins this arm: it is a bare code pointer
                        // with no struct backing, so its `.copy()` is the same identity copy.
                        if ((tokenType == CFlatParser::Dot || tokenType == CFlatParser::Arrow)
                            && (namedVar.TypeAndValue.Pointer
                                || Compiler(ctx)->IsThinEncodedClosureType(namedVar.TypeAndValue.TypeName))
                            && !namedVar.TypeAndValue.IsInterface
                            && !(namedVar.TypeAndValue.IsUnique
                                 || namedVar.IsOwning)
                            && NextMemberName(ctx, parseTree) == "copy")
                        {
                            // Capture the pointer VALUE now: the auto-deref below leaves namedVar in a
                            // deref'd (non-pointer) shape by the time the call is reached, so [PFX-copy-ptr]
                            // reads this stashed value/type rather than the mutated namedVar.
                            pendingPtrCopyIdentity = true;
                            pendingPtrCopyValue = namedVar.Primary ? namedVar.Primary : LoadNamedVariable(namedVar);
                            pendingPtrCopyType  = namedVar.TypeAndValue;
                        }

                        // For any member access on a pointer to a known struct, load the pointer
                        // so subsequent field/method lookups work. '.' auto-deduces the dereference
                        // just like '->'; '?.' does the same but also arms the null-conditional check.
                        if (namedVar.TypeAndValue.Pointer
                            && !namedVar.TypeAndValue.TypeName.empty()
                            && !namedVar.TypeAndValue.IsInterface)
                        {
                            auto sd = Compiler(ctx)->GetDataStructure(namedVar.TypeAndValue.TypeName);
                            if (sd.StructType)
                            {
                                // Deref of an explicitly-moved-null thin pointer local is statically
                                // null - reject it (plain reads stay legal). SKIP `?.`.
                                if (!nullConditionalPending)
                                    Compiler(ctx)->RecordNullDerefFor(namedVar, ctx->getStart()->getLine(),
                                        ctx->getStart()->getCharPositionInLine());
                                if (Compiler(ctx)->IsExplicitlyMovedNullHere(namedVar) && !nullConditionalPending)
                                    LogErrorContext(ctx, std::format(
                                        "dereference of moved variable '{}' (it is null after the move)",
                                        namedVar.CallerName));
                                llvm::Value* ptrVal = LoadNamedVariable(namedVar);
                                // --sanitize=ownership: guard `p->f` / `p.f` deref against a null
                                // (moved/freed) pointer. SKIP `?.` - the null-conditional operator
                                // legitimately tolerates null and short-circuits, so it is not a bug.
                                if (!nullConditionalPending)
                                    Compiler(ctx)->EmitOwnDerefGuard(namedVar.Storage, ptrVal,
                                        ctx->getStart()->getLine(), ctx->getStart()->getCharPositionInLine());
                                structVar.Storage      = ptrVal;
                                structVar.Primary      = nullptr;
                                structVar.BaseType     = sd.StructType;
                                structVar.TypeAndValue = namedVar.TypeAndValue;
                                structVar.TypeAndValue.Pointer = false;
                                // Preserve borrow-origin across the auto-deref so 'move param->field'
                                // can detect that the parent pointer is a borrowed parameter.
                                structVar.IsBorrowed      = namedVar.IsBorrowed;
                                structVar.BorrowedOrigin  = namedVar.BorrowedOrigin;
                                structVar.FieldPathThroughPointer = namedVar.FieldPathThroughPointer;
                                structVar.FieldPathRoot = namedVar.FieldPathRoot;
                                structVar.ContainsBondedClosure = namedVar.ContainsBondedClosure;
                                structVar.BondedSources = namedVar.BondedSources;
                            }
                        }
                        else if (!namedVar.TypeAndValue.Pointer
                                 && !namedVar.TypeAndValue.TypeName.empty()
                                 && !namedVar.TypeAndValue.IsInterface
                                 && namedVar.Storage != nullptr)
                        {
                            // Embedded struct field (value, not pointer): namedVar.Storage is the GEP
                            // address of the embedded field - use it directly as the struct base so
                            // chained method calls (e.g. w.inbox.send()) pass the correct 'this'.
                            auto sd = Compiler(ctx)->GetDataStructure(namedVar.TypeAndValue.TypeName);
                            if (sd.StructType)
                            {
                                structVar.Storage      = namedVar.Storage;
                                structVar.Primary      = nullptr;
                                structVar.BaseType     = sd.StructType;
                                structVar.TypeAndValue = namedVar.TypeAndValue;
                                structVar.IsBorrowed      = namedVar.IsBorrowed;
                                structVar.BorrowedOrigin  = namedVar.BorrowedOrigin;
                                structVar.IsParenthesizedProducedTemp = namedVar.IsParenthesizedProducedTemp;
                                structVar.TernaryTempAlreadyRegistered = namedVar.TernaryTempAlreadyRegistered;
                                structVar.ContainsBondedClosure = namedVar.ContainsBondedClosure;
                                structVar.BondedSources = namedVar.BondedSources;
                            }
                        }
                        else if (nullConditionalPending
                                 && LLVMBackend::IsHResultType(namedVar.TypeAndValue.TypeName))
                        {
                            // HResult `?.`/`?->` chain: unwrap `.value` (an object pointer) as the
                            // receiver for the next call and arm the failure short-circuit; the call
                            // dispatch reads the source HResult's hr to decide skip-vs-call + propagate.
                            auto* compiler = Compiler(ctx);
                            auto hrSD = compiler->GetDataStructure(namedVar.TypeAndValue.TypeName);
                            if (hrSD.StructType && hrSD.StructFields.size() >= 2)
                            {
                                // An HResult the user bound (local/field/param) carries Storage and stays
                                // the user's to release; only a bare rvalue can be an unowned temporary.
                                bool isTemporary = namedVar.Storage == nullptr && namedVar.Primary != nullptr;
                                llvm::Value* hrMem = namedVar.Storage;
                                if (!hrMem)
                                {
                                    hrMem = compiler->AllocaAtEntry(hrSD.StructType, nullptr, "hrchain");
                                    compiler->builder->CreateStore(
                                        namedVar.Primary ? namedVar.Primary : LoadNamedVariable(namedVar), hrMem);
                                }
                                const auto& valField = hrSD.StructFields[1];
                                auto* valuePtr = compiler->builder->CreateLoad(
                                    compiler->GetType(valField, nullptr, true),
                                    compiler->builder->CreateStructGEP(hrSD.StructType, hrMem, 1));
                                auto valSD = compiler->GetDataStructure(valField.TypeName);
                                structVar.Storage      = valuePtr;
                                structVar.Primary      = nullptr;
                                structVar.BaseType     = valSD.StructType;
                                structVar.TypeAndValue = valField;
                                structVar.TypeAndValue.Pointer = false;
                                hresultChainPending = true;
                                hresultChainStorage = hrMem;
                                hresultChainType    = namedVar.TypeAndValue.TypeName;
                                nullConditionalPending = false;  // the HResult path supersedes null-ptr

                                // An unnamed temporary strands its pointer, so release it - but only
                                // where the +1 is guaranteed: a [winrt] object from a slot [out,retval].
                                hresultChainReleasePtr = nullptr;
                                hresultChainReleaseType.clear();
                                if (isTemporary && namedVar.Primary == lastWinrtSlotCallResult
                                    && valField.Pointer && compiler->IsWinrtClass(valField.TypeName))
                                {
                                    hresultChainReleasePtr  = valuePtr;
                                    hresultChainReleaseType = valField.TypeName;
                                }
                            }
                        }
                        break;
                    }
                    case CFlatParser::Tilde:
                    {
                        // expr.~() - call destructor in place, no memory freed.
                        if (prevToken == CFlatParser::Dot)
                        {
                            auto* compiler = Compiler(ctx);
                            std::string typeName;
                            llvm::Value* thisPtr = nullptr;

                            if (!structVar.TypeAndValue.TypeName.empty() && structVar.Storage)
                            {
                                typeName = structVar.TypeAndValue.TypeName;
                                thisPtr  = structVar.Storage;
                            }
                            else if (!namedVar.TypeAndValue.TypeName.empty())
                            {
                                typeName = namedVar.TypeAndValue.TypeName;
                                thisPtr  = namedVar.TypeAndValue.Pointer
                                               ? LoadNamedVariable(namedVar)
                                               : namedVar.Storage;
                            }

                            if (typeName == "string")
                                compiler->EnsureStringDtorRegistered();

                            if (!typeName.empty() && thisPtr)
                            {
                                // No destructor = no-op (primitive or struct with no cleanup needed).
                                if (auto* dtor = compiler->GetOrCreateFullDestructor(typeName))
                                    compiler->builder->CreateCall(dtor, { thisPtr });
                            }
                            else
                            {
                                LogErrorContext(ctx, "'.~()' requires a named struct type");
                            }
                            namedVar = {};
                            structVar = {};
                        }
                        break;
                    }
                    case CFlatParser::PlusPlus:
                    {
                        if (namedVar.TypeAndValue.IsArrayView)
                            LogErrorContext(ctx, "'++' is not allowed on an array-view 'T[]' - it has no pointer arithmetic; index it with 'a[i]' instead");
                        if (ncChainNullBlock != nullptr)
                        {
                            LogErrorContext(ctx, NullConditionalNotWritable("++"));
                            break;
                        }
                        CheckGuardedWrite(ctx, namedVar);
                        llvm::Value* incrementStorage = namedVar.Storage
                            ? namedVar.Storage : parenthesizedPostfixStorage;
                        if (incrementStorage)
                        {
                            llvm::Type* et = nullptr;
                            if (namedVar.TypeAndValue.Pointer)
                            {
                                auto elemTV = namedVar.TypeAndValue;
                                elemTV.ElemPointer ? (elemTV.ElemPointer = false) : (elemTV.Pointer = false, elemTV.IsInterfacePointer = false);
                                et = Compiler(ctx)->GetType(elemTV);
                            }
                            PlusPlus[incrementStorage].Amount++;
                            PlusPlus[incrementStorage].ElemType = et;
                            PlusPlus[incrementStorage].LoadType = namedVar.UnionFieldType
                                ? namedVar.UnionFieldType
                                : (parenthesizedPostfixStorage ? namedVar.BaseType : nullptr);
                        }
                        else
                        {
                            LogErrorContext(ctx, "'++' requires an addressable operand (a variable, field, element, or dereferenced pointer)");
                        }
                        break;
                    }
                    case CFlatParser::MinusMinus:
                    {
                        if (namedVar.TypeAndValue.IsArrayView)
                            LogErrorContext(ctx, "'--' is not allowed on an array-view 'T[]' - it has no pointer arithmetic; index it with 'a[i]' instead");
                        if (ncChainNullBlock != nullptr)
                        {
                            LogErrorContext(ctx, NullConditionalNotWritable("--"));
                            break;
                        }
                        CheckGuardedWrite(ctx, namedVar);
                        llvm::Value* decrementStorage = namedVar.Storage
                            ? namedVar.Storage : parenthesizedPostfixStorage;
                        if (decrementStorage)
                        {
                            llvm::Type* et = nullptr;
                            if (namedVar.TypeAndValue.Pointer)
                            {
                                auto elemTV = namedVar.TypeAndValue;
                                elemTV.ElemPointer ? (elemTV.ElemPointer = false) : (elemTV.Pointer = false, elemTV.IsInterfacePointer = false);
                                et = Compiler(ctx)->GetType(elemTV);
                            }
                            PlusPlus[decrementStorage].Amount--;
                            PlusPlus[decrementStorage].ElemType = et;
                            PlusPlus[decrementStorage].LoadType = namedVar.UnionFieldType
                                ? namedVar.UnionFieldType
                                : (parenthesizedPostfixStorage ? namedVar.BaseType : nullptr);
                        }
                        else
                        {
                            LogErrorContext(ctx, "'--' requires an addressable operand (a variable, field, element, or dereferenced pointer)");
                        }
                        break;
                    }
                    // [PFX-2] member name after . / -> (or a namespaced name): resolve to a struct
                    // field (GEP/load), else clear namedVar and defer to the call path [PFX-7] as a
                    // member fn / [winrt] vtable slot / UFCS target. Unresolved -> "Unknown identifier".
                    case CFlatParser::Identifier:
                    {
                        // [PFX-2-dangle] Arm for any member name; the branches below refine the
                        // owner / method flag, and the tail of this case disarms on a real field.
                        if (prevToken == CFlatParser::Dot || prevToken == CFlatParser::Arrow
                            || prevToken == CFlatParser::QuestionDot)
                            danglingMemberName = terminal->getText();

                        if (!namespaceContext.empty())
                        {
                            // "$global$:<alias>" sentinel: file-scoped import alias.
                            // Resolve Alias.Symbol -> Symbol only if Symbol is a member of that alias.
                            bool isFileAlias = namespaceContext.starts_with("$global$:");
                            std::string memberName = terminal->getText();
                            std::string qualifiedName;
                            if (isFileAlias)
                            {
                                std::string aliasName = namespaceContext.substr(9);
                                qualifiedName = Compiler(ctx)->IsImportAliasMember(aliasName, memberName)
                                    ? memberName
                                    : namespaceContext + "." + memberName;  // will fail -> error
                            }
                            else
                            {
                                qualifiedName = namespaceContext + "." + memberName;
                            }
                            if (Compiler(ctx)->IsNamespace(qualifiedName))
                            {
                                namespaceContext = Compiler(ctx)->ResolveNamespace(qualifiedName);
                                primaryIdentifier = namespaceContext;
                                namedVar = {};
                            }
                            else
                            {
                                if (!isFileAlias && Compiler(ctx)->IsDataStructure(qualifiedName)
                                    && Compiler(ctx)->GetLocalVariable(memberName).Storage == nullptr
                                    && Compiler(ctx)->GetFunctionArgument(memberName).GetValue() == nullptr)
                                {
                                    // A namespace-qualified aggregate is a type qualifier for a
                                    // following static member call (N.S.f()), not a namespace value.
                                    namespaceContext = qualifiedName;
                                    primaryIdentifier = qualifiedName;
                                    namedVar = {};
                                    structVar = {};
                                    interfaceVar = {};
                                    break;
                                }
                                // Qualified name (e.g. EnumName.Member, Cfg.W) - try to resolve
                                // as a global variable (enum member / namespace global) or a
                                // function. Fall back to leaving namedVar empty so later code
                                // can handle it (constructor calls, generic templates, ...).
                                // An enum member: rebind the owner to the enum's registered
                                // namespace-scoped key so `Dir.Back` reads the enclosing Dir.
                                if (auto dot = qualifiedName.rfind('.'); dot != std::string::npos)
                                {
                                    std::string owner = qualifiedName.substr(0, dot);
                                    std::string enumKey = Compiler(ctx)->ResolveEnumTypeName(owner);
                                    if (!enumKey.empty() && enumKey != owner)
                                        qualifiedName = enumKey + qualifiedName.substr(dot);
                                }
                                primaryIdentifier = qualifiedName;
                                bool wasRealNamespace = !isFileAlias && Compiler(ctx)->IsNamespace(namespaceContext);
                                std::string namespaceName = namespaceContext;
                                namespaceContext.clear();

                                auto globalNV = Compiler(ctx)->GetGlobalVariableNV(primaryIdentifier);
                                if (globalNV.Storage != nullptr)
                                {
                                    // Reaching into a namespace does not bypass its guard group.
                                    CheckGlobalGuard(ctx, primaryIdentifier, globalNV);
                                    namedVar = globalNV;
                                }
                                else if (Compiler(ctx)->GetFunction(primaryIdentifier))
                                {
                                    namedVar.Primary = Compiler(ctx)->GetFunctionForFuncPtr(primaryIdentifier);
                                    namedVar.CallerName = primaryIdentifier;
                                }
                                else
                                {
                                    namedVar = {};
                                    // A member of a real namespace that is not a global, a
                                    // function, a type, a generic template, or a return-block
                                    // cannot resolve later - error now instead of silently
                                    // evaluating to undef.
                                    if (wasRealNamespace
                                        && !Compiler(ctx)->IsDataStructure(qualifiedName)
                                        && genericFunctionTemplates.count(qualifiedName) == 0
                                        && genericStructTemplates.count(qualifiedName) == 0
                                        && genericClassTemplates.count(qualifiedName) == 0
                                        && Compiler(ctx)->GetReturnBlock(qualifiedName) == nullptr)
                                    {
                                        LogErrorContext(ctx, std::format(
                                            "'{}' is not a member of namespace '{}'.",
                                            memberName, namespaceName));
                                    }
                                }
                            }
                        }
                        else if (interfaceVar.TypeAndValue.IsInterface)
                        {
                            // A name on an interface receiver: an interface FIELD resolves to an
                            // lvalue here (data ptr + the vtable's byte-offset slot); anything else
                            // is a method name, recorded for dispatch at the call site below.
                            primaryIdentifier = terminal->getText();
                            namedVar = {};
                            nullIfaceRecvText = ReceiverSourceText();

                            auto* compiler = Compiler(ctx);
                            // Deref of a moved interface local (method receiver or interface field)
                            // is null - record it for the cross-block MAY-null fixpoint. SKIP '?.'.
                            if (!nullConditionalPending)
                                compiler->RecordNullDerefFor(interfaceVar, ctx->getStart()->getLine(),
                                    ctx->getStart()->getCharPositionInLine());
                            if (compiler->IsExplicitlyMovedNullHere(interfaceVar) && !nullConditionalPending)
                                LogErrorContext(ctx, std::format(
                                    "dereference of moved variable '{}' (it is null after the move)",
                                    interfaceVar.CallerName));
                            const std::string& ifaceName = interfaceVar.TypeAndValue.TypeName;
                            int ifaceFieldIdx = compiler->InterfaceFieldIndex(ifaceName, primaryIdentifier);
                            if (ifaceFieldIdx >= 0 && !interfaceVar.TypeAndValue.IsInterfacePointer)
                            {
                                // A FRESH load off the slot is emitted here and now, so it can anchor
                                // the definitely-null proof.
                                llvm::LoadInst* freshFatLoad = interfaceVar.Primary != nullptr
                                    ? nullptr
                                    : compiler->CreateLoad(compiler->GetFatPtrType(), interfaceVar.Storage);
                                llvm::Value* fatVal = interfaceVar.Primary != nullptr
                                    ? interfaceVar.Primary : freshFatLoad;

                                // A parenthesised receiver (`(lv).tag`) leaves the fat value already
                                // in Primary. That earlier load anchors the proof only when it reads
                                // THIS slot in THIS block; the access consumes the loaded value, so
                                // stores after it cannot change what faults. A load from an EARLIER
                                // block is not anchored - this block's stores are the wrong evidence.
                                llvm::LoadInst* anchorFatLoad = freshFatLoad;
                                if (anchorFatLoad == nullptr && interfaceVar.Storage != nullptr)
                                {
                                    auto* priorLoad = llvm::dyn_cast<llvm::LoadInst>(interfaceVar.Primary);
                                    if (priorLoad != nullptr
                                        && priorLoad->getPointerOperand() == interfaceVar.Storage
                                        && priorLoad->getType() == compiler->GetFatPtrType()
                                        && priorLoad->getParent() == compiler->builder->GetInsertBlock())
                                        anchorFatLoad = priorLoad;
                                }

                                // Definitely-null member access: same slot proof as the method
                                // dispatch below (RunNullIfaceDispatchCheck). Covers the write form
                                // too - `lv.tag = 5` shares this one lvalue. '?.' is the sanctioned
                                // spelling for a maybe-null receiver and is never recorded.
                                if (!nullConditionalPending && anchorFatLoad != nullptr)
                                {
                                    LLVMBackend::NullIfaceDispatchSite fldSite;
                                    fldSite.VarName = interfaceVar.CallerName.empty()
                                        ? interfaceVar.TypeAndValue.VariableName : interfaceVar.CallerName;
                                    fldSite.ReceiverText = nullIfaceRecvText;
                                    fldSite.MemberName = primaryIdentifier;
                                    fldSite.IsField = true;
                                    fldSite.Line = (int)ctx->getStart()->getLine();
                                    fldSite.Col = (int)ctx->getStart()->getCharPositionInLine();
                                    compiler->RecordPendingNullIfaceDispatch(
                                        fldSite, interfaceVar.Storage, anchorFatLoad, ifaceName);
                                }

                                // '?.' on an interface FIELD: the address is data + vtable[slot], so
                                // BOTH halves must be live or the offset load itself faults.
                                if (nullConditionalPending)
                                {
                                    auto* vtPtr = compiler->builder->CreateExtractValue(fatVal, { 0u }, "nc_iface_vtable");
                                    auto* dtPtr = compiler->builder->CreateExtractValue(fatVal, { 1u }, "nc_iface_data");
                                    auto* live = compiler->builder->CreateAnd(
                                        compiler->builder->CreateICmpNE(vtPtr, llvm::Constant::getNullValue(vtPtr->getType())),
                                        compiler->builder->CreateICmpNE(dtPtr, llvm::Constant::getNullValue(dtPtr->getType())),
                                        "nc_iface_live");
                                    ncEnterGuard(live);
                                }

                                const auto& fieldType = (*compiler->GetInterfaceFields(ifaceName))[ifaceFieldIdx];
                                auto* fieldLLVMType = compiler->GetType(fieldType);
                                auto* addr = compiler->EmitInterfaceFieldAddress(
                                    fatVal, ifaceName, primaryIdentifier, fieldLLVMType);
                                namedVar.Storage  = addr;
                                namedVar.BaseType = fieldLLVMType;
                                namedVar.Primary  = llvm::isa<llvm::ArrayType>(fieldLLVMType)
                                    ? nullptr
                                    : compiler->CreateLoad(fieldLLVMType, addr);
                                namedVar.TypeAndValue = fieldType;
                                namedVar.TypeAndValue.ParentVariableName = interfaceVar.TypeAndValue.VariableName;
                                namedVar.OwningStructName = ifaceName;
                                namedVar.FieldName        = primaryIdentifier;
                                namedVar.IsInterfaceField = true;
                            }
                            else if (compiler->HasInterfaceMethod(ifaceName, primaryIdentifier))
                            {
                                // Not an interface field but a contract method: dispatched at the
                                // call below. Arm [PFX-2-dangle] in case no '()' follows.
                                danglingMemberOwner = ifaceName;
                                danglingIsMethod    = true;
                            }
                        }
                        else if (structVar.BaseType)
                        {
                            primaryIdentifier = terminal->getText();
                            auto dataStructure = Compiler(ctx)->GetDataStructure(llvm::dyn_cast<llvm::StructType>(structVar.BaseType));

                            // [PFX-2a] Consumed-COM member sugar: on a thin COM interface pointer - a struct
                            // whose SOLE field `lpVtbl` points at a vtable of function-pointer slots - a name
                            // that is not a field but IS a vtable slot routes through the vtable, so
                            // `recv->Method(args)` means `recv->lpVtbl->Method(args)`. Covers both winmd- and
                            // header-imported COM (the vtable struct comes from the actual `lpVtbl` field, not a
                            // name convention). The single-field shape is what distinguishes a consumed thin
                            // interface from a produce-side `[winrt] class` object (lpVtbl + refcount + fields),
                            // which keeps its own receiver-injecting dispatch. Redirect structVar to the
                            // dereferenced vtable so the slot resolves as a fn-ptr field below and dispatches.
                            if (auto* compiler = Compiler(ctx);
                                primaryIdentifier != "lpVtbl" && structVar.Storage && dataStructure.StructType
                                && dataStructure.StructFields.size() == 1
                                && dataStructure.StructFields[0].VariableName == "lpVtbl")
                            {
                                std::string vtblName = dataStructure.StructFields[0].TypeName;
                                auto vtblData = compiler->GetDataStructure(vtblName);
                                bool isSlot = false;
                                if (vtblData.StructType)
                                    for (const auto& f : vtblData.StructFields)
                                        if (f.VariableName == primaryIdentifier && f.IsFunctionPointer)
                                        { isSlot = true; break; }
                                if (isSlot)
                                {
                                    // Capture the object pointer (the `this`) BEFORE overwriting
                                    // Storage with the vtable pointer, so the call can inject it.
                                    pendingThinComReceiver = structVar.Storage;
                                    auto* vtblPtr = compiler->builder->CreateLoad(
                                        cflat_llvm::PointerTo(vtblData.StructType),
                                        compiler->CreateStructGEP(structVar.BaseType, structVar.Storage, 0));
                                    structVar.Storage      = vtblPtr;
                                    structVar.Primary      = nullptr;
                                    structVar.BaseType     = vtblData.StructType;
                                    structVar.TypeAndValue = {};
                                    structVar.TypeAndValue.TypeName = vtblName;
                                    dataStructure = vtblData;
                                }
                            }

                            // Bitfield access path: side-table lookup, GEP to the storage word,
                            // emit shift+mask for the read, remember enough on the NamedVariable
                            // that write-side codegen can do a read-modify-write later.
                            const LLVMBackend::BitfieldInfo* bfHit = nullptr;
                            for (const auto& b : dataStructure.Bitfields)
                                if (b.Name == primaryIdentifier) { bfHit = &b; break; }
                            if (bfHit && !structVar.Storage)
                            {
                                LogErrorContext(ctx, std::format(
                                    "bitfield '{}' has no addressable storage in this context", primaryIdentifier));
                            }
                            if (bfHit && structVar.Storage)
                            {
                                auto* compiler = Compiler(ctx);
                                const auto& storageField = dataStructure.StructFields[bfHit->StorageFieldIndex];
                                auto* storageTy = compiler->GetType(storageField);
                                auto* storagePtr = compiler->CreateStructGEP(structVar.BaseType, structVar.Storage, bfHit->StorageFieldIndex);
                                // Shared read/write masking lives in EmitBitfieldAccess (single source
                                // of truth; the transparent anonymous-member path uses it too).
                                namedVar = EmitBitfieldAccess(compiler, storagePtr, storageTy, *bfHit,
                                                              structVar.TypeAndValue.VariableName,
                                                              structVar.TypeAndValue.TypeName);
                                continue;
                            }

                            uint32_t fieldIndex = 0;

                            for (const auto& field : dataStructure.StructFields)
                            {
                                if (field.VariableName == primaryIdentifier)
                                {
                                    break;
                                }
                                fieldIndex++;
                            }

                            if (fieldIndex < dataStructure.StructFields.size())
                            {
                                const auto& fieldType = dataStructure.StructFields[fieldIndex];

                                // Lock-set check: if this field is guarded, verify the lock is held.
                                if (!fieldType.GuardedBy.empty())
                                {
                                    std::string receiverName = structVar.TypeAndValue.VariableName;
                                    if (!receiverName.empty())
                                    {
                                        std::string requiredLock = receiverName + "." + fieldType.GuardedBy;
                                        if (currentLockSet.find(requiredLock) == currentLockSet.end())
                                        {
                                            LogErrorContext(ctx, std::format(
                                                "Field '{}' is guarded by '{}': must hold '{}' before accessing it.",
                                                primaryIdentifier, fieldType.GuardedBy, requiredLock));
                                        }
                                    }
                                }

                                // Cross-thread sharing scan (--xthread-scan N): report when a
                                // field of a type seen escaping a thread spawn is accessed and is
                                // neither atomic nor lock-guarded. Information gathering only; never
                                // errors. Prints an [xthread] line to stdout (not the diag sink).
                                Compiler(ctx)->ReportXthreadFieldAccess(
                                    structVar.TypeAndValue.VariableName, primaryIdentifier,
                                    structVar.TypeAndValue.TypeName, fieldType);

                                if (nullConditionalPending && structVar.Storage != nullptr)
                                {
                                    // An ARRAY field must stay Storage-only here (no load), same as
                                    // the non-guarded arm below - a later '[i]' subscript needs the GEP.
                                    ncEnterGuard(structVar.Storage);

                                    auto* fieldLLVMType = Compiler(ctx)->GetType(fieldType);
                                    if (dataStructure.IsUnion)
                                    {
                                        namedVar.Storage = structVar.Storage;  // union: all fields at offset 0
                                        namedVar.UnionFieldType = fieldLLVMType;
                                        if (llvm::isa<llvm::ArrayType>(fieldLLVMType))
                                        {
                                            namedVar.Primary = nullptr;
                                            namedVar.BaseType = fieldLLVMType;
                                        }
                                        else
                                        {
                                            namedVar.Primary = Compiler(ctx)->CreateLoad(fieldLLVMType, namedVar.Storage);
                                            namedVar.BaseType = namedVar.Primary->getType();
                                        }
                                    }
                                    else
                                    {
                                        namedVar.UnionFieldType = nullptr;
                                        namedVar.Storage = Compiler(ctx)->CreateStructGEP(structVar.BaseType, structVar.Storage, fieldIndex);
                                        if (llvm::isa<llvm::ArrayType>(fieldLLVMType))
                                        {
                                            namedVar.Primary = nullptr;
                                            namedVar.BaseType = fieldLLVMType;
                                        }
                                        else
                                        {
                                            namedVar.Primary = Compiler(ctx)->CreateLoad(namedVar.Storage);
                                            namedVar.BaseType = namedVar.Primary->getType();
                                        }
                                    }
                                    namedVar.TypeAndValue = fieldType;
                                    namedVar.TypeAndValue.ParentVariableName = structVar.TypeAndValue.VariableName;
                                    namedVar.ContainsBondedClosure = structVar.ContainsBondedClosure
                                        && IsBondedClosureContainer(Compiler(ctx), fieldType);
                                    if (namedVar.ContainsBondedClosure)
                                        namedVar.BondedSources = structVar.BondedSources;
                                    if (structVar.ContainsBondedClosure
                                        && (fieldType.IsFunctionPointer
                                            || Compiler(ctx)->GetEncodedClosureType(fieldType.TypeName) != nullptr))
                                    {
                                        namedVar.IsBonded = true;
                                        namedVar.BondedSources = structVar.BondedSources;
                                        if (namedVar.Primary != nullptr)
                                            Compiler(ctx)->RegisterBondedValue(namedVar.Primary,
                                                                                namedVar.BondedSources);
                                    }
                                    namedVar.IsAliasBorrow = namedVar.IsAliasBorrow
                                        || structVar.IsAliasBorrow
                                        || (structVar.TypeAndValue.IsAlias && !structVar.TypeAndValue.Pointer);
                                    if (structVar.TypeAndValue.IsAlias && !structVar.TypeAndValue.Pointer
                                        && !structVar.IsAliasBorrow
                                        && !structVar.RootIsAliasBorrowLocal)
                                        namedVar.FromOwningTempField = true;
                                }
                                else
                                {
                                    if (structVar.Storage)
                                    {
                                        auto* fieldLLVMType = Compiler(ctx)->GetType(fieldType);
                                        if (dataStructure.IsUnion)
                                        {
                                            // Union: all fields alias at offset 0. Store raw alloca pointer
                                            // and record the field type so derefLoad/derefAssign use it.
                                            namedVar.Storage = structVar.Storage;
                                            namedVar.UnionFieldType = fieldLLVMType;
                                            if (llvm::isa<llvm::ArrayType>(fieldLLVMType))
                                            {
                                                namedVar.Primary = nullptr;
                                                namedVar.BaseType = fieldLLVMType;
                                            }
                                            else
                                            {
                                                namedVar.Primary = Compiler(ctx)->CreateLoad(fieldLLVMType, namedVar.Storage);
                                                namedVar.BaseType = namedVar.Primary->getType();
                                            }
                                        }
                                        else
                                        {
                                            // Not a union field: clear any inherited UnionFieldType from a parent
                                            // union access in the chain (e.g. union.structField.subField).
                                            namedVar.UnionFieldType = nullptr;
                                            namedVar.Storage = Compiler(ctx)->CreateStructGEP(structVar.BaseType, structVar.Storage, fieldIndex);
                                            if (llvm::isa<llvm::ArrayType>(fieldLLVMType))
                                            {
                                                // Array field: keep GEP pointer; don't load the whole array
                                                namedVar.Primary = nullptr;
                                                namedVar.BaseType = fieldLLVMType;
                                            }
                                            else
                                            {
                                                namedVar.Primary = Compiler(ctx)->CreateLoad(namedVar.Storage);
                                                namedVar.BaseType = namedVar.Primary->getType();
                                            }
                                        }
                                    }
                                    else if (structVar.Primary)
                                    {
                                        namedVar.Storage = nullptr;
                                        namedVar.UnionFieldType = nullptr;
                                        // Unions with no backing storage can't reinterpret inline values.
                                        if (!dataStructure.IsUnion)
                                            namedVar.Primary = Compiler(ctx)->CreateExtractValue(structVar.Primary, fieldIndex);
                                        namedVar.BaseType = namedVar.Primary ? namedVar.Primary->getType() : nullptr;

                                        // Field of a by-value owning-struct temp (`makeToken().text`): tag it (persisting
                                        // double-frees), and destruct the temp when it OWNS the buffer (a `move` or plain
                                        // owning return) but NOT for an `alias` borrow (`get()`), which would double-free.
                                        const std::string& parentType = structVar.TypeAndValue.TypeName;
                                        bool parentIsOwningTemp = !dataStructure.IsUnion
                                            && !parentType.empty()
                                            && parentType != "string"
                                            && parentType != "__closure_fat_ptr"
                                            && Compiler(ctx)->IsOwningValueType(parentType);
                                        // Owns the temp's buffer unless it is an `alias` (borrow) return.
                                        bool parentOwnsTemp = parentIsOwningTemp
                                            && !structVar.TypeAndValue.IsAlias
                                            // Preserve an owning origin across nested fields, but
                                            // keep an alias-container origin non-owning at every hop.
                                            && (!structVar.FromOwningTempField || structVar.OwningTempParent);
                                        // Records WHO owns the field for the temp-source diagnostics:
                                        // the temp itself, or whatever an `alias` return borrowed from.
                                        namedVar.OwningTempParent = parentOwnsTemp;
                                        if (parentOwnsTemp && !structVar.FromOwningTempField)
                                        {
                                            auto* tempAlloca = Compiler(ctx)->AllocaAtEntry(structVar.BaseType, nullptr, "owntemp");
                                            Compiler(ctx)->builder->CreateStore(structVar.Primary, tempAlloca);
                                            Compiler(ctx)->RegisterOwnedStructTemp(tempAlloca, parentType);
                                            // The temp owns this field: let a persist site move it out (store + zero
                                            // the source) rather than reject. Only an owning field can be moved.
                                            if (Compiler(ctx)->IsOwningValueType(fieldType.TypeName))
                                            {
                                                namedVar.MovableTempField = true;
                                                namedVar.MoveTempStructAlloca = tempAlloca;
                                                namedVar.MoveTempStructType = structVar.BaseType;
                                                namedVar.MoveTempFieldIndex = fieldIndex;
                                            }
                                        }
                                        if (parentIsOwningTemp || structVar.FromOwningTempField)
                                        {
                                            namedVar.FromOwningTempField = true;
                                            if (namedVar.Primary != nullptr && !namedVar.OwningTempParent)
                                                Compiler(ctx)->RegisterTempFieldValue(namedVar.Primary);
                                            // A blessed unique<X> field VALUE is owning by its type.
                                            // Ledger the read so a later '?:' / '??' join still
                                            // carries provenance the joined value cannot.
                                            if (namedVar.Primary != nullptr && !fieldType.Pointer
                                                && Compiler(ctx)->IsCoreUniqueType(fieldType.TypeName))
                                            {
                                                Compiler(ctx)->RegisterOwningTempUniqueField(namedVar.Primary);
                                                Compiler(ctx)->RegisterTempFieldValue(namedVar.Primary);
                                            }
                                        }
                                    }
                                    namedVar.TypeAndValue = fieldType;
                                    namedVar.TypeAndValue.ParentVariableName = structVar.TypeAndValue.VariableName;
                                    namedVar.ContainsBondedClosure = structVar.ContainsBondedClosure
                                        && IsBondedClosureContainer(Compiler(ctx), fieldType);
                                    if (namedVar.ContainsBondedClosure)
                                        namedVar.BondedSources = structVar.BondedSources;
                                    if (structVar.ContainsBondedClosure
                                        && (fieldType.IsFunctionPointer
                                            || Compiler(ctx)->GetEncodedClosureType(fieldType.TypeName) != nullptr))
                                    {
                                        namedVar.IsBonded = true;
                                        namedVar.BondedSources = structVar.BondedSources;
                                        if (namedVar.Primary != nullptr)
                                            Compiler(ctx)->RegisterBondedValue(namedVar.Primary,
                                                                                namedVar.BondedSources);
                                    }
                                    namedVar.IsAliasBorrow = namedVar.IsAliasBorrow
                                        || structVar.IsAliasBorrow
                                        || (structVar.TypeAndValue.IsAlias && !structVar.TypeAndValue.Pointer);
                                    if (structVar.TypeAndValue.IsAlias && !structVar.TypeAndValue.Pointer
                                        && !structVar.IsAliasBorrow
                                        && !structVar.RootIsAliasBorrowLocal)
                                        namedVar.FromOwningTempField = true;
                                    // Mark this NamedVariable as a struct-field access so 'delete obj->field'
                                    // can reject it when invoked outside the owning struct's own methods.
                                    namedVar.OwningStructName = structVar.TypeAndValue.TypeName;
                                    namedVar.FieldName        = primaryIdentifier;
                                    namedVar.FieldPathText = structVar.FieldPathText.empty()
                                        ? structVar.TypeAndValue.VariableName + "." + primaryIdentifier
                                        : structVar.FieldPathText + "." + primaryIdentifier;
                                    // Carry the path's ROOT variable forward: on `w.a.b` the parent
                                    // of `.b` is the field `a`, whose own root is still `w`.
                                    namedVar.FieldPathRoot = structVar.FieldPathRoot.empty()
                                        ? structVar.TypeAndValue.VariableName : structVar.FieldPathRoot;
                                    namedVar.FieldPathThroughPointer = structVar.FieldPathThroughPointer
                                        || structVar.TypeAndValue.Pointer;
                                    // An alias-return binding can retain the source owner's path label
                                    // while its own alloca is being introduced. Re-resolve the first
                                    // field hop against the local binding so the shallow-copy borrow
                                    // does not masquerade as the original owner.
                                    const auto* rootBinding = Compiler(ctx)->FindVariableByStorage(
                                        structVar.Storage);
                                    bool rootIsAliasLocal = (structVar.TypeAndValue.IsAlias
                                            && !structVar.TypeAndValue.Pointer
                                            && !structVar.CallerName.empty())
                                        || (rootBinding != nullptr
                                            && IsAliasBorrowLocalBinding(*rootBinding));
                                    if (rootIsAliasLocal)
                                    {
                                        namedVar.FieldPathRoot = structVar.CallerName.empty()
                                            ? structVar.TypeAndValue.VariableName : structVar.CallerName;
                                    }
                                    // Settle the borrowed-parameter question HERE, against the
                                    // RESOLVED parent binding, and record it (see FieldPathRoot).
                                    namedVar.RootIsBorrowedByValueParam = structVar.FieldPathRoot.empty()
                                        ? IsBorrowedByValueParamBinding(Compiler(ctx), structVar)
                                        : structVar.RootIsBorrowedByValueParam;
                                    // A pointer LOCAL holding `&param` roots at the parameter too;
                                    // the binding carries the fact the storage walk cannot see.
                                    // The base's own NamedVariable carries it when the storage
                                    // lookup misses (a loaded pointer has no alloca to key on).
                                    if (structVar.PointsToBorrowedByValueParam
                                        || (rootBinding != nullptr
                                            && rootBinding->PointsToBorrowedByValueParam))
                                        namedVar.RootIsBorrowedByValueParam = true;
                                    namedVar.PointsToBorrowedByValueParam =
                                        structVar.PointsToBorrowedByValueParam;
                                    // Same shape for an `alias`-BORROW local root, settled here for
                                    // the same reason (a downstream lookup cannot see a shadow).
                                    namedVar.RootIsAliasBorrowLocal = rootIsAliasLocal
                                        || (structVar.FieldPathRoot.empty()
                                            ? IsAliasBorrowLocalBinding(structVar)
                                            : structVar.RootIsAliasBorrowLocal);
                                    // A cast off this read (`(Res*)b.p`, `free((void*)b.p)`) severs
                                    // Storage and rewrites the type; carry the unique provenance so
                                    // the borrow rules still fire (see IsUniqueFieldRead / Trap B).
                                    if ((fieldType.IsUnique
                                            && fieldType.Pointer)
                                        || (Compiler(ctx)->IsCoreUniqueType(fieldType.TypeName)
                                            && !fieldType.Pointer))
                                        namedVar.IsUniqueFieldAlias = true;
                                    // Ledger the read by VALUE so a cast or a join, which drop every
                                    // flag above, still answer the persist-site guard.
                                    if (DeclaredOwningTempUniqueFieldRead(namedVar))
                                        Compiler(ctx)->RegisterOwningTempUniqueField(namedVar.Primary);
                                    // Field declared `alignas(_, N)`: stamp the block alignment onto the
                                    // result so `delete obj->field` / scope-exit free via __delete_aligned.
                                    namedVar.AllocAlignment   = fieldType.AllocAlignValue;
                                    // Propagate borrow-origin so 'move param->field' can detect
                                    // the parent pointer is a borrowed parameter.
                                    namedVar.IsBorrowed       = structVar.IsBorrowed;
                                    namedVar.BorrowedOrigin   = structVar.BorrowedOrigin;
                                    // A direct read is the source identity for an implicit move.
                                    // Joins carry this ledger forward so only the selected arm is nulled.
                                    if (IsUniqueFieldRead(namedVar))
                                        Compiler(ctx)->RegisterUniqueFieldRead(
                                            namedVar.Primary, namedVar.Storage);
                                }
                            }
                            // The generic-template leg resolves the bare spelling to its namespace's
                            // key: this gate decides whether the extension-method dispatch is reached.
                            else if (Compiler(ctx)->GetFunction(primaryIdentifier)
                                     || genericFunctionTemplates.count(Compiler(ctx)->ResolveGenericFunctionBase(primaryIdentifier))
                                     || !GenericMethodTemplateKey(structVar.TypeAndValue.TypeName, primaryIdentifier).empty()
                                     || (primaryIdentifier == "toFunction" && structVar.TypeAndValue.TypeName == "__closure_fat_ptr")
                                     || Compiler(ctx)->GetWinrtSlot(structVar.TypeAndValue.TypeName, primaryIdentifier))
                            {
                                // Not a field - a member function, an extension method template, the
                                // Lambda<T>.toFunction() builtin, or a [winrt] COM vtable slot (e.g.
                                // AddRef/Release/QueryInterface). All are lowered at the call dispatch below.
                                namedVar = {};
                                // Arm [PFX-2-dangle]: without a following '()' this is `obj.method`.
                                danglingMemberOwner = structVar.TypeAndValue.TypeName;
                                if (danglingMemberOwner.empty())
                                    if (auto* st = llvm::dyn_cast<llvm::StructType>(structVar.BaseType))
                                        danglingMemberOwner = st->getName().str();
                                danglingIsMethod = true;
                            }
                            else if (LLVMBackend::NamedVariable anonNV;
                                     ResolveTransparentAnonField(ctx, structVar, primaryIdentifier, anonNV))
                            {
                                // C11 transparent anonymous-member access: the field lives inside an
                                // anonymous (synthetic "__anonN") struct/union member of the base
                                // (e.g. LARGE_INTEGER's LowPart/HighPart). Resolved as if written
                                // base.__anonN.field.
                                namedVar = anonNV;
                            }
                            else
                            {
                                LogErrorContext(primaryCtx, std::format("Unknown identifier '{}'.", primaryIdentifier));
                            }
                        }
                        else if (prevToken == CFlatParser::Dot && [&]() -> bool {
                            // Named variables (alloca-backed): TypeName is reliable.
                            if (namedVar.TypeAndValue.IsFloatingPoint() >= 0) return true;
                            // Inline float expressions (e.g. (-2.5f)): TypeName is empty,
                            // but Primary holds the unloaded LLVM float value.
                            if (namedVar.Primary != nullptr
                                && namedVar.Primary->getType()->isFloatingPointTy()) return true;
                            // Integer conversion methods - matched by name so they work on any
                            // integer-typed base (named var, inline literal, call result).
                            static const std::unordered_set<std::string> intConvert = {
                                "to_i8","to_u8","to_i16","to_u16","to_i32","to_u32","to_i64","to_u64"
                            };
                            if (intConvert.count(terminal->getText()) > 0) return true;
                            // UFCS: a primitive (integer-like, incl. bool) base calling a free
                            // function by name - e.g. n.toString(), n.toString(16). The base value
                            // stays in namedVar and is pushed as the self argument when '()' is
                            // dispatched below. Float bases are already covered above.
                            // An enum is integer-backed, so it supports the same UFCS/method dispatch
                            // as its backing type - e.g. `enumVal.copy()` routes to the copy choke point.
                            bool baseIsEnum = !namedVar.TypeAndValue.TypeName.empty()
                                && !Compiler(ctx)->GetEnumBackingType(namedVar.TypeAndValue.TypeName).empty();
                            bool baseIsIntegerLike =
                                namedVar.TypeAndValue.IsInteger() >= 0
                                || namedVar.TypeAndValue.TypeName == "bool"
                                || baseIsEnum
                                || (namedVar.Primary != nullptr
                                    && namedVar.Primary->getType()->isIntegerTy());
                            if (baseIsIntegerLike && Compiler(ctx)->GetFunction(terminal->getText()))
                                return true;
                            // A thin encoded closure element is a bare code pointer with no struct
                            // backing, so a free function on it (`.copy()`) is the same UFCS call.
                            if (Compiler(ctx)->IsThinEncodedClosureType(namedVar.TypeAndValue.TypeName)
                                && Compiler(ctx)->GetFunction(terminal->getText()))
                                return true;
                            return false;
                        }())
                        {
                            // Method name on a primitive float/double, an integer conversion, or a
                            // free-function UFCS call (e.g. f.round(), (-2.5f).abs(), x.to_i32(),
                            // n.toString()). Record the method name; the base value stays in namedVar.
                            // Actual dispatch happens when '()' is processed below.
                            primaryIdentifier = terminal->getText();
                            // Arm [PFX-2-dangle]: the base value stays in namedVar, so without a
                            // following '()' the method name would be silently dropped.
                            danglingMemberOwner = namedVar.TypeAndValue.TypeName;
                            danglingIsMethod = true;
                        }
                        else
                        {
                            namedVar = ParseIdentifier(terminal);
                        }

                        if (namedVar.TypeAndValue.IsInterface)
                        {
                            interfaceVar = namedVar;
                            structVar = {};
                        }
                        else if (namedVar.BaseType && namedVar.BaseType->isStructTy())
                        {
                            structVar = namedVar;
                            interfaceVar = {};
                        }
                        else if (namedVar.BaseType && llvm::isa<llvm::ArrayType>(namedVar.BaseType))
                        {
                            // Fixed-array field: clear structVar so the subscript handler does not
                            // mistake the parent struct as the operator[] target.
                            structVar = {};
                        }
                        else if (namedVar.BaseType != nullptr || namedVar.Primary != nullptr)
                        {
                            // Member resolved to a concrete non-struct value (e.g. a primitive
                            // field `s.x`). Clear structVar/interfaceVar so a following UFCS or
                            // method call binds the field VALUE as the receiver rather than the
                            // parent struct left over from the previous chain link.
                            structVar = {};
                            interfaceVar = {};
                        }
                        receiverWasFixedArray = hasFixedArrayShape(namedVar);

                        // [PFX-2-dangle] Disarm when the name resolved to a real value (a field, a
                        // namespace global, an enum member, ...); a confirmed method name stays armed.
                        if (!danglingIsMethod
                            && (namedVar.Primary != nullptr || namedVar.Storage != nullptr
                                || namedVar.BaseType != nullptr || !namespaceContext.empty()))
                        {
                            danglingMemberName.clear();
                            danglingMemberOwner.clear();
                        }

                        break;
                    }
                    }
                }
                else if (parseTree->getTreeType() == antlr4::tree::ParseTreeType::RULE)
                {
                    auto ruleContext = dynamic_cast<antlr4::RuleContext*>(parseTree);
                    auto ruleID = ruleContext->getRuleIndex();
                    switch (ruleID)
                    {
                    case CFlatParser::RulePrimaryExpression:
                    {
                        auto prevPrimary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(parseTree);
                        std::optional<CallArgumentScope> receiverTernaryScope;
                        if (childLimit > 1
                            && (ctx->children[1]->getText() == "."
                                || ctx->children[1]->getText() == "->"
                                || ctx->children[1]->getText() == "?."))
                            receiverTernaryScope.emplace(inCallArgument_, ternaryCallArgumentDepth_);

                        // `global::name` scope-escape qualifier: resolve the following name at the
                        // ROOT (file/global) scope, bypassing the enclosing-namespace lookup. The
                        // leading token must literally be `global` (a contextual soft keyword).
                        if (prevPrimary->DoubleColon() != nullptr)
                        {
                            auto* compiler = Compiler(ctx);
                            auto* leadTok = prevPrimary->Identifier();
                            if (leadTok == nullptr || leadTok->getText() != "global")
                            {
                                LogErrorContext(prevPrimary, "expected 'global' before '::' scope qualifier");
                                namedVar = {};
                                structVar = {};
                                break;
                            }

                            auto* gid = prevPrimary->genericIdentifier();
                            std::string rootName = gid->Identifier()->getText();

                            if (compiler->IsNamespace(rootName))
                            {
                                // global::Outer.Inner.x - seed the namespace context at root; the
                                // following `.member` postfix builds a dotted (root-anchored) name,
                                // which ResolveQualifiedName never prepends the current namespace to.
                                namespaceContext = compiler->ResolveNamespace(rootName);
                                primaryIdentifier = namespaceContext;
                                namedVar = {};
                                structVar = {};
                                interfaceVar = {};
                                break;
                            }

                            // Dot-less root symbol: a global variable, or a function (value or call).
                            primaryIdentifier = rootName;
                            namedVar = {};
                            auto globalNV = compiler->GetGlobalVariableNV(rootName);
                            if (globalNV.Storage != nullptr)
                            {
                                namedVar = globalNV;
                            }
                            else if (compiler->GetFunction(rootName))
                            {
                                namedVar.Primary = compiler->GetFunctionForFuncPtr(rootName);
                                namedVar.CallerName = rootName;
                                globalScopeCall = true;
                            }
                            else
                            {
                                LogErrorContext(prevPrimary, std::format("Undefined global symbol '{}'.", rootName));
                            }
                            structVar = {};
                            interfaceVar = {};
                            break;
                        }

                        // If the primary is a generic instantiation (e.g. Box<MyInt>),
                        // map it to its mangled constructor name (e.g. Box$MyInt).
                        // Apply type substitutions for generic parameters.
                        if (prevPrimary->genericIdentifier() != nullptr && prevPrimary->genericIdentifier()->genericTypeParameters() != nullptr && prevPrimary->genericIdentifier()->Identifier() != nullptr)
                        {
                            std::string baseName = prevPrimary->genericIdentifier()->Identifier()->getText();
                            // A BARE call inside a namespace must reach its own namespace's
                            // generic-function key before a same-named global one.
                            std::string gfKey = Compiler(ctx)->ResolveGenericFunctionBase(baseName);
                            bool isGenericFunc = genericFunctionTemplates.count(gfKey) != 0;
                            std::string mangledName = baseName;
                            std::vector<std::string> typeArgs;
                            for (auto* entry : prevPrimary->genericIdentifier()->genericTypeParameters()->typeParameterList()->typeParameterEntry())
                            {
                                if (TypeArgHasUnique(entry))
                                    LogErrorContext(entry, "unique is not supported as an explicit generic function type argument");
                                typeArgs.push_back(ResolveTypeArgEntry(entry));
                            }
                            mangledName = MangleGenericInstance(*Compiler(), baseName, typeArgs);
                            bool isFollowedByCall = false;
                            for (size_t i = 0; i + 1 < ctx->children.size(); i++)
                            {
                                if (ctx->children[i] != parseTree) continue;
                                auto* next = dynamic_cast<antlr4::tree::TerminalNode*>(ctx->children[i + 1]);
                                isFollowedByCall = next != nullptr && next->getSymbol()->getType() == CFlatParser::LeftParen;
                                break;
                            }
                            // If this is a generic function template (e.g. wrap<int>),
                            // instantiate it and record the mangled name as CallerName so
                            // ParseDeclaration can resolve the correct function pointer.
                            if (isGenericFunc)
                            {
                                const auto& typeParams = Compiler(ctx)->gts.genericFunctionTypeParams[gfKey];
                                auto packIt = Compiler(ctx)->gts.genericFunctionPackIndex.find(gfKey);
                                size_t packIdx = packIt != Compiler(ctx)->gts.genericFunctionPackIndex.end()
                                    ? packIt->second : std::string::npos;
                                // Omitted TRAILING value arguments come from the declared defaults
                                // before the arity is judged.
                                if (packIdx == std::string::npos)
                                    FillGenericValueDefaults(*Compiler(ctx), gfKey,
                                                             typeParams.size(), typeArgs);
                                bool wrongArity = packIdx == std::string::npos
                                    ? typeParams.size() != typeArgs.size()
                                    : typeArgs.size() < packIdx;
                                if (isFollowedByCall && wrongArity)
                                    LogErrorContext(prevPrimary, std::format(
                                        "generic function '{}' expects {} type argument(s), but the call provides {}",
                                        baseName, typeParams.size(), typeArgs.size()));
                                mangledName = MangledGenericName(gfKey, typeArgs);
                                InstantiateGenericFunction(gfKey, typeArgs);
                                primaryIdentifier = mangledName;
                                callDisplayName = prevPrimary->getText();
                                namedVar = {};
                                namedVar.CallerName = mangledName;
                                break;
                            }
                            // An instantiated generic type used as a static-member qualifier
                            // (`Holder<i64>.sget<int>(x)`): seed the namespace context with the MANGLED
                            // name, since that is what the instantiation's members are registered under.
                            // Gated on a following '.' so a constructor temp (`Holder<i64>().m()`) keeps
                            // its receiver and dispatches as an instance call.
                            if ((genericClassTemplates.count(baseName) || genericStructTemplates.count(baseName))
                                && IsFollowedByDot(ctx, parseTree))
                            {
                                namespaceContext = mangledName;
                                primaryIdentifier = mangledName;
                                namedVar = {};
                                structVar = {};
                                interfaceVar = {};
                                break;
                            }
                            if (!genericClassTemplates.count(baseName) && !genericStructTemplates.count(baseName)
                                && isFollowedByCall)
                                LogErrorContext(prevPrimary, std::format("unknown generic function '{}'", baseName));
                            primaryIdentifier = mangledName;
                            namedVar = {};
                            break;
                        }

                        primaryIdentifier = prevPrimary->getText();

                        if (prevPrimary->genericIdentifier() != nullptr && prevPrimary->genericIdentifier()->Identifier() != nullptr)
                        {
                            std::string idName = prevPrimary->genericIdentifier()->Identifier()->getText();
                            if (Compiler(ctx)->IsNamespace(idName))
                            {
                                namespaceContext = Compiler(ctx)->ResolveNamespace(idName);
                                namedVar = {};
                                structVar = {};
                            }
                            else if (Compiler(ctx)->IsDataStructure(
                                         Compiler(ctx)->ResolveQualifiedName(prevPrimary->getText()))
                                     && Compiler(ctx)->GetLocalVariable(idName).Storage == nullptr
                                     && Compiler(ctx)->GetFunctionArgument(idName).GetValue() == nullptr)
                            {
                                // Type name used as qualifier for static method access: ClassName.Method()
                                // Constructor calls (ClassName()) still work because functionName = "ClassName".
                                namespaceContext = Compiler(ctx)->ResolveQualifiedName(prevPrimary->getText());
                                namedVar = {};
                                structVar = {};
                            }
                            else
                            {
                                namedVar.Primary = ParsePrimaryExpression(prevPrimary);
                                namedVar.Storage = nullptr;
                                if (namedVar.Primary == nullptr)
                                    namedVar = ParseIdentifier(prevPrimary->genericIdentifier()->Identifier());
                            }
                        }
                        else
                        {
                            // An IMMEDIATELY-INVOKED lambda literal takes its return type from how
                            // THIS call's result is used, not from an enclosing literal: a discarded
                            // invocation declares void, a value context declares the destination
                            // type. Without this the inner literal inherits lambdaExpectedType and
                            // is misdiagnosed as missing a return / returning from void.
                            // Installed only when this postfix actually supplies a context, so a
                            // primary that publishes lambdaExpectedType as a side-channel to the
                            // suffix walk is left alone.
                            std::optional<LambdaExpectedTypeRestoreGuard> immediateCallRestore;
                            if (directCallWhole)
                            {
                                if (use == ResultUse::Discard)
                                {
                                    immediateCallRestore.emplace(&lambdaExpectedType);
                                    lambdaExpectedType = {};
                                    lambdaExpectedType.FuncPtrReturnTypeName = "void";
                                }
                                else if (!declExpectedType.TypeName.empty())
                                {
                                    immediateCallRestore.emplace(&lambdaExpectedType);
                                    LLVMBackend::TypeAndValue ctxTv;
                                    // Ownership rides the synthesized signature: an owning or
                                    // aliasing destination must reach the literal's return type,
                                    // not just its spelling.
                                    ctxTv.FuncPtrReturnTypeName = declExpectedType.TypeName;
                                    ctxTv.FuncPtrReturnPointer  = declExpectedType.Pointer;
                                    ctxTv.FuncPtrReturnPointerDepth = declExpectedType.ValuePointerDepth();
                                    // A `unique` destination is an owning LOCATION; the return
                                    // spelling that fills it is `move`, so both map to Owned.
                                    ctxTv.FuncPtrReturnOwned    = declExpectedType.IsMove
                                                               || declExpectedType.IsUnique
                                                               || (!declExpectedType.Pointer
                                                                   && Compiler(ctx)->IsCoreUniqueType(
                                                                       declExpectedType.TypeName));
                                    ctxTv.FuncPtrReturnAlias    = declExpectedType.IsAlias;
                                    lambdaExpectedType = ctxTv;
                                }
                            }
                            // A parenthesized primary that IS the whole postfix expression keeps the
                            // caller's position; a suffix (`(g())(x)`, `(p).f`) consumes it as a value.
                            namedVar.Primary = ParsePrimaryExpression(
                                prevPrimary, childLimit == 1 ? use : ResultUse::Value);
                            namedVar.Storage = nullptr;
                            if (auto* literal = prevPrimary->Constant())
                            {
                                auto literalType = ParseLiteralTypeAndValue(literal->getText());
                                if (!literalType.TypeName.empty())
                                    namedVar.TypeAndValue = literalType;
                            }
                            // `default` takes the destination's type - publish it so a following
                            // suffix and the caller both see the right type, not an empty one.
                            if (prevPrimary->Default() != nullptr && !declExpectedType.TypeName.empty())
                                namedVar.TypeAndValue = declExpectedType;
                            // A STRING LITERAL followed by `.method()` is an ordinary `string`
                            // receiver: wrap the global into a {ptr,len} `string` and give it a
                            // temp slot, the same materialization the two-statement workaround
                            // (`string t = "abc"; t.length()`) performs. The wrapper points at a
                            // global constant, so the temp owns NOTHING and needs no destruction.
                            if (childLimit > 1 && ctx->children[1]->getText() == "."
                                && namedVar.Primary != nullptr
                                && namedVar.TypeAndValue.TypeName.empty())
                            {
                                auto* compilerP = Compiler(ctx);
                                bool isStringLiteral = prevPrimary->StringLiteral().size() == 1;
                                if (!isStringLiteral && prevPrimary->expression() != nullptr)
                                {
                                    if (auto* literalValue = llvm::dyn_cast<llvm::Constant>(namedVar.Primary))
                                        isStringLiteral = compilerP->stringLiteralLenByPtr.count(literalValue) != 0;
                                }
                                if (isStringLiteral)
                                {
                                    if (auto* strTy = llvm::StructType::getTypeByName(
                                            *compilerP->context, "string"))
                                    {
                                        llvm::Value* strVal = namedVar.Primary->getType() == strTy
                                            ? namedVar.Primary
                                            : compilerP->WrapStringLiteralAsString(namedVar.Primary);
                                        auto* tmp = compilerP->CreateAlloca(strTy);
                                        compilerP->builder->CreateStore(strVal, tmp);
                                        namedVar.Primary  = strVal;
                                        namedVar.Storage  = tmp;
                                        namedVar.BaseType = strTy;
                                        namedVar.TypeAndValue = {};
                                        namedVar.TypeAndValue.TypeName = "string";
                                    }
                                }
                            }
                            // If the primary is a parenthesized cast expression, propagate its type
                            // so that chained member access (e.g. ((Struct*)ptr)->field) works.
                            // The <Tag> element sugar uses the same channel to publish its node
                            // pointer type (Tag*) so interface boxing and member access resolve.
                            if ((prevPrimary->expression() != nullptr || prevPrimary->elementExpression() != nullptr)
                                && !lastParenExprType.TypeName.empty())
                                namedVar.TypeAndValue = lastParenExprType;
                            if (prevPrimary->elementExpression() != nullptr)
                                AdoptWrapperProvenance(namedVar, lastParenExprNamed);
                            // A parenthesized lvalue keeps its storage so a STORE writes through to
                            // the object ('(*p) = 9'). Postfix '(*p)++' is still a no-op - see
                            // internal/issue/p2/paren-deref-increment-is-a-silent-no-op.md.
                            if (prevPrimary->expression() != nullptr)
                            {
                                namedVar.Storage = lastParenExprStorage;
                                parenthesizedPostfixStorage = namedVar.Storage;
                            }
                            // Parentheses change the spelling, never the value: hand the ownership
                            // arms the SAME provenance the bare operand would have carried.
                            if (prevPrimary->expression() != nullptr)
                                AdoptWrapperProvenance(namedVar, lastParenExprNamed);
                            // Restore the owning-temp provenance only when the inner expression
                            // carried it, so no other parenthesized shape gains names it lacked.
                            if (prevPrimary->expression() != nullptr && lastParenExprFromOwningTempField)
                            {
                                namedVar.FromOwningTempField = true;
                                namedVar.OwningTempParent = lastParenExprOwningTempParent;
                                namedVar.OwningStructName = lastParenExprOwningStructName;
                                namedVar.FieldName = lastParenExprFieldName;
                                namedVar.CallerName = lastParenExprCallerName;
                            }
                            lastParenExprType = {};
                            lastParenExprStorage = nullptr;
                            lastParenExprFromOwningTempField = false;
                            lastParenExprOwningTempParent = false;
                            lastParenExprOwningStructName.clear();
                            lastParenExprFieldName.clear();
                            lastParenExprCallerName.clear();
                            auto parenInnerNamed = lastParenExprNamed;
                            lastParenExprNamed = {};
                            // A redundant inner paren already spilled the temp and proved the flag;
                            // carry it out, or `((makeBox())).m()` leaks the receiver.
                            bool parenthesizedProducedTemp = prevPrimary->expression() != nullptr
                                && (parenInnerNamed.IsParenthesizedProducedTemp
                                || (parenInnerNamed.Storage == nullptr
                                    && parenInnerNamed.Primary != nullptr
                                    && Compiler(ctx)->IsProducedTempValue(parenInnerNamed.Primary)
                                    && !parenInnerNamed.TypeAndValue.Pointer
                                    && !parenInnerNamed.TypeAndValue.IsAlias
                                    && !parenInnerNamed.FromOwningTempField));

                            // An expression result can carry its named struct type only in LLVM.
                            // Recover it before materializing so method dispatch sees the receiver.
                            if (prevPrimary->expression() != nullptr
                                && namedVar.TypeAndValue.TypeName.empty()
                                && namedVar.Primary != nullptr)
                            {
                                if (auto* st = llvm::dyn_cast<llvm::StructType>(namedVar.Primary->getType());
                                    st != nullptr && st->hasName())
                                {
                                    std::string typeName = st->getName().str();
                                    if (Compiler(ctx)->IsDataStructure(typeName))
                                    {
                                        namedVar.TypeAndValue.TypeName = typeName;
                                        namedVar.BaseType = st;
                                    }
                                }
                            }

                            // A parenthesized expression yielding a known struct publishes its type
                            // and (for an lvalue) its storage through the side channel above, but
                            // never its BaseType - recover it here so the struct-receiver branch
                            // below picks the primary up. Without it a parenthesized struct has no
                            // structVar, which the subscript suffix reads: `(*p)[i]` found no
                            // operator[] and fell through to the raw-index path with a null BaseType.
                            // By-VALUE only (e.g. `((string)buf)`): the value lives in Primary with no
                            // addressable storage, so a following `.method()` could not establish a
                            // receiver and fell back to treating the primary text as a function name
                            // ("unknown function '((string)buf)'"). Materialize it into a temp alloca -
                            // matching the two-statement `string t = (string)buf; t.copy()` workaround.
                            // An lvalue (`(*p)`) already has storage; keep it so writes land in the
                            // pointee rather than a copy.
                            if (namedVar.Primary != nullptr
                                && namedVar.BaseType == nullptr
                                && !namedVar.TypeAndValue.Pointer
                                && !namedVar.TypeAndValue.TypeName.empty())
                            {
                                auto sd = Compiler(ctx)->GetDataStructure(namedVar.TypeAndValue.TypeName);
                                if (sd.StructType != nullptr
                                    && namedVar.Primary->getType() == sd.StructType)
                                {
                                    namedVar.BaseType = sd.StructType;
                                    if (namedVar.Storage == nullptr)
                                    {
                                        auto* tmp = Compiler(ctx)->CreateAlloca(sd.StructType);
                                        Compiler(ctx)->builder->CreateStore(namedVar.Primary, tmp);
                                        namedVar.Storage = tmp;
                                    }
                                }
                            }
                            if (parenthesizedProducedTemp)
                            {
                                namedVar.IsParenthesizedProducedTemp = true;
                                namedVar.TernaryTempAlreadyRegistered = parenInnerNamed.TernaryTempAlreadyRegistered;
                            }
                            // A parenthesized POINTER lvalue keeps its element type too: the
                            // delete-retire null store pairs Storage with BaseType, and without it
                            // `delete (r)` left the local live and freed it a second time at exit.
                            if (prevPrimary->expression() != nullptr
                                && namedVar.BaseType == nullptr
                                && namedVar.Storage != nullptr
                                && namedVar.Storage == parenInnerNamed.Storage)
                                namedVar.BaseType = parenInnerNamed.BaseType;
                        }

                        // If the primary was a lambda, propagate its function-pointer type.
                        if (prevPrimary->lambdaExpression() != nullptr)
                        {
                            namedVar.TypeAndValue = lastLambdaType;
                            namedVar.LambdaCaptureNames = Compiler(ctx)->lastCallLambdaCaptureNames;
                            namedVar.LambdaReferenceCaptureNames =
                                Compiler(ctx)->lastLambdaReferenceCaptureNames;
                            lastLambdaType = {};
                            Compiler(ctx)->lastCallLambdaCaptureNames.clear();
                            Compiler(ctx)->lastLambdaReferenceCaptureNames.clear();
                        }

                        if (namedVar.TypeAndValue.IsInterface)
                        {
                            interfaceVar = namedVar;
                            structVar = {};
                        }
                        else if (namedVar.BaseType && namedVar.BaseType->isStructTy())
                        {
                            structVar = namedVar;
                            interfaceVar = {};
                        }
                        else if (!namedVar.TypeAndValue.TypeName.empty() && namedVar.TypeAndValue.Pointer
                                 && Compiler(ctx)->GetDataStructure(namedVar.TypeAndValue.TypeName).StructType != nullptr)
                        {
                            // Opaque pointer to a known struct (e.g. string* a) - track as structVar
                            // so member call dispatch can dereference it correctly.
                            structVar = namedVar;
                            interfaceVar = {};
                        }
                        else if (!namedVar.Storage)
                        {
                            structVar = {};
                            interfaceVar = {};
                        }
                        receiverWasFixedArray = hasFixedArrayShape(namedVar);

                        break;
                    }
                    // [PFX-3] subscript `[expr]`: array / pointer / simd / span fast path / user operator[].
                    case CFlatParser::RuleExpression:
                    {
                        // Bracket [] operation

                        auto expressCtx = dynamic_cast<CFlatParser::ExpressionContext*>(ruleContext);
                        // Acquire the index via the named variant so we keep its declared
                        // integer signedness (u8/u16/u32 -> unsigned). ParseExpression drops
                        // the unsigned flag, which matters when the index is widened to the
                        // pointer width below: LLVM treats GEP indices as SIGNED, so a narrow
                        // unsigned index (e.g. (u8)b with b >= 128) would sign-extend to a
                        // negative offset. Single evaluation - do not also call ParseExpression.
                        LLVMBackend::TypeAndValue indexExpectedType;
                        indexExpectedType.TypeName = "int";
                        DeclExpectedTypeScope indexExpectedScope(&declExpectedType, indexExpectedType);
                        auto idxNamed = ParseAssignmentExpressionNamed(expressCtx->assignmentExpression());
                        bool idxIsUnsigned = idxNamed.TypeAndValue.IsUnsignedInteger() != -1;
                        auto rvalue = LoadNamedVariable(idxNamed);
                        ProcessPlusPlus();

                        // span<T> noalias fast path: lower `y[i]` to the field subscript `y._ptr[i]`.
                        // span::operator[] reaches the buffer through `this`, so every span's element
                        // access collapses to one origin and two distinct spans never prove disjoint at
                        // -O2 (the documented footgun that forces `T[] yv = y.data()` first). Indexing the
                        // `_ptr` array-view field directly keys the element's alias scope to the RECEIVER
                        // (y vs x) via ParentVariableName, exactly as the local-`T[]` form does, restoring
                        // the contract for the natural `y[i]`. Matched structurally - a struct with an
                        // IsArrayView field named `_ptr` - so the may-alias sibling view<T> (whose `_ptr`
                        // is a raw T*, IsArrayView=false) is correctly excluded and keeps method dispatch.
                        // Requires addressable receiver storage; an rvalue span falls through to the
                        // operator[] call (no stable origin to key, so no metadata is lost).
                        if (rvalue && structVar.BaseType && structVar.BaseType->isStructTy())
                        {
                            int bufIndex = Compiler(ctx)->ArrayViewBufferFieldIndex(structVar.TypeAndValue.TypeName);
                            if (bufIndex >= 0 && structVar.Storage != nullptr)
                            {
                                // Bind the StructData to a named ref first (lifetime-extends the
                                // by-value temporary); indexing it inline would dangle through
                                // vector::operator[] (a call defeats lifetime extension).
                                const auto& spanDS = Compiler(ctx)->GetDataStructure(structVar.TypeAndValue.TypeName);
                                const auto& bufField = spanDS.StructFields[bufIndex];
                                namedVar = {};
                                namedVar.Storage = Compiler(ctx)->CreateStructGEP(structVar.BaseType, structVar.Storage, (uint32_t)bufIndex);
                                namedVar.Primary = Compiler(ctx)->CreateLoad(namedVar.Storage);
                                namedVar.BaseType = namedVar.Primary->getType();
                                namedVar.TypeAndValue = bufField;
                                namedVar.TypeAndValue.ParentVariableName = structVar.TypeAndValue.VariableName;
                                namedVar.OwningStructName = structVar.TypeAndValue.TypeName;
                                namedVar.FieldName = "_ptr";
                                // namedVar is now the `_ptr` array-view (Pointer=true): the operator[]
                                // guard below skips and the IsArrayView subscript branch tags the noalias
                                // scope from the receiver origin.
                            }
                            else if (bufIndex >= 0 && currentVectorizeBodyLine_ != 0)
                            {
                                // Span subscript with no addressable receiver (an rvalue span): it falls
                                // through to the operator[] method call, which can't carry noalias. Record
                                // it so a surviving runtime alias check names this site (Detection A).
                                Compiler(ctx)->NoteVectorizeSpanAccessor(currentVectorizeBodyLine_, "operator[]",
                                    structVar.TypeAndValue.VariableName,
                                    (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine());
                            }
                        }

                        // If the base is a struct value with a user-defined operator[],
                        // dispatch to it (member call with 'this' as first arg).
                        if (rvalue && !namedVar.TypeAndValue.Pointer
                            && structVar.BaseType && structVar.BaseType->isStructTy()
                            && Compiler(ctx)->GetFunction("operator[]"))
                        {
                            LLVMBackend::NamedVariable thisNV = structVar;
                            thisNV.TypeAndValue.VariableName = "";
                            if (!thisNV.TypeAndValue.Pointer && thisNV.Storage == nullptr && thisNV.Primary != nullptr)
                            {
                                auto* temp = Compiler(ctx)->CreateAlloca(structVar.BaseType);
                                Compiler(ctx)->CreateAssignment(thisNV.Primary, temp);
                                thisNV.Storage = temp;
                            }

                            LLVMBackend::NamedVariable idxNV;
                            idxNV.Primary  = rvalue;
                            idxNV.BaseType = rvalue->getType();

                            CheckMovedReceiver(structVar);
                            auto* result = Compiler(ctx)->CreateOverloadedFunctionCall("operator[]", { thisNV, idxNV });
                            if (result)
                            {
                                namedVar.Primary  = result;
                                namedVar.Storage  = nullptr;
                                namedVar.BaseType = result->getType();
                                namedVar.TypeAndValue = Compiler(ctx)->lastCallReturnType;
                                PrepareAliasCallResult(ctx, namedVar);
                                if (namedVar.TypeAndValue.IsInterface)
                                {
                                    // operator[] returned an interface fat-ptr - expose as interfaceVar
                                    // so subsequent member accesses dispatch via vtable.
                                    interfaceVar = namedVar;
                                    structVar = {};
                                }
                                else if (result->getType()->isStructTy())
                                {
                                    if (auto* st = llvm::dyn_cast<llvm::StructType>(result->getType()))
                                        if (!st->isLiteral() && st->hasName())
                                            namedVar.TypeAndValue.TypeName = st->getName().str();
                                    structVar = namedVar;
                                    interfaceVar = {};
                                }
                                else if (!namedVar.TypeAndValue.TypeName.empty() && namedVar.TypeAndValue.Pointer
                                         && Compiler(ctx)->GetDataStructure(namedVar.TypeAndValue.TypeName).StructType != nullptr)
                                {
                                    structVar = namedVar;
                                    interfaceVar = {};
                                }
                                else
                                {
                                    structVar = {};
                                    interfaceVar = {};
                                }
                                break;
                            }
                        }

                        if (!(rvalue && rvalue->getType()->isIntegerTy()))
                        {
                            LogErrorContext(expressCtx, "Expecting be an integer type.");
                        }

                        // Remember the BASE's view-ness before the element branches clear it; the
                        // element slot of a user `T[]` view is LIVE storage, unlike a container's.
                        bool baseWasArrayView = namedVar.TypeAndValue.IsArrayView;

                        // Widen the index to the pointer width (i64) with the extension that
                        // matches its declared signedness. LLVM treats GEP indices as signed
                        // and would implicitly sign-extend a narrow index; an unsigned narrow
                        // index (u8/u16/u32) whose top bit is set must zero-extend instead, or
                        // it becomes a negative/huge offset (e.g. (u8)200 -> -56). Reached only
                        // by the built-in array/pointer/simd subscripts below; a user operator[]
                        // overload breaks out above with the original-width index.
                        if (rvalue && rvalue->getType()->isIntegerTy() && !rvalue->getType()->isIntegerTy(64))
                        {
                            auto* idxI64Ty = Compiler(ctx)->builder->getInt64Ty();
                            rvalue = idxIsUnsigned
                                ? Compiler(ctx)->builder->CreateZExt(rvalue, idxI64Ty, "idxzext")
                                : Compiler(ctx)->builder->CreateSExt(rvalue, idxI64Ty, "idxsext");
                        }

                        // An inline array FIELD read off a by-value temporary (`mk().vals[i]`) is an
                        // extractvalue - a register with no address to GEP into. Spill it to a local so
                        // the element access below has a base, matching C's rule that the returned temp
                        // has automatic storage to the end of the full expression. The copy is SHALLOW,
                        // so tag the storage: 'move' and stores through it stay rejected.
                        if (namedVar.Storage == nullptr && namedVar.Primary != nullptr
                            && namedVar.BaseType && llvm::isa<llvm::ArrayType>(namedVar.BaseType))
                        {
                            auto* spill = Compiler(ctx)->CreateAlloca(namedVar.BaseType);
                            Compiler(ctx)->CreateAssignment(namedVar.Primary, spill);
                            namedVar.Storage = spill;
                            namedVar.IsTempSpillStorage = true;
                        }

                        if (namedVar.BaseType && namedVar.BaseType->isVectorTy())
                        {
                            // simd<T,N> lane read: load the whole vector, extractelement. This yields
                            // a value (not an addressable lvalue), so it flows through Primary.
                            auto* vecTy = llvm::cast<llvm::FixedVectorType>(namedVar.BaseType);
                            llvm::Value* vecVal = LoadNamedVariable(namedVar);
                            namedVar.Primary = Compiler(ctx)->builder->CreateExtractElement(vecVal, rvalue, "simdlane");
                            namedVar.Storage = nullptr;
                            namedVar.BaseType = vecTy->getElementType();
                            // TypeName already holds the element scalar (e.g. "float"); drop the simd-ness.
                            namedVar.TypeAndValue.IsSimd = false;
                            namedVar.TypeAndValue.SimdLanes = 0;
                        }
                        // A base with no resolved type cannot be indexed. Guard before the dyn_cast:
                        // it asserts on a null operand, so an unhandled primary shape used to abort
                        // the compiler with no diagnostic instead of pointing at the source.
                        else if (namedVar.BaseType == nullptr)
                        {
                            LogErrorContext(expressCtx,
                                "cannot apply '[]' here - the type of the indexed expression is unknown.");
                            namedVar = {};
                            structVar = {};
                            break;
                        }
                        // A null-Storage base with no addressable backing would build the GEPs below
                        // on a null pointer and crash. Split by shape: an inline array field of a
                        // by-value temporary vs. any other non-indexable temporary (scalar, fat
                        // pointer, or a struct rvalue that reached here with no operator[] match).
                        else if (namedVar.Storage == nullptr && llvm::isa<llvm::ArrayType>(namedVar.BaseType))
                        {
                            LogErrorContext(expressCtx,
                                "'[]' requires an addressable source (field or local) - an inline array "
                                "field of a temporary value has no storage. Bind the call result to a "
                                "local first, then index it.");
                            namedVar = {};
                            structVar = {};
                            break;
                        }
                        else if (namedVar.Storage == nullptr && !namedVar.TypeAndValue.Pointer)
                        {
                            LogErrorContext(expressCtx,
                                "'[]' requires an addressable source (field or local) - cannot apply "
                                "'[]' to a non-indexable temporary value.");
                            namedVar = {};
                            structVar = {};
                            break;
                        }
                        else if (auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(namedVar.BaseType))
                        {
                            // Fixed-size array (char buf[N] or char* words[N]): two-index GEP {0, i}.
                            // Check BaseType first so pointer-element arrays (char*[N]) don't fall
                            // into the pointer-arithmetic branch below.
                            llvm::Value* zero = Compiler(ctx)->builder->getInt64(0);
                            llvm::Value* elemPtr = Compiler(ctx)->builder->CreateGEP(
                                arrTy, namedVar.Storage, {zero, rvalue}, "arrayelemptr");
                            // When the array lives in a GLOBAL, the base pointer is a constant, so a
                            // constant index (e.g. g[0], whose all-zero GEP is a no-op) folds the GEP
                            // back into a Constant - the GlobalVariable itself or a ConstantExpr. Such
                            // a result loses element-type provenance: every consumer that recovers the
                            // type from storage (GetTypeFromStorage, used by load and store) would then
                            // see the whole [N x T] aggregate, not the element type. Force a real GEP
                            // instruction so the element type is recoverable, matching how a local
                            // array (alloca base, never folded) already behaves.
                            if (llvm::isa<llvm::Constant>(elemPtr))
                                elemPtr = Compiler(ctx)->builder->Insert(
                                    llvm::GetElementPtrInst::CreateInBounds(
                                        arrTy, namedVar.Storage, {zero, rvalue}, "arrayelemptr"));
                            namedVar.Storage = elemPtr;
                            namedVar.BaseType = arrTy->getElementType();
                            namedVar.TypeAndValue.ConstArraySize = 0;
                            namedVar.FieldPathThroughPointer = true;
                            // The GEP above already resolved the union reinterpret; leaving the
                            // whole FIELD type set would load/store the element as the whole array.
                            namedVar.UnionFieldType = nullptr;
                        }
                        else if (namedVar.TypeAndValue.Pointer)
                        {
                            namedVar.FieldPathThroughPointer = true;
                            // Indexing through a pointer (e.g. char* p; p[i]).
                            auto elementTypeAndValue = namedVar.TypeAndValue;
                            // Array-view element access: tag the element's load/store with this view's
                            // alias scope so the optimizer can prove distinct views are disjoint
                            // (dropping the runtime overlap check) even when the view rides inside a
                            // by-value struct field, where the `noalias` parameter attribute cannot
                            // reach. Key the scope by the view's stable origin (the struct instance
                            // for a field view `s.v[i]`, else the variable/param name) so every access
                            // to the same buffer shares a scope and copies are not falsely disjoint.
                            if (namedVar.TypeAndValue.IsArrayView)
                            {
                                std::string originKey = namedVar.TypeAndValue.ParentVariableName;
                                if (originKey.empty()) originKey = namedVar.TypeAndValue.VariableName;
                                if (originKey.empty()) originKey = namedVar.CallerName;
                                elementTypeAndValue.NoaliasScopeId = Compiler(ctx)->GetOrMintViewScope(originKey);
                            }
                            // An indexed element is a single slot, never a whole-allocation view -
                            // drop the array-view flag so `&a[i]` cannot be bound back into a `T[]`.
                            elementTypeAndValue.IsArrayView = false;
                            // Indexing strips one level, so a recorded depth must step down with it -
                            // left alone, a `T**` buffer's element falsely proves depth 2.
                            if (elementTypeAndValue.PointerDepth >= 1)
                                elementTypeAndValue.PointerDepth--;
                            if (elementTypeAndValue.ElemPointer)
                            {
                                // Double-pointer (e.g. T* where T=Employee*): element type is T* (Employee*).
                                // Keep Pointer=true, clear ElemPointer - element is a pointer value.
                                elementTypeAndValue.ElemPointer = false;
                            }
                            else
                            {
                                elementTypeAndValue.Pointer = false;
                                // For interface arrays (T* where T=IFace), the element is a bare fat-ptr
                                // {i8*,i8*}, not a pointer-to-fat-ptr. Clear IsInterfacePointer so
                                // GetType returns {i8*,i8*} instead of {i8*,i8*}*.
                                elementTypeAndValue.IsInterfacePointer = false;
                            }
                            auto elementType = Compiler(ctx)->GetType(elementTypeAndValue);
                            llvm::Value* subBaseStorage = namedVar.Storage;
                            // An indexed deref (`p[i]`) of an explicitly-moved-null thin pointer local
                            // is statically null - reject it, same as the '->'/'.'/'*' guards.
                            Compiler(ctx)->RecordNullDerefFor(namedVar, ctx->getStart()->getLine(),
                                ctx->getStart()->getCharPositionInLine());
                            if (Compiler(ctx)->IsExplicitlyMovedNullHere(namedVar))
                                LogErrorContext(ctx, std::format(
                                    "dereference of moved variable '{}' (it is null after the move)",
                                    namedVar.CallerName));
                            auto ptrValue = LoadNamedVariable(namedVar);
                            // --sanitize=ownership (M1): guard `p[i]` deref of a moved-from local.
                            Compiler(ctx)->EmitOwnDerefGuard(subBaseStorage, ptrValue,
                                ctx->getStart()->getLine(), ctx->getStart()->getCharPositionInLine());
                            namedVar.Storage = Compiler(ctx)->CreateGEP(elementType, ptrValue, rvalue);
                            namedVar.BaseType = elementType;
                            namedVar.TypeAndValue = elementTypeAndValue;
                        }
                        else
                        {
                            namedVar.Storage = Compiler(ctx)->CreateGEP(namedVar.BaseType, namedVar.Storage, rvalue);
                            namedVar.BaseType = namedVar.Storage->getType();
                        }
                        // The lvalue branches above produce a Storage pointer; clear the stale Primary
                        // so the result is read from Storage. The simd-lane branch instead produces a
                        // value via Primary (Storage == nullptr) - keep it.
                        if (namedVar.Storage)
                            namedVar.Primary = nullptr;

                        // An indexed element of an interface array is itself an interface value:
                        // refresh interfaceVar or `a[i].method()` would dispatch off the stale base
                        // address captured for `a`, silently ignoring the index.
                        if (namedVar.Storage && namedVar.BaseType == Compiler(ctx)->GetFatPtrType())
                        {
                            interfaceVar = namedVar;
                            structVar = {};
                        }
                        else if (namedVar.BaseType && namedVar.BaseType->isStructTy())
                        {
                            structVar = namedVar;
                            interfaceVar = {};
                        }
                        else if (!namedVar.Storage)
                        {
                            structVar = {};
                            interfaceVar = {};
                        }
                        else
                        {
                            interfaceVar = {};
                        }

                        // A subscript result is an element (container slot): move-dataflow leaves
                        // index/deref lvalues untracked, so mark it so USE-recording skips it.
                        namedVar.IsElementAccess = true;
                        receiverWasFixedArray = false;
                        // Positive provenance for the ownership arms: only an addressable element
                        // of a `T[]` view qualifies (a simd lane has no Storage).
                        namedVar.IsViewElement = baseWasArrayView && namedVar.Storage != nullptr;
                        break;
                    }
                    case CFlatParser::RuleGenericTypeParameters:
                    {
                        auto* genParams = dynamic_cast<CFlatParser::GenericTypeParametersContext*>(ruleContext);
                        // A generic method called on a receiver (`h.get<int>()`) is keyed by its owner;
                        // that key wins over a same-named free generic function.
                        std::string templateName = GenericMethodTemplateKey(structVar.TypeAndValue.TypeName, primaryIdentifier);
                        if (templateName.empty())
                        {
                            // Bare spelling inside a namespace: reach that namespace's key first.
                            std::string gfKey = Compiler(ctx)->ResolveGenericFunctionBase(primaryIdentifier);
                            if (genericFunctionTemplates.count(gfKey))
                                templateName = gfKey;
                        }
                        if (!templateName.empty())
                        {
                            std::vector<std::string> typeArgs;
                            for (auto* entry : genParams->typeParameterList()->typeParameterEntry())
                            {
                                if (TypeArgHasUnique(entry))
                                    LogErrorContext(entry, "unique is not supported as an explicit generic function type argument");
                                typeArgs.push_back(ResolveTypeArgEntry(entry));
                            }
                            std::string mangled = InstantiateGenericFunction(templateName, typeArgs);
                            if (!mangled.empty())
                            {
                                primaryIdentifier = mangled;
                                namedVar.CallerName = mangled;  // expose mangled name for function<T> assignment
                            }
                        }
                        break;
                    }
                    // [PFX-4] call `(args)`: handles builtins (toFunction, va_start, ...), then the
                    // indirect-call [PFX-5] and member/free-call [PFX-6]/[PFX-7] paths below.
                    case CFlatParser::RuleArgumentExpressionList:
                    {
                        // Create Function Call
                        std::string functionName = primaryIdentifier;

                        auto argumentList = ctx->argumentExpressionList();

                        // [PFX-copy-ptr] Total .copy() over a bare-pointer BORROW receiver. [PFX-1] armed
                        // this and stashed the pointer value/type (the auto-deref since mutated namedVar).
                        // A pointer copy shares the pointee (shallow), so .copy() is IDENTITY - route the
                        // stashed pointer through copy()'s bitwise-identity arm. A `unique`/owning pointer
                        // never arms this flag (it keeps its auto-deref error), so no silent double-free.
                        if (functionName == "copy" && pendingPtrCopyIdentity
                            && (argumentList.empty()
                                || (functionArgCounter < (int)argumentList.size()
                                    && argumentList[functionArgCounter]->argumentNamedExpression().empty())))
                        {
                            pendingPtrCopyIdentity = false;
                            auto* compiler = Compiler(ctx);
                            LLVMBackend::NamedVariable selfArg;
                            selfArg.Primary  = pendingPtrCopyValue;
                            selfArg.BaseType = pendingPtrCopyValue ? pendingPtrCopyValue->getType() : nullptr;
                            selfArg.TypeAndValue = pendingPtrCopyType;
                            selfArg.TypeAndValue.VariableName = "";
                            llvm::Value* result = compiler->CreateOverloadedFunctionCall("copy", { selfArg });
                            namedVar = {};
                            namedVar.Primary  = result;
                            namedVar.BaseType = result ? result->getType() : nullptr;
                            namedVar.TypeAndValue = compiler->lastCallReturnType;
                            PrepareAliasCallResult(ctx, namedVar);
                            structVar = {};
                            interfaceVar = {};
                            functionArgCounter++;
                            break;
                        }

                        // Lambda<T>.toFunction(): lower a fat closure to a thin `function<T>` C pointer.
                        // Returns the bare code ptr when the closure does not capture (env null), or a
                        // null thin pointer when it does. No args; the receiver is the fat closure.
                        if (functionName == "toFunction"
                            && structVar.TypeAndValue.TypeName == "__closure_fat_ptr"
                            && (structVar.Storage != nullptr || structVar.Primary != nullptr))
                        {
                            auto* compiler = Compiler(ctx);
                            llvm::Value* fatVal = structVar.Storage
                                ? (structVar.UnionFieldType
                                    ? compiler->CreateLoad(structVar.UnionFieldType, structVar.Storage)
                                    : compiler->CreateLoad(structVar.Storage))
                                : structVar.Primary;

                            LLVMBackend::TypeAndValue thinTV = structVar.TypeAndValue;
                            thinTV.IsFunctionPointer = true;
                            thinTV.TypeName          = "__c_fn_ptr";   // IsThinFnPtr() derives from this
                            thinTV.VariableName      = "";

                            namedVar = {};
                            namedVar.Primary      = compiler->EmitFuncToFunctionLowering(fatVal, thinTV);
                            namedVar.Storage      = nullptr;
                            namedVar.BaseType     = namedVar.Primary->getType();
                            namedVar.TypeAndValue = thinTV;
                            structVar = {};
                            interfaceVar = {};
                            functionArgCounter++;
                            break;
                        }

                        // span<T> noalias fast path: lower `y.get(i)` / `y.set(i, v)` to the field
                        // subscript `y._ptr[i]` so the element access carries the receiver's alias scope.
                        // get()/set() reach the buffer through the method's `this`, collapsing every
                        // span's provenance to one origin, so two distinct spans never prove disjoint at
                        // -O2 (the documented footgun). Indexing the `_ptr` array-view field directly keys
                        // the scope to the RECEIVER (y vs x), exactly as the lowered `y[i]` and the
                        // local-`T[]` form do - restoring the contract for the natural method calls.
                        // Matched structurally (a struct with an IsArrayView `_ptr` field), so the
                        // may-alias sibling view<T> is excluded and keeps method dispatch. Restricted to
                        // an element type that OWNS NOTHING: this raw load/store bit-copies the element
                        // and skips every ownership arm, so an owning element (string, owning struct,
                        // interface, `unique` pointer) must fall through to the real method body instead.
                        int spanBufIndex = (functionName == "get" || functionName == "set")
                            && structVar.BaseType && structVar.BaseType->isStructTy()
                            && !structVar.TypeAndValue.Pointer
                            ? Compiler(ctx)->ArrayViewBufferFieldIndex(structVar.TypeAndValue.TypeName)
                            : -1;
                        if (spanBufIndex >= 0
                            && Compiler(ctx)->ArrayViewElementOwnsNothing(
                                   Compiler(ctx)->GetDataStructure(structVar.TypeAndValue.TypeName).StructFields[spanBufIndex]))
                        {
                            auto namedArgCtx = argumentList.size() > 0
                                ? argumentList[functionArgCounter]->argumentNamedExpression()
                                : std::vector<CFlatParser::ArgumentNamedExpressionContext*>{};
                            size_t expectedArgs = functionName == "set" ? 2 : 1;

                            // Requires an addressable receiver (an lvalue span); an rvalue span has no
                            // stable origin to key, so it falls through to the real method call. Record
                            // that fall-through for Detection A when inside a vectorize body so a
                            // surviving runtime alias check can name the accessor.
                            if (structVar.Storage != nullptr && namedArgCtx.size() == expectedArgs)
                            {
                                auto idxNV = this->ParseAssignmentExpressionNamed(namedArgCtx[0]->assignmentExpression());
                                llvm::Value* indexValue = idxNV.Primary ? idxNV.Primary : LoadNamedVariable(idxNV);
                                auto elem = LowerSpanElementAccess(ctx, structVar, indexValue);
                                if (elem.Storage != nullptr)
                                {
                                    if (functionName == "get")
                                    {
                                        // Read: load the element (TagViewElementAccess tags the load).
                                        namedVar = {};
                                        namedVar.Primary  = LoadNamedVariable(elem);
                                        namedVar.Storage  = nullptr;
                                        namedVar.BaseType = elem.BaseType;
                                        namedVar.TypeAndValue = elem.TypeAndValue;
                                        namedVar.TypeAndValue.NoaliasScopeId = -1; // a loaded value, no longer a view lvalue
                                    }
                                    else
                                    {
                                        // Write: store the value through the element, tagging the store.
                                        auto valNV = this->ParseAssignmentExpressionNamed(namedArgCtx[1]->assignmentExpression());
                                        llvm::Value* valValue = valNV.Primary ? valNV.Primary : LoadNamedVariable(valNV);
                                        bool valUnsigned = valNV.TypeAndValue.IsUnsignedInteger() != -1;
                                        auto* st = Compiler(ctx)->CreateAssignment(valValue, elem.Storage, valUnsigned, elem.BaseType);
                                        Compiler(ctx)->AttachViewNoalias(st, elem.TypeAndValue.NoaliasScopeId);
                                        namedVar = {};
                                    }
                                    structVar = {};
                                    interfaceVar = {};
                                    functionArgCounter++;
                                    break;
                                }
                            }
                            else if (currentVectorizeBodyLine_ != 0)
                            {
                                Compiler(ctx)->NoteVectorizeSpanAccessor(currentVectorizeBodyLine_, functionName,
                                    structVar.TypeAndValue.VariableName,
                                    (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine());
                            }
                        }

                        // Compile-time intrinsic: annotationof(TypeName, "fieldName", "AnnotationName")
                        // queries a field; annotationof(TypeName, "AnnotationName") queries the type
                        // itself (e.g. a class's [winrt]). Returns the annotation's argument value as
                        // a string constant, "1" for a present no-arg annotation, or "" if absent.
                        // Usable with `if const` to branch on annotation presence.
                        if (functionName == "annotationof")
                        {
                            std::string annValue;
                            auto* compiler = Compiler(ctx);
                            if (argumentList.size() > 0)
                            {
                                auto allArgs = argumentList[0]->argumentNamedExpression();

                                auto getArgText = [&](int idx) -> std::string {
                                    if (idx >= (int)allArgs.size()) return {};
                                    std::string t = allArgs[idx]->assignmentExpression()->getText();
                                    // Strip surrounding quotes from string literal args
                                    if (t.size() >= 2 && t.front() == '"' && t.back() == '"')
                                        return t.substr(1, t.size() - 2);
                                    return t;
                                };

                                // Two arities: annotationof(Type, "Ann") queries the type's own
                                // annotations; annotationof(Type, "field", "Ann") queries a field.
                                bool typeLevel = allArgs.size() < 3;
                                std::string typeName  = getArgText(0);
                                std::string fieldName = typeLevel ? std::string{} : getArgText(1);
                                std::string annName   = typeLevel ? getArgText(1) : getArgText(2);

                                // Resolve generic type substitutions
                                auto substIt = activeTypeSubstitutions.find(typeName);
                                if (substIt != activeTypeSubstitutions.end())
                                    typeName = substIt->second;

                                // The matched annotation's value: "1" for a present no-arg marker,
                                // the argument otherwise (multi-arg forms are comma-joined),
                                // or "" when absent (a null match).
                                auto annText = [](const LLVMBackend::AnnotationValue* a) -> std::string {
                                    if (!a) return {};
                                    if (a->Values.size() > 1)
                                    {
                                        std::string joined;
                                        for (const auto& v : a->Values)
                                        {
                                            if (!joined.empty()) joined += ",";
                                            joined += v;
                                        }
                                        return joined;
                                    }
                                    return a->Value.empty() ? "1" : a->Value;
                                };

                                if (typeLevel)
                                {
                                    // Type-level annotations ([winrt], [uuid], ...) live in the shared map.
                                    annValue = annText(compiler->FindTypeAnnotation(typeName, annName));
                                }
                                else
                                {
                                    auto structData = compiler->GetDataStructure(typeName);
                                    for (const auto& field : structData.StructFields)
                                    {
                                        if (field.VariableName != fieldName) continue;
                                        for (const auto& ann : field.Annotations)
                                            if (ann.Name == annName) { annValue = annText(&ann); break; }
                                        break;
                                    }
                                }
                            }
                            namedVar.Primary = compiler->CreateGlobalString("annotationof", annValue);
                            namedVar.TypeAndValue.TypeName = "string";
                            break;
                        }

                        // Compile-time literal folds: json_const and xml_const share this walker.

                        if (functionName == "json_const" || functionName == "xml_const")
                        {
                            auto* compiler = Compiler(ctx);
                            const std::string intrinsicName = functionName == "xml_const"
                                ? "xml_const" : "json_const";
                            auto args = argumentList.empty()
                                ? std::vector<CFlatParser::ArgumentNamedExpressionContext*>{}
                                : argumentList[0]->argumentNamedExpression();
                            if (args.size() != 2 || args[0]->assignmentExpression() == nullptr
                                || args[1]->initializerList() == nullptr)
                            {
                                LogErrorContext(ctx, std::format(
                                    "{}() requires a type name and a brace initializer", intrinsicName));
                                break;
                            }
                            std::string typeName = args[0]->assignmentExpression()->getText();
                            auto folded = FoldConstLiteral(ctx, intrinsicName, typeName,
                                                           args[1]->initializerList());
                            if (folded)
                                namedVar = compiler->MakeStringLiteralNV(*folded);
                            break;
                        }

                        // Intrinsic: reflect(obj, visitor)
                        // Compile-time resolves obj's struct type T, synthesizes __reflect_T if needed,
                        // then emits: visitor.beginObject(""); __reflect_T(obj, visitor); visitor.endObject();
                        if (functionName == "reflect")
                        {
                            auto* compiler = Compiler(ctx);

                            // 1. Validate arity
                            if (argumentList.empty() || argumentList[0]->argumentNamedExpression().size() < 2)
                            {
                                LogErrorContext(ctx, "reflect() requires exactly two arguments: reflect(obj, visitor)");
                                break;
                            }
                            auto namedArgCtx = argumentList[0]->argumentNamedExpression();

                            // 2. Evaluate obj argument
                            auto objNV = ParseAssignmentExpressionNamed(namedArgCtx[0]->assignmentExpression());
                            std::string structTypeName = objNV.TypeAndValue.TypeName;
                            bool isPtr = objNV.TypeAndValue.Pointer;

                            // Validate struct type exists
                            auto sd = compiler->GetDataStructure(structTypeName);
                            if (!sd.StructType)
                            {
                                LogErrorContext(ctx, std::format("reflect(): first argument must be a struct type, got '{}'",
                                    SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = structTypeName })));
                                break;
                            }

                            // 3. Evaluate visitor argument
                            auto visitorNV = ParseAssignmentExpressionNamed(namedArgCtx[1]->assignmentExpression());
                            if (!visitorNV.TypeAndValue.IsInterface || visitorNV.TypeAndValue.TypeName != "IReflector")
                            {
                                LogErrorContext(ctx, "reflect(): second argument must be an IReflector interface value");
                                break;
                            }

                            // Get visitor alloca for interface method calls
                            llvm::Value* visitorAlloca = visitorNV.Storage;
                            if (!visitorAlloca)
                            {
                                // Visitor was a temporary - alloca it now
                                auto* fatTy = compiler->GetFatPtrType();
                                visitorAlloca = compiler->builder->CreateAlloca(fatTy, nullptr, "reflect_visitor_tmp");
                                llvm::Value* visitorVal = visitorNV.Primary;
                                if (!visitorVal)
                                    visitorVal = compiler->CreateLoad(visitorNV.Storage);
                                compiler->builder->CreateStore(visitorVal, visitorAlloca);
                            }

                            // 4. Inline reflection code with recursive lambda for nested structs
                            auto emptyNameNV = compiler->MakeStringLiteralNV("");
                            auto structData = compiler->GetDataStructure(structTypeName);
                            if (!structData.StructType)
                            {
                                LogErrorContext(ctx, std::format("reflect: cannot find struct '{}'",
                                    SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = structTypeName })));
                                break;
                            }
                            if (structData.IsUnion)
                            {
                                LogErrorContext(ctx, std::format("reflect is not supported on union type '{}'",
                                    SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = structTypeName })));
                                break;
                            }

                            // Define recursive lambda to emit fields for any struct
                            std::function<void(const LLVMBackend::StructData&, llvm::Value*)> emitFields;
                            emitFields = [&](const LLVMBackend::StructData& sd, llvm::Value* objPtr)
                            {
                                if (sd.IsUnion)
                                {
                                    compiler->LogError(std::format(
                                        "JSON serialization is not supported on union type '{}'",
                                        SpellType(*compiler, LLVMBackend::TypeAndValue{
                                            .TypeName = sd.StructType->getName().str() })));
                                    return;
                                }
                                LLVMBackend::NamedVariable intNV, boolNV, floatNV, strNV;
                                for (size_t i = 0; i < sd.StructFields.size(); i++)
                                {
                                    const auto& field = sd.StructFields[i];

                                    // Synthetic bitfield storage slots (`__bfN`) are not user-visible;
                                    // the named bitfields they pack are emitted from sd.Bitfields below.
                                    // Synthetic `__padN` alignment slots are not user-visible either.
                                    if (field.IsBitfieldStorage || field.IsPadding) continue;

                                    // Skip [Private] fields
                                    bool isPrivate = false;
                                    for (const auto& ann : field.Annotations)
                                        if (ann.Name == "Private") { isPrivate = true; break; }
                                    if (isPrivate) continue;

                                    const std::string& typeName = field.TypeName;
                                    std::string displayName = field.VariableName;
                                    for (const auto& ann : field.Annotations)
                                        if (ann.Name == "JsonName" && !ann.Value.empty()) { displayName = ann.Value; break; }
                                    auto* gep = compiler->builder->CreateStructGEP(sd.StructType, objPtr, (unsigned)i,
                                        field.VariableName + "_ptr");

                                    if ((typeName == "int" || typeName == "i8" || typeName == "i16" || typeName == "i32" || typeName == "i64"
                                         || typeName == "u8" || typeName == "u16" || typeName == "u32" || typeName == "u64")
                                        && !field.Pointer)
                                    {
                                        auto* val = compiler->builder->CreateLoad(compiler->GetType(field), gep);
                                        // Widen to i64: visitInt takes i64 so a 64-bit field is not truncated.
                                        auto* widened = compiler->Upconvert(val, compiler->builder->getInt64Ty(), false);
                                        auto nameNV = compiler->MakeStringLiteralNV(displayName);
                                        intNV = {};
                                        intNV.Primary = widened;
                                        intNV.TypeAndValue.TypeName = "i64";
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitInt", {nameNV, intNV});
                                    }
                                    else if (typeName == "string" && !field.Pointer)
                                    {
                                        auto nameNV = compiler->MakeStringLiteralNV(displayName);
                                        strNV = {};
                                        strNV.Storage = gep;
                                        strNV.TypeAndValue.TypeName = "string";
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitString", {nameNV, strNV});
                                    }
                                    else if (typeName == "bool" && !field.Pointer)
                                    {
                                        auto* val = compiler->builder->CreateLoad(compiler->GetType(field), gep);
                                        auto nameNV = compiler->MakeStringLiteralNV(displayName);
                                        boolNV = {};
                                        boolNV.Primary = val;
                                        boolNV.TypeAndValue.TypeName = "bool";
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitBool", {nameNV, boolNV});
                                    }
                                    else if ((typeName == "float" || typeName == "double") && !field.Pointer)
                                    {
                                        llvm::Value* val = compiler->builder->CreateLoad(compiler->GetType(field), gep);
                                        // visitFloat takes double, so widen a 'float' field instead of narrowing.
                                        if (typeName == "float")
                                            val = compiler->builder->CreateFPCast(val, compiler->builder->getDoubleTy());
                                        auto nameNV = compiler->MakeStringLiteralNV(displayName);
                                        floatNV = {};
                                        floatNV.Primary = val;
                                        floatNV.TypeAndValue.TypeName = "double";
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitFloat", {nameNV, floatNV});
                                    }
                                    // ── list<T> field (value type, not pointer) ──────────────
                                    // Check BEFORE nested struct to avoid treating list as a struct
                                    else if (MangledBase(typeName) == "list" && !field.Pointer)
                                    {
                                        TypeSpelling listSpelling;
                                        if (!DemangleType(*compiler, typeName, listSpelling)
                                            || listSpelling.args.size() != 1)
                                            continue;
                                        std::string elemTypeName = MangleType(
                                            *compiler, listSpelling.args[0]);
                                        auto nameNV = compiler->MakeStringLiteralNV(displayName);
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "beginArray", {nameNV});

                                        // Call count() on the list struct
                                        LLVMBackend::NamedVariable selfNV;
                                        selfNV.Storage = gep;
                                        selfNV.TypeAndValue.TypeName = typeName;
                                        auto* countVal = compiler->CreateOverloadedFunctionCall("count", {selfNV});

                                        // Loop: for (int i = 0; i < count; i++)
                                        auto* i32Ty = compiler->builder->getInt32Ty();
                                        auto* indexAlloca = compiler->builder->CreateAlloca(i32Ty, nullptr, "reflect_arr_idx");
                                        compiler->builder->CreateStore(compiler->builder->getInt32(0), indexAlloca);

                                        auto* condBB = compiler->CreateBasicBlock("reflect_arr_cond");
                                        auto* bodyBB = compiler->CreateBasicBlock("reflect_arr_body");
                                        auto* afterBB = compiler->CreateBasicBlock("reflect_arr_after");
                                        compiler->builder->CreateBr(condBB);

                                        // condition: i < count
                                        compiler->builder->SetInsertPoint(condBB);
                                        auto* idx = compiler->builder->CreateLoad(i32Ty, indexAlloca);
                                        auto* cmp = compiler->builder->CreateICmpSLT(idx, countVal);
                                        compiler->builder->CreateCondBr(cmp, bodyBB, afterBB);

                                        // loop body: get element and dispatch
                                        compiler->builder->SetInsertPoint(bodyBB);
                                        auto* idx2 = compiler->builder->CreateLoad(i32Ty, indexAlloca);
                                        LLVMBackend::NamedVariable idxNV;
                                        idxNV.Primary = idx2;
                                        idxNV.TypeAndValue.TypeName = "int";
                                        auto elemNV = compiler->CreateOverloadedFunctionCall("get", {selfNV, idxNV});
                                        if (compiler->lastCallReturnType.IsAlias
                                            && !compiler->lastCallReturnType.Pointer && elemNV != nullptr)
                                        {
                                            elemNV = compiler->builder->CreateLoad(
                                                compiler->GetType(compiler->lastCallReturnType), elemNV,
                                                "reflect_alias_value");
                                            elemNV = compiler->ClearStringOwnedBit(elemNV);
                                            elemNV = compiler->ClearStructOwnedBits(
                                                elemNV, compiler->lastCallReturnType.TypeName);
                                        }
                                        auto emptyNV = compiler->MakeStringLiteralNV("");

                                        // Dispatch element by type
                                        if ((elemTypeName == "int" || elemTypeName == "i8" || elemTypeName == "i16" || elemTypeName == "i32" || elemTypeName == "i64"
                                             || elemTypeName == "u8" || elemTypeName == "u16" || elemTypeName == "u32" || elemTypeName == "u64"))
                                        {
                                            auto* widened = compiler->Upconvert(elemNV, compiler->builder->getInt64Ty(), false);
                                            LLVMBackend::NamedVariable elemIntNV;
                                            elemIntNV.Primary = widened;
                                            elemIntNV.TypeAndValue.TypeName = "i64";
                                            compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitInt", {emptyNV, elemIntNV});
                                        }
                                        else if (elemTypeName == "bool")
                                        {
                                            LLVMBackend::NamedVariable elemBoolNV;
                                            elemBoolNV.Primary = elemNV;
                                            elemBoolNV.TypeAndValue.TypeName = "bool";
                                            compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitBool", {emptyNV, elemBoolNV});
                                        }
                                        else if (elemTypeName == "float" || elemTypeName == "double")
                                        {
                                            llvm::Value* val = elemNV;
                                            if (elemTypeName == "float")
                                                val = compiler->builder->CreateFPCast(val, compiler->builder->getDoubleTy());
                                            LLVMBackend::NamedVariable elemFloatNV;
                                            elemFloatNV.Primary = val;
                                            elemFloatNV.TypeAndValue.TypeName = "double";
                                            compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitFloat", {emptyNV, elemFloatNV});
                                        }
                                        else if (elemTypeName == "string")
                                        {
                                            // For string, spill to alloca and set Storage
                                            LLVMBackend::TypeAndValue strTV;
                                            strTV.TypeName = "string";
                                            auto* strAlloca = compiler->builder->CreateAlloca(compiler->GetType(strTV), nullptr, "reflect_arr_elem");
                                            compiler->builder->CreateStore(elemNV, strAlloca);
                                            LLVMBackend::NamedVariable elemStrNV;
                                            elemStrNV.Storage = strAlloca;
                                            elemStrNV.TypeAndValue.TypeName = "string";
                                            compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitString", {emptyNV, elemStrNV});
                                        }
                                        else if (compiler->dataStructures.count(elemTypeName))
                                        {
                                            // Struct element - spill to alloca and recurse
                                            auto nestedData = compiler->GetDataStructure(elemTypeName);
                                            auto* elemAlloca = compiler->builder->CreateAlloca(nestedData.StructType, nullptr, "reflect_arr_elem");
                                            compiler->builder->CreateStore(elemNV, elemAlloca);
                                            compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "beginObject", {emptyNV});
                                            emitFields(nestedData, elemAlloca);
                                            compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "endObject", {});
                                        }

                                        // i++
                                        auto* nextIdx = compiler->builder->CreateAdd(idx2, compiler->builder->getInt32(1));
                                        compiler->builder->CreateStore(nextIdx, indexAlloca);
                                        compiler->builder->CreateBr(condBB);

                                        // after loop
                                        compiler->builder->SetInsertPoint(afterBB);
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "endArray", {});
                                    }
                                    // Enum values serialize through their declared backing integer.
                                    else if (!field.Pointer && !compiler->GetEnumBackingType(typeName).empty())
                                    {
                                        auto* val = compiler->builder->CreateLoad(compiler->GetType(field), gep);
                                        auto* widened = compiler->Upconvert(val, compiler->builder->getInt64Ty(), false);
                                        intNV = {};
                                        intNV.Primary = widened;
                                        intNV.TypeAndValue.TypeName = "i64";
                                        auto nameNV = compiler->MakeStringLiteralNV(displayName);
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitInt", {nameNV, intNV});
                                    }
                                    // ── nested struct (value type) ────────────────────────────
                                    else if (!field.Pointer && compiler->dataStructures.count(typeName))
                                    {
                                        auto nestedData = compiler->GetDataStructure(typeName);
                                        auto nameNV = compiler->MakeStringLiteralNV(displayName);
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "beginObject", {nameNV});
                                        emitFields(nestedData, gep);
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "endObject", {});
                                    }
                                    // ── nested struct pointer ─────────────────────────────────
                                    else if (field.Pointer && compiler->dataStructures.count(typeName))
                                    {
                                        auto nameNV = compiler->MakeStringLiteralNV(displayName);
                                        auto* ptrVal = compiler->builder->CreateLoad(
                                            llvm::PointerType::getUnqual(*compiler->context), gep);
                                        auto* isNull = compiler->builder->CreateICmpEQ(ptrVal,
                                            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrVal->getType())));
                                        auto* thenBB = compiler->CreateBasicBlock("reflect_null_f");
                                        auto* elseBB = compiler->CreateBasicBlock("reflect_obj_f");
                                        auto* mergeBB = compiler->CreateBasicBlock("reflect_merge_f");
                                        compiler->builder->CreateCondBr(isNull, thenBB, elseBB);
                                        // null branch
                                        compiler->builder->SetInsertPoint(thenBB);
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitNull", {nameNV});
                                        compiler->builder->CreateBr(mergeBB);
                                        // non-null branch
                                        compiler->builder->SetInsertPoint(elseBB);
                                        auto nestedData = compiler->GetDataStructure(typeName);
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "beginObject", {nameNV});
                                        emitFields(nestedData, ptrVal);
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "endObject", {});
                                        compiler->builder->CreateBr(mergeBB);
                                        // merge
                                        compiler->builder->SetInsertPoint(mergeBB);
                                    }
                                }

                                // Named bitfields live in a side-table, not StructFields.
                                // Extract each one from its storage word with a sign-aware shift+mask.
                                for (const auto& bf : sd.Bitfields)
                                {
                                    bool bfPrivate = false;
                                    for (const auto& ann : bf.Annotations)
                                        if (ann.Name == "Private") { bfPrivate = true; break; }
                                    if (bfPrivate) continue;

                                    std::string displayName = bf.Name;
                                    for (const auto& ann : bf.Annotations)
                                        if (ann.Name == "JsonName" && !ann.Value.empty()) { displayName = ann.Value; break; }

                                    const auto& storageField = sd.StructFields[bf.StorageFieldIndex];
                                    auto* storageTy = compiler->GetType(storageField);
                                    auto* storagePtr = compiler->builder->CreateStructGEP(sd.StructType, objPtr,
                                        bf.StorageFieldIndex, bf.Name + "_bf_ptr");
                                    auto* word = compiler->builder->CreateLoad(storageTy, storagePtr);

                                    unsigned w = bf.BitWidth;
                                    unsigned off = bf.BitOffset;
                                    unsigned storageBits = (unsigned)word->getType()->getIntegerBitWidth();

                                    llvm::Value* extracted;
                                    if (bf.IsUnsigned || bf.TypeName == "bool")
                                    {
                                        auto* shr = compiler->builder->CreateLShr(word,
                                            llvm::ConstantInt::get(word->getType(), off));
                                        uint64_t mask = (w == 64) ? ~uint64_t(0) : ((uint64_t(1) << w) - 1);
                                        extracted = compiler->builder->CreateAnd(shr,
                                            llvm::ConstantInt::get(word->getType(), mask));
                                    }
                                    else
                                    {
                                        unsigned leftShift = storageBits - w - off;
                                        auto* shl = compiler->builder->CreateShl(word,
                                            llvm::ConstantInt::get(word->getType(), leftShift));
                                        extracted = compiler->builder->CreateAShr(shl,
                                            llvm::ConstantInt::get(word->getType(), storageBits - w));
                                    }

                                    auto nameNV = compiler->MakeStringLiteralNV(displayName);

                                    if (bf.TypeName == "bool")
                                    {
                                        auto* asBool = compiler->builder->CreateICmpNE(extracted,
                                            llvm::ConstantInt::get(extracted->getType(), 0));
                                        LLVMBackend::NamedVariable bNV;
                                        bNV.Primary = asBool;
                                        bNV.TypeAndValue.TypeName = "bool";
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitBool", {nameNV, bNV});
                                    }
                                    else
                                    {
                                        auto* widened = compiler->Upconvert(extracted,
                                            compiler->builder->getInt64Ty(), bf.IsUnsigned);
                                        LLVMBackend::NamedVariable iNV;
                                        iNV.Primary = widened;
                                        iNV.TypeAndValue.TypeName = "i64";
                                        compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitInt", {nameNV, iNV});
                                    }
                                }
                            };

                            llvm::Value* objPtr = nullptr;
                            if (isPtr)
                            {
                                // Null-check for pointer obj
                                objPtr = objNV.Primary;
                                if (!objPtr)
                                    objPtr = compiler->CreateLoad(objNV.Storage);
                                auto ptrType = llvm::cast<llvm::PointerType>(objPtr->getType());
                                auto* isNull = compiler->builder->CreateICmpEQ(objPtr,
                                    llvm::ConstantPointerNull::get(ptrType));

                                auto* thenBB = compiler->CreateBasicBlock("reflect_null");
                                auto* elseBB = compiler->CreateBasicBlock("reflect_obj");
                                auto* mergeBB = compiler->CreateBasicBlock("reflect_merge");
                                compiler->builder->CreateCondBr(isNull, thenBB, elseBB);

                                // null branch: visitNull("")
                                compiler->builder->SetInsertPoint(thenBB);
                                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitNull", {emptyNameNV});
                                compiler->builder->CreateBr(mergeBB);

                                // non-null branch: beginObject + reflect fields + endObject
                                compiler->builder->SetInsertPoint(elseBB);
                                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "beginObject", {emptyNameNV});
                                emitFields(structData, objPtr);
                                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "endObject", {});
                                compiler->builder->CreateBr(mergeBB);

                                compiler->builder->SetInsertPoint(mergeBB);
                            }
                            else
                            {
                                // Value type: emit reflection code directly
                                objPtr = objNV.Storage;
                                if (!objPtr)
                                {
                                    LogErrorContext(ctx, "reflect(): cannot take address of temporary struct value");
                                    break;
                                }

                                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "beginObject", {emptyNameNV});
                                emitFields(structData, objPtr);
                                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "endObject", {});
                            }

                            // reflect() returns void
                            namedVar = {};
                            break;
                        }

                        // Compile-time intrinsic: reflect_set(obj, src)
                        // Symmetric dual of reflect(). Walks obj's struct fields at compile time,
                        // calls src.getXxx(fieldName) for each, and stores the result back into obj.
                        // src must be an IJSON interface value. Respects [Private] and [JsonName].
                        if (functionName == "reflect_set")
                        {
                            auto* compiler = Compiler(ctx);

                            // 1. Validate arity
                            if (argumentList.empty() || argumentList[0]->argumentNamedExpression().size() < 2)
                            {
                                LogErrorContext(ctx, "reflect_set() requires exactly two arguments: reflect_set(obj, src)");
                                break;
                            }
                            auto namedArgCtx = argumentList[0]->argumentNamedExpression();

                            // 2. Evaluate obj argument
                            auto objNV = ParseAssignmentExpressionNamed(namedArgCtx[0]->assignmentExpression());
                            std::string structTypeName = objNV.TypeAndValue.TypeName;
                            bool isPtr = objNV.TypeAndValue.Pointer;

                            auto sd = compiler->GetDataStructure(structTypeName);
                            if (!sd.StructType)
                            {
                                LogErrorContext(ctx, std::format("reflect_set(): first argument must be a struct type, got '{}'",
                                    SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = structTypeName })));
                                break;
                            }

                            // 3. Evaluate src argument - must be IJSON
                            auto srcNV = ParseAssignmentExpressionNamed(namedArgCtx[1]->assignmentExpression());
                            if (!srcNV.TypeAndValue.IsInterface || srcNV.TypeAndValue.TypeName != "IJSON")
                            {
                                LogErrorContext(ctx, "reflect_set(): second argument must be an IJSON interface value");
                                break;
                            }

                            // Ensure src is in an alloca for interface method dispatch
                            llvm::Value* srcAlloca = srcNV.Storage;
                            if (!srcAlloca)
                            {
                                auto* fatTy = compiler->GetFatPtrType();
                                srcAlloca = compiler->builder->CreateAlloca(fatTy, nullptr, "reflect_set_src");
                                llvm::Value* srcVal = srcNV.Primary ? srcNV.Primary : compiler->CreateLoad(srcNV.Storage);
                                compiler->builder->CreateStore(srcVal, srcAlloca);
                            }

                            // 4. Recursive lambda: populate fields of any struct from an IJSON alloca
                            std::function<void(const LLVMBackend::StructData&, llvm::Value*, llvm::Value*)> emitFieldSets;
                            emitFieldSets = [&](const LLVMBackend::StructData& sd, llvm::Value* objPtr, llvm::Value* srcA)
                            {
                                if (sd.IsUnion)
                                {
                                    compiler->LogError(std::format(
                                        "JSON deserialization is not supported on union type '{}'",
                                        SpellType(*compiler, LLVMBackend::TypeAndValue{
                                            .TypeName = sd.StructType->getName().str() })));
                                    return;
                                }
                                auto* fatTy = compiler->GetFatPtrType();

                                for (size_t i = 0; i < sd.StructFields.size(); i++)
                                {
                                    const auto& field = sd.StructFields[i];

                                    // Synthetic `__padN` alignment slots are not user-visible members.
                                    if (field.IsPadding) continue;

                                    // Skip [Private] fields
                                    bool isPrivate = false;
                                    for (const auto& ann : field.Annotations)
                                        if (ann.Name == "Private") { isPrivate = true; break; }
                                    if (isPrivate) continue;

                                    const std::string& typeName = field.TypeName;
                                    std::string displayName = field.VariableName;
                                    for (const auto& ann : field.Annotations)
                                        if (ann.Name == "JsonName" && !ann.Value.empty()) { displayName = ann.Value; break; }

                                    auto* gep = compiler->builder->CreateStructGEP(sd.StructType, objPtr, (unsigned)i,
                                        field.VariableName + "_ptr");
                                    auto nameNV = compiler->MakeStringLiteralNV(displayName);

                                    // ── int / sized integer ──────────────────────────────────
                                    if ((typeName == "int" || typeName == "i8" || typeName == "i16" || typeName == "i32" || typeName == "i64"
                                         || typeName == "u8" || typeName == "u16" || typeName == "u32" || typeName == "u64")
                                        && !field.Pointer)
                                    {
                                        auto* intVal = compiler->CallInterfaceMethod(srcA, "IJSON", "getInt", {nameNV});
                                        auto* narrowed = compiler->Upconvert(intVal, compiler->GetType(field), false);
                                        compiler->builder->CreateStore(narrowed, gep);
                                    }
                                    // ── bool ─────────────────────────────────────────────────
                                    else if (typeName == "bool" && !field.Pointer)
                                    {
                                        auto* boolVal = compiler->CallInterfaceMethod(srcA, "IJSON", "getBool", {nameNV});
                                        compiler->builder->CreateStore(boolVal, gep);
                                    }
                                    // ── float / double ────────────────────────────────────────
                                    else if ((typeName == "float" || typeName == "double") && !field.Pointer)
                                    {
                                        auto* fVal = compiler->CallInterfaceMethod(srcA, "IJSON", "getFloat", {nameNV});
                                        if (typeName == "double")
                                            fVal = compiler->builder->CreateFPCast(fVal, compiler->builder->getDoubleTy());
                                        compiler->builder->CreateStore(fVal, gep);
                                    }
                                    // ── string ───────────────────────────────────────────────
                                    else if (typeName == "string" && !field.Pointer)
                                    {
                                        auto* strVal = compiler->CallInterfaceMethod(srcA, "IJSON", "getString", {nameNV});
                                        // getString() returns a string that ALIASES the parser's Arena,
                                        // which is freed when the deserialize call returns. Copy it into
                                        // an owned heap buffer so the field outlives the arena - otherwise
                                        // the field dangles (and a string-freeing dtor on T double-frees
                                        // an arena pointer). See core/json.cb fromJson/fromJsonRaw.
                                        LLVMBackend::TypeAndValue strTV;
                                        strTV.TypeName = "string";
                                        auto* strLLTy = compiler->GetType(strTV);
                                        auto* srcStr = compiler->builder->CreateAlloca(strLLTy, nullptr, "rset_str_src");
                                        compiler->builder->CreateStore(strVal, srcStr);
                                        LLVMBackend::NamedVariable srcStrNV;
                                        srcStrNV.Storage = srcStr;
                                        srcStrNV.BaseType = strLLTy;
                                        srcStrNV.TypeAndValue.TypeName = "string";
                                        auto* owned = compiler->CreateOverloadedFunctionCall("copy", {srcStrNV});
                                        compiler->builder->CreateStore(owned, gep);
                                    }
                                    // Enum values use the same JSON integer representation as their backing type.
                                    else if (!field.Pointer && !compiler->GetEnumBackingType(typeName).empty())
                                    {
                                        auto* intVal = compiler->CallInterfaceMethod(srcA, "IJSON", "getInt", {nameNV});
                                        auto backingType = compiler->GetEnumBackingType(typeName);
                                        LLVMBackend::TypeAndValue backingTV;
                                        backingTV.TypeName = backingType;
                                        auto* narrowed = compiler->Upconvert(intVal, compiler->GetType(backingTV), false);
                                        compiler->builder->CreateStore(narrowed, gep);
                                    }
                                    // ── list<T> (value type) ──────────────────────────────────
                                    else if (MangledBase(typeName) == "list" && !field.Pointer)
                                    {
                                        TypeSpelling listSpelling;
                                        if (!DemangleType(*compiler, typeName, listSpelling)
                                            || listSpelling.args.size() != 1)
                                            continue;
                                        std::string elemTypeName = MangleType(
                                            *compiler, listSpelling.args[0]);

                                        // arr = src.getArray(name) -> alloca fat ptr
                                        auto* arrVal = compiler->CallInterfaceMethod(srcA, "IJSON", "getArray", {nameNV});
                                        auto* arrAlloca = compiler->builder->CreateAlloca(fatTy, nullptr, "reflect_set_arr");
                                        compiler->builder->CreateStore(arrVal, arrAlloca);

                                        // count = arr.count()
                                        auto* countVal = compiler->CallInterfaceMethod(arrAlloca, "IJSONArray", "count", {});

                                        // Loop i = 0..count
                                        auto* i32Ty = compiler->builder->getInt32Ty();
                                        auto* idxAlloca = compiler->builder->CreateAlloca(i32Ty, nullptr, "reflect_set_idx");
                                        compiler->builder->CreateStore(compiler->builder->getInt32(0), idxAlloca);

                                        auto* condBB = compiler->CreateBasicBlock("rset_arr_cond");
                                        auto* bodyBB = compiler->CreateBasicBlock("rset_arr_body");
                                        auto* afterBB = compiler->CreateBasicBlock("rset_arr_after");
                                        compiler->builder->CreateBr(condBB);

                                        compiler->builder->SetInsertPoint(condBB);
                                        auto* idx = compiler->builder->CreateLoad(i32Ty, idxAlloca);
                                        compiler->builder->CreateCondBr(
                                            compiler->builder->CreateICmpSLT(idx, countVal), bodyBB, afterBB);

                                        compiler->builder->SetInsertPoint(bodyBB);
                                        auto* idx2 = compiler->builder->CreateLoad(i32Ty, idxAlloca);
                                        LLVMBackend::NamedVariable idxNV;
                                        idxNV.Primary = idx2;
                                        idxNV.TypeAndValue.TypeName = "int";

                                        // list self NV for add()
                                        LLVMBackend::NamedVariable listNV;
                                        listNV.Storage = gep;
                                        listNV.TypeAndValue.TypeName = typeName;

                                        if (elemTypeName == "int" || elemTypeName == "i32")
                                        {
                                            auto* v = compiler->CallInterfaceMethod(arrAlloca, "IJSONArray", "getInt", {idxNV});
                                            LLVMBackend::NamedVariable elemNV;
                                            elemNV.Primary = v;
                                            elemNV.TypeAndValue.TypeName = "int";
                                            compiler->CreateOverloadedFunctionCall("add", {listNV, elemNV});
                                        }
                                        else if (elemTypeName == "bool")
                                        {
                                            auto* v = compiler->CallInterfaceMethod(arrAlloca, "IJSONArray", "getBool", {idxNV});
                                            LLVMBackend::NamedVariable elemNV;
                                            elemNV.Primary = v;
                                            elemNV.TypeAndValue.TypeName = "bool";
                                            compiler->CreateOverloadedFunctionCall("add", {listNV, elemNV});
                                        }
                                        else if (elemTypeName == "float" || elemTypeName == "double")
                                        {
                                            auto* v = compiler->CallInterfaceMethod(arrAlloca, "IJSONArray", "getFloat", {idxNV});
                                            LLVMBackend::NamedVariable elemNV;
                                            elemNV.Primary = v;
                                            elemNV.TypeAndValue.TypeName = "float";
                                            compiler->CreateOverloadedFunctionCall("add", {listNV, elemNV});
                                        }
                                        else if (elemTypeName == "string")
                                        {
                                            auto* v = compiler->CallInterfaceMethod(arrAlloca, "IJSONArray", "getString", {idxNV});
                                            LLVMBackend::TypeAndValue strTV;
                                            strTV.TypeName = "string";
                                            auto* strAlloca = compiler->builder->CreateAlloca(compiler->GetType(strTV), nullptr, "rset_arr_str");
                                            compiler->builder->CreateStore(v, strAlloca);
                                            // add() is a move - it would take ownership of the arena-aliasing
                                            // pointer, leaving the list pointing into the freed arena (dangle +
                                            // double-free on the list dtor). Copy out of the arena first so the
                                            // list owns its own heap buffer. See core/json.cb.
                                            auto* strLLTy = compiler->GetType(strTV);
                                            LLVMBackend::NamedVariable srcStrNV;
                                            srcStrNV.Storage = strAlloca;
                                            srcStrNV.BaseType = strLLTy;
                                            srcStrNV.TypeAndValue.TypeName = "string";
                                            auto* owned = compiler->CreateOverloadedFunctionCall("copy", {srcStrNV});
                                            auto* ownedAlloca = compiler->builder->CreateAlloca(strLLTy, nullptr, "rset_arr_str_owned");
                                            compiler->builder->CreateStore(owned, ownedAlloca);
                                            LLVMBackend::NamedVariable elemNV;
                                            elemNV.Storage = ownedAlloca;
                                            elemNV.BaseType = strLLTy;
                                            elemNV.TypeAndValue.TypeName = "string";
                                            // `owned` is copy()'s result - an owning heap buffer. Flag it so the
                                            // move-arg lowering hands that buffer straight to add() instead of
                                            // heap-copying it again and orphaning the original (a leak per element).
                                            elemNV.IsOwningString = true;
                                            compiler->CreateOverloadedFunctionCall("add", {listNV, elemNV});
                                        }
                                        else if (compiler->dataStructures.count(elemTypeName))
                                        {
                                            auto* subVal = compiler->CallInterfaceMethod(arrAlloca, "IJSONArray", "getObject", {idxNV});
                                            auto* subAlloca = compiler->builder->CreateAlloca(fatTy, nullptr, "rset_arr_sub");
                                            compiler->builder->CreateStore(subVal, subAlloca);

                                            auto nestedData = compiler->GetDataStructure(elemTypeName);
                                            auto* elemAlloca = compiler->builder->CreateAlloca(nestedData.StructType, nullptr, "rset_arr_elem");
                                            compiler->builder->CreateStore(
                                                llvm::Constant::getNullValue(nestedData.StructType), elemAlloca);
                                            emitFieldSets(nestedData, elemAlloca, subAlloca);

                                            LLVMBackend::NamedVariable elemNV;
                                            elemNV.Storage = elemAlloca;
                                            // BaseType is required so add()'s move-clear can zero the
                                            // element's source storage after the move. Without it the
                                            // move path sees a null base type and reports a bogus
                                            // "'move' argument has no resolved type" at a phantom
                                            // location (see LLVMBackend move-clear). Mirrors the
                                            // list<string> element branch above, which sets BaseType.
                                            elemNV.BaseType = nestedData.StructType;
                                            elemNV.TypeAndValue.TypeName = elemTypeName;
                                            compiler->CreateOverloadedFunctionCall("add", {listNV, elemNV});
                                        }

                                        // i++
                                        compiler->builder->CreateStore(
                                            compiler->builder->CreateAdd(idx2, compiler->builder->getInt32(1)), idxAlloca);
                                        compiler->builder->CreateBr(condBB);
                                        compiler->builder->SetInsertPoint(afterBB);
                                    }
                                    // ── nested struct (value) ─────────────────────────────────
                                    else if (!field.Pointer && compiler->dataStructures.count(typeName))
                                    {
                                        auto* subVal = compiler->CallInterfaceMethod(srcA, "IJSON", "getObject", {nameNV});
                                        auto* subAlloca = compiler->builder->CreateAlloca(fatTy, nullptr, "reflect_set_sub");
                                        compiler->builder->CreateStore(subVal, subAlloca);
                                        auto nestedData = compiler->GetDataStructure(typeName);
                                        emitFieldSets(nestedData, gep, subAlloca);
                                    }
                                    // ── nested struct pointer ─────────────────────────────────
                                    else if (field.Pointer && compiler->dataStructures.count(typeName))
                                    {
                                        auto hasfieldNV = nameNV;
                                        auto* hasVal = compiler->CallInterfaceMethod(srcA, "IJSON", "hasField", {hasfieldNV});
                                        auto* thenBB = compiler->CreateBasicBlock("rset_ptr_then");
                                        auto* mergeBB = compiler->CreateBasicBlock("rset_ptr_merge");
                                        compiler->builder->CreateCondBr(hasVal, thenBB, mergeBB);

                                        compiler->builder->SetInsertPoint(thenBB);
                                        auto* subVal = compiler->CallInterfaceMethod(srcA, "IJSON", "getObject", {nameNV});
                                        auto* subAlloca = compiler->builder->CreateAlloca(fatTy, nullptr, "reflect_set_subp");
                                        compiler->builder->CreateStore(subVal, subAlloca);

                                        auto nestedData = compiler->GetDataStructure(typeName);
                                        auto* newObj = compiler->builder->CreateCall(
                                            compiler->module->getFunction("__alloc_" + typeName) ?
                                                compiler->module->getFunction("__alloc_" + typeName) : nullptr,
                                            {});
                                        // Simpler: just zero-init an alloca and store pointer
                                        auto* ptrAlloca = compiler->builder->CreateAlloca(nestedData.StructType, nullptr, "rset_ptr_obj");
                                        compiler->builder->CreateStore(llvm::Constant::getNullValue(nestedData.StructType), ptrAlloca);
                                        emitFieldSets(nestedData, ptrAlloca, subAlloca);
                                        compiler->builder->CreateStore(ptrAlloca, gep);
                                        compiler->builder->CreateBr(mergeBB);

                                        compiler->builder->SetInsertPoint(mergeBB);
                                    }
                                }
                            };

                            // 5. Get obj pointer
                            llvm::Value* objPtr = nullptr;
                            if (isPtr)
                                objPtr = objNV.Primary ? objNV.Primary : compiler->CreateLoad(objNV.Storage);
                            else
                            {
                                objPtr = objNV.Storage;
                                if (!objPtr)
                                {
                                    LogErrorContext(ctx, "reflect_set(): cannot take address of temporary struct value");
                                    break;
                                }
                            }

                            emitFieldSets(sd, objPtr, srcAlloca);

                            namedVar = {};
                            break;
                        }

                        // Compile-time intrinsic: is_pointer(T) - returns 1 if the type parameter T
                        // resolves to a pointer type in the current generic instantiation, 0 otherwise.
                        // Useful with `if const` to branch on pointer vs value element types.
                        if (functionName == "is_pointer")
                        {
                            bool isPtr = false;
                            if (argumentList.size() > 0)
                            {
                                auto namedArgCtx = argumentList[functionArgCounter]->argumentNamedExpression();
                                if (!namedArgCtx.empty())
                                {
                                    std::string argText = namedArgCtx[0]->assignmentExpression()->getText();
                                    auto substIt = activeTypeSubstitutions.find(argText);
                                    if (substIt != activeTypeSubstitutions.end())
                                    {
                                        const std::string& resolved = substIt->second;
                                        isPtr = !resolved.empty() && resolved.back() == '*';
                                        // An interface value is a fat pointer { vtable*, data* },
                                        // so it belongs on the pointer (borrow) arm too.
                                        if (!isPtr)
                                        {
                                            std::string bare = resolved;
                                            StripOwnershipQualifiers(bare);
                                            isPtr = Compiler(ctx)->HasInterface(bare);
                                        }
                                    }
                                }
                            }
                            namedVar.Primary = llvm::ConstantInt::get(
                                llvm::Type::getInt1Ty(*Compiler(ctx)->context), isPtr ? 1 : 0);
                            namedVar.TypeAndValue.TypeName = "int";
                            break;
                        }

                        // Compile-time intrinsic: is_unique(T) - returns 1 if the type parameter T
                        // resolves to a `unique`-qualified (owning) type in the current instantiation,
                        // 0 otherwise. Orthogonal to is_pointer (unique Circle* is both). Outside a
                        // generic substitution context it returns 0 (same convention as is_pointer).
                        // Returns i1 typed "int" - use under `if const`, do not printf it directly.
                        if (functionName == "is_unique")
                        {
                            bool isUniq = false;
                            if (argumentList.size() > 0)
                            {
                                auto namedArgCtx = argumentList[functionArgCounter]->argumentNamedExpression();
                                if (!namedArgCtx.empty())
                                {
                                    std::string argText = namedArgCtx[0]->assignmentExpression()->getText();
                                    auto substIt = activeTypeSubstitutions.find(argText);
                                    if (substIt != activeTypeSubstitutions.end())
                                    {
                                        // A blessed core `unique<X>` substitution is the owning
                                        // spelling; the old qualifier marker no longer exists.
                                        const std::string& resolved = substIt->second;
                                        isUniq = Compiler(ctx)->IsCoreUniqueType(resolved);
                                    }
                                }
                            }
                            namedVar.Primary = llvm::ConstantInt::get(
                                llvm::Type::getInt1Ty(*Compiler(ctx)->context), isUniq ? 1 : 0);
                            namedVar.TypeAndValue.TypeName = "int";
                            break;
                        }

                        // Compile-time intrinsic: is_interface(T) - returns 1 if the type parameter T
                        // resolves to an interface VALUE (a fat pointer { vtable*, data* }), 0 otherwise.
                        // is_pointer(T) is also true for an interface value, so this is the
                        // discriminator a container needs when the two must behave differently.
                        if (functionName == "is_interface")
                        {
                            bool isIface = false;
                            if (argumentList.size() > 0)
                            {
                                auto namedArgCtx = argumentList[functionArgCounter]->argumentNamedExpression();
                                if (!namedArgCtx.empty())
                                {
                                    std::string argText = namedArgCtx[0]->assignmentExpression()->getText();
                                    auto substIt = activeTypeSubstitutions.find(argText);
                                    if (substIt != activeTypeSubstitutions.end())
                                    {
                                        std::string bare = substIt->second;
                                        StripOwnershipQualifiers(bare);
                                        if (!bare.empty() && bare.back() != '*')
                                            isIface = Compiler(ctx)->HasInterface(bare);
                                    }
                                }
                            }
                            namedVar.Primary = llvm::ConstantInt::get(
                                llvm::Type::getInt1Ty(*Compiler(ctx)->context), isIface ? 1 : 0);
                            namedVar.TypeAndValue.TypeName = "int";
                            break;
                        }

                        // Compile-time intrinsic: is_copyable(T) - returns 1 if a value of T can be
                        // copied, 0 if copying it would be refused. Mirrors the refusal condition in
                        // CreateOverloadedFunctionCall exactly: only a by-value struct with no copy()
                        // that transitively owns a `unique` pointer is non-copyable. Outside a generic
                        // substitution context it returns 0 (same convention as is_pointer/is_unique).
                        // Returns i1 typed "int" - use under `if const`, do not printf it directly.
                        if (functionName == "is_copyable")
                        {
                            bool isCopyable = false;
                            if (argumentList.size() > 0)
                            {
                                auto namedArgCtx = argumentList[functionArgCounter]->argumentNamedExpression();
                                if (!namedArgCtx.empty())
                                {
                                    std::string argText = namedArgCtx[0]->assignmentExpression()->getText();
                                    auto substIt = activeTypeSubstitutions.find(argText);
                                    if (substIt != activeTypeSubstitutions.end())
                                    {
                                        auto* be = Compiler(ctx);
                                        std::string base = LegacyUniqueSubstitutionSpelling(
                                            be, substIt->second);
                                        StripOwnershipQualifiers(base);
                                        // Shared predicate: a pointer (bare/unique/alias) or a
                                        // non-struct type is copyable; a struct is refused only by
                                        // the unique-owner rule (drives the copy-on-assign flip too).
                                        isCopyable = be->IsCopyableType(base);
                                    }
                                }
                            }
                            namedVar.Primary = llvm::ConstantInt::get(
                                llvm::Type::getInt1Ty(*Compiler(ctx)->context), isCopyable ? 1 : 0);
                            namedVar.TypeAndValue.TypeName = "int";
                            break;
                        }

                        // Compile-time intrinsic: compile_error("msg") - raises a compile error with
                        // the given message when this branch is INSTANTIATED (live for the current
                        // monomorphization); a no-op in dead `if const` branches (never codegen'd).
                        // Used by list.cb to reject copy() of a unique-element list.
                        if (functionName == "compile_error")
                        {
                            std::string msg = "compile_error";
                            if (argumentList.size() > 0)
                            {
                                auto namedArgCtx = argumentList[functionArgCounter]->argumentNamedExpression();
                                if (!namedArgCtx.empty())
                                {
                                    // Argument must be a string LITERAL - DequoteStringLiteral just
                                    // strips the first/last char, so a bare identifier would yield
                                    // garbage. Reject anything not quoted.
                                    std::string argText = namedArgCtx[0]->assignmentExpression()->getText();
                                    if (argText.size() < 2 || argText.front() != '"' || argText.back() != '"')
                                    {
                                        LogErrorContext(ctx, "compile_error requires a string literal");
                                        break;
                                    }
                                    msg = DequoteStringLiteral(argText);
                                }
                            }
                            // Deferred: methods are instantiated eagerly (whole class body), so firing
                            // now would reject mere declaration. Poison the enclosing function; the
                            // error fires later only if it is actually CALLED (CheckPoisonedFunctionCalls).
                            auto* curBlock = Compiler(ctx)->builder->GetInsertBlock();
                            if (curBlock != nullptr && curBlock->getParent() != nullptr)
                                Compiler(ctx)->poisonedFunctions[curBlock->getParent()->getName().str()] = msg;
                            namedVar.Primary = llvm::ConstantInt::get(
                                llvm::Type::getInt1Ty(*Compiler(ctx)->context), 0);
                            namedVar.TypeAndValue.TypeName = "int";
                            break;
                        }

                        // Compile-time intrinsic: is_primitive(T) - returns 1 if T resolves to a primitive
                        // type (integer, float, bool, void), 0 otherwise. Use with `if const` to branch
                        // on primitive vs struct element types in generic data structures.
                        if (functionName == "is_primitive")
                        {
                            static const std::unordered_set<std::string> kPrimitiveTypes = {
                                "bool", "void",
                                "char", "i8", "i16", "i32", "i64",
                                "u8", "u16", "u32", "u64",
                                "short", "int", "long", "ulong",
                                "float", "double",
                            };
                            bool isPrim = false;
                            if (argumentList.size() > 0)
                            {
                                auto namedArgCtx = argumentList[functionArgCounter]->argumentNamedExpression();
                                if (!namedArgCtx.empty())
                                {
                                    std::string argText = namedArgCtx[0]->assignmentExpression()->getText();
                                    auto substIt = activeTypeSubstitutions.find(argText);
                                    if (substIt != activeTypeSubstitutions.end())
                                    {
                                        std::string base = substIt->second;
                                        StripOwnershipQualifiers(base);
                                        isPrim = kPrimitiveTypes.count(base) > 0;
                                    }
                                }
                            }
                            namedVar.Primary = llvm::ConstantInt::get(
                                llvm::Type::getInt1Ty(*Compiler(ctx)->context), isPrim ? 1 : 0);
                            namedVar.TypeAndValue.TypeName = "int";
                            break;
                        }

                        // Compile-time intrinsic: is_string(T) - returns 1 if T resolves to the
                        // built-in `string` value type, 0 otherwise. Use with `if const` to deep-copy
                        // string elements in generic containers (string owns a heap buffer, so a
                        // shallow element copy would alias the buffer and double-free).
                        if (functionName == "is_string")
                        {
                            bool isStr = false;
                            if (argumentList.size() > 0)
                            {
                                auto namedArgCtx = argumentList[functionArgCounter]->argumentNamedExpression();
                                if (!namedArgCtx.empty())
                                {
                                    std::string argText = namedArgCtx[0]->assignmentExpression()->getText();
                                    auto substIt = activeTypeSubstitutions.find(argText);
                                    if (substIt != activeTypeSubstitutions.end())
                                    {
                                        std::string base = substIt->second;
                                        StripOwnershipQualifiers(base);
                                        isStr = (base == "string");
                                    }
                                }
                            }
                            namedVar.Primary = llvm::ConstantInt::get(
                                llvm::Type::getInt1Ty(*Compiler(ctx)->context), isStr ? 1 : 0);
                            namedVar.TypeAndValue.TypeName = "int";
                            break;
                        }

                        /*
                         * Compile-time intrinsic: embed("path") - folds a file's bytes into a
                         * private read-only constant global. The path resolves against the
                         * directory of the source file that WRITES the call, imports included.
                         * The result shape comes from the destination type: `string` when the
                         * destination is a string, `u8[N]` (N = the file size) otherwise.
                         */
                        if (functionName == "embed")
                        {
                            auto* compiler = Compiler(ctx);
                            auto namedArgCtx = argumentList.empty()
                                ? std::vector<CFlatParser::ArgumentNamedExpressionContext*>()
                                : argumentList[functionArgCounter]->argumentNamedExpression();
                            if (namedArgCtx.size() != 1)
                            {
                                LogErrorContext(ctx, "embed takes exactly one argument: a string "
                                    "literal path, as in 'embed(\"data/payload.bin\")'");
                                break;
                            }
                            std::string argText = namedArgCtx[0]->assignmentExpression()->getText();
                            if (argText.size() < 2 || argText.front() != '"' || argText.back() != '"')
                            {
                                LogErrorContext(ctx, "embed requires a string literal path, as in "
                                    "'embed(\"data/payload.bin\")'");
                                break;
                            }
                            std::string literalPath = ProcessRawText(argText, false);
                            std::string resolvedPath;
                            std::string embedError;
                            auto bytes = compiler->LoadEmbedFile(
                                literalPath, compiler->GetSourceFilePath(), resolvedPath, embedError);
                            if (!bytes.has_value())
                            {
                                LogErrorContext(ctx, std::format(
                                    "embed(\"{}\") cannot be read: {}", literalPath, embedError));
                                break;
                            }
                            compiler->RecordEmbedDependency(resolvedPath);

                            // `string` destination: byte-exact length, no NUL needed.
                            const auto& embedDest = declExpectedType;
                            if (embedDest.TypeName == "string" && !embedDest.Pointer
                                && embedDest.ConstArraySize == 0 && !embedDest.IsArrayView)
                            {
                                namedVar = compiler->MakeStringLiteralNV(
                                    std::string(bytes->begin(), bytes->end()));
                                break;
                            }
                            if (!embedDest.TypeName.empty() && embedDest.TypeName != "u8")
                            {
                                LogErrorContext(ctx, std::format(
                                    "embed(\"{}\") produces bytes; its destination must be spelled "
                                    "'u8[]' or 'string', not '{}'", literalPath,
                                    SpellType(*compiler, embedDest)));
                                break;
                            }

                            // An EXPLICIT extent must match the asset; only 'u8[]' sizes itself.
                            if (embedDest.TypeName == "u8" && !embedDest.Pointer
                                && !embedDest.IsArrayView && embedDest.ConstArraySize != 0
                                && embedDest.ConstArraySize != bytes->size())
                            {
                                LogErrorContext(ctx, std::format(
                                    "embed(\"{}\") holds {} bytes but the destination declares "
                                    "{} elements; write 'u8[]' to take the asset's own size",
                                    literalPath, bytes->size(), embedDest.ConstArraySize));
                                break;
                            }

                            auto* byteTy = llvm::Type::getInt8Ty(*compiler->context);
                            auto* arrTy = llvm::ArrayType::get(byteTy, bytes->size());
                            auto* init = llvm::ConstantDataArray::get(
                                *compiler->context, llvm::ArrayRef<uint8_t>(*bytes));
                            auto* gv = new llvm::GlobalVariable(
                                *compiler->module, arrTy, true, llvm::GlobalValue::PrivateLinkage,
                                init, "embed");
                            gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
                            namedVar = LLVMBackend::NamedVariable();
                            namedVar.Storage = gv;
                            namedVar.BaseType = arrTy;
                            namedVar.TypeAndValue.TypeName = "u8";
                            namedVar.TypeAndValue.ConstArraySize = bytes->size();
                            break;
                        }

                        // Hardware intrinsic: __rdtscp() - serializing read of the x86 CPU
                        // timestamp counter (RDTSCP). Returns an i64 raw cycle count. x86/Intel
                        // target only; callers should guard with `if const (__X86__)`. Wrapped
                        // by rdtscp() in time.cb for measuring loop jitter at sub-100ns resolution.
                        if (functionName == "__rdtscp")
                        {
                            namedVar.Primary = Compiler(ctx)->CreateRdtscp();
                            namedVar.Storage = nullptr;
                            namedVar.BaseType = namedVar.Primary->getType();
                            namedVar.TypeAndValue.TypeName = "i64";
                            break;
                        }

                        // Hardware intrinsic: __readcyclecounter() - target-independent read of
                        // the CPU cycle counter (llvm.readcyclecounter). Returns a u64 raw cycle
                        // count; lowers to RDTSC on x86 and the platform cycle register elsewhere.
                        // Non-serializing; wrapped by cycle_count() in intrinsic.cb.
                        if (functionName == "__readcyclecounter")
                        {
                            namedVar.Primary = Compiler(ctx)->CreateReadCycleCounter();
                            namedVar.Storage = nullptr;
                            namedVar.BaseType = namedVar.Primary->getType();
                            namedVar.TypeAndValue.TypeName = "u64";
                            break;
                        }

                        // Hardware intrinsic: __lfence() - emit the x86 LFENCE load/serializing
                        // fence. Returns nothing. x86/Intel target only; guard callers with
                        // `if const (__X86__)`. Wrapped by lfence() in time.cb; pair with rdtscp()
                        // to keep out-of-order execution from smearing a measured region.
                        if (functionName == "__lfence")
                        {
                            Compiler(ctx)->CreateLfence();
                            namedVar = {};
                            break;
                        }

                        // Hardware intrinsic: __pause() - emit the x86 PAUSE spin-loop hint.
                        // Returns nothing. x86/Intel target only; guard callers with
                        // `if const (__X86__)`. Wrapped by pause() in mutex.cb for spinlocks.
                        if (functionName == "__pause")
                        {
                            Compiler(ctx)->CreatePause();
                            namedVar = {};
                            break;
                        }

                        // Hardware intrinsic: __atomic_acquire_fence() - portable memory-ordering
                        // acquire fence (llvm.fence acquire). Returns nothing. Valid on every
                        // target: lowers to `dmb ishld` on arm64 and to no instruction on x86.
                        // Wrapped by fence_acquire() in intrinsic.cb.
                        if (functionName == "__atomic_acquire_fence")
                        {
                            Compiler(ctx)->CreateFenceAcquire();
                            namedVar = {};
                            break;
                        }

                        // Compiler intrinsics that take value arguments (bit ops, prefetch,
                        // fma, branch hints). Evaluate the argument expressions, then emit the
                        // matching LLVM intrinsic. These are wrapped by core/intrinsic.cb,
                        // core/math.cb (fma), so user code calls the friendly name.
                        if (functionName == "__popcount" || functionName == "__ctz" || functionName == "__clz" ||
                            functionName == "__prefetch"  || functionName == "__fma" ||
                            functionName == "__likely"    || functionName == "__unlikely")
                        {
                            std::vector<LLVMBackend::NamedVariable> argNVs;
                            if (argumentList.size() > 0)
                            {
                                auto namedArgCtx = argumentList[functionArgCounter]->argumentNamedExpression();
                                for (auto* namedArgument : namedArgCtx)
                                    argNVs.push_back(this->ParseAssignmentExpressionNamed(namedArgument->assignmentExpression()));
                            }
                            auto argValue = [&](size_t i) -> llvm::Value* {
                                return argNVs[i].Primary ? argNVs[i].Primary : LoadNamedVariable(argNVs[i]);
                            };

                            if (functionName == "__prefetch")
                            {
                                if (!argNVs.empty())
                                    Compiler(ctx)->CreatePrefetch(argValue(0));
                                namedVar = {};
                                break;
                            }

                            // The remaining intrinsics produce a value whose type mirrors an input.
                            namedVar = {};
                            if (functionName == "__popcount" || functionName == "__ctz" || functionName == "__clz")
                            {
                                llvm::Value* v = argValue(0);
                                namedVar.Primary = functionName == "__popcount" ? Compiler(ctx)->CreatePopcount(v)
                                                 : functionName == "__ctz"      ? Compiler(ctx)->CreateCtz(v)
                                                 :                                 Compiler(ctx)->CreateClz(v);
                                namedVar.TypeAndValue.TypeName = argNVs[0].TypeAndValue.TypeName;
                            }
                            else if (functionName == "__fma")
                            {
                                namedVar.Primary = Compiler(ctx)->CreateFma(argValue(0), argValue(1), argValue(2));
                                namedVar.TypeAndValue.TypeName = argNVs[0].TypeAndValue.TypeName;
                            }
                            else // __likely / __unlikely
                            {
                                namedVar.Primary = Compiler(ctx)->CreateExpect(argValue(0), functionName == "__likely");
                                namedVar.TypeAndValue.TypeName = "bool";
                            }
                            namedVar.Storage  = nullptr;
                            namedVar.BaseType = namedVar.Primary->getType();
                            break;
                        }

                        // Handle va_start / va_end - pass the va_list alloca address to the LLVM intrinsic.
                        if (functionName == "va_start" || functionName == "va_end")
                        {
                            if (argumentList.size() > 0)
                            {
                                auto namedArgCtx = argumentList[functionArgCounter]->argumentNamedExpression();
                                if (!namedArgCtx.empty())
                                {
                                    std::string varName = namedArgCtx[0]->assignmentExpression()->getText();
                                    auto vaVar = Compiler(ctx)->GetLocalVariable(varName);
                                    if (!vaVar.Storage) vaVar = Compiler(ctx)->GetFunctionArgument(varName);
                                    if (vaVar.Storage)
                                    {
                                        if (functionName == "va_start")
                                            Compiler(ctx)->CreateVaStart(vaVar.Storage);
                                        else
                                            Compiler(ctx)->CreateVaEnd(vaVar.Storage);
                                    }
                                }
                            }
                            namedVar = {};
                            break;
                        }

                        // [PFX-5] indirect call through a function<...> value (a loaded vtable slot,
                        // a callback field, or a thin/fat closure). No implicit receiver is prepended.
                        // Check if this is a function pointer variable - emit an indirect call.
                        // An encoded closure element (from list<Lambda<...>>.get / a field of a
                        // substituted encoded type) is a struct value, not IsFunctionPointer; recover
                        // its call descriptor from the registry (gap a).
                        const LLVMBackend::TypeAndValue* encClosure =
                            namedVar.TypeAndValue.IsFunctionPointer ? nullptr
                            : Compiler(ctx)->GetEncodedClosureType(namedVar.TypeAndValue.TypeName);
                        if (namedVar.TypeAndValue.IsFunctionPointer || encClosure != nullptr)
                        {
                            // Use-after-move at the CALLEE: invoking `f(...)` reads the closure `f`,
                            // so a moved-from closure local must be diagnosed exactly as a plain read
                            // would (mirrors LoadNamedVariable). Without this, `sink(move f); f(1)`
                            // dereferences a nulled env and segfaults instead of failing to compile.
                            if (namedVar.IdentifierLine > 0 && !namedVar.IsElementAccess)
                            {
                                Compiler(ctx)->RecordMoveUse(namedVar.CallerName, namedVar.FieldName,
                                                             namedVar.IdentifierLine, namedVar.IdentifierColumn);
                                if (auto moved = Compiler(ctx)->MovedUseSubject(namedVar); !moved.empty())
                                {
                                    Compiler(ctx)->currentLine = namedVar.IdentifierLine;
                                    Compiler(ctx)->currentColumn = namedVar.IdentifierColumn;
                                    Compiler(ctx)->LogError(std::format("use of moved variable '{}'", moved));
                                }
                            }
                            LLVMBackend::TypeAndValue funcPtrTV =
                                encClosure ? *encClosure : namedVar.TypeAndValue;
                            llvm::Value* funcPtr = nullptr;
                            // A union member's Storage is the union alloca (all fields alias at
                            // offset 0), so the load needs the FIELD type or it reads the union.
                            if (namedVar.Storage != nullptr && namedVar.UnionFieldType != nullptr)
                                funcPtr = Compiler(ctx)->CreateLoad(namedVar.UnionFieldType, namedVar.Storage);
                            else if (namedVar.Storage != nullptr)
                                funcPtr = Compiler(ctx)->CreateLoad(namedVar.Storage);
                            else if (namedVar.Primary != nullptr)
                            {
                                if (namedVar.TypeAndValue.IsAlias && !namedVar.TypeAndValue.Pointer
                                    && namedVar.Primary->getType()->isPointerTy())
                                    funcPtr = Compiler(ctx)->CreateLoad(
                                        Compiler(ctx)->GetType(namedVar.TypeAndValue), namedVar.Primary);
                                else
                                    funcPtr = namedVar.Primary;
                            }

                            if (funcPtr != nullptr)
                            {
                                std::vector<llvm::Value*> callArgs;
                                std::vector<LLVMBackend::NamedVariable> argNVs;
                                // Consumed-COM sugar ([PFX-2a]): the receiver was redirected through
                                // lpVtbl, so inject the captured object pointer as the implicit `this`
                                // first argument. A placeholder NamedVariable keeps argNVs 1:1 with
                                // callArgs for the move/bond checks below (`this` is never move/bonded).
                                if (pendingThinComReceiver != nullptr)
                                {
                                    callArgs.push_back(pendingThinComReceiver);
                                    argNVs.emplace_back();
                                    pendingThinComReceiver = nullptr;
                                }
                                if (argumentList.size() > 0)
                                {
                                    auto namedArgCtx = argumentList[functionArgCounter]->argumentNamedExpression();
                                    for (const auto& namedArgument : namedArgCtx)
                                    {
                                        size_t argParamIndex = argNVs.size();
                                        LLVMBackend::TypeAndValue argExpectedDest;
                                        std::optional<DeclExpectedTypeScope> argExpectedScope;
                                        if (argParamIndex < funcPtrTV.FuncPtrParams.size())
                                        {
                                            argExpectedDest = Compiler(ctx)->FuncPtrParamAsTypeAndValue(
                                                funcPtrTV.FuncPtrParams[argParamIndex], argParamIndex);
                                            argExpectedScope.emplace(&declExpectedType, argExpectedDest);
                                        }
                                        CallArgumentScope callArgumentScope(
                                            inCallArgument_, ternaryCallArgumentDepth_);
                                        auto argNV = this->ParseAssignmentExpressionNamed(namedArgument->assignmentExpression());
                                        if (argNV.ContainsBondedClosure)
                                            Compiler(ctx)->LogError(
                                                "cannot pass a holder containing a bonded closure to a function - the callee could stash it beyond the captured local's lifetime");
                                        auto argValue = argNV.Primary ? argNV.Primary : LoadNamedVariable(argNV);
                                        if (Compiler(ctx)->IsCoreUniqueToRawPointer(argNV, argExpectedDest))
                                            argValue = Compiler(ctx)->CreateCoreUniqueRawPointerCall(
                                                argNV, argExpectedDest);
                                        if (argNV.TypeAndValue.IsAlias && !argNV.TypeAndValue.Pointer
                                            && argNV.Primary != nullptr
                                            && (argNV.TypeAndValue.IsFunctionPointer
                                                || Compiler(ctx)->GetEncodedClosureType(argNV.TypeAndValue.TypeName) != nullptr))
                                            argValue = Compiler(ctx)->CreateLoad(
                                                Compiler(ctx)->GetType(argNV.TypeAndValue), argNV.Primary);
                                        if (argValue) callArgs.push_back(argValue);
                                        argNVs.push_back(argNV);

                                        // Compile-time use-after-move check: arg is a moved variable (or field).
                                        if (argNV.IdentifierLine > 0)
                                        {
                                            if (!argNV.IsElementAccess)
                                                Compiler(ctx)->RecordMoveUse(argNV.CallerName, argNV.FieldName,
                                                                             argNV.IdentifierLine, argNV.IdentifierColumn);
                                            if (auto moved = Compiler(ctx)->MovedUseSubject(argNV); !moved.empty())
                                            {
                                                // (Same diagnostic shape as direct-call site; conservative - only fires when source storage was already moved.)
                                                Compiler(ctx)->LogError(std::format("use of moved variable '{}'", moved));
                                            }
                                        }
                                    }
                                }
                                // Bond/move ownership-mismatch checks against the funcptr's per-param flags.
                                size_t pcount = std::min(argNVs.size(), funcPtrTV.FuncPtrParams.size());
                                // Pointer DEPTH: this door lowers its own argument list, so the
                                // scorer never sees it and `f(pp)` bound an `int(T*)` slot silently.
                                for (size_t i = 0; i < pcount; i++)
                                {
                                    auto why = Compiler(ctx)->FuncPtrArgDepthMismatch(
                                        argNVs[i], funcPtrTV.FuncPtrParams[i]);
                                    if (!why.empty())
                                        Compiler(ctx)->LogError(std::format(
                                            "call through function pointer '{}': parameter {} {}.",
                                            SpellFunctionSymbol(*Compiler(ctx), functionName), i, why));
                                }
                                for (size_t i = 0; i < pcount; i++)
                                {
                                    if (funcPtrTV.FuncPtrParams[i].IsMove && argNVs[i].IsBonded)
                                        Compiler(ctx)->LogError("cannot pass bonded value to 'move' parameter of function pointer - bonded values cannot be transferred out of their source's scope");
                                }
                                // [PFX-5a] INTERFACE-typed function-pointer parameter: box a concrete
                                // class/struct argument into a fat pointer (and upcast an already-boxed
                                // one), exactly as the direct-call path does.
                                for (size_t i = 0; i < pcount && i < callArgs.size(); i++)
                                {
                                    const auto& fp = funcPtrTV.FuncPtrParams[i];
                                    if (fp.Pointer || fp.TypeName.empty()) continue;
                                    if (argNVs[i].Storage == nullptr && argNVs[i].Primary == nullptr) continue;
                                    callArgs[i] = Compiler(ctx)->CoerceArgToInterface(
                                        argNVs[i], callArgs[i], fp.TypeName,
                                        std::format("function pointer '{}'",
                                            SpellFunctionSymbol(*Compiler(ctx), functionName)));
                                }
                                std::vector<llvm::Value*> rawArrayCounts(argNVs.size(), nullptr);
                                for (size_t i = 0; i < argNVs.size()
                                    && i < funcPtrTV.FuncPtrParams.size(); i++)
                                {
                                    const auto& fp = funcPtrTV.FuncPtrParams[i];
                                    if (fp.Pointer && fp.IsMove)
                                        rawArrayCounts[i] = Compiler(ctx)->RawArrayCountArgument(argNVs[i]);
                                }
                                Compiler(ctx)->ApplyFuncPtrSinkTransfer(
                                    functionName, funcPtrTV.FuncPtrParams, argNVs, true);
                                auto result = Compiler(ctx)->CreateIndirectCall(
                                    funcPtrTV, funcPtr, callArgs, &argNVs, &rawArrayCounts);
                                Compiler(ctx)->lastCallReturnsOwned = funcPtrTV.FuncPtrReturnOwned;
                                if (result != nullptr && funcPtrTV.FuncPtrReturnOwned)
                                {
                                    auto returnType = Compiler(ctx)->lastCallReturnType;
                                    returnType.IsMove = true;
                                    Compiler(ctx)->RegisterOwnedReturnTemp(result, functionName, returnType);
                                }
                                // A by-value owning-value struct return owns through a funcptr as through a
                                // direct call (no `move` on the type), so ledger it; `alias` returns stay out.
                                else if (result != nullptr
                                    && !funcPtrTV.FuncPtrReturnAlias
                                    && !Compiler(ctx)->lastCallReturnType.IsAlias
                                    && !Compiler(ctx)->lastCallReturnType.Pointer
                                    && Compiler(ctx)->lastCallReturnType.TypeName != "string"
                                    && Compiler(ctx)->IsOwningValueType(
                                        Compiler(ctx)->lastCallReturnType.TypeName))
                                {
                                    Compiler(ctx)->RegisterOwnedReturnTemp(
                                        result, functionName, Compiler(ctx)->lastCallReturnType);
                                }
                                // A lambda literal's INFERRED owning sinks ride the funcptr type;
                                // transfer the caller's source exactly as a direct call does.
                                Compiler(ctx)->ApplyFuncPtrSinkTransfer(
                                    functionName, funcPtrTV.FuncPtrParams, argNVs, false);
                                // Diagnose explicit move-to-borrow here; ownership transfer is
                                // centralized in ApplyFuncPtrSinkTransfer below.
                                for (size_t i = 0; i < pcount; i++)
                                {
                                    if (i < argNVs.size())
                                        Compiler(ctx)->DiagnoseExplicitMoveToBorrowParam(
                                            functionName, std::format("{}", i),
                                            funcPtrTV.FuncPtrParams[i].TypeName,
                                            funcPtrTV.FuncPtrParams[i].IsMove
                                                || funcPtrTV.FuncPtrParams[i].IsOwningSink,
                                            argNVs[i]);
                                }
                                // The call RESULT is a fresh value, not a read of the callee, so it must
                                // inherit no callee provenance - rebuild it rather than clear flags.
                                namedVar = {};
                                namedVar.Primary = result;
                                namedVar.BaseType = result ? result->getType() : nullptr;
                                namedVar.TypeAndValue = Compiler(ctx)->lastCallReturnType;
                                PrepareAliasCallResult(ctx, namedVar);
                                structVar = {};
                                interfaceVar = {};
                                ClassifyPostfixCallResult(ctx, namedVar, structVar, interfaceVar);
                                /*
                                 * A VOID call through a function value yields no LLVM value at all
                                 * (CreateIndirectCall returns nullptr for a void invoker), so every
                                 * consumer downstream would read an unwritten slot, drop the argument,
                                 * or dereference the null - silently on some paths, as a locationless
                                 * verifier dump or a compiler SIGSEGV on others. Reject at the CALL,
                                 * where the position is still known, rather than at each consumer.
                                 * Discard positions want exactly this, and `return <call>;` is settled
                                 * by EmitReturnExpression (a void crossing out of a void function is
                                 * legal), so both defer.
                                 */
                                // Resolve through `using V = void;` - the spelling is aliasable, so
                                // matching the literal "void" would leave the alias reading garbage.
                                if (result == nullptr && use == ResultUse::Value
                                    && !namedVar.TypeAndValue.Pointer
                                    && Compiler(ctx)->ResolveTypeAlias(namedVar.TypeAndValue.TypeName) == "void")
                                {
                                    // Past the first call in a chain (`make()()`, `fns.get(0)()`),
                                    // primaryIdentifier still names the PRODUCER - drop it instead.
                                    std::string subject = functionArgCounter > 0
                                        ? std::string("the call result")
                                        : std::format("function value '{}'",
                                            SpellFunctionSymbol(*Compiler(ctx), functionName));
                                    LogErrorContext(ctx, std::format(
                                        "call through {} returns 'void', so it produces "
                                        "no value to consume - call it as a statement", subject));
                                }
                                functionArgCounter++;
                                break;
                            }
                        }

                        // Check if this is a return-block function - inline it at the call site.
                        // A 'return' inside the block returns from the caller function.
                        if (const auto* rb = Compiler(ctx)->GetReturnBlock(functionName))
                        {
                            // A return-block call in a VALUE context is invalid: the block's inner
                            // 'return' emits a ret that exits the CALLER, so there is no SSA value to
                            // consume. Reject before inlining (emitting after the ret would leave a
                            // terminator mid-block -> module verification failure).
                            if (use != ResultUse::Discard)
                            {
                                LogErrorContext(ctx, std::format(
                                    "return-block function '{}' cannot be called in a value context (its "
                                    "'return' exits the caller, not the block) - call it as a bare statement",
                                    SpellFunctionSymbol(*Compiler(ctx), functionName)));
                                // A '?.' earlier in this chain may have left the insert point mid-chain
                                // (an "access" block with no terminator yet) - close it before bailing.
                                if (!Compiler(ctx)->IsBlockTerminated())
                                    Compiler(ctx)->builder->CreateUnreachable();
                                return {};
                            }

                            // The inlined 'return' emits a ret against the CALLER's declared return type.
                            // If the return-block's declared return type does not match the caller's, LLVM
                            // fails module verification (ret type mismatch). Reject with a clear diagnostic.
                            const std::string& callerRetName = Compiler(ctx)->currentFunctionReturnTypeName;
                            bool callerRetIsPtr = Compiler(ctx)->currentFunction != nullptr
                                && Compiler(ctx)->currentFunction->getReturnType()->isPointerTy();
                            if (rb->ReturnType.TypeName != callerRetName
                                || rb->ReturnType.Pointer != callerRetIsPtr)
                            {
                                std::string callerName = Compiler(ctx)->currentFunction != nullptr
                                    ? Compiler(ctx)->currentFunction->getName().str() : "<unknown>";
                                LogErrorContext(ctx, std::format(
                                    "return-block function '{}' returns '{}' but the calling function '{}' "
                                    "declares return type '{}' - the inlined 'return' would not match the "
                                    "caller's return type",
                                    SpellFunctionSymbol(*Compiler(ctx), functionName),
                                    SpellType(*Compiler(ctx), rb->ReturnType),
                                    SpellFunctionSymbol(*Compiler(ctx), callerName),
                                    SpellType(*Compiler(ctx), Compiler(ctx)->currentFunctionReturnTV)));
                                if (!Compiler(ctx)->IsBlockTerminated())
                                    Compiler(ctx)->builder->CreateUnreachable();
                                return {};
                            }

                            Compiler(ctx)->InitializeBlock(nullptr, true);

                            // Determine if the first param is an implicit 'this'
                            size_t paramOffset = (!rb->Params.empty() &&
                                rb->Params[0].VariableName.ends_with("__")) ? 1 : 0;

                            // Bind 'this' if present
                            if (paramOffset > 0)
                            {
                                LLVMBackend::NamedVariable thisVar;
                                if (structVar.BaseType)
                                {
                                    thisVar = structVar;
                                    thisVar.TypeAndValue.VariableName = "";
                                }
                                else
                                {
                                    thisVar = Compiler(ctx)->GetCurrentMemberThis(functionName);
                                }
                                if (thisVar.Storage != nullptr)
                                {
                                    const auto& thisParam = rb->Params[0];
                                    LLVMBackend::TypeAndValue tv;
                                    tv.TypeName = thisParam.TypeName;
                                    tv.VariableName = thisParam.VariableName;
                                    tv.Pointer = thisParam.Pointer;
                                    auto* alloca = Compiler(ctx)->CreateLocalVariable(tv);
                                    // thisVar.Storage may be a promoted-param alloca holding a pointer;
                                    // load through it to get the actual pointer value to bind.
                                    llvm::Value* thisVal = thisVar.Storage;
                                    if (thisVar.TypeAndValue.Pointer
                                        && llvm::isa<llvm::AllocaInst>(thisVal))
                                        thisVal = Compiler(ctx)->CreateLoad(thisVal);
                                    Compiler(ctx)->CreateAssignment(thisVal, alloca);
                                }
                            }

                            // Parse and bind explicit parameters to local variables
                            if (argumentList.size() > 0)
                            {
                                auto namedArgCtx = argumentList[functionArgCounter]->argumentNamedExpression();
                                for (size_t i = 0; i < namedArgCtx.size() && (i + paramOffset) < rb->Params.size(); ++i)
                                {
                                    auto argValue = this->ParseAssignmentExpression(namedArgCtx[i]->assignmentExpression());
                                    const auto& param = rb->Params[i + paramOffset];
                                    LLVMBackend::TypeAndValue tv;
                                    tv.TypeName = param.TypeName;
                                    tv.VariableName = param.VariableName;
                                    tv.Pointer = param.Pointer;
                                    if (tv.Pointer)
                                    {
                                        // Pointer params: store value directly as Primary so GetValue()
                                        // returns the pointer itself rather than the alloca address.
                                        Compiler(ctx)->RegisterPrimaryVariable(tv, argValue);
                                    }
                                    else
                                    {
                                        auto* alloca = Compiler(ctx)->CreateLocalVariable(tv);
                                        Compiler(ctx)->CreateAssignment(argValue, alloca);
                                    }
                                }
                            }

                            if (auto* blockItems = rb->Body->blockItemList())
                                ParseBlockItemList(blockItems);

                            Compiler(ctx)->CreateBlockBreak(nullptr, true);

                            // The block's 'return' already terminated the caller's basic block.
                            // Return an empty namedVar - callers must tolerate null after a terminator.
                            namedVar = {};
                        }
                        else if (interfaceVar.TypeAndValue.IsInterface)
                        {
                            bool receiverIsFixedArray = isFixedArrayReceiver();
                            bool receiverIsCharStringConversion = primaryIdentifier == "toString"
                                && ((interfaceVar.TypeAndValue.TypeName == "char"
                                     && interfaceVar.TypeAndValue.ConstArraySize > 0)
                                    || (namedVar.TypeAndValue.TypeName == "char"
                                        && namedVar.TypeAndValue.ConstArraySize > 0));
                            if (receiverIsFixedArray && !receiverIsCharStringConversion)
                                LogErrorContext(primaryCtx, std::format(
                                    "no overload of '{}' matches the given arguments.", primaryIdentifier));

                            // [PFX-nc-iface] '?.': test the receiver BEFORE the argument list, so a
                            // null receiver skips the arguments. Both arms share this one guard.
                            llvm::Value* ncIfacePtr = nullptr;
                            if (nullConditionalPending)
                            {
                                auto* compiler = Compiler(ctx);
                                ncIfacePtr = interfaceVar.Storage;
                                if (ncIfacePtr == nullptr && interfaceVar.Primary != nullptr)
                                {
                                    auto fatTy = compiler->GetFatPtrType();
                                    ncIfacePtr = compiler->CreateAlloca(fatTy);
                                    compiler->CreateAssignment(interfaceVar.Primary, ncIfacePtr);
                                }
                                // No Storage and no Primary (an unspillable receiver): fall through
                                // unguarded - nullConditionalPending stays armed but unconsumed here.
                                if (ncIfacePtr != nullptr)
                                {
                                    // Fat {vtable,data} receiver: test the DATA slot (index 1), not
                                    // ifacePtr itself (always a live alloca address, never null).
                                    auto* fatTy = compiler->GetFatPtrType();
                                    auto* ptrTy = cflat_llvm::PointerTo(compiler->builder->getInt8Ty());
                                    auto* dataField = compiler->CreateStructGEP(fatTy, ncIfacePtr, 1);
                                    auto* dataPtr = compiler->CreateLoad(ptrTy, dataField);
                                    ncEnterGuard(dataPtr);
                                }
                            }

                            // Collect extra call arguments
                            std::vector<LLVMBackend::NamedVariable> extraArgs;
                            if (argumentList.size() > 0)
                            {
                                auto namedArgCtx = argumentList[functionArgCounter]->argumentNamedExpression();
                                // The interface method's declared params (excluding 'this'), so a lambda
                                // literal argument gets its expected signature - without it the lambda's
                                // return type defaults to void and its `ret` fails module verification.
                                const std::vector<LLVMBackend::TypeAndValue>* ifaceParams =
                                    Compiler(ctx)->GetInterfaceMethodParams(
                                        interfaceVar.TypeAndValue.TypeName, primaryIdentifier,
                                        namedArgCtx.size());
                                // A named argument makes the call-site index diverge from the declared
                                // one, so map through the same binder MatchFunction uses.
                                std::vector<int64_t> declaredIdx(namedArgCtx.size());
                                for (size_t i = 0; i < declaredIdx.size(); ++i)
                                    declaredIdx[i] = (int64_t)i;
                                if (ifaceParams != nullptr)
                                {
                                    std::vector<std::string> argNames;
                                    argNames.reserve(namedArgCtx.size());
                                    for (const auto& na : namedArgCtx)
                                        argNames.push_back(na->Identifier() ? na->Identifier()->getText()
                                                                            : std::string());
                                    auto binding = LLVMBackend::ComputeArgumentPositions(
                                        argNames, *ifaceParams, false);
                                    if (binding.Ok) declaredIdx = binding.PosMap;
                                }
                                for (size_t argIdx = 0; argIdx < namedArgCtx.size(); ++argIdx)
                                {
                                    const auto& namedArgument = namedArgCtx[argIdx];
                                    auto argName = namedArgument->Identifier();
                                    lambdaExpectedType = {};
                                    // The matched PARAMETER is the destination context inside this
                                    // argument, so an enclosing declarator's type cannot leak in.
                                    LLVMBackend::TypeAndValue ifaceArgExpectedDest;
                                    if (ifaceParams != nullptr && declaredIdx[argIdx] >= 0
                                        && declaredIdx[argIdx] < (int64_t)ifaceParams->size())
                                    {
                                        const auto& p = (*ifaceParams)[declaredIdx[argIdx]];
                                        ifaceArgExpectedDest = p;
                                        if (p.IsFunctionPointer) lambdaExpectedType = p;
                                        else if (const auto* enc = Compiler(ctx)->GetEncodedClosureType(p.TypeName))
                                            lambdaExpectedType = *enc;
                                    }
                                    // Occurrence-scope this argument (see codeValueDataCasts_): a
                                    // cast here must not launder a sibling argument's join arm.
                                    size_t savedCastOcc = Compiler(ctx)->BeginCastOccurrence();
                                    size_t thisCastOcc = Compiler(ctx)->CurrentCastOccurrence();
                                    std::optional<DeclExpectedTypeScope> ifaceArgExpectedScope;
                                    ifaceArgExpectedScope.emplace(&declExpectedType, ifaceArgExpectedDest);
                                    CallArgumentScope callArgumentScope(
                                        inCallArgument_, ternaryCallArgumentDepth_);
                                    auto argNV = this->ParseAssignmentExpressionNamed(namedArgument->assignmentExpression());
                                    ifaceArgExpectedScope.reset();
                                    lambdaExpectedType = {};
                                    // Use-after-move check: a field access carries a populated Primary, so the
                                    // LoadNamedVariable check below is skipped for it - check explicitly here so
                                    // re-moving a field (or any moved variable) is rejected uniformly.
                                    bool coreUniqueRawBorrow = !ifaceArgExpectedDest.IsMove
                                        && !ifaceArgExpectedDest.IsUnique
                                        && ifaceArgExpectedDest.Pointer
                                        && !argNV.TypeAndValue.Pointer
                                        && Compiler(ctx)->IsCoreUniqueType(argNV.TypeAndValue.TypeName);
                                    if (argNV.IdentifierLine > 0 && !coreUniqueRawBorrow)
                                    {
                                        if (!argNV.IsElementAccess)
                                            Compiler(ctx)->RecordMoveUse(argNV.CallerName, argNV.FieldName,
                                                                         argNV.IdentifierLine, argNV.IdentifierColumn);
                                        if (auto moved = Compiler(ctx)->MovedUseSubject(argNV); !moved.empty())
                                        {
                                            Compiler(ctx)->currentLine = argNV.IdentifierLine;
                                            Compiler(ctx)->currentColumn = argNV.IdentifierColumn;
                                            Compiler(ctx)->LogError(std::format("use of moved variable '{}'", moved));
                                        }
                                    }
                                    auto argValue = argNV.Primary ? argNV.Primary : LoadNamedVariable(argNV);
                                    if (argNV.TypeAndValue.IsAlias && !argNV.TypeAndValue.Pointer
                                        && argNV.Primary != nullptr
                                        && (argNV.TypeAndValue.IsFunctionPointer
                                            || Compiler(ctx)->GetEncodedClosureType(argNV.TypeAndValue.TypeName) != nullptr))
                                        argValue = Compiler(ctx)->CreateLoad(
                                            Compiler(ctx)->GetType(argNV.TypeAndValue), argNV.Primary);
                                    if (!argValue) { Compiler(ctx)->EndCastOccurrence(savedCastOcc); break; }
                                    // An owned-string CALL result passed as a by-value (borrow) argument
                                    // has no named owner and must be freed at end-of-full-expression -
                                    // same rule as the direct-call arg loop. string.dtor's owned-bit
                                    // check makes this a safe no-op for a borrowed (alias) result.
                                    if (Compiler(ctx)->IsProducedTempValue(argValue)
                                        && argValue->getType() == llvm::StructType::getTypeByName(
                                               *Compiler(ctx)->context, "string"))
                                        Compiler(ctx)->RegisterOwnedStringTemp(argValue);
                                    LLVMBackend::NamedVariable argVar;
                                    // Named-argument binding uses the INTERFACE's declared parameter
                                    // names, not the implementor's - matches virtual dispatch.
                                    if (argName)
                                        argVar.TypeAndValue.VariableName = argName->getText();
                                    argVar.Primary = argValue;
                                    argVar.BaseType = argValue->getType();
                                    argVar.TernaryTempAlreadyRegistered = argNV.TernaryTempAlreadyRegistered;
                                    argVar.Storage = argNV.Storage;
                                    argVar.IsOwning = argNV.IsOwning;
                                    argVar.OwnsInterfaceBox = argNV.OwnsInterfaceBox;
                                    argVar.IsAdoptable = argNV.IsAdoptable;
                                    argVar.IsOwningString = argNV.IsOwningString;
                                    argVar.IsOwningStruct = argNV.IsOwningStruct;
                                    argVar.AllocatedByRawNewArray = argNV.AllocatedByRawNewArray;
                                    argVar.RawArrayLength = argNV.RawArrayLength;
                                    argVar.RawArrayLengthStorage = argNV.RawArrayLengthStorage;
                                    // Explicit 'move' at the call site: drives move-overload selection
                                    // and the borrow-param diagnostic after overload resolution.
                                    argVar.IsExplicitMove = argNV.IsExplicitMove;
                                    // Propagate over-alignment so a `move` param that would inherit the block
                                    // without its alignment tag is rejected instead of mis-freed.
                                    argVar.AllocAlignment = argNV.AllocAlignment;
                                    argVar.TypeAndValue.Pointer = argNV.TypeAndValue.Pointer;
                                    argVar.TypeAndValue.IsMove = argNV.TypeAndValue.IsMove;
                                    argVar.TypeAndValue.IsUnique = argNV.TypeAndValue.IsUnique;
                                    argVar.TypeAndValue.IsBorrowOfAliasElement =
                                        argNV.TypeAndValue.IsBorrowOfAliasElement;
                                    // Propagate the pointer SHAPE flags: without them a `T**` or a
                                    // `T[]` looks like a thin `T*` and gets boxed into an interface param.
                                    argVar.TypeAndValue.ElemPointer = argNV.TypeAndValue.ElemPointer;
                                    // The POSITIVE depth is the argument-side proof the depth gates
                                    // read; dropped here every argument looks depth-unrecorded.
                                    argVar.TypeAndValue.PointerDepth = argNV.TypeAndValue.PointerDepth;
                                    argVar.TypeAndValue.IsArrayView = argNV.TypeAndValue.IsArrayView;
                                    // Same for a FIXED `T[N]`: LoadNamedVariable already decayed argValue
                                    // to the element-0 address, so ConstArraySize is the only surviving
                                    // signal that this argument was an array, not a plain value - the
                                    // funcptr shape gate (FunctionPointerShapeOf) needs it to tell
                                    // `function<T>[N]` apart from a bare `function<T>` at the call site.
                                    argVar.TypeAndValue.ConstArraySize = argNV.TypeAndValue.ConstArraySize;
                                    argVar.CallerName = argNV.CallerName;
                                    // Per-field move tracking: moving `node->left` marks only that field.
                                    argVar.FieldName = argNV.FieldName;
                                    argVar.MovedFields = argNV.MovedFields;
                                    argVar.TypeAndValue.IsInterface = argNV.TypeAndValue.IsInterface;
                                    argVar.IsBonded = argNV.IsBonded;
                                    argVar.BondByAddress = argNV.BondByAddress;
                                    argVar.BondedSources = argNV.BondedSources;
                                    argVar.ContainsBondedClosure = argNV.ContainsBondedClosure;
                                    // Propagate lambda capture names so a capturing lambda passed to a
                                    // thin `function<>` parameter names its captures in the rejection
                                    // diagnostic, exactly as the direct call path does.
                                    // Argument PROVENANCE for a closure parameter: without it the
                                    // widen guard in LowerByValueArg cannot tell a `function<>`
                                    // value from an arbitrary data pointer. TypeName is left alone
                                    // so overload scoring is unaffected.
                                    argVar.TypeAndValue.IsFunctionPointer = argNV.TypeAndValue.IsFunctionPointer;
                                    // The SIGNATURE too: the overload scorer rejects a function
                                    // pointer whose signature disagrees with the parameter's.
                                    argVar.TypeAndValue.FuncPtrReturnTypeName = argNV.TypeAndValue.FuncPtrReturnTypeName;
                                    argVar.TypeAndValue.FuncPtrReturnPointer  = argNV.TypeAndValue.FuncPtrReturnPointer;
                                    argVar.TypeAndValue.FuncPtrReturnOwned = argNV.TypeAndValue.FuncPtrReturnOwned;
                                    argVar.TypeAndValue.FuncPtrParams         = argNV.TypeAndValue.FuncPtrParams;
                                    // Reading the side channel RETIRES it, like the two other
                                    // consumers, so a stale list cannot reach the next argument.
                                    argVar.LambdaCaptureNames = argNV.LambdaCaptureNames;
                                    if (argVar.LambdaCaptureNames.empty() && lastLambdaType.IsFunctionPointer)
                                    {
                                        argVar.LambdaCaptureNames = Compiler(ctx)->lastCallLambdaCaptureNames;
                                        Compiler(ctx)->lastCallLambdaCaptureNames.clear();
                                    }

                                    // Preserve unsigned-integer TypeName so Upconvert (LowerByValueArg)
                                    // chooses ZExt over SExt - without it a u8 200 arrives as -56.
                                    if (argNV.TypeAndValue.IsUnsignedInteger() != -1)
                                        argVar.TypeAndValue.TypeName = argNV.TypeAndValue.TypeName;

                                    // An INTERFACE argument's LLVM type is the shared fat-ptr struct, so the
                                    // struct-name extraction below would name it "__iface_fat_ptr" and the
                                    // derived->parent upcast (IText -> IElement) would be silently skipped -
                                    // storing the derived vtable in a parent slot, whose dtor index differs.
                                    if (argNV.TypeAndValue.IsInterface && !argNV.TypeAndValue.IsInterfacePointer)
                                    {
                                        argVar.TypeAndValue.TypeName = argNV.TypeAndValue.TypeName;
                                    }
                                    // Extract struct name if this is a struct type
                                    else if (auto* st = llvm::dyn_cast<llvm::StructType>(argValue->getType()))
                                    {
                                        auto structName = st->getName().str();
                                        if (!structName.empty())
                                            argVar.TypeAndValue.TypeName = structName;
                                    }

                                    // Propagate struct TypeName for pointer args so struct*->interface upcast works
                                    if (argVar.TypeAndValue.TypeName.empty() && argNV.TypeAndValue.Pointer
                                        && !argNV.TypeAndValue.TypeName.empty()
                                        && Compiler(ctx)->IsDataStructure(argNV.TypeAndValue.TypeName))
                                    {
                                        argVar.TypeAndValue.TypeName = argNV.TypeAndValue.TypeName;
                                    }

                                    // Same '??'-join recovery as the direct-call arg loop; here the
                                    // method's own declared params ARE the candidate set.
                                    if (argVar.TypeAndValue.TypeName.empty() && ifaceParams != nullptr
                                        && declaredIdx[argIdx] >= 0
                                        && declaredIdx[argIdx] < (int64_t)ifaceParams->size())
                                    {
                                        std::vector<const LLVMBackend::TypeAndValue*> paramsHere{
                                            &(*ifaceParams)[declaredIdx[argIdx]] };
                                        std::string joinIface;
                                        if (auto* fat = BoxPointerJoinArgument(paramsHere, argValue, joinIface))
                                        {
                                            argVar.Primary = fat;
                                            argVar.BaseType = fat->getType();
                                            argVar.TypeAndValue.TypeName = joinIface;
                                            argVar.TypeAndValue.IsInterface = true;
                                            argVar.TypeAndValue.Pointer = false;
                                        }
                                    }

                                    argVar.CastOccurrenceId = thisCastOcc;
                                    Compiler(ctx)->EndCastOccurrence(savedCastOcc);
                                    extraArgs.emplace_back(argVar);
                                }
                            }

                            if (Compiler(ctx)->HasInterfaceMethod(interfaceVar.TypeAndValue.TypeName, primaryIdentifier))
                            {
                                // Interface method dispatch via vtable.
                                if (interfaceVar.Primary != nullptr
                                    && Compiler(ctx)->IsNonOwningStructJoin(interfaceVar.Primary))
                                {
                                    LogErrorContext(ctx, std::format(
                                        "cannot call interface method '{}' on a mixed owning/borrowed "
                                        "ternary join - rewrite it as an if/else and bind each arm separately",
                                        primaryIdentifier));
                                }
                                // Ensure we have a {i8*,i8*}* pointer (alloca address).
                                // If the interface value was produced inline (Primary set, no Storage),
                                // spill it into a temp alloca first.
                                // A guarded chain already spilled/derived the receiver address above.
                                llvm::Value* ifacePtr = ncIfacePtr ? ncIfacePtr : interfaceVar.Storage;
                                if (ifacePtr == nullptr && interfaceVar.Primary != nullptr)
                                {
                                    auto fatTy = Compiler(ctx)->GetFatPtrType();
                                    ifacePtr = Compiler(ctx)->CreateAlloca(fatTy);
                                    Compiler(ctx)->CreateAssignment(interfaceVar.Primary, ifacePtr);
                                }
                                // A core unique interface stores its fat value in a wrapper field.
                                // Add a whole-wrapper witness for the uninitialized check; the normal
                                // field-path witness cannot distinguish a hidden declaration splat
                                // from a genuinely initialized wrapper.
                                if (!interfaceVar.CallerName.empty()
                                    && interfaceVar.Storage != nullptr
                                    && interfaceVar.TypeAndValue.IsInterface)
                                {
                                    auto owner = Compiler(ctx)->GetScopedLocalOrArgument(
                                        interfaceVar.CallerName);
                                    if (!owner.TypeAndValue.Pointer
                                        && Compiler(ctx)->IsCoreUniqueType(owner.TypeAndValue.TypeName)
                                        && owner.Storage != nullptr
                                        && owner.Storage != interfaceVar.Storage)
                                    {
                                        LLVMBackend::NullIfaceDispatchSite wrapperSite;
                                        wrapperSite.VarName = interfaceVar.CallerName;
                                        wrapperSite.MemberName = primaryIdentifier;
                                        wrapperSite.Line = (int)ctx->getStart()->getLine();
                                        wrapperSite.Col = (int)ctx->getStart()->getCharPositionInLine();
                                        Compiler(ctx)->RecordPendingNullIfaceDispatch(
                                            wrapperSite, owner.Storage, interfaceVar.Primary,
                                            interfaceVar.TypeAndValue.TypeName);
                                    }
                                }
                                bool cleanupInlineOwnedReceiver = interfaceVar.Primary != nullptr
                                    && interfaceVar.Storage == nullptr
                                    && Compiler(ctx)->FindOwnedReturnEntry(interfaceVar.Primary) != nullptr;

                                // Definitely-null dispatch: record the receiver only when it is a
                                // NAMED local's own slot dispatched with a plain '.'. A '?.' chain
                                // is the sanctioned spelling for a maybe-null receiver, and a
                                // spilled temp names no variable, so neither is recorded.
                                LLVMBackend::NullIfaceDispatchSite ncSite;
                                const LLVMBackend::NullIfaceDispatchSite* ncSitePtr = nullptr;
                                if (ncIfacePtr == nullptr && !nullConditionalPending
                                    && interfaceVar.Storage != nullptr && ifacePtr == interfaceVar.Storage)
                                {
                                    ncSite.VarName = interfaceVar.CallerName.empty()
                                        ? interfaceVar.TypeAndValue.VariableName : interfaceVar.CallerName;
                                    ncSite.ReceiverText = nullIfaceRecvText;
                                    ncSite.MemberName = primaryIdentifier;
                                    ncSite.Line = (int)ctx->getStart()->getLine();
                                    ncSite.Col = (int)ctx->getStart()->getCharPositionInLine();
                                    ncSitePtr = &ncSite;
                                }

                                namedVar.Primary = Compiler(ctx)->CallInterfaceMethod(
                                    ifacePtr,
                                    interfaceVar.TypeAndValue.TypeName,
                                    primaryIdentifier,
                                    extraArgs,
                                    ncSitePtr
                                );
                                if (cleanupInlineOwnedReceiver && ifacePtr != nullptr)
                                {
                                    Compiler(ctx)->SuppressCallerRelease(interfaceVar.Primary);
                                    auto* receiver = Compiler(ctx)->CreateLoad(
                                        Compiler(ctx)->GetFatPtrType(), ifacePtr);
                                    Compiler(ctx)->DeleteInterfaceValue(
                                        receiver, interfaceVar.TypeAndValue.TypeName, ifacePtr);
                                }
                                namedVar.Storage = nullptr;
                                namedVar.BaseType = namedVar.Primary ? namedVar.Primary->getType() : nullptr;
                                // Populate TypeAndValue from the interface method's return type and
                                // re-classify the result so a chained member/method call on it works
                                // (e.g. `e.toJson().data()` or `e.mk().get()`). Without this the result
                                // has no struct receiver and the chained call re-dispatches the stale name.
                                if (namedVar.Primary)
                                    namedVar.TypeAndValue = Compiler(ctx)->lastCallReturnType;
                                PrepareAliasCallResult(ctx, namedVar);
                                interfaceVar = {};
                                structVar = {};
                                ClassifyPostfixCallResult(ctx, namedVar, structVar, interfaceVar);
                            }
                            else
                            {
                                // Extension method: find a standalone or generic function and
                                // pass the interface value as the first argument.
                                std::string extFuncName;
                                // Bare spelling inside a namespace reaches that namespace's key first.
                                std::string gfIface = Compiler(ctx)->ResolveGenericFunctionBase(primaryIdentifier);
                                if (genericFunctionTemplates.count(gfIface))
                                    extFuncName = InferAndInstantiateGenericFunction(gfIface, interfaceVar.TypeAndValue.TypeName);
                                if (extFuncName.empty())
                                    extFuncName = primaryIdentifier;

                                std::vector<LLVMBackend::NamedVariable> allArgs;
                                LLVMBackend::NamedVariable ifaceArg = interfaceVar;
                                ifaceArg.TypeAndValue.VariableName = "";
                                allArgs.push_back(ifaceArg);
                                for (const auto& e : extraArgs)
                                    allArgs.push_back(e);

                                // The receiver guard for this arm is emitted at [PFX-nc-iface], before
                                // the argument list, so a null receiver skips the arguments too.
                                namedVar.Primary = Compiler(ctx)->CreateOverloadedFunctionCall(extFuncName, allArgs);
                                namedVar.Storage = nullptr;
                                namedVar.BaseType = namedVar.Primary ? namedVar.Primary->getType() : nullptr;
                                // Mirror the interface-method path: carry the return type and
                                // re-classify so a chained call on the result resolves.
                                if (namedVar.Primary)
                                    namedVar.TypeAndValue = Compiler(ctx)->lastCallReturnType;
                                interfaceVar = {};
                                structVar = {};
                                ClassifyPostfixCallResult(ctx, namedVar, structVar, interfaceVar);
                            }
                        }
                        else
                        {
                            // UFCS on a `char*` / `char[N]` receiver: decay the receiver to an i8*
                            // and bind it as the `self` argument so `p.toString()` dispatches to a
                            // free function `toString(char* self)`. A `char[N]` receiver decays to a
                            // pointer to its first element (the array alloca address), mirroring how a
                            // char array decays in argument position. Scoped to a `char` element type
                            // and gated on an actual `char* self` candidate, so a char* never
                            // coerce-matches a `string self` overload (the inbound char*->string wrap
                            // stays out of method-receiver binding). Runs before the integer/float
                            // primitive intercept below because a `char` pointer is "integer-like" by
                            // TypeName yet must not be treated as a scalar value receiver.
                            if (!structVar.BaseType && !primaryIdentifier.empty()
                                && namedVar.TypeAndValue.TypeName == "char")
                            {
                                bool charPtrRecv = namedVar.TypeAndValue.Pointer
                                    && !namedVar.TypeAndValue.IsArrayView;
                                bool charArrRecv = !namedVar.TypeAndValue.Pointer
                                    && namedVar.TypeAndValue.ConstArraySize > 0
                                    && namedVar.Storage != nullptr;

                                auto hasCharPtrSelf = [&]() -> bool {
                                    auto it = Compiler(ctx)->functionTable.find(primaryIdentifier);
                                    if (it == Compiler(ctx)->functionTable.end()) return false;
                                    for (const auto& sym : it->second)
                                        if (!sym.Parameters.empty()
                                            && sym.Parameters[0].TypeName == "char"
                                            && sym.Parameters[0].Pointer)
                                            return true;
                                    return false;
                                };

                                if ((charPtrRecv || charArrRecv) && hasCharPtrSelf())
                                {
                                    // Array alloca address already points at element 0 (opaque ptr);
                                    // a char* receiver loads its stored pointer value.
                                    llvm::Value* charPtr = charArrRecv
                                        ? namedVar.Storage
                                        : (namedVar.Storage
                                            ? (namedVar.UnionFieldType
                                                ? Compiler(ctx)->CreateLoad(namedVar.UnionFieldType, namedVar.Storage)
                                                : Compiler(ctx)->CreateLoad(namedVar.Storage))
                                            : namedVar.Primary);
                                    if (charPtr != nullptr)
                                    {
                                        // Mirror a normal char* argument: leave TypeName empty so
                                        // overload scoring routes through CompareUpconvert on the
                                        // pointer BaseType (a `char` TypeName would let the integer-rank
                                        // path perfect-match a scalar i8/char param). Pointer flag is
                                        // carried for accuracy; the scorer keys off BaseType here.
                                        LLVMBackend::NamedVariable selfArg;
                                        selfArg.Primary  = charPtr;
                                        selfArg.BaseType = charPtr->getType();
                                        selfArg.TypeAndValue.Pointer = true;
                                        structVar = selfArg;
                                    }
                                }
                            }

                            // Intercept primitive method calls:
                            //   float/double: f.round(), (-2.5f).abs(), etc.
                            //   integer:      x.to_i32(), n.to_u64(), etc.
                            if (!structVar.BaseType && !primaryIdentifier.empty())
                            {
                                // A union member's Storage is the union alloca; load the MEMBER type
                                // or the receiver value is the whole union.
                                llvm::Value* primVal = namedVar.Storage
                                    ? (namedVar.UnionFieldType
                                        ? Compiler(ctx)->CreateLoad(namedVar.UnionFieldType, namedVar.Storage)
                                        : (namedVar.TypeAndValue.IsAlias && !namedVar.TypeAndValue.Pointer
                                            ? Compiler(ctx)->CreateLoad(
                                                Compiler(ctx)->GetType(namedVar.TypeAndValue), namedVar.Storage)
                                            : Compiler(ctx)->CreateLoad(namedVar.Storage)))
                                    : namedVar.Primary;

                                if (primVal != nullptr && primVal->getType()->isFloatingPointTy())
                                {
                                    auto* result = Compiler(ctx)->CreateFloatIntrinsic(primaryIdentifier, primVal);
                                    if (result)
                                    {
                                        namedVar.Primary  = result;
                                        namedVar.Storage  = nullptr;
                                        namedVar.BaseType = result->getType();
                                        structVar = {};
                                        break;
                                    }
                                }
                                else if (primVal != nullptr && primVal->getType()->isIntegerTy())
                                {
                                    auto* result = Compiler(ctx)->CreateIntegerConvert(primaryIdentifier, primVal);
                                    if (result)
                                    {
                                        namedVar.Primary  = result;
                                        namedVar.Storage  = nullptr;
                                        namedVar.BaseType = result->getType();
                                        // Strip the "to_" prefix to get the CFlat type name (e.g. "i32", "u64").
                                        namedVar.TypeAndValue = {};
                                        namedVar.TypeAndValue.TypeName = primaryIdentifier.substr(3);
                                        structVar = {};
                                        break;
                                    }
                                }

                                // General UFCS on a primitive: dispatch `value.func(args)` to a free
                                // function `func(value, args)` (e.g. n.toString(), n.toString(16)). The
                                // float/int intrinsic attempts above take precedence; this only runs when
                                // none matched. The value becomes the self argument via structVar so the
                                // normal dispatch below pushes it as the first argument.
                                if (primVal != nullptr
                                    && (primVal->getType()->isIntegerTy() || primVal->getType()->isFloatingPointTy())
                                    && Compiler(ctx)->GetFunction(primaryIdentifier))
                                {
                                    LLVMBackend::NamedVariable selfArg;
                                    selfArg.Primary  = primVal;
                                    selfArg.BaseType = primVal->getType();
                                    selfArg.TypeAndValue.TypeName = namedVar.TypeAndValue.TypeName;
                                    structVar = selfArg;
                                }
                            }

                            // [PFX-6] argument assembly: prepend the receiver as implicit `this`
                            // (or inject the enclosing method's `this` for a bare call), then evaluate
                            // the user arguments into `arguments`.
                            std::vector<LLVMBackend::NamedVariable> arguments;
                            if (structVar.BaseType)
                            {
                                // LoadNamedVariable is not called for receivers, so check here.
                                CheckMovedReceiver(structVar);
                                LLVMBackend::NamedVariable argumentNamedVar = structVar; // Copy;
                                argumentNamedVar.TypeAndValue.VariableName = "";
                                // An owning-value rvalue temp receiver is dropped after the call - destruct it.
                                RegisterOwningTempReceiver(ctx, structVar, argumentNamedVar, functionName);
                                arguments.push_back(argumentNamedVar);
                            }
                            else if (!globalScopeCall)
                            {
                                // Bare call inside a member function - inject 'this' automatically
                                // if the callee is a method of the same struct. A `global::` call
                                // explicitly targets the root, so never inject an implicit 'this'.
                                auto thisVar = Compiler(ctx)->GetCurrentMemberThis(functionName);
                                if (thisVar.Storage != nullptr)
                                    arguments.push_back(thisVar);
                            }

                            // [PFX-7-slot] Resolved before [PFX-7] because the '?.' guard below must
                            // skip a COM slot call, which owns its own '?.'/HResult lowering.
                            const LLVMBackend::DeclTypeAndValue* winrtSlot =
                                (structVar.BaseType && !structVar.TypeAndValue.TypeName.empty())
                                ? Compiler(primaryCtx)->GetWinrtSlot(structVar.TypeAndValue.TypeName, functionName)
                                : nullptr;
                            // [PFX-nc-struct] '?.' short-circuit: emit the receiver null test BEFORE the
                            // argument list so a null receiver skips the arguments' side effects too.
                            bool ncCallGuarded = false;
                            if (winrtSlot == nullptr && nullConditionalPending && structVar.Storage != nullptr)
                            {
                                ncEnterGuard(structVar.Storage);
                                ncCallGuarded = true;
                            }

                            auto evaluateCallArguments = [&]()
                            {
                                if (argumentList.size() > 0)
                                {
                                    auto namedArgCtx = argumentList[functionArgCounter]->argumentNamedExpression();

                                // Look up the function signature to set lambdaExpectedType for lambda arguments.
                                const LLVMBackend::FunctionSymbol* funcSym = nullptr;
                                {
                                    auto it = Compiler(ctx)->functionTable.find(functionName);
                                    if (it != Compiler(ctx)->functionTable.end() && !it->second.empty())
                                    {
                                        // Select the overload matching the receiver type (and arity), not
                                        // front(): several types can share a method name (e.g. an atomic's
                                        // lock(this) release(val, body) vs an unrelated struct's release()).
                                        // front() could read the wrong param list and drop the lock(this)
                                        // guard-seeding, wrongly rejecting access to guarded fields.
                                        const std::string& recvType = structVar.TypeAndValue.TypeName;
                                        if (recvType.empty())
                                        {
                                            funcSym = &it->second.front();
                                        }
                                        else
                                        {
                                            size_t wantParams = arguments.size() + namedArgCtx.size();
                                            const LLVMBackend::FunctionSymbol* typeMatch = nullptr;
                                            for (const auto& cand : it->second)
                                            {
                                                if (cand.Parameters.empty()
                                                    || cand.Parameters.front().TypeName != recvType)
                                                    continue;
                                                if (!typeMatch) typeMatch = &cand;
                                                if (cand.Parameters.size() == wantParams) { funcSym = &cand; break; }
                                            }
                                            if (!funcSym) funcSym = typeMatch ? typeMatch : &it->second.front();
                                        }
                                    }
                                }
                                size_t paramOffset = arguments.empty() ? 0 : 1; // offset for implicit 'this'

                                // A named argument makes the call-site index diverge from the declared
                                // one, so map through the same binder MatchFunction uses.
                                std::vector<int64_t> declaredIdx(namedArgCtx.size());
                                for (size_t i = 0; i < declaredIdx.size(); ++i)
                                    declaredIdx[i] = (int64_t)(i + paramOffset);
                                if (funcSym != nullptr)
                                {
                                    std::vector<std::string> argNames;
                                    argNames.reserve(namedArgCtx.size());
                                    for (const auto& na : namedArgCtx)
                                        argNames.push_back(na->Identifier() ? na->Identifier()->getText()
                                                                            : std::string());
                                    auto binding = LLVMBackend::ComputeArgumentPositions(
                                        argNames, funcSym->Parameters, funcSym->Variadic, paramOffset);
                                    if (binding.Ok) declaredIdx = binding.PosMap;
                                }

                                for (size_t argIdx = 0; argIdx < namedArgCtx.size(); ++argIdx)
                                {
                                    const auto& namedArgument = namedArgCtx[argIdx];

                                    // '...' in call position: forward this function's variadic args as a va_list.
                                    if (namedArgument->Ellipsis())
                                    {
                                        if (!currentFunctionIsVariadic)
                                        {
                                            LogErrorContext(ctx, "'...' forwarding can only be used inside a variadic function");
                                            break;
                                        }
                                        if (!Compiler(ctx)->autoVaListAlloca)
                                        {
                                            LLVMBackend::TypeAndValue vaTv;
                                            vaTv.TypeName = "va_list";
                                            vaTv.VariableName = "__va_forward";
                                            Compiler(ctx)->autoVaListAlloca = llvm::cast<llvm::AllocaInst>(Compiler(ctx)->CreateLocalVariable(vaTv));
                                            Compiler(ctx)->CreateVaStart(Compiler(ctx)->autoVaListAlloca);
                                        }
                                        llvm::Value* vaValue = Compiler(ctx)->CreateLoad(Compiler(ctx)->autoVaListAlloca);
                                        LLVMBackend::NamedVariable argVar;
                                        argVar.Primary = vaValue;
                                        argVar.BaseType = vaValue->getType();
                                        argVar.TypeAndValue.TypeName = "va_list";
                                        arguments.emplace_back(argVar);
                                        continue;
                                    }

                                    // Field initializer argument: { field=val, ... } or paramName: { field=val, ... }
                                    if (namedArgument->initializerList())
                                    {
                                        auto* argNameToken = namedArgument->Identifier();
                                        std::string namedParam = argNameToken ? argNameToken->getText() : "";
                                        // Positional brace-init: use the bound slot, not the call-site
                                        // index, so an earlier named argument cannot shift it.
                                        int effectiveIdx = namedParam.empty() ? (int)declaredIdx[argIdx] : -1;
                                        std::string structType = ResolveInitializerArgType(ctx, functionName, effectiveIdx, namedParam);
                                        if (!structType.empty())
                                        {
                                            LLVMBackend::DeclTypeAndValue paramType;
                                            paramType.TypeName = structType;
                                            llvm::Value* defaultVal = GenerateDefaultValue(paramType);
                                            if (defaultVal)
                                            {
                                                auto* alloca = Compiler(ctx)->CreateAlloca(defaultVal->getType());
                                                Compiler(ctx)->CreateAssignment(defaultVal, alloca);
                                                CallArgumentScope braceTernaryScope(
                                                    inCallArgument_, ternaryCallArgumentDepth_);
                                                EmitFieldInitializer(alloca, structType, namedArgument->initializerList());
                                                llvm::Value* loaded = Compiler(ctx)->CreateLoad(alloca);
                                                LLVMBackend::NamedVariable argVar;
                                                argVar.Primary = loaded;
                                                argVar.BaseType = loaded->getType();
                                                argVar.TypeAndValue.TypeName = structType;
                                                argVar.TypeAndValue.VariableName = namedParam;
                                                arguments.emplace_back(argVar);
                                            }
                                        }
                                        continue;
                                    }

                                    // Set expected type when function expects a function-pointer at this position.
                                    // If the parameter is lock(this), also seed lambdaLockThisReceiver so the
                                    // lambda body gets currentLockSet seeded with the call-site receiver guard.
                                    lambdaExpectedType = {};
                                    lambdaLockThisReceiver = {};
                                    lambdaLockThisMode = LockMode::Exclusive;
                                    // The matched PARAMETER is the destination context inside this
                                    // argument, so an enclosing declarator's type cannot leak in.
                                    LLVMBackend::TypeAndValue argExpectedDest;
                                    if (funcSym && declaredIdx[argIdx] >= 0
                                        && declaredIdx[argIdx] < (int64_t)funcSym->Parameters.size())
                                    {
                                        const auto& paramTv = funcSym->Parameters[declaredIdx[argIdx]];
                                        argExpectedDest = paramTv;
                                        if (paramTv.IsFunctionPointer)
                                        {
                                            lambdaExpectedType = paramTv;
                                            // If the parameter is declared lock(this), build the qualified
                                            // guard name (e.g. "d.ready") to seed currentLockSet in the lambda body.
                                            if (paramTv.LockThis)
                                            {
                                                const std::string& parent = structVar.TypeAndValue.ParentVariableName;
                                                const std::string& field  = structVar.TypeAndValue.VariableName;
                                                if (!parent.empty() && !field.empty())
                                                    lambdaLockThisReceiver = parent + "." + field;
                                                else if (!field.empty())
                                                    lambdaLockThisReceiver = field;
                                                lambdaLockThisMode = paramTv.LockThisMode;
                                            }
                                        }
                                        // Encoded closure param (list<Lambda<...>>::add's `T value`, gap a):
                                        // seed the lambda's expected signature from the registry so its body
                                        // types its return/params correctly.
                                        else if (const auto* enc = Compiler(ctx)->GetEncodedClosureType(paramTv.TypeName))
                                        {
                                            lambdaExpectedType = *enc;
                                        }
                                    }
                                    auto argName = namedArgument->Identifier();
                                    // Occurrence-scope this argument (see codeValueDataCasts_): a
                                    // cast here must not launder a sibling argument's join arm.
                                    size_t savedCastOcc = Compiler(ctx)->BeginCastOccurrence();
                                    size_t thisCastOcc = Compiler(ctx)->CurrentCastOccurrence();
                                    std::optional<DeclExpectedTypeScope> argExpectedScope;
                                    argExpectedScope.emplace(&declExpectedType, argExpectedDest);
                                    CallArgumentScope callArgumentScope(
                                        inCallArgument_, ternaryCallArgumentDepth_);
                                    auto argNV = this->ParseAssignmentExpressionNamed(namedArgument->assignmentExpression());
                                    argExpectedScope.reset();
                                    if (argNV.ContainsBondedClosure)
                                        Compiler(ctx)->LogError(
                                            "cannot pass a holder containing a bonded closure to a function - the callee could stash it beyond the captured local's lifetime");
                                    // Use-after-move check: a field access carries a populated Primary, so the
                                    // LoadNamedVariable check below is skipped for it - check explicitly here so
                                    // re-moving a field (or any moved variable) is rejected uniformly.
                                    bool coreUniqueRawBorrow = !argExpectedDest.IsMove
                                        && !argExpectedDest.IsUnique
                                        && argExpectedDest.Pointer
                                        && !argNV.TypeAndValue.Pointer
                                        && Compiler(ctx)->IsCoreUniqueType(argNV.TypeAndValue.TypeName);
                                    if (argNV.IdentifierLine > 0 && !coreUniqueRawBorrow)
                                    {
                                        if (!argNV.IsElementAccess)
                                            Compiler(ctx)->RecordMoveUse(argNV.CallerName, argNV.FieldName,
                                                                         argNV.IdentifierLine, argNV.IdentifierColumn);
                                        if (auto moved = Compiler(ctx)->MovedUseSubject(argNV); !moved.empty())
                                        {
                                            Compiler(ctx)->currentLine = argNV.IdentifierLine;
                                            Compiler(ctx)->currentColumn = argNV.IdentifierColumn;
                                            Compiler(ctx)->LogError(std::format("use of moved variable '{}'", moved));
                                        }
                                    }
                                    // Load from storage if Primary isn't populated (simple variable reference)
                                    auto argValue = argNV.Primary ? argNV.Primary : LoadNamedVariable(argNV);
                                    if (argNV.TypeAndValue.IsAlias && !argNV.TypeAndValue.Pointer
                                        && argNV.Primary != nullptr
                                        && (argNV.TypeAndValue.IsFunctionPointer
                                            || Compiler(ctx)->GetEncodedClosureType(argNV.TypeAndValue.TypeName) != nullptr))
                                        argValue = Compiler(ctx)->CreateLoad(
                                            Compiler(ctx)->GetType(argNV.TypeAndValue), argNV.Primary);
                                    lambdaExpectedType = {};
                                    // caller's block was terminated (e.g. return-block inline)
                                    if (!argValue) { Compiler(ctx)->EndCastOccurrence(savedCastOcc); break; }
                                    LLVMBackend::NamedVariable argVar;

                                    if (argName)
                                        argVar.TypeAndValue.VariableName = argName->getText();
                                    argVar.Primary = argValue;
                                    argVar.BaseType = argValue->getType();
                                    argVar.TernaryTempAlreadyRegistered = argNV.TernaryTempAlreadyRegistered;
                                    // Propagate caller variable name for compile-time move tracking.
                                    argVar.CallerName = argNV.CallerName;
                                    // Propagate the field name (and inherited per-field move set) so that
                                    // moving a struct sub-path (`node->left`) marks only that field, not the
                                    // whole base variable - letting recursive owning-pointer-tree code compile.
                                    argVar.FieldName = argNV.FieldName;
                                    argVar.MovedFields = argNV.MovedFields;
                                    // Propagate storage and ownership so move-param zeroing works at the call site.
                                    argVar.Storage = argNV.Storage;
                                    argVar.IsOwning = argNV.IsOwning;
                                    argVar.OwnsInterfaceBox = argNV.OwnsInterfaceBox;
                                    argVar.IsAdoptable = argNV.IsAdoptable;
                                    argVar.IsOwningString = argNV.IsOwningString;
                                    argVar.IsOwningStruct = argNV.IsOwningStruct;
                                    argVar.AllocatedByRawNewArray = argNV.AllocatedByRawNewArray;
                                    argVar.RawArrayLength = argNV.RawArrayLength;
                                    argVar.RawArrayLengthStorage = argNV.RawArrayLengthStorage;
                                    // Explicit 'move' at the call site: drives move-overload selection
                                    // and the borrow-param diagnostic after overload resolution.
                                    argVar.IsExplicitMove = argNV.IsExplicitMove;
                                    // Propagate over-alignment so a `move` param that would inherit the block
                                    // without its alignment tag is rejected instead of mis-freed.
                                    argVar.AllocAlignment = argNV.AllocAlignment;
                                    argVar.TypeAndValue.Pointer = argNV.TypeAndValue.Pointer;
                                    argVar.TypeAndValue.IsMove = argNV.TypeAndValue.IsMove;
                                    argVar.TypeAndValue.IsUnique = argNV.TypeAndValue.IsUnique;
                                    // Propagate the array-view flag so a `T[]` argument is still seen
                                    // as a view at the call site (otherwise the noalias gate would
                                    // false-reject a legitimate view passed to a `T[]` parameter).
                                    argVar.TypeAndValue.IsArrayView = argNV.TypeAndValue.IsArrayView;
                                    // Same for `T**`: without it the argument looks like a plain `T*` and a
                                    // pointer-to-pointer would be boxed into an interface parameter.
                                    argVar.TypeAndValue.ElemPointer = argNV.TypeAndValue.ElemPointer;
                                    // The POSITIVE depth is the argument-side proof the depth gates
                                    // read; dropped here every argument looks depth-unrecorded.
                                    argVar.TypeAndValue.PointerDepth = argNV.TypeAndValue.PointerDepth;
                                    // Same for a FIXED `T[N]`: argValue is already the decayed element-0
                                    // address (LoadNamedVariable), so ConstArraySize is the only surviving
                                    // signal that this was an array - needed by the funcptr shape gate
                                    // (FunctionPointerShapeOf) to tell `function<T>[N]` from a bare value.
                                    argVar.TypeAndValue.ConstArraySize = argNV.TypeAndValue.ConstArraySize;
                                    // A stored 'function<>'/'Lambda<>' argument's SIGNATURE, so the overload
                                    // scorer can reject a function pointer of a disagreeing signature.
                                    // TypeName and IsFunctionPointer are deliberately left alone here.
                                    argVar.TypeAndValue.FuncPtrReturnTypeName = argNV.TypeAndValue.FuncPtrReturnTypeName;
                                    argVar.TypeAndValue.FuncPtrReturnPointer  = argNV.TypeAndValue.FuncPtrReturnPointer;
                                    argVar.TypeAndValue.FuncPtrReturnOwned = argNV.TypeAndValue.FuncPtrReturnOwned;
                                    argVar.TypeAndValue.FuncPtrParams         = argNV.TypeAndValue.FuncPtrParams;
                                    // Propagate bond info so bond-to-move checks work at the call site.
                                    argVar.IsBonded = argNV.IsBonded;
                                    argVar.BondByAddress = argNV.BondByAddress;
                                    argVar.BondedSources = argNV.BondedSources;
                                    argVar.ContainsBondedClosure = argNV.ContainsBondedClosure;
                                    // Propagate lambda capture names: when the lambda arg went through
                                    // postfix processing, the names land on argNV (the line below covers
                                    // the direct-arg path where they arrive via the side-channel instead).
                                    argVar.LambdaCaptureNames = argNV.LambdaCaptureNames;

                                    // Preserve unsigned-integer TypeName so Upconvert can choose ZExt over SExt.
                                    if (argNV.TypeAndValue.IsUnsignedInteger() != -1)
                                        argVar.TypeAndValue.TypeName = argNV.TypeAndValue.TypeName;

                                    // Propagate interface type so overload resolution matches IFoo->IFoo parameters.
                                    if (argNV.TypeAndValue.IsInterface)
                                    {
                                        argVar.TypeAndValue.IsInterface = true;
                                        argVar.TypeAndValue.TypeName = argNV.TypeAndValue.TypeName;
                                    }

                                    // Preserve a desugared unique<T> source for the common call
                                    // adapter. Overload matching uses this identity to apply the
                                    // existing unique<T> -> T* borrow through get(), including
                                    // generic methods such as dictionary.set().
                                    if (Compiler(ctx)->IsCoreUniqueType(argNV.TypeAndValue.TypeName))
                                        argVar.TypeAndValue.TypeName = argNV.TypeAndValue.TypeName;

                                    // Extract struct name if this is a struct type
                                    if (argVar.TypeAndValue.TypeName.empty())
                                    {
                                        if (auto* st = llvm::dyn_cast<llvm::StructType>(argValue->getType()))
                                        {
                                            auto structName = st->getName().str();
                                            if (!structName.empty())
                                                argVar.TypeAndValue.TypeName = structName;
                                        }
                                    }

                                    // Propagate struct TypeName for pointer args (e.g. move expressions, new)
                                    // so struct*->interface* upcast matching works in ComputeOverloadFunction.
                                    // Only propagate for known struct types - primitive TypeNames (char, bool,
                                    // int, ...) must stay empty so LLVM-type comparison handles them correctly.
                                    if (argVar.TypeAndValue.TypeName.empty() && argNV.TypeAndValue.Pointer
                                        && !argNV.TypeAndValue.TypeName.empty()
                                        && Compiler(ctx)->IsDataStructure(argNV.TypeAndValue.TypeName))
                                    {
                                        argVar.TypeAndValue.TypeName = argNV.TypeAndValue.TypeName;
                                    }

                                    // A '??' join has no TypeName for the scorer's interface clause.
                                    // Resolve the target interface here, then box the join per arm.
                                    if (argVar.TypeAndValue.TypeName.empty())
                                    {
                                        // Arity-filtered: a same-named overload of different arity
                                        // is not a candidate here.
                                        std::vector<const LLVMBackend::TypeAndValue*> paramsHere;
                                        size_t wantParams = paramOffset + namedArgCtx.size();
                                        auto ftIt = Compiler(ctx)->functionTable.find(functionName);
                                        if (ftIt != Compiler(ctx)->functionTable.end())
                                            for (const auto& cand : ftIt->second)
                                                if (cand.Parameters.size() == wantParams
                                                    && declaredIdx[argIdx] >= 0
                                                    && declaredIdx[argIdx] < (int64_t)cand.Parameters.size())
                                                    paramsHere.push_back(&cand.Parameters[declaredIdx[argIdx]]);
                                        std::string joinIface;
                                        if (auto* fat = BoxPointerJoinArgument(paramsHere, argValue, joinIface))
                                        {
                                            argVar.Primary = fat;
                                            argVar.BaseType = fat->getType();
                                            argVar.TypeAndValue.TypeName = joinIface;
                                            argVar.TypeAndValue.IsInterface = true;
                                            argVar.TypeAndValue.Pointer = false;
                                        }
                                    }

                                    // Propagate function-pointer type for lambda arguments
                                    if (lastLambdaType.IsFunctionPointer && argValue != nullptr)
                                    {
                                        argVar.TypeAndValue = lastLambdaType;
                                        argVar.LambdaCaptureNames = Compiler(ctx)->lastCallLambdaCaptureNames;
                                        lastLambdaType = {};
                                        Compiler(ctx)->lastCallLambdaCaptureNames.clear();
                                    }

                                    // An owned-string CALL result passed as a by-value (borrow)
                                    // argument has no named owner and must be freed at end-of-full-
                                    // expression. The dispatch choke point registers most owned-string
                                    // returns but excludes 'copy' (whose result is also stored directly
                                    // by synthesized memberwise copy); a copy() used inline as an
                                    // argument is not stored anywhere, so register it here. A 'move'
                                    // param unregisters it in the call dispatch, and string.dtor's
                                    // owned-bit check makes a borrowed (alias) result a safe no-op.
                                    // Mirrors RegisterBorrowedStringOperandTemp for operator operands.
                                    if (argValue && Compiler(ctx)->IsProducedTempValue(argValue)
                                        && argValue->getType() == llvm::StructType::getTypeByName(
                                               *Compiler(ctx)->context, "string"))
                                        Compiler(ctx)->RegisterOwnedStringTemp(argValue);

                                    argVar.CastOccurrenceId = thisCastOcc;
                                    Compiler(ctx)->EndCastOccurrence(savedCastOcc);
                                    arguments.emplace_back(argVar);
                                }
                            }
                            };
                            bool deferHresultChainArguments = hresultChainPending && winrtSlot != nullptr;
                            if (!deferHresultChainArguments)
                                evaluateCallArguments();

                            // A fixed array is not its element for extension-method lookup. The
                            // char[N].toString() path above is the one intentional array UFCS case.
                            bool receiverIsFixedArray = isFixedArrayReceiver();
                            bool receiverIsCharStringConversion = functionName == "toString"
                                && ((structVar.TypeAndValue.TypeName == "char"
                                     && structVar.TypeAndValue.ConstArraySize > 0)
                                    || (namedVar.TypeAndValue.TypeName == "char"
                                        && namedVar.TypeAndValue.ConstArraySize > 0));
                            if (receiverIsFixedArray && !receiverIsCharStringConversion)
                            {
                                LogErrorContext(primaryCtx, std::format(
                                    "no overload of '{}' matches the given arguments.",
                                    SpellFunctionSymbol(*Compiler(primaryCtx), functionName)));
                            }

                            // [PFX-7] call lowering, three ways: a [winrt] COM vtable dispatch
                            // (recv->lpVtbl->slot), a null-conditional `?.` guarded call, or the
                            // normal overloaded (member/free/extension) function call.
                            // `winrtSlot` is resolved above at [PFX-7-slot].
                            if (winrtSlot)
                            {
                                // [winrt] COM sugar: recv->Slot(args) dispatches through the vtable as
                                // recv->lpVtbl->Slot(recv, args). `arguments[0]` is the receiver; user
                                // args follow. QI/AddRef/Release exist ONLY in the vtable, so this is the
                                // only by-name path to them.
                                auto buildWinrtArgVals = [&]()
                                {
                                    std::vector<llvm::Value*> argVals;
                                    argVals.push_back(structVar.Storage);
                                    for (size_t ai = 1; ai < arguments.size(); ai++)
                                    {
                                        auto& a = arguments[ai];
                                        llvm::Value* v = a.Primary ? a.Primary : LoadNamedVariable(a);
                                        if (v) argVals.push_back(v);
                                    }
                                    return argVals;
                                };
                                auto* compiler = Compiler(primaryCtx);
                                if (hresultChainPending)
                                {
                                    // HResult `?.`/`?->` chain: structVar is the unwrapped `.value`
                                    // object. Skip the call and propagate hr when the source HResult
                                    // failed; otherwise dispatch and keep the call's HResult. The
                                    // whole chain stays an HResult<U> (U = this call's logical return).
                                    std::string resultHr;
                                    if (auto it = compiler->winrtSlotHResultType_.find(
                                            structVar.TypeAndValue.TypeName + "::" + functionName);
                                        it != compiler->winrtSlotHResultType_.end())
                                        resultHr = it->second;
                                    auto resSD = resultHr.empty() ? LLVMBackend::StructData{}
                                                                  : compiler->GetDataStructure(resultHr);
                                    if (!resSD.StructType)
                                    {
                                        LogErrorContext(primaryCtx, "'?.'/'?->' chains a method whose result "
                                            "is not an HResult<T> (void-returning chained calls are not supported yet)");
                                        hresultChainPending = false;
                                    }
                                    else
                                    {
                                        auto* b = compiler->builder.get();
                                        auto* srcSD = compiler->GetDataStructure(hresultChainType).StructType;
                                        auto* hr = b->CreateLoad(b->getInt32Ty(),
                                            b->CreateStructGEP(srcSD, hresultChainStorage, 0), "chain.hr");
                                        auto* failed = b->CreateICmpSLT(hr, b->getInt32(0), "chain.failed");
                                        auto* resAlloca = compiler->AllocaAtEntry(resSD.StructType, nullptr, "chain.res");
                                        auto* okBB = compiler->CreateBasicBlock("chain.ok");
                                        auto* failBB = compiler->CreateBasicBlock("chain.fail");
                                        auto* mergeBB = compiler->CreateBasicBlock("chain.merge");
                                        b->CreateCondBr(failed, failBB, okBB);

                                        compiler->SwitchToBlock(okBB);
                                        evaluateCallArguments();
                                        auto argVals = buildWinrtArgVals();
                                        std::string rt2; bool rp2 = false;
                                        auto* okRes = compiler->EmitWinrtSlotCall(
                                            structVar.TypeAndValue.TypeName, functionName, argVals, rt2, rp2);
                                        if (okRes) b->CreateStore(okRes, resAlloca);
                                        // Drop the +1 on the intermediate. Ok path only (the fail path
                                        // produced no object), and the call above already deref'd it.
                                        if (hresultChainReleasePtr)
                                        {
                                            std::string rt3; bool rp3 = false;
                                            compiler->EmitWinrtSlotCall(hresultChainReleaseType, "Release",
                                                { hresultChainReleasePtr }, rt3, rp3);
                                        }
                                        b->CreateBr(mergeBB);

                                        compiler->SwitchToBlock(failBB);
                                        b->CreateStore(llvm::Constant::getNullValue(resSD.StructType), resAlloca);
                                        b->CreateStore(hr, b->CreateStructGEP(resSD.StructType, resAlloca, 0));
                                        b->CreateBr(mergeBB);

                                        compiler->SwitchToBlock(mergeBB);
                                        namedVar = {};
                                        namedVar.Primary = b->CreateLoad(resSD.StructType, resAlloca);
                                        namedVar.BaseType = resSD.StructType;
                                        namedVar.TypeAndValue.TypeName = resultHr;
                                        namedVar.TypeAndValue.Pointer = false;
                                        // The chain's own result is a slot call's [out,retval] too, so a
                                        // further `?.` link may release it under the same rules.
                                        lastWinrtSlotCallResult = namedVar.Primary;
                                    }
                                    hresultChainPending = false;
                                    hresultChainReleasePtr = nullptr;
                                    hresultChainReleaseType.clear();
                                    globalScopeCall = false;
                                }
                                else
                                {
                                    auto argVals = buildWinrtArgVals();
                                    namedVar = {};
                                    std::string winrtResultType;
                                    bool winrtResultPtr = false;
                                    namedVar.Primary = compiler->EmitWinrtSlotCall(
                                        structVar.TypeAndValue.TypeName, functionName, argVals,
                                        winrtResultType, winrtResultPtr);
                                    namedVar.BaseType = namedVar.Primary ? namedVar.Primary->getType() : nullptr;
                                    namedVar.TypeAndValue.TypeName = winrtResultType;
                                    namedVar.TypeAndValue.Pointer  = winrtResultPtr;
                                    lastWinrtSlotCallResult = namedVar.Primary;
                                    globalScopeCall = false;
                                }
                            }
                            else if (ncCallGuarded)
                            {
                                // The receiver guard (and the rest of the chain) was already entered
                                // at [PFX-nc-struct]; only the generic extension resolution is left.
                                std::string resolvedFuncName = functionName;
                                // Bare spelling inside a namespace reaches that namespace's key first.
                                std::string gfName = Compiler(ctx)->ResolveGenericFunctionBase(functionName);
                                if (!Compiler(ctx)->GetFunction(functionName) && genericFunctionTemplates.count(gfName))
                                {
                                    std::string structTypeName = structVar.TypeAndValue.TypeName;
                                    if (structTypeName.empty() && structVar.BaseType)
                                    {
                                        if (auto* st = llvm::dyn_cast<llvm::StructType>(structVar.BaseType))
                                            structTypeName = st->getName().str();
                                    }
                                    for (const auto& iface : Compiler(ctx)->GetStructInterfaces(structTypeName))
                                    {
                                        auto inst = InferAndInstantiateGenericFunction(gfName, iface);
                                        if (!inst.empty()) { resolvedFuncName = inst; break; }
                                    }
                                }

                                namedVar.Primary = Compiler(ctx)->CreateOverloadedFunctionCall(
                                    resolvedFuncName, arguments, globalScopeCall, callDisplayName);
                                globalScopeCall = false;
                                {
                                    std::string rcvr = structVar.TypeAndValue.VariableName;
                                    if (rcvr.empty() && !compilerLLVM->lastCallParameterNames.empty())
                                    {
                                        const auto& fp = compilerLLVM->lastCallParameterNames[0];
                                        if (fp.ends_with("__"))
                                            rcvr = "this";
                                    }
                                    CheckCallSiteLocks(ctx, rcvr, arguments);
                                }
                                namedVar.Storage = nullptr;
                                namedVar.BaseType = namedVar.Primary ? namedVar.Primary->getType() : nullptr;
                                // A further chain link (e.g. `a?.nxt().get()`) must reclassify against
                                // nxt()'s result below, not the stale pre-call receiver type.
                                if (namedVar.Primary)
                                    namedVar.TypeAndValue = Compiler(ctx)->lastCallReturnType;
                                PrepareAliasCallResult(ctx, namedVar);
                                if (namedVar.Primary != nullptr && structVar.ContainsBondedClosure
                                    && (namedVar.TypeAndValue.IsFunctionPointer
                                        || Compiler(ctx)->GetEncodedClosureType(
                                            namedVar.TypeAndValue.TypeName) != nullptr))
                                {
                                    namedVar.IsBonded = true;
                                    namedVar.BondedSources = structVar.BondedSources;
                                    Compiler(ctx)->lastCallIsBonded = true;
                                    Compiler(ctx)->lastCallBondedSources = structVar.BondedSources;
                                    Compiler(ctx)->RegisterBondedValue(
                                        namedVar.Primary, structVar.BondedSources);
                                }
                                structVar = {};
                                interfaceVar = {};
                            }
                            else
                            {
                                // Try generic extension method instantiation if this is a
                                // generic function template and the struct implements a matching interface.
                                std::string resolvedFuncName = functionName;
                                // Bare spelling inside a namespace reaches that namespace's key first.
                                std::string gfName = Compiler(primaryCtx)->ResolveGenericFunctionBase(functionName);
                                if (!Compiler(primaryCtx)->GetFunction(functionName) && genericFunctionTemplates.count(gfName))
                                {
                                    std::string structTypeName = structVar.TypeAndValue.TypeName;
                                    if (structTypeName.empty() && structVar.BaseType)
                                    {
                                        if (auto* st = llvm::dyn_cast<llvm::StructType>(structVar.BaseType))
                                            structTypeName = st->getName().str();
                                    }
                                    for (const auto& iface : Compiler(primaryCtx)->GetStructInterfaces(structTypeName))
                                    {
                                        auto inst = InferAndInstantiateGenericFunction(gfName, iface);
                                        if (!inst.empty()) { resolvedFuncName = inst; break; }
                                    }
                                    // If interface-based inference failed, try to infer from argument types.
                                    if (resolvedFuncName == functionName)
                                    {
                                        auto inst = TryInferAndInstantiateFromArgs(gfName, arguments);
                                        if (!inst.empty()) resolvedFuncName = inst;
                                    }
                                }
                                namedVar.Primary = Compiler(primaryCtx)->CreateOverloadedFunctionCall(
                                    resolvedFuncName, arguments, globalScopeCall, callDisplayName);
                                globalScopeCall = false;
                                {
                                    std::string rcvr = structVar.TypeAndValue.VariableName;
                                    if (rcvr.empty() && !compilerLLVM->lastCallParameterNames.empty())
                                    {
                                        const auto& fp = compilerLLVM->lastCallParameterNames[0];
                                        if (fp.ends_with("__"))
                                            rcvr = "this";
                                    }
                                    CheckCallSiteLocks(primaryCtx, rcvr, arguments);
                                }
                                namedVar.Storage = nullptr;
                                namedVar.BaseType = namedVar.Primary ? namedVar.Primary->getType() : nullptr;
                                // Populate TypeAndValue from the resolved overload's return type
                                // so that subsequent member access (->field) can resolve the struct.
                                if (namedVar.Primary)
                                    namedVar.TypeAndValue = Compiler(primaryCtx)->lastCallReturnType;
                                PrepareAliasCallResult(primaryCtx, namedVar);
                                if (namedVar.Primary != nullptr && structVar.ContainsBondedClosure
                                    && (namedVar.TypeAndValue.IsFunctionPointer
                                        || Compiler(primaryCtx)->GetEncodedClosureType(
                                            namedVar.TypeAndValue.TypeName) != nullptr))
                                {
                                    namedVar.IsBonded = true;
                                    namedVar.BondedSources = structVar.BondedSources;
                                    Compiler(primaryCtx)->lastCallIsBonded = true;
                                    Compiler(primaryCtx)->lastCallBondedSources = structVar.BondedSources;
                                    Compiler(primaryCtx)->RegisterBondedValue(
                                        namedVar.Primary, structVar.BondedSources);
                                }
                                // A primitive result has no aggregate classifier: clear the previous
                                // receiver, else `box.get().toString()` reuses `box` as the receiver.
                                structVar = {};
                                interfaceVar = {};
                            }

                            if (namedVar.TypeAndValue.IsInterface)
                            {
                                interfaceVar = namedVar;
                                structVar = {};
                            }
                            else if (namedVar.BaseType && namedVar.BaseType->isStructTy())
                            {
                                structVar = namedVar;
                                interfaceVar = {};
                            }
                            else if (!namedVar.TypeAndValue.TypeName.empty() && namedVar.TypeAndValue.Pointer
                                     && Compiler(primaryCtx)->GetDataStructure(namedVar.TypeAndValue.TypeName).StructType != nullptr)
                            {
                                structVar = namedVar;
                                interfaceVar = {};
                            }
                        }

                        functionArgCounter++;
                        break;
                    }

                    default: {
                        LogErrorContext(ctx, std::format("Unexpected token '{}' in postfix expression.", parseTree->getText()));
                        if (!Compiler(ctx)->IsBlockTerminated())
                            Compiler(ctx)->builder->CreateUnreachable();
                        return {};
                    }
                    }
                }
            }

            // Whole-chain '?.' merge: at least one '?.' fired (ncEnterGuard), so merge the FINAL
            // link's result against the chain's null default here, once, instead of per-link.
            if (ncChainNullBlock != nullptr)
            {
                auto* compiler = Compiler(ctx);
                auto* resumeBlock = compiler->CreateBasicBlock("nc_resume");

                // A whole ARRAY as the chain's final result (e.g. `p?.arr`) can't be loaded into
                // a register, so it is excluded below and merged by ADDRESS here instead: a PHI
                // between the real GEP (access path) and a fresh zeroed array (null path).
                bool isArrayFinal = namedVar.Primary == nullptr && namedVar.Storage != nullptr
                    && namedVar.BaseType != nullptr && llvm::isa<llvm::ArrayType>(namedVar.BaseType);

                if (isArrayFinal)
                {
                    llvm::Type* arrType = namedVar.BaseType;
                    llvm::Value* liveArrayPtr = namedVar.Storage;
                    auto* accessPred = compiler->builder->GetInsertBlock();
                    if (ncTempMark.has_value())
                        compiler->FlushOwnedTempsSince(*ncTempMark, liveArrayPtr, ncHoistBlock);
                    compiler->CreateJump(resumeBlock);

                    compiler->SwitchToBlock(ncChainNullBlock);
                    auto* zeroAlloca = compiler->CreateAlloca(arrType);
                    compiler->builder->CreateStore(llvm::Constant::getNullValue(arrType), zeroAlloca);
                    auto* nullPred = compiler->builder->GetInsertBlock();
                    compiler->CreateJump(resumeBlock);

                    compiler->SwitchToBlock(resumeBlock);
                    auto* phi = compiler->builder->CreatePHI(liveArrayPtr->getType(), 2);
                    phi->addIncoming(liveArrayPtr, accessPred);
                    phi->addIncoming(zeroAlloca, nullPred);
                    namedVar.Storage = phi;
                    namedVar.Primary = nullptr;
                    namedVar.BaseType = arrType;
                }
                else
                {
                    // A Storage-only SCALAR final link (embedded/union field) has no Primary yet -
                    // load it now so the merge below gets a real value, not a silent blank.
                    if (namedVar.Primary == nullptr && namedVar.Storage != nullptr && namedVar.BaseType != nullptr)
                    {
                        namedVar.Primary = compiler->CreateLoad(namedVar.BaseType, namedVar.Storage);
                    }

                    llvm::Type* finalType = namedVar.Primary ? namedVar.Primary->getType() : nullptr;
                    // A void call's result is a non-null CallInst of void type - CreateAlloca(void)
                    // is an LLVM DataLayout trap, so void must NOT be treated as "has a result".
                    bool hasResult = finalType != nullptr && !finalType->isVoidTy();

                    if (hasResult)
                    {
                        auto* resultAlloca = compiler->CreateAlloca(finalType);
                        compiler->CreateAssignment(namedVar.Primary, resultAlloca);
                        if (ncTempMark.has_value())
                            compiler->FlushOwnedTempsSince(*ncTempMark, namedVar.Primary, ncHoistBlock);
                        compiler->CreateJump(resumeBlock);

                        compiler->SwitchToBlock(ncChainNullBlock);
                        compiler->CreateAssignment(llvm::Constant::getNullValue(finalType), resultAlloca);
                        compiler->CreateJump(resumeBlock);

                        compiler->SwitchToBlock(resumeBlock);
                        auto* result = compiler->CreateLoad(resultAlloca);
                        compiler->PropagateNullConditionalOwnership(namedVar.Primary, result);
                        namedVar.Storage = nullptr;
                        namedVar.Primary = result;
                        namedVar.BaseType = result->getType();
                    }
                    else
                    {
                        // Genuinely no result (void, or nothing at all) - merge control flow only.
                        if (ncTempMark.has_value())
                            compiler->FlushOwnedTempsSince(*ncTempMark, nullptr, ncHoistBlock);
                        compiler->CreateJump(resumeBlock);
                        compiler->SwitchToBlock(ncChainNullBlock);
                        compiler->CreateJump(resumeBlock);
                        compiler->SwitchToBlock(resumeBlock);
                        namedVar = {};
                    }
                }
            }

            // [PFX-2-dangle] The chain ended on a member name that was never called. For a method
            // that is a missing '()'; otherwise the name resolved to nothing at all. Either way the
            // NamedVariable below is empty and every caller dereferences it - diagnose here.
            if (!danglingMemberName.empty()
                && (danglingIsMethod
                    || (namedVar.Primary == nullptr && namedVar.Storage == nullptr
                        && namedVar.BaseType == nullptr)))
            {
                if (danglingIsMethod)
                {
                    std::string owner = danglingMemberOwner.empty()
                        ? std::string("the receiver") : std::format("'{}'", danglingMemberOwner);
                    LogErrorContext(ctx, std::format(
                        "'{}' is a method of {}; did you mean to call it? Write '{}()'.",
                        danglingMemberName, owner, danglingMemberName));
                }
                else
                {
                    LogErrorContext(ctx, std::format(
                        "'{}' does not name a value here. If it is a method, call it: '{}()'.",
                        danglingMemberName, danglingMemberName));
                }
            }

            return namedVar;
        }

        LogErrorContext(ctx, "Postfix expression has no primary expression.");
        return {};
    }

CFlatParser::UnaryExpressionContext* MainListener::tryGetUnaryExpression(antlr4::RuleContext* ctx) {
        if (ctx->getRuleIndex() == CFlatParser::RuleUnaryExpression)
            return dynamic_cast<CFlatParser::UnaryExpressionContext*>(ctx);

        antlr4::RuleContext* singleRuleChild = nullptr;
        for (auto* child : ctx->children)
        {
            if (child->getTreeType() == antlr4::tree::ParseTreeType::RULE)
            {
                if (singleRuleChild != nullptr)
                    return nullptr; // multiple rule children - complex expression
                singleRuleChild = dynamic_cast<antlr4::RuleContext*>(child);
            }
        }
        return singleRuleChild ? tryGetUnaryExpression(singleRuleChild) : nullptr;
    }

CFlatParser::PostfixExpressionContext* MainListener::tryGetPostfixExpression(antlr4::RuleContext* ctx) {
        if (ctx->getRuleIndex() == CFlatParser::RulePostfixExpression)
            return dynamic_cast<CFlatParser::PostfixExpressionContext*>(ctx);

        antlr4::RuleContext* singleRuleChild = nullptr;
        for (auto* child : ctx->children)
        {
            if (child->getTreeType() == antlr4::tree::ParseTreeType::RULE)
            {
                if (singleRuleChild != nullptr)
                    return nullptr; // multiple rule children - complex expression
                singleRuleChild = dynamic_cast<antlr4::RuleContext*>(child);
            }
        }
        return singleRuleChild ? tryGetPostfixExpression(singleRuleChild) : nullptr;
    }

std::string MainListener::DeleteOperandCalleeName(CFlatParser::UnaryExpressionContext* ue) {
        if (ue == nullptr) return "";
        std::string text = ue->getText();
        size_t paren = text.find('(');
        std::string head = paren == std::string::npos ? text : text.substr(0, paren);
        size_t arrow = head.rfind("->");
        size_t dot   = head.rfind('.');
        size_t cut   = std::string::npos;
        if (arrow != std::string::npos) cut = arrow + 2;
        if (dot != std::string::npos && (cut == std::string::npos || dot + 1 > cut)) cut = dot + 1;
        std::string name = cut == std::string::npos ? head : head.substr(cut);
        for (char c : name)
            if (!(std::isalnum((unsigned char)c) || c == '_')) return "";
        return name;
    }

CFlatParser::CastExpressionContext* MainListener::tryGetCastExpression(antlr4::RuleContext* ctx) {
        if (ctx->getRuleIndex() == CFlatParser::RuleCastExpression)
        {
            auto* ce = dynamic_cast<CFlatParser::CastExpressionContext*>(ctx);
            if (ce != nullptr && ce->typeName() != nullptr && ce->castExpression() != nullptr)
                return ce;
        }

        antlr4::RuleContext* singleRuleChild = nullptr;
        for (auto* child : ctx->children)
        {
            if (child->getTreeType() == antlr4::tree::ParseTreeType::RULE)
            {
                if (singleRuleChild != nullptr)
                    return nullptr; // multiple rule children - complex expression
                singleRuleChild = dynamic_cast<antlr4::RuleContext*>(child);
            }
        }
        return singleRuleChild ? tryGetCastExpression(singleRuleChild) : nullptr;
    }

namespace {

enum class BraceKind { Escaped, PlainChar, Verbatim, Segment };

/*
 * The single definition of what a '{' at rawText[i] means (i < end, end = index of the closing
 * quote); ParseFormatString and HasInterpolation both use it so they cannot disagree.
 *   Escaped   - '{{', a literal '{'.
 *   PlainChar - an empty pair '{}' or a '{' with no matching '}': no expression, so the braces
 *               are ordinary text and the literal is NOT interpolated (closeIdx = last index).
 *   Verbatim  - matched braces whose content cannot be an expression (JSON-ish, starting with
 *               '"' or '\'). Still routed through the format path, which copies the region
 *               verbatim - the plain path would fold any '{{'/'}}' inside it.
 *   Segment   - a real interpolation; closeIdx is the matching '}'.
 */
BraceKind ClassifyBrace(const std::string& rawText, size_t i, size_t end, size_t& closeIdx)
{
    if (i + 1 < end && rawText[i + 1] == '{')
        return BraceKind::Escaped;

    int depth = 1;
    size_t j = i + 1;
    while (j < end && depth > 0)
    {
        if (rawText[j] == '{') depth++;
        else if (rawText[j] == '}') depth--;
        if (depth > 0) j++;
    }
    if (depth > 0)
    {
        closeIdx = i; // no matching '}' in this literal - the '{' is a plain character
        return BraceKind::PlainChar;
    }

    closeIdx = j;
    if (j == i + 1)
        return BraceKind::PlainChar; // empty '{}' - no expression to interpolate
    char first = rawText[i + 1];
    if (first == '\\' || first == '"')
        return BraceKind::Verbatim;
    return BraceKind::Segment;
}

} // namespace

bool MainListener::HasInterpolation(const std::string& rawText) {
        bool inEscape = false;
        size_t end = rawText.size() - 1; // index of the closing "
        for (size_t i = 1; i < end; i++) // skip opening/closing "
        {
            char c = rawText[i];
            if (inEscape) { inEscape = false; continue; }
            if (c == '\\') { inEscape = true; continue; }
            if (c != '{') continue;

            size_t closeIdx = i;
            BraceKind kind = ClassifyBrace(rawText, i, end, closeIdx);
            if (kind == BraceKind::Segment || kind == BraceKind::Verbatim)
                return true;
            // Escaped '{{' consumes one extra char; a plain-char run consumes through closeIdx.
            i = (kind == BraceKind::Escaped) ? i + 1 : closeIdx;
        }
        return false;
    }

llvm::Value* MainListener::ParseFormatString(CFlatParser::PrimaryExpressionContext* ctx, const std::string& rawText) {
        auto* compiler = Compiler(ctx);
        compiler->EnsureStrConcatRegistered();

        // Collect segment data: each entry is {i8* ptr, i32 len} stored in alloca arrays
        struct Segment { llvm::Value* ptr; llvm::Value* len; };
        std::vector<Segment> segments;

        auto* i8Ty  = compiler->builder->getInt8Ty();
        auto* i32Ty = compiler->builder->getInt32Ty();

        // Walk rawText between the outer quotes, splitting on unescaped { ... }
        // rawText format:  "...{expr}..."  (quotes included)
        size_t i = 1; // skip opening "
        size_t end = rawText.size() - 1; // stop before closing "
        std::string litAccum;

        auto flushLiteral = [&]()
        {
            if (litAccum.empty()) return;
            // Re-encode as a quoted string so ProcessRawText can decode escapes
            std::string quoted = "\"" + litAccum + "\"";
            std::string text = ProcessRawText(quoted);
            auto* gv  = compiler->CreateGlobalString("fmtlit", text);
            auto* len = compiler->builder->getInt32((int32_t)text.size());
            segments.push_back({ gv, len });
            litAccum.clear();
        };

        bool inEscape = false;
        while (i < end)
        {
            char c = rawText[i];
            if (inEscape)
            {
                litAccum += '\\';
                litAccum += c;
                inEscape = false;
                i++;
                continue;
            }
            if (c == '\\')
            {
                inEscape = true;
                i++;
                continue;
            }
            if (c == '{')
            {
                size_t closeIdx = i;
                BraceKind kind = ClassifyBrace(rawText, i, end, closeIdx);
                if (kind == BraceKind::Escaped)
                {
                    // {{ is a literal '{', not an interpolation start.
                    litAccum += '{';
                    i += 2;
                    continue;
                }
                if (kind == BraceKind::PlainChar || kind == BraceKind::Verbatim)
                {
                    // Unmatched '{', empty '{}', or non-expression content (e.g. JSON) - literal text.
                    litAccum.append(rawText, i, closeIdx - i + 1);
                    i = closeIdx + 1;
                    continue;
                }

                std::string exprText = rawText.substr(i + 1, closeIdx - i - 1);
                i = closeIdx + 1; // skip past '}'

                flushLiteral();

                // Re-parse the expression text
                antlr4::ANTLRInputStream exprInput(exprText);
                CFlatLexer exprLexer(&exprInput);
                antlr4::CommonTokenStream exprTokens(&exprLexer);
                CFlatParser exprParser(&exprTokens);
                exprParser.removeErrorListeners(); // we emit our own targeted diagnostic
                auto localizeMessage = compilerLLVM->MakeDiagnosticLocalizer();
                exprParser.setErrorHandler(std::make_shared<CFlatErrorStrategy>(localizeMessage));
                auto* exprCtx = exprParser.assignmentExpression();

                // The braces must enclose exactly one complete expression. Leftover tokens
                // or syntax errors mean this was almost certainly not meant as interpolation
                // (e.g. embedded HLSL/JSON/code with literal braces). Emit a targeted hint
                // instead of letting the partial parse fail later as a confusing
                // "undefined variable" at a synthetic location.
                if (exprParser.getNumberOfSyntaxErrors() > 0
                    || exprParser.getCurrentToken()->getType() != cflat::kTokenEOF)
                {
                    std::string shown = exprText.size() > 40 ? exprText.substr(0, 40) + "..." : exprText;
                    LogErrorContext(ctx, std::format(
                        "'{{' in this string starts an interpolation, but the text between the "
                        "braces (\"{}\") is not a single valid expression. If you meant a literal "
                        "brace, double it: write '{{{{' and '}}}}'.", shown));
                    return nullptr;
                }

                auto nv = ParseAssignmentExpressionNamed(exprCtx);

                llvm::Value* ptr = nullptr;
                llvm::Value* len = nullptr;

                bool isString = nv.TypeAndValue.TypeName == "string";

                if (isString)
                {
                    // Already a string struct - extract _ptr (field 0) and _len (field 1)
                    llvm::Value* strVal = nv.Primary ? nv.Primary : compiler->CreateLoad(nv.Storage);
                    ptr = compiler->builder->CreateExtractValue(strVal, { 0u });
                    // Mask off _len's high OWNED bit - this length feeds __strconcat's segment lens.
                    len = compiler->builder->CreateAnd(
                        compiler->builder->CreateExtractValue(strVal, { 1u }),
                        compiler->builder->getInt32(0x7FFFFFFF));
                }
                else
                {
                    // Call operator string to convert to string struct
                    LLVMBackend::NamedVariable arg = nv;
                    arg.TypeAndValue.VariableName = "";
                    auto* strVal = compiler->CreateOverloadedFunctionCall("operator string", { arg });
                    if (!strVal)
                    {
                        compiler->LogError("no operator string for expression in format string: " + exprText);
                        return nullptr;
                    }
                    ptr = compiler->builder->CreateExtractValue(strVal, { 0u });
                    len = compiler->builder->CreateAnd(
                        compiler->builder->CreateExtractValue(strVal, { 1u }),
                        compiler->builder->getInt32(0x7FFFFFFF));
                    // `operator string` heap-allocates an owned conversion temp (e.g. int -> string).
                    // __strconcat only reads its bytes, so register the temp for end-of-statement
                    // cleanup or it leaks. (String args are already-registered operator+ temps or
                    // borrowed locals, so they need no registration here - hence only this branch.)
                    compiler->RegisterOwnedStringTemp(strVal);
                }

                segments.push_back({ ptr, len });
                continue;
            }
            // }} is a literal '}', not special on its own, but symmetric with {{.
            if (c == '}' && i + 1 < end && rawText[i + 1] == '}')
            {
                litAccum += '}';
                i += 2;
                continue;
            }
            litAccum += c;
            i++;
        }
        flushLiteral();

        if (segments.empty())
        {
            // Degenerate: no content at all - return empty string struct
            auto* gv = compiler->CreateGlobalString("fmtempty", "");
            return compiler->WrapStringLiteralAsString(gv);
        }

        int count = (int)segments.size();
        auto* i32ArrTy = llvm::ArrayType::get(i32Ty, count);
        auto* ptrArrTy = llvm::ArrayType::get(cflat_llvm::PointerTo(i8Ty), count);

        auto* ptrArr = compiler->builder->CreateAlloca(ptrArrTy, nullptr, "fmtptrs");
        auto* lenArr = compiler->builder->CreateAlloca(i32ArrTy, nullptr, "fmtlens");

        for (int k = 0; k < count; k++)
        {
            auto* ptrGep = compiler->builder->CreateConstInBoundsGEP2_32(ptrArrTy, ptrArr, 0, k);
            compiler->builder->CreateStore(segments[k].ptr, ptrGep);
            auto* lenGep = compiler->builder->CreateConstInBoundsGEP2_32(i32ArrTy, lenArr, 0, k);
            compiler->builder->CreateStore(segments[k].len, lenGep);
        }

        auto* ptrBase = compiler->builder->CreateConstInBoundsGEP2_32(ptrArrTy, ptrArr, 0, 0);
        auto* lenBase = compiler->builder->CreateConstInBoundsGEP2_32(i32ArrTy, lenArr, 0, 0);

        LLVMBackend::NamedVariable nvPtrs, nvLens, nvCount;
        nvPtrs.Primary  = ptrBase;
        nvPtrs.TypeAndValue = { "i8", "ptrs", true, false };
        nvLens.Primary  = lenBase;
        nvLens.TypeAndValue = { "i32", "lens", true, false };
        nvCount.Primary = compiler->builder->getInt32(count);
        nvCount.TypeAndValue = { "i32", "count", false, false };

        return compiler->CreateOverloadedFunctionCall("__strconcat", { nvPtrs, nvLens, nvCount });
    }

std::vector<MainListener::CaptureInfo> MainListener::CollectLambdaCaptures(
        antlr4::ParserRuleContext* bodyCtx,
        const std::set<std::string>& lambdaParamNames,
        LLVMBackend* compiler) {
        std::set<std::string> seenNames;
        std::vector<CaptureInfo> captures;

        // `shadowed` grows as we descend into nested lambdas: it holds the parameter names of the
        // nested lambdas currently being walked, so a nested parameter that shadows an enclosing
        // name is not mis-captured.
        std::function<void(antlr4::tree::ParseTree*, const std::set<std::string>&)> walk =
            [&](antlr4::tree::ParseTree* node, const std::set<std::string>& shadowed)
        {
            if (!node) return;

            // A nested lambda does NOT terminate the walk: a variable it references from an
            // ENCLOSING frame must become a TRANSITIVE capture of THIS lambda. Otherwise the inner
            // env would store the enclosing function's alloca from a context (this lambda's body)
            // where it does not dominate - a verifier failure. Capturing it here re-registers it as
            // a local of this lambda (backed by this env), so the nested lambda then captures it
            // from this frame, where it dominates (standard nested-closure lowering). Descend with
            // the nested lambda's own parameters shadowed.
            if (auto* nested = dynamic_cast<CFlatParser::LambdaExpressionContext*>(node))
            {
                std::set<std::string> inner = shadowed;
                if (auto* pl = nested->lambdaParamList())
                    for (auto* p : pl->lambdaParam())
                        inner.insert(p->Identifier()->getText());
                if (auto* body = nested->lambdaBody())
                    walk(body, inner);
                return;
            }

            // primaryExpression::genericIdentifier is the only AST node representing a
            // standalone identifier (not a member name after '.' or a named-arg label).
            if (auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node))
            {
                if (auto* gi = primary->genericIdentifier())
                {
                    std::string name = gi->Identifier()->getText();
                    // 'this' inside a method body is the implicit self pointer (Storage==nullptr,
                    // Primary-backed), so the storage-keyed frame search below skips it. Capture it
                    // explicitly: store the self pointer by value into the env and re-register it as
                    // 'this' in the invoker so member access / bare method calls resolve normally.
                    if (name == "this" && !seenNames.count("this") && !lambdaParamNames.count("this"))
                    {
                        auto thisPtr = compiler->GetThisPointer();
                        if (thisPtr.Storage != nullptr)
                        {
                            seenNames.insert("this");
                            CaptureInfo ci;
                            ci.Name         = "this";
                            ci.TV           = thisPtr.TypeAndValue;  // {TypeName=struct, Pointer=true}
                            ci.TV.Pointer   = true;
                            ci.OuterStorage = thisPtr.Storage;
                            ci.ByReference  = false;
                            ci.IsThis       = true;
                            captures.push_back(ci);
                        }
                        return;
                    }
                    // NOTE: functionTable / dataStructures are deliberately NOT excluded
                    // here. A local variable or parameter shadows a same-named function,
                    // method, or struct, so the enclosing-scope frame search below is the
                    // authoritative gate: if `name` resolves to a local/arg with storage we
                    // capture it; otherwise (a real function/type reference) the search
                    // finds nothing and we capture nothing. Excluding on functionTable here
                    // dropped captures whose name collided with an in-scope method (e.g. a
                    // captured `seed` when random.cb defines Random.seed), leaving the body
                    // to reference the OUTER storage across the closure boundary.
                    if (!seenNames.count(name)
                        && !lambdaParamNames.count(name)
                        && !shadowed.count(name)
                        && !compiler->globalNamedVariable.count(name))
                    {
                        for (const auto& frame : std::ranges::reverse_view(compiler->stackNamedVariable))
                        {
                            auto localIt = frame.namedVariable.find(name);
                            auto addCapture = [&](const LLVMBackend::NamedVariable& nv)
                            {
                                if (!nv.Storage) return;
                                seenNames.insert(name);
                                CaptureInfo ci;
                                ci.Name = name;
                                ci.TV   = nv.TypeAndValue;
                                ci.OuterStorage = nv.Storage;
                                ci.SourceIsStaticLocal = nv.IsStaticLocal;
                                // Every non-pointer struct value captures BY REFERENCE, as
                                // doc/LANGUAGE.md "Reference capture" documents - a container
                                // (list/dictionary) is a struct like any other, and value-capturing
                                // it silently discarded the body's mutations. Two exceptions stay by
                                // value: 'string' is a {i8*,i32} value whose methods take it by value,
                                // and '__closure_fat_ptr' (a captured lambda) is env-cloned so a
                                // returned outer closure keeps its inner closure alive.
                                ci.ByReference = !ci.TV.Pointer
                                    && !ci.TV.IsFunctionPointer
                                    && !ci.TV.IsPrimitive()
                                    && ci.TV.TypeName != "string"
                                    && ci.TV.TypeName != "__closure_fat_ptr"
                                    && compiler->dataStructures.count(ci.TV.TypeName);
                                captures.push_back(ci);
                            };
                            if (localIt != frame.namedVariable.end())
                            {
                                addCapture(localIt->second);
                                break;
                            }
                            auto argIt = frame.functionArgument.find(name);
                            if (argIt != frame.functionArgument.end())
                            {
                                addCapture(argIt->second);
                                break;
                            }
                        }
                    }
                }
            }

            for (size_t i = 0; i < node->children.size(); i++)
                walk(node->children[i], shadowed);
        };

        if (bodyCtx) walk(bodyCtx, {});
        return captures;
    }

LLVMBackend::NamedVariable MainListener::ParseLambdaExpression(CFlatParser::LambdaExpressionContext* ctx) {
        auto* compiler = Compiler(ctx);

        // The body is a function body, not part of the argument expression the literal sits in.
        CallArgumentSuspendScope lambdaArgumentScope(inCallArgument_, ternaryCallArgumentDepth_);

        // Parse lambda parameter list
        std::vector<LLVMBackend::DeclTypeAndValue> params;
        if (auto* paramList = ctx->lambdaParamList())
        {
            for (auto* param : paramList->lambdaParam())
            {
                LLVMBackend::DeclTypeAndValue p;
                p.TypeName = compiler->ResolveTypeAlias(param->typeSpecifier()->getText());
                p.Pointer = param->pointer() != nullptr;
                // `(move T p) => ...` - the literal's half of the `Lambda<void(move T)>` spelling.
                p.IsMove = param->Move() != nullptr;
                p.IsInterface = compiler->IsInterfaceType(p.TypeName);
                if (p.IsInterface)
                    p.IsInterfacePointer = param->pointer() != nullptr;
                p.VariableName = param->Identifier()->getText();
                params.push_back(p);
            }
        }

        // A lambda literal is a function definition on a different parse path, so run the same
        // owning-value sink inference over its parameter list against its own body. Without this
        // the body consumes a by-value owning param the caller still owns, and both free it.
        {
            std::vector<LLVMBackend::TypeAndValue> sinkParams(params.begin(), params.end());
            ApplyOwningSinkInferenceToBody(compiler, ctx->lambdaBody(), sinkParams, SinkIfConstEvaluator());
            for (size_t i = 0; i < params.size(); i++)
            {
                params[i].IsOwningSink = sinkParams[i].IsOwningSink;
                params[i].IsConsumeInferredSink = sinkParams[i].IsConsumeInferredSink;
            }
        }

        // Return type from lambdaExpectedType (threaded from declaration or argument context).
        // Resolve a plain alias, so `using V = void; Lambda<V()>` is the same type as
        // `Lambda<void()>` to every TypeName check below. A target spelling a pointer
        // (`using VP = void*`) is left alone - Pointer rides a separate field here.
        LLVMBackend::TypeAndValue returnType;
        returnType.TypeName = lambdaExpectedType.FuncPtrReturnTypeName;
        if (std::string resolved = compiler->ResolveTypeAlias(returnType.TypeName);
            resolved.find('*') == std::string::npos)
            returnType.TypeName = resolved;
        returnType.Pointer = lambdaExpectedType.FuncPtrReturnPointer;
        returnType.IsMove = lambdaExpectedType.FuncPtrReturnOwned;
        returnType.IsAlias = lambdaExpectedType.FuncPtrReturnAlias;
        // An EMPTY expected return type means no context supplied one (e.g. an immediately-invoked
        // literal). "void" is a fallback, not a declaration, and the two must not be confused: a
        // declared void discards its expression body, an inferred one cannot.
        bool returnTypeInferred = returnType.TypeName.empty();
        if (returnTypeInferred)
            returnType.TypeName = "void";

        // Keep lambda counter in sync: closure name matches lambda name index.
        size_t lambdaIdx  = compiler->lambdaCounter;
        std::string lambdaName = compiler->CreateAnonFunctionName(); // post-increments lambdaCounter

        std::set<std::string> lambdaParamNames;
        for (const auto& p : params)
            lambdaParamNames.insert(p.VariableName);

        // Scan body for captures from the enclosing function scope BEFORE saving builder state.
        auto captures = CollectLambdaCaptures(ctx->lambdaBody(), lambdaParamNames, compiler);

        // A by-value capture of an owning value type is DEEP-COPIED into the env, which then OWNS
        // it (its cleanup fn frees it exactly once); the invoker's unpacked local only BORROWS it.
        // Both the deep-copy-at-store and the borrow-in-body decisions key off this one predicate.
        // Only 'string' and a captured lambda reach here now (everything else is reference-captured
        // above); the deep copy goes through the type's own copy(). A nested closure IS deep-copyable
        // via `__closure_fat_ptr.copy` (env clone), so its captured inner env has an independent
        // lifetime and does not dangle once the inner closure's scope closes; the invoker's unpacked
        // capture is marked IsAliasBorrow, which the indirect-call result path clears so the borrow
        // does not leak onto the closure's call results.
        auto isOwningCap = [&](const CaptureInfo& cap) -> bool {
            if (cap.ByReference || cap.IsThis) return false;
            if (cap.TV.Pointer || cap.TV.ConstArraySize > 0) return false;
            if (cap.TV.TypeName == "string") return true;
            return compiler->ClosureCaptureDeepCopyable(cap.TV.TypeName);
        };

        // Build closure struct alloca in the OUTER function before switching IR context.
        llvm::AllocaInst* closureAlloca = nullptr;
        llvm::StructType* closureStructTy = nullptr;
        llvm::Value* envForFatTagged = nullptr;   // tagged i8* env stored in the fat struct (Option A)
        auto* i8PtrTy = cflat_llvm::PointerTo(compiler->builder->getInt8Ty());

        if (!captures.empty())
        {
            std::string closureName = "__closure_" + std::to_string(lambdaIdx);
            std::vector<llvm::Type*> closureFields;
            for (const auto& cap : captures)
            {
                if (cap.ByReference)
                    closureFields.push_back(cflat_llvm::PointerTo(compiler->GetDataStructure(cap.TV.TypeName).StructType));
                else
                    closureFields.push_back(compiler->GetType(cap.TV));
            }
            closureStructTy = llvm::StructType::create(*compiler->context, closureFields, closureName);

            // A by-value capture of an owning value type must be DEEP-COPIED into the env so the
            // closure owns an independent buffer with its own lifetime - otherwise the capture
            // aliases the source and dangles once the source is destructed (the empty-captured-
            // string bug). Collect the owning field slots so we can (a) deep-copy them at store
            // time and (b) generate a cleanup fn the env copy/dtor use to deep-copy / destruct them.
            std::vector<std::pair<unsigned, std::string>> owningFields;
            for (size_t i = 0; i < captures.size(); i++)
                if (isOwningCap(captures[i]))
                    owningFields.emplace_back((unsigned)i, captures[i].TV.TypeName);
            llvm::Function* cleanupFn = compiler->GenerateClosureCaptureCleanup(
                "__closure_cleanup_" + std::to_string(lambdaIdx), closureStructTy, owningFields);

            // Owning heap env (lambda Option A): allocate the captures block on the heap via the
            // library primitive so the closure VALUE can outlive its defining frame (Bug 8) and
            // is freed/cloned by the value-type machinery. __closure_env_new(size, cleanup) returns
            // a TAGGED captures pointer (low bit set); we populate captures through the untagged
            // base and store the tagged pointer in the fat struct. If function.cb is not yet
            // available (a capturing lambda in an early core file imported before it), fall back to
            // the legacy stack env - untagged, so it reads as borrowed and is never freed/cloned.
            llvm::Value* envCaptureBase = nullptr;
            bool heapEnv = false;
            if (llvm::Function* envNewFn = compiler->GetFunction("__closure_env_new"))
            {
                heapEnv = true;
                uint64_t sz = compiler->module->getDataLayout().getTypeAllocSize(closureStructTy);
                auto* sizeC     = llvm::ConstantInt::get(compiler->builder->getInt64Ty(), sz);
                llvm::Value* cleanupArg = cleanupFn
                    ? compiler->builder->CreateBitCast(cleanupFn, i8PtrTy)
                    : static_cast<llvm::Value*>(llvm::ConstantPointerNull::get(i8PtrTy));
                auto* taggedEnv = compiler->builder->CreateCall(envNewFn, { sizeC, cleanupArg }, "closure_env");
                envForFatTagged = taggedEnv;
                auto* envInt  = compiler->builder->CreatePtrToInt(taggedEnv, compiler->builder->getInt64Ty());
                auto* baseInt = compiler->builder->CreateAnd(envInt, compiler->builder->getInt64(~(uint64_t)1));
                auto* baseI8  = compiler->builder->CreateIntToPtr(baseInt, i8PtrTy);
                envCaptureBase = compiler->builder->CreateBitCast(baseI8, cflat_llvm::PointerTo(closureStructTy), "closure");
            }
            else
            {
                closureAlloca   = compiler->AllocaAtEntry(closureStructTy, nullptr, closureName);
                envForFatTagged = compiler->builder->CreateBitCast(closureAlloca, i8PtrTy, "closure_i8");
                envCaptureBase  = closureAlloca;
            }

            for (size_t i = 0; i < captures.size(); i++)
            {
                auto* fieldGEP = compiler->builder->CreateStructGEP(closureStructTy, envCaptureBase, (unsigned)i);
                if (captures[i].ByReference)
                {
                    // Store pointer to outer struct (alloca address) in closure field.
                    compiler->builder->CreateStore(captures[i].OuterStorage, fieldGEP);
                }
                else if (heapEnv && isOwningCap(captures[i]))
                {
                    // Deep-copy the owning value into the env so it has independent lifetime
                    // (the cleanup fn frees this copy; the source keeps its own).
                    LLVMBackend::NamedVariable srcNV;
                    srcNV.Storage  = captures[i].OuterStorage;
                    srcNV.BaseType = compiler->GetType(captures[i].TV);
                    srcNV.TypeAndValue.TypeName = captures[i].TV.TypeName;
                    if (auto* copied = compiler->CreateOverloadedFunctionCall("copy", { srcNV }))
                        compiler->builder->CreateStore(copied, fieldGEP);
                }
                else
                {
                    // Store a copy of the value in the closure field (scalars/pointers: shallow).
                    auto* val = compiler->CreateLoad(compiler->GetType(captures[i].TV), captures[i].OuterStorage);
                    compiler->builder->CreateStore(val, fieldGEP);
                }
            }
        }

        // Save builder position - invoker emits into a separate LLVM function.
        LLVMBackend::BuilderStateGuard savedState(compiler);

        // Invoker signature: (user_params..., i8* __env) -> RetType. Env is the trailing
        // param (the closure ABI is env-last) so a non-capturing lambda's invoker is directly
        // bitcast-compatible with a bare C function pointer at a C callback site.
        LLVMBackend::DeclTypeAndValue envParam;
        envParam.TypeName = "void"; envParam.Pointer = true; envParam.VariableName = "__env";
        std::vector<LLVMBackend::TypeAndValue> allParams;
        allParams.insert(allParams.end(), params.begin(), params.end());
        allParams.push_back(envParam);

        auto* fn = compiler->CreateFunctionDefinition(lambdaName, returnType, allParams);
        compiler->InitializeBlock(&fn->front(), false);
        LLVMBackend::AliasScopeGuard functionAliasScope(compiler);
        struct LambdaDeferredCheckGuard
        {
            LLVMBackend* Compiler;
            llvm::Function* Function;
            bool Completed = false;
            ~LambdaDeferredCheckGuard()
            {
                if (Completed) return;
                Compiler->DiscardNullDerefEvents(Function);
                Compiler->DiscardPendingReturnDangleChecks(Function);
                Compiler->DiscardPendingNullIfaceDispatch(Function);
            }
        } deferredCheckGuard{ compiler, fn };
        // Fresh straight-line for this function/lambda body; restore the enclosing walk's flag on
        // exit so a nested lambda's return does not leak into the surrounding expression.
        ReturnFlagGuard functionReturnFlagGuard(&straightLineReturned_);
        straightLineReturned_ = false;

        // Unpack captured variables from env (the trailing param) into the invoker's scope.
        if (!captures.empty() && closureStructTy)
        {
            auto* envArg    = fn->getArg((unsigned)fn->arg_size() - 1);
            auto* closurePtr = compiler->builder->CreateBitCast(
                envArg, cflat_llvm::PointerTo(closureStructTy), "closure");

            for (size_t i = 0; i < captures.size(); i++)
            {
                auto* fieldGEP = compiler->builder->CreateStructGEP(
                    closureStructTy, closurePtr, (unsigned)i);
                const auto& cap = captures[i];

                if (cap.IsThis)
                {
                    // Load the captured self pointer, park it in an alloca-of-pointer (the shape
                    // GetThisPointer expects), and register it as the invoker's 'this'.
                    auto* capTy     = compiler->GetType(cap.TV);
                    auto* capVal    = compiler->builder->CreateLoad(capTy, fieldGEP, "this_val");
                    auto* capAlloca = compiler->builder->CreateAlloca(capTy, nullptr, "this");
                    compiler->builder->CreateStore(capVal, capAlloca);
                    LLVMBackend::TypeAndValue thisTv = cap.TV;
                    thisTv.VariableName = cap.TV.TypeName + "__";
                    compiler->RegisterThisPointer(thisTv, capAlloca, capTy);
                    continue;
                }

                auto& captureNV = compiler->GetOrCreateStackVariable(cap.Name);
                compiler->RecordMoveGenBind(cap.Name); // fresh capture binding in the lambda body

                if (cap.ByReference)
                {
                    // Load pointer to the outer struct (the address stored in the closure).
                    auto* structTy  = compiler->GetDataStructure(cap.TV.TypeName).StructType;
                    auto* outerPtr  = compiler->builder->CreateLoad(
                        cflat_llvm::PointerTo(structTy), fieldGEP, cap.Name + "_ref");

                    if (cap.TV.ConstArraySize > 0)
                    {
                        // A fixed array 'T[N]' captured by reference decays to an element
                        // pointer 'T*' inside the lambda (indexed with arr[i]); register
                        // it as a pointer variable.
                        LLVMBackend::TypeAndValue captureTV = cap.TV;
                        captureTV.Pointer        = true;
                        captureTV.ConstArraySize = 0;  // inside the lambda this is a T*, not a T[N]
                        captureNV.Primary      = outerPtr;
                        captureNV.TypeAndValue = captureTV;
                        captureNV.BaseType     = cflat_llvm::PointerTo(structTy);
                    }
                    else
                    {
                        // A single named struct local captured by reference. The loaded
                        // pointer IS the variable's storage (its address), so register the
                        // capture exactly like an ordinary named struct local: a non-pointer
                        // struct whose Storage is that address. This makes every use site -
                        // field access, method receiver, and (the I2 bug) operator-overload
                        // dispatch - dereference the capture to the struct value identically
                        // to a stack local. Registering it as a pointer-type variable instead
                        // left LoadNamedVariable returning the raw pointer, so an overloaded
                        // operator (e.g. vec3 * double) saw a `ptr` operand and emitted
                        // `fmul ptr, double` -> invalid IR.
                        LLVMBackend::TypeAndValue captureTV = cap.TV;
                        captureTV.Pointer        = false;
                        captureTV.ConstArraySize = 0;
                        captureNV.Storage      = outerPtr;
                        captureNV.TypeAndValue = captureTV;
                        captureNV.BaseType     = structTy;
                    }
                    // A by-reference capture BORROWS the source; the env never owns it. Without this,
                    // every CALL destructs the caller's struct through the borrowed pointer.
                    captureNV.IsAliasBorrow = true;
                    // The OUTER frame owns it, so a `return` of an owning field read off this
                    // capture must copy - see the ref-capture arm in ParseReturnStatement.
                    captureNV.IsClosureRefCapture = true;
                }
                else
                {
                    // Load copied value; store into a local alloca so the body can modify it.
                    auto* capTy  = compiler->GetType(cap.TV);
                    llvm::Value* capVal = compiler->builder->CreateLoad(capTy, fieldGEP, cap.Name + "_val");
                    // The env owns the capture; this unpacked local only BORROWS it, so its
                    // runtime OWNED bits must be clear - see the IsAliasBorrow note below, of
                    // which this is the runtime half. Without it the local reads as a second
                    // owner of the env's buffer, and every consumer that trusts the bit (a
                    // `return` handing it to the caller, a rebind freeing the old value)
                    // frees storage the env's cleanup fn frees again.
                    if (isOwningCap(cap))
                        capVal = cap.TV.TypeName == "string"
                            ? compiler->ClearStringOwnedBit(capVal)
                            : compiler->ClearStructOwnedBits(capVal, cap.TV.TypeName);
                    auto* capAlloca = compiler->builder->CreateAlloca(capTy, nullptr, cap.Name);
                    compiler->builder->CreateStore(capVal, capAlloca);
                    captureNV.Storage = capAlloca;
                    captureNV.TypeAndValue = cap.TV;
                    captureNV.BaseType     = capTy;
                    // The ENV owns an owning-value capture (its cleanup fn frees it exactly once);
                    // the body's unpacked local only BORROWS it, so suppress its scope-exit
                    // destructor - otherwise it would free the env's buffer (a double-free).
                    if (isOwningCap(cap))
                    {
                        captureNV.IsAliasBorrow = true;
                        captureNV.IsClosureValueCapture = true;
                        RecordAliasBorrowDeclBlock(compiler, captureNV);
                    }
                }
            }
        }

        // If the lambda parameter was declared lock(this), seed currentLockSet with the
        // resolved receiver guard (e.g. "d.ready") so GuardedBy checks inside the body pass.
        // The mode comes from the suffix: lock(this.optimistic) grants reads only.
        std::string consumedLockThisReceiver = lambdaLockThisReceiver;
        LockMode consumedLockThisMode = lambdaLockThisMode;
        lambdaLockThisReceiver = {};  // consumed - clear before body in case of nested lambdas
        lambdaLockThisMode = LockMode::Exclusive;
        std::unordered_map<std::string, LockMode> savedLockSet;
        if (!consumedLockThisReceiver.empty())
        {
            savedLockSet = currentLockSet;
            currentLockSet[consumedLockThisReceiver] = consumedLockThisMode;
        }

        // A `return <expr>;` in this body must know whether "void" was DECLARED or inferred.
        // RAII because LogError throws; saved/restored so a nested lambda restores ours.
        struct InferredReturnScope
        {
            MainListener* self; llvm::Function* savedFn; std::string savedName;
            InferredReturnScope(MainListener* s, llvm::Function* fn, const std::string& name)
                : self(s), savedFn(s->lambdaReturnInferredFn_),
                  savedName(s->lambdaReturnInferredName_)
            {
                self->lambdaReturnInferredFn_ = fn;
                self->lambdaReturnInferredName_ = name;
            }
            ~InferredReturnScope()
            {
                self->lambdaReturnInferredFn_ = savedFn;
                self->lambdaReturnInferredName_ = savedName;
            }
        } inferredReturnScope(this,
                              returnTypeInferred ? compiler->currentFunction : nullptr,
                              lambdaName);

        // Parse body
        // Inside the body, the destination context is THIS lambda's return type, not the
        // enclosing declaration's: an immediately-invoked literal nested in the body must read
        // `int`, not the `Lambda<int()>` the outer variable was declared with. An inferred
        // return type supplies no context, so the scope clears it.
        LLVMBackend::TypeAndValue bodyExpectedDest;
        if (!returnTypeInferred)
            bodyExpectedDest = returnType;
        DeclExpectedTypeScope lambdaBodyExpectedScope(&declExpectedType, bodyExpectedDest);

        if (auto* body = ctx->lambdaBody())
        {
            if (auto* block = body->compoundStatement())
                {
                if (auto* items = block->blockItemList())
                    ParseBlockItemList(items);
            }
            else if (auto* expr = body->assignmentExpression())
            {
                if (!compiler->IsBlockTerminated())
                {
                    auto* discardExpr = lambdaDiscardRhs_ != nullptr ? lambdaDiscardRhs_ : expr;
                    bool forcedDiscard = lambdaDiscardRhs_ != nullptr;
                    if (returnType.TypeName == "void" && !returnType.Pointer)
                    {
                        // On a VOID lambda `=> expr` is a DISCARDED full expression, not a return:
                        // mirror ParseStatement's expression-statement arm exactly. The void arm
                        // below then emits the CreateRetVoid. FlushOwnedTemps stands in for the
                        // block-item boundary flush an expression body never reaches. `void*`
                        // returns a real value and stays on the return lowering.
                        bool bareExpr = !forcedDiscard && expr->assignmentOperator() == nullptr;
                        auto resultNV = ParseAssignmentExpressionNamed(discardExpr, ResultUse::Discard);
                        // No context declared the return type, so "void" here is a fallback. A body
                        // that yields a VALUE would be silently dropped and the caller would read
                        // garbage - say so instead (a void-yielding body is a genuine discard).
                        if (returnTypeInferred && resultNV.Primary != nullptr
                            && !resultNV.Primary->getType()->isVoidTy())
                            LogErrorContext(expr, std::format(
                                "cannot infer the return type of lambda '{}': its body yields a value "
                                "but no 'function<...>' or 'Lambda<...>' type reaches it here; bind the "
                                "lambda to a typed variable or parameter and call it through that",
                                lambdaName));
                        if (bareExpr) DiagnoseDiscardedOwningReturn(expr, resultNV);
                        ProcessPlusPlus();
                        RegisterDiscardedOwningStructTemp(resultNV);
                        compiler->FlushOwnedTemps();
                    }
                    else
                    {
                        // `=> expr` is `=> { return expr; }`: go through the shared return lowering
                        // so the body runs the same ownership gates a block body does.
                        EmitReturnExpression(expr, expr, expr->getText());
                    }
                }
            }
        }

        if (!consumedLockThisReceiver.empty())
            currentLockSet = savedLockSet;

        // A dead trailing block (e.g. fall-off of a `while (true)` with no break)
        // does not fall through - do not flag it as a missing return; close it
        // with 'unreachable' for module validity.
        bool lambdaBlockUnreachable = compiler->IsCurrentBlockUnreachable();

        if (returnType.TypeName == "void" && !compiler->IsBlockTerminated())
            compiler->CreateReturnCall(nullptr);
        else if (returnType.TypeName != "void" && !compiler->IsBlockTerminated() && !lambdaBlockUnreachable)
            LogErrorContext(ctx, std::format("Lambda '{}' missing return statement.", lambdaName));

        compiler->CreateBlockBreak(nullptr, true);

        if (!compiler->IsBlockTerminated() && lambdaBlockUnreachable)
            compiler->builder->CreateUnreachable();

        // Lambda invokers do not pass through the named-function completion hook. Run the same
        // deferred checks while this function's CFG and per-function ledgers are still current.
        compiler->RunDeferredEndOfBodyChecks(fn);
        deferredCheckGuard.Completed = true;

        savedState.restore();

        // Build user-visible function-pointer TypeAndValue (no __env param).
        LLVMBackend::TypeAndValue tv;
        tv.IsFunctionPointer = true;
        // Tag the value with the closure backing type so the value-type ownership machinery
        // (move-by-default, scope-exit/struct teardown, copy-clone) recognizes it (Option A).
        // The call signature still rides FuncPtrReturnTypeName/FuncPtrParams; GetType keys off
        // IsFunctionPointer before TypeName, so the closure fat type is still emitted.
        tv.TypeName = "__closure_fat_ptr";
        tv.FuncPtrReturnTypeName = returnType.TypeName;
        tv.FuncPtrReturnPointer  = returnType.Pointer;
        tv.FuncPtrReturnOwned = returnType.IsMove;
        tv.FuncPtrReturnAlias = returnType.IsAlias;
        tv.FuncPtrReturnPointerDepth = returnType.ValuePointerDepth();
        for (const auto& p : params)
        {
            LLVMBackend::TypeAndValue::FuncPtrParam fp;
            fp.TypeName = p.TypeName;
            fp.Pointer  = p.Pointer;
            fp.PointerDepth = p.ValuePointerDepth();
            // Carry the inferred sink onto the TYPE so the indirect call site can null the
            // caller's source; a declared Lambda<...> spelling never sets these.
            fp.IsOwningSink = p.IsOwningSink;
            fp.IsConsumeInferredSink = p.IsConsumeInferredSink;
            // A SPELLED `move` param rides the type, exactly as a named function's does.
            fp.IsMove = p.IsMove;
            tv.FuncPtrParams.push_back(fp);
        }

        // Build closure fat struct {invoker_as_i8ptr, env_as_i8ptr_or_null}. The env is the
        // tagged heap-env pointer (Option A); null for a non-capturing lambda.
        auto* fnAsI8   = compiler->builder->CreateBitCast(fn, i8PtrTy, "lambda_fn_i8");
        llvm::Value* envForFat = envForFatTagged
            ? envForFatTagged
            : static_cast<llvm::Value*>(llvm::ConstantPointerNull::get(i8PtrTy));

        auto* closureFatTy = compiler->GetClosureFatPtrType();
        llvm::Value* fat = llvm::UndefValue::get(closureFatTy);
        fat = compiler->builder->CreateInsertValue(fat, fnAsI8,     {0u});
        fat = compiler->builder->CreateInsertValue(fat, envForFat,  {1u});

        lastLambdaType = tv;
        LLVMBackend::NamedVariable result;
        result.Primary     = fat;
        result.TypeAndValue = tv;

        // Register the lambda value as an owned-closure temporary (Option A): if it is not bound
        // to a named owner (e.g. passed directly by value as an argument), its heap env is freed
        // at end-of-full-expression by FlushOwnedClosureTemps. A decl-init / assignment / field
        // store that claims it calls UnregisterOwnedClosureTemp so only the owner frees it.
        compiler->RegisterOwnedClosureTemp(fat);

        // Record what this lambda captures (names are already de-duplicated by
        // CollectLambdaCaptures) so a later attempt to pass it to a C function-pointer
        // parameter can report exactly which variables block the conversion. Mirror it onto
        // the compiler-level side-channel since only the value survives into the arg list.
        for (const auto& cap : captures)
        {
            result.LambdaCaptureNames.push_back(cap.Name);
            if (cap.ByReference && !cap.SourceIsStaticLocal)
                result.LambdaReferenceCaptureNames.push_back(cap.Name);
        }
        compiler->lastCallLambdaCaptureNames = result.LambdaCaptureNames;
        compiler->lastLambdaReferenceCaptureNames = result.LambdaReferenceCaptureNames;

        // Phase 6: Bond tracking - reference-captured variables are held by pointer.
        // The lambda borrows stack addresses that cannot outlive their source scope.
        for (const auto& cap : captures)
        {
            // A copied closure extracted from a bonded holder still borrows the
            // holder's captured frame and cannot be carried into another lambda.
            for (const auto& frame : std::ranges::reverse_view(compiler->stackNamedVariable))
            {
                auto it = frame.namedVariable.find(cap.Name);
                if (it == frame.namedVariable.end()) continue;
                if (it->second.ContainsBondedClosure || it->second.IsBonded)
                    LogErrorContext(ctx, std::format(
                        "cannot capture '{}' in a lambda - it holds a bonded closure that would outlive its captured local",
                        cap.Name));
                break;
            }
            if (cap.ByReference)
            {
                result.IsBonded = true;
                result.BondedSources.push_back(cap.Name);
            }
        }
        if (result.IsBonded)
        {
            // Kind A capture-bond: the closure holds &source (the alloca address).
            // Reassigning source writes a still-live slot, so the reassignment block is spurious.
            // Set BondByAddress so FindActiveBondBorrower skips the reassignment check for this kind.
            result.BondByAddress              = true;
            compiler->lastCallIsBonded        = true;
            compiler->lastCallBondByAddress   = true;
            compiler->lastCallBondedSources   = result.BondedSources;
        }

        return result;
    }

std::string MainListener::LLVMTypeToTypeName(llvm::Type* ty, const std::string& structHint) {
        if (!ty) return "";
        if (ty->isIntegerTy(1))  return "bool";
        if (ty->isIntegerTy(8))  return "i8";
        if (ty->isIntegerTy(16)) return "i16";
        if (ty->isIntegerTy(32)) return "int";
        if (ty->isIntegerTy(64)) return "i64";
        if (ty->isFloatTy())     return "float";
        if (ty->isDoubleTy())    return "double";
        if (auto* st = llvm::dyn_cast<llvm::StructType>(ty))
            return st->hasName() ? st->getName().str() : structHint;
        if (!structHint.empty()) return structHint;
        return "";
    }

LLVMBackend::NamedVariable MainListener::ParseTupleExpression(CFlatParser::TupleExpressionContext* ctx) {
        auto* compiler = Compiler(ctx);
        auto entries = ctx->tupleConstructEntry();

        // Evaluate each element and collect its type name
        std::vector<llvm::Value*> elemValues;
        std::vector<std::string> typeArgs;
        for (auto* entry : entries)
        {
            // The tuple is the destination expression; its enclosing type is not the type of
            // any one element. Element-specific inference is not available here, so clear it.
            DeclExpectedTypeScope elementExpectedScope(&declExpectedType, {});
            auto nv = ParseAssignmentExpressionNamed(entry->assignmentExpression());
            auto* loaded = LoadNamedVariable(nv);
            elemValues.push_back(loaded);

            // Prefer TypeAndValue.TypeName (set for variables); fall back to LLVM type
            std::string typeName = nv.TypeAndValue.TypeName;
            bool typeNameInferred = typeName.empty();
            if (typeName.empty() && loaded)
                typeName = LLVMTypeToTypeName(loaded->getType(), nv.TypeAndValue.TypeName);
            // C integer promotion: untyped small-integer literals widen to int (i32)
            if (typeNameInferred && (typeName == "i8" || typeName == "i16"))
            {
                typeName = "int";
                loaded = compiler->builder->CreateSExt(loaded, llvm::Type::getInt32Ty(*compiler->context));
                elemValues.back() = loaded;
            }
            // Encode the element's reference kind in the type-arg string so the constructed
            // tuple mangles to the same name as its declared `(T[], ...)` / `tuple<T*, ...>` type:
            // "[]" for a noalias array-view, "*" for a plain pointer.
            if (nv.TypeAndValue.IsArrayView && !typeName.empty() && typeName.back() != ']')
                // A view OF pointers carries its element's star: `int*[]`, not `int[]`.
                typeName += std::string(nv.TypeAndValue.ElemPointer ? 1 : 0, '*') + "[]";
            else if (nv.TypeAndValue.Pointer && !nv.TypeAndValue.IsArrayView
                     && !typeName.empty() && typeName.back() != '*')
                typeName += "*";
            typeArgs.push_back(typeName);
        }

        std::string mangledName = MangledGenericName("tuple", typeArgs);
        tupleTypeArgs[mangledName] = typeArgs;

        // Ensure the tuple instantiation is processed before we use its struct layout
        if (!instantiatedGenerics.count(mangledName) &&
            (genericStructTemplates.count("tuple") || genericClassTemplates.count("tuple")))
        {
            QueuePendingInstantiation("tuple", typeArgs, mangledName);
            instantiatedGenerics.insert(mangledName);
            if (!compiler->GetDataStructure(mangledName).StructType)
            {
                compiler->CreateStructType(mangledName, {});
                LLVMBackend::TypeAndValue rt{ .TypeName = mangledName };
                compiler->CreateFunctionDeclaration(mangledName, rt, {});
            }
        }
        // ProcessPendingInstantiations calls ParseStructDefinition which calls
        // CreateFunctionDefinition and moves the builder into the new constructor.
        // Save and restore so we continue emitting into the caller's function body.
        {
            LLVMBackend::BuilderStateGuard savedState(compiler);
            ProcessPendingInstantiations();
        }

        // Allocate the tuple struct and store each element into item_i field
        LLVMBackend::TypeAndValue tupleType{ .TypeName = mangledName };
        auto* structType = compiler->GetDataStructure(mangledName).StructType;
        auto* alloca = compiler->CreateAlloca(structType);
        const auto& structData = compiler->GetDataStructure(mangledName);
        for (size_t i = 0; i < elemValues.size(); i++)
        {
            std::string fieldName = "item_" + std::to_string(i);
            unsigned fieldIdx = 0;
            for (const auto& f : structData.StructFields)
            {
                if (f.VariableName == fieldName) break;
                fieldIdx++;
            }
            auto* gep = compiler->CreateStructGEP(structType, alloca, fieldIdx);
            compiler->builder->CreateStore(elemValues[i], gep);
        }

        LLVMBackend::NamedVariable result;
        result.Storage = alloca;
        result.Primary = compiler->builder->CreateLoad(structType, alloca);
        result.TypeAndValue = tupleType;
        return result;
    }

llvm::Value* MainListener::EmitHeapDefaultConstruct(LLVMBackend* compiler, const std::string& typeName,
                                          antlr4::ParserRuleContext* errCtx) {
        LLVMBackend::TypeAndValue typeInfo{ .TypeName = typeName };
        llvm::Type* elemType = compiler->GetType(typeInfo);
        if (!elemType || !elemType->isSized())
        {
            LogErrorContext(errCtx, std::format("'<{}>': cannot construct unknown or unsized type",
                SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = typeName })));
            return nullptr;
        }

        uint64_t effAlign = compiler->GetEffectiveAlignmentForType(typeName, elemType);
        llvm::Value* sizeVal;
        if (effAlign > 1)
        {
            uint64_t paddedSize = compiler->GetEffectiveAllocSize(elemType, effAlign);
            sizeVal = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*compiler->context), paddedSize);
        }
        else
        {
            sizeVal = compiler->GetTypeSizeBytes(elemType);
        }

        bool useAligned = effAlign > LLVMBackend::kDefaultNewAlign;
        LLVMBackend::NamedVariable szArg;
        szArg.Primary = sizeVal;
        szArg.BaseType = sizeVal->getType();
        std::vector<LLVMBackend::NamedVariable> newArgs = { szArg };
        if (useAligned)
        {
            LLVMBackend::NamedVariable alignArg;
            alignArg.Primary = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*compiler->context), effAlign);
            alignArg.BaseType = alignArg.Primary->getType();
            newArgs.push_back(alignArg);
        }

        llvm::Value* rawPtr = nullptr;
        std::string opNewName = typeName + ".operator new";
        if (compiler->GetFunction(opNewName))
            rawPtr = compiler->CreateOverloadedFunctionCall(opNewName, newArgs);
        else if (compiler->GetFunction("operator new"))
            rawPtr = compiler->CreateOverloadedFunctionCall("operator new", newArgs);
        else
        {
            LogErrorContext(errCtx, "'<>' element sugar requires 'operator new' to be defined");
            return nullptr;
        }

        llvm::Value* typedPtr = compiler->builder->CreateBitCast(rawPtr, cflat_llvm::PointerTo(elemType), "elemnew");
        if (compiler->GetFunction(typeName))
        {
            llvm::Value* structVal = compiler->CreateOverloadedFunctionCall(typeName, {});
            if (structVal)
                compiler->builder->CreateStore(structVal, typedPtr);
        }
        return typedPtr;
    }

llvm::Value* MainListener::ParseElementExpression(CFlatParser::ElementExpressionContext* ctx) {
        auto* compiler = Compiler(ctx);
        DeclExpectedTypeScope elementExpectedScope(&declExpectedType, {});

        auto tags = ctx->Identifier();
        std::string tagName = tags[0]->getText();
        if (tags.size() > 1 && tags[1]->getText() != tagName)
        {
            LogErrorContext(ctx, std::format(
                "mismatched closing tag </{}> for <{}>", tags[1]->getText(), tagName));
            return nullptr;
        }

        auto it = compiler->dataStructures.find(tagName);
        if (it == compiler->dataStructures.end())
        {
            LogErrorContext(ctx, std::format(
                "<{}> is not a known type in scope; the element tag must name a struct/class type", tagName));
            return nullptr;
        }

        llvm::Value* structPtr = EmitHeapDefaultConstruct(compiler, tagName, ctx);
        if (!structPtr)
            return nullptr;
        const auto& sd = it->second;

        // Attributes -> ownership-correct field stores.
        for (auto* attr : ctx->elementAttribute())
        {
            std::string fieldName = attr->Identifier()->getText();
            LLVMBackend::NamedVariable rightNV = {};
            // Occurrence-scope this ONE attribute's evaluation - same multi-field-in-one-statement
            // collision as brace-init (see codeValueDataCasts_).
            size_t savedCastOcc = compiler->BeginCastOccurrence();
            size_t thisCastOcc = compiler->CurrentCastOccurrence();
            if (auto* sl = attr->StringLiteral())
            {
                // attr="literal" is a static string; use attr={expr} for interpolation
                // or any dynamic value.
                std::string processed = ProcessRawText(sl->getText(), /*foldBraces=*/true);
                llvm::Value* litVal = compiler->CreateGlobalString("", processed);
                rightNV.Primary = litVal;
                rightNV.BaseType = litVal ? litVal->getType() : nullptr;
            }
            else
            {
                // Thread the target field's function-pointer type into a lambda RHS, so
                // `label={(int n) => { return s; }}` infers its return type the way the
                // equivalent `t.label = (int n) => {...}` field assignment already does.
                for (const auto& field : sd.StructFields)
                {
                    if (field.VariableName == fieldName && field.IsFunctionPointer)
                    {
                        lambdaExpectedType = field;
                        break;
                    }
                }
                rightNV = ParseAssignmentExpressionNamed(attr->assignmentExpression());
                lambdaExpectedType = {};
            }
            rightNV.CastOccurrenceId = thisCastOcc;
            compiler->EndCastOccurrence(savedCastOcc);
            EmitOneFieldInit(structPtr, sd, tagName, fieldName, rightNV, attr);
        }

        // Children -> add() calls. The receiver self NV is rebuilt per call because
        // CreateOverloadedFunctionCall may consume/null move args.
        auto makeSelf = [&]() {
            LLVMBackend::NamedVariable selfNV;
            selfNV.Primary = structPtr;
            selfNV.BaseType = structPtr->getType();
            selfNV.TypeAndValue.TypeName = tagName;
            selfNV.TypeAndValue.Pointer = true;
            return selfNV;
        };
        for (auto* content : ctx->elementContent())
        {
            LLVMBackend::NamedVariable childNV = {};
            if (auto* childEl = content->elementExpression())
            {
                llvm::Value* childPtr = ParseElementExpression(childEl);
                if (!childPtr)
                    continue;
                childNV.Primary = childPtr;
                childNV.BaseType = childPtr->getType();
                childNV.TypeAndValue.TypeName = childEl->Identifier()[0]->getText();
                childNV.TypeAndValue.Pointer = true;
                childNV.IsAdoptable = true;
            }
            else
            {
                childNV = ParseAssignmentExpressionNamed(content->assignmentExpression());
            }
            childNV.TypeAndValue.VariableName = "";   // forwarded positionally into add()
            compiler->CreateOverloadedFunctionCall("add", { makeSelf(), childNV });
        }

        // Publish the result type (Tag*) on the primary->postfix side-channel so the
        // caller boxes it into an interface where needed (e.g. `return <View/>` or
        // assigning to an `Element` slot) and member access on the result resolves.
        lastParenExprType = {};
        lastParenExprType.TypeName = tagName;
        lastParenExprType.Pointer = true;
        lastParenExprStorage = nullptr;
        lastParenExprFromOwningTempField = false;
        lastParenExprOwningTempParent = false;
        lastParenExprOwningStructName.clear();
        lastParenExprFieldName.clear();
        lastParenExprCallerName.clear();
        lastParenExprNamed = {};
        lastParenExprNamed.IsAdoptable = true;

        // Launder ownership: the result is an unowned-by-tracker heap pointer the
        // caller manages (add to a parent or deleteTree), exactly like a factory such
        // as view(). Clearing these keeps `return <View/>` from tripping the
        // "owning value boxed into interface return" check.
        compiler->lastOwningResult = false;
        compiler->lastAllocAlignment = 0;
        compiler->lastCallReturnsOwned = false;

        return structPtr;
    }

llvm::Value* MainListener::ParsePrimaryExpression(CFlatParser::PrimaryExpressionContext* ctx, ResultUse use) {
        auto* compiler = Compiler(ctx);

        if (auto* tupleCtx = ctx->tupleExpression())
            return ParseTupleExpression(tupleCtx).Primary;

        if (auto* lambdaCtx = ctx->lambdaExpression())
            return ParseLambdaExpression(lambdaCtx).Primary;

        if (auto* elemCtx = ctx->elementExpression())
            return ParseElementExpression(elemCtx);

        auto expressionCtx = ctx->expression();
        auto constant = ctx->Constant();
        auto stringLiteral = ctx->StringLiteral();

        // `default` as an expression: the value is the destination type's default. Without a
        // known destination there is nothing to default TO, so say that rather than emitting
        // a parser mismatch on the token that follows.
        if (ctx->Default() != nullptr)
        {
            if (declExpectedType.TypeName.empty() || declExpectedType.TypeName == "auto")
            {
                LogErrorContext(ctx,
                    "'default' needs a known target type here - it takes its value from the "
                    "destination, and this position supplies none. Write the type explicitly "
                    "(e.g. 'T x = default;' and use 'x'), or cast the destination.");
                return nullptr;
            }
            LLVMBackend::DeclTypeAndValue dtv;
            static_cast<LLVMBackend::TypeAndValue&>(dtv) = declExpectedType;
            return GenerateDefaultValue(dtv);
        }

        if (ctx->TypeOf())
        {
            // typeof(int), typeof(bool), typeof(MyStruct) - type specifier used directly
            if (auto* ts = ctx->typeSpecifier())
            {
                LLVMBackend::TypeAndValue type;
                type.TypeName = ParseTypeSpecifierName(ts);
                return compiler->CreateGlobalString("typeof", SpellType(*compiler, type));
            }

            // typeof(expr) - navigate down to unaryExpression to read TypeAndValue
            LLVMBackend::TypeAndValue type;

            // ANTLR picks the expression alternative for user-defined type names (Identifier
            // matches both expression and typeSpecifier); catch them here before evaluating.
            if (compiler->GetDataStructure(expressionCtx->getText()).StructType != nullptr)
                type.TypeName = expressionCtx->getText();

            if (type.TypeName.empty())
            {
                if (auto* ue = tryGetUnaryExpression(expressionCtx))
                {
                    auto namedVar = ParseUnaryExpression(ue);
                    type = namedVar.TypeAndValue;
                // 'auto' variables carry the literal text "auto" (or an empty
                // string) as their declared TypeName even though the concrete
                // type is known. Recover it from the resolved LLVM BaseType,
                // which holds the value type even for pointer variables.
                // Note: LLVM integer types are signless, so an 'auto' variable
                // bound to an unsigned value reports the signed name (e.g. "int"
                // for u32, "i8" for char) - the signedness is not recoverable here.
                    if ((type.TypeName == "auto" || type.TypeName.empty()) && namedVar.BaseType != nullptr)
                        type.TypeName = LLVMTypeToTypeName(namedVar.BaseType);
                }
            }

            std::string spelling = type.TypeName.empty() || type.TypeName == "auto"
                ? "unknown" : SpellType(*compiler, type);
            return compiler->CreateGlobalString("typeof", spelling);
        }
        else if (ctx->NameOf())
        {
            std::string fullText = expressionCtx->getText();
            // Return just the last identifier after any '.' or '->'
            size_t dotPos = fullText.rfind('.');
            size_t arrowPos = fullText.rfind("->");
            size_t lastSep = 0;
            if (dotPos != std::string::npos) lastSep = std::max(lastSep, dotPos + 1);
            if (arrowPos != std::string::npos) lastSep = std::max(lastSep, arrowPos + 2);
            std::string name = fullText.substr(lastSep);
            return compiler->CreateGlobalString("nameof", name);
        }
        else if (ctx->IidOf())
        {
            // iidof(IReference<int>) / iidof(ISomeInterface) -> a REFIID-shaped pointer to a
            // static 16-byte GUID. For a parameterized type the IID is the derived PIID (the type
            // is instantiated on demand here); for a plain interface it is its stored IID.
            auto* ts = ctx->typeSpecifier();
            std::string base;
            std::vector<std::string> typeArgs;
            if (auto* gp = GenericSpecOf(ts, base))
            {
                base = compiler->ResolveGenericBaseAlias(base);
                for (auto* entry : gp->typeParameterList()->typeParameterEntry())
                    typeArgs.push_back(ResolveTypeArgEntry(entry));
            }
            else
            {
                base = ts ? ts->getText() : "";
            }
            // A bare type-parameter (e.g. iidof(T) inside ComPtr<T>) must resolve to the
            // specialization's concrete type argument, the same way T*/T type positions do.
            // Route base through any `using` alias and the active generic substitution map so
            // T -> the instantiated mangled interface (e.g. "IReference$int") reaches the PIID table.
            if (typeArgs.empty())
            {
                base = compiler->ResolveTypeAlias(base);
                auto substIt = activeTypeSubstitutions.find(base);
                if (substIt != activeTypeSubstitutions.end())
                {
                    // The substituted argument is spelled as the caller wrote it - which for a
                    // WinMD type is usually an alias (ComPtr<IPropertyValueStatics>), so expand
                    // it too before looking up the IID.
                    base = compiler->ResolveTypeAlias(substIt->second);
                }
            }
            std::string mangled = typeArgs.empty() ? base : MangledGenericName(base, typeArgs);
            if (!typeArgs.empty())
                compiler->InstantiateWinrtGenericInterface(base, typeArgs, mangled);
            if (auto* g = compiler->EmitIidGlobalFor(mangled))
                return g;
            compiler->LogError("iidof: no IID known for type '" +
                               SpellType(*compiler, LLVMBackend::TypeAndValue{
                                   .TypeName = typeArgs.empty() ? base : mangled }) +
                               "' (only [winrt]/imported interfaces and parameterized WinRT interfaces have one)");
            return llvm::Constant::getNullValue(compiler->builder->getPtrTy());
        }
        else if (ctx->WinrtDelegate())
        {
            // winrtDelegate(DelegateType, closure) -> a COM-callable delegate object (i8*) whose
            // Invoke forwards the WinRT ABI args to the closure. The delegate type resolves the
            // IID/PIID + Invoke signature from imported metadata; the closure is cloned in and
            // owned by the object (released on final COM Release). See EmitWinrtDelegateObject.
            auto* ts = ctx->typeSpecifier();
            std::string base;
            std::vector<std::string> typeArgs;
            if (auto* gp = GenericSpecOf(ts, base))
            {
                base = compiler->ResolveGenericBaseAlias(base);
                for (auto* entry : gp->typeParameterList()->typeParameterEntry())
                    typeArgs.push_back(ResolveTypeArgEntry(entry));
            }
            else
            {
                base = ts ? ts->getText() : "";
                base = compiler->ResolveTypeAlias(base);   // `using RoutedEventHandler = Microsoft...;`
            }

            // Evaluate the closure argument and normalize it to a fat closure {code, env} value.
            auto nv = ParseAssignmentExpressionNamed(ctx->assignmentExpression());
            llvm::Value* cloVal = LoadNamedVariable(nv);
            if (auto* fn = llvm::dyn_cast<llvm::Function>(cloVal))
                cloVal = compiler->WrapBareValueAsFatStruct(fn);
            else if (cloVal && cloVal->getType() == compiler->GetClosureFatPtrType())
            { /* already a fat closure value */ }
            else if (cloVal && cloVal->getType()->isPointerTy())
                cloVal = compiler->WidenThinToFat(cloVal);   // thin function<T>
            else
            {
                compiler->LogError("winrtDelegate: second argument must be a closure "
                                   "(lambda, function<...>, or Lambda<...>)");
                return llvm::Constant::getNullValue(compiler->builder->getPtrTy());
            }

            if (auto* v = compiler->EmitWinrtDelegateObject(base, typeArgs, cloVal))
                return v;
            return llvm::Constant::getNullValue(compiler->builder->getPtrTy());
        }
        else if (expressionCtx != nullptr)
        {
            // Use ParseAssignmentExpressionNamed to preserve TypeAndValue (e.g. cast type)
            // for ((Struct*)ptr)->field member-access chains that follow this primary.
            auto nv = ParseAssignmentExpressionNamed(expressionCtx->assignmentExpression(), use);
            ProcessPlusPlus();
            lastParenExprType = nv.TypeAndValue;
            lastParenExprStorage = nv.Storage;
            lastParenExprFromOwningTempField = nv.FromOwningTempField;
            lastParenExprOwningTempParent = nv.OwningTempParent;
            lastParenExprOwningStructName = nv.OwningStructName;
            lastParenExprFieldName = nv.FieldName;
            lastParenExprCallerName = nv.CallerName;
            lastParenExprNamed = nv;
            auto* loaded = LoadNamedVariable(nv);
            if (nv.FromOwningTempField && !nv.OwningTempParent)
                compiler->RegisterTempFieldValue(loaded);
            return loaded;
        }
        else if (stringLiteral.size() > 0)
        {
            // Adjacent string literals are NOT concatenated (unlike C). Silently keeping
            // only the first would lose data, so require the explicit '+' operator.
            if (stringLiteral.size() > 1)
            {
                LogErrorContext(ctx,
                    "adjacent string literals are not concatenated. Join them with the '+' "
                    "operator, e.g. \"a\" + \"b\".");
                return nullptr;
            }
            // TODO handle encoding u8,u,U,L
            std::string rawText = ctx->getText();
            if (HasInterpolation(rawText))
                return ParseFormatString(ctx, rawText);
            std::string processed = ProcessRawText(rawText, /*foldBraces=*/true);
            return compiler->CreateGlobalString("", processed);
        }
        else if (constant)
        {
            std::string constantText = constant->getText();
            if (constantText == "true")
            {
                return compiler->CreateConstant("bool", constantText);
            }
            else if (constantText == "false")
            {
                return compiler->CreateConstant("bool", constantText);
            }
            else if (constantText == "nullptr")
            {
                return compiler->CreateConstant("nullptr", constantText);
            }
            else if (constantText.front() == '\'' ||
                (constantText.size() > 1 &&
                    (constantText[0] == 'L' || constantText[0] == 'u' || constantText[0] == 'U') &&
                    constantText[1] == '\''))
            {
                char c = ParseCharLiteral(constantText);
                return compiler->CreateConstant(LLVMBackend::ConstantVariant(c));
            }
            else
            {
                std::string constantRaw = constant->getText();
                auto number = ParseNumberConstant(constantRaw);
                auto value = compiler->CreateConstant(number);
                return value;
            }
        }

        return nullptr;
    }

LLVMBackend::NamedVariable MainListener::ParseIdentifier(antlr4::tree::TerminalNode* node) {
        auto* compiler = Compiler();
        if (!node)
            return {};

        std::string name = node->getText();
        LLVMBackend::NamedVariable namedVar = {};

        // Check compile-time macros (constant throughout compilation)
        auto macro = compiler->GetCompileTimeMacro(name);
        if (macro.value != nullptr)
        {
            namedVar.Primary = macro.value;
            namedVar.BaseType = macro.value->getType();
            return namedVar;
        }

        // Special case: __FUNCTION__ is context-dependent (changes per function)
        if (name == "__FUNCTION__")
        {
            auto str = compiler->CreateGlobalString("__FUNCTION__", compiler->GetCurrentFunctionName());
            namedVar.Primary = str;
            namedVar.BaseType = str->getType();
            return namedVar;
        }

        // Special case: __LINE__ is location-dependent (changes per location)
        if (name == "__LINE__")
        {
            int line = (int)node->getSymbol()->getLine();
            auto val = compiler->CreateConstant("int", std::to_string(line));
            namedVar.Primary = val;
            namedVar.BaseType = val->getType();
            return namedVar;
        }

        // 'this' inside a member-function body resolves to the implicit self pointer.
        if (name == "this")
        {
            auto thisVar = compiler->GetThisPointer();
            if (thisVar.Storage != nullptr || thisVar.Primary != nullptr)
            {
                thisVar.IdentifierLine = (int)node->getSymbol()->getLine();
                thisVar.IdentifierColumn = (int)node->getSymbol()->getCharPositionInLine();
                return thisVar;
            }
        }

        // Locals and parameters resolved together with correct lexical precedence (innermost
        // frame first; a local shadows a parameter within a frame). A separate local-then-argument
        // lookup would let an ENCLOSING function's local beat a lambda's own parameter of the same
        // name - see GetScopedLocalOrArgument.
        namedVar = compiler->GetScopedLocalOrArgument(name);
        if (namedVar.Storage != nullptr || namedVar.Primary != nullptr)
        {
            namedVar.IdentifierLine = (int)node->getSymbol()->getLine();
            namedVar.IdentifierColumn = (int)node->getSymbol()->getCharPositionInLine();
            return namedVar;
        }

        auto memberVar = compiler->GetMemberVariable(name);
        if (memberVar.Storage != nullptr)
        {
            // Lock-set check: self-access inside a struct method.
            if (!memberVar.TypeAndValue.GuardedBy.empty())
            {
                if (currentLockSet.find(memberVar.TypeAndValue.GuardedBy) == currentLockSet.end())
                {
                    LogErrorContext(node, std::format(
                        "Field '{}' is guarded by '{}': must hold '{}' before accessing it.",
                        name, memberVar.TypeAndValue.GuardedBy, memberVar.TypeAndValue.GuardedBy));
                }
            }
            // Cross-thread sharing scan (--xthread-scan N): self-field access inside a method
            // of a type seen escaping a thread spawn (e.g. a program's run-thread). The owning
            // struct is derived from the enclosing function name (GetMemberVariable does not
            // tag OwningStructName, by design - that would trip the delete-encapsulation check).
            // Prints an [xthread] line to stdout; information gathering only, never an error.
            if (compiler->GetXthreadScanLevel() > 0)
            {
                std::string owner = SplitEnclosingStruct(compiler->GetCurrentFunctionName(), compiler);
                compiler->ReportXthreadFieldAccess("this", name, owner, memberVar.TypeAndValue);
            }
            return memberVar;
        }

        // try getting global variable
        {
            auto globalNV = compiler->GetGlobalVariableNV(name);
            if (globalNV.Storage != nullptr)
            {
                CheckGlobalGuard(node, name, globalNV);
                globalNV.IdentifierLine = (int)node->getSymbol()->getLine();
                globalNV.IdentifierColumn = (int)node->getSymbol()->getCharPositionInLine();
                return globalNV;
            }
        }

        if (compiler->GetFunction(name))
        {
            // Use GetFunctionForFuncPtr so that a plain name resolves to the top-level
            // function rather than a struct method registered under the same key.
            namedVar.Primary = compiler->GetFunctionForFuncPtr(name);
            namedVar.CallerName = name;
            return namedVar;
        }

        // Return-block functions have no IR entry; they are inlined at the call site.
        if (compiler->GetReturnBlock(name) != nullptr)
            return {};

        // Generic function templates have no IR entry until instantiated.
        // The call-site dispatch (PostfixExpression's RuleArgumentExpressionList branch)
        // calls TryInferAndInstantiateFromArgs once the arguments are known. The bare spelling is
        // resolved to its namespace's key here: the ResolveQualifiedName fallback below has no
        // generic-function-only key in its accept set, so it can never rescue this.
        if (genericFunctionTemplates.count(compiler->ResolveGenericFunctionBase(name)))
            return {};

        // Compiler intrinsics handled at the call site - not in the function table.
        static const std::unordered_set<std::string> kIntrinsics = {
            "va_start", "va_end", "is_pointer", "is_unique", "is_interface", "is_copyable", "is_primitive", "is_string", "annotationof",
            "compile_error", "embed",
            "reflect", "reflect_set", "json_const", "xml_const", "__rdtscp", "__readcyclecounter", "__lfence", "__pause",
            "__popcount", "__ctz", "__clz", "__prefetch", "__fma", "__likely", "__unlikely",
            "__atomic_acquire_fence",
        };
        if (kIntrinsics.count(name))
            return {};

        // Enclosing-namespace sibling: a bare name inside "namespace N" may refer to a
        // sibling member "N.<name>" (walking outward through parent namespaces).
        // ResolveQualifiedName performs that lookup using the current namespace context.
        std::string nsQualified = compiler->ResolveQualifiedName(name);
        if (nsQualified != name)
        {
            // Sibling namespace global (registered qualified, e.g. "Cfg.W").
            auto nsGlobalNV = compiler->GetGlobalVariableNV(nsQualified);
            if (nsGlobalNV.Storage != nullptr)
            {
                CheckGlobalGuard(node, name, nsGlobalNV);
                nsGlobalNV.IdentifierLine = (int)node->getSymbol()->getLine();
                nsGlobalNV.IdentifierColumn = (int)node->getSymbol()->getCharPositionInLine();
                return nsGlobalNV;
            }
            if (compiler->GetFunction(nsQualified))
            {
                namedVar.Primary = compiler->GetFunctionForFuncPtr(nsQualified);
                namedVar.CallerName = nsQualified;
                return namedVar;
            }
            // Return-block and generic-template siblings have no IR entry yet; defer to
            // the call-site dispatch (which re-qualifies through ResolveQualifiedName).
            if (compiler->GetReturnBlock(nsQualified) != nullptr)
                return {};
            if (genericFunctionTemplates.count(nsQualified))
                return {};
        }

        LogErrorContext(node, std::format("Undefined variable {}.", name));
        return {};
    }

char MainListener::ProcessEscapeChar(std::string::const_iterator& itr, const std::string::const_iterator& end) {
        if (itr == end)
            return '\\';

        char esc = *itr++;
        if (esc == 'x' || esc == 'X')
        {
            std::string hex;
            while (itr != end && *itr != '\'' && *itr != '"')
                hex += *itr++;
            return static_cast<char>(std::stoi(hex, nullptr, 16));
        }
        // Octal escape: 1-3 octal digits (\0, \033, \177, etc.).
        if (esc >= '0' && esc <= '7')
        {
            int val = esc - '0';
            for (int i = 0; i < 2 && itr != end && *itr >= '0' && *itr <= '7'; i++)
            {
                val = val * 8 + (*itr - '0');
                ++itr;
            }
            return static_cast<char>(val);
        }
        switch (esc)
        {
        case 'n':  return '\n';
        case 't':  return '\t';
        case 'r':  return '\r';
        case '\\': return '\\';
        case '\'': return '\'';
        case '"':  return '"';
        case 'a':  return '\a';
        case 'b':  return '\b';
        case 'f':  return '\f';
        case 'v':  return '\v';
        case '{':  return '{';
        case '}':  return '}';
        default:   return esc;
        }
    }

char MainListener::ParseCharLiteral(const std::string& text) {
        auto itr = text.cbegin();

        // Skip encoding prefix (L, u, U)
        if (*itr == 'L' || *itr == 'u' || *itr == 'U')
            ++itr;

        ++itr; // skip opening '

        if (*itr == '\\')
        {
            ++itr;
            return ProcessEscapeChar(itr, text.cend());
        }

        return *itr;
    }

std::string MainListener::ProcessRawText(const std::string& rawText, bool foldBraces) {
        std::string output;
        auto itr = rawText.cbegin();

        // Skip encoding prefix (u8, u, U, L)
        if (*itr == 'u' && *(itr + 1) == '8')
            itr += 2;
        else if (*itr == 'u' || *itr == 'U' || *itr == 'L')
            ++itr;

        ++itr; // skip opening "

        while (itr != rawText.cend() && *itr != '"')
        {
            if (*itr == '\\')
            {
                ++itr;
                output += ProcessEscapeChar(itr, rawText.cend());
            }
            else if (foldBraces && (*itr == '{' || *itr == '}') && (itr + 1) != rawText.cend() && *(itr + 1) == *itr)
            {
                // {{ -> { and }} -> } (only for source-level non-interpolated strings)
                output += *itr;
                itr += 2;
            }
            else
            {
                output += *itr++;
            }
        }

        return output;
    }

llvm::Value* MainListener::ParseExpression(CFlatParser::ExpressionContext* ctx) {
        auto assignCtxs = ctx->assignmentExpression();
        auto left = this->ParseAssignmentExpression(assignCtxs);
        ProcessPlusPlus();
        return left;

        /*
        // TODO: handle comma operator.
        if (assignCtxs.size() > 0)
        {
            llvm::Value* left = nullptr;
            for (const auto& assignCtx : assignCtxs)
            {
                left = this->ParseAssignmentExpression(assignCtx);
                ProcessPlusPlus();
            }

            return left;
        }
        */

        LogErrorContext(ctx, "Expression has no assignment sub-expressions.");
        return nullptr;
    }

void MainListener::RegisterDiscardedOwningStructTemp(const LLVMBackend::NamedVariable& nv) {
        const std::string& typeName = nv.TypeAndValue.TypeName;
        if (nv.Primary == nullptr || nv.Storage != nullptr || nv.BaseType == nullptr) return;
        if (typeName.empty()) return;
        auto* compiler = Compiler();
        if (!compiler->IsProducedTempValue(nv.Primary)) return;
        if (typeName == "string")
        {
            compiler->RegisterOwnedStringTemp(nv.Primary);
            return;
        }
        if (typeName == "__closure_fat_ptr")
        {
            if (!compiler->IsOwnedClosureTemp(nv.Primary))
                compiler->RegisterOwnedClosureTemp(nv.Primary);
            return;
        }
        // A POINTER rvalue is not a struct value: spilling it would destruct the pointer's own
        // bits as if they were the object. Owning pointers are handled by the owned-ptr temp list.
        if (nv.TypeAndValue.Pointer || nv.TypeAndValue.IsAlias || nv.FromOwningTempField) return;

        if (!compiler->IsOwningValueType(typeName)) return;

        auto* tempAlloca = compiler->AllocaAtEntry(nv.BaseType, nullptr, "discardtemp");
        compiler->builder->CreateStore(nv.Primary, tempAlloca);
        compiler->RegisterOwnedStructTemp(tempAlloca, typeName);
    }

void MainListener::DiagnoseDiscardedOwningReturn(antlr4::ParserRuleContext* ctx, const LLVMBackend::NamedVariable& nv) {
        auto* compiler = Compiler(ctx);
        std::string fnName;
        if (const std::string* fn = compiler->FindOwnedReturnTemp(nv.Primary))
            fnName = *fn;                             // string / pointer / interface owning return
        else if (IsDiscardedOwningStructResult(nv))
            fnName = nv.CallerName.empty() ? "<call>" : nv.CallerName;  // owning-value struct return
        else
            return;
        LogErrorContext(ctx, std::format(
            "owning return value of '{}' must not be discarded; bind it, move it, delete it, "
            "pass it on, or discard it explicitly with '_ ='", fnName));
    }

bool MainListener::IsDiscardedOwningStructResult(const LLVMBackend::NamedVariable& nv) {
        const std::string& t = nv.TypeAndValue.TypeName;
        if (nv.Primary == nullptr || nv.Storage != nullptr || nv.BaseType == nullptr) return false;
        if (nv.TypeAndValue.Pointer || nv.TypeAndValue.IsAlias || nv.FromOwningTempField) return false;
        if (t.empty() || t == "string" || t == "__closure_fat_ptr") return false;
        return Compiler()->IsOwningValueType(t);
    }

bool MainListener::MethodConsumesReceiver(const std::string& functionName, const std::string& recvType) {
        auto* compiler = Compiler();
        auto it = compiler->functionTable.find(functionName);
        if (it == compiler->functionTable.end()) return false;
        for (const auto& cand : it->second)
        {
            const auto& params = cand.Parameters;
            if (!params.empty() && params.front().TypeName == recvType
                && !params.front().Pointer && params.front().IsMove)
                return true;
        }
        return false;
    }

void MainListener::RegisterOwningTempReceiver(antlr4::ParserRuleContext* ctx,
                                    const LLVMBackend::NamedVariable& receiver,
                                    LLVMBackend::NamedVariable& thisArg,
                                    const std::string& functionName) {
        const std::string& typeName = receiver.TypeAndValue.TypeName;
        bool parenthesizedSpill = receiver.IsParenthesizedProducedTemp;
        if (receiver.Primary == nullptr && (!parenthesizedSpill || receiver.Storage == nullptr)) return;
        if (receiver.Storage != nullptr && !parenthesizedSpill) return;
        if (receiver.BaseType == nullptr || !receiver.BaseType->isStructTy()) return;
        if (receiver.TypeAndValue.Pointer) return;
        if (typeName.empty() || typeName == "__closure_fat_ptr") return;
        if (receiver.TypeAndValue.IsAlias || receiver.FromOwningTempField) return;

        auto* compiler = Compiler(ctx);
        if (typeName == "string")
        {
            compiler->RegisterOwnedStringTemp(receiver.Primary);
            return;
        }
        if (!compiler->IsOwningValueType(typeName)) return;
        if (MethodConsumesReceiver(functionName, typeName)) return;

        if (parenthesizedSpill && receiver.Storage != nullptr)
        {
            if (!receiver.TernaryTempAlreadyRegistered)
                compiler->RegisterOwnedStructTemp(receiver.Storage, typeName);
            thisArg.Storage = receiver.Storage;
            return;
        }

        auto* tempAlloca = compiler->AllocaAtEntry(receiver.BaseType, nullptr, "recvtemp");
        compiler->builder->CreateStore(receiver.Primary, tempAlloca);
        compiler->RegisterOwnedStructTemp(tempAlloca, typeName);
        thisArg.Storage = tempAlloca;   // the call and the destructor address the same temp
    }

void MainListener::ProcessPlusPlus() {
        if (PlusPlus.size() > 0)
        {
            for (auto& [destination, w] : PlusPlus)
                Compiler()->CreateIncrement(destination, w.Amount, w.ElemType, w.LoadType);

            PlusPlus.clear();
        }
    }

void MainListener::ScanAndQueueGenericTypeUses(antlr4::RuleContext* ctx, bool topLevel) {
        if (!ctx) return;
        // Scratch frame: a body-scope `using` this pre-scan meets binds HERE, in source order, and
        // is gone before the real walk registers it - the queued type argument still folds to it.
        std::optional<LLVMBackend::AliasScopeGuard> scanAliasScope;
        if (topLevel) scanAliasScope.emplace(Compiler());
        for (auto* child : ctx->children)
        {
            auto* ruleCtx = dynamic_cast<antlr4::RuleContext*>(child);
            if (!ruleCtx) continue;

            // Use getRuleIndex() (integer compare) instead of dynamic_cast for each node type.
            switch (ruleCtx->getRuleIndex())
            {
            case CFlatParser::RuleStructDefinition:
                // Skip generic template struct/class definition bodies (contain unbound T)
                if (static_cast<CFlatParser::StructDefinitionContext*>(ruleCtx)->genericTypeParameters() != nullptr)
                    continue;
                break;
            case CFlatParser::RuleClassDefinition:
                if (static_cast<CFlatParser::ClassDefinitionContext*>(ruleCtx)->genericTypeParameters() != nullptr)
                    continue;
                break;
            case CFlatParser::RuleUsingDeclaration:
                RegisterPureRenameAlias(Compiler(),
                                        static_cast<CFlatParser::UsingDeclarationContext*>(ruleCtx));
                break;
            case CFlatParser::RuleIfConstDeclaration:
            case CFlatParser::RuleIfConstMember:
            {
                // Declaration/member `if const`: same reason as the statement form - an alias an
                // arm declares is confined to a frame of its own and never reaches the file map.
                LLVMBackend::AliasScopeGuard armAliasScope(Compiler());
                ScanAndQueueGenericTypeUses(ruleCtx, /*topLevel*/ false);
                continue;
            }
            case CFlatParser::RuleSelectionStatement:
            {
                // Statement-level `if const`: the main pass walks ONLY the taken arm and a `using`
                // there is function-scoped, so scan that arm in the body frame, the dead arm apart.
                auto* sel = dynamic_cast<CFlatParser::SelectionStatementContext*>(ruleCtx);
                if (sel != nullptr && sel->If() != nullptr && sel->Const() != nullptr)
                {
                    auto folded = FoldCompileTimeInt(Compiler(), sel->expression());
                    if (!folded.has_value())
                    {
                        // Undecidable at scan time: no arm's alias may bind, so every arm scans in
                        // a frame of its own - the main pass stays the only one that decides.
                        LLVMBackend::AliasScopeGuard armAliasScope(Compiler());
                        ScanAndQueueGenericTypeUses(ruleCtx, /*topLevel*/ false);
                        continue;
                    }
                    auto arms = sel->statement();
                    bool isTrue = *folded != 0;
                    auto* taken = isTrue ? (arms.empty() ? nullptr : arms[0])
                                         : (arms.size() > 1 ? arms[1] : nullptr);
                    auto* dead = isTrue ? (arms.size() > 1 ? arms[1] : nullptr)
                                        : (arms.empty() ? nullptr : arms[0]);
                    ScanAndQueueGenericTypeUses(sel->expression(), /*topLevel*/ false);
                    if (dead != nullptr)
                    {
                        LLVMBackend::AliasScopeGuard deadArmAliasScope(Compiler());
                        ScanAndQueueGenericTypeUses(dead, /*topLevel*/ false);
                    }
                    if (taken != nullptr) ScanAndQueueGenericTypeUses(taken, /*topLevel*/ false);
                    continue;
                }
                break;
            }
            case CFlatParser::RuleDeclaration:
                {
                    // `auto x = new T[n];` deduces array<T> (see ArmArrayNewDesugar), which names
                    // no generic type in source - queue the instantiation here so its methods
                    // exist before the body is emitted. Children are still walked below.
                    auto* decl = static_cast<CFlatParser::DeclarationContext*>(ruleCtx);
                    auto* specs = decl->declarationSpecifiers();
                    auto* initList = decl->initDeclaratorList();
                    if (specs != nullptr && initList != nullptr && specs->getText() == "auto")
                    {
                        for (auto* initDecl : initList->initDeclarator())
                        {
                            auto* init = initDecl->initializer();
                            auto* ne = init != nullptr ? AsDirectNew(init->assignmentExpression()) : nullptr;
                            if (ne == nullptr || ne->assignmentExpression() == nullptr) continue;
                            std::string elem = ParseTypeSpecifierName(ne->typeSpecifier());
                            QueueGenericInstantiation("array", { elem }, MangledGenericName("array", { elem }));
                        }
                    }
                }
                break;
            case CFlatParser::RuleTypeSpecifier:
                {
                    // typeSpecifier with generic params: e.g. the "Box<MyInt>" in "Box<MyInt> b"
                    // Apply type substitutions for generic parameters.
                    auto* typeSpec = static_cast<CFlatParser::TypeSpecifierContext*>(ruleCtx);
                    if (typeSpec->tupleTypeSpecifier() != nullptr)
                    {
                        auto* tupleSpec = typeSpec->tupleTypeSpecifier();
                        if (tupleSpec->tupleTypePackEntry() == nullptr)
                        {
                            std::vector<std::string> typeArgs;
                            for (auto* entry : tupleSpec->tupleTypeEntry())
                                typeArgs.push_back(TupleEntryArgName(Compiler(entry), entry));
                            std::string mangledName = MangledGenericName("tuple", typeArgs);
                            tupleTypeArgs[mangledName] = typeArgs;
                            QueueGenericInstantiation("tuple", typeArgs, mangledName);
                        }
                        break;
                    }
                    std::string baseName;
                    // GenericSpecOf covers both spellings: bare 'Box<int>' and qualified 'NS.Box<int>'.
                    if (auto* genParams = GenericSpecOf(typeSpec, baseName))
                    {
                        baseName = Compiler()->ResolveGenericBaseAlias(baseName);
                        std::vector<std::string> typeArgs;
                        // ResolveTypeArgEntry applies active substitutions AND recursively
                        // resolves/queues nested generics (e.g. list<int> inside list<list<int>>).
                        for (auto* entry : genParams->typeParameterList()->typeParameterEntry())
                            typeArgs.push_back(ResolveTypeArgEntry(entry));
                        std::string mangledName = MangledGenericName(baseName, typeArgs);
                        if (!instantiatedGenerics.count(mangledName))
                        {
                            QueuePendingInstantiation(baseName, typeArgs, mangledName, genParams);
                            instantiatedGenerics.insert(mangledName);
                        }
                    }
                    break;
                }
            case CFlatParser::RulePrimaryExpression:
                {
                    // primaryExpression with generic params: e.g. the "Box<MyInt>" in "Box<MyInt>()"
                    // Apply type substitutions for generic parameters.
                    auto* primaryExpr = static_cast<CFlatParser::PrimaryExpressionContext*>(ruleCtx);
                    if (primaryExpr->genericIdentifier() != nullptr && primaryExpr->genericIdentifier()->genericTypeParameters() != nullptr && primaryExpr->genericIdentifier()->Identifier() != nullptr)
                    {
                        std::string baseName = Compiler()->ResolveGenericBaseAlias(
                            primaryExpr->genericIdentifier()->Identifier()->getText());
                        std::vector<std::string> typeArgs;
                        for (auto* entry : primaryExpr->genericIdentifier()->genericTypeParameters()->typeParameterList()->typeParameterEntry())
                            typeArgs.push_back(ResolveTypeArgEntry(entry));
                        std::string mangledName = MangledGenericName(baseName, typeArgs);
                        if (!instantiatedGenerics.count(mangledName))
                        {
                            QueuePendingInstantiation(baseName, typeArgs, mangledName,
                                primaryExpr->genericIdentifier()->genericTypeParameters());
                            instantiatedGenerics.insert(mangledName);
                        }
                    }
                    break;
                }
            }

            ScanAndQueueGenericTypeUses(ruleCtx, /*topLevel*/ false);
        }
    }
