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

// ---- Definitions moved out of LLVMBackend.h (CodegenHelpers) ----

void LLVMBackend::createFunctionBlock(llvm::Function* fn, const std::string& friendlyName, std::vector<LLVMBackend::TypeAndValue> arguments, bool returnsOwned, bool returnIsArrayView, const std::string& returnTypeName, const AbiRecipe* abiRecipe)
{
        // all function starts at "entry" block
        auto entry = CreateBasicBlock("entry", fn);
        builder->SetInsertPoint(entry);
        // Drop any debug location before emitting parameter allocas/stores: instructions tagged
        // with the outer DISubprogram trip the verifier (`!dbg attachment points at wrong subprogram`).
        builder->SetCurrentDebugLocation(llvm::DebugLoc());
        auto& stackState = stackNamedVariable.emplace_back();

        stackState.continueBlock = &fn->back();
        stackState.resumeBlock = &fn->back();
        stackState.isFunction = true;
        stackState.functionName = friendlyName;
        currentFunctionReturnsOwned = returnsOwned;
        currentFunctionReturnIsArrayView = returnIsArrayView;
        currentFunctionReturnTypeName = returnTypeName;
        currentFunctionAbiRecipe = abiRecipe != nullptr ? *abiRecipe : AbiRecipe{};
        // The bond side channel is per-call: a value left set by a swallowed diagnostic (or by a
        // deferred lambda body) must never answer the next function's return/assign checks.
        lastCallIsBonded = false;
        lastCallBondByAddress = false;
        lastCallBondedSources.clear();
        // Reset the per-function alias-scope registry; scopes from a prior function must never leak.
        aliasDomain_ = nullptr;
        aliasScopes_.clear();
        viewScopeByOrigin_.clear();
        // Boxing provenance is per-function value identity; a prior function's entries must not
        // answer a lookup here (see interfaceBoxRecords_).
        interfaceBoxRecords_.clear();
        nullCoalesceJoins_.clear();
        joinArmOccurrences_.clear();
        codeValues_.clear();
        dataValues_.clear();
        codeValueDataCasts_.clear();
        currentCastOccurrence_ = 0;
        owningTempUniqueFields_.clear();
        launderedTempUniqueFields_.clear();
        pendingLaunderTempUniqueFields_.clear();
        dataValueCodeCasts_.clear();
        rawArrayResults_.clear();

        // populate function arguments
        auto itr_nameArg = arguments.begin();
        auto llvmArgIt = fn->arg_begin();
        if (abiRecipe != nullptr && abiRecipe->retSlot.kind == AbiSlot::SRetReturn
            && llvmArgIt != fn->arg_end())
            ++llvmArgIt;
        size_t abiParamIndex = 0;
        for (; itr_nameArg != arguments.end() && llvmArgIt != fn->arg_end(); ++itr_nameArg)
        {
            auto* incomingArg = &*llvmArgIt++;
            incomingArg->setName(itr_nameArg->VariableName);

            const AbiSlot* abiSlot = nullptr;
            llvm::Argument* secondAbiArg = nullptr;
            if (abiRecipe != nullptr && abiParamIndex < abiRecipe->paramSlots.size())
            {
                abiSlot = &abiRecipe->paramSlots[abiParamIndex++];
                if (abiSlot->kind == AbiSlot::CoercePair && llvmArgIt != fn->arg_end())
                    secondAbiArg = &*llvmArgIt++;
            }

            // ABI-lowered struct parameters arrive as pointers, integer/SSE values, or a pair
            // of eightbytes. Rebuild the natural struct value before the normal parameter
            // binding code stores it in the function's local slot.
            llvm::Value* argValue = incomingArg;
            if (abiSlot != nullptr && abiSlot->kind != AbiSlot::Direct)
            {
                if (abiSlot->kind == AbiSlot::ByVal)
                    argValue = builder->CreateLoad(abiSlot->structTy, incomingArg);
                else
                {
                    auto* slot = AllocaAtEntry(abiSlot->structTy, nullptr,
                        itr_nameArg->VariableName + ".abi");
                    if (abiSlot->kind == AbiSlot::CoerceToInt)
                    {
                        StoreCoerceAt(slot, incomingArg, 0);
                        argValue = builder->CreateLoad(abiSlot->structTy, slot);
                    }
                    else if (abiSlot->kind == AbiSlot::CoercePair && secondAbiArg != nullptr)
                    {
                        StoreCoerceAt(slot, incomingArg, 0);
                        StoreCoerceAt(slot, secondAbiArg, 8);
                        argValue = builder->CreateLoad(abiSlot->structTy, slot);
                    }
                }
            }

            llvm::Argument* rawArrayCountArg = nullptr;
            if (abiRecipe == nullptr && ParameterCarriesRawArrayCount(*itr_nameArg)
                && llvmArgIt != fn->arg_end())
                rawArrayCountArg = &*llvmArgIt++;
            if (rawArrayCountArg != nullptr)
                rawArrayCountArg->setName(itr_nameArg->VariableName + ".raw_array_count");

            // A non-pointer `alias T` param arrives as a POINTER to the caller's object: bind
            // Storage to it directly so reads, writes and `&param` all reach the caller's slot.
            if (ParameterIsAliasByPointer(*itr_nameArg))
            {
                NamedVariable namedVar{
                    .TypeAndValue = *itr_nameArg,
                    .BaseType = GetType(*itr_nameArg),
                    .Primary = nullptr,
                    .Storage = argValue,
                };
                RegisterFunctionArgument(itr_nameArg->VariableName, namedVar);
            }
            // An `IFace[]` view arrives as a thin pointer to a run of fat structs, not as one
            // fat struct by value, so it must fall through to the pointer-parameter path below.
            else if (itr_nameArg->IsFatInterfaceValue())
            {
                // Interface args arrive by value ({i8*,i8*}). Store in a temp alloca so
                // Storage is a {i8*,i8*}* pointer suitable for CallInterfaceMethod GEP.
                // The parameter's own slot bypasses CreateLocalVariable, and a direct call passing
                // an already-fat argument bypasses CoerceArgToInterface, so record it here.
                RecordInterfaceMaterialization(itr_nameArg->TypeName, "the type of a parameter");
                auto fatTy = GetFatPtrType();
                auto tmp = builder->CreateAlloca(fatTy, nullptr, itr_nameArg->VariableName);
                builder->CreateStore(argValue, tmp);
                NamedVariable namedVar{
                    .TypeAndValue = *itr_nameArg,
                    .BaseType = fatTy,
                    .Primary = nullptr,
                    .Storage = tmp,
                    // A `move` interface param takes ownership: mark it owning so 'delete x' is
                    // permitted (not a borrow) and scope exit destructs it if not deleted. A plain
                    .IsOwning = itr_nameArg->IsMove,
                };
                RegisterFunctionArgument(itr_nameArg->VariableName, namedVar);
            }
            else if (itr_nameArg->IsMove && itr_nameArg->Pointer)
            {
                // move parameter: alloca a slot so we can null it out on scope exit
                auto* ptrTy = GetType(*itr_nameArg, nullptr, true);
                auto* alloc = builder->CreateAlloca(ptrTy, nullptr, itr_nameArg->VariableName);
                builder->CreateStore(argValue, alloc);
                NamedVariable namedVar{
                    .TypeAndValue = *itr_nameArg,
                    .BaseType = ptrTy,
                    .Primary = nullptr,
                    .Storage = alloc,
                    .IsOwning = true,
                    // `alignas(_, N)` on the param: the block is N-aligned, so scope-exit cleanup
                    // frees via __delete_aligned. The call site already checked the arg agrees.
                    .AllocAlignment = itr_nameArg->AllocAlignValue,
                };
                namedVar.RawArrayLengthStorage =
                    AllocaAtEntry(builder->getInt64Ty(), nullptr,
                                  itr_nameArg->VariableName + ".raw_array_count");
                builder->CreateStore(rawArrayCountArg != nullptr
                    ? static_cast<llvm::Value*>(rawArrayCountArg)
                    : static_cast<llvm::Value*>(builder->getInt64(-1)),
                    namedVar.RawArrayLengthStorage);
                namedVar.AllocatedByRawNewArray = true;
                RegisterFunctionArgument(itr_nameArg->VariableName, namedVar);
            }
            else if (itr_nameArg->IsMove && itr_nameArg->TypeName == "string")
            {
                // move string parameter: alloca a slot so the destructor can free the buffer on scope exit
                auto* strTy = GetType(*itr_nameArg, nullptr, false);
                auto* alloc = builder->CreateAlloca(strTy, nullptr, itr_nameArg->VariableName);
                builder->CreateStore(argValue, alloc);
                NamedVariable namedVar{
                    .TypeAndValue = *itr_nameArg,
                    .BaseType = strTy,
                    .Primary = nullptr,
                    .Storage = alloc,
                    .IsOwningString = true,
                };
                RegisterFunctionArgument(itr_nameArg->VariableName, namedVar);
            }
            else if (itr_nameArg->IsMove && !itr_nameArg->Pointer)
            {
                // move struct parameter: alloca a slot so the destructor runs on scope exit
                auto* structTy = GetType(*itr_nameArg, nullptr, false);
                // An unresolved generic leaves a registered-but-unsized shell; allocating it
                // asserts inside LLVM ("Cannot getTypeInfo() on a type that is unsized!").
                // Give the slot a sized placeholder and skip the store, but keep BaseType as the
                // shell so member lookup still fails where it should - the accurate diagnostic is
                // the downstream one, and pre-empting it here would report a worse error.
                bool unsizedShell = structTy != nullptr && structTy->isStructTy() && !structTy->isSized();
                auto* slotTy = unsizedShell ? (llvm::Type*)builder->getInt8Ty() : structTy;
                auto* alloc = builder->CreateAlloca(slotTy, nullptr, itr_nameArg->VariableName);
                if (!unsizedShell)
                    builder->CreateStore(argValue, alloc);
                NamedVariable namedVar{
                    .TypeAndValue = *itr_nameArg,
                    .BaseType = structTy,
                    .Primary = nullptr,
                    .Storage = alloc,
                    .IsOwningStruct = true,
                };
                RegisterFunctionArgument(itr_nameArg->VariableName, namedVar);
            }
            else if (!itr_nameArg->Pointer && GetType(*itr_nameArg)->isStructTy())
            {
                // Struct value parameter: store into an alloca so its address is available
                // (e.g. for reflect(), GEP field access, and taking &param).
                auto* structTy = GetType(*itr_nameArg);
                // Same unsized-shell handling as the move-struct arm above.
                bool unsizedShell = structTy != nullptr && structTy->isStructTy() && !structTy->isSized();
                auto* slotTy = unsizedShell ? (llvm::Type*)builder->getInt8Ty() : structTy;
                auto* alloc = builder->CreateAlloca(slotTy, nullptr, itr_nameArg->VariableName);
                if (!unsizedShell)
                    builder->CreateStore(argValue, alloc);
                NamedVariable namedVar{
                    .TypeAndValue = *itr_nameArg,
                    .BaseType = structTy,
                    .Storage = alloc,
                };
                // A plain by-value OWNING-VALUE param (string/owning struct/fat closure) that is
                // NOT a sink borrows the caller's value - the caller keeps ownership. Mark it so a
                // downstream consuming call (a sink/`move`/unique param) is rejected, not laundered.
                // A consume-inferred sink of a COPYABLE type does NOT consume (store is a copy), so
                // it is still a borrow here - OwningSinkConsumesConcrete makes that call per T.
                // A by-value core unique<X> param is a synthesized sink, exactly as the
                // `unique X*` spelling it desugars from: the callee owns it on entry.
                bool coreUniqueValueParam = !itr_nameArg->Pointer
                    && IsCoreUniqueType(itr_nameArg->TypeName);
                if (!OwningSinkConsumesConcrete(*itr_nameArg) && !itr_nameArg->IsMove
                    && !itr_nameArg->IsAlias
                    && !coreUniqueValueParam
                    && IsOwningValueOrClosureType(itr_nameArg->TypeName))
                    namedVar.IsBorrowedOwningValue = true;
                // 8a: an owning sink that CONSUMES for this concrete type (a non-copyable owner) OWNS
                // the value on entry. A path that does not move it out must release it at scope exit,
                // or the poisoned caller's value leaks. Marking it owning routes it through the
                // scope-exit full destructor (a no-op on a moved-out/nulled slot, same as a local).
                if ((OwningSinkConsumesConcrete(*itr_nameArg) || coreUniqueValueParam)
                    && IsOwningValueType(itr_nameArg->TypeName)
                    && !IsCopyableType(itr_nameArg->TypeName))
                    namedVar.IsOwningStruct = true;
                RegisterFunctionArgument(itr_nameArg->VariableName, namedVar);
            }
            else
            {
                // Alloca all remaining params (scalars and non-owning pointers) so they
                // are writable (CreateStore targets) just like local variables.
                auto* ty = GetType(*itr_nameArg, nullptr, itr_nameArg->Pointer);
                auto* alloc = builder->CreateAlloca(ty, nullptr, itr_nameArg->VariableName);
                builder->CreateStore(argValue, alloc);
                NamedVariable namedVar
                {
                    .TypeAndValue = *itr_nameArg,
                    .BaseType = ty,
                    .Storage = alloc,
                };
                // Non-move pointer parameters borrow from the caller. Track this so we can
                // reject 'delete' on the param or any local that aliases it via assignment/cast.
                if (itr_nameArg->Pointer && !itr_nameArg->IsMove)
                {
                    namedVar.IsBorrowed = true;
                    namedVar.BorrowedOrigin = itr_nameArg->VariableName;
                }
                RegisterFunctionArgument(itr_nameArg->VariableName, namedVar);
            }
            if (symbolSink_ && !itr_nameArg->VariableName.empty())
                symbolSink_->RegisterVariable(itr_nameArg->VariableName, itr_nameArg->TypeName);

            RecordMoveGenBind(itr_nameArg->VariableName); // fresh parameter binding

        }

        currentFunction = fn;
        autoVaListAlloca = nullptr;
    }

