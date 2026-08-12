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

// ---- Definitions moved out of LLVMBackend.h (ControlFlowAndFunctions) ----

void LLVMBackend::DumpState() const
{
        std::cout << std::format("  File: {}\n", sourceFileName.empty() ? "<unknown>" : sourceFileName);
        std::cout << std::format("  Location: {}:{}\n", currentLine, currentColumn);

        if (currentFunction)
            std::cout << std::format("  Function: {}\n", currentFunction->getName().str());
        else
            std::cout << "  Function: <none>\n";

        std::cout << std::format("  Scope depth: {}\n", stackNamedVariable.size());
        if (!stackNamedVariable.empty())
        {
            const auto& top = stackNamedVariable.back();
            std::cout << "  Top scope locals:";
            for (const auto& [name, _] : top.namedVariable)
                std::cout << " " << name;
            std::cout << "\n";
        }

        std::cout << std::format("  Structs registered: {}\n", dataStructures.size());
        std::cout << std::format("  Functions registered: {}\n", functionTable.size());
    }

void LLVMBackend::InitDebugInfo(const std::string& filename, const std::string& directory)
{
        diBuilder = std::make_unique<llvm::DIBuilder>(*module);
        module->addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
        // Target is *-pc-windows-msvc: emit CodeView so lld-link can produce a PDB.
        // DWARF would be ignored by VS / WinDbg / cppvsdbg on Windows.
        module->addModuleFlag(llvm::Module::Warning, "CodeView", 1);
        diFile = diBuilder->createFile(filename, directory);
        compileUnit = diBuilder->createCompileUnit(llvm::dwarf::DW_LANG_C99, diFile, "cflat", false, "", 0);
        // Seed cache so the primary translation unit's path resolves to the same
        // DIFile node that the compile unit references (avoids duplicates).
        if (!currentSourceFilePath_.empty())
            diFileCache_[currentSourceFilePath_] = diFile;
    }

void LLVMBackend::FinalizeDebugInfo()
{
        if (!diBuilder) return;
        // Emit DI for all globals now that struct types are fully laid out.
        for (auto& p : pendingGlobalDI_)
        {
            auto* diType = GetDIType(p.typeValue);
            auto* diGVE = diBuilder->createGlobalVariableExpression(
                p.scope != nullptr ? p.scope : compileUnit,
                p.typeValue.VariableName, p.gVar->getName(),
                p.file, p.line, diType,
                p.isLocalToUnit, /*isDefined*/ true);
            p.gVar->addDebugInfo(diGVE);
        }
        pendingGlobalDI_.clear();
        diBuilder->finalize();
    }

void LLVMBackend::SetCurrentDebugLocation(size_t line, size_t col)
{
        if (!currentSubprogram || !diBuilder) return;
        builder->SetCurrentDebugLocation(llvm::DILocation::get(*context, (unsigned)line, (unsigned)col, currentSubprogram));
    }

void LLVMBackend::ClearCurrentSubprogram()
{
        currentSubprogram = nullptr;
        builder->SetCurrentDebugLocation(llvm::DebugLoc());
    }

void LLVMBackend::AbortFunctionBlocks(size_t targetDepth)
{
        for (auto& fn : *module)
        {
            for (auto& bb : fn)
            {
                if (!bb.getTerminator())
                {
                    builder->SetInsertPoint(&bb);
                    builder->CreateUnreachable();
                }
            }
        }
        while (stackNamedVariable.size() > targetDepth)
            stackNamedVariable.pop_back();
    }

LLVMBackend::BuilderState LLVMBackend::SaveBuilderState()
{
        BuilderState s{ builder->saveIP(), currentFunction, currentSubprogram, builder->getCurrentDebugLocation(),
                        currentFunctionReturnsOwned, currentFunctionReturnIsArrayView, currentFunctionReturnTypeName,
                        currentFunctionReturnTV };
        // Park the outer function's pending owned temps in the saved state and start the
        // nested emission with empty lists (see the BuilderState field comment).
        s.pendingStringTemps  = std::move(pendingOwnedStringTemps);
        s.pendingClosureTemps = std::move(pendingOwnedClosureTemps);
        s.pendingStructTemps  = std::move(pendingOwnedStructTemps);
        s.pendingPtrTemps     = std::move(pendingOwnedPtrTemps);
        s.ownedReturnTemps    = std::move(ownedReturnTemps_);
        s.ownedReturnReleaseTemps = std::move(ownedReturnReleaseTemps_);
        s.ownedNewTemps       = std::move(ownedNewTemps_);
        s.valueElementTypeNames = std::move(valueElementTypeNames_);
        s.fatInterfaceValueTypeNames = std::move(fatInterfaceValueTypeNames_);
        s.interfaceBoxRecords = std::move(interfaceBoxRecords_);
        s.nullCoalesceJoins   = std::move(nullCoalesceJoins_);
        s.joinArmOccurrences  = std::move(joinArmOccurrences_);
        s.codeValues          = std::move(codeValues_);
        s.dataValues          = std::move(dataValues_);
        s.codeValueDataCasts  = std::move(codeValueDataCasts_);
        s.owningTempUniqueFields = std::move(owningTempUniqueFields_);
        s.launderedTempUniqueFields = std::move(launderedTempUniqueFields_);
        s.pendingLaunderTempUniqueFields = std::move(pendingLaunderTempUniqueFields_);
        s.dataValueCodeCasts  = std::move(dataValueCodeCasts_);
        s.savedCastOccurrence = currentCastOccurrence_;
        currentCastOccurrence_ = 0;
        s.movedOutPtrValues   = std::move(movedOutPtrValues_);
        s.movedBorrowedPtrValues = std::move(movedBorrowedPtrValues_);
        s.movedBorrowedThroughFieldValues = std::move(movedBorrowedThroughFieldValues_);
        s.nonOwningStructJoins = std::move(nonOwningStructJoins_);
        s.uniqueFieldReadValues = std::move(uniqueFieldReadValues_);
        s.uniqueFieldReadJoins = std::move(uniqueFieldReadJoins_);
        // Mark the function we are leaving mid-body INCOMPLETE for the escape analysis
        // (see FunctionBodyIsComplete); RestoreBuilderState pops it back off.
        if (currentFunction != nullptr) suspendedFunctions_.push_back(currentFunction);
        pendingOwnedStringTemps.clear();
        pendingOwnedClosureTemps.clear();
        pendingOwnedStructTemps.clear();
        pendingOwnedPtrTemps.clear();
        ownedReturnTemps_.clear();
        ownedReturnReleaseTemps_.clear();
        ownedNewTemps_.clear();
        valueElementTypeNames_.clear();
        fatInterfaceValueTypeNames_.clear();
        interfaceBoxRecords_.clear();
        nullCoalesceJoins_.clear();
        joinArmOccurrences_.clear();
        codeValues_.clear();
        dataValues_.clear();
        codeValueDataCasts_.clear();
        owningTempUniqueFields_.clear();
        launderedTempUniqueFields_.clear();
        pendingLaunderTempUniqueFields_.clear();
        dataValueCodeCasts_.clear();
        movedOutPtrValues_.clear();
        movedBorrowedPtrValues_.clear();
        movedBorrowedThroughFieldValues_.clear();
        nonOwningStructJoins_.clear();
        uniqueFieldReadValues_.clear();
        uniqueFieldReadJoins_.clear();
        return s;
    }

