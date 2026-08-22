#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/Instrumentation/AddressSanitizer.h>
#include <llvm/Object/COFF.h>
#include <llvm/Object/Binary.h>
#include <llvm/Object/Archive.h>
#include <llvm/Object/COFFImportFile.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/TimeProfiler.h>
#include <llvm/Support/JSON.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticHandler.h>
#pragma warning(pop)
#include <antlr4-runtime.h>

#include "platform/GeneratedParser.h"
#include "LLVMBackend.h"
#include "MainListener.h"
#include "GrammarTreeListener.h"
#include <filesystem>
#include <optional>
#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#if defined(__APPLE__)
// Step 3 (macOS self-contained link): harvest libSystem's exported symbols from
// the live dyld shared cache to synthesize a linker stub, so -o needs no SDK.
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <dlfcn.h>
#include <cstring>
#include <sys/sysctl.h>
#endif

// ---- Definitions moved out of LLVMBackend.h (Overloads) ----

// The '*' count a diagnostic prints for a value type. Depth caps at 2, and array/view/simd/fat
// shapes spell it about their ELEMENT rather than themselves.
static std::string PointerStars(const LLVMBackend::TypeAndValue& tv)
{
    if (!tv.Pointer) return "";
    bool deep = tv.ElemPointer || tv.PointerDepth >= 2;
    return (deep && tv.DepthIsAboutThisValue()) ? "**" : "*";
}

std::pair<std::vector<LLVMBackend::NamedVariable>, LLVMBackend::FunctionSymbol> LLVMBackend::ComputeOverloadFunction(std::vector<std::pair<std::vector<NamedVariable>, FunctionSymbol>> candidates) const
{
        std::pair<std::vector<NamedVariable>, FunctionSymbol> possibleResult;
        std::pair<std::vector<NamedVariable>, FunctionSymbol> bestPerfect;
        int bestPerfectScore = -1;  // moveScore is always >= 0; -1 means "no perfect match yet"
        int bestPossibleScore = -1; // same, for the promotion/implicit tier
        // Fewest function-pointer shape mismatches seen in the promotion/implicit tier so far.
        int bestPossibleShapeMismatches = std::numeric_limits<int>::max();
        // int score = 0; // 2 for promotionMatch, 1 for implicitMatch

        for (const auto& pair : candidates)
        {
            const auto& [arguments, candidate] = pair;

            if (candidate.Variadic)
            {
                /*
                 * A variadic candidate is taken without per-argument scoring, so the code-value
                 * gate below never runs for it and `lam(Rec*, ...)` absorbed a function pointer
                 * exactly as the non-variadic sibling used to. Only the DECLARED parameters are
                 * judged: an argument in the `...` tail has no parameter to disagree with, and C
                 * passes function pointers through `...` routinely (`printf("%p", fn)`).
                 *
                 * Wider than the non-variadic sites: those judge only pointer/string parameters,
                 * and a DECLARED scalar here (`lam(int n, ...)`) absorbed the code address into an
                 * i32 slot and reached the LLVM verifier as a fatal "Call parameter type does not
                 * match function signature!". The non-variadic twin `lam(int n)` already rejects
                 * cleanly, so only this arm needs the wider question.
                 */
                bool codeIntoDataParam = false;
                auto varParamItr = candidate.Parameters.begin();
                for (const auto& arg : arguments)
                {
                    if (varParamItr == candidate.Parameters.end()) break;
                    if (ArgumentIsCodeValue(arg, arg.CastOccurrenceId) && !ParameterAcceptsCodeValue(*varParamItr))
                        codeIntoDataParam = true;
                    ++varParamItr;
                }
                if (codeIntoDataParam)
                    continue;

                // Variadic is a fallback: prefer any exact non-variadic match over it.
                // Reset the score so a later non-variadic candidate always overrides it.
                possibleResult = pair;
                bestPossibleScore = -1;
                bestPossibleShapeMismatches = std::numeric_limits<int>::max();
                continue;
            }

            bool perfectMatch = true;
            bool promotionMatch = true;
            bool implicitMatch = true;
            // Function-pointer arguments whose indirection shape disagrees with the parameter.
            int shapeMismatches = 0;

            auto candidateParamItr = candidate.Parameters.begin();
            for (const auto& arg : arguments)
            {
                int result = -1;

                // function<T> parameter: accept any function-compatible argument (named function,
                // lambda fat struct, or stored function<T> variable). Type fidelity is checked at codegen.
                // An encoded closure param (list<Lambda<...>>::add's `T value`, gap a) accepts the same
                // arguments; an encoded closure arg satisfies a function<T> param likewise.
                if ((candidateParamItr->IsFunctionPointer || IsEncodedClosureType(candidateParamItr->TypeName))
                    && (ArgumentIsFunctionPointerish(arg)
                        || (arg.BaseType && arg.BaseType->isPointerTy())))
                {
                    // `function<T>`, `function<T>*` and `function<T>[]` are three DISTINCT
                    // overloads; a disagreeing shape may still bind but never scores perfect.
                    result = (FunctionPointerShapeOf(arg.TypeAndValue, &arg)
                           == FunctionPointerShapeOf(*candidateParamItr, nullptr)) ? 0 : 1;
                    if (result != 0)
                        shapeMismatches++;

                    // Shapes agree: signatures that provably name different function types are
                    // proof. Only at equal shape - a shape mismatch stays score-1 bindable.
                    if (result == 0
                        && FuncPtrSignaturesProvablyDiffer(arg.TypeAndValue, *candidateParamItr))
                        result = -1;
                    // A NAMED function argument carries its signature in the function table, not
                    // on the argument, so it needs the overload-set form of the same proof.
                    if (result != -1 && NamedFunctionArgMismatches(arg, *candidateParamItr))
                        result = -1;
                }
                else if (arg.TypeAndValue.TypeName != "")
                {
                    // Resolve enum types for comparison: if either arg or param is an enum, use its backing type
                    auto resolveName = [&](const std::string& tn) -> std::string
                        {
                            if (tn.empty()) return tn;
                            auto it = enumBackingTypes.find(tn);
                            return (it != enumBackingTypes.end()) ? it->second : tn;
                        };

                    LLVMBackend::TypeAndValue tmpArg = arg.TypeAndValue;
                    LLVMBackend::TypeAndValue tmpParam = *candidateParamItr;

                    tmpArg.TypeName = resolveName(tmpArg.TypeName);
                    tmpParam.TypeName = resolveName(tmpParam.TypeName);

                    if (tmpArg.IsTypeMatch(tmpParam))
                        result = 0;
                    else if (tmpArg.IsTypePromotion(tmpParam))
                    {
                        // Positive = widening promotion (valid but non-perfect). Integer promotions
                        // report the source bit width; a floating-point promotion (float -> double)
                        // is not an integer, so IsInteger() returns -1 - use 1 so float -> double
                        // still scores as a valid promotion instead of a spurious no-match.
                        int bits = tmpArg.IsInteger();
                        result = (bits != -1) ? bits : 1;
                    }
                    else
                    {
                        // Same signedness group: int<->i32, long<->i64, char<->i8, etc.
                        // Same width  -> perfect match (result=0): int==i32, long==i64.
                        // Diff width  -> implicit conversion (result=1): i64->int, etc.
                        int myBits = tmpArg.IsInteger();
                        int otherBits = tmpParam.IsInteger();
                        bool myUnsigned = tmpArg.IsUnsignedInteger() != -1;
                        bool otherUnsigned = tmpParam.IsUnsignedInteger() != -1;
                        if (myBits != -1 && otherBits != -1 && myUnsigned == otherUnsigned)
                            result = (myBits == otherBits) ? 0 : 1;

                        // Unsigned source into a signed param at equal or greater width is a safe
                        // implicit conversion (e.g. u8 -> int, u64 -> i64); Upconvert zero-extends.
                        if (result < 0 && myBits != -1 && otherBits != -1 && myUnsigned && !otherUnsigned && myBits <= otherBits)
                            result = (myBits == otherBits) ? 1 : 1;

                        if (result < 0)
                        {
                            int myFP = tmpArg.IsFloatingPoint();
                            int otherFP = tmpParam.IsFloatingPoint();
                            if (myFP != -1 && otherFP != -1)
                                result = (myFP == otherFP) ? 0 : 1;
                        }

                        // Any pointer type is implicitly convertible to void*. Deliberately NOT
                        // gated on the argument's function-ness: `function<T>*` is the ADDRESS of a
                        // slot, i.e. data. The gate that refuses a function-pointer VALUE is in the
                        // empty-TypeName branch below - the shape such a value actually arrives in.
                        if (result < 0 && arg.TypeAndValue.Pointer &&
                            candidateParamItr->Pointer && candidateParamItr->TypeName == "void")
                        {
                            result = 0;
                        }

                        // Interface upcast using original names (interfaces are not enums).
                        // Handles both struct->interface (value) and struct*->interface (pointer).
                        if (result < 0 && candidateParamItr->IsInterface &&
                            !arg.TypeAndValue.IsInterface &&
                            StructImplementsInterface(arg.TypeAndValue.TypeName, candidateParamItr->TypeName))
                        {
                            result = 0;
                        }

                        // Derived interface -> parent interface (IButton arg to an IElement param).
                        // Implicit (1), not perfect, so an exact same-interface overload still wins.
                        if (result < 0 && candidateParamItr->IsInterface && arg.TypeAndValue.IsInterface &&
                            InterfaceInheritsFrom(arg.TypeAndValue.TypeName, candidateParamItr->TypeName))
                        {
                            result = 1;
                        }
                    }
                }
                else
                {
                    auto candidateParam = GetType(*candidateParamItr);
                    result = CompareUpconvert(arg.BaseType, candidateParam);

                    /*
                     * A function pointer or closure VALUE does not implicitly convert to a DATA
                     * pointer of any pointee - it is code, not data, and ISO C has no
                     * function-to-object-pointer conversion either. Without this a candidate
                     * refuted on its SIGNATURE silently rebound onto a pointer-absorbing sibling.
                     * This branch is where such a value arrives: the call-argument loop copies its
                     * signature but deliberately leaves TypeName empty, and opaque pointers then
                     * make CompareUpconvert accept it against any pointer parameter alike.
                     *
                     * Only the plain-VALUE shape is refused, decided by the same helper the funcptr
                     * arm uses: a `function<T>*` is the ADDRESS of a slot and a `function<T>[N]`
                     * decays to one, and both are plain data pointers that must keep converting.
                     */
                    bool argIsCodeValue = ArgumentIsCodeValue(arg, arg.CastOccurrenceId);

                    // The pointee is never itself a function-pointer type here: the funcptr arm
                    // above claims every such parameter whenever the argument is code.
                    if (result >= 0 && candidateParamItr->Pointer && argIsCodeValue)
                        result = -1;

                    // Opaque pointers make every pointer pair look identical to CompareUpconvert.
                    // An argument whose CFlat type is unknown (empty TypeName - primitive pointers
                    // like '&boolVar') binding to a pointer-to-struct parameter is only an IMPLICIT
                    // match, never a perfect one: a perfect match here would let a method's 'this'
                    // swallow any pointer in a free call (e.g. readLine(&eof) resolving to
                    // File.readLine with the bool* as 'this') and beat the exact free overload.
                    // nullptr stays a perfect match for any pointer parameter.
                    if (result == 0 && candidateParamItr->Pointer
                        && IsDataStructure(candidateParamItr->TypeName)
                        && arg.BaseType && arg.BaseType->isPointerTy()
                        && !(arg.Primary && llvm::isa<llvm::ConstantPointerNull>(arg.Primary)))
                        result = 1;

                    // Interface upcast: struct value (TypeName empty, BaseType is struct) to interface param
                    if (result < 0 && candidateParamItr->IsInterface && arg.BaseType)
                    {
                        if (auto* st = llvm::dyn_cast<llvm::StructType>(arg.BaseType))
                        {
                            auto structName = st->getName().str();
                            if (!structName.empty() && StructImplementsInterface(structName, candidateParamItr->TypeName))
                                result = 0;
                        }
                    }
                    // Implicit char* -> string coercion: string literal or char* passed to a string param.
                    // A code value is refused here for the same reason as the pointer gate above -
                    // it lowered to `operator string(char*)` reading the callee's machine code.
                    if (result < 0 && candidateParamItr->TypeName == "string" && !candidateParamItr->Pointer
                        && arg.BaseType && arg.BaseType->isPointerTy() && !argIsCodeValue)
                        result = 1;
                }

                /*
                 * Depth OVERRIDES both branches above, because both re-granted a pair the depth
                 * gates refuse: the named branch's numeric fallback scores `int**` against `int*`
                 * as one 32-bit "int", and the empty-TypeName branch sees only opaque pointers.
                 * Same predicate IsTypeMatch uses, so it refuses exactly what that refuses.
                 */
                if (result >= 0 && arg.TypeAndValue.PointerDepthRefuses(*candidateParamItr))
                    result = -1;

                if (result != 0)
                {
                    perfectMatch = false;
                }

                if (result < 0)
                {
                    promotionMatch = false;
                    implicitMatch = false;
                }

                if (!(perfectMatch || promotionMatch || implicitMatch))
                {
                    // quick break if matches is no longer possible.
                    break;
                }

                ++candidateParamItr;
            }

            if (perfectMatch)
            {
                int moveScore = ScoreMoveAgreement(arguments, candidate);
                if (moveScore > bestPerfectScore)
                {
                    bestPerfectScore = moveScore;
                    bestPerfect = pair;
                }
                continue;
            }

            // Promotion/implicit tier needs the SAME move tie-break as the perfect tier: an
            // int LITERAL key is only a promotion match, so `d.add(1, namedLvalue)` used to
            // degrade both overloads to this tier and silently keep the last-declared one -
            // the `move` overload - consuming the caller's variable.
            if (promotionMatch || implicitMatch)
            {
                int moveScore = ScoreMoveAgreement(arguments, candidate);
                // This tier ignores per-argument quality, so `pick(arr, 3)` picked by declaration
                // position. Prefer agreeing shapes; equal counts fall through to the old rule.
                if (shapeMismatches < bestPossibleShapeMismatches
                    || (shapeMismatches == bestPossibleShapeMismatches
                        && moveScore >= bestPossibleScore))   // >= keeps the pre-existing last-wins tie
                {
                    bestPossibleShapeMismatches = shapeMismatches;
                    bestPossibleScore = moveScore;
                    possibleResult = pair;
                }
            }
        }

        if (bestPerfectScore >= 0)
            return bestPerfect;

        return possibleResult;
    }

