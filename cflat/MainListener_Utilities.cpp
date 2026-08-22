#include "MainListener.h"

/*
 * A parameter list reads its types from declarationSpecifiers alone and never evaluates a
 * fixed dimension, so `T[N] p` would silently become a scalar `T p` - the body then indexes a
 * scalar and no `T[N]` argument can bind. Reject the spelling instead of decaying it. Three
 * spellings carry a dimension: bracketed `T[N] p`, an array alias, and the C-style `T p[N]`
 * (whose parameter name would otherwise bind as the text "p[N]"). Returns the diagnostic, or ""
 * when the parameter is not a fixed array. `T[]` never reaches here: empty brackets leave
 * ArraySize null and set IsArrayView.
 */
static std::string FixedArrayParamMessage(CFlatParser::ParameterDeclarationContext* paramDecl,
    const LLVMBackend::DeclTypeAndValue& paramType)
{
    auto* declarer = paramDecl->declarator();
    auto* direct = declarer != nullptr ? declarer->directDeclarator() : nullptr;
    bool bracketed = paramType.ArraySize != nullptr;
    bool aliased = !bracketed && paramType.AliasArraySize > 0;
    bool cstyle = !bracketed && !aliased && direct != nullptr && direct->assignmentExpression() != nullptr;
    if (!bracketed && !aliased && !cstyle)
        return {};

    // The spelling comes from the AST, never from TypeName: a funcptr lowers to "__c_fn_ptr".
    // An element pointer lives on the declarationSpecifier, beside the type, not in TypeName.
    std::string element;
    std::string stars;
    std::string dims;
    if (auto* specs = paramDecl->declarationSpecifiers())
    {
        for (auto* s : specs->declarationSpecifier())
        {
            if (s->typeSpecifier() == nullptr)
                continue;
            auto* dimSpec = ArrayDimsOf(s);
            if (bracketed && (dimSpec == nullptr || dimSpec->assignmentExpression().empty()))
                continue;
            element = s->typeSpecifier()->getText();
            stars = s->pointer() != nullptr ? s->pointer()->getText() : "";
            if (bracketed)
                dims = dimSpec->getText();
            break;
        }
    }
    if (element.empty())
        return {};
    if (cstyle)
        dims = "[" + direct->assignmentExpression()->getText() + "]";
    std::string spelled = element + stars + dims;
    std::string name = cstyle ? getDirectDeclName(direct) : paramType.VariableName;

    // An array ALIAS spells no brackets and no stars, so both come off the peeled element type.
    if (aliased && !paramType.IsFunctionPointer)
    {
        element = paramType.TypeName;
        stars = paramType.ElemPointer ? "**" : (paramType.Pointer ? "*" : "");
    }

    // A fat closure has no passable array spelling at all: 'Lambda<T>[]' and 'Lambda<T>*' are
    // both rejected, so offering a view here would send the user into a second error.
    if (paramType.IsFunctionPointer && !paramType.IsThinFnPtr())
        return std::format("fixed-size array parameter '{}' of type '{}' is not supported; a "
            "closure array cannot be passed in any spelling - take one closure by value instead.",
            name, spelled);

    // A view over a POINTER element ('T*[]') is unfeedable - no expression produces one - so a
    // pointer-element array gets the decay remedy the 'T[N]*' diagnostic already names.
    if (!stars.empty())
        return std::format("fixed-size array parameter '{}' of type '{}' is not supported; pass "
            "'{}{}*' instead (a fixed array decays to a pointer to its first element).",
            name, spelled, element, stars);

    return std::format("fixed-size array parameter '{}' of type '{}' is not supported; declare it "
        "as an array view '{}[]' instead.", name, spelled, element);
}