void LLVMBackend::RestoreBuilderState(const BuilderState& state)
{
        builder->restoreIP(state.ip);
        // Pop the suspend marker SaveBuilderState pushed for this frame (see suspendedFunctions_).
        if (!suspendedFunctions_.empty() && suspendedFunctions_.back() == state.function)
            suspendedFunctions_.pop_back();
        currentFunction = state.function;
        currentSubprogram = state.subprogram;
        builder->SetCurrentDebugLocation(state.debugLoc);
        currentFunctionReturnsOwned = state.returnsOwned;
        currentFunctionReturnIsArrayView = state.returnIsArrayView;
        currentFunctionReturnTypeName = state.returnTypeName;
        currentFunctionReturnTV  = state.returnTV;
        pendingOwnedStringTemps  = state.pendingStringTemps;
        pendingOwnedClosureTemps = state.pendingClosureTemps;
        pendingOwnedStructTemps  = state.pendingStructTemps;
        pendingOwnedPtrTemps     = state.pendingPtrTemps;
        ownedReturnTemps_        = state.ownedReturnTemps;
        ownedReturnReleaseTemps_ = state.ownedReturnReleaseTemps;
        ownedNewTemps_           = state.ownedNewTemps;
        valueElementTypeNames_   = state.valueElementTypeNames;
        fatInterfaceValueTypeNames_ = state.fatInterfaceValueTypeNames;
        interfaceBoxRecords_    = state.interfaceBoxRecords;
        nullCoalesceJoins_      = state.nullCoalesceJoins;
        joinArmOccurrences_     = state.joinArmOccurrences;
        codeValues_             = state.codeValues;
        dataValues_             = state.dataValues;
        codeValueDataCasts_     = state.codeValueDataCasts;
        owningTempUniqueFields_ = state.owningTempUniqueFields;
        launderedTempUniqueFields_ = state.launderedTempUniqueFields;
        pendingLaunderTempUniqueFields_ = state.pendingLaunderTempUniqueFields;
        dataValueCodeCasts_     = state.dataValueCodeCasts;
        currentCastOccurrence_  = state.savedCastOccurrence;
        movedOutPtrValues_       = state.movedOutPtrValues;
        movedBorrowedPtrValues_  = state.movedBorrowedPtrValues;
        movedBorrowedThroughFieldValues_ = state.movedBorrowedThroughFieldValues;
        nonOwningStructJoins_    = state.nonOwningStructJoins;
        uniqueFieldReadValues_   = state.uniqueFieldReadValues;
        uniqueFieldReadJoins_    = state.uniqueFieldReadJoins;
    }

bool LLVMBackend::IsWinrtProjectedType(const std::string& name) const
{
        return winrtThinInterfaces_.count(name) != 0 || winrtValueStructs_.count(name) != 0
            || IsWinrtFullName(name);
    }

bool LLVMBackend::RelativePathEscapesUp(const std::filesystem::path& rel)
{
        return rel.string().rfind("..", 0) == 0;
    }

std::filesystem::path LLVMBackend::DefSitePath(const std::string& site)
{
        auto openParen = site.rfind('(');
        if (openParen == std::string::npos) return {};
        return std::filesystem::path(site.substr(0, openParen));
    }

std::string LLVMBackend::InstalledCoreRelative(const std::filesystem::path& p) const
{
        if (runtimeDir.empty()) return {};
        std::error_code ec;
        auto coreDir = std::filesystem::weakly_canonical(std::filesystem::path(runtimeDir) / "core", ec);
        if (ec) return {};
        auto rel = p.lexically_relative(coreDir);
        if (rel.empty() || RelativePathEscapesUp(rel)) return {};
        return rel.string();
    }

std::string LLVMBackend::TailBelowCoreDir(const std::filesystem::path& p)
{
        std::vector<std::filesystem::path> parts(p.begin(), p.end());
        for (size_t k = parts.size(); k-- > 0;)
        {
            if (k + 1 >= parts.size() || parts[k].string() != "core") continue;
            std::filesystem::path tail;
            for (size_t j = k + 1; j < parts.size(); ++j) tail /= parts[j];
            return tail.string();
        }
        return {};
    }

bool LLVMBackend::IsSameCoreFileDefSite(const std::string& siteA, const std::string& siteB) const
{
        auto pathA = DefSitePath(siteA), pathB = DefSitePath(siteB);
        if (pathA.empty() || pathB.empty()) return false;

        std::string installedA = InstalledCoreRelative(pathA), installedB = InstalledCoreRelative(pathB);
        if (installedA.empty() == installedB.empty()) return false;

        const std::string& installedRel = installedA.empty() ? installedB : installedA;
        const std::filesystem::path& other = installedA.empty() ? pathA : pathB;
        std::string otherTail = TailBelowCoreDir(other);
        return !otherTail.empty() && otherTail == installedRel;
    }

std::string LLVMBackend::ShortenDefSiteForDisplay(const std::string& site, bool isCore) const
{
        auto openParen = site.rfind('(');
        if (openParen == std::string::npos) return site;
        std::filesystem::path p(site.substr(0, openParen));
        std::string suffix = site.substr(openParen);

        if (isCore)
        {
            std::string rel = InstalledCoreRelative(p);
            if (!rel.empty())
                return (std::filesystem::path("core") / rel).string() + suffix;
        }

        std::error_code ec;
        auto rel = std::filesystem::relative(p, ec);
        if (!ec && !rel.empty() && !RelativePathEscapesUp(rel))
            return rel.string() + suffix;
        return p.filename().string() + suffix;
    }

llvm::BasicBlock* LLVMBackend::CreateBasicBlock(std::string name, llvm::Function* fn)
{
        if (fn == nullptr)
            fn = currentFunction;

        return llvm::BasicBlock::Create(*context, name, fn);
    }

void LLVMBackend::SwitchToBlock(llvm::BasicBlock* block)
{
        builder->SetInsertPoint(block);
    }

llvm::BranchInst* LLVMBackend::CreateJump(llvm::BasicBlock* block)
{
        if (block && IsInsertBlockLive())
            return builder->CreateBr(block);
        return nullptr;
    }

llvm::Type* LLVMBackend::GetFunctionReturnType(const std::string& functionName) const
{
        auto it = functionTable.find(functionName);
        if (it == functionTable.end() || it->second.empty())
            return nullptr;
        return GetType(it->second.front().ReturnType);
    }

LLVMBackend::TypeAndValue LLVMBackend::GetFunctionReturnTypeInfo(const std::string& functionName) const
{
        auto it = functionTable.find(functionName);
        if (it == functionTable.end() || it->second.empty())
            return {};
        return it->second.front().ReturnType;
    }

std::string LLVMBackend::CreateAnonFunctionName()
{
        return "__lambda_" + std::to_string(lambdaCounter++);
    }