llvm::DIType* LLVMBackend::GetDIType(const TypeAndValue& tv)
{
        using namespace llvm::dwarf;
        const llvm::DataLayout& DL = module->getDataLayout();
        unsigned ptrBits = DL.getPointerSizeInBits();

        // Fixed-size array: T[N] - wrap element DI in an array DIType.
        if (tv.ConstArraySize > 0)
        {
            TypeAndValue elem = tv;
            elem.ConstArraySize = 0;
            elem.ConstInnerDimensions.clear();
            auto* elemDI = GetDIType(elem);
            auto* elemTy = GetType(elem, nullptr, elem.Pointer);
            uint64_t total = DL.getTypeAllocSizeInBits(elemTy) * tv.ConstArraySize;
            llvm::SmallVector<llvm::Metadata*, 1> subs;
            subs.push_back(diBuilder->getOrCreateSubrange(0, (int64_t)tv.ConstArraySize));
            return diBuilder->createArrayType(total, 0, elemDI,
                diBuilder->getOrCreateArray(subs));
        }

        // Function pointer: build subroutine + pointer wrapper.
        if (tv.IsFunctionPointer)
        {
            std::vector<llvm::Metadata*> types;
            TypeAndValue retTV;
            retTV.TypeName = tv.FuncPtrReturnTypeName;
            retTV.Pointer = tv.FuncPtrReturnPointer;
            types.push_back((retTV.TypeName == "void" && !retTV.Pointer)
                ? nullptr : (llvm::Metadata*)GetDIType(retTV));
            for (const auto& p : tv.FuncPtrParams)
            {
                TypeAndValue pTV; pTV.TypeName = p.TypeName; pTV.Pointer = p.Pointer;
                types.push_back(GetDIType(pTV));
            }
            auto srt = diBuilder->createSubroutineType(
                diBuilder->getOrCreateTypeArray(types));
            return diBuilder->createPointerType(srt, ptrBits);
        }

        // Pointer wrapper: strip and recurse on base.
        if (tv.Pointer)
        {
            TypeAndValue base = tv;
            base.Pointer = false;
            auto* baseDI = GetDIType(base);
            return diBuilder->createPointerType(baseDI, ptrBits);
        }

        // Interface fat pointer {i8* vtable, i8* data} - cached once.
        if (tv.IsFatInterfaceValue())
        {
            auto it = diTypeCache.find("__interface_fatptr");
            if (it != diTypeCache.end()) return it->second;
            auto i8DI = diBuilder->createBasicType("i8", 8, DW_ATE_unsigned_char);
            auto i8Ptr = diBuilder->createPointerType(i8DI, ptrBits);
            std::vector<llvm::Metadata*> members = {
                diBuilder->createMemberType(compileUnit, "vtable", diFile, 0,
                    ptrBits, ptrBits, 0, llvm::DINode::FlagZero, i8Ptr),
                diBuilder->createMemberType(compileUnit, "data", diFile, 0,
                    ptrBits, ptrBits, ptrBits, llvm::DINode::FlagZero, i8Ptr),
            };
            auto fatTy = diBuilder->createStructType(compileUnit, "__interface",
                diFile, 0, ptrBits * 2, ptrBits, llvm::DINode::FlagZero, nullptr,
                diBuilder->getOrCreateArray(members));
            diTypeCache["__interface_fatptr"] = fatTy;
            return fatTy;
        }

        // Cache lookup for basic + struct types.
        auto cacheIt = diTypeCache.find(tv.TypeName);
        if (cacheIt != diTypeCache.end()) return cacheIt->second;

        llvm::DIType* basic = nullptr;
        if      (tv.TypeName == "int")    basic = diBuilder->createBasicType("int", 32, DW_ATE_signed);
        else if (tv.TypeName == "char")   basic = diBuilder->createBasicType("char", 8, DW_ATE_signed_char);
        else if (tv.TypeName == "short")  basic = diBuilder->createBasicType("short", 16, DW_ATE_signed);
        else if (tv.TypeName == "long")   basic = diBuilder->createBasicType("long", longBits_, DW_ATE_signed);
        else if (tv.TypeName == "ulong")  basic = diBuilder->createBasicType("ulong", longBits_, DW_ATE_unsigned);
        else if (tv.TypeName == "float")  basic = diBuilder->createBasicType("float", 32, DW_ATE_float);
        else if (tv.TypeName == "double") basic = diBuilder->createBasicType("double", 64, DW_ATE_float);
        else if (tv.TypeName == "bool")   basic = diBuilder->createBasicType("bool", 1, DW_ATE_boolean);
        else if (tv.TypeName == "i8")     basic = diBuilder->createBasicType("i8", 8, DW_ATE_signed);
        else if (tv.TypeName == "i16")    basic = diBuilder->createBasicType("i16", 16, DW_ATE_signed);
        else if (tv.TypeName == "i32")    basic = diBuilder->createBasicType("i32", 32, DW_ATE_signed);
        else if (tv.TypeName == "i64")    basic = diBuilder->createBasicType("i64", 64, DW_ATE_signed);
        else if (tv.TypeName == "u8")     basic = diBuilder->createBasicType("u8", 8, DW_ATE_unsigned);
        else if (tv.TypeName == "u16")    basic = diBuilder->createBasicType("u16", 16, DW_ATE_unsigned);
        else if (tv.TypeName == "u32")    basic = diBuilder->createBasicType("u32", 32, DW_ATE_unsigned);
        else if (tv.TypeName == "u64")    basic = diBuilder->createBasicType("u64", 64, DW_ATE_unsigned);
        else if (tv.TypeName == "void")   basic = diBuilder->createUnspecifiedType("void");

        if (basic)
        {
            diTypeCache[tv.TypeName] = basic;
            return basic;
        }

        // Struct (including generic instantiations like Box__int and the built-in `string`).
        auto sdIt = dataStructures.find(tv.TypeName);
        if (sdIt != dataStructures.end() && sdIt->second.StructType != nullptr
            && !sdIt->second.StructType->isOpaque())
        {
            auto* st = sdIt->second.StructType;
            uint64_t sizeBits = DL.getTypeAllocSizeInBits(st);
            uint64_t alignBits = (uint64_t)DL.getABITypeAlign(st).value() * 8;
            const llvm::StructLayout* SL = DL.getStructLayout(st);

            // Forward-declare into cache before recursing into fields so that self-referential
            // or mutually-recursive structs resolve without infinite recursion.
            auto fwd = diBuilder->createReplaceableCompositeType(
                llvm::dwarf::DW_TAG_structure_type, tv.TypeName, compileUnit, diFile, 0);
            diTypeCache[tv.TypeName] = fwd;

            std::vector<llvm::Metadata*> members;
            const auto& fields = sdIt->second.StructFields;
            unsigned n = st->getNumElements();
            for (size_t i = 0; i < fields.size() && i < n; ++i)
            {
                const auto& f = fields[i];
                if (f.IsPadding) continue;   // synthetic alignment slot: not a member
                auto* fieldTy = st->getElementType((unsigned)i);
                uint64_t fSize = DL.getTypeAllocSizeInBits(fieldTy);
                uint64_t fAlign = (uint64_t)DL.getABITypeAlign(fieldTy).value() * 8;
                uint64_t fOffset = SL->getElementOffsetInBits((unsigned)i);
                auto* fDI = GetDIType(f);
                members.push_back(diBuilder->createMemberType(
                    fwd, f.VariableName, diFile, 0,
                    fSize, (uint32_t)fAlign, fOffset, llvm::DINode::FlagZero, fDI));
            }

            auto real = diBuilder->createStructType(
                compileUnit, tv.TypeName, diFile, 0,
                sizeBits, (uint32_t)alignBits, llvm::DINode::FlagZero, nullptr,
                diBuilder->getOrCreateArray(members));

            fwd->replaceAllUsesWith(real);
            diTypeCache[tv.TypeName] = real;
            return real;
        }

        // Unknown / opaque - leave as unspecified so the debugger at least knows the name.
        // Do NOT cache: a struct may be opaque now and become real later (e.g. generic before body layout).
        return diBuilder->createUnspecifiedType(tv.TypeName);
    }

void LLVMBackend::RegisterBuiltinString()
{
        auto* ptrTy = cflat_llvm::PointerTo(builder->getInt8Ty());
        auto* i32Ty = builder->getInt32Ty();

        auto* strTy = llvm::StructType::getTypeByName(*context, "string");
        if (!strTy)
            strTy = llvm::StructType::create(*context, { ptrTy, i32Ty }, "string");

        DeclTypeAndValue ptrField;
        ptrField.TypeName = "i8";
        ptrField.VariableName = "_ptr";
        ptrField.Pointer = true;

        DeclTypeAndValue lenField;
        lenField.TypeName = "i32";
        lenField.VariableName = "_len";

        dataStructures["string"].StructType = strTy;
        dataStructures["string"].StructFields = { ptrField, lenField };

        // Create string() default constructor -> string{nullptr, 0}
        {
            auto* fnTy = llvm::FunctionType::get(strTy, {}, false);
            auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "string.ctor", *module);
            auto* entry = llvm::BasicBlock::Create(*context, "entry", fn);
            llvm::IRBuilder<> b(entry);
            b.CreateRet(llvm::ConstantAggregateZero::get(strTy));

            FunctionSymbol sym;
            sym.UniqueName = "string.ctor";
            sym.Function = fn;
            sym.ReturnType = TypeAndValue{ "string", "", false };
            sym.Parameters = {};
            functionTable["string"].push_back(sym);
        }

        // string.data/length/hash are library functions in core/string.cb. Only the bare
        // {i8*,i32} layout + default ctor live here for bootstrap (literal typing, __FILE__).

        // String destructor is registered lazily via EnsureStringDtorRegistered()
        // so that the C `free` function is available in the function table first.
    }

