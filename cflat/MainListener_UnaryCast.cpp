#include "MainListener.h"
#include <llvm/Support/SaveAndRestore.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicInst.h>
#include "MainListener_ExpressionsShared.h"
void MainListener::ResolveValuelessCastOperand(CFlatParser::CastExpressionContext* ctx,
                                               CFlatParser::CastExpressionContext* operandCtx,
                                               LLVMBackend::NamedVariable& namedVar,
                                               const LLVMBackend::TypeAndValue& destTypeName) {
        if (namedVar.Primary != nullptr || namedVar.Storage != nullptr)
            return;

        // Compiler() without a ctx: the ctx overload calls SetSourceLocation, which would move
        // the caret of every later diagnostic from the operand to the cast's '('.
        auto* compiler = Compiler();

        // A monomorphized instantiation's mangled name is unique, so it resolves exactly the way
        // a plain named function does (ParseIdentifier's GetFunctionForFuncPtr(name) arm).
        if (!namedVar.CallerName.empty())
        {
            namedVar.Primary = compiler->GetFunctionForFuncPtr(namedVar.CallerName);
            if (namedVar.Primary != nullptr)
            {
                // Instantiating the template left the caret inside the template BODY; point it at
                // the operand, which is where the plain named-function spelling leaves it.
                if (operandCtx != nullptr)
                    Compiler(operandCtx);
                return;
            }
        }

        std::string operandText = operandCtx != nullptr ? operandCtx->getText() : std::string();
        // The base name is the spelling before any type-argument list, so a template whose
        // arguments failed to form an instantiation is still recognized as one.
        std::string operandBase = operandText.substr(0, operandText.find('<'));
        bool isTemplate = !operandBase.empty()
            && genericFunctionTemplates.count(compiler->ResolveGenericFunctionBase(operandBase)) != 0;

        // A bare generic function template resolves to nothing until it is instantiated; name the
        // remedy, which the branch above compiles.
        if (isTemplate && operandBase == operandText)
        {
            LogErrorContext(ctx, std::format(
                "'{}' is a generic function template and has no value until it is instantiated; "
                "give it explicit type arguments (e.g. '{}<int>').", operandText, operandText));
        }
        // Type arguments WERE written but no instantiation came of them - wrong count, or an
        // argument that is not a type. Name that rather than claiming it is not a value.
        else if (isTemplate)
        {
            LogErrorContext(ctx, std::format(
                "'{}' did not instantiate the generic function template '{}', so it has no value; "
                "check the number and spelling of its type arguments.", operandText, operandBase));
        }
        else
        {
            // Spell the destination from the source text; a synthesized rendering would have to
            // guess at array-view, function-pointer and alias spellings.
            std::string destText = ctx->castTypeName() != nullptr ? ctx->castTypeName()->getText()
                : SpellType(*compiler, destTypeName);
            LogErrorContext(ctx, std::format(
                "'{}' does not name a value, so it cannot be cast to '{}' - a type name, a namespace "
                "and an uninstantiated generic template are not values.",
                operandText, destText));
        }
    }

// The name of `(name)(args)`: a bare or namespace-qualified spelling with no type arguments, or null.
static CFlatParser::TypeSpecifierContext* CastCalleeSpelling(CFlatParser::CastExpressionContext* ctx)
{
        auto* castName = ctx->castTypeName();
        if (castName == nullptr || ctx->castExpression() == nullptr || ctx->children.size() != 4
            || castName->typeQualifier() != nullptr || castName->abstractDeclarator() != nullptr
            || castName->typeSpecifier().size() != 1) return nullptr;
        auto* spec = castName->typeSpecifier()[0];
        if (auto* generic = spec->genericIdentifier())
            return generic->genericTypeParameters() == nullptr ? spec : nullptr;
        if (auto* qualified = spec->qualifiedGenericIdentifier())
            return qualified->genericTypeParameters() == nullptr ? spec : nullptr;
        return nullptr;
}

static bool CallFragmentIsWhole(CFlatParser& parser)
{
        return parser.getNumberOfSyntaxErrors() == 0
            && parser.getCurrentToken()->getType() == cflat::kTokenEOF;
}

// `(name)(args)` is a call when `name` is a function, or a visible variable that is callable. A visible
// variable decides first: a non-callable one shadows any function of its name, so the cast keeps its
// own "unknown type" diagnostic, as on master.
bool MainListener::CastIsCallSpelling(CFlatParser::CastExpressionContext* ctx)
{
        auto* spec = CastCalleeSpelling(ctx);
        if (spec == nullptr) return false;
        auto* compiler = Compiler(ctx);
        const std::string name = spec->getText();
        if (compiler->IsKnownTypeName(name)) return false;
        if (NamesVisibleVariable(compiler, name))
            return compiler->IsFunctionValueType(VisibleVariableType(compiler, name));
        const std::string resolved = compiler->ResolveQualifiedName(name);
        return compiler->functionTable.count(name) != 0 || compiler->functionTable.count(resolved) != 0
            || genericFunctionTemplates.count(name) != 0 || genericFunctionTemplates.count(resolved) != 0
            || genericFunctionTemplates.count(compiler->ResolveGenericFunctionBase(name)) != 0;
}

// The text of `(name)(args)` with the header parentheses blanked, placed at the original line and
// column of every token. Empty when the cast does not have the call shape.
std::string MainListener::CastCallFragment(CFlatParser::CastExpressionContext* ctx)
{
        auto* spec = CastCalleeSpelling(ctx);
        auto* closeNode = dynamic_cast<antlr4::tree::TerminalNode*>(ctx->children[2]);
        antlr4::CharStream* chars = ctx->start->getInputStream();
        if (spec == nullptr || closeNode == nullptr || chars == nullptr
            || ctx->castExpression()->stop == nullptr) return {};
        antlr4::Token* open = ctx->start;
        antlr4::Token* nameTok = spec->getStart();
        antlr4::Token* nameLast = spec->getStop();
        antlr4::Token* close = closeNode->getSymbol();
        // Keep line breaks in the blanked header, so every token keeps its line.
        auto blank = [](const std::string& s) {
            std::string blanked;
            for (char c : s) blanked += c == '\n' ? '\n' : ' ';
            return blanked;
        };
        std::string fragment(open->getLine() - 1, '\n');
        fragment.append(open->getCharPositionInLine(), ' ');
        fragment += blank(chars->getText(antlr4::misc::Interval(open->getStartIndex(),
            nameTok->getStartIndex() - 1)));
        fragment += chars->getText(antlr4::misc::Interval(nameTok->getStartIndex(),
            nameLast->getStopIndex()));
        fragment += blank(chars->getText(antlr4::misc::Interval(nameLast->getStopIndex() + 1,
            close->getStopIndex())));
        fragment += chars->getText(antlr4::misc::Interval(close->getStopIndex() + 1,
            ctx->castExpression()->stop->getStopIndex()));
        return fragment;
}

// The initializer as the call it spells: `R r = (makeR)(7);` becomes the tree of `makeR(7)`, so the
// C++ class-initialization routes that read a call see the same shape as the bare spelling.
CFlatParser::AssignmentExpressionContext* MainListener::CastCallInitializer(
    CFlatParser::AssignmentExpressionContext* assign)
{
        if (assign == nullptr) return nullptr;
        auto* cast = tryGetCastExpression(assign);
        if (cast == nullptr || cast->stop != assign->stop || !CastIsCallSpelling(cast)) return assign;
        const std::string text = CastCallFragment(cast);
        if (text.empty()) return assign;
        auto* parse = NewCallFragment(Compiler(assign), text);
        auto* call = parse->parser.assignmentExpression();
        return CallFragmentIsWhole(parse->parser) ? call : assign;
}

MainListener::CallFragmentParse* MainListener::NewCallFragment(LLVMBackend* compiler, const std::string& text)
{
        auto parse = std::make_unique<CallFragmentParse>(text);
        parse->lexer.removeErrorListeners();
        parse->parser.removeErrorListeners();
        parse->parser.setErrorHandler(std::make_shared<CFlatErrorStrategy>(compiler->MakeDiagnosticLocalizer()));
        callFragmentParses_.push_back(std::move(parse));
        return callFragmentParses_.back().get();
}

bool MainListener::TryReparseCastAsCall(CFlatParser::CastExpressionContext* ctx, bool lvalue,
                                        ResultUse use, LLVMBackend::NamedVariable& out)
{
        if (!CastIsCallSpelling(ctx)) return false;
        const std::string fragment = CastCallFragment(ctx);
        return !fragment.empty() && ReparseFragmentAsCast(Compiler(ctx), fragment, lvalue, use, out);
}

// Parses `fragment` as one cast-expression and emits it. A syntax error returns false and emits nothing.
bool MainListener::ReparseFragmentAsCast(LLVMBackend* compiler, const std::string& fragment, bool lvalue,
                                         ResultUse use, LLVMBackend::NamedVariable& out)
{
        auto* parse = NewCallFragment(compiler, fragment);
        auto* call = parse->parser.castExpression();
        if (!CallFragmentIsWhole(parse->parser)) return false;
        NormalizeForInitTree(parse->parser, call);
        out = ParseCastExpression(call, lvalue, use);
        return true;
}

LLVMBackend::NamedVariable MainListener::ParseCastExpression(CFlatParser::CastExpressionContext* ctx, bool lvalue,
                                                   ResultUse use) {
        LLVMBackend::NamedVariable rerouted;
        if (TryReparseCastAsCall(ctx, lvalue, use, rerouted)) return rerouted;
        auto result = ParseCastExpressionImpl(ctx, lvalue, use);
        // An explicit cast to a pointer gives it exactly the spelled qualification: it may drop or
        // add const at any level (R7 A).
        if (ctx->castExpression() != nullptr && ctx->castTypeName() != nullptr
            && (result.TypeAndValue.Pointer || result.TypeAndValue.IsArrayView))
        {
            auto* compiler = Compiler(ctx);
            std::string ignored;
            const uint8_t before = compiler->StoredConstLevels(result.Primary, ignored);
            result.Primary = compiler->MarkCastQualification(result.Primary, result.TypeAndValue,
                                                             CollapsedSourceText(ctx));
            // A re-read of the operand's own slot (`((int*)p)[0]`) would lose the stamp, so a cast
            // that changes the qualification reads through a slot of its own.
            const uint8_t depthMask = static_cast<uint8_t>((1u << result.TypeAndValue.ValuePointerDepth()) - 1u);
            // A view keeps its slot: its element count lives beside it.
            if (auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(result.Primary);
                load != nullptr && !result.TypeAndValue.IsArrayView && result.Storage != nullptr
                && load->getPointerOperand() == result.Storage
                && ((before ^ compiler->DeclaredPointeeConstLevels(result.TypeAndValue)) & depthMask) != 0)
            {
                auto* slot = compiler->CreateAlloca(result.Primary->getType());
                compiler->builder->CreateStore(result.Primary, slot);
                result.Storage = slot;
            }
        }
        // An explicit cast spells its type: `(int)(s + 1)` is an int, never a narrow-arithmetic mark.
        if (ctx->castTypeName() != nullptr)
            result.ArithNarrowWidth = 0;
        return result;
}

