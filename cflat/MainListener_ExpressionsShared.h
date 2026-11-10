#pragma once

class CxxAssignSretScope
{
    LLVMBackend* compiler_;
    CxxAssignSretScope* outer_;
    llvm::Value* savedDest_;
    std::string savedType_;
    bool consumed_ = false;
    static inline thread_local CxxAssignSretScope* current_ = nullptr;
public:
    explicit CxxAssignSretScope(LLVMBackend* compiler)
        : compiler_(compiler), outer_(current_), savedDest_(compiler->pendingCxxSretDest_),
          savedType_(compiler->pendingCxxSretTypeName_)
    {
        compiler_->pendingCxxSretDest_ = nullptr;
        compiler_->pendingCxxSretTypeName_.clear();
        current_ = this;
    }
    ~CxxAssignSretScope()
    {
        current_ = outer_;
        if (consumed_) return;
        compiler_->pendingCxxSretDest_ = savedDest_;
        compiler_->pendingCxxSretTypeName_ = std::move(savedType_);
    }
    CxxAssignSretScope(const CxxAssignSretScope&) = delete;
    CxxAssignSretScope& operator=(const CxxAssignSretScope&) = delete;
    // Whether the last operator call emitted under this scope constructed into its slot.
    bool Consumed() const { return consumed_; }
    // Whether an enclosing declarator / return slot of `typeName` waits for the operator result.
    static bool ArmedFor(LLVMBackend* compiler, const std::string& typeName)
    {
        return current_ != nullptr && current_->compiler_ == compiler
            && current_->savedDest_ != nullptr && current_->savedType_ == typeName;
    }
    static llvm::AllocaInst* TargetFor(LLVMBackend* compiler, const std::string& typeName)
    {
        return current_ != nullptr && current_->compiler_ == compiler
            && current_->savedDest_ != nullptr
            && compiler->ResolveTypeAlias(current_->savedType_)
                   == compiler->ResolveTypeAlias(typeName)
            ? llvm::dyn_cast<llvm::AllocaInst>(current_->savedDest_) : nullptr;
    }

    template <typename Emit>
    static llvm::Value* EmitOperatorCall(LLVMBackend* compiler, Emit&& emit)
    {
        auto* scope = current_;
        if (scope == nullptr || scope->compiler_ != compiler || scope->savedDest_ == nullptr)
            return emit();
        compiler->pendingCxxSretDest_ = scope->savedDest_;
        compiler->pendingCxxSretTypeName_ = scope->savedType_;
        llvm::Value* result = nullptr;
        try
        {
            result = emit();
        }
        catch (...)
        {
            scope->consumed_ = false;
            compiler->pendingCxxSretDest_ = nullptr;
            compiler->pendingCxxSretTypeName_.clear();
            throw;
        }
        scope->consumed_ = result != nullptr && compiler->pendingCxxSretDest_ == nullptr;
        compiler->pendingCxxSretDest_ = nullptr;
        compiler->pendingCxxSretTypeName_.clear();
        return result;
    }
};

class CxxFoldSretScope
{
    LLVMBackend* compiler_;
    llvm::Value* savedDest_;
    std::string savedType_;
public:
    CxxFoldSretScope(LLVMBackend* compiler, bool intermediate)
        : compiler_(intermediate ? compiler : nullptr),
          savedDest_(intermediate ? compiler->pendingCxxSretDest_ : nullptr),
          savedType_(intermediate ? compiler->pendingCxxSretTypeName_ : std::string())
    {
        if (compiler_ != nullptr)
        {
            compiler_->pendingCxxSretDest_ = nullptr;
            compiler_->pendingCxxSretTypeName_.clear();
        }
    }
    ~CxxFoldSretScope()
    {
        if (compiler_ != nullptr)
        {
            compiler_->pendingCxxSretDest_ = savedDest_;
            compiler_->pendingCxxSretTypeName_ = std::move(savedType_);
        }
    }
};