LLVMBackend::TypeAndValue LLVMBackend::MakeFuncPtrTypeAndValue(const std::string& functionName) const
{
        auto it = functionTable.find(functionName);
        if (it == functionTable.end() || it->second.empty())
            return {};

        // Prefer non-method overloads so that a plain function name isn't
        // shadowed by a struct method registered under the same key.
        const FunctionSymbol* chosen = &it->second.front();
        for (const auto& sym : it->second)
        {
            if (!sym.IsMethod) { chosen = &sym; break; }
        }

        TypeAndValue tv;
        tv.IsFunctionPointer = true;
        tv.FuncPtrReturnTypeName = chosen->ReturnType.TypeName;
        tv.FuncPtrReturnPointer = chosen->ReturnType.Pointer;
        tv.FuncPtrReturnOwned = chosen->ReturnType.IsMove
            || (chosen->ReturnType.IsUniqueTypeArg && chosen->ReturnType.Pointer);
        tv.FuncPtrReturnPointerDepth = chosen->ReturnType.ValuePointerDepth();
        for (const auto& p : chosen->Parameters)
        {
            TypeAndValue::FuncPtrParam fp;
            fp.TypeName = p.TypeName;
            fp.Pointer = p.Pointer;
            fp.AllocAlignValue = p.AllocAlignValue;
            fp.IsMove = p.IsMove;
            // Same as the Lookup twin: an inferred sink belongs to the function's signature.
            fp.IsOwningSink = p.IsOwningSink;
            fp.IsConsumeInferredSink = p.IsConsumeInferredSink;
            fp.PointerDepth = p.ValuePointerDepth();
            tv.FuncPtrParams.push_back(fp);
        }
        return tv;
    }

void LLVMBackend::CheckIndirectCallArgShape(llvm::Value* arg, llvm::Type* destTy, size_t index,
                                   const std::string& paramTypeName)
{
        if (arg == nullptr || destTy == nullptr) return;
        auto* stringTy = llvm::StructType::getTypeByName(*context, "string");
        if (arg->getType() == stringTy && destTy->isPointerTy()
            && (paramTypeName == "char" || paramTypeName == "i8"))
        {
            LogError(std::format(
                "cannot pass 'string' to the 'char*' parameter {} through a function value: a "
                "'string' is a {{ptr,len}} value, not a 'char*'. Pass the buffer explicitly with '.data()'.",
                index + 1));
            return;
        }
        if (!arg->getType()->isPointerTy() || destTy->isPointerTy()) return;

        // An unnamed slot gets the shorter message: naming a type it does not have, or advising a
        // '*' spelling of it, would be advice the caller cannot follow.
        bool writable = true;
        std::string shown = DisplayNameOfMangledType(paramTypeName, &writable);
        if (shown.empty())
        {
            LogError(std::format(
                "call through a function value: cannot pass a pointer as argument {} - that "
                "parameter is a by-value slot and there is no implicit dereference. Write '*' at "
                "the call site to pass the pointee.", index + 1));
            return;
        }
        // Same rule as the virtual site: a nested instantiation renders ambiguously, so it keeps
        // the raw name and loses the advice clause.
        std::string advice = writable
            ? std::format(", or declare the parameter as '{}*'", shown) : std::string();
        LogError(std::format(
            "call through a function value: cannot pass a pointer as argument {} - parameter {} is "
            "the by-value type '{}' and there is no implicit dereference. Write '*' at the call "
            "site to pass the pointee{}.",
            index + 1, index + 1, shown, advice));
    }

llvm::Value* LLVMBackend::CreateIndirectCall(const TypeAndValue& funcPtrType, llvm::Value* funcPtr, std::vector<llvm::Value*> args)
{
        auto* i8PtrTy = builder->getInt8Ty()->getPointerTo();

        // Thin `function<T>`: a bare C function pointer. Direct call, no env, exact C signature.
        if (funcPtrType.IsThinFnPtr())
        {
            std::vector<llvm::Type*> paramTypes;
            for (const auto& p : funcPtrType.FuncPtrParams)
            {
                TypeAndValue pTV; pTV.TypeName = p.TypeName; pTV.Pointer = p.Pointer;
                paramTypes.push_back(GetType(pTV));
            }
            TypeAndValue retTV;
            retTV.TypeName = funcPtrType.FuncPtrReturnTypeName;
            retTV.Pointer  = funcPtrType.FuncPtrReturnPointer;
            retTV.IsMove   = funcPtrType.FuncPtrReturnOwned;
            auto* retTy   = GetType(retTV);
            auto* cFnTy   = llvm::FunctionType::get(retTy, paramTypes, false);
            auto* fnPtr   = builder->CreateBitCast(funcPtr, cFnTy->getPointerTo(), "cfn_ptr");
            for (size_t i = 0; i < args.size() && i < paramTypes.size(); i++)
            {
                auto* destTy = paramTypes[i];
                auto* strTy  = llvm::StructType::getTypeByName(*context, "string");
                if (strTy && destTy == strTy && args[i]->getType()->isPointerTy())
                    args[i] = WrapStringLiteralAsString(args[i]);
                else
                    args[i] = Upconvert(args[i], destTy);
                CheckIndirectCallArgShape(args[i], destTy, i, funcPtrType.FuncPtrParams[i].TypeName);
            }
            lastCallReturnType = retTV;
            auto* result = builder->CreateCall(cFnTy, fnPtr, args);
            return retTy->isVoidTy() ? nullptr : result;
        }

        // Extract fn ptr (field 0) and env ptr (field 1) from the closure fat struct.
        auto* fnPtrI8 = builder->CreateExtractValue(funcPtr, {0u}, "fn_i8");
        auto* envPtr  = builder->CreateExtractValue(funcPtr, {1u}, "env_ptr");

        // Strip the OWNED tag (low bit) off the env - an owning heap env carries the tag (lambda
        // Option A) but the invoker expects a clean pointer. Borrowed/null env is unchanged.
        {
            auto* envInt = builder->CreatePtrToInt(envPtr, builder->getInt64Ty());
            auto* masked = builder->CreateAnd(envInt, builder->getInt64(~(uint64_t)1));
            envPtr = builder->CreateIntToPtr(masked, i8PtrTy, "env_untagged");
        }

        // Build invoker function type: (user_params..., i8* env) -> RetType (env-last ABI).
        std::vector<llvm::Type*> paramTypes;
        for (const auto& p : funcPtrType.FuncPtrParams)
        {
            TypeAndValue pTV; pTV.TypeName = p.TypeName; pTV.Pointer = p.Pointer;
            paramTypes.push_back(GetType(pTV));
        }
        paramTypes.push_back(i8PtrTy); // env (trailing)
        TypeAndValue retTV;
        retTV.TypeName = funcPtrType.FuncPtrReturnTypeName;
        retTV.Pointer  = funcPtrType.FuncPtrReturnPointer;
        retTV.IsMove   = funcPtrType.FuncPtrReturnOwned;
        auto* retTy     = GetType(retTV);
        auto* invokerTy = llvm::FunctionType::get(retTy, paramTypes, false);
        auto* fnPtr     = builder->CreateBitCast(fnPtrI8, invokerTy->getPointerTo(), "fn_ptr");

        // Upconvert user args to match declared param types (the trailing slot is env, skip it).
        // String literals arrive as i8* - wrap them into %string{ptr,len} when the
        // param expects a string value type.
        for (size_t i = 0; i < args.size() && i + 1 < paramTypes.size(); i++)
        {
            auto* destTy = paramTypes[i];
            auto* strTy  = llvm::StructType::getTypeByName(*context, "string");
            if (strTy && destTy == strTy && args[i]->getType()->isPointerTy())
                args[i] = WrapStringLiteralAsString(args[i]);
            else
                args[i] = Upconvert(args[i], destTy);
            CheckIndirectCallArgShape(args[i], destTy, i, funcPtrType.FuncPtrParams[i].TypeName);
        }

        // Append env to call args (env-last)
        std::vector<llvm::Value*> fullArgs(args.begin(), args.end());
        fullArgs.push_back(envPtr);

        lastCallReturnType = retTV;
        auto* result = builder->CreateCall(invokerTy, fnPtr, fullArgs);
        return retTy->isVoidTy() ? nullptr : result;
    }