LLVMBackend::ArgumentBinding LLVMBackend::ComputeArgumentPositions(const std::vector<std::string>& argNames,
        const std::vector<TypeAndValue>& targetArguments, bool isVariadic, size_t firstTarget)
{
        ArgumentBinding binding;

        if (firstTarget > targetArguments.size())
            return binding;

        const size_t inputSize = argNames.size();
        const size_t paramSize = targetArguments.size() - firstTarget;

        if (isVariadic ? inputSize < paramSize : inputSize != paramSize)
            return binding;

        binding.PosMap.assign(inputSize, -1);
        std::vector<bool> usedTargetMap(paramSize);

        // Pass 1: named arguments - resolve to their fixed-param position by name.
        for (size_t posIndex = 0; posIndex < inputSize; posIndex++)
        {
            if (argNames[posIndex].empty())
                continue;

            auto it = std::find_if(targetArguments.begin() + firstTarget, targetArguments.end(),
                [&](const auto& typeAndName) { return argNames[posIndex] == typeAndName.VariableName; });

            if (it == targetArguments.end())
            {
                binding.UnknownName = true;
                binding.FailedName = argNames[posIndex];
                return binding;
            }

            size_t slot = (size_t)std::distance(targetArguments.begin() + firstTarget, it);
            // A second named argument for the same parameter would leave another slot unbound,
            // so the caller would read a default-constructed argument. Reject instead.
            if (usedTargetMap[slot])
            {
                binding.DuplicateName = true;
                binding.FailedName = argNames[posIndex];
                return binding;
            }
            usedTargetMap[slot] = true;
            binding.PosMap[posIndex] = (int64_t)(firstTarget + slot);
        }

        // Pass 2: unnamed arguments - assign to the next free fixed-param slot; for
        // variadic targets, arguments that overflow the fixed params go to trailing slots.
        size_t targetIndex = 0;
        size_t nextVariadicIdx = paramSize;

        for (size_t posIndex = 0; posIndex < inputSize; posIndex++)
        {
            if (!argNames[posIndex].empty())
                continue;

            bool assigned = false;
            while (targetIndex < paramSize)
            {
                if (!usedTargetMap[targetIndex])
                {
                    binding.PosMap[posIndex] = (int64_t)(firstTarget + targetIndex);
                    usedTargetMap[targetIndex] = true;
                    targetIndex++;
                    assigned = true;
                    break;
                }
                targetIndex++;
            }

            if (!assigned)
            {
                if (!isVariadic)
                    return binding;
                binding.PosMap[posIndex] = (int64_t)(firstTarget + nextVariadicIdx++);
            }
        }

        if (std::find(binding.PosMap.begin(), binding.PosMap.end(), -1) != binding.PosMap.end())
            return binding;

        binding.Ok = true;
        return binding;
    }

std::vector<LLVMBackend::NamedVariable> LLVMBackend::MatchFunction(const std::vector<LLVMBackend::NamedVariable>& inputArguments, const std::vector<LLVMBackend::TypeAndValue>& targetArguments, bool isVariadic, bool probe)
{
        std::vector<std::string> argNames;
        argNames.reserve(inputArguments.size());
        for (const auto& input : inputArguments)
            argNames.push_back(input.TypeAndValue.VariableName);

        auto binding = ComputeArgumentPositions(argNames, targetArguments, isVariadic);
        if (!binding.Ok)
        {
            // LogError does not return, so a reported failure never falls through to the
            // reconstruction below - and an unbound slot can therefore never reach a caller.
            if (!probe && binding.UnknownName)
                LogErrorMessage("named argument '{}' does not match any parameter", { binding.FailedName });
            if (!probe && binding.DuplicateName)
                LogErrorMessage("duplicate named argument '{}'", { binding.FailedName });
            return {};
        }

        // Reconstruct arguments in matched order. firstTarget is 0 here, so every PosMap entry
        // indexes the result directly.
        std::vector<LLVMBackend::NamedVariable> result(inputArguments.size());
        for (size_t i = 0; i < inputArguments.size(); i++)
            result[binding.PosMap[i]] = inputArguments[i];
        return result;
    }

llvm::Value* LLVMBackend::TryEmitAtomicBuiltin(const std::string& name, const std::vector<llvm::Value*>& args)
{
        using namespace llvm;
        auto& ctx = *context;

        // arg[0] is always the pointer to the _value field (i64* or i32*)
        if (name == "__atomic_counter_increment" || name == "__atomic_counter_decrement" ||
            name == "__atomic_counter_add")
        {
            // atomicrmw add/sub relaxed ptr, delta -> returns old value; we return old+delta
            Value* ptr   = args[0];
            Value* delta = (name == "__atomic_counter_decrement")
                ? ConstantInt::get(Type::getInt64Ty(ctx), -1)
                : (args.size() > 1 ? args[1] : ConstantInt::get(Type::getInt64Ty(ctx), 1));
            auto op = (name == "__atomic_counter_decrement")
                ? AtomicRMWInst::Add  // add(-1) == sub
                : AtomicRMWInst::Add;
            auto* old = builder->CreateAtomicRMW(op, ptr, delta,
                MaybeAlign(), AtomicOrdering::Monotonic);
            // return old + delta (new value)
            return builder->CreateAdd(old, delta, "atomic_new");
        }
        if (name == "__atomic_counter_read")
        {
            Value* ptr = args[0];
            auto* li = builder->CreateLoad(Type::getInt64Ty(ctx), ptr, "atomic_load");
            li->setAtomic(AtomicOrdering::SequentiallyConsistent);
            li->setAlignment(Align(8));
            return li;
        }
        if (name == "__atomic_flag_test_and_set")
        {
            Value* ptr = args[0];
            auto* one = ConstantInt::get(Type::getInt32Ty(ctx), 1);
            // xchg acquire: returns old value; 0 means we acquired the flag
            auto* old = builder->CreateAtomicRMW(AtomicRMWInst::Xchg, ptr, one,
                MaybeAlign(), AtomicOrdering::Acquire);
            // return true if old was 1 (flag was already set = contention)
            return builder->CreateICmpNE(old, ConstantInt::get(Type::getInt32Ty(ctx), 0), "was_set");
        }
        if (name == "__atomic_flag_clear")
        {
            Value* ptr = args[0];
            auto* zero = ConstantInt::get(Type::getInt32Ty(ctx), 0);
            auto* si = builder->CreateStore(zero, ptr);
            si->setAtomic(AtomicOrdering::Release);
            si->setAlignment(Align(4));
            return ConstantInt::get(Type::getInt32Ty(ctx), 0); // void: unused
        }
        if (name == "__atomic_i32_load" || name == "__atomic_i64_load")
        {
            bool is64 = (name == "__atomic_i64_load");
            Value* ptr = args[0];
            auto* ty = is64 ? Type::getInt64Ty(ctx) : Type::getInt32Ty(ctx);
            auto* li = builder->CreateLoad(ty, ptr, "atomic_load");
            li->setAtomic(AtomicOrdering::SequentiallyConsistent);
            li->setAlignment(is64 ? Align(8) : Align(4));
            return li;
        }
        if (name == "__atomic_i32_store" || name == "__atomic_i64_store")
        {
            bool is64 = (name == "__atomic_i64_store");
            Value* ptr = args[0];
            Value* val = args[1];
            auto* si = builder->CreateStore(val, ptr);
            si->setAtomic(AtomicOrdering::SequentiallyConsistent);
            si->setAlignment(is64 ? Align(8) : Align(4));
            return ConstantInt::get(Type::getInt32Ty(ctx), 0); // void: unused
        }
        if (name == "__atomic_i32_cas" || name == "__atomic_i64_cas")
        {
            bool is64 = (name == "__atomic_i64_cas");
            Value* ptr      = args[0];
            Value* expected = args[1];
            Value* desired  = args[2];
            auto* result = builder->CreateAtomicCmpXchg(ptr, expected, desired,
                MaybeAlign(),
                AtomicOrdering::AcquireRelease,
                AtomicOrdering::Monotonic);
            // extract the success bit (second element of {T, i1})
            return builder->CreateExtractValue(result, 1, "cas_ok");
        }
        if (name == "__atomic_release_store_i32" || name == "__atomic_release_store_i64" || name == "__atomic_release_store_flag")
        {
            bool is64   = (name == "__atomic_release_store_i64");
            bool isFlag = (name == "__atomic_release_store_flag");
            Value* ptr = args[0];
            Value* val = args[1];
            // bool is i1 in LLVM; atomic ops require byte-sized types - widen to i32.
            if (isFlag)
                val = builder->CreateZExt(val, Type::getInt32Ty(ctx), "flag_i32");
            auto* si = builder->CreateStore(val, ptr);
            si->setAtomic(AtomicOrdering::Release);
            si->setAlignment(is64 ? Align(8) : Align(4));
            return ConstantInt::get(Type::getInt32Ty(ctx), 0); // void: unused
        }
        if (name == "__atomic_acquire_load_i32" || name == "__atomic_acquire_load_i64" || name == "__atomic_acquire_load_flag")
        {
            bool is64   = (name == "__atomic_acquire_load_i64");
            bool isFlag = (name == "__atomic_acquire_load_flag");
            Value* ptr = args[0];
            auto* ty = is64 ? Type::getInt64Ty(ctx) : Type::getInt32Ty(ctx);
            auto* li = builder->CreateLoad(ty, ptr, "atomic_acq_load");
            li->setAtomic(AtomicOrdering::Acquire);
            li->setAlignment(is64 ? Align(8) : Align(4));
            // bool return: compare i32 result with zero.
            if (isFlag)
                return builder->CreateICmpNE(li, ConstantInt::get(Type::getInt32Ty(ctx), 0), "flag_bool");
            return li;
        }
        return nullptr; // not an atomic builtin
    }