inline CFlatParser::ConditionalExpressionContext* ArmTernaryThroughParens(antlr4::tree::ParseTree* node);
inline bool IsParseTreeAncestor(antlr4::tree::ParseTree* ancestor,
                                antlr4::tree::ParseTree* node)
{
    for (auto* current = node; current != nullptr; current = current->parent)
        if (current == ancestor) return true;
    return false;
}
inline bool IsCallArgumentContaining(
    CFlatParser::PostfixExpressionContext* call,
    const antlr4::tree::TerminalNode* callParen,
    CFlatParser::AdditiveExpressionContext* expression)
{
    if (call == nullptr || callParen == nullptr || expression == nullptr) return false;
    for (auto* list : call->argumentExpressionList())
    {
        bool matchesCall = false;
        for (size_t i = 1; i < call->children.size(); ++i)
            if (call->children[i] == list && call->children[i - 1] == callParen)
            {
                matchesCall = true;
                break;
            }
        if (!matchesCall) continue;
        for (auto* argument : list->argumentNamedExpression())
            if (IsParseTreeAncestor(argument, expression)) return true;
    }
    return false;
}
inline std::vector<std::string> CxxFreeOperatorSources(const std::string& className,
                                                        const std::string& operatorName)
{
    const std::string base = className.substr(0, className.find('$'));
    const size_t dot = base.rfind('.');
    std::vector<std::string> sources;
    if (dot != std::string::npos)
        sources.push_back(base.substr(0, dot) + "." + operatorName);
    sources.push_back(operatorName);
    return sources;
}
template <typename Parse>
inline auto ParseCxxFoldOperand(LLVMBackend* compiler, Parse&& parse)
{
    // A nested by-value call is a full-expression temporary, not the enclosing fold's result slot.
    CxxFoldSretScope operandSret(compiler, true);
    return parse();
}
inline std::string PtrdiffTypeName(LLVMBackend* compiler)
{
    LLVMBackend::TypeAndValue longType;
    longType.TypeName = "long";
    llvm::Type* longLlvm = compiler->GetType(longType);
    return longLlvm != nullptr && longLlvm->isIntegerTy(64) ? std::string("long") : std::string("i64");
}
inline LLVMBackend::TypedValue ArithRunningOperand(const LLVMBackend::TypedValue& running,
                                                    llvm::Value* current)
{
        if (running.value == current) return running;
        LLVMBackend::TypedValue fresh;
        fresh.value = current;
        fresh.sourceTypeName = "int";
        fresh.hasSourceType = true;
        return fresh;
}
inline LLVMBackend::TypedValue ArithComputedOperand(llvm::Value* computed, unsigned mark)
{
        LLVMBackend::TypedValue out;
        out.value = computed;
        out.sourceTypeName = "int";  // a computed int result is never an unnamed literal
        out.hasSourceType = true;
        out.arithNarrowWidth = static_cast<uint8_t>(mark);
        return out;
}
inline LLVMBackend::TypedValue NamedArithOperand(const LLVMBackend::NamedVariable& named,
                                                  llvm::Value* loaded)
{
        LLVMBackend::TypedValue out;
        out.value = loaded;
        out.sourceTypeName = named.TypeAndValue.TypeName;
        out.hasSourceType = !named.TypeAndValue.TypeName.empty();
        out.arithNarrowWidth = named.ArithNarrowWidth;
        return out;
}
// A floating scalar operator result keeps its name: long double and double share one LLVM type.
inline std::string FloatingOperatorReturnName(const LLVMBackend::TypeAndValue& returnType)
{
    if (returnType.Pointer || returnType.IsAlias || returnType.PointerDepth != 0
        || !returnType.ConstInnerDimensions.empty() || returnType.ConstArraySize != 0)
        return std::string();
    const std::string& name = returnType.TypeName;
    return name == "float" || name == "double" || name == "longdouble" ? name : std::string();
}
inline std::string ScalarArithmeticResultName(LLVMBackend* compiler,
                                               const std::string& left,
                                               const std::string& right,
                                               llvm::Type* resultType)
{
    struct Arith { int rank; bool isUnsigned; bool floating; };
    auto classify = [](const std::string& name, Arith& out) {
        static const std::unordered_map<std::string, Arith> table = {
            { "bool", { 4, false, false } }, { "char", { 4, false, false } },
            { "i8", { 4, false, false } }, { "u8", { 4, false, false } },
            { "short", { 4, false, false } }, { "i16", { 4, false, false } },
            { "u16", { 4, false, false } },   // integral promotion: all of these are `int`
            { "int", { 4, false, false } }, { "i32", { 4, false, false } },
            { "uint", { 4, true, false } }, { "u32", { 4, true, false } },
            { "long", { 5, false, false } }, { "ulong", { 5, true, false } },
            { "i64", { 6, false, false } }, { "u64", { 6, true, false } },
            { "float", { 7, false, true } }, { "double", { 8, false, true } },
            { "longdouble", { 9, false, true } },
        };
        auto it = table.find(name);
        if (it == table.end()) return false;
        out = it->second;
        return true;
    };
    auto nameOf = [](const Arith& a) -> std::string {
        static const char* const sNames[] = { "", "", "", "", "int", "long", "i64" };
        static const char* const uNames[] = { "", "", "", "", "uint", "ulong", "u64" };
        if (a.floating) return a.rank == 7 ? "float" : a.rank == 8 ? "double" : "longdouble";
        return a.isUnsigned ? uNames[a.rank] : sNames[a.rank];
    };
    // An unscoped C++ enum operand takes part as its promoted type.
    auto classifyOperand = [&](const std::string& name, Arith& out) {
        if (classify(name, out)) return true;
        const std::string promoted = name.empty() ? name : compiler->CxxEnumPromotedTypeName(name);
        return !promoted.empty() && classify(promoted, out);
    };
    Arith l{}, r{};
    const bool knownLeft = classifyOperand(left, l);
    const bool knownRight = classifyOperand(right, r);
    // A named operand outside the table (a scoped enum, a class) is no guess: no identity.
    if ((!knownLeft && !left.empty()) || (!knownRight && !right.empty())) return std::string();
    Arith result{};
    if (knownLeft && knownRight)
    {
        if (l.floating || r.floating)
            result = !r.floating || (l.floating && l.rank >= r.rank) ? l : r;
        else if (l.isUnsigned == r.isUnsigned)
            result = l.rank >= r.rank ? l : r;
        else
        {
            const Arith& u = l.isUnsigned ? l : r;
            const Arith& sgn = l.isUnsigned ? r : l;
            // The signed type wins only when wider: long is 64-bit on LP64, 32-bit on LLP64.
            auto bits = [&](const Arith& a) -> unsigned {
                if (a.rank != 5) return a.rank == 4 ? 32u : 64u;
                LLVMBackend::TypeAndValue longType;
                longType.TypeName = "long";
                llvm::Type* longLlvm = compiler->GetType(longType);
                return longLlvm != nullptr && longLlvm->isIntegerTy(32) ? 32u : 64u;
            };
            if (u.rank >= sgn.rank) result = u;
            else if (bits(sgn) > bits(u)) result = sgn;
            else result = { sgn.rank, true, false };
        }
    }
    else if (knownLeft || knownRight)
        result = knownLeft ? l : r;
    else
        return std::string();
    std::string name = nameOf(result);
    LLVMBackend::TypeAndValue type;
    type.TypeName = name;
    llvm::Type* identityType = name.empty() ? nullptr : compiler->GetType(type);
    // An unsuffixed floating literal is a C++ double that CFlat lowers as float: a floating
    // result may be narrower than its C++ type (`1e3 + 1` is double, computed as float).
    const bool widerFloating = identityType != nullptr && resultType != nullptr
        && identityType->isFloatingPointTy() && resultType->isFloatingPointTy()
        && identityType->getPrimitiveSizeInBits() > resultType->getPrimitiveSizeInBits();
    if (identityType == nullptr || (identityType != resultType && !widerFloating))
        return std::string();
    return name;
}
inline std::string CxxLiteralOperandIdentity(antlr4::ParserRuleContext* operand)
{
    if (operand == nullptr || operand->getStart() == nullptr || operand->getStop() == nullptr
        || operand->getStop()->getTokenIndex() < operand->getStart()->getTokenIndex()
        || operand->getStop()->getTokenIndex() - operand->getStart()->getTokenIndex() > 4)
        return std::string();
    return LLVMBackend::LiteralIdentityForOverload(operand->getText());
}
inline bool IsCflatArithmeticName(const std::string& name)
{
    static const std::set<std::string> names = {
        "bool", "char", "i8", "u8", "short", "i16", "u16", "int", "i32", "uint", "u32",
        "long", "i64", "ulong", "u64", "float", "double" };
    return names.count(name) != 0;
}
inline std::string CxxScalarOperandIdentity(LLVMBackend* compiler,
                                            const std::string& identity,
                                            const std::string& sourceTypeName,
                                            llvm::Value* value)
{
    if (!identity.empty()) return identity;
    if (compiler == nullptr || value == nullptr
        || value->getType()->isPointerTy() || value->getType()->isStructTy())
        return std::string();
    // Imported unscoped enumerators carry their enum type on the value. Check that
    // representation first, then use clang's registered integral promotion type.
    const std::string promotedEnum = compiler->CxxEnumPromotedTypeName(sourceTypeName);
    if (!promotedEnum.empty())
    {
        LLVMBackend::TypeAndValue enumType;
        enumType.TypeName = sourceTypeName;
        llvm::Type* declaredType = compiler->GetType(enumType, nullptr, false);
        return declaredType == value->getType() && declaredType->isIntegerTy()
            ? promotedEnum : std::string();
    }
    if (!IsCflatArithmeticName(sourceTypeName)) return std::string();
    LLVMBackend::TypeAndValue sourceType;
    sourceType.TypeName = sourceTypeName;
    llvm::Type* declaredType = compiler->GetType(sourceType, nullptr, false);
    if (declaredType == value->getType()
        && (declaredType->isIntegerTy() || declaredType->isFloatingPointTy()))
        return sourceTypeName;
    return std::string();
}
inline std::string ScalarTypeNameForValue(llvm::Type* type)
{
    if (type == nullptr) return std::string();
    if (type->isDoubleTy()) return "double";
    if (type->isFloatTy())  return "float";
    if (!type->isIntegerTy()) return std::string();
    switch (type->getIntegerBitWidth())
    {
        case 1:  return "bool";
        case 8:  return "char";
        case 16: return "short";
        case 32: return "int";
        case 64: return "long long";
        default: return std::string();
    }
}
inline std::string_view MirrorRelationSpelling(std::string_view op)
{
    if (op == "<") return ">";
    if (op == ">") return "<";
    if (op == "<=") return ">=";
    return "<=";
}
inline bool IsBinaryExpressionRuleWithOperator(antlr4::tree::ParseTree* node)
{
    if (auto* p = dynamic_cast<CFlatParser::MultiplicativeExpressionContext*>(node))
        return p->castExpression().size() > 1;
    if (auto* p = dynamic_cast<CFlatParser::AdditiveExpressionContext*>(node))
        return p->multiplicativeExpression().size() > 1;
    if (auto* p = dynamic_cast<CFlatParser::ShiftExpressionContext*>(node))
        return p->additiveExpression().size() > 1;
    if (auto* p = dynamic_cast<CFlatParser::SpaceshipExpressionContext*>(node))
        return p->shiftExpression().size() > 1;
    if (auto* p = dynamic_cast<CFlatParser::RelationalExpressionContext*>(node))
        return p->spaceshipExpression().size() > 1;
    if (auto* p = dynamic_cast<CFlatParser::EqualityExpressionContext*>(node))
        return p->typeCheckExpression().size() > 1;
    if (auto* p = dynamic_cast<CFlatParser::AndExpressionContext*>(node))
        return p->equalityExpression().size() > 1;
    if (auto* p = dynamic_cast<CFlatParser::ExclusiveOrExpressionContext*>(node))
        return p->andExpression().size() > 1;
    if (auto* p = dynamic_cast<CFlatParser::InclusiveOrExpressionContext*>(node))
        return p->exclusiveOrExpression().size() > 1;
    if (auto* p = dynamic_cast<CFlatParser::LogicalAndExpressionContext*>(node))
        return p->inclusiveOrExpression().size() > 1;
    if (auto* p = dynamic_cast<CFlatParser::LogicalOrExpressionContext*>(node))
        return p->logicalAndExpression().size() > 1;
    return false;
}
inline bool IsParenthesizedConditionalOperand(antlr4::tree::ParseTree* node)
{
    return ArmTernaryThroughParens(node) != nullptr;
}
template <typename LeafFn>
inline bool ForEachPointerPhiLeaf(llvm::Value* storage,
    llvm::SmallPtrSet<llvm::PHINode*, 16>& visited, LeafFn&& visitLeaf,
    bool emptyJoinSucceeds = false)
{
    if (auto* join = llvm::dyn_cast_or_null<llvm::PHINode>(storage))
    {
        if (!join->getType()->isPointerTy()) return visitLeaf(storage);
        if (join->getNumIncomingValues() == 0) return emptyJoinSucceeds;
        if (!visited.insert(join).second) return true;
        for (llvm::Value* incoming : join->incoming_values())
            if (!ForEachPointerPhiLeaf(incoming, visited, visitLeaf, emptyJoinSucceeds)) return false;
        return true;
    }
    return visitLeaf(storage);
}
inline bool TernaryIsBinaryOperand(CFlatParser::ConditionalExpressionContext* ctx)
{
    for (auto* parent = ctx != nullptr ? ctx->parent : nullptr; parent != nullptr;
         parent = parent->parent)
    {
        // A postfix suffix consumes the ternary before any surrounding built-in operator sees it;
        // only a bare parenthesized ternary is an operand of that operator.
        if (auto* postfix = dynamic_cast<CFlatParser::PostfixExpressionContext*>(parent))
            if (postfix->children.size() > 1) return false;
        if (IsBinaryExpressionRuleWithOperator(parent)) return true;
        if (auto* assignment = dynamic_cast<CFlatParser::AssignmentExpressionContext*>(parent))
            if (assignment->assignmentOperator() != nullptr) return true;
    }
    return false;
}
inline bool TernaryIsAddressOperand(CFlatParser::ConditionalExpressionContext* ctx)
{
    antlr4::tree::ParseTree* from = ctx;
    for (antlr4::tree::ParseTree* node = ctx != nullptr ? ctx->parent : nullptr;
         node != nullptr; from = node, node = node->parent)
    {
        // `unaryOperator castExpression` is the two-child form; a one-child unary is a pass-through.
        if (auto* unary = dynamic_cast<CFlatParser::UnaryExpressionContext*>(node);
            unary != nullptr && unary->children.size() == 2)
            return unary->children[0]->getText() == "&";
        if (auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node);
            primary != nullptr && primary->children.size() == 3 && primary->expression() != nullptr)
            continue;
        if (auto* cond = dynamic_cast<CFlatParser::ConditionalExpressionContext*>(node);
            cond != nullptr && cond->children.size() > 1)
        {
            // Only a `?:` arm passes the address on; its condition (children[0]) yields a bool.
            if (cond->Question() == nullptr || cond->children[0] == from) return false;
            continue;
        }
        auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(node);
        if (rule == nullptr || rule->children.size() != 1) return false;
    }
    return false;
}
inline bool TernaryIsMemberReceiver(CFlatParser::ConditionalExpressionContext* ctx)
{
    antlr4::tree::ParseTree* from = ctx;
    for (antlr4::tree::ParseTree* node = ctx != nullptr ? ctx->parent : nullptr;
         node != nullptr; from = node, node = node->parent)
    {
        if (auto* postfix = dynamic_cast<CFlatParser::PostfixExpressionContext*>(node))
        {
            // A one-child postfix is a pass-through (a parenthesized arm's own postfix node).
            if (postfix->children.size() > 1)
                return postfix->children[0] == from && postfix->children[1]->getText() == ".";
            continue;
        }
        if (auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node);
            primary != nullptr && primary->children.size() == 3 && primary->expression() != nullptr)
            continue;
        if (auto* cond = dynamic_cast<CFlatParser::ConditionalExpressionContext*>(node);
            cond != nullptr && cond->children.size() > 1)
        {
            if (cond->Question() == nullptr || cond->children[0] == from) return false;
            continue;
        }
        auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(node);
        if (rule == nullptr || rule->children.size() != 1) return false;
    }
    return false;
}
inline bool ParseTreeContainsTernary(antlr4::tree::ParseTree* node)
{
    if (node == nullptr) return false;
    if (auto* conditional = dynamic_cast<CFlatParser::ConditionalExpressionContext*>(node))
        if (conditional->Question() != nullptr) return true;
    for (auto* child : node->children)
        if (ParseTreeContainsTernary(child)) return true;
    return false;
}
inline bool ParseTreeContainsDefault(antlr4::tree::ParseTree* node)
{
    if (auto* terminal = dynamic_cast<antlr4::tree::TerminalNode*>(node))
        return terminal->getSymbol()->getType() == CFlatParser::Default;
    if (node == nullptr) return false;
    for (auto* child : node->children)
        if (ParseTreeContainsDefault(child)) return true;
    return false;
}
inline bool IsParenthesizedBinaryOperandShape(antlr4::ParserRuleContext* ctx)
{
    bool sawParens = false;
    for (antlr4::tree::ParseTree* node = ctx->parent; node != nullptr; node = node->parent)
    {
        if (dynamic_cast<CFlatParser::MultiplicativeExpressionContext*>(node) != nullptr
            || dynamic_cast<CFlatParser::AdditiveExpressionContext*>(node) != nullptr
            || dynamic_cast<CFlatParser::ShiftExpressionContext*>(node) != nullptr
            || dynamic_cast<CFlatParser::SpaceshipExpressionContext*>(node) != nullptr
            || dynamic_cast<CFlatParser::RelationalExpressionContext*>(node) != nullptr
            || dynamic_cast<CFlatParser::EqualityExpressionContext*>(node) != nullptr
            || dynamic_cast<CFlatParser::AndExpressionContext*>(node) != nullptr
            || dynamic_cast<CFlatParser::ExclusiveOrExpressionContext*>(node) != nullptr
            || dynamic_cast<CFlatParser::InclusiveOrExpressionContext*>(node) != nullptr)
        {
            if (node->children.size() > 1) return sawParens;
            continue;
        }
        if (auto* unary = dynamic_cast<CFlatParser::UnaryExpressionContext*>(node);
            unary != nullptr && unary->children.size() == 2)
        {
            // `unaryOperator castExpression`: only operators that route to overloads.
            auto* op = dynamic_cast<CFlatParser::UnaryOperatorContext*>(unary->children[0]);
            if (op == nullptr) return false;
            const std::string text = op->getText();
            return sawParens && (text == "-" || text == "+" || text == "~" || text == "!"
                || text == "*");
        }
        if (dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node) != nullptr)
        {
            auto* open = node->children.size() == 3
                ? dynamic_cast<antlr4::tree::TerminalNode*>(node->children[0]) : nullptr;
            if (open == nullptr || open->getSymbol()->getType() != CFlatParser::LeftParen)
                return false;
            sawParens = true;
            continue;
        }
        if (node->children.size() != 1) return false;
    }
    return false;
}
inline unsigned BinaryOperandBits(llvm::Value* value)
{
    return value != nullptr && value->getType()->isIntegerTy()
        ? value->getType()->getIntegerBitWidth() : 0;
}
inline bool BinaryJoinIsUnsigned(bool leftUnsigned, unsigned leftBits,
                                 bool rightUnsigned, unsigned rightBits)
{
    if (leftBits == 0 || rightBits == 0)
        return leftUnsigned || rightUnsigned;

    // C promotes every integer narrower than int to signed int before this rank check.
    if (leftBits < 32) { leftBits = 32; leftUnsigned = false; }
    if (rightBits < 32) { rightBits = 32; rightUnsigned = false; }

    if (leftUnsigned == rightUnsigned)
        return leftUnsigned;
    return leftUnsigned ? leftBits >= rightBits : rightBits >= leftBits;
}
inline std::string UnrelatedPointerJoinDiagnostic(
    LLVMBackend* compiler, LLVMBackend::TypeAndValue a, LLVMBackend::TypeAndValue b,
    const char* op)
{
    auto spell = [&](LLVMBackend::TypeAndValue type) {
        type.TypeName = compiler->ResolveTypeAlias(type.TypeName);
        if (type.PointerDepth == 0 && !type.ElemPointer) type.PointerDepth = 1;
        return SpellDiagnosticType(*compiler, type);
    };
    return std::format("cannot join '{}' and '{}' in a '{}': the pointee types differ; "
                       "use an explicit cast on one arm",
                       spell(std::move(a)), spell(std::move(b)), op);
}
inline CFlatParser::ConditionalExpressionContext* ArmTernaryThroughParens(
        antlr4::tree::ParseTree* node) {
        if (auto* cond = dynamic_cast<CFlatParser::ConditionalExpressionContext*>(node);
            cond != nullptr && cond->Question() != nullptr)
            return cond;
        auto* ctx = dynamic_cast<antlr4::ParserRuleContext*>(node);
        if (ctx == nullptr) return nullptr;
        if (ctx->children.size() == 1) return ArmTernaryThroughParens(ctx->children[0]);
        if (auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(ctx);
            primary != nullptr && primary->expression() != nullptr && primary->children.size() == 3
            && primary->children[0]->getText() == "(")
            return ArmTernaryThroughParens(primary->expression());
        return nullptr;
    }