void LLVMBackend::RegisterBuiltinStrConcat()
{
        auto* i8Ty     = builder->getInt8Ty();
        auto* ptrTy    = cflat_llvm::PointerTo(i8Ty);
        auto* ptrPtrTy = cflat_llvm::PointerTo(ptrTy);
        auto* i32Ty    = builder->getInt32Ty();
        auto* i32PtrTy = cflat_llvm::PointerTo(i32Ty);
        auto* i64Ty    = builder->getInt64Ty();
        auto* strTy    = llvm::StructType::getTypeByName(*context, "string");

        // Use the function-table overload of malloc so that on Win32 we get the
        // i64-wrapper (_malloc_U8Ptr_i64_) rather than the raw extern malloc(i32).
        auto* mallocFn = GetFunction("malloc");
        if (!mallocFn)
        {
            auto* mallocTy = llvm::FunctionType::get(ptrTy, { i64Ty }, false);
            mallocFn = llvm::dyn_cast<llvm::Function>(
                module->getOrInsertFunction("malloc", mallocTy).getCallee());
        }

        auto* fnTy = llvm::FunctionType::get(strTy, { ptrPtrTy, i32PtrTy, i32Ty }, false);
        auto* fn   = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, "__strconcat", *module);
        auto  argIt    = fn->arg_begin();
        auto* argPtrs  = &*argIt++;  argPtrs->setName("ptrs");
        auto* argLens  = &*argIt++;  argLens->setName("lens");
        auto* argCount = &*argIt;    argCount->setName("count");

        auto* entryBB = llvm::BasicBlock::Create(*context, "entry",    fn);
        auto* sumCond = llvm::BasicBlock::Create(*context, "sum.cond", fn);
        auto* sumBody = llvm::BasicBlock::Create(*context, "sum.body", fn);
        auto* allocBB = llvm::BasicBlock::Create(*context, "alloc",    fn);
        auto* cpyCond = llvm::BasicBlock::Create(*context, "cpy.cond", fn);
        auto* cpyBody = llvm::BasicBlock::Create(*context, "cpy.body", fn);
        auto* cpyNext = llvm::BasicBlock::Create(*context, "cpy.next", fn);
        auto* nullBB  = llvm::BasicBlock::Create(*context, "null",     fn);

        llvm::IRBuilder<> rb(entryBB);
        auto* totalA = rb.CreateAlloca(i32Ty, nullptr, "total");
        auto* bufA   = rb.CreateAlloca(ptrTy, nullptr, "buf");
        auto* dstA   = rb.CreateAlloca(ptrTy, nullptr, "dst");
        auto* idxA   = rb.CreateAlloca(i32Ty, nullptr, "idx");
        rb.CreateStore(rb.getInt32(0), totalA);
        rb.CreateStore(rb.getInt32(0), idxA);
        rb.CreateBr(sumCond);

        // Sum loop: total = sum of all segment lengths
        rb.SetInsertPoint(sumCond);
        rb.CreateCondBr(rb.CreateICmpSLT(rb.CreateLoad(i32Ty, idxA), argCount), sumBody, allocBB);
        rb.SetInsertPoint(sumBody);
        {
            auto* i   = rb.CreateLoad(i32Ty, idxA);
            auto* len = rb.CreateLoad(i32Ty, rb.CreateGEP(i32Ty, argLens, i));
            rb.CreateStore(rb.CreateAdd(rb.CreateLoad(i32Ty, totalA), len), totalA);
            rb.CreateStore(rb.CreateAdd(i, rb.getInt32(1)), idxA);
            rb.CreateBr(sumCond);
        }

        // Allocate buffer: total + 1 bytes
        rb.SetInsertPoint(allocBB);
        {
            auto* total   = rb.CreateLoad(i32Ty, totalA);
            auto* total64 = rb.CreateSExt(total, i64Ty);
            auto* buf     = rb.CreateCall(mallocFn, { rb.CreateAdd(total64, rb.getInt64(1)) }, "buf");
            rb.CreateStore(buf, bufA);
            rb.CreateStore(buf, dstA);
            rb.CreateStore(rb.getInt32(0), idxA);
            rb.CreateBr(cpyCond);
        }

        // Copy loop: iterate over segments
        rb.SetInsertPoint(cpyCond);
        rb.CreateCondBr(rb.CreateICmpSLT(rb.CreateLoad(i32Ty, idxA), argCount), cpyBody, nullBB);

        rb.SetInsertPoint(cpyBody);
        {
            auto* i      = rb.CreateLoad(i32Ty, idxA);
            auto* src    = rb.CreateLoad(ptrTy,  rb.CreateGEP(ptrTy,  argPtrs, i));
            auto* segLen = rb.CreateLoad(i32Ty,  rb.CreateGEP(i32Ty,  argLens, i));
            auto* jA     = rb.CreateAlloca(i32Ty, nullptr, "j");
            rb.CreateStore(rb.getInt32(0), jA);
            auto* bCond  = llvm::BasicBlock::Create(*context, "b.cond", fn);
            auto* bBody  = llvm::BasicBlock::Create(*context, "b.body", fn);
            rb.CreateBr(bCond);
            rb.SetInsertPoint(bCond);
            rb.CreateCondBr(rb.CreateICmpSLT(rb.CreateLoad(i32Ty, jA), segLen), bBody, cpyNext);
            rb.SetInsertPoint(bBody);
            {
                auto* j2     = rb.CreateLoad(i32Ty, jA);
                auto* dstNow = rb.CreateLoad(ptrTy, dstA);
                auto* byte   = rb.CreateLoad(i8Ty, rb.CreateGEP(i8Ty, src, j2));
                rb.CreateStore(byte, rb.CreateGEP(i8Ty, dstNow, j2));
                rb.CreateStore(rb.CreateAdd(j2, rb.getInt32(1)), jA);
                rb.CreateBr(bCond);
            }
        }

        rb.SetInsertPoint(cpyNext);
        {
            auto* i      = rb.CreateLoad(i32Ty, idxA);
            auto* segLen = rb.CreateLoad(i32Ty, rb.CreateGEP(i32Ty, argLens, i));
            auto* dst    = rb.CreateLoad(ptrTy, dstA);
            auto* sLen64 = rb.CreateSExt(segLen, i64Ty);
            rb.CreateStore(rb.CreateGEP(i8Ty, dst, sLen64), dstA);
            rb.CreateStore(rb.CreateAdd(i, rb.getInt32(1)), idxA);
            rb.CreateBr(cpyCond);
        }

        // Null-terminate the concatenated buffer, build and return a string struct by value.
        rb.SetInsertPoint(nullBB);
        {
            rb.CreateStore(rb.getInt8(0), rb.CreateLoad(ptrTy, dstA));
            llvm::Value* strVal = llvm::UndefValue::get(strTy);
            strVal = rb.CreateInsertValue(strVal, rb.CreateLoad(ptrTy,  bufA),   { 0u });
            // The concatenated buffer is freshly malloc'd: set _len's high OWNED bit so the
            // result owns it (string-redesign FINAL MODEL). length() masks the bit back off.
            auto* ownedLen = rb.CreateOr(rb.CreateLoad(i32Ty, totalA), rb.getInt32(0x80000000));
            strVal = rb.CreateInsertValue(strVal, ownedLen, { 1u });
            rb.CreateRet(strVal);
        }

        FunctionSymbol sym;
        sym.UniqueName = "__strconcat";
        sym.Function   = fn;
        sym.ReturnType = TypeAndValue{ "string", "", false, false };
        sym.Parameters = {
            TypeAndValue{ "i8",  "ptrs",  true,  false },
            TypeAndValue{ "i32", "lens",  true,  false },
            TypeAndValue{ "i32", "count", false, false },
        };
        functionTable["__strconcat"].push_back(sym);
    }

void LLVMBackend::Init()
{
        context = std::make_unique<llvm::LLVMContext>();
        module = std::make_unique<llvm::Module>("cflat", *context);
        builder = std::make_unique<llvm::IRBuilder<>>(*context);

        // Pre-register the built-in `string` value type { i8* _ptr, i32 _len }.
        RegisterBuiltinString();

        // Pre-register the closure fat type { i8* code, i8* env } (lambda Option A).
        // Dtor + copy registered lazily when function.cb env primitives are available.
        RegisterBuiltinClosure();
    }

void LLVMBackend::RegisterBuiltinClosure()
{
        auto* closureTy = GetClosureFatPtrType();

        DeclTypeAndValue codeField;
        codeField.TypeName = "i8";
        codeField.VariableName = "code";
        codeField.Pointer = true;

        DeclTypeAndValue envField;
        envField.TypeName = "i8";
        envField.VariableName = "env";
        envField.Pointer = true;

        dataStructures["__closure_fat_ptr"].StructType = closureTy;
        dataStructures["__closure_fat_ptr"].StructFields = { codeField, envField };
    }

void LLVMBackend::EnsureClosureLifetimeRegistered()
{
        if (closureLifetimeRegistered) return;

        // Resolve the library primitives. If function.cb has not been compiled yet (e.g. a
        // closure appears in a core file imported before it), defer - try again on next use.
        auto* freeFn  = GetFunction("__closure_env_free");
        auto* cloneFn = GetFunction("__closure_env_clone");
        if (!freeFn || !cloneFn) return;
        closureLifetimeRegistered = true;

        auto* closureTy = GetClosureFatPtrType();
        auto* i8PtrTy   = cflat_llvm::PointerTo(builder->getInt8Ty());
        auto* voidTy    = llvm::Type::getVoidTy(*context);
        auto* i64Ty     = builder->getInt64Ty();
        auto* i32Ty     = builder->getInt32Ty();

        // A per-closure capture-cleanup fn (`void(i8* dst, i8* src, i32 mode)`) is stashed in the
        // env header at (captures - 8) by __closure_env_new, or null for a scalar-only closure.
        // Load it from a TAGGED, non-null owned env; returns null if env is null/borrowed/no-cleanup.
        auto* cleanupFnTy = llvm::FunctionType::get(voidTy, { i8PtrTy, i8PtrTy, i32Ty }, false);
        auto loadCleanup = [&](llvm::IRBuilder<>& b, llvm::Value* env) -> llvm::Value* {
            auto* envInt  = b.CreatePtrToInt(env, i64Ty);
            auto* capAddr = b.CreateAnd(envInt, b.getInt64(~(uint64_t)1));
            auto* slotAddr = b.CreateSub(capAddr, b.getInt64(8));
            auto* slotPtr = b.CreateIntToPtr(slotAddr, cflat_llvm::PointerTo(cflat_llvm::PointerTo(cleanupFnTy)));
            return b.CreateLoad(cflat_llvm::PointerTo(cleanupFnTy), slotPtr, "cleanupfn");
        };
        auto envIsOwned = [&](llvm::IRBuilder<>& b, llvm::Value* env) -> llvm::Value* {
            auto* envInt = b.CreatePtrToInt(env, i64Ty);
            return b.CreateICmpNE(b.CreateAnd(envInt, b.getInt64(1)), b.getInt64(0));
        };
        auto capPtrOf = [&](llvm::IRBuilder<>& b, llvm::Value* env) -> llvm::Value* {
            auto* envInt  = b.CreatePtrToInt(env, i64Ty);
            auto* capAddr = b.CreateAnd(envInt, b.getInt64(~(uint64_t)1));
            return b.CreateIntToPtr(capAddr, i8PtrTy);
        };

        // Destructor: destruct owning captures (via the header cleanup fn), free the heap env
        // (no-op if borrowed/null), and null the field.
        {
            auto* dtorTy = llvm::FunctionType::get(voidTy, { cflat_llvm::PointerTo(closureTy) }, false);
            auto* dtorFn = llvm::Function::Create(dtorTy, llvm::Function::InternalLinkage,
                                                  "__closure_fat_ptr.dtor", *module);
            dtorFn->arg_begin()->setName("self");
            auto* entry = llvm::BasicBlock::Create(*context, "entry", dtorFn);
            auto* ownBB = llvm::BasicBlock::Create(*context, "owned", dtorFn);
            auto* callBB = llvm::BasicBlock::Create(*context, "callcleanup", dtorFn);
            auto* freeBB = llvm::BasicBlock::Create(*context, "freeenv", dtorFn);
            llvm::IRBuilder<> b(entry);
            auto* self   = &*dtorFn->arg_begin();
            auto* envPtr = b.CreateStructGEP(closureTy, self, 1, "envfield");
            auto* env    = b.CreateLoad(i8PtrTy, envPtr, "env");
            b.CreateCondBr(envIsOwned(b, env), ownBB, freeBB);

            b.SetInsertPoint(ownBB);
            auto* cleanup = loadCleanup(b, env);
            b.CreateCondBr(b.CreateICmpNE(cleanup,
                llvm::ConstantPointerNull::get(cflat_llvm::PointerTo(cleanupFnTy))), callBB, freeBB);

            b.SetInsertPoint(callBB);
            b.CreateCall(cleanupFnTy, cleanup,
                { capPtrOf(b, env), llvm::ConstantPointerNull::get(i8PtrTy), b.getInt32(0) });
            b.CreateBr(freeBB);

            b.SetInsertPoint(freeBB);
            b.CreateCall(freeFn->getFunctionType(), freeFn, { env });
            b.CreateStore(llvm::ConstantPointerNull::get(i8PtrTy), envPtr);
            b.CreateRetVoid();
            RegisterDestructor("__closure_fat_ptr", dtorFn);
        }

        // Registered so HasCopyOverloadFor short-circuits memberwise synth, which would
        // shallow-copy the env pointer and double-free.
        {
            auto* copyTy = llvm::FunctionType::get(closureTy, { closureTy }, false);
            auto* copyFn = llvm::Function::Create(copyTy, llvm::Function::InternalLinkage,
                                                  "__closure_fat_ptr.copy", *module);
            copyFn->arg_begin()->setName("self");
            auto* entry  = llvm::BasicBlock::Create(*context, "entry", copyFn);
            auto* ownBB  = llvm::BasicBlock::Create(*context, "owned", copyFn);
            auto* callBB = llvm::BasicBlock::Create(*context, "callcleanup", copyFn);
            auto* doneBB = llvm::BasicBlock::Create(*context, "done", copyFn);
            llvm::IRBuilder<> b(entry);
            auto* self   = &*copyFn->arg_begin();
            auto* code   = b.CreateExtractValue(self, { 0u }, "code");
            auto* env    = b.CreateExtractValue(self, { 1u }, "env");
            auto* newEnv = b.CreateCall(cloneFn->getFunctionType(), cloneFn, { env }, "clonedenv");
            // Deep-copy owning captures into the fresh env (the byte-clone left them aliasing src).
            b.CreateCondBr(envIsOwned(b, newEnv), ownBB, doneBB);

            b.SetInsertPoint(ownBB);
            auto* cleanup = loadCleanup(b, newEnv);
            b.CreateCondBr(b.CreateICmpNE(cleanup,
                llvm::ConstantPointerNull::get(cflat_llvm::PointerTo(cleanupFnTy))), callBB, doneBB);

            b.SetInsertPoint(callBB);
            b.CreateCall(cleanupFnTy, cleanup, { capPtrOf(b, newEnv), capPtrOf(b, env), b.getInt32(1) });
            b.CreateBr(doneBB);

            b.SetInsertPoint(doneBB);
            llvm::Value* fat = llvm::UndefValue::get(closureTy);
            fat = b.CreateInsertValue(fat, code,   { 0u });
            fat = b.CreateInsertValue(fat, newEnv, { 1u });
            b.CreateRet(fat);

            FunctionSymbol sym;
            sym.UniqueName = "__closure_fat_ptr.copy";
            sym.Function   = copyFn;
            sym.ReturnType = TypeAndValue{ "__closure_fat_ptr", "", false };
            sym.ReturnType.IsMove = true;   // the fresh clone is owned by the caller
            sym.Parameters = { TypeAndValue{ "__closure_fat_ptr", "self", false } };
            functionTable["copy"].push_back(sym);
        }
    }