bool LLVMBackend::RejectFuncPtrShapeMismatch(const NamedVariable& arg, const TypeAndValue& param)
{
        if (!param.IsFunctionPointer && !IsEncodedClosureType(param.TypeName))
            return false;

        int paramShape = FunctionPointerShapeOf(param, nullptr);
        int argShape = FunctionPointerShapeOf(arg.TypeAndValue, &arg);
        bool argIsNullLiteral = arg.Primary != nullptr
            && llvm::isa<llvm::ConstantPointerNull>(arg.Primary);

        const char* shapesAre = "'function<>', 'function<>*' and 'function<>[]' are three distinct "
            "shapes and none converts to another implicitly.";

        if (paramShape != 0 && argShape == 0 && !argIsNullLiteral)
        {
            // '&' produces a POINTER, so that advice is false for a view parameter.
            const char* advice = paramShape == 2
                ? "Pass a 'function<>[N]' array or another view to supply the shape it expects."
                : "Take the address with '&' to supply the shape it expects.";
            LogErrorMessage("cannot pass {} to closure parameter '{}', which expects {}: {} {}",
                { FuncPtrShapeWord(arg.TypeAndValue, &arg), param.VariableName,
                  FuncPtrShapeWord(param, nullptr), shapesAre, advice });
            return true;
        }

        if (paramShape == 0 && argShape == 2)
        {
            LogErrorMessage("cannot pass {} to closure parameter '{}', which expects {}: {} "
                "Index an element to supply the shape it expects.",
                { FuncPtrShapeWord(arg.TypeAndValue, &arg), param.VariableName,
                  FuncPtrShapeWord(param, nullptr), shapesAre });
            return true;
        }

        return false;
    }

bool LLVMBackend::RejectCodeValueIntoDataParam(const NamedVariable& arg, const TypeAndValue& param,
        const std::string& ifaceName, const std::string& methodName)
{
        // An interface parameter is left to the boxing path, exactly as the shape gate above is.
        if (param.IsInterface) return false;
        if (!CodeValueIntoDataDestination(arg, param)) return false;

        // Spelled from the DECLARED parameter, the only type the vtable slot knows.
        std::string spelling = param.IsArrayView
            ? param.TypeName + std::string(param.ElemPointer ? 1 : 0, '*') + "[]"
            : param.TypeName + std::string(param.Pointer ? 1 : 0, '*');
        // The cast escape is advised only where it compiles - a view rejects a raw 'T*' by its own
        // rule, and '(string)' of a raw value is itself refused.
        std::string advice = (param.Pointer && !param.IsArrayView && param.ConstArraySize == 0)
            ? spelling : std::string();
        // Unmangle the callee only on the instantiation REGISTRY, never on a bare '__' in the name:
        // DisplayNameOfMangledType splits unconditionally and would rewrite a plain 'I__x'.
        std::string callee = gts.genericInterfaceInstances.count(ifaceName) > 0
            ? DisplayNameOfMangledType(ifaceName) : ifaceName;
        callee += "." + methodName;
        std::string what = param.VariableName.empty()
            ? std::format("parameter of '{}'", callee)
            : std::format("parameter '{}' of '{}'", param.VariableName, callee);
        LogRawError(DescribeCodeValueIntoData(spelling, "pass", advice, what));
        return true;
    }

