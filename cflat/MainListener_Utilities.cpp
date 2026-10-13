#include <algorithm>
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
            element = CanonicalDeclarationTypeName(specs->declarationSpecifier(), s->typeSpecifier()->getText());
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
            if (HasSoftDeclarationSpecifier(paramDecl->declarationSpecifiers(), "application"))
                LogErrorContext(paramDecl, "application declarations are only allowed at file scope");
            LLVMBackend::DeclTypeAndValue paramType = this->ParseDeclarationSpecifiers(paramDecl->declarationSpecifiers());
            if (auto declarer = paramDecl->declarator())
            {
                if (auto directDeclarer = declarer->directDeclarator())
                {
                    paramType.VariableName = directDeclarer->getText();
                }
            }

            if (paramType.IsMove && paramType.IsBond)
                LogErrorContext(paramDecl, std::format("parameter '{}': 'bond' and 'move' are mutually exclusive", paramType.VariableName));

            if (paramType.IsAdopt)
            {
                if (!paramType.IsFatInterfaceValue())
                    LogErrorContext(paramDecl, std::format(
                        "parameter '{}' declared 'adopt' must be a plain interface value",
                        paramType.VariableName));
                if (paramType.IsMove || paramType.IsBond || paramType.IsAlias
                    || paramType.IsUnique)
                    LogErrorContext(paramDecl, std::format(
                        "parameter '{}': 'adopt' cannot be combined with another ownership qualifier",
                        paramType.VariableName));
            }

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
        lastNumberLiteralIsBitPattern = false;
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
        bool hexFloat = false;
        if (isHex)
        {
            // Hex digits and '.', then an optional binary exponent (p/P, sign, decimal digits). The
            // exponent makes the literal floating (C: 0x1p-2f); the rest is the suffix (f/F/l/L).
            size_t idx = 2; // skip 0x
            while (idx < s.size() && (std::isxdigit(static_cast<unsigned char>(s[idx])) || s[idx] == '.')) ++idx;
            if (idx < s.size() && (s[idx] == 'p' || s[idx] == 'P'))
            {
                hexFloat = true;
                ++idx;
                if (idx < s.size() && (s[idx] == '+' || s[idx] == '-')) ++idx;
                while (idx < s.size() && std::isdigit(static_cast<unsigned char>(s[idx]))) ++idx;
            }
            else
            {
                // No exponent: integer digits only. A '.' here is refused by the caller (LogError).
                idx = 2;
                while (idx < s.size() && std::isxdigit(static_cast<unsigned char>(s[idx]))) ++idx;
            }
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
        bool looksFloat = hexFloat || (numberPart.find('.') != std::string::npos) ||
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

            // Hex floats keep an overflow as inf (clang: 0x1p2000 is double inf, 0x1p200f is float inf);
            // a decimal overflow stays 0 as before.
            if (hasF)
            {
                float f = std::strtof(cstr, &end);   // explicit 'f' suffix -> float
                if (end == cstr || (!hexFloat && std::isinf(f))) return 0.0;
                return f;
            }

            double dval = std::strtod(cstr, &end);
            if (end == cstr || (!hexFloat && std::isinf(dval))) return 0.0;
            if (hasD)
                return dval;                         // explicit 'd' suffix -> double
            // Hex float (C: 0x1p-2 is double): unsuffixed -> double. l/L takes the decimal rule below
            // (CFlat has no long double literal; decimal 0.25l is f32 too).
            if (hexFloat && lCount == 0)
                return dval;

            // No suffix: mirror the integer rule (pick the smallest type that holds the
            // value exactly). Use float if the literal round-trips through float without
            // loss of precision; otherwise fall back to double. So 1.5 -> f32, but a
            // literal like 3.141592653589793 that loses precision in float stays f64.
            float fval = static_cast<float>(dval);
            if (static_cast<double>(fval) == dval)
                return fval;
            return dval;
        }

        // Integer handling. Support hex/octal/decimal by using base 0. Values beyond uint64
        // use APInt so the source spelling is preserved instead of overflowing stoull.
        unsigned long long uval = 0;
        bool parsedU64 = true;
        try
        {
            if (numberPart.empty())
                uval = 0;
            else
            {
                size_t consumed = 0;
                if (numberPart.size() > 2 && numberPart[0] == '0' && (numberPart[1] == 'b' || numberPart[1] == 'B'))
                {
                    // strtoull's base 0 has no 0b prefix: parse the digits in base 2 (overflow -> wide path).
                    uval = std::stoull(numberPart.substr(2), &consumed, 2);
                    parsedU64 = consumed == numberPart.size() - 2;
                }
                else
                {
                    uval = std::stoull(numberPart, &consumed, 0);
                    parsedU64 = consumed == numberPart.size();
                }
            }
        }
        catch (...) { parsedU64 = false; }

        if (!parsedU64)
        {
            std::string digits = numberPart;
            unsigned radix = 10;
            if (digits.size() >= 2 && digits[0] == '0'
                && (digits[1] == 'x' || digits[1] == 'X'))
            {
                radix = 16;
                digits = digits.substr(2);
            }
            else if (digits.size() >= 2 && digits[0] == '0'
                     && (digits[1] == 'b' || digits[1] == 'B'))
            {
                radix = 2;
                digits = digits.substr(2);
            }
            else if (digits.size() > 1 && digits[0] == '0')
            {
                radix = 8;
                digits = digits.substr(1);
            }
            // Only a well-formed digit string takes the wide path: this parser also sees
            // non-numeric literal text (a char literal), which the old path treated as 0.
            const bool validDigits = !digits.empty() && std::all_of(digits.begin(), digits.end(),
                [radix](unsigned char c) {
                    if (!std::isalnum(c)) return false;
                    unsigned d = std::isdigit(c) ? c - '0' : std::tolower(c) - 'a' + 10;
                    return d < radix;
                });
            if (validDigits && digits.size() <= 128)
            {
                llvm::APInt wide(128, digits, radix);
                if (negative) wide = -wide;
                return LLVMBackend::WideIntegerConstant{
                    wide, !negative && wide.ugt(llvm::APInt::getSignedMaxValue(128)) };
            }
            uval = 0;
        }

        // Sign is an operator on the magnitude: a negative literal keeps its existing folding.
        if (negative)
        {
            if (lCount >= 1)
            {
                // LLP64: a literal whose [lex.icon] type is 'long'/'ulong' lowers at that 32-bit width.
                if (LLVMBackend::longBits_ == 32)
                {
                    const std::string identity = LLVMBackend::LiteralIdentityForOverload(rawNumber);
                    if (identity == "long" || identity == "ulong")
                        return static_cast<int>(-static_cast<long long>(uval));
                }
                return static_cast<int64_t>(-static_cast<long long>(uval));
            }
            long long sval = -static_cast<long long>(uval);
            if (sval >= std::numeric_limits<int8_t>::min() && sval <= std::numeric_limits<int8_t>::max())
                return static_cast<char>(sval);
            if (sval >= std::numeric_limits<int16_t>::min() && sval <= std::numeric_limits<int16_t>::max())
                return static_cast<short>(sval);
            if (sval >= std::numeric_limits<int>::min() && sval <= std::numeric_limits<int>::max())
                return static_cast<int>(sval);
            return static_cast<int64_t>(sval);
        }

        // Unsuffixed value that fits int: CFlat's smallest-type typing (char/short/int).
        const bool suffixed = hasU || lCount >= 1;
        if (!suffixed && uval <= static_cast<unsigned long long>(std::numeric_limits<int>::max()))
        {
            if (uval <= static_cast<unsigned long long>(std::numeric_limits<int8_t>::max()))
                return static_cast<char>(uval);
            if (uval <= static_cast<unsigned long long>(std::numeric_limits<int16_t>::max()))
                return static_cast<short>(uval);
            return static_cast<int>(uval);
        }

        // Everything else takes the [lex.icon] ladder: one table in LLVMBackend, shared with the overload
        // identity. Lowered width feeds the switch-label bit-pattern match (see CollectCases).
        const bool nonDecimal = numberPart.size() > 1 && numberPart[0] == '0';   // 0x, 0b, octal
        const LLVMBackend::IntegerLiteralRung rung = LLVMBackend::IntegerLiteralLadderPick(
            uval, nonDecimal, hasU, lCount);
        const bool longIs64 = LLVMBackend::longBits_ == 64;
        lastNumberLiteralIsBitPattern = true;
        lastNumberLiteralBits = uval;
        switch (rung)
        {
        case LLVMBackend::IntegerLiteralRung::Int:
            lastNumberLiteralWidth = 32;
            return static_cast<int>(uval);
        case LLVMBackend::IntegerLiteralRung::UInt:
            lastNumberLiteralWidth = 32;
            return static_cast<unsigned int>(static_cast<uint32_t>(uval));
        case LLVMBackend::IntegerLiteralRung::Long:
            lastNumberLiteralWidth = longIs64 ? 64 : 32;
            if (longIs64) return static_cast<int64_t>(uval);
            return static_cast<int>(static_cast<uint32_t>(uval));
        case LLVMBackend::IntegerLiteralRung::ULong:
            lastNumberLiteralWidth = longIs64 ? 64 : 32;
            if (longIs64) return static_cast<uint64_t>(uval);
            return static_cast<unsigned int>(static_cast<uint32_t>(uval));
        case LLVMBackend::IntegerLiteralRung::U64:
            lastNumberLiteralWidth = 64;
            return static_cast<uint64_t>(uval);
        case LLVMBackend::IntegerLiteralRung::I64:
        case LLVMBackend::IntegerLiteralRung::None:
        default:
            // None: a decimal past i64 max with no suffix (no row holds it). Keeps the bit pattern.
            lastNumberLiteralWidth = 64;
            return static_cast<int64_t>(uval);
        }
    }

