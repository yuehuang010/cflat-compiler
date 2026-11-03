#include "MainListener.h"
#include <llvm/Support/SaveAndRestore.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicInst.h>
#include "MainListener_ExpressionsShared.h"




// Name a direct call with a borrowed parameter return, keeping proven owning arguments accepted.
std::string MainListener::BorrowedParameterCallName(
    const LLVMBackend::NamedVariable& source, llvm::Value* value)
{
    auto* compiler = compilerLLVM;
    if (UniquePointerSourceIsOwned(source) || source.IsBorrowed || source.IsAliasBorrow
        || source.IsAliasReturnBorrow || source.BorrowedUniqueFieldViaCall
        || compiler->lastCallReturnsOwned || compiler->IsOwningPtrTempValue(value)
        || compiler->IsMovedOutPtrValue(value) || compiler->IsOwnedNewTemp(value))
        return {};
    auto* call = llvm::dyn_cast_or_null<llvm::CallBase>(value);
    const llvm::Function* callee = call != nullptr ? call->getCalledFunction() : nullptr;
    const auto* symbol = callee == nullptr ? nullptr : compiler->FindSymbolForFunction(callee);
    if (symbol == nullptr || symbol->IsCxx || !symbol->ReturnType.Pointer
        || !symbol->ReturnsParameterBorrow)
        return {};
    const int parameterIndex = symbol->ReturnsSingleParameterIndex;
    if (parameterIndex >= 0 && static_cast<unsigned>(parameterIndex) < call->arg_size())
    {
        llvm::Value* returnedArgument = call->getArgOperand(static_cast<unsigned>(parameterIndex));
        if (compiler->IsOwningPtrTempValue(returnedArgument)
            || compiler->IsMovedOutPtrValue(returnedArgument)
            || compiler->IsOwnedNewTemp(returnedArgument))
            return {};
    }
    return symbol->SourceName;
}



/*
 * The CFlat name of C's ptrdiff_t on the target: the target long when long is 64-bit (LP64),
 * else the 64-bit long long spelled i64 (LLP64). Both are signed and 64-bit.
 */

/*
 * The type name of a built-in arithmetic result from its operand names, using the usual
 * arithmetic conversions. `long + 1` stays `long` even when LLVM shares its type with `i64`.
 */
/*
 * r3 call-argument mark of a built-in arithmetic chain. A chain carries the mark of the value it was
 * computed for; any other value (a pointer step, an overload result) is unmarked and not a literal.
 */




// The C++ type of an operand spelled as one literal (`1e3`, `-2`, `(1.0f)`): its token span is
// checked first so a long operand never has its text built.

/*
 * A slot armed by an enclosing declarator / return / array element (`T b = (a = 9);`) while an
 * assignment is compiled. The operand parse never sees it, so no RHS call constructs into it.
 * Only the assignment's own C++ operator call is shown the slot (EmitOperatorCall): a by-value
 * (prvalue) result constructs straight into it, a `T&` result leaves it, and the caller then
 * copies from the destination lvalue. The LAST emission decides, since a speculative call can be
 * discarded and re-emitted. Scopes nest per thread; a generic instantiated mid-expression pushes
 * and pops its own.
 */

/*
 * The compound assignment operators, each paired with the binary operator it falls back to when
 * no compound overload exists. One table drives the assignment path's fallback, the C++
 * "this operator writes through its receiver" test, and the operator-form diagnostics.
 */

// The CFlat spelling of a machine type, for an operand that reaches operator lookup as a bare
// llvm::Value (a literal or a temporary). Empty when the type is not a plain scalar.
// Expression text that is one string literal, through transparent parentheses: `"A"`, `("A")`.
static bool IsStringLiteralText(std::string_view text)
{
        while (text.size() >= 2 && text.front() == '(' && text.back() == ')')
        {
            int depth = 0;
            bool inString = false;
            bool wraps = true;
            for (size_t i = 0; i + 1 < text.size(); ++i)
            {
                const char c = text[i];
                if (inString) { if (c == '\\') ++i; else if (c == '"') inString = false; continue; }
                if (c == '"') inString = true;
                else if (c == '(') ++depth;
                else if (c == ')' && --depth == 0) { wraps = false; break; }
            }
            if (!wraps) break;
            text = text.substr(1, text.size() - 2);
        }
        return text.size() >= 2 && text.front() == '"' && text.back() == '"';
}

// A CFlat arithmetic type name (signed and unsigned spelled apart; the name, not the width, says which).


/*
 * The four relational operators C++20 REWRITES from operator<=>, each with the ordering values
 * that make it true. Every comparison category encodes less = -1, equivalent = 0, greater = 1;
 * anything else is partial_ordering::unordered (libc++ spells it -127, libstdc++ 2), for which
 * all four relations are false - which is why these are closed value ranges and not sign tests.
 */

/*
 * The reversed rewrite of `x @ y` is `0 @ (y <=> x)` - the same relation with 0 on the left -
 * so the ordering must be read against the MIRROR relation's range: `x > y` reversed is
 * `(y <=> x) < 0`, which is the "<" range. Returns the mirrored spelling.
 */


// True when a `?:` arm is an imported macro constant, or a `?:` joining one, through any
// parentheses. Each leaf is resolved by symbol (IsCxxMacroGlobalName), so a local of the same
// name is no macro and nested joins compose from the arms, not from the text.
static bool IsCxxMacroPrvalueArm(LLVMBackend* compiler, antlr4::tree::ParseTree* node)
{
    if (node == nullptr) return false;
    if (auto* cond = dynamic_cast<CFlatParser::ConditionalExpressionContext*>(node);
        cond != nullptr && cond->Question() != nullptr)
    {
        return IsCxxMacroPrvalueArm(compiler, cond->expression())
            || IsCxxMacroPrvalueArm(compiler, cond->conditionalExpression());
    }
    auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(node);
    if (rule == nullptr) return false;
    if (rule->children.size() == 3 && rule->children[0]->getText() == "("
        && rule->children[2]->getText() == ")")
        return IsCxxMacroPrvalueArm(compiler, rule->children[1]);
    if (rule->children.size() != 1) return false;
    antlr4::tree::ParseTree* child = rule->children[0];
    if (dynamic_cast<antlr4::tree::TerminalNode*>(child) != nullptr)
    {
        const std::string text = child->getText();
        if (text.empty() || std::isdigit(static_cast<unsigned char>(text.front()))) return false;
        for (char c : text)
        {
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
        }
        return compiler->IsCxxMacroGlobalName(text);
    }
    return IsCxxMacroPrvalueArm(compiler, child);
}





// True when the ternary is the operand of a unary `&`, through any parentheses and through the
// arms of an enclosing `?:` (`&(c ? (d ? a : b) : e)` names the selected object at every level).

// True when the ternary is the receiver of a `.` member access, through any parentheses.
// An arm of an enclosing `?:` passes the receiver on (`(c ? a : (u ? x : y)).f`); its condition does not.



bool MainListener::AllowBondedClosureFieldStore(
        LLVMBackend* compiler,
        LLVMBackend::NamedVariable& destination,
        llvm::Value* fieldStorage,
        const std::vector<std::string>& sources,
        antlr4::ParserRuleContext* /*errCtx*/) {
        if (destination.FieldPathThroughPointer || sources.empty()) return false;

        for (const auto& source : sources)
        {
            for (const auto& frame : std::ranges::reverse_view(compiler->stackNamedVariable))
            {
                auto it = frame.namedVariable.find(source);
                if (it == frame.namedVariable.end()) continue;
                if (it->second.ContainsBondedClosure) return false;
                break;
            }
        }

        std::string holderName = destination.FieldPathRoot;
        const LLVMBackend::NamedVariable* holder = nullptr;
        if (!holderName.empty())
        {
            for (const auto& frame : std::ranges::reverse_view(compiler->stackNamedVariable))
            {
                auto it = frame.namedVariable.find(holderName);
                if (it != frame.namedVariable.end())
                {
                    holder = &it->second;
                    break;
                }
            }
        }

        // Brace initialization passes the holder storage directly, so recover its root alloca.
        if (holder == nullptr && fieldStorage != nullptr)
        {
            llvm::Value* rootStorage = fieldStorage;
            while (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(rootStorage))
                rootStorage = gep->getPointerOperand();
            while (auto* bitcast = llvm::dyn_cast<llvm::BitCastInst>(rootStorage))
                rootStorage = bitcast->getOperand(0);
            if (!llvm::isa<llvm::AllocaInst>(rootStorage)) return false;
            holderName = compiler->FindVariableNameByStorage(rootStorage);
            if (holderName.empty()) return false;
            for (const auto& frame : std::ranges::reverse_view(compiler->stackNamedVariable))
            {
                auto it = frame.namedVariable.find(holderName);
                if (it != frame.namedVariable.end())
                {
                    holder = &it->second;
                    break;
                }
            }
        }

        if (holder == nullptr || holder->Storage == nullptr
            || !llvm::isa<llvm::AllocaInst>(holder->Storage)
            || holder->TypeAndValue.Pointer)
            return false;

        const size_t holderDepth = compiler->FindVariableScopeDepth(holderName);
        if (holderDepth == SIZE_MAX) return false;
        for (const auto& source : sources)
        {
            const size_t sourceDepth = compiler->FindVariableScopeDepth(source);
            if (sourceDepth == SIZE_MAX || holderDepth < sourceDepth) return false;
            if (holderDepth == sourceDepth)
            {
                const LLVMBackend::NamedVariable* sourceNV = nullptr;
                for (const auto& frame : std::ranges::reverse_view(compiler->stackNamedVariable))
                {
                    auto it = frame.namedVariable.find(source);
                    if (it != frame.namedVariable.end())
                    {
                        sourceNV = &it->second;
                        break;
                    }
                    auto argIt = frame.functionArgument.find(source);
                    if (argIt != frame.functionArgument.end())
                    {
                        sourceNV = &argIt->second;
                        break;
                    }
                }
                if (holder->DeclSequence == 0 || sourceNV == nullptr
                    || sourceNV->DeclSequence == 0 || holder->DeclSequence <= sourceNV->DeclSequence)
                    return false;
            }
        }

        // Store the fact on the live binding so later moves, returns, and calls can reject it.
        for (auto& frame : std::ranges::reverse_view(compiler->stackNamedVariable))
        {
            auto it = frame.namedVariable.find(holderName);
            if (it == frame.namedVariable.end()) continue;
            it->second.ContainsBondedClosure = true;
            for (const auto& source : sources)
                if (std::find(it->second.BondedSources.begin(), it->second.BondedSources.end(), source)
                    == it->second.BondedSources.end())
                    it->second.BondedSources.push_back(source);
            return true;
        }
        return false;
    }

bool MainListener::IsBondedClosureContainer(
        LLVMBackend* compiler, const LLVMBackend::DeclTypeAndValue& type) {
        std::unordered_set<std::string> visiting;
        auto contains = [&](auto&& self, const LLVMBackend::DeclTypeAndValue& current) -> bool {
            if (current.Pointer || current.IsFunctionPointer || current.TypeName.empty()
                || compiler->GetEncodedClosureType(current.TypeName) != nullptr)
                return false;
            if (!visiting.insert(current.TypeName).second) return false;
            auto it = compiler->dataStructures.find(current.TypeName);
            if (it == compiler->dataStructures.end())
            {
                visiting.erase(current.TypeName);
                return false;
            }
            for (const auto& field : it->second.StructFields)
            {
                if (field.IsFunctionPointer || compiler->GetEncodedClosureType(field.TypeName) != nullptr
                    || self(self, field))
                {
                    visiting.erase(current.TypeName);
                    return true;
                }
            }
            visiting.erase(current.TypeName);
            return false;
        };
        return contains(contains, type);
    }

// The expression is a whole call argument, possibly parenthesized: `f(c ? a : b)`, `f((c ? a : b))`.
bool MainListener::IsCallArgumentThroughParens(antlr4::ParserRuleContext* ctx)
{
    for (auto* node = ctx->parent; node != nullptr; )
    {
        if (dynamic_cast<CFlatParser::ArgumentNamedExpressionContext*>(node) != nullptr)
            return true;
        auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(node);
        if (rule == nullptr) return false;
        const bool parens = rule->children.size() == 3 && rule->children[0]->getText() == "("
            && rule->children[2]->getText() == ")";
        if (!parens && rule->children.size() != 1) return false;
        node = rule->parent;
    }
    return false;
}

// A parenthesized conditional that is an operand of an operator a C++ class can overload, as in
// `(k ? C(1) : C(2)) == b` or `-(k ? C(1) : C(2))`. Structural only: the operand becomes the
// by-value operator parameter or the receiver. `&&` and `||` never route to overloads.

bool MainListener::IsWholeCxxCallArgumentShape(
        antlr4::ParserRuleContext* ctx, const LLVMBackend::TypeAndValue& expected) const
{
        if (ctx == nullptr) return false;
        if (IsDirectCallArgument(ctx) || IsCallArgumentThroughParens(ctx)) return true;
        if (expected.TypeName.empty() || expected.Pointer || expected.PointerDepth > 0
            || cxxCallArgumentExpectedType_.TypeName.empty()
            || cxxCallArgumentExpectedType_.Pointer || cxxCallArgumentExpectedType_.PointerDepth > 0
            || compilerLLVM->GetType(expected) != compilerLLVM->GetType(cxxCallArgumentExpectedType_))
            return false;

        bool sawTypeCast = false;
        for (auto* node = ctx->parent; node != nullptr; )
        {
            if (dynamic_cast<CFlatParser::ArgumentNamedExpressionContext*>(node) != nullptr)
                return sawTypeCast;
            if (auto* cast = dynamic_cast<CFlatParser::CastExpressionContext*>(node))
            {
                if (cast->castTypeName() != nullptr && cast->castExpression() != nullptr)
                {
                    antlr4::tree::ParseTree* childOnPath = ctx;
                    while (childOnPath != nullptr && childOnPath->parent != cast)
                        childOnPath = childOnPath->parent;
                    if (childOnPath != cast->castExpression()) return false;
                    sawTypeCast = true;
                }
                else if (cast->children.size() != 1)
                    return false;
                node = cast->parent;
                continue;
            }
            auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(node);
            if (rule == nullptr) return false;
            if (rule->children.size() == 1)
            {
                node = rule->parent;
                continue;
            }
            if (rule->children.size() == 3)
            {
                auto* left = dynamic_cast<antlr4::tree::TerminalNode*>(rule->children[0]);
                auto* right = dynamic_cast<antlr4::tree::TerminalNode*>(rule->children[2]);
                if (left != nullptr && right != nullptr
                    && left->getSymbol()->getType() == CFlatParser::LeftParen
                    && right->getSymbol()->getType() == CFlatParser::RightParen)
                {
                    node = rule->parent;
                    continue;
                }
            }
            return false;
        }
        return false;
}

LLVMBackend::NamedVariable MainListener::ParseAssignmentExpressionNamed(CFlatParser::AssignmentExpressionContext* ctx,
                                                              ResultUse use) {
        auto result = ParseAssignmentExpressionNamedBody(ctx, use);
        // Direct call arguments are resolved against the selected parameter below. Other uses
        // consume a value, so detach an owning conditional read before it reaches a plain store.
        bool directCallArgument = false;
        antlr4::tree::ParseTree* node = ctx;
        while (node != nullptr && node->parent != nullptr)
        {
            auto* parent = node->parent;
            if (dynamic_cast<CFlatParser::ArgumentNamedExpressionContext*>(parent) != nullptr)
            {
                directCallArgument = true;
                break;
            }
            if (parent->children.size() == 1)
            {
                node = parent;
                continue;
            }
            auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(parent);
            if (primary != nullptr && primary->LeftParen() != nullptr
                && primary->expression() != nullptr)
            {
                node = parent;
                continue;
            }
            break;
        }
        if (!directCallArgument && use != ResultUse::Discard)
            MaterializeNullConditionalOwningValue(result, ctx);
        // T483: only a `move` written directly over a call is a call-result move. A `?:` of moves
        // (T249) joins its own temp, so its arguments must not take the direct-call transfer.
        if (auto* moveSite = ctx != nullptr ? TopLevelMoveExpression(ctx) : nullptr)
        {
            auto* operand = moveSite->unaryExpression() != nullptr
                ? moveSite->unaryExpression()->postfixExpression() : nullptr;
            result.IsDirectMoveCall = operand != nullptr && operand->children.size() >= 3
                && dynamic_cast<antlr4::tree::TerminalNode*>(operand->children.back()) != nullptr
                && operand->children.back()->getText() == ")";
        }
        return result;
}

void MainListener::MaterializeNullConditionalOwningValue(
        LLVMBackend::NamedVariable& value,
        antlr4::ParserRuleContext* ctx) {
        auto* compiler = Compiler(ctx);
        if (!value.IsNullConditionalOwningValue) return;
        const std::string typeName = value.TypeAndValue.TypeName;
        if (!compiler->IsCopyableType(typeName)
            || compiler->IsLoweredCFlatOnlyStruct(typeName))
        {
            LogErrorContext(ctx, std::format(
                "cannot read owning struct '{}' by value from a null-conditional '?.' access; "
                "use an explicit null check and move or copy the field", typeName));
        }

        LLVMBackend::NamedVariable source;
        source.Primary = value.Primary;
        source.BaseType = value.BaseType;
        source.TypeAndValue.TypeName = typeName;
        llvm::Value* copied = EmitCopyableOwnerCopy(source, value.Primary, ctx);
        if (copied == nullptr || copied == value.Primary)
            LogErrorContext(ctx, std::format(
                "cannot safely copy owning struct '{}' from a null-conditional '?.' access",
                typeName));

        auto type = value.TypeAndValue;
        type.VariableName.clear();
        type.ParentVariableName.clear();
        // The deep copy is a fresh owned temporary. Its next consumer transfers it;
        // treating it as an lvalue would copy the same value a second time.
        type.IsMove = true;
        value = {};
        value.TypeAndValue = std::move(type);
        value.BaseType = copied->getType();
        value.Primary = copied;
        value.IsRvalue = true;
}

// The tuple literal of a cast chain that is ONLY that literal: no sizeof/unary operator, cast, or
// postfix suffix. Anything else keeps ordinary evaluation, so sizeof, member and ++ apply as on master.
static CFlatParser::TupleExpressionContext* BareTupleLiteralCast(CFlatParser::CastExpressionContext* cast)
{
    if (cast == nullptr || cast->children.size() != 1) return nullptr;
    auto* unary = cast->unaryExpression();
    if (unary == nullptr || unary->children.size() != 1) return nullptr;
    auto* postfix = unary->postfixExpression();
    if (postfix == nullptr || postfix->children.size() != 1) return nullptr;
    auto* primary = postfix->primaryExpression();
    if (primary == nullptr || primary->children.size() != 1) return nullptr;
    return primary->tupleExpression();
}

LLVMBackend::NamedVariable MainListener::ParseAssignmentExpressionNamedBody(CFlatParser::AssignmentExpressionContext* ctx,
                                                              ResultUse use) {
        // Snapshot and clear the owned-return flag so FinishAssignmentExpressionNamed
        // can tell whether THIS expression (not a stale prior call) ended in an
        // owned-string-returning call.
        bool savedOwned = compilerLLVM->lastCallReturnsOwned;
        compilerLLVM->lastCallReturnsOwned = false;
        // Clear any stale new/move owning flag left by a PRIOR statement (e.g. a bare
        // `items.add(new T())` whose `new` result was moved into the callee, not consumed
        // by a declaration). Only a new/move parsed WITHIN this RHS may mark the target
        // owning; without this, the next pointer/interface declaration in the same scope
        // (notably `T* d = expr as U;`, whose cast produces no owning signal of its own)
        // falsely inherits ownership and double-frees at scope exit. Mirrors the existing
        // assignment-path reset (see operatorText == "=" below).
        compilerLLVM->lastOwningResult = false;
        compilerLLVM->lastAllocAlignment = 0;
        compilerLLVM->lastCallReturnsAllocAlign = 0;
        // Only a `move <element slot>` parsed WITHIN this RHS may key the container-slot ownership
        // override (see ParseDeclaration); clear any value left by a prior statement.
        compilerLLVM->lastMovedFromContainerSlot = false;

        CFlatParser::LambdaExpressionContext* directLambda = nullptr;
        if (auto* unary = ctx->unaryExpression())
            if (auto* postfix = unary->postfixExpression())
                if (auto* primary = postfix->primaryExpression())
                    directLambda = primary->lambdaExpression();
        if (directLambda != nullptr && ctx->assignmentOperator() != nullptr
            && ctx->assignmentOperator()->getText() == "=" && ctx->assignmentExpression() != nullptr
            && directLambda->lambdaBody() != nullptr && directLambda->lambdaBody()->getText() == "_")
        {
            auto* savedDiscardRhs = lambdaDiscardRhs_;
            lambdaDiscardRhs_ = ctx->assignmentExpression();
            auto result = ParseLambdaExpression(directLambda);
            lambdaDiscardRhs_ = savedDiscardRhs;
            return result;
        }

        auto* condCtx = ctx->conditionalExpression();
        if (condCtx && !ctx->assignmentOperator()
            && !condCtx->Question() && !condCtx->QuestionQuestion())
        {
            auto* lor = condCtx->logicalOrExpression();
            if (lor)
            {
                auto las = lor->logicalAndExpression();
                if (las.size() == 1)
                {
                    auto ios = las[0]->inclusiveOrExpression();
                    if (ios.size() == 1)
                    {
                        auto eos = ios[0]->exclusiveOrExpression();
                        if (eos.size() == 1)
                        {
                            auto ands = eos[0]->andExpression();
                            if (ands.size() == 1)
                            {
                                auto eqs = ands[0]->equalityExpression();
                                if (eqs.size() == 1)
                                {
                                    auto tcs = eqs[0]->typeCheckExpression();
                                    // Guard: skip the fast path if the typeCheckExpression has 'is'/'as'
                                    // operators - those must be handled by ParseConditionalExpression.
                                    if (tcs.size() == 1 && tcs[0]->typeSpecifier().empty())
                                    {
                                        auto* relCtx = tcs[0]->relationalExpression();
                                        auto rels = relCtx ? relCtx->shiftExpression() : std::vector<CFlatParser::ShiftExpressionContext*>{};
                                        if (rels.size() == 1)
                                        {
                                            auto shs = rels[0]->additiveExpression();
                                            if (shs.size() == 1)
                                            {
                                                auto adds = shs[0]->multiplicativeExpression();
                                                if (adds.size() == 1)
                                                {
                                                    auto muls = adds[0]->castExpression();
                                                    // A `(value) op y` cast hazard re-parses at the
                                                    // and-expression level, below.
                                                    if (muls.size() == 1 && FindValueCastHazard(muls[0]) == nullptr)
                                                    {
                                                        auto* cast = muls[0];
                                                        if (auto* tuple = BareTupleLiteralCast(cast))
                                                            return FinishAssignmentExpressionNamed(
                                                                ParseTupleExpression(tuple), savedOwned);
                                                        // Pure single-child passthrough all the way down:
                                                        // this fast path IS the only shape where a bare
                                                        // return-block call reaches the inliner, so forward
                                                        // the use unchanged. Any operator/value context
                                                        // takes the fallback below with ResultUse::Value.
                                                        auto passthrough = ParseCastExpression(muls[0], false, use);
                                                        const std::string text = ctx->getText();
                                                        passthrough.IsStringLiteral = IsStringLiteralText(text);
                                                        passthrough.CxxParamLastUse = IsLastUseOfForeignCxxParam(
                                                            compilerLLVM, ctx, passthrough);
                                                        return FinishAssignmentExpressionNamed(passthrough, savedOwned);
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        // The statement parser has a dedicated `_ = expr` discard arm, but a lambda expression
        // body reaches this named path directly. Route the same top-level spelling through the
        // existing implementation so the remedy named by the owning-return diagnostic remains
        // writable in expression-body position (and in a parenthesized wrapper around it).
        if (ctx->assignmentOperator() != nullptr && ctx->assignmentOperator()->getText() == "="
            && (ctx->unaryExpression() != nullptr && ctx->unaryExpression()->getText() == "_"
                || ctx->getText().starts_with("_=")))
        {
            ParseAssignmentExpression(ctx);
            return {};
        }
        if (ctx->assignmentOperator() == nullptr)
        {
            auto* cond = ctx->conditionalExpression();
            auto* ors = cond != nullptr ? cond->logicalOrExpression() : nullptr;
            if (cond != nullptr && cond->Question() == nullptr && cond->QuestionQuestion() == nullptr
                && ors != nullptr && ors->logicalAndExpression().size() == 1)
            {
                auto* andExpr = ors->logicalAndExpression(0);
                if (andExpr->inclusiveOrExpression().size() == 1)
                {
                    auto* inclusive = andExpr->inclusiveOrExpression(0);
                    if (inclusive->exclusiveOrExpression().size() == 1)
                    {
                        auto* exclusive = inclusive->exclusiveOrExpression(0);
                        if (exclusive->andExpression().size() == 1)
                        {
                            auto* andBit = exclusive->andExpression(0);
                            if (andBit->equalityExpression().size() == 1)
                            {
                                auto* equality = andBit->equalityExpression(0);
                                if (equality->typeCheckExpression().size() == 1)
                                {
                                    auto* typeCheck = equality->typeCheckExpression(0);
                                    if (typeCheck->Is().empty() && typeCheck->As().empty())
                                    {
                                        auto* relational = typeCheck->relationalExpression();
                                        if (relational->shiftExpression().size() == 1)
                                        {
                                            auto* shift = relational->shiftExpression(0);
                                            if (shift->additiveExpression().size() == 1)
                                            {
                                                auto* additive = shift->additiveExpression(0);
                                                if (additive->multiplicativeExpression().size() == 1)
                                                {
                                                    auto* multiplicative = additive->multiplicativeExpression(0);
                                                    if (multiplicative->castExpression().size() == 1)
                                                    {
                                                        auto* cast = multiplicative->castExpression(0);
                                                        if (auto* tuple = BareTupleLiteralCast(cast))
                                                            return FinishAssignmentExpressionNamed(
                                                                ParseTupleExpression(tuple), savedOwned);
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        // Fall back: call ParseConditionalExpression directly (when no assignment) so we can
        // recover the isUnsigned flag from TypedValue and synthesize the TypeName for Upconvert.
        {
            LLVMBackend::NamedVariable result;
            LLVMBackend::TypedValue joinShape;  // a `?:` join's pointer shape, filled below
            auto* condCtx = ctx->conditionalExpression();
            if (condCtx && !ctx->assignmentOperator())
            {
                auto tv = ParseConditionalExpression(condCtx, use);
                joinShape = tv;
                result.Primary = tv.value;
                result.JoinClassPointee = tv.joinClassPointee;
                result.JoinClassPointeeDepth = tv.joinClassPointeeDepth;
                result.TypeAndValue.IsAlias = tv.isAlias;
                result.IsRvalue = tv.isRvalue;
                result.IsCxxPrvalueConditional = tv.isCxxPrvalueConditional;
                result.IsPointerArithmeticResult = tv.isPointerArithmeticResult;
                // An arm that is a macro constant is a prvalue, so the `?:` joins to a prvalue
                // ([expr.cond]) whatever the other arm is and whatever the storage holds.
                result.IsCxxMacroJoin = condCtx->Question() != nullptr
                    && (IsCxxMacroPrvalueArm(compilerLLVM, condCtx->expression())
                        || IsCxxMacroPrvalueArm(compilerLLVM, condCtx->conditionalExpression()));
                // An arithmetic result's C++ type for wrapper deduction; native typing keeps the
                // width-based TypeName assigned below (e.g. i64). `?:` is not carried here.
                if (condCtx->Question() == nullptr && condCtx->QuestionQuestion() == nullptr)
                    result.CxxArithIdentity = tv.cxxArithIdentity;
                result.Storage = tv.storage;
                result.ArithNarrowWidth = tv.arithNarrowWidth;
                result.CxxRefValueType = tv.cxxRefValueType;
                result.ConditionalMoveSources = tv.conditionalMoveSources;
                result.PointeeMove = tv.sourceIsPointeeMove;
                result.RootIsBorrowedThisField = tv.sourceRootIsBorrowedThisField;
                result.ThisFieldPath = tv.sourceThisFieldPath;
                result.IsExplicitMove = tv.isExplicitMove;
                result.TernaryOwnedCallArms = tv.ownedCallArms;
                // A `?:` over two addressable C++ objects joins their storage too; keep it so a
                // parenthesized ternary operand still names the SELECTED object.
                if (result.Storage == nullptr && tv.receiverStorage != nullptr
                    && tv.value != nullptr && !tv.value->getType()->isPointerTy())
                    result.Storage = tv.receiverStorage;
                if (result.Primary)
                {
                    result.BaseType = result.Primary->getType();
                    if (joinShape.value == result.Primary && joinShape.hasSourceType
                        && result.TypeAndValue.TypeName.empty())
                        result.TypeAndValue = joinShape.sourceType;
                    if (result.TypeAndValue.TypeName.empty()
                        && result.BaseType->isStructTy()
                        && compilerLLVM->IsCxxRecord(tv.sourceTypeName))
                        result.TypeAndValue.TypeName = tv.sourceTypeName;
                    if (result.TypeAndValue.TypeName.empty()
                        && tv.cxxRefValueType != nullptr
                        && compilerLLVM->IsCxxRecord(tv.sourceTypeName))
                        result.TypeAndValue.TypeName = tv.sourceTypeName;
                    if (result.Primary->getType()->isIntegerTy()
                        && !(inCxxCallArgument_ && joinShape.hasSourceType))
                    {
                        unsigned bits = result.Primary->getType()->getIntegerBitWidth();
                        bool isUnsigned = tv.isUnsigned || tv.isUnsignedStorage;
                        // A `?:` of same-width arms keeps the arms' spelling: `c ? c : c` is `char`, not
                        // the width name `i8` that overload ranking reads as a promoted int.
                        const bool ternaryJoin = condCtx->Question() != nullptr;
                        llvm::Type* joinedArmType = ternaryJoin && joinShape.hasSourceType
                            && !joinShape.sourceType.Pointer && !joinShape.sourceType.TypeName.empty()
                            ? compilerLLVM->GetType(joinShape.sourceType) : nullptr;
                        const bool armNamed = joinedArmType != nullptr && joinedArmType->isIntegerTy()
                            && joinedArmType->getIntegerBitWidth() == bits;
                        if (!armNamed)
                        {
                            if      (bits == 8)  result.TypeAndValue.TypeName = isUnsigned ? "u8"  : "i8";
                            else if (bits == 16) result.TypeAndValue.TypeName = isUnsigned ? "u16" : "i16";
                            else if (bits == 32) result.TypeAndValue.TypeName = isUnsigned ? "u32" : "int";
                            else if (bits == 64) result.TypeAndValue.TypeName = isUnsigned ? "u64" : "i64";
                            else if (bits == 128) result.TypeAndValue.TypeName = isUnsigned ? "u128" : "i128";
                        }
                        // A `?:` whose two arms are one enum type is that enum, as in C++.
                        if (std::string enumKey = compilerLLVM->ResolveEnumTypeName(tv.sourceTypeName);
                            !enumKey.empty() && (condCtx->Question() != nullptr))
                        {
                            result.TypeAndValue.TypeName = enumKey;
                            result.TypeAndValue.EnumBacking = compilerLLVM->GetEnumBackingType(enumKey);
                            result.TypeAndValue.IsScopedEnum = compilerLLVM->IsScopedEnumTypeName(enumKey);
                        }
                    }
                    // A '?:' join of two interface values yields a phi/select with no NamedVariable
                    // of its own to carry IsInterface/TypeName - it is a bare fat {vtable,data}
                    // struct. Recover the interface identity from the per-value ledger that
                    // PropagateFatInterfaceJoin stamped during the join, so a by-value call argument
                    // (which reads TypeAndValue, not the ledger) matches an interface parameter
                    // instead of showing the raw "__iface_fat_ptr" struct name to overload resolution.
                    else if (result.BaseType == compilerLLVM->GetFatPtrType())
                    {
                        std::string ifaceName = compilerLLVM->FindFatInterfaceValueTypeName(result.Primary);
                        if (!ifaceName.empty() && ifaceName != LLVMBackend::kAmbiguousFatInterface)
                        {
                            result.TypeAndValue.IsInterface = true;
                            result.TypeAndValue.TypeName = ifaceName;
                        }
                    }
                }
            }
            else
            {
                LLVMBackend::NamedVariable assignmentResult;
                result.Primary = ParseAssignmentExpression(ctx,
                    use == ResultUse::Discard ? nullptr : &assignmentResult);
                if (result.Primary)
                {
                    result.BaseType = result.Primary->getType();
                    result.TypeAndValue = assignmentResult.TypeAndValue;
                    result.Storage = assignmentResult.Storage;
                    result.IsRvalue = assignmentResult.IsRvalue;
                    if (assignmentResult.TypeAndValue.IsAlias
                        && assignmentResult.Storage != nullptr)
                        result.Primary = nullptr;
                    result.ChainedAssignSlot = assignmentResult.ChainedAssignSlot;
                    result.ChainedAssignName = assignmentResult.ChainedAssignName;
                    result.IsUniqueFieldAssignmentResult =
                        assignmentResult.IsUniqueFieldAssignmentResult;
                    // All of it: an owning struct destination carries its borrow provenance.
                    AdoptWrapperProvenance(result, assignmentResult);
                }
            }
            if (const auto* raw = compilerLLVM->FindRawArrayResult(result.Primary))
            {
                result.AllocatedByRawNewArray = !raw->IsArrayView;
                result.RawArrayLength = raw->Count;
                result.AllocAlignment = raw->AllocAlign;
                result.IsOwning = compilerLLVM->RawArrayResultMayOwn(result.Primary);
            }
            // A parenthesized `is`/`as` expression reaches this fallback as a raw value, so the
            // result otherwise loses the target type before a postfix member access sees it.
            // Restore the target identity for `(p as IMore).more()` and the corresponding `as`
            // value shapes without changing ordinary expression inference.
            if (result.Primary != nullptr && condCtx != nullptr && !ctx->assignmentOperator())
            {
                if (auto* typeCheck = SoleTypeCheckExpressionOf(condCtx);
                    typeCheck != nullptr && typeCheck->typeSpecifier().size() == 1
                    && typeCheck->children.size() >= 2
                    && typeCheck->children[1]->getText() == "as")
                {
                    std::string target = ParseTypeSpecifierName(typeCheck->typeSpecifier()[0],
                        typeCheck->multiWordTypeSuffix().empty()
                            ? nullptr : typeCheck->multiWordTypeSuffix()[0]);
                    result.TypeAndValue.TypeName = compilerLLVM->ResolveTypeAlias(target);
                    result.TypeAndValue.IsInterface = compilerLLVM->HasInterface(result.TypeAndValue.TypeName);
                    result.TypeAndValue.Pointer = !result.TypeAndValue.IsInterface
                        && result.Primary->getType()->isPointerTy();
                    result.TypeAndValue.IsInterfacePointer = false;
                }
            }
            if (result.Primary != nullptr && compilerLLVM->IsAliasValue(result.Primary))
                result.TypeAndValue.IsAlias = true;
            if (result.Primary != nullptr)
                if (const auto* sources = compilerLLVM->FindBondedValue(result.Primary))
                {
                    result.IsBonded = true;
                    result.BondedSources = *sources;
                }
            // A '?:' / '??' result reaches here as a raw join with no type name; fill only the
            // type identity so flags already decided above (IsAlias) are not overwritten.
            if (result.Primary != nullptr && result.TypeAndValue.TypeName.empty()
                && condCtx != nullptr
                && (condCtx->Question() != nullptr || condCtx->QuestionQuestion() != nullptr))
            {
                // A view join names its element only through the ledger the join stamped.
                if (const auto* viewJoin = compilerLLVM->FindViewJoinType(result.Primary))
                {
                    result.TypeAndValue.TypeName = viewJoin->TypeName;
                    result.TypeAndValue.Pointer = true;
                    result.TypeAndValue.ElemPointer = viewJoin->ElemPointer;
                    result.TypeAndValue.IsArrayView = true;
                    result.TypeAndValue.IsInterface = viewJoin->IsInterface;
                }
                // Both arms named one pointer shape: that proves pointee AND depth.
                else if (joinShape.value == result.Primary && joinShape.pointerDepth >= 1
                         && !joinShape.sourceTypeName.empty())
                {
                    result.TypeAndValue.TypeName = joinShape.sourceTypeName;
                    result.TypeAndValue.Pointer = true;
                    result.TypeAndValue.PointerDepth = joinShape.pointerDepth;
                    result.TypeAndValue.ElemPointer = joinShape.elemPointer;
                }
                // Both arms point at one native class: rank the call argument as that class
                // pointer. Call arguments only, and `?:` only - a '??' join keeps its arms' ranking.
                else if (joinShape.value == result.Primary && !joinShape.joinClassPointee.empty()
                         && condCtx->Question() != nullptr
                         && inCallArgument_ && IsCallArgumentThroughParens(ctx)
                         && InferTernaryArmType(result.Primary).TypeName.empty())
                {
                    result.TypeAndValue.TypeName = joinShape.joinClassPointee;
                    result.TypeAndValue.Pointer = true;
                    result.TypeAndValue.PointerDepth = joinShape.joinClassPointeeDepth;
                    result.TypeAndValue.ElemPointer = joinShape.joinClassPointeeDepth > 1;
                }
                else
                {
                    auto inferred = InferTernaryArmType(result.Primary);
                    if (!inferred.TypeName.empty())
                    {
                        result.TypeAndValue.TypeName = inferred.TypeName;
                        result.TypeAndValue.Pointer = inferred.Pointer;
                        result.TypeAndValue.IsInterface = inferred.IsInterface;
                    }
                }
                result.TypeAndValue.SetPointeeConstBit(joinShape.value == result.Primary
                    && joinShape.cxxPointeeConst && result.TypeAndValue.Pointer);
            }
            if (result.Primary != nullptr && compilerLLVM->IsTempFieldValue(result.Primary))
            {
                result.FromOwningTempField = true;
                if (result.TypeAndValue.TypeName.empty())
                    result.TypeAndValue.TypeName = LLVMTypeToTypeName(result.Primary->getType());
            }
            const std::string text = ctx->getText();
            result.IsStringLiteral = IsStringLiteralText(text);
            if (text == "nullptr" && llvm::isa_and_nonnull<llvm::ConstantPointerNull>(result.Primary))
                result.TypeAndValue.IsCxxNullptrT = true;
            result.TernaryTempAlreadyRegistered = inCallArgument_
                && result.Primary != nullptr && llvm::isa<llvm::PHINode>(result.Primary)
                && !TernaryIsBinaryOperand(condCtx);
            // A bare `x as T` whose T is x's own type reached here as a raw value. Restore the
            // operand's storage and provenance, matched on the exact node so `x as T + 1` cannot.
            if (condCtx != nullptr && lastRedundantAsCtx_ != nullptr
                && SoleTypeCheckExpressionOf(condCtx) == lastRedundantAsCtx_)
            {
                result.Storage = lastRedundantAsNamed_.Storage;
                result.TypeAndValue = lastRedundantAsNamed_.TypeAndValue;
                AdoptWrapperProvenance(result, lastRedundantAsNamed_);
            }
            result.CxxParamLastUse = IsLastUseOfForeignCxxParam(compilerLLVM, ctx, result);
            lastRedundantAsCtx_ = nullptr;
            return FinishAssignmentExpressionNamed(result, savedOwned);
        }
    }

bool MainListener::RejectPointerShapedInterfaceUpcast(antlr4::ParserRuleContext* errCtx,
                                            const LLVMBackend::TypeAndValue& src,
                                            const std::string& interfaceName) {
        if (interfaceName.empty()) return false;
        std::string shape = compilerLLVM->DescribePointerShapedInterfaceSource(src);
        if (shape.empty()) return false;
        LLVMBackend::TypeAndValue sourceBase = src;
        sourceBase.Pointer = false;
        sourceBase.ElemPointer = false;
        sourceBase.PointerDepth = 0;
        sourceBase.IsArrayView = false;
        LogErrorContext(errCtx, compilerLLVM->FormatPointerShapedInterfaceUpcastError(
            shape, SpellType(*compilerLLVM, sourceBase),
            SpellType(*compilerLLVM, LLVMBackend::TypeAndValue{ .TypeName = interfaceName })));
        return true;
    }

bool MainListener::RejectPrimitiveShapedInterfaceUpcast(antlr4::ParserRuleContext* errCtx,
                                              const LLVMBackend::TypeAndValue& src,
                                              const std::string& interfaceName) {
        if (interfaceName.empty() || !compilerLLVM->HasInterface(interfaceName))
            return false;
        if (!LLVMBackend::IsPrimitiveTypeName(compilerLLVM->ResolveTypeAlias(src.TypeName)))
            return false;
        return RejectPointerShapedInterfaceUpcast(errCtx, src, interfaceName);
    }

LLVMBackend::InterfaceBoxSource MainListener::ClassifyInterfaceBoxSource(llvm::Value* dataPtr,
                                                              const LLVMBackend::NamedVariable* srcNV,
                                                              bool ownershipTransferred) {
        using Source = LLVMBackend::InterfaceBoxSource;
        if (dataPtr == nullptr) return Source::Unknown;
        llvm::Value* base = dataPtr->stripPointerCasts();
        while (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(base))
            base = gep->getPointerOperand()->stripPointerCasts();
        // Storage shape decides the FRAME case before ownership does: a by-value class local with
        // an owning binding still lives in this frame, and a box over it dies with the frame.
        if (llvm::isa<llvm::AllocaInst>(base)) return Source::FrameStorage;
        if (ownershipTransferred || (srcNV != nullptr && srcNV->IsOwning)
            || compilerLLVM->IsOwningValue(dataPtr))
            return Source::Heap;
        if (srcNV != nullptr && !srcNV->CallerName.empty()
            && compilerLLVM->IsFunctionParameter(srcNV->CallerName))
            return Source::Parameter;
        if (llvm::isa<llvm::GlobalVariable>(base)) return Source::Global;
        if (llvm::isa<llvm::Argument>(base)) return Source::Parameter;
        return Source::Unknown;
    }

bool MainListener::RetireOwningSourceOfBoxedValue(llvm::Value* value) {
        auto* compiler = compilerLLVM;
        auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(value);
        if (load == nullptr || !load->getType()->isPointerTy()) return false;
        if (!compiler->IsOwningValue(load)) return false;
        auto* slot = load->getPointerOperand();
        compiler->builder->CreateStore(
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(load->getType())), slot);
        std::string name = compiler->FindVariableNameByStorage(slot);
        if (!name.empty())
        {
            compiler->MarkVariableMoved(name);
            compiler->MarkVariableMovedIntoInterface(name);
        }
        return true;
    }

llvm::Value* MainListener::BoxConcreteIntoInterface(antlr4::ParserRuleContext* errCtx, llvm::Value* srcValue,
                                          bool sourceIsPointer, const std::string& structName,
                                          const std::string& interfaceName,
                                          const LLVMBackend::NamedVariable* srcNV,
                                          bool adoptsOwnership) {
        auto* compiler = compilerLLVM;
        if (!compiler->StructImplementsInterface(structName, interfaceName))
        {
            LogErrorContext(errCtx, std::format("'{}' does not implement interface '{}'",
                SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = structName }),
                SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = interfaceName })));
            return nullptr;
        }
        if (srcNV != nullptr
            && RejectPointerShapedInterfaceUpcast(errCtx, srcNV->TypeAndValue, interfaceName))
            return nullptr;

        // A pointer source IS the data pointer; a value source needs its address, preferring the
        // binding's own storage over a spill.
        llvm::Value* dataPtr = srcValue;
        if (!sourceIsPointer)
            dataPtr = srcNV != nullptr && srcNV->Storage != nullptr
                ? srcNV->Storage : AddressOfClassValueOperand(srcValue, compiler);

        auto* vtable = compiler->GetOrCreateVTable(structName, interfaceName);
        llvm::Value* fat = compiler->BuildInterfaceFatValue(vtable, dataPtr);

        // Ownership transfer: null the owning source and mark it moved so its scope-exit free
        // cannot double-free the object the interface box now owns.
        bool transferred = false;
        if (adoptsOwnership && srcNV != nullptr && srcNV->IsOwning && sourceIsPointer
            && srcNV->Storage != nullptr)
        {
            if (auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(srcNV->BaseType))
            {
                compiler->builder->CreateStore(llvm::ConstantPointerNull::get(ptrTy), srcNV->Storage);
                transferred = true;
                if (!srcNV->CallerName.empty())
                {
                    compiler->MarkVariableMoved(srcNV->CallerName);
                    compiler->MarkVariableMovedIntoInterface(srcNV->CallerName);
                }
            }
        }
        // The binding-erased spelling: same transfer, recovered from the VALUE instead of a name.
        if (adoptsOwnership && !transferred && sourceIsPointer)
            transferred = RetireOwningSourceOfBoxedValue(dataPtr);

        LLVMBackend::InterfaceBoxRecord record;
        record.FatValue = fat;
        record.DataPointer = dataPtr;
        record.SourceClassName = structName;
        record.InterfaceName = interfaceName;
        record.Source = ClassifyInterfaceBoxSource(dataPtr, srcNV, transferred);
        record.OwnershipTransferred = transferred;
        record.AliasOfOwnedLocal = !transferred
            && PointerRootKind(dataPtr, compiler) == PointerValueRootKind::AliasOfOwnedLocal;
        record.SourceKeepsOwner = ClassifyBoxedSourceKeepsOwner(dataPtr, srcNV, transferred);
        if (record.SourceKeepsOwner)
        {
            record.SourceDisplayName = DescribeBoxedSourceOwner(dataPtr, srcNV);
        }
        compiler->RegisterInterfaceBox(record);
        return fat;
    }

bool MainListener::BindingKeepsOwnershipOfBoxedObject(const LLVMBackend::NamedVariable* nv) const {
        return compilerLLVM->BindingKeepsOwnershipOfBoxedObject(nv);
    }

std::string MainListener::DescribeBoxedSourceOwner(llvm::Value* dataPtr,
                                         const LLVMBackend::NamedVariable* srcNV) const {
        const LLVMBackend::NamedVariable* nv = ProvingBindingForBoxedSource(dataPtr, srcNV);
        if (nv == nullptr)
        {
            // The value-keyed proof above names the FIELD, the only owner there is to name.
            if (dataPtr != nullptr)
                if (const auto* fb = compilerLLVM->FindUniqueFieldBorrowResult(dataPtr))
                    return std::format("'{}'", fb->FieldOwner);
            return {};
        }
        // A container's element: the CONTAINER frees it, never the local holding the borrow, so
        // an unnameable container falls back to a phrase rather than blaming the local.
        // Gated exactly as in BindingKeepsOwnershipOfBoxedObject: after a '??=' the element fact is
        // not the proof, so naming the container alone would under-name a join the guard fired on.
        if (nv->BorrowsOwnedElement && !nv->CoalesceRebound)
            return nv->OwnedElementContainer.empty()
                ? std::string("its container") : std::format("'{}'", nv->OwnedElementContainer);
        // Carried across a `p = q;` store from the RHS binding's own proof; already rendered there.
        if (nv->InheritedKeepsOwner && !nv->InheritedKeepsOwnerSource.empty())
            return nv->InheritedKeepsOwnerSource;
        if (JoinArmsStillKeepOwner(*nv) && !nv->JoinKeepsOwnerSource.empty())
            return nv->JoinKeepsOwnerSource;
        if (!nv->FieldName.empty())
            return nv->OwningStructName.empty()
                ? std::format("'{}'", nv->FieldName)
                : std::format("'{}.{}'", nv->OwningStructName, nv->FieldName);
        // A bare self-field read (`u` inside the struct's own method) comes from GetMemberVariable,
        // which leaves FieldName and OwningStructName empty; the enclosing method names the owner.
        bool isCoreUniqueValue = compilerLLVM->IsCoreUniqueType(nv->TypeAndValue.TypeName)
            && !nv->TypeAndValue.Pointer;
        if (nv->IsUniqueFieldAlias
            || nv->TypeAndValue.IsUnique || isCoreUniqueValue)
        {
            std::string field = nv->TypeAndValue.VariableName.empty()
                ? nv->CallerName : nv->TypeAndValue.VariableName;
            std::string owner = compilerLLVM->GetCurrentFunctionName();
            if (auto dot = owner.find('.'); dot != std::string::npos) owner = owner.substr(0, dot);
            else if (!owner.empty() && owner[0] == '~') owner = owner.substr(1);
            else owner.clear();
            if (field.empty()) return "its owner";
            return owner.empty() || !compilerLLVM->IsDataStructure(owner)
                ? std::format("'{}'", field) : std::format("'{}.{}'", owner, field);
        }
        // A borrow: name the ORIGIN, never the local holding it - the caller's parameter (or the
        // `unique` field) is what frees the object, exactly as the raw-delete diagnostic reports it.
        if (!nv->BorrowedUniqueField.empty()) return std::format("'{}'", nv->BorrowedUniqueField);
        if (nv->IsBorrowed && !nv->BorrowedOrigin.empty())
            return std::format("'{}'", nv->BorrowedOrigin);
        // A plain copy of an owning local: name the OWNER, never the copy holding it.
        if (compilerLLVM->OwningLocalCopyStillAliases(*nv))
            return std::format("'{}'", nv->OwningLocalOrigin);
        if (!nv->CallerName.empty()) return std::format("'{}'", nv->CallerName);
        auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(dataPtr);
        std::string name = load == nullptr
            ? std::string() : compilerLLVM->FindVariableNameByStorage(load->getPointerOperand());
        return name.empty() ? std::string() : std::format("'{}'", name);
    }

std::string MainListener::DescribeAssignedSourceOwner(const LLVMBackend::NamedVariable& rightNV) const {
        if (rightNV.Storage == nullptr || !llvm::isa<llvm::AllocaInst>(rightNV.Storage)) return {};
        const auto* srcNV = compilerLLVM->FindVariableByStorage(rightNV.Storage);
        if (!BindingKeepsOwnershipOfBoxedObject(srcNV)) return {};
        std::string owner = DescribeBoxedSourceOwner(nullptr, srcNV);
        return owner.empty() ? std::string("the binding it was assigned from") : owner;
    }

std::string MainListener::JoinArmsKeepOwner(llvm::Value* joined,
                                  std::vector<llvm::Value*>* slotsOut) const {
        if (slotsOut != nullptr) slotsOut->clear();
        std::vector<llvm::Value*> arms;
        if (auto* phi = llvm::dyn_cast_or_null<llvm::PHINode>(joined))
        {
            if (!phi->getType()->isPointerTy()) return {};
            for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
                arms.push_back(phi->getIncomingValue(i));
        }
        else if (auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(joined))
        {
            const auto* join = compilerLLVM->FindNullCoalesceJoin(load);
            if (join == nullptr) return {};
            for (const auto& arm : join->Arms) arms.push_back(arm.Value);
        }
        else return {};

        std::string owner;
        bool proved = false;
        for (auto* arm : arms)
        {
            if (arm == nullptr) return {};
            // The literal and a binding PROVABLY parked at null are the same neutral arm.
            if (JoinArmIsProvablyNull(arm)) continue;
            // A PROVEN `move` arm detached its source, so that binding frees nothing and cannot be
            // the other owner. Keyed on the arm VALUE - the source NamedVariable keeps IsOwning.
            if (compilerLLVM->IsMovedOutPtrValue(arm)) return {};
            auto* load = llvm::dyn_cast<llvm::LoadInst>(arm);
            if (load == nullptr || !load->getType()->isPointerTy()) return {};
            const auto* nv = compilerLLVM->FindVariableByStorage(load->getPointerOperand());
            if (!BindingKeepsOwnershipOfBoxedObject(nv)) return {};
            if (owner.empty()) owner = DescribeBoxedSourceOwner(arm, nullptr);
            if (slotsOut != nullptr) slotsOut->push_back(load->getPointerOperand());
            proved = true;
        }
        if (!proved) return {};
        return owner.empty() ? std::string("the binding each arm was joined from") : owner;
    }

bool MainListener::JoinArmIsProvablyNull(llvm::Value* arm, int depth) const {
        if (arm == nullptr) return false;
        if (llvm::isa<llvm::ConstantPointerNull>(arm)) return true;
        if (depth > 3) return false;
        auto* load = llvm::dyn_cast<llvm::LoadInst>(arm);
        if (load == nullptr || !load->getType()->isPointerTy()) return false;
        auto* slot = llvm::dyn_cast<llvm::AllocaInst>(load->getPointerOperand());
        // An escaping slot can be written through a pointer no store here accounts for, so the
        // store survey below would not be a survey of every reaching definition.
        if (slot == nullptr || !compilerLLVM->AllocaIsLoadStoreOnly(slot)) return false;
        bool sawStore = false;
        for (llvm::User* u : slot->users())
        {
            auto* store = llvm::dyn_cast<llvm::StoreInst>(u);
            if (store == nullptr || store->getPointerOperand() != slot) continue;
            sawStore = true;
            if (!JoinArmIsProvablyNull(store->getValueOperand(), depth + 1)) return false;
        }
        // No store at all is "unknown", not "null" - a parameter slot or an unwritten alloca.
        return sawStore;
    }

bool MainListener::JoinArmsStillKeepOwner(const LLVMBackend::NamedVariable& nv, int depth) const {
        return compilerLLVM->JoinArmsStillKeepOwner(nv, depth);
    }

bool MainListener::InterfaceBoxBorrowStillKeepsOwner(
    const LLVMBackend::NamedVariable& nv) const {
        if (!nv.BorrowedInterfaceBox) return false;
        // Direct boxes intentionally have no slot; slot-backed join records get the precise
        // rebind retirement.
        if (nv.BorrowedInterfaceBoxSlots.empty()) return true;
        for (auto* slot : nv.BorrowedInterfaceBoxSlots)
        {
            const auto* arm = compilerLLVM->FindVariableByStorage(slot);
            // Non-local slots (for example a unique field GEP) were never rebindable through
            // the boxed local's spelling. Keep their established positive proof intact.
            if (arm != nullptr && arm->PointerRebound) return false;
        }
        return true;
    }

const LLVMBackend::NamedVariable* MainListener::ProvingBindingForBoxedSource(
        llvm::Value* dataPtr, const LLVMBackend::NamedVariable* srcNV) const {
        if (BindingKeepsOwnershipOfBoxedObject(srcNV)) return srcNV;
        auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(dataPtr);
        if (load == nullptr || !load->getType()->isPointerTy()) return nullptr;
        const auto* slotNV = compilerLLVM->FindVariableByStorage(load->getPointerOperand());
        return BindingKeepsOwnershipOfBoxedObject(slotNV) ? slotNV : nullptr;
    }

bool MainListener::ClassifyBoxedSourceKeepsOwner(llvm::Value* dataPtr,
                                       const LLVMBackend::NamedVariable* srcNV,
                                       bool ownershipTransferred) const {
        if (ownershipTransferred) return false;
        // A boxed call RESULT proven to borrow a live `unique` field has no binding at all, so ask
        // the value-keyed ledger directly - the field is the owner either way.
        if (dataPtr != nullptr && compilerLLVM->FindUniqueFieldBorrowResult(dataPtr) != nullptr)
            return true;
        return ProvingBindingForBoxedSource(dataPtr, srcNV) != nullptr;
    }

bool MainListener::InterfaceBoxValueIsProvablyBorrowed(llvm::Value* fatValue,
                                             std::vector<std::string>& sourceNames,
                                             std::vector<llvm::Value*>& ownerSlots) {
        if (fatValue == nullptr) return false;
        auto* compiler = compilerLLVM;
        if (auto* record = compiler->FindInterfaceBoxByFatValue(fatValue))
        {
            if (!record->SourceKeepsOwner) return false;
            if (!record->SourceDisplayName.empty())
                sourceNames.push_back(record->SourceDisplayName);
            if (record->SourceOwnerSlot != nullptr)
                ownerSlots.push_back(record->SourceOwnerSlot);
            return true;
        }

        auto* phi = llvm::dyn_cast<llvm::PHINode>(fatValue);
        if (phi == nullptr) return false;
        bool sawBox = false;
        for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
        {
            llvm::Value* arm = phi->getIncomingValue(i);
            if (auto* constant = llvm::dyn_cast<llvm::Constant>(arm);
                constant != nullptr && constant->isNullValue())
                continue;
            const auto* record = compiler->FindInterfaceBoxByFatValue(arm);
            if (record == nullptr) return false;
            // Neutral, exactly like the null-literal arm skipped above: it owns nothing, so it
            // neither proves another owner nor blocks the arms that do.
            if (record->SourceProvablyNull) continue;
            if (!record->SourceKeepsOwner) return false;
            sawBox = true;
            const std::string& name = record->SourceDisplayName;
            if (!name.empty()
                && std::find(sourceNames.begin(), sourceNames.end(), name) == sourceNames.end())
                sourceNames.push_back(name);
            if (record->SourceOwnerSlot != nullptr
                && std::find(ownerSlots.begin(), ownerSlots.end(), record->SourceOwnerSlot)
                    == ownerSlots.end())
                ownerSlots.push_back(record->SourceOwnerSlot);
        }
        return sawBox;
    }

std::string MainListener::DescribeInterfaceBoxOwners(const std::vector<std::string>& names) const {
        std::string text;
        for (size_t i = 0; i < names.size(); i++)
        {
            if (i > 0) text += (i + 1 == names.size()) ? " or " : ", ";
            text += names[i];   // already quoted (or a phrase) by DescribeBoxedSourceOwner
        }
        return text;
    }

void MainListener::TagInterfaceBoxProvenance(const std::string& varName, llvm::Value* fatValue) {
        if (varName.empty() || fatValue == nullptr) return;
        if (auto* constant = llvm::dyn_cast<llvm::Constant>(fatValue);
            constant != nullptr && constant->isNullValue())
            return;
        std::vector<std::string> sourceNames;
        std::vector<llvm::Value*> ownerSlots;
        bool borrowed = InterfaceBoxValueIsProvablyBorrowed(fatValue, sourceNames, ownerSlots);
        compilerLLVM->SetInterfaceBoxIsBorrowed(varName, borrowed,
                                                DescribeInterfaceBoxOwners(sourceNames));
        compilerLLVM->SetInterfaceBoxBorrowSlots(
            varName, borrowed ? ownerSlots : std::vector<llvm::Value*>{});
    }

bool MainListener::FatValueOwnsHeapBox(llvm::Value* fatValue) {
        if (fatValue == nullptr) return false;
        if (auto* rec = compilerLLVM->FindInterfaceBoxByFatValue(fatValue))
            if (rec->Source == LLVMBackend::InterfaceBoxSource::Heap) return true;
        std::vector<llvm::Value*> dataPtrs;
        std::unordered_set<const llvm::Value*> seen;
        CollectFatValueFields(fatValue, 1u, dataPtrs, seen);
        for (auto* data : dataPtrs)
            if (compilerLLVM->FindInterfaceBoxByDataPointer(
                    data, LLVMBackend::InterfaceBoxSource::Heap) != nullptr)
                return true;
        return false;
    }

/*
 * Classifies the storage root of a pointer VALUE (see PointerValueRootKind). GEPs and casts are
 * stripped. A load from an owning slot is OwnedLocal when the slot is a local of this frame and
 * Owned otherwise (field, call result). A load from a function argument or a global is Borrowed.
 * An alloca address (a frame array, a by-value struct, or an element or field of one) is
 * FrameLocal. A local alias follows stores through a per-query memo and active-path set, bounded by
 * a step budget:
 * a stored OwnedLocal becomes AliasOfOwnedLocal, a stored FrameLocal stays FrameLocal. A phi or
 * select takes its worst arm; field-consume proofs also track whether every arm is local. A direct
 * call reads the callee's ReturnsOwned fact; an indirect pointer call uses its declared return
 * move marker. Anything undecidable is Undecided.
 */
static bool IsLocalPointerRoot(PointerValueRootKind kind)
    {
        return kind == PointerValueRootKind::FrameLocal
            || kind == PointerValueRootKind::OwnedLocal
            || kind == PointerValueRootKind::AliasOfOwnedLocal;
    }

struct PointerValueRootAnswer
    {
        PointerValueRootKind Kind;
        bool EveryArmLocal;
    };

struct PointerValueRootQuery
    {
        static constexpr size_t StepBudget = 32768;
        std::unordered_map<const llvm::Value*, PointerValueRootAnswer> Memo[2];
        std::unordered_set<const llvm::Value*> InProgress;
        size_t Steps = 0;
        bool Exhausted = false;
        bool DepthCapped = false;   // some arm hit the depth cap; its Undecided proves nothing
        bool StatementOwnedTempRoot = false;
        // Interface-return verdicts walk relay/fluent chains far deeper than the default cap.
        int MaxDepth = 8;

        bool TakeStep()
            {
                if (Steps >= StepBudget)
                {
                    Exhausted = true;
                    return false;
                }
                ++Steps;
                return true;
            }
    };

static PointerValueRootAnswer PointerValueRoot(llvm::Value* value, LLVMBackend* compiler,
                                                llvm::Function* fn, int depth,
                                                PointerValueRootQuery& query,
                                                bool frameLocalContainer = false);

/*
 * T522: the pointer values the fields of local aggregate memory may hold just before a program
 * point - a per-slot points-to answer. Slots are pointer-sized byte offsets from the alloca. A
 * backward scan from the point kills a slot at its nearest whole store on each path; a
 * whole-value store or memcpy hands the covered slots to the stored value / copy source, read as of
 * that instruction. Variable-index writes join without killing; a variable-index query reads every
 * slot. A by-value parameter holds the caller's borrows (elided at the caller); a struct returned by
 * a call holds what its arguments hold (owning struct types are unproven). Any escape of the
 * aggregate's address, an unknown producer or the budget answers false (unproven).
 */
// The synthesized closure value destructor / copy (`__closure_fat_ptr.dtor.N(ptr)`,
// `__closure_fat_ptr.copy.N(value)`): neither stores the slot address anywhere.
static bool IsClosureValueHelperCall(const llvm::Value* value, std::string_view kind)
    {
        const auto* call = llvm::dyn_cast<llvm::CallBase>(value);
        const llvm::Function* callee = call == nullptr ? nullptr : call->getCalledFunction();
        if (callee == nullptr || call->arg_size() != 1) return false;
        const std::string base = "__closure_fat_ptr." + std::string(kind);
        return IsClosureHelperName(callee->getName(), base);
    }

// A struct passed in its C ABI form: a whole-size non-pointer load (`i64`, `[2 x i64]`) of a struct
// copy slot (abi.coerce). Null for anything else, a field read included.
static llvm::AllocaInst* CoercedStructSlot(llvm::Value* value)
    {
        auto* load = llvm::dyn_cast<llvm::LoadInst>(value);
        if (load == nullptr || load->getType()->isPointerTy()) return nullptr;
        auto* slot = llvm::dyn_cast<llvm::AllocaInst>(load->getPointerOperand()->stripPointerCasts());
        if (slot == nullptr || !slot->getAllocatedType()->isStructTy() || slot->getAllocatedType() == load->getType())
            return nullptr;
        const llvm::DataLayout& layout = load->getModule()->getDataLayout();
        if (layout.getTypeAllocSize(load->getType()) != layout.getTypeAllocSize(slot->getAllocatedType()))
            return nullptr;
        return slot;
    }

// A struct passed by value through a hidden pointer to a caller copy: `ptr byval(%S) %abi.byval`
// (Windows x64) or a plain `ptr %abi.byval` (arm64, over 16 bytes). The copy slot; the argument is
// the struct's value, never a borrow of the copy.
static llvm::AllocaInst* ByValStructSlot(const llvm::CallBase* call, unsigned index)
    {
        if (call == nullptr || index >= call->arg_size()) return nullptr;
        auto* slot = llvm::dyn_cast<llvm::AllocaInst>(call->getArgOperand(index)->stripPointerCasts());
        if (slot == nullptr || !slot->getAllocatedType()->isStructTy()) return nullptr;
        return call->isByValArgument(index) || slot->getName().starts_with("abi.byval") ? slot : nullptr;
    }

class AggregateFieldContents
{
public:
    // `borrowedMemory`: true when an address outside this frame is proven caller / global memory;
    // a struct read from there holds the caller's borrows.
    AggregateFieldContents(LLVMBackend* compiler, const llvm::DataLayout& layout, const llvm::Value* allowedCall,
                           std::function<bool(llvm::Value*)> borrowedMemory)
        : compiler_(compiler), layout_(layout), allowedCall_(allowedCall),
          pointerSize_(static_cast<int64_t>(layout.getPointerSize())), borrowedMemory_(std::move(borrowedMemory)) {}
    std::vector<llvm::Value*> Held;
    // The fields readable through `address` (the whole sub-object it names) just before `point`.
    bool ThroughAddress(llvm::Value* address, llvm::Instruction* point, llvm::Type* pointee = nullptr)
    {
        pointee_ = pointee;
        return slotsAt(address, point, nullptr, 0);
    }
    // The fields of an aggregate value at `point`.
    bool OfValue(llvm::Value* value, llvm::Instruction* point)
    {
        return valueContents(value, allSlots(value->getType()), point, 0);
    }
    static llvm::AllocaInst* AggregateBase(llvm::Value* address)
    {
        address = address->stripPointerCasts();
        while (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(address))
            address = gep->getPointerOperand()->stripPointerCasts();
        auto* base = llvm::dyn_cast<llvm::AllocaInst>(address);
        if (base == nullptr || base->isArrayAllocation()
            || !(base->getAllocatedType()->isStructTy() || base->getAllocatedType()->isArrayTy()))
            return nullptr;
        return base;
    }

private:
    struct BaseInfo
    {
        bool Escapes = false;
        std::unordered_map<const llvm::Value*, std::optional<int64_t>> Offsets;
    };
    LLVMBackend* compiler_;
    const llvm::DataLayout& layout_;
    const llvm::Value* allowedCall_;
    int64_t pointerSize_;
    std::function<bool(llvm::Value*)> borrowedMemory_;
    int budget_ = 20000;
    // Each relay hop (copy slot, call, result slot) costs about four levels; the budget bounds work.
    static constexpr int kMaxDepth = 24;
    llvm::Type* pointee_ = nullptr;
    std::unordered_map<const llvm::AllocaInst*, BaseInfo> bases_;

    std::vector<int64_t> allSlots(llvm::Type* type) const
    {
        std::vector<int64_t> slots;
        const int64_t size = static_cast<int64_t>(layout_.getTypeAllocSize(type));
        for (int64_t offset = 0; offset < size; offset += pointerSize_) slots.push_back(offset);
        return slots;
    }
    bool indexedOffset(llvm::Type* type, llvm::ArrayRef<unsigned> indices, int64_t& offset) const
    {
        offset = 0;
        for (unsigned index : indices)
        {
            if (auto* structType = llvm::dyn_cast<llvm::StructType>(type))
            {
                offset += static_cast<int64_t>(layout_.getStructLayout(structType)->getElementOffset(index));
                type = structType->getElementType(index);
            }
            else if (auto* arrayType = llvm::dyn_cast<llvm::ArrayType>(type))
            {
                offset += static_cast<int64_t>(index * layout_.getTypeAllocSize(arrayType->getElementType()));
                type = arrayType->getElementType();
            }
            else
                return false;
        }
        return true;
    }
    const BaseInfo& info(llvm::AllocaInst* base)
    {
        auto [it, inserted] = bases_.try_emplace(base);
        BaseInfo& baseInfo = it->second;
        if (!inserted) return baseInfo;
        baseInfo.Offsets[base] = 0;
        std::vector<llvm::Value*> work{ base };
        for (size_t i = 0; i < work.size() && !baseInfo.Escapes; ++i)
        {
            llvm::Value* current = work[i];
            const std::optional<int64_t> offset = baseInfo.Offsets[current];
            for (llvm::User* user : current->users())
            {
                if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user);
                    gep != nullptr && gep->getPointerOperand() == current)
                {
                    std::optional<int64_t> next;
                    llvm::APInt delta(layout_.getIndexTypeSizeInBits(gep->getType()), 0);
                    if (offset && gep->accumulateConstantOffset(layout_, delta)) next = *offset + delta.getSExtValue();
                    if (baseInfo.Offsets.emplace(gep, next).second) work.push_back(gep);
                    continue;
                }
                if (auto* cast = llvm::dyn_cast<llvm::CastInst>(user);
                    cast != nullptr && cast->getType()->isPointerTy())
                {
                    if (baseInfo.Offsets.emplace(cast, offset).second) work.push_back(cast);
                    continue;
                }
                if (llvm::isa<llvm::LoadInst>(user)) continue;
                if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                    store != nullptr && store->getPointerOperand() == current && store->getValueOperand() != current)
                    continue;
                if (auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(user))
                {
                    switch (intrinsic->getIntrinsicID())
                    {
                    case llvm::Intrinsic::lifetime_start:
                    case llvm::Intrinsic::lifetime_end:
                    case llvm::Intrinsic::dbg_declare:
                    case llvm::Intrinsic::dbg_value:
                    case llvm::Intrinsic::memcpy:
                    case llvm::Intrinsic::memmove:
                        continue;
                    case llvm::Intrinsic::memset:
                        if (intrinsic->getArgOperand(0) == current) continue;
                        break;
                    default:
                        break;
                    }
                }
                if (user == allowedCall_) continue;
                if (IsClosureValueHelperCall(user, "dtor")) continue;  // frees the env, keeps no address
                // A by-value struct copy handed to its call (byval / indirect): the callee's value.
                if (auto* call = llvm::dyn_cast<llvm::CallBase>(user); call != nullptr && current == base
                    && std::all_of(call->arg_begin(), call->arg_end(), [&](const llvm::Use& use) {
                           return use.get() != current || ByValStructSlot(call, call->getArgOperandNo(&use)) != nullptr;
                       }))
                    continue;
                // A constructor's sret slot: the callee writes the whole object (a store, below).
                if (auto* call = llvm::dyn_cast<llvm::CallBase>(user);
                    call != nullptr && call->arg_size() > 0 && call->hasStructRetAttr()
                    && call->getArgOperand(0) == current && offset && *offset == 0
                    && std::none_of(call->arg_begin() + 1, call->arg_end(),
                                    [&](const llvm::Use& use) { return use.get() == current; }))
                    continue;
                baseInfo.Escapes = true;
                break;
            }
        }
        return baseInfo;
    }
    // `relative` (optional): read only these offsets from `address`; else the whole sub-object.
    bool slotsAt(llvm::Value* address, llvm::Instruction* point, const std::vector<int64_t>* relative, int depth)
    {
        if (depth > kMaxDepth || budget_ <= 0) return false;
        llvm::AllocaInst* base = AggregateBase(address);
        if (base == nullptr)
            return relative != nullptr && borrowedMemory_
                && !llvm::isa<llvm::AllocaInst>(llvm::getUnderlyingObject(address)) && borrowedMemory_(address);
        const BaseInfo& baseInfo = info(base);
        if (baseInfo.Escapes) return false;
        auto found = baseInfo.Offsets.find(address);
        if (found == baseInfo.Offsets.end()) return false;
        const int64_t allocSize = static_cast<int64_t>(layout_.getTypeAllocSize(base->getAllocatedType()));
        std::set<int64_t> live;
        if (!found->second)
        {
            if (relative != nullptr) return false;
            for (int64_t offset = 0; offset < allocSize; offset += pointerSize_) live.insert(offset);
        }
        else if (relative != nullptr)
        {
            for (int64_t r : *relative) live.insert(*found->second + r);
        }
        else
        {
            int64_t size = allocSize - *found->second;
            if (pointee_ != nullptr && pointee_->isSized())
                size = std::min<int64_t>(size, static_cast<int64_t>(layout_.getTypeAllocSize(pointee_)));
            else if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(address->stripPointerCasts()))
                size = std::min<int64_t>(size, static_cast<int64_t>(layout_.getTypeAllocSize(gep->getResultElementType())));
            for (int64_t offset = *found->second; offset < *found->second + size; ++offset)
                if (offset % pointerSize_ == 0) live.insert(offset);
        }
        struct Item
        {
            llvm::BasicBlock* Block;
            llvm::Instruction* Before;
            std::set<int64_t> Live;
        };
        std::unordered_map<const llvm::BasicBlock*, std::set<int64_t>> seen;
        std::vector<Item> work{ { point->getParent(), point, std::move(live) } };
        while (!work.empty())
        {
            Item item = std::move(work.back());
            work.pop_back();
            auto cursor = item.Before != nullptr ? item.Before->getIterator() : item.Block->end();
            bool reachedBase = false;
            while (cursor != item.Block->begin() && !item.Live.empty())
            {
                --cursor;
                llvm::Instruction& instruction = *cursor;
                if (--budget_ <= 0) return false;
                if (&instruction == base)
                {
                    reachedBase = true;
                    break;
                }
                if (auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
                {
                    auto destination = baseInfo.Offsets.find(store->getPointerOperand());
                    if (destination == baseInfo.Offsets.end()) continue;
                    llvm::Value* stored = store->getValueOperand();
                    if (stored->getType()->isPointerTy())
                    {
                        if (!destination->second) Held.push_back(stored);
                        else if (item.Live.erase(*destination->second) != 0) Held.push_back(stored);
                        continue;
                    }
                    // A struct written in its C ABI form (`store i64 %call, ptr %abi.ret`) is a whole
                    // object; any other scalar store holds no pointer.
                    if (!stored->getType()->isAggregateType()
                        && !(destination->second && *destination->second == 0 && base->getAllocatedType()->isStructTy()
                             && static_cast<int64_t>(layout_.getTypeAllocSize(stored->getType())) == allocSize))
                        continue;
                    const int64_t size = static_cast<int64_t>(layout_.getTypeAllocSize(stored->getType()));
                    if (!destination->second)
                    {
                        if (!valueContents(stored, allSlots(stored->getType()), &instruction, depth + 1)) return false;
                        continue;
                    }
                    std::vector<int64_t> covered;
                    for (int64_t slot : item.Live)
                        if (slot >= *destination->second && slot < *destination->second + size)
                            covered.push_back(slot - *destination->second);
                    if (covered.empty()) continue;
                    if (!valueContents(stored, covered, &instruction, depth + 1)) return false;
                    for (int64_t r : covered) item.Live.erase(*destination->second + r);
                    continue;
                }
                // An sret call writes the whole object: it holds what the other arguments hold.
                if (auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    call != nullptr && call->arg_size() > 0 && call->hasStructRetAttr())
                {
                    auto destination = baseInfo.Offsets.find(call->getArgOperand(0));
                    if (destination != baseInfo.Offsets.end())
                    {
                        auto* structType = llvm::dyn_cast_or_null<llvm::StructType>(call->getParamStructRetType(0));
                        if (!destination->second || structType == nullptr || !structType->hasName()
                            || compiler_->StructValueMayOwnPointees(structType->getName().str()))
                            return false;
                        const int64_t size = static_cast<int64_t>(layout_.getTypeAllocSize(structType));
                        std::vector<int64_t> covered;
                        for (int64_t slot : item.Live)
                            if (slot >= *destination->second && slot < *destination->second + size) covered.push_back(slot);
                        if (covered.empty()) continue;
                        for (unsigned a = 1; a < call->arg_size(); ++a)
                        {
                            llvm::Value* argument = call->getArgOperand(a);
                            if (llvm::isa<llvm::Constant>(argument)) continue;
                            if (llvm::AllocaInst* copy = ByValStructSlot(call, a))
                            {
                                const std::vector<int64_t> whole = allSlots(copy->getAllocatedType());
                                if (!slotsAt(copy, call, &whole, depth + 1)) return false;
                            }
                            else if (argument->getType()->isPointerTy()) Held.push_back(argument);
                            else if (llvm::AllocaInst* coerced = CoercedStructSlot(argument))
                            {
                                auto* load = llvm::cast<llvm::LoadInst>(argument);
                                const std::vector<int64_t> whole = allSlots(coerced->getAllocatedType());
                                if (!slotsAt(load->getPointerOperand(), load, &whole, depth + 1)) return false;
                            }
                            else if (argument->getType()->isAggregateType()
                                     && !valueContents(argument, allSlots(argument->getType()), call, depth + 1))
                                return false;
                        }
                        for (int64_t slot : covered) item.Live.erase(slot);
                        continue;
                    }
                }
                // A closure destructor on a slot about to be overwritten clears it.
                if (IsClosureValueHelperCall(&instruction, "dtor"))
                {
                    auto destination = baseInfo.Offsets.find(llvm::cast<llvm::CallBase>(instruction).getArgOperand(0));
                    if (destination != baseInfo.Offsets.end() && destination->second)
                    {
                        const int64_t size = static_cast<int64_t>(layout_.getTypeAllocSize(compiler_->GetClosureFatPtrType()));
                        for (int64_t slot = *destination->second; slot < *destination->second + size; ++slot)
                            item.Live.erase(slot);
                    }
                    continue;
                }
                auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
                if (intrinsic == nullptr) continue;
                const auto id = intrinsic->getIntrinsicID();
                if (id != llvm::Intrinsic::memcpy && id != llvm::Intrinsic::memmove && id != llvm::Intrinsic::memset)
                    continue;
                auto destination = baseInfo.Offsets.find(intrinsic->getArgOperand(0));
                if (destination == baseInfo.Offsets.end()) continue;
                auto* length = llvm::dyn_cast<llvm::ConstantInt>(intrinsic->getArgOperand(2));
                if (id == llvm::Intrinsic::memset)
                {
                    if (!destination->second || length == nullptr) continue;
                    for (auto it = item.Live.begin(); it != item.Live.end();)
                        it = *it >= *destination->second && *it < *destination->second + length->getSExtValue()
                            ? item.Live.erase(it) : std::next(it);
                    continue;
                }
                if (!destination->second || length == nullptr) return false;
                std::vector<int64_t> covered;
                for (int64_t slot : item.Live)
                    if (slot >= *destination->second && slot < *destination->second + length->getSExtValue())
                        covered.push_back(slot - *destination->second);
                if (covered.empty()) continue;
                if (!slotsAt(intrinsic->getArgOperand(1), &instruction, &covered, depth + 1)) return false;
                for (int64_t r : covered) item.Live.erase(*destination->second + r);
            }
            if (item.Live.empty() || reachedBase) continue;
            for (llvm::BasicBlock* predecessor : llvm::predecessors(item.Block))
            {
                std::set<int64_t> fresh;
                auto& already = seen[predecessor];
                for (int64_t slot : item.Live)
                    if (already.insert(slot).second) fresh.insert(slot);
                if (!fresh.empty()) work.push_back({ predecessor, nullptr, std::move(fresh) });
            }
        }
        return true;
    }
    bool valueContents(llvm::Value* value, const std::vector<int64_t>& slots, llvm::Instruction* point, int depth)
    {
        if (depth > kMaxDepth || budget_ <= 0) return false;
        if (slots.empty() || llvm::isa<llvm::Constant>(value) || llvm::isa<llvm::Argument>(value)) return true;
        if (auto* load = llvm::dyn_cast<llvm::LoadInst>(value))
            return slotsAt(load->getPointerOperand(), load, &slots, depth + 1);
        if (auto* insert = llvm::dyn_cast<llvm::InsertValueInst>(value))
        {
            int64_t offset = 0;
            if (!indexedOffset(insert->getAggregateOperand()->getType(), insert->getIndices(), offset)) return false;
            llvm::Value* inserted = insert->getInsertedValueOperand();
            const int64_t size = static_cast<int64_t>(layout_.getTypeAllocSize(inserted->getType()));
            std::vector<int64_t> inside, outside;
            for (int64_t slot : slots)
                (slot >= offset && slot < offset + size ? inside : outside).push_back(slot);
            if (!inside.empty())
            {
                if (inserted->getType()->isPointerTy())
                {
                    if (!llvm::isa<llvm::Constant>(inserted)) Held.push_back(inserted);
                }
                else if (inserted->getType()->isAggregateType())
                {
                    for (int64_t& slot : inside) slot -= offset;
                    if (!valueContents(inserted, inside, point, depth + 1)) return false;
                }
            }
            return valueContents(insert->getAggregateOperand(), outside, point, depth + 1);
        }
        if (auto* extract = llvm::dyn_cast<llvm::ExtractValueInst>(value))
        {
            int64_t offset = 0;
            if (!indexedOffset(extract->getAggregateOperand()->getType(), extract->getIndices(), offset)) return false;
            std::vector<int64_t> shifted;
            for (int64_t slot : slots) shifted.push_back(slot + offset);
            return valueContents(extract->getAggregateOperand(), shifted, point, depth + 1);
        }
        if (auto* phi = llvm::dyn_cast<llvm::PHINode>(value))
        {
            for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i)
                if (!valueContents(phi->getIncomingValue(i), slots, phi->getIncomingBlock(i)->getTerminator(), depth + 1))
                    return false;
            return true;
        }
        if (auto* select = llvm::dyn_cast<llvm::SelectInst>(value))
            return valueContents(select->getTrueValue(), slots, select, depth + 1)
                && valueContents(select->getFalseValue(), slots, select, depth + 1);
        // A closure copy shares the source closure's environment.
        if (IsClosureValueHelperCall(value, "copy"))
            return valueContents(llvm::cast<llvm::CallBase>(value)->getArgOperand(0), slots, point, depth + 1);
        if (auto* call = llvm::dyn_cast<llvm::CallBase>(value))
        {
            // Elision for a struct result: it may hold whatever its arguments hold.
            auto* structType = llvm::dyn_cast<llvm::StructType>(value->getType());
            if (structType != nullptr && structType->hasName()
                && compiler_->StructValueMayOwnPointees(structType->getName().str()))
                return false;
            for (unsigned a = 0; a < call->arg_size(); ++a)
            {
                llvm::Value* argument = call->getArgOperand(a);
                if (llvm::isa<llvm::Constant>(argument)) continue;
                if (llvm::AllocaInst* copy = ByValStructSlot(call, a))
                {
                    const std::vector<int64_t> whole = allSlots(copy->getAllocatedType());
                    if (!slotsAt(copy, call, &whole, depth + 1)) return false;
                }
                else if (argument->getType()->isPointerTy()) Held.push_back(argument);
                else if (llvm::AllocaInst* coerced = CoercedStructSlot(argument))
                {
                    auto* load = llvm::cast<llvm::LoadInst>(argument);
                    const std::vector<int64_t> whole = allSlots(coerced->getAllocatedType());
                    if (!slotsAt(load->getPointerOperand(), load, &whole, depth + 1)) return false;
                }
                else if (argument->getType()->isAggregateType()
                         && !valueContents(argument, allSlots(argument->getType()), call, depth + 1))
                    return false;
            }
            return true;
        }
        return false;
    }
};

static llvm::Value* UntagClosureEnv(llvm::Value* value)
    {
        for (int step = 0; step < 8; ++step)
        {
            value = value->stripPointerCasts();
            if (auto* cast = llvm::dyn_cast<llvm::CastInst>(value);
                cast != nullptr && (llvm::isa<llvm::IntToPtrInst>(cast) || llvm::isa<llvm::PtrToIntInst>(cast)))
            {
                value = cast->getOperand(0);
                continue;
            }
            if (auto* binary = llvm::dyn_cast<llvm::BinaryOperator>(value);
                binary != nullptr && binary->getOpcode() == llvm::Instruction::And
                && llvm::isa<llvm::ConstantInt>(binary->getOperand(1)))
            {
                value = binary->getOperand(0);
                continue;
            }
            break;
        }
        return value;
    }

// The values stored into a __closure_env_new environment's capture slots.
static void ClosureEnvCaptures(const std::vector<llvm::CallBase*>& envs, std::vector<llvm::Value*>& captures)
    {
        for (llvm::CallBase* env : envs)
        {
            std::vector<llvm::Value*> derived{env};
            std::unordered_set<const llvm::Value*> derivedSeen{env};
            for (size_t i = 0; i < derived.size(); ++i)
                for (llvm::User* user : derived[i]->users())
                {
                    if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                    {
                        if (store->getPointerOperand() == derived[i]) captures.push_back(store->getValueOperand());
                        continue;
                    }
                    if (llvm::isa<llvm::GetElementPtrInst>(user) || llvm::isa<llvm::CastInst>(user)
                        || (llvm::isa<llvm::BinaryOperator>(user)
                            && llvm::cast<llvm::BinaryOperator>(user)->getOpcode() == llvm::Instruction::And))
                        if (derivedSeen.insert(user).second) derived.push_back(user);
                }
        }
    }

// The __closure_env_new call a (tagged) environment pointer comes from, else null.
static llvm::CallBase* ClosureEnvAllocation(llvm::Value* value, LLVMBackend* compiler)
    {
        llvm::Function* envNew = compiler->GetFunction("__closure_env_new");
        auto* env = llvm::dyn_cast<llvm::CallBase>(UntagClosureEnv(value));
        if (envNew == nullptr || env == nullptr || env->getCalledFunction() != envNew) return nullptr;
        return env;
    }

// T522: set when a frame verdict comes from provenance the model cannot follow (a budget or depth
// limit, an unresolved closure environment, an unknown aggregate producer), not a named local.
static thread_local bool t_unprovenRoot = false;
bool TakeUnprovenPointerRoot()
{
    const bool unproven = t_unprovenRoot;
    t_unprovenRoot = false;
    return unproven;
}
// T522: set while an interface return is classified; calls to later-defined callees land here.
static thread_local std::vector<LLVMBackend::DeferredCallBorrow>* t_deferredBorrows = nullptr;
// True while the value being rooted is the returned interface's own object (its data pointer
// flows unchanged): a fat call result there points at an implementer of the returned interface.
static thread_local bool t_returnedObject = false;
struct DeferredBorrowCollector
{
    std::vector<LLVMBackend::DeferredCallBorrow>* saved;
    bool savedObject;
    explicit DeferredBorrowCollector(std::vector<LLVMBackend::DeferredCallBorrow>* target,
                                     bool returnedObject = false)
        : saved(t_deferredBorrows), savedObject(t_returnedObject)
    {
        t_deferredBorrows = target;
        t_returnedObject = returnedObject && target != nullptr;
    }
    ~DeferredBorrowCollector()
    {
        t_deferredBorrows = saved;
        t_returnedObject = savedObject;
    }
};
struct ReturnedObjectScope
{
    bool saved;
    explicit ReturnedObjectScope(bool value) : saved(t_returnedObject) { t_returnedObject = value; }
    ~ReturnedObjectScope() { t_returnedObject = saved; }
};

static PointerValueRootKind PointerValueRootImpl(llvm::Value* value, LLVMBackend* compiler,
                                                 llvm::Function* fn, int depth,
                                                 PointerValueRootQuery& query,
                                                 bool* everyArmLocal,
                                                 bool frameLocalContainer)
    {
        using K = PointerValueRootKind;
        auto worst = [](K a, K b) { return static_cast<int>(a) >= static_cast<int>(b) ? a : b; };
        // The returned object survives only phi/select arms; every other step roots something else.
        const bool returnedObject = t_returnedObject;
        ReturnedObjectScope nestedRoots(false);
        auto rootForAlternative = [&](llvm::Value* alternative) {
            ReturnedObjectScope arm(returnedObject);
            PointerValueRootAnswer answer = PointerValueRoot(
                alternative, compiler, fn, depth + 1, query, frameLocalContainer);
            if (everyArmLocal != nullptr
                && (!IsLocalPointerRoot(answer.Kind) || !answer.EveryArmLocal))
                *everyArmLocal = false;
            return answer.Kind;
        };
        llvm::Value* v = value->stripPointerCasts();
        // A non-zero index, or a step into an aggregate (more than one index, e.g. `&hp->first`), is an
        // interior pointer: a borrow of whatever owns the base. `&t[0]` is one zero index: a move.
        bool interior = false;
        while (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(v))
        {
            if (gep->getNumIndices() > 1) interior = true;
            for (const llvm::Value* idx : gep->indices())
                if (!llvm::isa<llvm::ConstantInt>(idx) || !llvm::cast<llvm::ConstantInt>(idx)->isZero())
                    interior = true;
            v = gep->getPointerOperand()->stripPointerCasts();
        }
        if (auto* phi = llvm::dyn_cast<llvm::PHINode>(v))
        {
            K kind = K::Borrowed;
            for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
                kind = worst(kind, rootForAlternative(phi->getIncomingValue(i)));
            return kind;
        }
        if (auto* sel = llvm::dyn_cast<llvm::SelectInst>(v))
            return worst(rootForAlternative(sel->getTrueValue()),
                         rootForAlternative(sel->getFalseValue()));
        if (llvm::isa<llvm::ConstantPointerNull>(v) || llvm::isa<llvm::GlobalVariable>(v))
            return K::Borrowed;
        // A lambda's own (tag-stripped) environment parameter: its captures belong to the creator.
        if (llvm::isa<llvm::IntToPtrInst>(v) && llvm::isa<llvm::Argument>(UntagClosureEnv(v)))
            return K::Borrowed;
        // Direct calls use the callee fact; indirect calls carry their declared pointer-return
        // ownership because the LLVM call type alone has no CFlat move bit.
        // T522 elision: a non-owning result borrows from every pointer argument (receiver included)
        // unless the callee summary narrows it to some (a parameter) or none (fresh, a global).
        if (auto* call = llvm::dyn_cast<llvm::CallBase>(v))
        {
            llvm::Function* callee = call->getCalledFunction();
            if (callee == nullptr && call->getMetadata("cflat.borrowed-pointer-return") == nullptr)
                return K::Owned;
            if (callee != nullptr && compiler->CalleeOwnsReturnFact(callee)) return K::Owned;
            if (callee != nullptr && callee->isIntrinsic()) return K::Borrowed;
            /*
             * Type narrowing (ruling 2026-10-25, round 5): the returned interface's object is an
             * implementer of it, so an argument whose type reaches no implementer cannot be its
             * borrow source. Only for the returned object itself; nested roots are not narrowed.
             */
            const std::string& resultInterface = compiler->currentFunctionReturnTypeName;
            const bool typeNarrow = returnedObject && t_deferredBorrows != nullptr
                && call->getType() == compiler->GetFatPtrType() && compiler->IsInterfaceType(resultInterface);
            // A may-borrow of several arguments is never a proof that the result is local.
            if (everyArmLocal != nullptr) *everyArmLocal = false;
            std::vector<std::pair<unsigned, unsigned>> indexes;
            bool pendingSummary = false;
            const bool narrowed = callee != nullptr
                && compiler->ReturnBorrowedArguments(callee, indexes, &pendingSummary);
            if (!narrowed)
                for (unsigned i = 0; i < call->arg_size(); ++i)
                    indexes.push_back({ i, 0u });
            K kind = K::Borrowed;
            auto borrowOf = [&](llvm::Value* argument) {
                if (narrowed)
                    query.StatementOwnedTempRoot = query.StatementOwnedTempRoot
                        || compiler->IsStatementOwnedTempBorrowSource(argument);
                K argumentRoot = PointerValueRoot(argument, compiler, fn, depth + 1, query).Kind;
                if (argumentRoot == K::FrameLocal)
                {
                    // A method on a local owner (`u.get()` on a local unique): a borrow of that owner.
                    const auto* owner = compiler->FindVariableByStorage(argument->stripPointerCasts());
                    if (owner != nullptr
                        && (owner->IsOwning || owner->TypeAndValue.IsUnique
                            || compiler->IsCoreUniqueType(owner->TypeAndValue.TypeName)))
                        argumentRoot = K::AliasOfOwnedLocal;
                }
                if (argumentRoot == K::OwnedLocal || argumentRoot == K::AliasOfOwnedLocal)
                    kind = worst(kind, K::AliasOfOwnedLocal);
                else if (argumentRoot == K::FrameLocal)
                    kind = worst(kind, K::FrameLocal);
            };
            // A pointer read through a local pointer slot's address (`g(&q)` reading `*pp`) borrows
            // what the slot holds, not the slot.
            // Memory outside this frame whose address is a proven borrow (a parameter, a global).
            auto borrowedMemory = [&](llvm::Value* address) {
                // A pointer parameter used directly (a lambda's environment) addresses caller memory.
                if (llvm::isa<llvm::Argument>(llvm::getUnderlyingObject(address))) return true;
                return PointerValueRoot(address, compiler, fn, depth + 1, query).Kind == K::Borrowed;
            };
            // `pointee`: the struct the callee's parameter points at (null = the whole aggregate).
            std::function<void(llvm::Value*, unsigned, int, llvm::Type*)> borrowAt =
                [&](llvm::Value* argument, unsigned derefs, int guard, llvm::Type* pointee) {
                    auto* slot = llvm::dyn_cast<llvm::AllocaInst>(argument->stripPointerCasts());
                    if (derefs > 0 && guard < 4 && slot != nullptr && slot->getAllocatedType()->isPointerTy())
                    {
                        std::vector<llvm::Value*> held;
                        bool plain = true;
                        for (llvm::User* user : slot->users())
                        {
                            if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                                store != nullptr && store->getPointerOperand()->stripPointerCasts() == slot)
                                held.push_back(store->getValueOperand());
                            else if (!llvm::isa<llvm::LoadInst>(user) && !llvm::isa<llvm::IntrinsicInst>(user)
                                     && user != call)
                                plain = false;
                        }
                        if (plain && !held.empty())
                        {
                            for (llvm::Value* value : held)
                                if (!llvm::isa<llvm::Constant>(value)) borrowAt(value, derefs - 1, guard + 1, nullptr);
                            return;
                        }
                    }
                    // A pointer field read through a local aggregate's address (`get(&h)`, `get(&a[1])`
                    // reading `h->p`): what those fields hold at the call (AggregateFieldContents).
                    if (derefs > 0 && guard < 4 && AggregateFieldContents::AggregateBase(argument) != nullptr)
                    {
                        AggregateFieldContents contents(compiler, call->getModule()->getDataLayout(), call,
                                                        borrowedMemory);
                        bool plain = contents.ThroughAddress(argument, call, pointee);
                        if (!plain) t_unprovenRoot = true;
                        std::vector<llvm::Value*> held = std::move(contents.Held);
                        // A field holding anything but a proven borrow (a global, a parameter) may be
                        // owned by the aggregate and die with it.
                        if (plain && derefs == 1)
                            for (llvm::Value* value : held)
                                if (!llvm::isa<llvm::Constant>(value)
                                    && PointerValueRoot(value, compiler, fn, depth + 1, query).Kind != K::Borrowed)
                                    plain = false;
                        if (plain)
                        {
                            for (llvm::Value* value : held)
                                if (!llvm::isa<llvm::Constant>(value) && derefs > 1)
                                    borrowAt(value, derefs - 1, guard + 1, nullptr);
                            return;
                        }
                    }
                    borrowOf(argument);
                };
            // A by-value struct argument lends what its pointer fields hold (unproven = frame).
            // A struct passed in its C ABI form (`load i64` / `load [2 x i64]` of an abi.coerce copy).
            // A struct in its ABI form: a coerced load, or a byval hidden pointer to a copy.
            auto coercedSlot = [&](llvm::Value* argument) -> llvm::AllocaInst* {
                if (llvm::AllocaInst* slot = CoercedStructSlot(argument)) return slot;
                for (unsigned i = 0; i < call->arg_size(); ++i)
                    if (call->getArgOperand(i) == argument)
                        if (llvm::AllocaInst* copy = ByValStructSlot(call, i)) return copy;
                return nullptr;
            };
            /*
             * A closure value (an argument, a field, a capture) lends whatever its environment
             * captured: pointer captures directly, struct / interface / closure captures through
             * their fields. The environment itself is fresh heap.
             */
            std::function<void(llvm::Value*)> borrowAggregate;
            int captureNesting = 0;
            auto borrowCaptured = [&](llvm::Value* captured) {
                if (llvm::isa<llvm::Constant>(captured)) return;
                if (captured->getType()->isPointerTy()) borrowOf(captured);
                else if (captured->getType()->isAggregateType() || CoercedStructSlot(captured) != nullptr)
                    borrowAggregate(captured);
            };
            auto borrowHeld = [&](llvm::Value* held) {
                llvm::CallBase* env = ClosureEnvAllocation(held, compiler);
                if (env == nullptr)
                {
                    borrowOf(held);
                    return;
                }
                if (++captureNesting > 8)
                {
                    kind = worst(kind, K::FrameLocal);  // unproven
                    t_unprovenRoot = true;
                }
                else
                {
                    std::vector<llvm::Value*> captures;
                    ClosureEnvCaptures({ env }, captures);
                    for (llvm::Value* captured : captures) borrowCaptured(captured);
                }
                --captureNesting;
            };
            borrowAggregate = [&](llvm::Value* argument) {
                query.StatementOwnedTempRoot = query.StatementOwnedTempRoot
                    || compiler->IsStatementOwnedTempBorrowSource(argument);
                AggregateFieldContents contents(compiler, call->getModule()->getDataLayout(), nullptr,
                                                borrowedMemory);
                llvm::AllocaInst* coerced = coercedSlot(argument);
                llvm::Instruction* point = CoercedStructSlot(argument) != nullptr
                    ? llvm::cast<llvm::Instruction>(argument) : call;
                if (coerced != nullptr ? !contents.ThroughAddress(coerced, point)
                                       : !contents.OfValue(argument, call))
                {
                    kind = worst(kind, K::FrameLocal);
                    t_unprovenRoot = true;
                    return;
                }
                for (llvm::Value* held : contents.Held) borrowHeld(held);
            };
            // The closure value a closure call's environment argument was taken from (`fat.env`).
            auto closureFatOf = [&](llvm::Value* argument) -> llvm::Value* {
                auto* extract = llvm::dyn_cast<llvm::ExtractValueInst>(UntagClosureEnv(argument));
                if (extract == nullptr || extract->getNumIndices() != 1 || extract->getIndices()[0] != 1
                    || extract->getAggregateOperand()->getType() != compiler->GetClosureFatPtrType())
                    return nullptr;
                return extract->getAggregateOperand();
            };
            auto isAggregateArgument = [&](llvm::Value* argument) {
                return (argument->getType()->isAggregateType() && !llvm::isa<llvm::Constant>(argument))
                    || coercedSlot(argument) != nullptr;
            };
            /*
             * The callee's body is emitted later: at an interface return, record what each argument
             * would contribute at each dereference depth and let the module-end summary pick (no
             * contribution now). Elsewhere the full-argument borrow below stands.
             */
            // An argument the type narrowing drops now may become reachable through an implementer
            // registered later: decide at module end like a pending summary.
            // The CFlat type of argument i: a byval argument is its struct, not the hidden pointer.
            auto reachKey = [&](unsigned i) {
                if (llvm::AllocaInst* copy = ByValStructSlot(call, i))
                    if (auto* byValType = llvm::cast<llvm::StructType>(copy->getAllocatedType()); byValType->hasName())
                        return byValType->getName().str();
                return compiler->ArgumentReachTypeKey(call->getArgOperand(i));
            };
            bool typeExcludes = false;
            if (typeNarrow && !narrowed)
                for (unsigned i = 0; i < call->arg_size() && !typeExcludes; ++i)
                {
                    llvm::Value* argument = call->getArgOperand(i);
                    if ((isAggregateArgument(argument)
                         || (argument->getType()->isPointerTy() && !llvm::isa<llvm::Constant>(argument)))
                        && !compiler->TypeMayReachImplementer(reachKey(i), resultInterface))
                        typeExcludes = true;
                }
            if ((pendingSummary || typeExcludes) && t_deferredBorrows != nullptr)
            {
                LLVMBackend::DeferredCallBorrow record;
                record.Callee = callee;
                record.NarrowByInterface = typeNarrow;
                for (unsigned i = 0; i < call->arg_size(); ++i)
                {
                    std::array<int, 5> roots{};
                    std::vector<LLVMBackend::DeferredCallBorrow> children;
                    llvm::Value* argument = call->getArgOperand(i);
                    record.ArgTypes.push_back(reachKey(i));
                    if (isAggregateArgument(argument))
                    {
                        DeferredBorrowCollector nested(&children);
                        kind = K::Borrowed;
                        const bool unprovenBefore = t_unprovenRoot;
                        t_unprovenRoot = false;
                        borrowAggregate(argument);
                        roots.fill(kind == K::FrameLocal ? (t_unprovenRoot ? 4 : 3) : kind == K::AliasOfOwnedLocal ? 2 : 0);
                        t_unprovenRoot = unprovenBefore;
                    }
                    else if (argument->getType()->isPointerTy() && !llvm::isa<llvm::Constant>(argument))
                    {
                        // Deferred calls inside this argument hang off it, not off the return.
                        std::vector<LLVMBackend::DeferredCallBorrow> repeated;
                        for (unsigned derefs = 0; derefs < roots.size(); ++derefs)
                        {
                            // Nested records once (depth 0); deeper passes would only repeat them.
                            DeferredBorrowCollector nested(derefs == 0 ? &children : &repeated);
                            kind = K::Borrowed;
                            const bool unprovenBefore = t_unprovenRoot;
                            t_unprovenRoot = false;
                            // A closure call borrows what its closure value holds (its captures).
                            if (llvm::Value* closure = callee == nullptr ? closureFatOf(argument) : nullptr;
                                derefs == 0 && closure != nullptr)
                                borrowAggregate(closure);
                            else if (derefs == 0) borrowOf(argument);
                            else borrowAt(argument, derefs, 0, callee == nullptr ? nullptr : compiler->ParameterPointeeStructType(callee, i));
                            roots[derefs] = kind == K::FrameLocal ? (t_unprovenRoot ? 4 : 3)
                                : kind == K::AliasOfOwnedLocal ? 2 : 0;
                            t_unprovenRoot = unprovenBefore;
                        }
                    }
                    record.ArgRoots.push_back(roots);
                    record.ArgChildren.push_back(std::move(children));
                }
                t_deferredBorrows->push_back(std::move(record));
                return K::Borrowed;
            }
            for (auto [index, derefs] : indexes)
            {
                if (index >= call->arg_size()) continue;
                llvm::Value* argument = call->getArgOperand(index);
                // An aggregate argument (struct, interface, closure) lends what it holds.
                if (isAggregateArgument(argument))
                {
                    borrowAggregate(argument);
                    continue;
                }
                if (!argument->getType()->isPointerTy() || llvm::isa<llvm::Constant>(argument)) continue;
                if (derefs > 0)
                {
                    borrowAt(argument, derefs, 0, callee == nullptr ? nullptr : compiler->ParameterPointeeStructType(callee, index));
                    continue;
                }
                // A closure call borrows what its closure value holds (its captures); an
                // environment the contents model cannot resolve is unproven, never a heap borrow.
                if (llvm::Value* closure = callee == nullptr ? closureFatOf(argument) : nullptr)
                {
                    borrowAggregate(closure);
                    continue;
                }
                borrowOf(argument);
            }
            return kind;
        }
        auto* load = llvm::dyn_cast<llvm::LoadInst>(v);
        if (load == nullptr)
            return llvm::isa<llvm::AllocaInst>(v) ? K::FrameLocal : K::Undecided;
        if (compiler->IsOwningValue(load))
        {
            if (!llvm::isa<llvm::AllocaInst>(load->getPointerOperand()->stripPointerCasts()))
                return K::Owned;
            return interior ? K::AliasOfOwnedLocal : K::OwnedLocal;
        }
        llvm::Value* addr = load->getPointerOperand()->stripPointerCasts();
        // A lambda capture is registered as argument storage but lives in the environment, which
        // the body may write (`px = &y;`): only a parameter's own slot is a plain borrow.
        if (llvm::isa<llvm::GlobalVariable>(addr)
            || (compiler->IsFunctionArgumentStorage(addr) && llvm::isa<llvm::AllocaInst>(addr)))
            return K::Borrowed;
        /*
         * T522: a container whose address escapes (into a call, a pointer slot, pointer
         * arithmetic) or that lives behind a local pointer slot (`h->p`, heap or caller object).
         * Derived = every address of it this function can name (GEPs, casts, reloads of pointer
         * slots holding one, aggregate-copy and memcpy sources). Calls = calls receiving one.
         * Escaping to non-frame memory (a global, the heap) makes every call a writer.
         */
        struct ContainerInfo
        {
            std::unordered_set<const llvm::Value*> Derived;
            std::unordered_set<const llvm::CallBase*> Calls;
            std::vector<llvm::StoreInst*> Stores;
            bool Global = false;
        };
        auto analyzeContainer = [&](std::vector<llvm::Value*> roots) {
            ContainerInfo info;
            info.Derived.insert(roots.begin(), roots.end());
            auto push = [&](llvm::Value* next) {
                if (info.Derived.insert(next).second) roots.push_back(next);
            };
            for (size_t i = 0; i < roots.size(); ++i)
            {
                if (roots.size() > 256)
                {
                    info.Global = true;
                    break;
                }
                llvm::Value* current = roots[i];
                for (llvm::Use& use : current->uses())
                {
                    llvm::User* user = use.getUser();
                    if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                    {
                        if (store->getPointerOperand() == current)
                        {
                            llvm::Value* stored = store->getValueOperand();
                            if (stored->getType()->isPointerTy()) info.Stores.push_back(store);
                            else if (auto* copied = llvm::dyn_cast<llvm::LoadInst>(stored);
                                     copied != nullptr && copied->getType()->isAggregateType())
                                push(const_cast<llvm::Value*>(llvm::getUnderlyingObject(copied->getPointerOperand())));
                            continue;
                        }
                        auto* slot = llvm::dyn_cast<llvm::AllocaInst>(
                            store->getPointerOperand()->stripPointerCasts());
                        if (slot == nullptr || !slot->getAllocatedType()->isPointerTy())
                        {
                            info.Global = true;
                            continue;
                        }
                        for (llvm::User* slotUser : slot->users())
                            if (auto* reload = llvm::dyn_cast<llvm::LoadInst>(slotUser);
                                reload != nullptr && reload->getPointerOperand()->stripPointerCasts() == slot)
                                push(reload);
                        continue;
                    }
                    if (auto* memory = llvm::dyn_cast<llvm::MemTransferInst>(user))
                    {
                        if (memory->getRawDest() == current)
                            push(const_cast<llvm::Value*>(llvm::getUnderlyingObject(memory->getRawSource())));
                        continue;
                    }
                    if (llvm::isa<llvm::IntrinsicInst>(user) || llvm::isa<llvm::LoadInst>(user)
                        || llvm::isa<llvm::ICmpInst>(user) || llvm::isa<llvm::PtrToIntInst>(user)
                        || llvm::isa<llvm::ReturnInst>(user) || llvm::isa<llvm::InsertValueInst>(user))
                        continue;
                    if (auto* call = llvm::dyn_cast<llvm::CallBase>(user))
                    {
                        info.Calls.insert(call);
                        continue;
                    }
                    if (llvm::isa<llvm::GetElementPtrInst>(user) || llvm::isa<llvm::CastInst>(user)
                        || llvm::isa<llvm::PHINode>(user) || llvm::isa<llvm::SelectInst>(user))
                    {
                        push(user);
                        continue;
                    }
                    info.Global = true;
                }
            }
            if (info.Global)
                for (auto& block : *fn)
                    for (auto& instruction : block)
                        if (auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                            call != nullptr && !llvm::isa<llvm::IntrinsicInst>(call))
                            info.Calls.insert(call);
            return info;
        };
        // What a store into the container may leave there (an owning local stored is a move).
        auto storedIntoContainer = [&](llvm::StoreInst* store) {
            K stored = PointerValueRoot(store->getValueOperand(), compiler, fn, depth + 1, query).Kind;
            if (store->getMetadata(fn->getContext().getMDKindID(
                    "cflat.interface-return.alias-owned-local")) != nullptr)
                stored = K::AliasOfOwnedLocal;
            return stored == K::OwnedLocal ? K::Owned : stored;
        };
        // What a call receiving the container may store into it: its other pointer arguments.
        auto passedIntoContainer = [&](const llvm::CallBase* call, const ContainerInfo& info) {
            K result = K::Borrowed;
            for (unsigned a = 0; a < call->arg_size(); ++a)
            {
                const llvm::Value* argument = call->getArgOperand(a);
                // A byval struct copy hands over what its fields hold, not the copy's address.
                if (llvm::AllocaInst* copy = ByValStructSlot(call, a))
                {
                    AggregateFieldContents contents(compiler, call->getModule()->getDataLayout(), nullptr,
                        [&](llvm::Value* address) {
                            return PointerValueRoot(address, compiler, fn, depth + 1, query).Kind == K::Borrowed;
                        });
                    if (!contents.ThroughAddress(copy, const_cast<llvm::CallBase*>(call)))
                    {
                        result = worst(result, K::FrameLocal);
                        continue;
                    }
                    for (llvm::Value* held : contents.Held)
                    {
                        K passed = PointerValueRoot(held, compiler, fn, depth + 1, query).Kind;
                        if (passed == K::OwnedLocal) passed = K::AliasOfOwnedLocal;
                        if (passed == K::FrameLocal || passed == K::AliasOfOwnedLocal) result = worst(result, passed);
                    }
                    continue;
                }
                if (!argument->getType()->isPointerTy() || llvm::isa<llvm::Constant>(argument)
                    || info.Derived.count(argument) != 0 || info.Derived.count(argument->stripPointerCasts()) != 0)
                    continue;
                K passed = PointerValueRoot(const_cast<llvm::Value*>(argument), compiler, fn, depth + 1, query).Kind;
                if (passed == K::OwnedLocal) passed = K::AliasOfOwnedLocal;
                if (passed == K::FrameLocal || passed == K::AliasOfOwnedLocal) result = worst(result, passed);
            }
            return result;
        };
        // Flow-insensitive answer: every store into it and every writer call.
        auto worstOverContainer = [&](const ContainerInfo& info) {
            if (everyArmLocal != nullptr) *everyArmLocal = false;
            K result = K::Borrowed;
            for (llvm::StoreInst* store : info.Stores) result = worst(result, storedIntoContainer(store));
            for (const llvm::CallBase* call : info.Calls) result = worst(result, passedIntoContainer(call, info));
            return result;
        };
        auto rootFromAggregateSlot = [&](llvm::Value* readAddress) {
            if (fn == nullptr) return K::Borrowed;
            llvm::Value* base = readAddress;
            while (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(base))
                base = gep->getPointerOperand()->stripPointerCasts();
            auto* aggregate = llvm::dyn_cast<llvm::AllocaInst>(base);
            const bool localAggregate = aggregate != nullptr
                && (llvm::isa<llvm::ArrayType>(aggregate->getAllocatedType())
                    || llvm::isa<llvm::StructType>(aggregate->getAllocatedType()));
            // Set when the container is tracked through its derived addresses (T522).
            ContainerInfo tracked;
            bool isTracked = false;
            if (!localAggregate && !frameLocalContainer)
            {
                isTracked = true;
                // A heap (or caller) object reached through a local pointer slot `h->p`.
                auto* baseLoad = llvm::dyn_cast<llvm::LoadInst>(base);
                auto* pointerSlot = baseLoad == nullptr ? nullptr
                    : llvm::dyn_cast<llvm::AllocaInst>(baseLoad->getPointerOperand()->stripPointerCasts());
                if (pointerSlot != nullptr && !pointerSlot->getAllocatedType()->isPointerTy())
                    return K::Borrowed;
                std::vector<llvm::Value*> objects;
                // A pointer parameter used directly (a lambda's environment `px` capture): the
                // object is the caller's, but this body's own writes into it still count.
                if (pointerSlot == nullptr && llvm::isa<llvm::Argument>(base))
                    objects.push_back(base);
                else if (pointerSlot == nullptr)
                {
                    // A deeper chain (`hh->h->p`): every load with the same access path (root
                    // slot, argument or global, then constant GEPs and loads) names the object.
                    auto accessPath = [](llvm::Value* value, std::vector<const void*>& path) {
                        for (int step = 0; step < 16; ++step)
                        {
                            value = value->stripPointerCasts();
                            if (llvm::isa<llvm::AllocaInst>(value) || llvm::isa<llvm::Argument>(value)
                                || llvm::isa<llvm::GlobalVariable>(value))
                            {
                                path.push_back(value);
                                return true;
                            }
                            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(value))
                            {
                                path.push_back(nullptr);
                                value = load->getPointerOperand();
                                continue;
                            }
                            auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(value);
                            if (gep == nullptr) return false;
                            path.push_back(gep->getSourceElementType());
                            for (const llvm::Value* index : gep->indices())
                            {
                                auto* constant = llvm::dyn_cast<llvm::ConstantInt>(index);
                                if (constant == nullptr) return false;
                                path.push_back(reinterpret_cast<const void*>(
                                    static_cast<uintptr_t>(constant->getZExtValue() + 1)));
                            }
                            value = gep->getPointerOperand();
                        }
                        return false;
                    };
                    std::vector<const void*> basePath;
                    if (baseLoad == nullptr || !accessPath(baseLoad, basePath)) return K::Borrowed;
                    for (auto& block : *fn)
                        for (auto& instruction : block)
                            if (auto* other = llvm::dyn_cast<llvm::LoadInst>(&instruction);
                                other != nullptr && other->getType()->isPointerTy())
                            {
                                std::vector<const void*> otherPath;
                                if (accessPath(other, otherPath) && otherPath == basePath)
                                    objects.push_back(other);
                            }
                    // Stores replacing the object (`hh->h = new H()`) hand it in too.
                    for (llvm::Value* object : std::vector<llvm::Value*>(objects))
                        for (llvm::User* user : llvm::cast<llvm::LoadInst>(object)->getPointerOperand()->users())
                            if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                                store != nullptr && !llvm::isa<llvm::Constant>(store->getValueOperand())
                                && store->getPointerOperand() == llvm::cast<llvm::LoadInst>(object)->getPointerOperand())
                                objects.push_back(store->getValueOperand()->stripPointerCasts());
                }
                else
                for (llvm::User* user : pointerSlot->users())
                {
                    if (auto* reload = llvm::dyn_cast<llvm::LoadInst>(user);
                        reload != nullptr && reload->getPointerOperand()->stripPointerCasts() == pointerSlot)
                        objects.push_back(reload);
                    else if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                             store != nullptr && store->getPointerOperand()->stripPointerCasts() == pointerSlot)
                    {
                        llvm::Value* object = store->getValueOperand()->stripPointerCasts();
                        if (!llvm::isa<llvm::Constant>(object) && !llvm::isa<llvm::Argument>(object))
                            objects.push_back(object);
                    }
                }
                tracked = analyzeContainer(std::move(objects));
                if (tracked.Global) return worstOverContainer(tracked);
                if (everyArmLocal != nullptr) *everyArmLocal = false;
            }
            if (!localAggregate && !isTracked)
            {
                K baseRoot = PointerValueRoot(base, compiler, fn, depth + 1, query).Kind;
                // Trace non-alloca containers only through a proven frame-local root.
                if (!IsLocalPointerRoot(baseRoot))
                    return K::Borrowed;
                if (auto* load = llvm::dyn_cast<llvm::LoadInst>(base))
                {
                    const auto* owner = compiler->FindVariableByStorage(
                        load->getPointerOperand()->stripPointerCasts());
                    const bool uniqueOwner = owner != nullptr
                        && (owner->TypeAndValue.IsUnique || owner->IsUniqueFieldAlias
                            || compiler->IsCoreUniqueType(owner->TypeAndValue.TypeName));
                    if (owner != nullptr && owner->IsOwning && !uniqueOwner)
                        return K::Borrowed;
                }
            }

            const llvm::DataLayout& layout = fn->getParent()->getDataLayout();
            if (localAggregate)
            {
                llvm::SmallVector<const llvm::Value*, 16> aggregateUses{aggregate};
                llvm::SmallPtrSet<const llvm::Value*, 16> seenAggregateUses{aggregate};
                while (!aggregateUses.empty())
                {
                    const llvm::Value* current = aggregateUses.pop_back_val();
                    for (const llvm::User* user : current->users())
                    {
                        if (const auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user);
                            gep != nullptr && gep->getPointerOperand() == current)
                        {
                            if (seenAggregateUses.insert(gep).second) aggregateUses.push_back(gep);
                            continue;
                        }
                        if (const auto* cast = llvm::dyn_cast<llvm::CastInst>(user);
                            cast != nullptr && cast->getOperand(0) == current)
                        {
                            if (seenAggregateUses.insert(cast).second) aggregateUses.push_back(cast);
                            continue;
                        }
                        if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(user);
                            load != nullptr && load->getPointerOperand() == current)
                            continue;
                        if (const auto* memory = llvm::dyn_cast<llvm::MemIntrinsic>(user);
                            memory != nullptr && memory->getDest()->stripPointerCasts() == current)
                            continue;
                        if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                            store != nullptr && store->getPointerOperand() == current)
                            continue;
                        if (const auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(user);
                            intrinsic != nullptr
                            && (intrinsic->getIntrinsicID() == llvm::Intrinsic::dbg_declare
                                || intrinsic->getIntrinsicID() == llvm::Intrinsic::dbg_value
                                || intrinsic->getIntrinsicID() == llvm::Intrinsic::lifetime_start
                                || intrinsic->getIntrinsicID() == llvm::Intrinsic::lifetime_end))
                            continue;
                        // An escaped address: a callee, a pointer slot or arithmetic may write it.
                        tracked = analyzeContainer({ aggregate });
                        if (tracked.Global) return worstOverContainer(tracked);
                        if (everyArmLocal != nullptr) *everyArmLocal = false;
                        isTracked = true;
                        aggregateUses.clear();
                        break;
                    }
                }
            }

            // Stores per alloca slot (count, first stored value in function order), built in one
            // pass on first use: per-store whole-function rescans made large functions quadratic.
            struct SlotStores { unsigned Count = 0; llvm::StoreInst* First = nullptr; };
            std::unordered_map<const llvm::Value*, SlotStores> slotStores;
            bool slotStoresBuilt = false;
            auto storesOf = [&](const llvm::Value* slot) -> const SlotStores& {
                if (!slotStoresBuilt)
                {
                    slotStoresBuilt = true;
                    for (auto& block : *fn)
                        for (auto& instruction : block)
                            if (auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
                            {
                                const llvm::Value* target = store->getPointerOperand()->stripPointerCasts();
                                if (!llvm::isa<llvm::AllocaInst>(target)) continue;
                                SlotStores& entry = slotStores[target];
                                if (entry.Count++ == 0) entry.First = store;
                            }
                }
                static const SlotStores none;
                auto found = slotStores.find(slot);
                return found == slotStores.end() ? none : found->second;
            };
            auto storesToSlot = [&](const llvm::Value* slot) { return storesOf(slot).Count; };
            std::function<bool(llvm::Value*, llvm::Value*)> sameIndex =
                [&](llvm::Value* left, llvm::Value* right) {
                    if (left == right) return true;
                    auto* leftLoad = llvm::dyn_cast<llvm::LoadInst>(left);
                    auto* rightLoad = llvm::dyn_cast<llvm::LoadInst>(right);
                    if (leftLoad != nullptr && rightLoad != nullptr)
                    {
                        llvm::Value* leftSlot = leftLoad->getPointerOperand()->stripPointerCasts();
                        llvm::Value* rightSlot = rightLoad->getPointerOperand()->stripPointerCasts();
                        if (leftSlot != rightSlot || !llvm::isa<llvm::AllocaInst>(leftSlot)) return false;
                        return storesToSlot(leftSlot) == 1;
                    }
                    auto* leftCast = llvm::dyn_cast<llvm::CastInst>(left);
                    auto* rightCast = llvm::dyn_cast<llvm::CastInst>(right);
                    return leftCast != nullptr && rightCast != nullptr
                        && leftCast->getOpcode() == rightCast->getOpcode()
                        && sameIndex(leftCast->getOperand(0), rightCast->getOperand(0));
                };
            auto isStablePointerGetter = [](llvm::Function* function) {
                if (function == nullptr || function->isDeclaration()
                    || !function->getReturnType()->isPointerTy()) return false;
                llvm::LoadInst* returnedLoad = nullptr;
                llvm::ReturnInst* returned = nullptr;
                for (auto& block : *function)
                    for (auto& instruction : block)
                    {
                        if (auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
                        {
                            if (returnedLoad != nullptr || !load->getType()->isPointerTy()
                                || !llvm::isa<llvm::Argument>(load->getPointerOperand()))
                                return false;
                            returnedLoad = load;
                        }
                        else if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(&instruction))
                        {
                            if (returned != nullptr) return false;
                            returned = ret;
                        }
                        else if (auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
                                 intrinsic == nullptr
                                 || (intrinsic->getIntrinsicID() != llvm::Intrinsic::dbg_declare
                                     && intrinsic->getIntrinsicID() != llvm::Intrinsic::dbg_value))
                            return false;
                    }
                return returnedLoad != nullptr && returned != nullptr
                    && returned->getReturnValue() == returnedLoad;
            };
            std::function<bool(llvm::Value*, llvm::Value*)> sameAddress =
                [&](llvm::Value* left, llvm::Value* right) {
                    if (left == right) return true;
                    left = left->stripPointerCasts();
                    right = right->stripPointerCasts();
                    if (left == right) return true;
                    auto resolveSingleStore = [&](llvm::Value* value) -> llvm::Value* {
                        auto* load = llvm::dyn_cast<llvm::LoadInst>(value);
                        if (load == nullptr) return nullptr;
                        llvm::Value* slot = load->getPointerOperand()->stripPointerCasts();
                        if (!llvm::isa<llvm::AllocaInst>(slot) || storesToSlot(slot) != 1)
                            return nullptr;
                        llvm::Value* stored = storesOf(slot).First->getValueOperand()->stripPointerCasts();
                        return llvm::isa<llvm::LoadInst>(stored) ? nullptr : stored;
                    };
                    if (llvm::Value* resolved = resolveSingleStore(left);
                        resolved != nullptr && sameAddress(resolved, right)) return true;
                    if (llvm::Value* resolved = resolveSingleStore(right);
                        resolved != nullptr && sameAddress(left, resolved)) return true;
                    auto* leftCall = llvm::dyn_cast<llvm::CallBase>(left);
                    auto* rightCall = llvm::dyn_cast<llvm::CallBase>(right);
                    if (leftCall != nullptr && rightCall != nullptr
                        && leftCall->getCalledFunction() == rightCall->getCalledFunction()
                        && isStablePointerGetter(leftCall->getCalledFunction())
                        && leftCall->arg_size() == rightCall->arg_size())
                    {
                        bool sameArguments = true;
                        for (unsigned i = 0; i < leftCall->arg_size(); ++i)
                            sameArguments = sameArguments
                                && leftCall->getArgOperand(i)->stripPointerCasts()
                                    == rightCall->getArgOperand(i)->stripPointerCasts();
                        if (sameArguments) return true;
                    }
                    int64_t leftOffset = 0;
                    int64_t rightOffset = 0;
                    const llvm::Value* leftBase = llvm::GetPointerBaseWithConstantOffset(
                        left, leftOffset, layout);
                    const llvm::Value* rightBase = llvm::GetPointerBaseWithConstantOffset(
                        right, rightOffset, layout);
                    if (leftBase == rightBase && leftOffset == rightOffset) return true;
                    auto* leftGep = llvm::dyn_cast<llvm::GetElementPtrInst>(left);
                    auto* rightGep = llvm::dyn_cast<llvm::GetElementPtrInst>(right);
                    if (leftGep == nullptr || rightGep == nullptr
                        || leftGep->getNumIndices() != rightGep->getNumIndices()
                        || !sameAddress(leftGep->getPointerOperand(), rightGep->getPointerOperand()))
                        return false;
                    auto leftIndex = leftGep->idx_begin();
                    auto rightIndex = rightGep->idx_begin();
                    for (; leftIndex != leftGep->idx_end(); ++leftIndex, ++rightIndex)
                    {
                        auto* leftConstant = llvm::dyn_cast<llvm::ConstantInt>(*leftIndex);
                        auto* rightConstant = llvm::dyn_cast<llvm::ConstantInt>(*rightIndex);
                        if (leftConstant != nullptr && rightConstant != nullptr)
                        {
                            if (leftConstant->getValue() != rightConstant->getValue()) return false;
                        }
                        else if (!sameIndex(*leftIndex, *rightIndex)) return false;
                    }
                    return true;
                };

            int64_t readOffset = 0;
            const llvm::Value* readBase = llvm::GetPointerBaseWithConstantOffset(
                readAddress, readOffset, layout);
            auto fixedStoreSize = [&](llvm::Type* type) -> uint64_t {
                llvm::TypeSize size = layout.getTypeStoreSize(type);
                return size.isScalable() ? 0 : size.getFixedValue();
            };
            uint64_t readSize = fixedStoreSize(load->getType());
            auto writeCoversRead = [&](llvm::Value* writeAddress, uint64_t writeSize) {
                if (writeSize == 0 || readSize == 0) return false;
                int64_t writeOffset = 0;
                const llvm::Value* writeBase = llvm::GetPointerBaseWithConstantOffset(
                    writeAddress, writeOffset, layout);
                return sameAddress(const_cast<llvm::Value*>(readBase),
                        const_cast<llvm::Value*>(writeBase))
                    && writeOffset <= readOffset
                    && static_cast<uint64_t>(readOffset - writeOffset) + readSize <= writeSize;
            };
            const bool pointerLoadedContainer = !localAggregate && frameLocalContainer
                && llvm::isa<llvm::LoadInst>(base);
            llvm::Value* pointerContainerSlot = pointerLoadedContainer
                ? llvm::cast<llvm::LoadInst>(base)->getPointerOperand()->stripPointerCasts() : nullptr;
            // Forward use-list walk from the container address (level k = slot holding level k-1);
            // an alias stored off-frame, a retaining/unknown callee, or an exhausted walk escapes.
            bool pointerContainerWalked = false;
            bool pointerContainerEscaped = false;
            std::unordered_set<const llvm::Value*> pointerContainerAliases;
            std::unordered_set<const llvm::CallBase*> pointerContainerCalls;
            auto walkPointerContainer = [&]() {
                if (pointerContainerWalked) return;
                pointerContainerWalked = true;
                constexpr int kMaxLevel = 6;
                std::vector<std::pair<llvm::Value*, int>> work;
                std::set<std::pair<const llvm::Value*, int>> seen;
                auto push = [&](llvm::Value* value, int level) {
                    if (level > kMaxLevel) { pointerContainerEscaped = true; return; }
                    if (seen.insert({ value, level }).second) work.push_back({ value, level });
                };
                // Roots: addresses stored into the slot `base` was loaded from, through slot copies.
                auto* rootSlot = llvm::dyn_cast<llvm::AllocaInst>(pointerContainerSlot);
                if (rootSlot == nullptr) { pointerContainerEscaped = true; return; }
                std::vector<llvm::AllocaInst*> slots { rootSlot };
                std::unordered_set<const llvm::Value*> slotSeen { rootSlot };
                for (size_t index = 0; index < slots.size() && !pointerContainerEscaped; ++index)
                    for (auto* user : slots[index]->users())
                    {
                        auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                        if (store == nullptr || store->getPointerOperand()->stripPointerCasts() != slots[index])
                            continue;
                        llvm::Value* stored = store->getValueOperand()->stripPointerCasts();
                        if (llvm::isa<llvm::ConstantPointerNull>(stored)) continue;
                        if (auto* load = llvm::dyn_cast<llvm::LoadInst>(stored))
                        {
                            auto* from = llvm::dyn_cast<llvm::AllocaInst>(load->getPointerOperand()->stripPointerCasts());
                            if (from == nullptr) { pointerContainerEscaped = true; break; }
                            if (slotSeen.insert(from).second) slots.push_back(from);
                            continue;
                        }
                        if (!llvm::isa<llvm::AllocaInst>(stored) && !llvm::isa<llvm::GetElementPtrInst>(stored))
                        {
                            pointerContainerEscaped = true;
                            break;
                        }
                        push(stored, 0);
                    }
                while (!work.empty() && !pointerContainerEscaped)
                {
                    auto [value, level] = work.back();
                    work.pop_back();
                    pointerContainerAliases.insert(value);
                    for (auto& use : value->uses())
                    {
                        auto* user = use.getUser();
                        if (auto* load = llvm::dyn_cast<llvm::LoadInst>(user))
                        {
                            if (level > 0) push(load, level - 1);
                            continue;
                        }
                        if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                        {
                            if (store->getValueOperand() != value) continue;
                            auto* destination = llvm::dyn_cast<llvm::AllocaInst>(
                                store->getPointerOperand()->stripPointerCasts());
                            if (destination == nullptr || !destination->getAllocatedType()->isPointerTy())
                                pointerContainerEscaped = true;
                            else push(destination, level + 1);
                            continue;
                        }
                        if (auto* call = llvm::dyn_cast<llvm::CallBase>(user))
                        {
                            if (llvm::isa<llvm::IntrinsicInst>(call)) continue;
                            pointerContainerCalls.insert(call);
                            // A level-0 alias handed to a callee that provably keeps nothing is no escape.
                            if (level != 0 || !call->isArgOperand(&use) || call->getCalledFunction() == nullptr
                                || compiler->CalleeMayRetainArgument(call->getCalledFunction(),
                                       call->getArgOperandNo(&use)))
                                pointerContainerEscaped = true;
                            continue;
                        }
                        if (llvm::isa<llvm::GetElementPtrInst>(user) && level == 0)
                            push(user, 0);
                        else if (llvm::isa<llvm::PHINode>(user) || llvm::isa<llvm::SelectInst>(user)
                                 || llvm::isa<llvm::BitCastInst>(user))
                            push(user, level);
                        else if (!llvm::isa<llvm::ICmpInst>(user))
                            pointerContainerEscaped = true;
                        if (pointerContainerEscaped) break;
                    }
                }
            };
            auto callReachesPointerContainer = [&](llvm::CallBase* call) {
                walkPointerContainer();
                return pointerContainerCalls.count(call) != 0;
            };
            auto pointerContainerEscapes = [&]() {
                walkPointerContainer();
                return pointerContainerEscaped;
            };
            bool callBetweenPointerLoadedContainerStoreAndRead = false;
            K mayWritten = K::Borrowed;
            auto inspectWrite = [&](llvm::Instruction* instruction, bool& killed) {
                // A call handed the tracked container may store its other pointer arguments.
                if (isTracked)
                    if (auto* call = llvm::dyn_cast<llvm::CallBase>(instruction);
                        call != nullptr && tracked.Calls.count(call) != 0)
                        mayWritten = worst(mayWritten, passedIntoContainer(call, tracked));
                if (pointerLoadedContainer)
                    if (auto* call = llvm::dyn_cast<llvm::CallBase>(instruction);
                        call != nullptr && (call->getCalledFunction() == nullptr
                            || !call->getCalledFunction()->isIntrinsic()))
                    {
                        if (callReachesPointerContainer(call) || pointerContainerEscapes())
                        {
                            callBetweenPointerLoadedContainerStoreAndRead = true;
                            killed = true;
                            return K::Borrowed;
                        }
                    }
                if (auto* store = llvm::dyn_cast<llvm::StoreInst>(instruction))
                {
                    if (sameAddress(store->getPointerOperand(), readAddress))
                    {
                        killed = true;
                        K stored = rootForAlternative(store->getValueOperand());
                        if (store->getMetadata(fn->getContext().getMDKindID(
                                "cflat.interface-return.alias-owned-local")) != nullptr)
                            stored = K::AliasOfOwnedLocal;
                        if (stored == K::OwnedLocal) stored = K::Owned;
                        return stored;
                    }
                    if (writeCoversRead(store->getPointerOperand(),
                                        fixedStoreSize(store->getValueOperand()->getType())))
                    {
                        killed = true;
                        return K::Borrowed;
                    }
                    // A write through a derived address of a tracked container (`*pp = alias`).
                    if (isTracked && store->getValueOperand()->getType()->isPointerTy()
                        && (tracked.Derived.count(store->getPointerOperand()) != 0
                            || tracked.Derived.count(store->getPointerOperand()->stripPointerCasts()) != 0))
                        mayWritten = worst(mayWritten, storedIntoContainer(store));
                    // `s[j] = alias` may be the slot read: merge it without ending the walk.
                    else if (store->getValueOperand()->getType()->isPointerTy()
                        && llvm::getUnderlyingObject(store->getPointerOperand())
                            == llvm::getUnderlyingObject(readAddress))
                    {
                        int64_t writeOffset = 0;
                        const llvm::Value* writeBase = llvm::GetPointerBaseWithConstantOffset(
                            store->getPointerOperand(), writeOffset, layout);
                        const uint64_t writeSize = fixedStoreSize(store->getValueOperand()->getType());
                        const bool disjoint = writeBase == readBase && writeSize != 0 && readSize != 0
                            && (writeOffset + static_cast<int64_t>(writeSize) <= readOffset
                                || readOffset + static_cast<int64_t>(readSize) <= writeOffset);
                        if (!disjoint)
                        {
                            K stored = rootForAlternative(store->getValueOperand());
                            if (stored == K::OwnedLocal) stored = K::Owned;
                            mayWritten = worst(mayWritten, stored);
                        }
                    }
                }
                if (auto* memory = llvm::dyn_cast<llvm::MemIntrinsic>(instruction))
                {
                    auto* length = llvm::dyn_cast<llvm::ConstantInt>(memory->getLength());
                    if (length != nullptr)
                    {
                        int64_t writeOffset = 0;
                        const llvm::Value* writeBase = llvm::GetPointerBaseWithConstantOffset(
                            memory->getDest(), writeOffset, layout);
                        uint64_t writeSize = length->getZExtValue();
                        int64_t writeEnd = writeOffset + static_cast<int64_t>(writeSize);
                        int64_t readEnd = readOffset + static_cast<int64_t>(readSize);
                        if (writeCoversRead(memory->getDest(), writeSize)
                            || (sameAddress(const_cast<llvm::Value*>(writeBase),
                                    const_cast<llvm::Value*>(readBase))
                                && writeOffset < readEnd && readOffset < writeEnd))
                        {
                            killed = true;
                            return K::Borrowed;
                        }
                    }
                }
                return K::Borrowed;
            };

            K merged = K::Borrowed;
            auto scanBlock = [&](llvm::BasicBlock* block, llvm::Instruction* before) {
                auto current = before != nullptr ? before->getIterator() : block->end();
                while (current != block->begin())
                {
                    --current;
                    bool killed = false;
                    K result = inspectWrite(&*current, killed);
                    if (killed)
                    {
                        merged = static_cast<int>(merged) >= static_cast<int>(result)
                            ? merged : result;
                        return true;
                    }
                }
                return false;
            };
            std::vector<std::pair<llvm::BasicBlock*, llvm::Instruction*>> pending;
            llvm::SmallPtrSet<const llvm::BasicBlock*, 32> visited;
            if (!scanBlock(load->getParent(), load))
                for (llvm::BasicBlock* predecessor : llvm::predecessors(load->getParent()))
                    pending.emplace_back(predecessor, nullptr);
            for (size_t i = 0; i < pending.size(); ++i)
            {
                auto [block, before] = pending[i];
                if (block == nullptr || !visited.insert(block).second) continue;
                if (!scanBlock(block, before))
                    for (llvm::BasicBlock* predecessor : llvm::predecessors(block))
                        pending.emplace_back(predecessor, nullptr);
            }
            if (callBetweenPointerLoadedContainerStoreAndRead) return mayWritten;
            return worst(merged, mayWritten);
        };
        if (auto* fieldAddr = llvm::dyn_cast<llvm::GetElementPtrInst>(addr))
        {
            // A lambda environment (`__closure_<n>`, unregistered) never owns a pointer capture
            // (isOwningCap): what the body stored there decides, like any other capture slot.
            auto* envType = llvm::dyn_cast<llvm::StructType>(fieldAddr->getSourceElementType());
            const bool envCapture = envType != nullptr && envType->hasName()
                && envType->getName().starts_with("__closure_") && envType->getName().size() > 10
                && std::isdigit(static_cast<unsigned char>(envType->getName()[10]))
                && load->getType()->isPointerTy();
            if (!envCapture && envType != nullptr && compiler->IsOwningFieldAddress(fieldAddr))
                return K::Owned;
            return rootFromAggregateSlot(addr);
        }
        if (auto* ptrSlotLoad = llvm::dyn_cast<llvm::LoadInst>(addr))
        {
            // Field 0 has no GEP (IRBuilder folds a zero offset): the address is the struct
            // pointer itself, so the load reads the first field of the pointee.
            const auto* nv = compiler->FindVariableByStorage(
                ptrSlotLoad->getPointerOperand()->stripPointerCasts());
            // `this` is a single pointer level spelled PointerDepth 0 (T522: `p = alias; return p;`).
            const bool singleLevel = nv != nullptr && (nv->TypeAndValue.PointerDepth == 1
                || (nv->TypeAndValue.PointerDepth == 0 && nv->TypeAndValue.Pointer));
            if (singleLevel && !nv->TypeAndValue.IsArrayView)
                return compiler->IsOwningStructField(nv->TypeAndValue.TypeName, 0)
                    ? K::Owned : rootFromAggregateSlot(addr);
            // Through deeper pointers (`*pp`): a borrow when the load chain roots in a parameter.
            llvm::Value* root = addr;
            while (auto* inner = llvm::dyn_cast<llvm::LoadInst>(root))
                root = inner->getPointerOperand()->stripPointerCasts();
            if (compiler->IsFunctionArgumentStorage(root)) return K::Borrowed;
            // `Thing** pp = &t; return *pp;`: a pp slot that stores the address of an owning local
            // makes the chain a borrow of that local.
            if (auto* ppSlot = llvm::dyn_cast<llvm::AllocaInst>(root); ppSlot != nullptr && fn != nullptr)
                for (auto& bb : *fn)
                    for (auto& inst : bb)
                        if (auto* store = llvm::dyn_cast<llvm::StoreInst>(&inst))
                            if (store->getPointerOperand()->stripPointerCasts() == ppSlot)
                                if (auto* target = llvm::dyn_cast<llvm::AllocaInst>(
                                        store->getValueOperand()->stripPointerCasts()))
                                    if (const auto* tnv = compiler->FindVariableByStorage(target);
                                        tnv != nullptr && tnv->IsOwning)
                                        return K::AliasOfOwnedLocal;
            // T522: field 0 of an object reached through a chain (`hh->h->p`): its stores decide.
            K viaContainer = rootFromAggregateSlot(addr);
            return viaContainer == K::Borrowed ? K::Owned : viaContainer;
        }
        auto* slot = llvm::dyn_cast<llvm::AllocaInst>(addr);
        if (slot == nullptr) return rootFromAggregateSlot(addr);
        if (llvm::isa<llvm::ArrayType>(slot->getAllocatedType()))
            return rootFromAggregateSlot(addr);  // element 0 of a local array: its stores decide, like `ps[1]`
        if (llvm::isa<llvm::StructType>(slot->getAllocatedType()))
        {
            // Field 0 has no GEP: a load straight from a struct local reads its first field. The
            // stores into that field are not stores into an alias slot.
            const auto* snv = compiler->FindVariableByStorage(slot);
            if (snv == nullptr) return K::Undecided;
            return compiler->IsOwningStructField(snv->TypeAndValue.TypeName, 0)
                ? K::Owned : rootFromAggregateSlot(addr);
        }
        // A local alias: every store into the slot decides. No store = undecided.
        K kind = K::Borrowed;
        bool anyStore = false;
        if (fn != nullptr)
            for (auto& bb : *fn)
                for (auto& inst : bb)
                    if (auto* store = llvm::dyn_cast<llvm::StoreInst>(&inst))
                        if (store->getPointerOperand()->stripPointerCasts() == slot)
                        {
                            anyStore = true;
                            K stored = rootForAlternative(store->getValueOperand());
                            if (stored == K::OwnedLocal) stored = K::AliasOfOwnedLocal;
                            kind = worst(kind, stored);
                        }
        // T522: a slot whose address escapes (`setp(&q, t)`, `Thing** pp = &q; *pp = t;`) also
        // holds whatever is written through that address.
        if (fn != nullptr && anyStore)
        {
            bool escapes = false;
            for (const llvm::User* user : slot->users())
            {
                if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                    store != nullptr && store->getPointerOperand()->stripPointerCasts() == slot
                    && store->getValueOperand()->stripPointerCasts() != slot)
                    continue;
                if (llvm::isa<llvm::LoadInst>(user) || llvm::isa<llvm::IntrinsicInst>(user)) continue;
                escapes = true;
                break;
            }
            if (escapes)
            {
                ContainerInfo info = analyzeContainer({ slot });
                if (everyArmLocal != nullptr) *everyArmLocal = false;
                for (llvm::StoreInst* store : info.Stores)
                {
                    // An owning local stored through the address is moved there (its flag clears).
                    if (store->getPointerOperand()->stripPointerCasts() == slot) continue;
                    kind = worst(kind, storedIntoContainer(store));
                }
                for (const llvm::CallBase* call : info.Calls)
                    kind = worst(kind, passedIntoContainer(call, info));
            }
        }
        return anyStore ? kind : K::Undecided;
    }

static PointerValueRootAnswer PointerValueRoot(llvm::Value* value, LLVMBackend* compiler,
                                                llvm::Function* fn, int depth,
                                                PointerValueRootQuery& query,
                                                bool frameLocalContainer)
    {
        using K = PointerValueRootKind;
        if (depth > query.MaxDepth) query.DepthCapped = true;
        if (value == nullptr || depth > query.MaxDepth) return {K::Undecided, true};
        if (query.Exhausted || !query.TakeStep()) return {K::Undecided, false};
        const size_t mode = frameLocalContainer ? 1 : 0;
        // While deferred records are collected a call's answer depends on its context (which
        // record list, returned object or not): never reuse it, or a record would be lost.
        const bool memoize = t_deferredBorrows == nullptr || !llvm::isa<llvm::CallBase>(value);
        auto found = memoize ? query.Memo[mode].find(value) : query.Memo[mode].end();
        if (found != query.Memo[mode].end()) return found->second;
        if (!query.InProgress.insert(value).second) return {K::Borrowed, false};

        bool everyArmLocal = true;
        K kind = PointerValueRootImpl(value, compiler, fn, depth, query,
                                      &everyArmLocal, frameLocalContainer);
        query.InProgress.erase(value);
        if (query.Exhausted) return {K::Undecided, false};
        PointerValueRootAnswer answer{kind, everyArmLocal};
        if (memoize) query.Memo[mode].emplace(value, answer);
        return answer;
    }

static thread_local bool t_rootExhausted = false;
static thread_local bool t_rootDepthCapped = false;
// Depth cap of an interface-return verdict: a cut there fails closed, so it sits far above real
// relay and fluent chains (a 12-hop `pk` chain, ten `.next()` calls).
static constexpr int kInterfaceReturnMaxDepth = 96;
static thread_local int t_rootMaxDepth = 8;

int CallResultFrameBorrowKind(llvm::Value* call, LLVMBackend* compiler, llvm::Function* fn,
                              std::vector<LLVMBackend::DeferredCallBorrow>* deferred,
                              bool* statementOwnedTempRoot)
    {
        if (statementOwnedTempRoot != nullptr) *statementOwnedTempRoot = false;
        if (!llvm::isa_and_nonnull<llvm::CallBase>(call)) return 0;
        DeferredBorrowCollector collector(deferred, true);
        t_unprovenRoot = false;
        PointerValueRootQuery query;
        query.MaxDepth = kInterfaceReturnMaxDepth;
        PointerValueRootAnswer answer = PointerValueRoot(call, compiler, fn, 0, query);
        if (statementOwnedTempRoot != nullptr)
            *statementOwnedTempRoot = query.StatementOwnedTempRoot;
        // 3: a frame verdict from unproven provenance (its own diagnostic text). A walk that ran
        // out of steps or depth proved nothing: refuse unproven unless a local borrow was found.
        const bool cut = query.Exhausted || query.DepthCapped;
        if (cut && deferred != nullptr) deferred->clear();
        const bool localBorrow = !query.Exhausted && (answer.Kind == PointerValueRootKind::AliasOfOwnedLocal
            || answer.Kind == PointerValueRootKind::FrameLocal);
        if (cut && !localBorrow) return 3;
        if (answer.Kind == PointerValueRootKind::AliasOfOwnedLocal) return 1;
        if (answer.Kind == PointerValueRootKind::FrameLocal) return TakeUnprovenPointerRoot() ? 3 : 2;
        return 0;
    }

PointerValueRootKind MainListener::PointerRootKindDeferring(llvm::Value* value, LLVMBackend* compiler,
        std::vector<LLVMBackend::DeferredCallBorrow>& deferred) {
        DeferredBorrowCollector collector(&deferred, true);
        t_unprovenRoot = false;
        t_rootMaxDepth = kInterfaceReturnMaxDepth;
        PointerValueRootKind kind = PointerRootKind(value, compiler);
        t_rootMaxDepth = 8;
        // Out of steps or depth: nothing was proven, so refuse unproven unless a local borrow was found.
        const bool localBorrow = kind == PointerValueRootKind::FrameLocal
            || kind == PointerValueRootKind::AliasOfOwnedLocal;
        if (t_rootExhausted || (t_rootDepthCapped && !localBorrow))
        {
            deferred.clear();
            t_unprovenRoot = true;
            return PointerValueRootKind::FrameLocal;
        }
        if (kind == PointerValueRootKind::Undecided) deferred.clear();
        return kind;
    }

PointerValueRootKind MainListener::PointerRootKind(llvm::Value* value, LLVMBackend* compiler) {
        PointerValueRootQuery query;
        query.MaxDepth = t_rootMaxDepth;
        PointerValueRootAnswer answer = PointerValueRoot(
            value, compiler, compiler->currentFunction, 0, query);
        t_rootExhausted = query.Exhausted;
        t_rootDepthCapped = query.DepthCapped;
        return query.Exhausted ? PointerValueRootKind::Undecided : answer.Kind;
    }

bool MainListener::PointerRootKindEveryArmLocal(llvm::Value* value, LLVMBackend* compiler,
                                                bool frameLocalContainer) {
        PointerValueRootQuery query;
        PointerValueRootAnswer answer = PointerValueRoot(
            value, compiler, compiler->currentFunction, 0, query, frameLocalContainer);
        return !query.Exhausted && answer.EveryArmLocal && IsLocalPointerRoot(answer.Kind);
    }

bool MainListener::PointerArithmeticRootLeaks(llvm::Value* value, LLVMBackend* compiler) {
        return PointerRootKind(value, compiler) != PointerValueRootKind::Borrowed;
    }

bool MainListener::ReturnLeaksOwnershipIntoInterface(llvm::Value* right, LLVMBackend* compiler,
                                                     bool pointerArithmetic) {
        if (right == nullptr || compiler->currentFunctionReturnsOwned) return false;
        auto* fatTy = compiler->GetFatPtrType();
        if (fatTy == nullptr || compiler->currentFunction == nullptr
            || compiler->currentFunction->getReturnType() != fatTy)
            return false;
        if (right->getType() == fatTy)
            return FrameLocalDataOfFatValue(right) == nullptr && FatValueOwnsHeapBox(right);
        // A pointer-arithmetic result is decided by its provenance alone: the per-call owned
        // flags describe the last call or load, not the arithmetic result.
        if (pointerArithmetic && right->getType()->isPointerTy())
            return !llvm::isa<llvm::Constant>(right) && PointerArithmeticRootLeaks(right, compiler);
        return right->getType()->isPointerTy() && !llvm::isa<llvm::Constant>(right)
            && (compiler->IsOwningValue(right) || compiler->lastCallReturnsOwned
                || compiler->lastOwningResult);
    }

std::vector<MainListener::InterfaceJoinArm> MainListener::CollectPointerJoinArms(llvm::Value* value) const {
        std::vector<InterfaceJoinArm> arms;
        if (auto* phi = llvm::dyn_cast_or_null<llvm::PHINode>(value))
        {
            if (!phi->getType()->isPointerTy()) return arms;
            for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
                arms.push_back({ phi->getIncomingValue(i), phi->getIncomingBlock(i) });
            return arms;
        }
        if (auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(value))
        {
            if (!load->getType()->isPointerTy()) return arms;
            if (const auto* join = compilerLLVM->FindNullCoalesceJoin(load))
                for (const auto& arm : join->Arms) arms.push_back({ arm.Value, arm.Block });
        }
        return arms;
    }

bool MainListener::NestedJoinArmsBoxable(llvm::Value* value, const std::string& interfaceName,
                               std::string* armFailure, int depth) {
        if (depth > kMaxNestedJoinDepth) return false;
        auto arms = CollectPointerJoinArms(value);
        if (arms.empty()) return false;
        for (const auto& arm : arms)
        {
            if (arm.Value == nullptr || arm.Block == nullptr) return false;
            if (cflat_llvm::GetTerminatorOrNull(arm.Block) == nullptr) return false;
            if (llvm::isa<llvm::ConstantPointerNull>(arm.Value)) continue;
            ResolvedJoinArmClass resolved;
            if (!ResolveJoinArmClass(arm.Value, resolved))
            {
                if (!NestedJoinArmsBoxable(arm.Value, interfaceName, armFailure, depth + 1))
                    return false;
                continue;
            }
            if (!compilerLLVM->StructImplementsInterface(resolved.Name, interfaceName))
            {
                if (armFailure != nullptr)
                    *armFailure = std::format("'{}' does not implement it", resolved.Name);
                return false;
            }
        }
        return true;
    }

bool MainListener::CollectJoinArmClasses(llvm::Value* value, std::vector<std::string>& out, int depth) {
        if (depth > kMaxNestedJoinDepth) return false;
        auto arms = CollectPointerJoinArms(value);
        if (arms.empty()) return false;
        for (const auto& arm : arms)
        {
            if (arm.Value == nullptr) return false;
            if (llvm::isa<llvm::ConstantPointerNull>(arm.Value)) continue;
            ResolvedJoinArmClass resolved;
            if (ResolveJoinArmClass(arm.Value, resolved))
            {
                if (!compilerLLVM->IsDataStructure(resolved.Name)) return false;
                out.push_back(resolved.Name);
                continue;
            }
            if (!CollectJoinArmClasses(arm.Value, out, depth + 1)) return false;
        }
        return true;
    }

bool MainListener::ResolveJoinArmClass(llvm::Value* value, ResolvedJoinArmClass& result) const {
        result = {};
        if (value == nullptr || !value->getType()->isPointerTy()) return false;
        result.Name = compilerLLVM->ResolvePointerElementTypeName(value);
        if (result.Name.empty()) return false;
        if (auto* load = llvm::dyn_cast<llvm::LoadInst>(value))
            result.Source = compilerLLVM->FindVariableByStorage(load->getPointerOperand());
        return true;
    }

llvm::Value* MainListener::BuildJoinArmPredicate(llvm::Value* value,
                                                  const std::string& targetTypeName,
                                                  bool targetIsInterface,
                                                  LLVMBackend* compiler,
                                                  std::string& failure,
                                                  int depth) {
        if (depth > kMaxNestedJoinDepth)
        {
            failure = "nested join is too deep to classify";
            return nullptr;
        }
        auto arms = CollectPointerJoinArms(value);
        if (arms.empty())
        {
            failure = "the arm's concrete class cannot be determined; bind the arm to a "
                      "local variable of the class type first";
            return nullptr;
        }

        std::vector<llvm::Value*> answers;
        answers.reserve(arms.size());
        for (const auto& arm : arms)
        {
            if (arm.Value == nullptr || arm.Block == nullptr)
            {
                failure = "the arm's concrete class cannot be determined; bind the arm to a "
                          "local variable of the class type first";
                return nullptr;
            }
            if (llvm::isa<llvm::ConstantPointerNull>(arm.Value))
            {
                answers.push_back(compiler->builder->getInt1(false));
                continue;
            }

            ResolvedJoinArmClass resolved;
            if (ResolveJoinArmClass(arm.Value, resolved))
            {
                if (!compiler->IsDataStructure(resolved.Name))
                {
                    failure = std::format("'{}' is not a concrete class", resolved.Name);
                    return nullptr;
                }
                bool matches = targetIsInterface
                    ? compiler->StructImplementsInterface(resolved.Name, targetTypeName)
                    : resolved.Name == targetTypeName;
                answers.push_back(compiler->builder->getInt1(matches));
                continue;
            }

            std::string nestedFailure;
            llvm::Value* nested = BuildJoinArmPredicate(arm.Value, targetTypeName,
                                                         targetIsInterface, compiler,
                                                         nestedFailure, depth + 1);
            if (nested == nullptr)
            {
                failure = nestedFailure.empty()
                    ? "the arm's concrete class cannot be determined; bind the arm to a local "
                      "variable of the class type first"
                    : nestedFailure;
                return nullptr;
            }
            answers.push_back(nested);
        }

        if (answers.empty()) return compiler->builder->getInt1(false);
        bool allSame = true;
        bool firstConstant = false;
        if (auto* first = llvm::dyn_cast<llvm::ConstantInt>(answers.front()))
            firstConstant = first->isOne();
        else
            allSame = false;
        for (size_t i = 1; i < answers.size() && allSame; ++i)
        {
            auto* current = llvm::dyn_cast<llvm::ConstantInt>(answers[i]);
            if (current == nullptr || current->isOne() != firstConstant) allSame = false;
        }
        if (allSame) return compiler->builder->getInt1(firstConstant);

        llvm::Instruction* joinPoint = llvm::dyn_cast<llvm::Instruction>(value);
        auto* phi = llvm::dyn_cast_or_null<llvm::PHINode>(value);
        auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(value);
        if (joinPoint == nullptr || (phi == nullptr && load == nullptr))
        {
            failure = "the join's runtime arm cannot be classified";
            return nullptr;
        }
        auto* savedBlock = compiler->builder->GetInsertBlock();
        auto savedPoint = compiler->builder->GetInsertPoint();
        compiler->builder->SetInsertPoint(joinPoint);
        auto* answerPhi = compiler->builder->CreatePHI(compiler->builder->getInt1Ty(),
                                                        answers.size(), "join_is");
        for (size_t i = 0; i < answers.size(); ++i)
            answerPhi->addIncoming(answers[i], arms[i].Block);
        compiler->builder->SetInsertPoint(savedBlock, savedPoint);
        return answerPhi;
    }

llvm::Value* MainListener::BoxInterfaceJoinArms(const std::vector<InterfaceJoinArm>& arms,
                                      llvm::Value* joinValue, llvm::Instruction* joinPoint,
                                      const std::string& interfaceName, std::string* armFailure,
                                      bool transferArmOwnership, bool* armNotOwned) {
        auto* compiler = compilerLLVM;
        unsigned count = static_cast<unsigned>(arms.size());
        if (count == 0 || joinPoint == nullptr || interfaceName.empty()) return nullptr;
        // Resolve every arm first: a partial rewrite would leave half-boxed IR behind.
        std::vector<std::string> armTypes(count);
        std::vector<const LLVMBackend::NamedVariable*> armSources(count, nullptr);
        std::vector<bool> armIsNestedJoin(count, false);
        for (unsigned i = 0; i < count; i++)
        {
            llvm::Value* incoming = arms[i].Value;
            if (incoming == nullptr || arms[i].Block == nullptr) return nullptr;
            if (llvm::isa<llvm::ConstantPointerNull>(incoming)) continue;
            ResolvedJoinArmClass resolved;
            if (ResolveJoinArmClass(incoming, resolved))
            {
                armTypes[i] = resolved.Name;
                armSources[i] = resolved.Source;
            }
            if (armTypes[i].empty())
            {
                // The arm is itself a JOIN (a chained '??', or either spelling nested in the
                // other), so it has no class of its own - box it recursively in its own block.
                std::string nestedFailure;
                if (NestedJoinArmsBoxable(incoming, interfaceName, &nestedFailure)
                    && cflat_llvm::GetTerminatorOrNull(arms[i].Block) != nullptr)
                {
                    armIsNestedJoin[i] = true;
                    continue;
                }
                if (armFailure != nullptr)
                    *armFailure = nestedFailure.empty()
                        ? "the arm's concrete class cannot be determined; bind the arm to a "
                          "local variable of the class type first"
                        : nestedFailure;
                return nullptr;
            }
            if (!compiler->StructImplementsInterface(armTypes[i], interfaceName))
            {
                if (armFailure != nullptr)
                    *armFailure = std::format("'{}' does not implement it", armTypes[i]);
                return nullptr;
            }
            if (cflat_llvm::GetTerminatorOrNull(arms[i].Block) == nullptr) return nullptr;
            /*
             * A 'move' return transfers EVERY arm, so an arm that owns nothing would hand the
             * caller a second owner of a live borrow (double free). The whole-expression check on
             * the return path cannot see this: boxing turns the operand into a struct and that
             * check is gated on a pointer operand. Ask per arm instead - but PROVABLY, since the
             * accept side is the safe one here: only a load off a live binding that declares
             * itself non-owning is rejected. A call result, a phi, a field GEP or an unresolvable
             * slot all answer "cannot tell" and are accepted, which is what keeps this from
             * reopening the false rejection that the whole-expression check used to produce.
             */
            if (transferArmOwnership && !JoinArmIsProvablyNull(incoming)
                && !compiler->TernaryArmJoinsOwning(incoming)
                && !compiler->IsMovedOutPtrValue(incoming)
                && compiler->IsProvablyNonOwningPointerLoad(incoming))
            {
                if (armNotOwned != nullptr) *armNotOwned = true;
                return nullptr;
            }
        }

        auto* savedBlock = compiler->builder->GetInsertBlock();
        auto savedPoint = compiler->builder->GetInsertPoint();
        auto* fatTy = compiler->GetFatPtrType();
        std::vector<llvm::Value*> boxed(count, nullptr);
        for (unsigned i = 0; i < count; i++)
        {
            if (armIsNestedJoin[i])
            {
                // Boxed at the NESTED join's own join point, so its fat phi lands in the block
                // that really branches here - arms[i].Block is that block, and it dominates it.
                std::string nestedFailure;
                bool nestedNotOwned = false;
                llvm::Value* nested = UpcastPointerJoinToInterface(
                    arms[i].Value, interfaceName, &nestedFailure, nullptr, transferArmOwnership,
                    &nestedNotOwned);
                if (nested == nullptr)
                {
                    compiler->builder->SetInsertPoint(savedBlock, savedPoint);
                    // Propagate the INNER verdict: a not-owned inner arm must reach the caller as
                    // the ownership diagnostic, not as the useless "bind it to a local" remedy.
                    if (nestedNotOwned)
                    {
                        if (armNotOwned != nullptr) *armNotOwned = true;
                    }
                    else if (armFailure != nullptr)
                        *armFailure = nestedFailure.empty()
                            ? "the arm's concrete class cannot be determined; bind the arm "
                              "to a local variable of the class type first"
                            : nestedFailure;
                    return nullptr;
                }
                boxed[i] = nested;
                continue;
            }
            if (armTypes[i].empty())
            {
                boxed[i] = llvm::Constant::getNullValue(fatTy);
                continue;
            }
            compiler->builder->SetInsertPoint(cflat_llvm::GetTerminatorOrNull(arms[i].Block));
            auto* vtable = compiler->GetOrCreateVTable(armTypes[i], interfaceName);
            llvm::Value* armData = arms[i].Value;
            boxed[i] = compiler->BuildInterfaceFatValue(vtable, armData);

            // Ownership transfer for an escaping box (see transferArmOwnership). Null the arm's
            // OWNING source in the arm's OWN block, so only the untaken arm is freed at scope exit.
            bool transferred = false;
            if (transferArmOwnership)
                if (auto* load = llvm::dyn_cast<llvm::LoadInst>(armData);
                    load != nullptr && load->getType()->isPointerTy() && compiler->IsOwningValue(load))
                {
                    compiler->builder->CreateStore(
                        llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(load->getType())),
                        load->getPointerOperand());
                    transferred = true;
                }
            // NO MarkVariableMoved here, unlike BoxConcreteIntoInterface: the transfer is
            // RUNTIME-conditional, so a compile-time flag false-rejects the not-taken path.

            // Ledger the box with the same provenance classification the single-value site records,
            // so the guards that ask WHERE a boxed object lives can answer a join too.
            LLVMBackend::InterfaceBoxRecord record;
            record.FatValue = boxed[i];
            record.DataPointer = armData;
            record.SourceClassName = armTypes[i];
            record.InterfaceName = interfaceName;
            record.Source = ClassifyInterfaceBoxSource(armData, armSources[i], transferred);
            record.OwnershipTransferred = transferred;
            record.AliasOfOwnedLocal = !transferred
                && PointerRootKind(armData, compiler) == PointerValueRootKind::AliasOfOwnedLocal;
            // A PROVEN `move` arm detached its source here, so no other binding frees it. Same
            // demotion, and the same value keying, JoinArmsKeepOwner applies to the raw spelling.
            bool armMovedOut = compiler->IsMovedOutPtrValue(armData);
            record.SourceKeepsOwner = !armMovedOut
                && ClassifyBoxedSourceKeepsOwner(armData, armSources[i], transferred);
            // The arm keeps its real vtable (the IR is unchanged); only the LEDGER calls it neutral.
            record.SourceProvablyNull = JoinArmIsProvablyNull(armData);
            if (record.SourceKeepsOwner)
            {
                record.SourceDisplayName = DescribeBoxedSourceOwner(armData, armSources[i]);
                if (const auto* proving = ProvingBindingForBoxedSource(armData, armSources[i]))
                {
                    llvm::Value* ownerSlot = proving->Storage;
                    // A plain assignment such as `p = q` leaves p's binding as the display and
                    // proof carrier, but q is the owner slot to re-ask after a boxed join.
                    if (armSources[i] != nullptr && armSources[i]->InheritedKeepsOwner
                        && !armSources[i]->BorrowedOrigin.empty())
                    {
                        auto originNV = compiler->GetScopedLocalOrArgument(
                            BorrowedOriginRoot(armSources[i]->BorrowedOrigin));
                        if (originNV.Storage != nullptr)
                            if (compiler->FindVariableByStorage(originNV.Storage) != nullptr)
                                ownerSlot = originNV.Storage;
                    }
                    if (ownerSlot != nullptr && llvm::isa<llvm::AllocaInst>(ownerSlot))
                        record.SourceOwnerSlot = ownerSlot;
                }
            }
            compiler->RegisterInterfaceBox(record);
        }
        compiler->builder->SetInsertPoint(joinPoint);
        auto* fatPhi = compiler->builder->CreatePHI(fatTy, count, "ternary_iface");
        for (unsigned i = 0; i < count; i++)
            fatPhi->addIncoming(boxed[i], arms[i].Block);
        compiler->builder->SetInsertPoint(savedBlock, savedPoint);

        /*
         * Carry the per-arm ownership verdict onto the boxed join. The ledgers track the RAW thin
         * arms, and the receiver now adopts a different value (fatPhi), so the verdict must travel
         * with it or a join with a live BORROW arm is destroyed by its receiver and again by its
         * real owner. The diagnostic entry rides along either way (a suppressed one stays visible
         * to the no-discard check); the thin join's own entries are retired so nothing misfires off
         * a value that only feeds the boxing.
         */
        bool armsAllOwn = true;
        for (unsigned i = 0; i < count; i++)
            if (!compiler->TernaryArmJoinsOwning(arms[i].Value)) armsAllOwn = false;
        compiler->PropagateOwnedReturnTemp(joinValue, fatPhi);
        if (armsAllOwn)
        {
            if (compiler->IsMovedOutPtrValue(joinValue)) compiler->RegisterMovedOutPtrValue(fatPhi);
        }
        else
        {
            compiler->SuppressCallerRelease(fatPhi);
            compiler->SuppressCallerRelease(joinValue);
            compiler->ClearOwnedResultChannels();
        }
        return fatPhi;
    }

llvm::Value* MainListener::UpcastTernaryPhiToInterface(llvm::Value* right, const std::string& interfaceName,
                                             std::string* armFailure,
                                             bool transferArmOwnership,
                                             bool* armNotOwned) {
        auto* phi = llvm::dyn_cast_or_null<llvm::PHINode>(right);
        if (phi == nullptr || interfaceName.empty() || !phi->getType()->isPointerTy()) return nullptr;
        std::vector<InterfaceJoinArm> arms;
        for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
            arms.push_back({ phi->getIncomingValue(i), phi->getIncomingBlock(i) });
        return BoxInterfaceJoinArms(arms, phi, phi, interfaceName, armFailure, transferArmOwnership,
                                    armNotOwned);
    }

llvm::Value* MainListener::UpcastNullCoalesceToInterface(llvm::Value* right, const std::string& interfaceName,
                                               std::string* armFailure,
                                               bool transferArmOwnership,
                                               bool* armNotOwned) {
        auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(right);
        if (load == nullptr || interfaceName.empty() || !load->getType()->isPointerTy()) return nullptr;
        const auto* join = compilerLLVM->FindNullCoalesceJoin(load);
        if (join == nullptr) return nullptr;
        std::vector<InterfaceJoinArm> arms;
        for (const auto& arm : join->Arms) arms.push_back({ arm.Value, arm.Block });
        return BoxInterfaceJoinArms(arms, load, load, interfaceName, armFailure, transferArmOwnership,
                                    armNotOwned);
    }

llvm::Value* MainListener::UpcastPointerJoinToInterface(llvm::Value* right, const std::string& interfaceName,
                                              std::string* armFailure,
                                              std::string* joinSpelling,
                                              bool transferArmOwnership,
                                              bool* armNotOwned) {
        if (auto* fat = UpcastTernaryPhiToInterface(right, interfaceName, armFailure,
                                                   transferArmOwnership, armNotOwned))
            return fat;
        if (armNotOwned != nullptr && *armNotOwned)
        {
            if (joinSpelling != nullptr) *joinSpelling = "?:";
            return nullptr;
        }
        if (armFailure != nullptr && !armFailure->empty())
        {
            if (joinSpelling != nullptr) *joinSpelling = "?:";
            return nullptr;
        }
        auto* fat = UpcastNullCoalesceToInterface(right, interfaceName, armFailure,
                                                  transferArmOwnership, armNotOwned);
        if (fat == nullptr && joinSpelling != nullptr
            && ((armNotOwned != nullptr && *armNotOwned)
                || (armFailure != nullptr && !armFailure->empty())))
            *joinSpelling = "??";
        return fat;
    }

// Two classes implementing one interface, inherited ones included (Circle : IDrawable : IShape).
bool MainListener::ClassesShareInterface(const std::string& a, const std::string& b) const {
        for (const auto& [iface, methods] : compilerLLVM->interfaceTable)
            if (compilerLLVM->StructImplementsInterface(a, iface)
                && compilerLLVM->StructImplementsInterface(b, iface))
                return true;
        return false;
    }


void MainListener::RefuseUnboxedClassJoinArgument(antlr4::ParserRuleContext* ctx, llvm::Value* argValue) {
        auto* compiler = compilerLLVM;
        if (argValue == nullptr || !argValue->getType()->isPointerTy()) return;
        std::vector<std::string> armClasses;
        if (!CollectJoinArmClasses(argValue, armClasses)) return;
        std::vector<std::string> distinct;
        for (const auto& cls : armClasses)
            if (std::find(distinct.begin(), distinct.end(), cls) == distinct.end())
                distinct.push_back(cls);
        // A shared-interface join is allowed only when the call actually boxed it.
        if (distinct.size() < 2 || !ClassesShareInterface(distinct[0], distinct[1])) return;
        const char* op = compiler->FindNullCoalesceJoin(argValue) != nullptr ? "??" : "?:";
        LogErrorContext(ctx, UnrelatedPointerJoinDiagnostic(compiler,
            LLVMBackend::TypeAndValue{ .TypeName = distinct[0], .Pointer = true, .PointerDepth = 1 },
            LLVMBackend::TypeAndValue{ .TypeName = distinct[1], .Pointer = true, .PointerDepth = 1 },
            op));
    }

// ComputeOverloadFunction's own test, one parameter at a time: does `cand` take `arg` at `index`.
static bool CandidateTakesArgument(LLVMBackend* compiler, const LLVMBackend::FunctionSymbol& cand,
                                   size_t index, const LLVMBackend::NamedVariable& arg)
{
        LLVMBackend::FunctionSymbol probe = cand;
        probe.Parameters = { cand.Parameters[index] };
        probe.IsMethod = false;
        probe.UniqueName = "__cflat_join_arg_probe";   // no declared C++ spelling to look up
        probe.CxxAbi.valid = false;
        probe.Recipe.paramSlots.clear();
        const std::vector<std::pair<std::vector<LLVMBackend::NamedVariable>,
                                    LLVMBackend::FunctionSymbol>> one = { { { arg }, probe } };
        return !compiler->ComputeOverloadFunction(one).second.Parameters.empty();
    }

// A class-join argument the arg loop could not box, judged once every argument is typed: a
// candidate that cannot take the OTHER arguments is dropped, then the join boxes into the one
// interface the rest share. Nothing viable, or a pointer candidate still viable, returns nullptr.
llvm::Value* MainListener::BoxDeferredJoinArgument(
        const std::vector<LLVMBackend::NamedVariable>& arguments,
        const std::vector<int64_t>& declaredIdx, const std::string& functionName,
        size_t wantParams, size_t argIdx, llvm::Value* argValue, std::string& ifaceNameOut) {
        auto* compiler = compilerLLVM;
        if (arguments.size() != declaredIdx.size() || argIdx >= arguments.size()) return nullptr;
        const int64_t joinSlot = declaredIdx[argIdx];
        auto ftIt = compiler->functionTable.find(functionName);
        if (joinSlot < 0 || ftIt == compiler->functionTable.end()) return nullptr;
        std::vector<const LLVMBackend::TypeAndValue*> viable;
        for (const auto& cand : ftIt->second)
        {
            if (cand.Parameters.size() != wantParams || joinSlot >= (int64_t)cand.Parameters.size())
                continue;
            bool takesRest = true;
            for (size_t j = 0; j < arguments.size() && takesRest; ++j)
            {
                // An unbound named argument keeps the candidate: the conservative side.
                if (j == argIdx || declaredIdx[j] < 0
                    || declaredIdx[j] >= (int64_t)cand.Parameters.size())
                    continue;
                takesRest = CandidateTakesArgument(compiler, cand, (size_t)declaredIdx[j], arguments[j]);
            }
            if (takesRest) viable.push_back(&cand.Parameters[joinSlot]);
        }
        if (viable.empty()) return nullptr;
        return BoxPointerJoinArgument(viable, argValue, ifaceNameOut);
    }

llvm::Value* MainListener::BoxPointerJoinArgument(
        const std::vector<const LLVMBackend::TypeAndValue*>& paramsAtPosition,
        llvm::Value* argValue, std::string& ifaceNameOut) {
        auto* compiler = compilerLLVM;
        if (argValue == nullptr || !argValue->getType()->isPointerTy()) return nullptr;

        // Every LEAF arm's concrete class, so "implements" can be asked of all of them at once.
        // Recurses through nested joins, whose result carries no class of its own.
        std::vector<std::string> armClasses;
        if (!CollectJoinArmClasses(argValue, armClasses)) return nullptr;
        if (armClasses.empty()) return nullptr;

        std::string target;
        for (const auto* param : paramsAtPosition)
        {
            if (param == nullptr) continue;
            // ANY pointer parameter can take the raw join and would win the call it is
            // about to be pre-empted out of - void*, char*, int* included. Never box past one.
            if (param->Pointer) return nullptr;
            if (!param->IsInterface || param->TypeName.empty()) continue;
            bool allImplement = true;
            for (const auto& cls : armClasses)
                if (!compiler->StructImplementsInterface(cls, param->TypeName)) allImplement = false;
            if (!allImplement) continue;
            if (!target.empty() && target != param->TypeName) return nullptr;
            target = param->TypeName;
        }
        if (target.empty()) return nullptr;

        std::string armFailure;
        llvm::Value* fat = UpcastPointerJoinToInterface(argValue, target, &armFailure);
        if (fat == nullptr) return nullptr;
        ifaceNameOut = target;
        return fat;
    }

bool MainListener::IsAllNullPhi(llvm::Value* value) const {
        auto* phi = llvm::dyn_cast_or_null<llvm::PHINode>(value);
        if (phi == nullptr || phi->getNumIncomingValues() == 0) return false;
        for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
        {
            auto* constant = llvm::dyn_cast<llvm::Constant>(phi->getIncomingValue(i));
            if (constant == nullptr || !constant->isNullValue()) return false;
        }
        return true;
    }

bool MainListener::RejectRawPointerToArrayView(antlr4::ParserRuleContext* ctx,
                                     const LLVMBackend::TypeAndValue& target,
                                     const LLVMBackend::NamedVariable& rhsNV) {
        const auto& rhs = rhsNV.TypeAndValue;
        // A counted `move T*` call result (or a direct `new T[n]`) is a fresh WHOLE allocation,
        // exactly what the noalias contract of `T[]` asks for - it binds; the count travels
        // with the ledgered result. A named raw pointer stays rejected (decay is one-way).
        if (target.IsArrayView && rhs.Pointer && !rhs.IsArrayView
            && Compiler()->FindRawArrayResult(rhsNV.Primary) != nullptr)
            return false;
        // A fixed `T*[N]` decays to its first POINTER ELEMENT, so it carries Pointer even
        // though it is a real bounded array source. Its matching `T*[]` view preserves that
        // element star in ElemPointer and is safe to bind; a raw `T*` remains rejected.
        if (target.IsArrayView && rhs.Pointer && !rhs.IsArrayView
            && !(target.ElemPointer && rhs.ConstArraySize > 0))
        {
            LogErrorContext(ctx, "cannot bind a raw pointer 'T*' to an array-view 'T[]' - a view "
                "must span a whole allocation (it comes only from 'new T[n]' or another 'T[]'); "
                "the 'T[] -> T*' decay is one-way");
            return true;
        }
        return false;
    }

bool MainListener::RejectUniqueArrayMoveIntoPointer(antlr4::ParserRuleContext* ctx,
                                     const LLVMBackend::TypeAndValue& target,
                                     const LLVMBackend::NamedVariable& rhsNV) {
        const auto& rhs = rhsNV.TypeAndValue;
        if (!target.Pointer || target.IsArrayView || rhs.Pointer || rhs.ConstArraySize == 0)
            return false;
        if (!(rhs.IsMove || rhsNV.IsExplicitMove)) return false;
        if (!(rhs.IsUnique || Compiler()->IsCoreUniqueType(rhs.TypeName))) return false;
        const std::string name = rhsNV.CallerName.empty() ? rhs.VariableName : rhsNV.CallerName;
        LogErrorContext(ctx, std::format(
            "cannot move unique array '{}' into a pointer - a pointer does not own the array; "
            "borrow its first element with '&{}[0]'", name, name));
        return true;
    }

/*
 * Spell a cflat primitive name for an UNNAMED constant source. Only a scalar integer or float is
 * a "single value" for the array-view gate; anything else returns "" and is left accepted.
 */
std::string MainListener::SpellPrimitiveConstantType(llvm::Type* type) {
        if (type == nullptr) return std::string();
        // An unnamed literal carries no cflat type - only its materialized width - so an integer
        // one spells 'int' exactly as the named-arithmetic path does ('4 + 1' reports 'int' into
        // both 'int[]' and 'char[]'). 'i1' is the one width no integer literal shares with 'bool'.
        if (type->isIntegerTy())
            return type->getIntegerBitWidth() == 1 ? "bool" : "int";
        if (type->isDoubleTy()) return "double";
        if (type->isFloatingPointTy()) return "float";
        return std::string();
    }

/*
 * Is this array-view source the enumerator of 'enumKey' whose value is zero? A member arrives named
 * '<enum>.<member>' with no folded constant, so the enum registry is what decides its value.
 */
static bool EnumMemberSourceIsZero(const LLVMBackend& compiler, const std::string& enumKey,
                                   const LLVMBackend::NamedVariable& rhsNV)
{
        const std::string prefix = enumKey + ".";
        const std::string& name = rhsNV.TypeAndValue.VariableName;
        if (!name.starts_with(prefix)) return false;
        int64_t value = 0;
        return compiler.TryGetEnumMemberInt(enumKey, name.substr(prefix.size()), value)
            && value == 0;
    }

/*
 * A primitive VALUE bound to a 'T[]' is reinterpreted as a thin pointer and indexed, so only the
 * NULL-view spelling may bind. '0', 'nullptr', 'default' and any folded zero are that spelling;
 * anything else is a number that reaches v[0] as an address, named ('int x') or bare ('5').
 */
bool MainListener::RejectPrimitiveValueIntoArrayView(antlr4::ParserRuleContext* ctx,
                                     const LLVMBackend::TypeAndValue& target,
                                     const LLVMBackend::NamedVariable& rhsNV) {
        const auto& rhs = rhsNV.TypeAndValue;
        if (!target.IsArrayView) return false;
        // Prove it is a bare primitive SCALAR before rejecting: a view, an array, a pointer, a
        // string literal and a fat value all keep their own rules. An unnamed source stays in -
        // it is judged by its constant below, since the null spellings name no type either.
        if (rhs.IsArrayView || rhs.ConstArraySize != 0 || rhs.Pointer || rhs.IsInterface
            || rhs.IsFunctionPointer || rhs.IsSimd)
            return false;
        // Array SHAPE, not element name: a ROW of a fixed 'int[2][3]' also names 'int' with no
        // ConstArraySize, and it is a real element source - its inner dimensions are the proof.
        if (!rhs.ConstInnerDimensions.empty()) return false;
        // A temp that already folded to a constant zero is the null-view spelling; a NAMED int is
        // not, whatever it holds, since nothing here proves the slot stays zero.
        auto* folded = llvm::dyn_cast_or_null<llvm::Constant>(rhsNV.Primary);
        if (folded != nullptr && folded->isNullValue()) return false;
        std::string sourceSpelling;
        if (rhs.TypeName.empty())
        {
            /*
             * An UNNAMED source names no type, so decide it by the constant instead of by the
             * name: '0', 'nullptr', 'default' and a folded zero took the null exit just above, a
             * non-constant unnamed value is not proven to be a number and keeps its own rules,
             * and what is left is a bare non-zero literal that v[0] would index as an address.
             */
            if (folded == nullptr) return false;
            sourceSpelling = SpellPrimitiveConstantType(folded->getType());
            if (sourceSpelling.empty()) return false;
        }
        else
        {
            // An enum member names its ENUM, yet it reaches v[0] as its backing integer does, so
            // hop the enum to that backing name and judge it exactly like a named primitive.
            std::string resolved = Compiler()->ResolveTypeAlias(rhs.TypeName);
            if (std::string enumKey = Compiler()->ResolveEnumTypeName(resolved); !enumKey.empty())
            {
                // A member reaches here as a LOAD of its enumerator global, not as a constant, so
                // ask the enum registry for the value: a zero member is still the null spelling.
                if (EnumMemberSourceIsZero(*Compiler(), enumKey, rhsNV)) return false;
                resolved = Compiler()->ResolveTypeAlias(Compiler()->GetEnumBackingType(enumKey));
            }
            if (!LLVMBackend::IsPrimitiveTypeName(resolved))
                return false;
            sourceSpelling = SpellType(*Compiler(), rhs);
        }
        auto* compiler = Compiler(ctx);
        compiler->LogErrorMessage(
            "cannot bind a '{}' value to array view '{}' - a view indexes whole '{}' elements, so "
            "its source must be an array view (or a null one), not a single value",
            { sourceSpelling, SpellType(*compiler, target), target.TypeName });
        return true;
    }

/*
 * The address of a const C++ object into a mutable C++ record pointer slot: clang refuses the store
 * (discards qualifiers). Every slot kind is refused, except a plain scalar local (the T225 pick).
 */
static std::string ConstPointeeSlotKind(llvm::Value* destination) {
    if (destination == nullptr) return {};
    if (auto* slot = llvm::dyn_cast<llvm::AllocaInst>(destination))
        return slot->getAllocatedType()->isArrayTy() ? "array element" : std::string();
    if (llvm::isa<llvm::GlobalVariable>(destination)) return "global";
    if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(destination))
        return gep->getSourceElementType()->isStructTy() ? "field" : "array element";
    return "dereferenced slot";
}

bool MainListener::RejectConstRecordPointeeStore(antlr4::ParserRuleContext* ctx,
                                     const LLVMBackend::TypeAndValue& target,
                                     const LLVMBackend::NamedVariable& rhsNV,
                                     const std::string& kind, const std::string& name) {
        auto* compiler = Compiler(ctx);
        if (!target.Pointer || target.PointerDepth > 1 || !compiler->IsCxxRecord(target.TypeName))
            return false;
        if (compiler->CxxRecordPointeeConstRelation(rhsNV.TypeAndValue, target)
            != LLVMBackend::CxxPointeeConstRelation::Drops)
            return false;
        const std::string className = compiler->DisplayCxxClassName(target.TypeName);
        LogErrorContext(ctx, std::format(
            "cannot assign the address of a const C++ object to {} '{}': a mutable '{} *' {} could "
            "write through read-only storage. Declare it as 'const {} *' instead.",
            kind, name, className, kind, className));
        return true;
    }

bool MainListener::RejectArrayViewElementMismatch(antlr4::ParserRuleContext* ctx,
                                     const LLVMBackend::TypeAndValue& target,
                                     const LLVMBackend::NamedVariable& rhsNV) {
        std::string destElement;
        std::string srcElement;
        if (!Compiler()->ArrayViewElementMismatch(target, rhsNV.TypeAndValue, destElement, srcElement))
            return false;
        if (target.IsArrayView)
            LogErrorContext(ctx, Compiler()->LocalizeMessage(
                "cannot bind an array view of '{}' to a destination of '{}' - a view indexes by its "
                "own element, so the element types must match", { srcElement, destElement }));
        else
            // Destination is a plain pointer (the one-way 'T[] -> T*' decay), not a view - a
            // depth change is proven broken, but wording must not imply the destination is one.
            LogErrorContext(ctx, Compiler()->LocalizeMessage(
                "cannot bind an array view of '{}' to a plain pointer whose pointee is '{}' - only "
                "a view whose element matches the pointee may decay to a pointer", { srcElement, destElement }));
        return true;
    }

/*
 * [immovable] assignment source: `a = b` / `a = move b` transfers the source value into the
 * target, a move of an immovable value. Only a provably fresh rvalue (C++ prvalue, constructed in place) is accepted.
 * Returns true after logging the refusal.
 */
bool MainListener::RefuseImmovableAssignSource(LLVMBackend* compiler, antlr4::ParserRuleContext* ctx,
                                        const std::string& operatorText, const LLVMBackend::NamedVariable& rightNV)
{
        if (operatorText != "=" || rightNV.TypeAndValue.Pointer) return false;
        if (!rightNV.IsExplicitMove && compiler->IsImmovableFreshOperand(rightNV)) return false;
        // `a = default;` rebuilds the target in place; no value is transferred.
        if (auto* assign = dynamic_cast<CFlatParser::AssignmentExpressionContext*>(ctx);
            assign != nullptr && IsDefaultOnlyExpression(assign->assignmentExpression()))
            return false;
        if (!compiler->IsImmovableType(rightNV.TypeAndValue.TypeName)) return false;
        LogErrorContext(ctx, std::format(
            "cannot assign a value of type '{}': the type is [immovable], so assignment may not "
            "transfer it (construct it in place instead)",
            SpellDiagnosticType(*compiler, rightNV.TypeAndValue)));
        return true;
}

// T250: true when every value arm of an assignment RHS is a direct `new` or a `nullptr` constant,
// with at least one `new` (`c ? new T() : nullptr`). Parse-tree shape only; any other arm keeps
// the borrow. Recurses through the ?: chain and parentheses, as RawNewArrayValueOf does.
static bool OwnedOrNullArms(antlr4::tree::ParseTree* node, bool& sawNew)
{
    if (node == nullptr) return false;
    // The lexer emits `nullptr` as a Constant token (same as PostfixExpression's constant check).
    if (auto* term = dynamic_cast<antlr4::tree::TerminalNode*>(node))
        return term->getSymbol()->getType() == CFlatParser::Constant
            && term->getText() == "nullptr";
    if (dynamic_cast<CFlatParser::NewExpressionContext*>(node) != nullptr)
    {
        sawNew = true;
        return true;
    }
    if (auto* conditional = dynamic_cast<CFlatParser::ConditionalExpressionContext*>(node))
    {
        if (conditional->QuestionQuestion() != nullptr) return false;
        if (conditional->Question() != nullptr)
            return OwnedOrNullArms(conditional->expression(), sawNew)
                && OwnedOrNullArms(conditional->conditionalExpression(), sawNew);
        return OwnedOrNullArms(conditional->logicalOrExpression(), sawNew);
    }
    if (auto* ctx = dynamic_cast<antlr4::ParserRuleContext*>(node))
    {
        if (ctx->children.size() == 1)
            return OwnedOrNullArms(ctx->children[0], sawNew);
        if (auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(ctx);
            primary != nullptr && primary->expression() != nullptr && primary->children.size() == 3)
            return OwnedOrNullArms(primary->expression(), sawNew);
    }
    return false;
}

// T413: a raw `new` result (string slots of a raw array are never destructed) or null.
static bool IsRawNewOrNullValue(llvm::Value* value)
{
    value = value->stripPointerCasts();
    if (llvm::isa<llvm::ConstantPointerNull>(value)) return true;
    auto* call = llvm::dyn_cast<llvm::CallBase>(value);
    auto* callee = call != nullptr ? call->getCalledFunction() : nullptr;
    if (callee == nullptr) return false;
    llvm::StringRef name = callee->getName();
    return name.starts_with("_operator new$") || name.starts_with("__cflat_opnew_");
}

// T413: true when the defined callee only stores raw `new` results or null through parameter
// `index` and the parameter reaches nothing else. Undefined or indirect callees are unproven.
static bool CalleeStoresOnlyRawNewThroughParam(llvm::Function* callee, unsigned index)
{
    if (callee == nullptr || callee->isDeclaration() || index >= callee->arg_size()) return false;
    llvm::Argument* parameter = callee->getArg(index);
    auto loadUsesStoreOnlyRawNew = [&](llvm::LoadInst* load) {
        for (auto* user : load->users())
        {
            if (llvm::isa<llvm::LoadInst>(user)) continue;
            auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
            if (store == nullptr || store->getPointerOperand() != load
                || store->getValueOperand() == load || !IsRawNewOrNullValue(store->getValueOperand()))
                return false;
        }
        return true;
    };
    for (auto* user : parameter->users())
    {
        auto* spill = llvm::dyn_cast<llvm::StoreInst>(user);
        auto* slot = spill != nullptr
            ? llvm::dyn_cast<llvm::AllocaInst>(spill->getPointerOperand()->stripPointerCasts()) : nullptr;
        if (slot == nullptr || spill->getValueOperand() != parameter) return false;
        for (auto* slotUser : slot->users())
        {
            if (auto* store = llvm::dyn_cast<llvm::StoreInst>(slotUser))
            {
                if (store->getValueOperand() != parameter) return false;
                continue;
            }
            auto* load = llvm::dyn_cast<llvm::LoadInst>(slotUser);
            if (load == nullptr || !loadUsesStoreOnlyRawNew(load)) return false;
        }
    }
    return true;
}

// T413: the outermost loop enclosing `node` inside its function or lambda body, or null.
static antlr4::ParserRuleContext* OutermostEnclosingLoop(antlr4::tree::ParseTree* node)
{
    antlr4::ParserRuleContext* loop = nullptr;
    for (auto* current = node; current != nullptr; current = current->parent)
    {
        if (dynamic_cast<CFlatParser::FunctionDefinitionContext*>(current) != nullptr
            || dynamic_cast<CFlatParser::LambdaExpressionContext*>(current) != nullptr)
            break;
        if (auto* iteration = dynamic_cast<CFlatParser::IterationStatementContext*>(current))
            loop = iteration;
    }
    return loop;
}

// T413: climb parents that wrap `node` as their only child.
static antlr4::tree::ParseTree* LiftSingleChild(antlr4::tree::ParseTree* node)
{
    while (node->parent != nullptr && node->parent->children.size() == 1) node = node->parent;
    return node;
}

// T413: does a declarator list declare `name`?
static bool InitDeclaratorsDeclare(antlr4::tree::ParseTree* list, const std::string& name)
{
    if (list == nullptr) return false;
    if (auto* init = dynamic_cast<CFlatParser::InitDeclaratorContext*>(list))
    {
        auto* direct = init->declarator() != nullptr ? init->declarator()->directDeclarator() : nullptr;
        return direct != nullptr && direct->Identifier() != nullptr && direct->Identifier()->getText() == name;
    }
    for (auto* child : list->children)
        if ((dynamic_cast<CFlatParser::InitDeclaratorContext*>(child) != nullptr
             || dynamic_cast<CFlatParser::InitDeclaratorListContext*>(child) != nullptr)
            && InitDeclaratorsDeclare(child, name))
            return true;
    return false;
}

// T413: the latest declaration of `name` among block items of `list` (nested lists included).
static antlr4::tree::ParseTree* LatestBlockDeclaration(antlr4::tree::ParseTree* list,
                                                       antlr4::tree::ParseTree* stopAt, const std::string& name)
{
    antlr4::tree::ParseTree* found = nullptr;
    for (auto* child : list->children)
    {
        if (child == stopAt) break;
        if (dynamic_cast<CFlatParser::BlockItemListContext*>(child) != nullptr)
        {
            if (auto* inner = LatestBlockDeclaration(child, nullptr, name)) found = inner;
        }
        else if (auto* item = dynamic_cast<CFlatParser::BlockItemContext*>(child);
                 item != nullptr && item->declaration() != nullptr
                 && InitDeclaratorsDeclare(item->declaration()->initDeclaratorList(), name))
            found = item->declaration();
    }
    return found;
}

// T413: the local declaration `name` at `node` binds to, by lexical scope (block items before
// the use, for/range-for and lambda parameters); null for parameters, fields and globals.
static antlr4::tree::ParseTree* ResolveLocalDeclaration(antlr4::tree::ParseTree* node, const std::string& name)
{
    for (auto* child = node, *current = node->parent; current != nullptr; child = current, current = current->parent)
    {
        if (dynamic_cast<CFlatParser::FunctionDefinitionContext*>(current) != nullptr) return nullptr;
        if (dynamic_cast<CFlatParser::BlockItemListContext*>(current) != nullptr)
        {
            if (auto* declaration = LatestBlockDeclaration(current, child, name)) return declaration;
        }
        else if (auto* iteration = dynamic_cast<CFlatParser::IterationStatementContext*>(current))
        {
            if (iteration->Identifier() != nullptr && iteration->Identifier()->getText() == name)
                return iteration;
            if (auto* condition = iteration->forCondition();
                condition != nullptr && condition->forDeclaration() != nullptr
                && InitDeclaratorsDeclare(condition->forDeclaration()->initDeclaratorList(), name))
                return condition->forDeclaration();
        }
        else if (auto* lambda = dynamic_cast<CFlatParser::LambdaExpressionContext*>(current);
                 lambda != nullptr && lambda->lambdaParamList() != nullptr)
            for (auto* param : lambda->lambdaParamList()->lambdaParam())
                if (param->Identifier() != nullptr && param->Identifier()->getText() == name) return param;
    }
    return nullptr;
}

// T413: every use in `loop` of the holder `pp` or its referred slot keeps raw-new provenance:
// reads `(*pp)[i]` / `slot[i]`, `delete[] slot`, and stores of `new` / nullptr arms only.
// Uses are matched by declaration identity (`holderDecl` / `slotDecl` from the read point).
static bool LoopUsesKeepRawNewProvenance(antlr4::tree::ParseTree* loop, const std::string& holderName,
                                         antlr4::tree::ParseTree* holderDecl, const std::string& slotName,
                                         antlr4::tree::ParseTree* slotDecl)
{
    auto rawNewAssignmentTarget = [](antlr4::tree::ParseTree* lhs) {
        auto* assign = dynamic_cast<CFlatParser::AssignmentExpressionContext*>(lhs->parent);
        if (assign == nullptr || assign->unaryExpression() != lhs || assign->assignmentOperator() == nullptr
            || assign->assignmentOperator()->getText() != "=")
            return false;
        bool sawNew = false;
        return OwnedOrNullArms(assign->assignmentExpression(), sawNew);
    };
    std::function<bool(antlr4::tree::ParseTree*)> visit = [&](antlr4::tree::ParseTree* node) {
        auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node);
        if (primary != nullptr && (primary->Identifier() != nullptr || primary->genericIdentifier() != nullptr))
        {
            const std::string name = primary->Identifier() != nullptr
                ? primary->Identifier()->getText() : primary->genericIdentifier()->getText();
            const bool isSlot = name == slotName && ResolveLocalDeclaration(primary, name) == slotDecl;
            if (name == holderName && ResolveLocalDeclaration(primary, name) == holderDecl)
            {
                // Only `*pp`, then either `(*pp)[i]` or `*pp = new/nullptr`.
                auto* lifted = LiftSingleChild(primary);
                auto* deref = dynamic_cast<CFlatParser::UnaryExpressionContext*>(lifted->parent);
                if (deref == nullptr || deref->unaryOperator() == nullptr
                    || deref->unaryOperator()->Star() == nullptr)
                    return false;
                if (rawNewAssignmentTarget(deref)) return true;
                auto* derefLifted = LiftSingleChild(deref);
                auto* paren = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(derefLifted->parent);
                auto* subscript = paren != nullptr && paren->LeftParen() != nullptr
                    ? dynamic_cast<CFlatParser::PostfixExpressionContext*>(paren->parent) : nullptr;
                return subscript != nullptr && subscript->children.size() >= 2
                    && subscript->children[1]->getText() == "[";
            }
            if (isSlot)
            {
                auto* postfix = dynamic_cast<CFlatParser::PostfixExpressionContext*>(primary->parent);
                if (postfix != nullptr && postfix->children.size() >= 2
                    && postfix->children[1]->getText() == "[")
                    return true;
                auto* lifted = LiftSingleChild(primary);
                if (dynamic_cast<CFlatParser::DeleteExpressionContext*>(lifted->parent) != nullptr)
                    return true;
                // `&slot` bound into the holder itself (`string** pp = &raw;` / `pp = &raw;`).
                if (auto* addressOf = dynamic_cast<CFlatParser::UnaryExpressionContext*>(lifted->parent);
                    addressOf != nullptr && addressOf->unaryOperator() != nullptr
                    && addressOf->unaryOperator()->And() != nullptr)
                {
                    auto* bound = LiftSingleChild(addressOf);
                    auto* init = dynamic_cast<CFlatParser::InitializerContext*>(bound);
                    if (init == nullptr) init = dynamic_cast<CFlatParser::InitializerContext*>(bound->parent);
                    if (init != nullptr && InitDeclaratorsDeclare(init->parent, holderName)
                        && init->parent->parent != nullptr && init->parent->parent->parent == holderDecl)
                        return true;
                    if (auto* assign = dynamic_cast<CFlatParser::AssignmentExpressionContext*>(bound->parent);
                        assign != nullptr && assign->assignmentExpression() == bound
                        && assign->unaryExpression() != nullptr
                        && assign->unaryExpression()->getText() == holderName
                        && ResolveLocalDeclaration(assign, holderName) == holderDecl)
                        return true;
                    return false;
                }
                return rawNewAssignmentTarget(lifted);
            }
        }
        for (auto* child : node->children)
            if (!visit(child)) return false;
        return true;
    };
    return visit(loop);
}

bool MainListener::IsDeclaredPointerSubscriptString(
        CFlatParser::AssignmentExpressionContext* expression,
        llvm::Value* value, llvm::Value* storage)
{
        if (expression == nullptr || value == nullptr) return false;
        auto* stringType = llvm::StructType::getTypeByName(*compilerLLVM->context, "string");
        if (stringType == nullptr || value->getType() != stringType) return false;

        auto* postfix = tryGetPostfixExpression(expression);
        // R21: `Ns.g[i]` - a `.` member path ahead of the subscript; only a global slot decides it.
        const size_t memberPathChildren = postfix != nullptr && postfix->children.size() > 4
            ? postfix->children.size() - 4 : 0;
        bool memberPathOnly = memberPathChildren > 0 && memberPathChildren % 2 == 0
            && postfix->memberNameToken().size() == memberPathChildren / 2
            && postfix->genericTypeParameters().empty();
        for (size_t i = 1; memberPathOnly && i <= memberPathChildren; i += 2)
            memberPathOnly = postfix->children[i]->getText() == ".";
        if (postfix == nullptr || postfix->LeftBracket().size() != 1
            || postfix->RightBracket().size() != 1
            || postfix->expression().size() != 1
            || (postfix->children.size() != 4 && !memberPathOnly)
            || postfix->primaryExpression() == nullptr
            || postfix->children.front() != postfix->primaryExpression()
            || postfix->children.back() != postfix->RightBracket().back())
            return false;

        std::function<bool(CFlatParser::PrimaryExpressionContext*)> isBareBasePrimary;
        std::function<bool(CFlatParser::PostfixExpressionContext*)> isBareBasePostfix;
        isBareBasePostfix = [&](CFlatParser::PostfixExpressionContext* base) {
            return base != nullptr && base->children.size() == 1
                && base->primaryExpression() != nullptr
                && isBareBasePrimary(base->primaryExpression());
        };
        isBareBasePrimary = [&](CFlatParser::PrimaryExpressionContext* primary) {
            if (primary == nullptr) return false;
            if (primary->Identifier() != nullptr || primary->genericIdentifier() != nullptr)
                return true;
            if (primary->LeftParen() == nullptr || primary->expression() == nullptr)
                return false;
            auto* inner = primary->expression();
            if (auto* innerPostfix = tryGetPostfixExpression(inner))
                return isBareBasePostfix(innerPostfix);
            auto* innerUnary = tryGetUnaryExpression(inner);
            if (innerUnary == nullptr || innerUnary->unaryOperator() == nullptr
                || innerUnary->castExpression() == nullptr)
                return true;
            auto* unaryBase = tryGetPostfixExpression(innerUnary->castExpression());
            return unaryBase == nullptr || isBareBasePostfix(unaryBase);
        };
        if (!isBareBasePrimary(postfix->primaryExpression())) return false;
        if (memberPathOnly && postfix->primaryExpression()->Identifier() == nullptr
            && postfix->primaryExpression()->genericIdentifier() == nullptr)
            return false;

        auto* basePrimary = postfix->primaryExpression();
        bool parenthesizedPointerDeref = false;
        if (basePrimary->LeftParen() != nullptr && basePrimary->expression() != nullptr)
        {
            auto* innerUnary = tryGetUnaryExpression(basePrimary->expression());
            auto* derefBase = innerUnary != nullptr && innerUnary->castExpression() != nullptr
                ? tryGetPostfixExpression(innerUnary->castExpression()) : nullptr;
            parenthesizedPointerDeref = innerUnary != nullptr
                && innerUnary->unaryOperator() != nullptr
                && innerUnary->unaryOperator()->Star() != nullptr
                && derefBase != nullptr && derefBase->children.size() == 1
                && derefBase->primaryExpression() != nullptr
                && (derefBase->primaryExpression()->Identifier() != nullptr
                    || derefBase->primaryExpression()->genericIdentifier() != nullptr);
        }

        llvm::Value* subscriptBase = nullptr;
        auto* sourceGep = llvm::dyn_cast_or_null<llvm::GEPOperator>(storage);
        if (sourceGep != nullptr)
        {
            if (sourceGep->getNumIndices() != 1) return false;
            subscriptBase = sourceGep->getPointerOperand();
        }
        else if (auto* sourceLoad = llvm::dyn_cast<llvm::LoadInst>(value))
            subscriptBase = sourceLoad->getPointerOperand();
        else
            return false;

        auto* pointerLoad = llvm::dyn_cast_or_null<llvm::LoadInst>(subscriptBase);
        llvm::Value* pointerSlot = pointerLoad != nullptr
            ? pointerLoad->getPointerOperand() : subscriptBase;
        if (pointerSlot == nullptr) return false;

        const LLVMBackend::NamedVariable* pointerBinding = nullptr;
        bool referredRawNewArray = false;
        bool holderDecides = false;
        bool holderProvenRawNew = false;
        if (llvm::isa<llvm::AllocaInst>(pointerSlot) || llvm::isa<llvm::Argument>(pointerSlot))
            pointerBinding = compilerLLVM->FindVariableByStorage(pointerSlot);
        else if (parenthesizedPointerDeref)
        {
            // `(*pp)[i]` loads the address of `ps` from pp, then loads ps to form the GEP.
            // Follow only a single local store so raw-new provenance stays attached to ps.
            auto* addressLoad = llvm::dyn_cast<llvm::LoadInst>(pointerSlot);
            auto* holderSlot = addressLoad != nullptr ? addressLoad->getPointerOperand() : nullptr;
            auto* holderAlloca = llvm::dyn_cast_or_null<llvm::AllocaInst>(holderSlot);
            const auto* holderBinding = holderAlloca != nullptr
                ? compilerLLVM->FindVariableByStorage(holderAlloca) : nullptr;
            if (holderAlloca != nullptr && holderBinding != nullptr
                && holderBinding->TypeAndValue.Pointer
                && holderBinding->TypeAndValue.PointerDepth >= 2
                && !holderBinding->TypeAndValue.IsArrayView
                && !holderBinding->IsElementAccess
                && holderBinding->FieldName.empty()
                && holderBinding->OwningStructName.empty())
            {
                llvm::Value* referredSlot = nullptr;
                bool ambiguousStore = false;
                for (auto& block : *holderAlloca->getFunction())
                    for (auto& instruction : block)
                    {
                        auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
                        if (store == nullptr) continue;
                        // A store through `*pp` rebinds the referred pointer behind its binding's back.
                        auto* throughLoad = llvm::dyn_cast<llvm::LoadInst>(store->getPointerOperand()->stripPointerCasts());
                        if (throughLoad != nullptr && throughLoad->getPointerOperand()->stripPointerCasts() == holderAlloca)
                        {
                            // The provenance scan below decides whether the stored value may be adopted.
                            ambiguousStore = true;
                            continue;
                        }
                        if (store->getPointerOperand()->stripPointerCasts() == holderAlloca)
                        {
                            auto* storedAddress = store->getValueOperand()->stripPointerCasts();
                            if (!llvm::isa<llvm::AllocaInst>(storedAddress)
                                && !llvm::isa<llvm::Argument>(storedAddress))
                            {
                                ambiguousStore = true;
                                break;
                            }
                            if (referredSlot != nullptr && referredSlot != storedAddress)
                            {
                                ambiguousStore = true;
                                break;
                            }
                            referredSlot = storedAddress;
                        }
                    }
                // Single pass: a later store in a loop body, or any write through a second address
                // of pp or of the referred slot, is not visible here. Allow-list the uses; else default path.
                if (!loopBreakMovedStates_.empty()) ambiguousStore = true;
                // Adopt only when every value the referred slot can hold is a raw `new` result or null:
                // direct stores, stores through pp, defined callees, and later stores in an enclosing loop.
                if (referredSlot != nullptr && parenthesizedPointerDeref)
                {
                    holderDecides = true;
                    auto* slotAlloca = llvm::dyn_cast<llvm::AllocaInst>(referredSlot);
                    auto storedRawNew = [&](llvm::Value* stored) {
                        return IsRawNewOrNullValue(stored)
                            || compilerLLVM->FindRawArrayResult(stored->stripPointerCasts()) != nullptr;
                    };
                    auto callUseProven = [&](llvm::CallBase* call, llvm::Use& use) {
                        if (llvm::isa<llvm::IntrinsicInst>(call)) return true;
                        return call->isArgOperand(&use) && CalleeStoresOnlyRawNewThroughParam(
                            call->getCalledFunction(), call->getArgOperandNo(&use));
                    };
                    bool proven = slotAlloca != nullptr;
                    for (auto& use : holderAlloca->uses())
                    {
                        if (!proven) break;
                        auto* user = use.getUser();
                        if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                        {
                            proven = store->getPointerOperand() == holderAlloca
                                && store->getValueOperand()->stripPointerCasts() == referredSlot;
                            continue;
                        }
                        auto* load = llvm::dyn_cast<llvm::LoadInst>(user);
                        if (load == nullptr) { proven = false; break; }
                        for (auto& loadUse : load->uses())
                        {
                            auto* loadUser = loadUse.getUser();
                            if (llvm::isa<llvm::LoadInst>(loadUser)) continue;
                            if (auto* store = llvm::dyn_cast<llvm::StoreInst>(loadUser);
                                store != nullptr && store->getPointerOperand() == load
                                && store->getValueOperand() != load)
                            {
                                if (!storedRawNew(store->getValueOperand())) proven = false;
                                continue;
                            }
                            auto* call = llvm::dyn_cast<llvm::CallBase>(loadUser);
                            if (call == nullptr || !callUseProven(call, loadUse)) proven = false;
                        }
                    }
                    if (proven)
                        for (auto& use : referredSlot->uses())
                        {
                            auto* user = use.getUser();
                            if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                            {
                                if (store->getPointerOperand() == referredSlot)
                                    proven = proven && store->getValueOperand() != referredSlot
                                        && storedRawNew(store->getValueOperand());
                                else
                                    proven = proven && store->getPointerOperand() == holderAlloca;
                                continue;
                            }
                            if (llvm::isa<llvm::LoadInst>(user)) continue;
                            auto* call = llvm::dyn_cast<llvm::CallBase>(user);
                            proven = proven && call != nullptr && callUseProven(call, use);
                        }
                    // Stores later in an enclosing loop are not emitted yet; check them on the parse tree.
                    if (proven)
                        if (auto* loop = OutermostEnclosingLoop(expression))
                        {
                            // Both names must bind to these exact slots at the read, so the read
                            // point's lexical declarations identify them inside the loop.
                            auto* derefUnary = tryGetUnaryExpression(basePrimary->expression());
                            auto* derefPostfix = tryGetPostfixExpression(derefUnary->castExpression());
                            const std::string holderName = derefPostfix->primaryExpression()->getText();
                            const std::string slotName = compilerLLVM->FindVariableNameByStorage(referredSlot);
                            proven = !slotName.empty()
                                && compilerLLVM->FindVariableStorage(slotName).Storage == referredSlot
                                && compilerLLVM->FindVariableStorage(holderName).Storage == holderAlloca
                                && LoopUsesKeepRawNewProvenance(loop,
                                    holderName, ResolveLocalDeclaration(expression, holderName),
                                    slotName, ResolveLocalDeclaration(expression, slotName));
                        }
                    holderProvenRawNew = proven;
                }
                // The referred slot must also be stored exactly once: its raw-new flag is not path-sensitive.
                auto onlyPlainUses = [&](llvm::Value* slot, bool loadsMayFeedLoads) {
                    int storesIntoSlot = 0;
                    for (auto* user : slot->users())
                    {
                        if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                        {
                            const bool intoSlot = store->getPointerOperand()->stripPointerCasts() == slot
                                && store->getValueOperand() != slot;
                            const bool addressIntoHolder = slot != holderAlloca
                                && store->getPointerOperand()->stripPointerCasts() == holderAlloca;
                            if (!intoSlot && !addressIntoHolder) return false;
                            if (intoSlot && slot != holderAlloca && ++storesIntoSlot > 1) return false;
                            continue;
                        }
                        auto* load = llvm::dyn_cast<llvm::LoadInst>(user);
                        if (load == nullptr) return false;
                        if (loadsMayFeedLoads)
                            for (auto* loadUser : load->users())
                                if (!llvm::isa<llvm::LoadInst>(loadUser)) return false;
                    }
                    return true;
                };
                if (!ambiguousStore && (!onlyPlainUses(holderAlloca, true)
                    || (referredSlot != nullptr && !onlyPlainUses(referredSlot, false))))
                    ambiguousStore = true;
                if (referredSlot != nullptr)
                {
                    const auto* referredBinding = compilerLLVM->FindVariableByStorage(referredSlot);
                    referredRawNewArray = referredBinding != nullptr
                        && referredBinding->AllocatedByRawNewArray;
                    if (!ambiguousStore) pointerBinding = referredBinding;
                }
            }
        }

        const bool pointerBindingProvesLocalElement = pointerBinding != nullptr
            && pointerBinding->TypeAndValue.Pointer
            && !pointerBinding->TypeAndValue.IsArrayView
            && !pointerBinding->IsElementAccess
            && pointerBinding->FieldName.empty()
            && pointerBinding->OwningStructName.empty()
            && !pointerBinding->AllocatedByRawNewArray;
        llvm::Value* rawArrayBase = subscriptBase;
        if (auto* elementGep = llvm::dyn_cast_or_null<llvm::GEPOperator>(rawArrayBase);
            elementGep != nullptr && elementGep->getNumIndices() == 1)
            rawArrayBase = elementGep->getPointerOperand();
        const auto* rawArrayTarget = compilerLLVM->FindRawArrayResult(rawArrayBase);
        if (rawArrayTarget == nullptr && rawArrayBase != nullptr)
            rawArrayTarget = compilerLLVM->FindRawArrayResult(rawArrayBase->stripPointerCasts());
        const bool rawArrayResultTarget = rawArrayTarget != nullptr && !rawArrayTarget->IsArrayView;
        const bool rawNewArrayTarget = rawArrayResultTarget || referredRawNewArray
            || (pointerBinding != nullptr && pointerBinding->AllocatedByRawNewArray);
        // `(*pp)[i]` through a single referred local slot: adopt only a proven raw-new element.
        if (holderDecides) return !holderProvenRawNew;
        // R21 (ruling 2026-10-27): a base loaded from a global, field or closure-capture slot has no
        // compile-time provenance, so the read copies (C++ `std::string t = g[0]`).
        auto* slotGep = llvm::dyn_cast<llvm::GEPOperator>(pointerSlot);
        // A pointer-typed global, not a struct global whose field-zero GEP folded to the global.
        auto* slotGlobal = llvm::dyn_cast<llvm::GlobalVariable>(pointerSlot->stripPointerCasts());
        const bool globalSlot = slotGlobal != nullptr && slotGlobal->getValueType()->isPointerTy();
        const bool fieldSlot = slotGep != nullptr && slotGep->getSourceElementType()->isStructTy();
        if (pointerLoad != nullptr && pointerBinding == nullptr && !parenthesizedPointerDeref
            && pointerLoad->getType()->isPointerTy() && (globalSlot || (fieldSlot && !memberPathOnly))
            && (rawArrayTarget == nullptr || !rawArrayTarget->IsArrayView))
        {
            // A container's `_data[i]` in a generic instance over string keeps its bit-move: the
            // container consumes the read (swap, return), so a copy would leak the element.
            bool genericOverString = false;
            for (const auto& [param, argument] : activeTypeSubstitutions)
                genericOverString = genericOverString || argument == "string";
            if (globalSlot || !genericOverString) return true;
        }
        if (memberPathOnly) return false;
        // An unproven `(*pp)[i]` can be borrowed; its copy sites apply owned-array exceptions.
        return !rawNewArrayTarget && (pointerBindingProvesLocalElement || parenthesizedPointerDeref);
    }

llvm::Value* MainListener::ParseAssignmentExpression(
        CFlatParser::AssignmentExpressionContext* ctx,
        LLVMBackend::NamedVariable* assignmentResult) {
        auto* compiler = Compiler(ctx);
        // Block the assignment STATEMENT starts in: a borrow stored there is retired by an owning store
        // later in the same statement, however many blocks the RHS creates on the way.
        llvm::BasicBlock* assignStartBlock = compiler->builder->GetInsertBlock();
        auto condCtx = ctx->conditionalExpression();
        auto assignmentOp = ctx->assignmentOperator();
        auto unaryCtx = ctx->unaryExpression();

        if (condCtx != nullptr)
        {
            return ParseConditionalExpression(condCtx);
        }
        else if (assignmentOp != nullptr)
        {
            CallPostfixScope assignmentOperatorPostfix(this);
            // An enclosing declarator's / return's armed slot is hidden from the operand parse and
            // shown only to this assignment's own operator call (CxxAssignSretScope).
            CxxAssignSretScope enclosingSret(compiler);
            // Block the assignment STARTS in: evaluating the RHS may split it (an invoke under C++
            // unwinding, a guarded temp drop) though the store still runs whenever this point does.
            auto* assignmentBlock = compiler->builder->GetInsertBlock();
            auto operatorText = ctx->assignmentOperator()->getText();
            auto assignCtx = ctx->assignmentExpression();
            // `_ = expr` is an explicit discard: evaluate the RHS for its side effects, drop the
            // result, and - like a bare-statement temp - destruct an owning-struct rvalue at the
            // end of the full expression. `_` is never an lvalue, so only the plain `=` form is
            // meaningful; a compound op (`_ += x`) would have to read `_` and is rejected.
            if ((unaryCtx != nullptr && unaryCtx->getText() == "_")
                || ctx->getText().starts_with("_="))
            {
                if (operatorText != "=")
                    LogErrorContext(unaryCtx,
                        "'_' is a discard target; only '_ = expr' is allowed, not compound assignment.");

                // `_ = move <bare local>;` explicitly releases the owning local NOW: it runs the
                // same per-type teardown as scope exit, then the local is moved-from (a later read
                // is a compile error). `_ = move <bare global>;` gets the same NOW-release, minus
                // the compile-time moved-from tracking (globals have no stackNamedVariable frame to
                // hold it) - the nulled storage still makes a later read see it as moved-out at
                // runtime. Only the top-level bare-identifier move form is special; `_ = move
                // obj.field`, `_ = <call>()`, `_ = expr` keep the generic discard below.
                if (auto* mv = TopLevelMoveExpression(assignCtx))
                {
                    auto* inner = mv->unaryExpression();
                    std::string name = inner ? inner->getText() : std::string();
                    if (IsBareIdentifierText(name))
                    {
                        if (auto* nv = compiler->FindLiveNamedVariable(name))
                        {
                            RefuseConstObjectWrite(inner, *nv, nv->Storage, "move");
                            ReleaseOwningLocalNow(unaryCtx, nv, name);
                            return nullptr;
                        }
                        if (compiler->GetGlobalVariable(name) != nullptr)
                        {
                            auto globalNV = compiler->GetGlobalVariableNV(name);
                            RefuseConstObjectWrite(inner, globalNV, globalNV.Storage, "move");
                            ReleaseOwningGlobalNow(unaryCtx, name);
                            return nullptr;
                        }
                    }
                }

                // `move` may sit under parentheses (`_ = (move a[0])`); unwrap `( expr )` levels.
                auto* discardMove = [&]() -> CFlatParser::MoveExpressionContext* {
                    antlr4::tree::ParseTree* node = assignCtx;
                    while (node != nullptr)
                    {
                        if (auto* mv = dynamic_cast<CFlatParser::MoveExpressionContext*>(node))
                            return mv;
                        if (node->children.size() == 3 && node->children[0]->getText() == "("
                            && node->children[2]->getText() == ")"
                            && dynamic_cast<antlr4::tree::TerminalNode*>(node->children[0]))
                            node = node->children[1];
                        else if (node->children.size() == 1)
                            node = node->children[0];
                        else
                            return nullptr;
                    }
                    return nullptr;
                }();
                const bool explicitMoveDiscard = discardMove != nullptr;
                // A fixed-array or `T[]` view slot stays LIVE after the move (its owner tears it
                // down); a container's raw `_data[i]` slot ends its lifetime here instead.
                auto isLiveArraySlot = [](llvm::Value* slot, bool viewElement) {
                    auto* gep = llvm::dyn_cast_or_null<llvm::GEPOperator>(slot);
                    if (gep == nullptr) return false;
                    return (gep->getNumIndices() == 2 && gep->getSourceElementType()->isArrayTy())
                        || (gep->getNumIndices() == 1 && viewElement);
                };
                compiler->lastCxxRetTemp_ = nullptr;
                compiler->lastCxxRetValue_ = nullptr;
                auto rhsNV = ParseAssignmentExpressionNamed(assignCtx);
                // The discarded owning result is dropped here, never bound: a stale owned-call flag
                // must not reach the next pointer declaration or assignment (T199 r2).
                compiler->lastCallReturnsOwned = false;
                compiler->lastOwningResult = false;

                /*
                 * A foreign C++ element in a live array slot is still destroyed by the array. Like
                 * clang's `{ T t = std::move(arr[i]); }`, `_ = move arr[i]` move-constructs a temp
                 * and destroys it, leaving a valid moved-from slot (R3); a plain `_ = arr[i]` is
                 * clang's `(void)arr[i]`. A container's raw slot discard is the release itself.
                 */
                if (rhsNV.IsElementAccess && !rhsNV.TypeAndValue.Pointer
                    && rhsNV.Storage != nullptr
                    && compiler->IsForeignNontrivialCxxClass(rhsNV.TypeAndValue.TypeName))
                {
                    llvm::Type* elementType = rhsNV.BaseType != nullptr
                        ? rhsNV.BaseType : compiler->GetType(rhsNV.TypeAndValue);
                    if ((explicitMoveDiscard || rhsNV.IsExplicitMove) && elementType != nullptr
                        && isLiveArraySlot(rhsNV.Storage, rhsNV.IsViewElement))
                    {
                        // Clang's pick for the xvalue: a const element copies, a deleted move
                        // ctor is refused, as for `T x = move a[i]`.
                        const std::string& typeName = rhsNV.TypeAndValue.TypeName;
                        const bool constSource = compiler->CxxConstReceiverKind(rhsNV) != 0;
                        auto* temp = compiler->CreateAlloca(elementType);
                        bool constructed = false;
                        if (compiler->CxxExplicitMoveNeedsTransfer(typeName, constSource, false))
                        {
                            const std::string srcName = discardMove != nullptr
                                    && discardMove->unaryExpression() != nullptr
                                ? discardMove->unaryExpression()->getText()
                                : assignCtx->getText();
                            std::string refusal;
                            constructed = compiler->EmitCxxExplicitMoveTransfer(typeName, temp,
                                rhsNV.Storage, constSource, false, srcName, refusal);
                            if (!constructed)
                                LogErrorContext(assignCtx, refusal);
                        }
                        else
                            constructed = compiler->EmitCxxCopyOrMoveConstruct(typeName, temp,
                                rhsNV.Storage, true, "discarding a moved C++ element");
                        if (constructed)
                            if (auto* dtor = compiler->GetFullDestructorForDelete(typeName))
                                compiler->builder->CreateCall(dtor->getFunctionType(), dtor,
                                                              { temp });
                    }
                    else if (!isLiveArraySlot(rhsNV.Storage, rhsNV.IsViewElement))
                    {
                        if (auto* dtor = compiler->GetFullDestructorForDelete(
                                rhsNV.TypeAndValue.TypeName))
                            compiler->builder->CreateCall(dtor->getFunctionType(), dtor,
                                                          { rhsNV.Storage });
                    }
                    return rhsNV.Primary;
                }

                // `_ = move <container element slot>;` (e.g. `_ = move _data[i]`): the slot read
                // demoted an owning element to a borrow and there is no destination type to
                // re-derive from, so recover the element's true ownership from its desugared type
                // (carried onto the element by the subscript). ParseMoveExpression detached the
                // value (Storage null) and already
                // zeroed the slot, so materialize the value into a temp and run the SAME DropValue
                // teardown the `T tmp = move _data[i]` form gets: an owning ptr/iface/value/string
                // element frees exactly once, a bare borrow element frees nothing. This branch fully
                // owns the slot-move discard - it must NOT fall through to the struct-temp helper
                // (which mishandles a detached pointer element as an owning value and over-frees).
                bool slotMove = compiler->lastMovedFromContainerSlot;
                compiler->lastMovedFromContainerSlot = false;
                /*
                 * A lowered struct element is not trivially relocatable, so the bytes go back. A
                 * live array slot is then moved memberwise into a temp that is destroyed, leaving
                 * a valid moved-from object as clang's `std::move(arr[i])` does (R3): a zeroed
                 * std::list is not one. A container's raw slot is destroyed in place and zeroed.
                 */
                if (slotMove && rhsNV.Primary != nullptr
                    && compiler->IsLoweredCFlatOnlyStruct(rhsNV.TypeAndValue.TypeName)
                    && !rhsNV.TypeAndValue.Pointer)
                    if (auto* load = llvm::dyn_cast<llvm::LoadInst>(rhsNV.Primary))
                    {
                        auto* elementSlot = load->getPointerOperand();
                        compiler->builder->CreateStore(rhsNV.Primary, elementSlot);
                        auto* dtor = compiler->GetOrCreateFullDestructor(
                            rhsNV.TypeAndValue.TypeName);
                        if (isLiveArraySlot(elementSlot, rhsNV.IsViewElement))
                        {
                            auto* temp = compiler->CreateAlloca(rhsNV.Primary->getType());
                            if (compiler->EmitLoweredMemberwiseMove(rhsNV.TypeAndValue.TypeName,
                                    rhsNV.Primary->getType(), temp, elementSlot,
                                    "discarding a moved lowered element") && dtor != nullptr)
                                compiler->builder->CreateCall(dtor->getFunctionType(), dtor,
                                                              { temp });
                            return rhsNV.Primary;
                        }
                        if (dtor != nullptr)
                            compiler->builder->CreateCall(dtor->getFunctionType(), dtor,
                                                          { elementSlot });
                        compiler->builder->CreateStore(
                            llvm::Constant::getNullValue(rhsNV.Primary->getType()), elementSlot);
                        return rhsNV.Primary;
                    }
                if (slotMove && rhsNV.Primary != nullptr && rhsNV.BaseType != nullptr)
                {
                    LLVMBackend::NamedVariable slotNV;
                    slotNV.TypeAndValue = rhsNV.TypeAndValue;
                    slotNV.BaseType     = rhsNV.BaseType;
                    ApplyMovedSlotOwnership(slotNV, rhsNV.TypeAndValue);
                    auto* tmp = compiler->CreateAlloca(rhsNV.BaseType);
                    compiler->builder->CreateStore(rhsNV.Primary, tmp);
                    slotNV.Storage = tmp;
                    if (compiler->OwnsDroppableResource(slotNV))
                        compiler->DropValue(slotNV);
                    return rhsNV.Primary;
                }

                // Move a lowered field into a temp, then drop the moved-to object. Its source
                // C++ fields remain valid moved-from objects for assignment and scope cleanup.
                if (rhsNV.IsExplicitMove && rhsNV.TypeAndValue.IsMove
                    && !rhsNV.TypeAndValue.Pointer && rhsNV.BaseType != nullptr
                    && rhsNV.BaseType->isStructTy()
                    && compiler->IsLoweredCFlatOnlyStruct(rhsNV.TypeAndValue.TypeName)
                    && compiler->HasForeignNontrivialCxxField(rhsNV.TypeAndValue.TypeName)
                    && rhsNV.MoveSourceStorage != nullptr)
                {
                    auto* temp = compiler->CreateAlloca(rhsNV.BaseType);
                    compiler->builder->CreateStore(rhsNV.Primary, rhsNV.MoveSourceStorage);
                    if (compiler->EmitLoweredMemberwiseMove(rhsNV.TypeAndValue.TypeName,
                            rhsNV.BaseType, temp, rhsNV.MoveSourceStorage,
                            "discarding a moved lowered field"))
                    {
                        LLVMBackend::NamedVariable moved = rhsNV;
                        moved.Storage = temp;
                        moved.CallerName.clear();
                        moved.FieldName.clear();
                        moved.OwningStructName.clear();
                        moved.IsOwning = true;
                        if (compiler->OwnsDroppableResource(moved))
                            compiler->DropValue(moved);
                        compiler->lastOwningResult = false;
                        return rhsNV.Primary;
                    }
                }

                // `_ = <call>();` on an owning-POINTER return (`move R*`): the thing being dropped
                // is the returned POINTER, so free THAT (dtor + delete) at end-of-full-expression.
                if (const auto* owned = compiler->FindOwnedReturnEntry(rhsNV.Primary);
                    owned != nullptr && owned->IsOwningPtr)
                {
                    compiler->RegisterOwnedPtrTemp(rhsNV.Primary);
                    return rhsNV.Primary;
                }

                RegisterDiscardedOwningStructTemp(rhsNV);
                return rhsNV.Primary;
            }

            // The target of an assignment stays an array so a whole-member store is refused below.
            auto namedVar = [&]() {
                FlexibleArrayKeepScope keep(flexibleArrayKeep_, DirectOperandPostfixes(unaryCtx));
                return ParseUnaryExpression(unaryCtx);
            }();
            const llvm::GlobalVariable* enumMemberGlobal = nullptr;
            if (compiler->IsEnumMemberStorage(namedVar.Storage, &enumMemberGlobal))
                LogErrorContext(unaryCtx, std::format("cannot assign to enum member '{}'",
                    EnumMemberDiagnosticName(namedVar, unaryCtx, enumMemberGlobal)));
            if (operatorText != "=" && !namedVar.TypeAndValue.Pointer
                && compiler->IsCxxRecord(namedVar.TypeAndValue.TypeName))
            {
                antlr4::tree::ParseTree* node = unaryCtx;
                bool explicitMove = false;
                while (node != nullptr)
                {
                    if (dynamic_cast<CFlatParser::MoveExpressionContext*>(node) != nullptr)
                    {
                        explicitMove = true;
                        break;
                    }
                    if (auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node);
                        primary != nullptr && primary->expression() != nullptr
                        && primary->children.size() == 3)
                    {
                        node = primary->expression();
                        continue;
                    }
                    if (node->children.size() != 1) break;
                    node = node->children.front();
                }
                if (explicitMove)
                {
                    namedVar.IsExplicitMove = true;
                    namedVar.IsRvalue = true;
                }
            }
            if (operatorText != "=" && compiler->IsCxxRvalueRefClassResult(namedVar))
            {
                namedVar.Storage = namedVar.Primary;
                namedVar.Primary = nullptr;
                namedVar.BaseType = compiler->GetType(
                    LLVMBackend::TypeAndValue{ .TypeName = namedVar.TypeAndValue.TypeName });
                namedVar.TypeAndValue.Pointer = false;
                namedVar.TypeAndValue.IsRvalueRef = false;
                namedVar.IsRvalue = true;
            }
            if (namedVar.IsCxxMacroGlobal || namedVar.IsCxxMacroJoin)
                LogErrorContext(unaryCtx, std::format(
                    "cannot assign to macro constant '{}'", unaryCtx->getText()));
            auto destination = namedVar.Storage;
            struct SelfAssignPathRestore
            {
                std::string& slot;
                std::string saved;
                ~SelfAssignPathRestore() { slot = std::move(saved); }
            } selfAssignPathRestore{thisFieldAssignDestPath_, thisFieldAssignDestPath_};
            thisFieldAssignDestPath_ = operatorText == "=" ? ThisFieldSlotKey(namedVar) : std::string();
            bool constCxxCompoundCandidate = false;
            bool constCxxCompoundFreeCandidate = false;
            if (operatorText != "=" && !namedVar.TypeAndValue.Pointer
                && !namedVar.TypeAndValue.IsArrayView
                && compiler->IsCxxRecord(namedVar.TypeAndValue.TypeName)
                && compiler->CxxConstReceiverKind(namedVar) != 0)
            {
                const auto record = compiler->cxxRecordEntries_.find(
                    namedVar.TypeAndValue.TypeName);
                const std::string memberName = "operator" + operatorText;
                if (record != compiler->cxxRecordEntries_.end())
                    constCxxCompoundCandidate = std::any_of(record->second.members.begin(),
                        record->second.members.end(), [&](const auto& member) {
                            return member.kind == cflat_cinterop::RawCxxMember::Instance
                                && member.name == memberName && member.isConst
                                && !member.isDeleted && member.bindRefusal.empty()
                                && member.access == cflat_cinterop::AccessPublic;
                        });
                if (!constCxxCompoundCandidate)
                {
                    const std::string operatorName = "operator" + operatorText;
                    const auto sources = CxxFreeOperatorSources(
                        namedVar.TypeAndValue.TypeName, operatorName);
                    for (const std::string& source : sources)
                    {
                        if (compiler->HasCxxFunctionTemplateForRecordArgument(
                                source, namedVar.TypeAndValue.TypeName, 2))
                        {
                            constCxxCompoundCandidate = true;
                            constCxxCompoundFreeCandidate = true;
                            break;
                        }
                        compiler->TryBindCxxFunction(source);
                        auto freeCandidateBindsConstReceiver = [&](const auto& candidate) {
                            if (auto signatures = compiler->cxxFunctionSignatures_.find(
                                    candidate.SourceName);
                                signatures != compiler->cxxFunctionSignatures_.end())
                                for (const auto& signature : signatures->second)
                                    if (signature.linkageName == candidate.UniqueName
                                        && !signature.paramSpellings.empty())
                                    {
                                        const std::string& spelling = signature.paramSpellings[0];
                                        if (spelling.find('&') == std::string::npos) return true;
                                        bool constParameter = false;
                                        for (size_t pos = 0;
                                             (pos = spelling.find("const", pos)) != std::string::npos; )
                                        {
                                            const size_t end = pos + 5;
                                            const bool leftBoundary = pos == 0
                                                || std::isspace(static_cast<unsigned char>(spelling[pos - 1]));
                                            const bool rightBoundary = end == spelling.size()
                                                || std::isspace(static_cast<unsigned char>(spelling[end]));
                                            if (leftBoundary && rightBoundary)
                                            {
                                                constParameter = true;
                                                break;
                                            }
                                            pos = end;
                                        }
                                        return constParameter
                                            && (spelling.find("&&") == std::string::npos
                                                || namedVar.IsRvalue);
                                    }
                            const auto& parameter = candidate.Parameters[0];
                            if (parameter.IsRvalueRef)
                                return parameter.CxxConstRef() && namedVar.IsRvalue;
                            return !parameter.IsAlias || parameter.CxxConstRef();
                        };
                        const std::string lookup = source == operatorName
                            ? "__cxx_free." + operatorName : source;
                        if (auto it = compiler->functionTable.find(lookup);
                            it != compiler->functionTable.end())
                            for (const auto& candidate : it->second)
                                if (candidate.IsCxx && !candidate.IsMethod
                                    && candidate.SourceName == source
                                    && candidate.Parameters.size() == 2
                                    && freeCandidateBindsConstReceiver(candidate)
                                    && (candidate.Parameters[0].TypeName
                                            == namedVar.TypeAndValue.TypeName
                                        || compiler->IsCxxBaseOf(
                                            candidate.Parameters[0].TypeName,
                                            namedVar.TypeAndValue.TypeName)))
                                {
                                    constCxxCompoundCandidate = true;
                                    constCxxCompoundFreeCandidate = true;
                                    break;
                                }
                        if (constCxxCompoundCandidate) break;
                    }
                }
            }
            // A struct with a const field has no assignment (C++ deletes it): only construction
            // sets it. Init, copy/move construction and by-value passing stay legal.
            if (!constCxxCompoundCandidate
                && !RefuseConstWrite(unaryCtx, namedVar, destination,
                                     operatorText == "=" ? "assign to" : "modify", "assign to")
                && operatorText == "=" && !namedVar.TypeAndValue.Pointer
                && !namedVar.TypeAndValue.IsArrayView)
                if (std::string constField;
                    compiler->FindNativeConstField(namedVar.TypeAndValue.TypeName, constField))
                {
                    LLVMBackend::TypeAndValue wholeType;
                    wholeType.TypeName = namedVar.TypeAndValue.TypeName;
                    LogErrorContext(unaryCtx, std::format("cannot assign a whole '{}': field '{}' is const",
                        SpellDiagnosticType(*compiler, wholeType), constField));
                }

            // `value = default` default-constructs a C++ temporary even when this class does not
            // enter the nontrivial special-member assignment path below.
            auto* assignedExpr = ctx->assignmentExpression();
            if (operatorText == "=" && assignedExpr != nullptr
                && IsDefaultOnlyExpression(assignedExpr, false)
                && !namedVar.TypeAndValue.Pointer
                && compiler->IsCxxRecord(namedVar.TypeAndValue.TypeName))
            {
                const auto& typeName = namedVar.TypeAndValue.TypeName;
                std::string defaultCtorError;
                compiler->TryBindCxxImplicitDefaultCtor(typeName, defaultCtorError);
                if (!defaultCtorError.empty()) LogErrorContext(ctx, defaultCtorError);
                if (compiler->FindCxxDefaultCtor(typeName) == nullptr)
                {
                    std::string ambiguityError;
                    if (compiler->CxxDefaultCtorAmbiguityError(typeName, ambiguityError))
                        LogErrorContext(ctx, ambiguityError);
                    LogErrorContext(ctx, std::format("C++ class '{}' has no default constructor cflat can call",
                        compiler->DisplayCxxClassName(typeName)));
                }
            }

            // Recover an sret-backed C++ operator value as a prvalue for the next expression.
            auto cxxPrvalueAssignResult = [&](llvm::Value* opTemp) -> llvm::Value* {
                auto* slot = llvm::dyn_cast_or_null<llvm::AllocaInst>(opTemp);
                if (assignmentResult == nullptr || slot == nullptr
                    || namedVar.TypeAndValue.TypeName.empty()
                    || compiler->lastCallReturnType.IsAlias)
                    return nullptr;
                LLVMBackend::TypeAndValue classType;
                classType.TypeName = namedVar.TypeAndValue.TypeName;
                if (slot->getAllocatedType() != compiler->GetType(classType)) return nullptr;
                llvm::Value* loaded = compiler->CreateLoad(slot->getAllocatedType(), slot);
                *assignmentResult = {};
                assignmentResult->TypeAndValue = compiler->lastCallReturnType;
                if (assignmentResult->TypeAndValue.TypeName.empty())
                    assignmentResult->TypeAndValue = namedVar.TypeAndValue;
                assignmentResult->TypeAndValue.VariableName.clear();
                assignmentResult->TypeAndValue.IsMove = false;
                assignmentResult->TypeAndValue.IsAlias = false;
                assignmentResult->Primary = loaded;
                assignmentResult->BaseType = loaded->getType();
                assignmentResult->Storage = slot;
                assignmentResult->IsRvalue = true;
                compiler->lastCxxRetTemp_ = slot;
                compiler->lastCxxRetValue_ = loaded;
                return loaded;
            };

            // An imported C++ call may already have an sret slot for its prvalue. Route every
            // such assignment before the ordinary lvalue and special-member assignment paths.
            const std::string prvalueTypeName = namedVar.TypeAndValue.TypeName;
            const bool generatedPrvalueRecord =
                compiler->generatedCxxRecords_.count(prvalueTypeName) != 0;
            if (namedVar.IsRvalue && !namedVar.TypeAndValue.Pointer
                && (compiler->IsCxxRecord(prvalueTypeName) || generatedPrvalueRecord))
            {
                const auto methodName = operatorText == "=" ? std::string("operator=")
                    : operatorText.ends_with("=") ? "operator" + operatorText : std::string();
                bool userDeclared = false;
                if (!methodName.empty())
                {
                    if (compiler->IsCxxRecord(prvalueTypeName))
                        compiler->EnsureCxxMemberProjected(prvalueTypeName, methodName);
                    if (const auto* info = compiler->GetCxxClassInfo(prvalueTypeName))
                        userDeclared = std::any_of(info->directMethods.begin(),
                            info->directMethods.end(), [&](const auto& method) {
                                return method.raw.name == methodName && !method.raw.isImplicit
                                    && !method.raw.isDeleted && method.raw.bindRefusal.empty();
                            });
                    if (!userDeclared)
                        if (auto record = compiler->cxxRecordEntries_.find(prvalueTypeName);
                            record != compiler->cxxRecordEntries_.end())
                            userDeclared = std::any_of(record->second.members.begin(),
                                record->second.members.end(), [&](const auto& method) {
                                    return method.name == methodName && !method.isImplicit
                                        && !method.isDeleted && method.bindRefusal.empty();
                                });
                    if (!userDeclared && generatedPrvalueRecord)
                        userDeclared = HasOperatorOverloadForFirstParam(
                            methodName, prvalueTypeName);
                    if (!userDeclared && compiler->IsCxxRecord(prvalueTypeName))
                        userDeclared = compiler->HasCxxFunctionTemplateMember(
                            prvalueTypeName, methodName);
                }
                const bool cxxCompound = operatorText != "="
                    && compiler->IsCxxRecord(prvalueTypeName);
                const bool movedCxxCompoundSource = cxxCompound && namedVar.IsExplicitMove
                    && namedVar.Storage != nullptr;
                if (methodName.empty() || (!userDeclared && !cxxCompound))
                    LogErrorContext(unaryCtx,
                        "Left side of assignment is not an addressable lvalue.");

                if (movedCxxCompoundSource)
                    destination = namedVar.Storage;
                else if (destination == nullptr)
                {
                    destination = compiler->CreateAlloca(namedVar.BaseType);
                    compiler->CreateAssignment(namedVar.Primary, destination);
                    bool hasTrivialCxxDestructor = false;
                    if (compiler->IsCxxRecord(prvalueTypeName))
                        if (auto record = compiler->cxxRecordEntries_.find(prvalueTypeName);
                            record != compiler->cxxRecordEntries_.end())
                            hasTrivialCxxDestructor = record->second.hasTrivialDtor;
                    if (!hasTrivialCxxDestructor)
                        compiler->RegisterOwnedStructTemp(destination, prvalueTypeName);
                }
                auto rightNV = [&] {
                    CxxCallArgumentScope cxxAssignRhsScope(inCxxCallArgument_,
                        !namedVar.TypeAndValue.Pointer && compiler->IsCxxRecord(namedVar.TypeAndValue.TypeName));
                    return ParseAssignmentExpressionNamed(assignCtx);
                }();
                if (RefuseImmovableAssignSource(compiler, ctx, operatorText, rightNV)) return nullptr;
                // An overloaded assignment expression returns the C++ operator's declared
                // result. When that is T&, the ABI value is a pointer to the proxy object;
                // expose it as an lvalue T so a chained assignment can bind operator=(const T&).
                auto operatorRightNV = rightNV;
                llvm::Value* assignmentRef = rightNV.Primary;
                if (assignmentRef == nullptr && rightNV.Storage != nullptr
                    && rightNV.BaseType != nullptr && rightNV.BaseType->isPointerTy())
                    assignmentRef = compiler->builder->CreateLoad(
                        rightNV.BaseType, rightNV.Storage);
                if (assignCtx->assignmentOperator() != nullptr
                    && (rightNV.TypeAndValue.TypeName.empty()
                        || rightNV.TypeAndValue.TypeName == "ptr")
                    && assignmentRef != nullptr && assignmentRef->getType()->isPointerTy())
                {
                    // The nested assignment visitor can erase the C++ reference's pointee
                    // metadata to the opaque `ptr` spelling. Its result is still the declared
                    // proxy type of this assignment expression's left-hand operand.
                    if (operatorRightNV.TypeAndValue.TypeName.empty()
                        || operatorRightNV.TypeAndValue.TypeName == "ptr")
                    {
                        operatorRightNV.TypeAndValue = {};
                        operatorRightNV.TypeAndValue.TypeName = prvalueTypeName;
                    }
                    operatorRightNV.TypeAndValue.Pointer = false;
                    operatorRightNV.TypeAndValue.PointerDepth = 0;
                    operatorRightNV.TypeAndValue.ElemPointer = false;
                    operatorRightNV.IsRvalue = false;
                    operatorRightNV.Storage = assignmentRef;
                    operatorRightNV.BaseType = compiler->GetType(operatorRightNV.TypeAndValue);
                    operatorRightNV.Primary = compiler->builder->CreateLoad(
                        operatorRightNV.BaseType, operatorRightNV.Storage);
                }
                auto* right = LoadNamedVariable(operatorRightNV);
                auto* left = compiler->CreateLoad(namedVar.BaseType, destination);
                // The operand's proven depth keeps a `&x` pointee for C++ matching (T85).
                int rhsDepth = 0;
                bool rhsElemPointer = false;
                OperandPointerDepth(operatorRightNV, right, rhsDepth, rhsElemPointer);
                auto invokePrvalueOperator = [&]() -> llvm::Value* {
                    const std::string lookupName = operatorText == "=" ? "operator=" : methodName;
                    if (compiler->IsCxxRecord(prvalueTypeName)
                        && compiler->HasCxxFunctionTemplateMember(prvalueTypeName, lookupName))
                    {
                        LLVMBackend::NamedVariable receiver;
                        receiver.Primary = destination;
                        receiver.Storage = destination;
                        receiver.BaseType = destination->getType();
                        receiver.TypeAndValue.TypeName = prvalueTypeName;
                        receiver.TypeAndValue.Pointer = true;
                        std::vector<LLVMBackend::NamedVariable> templateArguments;
                        templateArguments.push_back(std::move(receiver));
                        templateArguments.push_back(rightNV);
                        std::string registeredName, templateError;
                        compiler->RequestCxxFunctionTemplate(
                            lookupName, prvalueTypeName, {}, templateArguments, {},
                            registeredName, templateError);
                    }
                    auto candidates = compiler->functionTable.find(lookupName);
                    if (operatorText != "=" || !compiler->IsCxxRecord(prvalueTypeName)
                        || candidates == compiler->functionTable.end())
                        return TryBinaryOperatorOverload(
                            left, operatorText, right, ctx, namedVar.BaseType,
                            rhsDepth, rhsElemPointer, destination,
                            operatorRightNV.Storage, false, false, namedVar.IsRvalue,
                            operatorRightNV.IsRvalue, prvalueTypeName,
                            operatorRightNV.TypeAndValue.TypeName,
                            0, false,
                            namedVar.CallerName.empty() ? namedVar.TypeAndValue.VariableName
                                : namedVar.CallerName,
                            namedVar.FieldName, namedVar.IsElementAccess,
                            {}, {}, false, namedVar.ConditionalMoveSources, {},
                            operatorRightNV.TypeAndValue.PointeeConstBit()
                                && operatorRightNV.TypeAndValue.Pointer, {}, operatorRightNV.CxxArithIdentity,
                            operatorRightNV.IsExplicitMove, namedVar.IsExplicitMove,
                            namedVar.TypeAndValue.IsCxxNullptrT,
                            operatorRightNV.TypeAndValue.IsCxxNullptrT,
                            !movedCxxCompoundSource, false,
                            compiler->CxxOperandObjectCv(namedVar),
                            compiler->CxxOperandObjectCv(operatorRightNV), false, false,
                            &namedVar);

                    std::set<std::string> declaredLinkages;
                    if (auto record = compiler->cxxRecordEntries_.find(prvalueTypeName);
                        record != compiler->cxxRecordEntries_.end())
                        for (const auto& member : record->second.members)
                            if (member.name == "operator=" && !member.isImplicit
                                && !member.isDeleted && member.bindRefusal.empty())
                                declaredLinkages.insert(member.linkageName);
                    auto original = candidates->second;
                    candidates->second.erase(std::remove_if(candidates->second.begin(),
                        candidates->second.end(), [&](const auto& candidate) {
                            return candidate.IsCxx && !candidate.Parameters.empty()
                                && candidate.Parameters[0].TypeName == prvalueTypeName
                                && declaredLinkages.count(candidate.UniqueName) == 0
                                && !candidate.UniqueName.starts_with("__cflat_tpl_");
                        }), candidates->second.end());
                    // Restored by key: the overload call may insert and rehash the table.
                    auto restore = [&]() {
                        compiler->functionTable[lookupName] = std::move(original);
                    };
                    try
                    {
                        auto* result = TryBinaryOperatorOverload(
                            left, operatorText, right, ctx, namedVar.BaseType,
                            rhsDepth, rhsElemPointer, destination,
                            operatorRightNV.Storage, false, false, false,
                            operatorRightNV.IsRvalue, prvalueTypeName,
                            operatorRightNV.TypeAndValue.TypeName,
                            0, false, {}, {}, false, {}, {}, false, {}, {},
                            operatorRightNV.TypeAndValue.PointeeConstBit()
                                && operatorRightNV.TypeAndValue.Pointer, {}, operatorRightNV.CxxArithIdentity,
                            operatorRightNV.IsExplicitMove, false,
                            namedVar.TypeAndValue.IsCxxNullptrT,
                            operatorRightNV.TypeAndValue.IsCxxNullptrT, true, false,
                            compiler->CxxOperandObjectCv(namedVar),
                            compiler->CxxOperandObjectCv(operatorRightNV));
                        restore();
                        return result;
                    }
                    catch (...)
                    {
                        restore();
                        throw;
                    }
                };
                // The by-value result constructs straight into the enclosing declarator's slot when
                // one is armed (the lvalue compound path already does this through EmitOperatorCall).
                if (auto* result = CxxAssignSretScope::EmitOperatorCall(compiler, invokePrvalueOperator))
                {
                    const auto& opResultType = compiler->lastCallReturnType;
                    if (assignmentResult != nullptr && result->getType()->isPointerTy()
                        && opResultType.IsAlias && !opResultType.IsRvalueRef
                        && !opResultType.Pointer && !opResultType.TypeName.empty()
                        && compiler->IsCxxRecord(opResultType.TypeName))
                    {
                        auto valueType = opResultType;
                        valueType.Pointer = false;
                        valueType.PointerDepth = 0;
                        valueType.ElemPointer = false;
                        auto* llvmValueType = compiler->GetType(valueType);
                        if (llvmValueType != nullptr && llvmValueType->isStructTy())
                        {
                            llvm::Value* resultValue = compiler->CreateLoad(llvmValueType, result);
                            *assignmentResult = {};
                            assignmentResult->TypeAndValue = opResultType;
                            assignmentResult->TypeAndValue.VariableName.clear();
                            assignmentResult->Primary = result;
                            PrepareAliasCallResult(unaryCtx, *assignmentResult, false);
                            return resultValue;
                        }
                    }
                    if (auto* prvalue = cxxPrvalueAssignResult(
                            compiler->lastCxxRetTemp_))
                        return prvalue;
                    if (assignmentResult != nullptr && !opResultType.IsAlias
                        && result->getType()->isStructTy()
                        && compiler->IsCxxRecord(prvalueTypeName))
                    {
                        *assignmentResult = {};
                        assignmentResult->TypeAndValue = opResultType;
                        assignmentResult->TypeAndValue.VariableName.clear();
                        assignmentResult->TypeAndValue.IsAlias = false;
                        assignmentResult->Primary = result;
                        assignmentResult->BaseType = result->getType();
                        assignmentResult->IsRvalue = true;
                        return result;
                    }
                    return result;
                }
                LogErrorContext(unaryCtx,
                    "Left side of assignment is not an addressable lvalue.");
            }

            /*
             * A by-value (`T`) operator result the enclosing slot did not take is the prvalue
             * instead: its temp (already on the owned-temp ledger, destroyed at the end of the full
             * expression) is exposed as the outermost C++ return temporary, so a chained
             * `c = (a = 3)` move-assigns from it and a declarator / return moves from it.
             */
            /*
             * M4b - assignment to a foreign NONTRIVIAL C++ object runs the C++ assignment
             * operator. The generic path would store bytes over a live object, which for a class
             * with a user copy/move assignment (or a destructor that owns something) corrupts it.
             * `y = move x` prefers move assignment and consumes x; the moved-from object is still
             * destroyed at scope exit (M4b ruling 3).
             */
            // Generic class assignment paths yield the destination when used as a value.
            auto classAssignValue = [&]() -> llvm::Value* {
                if (assignmentResult == nullptr || destination == nullptr) return nullptr;
                compiler->lastCxxRetTemp_ = nullptr;
                compiler->lastCxxRetValue_ = nullptr;
                llvm::Value* loaded = namedVar.BaseType != nullptr
                    ? compiler->CreateLoad(namedVar.BaseType, destination)
                    : compiler->CreateLoad(destination);
                *assignmentResult = {};
                assignmentResult->TypeAndValue = namedVar.TypeAndValue;
                assignmentResult->TypeAndValue.VariableName.clear();
                assignmentResult->TypeAndValue.IsMove = false;
                assignmentResult->Primary = loaded;
                assignmentResult->BaseType = loaded->getType();
                assignmentResult->Storage = destination;
                assignmentResult->CallerName = namedVar.CallerName;
                assignmentResult->FieldName = namedVar.FieldName;
                assignmentResult->MovePathKey = namedVar.MovePathKey;
                assignmentResult->MovePathHasDynamicIndex = namedVar.MovePathHasDynamicIndex;
                return loaded;
            };
            const auto* assignmentClassInfo =
                compiler->GetCxxClassInfo(namedVar.TypeAndValue.TypeName);
            const bool convertingCtorCandidate = assignmentClassInfo != nullptr
                && std::any_of(assignmentClassInfo->constructors.begin(),
                    assignmentClassInfo->constructors.end(), [](const auto& ctor) {
                        return ctor.params.size() > 1 && !ctor.isCopyCtor && !ctor.isMoveCtor;
                    });
            const bool hasUserDeclaredAssignment = assignmentClassInfo != nullptr
                && std::any_of(assignmentClassInfo->directMethods.begin(),
                    assignmentClassInfo->directMethods.end(), [](const auto& method) {
                        return method.raw.name == "operator=" && !method.raw.isImplicit;
                    });
            const bool templateCtorCandidate = operatorText == "=" && assignCtx != nullptr
                && assignmentClassInfo != nullptr
                && (assignmentClassInfo->hasCtorTemplate
                    || (convertingCtorCandidate && !hasUserDeclaredAssignment
                        && !assignmentClassInfo->hasAssignTemplate));
            if (operatorText == "=" && destination != nullptr
                && !namedVar.TypeAndValue.Pointer
                && (compiler->IsForeignNontrivialCxxClass(namedVar.TypeAndValue.TypeName)
                    || templateCtorCandidate))
            {
                {
                    const std::string tn = namedVar.TypeAndValue.TypeName;
                    // `sf = (int x) => ...` on a std.function: its signature types the lambda.
                    LambdaExpectedTypeRestoreGuard stdFunctionLambdaRestore(&lambdaExpectedType);
                    if (const auto* signature = compiler->StdFunctionClosureSignature(tn))
                        lambdaExpectedType = *signature;
                    auto* mv = TopLevelMoveExpression(assignCtx);
                    const bool useMove = mv != nullptr;
                    auto* srcCtx = useMove ? (antlr4::ParserRuleContext*)mv->unaryExpression()
                                           : (antlr4::ParserRuleContext*)assignCtx;
                    const std::string srcName = srcCtx != nullptr ? srcCtx->getText() : std::string();
                    auto* rhsUnary = assignCtx != nullptr ? tryGetUnaryExpression(assignCtx) : nullptr;
                    auto* rhsPostfix = rhsUnary != nullptr ? rhsUnary->postfixExpression() : nullptr;
                    const bool rhsTemporary = rhsPostfix != nullptr && !rhsPostfix->LeftParen().empty();
                    const bool rhsConstructor = assignCtx != nullptr
                        && ForeignCxxConstructArgs(assignCtx, tn) != nullptr;
                    const bool rhsDefault = IsDefaultOnlyExpression(assignCtx, false)
                        || IsSameTypeCastOfDefault(assignCtx, tn);
                    // Created on demand by whichever of release/assignment is compiled first, so an
                    // assignment may precede the releasing function in source order.
                    auto* destinationGlobalLiveFlag = compiler->EnsureGlobalCxxLiveFlag(namedVar);
                    const bool destinationReleased = namedVar.ExplicitlyMovedNull
                        && namedVar.ConditionalDropFlag == nullptr;
                    const bool destinationConditional = namedVar.ConditionalDropFlag != nullptr;
                    const bool destinationGlobalConditional = destinationGlobalLiveFlag != nullptr;
                    compiler->EnsureCxxMemberProjected(tn, "operator=");
                    const auto* info = compiler->GetCxxClassInfo(tn);
                    // wantConstRhs: -1 = no preference; 0/1 = the lvalue source is non-const/const.
                    // clang binds a const source only to a const reference and prefers the
                    // non-const reference for a non-const source.
                    auto lvalueReceiverAssignment = [](const auto& overloads, const auto& fallback,
                                                        int wantConstRhs = -1)
                        -> const LLVMBackend::CxxClassInfo::Structor* {
                        auto viable = [](const auto& c) {
                            return c.refQualifier == cflat_cinterop::CxxRefQualifierNone
                                || c.refQualifier == cflat_cinterop::CxxRefQualifierLValue;
                        };
                        if (wantConstRhs >= 0)
                            for (const auto& candidate : overloads)
                                if (viable(candidate) && candidate.rhsConstRef == (wantConstRhs == 1))
                                    return &candidate;
                        for (const auto& candidate : overloads)
                            if (viable(candidate))
                                return &candidate;
                        if (overloads.empty()
                            && (fallback.refQualifier == cflat_cinterop::CxxRefQualifierNone
                                || fallback.refQualifier == cflat_cinterop::CxxRefQualifierLValue))
                            return &fallback;
                        return nullptr;
                    };

                    auto ownedTempMark = compiler->MarkOwnedTemps();
                    compiler->lastCxxRetTemp_ = nullptr;
                    compiler->lastCxxRetValue_ = nullptr;
                    LLVMBackend::TypeAndValue tempType;
                    tempType.TypeName = tn;
                    llvm::Value* forcedTemp = nullptr;
                    bool consumed = false;
                    LLVMBackend::NamedVariable rhsNV;
                    LLVMBackend::NamedVariable moveSourceNV;
                    if (rhsDefault)
                    {
                        std::string defaultError;
                        compiler->TryBindCxxImplicitDefaultCtor(tn, defaultError);
                        if (!defaultError.empty()) LogErrorContext(ctx, defaultError);
                        const auto* ctor = compiler->FindCxxDefaultCtor(tn);
                        if (ctor == nullptr)
                        {
                            std::string ambiguityError;
                            if (compiler->CxxDefaultCtorAmbiguityError(tn, ambiguityError))
                                LogErrorContext(ctx, ambiguityError);
                            LogErrorContext(ctx, std::format(
                                "C++ class '{}' has no default constructor cflat can call",
                                compiler->DisplayCxxClassName(tn)));
                        }
                        forcedTemp = compiler->AllocaAtEntry(
                            compiler->GetType(tempType), nullptr, "cxx.assign.temp",
                            namedVar.TypeAndValue.AllocAlignValue);
                        compiler->EmitCxxStructorCall(tn, *ctor, forcedTemp, {});
                        compiler->RegisterOwnedStructTemp(forcedTemp, tn);
                    }
                    else
                    {
                        const bool armReturnTemp = rhsTemporary || rhsConstructor;
                        // One explicit move arm makes a C++ prvalue: each arm builds a temp, then move-assign.
                        // Both arms moved (xvalue) or neither (copy-assign) keep the direct path.
                        auto* assignTernary = SoleTernaryExpression(assignCtx);
                        const bool ternaryAssignTemp = !armReturnTemp && assignTernary != nullptr
                            && assignTernary->expression() != nullptr
                            && assignTernary->conditionalExpression() != nullptr
                            && (TopLevelMoveExpression(assignTernary->expression()->assignmentExpression())
                                != nullptr)
                                != (TopLevelMoveExpression(assignTernary->conditionalExpression())
                                    != nullptr);
                        if (armReturnTemp || ternaryAssignTemp)
                        {
                            forcedTemp = compiler->AllocaAtEntry(
                                compiler->GetType(tempType), nullptr, "cxx.assign.temp",
                                namedVar.TypeAndValue.AllocAlignValue);
                        }
                        if (armReturnTemp)
                        {
                            compiler->pendingCxxSretDest_ = forcedTemp;
                            compiler->pendingCxxSretTypeName_ = tn;
                        }
                        if (ternaryAssignTemp)
                        {
                            compiler->pendingCxxTernaryDeclDest_ = forcedTemp;
                            compiler->pendingCxxTernaryDeclTypeName_ = tn;
                            compiler->pendingCxxTernaryDeclConsumed_ = false;
                            compiler->pendingCxxTernaryDeclFailed_ = false;
                        }
                        rhsNV = [&] {
                            CxxCallArgumentScope cxxAssignRhsScope(inCxxCallArgument_,
                                !namedVar.TypeAndValue.Pointer && compiler->IsCxxRecord(namedVar.TypeAndValue.TypeName));
                            return ParseAssignmentExpressionNamed(assignCtx);
                        }();
                        compiler->LowerNonCapturingClosureForCxx(
                            rhsNV, compiler->StdFunctionClosureSignature(tn));
                        if (std::string refusal = compiler->CxxClosureArgumentRefusal(
                                rhsNV, compiler->DisplayCxxClassName(tn),
                                compiler->StdFunctionClosureSignature(tn));
                            !refusal.empty())
                            LogErrorContext(assignCtx, refusal);
                        consumed = armReturnTemp && compiler->pendingCxxSretDest_ == nullptr;
                        compiler->pendingCxxSretDest_ = nullptr;
                        compiler->pendingCxxSretTypeName_.clear();
                        if (ternaryAssignTemp)
                        {
                            consumed = compiler->pendingCxxTernaryDeclConsumed_
                                && !compiler->pendingCxxTernaryDeclFailed_;
                            compiler->pendingCxxTernaryDeclDest_ = nullptr;
                            compiler->pendingCxxTernaryDeclTypeName_.clear();
                            compiler->pendingCxxTernaryDeclConsumed_ = false;
                            compiler->pendingCxxTernaryDeclFailed_ = false;
                        }
                        if ((armReturnTemp || ternaryAssignTemp) && !consumed && rhsNV.Storage != forcedTemp)
                            forcedTemp = nullptr;
                    }
                    if (useMove) moveSourceNV = rhsNV;
                    auto markMoveSource = [&]() {
                        if (moveSourceNV.Storage == nullptr) return;
                        const bool fieldPath = !moveSourceNV.FieldName.empty()
                            || !moveSourceNV.OwningStructName.empty()
                            || moveSourceNV.IsElementAccess
                            || !moveSourceNV.MovePathKey.empty();
                        std::string root = moveSourceNV.CallerName;
                        if (fieldPath)
                        {
                            if (root.empty()) root = FieldPathRootName(moveSourceNV);
                            const std::string movePath = LLVMBackend::FieldMoveKey(moveSourceNV);
                            if (!root.empty() && !movePath.empty())
                                compiler->MarkVariableFieldMoved(root, movePath);
                            return;
                        }
                        // Whole-variable source only when its storage IS the variable: `move *p` moves the
                        // pointee, and `p` stays usable as in C++.
                        const auto* binding = compiler->FindVariableByStorage(moveSourceNV.Storage);
                        if (binding != nullptr && !binding->TypeAndValue.VariableName.empty())
                            compiler->MarkVariableMoved(binding->TypeAndValue.VariableName);
                    };

                    if (!rhsDefault && !rhsNV.TypeAndValue.Pointer
                        && compiler->IsCxxRecord(rhsNV.TypeAndValue.TypeName)
                        && compiler->HasImplicitClassConversionOperator(rhsNV.TypeAndValue.TypeName)
                        && rhsNV.TypeAndValue.TypeName != tn)
                    {
                        forcedTemp = compiler->AllocaAtEntry(
                            compiler->GetType(tempType), nullptr, "cxx.assign.converted");
                        std::string refusal;
                        const auto verdict = compiler->ConvertThroughCxxConvertingCtor(
                            rhsNV, tn, forcedTemp, refusal, /*directInit*/ false);
                        if (verdict == LLVMBackend::CxxConvertingCtorResult::Refused)
                            LogErrorContext(assignCtx, refusal);
                        if (verdict == LLVMBackend::CxxConvertingCtorResult::Converted)
                        {
                            rhsNV.Primary = nullptr;
                            rhsNV.Storage = forcedTemp;
                            rhsNV.BaseType = compiler->GetType(tempType);
                            rhsNV.TypeAndValue.TypeName = tn;
                            rhsNV.IsRvalue = true;
                        }
                        else
                            forcedTemp = nullptr;
                    }

                    auto* returnedTemp = compiler->lastCxxRetTemp_;
                    auto* returnedValue = compiler->lastCxxRetValue_;
                    // A returned temp of ANOTHER class (Eigen's `a + b` expression template) is a
                    // conversion source, never the T to move-assign from.
                    auto* returnedSlot = llvm::dyn_cast_or_null<llvm::AllocaInst>(returnedTemp);
                    const bool returnedHoldsTarget = returnedSlot == nullptr
                        || returnedSlot->getAllocatedType() == compiler->GetType(tempType);
                    const bool outermostTemp = returnedTemp != nullptr && returnedHoldsTarget
                        && ((rhsNV.Primary != nullptr && rhsNV.Primary == returnedValue)
                            || rhsNV.Storage == returnedTemp);
                    const bool forcedTempProduced = forcedTemp != nullptr
                        && (consumed || rhsNV.Storage == forcedTemp);
                    const bool unmarkedCxxTemp = !forcedTempProduced && !rhsDefault
                        && !outermostTemp && rhsNV.IsRvalue && rhsNV.Storage != nullptr
                        && !rhsNV.TypeAndValue.Pointer
                        && rhsNV.TypeAndValue.TypeName == tn;
                    llvm::Value* rhsTemp = forcedTempProduced || rhsDefault
                        ? forcedTemp : (outermostTemp ? returnedTemp
                                                      : (unmarkedCxxTemp ? rhsNV.Storage : nullptr));
                    // A '?:' of C++ temporaries joins the arms' SLOTS, so its storage looks like an
                    // lvalue; every incoming slot still pending destruction proves it a temporary.
                    std::function<bool(llvm::Value*, int)> pendingTempSlot =
                        [&](llvm::Value* slot, int depth) -> bool {
                        if (slot == nullptr || depth > 16) return false;
                        if (auto* phi = llvm::dyn_cast<llvm::PHINode>(slot))
                        {
                            if (phi->getNumIncomingValues() == 0) return false;
                            for (llvm::Value* incoming : phi->incoming_values())
                                if (!pendingTempSlot(incoming, depth + 1)) return false;
                            return true;
                        }
                        LLVMBackend::NamedVariable probe;
                        probe.Storage = slot;
                        return compiler->IsOwnedTempValue(probe);
                    };
                    if (rhsTemp == nullptr && !useMove && !rhsDefault
                        && rhsNV.Storage != nullptr && !rhsNV.TypeAndValue.Pointer
                        && rhsNV.TypeAndValue.TypeName == tn
                        && llvm::isa<llvm::PHINode>(rhsNV.Storage)
                        && pendingTempSlot(rhsNV.Storage, 0))
                        rhsTemp = rhsNV.Storage;
                    if (forcedTempProduced && !rhsDefault)
                        compiler->RegisterOwnedStructTemp(forcedTemp, tn);
                    compiler->lastCxxRetTemp_ = nullptr;
                    compiler->lastCxxRetValue_ = nullptr;

                    llvm::Value* sourceStorage = rhsNV.Storage;
                    // R5: `p = new T();` / `p = move raw;` into a `unique T*` (std::unique_ptr<T>)
                    // adopts into a temp that is then move-assigned (the old pointee is released).
                    bool adoptedRaw = false;
                    if (rhsTemp == nullptr && !rhsDefault
                        && compiler->IsRawPointerForCxxUniquePtr(tn, rhsNV))
                    {
                        LLVMBackend::NamedVariable adoptSource = rhsNV;
                        if (useMove)
                        {
                            adoptSource.IsExplicitMove = true;
                            if (adoptSource.CallerName.empty())
                                if (const auto* binding = compiler->FindVariableByStorage(adoptSource.Storage))
                                    adoptSource.CallerName = binding->TypeAndValue.VariableName;
                        }
                        auto* adoptTemp = compiler->AllocaAtEntry(
                            compiler->GetType(tempType), nullptr, "cxx.adopt.temp");
                        if (compiler->TryAdoptRawPointerIntoCxxUniquePtr(
                                tn, adoptTemp, adoptSource, "an assigned destination",
                                &namedVar.TypeAndValue))
                        {
                            compiler->RegisterOwnedStructTemp(adoptTemp, tn);
                            rhsTemp = adoptTemp;
                            adoptedRaw = true;
                        }
                    }
                    // A `T&&` result (std.move(x)) is an xvalue: move-assign from the referent,
                    // which is neither a temporary to destroy here nor a CFlat variable to consume.
                    // `z = k ? move a : move b;` joins two xvalues: clang move-assigns the selected arm's
                    // object directly (no temp), so the join's storage is that object (T288).
                    auto* assignTernaryArms = SoleTernaryExpression(assignCtx);
                    // Narrow gate (T288 r3): only a bare name of a non-const, non-pointer local or
                    // parameter counts. Fields, derefs, calls, indexes and const names keep master's path.
                    auto plainMutableMoveArm = [&](antlr4::tree::ParseTree* arm) {
                        auto* mv = TopLevelMoveExpression(arm);
                        if (mv == nullptr || mv->unaryExpression() == nullptr) return false;
                        const std::string name = mv->unaryExpression()->getText();
                        if (!IsBareIdentifierText(name)) return false;
                        auto* nv = compiler->FindLiveNamedVariable(name);
                        return nv != nullptr && !nv->TypeAndValue.Pointer
                            && !nv->TypeAndValue.CxxConstRef()
                            && compiler->CxxConstReceiverKind(*nv) == 0;
                    };
                    const bool bothArmsMovedJoin = !useMove && assignTernaryArms != nullptr
                        && assignTernaryArms->expression() != nullptr
                        && assignTernaryArms->conditionalExpression() != nullptr
                        && plainMutableMoveArm(assignTernaryArms->expression()->assignmentExpression())
                        && plainMutableMoveArm(assignTernaryArms->conditionalExpression());
                    const bool movedJoinSource = bothArmsMovedJoin && !adoptedRaw && rhsTemp == nullptr
                        && !rhsDefault && rhsNV.Storage != nullptr && !rhsNV.TypeAndValue.Pointer
                        && rhsNV.TypeAndValue.TypeName == tn;
                    const bool xvalueSource = !useMove && !adoptedRaw && rhsTemp == nullptr
                        && !rhsDefault && (compiler->IsCxxRvalueRefClassResult(rhsNV) || movedJoinSource)
                        && rhsNV.TypeAndValue.TypeName == tn;
                    if (xvalueSource) sourceStorage = movedJoinSource ? rhsNV.Storage : rhsNV.Primary;
                    std::string conversionRefusal;
                    /*
                     * A trivially copyable class admitted here only for its converting-ctor
                     * template: a same-class source is the plain trivial copy it always was, never
                     * a conversion or an operator call. Nontrivial classes keep the paths below.
                     */
                    if (!compiler->IsForeignNontrivialCxxClass(tn) && !rhsDefault && !adoptedRaw)
                    {
                        auto* classType = compiler->GetType(tempType);
                        llvm::Value* trivialCopy = nullptr;
                        if (rhsNV.Primary != nullptr && rhsNV.Primary->getType() == classType)
                            trivialCopy = rhsNV.Primary;
                        else if (!rhsNV.TypeAndValue.Pointer && rhsNV.Storage != nullptr
                            && compiler->ResolveTypeAlias(rhsNV.TypeAndValue.TypeName)
                                == compiler->ResolveTypeAlias(tn))
                            trivialCopy = compiler->CreateLoad(classType, rhsNV.Storage);
                        if (trivialCopy != nullptr)
                        {
                            compiler->builder->CreateStore(trivialCopy, destination);
                            if (namedVar.FieldName.empty() && !namedVar.CallerName.empty())
                            {
                                compiler->MarkVariableUnmoved(namedVar.CallerName);
                                compiler->MarkVariableNotExplicitlyMovedNull(namedVar.CallerName);
                            }
                            return classAssignValue();
                        }
                    }
                    /*
                     * Plan converting-constructors.md point 2: `t = u;` with a non-T source. A
                     * declared `operator=(U)` wins; otherwise `T(u)` is built and assigned as a
                     * moved temporary, exactly as the spelled `t = T(u);`.
                     */
                    const bool convertibleSource = !useMove && !adoptedRaw && rhsTemp == nullptr
                        && !rhsDefault && !xvalueSource
                        && (rhsNV.TypeAndValue.Pointer || rhsNV.TypeAndValue.TypeName != tn);
                    if (convertibleSource && !destinationReleased && !destinationConditional)
                    {
                        if (destinationGlobalConditional)
                        {
                            LLVMBackend::TypeAndValue selectedReturn;
                            auto* available = TryDirectCxxAssignOperator(
                                ctx, namedVar, tn, rhsNV, conversionRefusal,
                                /*probeOnly*/ true, &selectedReturn);
                            if (available != nullptr)
                            {
                                /*
                                 * Value position yields the operator's own result, as in C++. A
                                 * by-value result is constructed (sret) into ONE slot both edges
                                 * share, owned as a full-expression temp at the join; a `T&`
                                 * result is joined by a phi. Statement position retires a
                                 * by-value result on its own edge. An enclosing declarator /
                                 * return slot takes the result directly instead.
                                */
                                const bool valueUse = assignmentResult != nullptr;
                                const auto returnKind = GetCxxAssignReturnKind(
                                    ctx, tn, selectedReturn);
                                if (valueUse && (returnKind == CxxAssignReturnKind::Void
                                    || returnKind == CxxAssignReturnKind::Unsupported))
                                {
                                    CxxAssignOperatorValue(ctx, namedVar, tn, available,
                                        selectedReturn, assignmentResult);
                                    return nullptr;
                                }
                                llvm::AllocaInst* resultSlot = nullptr;
                                if (valueUse && returnKind == CxxAssignReturnKind::ByValue
                                    && !CxxAssignSretScope::ArmedFor(compiler, tn))
                                {
                                    LLVMBackend::TypeAndValue classType;
                                    classType.TypeName = tn;
                                    resultSlot = compiler->AllocaAtEntry(
                                        compiler->GetType(classType), nullptr,
                                        "cxx.assign.global.result");
                                }
                                std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> refResults;
                                bool refResultMissing = false;
                                llvm::Value* edgeRef = nullptr;
                                auto emitDirect = [&](bool reinitialized) {
                                    edgeRef = nullptr;
                                    const auto tempMark = compiler->MarkOwnedTemps();
                                    llvm::Value* direct = nullptr;
                                    if (resultSlot != nullptr)
                                    {
                                        auto* savedDest = compiler->pendingCxxSretDest_;
                                        auto savedType = compiler->pendingCxxSretTypeName_;
                                        compiler->pendingCxxSretDest_ = resultSlot;
                                        compiler->pendingCxxSretTypeName_ = tn;
                                        {
                                            CxxAssignSretScope slotScope(compiler);
                                            direct = TryDirectCxxAssignOperator(
                                                ctx, namedVar, tn, rhsNV, conversionRefusal);
                                        }
                                        compiler->pendingCxxSretDest_ = savedDest;
                                        compiler->pendingCxxSretTypeName_ = std::move(savedType);
                                    }
                                    else
                                        direct = TryDirectCxxAssignOperator(
                                            ctx, namedVar, tn, rhsNV, conversionRefusal);
                                    if (direct == nullptr)
                                        LogErrorContext(ctx, std::format(
                                            "cannot assign to {} '{}' from this expression; {}",
                                            reinitialized ? "reinitialized C++ global"
                                                          : "C++ class",
                                            compiler->DisplayCxxClassName(tn),
                                            conversionRefusal.empty()
                                                ? "the selected assignment operator could not be emitted"
                                                : conversionRefusal));
                                    else if (direct != nullptr)
                                        CxxAssignOperatorValue(ctx, namedVar, tn, direct,
                                            selectedReturn, assignmentResult,
                                            resultSlot != nullptr ? resultSlot
                                                : CxxAssignSretScope::TargetFor(compiler, tn));
                                    if (valueUse && returnKind == CxxAssignReturnKind::Reference
                                        && direct != nullptr)
                                        edgeRef = direct;
                                    else
                                        refResultMissing = true;
                                    // Value results stay live across the join; statements retire
                                    // each edge's operator temporaries here.
                                    if (!valueUse || returnKind != CxxAssignReturnKind::ByValue)
                                        compiler->FlushOwnedTempsSince(tempMark, nullptr, nullptr);
                                    if (!valueUse || returnKind != CxxAssignReturnKind::ByValue)
                                    {
                                        compiler->lastCxxRetTemp_ = nullptr;
                                        compiler->lastCxxRetValue_ = nullptr;
                                    }
                                };
                                auto* liveBlock = compiler->CreateBasicBlock(
                                    "cxx.assign.global.direct.live");
                                auto* constructBlock = compiler->CreateBasicBlock(
                                    "cxx.assign.global.direct.construct");
                                auto* doneBlock = compiler->CreateBasicBlock(
                                    "cxx.assign.global.direct.done");
                                auto* live = compiler->builder->CreateLoad(
                                    compiler->builder->getInt1Ty(), destinationGlobalLiveFlag,
                                    "cxx.assign.global.direct.livef");
                                compiler->CreateConditionJump(live, liveBlock, constructBlock);
                                compiler->SwitchToBlock(liveBlock);
                                emitDirect(/*reinitialized*/ false);
                                compiler->builder->CreateStore(compiler->builder->getInt1(true),
                                                               destinationGlobalLiveFlag);
                                // Incoming blocks are taken at each edge's jump, after any split.
                                if (edgeRef != nullptr)
                                    refResults.emplace_back(edgeRef, compiler->builder->GetInsertBlock());
                                compiler->CreateJump(doneBlock);
                                compiler->SwitchToBlock(constructBlock);
                                edgeRef = nullptr;
                                std::string defaultError;
                                auto source = rhsNV;
                                const auto constructed = compiler->ConvertThroughCxxConvertingCtor(
                                    source, tn, destination, conversionRefusal);
                                bool unreachableEdge = false;
                                if (constructed == LLVMBackend::CxxConvertingCtorResult::Converted)
                                {
                                    if (resultSlot != nullptr)
                                    {
                                        unreachableEdge = !compiler->EmitCxxCopyOrMoveConstruct(
                                            tn, resultSlot, destination, false,
                                            "from a reinitialized C++ global");
                                    }
                                    else
                                        edgeRef = destination;
                                }
                                else
                                {
                                    compiler->TryBindCxxImplicitDefaultCtor(tn, defaultError);
                                    const auto* defaultCtor = compiler->FindCxxDefaultCtor(tn);
                                    if (defaultCtor == nullptr)
                                    {
                                        unreachableEdge = true;
                                    }
                                    else
                                    {
                                        compiler->EmitCxxStructorCall(tn, *defaultCtor, destination, {});
                                        emitDirect(/*reinitialized*/ true);
                                    }
                                }
                                if (unreachableEdge)
                                {
                                    // Unreachable today: such a global cannot be released.
                                    auto* trap = llvm::Intrinsic::getOrInsertDeclaration(
                                        compiler->module.get(), llvm::Intrinsic::trap, {});
                                    compiler->builder->CreateCall(trap);
                                    compiler->builder->CreateUnreachable();
                                }
                                else
                                {
                                    compiler->builder->CreateStore(compiler->builder->getInt1(true),
                                                                   destinationGlobalLiveFlag);
                                    if (edgeRef != nullptr)
                                        refResults.emplace_back(
                                            edgeRef, compiler->builder->GetInsertBlock());
                                    compiler->CreateJump(doneBlock);
                                }
                                compiler->SwitchToBlock(doneBlock);
                                if (resultSlot != nullptr)
                                {
                                    compiler->RegisterOwnedStructTemp(resultSlot, tn);
                                    return cxxPrvalueAssignResult(resultSlot);
                                }
                                if (!valueUse || refResultMissing || refResults.empty())
                                    return classAssignValue();
                                auto* joined = refResults.front().first;
                                if (refResults.size() > 1)
                                {
                                    auto* phi = compiler->builder->CreatePHI(
                                        joined->getType(), (unsigned)refResults.size(),
                                        "cxx.assign.global.ref");
                                    for (auto& [value, block] : refResults)
                                        phi->addIncoming(value, block);
                                    joined = phi;
                                }
                                llvm::Value* loaded = compiler->CreateLoad(namedVar.BaseType, joined);
                                *assignmentResult = {};
                                assignmentResult->TypeAndValue = namedVar.TypeAndValue;
                                assignmentResult->TypeAndValue.VariableName.clear();
                                assignmentResult->TypeAndValue.IsMove = false;
                                assignmentResult->TypeAndValue.IsAlias = true;
                                assignmentResult->Primary = loaded;
                                assignmentResult->BaseType = loaded->getType();
                                assignmentResult->Storage = joined;
                                return loaded;
                            }
                            if (available == nullptr && !conversionRefusal.empty())
                                LogErrorContext(ctx, std::format(
                                    "cannot assign to C++ class '{}' from this expression; {}",
                                    compiler->DisplayCxxClassName(tn), conversionRefusal));
                        }
                        else
                        {
                            LLVMBackend::TypeAndValue selectedReturn;
                            const auto tempMark = compiler->MarkOwnedTemps();
                            auto* direct = TryDirectCxxAssignOperator(
                                ctx, namedVar, tn, rhsNV, conversionRefusal,
                                /*probeOnly*/ false, &selectedReturn);
                            if (direct != nullptr || !conversionRefusal.empty())
                            {
                                if (direct == nullptr)
                                {
                                    LogErrorContext(ctx, std::format(
                                        "cannot assign to C++ class '{}' from this expression; {}",
                                        compiler->DisplayCxxClassName(tn), conversionRefusal));
                                    return nullptr;
                                }
                                if (namedVar.FieldName.empty() && !namedVar.CallerName.empty())
                                {
                                    compiler->MarkVariableUnmoved(namedVar.CallerName);
                                    compiler->MarkVariableNotExplicitlyMovedNull(namedVar.CallerName);
                                }
                                if (assignmentResult == nullptr)
                                {
                                    compiler->FlushOwnedTempsSince(tempMark, nullptr, nullptr);
                                    compiler->lastCxxRetTemp_ = nullptr;
                                    compiler->lastCxxRetValue_ = nullptr;
                                }
                                auto* assignmentSlot = CxxAssignSretScope::TargetFor(compiler, tn);
                                auto* value = CxxAssignOperatorValue(ctx, namedVar, tn, direct,
                                    selectedReturn, assignmentResult, assignmentSlot);
                                if (assignmentSlot != nullptr)
                                {
                                    compiler->lastCxxRetTemp_ = nullptr;
                                    compiler->lastCxxRetValue_ = nullptr;
                                }
                                return assignmentResult == nullptr ? nullptr : value;
                            }
                        }
                    }
                    // `t = move u;` with a class source is `t = T(move u)`: the source binds as
                    // an rvalue (T(U&&) wins) and is consumed like any moved C++ local.
                    bool movedConversion = false;
                    if (useMove && !adoptedRaw && rhsTemp == nullptr
                        && !rhsNV.CallerName.empty() && rhsNV.Storage != nullptr
                        && !rhsNV.TypeAndValue.Pointer && rhsNV.TypeAndValue.TypeName != tn
                        && !rhsNV.IsMoved && !rhsNV.ExplicitlyMovedNull)
                    {
                        LLVMBackend::NamedVariable converted = rhsNV;
                        converted.Primary = nullptr;
                        converted.IsExplicitMove = true;
                        compiler->SetCurrentDebugLocation(ctx->getStart()->getLine());
                        if (compiler->ConvertThroughCxxConvertingCtor(converted, tn, nullptr,
                                conversionRefusal)
                            == LLVMBackend::CxxConvertingCtorResult::Converted)
                        {
                            rhsNV = converted;
                            rhsTemp = converted.Storage;
                            movedConversion = true;
                        }
                    }
                    if (convertibleSource)
                    {
                        LLVMBackend::NamedVariable converted = rhsNV;
                        compiler->SetCurrentDebugLocation(ctx->getStart()->getLine());
                        switch (compiler->ConvertThroughCxxConvertingCtor(
                                    converted, tn, nullptr, conversionRefusal,
                                    /*directInit*/ false, /*allowScalarNarrowing*/ true))
                        {
                        case LLVMBackend::CxxConvertingCtorResult::Converted:
                            rhsNV = converted;
                            rhsTemp = converted.Storage;
                            break;
                        default:
                            break;
                        }
                    }
                    // `move x` of a C++ class: clang picks the member when cflat's direct one is
                    // not that pick (const source, deleted / implicit / absent move assignment).
                    bool moveTransfer = false;
                    bool moveConstSource = false;
                    if (useMove && !adoptedRaw && !movedConversion && !conversionRefusal.empty())
                    {
                        LogErrorContext(ctx, std::format(
                            "cannot assign to C++ class '{}' from this expression; {}",
                            compiler->DisplayCxxClassName(tn), conversionRefusal));
                    }
                    else if (useMove && !adoptedRaw && !movedConversion)
                    {
                        if (moveSourceNV.Storage == nullptr
                            || moveSourceNV.TypeAndValue.Pointer
                            || moveSourceNV.TypeAndValue.TypeName != tn)
                            LogErrorContext(ctx, std::format(
                                "'{}' is not a '{}' value that can be assigned to a '{}'", srcName, tn, tn));
                        if (moveSourceNV.IsMoved || moveSourceNV.ExplicitlyMovedNull)
                            LogMovedUse(ctx, srcName,
                                        IsPointeeMoveUse(moveSourceNV) && moveSourceNV.CallerName == srcName, true);
                        sourceStorage = moveSourceNV.Storage;
                        moveConstSource = compiler->CxxConstReceiverKind(moveSourceNV) != 0;
                        moveTransfer = sourceStorage != nullptr
                            && (compiler->CxxExplicitMoveNeedsTransfer(tn, moveConstSource, true)
                                || compiler->CxxExplicitMoveNeedsTransfer(tn, moveConstSource,
                                                                          false));
                        // Clang's own overload resolution (Sema) picked the assignment at extraction:
                        // a selected const T& borrows the moved source, and the shared ownership test decides.
                        if (info != nullptr
                            && (moveConstSource ? info->moveAssignConstSourceBindsConstRef
                                                : info->moveAssignBindsConstRef))
                        {
                            LLVMBackend::TypeAndValue selected;
                            selected.TypeName = tn;
                            selected.Pointer = true;
                            selected.SetCxxConstRef(true);
                            selected.VariableName = "o";
                            auto moved = moveSourceNV;
                            moved.IsExplicitMove = true;
                            compiler->DiagnoseExplicitMoveToBorrowParam("operator=", selected,
                                moved, /*foreignCxxCallee*/ true);
                        }
                    }
                    else if (xvalueSource)
                    {
                        // A proven xvalue of T: clang selects the member (emitAssign/emitConstruct).
                    }
                    else if (rhsTemp == nullptr && !conversionRefusal.empty())
                    {
                        LogErrorContext(ctx, std::format(
                            "cannot assign to C++ class '{}' from this expression; {}",
                            compiler->DisplayCxxClassName(tn), conversionRefusal));
                    }
                    else if (rhsTemp == nullptr
                        && (rhsNV.TypeAndValue.Pointer || rhsNV.TypeAndValue.TypeName != tn
                            || rhsNV.Storage == nullptr))
                    {
                        if (rhsNV.TypeAndValue.IsRvalueRef)
                        {
                            LLVMBackend::TypeAndValue referent = rhsNV.TypeAndValue;
                            referent.IsRvalueRef = false;
                            if (referent.ElemPointer) referent.ElemPointer = false;
                            else referent.Pointer = false;
                            LogErrorContext(ctx, std::format(
                                "cannot assign to C++ class '{}' from the rvalue reference '{}&&' "
                                "this call returns; only a '{}&&' result move-assigns it",
                                compiler->DisplayCxxClassName(tn),
                                SpellDiagnosticType(*compiler, referent),
                                compiler->DisplayCxxClassName(tn)));
                        }
                        LogErrorContext(ctx, std::format(
                            "cannot assign to C++ class '{}' from this expression; the source must be "
                            "a '{}' variable, optionally written 'move <variable>'",
                            compiler->DisplayCxxClassName(tn),
                            compiler->DisplayCxxClassName(tn)));
                    }
                    else if (rhsTemp == nullptr && (rhsNV.IsMoved || rhsNV.ExplicitlyMovedNull))
                    {
                        LogMovedUse(ctx, rhsNV.CallerName,
                                    IsPointeeMoveUse(rhsNV), true);
                    }
                    // A '?:' whose arms are both explicit moves (`a = c ? move b : move c`) is not
                    // a bare `move x`, yet still borrows its source into a const T& operator=.
                    if (!useMove && rhsNV.IsExplicitMove && rhsTemp == nullptr && !rhsDefault
                        && info != nullptr && rhsNV.Storage != nullptr
                        && !rhsNV.TypeAndValue.Pointer && rhsNV.TypeAndValue.TypeName == tn)
                    {
                        // A const arm makes the join a const xvalue: a plain copy-assign (T288 r2).
                        const bool constSource = compiler->CxxConstReceiverKind(rhsNV) != 0;
                        if (!constSource && info->moveAssignBindsConstRef)
                        {
                            LLVMBackend::TypeAndValue selected;
                            selected.TypeName = tn;
                            selected.Pointer = true;
                            selected.SetCxxConstRef(true);
                            selected.VariableName = "o";
                            compiler->DiagnoseExplicitMoveToBorrowParam("operator=", selected,
                                rhsNV, /*foreignCxxCallee*/ true);
                        }
                    }

                    const bool sourceIsTemporary = rhsTemp != nullptr || xvalueSource;
                    // An addressable, non-temporary source outlives this statement: an lvalue.
                    const bool sourceIsLvalue = !useMove && !sourceIsTemporary
                        && rhsNV.Storage != nullptr && !rhsNV.IsRvalue;
                    // An xvalue source (`[const] T&&`): clang picks the constructor / operator=.
                    auto emitXvalue = [&](bool assignment) {
                        std::string refusal;
                        if (!compiler->EmitCxxXvalueTransfer(tn, destination, sourceStorage,
                                rhsNV.TypeAndValue.CxxConstRef(), assignment, refusal))
                            LogErrorContext(ctx, refusal);
                    };
                    const auto* op = [&]() -> const LLVMBackend::CxxClassInfo::Structor* {
                        if (info == nullptr) return nullptr;
                        const LLVMBackend::CxxClassInfo::Structor* result = nullptr;
                        if ((useMove || sourceIsTemporary) && info->hasMoveAssign)
                            result = lvalueReceiverAssignment(
                                info->moveAssignOverloads, info->moveAssign);
                        if (result == nullptr && info->hasCopyAssign)
                            result = lvalueReceiverAssignment(
                                info->copyAssignOverloads, info->copyAssign,
                                sourceIsLvalue ? (rhsNV.TypeAndValue.IsConstObject() ? 1 : 0) : -1);
                        if (result == nullptr && !useMove && !sourceIsTemporary
                            && info->hasMoveAssign && !sourceIsLvalue)
                            result = lvalueReceiverAssignment(
                                info->moveAssignOverloads, info->moveAssign);
                        return result;
                    }();
                    // Only a move assignment exists: taking it from an lvalue would silently
                    // empty the source, which C++ refuses (its copy assignment is deleted).
                    if (op == nullptr && sourceIsLvalue && info != nullptr && info->hasMoveAssign
                        && !destinationReleased && !destinationConditional
                        && !destinationGlobalConditional)
                        LogErrorContext(ctx, std::format(
                            "cannot copy-assign C++ class '{}': it has no copy assignment operator "
                            "cflat can call, and assigning from an lvalue would silently move from "
                            "it; write 'move <variable>' or assign a temporary",
                            compiler->DisplayCxxClassName(tn)));

                    auto emitMoveTransfer = [&](bool assignment) {
                        std::string refusal;
                        if (!compiler->EmitCxxExplicitMoveTransfer(tn, destination, sourceStorage,
                                moveConstSource, assignment, srcName, refusal))
                            LogErrorContext(ctx, refusal);
                    };
                    auto emitConstruct = [&]() {
                        if (xvalueSource) return emitXvalue(false);
                        if (moveTransfer
                            && compiler->CxxExplicitMoveNeedsTransfer(tn, moveConstSource, false))
                            return emitMoveTransfer(false);
                        llvm::Value* source = rhsTemp != nullptr ? rhsTemp : sourceStorage;
                        if (source == nullptr)
                            LogErrorContext(ctx, std::format(
                                "cannot construct C++ class '{}' from this expression",
                                compiler->DisplayCxxClassName(tn)));
                        compiler->EmitCxxCopyOrMoveConstruct(
                            tn, destination, source, useMove || sourceIsTemporary,
                            "into an assigned destination");
                    };
                    auto emitAssign = [&]() {
                        if (xvalueSource) return emitXvalue(true);
                        if (moveTransfer
                            && compiler->CxxExplicitMoveNeedsTransfer(tn, moveConstSource, true))
                            return emitMoveTransfer(true);
                        llvm::Value* source = rhsTemp != nullptr ? rhsTemp : sourceStorage;
                        if (op == nullptr)
                        {
                            if (compiler->CxxHasDeletedAssignment(tn, useMove))
                                LogErrorContext(ctx,
                                    "clang: overload resolution selected deleted operator '='");
                            if (info != nullptr && source != nullptr
                                && (useMove ? info->hasTrivialMoveAssign
                                            : info->hasTrivialCopyAssign))
                            {
                                auto* value = compiler->CreateLoad(namedVar.BaseType, source);
                                compiler->builder->CreateStore(value, destination);
                                return;
                            }
                            // clang's own pick (a defaulted base-class operator=, as optional<E>'s)
                            // when cflat lists none; only a refusal there is the error below.
                            std::string clangRefusal;
                            if (source != nullptr
                                && compiler->EmitCxxXvalueTransfer(tn, destination, source,
                                       false, true, clangRefusal))
                                return;
                            LogErrorContext(ctx, std::format(
                                "C++ class '{}' has no assignment operator cflat can call (it is implicit, "
                                "deleted, inaccessible, or defined inline in the header) - assign through a "
                            "pointer, or re-declare the destination instead",
                            compiler->DisplayCxxClassName(tn)));
                        }
                        if (source == nullptr)
                            LogErrorContext(ctx, std::format(
                                "cannot assign to C++ class '{}' from this expression",
                                compiler->DisplayCxxClassName(tn)));
                        // `operator=(T other)` takes its parameter BY VALUE: it must be copy- or
                        // move-constructed from the source's address, never handed the address.
                        std::vector<LLVMBackend::NamedVariable> byValueSource;
                        if (op != nullptr && op->params.size() == 2 && !op->params[1].Pointer
                            && compiler->IsForeignNontrivialCxxClass(op->params[1].TypeName))
                        {
                            LLVMBackend::NamedVariable sourceVar;
                            sourceVar.Storage = source;
                            sourceVar.TypeAndValue.TypeName = tn;
                            sourceVar.IsExplicitMove = useMove || sourceIsTemporary;
                            byValueSource.push_back(std::move(sourceVar));
                        }
                        compiler->EmitCxxStructorCall(tn, *op, destination, { source },
                            byValueSource.empty() ? nullptr : &byValueSource);
                    };

                    if (destinationGlobalConditional)
                    {
                        auto* liveBlock = compiler->CreateBasicBlock("cxx.assign.global.live");
                        auto* constructBlock = compiler->CreateBasicBlock("cxx.assign.global.construct");
                        auto* doneBlock = compiler->CreateBasicBlock("cxx.assign.global.done");
                        auto* live = compiler->builder->CreateLoad(
                            compiler->builder->getInt1Ty(), destinationGlobalLiveFlag,
                            "cxx.assign.global.livef");
                        compiler->CreateConditionJump(live, liveBlock, constructBlock);
                        compiler->SwitchToBlock(liveBlock);
                        emitAssign();
                        compiler->CreateJump(doneBlock);
                        compiler->SwitchToBlock(constructBlock);
                        emitConstruct();
                        compiler->CreateJump(doneBlock);
                        compiler->SwitchToBlock(doneBlock);
                    }
                    else if (destinationConditional)
                    {
                        auto* liveBlock = compiler->CreateBasicBlock("cxx.assign.live");
                        auto* constructBlock = compiler->CreateBasicBlock("cxx.assign.construct");
                        auto* doneBlock = compiler->CreateBasicBlock("cxx.assign.done");
                        auto* live = compiler->builder->CreateLoad(
                            compiler->builder->getInt1Ty(), namedVar.ConditionalDropFlag,
                            "cxx.assign.livef");
                        compiler->CreateConditionJump(live, liveBlock, constructBlock);
                        compiler->SwitchToBlock(liveBlock);
                        emitAssign();
                        compiler->CreateJump(doneBlock);
                        compiler->SwitchToBlock(constructBlock);
                        emitConstruct();
                        compiler->CreateJump(doneBlock);
                        compiler->SwitchToBlock(doneBlock);
                    }
                    else if (destinationReleased)
                        emitConstruct();
                    else
                        emitAssign();

                    if (destinationGlobalConditional)
                        compiler->builder->CreateStore(compiler->builder->getInt1(true),
                                                       destinationGlobalLiveFlag);

                    if (useMove && !adoptedRaw) markMoveSource();
                    if (rhsTemp != nullptr)
                        compiler->FlushOwnedTempsSince(ownedTempMark, nullptr, nullptr);
                    if (namedVar.FieldName.empty() && !namedVar.CallerName.empty())
                    {
                        compiler->MarkVariableUnmoved(namedVar.CallerName);
                        compiler->MarkVariableNotExplicitlyMovedNull(namedVar.CallerName);
                    }
                    // A '=' store re-initializes the destination field path (reviveStoredField, which
                    // the finishStore tail runs); this foreign-class store returns before that tail.
                    if (operatorText == "=" && !namedVar.FieldName.empty())
                    {
                        const std::string movePath = LLVMBackend::FieldMoveKey(namedVar);
                        if (!movePath.empty())
                        {
                            movedf::ReviveFieldPath(namedVar.MovedFields, movePath);
                            if (!namedVar.CallerName.empty())
                                compiler->MarkVariableFieldUnmoved(namedVar.CallerName, movePath);
                        }
                    }
                    return classAssignValue();
                }
            }
            /*
             * `t = move x` of a TRIVIALLY COPYABLE C++ class whose std::move(x) assignment pick is
             * not the trivial copy (an assignment template, a const operator=): clang's pick runs
             * on the live source, as std::move does; anything else keeps the byte copy below.
             */
            if (operatorText == "=" && destination != nullptr && !namedVar.TypeAndValue.Pointer
                && compiler->IsCxxRecord(namedVar.TypeAndValue.TypeName)
                && !compiler->IsForeignNontrivialCxxClass(namedVar.TypeAndValue.TypeName))
                if (auto* mv = TopLevelMoveExpression(assignCtx);
                    mv != nullptr && mv->unaryExpression() != nullptr
                    && IsBareIdentifierText(mv->unaryExpression()->getText()))
                {
                    const std::string& tn = namedVar.TypeAndValue.TypeName;
                    const std::string srcName = mv->unaryExpression()->getText();
                    auto* srcNV = compiler->FindLiveNamedVariable(srcName);
                    const bool constSource = srcNV != nullptr && compiler->CxxConstReceiverKind(*srcNV) != 0;
                    if (srcNV != nullptr && srcNV->Storage != nullptr && !srcNV->TypeAndValue.Pointer
                        && srcNV->TypeAndValue.TypeName == tn
                        && compiler->CxxTrivialMoveNeedsTransfer(tn, constSource, true))
                    {
                        if (srcNV->IsMoved || srcNV->ExplicitlyMovedNull)
                            LogMovedUse(ctx, srcName,
                                        IsPointeeMoveUse(*srcNV) && srcNV->CallerName == srcName, true);
                        compiler->SetCurrentDebugLocation(ctx->getStart()->getLine());
                        std::string refusal;
                        if (!compiler->EmitCxxExplicitMoveTransfer(tn, destination, srcNV->Storage,
                                constSource, true, srcName, refusal))
                            LogErrorContext(ctx, refusal);
                        compiler->MarkVariableMoved(srcName);
                        if (namedVar.FieldName.empty() && !namedVar.CallerName.empty())
                        {
                            compiler->MarkVariableUnmoved(namedVar.CallerName);
                            compiler->MarkVariableNotExplicitlyMovedNull(namedVar.CallerName);
                        }
                        return classAssignValue();
                    }
                }

            // A range-for variable is a borrow of the current element, not an owning local. For
            // addressable arrays/views, route the ordinary assignment machinery to that element
            // slot. Container legs use their write-through `set` operation instead.
            if (operatorText == "=")
            {
                if (auto* range = FindActiveRangeForVariable(namedVar))
                {
                    if (range->elementStorage != nullptr)
                    {
                        namedVar.Storage = range->elementStorage;
                        namedVar.Primary = nullptr;
                        namedVar.BaseType = range->elementBaseType;
                        namedVar.TypeAndValue = range->elementType;
                        namedVar.CallerName.clear();
                        namedVar.FieldName.clear();
                        namedVar.IsRangeForBorrow = false;
                        destination = namedVar.Storage;
                    }
                    else
                    {
                        auto rightNV = [&] {
                            CxxCallArgumentScope cxxAssignRhsScope(inCxxCallArgument_,
                                !namedVar.TypeAndValue.Pointer && compiler->IsCxxRecord(namedVar.TypeAndValue.TypeName));
                            return ParseAssignmentExpressionNamed(assignCtx);
                        }();
                        LLVMBackend::NamedVariable indexNV;
                        indexNV.Primary = compiler->CreateLoad(range->indexStorage);
                        indexNV.BaseType = indexNV.Primary->getType();
                        indexNV.TypeAndValue.TypeName = "int";
                        LLVMBackend::NamedVariable selfArg = range->collection;
                        selfArg.TypeAndValue.VariableName.clear();
                        selfArg.Primary = nullptr;
                        llvm::Value* setResult = nullptr;
                        if (range->isInterface)
                        {
                            setResult = compiler->CallInterfaceMethod(
                                range->interfaceStorage, range->collection.TypeAndValue.TypeName,
                                "set", { indexNV, rightNV }, nullptr, &range->collection);
                        }
                        else
                        {
                            setResult = compiler->CreateOverloadedFunctionCall(
                                "set", { selfArg, indexNV, rightNV });
                        }
                        return setResult != nullptr ? setResult : LoadNamedVariable(rightNV);
                    }
                }
                else if (IsActiveRangeCollectionElement(namedVar))
                {
                    for (auto it = rangeForStack_.rbegin(); it != rangeForStack_.rend(); ++it)
                    {
                        if (it->collectionStorage == nullptr && it->collectionValue == nullptr)
                            continue;
                        LogErrorContext(unaryCtx, std::format(
                            "cannot overwrite collection storage while range-for variable '{}' "
                            "borrows its element; mutate through '{}' instead",
                            it->variableName, it->variableName));
                    }
                }
            }

            // Guarded-field WRITE check: the lock must be held exclusively. Runs on the resolved
            // LHS only, so a guarded field READ inside the LHS (e.g. an index 'a[n->count]') is
            // not mistaken for a write.
            CheckGuardedWrite(unaryCtx, namedVar);

            // Storing through a temp spill (`mk().vals[0] = x`) writes into a shallow copy that dies
            // with the full expression - the write is unobservable, so reject it rather than emit a
            // silently discarded store. The READ form is legal (the spill exists for it).
            if (namedVar.IsTempSpillStorage)
            {
                LogErrorContext(unaryCtx,
                    "cannot assign into an inline array field of a temporary value - the slot lives in "
                    "a copy that dies at the end of the expression, so the store is discarded. Bind the "
                    "call result to a local first, then assign into it.");
            }

            /*
             * Assignment to a WHOLE fixed array (`b = ...` where b is `T[N]`, not `b[i] = ...`).
             * Unsupported, and every spelling was already broken: a non-literal RHS reached an
             * invalid `ptr` -> `[N x T]` bitcast and failed module verification, while a string
             * literal folded that cast into a ConstantExpr the verifier does not check - which
             * then miscompiled to garbage, or crashed the compiler in SelectionDAG on an indexed
             * read. Rejected here rather than implemented: whole-array assignment is a feature
             * with its own axes (compound operators, field arrays, globals).
             */
            if (namedVar.TypeAndValue.ConstArraySize > 0
                && !namedVar.TypeAndValue.IsArrayView
                && namedVar.BaseType != nullptr && llvm::isa<llvm::ArrayType>(namedVar.BaseType))
            {
                if (std::string error = FlexibleArrayMemberLengthError(namedVar.TypeAndValue);
                    !error.empty())
                    LogErrorContext(unaryCtx, error);
                std::string rhsText = assignCtx != nullptr ? assignCtx->getText() : std::string();
                // The LHS as written, not CallerName - on a field receiver ('u.a') CallerName is
                // just the base variable, and the suggested remedy would name the wrong thing.
                std::string lhsName = unaryCtx != nullptr && !unaryCtx->getText().empty()
                    ? unaryCtx->getText() : namedVar.CallerName;
                if (!rhsText.empty() && rhsText.front() == '"')
                    LogErrorContext(unaryCtx, std::format(
                        "cannot assign a string literal to fixed array '{}' - CFlat has no "
                        "C-style character-array assignment. Use 'char* p = ...' to point at the "
                        "literal, 'string s = ...' for a managed string, or copy the bytes into "
                        "'{}' element by element.",
                        DescribeArrayShape(namedVar.TypeAndValue), lhsName));
                else
                    LogErrorContext(unaryCtx, std::format(
                        "assignment to a whole fixed array '{}' is not supported - assign its "
                        "elements ('{}[i] = ...'), or copy at the DECLARATION instead "
                        "('{} dst = <source>;'), which is supported.",
                        DescribeArrayShape(namedVar.TypeAndValue), lhsName,
                        DescribeArrayShape(namedVar.TypeAndValue)));
            }

            // The left side must resolve to an addressable storage location. A null Storage
            // means the LHS produced a value, not an lvalue - storing through it would
            // dereference null below (in isDerefStorage's isa<> checks). Reject it with a
            // diagnostic instead. A simd<T,N> lane subscript (`v[i]`) is the common offender:
            // it lowers to an extractelement value (lanes are read-only), so call that out.
            // Bitfield writes are exempt: they legitimately carry null Storage and route
            // the store through BitfieldStorage in bitfieldAssign below.
            // A fresh closure value (a ternary whose function arm was lifted, T188) has a copy temp as
            // its storage; writing there would be lost, so it is not an lvalue either.
            if (destination != nullptr && namedVar.IsRvalue && namedVar.Primary != nullptr
                && namedVar.Primary->getType() == compiler->GetClosureFatPtrType())
                LogErrorContext(unaryCtx, "Left side of assignment is not an addressable lvalue.");
            if (destination == nullptr && namedVar.BitfieldStorage == nullptr)
            {
                if (namedVar.Primary && llvm::isa<llvm::ExtractElementInst>(namedVar.Primary))
                    LogErrorContext(unaryCtx, "simd<T,N> lane write 'v[i] = ...' is not supported; lanes are read-only. Build a new vector value instead.");
                else
                    LogErrorContext(unaryCtx, "Left side of assignment is not an addressable lvalue.");
            }

            // A bound C++ const/constexpr namespace object lives in read-only storage, so the
            // store would trap at run time. Name the LHS as written, not the mangled symbol.
            auto* constGlobal = destination != nullptr
                ? llvm::dyn_cast<llvm::GlobalVariable>(llvm::getUnderlyingObject(destination))
                : nullptr;
            if (!constCxxCompoundCandidate
                && compiler->IsConstHeaderGlobal(constGlobal)
                && !compiler->IsCxxMutableFieldAccess(destination))
            {
                std::string lhsName = unaryCtx != nullptr && !unaryCtx->getText().empty()
                    ? unaryCtx->getText() : namedVar.CallerName;
                if (compiler->cGlobalSymbols_.count(constGlobal->getName().str()) != 0)
                    LogErrorContext(unaryCtx, std::format(
                        "cannot assign to '{}': it is a const C object, so its storage is read-only. "
                        "Copy it into a local and assign to that instead.", lhsName));
                else
                    LogErrorContext(unaryCtx, std::format(
                        "cannot assign to '{}': it is a const C++ object, so its storage is read-only. "
                        "Copy it into a local and assign to that instead.", lhsName));
            }

            // For through-pointer dereferences (*p), Storage is a raw loaded ptr (not alloca/gep/global)
            // and BaseType holds the pointee type. All loads/stores through `destination` must use it.
            // UnionFieldType is set for union field access - it overrides the load/store type so that
            // reinterpret semantics work correctly even with LLVM opaque pointers.
            auto isDerefStorage = [&]() {
                return namedVar.BaseType
                    && !llvm::isa<llvm::AllocaInst>(destination)
                    && !llvm::isa<llvm::GlobalVariable>(destination)
                    && (!llvm::isa<llvm::GetElementPtrInst>(destination)
                        || (namedVar.IsBorrowed && !namedVar.IsElementAccess));
            };
            auto derefLoad = [&]() -> llvm::Value* {
                // Bitfield: the extracted value was already computed at field-access time.
                if (namedVar.BitfieldStorage)
                    return namedVar.Primary;
                if (namedVar.UnionFieldType)
                    return compiler->CreateLoad(namedVar.UnionFieldType, destination);
                return isDerefStorage()
                    ? compiler->CreateLoad(namedVar.BaseType, destination)
                    : compiler->CreateLoad(destination);
            };
            // Bitfield read-modify-write: ((word & ~window) | ((val & valMask) << off)).
            // Returns the truncated/sign-extended bitfield value so the assignment
            // expression's result matches the bitfield's declared type.
            auto bitfieldAssign = [&](llvm::Value* val) -> llvm::Value* {
                // BitfieldStorageType is the field's LLVM type ([N x i8] for a byte run); the
                // arithmetic runs on its integer word.
                auto* storageTy = compiler->GetBitfieldWordType(namedVar.BitfieldStorageType);
                unsigned storageBits = (unsigned)storageTy->getIntegerBitWidth();
                unsigned w = namedVar.BitfieldWidth;
                unsigned off = namedVar.BitfieldOffset;
                uint64_t valMask = (w == 64) ? ~uint64_t(0) : ((uint64_t(1) << w) - 1);
                uint64_t windowMask = valMask << off;

                // Cast the incoming value to the storage word's integer width.
                // Third arg of CreateCast is isSigned (used only when widening); flip
                // BitfieldUnsigned to get sign-extension for signed bitfields.
                // Declared bool: any -> bool is a truth test (ruling 2026-10-13), zero-extended
                // so the word sees 0/1 and not a sign-extended i1.
                llvm::Value* valAsStorage = nullptr;
                if (namedVar.TypeAndValue.TypeName == "bool")
                    valAsStorage = compiler->builder->CreateZExt(compiler->CoerceToBoolCondition(val), storageTy);
                else
                {
                    compiler->RefuseImplicitFloatToInt(val, storageTy, namedVar.BitfieldUnsigned);
                    valAsStorage = compiler->CreateCast(val, storageTy, !namedVar.BitfieldUnsigned, namedVar.BitfieldUnsigned);
                }
                auto* valMasked = compiler->builder->CreateAnd(valAsStorage, cflat_llvm::GetIntTruncated(storageTy, valMask));
                auto* valShifted = compiler->builder->CreateShl(valMasked, cflat_llvm::GetIntTruncated(storageTy, off));
                auto* word = compiler->LoadBitfieldWord(namedVar.BitfieldStorage, namedVar.BitfieldStorageType);
                auto* cleared = compiler->builder->CreateAnd(word, cflat_llvm::GetIntTruncated(storageTy, ~windowMask));
                auto* newWord = compiler->builder->CreateOr(cleared, valShifted);
                compiler->StoreBitfieldWord(namedVar.BitfieldStorage, namedVar.BitfieldStorageType, newWord);

                // Recompute the extracted bitfield value (post-store) so this expression's
                // result is the value actually stored, with width/sign correctly applied.
                llvm::Value* result;
                if (namedVar.BitfieldUnsigned)
                {
                    result = valMasked;  // already masked to width, zero-extended
                }
                else
                {
                    unsigned leftShift = storageBits - w;
                    auto* shl = compiler->builder->CreateShl(valMasked, cflat_llvm::GetIntTruncated(storageTy, leftShift));
                    result = compiler->builder->CreateAShr(shl, cflat_llvm::GetIntTruncated(storageTy, leftShift));
                }
                return result;
            };
            auto derefAssign = [&](llvm::Value* val, bool isUnsigned) -> llvm::Value* {
                if (namedVar.BitfieldStorage)
                    return bitfieldAssign(val);

                /*
                 * Storing a non-array value into `[N x T]` storage. The whole-VARIABLE spelling is
                 * rejected above with its own wording; this catches every other receiver, of which
                 * a ROW of a multidimensional array (`b[0] = ...`) is the reachable one - indexing
                 * strips the outer dimension, so the whole-array guard never sees it. A string
                 * literal source here folded the bad cast into a ConstantExpr, which miscompiled
                 * to garbage or crashed the compiler in SelectionDAG on a later indexed read.
                 * The type is the INDEXED storage's (the GEP's), never the union FIELD's - a
                 * subscripted union field targets one element, not the whole field.
                 */
                auto* rowDestTy = isDerefStorage() ? namedVar.BaseType
                                                   : compiler->GetTypeFromStorage(destination);
                if (llvm::isa_and_nonnull<llvm::ArrayType>(rowDestTy)
                    && val != nullptr && val->getType() != rowDestTy)
                {
                    std::string lhsText = unaryCtx != nullptr ? unaryCtx->getText() : std::string("<lhs>");
                    LogErrorContext(unaryCtx, std::format(
                        "cannot assign to '{}' as a whole - it names {}, and CFlat has no "
                        "whole-array assignment. Copy the elements ('{}[i] = ...'), or bind an "
                        "array view ('T[] row = {};') and write through that.",
                        lhsText,
                        LLVMBackend::DescribeAggregateStorageShape(
                            rowDestTy, SpellType(*compiler, namedVar.TypeAndValue)),
                        lhsText, lhsText));
                }

                if (namedVar.UnionFieldType)
                {
                    auto* stored = compiler->CreateAssignment(val, destination, isUnsigned, namedVar.UnionFieldType,
                        namedVar.TypeAndValue.IsUnsignedInteger() != -1,
                        namedVar.TypeAndValue.IsArrayView);
                    return stored;
                }
                return isDerefStorage()
                    ? compiler->CreateAssignment(val, destination, isUnsigned, namedVar.BaseType,
                        namedVar.TypeAndValue.IsUnsignedInteger() != -1,
                        namedVar.TypeAndValue.IsArrayView)
                    : compiler->CreateAssignment(val, destination, isUnsigned, nullptr,
                        namedVar.TypeAndValue.IsUnsignedInteger() != -1,
                        namedVar.TypeAndValue.IsArrayView);
            };

            /*
             * Null-coalescing assignment: x ??= rhs  ->  if (x == 0/null) x = rhs. Only the null
             * test and the branch are emitted here; the STORE and every piece of bookkeeping that
             * goes with it are the plain-'=' tail below, entered with the builder positioned in the
             * assign arm. That routing is the whole point: this handler used to emit its own store
             * and RETURN, skipping the ownership transfer, the moved-flag revival, the bond and
             * borrow refreshes and the thin-function<> provenance gate the tail runs.
             */
            llvm::BasicBlock* coalesceResume = nullptr;
            std::optional<BranchPostfixScope> coalescePostfix;  // `??=` RHS runs only on null
            std::optional<OwnershipControlScope> coalesceControlScope;
            std::string coalesceLhsOwner;
            if (operatorText == "?" "?=")
            {
                auto* lhs = derefLoad();
                // The null test IS a guard read of the destination, and derefLoad bypasses
                // LoadNamedVariable, which is where that event is normally logged. Without it the
                // null dataflow cannot see the guard and reports the desugared-equivalent
                // `if (a == nullptr) { a = ...; }` shape as a moved-variable dereference.
                compiler->RecordNullRead(namedVar.CallerName);

                // Is this an ownership-tracked pointer binding? Same gate the plain '=' retirement
                // uses: bare identifier, its own alloca, not a field store.
                bool tracksOwnership = namedVar.TypeAndValue.Pointer && namedVar.FieldName.empty()
                    && !namedVar.CallerName.empty() && namedVar.Storage != nullptr
                    && llvm::isa<llvm::AllocaInst>(namedVar.Storage);
                // The LHS's PRE-STORE proof, taken before anything below can mutate the binding.
                if (tracksOwnership)
                {
                    const auto* lhsNV = compiler->FindVariableByStorage(namedVar.Storage);
                    if (BindingKeepsOwnershipOfBoxedObject(lhsNV))
                        coalesceLhsOwner = DescribeBoxedSourceOwner(nullptr, lhsNV);
                }

                auto* assignBlock = compiler->CreateBasicBlock("nullcoalasgn_assign");
                coalesceResume    = compiler->CreateBasicBlock("nullcoalasgn_resume");
                // Jump to the assign block only when lhs is null/zero.
                compiler->CreateConditionJump(lhs, coalesceResume, assignBlock);
                compiler->SwitchToBlock(assignBlock);
                coalescePostfix.emplace(this);
                coalesceControlScope.emplace(compiler);
                operatorText = "=";  // the tail IS the store path from here on
            }
            // Closes the '??=' assign arm and yields the destination's value at the join. Identity
            // for a plain '=', which has no arm to close.
            // A '=' store re-initializes a destination field: revive its moved mark on the binding
            // AND on the LHS copy taken at parse time, which later result loads read (`s.p = move o;`).
            auto reviveStoredField = [&]() {
                const std::string movePath = LLVMBackend::FieldMoveKey(namedVar);
                if (operatorText != "=" || coalesceResume != nullptr || movePath.empty())
                    return;
                movedf::ReviveFieldPath(namedVar.MovedFields, movePath);
                if (!namedVar.CallerName.empty())
                    compiler->MarkVariableFieldUnmoved(namedVar.CallerName, movePath);
            };
            bool uniqueFieldAssignmentResultProduced = false;
            llvm::Value* assignmentRhsValue = nullptr;
            const LLVMBackend::NamedVariable* storeSource = nullptr;
            auto finishStore = [&](llvm::Value* v) -> llvm::Value* {
                llvm::Value* resultValue = v;
                // A '=' store re-initializes a destination field path AFTER any source consume, so
                // a self-assign (`a.p = a.p`) cancels; a struct path clears every field under it.
                if (operatorText == "=" && !namedVar.FieldName.empty() && !namedVar.FieldPathText.empty()
                    && !namedVar.IsElementAccess)
                    compiler->RecordNullClear(namedVar.FieldPathText);
                reviveStoredField();
                if (coalesceResume == nullptr)
                {
                    if (auto* store = llvm::dyn_cast_or_null<llvm::StoreInst>(v))
                        resultValue = store->getValueOperand();
                }
                else
                {
                    if (coalescePostfix.has_value()) coalescePostfix->Flush();
                    compiler->CreateJump(coalesceResume);
                    compiler->SwitchToBlock(coalesceResume);
                    resultValue = derefLoad();
                }
                if (assignmentResult != nullptr)
                {
                    // The stored owner now belongs to the LHS; the assignment rvalue is only an alias.
                    compiler->lastCallReturnsOwned = false;
                    *assignmentResult = {};
                    assignmentResult->TypeAndValue = namedVar.TypeAndValue;
                    assignmentResult->TypeAndValue.VariableName.clear();
                    assignmentResult->TypeAndValue.IsMove = false;
                    assignmentResult->Primary = resultValue;
                    assignmentResult->BaseType = resultValue != nullptr
                        ? resultValue->getType() : namedVar.BaseType;
                    assignmentResult->IsUniqueFieldAssignmentResult =
                        (uniqueFieldAssignmentResultProduced
                            || (assignmentRhsValue != nullptr
                                && resultValue == assignmentRhsValue))
                        && coalesceResume == nullptr;
                    // An owning struct destination is named too: `T x = (w += 2)` / `return (w = y)`
                    // then consume `w` exactly as `T x = w` does, instead of adopting its bytes twice.
                    const bool ownedStructDest = !namedVar.TypeAndValue.Pointer
                        && !namedVar.TypeAndValue.IsInterface
                        && resultValue != nullptr && resultValue->getType()->isStructTy()
                        && !compiler->IsForeignNontrivialCxxClass(namedVar.TypeAndValue.TypeName)
                        && compiler->IsOwningValueType(namedVar.TypeAndValue.TypeName);
                    if (coalesceResume == nullptr
                        && (namedVar.TypeAndValue.IsUnique || ownedStructDest
                            || compiler->IsCoreUniqueType(namedVar.TypeAndValue.TypeName)))
                    {
                        assignmentResult->Storage = destination;
                        assignmentResult->CallerName = namedVar.CallerName;
                        assignmentResult->FieldName = namedVar.FieldName;
                        assignmentResult->MovePathKey = namedVar.MovePathKey;
                        assignmentResult->MovePathHasDynamicIndex = namedVar.MovePathHasDynamicIndex;
                        assignmentResult->IsOwning = true;
                        // Same provenance `T x = w` would see, so the borrow guards refuse
                        // `(h->f += 2)` exactly where they refuse `h->f`.
                        if (ownedStructDest)
                            AdoptWrapperProvenance(*assignmentResult, namedVar);
                    }
                    else if (namedVar.TypeAndValue.Pointer)
                    {
                        assignmentResult->IsBorrowed = true;
                        assignmentResult->BorrowedOrigin = namedVar.CallerName;
                        // T203/T250: a local slot fed by `new` (or ?: of new/nullptr) is recorded for an
                        // outer unique '='. Any other RHS keeps the borrow.
                        bool sawNewArm = false;
                        if (llvm::isa_and_nonnull<llvm::AllocaInst>(destination)
                            && namedVar.FieldName.empty() && !namedVar.CallerName.empty()
                            && OwnedOrNullArms(assignCtx, sawNewArm) && sawNewArm
                            && RawNewArrayValueOf(assignCtx) == nullptr)
                        {
                            assignmentResult->ChainedAssignSlot = destination;
                            assignmentResult->ChainedAssignName = namedVar.CallerName;
                        }
                    }
                }
                return resultValue;
            };

            // The assignment DESTINATION is the context an immediately-invoked literal in the RHS
            // takes its return type from, exactly as a declarator's type is.
            std::optional<DeclExpectedTypeScope> assignExpectedScope;
            if (operatorText == "=")
                assignExpectedScope.emplace(&declExpectedType, namedVar.TypeAndValue, true);

            // Thread expected function-pointer type into lambda RHS (for f = (x) => {...} reassignment)
            if (operatorText == "=" && namedVar.TypeAndValue.IsFunctionPointer)
                lambdaExpectedType = namedVar.TypeAndValue;
            // A generic-substituted closure field carries an ENCODED type name, not IsFunctionPointer;
            // recover its call descriptor so the lambda RHS infers the same return type.
            else if (operatorText == "=" && !namedVar.TypeAndValue.Pointer)
                if (const auto* enc = compiler->GetEncodedClosureType(namedVar.TypeAndValue.TypeName))
                    lambdaExpectedType = *enc;

            // Bond source reassignment check: error if LHS is a live bond source.
            // Only fire when destination is the variable's own alloca/global - field assignments
            // (GEP destinations) mutate the struct in place and do not invalidate any bond.
            // Must run before RHS evaluation so the error fires on the assignment statement.
            if (operatorText == "=" && !namedVar.CallerName.empty()
                && (llvm::isa<llvm::AllocaInst>(destination) || llvm::isa<llvm::GlobalVariable>(destination)))
            {
                auto borrower = compiler->FindActiveBondBorrower(namedVar.CallerName);
                if (!borrower.empty())
                    LogErrorContext(ctx, std::format("cannot reassign '{}' while '{}' holds a bonded reference to it - assign null to '{}' first to break the bond", namedVar.CallerName, borrower, borrower));
            }

            // Inbound alloc-align channel for direct assignment to an align-declared target (a field
            // whose clause was stamped on read, or a local carrying its declared clause). A DIRECT
            // `new` RHS inherits the target's allocation alignment; the store-check below then
            // verifies agreement. Indirect RHS shapes are left to error on mismatch, never inferred.
            uint64_t targetAllocAlign = namedVar.AllocAlignment > 0
                ? namedVar.AllocAlignment
                : namedVar.TypeAndValue.AllocAlignValue;
            if (operatorText == "=" && targetAllocAlign > LLVMBackend::kDefaultNewAlign
                && AsDirectNew(assignCtx) != nullptr)
                compiler->pendingInitAllocAlign = targetAllocAlign;
            // unique<T> frees a SINGLE object - it has no element count. A `new T[n]` reaching a
            // unique local is rejected here for the same reason the field, parameter and return
            // legs already reject it.
            if (operatorText == "=" && !namedVar.TypeAndValue.Pointer
                && namedVar.FieldName.empty()
                && (llvm::isa<llvm::AllocaInst>(destination)
                    || llvm::isa<llvm::GlobalVariable>(destination))
                && compiler->IsCoreUniqueType(namedVar.TypeAndValue.TypeName)
                && RawNewArrayValueOf(assignCtx) != nullptr)
                LogErrorContext(ctx, std::format(
                    "cannot store a heap array in unique local '{}': unique<T> does not own "
                    "arrays - use 'array<T>'", namedVar.CallerName));
            if (operatorText == "=")
                ArmArrayNewDesugar(assignCtx, namedVar.TypeAndValue);

            auto rightNV = [&] {
                CxxCallArgumentScope cxxAssignRhsScope(inCxxCallArgument_,
                    !namedVar.TypeAndValue.Pointer && compiler->IsCxxRecord(namedVar.TypeAndValue.TypeName));
                return ParseAssignmentExpressionNamed(assignCtx);
            }();
            storeSource = &rightNV;
            if (!namedVar.TypeAndValue.IsFunctionPointer && rightNV.GenericFunctionValue
                && rightNV.Primary == nullptr && !rightNV.CallerName.empty())
                rightNV.Primary = compiler->GetFunctionForFuncPtr(rightNV.CallerName);
            const bool explicitMoveClosureBind = assignCtx != nullptr
                && assignCtx->unaryExpression() != nullptr
                && assignCtx->unaryExpression()->moveExpression() != nullptr;
            if (operatorText == "=" && explicitMoveClosureBind
                && compiler->FuncPtrFewerParamsCanAdapt(rightNV.TypeAndValue,
                                                        namedVar.TypeAndValue))
            {
                rightNV.IsExplicitMove = true;
                rightNV.IsRvalue = true;
            }
            if (RefuseImmovableAssignSource(compiler, ctx, operatorText, rightNV)) return nullptr;
            // T203: `owner = raw = new T()` - the inner '=' left its local slot owning the object.
            // A unique destination takes that slot as its RHS, exactly as `owner = raw;` does.
            if (operatorText == "=" && !namedVar.TypeAndValue.Pointer
                && compiler->IsCoreUniqueType(namedVar.TypeAndValue.TypeName)
                && rightNV.IsBorrowed && rightNV.ChainedAssignSlot != nullptr
                && IsChainedAssignmentShape(assignCtx))
            {
                const auto* slotVar = compiler->FindVariableByStorage(rightNV.ChainedAssignSlot);
                if (slotVar != nullptr && slotVar->IsOwning)
                {
                    const std::string slotName = rightNV.ChainedAssignName;
                    rightNV = *slotVar;
                    rightNV.Primary = nullptr;
                    rightNV.CallerName = slotName;
                }
            }
            // `t = std.move(x)` of a trivially copyable C++ class: the xvalue's referent is
            // byte-copied (the trivial pick), or assigned through clang's pick when it is not.
            if (operatorText == "=" && !namedVar.TypeAndValue.Pointer
                && compiler->IsCxxRvalueRefClassResult(rightNV)
                && rightNV.TypeAndValue.TypeName == namedVar.TypeAndValue.TypeName
                && !compiler->IsForeignNontrivialCxxClass(namedVar.TypeAndValue.TypeName))
            {
                const std::string& tn = namedVar.TypeAndValue.TypeName;
                const bool constSource = rightNV.TypeAndValue.CxxConstRef();
                compiler->SetCurrentDebugLocation(ctx->getStart()->getLine());
                if (compiler->CxxTrivialMoveNeedsTransfer(tn, constSource, true))
                {
                    std::string refusal;
                    if (!compiler->EmitCxxXvalueTransfer(tn, destination, rightNV.Primary,
                                                         constSource, true, refusal))
                        LogErrorContext(ctx, refusal);
                }
                else if (auto* recordType = compiler->GetType(LLVMBackend::TypeAndValue{ .TypeName = tn }))
                    compiler->builder->CreateStore(
                        compiler->builder->CreateLoad(recordType, rightNV.Primary), destination);
                if (namedVar.FieldName.empty() && !namedVar.CallerName.empty())
                {
                    compiler->MarkVariableUnmoved(namedVar.CallerName);
                    compiler->MarkVariableNotExplicitlyMovedNull(namedVar.CallerName);
                }
                return classAssignValue();
            }
            if (operatorText == "=")
            {
                RejectNewPointerSlotDepthMismatch(ctx, rightNV, namedVar.TypeAndValue);
                compiler->RejectImplicitIntegerPointeePointerConversion(
                    rightNV.TypeAndValue, namedVar.TypeAndValue);
            }
            arrayNewDesugarCtx = nullptr;
            // The observer conversion below unwraps an alias result whose element type is the
            // core unique wrapper. Preserve that source fact for the pointer-local ledger; the
            // conversion necessarily replaces the wrapper TypeName with its raw pointee.
            const bool rhsCoreUniqueAliasElement = !rightNV.TypeAndValue.Pointer
                && rightNV.TypeAndValue.IsAlias
                && !rightNV.TypeAndValue.IsMove
                && compiler->IsCoreUniqueType(rightNV.TypeAndValue.TypeName);
            bool rightStringImplicitCopied = false;
            RejectLambdaReferenceCaptureEscape(
                IsProgramLifetimeStorage(namedVar), rightNV, ctx);
            if (operatorText == "=" && namedVar.TypeAndValue.IsFunctionPointer
                && namedVar.FieldName.empty() && !namedVar.CallerName.empty())
            {
                for (auto frameIt = compiler->stackNamedVariable.rbegin();
                     frameIt != compiler->stackNamedVariable.rend(); ++frameIt)
                {
                    auto dstIt = frameIt->namedVariable.find(namedVar.CallerName);
                    if (dstIt == frameIt->namedVariable.end() || dstIt->second.Storage != destination)
                        continue;
                    dstIt->second.LambdaCaptureNames = rightNV.LambdaCaptureNames;
                    dstIt->second.LambdaReferenceCaptureNames = rightNV.LambdaReferenceCaptureNames;
                    break;
                }
            }
            // An UNBOUND call result carries no binding, so the declaration/'=' classification
            // never saw it; stamp it here so the store doors read the same borrow facts.
            ApplyCallResultBorrowProvenance(compiler, rightNV);
            compiler->pendingInitAllocAlign = 0;  // one-shot; consumed by the `new` above if direct
            lambdaExpectedType = {};

            // A plain pointer assignment borrows a named owning local just like declaration-init.
            // Owning destinations and detached values stay on the existing transfer paths.
            const auto* existingDestination = compiler->FindVariableByStorage(destination);
            const bool destinationHadBorrowProvenance = existingDestination != nullptr
                && (existingDestination->IsBorrowed
                    || existingDestination->BorrowsOwningLocal
                    || existingDestination->BorrowsOwnedElement
                    || !existingDestination->BorrowedUniqueField.empty());
            // Set when this '=' retired the borrow the binding held (same statement block): the
            // owning store then proves the binding owns, as a fresh binding would.
            bool storeRetiredBorrow = false;
            bool assignmentBorrowsOwningLocal = false;
            std::string assignmentBorrowOrigin;
            llvm::Value* assignmentBorrowStorage = nullptr;
            const auto* borrowDestination = namedVar.TypeAndValue.IsArrayView
                && rightNV.Storage != destination ? nullptr : existingDestination;
            if (operatorText == "=" && namedVar.FieldName.empty()
                && llvm::isa<llvm::AllocaInst>(destination)
                && ShouldBorrowPlainPointerBinding(
                    namedVar.TypeAndValue, rightNV, borrowDestination, false))
            {
                const auto* sourceBinding = rightNV.Storage != nullptr
                    ? compiler->FindVariableByStorage(rightNV.Storage)
                    : rightNV.CallerName.empty() ? nullptr
                        : compiler->FindLiveNamedVariable(rightNV.CallerName);
                const std::string sourceName = rightNV.CallerName.empty()
                    ? compiler->FindVariableNameByStorage(rightNV.Storage)
                    : rightNV.CallerName;
                rightNV.IsOwning = false;
                rightNV.BorrowsOwningLocal = true;
                assignmentBorrowsOwningLocal = true;
                assignmentBorrowOrigin = sourceBinding != nullptr
                    && sourceBinding->BorrowsOwningLocal
                    && !sourceBinding->OwningLocalOrigin.empty()
                    ? sourceBinding->OwningLocalOrigin : sourceName;
                assignmentBorrowStorage = sourceBinding != nullptr
                    && sourceBinding->BorrowsOwningLocal
                    && sourceBinding->OwningLocalStorage != nullptr
                    ? sourceBinding->OwningLocalStorage : rightNV.Storage;
                rightNV.OwningLocalOrigin = assignmentBorrowOrigin;
                rightNV.OwningLocalStorage = assignmentBorrowStorage;
                rightNV.OwningLocalBorrowAfterRebind = sourceBinding != nullptr
                    && sourceBinding->PointerRebound;
            }
            /*
             * Hoisted above the rebind block below, which records it: this is the ONLY discriminator
             * that separates "pointed at a fresh owner" from "pointed at another borrow", and
             * PointerRebound alone cannot tell those apart. Full rationale at the `unique T*`
             * reassignment further down, which reads the same bool.
             */
            // An assignment expression over an owned temporary yields its borrowed destination value.
            // Keep the temporary's old identity from making an outer assignment adopt that borrow.
            bool srcIsOwnedPtrRhs = !rightNV.IsBorrowed
                && (AsDirectNew(assignCtx) != nullptr
                    || TopLevelMoveExpression(assignCtx) != nullptr
                    || compiler->IsOwningPtrTempValue(rightNV.Primary)
                    || compiler->IsOwnedNewTemp(rightNV.Primary)
                    || compiler->RawArrayResultOwns(rightNV.Primary)
                    || compiler->IsMovedOutPtrValue(rightNV.Primary));
            bool assignmentAdoptsOwnedSource = false;
            // Reassignment / field store into an array-view: `a = rawIntPtr;` or `s.view = p;`
            // would launder a raw pointer into the noalias contract - reject (decay is one-way).
            if (operatorText == "=")
            {
                // Ruling 2026-10-31: explicit `move` of a PROVEN borrowed pointer into any slot.
                if (rightNV.IsExplicitMove && namedVar.TypeAndValue.Pointer
                    && !namedVar.TypeAndValue.IsArrayView && compiler->MoveSourceIsAddressBorrow(rightNV))
                {
                    compiler->LogBorrowedMoveSlotError();
                    return finishStore(nullptr);
                }
                RejectRawPointerToArrayView(ctx, namedVar.TypeAndValue, rightNV);
                RejectUniqueArrayMoveIntoPointer(ctx, namedVar.TypeAndValue, rightNV);
                // Same door, element axis: `s.view = otherElemView;` strides by the wrong element.
                RejectArrayViewElementMismatch(ctx, namedVar.TypeAndValue, rightNV);
                // Same door, primitive axis: `v = runtimeInt;` reinterprets a number as an address.
                RejectPrimitiveValueIntoArrayView(ctx, namedVar.TypeAndValue, rightNV);
                // Same door, C++ axis: a const C++ object's address into a mutable record pointer slot.
                if (const std::string slotKind = ConstPointeeSlotKind(destination); !slotKind.empty())
                    RejectConstRecordPointeeStore(ctx, namedVar.TypeAndValue, rightNV, slotKind,
                        !namedVar.FieldName.empty() ? namedVar.FieldName
                            : !namedVar.TypeAndValue.VariableName.empty()
                                ? namedVar.TypeAndValue.VariableName : namedVar.CallerName);
                const bool sameScopeStringLocal = NamedVarIsString(namedVar)
                    && namedVar.FieldName.empty() && !isDerefStorage()
                    && !namedVar.CallerName.empty()
                    && llvm::isa<llvm::AllocaInst>(destination)
                    && !StackCharBufferOutlives(rightNV, namedVar);
                if (NamedVarIsString(namedVar) && !sameScopeStringLocal
                    && RejectStackCharBufferEscape(rightNV, ctx))
                    return finishStore(nullptr);
                if (sameScopeStringLocal)
                    compiler->SetStackCharBufferBorrow(
                        namedVar.CallerName,
                        rightNV.StackCharBufferBorrow,
                        rightNV.StackCharBufferSource.empty()
                            ? rightNV.CallerName : rightNV.StackCharBufferSource,
                        rightNV.StackCharBufferScopeDepth);
                // Permanently clear the declaration-time provenance flag on ANY reassignment,
                // regardless of what this RHS is. Recomputing it from THIS RHS would be
                // WALK-ORDER over the AST, not control flow: a reassignment inside one branch
                // must not poison (or clear) a delete reachable only from a different branch -
                // see SetViewOfFixedArrayStorage. Scoped to a plain bare-identifier LHS (field
                // stores are never tracked in the first place).
                if (namedVar.TypeAndValue.IsArrayView && namedVar.FieldName.empty()
                    && !namedVar.CallerName.empty() && namedVar.Storage != nullptr
                    && (llvm::isa<llvm::AllocaInst>(namedVar.Storage)
                        || llvm::isa<llvm::GlobalVariable>(namedVar.Storage)))
                    compiler->SetViewOfFixedArrayStorage(namedVar.CallerName, false);
                /*
                 * q11: a GLOBAL pointer destination (`g = p;`). The block below is gated on
                 * alloca storage and its ledger only walks stackNamedVariable, so a global has
                 * no slot to carry the fact and `delete g` was accepted where the identical
                 * local spelling is rejected. Record it in the per-function global ledger
                 * instead - same-function only, so a program-wide false rejection cannot arise
                 * (see globalAssignBorrowOrigin_). A store that does NOT carry a borrow retires
                 * any earlier fact, so a rebind to fresh memory is not poisoned by it.
                 */
                if (namedVar.TypeAndValue.Pointer && namedVar.FieldName.empty()
                    && namedVar.Storage != nullptr
                    && llvm::isa<llvm::GlobalVariable>(namedVar.Storage))
                {
                    // Keyed on the STORAGE, not the name: GetGlobalVariableNV records no
                    // CallerName, so a global arrives unnamed at both this store and the delete.
                    const std::string globalBorrowOrigin = rightNV.BorrowedOrigin.empty()
                        ? rightNV.CallerName : rightNV.BorrowedOrigin;
                    if (rightNV.IsBorrowed && !rightNV.TypeAndValue.IsMove
                        && !globalBorrowOrigin.empty())
                        compiler->globalAssignBorrowOrigin_[namedVar.Storage] = globalBorrowOrigin;
                    else
                        compiler->globalAssignBorrowOrigin_.erase(namedVar.Storage);
                    // Address-of provenance for the same global destination (`g = &x;`). Any
                    // other RHS retires it, exactly as the origin ledger above is retired.
                    if (compiler->IsBorrowedAddressValue(rightNV.Primary)
                        || rightNV.PointsToBorrowedAddress)
                        compiler->globalAssignBorrowedAddress_.insert(namedVar.Storage);
                    else
                        compiler->globalAssignBorrowedAddress_.erase(namedVar.Storage);
                }
                // Retire the declaration-time "someone else frees this" facts about a POINTER
                // binding that has just been pointed elsewhere - UNLESS the RHS binding proves an
                // owner of its own, in which case the proof is carried across. `p = q;` between two
                // borrowed parameters leaves p a borrow; retiring there laundered a double free.
                if (namedVar.TypeAndValue.Pointer && namedVar.FieldName.empty()
                    && !namedVar.CallerName.empty() && namedVar.Storage != nullptr
                    && llvm::isa<llvm::AllocaInst>(namedVar.Storage))
                {
                    /*
                     * '??=' is itself a JOIN: afterwards the binding holds either its OLD referent
                     * (arm not taken) or the RHS's (arm taken). So the fact survives only when BOTH
                     * sides prove an owner, and the diagnostic then names both - mirroring the
                     * '?:' / '??' rule rather than either side alone. Taking the RHS alone
                     * false-rejects `c = new Ci(); c ??= q;`; taking neither laundered `p ??= q`
                     * between two borrowed parameters, which the raw `delete p;` rejects.
                     */
                    // TransferPointerOwnershipOnStore consumes an owning pointer source later in
                    // this assignment. Do not snapshot that source's pre-transfer owner proof:
                    // the destination is the sole owner by the end of this statement.
                    // A stack or global address is never heap-owned, even when its object owns its
                    // elements: `raw = arr;` of a destructible array is a borrow of the array. Views
                    // keep their own provenance (SetViewOfFixedArrayStorage), so they are exempt.
                    const bool rhsIsStackAddress = !namedVar.TypeAndValue.IsArrayView
                        && LLVMBackend::IsProvableNonHeapAddressSource(rightNV);
                    bool reboundToOwnedStore = coalesceResume == nullptr
                        && !assignmentBorrowsOwningLocal
                        && (srcIsOwnedPtrRhs || rightNV.IsOwning)
                        && !rhsIsStackAddress
                        && !compiler->IsMovedBorrowedPtrValue(rightNV.Primary);
                    assignmentAdoptsOwnedSource = coalesceResume == nullptr
                        && !assignmentBorrowsOwningLocal
                        && srcIsOwnedPtrRhs
                        && !compiler->IsMovedBorrowedPtrValue(rightNV.Primary);
                    std::string assignedOwner = reboundToOwnedStore
                        ? std::string() : DescribeAssignedSourceOwner(rightNV);
                    // A cast erases the RHS binding's Storage; the boxed VALUE still names the slot,
                    // which is what the pre-tail '??=' handler asked. Coalesce path only.
                    if (coalesceResume != nullptr && assignedOwner.empty()
                        && ProvingBindingForBoxedSource(rightNV.Primary, nullptr) != nullptr)
                        assignedOwner = DescribeBoxedSourceOwner(rightNV.Primary, nullptr);
                    if (coalesceResume != nullptr)
                    {
                        std::string joined;
                        if (!coalesceLhsOwner.empty() && !assignedOwner.empty())
                        {
                            std::vector<std::string> owners{coalesceLhsOwner};
                            if (assignedOwner != coalesceLhsOwner) owners.push_back(assignedOwner);
                            joined = DescribeInterfaceBoxOwners(owners);
                        }
                        // '??=' keeps the OLD referent when the arm is not taken, so no store
                        // through it can prove the binding now holds an owner - never retire.
                        compiler->MarkPointerRebound(namedVar.CallerName, joined,
                                                     /*coalesceJoin*/ true,
                                                     /*reboundToOwnedValue*/ false);
                    }
                    else
                    {
                        /*
                         * srcIsOwnedPtrRhs minus its purely SYNTACTIC leg. `b = move p` off a BORROW
                         * carries the `move` token and owns nothing - ParseMoveExpression ledgers it
                         * in movedBorrowedPtrValues_, not movedOutPtrValues_ - so trusting the token
                         * here retired the borrow proof on a value that transfers nothing. Only the
                         * value-identity legs may prove ownership for a RETIREMENT; narrowed here
                         * rather than in srcIsOwnedPtrRhs, whose other consumer sits behind its own
                         * IsBorrowed gate and must keep the syntactic leg for a genuine transfer.
                         */
                        compiler->MarkPointerRebound(namedVar.CallerName,
                                                     reboundToOwnedStore ? std::string() : assignedOwner,
                                                     /*coalesceJoin*/ false, reboundToOwnedStore);
                    }
                    compiler->SetPointsToBorrowedByValueParam(
                        namedVar.CallerName, rightNV.PointsToBorrowedByValueParam);
                    // Assignment twin of the declaration's address-of provenance: `p = &x;` makes
                    // the binding a proven borrow, any other RHS retires the fact (a stale borrow
                    // proof would be a false rejection at a later '?:' join).
                    compiler->SetPointsToBorrowedAddress(
                        namedVar.CallerName,
                        compiler->IsBorrowedAddressValue(rightNV.Primary)
                            || rightNV.PointsToBorrowedAddress);
                    compiler->SetStackCharBufferBorrow(
                        namedVar.CallerName,
                        rightNV.StackCharBufferBorrow,
                        rightNV.StackCharBufferSource.empty()
                            ? rightNV.CallerName : rightNV.StackCharBufferSource,
                        rightNV.StackCharBufferScopeDepth);
                    // C++ pointee const is sticky like a `const P*` declaration: an assignment
                    // may set it, never clear it (a clear under a branch leaked to the join).
                    if (operatorText == "=" && namedVar.FieldName.empty()
                        && namedVar.TypeAndValue.Pointer && !namedVar.CallerName.empty()
                        && rightNV.TypeAndValue.PointeeConstBit() && rightNV.TypeAndValue.Pointer)
                        if (auto* local = compiler->FindLiveNamedVariable(namedVar.CallerName))
                            local->TypeAndValue.SetPointeeConstBit(true);
                    /*
                     * Record the borrow the RHS carries, which MarkPointerRebound above does not:
                     * it retires the declaration-time facts and re-arms only the OWNER-side proofs,
                     * so a local that BECOMES a borrow by '=' was untracked and `delete` through it
                     * double-freed. Mirrors the declaration path's srcIsBorrowed clause (including
                     * its Trap-B `unique`-field leg), so `d = p;` and `T* d = p;` reject alike.
                     */
                    std::string storeBorrowOrigin = rightNV.BorrowedOrigin.empty()
                        ? rightNV.CallerName : rightNV.BorrowedOrigin;
                    std::string storeBorrowUniqueField = rightNV.BorrowedUniqueField;
                    bool storeSrcIsBorrow = rightNV.IsBorrowed;
                    // Trap B: a plain copy of a `unique` field borrows the field - its synthesized
                    // destructor is still the owner. `move b.p` extracts instead and carries IsMove.
                    if (!storeSrcIsBorrow && !rightNV.TypeAndValue.IsMove
                        && ((rightNV.TypeAndValue.IsUnique
                                && rightNV.TypeAndValue.Pointer
                                && rightNV.Storage != nullptr)
                            || rightNV.IsUniqueFieldAlias))
                    {
                        storeSrcIsBorrow = true;
                        storeBorrowUniqueField = DescribeUniqueFieldOwner(rightNV);
                        storeBorrowOrigin = DescribeUniqueFieldAccess(rightNV);
                    }
                    // Trap B through a CALL, mirroring the declaration path: a callee that provably
                    // returns a live `unique` field hands this store a borrow of that field.
                    bool storeBorrowUniqueFieldViaCall = rightNV.BorrowedUniqueFieldViaCall;
                    if (!storeSrcIsBorrow && !rightNV.TypeAndValue.IsMove
                        && rightNV.TypeAndValue.Pointer)
                    {
                        const auto* fieldBorrow =
                            compiler->FindUniqueFieldBorrowResult(rightNV.Primary);
                        if (fieldBorrow != nullptr)
                        {
                            storeSrcIsBorrow = true;
                            storeBorrowUniqueField = fieldBorrow->FieldOwner;
                            storeBorrowOrigin = fieldBorrow->FieldOwner;
                            storeBorrowUniqueFieldViaCall = true;
                        }
                    }
                    // A stack or global address (`raw = &loc;`, `raw = arr;`) is a borrow too: the
                    // raw local never owns it, so a later reset must refuse it like a borrowed
                    // parameter. The root object's name is the origin the message names.
                    const bool storeSrcIsStackAddress = rhsIsStackAddress;
                    if (!storeSrcIsBorrow && !rightNV.TypeAndValue.IsMove
                        && (storeSrcIsStackAddress || rightNV.PointsToBorrowedAddress))
                    {
                        storeSrcIsBorrow = true;
                        if (storeBorrowOrigin.empty() && storeSrcIsStackAddress)
                        {
                            llvm::Value* root = rightNV.Primary != nullptr
                                ? rightNV.Primary->stripPointerCasts() : rightNV.Storage;
                            while (auto* gep = llvm::dyn_cast<llvm::GEPOperator>(root))
                                root = gep->getPointerOperand()->stripPointerCasts();
                            storeBorrowOrigin = compiler->FindVariableNameByStorage(root);
                            if (storeBorrowOrigin.empty())
                                storeBorrowOrigin = root->getName().str();
                        }
                    }
                    if (storeSrcIsBorrow && !reboundToOwnedStore)
                        compiler->RecordAssignBorrow(namedVar.CallerName, storeBorrowOrigin,
                                                     storeBorrowUniqueField,
                                                     !rightNV.FieldName.empty(),
                                                     /*keepExistingOrigin*/ coalesceResume != nullptr,
                                                     storeBorrowUniqueFieldViaCall);
                    else if (assignmentBorrowsOwningLocal && !reboundToOwnedStore)
                        compiler->RecordAssignOwningLocalBorrow(
                            namedVar.CallerName, assignmentBorrowOrigin, assignmentBorrowStorage);
                    else if (reboundToOwnedStore)
                        storeRetiredBorrow = compiler->RetireAssignBorrow(
                            namedVar.CallerName, assignStartBlock);
                    // A JOIN RHS carries no source binding, so no clause above can see it; ask its
                    // arms directly. After MarkPointerRebound, which retires any earlier join.
                    std::vector<llvm::Value*> storeJoinSlots;
                    std::string storeJoinOwner =
                        JoinArmsKeepOwner(rightNV.Primary, &storeJoinSlots);
                    compiler->SetJoinKeepsOwner(namedVar.CallerName, storeJoinOwner, storeJoinSlots);
                }
            }
            // An assignment to an existing variable (not a declaration) does not transfer
            // ownership - consuming lastOwningResult here would be wrong (the new-allocated
            // value is either moved via items.add(move p) or managed by the caller).
            // Reset the flag so it doesn't leak into the next declaration in this scope.
            // The over-alignment tag rides the same signal and must be reset with it, or a
            // later owning declaration adopts it and aligned-frees an ordinary block.
            // Capture whether the RHS is an OWNED source (direct `new`, a move-returning call,
            // or a `move` expression) before the flags are cleared: the D5 assignment reject for
            // a `unique` interface local (below, after the upcast) needs it to tell a heap-owned
            // source from a stack box / borrow.
            // lastOwningResult is a sticky global that a `new` ANYWHERE in the RHS sets, including
            // as a call argument, so `k = pick(g, new Sq())` adopted a borrow (double-free). Trust
            // it only when the RHS itself is a direct `new` or a top-level `move` - it stays the
            // ownership signal there, since a `move` of a BORROWED interface never sets it.
            // The value-identity leg (IsOwnedNewTemp) sees an owning `new` that reached the RESULT
            // through a transparent wrapper (a '?:' arm); a `new` in ARGUMENT position never does.
            bool srcIsOwnedForUniqueIface =
                (compiler->lastOwningResult
                    && (AsDirectNew(assignCtx) != nullptr || TopLevelMoveExpression(assignCtx) != nullptr))
                || compiler->lastCallReturnsOwned
                || compiler->IsOwnedNewTemp(rightNV.Primary)
                || rightNV.TypeAndValue.IsMove;
            bool topLevelMoveOwnsSource = false;
            if (auto* move = TopLevelMoveExpression(assignCtx))
            {
                std::string sourceName = move->unaryExpression() != nullptr
                    ? move->unaryExpression()->getText() : std::string();
                if (auto* source = compiler->FindLiveNamedVariable(sourceName))
                    topLevelMoveOwnsSource = source->IsOwning
                        || source->TypeAndValue.IsUnique;
            }
            bool copiedOwnerWasRebound = false;
            if (rightNV.BorrowsOwningLocal && rightNV.OwningLocalStorage != nullptr)
            {
                auto* source = compiler->FindVariableByStorage(rightNV.OwningLocalStorage);
                    copiedOwnerWasRebound = rightNV.PointerRebound
                        || (source != nullptr && source->PointerRebound);
            }
            bool srcIsOwnedForCoreUnique =
                (compiler->lastOwningResult
                    && (AsDirectNew(assignCtx) != nullptr || TopLevelMoveExpression(assignCtx) != nullptr))
                || compiler->lastCallReturnsOwned
                || compiler->IsOwnedNewTemp(rightNV.Primary)
                || compiler->IsOwningPtrTempValue(rightNV.Primary)
                || rightNV.IsOwning || (rightNV.IsExplicitMove && !rightNV.IsBorrowed)
                || topLevelMoveOwnsSource
                || (TopLevelMoveExpression(assignCtx) != nullptr && !rightNV.IsBorrowed)
                || compiler->IsMovedOutPtrValue(rightNV.Primary)
                || compiler->BorrowProofRetiredByRebind(rightNV)
                || copiedOwnerWasRebound
                || rightNV.TypeAndValue.IsMove;
            // Same question for a `unique T*` LOCAL reassignment, but answered ONLY from
            // properties of THIS RHS. lastOwningResult is deliberately excluded: it is a sticky
            // global that a `new` anywhere in the RHS sets - including as a call ARGUMENT - so
            // `b = addr(new R())` would inherit it and adopt a pointer the call merely borrowed
            // (freeing a global). The syntactic tests below see the RHS shape itself, and
            // lastCallReturnsOwned is derived from the resolved callee's declared `move`/unique
            // return type, so an ordinary borrow-returning call cannot fake ownership.
            // The IsOwnedNewTemp leg asks the same question BY VALUE IDENTITY instead of by shape,
            // so a `new` that reached the RESULT through a '?:' arm adopts; one in ARGUMENT
            // position produces a different value (the borrow-returning call's result) and does not.
            // The owning-RETURN leg is asked BY VALUE IDENTITY too (IsOwningPtrTempValue), not from
            // the bare lastCallReturnsOwned side-channel: that flag fires for any owning call in the
            // RHS, including an unselected '?:' arm, so a suppressed mixed join adopted a borrow.
            // The IsMovedOutPtrValue leg keeps this in step with the DECLARATION path: a non-mixed
            // '?:' join of `move` arms owns, but is in no other ledger, so `b = c ? move a : nullptr;`
            // left the moved-out object owned by nobody (leak). A MIXED join never reaches the
            // ledger, so it still borrows here.
            compiler->lastOwningResult = false;
            compiler->lastAllocAlignment = 0;
            compiler->lastCallReturnsAllocAlign = 0;
            auto right = LoadNamedVariable(rightNV);
            assignmentRhsValue = right;
            // Trivially-copyable imported records still have declared C++ assignment operators.
            // Use the nontrivial-class selector before aggregate storage rejects a scalar RHS.
            if (operatorText == "=" && destination != nullptr
                && !namedVar.TypeAndValue.Pointer
                && compiler->IsCxxRecord(namedVar.TypeAndValue.TypeName)
                && !compiler->IsForeignNontrivialCxxClass(namedVar.TypeAndValue.TypeName)
                && (rightNV.TypeAndValue.Pointer
                    || rightNV.TypeAndValue.TypeName != namedVar.TypeAndValue.TypeName))
            {
                compiler->EnsureCxxMemberProjected(
                    namedVar.TypeAndValue.TypeName, "operator=");
                std::string refusal;
                LLVMBackend::TypeAndValue selectedReturn;
                const auto tempMark = compiler->MarkOwnedTemps();
                if (auto* direct = TryDirectCxxAssignOperator(
                        ctx, namedVar, namedVar.TypeAndValue.TypeName, rightNV, refusal,
                        /*probeOnly*/ false, &selectedReturn))
                {
                    if (assignmentResult == nullptr)
                    {
                        compiler->FlushOwnedTempsSince(tempMark, nullptr, nullptr);
                        compiler->lastCxxRetTemp_ = nullptr;
                        compiler->lastCxxRetValue_ = nullptr;
                    }
                    auto* assignmentSlot = CxxAssignSretScope::TargetFor(
                        compiler, namedVar.TypeAndValue.TypeName);
                    auto* value = CxxAssignOperatorValue(ctx, namedVar,
                        namedVar.TypeAndValue.TypeName, direct, selectedReturn,
                        assignmentResult, assignmentSlot);
                    if (assignmentSlot != nullptr)
                    {
                        compiler->lastCxxRetTemp_ = nullptr;
                        compiler->lastCxxRetValue_ = nullptr;
                    }
                    return assignmentResult == nullptr ? nullptr : value;
                }
                if (!refusal.empty()) LogErrorContext(ctx, refusal);
            }
            if (copiedOwnerWasRebound && rightNV.BorrowsOwningLocal
                && rightNV.OwningLocalStorage != nullptr)
            {
                const auto* source = compiler->FindVariableByStorage(rightNV.OwningLocalStorage);
                if (source != nullptr
                    && (source->Primary == right || rightNV.OwningLocalBorrowAfterRebind))
                {
                    copiedOwnerWasRebound = false;
                    if (rightNV.OwningLocalBorrowAfterRebind && !rightNV.IsOwning
                        && !rightNV.TypeAndValue.IsMove)
                        srcIsOwnedForCoreUnique = false;
                }
            }
            bool coreUniqueAlignedDestination = operatorText == "="
                && !namedVar.TypeAndValue.Pointer
                && compiler->IsCoreUniqueType(namedVar.TypeAndValue.TypeName)
                && (llvm::isa_and_nonnull<llvm::GetElementPtrInst>(destination)
                    || llvm::isa_and_nonnull<llvm::GlobalVariable>(destination));
            if (coreUniqueAlignedDestination
                && RejectFieldAllocAlignMismatch(
                    namedVar.TypeAndValue, namedVar.TypeAndValue.AllocAlignValue,
                    rightNV, right,
                    namedVar.FieldName.empty()
                        ? std::string("a field")
                        : std::format("field '{}'", namedVar.FieldName),
                    ctx))
                return finishStore(right);
            srcIsOwnedForCoreUnique = srcIsOwnedForCoreUnique
                || compiler->IsMovedOutPtrValue(right);

            // A desugared unique<IFace> destination resets from an interface VALUE. Convert the
            // pointer-shaped assignment forms here, before the shared reset path below, so the
            // destination gets the same ownership-taking box as declaration initialization.
            bool coreUniqueInterfaceResetValue = false;
            if (operatorText == "=" && right != nullptr
                && compiler->IsCoreUniqueType(namedVar.TypeAndValue.TypeName)
                && !namedVar.TypeAndValue.Pointer
                && compiler->IsInterfaceType(MangledGenericArgument(
                    *compiler, namedVar.TypeAndValue.TypeName)))
            {
                const std::string interfaceName = MangledGenericArgument(
                    *compiler, namedVar.TypeAndValue.TypeName);
                bool sourceOwnsInterface = srcIsOwnedForCoreUnique;
                if (right->getType()->isPointerTy())
                {
                    if (auto* constant = llvm::dyn_cast<llvm::Constant>(right);
                        constant != nullptr && constant->isNullValue())
                        right = llvm::Constant::getNullValue(compiler->GetFatPtrType());
                    else if (sourceOwnsInterface)
                    {
                        std::string armFailure;
                        std::string joinSpelling = "?:";
                        if (rightNV.TypeAndValue.TypeName.empty())
                            right = UpcastPointerJoinToInterface(
                                right, interfaceName, &armFailure, &joinSpelling,
                                /*transferArmOwnership*/ true);
                        else if (compiler->StructImplementsInterface(
                                     rightNV.TypeAndValue.TypeName, interfaceName))
                            right = BoxConcreteIntoInterface(
                                ctx, right, rightNV.TypeAndValue.Pointer,
                                rightNV.TypeAndValue.TypeName, interfaceName, &rightNV,
                                /*adoptsOwnership*/ true);
                        if (right == nullptr && !armFailure.empty())
                            LogErrorContext(ctx, std::format(
                                "cannot convert '{}' arm to interface '{}': {}",
                                joinSpelling, SpellType(*compiler,
                                    LLVMBackend::TypeAndValue{ .TypeName = interfaceName }), armFailure));
                    }
                }
                else if (right->getType() == compiler->GetFatPtrType())
                {
                    sourceOwnsInterface = rightNV.IsOwning
                        || rightNV.IsExplicitMove
                        || rightNV.TypeAndValue.IsMove
                        || compiler->lastCallReturnsOwned
                        || compiler->FindOwnedReturnEntry(rightNV.Primary) != nullptr;
                    if (sourceOwnsInterface)
                    {
                        std::string sourceInterface = compiler->ResolveFatInterfaceSrcName(
                            right, rightNV.TypeAndValue.IsInterface
                                ? rightNV.TypeAndValue.TypeName : std::string());
                        right = compiler->ReboxInterfaceIfNeeded(
                            right, sourceInterface, interfaceName);
                    }
                }
                if (right != nullptr && right->getType() == compiler->GetFatPtrType()
                    && sourceOwnsInterface)
                {
                    rightNV.Primary = right;
                    rightNV.Storage = nullptr;
                    rightNV.BaseType = right->getType();
                    rightNV.TypeAndValue = {};
                    rightNV.TypeAndValue.TypeName = interfaceName;
                    rightNV.TypeAndValue.IsInterface = true;
                    rightNV.TypeAndValue.IsMove = true;
                    rightNV.IsOwning = true;
                    rightNV.IsBorrowed = false;
                    coreUniqueInterfaceResetValue = true;
                    srcIsOwnedForCoreUnique = true;
                }
            }
            // A core unique value assigned to a builtin unique pointer field transfers its
            // pointee, so expose the wrapper's _p slot to the existing field ownership path.
            // A unique<T>[N] source decays to the address of element 0 (as `&a[0]`), never a value.
            if (operatorText == "=" && right != nullptr
                && compiler->IsCoreUniqueType(rightNV.TypeAndValue.TypeName)
                && !rightNV.TypeAndValue.Pointer && namedVar.TypeAndValue.Pointer
                && namedVar.TypeAndValue.IsUnique && rightNV.TypeAndValue.ConstArraySize == 0)
            {
                // A moved wrapper field has already been nulled by ParseMoveExpression. Preserve
                // that ownership transfer when lowering it to the builtin pointer field path.
                bool movedCoreUnique = namedVar.FieldName.empty()
                    || rightNV.TypeAndValue.IsMove || rightNV.IsExplicitMove;
                auto* uniqueStorage = rightNV.Storage;
                if (uniqueStorage == nullptr)
                {
                    uniqueStorage = compiler->CreateAlloca(right->getType());
                    compiler->builder->CreateStore(right, uniqueStorage);
                }
                const auto& uniqueData = compiler->GetDataStructure(rightNV.TypeAndValue.TypeName);
                for (size_t i = 0; i < uniqueData.StructFields.size(); i++)
                    if (uniqueData.StructFields[i].VariableName == "_p")
                    {
                        auto* pointerField = compiler->CreateStructGEP(
                            right->getType(), uniqueStorage, (uint32_t)i);
                        right = compiler->CreateLoad(pointerField);
                        rightNV.Storage = pointerField;
                        rightNV.BaseType = right->getType();
                        break;
                    }
                if (right != nullptr)
                {
                    if (movedCoreUnique && rightNV.Storage != nullptr)
                    {
                        compiler->builder->CreateStore(
                            llvm::Constant::getNullValue(rightNV.BaseType), rightNV.Storage);
                        if (!rightNV.CallerName.empty())
                            compiler->MarkVariableExplicitlyMovedNull(rightNV.CallerName);
                    }
                    rightNV.TypeAndValue.TypeName = MangledGenericArgument(
                        *compiler, rightNV.TypeAndValue.TypeName);
                    rightNV.TypeAndValue.Pointer = true;
                    rightNV.TypeAndValue.IsUnique = false;
                    rightNV.TypeAndValue.IsMove = movedCoreUnique;
                    rightNV.IsOwning = true;
                    rightNV.IsExplicitMove = false;
                    rightNV.Primary = right;
                }
            }

            // A core unique value borrows its pointee when assigned to an ordinary raw pointer.
            // Keep this implicit conversion at the store site so fields and locals receive the
            // same observer semantics as plain call arguments.
            if (operatorText == "=" && right != nullptr
                && compiler->IsCoreUniqueType(rightNV.TypeAndValue.TypeName)
                && !rightNV.TypeAndValue.Pointer && namedVar.TypeAndValue.Pointer
                && !namedVar.TypeAndValue.IsUnique && rightNV.TypeAndValue.ConstArraySize == 0)
            {
                // Read the wrapper's pointer field directly. The ordinary `get()` overload is
                // equivalent, but its generated helper can be re-entered while pending generic
                // instantiations are being drained for interface-bearing pointee types.
                auto* uniqueStorage = rightNV.Storage;
                if (uniqueStorage == nullptr)
                {
                    uniqueStorage = compiler->CreateAlloca(right->getType());
                    compiler->builder->CreateStore(right, uniqueStorage);
                }
                const auto& uniqueData = compiler->GetDataStructure(rightNV.TypeAndValue.TypeName);
                for (size_t i = 0; i < uniqueData.StructFields.size(); i++)
                    if (uniqueData.StructFields[i].VariableName == "_p")
                    {
                        auto* pointerField = compiler->CreateStructGEP(
                            right->getType(), uniqueStorage, (uint32_t)i);
                        right = compiler->CreateLoad(pointerField);
                        break;
                    }
                if (right != nullptr)
                {
                    auto pointeeName = MangledGenericArgument(
                        *compiler, rightNV.TypeAndValue.TypeName);
                    rightNV.TypeAndValue.TypeName = pointeeName;
                    rightNV.TypeAndValue.Pointer = true;
                    rightNV.TypeAndValue.IsUnique = false;
                    rightNV.TypeAndValue.IsMove = false;
                    rightNV.IsOwning = false;
                    rightNV.IsExplicitMove = false;
                    rightNV.IsBorrowed = true;
                    rightNV.BorrowedOrigin = rightNV.CallerName;
                    rightNV.Primary = right;
                    rightNV.BaseType = right->getType();
                }
            }

            // A core unique value assigned from a raw owning expression is reset in place. The
            // value-to-value case remains on the normal owning-struct move path below.
            if (operatorText == "=" && right != nullptr
                && compiler->IsCoreUniqueType(namedVar.TypeAndValue.TypeName)
                && !namedVar.TypeAndValue.Pointer
                && (right->getType()->isPointerTy() || coreUniqueInterfaceResetValue))
            {
                auto* destinationGep = llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(
                    namedVar.Storage);
                auto* destinationStruct = destinationGep == nullptr ? nullptr
                    : llvm::dyn_cast<llvm::StructType>(destinationGep->getSourceElementType());
                const bool destinationIsField = !namedVar.FieldName.empty()
                    || namedVar.IsInterfaceField || namedVar.IsElementAccess
                    || (destinationStruct != nullptr && destinationStruct->hasName()
                        && !compiler->IsCoreUniqueType(destinationStruct->getName().str()));
                if (destinationIsField)
                {
                    const std::string fieldName = namedVar.FieldName.empty()
                        ? namedVar.TypeAndValue.VariableName : namedVar.FieldName;
                    RejectRawHeapArrayIntoUniqueField(
                        rightNV, namedVar.TypeAndValue, fieldName, ctx);
                }
                const bool uniquePointerJoinOwns = !destinationIsField
                    && !namedVar.TypeAndValue.IsArrayView
                    && !compiler->IsInterfaceType(MangledGenericArgument(
                        *compiler, namedVar.TypeAndValue.TypeName))
                    && right->getType()->isPointerTy()
                    && compiler->CanAdoptUniquePointerJoin(right);
                if (uniquePointerJoinOwns)
                    srcIsOwnedForCoreUnique = true;

                const std::string borrowedParamCall = BorrowedParameterCallName(
                    rightNV, rightNV.Primary);
                if (!borrowedParamCall.empty())
                {
                    const std::string uniqueType = SpellType(*compiler, namedVar.TypeAndValue);
                    const std::string destName = namedVar.CallerName.empty()
                        ? namedVar.TypeAndValue.VariableName : namedVar.CallerName;
                    if (destinationIsField)
                        LogErrorContext(ctx, std::format(
                            "cannot assign borrowed value '{}' to unique field '{}' - the source "
                            "still owns it; use 'new', a 'move' expression, or a move-returning call",
                            borrowedParamCall, DescribeUniqueFieldOwner(namedVar)));
                    else
                        LogErrorContext(ctx, std::format(
                            "cannot assign {} '{}' from borrowed value '{}' - the source "
                            "still owns it; use 'new', a 'move' expression, or a move-returning call",
                            uniqueType, destName, borrowedParamCall));
                    return finishStore(right);
                }

                // Assignment is declaration for a call whose return borrows a GLOBAL (`return gRaw;`):
                // refused here, in a '?:' arm too. A borrowed parameter or plain field stays adopted.
                // The callee summary is parse-tree based, so definition order does not matter.
                auto borrowedGlobalCallName = [&](llvm::Value* value) -> std::string {
                    auto* call = llvm::dyn_cast_or_null<llvm::CallBase>(value);
                    const llvm::Function* callee = call != nullptr ? call->getCalledFunction() : nullptr;
                    const auto* symbol = callee == nullptr ? nullptr : compiler->FindSymbolForFunction(callee);
                    if (symbol == nullptr || symbol->IsCxx || !symbol->ReturnType.Pointer
                        || symbol->ReturnsOwned || symbol->ReturnsAlias
                        || symbol->ReturnsAliasOfByValueParam || symbol->ReturnType.IsAlias
                        || compiler->TernaryArmJoinsOwning(value)
                        || !symbol->ReturnsGlobalBorrow)
                        return {};
                    // A generic instance's SourceName carries its '$' type arguments; show the base name.
                    return std::string(MangledBase(symbol->SourceName));
                };
                std::string borrowedGlobalName;
                if (!destinationIsField)
                {
                    if (auto* joinPhi = llvm::dyn_cast_or_null<llvm::PHINode>(right))
                        for (unsigned i = 0; borrowedGlobalName.empty() && i < joinPhi->getNumIncomingValues(); ++i)
                            borrowedGlobalName = borrowedGlobalCallName(joinPhi->getIncomingValue(i));
                    else if (!rightNV.IsBorrowed && !rightNV.IsAliasBorrow && !rightNV.IsOwning)
                        borrowedGlobalName = borrowedGlobalCallName(rightNV.Primary);
                }
                if (!borrowedGlobalName.empty())
                {
                    std::string uniqueType = SpellType(*compiler, namedVar.TypeAndValue);
                    while (uniqueType.ends_with("*")) uniqueType.pop_back();
                    const std::string destName = namedVar.CallerName.empty()
                        ? namedVar.TypeAndValue.VariableName : namedVar.CallerName;
                    LogErrorContext(ctx, std::format(
                        "cannot initialize {} '{}' from borrowed value '{}' - the source still owns it; "
                        "use 'new', a 'move' expression, or a move-returning call",
                        uniqueType, destName, borrowedGlobalName));
                    return finishStore(right);
                }
                const bool sourceIsUniqueField = IsUniqueFieldRead(rightNV);
                auto consumePlainBorrowedOwner = [&]() -> bool {
                    const LLVMBackend::NamedVariable* liveSource = rightNV.CallerName.empty()
                        ? nullptr : compiler->FindLiveNamedVariable(rightNV.CallerName);
                    const LLVMBackend::NamedVariable* owner = nullptr;
                    llvm::Value* ownerStorage = nullptr;
                    std::string ownerName;
                    auto selectOwner = [&](const LLVMBackend::NamedVariable* candidate,
                                           const std::string& candidateName) {
                        if (candidate == nullptr || !candidate->IsOwning)
                            return;
                    if (candidate->PointerRebound && !rightNV.OwningLocalBorrowAfterRebind
                            && candidate->Primary != right
                            && candidate->Primary != rightNV.Primary)
                            return;
                        owner = candidate;
                        ownerStorage = candidate->Storage;
                        ownerName = candidateName;
                    };
                    if (liveSource != nullptr && liveSource->BorrowsOwningLocal
                        && liveSource->OwningLocalStorage != nullptr
                        && (!compiler->BorrowProofRetiredByRebind(*liveSource)
                            || liveSource->OwningLocalBorrowAfterRebind))
                        selectOwner(compiler->FindVariableByStorage(
                            liveSource->OwningLocalStorage),
                            compiler->FindVariableNameByStorage(liveSource->OwningLocalStorage));
                    if (owner == nullptr && liveSource != nullptr
                        && liveSource->BorrowsOwningLocal
                        && !liveSource->OwningLocalOrigin.empty()
                        && (!compiler->BorrowProofRetiredByRebind(*liveSource)
                            || liveSource->OwningLocalBorrowAfterRebind))
                        selectOwner(compiler->FindLiveNamedVariable(liveSource->OwningLocalOrigin),
                                    liveSource->OwningLocalOrigin);
                    if (owner == nullptr && liveSource != nullptr)
                        selectOwner(liveSource, rightNV.CallerName);
                    if (owner == nullptr && rightNV.BorrowsOwningLocal
                        && rightNV.OwningLocalStorage != nullptr
                        && (!compiler->BorrowProofRetiredByRebind(rightNV)
                            || rightNV.OwningLocalBorrowAfterRebind))
                        selectOwner(compiler->FindVariableByStorage(
                            rightNV.OwningLocalStorage),
                            compiler->FindVariableNameByStorage(rightNV.OwningLocalStorage));
                    if (owner == nullptr && rightNV.BorrowsOwningLocal
                        && !rightNV.OwningLocalOrigin.empty()
                        && (!compiler->BorrowProofRetiredByRebind(rightNV)
                            || rightNV.OwningLocalBorrowAfterRebind))
                        selectOwner(compiler->FindLiveNamedVariable(rightNV.OwningLocalOrigin),
                                    rightNV.OwningLocalOrigin);
                    if (owner == nullptr && rightNV.Storage != nullptr)
                        selectOwner(compiler->FindVariableByStorage(rightNV.Storage),
                                    compiler->FindVariableNameByStorage(rightNV.Storage));
                    if (owner == nullptr || ownerName.empty())
                    {
                        return false;
                    }
                    if (ownerStorage != nullptr
                        && (llvm::isa<llvm::AllocaInst>(ownerStorage)
                            || llvm::isa<llvm::GlobalVariable>(ownerStorage)))
                    {
                        auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(owner->BaseType);
                        if (ptrTy == nullptr) return false;
                        compiler->builder->CreateStore(
                            llvm::ConstantPointerNull::get(ptrTy), ownerStorage);
                    }
                    else
                        compiler->SetVariableOwning(ownerName, false);
                    compiler->MarkVariableMoved(ownerName);
                    compiler->ConsumeOwnedNewTemp(owner->Primary != nullptr
                                                      ? owner->Primary : rightNV.Primary);
                    rightNV.IsOwning = true;
                    rightNV.IsBorrowed = false;
                    rightNV.BorrowsOwningLocal = false;
                    rightNV.BorrowedOrigin.clear();
                    rightNV.OwningLocalOrigin.clear();
                    rightNV.OwningLocalStorage = nullptr;
                    return true;
                };
                if (destinationIsField && !sourceIsUniqueField
                    && (rightNV.AssignBorrowBlock != nullptr
                        || rightNV.OwningLocalBorrowAfterRebind)
                    && (!srcIsOwnedForCoreUnique
                        || (rightNV.IsOwning && rightNV.Storage == nullptr))
                    && consumePlainBorrowedOwner())
                    srcIsOwnedForCoreUnique = true;
                if (sourceIsUniqueField)
                {
                    if (RejectConsumeOfBorrowedByValueParamField(compiler, rightNV, ctx))
                        return finishStore(right);
                    if (RejectJoinedSourceIntoUniqueField(compiler, rightNV, ctx))
                        return finishStore(right);
                    EmitImplicitUniqueFieldMove(rightNV, right, nullptr, {}, ctx);
                    rightNV.IsOwning = true;
                    rightNV.IsBorrowed = false;
                    rightNV.BorrowedOrigin.clear();
                }
                if (LLVMBackend::IsProvableNonHeapAddress(right))
                {
                    // A slot of a desugared `unique T* f[N]` keeps the ARRAY wording the builtin
                    // array leg gives, so the element is still named as an element.
                    std::string slot = namedVar.FieldName.empty() && !namedVar.CallerName.empty()
                        ? namedVar.CallerName : DescribeUniqueFieldOwner(namedVar);
                    RejectNonHeapAddressIntoUnique(
                        namedVar.IsElementAccess
                            ? std::format("an element of unique array '{}'", slot)
                            : destinationIsField
                                ? std::format("unique field '{}'", DescribeUniqueFieldOwner(namedVar))
                                : std::format("unique local '{}'", namedVar.CallerName), ctx);
                    return finishStore(right);
                }
                if (destinationIsField)
                {
                    std::string field = DescribeUniqueFieldOwner(namedVar);
                    std::string movedBorrowOrigin;
                    if (compiler->IsMovedBorrowedPtrValue(rightNV.Primary, &movedBorrowOrigin))
                    {
                        if (movedBorrowOrigin.empty()) movedBorrowOrigin = rightNV.CallerName;
                        LogErrorContext(ctx, std::format(
                            "cannot store borrowed parameter '{}' into unique field '{}'",
                            movedBorrowOrigin, field));
                        return finishStore(right);
                    }
                    // A '??' join whose arm is such a copy (`t.p = a ?? s.p`) is the same double free.
                    const auto* joinCopy = rightNV.BorrowsOwningLocal
                        ? nullptr : compiler->FindOwningLocalCopyJoinArm(rightNV.Primary);
                    if (joinCopy != nullptr || (rightNV.BorrowsOwningLocal
                        && compiler->OwningLocalCopyStillAliases(rightNV)))
                    {
                        const std::string copyName = joinCopy != nullptr
                            ? compiler->FindVariableNameByStorage(joinCopy->Storage) : rightNV.CallerName;
                        const std::string ownerName = joinCopy != nullptr
                            ? joinCopy->OwningLocalOrigin : rightNV.OwningLocalOrigin;
                        LogErrorContext(ctx, std::format(
                            "cannot store '{}' into unique field '{}' - '{}' copies '{}', which still owns "
                            "the object and frees it at scope exit, so the field's synthesized destructor "
                            "double-frees it.",
                            copyName, field, copyName, ownerName));
                        return finishStore(right);
                    }
                    if (llvm::isa_and_nonnull<llvm::AllocaInst>(rightNV.Storage))
                    {
                        const auto* source = compiler->FindVariableByStorage(rightNV.Storage);
                        std::string sourceName = rightNV.CallerName.empty()
                            ? rightNV.TypeAndValue.VariableName : rightNV.CallerName;
                        if (source != nullptr && !source->IsOwning
                            && JoinArmsStillKeepOwner(*source)
                            && !source->JoinKeepsOwnerSource.empty())
                        {
                            LogErrorContext(ctx, std::format(
                                "cannot store '{}' into unique field '{}' - every arm of the join "
                                "was bound from holds an object {} already frees, so the field's "
                                "synthesized destructor double-frees it. Store an object this frame owns instead.",
                                sourceName, field, source->JoinKeepsOwnerSource));
                            return finishStore(right);
                        }
                    }
                }
                // `t.p = c ?? x` / `t.p = f ? c : x` consume an owning local arm as `t.p = c;` does.
                if (destinationIsField) compiler->ConsumeOwningLocalJoinArms(right);
                bool nullSource = llvm::isa<llvm::ConstantPointerNull>(right);
                auto containsMoveOfBorrowedParam = [&](auto&& self, antlr4::tree::ParseTree* node) -> bool {
                    if (node == nullptr) return false;
                    if (auto* move = dynamic_cast<CFlatParser::MoveExpressionContext*>(node))
                        if (move->unaryExpression() != nullptr
                            && compiler->IsFunctionParameter(move->unaryExpression()->getText()))
                            return true;
                    for (auto* child : node->children)
                        if (self(self, child)) return true;
                    return false;
                };
                bool joinMovesBorrowedParam = containsMoveOfBorrowedParam(
                    containsMoveOfBorrowedParam, assignCtx)
                    && compiler->IsMovedBorrowedPtrValue(rightNV.Primary);
                bool unprovenRawValue = !srcIsOwnedForCoreUnique
                    && !sourceIsUniqueField
                    && rightNV.BorrowedUniqueField.empty()
                    && !joinMovesBorrowedParam
                    && !rightNV.PointsToBorrowedAddress
                    && (!rightNV.IsBorrowed
                        || (rightNV.BorrowedOrigin.empty()
                            && !compiler->IsFunctionParameter(rightNV.CallerName)));
                // Assignment is declaration: a source the declaration refuses as borrowed (no owning
                // flag on the value, the local, the arms of a join, or the RHS shape) is refused here
                // too. A raw struct field, an implicit-this or global raw field, and a join with such an
                // arm all reach this point without a field-shaped NamedVariable, so the test is the
                // owning flags rather than the source's parse shape.
                const bool declOwns = rightNV.IsOwning || rightNV.IsOwningStruct
                    || rightNV.TypeAndValue.IsMove || rightNV.IsExplicitMove
                    || compiler->lastOwningResult || srcIsOwnedPtrRhs || sourceIsUniqueField
                    || uniquePointerJoinOwns
                    || (right != nullptr && (compiler->IsOwnedNewTemp(right) || compiler->FindOwnedReturnTemp(right) != nullptr
                        || compiler->IsOwningPtrTempValue(right)
                        || compiler->IsMovedOutPtrValue(right)
                        || compiler->TernaryArmJoinsOwning(right)));
                // Only a raw STRUCT FIELD source (a load through a field GEP of a non-unique struct, or
                // a '?:' join with such an arm) is refused here. A raw local or a call result keeps the
                // round-2 behavior: a plain-field accessor's result is adopted by design.
                auto isRawFieldLoad = [&](llvm::Value* v) -> bool {
                    auto* ld = llvm::dyn_cast_or_null<llvm::LoadInst>(v);
                    if (ld == nullptr) return false;
                    auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(ld->getPointerOperand());
                    if (gep == nullptr) return false;
                    auto* st = llvm::dyn_cast<llvm::StructType>(gep->getSourceElementType());
                    return st != nullptr && st->hasName() && !compiler->IsCoreUniqueType(st->getName().str());
                };
                bool rawFieldSourced = isRawFieldLoad(right);
                if (auto* joinPhi = llvm::dyn_cast_or_null<llvm::PHINode>(right))
                    for (unsigned i = 0; i < joinPhi->getNumIncomingValues(); ++i)
                        rawFieldSourced = rawFieldSourced || isRawFieldLoad(joinPhi->getIncomingValue(i));
                if (unprovenRawValue && !declOwns && rawFieldSourced && !nullSource && !destinationIsField
                    && !namedVar.CallerName.empty() && right != nullptr
                    && !llvm::isa<llvm::ConstantPointerNull>(right) && !IsAllNullPhi(right))
                {
                    std::string uniqueType = SpellType(*compiler, namedVar.TypeAndValue);
                    while (uniqueType.ends_with("*")) uniqueType.pop_back();
                    const std::string srcName = rightNV.CallerName.empty()
                        ? std::string() : rightNV.CallerName;
                    if (srcName.empty())
                        LogErrorContext(ctx, std::format(
                            "cannot reset {} '{}' from a borrowed value - the source still owns it; "
                            "use 'new', a 'move' expression, or a move-returning call",
                            uniqueType, namedVar.CallerName));
                    else
                        LogErrorContext(ctx, std::format(
                            "cannot reset {} '{}' from borrowed value '{}' - the source still owns it; "
                            "use 'new', a 'move' expression, or a move-returning call",
                            uniqueType, namedVar.CallerName, srcName));
                    return finishStore(right);
                }
                if (srcIsOwnedForCoreUnique || sourceIsUniqueField || nullSource || unprovenRawValue)
                {
                    LLVMBackend::NamedVariable resetArg = rightNV;
                    // An adopted '?:' join owns its arms (the if/else join is named): the reset sees it owned.
                    if (uniquePointerJoinOwns) resetArg.IsOwning = true;
                    if (unprovenRawValue)
                    {
                        // A plain alias-returning call is the legacy exception: the raw local remains
                        // a borrow, so keep the wrapper out of scope-exit destruction after reset.
                        resetArg.IsBorrowed = false;
                        resetArg.BorrowedOrigin.clear();
                        resetArg.IsOwning = true;
                        resetArg.IsAliasBorrow = false;
                        resetArg.IsAliasReturnBorrow = false;
                        resetArg.TypeAndValue.IsAlias = false;
                    }
                    if (resetArg.TypeAndValue.TypeName.empty())
                    {
                        resetArg.TypeAndValue.TypeName = MangledGenericArgument(
                            *compiler, namedVar.TypeAndValue.TypeName);
                        resetArg.TypeAndValue.Pointer = true;
                        resetArg.TypeAndValue.PointerDepth = 0;
                    }
                    if (nullSource)
                    {
                        resetArg.TypeAndValue.TypeName = MangledGenericArgument(
                            *compiler, namedVar.TypeAndValue.TypeName);
                        resetArg.TypeAndValue.Pointer = true;
                        resetArg.TypeAndValue.PointerDepth = 0;
                    }
                    resetArg.TypeAndValue.VariableName.clear();
                    auto resetReceiver = namedVar;
                    auto* wrapperTy = compiler->GetType(namedVar.TypeAndValue);
                    if (resetReceiver.Storage != nullptr)
                    {
                        resetReceiver.Primary = resetReceiver.Storage;
                        resetReceiver.Storage = nullptr;
                    }
                    else if (resetReceiver.Primary != nullptr && wrapperTy != nullptr
                             && resetReceiver.Primary->getType() == wrapperTy)
                    {
                        auto* wrapperStorage = compiler->AllocaAtEntry(wrapperTy, nullptr,
                                                                        "unique.reset.receiver");
                        compiler->builder->CreateStore(resetReceiver.Primary, wrapperStorage);
                        resetReceiver.Primary = wrapperStorage;
                    }
                    resetReceiver.BaseType = wrapperTy == nullptr ? resetReceiver.BaseType
                        : cflat_llvm::PointerTo(wrapperTy);
                    resetReceiver.TypeAndValue.Pointer = true;
                    resetReceiver.TypeAndValue.VariableName.clear();
                    compiler->CreateOverloadedFunctionCall("reset", { resetReceiver, resetArg });
                    // After the reset's own borrow check, so a chained or borrowed source keeps its text.
                    if (!destinationIsField)
                        RejectRawHeapArrayIntoUniqueLocal(
                            rightNV, namedVar.TypeAndValue,
                            namedVar.CallerName.empty() ? namedVar.TypeAndValue.VariableName
                                                        : namedVar.CallerName, ctx);
                    if (uniquePointerJoinOwns)
                        compiler->ConsumeOwningLocalJoinArms(right, true, destination);
                    if (destinationIsField && rightNV.IsOwning
                        && !rightNV.TypeAndValue.IsMove
                        && rightNV.Storage != nullptr
                        && rightNV.Storage != destination
                        && (llvm::isa<llvm::AllocaInst>(rightNV.Storage)
                            || llvm::isa<llvm::GlobalVariable>(rightNV.Storage)))
                    {
                        auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(rightNV.BaseType);
                        if (ptrTy != nullptr)
                            compiler->builder->CreateStore(
                                llvm::ConstantPointerNull::get(ptrTy), rightNV.Storage);
                        if (!rightNV.CallerName.empty())
                            compiler->MarkVariableMoved(rightNV.CallerName);
                        compiler->ConsumeOwnedNewTemp(rightNV.Primary);
                    }
                    // A store into a named local re-initializes it (and its LHS copy): clear the moved
                    // state its own earlier consume left. A field destination is cleared in finishStore.
                    if (!namedVar.CallerName.empty() && !destinationIsField)
                    {
                        compiler->MarkVariableUnmoved(namedVar.CallerName);
                        compiler->MarkVariableNotExplicitlyMovedNull(namedVar.CallerName);
                        namedVar.IsMoved = false;
                        namedVar.ExplicitlyMovedNull = false;
                    }
                    if (!destinationIsField && !namedVar.CallerName.empty())
                        compiler->RecordNullClear(namedVar.CallerName);
                    if (coreUniqueInterfaceResetValue && !destinationIsField
                        && !namedVar.CallerName.empty())
                    {
                        compiler->MarkVariableUnmoved(namedVar.CallerName);
                        compiler->MarkVariableNotExplicitlyMovedNull(namedVar.CallerName);
                        compiler->SetVariableOwning(namedVar.CallerName, true);
                    }
                    if (unprovenRawValue && !destinationIsField && !namedVar.CallerName.empty())
                    {
                        auto& local = compiler->GetOrCreateStackVariable(namedVar.CallerName);
                        local.IsAliasBorrow = true;
                        local.IsOwning = false;
                    }
                }
                else
                {
                    std::string uniqueType = SpellType(*compiler, namedVar.TypeAndValue);
                    while (uniqueType.ends_with("*")) uniqueType.pop_back();
                    std::string name = namedVar.CallerName.empty()
                        ? namedVar.TypeAndValue.VariableName : namedVar.CallerName;
                    if (name.empty() && unaryCtx != nullptr)
                        name = unaryCtx->getText();
                    if (destinationIsField)
                    {
                        std::string field = DescribeUniqueFieldOwner(namedVar);
                        std::string movedBorrowOrigin;
                        bool movedBorrow = compiler->IsMovedBorrowedPtrValue(
                            rightNV.Primary, &movedBorrowOrigin);
                        if (movedBorrow)
                        {
                            if (movedBorrowOrigin.empty())
                                movedBorrowOrigin = rightNV.CallerName;
                            LogErrorContext(ctx, std::format(
                                "cannot store borrowed parameter '{}' into unique field '{}'",
                                movedBorrowOrigin, field));
                        }
                        else if (rightNV.BorrowsOwningLocal
                                 && compiler->OwningLocalCopyStillAliases(rightNV))
                        {
                            LogErrorContext(ctx, std::format(
                                "cannot store '{}' into unique field '{}' - '{}' copies '{}'",
                                rightNV.CallerName, field, rightNV.CallerName,
                                rightNV.OwningLocalOrigin));
                        }
                        else if (!rightNV.BorrowedUniqueField.empty())
                            RejectBorrowIntoUniqueField(
                                rightNV, std::format("unique field '{}'", field), ctx);
                        else if (rightNV.IsBorrowed && !rightNV.BorrowedOrigin.empty())
                            LogErrorContext(ctx, std::format(
                                "cannot store borrowed parameter '{}' into unique field '{}'",
                                rightNV.BorrowedOrigin, field));
                        else if (!rightNV.CallerName.empty()
                                 && compiler->IsFunctionParameter(rightNV.CallerName))
                            LogErrorContext(ctx, std::format(
                                "cannot store borrowed parameter '{}' into unique field '{}'",
                                rightNV.CallerName, field));
                        else
                            LogErrorContext(ctx, std::format(
                                "cannot store a borrowed value into unique field '{}' - the source "
                                "still owns it; use 'new', a 'move' expression, or a move-returning call",
                                field));
                        compiler->lastOwningResult = false;
                        compiler->lastCallReturnsOwned = false;
                        return finishStore(LoadNamedVariable(namedVar));
                    }
                    if (!rightNV.BorrowedUniqueField.empty())
                        LogErrorContext(ctx, std::format(
                            "cannot reset {} '{}' from a borrow of unique field '{}' - the field's "
                            "synthesized destructor still owns it; use 'new', a 'move' expression, "
                            "or a move-returning call",
                            uniqueType,
                            name, rightNV.BorrowedUniqueField));
                    else
                    {
                        const std::string sourceName =
                            LLVMBackend::BorrowedSourceName(rightNV);
                        if (sourceName.empty())
                            LogErrorContext(ctx, std::format(
                                "cannot reset {} '{}' from a borrowed value - the source still owns it; "
                                "use 'new', a 'move' expression, or a move-returning call",
                                uniqueType, name));
                        else
                            LogErrorContext(ctx, std::format(
                                "cannot reset {} '{}' from borrowed value '{}' - the source still owns it; "
                                "use 'new', a 'move' expression, or a move-returning call",
                                uniqueType, name,
                                sourceName));
                    }
                }
                compiler->lastOwningResult = false;
                compiler->lastCallReturnsOwned = false;
                // The result load reads the LHS copy, so revive it before the load, not in finishStore.
                reviveStoredField();
                // `reset` loaded this wrapper before storing, so namedVar.Primary is stale here.
                // Build the assignment value from the RHS without re-reading the field.
                if (destinationIsField && right != nullptr
                    && (right->getType()->isPointerTy() || right->getType()->isStructTy())
                    && compiler->IsCoreUniqueType(namedVar.TypeAndValue.TypeName))
                {
                    auto* wrapperTy = llvm::dyn_cast_or_null<llvm::StructType>(
                        compiler->GetType(namedVar.TypeAndValue));
                    if (wrapperTy != nullptr)
                    {
                        const auto& uniqueData = compiler->GetDataStructure(
                            namedVar.TypeAndValue.TypeName);
                        for (size_t i = 0; i < uniqueData.StructFields.size(); i++)
                        {
                            if (uniqueData.StructFields[i].VariableName != "_p"
                                || wrapperTy->getElementType((unsigned)i) != right->getType())
                                continue;
                            auto* assignedWrapper = compiler->builder->CreateInsertValue(
                                llvm::ConstantAggregateZero::get(wrapperTy), right,
                                { (unsigned)i }, "unique.assign.value");
                            // A fat-interface result still borrows the field; retain slot identity so
                            // a chained field store can move it without making local init owning.
                            if (right->getType()->isPointerTy())
                                uniqueFieldAssignmentResultProduced = true;
                            return finishStore(assignedWrapper);
                        }
                    }
                }
                return finishStore(LoadNamedVariable(namedVar));
            }

            // A plain assignment moves the RHS value into the named lvalue; if it was an
            // owned-string temporary (e.g. `s = a + b + c;`) the lvalue now holds it, so
            // drop it from the unnamed-temporary cleanup list - freeing it at end of
            // expression would leave the lvalue dangling (UAF / double free). Intermediate
            // concat temporaries stay queued and are still freed. Only for plain '='; a
            // compound '+=' RHS is a true intermediate that must be freed.
            if (operatorText == "=" && right != nullptr)
                compiler->UnregisterOwnedStringTemp(right);

            // Ownership of an owned heap string returned by the RHS (e.g. copy() /
            // operator+) transfers to the assigned-to string local: mark it owning so
            // the scope-exit destructor frees the buffer. Crucially, also CONSUME the
            // lastCallReturnsOwned flag here. Unlike the declaration path (which clears
            // it), a plain assignment used to leave it set, so the next declaration in
            // an adjacent scope (e.g. `string b = "literal";` in the else branch after
            // `name = a.copy();` in the if branch) would falsely inherit ownership and
            // its scope-exit destructor would free a string literal -> heap corruption.
            // Only a plain string LOCAL is tracked for scope-exit destruction. For a string
            // FIELD assignment (`obj.field = expr.copy()`), CallerName names the base struct,
            // not the field - marking it IsOwningString would mis-tag the whole struct (and,
            // for a returned value struct, block the by-value move-out in CreateReturnCall).
            // The field's buffer is owned via the containing struct's destructor instead, so
            // skip the mark for field accesses (FieldName non-empty) - just consume the flag.
            if (operatorText == "=" && compiler->lastCallReturnsOwned)
            {
                if (NamedVarIsString(namedVar) && !namedVar.CallerName.empty()
                    && namedVar.FieldName.empty() && !isDerefStorage()
                    && !namedVar.IsBorrowed)
                    compiler->MarkVariableOwningString(namedVar.CallerName);
                compiler->lastCallReturnsOwned = false;
            }

            // Implicit char* -> string coercion on assignment: `s = "lit";` / `s = charPtr;`
            // and the struct-field form `r.name = buf;`. Mirrors the declaration initializer
            // path (ParseDeclaration) so assigning a raw c-string to an EXISTING string runs
            // through operator string(const char*) - which sets _ptr AND _len = strlen(s) -
            // instead of storing the bare pointer with _len left 0. The resulting wrap is
            // non-owning, identical to the initializer form. Fires only when the LHS is a
            // non-pointer string and the RHS loaded to a raw i8*; string-to-string and all
            // other assignments are untouched.
            if (operatorText == "=" && right && NamedVarIsString(namedVar)
                && !namedVar.TypeAndValue.Pointer
                && right->getType() == cflat_llvm::PointerTo(compiler->builder->getInt8Ty()))
            {
                right = compiler->CoerceCharPointerToString(right);
            }

            // Wrap named function in closure fat struct when assigning to a function<T> variable.
            if (operatorText == "=" && namedVar.TypeAndValue.IsFunctionPointer
                && right && !right->getType()->isStructTy())
            {
                // Re-resolve by name to avoid picking a struct method that shares the
                // same plain key in functionTable (e.g. atomic_counter::add vs add).
                // Only when the RHS is a bare named function (right is already an
                // llvm::Function); a same-named local/param of function-pointer type
                // loads to a Value and must shadow the global function, not be replaced.
                std::string funcName = assignCtx->getText();
                int expectedParams = (int)namedVar.TypeAndValue.FuncPtrParams.size();
                if (llvm::isa<llvm::Function>(right))
                    if (auto* correctFn = compiler->GetFunctionForFuncPtr(funcName, expectedParams, &namedVar.TypeAndValue.FuncPtrParams, &namedVar.TypeAndValue, /*compareReturnOwnership*/ true))
                        right = correctFn;
                if (auto* fn = llvm::dyn_cast<llvm::Function>(right))
                {
                    VerifyFuncPtrAssignmentMoveFlags(funcName, namedVar.TypeAndValue, ctx);
                    LLVMBackend::NamedVariable boundValue;
                    boundValue.Primary = fn;
                    if (std::string constWhy = compiler->DescribeFuncPtrConstMismatch(
                            namedVar.TypeAndValue, boundValue); !constWhy.empty())
                        LogErrorContext(ctx, constWhy);
                    right = namedVar.TypeAndValue.IsThinFnPtr()
                          ? compiler->MakeThinFnPtrValue(fn, namedVar.TypeAndValue)
                          : compiler->WrapBareValueAsFatStruct(fn);
                }
            }
            // Fallback for generic function pointer reassignment (e.g. fn = wrap<int>):
            // the RHS produces a null value with CallerName set to the mangled function name.
            // This mirrors the same fallback in ParseDeclaration for the initial assignment.
            if (!right && operatorText == "=" && namedVar.TypeAndValue.IsFunctionPointer
                && !rightNV.CallerName.empty())
            {
                int expectedParams = (int)namedVar.TypeAndValue.FuncPtrParams.size();
                llvm::Function* genericFn = compiler->GetFunctionForFuncPtr(rightNV.CallerName, expectedParams, &namedVar.TypeAndValue.FuncPtrParams, &namedVar.TypeAndValue, /*compareReturnOwnership*/ true);
                if (genericFn)
                {
                    VerifyFuncPtrAssignmentMoveFlags(rightNV.CallerName, namedVar.TypeAndValue, ctx);
                    right = namedVar.TypeAndValue.IsThinFnPtr()
                          ? compiler->MakeThinFnPtrValue(genericFn, namedVar.TypeAndValue)
                          : compiler->WrapBareValueAsFatStruct(genericFn);
                }
            }
            // Fat closure -> thin function<T> on reassignment (mirrors the decl path): allowed only
            // when provably non-capturing (env is a compile-time null); otherwise reject with guidance.
            if (operatorText == "=" && right && namedVar.TypeAndValue.IsFunctionPointer
                && namedVar.TypeAndValue.IsThinFnPtr()
                && right->getType() == compiler->GetClosureFatPtrType())
            {
                compiler->UnregisterOwnedClosureTemp(right);
                if (compiler->ClosureIsStaticallyNonCapturing(right))
                    right = compiler->CoerceClosureFatToThin(right, namedVar.TypeAndValue);
                else
                {
                    LogErrorContext(ctx, compiler->DescribeCapturingClosureToThin(rightNV.LambdaCaptureNames));
                    right = llvm::UndefValue::get(compiler->BuildThinFnPtrType(namedVar.TypeAndValue));
                }
            }
            if (operatorText == "=" && right && !namedVar.TypeAndValue.Pointer
                && (namedVar.TypeAndValue.IsFunctionPointer
                    || compiler->GetEncodedClosureType(namedVar.TypeAndValue.TypeName) != nullptr))
                compiler->RejectLiteralZeroFunctionSlot(right, rightNV);
            // A thin function<T> destination fed a raw pointer not handled above (see
            // CheckThinFnPtrAssignProvenance's doc comment for why the FAT twin needs no gate here).
            if (operatorText == "=" && right && namedVar.TypeAndValue.IsFunctionPointer
                && namedVar.TypeAndValue.IsThinFnPtr() && !namedVar.TypeAndValue.Pointer
                && right->getType()->isPointerTy())
            {
                compiler->CheckThinFnPtrAssignProvenance(right, rightNV,
                    namedVar.FieldName.empty()
                        ? (namedVar.CallerName.empty()
                              ? (namedVar.TypeAndValue.VariableName.empty()
                                    ? std::string("the destination")
                                    : std::format("'{}'", namedVar.TypeAndValue.VariableName))
                              : std::format("'{}'", namedVar.CallerName))
                        : std::format("'{}.{}'", namedVar.CallerName, namedVar.FieldName));
            }
            // Widen a thin function<T> value to a fat Lambda<T>: store {code, null}, no thunk
            // (mirrors the decl path). Keeps thin -> Lambda -> toFunction lossless.
            if (operatorText == "=" && right && namedVar.TypeAndValue.IsFunctionPointer
                && !namedVar.TypeAndValue.IsThinFnPtr() && rightNV.TypeAndValue.IsThinFnPtr()
                && !right->getType()->isStructTy())
            {
                right = compiler->WidenThinToFat(right, namedVar.TypeAndValue);
            }
            // Same two conversions for a GENERIC-SUBSTITUTED closure field, whose type is an
            // ENCODED name rather than IsFunctionPointer. The destination decides the repr.
            if (operatorText == "=" && right && !namedVar.TypeAndValue.IsFunctionPointer
                && !namedVar.TypeAndValue.Pointer && !namedVar.TypeAndValue.IsArrayView
                && namedVar.TypeAndValue.ConstArraySize == 0)
            {
                if (const auto* enc = compiler->GetEncodedClosureType(namedVar.TypeAndValue.TypeName))
                {
                    if (enc->IsThinFnPtr() && right->getType() == compiler->GetClosureFatPtrType())
                    {
                        compiler->UnregisterOwnedClosureTemp(right);
                        if (compiler->ClosureIsStaticallyNonCapturing(right))
                            right = compiler->CoerceClosureFatToThin(right, *enc);
                        else
                        {
                            LogErrorContext(ctx, compiler->DescribeCapturingClosureToThin(rightNV.LambdaCaptureNames));
                            right = llvm::UndefValue::get(compiler->BuildThinFnPtrType(*enc));
                        }
                    }
                    else if (enc->IsThinFnPtr() && !right->getType()->isStructTy()
                             && right->getType()->isPointerTy())
                        // Same thin gate as the spelled destination above, for the
                        // generic-encoded element (Box<function<...>>.item = vp).
                        compiler->CheckThinFnPtrAssignProvenance(right, rightNV,
                            namedVar.FieldName.empty()
                                ? (namedVar.CallerName.empty() ? std::string("the destination")
                                                                : std::format("'{}'", namedVar.CallerName))
                                : std::format("'{}.{}'", namedVar.CallerName, namedVar.FieldName));
                    else if (!enc->IsThinFnPtr() && !right->getType()->isStructTy()
                             && right->getType()->isPointerTy())
                        right = compiler->WidenToClosureFatChecked(right, rightNV, {},
                            namedVar.FieldName.empty()
                                ? std::format("'{}'", namedVar.CallerName)
                                : std::format("'{}.{}'", namedVar.CallerName, namedVar.FieldName));
                }
            }
            // Funcptr-to-funcptr assignment, thin AND fat: return ownership and per-param IsMove flags must agree.
            LLVMBackend::TypeAndValue assignValueSig = rightNV.TypeAndValue;
            if (assignValueSig.FuncPtrReturnTypeName.empty() && !assignValueSig.Pointer)
                if (const auto* enc = compiler->GetEncodedClosureType(assignValueSig.TypeName))
                    assignValueSig = *enc;
            if (operatorText == "=" && namedVar.TypeAndValue.IsFunctionPointer
                && assignValueSig.IsFunctionPointer && right)
            {
                if (std::string why = compiler->FuncPtrValueOwnershipReason(
                        assignValueSig, namedVar.TypeAndValue); !why.empty())
                    LogErrorContext(ctx, std::format(
                        "incompatible function pointer assignment: {}", why));
            }

            // Variadic-ness and the bind rule are part of a function-pointer type, whatever the representation.
            // A generic-substituted closure field carries an encoded type name; its signature is the encoded one.
            LLVMBackend::TypeAndValue assignSlotSig = namedVar.TypeAndValue;
            if (!assignSlotSig.IsFunctionPointer && !assignSlotSig.Pointer)
                if (const auto* enc = compiler->GetEncodedClosureType(assignSlotSig.TypeName))
                    assignSlotSig = *enc;
            if (operatorText == "=" && assignSlotSig.IsFunctionPointer
                && assignValueSig.IsFunctionPointer && right)
            {
                if (compiler->ClosureValueCanAdaptToSlot(rightNV, assignSlotSig))
                    right = compiler->AdaptClosureValueToSlot(right, rightNV, assignSlotSig);
                if (std::string why = compiler->DescribeFuncPtrValueMismatch(
                        assignSlotSig, rightNV); !why.empty())
                    LogErrorContext(ctx, why);
                else if (std::string constWhy = compiler->DescribeFuncPtrConstMismatch(
                        assignSlotSig, rightNV); !constWhy.empty())
                    LogErrorContext(ctx, constWhy);
            }

            // An INFERRED sink cannot be adopted onto an already-registered destination (or a
            // struct field's type-wide entry), so the crossing loses it - name the spelling.
            // Deliberately OUTSIDE the fat-struct gate above: a THIN `function<>` destination
            // holds a bare code pointer, so its value is not a struct and would slip past.
            if (operatorText == "=" && namedVar.TypeAndValue.IsFunctionPointer && right)
            {
                int lost = compiler->FindLostClosureSinkParam(namedVar.TypeAndValue,
                                                              rightNV.TypeAndValue);
                if (lost >= 0)
                    LogErrorContext(ctx, compiler->DescribeLostClosureSink(
                        namedVar.TypeAndValue, (size_t)lost,
                        namedVar.FieldName.empty()
                            ? (namedVar.CallerName.empty() ? std::string("the destination type")
                                                           : std::format("'{}'", namedVar.CallerName))
                            : std::format("'{}.{}'", namedVar.CallerName, namedVar.FieldName)));
            }

            // Bond escape check: a bonded closure may use a proven stack holder only.
            if (operatorText == "=" && (rightNV.IsBonded || rightNV.ContainsBondedClosure
                                          || compiler->lastCallIsBonded))
            {
                const auto& bondedSources = rightNV.IsBonded ? rightNV.BondedSources : compiler->lastCallBondedSources;
                if (rightNV.ContainsBondedClosure)
                    LogErrorContext(ctx,
                        "bonded value cannot be stored in a struct field or through a pointer - bond lifetime would be untrackable");
                else if (!llvm::isa<llvm::AllocaInst>(destination) && !llvm::isa<llvm::GlobalVariable>(destination))
                {
                    if (!AllowBondedClosureFieldStore(compiler, namedVar, destination,
                                                       bondedSources, ctx))
                        LogErrorContext(ctx, "bonded value cannot be stored in a struct field or through a pointer - bond lifetime would be untrackable");
                }
                else if (!namedVar.CallerName.empty())
                {
                    size_t lhsDepth = compiler->FindVariableScopeDepth(namedVar.CallerName);
                    for (const auto& source : bondedSources)
                    {
                        size_t srcDepth = compiler->FindVariableScopeDepth(source);
                        if (srcDepth != SIZE_MAX && lhsDepth < srcDepth)
                            LogErrorContext(ctx, std::format("bonded value cannot be assigned to '{}' - '{}' is in a wider scope than its bond source '{}'", namedVar.CallerName, namedVar.CallerName, source));
                    }
                }
                else if (llvm::isa<llvm::GlobalVariable>(destination))
                    LogErrorContext(ctx,
                        "bonded value cannot be stored in a struct field or through a pointer - bond lifetime would be untrackable");
                compiler->lastCallIsBonded = false;
                compiler->lastCallBondByAddress = false;
                compiler->lastCallBondedSources.clear();
            }

            if (operatorText == "=" && namedVar.FieldName.empty()
                && !namedVar.CallerName.empty() && namedVar.Storage != nullptr
                && (llvm::isa<llvm::AllocaInst>(namedVar.Storage)
                    || llvm::isa<llvm::GlobalVariable>(namedVar.Storage)))
            {
                for (auto& frame : std::ranges::reverse_view(compiler->stackNamedVariable))
                {
                    auto it = frame.namedVariable.find(namedVar.CallerName);
                    if (it == frame.namedVariable.end() || it->second.Storage != namedVar.Storage)
                        continue;
                    if (rightNV.IsBonded)
                    {
                        it->second.IsBonded = true;
                        it->second.BondByAddress = rightNV.BondByAddress;
                        it->second.BondedSources = rightNV.BondedSources;
                        it->second.BondDeclBlock = compiler->builder->GetInsertBlock();
                        it->second.BondDeclFunction = it->second.BondDeclBlock != nullptr
                            ? it->second.BondDeclBlock->getParent() : nullptr;
                    }
                    else
                        compiler->ClearVariableBond(namedVar.CallerName);
                    break;
                }
            }

            // Interface upcast: struct* -> fat pointer when assigning to an interface variable.
            // Mirrors the same logic in the declaration initializer path (ParseDeclaration).
            // An 'IA[]' destination is a THIN view over fat elements, not a fat value, so it is
            // never a boxing target - the declarator door excludes views the same way.
            if (operatorText == "=" && namedVar.TypeAndValue.IsInterface
                && !namedVar.TypeAndValue.IsArrayView
                && right && right->getType() != compiler->GetFatPtrType())
            {
                // nullptr assigned to a non-pointer interface variable - produce null fat pointer {null, null}.
                // IsInterfacePointer (IFoo*) stores a pointer-to-fat-ptr, not a fat-ptr; skip this conversion.
                if (!namedVar.TypeAndValue.IsInterfacePointer && llvm::isa_and_nonnull<llvm::ConstantPointerNull>(right))
                    right = llvm::Constant::getNullValue(compiler->GetFatPtrType());
                std::string structName = rightNV.TypeAndValue.TypeName;
                // A '?:' / '??' join result carries no TypeName: box each arm in its own branch.
                // No ownership transfer: a join into a plain interface local is a BORROW by design.
                if (structName.empty() && !namedVar.TypeAndValue.IsInterfacePointer)
                {
                    std::string ternaryArmFailure;
                    std::string joinSpelling = "?:";
                    auto* ternaryFat = UpcastPointerJoinToInterface(
                        right, namedVar.TypeAndValue.TypeName, &ternaryArmFailure, &joinSpelling);
                    if (ternaryFat != nullptr)
                        right = ternaryFat;
                    else if (!ternaryArmFailure.empty())
                        LogErrorContext(ctx, std::format(
                            "cannot convert '{}' arm to interface '{}': {}",
                            joinSpelling, SpellType(*compiler, namedVar.TypeAndValue), ternaryArmFailure));
                }
                if (!structName.empty()
                    && compiler->StructImplementsInterface(structName, namedVar.TypeAndValue.TypeName))
                {
                    // A FIELD store keeps its own bookkeeping (TransferPointerOwnershipOnStore
                    // refcounts an escaping `new`), so only a plain slot adopts here.
                    bool destAdopts = llvm::isa_and_nonnull<llvm::AllocaInst>(destination)
                        || llvm::isa_and_nonnull<llvm::GlobalVariable>(destination);
                    right = BoxConcreteIntoInterface(
                        ctx, right, rightNV.TypeAndValue.Pointer, structName,
                        namedVar.TypeAndValue.TypeName, &rightNV, destAdopts);
                }
                // Same provable rejection as the decl-init spelling; a fat-ptr DESTINATION only,
                // since an 'IFoo*' slot stores a pointer and never bitcasts to the fat struct.
                else if (namedVar.TypeAndValue.IsFatInterfaceValue())
                {
                    RejectPrimitiveShapedInterfaceUpcast(
                        ctx, rightNV.TypeAndValue, namedVar.TypeAndValue.TypeName);
                }
            }
            // Derived-interface -> parent-interface assignment: re-box through the typedesc chain.
            // A '?:' RHS carries no NamedVariable TypeName, so fall back to its join ledger.
            else if (operatorText == "=" && namedVar.TypeAndValue.IsFatInterfaceValue()
                     && right && right->getType() == compiler->GetFatPtrType())
            {
                std::string srcIface = compiler->ResolveFatInterfaceSrcName(right,
                    rightNV.TypeAndValue.IsInterface ? rightNV.TypeAndValue.TypeName : std::string());
                right = compiler->ReboxInterfaceIfNeeded(
                    right, srcIface, namedVar.TypeAndValue.TypeName);
            }

            // Assignment leg of the borrowed-box tag. Scoped to a plain bare-identifier LHS with
            // alloca storage - a field store is never tracked. See TagInterfaceBoxProvenance.
            if (operatorText == "=" && namedVar.TypeAndValue.IsFatInterfaceValue()
                && namedVar.FieldName.empty() && !namedVar.CallerName.empty()
                && llvm::isa_and_nonnull<llvm::AllocaInst>(destination))
                TagInterfaceBoxProvenance(namedVar.CallerName, right);

            // A plain interface local adopts an owning result just like a value-struct local. Its
            // ownership is not implied by `unique`; the IsOwning bit is the per-slot state. When a
            // later assignment supplies a borrow, release the old box first and clear that bit so
            // scope exit does not release the borrowed source. A concrete pointer upcast may have
            // transferred ownership while boxing, so include that value-keyed fact as well.
            bool destPlainInterfaceLocal = operatorText == "="
                && namedVar.TypeAndValue.IsFatInterfaceValue()
                && !namedVar.TypeAndValue.IsUnique
                && namedVar.FieldName.empty() && !namedVar.CallerName.empty()
                && llvm::isa_and_nonnull<llvm::AllocaInst>(destination)
                && destination != rightNV.Storage;
            if (destPlainInterfaceLocal)
            {
                if (namedVar.IsOwning)
                    compiler->DropValue(namedVar);

                const auto* boxedRecord = compiler->FindInterfaceBoxByFatValue(right);
                bool boxedOwnershipTransferred = boxedRecord != nullptr
                    && boxedRecord->OwnershipTransferred;
                bool moveFromNamedInterface = rightNV.TypeAndValue.IsMove
                    && rightNV.TypeAndValue.IsFatInterfaceValue()
                    && TopLevelMoveExpression(assignCtx)
                    && rightNV.Storage != nullptr && !rightNV.IsBorrowed;
                bool sourceIsOwnedInterfaceResult =
                    compiler->FindOwnedReturnEntry(rightNV.Primary) != nullptr
                    || (srcIsOwnedForUniqueIface && TopLevelMoveExpression(assignCtx));
                bool sourceTransfersInterfaceOwnership =
                    sourceIsOwnedInterfaceResult
                    && (rightNV.Storage == nullptr || moveFromNamedInterface
                        || boxedOwnershipTransferred);
                if (moveFromNamedInterface)
                {
                    compiler->builder->CreateStore(
                        llvm::Constant::getNullValue(compiler->GetFatPtrType()), rightNV.Storage);
                    if (!rightNV.CallerName.empty())
                        compiler->MarkVariableMoved(rightNV.CallerName);
                }
                compiler->SetVariableOwning(namedVar.CallerName, sourceTransfersInterfaceOwnership);
                compiler->SetVariableOwnsInterfaceBox(
                    namedVar.CallerName, sourceTransfersInterfaceOwnership);
            }

            // D5 (assignment leg): a `unique` interface local owns a heap-boxed object, and its
            // scope-exit teardown frees the fat-ptr data pointer. Reassigning it from a borrowed
            // value or a stack box would either create a second owner (leak / double-free) or point
            // the owning slot at a STACK address that teardown then frees (heap corruption). Only a
            // heap `new`, a `move`, a move-returning call, or nullptr is legal - mirror decl-init
            // (ParseDeclaration). A null value (raw null or the null fat-ptr from the nullptr upcast
            // above) owns nothing and is allowed.
            bool destLegacyUniqueIface = namedVar.TypeAndValue.IsUnique
                && namedVar.TypeAndValue.IsFatInterfaceValue();
            bool destUniqueIface = destLegacyUniqueIface
                || (!namedVar.TypeAndValue.Pointer
                    && !InGenericInstantiation()
                    && compiler->IsCoreUniqueType(namedVar.TypeAndValue.TypeName)
                    && namedVar.TypeAndValue.TypeName.size() > 8
                    && compiler->IsInterfaceType(MangledGenericArgument(
                        *compiler, namedVar.TypeAndValue.TypeName))
                    && (llvm::isa<llvm::AllocaInst>(destination)
                        || llvm::isa<llvm::GlobalVariable>(destination)));
            bool srcIsNullForUniqueIface = right != nullptr
                && ((llvm::isa<llvm::Constant>(right)
                        && llvm::cast<llvm::Constant>(right)->isNullValue())
                    || IsAllNullPhi(right));
            // The implicit-move ruling reaches a `unique` INTERFACE-valued FIELD too, so D5 must
            // not pre-empt it. A LOCAL / GLOBAL destination keeps D5 and its own drop-old below.
            bool uniqueIfaceFieldImplicitMove = operatorText == "=" && destUniqueIface
                && !llvm::isa_and_nonnull<llvm::AllocaInst>(destination)
                && !llvm::isa_and_nonnull<llvm::GlobalVariable>(destination)
                && IsUniqueFieldRead(rightNV);
            if (operatorText == "=" && destUniqueIface && !uniqueIfaceFieldImplicitMove
                && !srcIsOwnedForUniqueIface && !srcIsNullForUniqueIface)
            {
                LogErrorContext(ctx, std::format(
                    "cannot assign a borrowed value to unique interface '{}' - the source still owns "
                    "it (or is a stack value), so this would leak or free a stack address at scope "
                    "exit; assign 'new', a 'move' expression, a move-returning call, or 'nullptr'",
                    namedVar.CallerName));
                return finishStore(right);
            }

            // A valid owning reassignment to a `unique` interface LOCAL frees the OLD pointee first
            // (its scope-exit teardown only sees the final value), then adopts ownership of the new
            // one. `s = nullptr` frees the old and stores null (release-early). Self-assign cannot
            // occur - the reject above only admits new / move / move-call / null.
            if (operatorText == "=" && destLegacyUniqueIface && !coreUniqueInterfaceResetValue
                && (llvm::isa<llvm::AllocaInst>(destination) || llvm::isa<llvm::GlobalVariable>(destination)))
            {
                auto* oldFat = compiler->builder->CreateLoad(compiler->GetFatPtrType(), destination);
                compiler->DeleteInterfaceValue(oldFat, namedVar.TypeAndValue.TypeName, nullptr);
                if (srcIsOwnedForUniqueIface && !namedVar.CallerName.empty())
                {
                    compiler->SetVariableOwning(namedVar.CallerName, true);
                    compiler->ConsumeOwnedNewTemp(rightNV.Primary);
                }
            }

            // Assignment leg of the code-value store gate - this one site also carries the field,
            // element and global stores, which all reach ParseAssignment with the slot's own type.
            if (right && compiler->CodeValueIntoDataDestination(rightNV, namedVar.TypeAndValue))
            {
                const auto& destTV = namedVar.TypeAndValue;
                // A compound operator asks a different question, and on a non-pointer destination
                // ('string +=' is concatenation) the offset wording would be false.
                LogErrorContext(ctx, operatorText == "="
                    ? compiler->DescribeCodeValueIntoData(
                        CodeValueDestSpelling(destTV), "assign", CodeValueCastAdvice(destTV))
                    : compiler->DescribeCodeValueAsCompoundOperand(
                        CodeValueDestSpelling(destTV), operatorText,
                        destTV.Pointer && !destTV.IsArrayView));
                return finishStore(right);
            }

            // A string LITERAL is a 'const char*', never a 'T*': catch it here or the slot points
            // at character data and the first member access through it faults.
            if (operatorText == "=")
                RejectStringLiteralIntoStructPointer(
                    ctx, namedVar.TypeAndValue, right,
                    namedVar.CallerName.empty() ? std::string("this location")
                    : namedVar.IsElementAccess
                        ? std::format("element of '{}'", namedVar.CallerName)
                        : std::format("'{}'", namedVar.CallerName));

            // `p = cppRefReturningCall();` binds to the referent, exactly as `&<call>` does -
            // the twin of the declaration-initializer leg in MainListener_Declarations.
            if (operatorText == "=" && right != nullptr && right->getType()->isStructTy())
                if (auto* refAddr = compiler->CxxReferenceResultAsPointer(
                        namedVar.TypeAndValue, rightNV,
                        namedVar.CallerName.empty() ? std::string("this location")
                                                    : std::format("'{}'", namedVar.CallerName)))
                    right = refAddr;

            // M6 - `basePtr = derivedPtr;` shifts to the base subobject, matching the declaration
            // initializer leg above.
            if (operatorText == "=" && right != nullptr)
                right = compiler->AdjustCxxPointerForStore(
                    namedVar.TypeAndValue, rightNV.TypeAndValue, right,
                    namedVar.CallerName.empty() ? std::string("this location")
                                                : std::format("'{}'", namedVar.CallerName));

            // Name the FIELD for a member store ('s.f'), not just the struct variable.
            if (operatorText == "=" && RejectImplicitPrimitiveToPointer(
                    ctx, namedVar.TypeAndValue, rightNV, right, "assign",
                    namedVar.CallerName.empty() ? std::string("this location")
                    : namedVar.FieldName.empty() ? std::format("'{}'", namedVar.CallerName)
                    : std::format("'{}.{}'", namedVar.CallerName, namedVar.FieldName)))
                return finishStore(right);

            if (right && !(namedVar.BaseType && namedVar.BaseType->isArrayTy())
                && RejectImplicitPointerToNumber(
                    ctx, namedVar.TypeAndValue, rightNV, right,
                    operatorText == "=" ? "assign" : "use in compound assignment",
                    namedVar.CallerName.empty() ? std::string("this location")
                    : namedVar.FieldName.empty() ? std::format("'{}'", namedVar.CallerName)
                    : std::format("'{}.{}'", namedVar.CallerName, namedVar.FieldName)))
                return finishStore(right);
            if (operatorText == "=" && right != nullptr
                && RejectConstDiscardingConversion(ctx, rightNV, right, namedVar.TypeAndValue))
                return finishStore(right);

            // Pointer variable assigned a struct value: catch the mismatch here
            // with a clear message rather than letting LLVM assert inside CreateCast.
            // Interface targets are exempt: an interface slot legitimately holds a fat-ptr
            // struct value (produced by the upcast above), even when Pointer is set.
            if (operatorText == "=" && right && namedVar.TypeAndValue.Pointer && !namedVar.TypeAndValue.IsFunctionPointer
                && !namedVar.TypeAndValue.IsInterface
                && right->getType()->isStructTy())
            {
                if (auto* converted = compiler->ConvertViaImplicitConversionOperator(
                        right, namedVar.TypeAndValue))
                    right = converted;
            }
            if (operatorText == "=" && right && namedVar.TypeAndValue.Pointer && !namedVar.TypeAndValue.IsFunctionPointer
                && !namedVar.TypeAndValue.IsInterface
                && right->getType()->isStructTy())
            {
                LogErrorContext(ctx, std::format(
                    "cannot assign a value of type '{}' to pointer variable '{}' - the right-hand side must be a pointer (call getPtr() or use '&')",
                    SpellType(*compiler, rightNV.TypeAndValue), namedVar.CallerName));
                return finishStore(right);
            }

            bool rhsUnsigned = rightNV.TypeAndValue.IsUnsignedInteger() != -1;
            bool usedCompoundOverload = false;
            // A C++ compound operator mutates the receiver in place; expression uses keep its
            // declared result type, while a discarded statement result is ignored.
            bool cxxCompoundInPlace = false;
            if (operatorText != "=")
            {
                // `w += k` READS w; derefLoad bypasses LoadNamedVariable, so log the move use it
                // would have (a compound on a moved owner otherwise runs on a nulled value).
                CheckMovedReceiver(namedVar);
                auto left = derefLoad();
                bool lhsUnsigned = namedVar.TypeAndValue.IsUnsignedInteger() != -1;

                // Pointer compound arithmetic: use element-typed GEP (C semantics).
                if (namedVar.TypeAndValue.Pointer
                    && (operatorText == "+=" || operatorText == "-=")
                    && right && right->getType()->isIntegerTy())
                {
                    auto elemTV = namedVar.TypeAndValue;
                    elemTV.ElemPointer ? (elemTV.ElemPointer = false) : (elemTV.Pointer = false, elemTV.IsInterfacePointer = false);
                    auto* et = compiler->GetType(elemTV);
                    // Pointer arithmetic offset: a u32 operand widens unsigned, so `p += 4294967295u`
                    // steps forward rather than back one element.
                    auto* idx = compiler->Upconvert(right, compiler->builder->getInt64Ty(), rhsUnsigned);
                    if (operatorText == "-=")
                        idx = compiler->builder->CreateNeg(idx, "neg");
                    if (et != nullptr && et->isVoidTy())
                    {
                        LogVoidPointerArithmetic(ctx);
                        right = left;
                    }
                    else
                        right = compiler->CreateGEP(et, left, idx, "ptrarith");
                }
                else
                {
                    std::string_view binaryOp;
                    for (const auto& [compoundOp, op] : kCompoundOperators)
                        if (operatorText == compoundOp) { binaryOp = op; break; }

                    llvm::Value* overload = nullptr;
                    bool compoundOverloadExists = false;
                    // `c op= move d` on a C++ record binds an rvalue reference: the explicit move
                    // is an xvalue, so the call takes the moved temp, never the source's slot.
                    const bool rhsIsRvalue = rightNV.IsRvalue
                        || (rightNV.IsExplicitMove
                            && compiler->IsCxxRecord(rightNV.TypeAndValue.TypeName));
                    // The operand's proven depth, as a binary operand reports it, so `m += &x`
                    // keeps its `int*` pointee for C++ matching (T85).
                    int rhsDepth = 0;
                    bool rhsElemPointer = false;
                    OperandPointerDepth(rightNV, right, rhsDepth, rhsElemPointer);
                    // `*this op= x` in a derived [cpp] struct: the protected member check sees the
                    // current object, as the binary operand path reports it.
                    const std::string& lhsSource = namedVar.CallerName.empty()
                        ? namedVar.TypeAndValue.VariableName : namedVar.CallerName;
                    auto emitCompound = [&]() {
                        return TryBinaryOperatorOverload(
                            left, operatorText, right, ctx, namedVar.BaseType,
                            rhsDepth, rhsElemPointer, namedVar.Storage,
                            rightNV.Storage, false, true, namedVar.IsRvalue, rhsIsRvalue,
                            namedVar.TypeAndValue.TypeName,
                            rightNV.TypeAndValue.TypeName,
                            0, false, lhsSource, namedVar.FieldName, namedVar.IsElementAccess,
                            {}, {}, false, namedVar.ConditionalMoveSources, {},
                            rightNV.TypeAndValue.PointeeConstBit()
                                && rightNV.TypeAndValue.Pointer, {}, rightNV.CxxArithIdentity,
                                rightNV.IsExplicitMove, namedVar.IsExplicitMove,
                                namedVar.TypeAndValue.IsCxxNullptrT,
                                rightNV.TypeAndValue.IsCxxNullptrT, namedVar.IsRvalue, false,
                                compiler->CxxOperandObjectCv(namedVar),
                                compiler->CxxOperandObjectCv(rightNV), false,
                                constCxxCompoundFreeCandidate, &namedVar);
                    };
                    auto isCxxClassValue = [&](llvm::Value* value) {
                        auto* st = value != nullptr
                            ? llvm::dyn_cast<llvm::StructType>(value->getType()) : nullptr;
                        if (st == nullptr || st->isLiteral() || !st->hasName()) return false;
                        const std::string name = st->getName().str();
                        return compiler->IsCxxRecord(name)
                            || compiler->generatedCxxRecords_.count(name) != 0;
                    };
                    const bool cxxClassLeft = !binaryOp.empty() && isCxxClassValue(left);
                    const bool cxxClassRight = !binaryOp.empty() && !cxxClassLeft
                        && left != nullptr && !left->getType()->isStructTy()
                        && isCxxClassValue(right);
                    const bool cxxClassCompoundAttempted = cxxClassLeft || cxxClassRight;
                    /*
                     * The WRITTEN compound operator of a C++ class operand, as C++ looks it up
                     * (members, associated namespaces, hidden friends): C++ never rewrites
                     * `a op= b` to `a = a op b`. It mutates the left operand in place.
                     */
                    auto emitCxxCompound = [&]() -> llvm::Value* {
                        compiler->lastCxxRetTemp_ = nullptr;
                        compiler->lastCxxRetValue_ = nullptr;
                        llvm::Value* result = CxxAssignSretScope::EmitOperatorCall(compiler, emitCompound);
                        if (result != nullptr)
                        {
                            cxxCompoundInPlace = true;
                            compoundOverloadExists = true;
                        }
                        return result;
                    };
                    if (cxxClassLeft)
                    {
                        std::string leftType = namedVar.TypeAndValue.TypeName;
                        if (leftType.empty())
                            leftType = llvm::cast<llvm::StructType>(left->getType())->getName().str();
                        const size_t typeSep = leftType.rfind('.');
                        const std::string operatorTemplateName = typeSep == std::string::npos
                            ? "operator" + operatorText
                            : leftType.substr(0, typeSep + 1) + "operator" + operatorText;
                        const bool compoundTemplateExists =
                            compiler->HasCxxFunctionTemplate(operatorTemplateName);
                        bool compoundOverloadExists = HasOperatorOverloadForFirstParam(
                            "operator" + operatorText, leftType);
                        // A member template operator (`template<class U> T& operator&=(U&&)`) is
                        // instantiated for this receiver and operand (each operand category is its own
                        // instance), as the assignment path does.
                        if (!compoundTemplateExists
                            && compiler->HasCxxFunctionTemplateMember(leftType, "operator" + operatorText))
                        {
                            LLVMBackend::NamedVariable receiver;
                            receiver.Primary = namedVar.Storage;
                            receiver.Storage = namedVar.Storage;
                            receiver.BaseType = namedVar.Storage->getType();
                            receiver.TypeAndValue.TypeName = leftType;
                            receiver.TypeAndValue.Pointer = true;
                            // A written `move` operand is an rvalue to the template's U&& parameter.
                            LLVMBackend::NamedVariable operand = rightNV;
                            operand.IsRvalue = rhsIsRvalue;
                            std::vector<LLVMBackend::NamedVariable> templateArguments = { receiver, operand };
                            std::string registeredName, templateError;
                            compiler->RequestCxxFunctionTemplate("operator" + operatorText, leftType, {},
                                templateArguments, {}, registeredName, templateError);
                            compoundOverloadExists = HasOperatorOverloadForFirstParam(
                                "operator" + operatorText, leftType);
                        }
                        if (!compoundOverloadExists && !compoundTemplateExists)
                        {
                            const bool retried =
                                compiler->TryBindRefusedCxxMember(leftType, "operator" + operatorText)
                                || compiler->TryBindCxxFunction(operatorTemplateName);
                            (void)retried;
                        }
                    }
                    if (cxxClassCompoundAttempted)
                        overload = emitCxxCompound();
                    if (!cxxClassCompoundAttempted && !binaryOp.empty() && left != nullptr
                        && left->getType()->isStructTy())
                    {
                        std::string leftType = namedVar.TypeAndValue.TypeName;
                        if (leftType.empty())
                            leftType = llvm::cast<llvm::StructType>(left->getType())->getName().str();
                        const bool cxxClass = compiler->IsCxxRecord(leftType)
                            || compiler->generatedCxxRecords_.count(leftType) != 0;
                        const size_t typeSep = leftType.rfind('.');
                        const std::string operatorTemplateName = typeSep == std::string::npos
                            ? "operator" + operatorText
                            : leftType.substr(0, typeSep + 1) + "operator" + operatorText;
                        const bool compoundTemplateExists = cxxClass
                            && compiler->HasCxxFunctionTemplate(operatorTemplateName);
                        compoundOverloadExists = HasOperatorOverloadForFirstParam(
                            "operator" + operatorText, leftType);
                        // An operator@= refused only for an unrequested return-type
                        // specialization (`Expr<W>& operator*=`) binds on use, as calls do.
                        if (cxxClass && !compoundOverloadExists && !compoundTemplateExists
                            && (compiler->TryBindRefusedCxxMember(leftType, "operator" + operatorText)
                                || compiler->TryBindCxxFunction(operatorTemplateName)))
                            compoundOverloadExists = HasOperatorOverloadForFirstParam(
                                "operator" + operatorText, leftType);
                        cxxCompoundInPlace = cxxClass
                            && (compoundOverloadExists || compoundTemplateExists);
                        compoundOverloadExists = compoundOverloadExists || compoundTemplateExists;
                        // Only a C++ in-place compound may take an enclosing slot; a CFlat
                        // compound's result is stored back into the destination instead.
                        if (compoundOverloadExists && cxxCompoundInPlace)
                        {
                            // Only this call's by-value result may be read back as the prvalue.
                            compiler->lastCxxRetTemp_ = nullptr;
                            compiler->lastCxxRetValue_ = nullptr;
                            overload = CxxAssignSretScope::EmitOperatorCall(compiler, emitCompound);
                        }
                        else if (compoundOverloadExists)
                            overload = emitCompound();
                    }
                    if (!cxxClassCompoundAttempted && overload == nullptr
                        && !compoundOverloadExists && cxxClassLeft)
                        overload = emitCxxCompound();
                    // A right class that may hide a friend `operator@=(T&, const C&)`: the written
                    // compound first, so an exact friend beats the desugared builtin conversion.
                    if (!cxxClassCompoundAttempted && overload == nullptr
                        && !compoundOverloadExists && cxxClassRight
                        && compiler->CxxClassMayHaveUnregisteredOperators(
                            llvm::cast<llvm::StructType>(right->getType())->getName().str()))
                        overload = emitCxxCompound();
                    // The CFlat desugaring keeps master's resolution only (registered operators,
                    // a builtin through one conversion); clang's ADL lookup is for written forms.
                    AdlSuppressScope adlSuppress(cxxAdlSuppressDepth_, cxxClassLeft || cxxClassRight);
                    if (!cxxClassCompoundAttempted && overload == nullptr
                        && !compoundOverloadExists && !binaryOp.empty())
                        overload = TryBinaryOperatorOverload(
                            left, std::string(binaryOp), right, ctx, namedVar.BaseType,
                            rhsDepth, rhsElemPointer, namedVar.Storage,
                            rightNV.Storage, false, true, false, rhsIsRvalue,
                            namedVar.TypeAndValue.TypeName,
                            rightNV.TypeAndValue.TypeName,
                            0, false, lhsSource, namedVar.FieldName, namedVar.IsElementAccess,
                            {}, {}, false, namedVar.ConditionalMoveSources, {},
                            rightNV.TypeAndValue.PointeeConstBit()
                                && rightNV.TypeAndValue.Pointer, {}, rightNV.CxxArithIdentity,
                                rightNV.IsExplicitMove, namedVar.IsExplicitMove,
                                namedVar.TypeAndValue.IsCxxNullptrT,
                                rightNV.TypeAndValue.IsCxxNullptrT, false, false,
                                compiler->CxxOperandObjectCv(namedVar),
                                compiler->CxxOperandObjectCv(rightNV), false, false, &namedVar);
                    adlSuppress.Release();
                    // Scalar left, C++ class right with no builtin answer: the written compound.
                    if (!cxxClassCompoundAttempted && overload == nullptr
                        && !compoundOverloadExists && cxxClassRight)
                        overload = emitCxxCompound();
                    if (overload != nullptr)
                    {
                        right = overload;
                        usedCompoundOverload = true;
                    }
                    else if (constCxxCompoundFreeCandidate
                        && RefuseConstWrite(unaryCtx, namedVar, destination, "modify", "assign to"))
                    {
                        return nullptr;
                    }
                    else if (!binaryOp.empty() && left != nullptr
                             && (left->getType()->isStructTy() || cxxClassRight))
                    {
                        LogErrorContext(unaryCtx, std::format(
                            "no overload for compound assignment '{}' on type '{}'",
                            operatorText, SpellType(*compiler, cxxClassRight
                                ? rightNV.TypeAndValue : namedVar.TypeAndValue)));
                    }
                    else
                    {
                        right = compiler->CreateOperation(operatorText, left, right, lhsUnsigned, rhsUnsigned);
                    }
                }
            }

            // The C++ operator mutates its receiver through its reference parameter. Bind a
            // returned T& with the same storage form as a plain reference-returning call.
            if (cxxCompoundInPlace && usedCompoundOverload)
            {
                auto* opTemp = compiler->lastCxxRetTemp_;
                auto* stored = finishStore(derefLoad());
                if (assignmentResult == nullptr || coalesceResume != nullptr
                    || namedVar.TypeAndValue.Pointer || destination == nullptr)
                    return stored;
                const auto& opResultType = compiler->lastCallReturnType;
                if (right != nullptr && right->getType()->isPointerTy()
                    && opResultType.IsAlias && !opResultType.IsRvalueRef
                    && !opResultType.TypeName.empty()
                    && compiler->IsCxxRecord(opResultType.TypeName))
                {
                    auto valueType = opResultType;
                    valueType.Pointer = false;
                    valueType.PointerDepth = 0;
                    valueType.ElemPointer = false;
                    auto* llvmValueType = compiler->GetType(valueType);
                    if (llvmValueType != nullptr && llvmValueType->isStructTy())
                    {
                        auto* resultValue = compiler->CreateLoad(llvmValueType, right);
                        *assignmentResult = {};
                        assignmentResult->TypeAndValue = opResultType;
                        assignmentResult->TypeAndValue.VariableName.clear();
                        assignmentResult->Primary = right;
                        PrepareAliasCallResult(unaryCtx, *assignmentResult, false);
                        return resultValue;
                    }
                }
                if (right != nullptr && right->getType()->isPointerTy()
                    && opResultType.Pointer && !opResultType.IsAlias
                    && !opResultType.TypeName.empty()
                    && compiler->IsCxxRecord(opResultType.TypeName))
                {
                    assignmentResult->TypeAndValue = opResultType;
                    assignmentResult->Primary = right;
                    assignmentResult->Storage = nullptr;
                    assignmentResult->BaseType = right->getType();
                    return right;
                }
                if (auto* prvalue = cxxPrvalueAssignResult(opTemp)) return prvalue;
                if (assignmentResult != nullptr && right != nullptr
                    && right->getType()->isStructTy() && !opResultType.IsAlias
                    && !opResultType.TypeName.empty()
                    && compiler->IsCxxRecord(opResultType.TypeName))
                {
                    *assignmentResult = {};
                    assignmentResult->TypeAndValue = opResultType;
                    assignmentResult->TypeAndValue.VariableName.clear();
                    assignmentResult->Primary = right;
                    assignmentResult->BaseType = right->getType();
                    assignmentResult->IsRvalue = true;
                    return right;
                }
                // A `T&` result is the destination lvalue (C++): a by-value consumer copies it.
                compiler->lastCxxRetTemp_ = nullptr;
                compiler->lastCxxRetValue_ = nullptr;
                if (compiler->IsForeignNontrivialCxxClass(namedVar.TypeAndValue.TypeName))
                {
                    assignmentResult->Storage = destination;
                    assignmentResult->CallerName = namedVar.CallerName;
                    assignmentResult->FieldName = namedVar.FieldName;
                    assignmentResult->MovePathKey = namedVar.MovePathKey;
                    assignmentResult->MovePathHasDynamicIndex = namedVar.MovePathHasDynamicIndex;
                }
                return stored;
            }

            // An overloaded compound assignment replaces the live LHS with its returned value.
            // Release the old owner first; this covers locals, fields, arrays and dereferences.
            if (usedCompoundOverload && right != nullptr && right->getType()->isStructTy())
            {
                if (compiler->IsOwningValueType(namedVar.TypeAndValue.TypeName))
                    if (auto* dtor = compiler->GetOrCreateFullDestructor(namedVar.TypeAndValue.TypeName))
                        compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { destination });
                compiler->UnregisterOwnedStringTemp(right);
                compiler->SuppressCallerRelease(right);
                // derefAssign, not CreateAssignment: a '*p op= x' destination is a raw loaded
                // pointer whose storage type cannot be inferred (it stores through BaseType).
                derefAssign(right, rhsUnsigned);
                // A consuming operator took the destination as its operand; the store makes it live again.
                if (!namedVar.CallerName.empty() && namedVar.FieldName.empty())
                {
                    compiler->MarkVariableUnmoved(namedVar.CallerName);
                    compiler->MarkVariableNotExplicitlyMovedNull(namedVar.CallerName);
                }
                // Return the stored value so a call argument cannot register the produced result twice.
                return finishStore(derefLoad());
            }

            // The core `unique<T, ALIGN>` holder owns its block's alignment through the ALIGN generic
            // argument, so its internal raw slot carries no clause and must not be judged against one.
            // Read the owner off the destination GEP: inside unique's own methods the field access
            // leaves OwningStructName empty.
            auto slotOwnerIsCoreUnique = [&]() {
                if (!namedVar.OwningStructName.empty())
                    return compiler->IsCoreUniqueType(namedVar.OwningStructName);
                auto* gep = llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(destination);
                if (gep == nullptr) return false;
                auto* st = llvm::dyn_cast<llvm::StructType>(gep->getSourceElementType());
                if (st == nullptr || !st->hasName()) return false;
                return compiler->IsCoreUniqueType(st->getName().str());
            };
            // X4 guard: copying a named owning-value into a struct field aliases its buffer (double-free).
            // Single-index GEP excluded - those are container-internal element stores that must not be flagged.
            auto* destGep = llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(destination);
            bool destIsStructField = (destGep && destGep->getNumIndices() == 2
                && destGep->getSourceElementType()->isStructTy())
                || namedVar.IsInterfaceField;  // reached via the interface's byte-offset slot
            // A fresh call result written through a by-reference closure is a whole-slot store,
            // despite Storage being the loaded pointer carried by the closure environment.
            bool destIsByRefClosureValue = namedVar.IsClosureRefCapture
                && namedVar.FieldName.empty() && !namedVar.TypeAndValue.Pointer
                && !namedVar.TypeAndValue.IsInterface
                && compiler->IsDataStructure(namedVar.TypeAndValue.TypeName)
                && compiler->IsOwningValueType(namedVar.TypeAndValue.TypeName)
                && compiler->IsCopyableType(namedVar.TypeAndValue.TypeName)
                && !compiler->IsForeignNontrivialCxxClass(namedVar.TypeAndValue.TypeName)
                && rightNV.IsRvalue
                && !rightNV.TypeAndValue.IsAlias && !rightNV.IsAliasBorrow
                && compiler->IsProducedTempValue(right);

            // A subscript of a FIXED array is a TWO-index GEP over `[N x T]`, so it is neither a
            // struct field nor an alloca/global - and not Part 6's SINGLE-index container slot
            // either. Its elements are LIVE default-constructed values, so an owning store here
            // must drop the old one (a container slot must not); keep the two accept sets apart.
            // A `T[]` VIEW subscript is a SINGLE-index GEP over the element type - the same shape a
            // container's internal `T*` buffer slot has, so the GEP alone cannot separate them and
            // Part 6's gate must stay untouched (the plan's LOAD-BEARING INVARIANT). IsViewElement
            // is the positive signal: the base binding was a user-visible `T[]`, whose viewed slots
            // are LIVE (a fixed array, a `new T[n]` allocation, or the caller's storage behind a
            // `T[]` parameter), so the destination joins the fixed-array accept set below.
            bool destIsViewElem = destGep && destGep->getNumIndices() == 1
                && namedVar.IsElementAccess && namedVar.IsViewElement
                && !namedVar.IsInterfaceField && !namedVar.BitfieldStorage
                && !namedVar.UnionFieldType;
            bool destIsFixedArrayElem = (destGep && destGep->getNumIndices() == 2
                && destGep->getSourceElementType()->isArrayTy()
                && namedVar.IsElementAccess
                && !namedVar.IsInterfaceField && !namedVar.BitfieldStorage
                && !namedVar.UnionFieldType)
                || destIsViewElem;
            // A desugared unique field reached through an array element is still a field even
            // though its GEP source type is the containing array, not the holder struct.
            bool destIsUniqueFieldSlot = !namedVar.FieldName.empty()
                && compiler->IsCoreUniqueType(namedVar.TypeAndValue.TypeName)
                && !namedVar.TypeAndValue.Pointer;
            if (!destIsUniqueFieldSlot && destIsStructField
                && compiler->IsCoreUniqueType(namedVar.TypeAndValue.TypeName)
                && !namedVar.TypeAndValue.Pointer)
                destIsUniqueFieldSlot = true;
            auto emitOwningDestinationDrop = [&]() {
                // Only a struct with a C++ field gets a drop flag, so only there is the static
                // released fact exact. A native released slot is a null shell: dropping it is safe.
                if (namedVar.ExplicitlyMovedNull && namedVar.ConditionalDropFlag == nullptr
                    && compiler->HasForeignNontrivialCxxField(namedVar.TypeAndValue.TypeName))
                    return;
                if (auto* dtor = compiler->GetOrCreateFullDestructor(
                        namedVar.TypeAndValue.TypeName))
                    compiler->EmitConditionalFullDestructor(namedVar, dtor);
            };

            // A field of an alias-borrow local can arrive through the generic owning-local
            // reassignment path rather than the specialized field-read arm. Ask the shared source
            // guard before that path classifies copy versus move; either classification would make
            // an owning local a second destroyer of the shallow field value.
            if (operatorText == "=" && !destIsStructField
                && namedVar.FieldName.empty()
                && (llvm::isa<llvm::AllocaInst>(destination)
                    || llvm::isa<llvm::GlobalVariable>(destination))
                && compiler->IsOwningValueType(namedVar.TypeAndValue.TypeName)
                && !namedVar.TypeAndValue.Pointer
                && RejectConsumeOfBorrowedByValueParamField(compiler, rightNV, ctx,
                    rightNV.IsElementAccess && !rightNV.MovePathHasDynamicIndex
                        && IsUniqueFieldRead(rightNV)))
                return finishStore(right);

            if (operatorText == "=" && destIsStructField)
                RejectFieldAllocAlignMismatch(
                    namedVar.TypeAndValue, namedVar.AllocAlignment, rightNV, right,
                    namedVar.FieldName.empty()
                        ? std::string("a field")
                        : std::format("field '{}'", namedVar.FieldName),
                    ctx,
                    slotOwnerIsCoreUnique());
            // An ELEMENT slot and a DEREFERENCED slot reach the same synthesized free as a field:
            // the deallocator is chosen from the SLOT's declared type, so an over-aligned block
            // stored here is freed with the ordinary `operator delete` (heap corruption). Same
            // rule, same door - a slot is a binding for the allocation-alignment rule too.
            else if (operatorText == "=" && !destIsStructField
                && namedVar.TypeAndValue.AllocAlignValue <= LLVMBackend::kDefaultNewAlign
                && destination != nullptr
                && (destIsFixedArrayElem || namedVar.IsElementAccess || isDerefStorage()))
                RejectFieldAllocAlignMismatch(
                    namedVar.TypeAndValue, namedVar.TypeAndValue.AllocAlignValue, rightNV, right,
                    namedVar.CallerName.empty()
                        ? std::string(destIsFixedArrayElem || namedVar.IsElementAccess
                                      ? "an element slot" : "a dereferenced slot")
                        : std::format("{} '{}'",
                                      destIsFixedArrayElem || namedVar.IsElementAccess
                                          ? "an element of" : "the slot behind",
                                      namedVar.CallerName),
                    ctx, false, "slot");
            // A GLOBAL slot is the last destination that reaches the synthesized free through its
            // DECLARED type, and it passes none of the doors above (no GEP, no element access, no
            // deref). Same rule, same message: an over-aligned block parked in an unclaused global
            // is freed with the ordinary deallocator, and a clause-bearing global must be fed a
            // matching allocation. `static` locals already reach the LOCAL door and are excluded.
            // A global is also a plain borrow slot, so the clause-bearing direction fires only on
            // PRECISE provenance (see GlobalStoreAllocAlignIsPrecise); unknown sources compile.
            else if (operatorText == "=" && !destIsStructField
                && namedVar.FieldName.empty() && !namedVar.IsElementAccess
                && !namedVar.IsStaticLocal
                && llvm::isa_and_nonnull<llvm::GlobalVariable>(destination)
                && (namedVar.TypeAndValue.AllocAlignValue <= LLVMBackend::kDefaultNewAlign
                    || GlobalStoreAllocAlignIsPrecise(compiler, rightNV, right)))
                RejectFieldAllocAlignMismatch(
                    namedVar.TypeAndValue, namedVar.TypeAndValue.AllocAlignValue, rightNV, right,
                    GlobalSlotDescription(namedVar),
                    ctx, false, "global");

            if (operatorText == "=" && destIsStructField
                && !SourceIsDanglingAliasBorrow(compiler, rightNV)
                && RejectStoreIntoBorrowedField(compiler, namedVar, ctx))
                return finishStore(right);

            // Closure store (Option A): closures are clone-safe, so named-source assignment auto-clones.
            // Old dest env freed for alloca/global only - skipped for struct-field GEP (slot may be uninitialized).
            // Keyed on the REPRESENTATION, not the spelling: a generic-substituted fat field
            // carries an ENCODED name but is the same `__closure_fat_ptr` struct and owns an env.
            bool destIsClosureFat = namedVar.TypeAndValue.TypeName == "__closure_fat_ptr"
                || (!namedVar.TypeAndValue.Pointer
                    && (namedVar.BaseType == compiler->GetClosureFatPtrType()
                        || compiler->IsFatEncodedClosureType(namedVar.TypeAndValue.TypeName)));
            if (operatorText == "=" && right && right->getType()->isStructTy() && destIsClosureFat)
            {
                right = CloneClosureFromNamedSource(rightNV, right, ctx);
                if (auto* destMeta = compiler->FindStackVariableByStorage(destination))
                {
                    const bool envKnownFree = (rightNV.ClosureEnvKnownFree
                            && !rightNV.ClosureEnvProvenanceTainted)
                        || llvm::isa_and_nonnull<llvm::Function>(rightNV.Primary)
                        || compiler->ClosureIsStaticallyNonCapturing(right);
                    destMeta->ClosureEnvProvenanceTainted =
                        destMeta->ClosureEnvProvenanceTainted || !envKnownFree;
                    destMeta->ClosureEnvKnownFree =
                        envKnownFree && !destMeta->ClosureEnvProvenanceTainted;
                }
                // CFlat value-type method receivers are constructed with null closure fields.
                // Free the old env only for initialized slots; another heap pointer may be raw.
                // The clone above reads the SOURCE env first, so a self-assign (`f = f`) is safe:
                // the independent clone exists before the old env is freed.
                auto thisVar = compiler->GetThisPointer();
                auto* thisStorage = llvm::dyn_cast_or_null<llvm::AllocaInst>(thisVar.Storage);
                auto isThisPointerLoad = [&](llvm::Value* slotBase) {
                    auto* base = slotBase != nullptr ? slotBase->stripPointerCasts() : nullptr;
                    auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(base);
                    return thisStorage != nullptr && load != nullptr
                        && load->getPointerOperand()->stripPointerCasts() == thisStorage;
                };
                bool destSlotInitialized =
                    llvm::isa<llvm::AllocaInst>(destination) || llvm::isa<llvm::GlobalVariable>(destination);
                if (!destSlotInitialized && destIsStructField && destGep != nullptr)
                {
                    auto* base = destGep->getPointerOperand()->stripPointerCasts();
                    destSlotInitialized = llvm::isa<llvm::AllocaInst>(base)
                        || llvm::isa<llvm::GlobalVariable>(base);
                    if (!destSlotInitialized)
                        destSlotInitialized = isThisPointerLoad(base);
                }
                if (!destSlotInitialized && !namedVar.OwningStructName.empty())
                    destSlotInitialized = isThisPointerLoad(destination);
                // A UNION member is NOT a known-initialized closure slot: every arm names the same
                // bytes, so the current contents may be an unrelated arm's value.
                if (namedVar.UnionFieldType != nullptr)
                    destSlotInitialized = false;
                if (destSlotInitialized)
                    if (auto* dtor = compiler->GetOrCreateFullDestructor("__closure_fat_ptr"))
                        compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { destination });
                derefAssign(right, rhsUnsigned);
                // The destination now owns this closure env, so drop the stored value from the
                // owned-closure temp-flush list to avoid a double-free (no-op for a clone result).
                compiler->UnregisterOwnedClosureTemp(right);
                return finishStore(right);
            }

            // A fresh immovable value is stored bitwise below, so the sret temps that feed it (one
            // load, or each ternary arm's load) hand ownership to the destination: never destroy them.
            if (operatorText == "=" && right && rightNV.IsRvalue
                && !rightNV.TypeAndValue.Pointer && !rightNV.TypeAndValue.IsAlias
                && compiler->IsImmovableCFlatStruct(rightNV.TypeAndValue.TypeName))
            {
                // Nested `?:` arms join through nested PHIs: walk every PHI down to its loads.
                llvm::SmallVector<llvm::Value*, 8> feeds{right};
                llvm::SmallPtrSet<llvm::Value*, 8> visitedFeeds;
                while (!feeds.empty())
                {
                    llvm::Value* feed = feeds.pop_back_val();
                    if (feed == nullptr || !visitedFeeds.insert(feed).second) continue;
                    if (auto* join = llvm::dyn_cast<llvm::PHINode>(feed))
                        for (llvm::Value* incoming : join->incoming_values())
                            feeds.push_back(incoming);
                    else if (auto* load = llvm::dyn_cast<llvm::LoadInst>(feed))
                        compiler->UnregisterOwnedStructTemp(load->getPointerOperand());
                }
            }
            // Field-identity of the two sides, computed up front so the field-store rejects/copies
            // below can all recognize a self-assign. Ownership is a runtime property, so a
            // field-to-field of a NON-copyable owner cannot be safely duplicated (see the rejects).
            auto* srcFieldGep = llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(rightNV.Storage);
            bool srcIsStructField = (srcFieldGep && srcFieldGep->getNumIndices() == 2
                && srcFieldGep->getSourceElementType()->isStructTy())
                || rightNV.IsInterfaceField;
            // Names cannot tell two receivers apart (an element's CallerName is the CONTAINER), so
            // only this proof can. DIAGNOSTICS may use it as-is; anything that EMITS a copy or a
            // destruct must use the -Initialized form, which also requires both slots to root in
            // stack/global storage. A slot reached through a heap pointer may hold garbage, and
            // deep-copying or destructing that is a SIGSEGV, so those paths keep the old no-op.
            // Scope after the 2026-08-10 implicit-move ruling: this proof serves the OWNING-VALUE
            // field paths only. `unique` fields no longer ask it - they always move.
            bool provablyDifferentSlots = ProvablyDifferentSlots(destination, rightNV.Storage);
            bool provablyDifferentInitializedSlots = provablyDifferentSlots
                && AddressRootIsStackOrGlobal(destination)
                && AddressRootIsStackOrGlobal(rightNV.Storage);
            bool selfFieldAssign = !namedVar.FieldName.empty()
                && namedVar.FieldName == rightNV.FieldName
                && namedVar.CallerName == rightNV.CallerName
                && !provablyDifferentSlots;
            // The store-side twin of selfFieldAssign, for the copy/destruct paths below.
            bool sameFieldStore = !namedVar.FieldName.empty()
                && namedVar.FieldName == rightNV.FieldName
                && namedVar.CallerName == rightNV.CallerName
                && !provablyDifferentInitializedSlots;

            // Self-assign of a `unique` field is one owner, not two, so the borrow/copy rejects
            // below must not fire on it. selfFieldAssign misses the bare `item = item` in a method:
            // NamedVariable comes from GetMemberVariable, which deliberately leaves FieldName
            // empty. Both sides then name a field of the same `this`, so the declared field name
            // identifies the slot (each access re-loads `this`, so the GEPs never compare equal).
            bool selfUniqueFieldAssign = selfFieldAssign
                || (namedVar.FieldName.empty() && rightNV.FieldName.empty()
                    && !namedVar.TypeAndValue.VariableName.empty()
                    && namedVar.TypeAndValue.VariableName == rightNV.TypeAndValue.VariableName);
            // Store-side twin, for the one helper below that COPIES as well as rejects.
            bool selfUniqueFieldStore = selfUniqueFieldAssign || sameFieldStore;

            // A desugared unique local/parameter is a move-only wrapper. Assigning it to a
            // desugared unique field transfers the wrapper and zeros the source slot.
            bool coreUniqueFieldMove = operatorText == "=" && destIsStructField
                && destIsUniqueFieldSlot && right != nullptr && right->getType()->isStructTy()
                && compiler->IsCoreUniqueType(namedVar.TypeAndValue.TypeName)
                && !namedVar.TypeAndValue.Pointer
                && rightNV.TypeAndValue.TypeName == namedVar.TypeAndValue.TypeName
                && !rightNV.TypeAndValue.Pointer && rightNV.Storage != nullptr
                && rightNV.Storage != destination && !IsUniqueFieldRead(rightNV);
            if (coreUniqueFieldMove)
            {
                if (auto* dtor = compiler->GetOrCreateFullDestructor(
                        namedVar.TypeAndValue.TypeName))
                    compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { destination });
                compiler->builder->CreateStore(right, destination);
                compiler->builder->CreateStore(
                    llvm::ConstantAggregateZero::get(right->getType()), rightNV.Storage);
                if (!rightNV.CallerName.empty())
                    compiler->MarkVariableMoved(rightNV.CallerName);
                uniqueFieldAssignmentResultProduced = true;
                return finishStore(right);
            }

            // A suppressed (mixed) '?:' join of an owning-value struct: neither an existing owning
            // local nor a field can carry the suppression, so reject before any store path runs.
            if (operatorText == "="
                && RejectNonOwningStructJoinStore(right, namedVar.TypeAndValue.TypeName,
                                                  destIsStructField ? "a struct field"
                                                                    : "an owning variable", ctx))
                return finishStore(right);

            // Storing a whole `alias` (borrow) value into a struct field - rejected ahead of the
            // generic owning-value reject below so the borrow keeps its precise message.
            if (operatorText == "=" && destIsStructField
                && RejectAliasStoreIntoField(rightNV, right, ctx))
                return finishStore(right);

            // Alias results also reach owning stores through dereferences and container indexers.
            // Check every addressable owning lvalue before the move/drop paths below.
            const bool destIsNamedOwningSlot = llvm::isa_and_nonnull<llvm::AllocaInst>(destination)
                || llvm::isa_and_nonnull<llvm::GlobalVariable>(destination);
            // A named borrow stored through a raw `*p` stays legal (btree lookup's `*out = found`
            // hands the caller an alias by contract); a direct alias-call result there is refused.
            auto* lhsUnary = ctx->unaryExpression();
            const bool lhsIsStarDeref = lhsUnary != nullptr && lhsUnary->unaryOperator() != nullptr
                && lhsUnary->unaryOperator()->getText() == "*";
            const bool destIsIndirectOwningSlot = destination != nullptr && (namedVar.IsElementAccess
                || (isDerefStorage() && (!lhsIsStarDeref
                    || compiler->FindStackVariableByStorage(rightNV.Storage) == nullptr)));
            if (operatorText == "=" && !destIsStructField && namedVar.FieldName.empty()
                && destination != nullptr
                && (destIsNamedOwningSlot || destIsIndirectOwningSlot)
                && compiler->IsOwningValueType(namedVar.TypeAndValue.TypeName)
                && !namedVar.TypeAndValue.Pointer
                && destination != rightNV.Storage
                && RejectAliasBorrowAdoption(rightNV, right,
                    namedVar.IsElementAccess ? "an owning array element" : "an owning variable", ctx))
                return finishStore(right);

            // The DESTINATION is a borrow local (`Box k = w.get(); k = makeBox(2);`). Its old value
            // is storage the real owner still frees, so every drop-old below must be SKIPPED here -
            // running it double-frees the owner's object. The store itself is unchanged, and the
            // borrow retires afterwards where that is provable (see RetireAliasBorrowOnRebind).
            const bool destIsAliasBorrowLocal = operatorText == "="
                && !destIsStructField && namedVar.FieldName.empty()
                && DestinationIsAliasBorrowLocal(namedVar);

            auto* uniqueArrayElementGep = rightNV.IsElementAccess
                ? llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(rightNV.Storage) : nullptr;
            const bool isConstantUniqueArrayFieldElement = !rightNV.MovePathHasDynamicIndex
                && uniqueArrayElementGep != nullptr
                && uniqueArrayElementGep->getSourceElementType()->isArrayTy()
                && IsUniqueFieldRead(rightNV);

            /*
             * A unique element read is normally a borrow-copy alias: a container keeps the object
             * and frees it at teardown. A constant-index element of a unique array field carries
             * field provenance and reaches the field consume below. Dynamic indices and fields of
             * an indexed struct stay on their existing borrow path.
             */
            if (operatorText == "=" && right && destination != nullptr
                && right->getType()->isStructTy()
                && rightNV.IsElementAccess
                && !isConstantUniqueArrayFieldElement
                && !rightNV.TypeAndValue.IsMove && !rightNV.IsExplicitMove
                && !rightNV.TypeAndValue.Pointer && !namedVar.TypeAndValue.Pointer
                && !destIsStructField && namedVar.FieldName.empty()
                && (llvm::isa<llvm::AllocaInst>(destination)
                    || llvm::isa<llvm::GlobalVariable>(destination))
                && destination != rightNV.Storage
                && compiler->IsCoreUniqueType(rightNV.TypeAndValue.TypeName)
                && rightNV.TypeAndValue.TypeName == namedVar.TypeAndValue.TypeName)
            {
                compiler->builder->CreateStore(right, destination);
                // Mutate the LIVE binding, not a fresh entry in this inner block scope: the
                // declaration's frame is the one the scope-exit destructor consults.
                if (auto* local = namedVar.CallerName.empty()
                        ? nullptr : compiler->FindLiveNamedVariable(namedVar.CallerName))
                {
                    local->IsAliasBorrow = true;
                    local->IsOwning = false;
                    local->IsOwningString = false;
                    RecordAliasBorrowDeclBlock(compiler, *local);
                }
                return finishStore(right);
            }

            // A direct read from a unique field is an implicit move when adopted by a unique local.
            // The field must be nulled before the old local value is released, matching the field-to-
            // field ruling and preventing the source field and destination local from co-owning it.
            if (operatorText == "=" && right && IsUniqueFieldRead(rightNV)
                && !destIsStructField && namedVar.FieldName.empty()
                && (llvm::isa<llvm::AllocaInst>(destination)
                    || llvm::isa<llvm::GlobalVariable>(destination))
                && namedVar.TypeAndValue.IsUnique
                && ((namedVar.TypeAndValue.Pointer && right->getType()->isPointerTy())
                    || (namedVar.TypeAndValue.IsFatInterfaceValue()
                        && right->getType()->isStructTy())))
            {
                if (RejectImplicitConsumeOfOutlivingOwner(compiler, rightNV, false, ctx))
                    return finishStore(right);
                if (RejectConsumeOfBorrowedByValueParamField(compiler, rightNV, ctx))
                    return finishStore(right);
                const bool destIsUniqueIface = namedVar.TypeAndValue.IsFatInterfaceValue();
                EmitImplicitUniqueFieldMove(
                    rightNV, right, destination,
                    destIsUniqueIface ? namedVar.TypeAndValue.TypeName : std::string(), ctx);
                if (!destIsUniqueIface)
                    compiler->EmitUniqueFieldDelete(
                        *compiler->builder, destination,
                        compiler->GetFullDestructorForDelete(namedVar.TypeAndValue.TypeName),
                        right);
                compiler->builder->CreateStore(right, destination);
                if (!namedVar.CallerName.empty())
                    compiler->SetVariableOwning(namedVar.CallerName, true);
                return finishStore(right);
            }

            // Copying a named owning value into a struct field by value. THE FLIP copies a copyable
            // owner in place (proceeds); a non-copyable owner is rejected. A self-assign is a no-op
            // (no copy - copying would leak the old buffer the self-store never frees). `rejectCopied`
            // records whether it produced the copy, so the field-to-field block below does not copy again.
            bool rejectCopied = false;
            // A lowered struct is never stored as a value: assignment runs operator= member-wise
            // from the source ADDRESS; a non-copyable owner moves, a user copy() result moves in.
            if (operatorText == "=" && right && right->getType()->isStructTy()
                && !namedVar.TypeAndValue.Pointer && !rightNV.TypeAndValue.Pointer
                && compiler->IsLoweredCFlatOnlyStruct(namedVar.TypeAndValue.TypeName)
                && rightNV.TypeAndValue.TypeName == namedVar.TypeAndValue.TypeName)
            {
                const std::string& loweredType = namedVar.TypeAndValue.TypeName;
                bool useMove = rightNV.TypeAndValue.IsMove || rightNV.IsExplicitMove;
                bool fromTemp = IsDefaultOnlyExpression(assignCtx);
                llvm::Value* source = rightNV.Storage;
                if (source != nullptr && rightNV.IsRvalue && !rightNV.IsExplicitMove
                    && !rightNV.TypeAndValue.IsMove)
                    fromTemp = true;
                // A stale call temp can still be a named slot (`a = a`): the value must be that call's.
                if (source != nullptr && right != nullptr
                    && ((right == compiler->lastLoweredRetValue_ && source == compiler->lastLoweredRetTemp_)
                        || (right == compiler->lastCxxRetValue_ && source == compiler->lastCxxRetTemp_)))
                    fromTemp = true;
                if (source == nullptr && right == compiler->lastLoweredRetValue_
                    && compiler->lastLoweredRetTemp_ != nullptr)
                {
                    source = compiler->lastLoweredRetTemp_;
                    fromTemp = true;
                }
                // `move a` hands back the value loaded before the source was zeroed: restore the
                // bytes, the moved-from source stays a live object destroyed at its scope exit.
                bool restoredMoveSource = false;
                if (source == nullptr)
                    if (auto* loaded = llvm::dyn_cast_or_null<llvm::LoadInst>(right))
                    {
                        source = loaded->getPointerOperand();
                        restoredMoveSource = rightNV.IsExplicitMove;
                    }
                if (source == nullptr)
                {
                    LogErrorContext(ctx, std::format(
                        "cannot assign struct '{}': the source is not addressable, and its C++ "
                        "fields must be assigned from their own storage", loweredType));
                    return finishStore(right);
                }
                if (restoredMoveSource)
                {
                    compiler->builder->CreateStore(right, source);
                    const std::string movedName = compiler->FindVariableNameByStorage(source);
                    if (!movedName.empty()) compiler->MarkVariableNotExplicitlyMovedNull(movedName);
                }
                const std::string sourceName = rightNV.Storage != nullptr
                    ? compiler->FindVariableNameByStorage(rightNV.Storage) : std::string();
                if (!useMove && !fromTemp && !compiler->IsCopyableType(loweredType))
                {
                    if (RejectConsumeOfBorrowedByValueParamField(compiler, rightNV, ctx))
                        return finishStore(right);
                    useMove = true;
                    if (!sourceName.empty() && rightNV.FieldName.empty())
                        compiler->MarkVariableMoved(sourceName);
                }
                else if (!useMove && !fromTemp && compiler->HasCopyOverloadFor(loweredType))
                {
                    LLVMBackend::NamedVariable copySource;
                    copySource.Storage = source;
                    copySource.BaseType = right->getType();
                    copySource.TypeAndValue.TypeName = loweredType;
                    auto* copied = compiler->CreateOverloadedFunctionCall("copy", { copySource });
                    if (copied == nullptr) return finishStore(right);
                    if (copied != compiler->lastLoweredRetValue_
                        || compiler->lastLoweredRetTemp_ == nullptr)
                    {
                        LogErrorContext(ctx, std::format(
                            "cannot assign struct '{}': its copy() result has no storage to "
                            "assign the C++ fields from", loweredType));
                        return finishStore(right);
                    }
                    source = compiler->lastLoweredRetTemp_;
                    fromTemp = true;
                }
                if (fromTemp) useMove = true;
                compiler->SetCurrentDebugLocation(ctx->getStart()->getLine());
                // A released destination (`_ = move a;`) holds destroyed C++ fields: operator=
                // on them is use-after-destroy, so the value is CONSTRUCTED member-wise instead.
                // A conditional release (released in one branch) decides at runtime by its drop
                // flag, as the foreign C++ class assignment does.
                auto* valueType = right->getType();
                auto emitAssign = [&]() -> bool {
                    return compiler->EmitLoweredMemberwiseAssign(loweredType, valueType,
                        destination, source, useMove, "in assignment");
                };
                auto emitConstruct = [&]() -> bool {
                    return useMove
                        ? compiler->EmitLoweredMemberwiseMove(loweredType, valueType, destination,
                                                              source, "in assignment")
                        : compiler->EmitLoweredMemberwiseCopy(loweredType, valueType, destination,
                                                              source, "in assignment");
                };
                bool emitted = true;
                if (namedVar.ConditionalDropFlag != nullptr)
                {
                    auto* liveBlock = compiler->CreateBasicBlock("lowered.assign.live");
                    auto* constructBlock = compiler->CreateBasicBlock("lowered.assign.construct");
                    auto* doneBlock = compiler->CreateBasicBlock("lowered.assign.join");
                    auto* live = compiler->builder->CreateLoad(
                        compiler->builder->getInt1Ty(), namedVar.ConditionalDropFlag,
                        "lowered.assign.livef");
                    compiler->CreateConditionJump(live, liveBlock, constructBlock);
                    compiler->SwitchToBlock(liveBlock);
                    emitted = emitAssign();
                    if (compiler->IsInsertBlockLive()) compiler->CreateJump(doneBlock);
                    compiler->SwitchToBlock(constructBlock);
                    emitted = emitConstruct() && emitted;
                    if (compiler->IsInsertBlockLive()) compiler->CreateJump(doneBlock);
                    compiler->SwitchToBlock(doneBlock);
                }
                else if (namedVar.ExplicitlyMovedNull)
                    emitted = emitConstruct();
                else
                    emitted = emitAssign();
                if (!emitted) return finishStore(right);
                // A prvalue source is a temporary this statement owns, moved from above: only
                // its moved-from C++ fields remain, its user destructor never runs on the husk.
                if (fromTemp)
                    if (auto* dtor = compiler->GetOrCreateMovedFromShellDestructor(loweredType))
                        compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { source });
                if (!namedVar.CallerName.empty() && namedVar.FieldName.empty())
                {
                    compiler->MarkVariableUnmoved(namedVar.CallerName);
                    // Clears the release and re-arms the drop flag: the destination is live again.
                    compiler->MarkVariableNotExplicitlyMovedNull(namedVar.CallerName);
                }
                return finishStore(right);
            }
            if (operatorText == "=" && destIsStructField
                && RejectOwningValueCopyIntoField(rightNV, right, selfUniqueFieldStore, rejectCopied, ctx))
                return finishStore(right);

            // A PROVABLE stack address into a scalar `unique` field: the shape gate keeps the
            // pointer-worded message off whole-array and interface fields, where it would be untrue.
            if (operatorText == "=" && right && destIsStructField
                && IsOwningUniquePointerField(namedVar.TypeAndValue)
                && namedVar.TypeAndValue.Pointer
                && namedVar.TypeAndValue.ConstArraySize == 0
                && !namedVar.TypeAndValue.IsInterface
                && !selfUniqueFieldAssign
                && LLVMBackend::IsProvableNonHeapAddress(right))
            {
                RejectNonHeapAddressIntoUnique(
                    std::format("unique field '{}'", DescribeUniqueFieldOwner(namedVar)), ctx);
                return finishStore(right);
            }

            // Storing a BORROW into a `unique` field. The field declares that it owns the pointee and
            // its synthesized destructor deletes it, but the borrow's real owner frees it as well.
            // This is Trap A from internal/plan/ownership-move-alias-discipline.md, diagnosable here
            // only because `unique` makes the ownership claim explicit. A `move` RHS, a `new` result,
            // `nullptr`, and ownership-transferring call results are not borrows and never reach here;
            // `h->slot = h->slot` is a self-assign, not a second owner, so it is excluded too.
            if (operatorText == "=" && right && destIsStructField
                && IsOwningUniquePointerField(namedVar.TypeAndValue)
                && rightNV.IsBorrowed
                && !rightNV.TypeAndValue.IsMove
                && !selfUniqueFieldAssign)
            {
                RejectBorrowIntoUniqueField(rightNV,
                    std::format("unique field '{}'", DescribeUniqueFieldOwner(namedVar)), ctx);
                return finishStore(right);
            }

            // Storing a plain COPY of a live OWNING local into a `unique` field. The copy carries no
            // IsBorrowed, so the clause above cannot see it, and the field's synthesized destructor
            // then frees a pointee the source frees again at scope exit. The DIRECT spelling
            // (`h.f = c;`) transfers ownership out of `c`, so it is both legal and the remedy.
            // Re-asked for liveness, so rebinding either end retires it (that copy is then the
            // sole owner and rejecting the store would LEAK).
            if (operatorText == "=" && right && destIsStructField
                && IsOwningUniquePointerField(namedVar.TypeAndValue)
                && !rightNV.TypeAndValue.IsMove
                && !selfUniqueFieldAssign
                && llvm::isa_and_nonnull<llvm::AllocaInst>(rightNV.Storage))
            {
                const auto* copyBind = compiler->FindVariableByStorage(rightNV.Storage);
                std::string srcName = rightNV.CallerName.empty()
                    ? rightNV.TypeAndValue.VariableName : rightNV.CallerName;
                if (srcName.empty()) srcName = "<expr>";
                if (copyBind != nullptr && compiler->OwningLocalCopyStillAliases(*copyBind))
                {
                    LogErrorContext(ctx, std::format(
                        "cannot store '{}' into unique field '{}' - '{}' copies '{}', which still owns "
                        "the object and frees it at scope exit, so the field's synthesized destructor "
                        "double-frees it. Store '{}' directly (that transfers ownership out of it), or "
                        "use 'move {}'.",
                        srcName, DescribeUniqueFieldOwner(namedVar), srcName,
                        copyBind->OwningLocalOrigin, copyBind->OwningLocalOrigin,
                        copyBind->OwningLocalOrigin));
                    return finishStore(right);
                }
                // The same store off a local bound from a proving '?:' / '??' JOIN. Its own delete
                // is rejected above; the field's synthesized destructor is the same second free.
                if (copyBind != nullptr && !copyBind->IsOwning
                    && JoinArmsStillKeepOwner(*copyBind)
                    && !copyBind->JoinKeepsOwnerSource.empty())
                {
                    LogErrorContext(ctx, std::format(
                        "cannot store '{}' into unique field '{}' - every arm of the join '{}' was "
                        "bound from holds an object {} already frees, so the field's synthesized "
                        "destructor double-frees it. Store an object this frame owns instead.",
                        srcName, DescribeUniqueFieldOwner(namedVar), srcName,
                        copyBind->JoinKeepsOwnerSource));
                    return finishStore(right);
                }
            }

            // Same store through a '?:' join that laundered a borrowed `move`: the joined value
            // carries no IsBorrowed, yet the field adopted a live pointee and double-freed (abort).
            std::string fieldBorrowMoveOrigin;
            if (operatorText == "=" && right && destIsStructField
                && IsOwningUniquePointerField(namedVar.TypeAndValue)
                && !rightNV.TypeAndValue.IsMove
                && !selfUniqueFieldAssign
                && compiler->IsMovedBorrowedPtrValue(rightNV.Primary, &fieldBorrowMoveOrigin))
            {
                LLVMBackend::NamedVariable borrowSrc = {};
                borrowSrc.CallerName     = fieldBorrowMoveOrigin;
                borrowSrc.BorrowedOrigin = fieldBorrowMoveOrigin;
                RejectBorrowIntoUniqueField(borrowSrc,
                    std::format("unique field '{}'", DescribeUniqueFieldOwner(namedVar)), ctx);
                return finishStore(right);
            }

            // A fat-interface slot fails both pointer-shaped tests, so it gets its own
            // destination leg (IsUniqueTempFieldRead / IsOwningUniqueInterfaceField).
            bool destOwnsUniquePointee = right && right->getType()->isPointerTy()
                && IsOwningUniquePointerField(namedVar.TypeAndValue);
            bool destOwnsUniqueInterface = right && right->getType()->isStructTy()
                && IsOwningUniqueInterfaceField(namedVar.TypeAndValue);
            bool destOwnsUniqueValue = right && right->getType()->isStructTy()
                && IsOwningUniqueField(namedVar.TypeAndValue)
                && !namedVar.TypeAndValue.Pointer && !namedVar.TypeAndValue.IsInterface;
            bool sourceIsCoreUniqueField = compiler->IsCoreUniqueType(rightNV.TypeAndValue.TypeName)
                && !rightNV.TypeAndValue.Pointer && !rightNV.TypeAndValue.IsMove
                && (srcIsStructField || !rightNV.FieldName.empty()
                    || !rightNV.OwningStructName.empty() || rightNV.IsInterfaceField);

            // A TEMPORARY's `unique` field is not an addressable source (no `move` spelling
            // either), so the implicit move below has no slot to null. Kept as a reject.
            if (operatorText == "=" && destIsStructField
                && (destOwnsUniquePointee || destOwnsUniqueInterface || destOwnsUniqueValue)
                && IsUniqueTempFieldRead(rightNV))
            {
                RejectUniqueTempFieldToField(
                    rightNV, std::format("unique field '{}'", DescribeUniqueFieldOwner(namedVar)), ctx);
                return finishStore(right);
            }

            /*
             * RULED 2026-08-10: a plain `=` between two `unique` fields is a UNIFORM IMPLICIT MOVE
             * (`c.p = a.p` nulls `a.p` and releases c.p's old pointee), with no proof, no index
             * analysis and no rejection - `move a.p` spells the same thing. The source is nulled
             * HERE, ahead of the drop-old further down, so a self-assign of any spelling
             * (`x.p = x.p`, `arr[i].p = arr[j].p` with i==j) reads an already-null destination and
             * frees nothing, exactly as the `move` spelling already did.
             */
            if (operatorText == "=" && destIsStructField
                && (destOwnsUniquePointee || destOwnsUniqueInterface || destOwnsUniqueValue)
                && (IsUniqueFieldRead(rightNV) || sourceIsCoreUniqueField))
            {
                // Nulling a borrowed by-value parameter's field nulls only the callee's copy, so
                // the caller frees the pointee twice. Same reject the `move` spelling raises.
                if (RejectConsumeOfBorrowedByValueParamField(compiler, rightNV, ctx))
                    return finishStore(right);
                if (RejectJoinedSourceIntoUniqueField(compiler, rightNV, ctx))
                    return finishStore(right);
                EmitImplicitUniqueFieldMove(rightNV, right, destination,
                    destOwnsUniqueInterface ? namedVar.TypeAndValue.TypeName : std::string(), ctx);
            }

            // A desugared field is non-copyable; move its loaded wrapper after dropping the target.
            bool destIsCoreUniqueFieldValue = destIsUniqueFieldSlot
                && compiler->IsCoreUniqueType(namedVar.TypeAndValue.TypeName)
                && !namedVar.TypeAndValue.Pointer;
            if (operatorText == "=" && right && right->getType()->isStructTy()
                && destIsCoreUniqueFieldValue
                && rightNV.TypeAndValue.TypeName == namedVar.TypeAndValue.TypeName
                && rightNV.TypeAndValue.IsMove)
            {
                if (auto* dtor = compiler->GetOrCreateFullDestructor(
                        namedVar.TypeAndValue.TypeName))
                    compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { destination });
                auto* stored = derefAssign(right, rhsUnsigned);
                uniqueFieldAssignmentResultProduced = true;
                return finishStore(stored);
            }

            // A plain (non-move) by-value `string` PARAMETER stored into a struct field: deep-copy it.
            // The parameter is a by-value copy of the caller's argument, but the CALLER still owns and
            // frees the underlying buffer (e.g. an owning concat temp passed as the argument is freed
            // at the caller's end-of-statement cleanup). A shallow store would alias that buffer into
            // the field, leaving it dangling - and silently corrupted once the freed block is reused -
            // after the caller frees the temp. A true move is impossible from here (the callee cannot
            // reach the caller's temp cleanup), so an independent owned copy is the only safe transfer;
            // it is exactly what the owning-string-into-field reject below recommends ('.copy()').
            // `move` params (handled by the move-string transfer below) and owned temps / call results
            // (null Storage, already transferred to us) keep their existing zero-copy paths.
            if (operatorText == "=" && right && right->getType()->isStructTy()
                && destIsStructField
                && NamedVarIsString(namedVar)
                && rightNV.TypeAndValue.TypeName == "string"
                && !rightNV.TypeAndValue.IsMove
                && rightNV.FieldName.empty()
                && rightNV.Storage != nullptr
                && !rightNV.CallerName.empty()
                && !sameFieldStore)
            {
                auto argNV = compiler->GetFunctionArgument(rightNV.CallerName);
                bool srcIsByValueStringParam =
                    argNV.Storage != nullptr
                    && argNV.Storage == rightNV.Storage
                    && argNV.TypeAndValue.TypeName == "string"
                    && !argNV.TypeAndValue.IsMove
                    && !argNV.IsOwningString;
                if (srcIsByValueStringParam)
                {
                    right = compiler->EmitOwnedStringDeepCopy(right);
                    rightStringImplicitCopied = true;
                }
            }

            // Implied move of a `move`-temp's owning field (`dest = makeToken().text`): the temp OWNS it,
            // so free dest's old value, store the field, and zero the source so the temp dtor skips it.
            // POINTER fields are excluded: this branch was written for owning VALUE types, and on a
            // `unique Item*` field it destructs the destination SLOT's address (crashes codegen).
            // A temp's core unique<T> field never implicitly moves into a destination of a
            // DIFFERENT type: the wrapper bits would be reinterpreted and its pointee stranded.
            if (operatorText == "=" && right && rightNV.MovableTempField
                && compiler->IsCoreUniqueType(rightNV.TypeAndValue.TypeName)
                && namedVar.TypeAndValue.TypeName != rightNV.TypeAndValue.TypeName)
                GuardOwningTempUniqueFieldEscape(rightNV, "a longer-lived location", ctx);
            if (operatorText == "=" && right && rightNV.MovableTempField
                && !rightNV.TypeAndValue.Pointer
                && compiler->IsOwningValueType(rightNV.TypeAndValue.TypeName))
            {
                if (!destIsAliasBorrowLocal)
                    if (compiler->GetOrCreateFullDestructor(namedVar.TypeAndValue.TypeName) != nullptr)
                        compiler->DropValue(namedVar);
                compiler->builder->CreateStore(right, destination);
                RetireAliasBorrowOnRebind(compiler, namedVar, assignmentBlock);
                auto* srcGep = compiler->builder->CreateStructGEP(
                    rightNV.MoveTempStructType, rightNV.MoveTempStructAlloca, rightNV.MoveTempFieldIndex, "movedfld");
                compiler->builder->CreateStore(
                    llvm::ConstantAggregateZero::get(right->getType()), srcGep);
                return finishStore(right);
            }

            // Storing an owning field of a NON-move (alias) temp (`x = aliasTok().text`) into a longer-
            // lived slot REJECTED: the buffer is owned elsewhere, so a 2nd owner double-frees. (Move-temps
            // took the implied-move branch above.)
            // A `string` field instead stores by IMPLICIT COPY (see AdoptImplicitStringTempCopy).
            if (operatorText == "=" && rightNV.FromOwningTempField
                && rightNV.TypeAndValue.TypeName == "string"
                && (rightStringImplicitCopied = AdoptImplicitStringTempCopy(rightNV, right, ctx)))
            {
            }
            else if (operatorText == "=" && rightNV.FromOwningTempField
                && compiler->IsOwningValueType(rightNV.TypeAndValue.TypeName))
            {
                LogErrorContext(ctx, std::format(
                    "cannot store '{}.{}' taken from a temporary into a longer-lived location; its buffer "
                    "is owned elsewhere and would double-free. Use '.copy()' for an independent copy, or "
                    "bind the whole call result to a local first and assign from that.",
                    SpellType(*compiler, LLVMBackend::TypeAndValue{
                        .TypeName = rightNV.OwningStructName }), rightNV.FieldName));
                return finishStore(right);
            }

            // A string from an alias accessor is a borrowed view of the container slot;
            // a field store would make that buffer outlive its owner.
            if (operatorText == "=" && destIsStructField
                && NamedVarIsString(namedVar)
                && !namedVar.TypeAndValue.IsAlias
                && rightNV.TypeAndValue.IsAlias
                && !rightNV.TypeAndValue.IsMove)
            {
                const bool namedAliasLocal = rightNV.Storage != nullptr
                    && !rightNV.IsElementAccess && !rightNV.CallerName.empty();
                // A string taken from a TEMPORARY stores by IMPLICIT COPY under the ruling. A NAMED
                // `alias` string local stays rejected: it is a deliberate borrow the user spelled,
                // and silently copying it would turn that borrow into a second owner.
                const bool copied = !namedAliasLocal
                    && (rightStringImplicitCopied = AdoptImplicitStringTempCopy(rightNV, right, ctx));
                if (!copied)
                {
                    const char* sourceText = namedAliasLocal
                        ? "an 'alias' string local" : "a string taken from a temporary";
                    LogErrorContext(ctx, std::format(
                        "cannot store {} into a longer-lived location; "
                        "its buffer is owned elsewhere and would be freed out from under the field. "
                        "Use '.copy()' for an independent copy.", sourceText));
                    return finishStore(right);
                }
            }

            // The whole-ELEMENT sibling for a destination the struct-field gate above does not
            // cover (`a = fields.get(0);` into a plain string local, a global, an array slot):
            // the borrow outlives the expression there too, so it takes the same implicit copy.
            // A DECLARED `alias` destination keeps the borrow the user spelled.
            if (operatorText == "=" && right != nullptr && !destIsStructField
                && !namedVar.TypeAndValue.IsAlias
                && IsImplicitCopyableStringTemp(rightNV))
            {
                rightStringImplicitCopied = AdoptImplicitStringTempCopy(rightNV, right, ctx);
            }

            // Same escape into a BORROWING destination (field, local, global, array element) with
            // a dtor-LESS pointee. After the gate above, so a dtor-bearing pointee keeps its wording.
            if (operatorText == "=")
            {
                // The guard THROWS when it rejects, so the skip below is what the original
                // 'return finishStore' was: insurance if LogErrorContext ever stopped throwing.
                GuardOwningTempUniqueFieldEscape(rightNV, "a longer-lived location", ctx);
                if (IsOwningTempUniqueFieldEscape(rightNV)) return finishStore(right);
                const auto* destinationBinding = compiler->FindVariableByStorage(destination);
                const auto* destinationType = destinationBinding != nullptr
                    ? &destinationBinding->TypeAndValue
                    : compiler->FindDeclaredTypeAndValueForStorage(destination);
                const auto* rawPointerDestinationType = destinationType;
                if (destIsStructField || !namedVar.FieldName.empty()
                    || namedVar.IsElementAccess || rawPointerDestinationType == nullptr)
                    rawPointerDestinationType = &namedVar.TypeAndValue;
                if (rawPointerDestinationType != nullptr)
                {
                    std::string destinationName = !namedVar.FieldName.empty()
                        ? namedVar.FieldName
                        : namedVar.IsElementAccess ? "array element"
                        : !namedVar.CallerName.empty() ? namedVar.CallerName
                        : !rawPointerDestinationType->VariableName.empty()
                            ? rawPointerDestinationType->VariableName : std::string();
                    if (destinationName.empty())
                        destinationName = compiler->FindVariableNameByStorage(destination);
                    if (destinationName.empty())
                        for (const auto& [globalName, global] : compiler->globalNamedVariable)
                            if (global == destination) { destinationName = globalName; break; }
                    GuardRawPointerIntoOwningTempField(rightNV,
                        *rawPointerDestinationType,
                        true,
                        namedVar.TypeAndValue.IsAlias || namedVar.IsAliasBorrow
                            || (destinationBinding != nullptr && destinationBinding->IsAliasBorrow)
                            || destIsAliasBorrowLocal,
                        destinationName, ctx);
                }
            }

            // A named owner or program-lifetime string source copies into the field. Field borrows
            // stay rejected because copying them would silently change the deliberate borrow.
            const bool rightHasProgramLifetimeStorage = IsProgramLifetimeStorage(rightNV);
            if (operatorText == "=" && right && right->getType()->isStructTy()
                && destIsStructField
                && NamedVarIsString(namedVar)
                && rightNV.TypeAndValue.TypeName == "string"
                && (rightNV.IsOwningString || rightNV.BorrowsOwnedString
                    || rightHasProgramLifetimeStorage)
                && rightNV.Storage != nullptr
                && !rightNV.TypeAndValue.IsMove
                && !rightStringImplicitCopied
                && !sameFieldStore)
            {
                if ((rightNV.IsOwningString || rightHasProgramLifetimeStorage)
                    && !rightNV.BorrowsOwnedString)
                {
                    right = compiler->EmitOwnedStringDeepCopy(right);
                    rightStringImplicitCopied = true;
                }
                else
                {
                    LogErrorContext(ctx, std::format(
                        "assigning a non-owning string '{}' into a struct field; use 'move' to transfer "
                        "ownership or '.copy()' for an independent copy", rightNV.CallerName));
                    return finishStore(right);
                }
            }

            // Field-to-field copy of an owning value. THE FLIP: a copyable owner COPIES field-to-field
            // (independent duplicate), leaving the source field live; the old destination field is
            // destructed below before the copy is stored. A NON-copyable owner (owns a `unique`, no
            // copy()) stays REJECTED - it has no safe generic duplicate.
            if (operatorText == "=" && right && right->getType()->isStructTy()
                && destIsStructField && srcIsStructField
                && !sameFieldStore
                && !rightNV.TypeAndValue.IsMove
                && rightNV.TypeAndValue.TypeName == namedVar.TypeAndValue.TypeName
                && compiler->IsOwningValueType(namedVar.TypeAndValue.TypeName)
                && !(destOwnsUniqueValue && IsUniqueFieldRead(rightNV)))
            {
                if (compiler->IsCopyableType(namedVar.TypeAndValue.TypeName))
                {
                    // RejectOwningValueCopyIntoField above already copied this copyable owner
                    // (rejectCopied) - copying again would orphan (leak) the first copy. Only copy
                    // here when it bailed (e.g. a source with no CallerName it could not classify).
                    if (!rejectCopied && !rightStringImplicitCopied)
                        right = EmitCopyableOwnerCopy(rightNV, right, ctx);
                }
                else
                {
                    LogErrorContext(ctx, std::format(
                        "field-to-field copy of owning value '{}' aliases its backing buffer and will "
                        "double-free at teardown; use '.copy()' for an independent copy or 'move' to "
                        "transfer ownership", SpellType(*compiler, namedVar.TypeAndValue)));
                    return finishStore(right);
                }
            }

            // Owning-value MOVE at reassignment: a shallow store aliases owned buffers (double-free).
            // Destruct the old destination first, then move source bits into it and zero the source.
            // Interface targets are excluded: the upcast above replaced `right` with a fat-ptr that
            // ALIASES the source's storage (dataPtr = &source), so zeroing the source would corrupt
            // the object the interface now points at (it reads back default/zero fields). An owning-
            // value bound to an interface is a borrow of the source storage - the source keeps
            // ownership and frees at scope exit (heap-`new` is the way to hand an interface an
            // independently-owned object). This mirrors the pointer source case, which is likewise
            // excluded by `!rightNV.TypeAndValue.Pointer`.
            // A source with STORAGE is an lvalue: a named slot (alloca/global) or an INDIRECT one
            // (`*ap`, `w.b`, `arr[i]`) whose Storage is the GEP / loaded address, and has no name.
            bool srcIsNamedSlot = rightNV.Storage != nullptr
                && (llvm::isa<llvm::AllocaInst>(rightNV.Storage)
                    || llvm::isa<llvm::GlobalVariable>(rightNV.Storage))
                && !rightNV.CallerName.empty();
            // An immovable call result carries its sret temp as Storage: a fresh value, not an lvalue.
            const bool srcIsLoweredPrvalue = rightNV.IsRvalue
                && !rightNV.IsExplicitMove && !rightNV.TypeAndValue.IsMove
                && (compiler->IsLoweredCFlatOnlyStruct(rightNV.TypeAndValue.TypeName)
                    || compiler->IsImmovableCFlatStruct(rightNV.TypeAndValue.TypeName));
            // Both are writable, so both are consumable. Excluded: an `alias`/borrow binding (the
            // separate alias-launder family) and an indirect `string` (its own owned-bit paths).
            bool srcIsOwningAssignLvalue = rightNV.Storage != nullptr
                && !srcIsLoweredPrvalue
                && !SourceIsDanglingAliasBorrow(compiler, rightNV)
                && (srcIsNamedSlot || rightNV.TypeAndValue.TypeName != "string");
            // String-field copies must not recopy a materialized ternary join or owned temporary.
            const bool stringFieldCopy = right != nullptr && right->getType()->isStructTy()
                && destIsStructField && NamedVarIsString(namedVar)
                && rightNV.TypeAndValue.TypeName == "string";
            const bool srcIsStorageJoin = stringFieldCopy && rightNV.Storage != nullptr
                && (llvm::isa<llvm::PHINode>(rightNV.Storage)
                    || llvm::isa<llvm::SelectInst>(rightNV.Storage));
            const bool srcIsOwnedTemp = stringFieldCopy && compiler->IsOwnedTempValue(rightNV);
            const bool srcIsPointerVariableSubscript = stringFieldCopy
                && IsDeclaredPointerSubscriptString(assignedExpr, right, rightNV.Storage);
            bool srcIsDerefLvalue = rightNV.Storage != nullptr && rightNV.BaseType != nullptr
                && !llvm::isa<llvm::AllocaInst>(rightNV.Storage)
                && !llvm::isa<llvm::GlobalVariable>(rightNV.Storage)
                && (!llvm::isa<llvm::GetElementPtrInst>(rightNV.Storage)
                    || srcIsPointerVariableSubscript)
                && (!stringFieldCopy || (!srcIsStorageJoin && !srcIsOwnedTemp));
            // A string loaded through a pointer loses the pointee's owner/borrow marker at the
            // dereference. A field destination outlives this expression, so preserve the source
            // and store an independent string copy just like a plain string local source.
            if (operatorText == "=" && right != nullptr && right->getType()->isStructTy()
                && destIsStructField && NamedVarIsString(namedVar)
                && rightNV.TypeAndValue.TypeName == "string"
                && srcIsDerefLvalue
                && !rightNV.TypeAndValue.IsMove
                && !rightNV.TypeAndValue.IsAlias && !rightNV.IsAliasBorrow
                && !rightStringImplicitCopied)
            {
                right = compiler->EmitOwnedStringDeepCopy(right);
                rightStringImplicitCopied = true;
            }
            if (operatorText == "=" && right && right->getType()->isStructTy()
                && !namedVar.TypeAndValue.IsInterface
                && (llvm::isa<llvm::AllocaInst>(destination) || llvm::isa<llvm::GlobalVariable>(destination))
                && srcIsOwningAssignLvalue
                && !srcIsDerefLvalue
                // A `move` PARAMETER is an owner (IsOwningStruct) and destructs at function exit,
                // so assigning it into an owning slot must TRANSFER just like an owned local.
                && (!rightNV.TypeAndValue.IsMove || rightNV.IsOwningStruct)
                && !rightNV.TypeAndValue.Pointer
                && !rightNV.TypeAndValue.IsArrayView
                && !rightNV.TypeAndValue.IsInterfacePointer
                && !rightNV.TypeAndValue.IsFunctionPointer
                && destination != rightNV.Storage
                // This arm returns early; a different aggregate type must fall through to the
                // normal assignment diagnostic instead of receiving a partial-width store.
                && compiler->ResolveTypeAlias(rightNV.TypeAndValue.TypeName)
                    == compiler->ResolveTypeAlias(namedVar.TypeAndValue.TypeName)
                && compiler->IsOwningValueType(rightNV.TypeAndValue.TypeName))
            {
                // THE FLIP via the shared decision: a copyable owner COPIES (source stays live), a
                // non-copyable owner MOVES. The copy/right value is produced BEFORE the old
                // destination is destructed (self-assign is guarded by destination != rightNV.Storage
                // above). On a Move, zero the moved-out source so its always-run scope-exit destructor
                // is a no-op (cflat value-type move = zero the source) and mark it moved.
                if (RejectConsumeOfBorrowedByValueParamField(compiler, rightNV, ctx))
                    return finishStore(right);
                AssignSourceKind kind;
                llvm::Value* toStore = ClassifyOwningAssignSource(
                    right, rightNV.TypeAndValue.TypeName, rightNV.TypeAndValue.IsMove, ctx, kind);
                if (kind == AssignSourceKind::Move
                    && RejectImplicitConsumeOfOutlivingOwner(
                        compiler, rightNV, rightNV.TypeAndValue.IsMove, ctx))
                    return finishStore(right);
                if (!destIsAliasBorrowLocal)
                    emitOwningDestinationDrop();
                compiler->builder->CreateStore(toStore, destination);
                compiler->RearmConditionalDropFlag(namedVar);
                RetireAliasBorrowOnRebind(compiler, namedVar, assignmentBlock);
                if (kind == AssignSourceKind::Move)
                {
                    compiler->builder->CreateStore(
                        llvm::ConstantAggregateZero::get(toStore->getType()), rightNV.Storage);
                    // Only a named slot has a spelling to report a later use of; an indirect lvalue
                    // (field/element/deref) is consumed silently, exactly as `move w.b` already is.
                    if (srcIsNamedSlot)
                    {
                        compiler->MarkVariableMoved(rightNV.CallerName);
                        if (compiler->HasForeignNontrivialCxxField(rightNV.TypeAndValue.TypeName)
                            || (compiler->IsCoreUniqueType(rightNV.TypeAndValue.TypeName)
                                && !rightNV.TypeAndValue.Pointer && rightNV.FieldName.empty()))
                            compiler->MarkVariableExplicitlyMovedNull(rightNV.CallerName);
                    }
                }
                // The destination is now live again (it may have been moved-from earlier).
                if (!namedVar.CallerName.empty() && namedVar.FieldName.empty())
                {
                    compiler->MarkVariableUnmoved(namedVar.CallerName);
                    compiler->MarkVariableNotExplicitlyMovedNull(namedVar.CallerName);
                }
                return finishStore(toStore);
            }

            // A dereferenced pointer on the RHS still targets a whole local/global destination
            // (`o = *op`), so it cannot use the LHS-deref arm below. Handle it here, including the
            // runtime same-address check needed when `op` points at `o`.
            if (operatorText == "=" && right && right->getType()->isStructTy()
                && srcIsDerefLvalue
                && !namedVar.TypeAndValue.IsInterface
                && (llvm::isa<llvm::AllocaInst>(destination) || llvm::isa<llvm::GlobalVariable>(destination))
                && !namedVar.TypeAndValue.Pointer
                && !namedVar.TypeAndValue.IsArrayView
                && !namedVar.TypeAndValue.IsInterfacePointer
                && !namedVar.TypeAndValue.IsFunctionPointer
                && (!rightNV.TypeAndValue.IsMove || rightNV.IsOwningStruct)
                && compiler->ResolveTypeAlias(rightNV.TypeAndValue.TypeName)
                    == compiler->ResolveTypeAlias(namedVar.TypeAndValue.TypeName)
                && compiler->IsOwningValueType(namedVar.TypeAndValue.TypeName))
            {
                AssignSourceKind kind;
                llvm::Value* toStore = ClassifyOwningAssignSource(
                    right, namedVar.TypeAndValue.TypeName, rightNV.TypeAndValue.IsMove, ctx, kind);
                if (kind == AssignSourceKind::Move
                    && RejectConsumeOfBorrowedByValueParamField(compiler, rightNV, ctx))
                    return finishStore(right);
                if (kind == AssignSourceKind::Move
                    && RejectImplicitConsumeOfOutlivingOwner(
                        compiler, rightNV, rightNV.TypeAndValue.IsMove, ctx))
                    return finishStore(right);
                if (kind == AssignSourceKind::Move && destination == rightNV.Storage)
                    return finishStore(right);

                if (kind == AssignSourceKind::Move)
                {
                    auto* selfBlock = compiler->CreateBasicBlock("ownassign_self");
                    auto* moveBlock = compiler->CreateBasicBlock("ownassign_move");
                    auto* doneBlock = compiler->CreateBasicBlock("ownassign_done");
                    auto* same = compiler->builder->CreateICmpEQ(destination, rightNV.Storage,
                        "ownassign_same");
                    compiler->CreateConditionJump(same, selfBlock, moveBlock);
                    compiler->SwitchToBlock(moveBlock);
                    if (auto* dtor = compiler->GetOrCreateFullDestructor(namedVar.TypeAndValue.TypeName))
                        compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { destination });
                    compiler->builder->CreateStore(toStore, destination);
                    compiler->RearmConditionalDropFlag(namedVar);
                    compiler->builder->CreateStore(
                        llvm::ConstantAggregateZero::get(toStore->getType()), rightNV.Storage);
                    compiler->CreateJump(doneBlock);
                    compiler->SwitchToBlock(selfBlock);
                    compiler->CreateJump(doneBlock);
                    compiler->SwitchToBlock(doneBlock);
                    auto* result = compiler->builder->CreatePHI(toStore->getType(), 2, "ownassign_result");
                    result->addIncoming(right, selfBlock);
                    result->addIncoming(toStore, moveBlock);
                    return finishStore(result);
                }

                if (auto* dtor = compiler->GetOrCreateFullDestructor(namedVar.TypeAndValue.TypeName))
                    compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { destination });
                compiler->builder->CreateStore(toStore, destination);
                compiler->RearmConditionalDropFlag(namedVar);
                return finishStore(toStore);
            }

            /*
             * Same transfer into a FIXED-ARRAY ELEMENT (`dst[0] = a`, `w.arr[0] = *bp`). The
             * two-index array GEP is neither an alloca/global nor a struct field, so every arm
             * above skipped it and the plain store below aliased the source (double free) and
             * orphaned the old element (leak). The element IS live - a fixed array default- or
             * zero-constructs every slot - so drop-old applies here, unlike Part 6's container slot.
             * `string` elements carry a runtime owned bit, so they take the dedicated arm below.
             */
            if (operatorText == "=" && right && right->getType()->isStructTy()
                && destIsFixedArrayElem
                && srcIsOwningAssignLvalue
                && !namedVar.TypeAndValue.IsInterface
                && !namedVar.TypeAndValue.Pointer
                && !namedVar.TypeAndValue.IsArrayView
                && (!rightNV.TypeAndValue.IsMove || rightNV.IsOwningStruct)
                && !rightNV.TypeAndValue.Pointer
                && !rightNV.TypeAndValue.IsArrayView
                && !rightNV.TypeAndValue.IsInterfacePointer
                && !rightNV.TypeAndValue.IsFunctionPointer
                && destination != rightNV.Storage
                && namedVar.TypeAndValue.TypeName != "string"
                // Same type on both sides (alias-resolved, per the `using` lesson). This arm
                // returns early, so a mismatch must fall through to the existing diagnostic.
                && compiler->ResolveTypeAlias(rightNV.TypeAndValue.TypeName)
                    == compiler->ResolveTypeAlias(namedVar.TypeAndValue.TypeName)
                && compiler->IsOwningValueType(rightNV.TypeAndValue.TypeName))
            {
                // Ordered for SELF-ASSIGN safety (`dst[i] = dst[i]`, which no compile-time proof
                // can rule out for a runtime index): produce the value, zero the source, and only
                // then destruct the destination. When the two addresses coincide the destructor
                // sees the already-zeroed slot and frees nothing, so the value survives the store.
                AssignSourceKind kind;
                llvm::Value* toStore = ClassifyOwningAssignSource(
                    right, rightNV.TypeAndValue.TypeName, rightNV.TypeAndValue.IsMove, ctx, kind);
                if (kind == AssignSourceKind::Move
                    && RejectConsumeOfBorrowedByValueParamField(compiler, rightNV, ctx))
                    return finishStore(right);
                if (kind == AssignSourceKind::Move
                    && RejectImplicitConsumeOfOutlivingOwner(
                        compiler, rightNV, rightNV.TypeAndValue.IsMove, ctx))
                    return finishStore(right);
                if (kind == AssignSourceKind::Move)
                {
                    compiler->builder->CreateStore(
                        llvm::ConstantAggregateZero::get(toStore->getType()), rightNV.Storage);
                    // Only a named slot has a spelling to report a later use of; an indirect
                    // lvalue (field/element/deref) is consumed silently, as `move w.b` already is.
                    if (srcIsNamedSlot)
                        compiler->MarkVariableMoved(rightNV.CallerName);
                }
                if (auto* dtor = compiler->GetOrCreateFullDestructor(namedVar.TypeAndValue.TypeName))
                    compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { destination });
                compiler->builder->CreateStore(toStore, destination);
                return finishStore(toStore);
            }

            /*
             * The `string` twin of the element transfer above. A bare `string` element is neither
             * a struct field nor a local/global, so both dedicated string arms skipped it and the
             * plain store bit-copied the {ptr,len,owned} aggregate: the source kept its owned bit
             * and the element got a copy of it, so both freed the buffer (double free), and the
             * old element was orphaned (leak). Deep-copy an LVALUE source so the element owns an
             * independent buffer exactly as the field arm does, then drop the old element (the
             * string dtor is null/owned-bit guarded, so a never-assigned or borrowed slot is a
             * safe no-op). ORDER is copy-FIRST: no compile-time proof rules out `dst[i] = dst[i]`
             * for a runtime index, and destructing first would deep-copy a freed buffer.
             */
            if (operatorText == "=" && right && right->getType()->isStructTy()
                && destIsFixedArrayElem
                && NamedVarIsString(namedVar)
                && !namedVar.TypeAndValue.Pointer
                // By REPRESENTATION, not by spelling: an `operator+` temp's NamedVariable carries
                // no TypeName, and it is exactly the source whose old element leaked.
                && right->getType() == llvm::StructType::getTypeByName(*compiler->context, "string"))
            {
                // An lvalue source keeps its own buffer, so the element needs an independent one.
                // A `move` or an owned temp / call result (no Storage) already hands us the buffer.
                if (rightNV.Storage != nullptr && !rightNV.TypeAndValue.IsMove)
                    right = compiler->EmitOwnedStringDeepCopy(right);
                if (auto* dtor = compiler->GetOrCreateFullDestructor("string"))
                    compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { destination });
                compiler->builder->CreateStore(right, destination);
                TransferMoveStringOwnershipOnStore(rightNV, ctx);
                return finishStore(right);
            }

            // Owning-value MOVE through a pointer-deref destination (`*pc = *pa`): a shallow store
            // would alias owned buffers (double-free) and orphan the destination's old value (leak).
            // A deref lvalue is neither an alloca/global (destIsLocalOwningVar) nor a 2-index
            // struct-field GEP (destIsStructField), so both paths above skip it. Container element
            // stores remain excluded; pointer-parameter array spelling is not an element-access
            // value and still carries the raw pointee storage needed here. Destruct the old
            // destination, move the source bits in, then
            // zero the source so its scope-exit dtor is a no-op. Use-after-move through a pointer is
            // left unenforced - a deref source (`*pa`) has no name to MarkVariableMoved.
            if (operatorText == "=" && right && destination
                && !namedVar.BitfieldStorage && !namedVar.UnionFieldType
                && right->getType()->isStructTy()
                && isDerefStorage()
                // Let the copyable captured owner use the whole-slot cleanup path below. The
                // dereference arm clones a fresh call result and leaves that owned temp orphaned.
                && !destIsByRefClosureValue
                && !namedVar.TypeAndValue.IsInterface
                && (!namedVar.TypeAndValue.Pointer || NamedVarIsString(namedVar))
                && !namedVar.TypeAndValue.IsInterfacePointer
                && !namedVar.TypeAndValue.IsFunctionPointer
                && !namedVar.TypeAndValue.IsArrayView
                // Same clause the alloca/global arm carries: a `move` PARAMETER is an owner
                // (IsOwningStruct) that destructs at function exit, so it must TRANSFER here too.
                && (!rightNV.TypeAndValue.IsMove || rightNV.IsOwningStruct
                    || compiler->IsProducedTempValue(right)
                    || rightNV.Storage == nullptr)
                && destination != rightNV.Storage
                && (compiler->IsOwningValueType(namedVar.TypeAndValue.TypeName)
                    || NamedVarIsString(namedVar)))
            {
                // THE FLIP via the shared decision: a copyable owner COPIES into the deref lvalue
                // (source stays live - no zero, no mark-moved); a non-copyable owner MOVES. Produce
                // the value before destructing the old destination. `move` rides the same argument
                // the alloca/global sibling passes, so a `move` source TRANSFERS for any owner.
                // A source that already hands us a SOLE-OWNED buffer - an owned call temp
                // (`p.name = a + b`), or a value an earlier arm on this same store already
                // deep-copied - transfers instead of copying: copying it again strands the
                // original, which has no other owner and nothing ever frees (a leak).
                // `string` only: an owning STRUCT temp is already destructed by the statement's
                // owned-temp cleanup, so copy-then-drop is leak-free there and stays as it is.
                const bool srcHandsOverOwnedBuffer = NamedVarIsString(namedVar)
                    && (rightStringImplicitCopied
                        || (rightNV.Storage == nullptr && compiler->IsProducedTempValue(right)));
                AssignSourceKind kind = AssignSourceKind::Move;
                llvm::Value* toStore = right;
                if (!srcHandsOverOwnedBuffer)
                {
                    toStore = ClassifyOwningAssignSource(
                        right, namedVar.TypeAndValue.TypeName, rightNV.TypeAndValue.IsMove, ctx, kind);
                    if (kind == AssignSourceKind::Move
                        && RejectConsumeOfBorrowedByValueParamField(compiler, rightNV, ctx))
                        return finishStore(right);
                    if (kind == AssignSourceKind::Move
                        && RejectImplicitConsumeOfOutlivingOwner(
                            compiler, rightNV, rightNV.TypeAndValue.IsMove, ctx))
                        return finishStore(right);
                }
                if (auto* dtor = compiler->GetOrCreateFullDestructor(namedVar.TypeAndValue.TypeName))
                    compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { destination });
                auto* assignResult = derefAssign(toStore, rhsUnsigned);
                // A handed-over temp has no source slot to clear and no name to mark moved.
                if (kind == AssignSourceKind::Move && !srcHandsOverOwnedBuffer)
                {
                    // Zero the moved-out source (a named local or `*pa`) so its dtor frees nothing.
                    if (rightNV.Storage != nullptr)
                        compiler->builder->CreateStore(
                            llvm::ConstantAggregateZero::get(toStore->getType()), rightNV.Storage);
                    // Enforce use-after-move only for a plain named source; a deref source has no name.
                    bool srcIsNamedVar = rightNV.Storage != nullptr
                        && (llvm::isa<llvm::AllocaInst>(rightNV.Storage)
                            || llvm::isa<llvm::GlobalVariable>(rightNV.Storage));
                    if (srcIsNamedVar && !rightNV.CallerName.empty())
                        compiler->MarkVariableMoved(rightNV.CallerName);
                }
                if (NamedVarIsString(namedVar))
                    TransferMoveStringOwnershipOnStore(rightNV, ctx);
                return finishStore(assignResult);
            }

            // Destruct old value of an owning-value-type struct FIELD or whole LOCAL variable before
            // overwriting (closes the reassignment LEAK). For a struct field this guards field stores;
            // for a local/global variable it covers `r = f()` where f returns an owned temp (the named-
            // variable-RHS move is already handled and returned above at the MOVE path, and an explicit
            // `move` RHS is nulled below, so only a temp/call-result RHS reaches here for a local dest).
            // Container element stores are excluded (would double-free if touched here). A FIXED
            // array element joins the same set: an lvalue source already returned from the element
            // transfer arm above, so only a temp / call-result RHS reaches here, and its old element
            // would otherwise be orphaned. A `string` element dropped its old value in its own
            // arm above and returned, so it must not be dropped a second time here.
            bool destIsLocalOwningVar = namedVar.FieldName.empty()
                && (llvm::isa<llvm::AllocaInst>(destination) || llvm::isa<llvm::GlobalVariable>(destination));
            // An `alias` CALL RESULT destination (`l[0] = ...`, `h.get() = ...`) is by construction
            // a LIVE element the container handed out - not the EMPTY slot `_data[i] = v` writes
            // into inside container code, which is why that single-index GEP stays excluded. A
            // named-local source already dropped the old owner in the deref arm above and returned;
            // an OWNED TEMP (`"x".copy()`, IsMove) skips that arm, so without this the old owner
            // was orphaned. Rooted at the call itself, never at a raw GEP.
            bool destIsAliasResultLvalue = namedVar.TypeAndValue.IsAlias
                && !namedVar.TypeAndValue.Pointer
                && !namedVar.TypeAndValue.IsInterface
                && !namedVar.TypeAndValue.IsArrayView
                && !namedVar.BitfieldStorage && !namedVar.UnionFieldType
                && llvm::isa_and_nonnull<llvm::CallBase>(destination);
            bool destIsDropOldElem = destIsFixedArrayElem
                && namedVar.TypeAndValue.TypeName != "string"
                && !namedVar.TypeAndValue.Pointer
                && !namedVar.TypeAndValue.IsInterface
                && !namedVar.TypeAndValue.IsArrayView;
            // sameFieldStore, not selfFieldAssign: this EMITS a destruct, so it may only treat two
            // elements as distinct when both root in known-initialized stack/global storage.
            // A pointer-subscript source is a borrow: copy it before this destination drop can
            // free the buffer being read.
            if (operatorText == "=" && right
                && NamedVarIsString(namedVar)
                && namedVar.FieldName.empty()
                && !namedVar.TypeAndValue.Pointer
                && (llvm::isa<llvm::AllocaInst>(destination)
                    || llvm::isa<llvm::GlobalVariable>(destination))
                && IsDeclaredPointerSubscriptString(assignedExpr, right, rightNV.Storage)
                && !IsOwningArrayStringElementRead(rightNV, right)
                && !IsRawHeapStringElementRead(rightNV, right))
            {
                right = compiler->EmitOwnedStringDeepCopy(right);
                if (!namedVar.CallerName.empty())
                    compiler->MarkVariableOwningString(namedVar.CallerName);
            }
            if (operatorText == "=" && right && right->getType()->isStructTy()
                && (destIsStructField || destIsLocalOwningVar || destIsByRefClosureValue || destIsDropOldElem
                    || destIsAliasResultLvalue)
                && destination != rightNV.Storage
                && !sameFieldStore
                && compiler->IsOwningValueType(namedVar.TypeAndValue.TypeName))
            {
                // NOTE: closes the reassignment LEAK but does NOT fix aliasing of an owning-value RHS -
                // ownership is a runtime property (_len owned bit), so auto copy/move is unsafe for string.
                if (!destIsAliasBorrowLocal && !coreUniqueInterfaceResetValue)
                    emitOwningDestinationDrop();
                RetireAliasBorrowOnRebind(compiler, namedVar, assignmentBlock);
            }

            // `unique T* field` reassignment: free the old pointee before overwriting it, or it
            // leaks. Shares EmitUniqueFieldDelete with the synthesized destructor so a field is
            // freed identically at teardown and at reassignment (a divergence between the two
            // would be a silent heap bug). Assigning `nullptr` frees the old pointee too - that is
            // the sanctioned "release early" idiom, and it is why `delete` on a unique field is
            // rejected as unnecessary. Passing `right` lets the helper skip the free on a
            // self-assign. The slot is trusted to be initialized on the same grounds the
            // synthesized destructor already trusts it: every construction path (stack local,
            // `new T()`, `new T[n]`, user-ctor entry) runs a default constructor that
            // zero-initializes the fields.
            // A builtin unique field owns and is released at teardown, so reassignment releases it too.
            bool consumedUniqueFieldSource = false;
            if (operatorText == "=" && right && right->getType()->isPointerTy()
                && (destIsStructField || destIsUniqueFieldSlot)
                && (namedVar.TypeAndValue.IsUnique
                    || (compiler->IsCoreUniqueType(namedVar.TypeAndValue.TypeName)
                        && !namedVar.TypeAndValue.Pointer)))
            {
                if (destIsStructField || destIsUniqueFieldSlot)
                {
                    const std::string fieldName = namedVar.FieldName.empty()
                        ? namedVar.TypeAndValue.VariableName : namedVar.FieldName;
                    RejectRawHeapArrayIntoUniqueField(
                        rightNV, namedVar.TypeAndValue, fieldName, ctx);
                }
                compiler->EmitUniqueFieldDelete(
                    *compiler->builder, destination,
                    compiler->GetFullDestructorForDelete(namedVar.TypeAndValue.TypeName),
                    right);

                // Transfer, not alias: storing a named OWNING `unique T*` local into a unique
                // pointer field must CONSUME the source (null its slot + mark moved) so the field
                // is the sole owner - mirrors the unique-LOCAL reassign and the by-value move sink.
                // Otherwise TransferPointerOwnershipOnStore's refcount-alias path (new-allocated
                // local escaping to a field) leaves both handles live over one pointee - and the
                // field free never consults that refcount, so a UAF is one perturbation away. A
                // `new`/call temp (null Storage) is already a sole owner and left as-is; a store
                // back into the same slot is skipped so it does not dangle; the null-out below is
                // what makes the source's scope-exit free a no-op.
                llvm::Value* srcStorage = rightNV.Storage;
                llvm::Type*  srcBaseTy  = rightNV.BaseType;
                std::string srcName = rightNV.CallerName;
                const auto* liveSource = rightNV.CallerName.empty()
                    ? nullptr : compiler->FindLiveNamedVariable(rightNV.CallerName);
                const auto* directOwner = srcStorage != nullptr
                    ? compiler->FindVariableByStorage(srcStorage) : nullptr;
                if (!rightNV.IsOwning && liveSource != nullptr
                    && liveSource->IsOwning && !liveSource->PointerRebound)
                {
                    srcStorage = liveSource->Storage;
                    srcBaseTy = liveSource->BaseType;
                    srcName = rightNV.CallerName;
                }
                if (!rightNV.IsOwning && liveSource != nullptr
                    && liveSource->BorrowsOwningLocal
                    && liveSource->OwningLocalStorage != nullptr
                    && !compiler->BorrowProofRetiredByRebind(*liveSource))
                {
                    const auto* owner = compiler->FindVariableByStorage(
                        liveSource->OwningLocalStorage);
                    if (owner != nullptr && owner->IsOwning && !owner->PointerRebound)
                    {
                        srcStorage = liveSource->OwningLocalStorage;
                        srcBaseTy = owner->BaseType;
                        srcName = compiler->FindVariableNameByStorage(srcStorage);
                    }
                }
                if (!rightNV.IsOwning && directOwner != nullptr
                    && directOwner->IsOwning && !directOwner->PointerRebound)
                {
                    srcName = compiler->FindVariableNameByStorage(srcStorage);
                }
                if (!rightNV.IsOwning && rightNV.BorrowsOwningLocal
                    && rightNV.OwningLocalStorage != nullptr
                    && !compiler->BorrowProofRetiredByRebind(rightNV))
                {
                    const auto* owner = compiler->FindVariableByStorage(rightNV.OwningLocalStorage);
                    if (owner != nullptr && owner->IsOwning && !owner->PointerRebound)
                    {
                        srcStorage = rightNV.OwningLocalStorage;
                        srcBaseTy = owner->BaseType;
                        srcName = compiler->FindVariableNameByStorage(srcStorage);
                    }
                }
                if (srcStorage == nullptr && !srcName.empty())
                {
                    auto ref = compiler->FindVariableStorage(srcName);
                    srcStorage = ref.Storage;
                    srcBaseTy  = ref.BaseType;
                }
                const bool sourceOwnsValue = rightNV.IsOwning
                    || (liveSource != nullptr
                        && (liveSource->IsOwning || liveSource->BorrowsOwningLocal))
                    || (directOwner != nullptr && directOwner->IsOwning
                        && !directOwner->PointerRebound)
                    || srcStorage != rightNV.Storage;
                if (sourceOwnsValue
                    && rightNV.TypeAndValue.Pointer
                    && !rightNV.TypeAndValue.IsArrayView
                    && !rightNV.TypeAndValue.IsMove && !srcName.empty()
                    && rightNV.FieldName.empty()
                    && srcStorage != nullptr && srcStorage != destination)
                {
                    if (auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(srcBaseTy))
                    {
                        if (llvm::isa<llvm::AllocaInst>(srcStorage)
                            || llvm::isa<llvm::GlobalVariable>(srcStorage))
                            compiler->builder->CreateStore(
                                llvm::ConstantPointerNull::get(ptrTy), srcStorage);
                        else
                            compiler->SetVariableOwning(srcName, false);
                        compiler->MarkVariableMoved(srcName);
                        compiler->ConsumeOwnedNewTemp(rightNV.Primary);
                        consumedUniqueFieldSource = true;
                    }
                }
                if (!consumedUniqueFieldSource && !rightNV.IsBorrowed
                    && (compiler->IsOwningPtrTempValue(rightNV.Primary)
                        || compiler->IsOwnedNewTemp(rightNV.Primary)))
                {
                    compiler->ConsumeOwnedNewTemp(rightNV.Primary);
                    consumedUniqueFieldSource = true;
                }
            }

            /*
             * Same reject for an ELEMENT of a `unique T* f[N]` fixed array, field or local. The
             * synthesized teardown releases every slot (EmitOwningUniqueArrayCleanup for a local,
             * EmitUniqueArrayFieldRelease for a field), so a stack address in any slot is freed
             * exactly like the scalar case. The subscript zeroes ConstArraySize and produces a
             * two-index GEP over the ARRAY type, so neither the struct-field nor the local leg
             * sees it: destIsStructField requires a struct source element type, and the local leg
             * requires an alloca/global destination.
             */
            auto* destArrGep = llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(destination);
            if (operatorText == "=" && right && right->getType()->isPointerTy()
                && namedVar.IsElementAccess
                && destArrGep && destArrGep->getNumIndices() == 2
                && destArrGep->getSourceElementType()->isArrayTy()
                && (namedVar.TypeAndValue.IsUnique
                    || (!namedVar.TypeAndValue.Pointer
                        && compiler->IsCoreUniqueType(namedVar.TypeAndValue.TypeName)))
                && (namedVar.TypeAndValue.Pointer
                    || !namedVar.TypeAndValue.IsInterface)
                && !namedVar.TypeAndValue.IsInterface
                && !namedVar.TypeAndValue.IsInterfacePointer
                && !namedVar.TypeAndValue.IsFunctionPointer
                && !namedVar.TypeAndValue.IsArrayView
                && LLVMBackend::IsProvableNonHeapAddress(right))
            {
                std::string slot = namedVar.FieldName.empty() && !namedVar.CallerName.empty()
                    ? namedVar.CallerName : DescribeUniqueFieldOwner(namedVar);
                RejectNonHeapAddressIntoUnique(
                    std::format("an element of unique array '{}'", slot), ctx);
                return finishStore(right);
            }

            // A unique pointer array element owns its pointee like a scalar unique location.
            // Require the same source proof before adopting a local or global array slot.
            if (operatorText == "=" && right && right->getType()->isPointerTy()
                && namedVar.IsElementAccess
                && destArrGep && destArrGep->getNumIndices() == 2
                && destArrGep->getSourceElementType()->isArrayTy()
                && namedVar.TypeAndValue.IsUnique && namedVar.TypeAndValue.Pointer
                && namedVar.FieldName.empty() && !destIsStructField
                && !namedVar.TypeAndValue.IsInterface
                && !namedVar.TypeAndValue.IsInterfacePointer
                && !namedVar.TypeAndValue.IsFunctionPointer
                && !namedVar.TypeAndValue.IsArrayView
                && destination != rightNV.Storage)
            {
                const std::string borrowedParamCall = BorrowedParameterCallName(
                    rightNV, rightNV.Primary);
                std::string movedBorrowOrigin;
                const bool movedBorrowed = compiler->IsMovedBorrowedPtrValue(
                    rightNV.Primary, &movedBorrowOrigin);
                const bool sourceIsUniqueField = IsUniqueFieldRead(rightNV);
                if (RejectConsumeOfBorrowedByValueParamField(compiler, rightNV, ctx))
                    return finishStore(right);
                const bool sourceIsBorrowed = rightNV.IsBorrowed || rightNV.IsAliasBorrow
                    || rightNV.IsAliasReturnBorrow || rightNV.BorrowsOwningLocal
                    || rightNV.TypeAndValue.IsAlias || !rightNV.BorrowedUniqueField.empty()
                    || rightNV.BorrowedUniqueFieldViaCall || movedBorrowed;
                // A join owns only when every arm owns; a call ledger entry on the joined value is no proof.
                const auto sourceArms = compiler->JoinArmValues(right);
                const bool sourceIsJoin = !sourceArms.empty();
                const bool everyArmOwns = std::ranges::all_of(sourceArms, [&](llvm::Value* arm) {
                    return compiler->TernaryArmJoinsOwning(arm, true) || compiler->IsOwnedNewTemp(arm)
                        || compiler->FindOwnedReturnTemp(arm) != nullptr; });
                const bool sourceOwnsValue = sourceIsUniqueField
                    || (!sourceIsBorrowed && sourceIsJoin && everyArmOwns)
                    || (!sourceIsBorrowed && !sourceIsJoin
                        && (rightNV.IsOwning || rightNV.IsNewAllocated
                            || rightNV.TypeAndValue.IsMove || rightNV.IsExplicitMove
                            || compiler->lastOwningResult || compiler->lastCallReturnsOwned
                            || srcIsOwnedPtrRhs
                            || compiler->CanAdoptUniquePointerJoin(rightNV.Primary)
                            || compiler->IsOwnedNewTemp(right)
                            || compiler->FindOwnedReturnTemp(right) != nullptr
                            || compiler->IsOwningPtrTempValue(right)
                            || compiler->IsMovedOutPtrValue(right)
                            || compiler->TernaryArmJoinsOwning(right)));
                const bool nullSource = llvm::isa<llvm::ConstantPointerNull>(right)
                    || IsAllNullPhi(right);
                if (!nullSource && (!sourceOwnsValue || !borrowedParamCall.empty()))
                {
                    const std::string uniqueType = std::format("unique<{}>",
                        SpellType(*compiler, LLVMBackend::TypeAndValue{
                            .TypeName = namedVar.TypeAndValue.TypeName }));
                    const std::string destName = namedVar.CallerName.empty()
                        ? unaryCtx == nullptr ? std::string("array element") : unaryCtx->getText()
                        : namedVar.CallerName;
                    const std::string sourceName = borrowedParamCall.empty()
                        ? LLVMBackend::BorrowedSourceName(rightNV) : borrowedParamCall;
                    if (sourceName.empty())
                        LogErrorContext(ctx, std::format(
                            "cannot assign {} '{}' from a borrowed value - the source still owns it; "
                            "use 'new', a 'move' expression, or a move-returning call",
                            uniqueType, destName));
                    else
                        LogErrorContext(ctx, std::format(
                            "cannot assign {} '{}' from borrowed value '{}' - the source still owns it; "
                            "use 'new', a 'move' expression, or a move-returning call",
                            uniqueType, destName, sourceName));
                    return finishStore(right);
                }
                // A named raw owning local/global hands its pointee to the element, as the scalar twin does.
                if (!nullSource && rightNV.IsOwning && !rightNV.TypeAndValue.IsUnique
                    && !rightNV.IsExplicitMove && rightNV.Storage != nullptr
                    && !rightNV.CallerName.empty() && rightNV.FieldName.empty()
                    && (llvm::isa<llvm::AllocaInst>(rightNV.Storage)
                        || llvm::isa<llvm::GlobalVariable>(rightNV.Storage)))
                {
                    if (auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(rightNV.BaseType))
                    {
                        compiler->builder->CreateStore(
                            llvm::ConstantPointerNull::get(ptrTy), rightNV.Storage);
                        compiler->MarkVariableMoved(rightNV.CallerName);
                    }
                }

                // A fixed unique-pointer array slot is live storage; release its old pointee
                // before replacement, matching scalar unique slots while preserving self-assign.
                compiler->EmitUniqueFieldDelete(
                    *compiler->builder, destination,
                    compiler->GetFullDestructorForDelete(namedVar.TypeAndValue.TypeName), right);
            }

            // Storing a PROVABLE stack address into a `unique T*` LOCAL. The local's scope-exit
            // teardown frees the pointee, so this would free a stack address. `&local` carries no
            // borrow provenance, so the reassignment legs below never see it.
            if (operatorText == "=" && right && right->getType()->isPointerTy()
                && !destIsStructField
                && (llvm::isa<llvm::AllocaInst>(destination) || llvm::isa<llvm::GlobalVariable>(destination))
                && (namedVar.TypeAndValue.IsUnique
                    || (!namedVar.TypeAndValue.Pointer
                        && compiler->IsCoreUniqueType(namedVar.TypeAndValue.TypeName)))
                && (namedVar.TypeAndValue.Pointer
                    || !namedVar.TypeAndValue.IsInterface)
                && !namedVar.TypeAndValue.IsInterface
                && !namedVar.TypeAndValue.IsInterfacePointer
                && !namedVar.TypeAndValue.IsFunctionPointer
                && !namedVar.TypeAndValue.IsArrayView
                && LLVMBackend::IsProvableNonHeapAddress(right))
            {
                RejectNonHeapAddressIntoUnique(
                    std::format("unique local '{}'", namedVar.CallerName), ctx);
                return finishStore(right);
            }

            // `unique T* LOCAL` reassignment (`b = a`): free the old pointee before overwriting,
            // or the destination's prior object leaks - the field case just above closes this for
            // struct fields, but a thin unique LOCAL fell through to the plain store. Scope exit
            // frees a unique local via EmitOwningPtrCleanup ONLY when IsOwning is set, so we also
            // adopt ownership below. Reuse EmitUniqueFieldDelete for the drop-old so the store-site
            // free matches teardown (a null old pointee is a safe no-op). The outer
            // destination != rightNV.Storage guard - and passing `right` to skip on an equal
            // pointee - make self-assign `a = a` free nothing and keep its live pointer. The owning
            // cleanup also uses the binding's active raw-array count for array reassignment.
            if (operatorText == "=" && right && right->getType()->isPointerTy()
                && !destIsStructField && destIsLocalOwningVar
                && namedVar.TypeAndValue.IsUnique
                && namedVar.TypeAndValue.Pointer
                && !namedVar.TypeAndValue.IsInterface
                && !namedVar.TypeAndValue.IsInterfacePointer
                && !namedVar.TypeAndValue.IsFunctionPointer
                && !namedVar.TypeAndValue.IsArrayView
                && destination != rightNV.Storage)
            {
                const bool uniquePointerJoinOwns = compiler->CanAdoptUniquePointerJoin(rightNV.Primary);

                // `move` of a BORROWED pointer parameter transfers nothing - the caller still owns
                // the pointee and frees it at its own scope exit. Adopting it into this owning slot
                // would free the caller's live object here and again there. ParseMoveExpression
                // carries IsBorrowed precisely so a store into an owning location can be rejected;
                // this is the LOCAL counterpart of RejectBorrowIntoUniqueField.
                if (rightNV.IsBorrowed)
                {
                    std::string origin = rightNV.BorrowedOrigin.empty()
                        ? rightNV.CallerName : rightNV.BorrowedOrigin;
                    bool srcIsField = !rightNV.FieldName.empty();
                    std::string srcDesc = srcIsField
                        ? std::format("{}.{}", rightNV.CallerName, rightNV.FieldName) : origin;
                    // A GLOBAL destination carries no CallerName and lives outside the stack
                    // frames, so recover its SOURCE spelling by storage identity, never the
                    // llvm value name (which carries uniquifying suffixes).
                    std::string destName = namedVar.CallerName.empty()
                        ? compiler->FindVariableNameByStorage(destination) : namedVar.CallerName;
                    if (destName.empty())
                        for (const auto& [gName, gVar] : compiler->globalNamedVariable)
                            if (gVar == destination) { destName = gName; break; }
                    RejectBorrowIntoUniqueLocal(srcDesc, BorrowedOriginRoot(origin), srcIsField,
                                                destName, false, ctx,
                                                rightNV.BorrowedUniqueFieldViaCall
                                                    ? rightNV.BorrowedUniqueField : std::string());
                    return finishStore(right);
                }

                // Same rejection when a '?:' join laundered the borrowed move: the joined VALUE
                // carries no IsBorrowed, yet every arm owned nothing. The DECLARATION form errors.
                std::string borrowMoveOrigin;
                if (compiler->IsMovedBorrowedPtrValue(rightNV.Primary, &borrowMoveOrigin))
                {
                    RejectBorrowIntoUniqueLocal(borrowMoveOrigin, BorrowedOriginRoot(borrowMoveOrigin),
                                                false, namedVar.CallerName, false, ctx);
                    return finishStore(right);
                }

                // A direct CFlat raw-pointer return is borrowed unless its declaration carries
                // owned/alias provenance. Assignment must enforce the same rule as declaration.
                RejectRawHeapArrayIntoUniqueLocal(
                    rightNV, namedVar.TypeAndValue, namedVar.CallerName, ctx);
                compiler->RecordUniqueOwnerLoopSink(rightNV,
                    SpellType(*compiler, namedVar.TypeAndValue), namedVar.CallerName, true);
                compiler->EmitOwningPtrCleanup(namedVar, right);
                // When the RHS transfers ownership (owning local, move param, or `new` temp -
                // exactly what TransferPointerOwnershipOnStore nulls), the destination adopts it.
                // A NON-owning destination (`unique R* b = nullptr; b = a;`) would otherwise be
                // skipped by the Pointer && IsOwning scope-exit gate and leak the transferred
                // pointee. Mirrors init and the unique-interface-local reassignment above.
                // srcIsOwnedPtrRhs covers the DETACHED owned sources whose NamedVariable carries
                // no IsOwning at all - `b = move a`, `b = new R()`, `b = moveReturningCall()`: an
                // explicit `move` returns a Storage-less value, so only the RHS shape identifies it.
                if ((rightNV.IsOwning || srcIsOwnedPtrRhs || uniquePointerJoinOwns)
                    && !namedVar.CallerName.empty())
                {
                    compiler->SetVariableOwning(namedVar.CallerName, true);
                    compiler->ConsumeOwnedNewTemp(rightNV.Primary);
                    if (uniquePointerJoinOwns)
                        compiler->ConsumeOwningLocalJoinArms(rightNV.Primary, true, destination);
                }
            }

            /*
             * Reading a fixed-array `string` element into an EXISTING whole string local/global
             * (`q = dst[0];`) - the assignment twin of the decl-init element arm. The element owns
             * its buffer (the store-side arm made sure of that), so a bit copy left both the local
             * and the element owning it and both freed it (rc 133). Deep-copy BEFORE the drop-old
             * below, so the destination's destructor cannot free a buffer this copy is reading.
             * Destination gated by representation (a `%string` alloca/global, whole variable).
             */
            if (operatorText == "=" && right
                && NamedVarIsString(namedVar)
                && namedVar.FieldName.empty()
                && !namedVar.TypeAndValue.Pointer
                && (llvm::isa<llvm::AllocaInst>(destination)
                    || llvm::isa<llvm::GlobalVariable>(destination))
                && IsOwningArrayStringElementRead(rightNV, right))
            {
                right = compiler->EmitOwnedStringDeepCopy(right);
                if (!namedVar.CallerName.empty())
                    compiler->MarkVariableOwningString(namedVar.CallerName);
            }

            // The raw `new string[n]` twin of the arm above: that allocation does not own its
            // elements, so the read BORROWS - clear the owned bit instead of deep-copying. Same
            // whole-local destination gate, so a container's element-to-element relocation
            // (`_keys[slot] = oldKeys[i]` in `dictionary._rehash`) never reaches it.
            if (operatorText == "=" && right
                && NamedVarIsString(namedVar)
                && namedVar.FieldName.empty()
                && !namedVar.TypeAndValue.Pointer
                && (llvm::isa<llvm::AllocaInst>(destination)
                    || llvm::isa<llvm::GlobalVariable>(destination))
                && IsRawHeapStringElementRead(rightNV, right))
                right = compiler->ClearStringOwnedBit(right);

            // A direct non-pointer alias return is now represented by its source slot. Binding it
            // to an ordinary string local still means copy, not a second owner of that slot.
            if (operatorText == "=" && right
                && NamedVarIsString(namedVar)
                && namedVar.FieldName.empty()
                && !namedVar.TypeAndValue.Pointer
                && !namedVar.TypeAndValue.IsAlias
                && (llvm::isa<llvm::AllocaInst>(destination)
                    || llvm::isa<llvm::GlobalVariable>(destination))
                && (rightNV.TypeAndValue.IsAlias || rightNV.IsAliasBorrow)
                && llvm::isa_and_nonnull<llvm::CallBase>(rightNV.Storage)
                && !rightNV.TypeAndValue.IsMove)
            {
                right = compiler->EmitOwnedStringDeepCopy(right);
            }

            const bool rhsBorrowsOwnedString = NamedVarIsString(rightNV)
                && !rightNV.TypeAndValue.IsMove
                && ((!rightNV.FieldName.empty() && !rightNV.OwningStructName.empty())
                    || rightNV.RootIsBorrowedThisField || rightNV.BorrowsOwnedString);
            if (operatorText == "=" && right && right->getType()->isStructTy()
                && NamedVarIsString(namedVar) && namedVar.FieldName.empty()
                && !namedVar.TypeAndValue.Pointer
                && (llvm::isa<llvm::AllocaInst>(destination)
                    || llvm::isa<llvm::GlobalVariable>(destination))
                && rhsBorrowsOwnedString
                && !rightNV.TypeAndValue.IsAlias && !rightNV.IsAliasBorrow)
            {
                // Keep assignment's established borrow semantics, but clear the copied owned bit
                // so a later rebind's string destructor cannot free the source field's buffer.
                right = compiler->ClearStringOwnedBit(right);
            }

            // Drop the old owned string in a local or mutable closure slot before rebinding it.
            // The string dtor checks the owned bit, so a borrowed or empty value is a safe no-op.
            // Self-assign is guarded by destination != rightNV.Storage.
            if (operatorText == "=" && right && right->getType()->isStructTy()
                && NamedVarIsString(namedVar)
                && namedVar.FieldName.empty()
                && (llvm::isa<llvm::AllocaInst>(destination)
                    || llvm::isa<llvm::GlobalVariable>(destination)
                    || namedVar.IsClosureValueCapture)
                && destination != rightNV.Storage)
            {
                if (auto* dtor = compiler->GetOrCreateFullDestructor("string"))
                    compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { destination });
            }

            // Part 6: single-index GEP (container element slot) store of an OWNING element type.
            // Lift the deliberate exclusion so `_data[i] = value` in generic container code defers to
            // T: COPY a copyable owner (source stays live), MOVE a non-copyable owner / unique
            // pointer (consume + zero + mark the source), or CLONE a closure. Gated to owning element
            // types, so raw non-owning arrays (primitives, borrows, bare pointers, interface values)
            // keep their plain store below. This does NOT drop the old slot value: a container
            // releases a live slot itself (list.set -> `_releaseAt` then `_placeAt`) and only ever
            // stores into an EMPTY slot here, so a drop-old would double-destruct the (default-
            // constructed) slot - `list<Val>` with a side-effecting dtor proved that. The core
            // containers uphold the EMPTY half by draining a fresh `new T[n]` buffer (_drainFresh),
            // whose slots are default-CONSTRUCTED and can hold live values. Returns early:
            // it does the source-consume itself, so the generic Transfer*OnStore helpers must not run.
            // Fire ONLY for a NAMED owning source (a by-value param / local, alloca- or
            // global-backed) - `_data[i] = value` / `_data[i] = move value` in collapsed container
            // code. An rvalue temp (`value.copy()`, `new T()`) and a GEP-source move
            // (`newData[i] = move _data[j]` in a grow loop) have no such storage and fall through to
            // the existing plain store, so un-collapsed containers are untouched by the lift.
            bool srcIsNamedLocal = rightNV.Storage != nullptr
                && (llvm::isa<llvm::AllocaInst>(rightNV.Storage)
                    || llvm::isa<llvm::GlobalVariable>(rightNV.Storage))
                && !rightNV.CallerName.empty();
            auto* destSlotGep = llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(destination);
            bool destIsElemSlot = operatorText == "=" && right && srcIsNamedLocal
                && destSlotGep && destSlotGep->getNumIndices() == 1
                && !destIsStructField && !namedVar.IsInterfaceField
                && !namedVar.BitfieldStorage && !namedVar.UnionFieldType
                && !namedVar.TypeAndValue.IsArrayView;
            const std::string& slotElemType = namedVar.TypeAndValue.TypeName;

            // Re-assignment re-derives raw-array state for an automatic or static pointer local.
            // A fixed-array RHS supplies its compile-time span to a view; unknown sources clear it.
            if (operatorText == "=" && namedVar.TypeAndValue.Pointer
                && !namedVar.CallerName.empty() && namedVar.FieldName.empty()
                && (llvm::isa_and_nonnull<llvm::AllocaInst>(namedVar.Storage)
                    || llvm::isa_and_nonnull<llvm::GlobalVariable>(namedVar.Storage)))
            {
                // The declaration path verifies the initializer against the declared clause; this
                // is the same check for the reassignment door, before the re-derive below.
                RejectLocalAllocAlignMismatch(namedVar, rightNV, right, ctx);
                // Reassigning an owning pointer/view local releases the block it held, before the
                // count below is re-derived. '??=' only stores into null, so it has nothing to free.
                if (coalesceResume == nullptr && right != nullptr
                    && llvm::isa<llvm::AllocaInst>(namedVar.Storage))
                {
                    // A move parameter's slot is an argument, not a local: its owner flag exists only
                    // once a borrow reached it (ActivateParamOwnFlag), so this finds it in the argument frames.
                    auto* viewLocal = compiler->FindStackVariableByStorage(namedVar.Storage);
                    if (viewLocal == nullptr)
                        viewLocal = compiler->FindStackArgumentByStorage(namedVar.Storage);
                    if (viewLocal != nullptr)
                        compiler->EmitOwnedPtrRelease(
                            *viewLocal, right,
                            rightNV.BorrowsOwningLocal ? rightNV.OwningLocalStorage : nullptr,
                            rightNV.IsBorrowed && !rightNV.IsOwning && !rightNV.BorrowsOwningLocal
                                && !rightNV.TypeAndValue.IsMove);
                }
                auto* asgNewArr = assignCtx ? AsDirectNew(assignCtx) : nullptr;
                const bool rhsIsWholeRawArrayBinding = rightNV.FieldName.empty()
                    && rightNV.OwningStructName.empty() && !rightNV.IsElementAccess
                    && (rightNV.Storage == nullptr
                        || llvm::isa<llvm::AllocaInst>(rightNV.Storage)
                        || llvm::isa<llvm::GlobalVariable>(rightNV.Storage));
                bool rhsIsRawArray = rhsIsWholeRawArrayBinding
                    && ((asgNewArr != nullptr && asgNewArr->assignmentExpression() != nullptr)
                        || rightNV.AllocatedByRawNewArray);
                llvm::Value* rhsCount = rhsIsWholeRawArrayBinding
                    ? compiler->LoadRawArrayLength(rightNV) : nullptr;
                if (namedVar.TypeAndValue.IsArrayView
                    && rightNV.TypeAndValue.ConstArraySize > 0
                    && !rightNV.TypeAndValue.IsArrayView)
                    rhsCount = compiler->builder->getInt64(
                        rightNV.TypeAndValue.ConstArraySize);
                compiler->SetVariableRawNewArray(
                    namedVar.CallerName,
                    !namedVar.TypeAndValue.IsArrayView && rhsIsRawArray,
                    rhsCount);
                // Re-derive the block's allocation alignment the same way. The declaration path
                // consumes lastAllocAlignment; a local assigned only after its declaration has no
                // such door, so its free site would use the wrong deallocator.
                compiler->SetVariableAllocAlignment(
                    namedVar.CallerName, rightNV.AllocAlignment);
                if (namedVar.TypeAndValue.IsArrayView
                    && rightNV.TypeAndValue.ConstArraySize > 0
                    && !rightNV.TypeAndValue.IsArrayView)
                    compiler->SetVariableOwning(namedVar.CallerName, false);
            }

            /*
             * A raw `new string[n]` element slot: a single-index GEP whose base is a LOAD from an
             * alloca-backed LOCAL `string*`. This is the exact shape whose destructive `delete` is
             * already rejected below ("a raw 'new string[n]' does not take ownership of assigned
             * strings ... its slots may hold borrowed text"), so the slot must NOT become an owner
             * here either - that mismatch is what let the local and the slot both free one buffer.
             * A CONTAINER's `_data[i]` loads its base from a struct field (list/dictionary), never
             * from an alloca, so the container store arms below are untouched. Admission is POSITIVE
             * provenance (`AllocatedByRawNewArray`): a decayed fixed array, a join and a parameter
             * all answer false and keep the plain store, so no unenumerated binding can dangle.
             */
            bool destIsRawHeapStringSlot = false;
            if (operatorText == "=" && right && destSlotGep
                && destSlotGep->getNumIndices() == 1
                && !destIsStructField && !namedVar.IsViewElement
                && !namedVar.TypeAndValue.IsArrayView && !namedVar.TypeAndValue.Pointer
                && NamedVarIsString(namedVar))
            {
                if (auto* baseLoad =
                        llvm::dyn_cast<llvm::LoadInst>(destSlotGep->getPointerOperand()))
                    destIsRawHeapStringSlot =
                        llvm::isa<llvm::AllocaInst>(baseLoad->getPointerOperand())
                        && RawHeapBaseIsNewArrayLocal(destSlotGep->getPointerOperand());
            }

            // Borrow into the raw heap slot, leaving the LVALUE source the single owner. Keyed on
            // alloca/global source storage, NOT CallerName, which a global source never carries.
            bool srcIsLvalueSlot = rightNV.Storage != nullptr
                && (llvm::isa<llvm::AllocaInst>(rightNV.Storage)
                    || llvm::isa<llvm::GlobalVariable>(rightNV.Storage));
            // Source type checked by REPRESENTATION: this arm returns early, so an ill-typed
            // source must fall through to the existing store diagnostics untouched.
            if (destIsRawHeapStringSlot && srcIsLvalueSlot
                && right->getType()
                    == llvm::StructType::getTypeByName(*compiler->context, "string"))
            {
                compiler->builder->CreateStore(compiler->ClearStringOwnedBit(right), destination);
                return finishStore(right);
            }

            if (destIsElemSlot && slotElemType == "__closure_fat_ptr"
                && right->getType()->isStructTy())
            {
                // Closure element: clone a named source (self-safe - the clone reads the source env
                // first), transfer a `move` source. No drop-old (empty slot; see the block header).
                if (!rightNV.TypeAndValue.IsMove)
                    right = CloneClosureFromNamedSource(rightNV, right, ctx);
                compiler->builder->CreateStore(right, destination);
                compiler->UnregisterOwnedClosureTemp(right);
                return finishStore(right);
            }

            if (destIsElemSlot
                && namedVar.TypeAndValue.IsUnique
                && namedVar.TypeAndValue.Pointer
                && right->getType()->isPointerTy())
            {
                // unique T* element: store the new pointer, then null + mark-moved a NAMED owning
                // source so exactly one owner frees. No drop-old (empty slot; see the block header).
                compiler->builder->CreateStore(right, destination);
                if (rightNV.Storage != nullptr && !rightNV.CallerName.empty()
                    && rightNV.FieldName.empty()
                    && (llvm::isa<llvm::AllocaInst>(rightNV.Storage)
                        || llvm::isa<llvm::GlobalVariable>(rightNV.Storage)))
                {
                    if (auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(rightNV.BaseType))
                    {
                        compiler->builder->CreateStore(
                            llvm::ConstantPointerNull::get(ptrTy), rightNV.Storage);
                        compiler->MarkVariableMoved(rightNV.CallerName);
                    }
                }
                return finishStore(right);
            }

            if (destIsElemSlot && right->getType()->isStructTy()
                && rightNV.TypeAndValue.IsFatInterfaceValue()
                && (rightNV.TypeAndValue.IsUnique
                    || (compiler->IsCoreUniqueType(rightNV.TypeAndValue.TypeName)
                        && !rightNV.TypeAndValue.Pointer)))
            {
                // unique <interface> element: store the fat value, then zero + mark-moved a NAMED
                // owning source so exactly one owner frees (mirrors the unique-pointer arm for the
                // fat-ptr slot). Without the zero a plain (non-move) unique-interface sink param -
                // now owning at entry (8a) - is freed again at its scope exit. No drop-old (empty
                // slot; see the block header). A bare (borrow) interface element never reaches here.
                compiler->builder->CreateStore(right, destination);
                if (rightNV.Storage != nullptr && !rightNV.CallerName.empty()
                    && rightNV.FieldName.empty()
                    && (llvm::isa<llvm::AllocaInst>(rightNV.Storage)
                        || llvm::isa<llvm::GlobalVariable>(rightNV.Storage)))
                {
                    compiler->builder->CreateStore(
                        llvm::ConstantAggregateZero::get(right->getType()), rightNV.Storage);
                    compiler->MarkVariableMoved(rightNV.CallerName);
                }
                return finishStore(right);
            }

            if (destIsElemSlot && right->getType()->isStructTy()
                && (compiler->IsOwningValueType(slotElemType) || slotElemType == "string"))
            {
                // Owning value struct / string element: COPY a copyable owner (source stays live),
                // MOVE a non-copyable owner or an explicit `move`. No drop-old (empty slot; see the
                // block header). On a Move, zero the moved-out NAMED source so its dtor is a no-op.
                AssignSourceKind slotKind;
                llvm::Value* toStore = ClassifyOwningAssignSource(
                    right, slotElemType, rightNV.TypeAndValue.IsMove, ctx, slotKind);
                if (slotKind == AssignSourceKind::Move
                    && RejectConsumeOfBorrowedByValueParamField(compiler, rightNV, ctx))
                    return finishStore(right);
                if (slotKind == AssignSourceKind::Move
                    && RejectImplicitConsumeOfOutlivingOwner(
                        compiler, rightNV, rightNV.TypeAndValue.IsMove, ctx))
                    return finishStore(right);
                compiler->builder->CreateStore(toStore, destination);
                if (slotKind == AssignSourceKind::Move && rightNV.Storage != nullptr)
                {
                    compiler->builder->CreateStore(
                        llvm::ConstantAggregateZero::get(toStore->getType()), rightNV.Storage);
                    if (!rightNV.CallerName.empty()
                        && (llvm::isa<llvm::AllocaInst>(rightNV.Storage)
                            || llvm::isa<llvm::GlobalVariable>(rightNV.Storage)))
                        compiler->MarkVariableMoved(rightNV.CallerName);
                }
                return finishStore(toStore);
            }

            auto* assignResult = derefAssign(right, rhsUnsigned);
            if (operatorText == "=" && namedVar.TypeAndValue.Pointer
                && !namedVar.TypeAndValue.IsUnique
                && (destIsStructField || namedVar.IsElementAccess)
                && rightNV.BorrowsOwningLocal
                && compiler->OwningLocalCopyStillAliases(rightNV))
                if (auto* store = llvm::dyn_cast_or_null<llvm::StoreInst>(assignResult))
                    store->setMetadata(store->getContext().getMDKindID(
                        "cflat.interface-return.alias-owned-local"),
                        llvm::MDNode::get(store->getContext(), {}));
            if (consumedUniqueFieldSource)
                compiler->ConsumeOwnedNewTemp(rightNV.Primary);
            // Array-view element store: tag the store with the view's alias scope (matches the load
            // side in TagViewElementAccess) so the vectorizer proves distinct views disjoint.
            if (namedVar.TypeAndValue.NoaliasScopeId >= 0)
                if (auto* st = llvm::dyn_cast_or_null<llvm::StoreInst>(assignResult))
                    compiler->AttachViewNoalias(st, namedVar.TypeAndValue.NoaliasScopeId);
            // Skip when the unique-field store above already consumed the source: it did the
            // transfer explicitly, and re-running here would take the aliasing refcount path.
            bool storeNulledSource = false;
            if (operatorText == "=" && !consumedUniqueFieldSource)
                storeNulledSource = TransferPointerOwnershipOnStore(
                    rightNV, destination, namedVar.TypeAndValue.IsInterface, ctx);
            // Ruling 2026-10-31: an owning `?:` arm stored into a raw local (a null arm does not help).
            if (operatorText == "=" && namedVar.TypeAndValue.Pointer && !namedVar.TypeAndValue.IsUnique
                && !namedVar.TypeAndValue.IsInterface && !namedVar.TypeAndValue.IsArrayView
                && namedVar.FieldName.empty() && !namedVar.CallerName.empty()
                && llvm::isa<llvm::AllocaInst>(destination)
                && rightNV.TernaryOwnedCallArms != 0 && !InGenericInstantiation())
                compiler->LogOwningCallRawBindError(namedVar.CallerName);
            // Ruling 2026-10-31: an owning call result stored into a raw local that does not adopt it leaks.
            if (operatorText == "=" && namedVar.TypeAndValue.Pointer && !namedVar.TypeAndValue.IsUnique
                && !namedVar.TypeAndValue.IsInterface && !namedVar.TypeAndValue.IsArrayView
                && namedVar.FieldName.empty() && !namedVar.CallerName.empty()
                && llvm::isa<llvm::AllocaInst>(destination)
                && !rightNV.IsExplicitMove && !rightNV.TypeAndValue.IsMove
                && !(assignmentAdoptsOwnedSource || (storeNulledSource && rightNV.FieldName.empty()))
                && rightNV.Primary != nullptr && rightNV.Storage == nullptr
                && compiler->ValueIsUniqueCallResult(rightNV.Primary) && !InGenericInstantiation())
                compiler->LogOwningCallRawBindError(namedVar.CallerName);
            // A plain pointer local adopts an owned new/move/call result at assignment, just as
            // declaration-init does. Borrowed named-pointer stores never reach this gate.
            // `p = q` from an owning pointer local nulled q above, so p is now the owner.
            if (operatorText == "="
                && (assignmentAdoptsOwnedSource || (storeNulledSource && rightNV.FieldName.empty()))
                && (!destinationHadBorrowProvenance
                    || (storeRetiredBorrow && existingDestination != nullptr
                        && !existingDestination->BorrowsOwnedElement))
                && namedVar.TypeAndValue.Pointer && !namedVar.TypeAndValue.IsUnique
                && namedVar.FieldName.empty() && !namedVar.CallerName.empty()
                && (llvm::isa<llvm::AllocaInst>(destination)
                    || llvm::isa<llvm::GlobalVariable>(destination)))
            {
                compiler->SetVariableOwning(namedVar.CallerName, true);
                compiler->RearmParamOwner(namedVar.CallerName);
                compiler->ConsumeOwnedNewTemp(rightNV.Primary);
                // Adopting from another local slot answers to that slot's aliases (leak if any).
                if (auto* adopter = compiler->FindStackVariableByStorage(destination))
                {
                    // The in-block borrow this store retired no longer suppresses the owner; a
                    // stale flag would make the return path free the returned object (T207 r2).
                    if (storeRetiredBorrow) adopter->BorrowSuppressedOwner = false;
                    compiler->GateOwnFlagAdoption(*adopter, right);
                }
            }
            // An owned store that another block's borrow still stands in front of (not retired, so
            // the compile-time owner bit stays clear and `owner = raw` keeps refusing). The runtime
            // flag alone records the new block, so scope exit frees it on this path.
            else if (operatorText == "="
                && (assignmentAdoptsOwnedSource || (storeNulledSource && rightNV.FieldName.empty()))
                && destinationHadBorrowProvenance && !storeRetiredBorrow
                && namedVar.TypeAndValue.Pointer && !namedVar.TypeAndValue.IsUnique
                && namedVar.FieldName.empty() && !namedVar.CallerName.empty()
                && (llvm::isa<llvm::AllocaInst>(destination)
                    || llvm::isa<llvm::GlobalVariable>(destination)))
            {
                compiler->ArmSuppressedOwnFlag(namedVar.CallerName);
                compiler->ConsumeOwnedNewTemp(rightNV.Primary);
                if (auto* adopter = compiler->FindStackVariableByStorage(destination))
                    compiler->GateOwnFlagAdoption(*adopter, right);
            }
            if (operatorText == "=" && namedVar.TypeAndValue.IsArrayView
                && namedVar.Storage != nullptr
                && !llvm::isa<llvm::AllocaInst>(destination))
            {
                bool movedOwnedNonLocalView = false;
                bool movedBorrowedNonLocalView = false;
                const llvm::Value* sourceStorage = rightNV.MoveSourceStorage != nullptr
                    ? rightNV.MoveSourceStorage : rightNV.Storage;
                // A pointer/view move result sets IsExplicitMove, not TypeAndValue.IsMove.
                if ((rightNV.TypeAndValue.IsMove || rightNV.IsExplicitMove)
                    && (sourceStorage != nullptr || !rightNV.MoveSourcePathKey.empty()))
                {
                    const auto sourceMayBeBorrowed = compiler->NonLocalViewMayBeBorrowed(
                        sourceStorage, rightNV.MoveSourcePathKey);
                    movedOwnedNonLocalView = sourceMayBeBorrowed.has_value()
                        && !sourceMayBeBorrowed.value();
                    movedBorrowedNonLocalView = sourceMayBeBorrowed.value_or(false);
                }
                const bool viewStoreTransfersOwnership = !assignmentBorrowsOwningLocal
                    && !movedBorrowedNonLocalView
                    && !compiler->IsMovedBorrowedPtrValue(rightNV.Primary)
                    && (srcIsOwnedPtrRhs
                        || assignmentAdoptsOwnedSource
                        || (storeNulledSource && rightNV.FieldName.empty())
                        || (rightNV.IsOwning && !rightNV.OwnFlagMayBeFalse)
                        || movedOwnedNonLocalView);
                compiler->TrackNonLocalViewAssignment(destination, rightNV.Primary,
                    sourceStorage, viewStoreTransfersOwnership,
                    compiler->NonLocalElementPathKey(namedVar), movedBorrowedNonLocalView,
                    &namedVar, &rightNV);
            }
            if (operatorText == "=" && namedVar.TypeAndValue.IsArrayView
                && namedVar.FieldName.empty() && !namedVar.CallerName.empty()
                && llvm::isa<llvm::AllocaInst>(destination))
                if (const auto* raw = compiler->FindRawArrayResult(rightNV.Primary);
                    raw != nullptr && compiler->RawArrayResultMayOwn(rightNV.Primary))
                {
                    llvm::Value* runtimeOwns = raw->RuntimeOwns;
                    bool runtimeCanBeFalse = !raw->Owns;
                    if (runtimeOwns == nullptr)
                        runtimeOwns = compiler->builder->getInt1(raw->Owns);
                    else if (const auto* constant =
                        llvm::dyn_cast<llvm::ConstantInt>(runtimeOwns))
                        runtimeCanBeFalse = constant->isZero();
                    compiler->SetVariableOwnFlagValue(
                        namedVar.CallerName, true, runtimeOwns, runtimeCanBeFalse,
                        raw->RuntimeOwns != nullptr);
                    compiler->ConsumeOwnedNewTemp(rightNV.Primary);
                }
            // A plain view copy of an owning local is borrowed even when it replaces an owned
            // value; retain T377's may-be-borrowed state so a later move sink refuses it.
            if (operatorText == "=" && assignmentBorrowsOwningLocal
                && namedVar.TypeAndValue.IsArrayView && namedVar.FieldName.empty()
                && !namedVar.CallerName.empty() && llvm::isa<llvm::AllocaInst>(destination))
                compiler->SetVariableOwnFlagValue(namedVar.CallerName, false,
                    compiler->builder->getInt1(false), true, false);
            // Transfer ownership for move string: null _ptr so string.dtor is a no-op
            // after the value has been moved to persistent storage (e.g. list::add).
            if (operatorText == "=")
                TransferMoveStringOwnershipOnStore(rightNV, ctx);
            // Reassignment to a moved variable (or field) makes it live again.
            if (operatorText == "=" && !namedVar.CallerName.empty())
            {
                if (const std::string movePath = LLVMBackend::FieldMoveKey(namedVar);
                    !movePath.empty())
                    compiler->MarkVariableFieldUnmoved(namedVar.CallerName, movePath);
                else if (!namedVar.IsElementAccess && namedVar.FieldName.empty()
                    && namedVar.OwningStructName.empty())
                {
                    compiler->MarkVariableUnmoved(namedVar.CallerName);
                    compiler->MarkVariableNotExplicitlyMovedNull(namedVar.CallerName);
                    // --sanitize=ownership (M1): the local is live again - reset its origin slot.
                    compiler->ClearOwnMoveOrigin(destination);
                }
            }
            /*
             * The three refreshes below RETIRE a fact, and '??=' stores CONDITIONALLY - the old
             * referent survives when the arm is not taken - so a '??=' takes the JOIN: the fact is
             * kept unless the new RHS also carries it. Retiring unconditionally would launder a
             * container-owned element borrow (measured: the desugared `if (g == nullptr) { g = ...; }`
             * spelling clears the taint, and the resulting raw `delete g;` double-frees).
             */
            // Reassignment to a bonded variable breaks the bond (per design: bond is to the instance).
            if (operatorText == "=" && coalesceResume == nullptr && !namedVar.CallerName.empty())
                compiler->ClearVariableBond(namedVar.CallerName);
            // Refresh the field-borrow taint on a reassigned whole-variable string local: it now
            // borrows a field iff the new RHS does (so `t = "lit"` after `t = b.name` clears it).
            if (operatorText == "=" && !namedVar.CallerName.empty() && namedVar.FieldName.empty()
                && NamedVarIsString(namedVar))
            {
                bool rhsBorrowsField = NamedVarIsString(rightNV)
                    && !rightNV.TypeAndValue.IsMove
                    && ((!rightNV.FieldName.empty() && !rightNV.OwningStructName.empty())
                        || rightNV.RootIsBorrowedThisField
                        || rightNV.BorrowsOwnedString);
                if (rhsBorrowsField || coalesceResume == nullptr)
                    compiler->SetVariableBorrowsOwnedString(namedVar.CallerName, rhsBorrowsField);
            }
            // Refresh the container-owned-element borrow taint on a reassigned pointer local: it
            // now borrows a container-owned element iff the new RHS does, so `g = new B()` after
            // `g = l.get(0)` clears it and a later `delete g` is allowed (item 2 false positive).
            if (operatorText == "=" && !namedVar.CallerName.empty() && namedVar.FieldName.empty()
                && namedVar.TypeAndValue.Pointer)
            {
                bool coreUniqueElementBorrow = rhsCoreUniqueAliasElement
                    || (rightNV.TypeAndValue.IsAlias
                    && compiler->IsCoreUniqueType(rightNV.TypeAndValue.TypeName)
                    && !rightNV.TypeAndValue.IsMove);
                bool rhsBorrowsElem = (rightNV.TypeAndValue.IsBorrowOfAliasElement
                    || coreUniqueElementBorrow) && !compiler->lastOwningResult;
                bool rhsExternallyOwned = rightNV.TypeAndValue.IsBorrowOfAliasElement
                    && !compiler->lastOwningResult;
                std::string container = rightNV.TypeAndValue.ParentVariableName.empty()
                    ? rightNV.CallerName : rightNV.TypeAndValue.ParentVariableName;
                if (rhsBorrowsElem || coalesceResume == nullptr)
                    compiler->SetVariableBorrowsOwnedElement(namedVar.CallerName, rhsBorrowsElem, container, rhsExternallyOwned);
            }
            return finishStore(assignResult);
        }
        else if (unaryCtx)
        {
            auto namedVar = ParseUnaryExpression(unaryCtx);
            auto destination = LoadNamedVariable(namedVar);
            return destination;
        }

        LogErrorContext(ctx, "Unhandled assignment expression form.");
        return nullptr;
    }