llvm::Value* LLVMBackend::CreateOverloadedFunctionCall(const std::string& functionNameIn, const std::vector<LLVMBackend::NamedVariable>& arguments, bool forceRoot,
        const std::string& displayName)
{
        std::string functionName = ResolveQualifiedName(functionNameIn, forceRoot);
        const std::string& shownFunctionName = displayName.empty() ? functionName : displayName;

        // Implicit `copy()` synthesis: a value type with no copy() of its own gets a memberwise
        // one generated on demand (the "every value type has an implicit copy() if undefined"
        // rule). Fires for both user `x.copy()` and the compiler's internal copy calls; an
        // existing copy() always wins (HasCopyOverloadFor). Pointers/primitives are skipped.
        if (functionName == "copy" && arguments.size() == 1
            && !arguments[0].TypeAndValue.Pointer
            && !arguments[0].TypeAndValue.TypeName.empty()
            && dataStructures.count(arguments[0].TypeAndValue.TypeName)
            && !HasCopyOverloadFor(arguments[0].TypeAndValue.TypeName))
        {
            // The closure fat type gets an env-cloning copy (not a memberwise one - both its
            // fields are pointers, which a memberwise copy would shallow-share and double-free).
            const std::string& copyType = arguments[0].TypeAndValue.TypeName;
            std::string uniquePath;
            if (copyType == "__closure_fat_ptr")
                EnsureClosureLifetimeRegistered();
            else if (TypeOwnsUniquePointer(copyType, &uniquePath))
            {
                // The single choke point for "someone wants a copy of this type and it has no
                // copy() of its own" - covers a user `.copy()`, a containing type's field copy,
                // and a by-value closure capture alike.
                LogErrorMessage(
                    "cannot copy '{}': its field '{}.{}' is '{}', so it owns a raw pointer that has "
                    "no generic deep-clone. A memberwise copy would share the pointer between two owners "
                    "and double-free at teardown. Write a '{}' method for '{}' that clones the pointee "
                    "itself, or '{}' the value to transfer ownership instead of copying it.",
                    { copyType, copyType, uniquePath, "unique", "copy()", copyType, "move" });
                return nullptr;
            }
            else if (IsOwningValueType(copyType))
                // Only an owning value type needs the deep memberwise synth; a POD struct is
                // handled by the bitwise fallback below, so synthesizing one would be dead.
                GetOrCreateMemberwiseCopy(copyType);
        }

        // Bitwise-copy fallback. The copy is deep only for an OWNING value type (string, a struct
        // with owning fields, a container, a closure), all handled above by the memberwise synth /
        // closure copy / unique error, or by a real copy() overload found in resolution below. Every
        // OTHER value reaching copy() - a pointer (incl. an interface pointer), a thin function
        // value, an enum, a primitive, a POD struct - has no deep-copy: the copy is the same bits
        // (it shares any pointee). Return them bitwise here, mirroring how '.~()' gracefully no-ops
        // on these types (GetOrCreateFullDestructor returns null). Firing before resolution also lets
        // list<enum>/list<IShape> copy() resolve and keeps a thin function off __closure_fat_ptr.copy.
        // arguments[0] is read only after the size==1 short-circuit (a 0-arg call must not index it).
        // A pointer copy is always a bitwise share (it copies the address, not the pointee), even
        // when the pointee type owns resources - so a pointer bypasses the owning / real-copy
        // guards that only apply to a by-value receiver.
        if (functionName == "copy" && arguments.size() == 1
            && arguments[0].TypeAndValue.TypeName != "__closure_fat_ptr"
            && (arguments[0].TypeAndValue.Pointer
                || (!IsOwningValueType(arguments[0].TypeAndValue.TypeName)
                    && !HasRealCopyOverloadFor(arguments[0].TypeAndValue.TypeName))))
        {
            const auto& arg = arguments[0];
            // A bitwise copy has the receiver's own type; publish it so the caller classifies the
            // result correctly (e.g. keeps IsInterface, so it binds to an interface parameter).
            lastCallReturnType = arg.TypeAndValue;
            // Interface element: the slot holds a bare fat {vtable,data} value. Load that from
            // Storage (Primary is a mis-classified single pointer) so it binds an interface param.
            if (arg.TypeAndValue.IsInterface && arg.Storage != nullptr)
                return CreateLoad(GetFatPtrType(), arg.Storage);
            if (arg.Primary != nullptr)
                return arg.Primary;
            if (arg.Storage != nullptr)
                return arg.BaseType && arg.UnionFieldType == nullptr
                    ? static_cast<llvm::Value*>(CreateLoad(arg.BaseType, arg.Storage))
                    : LoadArgStorage(arg);
        }

        auto funcSym = functionTable.find(functionName);
        if (funcSym == functionTable.end())
        {
            if (displayName.empty())
                LogErrorMessage("unknown function '{}'", { functionName });
            else
                LogErrorMessage("unknown generic function '{}'", { displayName });
            return nullptr;
        }

        const auto& candidates = funcSym->second;

        std::vector<std::pair<std::vector<NamedVariable>, FunctionSymbol>> resolvedCandidate;

        for (const auto& candidate : candidates)
        {
            if (candidate.Variadic)
            {
                // Route through MatchFunction so named fixed params are reordered correctly.
                // probe=true: a LOSING candidate's named-arg mismatch must not hard-error
                // out of this loop while a later candidate might still match - see the
                // non-probed re-run below for how the diagnostic is recovered when nothing
                // scores at all.
                auto matched = MatchFunction(arguments, candidate.Parameters, true, true);
                if (matched.size() > 0)
                {
                    resolvedCandidate.emplace_back(matched, candidate);
                    break;
                }
            }
            else if (arguments.size() == 0 && candidate.Parameters.size() == 0)
            {
                resolvedCandidate.emplace_back(arguments, candidate);
                break;
            }
            else
            {
                auto matched = MatchFunction(arguments, candidate.Parameters, false, true);
                if (matched.size() > 0)
                {
                    resolvedCandidate.emplace_back(matched, candidate);
                }
            }
        }

        const auto& [matched, candidate] = ComputeOverloadFunction(resolvedCandidate);

        if (candidate.Function == nullptr)
        {
            std::string msg = std::format("no overload of '{}' matches the given arguments.\n", shownFunctionName);

            // Recover a named-argument diagnostic only from candidates whose parameter names
            // actually bind. A losing candidate with different names must not blame the call for
            // a name miss when another candidate owns those names and failed for its types.
            std::vector<std::string> argumentNames;
            argumentNames.reserve(arguments.size());
            for (const auto& arg : arguments)
                argumentNames.push_back(arg.TypeAndValue.VariableName);
            bool replayedNameMatch = false;
            for (const auto& c : candidates)
            {
                auto binding = ComputeArgumentPositions(argumentNames, c.Parameters, c.Variadic);
                if (!binding.Ok) continue;
                replayedNameMatch = true;
                if (c.Variadic)
                    MatchFunction(arguments, c.Parameters, true, false);
                else if (!(arguments.size() == 0 && c.Parameters.size() == 0))
                    MatchFunction(arguments, c.Parameters, false, false);
            }
            // If every candidate missed the names, preserve the specific unknown/duplicate-name
            // diagnostic by replaying the first candidate once.
            if (!replayedNameMatch && !candidates.empty())
            {
                const auto& c = candidates.front();
                if (c.Variadic)
                    MatchFunction(arguments, c.Parameters, true, false);
                else if (!(arguments.size() == 0 && c.Parameters.size() == 0))
                    MatchFunction(arguments, c.Parameters, false, false);
            }

            // Call arguments
            msg += std::format("  Call arguments ({}):\n", arguments.size());
            for (size_t i = 0; i < arguments.size(); i++)
            {
                const auto& arg = arguments[i];
                std::string typeName = arg.TypeAndValue.TypeName;
                if (typeName.empty() && arg.BaseType)
                {
                    std::string typeStr;
                    llvm::raw_string_ostream rso(typeStr);
                    arg.BaseType->print(rso);
                    typeName = typeStr;
                }
                std::string name;
                if (arg.TypeAndValue.VariableName.empty())
                {
                    bool isThis = i == 0 && !candidates.empty() &&
                                  !candidates[0].Parameters.empty() &&
                                  candidates[0].Parameters[0].VariableName.ends_with("__");
                    name = isThis ? "<this>" : "<unnamed>";
                }
                else
                {
                    name = arg.TypeAndValue.VariableName;
                }
                msg += std::format("    [{}] {}{} {}\n", i, typeName, PointerStars(arg.TypeAndValue), name);
            }

            // Candidates
            msg += std::format("  Candidates ({}):\n", candidates.size());
            for (const auto& c : candidates)
            {
                std::string paramList;
                for (size_t i = 0; i < c.Parameters.size(); i++)
                {
                    if (i > 0) paramList += ", ";
                    const auto& p = c.Parameters[i];
                    paramList += std::format("{}{} {}", p.TypeName, PointerStars(p), p.VariableName);
                }
                msg += std::format("    {}({})\n", displayName.empty() ? c.UniqueName : shownFunctionName, paramList);
            }

            // If exactly one resolved candidate passed MatchFunction, show per-argument type comparison
            if (resolvedCandidate.size() == 1)
            {
                const auto& [resolvedArgs, resolvedSym] = resolvedCandidate.front();
                msg += std::format("  Argument mismatch detail (single resolved candidate: {}):\n",
                    displayName.empty() ? resolvedSym.UniqueName : shownFunctionName);
                size_t count = std::max(resolvedArgs.size(), resolvedSym.Parameters.size());
                for (size_t i = 0; i < count; i++)
                {
                    std::string argDesc = i < resolvedArgs.size() ? resolvedArgs[i].TypeAndValue.TypeName : "<missing>";
                    if (argDesc.empty() && i < resolvedArgs.size() && resolvedArgs[i].BaseType)
                    {
                        std::string typeStr;
                        llvm::raw_string_ostream rso(typeStr);
                        resolvedArgs[i].BaseType->print(rso);
                        argDesc = typeStr;
                    }
                    std::string argPtr = i < resolvedArgs.size() ? PointerStars(resolvedArgs[i].TypeAndValue) : "";
                    std::string paramDesc = i < resolvedSym.Parameters.size() ? resolvedSym.Parameters[i].TypeName : "<missing>";
                    std::string paramPtr = i < resolvedSym.Parameters.size() ? PointerStars(resolvedSym.Parameters[i]) : "";
                    msg += std::format("    [{}] arg={}{}  param={}{}\n", i, argDesc, argPtr, paramDesc, paramPtr);
                }
            }

            // Name the mechanism when a candidate was dropped for a function-pointer SIGNATURE:
            // the dump above prints only `arg=ptr param=__c_fn_ptr`, which points at no cause.
            for (const auto& c : candidates)
            {
                auto pi = c.Parameters.begin();
                for (size_t i = 0; i < arguments.size() && pi != c.Parameters.end(); i++, ++pi)
                {
                    if (!pi->IsFunctionPointer) continue;
                    std::string why = DescribeFuncPtrSignatureMismatch(arguments[i].TypeAndValue, *pi);
                    if (why.empty()) continue;
                    msg += std::format("  [{}] {}\n", c.UniqueName, why);
                    break;
                }
            }

            /*
             * Same service for the parameters a refuted funcptr candidate used to rebind onto:
             * opaque pointers make the dump above print two indistinguishable `ptr`s. Conditioned to
             * fire ONLY where the code-value gate is what refused, and the gate's question differs
             * per arm, so this condition is per-arm too. The NON-VARIADIC sites judge only an
             * argument in their own empty-TypeName shape (without that it misdirects: an arity
             * mismatch or a `__closure_fat_ptr` argument, which the non-empty-TypeName branch
             * rejects and where no such gate exists, would claim the line) and only a data
             * parameter. The VARIADIC gate calls ArgumentIsCodeValue unconditionally against every
             * declared parameter, so both requirements are dropped for a variadic candidate - a fat
             * `Lambda<T>` value into `lam(Rec*, ...)` is refused there and otherwise got no line.
             */
            for (const auto& c : candidates)
            {
                bool arityFits = c.Variadic ? arguments.size() >= c.Parameters.size()
                                            : arguments.size() == c.Parameters.size();
                if (!arityFits) continue;
                auto pi = c.Parameters.begin();
                for (size_t i = 0; i < arguments.size() && pi != c.Parameters.end(); i++, ++pi)
                {
                    if (!c.Variadic && !arguments[i].TypeAndValue.TypeName.empty()) continue;
                    if (!ArgumentIsCodeValue(arguments[i], arguments[i].CastOccurrenceId)) continue;
                    bool refused = c.Variadic ? !ParameterAcceptsCodeValue(*pi) : ParameterStoresData(*pi);
                    if (!refused) continue;
                    // Per-shape wording: "data type" is false at the scalar cell the variadic arm
                    // also judges, and a rejection's message must be true where it fires.
                    if (ParameterStoresData(*pi))
                        msg += std::format("  [{}] parameter {} is a data type ('{}{}') and the argument is "
                            "a function-pointer or closure VALUE - code does not convert to a data "
                            "pointer.\n", c.UniqueName, i, pi->TypeName, pi->Pointer ? "*" : "");
                    else
                        msg += std::format("  [{}] parameter {} has type '{}' and the argument is a "
                            "function-pointer or closure VALUE - code does not convert to a "
                            "non-pointer type.\n", c.UniqueName, i, pi->TypeName);
                    break;
                }
            }

            /*
             * Name the mechanism when a candidate was dropped for pointer DEPTH. Fires only where
             * the depth gate is what refused - both sides proven, which is the same condition
             * TypeAndValue::IsTypeMatch tests - so the wording is true wherever it appears.
             */
            for (const auto& c : candidates)
            {
                bool arityFits = c.Variadic ? arguments.size() >= c.Parameters.size()
                                            : arguments.size() == c.Parameters.size();
                if (!arityFits) continue;
                auto pi = c.Parameters.begin();
                for (size_t i = 0; i < arguments.size() && pi != c.Parameters.end(); i++, ++pi)
                {
                    bool tooDeep = (arguments[i].TypeAndValue.IsProvenDoublePointer()
                                 || arguments[i].TypeAndValue.IsProvenDecayedDoublePointer())
                                && pi->IsProvenSinglePointer();
                    bool tooShallow = arguments[i].TypeAndValue.IsProvenSinglePointerDepth()
                                   && pi->IsProvenDoublePointer();
                    if (!tooDeep && !tooShallow) continue;
                    // A mangled instantiation name is not writable source, so the advice clause is
                    // dropped rather than naming a type the user cannot spell.
                    bool writable = true;
                    std::string shown = DisplayNameOfMangledType(pi->TypeName, &writable);
                    std::string argShown = DisplayNameOfMangledType(arguments[i].TypeAndValue.TypeName);
                    // A PRIMITIVE pointer argument carries no CFlat TypeName at all, so the depth
                    // is all that is known about it - say only that, never "type '**'".
                    auto argType = [&](const char* stars, const char* unnamed) {
                        return argShown.empty() ? std::string(unnamed)
                                                : std::format("type '{}{}'", argShown, stars);
                    };
                    if (tooDeep)
                    {
                        std::string advice = writable
                            ? std::format(", or declare the parameter as '{}**'", shown) : std::string();
                        // A `T*[N]` argument decays to the element-0 ADDRESS, so the type the
                        // callee receives is `T**`; say so rather than naming the array.
                        std::string how = arguments[i].TypeAndValue.IsProvenDecayedDoublePointer()
                            && !argShown.empty()
                            ? std::format(" (a '{}*[{}]' array decays to '{}**')", argShown,
                                arguments[i].TypeAndValue.ConstArraySize, argShown)
                            : std::string();
                        msg += std::format("  [{}] parameter {} '{}' has type '{}*' and the argument has "
                            "{}{} - there is no implicit dereference. Dereference it with '*' at "
                            "the call site{}.\n",
                            c.UniqueName, i, pi->VariableName, shown,
                            argType("**", "one more level of indirection"), how, advice);
                    }
                    else
                    {
                        std::string advice = writable
                            ? std::format(", or declare the parameter as '{}*'", shown) : std::string();
                        msg += std::format("  [{}] parameter {} '{}' has type '{}**' and the argument has "
                            "{} - there is no implicit address-of. Take its address with '&' at "
                            "the call site{}.\n",
                            c.UniqueName, i, pi->VariableName, shown,
                            argType("*", "one fewer level of indirection"), advice);
                    }
                    break;
                }
            }

            LogRawError(msg);
            return nullptr;
        }

        // convert parameter to vector of llvm::value*
        std::vector<llvm::Value*> argList;
        auto candParamItr = candidate.Parameters.begin();
        size_t argIndex = 0;
        for (const auto& arg : matched)
        {
            // Variadic arguments past the declared parameter list must not be dispatched
            // through pointer-parameter logic (candParamItr points at the last declared
            // param, which for printf is 'ptr %fmt' - causing all variadic args to be
            // pushed as storage/GEP addresses instead of loaded values).
            bool inVariadicRange = candidate.Variadic && argIndex >= candidate.Parameters.size();

            // An owning value rvalue has no named owner that can survive the C vararg boundary.
            // The pointer ledger covers owning pointer returns/new; by-value owning returns use
            // the same value identity ledger plus the owning-struct type test. Keep string values
            // on the existing representation-based diagnostic below so its message is unchanged.
            bool argIsUnbound = arg.Storage == nullptr
                && FindVariableStorage(arg.CallerName).Storage == nullptr;
            bool argIsStringValue = arg.Primary != nullptr
                && arg.Primary->getType() == llvm::StructType::getTypeByName(*context, "string");
            bool argIsOwningValueRValue = argIsUnbound && arg.FieldName.empty()
                && !arg.TypeAndValue.Pointer
                && (arg.IsOwningStruct || IsOwningValueStructValue(arg.Primary)
                    || (arg.IsOwningString && !argIsStringValue
                        && arg.TypeAndValue.TypeName != "string"));

            if (inVariadicRange
                && (arg.IsExplicitMove || IsOwningPtrTempValue(arg.Primary)
                    || argIsOwningValueRValue))
            {
                LogErrorMessage(
                    "cannot pass an owning value to the variadic '{}' slot of '{}'; bind the "
                    "value to an owner first", { "...", functionName });
                return nullptr;
            }

            // Array-view parameter gate: a raw `T*` must not bind to a `T[]` parameter - that
            // would forge the noalias contract the view promises (a whole, distinct allocation).
            // Placed before the binding branches because an array-view param has Pointer=true and
            // is handled by the pointer-parameter branch below. The reverse (`T[] -> T*` decay)
            // is always safe; a view argument carries IsArrayView and passes.
            if (!inVariadicRange && candParamItr->IsArrayView
                && arg.TypeAndValue.Pointer && !arg.TypeAndValue.IsArrayView)
                LogErrorMessage(
                    "cannot pass a raw pointer '{}' as array-view parameter '{}' ('{}') - a view "
                    "must span a whole allocation (it comes only from '{}' or another '{}'); "
                    "the '{} -> {}' decay is one-way",
                    { "T*", candParamItr->VariableName, "T[]", "new T[n]", "T[]", "T[]", "T*" });

            // Closure SHAPE gate (value vs pointer vs view), shared with virtual dispatch.
            // Hoisted above the binding branches: it judges the pair, not one binding arm.
            if (!inVariadicRange && !candParamItr->IsInterface
                && RejectFuncPtrShapeMismatch(arg, *candParamItr))
                return nullptr;

            if (!inVariadicRange && candParamItr->IsInterface && !arg.TypeAndValue.IsInterface)
            {
                // A `unique` interface param takes ownership and frees its boxed object at scope
                // exit. A struct VALUE source would box a STACK address as the data pointer, so
                // that teardown would free a stack address (heap corruption). Only a heap pointer
                // (`new`) may transfer ownership here. Gated on IsUnique, not IsMove: a borrow
                // `list<IShape>::add(move T value)` is a move param but does not own, so a stack
                // value bound to it stays legal.
                if (candParamItr->IsUnique && !arg.TypeAndValue.Pointer)
                {
                    LogErrorMessage(
                        "call to '{}': cannot pass a stack value to {} interface parameter '{}' - "
                        "it takes ownership and frees the object at scope exit, so the source must be "
                        "a heap '{}' (or a '{}' of an owned interface value), not a stack value",
                        { functionName, "unique", candParamItr->VariableName, "new", "move" });
                    return nullptr;
                }
                // A pointer-shaped source (`T**`, `T[]`, `T[N]`, simd) is not an instance of its
                // element class, so boxing it would attach that class's vtable to the wrong storage.
                std::string argShape = DescribePointerShapedInterfaceSource(arg.TypeAndValue);
                if (!argShape.empty())
                {
                    LogRawError(FormatPointerShapedInterfaceUpcastError(
                        argShape, arg.TypeAndValue.TypeName, candParamItr->TypeName));
                    return nullptr;
                }
                // Derive struct name from TypeName if available, else from BaseType
                std::string structName = arg.TypeAndValue.TypeName;
                if (structName.empty() && arg.BaseType)
                {
                    if (auto* st = llvm::dyn_cast<llvm::StructType>(arg.BaseType))
                        structName = st->getName().str();
                }

                // Build fat value: vtable + data ptr -> {i8*, i8*} by value
                auto vtable = GetOrCreateVTable(structName, candParamItr->TypeName);
                llvm::Value* dataPtr = nullptr;
                if (arg.TypeAndValue.Pointer)
                {
                    // struct* -> interface*: data ptr IS the pointer value (not the alloca of the pointer).
                    dataPtr = arg.Primary != nullptr ? arg.Primary : LoadArgStorage(arg);
                }
                else if (arg.Storage != nullptr)
                {
                    dataPtr = arg.Storage;
                }
                else
                {
                    // Materialize a pointer to the struct value
                    auto structTy = arg.BaseType ? arg.BaseType : GetType(arg.TypeAndValue);
                    // Defensive: a hand-built NamedVariable with no BaseType and no resolvable
                    // TypeName yields a null/void type here; allocating it would crash LLVM.
                    // Report the bad call instead of forging an invalid alloca.
                    if (structTy == nullptr || structTy->isVoidTy())
                    {
                        LogErrorMessage(
                            "call to '{}': argument {} (interface parameter '{}') has no resolved type",
                            { functionName, std::to_string(argIndex), candParamItr->VariableName });
                        return nullptr;
                    }
                    auto tempAlloca = AllocaAtEntry(structTy, nullptr);
                    builder->CreateStore(arg.Primary, tempAlloca);
                    dataPtr = tempAlloca;
                }
                argList.push_back(BuildInterfaceFatValue(vtable, dataPtr));
            }
            else if (!inVariadicRange && candParamItr->IsInterface && arg.TypeAndValue.IsInterface)
            {
                // Interface -> interface: pass fat struct by value, re-boxing on an upcast
                llvm::Value* val = arg.Primary ? arg.Primary : LoadArgStorage(arg);
                argList.push_back(ReboxInterfaceIfNeeded(val, arg.TypeAndValue.TypeName, candParamItr->TypeName));
            }
            else if (!inVariadicRange && candParamItr->Pointer)
            {
                // A 'string' is a {ptr,len} value, not a char*: the lowering below would hand the
                // callee the PAIR's address. Only a variadic candidate reaches here with one (a
                // non-variadic candidate is rejected by overload scoring first).
                bool argIsStringValue = !arg.TypeAndValue.Pointer
                    && (arg.TypeAndValue.TypeName == "string"
                        || (arg.Primary != nullptr
                            && arg.Primary->getType() == llvm::StructType::getTypeByName(*context, "string")));
                if (argIsStringValue && (candParamItr->TypeName == "char" || candParamItr->TypeName == "i8"))
                {
                    LogErrorMessage(
                        "cannot pass '{}' to the '{}' parameter '{}' of '{}': a '{}' is a "
                        "{} value, not a '{}' - the callee would read the pair itself. Pass "
                        "the buffer explicitly with '{}'. An interpolated string literal is a "
                        "'{}': bind it first ({}; {}).",
                        { "string", "char*", candParamItr->VariableName, functionName, "string",
                          "{ptr,len}", "char*", ".data()", "string", "string s = \"{{x}}\"",
                          "printf(\"%s\", s.data())" });
                    return nullptr;
                }

                // For a non-pointer value passed to a pointer parameter (e.g. a field access
                // used as the 'this' receiver), prefer Storage (the GEP address) over Primary
                // (the pre-loaded value). Primary holds the struct value itself, which would
                // be the wrong type for a pointer parameter.
                // Guard: if Primary is already a pointer value (e.g. loaded from a global ptr),
                // use Primary directly - Storage would be the wrong level of indirection.
                if (!arg.TypeAndValue.Pointer && arg.Storage != nullptr
                    && !(arg.Primary != nullptr && arg.Primary->getType()->isPointerTy()))
                    argList.push_back(arg.Storage);
                else if (!arg.TypeAndValue.Pointer && arg.Storage == nullptr
                         && arg.Primary != nullptr && arg.Primary->getType()->isStructTy())
                {
                    // By-value struct parameter passed to a pointer parameter (e.g. args.count()
                    // where args is a list<T> value param). Materialize on the stack first.
                    auto* tempAlloca = AllocaAtEntry(arg.Primary->getType(), nullptr);
                    builder->CreateStore(arg.Primary, tempAlloca);
                    argList.push_back(tempAlloca);
                }
                else
                {
                    // arg is a pointer type; Storage may be an alloca holding the pointer
                    // (promoted param). Load through it to get the actual pointer value.
                    if (arg.Primary == nullptr && arg.Storage != nullptr
                        && llvm::isa<llvm::AllocaInst>(arg.Storage))
                        argList.push_back(LoadArgStorage(arg));
                    else
                        argList.push_back(arg.GetValue());
                }

                // Mirrors the 'uniqueAutoSink' entry rule (~3581/3608): both spellings bind the
                // param owning, and only HERE is the argument's origin still visible.
                llvm::Value* loweredArg = argList.back();
                bool paramIsUniqueAutoSink = candParamItr->IsUniqueTypeArg
                    && !candParamItr->IsAlias && !candParamItr->IsBorrowOfUniqueElement;
                if ((candParamItr->IsMove || paramIsUniqueAutoSink)
                    && IsProvableNonHeapAddress(loweredArg))
                {
                    const std::string qualifier = candParamItr->IsMove ? "'move'" : "unique";
                    LogErrorMessage(
                        "call to '{}': cannot pass the address of a stack or global value to {} "
                        "parameter '{}' - it takes ownership and frees the pointee at scope exit, "
                        "but neither is heap-allocated and freeing it is undefined. Use '{}' to "
                        "allocate on the heap, or drop '{}' if the callee only borrows.",
                        { functionName, qualifier, candParamItr->VariableName, "new", "new" });
                    return nullptr;
                }
            }
            else if (!inVariadicRange && candParamItr->IsFunctionPointer)
            {
                // function<T> parameter - dispatch depends on whether the callee is extern C.
                llvm::Value* val = arg.Primary ? arg.Primary : LoadArgStorage(arg);
                // Inspect the actual LLVM param type to distinguish fat struct vs C fn ptr.
                unsigned llvmParamIndex = (unsigned)argList.size();
                if (!candidate.External)
                {
                    llvmParamIndex = 0;
                    for (size_t i = 0; i < argIndex && i < candidate.Parameters.size(); i++)
                        llvmParamIndex += ParameterCarriesRawArrayCount(candidate.Parameters[i]) ? 2u : 1u;
                }
                auto* llvmParamTy = candidate.Function->getFunctionType()->getParamType(llvmParamIndex);
                if (llvmParamTy->isStructTy())
                {
                    // Internal function<T>: provide a closure fat struct {i8*, i8*}.
                    if (val && !val->getType()->isStructTy())
                    {
                        // Re-resolve a NAMED FUNCTION only: skips same-key method overloads and
                        // picks matching 'move' flags. On a call result CallerName is the CALLEE.
                        if (!arg.CallerName.empty() && llvm::isa<llvm::Function>(val))
                        {
                            int expectedCount = (int)candParamItr->FuncPtrParams.size();
                            if (auto* correctFn = GetFunctionForFuncPtr(arg.CallerName, expectedCount, &candParamItr->FuncPtrParams))
                                val = correctFn;
                            // Reject when no overload's IsMove flags match the destination signature.
                            if (!HasFunctionWithMoveFlags(arg.CallerName, candParamItr->FuncPtrParams))
                            {
                                LogErrorMessage(
                                    "function '{}' has no overload matching the '{}' modifiers required by parameter '{}' - '{}' is part of the function-pointer type",
                                    { arg.CallerName, "move", candParamItr->VariableName, "move" });
                            }
                        }
                        // Same provenance gate virtual dispatch applies (LowerByValueArg): under
                        // opaque pointers a data pointer would otherwise land in the code slot.
                        val = WidenToClosureFatChecked(val, arg, candParamItr->VariableName);
                    }
                }
                else
                {
                    // Extern C-compatible parameter: provide a bare C function pointer.
                    // The CFlat function's raw address escapes into separately-linked C code
                    // that may store and later call it by pointer, so restore external linkage
                    // (CreateFunctionDefinition defaults non-extern functions to internal) to
                    // keep the symbol's identity across the lld-link boundary.
                    if (auto* escFn = llvm::dyn_cast<llvm::Function>(val))
                        if (escFn->getLinkage() == llvm::Function::InternalLinkage)
                            escFn->setLinkage(llvm::Function::ExternalLinkage);
                    if (val && val->getType()->isStructTy())
                    {
                        // The argument is a CFlat closure fat struct {code, env} - a lambda or a
                        // `function<>` variable. A C function pointer is a bare code address with
                        // no env slot; the shared helper passes a provably non-capturing invoker
                        // and rejects anything that would lose captured state across the C ABI.
                        // A rejection does not return, so this never stores null.
                        val = LowerClosureFatToThinFnPtr(val, llvmParamTy,
                            candParamItr->VariableName, arg.LambdaCaptureNames);
                    }
                    else if (val && !val->getType()->isPointerTy())
                    {
                        val = builder->CreateBitCast(val, llvmParamTy, "fn_for_extern");
                    }
                    else if (val)
                    {
                        // Same provenance gate virtual dispatch applies (LowerByValueArg): the
                        // bitcast below would otherwise make a data pointer callable as code.
                        CheckThinFnPtrArgProvenance(val, arg, candParamItr->VariableName);
                        val = builder->CreateBitCast(val, llvmParamTy, "fn_for_extern");
                    }
                }
                argList.push_back(val);
            }
            else
            {
                llvm::Value* value = nullptr;
                if (arg.Primary == nullptr)
                {
                    value = LoadArgStorage(arg);
                }
                else
                {
                    value = arg.Primary;
                }

                if (!inVariadicRange)
                {
                    // Canonical by-value arg lowering (string coercion + move heap-copy);
                    // shared with virtual dispatch via CallInterfaceMethod.
                    value = LowerByValueArg(value, *candParamItr, arg);

                    // An owning-struct RVALUE temp passed to a by-value BORROW param has no named
                    // owner and the callee will not free it; register it for end-of-full-expression
                    // destruction. A param that TAKES ownership is excluded (it frees the temp): a
                    // `move` param, or an inferred owning-value move-SINK - otherwise the sink slot
                    // AND this end-of-expr flush both free the temp -> double-free.
                    // A CONSUME-inferred sink of a copyable owner does NOT take ownership (its store
                    // is a copy), so an rvalue temp must still be registered for end-of-expr freeing.
                    bool paramTakesOwnership = candParamItr->IsMove
                        || (OwningSinkConsumesConcrete(*candParamItr)
                            && (candParamItr->TypeName == "string" || IsOwningValueType(candParamItr->TypeName)));
                    if (!paramTakesOwnership)
                        RegisterBorrowedOwningStructTemp(arg);
                }
                else if (value->getType()->isIntegerTy(1))
                {
                    // C default argument promotion for a variadic slot: 'bool' (i1) widens to
                    // int and is ALWAYS zero-extended (true -> 1, never -1). Without this the
                    // vararg slot keeps the caller's garbage upper bits.
                    value = Upconvert(value, builder->getInt32Ty(), true);
                }
                else if (value->getType()->isIntegerTy(8) || value->getType()->isIntegerTy(16))
                {
                    // C default argument promotion for a variadic slot: widen a sub-int
                    // integer to int, choosing zero- vs sign-extension by the source type's
                    // signedness so that e.g. u8 255 promotes to 255, not -1. Signedness is
                    // only known here (CreateFunctionCall sees a bare llvm::Value).
                    value = PromoteToInt(value, arg.TypeAndValue.IsUnsignedInteger() != -1);
                }
                else if (!arg.TypeAndValue.Pointer
                         && value->getType() == llvm::StructType::getTypeByName(*context, "string"))
                {
                    // Keyed on the REPRESENTATION, not the spelling: an interpolated string
                    // literal reaches here as a string struct with no 'string' TypeName.
                    // A 'string' is a {ptr,len} value type, not a char*. A variadic slot is
                    // untyped, so the compiler cannot know the callee wants the char* (the
                    // format string is not visible here) - passing the struct silently feeds
                    // the LENGTH to the next slot and crashes on a second '%s'. cflat already
                    // rejects string -> char* at a typed parameter; be consistent and make the
                    // user state the lowering.
                    LogErrorMessage(
                        "cannot pass '{}' to the variadic '{}' of '{}': a variadic slot is "
                        "untyped and a '{}' is a {} value, not a '{}' - the length "
                        "field would be read as the next argument. Pass the buffer explicitly with "
                        "'{}' (e.g. {}).", { "string", "...", functionName, "string", "{ptr,len}",
                                              "char*", ".data()", "printf(\"%s\", s.data())" });
                }

                argList.push_back(value);
            }
            if (!inVariadicRange && candParamItr != candidate.Parameters.end() - 1)
                ++candParamItr;
            argIndex++;
        }

        // Intercept __atomic_* stubs: emit LLVM atomic IR directly.
        if (candidate.Function->getName().starts_with("__atomic_"))
        {
            auto* atomicResult = TryEmitAtomicBuiltin(candidate.Function->getName().str(), argList);
            if (atomicResult != nullptr)
            {
                lastCallReturnType = candidate.ReturnType;
                lastCallReturnsOwned = false;
                lastCallReturnsAllocAlign = 0;
                return atomicResult;
            }
        }


        // Check: bonded value must not be passed to a move parameter (would transfer ownership out of scope).
        for (size_t i = 0; i < candidate.Parameters.size() && i < matched.size(); i++)
        {
            if (candidate.Parameters[i].IsMove && matched[i].IsBonded)
                LogErrorMessage("parameter '{}': cannot pass bonded value to '{}' parameter - bonded values cannot be transferred out of their source's scope",
                    { candidate.Parameters[i].VariableName, "move" });
            // The element slot of a borrowing container is a legal `move` destination: the
            // container keeps the ONLY handle afterwards (manual-free idiom), so the generic
            // "move into a borrow parameter transfers nothing" diagnostic is wrong there.
            if (!IsBorrowingContainerElementSink(functionName, candidate.Parameters, i,
                                                 candidate.IsMethod))
                DiagnoseExplicitMoveToBorrowParam(functionName, candidate.Parameters[i], matched[i]);
            RejectOwningLocalIntoBorrowingContainer(functionName, candidate.Parameters, i,
                                                    candidate.IsMethod, matched[i]);
        }

        // C-extern ABI lowering: when the resolved candidate has struct-by-value params or
        // return, the LLVM Function was declared with the lowered signature (iN coerce /
        // byval ptr / sret). The current argList still holds CFlat-natural struct values -
        // EmitAbiLoweredCall rewrites it to match the recipe and reloads the struct return
        // for the caller. Otherwise fall through to the existing call path.
        // Remember where this callee was first called, so an end-of-module diagnostic
        // (CheckPoisonedFunctionCalls) can point at the real call site.
        if (candidate.Function != nullptr)
            firstCallLocation_.emplace(candidate.Function->getName().str(),
                std::make_pair(currentLine, currentColumn));

        llvm::Value* rawReturnCountSlot = nullptr;
        if (!candidate.Recipe.hasLowering && !candidate.External)
        {
            std::vector<llvm::Value*> abiArgs;
            abiArgs.reserve(argList.size() + candidate.Parameters.size() + 1);
            size_t normalIndex = 0;
            for (size_t i = 0; i < candidate.Parameters.size() && normalIndex < argList.size(); i++)
            {
                abiArgs.push_back(argList[normalIndex++]);
                if (ParameterCarriesRawArrayCount(candidate.Parameters[i]))
                    abiArgs.push_back(RawArrayCountArgument(matched[i]));
            }
            while (normalIndex < argList.size()) abiArgs.push_back(argList[normalIndex++]);
            if (ReturnCarriesRawArrayCount(candidate.ReturnType))
            {
                rawReturnCountSlot = CreateRawArrayReturnCountSlot();
                abiArgs.push_back(rawReturnCountSlot);
            }
            argList = std::move(abiArgs);
        }

        llvm::Value* result = candidate.Recipe.hasLowering
            ? EmitAbiLoweredCall(candidate, argList)
            : CreateFunctionCall(candidate.Function, argList);
        RegisterRawArrayCallResult(result, rawReturnCountSlot,
                                   candidate.ReturnType.AllocAlignValue);

        // Runs before ApplyMoveParamTransfer, whose UnregisterOwnedPtrTemp still has the
        // last word for a sink parameter.
        RegisterNonEscapingOwningPtrArgs(result);
        // A callee that provably hands EXACTLY this owning argument back aliases it, so the
        // result carries the ownership and an owning destination can adopt it.
        AdoptLaunderedOwningTempResult(result);
        // The REJECT-side twin: a callee whose every return reads a live `unique` field hands back
        // an object that field still owns, so this result is a borrow, not something to adopt.
        if (result != nullptr && candidate.ReturnType.Pointer && !candidate.ReturnsOwned)
            if (const auto* borrow = FindUniqueFieldBorrowReturn(candidate.Function))
                RegisterUniqueFieldBorrowResult(result, *borrow);

        // Extern C function returning a function pointer: the LLVM-level return type is a
        // bare ptr but CFlat function<T> variables hold the {fn, env} closure fat struct.
        // Wrap via a per-signature thunk so the indirect-call path (which always prepends
        // env to the args) reaches the real C function correctly.
        if (candidate.ReturnType.IsFunctionPointer
            && !candidate.ReturnType.IsThinFnPtr()
            && result != nullptr
            && result->getType()->isPointerTy())
        {
            result = WrapCFuncPtrAsFatStruct(result, candidate.ReturnType);
        }

        // Cache the resolved return type so callers can populate TypeAndValue after the call.
        lastCallReturnType = candidate.ReturnType;
        lastCallReturnType.IsAlias = candidate.ReturnsAlias; // mark borrow-return result; inert until consumed
        if (candidate.ReturnsAlias) RegisterAliasValue(result);
        // The call RESULT is now the current expression value, so a `new`/`move` that ran only in
        // the ARGUMENT list describes a different value: retire its sticky channels here.
        lastOwningResult = false;
        lastAllocAlignment = 0;
        // A substituted `unique X*` type-arg return owns like `move` (Pointer-gated: a struct
        // VALUE return e.g. list<T>.copy() may carry IsUniqueTypeArg but is not owned here).
        lastCallReturnsOwned = candidate.ReturnsOwned
            || (candidate.ReturnType.IsUniqueTypeArg && candidate.ReturnType.Pointer);
        // Ledger the owning-return result by value for the no-discard check: string / pointer /
        // interface via lastCallReturnsOwned, plus a by-value owning-value STRUCT return (move S).
        bool ownedValueStructReturn = !candidate.ReturnType.Pointer
            && candidate.ReturnType.TypeName != "string"
            && IsOwningValueType(candidate.ReturnType.TypeName);
        if (lastCallReturnsOwned || ownedValueStructReturn)
        {
            RegisterOwnedReturnTemp(result, functionName, candidate.ReturnType);
            // An `alias` return hands back a BORROW the callee still owns. Keep the entry VISIBLE
            // to the no-discard check, but never let it answer an ownership question - otherwise a
            // '?:' arm scores it owning and the receiving local destroys the callee's live value.
            if (candidate.ReturnsAlias) SuppressCallerRelease(result);
        }
        // Return-type `alignas(_, N)`: the callee hands back an N-aligned heap block. Stamp the
        // side-channel so the receiving local frees via __delete_aligned (consumed in ParseDeclaration).
        lastCallReturnsAllocAlign = candidate.ReturnType.AllocAlignValue;

        // Populate lock-check side-channels for call-site RequiredLocks verification.
        lastCallRequiredLocks = candidate.RequiredLocks;
        lastCallParameterNames.clear();
        for (const auto& p : candidate.Parameters)
            lastCallParameterNames.push_back(p.VariableName);

        // Populate bond side-channel: collect source variable names for bond parameters.
        lastCallIsBonded = false;
        lastCallBondByAddress = false;
        lastCallBondedSources.clear();
        for (size_t i = 0; i < candidate.Parameters.size() && i < matched.size(); i++)
        {
            if (candidate.Parameters[i].IsBond && !matched[i].CallerName.empty())
            {
                lastCallIsBonded = true;
                lastCallBondedSources.push_back(matched[i].CallerName);
            }
        }

        // A consuming closure bound to a DECLARED closure PARAMETER: the parameter's registered
        // type cannot adopt the inferred sink, so the callee consumes and the caller frees again.
        for (size_t i = 0; i < candidate.Parameters.size() && i < matched.size(); i++)
        {
            int lostSink = FindLostClosureSinkParam(candidate.Parameters[i],
                                                    matched[i].TypeAndValue);
            if (lostSink < 0) continue;
            LogRawError(DescribeLostClosureSink(candidate.Parameters[i], (size_t)lostSink,
                std::format("parameter '{}' of '{}'",
                            candidate.Parameters[i].VariableName, functionName)));
        }

        // Null out caller's storage for move parameters; mark the source moved (shared helper).
        ApplyMoveParamTransfer(functionName, candidate.Parameters, matched, true,
                               candidate.IsMethod);

        // A temp's `unique` field handed to a PLAIN `T*` parameter. Runs AFTER the sink reject
        // above, so `unique` / `move` parameters never reach the callee-side question.
        RecordTempUniqueFieldArgs(result, functionName, matched);

        // Register a closure-returning call RESULT as an owned closure temp (lambda Option A),
        // mirroring how a lambda LITERAL is tracked at creation. A binding site (decl-init /
        // assignment / field store / return) calls UnregisterOwnedClosureTemp so only the owner
        // frees it; a result used INLINE (invoked directly, or passed by value as an argument) and
        // never bound is freed by FlushOwnedClosureTemps at end-of-full-expression. Exclude the
        // `copy` clone - its result is always consumed by an owner or stored into a struct field by
        // the synthesized memberwise copy, so flushing it would double-free a now-owned field.
        // A monomorphized generic `T` return (e.g. queue<Lambda>::dequeue) has a bare-T static
        // ReturnType, not IsFunctionPointer, yet the runtime value IS a closure fat struct. Gate on
        // the concrete LLVM type so the returned env temp is cleaned up at end-of-expr (no leak).
        // An `alias T` BORROW return (e.g. queue<Lambda>::peek) must NOT be registered - freeing a
        // borrowed env would double-free the slot the container still owns.
        if (result != nullptr
            && functionName != "copy"
            && !candidate.ReturnsAlias
            && result->getType() == GetClosureFatPtrType())
            RegisterOwnedClosureTemp(result);

        // Register an owned-string-returning call RESULT as an owned string temp, mirroring the
        // closure case above and TrackOwnedStringOperatorResult (operator+). A binding site
        // (decl-init / assignment / field store / move-param / return) calls
        // UnregisterOwnedStringTemp so only the owner frees it; a result used INLINE - passed by
        // value as a borrow (non-move 'string') argument, or as an expression statement - is never
        // bound and would otherwise leak, so it is freed by FlushOwnedStringTemps at end-of-full-
        // expression. Exclude the 'copy' clone: the synthesized memberwise copy stores its result
        // straight into a struct field (GetOrCreateMemberwiseCopy), bypassing the assignment-path
        // unregister, so flushing it would double-free a now-owned field - same reasoning as closures.
        if (result != nullptr
            && candidate.ReturnsOwned
            && functionName != "copy"
            && result->getType() == llvm::StructType::getTypeByName(*context, "string"))
            RegisterOwnedStringTemp(result);

        return result;
    }