LLVMBackend::NamedVariable MainListener::ParseCastExpressionImpl(CFlatParser::CastExpressionContext* ctx, bool lvalue,
                                                   ResultUse use) {
        auto* compiler = Compiler(ctx);
        auto unaryCtx = ctx->unaryExpression();
        auto castExp = ctx->castExpression();
        auto typeName = ctx->castTypeName();

        if (unaryCtx != nullptr)
        {
            // Single-child passthrough: forward the use unchanged.
            return ParseUnaryExpression(unaryCtx, use);
        }
        else if (castExp && typeName)
        {
            // `(name) rest` where `name` is both a visible variable and a type: ANTLR has already
            // chosen the cast, and C would read an expression. Refuse rather than guess.
            if (auto* bareName = BareCastTypeNameIdentifier(typeName))
            {
                const std::string candidate = bareName->getText();
                PrimitiveTypeError spellingError;
                std::string typeCandidate;
                CanonicalizePrimitiveTypeWords({ candidate }, typeCandidate, spellingError);
                if (!HasPrimitiveTypeError(spellingError)
                    && compiler->IsKnownTypeName(typeCandidate)
                    && NamesVisibleVariable(compiler, candidate))
                {
                    LogErrorContext(ctx, compiler->LocalizeMessage(
                        "'({})' is ambiguous: '{}' names both a variable and a type; remove the parentheses or rename the variable",
                        { candidate, candidate }));
                    return {};
                }
            }
            auto destTypeName = ParseCastTypeName(typeName, false, ctx);
            // The explicit cast target is the operand's conversion context. Do not let the
            // outer initializer/return destination leak through the cast into its operand.
            DeclExpectedTypeScope castExpectedScope(&declExpectedType, destTypeName);
            // A cast to 'void' DISCARDS its operand, so a value-less call is legal under it -
            // read the destination off the spelling (alias-resolved) before parsing the operand.
            bool destIsVoid = compiler->ResolveTypeAlias(destTypeName.TypeName) == "void"
                && !destTypeName.Pointer && !destTypeName.ElemPointer && !destTypeName.IsArrayView;
            const bool castDefaultInPlace = compiler->pendingCxxSretDest_ != nullptr
                && !destTypeName.Pointer && !destTypeName.ElemPointer
                && !destTypeName.IsArrayView
                && compiler->ResolveTypeAlias(compiler->pendingCxxSretTypeName_)
                    == compiler->ResolveTypeAlias(destTypeName.TypeName)
                && compiler->IsForeignNontrivialCxxClass(
                    compiler->ResolveTypeAlias(destTypeName.TypeName))
                && IsSameTypeCastOfDefault(ctx, destTypeName.TypeName);
            bool castInitIdentityCall = false;
            if (ctx == pendingCxxCastInitContext_
                && compiler->pendingCxxSretDest_ != nullptr
                && !destTypeName.Pointer && !destTypeName.ElemPointer
                && !destTypeName.IsArrayView
                && compiler->ResolveTypeAlias(compiler->pendingCxxSretTypeName_)
                    == compiler->ResolveTypeAlias(destTypeName.TypeName)
                && compiler->IsForeignNontrivialCxxClass(
                    compiler->ResolveTypeAlias(destTypeName.TypeName)))
            {
                auto solePostfix = [&](auto&& self, antlr4::tree::ParseTree* node)
                    -> CFlatParser::PostfixExpressionContext* {
                    if (auto* postfix = dynamic_cast<CFlatParser::PostfixExpressionContext*>(node))
                        return postfix->argumentExpressionList().size() == 1
                            && postfix->children.size() == 4 ? postfix : nullptr;
                    if (auto* cast = dynamic_cast<CFlatParser::CastExpressionContext*>(node);
                        cast != nullptr && cast->castTypeName() != nullptr
                            && cast->castExpression() != nullptr && !CastIsCallSpelling(cast))
                    {
                        const auto nestedType = ParseCastTypeName(cast->castTypeName(), false, cast);
                        if (nestedType.Pointer || nestedType.ElemPointer || nestedType.IsArrayView
                            || compiler->ResolveTypeAlias(nestedType.TypeName)
                                != compiler->ResolveTypeAlias(destTypeName.TypeName))
                            return nullptr;
                        return self(self, cast->castExpression());
                    }
                    if (auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node);
                        primary != nullptr && primary->expression() != nullptr
                            && primary->children.size() == 3)
                        return self(self, primary->expression());
                    auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(node);
                    if (rule == nullptr) return nullptr;
                    if (rule->children.size() == 1)
                        return self(self, rule->children[0]);
                    return nullptr;
                };
                auto* postfix = solePostfix(solePostfix, castExp);
                if (postfix != nullptr)
                {
                    size_t callSuffixes = 0;
                    auto countCalls = [&](auto&& self, antlr4::tree::ParseTree* node) -> void {
                        if (auto* call = dynamic_cast<CFlatParser::PostfixExpressionContext*>(node))
                            callSuffixes += call->argumentExpressionList().size();
                        if (node != nullptr)
                            for (auto* child : node->children) self(self, child);
                    };
                    countCalls(countCalls, castExp);
                    castInitIdentityCall = callSuffixes == 1;
                }
            }
            LLVMBackend::NamedVariable namedVar;
            {
                // Conversion casts hide the initializer slot. Transparent identity casts around
                // a direct call may pass it through for guaranteed elision.
                struct HideCastInitDestination
                {
                    LLVMBackend* compiler;
                    llvm::Value* destination = nullptr;
                    std::string typeName;
                    bool active = false;
                    HideCastInitDestination(LLVMBackend* compiler, bool hide)
                        : compiler(compiler)
                    {
                        if (!hide || compiler->pendingCxxSretDest_ == nullptr) return;
                        destination = compiler->pendingCxxSretDest_;
                        typeName = compiler->pendingCxxSretTypeName_;
                        compiler->pendingCxxSretDest_ = nullptr;
                        compiler->pendingCxxSretTypeName_.clear();
                        active = true;
                    }
                    ~HideCastInitDestination()
                    {
                        if (!active) return;
                        compiler->pendingCxxSretDest_ = destination;
                        compiler->pendingCxxSretTypeName_ = std::move(typeName);
                    }
                } hide(compiler, ctx == pendingCxxCastInitContext_
                    && !castInitIdentityCall && !castDefaultInPlace);
                namedVar = ParseCastExpression(castExp, false,
                    destIsVoid ? ResultUse::Discard : ResultUse::Value);
            }
            if (castDefaultInPlace && compiler->pendingCxxSretDest_ == nullptr)
            {
                namedVar.TypeAndValue = destTypeName;
                namedVar.IsRvalue = true;
                return namedVar;
            }
            // A cast result is a new value: `(long)ZERO` is not the macro's literal (clang refuses it).
            namedVar.IsCxxMacroGlobal = false;
            namedVar.IsCxxNullIntegerMacro = false;
            // A cast names the C++ type itself: `(long)(ul + 1)` is no longer `unsigned long`.
            namedVar.CxxArithIdentity.clear();
            const std::string castSourceType =
                compiler->ResolveTypeAlias(namedVar.TypeAndValue.TypeName);
            if (IsRedundantCastOfSource(namedVar.TypeAndValue, destTypeName)
                && !namedVar.TypeAndValue.Pointer
                && !namedVar.IsExplicitMove
                && compiler->IsCxxRecord(castSourceType))
            {
                // `default` has storage, but its same-type cast is still a prvalue no-op.
                if (IsSameTypeCastOfDefault(ctx, destTypeName.TypeName))
                {
                    namedVar.IsRvalue = true;
                    return namedVar;
                }
                // A same-type cast of an lvalue is a prvalue copy in C++; consuming destinations
                // keep their existing elision path, while expression uses need their own object.
                if (namedVar.IsRvalue || namedVar.Storage == nullptr
                    || ctx == pendingCxxCastInitContext_ || use == ResultUse::ReturnOperand)
                    return namedVar;

                if (auto* outerCast = dynamic_cast<CFlatParser::CastExpressionContext*>(ctx->parent);
                    outerCast != nullptr && outerCast->castTypeName() != nullptr
                    && !CastIsCallSpelling(outerCast))
                {
                    auto outerType = ParseCastTypeName(outerCast->castTypeName(), false, outerCast);
                    if (IsRedundantCastOfSource(destTypeName, outerType))
                        return namedVar;
                }

                auto* classType = compiler->GetType(destTypeName);
                auto* temp = compiler->CreateAlloca(classType);
                // The cast temporary is direct-initialized: an explicit copy ctor is a candidate.
                if (!compiler->EmitCxxCopyOrMoveConstruct(castSourceType, temp, namedVar.Storage,
                        /*useMove*/ false, "for an explicit cast temporary", {}, false,
                        /*directInit*/ true))
                    return {};
                compiler->RegisterOwnedStructTemp(temp, castSourceType);

                LLVMBackend::NamedVariable castValue;
                castValue.TypeAndValue = destTypeName;
                // The cast spelling can be namespace-relative; retain the resolved operand name.
                castValue.TypeAndValue.TypeName = namedVar.TypeAndValue.TypeName;
                castValue.TypeAndValue.VariableName.clear();
                castValue.TypeAndValue.ParentVariableName.clear();
                castValue.BaseType = classType;
                castValue.Primary = compiler->CreateLoad(classType, temp);
                castValue.Storage = temp;
                castValue.IsRvalue = true;
                castValue.IsCxxPrvalueTemp = true;
                castValue.IsCxxSameTypeCastCopy = true;
                castValue.CxxLvalueKind = 0;
                return castValue;
            }
            auto type = compiler->GetType(destTypeName);

            // Materialize a stored operand into Primary. A fixed-size array decays to
            // a pointer to its first element ((u8*)buf on char[N], (char**)a on
            // char*[N]); loading the whole [N x T] aggregate would feed an invalid
            // aggregate-to-pointer bitcast. Everything else loads with the source
            // type so we read the correct number of bytes - loading directly with the
            // destination type reads too many bytes from narrower fields (e.g., an
            // i64 load from a u32 field corrupts the adjacent field).
            auto materialize = [&](LLVMBackend::NamedVariable& nv)
            {
                if (nv.Primary != nullptr || nv.Storage == nullptr)
                    return;
                if (nv.BaseType && llvm::isa<llvm::ArrayType>(nv.BaseType))
                {
                    auto* arrTy = llvm::cast<llvm::ArrayType>(nv.BaseType);
                    auto* zero = compiler->builder->getInt64(0);
                    nv.Primary = compiler->builder->CreateGEP(arrTy, nv.Storage, {zero, zero}, "arrptr");
                }
                else
                {
                    auto srcType = compiler->GetType(nv.TypeAndValue);
                    nv.Primary = compiler->CreateLoad(srcType, nv.Storage);
                }
                nv.Storage = nullptr;
            };

            ResolveValuelessCastOperand(ctx, castExp, namedVar, destTypeName);

            // A literal's machine value is already a pointer, but its CFlat type identity is
            // otherwise deferred until a context consumes it. Explicit casts need the same
            // char* source shape as a local so conversion overloads rank it correctly.
            auto* castStringConstant = llvm::dyn_cast_or_null<llvm::Constant>(namedVar.Primary);
            if ((namedVar.IsStringLiteral || compiler->IsStringLiteralConstant(castStringConstant))
                && namedVar.TypeAndValue.TypeName.empty())
            {
                namedVar.TypeAndValue.TypeName = "char";
                namedVar.TypeAndValue.Pointer = true;
                namedVar.TypeAndValue.PointerDepth = 1;
            }

            // `(void)expr` DISCARDS the operand's value and keeps only its side effects - C's
            // meaning for a void cast. The operand was already parsed at ResultUse::Discard above
            // (destIsVoid), so its effects already ran; produce the same value-less shape a void
            // CALL hands back (no Primary, no Storage) instead of ever reaching CreateCast, which
            // has no valid IR for a void destination (a bitcast to void is invalid, and folds to
            // garbage on a constant operand instead of erroring). Pointer/array-view destinations
            // ('(void*)p', '(V[])p') are real reinterprets, not discards, so they fall through.
            if (destIsVoid && !destTypeName.Pointer && !destTypeName.IsArrayView)
            {
                namedVar.Primary = nullptr;
                namedVar.Storage = nullptr;
                namedVar.TypeAndValue = LLVMBackend::TypeAndValue{};
                namedVar.TypeAndValue.TypeName = "void";
                return namedVar;
            }

            // Answered on the OPERAND's own type, before any of the lowering below rewrites it.
            bool redundantCastOfSource = IsRedundantCastOfSource(namedVar.TypeAndValue, destTypeName);
            llvm::Value* redundantCastStorage = redundantCastOfSource ? namedVar.Storage : nullptr;
            LLVMBackend::NamedVariable redundantCastSource = namedVar;

            // If the destination is a struct VALUE type and an operator overload
            // exists, call it (e.g. (string)charPtr calls operator string(char*)).
            // A POINTER (or array-view) destination is a pure reinterpret, never a
            // value conversion: '(string*)p' must not invoke 'operator string'. Doing
            // so produced a 'string' VALUE where a 'string*' was expected, which then
            // asserted the compiler downstream (e.g. an invalid bitcast in
            // 'delete[_] (string*)expr', or a 'no overload' error when no value
            // conversion matched the pointer operand).
            std::string operatorTargetSpelling = SpellType(*compiler, destTypeName);
            if (operatorTargetSpelling.empty()) operatorTargetSpelling = destTypeName.TypeName;
            std::string opName = "operator " + operatorTargetSpelling;
            bool isSpecialConversion = destTypeName.TypeName == "string";
            if (isSpecialConversion && !destTypeName.Pointer && !destTypeName.IsArrayView
                && compiler->GetFunction(opName) != nullptr)
            {
                auto argNV = namedVar;
                argNV.TypeAndValue.VariableName = "";  // clear name so positional matching is used
                // A C++ class keeps its storage: the conversion operator runs on the object.
                llvm::Value* cxxReceiverStorage =
                    (argNV.Primary == nullptr && !argNV.TypeAndValue.Pointer
                     && compiler->IsCxxRecord(argNV.TypeAndValue.TypeName))
                        ? argNV.Storage : nullptr;
                materialize(argNV);
                if (cxxReceiverStorage != nullptr) argNV.Storage = cxxReceiverStorage;

                // A bare `(string)primitive` cast is no longer a public conversion - a
                // primitive's NamedVariable here is an integer/floating-point scalar value
                // (char*/IString operands are pointer/struct values and still cast). Direct
                // the user to value.toString(); interpolation "{x}" and string concatenation
                // keep working because they reach the internal operator string hook by name,
                // not through this cast path. (This also covers the former literal-cast crash.)
                if (destTypeName.TypeName == "string" && argNV.Primary != nullptr
                    && (argNV.Primary->getType()->isIntegerTy()
                        || argNV.Primary->getType()->isFloatingPointTy()))
                {
                    LogErrorContext(ctx, "cannot cast a primitive value to 'string'; call "
                        "'value.toString()' instead (string interpolation \"{x}\" and '+' "
                        "concatenation still convert automatically)");
                    namedVar.TypeAndValue = destTypeName;
                    return namedVar;
                }

                auto result = compiler->CreateOverloadedFunctionCall(opName, { argNV });
                namedVar.Primary = result;
                namedVar.Storage = nullptr;
                namedVar.TypeAndValue = destTypeName;
                return namedVar;
            }

            // A C++ class operand keeps its storage for the conversion operator: a self-pointer
            // or mutating `operator T` must run on the object, not on a bitwise copy.
            llvm::Value* cxxCastStorage =
                (namedVar.Primary == nullptr && !namedVar.TypeAndValue.Pointer
                 && compiler->IsCxxRecord(namedVar.TypeAndValue.TypeName))
                    ? namedVar.Storage : nullptr;
            // A scalar lvalue keeps its slot for a `const T&` constructor parameter: `(R)b` binds
            // `b` like `(R)(b)` does. materialize() drops the storage of an unloaded identifier.
            llvm::Value* scalarCastStorage =
                (namedVar.Primary == nullptr && !namedVar.TypeAndValue.Pointer
                 && !namedVar.TypeAndValue.IsArrayView)
                    ? namedVar.Storage : nullptr;
            materialize(namedVar);

            // A pointer never casts to a float or a simd vector (clang refuses the vector form too).
            if (namedVar.Primary != nullptr && namedVar.Primary->getType()->isPointerTy()
                && (type->isFloatingPointTy() || type->isVectorTy()))
            {
                std::string target = SpellType(*compiler, destTypeName);
                if (target.empty()) target = destTypeName.TypeName;
                LogErrorContext(ctx, std::format(
                    "pointer cannot be cast to type '{}'", target));
                return {};
            }

            // An explicit cast to a foreign C++ value can use its converting constructor,
            // matching C++ direct-initialization such as `static_cast<std::string>(literal)`.
            const bool charPointerSource = namedVar.TypeAndValue.TypeName == "char"
                && namedVar.TypeAndValue.Pointer && !namedVar.TypeAndValue.ElemPointer
                && !namedVar.TypeAndValue.IsArrayView;
            const bool fixedArraySource = LLVMBackend::IsCxxFixedArrayArgument(namedVar);
            const bool cxxClassSource = compiler->IsCxxRecord(namedVar.TypeAndValue.TypeName);
            // A scalar operand direct-initializes a C++ class: `(R)(7)` is `R(7)`. Only a class
            // with constructors qualifies.
            const bool scalarSource = namedVar.Primary != nullptr && !namedVar.TypeAndValue.Pointer
                && !namedVar.TypeAndValue.IsArrayView
                && (namedVar.Primary->getType()->isIntegerTy()
                    || namedVar.Primary->getType()->isFloatingPointTy());
            // A pointer or function value direct-initializes a C++ class the same way: `(R)(p)` is `R(p)`.
            const bool pointerSource = !charPointerSource && !namedVar.TypeAndValue.IsAlias
                && !namedVar.TypeAndValue.IsRvalueRef && !namedVar.TypeAndValue.IsArrayView
                && !namedVar.TypeAndValue.ElemPointer
                && (namedVar.TypeAndValue.Pointer || namedVar.TypeAndValue.IsFunctionPointer
                    || llvm::isa_and_nonnull<llvm::Function>(namedVar.Primary));
            if ((charPointerSource || fixedArraySource || cxxClassSource || scalarSource || pointerSource)
                && !destTypeName.Pointer && !destTypeName.ElemPointer
                && !destTypeName.IsArrayView
                && (compiler->IsForeignNontrivialCxxClass(destTypeName.TypeName)
                    || ((scalarSource || pointerSource)
                        && compiler->IsForeignCxxClassWithConstructors(destTypeName.TypeName)))
                && namedVar.TypeAndValue.TypeName != destTypeName.TypeName)
            {
                auto converted = namedVar;
                converted.TypeAndValue.VariableName.clear();
                if (scalarSource && scalarCastStorage != nullptr) converted.Storage = scalarCastStorage;
                std::string refusal;
                llvm::Value* intoSlot = nullptr;
                std::string pendingType;
                if (ctx == pendingCxxCastInitContext_
                    && compiler->pendingCxxSretDest_ != nullptr
                    && compiler->pendingCxxSretTypeName_ == destTypeName.TypeName)
                {
                    intoSlot = compiler->pendingCxxSretDest_;
                    pendingType = compiler->pendingCxxSretTypeName_;
                    compiler->pendingCxxSretDest_ = nullptr;
                    compiler->pendingCxxSretTypeName_.clear();
                }
                switch (compiler->ConvertThroughCxxConvertingCtor(
                            converted, destTypeName.TypeName, intoSlot, refusal,
                            /*directInit*/ true))
                {
                case LLVMBackend::CxxConvertingCtorResult::Converted:
                    converted.TypeAndValue = destTypeName;
                    converted.IsStringLiteral = false;
                    return converted;
                case LLVMBackend::CxxConvertingCtorResult::Refused:
                    LogErrorContext(ctx, std::format(
                        "cannot cast to C++ class '{}'; {}",
                        compiler->DisplayCxxClassName(destTypeName.TypeName), refusal));
                    namedVar.TypeAndValue = destTypeName;
                    return namedVar;
                default:
                    if (intoSlot != nullptr)
                    {
                        compiler->pendingCxxSretDest_ = intoSlot;
                        compiler->pendingCxxSretTypeName_ = pendingType;
                    }
                    break;
                }
            }

            // User-defined conversion operators are explicit-only. Built-in casts keep their
            // meaning, and only an otherwise invalid cast from a struct value may reach this
            // lookup. The declaration must be a free function with one by-value source parameter
            // and a return type matching the target; malformed declarations do not participate.
            auto isBuiltinCast = [](llvm::Type* source, llvm::Type* target) {
                if (source == nullptr || target == nullptr) return false;
                if (source == target) return true;
                if (target->isIntegerTy(1))
                    return source->isIntegerTy() || source->isFloatingPointTy()
                        || source->isPointerTy();
                if (source->isIntegerTy() && target->isIntegerTy()) return true;
                if (source->isFloatingPointTy() && target->isFloatingPointTy()) return true;
                if (source->isIntegerTy() && target->isFloatingPointTy()) return true;
                if (source->isFloatingPointTy() && target->isIntegerTy()) return true;
                if (source->isPointerTy() && target->isIntegerTy()) return true;
                if (source->isIntegerTy() && target->isPointerTy()) return true;
                if (source->isPointerTy() && target->isPointerTy()) return true;
                return false;
            };
            auto* sourceLLVMType = namedVar.Primary != nullptr ? namedVar.Primary->getType() : nullptr;
            auto* sourceStruct = llvm::dyn_cast_or_null<llvm::StructType>(sourceLLVMType);
            std::string sourceTypeName = namedVar.TypeAndValue.TypeName;
            if (sourceTypeName.empty() && sourceStruct != nullptr && sourceStruct->hasName())
                sourceTypeName = sourceStruct->getName().str();
            if (sourceStruct != nullptr && destTypeName.IsPrimitive()
                && !compiler->IsCoreUniqueType(sourceTypeName))
            {
                auto converted = namedVar;
                converted.TypeAndValue.TypeName = sourceTypeName;
                if (cxxCastStorage != nullptr) converted.Storage = cxxCastStorage;
                if (compiler->ApplyCxxConversionOperator(converted, destTypeName, true))
                    return converted;
            }
            if (!isSpecialConversion && !destTypeName.IsArrayView
                && sourceLLVMType != nullptr && sourceLLVMType->isStructTy()
                && !compiler->IsCoreUniqueType(namedVar.TypeAndValue.TypeName)
                && !isBuiltinCast(sourceLLVMType, type))
            {
                auto hasMatchingConversion = [&]() {
                    if (sourceStruct == nullptr || !sourceStruct->hasName()) return false;
                    auto fnIt = compiler->functionTable.find(opName);
                    if (fnIt == compiler->functionTable.end()) return false;
                    for (const auto& candidate : fnIt->second)
                    {
                        if (candidate.Parameters.size() != 1) continue;
                        const auto& sourceParam = candidate.Parameters[0];
                        // A CFlat conversion is a free function taking its source BY VALUE. A
                        // C++ member conversion operator is the same conversion spelled with a
                        // 'this' receiver, so for a registered C++ record the pointer shape is
                        // accepted too - the overload call path does the receiver adjustment
                        // (the same relaxation round 8 made for 'operator bool').
                        if ((sourceParam.Pointer && !compiler->IsCxxRecord(sourceTypeName))
                            || sourceParam.ElemPointer
                            || sourceParam.TypeName != sourceTypeName)
                            continue;
                        const auto& candidateReturn = candidate.ReturnType;
                        if (candidateReturn.TypeName != destTypeName.TypeName
                            || candidateReturn.Pointer != destTypeName.Pointer
                            || candidateReturn.ElemPointer != destTypeName.ElemPointer
                            || candidateReturn.IsArrayView != destTypeName.IsArrayView)
                            continue;
                        return true;
                    }
                    return false;
                };

                if (!destTypeName.IsPrimitive() && hasMatchingConversion())
                {
                    auto argNV = namedVar;
                    argNV.TypeAndValue.VariableName.clear();
                    if (cxxCastStorage != nullptr) argNV.Storage = cxxCastStorage;
                    auto result = compiler->CreateOverloadedFunctionCall(opName, { argNV });
                    namedVar.Primary = result;
                    namedVar.Storage = nullptr;
                    namedVar.TypeAndValue = destTypeName;
                    return namedVar;
                }

                // A C++ conversion operator that exists but could not be bound (private, deleted,
                // no reachable definition, ...) is recorded per member under this same name, so
                // the cast reports WHY instead of claiming the conversion was never declared.
                if (compiler->IsCxxRecord(sourceTypeName)
                    && compiler->RejectInaccessibleCxxMember(sourceTypeName, opName))
                {
                    namedVar.TypeAndValue = destTypeName;
                    return namedVar;
                }

                // RejectInaccessibleCxxMember may have requested a specialization and registered
                // the conversion. Re-run the ordinary function-table lookup after that bind.
                if (!destTypeName.IsPrimitive() && hasMatchingConversion())
                {
                    auto argNV = namedVar;
                    argNV.TypeAndValue.VariableName.clear();
                    if (cxxCastStorage != nullptr) argNV.Storage = cxxCastStorage;
                    auto result = compiler->CreateOverloadedFunctionCall(opName, { argNV });
                    namedVar.Primary = result;
                    namedVar.Storage = nullptr;
                    namedVar.TypeAndValue = destTypeName;
                    return namedVar;
                }

                // No 'operator <dest>' - C++ still allows ONE standard conversion on top of a
                // user-defined one, so '(int)v[0]' reaches 'operator bool' and widens. A cast is
                // an explicit context, so an 'explicit' conversion operator participates here.
                {
                    auto converted = namedVar;
                    converted.TypeAndValue.TypeName = sourceTypeName;
                    if (cxxCastStorage != nullptr) converted.Storage = cxxCastStorage;
                    if (compiler->ApplyCxxConversionOperator(converted, destTypeName, true))
                        return converted;
                }

                LLVMBackend::TypeAndValue sourceType;
                sourceType.TypeName = sourceTypeName;
                std::string sourceDisplay = SpellType(*compiler, sourceType);
                if (sourceDisplay.empty()) sourceDisplay = sourceTypeName;
                std::string targetDisplay = SpellType(*compiler, destTypeName);
                if (targetDisplay.empty()) targetDisplay = destTypeName.TypeName;
                LogErrorContext(ctx, std::format(
                    "cannot cast '{}' to '{}'; no '{}' defined for '{}'",
                    sourceDisplay, targetDisplay, opName, sourceDisplay));
            }

            // A fat `Lambda<...>` closure is a 16-byte {code, env} struct; it must be
            // CONSTRUCTED from a lambda/named function, never reinterpreted from a scalar.
            // Casting a raw pointer (or any non-closure value) to it would emit an illegal
            // 8B->16B bitcast that fails module verification. The thin `function<...>` cast
            // is the supported path for a raw code address (e.g. a COM vtable slot).
            if (destTypeName.IsFunctionPointer && !destTypeName.IsThinFnPtr()
                && namedVar.Primary != nullptr && !namedVar.Primary->getType()->isStructTy())
            {
                LogErrorContext(ctx, "cannot cast to a 'Lambda<...>' closure; a fat closure "
                    "must be built from a lambda or named function, not reinterpreted from a "
                    "pointer. Cast to the thin 'function<...>' to call a raw code address.");
                namedVar.TypeAndValue = destTypeName;
                return namedVar;
            }

            // A core unique value used in a type-changing cast is observed through its raw
            // pointee, preserving the old `unique T*` cast behavior without copying the owner.
            if (!destTypeName.IsArrayView && namedVar.Primary != nullptr
                && compiler->IsCoreUniqueType(namedVar.TypeAndValue.TypeName)
                && !namedVar.TypeAndValue.Pointer
                && !compiler->IsCoreUniqueType(destTypeName.TypeName))
            {
                LLVMBackend::TypeAndValue rawType;
                rawType.TypeName = MangledGenericArgument(
                    *compiler, namedVar.TypeAndValue.TypeName);
                rawType.Pointer = true;
                namedVar.Primary = compiler->CreateCoreUniqueRawPointerCall(namedVar, rawType);
                namedVar.Storage = nullptr;
                namedVar.TypeAndValue = rawType;
            }

            // Explicit `(T[])p` escape: re-establish the noalias array-view from a raw
            // pointer. `T*` and `T[]` share an identical representation, so this is a pure
            // reinterpret (no bit manipulation) - the sanctioned inverse of the implicit
            // `T[] -> T*` decay, by which the programmer asserts the pointer spans a whole,
            // distinct allocation. The result carries IsArrayView, so it passes the one-way
            // bind gate (RejectRawPointerToArrayView) at every binding site.
            if (destTypeName.IsArrayView)
            {
                if (!namedVar.TypeAndValue.Pointer)
                    LogErrorContext(ctx, "'(T[])' cast requires a pointer source 'T*'; "
                        "cannot cast a non-pointer to an array-view");
                namedVar.TypeAndValue = destTypeName;
                return namedVar;
            }

            // A REDUNDANT `(T)x` names x's own type, so it is a spelling, not a conversion: hand
            // the result x's own storage and provenance back. Captured before materialize() and
            // CreateCast() drop them. A type-CHANGING cast keeps today's behaviour untouched.
            bool redundantCast = redundantCastOfSource;
            llvm::Value* redundantStorage = redundantCastStorage;

            bool srcIsSigned = namedVar.TypeAndValue.IsUnsignedInteger() == -1;
            // A cast off a temp's `unique` field is the "I mean this" spelling, and it drops the
            // ownership facts with the type; carry them to the RESULT so persist sites still see it.
            bool castOfTempUniqueField = IsOwningTempUniqueFieldEscape(namedVar);
            // A CANDIDATE launder (callee still below its call site) has to survive the cast too,
            // or the deferred destination gate loses the value the eager one keeps.
            const LLVMBackend::PendingLaunderTempUniqueField* castPending = castOfTempUniqueField
                ? nullptr : compiler->FindPendingLaunderTempUniqueField(namedVar.Primary);
            std::vector<std::pair<const llvm::Function*, unsigned>> castConds;
            std::string castCallee, castAccess;
            if (castPending != nullptr)
            { castConds = castPending->Conds; castCallee = castPending->CalleeName;
              castAccess = castPending->Access; }
            namedVar.Primary = compiler->CreateCast(namedVar.Primary, type, srcIsSigned,
                destTypeName.IsUnsignedInteger() != -1);
            namedVar.TypeAndValue = destTypeName;
            if (castOfTempUniqueField)
                compiler->RegisterOwningTempUniqueField(namedVar.Primary);
            else if (!castConds.empty())
                compiler->RegisterPendingLaunderTempUniqueField(namedVar.Primary, castConds,
                                                                castCallee, castAccess);
            // A ptr->ptr cast is a no-op under opaque pointers, so the result IS the ledgered
            // code value; launder it or the explicit cast the rejection advises is itself refused.
            if (compiler->ParameterStoresData(destTypeName))
                compiler->RegisterCodeValueDataCast(namedVar.Primary);
            // The mirror launder: a cast TO a code type is the escape hatch the closure-widen
            // gate advises, and the no-op cast result is still the ledgered data value.
            else if (destTypeName.IsFunctionPointer)
                compiler->RegisterDataValueCodeCast(namedVar.Primary);
            if (redundantCast)
            {
                namedVar.Storage = redundantStorage;
                // Keep the OPERAND's canonical type name: the cast's spelling can be
                // namespace-relative, and a later consume arm compares it with a declared type's.
                namedVar.TypeAndValue.TypeName = redundantCastSource.TypeAndValue.TypeName;
                namedVar.TypeAndValue.VariableName = redundantCastSource.TypeAndValue.VariableName;
                namedVar.TypeAndValue.ParentVariableName = redundantCastSource.TypeAndValue.ParentVariableName;
                AdoptWrapperProvenance(namedVar, redundantCastSource);
            }
            if (!destTypeName.Pointer && namedVar.Primary != nullptr
                && (namedVar.Primary->getType()->isIntegerTy()
                    || namedVar.Primary->getType()->isFloatingPointTy()))
            {
                // A scalar cast is a prvalue of the cast type, redundant or not: it keeps
                // neither the operand's slot and variable identity (a C++ `T&` must not bind,
                // nor write, the operand) nor the operand literal's identity.
                namedVar.Storage = nullptr;
                namedVar.CallerName.clear();
                namedVar.LiteralIdentity.clear();
                namedVar.CxxLvalueKind = 0;
            }
            return namedVar;
        }

        LogErrorContext(ctx, "Cast expression has no recognized form.");
        return {};
    }