void LLVMBackend::RegisterEncodedClosureType(const std::string& encodedName, const TypeAndValue& sig)
{
        if (encodedClosureTypes_.count(encodedName)) return;
        encodedClosureTypes_[encodedName] = sig;
        EnsureClosureLifetimeRegistered();

        if (!sig.IsThinFnPtr())
        {
            // Fat owning closure: alias the closure fat struct + its dtor so the encoded name is an
            // owning value type (container frees envs; GetType resolves; pointer-wrap works).
            auto& base = dataStructures["__closure_fat_ptr"];
            auto& ds = dataStructures[encodedName];
            ds.StructType   = base.StructType;
            ds.StructFields = base.StructFields;
            ds.Destructor   = base.Destructor;
            // Bind clone-by-default to the closure copy so a copy request deep-clones the env rather
            // than falling back to the memberwise synth (which shallow-shares env -> double-free).
            if (auto* copyFn = module->getFunction("__closure_fat_ptr.copy"))
            {
                FunctionSymbol sym;
                sym.UniqueName = encodedName + ".copy";
                sym.Function   = copyFn;
                sym.ReturnType = TypeAndValue{ encodedName, "", false };
                sym.ReturnType.IsMove = true;
                sym.Parameters = { TypeAndValue{ encodedName, "self", false } };
                functionTable["copy"].push_back(sym);
            }
        }
        // Thin C fn ptr: a bare code pointer, exactly like a non-generic `function<T>` value. It
        // gets NO struct backing - GetType lowers the encoded name straight to the fn-ptr type.
    }

bool LLVMBackend::IsEncodedClosureType(const std::string& name) const
{
        return encodedClosureTypes_.count(name) != 0;
    }

bool LLVMBackend::IsThinEncodedClosureType(const std::string& name) const
{
        auto it = encodedClosureTypes_.find(name);
        return it != encodedClosureTypes_.end() && it->second.IsThinFnPtr();
    }

std::string LLVMBackend::SpellEncodedClosureType(const TypeAndValue& enc) const
{
        return std::format("{}<{}>", enc.IsThinFnPtr() ? "function" : "Lambda", FuncPtrSpellingOf(enc));
    }

bool LLVMBackend::IsFatEncodedClosureType(const std::string& name) const
{
        auto it = encodedClosureTypes_.find(name);
        return it != encodedClosureTypes_.end() && !it->second.IsThinFnPtr();
    }

const LLVMBackend::TypeAndValue* LLVMBackend::GetEncodedClosureType(const std::string& name) const
{
        auto it = encodedClosureTypes_.find(name);
        return it == encodedClosureTypes_.end() ? nullptr : &it->second;
    }

void LLVMBackend::EnsureStringDtorRegistered()
{
        if (stringDtorRegistered) return;
        stringDtorRegistered = true;

        auto* strTy = llvm::StructType::getTypeByName(*context, "string");
        if (!strTy) return;

        // Prefer operator delete (allocator-aware) over raw free.
        // operator delete is from memory.cb which is imported before string.cb.
        auto* freeFn  = GetFunction("operator delete");
        if (!freeFn) freeFn = module->getFunction("free");
        if (!freeFn) return;   // neither available yet; destructor cannot be created

        auto* voidTy   = llvm::Type::getVoidTy(*context);
        auto* ptrTy    = cflat_llvm::PointerTo(builder->getInt8Ty());
        auto* strPtrTy = cflat_llvm::PointerTo(strTy);

        auto* dtorFnTy = llvm::FunctionType::get(voidTy, { strPtrTy }, false);
        auto* dtorFn   = llvm::Function::Create(dtorFnTy, llvm::Function::InternalLinkage, "string.dtor", *module);
        dtorFn->arg_begin()->setName("self");

        auto* entry = llvm::BasicBlock::Create(*context, "entry", dtorFn);
        llvm::IRBuilder<> b(entry);

        auto* i32Ty   = b.getInt32Ty();
        auto* self    = &*dtorFn->arg_begin();
        auto* ptrPtr  = b.CreateStructGEP(strTy, self, 0, "ptrfield");
        auto* lenPtr  = b.CreateStructGEP(strTy, self, 1, "lenfield");
        auto* ptr     = b.CreateLoad(ptrTy, ptrPtr, "ptr");
        auto* len     = b.CreateLoad(i32Ty, lenPtr, "len");
        auto* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
        // Free iff this string OWNS its buffer (_len high bit = OWNED flag).
        // Borrowed strings (literals, char* wraps) have bit clear - safe to call dtor on ANY string.
        auto* isOwned   = b.CreateICmpNE(
            b.CreateAnd(len, b.getInt32(0x80000000)), b.getInt32(0), "is_owned");
        auto* isNotNull = b.CreateICmpNE(ptr, nullPtr, "is_not_null");
        auto* shouldFree = b.CreateAnd(isOwned, isNotNull, "should_free");
        auto* freeBlock = llvm::BasicBlock::Create(*context, "free", dtorFn);
        auto* doneBlock = llvm::BasicBlock::Create(*context, "done", dtorFn);
        b.CreateCondBr(shouldFree, freeBlock, doneBlock);
        b.SetInsertPoint(freeBlock);
        b.CreateCall(freeFn->getFunctionType(), freeFn, { ptr });
        b.CreateBr(doneBlock);
        b.SetInsertPoint(doneBlock);
        b.CreateStore(nullPtr, ptrPtr);
        b.CreateStore(b.getInt32(0), lenPtr);
        b.CreateRetVoid();

        RegisterDestructor("string", dtorFn);
    }

llvm::Value* LLVMBackend::ClearStringOwnedBit(llvm::Value* value)
{
        if (value == nullptr) return value;
        auto* strTy = llvm::StructType::getTypeByName(*context, "string");
        if (strTy == nullptr || value->getType() != strTy) return value;
        auto* len    = builder->CreateExtractValue(value, { 1 }, "borrow.len");
        auto* masked = builder->CreateAnd(len, builder->getInt32(0x7FFFFFFF), "borrow.len.noown");
        return builder->CreateInsertValue(value, masked, { 1 }, "borrow.str");
    }

llvm::Value* LLVMBackend::ClearStructOwnedBits(llvm::Value* value, const std::string& typeName)
{
        if (value == nullptr || typeName.empty()) return value;
        auto dsIt = dataStructures.find(typeName);
        if (dsIt == dataStructures.end()) return value;
        auto* structTy = dsIt->second.StructType;
        if (structTy == nullptr || value->getType() != structTy) return value;

        for (unsigned i = 0; i < dsIt->second.StructFields.size(); ++i)
        {
            const auto& f = dsIt->second.StructFields[i];
            if (f.Pointer || f.ElemPointer || f.IsArrayView || f.IsSimd || f.IsBitfield || f.IsPadding)
                continue;
            if (f.ConstArraySize > 0)
            {
                // Owning fixed-array value field: clear the owned bit on every element so an
                // escaped shallow borrow does not free buffers the container still owns. Spill to
                // memory and walk (a pointer walk handles multi-dimensional + large N; ExtractValue
                // would need constant per-dimension indices and could not loop).
                if (f.TypeName != "string" && !IsOwningValueType(f.TypeName))
                    continue;
                llvm::Type* fieldTy = structTy->getElementType(i);
                llvm::Type* elemTy = nullptr;
                uint64_t n = PeelFixedArrayType(fieldTy, elemTy);
                auto* slot = AllocaAtEntry(fieldTy, nullptr, "fbarr.slot");
                builder->CreateStore(builder->CreateExtractValue(value, { i }, "fbarr.fld"), slot);
                EmitFixedArrayElementWalk(*builder, slot, elemTy, n, [&](llvm::Value* elemPtr) {
                    if (f.TypeName == "string")
                    {
                        auto* strTy  = llvm::StructType::getTypeByName(*context, "string");
                        auto* lenPtr = builder->CreateStructGEP(strTy, elemPtr, 1, "fbarr.lenp");
                        auto* len    = builder->CreateLoad(builder->getInt32Ty(), lenPtr, "fbarr.len");
                        auto* masked = builder->CreateAnd(len, builder->getInt32(0x7FFFFFFF), "fbarr.noown");
                        builder->CreateStore(masked, lenPtr);
                    }
                    else
                    {
                        auto* sub = builder->CreateLoad(elemTy, elemPtr, "fbarr.sub");
                        builder->CreateStore(ClearStructOwnedBits(sub, f.TypeName), elemPtr);
                    }
                });
                auto* reloaded = builder->CreateLoad(fieldTy, slot, "fbarr.reload");
                value = builder->CreateInsertValue(value, reloaded, { i }, "fbarr.set");
                continue;
            }
            if (f.TypeName == "string")
            {
                auto* len    = builder->CreateExtractValue(value, { i, 1u }, "fborrow.len");
                auto* masked = builder->CreateAnd(len, builder->getInt32(0x7FFFFFFF), "fborrow.noown");
                value = builder->CreateInsertValue(value, masked, { i, 1u }, "fborrow.str");
            }
            else if (IsOwningValueType(f.TypeName))
            {
                auto* sub = builder->CreateExtractValue(value, { i }, "fborrow.sub");
                sub = ClearStructOwnedBits(sub, f.TypeName);
                value = builder->CreateInsertValue(value, sub, { i }, "fborrow.subset");
            }
        }
        return value;
    }