llvm::Function* LLVMBackend::GetFunction(const std::string& functionName)
{
        auto functionSym = functionTable.find(functionName);

        if (functionSym != functionTable.end())
        {
            return functionSym->second.front().Function;
        }

        return module->getFunction(functionName);
    }

bool LLVMBackend::HasFunctionWithMoveFlags(std::string functionName, const std::vector<TypeAndValue::FuncPtrParam>& expectedParams) const
{
        functionName = ResolveQualifiedName(functionName);
        auto it = functionTable.find(functionName);
        if (it == functionTable.end()) return true;  // unknown - leave to other mechanisms
        bool sawCountMatch = false;
        for (const auto& sym : it->second)
        {
            if (sym.IsMethod) continue;
            if (sym.Parameters.size() != expectedParams.size()) continue;
            sawCountMatch = true;
            bool ok = true;
            for (size_t i = 0; i < sym.Parameters.size(); i++)
                if (sym.Parameters[i].IsMove != expectedParams[i].IsMove) { ok = false; break; }
            if (ok) return true;
        }
        // No exact-count overload exists -> not a 'move-modifier' problem; let other type-check paths handle.
        return !sawCountMatch;
    }

llvm::Function* LLVMBackend::GetFunctionForFuncPtr(std::string functionName, int expectedParamCount,
                                          const std::vector<TypeAndValue::FuncPtrParam>* expectedParams,
                                          const TypeAndValue* destSig)
{
        functionName = ResolveQualifiedName(functionName);
        auto it = functionTable.find(functionName);
        if (it == functionTable.end() || it->second.empty())
            return module->getFunction(functionName);

        std::vector<const FunctionSymbol*> viable;
        for (const auto& sym : it->second) viable.push_back(&sym);

        if (destSig != nullptr && !destSig->FuncPtrReturnTypeName.empty())
        {
            // The DESCRIBE function is the rule: asking it keeps the verdict and the message from
            // ever disagreeing about which overloads were refused and why.
            std::vector<const FunctionSymbol*> bindable;
            for (auto* sym : viable)
                if (DescribeFuncPtrBindMismatch(functionName, FuncPtrSigOfSymbol(*sym), *destSig).empty())
                    bindable.push_back(sym);
            if (bindable.empty())
            {
                LogRawError(DescribeFuncPtrBindMismatch(functionName,
                    FuncPtrSigOfSymbol(*viable.front()), *destSig));
            }
            else
            {
                viable = std::move(bindable);
            }
        }

        const auto& overloads = viable;
        if (overloads.size() == 1)
            return overloads.front()->Function;

        // When expectedParams is provided, prefer the overload whose per-param IsMove flags match exactly.
        auto moveFlagsMatch = [&](const FunctionSymbol& sym) -> bool {
            if (!expectedParams) return true;
            if (sym.Parameters.size() != expectedParams->size()) return false;
            for (size_t i = 0; i < sym.Parameters.size(); i++)
                if (sym.Parameters[i].IsMove != (*expectedParams)[i].IsMove) return false;
            return true;
        };

        // Pass 1: non-method, param count + move flags match.
        for (const auto* sym : overloads)
        {
            if (sym->IsMethod) continue;
            if (expectedParamCount >= 0 && (int)sym->Parameters.size() != expectedParamCount) continue;
            if (!moveFlagsMatch(*sym)) continue;
            return sym->Function;
        }
        // Pass 2: non-method, count only (legacy behavior when no expectedParams).
        for (const auto* sym : overloads)
        {
            if (sym->IsMethod) continue;
            if (expectedParamCount < 0 || (int)sym->Parameters.size() == expectedParamCount)
                return sym->Function;
        }
        // Fallback: any overload whose effective (non-self) param count matches.
        for (const auto* sym : overloads)
        {
            int effectiveCount = sym->IsMethod ? (int)sym->Parameters.size() - 1 : (int)sym->Parameters.size();
            if (expectedParamCount < 0 || effectiveCount == expectedParamCount)
                return sym->Function;
        }
        return overloads.front()->Function;
    }

