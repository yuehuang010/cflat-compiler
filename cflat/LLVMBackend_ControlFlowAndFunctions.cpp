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
        {
            const std::string rawFunction = currentFunction->getName().str();
            std::cout << std::format("  Function: {} ({})\n",
                                     SpellFunctionSymbol(*this, rawFunction), rawFunction);
        }
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
        for (const auto& [name, _] : dataStructures)
        {
            TypeAndValue type;
            type.TypeName = name;
            std::cout << std::format("    {} ({})\n", SpellType(*this, type), name);
        }
        std::cout << std::format("  Functions registered: {}\n", functionTable.size());
        for (const auto& [name, overloads] : functionTable)
            for (const auto& overload : overloads)
            {
                const std::string rawFunction = overload.UniqueName.empty()
                    ? name : overload.UniqueName;
                std::cout << std::format("    {} ({})\n",
                                         SpellFunctionSymbol(*this, rawFunction), rawFunction);
            }
    }

void LLVMBackend::InitDebugInfo(const std::string& filename, const std::string& directory)
{
        diBuilder = std::make_unique<llvm::DIBuilder>(*module);
        module->addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
        // Target is *-pc-windows-msvc: emit CodeView so lld-link can produce a PDB.
        // DWARF would be ignored by VS / WinDbg / cppvsdbg on Windows.
        if (targetWindows_)
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
                if (!cflat_llvm::GetTerminatorOrNull(&bb))
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
                        currentFunctionReturnTV, currentFunctionAbiRecipe };
        s.aliasDomain = aliasDomain_;
        s.aliasScopes = std::move(aliasScopes_);
        s.viewScopeByOrigin = std::move(viewScopeByOrigin_);
        aliasDomain_ = nullptr;
        aliasScopes_.clear();
        viewScopeByOrigin_.clear();
        // Park the outer function's pending owned temps in the saved state and start the
        // nested emission with empty lists (see the BuilderState field comment).
        s.pendingStringTemps  = std::move(pendingOwnedStringTemps);
        s.pendingClosureTemps = std::move(pendingOwnedClosureTemps);
        s.pendingStructTemps  = std::move(pendingOwnedStructTemps);
        s.pendingPtrTemps     = std::move(pendingOwnedPtrTemps);
        s.ownedReturnTemps    = std::move(ownedReturnTemps_);
        s.ownedReturnReleaseTemps = std::move(ownedReturnReleaseTemps_);
        s.ownedNewTemps       = std::move(ownedNewTemps_);
        s.nullConditionalTempResults = std::move(nullConditionalTempResults_);
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
        s.borrowedAddressValues = std::move(borrowedAddressValues_);
        s.movedBorrowedThroughFieldValues = std::move(movedBorrowedThroughFieldValues_);
        s.nonOwningStructJoins = std::move(nonOwningStructJoins_);
        s.uniqueFieldReadValues = std::move(uniqueFieldReadValues_);
        s.uniqueFieldReadJoins = std::move(uniqueFieldReadJoins_);
        s.aliasValues = std::move(aliasValues_);
        s.tempFieldValues = std::move(tempFieldValues_);
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
        nullConditionalTempResults_.clear();
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
        borrowedAddressValues_.clear();
        movedBorrowedThroughFieldValues_.clear();
        nonOwningStructJoins_.clear();
        uniqueFieldReadValues_.clear();
        uniqueFieldReadJoins_.clear();
        aliasValues_.clear();
        tempFieldValues_.clear();
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
        currentFunctionAbiRecipe = state.abiRecipe;
        aliasDomain_             = state.aliasDomain;
        aliasScopes_             = state.aliasScopes;
        viewScopeByOrigin_       = state.viewScopeByOrigin;
        pendingOwnedStringTemps  = state.pendingStringTemps;
        pendingOwnedClosureTemps = state.pendingClosureTemps;
        pendingOwnedStructTemps  = state.pendingStructTemps;
        pendingOwnedPtrTemps     = state.pendingPtrTemps;
        ownedReturnTemps_        = state.ownedReturnTemps;
        ownedReturnReleaseTemps_ = state.ownedReturnReleaseTemps;
        ownedNewTemps_           = state.ownedNewTemps;
        nullConditionalTempResults_ = state.nullConditionalTempResults;
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
        borrowedAddressValues_   = state.borrowedAddressValues;
        movedBorrowedThroughFieldValues_ = state.movedBorrowedThroughFieldValues;
        nonOwningStructJoins_    = state.nonOwningStructJoins;
        uniqueFieldReadValues_   = state.uniqueFieldReadValues;
        uniqueFieldReadJoins_    = state.uniqueFieldReadJoins;
        aliasValues_             = state.aliasValues;
        tempFieldValues_         = state.tempFieldValues;
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