llvm::SwitchInst* LLVMBackend::CreateSwitchInst(llvm::Value* cond, llvm::BasicBlock* defaultBlock, unsigned numCases)
{
        if (!cond->getType()->isIntegerTy())
            LogError("switch expression must be an integer type");
        return builder->CreateSwitch(cond, defaultBlock, numCases);
    }

llvm::ConstantInt* LLVMBackend::CoerceCaseValue(llvm::ConstantInt* val, llvm::Type* switchType)
{
        // getZExtValue avoids the isRepresentableByInt64 assert for constants > INT64_MAX.
        return llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(switchType), val->getZExtValue(), false);
    }

llvm::Function* LLVMBackend::GetOrDeclareStrcmp()
{
        if (auto* fn = module->getFunction("strcmp"))
            return fn;
        auto* i32Ty = builder->getInt32Ty();
        auto* ptrTy = builder->getInt8Ty()->getPointerTo();
        auto* fnTy = llvm::FunctionType::get(i32Ty, { ptrTy, ptrTy }, false);
        return llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage, "strcmp", module.get());
    }

llvm::Value* LLVMBackend::CoerceToBoolCondition(llvm::Value* cond)
{
        if (cond == nullptr || cond->getType()->isIntegerTy(1))
            return cond;

        if (cond->getType()->isPointerTy())
            return builder->CreateIsNotNull(cond);

        if (cond->getType()->isFloatingPointTy())
        {
            // Non-zero (and non-NaN-equal) floating point is true.
            return builder->CreateFCmpUNE(cond, llvm::ConstantFP::get(cond->getType(), 0.0), "tobool");
        }

        if (cond->getType()->isIntegerTy())
            return builder->CreateICmpNE(cond, llvm::ConstantInt::get(cond->getType(), 0), "tobool");

        // An aggregate (a `string`, any struct) has no truth value. Diagnose it here rather than
        // handing it to CreateCondBr / CreateSelect, which fails module verification opaquely.
        LogError(std::format(
            "condition must be a scalar (bool, integer, pointer or floating point), not '{}'"
            " - compare it explicitly",
            DescribeConditionType(cond->getType())));
        return builder->getFalse();
    }

std::string LLVMBackend::DescribeConditionType(llvm::Type* t) const
{
        if (auto* st = llvm::dyn_cast<llvm::StructType>(t))
            return st->hasName() ? st->getName().str() : std::string("struct");
        if (t->isArrayTy())  return "array";
        if (t->isVectorTy()) return "vector";
        return "value";
    }

llvm::BranchInst* LLVMBackend::CreateConditionJump(llvm::Value* cond, llvm::BasicBlock* trueBlock, llvm::BasicBlock* falseBlock)
{
        cond = CoerceToBoolCondition(cond);

        auto branchInst = builder->CreateCondBr(cond, trueBlock, falseBlock);
        builder->SetInsertPoint(trueBlock);
        return branchInst;
    }

llvm::PHINode* LLVMBackend::CreatePHINode(std::string name, int reserve)
{
        return builder->CreatePHI(builder->getInt1Ty(), reserve, name);
    }

llvm::Value* LLVMBackend::CreateSelect(llvm::Value* cond, llvm::Value* falseValue, llvm::Value* trueValue)
{
        return builder->CreateSelect(cond, trueValue, falseValue);
    }

llvm::BranchInst* LLVMBackend::CreateBlockBreak(llvm::BasicBlock* resumeBlock, bool exitBlockStack)
{
        if (exitBlockStack)
        {
            // Bare-semicolon form: expect_error("msg"); - if the expected error never fired before this scope exits, report failure.
            if (!expectedError.empty() && expectedErrorScopeDepth == stackNamedVariable.size())
            {
                std::cout << std::format("FAIL: expected error '{}' did not occur\n", expectedError);
                expectedError.clear();
                expectedErrorScopeDepth = SIZE_MAX;
                FailCompilation("expected error did not occur");
            }
            EmitDestructorsForScope(stackNamedVariable.back());
            stackNamedVariable.pop_back();
        }

        if (resumeBlock)
        {
            if (IsInsertBlockLive())
                return builder->CreateBr(resumeBlock);
        }

        return nullptr;
    }

void LLVMBackend::InitializeBlock(llvm::BasicBlock* block, bool enterBlockStack, llvm::BasicBlock* continueBlock, llvm::BasicBlock* resumeBlock, llvm::BasicBlock* elseBlock)
{
        if (enterBlockStack)
        {
            auto& stack = stackNamedVariable.emplace_back();
            stack.continueBlock = continueBlock;
            stack.resumeBlock = resumeBlock;
            stack.elseBlock = elseBlock;
        }

        if (block)
        {
            builder->SetInsertPoint(block);
        }
    }

bool LLVMBackend::IsByValueStructTV(const TypeAndValue& tv) const
{
        if (tv.Pointer) return false;
        if (tv.IsInterface || tv.IsFunctionPointer) return false;
        if (tv.TypeName.empty()) return false;
        auto it = dataStructures.find(tv.TypeName);
        if (it == dataStructures.end()) return false;
        auto* st = it->second.StructType;
        return st != nullptr && !st->isOpaque();
    }

void LLVMBackend::CollectScalarFields(llvm::Type* ty, uint64_t base, const llvm::DataLayout& dl,
                             std::vector<std::pair<uint64_t, llvm::Type*>>& out) const
{
        if (auto* st = llvm::dyn_cast<llvm::StructType>(ty))
        {
            const llvm::StructLayout* sl = dl.getStructLayout(st);
            for (unsigned i = 0; i < st->getNumElements(); ++i)
                CollectScalarFields(st->getElementType(i), base + sl->getElementOffset(i), dl, out);
        }
        else if (auto* at = llvm::dyn_cast<llvm::ArrayType>(ty))
        {
            llvm::Type* el = at->getElementType();
            uint64_t esz = dl.getTypeAllocSize(el);
            for (uint64_t i = 0; i < at->getNumElements(); ++i)
                CollectScalarFields(el, base + i * esz, dl, out);
        }
        else
        {
            out.push_back({ base, ty });
        }
    }