LLVMBackend::NamedVariable LLVMBackend::GetLocalVariable(const std::string& name)
{
        for (const auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
        {
            auto& nameVal = stackFrame.namedVariable;
            auto result = nameVal.find(name);

            if (result != nameVal.end())
            {
                auto nv = result->second;
                nv.CallerName = name;
                return nv;
            }
        }

        return {};
    }

LLVMBackend::NamedVariable LLVMBackend::GetScopedLocalOrArgument(const std::string& name)
{
        for (const auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = stackFrame.namedVariable.find(name); it != stackFrame.namedVariable.end())
            {
                auto nv = it->second;
                nv.CallerName = name;
                return nv;
            }
            if (auto it = stackFrame.functionArgument.find(name); it != stackFrame.functionArgument.end())
            {
                auto nv = it->second;
                nv.CallerName = name;
                return nv;
            }
        }

        return {};
    }

bool LLVMBackend::IsFunctionParameter(const std::string& name) const
{
        if (name.empty()) return false;
        for (const auto& frame : stackNamedVariable)
            if (frame.functionArgument.find(name) != frame.functionArgument.end())
                return true;
        return false;
    }

auto LLVMBackend::FindThisArgIt(const std::map<std::string, NamedVariable>& args)
        -> std::map<std::string, NamedVariable>::const_iterator
{
        for (auto it = args.begin(); it != args.end(); ++it)
            if (it->first.ends_with("__"))
                return it;
        return args.end();
    }

