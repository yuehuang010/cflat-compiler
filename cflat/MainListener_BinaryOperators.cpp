#include "MainListener.h"
#include <llvm/Support/SaveAndRestore.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicInst.h>
#include "CxxSpelling.h"
#include "LLVMBackend_CInteropShared.h"
#include "MainListener_ExpressionsShared.h"
// Call an operator template without non-template overloads rejected for operand cv/category.
template <typename Table>
static llvm::Value* CallOperatorWithoutExcluded(
    Table& functionTable, LLVMBackend* compiler, const std::string& lookupName, const std::string& callee,
    const std::vector<LLVMBackend::NamedVariable>& args, const std::set<std::string>& excludedLinkages)
{
    std::vector<std::string> names{ lookupName };
    if (callee != lookupName) names.push_back(callee);
    std::vector<std::pair<std::string, std::vector<LLVMBackend::FunctionSymbol>>> saved;
    if (!excludedLinkages.empty())
        for (const auto& name : names)
            if (auto table = functionTable.find(name); table != functionTable.end())
            {
                saved.emplace_back(name, table->second);
                std::erase_if(table->second, [&](const LLVMBackend::FunctionSymbol& candidate) {
                    return excludedLinkages.count(candidate.UniqueName) != 0;
                });
            }
    auto restoreTables = [&]() {
        for (auto& [name, original] : saved)
        {
            auto restore = functionTable.find(name);
            if (restore == functionTable.end()) continue;
            auto afterCall = std::move(restore->second);
            restore->second = original;
            for (auto& appended : afterCall)
                if (std::none_of(restore->second.begin(), restore->second.end(),
                    [&](const LLVMBackend::FunctionSymbol& existing) {
                        return existing.UniqueName == appended.UniqueName;
                    }))
                    restore->second.push_back(std::move(appended));
        }
    };
    llvm::Value* result = nullptr;
    try
    {
        result = compiler->CreateOverloadedFunctionCall(callee, args, true);
    }
    catch (...)
    {
        restoreTables();
        throw;
    }
    restoreTables();
    return result;
}

bool MainListener::CastTypeNameNamesValue(CFlatParser::CastTypeNameContext* typeName) {
        if (typeName == nullptr || typeName->typeQualifier() != nullptr
            || typeName->abstractDeclarator() != nullptr || typeName->typeSpecifier().size() != 1)
            return false;
        auto* typeSpec = typeName->typeSpecifier()[0];
        auto* compiler = Compiler();
        // `Spec<args>.name`: a value exactly when the member lookup says it is not a type.
        if (auto* memberType = typeSpec->memberTypeSpecifier())
            return !MemberTypeSpecifierNamesType(memberType);
        std::string text;
        if (auto* gid = typeSpec->genericIdentifier(); gid != nullptr && gid->genericTypeParameters() == nullptr)
            text = gid->getText();
        else if (auto* qid = typeSpec->qualifiedGenericIdentifier();
                 qid != nullptr && qid->genericTypeParameters() == nullptr)
            text = qid->getText();
        if (text.empty()
            || activeTypeSubstitutions.count(text) != 0 || activePackSubstitutions.count(text) != 0
            || compiler->IsTypeArgTypeKey(text))
            return false;
        const size_t dot = text.find('.');
        // A visible variable heads a member access (`(s.x)`); a bare name that is also a type
        // stays on the cast path, which reports the ambiguity.
        if (NamesVisibleVariable(compiler, text.substr(0, dot)))
            return dot != std::string::npos || !compiler->IsKnownTypeName(text);
        if (dot == std::string::npos) return false;
        // A dotted global: a namespace constant (`ns.K`) or an enumerator (`E.v`).
        if (compiler->GetGlobalVariableNV(text).Storage != nullptr
            || compiler->GetGlobalVariableNV(compiler->ResolveQualifiedName(text)).Storage != nullptr)
            return true;
        const std::string owner = text.substr(0, text.rfind('.'));
        if (!compiler->GetEnumBackingType(compiler->ResolveTypeAlias(owner)).empty())
            return !compiler->IsKnownTypeName(text);
        return false;
}

CFlatParser::CastExpressionContext* MainListener::FindValueCastHazard(antlr4::tree::ParseTree* node) {
        // Only the expression levels an and-expression owns down to its casts and unary operands;
        // a parenthesized sub-expression, call or index is its own and-expression.
        if (auto* cast = dynamic_cast<CFlatParser::CastExpressionContext*>(node))
        {
            if (cast->castTypeName() != nullptr && cast->castExpression() != nullptr)
            {
                auto* operand = cast->castExpression()->unaryExpression();
                auto* op = operand != nullptr ? operand->unaryOperator() : nullptr;
                if (op != nullptr)
                {
                    const std::string opText = op->getText();
                    if ((opText == "+" || opText == "-" || opText == "*" || opText == "&")
                        && CastTypeNameNamesValue(cast->castTypeName()))
                        return cast;
                }
            }
            if (cast->castExpression() != nullptr) return FindValueCastHazard(cast->castExpression());
            return cast->unaryExpression() != nullptr ? FindValueCastHazard(cast->unaryExpression()) : nullptr;
        }
        if (auto* unary = dynamic_cast<CFlatParser::UnaryExpressionContext*>(node))
            return unary->unaryOperator() != nullptr && unary->castExpression() != nullptr
                ? FindValueCastHazard(unary->castExpression()) : nullptr;
        if (dynamic_cast<CFlatParser::EqualityExpressionContext*>(node) == nullptr
            && dynamic_cast<CFlatParser::TypeCheckExpressionContext*>(node) == nullptr
            && dynamic_cast<CFlatParser::RelationalExpressionContext*>(node) == nullptr
            && dynamic_cast<CFlatParser::ShiftExpressionContext*>(node) == nullptr
            && dynamic_cast<CFlatParser::AdditiveExpressionContext*>(node) == nullptr
            && dynamic_cast<CFlatParser::MultiplicativeExpressionContext*>(node) == nullptr
            && dynamic_cast<CFlatParser::AndExpressionContext*>(node) == nullptr)
            return nullptr;
        for (auto* child : node->children)
            if (auto* found = FindValueCastHazard(child)) return found;
        return nullptr;
}

static void CollectValueCastTokens(antlr4::tree::ParseTree* node, CFlatParser::CastExpressionContext* hazard,
                                   std::vector<std::unique_ptr<antlr4::Token>>& out, bool& clean)
{
        if (auto* terminal = dynamic_cast<antlr4::tree::TerminalNode*>(node))
        {
            if (dynamic_cast<antlr4::tree::ErrorNode*>(node) != nullptr) { clean = false; return; }
            const bool open = !hazard->children.empty() && hazard->children.front() == node;
            const bool close = hazard->children.size() > 2 && hazard->children[2] == node;
            // The extra parenthesis copies the original one, so every position stays exact.
            if (open) out.push_back(std::make_unique<antlr4::CommonToken>(terminal->getSymbol()));
            out.push_back(std::make_unique<antlr4::CommonToken>(terminal->getSymbol()));
            if (close) out.push_back(std::make_unique<antlr4::CommonToken>(terminal->getSymbol()));
            return;
        }
        for (auto* child : node->children)
            CollectValueCastTokens(child, hazard, out, clean);
}

CFlatParser::AndExpressionContext* MainListener::ReparseValueCast(CFlatParser::AndExpressionContext* ctx,
                                                                  CFlatParser::CastExpressionContext* hazard) {
        const std::string text = ctx->getText();
        if (auto it = valueCastReparses_.find(ctx); it != valueCastReparses_.end()
            && it->second->hazard == hazard && it->second->text == text)
            return it->second->root;
        std::vector<std::unique_ptr<antlr4::Token>> copied;
        bool clean = true;
        CollectValueCastTokens(ctx, hazard, copied, clean);
        if (!clean) return nullptr;
        // The token stream renumbers its tokens from zero; analyses keyed on the token index
        // (last use of a C++ param, shadowing, container aliases) need the original index back.
        std::vector<std::pair<antlr4::CommonToken*, size_t>> originalIndex;
        originalIndex.reserve(copied.size());
        for (auto& token : copied)
            originalIndex.emplace_back(static_cast<antlr4::CommonToken*>(token.get()), token->getTokenIndex());
        auto entry = std::make_unique<ValueCastReparse>();
        entry->source = std::make_unique<antlr4::ListTokenSource>(std::move(copied));
        entry->tokens = std::make_unique<antlr4::CommonTokenStream>(entry->source.get());
        entry->parser = std::make_unique<CFlatParser>(entry->tokens.get());
        entry->parser->removeErrorListeners();
        entry->parser->setErrorHandler(std::make_shared<antlr4::BailErrorStrategy>());
        CFlatParser::AndExpressionContext* root = nullptr;
        try { root = entry->parser->andExpression(); NormalizeForInitTree(*entry->parser, root); }
        catch (const antlr4::ParseCancellationException&) { return nullptr; }
        if (root == nullptr || entry->parser->getNumberOfSyntaxErrors() != 0
            || entry->tokens->LA(1) != cflat::kTokenEOF)
            return nullptr;
        for (auto& [token, index] : originalIndex) token->setTokenIndex(index);
        // Upward walks (enclosing statement, function, return) see the original context.
        root->parent = ctx->parent;
        entry->root = root;
        entry->hazard = hazard;
        entry->text = text;
        valueCastReparses_[ctx] = std::move(entry);
        return root;
}

LLVMBackend::TypedValue MainListener::ParseAndExpression(CFlatParser::AndExpressionContext* ctx, ResultUse use) {
        // `(value) op y` read as a cast: evaluate the C reading instead (see ValueCastReparse).
        if (auto* hazard = FindValueCastHazard(ctx))
            if (auto* reparsed = ReparseValueCast(ctx, hazard))
                return ParseAndExpression(reparsed, use);
        CallPostfixScope operatorPostfix(this);
        DeclExpectedTypeGate declExpectedGate(&declExpectedType, ctx->children.size() == 1);
        auto nextCtxs = ctx->equalityExpression();
        if (nextCtxs.size() == 1)
            return ParseEqualityExpression(nextCtxs[0], use);

        if (nextCtxs.size() > 1)
        {
            auto* compiler = Compiler(ctx);
            compiler->lastCallReturnsOwned = false;
            auto lv = ParseCxxFoldOperand(compiler, [&] {
                return ParseEqualityExpression(nextCtxs[0], ResultUse::Value);
            });
            NormalizeCxxReferenceOperand(ctx, lv);
            RegisterBorrowedStringOperandTemp(compiler, lv.value);
            compiler->RegisterOwnedPtrTemp(lv.value);
            llvm::Value* acc = lv.value;
            LLVMBackend::TypedValue arithRunning = lv;
            std::string accumulatorCxxArithIdentity = CxxScalarOperandIdentity(
                compiler, lv.cxxArithIdentity, lv.sourceTypeName, lv.value);
            if (accumulatorCxxArithIdentity.empty())
                accumulatorCxxArithIdentity = CxxLiteralOperandIdentity(nextCtxs[0]);
            llvm::Value* accumulatorStorage = lv.receiverStorage;
            bool accumulatorIsRvalue = lv.isRvalue;
            llvm::Type* accumulatorRefType = nullptr;
            std::string accumulatorSourceTypeName = lv.sourceTypeName;
            bool unsignedStorage = false;
            for (size_t i = 1; i < nextCtxs.size(); i++)
            {
                compiler->lastCallReturnsOwned = false;
                auto rv = ParseCxxFoldOperand(compiler, [&] {
                    return ParseEqualityExpression(nextCtxs[i], ResultUse::Value);
                });
                NormalizeCxxReferenceOperand(ctx, rv);
                RegisterBorrowedStringOperandTemp(compiler, rv.value);
                compiler->RegisterOwnedPtrTemp(rv.value);
                CxxFoldSretScope foldSret(compiler, i + 1 < nextCtxs.size());
                // A struct operand routes to 'operator&' the way the shift/relational paths do;
                // without it an overloaded receiver reaches the raw integer op and fails the verifier.
                auto* overload = TryBinaryOperatorOverload(acc, "&", rv.value, ctx, lv.elemType,
                                                           rv.pointerDepth, rv.elemPointer,
                                                           accumulatorStorage, rv.receiverStorage,
                                                           true, true, accumulatorIsRvalue, rv.isRvalue,
                                                           acc == lv.value ? lv.sourceTypeName
                                                                           : std::string(),
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
                                                           rv.cxxObjectCv);
                if (overload == nullptr && acc != nullptr && rv.value != nullptr
                    && (acc->getType()->isStructTy() || rv.value->getType()->isStructTy()))
                    LogErrorContext(ctx, "no overload of 'operator&' matches the given arguments.");
                if (overload != nullptr)
                {
                    LLVMBackend::NamedVariable resultNV;
                    resultNV.Primary = overload;
                    resultNV.TypeAndValue = compiler->lastCallReturnType;
                    DiagnoseVoidResultConsumed(ctx, resultNV, use, "'operator&'");
                    CarryCxxOperatorResult(ctx, overload, i + 1 < nextCtxs.size(), acc,
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
                    if (rightIdentity.empty()) rightIdentity = CxxLiteralOperandIdentity(nextCtxs[i]);
                    bool resultUnsigned = BinaryJoinIsUnsigned(
                        lv.isUnsigned, BinaryOperandBits(acc), rv.isUnsigned, BinaryOperandBits(rv.value));
                    const LLVMBackend::TypedValue arithLhs = ArithRunningOperand(arithRunning, acc);
                    acc = compiler->CreateOperation(LLVMBackend::Operation::BitwiseAnd, acc, rv.value,
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

        LogErrorContext(ctx, "Bitwise-AND expression has no operands.");
        return {};
    }

void MainListener::LowerInterfaceNullCompare(antlr4::ParserRuleContext* ctx,
                                   LLVMBackend::TypedValue& lv, LLVMBackend::TypedValue& rv) {
        auto* compiler = Compiler(ctx);
        auto* fatTy = compiler->GetFatPtrType();
        auto isFat  = [&](const LLVMBackend::TypedValue& v)
            { return v.value != nullptr && v.value->getType() == fatTy; };
        auto isNull = [&](const LLVMBackend::TypedValue& v)
            { return llvm::isa_and_nonnull<llvm::ConstantPointerNull>(v.value); };

        if (isFat(lv) && isFat(rv))
        {
            lv.value = compiler->builder->CreateExtractValue(lv.value, { 1u }, "iface_data");
            rv.value = compiler->builder->CreateExtractValue(rv.value, { 1u }, "iface_data");
        }
        else if (isFat(lv) && isNull(rv))
            lv.value = compiler->builder->CreateExtractValue(lv.value, { 1u }, "iface_data");
        else if (isFat(rv) && isNull(lv))
            rv.value = compiler->builder->CreateExtractValue(rv.value, { 1u }, "iface_data");
    }

LLVMBackend::TypedValue MainListener::ParseEqualityExpression(CFlatParser::EqualityExpressionContext* ctx, ResultUse use) {
        CallPostfixScope operatorPostfix(this);
        DeclExpectedTypeGate declExpectedGate(&declExpectedType, ctx->children.size() == 1);
        auto nextCtxs = ctx->typeCheckExpression();
        if (nextCtxs.size() == 1)
        {
            return ParseTypeCheckExpression(nextCtxs[0], use);
        }
        else if (nextCtxs.size() == 2)
        {
            // Each operand's moved-slot value reads are deferred (CheckMovedValueRead); the operand
            // result decides below which of them is exempt. Receiver and dereference uses never defer.
            auto parseOperand = [&](CFlatParser::TypeCheckExpressionContext* operand,
                                    std::vector<DeferredMovedRead>& deferred) {
                struct RestoreSink
                {
                    std::vector<DeferredMovedRead>*& sink;
                    std::vector<DeferredMovedRead>* saved;
                    ~RestoreSink() { sink = saved; }
                } restore{ deferredMovedReads_, deferredMovedReads_ };
                deferredMovedReads_ = &deferred;
                return ParseTypeCheckExpression(operand, ResultUse::Value);
            };
            // Clear lastCallReturnsOwned before each operand: a prior operand would leave it set
            // and the next (possibly a named variable) would be mis-registered and double-freed.
            // Each owned-string operand that is a call result is a temporary the comparison only
            // borrows; register it for end-of-statement cleanup (see RegisterBorrowedStringOperandTemp).
            // An owning-POINTER operand (`makePtr() != nullptr`) is the same shape: the comparison
            // yields a bool, so the pointer cannot escape and the temp is freed at the same point.
            Compiler(ctx)->lastCallReturnsOwned = false;
            std::vector<DeferredMovedRead> lvMovedReads, rvMovedReads;
            auto lv = parseOperand(nextCtxs[0], lvMovedReads);
            NormalizeCxxReferenceOperand(ctx, lv);
            RegisterBorrowedStringOperandTemp(Compiler(ctx), lv.value);
            Compiler(ctx)->RegisterOwnedPtrTemp(lv.value);
            Compiler(ctx)->lastCallReturnsOwned = false;
            auto rv = parseOperand(nextCtxs[1], rvMovedReads);
            NormalizeCxxReferenceOperand(ctx, rv);
            RegisterBorrowedStringOperandTemp(Compiler(ctx), rv.value);
            Compiler(ctx)->RegisterOwnedPtrTemp(rv.value);
            // A moved slot compared with the null constant is exempt only when the operand IS that
            // slot's load (`a[0] == nullptr`, `o.a[0].p == (nullptr)`). A read beneath the operand
            // (`a[0]->next`, a call argument) keeps its check. Decided on the resolved values only.
            const bool lvIsNull = llvm::isa_and_nonnull<llvm::ConstantPointerNull>(lv.value);
            const bool rvIsNull = llvm::isa_and_nonnull<llvm::ConstantPointerNull>(rv.value);
            auto settleMovedReads = [&](const std::vector<DeferredMovedRead>& reads,
                                        const LLVMBackend::TypedValue& operand, bool otherIsNull) {
                // Pointer-to-pointer casts keep the slot's value: compare through them, never through a
                // dereference or a non-pointer cast (those are new values, not the read's value).
                auto stripPointerCasts = [](llvm::Value* v) {
                    while (auto* cast = llvm::dyn_cast_or_null<llvm::BitCastInst>(v))
                    {
                        if (!cast->getSrcTy()->isPointerTy() || !cast->getDestTy()->isPointerTy()) break;
                        v = cast->getOperand(0);
                    }
                    return v;
                };
                llvm::Value* whole = stripPointerCasts(operand.value);
                for (const auto& read : reads)
                {
                    const bool wholeOperand = read.ownSlot && read.value != nullptr && whole != nullptr
                        && whole == stripPointerCasts(read.value);
                    if (!(otherIsNull && wholeOperand))
                        LogMovedUse(read.subject, read.line, read.column, read.pointeeMove);
                }
            };
            settleMovedReads(lvMovedReads, lv, rvIsNull);
            settleMovedReads(rvMovedReads, rv, lvIsNull);
            std::string op = ctx->children[1]->getText();

            Compiler(ctx)->RejectImplicitIntegerPointeePointerConversion(
                InferTernaryArmType(lv), InferTernaryArmType(rv));

            // Interface comparisons use the DATA pointer (fat-ptr field 1): a failed `as <Interface>`
            // yields a zeroed fat pointer, while fat-vs-fat compares test object identity.
            // Reduce each fat operand to its data pointer before comparing pointers.
            LowerInterfaceNullCompare(ctx, lv, rv);

            // A core unique value compares null through its validity method, avoiding a struct
            // versus pointer LLVM comparison and keeping the spelling independent of its origin.
            auto coreUniqueType = [](llvm::Value* value) -> std::string {
                auto* structTy = llvm::dyn_cast_or_null<llvm::StructType>(
                    value != nullptr ? value->getType() : nullptr);
                if (structTy == nullptr || !structTy->hasName()) return {};
                std::string name = structTy->getName().str();
                return MangledBase(name) == "unique" ? name : std::string();
            };
            // R5: the `unique` keyword's std::unique_ptr<T> compares through get(), as C++ does.
            auto cxxUniquePtrType = [&](llvm::Value* value) -> std::string {
                auto* structTy = llvm::dyn_cast_or_null<llvm::StructType>(
                    value != nullptr ? value->getType() : nullptr);
                if (structTy == nullptr || !structTy->hasName()) return {};
                std::string name = structTy->getName().str();
                return Compiler(ctx)->CxxUniquePtrPointee(name).empty() ? std::string() : name;
            };
            auto lowerCxxUniquePtrValue = [&](LLVMBackend::TypedValue& uniqueValue) {
                std::string typeName = cxxUniquePtrType(uniqueValue.value);
                if (typeName.empty()) return;
                LLVMBackend::NamedVariable receiver;
                receiver.Primary = uniqueValue.value;
                receiver.Storage = uniqueValue.receiverStorage;
                receiver.BaseType = uniqueValue.value->getType();
                receiver.TypeAndValue.TypeName = typeName;
                LLVMBackend::TypeAndValue rawType;
                rawType.TypeName = Compiler(ctx)->CxxUniquePtrPointee(typeName);
                rawType.Pointer = true;
                uniqueValue.value = Compiler(ctx)->CreateCoreUniqueRawPointerCall(receiver, rawType);
                auto elemType = rawType;
                elemType.Pointer = false;
                uniqueValue.elemType = Compiler(ctx)->GetType(elemType);
                uniqueValue.pointerDepth = 0;
                uniqueValue.elemPointer = false;
                uniqueValue.receiverStorage = nullptr;
            };
            lowerCxxUniquePtrValue(lv);
            lowerCxxUniquePtrValue(rv);
            auto lowerCoreUniqueNullCompare = [&](LLVMBackend::TypedValue& uniqueValue,
                                                   bool isEqual) -> llvm::Value* {
                std::string typeName = coreUniqueType(uniqueValue.value);
                if (typeName.empty() || !Compiler(ctx)->IsCoreUniqueType(typeName)) return nullptr;
                LLVMBackend::NamedVariable receiver;
                receiver.Primary = uniqueValue.value;
                receiver.Storage = uniqueValue.receiverStorage;
                receiver.BaseType = uniqueValue.value->getType();
                receiver.TypeAndValue.TypeName = typeName;
                auto* valid = Compiler(ctx)->CreateOverloadedFunctionCall("valid", { receiver });
                return isEqual ? Compiler(ctx)->builder->CreateNot(valid, "unique_isnull") : valid;
            };
            if (llvm::isa_and_nonnull<llvm::ConstantPointerNull>(rv.value))
                if (auto* result = lowerCoreUniqueNullCompare(lv, op == "=="))
                    return { result, false };
            if (llvm::isa_and_nonnull<llvm::ConstantPointerNull>(lv.value))
                if (auto* result = lowerCoreUniqueNullCompare(rv, op == "=="))
                    return { result, false };

            auto lowerCoreUniqueValue = [&](LLVMBackend::TypedValue& uniqueValue) {
                std::string typeName = coreUniqueType(uniqueValue.value);
                if (typeName.empty() || !Compiler(ctx)->IsCoreUniqueType(typeName)) return;
                LLVMBackend::NamedVariable receiver;
                receiver.Primary = uniqueValue.value;
                receiver.Storage = uniqueValue.receiverStorage;
                receiver.BaseType = uniqueValue.value->getType();
                receiver.TypeAndValue.TypeName = typeName;
                LLVMBackend::TypeAndValue rawType;
                rawType.TypeName = MangledGenericArgument(*Compiler(ctx), typeName);
                rawType.Pointer = true;
                uniqueValue.value = Compiler(ctx)->CreateCoreUniqueRawPointerCall(receiver, rawType);
                auto elemType = rawType;
                elemType.Pointer = false;
                uniqueValue.elemType = Compiler(ctx)->GetType(elemType);
                uniqueValue.pointerDepth = 0;
                uniqueValue.elemPointer = false;
                uniqueValue.receiverStorage = nullptr;
            };
            lowerCoreUniqueValue(lv);
            lowerCoreUniqueValue(rv);

            auto* overload = TryBinaryOperatorOverload(lv, op, rv, ctx, lv.elemType,
                                                       rv.pointerDepth, rv.elemPointer,
                                                       lv.receiverStorage, rv.receiverStorage,
                                                       true, true, lv.isRvalue, rv.isRvalue,
                                                       lv.sourceTypeName, rv.sourceTypeName,
                                                       lv.pointerDepth, lv.elemPointer,
                                                       {}, {}, false, {}, {}, false, {}, {},
                                                       rv.cxxPointeeConst, lv.cxxArithIdentity, rv.cxxArithIdentity, rv.isExplicitMove, lv.isExplicitMove,
                                                       lv.hasSourceType && lv.sourceType.IsCxxNullptrT,
                                                       rv.hasSourceType && rv.sourceType.IsCxxNullptrT,
                                                       false, lv.cxxPointeeConst,
                                                       lv.cxxObjectCv, rv.cxxObjectCv);
            if (overload)
            {
                LLVMBackend::NamedVariable resultNV;
                resultNV.Primary = overload;
                resultNV.TypeAndValue = Compiler(ctx)->lastCallReturnType;
                DiagnoseVoidResultConsumed(ctx, resultNV, use, std::format("'operator{}'", op));
            }
            llvm::Value* result = overload ? overload
                                           : Compiler(ctx)->CreateOperation(op, lv, rv, lv.isUnsigned, rv.isUnsigned);
            LLVMBackend::TypedValue typedResult{ result, false };
            if (overload && Compiler(ctx)->lastCallReturnType.IsAlias
                && Compiler(ctx)->IsCxxRecord(Compiler(ctx)->lastCallReturnType.TypeName))
            {
                CarryCxxOperatorResult(ctx, overload, false, typedResult.value,
                    typedResult.receiverStorage, typedResult.isRvalue,
                    typedResult.cxxRefValueType, typedResult.sourceTypeName);
                typedResult.isAlias = true;
                typedResult.storage = typedResult.receiverStorage;
            }
            return typedResult;
        }

        LogErrorContext(ctx, "Equality expression has unexpected operand count.");
        return {};
    }

void MainListener::ResolveGenericFunctionValueOperand(LLVMBackend::NamedVariable& namedVar,
                                                      antlr4::ParserRuleContext* ctx) {
        if (namedVar.Primary == nullptr && namedVar.GenericFunctionValue && !namedVar.CallerName.empty())
        {
            namedVar.Primary = Compiler(ctx)->GetFunctionForFuncPtr(namedVar.CallerName);
            if (auto* fn = llvm::dyn_cast_or_null<llvm::Function>(namedVar.Primary);
                fn != nullptr && !namedVar.TypeAndValue.IsFunctionPointer)
                if (const auto* sym = Compiler(ctx)->FindSymbolForFunction(fn))
                    namedVar.TypeAndValue = Compiler(ctx)->FuncPtrSigOfSymbol(*sym);
        }
}

LLVMBackend::TypedValue MainListener::TypedValueOfNamedOperand(LLVMBackend::NamedVariable& namedVar,
                                                     antlr4::ParserRuleContext* ctx) {
        // A generic function instantiation named as a VALUE (`wrap<int>`) publishes only its mangled
        // CallerName. Resolve it to the instantiated function, with its signature, the way a plain
        // named function value is, so a `?:` arm sees the function and can compare it with the other.
        // Only this value path resolves it: declarations and assignments keep the CallerName fallback
        // that binds the signature check.
        ResolveGenericFunctionValueOperand(namedVar, ctx);
        MaterializeNullConditionalOwningValue(namedVar, ctx);
        bool isUnsigned = namedVar.TypeAndValue.IsUnsignedInteger() != -1;
        llvm::Type* elemType = nullptr;
        if (namedVar.TypeAndValue.Pointer)
        {
            auto elemTV = namedVar.TypeAndValue;
            elemTV.ElemPointer ? (elemTV.ElemPointer = false) : (elemTV.Pointer = false, elemTV.IsInterfacePointer = false);
            elemType = Compiler(ctx)->GetType(elemTV);
        }
        if (!namedVar.TypeAndValue.IsArrayView)
        {
            if (auto* arrayType = llvm::dyn_cast_or_null<llvm::ArrayType>(namedVar.BaseType))
                elemType = arrayType->getElementType();
        }
        LLVMBackend::TypedValue result{ LoadNamedVariable(namedVar), isUnsigned };
        result.arithNarrowWidth = namedVar.ArithNarrowWidth;
        result.isAlias = namedVar.TypeAndValue.IsAlias || namedVar.IsAliasBorrow;
        result.stackCharBufferBorrow = namedVar.StackCharBufferBorrow;
        result.stackCharBufferSource = namedVar.StackCharBufferSource;
        result.stackCharBufferScopeDepth = namedVar.StackCharBufferScopeDepth;
        result.sourceCallerName = namedVar.CallerName.empty()
            ? namedVar.TypeAndValue.VariableName : namedVar.CallerName;
        result.sourceFieldName = namedVar.FieldName;
        result.sourceMovePathKey = namedVar.MovePathKey;
        result.sourceMovePathHasDynamicIndex = namedVar.MovePathHasDynamicIndex;
        result.sourceIsElementAccess = namedVar.IsElementAccess;
        result.sourceIsPointeeMove = namedVar.IsExplicitMove && namedVar.PointeeMove;
        result.sourceRootIsBorrowedThisField = namedVar.RootIsBorrowedThisField;
        result.sourceThisFieldPath = namedVar.ThisFieldPath;
        result.conditionalMoveSources = namedVar.ConditionalMoveSources;
        // An explicit `move` of a C++ record is an xvalue: it binds an rvalue reference as a temp.
        result.isRvalue = namedVar.IsRvalue
            || (namedVar.IsExplicitMove && Compiler(ctx)->IsCxxRecord(namedVar.TypeAndValue.TypeName));
        result.isCxxPrvalueConditional = namedVar.IsCxxPrvalueConditional;
        result.isCxxSameTypeCastCopy = namedVar.IsCxxSameTypeCastCopy;
        result.cxxRefValueType = namedVar.CxxRefValueType;
        result.isExplicitMove = namedVar.IsExplicitMove;
        result.storage = result.isAlias ? namedVar.Storage : nullptr;
        result.receiverStorage = namedVar.Storage;
        if (result.isAlias) Compiler(ctx)->RegisterAliasValue(result.value);
        if (namedVar.FromOwningTempField && !namedVar.OwningTempParent)
            Compiler(ctx)->RegisterTempFieldValue(result.value);
        result.elemType = elemType;
        result.isArrayView = namedVar.TypeAndValue.IsArrayView;
        result.sourceTypeName = namedVar.TypeAndValue.TypeName;
        result.cxxArithIdentity = namedVar.CxxArithIdentity;
        result.sourceType = namedVar.TypeAndValue;
        result.hasSourceType = true;
        result.cxxClosureLenders = namedVar.CxxClosureLenders;
        result.cxxPointeeConst = namedVar.TypeAndValue.PointeeConstBit()
            && namedVar.TypeAndValue.Pointer;
        result.cxxObjectCv = Compiler(ctx)->CxxOperandObjectCv(namedVar);
        result.joinClassPointee = namedVar.JoinClassPointee;
        result.joinClassPointeeDepth = namedVar.JoinClassPointeeDepth;
        OperandPointerDepth(namedVar, result.value, result.pointerDepth, result.elemPointer);
        return result;
    }

/*
 * The pointer depth an operand's VALUE is proven to have, for an operator's right operand whose
 * raw llvm::Value has lost it. 0 = unrecorded; -1 = a pointer no depth describes (`T(*)[N]`).
 */
void MainListener::OperandPointerDepth(const LLVMBackend::NamedVariable& namedVar, llvm::Value* value,
                                       int& depth, bool& elemPointer) {
        const auto& tv = namedVar.TypeAndValue;
        const bool isPointerValue = value != nullptr && value->getType()->isPointerTy();
        // A fixed array - or a row of one (`m[1]` of `char[4][4]`) - decays to a pointer to its
        // element: element stars + 1. A row of rows decays to `T(*)[N]` instead.
        if (auto* arrTy = llvm::dyn_cast_or_null<llvm::ArrayType>(namedVar.BaseType);
            arrTy != nullptr && isPointerValue && !tv.IsArrayView && !tv.IsSimd
            && LLVMBackend::IsPrimitiveTypeName(tv.TypeName) && tv.TypeName != "void")
        {
            depth = arrTy->getElementType()->isArrayTy() ? -1 : FixedArrayElementStars(tv) + 1;
            elemPointer = depth >= 2;
            return;
        }
        // Only a depth that is about THIS value is carried; everything else stays unrecorded.
        if (tv.DepthIsAboutThisValue())
        {
            depth = tv.PointerDepth;
            elemPointer = tv.ElemPointer;
            // No recorded depth: the Pointer / ElemPointer bits still name it (`&c`, `u.c_str()`).
            if (depth == 0 && isPointerValue) depth = tv.ValuePointerDepth();
        }
        // A fixed 1-D `T[N]` of a primitive decays to a `T*` value: the depth is PROVEN at 1.
        else if (tv.ConstArraySize > 0 && tv.ConstInnerDimensions.empty() && !tv.Pointer
                 && !tv.ElemPointer && !tv.IsArrayView && !tv.IsSimd && isPointerValue
                 && LLVMBackend::IsPrimitiveTypeName(tv.TypeName) && tv.TypeName != "void")
            depth = 1;
    }

CFlatParser::CastExpressionContext* MainListener::SoleCastOperandOf(CFlatParser::RelationalExpressionContext* relCtx) {
        if (relCtx == nullptr) return nullptr;
        auto shifts = relCtx->shiftExpression();
        if (shifts.size() != 1) return nullptr;
        auto adds = shifts[0]->additiveExpression();
        if (adds.size() != 1) return nullptr;
        auto muls = adds[0]->multiplicativeExpression();
        if (muls.size() != 1) return nullptr;
        auto casts = muls[0]->castExpression();
        return casts.size() == 1 ? casts[0] : nullptr;
    }

LLVMBackend::TypedValue MainListener::ParseTypeCheckExpression(CFlatParser::TypeCheckExpressionContext* ctx, ResultUse use) {
        DeclExpectedTypeGate declExpectedGate(&declExpectedType, ctx->children.size() == 1);
        auto relCtx = ctx->relationalExpression();
        if (!relCtx)
        {
            LogErrorContext(ctx, "Type check expression has no operand.");
            return {};
        }

        // An 'is'/'as' operand is parsed through the cast level when it is a plain passthrough,
        // so the source binding survives; every other shape keeps today's relational path.
        auto typeSpecs = ctx->typeSpecifier();
        LLVMBackend::NamedVariable srcNV;
        const LLVMBackend::NamedVariable* srcBinding = nullptr;
        LLVMBackend::TypedValue tv;
        auto* castOperand = typeSpecs.empty() ? nullptr : SoleCastOperandOf(relCtx);
        if (castOperand != nullptr)
        {
            srcNV = ParseCastExpression(castOperand);
            tv = TypedValueOfNamedOperand(srcNV, castOperand);
            // `owner is/as X` on a blessed unique<I> probes the BORROWED interface value; the
            // wrapper keeps its scope-exit release.
            auto* castCompiler = Compiler(ctx);
            if (!srcNV.TypeAndValue.Pointer
                && castCompiler->IsCoreUniqueType(srcNV.TypeAndValue.TypeName)
                && castCompiler->IsInterfaceType(MangledGenericArgument(
                    *castCompiler, srcNV.TypeAndValue.TypeName)))
            {
                auto coreUnique = srcNV;
                coreUnique.TypeAndValue.VariableName.clear();
                auto* borrowed = castCompiler->CreateOverloadedFunctionCall("get", { coreUnique });
                std::string ifaceName = MangledGenericArgument(
                    *castCompiler, srcNV.TypeAndValue.TypeName);
                srcNV.TypeAndValue = {};
                srcNV.TypeAndValue.TypeName = ifaceName;
                srcNV.TypeAndValue.IsInterface = true;
                srcNV.Storage = nullptr;
                srcNV.Primary = borrowed;
                srcNV.BaseType = borrowed != nullptr ? borrowed->getType() : nullptr;
                srcNV.IsOwning = false;
                tv.value = borrowed;
                tv.elemType = nullptr;
            }
            srcBinding = &srcNV;
        }
        else
        {
            tv = ParseRelationalExpression(relCtx, typeSpecs.empty() ? use : ResultUse::Value);
        }
        llvm::Value* result = tv.value;
        llvm::Type* srcElemType = tv.elemType;

        // A REDUNDANT `x as T` (T is x's own value type) is a spelling of x, not a conversion:
        // publish the operand binding so the whole-expression consumer can adopt its provenance.
        lastRedundantAsCtx_ = nullptr;
        if (typeSpecs.size() == 1 && srcBinding != nullptr && ctx->children.size() >= 2
            && ctx->children[1]->getText() == "as")
        {
            LLVMBackend::TypeAndValue asTarget;
            asTarget.TypeName = ParseTypeSpecifierName(typeSpecs[0],
                ctx->multiWordTypeSuffix().empty() ? nullptr : ctx->multiWordTypeSuffix()[0]);
            if (IsRedundantCastOfSource(srcNV.TypeAndValue, asTarget))
            {
                lastRedundantAsCtx_ = ctx;
                lastRedundantAsNamed_ = srcNV;
            }
        }

        if (typeSpecs.size() > 0)
        {
            for (size_t i = 0; i < typeSpecs.size(); i++)
            {
                std::string op = ctx->children[2 * i + 1]->getText();  // 'is' or 'as' token
                std::string targetTypeName = ParseTypeSpecifierName(typeSpecs[i],
                    i < ctx->multiWordTypeSuffix().size() ? ctx->multiWordTypeSuffix()[i] : nullptr);

                // Name the source when a binding survived, so ClassifyCastSource can fill in
                // shape.TypeName for an interface-valued source (an LLVM fat pointer carries no
                // interface identity, so the name can only come from here).
                std::string srcTypeName = srcBinding != nullptr ? srcBinding->TypeAndValue.TypeName
                                                                : std::string{};
                /*
                 * M6 - `is` / `as` on an imported C++ class is a dynamic_cast, which needs the
                 * Itanium RTTI ABI (__dynamic_cast plus a typeinfo reference). CFlat's own is/as
                 * is driven by its interface type descriptors, which a C++ object does not carry,
                 * so answering from them would be silently wrong. Refuse instead.
                 */
                auto* cxxCompiler = Compiler(ctx);
                if (cxxCompiler->IsCxxRecord(srcTypeName) || cxxCompiler->IsCxxRecord(targetTypeName))
                {
                    LogErrorContext(ctx, std::format(
                        "'{}' is not supported on the imported C++ class '{}': a C++ runtime type "
                        "test is a dynamic_cast, which needs the C++ RTTI ABI (not supported yet). "
                        "Use a C++-side helper that performs the cast and returns the result",
                        op, cxxCompiler->DisplayCxxClassName(
                            cxxCompiler->IsCxxRecord(srcTypeName) ? srcTypeName : targetTypeName)));
                    return { result, false };
                }
                if (op == "is")
                {
                    result = GenerateIsCheck(result, targetTypeName, ctx, srcElemType, srcTypeName, srcBinding);
                }
                else if (op == "as")
                {
                    result = GenerateSafeCast(result, targetTypeName, ctx, srcElemType, srcBinding,
                                              srcTypeName);
                }
                // Only the first operand's static type is known here; a chained is/as
                // operates on the previous op's result, whose element type isn't tracked.
                srcElemType = nullptr;
                srcBinding = nullptr;
            }
            return { result, false };  // is/as result is bool or pointer, not unsigned
        }

        return tv;
    }

llvm::Value* MainListener::LoadTypeDescFromInterface(llvm::Value* interfaceValue, antlr4::ParserRuleContext* ctx) {
        auto* compiler = Compiler(ctx);
        auto ptrTy = cflat_llvm::PointerTo(compiler->builder->getInt8Ty());

        llvm::Value* vtablePtr;
        if (interfaceValue->getType()->isStructTy())
        {
            vtablePtr = compiler->builder->CreateExtractValue(interfaceValue, {0u});
        }
        else
        {
            auto fatTy = compiler->GetFatPtrType();
            auto vtablePtrField = compiler->builder->CreateStructGEP(fatTy, interfaceValue, 0);
            vtablePtr = compiler->builder->CreateLoad(ptrTy, vtablePtrField);
        }

        // vtable[0] holds the type descriptor pointer
        auto typeDescField = compiler->builder->CreateGEP(ptrTy, vtablePtr, compiler->builder->getInt32(0));
        return compiler->builder->CreateLoad(ptrTy, typeDescField);
    }

std::string MainListener::ConcreteStructNameFromElemType(llvm::Type* elemType, LLVMBackend* compiler) {
        auto* st = elemType ? llvm::dyn_cast<llvm::StructType>(elemType) : nullptr;
        if (!st || !st->hasName()) return "";
        std::string name = st->getName().str();
        return compiler->dataStructures.count(name) ? name : "";
    }

std::string MainListener::ConcreteStructNameFromValue(llvm::Value* value, LLVMBackend* compiler) {
        auto* st = value ? llvm::dyn_cast<llvm::StructType>(value->getType()) : nullptr;
        if (!st || !st->hasName()) return "";
        std::string name = st->getName().str();
        return compiler->dataStructures.count(name) ? name : "";
    }

bool MainListener::ClassifyPointerShapedSource(llvm::Value* value, llvm::Type* elemType, LLVMBackend* compiler,
                                     LLVMBackend::TypeAndValue& shape,
                                     const LLVMBackend::NamedVariable* srcBinding) {
        if (srcBinding != nullptr)
        {
            const auto& declared = srcBinding->TypeAndValue;
            if (declared.ConstArraySize > 0 || (declared.Pointer && declared.ElemPointer))
            {
                shape = declared;
                return true;
            }
        }

        llvm::Type* valueType = value->getType();
        llvm::ArrayType* arrayType = llvm::dyn_cast<llvm::ArrayType>(valueType);
        if (arrayType == nullptr && valueType->isPointerTy())
        {
            if (auto* gep = llvm::dyn_cast<llvm::GEPOperator>(value))
                arrayType = llvm::dyn_cast<llvm::ArrayType>(gep->getSourceElementType());
            else if (auto* global = llvm::dyn_cast<llvm::GlobalVariable>(value))
                arrayType = llvm::dyn_cast<llvm::ArrayType>(global->getValueType());
        }
        if (arrayType != nullptr)
        {
            // Unwrap inner dimensions to reach the class; the reported size stays the outer one.
            llvm::Type* inner = arrayType->getElementType();
            while (auto* nested = llvm::dyn_cast<llvm::ArrayType>(inner)) inner = nested->getElementType();
            std::string elem = ConcreteStructNameFromElemType(inner, compiler);
            // An array OF POINTERS names no class this way; fall through to the declared type.
            if (!elem.empty())
            {
                shape.TypeName = elem;
                shape.ConstArraySize = arrayType->getNumElements();
                return true;
            }
        }

        // Shapes no llvm type can name: a `T**`, and a global whose array ELEMENT is itself a
        // pointer. Both come from the binding's declared type instead.
        const llvm::Value* storage = nullptr;
        if (llvm::isa<llvm::GlobalVariable>(value)) storage = value;
        else if (auto* load = llvm::dyn_cast<llvm::LoadInst>(value))
        {
            // Gated on a POINTER elemType, which a plain `T*` and an array VIEW are not - both
            // were already classified as concrete pointers above.
            if (elemType != nullptr && elemType->isPointerTy()) storage = load->getPointerOperand();
        }
        if (storage != nullptr)
        {
            const auto* declared = compiler->FindDeclaredTypeAndValueForStorage(storage);
            // A REGISTERED class or a BUILTIN primitive element: both are named types whose
            // boxability is decidable, and both spellings must reject a primitive array in step.
            bool namedElement = declared != nullptr
                && (compiler->dataStructures.count(declared->TypeName)
                    || LLVMBackend::IsPrimitiveTypeName(compiler->ResolveTypeAlias(declared->TypeName)));
            if (namedElement
                && !compiler->DescribePointerShapedInterfaceSource(*declared).empty())
            {
                shape = *declared;  // copy out now; the pointer aliases live map storage
                return true;
            }
        }
        return false;
    }

MainListener::CastSourceKind MainListener::ClassifyCastSource(llvm::Value* value, llvm::Type* elemType, LLVMBackend* compiler,
                                      std::string& structName, LLVMBackend::TypeAndValue& shape,
                                      const std::string& srcTypeName,
                                      const LLVMBackend::NamedVariable* srcBinding) {
        structName.clear();
        shape = LLVMBackend::TypeAndValue{};
        if (value == nullptr) return CastSourceKind::Unknown;
        llvm::Type* valueType = value->getType();
        auto* fatTy = compiler->GetFatPtrType();

        // A genuine interface value: the fat aggregate itself, or a pointer to one. `shape` is
        // populated on BOTH arms - it used to be left default-constructed here, which silently gave
        // every caller an empty TypeName for the entire interface-valued input class.
        if (valueType == fatTy || (valueType->isPointerTy() && elemType == fatTy))
        {
            shape.TypeName = srcTypeName;   // empty when the caller could not name the source
            shape.IsInterface = true;
            shape.Pointer = (valueType != fatTy);
            return CastSourceKind::InterfaceValue;
        }

        // Keep the declared array shape ahead of its decayed element type: both are available on
        // a fixed-array operand, but only the binding distinguishes the whole array from arr[i].
        if (ClassifyPointerShapedSource(value, elemType, compiler, shape, srcBinding))
            return CastSourceKind::PointerShaped;

        structName = ConcreteStructNameFromElemType(elemType, compiler);
        if (!structName.empty()) return CastSourceKind::ConcretePointer;
        structName = ConcreteStructNameFromValue(value, compiler);
        if (!structName.empty()) return CastSourceKind::ConcreteValue;

        // A pointer join carries no elemType; each leaf arm has its own concrete class. The
        // collector recognizes both a PHI from '?:' and the slot-backed load from '??'.
        if (valueType->isPointerTy() && !CollectPointerJoinArms(value).empty())
            return CastSourceKind::TernaryPointerJoin;

        return CastSourceKind::Unknown;
    }

llvm::Value* MainListener::AddressOfClassValueOperand(llvm::Value* value, LLVMBackend* compiler) {
        if (auto* load = llvm::dyn_cast<llvm::LoadInst>(value))
            return load->getPointerOperand();
        auto* slot = compiler->CreateAlloca(value->getType());
        compiler->CreateAssignment(value, slot);
        return slot;
    }

llvm::Value* MainListener::GenerateIsCheck(llvm::Value* interfaceValue, const std::string& targetTypeNameIn,
                                  antlr4::ParserRuleContext* ctx, llvm::Type* srcElemType,
                                  const std::string& srcTypeNameIn,
                                  const LLVMBackend::NamedVariable* srcBinding) {
        auto* compiler = Compiler(ctx);
        // targetTypeName is LOAD-BEARING: every direct interfaceTable.find/count below used to
        // miss an aliased spelling that IsInterfaceType (which resolves aliases) would accept -
        // that was the bug (see internal/issue p1, now closed). targetTypeNameIn (unresolved) is
        // kept around and used in every LogErrorContext message below, so a rejected 'AliasIA'
        // is reported as 'AliasIA', not as the interface it resolves to.
        //
        // srcTypeName's resolution here is DEFENSIVE, not load-bearing: srcTypeNameIn always
        // arrives already resolved (the caller reads it off a NamedVariable.TypeAndValue.TypeName,
        // which ParseDeclarationSpecifiers resolves at declaration time), so this is a no-op today.
        // Kept so a future caller that stops pre-resolving does not silently reintroduce the same
        // asymmetry on the source side - Test/test_interface.cb's iface_alias_is_source /
        // iface_alias_as_source legs are a tripwire for exactly that, not current-bug coverage.
        std::string targetTypeName = compiler->ResolveTypeAlias(targetTypeNameIn);
        std::string srcTypeName = compiler->ResolveTypeAlias(srcTypeNameIn);
        LLVMBackend::TypeAndValue targetType;
        targetType.TypeName = targetTypeNameIn;
        const std::string targetTypeSpelling = SpellType(*compiler, targetType);

        std::string srcStructName;
        LLVMBackend::TypeAndValue srcShape;
        CastSourceKind srcKind = ClassifyCastSource(interfaceValue, srcElemType, compiler,
                                                    srcStructName, srcShape, srcTypeName, srcBinding);

        // An unrouted generic-interface SOURCE has no method table, so its type-descriptor slot is
        // null and dereferencing it crashes. Only the SOURCE: as a TARGET the name is never
        // dereferenced (the answer is a pure implementor lookup, correctly false), and rejecting it
        // there is a false rejection - see scratch/rev4/p/t13_iscls.cb.
        compiler->RecordInterfaceMaterialization(srcShape.TypeName, "the source of 'is'");

        // Interface target: shared helper, shared wording. Guard on its bool exactly as the other
        // call sites do, so control cannot reach the concrete-target message if it stopped throwing.
        if (srcKind == CastSourceKind::PointerShaped)
        {
            // Membership gates on the resolved name; the message spells what the user wrote.
            if (compiler->HasInterface(targetTypeName)
                && RejectPointerShapedInterfaceUpcast(ctx, srcShape, targetTypeNameIn))
                return nullptr;
            LogErrorContext(ctx, std::format(
                "'is' needs an interface value or a single class instance, not '{}'",
                compiler->DescribePointerShapedInterfaceSource(srcShape)));
            return nullptr;
        }

        // A '?:' join has one concrete class per arm, so the answer is per arm too.
        if (srcKind == CastSourceKind::TernaryPointerJoin)
        {
            bool targetIsInterface = compiler->HasInterface(targetTypeName);
            if (!targetIsInterface && !compiler->dataStructures.count(targetTypeName))
            {
                LogErrorContext(ctx, std::format("'{}' is not a known struct or interface type for 'is' check", targetTypeSpelling));
                return nullptr;
            }
            std::string failure;
            if (auto* answer = BuildJoinArmPredicate(interfaceValue, targetTypeName,
                                                      targetIsInterface, compiler, failure))
                return answer;
            LogErrorContext(ctx, std::format("cannot test join arm against '{}': {}",
                                             targetTypeSpelling, failure));
            return nullptr;
        }

        if (srcKind == CastSourceKind::Unknown)
        {
            LogErrorContext(ctx, std::format(
                "'is' requires an interface value or a class instance on the left of '{}'; this "
                "expression is neither", targetTypeSpelling));
            return nullptr;
        }

        // Concrete source, pointer (e.g. `w is IW` where w is W*) or stack value (`s is IW`),
        // not a boxed interface value: no vtable/typedesc header to load, so the answer is
        // known at compile time.
        if (srcKind == CastSourceKind::ConcretePointer || srcKind == CastSourceKind::ConcreteValue)
        {
            if (compiler->HasInterface(targetTypeName))
                return compiler->builder->getInt1(compiler->StructImplementsInterface(srcStructName, targetTypeName));
            if (compiler->dataStructures.count(targetTypeName))
                return compiler->builder->getInt1(srcStructName == targetTypeName);

            LogErrorContext(ctx, std::format("'{}' is not a known struct or interface type for 'is' check", targetTypeSpelling));
            return nullptr;
        }

        // Interface target: true when the runtime type is any implementor of that interface.
        // An OR-chain of typedesc compares - the same enumeration `switch` on an interface uses.
        if (compiler->HasInterface(targetTypeName))
        {
            auto loadedDesc = LoadTypeDescFromInterface(interfaceValue, ctx);
            llvm::Value* result = compiler->builder->getInt1(false);
            for (auto& [sName, sd] : compiler->dataStructures)
            {
                if (!sd.typeDescriptor || !compiler->StructImplementsInterface(sName, targetTypeName)) continue;
                auto* cmp = compiler->builder->CreateICmpEQ(loadedDesc, sd.typeDescriptor);
                result = compiler->builder->CreateOr(result, cmp);
            }
            for (auto& [pName, pd] : compiler->programTable)
            {
                if (!pd.typeDescriptor || !compiler->StructImplementsInterface(pName, targetTypeName)) continue;
                auto* cmp = compiler->builder->CreateICmpEQ(loadedDesc, pd.typeDescriptor);
                result = compiler->builder->CreateOr(result, cmp);
            }
            return result;
        }

        auto targetIt = compiler->dataStructures.find(targetTypeName);
        if (targetIt == compiler->dataStructures.end())
        {
            LogErrorContext(ctx, std::format("'{}' is not a known struct type for 'is' check", targetTypeSpelling));
            return nullptr;
        }

        auto* typeDesc = targetIt->second.typeDescriptor;
        if (!typeDesc)
        {
            LogErrorContext(ctx, std::format("'{}' has no type descriptor", targetTypeSpelling));
            return nullptr;
        }

        auto loadedDesc = LoadTypeDescFromInterface(interfaceValue, ctx);
        return compiler->builder->CreateICmpEQ(loadedDesc, typeDesc);
    }

llvm::Value* MainListener::GenerateSafeCast(llvm::Value* interfaceValue, const std::string& targetTypeNameIn,
                                  antlr4::ParserRuleContext* ctx, llvm::Type* srcElemType,
                                  const LLVMBackend::NamedVariable* srcBinding,
                                  const std::string& srcTypeNameIn) {
        auto* compiler = Compiler(ctx);
        auto ptrTy = cflat_llvm::PointerTo(compiler->builder->getInt8Ty());
        // targetTypeName resolution is LOAD-BEARING (see the matching comment in
        // GenerateIsCheck); targetTypeNameIn (unresolved) is kept for every message below so a
        // rejected alias is reported by the name the user wrote. srcTypeName's resolution is
        // DEFENSIVE only - srcTypeNameIn always arrives pre-resolved already.
        std::string targetTypeName = compiler->ResolveTypeAlias(targetTypeNameIn);
        std::string srcTypeName = compiler->ResolveTypeAlias(srcTypeNameIn);
        LLVMBackend::TypeAndValue targetType;
        targetType.TypeName = targetTypeNameIn;
        const std::string targetTypeSpelling = SpellType(*compiler, targetType);

        std::string srcStructName;
        LLVMBackend::TypeAndValue srcShape;
        CastSourceKind srcKind = ClassifyCastSource(interfaceValue, srcElemType, compiler,
                                                    srcStructName, srcShape, srcTypeName, srcBinding);

        // An unrouted generic-interface SOURCE has no method table, so its type-descriptor slot is
        // null and dereferencing it crashes. Only the SOURCE: as a TARGET the name is never
        // dereferenced (the answer is a pure implementor lookup, correctly false), and rejecting it
        // there is a false rejection - see scratch/rev4/p/t13_iscls.cb.
        compiler->RecordInterfaceMaterialization(srcShape.TypeName, "the source of 'as'");
        bool srcIsValue = srcKind == CastSourceKind::ConcreteValue;

        // Not one object: same rejection, same wording, as the plain-assignment spelling. Guarded
        // on the helper's bool like every other call site (see its comment for why).
        if (srcKind == CastSourceKind::PointerShaped)
        {
            // Membership gates on the resolved name; the message spells what the user wrote.
            if (compiler->HasInterface(targetTypeName)
                && RejectPointerShapedInterfaceUpcast(ctx, srcShape, targetTypeNameIn))
                return nullptr;
            LogErrorContext(ctx, std::format(
                "cannot cast '{}' to '{}' - index or dereference it first to get a single instance",
                compiler->DescribePointerShapedInterfaceSource(srcShape), targetTypeSpelling));
            return nullptr;
        }

        // A pointer join carries no single concrete class: box each arm in its own block, exactly
        // as the plain-assignment path does. This includes the slot-backed '??' spelling.
        if (srcKind == CastSourceKind::TernaryPointerJoin)
        {
            if (compiler->HasInterface(targetTypeName))
            {
                std::string armFailure;
                std::string joinSpelling;
                if (auto* fat = UpcastPointerJoinToInterface(interfaceValue, targetTypeName,
                                                              &armFailure, &joinSpelling))
                    return fat;
                LogErrorContext(ctx, armFailure.empty()
                    ? std::format("cannot convert '{}' result to interface '{}'",
                                  joinSpelling.empty() ? "join" : joinSpelling, targetTypeSpelling)
                    : std::format("cannot convert '{}' arm to interface '{}': {}",
                                  joinSpelling.empty() ? "join" : joinSpelling,
                                  targetTypeSpelling, armFailure));
                return nullptr;
            }
            LogErrorContext(ctx, std::format(
                "cannot cast a join result to '{}'; 'as' performs a runtime-checked downcast only "
                "from an interface value - bind the join to a local first", targetTypeSpelling));
            return nullptr;
        }

        if (srcKind == CastSourceKind::Unknown)
        {
            LogErrorContext(ctx, std::format(
                "'as' requires an interface value or a class instance on the left of '{}'; this "
                "expression is neither", targetTypeSpelling));
            return nullptr;
        }

        // CONCRETE source, pointer (e.g. `w as X` where w is W*) or stack value (`s as X`).
        // A concrete object carries no vtable/typedesc header, so every target is resolved
        // statically here; this branch always returns, keeping the runtime-checked downcast
        // below interface-source-only.
        if (srcKind == CastSourceKind::ConcretePointer || srcKind == CastSourceKind::ConcreteValue)
        {
            // Interface target: the shared boxing helper, so this spelling carries the same
            // implements/shape/ownership guards the plain-assignment spelling does.
            if (compiler->HasInterface(targetTypeName))
            {
                return BoxConcreteIntoInterface(ctx, interfaceValue, !srcIsValue, srcStructName,
                                                targetTypeName, srcBinding);
            }

            if (compiler->dataStructures.count(targetTypeName))
            {
                // Same concrete type: identity cast, the pointer is already what was asked for.
                if (srcStructName == targetTypeName) return interfaceValue;

                // Different concrete type. There is no inheritance between concrete classes and
                // no runtime type info on the object, so this can never be a checked downcast.
                LogErrorContext(ctx, std::format(
                    "cannot cast '{}' to unrelated type '{}'; 'as' performs a runtime-checked downcast only from an interface value",
                    SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = srcStructName }), targetTypeSpelling));
                return nullptr;
            }

            LogErrorContext(ctx, std::format("'{}' is not a known struct or interface type for 'as' cast", targetTypeSpelling));
            return nullptr;
        }

        // Only InterfaceValue reaches here, so the source carries the vtable + typedesc header
        // the checks below load from. Interface target: runtime-checked interface-to-interface.
        if (compiler->HasInterface(targetTypeName))
        {
            auto fatTy = compiler->GetFatPtrType();

            // Extract dataPtr from source fat pointer
            llvm::Value* dataPtr;
            if (interfaceValue->getType()->isStructTy())
                dataPtr = compiler->builder->CreateExtractValue(interfaceValue, {1u});
            else
            {
                auto dp = compiler->builder->CreateStructGEP(fatTy, interfaceValue, 1);
                dataPtr = compiler->builder->CreateLoad(ptrTy, dp);
            }

            // Load type descriptor from source
            llvm::Value* loadedDesc = LoadTypeDescFromInterface(interfaceValue, ctx);

            // Alloca for result: defaults to null fat ptr (aggregate zero)
            auto* resultAlloca = compiler->CreateAlloca(fatTy);
            compiler->builder->CreateStore(llvm::ConstantAggregateZero::get(fatTy), resultAlloca);

            auto* afterBlock = compiler->CreateBasicBlock("as_iface_after");

            // For each class that implements the target interface,
            // check if the concrete type matches and build the appropriate fat pointer
            for (auto& [sName, sd] : compiler->dataStructures)
            {
                if (!compiler->StructImplementsInterface(sName, targetTypeName)) continue;
                if (!sd.typeDescriptor) continue;

                auto* matchBlock = compiler->CreateBasicBlock("as_iface_match");
                auto* nextBlock = compiler->CreateBasicBlock("as_iface_next");

                auto* cmp = compiler->builder->CreateICmpEQ(loadedDesc, sd.typeDescriptor);
                compiler->builder->CreateCondBr(cmp, matchBlock, nextBlock);

                compiler->SwitchToBlock(matchBlock);
                auto* vtable = compiler->GetOrCreateVTable(sName, targetTypeName);
                auto fatVal = compiler->BuildInterfaceFatValue(vtable, dataPtr);
                compiler->builder->CreateStore(fatVal, resultAlloca);
                compiler->builder->CreateBr(afterBlock);

                compiler->SwitchToBlock(nextBlock);
            }
            // Also check programs that implement the target interface
            for (auto& [pName, pd] : compiler->programTable)
            {
                if (!compiler->StructImplementsInterface(pName, targetTypeName)) continue;
                if (!pd.typeDescriptor) continue;

                auto* matchBlock = compiler->CreateBasicBlock("as_iface_match");
                auto* nextBlock = compiler->CreateBasicBlock("as_iface_next");

                auto* cmp = compiler->builder->CreateICmpEQ(loadedDesc, pd.typeDescriptor);
                compiler->builder->CreateCondBr(cmp, matchBlock, nextBlock);

                compiler->SwitchToBlock(matchBlock);
                auto* vtable = compiler->GetOrCreateVTable(pName, targetTypeName);
                auto fatVal = compiler->BuildInterfaceFatValue(vtable, dataPtr);
                compiler->builder->CreateStore(fatVal, resultAlloca);
                compiler->builder->CreateBr(afterBlock);

                compiler->SwitchToBlock(nextBlock);
            }

            // No match: fall through (result stays null fat ptr)
            compiler->builder->CreateBr(afterBlock);
            compiler->SwitchToBlock(afterBlock);

            // Return the fat pointer value (aggregate)
            return compiler->builder->CreateLoad(fatTy, resultAlloca);
        }

        // Concrete target from an interface source: the runtime-checked downcast.
        auto targetIt = compiler->dataStructures.find(targetTypeName);
        if (targetIt == compiler->dataStructures.end())
        {
            LogErrorContext(ctx, std::format("'{}' is not a known struct or interface type for 'as' cast", targetTypeSpelling));
            return nullptr;
        }

        auto* typeDesc = targetIt->second.typeDescriptor;
        if (!typeDesc)
        {
            LogErrorContext(ctx, std::format("'{}' has no type descriptor", targetTypeSpelling));
            return nullptr;
        }

        // Extract data pointer (field 1)
        llvm::Value* dataPtr;
        if (interfaceValue->getType()->isStructTy())
            dataPtr = compiler->builder->CreateExtractValue(interfaceValue, {1u});
        else
        {
            auto fatTy = compiler->GetFatPtrType();
            auto dataPtrField = compiler->builder->CreateStructGEP(fatTy, interfaceValue, 1);
            dataPtr = compiler->builder->CreateLoad(ptrTy, dataPtrField);
        }

        auto loadedDesc = LoadTypeDescFromInterface(interfaceValue, ctx);
        auto typeMatches = compiler->builder->CreateICmpEQ(loadedDesc, typeDesc);

        // In opaque pointer mode, dataPtr is already the right pointer type - no bitcast needed
        auto nullPtr = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(dataPtr->getType()));
        // Reclaiming the concrete pointer out of a local that ADOPTED its box retires that
        // adoption, so an explicit `delete` on the result stays the single release path.
        if (srcBinding != nullptr && srcBinding->OwnsInterfaceBox && !srcBinding->CallerName.empty()
            && srcBinding->TypeAndValue.IsFatInterfaceValue() && !srcBinding->TypeAndValue.Pointer)
        {
            compiler->SetVariableOwning(srcBinding->CallerName, false);
            compiler->SetVariableOwnsInterfaceBox(srcBinding->CallerName, false);
        }
        return compiler->builder->CreateSelect(typeMatches, dataPtr, nullPtr);
    }

LLVMBackend::TypedValue MainListener::ParseRelationalExpression(CFlatParser::RelationalExpressionContext* ctx, ResultUse use) {
        CallPostfixScope operatorPostfix(this);
        DeclExpectedTypeGate declExpectedGate(&declExpectedType, ctx->children.size() == 1);
        auto nextCtxs = ctx->shiftExpression();
        if (nextCtxs.size() == 1)
        {
            return ParseShiftExpression(nextCtxs[0], use);
        }
        else if (nextCtxs.size() == 2)
        {
            // See ParseEqualityExpression: a string operand returned from a call (e.g.
            // `s < other.toString()`) is an owned temp the relational operator only borrows.
            // Reset lastCallReturnsOwned before each operand and register each call-result
            // operand for end-of-statement cleanup (see RegisterBorrowedStringOperandTemp).
            // Owning-POINTER operands are registered here too - see ParseEqualityExpression.
            Compiler(ctx)->lastCallReturnsOwned = false;
            auto lv = ParseShiftExpression(nextCtxs[0], ResultUse::Value);
            NormalizeCxxReferenceOperand(ctx, lv);
            RegisterBorrowedStringOperandTemp(Compiler(ctx), lv.value);
            Compiler(ctx)->RegisterOwnedPtrTemp(lv.value);
            Compiler(ctx)->lastCallReturnsOwned = false;
            auto rv = ParseShiftExpression(nextCtxs[1], ResultUse::Value);
            NormalizeCxxReferenceOperand(ctx, rv);
            RegisterBorrowedStringOperandTemp(Compiler(ctx), rv.value);
            Compiler(ctx)->RegisterOwnedPtrTemp(rv.value);
            std::string op = ctx->children[1]->getText();

            Compiler(ctx)->RejectImplicitIntegerPointeePointerConversion(
                InferTernaryArmType(lv), InferTernaryArmType(rv));

            auto* overload = TryBinaryOperatorOverload(lv, op, rv, ctx, lv.elemType,
                                                       rv.pointerDepth, rv.elemPointer,
                                                       lv.receiverStorage, rv.receiverStorage,
                                                       true, true, lv.isRvalue, rv.isRvalue,
                                                       lv.sourceTypeName, rv.sourceTypeName,
                                                       lv.pointerDepth, lv.elemPointer,
                                                       {}, {}, false, {}, {}, false, {}, {},
                                                       rv.cxxPointeeConst, lv.cxxArithIdentity, rv.cxxArithIdentity, rv.isExplicitMove, lv.isExplicitMove,
                                                       lv.hasSourceType && lv.sourceType.IsCxxNullptrT,
                                                       rv.hasSourceType && rv.sourceType.IsCxxNullptrT,
                                                       false, lv.cxxPointeeConst,
                                                       lv.cxxObjectCv, rv.cxxObjectCv);
            if (overload)
            {
                LLVMBackend::NamedVariable resultNV;
                resultNV.Primary = overload;
                resultNV.TypeAndValue = Compiler(ctx)->lastCallReturnType;
                DiagnoseVoidResultConsumed(ctx, resultNV, use, std::format("'operator{}'", op));
            }
            llvm::Value* result = overload ? overload
                                           : Compiler(ctx)->CreateOperation(op, lv, rv, lv.isUnsigned, rv.isUnsigned);
            LLVMBackend::TypedValue typedResult{ result, false };
            if (overload && Compiler(ctx)->lastCallReturnType.IsAlias
                && Compiler(ctx)->IsCxxRecord(Compiler(ctx)->lastCallReturnType.TypeName))
            {
                CarryCxxOperatorResult(ctx, overload, false, typedResult.value,
                    typedResult.receiverStorage, typedResult.isRvalue,
                    typedResult.cxxRefValueType, typedResult.sourceTypeName);
                typedResult.isAlias = true;
                typedResult.storage = typedResult.receiverStorage;
            }
            return typedResult;
        }

        LogErrorContext(ctx, "Relational expression has unexpected operand count.");
        return {};
    }

std::string MainListener::TryGetSimpleIdentifier(antlr4::ParserRuleContext* ctx) {
        if (!ctx) return "";
        auto& children = ctx->children;
        if (children.size() == 1)
        {
            if (auto* term = dynamic_cast<antlr4::tree::TerminalNode*>(children[0]))
            {
                if (term->getSymbol()->getType() == CFlatLexer::Identifier)
                    return term->getText();
            }
            if (auto* child = dynamic_cast<antlr4::ParserRuleContext*>(children[0]))
                return TryGetSimpleIdentifier(child);
        }
        return "";
    }

llvm::Function* MainListener::FindStreamMethodFn(LLVMBackend* compiler, const std::string& methodName) {
        auto it = compiler->functionTable.find(methodName);
        if (it == compiler->functionTable.end()) return nullptr;
        for (const auto& sym : it->second)
        {
            if (!sym.Parameters.empty() && sym.Parameters[0].TypeName == "stream")
                return sym.Function;
        }
        return nullptr;
    }

void MainListener::EmitProgramToStreamWire(const std::string& progName,
        llvm::Value* progStorage, llvm::Value* streamStorage,
        antlr4::ParserRuleContext* ctx) {
        auto* compiler = Compiler(ctx);
        auto* i8PtrTy  = cflat_llvm::PointerTo(compiler->builder->getInt8Ty());

        auto* writeBytesFn = FindStreamMethodFn(compiler, "write_bytes");
        if (!writeBytesFn)
        {
            LogErrorContext(ctx, "stream::write_bytes not found - import \"stream.cb\" before using >>.");
            return;
        }

        // Create/reuse __stream_write_shim(char* data, i32 len, i8* env) - casts env to stream*, calls write_bytes.
        // Env is the trailing param (closure ABI is env-last). The hook is non-owning: printf passes a
        // thread-local buffer; the shim copies it via write_bytes.
        llvm::Function* shim = compiler->module->getFunction("__stream_write_shim");
        if (!shim)
        {
            auto* charPtrTy = i8PtrTy;  // char* and i8* are the same in LLVM
            auto* i32Ty     = llvm::Type::getInt32Ty(*compiler->context);
            auto* shimTy    = llvm::FunctionType::get(
                compiler->builder->getVoidTy(), {charPtrTy, i32Ty, i8PtrTy}, false);
            shim = llvm::Function::Create(shimTy, llvm::Function::InternalLinkage,
                "__stream_write_shim", *compiler->module);
            auto* bb = llvm::BasicBlock::Create(*compiler->context, "entry", shim);
            llvm::IRBuilder<> b(bb);
            auto* streamTy = compiler->GetDataStructure("stream").StructType;
            auto* selfPtr  = b.CreateBitCast(shim->getArg(2), cflat_llvm::PointerTo(streamTy), "stream_self");
            b.CreateCall(writeBytesFn->getFunctionType(), writeBytesFn,
                {selfPtr, shim->getArg(0), shim->getArg(1)});
            b.CreateRetVoid();
        }

        // Build fat closure {shim_i8*, stream_i8*} and store into prog.onStdout.
        auto* shimI8 = compiler->builder->CreateBitCast(shim, i8PtrTy, "write_shim_i8");
        auto* envI8  = compiler->builder->CreateBitCast(streamStorage, i8PtrTy, "stream_env_i8");
        auto* fatTy  = compiler->GetClosureFatPtrType();
        llvm::Value* fat = llvm::UndefValue::get(fatTy);
        fat = compiler->builder->CreateInsertValue(fat, shimI8, {0u});
        fat = compiler->builder->CreateInsertValue(fat, envI8,  {1u});

        auto& pd  = compiler->programTable[progName];
        auto* gep = compiler->builder->CreateStructGEP(
            pd.StructType, progStorage, pd.OnStdoutFieldIndex, "on_stdout_gep");
        compiler->builder->CreateStore(fat, gep);

        // Also store the stream pointer into _out so programs can call _out.write_bytes() directly.
        if (pd.OutFieldIndex != (unsigned)-1)
        {
            auto* outGep = compiler->builder->CreateStructGEP(
                pd.StructType, progStorage, pd.OutFieldIndex, "out_gep");
            compiler->builder->CreateStore(streamStorage, outGep);
        }
    }

void MainListener::EmitStreamToProgramWire(llvm::Value* streamStorage,
        const std::string& progName, llvm::Value* progStorage,
        antlr4::ParserRuleContext* ctx) {
        auto* compiler = Compiler(ctx);
        auto* i8PtrTy  = cflat_llvm::PointerTo(compiler->builder->getInt8Ty());

        auto* readFn = FindStreamMethodFn(compiler, "read");
        if (!readFn)
        {
            LogErrorContext(ctx, "stream::read not found - import \"stream.cb\" before using >>.");
            return;
        }

        // Create/reuse __stream_read_shim(i8* env) -> char* - casts env to stream*, calls read.
        llvm::Function* shim = compiler->module->getFunction("__stream_read_shim");
        if (!shim)
        {
            auto* charPtrTy = i8PtrTy;
            auto* shimTy    = llvm::FunctionType::get(charPtrTy, {i8PtrTy}, false);
            shim = llvm::Function::Create(shimTy, llvm::Function::InternalLinkage,
                "__stream_read_shim", *compiler->module);
            auto* bb = llvm::BasicBlock::Create(*compiler->context, "entry", shim);
            llvm::IRBuilder<> b(bb);
            auto* streamTy = compiler->GetDataStructure("stream").StructType;
            auto* selfPtr  = b.CreateBitCast(shim->getArg(0), cflat_llvm::PointerTo(streamTy), "stream_self");
            auto* result   = b.CreateCall(readFn->getFunctionType(), readFn, {selfPtr}, "line");
            b.CreateRet(result);
        }

        auto* shimI8 = compiler->builder->CreateBitCast(shim, i8PtrTy, "read_shim_i8");
        auto* envI8  = compiler->builder->CreateBitCast(streamStorage, i8PtrTy, "stream_env_i8");
        auto* fatTy  = compiler->GetClosureFatPtrType();
        llvm::Value* fat = llvm::UndefValue::get(fatTy);
        fat = compiler->builder->CreateInsertValue(fat, shimI8, {0u});
        fat = compiler->builder->CreateInsertValue(fat, envI8,  {1u});

        auto& pd  = compiler->programTable[progName];
        auto* gep = compiler->builder->CreateStructGEP(
            pd.StructType, progStorage, pd.OnStdinFieldIndex, "on_stdin_gep");
        compiler->builder->CreateStore(fat, gep);

        // Wire return_buffer: when the consumer is done with a buffer, return it to the pool.
        auto* returnFn = FindStreamMethodFn(compiler, "return_buffer");
        if (returnFn)
        {
            // Create/reuse __stream_return_buffer_shim(char* buf, i8* env) - calls stream.return_buffer.
            // Env is the trailing param (closure ABI is env-last).
            llvm::Function* returnShim = compiler->module->getFunction("__stream_return_buffer_shim");
            if (!returnShim)
            {
                auto* charPtrTy = i8PtrTy;
                auto* shimTy    = llvm::FunctionType::get(
                    compiler->builder->getVoidTy(), {charPtrTy, i8PtrTy}, false);
                returnShim = llvm::Function::Create(shimTy, llvm::Function::InternalLinkage,
                    "__stream_return_buffer_shim", *compiler->module);
                auto* bb = llvm::BasicBlock::Create(*compiler->context, "entry", returnShim);
                llvm::IRBuilder<> b(bb);
                auto* streamTy = compiler->GetDataStructure("stream").StructType;
                auto* selfPtr  = b.CreateBitCast(returnShim->getArg(1), cflat_llvm::PointerTo(streamTy), "stream_self");
                b.CreateCall(returnFn->getFunctionType(), returnFn, {selfPtr, returnShim->getArg(0)});
                b.CreateRetVoid();
            }

            auto* returnShimI8 = compiler->builder->CreateBitCast(returnShim, i8PtrTy, "return_shim_i8");
            auto* returnEnvI8  = compiler->builder->CreateBitCast(streamStorage, i8PtrTy, "stream_env_i8_ret");
            llvm::Value* returnFat = llvm::UndefValue::get(fatTy);
            returnFat = compiler->builder->CreateInsertValue(returnFat, returnShimI8, {0u});
            returnFat = compiler->builder->CreateInsertValue(returnFat, returnEnvI8,  {1u});

            auto* retGep = compiler->builder->CreateStructGEP(
                pd.StructType, progStorage, pd.OnStdinReturnFieldIndex, "on_stdin_return_gep");
            compiler->builder->CreateStore(returnFat, retGep);
        }

        // Also store the stream pointer into _in so programs can call _in.read_buf() directly.
        if (pd.InStreamFieldIndex != (unsigned)-1)
        {
            auto* inGep = compiler->builder->CreateStructGEP(
                pd.StructType, progStorage, pd.InStreamFieldIndex, "in_gep");
            compiler->builder->CreateStore(streamStorage, inGep);
        }
    }

llvm::Value* MainListener::EmitArenaChannelShellAlloc(LLVMBackend* compiler) {
        const std::string channelType = ArenaChannelTypeName(*compiler);
        auto dsIt = compiler->dataStructures.find(channelType);
        if (dsIt == compiler->dataStructures.end()) return nullptr;
        auto* arenaTy    = dsIt->second.StructType;
        auto* arenaPtrTy = cflat_llvm::PointerTo(arenaTy);
        auto* ctorFn     = compiler->GetFunction(channelType);
        auto* mallocFn   = compiler->GetFunction("malloc");
        if (!ctorFn || !mallocFn) return llvm::Constant::getNullValue(arenaPtrTy);

        auto* arenaSize = compiler->GetTypeSizeBytes(arenaTy);
        auto* arenaRaw  = compiler->builder->CreateCall(
            mallocFn->getFunctionType(), mallocFn, {arenaSize}, "arena_shell_raw");
        auto* arenaPtr  = compiler->builder->CreateBitCast(arenaRaw, arenaPtrTy, "arena_shell_ptr");
        auto* arenaZero = compiler->builder->CreateCall(
            ctorFn->getFunctionType(), ctorFn, {}, "arena_shell_zero");
        compiler->builder->CreateStore(arenaZero, arenaPtr);
        return arenaPtr;
    }

void MainListener::EmitProgramToProgramArenaWire(const std::string& producerName, llvm::Value* producerStorage,
        const std::string& consumerName, llvm::Value* consumerStorage,
        antlr4::ParserRuleContext* ctx) {
        auto* compiler = Compiler(ctx);
        auto& producerPd = compiler->programTable[producerName];
        auto& consumerPd = compiler->programTable[consumerName];

        if (consumerPd.InboxArenaFieldIndex == (unsigned)-1 || producerPd.OutboxFieldIndex == (unsigned)-1)
        {
            LogErrorContext(ctx,
                "program-to-program '>>' requires arena_channel (import \"arena_channel.cb\").");
            return;
        }

        const std::string channelType = ArenaChannelTypeName(*compiler);
        auto dsIt = compiler->dataStructures.find(channelType);
        if (dsIt == compiler->dataStructures.end())
        {
            LogErrorContext(ctx, "arena_channel type not found for program '>>'.");
            return;
        }
        auto* arenaTy    = dsIt->second.StructType;
        auto* arenaPtrTy = cflat_llvm::PointerTo(arenaTy);

        // Select the no-arg init() overload (idempotent; init(int, i64) also exists).
        llvm::Function* initFn = nullptr;
        if (auto it = compiler->functionTable.find("init"); it != compiler->functionTable.end())
            for (const auto& sym : it->second)
                if (sym.Parameters.size() == 1 && sym.Parameters[0].TypeName == channelType)
                    { initFn = sym.Function; break; }

        if (!initFn)
        {
            LogErrorContext(ctx, "arena_channel init not found for program '>>'.");
            return;
        }

        // The consumer's inbox is a non-null shell allocated in its ctor. init() upgrades it to a
        // live ring (no-op if a previous `>>` already wired it - the fan-in case).
        auto* inboxGep = compiler->builder->CreateStructGEP(
            consumerPd.StructType, consumerStorage, consumerPd.InboxArenaFieldIndex, "inbox_gep");
        auto* inboxPtr = compiler->builder->CreateLoad(arenaPtrTy, inboxGep, "inbox_ptr");
        compiler->builder->CreateCall(initFn->getFunctionType(), initFn, {inboxPtr});

        // Rebind producer.outbox (its own self-loopback shell) to the consumer's live inbox.
        auto* outboxGep = compiler->builder->CreateStructGEP(
            producerPd.StructType, producerStorage, producerPd.OutboxFieldIndex, "outbox_gep");
        compiler->builder->CreateStore(inboxPtr, outboxGep);
    }

void MainListener::EmitProgramToProgramStreamWire(const std::string& producerName, llvm::Value* producerStorage,
        const std::string& consumerName, llvm::Value* consumerStorage,
        antlr4::ParserRuleContext* ctx) {
        auto* compiler = Compiler(ctx);
        auto streamDsIt = compiler->dataStructures.find("stream");
        if (streamDsIt == compiler->dataStructures.end()) return;  // stream.cb not imported - arena-only
        auto& streamDs = streamDsIt->second;

        auto* streamTy = streamDs.StructType;
        auto* ctorFn   = compiler->GetFunction("stream");
        auto* initFn   = FindStreamMethodFn(compiler, "init");
        if (!ctorFn || !initFn)
        {
            LogErrorContext(ctx, "stream ctor/init not found for program>>program piping - import \"stream.cb\".");
            return;
        }

        // Hidden stream local: alloca at entry, default-construct, init().
        auto* streamStorage = compiler->AllocaAtEntry(streamTy, nullptr, "pipe_stream");
        auto* zeroVal = compiler->builder->CreateCall(ctorFn->getFunctionType(), ctorFn, {}, "pipe_stream_zero");
        compiler->builder->CreateStore(zeroVal, streamStorage);
        compiler->builder->CreateCall(initFn->getFunctionType(), initFn, {streamStorage});

        // Mark _autoClose = true so the producer trampoline closes it after main() returns.
        unsigned autoCloseIdx = (unsigned)-1;
        const auto& fields = streamDs.StructFields;
        for (unsigned i = 0; i < fields.size(); ++i)
            if (fields[i].VariableName == "_autoClose") { autoCloseIdx = i; break; }
        if (autoCloseIdx != (unsigned)-1)
        {
            auto* acGep    = compiler->builder->CreateStructGEP(streamTy, streamStorage, autoCloseIdx, "autoclose_gep");
            auto* acElemTy = streamTy->getStructElementType(autoCloseIdx);
            compiler->builder->CreateStore(llvm::ConstantInt::get(acElemTy, 1), acGep);
        }

        // Register the hidden stream for scope-exit destruction (frees both buffers + the CVs).
        if (!compiler->stackNamedVariable.empty())
        {
            LLVMBackend::NamedVariable nv;
            nv.TypeAndValue.TypeName = "stream";
            nv.TypeAndValue.Pointer  = false;
            nv.Storage               = streamStorage;
            compiler->SetStackVariable(
                "__pipe_stream_" + std::to_string(compiler->pipeStreamCounter++), nv);
        }

        // Reuse the explicit-form wiring for both directions.
        EmitProgramToStreamWire(producerName, producerStorage, streamStorage, ctx);
        EmitStreamToProgramWire(streamStorage, consumerName, consumerStorage, ctx);
    }

bool MainListener::HasOperatorOverloadForFirstParam(const std::string& opName, const std::string& typeName) {
        auto* compiler = Compiler();
        if (compiler->IsCxxRecord(typeName)) compiler->EnsureCxxMemberProjected(typeName, opName);
        auto it = compiler->functionTable.find(opName);
        if (it == compiler->functionTable.end()) return false;
        for (const auto& sym : it->second)
            if (!sym.Parameters.empty() && sym.Parameters[0].TypeName == typeName)
                return true;
        return false;
    }


/*
 * Syntactic category of a call argument for C++ reference binding (NamedVariable::CxxLvalueKind):
 * 1 = lvalue designator (name, field, element, `*p`, parenthesised lvalue, `?:` of two lvalues),
 * 2 = call (an lvalue only through a `T&` result), 3 = assignment (plain or compound; refused
 * by ruling 2026-09-27), 0 = anything else: unary/binary arithmetic, casts, `x++`, literals,
 * `move`, `?.`.
 */
int8_t MainListener::CxxArgumentLvalueKind(antlr4::tree::ParseTree* tree)
{
        while (tree != nullptr)
        {
            if (auto* a = dynamic_cast<CFlatParser::AssignmentExpressionContext*>(tree))
            {
                if (a->assignmentOperator() != nullptr) return 3;
                if (a->conditionalExpression() == nullptr) return 0;
                tree = a->conditionalExpression();
                continue;
            }
            if (auto* c = dynamic_cast<CFlatParser::ConditionalExpressionContext*>(tree))
            {
                if (c->children.size() == 1) { tree = c->logicalOrExpression(); continue; }
                if (c->expression() == nullptr || c->conditionalExpression() == nullptr) return 0;
                return CxxArgumentLvalueKind(c->expression()) == 1
                    && CxxArgumentLvalueKind(c->conditionalExpression()) == 1 ? 1 : 0;
            }
            if (auto* e = dynamic_cast<CFlatParser::ExpressionContext*>(tree))
            {
                tree = e->assignmentExpression();
                continue;
            }
            if (auto* cast = dynamic_cast<CFlatParser::CastExpressionContext*>(tree))
            {
                if (cast->unaryExpression() == nullptr) return 0;
                tree = cast->unaryExpression();
                continue;
            }
            if (auto* u = dynamic_cast<CFlatParser::UnaryExpressionContext*>(tree))
            {
                if (u->postfixExpression() != nullptr && u->children.size() == 1)
                {
                    tree = u->postfixExpression();
                    continue;
                }
                return u->unaryOperator() != nullptr && u->children.size() == 2
                    && u->unaryOperator()->getText() == "*" ? 1 : 0;
            }
            if (auto* p = dynamic_cast<CFlatParser::PostfixExpressionContext*>(tree))
            {
                if (p->children.size() == 1) { tree = p->primaryExpression(); continue; }
                const std::string last = p->children.back()->getText();
                if (last == "++" || last == "--") return 0;
                if (last == "]") return 1;
                if (last == ")")
                    return p->children.size() >= 3
                        && p->children[p->children.size() - 3]->getText() == "~" ? 0 : 2;
                // A member access: the nearest accessor token decides ('?.' yields a value).
                for (auto it = p->children.rbegin(); it != p->children.rend(); ++it)
                    if (dynamic_cast<antlr4::tree::TerminalNode*>(*it) != nullptr)
                        return (*it)->getText() == "?." ? 0 : 1;
                return 0;
            }
            if (auto* pr = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(tree))
            {
                if (pr->genericIdentifier() != nullptr && pr->children.size() == 1) return 1;
                if (pr->expression() != nullptr && pr->children.size() == 3
                    && pr->children.front()->getText() == "(")
                {
                    tree = pr->expression();
                    continue;
                }
                return 0;
            }
            // logicalOr ... multiplicative: an lvalue only as a lone operand.
            auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(tree);
            if (rule == nullptr || rule->children.size() != 1) return 0;
            tree = rule->children.front();
        }
        return 0;
}

bool MainListener::ReportCxxConstPointerOperatorFailure(
        LLVMBackend* compiler, antlr4::ParserRuleContext* ctx,
        const std::string& sourceName,
        const std::vector<LLVMBackend::NamedVariable>& arguments,
        const std::string& op, const std::string& lhsCxxName,
        const std::string& rhsCxxName,
        bool lhsPointeeConst, bool rhsPointeeConst)
{
        // Same request shape as the failed one (an infix operator, no adlCall wrapper).
        for (size_t i = 0; i < arguments.size(); ++i)
        {
            const auto& source = arguments[i].TypeAndValue;
            const bool pointerArgument = source.Pointer
                || (arguments[i].Primary != nullptr
                    && arguments[i].Primary->getType()->isPointerTy());
            const bool pointeeConst = source.PointeeConstBit()
                || (i == 0 ? lhsPointeeConst : rhsPointeeConst);
            if (!pointerArgument || !pointeeConst) continue;
            std::vector<LLVMBackend::NamedVariable> retry = arguments;
            retry[i].TypeAndValue.Pointer = true;
            if (retry[i].TypeAndValue.PointerDepth == 0)
                retry[i].TypeAndValue.PointerDepth = 1;
            retry[i].TypeAndValue.SetPointeeConstBit(false);
            std::string retryName, retryError;
            if (!compiler->RequestCxxFreeFunction(sourceName, {}, retry, retryName,
                                                  retryError, op, lhsCxxName,
                                                  rhsCxxName)
                || retryName.empty())
                continue;
            const std::string argumentName = source.VariableName.empty()
                ? "<argument>" : source.VariableName;
            const size_t argumentIndex = i + (lhsCxxName.empty() ? 0 : 1) + 1;
            LogErrorContext(ctx, compiler->CxxConstPointerArgumentError(
                argumentIndex, argumentName, {}));
            return true;
        }
        return false;
}

/*
 * `a << b` / `a >> b` with a C++ class on the left is a C++ expression, so C++ resolves it: a
 * generated `decltype(auto) w(A& p0, B p1) { return p0 << p1; }` sees the members, the free
 * operator templates (`operator<<(basic_ostream<char, T>&, const char*)`), ADL candidates and
 * derived-to-base conversions. A scalar lvalue read by `>>` is passed as `T &` so the extraction
 * lands in the CFlat variable. nullptr -> no wrapper; the caller falls back to CFlat's resolution.
 */
llvm::Value* MainListener::TryCxxInfixShift(CFlatParser::ShiftExpressionContext* ctx,
                                            const std::string& op,
                                            const ShiftOperand& lhs, const std::string& lhsClass,
                                            llvm::Value* lhsStorage,
                                            const ShiftOperand& rhs, const std::string& rhsClass,
                                            llvm::Value* rhsStorage)
{
        auto* compiler = Compiler(ctx);
        if ((lhs.value.value == nullptr && lhs.cxxName.empty())
            || (rhs.value.value == nullptr && rhs.cxxName.empty()))
            return nullptr;
        const std::string& owner = !lhs.cxxName.empty() ? lhs.cxxName : lhsClass;
        const std::string base = owner.substr(0, owner.find('$'));
        const size_t dot = base.rfind('.');
        const std::string sourceName = (dot == std::string::npos ? std::string()
                                        : base.substr(0, dot) + ".") + "operator" + op;
        auto operand = [&](const ShiftOperand& o, const std::string& cls, llvm::Value* storage) {
            LLVMBackend::NamedVariable arg;
            arg.Primary = o.value.value;
            arg.BaseType = o.value.value->getType();
            const bool isClass = !cls.empty() && compiler->IsCxxRecord(cls);
            auto* literal = llvm::dyn_cast<llvm::ConstantInt>(o.value.value);
            if (isClass) arg.TypeAndValue.TypeName = cls;
            else if (literal != nullptr && o.named.Storage == nullptr && !o.literalType.empty())
            {
                // A CFlat literal is narrowed to its smallest type; C++ reads `42` as int.
                arg.TypeAndValue.TypeName = o.literalType;
                LLVMBackend::TypeAndValue declared;
                declared.TypeName = o.literalType;
                llvm::Type* literalTy = compiler->GetType(declared, nullptr, false);
                if (literalTy != nullptr && literalTy->isIntegerTy())
                {
                    arg.Primary = llvm::ConstantInt::get(literalTy, literal->getSExtValue(), true);
                    arg.BaseType = literalTy;
                }
            }
            else if (o.named.Storage != nullptr && !o.named.TypeAndValue.TypeName.empty())
                arg.TypeAndValue = o.named.TypeAndValue;
            else if (arg.BaseType->isPointerTy() && !o.value.sourceTypeName.empty())
            {
                // An unnamed pointer (`s.c_str()`, `(void*)p`): the declared pointee is the
                // source type name; the pointer level must survive or `char*` reads as `char`.
                arg.TypeAndValue.TypeName = o.value.sourceTypeName;
                arg.TypeAndValue.Pointer = true;
                arg.TypeAndValue.ElemPointer = o.value.elemPointer || o.value.pointerDepth > 1;
                arg.TypeAndValue.SetPointeeConstBit(o.value.cxxPointeeConst);
            }
            else arg.InferSourceTypeName = o.value.sourceTypeName;
            if (!isClass && o.named.Storage == nullptr)
            {
                arg.CxxArithIdentity = o.value.cxxArithIdentity;
                if (arg.CxxArithIdentity.empty())
                    arg.CxxArithIdentity = CxxLiteralOperandIdentity(o.expr);
            }
            arg.TypeAndValue.VariableName.clear();
            arg.Storage = o.value.isRvalue && isClass ? nullptr : storage;
            if (!isClass && o.named.Storage == nullptr) arg.Storage = nullptr;
            arg.IsRvalue = isClass && arg.Storage == nullptr;
            arg.CxxLvalueKind = o.expr != nullptr ? CxxArgumentLvalueKind(o.expr) : -1;
            // A scalar lvalue designator that is no simple name (`h.f`, `a[1]`, `*p`): its
            // address is the slot this expression loaded from.
            if (!isClass && arg.CxxLvalueKind == 1 && arg.Storage == nullptr)
                if (auto* load = llvm::dyn_cast<llvm::LoadInst>(arg.Primary))
                    arg.Storage = load->getPointerOperand();
            if (arg.IsRvalue)
            {
                auto* temp = compiler->CreateAlloca(arg.BaseType);
                compiler->CreateAssignment(arg.Primary, temp);
                arg.Storage = temp;
            }
            return arg;
        };
        std::vector<LLVMBackend::NamedVariable> args;
        if (lhs.cxxName.empty()) args.push_back(operand(lhs, lhsClass, lhsStorage));
        if (rhs.cxxName.empty()) args.push_back(operand(rhs, rhsClass, rhsStorage));
        std::string registeredName;
        std::string error;
        if (!compiler->RequestCxxFreeFunction(sourceName, {}, args, registeredName, error, op,
                                              lhs.cxxName, rhs.cxxName)
            || registeredName.empty())
        {
            if (!lhs.cxxName.empty() || !rhs.cxxName.empty())
            {
                auto shown = [&](const ShiftOperand& o, const std::string& cls) {
                    if (!o.cxxName.empty()) return o.cxxName;
                    if (!cls.empty())
                        return SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = cls });
                    return std::string("value");
                };
                LogErrorContext(ctx, std::format("C++ expression '{} {} {}' could not be bound ({})",
                                                 shown(lhs, lhsClass), op, shown(rhs, rhsClass),
                                                 error));
                return nullptr;
            }
            if (ReportCxxConstPointerOperatorFailure(
                    compiler, ctx, sourceName, args, op, lhs.cxxName, rhs.cxxName))
                return nullptr;
            /*
             * The wrapper hands C++ a scalar temporary as an rvalue. When the SAME operands bind
             * once that temporary is an lvalue, the refusal was a non-const `T&` parameter.
             */
            for (size_t i = 0; i < args.size(); ++i)
            {
                const auto& a = args[i];
                if (a.TypeAndValue.Pointer || compiler->IsCxxRecord(a.TypeAndValue.TypeName)
                    || a.BaseType == nullptr || !compiler->IsCxxRvalueReferenceArgument(a))
                    continue;
                std::vector<LLVMBackend::NamedVariable> retry = args;
                auto* slot = compiler->CreateAlloca(a.BaseType);
                compiler->CreateAssignment(a.Primary, slot);
                retry[i].Storage = slot;
                retry[i].IsRvalue = false;
                retry[i].Primary = compiler->builder->CreateLoad(a.BaseType, slot);
                retry[i].CxxLvalueKind = 1;
                std::string retryName, retryError;
                if (!compiler->RequestCxxFreeFunction(sourceName, {}, retry, retryName, retryError,
                                                      op, lhs.cxxName, rhs.cxxName)
                    || retryName.empty())
                    continue;
                LogErrorContext(ctx, std::format(
                    "parameter '{}' of '{}' is a non-const lvalue reference and cannot bind an "
                    "rvalue; pass an lvalue", i == 1 ? "right operand" : "left operand",
                    sourceName));
                return nullptr;
            }
            return nullptr;
        }
        FlushCallPostfix();
        return compiler->CreateOverloadedFunctionCall(registeredName, args, true);
}

MainListener::ShiftPairResult MainListener::ParseShiftPair(
    const ShiftOperand& lhs, const ShiftOperand& rhs, const std::string& op,
    CFlatParser::ShiftExpressionContext* ctx, ResultUse use, bool moreOperands) {
        auto* compiler = Compiler(ctx);
        const auto& lv = lhs.value;
        const auto& rv = rhs.value;
        const auto& lhsNV = lhs.named;
        const auto& rhsNV = rhs.named;
        const std::string& lhsName = lhs.name;
        const std::string& rhsName = rhs.name;
        const std::string& lhsType = lhsNV.TypeAndValue.TypeName;
        const std::string& rhsType = rhsNV.TypeAndValue.TypeName;

        auto pipeResult = [&]() {
            ShiftPairResult result;
            result.value = rv;
            result.named = rhsNV;
            result.name = rhsName;
            return result;
        };

        if (op == ">>")
        {
            llvm::Value* lhsStorage = lhsNV.Storage != nullptr ? lhsNV.Storage : lv.receiverStorage;
            llvm::Value* rhsStorage = rhsNV.Storage != nullptr ? rhsNV.Storage : rv.receiverStorage;

            bool lhsIsProgram = !lhsType.empty() && compiler->programTable.count(lhsType) > 0;
            bool rhsIsProgram = !rhsType.empty() && compiler->programTable.count(rhsType) > 0;
            bool lhsIsStream  = lhsType == "stream";
            bool rhsIsStream  = rhsType == "stream";

            if (lhsIsProgram && rhsIsProgram && lhsStorage && rhsStorage)
            {
                EmitProgramToProgramStreamWire(lhsType, lhsStorage, rhsType, rhsStorage, ctx);

                auto& lpd = compiler->programTable[lhsType];
                auto& rpd = compiler->programTable[rhsType];
                auto* i32Ty = llvm::Type::getInt32Ty(*compiler->context);
                auto* lUseGEP = compiler->builder->CreateStructGEP(
                    lpd.StructType, lhsStorage, lpd.UseChannelFieldIndex, "l_usechannel_gep");
                auto* rUseGEP = compiler->builder->CreateStructGEP(
                    rpd.StructType, rhsStorage, rpd.UseChannelFieldIndex, "r_usechannel_gep");
                auto* lOn = compiler->builder->CreateICmpNE(
                    compiler->builder->CreateLoad(i32Ty, lUseGEP, "l_usechannel"),
                    llvm::ConstantInt::get(i32Ty, 0), "l_usechannel_on");
                auto* rOn = compiler->builder->CreateICmpNE(
                    compiler->builder->CreateLoad(i32Ty, rUseGEP, "r_usechannel"),
                    llvm::ConstantInt::get(i32Ty, 0), "r_usechannel_on");
                auto* bothOn = compiler->builder->CreateAnd(lOn, rOn, "usechannel_both");

                auto* curFn   = compiler->builder->GetInsertBlock()->getParent();
                auto* wireBB  = llvm::BasicBlock::Create(*compiler->context, "arena_wire", curFn);
                auto* afterBB = llvm::BasicBlock::Create(*compiler->context, "arena_after", curFn);
                compiler->builder->CreateCondBr(bothOn, wireBB, afterBB);

                compiler->builder->SetInsertPoint(wireBB);
                EmitProgramToProgramArenaWire(lhsType, lhsStorage, rhsType, rhsStorage, ctx);
                compiler->builder->CreateBr(afterBB);

                compiler->builder->SetInsertPoint(afterBB);
                return pipeResult();
            }
            if (lhsIsProgram && rhsIsStream && lhsStorage && rhsStorage)
            {
                EmitProgramToStreamWire(lhsType, lhsStorage, rhsStorage, ctx);
                return pipeResult();
            }
            if (lhsIsStream && rhsIsProgram && rhsStorage)
            {
                llvm::Value* streamPtr = lhsStorage;
                if (!streamPtr)
                {
                    auto* streamTy = compiler->GetDataStructure("stream").StructType;
                    streamPtr = compiler->AllocaAtEntry(streamTy, nullptr, "stream_spill");
                    compiler->builder->CreateStore(lv.value, streamPtr);
                }
                EmitStreamToProgramWire(streamPtr, rhsType, rhsStorage, ctx);
                return pipeResult();
            }
            if (lhs.accumulated && (lhsIsProgram || rhsIsProgram || lhsIsStream || rhsIsStream))
            {
                LogErrorContext(ctx, "chained '>>' piping is not supported; split the statement");
                return {};
            }
        }

        if (op == ">>" || op == "<<")
        {
            // A non-final operator result is a temporary, not the declaration's sret slot (as `+`).
            CxxFoldSretScope resultSret(compiler, moreOperands);
            // A chained operand (the reference a previous `<<` returned) has no named variable;
            // its class comes from the loaded value.
            auto className = [&](const std::string& declared, llvm::Value* value) {
                if (!declared.empty()) return declared;
                auto* st = value != nullptr ? llvm::dyn_cast<llvm::StructType>(value->getType())
                                            : nullptr;
                return st != nullptr && !st->isLiteral() && st->hasName()
                    ? st->getName().str() : std::string();
            };
            const std::string lhsClass = className(lhsType, lv.value);
            const std::string rhsClass = className(rhsType, rv.value);
            if (!lhs.cxxName.empty() || !rhs.cxxName.empty()
                || (!lhsClass.empty() && compiler->IsCxxRecord(lhsClass))
                || (!rhsClass.empty() && compiler->IsCxxRecord(rhsClass)))
            {
                llvm::Value* lhsStorage = lhsNV.Storage != nullptr ? lhsNV.Storage
                                                                  : lv.receiverStorage;
                llvm::Value* rhsStorage = rhsNV.Storage != nullptr ? rhsNV.Storage
                                                                  : rv.receiverStorage;
                llvm::Value* overload = nullptr;
                const bool namedOperand = !lhs.cxxName.empty() || !rhs.cxxName.empty();
                // A written `move` operand spells the infix wrapper's parameter as `T&&`, which hides a
                // `const T&` operator's borrow from the diagnostic; the operator path sees the real one.
                const bool movedOperand = (rhs.value.isExplicitMove && rhs.cxxName.empty())
                    || (lhs.value.isExplicitMove && lhs.cxxName.empty());
                // An owned C++ prvalue lhs goes straight to the operator, as `+` does: the infix
                // wrapper forwards it as an rvalue and the operator's by-value parameter moves it.
                LLVMBackend::NamedVariable ownedLhsProbe;
                ownedLhsProbe.IsRvalue = true;
                ownedLhsProbe.Storage = lhsStorage;
                const bool ownedClassLhs = lv.isRvalue && lhs.cxxName.empty() && rhs.cxxName.empty()
                    && !lhsClass.empty() && compiler->IsCxxRecord(lhsClass) && lhsStorage != nullptr
                    && compiler->IsOwnedCxxPrvalueArgument(ownedLhsProbe,
                        compiler->GetType(LLVMBackend::TypeAndValue{ .TypeName = lhsClass }));
                if ((namedOperand || (!lhsClass.empty() && compiler->IsCxxRecord(lhsClass)))
                    && !movedOperand && !ownedClassLhs)
                    overload = TryCxxInfixShift(ctx, op, lhs, lhsClass, lhsStorage,
                                                rhs, rhsClass, rhsStorage);
                if (namedOperand && overload == nullptr) return {};
                if (overload == nullptr)
                    overload = TryBinaryOperatorOverload(
                        lv.value, op, rv.value, ctx, lv.elemType, rv.pointerDepth,
                        rv.elemPointer, lhsStorage, rhsStorage, false, true,
                        lv.isRvalue, rv.isRvalue, lv.sourceTypeName, rv.sourceTypeName,
                        lv.pointerDepth, lv.elemPointer, {}, {}, false,
                        rv.sourceCallerName, {}, false, {}, {},
                        rv.cxxPointeeConst, lv.cxxArithIdentity, rv.cxxArithIdentity, rv.isExplicitMove, lv.isExplicitMove,
                        lv.hasSourceType && lv.sourceType.IsCxxNullptrT,
                        rv.hasSourceType && rv.sourceType.IsCxxNullptrT,
                        false, lv.cxxPointeeConst, lv.cxxObjectCv, rv.cxxObjectCv);
                if (overload != nullptr)
                {
                    LLVMBackend::NamedVariable resultNV;
                    resultNV.Primary = overload;
                    resultNV.TypeAndValue = compiler->lastCallReturnType;
                    if (compiler->lastCxxRetValue_ == overload)
                        resultNV.Storage = compiler->lastCxxRetTemp_;
                    PrepareAliasCallResult(ctx, resultNV, false);
                    DiagnoseVoidResultConsumed(ctx, resultNV, use,
                                               std::format("'operator{}'", op));
                    ShiftPairResult result;
                    llvm::Value* carriedValue = nullptr;
                    llvm::Value* carriedStorage = nullptr;
                    bool carriedIsRvalue = false;
                    llvm::Type* carriedRefType = nullptr;
                    std::string carriedSourceType;
                    CarryCxxOperatorResult(ctx, overload, moreOperands, carriedValue,
                        carriedStorage, carriedIsRvalue, carriedRefType, carriedSourceType);
                    result.value = { carriedValue,
                        resultNV.TypeAndValue.IsUnsignedInteger() != -1 };
                    result.value.receiverStorage = carriedStorage;
                    result.value.isRvalue = carriedIsRvalue;
                    result.value.cxxRefValueType = carriedRefType;
                    result.value.sourceTypeName = carriedSourceType;
                    result.value.isAlias = resultNV.TypeAndValue.IsAlias;
                    result.named = resultNV;
                    return result;
                }
            }
            std::string opName = "operator" + op;
            // A call-result lhs has no declared type: its class comes from the loaded struct value.
            const std::string nativeLhsType = lhsType.empty() ? lhsClass : lhsType;
            if (!nativeLhsType.empty() && compiler->IsDataStructure(nativeLhsType)
                && HasOperatorOverloadForFirstParam(opName, nativeLhsType))
            {
                LLVMBackend::NamedVariable la;
                la.TypeAndValue = lhsNV.TypeAndValue;
                la.TypeAndValue.TypeName = nativeLhsType;
                la.TypeAndValue.VariableName = "";
                la.Primary      = lv.value;
                la.BaseType     = lv.value ? lv.value->getType() : nullptr;
                la.CallerName   = lhsName;
                LLVMBackend::NamedVariable ra;
                ra.TypeAndValue = rhsNV.TypeAndValue;
                ra.TypeAndValue.TypeName = rhsType.empty() ? rhsClass : rhsType;
                ra.TypeAndValue.VariableName = "";
                ra.Primary      = rv.value;
                ra.BaseType     = rv.value ? rv.value->getType() : nullptr;
                ra.CallerName   = rhsName;
                FlushCallPostfix();
                auto* res = compiler->CreateOverloadedFunctionCall(opName, { la, ra });
                LLVMBackend::NamedVariable resultNV;
                resultNV.Primary = res;
                resultNV.TypeAndValue = compiler->lastCallReturnType;
                PrepareAliasCallResult(ctx, resultNV, false);
                DiagnoseVoidResultConsumed(ctx, resultNV, use, std::format("'operator{}'", op));
                ShiftPairResult result;
                result.value = { res, resultNV.TypeAndValue.IsUnsignedInteger() != -1 };
                result.value.receiverStorage = resultNV.Storage;
                result.value.cxxRefValueType = CxxReferenceResultType(ctx, resultNV, res);
                result.named = resultNV;
                return result;
            }
            // A native struct lhs with no operator of this name: refuse here, not in the verifier.
            if (!nativeLhsType.empty() && compiler->IsDataStructure(nativeLhsType)
                && !compiler->IsCxxRecord(nativeLhsType) && lv.value != nullptr
                && lv.value->getType()->isStructTy())
            {
                std::string shown = SpellType(*compiler,
                    LLVMBackend::TypeAndValue{ .TypeName = nativeLhsType });
                if (shown.empty()) shown = nativeLhsType;
                LogErrorContext(ctx, std::format("no operator '{}' for type '{}'", op, shown));
                return {};
            }
        }

        // C 6.5.7: the result is the promoted LEFT operand, so a sub-int unsigned left is a signed int.
        const bool leftPromotedUnsigned = lv.isUnsigned && lv.value != nullptr
            && lv.value->getType()->isIntegerTy() && lv.value->getType()->getIntegerBitWidth() >= 32;
        ShiftPairResult result;
        result.value = { compiler->CreateOperation(op, lv, rv, lv.isUnsigned, rv.isUnsigned),
                         leftPromotedUnsigned };
        std::string leftIdentity = CxxScalarOperandIdentity(
            compiler, lv.cxxArithIdentity, lv.sourceTypeName, lv.value);
        if (leftIdentity.empty()) leftIdentity = lhs.literalType;
        if (!leftIdentity.empty() && result.value.value != nullptr)
            result.value.cxxArithIdentity = ScalarArithmeticResultName(
                compiler, leftIdentity, {}, result.value.value->getType());
        // C 6.5.7: the count is ignored; the result is a narrow int only for a runtime narrow left operand.
        if (lv.value != nullptr && !llvm::isa<llvm::Constant>(lv.value))
            result.value.arithNarrowWidth = static_cast<uint8_t>(LLVMBackend::ArithOperandNarrowWidth(lv));
        return result;
    }

LLVMBackend::TypedValue MainListener::ParseShiftExpression(CFlatParser::ShiftExpressionContext* ctx, ResultUse use) {
        CallPostfixScope operatorPostfix(this);
        DeclExpectedTypeGate declExpectedGate(&declExpectedType, ctx->children.size() == 1);
        auto nextCtxs = ctx->additiveExpression();
        if (nextCtxs.empty())
        {
            LogErrorContext(ctx, "Shift expression has no operands.");
            return {};
        }
        if (nextCtxs.size() == 1)
            return ParseAdditiveExpression(nextCtxs[0], use);

        std::vector<std::string> operators;
        for (size_t i = 0; i < ctx->children.size(); ++i)
        {
            auto* term = dynamic_cast<antlr4::tree::TerminalNode*>(ctx->children[i]);
            if (term == nullptr) continue;
            if (term->getText() == "<<")
            {
                operators.push_back("<<");
            }
            else if (term->getText() == ">" && i + 1 < ctx->children.size())
            {
                auto* next = dynamic_cast<antlr4::tree::TerminalNode*>(ctx->children[i + 1]);
                if (next != nullptr && next->getText() == ">")
                {
                    operators.push_back(">>");
                    ++i;
                }
            }
        }
        if (operators.size() + 1 != nextCtxs.size())
        {
            LogErrorContext(ctx, "Shift expression has unexpected operator count.");
            return {};
        }

        auto* compiler = Compiler(ctx);
        auto lookup = [&](const std::string& name) {
            if (name.empty()) return LLVMBackend::NamedVariable{};
            auto named = compiler->GetLocalVariable(name);
            if (named.Storage == nullptr) named = compiler->GetGlobalVariableNV(name);
            return named;
        };

        auto literalTypeOf = [](antlr4::ParserRuleContext* operand, const LLVMBackend::TypedValue& v) {
            auto* constant = llvm::dyn_cast_or_null<llvm::ConstantInt>(v.value);
            if (constant == nullptr || operand == nullptr) return std::string();
            const std::string text = operand->getText();
            if (text.size() >= 3 && text.front() == '\'' && text.back() == '\'') return std::string("char");
            size_t i = !text.empty() && text[0] == '-' ? 1 : 0;
            if (i >= text.size() || !std::isdigit((unsigned char)text[i])) return std::string();
            for (; i < text.size(); ++i)
                if (!std::isalnum((unsigned char)text[i]) && text[i] != '_') return std::string();
            const int64_t value = constant->getSExtValue();
            return std::string(value >= INT32_MIN && value <= INT32_MAX ? "int" : "long");
        };
        // `std.cout` / `std.endl`: a dotted name in an imported C++ namespace that names no
        // CFlat variable or type. A function (template) name such as a manipulator is spelled
        // into the C++ expression too, where C++ deduces it against the stream.
        auto cxxNameOf = [&](antlr4::ParserRuleContext* operand) -> std::string {
            std::string text = operand != nullptr ? operand->getText() : std::string();
            const size_t lastDot = text.rfind('.');
            if (lastDot == std::string::npos || lastDot == 0 || lastDot + 1 == text.size())
                return {};
            for (size_t i = 0; i < text.size(); ++i)
            {
                const char c = text[i];
                const bool start = i == 0 || text[i - 1] == '.';
                if (c == '.' ? start : !(std::isalpha((unsigned char)c) || c == '_'
                                         || (!start && std::isdigit((unsigned char)c))))
                    return {};
            }
            if (!compiler->IsCxxNamespace(text.substr(0, text.find('.')))
                || !compiler->IsCxxNamespace(text.substr(0, lastDot)))
                return {};
            if (lookup(text).Storage != nullptr || compiler->IsDataStructure(text)) return {};
            return text;
        };
        ShiftOperand lhs;
        lhs.cxxName = cxxNameOf(nextCtxs[0]);
        if (lhs.cxxName.empty())
        {
            lhs.value = ParseCxxFoldOperand(compiler, [&] {
                return ParseAdditiveExpression(nextCtxs[0], ResultUse::Value);
            });
            NormalizeCxxReferenceOperand(ctx, lhs.value);
            lhs.name = TryGetSimpleIdentifier(nextCtxs[0]);
            lhs.named = lookup(lhs.name);
            lhs.literalType = literalTypeOf(nextCtxs[0], lhs.value);
            lhs.expr = nextCtxs[0];
        }
        for (size_t i = 1; i < nextCtxs.size(); ++i)
        {
            ShiftOperand rhs;
            rhs.cxxName = cxxNameOf(nextCtxs[i]);
            if (rhs.cxxName.empty())
            {
                rhs.value = ParseCxxFoldOperand(compiler, [&] {
                    return ParseAdditiveExpression(nextCtxs[i], ResultUse::Value);
                });
                NormalizeCxxReferenceOperand(ctx, rhs.value);
                rhs.name = TryGetSimpleIdentifier(nextCtxs[i]);
                rhs.named = lookup(rhs.name);
                rhs.literalType = literalTypeOf(nextCtxs[i], rhs.value);
                rhs.expr = nextCtxs[i];
            }
            auto pair = ParseShiftPair(lhs, rhs, operators[i - 1], ctx,
                                       i + 1 == nextCtxs.size() ? use : ResultUse::Value,
                                       i + 1 < nextCtxs.size());
            if (pair.value.value == nullptr) return {};
            lhs.value = pair.value;
            lhs.named = pair.named;
            lhs.name = pair.name;
            lhs.accumulated = true;
            lhs.expr = nullptr;
            lhs.cxxName.clear();
            lhs.literalType.clear();
            // The pair returned a reference: reload the referenced object so the next operand
            // operates on it, not on the pointer (a chained `a << b << c`).
            if (i + 1 < nextCtxs.size() && lhs.value.cxxRefValueType != nullptr)
            {
                NormalizeCxxReferenceOperand(ctx, lhs.value);
                lhs.value.isAlias = true;
                lhs.named = {};
                lhs.name.clear();
            }
        }
        return lhs.value;
    }

// void has no size: `p + n`, `n + p` and `p - q` on a void* are refused before any GEP or
// element-size query (LLVM would assert sizing an unsized type).
void MainListener::LogVoidPointerArithmetic(antlr4::ParserRuleContext* ctx) {
        LogErrorContext(ctx, "pointer arithmetic on 'void*' is not allowed - 'void' has no size; "
            "cast to a typed pointer such as 'u8*' first");
    }

LLVMBackend::TypedValue MainListener::ParseAdditiveExpression(CFlatParser::AdditiveExpressionContext* ctx, ResultUse use) {
        CallPostfixScope operatorPostfix(this);
        DeclExpectedTypeGate declExpectedGate(&declExpectedType, ctx->children.size() == 1);
        auto nextCtxs = ctx->multiplicativeExpression();

        if (nextCtxs.size() == 1)
        {
            return ParseMultiplicativeExpression(nextCtxs[0], use);
        }
        else if (nextCtxs.size() > 1)
        {
            // Clear lastCallReturnsOwned before each operand: a preceding owned operator+ result would
            // otherwise leave the flag set and mis-register the next plain-variable operand (double-free).
            Compiler(ctx)->lastCallReturnsOwned = false;
            auto lv = ParseCxxFoldOperand(Compiler(ctx), [&] {
                return ParseMultiplicativeExpression(nextCtxs[0], ResultUse::Value);
            });
            NormalizeCxxReferenceOperand(ctx, lv);
            llvm::Value* lvalue = lv.value;
            LLVMBackend::TypedValue arithRunning = lv;
            llvm::Value* lhsStorage = lv.receiverStorage;
            bool lhsIsRvalue = lv.isRvalue;
            bool lhsIsAlias = lv.isAlias;
            LLVMBackend::TypeAndValue lhsAliasType = lv.sourceType;
            llvm::Type* lhsRefType = nullptr;
            std::string lhsSourceTypeName = lv.sourceTypeName;
            std::string lhsArithIdentity = lv.cxxArithIdentity;
            if (lhsArithIdentity.empty())
                lhsArithIdentity = CxxScalarOperandIdentity(
                    Compiler(ctx), {}, lv.sourceTypeName, lv.value);
            std::string lhsNativeArithmeticTypeName;
            std::string leftLiteralIdentity = CxxLiteralOperandIdentity(nextCtxs[0]);
            bool lu = lv.isUnsigned;
            llvm::Type* elemType = lv.elemType;
            bool unsignedStorage = false;
            bool fixedArrayPointerArithmetic = false;
            LLVMBackend::TypeAndValue pointerArithmeticSourceType = lv.sourceType;
            int pointerArithmeticDepth = lv.pointerDepth;
            bool hasPointerArithmeticResultType = false;
            TrackOwnedStringOperatorResult(Compiler(ctx), lvalue);
            // String concat (+) only borrows its operands: a plain (non-move) string-returning
            // call leaves lastCallReturnsOwned false, so TrackOwnedStringOperatorResult skips it.
            // Register call-result string operands directly so they are freed at end-of-statement.
            RegisterBorrowedStringOperandTemp(Compiler(ctx), lvalue);

            for (size_t i = 1; i < nextCtxs.size(); i++)
            {
                Compiler(ctx)->lastCallReturnsOwned = false;
                auto parseRightOperand = [&] {
                    return ParseCxxFoldOperand(Compiler(ctx), [&] {
                        return ParseMultiplicativeExpression(nextCtxs[i], ResultUse::Value);
                    });
                };
                auto rv = [&] {
                    LLVMBackend::TypedValue parsed;
                    if (ParseTreeContainsDefault(nextCtxs[i]))
                    {
                        CxxCallArgumentParseStateScope argumentState(Compiler(ctx));
                        parsed = parseRightOperand();
                        auto* temp = llvm::dyn_cast_or_null<llvm::AllocaInst>(
                            Compiler(ctx)->lastCxxRetTemp_);
                        auto* resultType = llvm::dyn_cast_or_null<llvm::StructType>(
                            parsed.value != nullptr ? parsed.value->getType() : nullptr);
                        if (temp != nullptr && parsed.value == Compiler(ctx)->lastCxxRetValue_
                            && resultType != nullptr && resultType->hasName()
                            && Compiler(ctx)->IsForeignNontrivialCxxClass(resultType->getName().str())
                            && temp->getAllocatedType() == resultType)
                        {
                            parsed.receiverStorage = temp;
                            parsed.isRvalue = true;
                            LLVMBackend::NamedVariable tempProbe;
                            tempProbe.Primary = parsed.value;
                            tempProbe.Storage = temp;
                            tempProbe.BaseType = resultType;
                            tempProbe.TypeAndValue.TypeName = resultType->getName().str();
                            if (!Compiler(ctx)->IsOwnedTempValue(tempProbe))
                                Compiler(ctx)->RegisterOwnedStructTemp(temp,
                                    resultType->getName().str());
                        }
                    }
                    else
                        parsed = parseRightOperand();
                    return parsed;
                }();
                NormalizeCxxReferenceOperand(ctx, rv);
                llvm::Value* rvalue = rv.value;
                bool ru = rv.isUnsigned;
                TrackOwnedStringOperatorResult(Compiler(ctx), rvalue);
                RegisterBorrowedStringOperandTemp(Compiler(ctx), rvalue);
                std::string op = ctx->children[i * 2 - 1]->getText();
                const size_t operatorIndex = i * 2 - 1;
                bool isPointerDifference = false;
                // Set again only by the built-in arithmetic arm below.
                const std::string leftArith = lhsArithIdentity;
                const std::string leftIdentity = !lhsArithIdentity.empty() ? lhsArithIdentity
                    : !leftLiteralIdentity.empty() ? leftLiteralIdentity : lhsSourceTypeName;
                const std::string leftNativeIdentity = !lhsNativeArithmeticTypeName.empty()
                    ? lhsNativeArithmeticTypeName
                    : !lhsSourceTypeName.empty() ? lhsSourceTypeName
                    : InferTernaryArmType(lv).TypeName;
                leftLiteralIdentity.clear();
                lhsArithIdentity.clear();

                if (lvalue->getType()->isPointerTy() && rvalue->getType()->isPointerTy() && op == "-")
                {
                    isPointerDifference = true;
                    // ptr - ptr -> element count (C ptrdiff_t semantics)
                    if ((elemType && elemType->isVoidTy()) || (rv.elemType && rv.elemType->isVoidTy()))
                    {
                        LogVoidPointerArithmetic(ctx);
                        elemType = nullptr;
                    }
                    auto* i64Ty = Compiler(ctx)->builder->getInt64Ty();
                    auto* byteDiff = Compiler(ctx)->builder->CreateSub(
                        Compiler(ctx)->builder->CreatePtrToInt(lvalue, i64Ty),
                        Compiler(ctx)->builder->CreatePtrToInt(rvalue, i64Ty),
                        "ptrdiff");
                    if (elemType)
                    {
                        Compiler(ctx)->TouchWaitingLayout(elemType);
                        // An unsized or zero-size element has no stride: dividing by it traps at run time.
                        if (!elemType->isSized()
                            || Compiler(ctx)->module->getDataLayout().getTypeAllocSize(elemType) == 0)
                            LogErrorContext(ctx, "pointer difference: the element type has no size, "
                                                 "so there is no element count to divide by. Cast both "
                                                 "pointers to a typed pointer first.");
                        auto* elemSize = Compiler(ctx)->GetTypeSizeBytes(elemType);
                        lvalue = Compiler(ctx)->builder->CreateSDiv(byteDiff, elemSize, "ptrdiff_elem");
                    }
                    else
                    {
                        lvalue = byteDiff;
                    }
                    elemType = nullptr;
                    fixedArrayPointerArithmetic = false;
                    hasPointerArithmeticResultType = false;
                    // ptrdiff_t is signed: the target long where long is 64-bit, else long long (i64).
                    lhsSourceTypeName = PtrdiffTypeName(Compiler(ctx));
                    lhsNativeArithmeticTypeName = lhsSourceTypeName;
                    lu = false;
                }
                else if (elemType && lvalue->getType()->isPointerTy()
                    && rvalue && rvalue->getType()->isIntegerTy()
                    && (op == "+" || op == "-"))
                {
                    if (lv.isArrayView || rv.isArrayView)
                        LogErrorContext(ctx, "pointer arithmetic is not allowed on an array-view 'T[]' - index it with 'a[i]' instead "
                            "(the view spans a whole allocation; arithmetic would create an aliasing pointer the noalias contract forbids)");
                    // Pointer arithmetic: ptr + int / ptr - int -> GEP
                    if (elemType->isVoidTy())
                    {
                        LogVoidPointerArithmetic(ctx);
                        continue;
                    }
                    if (rvalue->getType()->isIntegerTy(1))
                        rvalue = Compiler(ctx)->builder->CreateZExt(
                            rvalue, Compiler(ctx)->builder->getInt64Ty(), "bool_index");
                    if (op == "-")
                        rvalue = Compiler(ctx)->builder->CreateNeg(rvalue, "neg");
                    lvalue = Compiler(ctx)->CreateGEP(elemType, lvalue, rvalue, "ptrarith");
                    lhsStorage = nullptr;
                    lhsIsRvalue = true;
                    lhsIsAlias = false;
                    lhsAliasType = {};
                    lhsRefType = nullptr;
                    lhsNativeArithmeticTypeName.clear();
                    // A chained `1 + p + n` keeps the type the earlier step produced; only the
                    // first step on the original left operand reads lv.
                    const bool firstStepOnLeft = !hasPointerArithmeticResultType;
                    hasPointerArithmeticResultType = true;
                    if (firstStepOnLeft)
                    {
                        pointerArithmeticSourceType = lv.sourceType;
                        pointerArithmeticDepth = lv.pointerDepth;
                    }
                    if (firstStepOnLeft && !fixedArrayPointerArithmetic && lv.hasSourceType
                        && (lv.sourceType.ConstArraySize != 0
                            || !lv.sourceType.ConstInnerDimensions.empty())
                        && !lv.sourceType.IsArrayView)
                    {
                        fixedArrayPointerArithmetic = true;
                        pointerArithmeticSourceType = lv.sourceType;
                    }
                    // elemType stays the same - result is still a pointer to the same element type
                }
                else if (op == "+" && rv.elemType != nullptr && rvalue->getType()->isPointerTy()
                    && lvalue->getType()->isIntegerTy())
                {
                    if (rv.elemType->isVoidTy())
                    {
                        LogVoidPointerArithmetic(ctx);
                        lvalue = rvalue;
                        elemType = nullptr;
                        continue;
                    }
                    if (lvalue->getType()->isIntegerTy(1))
                        lvalue = Compiler(ctx)->builder->CreateZExt(
                            lvalue, Compiler(ctx)->builder->getInt64Ty(), "bool_index");
                    lvalue = Compiler(ctx)->CreateGEP(rv.elemType, rvalue, lvalue, "ptrarith");
                    lhsStorage = nullptr;
                    lhsIsRvalue = true;
                    lhsIsAlias = false;
                    lhsAliasType = {};
                    lhsRefType = nullptr;
                    lhsNativeArithmeticTypeName.clear();
                    lhsSourceTypeName.clear();
                    elemType = rv.elemType;
                    hasPointerArithmeticResultType = true;
                    pointerArithmeticSourceType = rv.sourceType;
                    pointerArithmeticDepth = rv.pointerDepth;
                    fixedArrayPointerArithmetic = rv.hasSourceType
                        && (rv.sourceType.ConstArraySize != 0
                            || !rv.sourceType.ConstInnerDimensions.empty())
                        && !rv.sourceType.IsArrayView;
                    if (fixedArrayPointerArithmetic)
                    {
                        pointerArithmeticSourceType = rv.sourceType;
                        pointerArithmeticDepth = rv.pointerDepth;
                    }
                }
                else
                {
                    unsigned leftBits = BinaryOperandBits(lvalue);
                    unsigned rightBits = BinaryOperandBits(rvalue);
                    CxxFoldSretScope foldSret(Compiler(ctx), i + 1 < nextCtxs.size());
                    auto* overload = TryBinaryOperatorOverload(lvalue, op, rvalue, ctx, nullptr,
                                                               rv.pointerDepth, rv.elemPointer,
                                                               lhsStorage, rv.receiverStorage,
                                                               true, true, lhsIsRvalue, rv.isRvalue,
                                                               lvalue == lv.value
                                                                   ? lv.sourceTypeName
                                                                   : std::string(),
                                                               rv.sourceTypeName,
                                                               lvalue == lv.value ? lv.pointerDepth : 0,
                                                               lvalue == lv.value && lv.elemPointer,
                                                               lvalue == lv.value ? lv.sourceCallerName : std::string(),
                                                               lvalue == lv.value ? lv.sourceFieldName : std::string(),
                                                               lvalue == lv.value && lv.sourceIsElementAccess,
                                                               rv.sourceCallerName, rv.sourceFieldName,
                                                               rv.sourceIsElementAccess,
                                                               lvalue == lv.value ? lv.conditionalMoveSources
                                                                                   : std::vector<LLVMBackend::ConditionalMoveSource>{},
                                                               rv.conditionalMoveSources,
                                                               rv.cxxPointeeConst,
                                                               leftArith,
                                                               rv.cxxArithIdentity, rv.isExplicitMove, lvalue == lv.value && lv.isExplicitMove,
                                                               lvalue == lv.value && lv.hasSourceType && lv.sourceType.IsCxxNullptrT,
                                                               rv.hasSourceType && rv.sourceType.IsCxxNullptrT,
                                                               false, lvalue == lv.value && lv.cxxPointeeConst,
                                                               lvalue == lv.value ? lv.cxxObjectCv : 0,
                                                               rv.cxxObjectCv,
                                                               lhsIsAlias);

                // char* + char* concatenation: TryBinaryOperatorOverload dispatches off a struct lvalue
                    // and can't reach raw i8*; both must qualify as c-strings so int* + int* still errors.
                    auto isCStr = [&](llvm::Value* v, llvm::Type* et) -> bool {
                        if (et && et->isIntegerTy(8)) return true;
                        if (auto* c = llvm::dyn_cast<llvm::Constant>(v))
                            return Compiler(ctx)->stringLiteralLenByPtr.count(c) > 0;
                        return false;
                    };
                    if (overload == nullptr && op == "+"
                        && lvalue->getType()->isPointerTy() && rvalue->getType()->isPointerTy()
                        && isCStr(lvalue, elemType) && isCStr(rvalue, rv.elemType))
                    {
                        LLVMBackend::NamedVariable leftNV;
                        leftNV.Primary  = lvalue;
                        leftNV.BaseType = lvalue->getType();
                        leftNV.TypeAndValue.TypeName = "char";
                        leftNV.TypeAndValue.Pointer  = true;
                        LLVMBackend::NamedVariable rightNV;
                        rightNV.Primary  = rvalue;
                        rightNV.BaseType = rvalue->getType();
                        rightNV.TypeAndValue.TypeName = "char";
                        rightNV.TypeAndValue.Pointer  = true;
                        FlushCallPostfix();
                        overload = Compiler(ctx)->CreateOverloadedFunctionCall("operator+", { leftNV, rightNV });
                        // Owned result: track it as an end-of-expression temporary so a
                        // bare `a + b` or a chained intermediate is freed (it is unregistered
                        // when bound to a named local / moved). See string-concat-temp-flush.md.
                        TrackOwnedStringOperatorResult(Compiler(ctx), overload);
                    }

                    if (overload)
                    {
                        LLVMBackend::NamedVariable resultNV;
                        resultNV.Primary = overload;
                        resultNV.TypeAndValue = Compiler(ctx)->lastCallReturnType;
                        DiagnoseVoidResultConsumed(ctx, resultNV, use, std::format("'operator{}'", op));
                    }
                    if (overload != nullptr)
                    {
                        lhsNativeArithmeticTypeName.clear();
                        CarryCxxOperatorResult(ctx, overload, i + 1 < nextCtxs.size(), lvalue,
                                               lhsStorage, lhsIsRvalue, lhsRefType,
                                               lhsSourceTypeName);
                        const auto& returnType = Compiler(ctx)->lastCallReturnType;
                        lhsIsAlias = returnType.IsAlias && !returnType.Pointer
                            && !Compiler(ctx)->IsCxxRecord(returnType.TypeName);
                        if (lhsIsAlias)
                        {
                            lhsAliasType = returnType;
                            lhsStorage = overload;
                            lhsIsRvalue = false;
                            lhsRefType = nullptr;
                            lhsSourceTypeName = returnType.TypeName;
                            if (lvalue != nullptr && lvalue->getType()->isPointerTy())
                            {
                                auto* aliasValueType = Compiler(ctx)->GetType(returnType);
                                if (aliasValueType != nullptr && aliasValueType->isStructTy())
                                    lvalue = Compiler(ctx)->CreateLoad(aliasValueType, overload);
                            }
                        }
                        else
                            lhsAliasType = {};
                    }
                    else
                    {
                        const LLVMBackend::TypedValue arithLhs = ArithRunningOperand(arithRunning, lvalue);
                        lvalue = Compiler(ctx)->CreateOperation(op, lvalue, rvalue, lu, ru);
                        arithRunning = ArithComputedOperand(lvalue,
                            LLVMBackend::ArithResultNarrowWidth(arithLhs, rv));
                        lhsStorage = nullptr;
                        lhsIsRvalue = true;
                        lhsIsAlias = false;
                        lhsAliasType = {};
                        lhsRefType = nullptr;
                        lhsSourceTypeName.clear();
                        lhsNativeArithmeticTypeName = lvalue != nullptr
                            ? ScalarArithmeticResultName(Compiler(ctx), leftNativeIdentity,
                                InferTernaryArmType(rv).TypeName, lvalue->getType())
                            : std::string();
                        lhsSourceTypeName = lhsNativeArithmeticTypeName;
                        if (lvalue != nullptr)
                        {
                            std::string rightIdentity = rv.cxxArithIdentity;
                            if (rightIdentity.empty()) rightIdentity = CxxLiteralOperandIdentity(nextCtxs[i]);
                            if (rightIdentity.empty()) rightIdentity = rv.sourceTypeName;
                            lhsArithIdentity = ScalarArithmeticResultName(Compiler(ctx), leftIdentity,
                                rightIdentity, lvalue->getType());
                        }
                    }
                    unsignedStorage = !overload && lvalue != nullptr && lvalue->getType()->isIntegerTy()
                        && lvalue->getType()->getIntegerBitWidth() < 32 && (lu || ru);
                    // An overload's signedness is its RETURN type, not the operands' flags.
                    lu = overload ? Compiler(ctx)->lastCallReturnType.IsUnsignedInteger() != -1
                                  : BinaryJoinIsUnsigned(lu, leftBits, ru, rightBits);
                    elemType = nullptr;  // arithmetic result is no longer a pointer
                    hasPointerArithmeticResultType = false;
                }

                // Pointer arithmetic can precede the real difference in a flat left-associative chain.
                // Retain only temps consumed by the emitted pointer-minus-pointer operation; an owner in a
                // sibling argument of the same call keeps its own classification.
                if (isPointerDifference)
                    for (auto& temp : arrowOwnedTemps_)
                        if (temp.Origin != nullptr && IsParseTreeAncestor(ctx, temp.Origin)
                            && IsCallArgumentContaining(temp.PendingCallResult,
                                temp.PendingCallParen, ctx))
                        {
                            temp.PendingCallResult = nullptr;
                            temp.PendingCallValue = nullptr;
                            temp.PendingCallReturnType = {};
                            temp.PendingCallParen = nullptr;
                            temp.ArgumentIndex = SIZE_MAX;
                            temp.NamedArgument = false;
                            temp.HasPendingCallValue = false;
                        }
                for (auto& temp : arrowOwnedTemps_)
                {
                    if (temp.PendingPointerDifference != ctx
                        || temp.PointerDifferenceOperatorIndex != operatorIndex)
                        continue;
                    if (!isPointerDifference)
                    {
                        size_t nextMinus = SIZE_MAX;
                        if (lvalue != nullptr && lvalue->getType()->isPointerTy())
                        {
                            for (size_t candidate = operatorIndex + 2;
                                 candidate + 1 < ctx->children.size(); candidate += 2)
                            {
                                auto* terminal = dynamic_cast<antlr4::tree::TerminalNode*>(
                                    ctx->children[candidate]);
                                if (terminal != nullptr
                                    && terminal->getSymbol()->getType() == CFlatParser::Minus)
                                {
                                    nextMinus = candidate;
                                    break;
                                }
                            }
                        }
                        if (nextMinus != SIZE_MAX)
                        {
                            temp.PointerDifferenceOperatorIndex = nextMinus;
                            continue;
                        }
                        if (temp.PendingCallResult == nullptr)
                            Compiler(ctx)->UnregisterOwnedStructTemp(temp.Storage);
                    }
                    else
                    {
                        temp.PendingCallResult = nullptr;
                        temp.PendingCallValue = nullptr;
                        temp.PendingCallReturnType = {};
                        temp.PendingCallParen = nullptr;
                        temp.ArgumentIndex = SIZE_MAX;
                        temp.NamedArgument = false;
                        temp.HasPendingCallValue = false;
                    }
                    temp.PendingPointerDifference = nullptr;
                    temp.PointerDifferenceOperatorIndex = SIZE_MAX;
                }
            }

            LLVMBackend::TypedValue result{ lvalue, lu };
            result.arithNarrowWidth = ArithRunningOperand(arithRunning, lvalue).arithNarrowWidth;
            result.isRvalue = lhsIsRvalue;
            result.isPointerArithmeticResult = hasPointerArithmeticResultType;
            result.receiverStorage = lhsStorage;
            result.cxxRefValueType = lhsRefType;
            result.isAlias = lhsIsAlias || (lhsRefType != nullptr && !lhsSourceTypeName.empty());
            result.storage = result.isAlias ? lhsStorage : nullptr;
            result.sourceTypeName = lhsSourceTypeName;
            if (lhsIsAlias)
            {
                result.sourceType = lhsAliasType;
                result.hasSourceType = true;
            }
            result.cxxArithIdentity = lhsArithIdentity;
            if (!lhsNativeArithmeticTypeName.empty())
            {
                result.sourceType.TypeName = lhsNativeArithmeticTypeName;
                result.hasSourceType = true;
            }
            if (hasPointerArithmeticResultType
                && (pointerArithmeticSourceType.Pointer || fixedArrayPointerArithmetic
                    || !pointerArithmeticSourceType.TypeName.empty()
                    || !lhsSourceTypeName.empty() || elemType != nullptr))
            {
                result.sourceType = pointerArithmeticSourceType;
                if (result.sourceType.TypeName.empty())
                    result.sourceType.TypeName = !lhsSourceTypeName.empty()
                        ? lhsSourceTypeName : LLVMTypeToTypeName(elemType);
                if (fixedArrayPointerArithmetic)
                    result.sourceType.PointerDepth = pointerArithmeticDepth > 0 ? pointerArithmeticDepth : 1;
                else if (result.sourceType.PointerDepth == 0)
                    result.sourceType.PointerDepth = pointerArithmeticDepth > 0 ? pointerArithmeticDepth : 1;
                if (fixedArrayPointerArithmetic)
                    result.sourceType.ElemPointer = result.sourceType.ElemPointer
                        || result.sourceType.Pointer;
                result.sourceType.Pointer = true;
                if (fixedArrayPointerArithmetic)
                {
                    result.sourceType.ConstArraySize = 0;
                    result.sourceType.ConstInnerDimensions.clear();
                    result.sourceType.IsArrayView = false;
                }
                result.sourceTypeName = result.sourceType.TypeName;
                result.hasSourceType = true;
            }
            result.isUnsignedStorage = unsignedStorage;
            result.elemType = elemType;
            return result;
        }

        LogErrorContext(ctx, "Additive expression has no operands.");
        return {};
    }

void MainListener::VerifyFuncPtrAssignmentMoveFlags(const std::string& funcName,
                                          const LLVMBackend::TypeAndValue& funcPtrType,
                                          antlr4::ParserRuleContext* ctx) {
        if (!funcPtrType.IsFunctionPointer) return;
        // No move modifiers anywhere -> nothing to enforce.
        bool anyMove = false;
        for (const auto& p : funcPtrType.FuncPtrParams) if (p.IsMove) { anyMove = true; break; }
        if (!anyMove)
        {
            // Even when destination has no move flags, reject if every overload only has move params.
            // We only flag when the function table has *no* matching all-borrow overload.
        }
        if (!Compiler(ctx)->HasFunctionWithMoveFlags(funcName, funcPtrType.FuncPtrParams))
        {
            std::string expected;
            for (size_t i = 0; i < funcPtrType.FuncPtrParams.size(); i++)
            {
                if (i) expected += ", ";
                if (funcPtrType.FuncPtrParams[i].IsMove) expected += "move ";
                LLVMBackend::TypeAndValue parameterType;
                parameterType.TypeName = funcPtrType.FuncPtrParams[i].TypeName;
                parameterType.Pointer = funcPtrType.FuncPtrParams[i].Pointer;
                parameterType.PointerDepth = funcPtrType.FuncPtrParams[i].PointerDepth;
                expected += SpellType(*Compiler(ctx), parameterType);
            }
            Compiler(ctx)->LogError(std::format(
                "function '{}' has no overload matching the 'move' modifiers of function pointer signature ({}) - 'move' is part of the function-pointer type and must agree on both sides",
                funcName, expected));
        }
    }

llvm::Value* MainListener::TagViewElementAccess(llvm::Value* loaded, const LLVMBackend::NamedVariable& namedVar) {
        if (namedVar.TypeAndValue.NoaliasScopeId >= 0)
            if (auto* inst = llvm::dyn_cast_or_null<llvm::Instruction>(loaded))
                Compiler()->AttachViewNoalias(inst, namedVar.TypeAndValue.NoaliasScopeId);
        return loaded;
    }

LLVMBackend::NamedVariable MainListener::LowerSpanElementAccess(
        antlr4::ParserRuleContext* ctx,
        const LLVMBackend::NamedVariable& structVar,
        llvm::Value* indexValue) {
        LLVMBackend::NamedVariable elem;
        if (indexValue == nullptr || structVar.Storage == nullptr
            || structVar.TypeAndValue.Pointer
            || !structVar.BaseType || !structVar.BaseType->isStructTy())
            return elem;

        auto* compiler = Compiler(ctx);
        int bufIndex = compiler->ArrayViewBufferFieldIndex(structVar.TypeAndValue.TypeName);
        if (bufIndex < 0)
            return elem;

        // Load the `_ptr` array-view field (the T* buffer).
        const auto& spanDS = compiler->GetDataStructure(structVar.TypeAndValue.TypeName);
        auto* bufGEP = compiler->CreateStructGEP(structVar.BaseType, structVar.Storage, (uint32_t)bufIndex);
        llvm::Value* bufPtr = compiler->CreateLoad(bufGEP);

        // Element TypeAndValue = the array-view field with the view-ness dropped (an indexed element
        // is a single slot, never a whole-allocation view), mirroring the IsArrayView subscript
        // branch in ParsePostfix. Key the alias scope to the receiver origin so distinct spans land
        // in distinct scopes; an unnamed (temporary) receiver yields scope -1 = no metadata.
        LLVMBackend::TypeAndValue elementTV = spanDS.StructFields[bufIndex];
        std::string originKey = structVar.TypeAndValue.VariableName;
        if (originKey.empty()) originKey = structVar.CallerName;
        int scopeId = compiler->GetOrMintViewScope(originKey);
        elementTV.IsArrayView = false;
        if (elementTV.ElemPointer)
            elementTV.ElemPointer = false;
        else
        {
            elementTV.Pointer = false;
            elementTV.IsInterfacePointer = false;
        }
        auto* elementType = compiler->GetType(elementTV);
        elementTV.NoaliasScopeId = scopeId;

        elem.Storage = compiler->CreateGEP(elementType, bufPtr, indexValue);
        elem.BaseType = elementType;
        elem.TypeAndValue = elementTV;
        // The buffer field IS a user `T[]` view, so its slots are live - same provenance the plain
        // `v[i]` subscript records. Today's sole caller (the span get/set fast path) reads only
        // Storage/BaseType, so no ownership arm consumes it yet; recording it keeps a future
        // caller from inheriting a FALSE negative, which is the unsound direction here.
        elem.IsViewElement = true;
        return elem;
    }

// A dereference or member use of a receiver is never exempt: `a[0]->next` reads through the moved value.
void MainListener::CheckMovedReceiver(const LLVMBackend::NamedVariable& receiver)
{
        CheckMovedUse(receiver, nullptr, nullptr);
    }

void MainListener::CheckMovedValueRead(const LLVMBackend::NamedVariable& nv, llvm::Value* readValue)
{
        CheckMovedUse(nv, deferredMovedReads_, readValue);
    }

void MainListener::CheckMovedUse(const LLVMBackend::NamedVariable& receiver,
                                 std::vector<DeferredMovedRead>* deferred, llvm::Value* readValue)
{
        const std::string movePath = LLVMBackend::FieldMoveKey(receiver);
        if (receiver.IdentifierLine > 0
            && (!receiver.IsElementAccess || !LLVMBackend::FieldMoveKey(receiver).empty()))
        {
            Compiler()->RecordMoveUse(receiver.CallerName, movePath,
                                      receiver.IdentifierLine, receiver.IdentifierColumn);
            if (auto moved = Compiler()->MovedUseSubject(receiver); !moved.empty())
            {
                const bool pointeeMove = moved == receiver.CallerName && IsPointeeMoveUse(receiver);
                if (deferred != nullptr)
                {
                    // Moved callables remain readable as null; other whole moved values keep the diagnostic.
                    const auto* encodedClosure = Compiler()->GetEncodedClosureType(
                        receiver.TypeAndValue.TypeName);
                    const bool callableValue = receiver.TypeAndValue.IsFunctionPointer
                        || encodedClosure != nullptr;
                    const bool ownSlot = callableValue
                        || (receiver.MovePathKey.find(']') != std::string::npos
                            && !movePath.empty()
                            && moved == movedf::DisplayPath(receiver.CallerName, movePath));
                    deferred->push_back({ readValue, moved,
                                          receiver.IdentifierLine, receiver.IdentifierColumn, ownSlot,
                                          pointeeMove });
                    return;
                }
                LogMovedUse(moved, receiver.IdentifierLine, receiver.IdentifierColumn,
                            pointeeMove,
                            receiver.IsExplicitMove && !receiver.FieldPathHasPointerDereference);
            }
        }
    }

bool MainListener::IsPointeeMoveUse(const LLVMBackend::NamedVariable& source)
{
        if (source.PointeeMove) return true;
        if (source.Storage != nullptr)
            if (const auto* binding = Compiler()->FindVariableByStorage(source.Storage))
                return binding->PointeeMove;
        if (!source.FieldName.empty() || !source.OwningStructName.empty()
            || source.CallerName.empty())
            return false;
        if (const auto* binding = Compiler()->FindLiveNamedVariable(source.CallerName))
            return binding->PointeeMove;
        return false;
    }

void MainListener::LogMovedUse(const std::string& subject, int line, int column, bool pointeeMove,
                               bool nonDerefUse)
{
        // No recorded identifier position: keep the ambient one, as the plain LogError did.
        if (line > 0)
        {
            Compiler()->currentLine = line;
            Compiler()->currentColumn = column;
        }
        Compiler()->LogError(MovedUseMessage(subject, pointeeMove, nonDerefUse));
    }

void MainListener::LogMovedUse(antlr4::ParserRuleContext* ctx, const std::string& subject, bool pointeeMove,
                               bool nonDerefUse)
{
        LogErrorContext(ctx, MovedUseMessage(subject, pointeeMove, nonDerefUse));
    }

std::string MainListener::MovedUseMessage(const std::string& subject, bool pointeeMove, bool nonDerefUse)
{
        if (!pointeeMove) return std::format("use of moved variable '{}'", subject);
        auto message = std::format("pointee of '{}' was moved", subject);
        if (nonDerefUse)
            message += std::format("; reassign '{}' before using it", subject);
        return message;
    }

llvm::Value* MainListener::LoadNamedVariable(LLVMBackend::NamedVariable& namedVar) {
        auto* compiler = Compiler();
        llvm::Value* result = LoadNamedVariableImpl(namedVar);
        // The moved check sees the value the read produced: a cast or comparison operand matches by identity.
        CheckMovedValueRead(namedVar, result);
        if (result != nullptr && namedVar.TypeAndValue.IsFatInterfaceValue()
            && !namedVar.TypeAndValue.Pointer)
            compiler->RegisterFatInterfaceValueTypeName(result, namedVar.TypeAndValue.TypeName);
        // Ledger a CODE value by the same value identity (see codeValues_). Synchronous check:
        // namedVar is not a stamped argument, so ask under the ambient occurrence.
        if (result != nullptr && compiler->ArgumentIsCodeValue(namedVar, compiler->CurrentCastOccurrence()))
            compiler->RegisterCodeValue(result);
        // Mirror ledger for the closure-widen gate, which asks the opposite question: a join of
        // values PROVEN to be data must not widen into a fat closure's code slot (see dataValues_).
        if (result != nullptr && compiler->ArgumentIsDataValue(namedVar))
            compiler->RegisterDataValue(result);
        const bool rawArrayBinding = namedVar.Storage != nullptr
            && (llvm::isa<llvm::AllocaInst>(namedVar.Storage)
                || llvm::isa<llvm::GlobalVariable>(namedVar.Storage))
            && namedVar.FieldName.empty() && !namedVar.IsElementAccess;
        if (result != nullptr && namedVar.AllocatedByRawNewArray && rawArrayBinding)
        {
            if (auto* count = compiler->LoadRawArrayLength(namedVar))
                compiler->RegisterRawArrayResult(result, count, namedVar.AllocAlignment, false);
        }
        return result;
    }

llvm::Value* MainListener::LoadNamedVariableImpl(LLVMBackend::NamedVariable& namedVar) {
        auto* compiler = Compiler();
        // Every value read of a named local. A dereference site records its Deref event BEFORE
        // calling this, so the read below cannot swallow its own check (see RecordNullRead).
        compiler->RecordNullRead(namedVar.CallerName, namedVar.Storage, namedVar.Primary);
        // A consumed `unique` field path reads as null; the read is a guard for its null state.
        if (std::string key = compiler->UniqueFieldNullKey(namedVar); !key.empty())
            compiler->RecordNullRead(key, namedVar.Storage, namedVar.Primary);
        if (namedVar.TypeAndValue.Pointer)
        {
            if (namedVar.Primary != nullptr)
                return namedVar.Primary;
            if (namedVar.Storage != nullptr)
            {
                // Pointer array decay: char*[3] used as char** -> GEP to first element.
                // Must check before the alloca load path below.
                if (namedVar.BaseType && llvm::isa<llvm::ArrayType>(namedVar.BaseType))
                {
                    auto* arrTy = llvm::cast<llvm::ArrayType>(namedVar.BaseType);
                    auto* zero = compiler->builder->getInt64(0);
                    return compiler->builder->CreateGEP(arrTy, namedVar.Storage, {zero, zero}, "arrptr");
                }
                if (llvm::isa<llvm::AllocaInst>(namedVar.Storage) ||
                    llvm::isa<llvm::GlobalVariable>(namedVar.Storage) ||
                    llvm::isa<llvm::GetElementPtrInst>(namedVar.Storage))
                    return TagViewElementAccess(compiler->CreateLoad(namedVar.Storage), namedVar);
                // Through-pointer dereference of a pointer type (e.g. *pp where pp is char**):
                // Storage is a raw loaded address - load again to get the pointer value.
                if (namedVar.BaseType && namedVar.BaseType->isPointerTy())
                    return TagViewElementAccess(compiler->CreateLoad(namedVar.BaseType, namedVar.Storage), namedVar);
                return namedVar.Storage;
            }
            return nullptr;
        }
        else
        {
            if (namedVar.Primary != nullptr)
                return namedVar.Primary;

            if (namedVar.Storage != nullptr)
            {
                // Array-to-pointer decay: fixed-size array used as a value decays to pointer to first element
                if (namedVar.BaseType && llvm::isa<llvm::ArrayType>(namedVar.BaseType))
                {
                    auto* arrTy = llvm::cast<llvm::ArrayType>(namedVar.BaseType);
                    auto* zero = compiler->builder->getInt64(0);
                    return compiler->builder->CreateGEP(arrTy, namedVar.Storage, {zero, zero}, "arrptr");
                }
                // An opaque C++ class (a layout cflat refused, e.g. virtual inheritance) has no
                // value to load: only its address binds (a reference), so the read stays poison.
                compiler->TouchWaitingLayout(namedVar.BaseType);
                if (namedVar.BaseType && namedVar.BaseType->isStructTy()
                    && !namedVar.BaseType->isSized())
                    return llvm::PoisonValue::get(namedVar.BaseType);
                // For through-pointer dereferences (Storage is a raw loaded ptr, not an alloca/gep/global),
                // use BaseType to emit the correctly-typed load (opaque pointers carry no type info).
                if (namedVar.BaseType
                    && !llvm::isa<llvm::AllocaInst>(namedVar.Storage)
                    && !llvm::isa<llvm::GlobalVariable>(namedVar.Storage)
                    && !llvm::isa<llvm::GetElementPtrInst>(namedVar.Storage))
                    return TagViewElementAccess(compiler->CreateLoad(namedVar.BaseType, namedVar.Storage), namedVar);
                return TagViewElementAccess(compiler->CreateLoad(namedVar.Storage), namedVar);
            }
        }

        // namedVar is empty - caller's block was already terminated (e.g. by a return-block inline).
        return nullptr;
    }

// C integer promotion for unary '-' / '~': an i8/i16/u8/u16 operand becomes a SIGNED i32
// before the operation, so `-(u8)128` is -128 and `~(u8)128` is -129 exactly as C computes them.
// A SIGNED narrow CONSTANT is left alone: cflat types a bare literal narrow on purpose, and the
// constant path below already folds it into the smallest fitting signed type.


llvm::Value* MainListener::TryUnaryOperatorOverload(
        llvm::Value* operand, const std::string& op,
        antlr4::ParserRuleContext* ctx, llvm::Value* operandStorage,
        const LLVMBackend::NamedVariable* operandSource, bool operandIsConst) {
        if (!operand) return nullptr;
        const bool operandExplicitMove = operandSource != nullptr && operandSource->IsExplicitMove;
        auto* compiler = Compiler(ctx);

        auto* ty = operand->getType();
        if (!ty->isStructTy()) return nullptr;
        auto* structTy = llvm::cast<llvm::StructType>(ty);
        if (structTy->isLiteral() || !structTy->hasName()) return nullptr;
        std::string typeName = structTy->getName().str();
        if (typeName == "__iface_fat_ptr") return nullptr;

        // The operand's own slot, only when the operand IS a plain load of it: a C++ operator
        // taking `this` / a non-const reference then mutates the object itself, not a copy.
        if (operandStorage != nullptr)
        {
            auto* load = llvm::dyn_cast<llvm::LoadInst>(operand);
            auto* valuePhi = llvm::dyn_cast<llvm::PHINode>(operand);
            auto* storagePhi = llvm::dyn_cast<llvm::PHINode>(operandStorage);
            bool parallelStorageJoin = valuePhi != nullptr && storagePhi != nullptr
                && valuePhi->getParent() == storagePhi->getParent()
                && valuePhi->getNumIncomingValues() == storagePhi->getNumIncomingValues();
            for (unsigned i = 0; parallelStorageJoin && i < valuePhi->getNumIncomingValues(); ++i)
                parallelStorageJoin = valuePhi->getIncomingBlock(i) == storagePhi->getIncomingBlock(i);
            if ((load == nullptr || load->getPointerOperand() != operandStorage)
                && !parallelStorageJoin)
                operandStorage = nullptr;
        }
        // Read-only objects (const C++ global, const& result) keep the copy: a non-const operator would write them.
        llvm::Value* readOnlyStorage = nullptr;
        bool constOperand = operandIsConst;
        // A C++ `volatile T&` result keeps its volatile into receiver cv ranking.
        bool volatileOperand = operandSource != nullptr
            && operandSource->TypeAndValue.IsCxxPointeeVolatile;
        // Only a `volatile T&` result (an alias) is a volatile LVALUE; a by-value cv result is a
        // prvalue, which elides into a by-value parameter. The call category does not tell them apart.
        const bool lvalueRefResult = operandSource != nullptr && operandSource->TypeAndValue.IsAlias
            && !operandSource->TypeAndValue.IsRvalueRef;
        const bool volatileLvalueOperand = volatileOperand && lvalueRefResult;
        // A `T&` call result is the object its load reads: that address is the operand's storage.
        if (operandStorage == nullptr && lvalueRefResult)
            if (auto* callLoad = llvm::dyn_cast_or_null<llvm::LoadInst>(operand))
                operandStorage = callLoad->getPointerOperand();
        if (operandStorage != nullptr)
        {
            auto* constGlobal = llvm::dyn_cast<llvm::GlobalVariable>(llvm::getUnderlyingObject(operandStorage));
            constOperand = constOperand || compiler->IsCxxConstReferent(operandStorage)
                || (constGlobal != nullptr && !compiler->IsCxxMutableFieldAccess(operandStorage)
                    && (constGlobal->isConstant()
                    || compiler->IsConstHeaderGlobal(constGlobal)));
            if (constOperand)
            {
                readOnlyStorage = operandStorage;
                operandStorage = nullptr;
            }
        }
        const bool mutableFieldReceiver = operandStorage != nullptr
            && compiler->IsCxxMutableFieldAccess(operandStorage);

        // A by-value cv C++ call result is a cv prvalue: rvalue, never a mutable target.
        int prvalueCv = 0;
        if (auto* callCtx = dynamic_cast<CFlatParser::UnaryExpressionContext*>(ctx);
            callCtx != nullptr && callCtx->castExpression() != nullptr
            && CxxArgumentLvalueKind(callCtx->castExpression()) == 2)
            prvalueCv = compiler->lastCallReturnValueCv_;
        const bool constPrvalue = (prvalueCv & 1) != 0;
        volatileOperand = volatileOperand || (prvalueCv & 2) != 0;
        std::string opName = "operator" + op;
        const std::string cxxBase = typeName.substr(0, typeName.find('$'));
        const size_t cxxDot = cxxBase.rfind('.');
        const std::string cxxFreeSourceName = cxxDot == std::string::npos
            ? opName : cxxBase.substr(0, cxxDot) + "." + opName;
        // A global-namespace class's member is named plain `operator-`, the same as its free
        // spelling (cxxFreeSourceName), so membership is IsMethod alone.
        auto isMemberReceiver = [&](const LLVMBackend::FunctionSymbol& candidate) {
            return !compiler->IsCxxRecord(typeName) || candidate.IsMethod;
        };
        llvm::Value* freeOperandStorage = operandStorage != nullptr ? operandStorage : readOnlyStorage;
        if (compiler->IsCxxRecord(typeName))
            compiler->EnsureCxxMemberProjected(typeName, opName);
        if (compiler->IsCxxRecord(typeName) && !UseCxxIncrementalRequests())
            compiler->TryBindRefusedCxxMember(typeName, opName);
        if (!compiler->GetFunction(opName))
            return TryUnaryFreeOperatorOverload(operand, op, ctx, freeOperandStorage,
                                                constOperand || constPrvalue,
                                                operandSource != nullptr && operandSource->IsExplicitMove,
                                                constPrvalue, volatileOperand, volatileLvalueOperand, lvalueRefResult);

        bool receiverFound = false;
        bool receiverConsumes = false;
        bool receiverSinkConsumes = false;
        // A FREE operator's only parameter is bound to the operand, so a written `move` is checked as
        // a call's argument; a member receiver is not a parameter (receiver move is out of scope).
        bool receiverIsFreeParam = false;
        if (auto it = compiler->functionTable.find(opName); it != compiler->functionTable.end())
            for (const auto& candidate : it->second)
                if (!candidate.Parameters.empty() && candidate.Parameters[0].TypeName == typeName
                    && isMemberReceiver(candidate))
                {
                    receiverFound = true;
                    receiverIsFreeParam = receiverIsFreeParam || !candidate.IsMethod;
                    const auto& param = candidate.Parameters[0];
                    if (!param.Pointer && param.IsMove) receiverConsumes = true;
                    const bool aliasReturnSlot = !candidate.IsCxx
                        && compiler->ParamIsAliasReturnSlot(candidate.ReturnType, param);
                    LLVMBackend::NamedVariable receiverArg = operandSource != nullptr
                        ? *operandSource : LLVMBackend::NamedVariable{};
                    receiverArg.Primary = operand;
                    receiverArg.BaseType = structTy;
                    receiverArg.TypeAndValue.TypeName = typeName;
                    receiverArg.TypeAndValue.Pointer = false;
                    const bool sinkConsumes = aliasReturnSlot
                        ? compiler->BorrowedOwningStructTempQualifies(receiverArg)
                        : compiler->OwningSinkConsumesConcrete(param);
                    if (!param.Pointer && sinkConsumes
                        && compiler->IsOwningValueType(param.TypeName))
                        receiverSinkConsumes = true;
                }
        if (!receiverFound)
            return TryUnaryFreeOperatorOverload(operand, op, ctx, freeOperandStorage,
                                                constOperand || constPrvalue,
                                                operandSource != nullptr && operandSource->IsExplicitMove,
                                                constPrvalue, volatileOperand, volatileLvalueOperand, lvalueRefResult);
        // The viable members that beat every free candidate; the member call is dispatched over these only.
        std::vector<std::string> cxxPinnedMembers;
        if (compiler->IsCxxRecord(typeName))
        {
            compiler->TryBindCxxFunction(cxxFreeSourceName);
            bool operandIsRvalue = operandSource != nullptr && operandSource->IsRvalue;
            if (auto* unaryCtx = dynamic_cast<CFlatParser::UnaryExpressionContext*>(ctx);
                unaryCtx != nullptr && unaryCtx->castExpression() != nullptr)
            {
                const int lvalueKind = CxxArgumentLvalueKind(unaryCtx->castExpression());
                // A call (kind 2) is an rvalue unless it yields a reference (an lvalue): a const or a `T&` result.
                if (lvalueKind >= 0)
                    operandIsRvalue = lvalueKind == 2 ? (operandIsRvalue && !constOperand && !lvalueRefResult)
                                                      : lvalueKind != 1;
            }
            constOperand = constOperand || constPrvalue;
            const int receiverCv = (constOperand ? 1 : 0) | (volatileOperand ? 2 : 0);
            LLVMBackend::NamedVariable objectArg;
            objectArg.Primary = operand;
            objectArg.BaseType = structTy;
            objectArg.TypeAndValue.TypeName = typeName;
            objectArg.TypeAndValue.SetCxxConstRef(constOperand);
            objectArg.TypeAndValue.IsCxxPointeeVolatile = volatileOperand;
            objectArg.IsRvalue = operandIsRvalue;
            objectArg.CxxLvalueKind = operandIsRvalue ? 0 : 1;
            /*
             * Every zero-argument member of the class, the const twin of a pair included. Each is
             * judged by the one implicit-object rule (CxxImplicitObjectViable); an inherited member
             * binds through a derived-to-base conversion, so any viable exact free outranks it.
             */
            const std::string constTwinName = LLVMBackend::CxxConstTwinName(opName);
            bool memberNamed = false;
            bool membersInherited = false;
            std::vector<const LLVMBackend::FunctionSymbol*> zeroArgMembers;
            std::set<std::string> seenMembers;
            for (const std::string& tableName : { opName, constTwinName })
                if (const auto functions = compiler->functionTable.find(tableName);
                    functions != compiler->functionTable.end())
                    for (const auto& candidate : functions->second)
                    {
                        if (!candidate.IsMethod || candidate.Parameters.size() != 1
                            || candidate.Parameters[0].TypeName != typeName) continue;
                        memberNamed = true;
                        if (!seenMembers.insert(candidate.UniqueName).second
                            || !compiler->CxxImplicitObjectViable(receiverCv, operandIsRvalue, candidate))
                            continue;
                        zeroArgMembers.push_back(&candidate);
                        membersInherited = membersInherited
                            || (!candidate.CxxInheritedOwner.empty()
                                && candidate.CxxInheritedOwner != typeName);
                    }
            const bool memberReachable = !zeroArgMembers.empty();
            std::vector<const LLVMBackend::FunctionSymbol*> constFreeReferences;
            // By-value free overloads: an identity conversion, indistinguishable from any
            // viable member's implicit object binding (clang: ambiguous).
            std::set<const LLVMBackend::FunctionSymbol*> byValueFrees;
            // Rvalue-reference free overloads with their cv bits (const = 1, volatile = 2).
            std::map<const LLVMBackend::FunctionSymbol*, int> rvalueRefFrees;
            if (const auto functions = compiler->functionTable.find(opName);
                functions != compiler->functionTable.end())
                for (const auto& candidate : functions->second)
                {
                    if (candidate.IsMethod || !candidate.IsCxx || candidate.Parameters.size() != 1
                        || candidate.Parameters[0].TypeName != typeName) continue;
                    if (auto signatures = compiler->cxxFunctionSignatures_.find(cxxFreeSourceName);
                        signatures != compiler->cxxFunctionSignatures_.end())
                        for (const auto& raw : signatures->second)
                            if (raw.linkageName == candidate.UniqueName && !raw.paramSpellings.empty())
                            {
                                // Only a free overload this operand can bind ties: `S&` needs a
                                // mutable lvalue, `S&&` a mutable rvalue; `const S&` binds any.
                                const auto& spelling = raw.paramSpellings[0];
                                const bool rvalueRef = spelling.find("&&") != std::string::npos;
                                const bool lvalueRef = !rvalueRef && spelling.find('&') != std::string::npos;
                                const bool constSpelled = spelling.find("const") != std::string::npos;
                                const bool volatileSpelled = spelling.find("volatile") != std::string::npos;
                                bool bindable = true;
                                if (lvalueRef && !constSpelled) bindable = !operandIsRvalue && !constOperand;
                                else if (rvalueRef && !constSpelled) bindable = operandIsRvalue && !constOperand;
                                else if (rvalueRef) bindable = operandIsRvalue;
                                // A volatile operand binds only a volatile reference.
                                if (volatileOperand && (lvalueRef || rvalueRef) && !volatileSpelled)
                                    bindable = false;
                                // clang: a volatile prvalue binds no plain reference, a volatile lvalue no `&&`.
                                if (volatileOperand && lvalueRef && !volatileLvalueOperand)
                                    bindable = false;
                                if (volatileLvalueOperand && rvalueRef)
                                    bindable = false;
                                if (!bindable) continue;
                                constFreeReferences.push_back(&candidate);
                                if (!lvalueRef && !rvalueRef) byValueFrees.insert(&candidate);
                                if (rvalueRef)
                                    rvalueRefFrees[&candidate] = (constSpelled ? 1 : 0)
                                        | (volatileSpelled ? 2 : 0);
                            }
                }
            // An inherited member's derived-to-base binding loses to any viable exact free.
            if (membersInherited && !constFreeReferences.empty())
                return TryUnaryFreeOperatorOverload(operand, op, ctx, freeOperandStorage,
                                                    constOperand,
                                                    operandSource != nullptr && operandSource->IsExplicitMove,
                                                    constPrvalue, volatileOperand, volatileLvalueOperand, lvalueRefResult);
            std::vector<const LLVMBackend::FunctionSymbol*> tiedFreeCandidates;
            for (const auto* member : zeroArgMembers)
            {
                auto memberRanks = compiler->RankCxxConversionSequences({ objectArg }, *member);
                if (memberRanks.size() != 1 || memberRanks[0].rank != -2) continue;
                for (const auto* freeCandidate : constFreeReferences)
                {
                    auto freeRanks = compiler->RankCxxConversionSequences({ objectArg }, *freeCandidate);
                    if (freeRanks.size() != 1 || freeRanks[0].rank < 0) continue;
                    bool crossing = false;
                    if (byValueFrees.count(freeCandidate) != 0
                        || (compiler->CompareCxxConversionRanks(memberRanks, freeRanks, crossing) == 0
                            && !crossing))
                        tiedFreeCandidates.push_back(freeCandidate);
                }
            }
            bool ambiguousMemberFreeTie = false;
            for (const auto* tiedFree : tiedFreeCandidates)
            {
                bool memberBeatsFree = false;
                auto freeRanks = compiler->RankCxxConversionSequences({ objectArg }, *tiedFree);
                if (freeRanks.size() != 1 || freeRanks[0].rank < 0) continue;
                for (const auto* member : zeroArgMembers)
                {
                    auto memberRanks = compiler->RankCxxConversionSequences({ objectArg }, *member);
                    if (memberRanks.size() != 1 || memberRanks[0].rank != -2) continue;
                    bool crossing = false;
                    if (compiler->CompareCxxConversionRanks(memberRanks, freeRanks, crossing) == -1
                        && !crossing)
                        memberBeatsFree = true;
                }
                if (!memberBeatsFree) ambiguousMemberFreeTie = true;
            }
            if (ambiguousMemberFreeTie)
            {
                LogErrorContext(ctx, std::format(
                    "ambiguous C++ unary operator '{}': a member and a free overload are both viable and rank equally",
                    opName));
                return nullptr;
            }
            // No viable member: TryUnaryFreeOperatorOverload decides. A bindable free that strictly
            // outranks every viable member (clang: `const&` beats `const volatile`) is the pick.
            bool freeOutranksMembers = false;
            for (const auto* freeCandidate : constFreeReferences)
            {
                if (zeroArgMembers.empty()) break;
                auto freeRanks = compiler->RankCxxConversionSequences({ objectArg }, *freeCandidate);
                if (freeRanks.size() != 1 || freeRanks[0].rank < 0) continue;
                bool outranksAll = true;
                for (const auto* member : zeroArgMembers)
                {
                    auto memberRanks = compiler->RankCxxConversionSequences({ objectArg }, *member);
                    bool crossing = false;
                    if (memberRanks.size() != 1 || memberRanks[0].rank != -2
                        || compiler->CompareCxxConversionRanks(memberRanks, freeRanks, crossing) != 1
                        || crossing)
                        outranksAll = false;
                }
                freeOutranksMembers = freeOutranksMembers || outranksAll;
            }
            // An `S&&` free is unranked against a class object: compare by cv alone (a non-ref-qualified
            // member is exempt from [over.ics.rank] 3.2.3, leaving 3.2.6).
            bool rvalueRefTie = false;
            for (const auto& [freeCandidate, freeCvBits] : rvalueRefFrees)
            {
                const int freeCv = freeCvBits;
                bool tied = false;
                bool beaten = false;
                bool outranksAll = !zeroArgMembers.empty();
                for (const auto* member : zeroArgMembers)
                {
                    auto memberRanks = compiler->RankCxxConversionSequences({ objectArg }, *member);
                    if (memberRanks.size() != 1 || memberRanks[0].rank != -2) { outranksAll = false; continue; }
                    // 3.2.3 applies to an lvalue-ref-qualified member: the rvalue-ref free binding wins.
                    if (member->CxxRefQualifier == cflat_cinterop::CxxRefQualifierLValue) continue;
                    const int memberCv = memberRanks[0].second;
                    if (memberCv == freeCv) tied = true;
                    else if ((memberCv & freeCv) == memberCv) beaten = true;
                    if (!((freeCv & memberCv) == freeCv && freeCv != memberCv)) outranksAll = false;
                }
                if (tied && !beaten) rvalueRefTie = true;
                freeOutranksMembers = freeOutranksMembers || outranksAll;
            }
            if (rvalueRefTie)
            {
                LogErrorContext(ctx, std::format(
                    "ambiguous C++ unary operator '{}': a member and a free overload are both viable and rank equally",
                    opName));
                return nullptr;
            }
            if (memberNamed && (!memberReachable || freeOutranksMembers))
                return TryUnaryFreeOperatorOverload(operand, op, ctx, freeOperandStorage,
                                                    constOperand,
                                                    operandSource != nullptr && operandSource->IsExplicitMove,
                                                    constPrvalue, volatileOperand, volatileLvalueOperand, lvalueRefResult);
            // Member wins over every free: the member call must not re-rank the free candidates by name.
            for (const auto* member : zeroArgMembers)
                cxxPinnedMembers.push_back(member->UniqueName);
        }

        // Determine whether to pass the operand by pointer or by value.
        bool usePointer = false;
        {
            auto funcSym = compiler->functionTable.find(opName);
            if (funcSym != compiler->functionTable.end())
            {
                for (const auto& candidate : funcSym->second)
                {
                    if (!candidate.Parameters.empty()
                        && candidate.Parameters[0].TypeName == typeName
                        && isMemberReceiver(candidate)
                        && candidate.Parameters[0].Pointer)
                    {
                        usePointer = true;
                        break;
                    }
                }
            }
        }

        // Only the BY-POINTER receiver leaks: the by-value receiver is already registered by the
        // call-argument lowering, and registering it here too would destruct the temp twice.
        bool receiverArmsAlreadyRegistered = inCallArgument_ && llvm::isa<llvm::PHINode>(operand);
        bool receiverTempAlreadyRegistered = receiverArmsAlreadyRegistered;
        if (usePointer && !receiverConsumes && !receiverArmsAlreadyRegistered)
        {
            LLVMBackend::NamedVariable receiverNV;
            receiverNV.Primary = operand;
            receiverNV.BaseType = structTy;
            receiverNV.TypeAndValue.TypeName = typeName;
            compiler->RegisterBorrowedOwningStructTemp(receiverNV, true);
            receiverTempAlreadyRegistered = true;
        }

        if (usePointer)
        {
            const bool cxxClass = compiler->IsCxxRecord(typeName)
                || compiler->generatedCxxRecords_.count(typeName) != 0;
            llvm::Value* tempAlloca = operandStorage != nullptr ? operandStorage : nullptr;
            // A read-only object whose class has a const twin of the operator calls that const
            // overload on the object itself (clang's pick); CreateOverloadedFunctionCall redirects.
                if (tempAlloca == nullptr && readOnlyStorage != nullptr && cxxClass)
                if (auto twin = compiler->functionTable.find(LLVMBackend::CxxConstTwinName(opName));
                    twin != compiler->functionTable.end()
                    && std::any_of(twin->second.begin(), twin->second.end(),
                           [&](const LLVMBackend::FunctionSymbol& c) {
                               return c.IsMethod && !c.Parameters.empty()
                                   && c.Parameters[0].TypeName == typeName;
                           }))
                    operandStorage = tempAlloca = readOnlyStorage;
            if (tempAlloca == nullptr)
            {
                tempAlloca = compiler->CreateAlloca(structTy);
                compiler->CreateAssignment(operand, tempAlloca);
            }

            LLVMBackend::NamedVariable thisNV;
            thisNV.TypeAndValue.TypeName = typeName;
            thisNV.TypeAndValue.Pointer  = true;
            thisNV.TypeAndValue.IsCxxMutableField = mutableFieldReceiver;
            thisNV.TypeAndValue.SetPointeeConstBit(constOperand);
            thisNV.TypeAndValue.IsCxxPointeeVolatile = volatileOperand;
            thisNV.IsCxxUnaryOperatorReceiver = true;
            thisNV.CxxPinnedMemberNames = cxxPinnedMembers;
            if (operandSource != nullptr && operandSource->CallerName == "this")
            {
                thisNV.CallerName = operandSource->CallerName;
                thisNV.FieldName = operandSource->FieldName;
            }
            thisNV.Primary = tempAlloca;
            // An lvalue receiver lets C++ pick the non-const overload.
            if (tempAlloca == operandStorage) thisNV.Storage = operandStorage;
            if (auto* unaryCtx = dynamic_cast<CFlatParser::UnaryExpressionContext*>(ctx);
                unaryCtx != nullptr && unaryCtx->castExpression() != nullptr)
            {
                thisNV.CxxLvalueKind = CxxArgumentLvalueKind(unaryCtx->castExpression());
                // A call result (kind 2) is an rvalue unless it yields a const reference.
                if (thisNV.CxxLvalueKind == 2)
                    thisNV.IsRvalue = operandSource != nullptr && operandSource->IsRvalue && !constOperand && !lvalueRefResult;
                else
                    thisNV.IsRvalue = thisNV.CxxLvalueKind >= 0
                        ? thisNV.CxxLvalueKind != 1
                        : (operandSource != nullptr && operandSource->IsRvalue);
            }
            thisNV.CxxLvalueReceiver = (tempAlloca == operandStorage && operandStorage != nullptr
                && thisNV.CxxLvalueKind == 1)
                || (lvalueRefResult && thisNV.CxxLvalueKind == 2);
            thisNV.TernaryTempAlreadyRegistered = receiverTempAlreadyRegistered;
            thisNV.IsExplicitMove = receiverIsFreeParam && operandExplicitMove;

            FlushCallPostfix();
            return compiler->CreateOverloadedFunctionCall(opName, { thisNV });
        }
        else
        {
            LLVMBackend::NamedVariable thisNV;
            thisNV.TypeAndValue.TypeName = typeName;
            thisNV.TypeAndValue.Pointer  = false;
            thisNV.TypeAndValue.IsCxxMutableField = mutableFieldReceiver;
            thisNV.TypeAndValue.SetCxxConstRef(constOperand);
            thisNV.TypeAndValue.IsCxxPointeeVolatile = volatileOperand;
            thisNV.Primary  = operand;
            thisNV.BaseType = structTy;
            thisNV.IsCxxUnaryOperatorReceiver = true;
            thisNV.CxxPinnedMemberNames = cxxPinnedMembers;
            thisNV.TernaryTempAlreadyRegistered = receiverTempAlreadyRegistered;
            thisNV.IsExplicitMove = receiverIsFreeParam && operandExplicitMove;
            // The receiver's value category decides which C++ overload binds (`S&` vs `S&&`).
            if (auto* unaryCtx = dynamic_cast<CFlatParser::UnaryExpressionContext*>(ctx);
                unaryCtx != nullptr && unaryCtx->castExpression() != nullptr)
            {
                thisNV.CxxLvalueKind = CxxArgumentLvalueKind(unaryCtx->castExpression());
                if (thisNV.CxxLvalueKind == 2)
                    thisNV.IsRvalue = operandSource != nullptr && operandSource->IsRvalue && !constOperand && !lvalueRefResult;
                else if (thisNV.CxxLvalueKind >= 0)
                    thisNV.IsRvalue = thisNV.CxxLvalueKind != 1;
            }
            if (operandSource != nullptr)
            {
                thisNV.CallerName = !operandSource->FieldName.empty()
                    ? operandSource->TypeAndValue.ParentVariableName
                    : operandSource->CallerName;
                thisNV.FieldName = operandSource->FieldName;
                thisNV.MovePathKey = operandSource->MovePathKey;
                thisNV.MovePathHasDynamicIndex = operandSource->MovePathHasDynamicIndex;
                thisNV.OwningStructName = operandSource->OwningStructName;
                thisNV.IsElementAccess = operandSource->IsElementAccess;
                thisNV.ConditionalMoveSources = operandSource->ConditionalMoveSources;
                thisNV.IsOwning = operandSource->IsOwning || operandSource->IsOwningStruct;
            }
            std::string consumedSource;
            if (receiverSinkConsumes && operandStorage != nullptr
                && !llvm::isa<llvm::PHINode>(operand) && !compiler->IsCxxRecord(typeName))
            {
                if (thisNV.FieldName.empty())
                    consumedSource = compiler->FindVariableNameByStorage(operandStorage);
                if (consumedSource.empty()) consumedSource = thisNV.CallerName;
                if (!consumedSource.empty())
                {
                    thisNV.Storage = operandStorage;
                    thisNV.CallerName = consumedSource;
                    thisNV.DeferMovedMark = true;
                    thisNV.ForceOperatorConsume = receiverSinkConsumes;
                    if (const auto* source = compiler->FindVariableByStorage(operandStorage))
                        thisNV.IsOwning = source->IsOwning || source->IsOwningStruct;
                }
            }
            FlushCallPostfix();
            llvm::Value* result = compiler->CreateOverloadedFunctionCall(opName, { thisNV });
            // Keep call lowering from retiring the source while it is still constructing the
            // operator argument; retire it as soon as the operator call has been emitted.
            if (!consumedSource.empty())
            {
                if (!thisNV.FieldName.empty())
                    compiler->MarkVariableFieldMoved(consumedSource, LLVMBackend::FieldMoveKey(thisNV));
                else if (!thisNV.IsElementAccess)
                    compiler->MarkVariableMoved(consumedSource);
            }
            return result;
        }
    }

// Unary `- + ! ~` on an imported C++ class whose operator is a FREE function (or a free function
// template) in the class's namespace: `W operator-(const W&)`. Mirrors the binary free-operator
// lookup: bind the namespace's operator lazily, then call the one-parameter candidate. Anything
// that is not a C++ class, or has no such candidate, answers nullptr and keeps the old fallbacks.
llvm::Value* MainListener::TryUnaryFreeOperatorOverload(
        llvm::Value* operand, const std::string& op,
        antlr4::ParserRuleContext* ctx, llvm::Value* operandStorage, bool operandIsConst, bool operandExplicitMove,
        bool operandIsConstPrvalue, bool operandIsVolatile, bool operandIsVolatileLvalue,
        bool operandIsLvalueRefResult) {
        auto* compiler = Compiler(ctx);
        if (operand == nullptr || !operand->getType()->isStructTy()) return nullptr;
        auto* structTy = llvm::cast<llvm::StructType>(operand->getType());
        if (structTy->isLiteral() || !structTy->hasName()) return nullptr;
        const std::string typeName = structTy->getName().str();
        if (!compiler->IsCxxRecord(typeName)
            && compiler->generatedCxxRecords_.count(typeName) == 0)
            return nullptr;

        const std::string opName = "operator" + op;
        // Template arguments follow the first '$' and carry their own dots; the namespace
        // comes from the base name.
        const std::string base = typeName.substr(0, typeName.find('$'));
        const size_t dot = base.rfind('.');
        const std::string ns = dot == std::string::npos ? std::string() : base.substr(0, dot);
        const std::string sourceName = ns.empty() ? opName : ns + "." + opName;
        compiler->TryBindCxxFunction(sourceName);
        const std::string lookupName = ns.empty() ? "__cxx_free." + opName : sourceName;

        // The operand's own storage when it has one; a copy in a temporary otherwise.
        bool operandIsRvalue = operandStorage == nullptr;
        if (auto* unaryCtx = dynamic_cast<CFlatParser::UnaryExpressionContext*>(ctx);
            unaryCtx != nullptr && unaryCtx->castExpression() != nullptr)
        {
            const int8_t category = CxxArgumentLvalueKind(unaryCtx->castExpression());
            // A `T&` call result is an lvalue (clang); the call category alone reads it as an rvalue.
            if (category >= 0)
                operandIsRvalue = category != 1 && !(category == 2 && operandIsLvalueRefResult);
        }
        if (operandIsConst && !operandIsConstPrvalue)
            operandIsRvalue = false;
        // The address of a `T&` call result is the object its load reads (the call returns it).
        llvm::Value* lvalueSource = operandStorage;
        if (lvalueSource == nullptr && operandIsLvalueRefResult)
            if (auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(operand))
                lvalueSource = load->getPointerOperand();
        auto storageFor = [&]() -> llvm::Value* {
            if (lvalueSource != nullptr) return lvalueSource;
            auto* temp = compiler->CreateAlloca(structTy);
            compiler->CreateAssignment(operand, temp);
            return temp;
        };

        bool candidateFound = false;
        bool reference = false;
        bool mutableReference = false;
        bool bindsRvalue = false;
        bool byValueCandidate = false;
        bool constLvalueReferenceCandidate = false;
        std::set<std::string> mutableReferenceLinkages;
        // A cv-qualified operand: the free overloads clang would refuse for the cv mismatch are dropped.
        std::set<std::string> cvRejectedLinkages;
        bool cvViable = false;
        // By-value copy of a volatile LVALUE: clang's copy-initialization decides. Only a refusal
        // with no viable constructor drops the candidate; other refusals stay for the call.
        llvm::Value* volatileCopySource = lvalueSource;
        std::optional<bool> volatileCopyViable;
        auto volatileLvalueCopyViable = [&]() -> bool {
            if (volatileCopyViable.has_value()) return *volatileCopyViable;
            LLVMBackend::NamedVariable probe;
            probe.Primary = operand;
            probe.Storage = volatileCopySource;
            probe.BaseType = structTy;
            probe.TypeAndValue.TypeName = typeName;
            probe.TypeAndValue.IsCxxPointeeVolatile = true;
            probe.TypeAndValue.SetCxxConstRef(operandIsConst);
            probe.IsRvalue = false;
            std::string wrapperName;
            std::string wrapperError;
            bool noViableCandidate = false;
            const bool hasViableCandidate = compiler->RequestCxxVariadicConstructor(typeName,
                { probe }, wrapperName, wrapperError, /*copyInit*/ true, &noViableCandidate)
                || !noViableCandidate;
            volatileCopyViable = hasViableCandidate;
            return hasViableCandidate;
        };
        if (auto fit = compiler->functionTable.find(lookupName);
            fit != compiler->functionTable.end())
        {
            for (const auto& candidate : fit->second)
            {
                if (candidate.IsMethod || candidate.Parameters.size() != 1
                    || candidate.SourceName != sourceName
                    || candidate.Parameters[0].TypeName != typeName) continue;
                candidateFound = true;
                bool rawReference = false;
                bool rawMutable = false;
                bool rawConstReference = false;
                bool rawVolatile = false;
                bool rawRvalueRef = false;
                bool rawConst = false;
                bool rawSeen = false;
                if (auto rawIt = compiler->cxxFunctionSignatures_.find(candidate.SourceName);
                    rawIt != compiler->cxxFunctionSignatures_.end())
                    for (const auto& raw : rawIt->second)
                        if (raw.linkageName == candidate.UniqueName && !raw.paramSpellings.empty())
                        {
                            const auto& spelling = raw.paramSpellings[0];
                            rawSeen = true;
                            rawReference = spelling.find('&') != std::string::npos;
                            rawVolatile = spelling.find("volatile") != std::string::npos;
                            rawRvalueRef = spelling.find("&&") != std::string::npos;
                            rawConst = spelling.find("const") != std::string::npos;
                            rawMutable = rawReference && !rawRvalueRef && !rawConst;
                            rawConstReference = rawReference && !rawRvalueRef && rawConst;
                        }
                if (operandIsVolatile || operandIsConst)
                {
                    // clang: a volatile operand binds only a volatile reference; a by-value copy of
                    // a volatile lvalue needs a constructor that accepts it (a prvalue elides).
                    const bool byValueParam = !rawReference && !candidate.Parameters[0].Pointer;
                    // clang: a volatile lvalue binds only a volatile `&`; a volatile prvalue binds
                    // only a volatile `&&` (`const volatile S&` refuses it), never a plain reference.
                    const bool volatileRefBinds = rawVolatile
                        && (operandIsVolatileLvalue ? !rawRvalueRef : rawRvalueRef);
                    const bool volatileRefused = operandIsVolatile
                        && ((rawSeen && rawReference && !volatileRefBinds)
                            || (byValueParam && operandIsVolatileLvalue && volatileCopySource != nullptr
                                && !volatileLvalueCopyViable()));
                    // clang: a const operand never binds a non-const `T&&` (the cv would be dropped).
                    const bool constRvalueRefused = operandIsConst && rawSeen && rawRvalueRef && !rawConst;
                    if (volatileRefused || constRvalueRefused)
                    {
                        cvRejectedLinkages.insert(candidate.UniqueName);
                        continue;
                    }
                    cvViable = true;
                }
                reference = reference || rawReference || candidate.Parameters[0].Pointer;
                byValueCandidate = byValueCandidate || (rawSeen
                    ? !rawReference : !candidate.Parameters[0].Pointer);
                constLvalueReferenceCandidate = constLvalueReferenceCandidate || (rawSeen
                    ? rawConstReference
                    : candidate.Parameters[0].CxxConstRef()
                        && !candidate.Parameters[0].IsRvalueRef);
                // No recorded spelling: the mapped alias flags decide, as in the binary path.
                if (!rawSeen)
                    rawMutable = candidate.IsCxx && candidate.Parameters[0].IsAlias
                        && !candidate.Parameters[0].CxxConstRef()
                        && !candidate.Parameters[0].IsRvalueRef
                        && !candidate.Parameters[0].IsCxxRefToPointer;
                if (rawMutable)
                {
                    mutableReference = true;
                    mutableReferenceLinkages.insert(candidate.UniqueName);
                }
                else bindsRvalue = true;
            }
        }

        if (byValueCandidate && constLvalueReferenceCandidate)
            LogErrorContext(ctx, std::format(
                "ambiguous C++ unary operator '{}': by-value and const-reference overloads are equally viable",
                opName));
        // A refused non-template free must not win the call: the function TEMPLATE decides with
        // those frees excluded from the overload set (clang: `const T&` deduces the cv it needs).
        std::set<std::string> excludedLinkages = cvRejectedLinkages;
        if (operandIsConst || operandIsRvalue)
            excludedLinkages.insert(mutableReferenceLinkages.begin(), mutableReferenceLinkages.end());
        // The namespace may still declare the operator as a function TEMPLATE over the operand's
        // class template: instantiate one for THIS operand through the same generated-wrapper path
        // an ordinary C++ template call uses. nullptr = no template binds (nothing was emitted).
        auto tryFunctionTemplate = [&]() -> llvm::Value* {
            if (!compiler->HasCxxFunctionTemplate(sourceName)) return nullptr;
            LLVMBackend::NamedVariable arg;
            arg.Primary = operand;
            arg.Storage = storageFor();
            arg.BaseType = operand->getType();
            arg.TypeAndValue.TypeName = typeName;
            arg.TypeAndValue.SetCxxConstRef(operandIsConst);
            arg.TypeAndValue.IsCxxPointeeVolatile = operandIsVolatile;
            arg.IsRvalue = operandIsRvalue;
            arg.IsExplicitMove = operandExplicitMove;
            if (auto* unaryCtx = dynamic_cast<CFlatParser::UnaryExpressionContext*>(ctx);
                unaryCtx != nullptr && unaryCtx->castExpression() != nullptr)
                arg.CxxLvalueKind = CxxArgumentLvalueKind(unaryCtx->castExpression());
            if (ctx != nullptr)
                for (auto* child : ctx->children)
                    if (auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(child))
                        arg.CxxLvalueKind = CxxArgumentLvalueKind(rule);
            std::vector<LLVMBackend::NamedVariable> templateArgs{ arg };
            std::string registeredName;
            std::string templateError;
            if (!compiler->RequestCxxFunctionTemplate(sourceName, {}, {}, templateArgs, {},
                                                     registeredName, templateError)
                || registeredName.empty())
                return nullptr;
            FlushCallPostfix();
            llvm::Value* result = CallOperatorWithoutExcluded(compiler->functionTable, compiler, lookupName, registeredName, templateArgs, excludedLinkages);
            if (result != nullptr) TrackOwnedStringOperatorResult(compiler, result);
            return result;
        };

        // Every non-template free was cv-refused: the template is the only candidate left.
        if ((operandIsVolatile || operandIsConst) && candidateFound && !cvViable)
        {
            if (llvm::Value* viaTemplate = tryFunctionTemplate()) return viaTemplate;
            LogErrorContext(ctx, std::format(
                "C++ '{}' has no viable overload: the cv-qualified operand binds no free overload",
                opName));
        }

        if (!candidateFound) return tryFunctionTemplate();

        if (mutableReference && !bindsRvalue && (operandIsConst || operandIsRvalue))
        {
            // A const or rvalue operand cannot bind the mutable reference: a template may still bind.
            if (llvm::Value* viaTemplate = tryFunctionTemplate()) return viaTemplate;
            const std::string reason = operandIsConst ? "the operand is const"
                                                       : "the operand is an rvalue";
            LogErrorContext(ctx, std::format(
                "C++ '{}' has no viable overload: '{}' was rejected because {} and requires a non-const lvalue reference",
                opName, opName + "(" + typeName + "&)", reason));
        }

        LLVMBackend::NamedVariable arg;
        arg.Primary = operand;
        arg.Storage = operandStorage;
        arg.BaseType = operand->getType();
        arg.TypeAndValue.TypeName = typeName;
        arg.TypeAndValue.Pointer = reference;
        arg.TypeAndValue.SetCxxConstRef(operandIsConst);
        arg.IsRvalue = operandIsRvalue;
        arg.IsExplicitMove = operandExplicitMove;
        if (auto* unaryCtx = dynamic_cast<CFlatParser::UnaryExpressionContext*>(ctx);
            unaryCtx != nullptr && unaryCtx->castExpression() != nullptr)
            arg.CxxLvalueKind = CxxArgumentLvalueKind(unaryCtx->castExpression());
        if (reference)
        {
            // The argument IS the address for a reference parameter.
            arg.Primary = storageFor();
            arg.Storage = arg.Primary;
            arg.BaseType = structTy;
        }
        FlushCallPostfix();
        llvm::Value* result = nullptr;
        if (excludedLinkages.empty() || compiler->functionTable.count(lookupName) != 0)
            result = CallOperatorWithoutExcluded(compiler->functionTable, compiler, lookupName, lookupName, { arg }, excludedLinkages);
        if (result != nullptr) TrackOwnedStringOperatorResult(compiler, result);
        return result;
    }

// Built-in unary `- + ~ !` on an imported C++ class reaches its operand through ONE implicit
// conversion function to an arithmetic type, exactly like C++: the operand becomes that value
// (integral promotion applied for `- + ~`) and the caller continues on the scalar. Runs after
// operator lookup, so a real member / free operator always wins; explicit conversion functions
// never take part. Returns 0 = not applicable, 1 = converted, 2 = an error was reported.
int MainListener::ConvertUnaryOperandViaImplicitConversion(
        llvm::Value*& value, LLVMBackend::NamedVariable& namedVar, const std::string& op,
        antlr4::ParserRuleContext* ctx) {
        auto* compiler = Compiler(ctx);
        if (value == nullptr || !value->getType()->isStructTy()) return 0;
        auto* structTy = llvm::cast<llvm::StructType>(value->getType());
        if (structTy->isLiteral() || !structTy->hasName()) return 0;
        const std::string typeName = structTy->getName().str();
        if (!compiler->IsCxxRecord(typeName)
            && compiler->generatedCxxRecords_.count(typeName) == 0)
            return 0;
        compiler->EnsureCxxConversionOperatorsProjected(typeName);
        const auto* info = compiler->GetCxxClassInfo(typeName);
        if (info == nullptr) return 0;
        std::vector<std::string> names;
        std::vector<std::string> projectedNames;
        LLVMBackend::TypeAndValue target;
        bool targetIsReferenceResult = false;
        std::set<std::string> visited;
        auto collect = [&](auto&& self, const std::string& owner) -> void {
            if (!visited.insert(owner).second) return;
            if (auto record = compiler->cxxRecordEntries_.find(owner);
                record != compiler->cxxRecordEntries_.end())
            {
                for (const auto& member : record->second.members)
                {
                    LLVMBackend::TypeAndValue returnType;
                    if (!member.isConversion || member.isExplicit || member.isDeleted
                        || !member.bindRefusal.empty()
                        || !compiler->MapCTypeToTypeAndValue(
                            member.retType, returnType, /*cxxBoundary*/ true))
                        continue;
                    const bool referenceResult = member.retType.find('&') != std::string::npos;
                    const bool scalarResult = returnType.IsPrimitive()
                        && (!returnType.Pointer || (returnType.IsAlias && referenceResult));
                    const bool pointerResult = op == "+" && returnType.Pointer
                        && !returnType.ElemPointer && !returnType.IsFunctionPointer;
                    if (!scalarResult && !pointerResult)
                        continue;
                    std::string spelling = returnType.IsPrimitive() && !returnType.Pointer
                        ? CanonicalPrimitiveTypeName(compiler->ResolveTypeAlias(returnType.TypeName))
                        : SpellType(*compiler, returnType);
                    const std::string projectedName = spelling.empty()
                        ? member.name : "operator " + spelling;
                    if (std::find(names.begin(), names.end(), member.name) == names.end())
                    {
                        names.push_back(member.name);
                        projectedNames.push_back(projectedName);
                        target = returnType;
                        targetIsReferenceResult = referenceResult;
                    }
                }
                for (const auto& base : record->second.bases)
                {
                    const std::string baseName = compiler->ResolveCxxBaseRecordName(base);
                    if (!baseName.empty()) self(self, baseName);
                }
            }
        };
        collect(collect, typeName);
        if (names.empty())
            return 0;
        if (names.size() > 1)
        {
            std::sort(names.begin(), names.end());
            std::string candidates;
            for (const auto& name : names)
                candidates += (candidates.empty() ? std::string() : std::string(" and "))
                    + "'" + name + "'";
            LogErrorContext(ctx, std::format(
                "ambiguous implicit conversion for built-in operator '{}': {} "
                "are viable conversion functions", op, candidates));
            return 2;
        }
        if (target.Pointer && targetIsReferenceResult)
        {
            target.DropCxxRef();
            target.Pointer = false;
            target.IsAlias = false;
        }
        target.VariableName.clear();
        // `~` has no floating-point built-in.
        if (op == "~" && target.IsFloatingPoint() != -1) return 0;
        LLVMBackend::NamedVariable conversionArg;
        conversionArg.Primary = value;
        conversionArg.Storage = compiler->StorageOfLoadedCxxObject(value);
        conversionArg.BaseType = value->getType();
        conversionArg.TypeAndValue.TypeName = typeName;
        std::string conversionName;
        std::set<std::string> visitedConversionOwners;
        auto findConversionFunction = [&](auto&& self, const std::string& owner) -> void {
            if (!visitedConversionOwners.insert(owner).second) return;
            auto record = compiler->cxxRecordEntries_.find(owner);
            if (record == compiler->cxxRecordEntries_.end()) return;
            for (const auto& member : record->second.members)
            {
                if (!member.isConversion) continue;
                LLVMBackend::TypeAndValue ret;
                if (!compiler->MapCTypeToTypeAndValue(member.retType, ret, true)) continue;
                const bool isReference = member.retType.find('&') != std::string::npos;
                if (ret.TypeName != target.TypeName || isReference != targetIsReferenceResult
                    || (!isReference && ret.Pointer != target.Pointer)) continue;
                std::string projectedName = member.name;
                std::string spelling = ret.IsPrimitive() && !ret.Pointer
                    ? CanonicalPrimitiveTypeName(compiler->ResolveTypeAlias(ret.TypeName))
                    : SpellType(*compiler, ret);
                if (!spelling.empty()) projectedName = "operator " + spelling;
                for (const std::string& name : { projectedName, member.name })
                    for (const std::string& candidateName : { owner + "." + name, name })
                        if (compiler->GetFunction(candidateName))
                        {
                            conversionName = candidateName;
                            return;
                        }
            }
            for (const auto& base : record->second.bases)
            {
                const std::string baseName = compiler->ResolveCxxBaseRecordName(base);
                if (!baseName.empty()) self(self, baseName);
                if (!conversionName.empty()) return;
            }
        };
        findConversionFunction(findConversionFunction, typeName);
        if (conversionName.empty()) conversionName = projectedNames.front();
        if (!compiler->GetFunction(conversionName))
        {
            const std::string qualifiedName = typeName + "." + conversionName;
            if (!compiler->GetFunction(qualifiedName))
                return 0;
            conversionName = qualifiedName;
        }
        const auto savedReturnType = compiler->lastCallReturnType;
        const bool savedReturnsOwned = compiler->lastCallReturnsOwned;
        const bool savedOwningResult = compiler->lastOwningResult;
        llvm::Value* converted = nullptr;
        if (!target.Pointer && !targetIsReferenceResult)
            converted = compiler->ConvertViaImplicitConversionOperator(value, target);
        if (converted == nullptr)
            converted = compiler->CreateOverloadedFunctionCall(conversionName,
                                                                 { conversionArg }, true);
        const auto conversionReturnType = compiler->lastCallReturnType;
        compiler->lastCallReturnType = savedReturnType;
        compiler->lastCallReturnsOwned = savedReturnsOwned;
        compiler->lastOwningResult = savedOwningResult;
        if (converted == nullptr) return 0;
        if (targetIsReferenceResult && converted->getType()->isPointerTy())
        {
            auto valueType = conversionReturnType;
            valueType.DropCxxRef();
            valueType.Pointer = false;
            valueType.PointerDepth = 0;
            valueType.ElemPointer = false;
            valueType.IsAlias = false;
            valueType.IsRvalueRef = false;
            valueType.IsCxxRefToPointer = false;
            auto* llvmValueType = compiler->GetType(valueType);
            if (llvmValueType == nullptr) return 0;
            converted = compiler->builder->CreateLoad(llvmValueType, converted,
                                                       "cxx.unary.conversion.ref");
        }
        namedVar.TypeAndValue = target;
        namedVar.BaseType = nullptr;
        if (op != "!" && converted->getType()->isIntegerTy())
        {
            // C integral promotion: bool / 8 / 16 bit operands become a signed i32.
            const bool isUnsigned = target.IsUnsignedInteger() != -1;
            if (converted->getType()->isIntegerTy(1))
            {
                converted = compiler->builder->CreateZExt(converted, compiler->builder->getInt32Ty());
                namedVar.TypeAndValue.TypeName = "i32";
            }
            else if (converted->getType()->getIntegerBitWidth() < 32)
            {
                converted = compiler->PromoteToInt(converted, isUnsigned);
                namedVar.TypeAndValue.TypeName = "i32";
            }
        }
        value = converted;
        return 1;
    }

// A first-class struct operand with no operator overload cannot be negated or complemented:
// report it instead of emitting integer IR on a struct (a module verification failure).
bool MainListener::ReportMissingUnaryStructOperator(antlr4::ParserRuleContext* ctx,
                                                    llvm::Value* operand, const std::string& op)
{
        if (operand == nullptr || !operand->getType()->isStructTy()) return false;
        auto* structTy = llvm::cast<llvm::StructType>(operand->getType());
        std::string typeName = structTy->hasName() ? structTy->getName().str() : std::string();
        std::string shown = typeName.empty() ? std::string() : SpellType(*Compiler(ctx),
            LLVMBackend::TypeAndValue{ .TypeName = typeName });
        if (shown.empty()) shown = typeName;
        if (std::string note = Compiler(ctx)->CxxMemberThroughNonPublicBaseNote(typeName, "operator" + op);
            !note.empty())
        {
            LogErrorContext(ctx, std::format("no operator '{}' for type '{}': {}", op, shown, note));
            return true;
        }
        LogErrorContext(ctx, std::format("no operator '{}' for type '{}'", op, shown));
        return true;
    }

void MainListener::TrackOwnedStringOperatorResult(LLVMBackend* compiler, llvm::Value* result) {
        if (result == nullptr || !compiler->lastCallReturnsOwned) return;
        auto* strTy = llvm::StructType::getTypeByName(*compiler->context, "string");
        if (strTy != nullptr && result->getType() == strTy)
            compiler->RegisterOwnedStringTemp(result);
    }

void MainListener::RegisterBorrowedStringOperandTemp(LLVMBackend* compiler, llvm::Value* operand) {
        if (operand == nullptr || !compiler->IsProducedTempValue(operand)) return;
        auto* strTy = llvm::StructType::getTypeByName(*compiler->context, "string");
        if (strTy != nullptr && operand->getType() == strTy)
            compiler->RegisterOwnedStringTemp(operand);
    }

bool MainListener::IsComparisonOperatorText(const std::string& op) {
        return op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=";
    }

llvm::Value* MainListener::TryPointerLhsOperatorOverload(
        llvm::Value* lvalue, const std::string& op, llvm::Value* rvalue,
        antlr4::ParserRuleContext* ctx, llvm::Type* lhsElemType) {
        auto* compiler = Compiler(ctx);

        // LOAD-BEARING: the RHS must NOT be pointer-typed. Both-pointer operands (including
        // `p == nullptr`) always stay on the builtin address comparison and never dispatch.
        if (!rvalue || rvalue->getType()->isPointerTy()) return nullptr;
        if (!IsComparisonOperatorText(op)) return nullptr;

        std::string opName = "operator" + op;
        std::string pointee = ConcreteStructNameFromElemType(lhsElemType, compiler);
        if (!pointee.empty()) compiler->EnsureCxxMemberProjected(pointee, opName);

        if (!pointee.empty() && compiler->GetFunction(opName))
        {
            auto funcSym = compiler->functionTable.find(opName);
            if (funcSym != compiler->functionTable.end())
            {
                for (const auto& candidate : funcSym->second)
                {
                    if (candidate.Parameters.empty()) continue;
                    if (candidate.Parameters[0].TypeName != pointee || !candidate.Parameters[0].Pointer) continue;

                    LLVMBackend::NamedVariable thisNV;
                    thisNV.TypeAndValue.TypeName = pointee;
                    thisNV.TypeAndValue.Pointer  = true;
                    thisNV.Primary  = lvalue;
                    thisNV.BaseType = lhsElemType;

                    LLVMBackend::NamedVariable rightNV;
                    rightNV.Primary  = rvalue;
                    rightNV.BaseType = rvalue->getType();
                    if (auto* rst = llvm::dyn_cast<llvm::StructType>(rvalue->getType()))
                        if (!rst->isLiteral() && rst->hasName())
                            rightNV.TypeAndValue.TypeName = rst->getName().str();

                    // The operator expression is the site key codegen's choice is recorded under.
                    compiler->pendingCallSite_ = ctx;
                    auto* result = compiler->CreateOverloadedFunctionCall(opName, { thisNV, rightNV });
                    compiler->pendingCallSite_ = nullptr;
                    TrackOwnedStringOperatorResult(compiler, result);
                    return result;
                }
            }
        }

        // A struct-valued RHS can never reach a valid builtin comparison against a pointer,
        // so name the failure here instead of emitting a malformed icmp.
        if (rvalue->getType()->isStructTy())
        {
            std::string lhsName = pointee.empty() ? "pointer" : pointee + "*";
            auto* rst = llvm::cast<llvm::StructType>(rvalue->getType());
            std::string rhsName = (!rst->isLiteral() && rst->hasName()) ? rst->getName().str() : "struct";
            LogErrorContext(ctx, std::format(
                "no overload of '{}' matches operands '{}' and '{}'; overloaded operators dispatch on a struct value "
                "or struct pointer left operand with a matching declared overload", opName, lhsName, rhsName));
            return nullptr;
        }

        return nullptr;
    }

llvm::Value* MainListener::TryBinaryOperatorOverload(
        llvm::Value* lvalue, const std::string& op, llvm::Value* rvalue,
        antlr4::ParserRuleContext* ctx, llvm::Type* lhsElemType,
        int rhsPointerDepth, bool rhsElemPointer, llvm::Value* lhsStorage,
        llvm::Value* rhsStorage, bool reportMissing, bool allowReversed,
        bool lhsIsRvalue, bool rhsIsRvalue,
        const std::string& lhsTypeName, const std::string& rhsTypeName,
        int lhsPointerDepth, bool lhsElemPointer,
        const std::string& lhsCallerName, const std::string& lhsFieldName,
        bool lhsIsElementAccess, const std::string& rhsCallerName,
        const std::string& rhsFieldName, bool rhsIsElementAccess,
        const std::vector<LLVMBackend::ConditionalMoveSource>& lhsConditionalMoves,
        const std::vector<LLVMBackend::ConditionalMoveSource>& rhsConditionalMoves,
        bool rhsCxxPointeeConst, const std::string& lhsArithIdentity,
        const std::string& rhsArithIdentity,
        bool rhsExplicitMove, bool lhsExplicitMove,
        bool lhsCxxNullptrT, bool rhsCxxNullptrT, bool lhsMaterializedPrvalue,
        bool lhsCxxPointeeConst, int lhsObjectCv, int rhsObjectCv, bool lhsIsAlias,
        bool refuseConstWriteOnNoViable,
        const LLVMBackend::NamedVariable* lhsSourceNV) {
        auto* compiler = Compiler(ctx);
        if (!lvalue) return nullptr;
        // An ADL request for this expression was ambiguous and refused: nested rewrites stay silent.
        if (ctx != nullptr && ctx == cxxAdlAmbiguousCtx_) return nullptr;
        // The marker covers one outermost resolution only: a generic body re-walks the same ctx.
        struct AdlAmbiguousScope
        {
            const antlr4::ParserRuleContext*& marker;
            bool outermost;
            ~AdlAmbiguousScope() { if (outermost) marker = nullptr; }
        } adlAmbiguousScope{ cxxAdlAmbiguousCtx_, cxxOperatorRewriteDepth_ == 0 };
        struct RewriteDepthScope
        {
            int& depth;
            explicit RewriteDepthScope(int& d) : depth(d) { ++depth; }
            ~RewriteDepthScope() { --depth; }
        };

        // The caller's storage is only usable as the receiver when it describes the SAME object
        // as the value. A PHI ((c ? a : b) == d, a re-materialized temporary) is fed by several
        // slots, so handing one arm's storage to a C++ operator that mutates its receiver would
        // write to the wrong object. Drop it and take the copy path instead.
        auto isCxxRecordValue = [&](llvm::Value* value) {
            if (value == nullptr || !value->getType()->isStructTy()) return false;
            auto* st = llvm::cast<llvm::StructType>(value->getType());
            if (st->isLiteral() || !st->hasName()) return false;
            return compiler->IsCxxRecord(st->getName().str());
        };
        auto isMemberCandidate = [&](const LLVMBackend::FunctionSymbol& candidate) {
            if (candidate.IsMethod) return true;
            if (!candidate.IsCxx) return false;
            auto rawIt = compiler->cxxFunctionSignatures_.find(candidate.SourceName);
            if (rawIt == compiler->cxxFunctionSignatures_.end()) return false;
            return std::any_of(rawIt->second.begin(), rawIt->second.end(), [&](const auto& raw) {
                return raw.isInstanceTemplateWrapper && raw.linkageName == candidate.UniqueName;
            });
        };
        auto isFreeCxxCandidate = [&](const LLVMBackend::FunctionSymbol& candidate) {
            return candidate.IsCxx && !isMemberCandidate(candidate);
        };
        const bool cxxCompoundStrictLookup = std::any_of(
            kCompoundOperators.begin(), kCompoundOperators.end(),
            [&](const auto& compound) { return compound.first == op; })
            && (isCxxRecordValue(lvalue) || isCxxRecordValue(rvalue));
        auto cxxCompoundBinaryFallbackMayBind = [&]() {
            std::string binaryOp;
            for (const auto& [compoundOp, candidate] : kCompoundOperators)
                if (compoundOp == op) { binaryOp = candidate; break; }
            if (binaryOp.empty()) return false;
            if (isCxxRecordValue(lvalue))
            {
                const std::string typeName =
                    llvm::cast<llvm::StructType>(lvalue->getType())->getName().str();
                if (HasOperatorOverloadForFirstParam("operator" + binaryOp, typeName)) return true;
                const std::string base = typeName.substr(0, typeName.find('$'));
                const size_t dot = base.rfind('.');
                const std::string freeName = dot == std::string::npos
                    ? "__cxx_free.operator" + binaryOp
                    : base.substr(0, dot) + ".operator" + binaryOp;
                compiler->TryBindCxxFunction(freeName);
                if (auto it = compiler->functionTable.find(freeName);
                    it != compiler->functionTable.end())
                    for (const auto& candidate : it->second)
                        if (isFreeCxxCandidate(candidate)
                            && candidate.Parameters.size() == 2
                            && (candidate.Parameters[0].TypeName == typeName
                                || compiler->IsCxxBaseOf(candidate.Parameters[0].TypeName, typeName)))
                            return true;
                return compiler->HasCxxFunctionTemplate(freeName)
                    || compiler->CxxClassMayHaveUnregisteredOperators(typeName);
            }
            return isCxxRecordValue(rvalue) && !lvalue->getType()->isStructTy();
        };
        // A storage PHI built BESIDE the value PHI, over the same incoming blocks, is the
        // parallel join: it names whichever arm was selected, so it stays.
        auto isParallelStorageJoin = [](llvm::Value* value, llvm::Value* storage) {
            auto* valuePhi = llvm::dyn_cast_or_null<llvm::PHINode>(value);
            auto* storagePhi = llvm::dyn_cast_or_null<llvm::PHINode>(storage);
            if (valuePhi == nullptr || storagePhi == nullptr) return false;
            if (valuePhi->getParent() != storagePhi->getParent()) return false;
            if (valuePhi->getNumIncomingValues() != storagePhi->getNumIncomingValues())
                return false;
            for (unsigned i = 0; i < valuePhi->getNumIncomingValues(); ++i)
                if (valuePhi->getIncomingBlock(i) != storagePhi->getIncomingBlock(i)) return false;
            return true;
        };
        // Only a C++ record keeps it: native alias joins stay on master's copy path.
        if (llvm::isa<llvm::PHINode>(lvalue)
            && !(isCxxRecordValue(lvalue) && isParallelStorageJoin(lvalue, lhsStorage)))
            lhsStorage = nullptr;
        if (rvalue != nullptr && llvm::isa<llvm::PHINode>(rvalue)
            && !(isCxxRecordValue(rvalue) && isParallelStorageJoin(rvalue, rhsStorage)))
            rhsStorage = nullptr;
        /*
         * An explicit `move` of a non-trivial C++ record keeps its source slot (ParseMoveExpression):
         * the rvalue read straight from it is an xvalue. When a candidate takes that operand as a
         * sink (`T&&` or by value) it binds in place, as `f(move d)` does; a candidate set of
         * only `const T&` keeps the materialized-temp path.
         */
        std::string rhsXvalueType;
        if (rhsIsRvalue && rhsStorage != nullptr && isCxxRecordValue(rvalue))
            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(rvalue);
                load != nullptr && load->getPointerOperand() == rhsStorage)
                rhsXvalueType = llvm::cast<llvm::StructType>(rvalue->getType())->getName().str();
        auto rhsXvalueSink = [&](const LLVMBackend::TypeAndValue& param) {
            return !rhsXvalueType.empty() && param.TypeName == rhsXvalueType
                && (param.IsRvalueRef || (!param.Pointer && !param.IsAlias));
        };
        // T235: a prvalue left operand read from its own sret/ctor slot is a sink like the right one.
        std::string lhsXvalueType;
        if (lhsIsRvalue && lhsStorage != nullptr && isCxxRecordValue(lvalue))
            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(lvalue);
                load != nullptr && load->getPointerOperand() == lhsStorage)
                lhsXvalueType = llvm::cast<llvm::StructType>(lvalue->getType())->getName().str();
        auto lhsXvalueSink = [&](const LLVMBackend::TypeAndValue& param) {
            return !lhsXvalueType.empty() && param.TypeName == lhsXvalueType
                && (param.IsRvalueRef || (!param.Pointer && !param.IsAlias));
        };

        // A C++ right operand keeps the left literal as the `char*` a C++ free operator is
        // declared over; wrapping it would offer only CFlat 'string' candidates.
        bool lhsStringLiteral = false;
        const bool equalityComparison = op == "==" || op == "!=";
        const bool lhsIsBareNullptr = lhsPointerDepth == 0
            && llvm::isa<llvm::ConstantPointerNull>(lvalue);
        const bool rhsIsBareNullptr = rhsPointerDepth == 0
            && llvm::isa_and_nonnull<llvm::ConstantPointerNull>(rvalue);

        // If the LHS is a string literal (ptr to global constant), wrap it
        // as a %string struct so operator+(string, ...) can match.
        if (lvalue->getType()->isPointerTy())
        {
            if (auto* c = llvm::dyn_cast<llvm::Constant>(lvalue))
            {
                if (compiler->stringLiteralLenByPtr.count(c))
                {
                    // Preserve raw pointer comparison for a string literal and bare nullptr.
                    if (isCxxRecordValue(rvalue))
                        lhsStringLiteral = true;
                    else if (!(equalityComparison && rhsIsBareNullptr))
                        lvalue = compiler->WrapStringLiteralAsString(lvalue);
                }
            }
        }

        auto* ty = lvalue->getType();
        auto* stringTy = llvm::StructType::getTypeByName(*compiler->context, "string");
        const bool lhsIsStringValue = stringTy != nullptr && ty == stringTy;
        const bool rhsIsStringValue = stringTy != nullptr && rvalue != nullptr
            && rvalue->getType() == stringTy;
        const bool anyComparison = equalityComparison
            || op == "<" || op == ">" || op == "<=" || op == ">=";
        if (anyComparison
            && ((lhsIsStringValue && rhsIsBareNullptr)
                || (rhsIsStringValue && lhsIsBareNullptr)))
        {
            LogErrorContext(ctx,
                "a 'string' is a value type and cannot be compared to nullptr; "
                "compare with an empty string or use .length()");
            return nullptr;
        }
        /*
         * A right operand that is a pointer no depth describes (`T(*)[N]` from an array of
         * arrays, depth -1) or with no identity at all must never satisfy a C++ `U*` parameter of
         * a specific depth: clang refuses it, and a guess binds `char*` data to a `char**` slot.
         * Null and string literals convert to any pointer and stay exempt.
         */
        const bool rhsDepthUnproven = rvalue != nullptr && rvalue->getType()->isPointerTy()
            && (rhsPointerDepth < 0 || (rhsPointerDepth == 0 && rhsTypeName.empty()))
            && !llvm::isa<llvm::ConstantPointerNull>(rvalue)
            && !(llvm::isa<llvm::Constant>(rvalue)
                 && compiler->stringLiteralLenByPtr.count(llvm::cast<llvm::Constant>(rvalue)));
        auto isTypedPointerParam = [&](const LLVMBackend::FunctionSymbol& candidate, size_t index) {
            if (!candidate.IsCxx || index >= candidate.Parameters.size()) return false;
            auto rawIt = compiler->cxxFunctionSignatures_.find(candidate.SourceName);
            if (rawIt != compiler->cxxFunctionSignatures_.end())
                for (const auto& raw : rawIt->second)
                    if (raw.linkageName == candidate.UniqueName && index < raw.paramSpellings.size())
                        return raw.paramSpellings[index].find('*') != std::string::npos
                            && raw.paramSpellings[index].find("void") == std::string::npos;
            // No recorded spelling: the mapped shape, where a reference is a pointer too.
            const auto& param = candidate.Parameters[index];
            return param.Pointer && !param.IsAlias && !param.CxxConstRef() && !param.IsRvalueRef
                && !param.TypeName.empty() && param.TypeName != "void";
        };
        auto refuseUnprovenPointer = [&](const std::string& operatorName) {
            LogErrorContext(ctx, std::format(
                "C++ '{}' cannot bind its right operand: the pointer's pointee type is not known here (a pointer to an array does not convert to a typed pointer parameter).",
                operatorName));
        };
        /*
         * C++20 REWRITES 'a != b' as '!(a == b)', so a class compiled at that standard (every
         * libc++ iterator, for one) declares only operator==. Bind the equality and negate it
         * when the type has no operator!= of its own; a bound '==' that does not yield an
         * integer is left alone.
         */
        auto negateEquality = [&]() -> llvm::Value* {
            if (op != "!=") return nullptr;
            llvm::Value* eq = TryBinaryOperatorOverload(lvalue, "==", rvalue, ctx, lhsElemType,
                                                        rhsPointerDepth, rhsElemPointer, lhsStorage,
                                                        rhsStorage, false, allowReversed,
                lhsIsRvalue, rhsIsRvalue,
                                                        lhsTypeName, rhsTypeName, lhsPointerDepth, lhsElemPointer,
                                                        {}, {}, false, {}, {}, false, {}, {}, rhsCxxPointeeConst, lhsArithIdentity, rhsArithIdentity, rhsExplicitMove, lhsExplicitMove,
                                                        lhsCxxNullptrT, rhsCxxNullptrT,
                                                        lhsMaterializedPrvalue, lhsCxxPointeeConst,
                                                        lhsObjectCv, rhsObjectCv);
            if (eq == nullptr || !eq->getType()->isIntegerTy()) return nullptr;
            if (eq->getType()->isIntegerTy(1)) return compiler->builder->CreateNot(eq);
            return compiler->builder->CreateICmpEQ(eq,
                       llvm::ConstantInt::get(eq->getType(), 0));
        };

        auto tryBoolComparisonConversion = [&]() -> llvm::Value* {
            if (op != "==" && op != "!=" && op != "&&" && op != "||") return nullptr;
            if (lvalue == nullptr || rvalue == nullptr) return nullptr;
            auto isEnumOperand = [&](const std::string& name) {
                return !name.empty() && (compiler->IsScopedEnumTypeName(name)
                    || !compiler->ResolveEnumTypeName(name).empty());
            };
            // A C++ class's bool conversion cannot erase a scoped enum's identity to make an
            // otherwise ill-formed comparison viable. Let the C++ operator candidate set decide.
            if ((isCxxRecordValue(lvalue) && isEnumOperand(rhsTypeName))
                || (isCxxRecordValue(rvalue) && isEnumOperand(lhsTypeName)))
                return nullptr;
            if (op == "==" || op == "!=")
            {
                auto hasExplicitOperatorBool = [&](llvm::Value* value,
                                                   const std::string& name) {
                    if (value == nullptr || name.empty() || !compiler->IsCxxRecord(name))
                        return false;
                    compiler->EnsureCxxMemberProjected(name, "operator bool");
                    const auto* info = compiler->GetCxxClassInfo(name);
                    return info != nullptr
                        && info->explicitConversions.count("operator bool") != 0;
                };
                if (hasExplicitOperatorBool(lvalue, lhsTypeName)
                    || hasExplicitOperatorBool(rvalue, rhsTypeName))
                {
                    auto operandType = [&](llvm::Value* value, const std::string& name) {
                        if (!name.empty())
                            return SpellType(*compiler,
                                LLVMBackend::TypeAndValue{ .TypeName = name });
                        if (auto* structType = llvm::dyn_cast<llvm::StructType>(value->getType());
                            structType != nullptr && structType->hasName())
                            return structType->getName().str();
                        return ScalarTypeNameForValue(value->getType());
                    };
                    const std::string leftType = operandType(lvalue, lhsTypeName);
                    const std::string rightType = operandType(rvalue, rhsTypeName);
                    LogErrorContext(ctx, std::format(
                        "invalid operands to binary expression ('{}' and '{}')",
                        leftType, rightType));
                    // Placeholder result so the caller does not cascade a "no operator" error.
                    return llvm::ConstantInt::getFalse(lvalue->getContext());
                }
            }
            // Only a class with an `operator bool` takes part (the way a condition converts it);
            // a class whose conversions are `operator int` / `operator double` is not a bool
            // comparison and must keep resolving through its own operators.
            auto convertClassToBool = [&](llvm::Value* value, const std::string& name) {
                if (name.empty() || (!compiler->IsCxxRecord(name)
                    && compiler->generatedCxxRecords_.count(name) == 0))
                    return value;
                EnsureOperatorBoolForValue(value);
                if (!compiler->HasOperatorBoolForType(value->getType())) return value;
                return compiler->CoerceToBoolCondition(value);
            };
            llvm::Value* left = convertClassToBool(lvalue, lhsTypeName);
            llvm::Value* right = convertClassToBool(rvalue, rhsTypeName);
            if (left == nullptr || right == nullptr || (left == lvalue && right == rvalue))
                return nullptr;
            if (op == "&&" || op == "||")
            {
                auto toBool = [&](llvm::Value* value) -> llvm::Value* {
                    if (value->getType()->isIntegerTy(1)) return value;
                    if (value->getType()->isIntegerTy())
                        return compiler->builder->CreateICmpNE(
                            value, llvm::ConstantInt::get(value->getType(), 0));
                    return nullptr;
                };
                left = toBool(left);
                right = toBool(right);
                if (left == nullptr || right == nullptr) return nullptr;
                return compiler->CreateOperation(op == "&&" ? LLVMBackend::Operation::LogicalAnd
                                                            : LLVMBackend::Operation::LogicalOr,
                                                 left, right);
            }
            else if (left->getType()->isIntegerTy(1) && right->getType()->isIntegerTy()
                     && !right->getType()->isIntegerTy(1))
                left = compiler->builder->CreateZExt(left, right->getType());
            else if (right->getType()->isIntegerTy(1) && left->getType()->isIntegerTy()
                     && !left->getType()->isIntegerTy(1))
                right = compiler->builder->CreateZExt(right, left->getType());
            if (left->getType() != right->getType()) return nullptr;
            return compiler->CreateOperation(op, left, right, false, false);
        };

        // C++ free operators are registered under a private namespace-qualified alias. Trigger
        // deferred binding for the namespaces of both class operands, then call the matching free
        // function through the normal overload/ABI path. The raw spelling is consulted only to
        // distinguish `const T&` from an actual `T*`; both have the same mapped pointer shape.
        // The last ADL-form request clang itself rejected (its text), for the T85 drop fallback.
        std::string adlClangRefusal;
        // True when that refusal is clang's "is ambiguous" diagnostic: refused, never a fallback.
        bool adlClangAmbiguous = false;
        // clangDecides: the ADL form asks clang even for a class hiding nothing (T85 template rank).
        auto collectOperatorOperandRules = [&]() {
            std::vector<antlr4::ParserRuleContext*> operands;
            if (ctx == nullptr) return operands;
            for (auto* child : ctx->children)
                if (auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(child))
                    if (!dynamic_cast<CFlatParser::AssignmentOperatorContext*>(rule))
                        operands.push_back(rule);
            return operands;
        };
        std::set<std::string> binaryExcludedLinkages;
        std::string binaryExcludedTable;
        auto tryFreeOperator = [&](bool adlOnly = false, bool clangDecides = false,
                                   bool memberFreeRanking = false) -> llvm::Value* {
            if (ctx != nullptr && ctx == cxxAdlAmbiguousCtx_) return nullptr;
            llvm::Value* freeLhsStorage = lhsIsRvalue ? nullptr : lhsStorage;
            llvm::Value* freeRhsStorage = rhsIsRvalue ? nullptr : rhsStorage;
            auto structInfo = [](llvm::Value* value) {
                std::pair<llvm::StructType*, std::string> result;
                if (value == nullptr || !value->getType()->isStructTy()) return result;
                auto* st = llvm::cast<llvm::StructType>(value->getType());
                if (st->isLiteral() || !st->hasName()) return result;
                result.first = st;
                result.second = st->getName().str();
                return result;
            };
            auto leftInfo = structInfo(lvalue);
            auto rightInfo = structInfo(rvalue);
            auto operands = collectOperatorOperandRules();
            auto setLiteralIdentity = [&](LLVMBackend::NamedVariable& arg, size_t side) {
                arg.CxxArithIdentity = side == 0 ? lhsArithIdentity : rhsArithIdentity;
                if (arg.CxxArithIdentity.empty())
                {
                    const std::string& sourceTypeName = side == 0 ? lhsTypeName : rhsTypeName;
                    llvm::Value* value = side == 0 ? lvalue : rvalue;
                    arg.CxxArithIdentity = CxxScalarOperandIdentity(
                        compiler, {}, sourceTypeName, value);
                }
                if (operands.size() == 2 && side < operands.size())
                {
                    arg.LiteralIdentity = LLVMBackend::LiteralIdentityForOverload(
                        operands[side]->getText(), &arg.LiteralIdentitySuffixed);
                    arg.LiteralSigned = LLVMBackend::LiteralTextHasSign(operands[side]->getText());
                }
                if (arg.LiteralIdentity.empty() && !arg.CxxArithIdentity.empty()
                    && arg.BaseType != nullptr
                    && (arg.BaseType->isIntegerTy() || arg.BaseType->isFloatingPointTy()))
                    arg.TypeAndValue.TypeName = arg.CxxArithIdentity;
            };
            auto isFat = [](const std::string& typeName) {
                return typeName == "__iface_fat_ptr" || typeName == "__closure_fat_ptr";
            };
            if (leftInfo.second.empty() && rightInfo.second.empty()) return nullptr;
            if (isFat(leftInfo.second) || isFat(rightInfo.second)) return nullptr;

            std::vector<std::string> namespaces;
            auto addNamespace = [&](const std::string& typeName) {
                if (typeName.empty()) return;
                // Template arguments follow the first '$' and carry their own dots
                // ("std.__wrap_iter$std.stringptr"); the namespace comes from the base name.
                const std::string base = typeName.substr(0, typeName.find('$'));
                const size_t dot = base.rfind('.');
                const std::string ns = dot == std::string::npos
                    ? std::string() : base.substr(0, dot);
                if (std::find(namespaces.begin(), namespaces.end(), ns) == namespaces.end())
                    namespaces.push_back(ns);
            };
            addNamespace(leftInfo.second);
            addNamespace(rightInfo.second);
            if (std::find(namespaces.begin(), namespaces.end(), std::string()) == namespaces.end())
                namespaces.push_back(std::string());

            const std::string opName = "operator" + op;
            /*
             * The free operator may be a FUNCTION TEMPLATE over the operand's class template -
             * every libc++ basic_string operator is. Nothing is registered for it until some
             * concrete operand pair asks, so instantiate one for THESE operands through the same
             * generated-wrapper path an ordinary C++ template call uses. The wrapper is keyed and
             * registered on the operand pair, never on the shared "ns.operatorX" name, so one
             * pair's wrapper can never answer for another.
             */
            auto callOperatorTemplate = [&](const std::string& sourceName,
                                            bool adlInfix = false) -> llvm::Value* {
                if (lvalue == nullptr || rvalue == nullptr) return nullptr;
                if (!adlInfix && !compiler->HasCxxFunctionTemplate(sourceName)) return nullptr;
                auto templateArgument = [&](llvm::Value* value, llvm::Value* storage,
                                            llvm::Value* ownSlot,
                                            llvm::StructType* structType,
                                            const std::string& typeName,
                                            const std::string& sourceTypeName,
                                            int pointerDepth, bool elemPointer,
                                            bool cxxNullptrT,
                                            bool pointeeConst = false) {
                    LLVMBackend::NamedVariable arg;
                    arg.Primary = value;
                    arg.Storage = storage;
                    arg.BaseType = value != nullptr ? value->getType() : nullptr;
                    arg.TypeAndValue.TypeName = typeName;
                    arg.TypeAndValue.IsCxxNullptrT = cxxNullptrT;
                    if (storage != nullptr)
                    {
                        auto* constGlobal = llvm::dyn_cast<llvm::GlobalVariable>(
                            llvm::getUnderlyingObject(storage));
                        arg.TypeAndValue.SetCxxConstRef(compiler->IsCxxConstReferent(storage)
                            || (constGlobal != nullptr && !compiler->IsCxxMutableFieldAccess(storage)
                                && (constGlobal->isConstant()
                                || compiler->IsConstHeaderGlobal(constGlobal))));
                    }
                    // A SCALAR operand has no struct name; its declared name is the only thing
                    // that tells a C++ template `char` from `i8` or `long` from `i64`.
                    if (typeName.empty())
                    {
                        const bool namedEnum = !sourceTypeName.empty()
                            && value != nullptr && value->getType()->isIntegerTy()
                            && !LLVMBackend::IsPrimitiveTypeName(sourceTypeName)
                            && !compiler->IsCxxRecord(sourceTypeName)
                            && !compiler->IsDataStructure(sourceTypeName);
                        if (!sourceTypeName.empty()
                            && (!compiler->ResolveEnumTypeName(sourceTypeName).empty()
                                || namedEnum))
                        {
                            arg.TypeAndValue.TypeName = sourceTypeName;
                            arg.TypeAndValue.IsScopedEnum =
                                compiler->IsScopedEnumTypeName(sourceTypeName);
                            arg.TypeAndValue.EnumBacking =
                                compiler->GetEnumBackingType(sourceTypeName);
                        }
                        else
                            arg.InferSourceTypeName = sourceTypeName;
                    }
                    // A pointer's declared name is its POINTEE: stamp the recorded depth or `s + p` binds the
                    // `char` overload. An unrecorded depth withholds the name, never guessed.
                    if (value != nullptr && value->getType()->isPointerTy())
                    {
                        if (pointerDepth >= 1)
                        {
                            arg.TypeAndValue.Pointer = true;
                            arg.TypeAndValue.PointerDepth = pointerDepth;
                            arg.TypeAndValue.ElemPointer = elemPointer;
                            // `s.c_str()` stays `const char*` to C++ overload resolution.
                            arg.TypeAndValue.SetPointeeConstBit(pointeeConst);
                        }
                        else if (typeName.empty() && !cxxNullptrT) arg.InferSourceTypeName.clear();
                    }
                    arg.IsRvalue = structType != nullptr && storage == nullptr;
                    // A temporary read from its own slot (or a '?:' slot join) passes that slot;
                    // a bitwise re-spill is a second object no ctor built.
                    auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(value);
                    const bool ownValue = ownSlot != nullptr
                        && ((load != nullptr && load->getPointerOperand() == ownSlot)
                            || isParallelStorageJoin(value, ownSlot));
                    if (structType != nullptr && arg.Storage == nullptr && arg.IsRvalue && ownValue)
                        arg.Storage = ownSlot;
                    else if (structType != nullptr && arg.Storage == nullptr)
                    {
                        auto* temp = compiler->CreateAlloca(structType);
                        compiler->CreateAssignment(value, temp);
                        arg.Storage = temp;
                    }
                    return arg;
                };
                std::vector<LLVMBackend::NamedVariable> templateArgs;
                templateArgs.push_back(templateArgument(lvalue, freeLhsStorage, lhsStorage,
                                                       leftInfo.first, leftInfo.second, lhsTypeName,
                                                       lhsPointerDepth, lhsElemPointer,
                                                       lhsCxxNullptrT));
                templateArgs.push_back(templateArgument(rvalue, freeRhsStorage, rhsStorage,
                                                        rightInfo.first, rightInfo.second, rhsTypeName,
                                                        rhsPointerDepth, rhsElemPointer,
                                                        rhsCxxNullptrT,
                                                        rhsCxxPointeeConst));
                if (rhsExplicitMove && templateArgs.size() == 2)
                    templateArgs[1].IsExplicitMove = true;
                if (lhsExplicitMove && templateArgs.size() == 2)
                    templateArgs[0].IsExplicitMove = true;
                // `a OP b` with exactly two operands: their syntax decides lvalue-ness.
                if (operands.size() == 2)
                    for (size_t side = 0; side < 2; ++side)
                        templateArgs[side].CxxLvalueKind = CxxArgumentLvalueKind(operands[side]);
                for (size_t side = 0; side < templateArgs.size(); ++side)
                    setLiteralIdentity(templateArgs[side], side);
                std::string registeredName;
                std::string templateError;
                /*
                 * ADL form: no namespace-scope candidate is registered (a hidden friend is not
                 * one), so C++ itself resolves `p0 OP p1`. A refusal stays silent - the caller
                 * goes on to CFlat's own fallbacks and diagnostics.
                 */
                if (adlInfix)
                {
                    adlClangRefusal.clear();
                    adlClangAmbiguous = false;
                    if (!compiler->RequestCxxFreeFunction(sourceName, {}, templateArgs,
                                                          registeredName, templateError, op)
                        || registeredName.empty())
                    {
                        if (ReportCxxConstPointerOperatorFailure(
                                compiler, ctx, sourceName, templateArgs, op,
                                {}, {}, lhsCxxPointeeConst, rhsCxxPointeeConst))
                            return nullptr;
                        // Nothing viable AT the infix expression: CFlat's own refusal of the smaller
                        // set says the same. Any other clang refusal (ambiguous, a body) is kept.
                        if (compiler->LastCxxRequestClangRejected() && !templateError.empty()
                            && (!compiler->LastCxxRequestNoViableAtRequest()
                                || (cxxCompoundStrictLookup && clangDecides)))
                        {
                            adlClangRefusal = templateError.starts_with("clang: ")
                                ? templateError : "clang: " + templateError;
                            adlClangAmbiguous = templateError.find("use of overloaded operator") != std::string::npos
                                && templateError.find("is ambiguous") != std::string::npos;
                        }
                        return nullptr;
                    }
                }
                else if (!compiler->RequestCxxFunctionTemplate(sourceName, {}, {}, templateArgs, {},
                                                         registeredName, templateError, op)
                    || registeredName.empty())
                {
                    if (!binaryExcludedLinkages.empty()) return nullptr;
                    if (llvm::Value* converted = tryBoolComparisonConversion()) return converted;
                    // A failed free-template request does not prove the class receiver is unbindable:
                    // member candidates remain in the complete compound-assignment lookup.
                    std::string rvalueSide;
                    for (size_t i = 0; i < templateArgs.size(); ++i)
                        if (templateArgs[i].IsRvalue && !templateArgs[i].TypeAndValue.Pointer)
                        {
                            rvalueSide = i == 0 ? "left" : "right";
                            break;
                        }
                    // Only the receiver side can still be rescued by a member; a right-operand refusal stands.
                    const bool deferClassCompoundRefusal =
                        cxxCompoundStrictLookup && isCxxRecordValue(lvalue) && rvalueSide != "right";
                    if (!deferClassCompoundRefusal && !rvalueSide.empty())
                        LogErrorContext(ctx, std::format(
                            "C++ '{}' cannot bind its {} operand because the CFlat expression is an rvalue and the parameter is a non-const reference.",
                            opName, rvalueSide));
                    // A scalar temporary refused by the template's non-const `U&` parameter.
                    else if (!deferClassCompoundRefusal
                        && templateError.find("is a non-const lvalue reference and cannot bind")
                            != std::string::npos)
                        LogErrorContext(ctx, templateError);
                    return nullptr;
                }
                FlushCallPostfix();
                llvm::Value* result = CallOperatorWithoutExcluded(
                    compiler->functionTable, compiler, binaryExcludedTable.empty() ? registeredName : binaryExcludedTable,
                    registeredName, templateArgs, binaryExcludedLinkages);
                if (result != nullptr) TrackOwnedStringOperatorResult(compiler, result);
                return result;
            };

            /*
             * Argument-dependent lookup the way C++ does it: with a C++ class operand, `p0 OP p1`
             * is resolved by clang over the associated namespaces AND the hidden friends defined
             * in the classes (found by ADL only, so nothing is registered for them here).
             */
            if (adlOnly)
            {
                static const std::set<std::string> adlOperators = {
                    "+", "-", "*", "/", "%", "<<", ">>", "&", "|", "^", "==", "!=", "<", "<=",
                    ">", ">=", "+=", "-=", "*=", "/=", "%=", "<<=", ">>=", "&=", "|=", "^="
                };
                if (adlOperators.count(op) == 0 || cxxOperatorRewriteDepth_ > 0
                    || cxxAdlSuppressDepth_ > 0)
                    return nullptr;
                auto cxxClass = [&](const std::string& name) {
                    return !name.empty() && (compiler->IsCxxRecord(name)
                                             || compiler->generatedCxxRecords_.count(name) != 0);
                };
                const bool leftClass = cxxClass(leftInfo.second);
                const bool rightClass = cxxClass(rightInfo.second);
                if (!leftClass && !rightClass) return nullptr;
                // Nothing to find beyond what master's lookup already tried: no clang request.
                auto mayHide = [&](const std::string& name) {
                    return cxxClass(name) && compiler->CxxClassMayHaveUnregisteredOperators(name);
                };
                auto enumOperand = [&](const std::string& name) {
                    return !name.empty() && (compiler->IsScopedEnumTypeName(name)
                        || !compiler->ResolveEnumTypeName(name).empty());
                };
                if (!clangDecides && !mayHide(leftInfo.second) && !mayHide(rightInfo.second)
                    && !enumOperand(lhsTypeName) && !enumOperand(rhsTypeName))
                    return nullptr;
                const std::string& owner = leftClass ? leftInfo.second : rightInfo.second;
                const std::string base = owner.substr(0, owner.find('$'));
                const size_t dot = base.rfind('.');
                adlClangAmbiguous = false;
                llvm::Value* adlResult = callOperatorTemplate((dot == std::string::npos
                    ? std::string("__cxx_free") : base.substr(0, dot)) + "." + opName, true);
                // Clang's ambiguity is the answer: refuse here, never fall back to a registered overload.
                if (adlResult == nullptr && adlClangAmbiguous && ctx != nullptr)
                {
                    cxxAdlAmbiguousCtx_ = ctx;
                    if (memberFreeRanking)
                        LogErrorContext(ctx, std::format(
                            "ambiguous C++ binary operator '{}': a member and a free overload are both viable and rank equally",
                            opName));
                    else
                        LogErrorContext(ctx, adlClangRefusal);
                }
                return adlResult;
            }

            for (const std::string& ns : namespaces)
            {
                const std::string sourceName = ns.empty() ? opName : ns + "." + opName;
                compiler->TryBindCxxFunction(sourceName);
                const std::string lookupName = ns.empty()
                    ? "__cxx_free." + opName : sourceName;

                auto isReferenceParam = [&](const LLVMBackend::FunctionSymbol& candidate,
                                            size_t index) {
                    auto rawIt = compiler->cxxFunctionSignatures_.find(candidate.SourceName);
                    if (rawIt == compiler->cxxFunctionSignatures_.end()) return false;
                    for (const auto& raw : rawIt->second)
                        if (raw.linkageName == candidate.UniqueName
                            && index < raw.paramSpellings.size())
                            return raw.paramSpellings[index].find('&') != std::string::npos;
                    return false;
                };
                auto isMutableReferenceParam = [&](const LLVMBackend::FunctionSymbol& candidate,
                                                   size_t index) {
                    if (!candidate.IsCxx) return false;
                    auto rawIt = compiler->cxxFunctionSignatures_.find(candidate.SourceName);
                    if (rawIt != compiler->cxxFunctionSignatures_.end())
                        for (const auto& raw : rawIt->second)
                            if (raw.linkageName == candidate.UniqueName
                                && index < raw.paramSpellings.size())
                            {
                                const auto& spelling = raw.paramSpellings[index];
                                return spelling.find('&') != std::string::npos
                                    && spelling.find("&&") == std::string::npos
                                    && spelling.find("const") == std::string::npos;
                            }
                    return candidate.Parameters[index].IsAlias
                        && !candidate.Parameters[index].CxxConstRef()
                        && !candidate.Parameters[index].IsRvalueRef
                        && !candidate.Parameters[index].IsCxxRefToPointer;
                };

                // An exact class-operand candidate always ranks above a user conversion, so the
                // conversion probes (one wrapper each) run only when no exact candidate exists.
                bool allowClassConversion = true;
                if (auto fit = compiler->functionTable.find(lookupName);
                    fit != compiler->functionTable.end())
                    for (const auto& candidate : fit->second)
                        if (!isMemberCandidate(candidate) && candidate.Parameters.size() >= 2
                            && candidate.SourceName == sourceName
                            && (leftInfo.second.empty()
                                || candidate.Parameters[0].TypeName == leftInfo.second)
                            && (rightInfo.second.empty()
                                || candidate.Parameters[1].TypeName == rightInfo.second))
                        {
                            allowClassConversion = false;
                            break;
                        }
                auto implicitClassOperandBinds = [&](const LLVMBackend::FunctionSymbol& candidate,
                                                      size_t index,
                                                      const std::string& sourceType,
                                                      llvm::Value* value,
                                                      llvm::Value* storage,
                                                      bool isRvalue) {
                    if (sourceType.empty() || index >= candidate.Parameters.size()
                        || candidate.Parameters[index].TypeName == sourceType)
                        return true;
                    if (!allowClassConversion) return false;
                    const auto& param = candidate.Parameters[index];
                    if (!candidate.IsCxx || !compiler->IsCxxRecord(sourceType)
                        || !compiler->IsCxxRecord(param.TypeName)
                        || (param.Pointer && !param.IsAlias && !param.IsRvalueRef)
                        || param.ElemPointer || param.IsCxxRefToPointer)
                        return false;

                    LLVMBackend::NamedVariable argument;
                    argument.Primary = value;
                    argument.Storage = storage;
                    argument.BaseType = value != nullptr ? value->getType() : nullptr;
                    argument.TypeAndValue.TypeName = sourceType;
                    argument.IsRvalue = isRvalue;
                    if (operands.size() == 2)
                        argument.CxxLvalueKind = CxxArgumentLvalueKind(operands[index]);
                    if (storage != nullptr)
                        argument.TypeAndValue.SetCxxConstRef(compiler->IsCxxConstReferent(storage));

                    // An rvalue reference is not IsAlias (CInterop 4176): a derived prvalue binds its base
                    // `Base&&` by the same derived-to-base test as a `const Base&`.
                    auto baseTarget = param;
                    if (param.IsRvalueRef && param.Pointer && !param.ElemPointer && isRvalue)
                        baseTarget.IsAlias = true;
                    if (compiler->IsCxxDerivedToBaseValue(argument.TypeAndValue, baseTarget, &argument))
                        return true;
                    // Cheap record scan first: each CanImplicitlyConstructCxxClass probe is a wrapper.
                    if (!compiler->HasImplicitClassConversionOperator(sourceType)
                        && !compiler->HasCxxImplicitConvertingCtor(param.TypeName))
                        return false;
                    if (compiler->CanImplicitlyConstructCxxClass(argument, param))
                        return true;
                    if (param.IsAlias || param.IsRvalueRef)
                        return false;

                    // A by-value parameter accepts the same implicit class conversion routes as
                    // a const reference; the selected call still materializes its value argument.
                    LLVMBackend::TypeAndValue referenceParam = param;
                    referenceParam.Pointer = true;
                    referenceParam.IsAlias = true;
                    referenceParam.IsRvalueRef = false;
                    referenceParam.IsCxxRefToPointer = false;
                    referenceParam.SetCxxConstRef(true);
                    return compiler->CanImplicitlyConstructCxxClass(argument, referenceParam);
                };
                auto hasBuiltinScalarConversion = [&](const std::string& sourceType) {
                    if (sourceType.empty() || !compiler->IsCxxRecord(sourceType)) return false;
                    std::function<bool(const std::string&, std::set<std::string>, int)> hasInClass;
                    hasInClass = [&](const std::string& typeName,
                                     std::set<std::string> hiddenConversions,
                                     int depth) {
                        if (depth > 32 || typeName.empty()
                            || !compiler->IsCxxRecord(typeName)) return false;
                        compiler->EnsureCxxConversionOperatorsProjected(typeName);
                        const auto* info = compiler->GetCxxClassInfo(typeName);
                        if (info == nullptr) return false;
                        for (const auto& method : info->directMethods)
                        {
                            if (!method.raw.isConversion) continue;
                            const bool hidden = !method.raw.name.empty()
                                && hiddenConversions.count(method.raw.name) != 0;
                            if (!method.raw.name.empty())
                                hiddenConversions.insert(method.raw.name);
                            // A deleted conversion still ranks (clang); its use is refused after selection.
                            if (!hidden && method.spellable && !method.raw.isExplicit
                                && method.raw.bindRefusal.empty()
                                && method.ret.IsPrimitive() && !method.ret.Pointer)
                                return true;
                        }
                        for (const auto& base : info->bases)
                        {
                            std::string baseType = compiler->ResolveCxxBaseIdentity(base);
                            if (baseType.empty()) baseType = base.name;
                            if (hasInClass(baseType, hiddenConversions, depth + 1)) return true;
                        }
                        return false;
                    };
                    return hasInClass(sourceType, {}, 0);
                };

                bool candidateFound = false;
                bool hasClassConversionCandidate = false;
                bool rightTypedPointer = false;
                bool leftReference = false;
                bool rightReference = false;
                bool leftByValueSame = false;
                bool rightByValueSame = false;
                bool leftMutableReference = false;
                bool rightMutableReference = false;
                // Refuse an rvalue only when NO viable candidate takes that operand otherwise.
                bool leftBindsRvalue = false;
                bool rightBindsRvalue = false;
                // A prvalue operand is a sink only when EVERY viable candidate takes it by value or
                // &&; a mixed set (one `const T&`) keeps the temp path. A written `move` needs any.
                bool leftXvalue = !lhsExplicitMove;
                bool rightXvalue = !rhsExplicitMove;
                std::set<std::string> leftMutableReferenceLinkages;
                std::set<std::string> rightMutableReferenceLinkages;
                // The bare `nullptr` literal operand (a typed null records a pointer depth).
                auto nullOperand = [](llvm::Value* value, int pointerDepth) {
                    return pointerDepth < 1 && llvm::isa_and_nonnull<llvm::ConstantPointerNull>(value);
                };
                auto nullArgument = [&]() {
                    LLVMBackend::NamedVariable arg;
                    arg.Primary = llvm::ConstantPointerNull::get(
                        llvm::PointerType::getUnqual(*compiler->context));
                    arg.BaseType = arg.Primary->getType();
                    arg.TypeAndValue.IsCxxNullptrT = true;
                    return arg;
                };
                if (auto fit = compiler->functionTable.find(lookupName);
                    fit != compiler->functionTable.end())
                {
                    for (const auto& candidate : fit->second)
                    {
                        if (isMemberCandidate(candidate) || candidate.Parameters.size() < 2
                            || candidate.SourceName != sourceName) continue;
                        // A class-first equality candidate is a C++20 reversed candidate when
                        // the written left operand is scalar; let reversedCandidate assemble it.
                        if ((op == "==" || op == "<=>") && leftInfo.second.empty()
                            && lvalue != nullptr
                            && (lvalue->getType()->isIntegerTy()
                                || lvalue->getType()->isFloatingPointTy())
                            && compiler->IsCxxRecord(candidate.Parameters[0].TypeName))
                            continue;
                        if (!implicitClassOperandBinds(candidate, 0, leftInfo.second, lvalue,
                                                       freeLhsStorage, lhsIsRvalue))
                            continue;
                        if (!implicitClassOperandBinds(candidate, 1, rightInfo.second, rvalue,
                                                       freeRhsStorage, rhsIsRvalue))
                            continue;
                        hasClassConversionCandidate = hasClassConversionCandidate
                            || (!leftInfo.second.empty()
                                && candidate.Parameters[0].TypeName != leftInfo.second)
                            || (!rightInfo.second.empty()
                                && candidate.Parameters[1].TypeName != rightInfo.second);
                        // A `nullptr` operand cannot bind this candidate's class reference:
                        // leave it to the function template set (the nullptr_t overload).
                        if ((nullOperand(lvalue, lhsPointerDepth)
                             && compiler->CxxNullCannotBindClassReference(
                                 candidate, candidate.Parameters[0], nullArgument()))
                            || (nullOperand(rvalue, rhsPointerDepth)
                                && compiler->CxxNullCannotBindClassReference(
                                    candidate, candidate.Parameters[1], nullArgument())))
                            continue;
                        candidateFound = true;
                        rightXvalue = rhsExplicitMove ? rightXvalue || rhsXvalueSink(candidate.Parameters[1])
                                                      : rightXvalue && rhsXvalueSink(candidate.Parameters[1]);
                        leftXvalue = lhsExplicitMove ? leftXvalue || lhsXvalueSink(candidate.Parameters[0])
                                                     : leftXvalue && lhsXvalueSink(candidate.Parameters[0]);
                        rightTypedPointer = rightTypedPointer || isTypedPointerParam(candidate, 1);
                        // Both operands reach this path as struct VALUES, so a parameter of pointer
                        // shape - `const T&` or `T&` - is passed as the address of that value. The
                        // raw spelling is the first source; the mapped shape is the fallback for a
                        // signature whose spelling was not recorded.
                        if (!leftInfo.second.empty())
                            leftReference = leftReference || isReferenceParam(candidate, 0)
                                         || candidate.Parameters[0].Pointer;
                        if (!rightInfo.second.empty())
                            rightReference = rightReference || isReferenceParam(candidate, 1)
                                         || candidate.Parameters[1].Pointer;
                        // A by-value parameter of the operand's own class beside a reference one: the
                        // operand stays a value, so both candidates rank it (clang: the pair ties).
                        if (!leftInfo.second.empty() && !isReferenceParam(candidate, 0)
                            && !candidate.Parameters[0].Pointer
                            && candidate.Parameters[0].TypeName == leftInfo.second)
                            leftByValueSame = true;
                        if (!rightInfo.second.empty() && !isReferenceParam(candidate, 1)
                            && !candidate.Parameters[1].Pointer
                            && candidate.Parameters[1].TypeName == rightInfo.second)
                            rightByValueSame = true;
                        if (isMutableReferenceParam(candidate, 0))
                        {
                            leftMutableReference = true;
                            leftMutableReferenceLinkages.insert(candidate.UniqueName);
                        }
                        else
                            leftBindsRvalue = true;
                        if (isMutableReferenceParam(candidate, 1))
                        {
                            rightMutableReference = true;
                            rightMutableReferenceLinkages.insert(candidate.UniqueName);
                        }
                        else
                            rightBindsRvalue = true;
                    }
                }
                if (candidateFound && hasClassConversionCandidate)
                {
                    const bool leftScalarConversion =
                        hasBuiltinScalarConversion(leftInfo.second);
                    const bool rightScalarConversion =
                        hasBuiltinScalarConversion(rightInfo.second);
                    const bool hasBuiltinOperandPair =
                        (leftScalarConversion
                            && ((rvalue != nullptr && (rvalue->getType()->isIntegerTy()
                                                      || rvalue->getType()->isFloatingPointTy()))
                                || rightScalarConversion))
                        || (rightScalarConversion && lvalue != nullptr
                            && (lvalue->getType()->isIntegerTy()
                                || lvalue->getType()->isFloatingPointTy()));
                    if (hasBuiltinOperandPair)
                    {
                        if (llvm::Value* ranked = callOperatorTemplate(sourceName, true))
                            return ranked;
                        if (adlClangAmbiguous && ctx != nullptr)
                        {
                            cxxAdlAmbiguousCtx_ = ctx;
                            LogErrorContext(ctx, adlClangRefusal);
                        }
                    }
                }
                // No ordinary free operator binds: the namespace may still declare the operator
                // as a function TEMPLATE over the operand's class template.
                if (!candidateFound)
                {
                    if (llvm::Value* templated = callOperatorTemplate(sourceName)) return templated;
                    continue;
                }

                auto makeArgument = [&](llvm::Value* value, llvm::Value* storage,
                                        llvm::StructType* structType, const std::string& typeName,
                                        bool reference, int pointerDepth, bool cxxNullptrT,
                                        bool byValueSame) {
                    LLVMBackend::NamedVariable result;
                    result.Primary = value;
                    result.Storage = storage;
                    result.BaseType = value != nullptr ? value->getType() : nullptr;
                    result.TypeAndValue.TypeName = typeName;
                    // A typed null (`(int*)nullptr`) records a pointer depth and stays a pointer.
                    const bool nullPointer = pointerDepth < 1
                        && llvm::isa_and_nonnull<llvm::ConstantPointerNull>(value);
                    const bool nullType = cxxNullptrT || nullPointer;
                    const bool preserveClassValue = (hasClassConversionCandidate || byValueSame)
                        && structType != nullptr && pointerDepth == 0 && !nullType
                        && !typeName.empty() && compiler->IsCxxRecord(typeName);
                    result.TypeAndValue.Pointer = !nullType && reference && !preserveClassValue;
                    // Preserve nullptr through free-operator assembly so C++ ranking can select
                    // nullptr_t over a direct class-reference binding.
                    result.TypeAndValue.IsCxxNullptrT = nullType;
                    if (nullType) result.TypeAndValue.TypeName.clear();
                    // A SCALAR operand (`5 == ops`, `sink << ops`) reaches here with no type
                    // name, and overload resolution cannot score an unnamed argument against a
                    // candidate parameter. Name it from its machine type.
                    if (!nullType && typeName.empty() && value != nullptr)
                        result.TypeAndValue.TypeName = ScalarTypeNameForValue(value->getType());
                    if (reference && !nullType && !preserveClassValue)
                    {
                        // The argument IS the address for a reference parameter: the operand's
                        // own storage when it has any, otherwise a materialized temporary.
                        if (storage != nullptr)
                            result.Primary = storage;
                        else if (structType != nullptr)
                        {
                            auto* temp = compiler->CreateAlloca(structType);
                            compiler->CreateAssignment(value, temp);
                            result.Primary = temp;
                            result.Storage = temp;
                        }
                        result.BaseType = structType;
                    }
                    else if (structType != nullptr && storage == nullptr)
                    {
                        // A C++ class prvalue still needs storage for a by-value copy or move.
                        auto* temp = compiler->CreateAlloca(structType);
                        compiler->CreateAssignment(value, temp);
                        result.Storage = temp;
                    }
                    return result;
                };

                if (leftMutableReference && !leftBindsRvalue && freeLhsStorage == nullptr)
                    LogErrorContext(ctx, std::format(
                        "C++ '{}' cannot bind its left operand because the CFlat expression is an rvalue and the parameter is a non-const reference.",
                        opName));
                if (rightMutableReference && !rightBindsRvalue
                    && (freeRhsStorage == nullptr || rightXvalue))
                    LogErrorContext(ctx, std::format(
                        "C++ '{}' cannot bind its right operand because the CFlat expression is an rvalue and the parameter is a non-const reference.",
                        opName));

                auto leftArg = makeArgument(lvalue, leftXvalue ? lhsStorage : freeLhsStorage,
                                             leftInfo.first, leftInfo.second, leftReference,
                                             lhsPointerDepth, lhsCxxNullptrT, leftByValueSame);
                auto rightArg = makeArgument(rvalue, rightXvalue ? rhsStorage : freeRhsStorage,
                                              rightInfo.first, rightInfo.second, rightReference,
                                              rhsPointerDepth, rhsCxxNullptrT, rightByValueSame);
                setLiteralIdentity(leftArg, 0);
                setLiteralIdentity(rightArg, 1);
                // The floating operand rule (C++ binary operator scalar parameters) applies to both sides.
                leftArg.CxxOperatorFloatToInteger = true;
                rightArg.CxxOperatorFloatToInteger = true;
                // A const prvalue / xvalue keeps its cv in ConstMask bit 0: the object of a value
                // shape, the referent of the address a reference parameter receives.
                auto keepObjectConst = [](LLVMBackend::NamedVariable& arg, int objectCv) {
                    if ((objectCv & 1) != 0) arg.TypeAndValue.SetPointeeConstBit(true);
                };
                keepObjectConst(leftArg, lhsObjectCv);
                keepObjectConst(rightArg, rhsObjectCv);
                int8_t leftCategory = -1;
                int8_t rightCategory = -1;
                if (operands.size() == 2)
                {
                    leftCategory = CxxArgumentLvalueKind(operands[0]);
                    rightCategory = CxxArgumentLvalueKind(operands[1]);
                    leftArg.CxxLvalueKind = leftCategory;
                    rightArg.CxxLvalueKind = rightCategory;
                }
                const bool leftIsRvalue = leftCategory == 1 ? false : lhsIsRvalue;
                const bool rightIsRvalue = rightCategory == 1 ? false : rhsIsRvalue;
                leftArg.IsRvalue = leftIsRvalue;
                rightArg.IsRvalue = rightIsRvalue;
                if (rightXvalue)
                {
                    rightArg.IsExplicitMove = true;
                    rightArg.IsRvalue = true;
                    // Without a written `move`, the xvalue is a prvalue temp (its sret or ctor slot).
                    rightArg.IsCxxPrvalueTemp = !rhsExplicitMove;
                    rightArg.CallerName = compiler->FindVariableNameByStorage(rhsStorage);
                }
                if (leftXvalue)
                {
                    leftArg.IsExplicitMove = true;
                    leftArg.IsRvalue = true;
                    leftArg.IsCxxPrvalueTemp = !lhsExplicitMove; // T235: same as the right operand
                    leftArg.CallerName = compiler->FindVariableNameByStorage(lhsStorage);
                }
                // A written `move x` operand reaches the call as an explicit move whatever the
                // chosen parameter, so a `const T&` operator refuses it as a call would.
                if (lhsExplicitMove)
                {
                    leftArg.IsExplicitMove = true;
                    if (leftArg.CallerName.empty())
                        leftArg.CallerName = compiler->FindVariableNameByStorage(lhsStorage);
                }
                if (rhsExplicitMove)
                {
                    rightArg.IsExplicitMove = true;
                    if (rightArg.CallerName.empty())
                        rightArg.CallerName = compiler->FindVariableNameByStorage(rhsStorage);
                }
                // An enum left operand is its enum type too (`E::B / obj` binds operator/(E, T)).
                if (leftInfo.second.empty() && lvalue != nullptr && lvalue->getType()->isIntegerTy())
                {
                    if (compiler->IsScopedEnumTypeName(lhsTypeName))
                        leftArg.TypeAndValue.TypeName = lhsTypeName;
                    else if (std::string enumKey = compiler->ResolveEnumTypeName(lhsTypeName);
                             !enumKey.empty())
                    {
                        leftArg.TypeAndValue.TypeName = enumKey;
                        leftArg.TypeAndValue.EnumBacking = compiler->GetEnumBackingType(enumKey);
                    }
                }
                if (leftInfo.second.empty() && lvalue != nullptr
                    && lvalue->getType()->isPointerTy() && lhsPointerDepth >= 1)
                {
                    leftArg.TypeAndValue.Pointer = true;
                    leftArg.TypeAndValue.PointerDepth = lhsPointerDepth;
                    leftArg.TypeAndValue.ElemPointer = lhsElemPointer;
                    if (LLVMBackend::IsPrimitiveTypeName(lhsTypeName))
                    {
                        leftArg.InferSourceTypeName = lhsTypeName;
                        leftArg.TypeAndValue.SetPointeeConstBit(lhsCxxPointeeConst);
                    }
                    else if (!rightInfo.second.empty()
                             && compiler->IsCxxRecord(rightInfo.second)
                             && compiler->IsCxxRecord(lhsTypeName))
                    {
                        leftArg.TypeAndValue.TypeName = lhsTypeName;
                        leftArg.TypeAndValue.SetPointeeConstBit(lhsCxxPointeeConst);
                        leftArg.AllowCxxClassPointerCtorConversion = true;
                    }
                }
                if (rightInfo.second.empty() && rvalue != nullptr)
                {
                    // A scalar pointer names its pointee by its recorded depth, like a call argument.
                    if (rvalue->getType()->isPointerTy() && rhsPointerDepth >= 1)
                    {
                        rightArg.TypeAndValue.Pointer = true;
                        rightArg.TypeAndValue.PointerDepth = rhsPointerDepth;
                        rightArg.TypeAndValue.ElemPointer = rhsElemPointer;
                        if (LLVMBackend::IsPrimitiveTypeName(rhsTypeName))
                        {
                            rightArg.InferSourceTypeName = rhsTypeName;
                            rightArg.TypeAndValue.SetPointeeConstBit(rhsCxxPointeeConst);
                        }
                        // A C++ record pointee keeps its name and const (see makeRightNV).
                        else if (!leftInfo.second.empty() && compiler->IsCxxRecord(leftInfo.second)
                                 && compiler->IsCxxRecord(rhsTypeName))
                        {
                            rightArg.TypeAndValue.TypeName = rhsTypeName;
                            rightArg.TypeAndValue.SetPointeeConstBit(rhsCxxPointeeConst);
                            rightArg.AllowCxxClassPointerCtorConversion = true;
                        }
                    }
                    else if (rhsDepthUnproven && rightTypedPointer)
                    {
                        refuseUnprovenPointer(opName);
                        return nullptr;
                    }
                    // An enum class operand is its enum type, never the `int` it lowers to.
                    else if (rvalue->getType()->isIntegerTy()
                             && (!compiler->ResolveEnumTypeName(rhsTypeName).empty()
                                 || compiler->IsScopedEnumTypeName(rhsTypeName)
                                 || compiler->enumBackingTypes.count(rhsTypeName) != 0))
                    {
                        rightArg.TypeAndValue.TypeName = compiler->ResolveEnumTypeName(rhsTypeName);
                        if (rightArg.TypeAndValue.TypeName.empty())
                            rightArg.TypeAndValue.TypeName = rhsTypeName;
                        rightArg.TypeAndValue.EnumBacking = compiler->GetEnumBackingType(
                            rightArg.TypeAndValue.TypeName);
                        rightArg.TypeAndValue.IsScopedEnum = compiler->IsScopedEnumTypeName(
                            rightArg.TypeAndValue.TypeName);
                    }
                }
                FlushCallPostfix();
                auto isConstStorage = [&](llvm::Value* storage) {
                    if (storage == nullptr) return false;
                    // A mutable field of a const object is writable: the field access decides.
                    if (compiler->IsCxxMutableFieldAccess(storage)) return false;
                    auto* global = llvm::dyn_cast<llvm::GlobalVariable>(
                        llvm::getUnderlyingObject(storage));
                    return compiler->IsCxxConstReferent(storage)
                        || (global != nullptr && (global->isConstant()
                            || compiler->IsConstHeaderGlobal(global)));
                };
                // T85: a candidate dropped for an unrelated pointee leaves the operator template to
                // rank beside the rest; clang decides that set, as it does for an ordinary call.
                if (compiler->HasCxxFunctionTemplate(sourceName)
                    && (compiler->CxxOperatorDropsUnrelatedPointee(lookupName, leftArg, 0, {})
                        || compiler->CxxOperatorDropsUnrelatedPointee(lookupName, rightArg, 1, {})))
                {
                    if (llvm::Value* ranked = callOperatorTemplate(sourceName, true)) return ranked;
                    // Clang's own refusal of the post-drop set (an ambiguity) is the answer.
                    if (!adlClangRefusal.empty())
                    {
                        LogErrorContext(ctx, adlClangRefusal);
                        return nullptr;
                    }
                }
                const bool constLeft = isConstStorage(freeLhsStorage);
                const bool constRight = isConstStorage(freeRhsStorage);
                leftArg.TypeAndValue.SetCxxConstRef(constLeft);
                rightArg.TypeAndValue.SetCxxConstRef(constRight);
                auto table = compiler->functionTable.find(lookupName);
                const bool filterReferences = (constLeft || leftIsRvalue || constRight || rightIsRvalue)
                    && table != compiler->functionTable.end();
                std::vector<LLVMBackend::FunctionSymbol> original;
                auto hiddenByOperand = [&](const LLVMBackend::FunctionSymbol& candidate) {
                    return ((constLeft || leftIsRvalue)
                            && leftMutableReferenceLinkages.count(candidate.UniqueName) != 0)
                        || ((constRight || rightIsRvalue)
                            && rightMutableReferenceLinkages.count(candidate.UniqueName) != 0);
                };
                compiler->cxxFilteredOperatorCandidates_.clear();
                if (filterReferences)
                {
                    original = table->second;
                    // Kept for the no-overload diagnostic, which would otherwise see an empty set.
                    for (const auto& candidate : table->second)
                        if (hiddenByOperand(candidate))
                            compiler->cxxFilteredOperatorCandidates_.push_back(candidate);
                    std::erase_if(table->second, hiddenByOperand);
                }
                auto restoreConstCandidates = [&]() {
                    compiler->cxxFilteredOperatorCandidates_.clear();
                    if (!filterReferences) return;
                    auto restore = compiler->functionTable.find(lookupName);
                    if (restore == compiler->functionTable.end()) return;
                    auto afterCall = std::move(restore->second);
                    restore->second = original;
                    for (auto& appended : afterCall)
                        if (std::none_of(restore->second.begin(), restore->second.end(),
                            [&](const LLVMBackend::FunctionSymbol& existing) {
                                return existing.UniqueName == appended.UniqueName;
                            }))
                            restore->second.push_back(std::move(appended));
                };
                binaryExcludedLinkages.clear();
                binaryExcludedTable = lookupName;
                for (const auto& candidate : compiler->cxxFilteredOperatorCandidates_)
                    binaryExcludedLinkages.insert(candidate.UniqueName);
                if (!binaryExcludedLinkages.empty() && compiler->HasCxxFunctionTemplate(sourceName))
                {
                    llvm::Value* templated = nullptr;
                    try { templated = callOperatorTemplate(sourceName); }
                    catch (...) { restoreConstCandidates(); binaryExcludedLinkages.clear(); throw; }
                    binaryExcludedLinkages.clear();
                    if (templated != nullptr) { restoreConstCandidates(); return templated; }
                }
                llvm::Value* result = nullptr;
                try
                {
                    // The operator expression is the site key codegen's choice is recorded under.
                    compiler->pendingCallSite_ = ctx;
                    result = compiler->CreateOverloadedFunctionCall(
                        lookupName, { leftArg, rightArg }, true);
                    compiler->pendingCallSite_ = nullptr;
                }
                catch (...)
                {
                    compiler->pendingCallSite_ = nullptr;
                    restoreConstCandidates();
                    throw;
                }
                restoreConstCandidates();
                if (result != nullptr)
                {
                    TrackOwnedStringOperatorResult(compiler, result);
                    return result;
                }
                if (compiler->HasCxxFunctionTemplate(sourceName))
                    if (llvm::Value* templated = callOperatorTemplate(sourceName))
                        return templated;
            }
            return nullptr;
        };

        /*
         * A mixed-operand comparison against a class that declares friend operators: only its
         * homogeneous friends are registered, so the registered set is incomplete and would refuse
         * (MSVC `error_code == errc::X` needs the hidden `operator==(const error_code&, const
         * error_condition&)`). ADL - clang over the whole set - goes first.
         */
        auto mixedFriendOperands = [&]() {
            auto name = [](llvm::Value* value) {
                if (value == nullptr) return std::string();
                auto* st = llvm::dyn_cast<llvm::StructType>(value->getType());
                return st != nullptr && !st->isLiteral() && st->hasName()
                    ? st->getName().str() : std::string();
            };
            const std::string left = name(lvalue);
            const std::string right = name(rvalue);
            return left != right
                && (compiler->CxxClassDeclaresFriendOperators(left)
                    || compiler->CxxClassDeclaresFriendOperators(right));
        };
        // Pointer-LHS mixed comparison: clang decides alone; the registered homogeneous friend would
        // accept a bogus pointer -> class conversion and crash.
        auto adlDecides = [&]() {
            return (IsComparisonOperatorText(op) || op == "<=>") && mixedFriendOperands();
        };

        /*
         * C++20 REWRITTEN relational candidate: a class that declares only `operator<=>` still
         * answers `a < b`, as `(a <=> b) < 0`. Every ordering category is a class holding one
         * signed member (libc++: `signed char __value_`, -1 / 0 / 1, plus -127 for
         * partial_ordering's `unordered`), so the ordering is lowered by reading that single
         * member and comparing it against zero. That is ABI-safe because the member is the
         * object's whole representation - the alternative, calling the ordering's own comparison
         * operators, is impossible: they are constexpr friends with no emitted symbol to bind.
         */
        auto rewriteFromSpaceship = [&]() -> llvm::Value* {
            const SpaceshipRelation* relation = nullptr;
            for (const auto& candidate : kSpaceshipRelations)
                if (op == candidate.spelling) { relation = &candidate; break; }
            if (relation == nullptr) return nullptr;
            RewriteDepthScope rewriteScope(cxxOperatorRewriteDepth_);
            lastRewriteWasReversed_ = false;
            llvm::Value* ordering = TryBinaryOperatorOverload(
                lvalue, "<=>", rvalue, ctx, lhsElemType, rhsPointerDepth, rhsElemPointer,
                lhsStorage, rhsStorage, false, allowReversed, lhsIsRvalue, rhsIsRvalue,
                lhsTypeName, rhsTypeName, lhsPointerDepth, lhsElemPointer,
                {}, {}, false, {}, {}, false, {}, {}, rhsCxxPointeeConst, lhsArithIdentity, rhsArithIdentity, rhsExplicitMove, lhsExplicitMove,
                lhsCxxNullptrT, rhsCxxNullptrT, lhsMaterializedPrvalue,
                lhsCxxPointeeConst, lhsObjectCv, rhsObjectCv);
            if (ordering == nullptr) return nullptr;
            // The ordering came from `rvalue <=> lvalue`: the relation applies with 0 on the left.
            if (lastRewriteWasReversed_)
            {
                const std::string_view mirror = MirrorRelationSpelling(op);
                for (const auto& candidate : kSpaceshipRelations)
                    if (mirror == candidate.spelling) relation = &candidate;
            }
            // The type mapper lowers every comparison category to that single signed byte; a
            // one-field wrapper is unwrapped here for a `<=>` that returns something else. An
            // ordering laid out as an opaque one-byte BLOB (`{ [1 x i8] }`, which is how an
            // import group that never requested the category spells it) unwraps the same way.
            while (ordering->getType()->isStructTy() || ordering->getType()->isArrayTy())
            {
                if (auto* st = llvm::dyn_cast<llvm::StructType>(ordering->getType()))
                {
                    if (st->getNumElements() != 1) return nullptr;
                }
                else if (llvm::cast<llvm::ArrayType>(ordering->getType())->getNumElements() != 1)
                    return nullptr;
                ordering = compiler->builder->CreateExtractValue(ordering, { 0 }, "ordval");
            }
            if (!ordering->getType()->isIntegerTy()) return nullptr;
            // `low <= value <= high` as one unsigned range test, so an `unordered` value falls
            // outside every relation instead of being read as "less" by a sign test.
            llvm::Type* orderingType = ordering->getType();
            llvm::Value* shifted = compiler->builder->CreateSub(
                ordering, llvm::ConstantInt::get(orderingType, relation->low, /*IsSigned=*/true),
                "ordbase");
            return compiler->builder->CreateICmpULT(
                shifted, llvm::ConstantInt::get(orderingType, relation->high - relation->low + 1),
                "ordcmp");
        };

        // C++20 REVERSED candidate: `b == a` is considered when only `a == b` is declared, which
        // is how a member `operator==(const Other&)` answers `other == self`. C++20 reverses
        // `==` and `<=>` ONLY - `!=` is REWRITTEN from `==` (negateEquality above) and has no
        // reversed form, so reversing it would answer an asymmetric `operator!=(A, B)` for
        // `b != a` with the operands swapped, silently returning the wrong result. The rewrite
        // is also limited to imported C++ operands: a native CFlat overload set keeps its
        // stricter "no operator" diagnostic.
        auto reversedCandidate = [&]() -> llvm::Value* {
            if (!allowReversed || rvalue == nullptr) return nullptr;
            if (op != "==" && op != "<=>") return nullptr;
            auto isCxxOperand = [&](llvm::Value* value) {
                if (value == nullptr || !value->getType()->isStructTy()) return false;
                auto* st = llvm::cast<llvm::StructType>(value->getType());
                if (st->isLiteral() || !st->hasName()) return false;
                return compiler->IsCxxRecord(st->getName().str());
            };
            if (!isCxxOperand(lvalue) && !isCxxOperand(rvalue)) return nullptr;
            RewriteDepthScope rewriteScope(cxxOperatorRewriteDepth_);
            llvm::Value* reversed = TryBinaryOperatorOverload(
                rvalue, op, lvalue, ctx, nullptr,
                lhsPointerDepth, lhsElemPointer,
                rhsStorage, lhsStorage, false, false,
                rhsIsRvalue, lhsIsRvalue,
                rhsTypeName, lhsTypeName,
                rhsPointerDepth, rhsElemPointer,
                {}, {}, false, {}, {}, false, {}, {}, lhsCxxPointeeConst,
                rhsArithIdentity, lhsArithIdentity, lhsExplicitMove, rhsExplicitMove,
                rhsCxxNullptrT, lhsCxxNullptrT, false, rhsCxxPointeeConst,
                rhsObjectCv, lhsObjectCv);
            // Read by rewriteFromSpaceship: its ordering is `rvalue <=> lvalue`.
            if (reversed != nullptr) lastRewriteWasReversed_ = true;
            return reversed;
        };

        // Every C++20 rewrite, in the order the standard considers them.
        auto tryRewrites = [&]() -> llvm::Value* {
            if (llvm::Value* rewritten = negateEquality())      return rewritten;
            if (llvm::Value* rewritten = rewriteFromSpaceship()) return rewritten;
            if (llvm::Value* rewritten = reversedCandidate())    return rewritten;
            return nullptr;
        };

        auto hasCxxMemberOperator = [&](const std::string& recordName,
                                        const std::string& operatorName) {
            if (recordName.empty() || !compiler->IsCxxRecord(recordName)) return false;
            compiler->EnsureCxxMemberProjected(recordName, operatorName);
            auto it = compiler->functionTable.find(operatorName);
            if (it == compiler->functionTable.end()) return false;
            for (const auto& candidate : it->second)
                if (candidate.IsCxx && isMemberCandidate(candidate)
                    && candidate.Parameters.size() == 2
                    && (candidate.Parameters[0].TypeName == recordName
                        || compiler->IsCxxBaseOf(candidate.Parameters[0].TypeName, recordName)))
                    return true;
            return false;
        };
        auto isCxxCompoundOperator = [&](std::string_view candidate) {
            return candidate != "??="
                && std::any_of(kCompoundOperators.begin(), kCompoundOperators.end(),
                    [&](const auto& compound) { return compound.first == candidate; });
        };
        auto relayCxxCompoundRefusal = [&]() -> llvm::Value* {
            // Const-write bypass: hand every no-viable path back to RefuseConstWrite.
            if (refuseConstWriteOnNoViable) return nullptr;
            std::string diagnostic = adlClangRefusal;
            if (diagnostic.empty())
                diagnostic = std::format("clang: no viable overloaded '{}'", "operator" + op);
            else if (!diagnostic.starts_with("clang: "))
                diagnostic = "clang: " + diagnostic;
            diagnostic += std::format("; no overload of 'operator{}' matches the given arguments", op);
            if (lhsMaterializedPrvalue)
                diagnostic += "; Left side of assignment is not an addressable lvalue";
            if (isCxxRecordValue(lvalue))
            {
                const std::string leftName =
                    llvm::cast<llvm::StructType>(lvalue->getType())->getName().str();
                const LLVMBackend::CxxClassInfo* info = compiler->GetCxxClassInfo(leftName);
                if (info != nullptr)
                    if (auto access = info->memberAccess.find("operator" + op);
                        access != info->memberAccess.end()
                        && access->second == cflat_cinterop::AccessProtected
                        && !compiler->CxxProtectedAccessAllowed(leftName, lhsCallerName == "this"))
                        diagnostic += std::format("; member '{}' of C++ class '{}' is protected",
                            "operator" + op, compiler->DisplayCxxClassName(leftName));
            }
            LogErrorContext(ctx, diagnostic);
            return nullptr;
        };
        auto tryClangForCxx20CandidateSet = [&]() -> llvm::Value* {
            if (cxxOperatorRewriteDepth_ != 0 || cxxAdlSuppressDepth_ != 0) return nullptr;
            // A scalar-left equality needs C++20's reversed candidates in the same set.
            if (op == "==" && !isCxxRecordValue(lvalue) && isCxxRecordValue(rvalue))
                return tryFreeOperator(true, true);
            auto hasMemberCandidate = [&](const std::string& className,
                                          const std::string& operatorName) {
                compiler->EnsureCxxMemberProjected(className, operatorName);
                if (auto it = compiler->functionTable.find(operatorName);
                    it != compiler->functionTable.end())
                    for (const auto& candidate : it->second)
                        if (candidate.IsCxx && isMemberCandidate(candidate)
                            && candidate.Parameters.size() == 2
                            && candidate.Parameters[0].TypeName == className)
                            return true;
                return false;
            };
            auto hasFreeTemplateCandidate = [&](const std::string& className,
                                                const std::string& operatorName) {
                for (const std::string& source : CxxFreeOperatorSources(className, operatorName))
                    if (compiler->HasCxxFunctionTemplate(source)) return true;
                return false;
            };
            auto hasFreeCandidate = [&](const std::string& className,
                                        const std::string& operatorName) {
                for (const std::string& source : CxxFreeOperatorSources(className, operatorName))
                {
                    compiler->TryBindCxxFunction(source);
                    const std::string lookup = source == operatorName
                        ? "__cxx_free." + operatorName : source;
                    if (auto it = compiler->functionTable.find(lookup);
                        it != compiler->functionTable.end())
                        for (const auto& candidate : it->second)
                            if (isFreeCxxCandidate(candidate)
                                && candidate.SourceName == source
                                && candidate.Parameters.size() == 2
                                && candidate.Parameters[0].TypeName == className)
                                return true;
                }
                return false;
            };
            /*
             * A free operator in the class's namespace and one at global scope are ONE C++
             * candidate set (ordinary lookup plus ADL). The namespace loop returns the first set
             * that matches, so clang decides when both sets hold a candidate for this operator.
             */
            auto sourceHasFreeCandidate = [&](const std::string& className,
                                              const std::string& source,
                                              const std::string& operatorName) {
                // Registry tests first: a source with no signatures binds nothing (no request).
                if (compiler->HasCxxFunctionTemplate(source)) return true;
                if (compiler->cxxFunctionSignatures_.count(source) == 0) return false;
                compiler->TryBindCxxFunction(source);
                const std::string lookup = source == operatorName
                    ? "__cxx_free." + operatorName : source;
                if (auto it = compiler->functionTable.find(lookup);
                    it != compiler->functionTable.end())
                    for (const auto& candidate : it->second)
                        if (isFreeCxxCandidate(candidate)
                            && candidate.SourceName == source
                            && candidate.Parameters.size() == 2
                            && candidate.Parameters[0].TypeName == className)
                            return true;
                return false;
            };
            if (isCxxRecordValue(lvalue) && !(isCxxCompoundOperator(op) && lhsIsRvalue))
            {
                const std::string leftName =
                    llvm::cast<llvm::StructType>(lvalue->getType())->getName().str();
                const std::string operatorName = "operator" + op;
                const std::vector<std::string> sources = CxxFreeOperatorSources(leftName, operatorName);
                // A member beside the free set keeps its own member-free tie diagnostic below.
                if (sources.size() == 2 && !hasMemberCandidate(leftName, operatorName)
                    && sourceHasFreeCandidate(leftName, sources[0], operatorName)
                    && sourceHasFreeCandidate(leftName, sources[1], operatorName))
                {
                    adlClangRefusal.clear();
                    if (llvm::Value* resolved = tryFreeOperator(true, true)) return resolved;
                    if (adlClangAmbiguous) return nullptr;
                    if (!adlClangRefusal.empty())
                    {
                        LogErrorContext(ctx, adlClangRefusal);
                        return nullptr;
                    }
                }
            }
            // An unregistered free function template must rank beside a member. When both are
            // present, clang resolves the complete candidate set; the registered set is incomplete.
            if (isCxxRecordValue(lvalue) && !isCxxCompoundOperator(op))
            {
                const std::string leftName =
                    llvm::cast<llvm::StructType>(lvalue->getType())->getName().str();
                const std::string operatorName = "operator" + op;
                if (hasFreeTemplateCandidate(leftName, operatorName)
                    && hasMemberCandidate(leftName, operatorName))
                {
                    const bool hasExactFree = hasFreeCandidate(leftName, operatorName);
                    adlClangRefusal.clear();
                    if (llvm::Value* resolved = tryFreeOperator(true, true, hasExactFree)) return resolved;
                    if (adlClangAmbiguous) return nullptr;
                    if (!adlClangRefusal.empty())
                    {
                        LogErrorContext(ctx, adlClangRefusal);
                        return nullptr;
                    }
                }
            }
            if (isCxxCompoundOperator(op) && isCxxRecordValue(lvalue) && !lhsIsRvalue)
            {
                const std::string opName = "operator" + op;
                const std::string leftName =
                    llvm::cast<llvm::StructType>(lvalue->getType())->getName().str();
                if (hasMemberCandidate(leftName, opName)
                    && (hasFreeCandidate(leftName, opName)
                        || hasFreeTemplateCandidate(leftName, opName)))
                    if (llvm::Value* resolved = tryFreeOperator(true, true, true)) return resolved;
                bool inheritedMember = false;
                if (auto it = compiler->functionTable.find(opName);
                    it != compiler->functionTable.end())
                    for (const auto& candidate : it->second)
                        if (candidate.IsCxx && isMemberCandidate(candidate)
                            && candidate.Parameters.size() == 2
                            && candidate.Parameters[0].TypeName != leftName
                            && compiler->IsCxxBaseOf(candidate.Parameters[0].TypeName, leftName))
                        {
                            inheritedMember = true;
                            break;
                        }
                const std::string base = leftName.substr(0, leftName.find('$'));
                const size_t dot = base.rfind('.');
                if (dot != std::string::npos)
                {
                    const std::string freeName = base.substr(0, dot) + "." + opName;
                    compiler->TryBindCxxFunction(freeName);
                    if (inheritedMember)
                        if (auto it = compiler->functionTable.find(freeName);
                            it != compiler->functionTable.end())
                            for (const auto& candidate : it->second)
                                if (isFreeCxxCandidate(candidate)
                                    && candidate.SourceName == freeName
                                    && candidate.Parameters.size() == 2
                                    && (candidate.Parameters[0].TypeName == leftName
                                        || compiler->IsCxxBaseOf(
                                            candidate.Parameters[0].TypeName, leftName)))
                                    return tryFreeOperator(true, true);
                }
            }
            if (op != "<" && op != "<=" && op != ">" && op != ">=") return nullptr;
            const std::string leftName = isCxxRecordValue(lvalue)
                ? llvm::cast<llvm::StructType>(lvalue->getType())->getName().str() : std::string();
            const std::string rightName = isCxxRecordValue(rvalue)
                ? llvm::cast<llvm::StructType>(rvalue->getType())->getName().str() : std::string();
            if (hasCxxMemberOperator(leftName, "operator<=>")
                || hasCxxMemberOperator(rightName, "operator<=>"))
                return tryFreeOperator(true, true, true);
            return nullptr;
        };

        auto reportNoOperator = [&](const std::string& typeName) -> llvm::Value* {
            if (ctx != nullptr && ctx == cxxAdlAmbiguousCtx_) return nullptr;
            // A C++ record with no viable member/free operator may still reach a built-in
            // operator through one implicit conversion function. Keep this after operator
            // lookup so a real C++ overload always wins, and never apply it to CFlat structs.
            auto tryImplicitBuiltinConversion = [&]() -> llvm::Value* {
                static const std::set<std::string> builtinOperators = {
                    "+", "-", "*", "/", "%", "<<", ">>", "&", "|", "^",
                    "==", "!=", "<", "<=", ">", ">=", "&&", "||"
                };
                if (builtinOperators.count(op) == 0) return nullptr;
                auto trySide = [&](llvm::Value* value, llvm::Value* other,
                                   const std::string& sourceName,
                                   const std::string& otherName,
                                   bool convertLeft) -> llvm::Value* {
                    if (value == nullptr || other == nullptr || sourceName.empty()
                        || (!compiler->IsCxxRecord(sourceName)
                            && compiler->generatedCxxRecords_.count(sourceName) == 0)
                        || (!other->getType()->isIntegerTy()
                            && !other->getType()->isFloatingPointTy()))
                        return nullptr;
                    std::string targetName = otherName.empty()
                        ? ScalarTypeNameForValue(other->getType()) : otherName;
                    if (targetName.empty() || !LLVMBackend::IsPrimitiveTypeName(targetName))
                        return nullptr;
                    compiler->EnsureCxxConversionOperatorsProjected(sourceName);
                    if (const auto* info = compiler->GetCxxClassInfo(sourceName))
                    {
                        std::vector<std::string> conversions;
                        for (const auto& method : info->directMethods)
                            if (method.spellable && method.raw.isConversion && !method.raw.isExplicit
                                && !method.raw.isDeleted && method.raw.bindRefusal.empty()
                                && method.ret.IsPrimitive())
                                // Any arithmetic result competes for `%` and the bitwise operators too (clang).
                                conversions.push_back(method.raw.name);
                        std::sort(conversions.begin(), conversions.end());
                        conversions.erase(std::unique(conversions.begin(), conversions.end()),
                                          conversions.end());
                        if (conversions.size() > 1)
                        {
                            std::string candidates;
                            for (const auto& name : conversions)
                                candidates += (candidates.empty() ? std::string() : std::string(" and "))
                                    + "'" + name + "'";
                            LogErrorContext(ctx, std::format(
                                "ambiguous implicit conversion for built-in operator '{}': {} "
                                "are viable conversion functions",
                                op, candidates));
                        }
                    }
                    // Lands in the conversion's own return type (clang: `d + 2` is double + int);
                    // targetName stays the other operand's type for signedness.
                    std::string convertedName = targetName;
                    if (const auto* convInfo = compiler->GetCxxClassInfo(sourceName))
                        for (const auto& method : convInfo->directMethods)
                            if (method.spellable && method.raw.isConversion && !method.raw.isExplicit
                                && !method.raw.isDeleted && method.raw.bindRefusal.empty()
                                && method.ret.IsPrimitive()
                                && (!method.ret.Pointer || method.raw.retType.ends_with('&')))
                            {
                                // A scalar `T&` return is a pointer until ApplyCxxConversionOperator loads it.
                                convertedName = method.ret.TypeName;
                                break;
                            }
                    LLVMBackend::TypeAndValue target;
                    target.TypeName = convertedName;
                    llvm::Value* converted = compiler->ConvertViaImplicitConversionOperator(
                        value, target);
                    if (converted == nullptr) return nullptr;
                    // C++ integral promotion: a narrower-than-int result (or bool) becomes int first.
                    if (auto* narrow = llvm::dyn_cast<llvm::IntegerType>(converted->getType());
                        narrow != nullptr && narrow->getBitWidth() < 32)
                    {
                        const bool zeroExtend = narrow->getBitWidth() == 1
                            || target.IsUnsignedInteger() != -1;
                        converted = zeroExtend
                            ? compiler->builder->CreateZExt(converted, compiler->builder->getInt32Ty())
                            : compiler->builder->CreateSExt(converted, compiler->builder->getInt32Ty());
                        target.TypeName = "int";
                    }
                    LLVMBackend::TypeAndValue otherType;
                    otherType.TypeName = targetName;
                    const bool otherUnsigned = otherType.IsUnsignedInteger() != -1;
                    if (convertLeft)
                        return compiler->CreateOperation(op, converted, other,
                            target.IsUnsignedInteger() != -1, otherUnsigned);
                    return compiler->CreateOperation(op, other, converted,
                        otherUnsigned, target.IsUnsignedInteger() != -1);
                };
                if (llvm::Value* converted = trySide(lvalue, rvalue, lhsTypeName,
                                                     rhsTypeName, true))
                    return converted;
                return trySide(rvalue, lvalue, rhsTypeName, lhsTypeName, false);
            };
            /*
             * A class that may hide an unregistered friend / ADL operator asks clang FIRST: an
             * exact-match friend beats the builtin reached through a user-defined conversion,
             * and clang itself picks that builtin when it is the best candidate. A class that
             * can hide nothing never issues the request (CxxClassMayHaveUnregisteredOperators).
             */
            if (llvm::Value* adl = tryFreeOperator(true)) return adl;
            if (llvm::Value* converted = tryImplicitBuiltinConversion()) return converted;
            if (!reportMissing || typeName.empty()
                || typeName == "__iface_fat_ptr" || typeName == "__closure_fat_ptr")
                return nullptr;
            if (op == "&" || op == "|" || op == "^")
            {
                LogErrorContext(ctx, std::format(
                    "no overload of 'operator{}' matches the given arguments.", op));
                return nullptr;
            }
            {
                if (const std::string error = compiler->CxxDeletedOperatorError(typeName, op);
                    !error.empty())
                    LogErrorContext(ctx, error);
                std::string shown = SpellType(*compiler,
                    LLVMBackend::TypeAndValue{ .TypeName = typeName });
                if (shown.empty()) shown = typeName;
                if (std::string note = compiler->CxxMemberThroughNonPublicBaseNote(typeName, "operator" + op);
                    !note.empty())
                {
                    LogErrorContext(ctx, std::format("no operator '{}' for type '{}': {}", op, shown, note));
                    return nullptr;
                }
                LogErrorContext(ctx, std::format("no operator '{}' for type '{}'", op, shown));
            }
            return nullptr;
        };

        if (ty->isPointerTy())
        {
            // An unwrapped left literal against a C++ right operand: its only candidates are the
            // free operators of that operand's namespace, which the struct paths below never see.
            // Nothing bound there -> wrap after all and re-enter, so an operator that stays
            // unmatched keeps the CFlat 'string' diagnostic and its candidate list. This runs
            // BEFORE the pointer-LHS path, which reports a struct RHS as a pointer misuse.
            if (lhsStringLiteral)
            {
                if (adlDecides())
                {
                    if (llvm::Value* adl = tryFreeOperator(true)) return adl;
                }
                else if (llvm::Value* freeResult = tryFreeOperator()) return freeResult;
                return TryBinaryOperatorOverload(
                    compiler->WrapStringLiteralAsString(lvalue), op, rvalue, ctx, lhsElemType,
                    rhsPointerDepth, rhsElemPointer, lhsStorage, rhsStorage, reportMissing,
                    allowReversed, lhsIsRvalue, rhsIsRvalue, lhsTypeName, rhsTypeName,
                    lhsPointerDepth, lhsElemPointer,
                    {}, {}, false, {}, {}, false, {}, {}, rhsCxxPointeeConst, lhsArithIdentity, rhsArithIdentity, rhsExplicitMove, lhsExplicitMove,
                    lhsCxxNullptrT, rhsCxxNullptrT, lhsMaterializedPrvalue,
                    lhsCxxPointeeConst, lhsObjectCv, rhsObjectCv);
            }
            bool triedCxxComparisonRewrites = false;
            if (isCxxRecordValue(rvalue))
            {
                if (adlDecides())
                {
                    if (llvm::Value* adl = tryFreeOperator(true)) return adl;
                }
                else if (llvm::Value* freeResult = tryFreeOperator()) return freeResult;
                if (IsComparisonOperatorText(op))
                {
                    triedCxxComparisonRewrites = true;
                    if (llvm::Value* rewritten = tryRewrites()) return rewritten;
                }
            }
            if (auto* bound = TryPointerLhsOperatorOverload(lvalue, op, rvalue, ctx, lhsElemType))
                return bound;
            if (!triedCxxComparisonRewrites)
                if (llvm::Value* rewritten = tryRewrites()) return rewritten;
            return nullptr;
        }

        if (!ty->isStructTy())
        {
            if (llvm::Value* exact = tryClangForCxx20CandidateSet()) return exact;
            if (mixedFriendOperands())
                if (llvm::Value* adl = tryFreeOperator(true)) return adl;
            llvm::Value* freeResult = tryFreeOperator();
            if (freeResult != nullptr) return freeResult;
            if (cxxCompoundStrictLookup && !lvalue->getType()->isStructTy()
                && isCxxRecordValue(rvalue))
            {
                adlClangRefusal.clear();
                if (llvm::Value* clangResult = tryFreeOperator(true, true)) return clangResult;
                return relayCxxCompoundRefusal();
            }
            // A scalar left operand still reaches a class operator through the C++20 reversed
            // candidate: `5 == ops` selects the member `operator==(int)` with swapped operands.
            if (llvm::Value* rewritten = tryRewrites()) return rewritten;
            if (rvalue != nullptr && rvalue->getType()->isStructTy())
            {
                auto* rhsStruct = llvm::cast<llvm::StructType>(rvalue->getType());
                if (!rhsStruct->isLiteral() && rhsStruct->hasName())
                    return reportNoOperator(rhsStruct->getName().str());
            }
            return nullptr;
        }
        auto* structTy = llvm::cast<llvm::StructType>(ty);
        if (structTy->isLiteral() || !structTy->hasName()) return nullptr;
        std::string typeName = structTy->getName().str();
        // Fat-ptr types compare by extracted field - let CreateOperation handle it.
        if (typeName == "__iface_fat_ptr" || typeName == "__closure_fat_ptr") return nullptr;

        if (llvm::Value* exact = tryClangForCxx20CandidateSet()) return exact;

        std::string opName = "operator" + op;
        auto isReceiverCandidate = [&](const LLVMBackend::FunctionSymbol& candidate) {
            const bool cxxFree = isFreeCxxCandidate(candidate);
            return candidate.SourceName == opName && !cxxFree;
        };
        if ((op == "+=" || op == "-=" || op == "*=" || op == "/=" || op == "%="
             || op == "<<=" || op == ">>=" || op == "&=" || op == "|=" || op == "^=")
            && compiler->IsCxxRecord(typeName)
            && !HasOperatorOverloadForFirstParam(opName, typeName))
            if (llvm::Value* freeResult = tryFreeOperator()) return freeResult;
        if (compiler->IsCxxRecord(typeName))
            compiler->EnsureCxxMemberProjected(typeName, opName);
        if (compiler->IsCxxRecord(rhsTypeName))
            compiler->EnsureCxxMemberProjected(rhsTypeName, opName);
        if (compiler->IsCxxRecord(typeName))
            compiler->TryBindRefusedCxxBaseMember(typeName, opName);
        auto isMutableReferenceParam = [&](const LLVMBackend::FunctionSymbol& candidate,
                                           size_t index) {
            if (!candidate.IsCxx) return false;
            auto rawIt = compiler->cxxFunctionSignatures_.find(candidate.SourceName);
            if (rawIt != compiler->cxxFunctionSignatures_.end())
                for (const auto& raw : rawIt->second)
                    if (raw.linkageName == candidate.UniqueName
                        && index < raw.paramSpellings.size())
                    {
                        const auto& spelling = raw.paramSpellings[index];
                        return spelling.find('&') != std::string::npos
                            && spelling.find("&&") == std::string::npos
                            && spelling.find("const") == std::string::npos;
                    }
            return index < candidate.Parameters.size()
                && candidate.Parameters[index].IsAlias
                && !candidate.Parameters[index].CxxConstRef()
                && !candidate.Parameters[index].IsRvalueRef
                && !candidate.Parameters[index].IsCxxRefToPointer;
        };
        if (!compiler->GetFunction(opName))
        {
            if (cxxCompoundStrictLookup && isCxxRecordValue(lvalue)
                && cxxCompoundBinaryFallbackMayBind())
            {
                if (llvm::Value* freeResult = tryFreeOperator()) return freeResult;
                adlClangRefusal.clear();
                if (llvm::Value* clangResult = tryFreeOperator(true, true)) return clangResult;
                return relayCxxCompoundRefusal();
            }
            if (llvm::Value* adl = tryFreeOperator(true)) return adl;
            llvm::Value* freeResult = tryFreeOperator();
            if (freeResult != nullptr) return freeResult;
            if (llvm::Value* rewritten = tryRewrites()) return rewritten;
            return reportNoOperator(typeName);
        }

        bool receiverFound = false;
        bool binaryReceiverFound = false;
        bool receiverConsumes = false;
        bool receiverSinkConsumes = false;
        if (auto it = compiler->functionTable.find(opName); it != compiler->functionTable.end())
            for (const auto& candidate : it->second)
                if (isReceiverCandidate(candidate) && !candidate.Parameters.empty()
                    && candidate.Parameters[0].TypeName == typeName)
                {
                    receiverFound = true;
                    if (candidate.Parameters.size() == 2) binaryReceiverFound = true;
                    if (!candidate.Parameters[0].Pointer && candidate.Parameters[0].IsMove)
                        receiverConsumes = true;
                    // A by-value left operand the body consumes (an inferred sink, `return x;`).
                    const auto& param = candidate.Parameters[0];
                    const bool aliasReturnSlot = !candidate.IsCxx
                        && compiler->ParamIsAliasReturnSlot(candidate.ReturnType, param);
                    LLVMBackend::NamedVariable receiverArg;
                    receiverArg.Primary = lvalue;
                    receiverArg.BaseType = structTy;
                    receiverArg.TypeAndValue.TypeName = typeName;
                    receiverArg.TypeAndValue.Pointer = false;
                    receiverArg.Storage = lhsStorage;
                    receiverArg.CallerName = lhsCallerName;
                    receiverArg.FieldName = lhsFieldName;
                    receiverArg.IsElementAccess = lhsIsElementAccess;
                    receiverArg.IsRvalue = lhsIsRvalue;
                    const bool sinkConsumes = aliasReturnSlot
                        ? compiler->BorrowedOwningStructTempQualifies(receiverArg)
                        : compiler->OwningSinkConsumesConcrete(param);
                    if (!param.Pointer && sinkConsumes
                        && compiler->IsOwningValueType(param.TypeName))
                        receiverSinkConsumes = true;
                }
        // Only a unary member of this name (MSVC valarray's `operator+()`): it does not hide the
        // namespace-scope binary templates. Nothing free binds -> the member path reports.
        if (receiverFound && !binaryReceiverFound && compiler->IsCxxRecord(typeName))
        {
            if (llvm::Value* adl = tryFreeOperator(true)) return adl;
            if (llvm::Value* freeResult = tryFreeOperator()) return freeResult;
        }
        // A C++ iterator often has only the member `operator-(difference_type)`, while the
        // iterator-pair subtraction is a hidden friend. Let clang resolve the full infix set
        // when no member accepts the other class directly; keep matching class-member operators
        // on the existing path.
        auto isBinaryArithmeticOperator = [&]() {
            return op == "+" || op == "-" || op == "*" || op == "/" || op == "%";
        };
        if (isBinaryArithmeticOperator() && compiler->IsCxxRecord(typeName)
            && isCxxRecordValue(rvalue))
        {
            const std::string rhsClassType =
                llvm::cast<llvm::StructType>(rvalue->getType())->getName().str();
            bool memberAcceptsClassRhs = false;
            if (auto it = compiler->functionTable.find(opName); it != compiler->functionTable.end())
                for (const auto& candidate : it->second)
                    if (isReceiverCandidate(candidate) && candidate.Parameters.size() >= 2
                        && candidate.Parameters[0].TypeName == typeName)
                    {
                        const std::string& paramType = candidate.Parameters[1].TypeName;
                        if (paramType == rhsClassType
                            || (compiler->IsCxxRecord(paramType)
                                && compiler->IsCxxBaseOf(paramType, rhsClassType)))
                        {
                            memberAcceptsClassRhs = true;
                            break;
                        }
                    }
            if (!memberAcceptsClassRhs)
            {
                if (llvm::Value* adl = tryFreeOperator(true)) return adl;
                if (llvm::Value* freeResult = tryFreeOperator()) return freeResult;
            }
        }
        if (!receiverFound)
        {
            if (cxxCompoundStrictLookup && isCxxRecordValue(lvalue)
                && cxxCompoundBinaryFallbackMayBind())
            {
                if (llvm::Value* freeResult = tryFreeOperator()) return freeResult;
                adlClangRefusal.clear();
                if (llvm::Value* clangResult = tryFreeOperator(true, true)) return clangResult;
                return relayCxxCompoundRefusal();
            }
            if (mixedFriendOperands())
                if (llvm::Value* adl = tryFreeOperator(true)) return adl;
            llvm::Value* freeResult = tryFreeOperator();
            if (freeResult != nullptr) return freeResult;
            // A C++20 rewrite (`a != b` -> `!(a == b)`) is a declared operator; it beats operator bool.
            if (llvm::Value* rewritten = tryRewrites()) return rewritten;
            if (llvm::Value* converted = tryBoolComparisonConversion()) return converted;
            return reportNoOperator(typeName);
        }
        // A ternary PHI inside a call argument is either covered per arm or by this operator;
        // keep the receiver's cleanup identity single-source.
        bool receiverArmsAlreadyRegistered = inCallArgument_ && llvm::isa<llvm::PHINode>(lvalue)
            && !ParseTreeContainsTernary(ctx);

        // Determine whether to pass lvalue by pointer or by value by inspecting the
        // registered candidates - check if any candidate's first param is a pointer to
        // this struct type. If so use pointer dispatch; otherwise use value dispatch.
        bool usePointer = false;
        bool lhsMutableReference = false;
        bool rhsMutableReference = false;
        bool lhsBindsRvalue = false;
        bool rhsBindsRvalue = false;
        {
            auto funcSym = compiler->functionTable.find(opName);
            if (funcSym != compiler->functionTable.end())
            {
                for (const auto& candidate : funcSym->second)
                {
                    if (isReceiverCandidate(candidate) && !candidate.Parameters.empty()
                        && candidate.Parameters[0].TypeName == typeName
                        && candidate.Parameters[0].Pointer)
                    {
                        usePointer = true;
                        break;
                    }
                }
            }
        }
        bool receiverTempAlreadyRegistered = receiverArmsAlreadyRegistered;
        // A by-VALUE receiver that is not a ternary join is registered by the call itself
        // (CreateOverloadedFunctionCall); registering it here too destroyed a temp twice.
        if (!receiverConsumes && !receiverSinkConsumes && !receiverArmsAlreadyRegistered
            && (usePointer || llvm::isa<llvm::PHINode>(lvalue)))
        {
            LLVMBackend::NamedVariable receiverNV;
            receiverNV.Primary = lvalue;
            receiverNV.BaseType = structTy;
            receiverNV.TypeAndValue.TypeName = typeName;
            compiler->RegisterBorrowedOwningStructTemp(receiverNV, true);
            receiverTempAlreadyRegistered = true;
        }

        bool rhsConsumes = false;
        bool rhsIsAlias = false;
        bool rhsXvalue = false;
        LLVMBackend::NamedVariable rhsArg;
        rhsArg.Primary = rvalue;
        rhsArg.BaseType = rvalue ? rvalue->getType() : nullptr;
        rhsArg.Storage = rhsStorage;
        rhsArg.TypeAndValue.TypeName = rhsTypeName;
        rhsArg.TypeAndValue.Pointer = false;
        if (rvalue && rvalue->getType()->isStructTy())
        {
            auto* rhsStruct = llvm::cast<llvm::StructType>(rvalue->getType());
            if (!rhsStruct->isLiteral() && rhsStruct->hasName())
                rhsArg.TypeAndValue.TypeName = rhsStruct->getName().str();
        }
        if (auto it = compiler->functionTable.find(opName); it != compiler->functionTable.end())
        {
            for (const auto& candidate : it->second)
            {
                if (!isReceiverCandidate(candidate) || candidate.Parameters.size() < 2
                    || candidate.Parameters[0].TypeName != typeName
                    || candidate.Parameters[0].Pointer != usePointer)
                    continue;
                const auto& rhsParam = candidate.Parameters[1];
                rhsIsAlias = rhsIsAlias || compiler->ParameterIsAliasByPointer(rhsParam);
                rhsXvalue = rhsXvalue || rhsXvalueSink(rhsParam);
                if (rhsParam.IsMove
                    || (compiler->OwningSinkConsumesConcrete(rhsParam)
                        && (rhsParam.TypeName == "string"
                            || compiler->IsOwningValueType(rhsParam.TypeName)))
                    || (!candidate.IsCxx
                        && compiler->ParamIsAliasReturnSlot(candidate.ReturnType, rhsParam)
                        && compiler->BorrowedOwningStructTempQualifies(rhsArg)))
                    rhsConsumes = true;
                // The receiver is an implicit object, not a free non-const reference parameter.
                // Its ref-qualifier is checked when the member call is emitted.
                lhsBindsRvalue = true;
                if (isMutableReferenceParam(candidate, 1)) rhsMutableReference = true;
                else rhsBindsRvalue = true;
            }
        }

        // A class that declares this operator for its own receiver resolves through it, as in
        // C++; only a class WITHOUT one (the vector<bool> proxy) compares through operator bool.
        bool declaresOperator = false;
        if (auto it = compiler->functionTable.find(opName); it != compiler->functionTable.end())
            for (const auto& candidate : it->second)
                if (isReceiverCandidate(candidate) && candidate.Parameters.size() >= 2
                    && candidate.Parameters[0].TypeName == typeName
                    && candidate.Parameters[0].Pointer == usePointer)
                {
                    declaresOperator = true;
                    break;
                }

        if (!declaresOperator)
            if (llvm::Value* converted = tryBoolComparisonConversion()) return converted;

        if (lhsIsRvalue && lhsMutableReference && !lhsBindsRvalue)
            LogErrorContext(ctx, std::format(
                "C++ '{}' cannot bind its left operand because the CFlat expression is an rvalue and the parameter is a non-const reference.",
                opName));
        if (rhsIsRvalue && rhsMutableReference && !rhsBindsRvalue)
            LogErrorContext(ctx, std::format(
                "C++ '{}' cannot bind its right operand because the CFlat expression is an rvalue and the parameter is a non-const reference.",
                opName));

        auto makeRightNV = [&]() {
            LLVMBackend::NamedVariable rightNV;
            rightNV.Primary  = rvalue;
            rightNV.BaseType = rvalue ? rvalue->getType() : nullptr;
            rightNV.CxxOperatorFloatToInteger = true;
            rightNV.CxxArithIdentity = rhsArithIdentity;
            std::vector<antlr4::ParserRuleContext*> operandContexts;
            for (auto* child : ctx->children)
                if (auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(child))
                    operandContexts.push_back(rule);
            if (operandContexts.size() == 2)
            {
                if (std::string identity = CxxLiteralOperandIdentity(operandContexts[1]);
                    !identity.empty())
                    rightNV.CxxArithIdentity = identity;
                rightNV.LiteralIdentity = LLVMBackend::LiteralIdentityForOverload(
                    operandContexts[1]->getText(), &rightNV.LiteralIdentitySuffixed);
                rightNV.LiteralSigned = LLVMBackend::LiteralTextHasSign(
                    operandContexts[1]->getText());
            }
            rightNV.CallerName = rhsCallerName;
            rightNV.FieldName = rhsFieldName;
            rightNV.IsElementAccess = rhsIsElementAccess;
            rightNV.ConditionalMoveSources = rhsConditionalMoves;
            rightNV.ForceOperatorConsume = rhsConsumes;
            if ((rhsObjectCv & 1) != 0) rightNV.TypeAndValue.SetPointeeConstBit(true);
            // A scalar variable or call result: its declared CFlat name is the proof (never the width),
            // kept only when it lowers to the value's own type.
            if (rightNV.CxxArithIdentity.empty() && rvalue != nullptr
                && !rvalue->getType()->isStructTy() && !rvalue->getType()->isPointerTy()
                && IsCflatArithmeticName(rhsTypeName))
            {
                LLVMBackend::TypeAndValue declared;
                declared.TypeName = rhsTypeName;
                if (compiler->GetType(declared, nullptr, false) == rvalue->getType())
                    rightNV.CxxArithIdentity = rhsTypeName;
            }
            if (rvalue && rvalue->getType()->isStructTy())
            {
                auto* rst = llvm::cast<llvm::StructType>(rvalue->getType());
                if (!rst->isLiteral() && rst->hasName())
                    rightNV.TypeAndValue.TypeName = rst->getName().str();
                // A C++ derived operand binds to a `const Base&` parameter (an operator==
                // inherited from a base) from its own storage, adjusted to the base subobject. A
                // temporary (`it != m.end()`) is materialized first, as C++ does for a reference.
                if (compiler->IsCxxRecord(rightNV.TypeAndValue.TypeName))
                {
                    llvm::Value* storage = rhsStorage;
                    if (storage == nullptr)
                        if (auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(rvalue))
                            if (auto* slot = llvm::dyn_cast<llvm::AllocaInst>(
                                    load->getPointerOperand());
                                slot != nullptr && compiler->FindVariableByStorage(slot) == nullptr)
                            {
                                storage = slot;
                                rightNV.IsRvalue = true;
                                rightNV.IsCxxPrvalueTemp = true;
                                LLVMBackend::NamedVariable tempProbe = rightNV;
                                tempProbe.Storage = slot;
                                if (!compiler->IsOwnedTempValue(tempProbe))
                                    compiler->RegisterOwnedStructTemp(slot,
                                        rightNV.TypeAndValue.TypeName);
                            }
                    if (storage == nullptr)
                    {
                        storage = compiler->CreateAlloca(rst);
                        compiler->CreateAssignment(rvalue, storage);
                        // An xvalue (`move d`) read from its source slot must bind `T&&` as the
                        // temp; a load from the source would hand the callee the zeroed slot.
                        if (rhsIsRvalue && llvm::isa<llvm::LoadInst>(rvalue))
                        {
                            rightNV.Primary = compiler->builder->CreateLoad(rst, storage);
                            rightNV.IsExplicitMove = true;
                        }
                    }
                    else if (rhsXvalue)
                    {
                        // The named source is marked moved after the call, as for `f(move d)`.
                        rightNV.IsExplicitMove = true;
                        rightNV.IsRvalue = true;
                        rightNV.CallerName = compiler->FindVariableNameByStorage(storage);
                    }
                    rightNV.Storage = storage;
                }
                else if (rhsStorage != nullptr && !rhsIsRvalue && !rhsExplicitMove)
                {
                    rightNV.Storage = rhsStorage;
                    rightNV.IsBorrowed = true;
                }
            }
            else if (rvalue && rvalue->getType()->isPointerTy())
            {
                // If the RHS is a string literal (ptr to global constant), wrap it
                // as a %string struct so operator+(string, string) can match.
                /*
                 * A C++ receiver is the exception: its operators are declared over `const char*`,
                 * and a CFlat `string` argument can never rank against one. Keep the literal as
                 * the `char*` it is and flag it, exactly as an ordinary C++ call argument does,
                 * so the shared char*-literal ranking in CreateOverloadedFunctionCall applies.
                 */
                if (auto* c = llvm::dyn_cast<llvm::Constant>(rvalue))
                {
                    if (compiler->stringLiteralLenByPtr.count(c))
                    {
                        if (compiler->IsCxxRecord(typeName))
                            rightNV.IsStringLiteral = true;
                        else
                        {
                            rightNV.Primary  = compiler->WrapStringLiteralAsString(rvalue);
                            rightNV.BaseType = rightNV.Primary->getType();
                            rightNV.TypeAndValue.TypeName = "string";
                        }
                    }
                }
                // A recorded operand depth is the only thing that survives the reduction to a raw
                // llvm::Value; stamped ONLY when positive, so an unproven pointer claims nothing.
                if (rightNV.TypeAndValue.TypeName.empty() && rhsPointerDepth >= 1)
                {
                    rightNV.TypeAndValue.Pointer      = true;
                    rightNV.TypeAndValue.PointerDepth = rhsPointerDepth;
                    rightNV.TypeAndValue.ElemPointer  = rhsElemPointer;
                    // Keep a primitive pointee aside, as a call argument does (`int*` into P(int*)).
                    // Its const too: `c_str()` never binds a C++ `char*` parameter.
                    if (LLVMBackend::IsPrimitiveTypeName(rhsTypeName))
                    {
                        rightNV.InferSourceTypeName = rhsTypeName;
                        rightNV.TypeAndValue.SetPointeeConstBit(rhsCxxPointeeConst);
                    }
                    // A C++ record pointee is named with its const, so `T*` / `const T*`
                    // operator siblings rank (and refuse) exactly as a call argument does.
                    else if (compiler->IsCxxRecord(typeName) && compiler->IsCxxRecord(rhsTypeName))
                    {
                        rightNV.TypeAndValue.TypeName = rhsTypeName;
                        rightNV.TypeAndValue.SetPointeeConstBit(rhsCxxPointeeConst);
                        rightNV.AllowCxxClassPointerCtorConversion = true;
                    }
                }
            }
            else if (rvalue && rhsStorage != nullptr && !rhsIsRvalue && rhsIsAlias)
            {
                // A scalar lvalue keeps its storage, so a reference parameter binds it as an lvalue.
                rightNV.Storage = rhsStorage;
            }
            // An enum operand is its enum type, never the `int` it lowers to; without one the
            // unnamed integer cannot bind a C++ enum parameter (CxxEnumParameterRefusesArgument).
            if (rvalue && rvalue->getType()->isIntegerTy() && rightNV.TypeAndValue.TypeName.empty())
            {
                if (compiler->IsScopedEnumTypeName(rhsTypeName))
                    rightNV.TypeAndValue.TypeName = rhsTypeName;
                else if (std::string enumKey = compiler->ResolveEnumTypeName(rhsTypeName);
                         !enumKey.empty())
                {
                    rightNV.TypeAndValue.TypeName = enumKey;
                    rightNV.TypeAndValue.EnumBacking = compiler->GetEnumBackingType(enumKey);
                }
            }
            rightNV.IsRvalue = rhsIsRvalue;
            if (rhsIsRvalue && !rhsExplicitMove && rightNV.Storage != nullptr
                && compiler->IsForeignNontrivialCxxClass(rightNV.TypeAndValue.TypeName))
            {
                LLVMBackend::NamedVariable tempProbe = rightNV;
                tempProbe.Primary = rvalue;
                if (compiler->IsOwnedTempValue(tempProbe)) rightNV.IsCxxPrvalueTemp = true;
            }
            rightNV.TernaryTempAlreadyRegistered = inCallArgument_
                && rvalue != nullptr && llvm::isa<llvm::PHINode>(rvalue)
                && !ParseTreeContainsTernary(ctx);
            // A named native owner into a consuming by-value operand (`O operator+(O o) { return
            // o; }`) is surrendered like `f(b)`: name the source so the call nulls and retires it.
            if (rhsConsumes && !rhsIsRvalue && rhsStorage != nullptr
                && rvalue != nullptr && rvalue->getType()->isStructTy()
                && !compiler->IsCxxRecord(rightNV.TypeAndValue.TypeName))
            {
                std::string source = rhsCallerName.empty()
                    ? compiler->FindVariableNameByStorage(rhsStorage) : rhsCallerName;
                if (!source.empty())
                {
                    rightNV.Storage = rhsStorage;
                    rightNV.CallerName = source;
                }
            }
            return rightNV;
        };

        auto rightNV = makeRightNV();
        if (rhsExplicitMove) rightNV.IsExplicitMove = true;
        // C++ class receiver: members and frees compete on the whole argument list, as the unary rule does
        // (T275). A free-only winner goes to the free path; a member winner pins the member set; a member
        // and a free that tie are ambiguous (clang). Not inside a C++20 rewrite (operands are reversed).
        std::vector<std::string> cxxPinnedMembers;
        bool lhsMutableFieldReceiver = false;
        int8_t receiverCategory = -1;
        bool receiverRvalue = lhsIsRvalue;
        // Compound assignments rank their inherited member and free candidates as one set.
        const bool plainBinaryOperator = op == "==" || op == "!=" || op == "<=" || op == ">="
            || (op != "=" && (op.back() != '=' || op.size() == 1));
        if ((plainBinaryOperator || isCxxCompoundOperator(op)) && compiler->IsCxxRecord(typeName)
            && cxxOperatorRewriteDepth_ == 0)
        {
            auto operandRules = collectOperatorOperandRules();
            if (operandRules.size() == 2)
            {
                receiverCategory = CxxArgumentLvalueKind(operandRules[0]);
                if (receiverCategory == 1) receiverRvalue = false;
                else if (receiverCategory == 0) receiverRvalue = true;
            }
            // Inherited members are projected on demand: an unprojected base member would never reach the
            // candidate set, and a viable free would win against nothing (clang ranks it against the member).
            compiler->EnsureCxxMemberProjected(typeName, opName);
            // Namespace-scope frees are bound lazily under "ns.operatorX"; bind them before ranking.
            std::vector<std::string> tables = { opName };
            {
                const std::string base = typeName.substr(0, typeName.find('$'));
                const size_t dot = base.rfind('.');
                if (dot != std::string::npos)
                {
                    const std::string nsOperator = base.substr(0, dot) + "." + opName;
                    compiler->TryBindCxxFunction(nsOperator);
                    tables.push_back(nsOperator);
                }
            }
            auto hasInaccessibleBaseFree = [&]() {
                for (const std::string& table : tables)
                    if (auto it = compiler->functionTable.find(table);
                        it != compiler->functionTable.end())
                        for (const auto& candidate : it->second)
                        {
                            if (!candidate.IsCxx || isMemberCandidate(candidate)
                                || candidate.SourceName != table || candidate.Parameters.size() != 2)
                                continue;
                            for (size_t i = 0; i < 2; ++i)
                            {
                                const std::string& sourceType = i == 0 ? typeName : rhsTypeName;
                                const std::string& targetType = candidate.Parameters[i].TypeName;
                                if (sourceType.empty() || targetType.empty()
                                    || sourceType == targetType
                                    || !compiler->IsCxxRecord(sourceType)
                                    || !compiler->IsCxxRecord(targetType))
                                    continue;
                                uint64_t offset = 0;
                                bool inaccessible = false;
                                compiler->FindCxxBaseOffset(sourceType, targetType,
                                                            offset, inaccessible);
                                if (inaccessible)
                                    return true;
                            }
                        }
                return false;
            };
            if (hasInaccessibleBaseFree())
            {
                adlClangRefusal.clear();
                if (llvm::Value* exact = tryFreeOperator(true, true)) return exact;
                if (!adlClangRefusal.empty())
                {
                    LogErrorContext(ctx, adlClangRefusal);
                    return nullptr;
                }
            }
            lhsMutableFieldReceiver = isCxxRecordValue(lvalue)
                && compiler->IsCxxMutableFieldAccess(lhsStorage);
            LLVMBackend::NamedVariable objectArg;
            objectArg.Primary = lvalue;
            objectArg.Storage = lhsStorage;
            objectArg.BaseType = structTy;
            objectArg.TypeAndValue.TypeName = typeName;
            objectArg.TypeAndValue.IsCxxMutableField = lhsMutableFieldReceiver;
            objectArg.IsRvalue = receiverRvalue;
            objectArg.CxxLvalueKind = receiverCategory;
            if ((lhsObjectCv & 1) != 0) objectArg.TypeAndValue.SetPointeeConstBit(true);
            const int receiverCv = compiler->CxxReceiverCv(objectArg);
            // Parameter binding of a free operator's first operand: 0 by value, 1 `T&`, 2 `const T&`, 3 `T&&`.
            auto freeParamRefKind = [&](const LLVMBackend::FunctionSymbol& candidate) {
                auto rawIt = compiler->cxxFunctionSignatures_.find(candidate.SourceName);
                if (rawIt != compiler->cxxFunctionSignatures_.end())
                    for (const auto& raw : rawIt->second)
                        if (raw.linkageName == candidate.UniqueName && !raw.paramSpellings.empty())
                        {
                            const auto& spelling = raw.paramSpellings[0];
                            if (spelling.find("&&") != std::string::npos) return 3;
                            if (spelling.find('&') != std::string::npos)
                                return spelling.find("const") != std::string::npos ? 2 : 1;
                            return 0;
                        }
                if (candidate.Parameters.empty()) return 0;
                if (candidate.Parameters[0].IsRvalueRef) return 3;
                if (candidate.Parameters[0].CxxConstRef()) return 2;
                return candidate.Parameters[0].IsAlias ? 1 : 0;
            };
            // A free over a base of the receiver's class, by value or by any reference: clang ranks it
            // beside the members, so it must be a candidate (not only `T&` / `const T&`).
            auto isInheritedBaseReferenceFree = [&](const LLVMBackend::FunctionSymbol& candidate) {
                if (isMemberCandidate(candidate) || candidate.Parameters.empty()) return false;
                const auto& param = candidate.Parameters[0];
                if (param.Pointer && !param.IsAlias && !param.IsRvalueRef) return false;
                const std::string& paramType = param.TypeName;
                return paramType != typeName && compiler->IsCxxBaseOf(paramType, typeName);
            };
            struct RankedOperator
            {
                std::string uniqueName;
                std::vector<LLVMBackend::CxxConversionRank> ranks;
                bool member = false;
            };
            std::vector<RankedOperator> viable;
            std::set<std::string> seen;
            bool hasMember = false;
            bool hasFree = false;
            bool objectOnlyFailure = false;
            for (const std::string& table : tables)
                if (auto it = compiler->functionTable.find(table); it != compiler->functionTable.end())
                    for (const auto& candidate : it->second)
                    {
                        if (!candidate.IsCxx || candidate.Parameters.size() != 2
                            || (candidate.Parameters[0].TypeName != typeName
                                && !isInheritedBaseReferenceFree(candidate))
                            || !seen.insert(candidate.UniqueName).second) continue;
                        if (isMemberCandidate(candidate) && table != opName) continue;
                        if (!isMemberCandidate(candidate) && candidate.SourceName != table) continue;
                        (isMemberCandidate(candidate) ? hasMember : hasFree) = true;
                        // The object's binding is decided here, from the parameter spelling: the rank leaves a
                        // non-const lvalue reference to an rvalue and an rvalue reference unranked (-1).
                        const int refKind = isMemberCandidate(candidate) ? 0 : freeParamRefKind(candidate);
                        const bool objectOk = isMemberCandidate(candidate)
                            ? compiler->CxxImplicitObjectViable(receiverCv, receiverRvalue, candidate)
                            : refKind == 0 || refKind == 2
                              || (refKind == 1 && !receiverRvalue
                                  && !LLVMBackend::CxxReferenceDropsCv(receiverCv & 1, 0))
                              || (refKind == 3 && receiverRvalue
                                  && !LLVMBackend::CxxReferenceDropsCv(receiverCv & 1, 0));
                        auto ranks = compiler->RankCxxConversionSequences({ objectArg, rightNV }, candidate);
                        if (ranks.size() != 2) continue;
                        if (refKind == 3 && ranks[0].rank == -1)
                        {
                            // An rvalue reference to the object's class: [over.ics.rank] 3.2.3 against a member.
                            ranks[0].rank = 0;
                            ranks[0].cvBase = typeName;
                            ranks[0].refTarget = typeName;
                            ranks[0].refBind = 1;
                        }
                        auto rankOk = [&](size_t i) {
                            if (ranks[i].rank == LLVMBackend::kCxxNonViableRank) return false;
                            if (i == 0 && isMemberCandidate(candidate)) return ranks[i].rank == -2;
                            return ranks[i].rank >= 0;
                        };
                        const bool rhsOk = rankOk(1);
                        if (rhsOk && !objectOk) objectOnlyFailure = true;
                        if (objectOk && rhsOk && rankOk(0))
                            viable.push_back({ candidate.UniqueName, ranks, isMemberCandidate(candidate) });
                    }
            if (isCxxCompoundOperator(op) && viable.empty())
            {
                if (llvm::Value* freeResult = tryFreeOperator()) return freeResult;
                if (refuseConstWriteOnNoViable) return nullptr;
                if (cxxCompoundBinaryFallbackMayBind())
                {
                    adlClangRefusal.clear();
                    if (llvm::Value* clangResult = tryFreeOperator(true, true)) return clangResult;
                    return relayCxxCompoundRefusal();
                }
            }
            // Nothing viable, and the object alone is why (the right operand binds): clang refuses too.
            if (hasMember && hasFree && viable.empty() && objectOnlyFailure)
            {
                LogErrorContext(ctx, std::format("no overload of 'operator{}' matches the given arguments.", op));
                return nullptr;
            }
            if ((hasMember && hasFree || isCxxCompoundOperator(op)) && !viable.empty())
            {
                std::vector<const RankedOperator*> winners;
                for (const auto& c : viable)
                {
                    bool beaten = false;
                    for (const auto& d : viable)
                    {
                        bool crossing = false;
                        if (&c != &d && compiler->CompareCxxConversionRanks(d.ranks, c.ranks, crossing) == -1
                            && !crossing)
                        {
                            beaten = true;
                            break;
                        }
                    }
                    if (!beaten) winners.push_back(&c);
                }
                if (isCxxCompoundOperator(op) && winners.size() > 1)
                {
                    adlClangRefusal.clear();
                    if (llvm::Value* clangResult = tryFreeOperator(true, true)) return clangResult;
                    return relayCxxCompoundRefusal();
                }
                bool winnerMember = false;
                bool winnerFree = false;
                for (const auto* w : winners) (w->member ? winnerMember : winnerFree) = true;
                if (winnerMember && winnerFree)
                {
                    if (isCxxCompoundOperator(op))
                    {
                        adlClangRefusal.clear();
                        if (llvm::Value* clangResult = tryFreeOperator(true, true)) return clangResult;
                        return relayCxxCompoundRefusal();
                    }
                    LogErrorContext(ctx, std::format(
                        "ambiguous C++ binary operator '{}': a member and a free overload are both viable and rank equally",
                        opName));
                    return nullptr;
                }
                if (winnerFree && !winnerMember) return tryFreeOperator();
                for (const auto* w : winners) cxxPinnedMembers.push_back(w->uniqueName);
            }
        }
        // T85: a member dropped for an unrelated pointee may leave a member operator template (not
        // harvested) to rank beside the rest; clang decides that set, and its refusal stands.
        if (compiler->IsCxxRecord(typeName)
            && compiler->CxxOperatorDropsUnrelatedPointee(opName, rightNV, 1, typeName))
        {
            adlClangRefusal.clear();
            if (llvm::Value* ranked = tryFreeOperator(true, true)) return ranked;
            if (!adlClangRefusal.empty())
            {
                LogErrorContext(ctx, adlClangRefusal);
                return nullptr;
            }
        }
        if (rhsDepthUnproven && rightNV.TypeAndValue.TypeName.empty() && !rightNV.IsStringLiteral)
            if (auto it = compiler->functionTable.find(opName); it != compiler->functionTable.end())
                for (const auto& candidate : it->second)
                    if (isReceiverCandidate(candidate) && candidate.Parameters.size() >= 2
                        && candidate.Parameters[0].TypeName == typeName
                        && isTypedPointerParam(candidate, 1))
                    {
                        refuseUnprovenPointer(opName);
                        return nullptr;
                    }
        if (!rhsConsumes && !rhsIsAlias && llvm::isa<llvm::PHINode>(rvalue)
            && !rightNV.TernaryTempAlreadyRegistered)
        {
            compiler->RegisterBorrowedOwningStructTemp(rightNV, true);
            rightNV.TernaryTempAlreadyRegistered = true;
        }

        if (usePointer)
        {
            // By-pointer dispatch: conventional user-defined struct operators (T* this).
            /*
             * A C++ operator selected by pointer takes its left operand as `this` or as a
             * reference, and C++ NEVER copies for either: it binds the caller's object. Hand over
             * the real storage when there is any, or a mutating operator (`a += b`,
             * `sink << value`) would update a temporary. A by-VALUE C++ parameter is not a
             * pointer, so it never reaches this branch and still gets its copy.
             */
            llvm::Value* tempAlloca = nullptr;
            if (lhsIsAlias && !compiler->IsCxxRecord(typeName)
                && lhsPointerDepth == 0 && lhsStorage != nullptr)
                tempAlloca = lhsStorage;
            else if ((!lhsIsRvalue || lhsExplicitMove) && lhsStorage != nullptr
                && (compiler->IsCxxRecord(typeName)
                    || compiler->generatedCxxRecords_.count(typeName) != 0))
                tempAlloca = lhsStorage;
            else
            {
                tempAlloca = compiler->CreateAlloca(structTy);
                compiler->CreateAssignment(lvalue, tempAlloca);
            }

            LLVMBackend::NamedVariable thisNV;
            thisNV.TypeAndValue.TypeName = typeName;
            thisNV.TypeAndValue.Pointer  = true;
            thisNV.TypeAndValue.IsCxxMutableField = lhsMutableFieldReceiver;
            thisNV.BaseType = structTy;
            thisNV.Primary = tempAlloca;
            if (lhsExplicitMove)
            {
                thisNV.Storage = lhsStorage;
                thisNV.CallerName = lhsCallerName.empty()
                    ? compiler->FindVariableNameByStorage(lhsStorage) : lhsCallerName;
                thisNV.FieldName = lhsFieldName;
                thisNV.IsElementAccess = lhsIsElementAccess;
                thisNV.ConditionalMoveSources = lhsConditionalMoves;
                if (lhsSourceNV != nullptr)
                {
                    thisNV.PointeeMove = lhsSourceNV->PointeeMove;
                    thisNV.OwningStructName = lhsSourceNV->OwningStructName;
                    thisNV.MovePathKey = lhsSourceNV->MovePathKey;
                    thisNV.MovePathHasDynamicIndex = lhsSourceNV->MovePathHasDynamicIndex;
                    thisNV.FieldPathText = lhsSourceNV->FieldPathText;
                    thisNV.FieldPathRoot = lhsSourceNV->FieldPathRoot;
                    thisNV.FieldPathThroughPointer = lhsSourceNV->FieldPathThroughPointer;
                    thisNV.FieldPathHasPointerDereference = lhsSourceNV->FieldPathHasPointerDereference;
                    thisNV.TypeAndValue.ParentVariableName =
                        lhsSourceNV->TypeAndValue.ParentVariableName;
                    if (!lhsSourceNV->FieldPathRoot.empty())
                        thisNV.CallerName = lhsSourceNV->FieldPathRoot;
                    else if (!lhsSourceNV->FieldName.empty()
                        && !lhsSourceNV->TypeAndValue.ParentVariableName.empty())
                        thisNV.CallerName = lhsSourceNV->TypeAndValue.ParentVariableName;
                    else if (!lhsSourceNV->CallerName.empty())
                        thisNV.CallerName = lhsSourceNV->CallerName;
                }
                thisNV.IsExplicitMove = true;
            }
            else if (lhsCallerName == "this")
            {
                thisNV.CallerName = lhsCallerName;
                thisNV.FieldName = lhsFieldName;
            }
            if (tempAlloca != lhsStorage && !lhsIsRvalue) thisNV.ConstReceiverStorage = lhsStorage;
            thisNV.TernaryTempAlreadyRegistered = receiverTempAlreadyRegistered;
            // The caller's own storage is an lvalue: a ref-qualified C++ member ranks it as one.
            thisNV.CxxLvalueReceiver = tempAlloca == lhsStorage && lhsStorage != nullptr
                && !lhsMaterializedPrvalue;

            thisNV.CxxPinnedMemberNames = cxxPinnedMembers;
            if (receiverCategory >= 0)
            {
                thisNV.CxxLvalueKind = receiverCategory;
                thisNV.IsRvalue = receiverRvalue;
            }
            FlushCallPostfix();
            // The operator expression is the site key codegen's choice is recorded under.
            compiler->pendingCallSite_ = ctx;
            auto* result = compiler->CreateOverloadedFunctionCall(opName, { thisNV, rightNV },
                false, {}, {}, false, {}, /*allowOperatorAliasReturnStringTemp*/ true, lhsStorage);
            compiler->pendingCallSite_ = nullptr;
            TrackOwnedStringOperatorResult(compiler, result);
            return result;
        }
        else
        {
            // By-value dispatch: built-in value types like string whose operators
            // are defined as operator+(T a, ...) rather than operator+(T* a, ...).
            LLVMBackend::NamedVariable thisNV;
            thisNV.TypeAndValue.TypeName = typeName;
            thisNV.TypeAndValue.Pointer  = false;
            thisNV.TypeAndValue.IsCxxMutableField = lhsMutableFieldReceiver;
            thisNV.Primary  = lvalue;
            thisNV.BaseType = structTy;
            thisNV.TernaryTempAlreadyRegistered = receiverTempAlreadyRegistered;
            thisNV.CallerName = lhsCallerName;
            if (lhsExplicitMove)
            {
                thisNV.Storage = lhsStorage;
                if (thisNV.CallerName.empty())
                    thisNV.CallerName = compiler->FindVariableNameByStorage(lhsStorage);
                thisNV.IsExplicitMove = true;
                thisNV.IsRvalue = true;
            }
            if ((lhsObjectCv & 1) != 0) thisNV.TypeAndValue.SetPointeeConstBit(true);
            thisNV.FieldName = lhsFieldName;
            thisNV.IsElementAccess = lhsIsElementAccess;
            thisNV.ConditionalMoveSources = lhsConditionalMoves;
            // A named native owner into a consuming by-value left operand is surrendered like
            // `f(a)`: name the source so the call nulls and retires it.
            if (receiverSinkConsumes && !lhsIsRvalue && lhsStorage != nullptr
                && !llvm::isa<llvm::PHINode>(lvalue) && !compiler->IsCxxRecord(typeName))
            {
                std::string source = lhsCallerName.empty()
                    ? compiler->FindVariableNameByStorage(lhsStorage) : lhsCallerName;
                if (!source.empty())
                {
                    thisNV.Storage = lhsStorage;
                    thisNV.CallerName = source;
                    thisNV.ForceOperatorConsume = true;
                }
            }

            thisNV.CxxPinnedMemberNames = cxxPinnedMembers;
            if (receiverCategory >= 0)
            {
                thisNV.CxxLvalueKind = receiverCategory;
                thisNV.IsRvalue = receiverRvalue;
            }
            FlushCallPostfix();
            // The operator expression is the site key codegen's choice is recorded under.
            compiler->pendingCallSite_ = ctx;
            auto* result = compiler->CreateOverloadedFunctionCall(opName, { thisNV, rightNV },
                false, {}, {}, false, {}, /*allowOperatorAliasReturnStringTemp*/ true);
            compiler->pendingCallSite_ = nullptr;
            TrackOwnedStringOperatorResult(compiler, result);
            return result;
        }

        return nullptr;
    }

LLVMBackend::TypedValue MainListener::ParseMultiplicativeExpression(CFlatParser::MultiplicativeExpressionContext* ctx, ResultUse use) {
        CallPostfixScope operatorPostfix(this);
        DeclExpectedTypeGate declExpectedGate(&declExpectedType, ctx->children.size() == 1);
        auto nextCtxs = ctx->castExpression();

        if (nextCtxs.size() == 1)
        {
            auto namedVar = ParseCastExpression(nextCtxs[0], false, use);
            return TypedValueOfNamedOperand(namedVar, nextCtxs[0]);
        }
        else if (nextCtxs.size() > 1)
        {
            auto firstNV = ParseCxxFoldOperand(Compiler(ctx), [&] {
                return ParseCastExpression(nextCtxs[0], false, ResultUse::Value);
            });
            ResolveGenericFunctionValueOperand(firstNV, nextCtxs[0]);
            NormalizeCxxReferenceNamed(ctx, firstNV);
            bool lu = firstNV.TypeAndValue.IsUnsignedInteger() != -1;
            bool lhsIsRvalue = firstNV.IsRvalue;
            llvm::Value* lvalue = LoadNamedVariable(firstNV);
            LLVMBackend::TypedValue arithRunning = NamedArithOperand(firstNV, lvalue);
            llvm::Value* lhsStorage = firstNV.Storage;
            llvm::Type* lhsRefType = nullptr;
            std::string lhsSourceTypeName = firstNV.TypeAndValue.TypeName;
            int firstPointerDepth = 0;
            bool firstElemPointer = false;
            std::string lhsArithIdentity = CxxScalarOperandIdentity(
                Compiler(ctx), firstNV.CxxArithIdentity, firstNV.TypeAndValue.TypeName, lvalue);
            std::string lhsNativeArithmeticTypeName;
            std::string leftLiteralIdentity = CxxLiteralOperandIdentity(nextCtxs[0]);
            bool unsignedStorage = false;

            for (size_t i = 1; i < nextCtxs.size(); i++)
            {
                // Set again only by the built-in arithmetic arm below.
                const std::string leftArith = lhsArithIdentity;
                const std::string leftIdentity = !lhsArithIdentity.empty() ? lhsArithIdentity
                    : !leftLiteralIdentity.empty() ? leftLiteralIdentity : lhsSourceTypeName;
                const std::string leftNativeIdentity = !lhsNativeArithmeticTypeName.empty()
                    ? lhsNativeArithmeticTypeName : lhsSourceTypeName;
                leftLiteralIdentity.clear();
                lhsArithIdentity.clear();
                auto rightNV = ParseCxxFoldOperand(Compiler(ctx), [&] {
                    return ParseCastExpression(nextCtxs[i], false, ResultUse::Value);
                });
                ResolveGenericFunctionValueOperand(rightNV, nextCtxs[i]);
                NormalizeCxxReferenceNamed(ctx, rightNV);
                bool ru = rightNV.TypeAndValue.IsUnsignedInteger() != -1;
                llvm::Value* rvalue = LoadNamedVariable(rightNV);
                CxxFoldSretScope foldSret(Compiler(ctx), i + 1 < nextCtxs.size());
                unsigned leftBits = BinaryOperandBits(lvalue);
                unsigned rightBits = BinaryOperandBits(rvalue);
                std::string op = ctx->children[i * 2 - 1]->getText();

                int rightDepth = 0;
                bool rightElemPointer = false;
                OperandPointerDepth(rightNV, rvalue, rightDepth, rightElemPointer);
                if (i == 1 && lvalue->getType()->isPointerTy())
                    OperandPointerDepth(firstNV, lvalue, firstPointerDepth, firstElemPointer);
                auto* overload = TryBinaryOperatorOverload(lvalue, op, rvalue, ctx, nullptr,
                                                              rightDepth, rightElemPointer,
                                                              lhsStorage, rightNV.Storage,
                                                              true, true, lhsIsRvalue,
                                                              rightNV.IsRvalue,
                                                              i == 1 ? firstNV.TypeAndValue.TypeName
                                                                     : std::string(),
                                                              rightNV.TypeAndValue.TypeName,
                                                              i == 1 ? firstPointerDepth : 0,
                                                              i == 1 && firstElemPointer,
                                                              std::string(), std::string(),
                                                              false, std::string(), std::string(),
                                                              false, {}, {}, false, leftArith,
                                                              rightNV.CxxArithIdentity, rightNV.IsExplicitMove, i == 1 && firstNV.IsExplicitMove,
                                                              i == 1 && firstNV.TypeAndValue.IsCxxNullptrT,
                                                              rightNV.TypeAndValue.IsCxxNullptrT,
                                                              false, i == 1 && firstNV.TypeAndValue.PointeeConstBit(),
                                                              i == 1 ? Compiler(ctx)->CxxOperandObjectCv(firstNV) : 0,
                                                              Compiler(ctx)->CxxOperandObjectCv(rightNV));
                if (overload)
                {
                    LLVMBackend::NamedVariable resultNV;
                    resultNV.Primary = overload;
                    resultNV.TypeAndValue = Compiler(ctx)->lastCallReturnType;
                    DiagnoseVoidResultConsumed(ctx, resultNV, use, std::format("'operator{}'", op));
                }
                if (overload != nullptr)
                {
                    lhsNativeArithmeticTypeName.clear();
                    CarryCxxOperatorResult(ctx, overload, i + 1 < nextCtxs.size(), lvalue,
                                           lhsStorage, lhsIsRvalue, lhsRefType,
                                           lhsSourceTypeName);
                }
                else
                {
                    const LLVMBackend::TypedValue arithLhs = ArithRunningOperand(arithRunning, lvalue);
                    lvalue = Compiler(ctx)->CreateOperation(op, lvalue, rvalue, lu, ru);
                    arithRunning = ArithComputedOperand(lvalue, LLVMBackend::ArithResultNarrowWidth(
                        arithLhs, NamedArithOperand(rightNV, rvalue)));
                    lhsStorage = nullptr;
                    lhsIsRvalue = true;
                    lhsRefType = nullptr;
                    lhsSourceTypeName.clear();
                    lhsNativeArithmeticTypeName = lvalue != nullptr
                        ? ScalarArithmeticResultName(Compiler(ctx), leftNativeIdentity,
                            rightNV.TypeAndValue.TypeName, lvalue->getType())
                        : std::string();
                    lhsSourceTypeName = lhsNativeArithmeticTypeName;
                    if (lvalue != nullptr)
                    {
                        std::string rightIdentity = rightNV.CxxArithIdentity;
                        if (rightIdentity.empty()) rightIdentity = CxxLiteralOperandIdentity(nextCtxs[i]);
                        if (rightIdentity.empty()) rightIdentity = rightNV.TypeAndValue.TypeName;
                        lhsArithIdentity = ScalarArithmeticResultName(Compiler(ctx), leftIdentity,
                            rightIdentity, lvalue->getType());
                    }
                }
                unsignedStorage = !overload && lvalue != nullptr && lvalue->getType()->isIntegerTy()
                    && lvalue->getType()->getIntegerBitWidth() < 32 && (lu || ru);
                // An overload's signedness is its RETURN type, not the operands' flags.
                lu = overload ? Compiler(ctx)->lastCallReturnType.IsUnsignedInteger() != -1
                              : BinaryJoinIsUnsigned(lu, leftBits, ru, rightBits);
            }

            LLVMBackend::TypedValue result{ lvalue, lu };
            result.arithNarrowWidth = ArithRunningOperand(arithRunning, lvalue).arithNarrowWidth;
            result.isRvalue = lhsIsRvalue;
            result.receiverStorage = lhsStorage;
            result.cxxRefValueType = lhsRefType;
            result.isAlias = lhsRefType != nullptr && !lhsSourceTypeName.empty();
            result.storage = result.isAlias ? lhsStorage : nullptr;
            result.sourceTypeName = lhsSourceTypeName;
            result.cxxArithIdentity = lhsArithIdentity;
            if (!lhsNativeArithmeticTypeName.empty())
            {
                result.sourceType.TypeName = lhsNativeArithmeticTypeName;
                result.hasSourceType = true;
            }
            result.isUnsignedStorage = unsignedStorage;
            return result;
        }

        LogErrorContext(ctx, "Multiplicative expression has no operands.");
        return {};
    }

/*
 * A cast operand can name no value at all, in which case ParseCastExpression used to reach
 * CreateCast with a null llvm::Value and SIGSEGV the compiler with zero diagnostic output.
 * Two outcomes are possible and both are handled here.
 *
 * A generic FUNCTION instantiation ('gid<double>') is the supportable one: the postfix walk
 * publishes it as an empty NamedVariable carrying only the mangled instantiation name, the
 * same convention the declarator and assignment paths already consume. It is a monomorphized
 * function symbol, so resolving it to its llvm::Function makes the cast behave exactly like a
 * cast of a plain named function - no gate is bypassed, the cast lands on the same
 * ParameterStoresData / RegisterCodeValueDataCast tail as '(void*)plainFn'.
 *
 * Everything else that reaches here (a type name, a namespace, a generic function template
 * with no type arguments) genuinely has no value, so it gets a located diagnostic.
 */