inline bool PromoteNarrowUnaryOperand(LLVMBackend* compiler, llvm::Value*& value,
                                      LLVMBackend::NamedVariable& namedVar)
{
        if (value == nullptr || !value->getType()->isIntegerTy()) return false;
        unsigned bits = value->getType()->getIntegerBitWidth();
        if (bits != 8 && bits != 16) return false;
        bool isUnsigned = namedVar.TypeAndValue.IsUnsignedInteger() != -1;
        if (!isUnsigned && llvm::isa<llvm::ConstantInt>(value)) return false;
        value = compiler->PromoteToInt(value, isUnsigned);
        namedVar.TypeAndValue.TypeName = "i32";
        return true;
}
inline bool PromoteBooleanUnaryOperand(LLVMBackend* compiler, llvm::Value*& value,
                                       LLVMBackend::NamedVariable& namedVar)
{
        if (value == nullptr || !value->getType()->isIntegerTy(1)) return false;
        value = compiler->Upconvert(value,
            compiler->GetType(LLVMBackend::TypeAndValue{ .TypeName = "i32" }), false);
        namedVar.TypeAndValue.TypeName = "i32";
        return true;
}
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 10> kCompoundOperators = {{
    { "+=", "+" }, { "-=", "-" }, { "*=", "*" }, { "/=", "/" }, { "%=", "%" },
    { "&=", "&" }, { "|=", "|" }, { "^=", "^" }, { "<<=", "<<" }, { ">>=", ">>" }
}};
struct SpaceshipRelation { std::string_view spelling; int low; int high; };
inline constexpr std::array<SpaceshipRelation, 4> kSpaceshipRelations = {{
    { "<", -1, -1 }, { ">", 1, 1 }, { "<=", -1, 0 }, { ">=", 0, 1 }
}};