LLVMBackend::TypeAndValue MainListener::ParseTypeName(
    CFlatParser::TypeNameContext* ctx, bool allowSizedArray,
    antlr4::ParserRuleContext* useSite) {
        if (ctx == nullptr) return {};
        auto* specCtx = ctx->specifierQualifierList();
        return ParseTypeNameParts(specCtx ? specCtx->typeSpecifier()
                                          : std::vector<CFlatParser::TypeSpecifierContext*>{},
            specCtx ? specCtx->typeQualifier() : nullptr, ctx->abstractDeclarator(),
            allowSizedArray, ctx, useSite);
    }

LLVMBackend::TypeAndValue MainListener::ParseCastTypeName(
    CFlatParser::CastTypeNameContext* ctx, bool allowSizedArray,
    antlr4::ParserRuleContext* useSite) {
        if (ctx == nullptr) return {};
        return ParseTypeNameParts(ctx->typeSpecifier(), ctx->typeQualifier(),
            ctx->abstractDeclarator(), allowSizedArray, ctx, useSite);
    }

LLVMBackend::TypeAndValue MainListener::ParseTypeNameParts(
    const std::vector<CFlatParser::TypeSpecifierContext*>& typeSpecs,
    CFlatParser::TypeQualifierContext* typeQualifier,
    CFlatParser::AbstractDeclaratorContext* abstractDecl, bool allowSizedArray,
    antlr4::ParserRuleContext* typeContext, antlr4::ParserRuleContext* useSite) {

        LLVMBackend::TypeAndValue typeValue;
        if (!typeSpecs.empty())
        {
                auto* typeSpec = typeSpecs[0];
                std::string baseName;
                auto* genParams = GenericSpecOf(typeSpec, baseName);
                if (auto* nestedType = typeSpec->typeName())
                {
                    typeValue = ParseTypeName(nestedType, allowSizedArray,
                        useSite != nullptr ? useSite : typeContext);
                    typeValue.ApplyWholeWordConst();
                }
                else if (auto* tupleSpec = typeSpec->tupleTypeSpecifier())
                    typeValue.TypeName = ParseTupleTypeSpecifier(tupleSpec);
                else if (auto* fpSpec = typeSpec->functionPointerSpecifier())
                {
                    // Cast target `(function<R(Args)>)addr` / `(Lambda<...>)x`: build the
                    // resolved closure type so a raw code address can be reinterpreted as a
                    // thin C function pointer. Mirrors the declaration path (BuildFuncPtrAliasType).
                    typeValue = BuildFuncPtrAliasType(fpSpec);
                }
                else if (genParams != nullptr)
                {
                    // Generic cast: (channel<int>*) -> mangle to channel__int
                    CanonicalTypeSpecifier canonical;
                    const TypeNameArgumentResolver resolveArg = [this](auto* entry) {
                        return ResolveTypeArgEntry(entry);
                    };
                    const std::vector<CFlatParser::TypeSpecifierContext*> genericSpecs{ typeSpec };
                    if (!ResolveCanonicalTypeSpecifier(compilerLLVM, genericSpecs,
                            &activeTypeSubstitutions, resolveArg, {}, true, canonical))
                        return typeValue;
                    baseName = canonical.GenericBase;
                    const auto& typeArgs = canonical.GenericArgs;
                    typeValue.TypeName = canonical.TypeName;
                    typeValue.ConstMask |= canonical.ConstMask;
                    const int aliasPointerDepth = canonical.PointerDepth;
                    if (aliasPointerDepth > 0)
                    {
                        typeValue.Pointer = true;
                        typeValue.ElemPointer = aliasPointerDepth >= 2;
                        typeValue.PointerDepth = aliasPointerDepth > 2 ? 0 : aliasPointerDepth;
                    }
                    std::string cxxError;
                    const bool cxxType = compilerLLVM->TryRequestCxxType(
                        baseName, typeArgs, typeValue.TypeName, cxxError);
                    if (!cxxType && !cxxError.empty()) LogCxxErrorContext(genParams, cxxError);
                    if (compilerLLVM->AnyGenericTypeTemplateNamed(baseName))
                        QueueAndProcessLocalEnumInstantiation(
                            genParams, baseName, typeArgs, typeValue.TypeName,
                            useSite != nullptr ? useSite : genParams);
                    // A deferred winmd generic interface named directly in a cast (no `using` or
                    // forward-ref scan reached it) is instantiated on demand here. Idempotent/cached,
                    // and a no-op false for CFlat generics (already queued by the scanner).
                    if (compilerLLVM->GetDataStructure(typeValue.TypeName).StructType == nullptr)
                        compilerLLVM->InstantiateWinrtGenericInterface(baseName, typeArgs, typeValue.TypeName);
                }
                else if (auto* memberType = typeSpec->memberTypeSpecifier())
                {
                    CanonicalTypeSpecifier canonical;
                    const TypeNameMemberResolver resolveMember = [this](auto* member) {
                        return ResolveMemberTypeSpecifier(member);
                    };
                    if (!ResolveCanonicalTypeSpecifier(compilerLLVM, typeSpecs,
                            &activeTypeSubstitutions, {}, resolveMember, true, canonical))
                        return typeValue;
                    typeValue.TypeName = canonical.TypeName;
                    typeValue.ConstMask |= canonical.ConstMask;
                    const int aliasPointerDepth = canonical.PointerDepth;
                    if (aliasPointerDepth > 0)
                    {
                        typeValue.Pointer = true;
                        typeValue.ElemPointer = aliasPointerDepth >= 2;
                        typeValue.PointerDepth = aliasPointerDepth > 2 ? 0 : aliasPointerDepth;
                    }
                    const size_t scope = typeValue.TypeName.rfind('.');
                    if (!compilerLLVM->IsKnownTypeName(typeValue.TypeName)
                        && scope != std::string::npos
                        && compilerLLVM->IsCxxNamespacePath(typeValue.TypeName.substr(0, scope)))
                    {
                        std::string cxxError;
                        if (!compilerLLVM->TryRequestCxxType(
                                typeValue.TypeName, {}, typeValue.TypeName, cxxError)
                            && cxxError.find("could not be instantiated:") != std::string::npos)
                            LogCxxErrorContext(memberType, cxxError);
                        typeValue.TypeName = compilerLLVM->ResolveTypeAlias(typeValue.TypeName);
                    }
                }
                else
                {
                    PrimitiveTypeError canonicalError;
                    CanonicalTypeSpecifier canonical;
                    const TypeNameArgumentResolver resolveArg = [this](auto* entry) {
                        return ResolveTypeArgEntry(entry);
                    };
                    const TypeNameMemberResolver resolveMember = [this](auto* member) {
                        return ResolveMemberTypeSpecifier(member);
                    };
                    if (!ResolveCanonicalTypeSpecifier(compilerLLVM, typeSpecs,
                            &activeTypeSubstitutions, resolveArg, resolveMember, true,
                            canonical, &canonicalError))
                        LogErrorContext(typeContext,
                            LocalizePrimitiveTypeError(compilerLLVM, canonicalError));
                    typeValue.TypeName = canonical.TypeName;
                    typeValue.ConstMask |= canonical.ConstMask;
                    if (IsUnparameterizedNamedTypeSpecifier(typeSpec)
                        && activeTypeSubstitutions.count(
                            typeSpec->getText()) == 0)
                    {
                        const auto aggregateScopes = EnclosingAggregateScopesForType(typeSpec);
                        const std::string contextual = ResolveContextualTypeName(compilerLLVM,
                            typeSpec->getText(), typeValue.TypeName, typeSpec, aggregateScopes,
                            NamespaceNameFromContext(compilerLLVM, typeSpec));
                        if (!contextual.empty()) typeValue.TypeName = contextual;
                    }
                    if (canonical.PointerDepth > 0)
                    {
                        typeValue.Pointer = true;
                        typeValue.ElemPointer = canonical.PointerDepth >= 2;
                        typeValue.PointerDepth = canonical.PointerDepth > 2
                            ? 0 : canonical.PointerDepth;
                    }
                    if (typeValue.TypeName == "longdouble" && !HasPrimitiveTypeError(canonicalError)
                        && !compilerLLVM->IsCInteropLongDoubleSupported())
                        LogErrorContext(typeContext, compilerLLVM->CInteropLongDoubleRefusal());
                    // A C function-pointer typedef is a function-type alias, not a type alias:
                    // expand it to its signature as the declaration path does (MainListener_Declarations).
                    if (!compilerLLVM->NativeTypeShadowsFunctionAlias(typeValue.TypeName))
                        if (const auto* fit = compilerLLVM->FindOrRequestFunctionTypeAlias(typeValue.TypeName);
                            fit != nullptr)
                        {
                            typeValue = *fit;
                            typeValue.Pointer = false;
                            typeValue.PointerDepth = 0;
                            typeValue.ElemPointer = false;
                        }
                }
        }
        else
        {
            // undefined type.
            LogErrorContext(typeContext, "Type name has no specifier-qualifier list.");
            return typeValue;
        }

        if (!typeValue.Pointer && !compilerLLVM->IsKnownTypeName(typeValue.TypeName))
        {
            const size_t scope = typeValue.TypeName.rfind('.');
            if (scope != std::string::npos
                && compilerLLVM->IsCxxNamespacePath(typeValue.TypeName.substr(0, scope)))
            {
                std::string cxxError;
                if (!compilerLLVM->TryRequestCxxType(
                        typeValue.TypeName, {}, typeValue.TypeName, cxxError)
                    && cxxError.find("could not be instantiated:") != std::string::npos)
                    LogCxxErrorContext(typeContext, cxxError);
                typeValue.TypeName = compilerLLVM->ResolveTypeAlias(typeValue.TypeName);
            }
        }

        if (abstractDecl && abstractDecl->pointer())
        {
            const std::string spelledBaseType = SpelledTypeNameBase(typeSpecs);
            for (auto* qualifier : ConstQualifiersAfterPointerStar(abstractDecl->pointer()))
                LogErrorContext(qualifier, ConstAfterPointerDiagnostic(spelledBaseType));
            typeValue.Pointer = true;
            const int declaredDepth = PointerDepthOf(abstractDecl->pointer());
            const int totalDepth = typeValue.PointerDepth + declaredDepth;
            typeValue.ElemPointer = typeValue.ElemPointer || totalDepth >= 2;
            typeValue.PointerDepth = totalDepth > PointerDepthCap ? 0 : totalDepth;
        }

        // Cast targets keep enum identity for lookup, but integer operations also need the
        // backing type so signedness survives casts of unsigned enum constants.
        if (!typeValue.Pointer && typeValue.EnumBacking.empty())
        {
            if (std::string enumKey = compilerLLVM->ResolveEnumTypeName(typeValue.TypeName);
                !enumKey.empty())
            {
                typeValue.TypeName = enumKey;
                typeValue.EnumBacking = compilerLLVM->GetEnumBackingType(enumKey);
                typeValue.IsScopedEnum = compilerLLVM->IsScopedEnumTypeName(enumKey);
            }
        }

        // `(T[])` cast target: the noalias array-view. Empty brackets only - a sized
        // `(T[N])` is not a meaningful cast. Mirrors the declaration path: an array-view
        // is a pointer repr carrying the noalias contract.
        if (abstractDecl)
        {
            // `(T[]*)` / `(T[N]*)`: a trailing '*' after the brackets is never a valid type.
            // Redirect to the array-view cast (return-early avoids a duplicate sized-array error).
            if (ArrayPtrOf(abstractDecl))
            {
                LogErrorContext(typeContext, "pointer to array-view '(T[]*)' is not a valid type; cast to "
                    "the array-view '(T[])' and take its address separately if you need a pointer");
                return typeValue;
            }
            if (auto* dimSpec = ArrayDimsOf(abstractDecl))
            {
                if (DimSpecIsUnsizedMultiDim(dimSpec))
                    LogErrorContext(typeContext, UnsizedMultiDimMessage(SpellType(*Compiler(), typeValue)));
                if (!allowSizedArray && !dimSpec->assignmentExpression().empty())
                    LogErrorContext(typeContext, "a sized array '(T[N])' is not a valid cast target; "
                        "use '(T[])' for the noalias array-view");
                if (dimSpec->assignmentExpression().empty())
                {
                    // Mirror the declaration path: a star already on the specifier (written, or
                    // folded in from a substitution/alias) is the ELEMENT star of `(T*[])`.
                    bool elementPointer = typeValue.Pointer;
                    typeValue.IsArrayView = true;
                    typeValue.Pointer = true;
                    typeValue.ElemPointer = elementPointer || typeValue.ElemPointer;
                }
            }
        }

        if (typeQualifier != nullptr)
            typeValue.ApplyWholeWordConst();

        return typeValue;
    }

LLVMBackend::NamedVariable MainListener::SizeOrAlignOfType(const LLVMBackend::TypeAndValue& typeValue,
                                                           llvm::Type* llvmType, bool isSizeof) {
        auto* compiler = Compiler();
        compiler->TouchWaitingLayout(llvmType);
        if (llvmType == nullptr || !llvmType->isSized())
        {
            compiler->LogError(std::format("sizeof/alignof: type '{}' is incomplete",
                typeValue.TypeName.empty() ? "<unknown>" : typeValue.TypeName));
            return {};
        }
        uint64_t effAlign = typeValue.Pointer
            ? 0
            : compiler->GetEffectiveAlignmentForType(typeValue.TypeName, llvmType);
        llvm::Value* result;
        if (isSizeof)
            result = effAlign > 1
                ? llvm::ConstantInt::get(llvm::Type::getInt64Ty(*compiler->context),
                                         compiler->GetEffectiveAllocSize(llvmType, effAlign))
                : compiler->GetTypeSizeBytes(llvmType);
        else
            result = effAlign > 1
                ? llvm::ConstantInt::get(llvm::Type::getInt64Ty(*compiler->context), effAlign)
                : compiler->GetTypeAlignBytes(llvmType);
        LLVMBackend::NamedVariable namedVar;
        namedVar.Primary = result;
        namedVar.TypeAndValue.TypeName = "i64";
        return namedVar;
    }