llvm::UncondBrInst* LLVMBackend::CreateJump(llvm::BasicBlock* block)
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
        return GetFunctionReturnABIType(it->second.front().ReturnType);
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
        tv.FuncPtrReturnOwned = chosen->ReturnType.IsMove;
        tv.FuncPtrReturnAlias = chosen->ReturnType.IsAlias;
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
            LogErrorMessage(
                "cannot pass '{}' to the '{}' parameter {} through a function value: a "
                "'{}' is a {} value, not a '{}'. Pass the buffer explicitly with '{}'.",
                { "string", "char*", std::to_string(index + 1), "string", "{ptr,len}",
                  "char*", ".data()" });
            return;
        }
        if (!arg->getType()->isPointerTy() || destTy->isPointerTy()) return;

        // An unnamed slot gets the shorter message: naming a type it does not have, or advising a
        // '*' spelling of it, would be advice the caller cannot follow.
        std::string shown = SpellType(*this, TypeAndValue{ .TypeName = paramTypeName });
        const bool writable = !shown.empty();
        if (shown.empty())
        {
            LogErrorMessage(
                "call through a function value: cannot pass a pointer as argument {} - that "
                "parameter is a by-value slot and there is no implicit dereference. Write '*' at "
                "the call site to pass the pointee.", { std::to_string(index + 1) });
            return;
        }
        // Same rule as the virtual site: a nested instantiation renders ambiguously, so it keeps
        // the raw name and loses the advice clause.
        std::string advice = writable
            ? std::format(", or declare the parameter as '{}*'", shown) : std::string();
        LogRawError(std::format(
            "call through a function value: cannot pass a pointer as argument {} - parameter {} is "
            "the by-value type '{}' and there is no implicit dereference. Write '*' at the call "
            "site to pass the pointee{}.",
            index + 1, index + 1, shown, advice));
    }

// Argument signedness for an indirect call: a u32 literal/expression must zero-extend
// into a wider parameter, exactly as the direct-call path does.
static bool IndirectArgIsUnsigned(const std::vector<LLVMBackend::NamedVariable>* argNVs, size_t i)
{
        return argNVs != nullptr && i < argNVs->size()
            && (*argNVs)[i].TypeAndValue.IsUnsignedInteger() != -1;
    }