uint64_t LLVMBackend::PeelFixedArrayType(llvm::Type* ty, llvm::Type*& elemTy)
{
        auto* arrTy = llvm::dyn_cast_or_null<llvm::ArrayType>(ty);
        if (arrTy == nullptr) return 0;
        uint64_t n = 1;
        while (arrTy != nullptr)
        {
            n *= arrTy->getNumElements();
            elemTy = arrTy->getElementType();
            arrTy = llvm::dyn_cast<llvm::ArrayType>(elemTy);
        }
        return n;
    }

void LLVMBackend::EmitFixedArrayElementWalk(llvm::IRBuilder<>& b, llvm::Value* base, llvm::Type* elemTy,
                                   uint64_t n, const std::function<void(llvm::Value*)>& emitElem)
{
        if (base == nullptr || elemTy == nullptr || n == 0) return;

        if (n <= kMaxUnrolledArrayElements)
        {
            for (uint64_t i = 0; i < n; i++)
                emitElem(b.CreateInBoundsGEP(elemTy, base, { b.getInt64(i) }, "arrelem"));
            return;
        }

        auto* fn = b.GetInsertBlock()->getParent();
        auto* preBB  = b.GetInsertBlock();
        auto* loopBB = llvm::BasicBlock::Create(*context, "arrwalk.loop", fn);
        auto* doneBB = llvm::BasicBlock::Create(*context, "arrwalk.done", fn);
        b.CreateBr(loopBB);

        b.SetInsertPoint(loopBB);
        auto* idx = b.CreatePHI(b.getInt64Ty(), 2, "arrwalk.i");
        idx->addIncoming(b.getInt64(0), preBB);
        emitElem(b.CreateInBoundsGEP(elemTy, base, { idx }, "arrelem"));
        auto* next = b.CreateAdd(idx, b.getInt64(1), "arrwalk.next");
        idx->addIncoming(next, b.GetInsertBlock());
        b.CreateCondBr(b.CreateICmpULT(next, b.getInt64(n)), loopBB, doneBB);
        b.SetInsertPoint(doneBB);
    }

void LLVMBackend::EmitFullDestructorOverStorage(llvm::IRBuilder<>& b, llvm::Value* storage,
                                       llvm::Type* storageTy, llvm::Function* dtor)
{
        if (storage == nullptr || dtor == nullptr) return;
        auto callDtor = [&](llvm::Value* p) { b.CreateCall(dtor->getFunctionType(), dtor, { p }); };
        llvm::Type* elemTy = nullptr;
        uint64_t n = PeelFixedArrayType(storageTy, elemTy);
        if (n == 0)
            callDtor(storage);
        else
            EmitFixedArrayElementWalk(b, storage, elemTy, n, callDtor);
    }

void LLVMBackend::EmitUniqueFieldDelete(llvm::IRBuilder<>& b, llvm::Value* fieldPtr,
                               llvm::Function* pointeeDtor, const std::string& typeName,
                               uint64_t allocAlign, llvm::Value* replacement)
{
        auto* ptrTy = llvm::PointerType::get(*context, 0);
        auto* ptrVal = b.CreateLoad(ptrTy, fieldPtr, "uq.ptr");
        auto* skip = b.CreateICmpEQ(ptrVal, llvm::ConstantPointerNull::get(ptrTy), "uq.isnull");
        if (replacement != nullptr && replacement->getType() == ptrTy)
            skip = b.CreateOr(skip, b.CreateICmpEQ(ptrVal, replacement, "uq.same"), "uq.skip");
        auto* fn = b.GetInsertBlock()->getParent();
        auto* deleteBB = llvm::BasicBlock::Create(*context, "uq.delete", fn);
        auto* afterBB  = llvm::BasicBlock::Create(*context, "uq.after", fn);
        b.CreateCondBr(skip, afterBB, deleteBB);

        b.SetInsertPoint(deleteBB);
        if (pointeeDtor != nullptr)
            b.CreateCall(pointeeDtor->getFunctionType(), pointeeDtor, { ptrVal });

        // An over-aligned block came from the aligned allocator, so it must be freed via
        // __delete_aligned to match - same rule as the `delete` site and EmitOwningPtrCleanup.
        uint64_t effAlign = allocAlign;
        TypeAndValue tv{ .TypeName = typeName };
        if (llvm::Type* t = GetType(tv); t != nullptr && t->isSized())
            effAlign = std::max(effAlign, GetEffectiveAlignmentForType(typeName, t));
        llvm::Function* del = effAlign > kDefaultNewAlign ? GetFunction("__delete_aligned")
                                                          : GetFunction("operator delete");
        if (del != nullptr)
            b.CreateCall(del->getFunctionType(), del, { ptrVal });

        b.CreateBr(afterBB);
        b.SetInsertPoint(afterBB);
    }

void LLVMBackend::EmitUniqueArrayFieldRelease(llvm::IRBuilder<>& b, llvm::Value* fieldPtr,
                                     llvm::Type* fieldTy, const std::string& typeName,
                                     bool isIface, uint64_t allocAlign)
{
        if (fieldTy == nullptr || !fieldTy->isArrayTy() || b.GetInsertBlock() == nullptr) return;
        NamedVariable nv;
        nv.Storage = fieldPtr;
        nv.BaseType = fieldTy;
        nv.AllocAlignment = allocAlign;
        nv.TypeAndValue.TypeName = typeName;
        nv.TypeAndValue.IsInterface = isIface;

        auto* savedBB = builder->GetInsertBlock();
        auto savedIt = builder->GetInsertPoint();
        auto* savedFn = currentFunction;
        // Same protocol SaveBuilderState uses: the outer function is suspended mid-body while
        // this dtor is emitted, so the escape analysis must not read it as complete.
        if (savedFn != nullptr) suspendedFunctions_.push_back(savedFn);
        currentFunction = b.GetInsertBlock()->getParent();
        builder->SetInsertPoint(b.GetInsertBlock());
        EmitOwningUniqueArrayCleanup(nv);
        b.SetInsertPoint(builder->GetInsertBlock());
        currentFunction = savedFn;
        if (savedFn != nullptr && !suspendedFunctions_.empty()
            && suspendedFunctions_.back() == savedFn) suspendedFunctions_.pop_back();
        if (savedBB != nullptr) builder->SetInsertPoint(savedBB, savedIt);
    }

void LLVMBackend::EmitUniqueInterfaceFieldRelease(llvm::IRBuilder<>& b, llvm::Value* fieldPtr,
                                         const std::string& ifaceName)
{
        if (fieldPtr == nullptr || b.GetInsertBlock() == nullptr) return;
        NamedVariable nv;
        nv.Storage = fieldPtr;
        nv.BaseType = GetFatPtrType();
        nv.TypeAndValue.TypeName = ifaceName;
        nv.TypeAndValue.IsInterface = true;

        auto* savedBB = builder->GetInsertBlock();
        auto savedIt = builder->GetInsertPoint();
        auto* savedFn = currentFunction;
        // Suspend the outer function for the duration - see EmitUniqueArrayFieldRelease.
        if (savedFn != nullptr) suspendedFunctions_.push_back(savedFn);
        currentFunction = b.GetInsertBlock()->getParent();
        builder->SetInsertPoint(b.GetInsertBlock());
        EmitOwningInterfaceCleanup(nv);
        b.SetInsertPoint(builder->GetInsertBlock());
        currentFunction = savedFn;
        if (savedFn != nullptr && !suspendedFunctions_.empty()
            && suspendedFunctions_.back() == savedFn) suspendedFunctions_.pop_back();
        if (savedBB != nullptr) builder->SetInsertPoint(savedBB, savedIt);
    }

llvm::Function* LLVMBackend::GetOrCreateFullDestructor(const std::string& typeName)
{
        // `string` full dtor is the lazily registered string dtor. Resolve live (never cache)
        // so a call before lazy registration doesn't poison the cache with null.
        if (typeName == "string")
        {
            EnsureStringDtorRegistered();
            auto it = dataStructures.find("string");
            return it != dataStructures.end() ? it->second.Destructor : nullptr;
        }

        // The closure fat type's full dtor is the lazily-registered env-freeing dtor (lambda
        // Option A). Resolve live (never cache a pre-registration null), like `string`.
        if (typeName == "__closure_fat_ptr")
        {
            EnsureClosureLifetimeRegistered();
            auto it = dataStructures.find("__closure_fat_ptr");
            return it != dataStructures.end() ? it->second.Destructor : nullptr;
        }

        // Only synthesized wrappers are cached - they are stable once built. Direct lookups
        // are resolved live so forward-declared types (e.g. generic ~list) aren't frozen as null.
        if (auto it = fullDestructorCache_.find(typeName); it != fullDestructorCache_.end())
            return it->second;

        auto dsIt = dataStructures.find(typeName);
        if (dsIt == dataStructures.end())
            return nullptr;

        // C++-style raw union semantics: the union has no hidden active-member tag, so the
        // compiler cannot safely synthesize member destruction. Only an explicitly written
        // union destructor runs; users that need managed alternatives must keep the tag and
        // lifetime policy in an enclosing wrapper.
        if (dsIt->second.IsUnion)
            return dsIt->second.Destructor;

        // Value-type member cycles are impossible (infinite size), but guard defensively
        // so a malformed registry cannot recurse forever - fall back to the user dtor.
        if (!fullDestructorInProgress_.insert(typeName).second)
            return dsIt->second.Destructor;

        llvm::Function* userDtor = dsIt->second.Destructor;

        // Collect member fields that need destruction.
        struct MemberWork { unsigned Index; llvm::Function* Dtor; bool IsUniquePtr; std::string TypeName; uint64_t AllocAlign; bool IsUniqueArray = false; bool IsIface = false; bool IsUniqueIface = false; };
        std::vector<MemberWork> work;
        for (unsigned i = 0; i < dsIt->second.StructFields.size(); ++i)
        {
            const auto& f = dsIt->second.StructFields[i];
            // An alias field is a borrowed reference to storage owned elsewhere. It must
            // never participate in the containing value's synthesized destruction.
            if (f.IsAlias)
                continue;
            // A remaining builtin `unique T* field` owns the pointee. Aligned fields stay on this
            // path so their allocation alignment reaches the matching deallocator.
            if (f.IsUnique
                && f.Pointer && !f.ElemPointer && !f.IsArrayView && !f.IsSimd
                && !f.IsBitfield && !f.IsPadding && f.ConstArraySize == 0)
            {
                work.push_back({ i, GetFullDestructorForDelete(f.TypeName), true, f.TypeName, f.AllocAlignValue });
                continue;
            }
            // A remaining builtin scalar `unique IFace field` is a {i8*,i8*} fat pointer, so
            // f.Pointer is false above. Route it to the fat-pointer-aware emitter (vtable dtor
            // slot + operator delete).
            if (f.IsUnique
                && f.IsFatInterfaceValue()
                && !f.ElemPointer && !f.IsSimd && !f.IsBitfield && !f.IsPadding
                && f.ConstArraySize == 0)
            {
                work.push_back({ i, nullptr, false, f.TypeName, 0, false, true, true });
                continue;
            }
            // A remaining builtin aligned `unique T* f[N]` / `unique IFace f[N]` owns every slot,
            // so release element by element. Null slots are skipped by the scalar emitters.
            if (f.IsUnique
                && f.ConstArraySize > 0
                && !f.ElemPointer && !f.IsArrayView && !f.IsSimd && !f.IsBitfield && !f.IsPadding
                && f.ConstInnerDimensions.empty()
                && (f.Pointer || f.IsFatInterfaceValue()))
            {
                bool iface = f.IsFatInterfaceValue();
                work.push_back({ i, nullptr, false, f.TypeName, f.AllocAlignValue, true, iface });
                continue;
            }
            if (f.Pointer || f.ElemPointer || f.IsArrayView || f.IsSimd || f.IsBitfield || f.IsPadding)
                continue;
            // Owning fixed-array value field (`SBox items[3]`): the FULLY-LIVE contract requires
            // every element be a live owned value, so destruct all N (EmitFullDestructorOverStorage
            // below walks them). Pointer/view/simd/bitfield arrays never reach here - the field-shape
            // skip above already caught them (e.g. btree_node's `children[17]` is a pointer array).
            if (f.ConstArraySize > 0)
            {
                if (llvm::Function* elemDtor = GetOrCreateFullDestructor(f.TypeName))
                    work.push_back({ i, elemDtor, false, f.TypeName, 0 });
                continue;
            }
            // `string` members ARE destructed: the owned bit in _len tells the dtor whether to free.
            // A borrowed string (literal/view; bit clear) is safely left alone by the dtor.
            if (llvm::Function* childDtor = GetOrCreateFullDestructor(f.TypeName))
                work.push_back({ i, childDtor, false, f.TypeName, 0 });
        }

        fullDestructorInProgress_.erase(typeName);

        if (work.empty())
        {
            // No member work: full destruction is exactly the user dtor (possibly null).
            // Resolved live (not cached) so a later-registered dtor is picked up.
            return userDtor;
        }

        // Synthesize a wrapper: user dtor first (so hand-written free-and-null logic runs
        // before member teardown), then each member's full destructor.
        auto* structTy = dsIt->second.StructType;
        auto* voidTy   = llvm::Type::getVoidTy(*context);
        auto* selfPtrTy = cflat_llvm::PointerTo(structTy);
        auto* fnTy = llvm::FunctionType::get(voidTy, { selfPtrTy }, false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                          typeName + ".dtorfull", *module);
        fn->arg_begin()->setName("self");
        fullDestructorCache_[typeName] = fn;   // memoize before body emission

        auto* entry = llvm::BasicBlock::Create(*context, "entry", fn);
        llvm::IRBuilder<> b(entry);
        auto* self = &*fn->arg_begin();

        if (userDtor)
            b.CreateCall(userDtor->getFunctionType(), userDtor, { self });

        for (const auto& w : work)
        {
            auto* fieldPtr = b.CreateStructGEP(structTy, self, w.Index, "fld");
            auto* fieldTy  = structTy->getElementType(w.Index);
            if (w.IsUniqueIface)
                EmitUniqueInterfaceFieldRelease(b, fieldPtr, w.TypeName);
            else if (w.IsUniqueArray)
                EmitUniqueArrayFieldRelease(b, fieldPtr, fieldTy,
                                            w.TypeName, w.IsIface, w.AllocAlign);
            else if (w.IsUniquePtr)
                EmitUniqueFieldDelete(b, fieldPtr, w.Dtor, w.TypeName, w.AllocAlign);
            else
                // Scalar field: one call; owning fixed-array field: one call per element.
                EmitFullDestructorOverStorage(b, fieldPtr, fieldTy, w.Dtor);
        }
        b.CreateRetVoid();
        return fn;
    }