LLVMBackend::NamedVariable LLVMBackend::GetMemberVariable(const std::string& name)
{
        for (const auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
        {
            const auto& functionArguments = stackFrame.functionArgument;

            if (functionArguments.size() > 0)
            {
                auto thisIt = FindThisArgIt(functionArguments);
                if (thisIt == functionArguments.end())
                    return {};

                const auto& memberStructName = thisIt->first;
                // Storage is either an alloca-of-struct (constructor) or an alloca-of-ptr (method param).
                // For the latter, load through it to get the actual struct pointer before GEP.
                llvm::Value* memberStructInstance = thisIt->second.Storage;
                if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(memberStructInstance))
                {
                    if (alloca->getAllocatedType()->isPointerTy())
                        memberStructInstance = CreateLoad(alloca);
                }
                auto truncName = memberStructName.substr(0, memberStructName.size() - 2);
                auto findResult = dataStructures.find(truncName);
                if (findResult != dataStructures.end())
                {
                    int count = 0;
                    const auto& sd = findResult->second;
                    for (const auto& structField : sd.StructFields)
                    {
                        if (structField.VariableName == name)
                        {
                            NamedVariable namedVar;
                            auto* fieldLLVMType = GetType(structField);
                            if (sd.IsUnion)
                            {
                                // Union: all fields alias at offset 0; load with explicit field type.
                                namedVar.Storage = memberStructInstance;
                                namedVar.UnionFieldType = fieldLLVMType;
                                namedVar.Primary = CreateLoad(fieldLLVMType, memberStructInstance);
                            }
                            else
                            {
                                namedVar.Storage = CreateStructGEP(sd.StructType, memberStructInstance, count);
                                namedVar.Primary = CreateLoad(namedVar.Storage);
                            }
                            namedVar.BaseType = namedVar.Primary->getType();
                            namedVar.TypeAndValue = structField;
                            // Preserve unique-field provenance across a later cast so a bare
                            // self-field read (`(Res*)p` inside a method) stays a tracked alias.
                            if (structField.IsUnique && structField.Pointer)
                                namedVar.IsUniqueFieldAlias = true;
                            // Field declared `alignas(_, N)`: stamp the block alignment so a bare
                            // `delete field` inside a member (e.g. the destructor) frees via
                            // __delete_aligned. GetMemberVariable purposely omits OwningStructName.
                            namedVar.AllocAlignment = structField.AllocAlignValue;
                            return namedVar;
                        }
                        count++;
                    }
                }
                return {};
            }
        }

        return {};
    }

LLVMBackend::NamedVariable LLVMBackend::GetCurrentMemberThis(const std::string& functionName)
{
        for (const auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
        {
            const auto& functionArguments = stackFrame.functionArgument;
            if (functionArguments.empty())
                continue;

            auto thisIt = FindThisArgIt(functionArguments);
            if (thisIt == functionArguments.end())
                break;

            const auto& thisArgName = thisIt->first;
            std::string structName = thisArgName.substr(0, thisArgName.size() - 2);

            auto funcIt = functionTable.find(functionName);
            if (funcIt == functionTable.end())
                break;

            for (const auto& sym : funcIt->second)
            {
                if (!sym.Parameters.empty() &&
                    sym.Parameters[0].TypeName == structName &&
                    sym.Parameters[0].Pointer)
                {
                    NamedVariable thisVar = thisIt->second;
                    thisVar.TypeAndValue.VariableName = "";
                    return thisVar;
                }
            }
            break;
        }
        return {};
    }

LLVMBackend::NamedVariable LLVMBackend::GetThisPointer()
{
        for (const auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
        {
            const auto& functionArguments = stackFrame.functionArgument;
            if (functionArguments.empty())
                continue;

            auto thisIt = FindThisArgIt(functionArguments);
            if (thisIt == functionArguments.end())
                break;  // first frame with arguments but no 'this' -> not a member body
            const auto& [key, nv] = *thisIt;
            std::string structName = key.substr(0, key.size() - 2);
            if (!IsDataStructure(structName))
                break;
            // Only a *method* self qualifies here: its storage is an alloca-of-pointer
            // (the incoming this* param), so loading it yields the struct pointer.
            // A constructor's self is an alloca-of-struct (value under construction);
            // leave that to the existing `this`-handling path.
            auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(nv.Storage);
            if (!alloca || !alloca->getAllocatedType()->isPointerTy())
                break;
            NamedVariable thisVar = nv;
            thisVar.CallerName = "this";
            thisVar.TypeAndValue.TypeName = structName;
            thisVar.TypeAndValue.Pointer = true;
            return thisVar;
        }
        return {};
    }

LLVMBackend::NamedVariable LLVMBackend::GetFunctionArgument(std::string name)
{
        for (const auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
        {
            const auto& nameVal = stackFrame.functionArgument;
            auto result = nameVal.find(name);

            if (result != nameVal.end())
            {
                auto nv = result->second;
                nv.CallerName = name;
                return nv;
            }
        }

        return {};
    }

size_t LLVMBackend::FindVariableScopeDepth(const std::string& name) const
{
        size_t depth = stackNamedVariable.size();
        for (const auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            --depth;
            if (frame.functionArgument.count(name) || frame.namedVariable.count(name))
                return depth;
        }
        return SIZE_MAX;
    }

std::string LLVMBackend::FindActiveBondBorrower(const std::string& sourceName) const
{
        for (const auto& frame : stackNamedVariable)
        {
            for (const auto& [name, nv] : frame.namedVariable)
            {
                if (nv.IsBonded && !nv.BondByAddress)
                {
                    for (const auto& src : nv.BondedSources)
                    {
                        if (src == sourceName)
                            return name;
                    }
                }
            }
        }
        return {};
    }

void LLVMBackend::ClearVariableBond(const std::string& name)
{
        auto* here = builder != nullptr ? builder->GetInsertBlock() : nullptr;
        auto* function = here != nullptr ? here->getParent() : nullptr;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
            {
                // A rebind in a nested control-flow block is not known to run before every later
                // use of the bonded value. Retire only a same-block store.
                if (it->second.BondDeclBlock == nullptr
                    || it->second.BondDeclBlock != here
                    || it->second.BondDeclFunction != function)
                    return;
                it->second.IsBonded = false;
                it->second.BondedSources.clear();
                it->second.BondDeclBlock = nullptr;
                it->second.BondDeclFunction = nullptr;
                return;
            }
        }
    }

bool LLVMBackend::IsBorrowingContainerElementSink(const std::string& functionName,
        const std::vector<TypeAndValue>& params, size_t paramIndex, bool isMethod) const
{
        // The receiver occupies params[0], so an element slot is never index 0. In every core
        // element-storing method the element is the LAST parameter (`set`/`insert`/dictionary
        // `add` take the index/key first), which pins the slot without a per-method table.
        if (!isMethod || paramIndex == 0 || params.size() < 2) return false;
        if (paramIndex != params.size() - 1) return false;

        std::string receiver = params[0].TypeName;
        if (!params[0].Pointer) return false;
        size_t sep = receiver.find("__");
        if (sep == std::string::npos) return false;   // not a generic instantiation
        // The template key keeps its namespace ("mylib.list"); the method table below is keyed
        // on the bare name.
        std::string qualified = receiver.substr(0, sep);
        std::string base = qualified;
        if (size_t dot = base.rfind('.'); dot != std::string::npos)
            base = base.substr(dot + 1);
        // Origin gate: only a template DECLARED in a core library file has the borrow semantics
        // this predicate encodes. A user-defined `stack<T>` (or `mylib.list<T>`) owns whatever
        // its own code says it owns and is none of this rule's business.
        if (gts.coreGenericTemplates.count(qualified) == 0) return false;

        // The element-storing methods, exactly as core/list.cb, dictionary.cb, queue.cb and
        // stack.cb declare them. hashset's `add(alias T value)` is deliberately absent: it
        // declares its parameter a borrow and offers no `move` overload for a pointer element,
        // so there would be no remedy to name.
        bool storing =
            (base == "list"       && (functionName == "add" || functionName == "set"
                                      || functionName == "insert"))
            || (base == "dictionary" && (functionName == "add" || functionName == "set"))
            || (base == "queue"      && functionName == "enqueue")
            || (base == "stack"      && functionName == "push");
        if (!storing) return false;

        // Bare pointer element only. `unique` (a real sink), `alias` (the opt-in borrow
        // spelling), an interface fat value and any by-value element all answer false.
        const TypeAndValue& elem = params[paramIndex];
        if (!elem.Pointer || elem.IsArrayView) return false;
        if (elem.IsInterfacePointer || elem.IsFatInterfaceValue() || elem.IsFunctionPointer)
            return false;
        if (elem.IsMove || elem.IsAlias || elem.IsUnique || elem.IsUniqueTypeArg) return false;
        if (elem.IsBorrowOfUniqueElement || elem.IsBorrowOfAliasElement) return false;
        return true;
    }

bool LLVMBackend::RejectOwningLocalIntoBorrowingContainer(const std::string& functionName,
        const std::vector<TypeAndValue>& params, size_t paramIndex, bool isMethod,
        const NamedVariable& arg)
{
        if (!IsBorrowingContainerElementSink(functionName, params, paramIndex, isMethod))
            return false;
        // `add(move p)` IS the remedy - it must reach the transfer, not this reject.
        if (arg.IsExplicitMove || arg.TypeAndValue.IsMove) return false;
        // Prove an owning POINTER binding. Everything unproven accepts.
        if (!arg.TypeAndValue.Pointer || arg.TypeAndValue.IsArrayView) return false;
        if (arg.TypeAndValue.IsUnique || arg.TypeAndValue.IsUniqueTypeArg) return false;
        if (arg.IsBorrowed || arg.IsAliasBorrow || arg.BorrowsOwnedElement) return false;
        if (!arg.IsOwning) return false;
        // Not live any more: already transferred, or handed to an interface box.
        if (arg.IsMoved || arg.ExplicitlyMovedNull || arg.MovedIntoInterface) return false;
        // A NAMED local of this frame: an rvalue (`new B()`) has no name, a global/static has
        // non-alloca storage, and a parameter (borrowed or `move`) is the caller's business.
        if (arg.CallerName.empty() || !arg.FieldName.empty()) return false;
        if (arg.IsStaticLocal) return false;
        if (arg.Storage == nullptr || !llvm::isa<llvm::AllocaInst>(arg.Storage)) return false;
        if (IsFunctionParameter(arg.CallerName)) return false;
        // The binding must still be the owning one the flags describe.
        if (!IsVariableOwning(arg.CallerName)) return false;
        // A `unique T*` local is an owner with a DECLARED policy: it is the spelling the ruling
        // keeps for "this object is owned here and the container borrows it", so a borrow-add
        // from one is the sanctioned borrow-collection shape and stays legal. The written
        // qualifier lives on the DECLARATION - a read hands out a plain pointer value.
        if (const NamedVariable* decl = FindVariableByStorage(arg.Storage))
            if (decl->TypeAndValue.IsUnique || decl->TypeAndValue.IsUniqueTypeArg) return false;

        LogErrorMessage(
            "call to '{}': '{}' still owns the object it was given, and frees it when it goes out "
            "of scope - but this container only BORROWS its elements and never frees them, so the "
            "stored element would dangle. Transfer the object with '{}', or declare the container's "
            "element '{}' so the container owns it.",
            { functionName, arg.CallerName,
              std::format("{}({} {})", functionName, "move", arg.CallerName), "unique T*" });
        return true;
    }

void LLVMBackend::MarkVariableMoved(const std::string& name)
{
        if (name.empty()) return;
        RecordMoveKill(name);
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                { it->second.IsMoved = true; return; }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                { it->second.IsMoved = true; return; }
        }
    }

void LLVMBackend::MarkVariableMovedIntoInterface(const std::string& name)
{
        if (name.empty()) return;
        RecordMoveKill(name);
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                { it->second.MovedIntoInterface = true; return; }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                { it->second.MovedIntoInterface = true; return; }
        }
    }