llvm::Value* LLVMBackend::CreateIndirectCall(const TypeAndValue& funcPtrType, llvm::Value* funcPtr,
                                             std::vector<llvm::Value*> args,
                                             const std::vector<NamedVariable>* argNVs,
                                             const std::vector<llvm::Value*>* rawArrayCounts)
{
        auto* i8PtrTy = cflat_llvm::PointerTo(builder->getInt8Ty());

        // Thin `function<T>`: a bare C function pointer. Direct call, no env, exact C signature.
        if (funcPtrType.IsThinFnPtr())
        {
            std::vector<llvm::Type*> paramTypes;
            for (const auto& p : funcPtrType.FuncPtrParams)
            {
                TypeAndValue pTV; pTV.TypeName = p.TypeName; pTV.Pointer = p.Pointer; pTV.IsMove = p.IsMove;
                paramTypes.push_back(GetType(pTV));
                if (ParameterCarriesRawArrayCount(pTV))
                    paramTypes.push_back(builder->getInt64Ty());
            }
            TypeAndValue retTV;
            retTV.TypeName = funcPtrType.FuncPtrReturnTypeName;
            retTV.Pointer  = funcPtrType.FuncPtrReturnPointer;
            retTV.IsMove   = funcPtrType.FuncPtrReturnOwned;
            retTV.IsAlias  = funcPtrType.FuncPtrReturnAlias;
            if (ReturnCarriesRawArrayCount(retTV))
                paramTypes.push_back(cflat_llvm::PointerTo(builder->getInt64Ty()));
            auto* retTy   = GetFunctionReturnABIType(retTV);
            auto* cFnTy   = llvm::FunctionType::get(retTy, paramTypes, false);
            auto* fnPtr   = builder->CreateBitCast(funcPtr, cflat_llvm::PointerTo(cFnTy), "cfn_ptr");
            std::vector<llvm::Value*> abiArgs;
            size_t typeIndex = 0;
            for (size_t i = 0; i < args.size() && i < funcPtrType.FuncPtrParams.size(); i++)
            {
                auto* destTy = paramTypes[typeIndex++];
                auto* strTy  = llvm::StructType::getTypeByName(*context, "string");
                if (strTy && destTy == strTy && args[i]->getType()->isPointerTy())
                    args[i] = WrapStringLiteralAsString(args[i]);
                else
                    args[i] = ConvertIntegerCallArgument(args[i], destTy,
                        IndirectArgIsUnsigned(argNVs, i),
                        SpellType(*this, TypeAndValue{ .TypeName = funcPtrType.FuncPtrParams[i].TypeName }),
                        std::format("parameter {} of a call through a function value", i + 1));
                CheckIndirectCallArgShape(args[i], destTy, i, funcPtrType.FuncPtrParams[i].TypeName);
                abiArgs.push_back(args[i]);
                TypeAndValue pTV;
                pTV.TypeName = funcPtrType.FuncPtrParams[i].TypeName;
                pTV.Pointer = funcPtrType.FuncPtrParams[i].Pointer;
                pTV.IsMove = funcPtrType.FuncPtrParams[i].IsMove;
                if (ParameterCarriesRawArrayCount(pTV))
                {
                    llvm::Value* count = rawArrayCounts != nullptr && i < rawArrayCounts->size()
                        ? (*rawArrayCounts)[i] : nullptr;
                    if (count == nullptr)
                        count = argNVs != nullptr && i < argNVs->size()
                            ? RawArrayCountArgument((*argNVs)[i]) : builder->getInt64(-1);
                    abiArgs.push_back(count);
                    typeIndex++;
                }
            }
            llvm::Value* rawReturnCountSlot = nullptr;
            if (ReturnCarriesRawArrayCount(retTV))
            {
                rawReturnCountSlot = CreateRawArrayReturnCountSlot();
                abiArgs.push_back(rawReturnCountSlot);
            }
            lastCallReturnType = retTV;
            auto* result = builder->CreateCall(cFnTy, fnPtr, abiArgs);
            llvm::Value* value = retTy->isVoidTy() ? nullptr : result;
            RegisterRawArrayCallResult(value, rawReturnCountSlot);
            return value;
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
            TypeAndValue pTV; pTV.TypeName = p.TypeName; pTV.Pointer = p.Pointer; pTV.IsMove = p.IsMove;
            paramTypes.push_back(GetType(pTV));
            if (ParameterCarriesRawArrayCount(pTV))
                paramTypes.push_back(builder->getInt64Ty());
        }
        paramTypes.push_back(i8PtrTy); // env (trailing)
        TypeAndValue retTV;
        retTV.TypeName = funcPtrType.FuncPtrReturnTypeName;
        retTV.Pointer  = funcPtrType.FuncPtrReturnPointer;
        retTV.IsMove   = funcPtrType.FuncPtrReturnOwned;
        retTV.IsAlias  = funcPtrType.FuncPtrReturnAlias;
        if (ReturnCarriesRawArrayCount(retTV))
            paramTypes.push_back(cflat_llvm::PointerTo(builder->getInt64Ty()));
        auto* retTy     = GetFunctionReturnABIType(retTV);
        auto* invokerTy = llvm::FunctionType::get(retTy, paramTypes, false);
        auto* fnPtr     = builder->CreateBitCast(fnPtrI8, cflat_llvm::PointerTo(invokerTy), "fn_ptr");

        // Upconvert user args to match declared param types (the trailing slot is env, skip it).
        // String literals arrive as i8* - wrap them into %string{ptr,len} when the
        // param expects a string value type.
        std::vector<llvm::Value*> userArgs;
        size_t typeIndex = 0;
        for (size_t i = 0; i < args.size() && i < funcPtrType.FuncPtrParams.size(); i++)
        {
            auto* destTy = paramTypes[typeIndex++];
            auto* strTy  = llvm::StructType::getTypeByName(*context, "string");
            if (strTy && destTy == strTy && args[i]->getType()->isPointerTy())
                args[i] = WrapStringLiteralAsString(args[i]);
            else
                args[i] = ConvertIntegerCallArgument(args[i], destTy,
                        IndirectArgIsUnsigned(argNVs, i),
                        SpellType(*this, TypeAndValue{ .TypeName = funcPtrType.FuncPtrParams[i].TypeName }),
                        std::format("parameter {} of a call through a function value", i + 1));
            CheckIndirectCallArgShape(args[i], destTy, i, funcPtrType.FuncPtrParams[i].TypeName);
            userArgs.push_back(args[i]);
            TypeAndValue pTV;
            pTV.TypeName = funcPtrType.FuncPtrParams[i].TypeName;
            pTV.Pointer = funcPtrType.FuncPtrParams[i].Pointer;
            pTV.IsMove = funcPtrType.FuncPtrParams[i].IsMove;
            if (ParameterCarriesRawArrayCount(pTV))
            {
                llvm::Value* count = rawArrayCounts != nullptr && i < rawArrayCounts->size()
                    ? (*rawArrayCounts)[i] : nullptr;
                if (count == nullptr)
                    count = argNVs != nullptr && i < argNVs->size()
                        ? RawArrayCountArgument((*argNVs)[i]) : builder->getInt64(-1);
                userArgs.push_back(count);
                typeIndex++;
            }
        }

        // Append env to call args (env-last)
        std::vector<llvm::Value*> fullArgs(userArgs.begin(), userArgs.end());
        fullArgs.push_back(envPtr);

        llvm::Value* rawReturnCountSlot = nullptr;
        if (ReturnCarriesRawArrayCount(retTV))
        {
            rawReturnCountSlot = CreateRawArrayReturnCountSlot();
            fullArgs.push_back(rawReturnCountSlot);
        }

        lastCallReturnType = retTV;
        auto* result = builder->CreateCall(invokerTy, fnPtr, fullArgs);
        llvm::Value* value = retTy->isVoidTy() ? nullptr : result;
        RegisterRawArrayCallResult(value, rawReturnCountSlot);
        return value;
    }

llvm::SwitchInst* LLVMBackend::CreateSwitchInst(llvm::Value* cond, llvm::BasicBlock* defaultBlock, unsigned numCases)
{
        if (!cond->getType()->isIntegerTy())
            LogErrorMessage("switch expression must be an integer type");
        return builder->CreateSwitch(cond, defaultBlock, numCases);
    }

std::string LLVMBackend::SpellIntegerType(llvm::Type* t, bool isUnsigned)
{
        unsigned bits = t->isIntegerTy() ? llvm::cast<llvm::IntegerType>(t)->getBitWidth() : 0;
        if (bits == 1) return "bool";
        return std::format("{}{}", isUnsigned ? "u" : "i", bits);
    }

llvm::APInt LLVMBackend::WidenCaseValue(llvm::ConstantInt* val, bool labelIsUnsigned)
{
        // Common 65-bit signed domain: wide enough to hold every i64 and every u64 exactly.
        const llvm::APInt& raw = val->getValue();
        return labelIsUnsigned ? raw.zext(65) : raw.sext(65);
    }