llvm::Function* LLVMBackend::GetFullDestructorForDelete(const std::string& typeName)
{
        if (llvm::Function* eager = GetOrCreateFullDestructor(typeName))
            return eager;
        // Eager resolve failed: trivially-destructible OR not-yet-complete. Only data
        // structures can need a (possibly recursive) destructor; anything else is null.
        if (dataStructures.find(typeName) == dataStructures.end())
            return nullptr;
        if (auto it = deferredFullDtor_.find(typeName); it != deferredFullDtor_.end())
            return it->second;
        auto* voidTy   = llvm::Type::getVoidTy(*context);
        auto* selfPtrTy = llvm::PointerType::get(*context, 0);
        auto* fnTy = llvm::FunctionType::get(voidTy, { selfPtrTy }, false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                          typeName + ".dtordeferred", *module);
        fn->arg_begin()->setName("self");
        deferredFullDtor_[typeName] = fn;
        deferredFullDtorOrder_.push_back(typeName);
        return fn;
    }

void LLVMBackend::EmitDeferredFullDestructorBodies()
{
        for (const std::string& typeName : deferredFullDtorOrder_)
        {
            llvm::Function* fn = deferredFullDtor_[typeName];
            if (!fn->empty())
                continue;
            auto* entry = llvm::BasicBlock::Create(*context, "entry", fn);
            llvm::IRBuilder<> b(entry);
            auto* self = &*fn->arg_begin();
            if (llvm::Function* real = GetOrCreateFullDestructor(typeName))
                b.CreateCall(real->getFunctionType(), real, { self });
            b.CreateRetVoid();
        }
    }

bool LLVMBackend::HasUserCopyMethod(const std::string& typeName) const
{
        auto it = functionTable.find("copy");
        if (it == functionTable.end())
            return false;
        for (const auto& sym : it->second)
            if (sym.IsMethod && sym.Parameters.size() == 1
                && sym.Parameters[0].TypeName == typeName)
                return true;
        return false;
    }

bool LLVMBackend::TypeNeedsManagedCopy(const std::string& typeName)
{
        if (typeName.empty() || typeName == "string")
            return false;
        if (dataStructures.find(typeName) == dataStructures.end())
            return false;
        if (!HasUserCopyMethod(typeName))
            return false;
        return GetOrCreateFullDestructor(typeName) != nullptr;
    }

void LLVMBackend::QueueAliasReturnInference(const std::string& functionName, const std::string& returnTypeName,
                                   const std::vector<TypeAndValue>& params)
{
        PendingAliasReturn pending;
        pending.FunctionName = functionName;
        pending.ReturnTypeName = returnTypeName;
        for (const auto& p : params)
            pending.ParamTypeNames.push_back(p.TypeName + (p.Pointer ? "*" : ""));
        pendingAliasReturnInference.push_back(std::move(pending));
    }

void LLVMBackend::ResolvePendingAliasReturnInference()
{
        if (pendingAliasReturnInference.empty()) return;
        auto pending = std::move(pendingAliasReturnInference);
        pendingAliasReturnInference.clear();
        for (const auto& p : pending)
        {
            // The forward-ref scan registers an EMPTY struct shell; fields arrive during the
            // codegen walk. Until then the ownership gate cannot be evaluated - stay queued.
            auto structIt = dataStructures.find(p.ReturnTypeName);
            if (structIt == dataStructures.end() || structIt->second.StructFields.empty())
            {
                pendingAliasReturnInference.push_back(p);
                continue;
            }
            if (!TypeOwnsUniquePointer(p.ReturnTypeName)) continue;
            auto it = functionTable.find(p.FunctionName);
            if (it == functionTable.end()) continue;
            for (auto& sym : it->second)
            {
                if (sym.ReturnType.TypeName != p.ReturnTypeName) continue;
                if (sym.ReturnType.Pointer || sym.ReturnType.IsMove) continue;
                if (sym.Parameters.size() != p.ParamTypeNames.size()) continue;
                bool same = true;
                for (size_t i = 0; i < sym.Parameters.size(); ++i)
                    if (sym.Parameters[i].TypeName + (sym.Parameters[i].Pointer ? "*" : "") != p.ParamTypeNames[i])
                    { same = false; break; }
                if (!same) continue;
                sym.ReturnsAlias = true;
                sym.ReturnType.IsAlias = true;
                // The forward-ref pass may have emitted this declaration before the
                // struct fields were known well enough to infer the borrow return. Keep
                // its LLVM ABI in sync before the main pass attaches the body.
                if (sym.Function != nullptr && !FunctionHasDefinition(sym.Function) && sym.Function->use_empty())
                {
                    auto* inferredType = GetFunctionType(sym.ReturnType, sym.Parameters,
                                                         sym.Variadic, sym.External);
                    if (inferredType != sym.Function->getFunctionType())
                    {
                        auto* oldFunction = sym.Function;
                        std::string oldName = oldFunction->getName().str();
                        oldFunction->setName(oldName + ".alias_old");
                        auto* newFunction = llvm::Function::Create(
                            inferredType, oldFunction->getLinkage(), oldName, *module);
                        newFunction->setCallingConv(oldFunction->getCallingConv());
                        newFunction->setAttributes(oldFunction->getAttributes());
                        newFunction->addFnAttr(llvm::Attribute::NullPointerIsValid);
                        oldFunction->eraseFromParent();
                        sym.Function = newFunction;
                    }
                }
            }
        }
    }

bool LLVMBackend::IsOwningValueOrClosureType(const std::string& typeName)
{
        return typeName == "string" || IsOwningValueType(typeName)
            || typeName == "__closure_fat_ptr" || IsEncodedClosureType(typeName);
    }

int LLVMBackend::JoinArmStringLiteralKind(const llvm::Value* value, int depth) const
{
    if (value == nullptr) return -1;
    if (auto* c = llvm::dyn_cast<llvm::Constant>(value))
    {
        // A null arm carries no data, so it neither proves nor blocks - the same neutral
        // reading JoinArmDataKind gives it. `default` on a pointer lowers to this null.
        if (c->isNullValue()) return 0;
        if (IsStringLiteralConstant(const_cast<llvm::Constant*>(c))) return 1;
    }
    return JoinIsAllStringLiterals(value, depth) ? 1 : -1;
}

bool LLVMBackend::JoinIsAllStringLiterals(const llvm::Value* value, int depth) const
{
    if (value == nullptr || depth > kMaxJoinArmDepth) return false;
    // A '?:' joins through a PHI whose incoming values ARE the arms; a '??' joins through a
    // slot, so its arms survive only in the nullCoalesceJoins_ ledger.
    if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(value))
    {
        if (phi->getNumIncomingValues() == 0) return false;
        for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
        {
            const int kind = JoinArmStringLiteralKind(phi->getIncomingValue(i), depth + 1);
            if (kind > 0) return true;
        }
        return false;
    }
    if (const NullCoalesceJoin* join = FindNullCoalesceJoin(value))
    {
        if (join->Arms.empty()) return false;
        for (const auto& arm : join->Arms)
        {
            const int kind = JoinArmStringLiteralKind(arm.Value, depth + 1);
            if (kind > 0) return true;
        }
        return false;
    }
    return false;
}

bool LLVMBackend::IsStringLiteralIntoStructPointer(const TypeAndValue& destTV, llvm::Value* right)
{
    if (right == nullptr || !destTV.Pointer) return false;
    // Only a SINGLE star over a plain struct slot is provably wrong: a 'char**' element, a
    // function pointer, an interface fat slot and an array view all read the value differently.
    if (destTV.IsFunctionPointer || destTV.IsInterface || destTV.IsArrayView) return false;
    if (destTV.PointerDepth > 1 || destTV.ElemPointer || destTV.ConstArraySize > 0) return false;
    auto* c = llvm::dyn_cast<llvm::Constant>(right);
    const bool literal = (c != nullptr && IsStringLiteralConstant(c))
        || JoinIsAllStringLiterals(right);
    if (!literal) return false;
    return GetDataStructure(destTV.TypeName).StructType != nullptr;
}

std::string LLVMBackend::DescribeStringLiteralIntoStructPointer(const TypeAndValue& destTV,
                                                   const std::string& destDesc) const
{
    return std::format(
        "cannot store a string literal into {} of type '{}{}' - a string literal is a "
        "'const char*', not a pointer to a '{}', so reading a member through it would interpret "
        "the characters as a '{}'. Bind the text to a '{}' value and store its address, or "
        "declare the slot 'const char*'. A cast on the literal itself is still rejected (the "
        "underlying pointer is unchanged); route it through a 'const char*' local first, e.g. "
        "'const char* r = \"...\"; {}* q = ({}*)r;', to assert the reinterpret explicitly.",
        destDesc, destTV.TypeName, destTV.IsNullable ? "?" : "*",
        destTV.TypeName, destTV.TypeName, destTV.TypeName, destTV.TypeName, destTV.TypeName);
}

bool LLVMBackend::HasNonTrivialDestructor(const std::string& typeName)
{
        if (typeName.empty())
            return false;
        return GetOrCreateFullDestructor(typeName) != nullptr;
    }

bool LLVMBackend::IsOwningValueType(const std::string& typeName)
{
        return HasTypeAnnotation(typeName, "unique") || HasNonTrivialDestructor(typeName);
    }