void LLVMBackend::SetViewOfFixedArrayStorage(const std::string& name, bool value, const std::string& sourceName)
{
        if (name.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
            {
                it->second.ViewOfFixedArrayStorage = value;
                it->second.ViewOfFixedArraySourceName = value ? sourceName : std::string();
                return;
            }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
            {
                it->second.ViewOfFixedArrayStorage = value;
                it->second.ViewOfFixedArraySourceName = value ? sourceName : std::string();
                return;
            }
        }
    }

void LLVMBackend::SetInterfaceBoxIsBorrowed(const std::string& name, bool borrowed,
                                   const std::string& sourceName)
{
        if (name.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            NamedVariable* nv = nullptr;
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                nv = &it->second;
            else if (auto it2 = frame.functionArgument.find(name); it2 != frame.functionArgument.end())
                nv = &it2->second;
            if (nv == nullptr) continue;
            if (!borrowed)
            {
                nv->InterfaceBoxProvenanceUnknown = true;
                nv->BorrowedInterfaceBox = false;
                nv->BorrowedInterfaceBoxSource.clear();
                nv->BorrowedInterfaceBoxSlots.clear();
                return;
            }
            if (nv->InterfaceBoxProvenanceUnknown) return;
            nv->BorrowedInterfaceBox = true;
            nv->BorrowedInterfaceBoxSource = sourceName;
            return;
        }
    }

void LLVMBackend::SetInterfaceBoxBorrowSlots(const std::string& name,
                                              const std::vector<llvm::Value*>& slots)
{
        if (name.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            NamedVariable* nv = nullptr;
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                nv = &it->second;
            else if (auto it2 = frame.functionArgument.find(name); it2 != frame.functionArgument.end())
                nv = &it2->second;
            if (nv == nullptr) continue;
            nv->BorrowedInterfaceBoxSlots = slots;
            return;
        }
    }

void LLVMBackend::MarkPointerRebound(const std::string& name, const std::string& inheritedOwner,
                            bool coalesceJoin, bool reboundToOwnedValue)
{
        if (name.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            NamedVariable* nv = nullptr;
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                nv = &it->second;
            else if (auto it2 = frame.functionArgument.find(name); it2 != frame.functionArgument.end())
                nv = &it2->second;
            if (nv == nullptr) continue;
            nv->PointerRebound = true;
            // Recorded, not consulted, here: a retiring consumer must also prove the store was
            // reached (same basic block), which is why the block travels with the bit.
            nv->ReboundToOwnedValue = reboundToOwnedValue;
            nv->ReboundBlock = reboundToOwnedValue ? builder->GetInsertBlock() : nullptr;
            nv->ReboundFunction = nv->ReboundBlock != nullptr ? nv->ReboundBlock->getParent() : nullptr;
            nv->InheritedKeepsOwner = !inheritedOwner.empty();
            nv->InheritedKeepsOwnerSource = inheritedOwner;
            nv->CoalesceRebound = coalesceJoin;
            // Written unconditionally so a plain '=' retires whatever join a declaration (or an
            // earlier store) recorded; SetJoinKeepsOwner re-arms it when THIS RHS is such a join.
            nv->JoinKeepsOwner = false;
            nv->JoinKeepsOwnerSource.clear();
            nv->JoinKeepsOwnerSlots.clear();
            return;
        }
    }

void LLVMBackend::SetJoinKeepsOwner(const std::string& name, const std::string& owner,
                           const std::vector<llvm::Value*>& slots)
{
        if (name.empty() || owner.empty() || slots.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            NamedVariable* nv = nullptr;
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                nv = &it->second;
            else if (auto it2 = frame.functionArgument.find(name); it2 != frame.functionArgument.end())
                nv = &it2->second;
            if (nv == nullptr) continue;
            nv->JoinKeepsOwner = true;
            nv->JoinKeepsOwnerSource = owner;
            nv->JoinKeepsOwnerSlots = slots;
            return;
        }
    }

void LLVMBackend::RecordAssignBorrow(const std::string& name, const std::string& origin,
                           const std::string& uniqueField, bool throughField, bool keepExistingOrigin,
                           bool uniqueFieldViaCall)
{
        if (name.empty() || origin.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            NamedVariable* nv = nullptr;
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                nv = &it->second;
            else if (auto it2 = frame.functionArgument.find(name); it2 != frame.functionArgument.end())
                nv = &it2->second;
            if (nv == nullptr) continue;
            // A binding that already owns what it holds is not made a borrow by this store; the
            // declaration path skips the same two cases for the same reason.
            if (nv->IsOwning || nv->IsNewAllocated) return;
            // A '??=' keeps the OLD referent when its arm is not taken, so it may not overwrite the
            // origin an existing borrow names - only supply one where there was none.
            if (keepExistingOrigin && nv->IsBorrowed && !nv->BorrowedOrigin.empty()) return;
            nv->IsBorrowed = true;
            nv->BorrowedOrigin = origin;
            nv->BorrowedUniqueField = uniqueField;
            nv->BorrowedUniqueFieldViaCall = uniqueFieldViaCall;
            nv->BorrowedThroughField = throughField;
            nv->AssignBorrowBlock = builder->GetInsertBlock();
            return;
        }
    }

void LLVMBackend::SetPointsToBorrowedByValueParam(const std::string& name, bool value)
{
        if (name.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            NamedVariable* nv = nullptr;
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                nv = &it->second;
            else if (auto it2 = frame.functionArgument.find(name); it2 != frame.functionArgument.end())
                nv = &it2->second;
            if (nv == nullptr) continue;
            nv->PointsToBorrowedByValueParam = value;
            return;
        }
    }

void LLVMBackend::RetireAssignBorrow(const std::string& name)
{
        if (name.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            NamedVariable* nv = nullptr;
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                nv = &it->second;
            else if (auto it2 = frame.functionArgument.find(name); it2 != frame.functionArgument.end())
                nv = &it2->second;
            if (nv == nullptr) continue;
            if (nv->AssignBorrowBlock == nullptr
                || nv->AssignBorrowBlock != builder->GetInsertBlock())
                return;
            nv->IsBorrowed = false;
            nv->BorrowedOrigin.clear();
            nv->BorrowedUniqueField.clear();
            nv->BorrowedUniqueFieldViaCall = false;
            nv->BorrowedThroughField = false;
            nv->AssignBorrowBlock = nullptr;
            return;
        }
    }

bool LLVMBackend::IsFunctionParameterStorage(const llvm::Value* slot) const
{
        if (slot == nullptr) return false;
        for (const auto& frame : stackNamedVariable)
            for (const auto& [varName, nv] : frame.functionArgument)
                if (nv.Storage == slot) return true;
        return false;
    }

void LLVMBackend::MarkVariableUnmoved(const std::string& name)
{
        if (name.empty()) return;
        RecordMoveGenRevive(name);
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                { it->second.IsMoved = false; return; }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                { it->second.IsMoved = false; return; }
        }
    }

void LLVMBackend::MarkVariableExplicitlyMovedNull(const std::string& name)
{
        if (name.empty() || !builder) return;
        llvm::BasicBlock* bb = builder->GetInsertBlock();
        if (!bb) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
            {
                if (it->second.AddressEscaped) return;
                it->second.ExplicitlyMovedNull = true;
                it->second.ExplicitNullBlock = bb;
                RecordNullSet(name);
                return;
            }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
            {
                if (it->second.AddressEscaped) return;
                it->second.ExplicitlyMovedNull = true;
                it->second.ExplicitNullBlock = bb;
                RecordNullSet(name);
                return;
            }
        }
    }

void LLVMBackend::MarkVariableNotExplicitlyMovedNull(const std::string& name)
{
        if (name.empty()) return;
        RecordNullClear(name);
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                { it->second.ExplicitlyMovedNull = false; it->second.ExplicitNullBlock = nullptr; return; }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                { it->second.ExplicitlyMovedNull = false; it->second.ExplicitNullBlock = nullptr; return; }
        }
    }

void LLVMBackend::MarkVariableAddressEscaped(const std::string& name)
{
        if (name.empty()) return;
        RecordNullEscape(name);
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
            {
                it->second.AddressEscaped = true;
                it->second.ExplicitlyMovedNull = false;
                it->second.ExplicitNullBlock = nullptr;
                return;
            }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
            {
                it->second.AddressEscaped = true;
                it->second.ExplicitlyMovedNull = false;
                it->second.ExplicitNullBlock = nullptr;
                return;
            }
        }
    }

bool LLVMBackend::IsExplicitlyMovedNullHere(const NamedVariable& nv) const
{
        return nv.ExplicitlyMovedNull && !nv.AddressEscaped && suppressExplicitNullDerefGuard_ == 0
            && builder && nv.ExplicitNullBlock == builder->GetInsertBlock();
    }

void LLVMBackend::RecordNullDerefFor(const NamedVariable& nv, int line, int col)
{
        if (nv.CallerName.empty() || !nv.FieldName.empty() || nv.AddressEscaped) return;
        if (!(nv.ExplicitlyMovedNull || nv.IsOwning || nv.TypeAndValue.IsUnique
              || nv.TypeAndValue.IsUniqueTypeArg))
            return;
        RecordNullDeref(nv.CallerName, line, col);
    }

void LLVMBackend::MarkVariableFieldMoved(const std::string& name, const std::string& field)
{
        if (name.empty() || field.empty()) return;
        RecordMoveKillField(name, field);
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                { it->second.MovedFields.insert(field); return; }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                { it->second.MovedFields.insert(field); return; }
        }
    }

void LLVMBackend::MarkVariableFieldUnmoved(const std::string& name, const std::string& field)
{
        if (name.empty() || field.empty()) return;
        RecordMoveGenReviveField(name, field);
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                { it->second.MovedFields.erase(field); return; }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                { it->second.MovedFields.erase(field); return; }
        }
    }

std::string LLVMBackend::MovedUseSubject(const NamedVariable& nv) const
{
        if (nv.IsMoved) return nv.CallerName;
        if (!nv.FieldName.empty() && nv.MovedFields.count(nv.FieldName))
            return nv.CallerName + "." + nv.FieldName;
        return "";
    }