bool LLVMBackend::CaseValueFits(const llvm::APInt& wide, bool labelIsUnsigned, llvm::Type* switchType, bool switchIsUnsigned)
{
        unsigned destBits = llvm::cast<llvm::IntegerType>(switchType)->getBitWidth();
        // An unsigned label is a bit pattern: it fits when its bits fit the operand width, on a
        // signed operand too (C: 0xFFFFFFFF converts to int -1). A negative label needs a signed operand.
        if (labelIsUnsigned || switchIsUnsigned)
            return !wide.isNegative() && wide.getActiveBits() <= destBits;
        return wide.sge(llvm::APInt::getSignedMinValue(destBits).sext(65))
            && wide.sle(llvm::APInt::getSignedMaxValue(destBits).sext(65));
    }

llvm::ConstantInt* LLVMBackend::CoerceCaseValue(llvm::ConstantInt* val, llvm::Type* switchType, bool labelIsUnsigned)
{
        // Convert through the widened value so a narrow signed label (-129 folds to i16) is
        // sign-extended, not zero-extended, into the switch operand's width.
        auto* destTy = llvm::cast<llvm::IntegerType>(switchType);
        llvm::APInt wide = WidenCaseValue(val, labelIsUnsigned);
        return llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(destTy, wide.trunc(destTy->getBitWidth())));
    }

llvm::Function* LLVMBackend::GetOrDeclareStrcmp()
{
        if (auto* fn = module->getFunction("strcmp"))
            return fn;
        auto* i32Ty = builder->getInt32Ty();
        auto* ptrTy = cflat_llvm::PointerTo(builder->getInt8Ty());
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

        // A blessed core unique<X> pointer arm is a one-slot wrapper over the raw pointer, so it
        // tests exactly as the `unique X*` spelling it desugars from.
        if (auto* st = llvm::dyn_cast<llvm::StructType>(cond->getType());
            st != nullptr && st->hasName() && st->getNumElements() == 1
            && st->getElementType(0)->isPointerTy() && IsCoreUniqueType(st->getName().str()))
            return builder->CreateIsNotNull(builder->CreateExtractValue(cond, 0));

        // An aggregate (a `string`, any struct) has no truth value. Diagnose it here rather than
        // handing it to CreateCondBr / CreateSelect, which fails module verification opaquely.
        LogErrorMessage(
            "condition must be a scalar (bool, integer, pointer or floating point), not '{}'"
            " - compare it explicitly",
            { DescribeConditionType(cond->getType()) });
        return builder->getFalse();
    }

llvm::Value* LLVMBackend::ConvertIntegerCallArgument(llvm::Value* value, llvm::Type* destType,
        bool srcIsUnsigned, const std::string& paramSpelling, const std::string& what)
{
        if (value == nullptr || destType == nullptr)
            return value;

        auto* srcType = value->getType();
        if (!srcType->isIntegerTy() || !destType->isIntegerTy())
            return Upconvert(value, destType, srcIsUnsigned);

        // Integer -> bool is legal at a call for constants and non-constants alike, and takes
        // the same door a cast / assignment / brace-init takes.
        if (destType->isIntegerTy(1) && !srcType->isIntegerTy(1))
            return CoerceToBoolCondition(value);

        // Narrowing has no implicit conversion at a call argument. Without this the mistyped
        // value reached the LLVM verifier as "Call parameter type does not match function
        // signature!" with no diagnostic of our own.
        if (srcType->getIntegerBitWidth() > destType->getIntegerBitWidth())
        {
            const std::string shown = paramSpelling.empty() ? std::string("a narrower integer")
                                                            : paramSpelling;
            LogErrorMessage(
                "cannot pass a {}-bit integer to {}: implicit integer narrowing to '{}' is not "
                "allowed at a call argument - cast it explicitly with '({})'",
                { std::to_string(srcType->getIntegerBitWidth()), what, shown, shown });
        }

        return Upconvert(value, destType, srcIsUnsigned);
    }

std::string LLVMBackend::DescribeConditionType(llvm::Type* t) const
{
        if (auto* st = llvm::dyn_cast<llvm::StructType>(t))
        {
            // The fat-pointer lowerings are compiler-internal names the user never wrote -
            // name the KIND instead of leaking '__iface_fat_ptr' / '__closure_fat_ptr'.
            if (!st->hasName()) return "struct";
            if (st->getName() == "__iface_fat_ptr")   return "interface value";
            if (st->getName() == "__closure_fat_ptr") return "closure value";
            return st->getName().str();
        }
        if (t->isArrayTy())  return "array";
        if (t->isVectorTy()) return "vector";
        return "value";
    }

llvm::CondBrInst* LLVMBackend::CreateConditionJump(llvm::Value* cond, llvm::BasicBlock* trueBlock, llvm::BasicBlock* falseBlock)
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

llvm::UncondBrInst* LLVMBackend::CreateBlockBreak(llvm::BasicBlock* resumeBlock, bool exitBlockStack)
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
            ptypes.push_back(cflat_llvm::PointerTo(recipe.retSlot.structTy));
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
                ptypes.push_back(cflat_llvm::PointerTo(s.structTy));
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

// A file-scope `main` with an entry-point signature IS the program entry point, so it keeps its
// unmangled linkage symbol even without `extern` - otherwise the linker reports undefined _main.
static bool IsImplicitEntryMain(const std::string& functionName,
                                const LLVMBackend::TypeAndValue& returnType,
                                const std::vector<LLVMBackend::TypeAndValue>& arguments,
                                bool isMethod, bool varargs)
{
        if (functionName != "main" || isMethod || varargs) return false;
        if (returnType.Pointer || returnType.IsInterface || returnType.IsMove || returnType.IsAlias) return false;
        if (returnType.TypeName != "int" && returnType.TypeName != "i32") return false;
        if (arguments.empty()) return true;
        if (arguments.size() != 2) return false;
        const auto& a0 = arguments[0];
        const auto& a1 = arguments[1];
        if (a0.Pointer || (a0.TypeName != "int" && a0.TypeName != "i32")) return false;
        return a1.Pointer && a1.ElemPointer;
    }