bool LLVMBackend::TypeOwnsUniquePointer(const std::string& typeName, std::string* outPath,
                               std::unordered_set<std::string>* seen) const
{
        if (HasTypeAnnotation(typeName, "unique"))
        {
            if (outPath) outPath->clear();
            return true;
        }
        std::unordered_set<std::string> localSeen;
        if (seen == nullptr) seen = &localSeen;
        if (!seen->insert(typeName).second) return false;
        auto it = dataStructures.find(typeName);
        if (it == dataStructures.end()) return false;
        for (const auto& f : it->second.StructFields)
        {
            // A unique interface field is a fat owning value rather than a thin pointer, but it
            // has the same non-copyable ownership contract and must block memberwise bit copies.
            bool ownsCoreUniqueField = IsCoreUniqueType(f.TypeName)
                && !f.Pointer && !f.ElemPointer
                && !f.IsArrayView && !f.IsSimd && !f.IsBitfield;
            bool ownsUniqueField = ownsCoreUniqueField
                || (f.IsUnique
                    && !f.IsAlias
                && !f.IsArrayView && !f.IsSimd && !f.IsBitfield
                && ((f.Pointer && !f.ElemPointer && f.ConstArraySize == 0)
                    || (f.IsInterface && !f.Pointer && f.ConstArraySize == 0)));
            if (ownsUniqueField)
            {
                if (outPath) *outPath = f.VariableName;
                return true;
            }
            // Only by-value struct members can carry the claim onward; a pointer member is a
            // borrow the copy shallow-shares by design.
            if (f.Pointer || f.ElemPointer || f.IsArrayView || f.IsSimd || f.IsBitfield || f.IsPadding)
                continue;
            std::string sub;
            if (TypeOwnsUniquePointer(f.TypeName, &sub, seen))
            {
                if (outPath) *outPath = f.VariableName + "." + sub;
                return true;
            }
        }
        return false;
    }

bool LLVMBackend::HasCopyOverloadFor(const std::string& typeName) const
{
        auto it = functionTable.find("copy");
        if (it == functionTable.end()) return false;
        for (const auto& sym : it->second)
            if (!sym.Parameters.empty() && sym.Parameters[0].TypeName == typeName)
                return true;
        return false;
    }

bool LLVMBackend::IsCopyableType(const std::string& typeNameIn) const
{
        std::string base = typeNameIn;
        if (base.rfind("alias ", 0) == 0) base = base.substr(6);
        if (base.empty()) return false;
        if (base.back() == '*') return true;
        if (dataStructures.count(base) == 0) return true;
        // A raw pointer/view plus a non-trivial destructor needs an author-defined copy policy.
        // Safe destructor-backed value types may still use the memberwise synth.
        if (!HasRealCopyOverloadFor(base) && StructSynthCopyUnsafe(base))
            return false;
        if (HasCopyOverloadFor(base)) return true;
        return !TypeOwnsUniquePointer(base);
    }

bool LLVMBackend::OwningSinkConsumesConcrete(const TypeAndValue& p)
{
        if (!p.IsOwningSink) return false;
        if (p.IsConsumeInferredSink) return !IsCopyableType(p.TypeName);
        return true;
    }

bool LLVMBackend::FuncPtrParamMoveAgrees(const TypeAndValue::FuncPtrParam& dest,
        const TypeAndValue::FuncPtrParam& src)
{
        if (dest.AllocAlignValue != 0 && src.AllocAlignValue != 0
            && dest.AllocAlignValue != src.AllocAlignValue)
            return false;
        if (dest.IsMove == src.IsMove) return true;
        // A consuming literal has no `move` token but does consume, so it satisfies a declared
        // sink. The reverse (source spells `move`, destination does not) still disagrees.
        if (!dest.IsMove || !src.IsOwningSink) return false;
        // ...but only when the sink CONSUMES for this concrete type. A consume-inferred sink of a
        // COPYABLE owner stores a copy, so the caller's declared-move transfer would poison a
        // source nobody took and leak it - that stays the pre-existing rejection.
        return OwningSinkConsumesConcrete(FuncPtrParamAsTypeAndValue(src, 0));
    }

int LLVMBackend::FindLostClosureSinkParam(const TypeAndValue& dest, const TypeAndValue& src)
{
        // The SIGNATURE is the proof, not IsFunctionPointer: a closure ARGUMENT reaches the
        // overload scorer with FuncPtrParams copied but IsFunctionPointer deliberately left alone.
        if (!dest.IsFunctionPointer || src.FuncPtrParams.empty()) return -1;
        if (dest.FuncPtrParams.size() != src.FuncPtrParams.size()) return -1;
        for (size_t i = 0; i < src.FuncPtrParams.size(); i++)
        {
            const auto& d = dest.FuncPtrParams[i];
            if (d.IsMove || d.IsOwningSink) continue;
            TypeAndValue s = FuncPtrParamAsTypeAndValue(src.FuncPtrParams[i], i);
            if (!OwningSinkConsumesConcrete(s)) continue;
            if (!IsOwningValueOrClosureType(s.TypeName)) continue;
            return (int)i;
        }
        return -1;
    }

std::string LLVMBackend::DescribeLostClosureSink(const TypeAndValue& dest, size_t index,
        const std::string& destDescription)
{
        // Rebuild the destination's own spelling with `move` added at the offending parameter.
        std::string family = dest.IsThinFnPtr() ? "function" : "Lambda";
        std::string spelled = family + "<" + dest.FuncPtrReturnTypeName
            + std::string(dest.FuncPtrReturnPointer ? "*" : "") + "(";
        for (size_t i = 0; i < dest.FuncPtrParams.size(); i++)
        {
            if (i > 0) spelled += ", ";
            if (i == index || dest.FuncPtrParams[i].IsMove) spelled += "move ";
            spelled += dest.FuncPtrParams[i].TypeName;
            if (dest.FuncPtrParams[i].Pointer) spelled += "*";
        }
        spelled += ")>";
        return std::format(
            "this closure CONSUMES parameter {} ('{}'), but {} does not spell it - ownership cannot "
            "travel with a closure value, so the caller would free it again. Spell the sink in the "
            "type: '{}'",
            index + 1, dest.FuncPtrParams[index].TypeName, destDescription, spelled);
    }

bool LLVMBackend::HasRealCopyOverloadFor(const std::string& typeName) const
{
        auto it = functionTable.find("copy");
        if (it == functionTable.end()) return false;
        const std::string synthName = typeName + ".copy.synth";
        for (const auto& sym : it->second)
            if (!sym.Parameters.empty() && sym.Parameters[0].TypeName == typeName
                && sym.UniqueName != synthName)
                return true;
        return false;
    }

bool LLVMBackend::ClosureCaptureDeepCopyable(const std::string& typeName)
{
        if (!IsOwningValueType(typeName)) return false;
        if (HasRealCopyOverloadFor(typeName)) return true;
        auto ds = dataStructures.find(typeName);
        if (ds == dataStructures.end()) return false;
        for (const auto& f : ds->second.StructFields)
            if (f.Pointer || f.ElemPointer || f.IsArrayView)
                return false;
        return true;
    }

bool LLVMBackend::StructSynthCopyUnsafe(const std::string& typeName) const
{
        if (typeName == "string" || typeName == "__closure_fat_ptr") return false;
        auto it = dataStructures.find(typeName);
        if (it == dataStructures.end() || it->second.Destructor == nullptr) return false;
        for (const auto& f : it->second.StructFields)
            if (f.Pointer || f.ElemPointer || f.IsArrayView)
                return true;
        return false;
    }

bool LLVMBackend::HasArrowOverloadFor(const std::string& typeName) const
{
        auto it = functionTable.find("operator->");
        if (it == functionTable.end()) return false;
        for (const auto& sym : it->second)
            if (!sym.Parameters.empty() && sym.Parameters[0].TypeName == typeName)
                return true;
        return false;
    }

bool LLVMBackend::MemberIsScalarField(const std::string& typeName, const std::string& memberName) const
{
        auto ds = dataStructures.find(typeName);
        if (ds == dataStructures.end()) return false;
        for (const auto& f : ds->second.StructFields)
        {
            if (f.VariableName != memberName) continue;
            return !f.Pointer && !f.IsInterface && !f.IsFunctionPointer
                && f.ConstArraySize == 0 && f.TypeName != "string"
                && dataStructures.find(f.TypeName) == dataStructures.end();
        }
        return false;
    }

bool LLVMBackend::TypeHasMember(const std::string& typeName, const std::string& memberName) const
{
        if (auto ds = dataStructures.find(typeName); ds != dataStructures.end())
        {
            for (const auto& f : ds->second.StructFields)
                if (f.VariableName == memberName) return true;
            for (const auto& b : ds->second.Bitfields)
                if (b.Name == memberName) return true;
        }
        if (auto fn = functionTable.find(memberName); fn != functionTable.end())
            for (const auto& sym : fn->second)
                if (!sym.Parameters.empty() && sym.Parameters[0].TypeName == typeName)
                    return true;
        return GetWinrtSlot(typeName, memberName) != nullptr;
    }

llvm::Function* LLVMBackend::GetOrCreateMemberwiseCopy(const std::string& typeName)
{
        if (auto it = memberwiseCopyCache_.find(typeName); it != memberwiseCopyCache_.end())
            return it->second;
        auto dsIt = dataStructures.find(typeName);
        if (dsIt == dataStructures.end()) return nullptr;
        auto* structTy = dsIt->second.StructType;
        if (structTy == nullptr) return nullptr;

        // Signature: T copy(T self) - self by value (a borrow; not destructed), result by value.
        auto* fnTy = llvm::FunctionType::get(structTy, { structTy }, false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                          typeName + ".copy.synth", *module);
        fn->arg_begin()->setName("self");
        memberwiseCopyCache_[typeName] = fn;   // memoize before body (re-entrancy / recursion safe)

        // Register so overload resolution (user `x.copy()` + internal copy calls) finds it.
        {
            FunctionSymbol sym;
            sym.UniqueName = typeName + ".copy.synth";
            sym.Function   = fn;
            sym.ReturnType = TypeAndValue{ typeName, "", false };
            sym.ReturnType.IsMove = true;       // the fresh copy is owned by the caller
            sym.Parameters = { TypeAndValue{ typeName, "self", false } };
            functionTable["copy"].push_back(sym);
        }

        auto savedIP = builder->saveIP();
        auto* entry = llvm::BasicBlock::Create(*context, "entry", fn);
        builder->SetInsertPoint(entry);

        // result = self  (shallow copy of every field; managed fields are fixed up below)
        auto* resultSlot = builder->CreateAlloca(structTy, nullptr, "result");
        builder->CreateStore(&*fn->arg_begin(), resultSlot);

        // Deep-copy each managed value field, overwriting the aliased shallow handle.
        for (unsigned i = 0; i < dsIt->second.StructFields.size(); ++i)
        {
            const auto& f = dsIt->second.StructFields[i];
            if (f.Pointer || f.ElemPointer || f.IsArrayView || f.IsSimd || f.IsBitfield || f.IsPadding)
                continue;                       // pointer/view/simd/bitfield/pad: shallow (pointee shared)
            if (f.ConstArraySize > 0)
            {
                // Owning fixed-array value field: deep-copy every element so the copy is
                // independent (FULLY-LIVE contract, in lockstep with the destructor).
                if (!HasCopyOverloadFor(f.TypeName) && !IsOwningValueType(f.TypeName))
                    continue;                   // POD element array: the shallow copy is correct
                llvm::Type* elemTy = nullptr;
                uint64_t n = PeelFixedArrayType(structTy->getElementType(i), elemTy);
                auto* base = builder->CreateStructGEP(structTy, resultSlot, i, "fldarr");
                EmitFixedArrayElementWalk(*builder, base, elemTy, n, [&](llvm::Value* elemPtr) {
                    NamedVariable elemNV;
                    elemNV.Storage  = elemPtr;
                    elemNV.BaseType = elemTy;
                    elemNV.TypeAndValue.TypeName = f.TypeName;
                    if (auto* copied = CreateOverloadedFunctionCall("copy", { elemNV }))
                        builder->CreateStore(copied, elemPtr);
                });
                continue;
            }
            if (!HasCopyOverloadFor(f.TypeName) && !IsOwningValueType(f.TypeName))
                continue;                       // POD field: the shallow copy is already correct
            auto* fieldPtr = builder->CreateStructGEP(structTy, resultSlot, i, "fld");
            NamedVariable argNV;
            argNV.Storage  = fieldPtr;
            argNV.BaseType = structTy->getElementType(i);
            argNV.TypeAndValue.TypeName = f.TypeName;
            if (auto* copied = CreateOverloadedFunctionCall("copy", { argNV }))
                builder->CreateStore(copied, fieldPtr);
        }

        auto* resultVal = builder->CreateLoad(structTy, resultSlot, "copyresult");
        builder->CreateRet(resultVal);
        builder->restoreIP(savedIP);
        return fn;
    }