// `sizeof` + a postfix operand that spells a TYPE (`sizeof(Point)`, `sizeof(int[4])`,
// `sizeof(buf)` naming a variable): its size, or an empty NamedVariable when the text is not a type.
LLVMBackend::NamedVariable MainListener::TrySizeofPostfixAsType(CFlatParser::UnaryExpressionContext* ctx,
                                                                CFlatParser::PostfixExpressionContext* postFixCtx) {
        auto* compiler = Compiler(ctx);
        const bool prefixSizeof = true;
        // The parser matched sizeof as a prefix on a postfixExpression.
        // Extract the type name from the text (e.g., "sizeof(Point)" -> "Point")
        std::string postfixText = postFixCtx->getText();

        // Remove outer parentheses if present
        if (!postfixText.empty() && postfixText[0] == '(' && postfixText.back() == ')')
        {
            postfixText = postfixText.substr(1, postfixText.length() - 2);
        }

        // Does this look like a type name? '(' ')' ',' count as type text only INSIDE
        // balanced generic brackets (Pair<int,float>, Box<function<int(int)>>).
        bool likelyType = !postfixText.empty() && (std::isalpha(postfixText[0]) || postfixText[0] == '_');
        std::vector<uint64_t> rawArrayDims;
        auto firstBracket = postfixText.find('[');
        if (firstBracket != std::string::npos)
        {
            std::string baseText = postfixText.substr(0, firstBracket);
            size_t pos = firstBracket;
            while (pos < postfixText.size())
            {
                if (postfixText[pos] != '[') { likelyType = false; break; }
                size_t close = postfixText.find(']', pos + 1);
                if (close == std::string::npos || close == pos + 1) { likelyType = false; break; }
                std::string dimText = postfixText.substr(pos + 1, close - pos - 1);
                size_t consumed = 0;
                unsigned long long dim = 0;
                try { dim = std::stoull(dimText, &consumed); }
                catch (...) { consumed = 0; }
                if (consumed != dimText.size() || dim == 0) { likelyType = false; break; }
                rawArrayDims.push_back(static_cast<uint64_t>(dim));
                pos = close + 1;
            }
            if (pos != postfixText.size() || rawArrayDims.empty()) likelyType = false;
            else postfixText = baseText;
        }
        int angleDepth = 0;
        bool usedBracketPunct = false;
        for (char c : postfixText)
        {
            if (c == '<') { angleDepth++; continue; }
            if (c == '>') { angleDepth--; continue; }
            if (std::isalnum(c) || c == '_' || c == '.' || c == '*')
                continue;
            if (angleDepth > 0 && (c == '(' || c == ')' || c == ','))
            {
                usedBracketPunct = true;
                continue;
            }
            likelyType = false;
            break;
        }
        // Only the newly-admitted punctuation demands balanced brackets; an unbalanced
        // spelling like 'a<b' keeps its existing (bracket-free) classification.
        if (usedBracketPunct && angleDepth != 0)
            likelyType = false;
        bool knownGenericType = false;
        if (likelyType && postfixText.find('<') != std::string::npos)
        {
            std::string genericBase = postfixText.substr(0, postfixText.find('<'));
            knownGenericType = compiler->AnyGenericTypeTemplateNamed(genericBase)
                || genericBase == "function" || genericBase == "Lambda";
            // A qualified foreign C++ template is not in the CFlat template registries.
            // Request it only when the parsed postfix has generic arguments but no call/index
            // suffix; TryRequestCxxType reports failure through its out parameter, not LogError.
            if (!knownGenericType && postFixCtx->argumentExpressionList().empty()
                && postFixCtx->expression().empty())
            {
                const auto open = postfixText.find('<');
                const auto close = postfixText.rfind('>');
                if (close != std::string::npos && close > open + 1
                    && close + 1 == postfixText.size())
                {
                    std::vector<std::string> typeArgs;
                    std::string current;
                    int nestedAngles = 0;
                    int nestedParens = 0;
                    bool validArgs = true;
                    for (size_t i = open + 1; i < close; i++)
                    {
                        const char c = postfixText[i];
                        if (c == '<') nestedAngles++;
                        else if (c == '>') nestedAngles--;
                        else if (c == '(') nestedParens++;
                        else if (c == ')') nestedParens--;
                        if (nestedAngles < 0 || nestedParens < 0)
                        {
                            validArgs = false;
                            break;
                        }
                        if (c == ',' && nestedAngles == 0 && nestedParens == 0)
                        {
                            if (current.empty()) { validArgs = false; break; }
                            typeArgs.push_back(current);
                            current.clear();
                        }
                        else current.push_back(c);
                    }
                    if (current.empty() || nestedAngles != 0 || nestedParens != 0) validArgs = false;
                    if (validArgs) typeArgs.push_back(current);
                    else typeArgs.clear();
                    if (!typeArgs.empty())
                    {
                    std::string resolvedBase = genericBase;
                    compiler->ResolveGenericAliasSpelling(resolvedBase, typeArgs);
                    compiler->CompleteCxxTemplateTypeArgs(resolvedBase, typeArgs);
                    std::string candidate = MangledGenericName(resolvedBase, typeArgs);
                    std::string cxxError;
                    knownGenericType = compiler->TryRequestCxxType(
                        genericBase, typeArgs, candidate, cxxError);
                    if (knownGenericType) postfixText = candidate;
                    }
                }
            }
            if (!knownGenericType) likelyType = false;
        }
        // `Spec<args>.name` is a type exactly when the lookup says the member is one.
        if (std::string memberType = TryPostfixAsMemberType(postFixCtx); !memberType.empty())
        {
            postfixText = memberType;
            likelyType = true;
            knownGenericType = false;
        }

        if (likelyType)
        {
            // Try to parse as a type
            LLVMBackend::TypeAndValue typeValue;
            typeValue.TypeName = postfixText;
            std::string rawTypeName = typeValue.TypeName;

            // Check for trailing * (pointer)
            if (!typeValue.TypeName.empty() && typeValue.TypeName.back() == '*')
            {
                typeValue.Pointer = true;
                typeValue.TypeName.pop_back();
            }

            // Apply active type-parameter substitutions (e.g. sizeof(T) inside a generic function body).
            {
                auto substIt = activeTypeSubstitutions.find(typeValue.TypeName);
                if (substIt != activeTypeSubstitutions.end())
                {
                    typeValue.TypeName = substIt->second;
                    while (!typeValue.TypeName.empty() && typeValue.TypeName.back() == '*')
                    {
                        typeValue.TypeName.pop_back();
                        typeValue.Pointer = true;
                    }
                }
            }

            if (knownGenericType && typeValue.TypeName.find('<') != std::string::npos)
            {
                antlr4::ANTLRInputStream typeInput(typeValue.TypeName);
                CFlatLexer typeLexer(&typeInput);
                antlr4::CommonTokenStream typeTokens(&typeLexer);
                CFlatParser typeParser(&typeTokens);
                typeLexer.removeErrorListeners();
                typeParser.removeErrorListeners();
                auto* parsedTypeName = typeParser.typeName();
                if (typeParser.getNumberOfSyntaxErrors() == 0 && typeTokens.LA(1) == -1)
                {
                    auto parsedType = ParseTypeName(parsedTypeName, true, ctx);
                    if (!parsedType.TypeName.empty())
                    {
                        parsedType.Pointer = parsedType.Pointer || typeValue.Pointer;
                        typeValue = std::move(parsedType);
                    }
                }
            }

            if (knownGenericType && typeValue.TypeName.find('<') != std::string::npos)
            {
                const auto open = typeValue.TypeName.find('<');
                const auto close = typeValue.TypeName.rfind('>');
                if (close != std::string::npos && close > open + 1
                    && close + 1 == typeValue.TypeName.size())
                {
                    std::string base = typeValue.TypeName.substr(0, open);
                    std::vector<std::string> args;
                    std::string current;
                    int angleDepth = 0;
                    int parenDepth = 0;
                    bool validArgs = true;
                    for (size_t i = open + 1; i < close; i++)
                    {
                        const char c = typeValue.TypeName[i];
                        if (c == '<') angleDepth++;
                        else if (c == '>') angleDepth--;
                        else if (c == '(') parenDepth++;
                        else if (c == ')') parenDepth--;
                        if (angleDepth < 0 || parenDepth < 0)
                        {
                            validArgs = false;
                            break;
                        }
                        if (c == ',' && angleDepth == 0 && parenDepth == 0)
                        {
                            if (current.empty()) { validArgs = false; break; }
                            args.push_back(current);
                            current.clear();
                        }
                        else current.push_back(c);
                    }
                    if (current.empty() || angleDepth != 0 || parenDepth != 0) validArgs = false;
                    if (validArgs && base != "function" && base != "Lambda")
                    {
                        args.push_back(current);
                        compiler->ResolveGenericAliasSpelling(base, args);
                        compiler->CompleteCxxTemplateTypeArgs(base, args);
                        typeValue.TypeName = MangledGenericName(base, args);
                    }
                }
            }

            // sizeof(T) where T is a pack param returns the element count
            if (prefixSizeof)
            {
                auto packIt = activePackSubstitutions.find(rawTypeName);
                if (packIt != activePackSubstitutions.end())
                {
                    LLVMBackend::NamedVariable namedVar;
                    namedVar.Primary = llvm::ConstantInt::get(
                        llvm::Type::getInt32Ty(*compiler->context),
                        (int)packIt->second.size());
                    namedVar.TypeAndValue.TypeName = "int";
                    return namedVar;
                }
            }

            // sizeof(var) / alignof(var): a bare identifier that names a visible
            // variable measures the variable's declared storage - most useful for
            // fixed arrays, where sizeof(buf) on 'char[128] buf' is 128. The variable
            // wins over a same-named type, as in C, where an ordinary identifier in
            // scope hides a type name. Adopting its TypeAndValue here lets the normal
            // type path below compute the (padded) size or alignment.
            if (!typeValue.Pointer
                && postfixText.find('.') == std::string::npos
                && postfixText.find('<') == std::string::npos
                && NamesVisibleVariable(compiler, postfixText))
            {
                typeValue = VisibleVariableType(compiler, postfixText);
                if (typeValue.TypeName == "auto" && rawArrayDims.empty())
                    return SizeOrAlignOfAutoVariable(postfixText, prefixSizeof);
            }

            // The grammar can interpret expression-shaped postfix text as a type name.
            // Only ask GetType to resolve spellings already known to the type system;
            // its unknown-type path emits a diagnostic and blocks expression fallback.
            if (!compiler->IsKnownTypeName(typeValue.TypeName)
                && !compiler->IsTypeArgTypeKey(typeValue.TypeName)
                && !knownGenericType)
            {
                if (std::string refusal = compiler->GetCxxBindingRefusal(postfixText);
                    !refusal.empty())
                    LogErrorContext(ctx, refusal);
                return {};
            }

            llvm::Type* llvmType = compiler->GetType(typeValue, nullptr, true);
            compiler->TouchWaitingLayout(llvmType);
            for (auto it = rawArrayDims.rbegin();
                 it != rawArrayDims.rend() && llvmType != nullptr; ++it)
                llvmType = llvm::ArrayType::get(llvmType, *it);
            if (llvmType && llvmType->isSized() && !llvmType->isVoidTy())
            {
                return SizeOrAlignOfType(typeValue, llvmType, prefixSizeof);
            }
            else if (llvmType != nullptr && !llvmType->isSized())
            {
                compiler->LogError(std::format("sizeof/alignof: type '{}' is incomplete",
                    typeValue.TypeName.empty() ? "<unknown>" : typeValue.TypeName));
            }
        }
        return {};
    }

// An `auto` local carries no declared TypeName; measure the storage it was actually given.
LLVMBackend::NamedVariable MainListener::SizeOrAlignOfAutoVariable(const std::string& name, bool isSizeof) {
        auto* compiler = Compiler();
        auto nv = compiler->GetScopedLocalOrArgument(name);
        llvm::Type* llvmType = nullptr;
        if (auto* slot = llvm::dyn_cast_or_null<llvm::AllocaInst>(nv.Storage))
            llvmType = slot->getAllocatedType();
        else if (nv.Primary != nullptr)
            llvmType = nv.Primary->getType();
        if (llvmType == nullptr || llvmType->isVoidTy())
        {
            compiler->LogError(std::format("sizeof/alignof: could not determine the type of '{}'", name));
            return {};
        }
        LLVMBackend::TypeAndValue typeValue;
        typeValue.Pointer = llvmType->isPointerTy();
        typeValue.TypeName = typeValue.Pointer ? "void" : LLVMTypeToTypeName(llvmType);
        return SizeOrAlignOfType(typeValue, llvmType, isSizeof);
    }

MainListener::UnevaluatedOperandType MainListener::EvaluateOperandTypeOnly(
    const std::function<LLVMBackend::NamedVariable()>& emit) {
        llvm::SaveAndRestore<int> unevaluated(unevaluatedOperandDepth_, unevaluatedOperandDepth_ + 1);
        auto* compiler = Compiler();
        std::unordered_map<llvm::Value*, IncrementWork> savedPostfix;
        savedPostfix.swap(PlusPlus);
        auto* savedRetTemp = compiler->lastCxxRetTemp_;
        auto* savedRetValue = compiler->lastCxxRetValue_;
        const bool savedBonded = compiler->lastCallIsBonded;
        // Every ledger holding a Value* / block of the region is restored or trimmed (stale = dangling
        // or recycled); SaveBuilderState parks owned-temp / join / alias ledgers, the rest are copied.
        auto detection = compiler->SnapshotDetectionLedgers();
        auto bondedValues = compiler->bondedValues_;
        auto deferredTempEscapes = compiler->deferredTempUniqueFieldEscapes_;
        auto tempUniqueFieldArgs = compiler->tempUniqueFieldArgs_;
        auto coreUniqueGetterSource = compiler->coreUniqueGetterSource_;
        auto ownSlotLeavingLoads = compiler->ownSlotLeavingLoads_;
        auto globalAssignBorrowOrigin = compiler->globalAssignBorrowOrigin_;
        auto globalAssignBorrowedAddress = compiler->globalAssignBorrowedAddress_;
        auto ownOriginSlots = compiler->ownOriginSlots_;
        auto uniqueFieldBorrowResults = compiler->uniqueFieldBorrowResults_;
        auto pendingAliasReturns = compiler->pendingAliasReturnInference;
        auto unwindPartial = compiler->unwindPartial_;
        auto unwindCallConsumedTemps = compiler->unwindCallConsumedTemps_;
        auto moveTransferConsumedTemps = compiler->moveTransferConsumedTemps_;
        const size_t nullIfaceGlobals = compiler->pendingNullIfaceGlobal_.size();
        const auto savedParenType = lastParenExprType;
        auto* const savedParenStorage = lastParenExprStorage;
        const bool savedParenFromTempField = lastParenExprFromOwningTempField;
        const bool savedParenTempParent = lastParenExprOwningTempParent;
        const auto savedParenStructName = lastParenExprOwningStructName;
        const auto savedParenFieldName = lastParenExprFieldName;
        auto state = compiler->SaveBuilderState();
        llvm::Function* host = compiler->currentFunction;
        llvm::Function* tmpFn = nullptr;
        if (host == nullptr || !compiler->IsInsertBlockLive())
        {
            // No live body (file / member scope): a private throwaway function, as if-const does.
            tmpFn = llvm::Function::Create(llvm::FunctionType::get(compiler->builder->getVoidTy(), false),
                llvm::Function::PrivateLinkage, "__unevaluated_tmp", compiler->module.get());
            host = tmpFn;
            compiler->currentFunction = tmpFn;
        }
        std::unordered_set<llvm::BasicBlock*> existing;
        for (auto& block : *host) existing.insert(&block);
        auto* scratch = llvm::BasicBlock::Create(*compiler->context, "unevaluated", host);
        compiler->builder->SetInsertPoint(scratch);
        // Flow-sensitive per-variable state (IsMoved / MovedFields, explicit-move null, address
        // escape, string-borrow and declaration blocks) lives on the scope stack: nothing moved.
        auto savedScopes = compiler->stackNamedVariable;
        auto savedLockSet = currentLockSet;
        // Dataflow events are tagged with the blocks about to be erased; drop them with the IR.
        auto logSize = [](auto& log, llvm::Function* fn) -> size_t {
            auto it = log.find(fn);
            return it == log.end() ? 0 : it->second.size();
        };
        const size_t moveEvents = logSize(compiler->moveEventLog_, host);
        // Null events, return-dangle checks and null-interface dispatches (whose Anchor is an
        // instruction of the region): the same rewind a scoped expect_error block uses.
        const auto pendingMark = compiler->MarkPendingAnalyses(host);
        // The destination's type describes the sizeof / typeof result, never its operand.
        DeclExpectedTypeScope noExpectedType(&declExpectedType, LLVMBackend::TypeAndValue{});
        CallArgumentSuspendScope noCallArgument(inCallArgument_, ternaryCallArgumentDepth_);
        const bool savedAutoTernaryInit = autoDeclTernaryInit_;
        autoDeclTernaryInit_ = false;

        auto discard = [&]() {
            auto trimLog = [&](auto& log, size_t keep) {
                auto it = log.find(host);
                if (it == log.end()) return;
                if (tmpFn != nullptr || keep == 0) log.erase(it);
                else if (it->second.size() > keep) it->second.resize(keep);
            };
            trimLog(compiler->moveEventLog_, moveEvents);
            if (tmpFn != nullptr)
            {
                compiler->nullEventLog_.erase(tmpFn);
                compiler->pendingReturnDangleChecks_.erase(tmpFn);
                compiler->pendingNullIfaceDispatch_.erase(tmpFn);
                compiler->uniqueFieldBorrowReturns_.erase(tmpFn);
                std::erase_if(compiler->paramRetainsMemo_, [&](const auto& e) { return e.first.first == tmpFn; });
                std::erase_if(compiler->paramRetainsPastCallMemo_, [&](const auto& e) { return e.first.first == tmpFn; });
            }
            else
                compiler->RewindPendingAnalyses(host, pendingMark);
            if (compiler->pendingNullIfaceGlobal_.size() > nullIfaceGlobals)
                compiler->pendingNullIfaceGlobal_.resize(nullIfaceGlobals);
            compiler->stackNamedVariable = std::move(savedScopes);
            currentLockSet = std::move(savedLockSet);
            autoDeclTernaryInit_ = savedAutoTernaryInit;
            if (tmpFn != nullptr)
            {
                for (auto& block : *tmpFn)
                    for (auto& inst : block)
                        if (!inst.use_empty()) inst.replaceAllUsesWith(llvm::PoisonValue::get(inst.getType()));
                tmpFn->eraseFromParent();
            }
            else
            {
                std::vector<llvm::BasicBlock*> created;
                for (auto& block : *host)
                    if (existing.count(&block) == 0) created.push_back(&block);
                // A use left in a surviving block (an entry-block store, say) must not dangle.
                for (auto* block : created)
                    for (auto& inst : *block)
                        if (!inst.use_empty()) inst.replaceAllUsesWith(llvm::PoisonValue::get(inst.getType()));
                for (auto* block : created) block->dropAllReferences();
                for (auto* block : created) block->eraseFromParent();
            }
            compiler->RestoreBuilderState(state);
            compiler->RestoreDetectionLedgers(std::move(detection));
            compiler->bondedValues_ = std::move(bondedValues);
            compiler->deferredTempUniqueFieldEscapes_ = std::move(deferredTempEscapes);
            compiler->tempUniqueFieldArgs_ = std::move(tempUniqueFieldArgs);
            compiler->coreUniqueGetterSource_ = std::move(coreUniqueGetterSource);
            compiler->ownSlotLeavingLoads_ = std::move(ownSlotLeavingLoads);
            compiler->globalAssignBorrowOrigin_ = std::move(globalAssignBorrowOrigin);
            compiler->globalAssignBorrowedAddress_ = std::move(globalAssignBorrowedAddress);
            compiler->ownOriginSlots_ = std::move(ownOriginSlots);
            compiler->uniqueFieldBorrowResults_ = std::move(uniqueFieldBorrowResults);
            compiler->pendingAliasReturnInference = std::move(pendingAliasReturns);
            compiler->unwindPartial_ = std::move(unwindPartial);
            compiler->unwindCallConsumedTemps_ = std::move(unwindCallConsumedTemps);
            compiler->moveTransferConsumedTemps_ = std::move(moveTransferConsumedTemps);
            lastParenExprType = savedParenType;
            lastParenExprStorage = savedParenStorage;
            lastParenExprFromOwningTempField = savedParenFromTempField;
            lastParenExprOwningTempParent = savedParenTempParent;
            lastParenExprOwningStructName = savedParenStructName;
            lastParenExprFieldName = savedParenFieldName;
            PlusPlus = std::move(savedPostfix);
            compiler->lastCxxRetTemp_ = savedRetTemp;
            compiler->lastCxxRetValue_ = savedRetValue;
            compiler->lastCallIsBonded = savedBonded;
        };

        UnevaluatedOperandType out;
        try
        {
            auto nv = emit();
            out.Type = nv.TypeAndValue;
            out.SourceTypeName = nv.InferSourceTypeName;
            out.BaseType = nv.BaseType;
            if (nv.Primary != nullptr)
            {
                out.ValueType = nv.Primary->getType();
                out.HasValue = true;
                out.IsStringLiteral = compiler->IsStringLiteralGlobal(nv.Primary);
            }
            else if (nv.Storage != nullptr)
                out.HasValue = true;
        }
        catch (...)
        {
            discard();
            throw;
        }
        discard();
        return out;
    }

LLVMBackend::NamedVariable MainListener::ParseUnaryExpression(CFlatParser::UnaryExpressionContext* ctx,
                                                    ResultUse use) {
        return ParseUnaryExpressionImpl(ctx, use, ctx->Sizeof().size());
    }

bool MainListener::IsCxxTemporaryPrvalue(const LLVMBackend::NamedVariable& value) const
{
        const auto& type = value.TypeAndValue;
        return compilerLLVM != nullptr
            && !value.IsCxxPrvalueSubobject && !value.IsCxxPrvaluePointerSubobject
            && (value.IsRvalue || value.IsCxxPrvalueTemp || value.IsCxxDirectConstruction
                || value.IsCxxPrvalueConditional)
            && !value.IsNormalizedCxxXvalue
            && !type.Pointer && !type.IsAlias && !type.IsCxxRef() && !type.IsRvalueRef
            && compilerLLVM->IsCxxRecord(compilerLLVM->ResolveTypeAlias(type.TypeName));
}

void MainListener::ClearCxxPrvalueProvenance(LLVMBackend::NamedVariable& value) const
{
        value.IsRvalue = false;
        value.IsNormalizedCxxXvalue = false;
        value.IsCxxDirectConstruction = false;
        value.IsCxxPrvalueTemp = false;
        value.IsCxxPrvalueSubobject = false;
        value.IsCxxPrvaluePointerSubobject = false;
        value.IsCxxPrvalueConditional = false;
        value.CxxPrvalueOwnerType.clear();
}

bool MainListener::IsPointerLikeParent(const LLVMBackend::TypeAndValue& parent) const
{
        return parent.ValuePointerDepth() >= 1 || parent.IsCxxRefToPointer;
}

void MainListener::PropagateParentConstToField(LLVMBackend::NamedVariable& field,
        const LLVMBackend::TypeAndValue& parent,
        const LLVMBackend::TypeAndValue& fieldType) const
{
        const bool parentConst = IsPointerLikeParent(parent)
            ? parent.PointeeConstBit() : (parent.IsConstObject() || parent.CxxConstRef());
        if (!parentConst || fieldType.IsAlias || fieldType.IsCxxRef()
            || fieldType.IsCxxMutableField) return;
        const int depth = field.TypeAndValue.ValuePointerDepth();
        if (depth >= 0 && depth < 8)
            field.TypeAndValue.ConstMask |= static_cast<uint8_t>(1u << depth);
}

void MainListener::PropagateCxxPrvalueSubobject(LLVMBackend::NamedVariable& field,
        const LLVMBackend::NamedVariable& parent,
        const LLVMBackend::TypeAndValue& fieldType, bool fieldIsPointer) const
{
        const bool parentIsPrvalueObject = !IsPointerLikeParent(parent.TypeAndValue)
            && !parent.TypeAndValue.IsCxxRef()
            && (IsCxxTemporaryPrvalue(parent) || parent.IsCxxPrvalueSubobject
                || parent.IsCxxPrvaluePointerSubobject);
        if (!parentIsPrvalueObject || fieldType.IsCxxRef() || fieldType.IsRvalueRef)
        {
            ClearCxxPrvalueProvenance(field);
            return;
        }
        const std::string ownerType = (parent.IsCxxPrvalueSubobject
            || parent.IsCxxPrvaluePointerSubobject)
            ? parent.CxxPrvalueOwnerType : parent.TypeAndValue.TypeName;
        if (!compilerLLVM->IsCxxRecord(compilerLLVM->ResolveTypeAlias(ownerType)))
        {
            ClearCxxPrvalueProvenance(field);
            return;
        }
        if (fieldIsPointer)
        {
            ClearCxxPrvalueProvenance(field);
            field.IsCxxPrvaluePointerSubobject = true;
        }
        else
        {
            field.IsCxxPrvalueSubobject = true;
            field.IsCxxPrvaluePointerSubobject = false;
        }
        field.CxxPrvalueOwnerType = ownerType;
}