std::vector<llvm::Type*> LLVMBackend::ClassifySysVStruct(llvm::StructType* st) const
{
        const llvm::DataLayout& dl = module->getDataLayout();
        uint64_t size = dl.getTypeAllocSize(st);
        if (size == 0 || size > 16) return {}; // MEMORY
        unsigned nEB = (unsigned)((size + 7) / 8);

        std::vector<std::pair<uint64_t, llvm::Type*>> fields;
        CollectScalarFields(st, 0, dl, fields);

        enum Cls { NoClass, Integer, Sse };
        Cls cls[2]      = { NoClass, NoClass };
        bool hasDouble[2] = { false, false };
        int  floatLanes[2] = { 0, 0 };
        for (const auto& f : fields)
        {
            uint64_t off = f.first;
            llvm::Type* t = f.second;
            uint64_t fsz = dl.getTypeAllocSize(t);
            // A scalar straddling an eightbyte boundary only happens with packing/misalignment;
            // bail to MEMORY rather than mis-coerce.
            if (off / 8 != (off + (fsz ? fsz - 1 : 0)) / 8) return {};
            unsigned eb = (unsigned)(off / 8);
            if (eb >= 2) return {};
            bool isSse = t->isFloatTy() || t->isDoubleTy();
            if (isSse)
            {
                if (cls[eb] == NoClass) cls[eb] = Sse;
                if (t->isDoubleTy()) hasDouble[eb] = true;
                else                 floatLanes[eb]++;
            }
            else
            {
                cls[eb] = Integer; // INTEGER wins over SSE in the same eightbyte
            }
        }

        std::vector<llvm::Type*> coerce;
        for (unsigned eb = 0; eb < nEB; ++eb)
        {
            uint64_t ebBytes = std::min<uint64_t>(8, size - (uint64_t)eb * 8);
            if (cls[eb] == Sse)
            {
                if (hasDouble[eb])
                    coerce.push_back(llvm::Type::getDoubleTy(*context));
                else if (floatLanes[eb] >= 2)
                    coerce.push_back(llvm::FixedVectorType::get(llvm::Type::getFloatTy(*context), 2));
                else
                    coerce.push_back(llvm::Type::getFloatTy(*context));
            }
            else // Integer or NoClass (empty eightbyte) -> integer of the covered bytes
            {
                coerce.push_back(llvm::Type::getIntNTy(*context, (unsigned)(ebBytes * 8)));
            }
        }
        return coerce;
    }

LLVMBackend::AbiSlot LLVMBackend::MakeCoerceSlot(llvm::StructType* st, uint64_t align,
                                  const std::vector<llvm::Type*>& coerce, AbiSlot::Kind memoryKind)
{
        AbiSlot slot;
        slot.structTy = st;
        slot.align    = align;
        if (coerce.empty()) { slot.kind = memoryKind; return slot; }
        if (coerce.size() == 1) { slot.kind = AbiSlot::CoerceToInt; slot.coerceTy = coerce[0]; return slot; }
        slot.kind = AbiSlot::CoercePair; slot.coerceTy = coerce[0]; slot.coerceTy2 = coerce[1];
        return slot;
    }

LLVMBackend::AbiSlot LLVMBackend::ClassifySysVParam(llvm::StructType* st, uint64_t align)
{
        return MakeCoerceSlot(st, align, ClassifySysVStruct(st), AbiSlot::ByVal);
    }

LLVMBackend::AbiSlot LLVMBackend::ClassifySysVReturn(llvm::StructType* st, uint64_t align)
{
        return MakeCoerceSlot(st, align, ClassifySysVStruct(st), AbiSlot::SRetReturn);
    }

bool LLVMBackend::IsAArch64HFA(llvm::StructType* st, llvm::Type*& base, unsigned& count) const
{
        const llvm::DataLayout& dl = module->getDataLayout();
        std::vector<std::pair<uint64_t, llvm::Type*>> fields;
        CollectScalarFields(st, 0, dl, fields);
        if (fields.empty() || fields.size() > 4) return false;
        llvm::Type* b = fields[0].second;
        if (!b->isFloatTy() && !b->isDoubleTy()) return false; // HVA (vectors) not handled
        for (const auto& f : fields)
            if (f.second != b) return false;
        // Reject padding/misalignment: a clean HFA is exactly count * sizeof(base).
        // Cast the (fixed) TypeSize values to uint64_t so the multiply is unambiguous
        // (AppleClang/libc++ rejects size_type * TypeSize otherwise).
        if ((uint64_t)dl.getTypeAllocSize(st) != fields.size() * (uint64_t)dl.getTypeAllocSize(b)) return false;
        base = b;
        count = (unsigned)fields.size();
        return true;
    }

std::vector<llvm::Type*> LLVMBackend::ClassifyAArch64Struct(llvm::StructType* st) const
{
        const llvm::DataLayout& dl = module->getDataLayout();
        uint64_t size = dl.getTypeAllocSize(st);
        if (size == 0) return {};

        llvm::Type* base = nullptr;
        unsigned hfaCount = 0;
        if (IsAArch64HFA(st, base, hfaCount))
        {
            llvm::Type* coerce = (hfaCount == 1)
                ? base
                : (llvm::Type*)llvm::ArrayType::get(base, hfaCount);
            return { coerce };
        }

        if (size > 16) return {}; // MEMORY
        if (size <= 8)
            return { llvm::Type::getIntNTy(*context, (unsigned)(size * 8)) };
        return { llvm::Type::getInt64Ty(*context),
                 llvm::Type::getIntNTy(*context, (unsigned)((size - 8) * 8)) };
    }

LLVMBackend::AbiSlot LLVMBackend::ClassifyAArch64Param(llvm::StructType* st, uint64_t align)
{
        return MakeCoerceSlot(st, align, ClassifyAArch64Struct(st), AbiSlot::ByVal);
    }

LLVMBackend::AbiSlot LLVMBackend::ClassifyAArch64Return(llvm::StructType* st, uint64_t align)
{
        return MakeCoerceSlot(st, align, ClassifyAArch64Struct(st), AbiSlot::SRetReturn);
    }

LLVMBackend::AbiSlot LLVMBackend::ClassifyAbiParam(const TypeAndValue& tv)
{
        AbiSlot slot;
        if (!IsByValueStructTV(tv)) return slot; // Direct
        auto* st = dataStructures.at(tv.TypeName).StructType;
        const llvm::DataLayout& dl = module->getDataLayout();
        uint64_t size  = dl.getTypeAllocSize(st);
        uint64_t align = dl.getABITypeAlign(st).value();
        if (targetArm64_)
            return ClassifyAArch64Param(st, align);
        if (!targetWindows_)
            return ClassifySysVParam(st, align);
        if (platformValue == 64 && (size == 1 || size == 2 || size == 4 || size == 8))
        {
            slot.kind     = AbiSlot::CoerceToInt;
            slot.coerceTy = llvm::Type::getIntNTy(*context, (unsigned)(size * 8));
            slot.structTy = st;
            slot.align    = align;
        }
        else
        {
            slot.kind     = AbiSlot::ByVal;
            slot.structTy = st;
            // On Win32 cdecl the parameter slot is 4-byte aligned (the stack itself is
            // S32-aligned), so byval must report align 4 even if the struct's natural
            // alignment is larger - mirroring clang. Otherwise the callee will use
            // aligned loads against an actually-misaligned stack slot.
            slot.align    = (platformValue == 32) ? std::min<uint64_t>(align, 4) : align;
        }
        return slot;
    }

