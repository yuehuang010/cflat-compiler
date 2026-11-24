#include "MainListener.h"
#include <charconv>
#include <optional>
#include <llvm/Support/SaveAndRestore.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicInst.h>
#include "MainListener_ExpressionsShared.h"
#include "CxxSpelling.h"

namespace {
class ShortCircuitMovedStateScope
{
public:
    explicit ShortCircuitMovedStateScope(LLVMBackend* compiler) : compiler_(compiler)
    {
        compiler_->PushMovedStateMergeSnapshot();
    }

    ~ShortCircuitMovedStateScope()
    {
        if (compiler_ != nullptr) compiler_->DiscardMovedStateMergeSnapshot();
    }

    void Merge()
    {
        if (compiler_ == nullptr) return;
        compiler_->MergeMovedStateMergeSnapshot();
        compiler_ = nullptr;
    }

private:
    LLVMBackend* compiler_;
};
}

llvm::Value* MainListener::BoxTernaryThinArmToInterface(llvm::Value* thinValue, const std::string& interfaceName,
                                              std::string& armFailure) {
        auto* compiler = compilerLLVM;
        auto* fatTy = compiler->GetFatPtrType();
        if (auto* c = llvm::dyn_cast<llvm::Constant>(thinValue); c != nullptr && c->isNullValue())
            return llvm::Constant::getNullValue(fatTy);

        // The thin arm may itself be a slot-backed '??' (or a nested '?:'). Box that join at its
        // own resume block; the outer mixed ternary block is not a predecessor of the inner phi.
        if (!CollectPointerJoinArms(thinValue).empty())
        {
            if (auto* boxed = UpcastPointerJoinToInterface(thinValue, interfaceName,
                                                            &armFailure))
                return boxed;
            return nullptr;
        }

        ResolvedJoinArmClass resolved;
        std::string className = ResolveJoinArmClass(thinValue, resolved)
            ? resolved.Name : std::string();
        if (className.empty())
        {
            armFailure = "the arm's concrete class cannot be determined; bind the arm to a "
                         "local variable of the class type first";
            return nullptr;
        }
        if (!compiler->StructImplementsInterface(className, interfaceName))
        {
            armFailure = std::format("'{}' does not implement it", className);
            return nullptr;
        }
        auto* vtable = compiler->GetOrCreateVTable(className, interfaceName);
        auto* boxed = compiler->BuildInterfaceFatValue(vtable, thinValue);
        // Ledger which interface this box was built against, so the sibling fat arm's interface
        // (both arms now agree) survives to the eventual phi and the receiver's rebox check.
        compiler->RegisterFatInterfaceValueTypeName(boxed, interfaceName);

        // Carry the ownership verdict onto the box: a fat arm is scored owning only via the
        // owning-RETURN ledger (a struct is never an owning-POINTER temp).
        if (compiler->TernaryArmJoinsOwning(thinValue))
        {
            if (compiler->IsMovedOutPtrValue(thinValue))
                compiler->RegisterMovedOutPtrValue(boxed);
            else
                compiler->RegisterOwnedReturnTemp(boxed, "?:", LLVMBackend::TypeAndValue{});
        }
        // thinValue's move-of-borrow provenance is dropped here: unreachable today (scored
        // non-owning above already), and the ledger refuses a struct (boxed) regardless.
        return boxed;
    }

bool MainListener::RejectCodeValueTernaryStringArm(CFlatParser::ConditionalExpressionContext* ctx,
                                         llvm::Value* armValue, size_t armOccurrence) {
        auto* compiler = Compiler(ctx);
        // Runs after both arms are parsed, so the ambient id is the enclosing slot's again - the
        // arm answers under the id IT evaluated under (see joinArmOccurrences_).
        if (!compiler->JoinArmCarriesCodeValue(armValue, armOccurrence)) return false;
        LogErrorContext(ctx, "cannot convert a function-pointer or closure VALUE in a '?:' arm to "
            "'string' - code is not a NUL-terminated buffer; write an explicit '(char*)' cast if "
            "the raw code address is what you want");
        return true;
    }

// A parenthesized `?:` arm returns the join just parsed by value: that join's element-borrow fact
// (set on lastTernaryJoinShape by ParseTernaryBranches) carries over to the arm.
static void CarryNestedTernaryBorrow(LLVMBackend* compiler, LLVMBackend::TypedValue& tv)
{
    const auto& nested = compiler->lastTernaryJoinShape;
    if (tv.value == nullptr || tv.value != nested.value) return;
    if (nested.stackCharBufferBorrow)
    {
        tv.stackCharBufferBorrow = true;
        tv.stackCharBufferSource = nested.stackCharBufferSource;
        tv.stackCharBufferScopeDepth = nested.stackCharBufferScopeDepth;
    }
    if (nested.borrowsOwnedElement)
    {
        tv.borrowsOwnedElement = true;
        tv.borrowedElementExternallyOwned = nested.borrowedElementExternallyOwned;
        tv.ownedElementContainer = nested.ownedElementContainer;
    }
}

static void JoinConditionalStackCharBufferBorrow(LLVMBackend::TypedValue& result,
        const LLVMBackend::TypedValue& trueArm,
        const LLVMBackend::TypedValue& falseArm)
{
    if (result.value == nullptr) return;
    auto* stringViewType = llvm::StructType::getTypeByName(
        result.value->getType()->getContext(), "string_view");
    auto* stringType = llvm::StructType::getTypeByName(
        result.value->getType()->getContext(), "string");
    if (result.value->getType() != stringViewType && result.value->getType() != stringType)
        return;
    if (!trueArm.stackCharBufferBorrow && !falseArm.stackCharBufferBorrow) return;

    const LLVMBackend::TypedValue* source = trueArm.stackCharBufferBorrow ? &trueArm : &falseArm;
    if (falseArm.stackCharBufferBorrow
        && (!trueArm.stackCharBufferBorrow
            || falseArm.stackCharBufferScopeDepth > trueArm.stackCharBufferScopeDepth))
        source = &falseArm;
    result.stackCharBufferBorrow = true;
    result.stackCharBufferSource = source->stackCharBufferSource;
    result.stackCharBufferScopeDepth = std::max(
        trueArm.stackCharBufferBorrow ? trueArm.stackCharBufferScopeDepth : 0,
        falseArm.stackCharBufferBorrow ? falseArm.stackCharBufferScopeDepth : 0);
}

// A `move x` ternary arm: the moved value, carrying any stack-buffer borrow of a nested ?:.
LLVM_ATTRIBUTE_NOINLINE LLVMBackend::TypedValue MainListener::ParseTernaryMoveArm(
        CFlatParser::MoveExpressionContext* moveCtx, antlr4::ParserRuleContext* armCtx) {
        auto named = ParseMoveExpression(moveCtx);
        auto value = TypedValueOfNamedOperand(named, armCtx);
        ProcessPlusPlus();
        CarryNestedTernaryBorrow(Compiler(armCtx), value);
        return value;
}

// A ternary arm parsed for its VALUE. Same lowering as ParseExpression (which forwards a
// non-assignment expression straight to ParseConditionalExpression), but it keeps the arm's
// signedness so the join can zero-extend an unsigned arm instead of sign-extending it.
LLVM_ATTRIBUTE_NOINLINE LLVMBackend::TypedValue MainListener::ParseTernaryArmExpression(
        CFlatParser::ExpressionContext* ctx) {
        auto* assignCtx = ctx->assignmentExpression();
        auto* bareIdentifier = BareIdentifierInExpression(ctx);
        if (assignCtx != nullptr && !HasAssignmentOperator(assignCtx))
            if (auto* move = TopLevelMoveExpression(ctx))
                return ParseTernaryMoveArm(move, ctx);
        if (assignCtx != nullptr && !HasAssignmentOperator(assignCtx)
            && assignCtx->conditionalExpression() != nullptr
            && bareIdentifier == nullptr)
        {
            auto tv = ParseConditionalExpression(assignCtx->conditionalExpression());
            ProcessPlusPlus();
            CarryNestedTernaryBorrow(Compiler(ctx), tv);
            return tv;
        }
        if (assignCtx != nullptr && !HasAssignmentOperator(assignCtx)
            && bareIdentifier != nullptr)
        {
            auto named = ParseAssignmentExpressionNamed(assignCtx);
            return TypedValueOfNamedOperand(named, ctx);
        }
        auto named = ParseAssignmentExpressionNamed(assignCtx);
        auto tv = TypedValueOfNamedOperand(named, ctx);
        ProcessPlusPlus();
        auto* value = tv.value;
        EnsureOperatorBoolForValue(value);
        tv.value = value;
        tv.isAlias = tv.value != nullptr && Compiler(ctx)->IsAliasValue(tv.value);
        CarryNestedTernaryBorrow(Compiler(ctx), tv);
        return tv;
}

bool MainListener::IsDefaultOnlyExpression(antlr4::ParserRuleContext* ctx,
        bool allowParentheses) const {
        if (ctx == nullptr) return false;
        bool sawDefault = false;
        std::function<bool(antlr4::tree::ParseTree*)> onlyDefault =
            [&](antlr4::tree::ParseTree* node) {
                if (auto* terminal = dynamic_cast<antlr4::tree::TerminalNode*>(node))
                {
                    const size_t tokenType = terminal->getSymbol()->getType();
                    if (tokenType == CFlatParser::Default && !sawDefault)
                    {
                        sawDefault = true;
                        return true;
                    }
                    return allowParentheses && (tokenType == CFlatParser::LeftParen
                        || tokenType == CFlatParser::RightParen);
                }
                for (auto* child : node->children)
                    if (!onlyDefault(child)) return false;
                return true;
            };
        return onlyDefault(ctx) && sawDefault;
}

static bool IsParenTerminal(antlr4::tree::ParseTree* node, size_t tokenType)
{
        auto* terminal = dynamic_cast<antlr4::tree::TerminalNode*>(node);
        return terminal != nullptr && terminal->getSymbol()->getType() == tokenType;
}

bool MainListener::IsSameTypeCastOfDefault(antlr4::ParserRuleContext* ctx,
        const std::string& typeName)
{
        for (antlr4::tree::ParseTree* node = ctx; node != nullptr; )
        {
            if (auto* cast = dynamic_cast<CFlatParser::CastExpressionContext*>(node);
                cast != nullptr && cast->castTypeName() != nullptr
                    && cast->castExpression() != nullptr && !CastIsCallSpelling(cast))
            {
                const auto castType = ParseCastTypeName(cast->castTypeName(), false, cast);
                return !castType.Pointer && !castType.ElemPointer && !castType.IsArrayView
                    && Compiler(cast)->ResolveTypeAlias(castType.TypeName)
                        == Compiler(cast)->ResolveTypeAlias(typeName)
                    && IsDefaultOnlyExpression(cast->castExpression(), true);
            }
            auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(node);
            if (rule == nullptr) return false;
            if (rule->children.size() == 1)
            {
                node = rule->children.front();
                continue;
            }
            if (rule->children.size() == 3
                && IsParenTerminal(rule->children[0], CFlatParser::LeftParen)
                && IsParenTerminal(rule->children[2], CFlatParser::RightParen))
            {
                node = rule->children[1];
                continue;
            }
            return false;
        }
        return false;
}

// True when `ctx` is an operand arm of a '?:' that is the WHOLE of a call argument, possibly through
// parentheses and typeName casts. Type-blind: the destination type is not consulted.
bool MainListener::IsTernaryArmOfCallArgument(antlr4::ParserRuleContext* ctx)
{
        antlr4::tree::ParseTree* node = ctx != nullptr ? ctx->parent : nullptr;
        antlr4::tree::ParseTree* child = ctx;
        for (; node != nullptr; child = node, node = node->parent)
        {
            auto* conditional = dynamic_cast<CFlatParser::ConditionalExpressionContext*>(node);
            if (conditional != nullptr && conditional->Question() != nullptr
                && child != static_cast<antlr4::tree::ParseTree*>(conditional->logicalOrExpression()))
                break;
        }
        if (node == nullptr) return false;
        // Only pass-through rules, parentheses and casts may separate the '?:' from the argument.
        child = node;
        for (auto* up = node->parent; up != nullptr; child = up, up = up->parent)
        {
            if (dynamic_cast<CFlatParser::ArgumentNamedExpressionContext*>(up) != nullptr) return true;
            // A cast rule without a typeName is a plain pass-through, like any single-child rule.
            if (auto* cast = dynamic_cast<CFlatParser::CastExpressionContext*>(up);
                cast != nullptr && cast->castTypeName() != nullptr)
            {
                if (cast->castExpression() != nullptr
                    && child == static_cast<antlr4::tree::ParseTree*>(cast->castExpression()))
                    continue;
                return false;
            }
            auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(up);
            if (rule == nullptr) return false;
            if (rule->children.size() == 1) continue;
            if (rule->children.size() == 3 && rule->children[1] == child
                && IsParenTerminal(rule->children[0], CFlatParser::LeftParen)
                && IsParenTerminal(rule->children[2], CFlatParser::RightParen))
                continue;
            return false;
        }
        return false;
}

bool MainListener::HasDefaultOnlyTernaryArm(antlr4::ParserRuleContext* ctx) const
{
        auto containsDefaultTernary = [&](auto&& self, antlr4::tree::ParseTree* node) -> bool {
            if (node == nullptr) return false;
            if (auto* ternary = dynamic_cast<CFlatParser::ConditionalExpressionContext*>(node);
                ternary != nullptr && ternary->Question() != nullptr)
                return IsDefaultOnlyExpression(ternary->expression())
                    || IsDefaultOnlyExpression(ternary->conditionalExpression())
                    || self(self, ternary->expression())
                    || self(self, ternary->conditionalExpression());

            auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(node);
            if (rule == nullptr) return false;
            if (auto* cast = dynamic_cast<CFlatParser::CastExpressionContext*>(rule);
                cast != nullptr && cast->castTypeName() != nullptr && cast->castExpression() != nullptr)
                return self(self, cast->castExpression());
            if (rule->children.size() == 1)
                return self(self, rule->children.front());
            if (rule->children.size() == 3
                && IsParenTerminal(rule->children[0], CFlatParser::LeftParen)
                && IsParenTerminal(rule->children[2], CFlatParser::RightParen))
                return self(self, rule->children[1]);
            return false;
        };
        return containsDefaultTernary(containsDefaultTernary, ctx);
}

/*
 * T418: the `?:` of a C++ call argument, through parentheses: `c ? default : x`, `(c ? x : default)`.
 * Null for anything else (an assignment, a cast, a comma list).
 */
static CFlatParser::ConditionalExpressionContext* DeferrableTernaryNode(antlr4::tree::ParseTree* node)
{
        while (node != nullptr)
        {
            if (auto* conditional = dynamic_cast<CFlatParser::ConditionalExpressionContext*>(node);
                conditional != nullptr && conditional->Question() != nullptr)
                return conditional;
            if (auto* assignment = dynamic_cast<CFlatParser::AssignmentExpressionContext*>(node);
                HasAssignmentOperator(assignment))
                return nullptr;
            auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(node);
            if (rule == nullptr) return nullptr;
            if (auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(rule);
                primary != nullptr && primary->expression() != nullptr)
            {
                node = primary->expression();
                continue;
            }
            if (rule->children.size() != 1) return nullptr;
            node = rule->children.front();
        }
        return nullptr;
}

static bool CxxInertLocalReadType(LLVMBackend* compiler,
                                  const LLVMBackend::TypeAndValue& type,
                                  bool condition)
{
        if (type.Pointer || type.PointerDepth != 0) return true;
        if (condition) return type.IsPrimitive();
        if (compiler->IsForeignNontrivialCxxClass(type.TypeName)) return true;
        return !compiler->IsDataStructure(compiler->ResolveTypeAlias(type.TypeName));
}

static bool ResolveCxxDeferredLocalField(
        LLVMBackend* compiler,
        antlr4::tree::ParseTree* node,
        LLVMBackend::NamedVariable& container,
        LLVMBackend::DeclTypeAndValue& field,
        CFlatParser::PostfixExpressionContext** outPostfix = nullptr)
{
        container = {};
        field = {};
        if (outPostfix != nullptr) *outPostfix = nullptr;
        auto* postfix = SolePostfixExpressionShape(node);
        if (postfix == nullptr || postfix->children.size() != 3
            || postfix->argumentExpressionList().size() != 0
            || postfix->expression().size() != 0
            || postfix->memberNameToken().size() != 1
            || postfix->genericTypeParameters().size() != 0)
            return false;
        auto* primary = postfix->primaryExpression();
        auto* generic = primary != nullptr ? primary->genericIdentifier() : nullptr;
        auto* member = dynamic_cast<CFlatParser::MemberNameTokenContext*>(postfix->children[2]);
        auto* memberIdentifier = member != nullptr ? member->Identifier() : nullptr;
        auto* memberOperator = dynamic_cast<antlr4::tree::TerminalNode*>(postfix->children[1]);
        if (generic == nullptr || generic->genericTypeParameters() != nullptr
            || memberIdentifier == nullptr || memberOperator == nullptr
            || memberOperator->getSymbol()->getType() != CFlatParser::Dot)
            return false;
        auto* baseIdentifier = generic->Identifier();
        if (baseIdentifier == nullptr) return false;
        auto local = compiler->GetScopedLocalOrArgument(baseIdentifier->getText());
        if (local.Storage == nullptr || local.TypeAndValue.Pointer
            || local.TypeAndValue.PointerDepth != 0 || local.TypeAndValue.IsAlias
            || !CxxInertLocalReadType(compiler, local.TypeAndValue, false))
            return false;
        auto structure = compiler->GetDataStructure(local.TypeAndValue.TypeName);
        if (structure.StructType == nullptr)
            structure = compiler->GetDataStructure(compiler->ResolveTypeAlias(local.TypeAndValue.TypeName));
        if (structure.StructType == nullptr) return false;
        const std::string& memberName = memberIdentifier->getText();
        for (const auto& candidate : structure.StructFields)
        {
            if (candidate.VariableName != memberName) continue;
            if (candidate.Pointer || candidate.PointerDepth != 0 || candidate.ElemPointer
                || candidate.IsAlias || candidate.IsArrayView || candidate.ConstArraySize != 0
                || !compiler->IsForeignNontrivialCxxClass(candidate.TypeName))
                return false;
            container = std::move(local);
            field = candidate;
            if (outPostfix != nullptr) *outPostfix = postfix;
            return true;
        }
        return false;
}

/*
 * T518: a sole zero-argument call of a CFlat function returning a nontrivial C++ class by value.
 * The wrapper calls it through a function pointer only in its selected arm (guaranteed elision).
 */
bool MainListener::ResolveCxxDeferredCallArm(antlr4::tree::ParseTree* node,
        LLVMBackend::NamedVariable& callable, LLVMBackend::TypeAndValue& returnType)
{
        callable = {};
        returnType = {};
        auto* postfix = SolePostfixExpressionShape(node);
        if (postfix == nullptr || postfix->LeftParen().size() != 1
            || postfix->RightParen().empty()
            || postfix->children.back() != postfix->RightParen().back()
            || postfix->argumentExpressionList().size() != 1
            || !postfix->argumentExpressionList().front()->argumentNamedExpression().empty()
            || CountCallParens(postfix) != 1)
            return false;
        auto* primary = postfix->primaryExpression();
        auto* generic = primary != nullptr ? primary->genericIdentifier() : nullptr;
        if (generic == nullptr || generic->genericTypeParameters() != nullptr
            || generic->Identifier() == nullptr)
            return false;
        auto resolved = ParseIdentifier(generic->Identifier());
        auto* function = resolved.Primary != nullptr
            ? llvm::dyn_cast<llvm::Function>(resolved.Primary->stripPointerCasts()) : nullptr;
        auto* compiler = Compiler();
        const auto* symbol = compiler->FindSymbolForFunction(function);
        if (symbol == nullptr || symbol->IsCxx || symbol->IsMethod || symbol->Variadic
            || !symbol->Parameters.empty() || symbol->ReturnType.Pointer
            || symbol->ReturnType.PointerDepth != 0 || symbol->ReturnType.IsAlias
            || !compiler->IsForeignNontrivialCxxReturnClass(symbol->ReturnType.TypeName))
            return false;
        callable.Primary = function;
        callable.BaseType = function->getType();
        callable.TypeAndValue = compiler->FuncPtrSigOfSymbol(*symbol);
        callable.CallerName = symbol->SourceName;
        callable.IsRvalue = true;
        returnType = symbol->ReturnType;
        return true;
}

bool MainListener::IsCxxDeferrableDefaultTernary(antlr4::ParserRuleContext* ctx, std::string& placeType,
                                                 bool allowCallArm)
{
        placeType.clear();
        auto* root = DeferrableTernaryNode(ctx);
        if (root == nullptr) return false;
        bool sawDefault = false;
        bool sawPlace = false;
        bool mixedPlaces = false;
        auto notePlace = [&](const LLVMBackend::TypeAndValue& type) {
            if (type.Pointer || type.PointerDepth != 0 || type.IsAlias
                || !Compiler()->IsForeignNontrivialCxxClass(type.TypeName))
                return false;
            if (sawPlace && placeType != type.TypeName) mixedPlaces = true;
            placeType = type.TypeName;
            sawPlace = true;
            return true;
        };
        auto visit = [&](auto&& self, antlr4::ParserRuleContext* node) -> bool {
            if (auto* conditional = DeferrableTernaryNode(node))
                return self(self, conditional->expression())
                    && self(self, conditional->conditionalExpression());
            if (IsDefaultOnlyExpression(node))
            {
                sawDefault = true;
                return true;
            }
            if (auto* move = TopLevelMoveExpression(node))
            {
                auto* identifier = BareIdentifierInExpression(move->unaryExpression());
                if (identifier == nullptr) return false;
                const auto* local = Compiler()->FindLiveNamedVariable(identifier->getText());
                return local != nullptr && local->Storage != nullptr
                    && notePlace(local->TypeAndValue);
            }
            auto* identifier = BareIdentifierInExpression(node);
            if (identifier != nullptr)
            {
                const auto* local = Compiler()->FindLiveNamedVariable(identifier->getText());
                return local != nullptr && local->Storage != nullptr && notePlace(local->TypeAndValue);
            }
            LLVMBackend::NamedVariable callable;
            LLVMBackend::TypeAndValue callReturn;
            if (allowCallArm && ResolveCxxDeferredCallArm(node, callable, callReturn))
                return notePlace(callReturn);
            LLVMBackend::NamedVariable container;
            LLVMBackend::DeclTypeAndValue field;
            if (!ResolveCxxDeferredLocalField(Compiler(), node, container, field))
                return false;
            return notePlace(field);
        };
        if (!visit(visit, root->expression()) || !visit(visit, root->conditionalExpression())
            || !sawDefault || !sawPlace || mixedPlaces)
        {
            placeType.clear();
            return false;
        }
        return true;
}

/*
 * T459: the arms build the temporary in place when the join consumes it. An unconsumed join keeps
 * the ordinary parse, which is what every other `?:` argument gets.
 */
LLVMBackend::NamedVariable MainListener::ParseCxxConstRefTernaryTemp(
        CFlatParser::AssignmentExpressionContext* ctx, const LLVMBackend::TypeAndValue& dest)
{
        auto* backend = Compiler();
        auto* objectType = backend->GetType(LLVMBackend::TypeAndValue{ .TypeName = dest.TypeName });
        auto* slot = backend->AllocaAtEntry(objectType, nullptr, "cxx.ctor.arg.default");
        CxxCallArgumentExpectedTypeScope callExpectedType(cxxCallArgumentExpectedType_, dest);
        DeclExpectedTypeScope declExpectedScope(&declExpectedType, dest, true);
        llvm::SaveAndRestore<llvm::Value*> destScope(backend->pendingCxxTernaryDeclDest_, slot);
        llvm::SaveAndRestore<std::string> typeScope(backend->pendingCxxTernaryDeclTypeName_, dest.TypeName);
        backend->pendingCxxTernaryDeclConsumed_ = false;
        backend->pendingCxxTernaryDeclFailed_ = false;
        auto parsed = ParseAssignmentExpressionNamed(ctx);
        const bool consumed = backend->pendingCxxTernaryDeclConsumed_ && !backend->pendingCxxTernaryDeclFailed_;
        backend->pendingCxxTernaryDeclConsumed_ = false;
        backend->pendingCxxTernaryDeclFailed_ = false;
        if (!consumed) return parsed;
        parsed = {};
        parsed.TypeAndValue = LLVMBackend::TypeAndValue{ .TypeName = dest.TypeName };
        parsed.BaseType = objectType;
        parsed.Primary = backend->CreateLoad(objectType, slot);
        parsed.Primary->setName("cxx.ctor.arg.value");
        parsed.Storage = slot;
        parsed.IsRvalue = true;
        parsed.IsCxxPrvalueTemp = true;
        // A call-argument join already owns the slot; a second entry would destroy it twice.
        const bool owned = std::any_of(backend->pendingOwnedStructTemps.begin(),
            backend->pendingOwnedStructTemps.end(), [&](const auto& t) { return t.Alloca == slot; });
        if (!owned) backend->RegisterOwnedStructTemp(slot, dest.TypeName);
        backend->lastCxxRetTemp_ = slot;
        backend->lastCxxRetValue_ = parsed.Primary;
        return parsed;
}

bool MainListener::ArgumentNamesStorageRoot(antlr4::ParserRuleContext* ctx, llvm::Value* storage,
                                             const std::string& thisFieldPath, bool* mayWrite,
                                             antlr4::ParserRuleContext* targetCtx)
{
        if (mayWrite != nullptr) *mayWrite = false;
        if (ctx == nullptr || storage == nullptr) return false;
        llvm::SmallVector<const llvm::Value*, 4> roots;
        llvm::getUnderlyingObjects(storage, roots);
        bool named = false;
        auto fieldPathsOverlap = [](const std::string& a, const std::string& b) {
            auto prefix = [](const std::string& x, const std::string& y) {
                return !x.empty() && x.size() <= y.size() && y.compare(0, x.size(), x) == 0
                    && (x.size() == y.size() || y[x.size()] == '.' || y[x.size()] == '[');
            };
            return prefix(a, b) || prefix(b, a);
        };
        std::string fieldPathForComparison = thisFieldPath;
        struct MemberPathInfo { std::string path; bool mayWrite = false; bool cut = false; };
        auto memberPathBeforeCallOrIndex = [&](CFlatParser::PostfixExpressionContext* postfix) {
            MemberPathInfo result;
            std::string path;
            if (postfix == nullptr) return result;
            bool pastPrimary = false;
            for (size_t i = 0; i < postfix->children.size(); ++i)
            {
                auto* child = postfix->children[i];
                if (!pastPrimary)
                {
                    if (dynamic_cast<CFlatParser::PrimaryExpressionContext*>(child) != nullptr)
                        pastPrimary = true;
                    continue;
                }
                auto* terminal = dynamic_cast<antlr4::tree::TerminalNode*>(child);
                if (terminal != nullptr && terminal->getText() == "[")
                {
                    auto* index = i + 1 < postfix->children.size()
                        ? dynamic_cast<CFlatParser::ExpressionContext*>(postfix->children[i + 1]) : nullptr;
                    bool integerLiteral = false;
                    if (index != nullptr)
                    {
                        size_t terminalCount = 0;
                        auto inspect = [&](auto&& self, antlr4::tree::ParseTree* node) -> void {
                            if (auto* token = dynamic_cast<antlr4::tree::TerminalNode*>(node))
                            {
                                ++terminalCount;
                                const std::string literal = token->getText();
                                integerLiteral = token->getSymbol()->getType() == CFlatParser::Constant
                                    && !literal.empty() && literal[0] >= '0' && literal[0] <= '9';
                            }
                            else if (node != nullptr)
                                for (auto* nested : node->children) self(self, nested);
                        };
                        inspect(inspect, index);
                        integerLiteral = integerLiteral && terminalCount == 1;
                    }
                    // Compare the value, not the spelling (`arr[1]` == `arr[0x1]`); a Constant whose
                    // integer value is not proven (float, overflow) cuts the path like any other index.
                    std::optional<unsigned long long> literalValue;
                    if (integerLiteral)
                    {
                        std::string digits = index->getText();
                        while (!digits.empty() && (digits.back() == 'u' || digits.back() == 'U'
                                                   || digits.back() == 'l' || digits.back() == 'L'))
                            digits.pop_back();
                        int base = 10;
                        size_t start = 0;
                        if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
                            base = 16, start = 2;
                        else if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'b' || digits[1] == 'B'))
                            base = 2, start = 2;
                        else if (digits.size() > 1 && digits[0] == '0')
                            base = 8, start = 1;
                        unsigned long long value = 0;
                        const char* first = digits.data() + start;
                        const char* last = digits.data() + digits.size();
                        auto [end, ec] = std::from_chars(first, last, value, base);
                        if (first != last && ec == std::errc() && end == last) literalValue = value;
                    }
                    if (literalValue)
                    {
                        path += "[" + std::to_string(*literalValue) + "]";
                        i += 2;
                        continue;
                    }
                    result.cut = true;
                    result.mayWrite = !path.empty() && fieldPathsOverlap(path, fieldPathForComparison);
                    break;
                }
                if (terminal != nullptr && terminal->getText() == "(")
                {
                    result.cut = true;
                    result.mayWrite = !path.empty() && fieldPathsOverlap(path, fieldPathForComparison);
                    break;
                }
                auto* member = dynamic_cast<CFlatParser::MemberNameTokenContext*>(child);
                if (member == nullptr) continue;
                size_t next = i + 1;
                if (next < postfix->children.size()
                    && dynamic_cast<CFlatParser::GenericTypeParametersContext*>(postfix->children[next]) != nullptr)
                    ++next;
                if (next < postfix->children.size())
                    if (auto* terminal = dynamic_cast<antlr4::tree::TerminalNode*>(postfix->children[next]);
                        terminal != nullptr && terminal->getText() == "(")
                    {
                        result.cut = true;
                        result.mayWrite = !path.empty() && fieldPathsOverlap(path, fieldPathForComparison);
                        break;
                    }
                path += (path.empty() ? "" : ".") + member->getText();
            }
            result.path = path;
            return result;
        };
        if (targetCtx != nullptr)
        {
            CFlatParser::PrimaryExpressionContext* targetPrimary = nullptr;
            auto findPrimary = [&](auto&& self, antlr4::tree::ParseTree* node) -> void {
                if (targetPrimary != nullptr || node == nullptr) return;
                if (auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node);
                    primary != nullptr && primary->genericIdentifier() != nullptr
                    && primary->genericIdentifier()->Identifier() != nullptr)
                {
                    targetPrimary = primary;
                    return;
                }
                for (auto* child : node->children) self(self, child);
            };
            findPrimary(findPrimary, targetCtx);
            if (targetPrimary != nullptr)
            {
                const std::string rootName = targetPrimary->genericIdentifier()->Identifier()->getText();
                auto* postfix = dynamic_cast<CFlatParser::PostfixExpressionContext*>(targetPrimary->parent);
                const auto targetPath = memberPathBeforeCallOrIndex(postfix);
                if (targetPath.path.find('[') != std::string::npos)
                {
                    if (rootName == "this")
                        fieldPathForComparison = targetPath.path;
                    else if (Compiler()->GetScopedLocalOrArgument(rootName).Storage == nullptr
                        && Compiler()->HasMemberVariable(rootName))
                        fieldPathForComparison = rootName
                            + (targetPath.path.empty() || targetPath.path[0] == '[' ? "" : ".")
                            + targetPath.path;
                }
            }
        }
        auto visit = [&](auto&& self, antlr4::tree::ParseTree* node) -> void {
            if (named || node == nullptr) return;
            if (auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node))
                if (auto* generic = primary->genericIdentifier();
                    generic != nullptr && generic->Identifier() != nullptr)
                {
                    // Resolved as codegen resolves a bare name: the innermost local or parameter.
                    const std::string name = generic->Identifier()->getText();
                    auto* postfix = dynamic_cast<CFlatParser::PostfixExpressionContext*>(primary->parent);
                    const auto memberInfo = memberPathBeforeCallOrIndex(postfix);
                    const std::string& memberPath = memberInfo.path;
                    if (memberInfo.mayWrite && mayWrite != nullptr) *mayWrite = true;
                    if (!memberInfo.mayWrite && !fieldPathForComparison.empty() && name == "this" && !memberPath.empty()
                        && fieldPathsOverlap(memberPath, fieldPathForComparison))
                    {
                        named = true;
                        return;
                    }
                    auto local = Compiler()->GetScopedLocalOrArgument(name);
                    llvm::Value* namedStorage = local.Storage;
                    if (namedStorage == nullptr && Compiler()->HasMemberVariable(name))
                    {
                        auto member = Compiler()->GetMemberVariable(name);
                        const std::string separator = memberPath.empty() || memberPath[0] == '[' ? "" : ".";
                        const std::string fullMemberPath = member.ThisFieldPath.empty()
                            ? name + separator + memberPath
                            : member.ThisFieldPath + separator + memberPath;
                        // A path cut at a call or a non-literal subscript may reach any sub-object:
                        // an overlap is a may-write, never a named write.
                        if (memberInfo.cut && !fieldPathForComparison.empty())
                        {
                            if (mayWrite != nullptr && fieldPathsOverlap(fullMemberPath, fieldPathForComparison))
                                *mayWrite = true;
                            return;
                        }
                        if (!memberInfo.mayWrite && !fieldPathForComparison.empty()
                            && fieldPathsOverlap(fullMemberPath, fieldPathForComparison))
                        {
                            named = true;
                            return;
                        }
                        namedStorage = member.Storage;
                    }
                    if (namedStorage == nullptr)
                    {
                        auto global = Compiler()->GetGlobalVariableNV(name);
                        namedStorage = global.Storage;
                        if (namedStorage == nullptr && postfix != nullptr)
                        {
                            std::string qualifiedName = name;
                            for (auto* member : postfix->memberNameToken())
                            {
                                qualifiedName += "." + member->getText();
                                global = Compiler()->GetGlobalVariableNV(qualifiedName);
                                if (global.Storage != nullptr)
                                {
                                    namedStorage = global.Storage;
                                    break;
                                }
                            }
                        }
                    }
                    const llvm::Value* root = namedStorage != nullptr
                        ? llvm::getUnderlyingObject(namedStorage) : nullptr;
                    if (root != nullptr && std::find(roots.begin(), roots.end(), root) != roots.end())
                    {
                        named = true;
                        return;
                    }
                }
            for (auto* child : node->children) self(self, child);
        };
        visit(visit, ctx);
        return named;
}

antlr4::ParserRuleContext* MainListener::CxxDefaultOnlyTernary(antlr4::ParserRuleContext* ctx)
{
        auto* root = DeferrableTernaryNode(ctx);
        if (root == nullptr) return nullptr;
        auto onlyDefaults = [&](auto&& self, antlr4::ParserRuleContext* node) -> bool {
            if (auto* nested = DeferrableTernaryNode(node))
                return self(self, nested->expression()) && self(self, nested->conditionalExpression());
            return IsDefaultOnlyExpression(node);
        };
        return onlyDefaults(onlyDefaults, root) ? root : nullptr;
}

bool MainListener::IsCxxTernaryArgument(antlr4::ParserRuleContext* ctx)
{
        return DeferrableTernaryNode(ctx) != nullptr;
}

bool MainListener::CanMaterializeMixedDefault(const std::string& defaultType)
{
    auto* compiler = Compiler();
    std::string ctorError;
    compiler->TryBindCxxImplicitDefaultCtor(defaultType, ctorError);
    return compiler->FindCxxDefaultCtor(defaultType) != nullptr
        || !compiler->CxxElementNeedsDefaultConstruction(defaultType);
}

// CALLPAR: `c ? default : local` (either order) whose other arm is a live nontrivial C++ class local.
bool MainListener::IsCxxDefaultLocalTernary(antlr4::ParserRuleContext* ctx)
{
        auto* conditional = DeferrableTernaryNode(ctx);
        if (conditional == nullptr || conditional->expression() == nullptr
            || conditional->conditionalExpression() == nullptr)
            return false;
        const bool trueDefault = IsDefaultOnlyExpression(conditional->expression());
        if (trueDefault == IsDefaultOnlyExpression(conditional->conditionalExpression())) return false;
        antlr4::tree::ParseTree* other = trueDefault
            ? static_cast<antlr4::tree::ParseTree*>(conditional->conditionalExpression())
            : static_cast<antlr4::tree::ParseTree*>(conditional->expression());
        auto* postfix = SolePostfixExpressionShape(other);
        auto* primary = postfix != nullptr && postfix->children.size() == 1 ? postfix->primaryExpression() : nullptr;
        auto* generic = primary != nullptr ? primary->genericIdentifier() : nullptr;
        if (generic == nullptr || generic->genericTypeParameters() != nullptr || generic->Identifier() == nullptr)
            return false;
        auto* compiler = Compiler(ctx);
        auto local = compiler->GetScopedLocalOrArgument(generic->Identifier()->getText());
        return local.Storage != nullptr && !local.TypeAndValue.Pointer && local.TypeAndValue.PointerDepth == 0
            && !local.TypeAndValue.IsAlias
            && compiler->IsForeignNontrivialCxxClass(compiler->ResolveTypeAlias(local.TypeAndValue.TypeName));
}

/*
 * REFDEF: C++ makes `c ? default : place` a prvalue, so the caller copies the selected place at the
 * argument's position. Only this exact shape is materialized that way; any call or move leaf keeps
 * the deferred path.
 */
bool MainListener::IsCxxMixedDefaultPlaceTernary(antlr4::ParserRuleContext* ctx)
{
        auto* root = DeferrableTernaryNode(ctx);
        if (root == nullptr) return false;
        bool hasDefault = false;
        bool hasPlace = false;
        bool hasOther = false;
        auto walk = [&](auto&& self, antlr4::tree::ParseTree* node) -> void {
            if (auto* conditional = DeferrableTernaryNode(node))
            {
                self(self, conditional->expression());
                self(self, conditional->conditionalExpression());
                return;
            }
            auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(node);
            if (rule != nullptr && IsDefaultOnlyExpression(rule)) hasDefault = true;
            else if (BareIdentifierInExpression(node) != nullptr) hasPlace = true;
            else hasOther = true;
        };
        walk(walk, root);
        return hasDefault && hasPlace && !hasOther;
}

/*
 * T418: every other argument must be inert because the wrapper reads the selected place after the
 * caller's arguments. A class prvalue beside it also pays the wrapper's extra copy.
 */
bool MainListener::IsCxxInertCallArgument(antlr4::ParserRuleContext* ctx, bool condition)
{
        if (ctx == nullptr) return false;
        if (!condition && IsDefaultOnlyExpression(ctx)) return true;
        if (auto* conditional = DeferrableTernaryNode(ctx); conditional != nullptr && !condition)
        {
            std::string placeType;
            if (!IsCxxDeferrableDefaultTernary(conditional, placeType)) return false;
            auto safeBranch = [&](auto&& self, antlr4::tree::ParseTree* branch) -> bool {
                auto* nested = DeferrableTernaryNode(branch);
                if (nested == nullptr) return true; // its leaf shape was checked above
                return IsCxxInertCallArgument(nested->logicalOrExpression(), true)
                    && self(self, nested->expression())
                    && self(self, nested->conditionalExpression());
            };
            return IsCxxInertCallArgument(conditional->logicalOrExpression(), true)
                && safeBranch(safeBranch, conditional->expression())
                && safeBranch(safeBranch, conditional->conditionalExpression());
        }
        if (!condition)
        {
            LLVMBackend::NamedVariable container;
            LLVMBackend::DeclTypeAndValue field;
            if (ResolveCxxDeferredLocalField(Compiler(), ctx, container, field)
                && !field.TypeName.empty()) return true;
        }
        auto* postfix = SolePostfixExpressionShape(ctx);
        if (postfix == nullptr || postfix->children.size() != 1) return false;
        auto* primary = postfix->primaryExpression();
        if (primary == nullptr) return false;
        if (primary->Constant() != nullptr || primary->NullPtrConstant() != nullptr
            || !primary->StringLiteral().empty()) return true;
        auto* identifier = BareIdentifierInExpression(ctx);
        if (identifier == nullptr) return false;
        const auto* local = Compiler()->FindLiveNamedVariable(identifier->getText());
        if (local == nullptr || local->Storage == nullptr) return false;
        // A condition must be a builtin read; overloaded operator bool may mutate an earlier leaf.
        return CxxInertLocalReadType(Compiler(), local->TypeAndValue, condition);
}

bool MainListener::IsCxxSideEffectFreeScalarExpression(antlr4::tree::ParseTree* node)
{
        auto* compiler = Compiler();
        auto resolveNameType = [&](antlr4::tree::TerminalNode* identifier,
                                   LLVMBackend::TypeAndValue& type) {
            if (identifier == nullptr) return false;
            const std::string name = identifier->getText();
            auto local = compiler->GetScopedLocalOrArgument(name);
            if (local.Storage != nullptr || local.Primary != nullptr || local.BitfieldStorage != nullptr)
            {
                type = local.TypeAndValue;
                return true;
            }
            const LLVMBackend::StructData* thisStruct = nullptr;
            const LLVMBackend::BitfieldInfo* bitfield = nullptr;
            int fieldIndex = -1;
            if (compiler->FindImplicitThisField(name, &thisStruct, &fieldIndex, &bitfield))
            {
                if (bitfield != nullptr)
                {
                    type.TypeName = bitfield->TypeName;
                    return true;
                }
                if (thisStruct != nullptr && fieldIndex >= 0
                    && static_cast<size_t>(fieldIndex) < thisStruct->StructFields.size())
                {
                    type = static_cast<const LLVMBackend::TypeAndValue&>(
                        thisStruct->StructFields[static_cast<size_t>(fieldIndex)]);
                    return true;
                }
            }
            auto globalType = compiler->globalVariableTypes.find(name);
            if (globalType != compiler->globalVariableTypes.end())
            {
                type = globalType->second;
                return true;
            }
            return false;
        };
        auto isScalarType = [](const LLVMBackend::TypeAndValue& type) {
            return type.IsPrimitive() || type.Pointer || type.PointerDepth != 0;
        };
        auto isScalar = [&](auto&& self, antlr4::tree::ParseTree* expression) -> bool {
            if (auto* expr = dynamic_cast<CFlatParser::ExpressionContext*>(expression))
                return expr->assignmentExpression() != nullptr
                    && self(self, expr->assignmentExpression());
            if (auto* assignment = dynamic_cast<CFlatParser::AssignmentExpressionContext*>(expression))
            {
                if (assignment->assignmentOperator() != nullptr) return false;
                if (assignment->conditionalExpression() != nullptr)
                    return self(self, assignment->conditionalExpression());
                return assignment->DigitSequence() != nullptr;
            }
            if (auto* conditional = dynamic_cast<CFlatParser::ConditionalExpressionContext*>(expression))
            {
                if (conditional->expression() != nullptr || conditional->QuestionQuestion() != nullptr)
                    return false;
                return conditional->logicalOrExpression() != nullptr
                    && self(self, conditional->logicalOrExpression());
            }
            if (auto* unary = dynamic_cast<CFlatParser::UnaryExpressionContext*>(expression))
            {
                if (unary->postfixExpression() != nullptr)
                    return self(self, unary->postfixExpression());
                auto* op = unary->unaryOperator();
                return op != nullptr && unary->castExpression() != nullptr
                    && (op->Plus() != nullptr || op->Minus() != nullptr
                        || op->Tilde() != nullptr || op->Not() != nullptr)
                    && self(self, unary->castExpression());
            }
            if (auto* cast = dynamic_cast<CFlatParser::CastExpressionContext*>(expression))
            {
                if (cast->castTypeName() != nullptr) return false;
                return cast->unaryExpression() != nullptr && self(self, cast->unaryExpression());
            }
            if (auto* postfix = dynamic_cast<CFlatParser::PostfixExpressionContext*>(expression))
            {
                if (!postfix->LeftParen().empty() || !postfix->LeftBracket().empty()
                    || !postfix->Arrow().empty() || !postfix->QuestionDot().empty()
                    || !postfix->PlusPlus().empty() || !postfix->MinusMinus().empty())
                    return false;
                auto* primary = postfix->primaryExpression();
                if (primary == nullptr) return false;
                if (primary->expression() != nullptr)
                    return postfix->children.size() == 1 && self(self, primary->expression());
                if (primary->Constant() != nullptr || primary->NullPtrConstant() != nullptr
                    || !primary->StringLiteral().empty())
                    return postfix->Dot().empty();
                auto* generic = primary->genericIdentifier();
                auto* baseIdentifier = generic != nullptr ? generic->Identifier() : nullptr;
                if (baseIdentifier == nullptr || generic->genericTypeParameters() != nullptr
                    || postfix->Dot().size() != postfix->memberNameToken().size())
                    return false;
                LLVMBackend::TypeAndValue valueType;
                if (!resolveNameType(baseIdentifier, valueType)) return false;
                for (auto* member : postfix->memberNameToken())
                {
                    auto* memberIdentifier = member->Identifier();
                    if (memberIdentifier == nullptr) return false;
                    auto structure = compiler->GetDataStructure(valueType.TypeName);
                    if (structure.StructType == nullptr)
                        structure = compiler->GetDataStructure(compiler->ResolveTypeAlias(valueType.TypeName));
                    auto field = std::find_if(structure.StructFields.begin(), structure.StructFields.end(),
                        [&](const auto& candidate) {
                            return candidate.VariableName == memberIdentifier->getText();
                        });
                    if (field == structure.StructFields.end()) return false;
                    valueType = static_cast<const LLVMBackend::TypeAndValue&>(*field);
                }
                return isScalarType(valueType);
            }
            if (auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(expression))
            {
                if (primary->Constant() != nullptr || primary->NullPtrConstant() != nullptr
                    || !primary->StringLiteral().empty()) return true;
                if (primary->expression() != nullptr) return self(self, primary->expression());
                return false;
            }
            auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(expression);
            if (rule == nullptr) return false;
            const bool builtinBinary =
                dynamic_cast<CFlatParser::LogicalOrExpressionContext*>(rule) != nullptr
                || dynamic_cast<CFlatParser::LogicalAndExpressionContext*>(rule) != nullptr
                || dynamic_cast<CFlatParser::InclusiveOrExpressionContext*>(rule) != nullptr
                || dynamic_cast<CFlatParser::ExclusiveOrExpressionContext*>(rule) != nullptr
                || dynamic_cast<CFlatParser::AndExpressionContext*>(rule) != nullptr
                || dynamic_cast<CFlatParser::EqualityExpressionContext*>(rule) != nullptr
                || dynamic_cast<CFlatParser::RelationalExpressionContext*>(rule) != nullptr
                || dynamic_cast<CFlatParser::ShiftExpressionContext*>(rule) != nullptr
                || dynamic_cast<CFlatParser::AdditiveExpressionContext*>(rule) != nullptr
                || dynamic_cast<CFlatParser::MultiplicativeExpressionContext*>(rule) != nullptr;
            if (builtinBinary)
            {
                bool hasOperand = false;
                for (auto* child : rule->children)
                {
                    auto* childRule = dynamic_cast<antlr4::ParserRuleContext*>(child);
                    if (childRule == nullptr) continue;
                    hasOperand = true;
                    if (!self(self, childRule)) return false;
                }
                return hasOperand;
            }
            return rule->children.size() == 1 && self(self, rule->children.front());
        };
        return isScalar(isScalar, node);
}

bool MainListener::BuildCxxDeferredDefaultTernary(antlr4::ParserRuleContext* ctx,
        const std::string& defaultType, LLVMBackend::NamedVariable& out, bool guardPlace,
        bool callOnly, bool materializeMixed)
{
        auto* compiler = Compiler();
        auto* root = DeferrableTernaryNode(ctx);
        auto* objectType = compiler->GetType(LLVMBackend::TypeAndValue{ .TypeName = defaultType });
        if (root == nullptr || objectType == nullptr) return false;
        auto* int32 = compiler->builder->getInt32Ty();
        auto* selector = compiler->AllocaAtEntry(int32, nullptr, "cxx.ternary.sel");
        std::vector<LLVMBackend::NamedVariable> children(1);
        int leafCount = 0;
        bool ok = true;
        bool sawCall = false;
        bool onlyCalls = true;
        std::unordered_map<antlr4::tree::ParseTree*, LLVMBackend::NamedVariable> deferredFieldPlaces;
        bool hasDefaultLeaf = false;
        bool hasPlaceLeaf = false;
        bool hasCallLeaf = false;
        bool hasMoveLeaf = false;
        auto prepareFieldPlaces = [&](auto&& self, antlr4::tree::ParseTree* node) -> bool {
            if (auto* conditional = DeferrableTernaryNode(node))
                return self(self, conditional->expression())
                    && self(self, conditional->conditionalExpression());
            auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(node);
            if (rule != nullptr && IsDefaultOnlyExpression(rule))
            {
                hasDefaultLeaf = true;
                onlyCalls = false;
                return !callOnly;
            }
            if (TopLevelMoveExpression(node) != nullptr)
            {
                hasMoveLeaf = true;
                onlyCalls = false;
                return !callOnly;
            }
            if (BareIdentifierInExpression(node) != nullptr)
            {
                hasPlaceLeaf = true;
                onlyCalls = false;
                return !callOnly;
            }
            LLVMBackend::NamedVariable callable;
            LLVMBackend::TypeAndValue callReturn;
            if (ResolveCxxDeferredCallArm(node, callable, callReturn))
            {
                if (compiler->ResolveTypeAlias(callReturn.TypeName)
                    != compiler->ResolveTypeAlias(defaultType))
                    return false;
                hasCallLeaf = true;
                sawCall = true;
                return true;
            }
            LLVMBackend::NamedVariable container;
            LLVMBackend::DeclTypeAndValue field;
            CFlatParser::PostfixExpressionContext* fieldPostfix = nullptr;
            if (!ResolveCxxDeferredLocalField(compiler, node, container, field, &fieldPostfix))
            {
                onlyCalls = false;
                return !callOnly;
            }
            onlyCalls = false;
            hasPlaceLeaf = true;
            if (callOnly) return false;
            if (field.TypeName.empty()) return false;
            auto place = ParsePostfixExpression(fieldPostfix, true);
            if (place.Storage == nullptr) return false;
            place.Primary = nullptr; // keep the lvalue address; the wrapper reads it only in its selected arm
            deferredFieldPlaces.emplace(node, std::move(place));
            return true;
        };
        if (!prepareFieldPlaces(prepareFieldPlaces, root)
            || (callOnly && (!sawCall || !onlyCalls))) return false;
        const bool materializeMixedPlace = guardPlace && materializeMixed && hasDefaultLeaf && hasPlaceLeaf
            && !hasCallLeaf && !hasMoveLeaf && CanMaterializeMixedDefault(defaultType);
        llvm::AllocaInst* earlyValueSlot = nullptr;
        llvm::AllocaInst* earlyValueLive = nullptr;
        if (materializeMixedPlace)
        {
            earlyValueSlot = llvm::dyn_cast_or_null<llvm::AllocaInst>(
                compiler->AllocaAtEntry(objectType, nullptr, "cxx.ternary.prvalue"));
            earlyValueLive = llvm::dyn_cast_or_null<llvm::AllocaInst>(
                compiler->AllocaAtEntry(compiler->builder->getInt1Ty(), nullptr,
                                        "cxx.ternary.prvalue.live"));
            if (earlyValueSlot == nullptr || earlyValueLive == nullptr) return false;
            compiler->builder->CreateStore(compiler->builder->getInt1(false), earlyValueLive);
        }
        auto constructDefaultValue = [&]() {
            std::string ctorError;
            compiler->TryBindCxxImplicitDefaultCtor(defaultType, ctorError);
            const LLVMBackend::CxxClassInfo::Structor* ctor =
                compiler->FindCxxDefaultCtor(defaultType);
            if (ctor != nullptr)
            {
                if (!compiler->EmitCxxStructorCall(defaultType, *ctor, earlyValueSlot, {}))
                    return false;
            }
            else if (compiler->CxxElementNeedsDefaultConstruction(defaultType))
                return false;
            else
                compiler->builder->CreateStore(llvm::Constant::getNullValue(objectType),
                                                earlyValueSlot);
            compiler->builder->CreateStore(compiler->builder->getInt1(true), earlyValueLive);
            return true;
        };
        // Leaves are numbered left to right; a subtree covers a contiguous range, so `sel < m` picks its true side.
        auto emit = [&](auto&& self, antlr4::ParserRuleContext* node) -> std::string {
            if (!ok) return {};
            if (auto* conditional = DeferrableTernaryNode(node))
            {
                std::optional<BranchPostfixScope> condPostfix;
                condPostfix.emplace(this);
                const auto mark = compiler->MarkOwnedTemps();
                auto condTv = ParseLogicalOrExpression(conditional->logicalOrExpression(), ResultUse::Value);
                condPostfix->Flush();
                condPostfix.reset();
                if (condTv.value == nullptr) { ok = false; return {}; }
                auto* condition = compiler->CoerceToBoolCondition(ConvertConditionToBool(condTv));
                compiler->FlushOwnedTempsSince(mark, nullptr, nullptr);
                auto* trueBlock = compiler->CreateBasicBlock("cxx_ternary_sel_true");
                auto* falseBlock = compiler->CreateBasicBlock("cxx_ternary_sel_false");
                auto* resumeBlock = compiler->CreateBasicBlock("cxx_ternary_sel_resume");
                compiler->CreateConditionJump(condition, trueBlock, falseBlock);
                std::string trueShape = self(self, conditional->expression());
                const int split = leafCount;
                compiler->CreateJump(resumeBlock);
                compiler->SwitchToBlock(falseBlock);
                std::string falseShape = self(self, conditional->conditionalExpression());
                compiler->CreateJump(resumeBlock);
                compiler->SwitchToBlock(resumeBlock);
                return std::format("(@S < {} ? {} : {})", split, trueShape, falseShape);
            }
            compiler->builder->CreateStore(compiler->builder->getInt32(leafCount++), selector);
            if (IsDefaultOnlyExpression(node))
            {
                if (materializeMixedPlace && !constructDefaultValue())
                { ok = false; return {}; }
                return "@D";
            }
            LLVMBackend::NamedVariable place;
            LLVMBackend::TypeAndValue callReturn;
            if (ResolveCxxDeferredCallArm(node, place, callReturn))
            {
                if (callReturn.TypeName != defaultType) { ok = false; return {}; }
                const size_t childIndex = children.size();
                children.push_back(std::move(place));
                return "@F" + std::to_string(childIndex);
            }
            if (auto* move = TopLevelMoveExpression(node))
                place = ParseMoveExpression(move);
            else if (auto field = deferredFieldPlaces.find(node); field != deferredFieldPlaces.end())
                place = field->second;
            else
                place = ParseIdentifier(BareIdentifierInExpression(node));
            llvm::Value* loaded = place.Primary != nullptr ? place.Primary : LoadNamedVariable(place);
            if (loaded == nullptr || place.Storage == nullptr) { ok = false; return {}; }
            place.Primary = loaded;
            place.BaseType = loaded->getType();
            if (materializeMixedPlace)
            {
                if (!compiler->EmitCxxCopyOrMoveConstruct(defaultType, earlyValueSlot,
                        place.Storage, false, "argument"))
                { ok = false; return {}; }
                compiler->builder->CreateStore(compiler->builder->getInt1(true), earlyValueLive);
                return "@0";
            }
            const bool movedLeaf = place.IsExplicitMove;
            place.IsExplicitMove = false; // the wrapper parameter receives the caller's lvalue
            place.IsRvalue = false;
            place.TypeAndValue.VariableName.clear();  // a positional child, not a named argument
            const size_t childIndex = children.size();
            children.push_back(std::move(place));
            return movedLeaf ? "@M" + std::to_string(childIndex)
                             : "@" + std::to_string(childIndex);
        };
        const std::string shape = emit(emit, root);
        if (!ok || shape.empty()) return false;
        // A leaf was read inside its arm block; the call after the join reads the slot again.
        for (size_t i = 1; i < children.size(); ++i)
            if (!compiler->IsCxxFunctionDesignatorArgument(children[i]))
                children[i].Primary = compiler->CreateLoad(children[i].BaseType, children[i].Storage);
        LLVMBackend::NamedVariable& sel = children.front();
        sel.Primary = compiler->CreateLoad(int32, selector);
        sel.BaseType = int32;
        sel.TypeAndValue.TypeName = "int";
        sel.IsRvalue = true;
        if (materializeMixedPlace)
        {
            // REFDEF: the caller's copy is an ordinary class prvalue temporary, as `T(args)` is.
            out = {};
            out.Primary = compiler->builder->CreateLoad(objectType, earlyValueSlot,
                                                         "cxx.ternary.prvalue.value");
            out.BaseType = objectType;
            out.Storage = earlyValueSlot;
            out.TypeAndValue.TypeName = defaultType;
            out.IsRvalue = true;
            out.IsCxxPrvalueTemp = true;
            out.CxxPlaceGuardType = defaultType;
            compiler->RegisterOwnedStructTemp(earlyValueSlot, defaultType, earlyValueLive);
            return true;
        }
        out = {};
        out.Primary = llvm::UndefValue::get(objectType);
        out.BaseType = objectType;
        out.TypeAndValue.TypeName = defaultType;
        out.IsRvalue = true;
        out.IsCxxDirectConstruction = true;
        out.CxxDirectConstructionType = defaultType;
        out.CxxDirectConstructionArguments = std::move(children);
        out.CxxDeferredTernaryShape = (guardPlace ? "@G" : "") + shape;
        return true;
}

/*
 * A view read out of a struct FIELD names no declared slot, so recover the element from the
 * field's own declaration. Three addressings, one question: the field GEP handed in as storage,
 * a load off that GEP, or an `extractvalue` out of a by-value struct that never reached memory
 * (a call result). Anything unresolvable stays unnamed - the caller then leaves the join alone.
 */
bool MainListener::ViewFieldElementForRead(llvm::Value* value, llvm::Value* storage,
                                           LLVMBackend::TypeAndValue& out) const {
        auto* compiler = compilerLLVM;
        if (storage == nullptr)
            if (auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(value))
                storage = load->getPointerOperand();
        if (const auto* field = compiler->FindDeclaredFieldTypeAndValueForStorage(storage);
            field != nullptr && field->IsArrayView)
        {
            out = *field;
            return true;
        }
        if (const auto* field = compiler->FindDeclaredFieldTypeAndValueForValue(value);
            field != nullptr && field->IsArrayView)
        {
            out = *field;
            return true;
        }
        return false;
    }

/*
 * A `T[]` view is a thin pointer, so a '?:' arm names its element only where the front end knew
 * it: the declared slot the arm was loaded from, the struct field it was read out of, the callee's
 * declared return type, or an inner join already stamped in the ledger. An arm that resolves to
 * none of those stays unnamed and the join is left alone (status quo: the element gate then takes
 * its unnamed escape).
 */
bool MainListener::TernaryArmViewType(llvm::Value* value, llvm::Value* storage,
                                      LLVMBackend::TypeAndValue& out) const {
        if (value == nullptr || !value->getType()->isPointerTy()) return false;
        auto* compiler = compilerLLVM;
        if (const auto* join = compiler->FindViewJoinType(value)) { out = *join; return true; }
        if (storage == nullptr)
            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(value))
                storage = load->getPointerOperand();
        if (storage != nullptr)
        {
            if (const auto* declared = compiler->FindDeclaredTypeAndValueForStorage(storage);
                declared != nullptr && declared->IsArrayView)
            {
                out = *declared;
                return true;
            }
        }
        // A FIXED array arm never loads: it decays to an all-zero GEP off its own storage, so
        // recover that storage. It indexes by its OWN element, so name it and reshape it into
        // the view it decays to.
        const llvm::Value* arrayStorage = storage;
        if (arrayStorage == nullptr)
            if (auto* gep = llvm::dyn_cast<llvm::GEPOperator>(value);
                gep != nullptr && gep->getSourceElementType()->isArrayTy()
                && gep->hasAllZeroIndices())
                arrayStorage = gep->getPointerOperand();
        const llvm::Value* fixedSlot = arrayStorage != nullptr ? arrayStorage : value;
        const auto* fixed = compiler->FindDeclaredTypeAndValueForStorage(fixedSlot);
        // A fixed-array FIELD names no local slot; its element lives on the field's declaration.
        if (fixed == nullptr && arrayStorage != nullptr)
            fixed = compiler->FindDeclaredFieldTypeAndValueForStorage(arrayStorage);
        if (fixed != nullptr && !fixed->IsArrayView && fixed->ConstArraySize != 0)
        {
            LLVMBackend::TypeAndValue shaped = *fixed;
            if (compiler->ReshapeFixedArrayAsView(shaped)) { out = shaped; return true; }
        }
        // A FIELD read names no local slot; its element lives on the field's declaration.
        if (ViewFieldElementForRead(value, storage, out)) return true;
        if (auto* call = llvm::dyn_cast<llvm::CallBase>(value))
            if (const auto* symbol = compiler->FindSymbolForFunction(call->getCalledFunction());
                symbol != nullptr && symbol->ReturnType.IsArrayView)
            {
                out = symbol->ReturnType;
                return true;
            }
        return false;
    }

// A `nullptr` / `default` arm carries no element of its own, so it is a NULL VIEW of whatever the
// other arm indexes - it never makes the join unnameable.
static bool TernaryArmIsNullView(llvm::Value* value) {
        auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(value);
        return constant != nullptr && constant->getType()->isPointerTy() && constant->isNullValue();
    }

/*
 * Agreeing arms hand their element to the join so every binding door judges it; disagreeing arms
 * are rejected here, where the two elements are still both in view. One named arm plus a null
 * constant is still named: the null arm inherits the named element. Only a join where NEITHER arm
 * names an element stays unnamed (status quo: the element gate takes its unnamed escape).
 */
void MainListener::PropagateTernaryViewElement(antlr4::ParserRuleContext* ctx,
                                               llvm::Value* trueValue, llvm::Value* trueStorage,
                                               llvm::Value* falseValue, llvm::Value* falseStorage,
                                               llvm::Value* join, bool nullCoalesce,
                                               const LLVMBackend::TypedValue* trueShape,
                                               const LLVMBackend::TypedValue* falseShape) {
        LLVMBackend::TypeAndValue trueType;
        LLVMBackend::TypeAndValue falseType;
        bool trueNamed = TernaryArmViewType(trueValue, trueStorage, trueType);
        bool falseNamed = TernaryArmViewType(falseValue, falseStorage, falseType);
        auto recoverShape = [&](const LLVMBackend::TypedValue* shape,
                                LLVMBackend::TypeAndValue& type) {
            if (shape == nullptr || !shape->hasSourceType) return false;
            type = shape->sourceType;
            if (type.IsArrayView) return true;
            return type.ConstArraySize != 0 && compilerLLVM->ReshapeFixedArrayAsView(type);
        };
        if (!trueNamed) trueNamed = recoverShape(trueShape, trueType);
        if (!falseNamed) falseNamed = recoverShape(falseShape, falseType);
        if (!trueNamed && !falseNamed) return;
        if (!trueNamed)
        {
            if (!TernaryArmIsNullView(trueValue)) return;
            trueType = falseType;
        }
        else if (!falseNamed)
        {
            if (!TernaryArmIsNullView(falseValue)) return;
            falseType = trueType;
        }
        std::string trueElement;
        std::string falseElement;
        if (compilerLLVM->ArrayViewElementMismatch(trueType, falseType, trueElement, falseElement))
        {
            if (nullCoalesce)
                LogErrorContext(ctx, compilerLLVM->LocalizeMessage(
                    "cannot join an array view of '{}' with an array view of '{}' in a '??' - a "
                    "view indexes by its own element, so both arms must share it",
                    { trueElement, falseElement }));
            else
                LogErrorContext(ctx, compilerLLVM->LocalizeMessage(
                    "cannot join an array view of '{}' with an array view of '{}' in a '?:' - a "
                    "view indexes by its own element, so both arms must share it",
                    { trueElement, falseElement }));
            return;
        }
        LLVMBackend::TypeAndValue joinType;
        joinType.TypeName = trueType.TypeName;
        joinType.Pointer = true;
        joinType.ElemPointer = trueType.ElemPointer;
        joinType.IsArrayView = true;
        joinType.IsInterface = trueType.IsInterface;
        if (join != nullptr) compilerLLVM->RegisterViewJoinType(join, joinType);
    }

LLVMBackend::TypeAndValue MainListener::InferTernaryArmType(llvm::Value* value) {
        LLVMBackend::TypeAndValue result;
        if (value == nullptr) return result;
        auto* compiler = compilerLLVM;
        auto* type = value->getType();
        if (type->isPointerTy())
        {
            result.TypeName = compiler->ResolvePointerElementTypeName(value);
            if (!result.TypeName.empty()) result.Pointer = true;
            return result;
        }
        if (type == compiler->GetFatPtrType())
        {
            result.TypeName = compiler->FindFatInterfaceValueTypeName(value);
            result.IsInterface = !result.TypeName.empty()
                && result.TypeName != LLVMBackend::kAmbiguousFatInterface;
            return result;
        }
        result.TypeName = LLVMTypeToTypeName(type);
        return result;
    }

LLVMBackend::TypeAndValue MainListener::InferTernaryArmType(
        const LLVMBackend::TypedValue& value) {
        if (value.hasSourceType) return value.sourceType;
        auto result = InferTernaryArmType(value.value);
        if (result.Pointer && !value.sourceTypeName.empty())
        {
            result.TypeName = value.sourceTypeName;
            result.ElemPointer = value.elemPointer;
            result.PointerDepth = value.pointerDepth;
        }
        return result;
    }

/*
 * The pointee of a pointer join whose element ledger is empty. A `??` names it from the arms that
 * have a pointee (a null arm is skipped). A `?:` (phi / select) needs every non-null arm to name
 * the same pointee, else nothing is proven. Reads the joined value's arms only, never its text.
 */
LLVMBackend::TypeAndValue MainListener::JoinedPointerArmType(llvm::Value* joined) {
        LLVMBackend::TypeAndValue result;
        if (joined == nullptr || !joined->getType()->isPointerTy()) return result;
        std::string name;
        bool hasVoidPointer = false;
        bool incompatiblePointees = false;
        auto notePointee = [&](const LLVMBackend::TypeAndValue& armType) {
            if (armType.TypeName == "void" && armType.Pointer && !armType.ElemPointer
                && armType.PointerDepth < 2 && !armType.IsInterface
                && !armType.IsInterfacePointer && !armType.IsFunctionPointer)
            {
                hasVoidPointer = true;
                return;
            }
            if (name.empty()) name = armType.TypeName;
            else if (name != armType.TypeName) incompatiblePointees = true;
        };
        // One arm's pointee: owned-new ledger first (a `new` call returns void*), then the arm's
        // spelling, then the DECLARED type of a loaded slot (nullptr-initialized local/global/field).
        auto armPointee = [&](llvm::Value* arm) {
            if (const auto* owned = compilerLLVM->FindOwnedNewTemp(arm);
                owned != nullptr && !owned->TypeName.empty())
            {
                LLVMBackend::TypeAndValue named;
                named.TypeName = owned->TypeName;
                named.Pointer = true;
                return named;
            }
            auto armType = InferTernaryArmType(arm);
            if (armType.TypeName.empty())
            {
                if (const auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(arm);
                    load != nullptr)
                {
                    const auto* slot = load->getPointerOperand();
                    const auto* declared = compilerLLVM->FindDeclaredTypeAndValueForStorage(slot);
                    if (declared == nullptr)
                        declared = compilerLLVM->FindDeclaredFieldTypeAndValueForStorage(slot);
                    if (declared != nullptr && declared->Pointer && !declared->ElemPointer
                        && !declared->IsArrayView && !declared->IsSimd && !declared->IsInterface
                        && !declared->IsInterfacePointer && declared->ConstArraySize == 0)
                    {
                        armType.TypeName = declared->TypeName;
                        armType.Pointer = true;
                    }
                }
            }
            return armType;
        };
        if (const auto* join = compilerLLVM->FindNullCoalesceJoin(joined))
        {
            for (const auto& arm : join->Arms)
            {
                const auto armType = armPointee(arm.Value);
                if (!armType.Pointer || armType.TypeName.empty()) continue;
                notePointee(armType);
            }
        }
        else
        {
            llvm::SmallVector<llvm::Value*, 2> arms;
            if (auto* phi = llvm::dyn_cast<llvm::PHINode>(joined))
                arms.append(phi->incoming_values().begin(), phi->incoming_values().end());
            else if (auto* select = llvm::dyn_cast<llvm::SelectInst>(joined))
            {
                arms.push_back(select->getTrueValue());
                arms.push_back(select->getFalseValue());
            }
            for (llvm::Value* arm : arms)
            {
                if (llvm::isa_and_nonnull<llvm::ConstantPointerNull>(arm)) continue;
                const auto armType = armPointee(arm);
                if (!armType.Pointer || armType.TypeName.empty() || armType.IsInterface)
                    return result;
                notePointee(armType);
            }
        }
        if (hasVoidPointer) name = "void";
        else if (incompatiblePointees) return result;
        if (name.empty()) return result;
        result.TypeName = name;
        result.Pointer = true;
        return result;
    }

// C usual arithmetic conversions for a '?:' join: the unsigned arm wins only when its width
// covers the other's, so `cond ? (i64)-1 : 1u` stays signed.
static bool TernaryJoinIsUnsigned(bool trueUnsigned, unsigned trueBits,
                                  bool falseUnsigned, unsigned falseBits)
{
    // C promotes every integer narrower than int to signed int before this rank check, so a
    // mixed-sign narrow join (`cond ? (u8)255 : (i8)-1`) is a SIGNED int join.
    if (trueBits != 0 && trueBits < 32 && falseBits != 0 && falseBits < 32
        && trueUnsigned != falseUnsigned)
    {
        trueBits = falseBits = 32;
        trueUnsigned = falseUnsigned = false;
    }
    if (trueUnsigned == falseUnsigned) return trueUnsigned;
    return trueUnsigned ? trueBits >= falseBits : falseBits >= trueBits;
}



// Distinct pointer pointees need an explicit cast, except for a direct C++ derived-to-base join.
static bool UnrelatedPointerJoinTypes(LLVMBackend* compiler,
                                      const LLVMBackend::TypeAndValue& a,
                                      const LLVMBackend::TypeAndValue& b)
{
    auto depth = [](const LLVMBackend::TypeAndValue& t) {
        return t.ValuePointerDepth();
    };
    if (!a.Pointer || !b.Pointer || a.IsFunctionPointer || b.IsFunctionPointer
        || a.IsInterface || b.IsInterface || a.IsInterfacePointer || b.IsInterfacePointer
        || a.IsArrayView || b.IsArrayView || a.ElemPointer != b.ElemPointer
        || depth(a) != depth(b))
        return false;
    const std::string an = compiler->ResolveTypeAlias(a.TypeName);
    const std::string bn = compiler->ResolveTypeAlias(b.TypeName);
    if (an.empty() || bn.empty() || an == bn) return false;
    // Builtin synonyms (i32/int, i16/short, long/iN) are one type, as in assignment.
    if (LLVMBackend::CanonicalPointeeTypeName(an) == LLVMBackend::CanonicalPointeeTypeName(bn)) return false;
    if (depth(a) == 1 && !a.ElemPointer)
    {
        auto from = a;
        auto to = b;
        from.TypeName = an;
        to.TypeName = bn;
        if (compiler->IsCxxDerivedToBasePointer(from, to)) return false;
        std::swap(from, to);
        if (compiler->IsCxxDerivedToBasePointer(from, to)) return false;
    }
    return true;
}


// The interface a `?:` result is boxed into (`IS s = ...`, `unique<IS>`), else empty.
static std::string TernaryInterfaceDestination(LLVMBackend& compiler,
                                               const LLVMBackend::TypeAndValue& expected) {
        if (expected.TypeName.empty()) return {};
        if (compiler.IsCoreUniqueType(expected.TypeName))
        {
            std::string inner = MangledGenericArgument(compiler, expected.TypeName);
            return compiler.IsInterfaceType(inner) ? inner : std::string{};
        }
        std::string name = compiler.ResolveTypeAlias(expected.TypeName);
        return (expected.IsInterface || compiler.IsInterfaceType(name))
            && expected.PointerDepth <= 1 && !expected.ElemPointer ? name : std::string{};
}

bool MainListener::TernaryPointerJoinIsRejected(LLVMBackend* compiler,
        const LLVMBackend::TypeAndValue& trueType, const LLVMBackend::TypeAndValue& falseType,
        bool trueSimd, bool falseSimd, bool trueArrayView, bool falseArrayView,
        bool trueNullArm, bool falseNullArm, const std::string& interfaceDestination,
        bool destinationPointer)
{
        if (trueSimd || falseSimd || trueArrayView || falseArrayView
            || trueNullArm || falseNullArm
            || !UnrelatedPointerJoinTypes(compiler, trueType, falseType))
            return false;
        if (trueType.PointerDepth > 1 || trueType.ElemPointer
            || falseType.PointerDepth > 1 || falseType.ElemPointer)
            return true;
        const std::string trueClass = compiler->ResolveTypeAlias(trueType.TypeName);
        const std::string falseClass = compiler->ResolveTypeAlias(falseType.TypeName);
        if (!interfaceDestination.empty())
            return !compiler->StructImplementsInterface(trueClass, interfaceDestination)
                || !compiler->StructImplementsInterface(falseClass, interfaceDestination);
        if (destinationPointer) return true;
        return !ClassesShareInterface(trueClass, falseClass);
}

static bool IsNestedConditionalJoinArm(antlr4::ParserRuleContext* ctx)
{
        for (auto* node = ctx == nullptr ? nullptr : ctx->parent; node != nullptr; )
        {
            auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(node);
            if (rule == nullptr) return false;
            if (auto* conditional = dynamic_cast<CFlatParser::ConditionalExpressionContext*>(rule);
                conditional != nullptr
                && (conditional->Question() != nullptr
                    || conditional->QuestionQuestion() != nullptr))
                return true;
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

bool MainListener::UnifyTernaryArmTypes(CFlatParser::ConditionalExpressionContext* ctx,
                              llvm::Value*& trueValue, llvm::Value*& falseValue,
                              LLVMBackend::TypedValue& trueShape,
                              LLVMBackend::TypedValue& falseShape,
                              llvm::Value*& trueStorage, llvm::Value*& falseStorage,
                              const std::function<void()>& atTrue,
                              const std::function<void()>& atFalse,
                              size_t trueOccurrence, size_t falseOccurrence,
                              bool trueIsUnsigned, bool falseIsUnsigned,
                              const std::string& interfaceDestination, bool destinationPointer,
                              const LLVMBackend::TypeAndValue* storeDestination) {
        auto* compiler = Compiler(ctx);
        if (!trueValue || !falseValue) return true;

        // Two views have their own element-join rule; a single view arm decays like a pointer.
        if (!(trueShape.isArrayView && falseShape.isArrayView))
        {
            auto trueType = InferTernaryArmType(trueShape);
            auto falseType = InferTernaryArmType(falseShape);
            if (falseShape.isArrayView && !trueShape.isArrayView)
                std::swap(trueType, falseType);
            if (!UnrelatedPointerJoinTypes(compiler, trueType, falseType))
                compiler->RejectImplicitIntegerPointeePointerConversion(
                    trueType, falseType);
            // A pointer arm the ledger cannot type (a field address) keeps the pointee its shape names.
            auto shapePointee = [](const LLVMBackend::TypedValue& shape, llvm::Value* value,
                                   LLVMBackend::TypeAndValue inferred) {
                if (inferred.Pointer || value == nullptr
                    || !value->getType()->isPointerTy()
                    || shape.pointerDepth < 1 || shape.sourceTypeName.empty())
                    return inferred;
                inferred.Pointer = true;
                inferred.TypeName = shape.sourceTypeName;
                inferred.PointerDepth = shape.pointerDepth;
                inferred.ElemPointer = shape.elemPointer;
                return inferred;
            };
            // A string literal arm is a `char*` (clang's const char*) for the pointee check, so
            // `P* ?: "abc"` is refused like `P* ?: char*`; a literal into char* still joins.
            // A null/default other arm carries no pointee: the literal-store gate names that case.
            auto literalAsChar = [&](llvm::Value* value, LLVMBackend::TypeAndValue named,
                                     llvm::Value* other) {
                auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(value);
                if (named.Pointer || constant == nullptr || !compiler->IsStringLiteralConstant(constant)
                    || llvm::isa_and_nonnull<llvm::ConstantPointerNull>(other))
                    return named;
                named.Pointer = true;
                named.TypeName = "char";
                named.PointerDepth = 1;
                return named;
            };
            const auto trueNamed = literalAsChar(trueValue,
                shapePointee(trueShape, trueValue, InferTernaryArmType(trueShape)), falseValue);
            const auto falseNamed = literalAsChar(falseValue,
                shapePointee(falseShape, falseValue, InferTernaryArmType(falseShape)), trueValue);
            const bool trueNullArm = llvm::isa_and_nonnull<llvm::ConstantPointerNull>(trueValue)
                || (trueShape.hasSourceType && trueShape.sourceType.IsCxxNullptrT);
            const bool falseNullArm = llvm::isa_and_nonnull<llvm::ConstantPointerNull>(falseValue)
                || (falseShape.hasSourceType && falseShape.sourceType.IsCxxNullptrT);
            const bool joinRejected = TernaryPointerJoinIsRejected(compiler, trueNamed, falseNamed,
                    trueNamed.IsSimd, falseNamed.IsSimd,
                    trueShape.isArrayView, falseShape.isArrayView,
                    trueNullArm, falseNullArm, interfaceDestination, destinationPointer);
            // Agreeing arms still have to fit a pointer destination: the joined value carries no
            // struct pointee, so the store check after the join cannot see `NY*` arms into `NS*`.
            if (!joinRejected && destinationPointer && storeDestination != nullptr
                && !DeclExpectedTypeScope::explicitCast)
            {
                // A string literal arm is left to the literal-store gate, which names that case.
                auto isLiteral = [&](llvm::Value* value) {
                    auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(value);
                    return constant != nullptr && compiler->IsStringLiteralConstant(constant);
                };
                const std::pair<const LLVMBackend::TypeAndValue*, llvm::Value*> arms[] = {
                    { &trueNamed, trueValue }, { &falseNamed, falseValue } };
                for (const auto& [named, value] : arms)
                {
                    if (!isLiteral(value)
                        && compiler->IsImplicitUnrelatedStructPointeeConversion(*named, *storeDestination))
                    {
                        LogErrorContext(ctx, compiler->ImplicitIntegerPointeeConversionText(
                            *named, *storeDestination));
                        break;
                    }
                    // A `T**` join reaches the store as a bare pointer, so a cross-class C++ arm
                    // (`B** = c ? d1 : d2`) is refused per arm; depth 1 converts per arm below.
                    if (LLVMBackend::ArrayDecayedPointer(*named).ValuePointerDepth() >= 2
                        && compiler->RejectCxxClassPointerStore(
                            *storeDestination, *named, "conditional expression"))
                        break;
                }
            }
            if (joinRejected)
            {
                LogErrorContext(ctx, UnrelatedPointerJoinDiagnostic(
                    compiler, trueNamed, falseNamed, "?:"));
            }
            // A nested join arm deferred its shared-interface classes (it had no destination); a
            // concrete pointer destination here takes it as a raw pointer, so refuse it now.
            else if (destinationPointer && interfaceDestination.empty()
                     && (llvm::isa_and_nonnull<llvm::PHINode>(trueValue)
                         || llvm::isa_and_nonnull<llvm::PHINode>(falseValue)
                         || llvm::isa_and_nonnull<llvm::SelectInst>(trueValue)
                         || llvm::isa_and_nonnull<llvm::SelectInst>(falseValue)
                         || compiler->FindNullCoalesceJoin(trueValue) != nullptr
                         || compiler->FindNullCoalesceJoin(falseValue) != nullptr))
            {
                std::vector<std::string> leafClasses;
                auto collect = [&](llvm::Value* value, const LLVMBackend::TypeAndValue& named) {
                    if (llvm::isa<llvm::PHINode>(value)
                        || llvm::isa<llvm::SelectInst>(value)
                        || compiler->FindNullCoalesceJoin(value) != nullptr)
                        return CollectJoinArmClasses(value, leafClasses);
                    if (llvm::isa<llvm::ConstantPointerNull>(value)) return true;
                    std::string name = compiler->ResolveTypeAlias(named.TypeName);
                    if (!named.Pointer || named.PointerDepth > 1 || named.ElemPointer
                        || !compiler->IsDataStructure(name))
                        return false;
                    leafClasses.push_back(name);
                    return true;
                };
                if (collect(trueValue, trueNamed) && collect(falseValue, falseNamed))
                {
                    std::vector<std::string> distinct;
                    for (const auto& cls : leafClasses)
                        if (std::find(distinct.begin(), distinct.end(), cls) == distinct.end())
                            distinct.push_back(cls);
                    if (distinct.size() >= 2 && ClassesShareInterface(distinct[0], distinct[1]))
                    {
                        auto spell = [&](const std::string& cls) {
                            return SpellDiagnosticType(*compiler, LLVMBackend::TypeAndValue{
                                .TypeName = cls, .Pointer = true, .PointerDepth = 1 });
                        };
                        LogErrorContext(ctx, std::format("cannot join '{}' and '{}' in a '?:': the "
                            "pointee types differ; use an explicit cast on one arm",
                            spell(distinct[0]), spell(distinct[1])));
                    }
                }
            }
        }

        // C integer promotion: mixed-sign sub-int arms both become a signed i32 before the join,
        // so `cond ? (u8)255 : (i8)-1` joins as -1, not 255. Runs before the same-type early-out
        // because u8 and i8 arms share one LLVM i8 type.
        if (trueValue->getType()->isIntegerTy() && falseValue->getType()->isIntegerTy()
            && trueIsUnsigned != falseIsUnsigned)
        {
            unsigned tb = trueValue->getType()->getIntegerBitWidth();
            unsigned fb = falseValue->getType()->getIntegerBitWidth();
            if ((tb == 8 || tb == 16) && (fb == 8 || fb == 16))
            {
                atTrue();  trueValue  = compiler->PromoteToInt(trueValue,  trueIsUnsigned);
                atFalse(); falseValue = compiler->PromoteToInt(falseValue, falseIsUnsigned);
                trueIsUnsigned = falseIsUnsigned = false;
            }
        }
        const bool trueNullptr = (trueShape.hasSourceType
                && trueShape.sourceType.IsCxxNullptrT)
            || llvm::isa_and_nonnull<llvm::ConstantPointerNull>(trueValue);
        const bool falseNullptr = (falseShape.hasSourceType
                && falseShape.sourceType.IsCxxNullptrT)
            || llvm::isa_and_nonnull<llvm::ConstantPointerNull>(falseValue);
        if (inCxxCallArgument_ && trueNullptr && falseValue->getType()->isPointerTy())
            trueValue = llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(falseValue->getType()));
        else if (inCxxCallArgument_ && falseNullptr && trueValue->getType()->isPointerTy())
            falseValue = llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(trueValue->getType()));

        // C++ lvalue conditionals between a derived object and its base stay aliases. Validate the
        // unique base conversion with clang, then join the values and addresses as the base type.
        if (ctx->Question() != nullptr && trueValue->getType()->isStructTy()
            && falseValue->getType()->isStructTy() && trueStorage != nullptr
            && falseStorage != nullptr && !trueShape.isRvalue && !falseShape.isRvalue
            && !trueShape.isExplicitMove && !falseShape.isExplicitMove)
        {
            auto objectType = [&](const LLVMBackend::TypedValue& shape, llvm::Value* value) {
                LLVMBackend::TypeAndValue type = shape.hasSourceType
                    ? shape.sourceType : InferTernaryArmType(shape);
                if (type.TypeName.empty()) type.TypeName = shape.sourceTypeName;
                auto* record = llvm::dyn_cast<llvm::StructType>(value->getType());
                if (type.TypeName.empty() && record != nullptr && record->hasName())
                    type.TypeName = record->getName().str();
                const bool cxxReference = type.IsCxxRef()
                    || (type.Pointer && (type.IsAlias || type.IsRvalueRef)
                        && !type.IsCxxRefToPointer);
                if (type.Pointer && !cxxReference) return LLVMBackend::TypeAndValue{};
                const bool isConst = CxxTernaryArmConst(type);
                type.Pointer = false;
                type.PointerDepth = 0;
                type.ElemPointer = false;
                type.IsAlias = false;
                type.IsRvalueRef = false;
                type.IsCxxRefToPointer = false;
                type.CxxRefLevel = -1;
                type.IsCxxConstValueReturn = false;
                type.ConstMask = isConst ? 1u : 0u;
                return type;
            };
            auto trueClassType = objectType(trueShape, trueValue);
            auto falseClassType = objectType(falseShape, falseValue);
            const std::string trueClass = compiler->ResolveTypeAlias(trueClassType.TypeName);
            const std::string falseClass = compiler->ResolveTypeAlias(falseClassType.TypeName);
            if (!trueClass.empty() && !falseClass.empty() && trueClass != falseClass
                && compiler->IsCxxRecord(trueClass) && compiler->IsCxxRecord(falseClass))
            {
                const size_t trueToFalse = compiler->CxxBaseSubobjectCount(trueClass, falseClass);
                const size_t falseToTrue = compiler->CxxBaseSubobjectCount(falseClass, trueClass);
                const bool trueIsDerived = trueToFalse == 1 && falseToTrue == 0;
                const bool falseIsDerived = falseToTrue == 1 && trueToFalse == 0;
                if (trueIsDerived || falseIsDerived)
                {
                    trueClassType.TypeName = trueClass;
                    falseClassType.TypeName = falseClass;
                    const auto verdict = compiler->ClassifyCxxTernary(trueClassType,
                        LLVMBackend::CxxArmCategory::Lvalue, falseClassType,
                        LLVMBackend::CxxArmCategory::Lvalue);
                    using CxxKind = LLVMBackend::CxxTernaryVerdict::Kind;
                    const std::string& baseClass = trueIsDerived ? falseClass : trueClass;
                    if (verdict.kind == CxxKind::LvalueRef
                        && (verdict.resultIsTrue ? trueClass : falseClass) == baseClass)
                    {
                        auto lowerDerivedArm = [&](bool isTrue) {
                            auto& shape = isTrue ? trueShape : falseShape;
                            llvm::Value*& value = isTrue ? trueValue : falseValue;
                            llvm::Value*& storage = isTrue ? trueStorage : falseStorage;
                            const auto& sourceType = isTrue ? trueClassType : falseClassType;
                            const std::string& derivedClass = isTrue ? trueClass : falseClass;
                            (isTrue ? atTrue : atFalse)();
                            LLVMBackend::NamedVariable source;
                            if (const auto* named = compiler->FindVariableByStorage(storage))
                                source = *named;
                            source.Primary = value;
                            source.Storage = storage;
                            source.BaseType = value->getType();
                            source.TypeAndValue = sourceType;
                            source.TypeAndValue.TypeName = derivedClass;
                            source.TypeAndValue.IsAlias = source.TypeAndValue.IsAlias || shape.isAlias;
                            source.IsAliasBorrow = shape.isAlias;
                            source.IsRvalue = false;
                            llvm::Value* adjusted = storage;
                            if (!compiler->LowerCxxReferenceToBase(
                                    derivedClass, baseClass, source, adjusted))
                                return false;
                            storage = adjusted;
                            shape.storage = adjusted;
                            shape.receiverStorage = adjusted;
                            value = compiler->CreateLoad(compiler->GetType(
                                LLVMBackend::TypeAndValue{ .TypeName = baseClass }), adjusted);
                            shape.value = value;
                            return true;
                        };
                        if (!(trueIsDerived ? lowerDerivedArm(true) : lowerDerivedArm(false)))
                            return false;

                        LLVMBackend::TypeAndValue resultType = trueIsDerived
                            ? falseClassType : trueClassType;
                        resultType.TypeName = baseClass;
                        resultType.ConstMask = (CxxTernaryArmConst(trueClassType)
                            || CxxTernaryArmConst(falseClassType)) ? 1u : 0u;
                        auto adoptBaseType = [&](LLVMBackend::TypedValue& shape,
                                                 llvm::Value* value, llvm::Value* storage) {
                            shape.value = value;
                            shape.storage = storage;
                            shape.receiverStorage = storage;
                            shape.sourceType = resultType;
                            shape.sourceTypeName = baseClass;
                            shape.hasSourceType = true;
                            shape.isRvalue = false;
                            shape.cxxObjectCv |= CxxTernaryArmConst(resultType) ? 1 : 0;
                        };
                        adoptBaseType(trueShape, trueValue, trueStorage);
                        adoptBaseType(falseShape, falseValue, falseStorage);
                    }
                }
            }
        }

        if (trueValue->getType() == falseValue->getType())
            return true;

        auto* ft = falseValue->getType();
        auto* tt = trueValue->getType();
        auto* strTy = llvm::StructType::getTypeByName(*compiler->context, "string");
        auto isCoreUniqueStruct = [&](llvm::Type* type) -> bool {
            auto* structTy = llvm::dyn_cast<llvm::StructType>(type);
            return structTy != nullptr && structTy->hasName()
                && compiler->IsCoreUniqueType(structTy->getName().str());
        };
        if (ft->isIntegerTy() && tt->isIntegerTy())
        {
            unsigned fb = ft->getIntegerBitWidth();
            unsigned tb = tt->getIntegerBitWidth();
            // The widened arm's OWN signedness picks zext vs sext, so a u-suffixed literal
            // arm does not sign-extend into the wider arm's type.
            if (fb < tb) { atFalse(); falseValue = compiler->Upconvert(falseValue, tt, falseIsUnsigned); }
            else         { atTrue();  trueValue  = compiler->Upconvert(trueValue,  ft, trueIsUnsigned); }
        }
        else if (ft->isFloatingPointTy() && tt->isFloatingPointTy())
        {
            // Widen the narrower branch (e.g. float literal 1.0 -> double).
            unsigned fb = ft->getScalarSizeInBits();
            unsigned tb = tt->getScalarSizeInBits();
            if (fb < tb) { atFalse(); falseValue = compiler->Upconvert(falseValue, tt); }
            else         { atTrue();  trueValue  = compiler->Upconvert(trueValue,  ft); }
        }
        else if (ft->isFloatingPointTy() && tt->isIntegerTy())
        {
            // Mixed int/float (e.g. cond ? 1 : 2.5): promote the integer side.
            atTrue();
            trueValue = compiler->Upconvert(trueValue, ft);
        }
        else if (ft->isIntegerTy() && tt->isFloatingPointTy())
        {
            atFalse();
            falseValue = compiler->Upconvert(falseValue, tt);
        }
        else if (strTy && tt == strTy && ft->isPointerTy())
        {
            // string vs char* literal (e.g. cond ? prefix : "0"): wrap the raw
            // pointer into a non-owning string so both branches are `string`.
            if (RejectCodeValueTernaryStringArm(ctx, falseValue, falseOccurrence)) return false;
            atFalse();
            falseValue = compiler->WrapStringLiteralAsString(falseValue);
        }
        else if (strTy && ft == strTy && tt->isPointerTy())
        {
            if (RejectCodeValueTernaryStringArm(ctx, trueValue, trueOccurrence)) return false;
            atTrue();
            trueValue = compiler->WrapStringLiteralAsString(trueValue);
        }
        else if (auto* fatTy = compiler->GetFatPtrType();
                 (tt == fatTy && isCoreUniqueStruct(ft)) || (ft == fatTy && isCoreUniqueStruct(tt)))
        {
            // A core unique interface wrapper joins with a plain interface value by borrowing
            // its `_p` field. Keep the wrapper arm non-owning so the mixed-join receiver check runs.
            bool trueIsWrapper = isCoreUniqueStruct(tt);
            llvm::Value*& wrapperValue = trueIsWrapper ? trueValue : falseValue;
            llvm::Value*& fatValue = trueIsWrapper ? falseValue : trueValue;
            auto* wrapperTy = llvm::cast<llvm::StructType>(wrapperValue->getType());
            const auto& data = compiler->GetDataStructure(wrapperTy->getName().str());
            size_t valueIndex = data.StructFields.size();
            for (size_t i = 0; i < data.StructFields.size(); ++i)
            {
                if (data.StructFields[i].VariableName == "_p")
                {
                    valueIndex = i;
                    break;
                }
            }
            if (valueIndex == data.StructFields.size())
            {
                LogErrorContext(ctx,
                    "internal compiler error: conditional interface value is missing its stored interface field");
                return false;
            }
            (trueIsWrapper ? atTrue : atFalse)();
            wrapperValue = compiler->builder->CreateExtractValue(
                wrapperValue, {(unsigned)valueIndex}, "unique_interface_value");
            std::string interfaceName = MangledGenericArgument(
                *compiler, wrapperTy->getName().str());
            compiler->RegisterFatInterfaceValueTypeName(wrapperValue, interfaceName);
            (trueIsWrapper ? atFalse : atTrue)();
            fatValue = compiler->ReboxInterfaceIfNeeded(
                fatValue, compiler->FindFatInterfaceValueTypeName(fatValue), interfaceName);
            if (fatValue == nullptr) return false;
        }
        else if (((tt->isStructTy() && ft->isPointerTy())
                  || (ft->isStructTy() && tt->isPointerTy()))
                 && ((trueValue->getType() == tt && isCoreUniqueStruct(tt))
                     || (falseValue->getType() == ft && isCoreUniqueStruct(ft))))
        {
            // A desugared `unique T*` local is a core unique<T> value. A ternary that mixes
            // its borrow (`uniqueValue`) with a raw T* must join as T*, so lower the unique arm
            // through its library observer before the ordinary pointer join diagnostics run.
            bool trueIsUnique = trueValue->getType() == tt && isCoreUniqueStruct(tt);
            bool falseIsUnique = falseValue->getType() == ft && isCoreUniqueStruct(ft);
            if (trueIsUnique == falseIsUnique) return true;
            auto lowerUnique = [&](llvm::Value*& value, llvm::Type* type) {
                LLVMBackend::NamedVariable receiver;
                receiver.Primary = value;
                receiver.BaseType = type;
                receiver.TypeAndValue.TypeName = llvm::cast<llvm::StructType>(type)->getName().str();
                receiver.TypeAndValue.VariableName.clear();
                receiver.Storage = compiler->CreateAlloca(type);
                compiler->builder->CreateStore(value, receiver.Storage);
                const auto& data = compiler->GetDataStructure(receiver.TypeAndValue.TypeName);
                llvm::Value* wrapper = value;
                for (size_t i = 0; i < data.StructFields.size(); i++)
                {
                    if (data.StructFields[i].VariableName == "_p")
                    {
                        auto* field = compiler->CreateStructGEP(type, receiver.Storage, (uint32_t)i);
                        value = compiler->CreateLoad(field);
                        compiler->RegisterValueElementTypeName(value,
                            MangledGenericArgument(
                                *compiler, llvm::cast<llvm::StructType>(type)->getName().str()));
                        // The wrapper carried the owning-temp-field provenance; the raw pointer
                        // this join actually stores has to carry it on.
                        if (compiler->IsLedgeredOwningTempUniqueField(wrapper))
                            compiler->RegisterOwningTempUniqueField(value);
                        if (compiler->IsMovedOutPtrValue(wrapper))
                            compiler->RegisterMovedOutPtrValue(value);
                        // An owned call result hands its pointer to this arm: null the wrapper and
                        // move its release onto the loaded pointer, so the arm owns per value.
                        if (compiler->FindOwnedReturnEntry(wrapper) != nullptr)
                        {
                            const bool borrowedPointerArgument = inCallArgument_
                                && declExpectedType.Pointer && !declExpectedType.IsMove
                                && !(declExpectedType.IsOwningSink
                                    && compiler->OwningSinkConsumesConcrete(declExpectedType));
                            if (!borrowedPointerArgument)
                            {
                                const std::string* ownerFn = compiler->FindOwnedReturnTemp(wrapper);
                                const std::string fnName = ownerFn != nullptr ? *ownerFn : std::string();
                                compiler->builder->CreateStore(
                                    llvm::ConstantPointerNull::get(compiler->builder->getPtrTy()), field);
                                LLVMBackend::TypeAndValue ptrType;
                                ptrType.TypeName = MangledGenericArgument(
                                    *compiler, llvm::cast<llvm::StructType>(type)->getName().str());
                                ptrType.Pointer = true;
                                compiler->PropagateOwnedReturnTemp(wrapper, value);
                                compiler->SuppressCallerRelease(wrapper);
                                compiler->RegisterOwnedReturnTemp(value, fnName, ptrType, false);
                            }
                        }
                        return;
                    }
                    if (data.StructFields[i].VariableName == "_p")
                    {
                        value = compiler->builder->CreateExtractValue(
                            value, {(unsigned)i}, "unique_interface_value");
                        compiler->RegisterFatInterfaceValueTypeName(
                            value, MangledGenericArgument(
                                *compiler, llvm::cast<llvm::StructType>(type)->getName().str()));
                        if (compiler->IsMovedOutPtrValue(wrapper))
                            compiler->RegisterMovedOutPtrValue(value);
                        return;
                    }
                }
                value = nullptr;
            };
            if (trueIsUnique) { atTrue(); lowerUnique(trueValue, tt); }
            else             { atFalse(); lowerUnique(falseValue, ft); }
            if (auto* fatTy = compiler->GetFatPtrType(); fatTy != nullptr
                && trueValue != nullptr && falseValue != nullptr)
            {
                bool trueIsFat = trueValue->getType() == fatTy;
                bool falseIsFat = falseValue->getType() == fatTy;
                if (trueIsFat != falseIsFat)
                {
                    llvm::Value*& thinValue = trueIsFat ? falseValue : trueValue;
                    (trueIsFat ? atFalse : atTrue)();
                    std::string armFailure;
                    thinValue = BoxTernaryThinArmToInterface(
                        thinValue, MangledGenericArgument(
                            *compiler, llvm::cast<llvm::StructType>(trueIsFat ? tt : ft)
                                ->getName().str()), armFailure);
                    if (thinValue == nullptr)
                        LogErrorContext(ctx, std::format(
                            "cannot convert '?:' arm to interface: {}", armFailure));
                }
            }
        }
        else if (auto* fatTy = compiler->GetFatPtrType();
                 (ft == fatTy && tt->isPointerTy()) || (tt == fatTy && ft->isPointerTy()))
        {
            // One arm already carries the interface fat struct (e.g. `move` of an
            // interface-TYPED local, which evaluates directly to {vtable,data} since the local
            // itself is already fat-typed); the other is still a thin pointer (`new T()` /
            // `nullptr`). Learn which interface from the fat arm's ledgered TypeName - the fat
            // struct itself does not say which interface it is, since every interface shares
            // this one LLVM type - then box the thin arm into the SAME interface, in its own
            // arm block, mirroring UpcastTernaryPhiToInterface's per-arm boxing.
            bool trueIsFat = (tt == fatTy);
            llvm::Value*& fatValue  = trueIsFat ? trueValue : falseValue;
            llvm::Value*& thinValue = trueIsFat ? falseValue : trueValue;
            std::string interfaceName = compiler->FindFatInterfaceValueTypeName(fatValue);
            if (interfaceName.empty())
            {
                LogErrorContext(ctx, "cannot convert '?:' arms to a common interface type: the "
                    "fat arm's interface could not be determined");
                return false;
            }
            std::string armFailure;
            (trueIsFat ? atFalse : atTrue)();
            llvm::Value* boxed = BoxTernaryThinArmToInterface(thinValue, interfaceName, armFailure);
            if (boxed == nullptr)
            {
                LogErrorContext(ctx, std::format(
                    "cannot convert '?:' arm to interface '{}': {}",
                    SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = interfaceName }), armFailure));
                return false;
            }
            thinValue = boxed;
        }
        else if (tt->isStructTy() && ft->isStructTy())
        {
            auto* trueStruct = llvm::cast<llvm::StructType>(tt);
            auto* falseStruct = llvm::cast<llvm::StructType>(ft);
            if (trueStruct->hasName() && falseStruct->hasName())
            {
                const std::string trueName = compiler->ResolveTypeAlias(trueStruct->getName().str());
                const std::string falseName = compiler->ResolveTypeAlias(falseStruct->getName().str());
                if (trueName != falseName && compiler->IsCxxRecord(trueName)
                    && compiler->IsCxxRecord(falseName))
                {
                    auto trueType = trueShape.hasSourceType
                        ? trueShape.sourceType : InferTernaryArmType(trueShape);
                    auto falseType = falseShape.hasSourceType
                        ? falseShape.sourceType : InferTernaryArmType(falseShape);
                    trueType.TypeName = trueName;
                    falseType.TypeName = falseName;
                    auto category = [](const LLVMBackend::TypedValue& shape) {
                        using CxxArmCategory = LLVMBackend::CxxArmCategory;
                        if (shape.isExplicitMove) return CxxArmCategory::Xvalue;
                        if (!shape.isRvalue) return CxxArmCategory::Lvalue;
                        if (shape.hasSourceType && shape.sourceType.IsRvalueRef)
                            return CxxArmCategory::Xvalue;
                        if (shape.hasSourceType && shape.sourceType.IsCxxRef())
                            return CxxArmCategory::Lvalue;
                        return CxxArmCategory::Prvalue;
                    };
                    const auto trueCategory = category(trueShape);
                    const auto falseCategory = category(falseShape);
                    const auto verdict = compiler->ClassifyCxxTernary(trueType,
                        trueCategory, falseType, falseCategory);
                    using CxxKind = LLVMBackend::CxxTernaryVerdict::Kind;
                    // A prefix-incompatible call refuses the join as master did (no xvalue lowering).
                    const bool prefixRefused = inCallArgument_ && cxxTernaryCallPrefixIncompatible_;
                    const bool xvalueJoin = trueCategory == LLVMBackend::CxxArmCategory::Xvalue
                        && falseCategory == LLVMBackend::CxxArmCategory::Xvalue;
                    // Only a derived-to-base join is lowered: the losing arm's class must be a
                    // base-class source of the result (D/C). A user conversion (X/C) stays refused.
                    const bool baseJoin = verdict.kind == CxxKind::XvalueRef
                        && compiler->IsCxxDerivedToBaseValue(verdict.resultIsTrue ? falseType : trueType,
                            verdict.resultIsTrue ? trueType : falseType, nullptr);
                    if (xvalueJoin && !prefixRefused && baseJoin)
                    {
                        const std::string resultName = compiler->ResolveTypeAlias(
                            verdict.resultIsTrue ? trueName : falseName);
                        auto resultType = verdict.resultIsTrue ? trueType : falseType;
                        resultType.TypeName = resultName;
                        resultType.Pointer = false;
                        resultType.PointerDepth = 0;
                        resultType.IsRvalueRef = false;
                        if (CxxTernaryArmConst(trueType) || CxxTernaryArmConst(falseType))
                            resultType.ConstMask = 1u;
                        auto lowerArm = [&](bool isTrue) {
                            LLVMBackend::TypedValue& shape = isTrue ? trueShape : falseShape;
                            llvm::Value*& value = isTrue ? trueValue : falseValue;
                            llvm::Value*& storage = isTrue ? trueStorage : falseStorage;
                            const auto& armType = isTrue ? trueType : falseType;
                            const auto armCategory = category(shape);
                            LLVMBackend::NamedVariable arm;
                            arm.Primary = value;
                            arm.Storage = storage;
                            arm.BaseType = value->getType();
                            arm.TypeAndValue = armType;
                            arm.IsRvalue = armCategory != LLVMBackend::CxxArmCategory::Lvalue;
                            (isTrue ? atTrue : atFalse)();
                            if (!compiler->LowerCxxTernaryArm(arm, trueType,
                                    trueCategory, falseType, falseCategory, isTrue,
                                    verdict, false))
                                return false;
                            value = arm.Primary;
                            storage = arm.Storage;
                            if (value == nullptr || value->getType()->isVoidTy())
                            {
                                auto* objectType = compiler->GetType(resultType);
                                if (objectType == nullptr || storage == nullptr) return false;
                                value = compiler->CreateLoad(objectType, storage);
                            }
                            shape.value = value;
                            shape.storage = storage;
                            shape.receiverStorage = storage;
                            shape.sourceType = resultType;
                            shape.sourceTypeName = resultName;
                            shape.hasSourceType = true;
                            shape.isRvalue = verdict.kind != CxxKind::LvalueRef;
                            shape.isExplicitMove = verdict.kind == CxxKind::XvalueRef;
                            shape.isAlias = false;
                            return true;
                        };
                        if (!lowerArm(true) || !lowerArm(false)) return false;
                        return true;
                    }
                    if (verdict.kind == LLVMBackend::CxxTernaryVerdict::Kind::Ambiguous)
                        LogErrorContext(ctx, std::format(
                            "ternary branches have ambiguous C++ conversions between '{}' and '{}'",
                            compiler->DisplayCxxClassName(trueName),
                            compiler->DisplayCxxClassName(falseName)));
                }
            }
            LogErrorContext(ctx, std::format(
                "ternary branches have incompatible types '{}' and '{}'",
                trueStruct->hasName() ? trueStruct->getName().str() : std::string("struct"),
                falseStruct->hasName() ? falseStruct->getName().str() : std::string("struct")));
            return false;
        }
        else
        {
            auto describe = [](llvm::Type* t) -> std::string {
                if (auto* st = llvm::dyn_cast<llvm::StructType>(t))
                    return (st->hasName() ? st->getName().str() : std::string("struct"));
                if (t->isPointerTy()) return "pointer";
                if (t->isIntegerTy()) return "i" + std::to_string(t->getIntegerBitWidth());
                if (t->isFloatTy())   return "float";
                if (t->isDoubleTy())  return "double";
                if (t->isVoidTy()) return "void";
                return "unknown";
            };
            // A literal zero or a closure beside a function-slot arm is the slot rule, not a type
            // clash (rulings 2026-10-20, 2026-10-31). Decided from the arm values, not their text.
            auto armType = [&](const LLVMBackend::TypedValue& shape) {
                return shape.hasSourceType ? shape.sourceType : InferTernaryArmType(shape);
            };
            const auto trueArm = armType(trueShape);
            const auto falseArm = armType(falseShape);
            const auto isFnSlotArm = [](const LLVMBackend::TypeAndValue& t, llvm::Value* v) {
                return t.IsFunctionPointer || llvm::isa<llvm::Function>(v);
            };
            const auto isLiteralZero = [](llvm::Value* v) {
                auto* c = llvm::dyn_cast_or_null<llvm::ConstantInt>(v);
                return c != nullptr && c->isZero();
            };
            const auto isClosureArm = [](const LLVMBackend::TypeAndValue& t) {
                return t.TypeName == "__closure_fat_ptr";
            };
            if ((isLiteralZero(falseValue) && (isFnSlotArm(trueArm, trueValue) || isClosureArm(trueArm)))
                || (isLiteralZero(trueValue) && (isFnSlotArm(falseArm, falseValue) || isClosureArm(falseArm))))
            {
                LogErrorContext(ctx,
                    "integer constant zero is not a null spelling for a function slot; use 'nullptr'");
                return false;
            }
            const auto isThinFnArm = [&](const LLVMBackend::TypeAndValue& t, llvm::Value* v) {
                return isFnSlotArm(t, v) || llvm::isa<llvm::ConstantPointerNull>(v);
            };
            if ((isClosureArm(trueArm) && isThinFnArm(falseArm, falseValue))
                || (isClosureArm(falseArm) && isThinFnArm(trueArm, trueValue)))
            {
                LogErrorContext(ctx, compiler->DescribeCapturingClosureToThin({}));
                return false;
            }
            // ctx->expression() is the true branch; ctx->conditionalExpression() the false.
            LogErrorContext(ctx, std::format(
                "ternary branches have incompatible types '{}' and '{}'",
                describe(tt), describe(ft)));
            return false;
        }
        return true;
    }

llvm::Value* MainListener::AdoptTernaryStringArm(LLVMBackend* compiler, llvm::Value* value, bool& deepCopied) {
        auto* strTy = llvm::StructType::getTypeByName(*compiler->context, "string");
        if (strTy == nullptr || value == nullptr || value->getType() != strTy) return value;

        auto* owned = compiler->EmitOwnedStringDeepCopy(value);
        if (owned == value) return value;
        deepCopied = true;
        // A temp nothing else owns: already registered, or a plain (non-`move`) string-returning
        // CALL, which the call path does not register. A local/field read is neither - its owner frees it.
        if (compiler->IsPendingOwnedStringTemp(value) || compiler->IsProducedTempValue(value))
        {
            compiler->UnregisterOwnedStringTemp(value);
            compiler->EmitOwnedStringTempFree(value);
        }
        return owned;
    }

void MainListener::FinishTernaryArm(LLVMBackend* compiler, llvm::Value*& value,
                          const LLVMBackend::OwnedTempMark& mark, bool& deepCopied,
                          llvm::BasicBlock* hoistTo, bool movedString) {
        // A moved arm: the moved-out buffer has no other owner, so the join takes it as is.
        // A copy here orphaned the source buffer (HeapAudit leak); the join registers the phi.
        auto* stringTy = llvm::StructType::getTypeByName(*compiler->context, "string");
        if (movedString && stringTy != nullptr && value != nullptr && value->getType() == stringTy)
            deepCopied = true;
        else
            value = AdoptTernaryStringArm(compiler, value, deepCopied);
        // A bare `new` arm in a call argument is ledgered too, so the join's conditional slot
        // frees it unless the callee retains the join (DropRetainedJoinArmPtrTemps).
        if (!deepCopied)
            compiler->HoistOwnedPtrTempsForAddress(mark, value, hoistTo, /*includeBareNew*/ inCallArgument_);
        compiler->FlushOwnedTempsSince(mark, deepCopied ? nullptr : value, hoistTo);
    }

llvm::Value* MainListener::CloneTernaryClosureValue(
        llvm::Value* value, antlr4::ParserRuleContext* ctx) {
        auto* compiler = Compiler(ctx);
        if (value == nullptr || value->getType() != compiler->GetClosureFatPtrType()) return value;

        LLVMBackend::NamedVariable source;
        source.Primary = value;
        source.BaseType = compiler->GetClosureFatPtrType();
        source.TypeAndValue.TypeName = "__closure_fat_ptr";
        auto* cloned = compiler->CreateOverloadedFunctionCall("copy", { source });
        if (cloned == nullptr) return value;
        if (const auto* sources = compiler->FindBondedValue(value))
            compiler->RegisterBondedValue(cloned, *sources);
        compiler->RegisterOwnedClosureTemp(cloned);
        return cloned;
    }

// Two closure signatures are the same only when every spelled and inferred component matches.
static bool SameClosureSignature(const LLVMBackend::TypeAndValue& a, const LLVMBackend::TypeAndValue& b)
{
    if (a.FuncPtrReturnTypeName != b.FuncPtrReturnTypeName
        || a.FuncPtrReturnPointer != b.FuncPtrReturnPointer
        || a.FuncPtrReturnPointerDepth != b.FuncPtrReturnPointerDepth
        || a.FuncPtrReturnOwned != b.FuncPtrReturnOwned
        || a.FuncPtrReturnAlias != b.FuncPtrReturnAlias
        || a.FuncPtrReturnResolvedKey != b.FuncPtrReturnResolvedKey
        || a.FuncPtrParams.size() != b.FuncPtrParams.size())
        return false;
    for (size_t i = 0; i < a.FuncPtrParams.size(); i++)
    {
        const auto& p = a.FuncPtrParams[i];
        const auto& q = b.FuncPtrParams[i];
        if (p.TypeName != q.TypeName || p.Pointer != q.Pointer || p.PointerDepth != q.PointerDepth
            || p.IsMove != q.IsMove || p.IsRvalueRef != q.IsRvalueRef
            || ((p.ConstMask == -1 || q.ConstMask == -1) && p.IsPointeeConst != q.IsPointeeConst)
            || p.ResolvedTypeKey != q.ResolvedTypeKey
            || p.IsOwningSink != q.IsOwningSink || p.IsConsumeInferredSink != q.IsConsumeInferredSink
            || p.IsReturnInferredSink != q.IsReturnInferredSink
            || p.IsWriteInferredSink != q.IsWriteInferredSink)
            return false;
    }
    return true;
}

/*
 * `auto b = t ? f : g;` over two function values of ONE signature keeps that signature, so the
 * declaration stays callable. Arms that disagree record nothing and the join stays refused.
 */
static void CarryCommonFunctionSignature(LLVMBackend* compiler, LLVMBackend::TypedValue& result,
                                         const LLVMBackend::TypedValue& trueShape,
                                         const LLVMBackend::TypedValue& falseShape)
{
    auto carryLenders = [&](const auto& lenders) {
        for (const auto& lender : lenders)
            if (std::find(result.cxxClosureLenders.begin(), result.cxxClosureLenders.end(), lender)
                == result.cxxClosureLenders.end())
                result.cxxClosureLenders.push_back(lender);
    };
    carryLenders(trueShape.cxxClosureLenders);
    carryLenders(falseShape.cxxClosureLenders);
    if (result.value == nullptr) return;
    auto isFunctionValue = [](const LLVMBackend::TypedValue& shape) {
        return shape.hasSourceType
            && (shape.sourceType.TypeName == "__closure_fat_ptr" || shape.sourceType.IsThinFnPtr())
            && shape.sourceType.IsFunctionPointer && !shape.sourceType.Pointer;
    };
    if (!isFunctionValue(trueShape) || !isFunctionValue(falseShape)) return;
    const bool fatClosure = trueShape.sourceType.TypeName == "__closure_fat_ptr";
    if ((fatClosure && (falseShape.sourceType.TypeName != "__closure_fat_ptr"
            || result.value->getType() != compiler->GetClosureFatPtrType()))
        || (!fatClosure && (!falseShape.sourceType.IsThinFnPtr()
            || !result.value->getType()->isPointerTy()))) return;
    const auto& t = trueShape.sourceType;
    if (!SameClosureSignature(t, falseShape.sourceType)) return;
    LLVMBackend::TypeAndValue type;
    type.TypeName = t.TypeName;
    type.IsFunctionPointer = true;
    type.FuncPtrReturnTypeName = t.FuncPtrReturnTypeName;
    type.FuncPtrReturnAliasSpelling = t.FuncPtrReturnAliasSpelling;
    type.FuncPtrReturnHasStrippedInnerDimensions = t.FuncPtrReturnHasStrippedInnerDimensions;
    type.FuncPtrReturnPointer = t.FuncPtrReturnPointer;
    type.FuncPtrReturnOwned = t.FuncPtrReturnOwned;
    type.FuncPtrReturnAlias = t.FuncPtrReturnAlias;
    type.FuncPtrReturnPointerDepth = t.FuncPtrReturnPointerDepth;
    type.FuncPtrReturnConstMask = t.FuncPtrReturnConstMask;
    type.FuncPtrReturnResolvedKey = t.FuncPtrReturnResolvedKey;
    type.FuncPtrVariadic = t.FuncPtrVariadic;
    type.FuncPtrNoPrototype = t.FuncPtrNoPrototype;
    type.FuncPtrParams = t.FuncPtrParams;
    result.sourceType = std::move(type);
    result.sourceTypeName = "__closure_fat_ptr";
    result.hasSourceType = true;
}

/*
 * A `?:` over two pointers of ONE primitive shape (same pointee, same depth) has that shape too,
 * so `s + (c ? a : b)` binds like `s + a`. Arms that disagree leave the join unrecorded; class
 * and interface joins keep their own inference (InferTernaryArmType, interface boxing).
 */
void MainListener::JoinTernaryPointerShape(LLVMBackend::TypedValue& result,
                                           const LLVMBackend::TypedValue& trueArm,
                                           const LLVMBackend::TypedValue& falseArm) {
        // C++'s composite pointer type of `const T*` and `T*` is `const T*`.
        if (result.value != nullptr && result.value->getType()->isPointerTy()
            && (trueArm.cxxPointeeConst || falseArm.cxxPointeeConst))
            result.cxxPointeeConst = true;
        if (result.value != nullptr && result.value->getType()->isPointerTy()
            && (trueArm.cxxSpelledCvrQualifier || falseArm.cxxSpelledCvrQualifier))
            result.cxxSpelledCvrQualifier = true;
        // Two integer arms naming one non-primitive type (an enum) keep it; the reader resolves it.
        if (result.value != nullptr && result.value->getType()->isIntegerTy()
            && trueArm.pointerDepth == 0 && falseArm.pointerDepth == 0
            && !trueArm.sourceTypeName.empty() && trueArm.sourceTypeName == falseArm.sourceTypeName
            && !LLVMBackend::IsPrimitiveTypeName(trueArm.sourceTypeName))
        {
            result.sourceTypeName = trueArm.sourceTypeName;
            return;
        }
        if (result.value == nullptr || !result.value->getType()->isPointerTy()) return;
        // A nullptr/default arm takes its partner's pointer shape (C++ composite pointer type);
        // a view partner is named by the view ledger instead, so it never reaches here.
        auto isNullArm = [](const LLVMBackend::TypedValue& arm) {
            return llvm::isa_and_nonnull<llvm::ConstantPointerNull>(arm.value)
                || (arm.hasSourceType && arm.sourceType.IsCxxNullptrT);
        };
        const bool trueNull = isNullArm(trueArm);
        const bool falseNull = isNullArm(falseArm);
        if (trueNull && falseNull) return;
        if (trueNull || falseNull)
        {
            const auto& shaped = trueNull ? falseArm : trueArm;
            if (shaped.isArrayView || shaped.pointerDepth < 1
                || !LLVMBackend::IsPrimitiveTypeName(shaped.sourceTypeName)
                || shaped.sourceTypeName == "void") return;
            result.pointerDepth = shaped.pointerDepth;
            result.elemPointer = shaped.elemPointer;
            result.sourceTypeName = shaped.sourceTypeName;
            return;
        }
        if (trueArm.pointerDepth < 1 || !LLVMBackend::IsPrimitiveTypeName(trueArm.sourceTypeName)
            || trueArm.sourceTypeName == "void") return;
        if (trueArm.pointerDepth != falseArm.pointerDepth
            || trueArm.elemPointer != falseArm.elemPointer
            || trueArm.sourceTypeName != falseArm.sourceTypeName) return;
        result.pointerDepth = trueArm.pointerDepth;
        result.elemPointer = trueArm.elemPointer;
        result.sourceTypeName = trueArm.sourceTypeName;
    }

/*
 * A conditional join (`?:`, `??`) over pointers to ONE class at one depth (or one such arm and a
 * null arm) has that class as its pointee, as C++'s composite pointer type. Recorded only on the
 * transient TypedValue: the element ledger and sourceTypeName stay untouched, so interface boxing
 * and ownership guards keep resolving each arm on its own. Call-argument ranking reads it for `?:`
 * only; a member receiver recovers it (see ParsePostfixExpressionInner).
 */
void MainListener::JoinTernaryClassPointee(LLVMBackend::TypedValue& result,
                                           const LLVMBackend::TypedValue& trueArm,
                                           const LLVMBackend::TypedValue& falseArm) {
        if (result.value == nullptr || !result.value->getType()->isPointerTy()) return;
        auto* compiler = compilerLLVM;
        // The arm's class pointee and pointer depth; an empty name = no class pointer.
        auto armClass = [&](const LLVMBackend::TypedValue& arm, bool& isNull, int& depth) -> std::string {
            isNull = llvm::isa_and_nonnull<llvm::ConstantPointerNull>(arm.value)
                || (arm.hasSourceType && arm.sourceType.IsCxxNullptrT);
            depth = 0;
            if (isNull || arm.value == nullptr) return {};
            // A borrowed `unique<T>` arm joins as its `T*` (the join converts it).
            if (arm.value->getType()->isStructTy() && !arm.isExplicitMove)
            {
                const auto type = InferTernaryArmType(arm);
                if (type.Pointer || !compiler->IsCoreUniqueType(type.TypeName)) return {};
                std::string name = compiler->ResolveTypeAlias(
                    MangledGenericArgument(*compiler, type.TypeName));
                if (name.empty() || compiler->IsInterfaceType(name)
                    || compiler->dataStructures.find(name) == compiler->dataStructures.end())
                    return {};
                depth = 1;
                return name;
            }
            if (!arm.value->getType()->isPointerTy()) return {};
            if (!arm.joinClassPointee.empty())
            {
                depth = arm.joinClassPointeeDepth;
                return arm.joinClassPointee;
            }
            if (arm.isArrayView) return {};
            const auto type = InferTernaryArmType(arm);
            if (!type.Pointer || type.IsArrayView || type.IsSimd || type.IsInterface
                || type.IsInterfacePointer || type.IsFunctionPointer || type.ConstArraySize != 0)
                return {};
            depth = type.ValuePointerDepth();
            if (depth < 1 || type.ElemPointer != (depth > 1)) return {};
            std::string name = compiler->ResolveTypeAlias(type.TypeName);
            if (name.empty() || LLVMBackend::IsPrimitiveTypeName(name)
                || compiler->IsInterfaceType(name)
                || (!compiler->IsCxxRecord(name)
                    && compiler->dataStructures.find(name) == compiler->dataStructures.end()))
                return {};
            return name;
        };
        bool trueNull = false;
        bool falseNull = false;
        int trueDepth = 0;
        int falseDepth = 0;
        const std::string trueClass = armClass(trueArm, trueNull, trueDepth);
        const std::string falseClass = armClass(falseArm, falseNull, falseDepth);
        if (!trueClass.empty() && ((trueClass == falseClass && trueDepth == falseDepth) || falseNull))
        {
            result.joinClassPointee = trueClass;
            result.joinClassPointeeDepth = trueDepth;
        }
        else if (!falseClass.empty() && trueNull)
        {
            result.joinClassPointee = falseClass;
            result.joinClassPointeeDepth = falseDepth;
        }
    }

// Structural: the arm is a `move` expression, possibly parenthesized (no text matching).
static bool ArmIsExplicitMove(antlr4::tree::ParseTree* node)
{
    while (node != nullptr)
    {
        if (dynamic_cast<CFlatParser::MoveExpressionContext*>(node) != nullptr) return true;
        if (node->children.size() == 3 && node->children[0]->getText() == "(")
            node = node->children[1];
        else if (node->children.size() == 1)
            node = node->children[0];
        else
            return false;
    }
    return false;
}

// The (parenthesized) right operand of a plain `=` assignment, by parse shape.
static bool IsPlainAssignmentSource(antlr4::tree::ParseTree* node)
{
    for (auto* cur = node; cur != nullptr && cur->parent != nullptr; cur = cur->parent)
    {
        auto* parent = dynamic_cast<antlr4::ParserRuleContext*>(cur->parent);
        if (parent == nullptr) return false;
        if (auto* asg = dynamic_cast<CFlatParser::AssignmentExpressionContext*>(parent);
            asg != nullptr && asg->assignmentOperator() != nullptr)
            return asg->assignmentExpression() == cur
                && asg->assignmentOperator()->getStart()->getType() == CFlatParser::Assign;
        auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(parent);
        const bool paren = primary != nullptr && primary->expression() == cur
            && parent->children.size() == 3;
        if (!paren && parent->children.size() != 1) return false;
    }
    return false;
}

// The outermost `?:` that ctx is an arm of, through parentheses and single-child rules (ctx itself
// when it is no arm). A condition operand is no arm.
static CFlatParser::ConditionalExpressionContext* OutermostArmJoin(
        CFlatParser::ConditionalExpressionContext* ctx)
{
    auto* top = ctx;
    antlr4::tree::ParseTree* from = ctx;
    for (antlr4::tree::ParseTree* p = ctx->parent; p != nullptr; from = p, p = p->parent)
    {
        auto* up = dynamic_cast<CFlatParser::ConditionalExpressionContext*>(p);
        if (up != nullptr && up->expression() != nullptr && up->conditionalExpression() != nullptr)
        {
            if (from == up->logicalOrExpression()) break;
            top = up;
            continue;
        }
        auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(p);
        const bool paren = primary != nullptr && primary->expression() == from
            && primary->children.size() == 3;
        if (p->children.size() != 1 && !paren) break;
    }
    return top;
}

// A raw owning LOCAL pointer arm reaching a `move` sink is an implicit consume of that local,
// like the bare argument. Fields, elements, borrows and explicit `move` arms keep the old path.
bool MainListener::CollectOwningLocalPointerArm(const LLVMBackend::TypedValue& shape,
        llvm::Value* value, llvm::Value* storage, llvm::BasicBlock* armBlock,
        antlr4::ParserRuleContext* armCtx, std::vector<LLVMBackend::ConditionalMoveSource>& out)
{
    if (storage == nullptr) storage = shape.receiverStorage;
    if (value == nullptr || storage == nullptr || armBlock == nullptr) return false;
    std::string sourceName = shape.sourceCallerName;
    if (sourceName.empty()) sourceName = Compiler()->FindVariableNameByStorage(storage);
    if (sourceName.empty() || !value->getType()->isPointerTy() || !shape.sourceFieldName.empty()
        || shape.sourceIsElementAccess || sourceName.find('.') != std::string::npos
        || shape.isExplicitMove || ArmIsExplicitMove(armCtx))
        return false;
    const auto* source = Compiler()->FindVariableByStorage(storage);
    if (source == nullptr || !source->TypeAndValue.Pointer || !Compiler()->IsVariableOwning(sourceName)
        || source->IsBorrowed || source->IsAliasBorrow || source->TypeAndValue.IsAlias)
        return false;
    out.push_back({ storage, value->getType(), armBlock, sourceName, std::string(), false, true });
    out.back().MoveParamSinkArgument = inCallArgument_;
    out.back().IsExplicitMove = shape.isExplicitMove;
    return true;
}

// A call-argument arm naming the active foreach element itself: an implicit move sink takes it out of
// its slot like the direct `sink(p)` does, so the taken arm nulls the slot before the call.
bool MainListener::CollectRangeElementArm(const LLVMBackend::TypedValue& shape,
        llvm::Value* value, llvm::Value* storage, llvm::BasicBlock* armBlock,
        std::vector<LLVMBackend::ConditionalMoveSource>& out)
{
    if (!inCallArgument_) return false;
    if (storage == nullptr) storage = shape.receiverStorage;
    if (value == nullptr || storage == nullptr || armBlock == nullptr
        || !value->getType()->isPointerTy() || !shape.sourceFieldName.empty()
        || shape.sourceIsElementAccess || shape.isExplicitMove)
        return false;
    const auto* source = Compiler()->FindVariableByStorage(storage);
    if (source == nullptr || !source->IsRangeForBorrow || !source->TypeAndValue.Pointer)
        return false;
    const auto* range = FindActiveRangeForVariable(*source);
    if (range == nullptr || range->elementStorage != storage) return false;
    out.push_back({ storage, value->getType(), armBlock, range->variableName, std::string(), false });
    out.back().MoveParamSinkArgument = true;
    return true;
}

bool MainListener::CollectBorrowedPointerArm(const LLVMBackend::TypedValue& shape,
        llvm::Value* value, llvm::Value* storage, llvm::BasicBlock* armBlock,
        std::vector<LLVMBackend::ConditionalMoveSource>& out)
{
    if (!inCallArgument_) return false;
    if (storage == nullptr) storage = shape.receiverStorage;
    if (value == nullptr || storage == nullptr || armBlock == nullptr
        || !value->getType()->isPointerTy() || !shape.sourceFieldName.empty()
        || shape.sourceIsElementAccess)
        return false;
    std::string sourceName = shape.sourceCallerName;
    if (sourceName.empty()) sourceName = Compiler()->FindVariableNameByStorage(storage);
    if (sourceName.empty()) return false;
    const auto* source = Compiler()->FindVariableByStorage(storage);
    if (source == nullptr || !source->TypeAndValue.Pointer
        || source->BorrowedThroughField || Compiler()->BorrowProofRetiredByRebind(*source))
        return false;
    const bool directBorrowParam = Compiler()->IsFunctionParameter(sourceName)
        && !Compiler()->IsVariableOwning(sourceName);
    const bool copiedBorrow = !source->BorrowedOrigin.empty()
        && Compiler()->IsFunctionParameter(source->BorrowedOrigin);
    // A plain copy of a live OWNING local (`P* view = n;`) still frees what the owner frees.
    const bool copiedOwningLocal = !directBorrowParam && !copiedBorrow
        && source->BorrowsOwningLocal && !source->BorrowedThroughField
        && Compiler()->OwningLocalCopyStillAliases(*source);
    if (Compiler()->IsVariableOwning(sourceName)) return false;
    if (copiedOwningLocal)
    {
        out.push_back({ storage, value->getType(), armBlock, sourceName, std::string(), false });
        auto& recorded = out.back();
        recorded.MoveParamSinkArgument = true;
        recorded.BorrowedPointer = true;
        recorded.BorrowedOrigin = source->OwningLocalOrigin;
        recorded.BorrowedFromOwningLocal = true;
        recorded.IsExplicitMove = shape.isExplicitMove;
        return true;
    }
    if (!(source->IsBorrowed || source->IsAliasBorrow || source->TypeAndValue.IsAlias
          || directBorrowParam)
        || (!directBorrowParam && !copiedBorrow))
        return false;
    out.push_back({ storage, value->getType(), armBlock, sourceName, std::string(), false });
    auto& recorded = out.back();
    recorded.MoveParamSinkArgument = true;
    recorded.BorrowedPointer = true;
    recorded.BorrowedOrigin = directBorrowParam ? sourceName : source->BorrowedOrigin;
    recorded.IsExplicitMove = shape.isExplicitMove;
    return true;
}

/*
 * `c ? a : b` over two function values. An OVERLOADED name arm takes the one overload that binds
 * the other arm's signature (zero or several is refused). Two arms whose signatures provably differ
 * are refused here, once, so the declaration, assignment, argument, return and C-typedef consumers
 * all see the same error and the join never carries a signature. Arms with no known signature are
 * left alone.
 */
void MainListener::UnifyTernaryFunctionArms(antlr4::ParserRuleContext* ctx,
        antlr4::ParserRuleContext* trueArm, antlr4::ParserRuleContext* falseArm,
        llvm::Value*& trueValue, llvm::Value*& falseValue,
        LLVMBackend::TypedValue& trueShape, LLVMBackend::TypedValue& falseShape)
{
        auto* compiler = Compiler(ctx);
        auto armSignature = [&](antlr4::ParserRuleContext* arm, const LLVMBackend::TypedValue& shape,
                                LLVMBackend::TypeAndValue& out) {
            if (shape.hasSourceType && shape.sourceType.IsFunctionPointer
                && !shape.sourceType.FuncPtrReturnTypeName.empty())
            {
                out = shape.sourceType;
                return true;
            }
            return ResolveBareFunctionArmSignature(arm, out);
        };
        auto findOverloads = [&](const std::string& name) {
            const std::string qualified = compiler->ResolveQualifiedName(name);
            auto overloads = compiler->functionTable.find(qualified);
            if (overloads == compiler->functionTable.end() && qualified != name)
                overloads = compiler->functionTable.find(name);
            return overloads;
        };
        auto overloadedIdentifier = [&](antlr4::ParserRuleContext* arm)
                -> antlr4::tree::TerminalNode* {
            auto* identifier = BareGenericFunctionValueIdentifierInExpression(arm);
            if (identifier == nullptr) return nullptr;
            const std::string name = identifier->getText();
            if (LexicalLocalTypeShadowsValue(compiler, identifier, name)
                || NamesVisibleVariable(compiler, name)) return nullptr;
            auto overloads = findOverloads(name);
            return overloads != compiler->functionTable.end() && overloads->second.size() > 1
                ? identifier : nullptr;
        };
        // The overload of `name` whose signature is `sig`: the unique match, else refused.
        auto pickOverload = [&](antlr4::ParserRuleContext* arm,
                                antlr4::tree::TerminalNode* identifier,
                                const LLVMBackend::TypeAndValue& sig) -> const LLVMBackend::FunctionSymbol* {
            const std::string name = identifier->getText();
            auto overloads = findOverloads(name);
            if (overloads == compiler->functionTable.end()) return nullptr;
            std::vector<const LLVMBackend::FunctionSymbol*> distinct;
            for (const auto& sym : overloads->second)
            {
                if (sym.IsMethod || sym.Variadic != sig.FuncPtrVariadic
                    || compiler->FuncPtrSignaturesProvablyDiffer(compiler->FuncPtrSigOfSymbol(sym), sig))
                    continue;
                // A C import and its hand-written CFlat declaration can register one symbol twice.
                const bool duplicate = std::any_of(distinct.begin(), distinct.end(), [&](const auto* prior) {
                    return (sym.Function != nullptr && sym.Function == prior->Function)
                        || (sym.Function == nullptr && prior->Function == nullptr
                            && sym.UniqueName == prior->UniqueName);
                });
                if (!duplicate) distinct.push_back(&sym);
            }
            if (distinct.size() == 1) return distinct.front();
            const size_t matches = distinct.size();
            LogErrorContext(arm, matches == 0
                ? std::format("no overload of '{}' has the signature '{}' of the other arm of the conditional expression",
                    name, compiler->FuncPtrSpellingOf(sig))
                : std::format("more than one overload of '{}' has the signature '{}' of the other arm of the conditional expression",
                    name, compiler->FuncPtrSpellingOf(sig)));
            return nullptr;
        };
        auto* trueOverloadedIdentifier = overloadedIdentifier(trueArm);
        auto* falseOverloadedIdentifier = overloadedIdentifier(falseArm);
        const bool trueOverloaded = trueOverloadedIdentifier != nullptr;
        const bool falseOverloaded = falseOverloadedIdentifier != nullptr;
        LLVMBackend::TypeAndValue trueSig;
        LLVMBackend::TypeAndValue falseSig;
        bool haveTrue = !trueOverloaded && armSignature(trueArm, trueShape, trueSig);
        bool haveFalse = !falseOverloaded && armSignature(falseArm, falseShape, falseSig);
        auto bindArm = [&](const LLVMBackend::FunctionSymbol& sym, llvm::Value*& value,
                           LLVMBackend::TypedValue& shape, LLVMBackend::TypeAndValue& sig) {
            sig = compiler->FuncPtrSigOfSymbol(sym);
            if (sym.Function != nullptr) value = sym.Function;
            shape.sourceType = sig;
            shape.sourceTypeName = sig.TypeName;
            shape.hasSourceType = true;
        };
        if (trueOverloaded && haveFalse)
        {
            if (const auto* sym = pickOverload(trueArm, trueOverloadedIdentifier, falseSig); sym != nullptr)
                {
                    bindArm(*sym, trueValue, trueShape, trueSig);
                    haveTrue = true;
                }
        }
        if (falseOverloaded && haveTrue)
        {
            if (const auto* sym = pickOverload(falseArm, falseOverloadedIdentifier, trueSig); sym != nullptr)
                {
                    bindArm(*sym, falseValue, falseShape, falseSig);
                    haveFalse = true;
                }
        }
        if (!haveTrue || !haveFalse) return;
        auto rejectExternCountArm = [&](llvm::Value*& value,
                const LLVMBackend::TypeAndValue& sig) {
            auto* fn = llvm::dyn_cast_or_null<llvm::Function>(value);
            if (fn == nullptr) return false;
            const auto* sym = compiler->FindSymbolForFunction(fn);
            if (sym == nullptr || !compiler->RejectExternMoveCountFuncPtrBind(
                    sym->UniqueName, *sym, &sig))
                return false;
            value = nullptr;
            return true;
        };
        if (rejectExternCountArm(trueValue, trueSig)
            || rejectExternCountArm(falseValue, falseSig))
            return;
        if (compiler->FuncPtrSignaturesProvablyDiffer(trueSig, falseSig)
            || trueSig.FuncPtrVariadic != falseSig.FuncPtrVariadic)
        {
            LogErrorContext(ctx, std::format(
                "the arms of the conditional expression have different signatures '{}' and '{}'",
                compiler->FuncPtrSpellingOf(trueSig), compiler->FuncPtrSpellingOf(falseSig)));
        }
}

/*
 * A named function or thin function<> arm joining a closure arm is lifted into the closure form -
 * the widen a Lambda<> slot applies to the same value - so the join is closure to closure. Only when
 * both signatures are known and agree; otherwise the join is refused as before.
 */
bool MainListener::LiftTernaryFunctionArmToClosure(antlr4::ParserRuleContext* ctx,
        llvm::Value*& trueValue, llvm::Value*& falseValue,
        LLVMBackend::TypedValue& trueShape, LLVMBackend::TypedValue& falseShape,
        llvm::Value*& trueStorage, llvm::Value*& falseStorage,
        const std::function<void()>& atTrue, const std::function<void()>& atFalse)
{
        auto* compiler = Compiler(ctx);
        auto* closureTy = compiler->GetClosureFatPtrType();
        bool lifted = false;
        auto signatureOf = [](const LLVMBackend::TypedValue& shape) -> const LLVMBackend::TypeAndValue* {
            return shape.hasSourceType && shape.sourceType.IsFunctionPointer
                && !shape.sourceType.FuncPtrReturnTypeName.empty() ? &shape.sourceType : nullptr;
        };
        auto liftArm = [&](llvm::Value*& arm, LLVMBackend::TypedValue& armShape,
                           const LLVMBackend::TypedValue& closureShape,
                           llvm::Value*& storage, llvm::Value*& otherStorage,
                           const std::function<void()>& atArm) {
            const LLVMBackend::TypeAndValue* closureSig = signatureOf(closureShape);
            if (closureSig == nullptr) return;
            LLVMBackend::TypeAndValue armSig;
            if (auto* fn = llvm::dyn_cast<llvm::Function>(arm))
            {
                const auto* symbol = compiler->FindSymbolForFunction(fn);
                if (symbol == nullptr || symbol->IsMethod) return;
                armSig = compiler->FuncPtrSigOfSymbol(*symbol);
            }
            else if (signatureOf(armShape) != nullptr && arm->getType()->isPointerTy()
                     && !arm->getType()->isStructTy())
            {
                armSig = armShape.sourceType;
            }
            else return;
            if (compiler->FuncPtrSignaturesProvablyDiffer(*closureSig, armSig)
                || closureSig->FuncPtrVariadic != armSig.FuncPtrVariadic)
                return;
            atArm();
            arm = compiler->WidenBareOrThinToClosureFat(arm, armSig);
            armShape.sourceType = *closureSig;
            armShape.sourceTypeName = closureSig->TypeName;
            armShape.hasSourceType = true;
            // The arm's storage slot has the function value's type, not the closure's: a storage
            // join would load a closure from it. The join is then a plain value join.
            storage = nullptr;
            otherStorage = nullptr;
            lifted = true;
        };
        const bool trueIsClosure = trueValue != nullptr && trueValue->getType() == closureTy;
        const bool falseIsClosure = falseValue != nullptr && falseValue->getType() == closureTy;
        if (trueIsClosure && !falseIsClosure && falseValue != nullptr)
            liftArm(falseValue, falseShape, trueShape, falseStorage, trueStorage, atFalse);
        else if (falseIsClosure && !trueIsClosure && trueValue != nullptr)
            liftArm(trueValue, trueShape, falseShape, trueStorage, falseStorage, atTrue);
        return lifted;
}

// A lambda literal that IS the whole arm, through single-child chains and plain parentheses.
// Shape probe only.
static bool IsSoleLambdaArm(antlr4::tree::ParseTree* node)
{
    while (node != nullptr)
    {
        if (dynamic_cast<CFlatParser::LambdaExpressionContext*>(node) != nullptr) return true;
        if (auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node);
            primary != nullptr && primary->expression() != nullptr && primary->children.size() == 3)
        {
            node = primary->expression();
            continue;
        }
        auto* ctx = dynamic_cast<antlr4::ParserRuleContext*>(node);
        if (ctx == nullptr || ctx->children.size() != 1) return false;
        node = ctx->children[0];
    }
    return false;
}

// The signature a lambda-literal arm is parsed against: the enclosing slot when it names a function
// type, else the closure signature of the OTHER arm when that arm is already parsed.
static LLVMBackend::TypeAndValue TernaryLambdaArmHint(const LLVMBackend::TypeAndValue& slot,
    const LLVMBackend::TypedValue* other)
{
    if (slot.IsFunctionPointer || other == nullptr || !other->hasSourceType) return slot;
    if (other->sourceType.IsFunctionPointer && !other->sourceType.Pointer) return other->sourceType;
    return slot;
}

// An arm reads a container-owned element: the predicate the declaration and assignment paths apply to a
// named source, read off the arm's own type (a nested ?: arm carries it on its TypedValue).
static bool ArmBorrowsOwnedElement(LLVMBackend* compiler, const LLVMBackend::TypedValue& shape)
{
    if (shape.borrowsOwnedElement) return true;
    if (!shape.hasSourceType) return false;
    const auto& type = shape.sourceType;
    if (type.IsBorrowOfAliasElement) return true;
    return type.IsAlias && !type.IsMove && compiler->IsCoreUniqueType(type.TypeName);
}

// A `?:` / `??` join of two operands borrows an element when either operand does; the container
// is named by that operand (the first one when both do). The other operand may be a fresh `new`.
static void JoinElementBorrow(LLVMBackend* compiler, LLVMBackend::TypedValue& result,
    const LLVMBackend::TypedValue& first, const LLVMBackend::TypedValue& second)
{
    const bool firstElem = ArmBorrowsOwnedElement(compiler, first);
    if (!firstElem && !ArmBorrowsOwnedElement(compiler, second)) return;
    const auto& elem = firstElem ? first : second;
    result.borrowsOwnedElement = true;
    result.borrowedElementExternallyOwned = elem.borrowedElementExternallyOwned
        || (elem.hasSourceType && elem.sourceType.IsBorrowOfAliasElement);
    result.ownedElementContainer = elem.borrowsOwnedElement ? elem.ownedElementContainer
        : !elem.sourceType.ParentVariableName.empty() ? elem.sourceType.ParentVariableName
        : elem.sourceCallerName;
}

// Const-ness of the object a move leaf moves (clang's cv of the final operand). Typed unevaluated,
// so the operand's own resolved type decides: a `->` takes the pointee, a `.` inherits the object's
// const and adds the member's, so a mutable pointer to a const N stays mutable, a const member
// under a mutable root is const, and implicit-this and global roots resolve the same way.
bool MainListener::MoveOperandConst(CFlatParser::MoveExpressionContext* mv) {
    if (mv == nullptr || mv->unaryExpression() == nullptr) return false;
    auto operand = EvaluateOperandTypeOnly([&]() {
        return ParseUnaryExpression(mv->unaryExpression(), ResultUse::Value);
    });
    if (!operand.HasValue) return false;
    const auto& type = operand.Type;
    return type.Pointer ? type.PointeeConst() : CxxTernaryArmConst(type);
}

// A member leaf that is not moved (`mkH(1).cd`) is const when its resolved type is: the join
// then copies from it (clang's composite is const). Typed unevaluated; a non-member leaf is no.
bool MainListener::MemberLeafConst(antlr4::tree::ParseTree* arm, bool anyLvalueLeaf) {
    bool memberAccess = false;
    std::function<void(antlr4::tree::ParseTree*)> findMemberAccess =
        [&](antlr4::tree::ParseTree* node) {
        if (auto* postfix = dynamic_cast<CFlatParser::PostfixExpressionContext*>(node);
            postfix != nullptr && !postfix->memberNameToken().empty())
            memberAccess = true;
        for (auto* child : node->children)
            if (!memberAccess) findMemberAccess(child);
    };
    findMemberAccess(arm);
    if (!memberAccess && anyLvalueLeaf)
    {
        // A named or dereferenced leaf, through parentheses; 'default', literals and calls carry no
        // const of their own.
        antlr4::tree::ParseTree* node = arm;
        while (node != nullptr)
        {
            auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node);
            if (primary != nullptr && primary->expression() != nullptr
                && primary->children.size() == 3)
            {
                node = primary->expression();
                continue;
            }
            if (primary != nullptr)
            {
                memberAccess = primary->genericIdentifier() != nullptr;
                break;
            }
            if (auto* unaryNode = dynamic_cast<CFlatParser::UnaryExpressionContext*>(node);
                unaryNode != nullptr && unaryNode->unaryOperator() != nullptr)
            {
                memberAccess = unaryNode->unaryOperator()->getText() == "*";
                break;
            }
            if (node->children.size() != 1) break;
            node = node->children[0];
        }
    }
    if (!memberAccess) return false;
    auto* unary = [&]() -> CFlatParser::UnaryExpressionContext* {
        auto* node = arm;
        while (node != nullptr)
        {
            if (auto* found = dynamic_cast<CFlatParser::UnaryExpressionContext*>(node))
                return found;
            if (auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node);
                primary != nullptr && primary->expression() != nullptr
                && primary->children.size() == 3)
            {
                node = primary->expression();
                continue;
            }
            if (node->children.size() != 1) return nullptr;
            node = node->children[0];
        }
        return nullptr;
    }();
    if (unary == nullptr) return false;
    auto operand = EvaluateOperandTypeOnly([&]() {
        return ParseUnaryExpression(unary, ResultUse::Value);
    });
    return operand.HasValue && CxxTernaryArmConst(operand.Type);
}

// clang's composite of a move join is const when a move leaf is const (MoveOperandConst). The
// composite then binds no move ctor / move-assign.
bool MainListener::JoinMoveLeafConst(CFlatParser::ConditionalExpressionContext* ctx,
                                     bool inspectMemberLeaves, bool inspectNamedLeaves) {
    // Climb through parentheses and single-child rules only; a condition operand is no join.
    auto* top = ctx;
    antlr4::tree::ParseTree* from = ctx;
    for (antlr4::tree::ParseTree* p = ctx->parent; p != nullptr; from = p, p = p->parent)
    {
        auto* up = dynamic_cast<CFlatParser::ConditionalExpressionContext*>(p);
        if (up != nullptr && up->expression() != nullptr && up->conditionalExpression() != nullptr)
        {
            if (from == up->logicalOrExpression()) break;
            top = up;
            continue;
        }
        const bool parens = p->children.size() == 3 && p->children[0]->getText() == "(";
        if (p->children.size() != 1 && !parens) break;
    }
    std::function<bool(antlr4::tree::ParseTree*)> leafConst =
        [&](antlr4::tree::ParseTree* arm) -> bool {
        if (auto* nested = SoleTernaryExpression(arm); nested != nullptr)
            return nested->expression() != nullptr && nested->conditionalExpression() != nullptr
                && (leafConst(nested->expression()->assignmentExpression())
                    || leafConst(nested->conditionalExpression()));
        auto* mv = TopLevelMoveExpression(arm);
        if (mv == nullptr && (inspectMemberLeaves || inspectNamedLeaves))
            return MemberLeafConst(arm, inspectNamedLeaves);
        if (mv == nullptr || mv->unaryExpression() == nullptr) return false;
        return MoveOperandConst(mv);
    };
    return top->expression() != nullptr && top->conditionalExpression() != nullptr
        && (leafConst(top->expression()->assignmentExpression())
            || leafConst(top->conditionalExpression()));
}

// The `?:` an arm is, looking through parentheses: `a ? X : (b ? Y : Z)` nests a claim on the
// declaration slot, which a plain operand (not a `?:`) must pass on rather than consume.

LLVMBackend::TypedValue MainListener::ParseTernaryBranches(
        CFlatParser::ConditionalExpressionContext* ctx,
        const LLVMBackend::TypedValue& condTv,
        CFlatParser::ExpressionContext* expressionTrueCtx,
        CFlatParser::ConditionalExpressionContext* expressionFalseCtx, ResultUse use,
        const LLVMBackend::TypeAndValue& outerExpected, llvm::Value* cxxTernaryDeclDest,
        const std::string& cxxTernaryDeclType, bool collapseLvalueArms,
        bool collapseLvalueSink) {
        // Each arm's owning-call state is snapshotted inside the body, before the other arm runs.
        int ownedCallArms = 0;
        auto joined = ParseTernaryBranchesBody(ctx, condTv, expressionTrueCtx, expressionFalseCtx,
            use, outerExpected, cxxTernaryDeclDest, cxxTernaryDeclType, collapseLvalueArms,
            collapseLvalueSink, ownedCallArms);
        joined.ownedCallArms = ownedCallArms;
        Compiler(ctx)->lastTernaryJoinValue = joined.value;
        Compiler(ctx)->lastTernaryJoinArms = ownedCallArms;
        Compiler(ctx)->lastTernaryJoinShape = joined;
        return joined;
}

LLVMBackend::TypedValue MainListener::ParseTernaryBranchesBody(
        CFlatParser::ConditionalExpressionContext* ctx,
        const LLVMBackend::TypedValue& condTv,
        CFlatParser::ExpressionContext* expressionTrueCtx,
        CFlatParser::ConditionalExpressionContext* expressionFalseCtx, ResultUse use,
        const LLVMBackend::TypeAndValue& outerExpected, llvm::Value* cxxTernaryDeclDest,
        const std::string& cxxTernaryDeclType, bool collapseLvalueArms,
        bool collapseLvalueSink, int& ownedCallArms) {
        auto* compiler = Compiler(ctx);
        const bool uniquePointerDestination =
            (outerExpected.IsUnique && outerExpected.Pointer && !outerExpected.ElemPointer
                && !outerExpected.IsArrayView)
            || (compiler->IsCoreUniqueType(outerExpected.TypeName)
                && !outerExpected.Pointer && !outerExpected.IsArrayView
                && !compiler->IsInterfaceType(MangledGenericArgument(
                    *compiler, outerExpected.TypeName)));
        auto trackGlobalUniquePointerArm = [&](llvm::Value* value, llvm::Value* storage,
                                                const LLVMBackend::TypedValue& shape) {
            if (!uniquePointerDestination || value == nullptr || storage == nullptr
                || !shape.hasSourceType || !shape.sourceType.IsUnique
                || !shape.sourceType.Pointer || shape.isExplicitMove
                || !llvm::isa<llvm::GlobalVariable>(storage))
                return;
            LLVMBackend::NamedVariable source;
            source.Primary = value;
            source.Storage = storage;
            source.BaseType = value->getType();
            source.CallerName = compiler->FindVariableNameByStorage(storage);
            source.TypeAndValue = shape.sourceType;
            source.TypeAndValue.IsMove = false;
            source.IsOwning = true;
            compiler->RegisterUniqueFieldRead(value, storage, {}, source);
        };
        auto isFunctionValueDestination = [&](const LLVMBackend::TypeAndValue& destination) {
            if (!destination.FuncPtrReturnTypeName.empty()) return true;
            if (destination.Pointer) return false;
            const auto* encoded = compiler->GetEncodedClosureType(destination.TypeName);
            return encoded != nullptr && !encoded->FuncPtrReturnTypeName.empty();
        };
        const bool functionValueDestination = isFunctionValueDestination(outerExpected);
        // A nested `?:` arm of a discarded conditional is still an operand of the outer one: it
        // keeps its value (type, category, storage) for the outer join; only the whole result is discarded.
        struct NestedLeafScope
        {
            std::vector<antlr4::misc::Interval>& leaves;
            std::vector<antlr4::ParserRuleContext*>& arms;
            size_t leafMark;
            size_t armMark;
            ~NestedLeafScope() { leaves.resize(leafMark); arms.resize(armMark); }
        } nestedLeafScope{ discardedNestedArmLeaves_, discardedNestedArms_,
                           discardedNestedArmLeaves_.size(), discardedNestedArms_.size() };
        auto nestedArmUse = [&](antlr4::tree::ParseTree* arm) {
            if (use != ResultUse::Discard || ArmTernaryThroughParens(arm) == nullptr) return use;
            auto collect = [&](auto&& self, antlr4::tree::ParseTree* node) -> void {
                if (auto* nested = ArmTernaryThroughParens(node))
                {
                    discardedNestedArms_.push_back(nested);
                    self(self, nested->expression());
                    self(self, nested->conditionalExpression());
                    return;
                }
                auto* leaf = dynamic_cast<antlr4::ParserRuleContext*>(node);
                while (leaf != nullptr)
                {
                    if (leaf->children.size() == 1)
                        leaf = dynamic_cast<antlr4::ParserRuleContext*>(leaf->children[0]);
                    else if (auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(leaf);
                             primary != nullptr && primary->expression() != nullptr
                             && primary->LeftParen() != nullptr)
                        leaf = primary->expression();
                    else
                        break;
                }
                if (leaf != nullptr) discardedNestedArmLeaves_.push_back(leaf->getSourceInterval());
            };
            collect(collect, arm);
            return ResultUse::Value;
        };
        auto parseFunctionDestinationConditionalArm = [&](
                CFlatParser::ConditionalExpressionContext* arm,
                const LLVMBackend::TypeAndValue& destination) {
            if (isFunctionValueDestination(destination))
                if (auto* identifier = BareGenericFunctionValueIdentifierInExpression(arm))
                {
                    auto named = ParseIdentifier(identifier);
                    return TypedValueOfNamedOperand(named, arm);
                }
            if (auto* move = TopLevelMoveExpression(arm))
                return ParseTernaryMoveArm(move, arm);
            auto value = ParseConditionalExpression(arm, nestedArmUse(arm));
            CarryNestedTernaryBorrow(compiler, value);
            if (isFunctionValueDestination(destination) && value.value != nullptr
                && value.value->getType()->isPointerTy()
                && compiler->IsNullFunctionSlotJoin(value.value))
                value.value = llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(value.value->getType()));
            return value;
        };
        const bool deferredNewCxxTernarySlot = ctx == newCxxTernaryElisionCandidate_
            && newCxxTernaryElisionDest_ != nullptr && !newCxxTernaryElisionType_.empty();
        const bool autoDeclTernary = std::exchange(autoDeclTernaryActive_, false)
            || deferredNewCxxTernarySlot;
        LLVMBackend::TypeAndValue pointerJoinDest;
        if (outerExpected.Pointer && compiler->IsCxxRecord(outerExpected.TypeName)
            && !inCallArgument_)
            pointerJoinDest = outerExpected;
        auto adjustCxxPointerArm = [&](llvm::Value*& value,
                                       const LLVMBackend::TypeAndValue* sourceOverride = nullptr) {
            if (value == nullptr || !pointerJoinDest.Pointer || !value->getType()->isPointerTy())
                return;
            if (compiler->builder->GetInsertBlock() == nullptr) return;
            auto source = sourceOverride != nullptr ? *sourceOverride : InferTernaryArmType(value);
            if (!source.Pointer || !compiler->IsCxxRecord(source.TypeName)) return;
            value = compiler->AdjustCxxPointerForStore(
                pointerJoinDest, source, value, "conditional expression");
        };
        const bool moveInterfaceReturn = compiler->currentFunctionReturnsOwned
            && compiler->currentFunctionReturnTV.IsInterface;
        std::string deferredPointerJoinDiagnostic;

        // A declaration of a nontrivial C++ class owns the destination slot; each ternary arm is
        // a possible producer, so the caller claims the state before parsing the condition.
        const bool cxxTernaryDecl = cxxTernaryDeclDest != nullptr
            && !cxxTernaryDeclType.empty();
        size_t cxxTernaryDeclArmsMoved = 0;
        size_t cxxTernaryDeclFreshArms = 0;  // arms built from a temporary, not copied from a place
        size_t cxxTernaryDeclScalarArms = 0;
        bool cxxTernaryDeclFailed = false;
        bool nestedArmDeclFailed = false;  // a nested arm already failed (and reported)
        bool trueCxxTernaryDeclDeferred = false;
        bool falseCxxTernaryDeclDeferred = false;
        bool trueCxxTernaryDeclBuilt = false;
        bool falseCxxTernaryDeclBuilt = false;
        if (condTv.value == nullptr)
        {
            if (cxxTernaryDecl) compiler->pendingCxxTernaryDeclFailed_ = true;
            return {};
        }

        // Value category of a derived class arm slicing into a declared base (clang's join rule).
        enum class DerivedArmCat { None, Lvalue, Xvalue, Prvalue };
        struct DerivedArmState
        {
            DerivedArmCat cat = DerivedArmCat::None;
            std::string typeName;
            bool deferred = false;
            llvm::Value* value = nullptr;
            llvm::Value* storage = nullptr;
        };
        DerivedArmState trueDerived;
        DerivedArmState falseDerived;
        bool trueBonded = false;
        bool falseBonded = false;
        std::vector<std::string> ternaryBondedSources;
        auto collectBondedSources = [&](bool bonded) {
            if (!bonded) return;
            for (const auto& source : compiler->lastCallBondedSources)
                if (std::find(ternaryBondedSources.begin(), ternaryBondedSources.end(), source)
                    == ternaryBondedSources.end())
                    ternaryBondedSources.push_back(source);
        };
        // Both arms explicit moves, parentheses included (`c ? (move a) : move b`): the shape only.
        auto armIsMove = [](antlr4::tree::ParseTree* node) {
            while (node != nullptr)
            {
                if (dynamic_cast<CFlatParser::MoveExpressionContext*>(node) != nullptr) return true;
                auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node);
                if (primary != nullptr && primary->expression() != nullptr
                    && primary->children.size() == 3)
                {
                    node = primary->expression();
                    continue;
                }
                if (node->children.size() != 1) return false;
                node = node->children[0];
            }
            return false;
        };
        const bool trueExplicitMove = expressionTrueCtx != nullptr
            && armIsMove(expressionTrueCtx->assignmentExpression());
        const bool falseExplicitMove = expressionFalseCtx != nullptr
            && armIsMove(expressionFalseCtx);
        /*
         * A moved arm whose bytes nothing consumes keeps them in its source: the operand of
         * `(c ? move a : move b).x` (read in place through the storage join) and a discarded
         * `c ? move a : move b;`. A discarded mixed join is a prvalue, so its moved bytes are
         * destroyed like the C++ temporary. The source is re-armed either way.
         */
        auto isParenMemberReceiver = [](antlr4::tree::ParseTree* node) {
            bool parens = false;
            for (; node != nullptr; node = node->parent)
            {
                auto* parent = dynamic_cast<antlr4::ParserRuleContext*>(node->parent);
                if (parent == nullptr) return false;
                if (dynamic_cast<CFlatParser::PostfixExpressionContext*>(parent) != nullptr
                    && parent->children.size() > 1)
                    return parens && parent->children[0] == node
                        && parent->children[1]->getText() == ".";
                const bool paren = parent->children.size() == 3
                    && parent->children[0]->getText() == "(" && parent->children[2]->getText() == ")";
                if (!paren && parent->children.size() != 1) return false;
                parens = parens || paren;
            }
            return false;
        };
        // A (parenthesised) `?:` whose arms all move counts as a moved arm here, recursively.
        std::function<bool(antlr4::tree::ParseTree*)> armAllMoves =
            [&](antlr4::tree::ParseTree* node) -> bool {
            while (node != nullptr)
            {
                if (dynamic_cast<CFlatParser::MoveExpressionContext*>(node) != nullptr) return true;
                if (auto* cond = dynamic_cast<CFlatParser::ConditionalExpressionContext*>(node);
                    cond != nullptr && cond->expression() != nullptr
                    && cond->conditionalExpression() != nullptr)
                    return armAllMoves(cond->expression())
                        && armAllMoves(cond->conditionalExpression());
                auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node);
                if (primary != nullptr && primary->expression() != nullptr
                    && primary->children.size() == 3)
                {
                    node = primary->expression();
                    continue;
                }
                if (node->children.size() != 1) return false;
                node = node->children[0];
            }
            return false;
        };
        const bool trueKeepMove = trueExplicitMove
            || (expressionTrueCtx != nullptr && armAllMoves(expressionTrueCtx));
        const bool falseKeepMove = falseExplicitMove
            || (expressionFalseCtx != nullptr && armAllMoves(expressionFalseCtx));
        const bool keepBothMoved = trueKeepMove && falseKeepMove;
        const bool movedReceiverArms = keepBothMoved && isParenMemberReceiver(ctx);
        // Re-arms the source behind `storage`; a nested join's sources in their own arm blocks.
        std::function<void(llvm::Value*)> rearmMovedSource = [&](llvm::Value* storage) {
            auto* join = llvm::dyn_cast<llvm::PHINode>(storage);
            if (join == nullptr)
            {
                compiler->MarkStorageNotExplicitlyMovedNull(storage);
                return;
            }
            const auto savedIP = compiler->builder->saveIP();
            for (unsigned i = 0; i < join->getNumIncomingValues(); ++i)
                if (auto* term = cflat_llvm::GetTerminatorOrNull(join->getIncomingBlock(i)))
                {
                    compiler->builder->SetInsertPoint(term);
                    rearmMovedSource(join->getIncomingValue(i));
                }
            compiler->builder->restoreIP(savedIP);
        };
        // clang's composite is const when a leaf is const (`c ? mkDCF(1).cd : D(2)`): a derived
        // arm of such a join copies, never moves. Read once, from the source shape.
        std::optional<bool> derivedJoinConstMemo;
        auto derivedJoinConst = [&]() {
            if (!derivedJoinConstMemo)
                derivedJoinConstMemo = JoinMoveLeafConst(ctx, /*inspectMemberLeaves*/ true);
            return *derivedJoinConstMemo;
        };
        // A const lvalue leaf (`k ? move a : c`, c const) makes clang's composite const too: it copies.
        std::optional<bool> lvalueJoinConstMemo;
        auto lvalueJoinConst = [&]() {
            if (!lvalueJoinConstMemo)
                lvalueJoinConstMemo = JoinMoveLeafConst(ctx, /*inspectMemberLeaves*/ true,
                    /*inspectNamedLeaves*/ true);
            return *lvalueJoinConstMemo;
        };
        // A discarded mixed join of a lowered CFlat struct or an imported class is a prvalue: clang
        // materializes the temporary from the selected arm, then destroys it at the statement end.
        // Leaf moves of an arm, through nested `?:`: bit 0 a moved leaf, bit 1 an unmoved one.
        std::function<int(antlr4::tree::ParseTree*)> armMoveLeaves =
            [&](antlr4::tree::ParseTree* node) -> int {
            if (auto* nested = ArmTernaryThroughParens(node); nested != nullptr
                && nested->expression() != nullptr && nested->conditionalExpression() != nullptr)
                return armMoveLeaves(nested->expression()) | armMoveLeaves(nested->conditionalExpression());
            return armIsMove(node) ? 1 : 2;
        };
        // Mixed moved and unmoved leaves make the discarded join a prvalue (clang's composite).
        const bool discardedPrvalueJoin = use == ResultUse::Discard && expressionTrueCtx != nullptr
            && expressionFalseCtx != nullptr
            && (armMoveLeaves(expressionTrueCtx) | armMoveLeaves(expressionFalseCtx)) == 3;
        /*
         * The i1 "selected leaf was moved" of a nested arm's storage join (incoming 0 = true arm),
         * emitted beside each join PHI. Null when a leaf is neither a move nor an lvalue: a
         * prvalue leaf owns its own temporary.
         */
        std::function<llvm::Value*(antlr4::tree::ParseTree*, llvm::Value*, const LLVMBackend::OwnedTempMark&)>
            nestedLeafMoved = [&](antlr4::tree::ParseTree* arm, llvm::Value* storage,
                                  const LLVMBackend::OwnedTempMark& armMark) -> llvm::Value* {
            if (storage == nullptr || !storage->getType()->isPointerTy()) return nullptr;
            auto* nested = ArmTernaryThroughParens(arm);
            auto* phi = llvm::dyn_cast<llvm::PHINode>(storage);
            if (nested == nullptr)
            {
                if (phi != nullptr) return nullptr;
                if (armIsMove(arm)) return compiler->builder->getTrue();
                const llvm::Value* root = storage->stripInBoundsOffsets();
                if (compiler->FindVariableByStorage(root) == nullptr
                    && compiler->IsOwnedStructTempSince(armMark, const_cast<llvm::Value*>(root)))
                    return nullptr;
                return compiler->builder->getFalse();
            }
            if (phi == nullptr || phi->getNumIncomingValues() != 2 || nested->expression() == nullptr
                || nested->conditionalExpression() == nullptr)
                return nullptr;
            auto* t = nestedLeafMoved(nested->expression(), phi->getIncomingValue(0), armMark);
            auto* f = nestedLeafMoved(nested->conditionalExpression(), phi->getIncomingValue(1), armMark);
            if (t == nullptr || f == nullptr) return nullptr;
            if (t == f) return t;
            const auto savedIP = compiler->builder->saveIP();
            compiler->builder->SetInsertPoint(phi);
            auto* moved = compiler->builder->CreatePHI(compiler->builder->getInt1Ty(), 2, "discard_leaf_moved");
            moved->addIncoming(t, phi->getIncomingBlock(0));
            moved->addIncoming(f, phi->getIncomingBlock(1));
            compiler->builder->restoreIP(savedIP);
            return moved;
        };
        auto discardLoweredArm = [&](bool explicitMove, llvm::Value* value, llvm::Value* storage,
                                     const std::string& sourceTypeName, bool armIsRvalue,
                                     const LLVMBackend::OwnedTempMark& armMark,
                                     antlr4::tree::ParseTree* armCtx) {
            if (!discardedPrvalueJoin || keepBothMoved
                || storage == nullptr || !storage->getType()->isPointerTy())
                return false;
            const bool nestedArm = llvm::isa<llvm::PHINode>(storage)
                && ArmTernaryThroughParens(armCtx) != nullptr;
            if (llvm::isa<llvm::PHINode>(storage) && !nestedArm) return false;
            // A discarded lvalue (or imported-class move) arm is not loaded: use the parsed type.
            auto* st = llvm::dyn_cast_or_null<llvm::StructType>(value != nullptr ? value->getType()
                : sourceTypeName.empty() ? nullptr
                : compiler->GetType(LLVMBackend::TypeAndValue{ .TypeName = sourceTypeName }));
            if (st == nullptr || !st->hasName()) return false;
            const std::string typeName = st->getName().str();
            // Imported only: a CFlat-declared [cpp] struct's moved arm is restored and moved like a
            // lowered struct's; its copy is deleted, so an unmoved arm keeps the path below.
            const bool foreignClass = compiler->IsForeignNontrivialCxxClass(typeName)
                && !compiler->IsCppStructName(typeName);
            const bool movedCppStruct = explicitMove && value != nullptr
                && compiler->IsCppStructName(typeName) && compiler->NeedsCxxLoweringType(typeName);
            if (!foreignClass && !movedCppStruct && (!compiler->IsLoweredCFlatOnlyStruct(typeName)
                    || (explicitMove && value == nullptr)))
                return false;
            if (nestedArm && !foreignClass) return false;
            if (nestedArm)
            {
                // A nested join selects the leaf at run time: move or copy as that leaf was written.
                auto* moved = nestedLeafMoved(armCtx, storage, armMark);
                if (moved == nullptr) return false;
                auto* temp = compiler->AllocaAtEntry(st, nullptr, "discardtemp");
                const bool joinConst = lvalueJoinConst();
                auto construct = [&](bool useMove) {
                    return compiler->EmitCxxCopyOrMoveConstruct(typeName, temp, storage,
                        useMove && !joinConst, "into a conditional expression",
                        /*displayTypeName*/ std::string(), /*constSource*/ joinConst);
                };
                bool built = false;
                if (auto* known = llvm::dyn_cast<llvm::ConstantInt>(moved))
                    built = construct(known->isOne());
                else
                {
                    auto* fn = compiler->builder->GetInsertBlock()->getParent();
                    auto* moveBB = llvm::BasicBlock::Create(*compiler->context, "discard_leaf_move", fn);
                    auto* copyBB = llvm::BasicBlock::Create(*compiler->context, "discard_leaf_copy", fn);
                    auto* doneBB = llvm::BasicBlock::Create(*compiler->context, "discard_leaf_done", fn);
                    compiler->builder->CreateCondBr(moved, moveBB, copyBB);
                    compiler->SwitchToBlock(moveBB);
                    built = construct(true);
                    compiler->CreateJump(doneBB);
                    compiler->SwitchToBlock(copyBB);
                    built = construct(false) && built;
                    compiler->CreateJump(doneBB);
                    compiler->SwitchToBlock(doneBB);
                }
                if (built) compiler->RegisterOwnedStructTemp(temp, typeName);
                return true;
            }
            const llvm::Value* root = storage->stripInBoundsOffsets();
            // An unmoved lvalue arm (variable, field, `*p`, element) is copied; a prvalue arm already
            // owns its temporary.
            if (!explicitMove && compiler->FindVariableByStorage(root) == nullptr
                && (armIsRvalue || compiler->IsProducedTempValue(value)
                    || compiler->IsOwnedStructTempSince(armMark, const_cast<llvm::Value*>(root))))
                return false;
            auto* temp = compiler->AllocaAtEntry(st, nullptr, "discardtemp");
            if (foreignClass)
            {
                // An imported class is never zeroed by `move`: construct from the source in place.
                const bool joinConst = lvalueJoinConst();
                if (compiler->EmitCxxCopyOrMoveConstruct(typeName, temp, storage,
                        /*useMove*/ explicitMove && !joinConst, "into a conditional expression",
                        /*displayTypeName*/ std::string(), /*constSource*/ joinConst))
                    compiler->RegisterOwnedStructTemp(temp, typeName);
                return true;
            }
            if (explicitMove)
            {
                compiler->builder->CreateStore(value, storage);
                if (lvalueJoinConst())
                    compiler->EmitLoweredMemberwiseCopy(typeName, st, temp, storage,
                        "into a conditional expression");
                else
                    compiler->EmitLoweredMemberwiseMove(typeName, st, temp, storage,
                        "into a conditional expression");
                compiler->MarkStorageNotExplicitlyMovedNull(storage);
            }
            else
                compiler->EmitLoweredMemberwiseCopy(typeName, st, temp, storage,
                    "into a conditional expression");
            compiler->RegisterOwnedStructTemp(temp, typeName);
            return true;
        };
        /*
         * A discarded nested all-move join's value is a PHI over its moved leaves, and its storage
         * is a spill: put each leaf load back where it was read, in its own arm block, and re-arm it.
         */
        std::function<void(llvm::Value*)> restoreMovedLeaves = [&](llvm::Value* value) {
            if (auto* join = llvm::dyn_cast<llvm::PHINode>(value))
            {
                compiler->UnregisterOwnedStringTemp(join);
                const auto savedIP = compiler->builder->saveIP();
                for (unsigned i = 0; i < join->getNumIncomingValues(); ++i)
                    if (auto* term = cflat_llvm::GetTerminatorOrNull(join->getIncomingBlock(i)))
                    {
                        compiler->builder->SetInsertPoint(term);
                        restoreMovedLeaves(join->getIncomingValue(i));
                    }
                compiler->builder->restoreIP(savedIP);
                return;
            }
            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(value))
            {
                compiler->builder->CreateStore(value, load->getPointerOperand());
                compiler->MarkStorageNotExplicitlyMovedNull(load->getPointerOperand());
                return;
            }
            // A leaf with no source slot (a moved prvalue) is dropped.
            if (auto* st = llvm::dyn_cast<llvm::StructType>(value->getType()); st != nullptr && st->hasName())
            {
                auto* temp = compiler->AllocaAtEntry(st, nullptr, "discardtemp");
                compiler->builder->CreateStore(value, temp);
                if (auto* dtor = compiler->GetOrCreateFullDestructor(st->getName().str()))
                    compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { temp });
            }
        };
        auto keepUnconsumedMovedArm = [&](bool explicitMove, llvm::Value* value, llvm::Value* storage,
                                          const LLVMBackend::TypedValue& shape,
                                          const LLVMBackend::OwnedTempMark& armMark,
                                          antlr4::tree::ParseTree* armCtx) {
            const bool discard = use == ResultUse::Discard;
            if (discardLoweredArm(explicitMove, value, storage, shape.sourceTypeName, shape.isRvalue,
                    armMark, armCtx))
                return;
            // A discarded moved string: all-move joins keep the bytes in the source (as structs do
            // below); a mixed join is a prvalue, so the moved buffer is freed here, in its arm.
            auto* stringTy = llvm::StructType::getTypeByName(*compiler->context, "string");
            if (discard && explicitMove && value != nullptr && stringTy != nullptr
                && value->getType() == stringTy)
            {
                // A nested join's phi is already a pending owned temp: the arm flush frees it.
                const bool pending = compiler->IsPendingOwnedStringTemp(value);
                if (keepBothMoved && llvm::isa<llvm::PHINode>(value))
                    restoreMovedLeaves(value);
                else if (keepBothMoved && storage != nullptr && storage->getType()->isPointerTy())
                {
                    compiler->UnregisterOwnedStringTemp(value);
                    compiler->builder->CreateStore(value, storage);
                    rearmMovedSource(storage);
                }
                else if (!pending)
                    compiler->EmitOwnedStringTempFree(value);
                return;
            }
            if (!explicitMove || (!discard && !movedReceiverArms) || value == nullptr
                || storage == nullptr || !storage->getType()->isPointerTy())
                return;
            auto* st = llvm::dyn_cast<llvm::StructType>(value->getType());
            if (st == nullptr || !st->hasName()
                || st == llvm::StructType::getTypeByName(*compiler->context, "string")
                || !compiler->IsDataStructure(st->getName().str()))
                return;
            if (discard && !keepBothMoved)
            {
                auto* movedTemp = compiler->AllocaAtEntry(st, nullptr, "discardtemp");
                compiler->builder->CreateStore(value, movedTemp);
                compiler->RegisterOwnedStructTemp(movedTemp, st->getName().str());
            }
            else if (discard && llvm::isa<llvm::PHINode>(value))
            {
                restoreMovedLeaves(value);
                return;
            }
            else
                compiler->builder->CreateStore(value, storage);
            rearmMovedSource(storage);
        };
        // Slices one derived arm into the declared base through the base's move (rvalue) or copy
        // constructor. The arm's temporaries die here, after the one construction.
        // constView: the copy binds a const source (a const object, or an xvalue the join copies).
        auto sliceDerivedArmInto = [&](llvm::Value* armValue, llvm::Value* armStorage, bool isRvalue,
                                       bool constView = false) {
            LLVMBackend::NamedVariable slice;
            slice.Primary = armValue;
            slice.Storage = armStorage;
            slice.BaseType = armValue->getType();
            slice.TypeAndValue.TypeName = llvm::cast<llvm::StructType>(armValue->getType())->getName().str();
            slice.TypeAndValue.ConstMask = constView && !isRvalue ? 1u : 0u;
            slice.IsRvalue = isRvalue;
            std::string refusal;
            if (compiler->ConvertThroughCxxConvertingCtor(slice, cxxTernaryDeclType,
                    cxxTernaryDeclDest, refusal, /*directInit*/ false, /*allowScalarNarrowing*/ true)
                != LLVMBackend::CxxConvertingCtorResult::Converted)
            {
                cxxTernaryDeclFailed = true;
                return false;
            }
            return true;
        };
        auto moveCxxTernaryArmIntoDecl = [&](llvm::Value* armValue, llvm::Value* armStorage,
                                              const LLVMBackend::OwnedTempMark& armMark,
                                              bool explicitMove,
                                              const LLVMBackend::TypeAndValue& armType,
                                              DerivedArmState& arm,
                                              bool& deferred, bool& built) {
            if (!cxxTernaryDecl) return;
            // A scalar arm (an enum value) is no producer of the class: both scalar arms join as
            // a value and the declaration converts it like any other initializer.
            auto* scalarDeclSlot = llvm::dyn_cast_or_null<llvm::AllocaInst>(cxxTernaryDeclDest);
            if (armValue != nullptr && armValue->getType()->isIntegerTy()
                && (scalarDeclSlot == nullptr
                    || armValue->getType() != scalarDeclSlot->getAllocatedType()))
            {
                ++cxxTernaryDeclScalarArms;
                return;
            }
            // So is a C++ record pointer (or null) arm: the declaration converts the joined pointer.
            if (armValue != nullptr && armValue->getType()->isPointerTy()
                && (llvm::isa<llvm::ConstantPointerNull>(armValue)
                    || (armType.Pointer && armType.ValuePointerDepth() == 1
                        && compiler->IsCxxRecord(armType.TypeName))
                    || (cxxNestedCallTernaryClaim_
                        && !compiler->IsCxxRecord(compiler->ResolveTypeAlias(armType.TypeName)))))
            {
                ++cxxTernaryDeclScalarArms;
                return;
            }
            const bool fieldOfTemp = !explicitMove && armValue != nullptr && armStorage != nullptr
                && compiler->IsFieldOfOwnedStructTemp(armStorage, armValue);
            // A derived class arm (`D(1)`, or a D lvalue or xvalue) slices into the declared base.
            // A prvalue slices now; an lvalue or xvalue waits for the other arm's category.
            if (armValue != nullptr && armStorage != nullptr)
                if (auto* armStruct = llvm::dyn_cast<llvm::StructType>(armValue->getType());
                    armStruct != nullptr && armStruct->hasName()
                    && armStruct->getName().str() != cxxTernaryDeclType)
                {
                    LLVMBackend::NamedVariable slice;
                    slice.Primary = armValue;
                    slice.Storage = armStorage;
                    slice.BaseType = armValue->getType();
                    slice.TypeAndValue.TypeName = armStruct->getName().str();
                    LLVMBackend::NamedVariable ownedProbe = slice;
                    const bool joinConst = derivedJoinConst();
                    const bool derivedMove = explicitMove && !CxxTernaryArmConst(armType)
                        && !joinConst;
                    // A field of an owned temp (`mkH(1).d`) is an xvalue, not a place (clang).
                    slice.IsRvalue = derivedMove || fieldOfTemp || compiler->IsOwnedTempValue(ownedProbe);
                    if (compiler->IsCxxDerivedToBaseValue(slice.TypeAndValue,
                            LLVMBackend::TypeAndValue{ .TypeName = cxxTernaryDeclType }, &slice))
                    {
                        arm.typeName = slice.TypeAndValue.TypeName;
                        arm.cat = explicitMove || fieldOfTemp ? DerivedArmCat::Xvalue
                            : slice.IsRvalue ? DerivedArmCat::Prvalue : DerivedArmCat::Lvalue;
                        if (arm.cat != DerivedArmCat::Prvalue)
                        {
                            arm.deferred = true;
                            arm.value = armValue;
                            arm.storage = armStorage;
                            ++cxxTernaryDeclArmsMoved;
                            return;
                        }
                        const bool sliced = sliceDerivedArmInto(armValue, armStorage,
                            /*isRvalue*/ !derivedJoinConst(), /*constView*/ derivedJoinConst());
                        compiler->FlushOwnedTempsSince(armMark, nullptr, nullptr);
                        if (!sliced) return;
                        built = true;
                        ++cxxTernaryDeclArmsMoved;
                        ++cxxTernaryDeclFreshArms;
                        return;
                    }
                }
            if (armValue != nullptr && compiler->IsForeignNontrivialCxxClass(cxxTernaryDeclType))
                if (auto* armStruct = llvm::dyn_cast<llvm::StructType>(armValue->getType());
                    armStruct != nullptr && armStruct->hasName())
                {
                    const std::string armTypeName = armStruct->getName().str();
                    if (compiler->IsCxxRecord(armTypeName)
                        && compiler->ResolveTypeAlias(armTypeName)
                            != compiler->ResolveTypeAlias(cxxTernaryDeclType))
                    {
                        deferred = true;
                        return;
                    }
                }
            // A field or element of an owned temp is a place inside that temp, not the call's
            // return temporary the arm last published (`mkHA(1).a[0]` keeps `mkHA`'s sret).
            llvm::Value* temp = fieldOfTemp ? nullptr : compiler->lastCxxRetTemp_;
            if (temp == nullptr && armStorage != nullptr)
            {
                LLVMBackend::NamedVariable tempNV;
                tempNV.Primary = armValue;
                tempNV.Storage = armStorage;
                tempNV.BaseType = armValue != nullptr ? armValue->getType() : nullptr;
                tempNV.TypeAndValue.TypeName = cxxTernaryDeclType;
                if (compiler->IsOwnedTempValue(tempNV)) temp = armStorage;
            }
            const bool sameValue = armValue != nullptr
                && (armValue == compiler->lastCxxRetValue_ || armStorage == temp);
            auto* declSlot = llvm::dyn_cast_or_null<llvm::AllocaInst>(cxxTernaryDeclDest);
            llvm::Type* declType = declSlot != nullptr ? declSlot->getAllocatedType()
                : compiler->GetType(LLVMBackend::TypeAndValue{ .TypeName = cxxTernaryDeclType });
            if (temp == nullptr && armStorage != nullptr && declType != nullptr
                && armValue != nullptr && armValue->getType() == declType)
            {
                if (compiler->IsImmovableCFlatStruct(cxxTernaryDeclType))
                {
                    cxxTernaryDeclFailed = true;
                    return;
                }
                // An explicit move zeroed its source: restore the loaded bytes, then move the C++
                // fields (the source stays a live moved-from object, as in a plain `T x = move y;`).
                const bool lowered = compiler->NeedsCxxLoweringType(cxxTernaryDeclType);
                const bool useMove = (explicitMove && !lvalueJoinConst())
                    || (fieldOfTemp && !CxxTernaryArmConst(armType));
                // Only a CFlat-declared struct's move zeroes the source; an imported class is intact.
                const bool zeroedSource = explicitMove
                    && compiler->IsDataStructure(cxxTernaryDeclType);
                if (zeroedSource || (lowered && useMove))
                    compiler->builder->CreateStore(armValue, armStorage);
                bool copied = false;
                if (lowered && useMove)
                {
                    copied = compiler->EmitLoweredMemberwiseMove(cxxTernaryDeclType, declType,
                        cxxTernaryDeclDest, armStorage, "into a ternary declaration");
                }
                else if (lowered)
                    copied = compiler->EmitLoweredMemberwiseCopy(cxxTernaryDeclType, declType,
                        cxxTernaryDeclDest, armStorage, "into a ternary declaration");
                else
                    copied = compiler->EmitCxxCopyOrMoveConstruct(
                        cxxTernaryDeclType, cxxTernaryDeclDest, armStorage,
                        /*useMove*/ useMove, "into a ternary declaration", {}, false,
                        false, useMove);
                if (!copied)
                {
                    cxxTernaryDeclFailed = true;
                    return;
                }
                // The source is left a live moved-from object, so its scope exit destroys it.
                if (zeroedSource) compiler->MarkStorageNotExplicitlyMovedNull(armStorage);
                built = true;
                ++cxxTernaryDeclArmsMoved;
                return;
            }
            auto* tempSlot = llvm::dyn_cast_or_null<llvm::AllocaInst>(temp);
            const bool sameType = tempSlot != nullptr && declType != nullptr
                && tempSlot->getAllocatedType() == declType;
            if (!sameValue || !sameType)
            {
                cxxTernaryDeclFailed = true;
                return;
            }
            if (!compiler->EmitCxxCopyOrMoveConstruct(cxxTernaryDeclType, cxxTernaryDeclDest,
                                                       temp, /*useMove*/ true,
                                                       "into a ternary declaration", {}, false,
                                                       false, true))
            {
                cxxTernaryDeclFailed = true;
                return;
            }
            // The source temporary is selected by this arm, so destroy it here. Hoisting it to
            // the join would either destroy an unselected arm or leave the moved source live.
            compiler->FlushOwnedTempsSince(armMark, nullptr, nullptr);
            built = true;
            ++cxxTernaryDeclArmsMoved;
            ++cxxTernaryDeclFreshArms;
        };

        /*
         * C++17: the selected prvalue initializes the result object. An arm whose VALUE is the
         * outermost call's sret temporary (any shape: `f()`, `(f())`, `a + b`, `f().m()`) is
         * retargeted after the fact - every use of that fresh temporary becomes `dest`, so the
         * call built there and nothing is moved or destroyed. Inner temporaries are untouched.
         */
        auto retargetCxxArmTemp = [&](llvm::Value* armValue, llvm::Value* armStorage,
                                      llvm::Value* dest, llvm::Type* destType) {
            auto* temp = llvm::dyn_cast_or_null<llvm::AllocaInst>(armStorage);
            if (temp == nullptr || dest == nullptr || temp == dest || armValue == nullptr
                || temp != compiler->lastCxxRetTemp_ || destType == nullptr
                || temp->getAllocatedType() != destType || armValue->getType() != destType)
                return false;
            compiler->UnregisterOwnedStructTemp(temp);
            temp->replaceAllUsesWith(dest);
            compiler->lastCxxRetTemp_ = nullptr;
            return true;
        };
        auto cxxTernaryDeclLlvmType = [&]() -> llvm::Type* {
            auto* declSlot = llvm::dyn_cast_or_null<llvm::AllocaInst>(cxxTernaryDeclDest);
            return declSlot != nullptr ? declSlot->getAllocatedType()
                : compiler->GetType(LLVMBackend::TypeAndValue{ .TypeName = cxxTernaryDeclType });
        };
        // A constructor-expression arm (`T(args)`) builds an owned temp inside its own arm. That
        // temp is the selected object, so the constructor writes the slot directly (C++17 elision).
        auto retargetOwnedCtorArm = [&](llvm::Value* armValue, llvm::Value* armStorage,
                                         const LLVMBackend::OwnedTempMark& armMark) {
            auto* temp = llvm::dyn_cast_or_null<llvm::AllocaInst>(armStorage);
            if (temp == nullptr || temp == cxxTernaryDeclDest || armValue == nullptr
                || temp->getAllocatedType() != cxxTernaryDeclLlvmType()
                || !compiler->IsOwnedStructTempSince(armMark, temp))
                return false;
            compiler->UnregisterOwnedStructTemp(temp);
            temp->replaceAllUsesWith(cxxTernaryDeclDest);
            // Inner temporaries of this arm die here, as on the move path; the selected one does not.
            compiler->FlushOwnedTempsSince(armMark, nullptr, nullptr);
            return true;
        };
        auto cxxTernaryArmBuiltInPlace = [&](llvm::Value* armValue, llvm::Value* armStorage,
                                              const LLVMBackend::OwnedTempMark& armMark) {
            if (!cxxTernaryDecl) return false;
            const bool alreadyInDestination = armStorage == cxxTernaryDeclDest
                && armValue != nullptr && armValue->getType() == cxxTernaryDeclLlvmType();
            auto* loweredTemp = llvm::dyn_cast_or_null<llvm::AllocaInst>(armStorage);
            const bool retargetedLoweredTemp = loweredTemp != nullptr
                && loweredTemp != cxxTernaryDeclDest
                && loweredTemp == compiler->lastLoweredRetTemp_
                && armValue == compiler->lastLoweredRetValue_
                && loweredTemp->getAllocatedType() == cxxTernaryDeclLlvmType()
                && armValue->getType() == cxxTernaryDeclLlvmType();
            if (retargetedLoweredTemp)
            {
                loweredTemp->replaceAllUsesWith(cxxTernaryDeclDest);
                compiler->lastLoweredRetTemp_ = nullptr;
                compiler->lastLoweredRetValue_ = nullptr;
            }
            if (!alreadyInDestination && !retargetedLoweredTemp
                && !retargetCxxArmTemp(armValue, armStorage, cxxTernaryDeclDest,
                    cxxTernaryDeclLlvmType())
                && !retargetOwnedCtorArm(armValue, armStorage, armMark))
                return false;
            ++cxxTernaryDeclArmsMoved;
            ++cxxTernaryDeclFreshArms;
            return true;
        };

        // Outside a declaration, a single-call false arm constructs into the true arm's C++
        // return temporary, so the join is ONE prvalue temporary a declaration can adopt.
        llvm::Value* trueCxxRetTemp = nullptr;
        llvm::Value* falseCxxRetTemp = nullptr;
        llvm::Value* sharedCxxRetTemp = nullptr;
        // Same for a classified CFlat-only result: both arms build in ONE sret slot, so the
        // join is relocated by the adopting local instead of bitwise from two slots.
        llvm::AllocaInst* trueLoweredRetTemp = nullptr;
        llvm::AllocaInst* falseLoweredRetTemp = nullptr;
        // Slot of a nested lowered '?:' join arm (its prvalue is built in place, like a call arm).
        llvm::AllocaInst* trueJoinSlot = nullptr;
        llvm::AllocaInst* falseJoinSlot = nullptr;
        auto loweredJoinSlotOf = [](llvm::Value* value) -> llvm::AllocaInst* {
            auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(value);
            if (load == nullptr || !load->hasMetadata("cflat.lowered.join")) return nullptr;
            return llvm::dyn_cast<llvm::AllocaInst>(load->getPointerOperand());
        };
        llvm::AllocaInst* sharedLoweredRetTemp = nullptr;
        std::string sharedCxxRetType;

        struct TernaryCallArgumentDepthScope
        {
            int& depth;
            bool active;
            TernaryCallArgumentDepthScope(int& d, bool enabled) : depth(d), active(enabled) {
                if (active) ++depth;
            }
            ~TernaryCallArgumentDepthScope() { if (active) --depth; }
            bool IsOutermost() const { return active && depth == 1; }
        } ternaryDepth(ternaryCallArgumentDepth_, inCallArgument_);
        // A nested `?:` arm builds the argument slot in place, as in a declaration (MIXRES).
        const bool nestedTernaryArm = ArmTernaryThroughParens(expressionTrueCtx) != nullptr
            || ArmTernaryThroughParens(expressionFalseCtx) != nullptr;
        const bool cxxByValueCallArgumentJoin = cxxTernaryDecl && inCallArgument_
            && ternaryDepth.IsOutermost()
            && (IsDefaultOnlyExpression(expressionTrueCtx)
                || IsDefaultOnlyExpression(expressionFalseCtx)
                || HasDefaultOnlyTernaryArm(expressionTrueCtx)
                || HasDefaultOnlyTernaryArm(expressionFalseCtx)
                || nestedTernaryArm)
            && IsWholeCxxCallArgumentShape(ctx, outerExpected);
        const bool cxxCallArgumentJoin = cxxTernaryDecl && inCallArgument_
            && inCxxCallArgument_ && ternaryDepth.IsOutermost()
            && (IsDefaultOnlyExpression(expressionTrueCtx)
                || IsDefaultOnlyExpression(expressionFalseCtx)
                || HasDefaultOnlyTernaryArm(expressionTrueCtx)
                || HasDefaultOnlyTernaryArm(expressionFalseCtx))
            && IsWholeCxxCallArgumentShape(ctx, outerExpected);

        auto parseTernaryDefaultArm = [&](antlr4::ParserRuleContext* arm, auto&& parse) {
            struct PendingSretScope
            {
                LLVMBackend* compiler;
                bool active;
                llvm::Value* savedDest;
                llvm::Value* savedTargetDest;
                const void* savedCallSite;
                std::string savedType;
                PendingSretScope(LLVMBackend* c, llvm::Value* dest, const std::string& typeName,
                                 bool enable)
                    : compiler(c), active(enable), savedDest(c->pendingCxxSretDest_),
                      savedTargetDest(c->pendingCxxSretTargetDest_),
                      savedCallSite(c->pendingCxxSretCallSite_),
                      savedType(c->pendingCxxSretTypeName_)
                {
                    if (!active) return;
                    compiler->pendingCxxSretDest_ = dest;
                    compiler->pendingCxxSretTypeName_ = typeName;
                }
                bool Consumed() const { return active && compiler->pendingCxxSretDest_ == nullptr; }
                ~PendingSretScope()
                {
                    if (!active) return;
                    compiler->pendingCxxSretDest_ = savedDest;
                    compiler->pendingCxxSretTargetDest_ = savedTargetDest;
                    compiler->pendingCxxSretCallSite_ = savedCallSite;
                    compiler->pendingCxxSretTypeName_ = std::move(savedType);
                }
            } pendingSret(compiler, cxxTernaryDeclDest, cxxTernaryDeclType,
                cxxTernaryDecl && (IsDefaultOnlyExpression(arm)
                        || IsSameTypeCastOfDefault(arm, cxxTernaryDeclType))
                    && compiler->ReturnsViaInPlaceSret(cxxTernaryDeclType));
            auto value = parse();
            const bool builtInPlace = pendingSret.Consumed();
            if (builtInPlace)
            {
                auto* llvmType = compiler->GetType(
                    LLVMBackend::TypeAndValue{ .TypeName = cxxTernaryDeclType });
                value.value = compiler->CreateLoad(llvmType, cxxTernaryDeclDest);
                value.value->setName("cxx.ternary.default");
                value.storage = cxxTernaryDeclDest;
                value.isRvalue = true;
                value.sourceType.TypeName = cxxTernaryDeclType;
                value.hasSourceType = true;
            }
            return std::pair{ value, builtInPlace };
        };

        auto* trueBlock   = compiler->CreateBasicBlock("ternary_true");
        auto* falseBlock  = compiler->CreateBasicBlock("ternary_false");
        auto* resumeBlock = compiler->CreateBasicBlock("ternary_resume");

        // Coerces a non-bool condition to i1; leaves the insert point in trueBlock.
        compiler->CreateConditionJump(ConvertConditionToBool(condTv), trueBlock, falseBlock);
        // The block holding the branch dominates both arms AND the resume block - the one place
        // an arm's owning struct temp can be zeroed so it can outlive the arm (FinishTernaryArm).
        auto* branchBlock = trueBlock->getSinglePredecessor();

        llvm::Value* trueValue  = nullptr;
        llvm::Value* falseValue = nullptr;
        // Each arm's pointer shape (pointee name + depth), joined onto the result when both agree.
        LLVMBackend::TypedValue trueShape, falseShape;
        llvm::Value* trueStorage = nullptr;
        llvm::Value* falseStorage = nullptr;
        llvm::UncondBrInst* trueBr  = nullptr;
        llvm::UncondBrInst* falseBr = nullptr;
        llvm::BasicBlock* trueEnd = nullptr;
        llvm::BasicBlock* falseEnd = nullptr;
        bool deferTrueDiscardCxxTemps = false;
        bool deferFalseDiscardCxxTemps = false;
        auto ternaryArmSourceType = [&](const LLVMBackend::TypedValue& shape,
                                        llvm::Value* value, llvm::Value* storage) {
            LLVMBackend::TypeAndValue type = shape.hasSourceType
                ? shape.sourceType : LLVMBackend::TypeAndValue{};
            if (type.TypeName.empty()) type.TypeName = shape.sourceTypeName;
            if (type.TypeName.empty())
                if (auto* st = llvm::dyn_cast_or_null<llvm::StructType>(
                        value != nullptr ? value->getType() : nullptr); st != nullptr && st->hasName())
                    type.TypeName = st->getName().str();
            if (type.TypeName.empty())
                if (auto* alloca = llvm::dyn_cast_or_null<llvm::AllocaInst>(storage))
                    if (auto* st = llvm::dyn_cast<llvm::StructType>(alloca->getAllocatedType());
                        st != nullptr && st->hasName())
                        type.TypeName = st->getName().str();
            return type;
        };
        auto consumeNewCxxTernaryElision = [&](LLVMBackend::TypedValue& joined) {
            if (ctx != newCxxTernaryElisionCandidate_
                || newCxxTernaryElisionDest_ == nullptr
                || newCxxTernaryElisionType_.empty() || joined.value == nullptr
                || !joined.isRvalue || joined.isAlias
                || trueValue == nullptr || falseValue == nullptr)
                return false;
            const std::string targetName = compiler->ResolveTypeAlias(newCxxTernaryElisionType_);
            if (targetName.empty() || !compiler->IsForeignNontrivialCxxClass(targetName))
                return false;
            auto* targetType = compiler->GetType(
                LLVMBackend::TypeAndValue{ .TypeName = targetName });
            if (targetType == nullptr || joined.value->getType() != targetType)
                return false;
            auto isExactArm = [&](const LLVMBackend::TypedValue& shape, llvm::Value* value,
                                  llvm::Value* storage) {
                const auto type = ternaryArmSourceType(shape, value, storage);
                return value != nullptr && value->getType() == targetType
                    && !type.Pointer && !type.ElemPointer && !type.IsCxxRef()
                    && !type.IsArrayView && !type.IsInterface && !type.IsUnique
                    && !type.TypeName.empty()
                    && compiler->ResolveTypeAlias(type.TypeName) == targetName;
            };
            if (!isExactArm(trueShape, trueValue, trueStorage)
                || !isExactArm(falseShape, falseValue, falseStorage))
                return false;
            auto* resultSlot = llvm::dyn_cast_or_null<llvm::AllocaInst>(joined.storage);
            if (resultSlot == nullptr || resultSlot == newCxxTernaryElisionDest_
                || resultSlot->getAllocatedType() != targetType
                || compiler->FindVariableByStorage(resultSlot) != nullptr)
                return false;
            const auto ownedResult = std::find_if(compiler->pendingOwnedStructTemps.begin(),
                compiler->pendingOwnedStructTemps.end(), [&](const auto& temp) {
                    return temp.Alloca == resultSlot
                        && compiler->ResolveTypeAlias(temp.TypeName) == targetName;
                });
            if (ownedResult == compiler->pendingOwnedStructTemps.end()) return false;

            resultSlot->replaceAllUsesWith(newCxxTernaryElisionDest_);
            compiler->UnregisterOwnedStructTemp(resultSlot);
            joined.storage = newCxxTernaryElisionDest_;
            joined.receiverStorage = newCxxTernaryElisionDest_;
            compiler->lastCxxRetTemp_ = newCxxTernaryElisionDest_;
            compiler->lastCxxRetValue_ = joined.value;
            compiler->pendingCxxTernaryDeclConsumed_ = true;
            compiler->pendingCxxTernaryDeclFailed_ = false;
            return true;
        };
        // Emits a deferred lvalue or xvalue derived arm at the end of its own arm, once the other arm
        // is known. Mixed categories of one derived class join as a prvalue (clang): the arm copies
        // or moves into a class temporary, and that temporary slices into the base.
        auto resolveDerivedArm = [&](DerivedArmState& arm, const DerivedArmState& other,
                                     llvm::BasicBlock*& endBlock) {
            if (!arm.deferred) return;
            arm.deferred = false;
            if (endBlock == nullptr || cflat_llvm::GetTerminatorOrNull(endBlock) != nullptr)
            {
                cxxTernaryDeclFailed = true;
                return;
            }
            auto savedIP = compiler->builder->saveIP();
            compiler->builder->SetInsertPoint(endBlock);
            const auto mark = compiler->MarkOwnedTemps();
            if (other.typeName == arm.typeName && other.cat != arm.cat)
            {
                llvm::Type* classType = compiler->GetType(
                    LLVMBackend::TypeAndValue{ .TypeName = arm.typeName });
                auto* temp = compiler->CreateAlloca(classType);
                // A const join copies the arm into its temporary (clang's const composite).
                const bool joinConst = derivedJoinConst();
                if (compiler->EmitCxxCopyOrMoveConstruct(arm.typeName, temp, arm.storage,
                        /*useMove*/ arm.cat == DerivedArmCat::Xvalue && !joinConst,
                        "into a conditional expression", /*displayTypeName*/ std::string(),
                        /*constSource*/ joinConst))
                {
                    compiler->RegisterOwnedStructTemp(temp, arm.typeName);
                    sliceDerivedArmInto(compiler->builder->CreateLoad(classType, temp), temp,
                        /*isRvalue*/ !joinConst, /*constView*/ joinConst);
                }
                else
                    cxxTernaryDeclFailed = true;
            }
            else
            {
                const bool joinConst = derivedJoinConst();
                sliceDerivedArmInto(arm.value, arm.storage,
                    /*isRvalue*/ arm.cat == DerivedArmCat::Xvalue && !joinConst,
                    /*constView*/ joinConst && arm.cat != DerivedArmCat::Lvalue);
            }
            compiler->FlushOwnedTempsSince(mark, nullptr, nullptr);
            // A constructor call may end the block (an invoke): the arm's end is its continuation.
            endBlock = compiler->builder->GetInsertBlock();
            compiler->builder->restoreIP(savedIP);
        };
        bool trueOwnedString  = false;
        bool falseOwnedString = false;
        bool trueAlias = false;
        bool falseAlias = false;
        bool trueTempField = false;
        bool falseTempField = false;
        bool trueUnsigned = false;
        bool falseUnsigned = false;
        LLVMBackend::OwnedTempMark trueMark  = compiler->MarkOwnedTemps();
        LLVMBackend::OwnedTempMark falseMark = trueMark;
        const auto ternaryPreMovedState = compiler->SaveMovedState();
        auto trueTernaryMovedState = ternaryPreMovedState;
        auto falseTernaryMovedState = ternaryPreMovedState;
        // Give each arm its OWN cast occurrence: both arms share the enclosing argument slot, so
        // without this a cast on one arm launders a bare sibling naming the same constant.
        size_t trueOcc  = compiler->CurrentCastOccurrence();
        size_t falseOcc = trueOcc;
        bool trueDefault = IsDefaultOnlyExpression(expressionTrueCtx);
        bool falseDefault = IsDefaultOnlyExpression(expressionFalseCtx);
        // A lambda-literal arm reads its return type from lambdaExpectedType, which a call or
        // assignment parsed earlier clears (`m ? mk(5) : (int a) => {...}` lost the declared slot).
        // So a literal beside a non-literal arm is parsed AFTER that arm, against its signature.
        const LLVMBackend::TypeAndValue slotLambdaType = lambdaExpectedType;
        const bool trueLambdaArm = IsSoleLambdaArm(expressionTrueCtx);
        const bool falseLambdaArm = IsSoleLambdaArm(expressionFalseCtx);
        const bool falseFirst = trueDefault || (trueLambdaArm && !falseLambdaArm && !falseDefault);
        auto expectedForDefault = [&](llvm::Value* otherValue, bool otherIsDefault) {
            auto expected = otherIsDefault ? LLVMBackend::TypeAndValue{} : InferTernaryArmType(otherValue);
            if (expected.TypeName.empty()) expected = outerExpected;
            return expected;
        };
        auto isBorrowedTernaryArm = [&](llvm::Value* storage) {
            const auto* source = storage != nullptr ? compiler->FindVariableByStorage(storage) : nullptr;
            return source != nullptr && (source->IsAliasBorrow || source->IsBorrowedOwningValue
                || source->RootIsBorrowedByValueParam || source->TypeAndValue.IsAlias);
        };
        auto isOwningRecordValue = [&](llvm::Value* value) {
            auto* structType = llvm::dyn_cast_or_null<llvm::StructType>(value != nullptr
                ? value->getType() : nullptr);
            if (structType == nullptr || !structType->hasName()) return false;
            const std::string typeName = structType->getName().str();
            return !compiler->IsCxxRecord(typeName) && compiler->IsOwningValueType(typeName);
        };
        auto isAddressableLvalue = [&](llvm::Value* value, llvm::Value* storage) {
            if (value == nullptr || storage == nullptr) return false;
            LLVMBackend::NamedVariable armNV;
            armNV.Primary = value;
            armNV.Storage = storage;
            if (auto* structType = llvm::dyn_cast<llvm::StructType>(value->getType());
                structType != nullptr && structType->hasName())
                armNV.TypeAndValue.TypeName = structType->getName().str();
            return !compiler->IsOwnedTempValue(armNV);
        };
        // Redundant primitive casts keep their operand slot in IR, but are still prvalues.
        auto isExplicitValueCastArm = [](antlr4::tree::ParseTree* node) {
            while (node != nullptr)
            {
                if (auto* cast = dynamic_cast<CFlatParser::CastExpressionContext*>(node))
                {
                    if (cast->castTypeName() != nullptr) return true;
                    if (cast->children.size() == 1)
                    {
                        node = cast->children[0];
                        continue;
                    }
                    return false;
                }
                if (auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node);
                    primary != nullptr && primary->expression() != nullptr
                    && primary->LeftParen() != nullptr)
                {
                    node = primary->expression();
                    continue;
                }
                auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(node);
                if (rule == nullptr || rule->children.size() != 1) return false;
                node = rule->children[0];
            }
            return false;
        };
        auto collapseThisLvalue = [&](llvm::Value* value, llvm::Value* storage) {
            return collapseLvalueArms || (collapseLvalueSink
                && isOwningRecordValue(value) && isAddressableLvalue(value, storage)
                && !compiler->IsTempFieldValue(value));
        };
        auto markImplicitTernaryMove = [&](llvm::Value* storage, llvm::Value* value) {
            if (!collapseThisLvalue(value, storage) || storage == nullptr || value == nullptr
                || outerExpected.IsAlias || outerExpected.IsMove
                || outerExpected.Pointer || outerExpected.TypeName.empty()
                || !compiler->IsOwningValueType(outerExpected.TypeName)
                || compiler->IsCopyableType(outerExpected.TypeName))
                return;
            const auto* source = compiler->FindVariableByStorage(storage);
            if (source != nullptr && isBorrowedTernaryArm(storage))
                return;
            const std::string sourceName = compiler->FindVariableNameByStorage(storage);
            if (!sourceName.empty()) compiler->MarkVariableMoved(sourceName);
        };
        /*
         * `T(args)` builds an owned temporary that is no call's sret slot. In an `auto`
         * declaration's arms it joins exactly like one, so publish it as the arm's C++ return
         * temporary: the lvalue-arm copy and the declaration's elision then treat it alike.
         */
        auto publishCxxCtorTemp = [&](llvm::Value* value, llvm::Value* storage) {
            if (!autoDeclTernary || cxxTernaryDecl || value == nullptr
                || !llvm::isa_and_nonnull<llvm::AllocaInst>(storage)
                || storage == compiler->lastCxxRetTemp_)
                return;
            auto* st = llvm::dyn_cast<llvm::StructType>(value->getType());
            if (st == nullptr || !st->hasName()
                || !compiler->IsForeignNontrivialCxxReturnClass(st->getName().str()))
                return;
            LLVMBackend::NamedVariable probe;
            probe.Primary = value;
            probe.Storage = storage;
            probe.TypeAndValue.TypeName = st->getName().str();
            if (!compiler->IsOwnedTempValue(probe)) return;
            compiler->lastCxxRetTemp_ = storage;
            compiler->lastCxxRetValue_ = value;
        };
        // A nested `?:` arm of a declaration claims the same slot, so its selected arm is built in
        // place too. The state it leaves: 1 = built the slot, 2 = failed, 0 = a plain value.
        auto armNestedCxxTernary = [&](bool nested) {
            if (!nested) return;
            compiler->pendingCxxTernaryDeclDest_ = cxxTernaryDeclDest;
            compiler->pendingCxxTernaryDeclTypeName_ = cxxTernaryDeclType;
            compiler->pendingCxxTernaryDeclConsumed_ = false;
            compiler->pendingCxxTernaryDeclFailed_ = false;
        };
        // A consumed nested join of one derived class is an arm of that class with the category it
        // joined to: the other arm then joins it through a temporary, as clang does.
        auto adoptNestedDerivedArm = [&](int nestedState, DerivedArmState& arm) {
            if (nestedState == 1 && !compiler->pendingCxxTernaryDeclArmType_.empty())
            {
                arm.typeName = compiler->pendingCxxTernaryDeclArmType_;
                arm.cat = static_cast<DerivedArmCat>(compiler->pendingCxxTernaryDeclArmCat_);
            }
            compiler->pendingCxxTernaryDeclArmType_.clear();
            compiler->pendingCxxTernaryDeclArmCat_ = 0;
        };
        // `p++` / `p--` arms load the old value and then store the slot (now, or queued in PlusPlus
        // until the arm's postfix flush): a prvalue, not the slot.
        bool trueSlotRewritten = false, falseSlotRewritten = false;
        auto loadSlotRewrittenAfter = [](llvm::LoadInst* load) {
            for (auto* i = load->getNextNode(); i != nullptr; i = i->getNextNode())
                if (auto* st = llvm::dyn_cast<llvm::StoreInst>(i))
                    if (st->getPointerOperand() == load->getPointerOperand()) return true;
            return false;
        };
        auto takeNestedCxxTernaryArm = [&](bool nested) -> int {
            if (!nested) return 0;
            compiler->pendingCxxTernaryDeclDest_ = nullptr;
            compiler->pendingCxxTernaryDeclTypeName_.clear();
            const bool failed = compiler->pendingCxxTernaryDeclFailed_;
            const bool consumed = compiler->pendingCxxTernaryDeclConsumed_;
            compiler->pendingCxxTernaryDeclConsumed_ = false;
            compiler->pendingCxxTernaryDeclFailed_ = false;
            return failed ? 2 : (consumed ? 1 : 0);
        };
        auto restoreNestedDeclArm = [&](int nestedState, LLVMBackend::TypedValue& shape,
                                        llvm::Value*& value, llvm::Value*& storage, bool& built) {
            if (nestedState != 1) return;
            llvm::Type* objectType = compiler->GetType(
                LLVMBackend::TypeAndValue{ .TypeName = cxxTernaryDeclType });
            if (objectType == nullptr || cxxTernaryDeclDest == nullptr)
            {
                cxxTernaryDeclFailed = true;
                return;
            }
            value = compiler->CreateLoad(objectType, cxxTernaryDeclDest);
            storage = cxxTernaryDeclDest;
            shape.value = value;
            shape.storage = storage;
            shape.receiverStorage = storage;
            shape.sourceType = LLVMBackend::TypeAndValue{ .TypeName = cxxTernaryDeclType };
            shape.sourceTypeName = cxxTernaryDeclType;
            shape.hasSourceType = true;
            shape.isRvalue = true;
            shape.isAlias = false;
            built = true;
        };
        auto normalizeOwningArmType = [&](LLVMBackend::NamedVariable& armNV) {
            auto* st = llvm::dyn_cast<llvm::StructType>(armNV.BaseType);
            if (st == nullptr || !st->hasName()) return;
            const std::string actualName = st->getName().str();
            if (compiler->IsOwningValueType(actualName)
                && !armNV.TypeAndValue.TypeName.empty()
                && compiler->ResolveTypeAlias(armNV.TypeAndValue.TypeName)
                    != compiler->ResolveTypeAlias(actualName))
            {
                armNV.TypeAndValue.TypeName = actualName;
                armNV.TypeAndValue.IsInterface = false;
                armNV.TypeAndValue.IsInterfacePointer = false;
            }
        };
        auto parseTrueArm = [&]() {
            compiler->SwitchToBlock(trueBlock);
            OwnershipControlScope armControlScope(compiler);
            BranchPostfixScope armPostfix(this);  // this arm's postfix ++/-- land in this arm only
            trueMark = compiler->MarkOwnedTemps();
            if (cxxTernaryDecl)
            {
                compiler->lastCxxRetTemp_ = nullptr;
                compiler->lastCxxRetValue_ = nullptr;
            }
            LLVMBackend::CastOccurrenceScope armScope(compiler);
            trueOcc = armScope.Id;
            std::optional<DeclExpectedTypeScope> expectedScope;
            if (trueDefault)
            {
                auto expected = expectedForDefault(falseValue, falseDefault);
                if (!expected.TypeName.empty())
                    expectedScope.emplace(&declExpectedType, expected,
                        isFunctionValueDestination(expected));
            }
            else if (functionValueDestination && use != ResultUse::Discard)
                expectedScope.emplace(&declExpectedType, outerExpected, true);
            std::optional<LambdaExpectedTypeRestoreGuard> lambdaArmScope;
            if (trueLambdaArm)
            {
                lambdaArmScope.emplace(&lambdaExpectedType);
                lambdaExpectedType = TernaryLambdaArmHint(slotLambdaType,
                    falseFirst && !falseLambdaArm ? &falseShape : nullptr);
            }
            const bool trueNested = cxxTernaryDecl && use != ResultUse::Discard
                && ArmTernaryThroughParens(expressionTrueCtx) != nullptr;
            armNestedCxxTernary(trueNested);
            if (use == ResultUse::Discard)
            {
                auto trueNV = ParseAssignmentExpressionNamed(
                    expressionTrueCtx->assignmentExpression(), nestedArmUse(expressionTrueCtx));
                trueValue = trueNV.Primary;
                trueStorage = trueNV.Storage;
                trueAlias = trueNV.TypeAndValue.IsAlias || trueNV.IsAliasBorrow;
                trueUnsigned = trueNV.TypeAndValue.IsUnsignedInteger() != -1;
                trueShape.value = trueValue;
                trueShape.sourceType = trueNV.TypeAndValue;
                trueShape.sourceTypeName = trueNV.TypeAndValue.TypeName;
                trueShape.hasSourceType = !trueNV.TypeAndValue.TypeName.empty()
                    || trueNV.TypeAndValue.IsFunctionPointer
                    || trueNV.TypeAndValue.IsCxxNullptrT;
                trueShape.isArrayView = trueNV.TypeAndValue.IsArrayView;
                trueShape.isAlias = trueAlias;
                trueShape.storage = trueStorage;
                // Same value category as TypedValueOfNamedOperand: a call result or `move x` is an rvalue.
                trueShape.isRvalue = trueNV.IsRvalue || (trueNV.IsExplicitMove
                    && compiler->IsCxxRecord(trueNV.TypeAndValue.TypeName));
                trueShape.isExplicitMove = trueNV.IsExplicitMove;
                trueShape.isDataNullCast = trueNV.IsDataNullCast;
                OperandPointerDepth(trueNV, trueValue, trueShape.pointerDepth, trueShape.elemPointer);
                if (trueShape.hasSourceType && trueShape.sourceType.Pointer
                    && trueShape.pointerDepth > 0)
                    trueShape.sourceType.PointerDepth = trueShape.pointerDepth;
                else if (trueShape.hasSourceType && trueShape.pointerDepth > 0
                    && trueValue != nullptr && trueValue->getType()->isPointerTy()
                    && !trueShape.sourceType.IsArrayView
                    && !trueShape.sourceType.IsFunctionPointer)
                {
                    trueShape.sourceType.Pointer = true;
                    trueShape.sourceType.PointerDepth = trueShape.pointerDepth;
                    trueShape.sourceType.ElemPointer = trueShape.elemPointer;
                }
                ProcessPlusPlus();
            }
            else
            {
                auto parsedTrue = parseTernaryDefaultArm(expressionTrueCtx, [&] {
                    return ParseTernaryArmExpression(expressionTrueCtx);
                });
                auto trueTv = parsedTrue.first;
                trueValue = trueTv.value;
                trueShape = trueTv;
                trueStorage = trueTv.storage;
                // A NESTED `?:` arm publishes its own storage join on receiverStorage: a PHI
                // beside the value PHI names the selected object, so seed this arm from it.
                if (trueStorage == nullptr && llvm::isa_and_nonnull<llvm::PHINode>(trueValue))
                    trueStorage = trueTv.receiverStorage;
                trueAlias = trueTv.isAlias;
                trueUnsigned = trueTv.isUnsigned;
            }
            // Owning-call snapshot of the TRUE arm, taken before the false arm can overwrite the last call.
            // A parenthesized nested ?: arm arrives with its join value but without its TypedValue bits.
            const bool trueNestedOwned = trueValue != nullptr
                && trueValue == compiler->lastTernaryJoinValue && compiler->lastTernaryJoinArms != 0;
            if (trueShape.ownedCallArms != 0 || trueNestedOwned
                || compiler->ValueIsUniqueCallResult(trueValue))
                ownedCallArms |= 1;
            // An arm value loaded from storage keeps that storage, so the join stays an lvalue.
            // A classified call result loaded from its sret slot is an rvalue, not a load.
            // The rewrite flag is read even when the arm already names storage: a postfix arm does.
            if (trueValue != nullptr && !compiler->IsProducedTempValue(trueValue))
                if (auto* load = llvm::dyn_cast<llvm::LoadInst>(trueValue))
                {
                    if (trueStorage == nullptr) trueStorage = load->getPointerOperand();
                    trueSlotRewritten = loadSlotRewrittenAfter(load)
                        || PlusPlus.count(load->getPointerOperand()) != 0;
                }
            trueAlias = trueAlias || compiler->IsAliasValue(trueValue);
            const int trueNestedState = takeNestedCxxTernaryArm(trueNested);
            restoreNestedDeclArm(trueNestedState, trueShape, trueValue,
                trueStorage, trueCxxTernaryDeclBuilt);
            if (trueNestedState == 2) cxxTernaryDeclFailed = nestedArmDeclFailed = true;
            adoptNestedDerivedArm(trueNestedState, trueDerived);
            trueTempField = compiler->IsTempFieldValue(trueValue);
            publishCxxCtorTemp(trueValue, trueStorage);
            if (trueValue != nullptr && trueValue == compiler->lastLoweredRetValue_)
                if (auto* st = llvm::dyn_cast<llvm::StructType>(trueValue->getType());
                    st != nullptr && st->hasName()
                    && compiler->NeedsCxxLoweringType(st->getName().str()))
                    trueLoweredRetTemp =
                        llvm::dyn_cast_or_null<llvm::AllocaInst>(compiler->lastLoweredRetTemp_);
            trueJoinSlot = loweredJoinSlotOf(trueValue);
            if (!cxxTernaryDecl && use != ResultUse::Discard && trueStorage != nullptr
                && trueStorage == compiler->lastCxxRetTemp_
                && llvm::isa<llvm::AllocaInst>(trueStorage) && trueValue != nullptr)
                if (auto* st = llvm::dyn_cast<llvm::StructType>(trueValue->getType());
                    st != nullptr && st->hasName()
                    && compiler->IsForeignNontrivialCxxReturnClass(st->getName().str()))
                {
                    trueCxxRetTemp = trueStorage;
                    sharedCxxRetType = st->getName().str();
                }
            if (trueNestedState == 1)
            {
                ++cxxTernaryDeclArmsMoved;
                ++cxxTernaryDeclFreshArms;
            }
            else if (trueNestedState == 0)
            {
                trueCxxTernaryDeclBuilt = cxxTernaryArmBuiltInPlace(
                    trueValue, trueStorage, trueMark);
                if (!trueCxxTernaryDeclBuilt)
                    moveCxxTernaryArmIntoDecl(trueValue, trueStorage, trueMark,
                        trueExplicitMove,
                        trueShape.hasSourceType ? trueShape.sourceType : InferTernaryArmType(trueValue),
                        trueDerived, trueCxxTernaryDeclDeferred,
                        trueCxxTernaryDeclBuilt);
            }
            if (ternaryDepth.IsOutermost() && !TernaryIsBinaryOperand(ctx)
                && trueValue != nullptr && !outerExpected.IsMove
                && !(outerExpected.IsOwningSink && compiler->OwningSinkConsumesConcrete(outerExpected)))
            {
                LLVMBackend::NamedVariable armNV;
                armNV.Primary = trueValue;
                armNV.BaseType = trueValue->getType();
                armNV.TypeAndValue = outerExpected;
                if (armNV.TypeAndValue.TypeName.empty())
                    if (auto* st = llvm::dyn_cast<llvm::StructType>(trueValue->getType()); st && st->hasName())
                        armNV.TypeAndValue.TypeName = st->getName().str();
                armNV.TypeAndValue.Pointer = false;
                armNV.TypeAndValue.IsAlias = false;
                armNV.TypeAndValue.IsMove = false;
                normalizeOwningArmType(armNV);
                // A moved plain-struct arm passed by value: its bytes left the zeroed source, so a
                // per-arm temp owns them until the call returns, as for a produced arm.
                const std::string& armTypeName = armNV.TypeAndValue.TypeName;
                if (trueExplicitMove && inCallArgument_ && !movedReceiverArms
                    && trueValue->getType()->isStructTy()
                    && trueValue->getType()
                        != llvm::StructType::getTypeByName(*compiler->context, "string")
                    && compiler->IsDataStructure(armTypeName)
                    && !compiler->IsForeignNontrivialCxxClass(armTypeName)
                    && compiler->IsOwningValueType(armTypeName)
                    && compiler->GetType(armNV.TypeAndValue) == trueValue->getType())
                {
                    auto* movedTemp = compiler->AllocaAtEntry(trueValue->getType(), nullptr, "argtemp");
                    compiler->builder->CreateStore(trueValue, movedTemp);
                    compiler->RegisterOwnedStructTemp(movedTemp, armTypeName);
                }
                else
                    compiler->RegisterBorrowedOwningStructTemp(armNV, true);
            }
            keepUnconsumedMovedArm(trueKeepMove, trueValue, trueStorage, trueShape, trueMark,
                expressionTrueCtx);
            if (use == ResultUse::Discard)
            {
                const auto type = ternaryArmSourceType(trueShape, trueValue, trueStorage);
                deferTrueDiscardCxxTemps = !type.Pointer && compiler->IsCxxRecord(type.TypeName);
                if (!deferTrueDiscardCxxTemps)
                    compiler->FlushOwnedTempsSince(trueMark, nullptr, branchBlock);
            }
            else if (!trueCxxTernaryDeclDeferred)
                FinishTernaryArm(compiler, trueValue, trueMark, trueOwnedString, branchBlock,
                    trueExplicitMove);
            armPostfix.Flush();
            trueEnd = compiler->builder->GetInsertBlock();
            trueBonded = compiler->lastCallIsBonded;
            collectBondedSources(trueBonded);
        };
        /*
         * C++: `c ? lvalue : prvalue` of one class is a prvalue - the lvalue arm COPY-constructs
         * the other arm's return temporary, which then joins as one slot a declaration adopts.
         * Only for an `auto` declaration (a typed one copies in moveCxxTernaryArmIntoDecl);
         * other sinks keep the mixed-ownership rejection.
         */
        // A reference-returning C++ call is an alias arm with real storage, so it copies too.
        // A const field of a temp is copied, not moved (clang): only a mutable one is an xvalue.
        auto isOwnedTempField = [&](llvm::Value* value, llvm::Value* storage,
                                    const LLVMBackend::TypeAndValue& armType) {
            return value != nullptr && storage != nullptr && !CxxTernaryArmConst(armType)
                && compiler->IsFieldOfOwnedStructTemp(storage, value);
        };
        auto isCopyableCxxLvalueArm = [&](llvm::Value* value, llvm::Value* storage,
                                          llvm::Type* joinType, bool explicitMove,
                                          const LLVMBackend::TypeAndValue& armType) {
            return !cxxTernaryDecl && use != ResultUse::Discard && !trueDefault
                && autoDeclTernary && !outerExpected.IsAlias && !compiler->currentFunctionReturnTV.IsAlias
                && value != nullptr && value->getType() == joinType
                && isAddressableLvalue(value, storage)
                && (explicitMove || !compiler->IsTempFieldValue(value)
                    || isOwnedTempField(value, storage, armType));
        };
        auto materializeCxxLvalueArm = [&]() {
            if (sharedCxxRetTemp != nullptr || trueValue == nullptr || falseValue == nullptr)
                return;
            if (trueCxxRetTemp != nullptr
                && isCopyableCxxLvalueArm(falseValue, falseStorage, trueValue->getType(),
                                           falseExplicitMove, falseShape.sourceType))
            {
                if (!compiler->EmitCxxCopyOrMoveConstruct(sharedCxxRetType, trueCxxRetTemp,
                        falseStorage, /*useMove*/ falseExplicitMove
                            || isOwnedTempField(falseValue, falseStorage, falseShape.sourceType),
                        "into a conditional expression"))
                    return;
                sharedCxxRetTemp = trueCxxRetTemp;
                compiler->RegisterOwnedStructTemp(sharedCxxRetTemp, sharedCxxRetType);
                falseStorage = sharedCxxRetTemp;
                falseValue = compiler->builder->CreateLoad(trueValue->getType(), sharedCxxRetTemp);
                falseAlias = false;   // now the copy in the temporary, no longer the referent
                return;
            }
            // The mirror: the FALSE arm is the temporary, so the copy goes at the true arm's end.
            auto* falseTemp = llvm::dyn_cast_or_null<llvm::AllocaInst>(falseStorage);
            auto* st = llvm::dyn_cast<llvm::StructType>(falseValue->getType());
            if (trueCxxRetTemp != nullptr || falseTemp == nullptr
                || falseStorage != compiler->lastCxxRetTemp_ || st == nullptr || !st->hasName()
                || !compiler->IsForeignNontrivialCxxReturnClass(st->getName().str())
                || !isCopyableCxxLvalueArm(trueValue, trueStorage, st, trueExplicitMove,
                                           trueShape.sourceType)
                || trueEnd == nullptr || cflat_llvm::GetTerminatorOrNull(trueEnd) != nullptr)
                return;
            const std::string typeName = st->getName().str();
            auto savedIP = compiler->builder->saveIP();
            compiler->builder->SetInsertPoint(trueEnd);
            const auto mark = compiler->MarkOwnedTemps();
            if (compiler->EmitCxxCopyOrMoveConstruct(typeName, falseTemp, trueStorage,
                    /*useMove*/ trueExplicitMove
                        || isOwnedTempField(trueValue, trueStorage, trueShape.sourceType),
                    "into a conditional expression"))
            {
                sharedCxxRetTemp = falseTemp;
                compiler->RegisterOwnedStructTemp(falseTemp, typeName);
                trueStorage = falseTemp;
                trueValue = compiler->builder->CreateLoad(st, falseTemp);
                trueAlias = false;
                compiler->FlushOwnedTempsSince(mark, nullptr, branchBlock);
                trueEnd = compiler->builder->GetInsertBlock();
            }
            compiler->builder->restoreIP(savedIP);
        };
        auto parseFalseArm = [&]() {
            compiler->SwitchToBlock(falseBlock);
            OwnershipControlScope armControlScope(compiler);
            BranchPostfixScope armPostfix(this);  // this arm's postfix ++/-- land in this arm only
            falseMark = compiler->MarkOwnedTemps();
            if (cxxTernaryDecl)
            {
                compiler->lastCxxRetTemp_ = nullptr;
                compiler->lastCxxRetValue_ = nullptr;
            }
            LLVMBackend::CastOccurrenceScope armScope(compiler);
            falseOcc = armScope.Id;
            std::optional<DeclExpectedTypeScope> expectedScope;
            if (falseDefault)
            {
                auto expected = expectedForDefault(trueValue, trueDefault);
                if (!expected.TypeName.empty())
                    expectedScope.emplace(&declExpectedType, expected,
                        isFunctionValueDestination(expected));
            }
            else if (functionValueDestination && use != ResultUse::Discard)
                expectedScope.emplace(&declExpectedType, outerExpected, true);
            std::optional<LambdaExpectedTypeRestoreGuard> lambdaArmScope;
            if (falseLambdaArm)
            {
                lambdaArmScope.emplace(&lambdaExpectedType);
                lambdaExpectedType = TernaryLambdaArmHint(slotLambdaType,
                    !falseFirst && !trueLambdaArm ? &trueShape : nullptr);
            }
            const bool falseNested = cxxTernaryDecl && use != ResultUse::Discard
                && ArmTernaryThroughParens(expressionFalseCtx) != nullptr;
            armNestedCxxTernary(falseNested);
            auto parsedFalse = parseTernaryDefaultArm(expressionFalseCtx, [&] {
                return parseFunctionDestinationConditionalArm(expressionFalseCtx, outerExpected);
            });
            auto falseTv = parsedFalse.first;
            falseValue = falseTv.value;
            falseShape = falseTv;
            falseUnsigned = falseTv.isUnsigned;
            falseStorage = falseTv.storage;
            // A NESTED `?:` arm publishes its own storage join on receiverStorage: a PHI
            // beside the value PHI names the selected object, so seed this arm from it.
            if (falseStorage == nullptr && llvm::isa_and_nonnull<llvm::PHINode>(falseValue))
                falseStorage = falseTv.receiverStorage;
            falseAlias = falseTv.isAlias;
            // Owning-call snapshot of the FALSE arm, right after it is emitted.
            const bool falseNestedOwned = falseValue != nullptr
                && falseValue == compiler->lastTernaryJoinValue && compiler->lastTernaryJoinArms != 0;
            if (falseShape.ownedCallArms != 0 || falseNestedOwned
                || compiler->ValueIsUniqueCallResult(falseValue))
                ownedCallArms |= 2;
            // A classified call result loaded from its sret slot is an rvalue, not a load.
            if (falseValue != nullptr && !compiler->IsProducedTempValue(falseValue))
                if (auto* load = llvm::dyn_cast<llvm::LoadInst>(falseValue))
                {
                    if (falseStorage == nullptr) falseStorage = load->getPointerOperand();
                    falseSlotRewritten = loadSlotRewrittenAfter(load)
                        || PlusPlus.count(load->getPointerOperand()) != 0;
                }
            falseAlias = falseAlias || compiler->IsAliasValue(falseValue);
            const int falseNestedState = takeNestedCxxTernaryArm(falseNested);
            restoreNestedDeclArm(falseNestedState, falseShape, falseValue,
                falseStorage, falseCxxTernaryDeclBuilt);
            if (falseNestedState == 2) cxxTernaryDeclFailed = nestedArmDeclFailed = true;
            adoptNestedDerivedArm(falseNestedState, falseDerived);
            falseTempField = compiler->IsTempFieldValue(falseValue);
            publishCxxCtorTemp(falseValue, falseStorage);
            if (falseValue != nullptr && falseValue == compiler->lastLoweredRetValue_)
                if (auto* st = llvm::dyn_cast<llvm::StructType>(falseValue->getType());
                    st != nullptr && st->hasName()
                    && compiler->NeedsCxxLoweringType(st->getName().str()))
                    falseLoweredRetTemp =
                        llvm::dyn_cast_or_null<llvm::AllocaInst>(compiler->lastLoweredRetTemp_);
            falseJoinSlot = loweredJoinSlotOf(falseValue);
            if (!cxxTernaryDecl && use != ResultUse::Discard && falseStorage != nullptr
                && falseStorage == compiler->lastCxxRetTemp_
                && llvm::isa<llvm::AllocaInst>(falseStorage) && falseValue != nullptr)
                if (auto* st = llvm::dyn_cast<llvm::StructType>(falseValue->getType());
                    st != nullptr && st->hasName()
                    && compiler->IsForeignNontrivialCxxReturnClass(st->getName().str()))
                    falseCxxRetTemp = falseStorage;
            if (trueLoweredRetTemp != nullptr && falseValue != nullptr && trueValue != nullptr
                && falseValue == compiler->lastLoweredRetValue_
                && falseValue->getType() == trueValue->getType())
                if (auto* falseTemp =
                        llvm::dyn_cast_or_null<llvm::AllocaInst>(compiler->lastLoweredRetTemp_);
                    falseTemp != nullptr && falseTemp != trueLoweredRetTemp
                    && falseTemp->getAllocatedType() == trueLoweredRetTemp->getAllocatedType())
                {
                    falseTemp->replaceAllUsesWith(trueLoweredRetTemp);
                    falseTemp->eraseFromParent();
                    falseLoweredRetTemp = trueLoweredRetTemp;
                    sharedLoweredRetTemp = trueLoweredRetTemp;
                }
            // Outside a declaration the false arm's temporary becomes the true arm's, so the
            // join is ONE prvalue temporary; its own live flag guards the shared destructor.
            if (trueCxxRetTemp != nullptr && !trueDefault && trueValue != nullptr
                && retargetCxxArmTemp(falseValue, falseStorage, trueCxxRetTemp, trueValue->getType()))
            {
                sharedCxxRetTemp = trueCxxRetTemp;
                falseStorage = sharedCxxRetTemp;
                compiler->RegisterOwnedStructTemp(sharedCxxRetTemp, sharedCxxRetType);
            }
            materializeCxxLvalueArm();
            if (falseNestedState == 1)
            {
                ++cxxTernaryDeclArmsMoved;
                ++cxxTernaryDeclFreshArms;
            }
            else if (falseNestedState == 0)
            {
                falseCxxTernaryDeclBuilt = cxxTernaryArmBuiltInPlace(
                    falseValue, falseStorage, falseMark);
                if (!falseCxxTernaryDeclBuilt)
                    moveCxxTernaryArmIntoDecl(falseValue, falseStorage, falseMark,
                        falseExplicitMove,
                        falseShape.hasSourceType ? falseShape.sourceType : InferTernaryArmType(falseValue),
                        falseDerived, falseCxxTernaryDeclDeferred,
                        falseCxxTernaryDeclBuilt);
            }
            if (ternaryDepth.IsOutermost() && !TernaryIsBinaryOperand(ctx)
                && falseValue != nullptr && !outerExpected.IsMove
                && !(outerExpected.IsOwningSink && compiler->OwningSinkConsumesConcrete(outerExpected)))
            {
                LLVMBackend::NamedVariable armNV;
                armNV.Primary = falseValue;
                armNV.BaseType = falseValue->getType();
                armNV.TypeAndValue = outerExpected;
                if (armNV.TypeAndValue.TypeName.empty())
                    if (auto* st = llvm::dyn_cast<llvm::StructType>(falseValue->getType()); st && st->hasName())
                        armNV.TypeAndValue.TypeName = st->getName().str();
                armNV.TypeAndValue.Pointer = false;
                armNV.TypeAndValue.IsAlias = false;
                armNV.TypeAndValue.IsMove = false;
                normalizeOwningArmType(armNV);
                // A moved plain-struct arm passed by value: its bytes left the zeroed source, so a
                // per-arm temp owns them until the call returns, as for a produced arm.
                const std::string& armTypeName = armNV.TypeAndValue.TypeName;
                if (falseExplicitMove && inCallArgument_ && !movedReceiverArms
                    && falseValue->getType()->isStructTy()
                    && falseValue->getType()
                        != llvm::StructType::getTypeByName(*compiler->context, "string")
                    && compiler->IsDataStructure(armTypeName)
                    && !compiler->IsForeignNontrivialCxxClass(armTypeName)
                    && compiler->IsOwningValueType(armTypeName)
                    && compiler->GetType(armNV.TypeAndValue) == falseValue->getType())
                {
                    auto* movedTemp = compiler->AllocaAtEntry(falseValue->getType(), nullptr, "argtemp");
                    compiler->builder->CreateStore(falseValue, movedTemp);
                    compiler->RegisterOwnedStructTemp(movedTemp, armTypeName);
                }
                else
                    compiler->RegisterBorrowedOwningStructTemp(armNV, true);
            }
            keepUnconsumedMovedArm(falseKeepMove, falseValue, falseStorage, falseShape, falseMark,
                expressionFalseCtx);
            if (use == ResultUse::Discard)
            {
                const auto type = ternaryArmSourceType(falseShape, falseValue, falseStorage);
                deferFalseDiscardCxxTemps = !type.Pointer && compiler->IsCxxRecord(type.TypeName);
                if (!deferFalseDiscardCxxTemps)
                    compiler->FlushOwnedTempsSince(falseMark, nullptr, branchBlock);
            }
            else if (!falseCxxTernaryDeclDeferred)
                FinishTernaryArm(compiler, falseValue, falseMark, falseOwnedString, branchBlock,
                    falseExplicitMove);
            armPostfix.Flush();
            falseEnd = compiler->builder->GetInsertBlock();
            falseBonded = compiler->lastCallIsBonded;
            collectBondedSources(falseBonded);
        };
        try
        {
            if (falseFirst)
            {
                parseFalseArm();
                falseTernaryMovedState = compiler->SaveMovedState();
                compiler->RestoreMovedState(ternaryPreMovedState);
                parseTrueArm();
                trueTernaryMovedState = compiler->SaveMovedState();
            }
            else
            {
                parseTrueArm();
                trueTernaryMovedState = compiler->SaveMovedState();
                compiler->RestoreMovedState(ternaryPreMovedState);
                parseFalseArm();
                falseTernaryMovedState = compiler->SaveMovedState();
            }
            if (functionValueDestination)
            {
                auto normalizeNullMacroArm = [&](llvm::Value*& value) {
                    if (value != nullptr && value->getType()->isPointerTy()
                        && compiler->IsNullFunctionSlotJoin(value))
                        value = llvm::ConstantPointerNull::get(
                            llvm::cast<llvm::PointerType>(value->getType()));
                };
                normalizeNullMacroArm(trueValue);
                normalizeNullMacroArm(falseValue);
            }
            resolveDerivedArm(trueDerived, falseDerived, trueEnd);
            resolveDerivedArm(falseDerived, trueDerived, falseEnd);
            const bool lvalueArmCandidate = (collapseThisLvalue(trueValue, trueStorage)
                    && collapseThisLvalue(falseValue, falseStorage)) && trueValue != nullptr
                && falseValue != nullptr && trueValue->getType() == falseValue->getType()
                && !trueValue->getType()->isPointerTy()
                && trueStorage != nullptr && falseStorage != nullptr
                && !isBorrowedTernaryArm(trueStorage) && !isBorrowedTernaryArm(falseStorage);
            if (lvalueArmCandidate)
            {
                compiler->RestoreMovedState(trueTernaryMovedState);
                markImplicitTernaryMove(trueStorage, trueValue);
                trueTernaryMovedState = compiler->SaveMovedState();
                compiler->RestoreMovedState(falseTernaryMovedState);
                markImplicitTernaryMove(falseStorage, falseValue);
                falseTernaryMovedState = compiler->SaveMovedState();
            }
            compiler->MergeMovedStates(trueTernaryMovedState, falseTernaryMovedState);
            if (use == ResultUse::Discard)
            {
                LLVMBackend::NamedVariable arm;
                arm.Primary = trueValue;
                DiagnoseDiscardedOwningReturn(ctx, arm);
                arm.Primary = falseValue;
                DiagnoseDiscardedOwningReturn(ctx, arm);
            }
        }
        catch (...)
        {
            // An error thrown mid-arm leaves half-emitted blocks; terminate them so the module
            // still verifies when the error is caught (expect_error) and compilation continues.
            if (compiler->IsInsertBlockLive()) compiler->builder->CreateBr(resumeBlock);
            for (auto* bb : { trueBlock, falseBlock })
            {
                if (cflat_llvm::GetTerminatorOrNull(bb) != nullptr) continue;
                compiler->SwitchToBlock(bb);
                compiler->builder->CreateBr(resumeBlock);
            }
            compiler->SwitchToBlock(resumeBlock);
            // Drop, without emitting anything, whatever the aborted arms registered: those
            // entries are keyed to arm blocks and would otherwise outlive this expression.
            compiler->DiscardOwnedTempsSince(falseMark);
            if (!trueDefault || trueEnd != nullptr)
                compiler->DiscardOwnedTempsSince(trueMark);
            throw;
        }

        std::string constPrvalueJoinType; // class of a prvalue join beside a const place leaf
        // C++ makes a same-type conditional a prvalue when one arm is a prvalue. Copy the
        // corresponding lvalue arm into its own branch temporary before the join.
        if (!cxxTernaryDecl && use != ResultUse::Discard && trueValue != nullptr
            && falseValue != nullptr && trueValue->getType() == falseValue->getType())
            if (auto* classType = llvm::dyn_cast<llvm::StructType>(trueValue->getType());
                classType != nullptr && classType->hasName()
                && compiler->IsCxxRecord(classType->getName().str()))
            {
                const std::string typeName = classType->getName().str();
                auto isOwnedCxxTemp = [&](llvm::Value* value, llvm::Value* storage) {
                    if (storage == nullptr || value == nullptr) return false;
                    LLVMBackend::NamedVariable probe;
                    probe.Primary = value;
                    probe.Storage = storage;
                    probe.BaseType = value->getType();
                    probe.TypeAndValue.TypeName = typeName;
                    return compiler->IsOwnedTempValue(probe);
                };
                auto materializeArm = [&](bool trueArm, bool useMove, bool constSource = false) {
                    llvm::Value*& value = trueArm ? trueValue : falseValue;
                    llvm::Value*& storage = trueArm ? trueStorage : falseStorage;
                    LLVMBackend::TypedValue& shape = trueArm ? trueShape : falseShape;
                    bool& alias = trueArm ? trueAlias : falseAlias;
                    llvm::BasicBlock*& endBlock = trueArm ? trueEnd : falseEnd;
                    auto* temp = compiler->CreateAlloca(classType);
                    auto savedIP = compiler->builder->saveIP();
                    if (auto* term = cflat_llvm::GetTerminatorOrNull(endBlock))
                        compiler->builder->SetInsertPoint(term);
                    else
                        compiler->builder->SetInsertPoint(endBlock);
                    const auto mark = compiler->MarkOwnedTemps();
                    if (compiler->EmitCxxCopyOrMoveConstruct(typeName, temp, storage,
                            useMove, "into a conditional expression",
                            /*displayTypeName*/ std::string(), constSource))
                    {
                        compiler->RegisterOwnedStructTemp(temp, typeName);
                        value = compiler->builder->CreateLoad(classType, temp);
                        storage = temp;
                        shape.value = value;
                        shape.storage = temp;
                        shape.receiverStorage = temp;
                        shape.isRvalue = true;
                        alias = false;
                        compiler->FlushOwnedTempsSince(mark, value, branchBlock);
                        endBlock = compiler->builder->GetInsertBlock();
                    }
                    compiler->builder->restoreIP(savedIP);
                };
                // A nested join arm owns when every storage leaf is an unnamed owned class temp.
                auto isOwnedCxxJoinTemp = [&](llvm::Value* value, llvm::Value* storage) {
                    if (isOwnedCxxTemp(value, storage)) return true;
                    if (!llvm::isa_and_nonnull<llvm::PHINode>(storage)) return false;
                    llvm::SmallPtrSet<llvm::PHINode*, 16> visitedJoins;
                    return ForEachPointerPhiLeaf(storage, visitedJoins, [&](llvm::Value* leaf) {
                        auto* slot = llvm::dyn_cast_or_null<llvm::AllocaInst>(leaf);
                        return slot != nullptr && compiler->FindVariableByStorage(slot) == nullptr
                            && std::any_of(compiler->pendingOwnedStructTemps.begin(),
                                compiler->pendingOwnedStructTemps.end(),
                                [&](const auto& temp) { return temp.Alloca == slot; });
                    });
                };
                const bool trueTemp = isOwnedCxxTemp(trueValue, trueStorage);
                const bool falseTemp = isOwnedCxxTemp(falseValue, falseStorage);
                const bool trueMoveLvalueJoin = trueShape.isExplicitMove
                    && !falseShape.isExplicitMove && !falseShape.isRvalue;
                const bool falseMoveLvalueJoin = falseShape.isExplicitMove
                    && !trueShape.isExplicitMove && !trueShape.isRvalue;
                // The copied place arm is a plain class prvalue now, not the `T&` / alias it read.
                auto materializePlaceArm = [&](bool trueArm) {
                    LLVMBackend::TypedValue& shape = trueArm ? trueShape : falseShape;
                    llvm::Value* const before = trueArm ? trueValue : falseValue;
                    materializeArm(trueArm, false, lvalueJoinConst());
                    if ((trueArm ? trueValue : falseValue) == before) return;
                    if (shape.hasSourceType && shape.sourceType.IsCxxRef())
                        shape.sourceType = LLVMBackend::TypeAndValue{ .TypeName = typeName };
                    shape.isAlias = false;
                    if (lvalueJoinConst()) constPrvalueJoinType = typeName;
                };
                if (TernaryIsMemberReceiver(ctx) && trueMoveLvalueJoin)
                {
                    materializeArm(true, true);
                    materializeArm(false, false);
                }
                else if (TernaryIsMemberReceiver(ctx) && falseMoveLvalueJoin)
                {
                    materializeArm(true, false);
                    materializeArm(false, true);
                }
                else if (trueTemp && trueShape.isCxxSameTypeCastCopy && !falseTemp && !falseShape.isRvalue
                    && isAddressableLvalue(falseValue, falseStorage))
                    materializeArm(false, false);
                else if (falseTemp && falseShape.isCxxSameTypeCastCopy && !trueTemp && !trueShape.isRvalue
                    && isAddressableLvalue(trueValue, trueStorage))
                    materializeArm(true, false);
                // Imported class prvalue arm beside an unmoved place: the join is a prvalue and the
                // place arm is copied, as clang does; a const leaf makes it a const prvalue.
                else if (compiler->IsForeignNontrivialCxxClass(typeName)
                    && isOwnedCxxJoinTemp(trueValue, trueStorage) && !falseTemp
                    && !falseShape.isRvalue && !falseShape.isExplicitMove
                    && !falseDefault
                    && isAddressableLvalue(falseValue, falseStorage))
                    materializePlaceArm(false);
                else if (compiler->IsForeignNontrivialCxxClass(typeName)
                    && isOwnedCxxJoinTemp(falseValue, falseStorage) && !trueTemp
                    && !trueShape.isRvalue && !trueShape.isExplicitMove
                    && !trueDefault
                    && isAddressableLvalue(trueValue, trueStorage))
                    materializePlaceArm(true);
            }

        // A C++ prvalue conditional used as a call argument (C++ or CFlat callee) or an operator operand
        // is the by-value parameter object: redirect every selected-arm producer to one slot.
        const bool nativeCxxByValueArgument = !outerExpected.TypeName.empty()
            && !outerExpected.Pointer && outerExpected.PointerDepth == 0
            && !outerExpected.IsAlias && !outerExpected.IsRvalueRef
            && compiler->IsForeignNontrivialCxxClass(
                compiler->ResolveTypeAlias(outerExpected.TypeName))
            && (IsDirectCallArgument(ctx) || IsCallArgumentThroughParens(ctx));
        const bool cxxCallArgumentShape = (inCxxCallArgument_
                && IsWholeCxxCallArgumentShape(ctx, outerExpected))
            || nativeCxxByValueArgument
            || IsParenthesizedBinaryOperandShape(ctx);
        if (!cxxTernaryDecl && use != ResultUse::Discard
            && cxxCallArgumentShape
            && trueValue != nullptr && falseValue != nullptr
            && trueValue->getType() == falseValue->getType()
            && !trueShape.isExplicitMove && !falseShape.isExplicitMove
            && trueShape.isRvalue && falseShape.isRvalue
            && !trueShape.isAlias && !falseShape.isAlias
            && !(trueShape.hasSourceType && trueShape.sourceType.IsCxxRef())
            && !(falseShape.hasSourceType && falseShape.sourceType.IsCxxRef()))
        {
            const auto trueType = ternaryArmSourceType(trueShape, trueValue, trueStorage);
            const auto falseType = ternaryArmSourceType(falseShape, falseValue, falseStorage);
            const std::string trueName = compiler->ResolveTypeAlias(trueType.TypeName);
            const std::string falseName = compiler->ResolveTypeAlias(falseType.TypeName);
            if (!trueName.empty() && trueName == falseName
                && compiler->IsForeignNontrivialCxxClass(trueName))
            {
                auto* objectType = compiler->GetType(
                    LLVMBackend::TypeAndValue{ .TypeName = trueName });
                std::vector<llvm::AllocaInst*> producerSlots;
                llvm::SmallPtrSet<llvm::PHINode*, 16> visitedStorageJoins;
                llvm::SmallPtrSet<llvm::AllocaInst*, 16> seenProducerSlots;
                auto collectProducerSlot = [&](llvm::Value* storage) {
                    auto* slot = llvm::dyn_cast_or_null<llvm::AllocaInst>(storage);
                    if (slot == nullptr) return false;
                    if (objectType == nullptr || slot->getAllocatedType() != objectType
                        || compiler->FindVariableByStorage(slot) != nullptr
                        || std::none_of(compiler->pendingOwnedStructTemps.begin(),
                            compiler->pendingOwnedStructTemps.end(), [&](const auto& temp) {
                                return temp.Alloca == slot
                                    && compiler->ResolveTypeAlias(temp.TypeName) == trueName;
                            })) return false;
                    if (seenProducerSlots.insert(slot).second) producerSlots.push_back(slot);
                    return true;
                };
                if (ForEachPointerPhiLeaf(trueStorage, visitedStorageJoins, collectProducerSlot)
                    && ForEachPointerPhiLeaf(falseStorage, visitedStorageJoins, collectProducerSlot)
                    && !producerSlots.empty()
                    && trueEnd != nullptr && falseEnd != nullptr
                    && cflat_llvm::GetTerminatorOrNull(trueEnd) == nullptr
                    && cflat_llvm::GetTerminatorOrNull(falseEnd) == nullptr)
                {
                    auto* resultSlot = compiler->AllocaAtEntry(
                        objectType, nullptr, "cxx.ternary.arg");
                    for (llvm::AllocaInst* slot : producerSlots)
                    {
                        slot->replaceAllUsesWith(resultSlot);
                        compiler->UnregisterOwnedStructTemp(slot);
                    }
                    compiler->builder->SetInsertPoint(trueEnd);
                    compiler->CreateJump(resumeBlock);
                    compiler->builder->SetInsertPoint(falseEnd);
                    compiler->CreateJump(resumeBlock);
                    compiler->SwitchToBlock(resumeBlock);
                    compiler->RegisterOwnedStructTemp(resultSlot, trueName);
                    auto* result = compiler->CreateLoad(objectType, resultSlot);
                    result->setName("cxx.ternary.arg.value");
                    compiler->lastCxxRetTemp_ = resultSlot;
                    compiler->lastCxxRetValue_ = result;
                    LLVMBackend::TypedValue joined{ result, false };
                    joined.isRvalue = true;
                    joined.storage = resultSlot;
                    joined.receiverStorage = resultSlot;
                    joined.sourceType = LLVMBackend::TypeAndValue{ .TypeName = trueName };
                    // A copied const place leaf keeps clang's const composite (binds const T&, not T&&).
                    if (!constPrvalueJoinType.empty()) joined.sourceType.ConstMask = 1u;
                    joined.sourceTypeName = trueName;
                    joined.hasSourceType = true;
                    consumeNewCxxTernaryElision(joined);
                    return joined;
                }
            }
        }

        // T306 r3: a default-only argument joins into the destination only when the other arm IS
        // that class. Any other arm type (a derived class, or a cast to another class) refuses here.
        if (cxxCallArgumentJoin && trueValue != nullptr && falseValue != nullptr)
        {
            const bool trueIsDefault = IsDefaultOnlyExpression(expressionTrueCtx);
            const bool falseIsDefault = IsDefaultOnlyExpression(expressionFalseCtx);
            llvm::Value* otherValue = trueIsDefault ? falseValue : trueValue;
            if (!(trueIsDefault && falseIsDefault)
                && otherValue->getType() != cxxTernaryDeclLlvmType())
            {
                antlr4::ParserRuleContext* defaultArmCtx = trueIsDefault ? expressionTrueCtx
                    : static_cast<antlr4::ParserRuleContext*>(expressionFalseCtx);
                antlr4::ParserRuleContext* otherArmCtx = trueIsDefault
                    ? static_cast<antlr4::ParserRuleContext*>(expressionFalseCtx) : expressionTrueCtx;
                // A place (named variable, field, deref) owns nothing here: the arms differ in TYPE.
                // A call result or other temporary lives in an unnamed slot and keeps the ownership text.
                llvm::Value* otherStorage = trueIsDefault ? falseStorage : trueStorage;
                const bool otherIsPlace = otherStorage != nullptr
                    && isAddressableLvalue(otherValue, otherStorage)
                    && !compiler->IsProducedTempValue(otherValue)
                    && (!llvm::isa<llvm::AllocaInst>(otherStorage)
                        || compiler->FindVariableByStorage(otherStorage) != nullptr);
                // Only a nontrivial C++ class temporary can own; a scalar, pointer or CFlat value is a type mismatch.
                const auto otherArmType = InferTernaryArmType(otherValue);
                const std::string otherSpelled = SpellType(*compiler, otherArmType);
                if (!otherIsPlace && otherSpelled.empty())
                    LogErrorContext(ctx, compiler->LocalizeMessage(
                        "'?:' arms differ in type: 'default' builds the argument type '{}', but '{}' is not "
                        "that type. Convert that arm to the argument type.",
                        { SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = cxxTernaryDeclType }),
                          CollapsedSourceText(otherArmCtx) }));
                else if (otherIsPlace || !compiler->IsForeignNontrivialCxxClass(otherArmType.TypeName))
                    LogErrorContext(ctx, compiler->LocalizeMessage(
                        "'?:' arms differ in type: '{}' is a '{}', but 'default' builds the argument type "
                        "'{}'. Convert that arm to the argument type.",
                        { CollapsedSourceText(otherArmCtx),
                          otherSpelled,
                          SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = cxxTernaryDeclType }) }));
                else
                    ReportMixedOwnershipTernaryJoin(otherArmCtx, defaultArmCtx, ctx);
            }
        }
        enum class CxxValueJoinResult { NotApplicable, DeclarationBuilt, CallArgumentBuilt };
        LLVMBackend::TypedValue cxxCallArgumentValue;
        std::vector<LLVMBackend::ConditionalMoveSource> cxxCallArgumentMoveSources;
        auto collectCallArgumentMoveSources = [&](const LLVMBackend::TypedValue& shape,
                                                   llvm::Value* value, llvm::Value* storage,
                                                   llvm::BasicBlock* armBlock) {
            auto appendUnique = [&](const LLVMBackend::ConditionalMoveSource& source) {
                auto existing = std::find_if(cxxCallArgumentMoveSources.begin(),
                    cxxCallArgumentMoveSources.end(), [&](const auto& candidate) {
                        return candidate.Storage == source.Storage
                            && candidate.ArmBlock == source.ArmBlock
                            && candidate.CallerName == source.CallerName;
                    });
                if (existing == cxxCallArgumentMoveSources.end())
                    cxxCallArgumentMoveSources.push_back(source);
            };
            for (const auto& source : shape.conditionalMoveSources) appendUnique(source);
            if (!shape.isExplicitMove || armBlock == nullptr || value == nullptr) return;
            if (storage == nullptr) storage = shape.receiverStorage;
            if (storage == nullptr) return;
            const std::string sourceName = shape.sourceCallerName.empty()
                ? compiler->FindVariableNameByStorage(storage) : shape.sourceCallerName;
            if (sourceName.empty()) return;
            LLVMBackend::ConditionalMoveSource source;
            source.Storage = storage;
            source.ValueType = value->getType();
            source.ArmBlock = armBlock;
            source.CallerName = sourceName;
            source.FieldName = shape.sourceFieldName;
            source.IsElementAccess = shape.sourceIsElementAccess;
            source.MoveParamSinkArgument = true;
            source.MovePathKey = shape.sourceMovePathKey;
            source.MovePathHasDynamicIndex = shape.sourceMovePathHasDynamicIndex;
            source.PointeeMove = shape.sourceIsPointeeMove;
            source.IsExplicitMove = true;
            appendUnique(source);
        };
        auto lowerMixedCxxValueJoin = [&]() {
            if (use == ResultUse::Discard || trueValue == nullptr || falseValue == nullptr
                || trueValue->getType()->isVoidTy() || falseValue->getType()->isVoidTy())
                return CxxValueJoinResult::NotApplicable;
            auto trueType = ternaryArmSourceType(trueShape, trueValue, trueStorage);
            auto falseType = ternaryArmSourceType(falseShape, falseValue, falseStorage);
            if (trueType.TypeName.empty() || falseType.TypeName.empty()
                || trueType.Pointer || falseType.Pointer || trueType.IsArrayView || falseType.IsArrayView
                || trueType.IsInterface || falseType.IsInterface
                || trueType.IsUnique || falseType.IsUnique)
                return CxxValueJoinResult::NotApplicable;
            const std::string trueName = compiler->ResolveTypeAlias(trueType.TypeName);
            const std::string falseName = compiler->ResolveTypeAlias(falseType.TypeName);
            using CxxArmCategory = LLVMBackend::CxxArmCategory;
            auto armCategory = [&](const LLVMBackend::TypedValue& shape) {
                if (shape.storage != nullptr
                    && (shape.storage == trueCxxRetTemp || shape.storage == falseCxxRetTemp))
                    return CxxArmCategory::Prvalue;
                if (shape.isExplicitMove) return CxxArmCategory::Xvalue;
                if (!shape.isRvalue) return CxxArmCategory::Lvalue;
                if (shape.hasSourceType && shape.sourceType.IsRvalueRef)
                    return CxxArmCategory::Xvalue;
                if (shape.hasSourceType && shape.sourceType.IsCxxRef())
                    return CxxArmCategory::Lvalue;
                return CxxArmCategory::Prvalue;
            };
            CxxArmCategory trueCategory = armCategory(trueShape);
            CxxArmCategory falseCategory = armCategory(falseShape);
            if (trueName.empty() || falseName.empty()
                || !compiler->IsCxxRecord(trueName) || !compiler->IsCxxRecord(falseName))
                return CxxValueJoinResult::NotApplicable;
            const bool sameClass = trueName == falseName;
            const bool lvalueXvalueMix = (trueCategory == CxxArmCategory::Lvalue
                    && falseCategory == CxxArmCategory::Xvalue)
                || (trueCategory == CxxArmCategory::Xvalue
                    && falseCategory == CxxArmCategory::Lvalue);
            const bool prvalueXvalueMix = (trueCategory == CxxArmCategory::Prvalue
                    && falseCategory == CxxArmCategory::Xvalue)
                || (trueCategory == CxxArmCategory::Xvalue
                    && falseCategory == CxxArmCategory::Prvalue);
            if (sameClass && !lvalueXvalueMix
                && !(trueCategory == CxxArmCategory::Xvalue
                    && falseCategory == CxxArmCategory::Xvalue)
                && !prvalueXvalueMix)
                return CxxValueJoinResult::NotApplicable;

            const bool callArgument = !cxxTernaryDecl && inCallArgument_
                && !cxxTernaryCallPrefixIncompatible_
                && ternaryDepth.IsOutermost()
                && (outerExpected.TypeName.empty()
                    || ((!outerExpected.Pointer && !outerExpected.IsAlias
                            && !outerExpected.IsRvalueRef)
                        || (outerExpected.IsCxxRef()
                            && outerExpected.PointerDepth == 0))
                        && compiler->IsForeignNontrivialCxxClass(
                            compiler->ResolveTypeAlias(outerExpected.TypeName)))
                && IsWholeCxxCallArgumentShape(ctx, outerExpected);
            const bool callThroughSink = callArgument && cxxTernaryCallSink_ != nullptr;
            if (!cxxTernaryDecl && !callArgument)
                return CxxValueJoinResult::NotApplicable;
            if (cxxTernaryDecl && !trueCxxTernaryDeclDeferred
                && !falseCxxTernaryDeclDeferred)
                return CxxValueJoinResult::NotApplicable;

            auto rvalueRefClassArm = [&](const LLVMBackend::TypeAndValue& type,
                                         const LLVMBackend::TypedValue& shape,
                                         llvm::Value* value) {
                return type.Pointer && type.IsRvalueRef && !type.ElemPointer
                    && type.PointerDepth <= 1 && shape.isRvalue && value != nullptr
                    && value->getType()->isPointerTy();
            };
            auto bindRvalueRefArm = [&](LLVMBackend::TypeAndValue& type, llvm::Value*& value,
                                        llvm::Value*& storage, llvm::BasicBlock* endBlock) {
                type.Pointer = false;
                type.PointerDepth = 0;
                type.IsRvalueRef = false;
                storage = value;
                auto savedIP = compiler->builder->saveIP();
                compiler->builder->SetInsertPoint(endBlock);
                value = compiler->CreateLoad(compiler->GetType(type), storage);
                compiler->builder->restoreIP(savedIP);
            };
            if (rvalueRefClassArm(trueType, trueShape, trueValue))
                bindRvalueRefArm(trueType, trueValue, trueStorage, trueEnd);
            if (rvalueRefClassArm(falseType, falseShape, falseValue))
                bindRvalueRefArm(falseType, falseValue, falseStorage, falseEnd);
            trueType.TypeName = trueName;
            falseType.TypeName = falseName;

            using Kind = LLVMBackend::CxxTernaryVerdict::Kind;
            LLVMBackend::CxxTernaryVerdict verdict;
            verdict = compiler->ClassifyCxxTernary(
                trueType, trueCategory, falseType, falseCategory);
            if (verdict.kind == Kind::Ambiguous)
                LogErrorContext(ctx, std::format(
                    "ternary branches have ambiguous C++ conversions between '{}' and '{}'",
                    compiler->DisplayCxxClassName(trueName),
                    compiler->DisplayCxxClassName(falseName)));
            if (verdict.kind == Kind::Ill)
            {
                if (callArgument)
                    LogErrorContext(ctx, std::format(
                        "ternary branches have no common C++ type: '{}' and '{}'",
                        compiler->DisplayCxxClassName(trueName),
                        compiler->DisplayCxxClassName(falseName)));
                return CxxValueJoinResult::NotApplicable;
            }
            const std::string resultName = compiler->ResolveTypeAlias(
                verdict.resultIsTrue ? trueType.TypeName : falseType.TypeName);
            std::unordered_map<std::string, bool> byValueCtorMemo;
            auto hasUniqueLvalueByValueConversion = [&](const std::string& sourceName) {
                auto found = byValueCtorMemo.find(sourceName);
                if (found != byValueCtorMemo.end()) return found->second;
                const bool unique = compiler->FindUniqueByValueConvertingCtor(resultName, sourceName);
                byValueCtorMemo.emplace(sourceName, unique);
                return unique;
            };
            const bool lvalueByValueConversion =
                (trueCategory == CxxArmCategory::Lvalue
                    && hasUniqueLvalueByValueConversion(trueName))
                || (falseCategory == CxxArmCategory::Lvalue
                    && hasUniqueLvalueByValueConversion(falseName));
            // CFlat callees keep the legacy lowering for lvalue by-value conversions; C++ callees
            // without a sink (methods, ctors) take the join below, like clang (MIXRES).
            if (callArgument && !callThroughSink && lvalueByValueConversion && !inCxxCallArgument_)
                return CxxValueJoinResult::NotApplicable;
            const std::string targetName = cxxTernaryDecl
                ? compiler->ResolveTypeAlias(cxxTernaryDeclType)
                : outerExpected.TypeName.empty() ? resultName
                : compiler->ResolveTypeAlias(outerExpected.TypeName);
            if (resultName.empty() || resultName != targetName)
                return CxxValueJoinResult::NotApplicable;
            llvm::Type* resultObject = compiler->GetType(
                LLVMBackend::TypeAndValue{ .TypeName = resultName });
            if (resultObject == nullptr || !resultObject->isSized())
                return CxxValueJoinResult::NotApplicable;
            const bool lvalueReferenceJoin = verdict.kind == Kind::LvalueRef
                && trueCategory == CxxArmCategory::Lvalue
                && falseCategory == CxxArmCategory::Lvalue;
            const bool xvalueReferenceJoin = sameClass && verdict.kind == Kind::XvalueRef
                && trueCategory == CxxArmCategory::Xvalue
                && falseCategory == CxxArmCategory::Xvalue;
            const std::string callSinkReturnName = compiler->ResolveTypeAlias(
                cxxTernaryCallSinkReturnType_.TypeName);
            const bool callSinkReturnsClass = !callSinkReturnName.empty()
                && compiler->IsCxxRecord(callSinkReturnName);
            const bool directByValueXvalueJoin = callArgument && xvalueReferenceJoin
                && callThroughSink && !outerExpected.IsCxxRef()
                && !cxxTernaryCallSinkTakesReference_;
            // Keep direct by-value xvalue joins on the legacy sink path and bookkeeping.
            if (callThroughSink && !callSinkReturnsClass && !directByValueXvalueJoin)
            {
                collectCallArgumentMoveSources(trueShape, trueValue, trueStorage, trueEnd);
                collectCallArgumentMoveSources(falseShape, falseValue, falseStorage, falseEnd);
            }
            if (callArgument && ((outerExpected.IsCxxRef() && lvalueReferenceJoin)
                || (xvalueReferenceJoin && !directByValueXvalueJoin)))
            {
                auto lowerReferenceAddress = [&](bool trueArm) -> llvm::Value* {
                    const auto& armType = trueArm ? trueType : falseType;
                    llvm::Value*& address = trueArm ? trueStorage : falseStorage;
                    const auto& armValue = trueArm ? trueValue : falseValue;
                    if (address == nullptr || armValue == nullptr) return nullptr;
                    if (compiler->ResolveTypeAlias(armType.TypeName) != resultName)
                    {
                        LLVMBackend::NamedVariable source;
                        source.Primary = armValue;
                        source.Storage = address;
                        source.BaseType = armValue->getType();
                        source.TypeAndValue = armType;
                        source.IsRvalue = xvalueReferenceJoin;
                        if (!compiler->LowerCxxReferenceToBase(armType.TypeName, resultName,
                                                               source, address))
                            return nullptr;
                    }
                    return address;
                };
                if (trueEnd == nullptr || falseEnd == nullptr)
                    return CxxValueJoinResult::NotApplicable;
                std::array<llvm::Value*, 2> addresses{};
                std::array<llvm::BasicBlock*, 2> incomingBlocks{};
                for (bool trueArm : { true, false })
                {
                    llvm::BasicBlock* endBlock = trueArm ? trueEnd : falseEnd;
                    compiler->builder->SetInsertPoint(endBlock);
                    // `move a` of a CFlat-declared class zeroed `a`: the callee binds the object
                    // itself, so restore it; it stays a live moved-from object (destroyed later).
                    llvm::Value* armSource = trueArm ? trueStorage : falseStorage;
                    llvm::Value* armBytes = trueArm ? trueValue : falseValue;
                    if (xvalueReferenceJoin && (trueArm ? trueExplicitMove : falseExplicitMove)
                        && armSource != nullptr && armBytes != nullptr
                        && armBytes->getType()->isStructTy()
                        && compiler->IsDataStructure((trueArm ? trueType : falseType).TypeName))
                    {
                        compiler->builder->CreateStore(armBytes, armSource);
                        compiler->MarkStorageNotExplicitlyMovedNull(armSource);
                    }
                    llvm::Value* address = lowerReferenceAddress(trueArm);
                    if (address == nullptr) return CxxValueJoinResult::NotApplicable;
                    const size_t index = trueArm ? 0 : 1;
                    addresses[index] = address;
                    incomingBlocks[index] = compiler->builder->GetInsertBlock();
                }
                compiler->SwitchToBlock(resumeBlock);
                auto* resultAddress = compiler->builder->CreatePHI(
                    cflat_llvm::PointerTo(resultObject), 2, "cxx.ternary.lvalue");
                resultAddress->addIncoming(addresses[0], incomingBlocks[0]);
                resultAddress->addIncoming(addresses[1], incomingBlocks[1]);
                for (llvm::BasicBlock* incoming : incomingBlocks)
                {
                    compiler->builder->SetInsertPoint(incoming);
                    if (cflat_llvm::GetTerminatorOrNull(incoming) == nullptr)
                        compiler->CreateJump(resumeBlock);
                }
                compiler->SwitchToBlock(resumeBlock);
                auto* joined = compiler->CreateLoad(resultObject, resultAddress);
                joined->setName(xvalueReferenceJoin
                    ? "cxx.ternary.xvalue.value" : "cxx.ternary.lvalue.value");
                cxxCallArgumentValue = LLVMBackend::TypedValue{ joined, false };
                cxxCallArgumentValue.storage = resultAddress;
                cxxCallArgumentValue.isRvalue = xvalueReferenceJoin;
                cxxCallArgumentValue.isExplicitMove = xvalueReferenceJoin;
                cxxCallArgumentValue.sourceType.TypeName = resultName;
                const bool constResult = CxxTernaryArmConst(trueType)
                    || CxxTernaryArmConst(falseType);
                if (constResult)
                    cxxCallArgumentValue.sourceType.ConstMask = 1;
                cxxCallArgumentValue.sourceType.IsMove = xvalueReferenceJoin && !constResult;
                cxxCallArgumentValue.hasSourceType = true;
                return CxxValueJoinResult::CallArgumentBuilt;
            }
            std::string callReturnSpelling;
            llvm::Type* callReturnType = nullptr;
            if (callThroughSink)
            {
                callReturnType = cxxTernaryCallSink_->getReturnType();
                // A void callee's sink returns no value: both arms are the void call itself.
                const bool voidSink = callReturnType != nullptr && callReturnType->isVoidTy();
                const unsigned sinkArity = cxxTernaryCallSink_->arg_size();
                if (callReturnType == nullptr || callReturnType->isAggregateType()
                    || (sinkArity != 1 && sinkArity != 2)
                    || !cxxTernaryCallSink_->getFunctionType()->getParamType(0)->isPointerTy())
                    return CxxValueJoinResult::NotApplicable;
                if (sinkArity == 2
                    && (cxxTernaryCallSinkEnvironment_ == nullptr
                        || !cxxTernaryCallSink_->getFunctionType()->getParamType(1)->isPointerTy()))
                    return CxxValueJoinResult::NotApplicable;
                if (voidSink) callReturnSpelling = "void";
                else if (cxxTernaryCallSinkReturnType_.TypeName.empty()
                    || !compiler->CxxSpellingForCflatType(
                        cxxTernaryCallSinkReturnType_.TypeName, callReturnSpelling))
                    return CxxValueJoinResult::NotApplicable;
            }
            const bool callerOwnsSinkResult = callThroughSink && !lvalueByValueConversion
                && (cxxTernaryCallSinkTakesReference_
                    || !compiler->IsCxxParamDestroyedInCallee(resultName));
            llvm::Value* resultSlot = cxxTernaryDecl ? cxxTernaryDeclDest
                : callThroughSink && !callerOwnsSinkResult ? nullptr
                : compiler->AllocaAtEntry(resultObject, nullptr, "cxx.ternary.arg");
            if (resultSlot == nullptr && !callThroughSink)
                return CxxValueJoinResult::NotApplicable;

            auto lowerArm = [&](bool trueArm, bool alreadyBuilt) {
                if (alreadyBuilt) return true;
                LLVMBackend::TypedValue& shape = trueArm ? trueShape : falseShape;
                llvm::Value*& value = trueArm ? trueValue : falseValue;
                llvm::Value*& storage = trueArm ? trueStorage : falseStorage;
                llvm::BasicBlock*& endBlock = trueArm ? trueEnd : falseEnd;
                const auto& armType = trueArm ? trueType : falseType;
                const auto armKind = trueArm ? trueCategory : falseCategory;
                const bool armIsCxxRetTemp = storage != nullptr
                    && storage == (trueArm ? trueCxxRetTemp : falseCxxRetTemp);
                if (endBlock == nullptr) return false;
                auto savedIP = compiler->builder->saveIP();
                compiler->builder->SetInsertPoint(endBlock);
                LLVMBackend::NamedVariable arm;
                arm.Primary = value;
                arm.Storage = storage;
                arm.BaseType = value != nullptr ? value->getType() : nullptr;
                arm.TypeAndValue = armType;
                arm.IsRvalue = armKind != CxxArmCategory::Lvalue;
                const auto& armMark = trueArm ? trueMark : falseMark;
                std::vector<llvm::Value*> callerOwnedProducerSlots;
                auto tracksByValueConversion = [&](const LLVMBackend::TypeAndValue& sourceType,
                                                    const std::string& sourceName) {
                    const bool calleeDestroysSource =
                        compiler->IsCxxParamDestroyedInCallee(sourceName);
                    if ((!callArgument && !calleeDestroysSource)
                        || sourceType.Pointer || sourceType.ElemPointer
                        || (armKind != CxxArmCategory::Prvalue && !armIsCxxRetTemp
                            && armKind != CxxArmCategory::Lvalue)
                        || sourceName == resultName)
                        return false;
                    return hasUniqueLvalueByValueConversion(sourceName)
                        && !compiler->HasImplicitClassConversionOperator(sourceName);
                };
                const bool trackConvertedProducer = tracksByValueConversion(armType,
                    compiler->ResolveTypeAlias(armType.TypeName));
                if (!compiler->LowerCxxTernaryArm(arm, trueType, trueCategory,
                        falseType, falseCategory, trueArm, verdict, false, resultSlot,
                        callThroughSink ? cxxTernaryCallSink_ : nullptr, callReturnSpelling,
                        &callerOwnedProducerSlots,
                        trackConvertedProducer,
                        callThroughSink ? cxxTernaryCallSinkEnvironment_ : nullptr,
                        callThroughSink && cxxTernaryCallSinkTakesReference_))
                {
                    compiler->builder->restoreIP(savedIP);
                    return false;
                }
                if (trackConvertedProducer
                    && compiler->IsCxxParamDestroyedInCallee(
                        compiler->ResolveTypeAlias(armType.TypeName)))
                {
                    llvm::SmallPtrSet<llvm::PHINode*, 16> visitedProducerJoins;
                    auto unregisterProducer = [&](llvm::Value* slot) {
                        compiler->UnregisterOwnedStructTemp(slot);
                        return true;
                    };
                    for (llvm::Value* slot : callerOwnedProducerSlots)
                        ForEachPointerPhiLeaf(slot, visitedProducerJoins, unregisterProducer, true);
                    callerOwnedProducerSlots.clear();
                }
                llvm::Value* keepCallerOwnedProducer = nullptr;
                // Keep ledger order so reverse cleanup preserves C++ temporary interleaving.
                auto hoistArmStructTemps = [&](size_t from, llvm::Value* keep) {
                    for (size_t i = from; i < compiler->pendingOwnedStructTemps.size(); ++i)
                    {
                        auto& temp = compiler->pendingOwnedStructTemps[i];
                        if (temp.Alloca == nullptr || temp.Alloca == keep) continue;
                        compiler->HoistOwnedStructTempTo(temp, branchBlock);
                    }
                };
                if (callerOwnedProducerSlots.size() == 1)
                {
                    keepCallerOwnedProducer = callerOwnedProducerSlots.front();
                    if (!compiler->IsOwnedStructTempSince(armMark, keepCallerOwnedProducer))
                        compiler->RegisterOwnedStructTemp(keepCallerOwnedProducer, armType.TypeName);
                    auto producer = std::find_if(compiler->pendingOwnedStructTemps.begin(),
                        compiler->pendingOwnedStructTemps.end(), [&](const auto& temp) {
                            return temp.Alloca == keepCallerOwnedProducer;
                        });
                    if (producer != compiler->pendingOwnedStructTemps.end())
                        compiler->HoistOwnedStructTempTo(*producer, branchBlock);
                }
                else
                {
                    size_t insertAt = armMark.Structs;
                    LLVMBackend::OwnedTempMark armFlushMark = armMark;
                    for (llvm::Value* slot : callerOwnedProducerSlots)
                    {
                        if (compiler->IsOwnedStructTempSince(armMark, slot))
                            compiler->ReleaseOwnedStructTempInPlace(slot);
                        compiler->RegisterOwnedCxxParamTempAt(slot, armType.TypeName, insertAt++);
                    }
                    for (llvm::Value* slot : callerOwnedProducerSlots)
                    {
                        auto producer = std::find_if(compiler->pendingOwnedStructTemps.rbegin(),
                            compiler->pendingOwnedStructTemps.rend(), [&](const auto& temp) {
                                return temp.Alloca == slot;
                            });
                        if (producer != compiler->pendingOwnedStructTemps.rend())
                            compiler->HoistOwnedStructTempTo(*producer, branchBlock);
                    }
                    armFlushMark.Structs = insertAt;
                    hoistArmStructTemps(armFlushMark.Structs, nullptr);
                    armFlushMark.Structs = compiler->pendingOwnedStructTemps.size();
                    compiler->FlushOwnedTempsSince(armFlushMark, nullptr, branchBlock);
                }
                if (callerOwnedProducerSlots.size() <= 1)
                {
                    auto armOtherTempsMark = armMark;
                    hoistArmStructTemps(armOtherTempsMark.Structs, keepCallerOwnedProducer);
                    armOtherTempsMark.Structs = compiler->pendingOwnedStructTemps.size();
                    compiler->FlushOwnedTempsSince(armOtherTempsMark,
                        keepCallerOwnedProducer, branchBlock);
                }
                value = arm.Primary;
                storage = arm.Storage;
                shape.value = value;
                shape.storage = storage;
                shape.receiverStorage = storage;
                shape.sourceType = LLVMBackend::TypeAndValue{ .TypeName = resultName };
                shape.sourceTypeName = resultName;
                shape.hasSourceType = true;
                shape.isRvalue = true;
                shape.isAlias = false;
                if (trueArm) trueAlias = false;
                else falseAlias = false;
                endBlock = compiler->builder->GetInsertBlock();
                compiler->builder->restoreIP(savedIP);
                return true;
            };
            if (!lowerArm(true, cxxTernaryDecl && trueCxxTernaryDeclBuilt)
                || !lowerArm(false, cxxTernaryDecl && falseCxxTernaryDeclBuilt))
                LogErrorContext(ctx, std::format(
                    "cannot lower the common C++ conditional type '{}' into its destination",
                    compiler->DisplayCxxClassName(resultName)));

            if (cxxTernaryDecl)
            {
                trueCxxTernaryDeclBuilt = true;
                falseCxxTernaryDeclBuilt = true;
                cxxTernaryDeclArmsMoved = 2;
                cxxTernaryDeclFreshArms = 2;
                cxxTernaryDeclFailed = false;
                return CxxValueJoinResult::DeclarationBuilt;
            }

            for (llvm::BasicBlock* endBlock : { trueEnd, falseEnd })
            {
                if (endBlock == nullptr || cflat_llvm::GetTerminatorOrNull(endBlock) != nullptr)
                    continue;
                compiler->builder->SetInsertPoint(endBlock);
                compiler->CreateJump(resumeBlock);
            }
            compiler->SwitchToBlock(resumeBlock);
            if (callThroughSink)
            {
                if (callerOwnsSinkResult)
                    compiler->RegisterOwnedStructTemp(resultSlot, resultName);
                // No PHI for a void result: the true arm's void call stands for the expression, and
                // only its type is read (no value flows out of the join).
                llvm::Value* joined = trueValue;
                if (!callReturnType->isVoidTy())
                {
                    auto* phi = compiler->builder->CreatePHI(callReturnType, 2,
                        "cxx.ternary.call.result");
                    phi->addIncoming(trueValue, trueEnd);
                    phi->addIncoming(falseValue, falseEnd);
                    joined = phi;
                }
                cxxTernaryCallResult_ = joined;
                cxxTernaryCallResultType_ = cxxTernaryCallSinkReturnType_;
                cxxTernaryCallConsumed_ = true;
                cxxCallArgumentValue = LLVMBackend::TypedValue{ joined, false };
                cxxCallArgumentValue.sourceType = cxxTernaryCallSinkReturnType_;
                cxxCallArgumentValue.hasSourceType = true;
                return CxxValueJoinResult::CallArgumentBuilt;
            }
            auto* joined = compiler->CreateLoad(resultObject, resultSlot);
            joined->setName("cxx.ternary.arg.value");
            compiler->RegisterOwnedStructTemp(resultSlot, resultName);
            compiler->lastCxxRetTemp_ = resultSlot;
            compiler->lastCxxRetValue_ = joined;
            cxxCallArgumentValue = LLVMBackend::TypedValue{ joined, false };
            cxxCallArgumentValue.isRvalue = true;
            cxxCallArgumentValue.storage = resultSlot;
            cxxCallArgumentValue.sourceType.TypeName = resultName;
            cxxCallArgumentValue.hasSourceType = true;
            return CxxValueJoinResult::CallArgumentBuilt;
        };
        const auto cxxValueJoinResult = lowerMixedCxxValueJoin();
        if (cxxValueJoinResult == CxxValueJoinResult::CallArgumentBuilt)
        {
            cxxCallArgumentValue.conditionalMoveSources = std::move(cxxCallArgumentMoveSources);
            const std::string callSinkReturnName = compiler->ResolveTypeAlias(
                cxxTernaryCallSinkReturnType_.TypeName);
            const bool callSinkReturnsClass = !callSinkReturnName.empty()
                && compiler->IsCxxRecord(callSinkReturnName);
            if (cxxTernaryCallConsumed_ && !callSinkReturnsClass)
                for (const auto& source : cxxCallArgumentValue.conditionalMoveSources)
                {
                    if (!source.IsExplicitMove || source.CallerName.empty()) continue;
                    if (!source.MovePathKey.empty())
                        compiler->MarkVariableFieldMoved(source.CallerName, source.MovePathKey);
                    else if (!source.FieldName.empty())
                        compiler->MarkVariableFieldMoved(source.CallerName, source.FieldName);
                    else
                        compiler->MarkVariableMoved(source.CallerName, source.PointeeMove);
                }
            consumeNewCxxTernaryElision(cxxCallArgumentValue);
            return cxxCallArgumentValue;
        }

        if (cxxTernaryDecl && !cxxTernaryDeclFailed && cxxTernaryDeclArmsMoved == 2)
        {
            if (trueEnd != nullptr && cflat_llvm::GetTerminatorOrNull(trueEnd) == nullptr)
            {
                compiler->builder->SetInsertPoint(trueEnd);
                compiler->CreateJump(resumeBlock);
            }
            if (falseEnd != nullptr && cflat_llvm::GetTerminatorOrNull(falseEnd) == nullptr)
            {
                compiler->builder->SetInsertPoint(falseEnd);
                compiler->CreateJump(resumeBlock);
            }
            compiler->SwitchToBlock(resumeBlock);
            compiler->pendingCxxTernaryDeclConsumed_ = true;
            // A nested join publishes its derived class and category to the enclosing arm.
            compiler->pendingCxxTernaryDeclArmType_.clear();
            compiler->pendingCxxTernaryDeclArmCat_ = 0;
            if (!trueDerived.typeName.empty() && trueDerived.typeName == falseDerived.typeName)
            {
                compiler->pendingCxxTernaryDeclArmType_ = trueDerived.typeName;
                compiler->pendingCxxTernaryDeclArmCat_ = static_cast<int>(
                    trueDerived.cat == falseDerived.cat ? trueDerived.cat : DerivedArmCat::Prvalue);
            }
            if (cxxByValueCallArgumentJoin)
            {
                auto* joined = compiler->CreateLoad(cxxTernaryDeclLlvmType(), cxxTernaryDeclDest);
                joined->setName("cxx.ternary.arg.value");
                compiler->RegisterOwnedStructTemp(cxxTernaryDeclDest, cxxTernaryDeclType);
                compiler->lastCxxRetTemp_ = cxxTernaryDeclDest;
                compiler->lastCxxRetValue_ = joined;
                LLVMBackend::TypedValue result{ joined, false };
                result.isRvalue = true;
                result.storage = cxxTernaryDeclDest;
                result.sourceType.TypeName = cxxTernaryDeclType;
                result.hasSourceType = true;
                return result;
            }
            // Two fresh arms built the slot in place: the join is a prvalue, not a place.
            LLVMBackend::TypedValue consumed{};
            consumed.isRvalue = cxxTernaryDeclFreshArms == 2;
            return consumed;
        }
        const bool cxxTernaryDeclScalarJoin = cxxTernaryDecl && !cxxTernaryDeclFailed
            && cxxTernaryDeclArmsMoved == 0 && cxxTernaryDeclScalarArms == 2;
        if (cxxTernaryDecl && !cxxTernaryDeclScalarJoin)
        {
            // A nested call-argument claim that fails to build reports the arm clash, not the
            // overload fallthrough of an empty argument (MIXRES).
            if (cxxNestedCallTernaryClaim_ && !nestedArmDeclFailed)
            {
                auto describeArm = [](llvm::Value* v) -> std::string {
                    llvm::Type* t = v != nullptr ? v->getType() : nullptr;
                    if (auto* st = llvm::dyn_cast_or_null<llvm::StructType>(t))
                        return st->hasName() ? st->getName().str() : std::string("struct");
                    if (t != nullptr && t->isPointerTy()) return "pointer";
                    if (t != nullptr && t->isIntegerTy()) return "i" + std::to_string(t->getIntegerBitWidth());
                    return "unknown";
                };
                LogErrorContext(ctx, std::format(
                    "ternary branches have incompatible types '{}' and '{}'",
                    describeArm(trueValue), describeArm(falseValue)));
            }
            if (trueEnd != nullptr && cflat_llvm::GetTerminatorOrNull(trueEnd) == nullptr)
            {
                compiler->builder->SetInsertPoint(trueEnd);
                compiler->CreateJump(resumeBlock);
            }
            if (falseEnd != nullptr && cflat_llvm::GetTerminatorOrNull(falseEnd) == nullptr)
            {
                compiler->builder->SetInsertPoint(falseEnd);
                compiler->CreateJump(resumeBlock);
            }
            compiler->SwitchToBlock(resumeBlock);
            compiler->pendingCxxTernaryDeclFailed_ = true;
            return {};
        }

        // A nested arm of a discarded conditional is parsed for its value; with a void branch it has
        // none for the outer join, so it finishes as the discarded statement it is.
        if (use == ResultUse::Value
            && (trueValue == nullptr || trueValue->getType()->isVoidTy()
                || falseValue == nullptr || falseValue->getType()->isVoidTy())
            && std::find(discardedNestedArms_.begin(), discardedNestedArms_.end(), ctx)
                != discardedNestedArms_.end())
            use = ResultUse::Discard;
        // A ternary statement evaluates an arm for side effects only. In particular, both arms
        // may be void calls. It still has to validate any value-producing arms as one join.
        if (use == ResultUse::Discard)
        {
            auto soleUnary = [](antlr4::tree::ParseTree* node)
                    -> CFlatParser::UnaryExpressionContext* {
                while (node != nullptr)
                {
                    if (auto* unary = dynamic_cast<CFlatParser::UnaryExpressionContext*>(node))
                        return unary;
                    if (auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node);
                        primary != nullptr && primary->expression() != nullptr
                        && primary->children.size() == 3)
                    {
                        node = primary->expression();
                        continue;
                    }
                    auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(node);
                    if (rule == nullptr || rule->children.size() != 1) return nullptr;
                    node = rule->children[0];
                }
                return nullptr;
            };
            auto recoverNamedArmShape = [&](antlr4::tree::ParseTree* arm,
                                            LLVMBackend::TypedValue& shape,
                                            llvm::Value* value) {
                auto* identifier = BareIdentifierInExpression(arm);
                bool addressOf = false;
                if (identifier == nullptr)
                {
                    auto* unary = soleUnary(arm);
                    if (unary != nullptr && unary->unaryOperator() != nullptr
                        && unary->unaryOperator()->And() != nullptr)
                    {
                        identifier = BareIdentifierInExpression(unary->castExpression());
                        addressOf = identifier != nullptr;
                    }
                }
                if (identifier == nullptr) return;
                const std::string name = identifier->getText();
                if (!NamesVisibleVariable(compiler, name)) return;
                auto source = VisibleVariableType(compiler, name);
                const bool fixedArray = source.ConstArraySize != 0;
                if (addressOf)
                {
                    if (source.Pointer)
                    {
                        source.PointerDepth = std::max(1, source.PointerDepth) + 1;
                        source.ElemPointer = true;
                    }
                    else
                    {
                        source.Pointer = true;
                        source.PointerDepth = 1;
                    }
                    source.IsArrayView = false;
                }
                else if (fixedArray)
                {
                    source.IsArrayView = false;
                    source.Pointer = true;
                    source.PointerDepth = 1;
                }
                shape.value = value;
                shape.sourceType = source;
                shape.sourceTypeName = source.TypeName;
                shape.hasSourceType = !source.TypeName.empty()
                    || source.IsFunctionPointer || source.IsCxxNullptrT;
                shape.isArrayView = source.IsArrayView && !fixedArray;
                shape.pointerDepth = source.PointerDepth;
                shape.elemPointer = source.ElemPointer;
            };
            recoverNamedArmShape(expressionTrueCtx, trueShape, trueValue);
            recoverNamedArmShape(expressionFalseCtx, falseShape, falseValue);
            // Each arm's category comes from the arm expression: `move x` and a T&& call are
            // xvalues, a T& call or a named object an lvalue, any other value-producing arm a prvalue.
            using CxxArmCategory = LLVMBackend::CxxArmCategory;
            auto armCategory = [&](const LLVMBackend::TypedValue& shape) {
                if (shape.isExplicitMove) return CxxArmCategory::Xvalue;
                if (!shape.isRvalue) return CxxArmCategory::Lvalue;
                if (shape.hasSourceType && shape.sourceType.IsRvalueRef) return CxxArmCategory::Xvalue;
                if (shape.hasSourceType && shape.sourceType.IsCxxRef()) return CxxArmCategory::Lvalue;
                return CxxArmCategory::Prvalue;
            };
            const CxxArmCategory trueCategory = armCategory(trueShape);
            const CxxArmCategory falseCategory = armCategory(falseShape);
            auto rejectUnmaterializedAggregateMismatch = [&]() {
                if (!trueShape.hasSourceType || !falseShape.hasSourceType) return false;
                const auto trueType = trueShape.sourceType;
                const auto falseType = falseShape.sourceType;
                if (trueType.TypeName.empty() || falseType.TypeName.empty()
                    || trueType.Pointer || falseType.Pointer
                    || trueType.IsArrayView || falseType.IsArrayView
                    || trueType.IsFunctionPointer || falseType.IsFunctionPointer
                    || trueType.IsInterface || falseType.IsInterface
                    || trueType.IsUnique || falseType.IsUnique)
                    return false;
                const std::string trueName = compiler->ResolveTypeAlias(trueType.TypeName);
                const std::string falseName = compiler->ResolveTypeAlias(falseType.TypeName);
                if (trueName.empty() || falseName.empty() || trueName == falseName)
                    return false;
                const bool trueCxx = compiler->IsCxxRecord(trueName);
                const bool falseCxx = compiler->IsCxxRecord(falseName);
                if (trueCxx != falseCxx) return false;
                if (trueCxx)
                {
                    if (cxxTernaryDecl) return false;
                    using CxxKind = LLVMBackend::CxxTernaryVerdict::Kind;
                    const auto verdict = compiler->ClassifyCxxTernary(trueType, trueCategory,
                                                                      falseType, falseCategory);
                    if (verdict.kind == CxxKind::Ambiguous)
                    {
                        LogErrorContext(ctx, std::format(
                            "ternary branches have ambiguous C++ conversions between '{}' and '{}'",
                            compiler->DisplayCxxClassName(trueName),
                            compiler->DisplayCxxClassName(falseName)));
                        return true;
                    }
                    if (verdict.kind != CxxKind::Ill) return false;
                    const std::string target = compiler->DisplayCxxClassName(trueName);
                    LogErrorContext(ctx, std::format(
                        "cannot initialize C++ class '{}' from this expression; use '{}(args)', "
                        "'= default', a '{}' lvalue, or 'move <{}> lvalue'",
                        target, target, target, target));
                    return true;
                }
                if (!compiler->IsDataStructure(trueName)
                    || !compiler->IsDataStructure(falseName))
                    return false;
                LogErrorContext(ctx, std::format(
                    "ternary branches have incompatible types '{}' and '{}'",
                    trueName, falseName));
                return true;
            };
            const bool trueLive = trueEnd != nullptr
                && cflat_llvm::GetTerminatorOrNull(trueEnd) == nullptr;
            const bool falseLive = falseEnd != nullptr
                && cflat_llvm::GetTerminatorOrNull(falseEnd) == nullptr;
            const bool trueVoidArm = trueValue != nullptr && trueValue->getType()->isVoidTy();
            const bool falseVoidArm = falseValue != nullptr && falseValue->getType()->isVoidTy();
            auto flushDeferredCxxArm = [&](bool trueArm) {
                bool& deferred = trueArm ? deferTrueDiscardCxxTemps : deferFalseDiscardCxxTemps;
                if (!deferred) return;
                llvm::BasicBlock*& endBlock = trueArm ? trueEnd : falseEnd;
                if (endBlock == nullptr) return;
                auto savedIP = compiler->builder->saveIP();
                if (auto* term = cflat_llvm::GetTerminatorOrNull(endBlock))
                    compiler->builder->SetInsertPoint(term);
                else
                    compiler->builder->SetInsertPoint(endBlock);
                compiler->FlushOwnedTempsSince(trueArm ? trueMark : falseMark, nullptr, nullptr);
                if (auto* current = compiler->builder->GetInsertBlock()) endBlock = current;
                compiler->builder->restoreIP(savedIP);
                deferred = false;
            };
            if (trueLive && falseLive && !trueVoidArm && !falseVoidArm)
            {
                auto trueClassType = ternaryArmSourceType(trueShape, trueValue, trueStorage);
                auto falseClassType = ternaryArmSourceType(falseShape, falseValue, falseStorage);
                const std::string trueClassName = compiler->ResolveTypeAlias(trueClassType.TypeName);
                const std::string falseClassName = compiler->ResolveTypeAlias(falseClassType.TypeName);
                // A T&& call result arrives as a pointer to the referent; as an arm it is a T xvalue.
                auto rvalueRefClassArm = [&](const LLVMBackend::TypeAndValue& type,
                                             const LLVMBackend::TypedValue& shape, llvm::Value* value) {
                    return type.Pointer && type.IsRvalueRef && !type.ElemPointer
                        && type.PointerDepth <= 1 && shape.isRvalue && value != nullptr
                        && value->getType()->isPointerTy();
                };
                const bool trueRvalueRef = rvalueRefClassArm(trueClassType, trueShape, trueValue);
                const bool falseRvalueRef = rvalueRefClassArm(falseClassType, falseShape, falseValue);
                const bool cxxClassConditional = (!trueClassType.Pointer || trueRvalueRef)
                    && (!falseClassType.Pointer || falseRvalueRef)
                    && !trueClassName.empty() && !falseClassName.empty()
                    && (trueClassName != falseClassName
                        || (trueCategory == CxxArmCategory::Xvalue
                            && falseCategory == CxxArmCategory::Xvalue))
                    && compiler->IsCxxRecord(trueClassName) && compiler->IsCxxRecord(falseClassName);
                auto bindRvalueRefArm = [&](LLVMBackend::TypeAndValue& type, llvm::Value*& value,
                                            llvm::Value*& storage, llvm::BasicBlock* endBlock) {
                    type.Pointer = false;
                    type.PointerDepth = 0;
                    type.IsRvalueRef = false;
                    storage = value;
                    auto savedIP = compiler->builder->saveIP();
                    compiler->builder->SetInsertPoint(endBlock);
                    value = compiler->CreateLoad(compiler->GetType(type), storage);
                    compiler->builder->restoreIP(savedIP);
                };
                if (cxxClassConditional)
                {
                    if (trueRvalueRef) bindRvalueRefArm(trueClassType, trueValue, trueStorage, trueEnd);
                    if (falseRvalueRef)
                        bindRvalueRefArm(falseClassType, falseValue, falseStorage, falseEnd);
                    trueClassType.TypeName = trueClassName;
                    falseClassType.TypeName = falseClassName;
                    trueShape.sourceType = trueClassType;
                    trueShape.sourceTypeName = trueClassName;
                    trueShape.hasSourceType = true;
                    falseShape.sourceType = falseClassType;
                    falseShape.sourceTypeName = falseClassName;
                    falseShape.hasSourceType = true;
                    // clang's conditional rules decide validity, ambiguity and the result type/category.
                    const auto cxxVerdict = compiler->ClassifyCxxTernary(
                        trueClassType, trueCategory, falseClassType, falseCategory);
                    using CxxKind = LLVMBackend::CxxTernaryVerdict::Kind;
                    if (cxxVerdict.kind == CxxKind::Ambiguous || cxxVerdict.kind == CxxKind::Ill)
                        rejectUnmaterializedAggregateMismatch();
                    else
                    {
                        // Each arm is lowered in its own block; the join merges the storage it leaves.
                        const LLVMBackend::TypeAndValue& resultType = cxxVerdict.resultIsTrue
                            ? trueClassType : falseClassType;
                        auto lowerCxxArm = [&](bool trueArm) {
                            LLVMBackend::TypedValue& shape = trueArm ? trueShape : falseShape;
                            llvm::Value*& value = trueArm ? trueValue : falseValue;
                            llvm::Value*& storage = trueArm ? trueStorage : falseStorage;
                            llvm::BasicBlock*& endBlock = trueArm ? trueEnd : falseEnd;
                            bool& alias = trueArm ? trueAlias : falseAlias;
                            if (endBlock == nullptr) return false;
                            auto savedIP = compiler->builder->saveIP();
                            compiler->builder->SetInsertPoint(endBlock);
                            LLVMBackend::NamedVariable arm;
                            arm.Primary = value;
                            arm.Storage = storage;
                            arm.BaseType = value != nullptr ? value->getType() : nullptr;
                            arm.TypeAndValue = trueArm ? trueClassType : falseClassType;
                            arm.IsRvalue = (trueArm ? trueCategory : falseCategory)
                                != CxxArmCategory::Lvalue;
                            LLVMBackend::OwnedTempMark armMark = compiler->MarkOwnedTemps();
                            if (!compiler->LowerCxxTernaryArm(arm, trueClassType, trueCategory,
                                    falseClassType, falseCategory, trueArm, cxxVerdict,
                                    use == ResultUse::Discard))
                            {
                                compiler->builder->restoreIP(savedIP);
                                return false;
                            }
                            // A discarded result dies with its arm (clang: end of the full expression);
                            // a used result is hoisted to the branch block, zeroed so it outlives the arm.
                            compiler->FlushOwnedTempsSince(armMark, nullptr,
                                use == ResultUse::Discard ? nullptr : branchBlock);
                            value = arm.Primary;
                            storage = arm.Storage;
                            shape.value = value;
                            shape.storage = storage;
                            shape.receiverStorage = storage;
                            shape.sourceType = resultType;
                            shape.sourceTypeName = resultType.TypeName;
                            shape.hasSourceType = true;
                            shape.isRvalue = cxxVerdict.kind != CxxKind::LvalueRef;
                            shape.isExplicitMove = cxxVerdict.kind == CxxKind::XvalueRef;
                            shape.isAlias = false;
                            alias = false;
                            endBlock = compiler->builder->GetInsertBlock();
                            compiler->builder->restoreIP(savedIP);
                            return true;
                        };
                        if (!lowerCxxArm(true) || !lowerCxxArm(false))
                            LogErrorContext(ctx, std::format(
                                "cannot initialize C++ class '{}' from this expression",
                                compiler->DisplayCxxClassName(trueClassName)));
                    }
                }
                UnifyTernaryFunctionArms(ctx, expressionTrueCtx, expressionFalseCtx,
                    trueValue, falseValue, trueShape, falseShape);
                if (trueValue == nullptr || falseValue == nullptr)
                {
                    if (rejectUnmaterializedAggregateMismatch())
                    {
                        for (auto* bb : { trueEnd, falseEnd })
                        {
                            if (bb == nullptr || cflat_llvm::GetTerminatorOrNull(bb) != nullptr)
                                continue;
                            compiler->SwitchToBlock(bb);
                            compiler->CreateJump(resumeBlock);
                        }
                        compiler->SwitchToBlock(resumeBlock);
                        return {};
                    }
                    auto trueType = trueShape.hasSourceType
                        ? trueShape.sourceType : InferTernaryArmType(trueShape);
                    auto falseType = falseShape.hasSourceType
                        ? falseShape.sourceType : InferTernaryArmType(falseShape);
                    const bool trueNull = trueType.IsCxxNullptrT
                        || llvm::isa_and_nonnull<llvm::ConstantPointerNull>(trueValue);
                    const bool falseNull = falseType.IsCxxNullptrT
                        || llvm::isa_and_nonnull<llvm::ConstantPointerNull>(falseValue);
                    if (TernaryPointerJoinIsRejected(compiler, trueType, falseType,
                            trueType.IsSimd, falseType.IsSimd,
                            trueShape.isArrayView || trueType.IsArrayView,
                            falseShape.isArrayView || falseType.IsArrayView,
                            trueNull, falseNull,
                            TernaryInterfaceDestination(*compiler, outerExpected),
                            !inCallArgument_ && (outerExpected.Pointer || outerExpected.PointerDepth > 0)))
                        LogErrorContext(ctx, UnrelatedPointerJoinDiagnostic(
                            compiler, trueType, falseType, "?:"));
                }
                if (trueValue != nullptr && falseValue != nullptr)
                {
                    llvm::BasicBlock** activeArmEnd = nullptr;
                    auto saveActiveArmEnd = [&]() {
                        if (activeArmEnd != nullptr)
                            if (auto* block = compiler->builder->GetInsertBlock())
                                *activeArmEnd = block;
                    };
                    auto atTrue = [&]() {
                        saveActiveArmEnd();
                        compiler->builder->SetInsertPoint(trueEnd);
                        activeArmEnd = &trueEnd;
                    };
                    auto atFalse = [&]() {
                        saveActiveArmEnd();
                        compiler->builder->SetInsertPoint(falseEnd);
                        activeArmEnd = &falseEnd;
                    };
                    const bool validTypes = UnifyTernaryArmTypes(ctx,
                        trueValue, falseValue, trueShape, falseShape,
                        trueStorage, falseStorage, atTrue, atFalse,
                        trueOcc, falseOcc, trueUnsigned, falseUnsigned,
                        TernaryInterfaceDestination(*compiler, outerExpected),
                        !inCallArgument_ && (outerExpected.Pointer || outerExpected.PointerDepth > 0),
                        &outerExpected);
                    saveActiveArmEnd();
                    if (!validTypes)
                    {
                        for (auto* bb : { trueEnd, falseEnd })
                        {
                            if (bb == nullptr || cflat_llvm::GetTerminatorOrNull(bb) != nullptr)
                                continue;
                            compiler->SwitchToBlock(bb);
                            compiler->CreateJump(resumeBlock);
                        }
                        compiler->SwitchToBlock(resumeBlock);
                        return {};
                    }
                }
                if (trueShape.isArrayView || falseShape.isArrayView)
                    PropagateTernaryViewElement(ctx, trueValue, trueStorage,
                        falseValue, falseStorage, nullptr, false, &trueShape, &falseShape);
            }
            // The later arm's temps sit after the earlier arm's in the owned list: flush it first.
            flushDeferredCxxArm(false);
            flushDeferredCxxArm(true);
            if (trueEnd != nullptr && cflat_llvm::GetTerminatorOrNull(trueEnd) == nullptr)
            {
                compiler->builder->SetInsertPoint(trueEnd);
                compiler->CreateJump(resumeBlock);
            }
            if (falseEnd != nullptr && cflat_llvm::GetTerminatorOrNull(falseEnd) == nullptr)
            {
                compiler->builder->SetInsertPoint(falseEnd);
                compiler->CreateJump(resumeBlock);
            }
            compiler->SwitchToBlock(resumeBlock);
            return {};
        }

        // An arm whose own lowering terminated its block never reaches the join; the ternary can
        // then only yield the other arm, so no PHI is needed (and none is legal).
        bool trueLive = trueEnd != nullptr && cflat_llvm::GetTerminatorOrNull(trueEnd) == nullptr;
        bool falseLive = falseEnd != nullptr && cflat_llvm::GetTerminatorOrNull(falseEnd) == nullptr;
        if (!trueLive || !falseLive)
        {
            if (trueLive)
            {
                compiler->builder->SetInsertPoint(trueEnd);
                trueBr = compiler->CreateJump(resumeBlock);
            }
            if (falseLive)
            {
                compiler->builder->SetInsertPoint(falseEnd);
                falseBr = compiler->CreateJump(resumeBlock);
            }
            compiler->SwitchToBlock(resumeBlock);
            if (!trueLive && !falseLive) return {};
            llvm::Value* only = trueBr != nullptr ? trueValue : falseValue;
            compiler->PromoteCastOccurrence(only, trueBr != nullptr ? trueOcc : falseOcc);
            LLVMBackend::TypedValue result{ only, trueBr != nullptr ? trueUnsigned : falseUnsigned };
            result.isAlias = trueBr != nullptr ? trueAlias : falseAlias;
            result.storage = trueBr != nullptr ? trueStorage : falseStorage;
            result.arithNarrowWidth = (trueBr != nullptr ? trueShape : falseShape).arithNarrowWidth;
            JoinTernaryPointerShape(result, trueBr != nullptr ? trueShape : falseShape,
                                    trueBr != nullptr ? trueShape : falseShape);
            JoinTernaryClassPointee(result, trueBr != nullptr ? trueShape : falseShape,
                                    trueBr != nullptr ? trueShape : falseShape);
            const auto& onlyShape = trueBr != nullptr ? trueShape : falseShape;
            JoinConditionalStackCharBufferBorrow(result, onlyShape, onlyShape);
            return result;
        }

        UnifyTernaryFunctionArms(ctx, expressionTrueCtx, expressionFalseCtx, trueValue, falseValue,
                                 trueShape, falseShape);
        if (trueValue == nullptr || falseValue == nullptr)
        {
            // A valueless arm still opened its block. Close every unterminated arm block, or
            // the module passes see a malformed function and assert in getTerminator().
            for (auto* bb : { trueEnd, falseEnd, trueBlock, falseBlock })
            {
                if (bb == nullptr || cflat_llvm::GetTerminatorOrNull(bb) != nullptr) continue;
                compiler->SwitchToBlock(bb);
                compiler->CreateJump(resumeBlock);
            }
            compiler->SwitchToBlock(resumeBlock);
            return {};
        }

        // A conditional of owned unique sources (a desugared field, an owning local) must remain a
        // raw pointer join so the selected slot can be nulled by the implicit move at the destination.
        auto lowerCoreUniqueField = [&](llvm::Value*& value, llvm::Value* storage,
                                        llvm::BasicBlock* block, const std::string& nullKey,
                                        const LLVMBackend::TypedValue& shape) {
            auto* structTy = llvm::dyn_cast<llvm::StructType>(value->getType());
            if (structTy == nullptr || !structTy->hasName()
                || !compiler->IsCoreUniqueType(structTy->getName().str()) || storage == nullptr)
                return false;
            compiler->builder->SetInsertPoint(block);
            LLVMBackend::NamedVariable receiver;
            receiver.Primary = value;
            receiver.Storage = storage;
            receiver.BaseType = storage->getType();
            receiver.TypeAndValue.TypeName = structTy->getName().str();
            LLVMBackend::TypeAndValue rawType;
            rawType.TypeName = MangledGenericArgument(
                *compiler, receiver.TypeAndValue.TypeName);
            rawType.Pointer = true;
            value = compiler->CreateCoreUniqueRawPointerCall(receiver, rawType);
            if (value != nullptr)
            {
                LLVMBackend::NamedVariable source;
                source.Primary = value;
                source.Storage = storage;
                source.BaseType = value->getType();
                source.CallerName = shape.sourceCallerName;
                source.FieldName = shape.sourceFieldName;
                source.RootIsAliasBorrowLocal = shape.sourceRootIsAliasBorrowLocal;
                source.IsAliasBorrow = shape.isAlias;
                source.TypeAndValue = shape.sourceType;
                // This read was lowered from a core unique wrapper, so the wrapper still owns
                // the pointer until a move explicitly consumes this field read.
                source.IsUniqueFieldAlias = true;
                // The owning struct of a field arm, as the field-read ledger records it (consume checks need it).
                if (auto* fieldGep = llvm::dyn_cast<llvm::GetElementPtrInst>(storage);
                    fieldGep != nullptr && fieldGep->getNumIndices() == 2
                    && fieldGep->getSourceElementType()->isStructTy())
                    source.OwningStructName = fieldGep->getSourceElementType()->getStructName().str();
                source.IsBorrowed = !shape.sourceBorrowedOrigin.empty();
                source.BorrowedOrigin = shape.sourceBorrowedOrigin;
                if (uniquePointerDestination
                    && llvm::isa_and_nonnull<llvm::GlobalVariable>(storage))
                {
                    source.TypeAndValue.TypeName = rawType.TypeName;
                    source.TypeAndValue.Pointer = true;
                    source.TypeAndValue.IsUnique = true;
                }
                compiler->RegisterUniqueFieldRead(value, storage, nullKey, source);
            }
            return value != nullptr;
        };
        auto isCoreUniqueStructValue = [&](llvm::Value* value) {
            auto* structTy = llvm::dyn_cast_or_null<llvm::StructType>(
                value == nullptr ? nullptr : value->getType());
            return structTy != nullptr && structTy->hasName()
                && compiler->IsCoreUniqueType(structTy->getName().str());
        };
        const bool expectedUniqueInterface = compiler->IsCoreUniqueType(outerExpected.TypeName)
            && compiler->IsInterfaceType(
                MangledGenericArgument(*compiler, outerExpected.TypeName));
        const bool expectedInterfaceDestination = outerExpected.IsInterface || expectedUniqueInterface;
        // A move-returning interface function consumes each selected unique<T> arm. Lower the
        // wrapper through release in its own arm so the selected source is empty at scope exit;
        // the returned pointer/fat value then goes through the ordinary interface boxing path.
        auto lowerUniqueMoveReturnArm = [&](llvm::Value*& value, llvm::Value*& storage,
                                            llvm::BasicBlock*& block, bool explicitMove) {
            auto* structTy = llvm::dyn_cast_or_null<llvm::StructType>(
                value == nullptr ? nullptr : value->getType());
            // Explicit `move` expressions already detach their wrapper in ParseMoveExpression.
            // Extract those detached values without releasing them again. An implicit unique<T>
            // arm in a move-interface return needs the release below to detach the selected arm.
            bool consumeMovedWrapper = moveInterfaceReturn && !explicitMove;
            bool extractDetachedWrapper = expectedInterfaceDestination && explicitMove;
            if ((!consumeMovedWrapper && !extractDetachedWrapper)
                || structTy == nullptr || !structTy->hasName()
                || !compiler->IsCoreUniqueType(structTy->getName().str()))
                return false;
            const auto& data = compiler->GetDataStructure(structTy->getName().str());
            std::string fieldName;
            size_t fieldIndex = 0;
            LLVMBackend::TypeAndValue releasedType;
            bool interfaceField = false;
            for (size_t i = 0; i < data.StructFields.size(); ++i)
            {
                const auto& field = data.StructFields[i];
                if (field.VariableName == "_p")
                {
                    fieldName = field.VariableName;
                    fieldIndex = i;
                    releasedType = field;
                    interfaceField = field.IsInterface;
                    break;
                }
            }
            if (fieldName.empty())
            {
                return false;
            }

            compiler->builder->SetInsertPoint(block);
            if (extractDetachedWrapper)
            {
                value = compiler->CreateExtractValue(value, (unsigned)fieldIndex);
            }
            else
            {
                LLVMBackend::NamedVariable receiver;
                receiver.Primary = value;
                receiver.Storage = storage;
                receiver.BaseType = structTy;
                receiver.TypeAndValue.TypeName = structTy->getName().str();
                receiver.TypeAndValue.IsMove = true;
                receiver.IsExplicitMove = true;
                receiver.IsOwning = true;
                receiver.IsBorrowed = false;
                receiver.CallerName.clear();
                releasedType.IsMove = true;
                releasedType.Pointer = !interfaceField;
                releasedType.IsInterface = interfaceField;
                releasedType.IsInterfacePointer = false;
                value = compiler->CreateCoreUniqueRawPointerCall(receiver, releasedType);
            }
            if (value == nullptr) return false;
            if (!interfaceField)
                compiler->RegisterValueElementTypeName(value, releasedType.TypeName);
            else
                compiler->RegisterFatInterfaceValueTypeName(value, releasedType.TypeName);
            compiler->RegisterMovedOutPtrValue(value);
            if (expectedInterfaceDestination)
            {
                std::string target = expectedUniqueInterface
                    ? MangledGenericArgument(*compiler, outerExpected.TypeName)
                    : outerExpected.TypeName;
                if (!interfaceField)
                {
                    std::string armFailure;
                    value = BoxTernaryThinArmToInterface(value, target, armFailure);
                    if (value == nullptr)
                        LogErrorContext(ctx, std::format(
                            "cannot convert '?:' arm to interface '{}': {}", target, armFailure));
                }
                else
                {
                    bool moved = compiler->IsMovedOutPtrValue(value);
                    value = compiler->ReboxInterfaceIfNeeded(
                        value, compiler->FindFatInterfaceValueTypeName(value), target);
                    compiler->RegisterFatInterfaceValueTypeName(value, target);
                    if (moved) compiler->RegisterMovedOutPtrValue(value);
                }
            }
            compiler->lastOwningResult = true;
            storage = nullptr;
            block = compiler->builder->GetInsertBlock();
            return true;
        };
        lowerUniqueMoveReturnArm(trueValue, trueStorage, trueEnd, trueExplicitMove);
        lowerUniqueMoveReturnArm(falseValue, falseStorage, falseEnd, falseExplicitMove);
        // An owned source arm (unique field, owning unique local, global pointer) joins as a raw pointer;
        // EmitImplicitUniqueFieldMove nulls the taken slot on its path and refuses a global source.
        auto ownedSourceArm = [&](llvm::Value* value, llvm::Value* storage, bool alias,
                                  bool explicitMove) {
            // A detached `move` value (also a parenthesized one) is answered by value identity.
            if (value == nullptr || alias || explicitMove || compiler->IsMovedOutPtrValue(value))
                return false;
            if (isCoreUniqueStructValue(value))
            {
                // A unique interface arm keeps the struct join and its ownership refusal.
                const std::string uniqueName = llvm::cast<llvm::StructType>(value->getType())->getName().str();
                if (compiler->IsInterfaceType(MangledGenericArgument(*compiler, uniqueName))) return false;
                return compiler->IsUniqueFieldReadValue(value)
                    || llvm::isa_and_nonnull<llvm::AllocaInst>(storage)
                    || llvm::isa_and_nonnull<llvm::GlobalVariable>(storage);
            }
            return value->getType()->isPointerTy()
                && llvm::isa_and_nonnull<llvm::GlobalVariable>(storage);
        };
        // The other arm may be any plain pointer (null, a fresh new, a pointer local): it adds no
        // source, exactly as a '??' rhs does.
        const bool trueOwnedArm = ownedSourceArm(trueValue, trueStorage, trueAlias, trueExplicitMove);
        const bool falseOwnedArm = ownedSourceArm(falseValue, falseStorage, falseAlias, falseExplicitMove);
        // Two by-value unique<T> locals keep the struct join (a call argument takes the struct).
        const bool structJoin = isCoreUniqueStructValue(trueValue) && isCoreUniqueStructValue(falseValue);
        const bool anyFieldArm = (isCoreUniqueStructValue(trueValue) && compiler->IsUniqueFieldReadValue(trueValue))
            || (isCoreUniqueStructValue(falseValue) && compiler->IsUniqueFieldReadValue(falseValue));
        const bool lowerUniqueFieldJoin = (trueOwnedArm || falseOwnedArm)
            && (trueOwnedArm || (trueValue != nullptr && trueValue->getType()->isPointerTy()))
            && (falseOwnedArm || (falseValue != nullptr && falseValue->getType()->isPointerTy()))
            && (!structJoin || anyFieldArm);
        if (lowerUniqueFieldJoin)
        {
            auto joinOwnedArm = [&](llvm::Value*& value, llvm::Value* storage,
                                    llvm::BasicBlock*& block,
                                    const LLVMBackend::TypedValue& shape) {
                const std::string nullKey = compiler->UniqueFieldReadNullKey(value);
                if (!isCoreUniqueStructValue(value))
                {
                    compiler->builder->SetInsertPoint(block);
                    compiler->RegisterUniqueFieldRead(value, storage, nullKey);
                    return;
                }
                lowerCoreUniqueField(value, storage, block, nullKey, shape);
                // The get() call may be an invoke that splits the arm: track its new end.
                block = compiler->builder->GetInsertBlock();
            };
            if (trueOwnedArm) joinOwnedArm(trueValue, trueStorage, trueEnd, trueShape);
            if (falseOwnedArm) joinOwnedArm(falseValue, falseStorage, falseEnd, falseShape);
            compiler->SwitchToBlock(resumeBlock);
            if (trueValue == nullptr || falseValue == nullptr) return {};
        }
        // Do arm-local conversions before adding their jumps. In particular, a runtime char*
        // converted to string emits a call to operator string and cannot be inserted after a
        // terminator.
        // Arm-local calls can emit invokes; save their continuation before switching arms.
        llvm::BasicBlock** activeArmEnd = nullptr;
        auto saveActiveArmEnd = [&]() {
            if (activeArmEnd != nullptr)
                if (auto* block = compiler->builder->GetInsertBlock()) *activeArmEnd = block;
        };
        auto atTrue = [&]() {
            saveActiveArmEnd();
            compiler->builder->SetInsertPoint(trueEnd);
            activeArmEnd = &trueEnd;
        };
        auto atFalse = [&]() {
            saveActiveArmEnd();
            compiler->builder->SetInsertPoint(falseEnd);
            activeArmEnd = &falseEnd;
        };
        bool joinUnsigned = TernaryJoinIsUnsigned(
            trueUnsigned,  trueValue->getType()->isIntegerTy()  ? trueValue->getType()->getIntegerBitWidth()  : 0,
            falseUnsigned, falseValue->getType()->isIntegerTy() ? falseValue->getType()->getIntegerBitWidth() : 0);
        auto cxxReferenceRecordType = [&](llvm::Value* value, llvm::Value* storage, bool isAlias) {
            auto* record = llvm::dyn_cast_or_null<llvm::StructType>(
                value != nullptr ? value->getType() : nullptr);
            return isAlias && storage != nullptr && record != nullptr && record->hasName()
                && compiler->IsCxxRecord(record->getName().str());
        };
        const bool trueCxxReference = cxxReferenceRecordType(trueValue, trueStorage, trueAlias);
        const bool falseCxxReference = cxxReferenceRecordType(falseValue, falseStorage, falseAlias);
        const bool trueCxxPointer = trueValue != nullptr && trueValue->getType()->isPointerTy()
            && compiler->IsCxxRecord(InferTernaryArmType(trueValue).TypeName);
        const bool falseCxxPointer = falseValue != nullptr && falseValue->getType()->isPointerTy()
            && compiler->IsCxxRecord(InferTernaryArmType(falseValue).TypeName);
        // Every arm must be addressable (a reference result or a pointer): a by-value arm keeps
        // the value join and its existing diagnostic.
        if ((trueCxxReference || falseCxxReference)
            && (trueCxxReference || (trueValue != nullptr && trueValue->getType()->isPointerTy()))
            && (falseCxxReference || (falseValue != nullptr && falseValue->getType()->isPointerTy())))
        {
            LLVMBackend::TypeAndValue pointerJoinDest = outerExpected;
            if (!pointerJoinDest.Pointer || !compiler->IsCxxRecord(pointerJoinDest.TypeName))
            {
                pointerJoinDest = {};
                if (trueCxxPointer) pointerJoinDest = InferTernaryArmType(trueValue);
                else if (falseCxxPointer) pointerJoinDest = InferTernaryArmType(falseValue);
            }
            const bool wantPointerJoin = pointerJoinDest.Pointer
                && compiler->IsCxxRecord(pointerJoinDest.TypeName);
            if (wantPointerJoin)
            {
                auto addressFor = [&](llvm::Value* value, llvm::Value* storage) {
                    auto source = InferTernaryArmType(value);
                    if (source.Pointer)
                        return compiler->AdjustCxxPointerForStore(
                            pointerJoinDest, source, value, "conditional expression");
                    source.Pointer = true;
                    return compiler->AdjustCxxPointerForStore(
                        pointerJoinDest, source, storage, "conditional expression");
                };
                if (trueCxxReference)
                {
                    atTrue();
                    trueValue = addressFor(trueValue, trueStorage);
                    trueStorage = nullptr;
                    trueAlias = false;
                }
                if (falseCxxReference)
                {
                    atFalse();
                    falseValue = addressFor(falseValue, falseStorage);
                    falseStorage = nullptr;
                    falseAlias = false;
                }
            }
        }
        // `&(c ? a : b)` needs two lvalue arms (clang). A literal, call or postfix ++/-- arm is a prvalue;
        // arms of different types join through a conversion, also a prvalue (a cast arm was converted by its type).
        const bool addressOperand = TernaryIsAddressOperand(ctx);
        const bool memberReceiver = TernaryIsMemberReceiver(ctx);
        const bool trueArmIsRvalue = trueShape.isRvalue
            || ((addressOperand || memberReceiver) && isExplicitValueCastArm(expressionTrueCtx));
        const bool falseArmIsRvalue = falseShape.isRvalue
            || ((addressOperand || memberReceiver) && isExplicitValueCastArm(expressionFalseCtx));
        auto armSourceType = [](llvm::Value* arm) -> llvm::Type* {
            if (auto* cast = llvm::dyn_cast<llvm::CastInst>(arm)) return cast->getSrcTy();
            return arm->getType();
        };
        if (addressOperand && trueValue != nullptr && falseValue != nullptr
            && !trueValue->getType()->isPointerTy() && !falseValue->getType()->isPointerTy())
        {
            // C++ class arms are categorized by the C++ prvalue path ('&' names the C++ temporary); the
            // MSVC sret temp is an owned temp here, Itanium's register return is not.
            auto isCxxRecordArm = [&](llvm::Value* arm) {
                auto* st = llvm::dyn_cast<llvm::StructType>(arm->getType());
                return st != nullptr && st->hasName() && compiler->IsCxxRecord(st->getName().str());
            };
            const bool cxxRecordArms = isCxxRecordArm(trueValue) && isCxxRecordArm(falseValue);
            if (!cxxRecordArms
                && (!isAddressableLvalue(trueValue, trueStorage) || !isAddressableLvalue(falseValue, falseStorage)
                    || trueArmIsRvalue || falseArmIsRvalue))
                LogErrorContext(ctx, "cannot take the address of a '?:' unless both arms are lvalues; "
                    "a postfix ++/-- arm, a literal or a call result is a prvalue");
            else if (armSourceType(trueValue) != armSourceType(falseValue) || trueUnsigned != falseUnsigned)
                LogErrorContext(ctx, "cannot take the address of a '?:' whose arms have different types; "
                    "the converted result is a prvalue");
        }
        if (memberReceiver && collapseLvalueSink
            && TernaryMemberReceiverIsAssignmentTarget(
                compiler, ctx, trueValue != nullptr ? trueValue->getType() : nullptr)
            && trueValue != nullptr && falseValue != nullptr
            && trueValue->getType() == falseValue->getType()
            && trueValue->getType()->isStructTy()
            && !compiler->IsCxxRecord(llvm::cast<llvm::StructType>(trueValue->getType())->getName().str())
            && (trueArmIsRvalue || falseArmIsRvalue))
            LogErrorContext(ctx, "cannot assign to a member of a '?:' unless both arms are lvalues");
        const bool liftedArm = LiftTernaryFunctionArmToClosure(ctx, trueValue, falseValue, trueShape,
                                        falseShape, trueStorage, falseStorage, atTrue, atFalse);
        if (!UnifyTernaryArmTypes(ctx, trueValue, falseValue, trueShape, falseShape,
                trueStorage, falseStorage, atTrue, atFalse, trueOcc, falseOcc,
                trueUnsigned, falseUnsigned, TernaryInterfaceDestination(*compiler, outerExpected),
                !inCallArgument_ && (outerExpected.Pointer || outerExpected.PointerDepth > 0),
                        &outerExpected))
        {
            atTrue(); compiler->CreateJump(resumeBlock);
            atFalse(); compiler->CreateJump(resumeBlock);
            compiler->SwitchToBlock(resumeBlock);
            return {};
        }
        /*
         * C++ composite pointer type: arms pointing at a C++ class and at a public base of it join
         * as the base ([expr.cond]/7); one class (or one class beside null) joins as itself. The
         * derived arm is converted on its own path before the PHI (a base at a non-zero offset
         * moves the address; null stays null), then both to a pointer destination's base.
         */
        LLVMBackend::TypeAndValue compositeCxxPointee;
        if (trueValue != nullptr && falseValue != nullptr
            && trueValue->getType()->isPointerTy() && falseValue->getType()->isPointerTy())
        {
            auto armPointer = [&](const LLVMBackend::TypedValue& shape, llvm::Value* value) {
                auto type = InferTernaryArmType(shape);
                if (!type.Pointer || type.ElemPointer || type.ValuePointerDepth() != 1
                    || type.IsInterface || type.IsInterfacePointer || type.IsFunctionPointer
                    || type.IsArrayView || shape.isArrayView)
                    return LLVMBackend::TypeAndValue{};
                // The value's ledger pointee wins, as adjustCxxPointerArm reads it: a CFlat pointer
                // cast `(B*)d` keeps d's address, so the arm still points at the derived class.
                const auto ledger = InferTernaryArmType(value);
                if (ledger.Pointer && compiler->IsCxxRecord(compiler->ResolveTypeAlias(ledger.TypeName)))
                    type.TypeName = ledger.TypeName;
                type.TypeName = compiler->ResolveTypeAlias(type.TypeName);
                return compiler->IsCxxRecord(type.TypeName) ? type : LLVMBackend::TypeAndValue{};
            };
            const auto trueType = armPointer(trueShape, trueValue);
            const auto falseType = armPointer(falseShape, falseValue);
            const bool trueToBase = trueType.Pointer && falseType.Pointer
                && compiler->IsCxxDerivedToBasePointer(trueType, falseType);
            const bool falseToBase = trueType.Pointer && falseType.Pointer && !trueToBase
                && compiler->IsCxxDerivedToBasePointer(falseType, trueType);
            const bool sameClass = trueType.Pointer && falseType.Pointer
                && trueType.TypeName == falseType.TypeName;
            auto armNull = [](const LLVMBackend::TypedValue& shape, llvm::Value* value) {
                return llvm::isa<llvm::ConstantPointerNull>(value)
                    || (shape.hasSourceType && shape.sourceType.IsCxxNullptrT);
            };
            const bool trueNull = armNull(trueShape, trueValue);
            const bool falseNull = armNull(falseShape, falseValue);
            const bool nullBeside = (trueNull && falseType.Pointer && !falseNull)
                || (falseNull && trueType.Pointer && !trueNull);
            if (trueToBase || falseToBase || sameClass || nullBeside)
            {
                compositeCxxPointee = LLVMBackend::TypeAndValue{
                    .TypeName = (trueToBase || trueNull ? falseType : trueType).TypeName,
                    .Pointer = true, .PointerDepth = 1 };
                // A destination the composite reaches converts both arms from it; any other
                // destination sees the composite (a derived-pointer store is still accepted: p3 issue).
                if (!pointerJoinDest.Pointer
                    || (pointerJoinDest.TypeName != compositeCxxPointee.TypeName
                        && !compiler->IsCxxDerivedToBasePointer(compositeCxxPointee, pointerJoinDest)))
                    pointerJoinDest = compositeCxxPointee;
                atTrue();
                if (trueToBase)
                    trueValue = compiler->AdjustCxxPointerForStore(
                        compositeCxxPointee, trueType, trueValue, "conditional expression");
                if (!trueNull) adjustCxxPointerArm(trueValue, &compositeCxxPointee);
                atFalse();
                if (falseToBase)
                    falseValue = compiler->AdjustCxxPointerForStore(
                        compositeCxxPointee, falseType, falseValue, "conditional expression");
                if (!falseNull) adjustCxxPointerArm(falseValue, &compositeCxxPointee);
            }
        }
        if (!compositeCxxPointee.Pointer)
        {
            // A fixed array arm (or a row `arr[1]`) converts as the pointer it decays to
            // (`c ? derivedArr : basePtr`).
            auto decayedArm = [&](const LLVMBackend::TypedValue& shape) {
                const auto type = InferTernaryArmType(shape);
                return type.ConstArraySize > 0 || !type.ConstInnerDimensions.empty()
                    ? LLVMBackend::ArrayDecayedPointer(type) : LLVMBackend::TypeAndValue{};
            };
            const auto trueArray = decayedArm(trueShape);
            const auto falseArray = decayedArm(falseShape);
            atTrue();
            adjustCxxPointerArm(trueValue, trueArray.Pointer ? &trueArray : nullptr);
            atFalse();
            adjustCxxPointerArm(falseValue, falseArray.Pointer ? &falseArray : nullptr);
        }
        // The fat interface LLVM type is shared by every interface. Rebox each moved arm against
        // the declared return interface after unification, using the per-value source-interface
        // ledger; this also handles parent/derived arms whose LLVM types are identical.
        if ((outerExpected.IsInterface || expectedUniqueInterface)
            && trueValue->getType() == compiler->GetFatPtrType()
            && falseValue->getType() == compiler->GetFatPtrType())
        {
            std::string target = outerExpected.TypeName;
            if (compiler->IsCoreUniqueType(target))
                target = MangledGenericArgument(*compiler, target);
            atTrue();
            trueValue = compiler->ReboxInterfaceIfNeeded(
                trueValue, compiler->FindFatInterfaceValueTypeName(trueValue), target);
            atFalse();
            falseValue = compiler->ReboxInterfaceIfNeeded(
                falseValue, compiler->FindFatInterfaceValueTypeName(falseValue), target);
        }
        if (trueValue->getType()->isVoidTy())
        {
            atTrue(); compiler->CreateJump(resumeBlock);
            atFalse(); compiler->CreateJump(resumeBlock);
            compiler->SwitchToBlock(resumeBlock);
            if (std::find(discardedNestedArms_.begin(), discardedNestedArms_.end(), ctx)
                == discardedNestedArms_.end())
                LogErrorContext(ctx, "ternary branches must produce a value; 'void' is not allowed");
            return {};
        }

        // An arm that only BECAME a string in unification is a wrapped char* literal, borrowing
        // nothing the flush could free, so copying after it is safe (see FinishTernaryArm).
        auto* strTy = llvm::StructType::getTypeByName(*compiler->context, "string");
        bool ownedString = false;
        if (strTy != nullptr && trueValue->getType() == strTy)
        {
            // A moved arm's buffer has no other owner (its source is zeroed): the join takes it.
            if (trueExplicitMove) trueOwnedString = true;
            if (falseExplicitMove) falseOwnedString = true;
            if (!trueOwnedString)  { atTrue();  trueValue  = AdoptTernaryStringArm(compiler, trueValue,  trueOwnedString); }
            if (!falseOwnedString) { atFalse(); falseValue = AdoptTernaryStringArm(compiler, falseValue, falseOwnedString); }
            ownedString = trueOwnedString && falseOwnedString;
        }
        std::unique_ptr<TernaryArrayJoinState> rawJoinData;
        rawJoinData = PrepareTernaryArrayJoinState(
            trueValue, falseValue, trueStorage, falseStorage, trueShape, falseShape,
            atTrue, atFalse);
        const bool rawArrayJoin = rawJoinData != nullptr;
        // A named function arm becomes its thin pointer BEFORE the join: after the PHI the Function
        // (and so its signature) is gone. The function's own signature is the arm's type here.
        atTrue();
        trueValue = compiler->BareFunctionToThinValue(trueValue, nullptr);
        atFalse();
        falseValue = compiler->BareFunctionToThinValue(falseValue, nullptr);
        llvm::AllocaInst* trueAliasTemp = nullptr;
        llvm::AllocaInst* falseAliasTemp = nullptr;
        if (compiler->currentFunctionReturnTV.IsAlias || trueAlias || falseAlias)
        {
            if (trueStorage == nullptr)
                trueAliasTemp = compiler->AllocaAtEntry(trueValue->getType(), nullptr, "alias_join_true");
            if (falseStorage == nullptr)
                falseAliasTemp = compiler->AllocaAtEntry(falseValue->getType(), nullptr, "alias_join_false");
            // A mixed join has no one source slot on the owning arm. Keep that arm in a
            // temporary slot so the alias-return path can preserve the old permissive shape;
            // the borrowed arm still returns its real storage when selected.
            if (trueAliasTemp != nullptr)
            {
                atTrue();
                compiler->builder->CreateStore(trueValue, trueAliasTemp);
            }
            if (falseAliasTemp != nullptr)
            {
                atFalse();
                compiler->builder->CreateStore(falseValue, falseAliasTemp);
            }
        }
        // T197: a closure join with an owned temp arm (call result or lambda literal) MOVES that
        // temp into the join - no copy, no extra retain. The arm that is not selected never runs,
        // so its temp never exists and needs no drop. A borrowed closure arm is copied in its own
        // block, so the join owns exactly one closure on every path and the join registers it.
        const bool closureJoinOwnsArms = trueValue != nullptr && falseValue != nullptr
            && trueValue->getType() == compiler->GetClosureFatPtrType()
            && falseValue->getType() == compiler->GetClosureFatPtrType()
            && (compiler->IsOwnedClosureTemp(trueValue) || compiler->IsOwnedClosureTemp(falseValue));
        if (closureJoinOwnsArms)
        {
            auto ownClosureArm = [&](llvm::Value*& arm) {
                if (compiler->IsOwnedClosureTemp(arm))
                {
                    compiler->UnregisterOwnedClosureTemp(arm);
                    compiler->UnregisterOwnedStructTempsHolding(arm);
                    return;
                }
                llvm::Value* owned = CloneTernaryClosureValue(arm, ctx);
                if (owned != arm) compiler->UnregisterOwnedClosureTemp(owned);
                arm = owned;
            };
            atTrue();
            ownClosureArm(trueValue);
            atFalse();
            ownClosureArm(falseValue);
        }
        // Lowered CFlat struct join: one shared temp; move arms move fields in, copy arms copy,
        // fresh call arms build in place (clang prvalue conditional, no zeroed bitwise join).
        const std::string loweredJoinName = trueValue != nullptr && falseValue != nullptr
            && trueValue->getType() == falseValue->getType()
            && trueValue->getType()->isStructTy()
            ? (llvm::cast<llvm::StructType>(trueValue->getType())->hasName()
                ? llvm::cast<llvm::StructType>(trueValue->getType())->getName().str() : std::string())
            : std::string();
        const bool loweredJoinArms = !cxxTernaryDecl && use != ResultUse::Discard
            && !closureJoinOwnsArms && !trueAlias && !falseAlias
            && (trueExplicitMove || falseExplicitMove || trueJoinSlot != nullptr
                || falseJoinSlot != nullptr || trueLoweredRetTemp != nullptr
                || falseLoweredRetTemp != nullptr) && !loweredJoinName.empty()
            && compiler->NeedsCxxLoweringType(loweredJoinName)
            && (trueExplicitMove ? trueStorage != nullptr
                : trueJoinSlot != nullptr || trueLoweredRetTemp != nullptr || trueStorage != nullptr)
            && (falseExplicitMove ? falseStorage != nullptr
                : falseJoinSlot != nullptr || falseLoweredRetTemp != nullptr || falseStorage != nullptr);
        llvm::AllocaInst* loweredJoinTemp = nullptr;
        // `z = k ? move a : move b` is an xvalue join: clang move-assigns z from the selected source
        // with no temp, so each arm restores and re-arms its source and the join names its storage.
        // A nested all-move arm of such an assignment, or of a discarded all-move join, is an xvalue
        // join too: the outermost join names the selected leaf's storage, no inner temp.
        auto* xvalueTop = OutermostArmJoin(ctx);
        const bool loweredXvalueAssign = loweredJoinArms && keepBothMoved
            && use == ResultUse::Value
            && (xvalueTop == ctx || (armAllMoves(xvalueTop->expression())
                && armAllMoves(xvalueTop->conditionalExpression())))
            && (IsPlainAssignmentSource(xvalueTop)
                || std::find(discardedNestedArms_.begin(), discardedNestedArms_.end(), ctx)
                    != discardedNestedArms_.end());
        if (loweredXvalueAssign)
        {
            // A nested xvalue join arm already restored and re-armed its own sources.
            if (!llvm::isa<llvm::PHINode>(trueStorage))
            {
                atTrue();
                compiler->builder->CreateStore(trueValue, trueStorage);
                compiler->MarkStorageNotExplicitlyMovedNull(trueStorage);
            }
            if (!llvm::isa<llvm::PHINode>(falseStorage))
            {
                atFalse();
                compiler->builder->CreateStore(falseValue, falseStorage);
                compiler->MarkStorageNotExplicitlyMovedNull(falseStorage);
            }
        }
        else if (loweredJoinArms)
        {
            llvm::Type* loweredType = trueValue->getType();
            auto* joinTemp = compiler->CreateAlloca(loweredType);
            auto buildLoweredArm = [&](llvm::Value* value, llvm::Value* storage, bool explicitMove,
                                       llvm::AllocaInst* freshTemp) {
                if (freshTemp != nullptr)
                {
                    compiler->UnregisterOwnedStructTemp(freshTemp);
                    freshTemp->replaceAllUsesWith(joinTemp);
                    return;
                }
                if (explicitMove)
                {
                    compiler->builder->CreateStore(value, storage);
                    // A const lvalue arm makes the composite const: the moved arm is copied.
                    if (lvalueJoinConst())
                        compiler->EmitLoweredMemberwiseCopy(loweredJoinName, loweredType, joinTemp,
                            storage, "into a conditional expression");
                    else
                        compiler->EmitLoweredMemberwiseMove(loweredJoinName, loweredType, joinTemp,
                            storage, "into a conditional expression");
                    // The source is left a live moved-from object, so its scope exit destroys it.
                    compiler->MarkStorageNotExplicitlyMovedNull(storage);
                    return;
                }
                compiler->EmitLoweredMemberwiseCopy(loweredJoinName, loweredType, joinTemp,
                    storage, "into a conditional expression");
            };
            // A constructed arm (`LU(5)`) is the load of its own arm-owned temp: built in place too.
            auto freshArmTemp = [&](llvm::Value* value, llvm::Value* storage, bool explicitMove,
                                    llvm::AllocaInst* known,
                                    const LLVMBackend::OwnedTempMark& armMark) -> llvm::AllocaInst* {
                if (known != nullptr || explicitMove) return known;
                auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(value);
                auto* slot = llvm::dyn_cast_or_null<llvm::AllocaInst>(storage);
                if (load == nullptr || slot == nullptr || load->getPointerOperand() != slot
                    || slot->getAllocatedType() != loweredType
                    || compiler->FindVariableByStorage(slot) != nullptr
                    || !compiler->IsOwnedStructTempSince(armMark, slot))
                    return nullptr;
                return slot;
            };
            // A throwing member copy emits an invoke that splits the arm block: track its new end.
            atTrue();
            buildLoweredArm(trueValue, trueStorage, trueExplicitMove,
                freshArmTemp(trueValue, trueStorage, trueExplicitMove,
                    trueJoinSlot != nullptr ? trueJoinSlot : trueLoweredRetTemp, trueMark));
            trueEnd = compiler->builder->GetInsertBlock();
            atFalse();
            buildLoweredArm(falseValue, falseStorage, falseExplicitMove,
                freshArmTemp(falseValue, falseStorage, falseExplicitMove,
                    falseJoinSlot != nullptr ? falseJoinSlot : falseLoweredRetTemp, falseMark));
            falseEnd = compiler->builder->GetInsertBlock();
            loweredJoinTemp = joinTemp;
            // The join temp owns the selected arm; a per-arm owner (argtemp) would destroy it again.
            compiler->UnregisterOwnedStructTempsHolding(trueValue);
            compiler->UnregisterOwnedStructTempsHolding(falseValue);
        }
        atTrue();
        trueBr = compiler->CreateJump(resumeBlock);
        atFalse();
        falseBr = compiler->CreateJump(resumeBlock);
        compiler->SwitchToBlock(resumeBlock);
        auto* phi = compiler->builder->CreatePHI(trueValue->getType(), 2, "ternary");
        phi->addIncoming(trueValue,  trueEnd);
        phi->addIncoming(falseValue, falseEnd);
        compiler->RegisterJoinArmCastOccurrence(phi, 0, trueOcc);
        compiler->RegisterJoinArmCastOccurrence(phi, 1, falseOcc);
        compiler->PropagateProducedTempValue(trueValue, phi);
        compiler->PropagateProducedTempValue(falseValue, phi);
        // A pointer join with a storage-less arm (`x + 1`, a call, a literal) is a pointer prvalue:
        // a non-const `T*&` argument refuses it, as clang does (LowerAliasByPointerArg reads this mark).
        if (trueValue != nullptr && falseValue != nullptr && trueValue->getType()->isPointerTy()
            && falseValue->getType()->isPointerTy() && (trueStorage == nullptr || falseStorage == nullptr))
            phi->setMetadata("cflat.ptr.prvalue.join", llvm::MDNode::get(compiler->builder->getContext(), {}));
        llvm::Value* storageJoin = nullptr;
        // Both arms are addressable values: join their STORAGE as well, so the result names the
        // SELECTED object instead of becoming a detached copy of the joined value.
        llvm::Value* cxxLvalueStorageJoin = nullptr;
        bool cxxRecordJoin = false;
        if (trueValue != nullptr && trueValue->getType()->isStructTy())
        {
            auto* structTy = llvm::cast<llvm::StructType>(trueValue->getType());
            cxxRecordJoin = structTy->hasName() && compiler->IsCxxRecord(structTy->getName().str());
        }
        const bool mixedCxxRecordCategories = cxxRecordJoin
            && (trueShape.isRvalue != falseShape.isRvalue);
        // Two addressable arms join their STORAGE, record or plain scalar: `c ? a : b` selects
        // an OBJECT, so a C++ `T&` parameter must bind the selected one, not a copy. POINTER
        // joins are excluded - their ownership analysis reads the value, not a joined slot.
        const bool scalarLvalueJoin = !cxxRecordJoin && trueValue != nullptr
            && (trueValue->getType()->isIntegerTy() || trueValue->getType()->isFloatingPointTy())
            && (trueShape.isExplicitMove || !trueSlotRewritten)
            && (falseShape.isExplicitMove || !falseSlotRewritten);
        const bool collapseSinkJoin = collapseLvalueSink && isOwningRecordValue(trueValue)
            && isOwningRecordValue(falseValue)
            && isAddressableLvalue(trueValue, trueStorage)
            && isAddressableLvalue(falseValue, falseStorage)
            && !trueTempField && !falseTempField;
        // `&(c ? a : b)` names the SELECTED lvalue in every context (call argument, return, assignment):
        // without this join the address is taken of a copy of the joined value.
        // Pointer arms keep their own slot join (pointerSlotJoin), so only record and scalar arms here.
        const bool addressLvalueJoin = addressOperand
            && trueValue != nullptr && falseValue != nullptr
            && !trueValue->getType()->isPointerTy() && !falseValue->getType()->isPointerTy()
            && isAddressableLvalue(trueValue, trueStorage)
            && isAddressableLvalue(falseValue, falseStorage)
            && !trueTempField && !falseTempField;
        // `(c ? a : b).f` names the selected record too, so `.f = v` writes it (clang); owning arms keep the value join.
        const bool memberReceiverJoin = memberReceiver && trueValue != nullptr
            && falseValue != nullptr && trueValue->getType()->isStructTy()
            && falseValue->getType()->isStructTy() && !isOwningRecordValue(trueValue)
            && !isOwningRecordValue(falseValue)
            && !trueArmIsRvalue && !falseArmIsRvalue
            && isAddressableLvalue(trueValue, trueStorage) && isAddressableLvalue(falseValue, falseStorage)
            && !trueTempField && !falseTempField;
        const bool collapseStorageJoin = collapseLvalueArms || collapseSinkJoin || addressLvalueJoin
            || memberReceiverJoin;
        // A scalar arm rewritten after its load (`i++`, `i--`) names no lvalue slot; records keep their join.
        const bool scalarSlotRewritten = trueValue != nullptr && falseValue != nullptr
            && (((trueValue->getType()->isIntegerTy() || trueValue->getType()->isFloatingPointTy())
                    && trueSlotRewritten && !trueShape.isExplicitMove)
                || ((falseValue->getType()->isIntegerTy() || falseValue->getType()->isFloatingPointTy())
                    && falseSlotRewritten && !falseShape.isExplicitMove));
        const bool valueLvalueJoin = collapseStorageJoin && trueValue != nullptr && falseValue != nullptr
            && trueValue->getType() == falseValue->getType()
            && !trueValue->getType()->isPointerTy()
            && trueStorage != nullptr && falseStorage != nullptr
            && !scalarSlotRewritten
            && !isBorrowedTernaryArm(trueStorage) && !isBorrowedTernaryArm(falseStorage);
        // C++ `T*&` / `T* const&` args bind the SELECTED pointer slot, as record and scalar joins do.
        // Call arguments only; owning, borrowed, moved or mixed-type arms keep the value join.
        auto pointerSlotArm = [&](llvm::Value* storage) {
            const auto* source = compiler->FindVariableByStorage(storage);
            return source == nullptr || !source->IsOwning;
        };
        const LLVMBackend::TypeAndValue truePointerArmType = trueShape.hasSourceType
            ? trueShape.sourceType : InferTernaryArmType(trueValue);
        const LLVMBackend::TypeAndValue falsePointerArmType = falseShape.hasSourceType
            ? falseShape.sourceType : InferTernaryArmType(falseValue);
        // `&(c ? p : q)` names the selected pointer slot too; owning arms still keep the value join.
        const bool pointerSlotJoin = (inCallArgument_ || addressOperand)
            && trueValue != nullptr && falseValue != nullptr
            && trueValue->getType()->isPointerTy() && falseValue->getType()->isPointerTy()
            && trueStorage != nullptr && falseStorage != nullptr
            && !trueShape.isExplicitMove && !falseShape.isExplicitMove
            && !trueAlias && !falseAlias && !trueTempField && !falseTempField
            && !isBorrowedTernaryArm(trueStorage) && !isBorrowedTernaryArm(falseStorage)
            && pointerSlotArm(trueStorage) && pointerSlotArm(falseStorage)
            && !trueSlotRewritten && !falseSlotRewritten
            && truePointerArmType.TypeName == falsePointerArmType.TypeName
            && truePointerArmType.PointerDepth == falsePointerArmType.PointerDepth;
        // A moved arm of a CFlat-declared class leaves a live moved-from source; a consumer
        // constructing from the storage join restores the selected source's bytes first.
        const bool zeroedMoveArms = cxxRecordJoin && !cxxTernaryDecl && loweredJoinTemp == nullptr
            && (trueExplicitMove || falseExplicitMove) && trueBr != nullptr && falseBr != nullptr
            && compiler->IsDataStructure(
                llvm::cast<llvm::StructType>(trueValue->getType())->getName().str());
        if (zeroedMoveArms)
        {
            const auto savedIP = compiler->builder->saveIP();
            if (trueExplicitMove && trueStorage != nullptr)
            {
                compiler->builder->SetInsertPoint(trueBr);
                compiler->MarkStorageNotExplicitlyMovedNull(trueStorage);
            }
            if (falseExplicitMove && falseStorage != nullptr)
            {
                compiler->builder->SetInsertPoint(falseBr);
                compiler->MarkStorageNotExplicitlyMovedNull(falseStorage);
            }
            compiler->builder->restoreIP(savedIP);
        }
        if (sharedCxxRetTemp != nullptr && trueStorage == sharedCxxRetTemp
            && falseStorage == sharedCxxRetTemp)
            cxxLvalueStorageJoin = sharedCxxRetTemp;
        // T197: an owned closure join is a fresh value, not an lvalue; a storage PHI here made
        // a nested callee load through an untyped pointer (null-callee ICE).
        else if (!closureJoinOwnsArms
            && (cxxRecordJoin || scalarLvalueJoin || valueLvalueJoin || pointerSlotJoin)
            && trueStorage != nullptr && falseStorage != nullptr)
        {
            auto* cxxStoragePhi = compiler->builder->CreatePHI(
                compiler->builder->getPtrTy(), 2, "cxx_lvalue_storage_join");
            cxxStoragePhi->addIncoming(trueStorage, trueEnd);
            cxxStoragePhi->addIncoming(falseStorage, falseEnd);
            cxxLvalueStorageJoin = cxxStoragePhi;
            if (zeroedMoveArms) compiler->MarkZeroedMoveStorageJoin(cxxStoragePhi);
        }
        if (loweredXvalueAssign && cxxLvalueStorageJoin == nullptr)
        {
            auto* xvalueStorage = compiler->builder->CreatePHI(
                compiler->builder->getPtrTy(), 2, "lowered_xvalue_join");
            xvalueStorage->addIncoming(trueStorage, trueEnd);
            xvalueStorage->addIncoming(falseStorage, falseEnd);
            cxxLvalueStorageJoin = xvalueStorage;
        }
        if (cxxRecordJoin && HasDefaultOnlyTernaryArm(ctx)
            && llvm::isa_and_nonnull<llvm::PHINode>(cxxLvalueStorageJoin))
            compiler->RegisterCxxDefaultTernaryStorageJoin(cxxLvalueStorageJoin);
        const bool collapsedLvalueJoin = collapseStorageJoin && cxxLvalueStorageJoin != nullptr;
        // The shared temporary holds the selected arm: the join is its loaded value. Like a lowered
        // call result it is a produced value the consumer owns, not a registered temporary.
        // The load is emitted after every PHI of the join.
        llvm::Value* loweredJoined = nullptr;
        if (loweredJoinTemp != nullptr)
        {
            cxxLvalueStorageJoin = nullptr;
            loweredJoined = compiler->builder->CreateLoad(loweredJoinTemp->getAllocatedType(),
                loweredJoinTemp);
            if (auto* joinLoad = llvm::dyn_cast<llvm::LoadInst>(loweredJoined))
                joinLoad->setMetadata("cflat.lowered.join",
                    llvm::MDNode::get(compiler->builder->getContext(), {}));
            // Produced like a lowered call result: a parenthesized read registers its spill.
            compiler->nullConditionalTempResults_.push_back(loweredJoined);
            sharedLoweredRetTemp = loweredJoinTemp;
            // A by-value argument join is owned by the statement until the call adopts it, so a
            // throwing later sibling unwinds it; the adopting call disarms it (no double destroy).
            if (inCallArgument_ && (IsDirectCallArgument(ctx) || IsCallArgumentThroughParens(ctx)))
                compiler->RegisterOwnedStructTemp(loweredJoinTemp, loweredJoinName);
        }
        auto isOwnedCxxTempArm = [&](llvm::Value* value, llvm::Value* storage) {
            if (value == nullptr || storage == nullptr || !value->getType()->isStructTy())
                return false;
            auto* structType = llvm::cast<llvm::StructType>(value->getType());
            if (!structType->hasName()
                || !compiler->IsForeignNontrivialCxxClass(structType->getName().str()))
                return false;
            LLVMBackend::NamedVariable arm;
            arm.Primary = value;
            arm.Storage = storage;
            arm.TypeAndValue.TypeName = structType->getName().str();
            return compiler->IsOwnedTempValue(arm);
        };
        const bool bothOwnedCxxTempArms = isOwnedCxxTempArm(trueValue, trueStorage)
            && isOwnedCxxTempArm(falseValue, falseStorage);
        if (compiler->currentFunctionReturnTV.IsAlias || trueAlias || falseAlias)
        {
            auto* storagePhi = compiler->builder->CreatePHI(
                compiler->builder->getPtrTy(), 2, "alias_storage_join");
            storagePhi->addIncoming(trueStorage != nullptr
                    ? trueStorage : static_cast<llvm::Value*>(trueAliasTemp), trueEnd);
            storagePhi->addIncoming(falseStorage != nullptr
                    ? falseStorage : static_cast<llvm::Value*>(falseAliasTemp), falseEnd);
            storageJoin = storagePhi;
        }

        if (rawArrayJoin)
        {
            auto* countPhi = compiler->builder->CreatePHI(
                compiler->builder->getInt64Ty(), 2, "raw_array_count_join");
            countPhi->addIncoming(rawJoinData->TrueCount.Count, trueEnd);
            countPhi->addIncoming(rawJoinData->FalseCount.Count, falseEnd);
            auto allocAlignOf = [&](llvm::Value* value) {
                if (const auto* raw = compiler->FindRawArrayResult(value))
                    return raw->AllocAlign;
                if (const auto* scalar = compiler->FindOwnedNewTemp(value))
                    return scalar->AllocAlign;
                return uint64_t{0};
            };
            auto isNull = [](llvm::Value* value) {
                auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(value);
                return constant != nullptr && constant->isNullValue();
            };
            uint64_t trueAlign = allocAlignOf(trueValue);
            uint64_t falseAlign = allocAlignOf(falseValue);
            if (rawJoinData->IsView ? (rawJoinData->ViewAlwaysOwns && !isNull(trueValue)
                    && !isNull(falseValue) && trueAlign != falseAlign)
                : (!isNull(trueValue) && !isNull(falseValue) && trueAlign != falseAlign))
                LogErrorContext(ctx, "cannot join owning pointer allocations with different "
                    "alignment; use the same 'alignas' on every allocating arm");
            uint64_t commonAlign = rawJoinData->IsView
                ? (rawJoinData->TrueViewMayOwn ? trueAlign : falseAlign)
                : (isNull(trueValue) ? falseAlign : trueAlign);
            if (rawJoinData->IsView)
            {
                if (rawJoinData->ViewMayOwn)
                {
                    auto* ownsPhi = compiler->builder->CreatePHI(
                        compiler->builder->getInt1Ty(), 2, "view_ternary_owns");
                    ownsPhi->addIncoming(rawJoinData->TrueViewOwns, trueEnd);
                    ownsPhi->addIncoming(rawJoinData->FalseViewOwns, falseEnd);
                    compiler->RegisterRawArrayResult(
                        phi, countPhi, commonAlign, rawJoinData->ViewAlwaysOwns, ownsPhi, true,
                        rawJoinData->TrueCount.Known && rawJoinData->FalseCount.Known,
                        rawJoinData->TrueCount.SourceParams);
                }
                else
                    compiler->RegisterRawArrayResult(phi, countPhi, commonAlign, false,
                        nullptr, true, rawJoinData->TrueCount.Known
                            && rawJoinData->FalseCount.Known, rawJoinData->TrueCount.SourceParams);
            }
            else if (rawJoinData->IsFixed)
                compiler->RegisterRawArrayResult(phi, countPhi, 0, false, nullptr, false,
                    rawJoinData->TrueCount.Known && rawJoinData->FalseCount.Known,
                    rawJoinData->TrueCount.SourceParams);
            else
                compiler->RegisterRawArrayResult(phi, countPhi, commonAlign);
        }

        // One C++ reference arm joined with a plain pointer is a borrowed pointer, not an alias join.
        const bool cxxRefBorrowJoin = (compiler->IsCxxReferenceAliasArm(trueValue)
                && !compiler->IsAliasValue(falseValue))
            || (compiler->IsCxxReferenceAliasArm(falseValue) && !compiler->IsAliasValue(trueValue));
        if (ownedString)
        {
            compiler->RegisterOwnedStringTemp(phi);
            if (trueTempField || falseTempField) compiler->RegisterTempFieldValue(phi);
            compiler->lastCallReturnsOwned = true;
            LLVMBackend::TypedValue result{ phi, true };
            // Arm copies into owned storage do not carry the source stack borrow through the join.
            if (trueOwnedString && !trueExplicitMove) trueShape.stackCharBufferBorrow = false;
            if (falseOwnedString && !falseExplicitMove) falseShape.stackCharBufferBorrow = false;
            JoinConditionalStackCharBufferBorrow(result, trueShape, falseShape);
            result.isAlias = (trueAlias || falseAlias) && !cxxRefBorrowJoin;
            result.storage = storageJoin != nullptr ? storageJoin : cxxLvalueStorageJoin;
            result.receiverStorage = cxxLvalueStorageJoin;
            result.isCxxPrvalueConditional = mixedCxxRecordCategories;
            // No storage join means at least one arm was a temporary, so the join is one too:
            // say so, or a `T&` parameter would silently bind a copy.
            result.isRvalue = cxxRecordJoin && cxxLvalueStorageJoin == nullptr;
            return result;
        }

        // A ternary is a transparent wrapper: ownership rides out on the joined value.
        // Mixed owning/borrow POINTER joins are handled in PropagateTernaryOwnership.
        // A mixed join owns NOTHING the receiver may free, so the sticky per-expression ownership
        // side-channels must be cleared too: they carry no value identity, so a declaration would
        // otherwise adopt the phi whichever arm ran and double-free the borrow arm's live pointee.
        // A nested lowered join arm is a prvalue the outer join owns: it is not a mixed borrow.
        const bool nestedLoweredJoin = loweredJoinTemp != nullptr
            && (trueJoinSlot != nullptr || falseJoinSlot != nullptr);
        if (!collapsedLvalueJoin && !nestedLoweredJoin
            && compiler->PropagateTernaryOwnership(
                trueValue, falseValue, phi, uniquePointerDestination))
        {
            compiler->ClearOwnedResultChannels();
            RejectMixedOwnershipTernaryJoin(trueValue, falseValue, phi,
                expressionTrueCtx, expressionFalseCtx, ctx);
            RejectMixedOwnershipPointerTernaryJoin(trueValue, falseValue, phi,
                trueStorage, falseStorage, expressionTrueCtx, expressionFalseCtx, ctx,
                inCallArgument_ && IsDirectCallArgument(ctx)
                    ? &deferredPointerJoinDiagnostic : nullptr);
        }
        /*
         * T391: a `default` arm joined by STORAGE into a C++ call argument is an unowned class
         * temporary nothing destroys. Own it on the ledger behind a live flag set in that arm only,
         * so the statement end destroys it exactly when the arm ran (the lvalue arm is untouched).
         */
        if (inCallArgument_ && inCxxCallArgument_ && !cxxCallArgumentJoin && trueDefault != falseDefault
            && cxxRecordJoin && trueValue != nullptr && falseValue != nullptr
            && trueValue->getType() == falseValue->getType() && branchBlock != nullptr)
            if (auto* storagePhi = llvm::dyn_cast_or_null<llvm::PHINode>(cxxLvalueStorageJoin))
            {
                llvm::BasicBlock* defaultEnd = trueDefault ? trueEnd : falseEnd;
                auto* recordType = llvm::cast<llvm::StructType>(trueValue->getType());
                const std::string typeName = recordType->getName().str();
                const int incoming = defaultEnd != nullptr ? storagePhi->getBasicBlockIndex(defaultEnd) : -1;
                auto* temp = incoming < 0 ? nullptr
                    : llvm::dyn_cast<llvm::AllocaInst>(storagePhi->getIncomingValue(incoming));
                LLVMBackend::NamedVariable tempArm;
                tempArm.Storage = temp;
                tempArm.TypeAndValue.TypeName = typeName;
                if (temp != nullptr && (outerExpected.TypeName.empty() || outerExpected.TypeName == typeName)
                    && compiler->IsForeignNontrivialCxxClass(typeName)
                    && temp->getAllocatedType() == recordType
                    && temp->getParent() == &temp->getFunction()->getEntryBlock()
                    && compiler->FindVariableByStorage(temp) == nullptr
                    && !compiler->IsOwnedTempValue(tempArm)
                    && cflat_llvm::GetTerminatorOrNull(branchBlock) != nullptr
                    && cflat_llvm::GetTerminatorOrNull(defaultEnd) != nullptr)
                {
                    auto* liveFlag = compiler->AllocaAtEntry(compiler->builder->getInt1Ty(), nullptr,
                        "ternary.default.live");
                    auto savedIP = compiler->builder->saveIP();
                    compiler->builder->SetInsertPoint(cflat_llvm::GetTerminatorOrNull(branchBlock));
                    compiler->builder->CreateStore(compiler->builder->getInt1(false), liveFlag);
                    compiler->builder->SetInsertPoint(cflat_llvm::GetTerminatorOrNull(defaultEnd));
                    compiler->builder->CreateStore(compiler->builder->getInt1(true), liveFlag);
                    compiler->builder->restoreIP(savedIP);
                    compiler->pendingOwnedStructTemps.push_back({ temp, typeName, branchBlock, liveFlag });
                }
            }
        if (!cxxRefBorrowJoin) compiler->PropagateAliasValue(trueValue, falseValue, phi);
        compiler->PropagateBondedValue(trueValue, falseValue, phi);
        if ((trueAlias || falseAlias) && !cxxRefBorrowJoin) compiler->RegisterAliasValue(phi);
        if (trueTempField || falseTempField) compiler->RegisterTempFieldValue(phi);
        // A `move` arm is explicit by parse shape: flag its read before the join copies it.
        if (ArmIsExplicitMove(expressionTrueCtx))
            compiler->MarkUniqueFieldReadExplicitMove(trueValue, nullptr, nullptr);
        if (ArmIsExplicitMove(expressionFalseCtx))
            compiler->MarkUniqueFieldReadExplicitMove(falseValue, nullptr, nullptr);
        trackGlobalUniquePointerArm(trueValue, trueStorage, trueShape);
        trackGlobalUniquePointerArm(falseValue, falseStorage, falseShape);
        compiler->PropagateUniqueFieldRead(trueValue, falseValue, phi);
        compiler->PropagateFatInterfaceJoin(trueValue, falseValue, phi);
        PropagateTernaryViewElement(ctx, trueValue, trueStorage, falseValue, falseStorage, phi);
        if (phi->getType()->isPointerTy() && pointerJoinDest.Pointer)
            compiler->RegisterValueElementTypeName(phi, pointerJoinDest.TypeName);
        if (inCxxCallArgument_ && phi->getType()->isPointerTy())
        {
            const bool trueNullptr = (trueShape.hasSourceType
                    && trueShape.sourceType.IsCxxNullptrT)
                || llvm::isa_and_nonnull<llvm::ConstantPointerNull>(trueValue);
            const bool falseNullptr = (falseShape.hasSourceType
                    && falseShape.sourceType.IsCxxNullptrT)
                || llvm::isa_and_nonnull<llvm::ConstantPointerNull>(falseValue);
            auto pointerArm = trueNullptr
                ? InferTernaryArmType(falseShape) : InferTernaryArmType(trueShape);
            if ((trueNullptr || falseNullptr) && pointerArm.Pointer)
                compiler->RegisterValueElementTypeName(phi, pointerArm.TypeName);
        }
        // An owned join is the phi itself (its arms were moved or copied in place); a plain join copies it.
        llvm::Value* resultValue = loweredJoined != nullptr ? loweredJoined
            : closureJoinOwnsArms ? phi : CloneTernaryClosureValue(phi, ctx);
        if (closureJoinOwnsArms) compiler->RegisterOwnedClosureTemp(phi);
        LLVMBackend::TypedValue result{ resultValue, joinUnsigned };
        // A join reading a container-owned element borrows it, whichever arm is the fresh one: the
        // `new` arm does not make the join owning, so the element arm's fact is carried through.
        JoinElementBorrow(compiler, result, trueShape, falseShape);
        JoinConditionalStackCharBufferBorrow(result, trueShape, falseShape);
        result.isAlias = (trueAlias || falseAlias) && !cxxRefBorrowJoin;
        // A join is an explicit move only when every arm is one: `cond ? move a : b` stays a borrow.
        result.isExplicitMove = trueShape.isExplicitMove && falseShape.isExplicitMove;
        // Either arm a data-pointer null cast makes the join one (refused at a function slot).
        result.isDataNullCast = trueShape.isDataNullCast || falseShape.isDataNullCast;
        result.storage = storageJoin != nullptr ? storageJoin : cxxLvalueStorageJoin;
        result.receiverStorage = cxxLvalueStorageJoin;
        result.isCxxPrvalueConditional = mixedCxxRecordCategories;
        // Both arms fresh (call results, sret temps) make the join fresh even when no record join is
        // visible here: C++ immovable operands are refused by place, so a prvalue join must stay one.
        // A lifted function arm is a fresh closure value, so the join is not assignable (T188).
        result.isRvalue = liftedArm || loweredJoined != nullptr || (cxxRecordJoin
            && (cxxLvalueStorageJoin == nullptr || bothOwnedCxxTempArms))
            || (trueShape.isRvalue && falseShape.isRvalue
                && !trueShape.isExplicitMove && !falseShape.isExplicitMove);
        // r3: a `?:` of narrow int arms keeps the wider arm mark (both arms narrow, one runtime).
        result.arithNarrowWidth = LLVMBackend::ArithResultNarrowWidth(trueShape, falseShape);
        JoinTernaryPointerShape(result, trueShape, falseShape);
        JoinTernaryClassPointee(result, trueShape, falseShape);
        if (compositeCxxPointee.Pointer && result.value != nullptr
            && result.value->getType()->isPointerTy())
        {
            result.joinClassPointee = pointerJoinDest.TypeName;
            result.joinClassPointeeDepth = 1;
        }
        auto moveArmIsConst = [&](const LLVMBackend::TypedValue& shape,
                                  antlr4::tree::ParseTree* arm) {
            if (shape.hasSourceType && CxxTernaryArmConst(shape.sourceType)) return true;
            return MoveOperandConst(TopLevelMoveExpression(arm));
        };
        // Same-class C++ arms: the joined object is const when either arm's object is const.
        if (cxxRecordJoin && trueShape.hasSourceType && falseShape.hasSourceType
            && !trueShape.sourceType.Pointer && !falseShape.sourceType.Pointer
            && trueShape.sourceType.TypeName == falseShape.sourceType.TypeName
            && (moveArmIsConst(trueShape, expressionTrueCtx)
                || moveArmIsConst(falseShape, expressionFalseCtx)))
        {
            result.sourceType = LLVMBackend::TypeAndValue{ .TypeName = trueShape.sourceType.TypeName };
            result.sourceType.ConstMask = 1u;
            result.sourceTypeName = result.sourceType.TypeName;
            result.hasSourceType = true;
        }
        else if (!constPrvalueJoinType.empty())
        {
            result.sourceType = LLVMBackend::TypeAndValue{ .TypeName = constPrvalueJoinType };
            result.sourceType.ConstMask = 1u;
            result.sourceTypeName = result.sourceType.TypeName;
            result.hasSourceType = true;
        }
        auto trueType = trueShape.hasSourceType ? trueShape.sourceType : InferTernaryArmType(trueValue);
        auto falseType = falseShape.hasSourceType ? falseShape.sourceType : InferTernaryArmType(falseValue);
        auto recoverFunctionNameType = [&](const LLVMBackend::TypedValue& shape,
                                           llvm::Value* value,
                                           LLVMBackend::TypeAndValue& type) {
            if (shape.hasSourceType && shape.sourceType.Pointer
                && !shape.sourceType.IsThinFnPtr()) return;
            auto* fn = llvm::dyn_cast_or_null<llvm::Function>(value);
            const auto* symbol = fn == nullptr ? nullptr : compiler->FindSymbolForFunction(fn);
            if (symbol == nullptr) return;
            auto overloadSet = compiler->functionTable.find(symbol->SourceName);
            if (overloadSet == compiler->functionTable.end() || overloadSet->second.size() != 1)
                return;
            type = {};
            type.TypeName = "__c_fn_ptr";
            type.IsFunctionPointer = true;
            type.FuncPtrReturnTypeName = symbol->ReturnType.TypeName;
            type.FuncPtrReturnPointer = symbol->ReturnType.Pointer;
            type.FuncPtrVariadic = symbol->Variadic;
            type.FuncPtrReturnPointerDepth = symbol->ReturnType.PointerDepth;
            for (const auto& parameter : symbol->Parameters)
            {
                LLVMBackend::TypeAndValue::FuncPtrParam fp;
                fp.TypeName = parameter.TypeName;
                fp.Pointer = parameter.Pointer;
                fp.IsArrayView = parameter.IsArrayView;
                fp.PointerDepth = parameter.PointerDepth;
                fp.IsMove = parameter.IsMove;
                fp.IsRvalueRef = parameter.IsRvalueRef;
                fp.ConstMask = parameter.FunctionParamConstMask();
                fp.IsCxxRefToPointer = parameter.IsCxxRefToPointer;
                if (fp.IsCxxRefToPointer && parameter.CxxConstRef())
                    fp.ConstMask |= static_cast<int64_t>(LLVMBackend::TypeAndValue::ConstBitAt(fp.PointerDepth));
                fp.IsPointeeConst = (fp.ConstMask & 1) != 0;
                fp.IsAlias = compiler->ParameterIsAliasByPointer(parameter);
                type.FuncPtrParams.push_back(std::move(fp));
            }
        };
        recoverFunctionNameType(trueShape, trueValue, trueType);
        recoverFunctionNameType(falseShape, falseValue, falseType);
        std::string trueSpelling;
        std::string falseSpelling;
        if (compiler->CxxThinFunctionPointerSpelling(trueType, trueSpelling)
            && compiler->CxxThinFunctionPointerSpelling(falseType, falseSpelling)
            && trueSpelling == falseSpelling)
        {
            result.sourceType = std::move(trueType);
            result.sourceTypeName = result.sourceType.TypeName;
            result.hasSourceType = true;
        }
        else
        {
            // A nullptr arm takes its thin function-pointer partner's type (C++ composite pointer type).
            auto nullArm = [](const LLVMBackend::TypedValue& shape, llvm::Value* value) {
                return llvm::isa_and_nonnull<llvm::ConstantPointerNull>(value)
                    || (shape.hasSourceType && shape.sourceType.IsCxxNullptrT);
            };
            const bool trueNull = nullArm(trueShape, trueValue);
            const bool falseNull = nullArm(falseShape, falseValue);
            std::string spelling;
            if (trueNull != falseNull
                && compiler->CxxThinFunctionPointerSpelling(trueNull ? falseType : trueType, spelling))
            {
                result.sourceType = trueNull ? std::move(falseType) : std::move(trueType);
                result.sourceTypeName = result.sourceType.TypeName;
                result.hasSourceType = true;
            }
        }
        CarryCommonFunctionSignature(compiler, result, trueShape, falseShape);
        if (resultValue != nullptr
            && (resultValue->getType()->isIntegerTy()
                || resultValue->getType()->isFloatingPointTy()))
        {
            auto sourceType = [&](const LLVMBackend::TypedValue& shape, llvm::Value* value,
                                  antlr4::ParserRuleContext* arm) {
                if (inCxxCallArgument_)
                {
                    const std::string literal = LLVMBackend::LiteralIdentityForOverload(arm->getText());
                    if (!literal.empty())
                    {
                        LLVMBackend::TypeAndValue type;
                        type.TypeName = literal;
                        return type;
                    }
                    // C++ arithmetic identity can differ from the CFlat operand type of a literal.
                    if (!shape.cxxArithIdentity.empty())
                    {
                        LLVMBackend::TypeAndValue type;
                        type.TypeName = shape.cxxArithIdentity;
                        return type;
                    }
                }
                if (shape.hasSourceType && !shape.sourceType.TypeName.empty()) return shape.sourceType;
                if (!shape.sourceTypeName.empty())
                {
                    LLVMBackend::TypeAndValue type;
                    type.TypeName = shape.sourceTypeName;
                    return type;
                }
                return InferTernaryArmType(value);
            };
            auto left = sourceType(trueShape, trueValue, expressionTrueCtx);
            auto right = sourceType(falseShape, falseValue, expressionFalseCtx);
            auto isEnum = [&](const LLVMBackend::TypeAndValue& type) {
                return !compiler->ResolveEnumTypeName(type.TypeName).empty();
            };
            LLVMBackend::TypeAndValue common;
            // Same-type arms keep their type with no promotion ([expr.cond]/7), lvalues included.
            if (!left.TypeName.empty() && left.TypeName == right.TypeName)
                common = left;
            else
            {
                if (isEnum(left))
                {
                    const std::string enumKey = compiler->ResolveEnumTypeName(left.TypeName);
                    if (compiler->IsScopedEnumTypeName(enumKey)) left.TypeName.clear();
                    else if (auto promoted = compiler->enumPromotedTypes_.find(enumKey);
                             promoted != compiler->enumPromotedTypes_.end())
                        left.TypeName = promoted->second;
                    else left.TypeName = compiler->GetEnumBackingType(enumKey);
                }
                if (isEnum(right))
                {
                    const std::string enumKey = compiler->ResolveEnumTypeName(right.TypeName);
                    if (compiler->IsScopedEnumTypeName(enumKey)) right.TypeName.clear();
                    else if (auto promoted = compiler->enumPromotedTypes_.find(enumKey);
                             promoted != compiler->enumPromotedTypes_.end())
                        right.TypeName = promoted->second;
                    else right.TypeName = compiler->GetEnumBackingType(enumKey);
                }
                auto promoted = [](std::string type) {
                    if (type == "bool" || type == "char" || type == "i8" || type == "u8"
                        || type == "short" || type == "i16" || type == "u16"
                        || type == "c8" || type == "c16" || type == "c32" || type == "wchar")
                        return std::string("int");
                    if (type == "i32") return std::string("int");
                    return type;
                };
                std::string a = promoted(left.TypeName), b = promoted(right.TypeName);
                auto floatRank = [](const std::string& type) {
                    return type == "longdouble" ? 3 : type == "double" ? 2 : type == "float" ? 1 : 0;
                };
                auto integer = [](const std::string& type) {
                    if (type == "int") return std::pair(3, false);
                    if (type == "u32" || type == "uint") return std::pair(3, true);
                    if (type == "long") return std::pair(4, false);
                    if (type == "ulong") return std::pair(4, true);
                    if (type == "i64") return std::pair(5, false);
                    if (type == "u64") return std::pair(5, true);
                    return std::pair(0, false);
                };
                if (!a.empty() && !b.empty() && (floatRank(a) || floatRank(b)))
                    common.TypeName = floatRank(a) >= floatRank(b) ? a : b;
                else
                {
                    auto [arank, aunsigned] = integer(a);
                    auto [brank, bunsigned] = integer(b);
                    if (arank != 0 && brank != 0)
                    {
                        if (a == b) common.TypeName = a;
                        else if (aunsigned == bunsigned)
                            common.TypeName = arank >= brank ? a : b;
                        else
                        {
                            const std::string& unsignedType = aunsigned ? a : b;
                            const std::string& signedType = aunsigned ? b : a;
                            const int urank = integer(unsignedType).first;
                            const int srank = integer(signedType).first;
                            LLVMBackend::TypeAndValue signedTv;
                            signedTv.TypeName = signedType;
                            LLVMBackend::TypeAndValue unsignedTv;
                            unsignedTv.TypeName = unsignedType;
                            llvm::Type* signedLlvm = compiler->GetType(signedTv);
                            llvm::Type* unsignedLlvm = compiler->GetType(unsignedTv);
                            if (urank >= srank) common.TypeName = unsignedType;
                            else if (signedLlvm != nullptr && unsignedLlvm != nullptr
                                     && signedLlvm->getIntegerBitWidth()
                                         > unsignedLlvm->getIntegerBitWidth())
                                common.TypeName = signedType;
                            else
                                common.TypeName = signedType == "int" ? "u32"
                                    : signedType == "long" ? "ulong" : "u64";
                        }
                    }
                }
            }
            if (!common.TypeName.empty())
            {
                llvm::Type* commonLlvmType = compiler->GetType(common);
                if (commonLlvmType != nullptr && commonLlvmType != resultValue->getType())
                    resultValue = compiler->CreateCast(resultValue, commonLlvmType, !result.isUnsigned);
                result.value = resultValue;
                result.isUnsigned = common.IsUnsignedInteger() != -1;
                result.sourceType = common;
                result.sourceTypeName = common.TypeName;
                result.hasSourceType = true;
            }
        }
        auto collectConditionalMoveSources = [&](const LLVMBackend::TypedValue& shape,
                                                 llvm::Value* value, llvm::Value* storage,
                                                 llvm::BasicBlock* armBlock,
                                                 antlr4::ParserRuleContext* armCtx) {
            auto markThisSources = [&](size_t first) {
                if (!shape.sourceRootIsBorrowedThisField || shape.isExplicitMove) return;
                // Only an owning arm (unique / owning value) is a this-field consume (T447).
                const auto& armType = shape.sourceType;
                if (!shape.hasSourceType || !(armType.IsUnique || compiler->IsCoreUniqueType(armType.TypeName)
                        || (!armType.Pointer && compiler->IsOwningValueType(armType.TypeName))))
                    return;
                for (size_t i = first; i < result.conditionalMoveSources.size(); ++i)
                {
                    result.conditionalMoveSources[i].RootIsBorrowedThisField = true;
                    result.conditionalMoveSources[i].ThisFieldPath = shape.sourceThisFieldPath;
                }
            };
            if (storage == nullptr) storage = shape.receiverStorage;
            if (!shape.conditionalMoveSources.empty())
            {
                result.conditionalMoveSources.insert(result.conditionalMoveSources.end(),
                    shape.conditionalMoveSources.begin(), shape.conditionalMoveSources.end());
                return;
            }
            std::string sourceName = shape.sourceCallerName;
            if (sourceName.empty() && storage != nullptr)
                sourceName = compiler->FindVariableNameByStorage(storage);
            if (sourceName.empty() || storage == nullptr || value == nullptr)
                return;
            std::string typeName = shape.sourceTypeName;
            if (typeName.empty())
                if (auto* st = llvm::dyn_cast<llvm::StructType>(value->getType());
                    st != nullptr && st->hasName()) typeName = st->getName().str();
            if (CollectRangeElementArm(shape, value, storage, armBlock, result.conditionalMoveSources))
                return;
            size_t firstThisSource = result.conditionalMoveSources.size();
            if (CollectBorrowedPointerArm(shape, value, storage, armBlock,
                    result.conditionalMoveSources))
            {
                markThisSources(firstThisSource);
                return;
            }
            firstThisSource = result.conditionalMoveSources.size();
            if (CollectOwningLocalPointerArm(shape, value, storage, armBlock, armCtx,
                    result.conditionalMoveSources))
            {
                markThisSources(firstThisSource);
                return;
            }
            if (inCallArgument_ && value->getType()->isPointerTy()
                && (!shape.sourceMovePathKey.empty()
                    || (!shape.sourceFieldName.empty() && !shape.sourceIsElementAccess)))
            {
                auto* fieldAddr = llvm::dyn_cast<llvm::GetElementPtrInst>(storage);
                if (fieldAddr != nullptr && compiler->IsOwningFieldAddress(fieldAddr))
                {
                    result.conditionalMoveSources.push_back({ storage, value->getType(), armBlock,
                        sourceName, shape.sourceFieldName, false });
                    result.conditionalMoveSources.back().MovePathKey = shape.sourceMovePathKey;
                    result.conditionalMoveSources.back().MovePathHasDynamicIndex =
                        shape.sourceMovePathHasDynamicIndex;
                    result.conditionalMoveSources.back().PointeeMove = shape.sourceIsPointeeMove;
                    result.conditionalMoveSources.back().MoveParamSinkArgument = true;
                    result.conditionalMoveSources.back().IsExplicitMove = shape.isExplicitMove;
                    markThisSources(result.conditionalMoveSources.size() - 1);
                    return;
                }
            }
            const bool coreUniqueSource = compiler->IsCoreUniqueType(typeName);
            if (typeName.empty() || (!compiler->IsOwningValueType(typeName)
                    && !coreUniqueSource && !shape.isExplicitMove)
                || (!coreUniqueSource && compiler->IsCopyableType(typeName)
                    && !shape.isExplicitMove)) return;
            result.conditionalMoveSources.push_back({ storage, value->getType(), armBlock,
                sourceName, shape.sourceFieldName, shape.sourceIsElementAccess });
            result.conditionalMoveSources.back().MovePathKey = shape.sourceMovePathKey;
            result.conditionalMoveSources.back().MovePathHasDynamicIndex =
                shape.sourceMovePathHasDynamicIndex;
            result.conditionalMoveSources.back().PointeeMove = shape.sourceIsPointeeMove;
            result.conditionalMoveSources.back().MoveParamSinkArgument = inCallArgument_;
            result.conditionalMoveSources.back().IsExplicitMove = shape.isExplicitMove;
            markThisSources(result.conditionalMoveSources.size() - 1);
        };
        collectConditionalMoveSources(trueShape, trueValue, trueStorage, trueEnd,
                                      expressionTrueCtx);
        collectConditionalMoveSources(falseShape, falseValue, falseStorage, falseEnd,
                                      expressionFalseCtx);
        if (!deferredPointerJoinDiagnostic.empty())
        {
            auto borrowed = std::find_if(result.conditionalMoveSources.begin(),
                result.conditionalMoveSources.end(), [](const auto& source) {
                    return source.BorrowedPointer;
                });
            if (borrowed != result.conditionalMoveSources.end())
                borrowed->DeferredDiagnostic = deferredPointerJoinDiagnostic;
            else
            {
                result.conditionalMoveSources.push_back({});
                result.conditionalMoveSources.back().MoveParamSinkArgument = true;
                result.conditionalMoveSources.back().BorrowedPointer = true;
                result.conditionalMoveSources.back().DeferredDiagnostic =
                    deferredPointerJoinDiagnostic;
            }
        }
        if (sharedCxxRetTemp != nullptr && cxxLvalueStorageJoin == sharedCxxRetTemp)
        {
            compiler->lastCxxRetTemp_ = sharedCxxRetTemp;
            compiler->lastCxxRetValue_ = resultValue;
        }
        if (sharedLoweredRetTemp != nullptr && resultValue != nullptr && result.storage == nullptr)
        {
            compiler->lastLoweredRetTemp_ = sharedLoweredRetTemp;
            compiler->lastLoweredRetValue_ = resultValue;
            compiler->nullConditionalTempResults_.push_back(resultValue);
        }
        compiler->lastCallIsBonded = trueBonded || falseBonded;
        compiler->lastCallBondedSources = std::move(ternaryBondedSources);
        consumeNewCxxTernaryElision(result);
        return result;
    }

LLVM_ATTRIBUTE_NOINLINE static bool LowerCxxReferenceJoinArmToPointer(
        LLVMBackend* compiler, llvm::Value*& value, llvm::Value*& storage,
        LLVMBackend::TypedValue& shape, const LLVMBackend::TypeAndValue& destination,
        const char* destinationDescription) {
        if (compiler == nullptr || value == nullptr || storage == nullptr || !destination.Pointer
            || destination.PointerDepth > 1 || destination.ElemPointer || destination.IsArrayView
            || !compiler->IsCxxRecord(destination.TypeName))
            return false;
        auto* record = llvm::dyn_cast<llvm::StructType>(value->getType());
        if (record == nullptr || !record->hasName()
            || !compiler->IsCxxRecord(record->getName().str()))
            return false;

        LLVMBackend::NamedVariable source;
        source.TypeAndValue = shape.sourceType;
        source.Primary = value;
        source.BaseType = value->getType();
        source.Storage = storage;
        auto* address = compiler->CxxReferenceResultAsPointer(destination, source,
                                                               destinationDescription);
        if (address == nullptr) return false;
        const bool sourcePointeeConst = shape.sourceType.CxxConstRef();
        value = address;
        storage = nullptr;
        shape.value = value;
        shape.storage = nullptr;
        shape.receiverStorage = nullptr;
        shape.sourceType = destination;
        shape.hasSourceType = true;
        shape.sourceTypeName = destination.TypeName;
        shape.pointerDepth = destination.PointerDepth;
        shape.elemPointer = destination.ElemPointer;
        shape.isAlias = false;
        shape.isRvalue = true;
        shape.cxxRefValueType = nullptr;
        shape.cxxPointeeConst = sourcePointeeConst;
        return true;
}

LLVMBackend::TypedValue MainListener::ParseConditionalExpression(
        CFlatParser::ConditionalExpressionContext* ctx, ResultUse use) {
        RequireStackHeadroom(ctx);
        LLVMBackend::TypeAndValue outerExpected = declExpectedType;
        auto* compiler = Compiler(ctx);
        const bool uniquePointerDestination =
            (outerExpected.IsUnique && outerExpected.Pointer && !outerExpected.ElemPointer
                && !outerExpected.IsArrayView)
            || (compiler->IsCoreUniqueType(outerExpected.TypeName)
                && !outerExpected.Pointer && !outerExpected.IsArrayView
                && !compiler->IsInterfaceType(MangledGenericArgument(
                    *compiler, outerExpected.TypeName)));
        const bool uniqueInterfaceDestination = IsOwningUniqueInterfaceField(outerExpected);
        auto trackGlobalUniquePointerArm = [&](llvm::Value* value, llvm::Value* storage,
                                                const LLVMBackend::TypedValue& shape) {
            if (!uniquePointerDestination || value == nullptr || storage == nullptr
                || !shape.hasSourceType || !shape.sourceType.IsUnique
                || !shape.sourceType.Pointer || shape.isExplicitMove
                || !llvm::isa<llvm::GlobalVariable>(storage))
                return;
            LLVMBackend::NamedVariable source;
            source.Primary = value;
            source.Storage = storage;
            source.BaseType = value->getType();
            source.CallerName = compiler->FindVariableNameByStorage(storage);
            source.TypeAndValue = shape.sourceType;
            source.TypeAndValue.IsMove = false;
            source.IsOwning = true;
            compiler->RegisterUniqueFieldRead(value, storage, {}, source);
        };
        auto isFunctionValueDestination = [&](const LLVMBackend::TypeAndValue& destination) {
            if (!destination.FuncPtrReturnTypeName.empty()) return true;
            if (destination.Pointer) return false;
            const auto* encoded = compiler->GetEncodedClosureType(destination.TypeName);
            return encoded != nullptr && !encoded->FuncPtrReturnTypeName.empty();
        };
        const bool functionValueDestination = isFunctionValueDestination(outerExpected);
        auto parseFunctionDestinationConditionalArm = [&] (
                CFlatParser::ConditionalExpressionContext* arm,
                const LLVMBackend::TypeAndValue& destination) {
            if (isFunctionValueDestination(destination))
                if (auto* identifier = BareGenericFunctionValueIdentifierInExpression(arm))
                {
                    auto named = ParseIdentifier(identifier);
                    return TypedValueOfNamedOperand(named, arm);
                }
            if (auto* move = TopLevelMoveExpression(arm))
                return ParseTernaryMoveArm(move, arm);
            auto value = ParseConditionalExpression(arm, use);
            CarryNestedTernaryBorrow(compiler, value);
            if (isFunctionValueDestination(destination) && value.value != nullptr
                && value.value->getType()->isPointerTy()
                && compiler->IsNullFunctionSlotJoin(value.value))
                value.value = llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(value.value->getType()));
            return value;
        };
        if (ctx->expression() != nullptr && ctx->conditionalExpression() != nullptr
            && IsGenericFunctionScalarDestination(compiler, outerExpected))
        {
            auto rejectGenericArm = [&](antlr4::tree::ParseTree* arm) {
                auto* identifier = BareGenericFunctionValueIdentifierInExpression(arm);
                if (identifier == nullptr) return false;
                const std::string name = identifier->getText();
                if (NamesVisibleVariable(compiler, name)) return false;
                const std::string genericBase = compiler->ResolveGenericFunctionBase(name);
                if (!genericFunctionTemplates.count(genericBase)) return false;
                const std::string qualified = compiler->ResolveQualifiedName(name);
                if (compiler->GetFunction(name) != nullptr
                    || compiler->GetFunction(qualified) != nullptr)
                    return false;
                LogErrorContext(identifier, GenericFunctionScalarValueDiagnostic(compiler, name));
                return true;
            };
            if (rejectGenericArm(ctx->expression())
                || rejectGenericArm(ctx->conditionalExpression()))
                return {};
        }
        if (ctx->expression() != nullptr && ctx->conditionalExpression() != nullptr
            && !functionValueDestination
            && !IsGenericFunctionScalarDestination(compiler, outerExpected))
        {
            auto rejectUndeducedGenericArm = [&](antlr4::tree::ParseTree* arm) {
                auto* identifier = BareGenericFunctionValueIdentifierInExpression(arm);
                if (identifier == nullptr) return false;
                const std::string name = identifier->getText();
                if (LexicalLocalTypeShadowsValue(compiler, identifier, name)
                    || NamesVisibleVariable(compiler, name))
                    return false;
                const std::string genericBase = compiler->ResolveGenericFunctionBase(name);
                if (!genericFunctionTemplates.count(genericBase)) return false;
                LogErrorContext(identifier, std::format(
                    "generic function '{}' needs a function-type destination to deduce T", name));
                return true;
            };
            if (rejectUndeducedGenericArm(ctx->expression())
                || rejectUndeducedGenericArm(ctx->conditionalExpression()))
                return {};
        }
        std::optional<OwnershipControlScope> nullCoalesceControlScope;
        const bool autoDeclTernary = std::exchange(autoDeclTernaryInit_, false);
        DeclExpectedTypeGate declExpectedGate(&declExpectedType,
            ctx->children.size() == 1
                || (declExpectedType.Pointer && compiler->IsCxxRecord(declExpectedType.TypeName)));
        auto adjustCxxPointerArm = [&](llvm::Value*& value, const LLVMBackend::TypeAndValue& dest) {
            if (value == nullptr || !dest.Pointer || !value->getType()->isPointerTy()
                || !compiler->IsCxxRecord(dest.TypeName)) return;
            if (compiler->builder->GetInsertBlock() == nullptr) return;
            auto source = InferTernaryArmType(value);
            if (!source.Pointer || !compiler->IsCxxRecord(source.TypeName)) return;
            value = compiler->AdjustCxxPointerForStore(dest, source, value, "conditional expression");
        };
        auto carryCommonThinFunctionPointerType = [&](LLVMBackend::TypedValue& result,
                                                       const LLVMBackend::TypedValue& trueShape,
                                                       const LLVMBackend::TypedValue& falseShape,
                                                       llvm::Value* trueValue,
                                                       llvm::Value* falseValue) {
            auto sourceType = [&](const LLVMBackend::TypedValue& shape, llvm::Value* value) {
                auto type = shape.hasSourceType ? shape.sourceType : InferTernaryArmType(value);
                if (shape.hasSourceType && shape.sourceType.Pointer
                    && !shape.sourceType.IsThinFnPtr()) return type;
                auto* fn = llvm::dyn_cast_or_null<llvm::Function>(value);
                const auto* symbol = fn == nullptr ? nullptr : compiler->FindSymbolForFunction(fn);
                if (symbol == nullptr) return type;
                auto overloadSet = compiler->functionTable.find(symbol->SourceName);
                if (overloadSet == compiler->functionTable.end() || overloadSet->second.size() != 1)
                    return type;
                type = {};
                type.TypeName = "__c_fn_ptr";
                type.IsFunctionPointer = true;
                type.FuncPtrReturnTypeName = symbol->ReturnType.TypeName;
                type.FuncPtrReturnPointer = symbol->ReturnType.Pointer;
                type.FuncPtrVariadic = symbol->Variadic;
                type.FuncPtrReturnPointerDepth = symbol->ReturnType.PointerDepth;
                for (const auto& parameter : symbol->Parameters)
                {
                    LLVMBackend::TypeAndValue::FuncPtrParam fp;
                    fp.TypeName = parameter.TypeName;
                    fp.Pointer = parameter.Pointer;
                    fp.IsArrayView = parameter.IsArrayView;
                    fp.PointerDepth = parameter.PointerDepth;
                    fp.IsMove = parameter.IsMove;
                    fp.IsRvalueRef = parameter.IsRvalueRef;
                    fp.ConstMask = parameter.FunctionParamConstMask();
                    fp.IsCxxRefToPointer = parameter.IsCxxRefToPointer;
                    if (fp.IsCxxRefToPointer && parameter.CxxConstRef())
                        fp.ConstMask |= static_cast<int64_t>(LLVMBackend::TypeAndValue::ConstBitAt(fp.PointerDepth));
                    fp.IsPointeeConst = (fp.ConstMask & 1) != 0;
                    fp.IsAlias = compiler->ParameterIsAliasByPointer(parameter);
                    type.FuncPtrParams.push_back(std::move(fp));
                }
                return type;
            };
            auto left = sourceType(trueShape, trueValue);
            auto right = sourceType(falseShape, falseValue);
            auto hasRecordedConstMasks = [](const LLVMBackend::TypeAndValue& type) {
                return std::all_of(type.FuncPtrParams.begin(), type.FuncPtrParams.end(),
                    [](const auto& param) { return param.ConstMask >= 0; });
            };
            if (hasRecordedConstMasks(left) && hasRecordedConstMasks(right))
            {
                if (SameClosureSignature(left, right))
                {
                    result.sourceType = std::move(left);
                    result.sourceTypeName = result.sourceType.TypeName;
                    result.hasSourceType = true;
                }
                return;
            }
            std::string leftSpelling;
            std::string rightSpelling;
            if (compiler->CxxThinFunctionPointerSpelling(left, leftSpelling)
                && compiler->CxxThinFunctionPointerSpelling(right, rightSpelling)
                && leftSpelling == rightSpelling)
            {
                result.sourceType = std::move(left);
                result.sourceTypeName = result.sourceType.TypeName;
                result.hasSourceType = true;
            }
        };
        auto cxxPointerJoinDest = [&]() -> LLVMBackend::TypeAndValue {
            if (outerExpected.Pointer && compiler->IsCxxRecord(outerExpected.TypeName)
                && !inCallArgument_)
                return outerExpected;
            return {};
        };
        bool collapseLvalueArms = false;
        bool collapseLvalueSink = false;
        bool crossedCallArgument = false;
        for (auto* parent = ctx->parent; parent != nullptr; parent = parent->parent)
        {
            if (dynamic_cast<CFlatParser::ArgumentNamedExpressionContext*>(parent) != nullptr
                || dynamic_cast<CFlatParser::ArgumentExpressionListContext*>(parent) != nullptr)
            {
                crossedCallArgument = true;
            }
            if (dynamic_cast<CFlatParser::InitializerContext*>(parent) != nullptr)
            {
                collapseLvalueArms = !crossedCallArgument;
                break;
            }
            if (!crossedCallArgument)
                if (auto* assignment = dynamic_cast<CFlatParser::AssignmentExpressionContext*>(parent);
                    HasAssignmentOperator(assignment))
                {
                    collapseLvalueSink = true;
                    break;
                }
        }
        auto logicCtx = ctx->logicalOrExpression();

        if (ctx->QuestionQuestion())
        {
            auto isWholeJoinMove = [&]() {
                auto* node = static_cast<antlr4::tree::ParseTree*>(ctx);
                while (node != nullptr && node->parent != nullptr)
                {
                    auto* parent = node->parent;
                    if (dynamic_cast<CFlatParser::MoveExpressionContext*>(parent) != nullptr)
                        return true;
                    if (parent->children.size() == 1
                        || (dynamic_cast<CFlatParser::PrimaryExpressionContext*>(parent) != nullptr
                            && static_cast<CFlatParser::PrimaryExpressionContext*>(parent)->expression() == node))
                    {
                        node = parent;
                        continue;
                    }
                    if (auto* conditional = dynamic_cast<CFlatParser::ConditionalExpressionContext*>(parent))
                    {
                        const bool coalesceOperand = conditional->QuestionQuestion() != nullptr
                            && (conditional->logicalOrExpression() == node
                                || conditional->conditionalExpression() == node);
                        const bool ternaryResultArm = conditional->Question() != nullptr
                            && (conditional->expression() == node
                                || conditional->conditionalExpression() == node);
                        if (coalesceOperand || ternaryResultArm)
                        {
                            node = parent;
                            continue;
                        }
                    }
                    return false;
                }
                return false;
            };
            const bool wholeJoinIsMoved = isWholeJoinMove();
            // Null-coalescing: lhs ?? rhs  ->  (lhs != null) ? lhs : rhs
            // Per-arm cast occurrence, exactly as ParseTernaryBranches scopes its two arms.
            llvm::Value* lhs = nullptr;
            LLVMBackend::TypedValue lhsTv;
            size_t lhsOcc = compiler->CurrentCastOccurrence();
            {
                LLVMBackend::CastOccurrenceScope armScope(compiler);
                lhsOcc = armScope.Id;
                lhsTv = ParseLogicalOrExpression(logicCtx, ResultUse::Value);
                CarryNestedTernaryBorrow(compiler, lhsTv);
                lhs = lhsTv.value;
            }
            if (!lhs) return {};
            // A `move` arm is explicit by parse shape: mark its join source before the join is built.
            if (ArmIsExplicitMove(logicCtx))
                compiler->MarkUniqueFieldReadExplicitMove(lhs, lhsTv.receiverStorage,
                    compiler->builder->GetInsertBlock());
            auto pointerJoinDest = cxxPointerJoinDest();
            adjustCxxPointerArm(lhs, pointerJoinDest);
            const auto coalesceLeftExpected = InferTernaryArmType(lhsTv);
            const auto coalesceFunctionExpected = isFunctionValueDestination(
                coalesceLeftExpected) ? coalesceLeftExpected : outerExpected;
            const bool coalesceHasFunctionExpected = isFunctionValueDestination(
                coalesceFunctionExpected);
            llvm::Value* lhsStorage = lhsTv.receiverStorage;
            const bool lhsUniqueFieldRead = compiler->IsUniqueFieldReadValue(lhs);
            auto clearUniqueFieldRead = [&](llvm::Value* value, llvm::Value* storage) {
                if (value == nullptr || storage == nullptr) return;
                if (value->getType()->isPointerTy())
                    compiler->builder->CreateStore(
                        llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(value->getType())),
                        storage);
                else if (value->getType()->isStructTy())
                    compiler->builder->CreateStore(
                        llvm::ConstantAggregateZero::get(value->getType()), storage);
            };

            llvm::Value* conditionValue = lhs;
            auto lhsType = InferTernaryArmType(lhs);
            if (!outerExpected.Pointer && compiler->IsCoreUniqueType(lhsType.TypeName))
            {
                LLVMBackend::NamedVariable uniqueValue;
                uniqueValue.Primary = lhs;
                uniqueValue.BaseType = lhs->getType();
                uniqueValue.TypeAndValue.TypeName = lhsType.TypeName;
                uniqueValue.TypeAndValue.VariableName.clear();
                if (auto* valid = compiler->CreateOverloadedFunctionCall("valid", { uniqueValue }))
                    conditionValue = valid;
            }

            // A move-interface return must release each unique<T> arm before the join is
            // materialized. Otherwise the wrapper dtors run at function exit and leave the
            // returned interface pointing at freed storage. Keep the join raw while the two
            // branch-local releases are emitted; the return path boxes it afterward.
            bool moveUniquePointerJoin = compiler->currentFunctionReturnsOwned
                && compiler->currentFunctionReturnTV.IsInterface;
            if (moveUniquePointerJoin)
            {
                auto* lhsStruct = llvm::dyn_cast<llvm::StructType>(lhs->getType());
                moveUniquePointerJoin = lhsStruct != nullptr && lhsStruct->hasName()
                    && compiler->IsCoreUniqueType(lhsStruct->getName().str());
                if (moveUniquePointerJoin)
                {
                    const auto& uniqueData = compiler->GetDataStructure(lhsStruct->getName().str());
                    moveUniquePointerJoin = std::ranges::any_of(uniqueData.StructFields,
                        [](const auto& field) { return field.VariableName == "_p"; });
                }
            }
            // An owning unique LOCAL or FIELD lhs joins as a raw pointer, like a '?:' arm; the
            // destination nulls its slot on the taken path only, a raw destination borrows it.
            // A unique struct value (not an interface) with a storage slot and no explicit move.
            auto ownedUniqueShape = [&](llvm::Value* value, llvm::Value* storage,
                                        const LLVMBackend::TypedValue& shape) {
                auto* st = llvm::dyn_cast_or_null<llvm::StructType>(
                    value == nullptr ? nullptr : value->getType());
                if (st == nullptr || !st->hasName() || !compiler->IsCoreUniqueType(st->getName().str())
                    || storage == nullptr || shape.isExplicitMove)
                    return false;
                // An interface unique<I> keeps its struct shape (a named interface argument).
                return !compiler->IsInterfaceType(MangledGenericArgument(*compiler, st->getName().str()));
            };
            // Shared by both arms: a unique struct value whose storage is an owning local.
            auto owningUniqueLocal = [&](llvm::Value* value, llvm::Value* storage,
                                         const LLVMBackend::TypedValue& shape) {
                if (!ownedUniqueShape(value, storage, shape)) return false;
                const std::string localName = compiler->FindVariableNameByStorage(storage);
                const auto* local = compiler->FindVariableByStorage(storage);
                return local != nullptr && !localName.empty() && localName.find('.') == std::string::npos
                    && !local->IsBorrowed && !local->IsAliasBorrow && !local->TypeAndValue.IsAlias;
            };
            // An owning unique FIELD lhs (`o.p`) takes the same raw arm: only the taken path nulls it.
            // A field reached through a borrowed `this` keeps the struct join (T466 pins its sink).
            const bool uniqueFieldJoin = !moveUniquePointerJoin && lhsUniqueFieldRead
                && !lhsTv.sourceRootIsBorrowedThisField && ownedUniqueShape(lhs, lhsStorage, lhsTv);
            const bool localUniqueJoin = !moveUniquePointerJoin && !lhsUniqueFieldRead
                && owningUniqueLocal(lhs, lhsStorage, lhsTv);
            const bool globalUniqueJoin = uniquePointerDestination && !lhsUniqueFieldRead
                && ownedUniqueShape(lhs, lhsStorage, lhsTv)
                && llvm::isa_and_nonnull<llvm::GlobalVariable>(lhsStorage);
            const bool rawUniqueJoin = uniqueFieldJoin || localUniqueJoin || globalUniqueJoin;
            llvm::Type* resultType = lhs->getType();
            if (moveUniquePointerJoin || rawUniqueJoin)
            {
                auto* lhsStruct = llvm::cast<llvm::StructType>(lhs->getType());
                const auto& uniqueData = compiler->GetDataStructure(lhsStruct->getName().str());
                for (const auto& field : uniqueData.StructFields)
                    if (field.VariableName == "_p")
                    {
                        if (auto* rawType = compiler->GetType(field)) resultType = rawType;
                        break;
                    }
            }
            auto* resultAlloca = compiler->CreateAlloca(resultType);
            auto* rawCountAlloca = compiler->CreateAlloca(compiler->builder->getInt64Ty());
            TernaryArrayCountState lhsCount;
            TernaryArrayCountState rhsCount;

            auto* nullBlock = compiler->CreateBasicBlock("nullcoal_null");
            auto* notNullBlock = compiler->CreateBasicBlock("nullcoal_notnull");
            auto* resumeBlock = compiler->CreateBasicBlock("nullcoal_resume");
            // Hidden owner of a runtime fallback (LLVMBackend::NullCoalesceFallback); empty unless
            // the RHS is a bare `new` (or an inner '??' that owns one) and the LHS is not provably null.
            LLVMBackend::NullCoalesceFallback fallbackOwner;
            bool rhsOwnsFallback = false;

            // A bare `new` LHS runs on both paths; ledger it here so the after-call cleanup frees it.
            if (inCallArgument_ && compiler->IsOwnedNewTemp(lhs))
                compiler->RegisterOwnedPtrTemp(lhs);
            compiler->CreateConditionJump(conditionValue, notNullBlock, nullBlock);
            // insert point is now notNullBlock (lhs is not null)
            if (moveUniquePointerJoin)
            {
                auto* lhsStruct = llvm::dyn_cast<llvm::StructType>(lhs->getType());
                if (lhsStruct != nullptr && lhsStruct->hasName())
                {
                    LLVMBackend::NamedVariable receiver;
                    receiver.Primary = lhs;
                    receiver.Storage = lhsStorage;
                    receiver.BaseType = lhsStruct;
                    receiver.TypeAndValue.TypeName = lhsStruct->getName().str();
                    receiver.TypeAndValue.IsMove = true;
                    receiver.IsExplicitMove = true;
                    receiver.IsOwning = true;
                    LLVMBackend::TypeAndValue rawType;
                    rawType.TypeName = MangledGenericArgument(
                        *compiler, receiver.TypeAndValue.TypeName);
                    rawType.Pointer = true;
                    rawType.IsMove = true;
                    lhs = compiler->CreateCoreUniqueRawPointerCall(receiver, rawType);
                    lhsStorage = nullptr;
                }
            }
            if (rawUniqueJoin)
            {
                // Raw `get` arm: the destination's implicit move nulls the slot on this path. A field
                // arm keeps its read's null key, so a deref after the join is refused as maybe-moved.
                const std::string lhsNullKey = compiler->UniqueFieldReadNullKey(lhs);
                auto* lhsStruct = llvm::cast<llvm::StructType>(lhs->getType());
                LLVMBackend::NamedVariable receiver;
                receiver.Primary = lhs;
                receiver.Storage = lhsStorage;
                receiver.BaseType = lhsStruct;
                receiver.TypeAndValue.TypeName = lhsStruct->getName().str();
                LLVMBackend::TypeAndValue rawType;
                rawType.TypeName = MangledGenericArgument(*compiler, receiver.TypeAndValue.TypeName);
                rawType.Pointer = true;
                lhs = compiler->CreateCoreUniqueRawPointerCall(receiver, rawType);
                if (globalUniqueJoin)
                {
                    LLVMBackend::NamedVariable source;
                    source.Primary = lhs;
                    source.Storage = lhsStorage;
                    source.BaseType = lhs->getType();
                    source.CallerName = compiler->FindVariableNameByStorage(lhsStorage);
                    source.TypeAndValue.TypeName = rawType.TypeName;
                    source.TypeAndValue.Pointer = true;
                    source.TypeAndValue.IsUnique = true;
                    compiler->RegisterUniqueFieldRead(lhs, lhsStorage, lhsNullKey, source);
                }
                else
                    compiler->RegisterUniqueFieldRead(lhs, lhsStorage, lhsNullKey);
            }
            // A named-function LHS becomes its thin pointer before the join store (thin result only).
            if (resultAlloca->getAllocatedType()->isPointerTy())
                lhs = compiler->BareFunctionToThinValue(lhs, nullptr);
            compiler->CreateAssignment(lhs, resultAlloca);
            // Clear here only for an owning destination; borrowed results retain the field's owner.
            if (lhsUniqueFieldRead && !rawUniqueJoin
                && (uniquePointerDestination || uniqueInterfaceDestination || wholeJoinIsMoved))
                clearUniqueFieldRead(lhs, lhsStorage);
            lhsCount = GetTernaryArrayCountState(lhs, lhsStorage, lhsTv);
            compiler->builder->CreateStore(
                lhsCount.Available && lhsCount.Count != nullptr
                    ? compiler->Upconvert(lhsCount.Count, compiler->builder->getInt64Ty())
                    : compiler->builder->getInt64(-1),
                rawCountAlloca);
            auto* lhsBr = compiler->CreateJump(resumeBlock);

            compiler->SwitchToBlock(nullBlock);
            nullCoalesceControlScope.emplace(compiler);
            llvm::Value* rhs = nullptr;
            llvm::Value* rhsStorage = nullptr;
            LLVMBackend::TypedValue rhsShape;
            bool lhsAlias = compiler->IsAliasValue(lhs);
            bool rhsAlias = false;
            bool rhsCxxReferencePointer = false;
            bool lhsTempField = compiler->IsTempFieldValue(lhs);
            bool rhsTempField = false;
            bool rhsUniqueFieldRead = false;
            size_t rhsOcc = compiler->CurrentCastOccurrence();
            LLVMBackend::OwnedTempMark rhsMark = compiler->MarkOwnedTemps();
            bool rhsAddrIntoTemp = false;
            llvm::UncondBrInst* rhsBr = nullptr;
            try
            {
                BranchPostfixScope rhsPostfix(this);  // the RHS runs only when the LHS is null
                {
                    LLVMBackend::CastOccurrenceScope armScope(compiler);
                    rhsOcc = armScope.Id;
                    // Only a bare `default` rhs needs a published type; any other rhs keeps the
                    // withdrawn context, exactly as the '?:' arms do.
                    std::optional<DeclExpectedTypeScope> expectedScope;
                    if (IsDefaultOnlyExpression(ctx->conditionalExpression()))
                    {
                        auto rhsExpected = coalesceLeftExpected;
                        if (rhsExpected.TypeName.empty()) rhsExpected = outerExpected;
                        if (!rhsExpected.TypeName.empty())
                            expectedScope.emplace(&declExpectedType, rhsExpected,
                                isFunctionValueDestination(rhsExpected));
                    }
                    else if (coalesceHasFunctionExpected)
                        expectedScope.emplace(&declExpectedType, coalesceFunctionExpected, true);
                    auto rhsTv = parseFunctionDestinationConditionalArm(
                        ctx->conditionalExpression(), coalesceFunctionExpected);
                    rhsShape = rhsTv;
                    rhs = rhsTv.value;
                    rhsStorage = rhsTv.receiverStorage;
                    if (ArmIsExplicitMove(ctx->conditionalExpression()))
                        compiler->MarkUniqueFieldReadExplicitMove(rhs, rhsStorage,
                            compiler->builder->GetInsertBlock());
                    rhsCxxReferencePointer = LowerCxxReferenceJoinArmToPointer(compiler, rhs, rhsStorage,
                        rhsShape, coalesceLeftExpected, "null-coalescing expression");
                    auto lhsJoinType = InferTernaryArmType(lhsTv);
                    auto rhsJoinType = InferTernaryArmType(rhsShape);
                    const bool lhsNullArm = llvm::isa_and_nonnull<llvm::ConstantPointerNull>(lhs)
                        || (lhsTv.hasSourceType && lhsTv.sourceType.IsCxxNullptrT);
                    const bool rhsNullArm = llvm::isa_and_nonnull<llvm::ConstantPointerNull>(rhs)
                        || (rhsShape.hasSourceType && rhsShape.sourceType.IsCxxNullptrT);
                    const std::string interfaceDestination =
                        TernaryInterfaceDestination(*compiler, outerExpected);
                    if (!lhsNullArm && !rhsNullArm
                        && UnrelatedPointerJoinTypes(compiler, lhsJoinType, rhsJoinType))
                    {
                        const std::string lhsClass = compiler->ResolveTypeAlias(lhsJoinType.TypeName);
                        const std::string rhsClass = compiler->ResolveTypeAlias(rhsJoinType.TypeName);
                        const bool shareInterface = ClassesShareInterface(lhsClass, rhsClass);
                        const bool canBox = !interfaceDestination.empty()
                            ? compiler->StructImplementsInterface(lhsClass, interfaceDestination)
                                && compiler->StructImplementsInterface(rhsClass, interfaceDestination)
                            : inCallArgument_ && shareInterface;
                        if (!canBox)
                        {
                            const bool deferToParent = IsNestedConditionalJoinArm(ctx)
                                && !outerExpected.Pointer && outerExpected.PointerDepth == 0
                                && !outerExpected.ElemPointer && shareInterface;
                            if (!deferToParent)
                                LogErrorContext(ctx, UnrelatedPointerJoinDiagnostic(
                                    compiler, lhsJoinType, rhsJoinType, "??"));
                        }
                    }
                    else if (!inCallArgument_
                             && (outerExpected.Pointer || outerExpected.PointerDepth > 0)
                             && interfaceDestination.empty())
                    {
                        auto isJoinValue = [&](llvm::Value* value) {
                            return llvm::isa_and_nonnull<llvm::PHINode>(value)
                                || llvm::isa_and_nonnull<llvm::SelectInst>(value)
                                || compiler->FindNullCoalesceJoin(value) != nullptr;
                        };
                        if (isJoinValue(lhs) || isJoinValue(rhs))
                        {
                            std::vector<std::string> leafClasses;
                            auto collect = [&](llvm::Value* value,
                                               const LLVMBackend::TypeAndValue& named) {
                                if (isJoinValue(value))
                                    return CollectJoinArmClasses(value, leafClasses);
                                if (llvm::isa_and_nonnull<llvm::ConstantPointerNull>(value))
                                    return true;
                                const std::string name =
                                    compiler->ResolveTypeAlias(named.TypeName);
                                if (!named.Pointer || named.PointerDepth > 1 || named.ElemPointer
                                    || !compiler->IsDataStructure(name))
                                    return false;
                                leafClasses.push_back(name);
                                return true;
                            };
                            if (collect(lhs, lhsJoinType) && collect(rhs, rhsJoinType))
                            {
                                std::vector<std::string> distinct;
                                for (const auto& cls : leafClasses)
                                    if (std::find(distinct.begin(), distinct.end(), cls)
                                        == distinct.end())
                                        distinct.push_back(cls);
                                if (distinct.size() >= 2
                                    && ClassesShareInterface(distinct[0], distinct[1]))
                                {
                                    auto pointerType = [](const std::string& cls) {
                                        return LLVMBackend::TypeAndValue{
                                            .TypeName = cls, .Pointer = true, .PointerDepth = 1 };
                                    };
                                    LogErrorContext(ctx, UnrelatedPointerJoinDiagnostic(
                                        compiler, pointerType(distinct[0]),
                                        pointerType(distinct[1]), "??"));
                                }
                            }
                        }
                    }
                    adjustCxxPointerArm(rhs, pointerJoinDest);
                    rhsUniqueFieldRead = compiler->IsUniqueFieldReadValue(rhs);
                    rhsAlias = !rhsCxxReferencePointer && compiler->IsAliasValue(rhs);
                    rhsTempField = compiler->IsTempFieldValue(rhs);
                }
                // An owning unique LOCAL rhs joins like the lhs local: its slot is a join source.
                const bool rhsOwnedLocal = !moveUniquePointerJoin && !rhsUniqueFieldRead
                    && owningUniqueLocal(rhs, rhsStorage, rhsShape);
                const bool rhsOwnedGlobal = uniquePointerDestination && !rhsUniqueFieldRead
                    && ownedUniqueShape(rhs, rhsStorage, rhsShape)
                    && llvm::isa_and_nonnull<llvm::GlobalVariable>(rhsStorage);
                // A raw-pointer coalesce result observes a desugared unique field on its RHS.
                if (rhs != nullptr && resultAlloca->getAllocatedType()->isPointerTy())
                {
                    auto* rhsStruct = llvm::dyn_cast<llvm::StructType>(rhs->getType());
                    if (rhsStruct != nullptr && rhsStruct->hasName()
                        && compiler->IsCoreUniqueType(rhsStruct->getName().str()))
                    {
                        const std::string rhsNullKey = compiler->UniqueFieldReadNullKey(rhs);
                        LLVMBackend::NamedVariable receiver;
                        receiver.Primary = rhs;
                        receiver.Storage = rhsStorage;
                        receiver.BaseType = rhs->getType();
                        receiver.TypeAndValue.TypeName = rhsStruct->getName().str();
                        receiver.TypeAndValue.IsMove = moveUniquePointerJoin;
                        receiver.IsExplicitMove = moveUniquePointerJoin;
                        receiver.IsOwning = moveUniquePointerJoin;
                        LLVMBackend::TypeAndValue rawType;
                        rawType.TypeName = MangledGenericArgument(
                            *compiler, receiver.TypeAndValue.TypeName);
                        rawType.Pointer = true;
                        rawType.IsMove = moveUniquePointerJoin;
                        rhs = compiler->CreateCoreUniqueRawPointerCall(receiver, rawType);
                        if (rhsTempField)
                            compiler->RegisterOwningTempUniqueField(rhs);
                        // The raw read keeps the field slot as its source, so the join records it and
                        // the destination's implicit move (not this join) nulls it when it is consumed.
                        if (rhsUniqueFieldRead || rhsOwnedLocal || rhsOwnedGlobal)
                        {
                            if (rhsOwnedGlobal)
                            {
                                LLVMBackend::NamedVariable source;
                                source.Primary = rhs;
                                source.Storage = rhsStorage;
                                source.BaseType = rhs->getType();
                                source.CallerName = compiler->FindVariableNameByStorage(rhsStorage);
                                source.TypeAndValue.TypeName = rawType.TypeName;
                                source.TypeAndValue.Pointer = true;
                                source.TypeAndValue.IsUnique = true;
                                compiler->RegisterUniqueFieldRead(
                                    rhs, rhsStorage, rhsNullKey, source);
                            }
                            else
                                compiler->RegisterUniqueFieldRead(rhs, rhsStorage, rhsNullKey);
                        }
                    }
                }
                // A struct join (a unique field lhs) keeps the struct rhs: its slot is still the source.
                else if (rhsOwnedLocal || rhsOwnedGlobal)
                    compiler->RegisterUniqueFieldRead(rhs, rhsStorage);
                // `nullcoal_null` does not dominate the resume block, so the end-of-statement flush
                // would skip its temps; mirror FinishTernaryArm and keep the yielded value. The
                // branch block does dominate the resume, so struct temps hoist there instead.
                // The join below is a load off a slot, which the address walk cannot follow.
                rhsAddrIntoTemp = compiler->AddressIntoStatementPtrTemp(rhs);
                if (!inCallArgument_ && !moveUniquePointerJoin && rhs != nullptr && lhs != nullptr
                    && resultAlloca->getAllocatedType()->isPointerTy()
                    && lhs->getType()->isPointerTy() && rhs->getType()->isPointerTy()
                    && !compiler->IsRawArrayResult(lhs) && !compiler->IsRawArrayResult(rhs)
                    && !JoinArmIsProvablyNull(lhs))
                {
                    if (const auto* owned = compiler->FindOwnedNewTemp(rhs); owned != nullptr)
                    {
                        rhsOwnsFallback = true;
                        fallbackOwner.TypeName = owned->TypeName;
                        fallbackOwner.AllocAlign = owned->AllocAlign;
                        fallbackOwner.Slot = compiler->AllocaAtEntry(rhs->getType(), nullptr,
                                                                     "coalesce.fallback");
                    }
                    else if (const auto* inner = compiler->FindNullCoalesceJoin(rhs);
                             inner != nullptr && inner->Fallback.Slot != nullptr)
                        fallbackOwner = inner->Fallback;
                    if (fallbackOwner.Slot != nullptr)
                    {
                        // The LHS-taken path never runs the RHS, so the slot starts null on it.
                        llvm::IRBuilderBase::InsertPointGuard guard(*compiler->builder);
                        compiler->builder->SetInsertPoint(lhsBr);
                        compiler->builder->CreateStore(
                            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(rhs->getType())),
                            fallbackOwner.Slot);
                    }
                }
                // Address-derived RHS temps hoist outside calls too (`(c ? (n ?? &(new T())->v) : p)`);
                // a bare `new` RHS is ledgered only in a call argument.
                compiler->HoistOwnedPtrTempsForAddress(rhsMark, rhs,
                                                       nullBlock->getSinglePredecessor(),
                                                       /*includeBareNew*/ inCallArgument_);
                compiler->FlushOwnedTempsSince(rhsMark, rhs, nullBlock->getSinglePredecessor());
                auto lhsSourceType = InferTernaryArmType(lhsTv);
                auto rhsSourceType = InferTernaryArmType(rhsShape);
                compiler->RejectImplicitIntegerPointeePointerConversion(
                    lhsSourceType, rhsSourceType);
                // Agreeing operands still have to fit a pointer destination (the `?:` twin above).
                if (!inCallArgument_ && (outerExpected.Pointer || outerExpected.PointerDepth > 0)
                    && !DeclExpectedTypeScope::explicitCast
                    && !compiler->IsImplicitUnrelatedStructPointeeConversion(lhsSourceType, rhsSourceType)
                    && !compiler->IsImplicitUnrelatedStructPointeeConversion(rhsSourceType, lhsSourceType))
                {
                    for (const auto* operand : { &lhsSourceType, &rhsSourceType })
                    {
                        if (compiler->IsImplicitUnrelatedStructPointeeConversion(*operand, outerExpected))
                        {
                            LogErrorContext(ctx, compiler->ImplicitIntegerPointeeConversionText(
                                *operand, outerExpected));
                            break;
                        }
                        // The `?:` twin above: a cross-class C++ `T**` operand is refused here.
                        if (LLVMBackend::ArrayDecayedPointer(*operand).ValuePointerDepth() >= 2
                            && compiler->RejectCxxClassPointerStore(
                                outerExpected, *operand, "conditional expression"))
                            break;
                    }
                }
                if (resultAlloca->getAllocatedType()->isPointerTy())
                    rhs = compiler->BareFunctionToThinValue(rhs, nullptr);
                compiler->CreateAssignment(rhs, resultAlloca);
                if (rhsOwnsFallback)
                    compiler->builder->CreateStore(rhs, fallbackOwner.Slot);
                rhsCount = GetTernaryArrayCountState(rhs, rhsStorage, rhsShape);
                compiler->builder->CreateStore(
                    rhsCount.Available && rhsCount.Count != nullptr
                        ? compiler->Upconvert(rhsCount.Count, compiler->builder->getInt64Ty())
                        : compiler->builder->getInt64(-1),
                    rawCountAlloca);
                rhsPostfix.Flush();
                rhsBr = compiler->CreateJump(resumeBlock);
            }
            catch (...)
            {
                // expect_error continues after LogError throws, so every arm opened above must
                // be closed before the error propagates to its recovery path.
                if (compiler->IsInsertBlockLive()) compiler->builder->CreateBr(resumeBlock);
                for (auto* block : { nullBlock, notNullBlock })
                {
                    if (cflat_llvm::GetTerminatorOrNull(block) == nullptr)
                    {
                        compiler->SwitchToBlock(block);
                        compiler->builder->CreateBr(resumeBlock);
                    }
                }
                compiler->SwitchToBlock(resumeBlock);
                compiler->DiscardOwnedTempsSince(rhsMark);
                throw;
            }

            nullCoalesceControlScope.reset();
            compiler->SwitchToBlock(resumeBlock);
            auto* joined = compiler->CreateLoad(resultAlloca);
            // A C++ reference result is an alias value, but as one arm of a '??' it is only a
            // borrowed pointer: `p ?? &cell()` joins to a plain pointer unless a cflat alias joins.
            const bool joinAlias = (lhsAlias && rhsAlias)
                || (lhsAlias && !compiler->IsCxxReferenceAliasArm(lhs))
                || (rhsAlias && !compiler->IsCxxReferenceAliasArm(rhs));
            if (rhsAddrIntoTemp || compiler->AddressIntoStatementPtrTemp(lhs))
                compiler->RegisterAddressIntoTempValue(joined);
            if (joined != nullptr && pointerJoinDest.Pointer)
                compiler->RegisterValueElementTypeName(joined, pointerJoinDest.TypeName);
            if (joinAlias) compiler->RegisterAliasValue(joined);
            if (lhsTempField || rhsTempField) compiler->RegisterTempFieldValue(joined);
            // A '??' joins two views exactly as '?:' does, and the join is a plain load off a
            // slot - so name its element here or every binding door takes the unnamed escape.
            PropagateTernaryViewElement(ctx, lhs, lhsStorage, rhs, rhsStorage, joined, true);
            // Ledger the arms for the interface-boxing path: this joins through a SLOT, so the
            // result is a plain load and the arms are unrecoverable from the IR afterwards.
            if (joined != nullptr && lhsBr != nullptr && rhsBr != nullptr
                && rhs != nullptr && joined->getType()->isPointerTy()
                && lhs->getType()->isPointerTy() && rhs->getType()->isPointerTy())
            {
                compiler->RegisterNullCoalesceJoin(
                    joined, { { lhs, lhsBr->getParent() }, { rhs, rhsBr->getParent() } });
                compiler->AttachNullCoalesceFallback(joined, fallbackOwner);
                if (!inCallArgument_ && JoinArmIsProvablyNull(lhs)
                    && compiler->IsOwnedNewTemp(rhs))
                    compiler->PropagateOwnedNewTemp(rhs, joined);
                compiler->PropagateBondedValue(lhs, rhs, joined);
                compiler->RegisterJoinArmCastOccurrence(joined, 0, lhsOcc);
                compiler->RegisterJoinArmCastOccurrence(joined, 1, rhsOcc);
                trackGlobalUniquePointerArm(lhs, lhsStorage, lhsTv);
                trackGlobalUniquePointerArm(rhs, rhsStorage, rhsShape);
                compiler->PropagateUniqueFieldRead(lhs, rhs, joined);
                bool rawJoin = (compiler->IsRawArrayResult(lhs)
                        || compiler->IsRawArrayResult(rhs))
                    && compiler->TernaryArmJoinsOwning(lhs)
                    && compiler->TernaryArmJoinsOwning(rhs);
                auto lhsJoinType = InferTernaryArmType(lhsTv);
                auto rhsJoinType = InferTernaryArmType(rhsShape);
                auto isNullArm = [](llvm::Value* value,
                                    const LLVMBackend::TypedValue& shape) {
                    auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(value);
                    return (constant != nullptr && constant->isNullValue())
                        || (shape.hasSourceType && shape.sourceType.IsCxxNullptrT);
                };
                const bool viewArrayJoin = resultAlloca->getAllocatedType()->isPointerTy()
                    && ((lhsJoinType.IsArrayView
                            && (rhsJoinType.IsArrayView || isNullArm(rhs, rhsShape)))
                        || (rhsJoinType.IsArrayView && isNullArm(lhs, lhsTv)));
                if (rawJoin || viewArrayJoin)
                {
                    auto allocAlignOf = [&](llvm::Value* value) {
                        if (const auto* raw = compiler->FindRawArrayResult(value))
                            return raw->AllocAlign;
                        if (const auto* scalar = compiler->FindOwnedNewTemp(value))
                            return scalar->AllocAlign;
                        return uint64_t{0};
                    };
                    uint64_t lhsAlign = allocAlignOf(lhs);
                    uint64_t rhsAlign = allocAlignOf(rhs);
                    bool lhsIsNull = isNullArm(lhs, lhsTv);
                    if (rawJoin && !lhsIsNull && lhsAlign != rhsAlign)
                        LogErrorContext(ctx, "cannot join owning pointer allocations with different "
                            "alignment; use the same 'alignas' on every allocating arm");
                    if (viewArrayJoin)
                    {
                        std::vector<size_t> countSources = lhsCount.SourceParams;
                        for (size_t index : rhsCount.SourceParams)
                            if (std::find(countSources.begin(), countSources.end(), index)
                                == countSources.end())
                                countSources.push_back(index);
                        compiler->RegisterRawArrayResult(joined,
                            compiler->builder->CreateLoad(compiler->builder->getInt64Ty(),
                                rawCountAlloca, "raw_array_count_join"),
                            lhsIsNull ? rhsAlign : lhsAlign, false, nullptr, true,
                            lhsCount.Known && rhsCount.Known, std::move(countSources));
                    }
                    if (rawJoin)
                    {
                        compiler->RegisterRawArrayResult(
                            joined, compiler->builder->CreateLoad(
                                compiler->builder->getInt64Ty(), rawCountAlloca,
                                "raw_array_count_join"),
                            lhsIsNull ? rhsAlign : lhsAlign);
                        compiler->PropagateTernaryOwnership(
                            lhs, rhs, joined, uniquePointerDestination);
                    }
                }
            }
            // A blessed unique<X> join is a one-slot STRUCT, so the pointer-shaped ledgering
            // above skips it. Its arms are just as unrecoverable from the IR, and the
            // owning-temp-field escape guards read this ledger.
            else if (joined != nullptr && lhs != nullptr && rhs != nullptr
                     && lhsBr != nullptr && rhsBr != nullptr
                     && joined->getType() == lhs->getType())
            {
                auto* st = llvm::dyn_cast<llvm::StructType>(joined->getType());
                if (st != nullptr && st->hasName()
                    && compiler->IsCoreUniqueType(st->getName().str()))
                {
                    compiler->RegisterNullCoalesceJoin(
                        joined, { { lhs, lhsBr->getParent() }, { rhs, rhsBr->getParent() } });
                    compiler->PropagateUniqueFieldRead(lhs, rhs, joined);
                }
            }
            LLVMBackend::TypedValue result{ joined, false };
            JoinElementBorrow(compiler, result, lhsTv, rhsShape);
            // Both arms named one raw pointer shape: the '??' join carries it like a '?:' join.
            JoinTernaryPointerShape(result, lhsTv, rhsShape);
            JoinTernaryClassPointee(result, lhsTv, rhsShape);
            bool joinedIsCoreUniqueStruct = false;
            if (joined != nullptr)
                if (auto* st = llvm::dyn_cast<llvm::StructType>(joined->getType());
                    st != nullptr && st->hasName())
                    joinedIsCoreUniqueStruct = compiler->IsCoreUniqueType(st->getName().str());
            if (inCallArgument_ && joined != nullptr
                && (joined->getType()->isPointerTy() || joinedIsCoreUniqueStruct)
                && lhsBr != nullptr && rhsBr != nullptr)
            {
                auto collectMoveSinkArm = [&](const LLVMBackend::TypedValue& shape,
                                               llvm::Value* value, llvm::Value* storage,
                                               llvm::BasicBlock* armBlock) {
                    auto markThisSources = [&](size_t first) {
                        if (!shape.sourceRootIsBorrowedThisField || shape.isExplicitMove) return;
                        // Only an owning arm (unique / owning value) is a this-field consume (T447).
                        const auto& armType = shape.sourceType;
                        if (!shape.hasSourceType || !(armType.IsUnique || compiler->IsCoreUniqueType(armType.TypeName)
                                || (!armType.Pointer && compiler->IsOwningValueType(armType.TypeName))))
                            return;
                        for (size_t i = first; i < result.conditionalMoveSources.size(); ++i)
                        {
                            result.conditionalMoveSources[i].RootIsBorrowedThisField = true;
                            result.conditionalMoveSources[i].ThisFieldPath = shape.sourceThisFieldPath;
                        }
                    };
                    if (!shape.conditionalMoveSources.empty())
                    {
                        result.conditionalMoveSources.insert(result.conditionalMoveSources.end(),
                            shape.conditionalMoveSources.begin(), shape.conditionalMoveSources.end());
                        return;
                    }
                    if (CollectRangeElementArm(shape, value, storage, armBlock,
                            result.conditionalMoveSources))
                        return;
                    size_t firstThisSource = result.conditionalMoveSources.size();
                    if (CollectBorrowedPointerArm(shape, value, storage, armBlock,
                            result.conditionalMoveSources)
                        || CollectOwningLocalPointerArm(shape, value, storage, armBlock, nullptr,
                            result.conditionalMoveSources))
                    {
                        markThisSources(firstThisSource);
                        return;
                    }
                    // A unique field arm is a join source below (FindUniqueFieldReadJoin), with its null key.
                    if (value == nullptr || storage == nullptr || !value->getType()->isPointerTy()
                        || compiler->IsUniqueFieldReadValue(value)
                        || (shape.sourceFieldName.empty() && !shape.sourceRootIsBorrowedThisField)
                        || shape.sourceIsElementAccess)
                        return;
                    auto* fieldAddr = llvm::dyn_cast<llvm::GetElementPtrInst>(storage);
                    if (fieldAddr == nullptr || !compiler->IsOwningFieldAddress(fieldAddr)) return;
                    std::string sourceName = shape.sourceCallerName;
                    if (sourceName.empty()) sourceName = compiler->FindVariableNameByStorage(storage);
                    result.conditionalMoveSources.push_back({ storage, value->getType(), armBlock,
                        sourceName, shape.sourceFieldName, false });
                    result.conditionalMoveSources.back().MoveParamSinkArgument = true;
                    result.conditionalMoveSources.back().IsExplicitMove = shape.isExplicitMove;
                    markThisSources(result.conditionalMoveSources.size() - 1);
                };
                collectMoveSinkArm(lhsTv, lhs, lhsStorage, lhsBr->getParent());
                collectMoveSinkArm(rhsShape, rhs, rhsStorage, rhsBr->getParent());
                // The `??` join ledger retains local field arms whose raw-pointer source shape
                // was lost while lowering the coalesce value.
                if (const auto* fieldJoin = compiler->FindUniqueFieldReadJoin(joined))
                    for (const auto& source : fieldJoin->Sources)
                    {
                        // Pointer / `this` roots carry no NullKey: still clear the arm at run time.
                        if (source.Storage == nullptr || source.Block == nullptr) continue;
                        auto existing = std::find_if(result.conditionalMoveSources.begin(),
                            result.conditionalMoveSources.end(), [&](const auto& moveSource) {
                                return moveSource.Storage == source.Storage;
                            });
                        if (existing != result.conditionalMoveSources.end())
                        {
                            if (!existing->BorrowedPointer)
                            {
                                existing->MoveParamSinkArgument = true;
                                existing->NullKey = source.NullKey;
                            }
                            continue;
                        }
                        result.conditionalMoveSources.push_back({});
                        auto& moveSource = result.conditionalMoveSources.back();
                        moveSource.Storage = source.Storage;
                        moveSource.ValueType = joined->getType();
                        moveSource.ArmBlock = source.Block;
                        moveSource.MoveParamSinkArgument = true;
                        moveSource.NullKey = source.NullKey;
                    }
            }
            result.isAlias = joinAlias;
            return result;
        }

        // Claim the declaration destination before parsing the condition. A parenthesized
        // ternary in the condition recursively enters here and must not steal the outer slot.
        // Only a real `?:` claims it: a plain operand (a parenthesized arm `a ? X : (b ? Y : Z)`)
        // passes the slot on to the ternary inside it.
        const bool claimsCxxTernaryDecl = ctx->Question() != nullptr;
        // A nested `?:` arm also builds the slot, for a by-value or `const T&` parameter (MIXRES).
        const bool nestedCallTernaryArm = claimsCxxTernaryDecl && inCallArgument_
            && (ArmTernaryThroughParens(ctx->expression()) != nullptr
                || ArmTernaryThroughParens(ctx->conditionalExpression()) != nullptr);
        const bool constRefNestedCallTernary = nestedCallTernaryArm && outerExpected.IsCxxRef()
            && outerExpected.CxxConstRef() && !outerExpected.IsRvalueRef
            && outerExpected.CxxReferentLevel() == 0
            && compiler->IsForeignNontrivialCxxClass(outerExpected.TypeName)
            && IsWholeCxxCallArgumentShape(ctx, outerExpected);
        const bool cxxCallDefaultTernary = constRefNestedCallTernary
            || (claimsCxxTernaryDecl && inCallArgument_
            && !outerExpected.Pointer && !outerExpected.IsAlias
            && !outerExpected.IsRvalueRef
            && compiler->IsForeignNontrivialCxxClass(outerExpected.TypeName)
            && (IsDefaultOnlyExpression(ctx->expression())
                || IsDefaultOnlyExpression(ctx->conditionalExpression())
                || HasDefaultOnlyTernaryArm(ctx->expression())
                || HasDefaultOnlyTernaryArm(ctx->conditionalExpression())
                || nestedCallTernaryArm)
            && IsWholeCxxCallArgumentShape(ctx, outerExpected));
        auto* cxxCallTernaryType = cxxCallDefaultTernary
            ? compiler->GetType(LLVMBackend::TypeAndValue{ .TypeName = outerExpected.TypeName })
            : nullptr;
        llvm::Value* cxxCallTernarySlot = cxxCallTernaryType != nullptr
            ? compiler->AllocaAtEntry(cxxCallTernaryType, nullptr, "cxx.ternary.arg") : nullptr;
        struct CxxCallTernaryPendingScope
        {
            LLVMBackend* backend;
            bool active;
            llvm::Value* savedDest;
            std::string savedType;
            bool savedConsumed;
            bool savedFailed;
            CxxCallTernaryPendingScope(LLVMBackend* b, llvm::Value* slot,
                                       const std::string& typeName)
                : backend(b), active(slot != nullptr), savedDest(b->pendingCxxTernaryDeclDest_),
                  savedType(b->pendingCxxTernaryDeclTypeName_),
                  savedConsumed(b->pendingCxxTernaryDeclConsumed_),
                  savedFailed(b->pendingCxxTernaryDeclFailed_)
            {
                if (!active) return;
                backend->pendingCxxTernaryDeclDest_ = slot;
                backend->pendingCxxTernaryDeclTypeName_ = typeName;
                backend->pendingCxxTernaryDeclConsumed_ = false;
                backend->pendingCxxTernaryDeclFailed_ = false;
            }
            ~CxxCallTernaryPendingScope()
            {
                if (!active) return;
                backend->pendingCxxTernaryDeclDest_ = savedDest;
                backend->pendingCxxTernaryDeclTypeName_ = std::move(savedType);
                backend->pendingCxxTernaryDeclConsumed_ = savedConsumed;
                backend->pendingCxxTernaryDeclFailed_ = savedFailed;
            }
        } cxxCallTernaryPending(compiler, cxxCallTernarySlot, outerExpected.TypeName);
        // A non-class pointer leaf (a string literal) of a nested claim joins as a scalar.
        llvm::SaveAndRestore<bool> nestedCallClaimScope(cxxNestedCallTernaryClaim_,
            cxxNestedCallTernaryClaim_ || (cxxCallTernarySlot != nullptr && nestedCallTernaryArm));
        llvm::Value* cxxTernaryDeclDest = claimsCxxTernaryDecl
            ? compiler->pendingCxxTernaryDeclDest_ : nullptr;
        const std::string cxxTernaryDeclType = claimsCxxTernaryDecl
            ? compiler->pendingCxxTernaryDeclTypeName_ : std::string();
        const bool cxxTernaryDecl = cxxTernaryDeclDest != nullptr
            && !cxxTernaryDeclType.empty();
        if (claimsCxxTernaryDecl)
        {
            compiler->pendingCxxTernaryDeclDest_ = nullptr;
            compiler->pendingCxxTernaryDeclTypeName_.clear();
        }
        auto failCxxTernaryDecl = [&] {
            if (cxxTernaryDecl) compiler->pendingCxxTernaryDeclFailed_ = true;
        };

        // Grammar: logicalOrExpression ('?' expression ':' conditionalExpression)?
        // - so `expression` is the TRUE branch and `conditionalExpression` is the FALSE branch.
        auto expressionTrueCtx = ctx->expression();
        auto expressionFalseCtx = ctx->conditionalExpression();

        if (logicCtx != nullptr)
        {
            // A `?:` condition ends at a sequence point: its postfix ++/-- land before either arm.
            std::optional<BranchPostfixScope> condPostfix;
            if (expressionTrueCtx != nullptr) condPostfix.emplace(this);
            auto condTv = ParseLogicalOrExpression(
                logicCtx, expressionTrueCtx != nullptr ? ResultUse::Value : use);
            if (condPostfix.has_value())
            {
                condPostfix->Flush();
                condPostfix.reset();
            }

            // Both expression should exist or not exist.
            if ((expressionFalseCtx != nullptr) != (expressionTrueCtx != nullptr))
            {
                failCxxTernaryDecl();
                LogErrorContext(ctx, "Conditional expression requires both true and false branches.");
                return {};
            }
            else if (expressionFalseCtx != nullptr && (expressionTrueCtx != nullptr))
            {
                // In a function, branch so only the selected arm runs. A constant context (enum,
                // bitfield width, alignas, global init) has no live block and keeps the eager form.
                auto* insertBB = compiler->builder->GetInsertBlock();
                if (insertBB != nullptr && cflat_llvm::GetTerminatorOrNull(insertBB) == nullptr
                    && compiler->currentFunction != nullptr
                    && insertBB->getParent() == compiler->currentFunction)
                {
                    autoDeclTernaryActive_ = autoDeclTernary;
                    return ParseTernaryBranches(ctx, condTv, expressionTrueCtx, expressionFalseCtx,
                        use, outerExpected, cxxTernaryDeclDest, cxxTernaryDeclType,
                        collapseLvalueArms, collapseLvalueSink);
                }

                // Eager fallback: BOTH arms execute unconditionally, so a deref only sound under
                // one arm's condition is unsafe here - suppress the same-block moved-null guard.
                failCxxTernaryDecl();
                llvm::Value* trueValue  = nullptr;
                llvm::Value* falseValue = nullptr;
                bool trueUnsigned = false;
                bool falseUnsigned = false;
                LLVMBackend::TypedValue trueShape, falseShape;
                bool trueDefault = IsDefaultOnlyExpression(expressionTrueCtx);
                bool falseDefault = IsDefaultOnlyExpression(expressionFalseCtx);
                auto expectedForDefault = [&](llvm::Value* otherValue, bool otherIsDefault) {
                    auto expected = otherIsDefault ? LLVMBackend::TypeAndValue{}
                        : InferTernaryArmType(otherValue);
                    if (expected.TypeName.empty()) expected = outerExpected;
                    return expected;
                };
                auto parseTrueArm = [&]() {
                    std::optional<DeclExpectedTypeScope> expectedScope;
                    if (outerExpected.Pointer && compiler->IsCxxRecord(outerExpected.TypeName))
                        expectedScope.emplace(&declExpectedType, outerExpected);
                    else if (trueDefault)
                    {
                        auto expected = expectedForDefault(falseValue, falseDefault);
                        if (!expected.TypeName.empty())
                            expectedScope.emplace(&declExpectedType, expected,
                                isFunctionValueDestination(expected));
                    }
                    else if (functionValueDestination && use != ResultUse::Discard)
                        expectedScope.emplace(&declExpectedType, outerExpected, true);
                    if (use == ResultUse::Discard)
                    {
                        auto trueNV = ParseAssignmentExpressionNamed(
                            expressionTrueCtx->assignmentExpression(), use);
                        trueValue = trueNV.Primary;
                        trueUnsigned = trueNV.TypeAndValue.IsUnsignedInteger() != -1;
                        ProcessPlusPlus();
                    }
                    else
                    {
                        auto trueTv = ParseTernaryArmExpression(expressionTrueCtx);
                        trueValue = trueTv.value;
                        trueShape = trueTv;
                        trueUnsigned = trueTv.isUnsigned;
                    }
                };
                auto parseFalseArm = [&]() {
                    std::optional<DeclExpectedTypeScope> expectedScope;
                    if (outerExpected.Pointer && compiler->IsCxxRecord(outerExpected.TypeName))
                        expectedScope.emplace(&declExpectedType, outerExpected);
                    else if (falseDefault)
                    {
                        auto expected = expectedForDefault(trueValue, trueDefault);
                        if (!expected.TypeName.empty())
                            expectedScope.emplace(&declExpectedType, expected,
                                isFunctionValueDestination(expected));
                    }
                    else if (functionValueDestination && use != ResultUse::Discard)
                        expectedScope.emplace(&declExpectedType, outerExpected, true);
                    auto falseTv = parseFunctionDestinationConditionalArm(
                        expressionFalseCtx, outerExpected);
                    falseValue = falseTv.value;
                    falseShape = falseTv;
                    falseUnsigned = falseTv.isUnsigned;
                };
                {
                    SuppressExplicitNullDerefGuardScope suppressGuard(compiler);
                    if (trueDefault) { parseFalseArm(); parseTrueArm(); }
                    else { parseTrueArm(); parseFalseArm(); }
                }

                // A branch that is a plain (non-`move`) string-returning CALL yields a fresh owned
                // temp the call path did not register (only `move` returns are). CreateSelect below
                // evaluates BOTH branches and the string case deep-copies the winner into a new owned
                // buffer, leaving each branch's temp unreferenced - so an unregistered plain-return
                // temp leaks. Register any call-result string branch for end-of-statement cleanup;
                // this reuses the operator-operand path's logic: a named local / field read lowers to
                // a load (not a CallInst) and is skipped, so it is never double-freed, and the free is
                // owned-bit-gated so a borrow return is a runtime no-op. `move` returns are already
                // registered and re-registration is idempotent (deduped).
                RegisterBorrowedStringOperandTemp(compiler, trueValue);
                RegisterBorrowedStringOperandTemp(compiler, falseValue);
                UnifyTernaryFunctionArms(ctx, expressionTrueCtx, expressionFalseCtx, trueValue, falseValue,
                                         trueShape, falseShape);

                auto here = []() {};   // eager form: both arms already live in the current block
                llvm::Value* eagerTrueStorage = trueShape.storage;
                llvm::Value* eagerFalseStorage = falseShape.storage;
                const bool eagerLifted = LiftTernaryFunctionArmToClosure(ctx, trueValue, falseValue,
                    trueShape, falseShape, eagerTrueStorage, eagerFalseStorage, here, here);
                bool joinUnsigned = TernaryJoinIsUnsigned(
                    trueUnsigned,  trueValue != nullptr && trueValue->getType()->isIntegerTy()
                        ? trueValue->getType()->getIntegerBitWidth() : 0,
                    falseUnsigned, falseValue != nullptr && falseValue->getType()->isIntegerTy()
                        ? falseValue->getType()->getIntegerBitWidth() : 0);
                if (!UnifyTernaryArmTypes(ctx, trueValue, falseValue, trueShape, falseShape,
                        eagerTrueStorage, eagerFalseStorage, here, here,
                        compiler->CurrentCastOccurrence(), compiler->CurrentCastOccurrence(),
                        trueUnsigned, falseUnsigned,
                        TernaryInterfaceDestination(*compiler, outerExpected),
                        !inCallArgument_ && (outerExpected.Pointer || outerExpected.PointerDepth > 0),
                        &outerExpected))
                    return {};

                auto pointerJoinDest = cxxPointerJoinDest();
                adjustCxxPointerArm(trueValue, pointerJoinDest);
                adjustCxxPointerArm(falseValue, pointerJoinDest);

                // LLVM's select requires an i1 condition; a non-bool CFlat condition
                // (int, char, pointer, float) must be lowered the same way if/while do.
                auto* selectCond = compiler->CoerceToBoolCondition(ConvertConditionToBool(condTv));
                auto* selectValue = compiler->CreateSelect(selectCond, falseValue, trueValue);
                RegisterSelectArrayViewJoin(outerExpected.IsArrayView, selectValue, selectCond,
                    trueValue, falseValue, trueShape, falseShape);

                // Owning-string ternary: a branch that reads an owning string FIELD or a named
                // owning local (`cond ? this.name : "-"`) yields a struct whose OWNED bit rides
                // along with the aliased buffer pointer. The select copies that struct verbatim,
                // so the ternary result shallow-aliases the branch's buffer; binding it to a local
                // (whose owned-bit-gated dtor then fires) frees a buffer the field/local still owns
                // (use-after-free). Deep-copy the selected string into an independent owned buffer
                // (always safe, mirrors the manual `"" + this.name` workaround and the direct
                // `string x = obj.name` deep-copy path), then hand it back as an owned temporary so
                // the standard owned-temp machinery frees it if it is not moved into a named local.
                auto* strTy = llvm::StructType::getTypeByName(*compiler->context, "string");
                if (strTy != nullptr && selectValue != nullptr && selectValue->getType() == strTy)
                {
                    auto* owned = compiler->EmitOwnedStringDeepCopy(selectValue);
                    if (owned != selectValue)
                    {
                        compiler->RegisterOwnedStringTemp(owned);
                        compiler->lastCallReturnsOwned = true;
                        return { owned, true };
                    }
                }
                // Eager form: both arms already ran, so only the selected one can be adopted
                // and the other leaks regardless. See PropagateTernaryOwnership.
                if (compiler->PropagateTernaryOwnership(
                        trueValue, falseValue, selectValue, uniquePointerDestination))
                {
                    compiler->ClearOwnedResultChannels();
                    RejectMixedOwnershipTernaryJoin(trueValue, falseValue, selectValue,
                        expressionTrueCtx, expressionFalseCtx, ctx);
                }
                compiler->PropagateAliasValue(trueValue, falseValue, selectValue);
                compiler->PropagateBondedValue(trueValue, falseValue, selectValue);
                compiler->PropagateFatInterfaceJoin(trueValue, falseValue, selectValue);
                PropagateTernaryViewElement(ctx, trueValue, nullptr, falseValue, nullptr, selectValue);
                if (selectValue != nullptr && pointerJoinDest.Pointer)
                    compiler->RegisterValueElementTypeName(selectValue, pointerJoinDest.TypeName);
                selectValue = CloneTernaryClosureValue(selectValue, ctx);
                LLVMBackend::TypedValue result{ selectValue, joinUnsigned };
                result.isAlias = compiler->IsAliasValue(selectValue);
                JoinTernaryPointerShape(result, trueShape, falseShape);
                JoinTernaryClassPointee(result, trueShape, falseShape);
                JoinConditionalStackCharBufferBorrow(result, trueShape, falseShape);
                carryCommonThinFunctionPointerType(result, trueShape, falseShape,
                                                    trueValue, falseValue);
                CarryCommonFunctionSignature(compiler, result, trueShape, falseShape);
                if (eagerLifted) result.isRvalue = true;
                return result;
            }

            return condTv;
        }

        failCxxTernaryDecl();
        LogErrorContext(ctx, "Conditional expression has no logical-or sub-expression.");
        return {};
    }


/*
 * C++ lets a class overload && and ||. Such an overload is an ORDINARY call: C++ itself drops
 * short-circuit evaluation there, so both operands are evaluated and folded left to right. Returns
 * null when the first operand is not a class with that operator, which keeps every other
 * expression on the short-circuiting path below.
 */
LLVMBackend::TypedValue MainListener::TryClassLogicalOperatorChain(
        const std::string& op, antlr4::ParserRuleContext* ctx,
        const LLVMBackend::TypedValue& first,
        const std::function<LLVMBackend::TypedValue(size_t)>& parseOperand,
        size_t operandCount) {
        auto* compiler = Compiler(ctx);
        if (first.value == nullptr || !first.value->getType()->isStructTy()) return {};
        auto* structTy = llvm::cast<llvm::StructType>(first.value->getType());
        if (structTy->isLiteral() || !structTy->hasName()) return nullptr;
        const std::string typeName = structTy->getName().str();
        if (!compiler->IsCxxRecord(typeName)
            || !HasOperatorOverloadForFirstParam("operator" + op, typeName))
            return {};

        llvm::Value* accumulator = first.value;
        llvm::Value* accumulatorStorage = first.receiverStorage;
        bool accumulatorIsRvalue = first.isRvalue;
        llvm::Type* accumulatorRefType = nullptr;
        std::string accumulatorSourceTypeName = first.sourceTypeName;
        for (size_t i = 1; i < operandCount; ++i)
        {
            auto rv = ParseCxxFoldOperand(compiler, [&] { return parseOperand(i); });
            NormalizeCxxReferenceOperand(ctx, rv);
            CxxFoldSretScope foldSret(compiler, i + 1 < operandCount);
            llvm::Value* folded = accumulator != nullptr && accumulator->getType()->isStructTy()
                ? TryBinaryOperatorOverload(accumulator, op, rv.value, ctx, first.elemType,
                                            rv.pointerDepth, rv.elemPointer, accumulatorStorage,
                                            rv.receiverStorage, false, true,
                                            accumulatorIsRvalue, rv.isRvalue,
                                            accumulator == first.value ? first.sourceTypeName
                                                                       : std::string(),
                                            rv.sourceTypeName, 0, false, {}, {}, false, {}, {},
                                            false, {}, {}, rv.cxxPointeeConst,
                                            accumulator == first.value ? first.cxxArithIdentity : std::string(),
                                            rv.cxxArithIdentity, rv.isExplicitMove, accumulator == first.value && first.isExplicitMove,
                                            accumulator == first.value && first.hasSourceType && first.sourceType.IsCxxNullptrT,
                                            rv.hasSourceType && rv.sourceType.IsCxxNullptrT,
                                            false, false,
                                            accumulator == first.value ? first.cxxObjectCv : 0,
                                            rv.cxxObjectCv, false, false, nullptr, nullptr,
                                            rv.bitfieldStorage)
                : nullptr;
            if (folded == nullptr)
            {
                // Past the first call the accumulator is the operator's own result (a bool for
                // the usual C++ spelling), so the rest of the chain is the ordinary reduction.
                accumulator = compiler->CoerceToBoolCondition(
                    accumulator == first.value ? ConvertConditionToBool(first)
                                               : ConvertConditionToBool(accumulator));
                llvm::Value* right = compiler->CoerceToBoolCondition(ConvertConditionToBool(rv));
                accumulator = compiler->CreateOperation(
                    op == "&&" ? LLVMBackend::Operation::LogicalAnd
                               : LLVMBackend::Operation::LogicalOr, accumulator, right);
                accumulatorStorage = nullptr;
                accumulatorIsRvalue = true;
                accumulatorRefType = nullptr;
            }
            else
                CarryCxxOperatorResult(ctx, folded, i + 1 < operandCount, accumulator,
                                       accumulatorStorage, accumulatorIsRvalue,
                                       accumulatorRefType, accumulatorSourceTypeName);
        }
        LLVMBackend::TypedValue result{ accumulator, false };
        result.receiverStorage = accumulatorStorage;
        result.cxxRefValueType = accumulatorRefType;
        result.isRvalue = accumulatorIsRvalue;
        result.sourceTypeName = accumulatorSourceTypeName;
        result.isAlias = accumulatorRefType != nullptr && compiler->lastCallReturnType.IsAlias;
        return result;
    }

LLVMBackend::TypedValue MainListener::ParseLogicalOrExpression(CFlatParser::LogicalOrExpressionContext* ctx, ResultUse use) {
        DeclExpectedTypeGate declExpectedGate(&declExpectedType, ctx->children.size() == 1);
        auto* compiler = Compiler(ctx);
        auto logicCtxs = ctx->logicalAndExpression();

        if (logicCtxs.size() == 1)
        {
            return ParseLogicalAndExpression(logicCtxs[0], use);
        }
        else if (logicCtxs.size() > 1)
        {
            llvm::Value* left = nullptr;

            // Always use resultStorage path. The elseBlock optimization was broken:
            // when the first || operand is true it jumped to blockFalse instead of blockTrue,
            // because only the false-destination is stored in the block context.
            // Sequence point after each operand: its postfix ++/-- land before the next is tested.
            std::optional<BranchPostfixScope> firstPostfix(std::in_place, this);
            auto firstOperand = ParseCxxFoldOperand(compiler, [&] {
                return ParseLogicalAndExpression(logicCtxs[0], ResultUse::Value);
            });
            firstPostfix->Flush();
            firstPostfix.reset();
            if (LLVMBackend::TypedValue classChain = TryClassLogicalOperatorChain(
                    "||", ctx, firstOperand,
                    [&](size_t i) { return ParseLogicalAndExpression(logicCtxs[i], ResultUse::Value); },
                    logicCtxs.size()))
                return classChain;

            LLVMBackend::TypeAndValue boolValue = { .TypeName = "bool",.VariableName = "", .Pointer = false };
            auto resultStorage = compiler->CreateAlloca(compiler->GetType(boolValue));
            // Tag for the null-state walker: only these compiler slots carry condition temps.
            resultStorage->setName("cond.or");
            auto resumeBlock = compiler->CreateBasicBlock("resumeOR");
            llvm::BasicBlock* logicStart = nullptr;   // dominates every operand and the resume

            for (const auto& logicCtx : logicCtxs)
            {
                if (left == nullptr)
                {
                    left = compiler->CoerceToBoolCondition(ConvertConditionToBool(firstOperand));
                    compiler->CreateAssignment(left, resultStorage);
                }
                else
                {
                    auto falseBlock = compiler->CreateBasicBlock("falseOR");
                    if (logicStart == nullptr) logicStart = compiler->builder->GetInsertBlock();
                    auto branch = compiler->CreateConditionJump(left, resumeBlock, falseBlock);

                    compiler->InitializeBlock(falseBlock, false);
                    OwnershipControlScope shortCircuitControlScope(compiler);
                    ShortCircuitMovedStateScope preRhsMovedState(compiler);
                    LLVMBackend::OwnedTempMark rhsMark = compiler->MarkOwnedTemps();
                    BranchPostfixScope rhsPostfix(this);  // runs only when the left side is false
                    auto rightOperand = ParseCxxFoldOperand(compiler, [&] {
                        return ParseLogicalAndExpression(logicCtx, ResultUse::Value);
                    });
                    preRhsMovedState.Merge();
                    llvm::Value* right = compiler->CoerceToBoolCondition(ConvertConditionToBool(rightOperand));
                    rhsPostfix.Flush();
                    left = compiler->CreateOperation(LLVMBackend::Operation::LogicalOr, left, right);
                    // The short-circuit block does not dominate resumeOR, so the end-of-statement
                    // flush would skip its temps; the operands are already reduced to a bool here.
                    // A C++ brace list built here lives to the end of the full-expression (as
                    // in C++), guarded by a flag set only when this operand ran.
                    auto braceLists = compiler->HoistBraceListTempsSince(rhsMark, logicStart);
                    compiler->FlushOwnedTempsSince(rhsMark, nullptr);
                    compiler->RestoreOwnedStructTemps(std::move(braceLists));
                    compiler->CreateAssignment(left, resultStorage);
                }
            }

            compiler->CreateBlockBreak(resumeBlock, false);

            compiler->InitializeBlock(resumeBlock, false);
            return { compiler->CreateLoad(resultStorage), false };
        }

        LogErrorContext(ctx, "Logical-OR expression has no operands.");
        return {};
    }

LLVMBackend::TypedValue MainListener::ParseLogicalAndExpression(CFlatParser::LogicalAndExpressionContext* ctx, ResultUse use) {
        DeclExpectedTypeGate declExpectedGate(&declExpectedType, ctx->children.size() == 1);
        auto* compiler = Compiler(ctx);
        auto inclusiveCtxs = ctx->inclusiveOrExpression();

        if (inclusiveCtxs.size() == 1)
        {
            return ParseInclusiveOrExpression(inclusiveCtxs[0], use);
        }
        else if (inclusiveCtxs.size() > 1)
        {
            llvm::Value* left = nullptr;

            // Always use resultStorage path. The elseBlock optimization was broken:
            // in non-condition contexts (e.g. bool x = a && b inside a loop body) the
            // elseBlock from an enclosing scope (loop exit) was incorrectly used as the
            // false-branch target, causing the loop to exit instead of continuing.
            // Sequence point after each operand: its postfix ++/-- land before the next is tested.
            std::optional<BranchPostfixScope> firstPostfix(std::in_place, this);
            auto firstOperand = ParseCxxFoldOperand(compiler, [&] {
                return ParseInclusiveOrExpression(inclusiveCtxs[0], ResultUse::Value);
            });
            firstPostfix->Flush();
            firstPostfix.reset();
            if (LLVMBackend::TypedValue classChain = TryClassLogicalOperatorChain(
                    "&&", ctx, firstOperand,
                    [&](size_t i) { return ParseCxxFoldOperand(compiler, [&] {
                        return ParseInclusiveOrExpression(inclusiveCtxs[i], ResultUse::Value);
                    }); },
                    inclusiveCtxs.size()))
                return classChain;

            LLVMBackend::TypeAndValue boolValue = { .TypeName = "bool",.VariableName = "", .Pointer = false };
            auto resultStorage = compiler->CreateAlloca(compiler->GetType(boolValue));
            // Tag for the null-state walker: only these compiler slots carry condition temps.
            resultStorage->setName("cond.and");
            auto resumeBlock = compiler->CreateBasicBlock("resumeAND");
            llvm::BasicBlock* logicStart = nullptr;   // dominates every operand and the resume

            for (const auto& inclusiveCtx : inclusiveCtxs)
            {
                if (left == nullptr)
                {
                    left = compiler->CoerceToBoolCondition(ConvertConditionToBool(firstOperand));
                    compiler->CreateAssignment(left, resultStorage);
                }
                else
                {
                    auto trueBlock = compiler->CreateBasicBlock("trueAND");
                    if (logicStart == nullptr) logicStart = compiler->builder->GetInsertBlock();
                    auto branch = compiler->CreateConditionJump(left, trueBlock, resumeBlock);

                    compiler->InitializeBlock(trueBlock, false);
                    OwnershipControlScope shortCircuitControlScope(compiler);
                    ShortCircuitMovedStateScope preRhsMovedState(compiler);
                    LLVMBackend::OwnedTempMark rhsMark = compiler->MarkOwnedTemps();
                    BranchPostfixScope rhsPostfix(this);  // runs only when the left side is true
                    auto rightOperand = ParseCxxFoldOperand(compiler, [&] {
                        return ParseInclusiveOrExpression(inclusiveCtx, ResultUse::Value);
                    });
                    preRhsMovedState.Merge();
                    llvm::Value* right = compiler->CoerceToBoolCondition(ConvertConditionToBool(rightOperand));
                    rhsPostfix.Flush();
                    left = compiler->CreateOperation(LLVMBackend::Operation::LogicalAnd, left, right);
                    // The short-circuit block does not dominate resumeAND, so the end-of-statement
                    // flush would skip its temps; the operands are already reduced to a bool here.
                    // A C++ brace list built here lives to the end of the full-expression (as
                    // in C++), guarded by a flag set only when this operand ran.
                    auto braceLists = compiler->HoistBraceListTempsSince(rhsMark, logicStart);
                    compiler->FlushOwnedTempsSince(rhsMark, nullptr, logicStart);
                    compiler->RestoreOwnedStructTemps(std::move(braceLists));
                    compiler->CreateAssignment(left, resultStorage);
                }
            }

            compiler->CreateBlockBreak(resumeBlock, false);

            compiler->InitializeBlock(resumeBlock, false);
            return { compiler->CreateLoad(resultStorage), false };
        }

        LogErrorContext(ctx, "Logical-AND expression has no operands.");
        return {};
    }

LLVMBackend::TypedValue MainListener::ParseInclusiveOrExpression(CFlatParser::InclusiveOrExpressionContext* ctx, ResultUse use) {
        CallPostfixScope operatorPostfix(this);
        DeclExpectedTypeGate declExpectedGate(&declExpectedType, ctx->children.size() == 1);
        auto exclusiveCtxs = ctx->exclusiveOrExpression();
        if (exclusiveCtxs.size() == 1)
            return ParseExclusiveOrExpression(exclusiveCtxs[0], use);

        if (exclusiveCtxs.size() > 1)
        {
            auto* compiler = Compiler(ctx);
            compiler->lastCallReturnsOwned = false;
            auto lv = ParseCxxFoldOperand(compiler, [&] {
                return ParseExclusiveOrExpression(exclusiveCtxs[0], ResultUse::Value);
            });
            NormalizeCxxReferenceOperand(ctx, lv);
            RegisterBorrowedStringOperandTemp(compiler, lv.value);
            compiler->RegisterOwnedPtrTemp(lv.value);
        llvm::Value* acc = lv.value;
        LLVMBackend::TypedValue arithRunning = lv;
        std::string accumulatorCxxArithIdentity = CxxScalarOperandIdentity(
            compiler, lv.cxxArithIdentity, lv.sourceTypeName, lv.value);
        if (accumulatorCxxArithIdentity.empty())
            accumulatorCxxArithIdentity = CxxLiteralOperandIdentity(exclusiveCtxs[0]);
        llvm::Value* accumulatorStorage = lv.receiverStorage;
        bool accumulatorIsRvalue = lv.isRvalue;
        llvm::Type* accumulatorRefType = nullptr;
        std::string accumulatorSourceTypeName = lv.sourceTypeName;
            bool unsignedStorage = false;
            for (size_t i = 1; i < exclusiveCtxs.size(); i++)
            {
                compiler->lastCallReturnsOwned = false;
                auto rv = ParseCxxFoldOperand(compiler, [&] {
                    return ParseExclusiveOrExpression(exclusiveCtxs[i], ResultUse::Value);
                });
                NormalizeCxxReferenceOperand(ctx, rv);
                RegisterBorrowedStringOperandTemp(compiler, rv.value);
                compiler->RegisterOwnedPtrTemp(rv.value);
                CxxFoldSretScope foldSret(compiler, i + 1 < exclusiveCtxs.size());
                // A struct operand routes to 'operator|' the way the shift/relational paths do;
                // without it an overloaded receiver reaches the raw integer op and fails the verifier.
                auto* overload = TryBinaryOperatorOverload(acc, "|", rv.value, ctx, lv.elemType,
                                                           rv.pointerDepth, rv.elemPointer,
                                                           accumulatorStorage, rv.receiverStorage,
                                                           true, true, accumulatorIsRvalue, rv.isRvalue,
                                                           accumulatorSourceTypeName,
                                                           rv.sourceTypeName,
                                                           acc == lv.value ? lv.pointerDepth : 0,
                                                           acc == lv.value && lv.elemPointer, {}, {},
                                                           false, {}, {}, false, {}, {},
                                                           rv.cxxPointeeConst,
                                                           acc == lv.value ? lv.cxxArithIdentity : std::string(),
                                                           rv.cxxArithIdentity, rv.isExplicitMove, acc == lv.value && lv.isExplicitMove,
                                                           acc == lv.value && lv.hasSourceType && lv.sourceType.IsCxxNullptrT,
                                                           rv.hasSourceType && rv.sourceType.IsCxxNullptrT,
                                                           false, acc == lv.value && lv.cxxPointeeConst,
                                                           acc == lv.value ? lv.cxxObjectCv : 0,
                                                           rv.cxxObjectCv, false, false, nullptr,
                                                           exclusiveCtxs[i], rv.bitfieldStorage);
                if (overload == nullptr && acc != nullptr && rv.value != nullptr
                    && (acc->getType()->isStructTy() || rv.value->getType()->isStructTy()))
                    LogErrorContext(ctx, "no overload of 'operator|' matches the given arguments.");
                if (overload != nullptr)
                {
                    LLVMBackend::NamedVariable resultNV;
                    resultNV.Primary = overload;
                    resultNV.TypeAndValue = compiler->lastCallReturnType;
                    DiagnoseVoidResultConsumed(ctx, resultNV, use, "'operator|'");
                    CarryCxxOperatorResult(ctx, overload, i + 1 < exclusiveCtxs.size(), acc,
                                           accumulatorStorage, accumulatorIsRvalue,
                                           accumulatorRefType, accumulatorSourceTypeName);
                    accumulatorCxxArithIdentity.clear();
                    lv.isUnsigned = resultNV.TypeAndValue.IsUnsignedInteger() != -1;
                }
                else
                {
                    const std::string leftIdentity = accumulatorCxxArithIdentity;
                    std::string rightIdentity = CxxScalarOperandIdentity(
                        compiler, rv.cxxArithIdentity, rv.sourceTypeName, rv.value);
                    if (rightIdentity.empty()) rightIdentity = CxxLiteralOperandIdentity(exclusiveCtxs[i]);
                    bool resultUnsigned = BinaryJoinIsUnsigned(
                        lv.isUnsigned, BinaryOperandBits(acc), rv.isUnsigned, BinaryOperandBits(rv.value));
                    const LLVMBackend::TypedValue arithLhs = ArithRunningOperand(arithRunning, acc);
                    acc = compiler->CreateOperation(LLVMBackend::Operation::BitwiseOr, acc, rv.value,
                                                     lv.isUnsigned, rv.isUnsigned);
                    accumulatorCxxArithIdentity = acc != nullptr
                        ? ScalarArithmeticResultName(compiler, leftIdentity, rightIdentity,
                                                     acc->getType())
                        : std::string();
                    arithRunning = ArithComputedOperand(acc, LLVMBackend::ArithResultNarrowWidth(arithLhs, rv));
                    accumulatorStorage = nullptr;
                    accumulatorIsRvalue = true;
                    accumulatorRefType = nullptr;
                    accumulatorSourceTypeName.clear();
                    unsignedStorage = acc != nullptr && acc->getType()->isIntegerTy()
                        && acc->getType()->getIntegerBitWidth() < 32
                        && (lv.isUnsigned || rv.isUnsigned);
                    lv.isUnsigned = resultUnsigned;
                }
            }
            LLVMBackend::TypedValue result{ acc, lv.isUnsigned };
            result.arithNarrowWidth = ArithRunningOperand(arithRunning, acc).arithNarrowWidth;
            result.isRvalue = accumulatorIsRvalue;
            result.receiverStorage = accumulatorStorage;
            result.cxxRefValueType = accumulatorRefType;
            result.isAlias = accumulatorRefType != nullptr
                && !accumulatorSourceTypeName.empty();
            result.storage = result.isAlias ? accumulatorStorage : nullptr;
            result.sourceTypeName = accumulatorSourceTypeName;
            result.cxxArithIdentity = accumulatorCxxArithIdentity;
            result.isUnsignedStorage = unsignedStorage;
            return result;
        }

        LogErrorContext(ctx, "Inclusive-OR expression has no operands.");
        return {};
    }

LLVMBackend::TypedValue MainListener::ParseExclusiveOrExpression(CFlatParser::ExclusiveOrExpressionContext* ctx, ResultUse use) {
        CallPostfixScope operatorPostfix(this);
        DeclExpectedTypeGate declExpectedGate(&declExpectedType, ctx->children.size() == 1);
        auto andCtxs = ctx->andExpression();
        if (andCtxs.size() == 1)
            return ParseAndExpression(andCtxs[0], use);

        if (andCtxs.size() > 1)
        {
            auto* compiler = Compiler(ctx);
            compiler->lastCallReturnsOwned = false;
            auto lv = ParseCxxFoldOperand(compiler, [&] {
                return ParseAndExpression(andCtxs[0], ResultUse::Value);
            });
            NormalizeCxxReferenceOperand(ctx, lv);
            RegisterBorrowedStringOperandTemp(compiler, lv.value);
            compiler->RegisterOwnedPtrTemp(lv.value);
            llvm::Value* acc = lv.value;
            LLVMBackend::TypedValue arithRunning = lv;
            std::string accumulatorCxxArithIdentity = CxxScalarOperandIdentity(
                compiler, lv.cxxArithIdentity, lv.sourceTypeName, lv.value);
            if (accumulatorCxxArithIdentity.empty())
                accumulatorCxxArithIdentity = CxxLiteralOperandIdentity(andCtxs[0]);
            llvm::Value* accumulatorStorage = lv.receiverStorage;
            bool accumulatorIsRvalue = lv.isRvalue;
            llvm::Type* accumulatorRefType = nullptr;
            std::string accumulatorSourceTypeName = lv.sourceTypeName;
            bool unsignedStorage = false;
            for (size_t i = 1; i < andCtxs.size(); i++)
            {
                compiler->lastCallReturnsOwned = false;
                auto rv = ParseCxxFoldOperand(compiler, [&] {
                    return ParseAndExpression(andCtxs[i], ResultUse::Value);
                });
                NormalizeCxxReferenceOperand(ctx, rv);
                RegisterBorrowedStringOperandTemp(compiler, rv.value);
                compiler->RegisterOwnedPtrTemp(rv.value);
                CxxFoldSretScope foldSret(compiler, i + 1 < andCtxs.size());
                // A struct operand routes to 'operator^' the way the shift/relational paths do;
                // without it an overloaded receiver reaches the raw integer op and fails the verifier.
                auto* overload = TryBinaryOperatorOverload(acc, "^", rv.value, ctx, lv.elemType,
                                                           rv.pointerDepth, rv.elemPointer,
                                                           accumulatorStorage, rv.receiverStorage,
                                                           true, true, accumulatorIsRvalue, rv.isRvalue,
                                                           accumulatorSourceTypeName,
                                                           rv.sourceTypeName,
                                                           acc == lv.value ? lv.pointerDepth : 0,
                                                           acc == lv.value && lv.elemPointer, {}, {},
                                                           false, {}, {}, false, {}, {},
                                                           rv.cxxPointeeConst,
                                                           acc == lv.value ? lv.cxxArithIdentity : std::string(),
                                                           rv.cxxArithIdentity, rv.isExplicitMove, acc == lv.value && lv.isExplicitMove,
                                                           acc == lv.value && lv.hasSourceType && lv.sourceType.IsCxxNullptrT,
                                                           rv.hasSourceType && rv.sourceType.IsCxxNullptrT,
                                                           false, acc == lv.value && lv.cxxPointeeConst,
                                                           acc == lv.value ? lv.cxxObjectCv : 0,
                                                           rv.cxxObjectCv, false, false, nullptr,
                                                           andCtxs[i], rv.bitfieldStorage);
                if (overload == nullptr && acc != nullptr && rv.value != nullptr
                    && (acc->getType()->isStructTy() || rv.value->getType()->isStructTy()))
                    LogErrorContext(ctx, "no overload of 'operator^' matches the given arguments.");
                if (overload != nullptr)
                {
                    LLVMBackend::NamedVariable resultNV;
                    resultNV.Primary = overload;
                    resultNV.TypeAndValue = compiler->lastCallReturnType;
                    DiagnoseVoidResultConsumed(ctx, resultNV, use, "'operator^'");
                    CarryCxxOperatorResult(ctx, overload, i + 1 < andCtxs.size(), acc,
                                           accumulatorStorage, accumulatorIsRvalue,
                                           accumulatorRefType, accumulatorSourceTypeName);
                    accumulatorCxxArithIdentity.clear();
                    lv.isUnsigned = resultNV.TypeAndValue.IsUnsignedInteger() != -1;
                }
                else
                {
                    const std::string leftIdentity = accumulatorCxxArithIdentity;
                    std::string rightIdentity = CxxScalarOperandIdentity(
                        compiler, rv.cxxArithIdentity, rv.sourceTypeName, rv.value);
                    if (rightIdentity.empty()) rightIdentity = CxxLiteralOperandIdentity(andCtxs[i]);
                    bool resultUnsigned = BinaryJoinIsUnsigned(
                        lv.isUnsigned, BinaryOperandBits(acc), rv.isUnsigned, BinaryOperandBits(rv.value));
                    const LLVMBackend::TypedValue arithLhs = ArithRunningOperand(arithRunning, acc);
                    acc = compiler->CreateOperation(LLVMBackend::Operation::BitwiseXor, acc, rv.value,
                                                     lv.isUnsigned, rv.isUnsigned);
                    accumulatorCxxArithIdentity = acc != nullptr
                        ? ScalarArithmeticResultName(compiler, leftIdentity, rightIdentity,
                                                     acc->getType())
                        : std::string();
                    arithRunning = ArithComputedOperand(acc, LLVMBackend::ArithResultNarrowWidth(arithLhs, rv));
                    accumulatorStorage = nullptr;
                    accumulatorIsRvalue = true;
                    accumulatorRefType = nullptr;
                    accumulatorSourceTypeName.clear();
                    unsignedStorage = acc != nullptr && acc->getType()->isIntegerTy()
                        && acc->getType()->getIntegerBitWidth() < 32
                        && (lv.isUnsigned || rv.isUnsigned);
                    lv.isUnsigned = resultUnsigned;
                }
            }
            LLVMBackend::TypedValue result{ acc, lv.isUnsigned };
            result.arithNarrowWidth = ArithRunningOperand(arithRunning, acc).arithNarrowWidth;
            result.isRvalue = accumulatorIsRvalue;
            result.receiverStorage = accumulatorStorage;
            result.cxxRefValueType = accumulatorRefType;
            result.isAlias = accumulatorRefType != nullptr
                && !accumulatorSourceTypeName.empty();
            result.storage = result.isAlias ? accumulatorStorage : nullptr;
            result.sourceTypeName = accumulatorSourceTypeName;
            result.cxxArithIdentity = accumulatorCxxArithIdentity;
            result.isUnsignedStorage = unsignedStorage;
            return result;
        }

        LogErrorContext(ctx, "Exclusive-OR expression has no operands.");
        return {};
    }