LLVMBackend::NamedVariable MainListener::ParseOffsetofText(antlr4::ParserRuleContext* errCtx,
                                                             const std::string& typeText,
                                                             const std::vector<std::string>& path)
{
    auto* compiler = Compiler(errCtx);
    antlr4::ANTLRInputStream input(typeText);
    CFlatLexer lexer(&input);
    antlr4::CommonTokenStream tokens(&lexer);
    CFlatParser parser(&tokens);
    lexer.removeErrorListeners();
    parser.removeErrorListeners();
    auto* typeCtx = parser.typeName();
    if (parser.getNumberOfSyntaxErrors() != 0 || tokens.LA(1) != -1)
    {
        LogErrorContext(errCtx, "offsetof: type must be a record type");
        return {};
    }
    auto typeValue = ParseTypeName(typeCtx, false, errCtx);
    if (typeValue.Pointer || typeValue.TypeName.empty())
    {
        LogErrorContext(errCtx, "offsetof: type must be a record type");
        return {};
    }
    uint64_t totalOffset = 0;
    std::string currentType = typeValue.TypeName;
    for (size_t part = 0; part < path.size(); ++part)
    {
        auto found = compiler->dataStructures.find(currentType);
        if (found == compiler->dataStructures.end())
        {
            LogErrorContext(errCtx, std::format("offsetof: type '{}' is not a record", currentType));
            return {};
        }
        auto& record = found->second;
        if (!record.CxxOffsetLayout && !compiler->IsCxxRecord(currentType)
            && (record.StructType == nullptr || record.StructType->isOpaque()))
        {
            LogErrorContext(errCtx, std::format("offsetof: type '{}' is incomplete", currentType));
            return {};
        }
        const std::string& fieldName = path[part];
        auto bitfield = std::find_if(record.Bitfields.begin(), record.Bitfields.end(),
            [&](const auto& field) { return field.Name == fieldName; });
        if (bitfield != record.Bitfields.end())
        {
            LogErrorContext(errCtx, std::format("offsetof: bitfield member '{}' is not allowed", fieldName));
            return {};
        }
        auto field = std::find_if(record.StructFields.begin(), record.StructFields.end(),
            [&](const auto& candidate) { return candidate.VariableName == fieldName; });
        if (field == record.StructFields.end())
        {
            compiler->EnsureCxxMemberProjected(currentType, fieldName);
            found = compiler->dataStructures.find(currentType);
            if (found != compiler->dataStructures.end())
                field = std::find_if(found->second.StructFields.begin(), found->second.StructFields.end(),
                    [&](const auto& candidate) { return candidate.VariableName == fieldName; });
        }
        if (found == compiler->dataStructures.end() || field == found->second.StructFields.end())
        {
            LogErrorContext(errCtx, std::format("offsetof: unknown field '{}' in '{}'", fieldName, currentType));
            return {};
        }
        const size_t index = static_cast<size_t>(field - found->second.StructFields.begin());
        if (!found->second.IsUnion)
        {
            if (found->second.CxxOffsetLayout && index < found->second.CxxFieldOffsets.size())
                totalOffset += found->second.CxxFieldOffsets[index];
            else if (found->second.StructType != nullptr && !found->second.StructType->isOpaque())
                totalOffset += compiler->module->getDataLayout().getStructLayout(
                    found->second.StructType)->getElementOffset(index);
        }
        if (part + 1 < path.size())
        {
            if (field->ConstArraySize > 0 || field->AliasArraySize > 0)
            {
                LogErrorContext(errCtx, std::format("offsetof: field path cannot pass through array field '{}'", fieldName));
                return {};
            }
            if (field->Pointer || field->IsArrayView)
            {
                LogErrorContext(errCtx, std::format("offsetof: field path cannot pass through pointer field '{}'", fieldName));
                return {};
            }
            currentType = field->TypeName;
            if (currentType.empty())
            {
                LogErrorContext(errCtx, std::format("offsetof: field '{}' is not a record", fieldName));
                return {};
            }
        }
    }
    LLVMBackend::NamedVariable result;
    result.Primary = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*compiler->context), totalOffset);
    result.TypeAndValue.TypeName = "u64";
    return result;
}