LLVMBackend::AbiSlot LLVMBackend::ClassifyAbiReturn(const TypeAndValue& tv)
{
        AbiSlot slot;
        if (!IsByValueStructTV(tv)) return slot; // Direct
        auto* st = dataStructures.at(tv.TypeName).StructType;
        const llvm::DataLayout& dl = module->getDataLayout();
        uint64_t size  = dl.getTypeAllocSize(st);
        uint64_t align = dl.getABITypeAlign(st).value();
        if (targetArm64_)
            return ClassifyAArch64Return(st, align);
        if (!targetWindows_)
            return ClassifySysVReturn(st, align);
        if (size == 1 || size == 2 || size == 4 || size == 8)
        {
            slot.kind     = AbiSlot::CoerceToInt;
            slot.coerceTy = llvm::Type::getIntNTy(*context, (unsigned)(size * 8));
            slot.structTy = st;
            slot.align    = align;
        }
        else
        {
            slot.kind     = AbiSlot::SRetReturn;
            slot.structTy = st;
            slot.align    = align;
        }
        return slot;
    }

LLVMBackend::AbiRecipe LLVMBackend::ComputeAbiRecipe(const TypeAndValue& retType,
                               const std::vector<TypeAndValue>& params)
{
        AbiRecipe recipe;
        recipe.retSlot = ClassifyAbiReturn(retType);
        recipe.paramSlots.reserve(params.size());
        for (const auto& p : params)
            recipe.paramSlots.push_back(ClassifyAbiParam(p));
        recipe.hasLowering = (recipe.retSlot.kind != AbiSlot::Direct);
        if (!recipe.hasLowering)
            for (const auto& s : recipe.paramSlots)
                if (s.kind != AbiSlot::Direct) { recipe.hasLowering = true; break; }
        return recipe;
    }

llvm::FunctionType* LLVMBackend::BuildExternFunctionType(const TypeAndValue& retType,
                                                const std::vector<TypeAndValue>& params,
                                                bool varargs,
                                                const AbiRecipe& recipe)
{
        std::vector<llvm::Type*> ptypes;
        ptypes.reserve(params.size() + (recipe.retSlot.kind == AbiSlot::SRetReturn ? 1 : 0));

        llvm::Type* loweredRet = nullptr;
        if (recipe.retSlot.kind == AbiSlot::SRetReturn)
        {
            loweredRet = builder->getVoidTy();
            // Hidden sret pointer goes first.
            ptypes.push_back(recipe.retSlot.structTy->getPointerTo());
        }
        else if (recipe.retSlot.kind == AbiSlot::CoerceToInt)
            loweredRet = recipe.retSlot.coerceTy;
        else if (recipe.retSlot.kind == AbiSlot::CoercePair)
            loweredRet = llvm::StructType::get(*context, { recipe.retSlot.coerceTy, recipe.retSlot.coerceTy2 });
        else
            loweredRet = GetCCompatibleType(retType);

        for (size_t i = 0; i < params.size(); ++i)
        {
            const AbiSlot& s = recipe.paramSlots[i];
            if (s.kind == AbiSlot::CoerceToInt)
                ptypes.push_back(s.coerceTy);
            else if (s.kind == AbiSlot::CoercePair)
            {
                ptypes.push_back(s.coerceTy);
                ptypes.push_back(s.coerceTy2);
            }
            else if (s.kind == AbiSlot::ByVal)
                ptypes.push_back(s.structTy->getPointerTo());
            else
                ptypes.push_back(GetCCompatibleType(params[i]));
        }
        return llvm::FunctionType::get(loweredRet, ptypes, varargs);
    }

void LLVMBackend::ApplyAbiAttributes(llvm::Function* fn, const AbiRecipe& recipe)
{
        unsigned attrIdx = 0; // LLVM param attribute indices are 0-based on the function's actual param list
        if (recipe.retSlot.kind == AbiSlot::SRetReturn)
        {
            fn->addParamAttr(attrIdx, llvm::Attribute::getWithStructRetType(*context, recipe.retSlot.structTy));
            fn->addParamAttr(attrIdx, llvm::Attribute::NoAlias);
            if (recipe.retSlot.align > 0)
                fn->addParamAttr(attrIdx, llvm::Attribute::getWithAlignment(*context, llvm::Align(recipe.retSlot.align)));
            ++attrIdx;
        }
        for (size_t i = 0; i < recipe.paramSlots.size(); ++i)
        {
            const AbiSlot& s = recipe.paramSlots[i];
            if (s.kind == AbiSlot::ByVal)
            {
                fn->addParamAttr(attrIdx, llvm::Attribute::getWithByValType(*context, s.structTy));
                if (s.align > 0)
                    fn->addParamAttr(attrIdx, llvm::Attribute::getWithAlignment(*context, llvm::Align(s.align)));
            }
            attrIdx += SlotLLVMParamCount(s); // CoercePair expands to two LLVM params
        }
    }

unsigned LLVMBackend::SlotLLVMParamCount(const AbiSlot& s)
{
        return s.kind == AbiSlot::CoercePair ? 2u : 1u;
    }