LLVMBackend::TypeAndValue MainListener::ParseLiteralTypeAndValue(const std::string& rawNumber) {
        LLVMBackend::TypeAndValue type;
        if (rawNumber == "nullptr" || rawNumber == "true" || rawNumber == "false")
            return type;
        if (rawNumber.size() >= 3 && rawNumber.back() == '\'')
        {
            if (rawNumber.front() == '\'') type.TypeName = "char";
            else if (rawNumber.size() >= 4 && rawNumber[1] == '\'')
            {
                if (rawNumber.front() == 'L') type.TypeName = "wchar";
                else if (rawNumber.front() == 'u') type.TypeName = "c16";
                else if (rawNumber.front() == 'U') type.TypeName = "c32";
            }
            return type;
        }
        auto constant = ParseNumberConstant(rawNumber);
        if (std::get_if<LLVMBackend::WideIntegerConstant>(&constant) != nullptr)
            type.TypeName = std::get<LLVMBackend::WideIntegerConstant>(constant).IsUnsigned ? "u128" : "i128";
        else if (std::get_if<unsigned char>(&constant) != nullptr)
            type.TypeName = "u8";
        else if (std::get_if<unsigned short>(&constant) != nullptr)
            type.TypeName = "u16";
        else if (std::get_if<unsigned int>(&constant) != nullptr)
            type.TypeName = "u32";
        else if (std::get_if<uint64_t>(&constant) != nullptr)
            type.TypeName = "u64";
        // The [lex.icon] row names the type of a suffixed literal and of an UNSIGNED unsuffixed one, so an
        // unsuffixed 0x8000000000000000 is unsigned long, not u64. An unsuffixed signed literal keeps its
        // untyped int64 form (typeof reports i64), as before the ladder.
        bool suffixedInteger = false;
        const std::string ladderType = LLVMBackend::LiteralIdentityForOverload(rawNumber, &suffixedInteger);
        const bool unsignedRow = ladderType == "u32" || ladderType == "ulong" || ladderType == "u64";
        if (!ladderType.empty() && ladderType != "int" && (suffixedInteger || unsignedRow))
            type.TypeName = ladderType;
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