std::vector<LLVMBackend::DeclTypeAndValue> MainListener::ParseParameterTypeList(CFlatParser::ParameterTypeListContext* paramTypeList) {
        std::vector<LLVMBackend::DeclTypeAndValue> params;

        if (paramTypeList == nullptr)
            return params;

        auto paramList = paramTypeList->parameterList();
        auto paramDeclList = paramList->parameterDeclaration();

        for (auto paramDecl : paramDeclList)
        {
            if (HasSoftDeclarationSpecifier(paramDecl->declarationSpecifiers(), "manifest"))
                LogErrorContext(paramDecl, "manifest declarations are only allowed at file scope");
            LLVMBackend::DeclTypeAndValue paramType = this->ParseDeclarationSpecifiers(paramDecl->declarationSpecifiers());
            if (auto declarer = paramDecl->declarator())
            {
                if (auto directDeclarer = declarer->directDeclarator())
                {
                    paramType.VariableName = directDeclarer->getText();
                }
            }

            // D4: a `unique` parameter is a synthesized move parameter - the callee declares the
            // ownership sink, so passing a named unique value transfers and nulls it via the existing
            // move machinery. Post-substitution unique params already carry IsMove (set in
            // ParseDeclarationSpecifiers); this covers the direct `unique X*` spelling.
            if (paramType.IsUnique
                && ((paramType.Pointer && !paramType.ElemPointer
                && !paramType.IsArrayView) || paramType.IsInterface))
                paramType.IsMove = true;

            if (paramType.IsMove && paramType.IsBond)
                LogErrorContext(paramDecl, std::format("parameter '{}': 'bond' and 'move' are mutually exclusive", paramType.VariableName));

            // Fires before the body is walked, so no decayed signature reaches codegen.
            if (std::string fixedArray = FixedArrayParamMessage(paramDecl, paramType); !fixedArray.empty())
                LogErrorContext(paramDecl, fixedArray);

            // A parameter's `alignas(_, N)` allocation alignment now rides in declarationSpecifiers
            // (prefix): ParseDeclarationSpecifiers above already set paramType.AllocAlignValue. The
            // block a `move` param owns is N-aligned, so the callee frees it via __delete_aligned and
            // the call site checks the argument agrees.

            if (auto* lc = paramDecl->lockClause())
            {
                auto args = lc->lockArgList()->expression();
                std::string lockText = args.size() == 1 ? args[0]->getText() : std::string();
                if (args.size() == 1 && NormalizeLockText(lockText) == "this")
                {
                    paramType.LockThis = true;
                    paramType.LockThisMode = LockModeFromSuffix(LockTextMode(lockText));
                }
                else
                    LogErrorContext(paramDecl, "lock(this) is the only supported form on function parameters");
            }

            paramType.DefaultValue = paramDecl->initializer();
            params.push_back(paramType);

            if (paramType.VariableName == "")
            {
                PrintContext(paramDecl);
                std::cout << "Function parameter name is missing.\n";
            }
        }

        return params;
    }