void LLVMBackend::CreateFunctionDeclaration(std::string functionName, LLVMBackend::TypeAndValue returnType, std::vector<LLVMBackend::TypeAndValue> arguments, bool external, bool varargs, bool returnsOwned, bool isMethod, CallingConv callConv, const std::string& linkageName)
{
        // For extern C declarations, compute an ABI recipe so struct-by-value params/returns
        // are lowered (coerce-to-int / byval / sret) per the Win64 or Win32 MSVC ABI. If the
        // recipe has no lowering (scalar/pointer only) the existing GetFunctionType path is used.
        AbiRecipe recipe;
        bool useRecipe = false;
        if (external)
        {
            recipe = ComputeAbiRecipe(returnType, arguments);
            useRecipe = recipe.hasLowering;
        }

        llvm::FunctionType* functionType = useRecipe
            ? BuildExternFunctionType(returnType, arguments, varargs, recipe)
            : GetFunctionType(returnType, arguments, varargs, external);
        std::string mangledName = external ? (linkageName.empty() ? functionName : linkageName)
                                           : ComputeMangledName(functionName, returnType, arguments, varargs);

        if (llvm::Function* existing = module->getFunction(mangledName))
        {
            // A repeat declaration under the same lookup name is a no-op. A *different*
            // lookup name for an already-emitted linkage symbol (core's os.windows.Sleep
            // and a header-imported bare Sleep) still registers below, reusing the
            // existing llvm::Function via getOrInsertFunction.
            for (const auto& sym : functionTable[functionName])
                if (sym.UniqueName == mangledName)
                    return;

            // The linkage symbol already exists with a DIFFERENT signature. This happens
            // when a user `extern` collides with a core-library extern of the same name
            // (e.g. fwrite, declared in os.windows with 32-bit params). getOrInsertFunction
            // would hand back the existing function, so the new overload's calls coerce
            // args to the user's types but dispatch to the old callee - an LLVM "bad
            // signature" assert at codegen. Reject with a clear diagnostic instead.
            if (external && existing->getFunctionType() != functionType)
            {
                LogError(std::format(
                    "conflicting declaration of extern '{}': a function with this linkage "
                    "name already exists with a different signature (e.g. in a core library "
                    "such as os.windows). Rename your extern, or call the existing one "
                    "(for file I/O use os.windows.fopen/fread/fwrite/fclose).",
                    functionName));
                return;
            }
        }

        auto funcCallee = module->getOrInsertFunction(mangledName, functionType);
        llvm::Value* calleeValue = funcCallee.getCallee();

        if (llvm::Function* fn = llvm::dyn_cast<llvm::Function>(calleeValue))
        {
            if (callConv == CallingConv::Stdcall && platformValue == 32)
                fn->setCallingConv(llvm::CallingConv::X86_StdCall);
            else if (callConv == CallingConv::Cdecl)
                fn->setCallingConv(llvm::CallingConv::C);

            if (useRecipe)
                ApplyAbiAttributes(fn, recipe);

            auto& symList = functionTable[functionName];
            FunctionSymbol funcSym = {
                .UniqueName = mangledName,
                .Function = fn,
                .ReturnType = returnType,
                .Variadic = fn->isVarArg(),
                .ReturnsOwned = returnsOwned,
                .ReturnsAlias = returnType.IsAlias, // 'alias' return: caller must not free the interior
                .IsMethod = isMethod,
                .Recipe = recipe,
            };

            for (const auto& arg : arguments)
            {
                funcSym.Parameters.push_back(arg);
            }

            symList.push_back(funcSym);
        }
    }

const LLVMBackend::FunctionSymbol* LLVMBackend::GetFunctionSymbol(llvm::Function* fn) const
{
        for (const auto& [key, syms] : functionTable)
            for (const auto& sym : syms)
                if (sym.Function == fn)
                    return &sym;
        return nullptr;
    }

void LLVMBackend::SetFunctionRequiredLocks(const std::string& functionName, std::vector<std::string> locks)
{
        auto it = functionTable.find(functionName);
        if (it != functionTable.end() && !it->second.empty())
            it->second.back().RequiredLocks = std::move(locks);
    }

llvm::Type* LLVMBackend::BuildThinFnPtrType(const TypeAndValue& tv) const
{
        std::vector<llvm::Type*> paramTypes;
        for (const auto& p : tv.FuncPtrParams)
        {
            TypeAndValue pTV; pTV.TypeName = p.TypeName; pTV.Pointer = p.Pointer;
            paramTypes.push_back(GetType(pTV));
        }
        TypeAndValue retTV;
        retTV.TypeName = tv.FuncPtrReturnTypeName;
        retTV.Pointer  = tv.FuncPtrReturnPointer;
        return llvm::FunctionType::get(GetType(retTV), paramTypes, false)->getPointerTo();
    }

llvm::Type* LLVMBackend::GetCCompatibleType(const TypeAndValue& tv) const
{
        if (tv.IsFunctionPointer)
            return BuildThinFnPtrType(tv);
        return GetType(tv);
    }

llvm::FunctionType* LLVMBackend::GetFunctionType(const LLVMBackend::TypeAndValue& returnType, const std::vector<LLVMBackend::TypeAndValue>& arguments, bool varargs, bool externC)
{
        std::vector<llvm::Type*> types;
        types.reserve(arguments.size());

        for (const LLVMBackend::TypeAndValue& arg : arguments)
        {
            types.emplace_back(externC ? GetCCompatibleType(arg) : GetType(arg));
        }

        auto* retTy = externC ? GetCCompatibleType(returnType) : GetType(returnType);
        return llvm::FunctionType::get(retTy, types, varargs);
    }

std::string LLVMBackend::ComputeMangledName(const std::string& functionName, const LLVMBackend::TypeAndValue& returnType, const std::vector<LLVMBackend::TypeAndValue>& arguments, bool varargs)
{
        std::string argumentString = {};

        for (const auto& argument : arguments)
        {
            argumentString += argument.ToUniqueString();
            if (argument.IsMove)
                argumentString += "M";
        }

        std::string uniqueName = std::format("_{}_{}_{}_", functionName, returnType.ToUniqueString(), argumentString);

        return uniqueName;
    }