/*
 * Rebuild the real FunctionType for every signature that was declared while one of its
 * by-value aggregates was still an opaque shell, now that the main pass has set that body.
 * Nothing here computes a layout: the signature is recomputed from the SAME completed
 * StructType the main pass built, and the provisional body-less declaration is replaced.
 */
void LLVMBackend::FlushPendingFunctionDeclarations()
{
        if (flushingPendingDeclarations_ || pendingFunctionDeclarations_.empty()) return;
        flushingPendingDeclarations_ = true;
        auto parked = std::move(pendingFunctionDeclarations_);
        pendingFunctionDeclarations_.clear();
        for (auto& d : parked)
        {
            // Still incomplete: park it again, untouched.
            if (FindIncompleteByValueAggregate(d.Arguments, d.External) != nullptr)
            {
                pendingFunctionDeclarations_.push_back(std::move(d));
                continue;
            }

            AbiRecipe recipe;
            bool useRecipe = false;
            if (d.External)
            {
                recipe = ComputeAbiRecipe(d.ReturnType, d.Arguments);
                useRecipe = recipe.hasLowering;
            }
            llvm::FunctionType* wanted = useRecipe
                ? BuildExternFunctionType(d.ReturnType, d.Arguments, d.Varargs, recipe)
                : GetFunctionType(d.ReturnType, d.Arguments, d.Varargs, d.External);

            llvm::Function* provisional = module->getFunction(d.MangledName);
            if (provisional == nullptr || wanted == nullptr
                || provisional->getFunctionType() == wanted)
                continue;
            if (FunctionHasDefinition(provisional) || !provisional->use_empty())
            {
                LogError(std::format(
                    "'{}' was used before the by-value type in its signature was complete, so its "
                    "calling convention cannot be repaired. Define that type before the signature.",
                    SpellFunctionSymbol(*this, d.FunctionName)));
                continue;
            }

            auto conv = provisional->getCallingConv();
            provisional->setName(d.MangledName + ".provisional");
            llvm::Function* repaired = createFunctionProto(d.MangledName, wanted);
            repaired->setCallingConv(conv);
            if (useRecipe)
                ApplyAbiAttributes(repaired, recipe);
            provisional->replaceAllUsesWith(repaired);
            provisional->eraseFromParent();

            auto it = functionTable.find(d.FunctionName);
            if (it != functionTable.end())
                for (auto& sym : it->second)
                    if (sym.UniqueName == d.MangledName)
                    {
                        sym.Function = repaired;
                        if (d.External) sym.Recipe = recipe;
                    }
        }
        flushingPendingDeclarations_ = false;
    }

// Module end: nothing else will complete these aggregates. A provisional declaration that is
// actually used from here on would carry the wrong ABI, so report it instead.
void LLVMBackend::ReportUnresolvedProvisionalDeclarations()
{
        FlushPendingFunctionDeclarations();
        auto leftovers = std::move(pendingFunctionDeclarations_);
        pendingFunctionDeclarations_.clear();
        for (const auto& d : leftovers)
        {
            llvm::Function* fn = module ? module->getFunction(d.MangledName) : nullptr;
            if (fn == nullptr || (!FunctionHasDefinition(fn) && fn->use_empty())) continue;
            auto* incomplete = FindIncompleteByValueAggregate(d.Arguments, d.External);
            LogError(std::format(
                "type '{}' is never completed, so '{}' cannot take it by value. "
                "Define the type's body, or pass it by pointer.",
                incomplete != nullptr
                    ? SpellType(*this, TypeAndValue{ .TypeName = incomplete->getName().str() })
                    : std::string("<unknown>"),
                SpellFunctionSymbol(*this, d.FunctionName)));
        }
    }