LLVMBackend::ConstantVariant MainListener::ParseNumberConstant(std::string rawNumber) {
        // Support suffixes: u/U, l/L, ll/LL, f/F, d/D and floating point forms with '.' or exponent.
        if (rawNumber.empty())
            return 0;

        // Handle optional leading sign
        bool negative = false;
        std::string s = rawNumber;
        if (s[0] == '+' || s[0] == '-')
        {
            negative = (s[0] == '-');
            s = s.substr(1);
        }

        // Determine if this is a hex literal (0x...)
        bool isHex = (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'));

        // Extract numeric part and suffix carefully.
        size_t suffixPos = s.size();
        if (isHex)
        {
            // For hex, scan forward from the "0x" prefix to include all hex digits
            size_t idx = 2; // skip 0x
            while (idx < s.size() && std::isxdigit(static_cast<unsigned char>(s[idx]))) ++idx;
            suffixPos = idx;
        }
        else
        {
            // For non-hex, strip trailing letters that belong to known suffix set (u,l,f,d)
            while (suffixPos > 0)
            {
                char c = s[suffixPos - 1];
                char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (lc == 'u' || lc == 'l' || lc == 'f' || lc == 'd')
                    --suffixPos;
                else
                    break;
            }
        }

        std::string numberPart = s.substr(0, suffixPos);
        std::string suffix = s.substr(suffixPos);
        for (auto& c : suffix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        bool hasU = suffix.find('u') != std::string::npos;
        int lCount = 0;
        for (char c : suffix) if (c == 'l') ++lCount;
        bool hasF = suffix.find('f') != std::string::npos;
        bool hasD = suffix.find('d') != std::string::npos;

        // For hex literals, 'e'/'E' are valid hex digits, not exponent markers.
        // Only treat e/E as a float indicator for decimal/non-hex numbers.
        bool looksFloat = (numberPart.find('.') != std::string::npos) ||
            (!isHex && numberPart.find('e') != std::string::npos) ||
            (!isHex && numberPart.find('E') != std::string::npos);

        // Floating point handling
        if (hasF || hasD || looksFloat)
        {
            // strtod/strtof, NOT stod/stof: the latter throw out_of_range whenever errno is
            // ERANGE, and strtod sets ERANGE on a legal UNDERFLOW to a subnormal even though
            // it returns the correctly-rounded subnormal. Caught by the old blanket handler,
            // that silently turned a subnormal literal like 1e-320 into 0.0. Only a genuine
            // overflow (+-inf) is an error here.
            const char* cstr = numberPart.c_str();
            char* end = nullptr;

            if (hasF)
            {
                float f = std::strtof(cstr, &end);   // explicit 'f' suffix -> float
                if (end == cstr || std::isinf(f)) return 0.0;
                return f;
            }

            double dval = std::strtod(cstr, &end);
            if (end == cstr || std::isinf(dval)) return 0.0;
            if (hasD)
                return dval;                         // explicit 'd' suffix -> double

            // No suffix: mirror the integer rule (pick the smallest type that holds the
            // value exactly). Use float if the literal round-trips through float without
            // loss of precision; otherwise fall back to double. So 1.5 -> f32, but a
            // literal like 3.141592653589793 that loses precision in float stays f64.
            float fval = static_cast<float>(dval);
            if (static_cast<double>(fval) == dval)
                return fval;
            return dval;
        }

        // Integer handling. Support hex/octal/decimal by using base 0.
        unsigned long long uval = 0;
        try
        {
            if (numberPart.empty())
                uval = 0;
            else
                uval = std::stoull(numberPart, nullptr, 0);
        }
        catch (...) { uval = 0; }

        // If a long/long long suffix is present, prefer 64-bit result.
        if (lCount >= 1)
        {
            if (negative)
            {
                long long sval = -static_cast<long long>(uval);
                return static_cast<int64_t>(sval);
            }
            if (hasU)
                return static_cast<uint64_t>(uval);
            else
            {
                return static_cast<int64_t>(uval);
            }
        }

        // No explicit long suffix: pick smallest reasonable signed type unless 'u' forces unsigned semantics
        if (negative)
        {
            long long sval = -static_cast<long long>(uval);
            if (sval >= std::numeric_limits<int8_t>::min() && sval <= std::numeric_limits<int8_t>::max())
                return static_cast<char>(sval);
            if (sval >= std::numeric_limits<int16_t>::min() && sval <= std::numeric_limits<int16_t>::max())
                return static_cast<short>(sval);
            if (sval >= std::numeric_limits<int>::min() && sval <= std::numeric_limits<int>::max())
                return static_cast<int>(sval);
            return static_cast<int64_t>(sval);
        }

        if (hasU)
        {
            // A plain u suffix is a u32 literal, even when its value fits in signed int.
            if (uval <= 0xFFFFFFFFull)
                return static_cast<unsigned int>(static_cast<uint32_t>(uval));
            return static_cast<uint64_t>(uval);
        }

        if (uval <= static_cast<unsigned long long>(std::numeric_limits<int8_t>::max()))
            return static_cast<char>(uval);
        if (uval <= static_cast<unsigned long long>(std::numeric_limits<int16_t>::max()))
            return static_cast<short>(uval);
        if (uval <= static_cast<unsigned long long>(std::numeric_limits<int>::max()))
            return static_cast<int>(uval);
        // C++ rule: a hex literal past i32 range but inside u32 is unsigned; with no 'u' suffix
        // it stays i32 here, preserving the bit pattern (0xBA63E001 -> i32(-1168474111)).
        if (isHex && uval <= 0xFFFFFFFFull)
            return static_cast<int>(static_cast<uint32_t>(uval));
        return static_cast<int64_t>(uval);
    }

LLVMBackend::TypeAndValue MainListener::ParseLiteralTypeAndValue(const std::string& rawNumber) {
        LLVMBackend::TypeAndValue type;
        auto constant = ParseNumberConstant(rawNumber);
        if (std::get_if<unsigned char>(&constant) != nullptr)
            type.TypeName = "u8";
        else if (std::get_if<unsigned short>(&constant) != nullptr)
            type.TypeName = "u16";
        else if (std::get_if<unsigned int>(&constant) != nullptr)
            type.TypeName = "u32";
        else if (std::get_if<uint64_t>(&constant) != nullptr)
            type.TypeName = "u64";
        return type;
    }

void MainListener::LogWarningContext(antlr4::ParserRuleContext* ctx, std::string warningMessage) {
        size_t line = ctx->getStart()->getLine();
        size_t column = ctx->getStart()->getCharPositionInLine();
        std::cout << std::format("{}({},{}): {}\n", sourceFileName, line, column, warningMessage);
    }

void MainListener::PrintContext(antlr4::ParserRuleContext* ctx, std::string suffix) {
        size_t line = ctx->getStart()->getLine();
        size_t column = ctx->getStart()->getCharPositionInLine();
        std::cout << std::format("[{},{}] {} : {} : {}\n", line, column, parser->getRuleNames()[ctx->getRuleIndex()], ctx->getText(), suffix);
    }