std::string LLVMBackend::SourceFileLeaf(const std::string& path)
{
        size_t slash = path.find_last_of("/\\");
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

void LLVMBackend::DiagnoseDuplicateFunctionBody(const std::string& functionName,
        const std::string& mangledName, size_t line) const
{
        auto it = functionBodyOrigin_.find(mangledName);
        if (it == functionBodyOrigin_.end()) return;
        const std::string& firstFile = it->second.first;
        size_t firstLine = it->second.second;
        if (line == 0 || firstLine == 0 || firstLine == line) return;
        LogError(std::format("redefinition of '{}' - the same overload is already defined at "
            "{}({}). Two parameter lists that differ only in a SPELLING of one type ('int' and "
            "'i32' name the same type) are one overload, not two.",
            functionName, firstFile, firstLine));
    }

bool LLVMBackend::OverloadSlotIsDefined(const std::string& functionName, const LLVMBackend::TypeAndValue& returnType,
        const std::vector<LLVMBackend::TypeAndValue>& arguments, bool varargs,
        std::string* originFile, size_t* originLine)
{
        std::string mangledName = ComputeMangledName(functionName, returnType, arguments, varargs);
        auto* fn = module->getFunction(mangledName);
        if (fn == nullptr || fn->empty()) return false;
        auto it = functionBodyOrigin_.find(mangledName);
        if (originFile) *originFile = it != functionBodyOrigin_.end() ? it->second.first : std::string();
        if (originLine) *originLine = it != functionBodyOrigin_.end() ? it->second.second : 0;
        return true;
    }

llvm::Function* LLVMBackend::CreateFunctionDefinition(const std::string& functionName, LLVMBackend::TypeAndValue returnType, std::vector<LLVMBackend::TypeAndValue> arguments, bool external, bool varargs, size_t line, bool returnsOwned, bool isMethod, CallingConv callConv, size_t scopeLine)
{
        llvm::FunctionType* functionType = GetFunctionType(returnType, arguments, varargs, external);

        std::string mangledName = external ? functionName : ComputeMangledName(functionName, returnType, arguments, varargs);

        if (functionType == nullptr)
        {
            functionType = llvm::FunctionType::get(builder->getVoidTy(), false);
        }

        auto fn = module->getFunction(mangledName);
        bool alreadyDeclared = false;

        if (fn != nullptr)
        {
            if (!fn->empty())
            {
                DiagnoseDuplicateFunctionBody(functionName, mangledName, line);
                if (verbose) std::cout << std::format("[verbose] skipping duplicate definition of '{}'\n", functionName);
                return fn;
            }
            // Pre-declared by ForwardRefScanner - reuse the declaration and attach a body.
            alreadyDeclared = true;
        }
        else
        {
            fn = createFunctionProto(mangledName, functionType);
        }

        // The slot this definition claims. Overwritten while the proto is still body-less, so the
        // recorded origin is always the definition that actually attaches a body first.
        functionBodyOrigin_[mangledName] = { SourceFileLeaf(currentSourceFilePath_), line };

        // CFlat treats null pointer dereferences as defined behavior (hardware fault -> SEH).
        // Ensure the attribute is set even on pre-declared functions that skipped createFunctionProto.
        fn->addFnAttr(llvm::Attribute::NullPointerIsValid);

        // Apple's arm64 ABI requires x29 (the frame pointer) to form a valid linked list of
        // frame records for every non-leaf function; "non-leaf" matches Apple clang's own
        // default (leaf functions may still omit it). This also lets Darwin's backtrace()
        // (frame-pointer walk, unlike Windows' unwind-table-based CaptureStackBackTrace) and
        // native tools (Instruments/sample/debuggers) unwind cflat-generated stacks.
        if (targetMacOS_)
            fn->addFnAttr("frame-pointer", "non-leaf");

        // The whole program (user files + core libraries) is compiled into a single module
        // and optimized with the per-module pipeline before lld-link merges any separately
        // compiled C objects. A non-extern CFlat function therefore never needs to be visible
        // by name to anything outside this module, so give it internal linkage. That unlocks
        // interprocedural optimization the optimizer must otherwise withhold from symbols it
        // assumes are externally referenced (dead-function elimination, inline-then-delete,
        // argument promotion / specialization). Exceptions stay external:
        //   - `external` (extern C) functions are the ABI surface for the linked C objects.
        //   - `main` is the linker entry symbol.
        //   - A function whose raw address is handed to a bare C callback is promoted back to
        //     external at the call site (see CreateOverloadedFunctionCall) so the linker keeps
        //     its identity. We only internalize here, where a body is being attached - never on
        //     declaration-only protos (an internal declaration with no body is invalid IR).
        if (!external && functionName != "main")
            fn->setLinkage(llvm::Function::InternalLinkage);

        if (callConv == CallingConv::Stdcall && platformValue == 32)
            fn->setCallingConv(llvm::CallingConv::X86_StdCall);
        else if (callConv == CallingConv::Cdecl)
            fn->setCallingConv(llvm::CallingConv::C);

        // Thin `int[]` array-view params carry a noalias contract (distinct views point at
        // distinct whole allocations). Stamp LLVM noalias so the loop vectorizer can drop its
        // runtime alias check. Native params map 1:1 to fn args (no sret/byval reordering);
        // skipped for extern C, where pointers alias freely.
        if (!external)
        {
            unsigned ai = 0;
            for (const auto& a : arguments)
            {
                if (a.IsArrayView && ai < fn->arg_size())
                    fn->addParamAttr(ai, llvm::Attribute::NoAlias);
                ++ai;
            }
        }

        createFunctionBlock(fn, functionName, arguments, returnsOwned, returnType.IsArrayView, returnType.TypeName);
        // Sibling of the currentFunctionReturn* fields set inside createFunctionBlock: retain the
        // full return TypeAndValue so a returned lambda literal can adopt a function<> return type.
        currentFunctionReturnTV = returnType;
        // Per-function by construction: see globalAssignBorrowOrigin_'s comment.
        globalAssignBorrowOrigin_.clear();

        if (diBuilder && diFile && line > 0)
        {
            // Reuse an existing subprogram if the function was previously processed
            // (e.g. constraint-instantiation re-entered this path). Creating a new
            // distinct DISubprogram and overwriting setSubprogram() leaves the prior
            // node orphaned, which the LLVM verifier rejects.
            auto sp = fn->getSubprogram();
            llvm::DIFile* fnFile = GetDIFileForCurrentSource();
            if (!fnFile) fnFile = diFile;
            if (!sp)
            {
                // DWARF / CodeView subroutine type: element 0 is the return type
                // (nullptr means void), followed by parameter types in order.
                std::vector<llvm::Metadata*> diTypes;
                diTypes.reserve(1 + arguments.size());
                if (returnType.TypeName == "void" && !returnType.Pointer)
                    diTypes.push_back(nullptr);
                else
                    diTypes.push_back(GetDIType(returnType));
                for (const auto& a : arguments)
                    diTypes.push_back(GetDIType(a));

                auto funcDIType = diBuilder->createSubroutineType(
                    diBuilder->getOrCreateTypeArray(diTypes));
                unsigned effectiveScopeLine = (unsigned)(scopeLine != 0 ? scopeLine : line);
                sp = diBuilder->createFunction(
                    fnFile, functionName, fn->getName(),
                    fnFile, (unsigned)line, funcDIType, effectiveScopeLine,
                    llvm::DINode::FlagPrototyped,
                    llvm::DISubprogram::SPFlagDefinition
                );
                fn->setSubprogram(sp);
            }
            currentSubprogram = sp;
            builder->SetCurrentDebugLocation(llvm::DILocation::get(*context, (unsigned)line, 0, sp));

            // Attach parameter DI to the allocas createFunctionBlock just emitted.
            // argNo is 1-based and follows declaration order.
            auto& frame = stackNamedVariable.back();
            unsigned argNo = 1;
            for (const auto& a : arguments)
            {
                auto it = frame.functionArgument.find(a.VariableName);
                if (it != frame.functionArgument.end() && it->second.Storage != nullptr)
                {
                    auto diParam = diBuilder->createParameterVariable(
                        sp, a.VariableName, argNo, fnFile,
                        (unsigned)line, GetDIType(a), /*alwaysPreserve*/ true);
                    diBuilder->insertDeclare(
                        it->second.Storage, diParam, diBuilder->createExpression(),
                        llvm::DILocation::get(*context, (unsigned)line, 0, sp),
                        builder->GetInsertBlock());
                }
                ++argNo;
            }
        }

        if (!alreadyDeclared)
        {
            auto& symList = functionTable[functionName];
            FunctionSymbol funcSym = {
                .UniqueName = mangledName,
                .Function = fn,
                .ReturnType = returnType,
                .Variadic = fn->isVarArg(),
                .ReturnsOwned = returnsOwned,
                .ReturnsAlias = returnType.IsAlias, // 'alias' return: caller must not free the interior
                .IsMethod = isMethod,
            };

            for (const auto& arg : arguments)
            {
                funcSym.Parameters.push_back(arg);
            }

            symList.push_back(funcSym);
        }
        else if (isMethod)
        {
            // ForwardRefScanner registered this symbol; propagate IsMethod in case it wasn't set there.
            auto it = functionTable.find(functionName);
            if (it != functionTable.end())
            {
                for (auto& sym : it->second)
                {
                    if (sym.Function == fn) { sym.IsMethod = true; break; }
                }
            }
        }
        return fn;
    }