void LLVMBackend::CreateFunctionDeclaration(const std::string& functionName, const LLVMBackend::TypeAndValue& returnType, const std::vector<LLVMBackend::TypeAndValue>& arguments, bool external, bool varargs, bool returnsOwned, bool isMethod, CallingConv callConv, const std::string& linkageName)
{
        // ForwardRefScanner registers signatures before any struct BODY exists. An opaque
        // by-value aggregate has no legal FunctionType yet, so the declaration is emitted with a
        // PROVISIONAL signature and re-derived by FlushPendingFunctionDeclarations once the main
        // pass sets the real body. Registration order in functionTable is unchanged.
        bool provisional = FindIncompleteByValueAggregate(arguments, external) != nullptr;

        if (external)
        {
            auto it = functionTable.find(functionName);
            if (it != functionTable.end())
                std::erase_if(it->second, [](const FunctionSymbol& sym) {
                    return sym.IsCInteropAlias;
                });
        }

        // For extern C declarations, compute an ABI recipe so struct-by-value params/returns
        // are lowered (coerce-to-int / byval / sret) per the Win64 or Win32 MSVC ABI. If the
        // recipe has no lowering (scalar/pointer only) the existing GetFunctionType path is used.
        AbiRecipe recipe;
        bool useRecipe = false;
        if (external && !provisional)
        {
            recipe = ComputeAbiRecipe(returnType, arguments);
            useRecipe = recipe.hasLowering;
        }

        llvm::FunctionType* functionType = useRecipe
            ? BuildExternFunctionType(returnType, arguments, varargs, recipe)
            : GetFunctionType(returnType, arguments, varargs, external, provisional);
        // Only a `main` declared in the root translation unit is the program entry point -
        // an imported library's own `main` must mangle normally or it collides with the app's.
        bool entryMain = !external && currentSourceFilePath_ == analyzedRootPath_
            && IsImplicitEntryMain(functionName, returnType, arguments, isMethod, varargs);
        std::string mangledName = external ? (linkageName.empty() ? functionName : linkageName)
                                 : entryMain ? functionName
                                             : ComputeMangledName(functionName, returnType, arguments, varargs);

        if (llvm::Function* existing = module->getFunction(mangledName))
        {
            // A repeat declaration under the same lookup name is a no-op. A *different*
            // lookup name for an already-emitted linkage symbol (core's os.windows.Sleep
            // and a header-imported bare Sleep) still registers below, reusing the
            // existing llvm::Function via getOrInsertFunction.
            // A PROVISIONAL signature is a placeholder either side, so a width comparison
            // against it means nothing - keep the plain no-op for those.
            bool existingProvisional = provisional;
            for (const auto& d : pendingFunctionDeclarations_)
                if (d.MangledName == mangledName) existingProvisional = true;

            for (const auto& sym : functionTable[functionName])
                if (sym.UniqueName == mangledName)
                {
                    /*
                     * The repeat is a no-op only when it AGREES. A DIFFERENT extern signature
                     * under the same linkage name used to be dropped here in silence, so
                     * `extern void exit(u8 c);` next to core's `extern void exit(int status);`
                     * bound the int one and the declared u8 was never scored - the ruling
                     * "no implicit integer narrowing at a call argument" was bypassed by
                     * spelling. Fall through to the conflict diagnostic below instead.
                     */
                    if (!external || existingProvisional
                        || existing->getFunctionType() == functionType)
                        return;
                    break;
                }

            // The linkage symbol already exists with a DIFFERENT signature. This happens
            // when a user `extern` collides with a core-library extern of the same name
            // (e.g. fwrite, declared in os.windows with 32-bit params). getOrInsertFunction
            // would hand back the existing function, so the new overload's calls coerce
            // args to the user's types but dispatch to the old callee - an LLVM "bad
            // signature" assert at codegen. Reject with a clear diagnostic instead.
            if (external && existing->getFunctionType() != functionType)
            {
                LogErrorMessage(
                    "conflicting declaration of extern '{}': a function with this linkage "
                    "name already exists with a different signature (e.g. in a core library "
                    "such as os.windows). Rename your extern, or call the existing one "
                    "(for file I/O use os.windows.fopen/fread/fwrite/fclose).",
                    { SpellFunctionSymbol(*this, functionName) });
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
                .SourceName = functionName,
                .Function = fn,
                .ReturnType = returnType,
                .Variadic = fn->isVarArg(),
                .External = external,
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

            if (provisional)
                pendingFunctionDeclarations_.push_back(PendingFunctionDeclaration{
                    .FunctionName = functionName,
                    .MangledName = mangledName,
                    .ReturnType = returnType,
                    .Arguments = arguments,
                    .External = external,
                    .Varargs = varargs,
                });
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

static llvm::StructType* AsOpaqueStruct(llvm::Type* type)
{
        auto* structTy = llvm::dyn_cast_or_null<llvm::StructType>(type);
        return (structTy != nullptr && structTy->isOpaque()) ? structTy : nullptr;
    }

// LLVM rejects an unsized (opaque) PARAMETER type - isValidArgumentType - but accepts an
// opaque RETURN type, which stays correct on its own because setBody completes the very same
// StructType object. So only parameters ever need a stand-in.
static llvm::Type* SizedParamOrPlaceholder(llvm::Type* type, llvm::IRBuilder<>& b)
{
        return AsOpaqueStruct(type) != nullptr ? b.getInt8Ty() : type;
    }

llvm::Type* LLVMBackend::BuildThinFnPtrType(const TypeAndValue& tv) const
{
        std::vector<llvm::Type*> paramTypes;
        for (const auto& p : tv.FuncPtrParams)
        {
            TypeAndValue pTV; pTV.TypeName = p.TypeName; pTV.Pointer = p.Pointer;
            pTV.IsMove = p.IsMove;
            paramTypes.push_back(SizedParamOrPlaceholder(GetType(pTV), *builder));
            if (ParameterCarriesRawArrayCount(pTV))
                paramTypes.push_back(builder->getInt64Ty());
        }
        TypeAndValue retTV;
        retTV.TypeName = tv.FuncPtrReturnTypeName;
        retTV.Pointer  = tv.FuncPtrReturnPointer;
        retTV.IsMove   = tv.FuncPtrReturnOwned;
        retTV.IsAlias  = tv.FuncPtrReturnAlias;
        if (ReturnCarriesRawArrayCount(retTV))
            paramTypes.push_back(cflat_llvm::PointerTo(builder->getInt64Ty()));
        return cflat_llvm::PointerTo(llvm::FunctionType::get(GetFunctionReturnABIType(retTV), paramTypes, false));
    }

bool LLVMBackend::ParameterCarriesRawArrayCount(const TypeAndValue& param) const
{
        return param.Pointer && param.IsMove;
    }

bool LLVMBackend::ReturnCarriesRawArrayCount(const TypeAndValue& returnType) const
{
        return returnType.Pointer && returnType.IsMove;
    }

llvm::Value* LLVMBackend::RawArrayCountArgument(const NamedVariable& arg)
{
        if (!arg.FieldName.empty() || !arg.OwningStructName.empty() || arg.IsElementAccess)
            return builder->getInt64(-1);
        if (arg.Storage != nullptr && !llvm::isa<llvm::AllocaInst>(arg.Storage)
            && !llvm::isa<llvm::GlobalVariable>(arg.Storage))
            return builder->getInt64(-1);
        llvm::Value* count = LoadRawArrayLength(arg);
        return count != nullptr ? Upconvert(count, builder->getInt64Ty()) : builder->getInt64(-1);
    }

llvm::Value* LLVMBackend::CreateRawArrayReturnCountSlot()
{
        auto* slot = AllocaAtEntry(builder->getInt64Ty(), nullptr, "raw_array_return_count");
        builder->CreateStore(builder->getInt64(-1), slot);
        return slot;
    }

void LLVMBackend::RegisterRawArrayCallResult(llvm::Value* result, llvm::Value* countSlot,
                                             uint64_t allocAlign)
{
        if (result == nullptr || countSlot == nullptr) return;
        RegisterRawArrayResult(result,
            builder->CreateLoad(builder->getInt64Ty(), countSlot, "raw_array_return_count"),
            allocAlign);
    }

llvm::Argument* LLVMBackend::CurrentRawArrayReturnCountArgument() const
{
        if (currentFunction == nullptr || !ReturnCarriesRawArrayCount(currentFunctionReturnTV)
            || currentFunction->arg_empty()) return nullptr;
        return &*std::prev(currentFunction->arg_end());
    }

llvm::Type* LLVMBackend::GetCCompatibleType(const TypeAndValue& tv) const
{
        if (tv.IsFunctionPointer)
            return BuildThinFnPtrType(tv);
        return GetType(tv);
    }

llvm::StructType* LLVMBackend::FindIncompleteByValueAggregate(
        const std::vector<LLVMBackend::TypeAndValue>& arguments, bool externC)
{
        // Mirrors the parameter type selection in GetFunctionType below: anything it wraps in a
        // pointer is legal while opaque, so only the raw by-value shapes are inspected here.
        for (const LLVMBackend::TypeAndValue& arg : arguments)
        {
            if (!externC && ParameterIsAliasByPointer(arg)) continue;
            if (auto* opaque = AsOpaqueStruct(externC ? GetCCompatibleType(arg) : GetType(arg)))
                return opaque;
        }
        return nullptr;
    }

llvm::FunctionType* LLVMBackend::GetFunctionType(const LLVMBackend::TypeAndValue& returnType, const std::vector<LLVMBackend::TypeAndValue>& arguments, bool varargs, bool externC, bool allowIncomplete)
{
        // Last line of defence: an opaque aggregate is not a valid LLVM argument type.
        // Report it instead of letting llvm::FunctionType::get assert.
        llvm::StructType* incomplete = FindIncompleteByValueAggregate(arguments, externC);
        if (incomplete != nullptr && !allowIncomplete)
            LogError(std::format(
                "type '{}' is incomplete here, so it cannot be passed by value. "
                "Define its body before this signature, or pass it by pointer.",
                SpellType(*this, TypeAndValue{ .TypeName = incomplete->getName().str() })));

        std::vector<llvm::Type*> types;
        types.reserve(arguments.size());

        auto sized = [this](llvm::Type* type) { return SizedParamOrPlaceholder(type, *builder); };

        for (const LLVMBackend::TypeAndValue& arg : arguments)
        {
            if (!externC && ParameterIsAliasByPointer(arg))
                types.emplace_back(cflat_llvm::PointerTo(GetType(arg)));
            else
                types.emplace_back(sized(externC ? GetCCompatibleType(arg) : GetType(arg)));
            if (!externC && ParameterCarriesRawArrayCount(arg))
                types.emplace_back(builder->getInt64Ty());
        }

        if (!externC && ReturnCarriesRawArrayCount(returnType))
            types.emplace_back(cflat_llvm::PointerTo(builder->getInt64Ty()));

        auto* retTy = externC ? GetCCompatibleType(returnType) : GetFunctionReturnABIType(returnType);
        return llvm::FunctionType::get(retTy, types, varargs);
    }

llvm::Type* LLVMBackend::GetFunctionReturnABIType(const TypeAndValue& returnType) const
{
        auto* valueType = GetType(returnType);
        if (valueType != nullptr && returnType.IsAlias && !returnType.Pointer
            && !returnType.IsArrayView)
            return cflat_llvm::PointerTo(valueType);
        return valueType;
}

bool LLVMBackend::ParameterIsAliasByPointer(const TypeAndValue& param) const
{
        // `alias` on a POINTER/view/interface/closure param already names a borrow in its own
        // representation; only the by-value shapes need the address to reach the caller's object.
        if (!param.IsAlias || param.Pointer || param.IsArrayView || param.IsInterface
            || param.IsFunctionPointer || param.IsMove)
            return false;
        auto* valueType = GetType(param);
        return valueType != nullptr && !valueType->isVoidTy() && !valueType->isPointerTy();
}

std::string LLVMBackend::ComputeMangledName(const std::string& functionName, const LLVMBackend::TypeAndValue& returnType, const std::vector<LLVMBackend::TypeAndValue>& arguments, bool varargs)
{
        std::string uniqueName = MangleFunctionSymbol(*this, functionName, returnType,
                                                      arguments, varargs);
#ifndef NDEBUG
        FunctionSymbolSpelling spelling;
        assert(DemangleFunctionSymbol(*this, uniqueName, spelling));
        assert(MangleFunctionSymbol(*this, spelling) == uniqueName);
#endif
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
        LogErrorMessage("redefinition of '{}' - the same overload is already defined at "
            "{}({}). Two parameter lists that differ only in a SPELLING of one type ('int' and "
            "'i32' name the same type) are one overload, not two.",
            { SpellFunctionSymbol(*this, functionName), firstFile, std::to_string(firstLine) });
    }

bool LLVMBackend::OverloadSlotIsDefined(const std::string& functionName, const LLVMBackend::TypeAndValue& returnType,
        const std::vector<LLVMBackend::TypeAndValue>& arguments, bool varargs,
        std::string* originFile, size_t* originLine)
{
        std::string mangledName = ComputeMangledName(functionName, returnType, arguments, varargs);
        auto* fn = module->getFunction(mangledName);
        if (fn == nullptr || !FunctionHasDefinition(fn)) return false;
        auto it = functionBodyOrigin_.find(mangledName);
        if (originFile) *originFile = it != functionBodyOrigin_.end() ? it->second.first : std::string();
        if (originLine) *originLine = it != functionBodyOrigin_.end() ? it->second.second : 0;
        return true;
    }

llvm::Function* LLVMBackend::CreateFunctionDefinition(const std::string& functionName, const LLVMBackend::TypeAndValue& returnType, const std::vector<LLVMBackend::TypeAndValue>& arguments, bool external, bool varargs, size_t line, bool returnsOwned, bool isMethod, CallingConv callConv, size_t scopeLine)
{
        // A signature parked during the scan must reach the function table before this body
        // (and its call sites) are emitted.
        FlushPendingFunctionDeclarations();
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

        // Only a `main` declared in the root translation unit is the program entry point -
        // an imported library's own `main` must mangle normally or it collides with the app's.
        bool entryMain = !external && currentSourceFilePath_ == analyzedRootPath_
            && IsImplicitEntryMain(functionName, returnType, arguments, isMethod, varargs);
        std::string mangledName = (external || entryMain) ? functionName
                                : ComputeMangledName(functionName, returnType, arguments, varargs);

        if (functionType == nullptr)
        {
            functionType = llvm::FunctionType::get(builder->getVoidTy(), false);
        }

        auto fn = module->getFunction(mangledName);
        if (external && fn != nullptr && fn->getFunctionType() != functionType)
        {
            // ForwardRefScanner may have emitted a natural declaration before the returned
            // record was completed. Replace that body-less placeholder once the real ABI recipe
            // is available; retaining it would force the definition and every caller onto the
            // wrong C ABI signature.
            if (!FunctionHasDefinition(fn) && fn->use_empty())
            {
                fn->setName(mangledName + ".preabi");
                fn = createFunctionProto(mangledName, functionType);
                auto it = functionTable.find(functionName);
                if (it != functionTable.end())
                    for (auto& sym : it->second)
                        if (sym.UniqueName == mangledName)
                        {
                            sym.Function = fn;
                            sym.Recipe = recipe;
                        }
            }
            else
            {
                LogErrorMessage(
                    "cannot repair the extern '{}' declaration after ABI lowering was selected: "
                    "the old declaration is already used",
                    { SpellFunctionSymbol(*this, functionName) });
                return fn;
            }
        }
        bool alreadyDeclared = false;

        if (fn != nullptr)
        {
            if (FunctionHasDefinition(fn))
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
        if (useRecipe)
            ApplyAbiAttributes(fn, recipe);

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
                ai += ParameterCarriesRawArrayCount(a) ? 2u : 1u;
            }
        }

        createFunctionBlock(fn, functionName, arguments, returnsOwned, returnType.IsArrayView,
                            returnType.TypeName, useRecipe ? &recipe : nullptr);
        // Sibling of the currentFunctionReturn* fields set inside createFunctionBlock: retain the
        // full return TypeAndValue so a returned lambda literal can adopt a function<> return type.
        currentFunctionReturnTV = returnType;
        // Per-function by construction: see globalAssignBorrowOrigin_'s comment.
        globalAssignBorrowOrigin_.clear();
        globalAssignBorrowedAddress_.clear();

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
                .SourceName = functionName,
                .Function = fn,
                .ReturnType = returnType,
                .Variadic = fn->isVarArg(),
                .External = external,
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
        else
        {
            // ForwardRefScanner registered this symbol. Refresh the complete semantic signature
            // from the main pass: generic substitution can add return provenance (for example,
            // `alias T` instantiated with a core unique wrapper), which the forward pass cannot
            // derive before the body is materialized.
            auto it = functionTable.find(functionName);
            if (it != functionTable.end())
            {
                for (auto& sym : it->second)
                    if (sym.Function == fn)
                    {
                        sym.ReturnType = returnType;
                        sym.ReturnsOwned = returnsOwned;
                        sym.ReturnsAlias = returnType.IsAlias;
                        sym.Parameters = arguments;
                        if (isMethod) sym.IsMethod = true;
                        break;
                    }
            }
        }
        return fn;
    }