LLVMBackend::NamedVariable MainListener::ParseUnaryExpressionImpl(CFlatParser::UnaryExpressionContext* ctx,
                                                    ResultUse use, size_t sizeofs) {
        RequireStackHeadroom(ctx);
        auto* compiler = Compiler(ctx);
        CallPostfixScope unaryOperatorPostfix(this);
        auto postFixCtx = ctx->postfixExpression();
        auto castExpCtx = ctx->castExpression();
        auto unaryOperator = ctx->unaryOperator();
        auto typeNameCtx = ctx->typeName();
        auto clearConsumedUnarySource = [](LLVMBackend::NamedVariable& value) {
            value.CallerName.clear();
            value.FieldName.clear();
            value.OwningStructName.clear();
            value.FieldPathText.clear();
            value.ConditionalMoveSources.clear();
            value.TypeAndValue.VariableName.clear();
            value.TypeAndValue.ParentVariableName.clear();
            // An operator result is a new value, never the macro itself (only parens pass through).
            value.IsCxxMacroGlobal = false;
            value.IsCxxNullIntegerMacro = false;
        };

        if (ctx->offsetofFieldPath() != nullptr
            && ctx->getStart()->getText() == "__builtin_offsetof")
        {
            std::vector<std::string> path;
            for (auto* part : ctx->offsetofFieldPath()->Identifier()) path.push_back(part->getText());
            return ParseOffsetofText(ctx, CollapsedSourceText(typeNameCtx), path);
        }

        if (ctx->getStart()->getText() == "alignof" && ctx->unaryExpression() != nullptr)
        {
            auto operand = EvaluateOperandTypeOnly([&]() {
                FlexibleArrayKeepScope keep(flexibleArrayKeep_,
                    DirectOperandPostfixes(ctx->unaryExpression()));
                return ParseUnaryExpression(ctx->unaryExpression(), ResultUse::Value);
            });
            if (!operand.HasValue)
            {
                LogErrorContext(ctx, "sizeof/alignof: could not determine type");
                return {};
            }
            auto typeValue = operand.Type;
            if ((typeValue.TypeName == "auto" || typeValue.TypeName.empty()) && operand.ValueType != nullptr)
            {
                typeValue = {};
                typeValue.Pointer = operand.ValueType->isPointerTy();
                typeValue.TypeName = typeValue.Pointer ? "void" : LLVMTypeToTypeName(operand.ValueType);
                if (operand.ValueType->isVoidTy()) typeValue.TypeName.clear();
            }
            llvm::Type* llvmType = typeValue.TypeName.empty()
                ? nullptr : compiler->GetType(typeValue, nullptr, true);
            if (llvmType == nullptr || llvmType->isVoidTy())
                llvmType = operand.ValueType;
            if (llvmType == nullptr || llvmType->isVoidTy())
            {
                LogErrorContext(ctx, "sizeof/alignof: could not resolve type to LLVM type");
                return {};
            }
            return SizeOrAlignOfType(typeValue, llvmType, false);
        }

        // Only `sizeof '(' typeName ')'` has a type operand; every other prefix (`sizeof x`,
        // `sizeof(expr)`, `sizeof -x`, `sizeof alignof(T)`) takes an unevaluated expression.
        const bool sizeofTypeNameForm = typeNameCtx != nullptr && ctx->children.size() == 4
            && !ctx->Sizeof().empty();
        if (sizeofs > 0 && !sizeofTypeNameForm)
        {
            if (sizeofs == 1 && postFixCtx != nullptr)
            {
                // `sizeof(T)` may still reach here with the type parsed as an expression.
                auto asType = TrySizeofPostfixAsType(ctx, postFixCtx);
                if (asType.Primary != nullptr) return asType;
            }
            auto operand = EvaluateOperandTypeOnly([&]() {
                FlexibleArrayKeepScope keep(flexibleArrayKeep_, DirectOperandPostfixes(ctx));
                return ParseUnaryExpressionImpl(ctx, ResultUse::Value, sizeofs - 1);
            });
            if (IsZeroLengthArrayMemberOperand(operand.Type))
            {
                LLVMBackend::NamedVariable zero;
                zero.Primary = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*compiler->context), 0);
                zero.TypeAndValue.TypeName = "i64";
                return zero;
            }
            if (std::string error = FlexibleArrayMemberLengthError(operand.Type);
                !error.empty())
            {
                LogErrorContext(ctx, error);
                return {};
            }
            if (!operand.HasValue)
            {
                LogErrorContext(ctx, "sizeof/alignof: could not determine type");
                return {};
            }
            auto typeValue = operand.Type;
            if ((typeValue.TypeName == "auto" || typeValue.TypeName.empty()) && operand.ValueType != nullptr)
            {
                // No declared type: the operand value's own LLVM type is exact (a pointer stays one).
                typeValue = {};
                typeValue.Pointer = operand.ValueType->isPointerTy();
                typeValue.TypeName = typeValue.Pointer ? "void" : LLVMTypeToTypeName(operand.ValueType);
                if (operand.ValueType->isVoidTy()) typeValue.TypeName.clear();
            }
            llvm::Type* llvmType = typeValue.TypeName.empty()
                ? nullptr : compiler->GetType(typeValue, nullptr, true);
            if (llvmType == nullptr || llvmType->isVoidTy())
                llvmType = operand.ValueType;
            if (llvmType == nullptr || llvmType->isVoidTy())
            {
                LogErrorContext(ctx, "sizeof/alignof: could not resolve type to LLVM type");
                return {};
            }
            return SizeOrAlignOfType(typeValue, llvmType, true);
        }

        if (postFixCtx != nullptr)
        {
            // Single-child passthrough: forward the use unchanged.
            return ParsePostfixExpression(postFixCtx, false, 0, use);
        }
        else if (auto* newCtx = ctx->newExpression())
        {
            return ParseNewExpression(newCtx);
        }
        else if (auto* delCtx = ctx->deleteExpression())
        {
            return ParseDeleteExpression(delCtx);
        }
        else if (auto* moveCtx = ctx->moveExpression())
        {
            return ParseMoveExpression(moveCtx);
        }
        else if (auto* opStrCtx = ctx->operatorStringExpression())
        {
            return ParseOperatorStringExpression(opStrCtx);
        }
        else if (unaryOperator && castExpCtx)
        {
            /* unaryOperator : '&' | '*'| '+'| '-'| '~'| '!'; */

            std::string opText = unaryOperator->getText();
            const auto expectedTypeBeforeUnary = declExpectedType;
            // Logical-not, dereference, and address-of change the operand's type, so the
            // enclosing destination cannot describe that operand. Built-in +, -, and ~ are
            // type-preserving and retain the destination context for their operand.
            const bool genericFunctionAddress = opText == "&"
                && !declExpectedType.FuncPtrReturnTypeName.empty()
                && GenericFunctionValueIdentifier(castExpCtx, true) != nullptr;
            std::optional<DeclExpectedTypeScope> unaryExpectedScope;
            if (opText == "!" || opText == "*"
                || (opText == "&" && !genericFunctionAddress))
                unaryExpectedScope.emplace(&declExpectedType, LLVMBackend::TypeAndValue{});

            // For '!', suspend any active short-circuit else-block before
            // evaluating the operand.  Without this, !(A && B) in a while
            // condition would short-circuit the inner && directly to the
            // loop-exit block when A is false - bypassing the negation -
            // instead of continuing the loop as !(false && B) == true demands.
            llvm::BasicBlock* savedElse = nullptr;
            if (opText == "!")
                savedElse = compiler->ExchangeElseBlock(nullptr);

            std::optional<FlexibleArrayKeepScope> addressKeep;
            if (opText == "&") addressKeep.emplace(flexibleArrayKeep_, DirectOperandPostfixes(castExpCtx));
            if (opText == "&")
            {
                const std::string operandText = castExpCtx->getText();
                const size_t dot = operandText.rfind('.');
                if (dot != std::string::npos)
                {
                    const std::string owner = operandText.substr(0, dot);
                    const std::string memberName = operandText.substr(dot + 1);
                    const size_t genericOpen = owner.find('<');
                    if (genericOpen != std::string::npos)
                    {
                        std::string templateName = owner.substr(0, genericOpen);
                        if (const size_t nsDot = templateName.rfind('.'); nsDot != std::string::npos)
                            templateName = templateName.substr(nsDot + 1);
                        CFlatParser::StructDefinitionContext* structTemplate = nullptr;
                        if (auto it = genericStructTemplates.find(templateName);
                            it != genericStructTemplates.end())
                            structTemplate = it->second;
                        CFlatParser::ClassDefinitionContext* classTemplate = nullptr;
                        if (auto it = genericClassTemplates.find(templateName);
                            it != genericClassTemplates.end())
                            classTemplate = it->second;
                        auto hasInstanceMethod = [&](auto* definition) {
                            if (definition == nullptr) return false;
                            for (auto* aggregateMember : definition->aggregateMember())
                            {
                                auto* function = aggregateMember->functionDefinition();
                                if (function != nullptr && function->directDeclarator() != nullptr
                                    && function->directDeclarator()->getText() == memberName)
                                    return function->declarationSpecifiers() == nullptr
                                        || function->declarationSpecifiers()->getText().find("static")
                                            == std::string::npos;
                            }
                            return false;
                        };
                        if (hasInstanceMethod(structTemplate) || hasInstanceMethod(classTemplate))
                        {
                            LogErrorContext(ctx,
                                "pointers to members are not supported in CFlat; use a lambda");
                            return {};
                        }
                    }
                }
            }
            // RAII: ExpectedErrorReceived can unwind out of the operand and must not leak the depth.
            struct AddressOfDepthScope
            {
                unsigned* depth;
                ~AddressOfDepthScope() { if (depth) --*depth; }
            } addressDepth{ opText == "&" ? &addressOfOperandDepth_ : nullptr };
            if (addressDepth.depth) ++*addressDepth.depth;
            auto namedVar = ParseCastExpression(castExpCtx);
            const bool unarySourceHasPointerDereference = namedVar.FieldPathHasPointerDereference;
            const bool unarySourcePointerFrameLocal = namedVar.FieldPathPointerFrameLocal;
            if (addressDepth.depth) { --*addressDepth.depth; addressDepth.depth = nullptr; }
            addressKeep.reset();
            const bool addressOfPointerObject = opText == "&"
                && namedVar.TypeAndValue.IsProvenSinglePointerDepth();
            const bool cxxReferenceResult = namedVar.TypeAndValue.IsAlias
                && !namedVar.TypeAndValue.Pointer
                && compiler->IsCxxRecord(namedVar.TypeAndValue.TypeName)
                && namedVar.Storage != nullptr
                && namedVar.Storage->getType()->isPointerTy()
                && compiler->IsAliasValue(namedVar.Storage);

            // Reference results have their referent type here; reject known non-pointers before loads.
            // Records and enums continue through overload / implicit-pointer-conversion lookup.
            const auto& unaryOperandType = namedVar.TypeAndValue;
            const bool unaryOperandEnum = opText == "*"
                && !compiler->ResolveEnumTypeName(unaryOperandType.TypeName).empty();
            const auto* encodedCallableType = opText == "*"
                ? compiler->GetEncodedClosureType(unaryOperandType.TypeName) : nullptr;
            auto* callableStruct = llvm::dyn_cast_or_null<llvm::StructType>(namedVar.BaseType);
            const bool unaryOperandCallableType = opText == "*"
                && (unaryOperandType.IsFunctionPointer
                    || unaryOperandType.TypeName == "__c_fn_ptr"
                    || unaryOperandType.TypeName == "__closure_fat_ptr"
                    || encodedCallableType != nullptr
                    || (callableStruct != nullptr && callableStruct->hasName()
                        && callableStruct->getName() == "__closure_fat_ptr"));
            const bool resolvedCallableNeedsNoFurtherLoad = !unaryOperandType.Pointer
                && !unaryOperandType.ElemPointer && unaryOperandType.PointerDepth == 0;
            const bool unaryOperandCallable = opText == "*"
                && ((unaryOperandCallableType && resolvedCallableNeedsNoFurtherLoad)
                    || llvm::isa_and_nonnull<llvm::Function>(namedVar.Primary));
            if (opText == "*")
            {
                llvm::Type* unaryOperandLLVMType = namedVar.BaseType;
                if (unaryOperandLLVMType == nullptr && namedVar.Primary != nullptr)
                    unaryOperandLLVMType = namedVar.Primary->getType();
                const bool unaryOperandArray = unaryOperandLLVMType != nullptr
                    && unaryOperandLLVMType->isArrayTy();
                const bool unaryOperandPointerLike = unaryOperandType.Pointer
                    || unaryOperandType.ElemPointer || unaryOperandType.IsFunctionPointer
                    || unaryOperandType.IsArrayView || unaryOperandType.IsInterfacePointer
                    || unaryOperandType.IsCxxRefToPointer || unaryOperandType.PointerDepth > 0
                    || unaryOperandType.ConstArraySize > 0 || unaryOperandArray;
                const bool unaryOperandMayOverload = compiler->IsCxxRecord(unaryOperandType.TypeName)
                    || compiler->dataStructures.count(unaryOperandType.TypeName) != 0
                    || unaryOperandType.IsInterface || unaryOperandEnum || unaryOperandCallableType
                    || unaryOperandCallable;
                const bool hasUnaryOperandSpelling = !unaryOperandType.TypeName.empty()
                    && unaryOperandType.TypeName != "auto";
                if (!unaryOperandPointerLike && !unaryOperandMayOverload
                    && !compiler->IsCoreUniqueType(unaryOperandType.TypeName)
                    && (hasUnaryOperandSpelling || unaryOperandLLVMType != nullptr))
                {
                    auto diagnosticType = unaryOperandType;
                    // An untyped integer literal is narrowed in LLVM; spell it as C does ('int', or 'long').
                    auto* unaryOperandLiteral = llvm::dyn_cast_or_null<llvm::ConstantInt>(namedVar.Primary);
                    if (!hasUnaryOperandSpelling && unaryOperandLiteral != nullptr)
                        diagnosticType.TypeName = unaryOperandLiteral->getValue().isSignedIntN(32) ? "int" : "long";
                    else if (!hasUnaryOperandSpelling && unaryOperandLLVMType != nullptr)
                        diagnosticType.TypeName = LLVMTypeToTypeName(unaryOperandLLVMType);
                    LogErrorContext(ctx, std::format(
                        "indirection requires pointer operand ('{}' invalid)",
                        SpellType(*compiler, diagnosticType)));
                }
            }

            if (opText == "!")
                compiler->ExchangeElseBlock(savedElse);
            // A C++ `T&&` class result is an xvalue at its address: the overload acts on that object.
            const bool xvalueOperand = (opText == "!" || opText == "-" || opText == "+" || opText == "~")
                && compiler->IsCxxRvalueRefClassResult(namedVar);
            if (xvalueOperand)
            {
                namedVar.Storage = namedVar.Primary;
                namedVar.Primary = nullptr;
                namedVar.BaseType = compiler->GetType(LLVMBackend::TypeAndValue{ .TypeName = namedVar.TypeAndValue.TypeName });
                namedVar.TypeAndValue.Pointer = false;
                namedVar.TypeAndValue.IsRvalueRef = false;
                namedVar.IsRvalue = true;
            }

            if (opText == "&")
            {
                std::string temporaryType;
                if (IsCxxTemporaryPrvalue(namedVar))
                    temporaryType = namedVar.TypeAndValue.TypeName;
                else if (namedVar.IsCxxPrvalueSubobject
                    || namedVar.IsCxxPrvaluePointerSubobject)
                    temporaryType = namedVar.CxxPrvalueOwnerType;
                if (!temporaryType.empty())
                    LogErrorContext(ctx, std::format(
                        "cannot take the address of a temporary of C++ type '{}'",
                        compiler->DisplayCxxClassName(temporaryType)));

                const llvm::GlobalVariable* enumMemberGlobal = nullptr;
                if (compiler->IsEnumMemberStorage(namedVar.Storage, &enumMemberGlobal))
                    LogErrorContext(ctx, std::format("cannot take the address of enum member '{}'",
                        EnumMemberDiagnosticName(namedVar, castExpCtx, enumMemberGlobal)));
                if (namedVar.TypeAndValue.IsArrayView)
                    LogErrorContext(ctx, std::format(
                        "cannot take the address of array-view '{}'; use '&{}[0]' to get the element address",
                        SpellType(*compiler, namedVar.TypeAndValue), castExpCtx->getText()));
                if (namedVar.IsCxxMacroGlobal || namedVar.IsCxxMacroJoin)
                {
                    LogErrorContext(ctx, std::format(
                        "cannot take the address of macro constant '{}'", castExpCtx->getText()));
                }
                if (namedVar.BitfieldStorage)
                {
                    LogErrorContext(ctx, std::format(
                        "cannot take the address of bitfield '{}' - bitfields are not byte-addressable",
                        namedVar.FieldName));
                }
                if (!namedVar.Storage && namedVar.IsUnresolvedGenericFunctionTemplate)
                {
                    auto* identifier = GenericFunctionValueIdentifier(castExpCtx, true);
                    const std::string functionName = identifier != nullptr
                        ? identifier->getText() : namedVar.CallerName;
                    LogErrorContext(ctx, std::format(
                        "cannot take the address of generic function '{}' without explicit type arguments; "
                        "bind it to a function<...> of the wanted type so they are deduced", functionName));
                }
                if (!namedVar.Storage && !namedVar.CallerName.empty())
                {
                    if (namedVar.Primary == nullptr)
                        namedVar.Primary = compiler->GetFunctionForFuncPtr(namedVar.CallerName);
                    if (llvm::isa_and_nonnull<llvm::Function>(namedVar.Primary))
                    {
                        const auto& expected = expectedTypeBeforeUnary;
                        if (expected.IsFunctionPointer && expected.IsThinFnPtr()
                            && expected.Pointer)
                        {
                            auto functionValueType = namedVar.TypeAndValue;
                            if (functionValueType.FuncPtrReturnTypeName.empty())
                                if (const auto* symbol = compiler->FindSymbolForFunction(
                                        llvm::cast<llvm::Function>(namedVar.Primary)))
                                    functionValueType = compiler->FuncPtrSigOfSymbol(*symbol);
                            functionValueType.Pointer = false;
                            functionValueType.ElemPointer = false;
                            functionValueType.PointerDepth = 0;
                            const std::string functionValueTypeName = std::format(
                                "function<{}>", compiler->FuncPtrSpellingOf(functionValueType));
                            // Spell the source name (`idt<int>`), never the mangled instance (invertible mangling).
                            const std::string sourceName = compiler->FindFunctionSourceName(
                                llvm::cast<llvm::Function>(namedVar.Primary));
                            std::string functionName = SpellFunctionSymbol(*compiler, sourceName);
                            if (functionName == sourceName)
                                functionName = SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = sourceName });
                            LogErrorContext(ctx, std::format(
                                "'&{}' is already the function value '{}'; drop the '&' or declare "
                                "the destination as '{}'",
                                functionName, functionValueTypeName, functionValueTypeName));
                        }
                        return namedVar;
                    }
                }
                if (!namedVar.Storage && IsParenthesizedConditionalOperand(castExpCtx))
                {
                    // Clang: a `?:` address needs both arms to be lvalues, else no object exists.
                    LogErrorContext(ctx, "cannot take the address of a '?:' unless both arms are lvalues; "
                        "a postfix ++/-- arm, a literal or a call result is a prvalue");
                }
                else if (!namedVar.Storage)
                {
                    LogErrorContext(ctx, "Unable to get an Address-of an object without a Storage.");
                }

                // `&w` on a borrowed by-value struct parameter launders every later field path
                // past the root walk, so record the fact on the produced pointer (measured rc 134).
                {
                    LLVMBackend::NamedVariable probe = namedVar;
                    if (probe.TypeAndValue.VariableName.empty())
                        probe.TypeAndValue.VariableName = namedVar.CallerName;
                    if (IsBorrowedByValueParamBinding(compiler, probe))
                    {
                        namedVar.PointsToBorrowedByValueParam = true;
                        PrepareBorrowedStringParamForDataMut(compiler, probe);
                    }
                }
                bool addressOfUniqueSlot = false;
                const bool coreUniqueValue = compiler->IsCoreUniqueType(namedVar.TypeAndValue.TypeName)
                    && !namedVar.TypeAndValue.Pointer && namedVar.Storage != nullptr;
                std::string addressedUniqueTypeName;
                if (namedVar.TypeAndValue.IsCoreUniqueOwner && namedVar.TypeAndValue.Pointer
                    && !namedVar.TypeAndValue.ElemPointer
                    && !namedVar.TypeAndValue.IsArrayView
                    && namedVar.TypeAndValue.ConstArraySize == 0
                    && namedVar.Storage != nullptr)
                {
                    std::vector<std::string> uniqueArgs{ namedVar.TypeAndValue.TypeName };
                    if (namedVar.TypeAndValue.AllocAlignValue != 0)
                        uniqueArgs.push_back(std::to_string(namedVar.TypeAndValue.AllocAlignValue));
                    const std::string candidate = MangledGenericName("unique", uniqueArgs);
                    if (compiler->IsCoreUniqueType(candidate))
                        addressedUniqueTypeName = candidate;
                }
                if (coreUniqueValue || !addressedUniqueTypeName.empty())
                {
                    namedVar.AddressedUniqueSlot = namedVar.Storage;
                    if (coreUniqueValue)
                    {
                        addressedUniqueTypeName = namedVar.TypeAndValue.TypeName;
                        const auto& uniqueData = compiler->GetDataStructure(addressedUniqueTypeName);
                        for (size_t i = 0; i < uniqueData.StructFields.size(); i++)
                            if (uniqueData.StructFields[i].VariableName == "_p")
                            {
                                namedVar.Primary = compiler->CreateStructGEP(
                                    namedVar.BaseType, namedVar.Storage, (uint32_t)i);
                                break;
                            }
                    }
                    else
                        namedVar.Primary = namedVar.Storage;
                    if (namedVar.Primary != nullptr)
                    {
                        namedVar.AddressedUniqueTypeName = addressedUniqueTypeName;
                        namedVar.TypeAndValue.TypeName = addressedUniqueTypeName;
                        namedVar.TypeAndValue.Pointer = true;
                        namedVar.TypeAndValue.ElemPointer = false;
                        namedVar.TypeAndValue.PointerDepth = 0;
                        namedVar.TypeAndValue.IsUnique = false;
                        namedVar.TypeAndValue.IsCoreUniqueOwner = false;
                        namedVar.TypeAndValue.IsMove = false;
                        namedVar.TypeAndValue.IsInterface = false;
                        namedVar.TypeAndValue.IsInterfacePointer = false;
                        namedVar.BaseType = namedVar.Primary->getType();
                        namedVar.Storage = nullptr;
                        addressOfUniqueSlot = true;
                    }
                    else
                    {
                        namedVar.AddressedUniqueSlot = nullptr;
                        namedVar.AddressedUniqueTypeName.clear();
                        namedVar.Primary = namedVar.Storage;
                        namedVar.Storage = nullptr;
                    }
                }
                else
                {
                    namedVar.Primary = namedVar.Storage;
                    namedVar.Storage = nullptr;
                }
                if (namedVar.TypeAndValue.TypeName == "char"
                    && PointsIntoStackFrame(namedVar.Primary))
                {
                    namedVar.StackCharBufferBorrow = true;
                    namedVar.StackCharBufferSource = !namedVar.CallerName.empty()
                        ? namedVar.CallerName : namedVar.TypeAndValue.VariableName;
                    namedVar.StackCharBufferScopeDepth = namedVar.DeclarationScopeDepth;
                }
                // `&` over an alias lvalue normally records borrow provenance for return/delete.
                // C++ reference-result addresses use the dedicated frame-rooted return gate.
                // A non-pointer `alias` PARAMETER is a borrow of the CALLER's object, so its
                // address is the caller's - not frame-local, and legitimate to return or store.
                const bool addrOfAliasParam = namedVar.Primary != nullptr
                    && llvm::isa<llvm::Argument>(namedVar.Primary)
                    && compiler->ParameterIsAliasByPointer(namedVar.TypeAndValue);
                if (namedVar.IsAliasBorrow || namedVar.TypeAndValue.IsAlias)
                {
                    namedVar.FromOwningTempField = false;
                    namedVar.OwningTempParent = false;
                    namedVar.PointsToAliasBorrow = !addrOfAliasParam && !cxxReferenceResult;
                    namedVar.IsAliasBorrow = false;
                    namedVar.IsAliasReturnBorrow = false;
                    namedVar.TypeAndValue.IsAlias = false;
                }
                // Positive borrow provenance, recorded where the value is PRODUCED: an
                // address-of result points at storage some other binding owns, so it can never be
                // the owning side of a '?:' join.
                compiler->RegisterBorrowedAddressValue(namedVar.Primary);
                // The address of an element (e.g. &a[i]) is a raw pointer to one slot, never a
                // whole-allocation array-view. Clear the flag so it cannot bind back into a `T[]`
                // (which would forge an offset view that overlaps the original - the escape hatch
                // the noalias contract must keep one-way). Mark a scalar operand's address as a
                // pointer so the array-view bind gate sees `&a[i]` as the raw `int*` it is.
                namedVar.TypeAndValue.IsArrayView = false;
                // `&flexible` is a pointer to its first element, never a sized array.
                if (namedVar.TypeAndValue.IsFlexibleArrayMember)
                {
                    namedVar.TypeAndValue.IsFlexibleArrayMember = false;
                    namedVar.TypeAndValue.IsZeroLengthArrayMember = false;
                    namedVar.TypeAndValue.ConstArraySize = 0;
                    namedVar.TypeAndValue.ConstInnerDimensions.clear();
                }
                /*
                 * Depth: `&` adds one level to an ALREADY-RECORDED depth. An operand whose depth
                 * was never recorded stays unrecorded - `&` cannot invent a claim. An operand
                 * already PROVEN at the cap would produce a depth-3 VALUE, which the two-level
                 * model cannot represent; refuse it where it is produced, exactly as a written
                 * `T***` is refused at its declarator, rather than clamping it back to `T**`.
                 */
                if (namedVar.TypeAndValue.PointerDepth >= PointerDepthCap
                    && namedVar.TypeAndValue.DepthIsAboutThisValue())
                {
                    std::string subject = namedVar.CallerName.empty()
                        ? std::string("address-of expression")
                        : std::format("address-of '{}{}'", namedVar.CallerName,
                            namedVar.FieldName.empty() ? "" : "." + namedVar.FieldName);
                    LogErrorContext(ctx, PointerDepthCapMessage(subject,
                        namedVar.TypeAndValue.PointerDepth + 1));
                }
                if (namedVar.TypeAndValue.PointerDepth >= 1)
                    namedVar.TypeAndValue.PointerDepth =
                        namedVar.TypeAndValue.PointerDepth >= 2 ? 2 : namedVar.TypeAndValue.PointerDepth + 1;
                if (addressOfUniqueSlot) namedVar.TypeAndValue.PointerDepth = 1;
                // The operand's storage moved to Primary above: judge the object at that address.
                LLVMBackend::NamedVariable addressedObject = namedVar;
                if (addressedObject.Storage == nullptr) addressedObject.Storage = namedVar.Primary;
                compiler->DisarmNonLocalViewFieldsForEscape(addressedObject.Storage, true);
                const bool addressOfConstObject = !namedVar.TypeAndValue.Pointer
                    && compiler->CxxConstReceiverKind(addressedObject) != 0
                    && !namedVar.TypeAndValue.IsCxxRefToPointer;
                if (!namedVar.TypeAndValue.Pointer)
                    namedVar.TypeAndValue.Pointer = true;
                if (addressOfConstObject)
                    namedVar.TypeAndValue.SetPointeeConstBit(true);
                namedVar.IsCxxAddressOfPointerValue = addressOfPointerObject
                    && namedVar.TypeAndValue.IsProvenDoublePointer();
                // The address of an element borrows, it does not own - clear IsOwning so taking
                // `&a[i]` does not move/consume the owning buffer `a` it points into.
                namedVar.IsOwning = false;
                // Move-dataflow: taking a variable's address (e.g. an out-parameter `receive(&v)`)
                // may reinitialize it, so treat it as a revive - the value is live again afterward.
                compiler->RecordMoveGenRevive(namedVar.CallerName);
                // The escaped address could rewrite the pointee (e.g. `fill(&a)`) - latch the
                // explicit-move-null deref guard off for good, per MarkVariableAddressEscaped.
                compiler->MarkVariableAddressEscaped(namedVar.FieldPathRoot.empty()
                    ? namedVar.CallerName : namedVar.FieldPathRoot);
            }
            else if (opText == "*")
            {
                // Clang cannot choose an overload from a dereference of a function designator;
                // argument-based selection happens only for a direct call, not `(*name)(args)`.
                if (llvm::isa_and_nonnull<llvm::Function>(namedVar.Primary)
                    && !namedVar.CallerName.empty())
                {
                    const auto functionName = compiler->ResolveQualifiedName(namedVar.CallerName);
                    auto overloads = compiler->functionTable.find(functionName);
                    if (overloads != compiler->functionTable.end())
                    {
                        std::vector<llvm::Function*> distinctFunctions;
                        for (const auto& overload : overloads->second)
                        {
                            if (overload.IsMethod || overload.Function == nullptr
                                || std::find(distinctFunctions.begin(), distinctFunctions.end(),
                                             overload.Function) != distinctFunctions.end())
                                continue;
                            distinctFunctions.push_back(overload.Function);
                        }
                        if (distinctFunctions.size() > 1)
                            LogErrorContext(ctx,
                                "reference to overloaded function could not be resolved");
                    }
                }

                // Callable identity includes substituted generic types and named function designators.
                const auto& callableShape = namedVar.TypeAndValue;
                if (unaryOperandCallable && !callableShape.Pointer && !callableShape.ElemPointer
                    && callableShape.PointerDepth == 0 && callableShape.ConstArraySize == 0
                    && callableShape.ConstInnerDimensions.empty() && !callableShape.IsArrayView
                    && !callableShape.IsInterfacePointer && !callableShape.IsCxxRefToPointer)
                    return namedVar;

                // A multidimensional array decays to a pointer to its first subarray. Dereference
                // that pointer as an array lvalue so the next `*` can decay the row to an element.
                auto* fixedArrayType = llvm::dyn_cast_or_null<llvm::ArrayType>(namedVar.BaseType);
                const auto arrayShape = namedVar.TypeAndValue;
                const bool arrayOperand = fixedArrayType != nullptr && namedVar.Storage != nullptr
                    && !arrayShape.Pointer && !arrayShape.ElemPointer && !arrayShape.IsArrayView
                    && !arrayShape.IsSimd && !arrayShape.IsInterface && !arrayShape.IsFunctionPointer;
                if (arrayOperand && arrayShape.ConstArraySize > 0
                    && !arrayShape.ConstInnerDimensions.empty()
                    && llvm::isa<llvm::ArrayType>(fixedArrayType->getElementType()))
                {
                    auto* zero = compiler->builder->getInt64(0);
                    namedVar.Storage = compiler->builder->CreateGEP(
                        fixedArrayType, namedVar.Storage, {zero, zero}, "arrayrow");
                    namedVar.BaseType = fixedArrayType->getElementType();
                    namedVar.Primary = nullptr;
                    namedVar.TypeAndValue.ConstArraySize = arrayShape.ConstInnerDimensions.front();
                    namedVar.TypeAndValue.ConstInnerDimensions.erase(
                        namedVar.TypeAndValue.ConstInnerDimensions.begin());
                    return namedVar;
                }

                // A fixed-array subscript clears the outer ConstArraySize; its LLVM ArrayType and
                // remaining dimensions still identify the row. Dereference the row's first element.
                if (arrayOperand && arrayShape.ConstArraySize == 0
                    && !arrayShape.ConstInnerDimensions.empty())
                {
                    std::vector<uint64_t> remainingDimensions = arrayShape.ConstInnerDimensions;
                    remainingDimensions.erase(remainingDimensions.begin());
                    auto* zero = compiler->builder->getInt64(0);
                    namedVar.Storage = compiler->builder->CreateGEP(
                        fixedArrayType, namedVar.Storage, {zero, zero}, "arrayrowelement");
                    namedVar.BaseType = fixedArrayType->getElementType();
                    namedVar.Primary = nullptr;
                    namedVar.TypeAndValue.ConstArraySize = remainingDimensions.empty()
                        ? 0 : remainingDimensions.front();
                    namedVar.TypeAndValue.ConstInnerDimensions.clear();
                    if (!remainingDimensions.empty())
                        namedVar.TypeAndValue.ConstInnerDimensions.assign(
                            remainingDimensions.begin() + 1, remainingDimensions.end());
                    return namedVar;
                }

                // Array-to-pointer decay: `*arr` on a fixed array T[N] reads its first element
                // (clang). The slot holds the array's address, which is the element pointer.
                const auto& arrayTv = namedVar.TypeAndValue;
                if (arrayTv.ConstArraySize > 0 && !arrayTv.Pointer && !arrayTv.ElemPointer
                    && !arrayTv.IsArrayView && !arrayTv.IsSimd
                    && !arrayTv.IsInterface && !arrayTv.IsFunctionPointer
                    && namedVar.Storage != nullptr && namedVar.Storage->getType()->isPointerTy())
                {
                    auto* decayedSlot = compiler->CreateAlloca(compiler->builder->getPtrTy());
                    compiler->builder->CreateStore(namedVar.Storage, decayedSlot);
                    namedVar.TypeAndValue.ConstArraySize = 0;
                    namedVar.TypeAndValue.Pointer = true;
                    namedVar.Storage = decayedSlot;
                    namedVar.Primary = nullptr;
                }
                const bool cxxObjectReference = namedVar.TypeAndValue.Pointer
                    && (namedVar.TypeAndValue.IsAlias || namedVar.TypeAndValue.IsRvalueRef)
                    && compiler->IsCxxRecord(namedVar.TypeAndValue.TypeName);
                if ((!namedVar.TypeAndValue.Pointer || cxxObjectReference)
                    && !compiler->IsCoreUniqueType(namedVar.TypeAndValue.TypeName))
                {
                    const int8_t operandCategory = ctx->castExpression() != nullptr
                        ? CxxArgumentLvalueKind(ctx->castExpression()) : -1;
                    auto operatorSource = namedVar;
                    llvm::Value* operandStorage = nullptr;
                    llvm::Value* operand = nullptr;
                    if (cxxObjectReference)
                    {
                        auto* sourceAddress = this->LoadNamedVariable(namedVar);
                        auto valueType = namedVar.TypeAndValue;
                        valueType.Pointer = false;
                        valueType.PointerDepth = 0;
                        valueType.ElemPointer = false;
                        valueType.IsAlias = false;
                        valueType.IsRvalueRef = false;
                        valueType.IsCxxRefToPointer = false;
                        auto* objectType = compiler->GetType(valueType);
                        if (sourceAddress != nullptr && sourceAddress->getType()->isPointerTy()
                            && objectType != nullptr)
                        {
                            operand = compiler->CreateLoad(objectType, sourceAddress);
                            operandStorage = sourceAddress;
                            operatorSource.Primary = operand;
                            operatorSource.Storage = sourceAddress;
                            operatorSource.BaseType = objectType;
                            operatorSource.TypeAndValue = valueType;
                            operatorSource.IsRvalue = namedVar.TypeAndValue.IsRvalueRef
                                || namedVar.IsRvalue;
                        }
                    }
                    else
                    {
                        const bool constReferenceResult = namedVar.TypeAndValue.CxxConstRef()
                            || compiler->IsCxxConstClassObject(namedVar.TypeAndValue)
                            || (namedVar.Storage != nullptr
                                && compiler->IsCxxConstReferent(namedVar.Storage));
                        operandStorage = (operandCategory == 2
                            || (operandCategory < 0 && namedVar.IsRvalue))
                            && !constReferenceResult ? nullptr : namedVar.Storage;
                        operand = this->LoadNamedVariable(namedVar);
                    }
                    const bool constReferenceResult = operatorSource.TypeAndValue.CxxConstRef()
                        || compiler->IsCxxConstClassObject(operatorSource.TypeAndValue)
                        || (operandStorage != nullptr
                            && compiler->IsCxxConstReferent(operandStorage));
                    if (auto* overload = TryUnaryOperatorOverload(
                            operand, "*", ctx, operandStorage, &operatorSource,
                            constReferenceResult))
                    {
                        namedVar.Primary = overload;
                        namedVar.Storage = nullptr;
                        namedVar.BaseType = overload->getType();
                        namedVar.TypeAndValue = compiler->lastCallReturnType;
                        namedVar.FieldPathFromForwardedArrow = false;
                        PrepareAliasCallResult(ctx, namedVar);
                        return namedVar;
                    }
                    if (unaryOperandEnum)
                        LogErrorContext(ctx, std::format(
                            "indirection requires pointer operand ('{}' invalid)",
                            SpellType(*compiler, namedVar.TypeAndValue)));
                    auto* structTy = llvm::dyn_cast_or_null<llvm::StructType>(
                        operand != nullptr ? operand->getType() : nullptr);
                    if (structTy != nullptr && structTy->hasName()
                        && (compiler->IsCxxRecord(structTy->getName().str())
                            || compiler->generatedCxxRecords_.count(structTy->getName().str()) != 0))
                    {
                        compiler->EnsureCxxConversionOperatorsProjected(structTy->getName().str());
                        LLVMBackend::TypeAndValue intPointer;
                        intPointer.TypeName = "int";
                        intPointer.Pointer = true;
                        const auto* info = compiler->GetCxxClassInfo(structTy->getName().str());
                        bool pointerConversionAttempted = false;
                        if (auto record = compiler->cxxRecordEntries_.find(structTy->getName().str());
                            record != compiler->cxxRecordEntries_.end())
                        {
                            for (const auto& member : record->second.members)
                            {
                                if (!member.isConversion || member.isExplicit || member.isDeleted
                                    || member.retType.find('*') == std::string::npos) continue;
                                pointerConversionAttempted = true;
                                break;
                            }
                        }
                        if (info != nullptr)
                        {
                            for (const auto& method : info->directMethods)
                            {
                                if (!method.spellable || !method.raw.isConversion || method.raw.isExplicit
                                    || method.raw.isDeleted || !method.ret.Pointer) continue;
                                pointerConversionAttempted = true;
                                break;
                            }
                        }
                        if (auto* converted = compiler->ConvertViaImplicitConversionOperator(
                                operand, intPointer))
                        {
                            auto* pointerType = compiler->GetType(intPointer);
                            auto* pointerSlot = compiler->CreateAlloca(pointerType);
                            compiler->CreateAssignment(converted, pointerSlot);
                            namedVar.TypeAndValue = intPointer;
                            namedVar.Primary = nullptr;
                            namedVar.Storage = pointerSlot;
                            namedVar.BaseType = pointerType;
                        }
                        if (!namedVar.TypeAndValue.Pointer && info != nullptr)
                        {
                            for (const auto& method : info->directMethods)
                            {
                                if (!method.spellable || !method.raw.isConversion || method.raw.isExplicit
                                    || method.raw.isDeleted || !method.ret.Pointer) continue;
                                auto* converted = compiler->ConvertViaImplicitConversionOperator(
                                    operand, method.ret);
                                if (converted == nullptr) continue;
                                auto* pointerType = compiler->GetType(method.ret);
                                auto* pointerSlot = compiler->CreateAlloca(pointerType);
                                compiler->CreateAssignment(converted, pointerSlot);
                                namedVar.TypeAndValue = method.ret;
                                namedVar.Primary = nullptr;
                                namedVar.Storage = pointerSlot;
                                namedVar.BaseType = pointerType;
                                break;
                            }
                        }
                        if (pointerConversionAttempted && !namedVar.TypeAndValue.Pointer)
                            LogErrorContext(ctx, std::format(
                                "cannot dereference C++ value of type '{}': its pointer conversion could not be lowered",
                                structTy->getName().str()));
                    }
                }
                if (!namedVar.TypeAndValue.Pointer
                    && compiler->IsCoreUniqueType(namedVar.TypeAndValue.TypeName))
                {
                    auto coreUnique = namedVar;
                    coreUnique.TypeAndValue.VariableName.clear();
                    const bool coreUniqueOwnerFrameLocal =
                        CoreUniqueOwnerPathIsFrameLocal(compiler, coreUnique);
                    if (compiler->IsMovedDerefHere(coreUnique))
                        LogErrorContext(ctx, compiler->MovedDerefDiagnostic(coreUnique));
                    auto pointeeName = MangledGenericArgument(
                        *compiler, coreUnique.TypeAndValue.TypeName);
                    LLVMBackend::TypeAndValue rawType;
                    rawType.TypeName = pointeeName;
                    rawType.Pointer = true;
                    auto raw = compiler->CreateCoreUniqueRawPointerCall(coreUnique, rawType);
                    compiler->RecordNullDerefFor(coreUnique,
                        ctx->getStart()->getLine(), ctx->getStart()->getCharPositionInLine());
                    namedVar.TypeAndValue = {};
                    namedVar.TypeAndValue.TypeName = pointeeName;
                    namedVar.Storage = raw;
                    namedVar.Primary = nullptr;
                    namedVar.BaseType = compiler->GetType(namedVar.TypeAndValue);
                    namedVar.FieldPathThroughPointer = true;
                    namedVar.FieldPathHasPointerDereference = true;
                    namedVar.FieldPathPointerFrameLocal = coreUniqueOwnerFrameLocal
                        && (!coreUnique.FieldPathHasPointerDereference
                            || coreUnique.FieldPathPointerFrameLocal);
                    namedVar.FieldPathFromForwardedArrow = false;
                    namedVar.FieldPathRoot = coreUnique.FieldPathRoot;
                    if (namedVar.FieldPathRoot.empty())
                    {
                        namedVar.FieldPathRoot = !coreUnique.CallerName.empty()
                            ? coreUnique.CallerName
                            : compiler->FindVariableNameByStorage(coreUnique.Storage);
                        if (namedVar.FieldPathRoot.empty())
                        {
                            if (auto* global = llvm::dyn_cast_or_null<llvm::GlobalVariable>(
                                    coreUnique.Storage))
                                namedVar.FieldPathRoot = global->getName().str();
                        }
                    }
                    namedVar.IsStaticLocal = coreUnique.IsStaticLocal;
                    compiler->EmitOwnDerefGuard(
                        coreUnique.Storage, raw,
                        ctx->getStart()->getLine(),
                        ctx->getStart()->getCharPositionInLine());
                    return namedVar;
                }
                // Rvalue pointer expressions carry their value in Primary; spill it for the load path.
                const bool isPointerValue = namedVar.TypeAndValue.Pointer
                    || namedVar.TypeAndValue.ElemPointer || namedVar.TypeAndValue.PointerDepth > 0;
                if (namedVar.Storage == nullptr && namedVar.Primary != nullptr
                    && isPointerValue && namedVar.Primary->getType()->isPointerTy())
                {
                    auto* pointerValue = namedVar.Primary;
                    namedVar.Storage = compiler->CreateAlloca(pointerValue->getType());
                    compiler->builder->CreateStore(pointerValue, namedVar.Storage);
                    namedVar.Primary = nullptr;
                    namedVar.BaseType = pointerValue->getType();
                }
                if (!namedVar.Storage)
                {
                    LogErrorContext(ctx, "cannot dereference a value without addressable storage; "
                        "bind the pointer to a local before dereferencing it");
                }

                // A C++ class value (e.g. a class-reference member) has pointer storage, so `*r` would load
                // its first field; judge records only (template-substituted pointers lack the Pointer bit).
                // A plain CFlat struct VALUE is judged too: its storage holds the struct itself, so
                // `*s` would load a field as a pointer. Fixed arrays already decayed above.
                const auto& derefTv = namedVar.TypeAndValue;
                const bool plainStructValue = compiler->dataStructures.count(derefTv.TypeName) != 0
                    && derefTv.ConstArraySize == 0 && !derefTv.IsInterface
                    && !derefTv.IsInterfacePointer && !derefTv.IsSimd;
                if (!derefTv.Pointer && !derefTv.ElemPointer && !derefTv.IsFunctionPointer
                    && !derefTv.IsArrayView && derefTv.PointerDepth < 1
                    && (compiler->IsCxxRecord(derefTv.TypeName) || plainStructValue))
                    LogErrorContext(ctx, std::format(
                        "indirection requires a pointer operand ('{}' invalid)",
                        SpellType(*compiler, namedVar.TypeAndValue)));

                if (!namedVar.Storage->getType()->isPointerTy())
                {
                    LogErrorContext(ctx, "Unable to dereference a non-Pointer.");
                }

                // Strip one pointer level from the CFlat type to get the pointee type.
                // For char** (ElemPointer=true): deref gives char* (keep Pointer, clear ElemPointer).
                // For char* (ElemPointer=false): deref gives char (clear Pointer).
                // Depth: `*` removes one level from an already-recorded depth; an unrecorded
                // operand stays unrecorded.
                const auto derefOperandType = namedVar.TypeAndValue;
                // A decayed T[N][M] is a pointer to T[M]. On `*p`, move the first preserved
                // inner dimension back to the pointee's outer array shape before lowering it.
                if (namedVar.TypeAndValue.Pointer && !namedVar.TypeAndValue.ElemPointer
                    && namedVar.TypeAndValue.ConstArraySize == 0
                    && !namedVar.TypeAndValue.ConstInnerDimensions.empty())
                {
                    namedVar.TypeAndValue.ConstArraySize =
                        namedVar.TypeAndValue.ConstInnerDimensions.front();
                    namedVar.TypeAndValue.ConstInnerDimensions.erase(
                        namedVar.TypeAndValue.ConstInnerDimensions.begin());
                }
                if (namedVar.TypeAndValue.PointerDepth >= 1)
                    namedVar.TypeAndValue.PointerDepth--;
                if (namedVar.TypeAndValue.ElemPointer)
                    namedVar.TypeAndValue.ElemPointer = false;
                else
                {
                    namedVar.TypeAndValue.Pointer = false;
                    namedVar.TypeAndValue.IsInterfacePointer = false;
                }
                auto* pointeeType = compiler->GetType(namedVar.TypeAndValue);
                // A TYPELESS pointee (a 'void*') cannot be loaded: LLVM's own alignment query
                // traps on it instead of reporting anything. An unsized NAMED type (an
                // incomplete C record) is left alone - `&*p` on one is legal and the load site
                // has its own "incomplete here" diagnostic.
                if (pointeeType == nullptr || pointeeType->isVoidTy())
                    LogErrorContext(ctx, std::format(
                        "cannot dereference '{}': its pointee has no size, so there is nothing to "
                        "load. Cast it to a typed pointer first (for example '*(int*)p').",
                        SpellType(*compiler, derefOperandType)));
                llvm::Value* baseStorage = namedVar.Storage;
                // A `*p` dereference of an explicitly-moved-null thin pointer local is statically
                // null - reject it, same as the '->'/'.' guard.
                compiler->RecordNullDerefFor(namedVar, ctx->getStart()->getLine(),
                    ctx->getStart()->getCharPositionInLine());
                if (compiler->IsMovedDerefHere(namedVar))
                    LogErrorContext(ctx, compiler->MovedDerefDiagnostic(namedVar));
                llvm::Value* loadedPtr = compiler->CreateLoad(namedVar.Storage);
                std::string pointerPathRoot = namedVar.FieldPathRoot;
                if (pointerPathRoot.empty())
                    pointerPathRoot = !namedVar.CallerName.empty()
                        ? namedVar.CallerName : compiler->FindVariableNameByStorage(baseStorage);
                // The pointee is an lvalue, never a call's sret temporary: a stale one (an earlier
                // `?:` arm, a sibling argument) must not be adopted as its storage below.
                compiler->lastCxxRetTemp_ = nullptr;
                compiler->lastCxxRetValue_ = nullptr;
                // --sanitize=ownership (M1): guard `*p` deref of a moved-from local.
                compiler->EmitOwnDerefGuard(baseStorage, loadedPtr,
                    ctx->getStart()->getLine(), ctx->getStart()->getCharPositionInLine());
                // The deref'd location: loadedPtr is the address; pointeeType is what it holds.
                // Storing it in Storage (not Primary) makes it usable as both lvalue and rvalue.
                namedVar.Storage  = loadedPtr;
                namedVar.Primary  = nullptr;
                namedVar.BaseType = pointeeType;
                namedVar.FieldPathRoot = std::move(pointerPathRoot);
                namedVar.FieldPathHasPointerDereference = true;
                namedVar.FieldPathPointerFrameLocal = PointerRootKindEveryArmLocal(loadedPtr, compiler)
                    && (!unarySourceHasPointerDereference || unarySourcePointerFrameLocal);
                namedVar.FieldPathFromForwardedArrow = false;
                ClearCxxPrvalueProvenance(namedVar);
                // `w[i]` identifies a pointer slot in an array view; after `*w[i]`, the result
                // is the pointed-to lvalue and no longer belongs to the view's element storage.
                namedVar.IsViewElement = false;
            }
            else if (opText == "!")
            {
                const int8_t operandCategory = ctx->castExpression() != nullptr
                    ? CxxArgumentLvalueKind(ctx->castExpression()) : -1;
                const bool constReferenceResult = compiler->CxxConstReceiverKind(namedVar) != 0;
                llvm::Value* operandStorage = (operandCategory == 2
                    || (operandCategory < 0 && namedVar.IsRvalue))
                    && !constReferenceResult && !xvalueOperand ? nullptr : namedVar.Storage;
                auto newValue = this->LoadNamedVariable(namedVar);
                if (auto* overload = TryUnaryOperatorOverload(newValue, "!", ctx, operandStorage, &namedVar,
                                                               constReferenceResult))
                {
                    namedVar.Primary = overload;
                    namedVar.TypeAndValue = compiler->lastCallReturnType;
                    clearConsumedUnarySource(namedVar);
                }
                else
                {
                    EnsureOperatorBoolForValue(newValue);
                    if (newValue != nullptr && compiler->HasOperatorBoolForType(newValue->getType()))
                    {
                        namedVar.Primary = compiler->CreateLogicalNot(
                            compiler->CoerceToBoolCondition(newValue));
                        namedVar.TypeAndValue = {};
                        namedVar.TypeAndValue.TypeName = "bool";
                    }
                    else if (ConvertUnaryOperandViaImplicitConversion(newValue, namedVar, "!", ctx) == 2)
                        return namedVar;
                    else
                    {
                        // An aggregate operand has no truth value; without this LLVM asserts in
                        // CreateNot instead of reporting it.
                        if (newValue != nullptr && !newValue->getType()->isSingleValueType())
                        {
                            LogErrorContext(ctx, std::format(
                                "operator '!' needs a bool, number or pointer operand; '{}' has no operator bool",
                                namedVar.TypeAndValue.TypeName));
                            newValue = compiler->builder->getInt1(false);
                        }
                        // Logical negation yields a plain rvalue bool, whatever the operand was.
                        namedVar.Primary = compiler->CreateLogicalNot(newValue);
                        namedVar.TypeAndValue = {};
                        namedVar.TypeAndValue.TypeName = "bool";
                        namedVar.BaseType = nullptr;
                        namedVar.IsOwning = false;
                        namedVar.BitfieldStorage = nullptr;
                    }
                }
                namedVar.Storage = nullptr;
            }
            else if (opText == "-")
            {
                const int8_t operandCategory = ctx->castExpression() != nullptr
                    ? CxxArgumentLvalueKind(ctx->castExpression()) : -1;
                const bool constReferenceResult = namedVar.TypeAndValue.CxxConstRef()
                    || compiler->IsCxxConstClassObject(namedVar.TypeAndValue)
                    || (namedVar.Storage != nullptr
                        && compiler->IsCxxConstReferent(namedVar.Storage));
                llvm::Value* operandStorage = (operandCategory == 2
                    || (operandCategory < 0 && namedVar.IsRvalue))
                    && !constReferenceResult && !xvalueOperand ? nullptr : namedVar.Storage;
                auto newValue = this->LoadNamedVariable(namedVar);
                // r3: a runtime narrow operand marks the negation (C: the int result of a narrow operand).
                const unsigned unaryMark = newValue == nullptr || llvm::isa<llvm::Constant>(newValue) ? 0u
                    : LLVMBackend::ArithOperandNarrowWidth(NamedArithOperand(namedVar, newValue));
                auto* overload = TryUnaryOperatorOverload(newValue, "-", ctx, operandStorage, &namedVar,
                                                           constReferenceResult);
                if (overload == nullptr
                    && ConvertUnaryOperandViaImplicitConversion(newValue, namedVar, "-", ctx) == 2)
                    return namedVar;
                if (overload != nullptr)
                {
                    namedVar.Primary = overload;
                    namedVar.TypeAndValue = compiler->lastCallReturnType;
                    namedVar.Storage = nullptr;
                    clearConsumedUnarySource(namedVar);
                }
                else if (ReportMissingUnaryStructOperator(ctx, newValue, "-"))
                    return namedVar;
                else if (PromoteBooleanUnaryOperand(compiler, newValue, namedVar)
                         || PromoteNarrowUnaryOperand(compiler, newValue, namedVar))
                {
                    namedVar.Primary = compiler->CreateNeg(newValue);
                    namedVar.Storage = nullptr;
                }
                else
                {
                    // Fold negation of signed integer constants into the smallest fitting type
                    // (-32768 fits i16); an unsigned operand instead wraps at its own width (-5u).
                    if (namedVar.TypeAndValue.IsUnsignedInteger() != -1)
                    {
                        namedVar.Primary = compiler->CreateNeg(newValue);
                    }
                    else if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(newValue);
                             ci != nullptr && ci->getBitWidth() <= 64)
                    {
                        int64_t neg = -(int64_t)ci->getSExtValue();
                        if (neg >= std::numeric_limits<int8_t>::min() && neg <= std::numeric_limits<int8_t>::max())
                            namedVar.Primary = compiler->builder->getInt8((int8_t)neg);
                        else if (neg >= std::numeric_limits<int16_t>::min() && neg <= std::numeric_limits<int16_t>::max())
                            namedVar.Primary = compiler->builder->getInt16((int16_t)neg);
                        else if (neg >= (int64_t)std::numeric_limits<int32_t>::min() && neg <= (int64_t)std::numeric_limits<int32_t>::max())
                            namedVar.Primary = compiler->builder->getInt32((int32_t)neg);
                        else
                            namedVar.Primary = compiler->builder->getInt64(neg);
                    }
                    else
                    {
                        namedVar.Primary = compiler->CreateNeg(newValue);
                    }
                    namedVar.Storage = nullptr;
                }
                namedVar.ArithNarrowWidth = static_cast<uint8_t>(unaryMark);
            }
            else if (opText == "+")
            {
                const int8_t operandCategory = ctx->castExpression() != nullptr
                    ? CxxArgumentLvalueKind(ctx->castExpression()) : -1;
                const bool constReferenceResult = namedVar.TypeAndValue.CxxConstRef()
                    || compiler->IsCxxConstClassObject(namedVar.TypeAndValue)
                    || (namedVar.Storage != nullptr
                        && compiler->IsCxxConstReferent(namedVar.Storage));
                llvm::Value* operandStorage = (operandCategory == 2
                    || (operandCategory < 0 && namedVar.IsRvalue))
                    && !constReferenceResult && !xvalueOperand ? nullptr : namedVar.Storage;
                auto newValue = this->LoadNamedVariable(namedVar);
                auto* overload = TryUnaryOperatorOverload(newValue, "+", ctx, operandStorage, &namedVar,
                                                           constReferenceResult);
                if (overload == nullptr
                    && ConvertUnaryOperandViaImplicitConversion(newValue, namedVar, "+", ctx) == 2)
                    return namedVar;
                if (overload != nullptr)
                {
                    namedVar.Primary = overload;
                    namedVar.TypeAndValue = compiler->lastCallReturnType;
                    clearConsumedUnarySource(namedVar);
                }
                else if (ReportMissingUnaryStructOperator(ctx, newValue, "+"))
                    return namedVar;
                else
                {
                    PromoteBooleanUnaryOperand(compiler, newValue, namedVar);
                    // unary + is a no-op: just load the value
                    namedVar.Primary = newValue;
                    // ...but a new value: `+ZERO` is no longer the macro's literal.
                    namedVar.IsCxxMacroGlobal = false;
                    namedVar.IsCxxNullIntegerMacro = false;
                }
                namedVar.Storage = nullptr;
            }
            else if (opText == "~")
            {
                const int8_t operandCategory = ctx->castExpression() != nullptr
                    ? CxxArgumentLvalueKind(ctx->castExpression()) : -1;
                const bool constReferenceResult = namedVar.TypeAndValue.CxxConstRef()
                    || compiler->IsCxxConstClassObject(namedVar.TypeAndValue)
                    || (namedVar.Storage != nullptr
                        && compiler->IsCxxConstReferent(namedVar.Storage));
                llvm::Value* operandStorage = (operandCategory == 2
                    || (operandCategory < 0 && namedVar.IsRvalue))
                    && !constReferenceResult && !xvalueOperand ? nullptr : namedVar.Storage;
                auto newValue = this->LoadNamedVariable(namedVar);
                const unsigned unaryMark = newValue == nullptr || llvm::isa<llvm::Constant>(newValue) ? 0u
                    : LLVMBackend::ArithOperandNarrowWidth(NamedArithOperand(namedVar, newValue));
                auto* overload = TryUnaryOperatorOverload(newValue, "~", ctx, operandStorage, &namedVar,
                                                           constReferenceResult);
                if (overload == nullptr
                    && ConvertUnaryOperandViaImplicitConversion(newValue, namedVar, "~", ctx) == 2)
                    return namedVar;
                if (overload != nullptr)
                {
                    namedVar.Primary = overload;
                    namedVar.TypeAndValue = compiler->lastCallReturnType;
                    clearConsumedUnarySource(namedVar);
                }
                else if (ReportMissingUnaryStructOperator(ctx, newValue, "~"))
                    return namedVar;
                else
                {
                    if (!PromoteBooleanUnaryOperand(compiler, newValue, namedVar))
                        PromoteNarrowUnaryOperand(compiler, newValue, namedVar);
                    namedVar.Primary = compiler->CreateNot(newValue);
                }
                namedVar.Storage = nullptr;
                namedVar.ArithNarrowWidth = static_cast<uint8_t>(unaryMark);
            }
            else
            {
                LogErrorContext(ctx, std::format("{} operator is not yet implemented.", opText));
                return namedVar;
            }

            PrepareAliasCallResult(ctx, namedVar);
            // A unary operator overload declared 'void' lands here with the same value-less
            // result a direct call does - same ruling, same wording.
            DiagnoseVoidResultConsumed(ctx, namedVar, use, std::format("'operator{}'", opText));
            return namedVar;
        }
        else if (auto* typeNameCtx = ctx->typeName())
        {
            // Handle sizeof(typeName) or alignof(typeName)
            std::string text = ctx->getText();

            // Check if this is a sizeof or alignof expression
            bool isSizeof = text.find("sizeof(") != std::string::npos;
            bool isAlignof = text.find("alignof(") != std::string::npos;

            if (isSizeof || isAlignof)
            {
                if (isSizeof || isAlignof)
                {
                    auto* bareName = BareTypeNameIdentifier(typeNameCtx);
                    if (!ctx->Sizeof().empty() && bareName != nullptr
                        && typeNameCtx->abstractDeclarator() == nullptr
                        && !NamesVisibleVariable(compiler, bareName->getText())
                        && !compiler->IsKnownTypeName(bareName->getText())
                        && !compiler->AnyGenericTypeTemplateNamed(bareName->getText()))
                    {
                        const std::string genericBase = compiler->ResolveGenericFunctionBase(
                            bareName->getText());
                        if (genericFunctionTemplates.count(genericBase) != 0)
                            LogErrorContext(ctx, std::format(
                                "generic function '{}' has no size", bareName->getText()));
                    }
                    auto* specList = typeNameCtx->specifierQualifierList();
                    bool resolvesAsType = false;
                    if (specList != nullptr)
                    {
                        const std::string spelling = specList->getText();
                        // A visible local, parameter or this-field named by the first component wins,
                        // as in C++: `sizeof(X.Y)` is then a field read, not a scoped type.
                        const std::string sizeofHead = spelling.substr(0, spelling.find('.'));
                        const bool sizeofHeadIsVariable = spelling.find('.') != std::string::npos
                            && (compiler->FindLiveNamedVariable(sizeofHead) != nullptr
                                || NamesVisibleVariable(compiler, sizeofHead)
                                || compiler->FindImplicitThisField(sizeofHead, nullptr, nullptr) != nullptr);
                        resolvesAsType = spelling.find('<') == std::string::npos && !sizeofHeadIsVariable
                            && (compiler->IsKnownTypeName(spelling)
                                || compiler->IsTypeArgTypeKey(spelling)
                                || activeTypeSubstitutions.count(spelling) != 0
                                || activePackSubstitutions.count(spelling) != 0);
                        if (!resolvesAsType && bareName != nullptr
                            && specList->getText() == bareName->getText()
                            && typeNameCtx->abstractDeclarator() == nullptr
                            && NamesVisibleVariable(compiler, bareName->getText()))
                            resolvesAsType = true;
                        // A bare identifier in a declaration type position can be forward-declared
                        // or hidden behind a conditional scan; preserve type parsing unless shadowed.
                        if (!resolvesAsType && bareName != nullptr
                            && specList->getText() == bareName->getText()
                            && typeNameCtx->getText() == bareName->getText()
                            && typeNameCtx->abstractDeclarator() == nullptr
                            && !NamesVisibleVariable(compiler, bareName->getText()))
                            resolvesAsType = true;
                        // A variable head claims no type spelling below, as in C: sizeof(Z.B) reads the field.
                        for (auto* typeSpec : sizeofHeadIsVariable ? std::vector<CFlatParser::TypeSpecifierContext*>{}
                                                                   : specList->typeSpecifier())
                        {
                            static const std::unordered_set<std::string> primitiveWords = {
                                "void", "char", "i8", "u8", "c8", "short", "i16", "u16", "c16",
                                "int", "i32", "u32", "uint", "c32", "long", "ulong", "i64", "u64",
                                "longlong", "ulonglong", "i128", "u128", "wchar", "float", "double", "longdouble", "bool",
                                "signed", "unsigned" };
                            if (primitiveWords.count(typeSpec->getText()) != 0
                                || compiler->IsTypeArgTypeKey(typeSpec->getText())
                                || activeTypeSubstitutions.count(typeSpec->getText()) != 0
                                || activePackSubstitutions.count(typeSpec->getText()) != 0)
                            {
                                resolvesAsType = true;
                                break;
                            }
                            if (typeSpec->simdTypeSpecifier() != nullptr)
                            {
                                resolvesAsType = true;
                                break;
                            }
                            // `Spec<args>.name`: a type exactly when the lookup says so; a static
                            // value member (`sizeof(S<int>.value)`) reparses as an expression below.
                            if (auto* memberType = typeSpec->memberTypeSpecifier())
                            {
                                resolvesAsType = MemberTypeSpecifierNamesType(memberType);
                                if (resolvesAsType) break;
                                continue;
                            }
                            std::string genericBase;
                            auto* genericSpec = GenericSpecOf(typeSpec, genericBase);
                            if (genericSpec == nullptr) continue;
                            const auto scopes = EnclosingAggregateScopesFromContext(compiler, typeSpec);
                            const std::string resolvedBase = ResolveGenericTemplateBaseAtScope(
                                compiler, genericBase, scopes, typeSpec, genericBase);
                            const bool scopeRejected = resolvedBase.empty();
                            resolvesAsType = !scopeRejected
                                && (compiler->IsGenericTemplateKey(resolvedBase)
                                    || compiler->AnyGenericTypeTemplateNamed(resolvedBase)
                                    || genericBase == "function" || genericBase == "Lambda");
                            if (!resolvesAsType && !scopeRejected)
                            {
                                std::vector<std::string> typeArgs;
                                for (auto* entry : genericSpec->typeParameterList()->typeParameterEntry())
                                    typeArgs.push_back(ResolveTypeArgEntry(entry));
                                std::string candidateBase = genericBase;
                                compiler->ResolveGenericAliasSpelling(candidateBase, typeArgs);
                                compiler->CompleteCxxTemplateTypeArgs(candidateBase, typeArgs);
                                std::string candidate = MangledGenericName(candidateBase, typeArgs);
                                std::string cxxError;
                                resolvesAsType = compiler->TryRequestCxxType(
                                    genericBase, typeArgs, candidate, cxxError);
                            }
                            if (resolvesAsType) break;
                        }
                        if (!resolvesAsType && spelling.find('.') != std::string::npos && !sizeofHeadIsVariable)
                        {
                            std::string candidate = compiler->ResolveQualifiedName(spelling, true);
                            std::vector<std::string> aggregateScopes;
                            for (auto it = structScopeStack.rbegin(); it != structScopeStack.rend(); ++it)
                                aggregateScopes.push_back(*it);
                            if (aggregateScopes.empty())
                                aggregateScopes = EnclosingAggregateNamesFromContext(compiler, typeNameCtx);
                            CFlatParser::TypeSpecifierContext* contextualTypeSpec = nullptr;
                            if (auto* specifiers = typeNameCtx->specifierQualifierList())
                                for (auto* specifier : specifiers->typeSpecifier())
                                    if (IsUnparameterizedNamedTypeSpecifier(specifier))
                                    {
                                        contextualTypeSpec = specifier;
                                        break;
                                    }
                            const std::string contextual = ResolveContextualTypeName(compiler, spelling,
                                candidate, contextualTypeSpec, aggregateScopes,
                                NamespaceNameFromContext(compiler, typeNameCtx));
                            if (!contextual.empty())
                            {
                                candidate = contextual;
                                resolvesAsType = true;
                            }
                            else
                                resolvesAsType = compiler->IsKnownTypeName(candidate)
                                    || compiler->IsTypeArgTypeKey(candidate);
                            const size_t scope = candidate.rfind('.');
                            if (!resolvesAsType && scope != std::string::npos
                                && compiler->IsCxxNamespacePath(candidate.substr(0, scope)))
                            {
                                std::string cxxError;
                                resolvesAsType = compiler->TryRequestCxxType(
                                    candidate, {}, candidate, cxxError);
                            }
                        }
                    }

                    const std::string written = typeNameCtx->getText();
                    const auto firstParen = written.find('(');
                    int angleDepthAtParen = 0;
                    if (firstParen != std::string::npos)
                        for (size_t i = 0; i < firstParen; i++)
                        {
                            if (written[i] == '<') angleDepthAtParen++;
                            else if (written[i] == '>') angleDepthAtParen--;
                        }
                    if (firstParen != std::string::npos && angleDepthAtParen <= 0)
                        resolvesAsType = false;
                    const auto firstAngle = written.find('<');
                    const auto closeAngle = written.rfind('>');
                    const bool ambiguousTemplateComparison = firstAngle != std::string::npos
                        && written.find(',', firstAngle) != std::string::npos
                        && closeAngle != std::string::npos && closeAngle + 1 < written.size()
                        && (std::isalnum(static_cast<unsigned char>(written[closeAngle + 1]))
                            || written[closeAngle + 1] == '_');
                    // Keep the diagnosed `sizeof(a<b,c>d)` case on the type diagnostic path;
                    // its earlier expression fallback produced invalid IR.
                    resolvesAsType = resolvesAsType || ambiguousTemplateComparison;

                    // The grammar can read an expression such as `s.field` or `arr[i]` as a
                    // typeName. Reparse only unresolved spellings as an expression; type probes
                    // above are registry checks and never emit diagnostics.
                    if (!resolvesAsType)
                    {
                        auto* start = typeNameCtx->getStart();
                        auto* stop = typeNameCtx->getStop();
                        auto* source = start != nullptr ? start->getInputStream() : nullptr;
                        std::string expressionText = source != nullptr && stop != nullptr
                            ? source->getText(antlr4::misc::Interval(
                                start->getStartIndex(), stop->getStopIndex()))
                            : typeNameCtx->getText();
                        antlr4::ANTLRInputStream exprInput(expressionText);
                        CFlatLexer exprLexer(&exprInput);
                        // Reparsed tokens keep the operand's source position for diagnostics.
                        if (start != nullptr)
                        {
                            exprLexer.setLine(start->getLine());
                            exprLexer.setCharPositionInLine(start->getCharPositionInLine());
                        }
                        antlr4::CommonTokenStream exprTokens(&exprLexer);
                        CFlatParser exprParser(&exprTokens);
                        exprParser.removeErrorListeners();
                        auto localizeMessage = compiler->MakeDiagnosticLocalizer();
                        exprParser.setErrorHandler(std::make_shared<CFlatErrorStrategy>(localizeMessage));
                        auto* exprCtx = exprParser.unaryExpression();
                        NormalizeForInitTree(exprParser, exprCtx);
                        if (exprParser.getNumberOfSyntaxErrors() == 0
                            && exprParser.getCurrentToken()->getType() == cflat::kTokenEOF)
                        {
                            auto operand = EvaluateOperandTypeOnly([&]() {
                                FlexibleArrayKeepScope keep(flexibleArrayKeep_,
                                    DirectOperandPostfixes(exprCtx));
                                return ParseUnaryExpressionImpl(exprCtx, ResultUse::Value, 0);
                            });
                            // alignof of a flexible member is its element alignment, as in C.
                            if (isSizeof && IsZeroLengthArrayMemberOperand(operand.Type))
                            {
                                LLVMBackend::NamedVariable zero;
                                zero.Primary = llvm::ConstantInt::get(
                                    llvm::Type::getInt64Ty(*compiler->context), 0);
                                zero.TypeAndValue.TypeName = "i64";
                                return zero;
                            }
                            if (std::string error = FlexibleArrayMemberLengthError(operand.Type);
                                isSizeof && !error.empty())
                            {
                                LogErrorContext(ctx, error);
                                return {};
                            }
                            if (!operand.HasValue)
                            {
                                LogErrorContext(ctx, "sizeof/alignof: could not determine type");
                                return {};
                            }
                            auto typeValue = operand.Type;
                            if ((typeValue.TypeName == "auto" || typeValue.TypeName.empty())
                                && operand.ValueType != nullptr)
                            {
                                typeValue = {};
                                typeValue.Pointer = operand.ValueType->isPointerTy();
                                typeValue.TypeName = typeValue.Pointer
                                    ? "void" : LLVMTypeToTypeName(operand.ValueType);
                                if (operand.ValueType->isVoidTy()) typeValue.TypeName.clear();
                            }
                            llvm::Type* llvmType = typeValue.TypeName.empty()
                                ? nullptr : compiler->GetType(typeValue, nullptr, true);
                            if (llvmType == nullptr || llvmType->isVoidTy())
                                llvmType = operand.ValueType;
                            if (llvmType == nullptr || llvmType->isVoidTy())
                            {
                                LogErrorContext(ctx, "sizeof/alignof: could not resolve type to LLVM type");
                                return {};
                            }
                            return SizeOrAlignOfType(typeValue, llvmType, isSizeof);
                        }
                    }
                }

                // A bare name that is a visible variable measures the variable, even when it also
                // names a type: in C an ordinary identifier in scope hides a type name (so typeof).
                auto* bareName = BareTypeNameIdentifier(typeNameCtx);
                auto typeValue = bareName != nullptr && NamesVisibleVariable(compiler, bareName->getText())
                    ? VisibleVariableType(compiler, bareName->getText())
                    : ParseTypeName(typeNameCtx, true, ctx);
                if (bareName != nullptr && typeValue.TypeName == "auto")
                    return SizeOrAlignOfAutoVariable(bareName->getText(), isSizeof);
                if (typeValue.TypeName.empty())
                {
                    LogErrorContext(ctx, "sizeof/alignof: could not determine type");
                    return {};
                }

                // sizeof(T) where T is a pack param returns the element count, not byte size
                if (isSizeof)
                {
                    auto packIt = activePackSubstitutions.find(typeNameCtx->getText());
                    if (packIt != activePackSubstitutions.end())
                    {
                        LLVMBackend::NamedVariable namedVar;
                        namedVar.Primary = llvm::ConstantInt::get(
                            llvm::Type::getInt32Ty(*compiler->context),
                            (int)packIt->second.size());
                        namedVar.TypeAndValue.TypeName = "int";
                        return namedVar;
                    }
                }

                std::vector<uint64_t> arrayDims;
                auto peelArrayDims = [](std::string& name, std::vector<uint64_t>& dims) {
                    auto first = name.find('[');
                    if (first == std::string::npos) return;
                    std::string base = name.substr(0, first);
                    size_t pos = first;
                    while (pos < name.size())
                    {
                        if (name[pos] != '[') return;
                        size_t close = name.find(']', pos + 1);
                        if (close == std::string::npos || close == pos + 1) return;
                        std::string text = name.substr(pos + 1, close - pos - 1);
                        size_t consumed = 0;
                        unsigned long long dim = 0;
                        try { dim = std::stoull(text, &consumed); }
                        catch (...) { consumed = 0; }
                        if (consumed != text.size() || dim == 0) return;
                        dims.push_back(static_cast<uint64_t>(dim));
                        pos = close + 1;
                    }
                    if (pos == name.size()) name = base;
                    else dims.clear();
                };
                auto elementTV = typeValue;
                peelArrayDims(elementTV.TypeName, arrayDims);
                auto* dimSpec = typeNameCtx->abstractDeclarator() != nullptr
                    ? ArrayDimsOf(typeNameCtx->abstractDeclarator()) : nullptr;
                if (dimSpec != nullptr && !dimSpec->assignmentExpression().empty())
                {
                    auto dimExprs = dimSpec->assignmentExpression();
                    for (auto* dimExpr : dimExprs)
                    {
                        // Same folder and range checks as a declared dimension (macros, enumerators).
                        // A folded value that is zero or oversized was already diagnosed there.
                        auto* dimConst = TryFoldArrayDim(dimExpr, false);
                        if (dimConst == nullptr)
                        {
                            auto* dimValue = ParseAssignmentExpression(dimExpr);
                            dimConst = dimValue != nullptr
                                ? llvm::dyn_cast<llvm::ConstantInt>(dimValue) : nullptr;
                            if (dimConst == nullptr || dimConst->isZero())
                            {
                                LogErrorContext(dimExpr, "sizeof: array dimensions must be positive constants");
                                return {};
                            }
                        }
                        else if (dimConst->isZero() || dimConst->getZExtValue() > (uint64_t)INT32_MAX)
                            return {};
                        arrayDims.push_back(dimConst->getZExtValue());
                    }
                }
                llvm::Type* llvmType = nullptr;
                if (!arrayDims.empty())
                {
                    elementTV.IsArrayView = false;
                    llvmType = compiler->GetType(elementTV, nullptr, true);
                    for (auto it = arrayDims.rbegin(); it != arrayDims.rend() && llvmType != nullptr; ++it)
                        llvmType = llvm::ArrayType::get(llvmType, *it);
                }
                else
                {
                    if (!typeValue.Pointer && !compiler->IsKnownTypeName(elementTV.TypeName))
                    {
                        auto varNV = compiler->GetLocalVariable(typeNameCtx->getText());
                        if (varNV.Storage == nullptr && varNV.Primary == nullptr)
                            varNV = compiler->GetGlobalVariableNV(typeNameCtx->getText());
                        if (varNV.Storage != nullptr || varNV.Primary != nullptr)
                        {
                            typeValue = varNV.TypeAndValue;
                            elementTV = typeValue;
                        }
                    }
                    // `sizeof(a<b,c>d)` parses as a typeName and is then mangled ('a__b__c'),
                    // so the reader is told about a type they never wrote. Name the spelling.
                    if (!compiler->IsKnownTypeName(elementTV.TypeName))
                    {
                        if (std::string refusal = compiler->GetCxxBindingRefusal(
                                typeNameCtx->getText());
                            !refusal.empty())
                            LogErrorContext(ctx, refusal);
                        std::string written = typeNameCtx->getText();
                        size_t close = written.rfind('>');
                        if (close != std::string::npos && close + 1 < written.size()
                            && (std::isalnum(static_cast<unsigned char>(written[close + 1]))
                                || written[close + 1] == '_'))
                            LogErrorContext(ctx, std::format(
                                "'{}' is not a type name; the operand of '{}' is read as a TYPE here, "
                                "so a comparison or tuple expression is not accepted - bind it to a "
                                "variable first and measure that", written, isSizeof ? "sizeof" : "alignof"));
                    }
                    llvmType = compiler->GetType(elementTV, nullptr, true);
                }
                compiler->TouchWaitingLayout(llvmType);
                if (!llvmType)
                {
                    LogErrorContext(ctx, "sizeof/alignof: could not resolve type to LLVM type");
                    return {};
                }
                if (!llvmType->isSized())
                {
                    LogErrorContext(ctx, std::format("sizeof/alignof: type '{}' is incomplete",
                        typeValue.TypeName.empty() ? "<unknown>" : typeValue.TypeName));
                    return {};
                }

                // Get the size or alignment value, honoring any struct-level alignas.
                uint64_t effAlign = typeValue.Pointer
                    ? 0
                    : compiler->GetEffectiveAlignmentForType(typeValue.TypeName, llvmType);
                llvm::Value* result;
                if (isSizeof)
                {
                    if (effAlign > 1)
                    {
                        uint64_t paddedSize = compiler->GetEffectiveAllocSize(llvmType, effAlign);
                        result = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*compiler->context), paddedSize);
                    }
                    else
                    {
                        result = compiler->GetTypeSizeBytes(llvmType);
                    }
                }
                else
                {
                    if (effAlign > 1)
                        result = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*compiler->context), effAlign);
                    else
                        result = compiler->GetTypeAlignBytes(llvmType);
                }

                // Return as a named variable with i64 type
                LLVMBackend::NamedVariable namedVar;
                namedVar.Primary = result;
                namedVar.TypeAndValue.TypeName = "i64";
                namedVar.Storage = nullptr;
                return namedVar;
            }
        }

        LogErrorContext(ctx, "Unary expression has no recognized form.");
        return {};
    }