llvm::Function* LLVMBackend::GenerateClosureCaptureCleanup(const std::string& name, llvm::StructType* capTy,
        const std::vector<std::pair<unsigned, std::string>>& owningFields)
{
        if (owningFields.empty()) return nullptr;
        auto* i8PtrTy = cflat_llvm::PointerTo(builder->getInt8Ty());
        auto* i32Ty   = builder->getInt32Ty();
        auto* voidTy  = llvm::Type::getVoidTy(*context);
        auto* fnTy = llvm::FunctionType::get(voidTy, { i8PtrTy, i8PtrTy, i32Ty }, false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, name, module.get());

        auto savedIP  = builder->saveIP();
        auto* entry   = llvm::BasicBlock::Create(*context, "entry", fn);
        auto* cloneBB = llvm::BasicBlock::Create(*context, "clone", fn);
        auto* freeBB  = llvm::BasicBlock::Create(*context, "free", fn);
        builder->SetInsertPoint(entry);
        auto* dstCaps = builder->CreateBitCast(fn->getArg(0), cflat_llvm::PointerTo(capTy), "dstcaps");
        auto* isClone = builder->CreateICmpEQ(fn->getArg(2), builder->getInt32(1));
        builder->CreateCondBr(isClone, cloneBB, freeBB);

        // CLONE: dst currently aliases src's owning handles (byte copy); replace each with a
        // deep copy so dst owns independent buffers.
        builder->SetInsertPoint(cloneBB);
        auto* srcCaps = builder->CreateBitCast(fn->getArg(1), cflat_llvm::PointerTo(capTy), "srccaps");
        for (const auto& [idx, tn] : owningFields)
        {
            NamedVariable argNV;
            argNV.Storage  = builder->CreateStructGEP(capTy, srcCaps, idx);
            argNV.BaseType = capTy->getElementType(idx);
            argNV.TypeAndValue.TypeName = tn;
            if (auto* copied = CreateOverloadedFunctionCall("copy", { argNV }))
                builder->CreateStore(copied, builder->CreateStructGEP(capTy, dstCaps, idx));
        }
        builder->CreateRetVoid();

        // FREE: destruct dst's owning fields.
        builder->SetInsertPoint(freeBB);
        for (const auto& [idx, tn] : owningFields)
        {
            auto* fldPtr = builder->CreateStructGEP(capTy, dstCaps, idx);
            if (auto* dtor = GetOrCreateFullDestructor(tn))
                builder->CreateCall(dtor->getFunctionType(), dtor,
                    { builder->CreateBitCast(fldPtr, dtor->getArg(0)->getType()) });
        }
        builder->CreateRetVoid();
        builder->restoreIP(savedIP);
        return fn;
    }

void LLVMBackend::EnsureStrConcatRegistered()
{
        if (strConcatRegistered) return;
        strConcatRegistered = true;
        RegisterBuiltinStrConcat();
    }

llvm::StructType* LLVMBackend::VaListTagType()
{
        auto* i32Ty = llvm::Type::getInt32Ty(*context);
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        return llvm::StructType::get(*context, { i32Ty, i32Ty, ptrTy, ptrTy });
    }

void LLVMBackend::CreateVaStart(llvm::Value* apAlloca)
{
        auto* fn = llvm::Intrinsic::getOrInsertDeclaration(module.get(), llvm::Intrinsic::vastart,
                                                                { apAlloca->getType() });
        // macOS arm64 (Darwin): va_list is a plain char* (Apple passes every variadic
        // argument on the stack), so the slot IS the va_list - init it directly, exactly
        // like the Windows path. NOT the x86-64 SysV __va_list_tag indirection below.
        if (!targetWindows_ && !targetMacOS_)
        {
            // x86-64 SysV: allocate the 24-byte tag, point the va_list slot at it, init the tag.
            auto* tag = AllocaAtEntry(VaListTagType(), nullptr, "va.tag", 16);
            builder->CreateStore(tag, apAlloca);
            builder->CreateCall(fn, {tag});
            return;
        }
        builder->CreateCall(fn, {apAlloca});
    }

void LLVMBackend::CreateVaEnd(llvm::Value* apAlloca)
{
        auto* fn = llvm::Intrinsic::getOrInsertDeclaration(module.get(), llvm::Intrinsic::vaend,
                                                                { apAlloca->getType() });
        if (!targetWindows_ && !targetMacOS_)
        {
            // x86-64 SysV: the slot holds the tag pointer (set by CreateVaStart); va_end takes the tag.
            auto* tag = builder->CreateLoad(llvm::PointerType::getUnqual(*context), apAlloca, "va.tag");
            builder->CreateCall(fn, {tag});
            return;
        }
        builder->CreateCall(fn, {apAlloca});
    }

llvm::Value* LLVMBackend::CreateFloatIntrinsic(const std::string& methodName, llvm::Value* floatVal)
{
        llvm::Intrinsic::ID id;
        if      (methodName == "round")     id = llvm::Intrinsic::round;
        else if (methodName == "floor")     id = llvm::Intrinsic::floor;
        else if (methodName == "ceil")      id = llvm::Intrinsic::ceil;
        else if (methodName == "trunc")     id = llvm::Intrinsic::trunc;
        else if (methodName == "abs")       id = llvm::Intrinsic::fabs;
        else if (methodName == "rint")      id = llvm::Intrinsic::rint;
        else if (methodName == "nearbyint") id = llvm::Intrinsic::nearbyint;
        else if (methodName == "sqrt")      id = llvm::Intrinsic::sqrt;
        else if (methodName == "sin")       id = llvm::Intrinsic::sin;
        else if (methodName == "cos")       id = llvm::Intrinsic::cos;
        else if (methodName == "exp")       id = llvm::Intrinsic::exp;
        else if (methodName == "exp2")      id = llvm::Intrinsic::exp2;
        else if (methodName == "log")       id = llvm::Intrinsic::log;
        else if (methodName == "log2")      id = llvm::Intrinsic::log2;
        else if (methodName == "log10")     id = llvm::Intrinsic::log10;
        else return nullptr;

        auto* fn = llvm::Intrinsic::getOrInsertDeclaration(module.get(), id, {floatVal->getType()});
        return builder->CreateCall(fn, {floatVal});
    }

llvm::Value* LLVMBackend::CreateRdtscp()
{
        // llvm.x86.rdtscp returns { i64 cycles, i32 aux } and takes no arguments.
        auto* fn   = llvm::Intrinsic::getOrInsertDeclaration(module.get(), llvm::Intrinsic::x86_rdtscp);
        auto* call = builder->CreateCall(fn, {});
        return builder->CreateExtractValue(call, { 0u }, "rdtscp");
    }

llvm::Value* LLVMBackend::CreateReadCycleCounter()
{
        // llvm.readcyclecounter is target-independent: it returns an i64 cycle
        // count and takes no arguments. Lowers to RDTSC on x86, the cycle-count
        // register elsewhere (e.g. mftb/CNTVCT), or 0 where unsupported.
        auto* fn = llvm::Intrinsic::getOrInsertDeclaration(module.get(), llvm::Intrinsic::readcyclecounter);
        return builder->CreateCall(fn, {}, "cyclecount");
    }

void LLVMBackend::CreateLfence()
{
        // llvm.x86.sse2.lfence returns void and takes no arguments.
        auto* fn = llvm::Intrinsic::getOrInsertDeclaration(module.get(), llvm::Intrinsic::x86_sse2_lfence);
        builder->CreateCall(fn, {});
    }

void LLVMBackend::CreateFenceAcquire()
{
        builder->CreateFence(llvm::AtomicOrdering::Acquire);
    }

void LLVMBackend::CreatePause()
{
        // llvm.x86.sse2.pause returns void and takes no arguments.
        auto* fn = llvm::Intrinsic::getOrInsertDeclaration(module.get(), llvm::Intrinsic::x86_sse2_pause);
        builder->CreateCall(fn, {});
    }

llvm::Value* LLVMBackend::CreatePopcount(llvm::Value* intVal)
{
        auto* fn = llvm::Intrinsic::getOrInsertDeclaration(module.get(), llvm::Intrinsic::ctpop, { intVal->getType() });
        return builder->CreateCall(fn, { intVal }, "popcount");
    }

llvm::Value* LLVMBackend::CreateCtz(llvm::Value* intVal)
{
        auto* fn = llvm::Intrinsic::getOrInsertDeclaration(module.get(), llvm::Intrinsic::cttz, { intVal->getType() });
        return builder->CreateCall(fn, { intVal, llvm::ConstantInt::getFalse(*context) }, "ctz");
    }

llvm::Value* LLVMBackend::CreateClz(llvm::Value* intVal)
{
        auto* fn = llvm::Intrinsic::getOrInsertDeclaration(module.get(), llvm::Intrinsic::ctlz, { intVal->getType() });
        return builder->CreateCall(fn, { intVal, llvm::ConstantInt::getFalse(*context) }, "clz");
    }

void LLVMBackend::CreatePrefetch(llvm::Value* addr)
{
        auto* i32ty = llvm::Type::getInt32Ty(*context);
        auto* fn = llvm::Intrinsic::getOrInsertDeclaration(module.get(), llvm::Intrinsic::prefetch, { addr->getType() });
        builder->CreateCall(fn, {
            addr,
            llvm::ConstantInt::get(i32ty, 0),   // rw: 0 = read
            llvm::ConstantInt::get(i32ty, 3),   // locality: 3 = high temporal
            llvm::ConstantInt::get(i32ty, 1) }); // cache type: 1 = data cache
    }

llvm::Value* LLVMBackend::CreateFma(llvm::Value* a, llvm::Value* b, llvm::Value* c)
{
        auto* fn = llvm::Intrinsic::getOrInsertDeclaration(module.get(), llvm::Intrinsic::fma, { a->getType() });
        return builder->CreateCall(fn, { a, b, c }, "fma");
    }

llvm::Value* LLVMBackend::CreateExpect(llvm::Value* cond, bool expected)
{
        auto* fn = llvm::Intrinsic::getOrInsertDeclaration(module.get(), llvm::Intrinsic::expect, { cond->getType() });
        return builder->CreateCall(fn, { cond, llvm::ConstantInt::get(cond->getType(), expected ? 1 : 0) }, "expect");
    }

llvm::Value* LLVMBackend::CreateIntegerConvert(const std::string& methodName, llvm::Value* intVal)
{
        struct Target { unsigned bits; bool isSigned; };
        static const std::unordered_map<std::string, Target> table = {
            {"to_i8",  {8,  true}},  {"to_u8",  {8,  false}},
            {"to_i16", {16, true}},  {"to_u16", {16, false}},
            {"to_i32", {32, true}},  {"to_u32", {32, false}},
            {"to_i64", {64, true}},  {"to_u64", {64, false}},
        };
        auto it = table.find(methodName);
        if (it == table.end()) return nullptr;

        unsigned srcBits  = intVal->getType()->getIntegerBitWidth();
        unsigned destBits = it->second.bits;
        auto* destTy = llvm::Type::getIntNTy(*context, destBits);

        if (srcBits > destBits)  return builder->CreateTrunc(intVal, destTy);
        if (srcBits < destBits)  return it->second.isSigned
                                     ? builder->CreateSExt(intVal, destTy)
                                     : builder->CreateZExt(intVal, destTy);
        return intVal; // same width - no-op
    }
