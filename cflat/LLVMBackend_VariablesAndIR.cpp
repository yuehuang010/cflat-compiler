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

// ---- Definitions moved out of LLVMBackend.h (VariablesAndIR) ----

llvm::GlobalVariable* LLVMBackend::CreateGlobalVariable(TypeAndValue typeValue, llvm::Constant* initValue, bool threadLocal, uint64_t userAlign, bool externalDecl)
{
        // A file-scope global never passes through CreateLocalVariable.
        if (!typeValue.Pointer)
            RecordInterfaceMaterialization(typeValue.TypeName, "the type of a global variable");
        llvm::Type* destinationType = GetType(typeValue);
        if (initValue)
        {
            if (auto intValue = llvm::dyn_cast<llvm::ConstantInt>(initValue))
            {
                // An FP global from an integer literal (`float g = 1;`) - legal for a LOCAL, and
                // the int path below asks a non-integer type for its bit width.
                if (destinationType->isFloatingPointTy())
                {
                    initValue = llvm::ConstantFP::get(destinationType,
                        (double)intValue->getSExtValue());
                }
                else if (destinationType->isIntegerTy()
                    && intValue->getIntegerType()->getBitWidth() != destinationType->getIntegerBitWidth())
                {
                    // Sign-extend, not zero-extend: a negative literal (e.g. the unary-minus
                    // fold that narrows -150 to i16) must widen via its signed value or the
                    // sign bit is lost and the constant reads back as a large positive number.
                    initValue = builder->getIntN(destinationType->getIntegerBitWidth(),
                        (uint64_t)intValue->getSExtValue());
                }
            }
            else if (auto fpValue = llvm::dyn_cast<llvm::ConstantFP>(initValue))
            {
                // Widen a narrower FP constant to the global's type (float -> double is implicit).
                // Narrowing is left alone so it still errors at use.
                if (destinationType->isFloatingPointTy() &&
                    fpValue->getType()->getScalarSizeInBits() < destinationType->getScalarSizeInBits())
                {
                    llvm::APFloat widened = fpValue->getValueAPF();
                    bool losesInfo = false;
                    widened.convert(destinationType->getFltSemantics(),
                        llvm::APFloat::rmNearestTiesToEven, &losesInfo);
                    initValue = llvm::ConstantFP::get(destinationType->getContext(), widened);
                }
            }
            // Coerce a null-value (e.g. nullptr) to the destination type when types differ.
            // Handles fat-ptr structs and function pointers initialized with = nullptr.
            else if (initValue->isNullValue() && initValue->getType() != destinationType)
            {
                initValue = llvm::Constant::getNullValue(destinationType);
            }
        }
        else if (!externalDecl)
        {
            // Zero-initialize - works for all types: primitives, pointers, structs, fat-ptrs.
            initValue = llvm::Constant::getNullValue(destinationType);
        }
        // externalDecl with no initValue: leave initValue null so the GlobalVariable below is a
        // declaration (external reference), not a definition.

        // A global's initializer is a Constant, so nothing casts it: a single non-aggregate value
        // on aggregate/vector storage fails module verification with no source location.
        bool destIsWideStorage = destinationType != nullptr
            && (destinationType->isAggregateType() || destinationType->isVectorTy());
        bool srcIsSingleValue = initValue != nullptr
            && !initValue->getType()->isAggregateType() && !initValue->getType()->isVectorTy();
        if (destIsWideStorage && srcIsSingleValue && initValue->getType() != destinationType)
        {
            if (initValue->getType()->isPointerTy())
                LogError(std::format(
                    "cannot initialize global {} '{}' from a pointer value or a string literal - "
                    "CFlat has no C-style character-array initializer. Use a brace list "
                    "('{{'a','b',...}}'), '= default' to zero it, or declare '{}' as a pointer or a "
                    "'string' to point at the literal.",
                    DescribeAggregateStorageShape(destinationType, typeValue.TypeName),
                    typeValue.VariableName, typeValue.VariableName));
            std::string declText = typeValue.IsSimd
                ? std::format("simd<{},{}>", typeValue.TypeName, typeValue.SimdLanes)
                : typeValue.TypeName;
            LogError(std::format(
                "cannot initialize global '{} {}' from a single scalar value - it names {}, which "
                "is not assignable from one value. Use a brace list ('{{...}}') to fill it, or "
                "'= default' to zero it.",
                declText, typeValue.VariableName,
                destinationType->isVectorTy()
                    ? std::format("simd vector storage with {} lanes",
                                  llvm::cast<llvm::FixedVectorType>(destinationType)->getNumElements())
                    : DescribeAggregateStorageShape(destinationType, typeValue.TypeName)));
        }

        // Extern globals link to the bare C symbol: a namespaced declaration like
        // os.posix's `stdout` must resolve to libc `stdout`, not a symbol literally
        // named "os.posix.stdout". This mirrors extern functions (whose linkage name
        // is the un-namespaced name). The full namespaced name stays the cflat lookup
        // key below. Non-namespaced externs (no '.') and definitions are unaffected.
        std::string symbolName = typeValue.VariableName;
        if (externalDecl)
        {
            auto dot = symbolName.find_last_of('.');
            if (dot != std::string::npos) symbolName = symbolName.substr(dot + 1);
        }

        auto gVar = new llvm::GlobalVariable(
            *module,
            destinationType,
            false, // isConstant
            llvm::GlobalValue::LinkageTypes::ExternalLinkage,
            initValue, // Initial value
            symbolName // Name (bare C symbol for externs)
        );

        if (threadLocal)
            gVar->setThreadLocalMode(llvm::GlobalVariable::GeneralDynamicTLSModel);

        // Apply effective alignment (decl-level alignas, struct alignas, or ABI).
        uint64_t effAlign = GetEffectiveAlignmentForType(typeValue.TypeName, destinationType);
        if (userAlign > effAlign) effAlign = userAlign;
        // Floor thread-locals at 16: the store vectorizer pairs adjacent 8-byte field stores into
        // a 16-byte aligned store (vmovaps), and emutls (--run) allocates each TLS block on the
        // global's declared alignment. Without an explicit 16 here the control records 8 and the
        // aligned store faults. Native .tls over-aligns the section to 16, so this only bites --run.
        if (threadLocal && effAlign < 16) effAlign = 16;
        uint64_t abiAlign = (destinationType && destinationType->isSized())
            ? module->getDataLayout().getABITypeAlign(destinationType).value() : 0;
        if (effAlign > abiAlign)
            gVar->setAlignment(llvm::Align(effAlign));

        // File-scope lock group: stamp the guardian onto the global so identifier
        // resolution can enforce the lock-set on every access.
        if (!pendingGlobalGuardedBy.empty())
            typeValue.GuardedBy = pendingGlobalGuardedBy;

        globalNamedVariable[typeValue.VariableName] = gVar;
        globalVariableTypes[typeValue.VariableName] = typeValue;

        // Record definition order for end-of-main destruction (see EmitGlobalDestructorsInMain).
        // Externs (not ours to free), thread-locals (main destroys only its own copy) and
        // core-library globals (process-lifetime infrastructure) are excluded.
        if (!externalDecl && !threadLocal && !currentSourceIsCore_ && !typeValue.VariableName.empty()
            && std::find(globalDtorOrder_.begin(), globalDtorOrder_.end(), typeValue.VariableName)
               == globalDtorOrder_.end())
            globalDtorOrder_.push_back(typeValue.VariableName);

        if (symbolSink_ && !typeValue.VariableName.empty())
            symbolSink_->RegisterVariable(typeValue.VariableName, typeValue.TypeName);

        if (diBuilder && diFile && !typeValue.VariableName.empty() && !externalDecl)
        {
            llvm::DIFile* gvFile = GetDIFileForCurrentSource();
            if (!gvFile) gvFile = diFile;
            pendingGlobalDI_.push_back({gVar, typeValue, gvFile, (unsigned)currentLine,
                compileUnit, false});
        }

        return gVar;
    }

llvm::AllocaInst* LLVMBackend::AllocaAtEntry(llvm::Type* type, llvm::Value* arraySize, const llvm::Twine& name, uint64_t align)
{
        llvm::AllocaInst* a;
        if (arraySize != nullptr)
        {
            a = builder->CreateAlloca(type, arraySize, name);
        }
        else
        {
            auto* currentBlock = builder->GetInsertBlock();
            auto* entryBlock = &currentBlock->getParent()->getEntryBlock();
            if (currentBlock == entryBlock)
            {
                a = builder->CreateAlloca(type, nullptr, name);
            }
            else
            {
                llvm::IRBuilder<> eb(entryBlock, entryBlock->begin());
                a = eb.CreateAlloca(type, nullptr, name);
            }
        }
        if (align != 0)
            a->setAlignment(llvm::Align(align));
        return a;
    }

void LLVMBackend::CreateOwnOriginSlot(llvm::Value* storage)
{
        if (!sanitizeOwnership_ || storage == nullptr) return;
        if (ownOriginSlots_.count(storage)) return;
        if (auto* global = llvm::dyn_cast<llvm::GlobalVariable>(storage))
        {
            auto* origin = new llvm::GlobalVariable(
                *module, builder->getInt64Ty(), false, llvm::GlobalValue::InternalLinkage,
                builder->getInt64(0), global->getName() + ".own_origin");
            origin->setAlignment(llvm::Align(8));
            ownOriginSlots_[storage] = origin;
            return;
        }
        auto* slot = AllocaAtEntry(builder->getInt64Ty(), nullptr, "own_origin");
        builder->CreateStore(builder->getInt64(0), slot);
        ownOriginSlots_[storage] = slot;
    }

void LLVMBackend::SetOwnMoveOrigin(llvm::Value* storage, size_t line, size_t col)
{
        if (!sanitizeOwnership_ || storage == nullptr) return;
        auto it = ownOriginSlots_.find(storage);
        if (it == ownOriginSlots_.end()) return;
        uint64_t enc = ((uint64_t)line << 32) | (uint32_t)col;
        builder->CreateStore(builder->getInt64(enc), it->second);
    }

void LLVMBackend::ClearOwnMoveOrigin(llvm::Value* storage)
{
        if (!sanitizeOwnership_ || storage == nullptr) return;
        auto it = ownOriginSlots_.find(storage);
        if (it == ownOriginSlots_.end()) return;
        builder->CreateStore(builder->getInt64(0), it->second);
    }

void LLVMBackend::EmitOwnDerefGuard(llvm::Value* storage, llvm::Value* loadedPtr, size_t useLine, size_t useCol)
{
        if (!sanitizeOwnership_ || loadedPtr == nullptr) return;
        if (!loadedPtr->getType()->isPointerTy()) return;
        llvm::Function* trapFn = GetFunction("__cflat_own_trap");
        if (trapFn == nullptr) return;  // shim not linked; nothing to call

        // An origin slot exists only for a tracked owning local; its value is the move site (or 0).
        llvm::Value* originSlot = nullptr;
        if (storage != nullptr)
        {
            auto it = ownOriginSlots_.find(storage);
            if (it != ownOriginSlots_.end()) originSlot = it->second;
        }

        auto* isNull = builder->CreateICmpEQ(loadedPtr,
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(loadedPtr->getType())), "own_null");
        llvm::Function* fn = builder->GetInsertBlock()->getParent();
        auto* trapBB = llvm::BasicBlock::Create(*context, "own_trap", fn);
        auto* contBB = llvm::BasicBlock::Create(*context, "own_cont", fn);
        builder->CreateCondBr(isNull, trapBB, contBB);

        builder->SetInsertPoint(trapBB);
        llvm::Value* originLine = builder->getInt32(0);
        llvm::Value* originCol  = builder->getInt32(0);
        if (originSlot != nullptr)
        {
            auto* origin = builder->CreateLoad(builder->getInt64Ty(), originSlot, "own_origin_v");
            originLine = builder->CreateTrunc(builder->CreateLShr(origin, builder->getInt64(32)), builder->getInt32Ty());
            originCol  = builder->CreateTrunc(builder->CreateAnd(origin, builder->getInt64(0xffffffff)), builder->getInt32Ty());
        }
        builder->CreateCall(trapFn->getFunctionType(), trapFn,
            { builder->getInt32((uint32_t)useLine), builder->getInt32((uint32_t)useCol), originLine, originCol });
        builder->CreateBr(contBB);  // trap aborts; keep the IR well-formed
        builder->SetInsertPoint(contBB);
    }

void LLVMBackend::RequestStaticLocalStorage(const std::string& varName, const std::string& typeName)
{
        pendingStaticLocalName_ = varName;
        pendingStaticLocalType_ = typeName;
        auto* bb = builder->GetInsertBlock();
        pendingStaticLocalFn_ = bb != nullptr ? bb->getParent() : nullptr;
        pendingStaticLocalDepth_ = stackNamedVariable.size();
    }

void LLVMBackend::ClearStaticLocalRequest()
{
        pendingStaticLocalName_.clear();
        pendingStaticLocalType_.clear();
        pendingStaticLocalFn_ = nullptr;
        pendingStaticLocalDepth_ = 0;
    }

// The declaration that asked for static storage is the only one allowed to take it: same name,
// same type, same enclosing function, same scope depth.
bool LLVMBackend::MatchesStaticLocalRequest(const TypeAndValue& typeValue) const
{
        if (pendingStaticLocalName_.empty()) return false;
        if (pendingStaticLocalName_ != typeValue.VariableName) return false;
        if (pendingStaticLocalType_ != typeValue.TypeName) return false;
        if (pendingStaticLocalDepth_ != stackNamedVariable.size()) return false;
        auto* bb = builder->GetInsertBlock();
        return pendingStaticLocalFn_ == (bb != nullptr ? bb->getParent() : nullptr);
    }

llvm::Value* LLVMBackend::CreateLocalVariable(TypeAndValue typeValue, llvm::Type* autoType, llvm::Value* arraySize, size_t line, uint64_t userAlign)
{
        // No enclosing scope means a file-scope declaration reached the local path (a stale
        // global_scope). back() on the empty scope stack is UB - diagnose instead of corrupting.
        if (stackNamedVariable.empty())
            LogError(std::format("internal: local variable '{}' declared with no enclosing scope",
                                 typeValue.VariableName));

        if (!typeValue.Pointer)
            RecordInterfaceMaterialization(typeValue.TypeName, "the type of a local variable");

        auto type = GetType(typeValue, autoType);
        // An unsized opaque shell has no by-value layout. The shell may come from C interop,
        // a forward declaration, or a declaration whose body was abandoned after an expected
        // error, so this site must describe the mechanism rather than guess at its provenance.
        if (!typeValue.Pointer && type != nullptr && type->isStructTy() && !type->isSized())
        {
            LogError(std::format(
                "type '{}' is incomplete here (its layout is not available at this point); "
                "it can only be used through a pointer", typeValue.TypeName));
            type = builder->getInt8Ty();  // sized placeholder; the error already aborts the compile
        }
        // GetType returns [N x T] for fixed-size arrays; passing N again as element-count
        // would allocate N x [N x T] - an N x N over-allocation (int[1024] -> 4MB not 4KB).
        if (typeValue.ConstArraySize > 0)
            arraySize = nullptr;
        // Effective alignment: max(decl-level alignas, struct-level alignas, ABI).
        uint64_t effAlign = GetEffectiveAlignmentForType(typeValue.TypeName, type);
        if (userAlign > effAlign) effAlign = userAlign;

        // `static` local: storage is an internal module global (program lifetime), not an alloca.
        // The declaration path already opened the run-once guard around the initializer.
        if (MatchesStaticLocalRequest(typeValue))
        {
            ClearStaticLocalRequest();
            // A run-time length has no meaning for storage created once at module scope; rejecting
            // beats emitting a mis-sized global (which fails module verification downstream).
            if (arraySize != nullptr)
                LogError(std::format(
                    "a 'static' local array must have a compile-time constant length; '{}' is sized "
                    "by a run-time value, which cannot be given program lifetime", typeValue.VariableName));
            std::string owner;
            if (auto* bb = builder->GetInsertBlock(); bb != nullptr && bb->getParent() != nullptr)
                owner = bb->getParent()->getName().str();
            // LLVM uniquifies a repeated name, so two same-named statics in one function (or in
            // separate blocks) still get distinct storage.
            auto* gv = new llvm::GlobalVariable(
                *module, type, false, llvm::GlobalValue::InternalLinkage,
                llvm::Constant::getNullValue(type), owner + ".static." + typeValue.VariableName);
            if (effAlign > 0) gv->setAlignment(llvm::Align(effAlign));
            auto& staticVariable = stackNamedVariable.back().namedVariable[typeValue.VariableName];
            staticVariable.Storage = gv;
            staticVariable.TypeAndValue = typeValue;
            staticVariable.BaseType = type;
            staticVariable.IsStaticLocal = true;
            RecordMoveGenBind(typeValue.VariableName);
            if (symbolSink_ && !typeValue.VariableName.empty())
                symbolSink_->RegisterVariable(typeValue.VariableName, typeValue.TypeName,
                                              GetSourceFilePath(), (int)line, 0);
            CreateOwnOriginSlot(gv);
            if (diBuilder && diFile && !typeValue.VariableName.empty())
            {
                llvm::DIFile* gvFile = GetDIFileForCurrentSource();
                if (!gvFile) gvFile = diFile;
                pendingGlobalDI_.push_back({gv, typeValue, gvFile, (unsigned)line,
                    currentSubprogram, true});
            }
            return gv;
        }
        // Only annotate when above the natural ABI alignment - otherwise LLVM's
        // default is already correct and we avoid noisy IR.
        uint64_t abiAlign = (type && type->isSized())
            ? module->getDataLayout().getABITypeAlign(type).value() : 0;
        uint64_t allocaAlign = (effAlign > abiAlign) ? effAlign : 0;
        auto alloc = AllocaAtEntry(type, arraySize, typeValue.VariableName, allocaAlign);
        auto& namedVariable = stackNamedVariable.back().namedVariable[typeValue.VariableName];
        namedVariable.Storage = alloc;
        namedVariable.TypeAndValue = typeValue;
        namedVariable.BaseType = type;
        RecordMoveGenBind(typeValue.VariableName); // fresh local binding
        // --sanitize=ownership (M1): give every pointer local a zero-initialized move-origin slot.
        if (typeValue.Pointer)
            CreateOwnOriginSlot(alloc);



        if (symbolSink_ && !typeValue.VariableName.empty())
            symbolSink_->RegisterVariable(typeValue.VariableName, typeValue.TypeName,
                                          GetSourceFilePath(), (int)line, 0);

        if (diBuilder && currentSubprogram && (unsigned)line > 0)
        {
            auto diType = GetDIType(typeValue);
            llvm::DIFile* locFile = currentSubprogram->getFile();
            if (!locFile) locFile = diFile;
            auto diVar = diBuilder->createAutoVariable(currentSubprogram, typeValue.VariableName, locFile, (unsigned)line, diType);
            diBuilder->insertDeclare(alloc, diVar, diBuilder->createExpression(),
                llvm::DILocation::get(*context, (unsigned)line, 0, currentSubprogram),
                builder->GetInsertBlock());
        }

        return alloc;
    }

void LLVMBackend::RegisterPrimaryVariable(const TypeAndValue& typeValue, llvm::Value* value)
{
        auto& namedVariable = stackNamedVariable.back().namedVariable[typeValue.VariableName];
        namedVariable.Primary = value;
        namedVariable.Storage = nullptr;
        namedVariable.TypeAndValue = typeValue;
        namedVariable.BaseType = value->getType();
        RecordMoveGenBind(typeValue.VariableName); // fresh inlined-param binding

        if (symbolSink_ && !typeValue.VariableName.empty())
            symbolSink_->RegisterVariable(typeValue.VariableName, typeValue.TypeName);
    }

llvm::AllocaInst* LLVMBackend::CreateAlloca(llvm::Type* type)
{
        return AllocaAtEntry(type, nullptr);
    }

void LLVMBackend::RegisterThisPointer(const TypeAndValue& tv, llvm::Value* storage, llvm::Type* baseType)
{
        NamedVariable namedVar{
            .TypeAndValue = tv,
            .BaseType = baseType,
            .Primary = nullptr,
            .Storage = storage,
        };
        stackNamedVariable.back().functionArgument[tv.VariableName] = namedVar;

        // Also register under "this" so `this->field` resolves correctly.
        // Primary=storage makes LoadNamedVariable return the pointer, not load through it.
        TypeAndValue thisTv = tv;
        thisTv.VariableName = "this";
        NamedVariable thisVar{
            .TypeAndValue = thisTv,
            .BaseType = baseType,
            .Primary = storage,
            .Storage = nullptr,
        };
        stackNamedVariable.back().functionArgument["this"] = thisVar;
    }

llvm::Value* LLVMBackend::CreateIncrement(llvm::Value* destination, int amount, llvm::Type* elemType,
                                          llvm::Type* loadType)
{
        // `loadType` is set only for a UNION member, whose Storage is the union alloca - inferring
        // the type off that storage reads (and would write back) the whole union.
        llvm::LoadInst* loadInst = loadType ? CreateLoad(loadType, destination) : CreateLoad(destination);

        if (loadInst->getType()->isPointerTy())
        {
            // Pointer increment/decrement: step by elemType-sized strides (C semantics).
            // Fall back to i8 (byte stride) only when element type is unknown.
            auto* stepTy = elemType ? elemType : builder->getInt8Ty();
            auto* step = llvm::ConstantInt::get(builder->getInt64Ty(), amount);
            auto* newPtr = builder->CreateGEP(stepTy, loadInst, step, "ptrinc");
            return builder->CreateStore(newPtr, destination);
        }

        auto value = llvm::ConstantInt::get(loadInst->getType(), amount);
        auto newValue = CreateOperation(Operation::Add, loadInst, value);
        return builder->CreateStore(newValue, destination);
    }

llvm::Value* LLVMBackend::CreateInsertValue(llvm::Value* structInstance, llvm::Value* newValue, unsigned int index)
{
        return builder->CreateInsertValue(structInstance, newValue, index);
    }

llvm::Value* LLVMBackend::CreateStructGEP(llvm::Type* structType, llvm::Value* structAlloc, unsigned int index, std::string variableName)
{
        // LLVM 18 opaque pointer mode: ConstantFolder folds GlobalVariable GEPs into ConstantExpr,
        // which calls ConstantExpr::getCast with an invalid type combination and asserts. Insert directly.
        if (llvm::isa<llvm::GlobalVariable>(structAlloc))
        {
            llvm::Value* idxs[] = { builder->getInt32(0), builder->getInt32(index) };
            auto* gep = llvm::GetElementPtrInst::CreateInBounds(structType, structAlloc, idxs);
            builder->Insert(gep, variableName);
            return gep;
        }
        return builder->CreateStructGEP(structType, structAlloc, index, variableName);
    }

llvm::Value* LLVMBackend::CreateGEP(llvm::Type* type, llvm::Value* ptr, llvm::Value* offset, std::string name)
{
        return builder->CreateGEP(type, ptr, offset, name);
    }

llvm::Value* LLVMBackend::CreateExtractValue(llvm::Value* structInstance, unsigned int index)
{
        return builder->CreateExtractValue(structInstance, index);
    }

llvm::StoreInst* LLVMBackend::CreateAssignment(llvm::Value* value, llvm::Value* destination, bool srcIsUnsigned, llvm::Type* explicitDestType)
{
        auto destType = explicitDestType ? explicitDestType : GetTypeFromStorage(destination);
        if (destType == builder->getInt1Ty())
        {
            value = builder->CreateICmpNE(value, llvm::ConstantInt::get(value->getType(), 0), "tobool");
        }
        else
        {
            // When assigning a raw ptr (string literal) into a `string` struct variable,
            // wrap it into the { _ptr, _len } struct instead of attempting an invalid bitcast.
            auto* strTy = llvm::StructType::getTypeByName(*context, "string");
            if (strTy && destType == strTy && value->getType()->isPointerTy() && value->getType() != strTy)
            {
                value = WrapStringLiteralAsString(value);
            }
            else if (auto* dvt = llvm::dyn_cast<llvm::FixedVectorType>(destType);
                     dvt != nullptr && !value->getType()->isVectorTy()
                     && (value->getType()->isIntegerTy() || value->getType()->isFloatingPointTy()))
            {
                // Scalar into simd<T,N> storage: splat across the lanes, the same rule the
                // declaration initializer uses. The else arm below casts scalar-to-vector, a
                // bitcast that wrote the value into every EVEN lane and left the odd ones zero.
                value = ConvertScalarToType(value, dvt->getElementType(), srcIsUnsigned);
                value = builder->CreateVectorSplat(dvt->getElementCount(), value);
            }
            else
            {
                // Upconvert handles integer widening with correct sign semantics (SExt for signed, ZExt for unsigned).
                // CreateCast handles all other conversions (truncation, int<->float, ptr<->int, etc.).
                value = Upconvert(value, destType, srcIsUnsigned);
                if (value->getType() != destType)
                    value = CreateCast(value, destType);
            }
        }
        return builder->CreateStore(value, destination);
    }

llvm::LoadInst* LLVMBackend::CreateLoad(llvm::Value* value)
{
        llvm::Type* type = GetTypeFromStorage(value);
        return builder->CreateLoad(type, value);
    }

llvm::LoadInst* LLVMBackend::CreateLoad(llvm::Type* type, llvm::Value* value)
{
        return builder->CreateLoad(type, value);
    }

llvm::Value* LLVMBackend::LoadArgStorage(const NamedVariable& arg)
{
        if (arg.Storage == nullptr)
            return nullptr;
        return arg.UnionFieldType ? CreateLoad(arg.UnionFieldType, arg.Storage)
                                  : static_cast<llvm::Value*>(CreateLoad(arg.Storage));
    }

llvm::Value* LLVMBackend::Upconvert(llvm::Value* value, llvm::Value* destination, bool srcIsUnsigned) const
{
        auto destType = GetTypeFromStorage(destination);
        return Upconvert(value, destType, srcIsUnsigned);
    }

llvm::Value* LLVMBackend::Upconvert(llvm::Value* value, llvm::Type* destType, bool srcIsUnsigned) const
{
        auto srcType = value->getType();
        if (srcType->isIntegerTy() && destType->isIntegerTy())
        {
            auto targetSize = destType->getIntegerBitWidth();
            auto srcSize = srcType->getIntegerBitWidth();

            if (srcSize < targetSize)
            {
                // i1 (bool) and unsigned types zero-extend; signed types sign-extend
                if (srcSize == 1 || srcIsUnsigned)
                    return builder->CreateZExt(value, destType);
                return builder->CreateSExt(value, destType);
            }
        }
        else if (srcType->isFloatingPointTy() && destType->isFloatingPointTy())
        {
            auto targetSize = destType->getScalarSizeInBits();
            auto srcSize = srcType->getScalarSizeInBits();

            // Widening only (float -> double). Narrowing not handled here -
            // use a typed literal (0.0f) or explicit cast so types match.
            if (srcSize < targetSize)
                return builder->CreateFPExt(value, destType);
        }
        else if (srcType->isIntegerTy() && destType->isFloatingPointTy())
        {
            // Integer literal initializer for a float/double field (e.g. float x = 0)
            if (auto* constInt = llvm::dyn_cast<llvm::ConstantInt>(value))
                return llvm::ConstantFP::get(destType, (double)(int64_t)constInt->getZExtValue());
            return builder->CreateSIToFP(value, destType);
        }
        else if (srcType->isIntegerTy() && destType->isPointerTy())
        {
            // Integer 0 assigned to a pointer field - produce a proper null/ptr constant.
            if (auto* constInt = llvm::dyn_cast<llvm::ConstantInt>(value))
            {
                if (constInt->isZero())
                    return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(destType));
            }
            // Widen to pointer width with sign semantics before inttoptr (which
            // zero-extends), so a negative sentinel keeps its high bits.
            unsigned ptrBits = module->getDataLayout().getPointerSizeInBits();
            if (srcType->getIntegerBitWidth() < ptrBits)
            {
                auto* ptrInt = llvm::Type::getIntNTy(*context, ptrBits);
                value = (srcIsUnsigned || srcType->getIntegerBitWidth() == 1)
                    ? builder->CreateZExt(value, ptrInt)
                    : builder->CreateSExt(value, ptrInt);
            }
            return builder->CreateIntToPtr(value, destType);
        }

        return value;
    }

llvm::Value* LLVMBackend::PromoteToInt(llvm::Value* value, bool isUnsigned) const
{
        auto* type = value->getType();
        if (type->isIntegerTy())
        {
            unsigned width = type->getIntegerBitWidth();
            if (width == 8 || width == 16)
                return Upconvert(value, builder->getInt32Ty(), isUnsigned);
        }
        return value;
    }

int LLVMBackend::CompareUpconvert(llvm::Type* srcType, llvm::Type* destType) const
{
        // Null srcType (e.g. bare literal cast through operator overload, no TypeName): return -1
        // so caller reports "no overload matches" instead of segfaulting on null dereference.
        if (srcType == nullptr || destType == nullptr)
            return -1;

        if (srcType->isPointerTy() && destType->isPointerTy())
        {
            return 0;
        }
        if (srcType->isPointerTy() || destType->isPointerTy())
        {
            return -1;  // pointer vs non-pointer: incompatible
        }

        if (srcType->isIntegerTy() && destType->isIntegerTy())
        {
            auto targetSize = destType->getIntegerBitWidth();
            auto srcSize = srcType->getIntegerBitWidth();

            // upconvert is needed
            if (srcSize <= targetSize)
            {
                return targetSize - srcSize;
            }
        }
        else if (srcType->isFloatingPointTy() && destType->isFloatingPointTy())
        {
            auto targetSize = destType->getScalarSizeInBits();
            auto srcSize = srcType->getScalarSizeInBits();

            // upconvert is needed
            if (srcSize <= targetSize)
            {
                return targetSize - srcSize;
            }
        }

        return -1;
    }

llvm::Type* LLVMBackend::GetTypeFromStorage(llvm::Value* value) const
{
        llvm::Type* type = nullptr;

        if (auto* allocaInst = llvm::dyn_cast<llvm::AllocaInst>(value))
        {
            // If it's a direct alloca, we know the type it was created with
            type = allocaInst->getAllocatedType();
        }
        else if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(value))
        {
            // If it's a GEP (e.g., from CreateStructGEP), get the element type
            type = gep->getResultElementType();

            if (type == nullptr)
            {
                type = gep->getSourceElementType();
            }
        }
        else if (auto* global = llvm::dyn_cast<llvm::GlobalVariable>(value))
        {
            // If it's a global variable
            type = global->getValueType();
        }
        else
        {
            type = value->getType();
        }

        if (type == nullptr)
        {
            LogError(std::format("GetTypeFromStorage could not resolve type for value '{}'", value->getName().str()));
        }

        return type;
    }

std::string LLVMBackend::DescribeAggregateStorageShape(llvm::Type* type,
                                                     const std::string& elementTypeName)
{
        if (!llvm::isa_and_nonnull<llvm::ArrayType>(type))
            return "struct storage";
        std::string dims;
        while (auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(type))
        {
            dims += std::format("[{}]", arrTy->getNumElements());
            type = arrTy->getElementType();
        }
        // A simd element keeps its own spelling: the caller's name is only the LANE type, so
        // without this an array of vectors reads as 'float[2]' instead of 'simd<float,4>[2]'.
        std::string elemName = elementTypeName;
        if (auto* vecTy = llvm::dyn_cast<llvm::FixedVectorType>(type); vecTy != nullptr && !elemName.empty())
            elemName = std::format("simd<{},{}>", elemName, vecTy->getNumElements());
        if (!elemName.empty())
            return std::format("fixed array '{}{}'", elemName, dims);
        return std::format("fixed-array storage with dimensions {}", dims);
    }

llvm::Value* LLVMBackend::CreateCast(llvm::Value* value, llvm::Type* destType, bool isSigned)
{
        auto srcType = value->getType();

        if (srcType == destType)
            return value;

        // A function address converted to a scalar is the forgot-the-parens bug
        // ('double pi = Math.PI;') - reject before it bitcasts into garbage.
        if (destType->isIntegerTy() || destType->isFloatingPointTy())
            RejectBareFunctionValue(value);

        // Integer <-> Integer
        if (srcType->isIntegerTy() && destType->isIntegerTy())
        {
            unsigned srcBits = srcType->getIntegerBitWidth();
            unsigned dstBits = destType->getIntegerBitWidth();
            if (dstBits < srcBits)
                return builder->CreateTrunc(value, destType);
            // i1 (bool) and unsigned types zero-extend; signed types sign-extend
            if (srcBits == 1 || !isSigned)
                return builder->CreateZExt(value, destType);
            return builder->CreateSExt(value, destType);
        }

        // Float <-> Float
        if (srcType->isFloatingPointTy() && destType->isFloatingPointTy())
        {
            if (destType->getScalarSizeInBits() < srcType->getScalarSizeInBits())
                return builder->CreateFPTrunc(value, destType);
            else
                return builder->CreateFPExt(value, destType);
        }

        // Integer -> Float
        if (srcType->isIntegerTy() && destType->isFloatingPointTy())
        {
            return builder->CreateSIToFP(value, destType);
        }

        // Float -> Integer
        if (srcType->isFloatingPointTy() && destType->isIntegerTy())
        {
            return builder->CreateFPToSI(value, destType);
        }

        // Pointer -> Integer
        if (srcType->isPointerTy() && destType->isIntegerTy())
            return builder->CreatePtrToInt(value, destType);

        // Integer -> Pointer
        if (srcType->isIntegerTy() && destType->isPointerTy())
        {
            // inttoptr zero-extends a narrow source; widen with the source's sign
            // first so (void*)(-2) yields 0xff...fe (RTLD_DEFAULT-style sentinels).
            unsigned ptrBits = module->getDataLayout().getPointerSizeInBits();
            if (srcType->getIntegerBitWidth() < ptrBits)
                value = CreateCast(value, llvm::Type::getIntNTy(*context, ptrBits), isSigned);
            return builder->CreateIntToPtr(value, destType);
        }

        // Pointer -> Pointer (reinterpret)
        if (srcType->isPointerTy() && destType->isPointerTy())
            return builder->CreateBitCast(value, destType);

        // Struct -> Pointer: invalid; caller should have emitted an error already.
        // Return the value unchanged rather than letting LLVM assert.
        if (srcType->isStructTy() && destType->isPointerTy())
        {
            LogError("cannot assign a struct value to a pointer variable - use getPtr() or take the address with '&'");
            return value;
        }

        /*
         * A bitcast may not have an aggregate operand at all, so a non-aggregate -> aggregate
         * conversion here is a pure backstop: every spelling that reaches it would otherwise fail
         * module verification, or - when the source is a Constant, which folds the cast into an
         * unchecked ConstantExpr - miscompile or crash the compiler in SelectionDAG. The front end
         * rejects the known spellings with a construct-specific message; this catches the ones
         * nobody enumerated, and gives them a source location.
         */
        if (destType->isAggregateType() && !srcType->isAggregateType())
        {
            // A null source zeroes the storage, matching the '= nullptr' carve-out on the pointer
            // path. Rejecting '= 0' while accepting '= nullptr' on the same storage is arbitrary.
            auto* srcConst = llvm::dyn_cast<llvm::Constant>(value);
            if (srcConst != nullptr && srcConst->isNullValue())
                return llvm::Constant::getNullValue(destType);

            if (srcType->isPointerTy())
            {
                LogError(std::format(
                    "cannot store a pointer value into {} - a fixed array is not assignable from a "
                    "pointer or a string literal. Assign its elements individually, or declare the "
                    "destination as a pointer 'T*' or an array view 'T[]' to borrow the source "
                    "instead of copying it.",
                    DescribeAggregateStorageShape(destType)));
                return value;
            }
            LogError(std::format(
                "cannot store a single scalar value into {} - a fixed array or struct is not "
                "assignable from one value. Use '= default' to zero it, a brace list to fill it, "
                "or assign its elements or fields individually.",
                DescribeAggregateStorageShape(destType)));
            return value;
        }

        // Aggregate operand: the frontend should have decayed a fixed array to a
        // pointer (or rejected the cast) before reaching here. Emit a diagnostic
        // instead of a bitcast LLVM's verifier would reject.
        if (srcType->isAggregateType())
        {
            LogError("cannot cast an aggregate value - a fixed array decays to a pointer to its first element");
            return value;
        }

        // Fallback: BitCast for same-size reinterpretation
        return builder->CreateBitCast(value, destType);
    }

llvm::Value* LLVMBackend::CreateCast(llvm::Instruction::CastOps op, llvm::Value* value, llvm::Type* destType)
{
        return builder->CreateCast(op, value, destType);
    }

unsigned LLVMBackend::BitfieldStorageBits(const std::string& typeName)
{
        if (typeName == "bool")  return 8;   // CFlat bool is i8 in storage
        if (typeName == "char" || typeName == "i8"  || typeName == "u8")  return 8;
        if (typeName == "short"|| typeName == "i16" || typeName == "u16") return 16;
        if (typeName == "int"  || typeName == "i32" || typeName == "u32") return 32;
        if (typeName == "i64" || typeName == "u64") return 64;
        // target-native C `long` / `unsigned long`
        if (typeName == "long" || typeName == "ulong") return longBits_;
        return 0;
    }

std::vector<LLVMBackend::DeclTypeAndValue> LLVMBackend::PackBitfields(
        const std::vector<DeclTypeAndValue>& in,
        std::vector<BitfieldInfo>& outBitfields)
{
        std::vector<DeclTypeAndValue> out;
        outBitfields.clear();
        int synthIdx = 0;
        size_t i = 0;
        while (i < in.size())
        {
            const auto& cur = in[i];
            if (!cur.IsBitfield)
            {
                out.push_back(cur);
                i++;
                continue;
            }

            unsigned storageBits = BitfieldStorageBits(cur.TypeName);
            if (storageBits == 0)
            {
                LogError("bitfield '" + cur.VariableName + "' has unsupported underlying type '" + cur.TypeName + "' (must be an integer or bool type)");
                i++;
                continue;
            }
            if (cur.BitWidth > storageBits)
            {
                LogError("bitfield '" + cur.VariableName + "' width " + std::to_string(cur.BitWidth)
                       + " exceeds underlying type '" + cur.TypeName + "' width " + std::to_string(storageBits));
                i++;
                continue;
            }

            // Width-0 acts as a boundary marker; close any open run (here means
            // do not start one). A name is permitted but inaccessible (it has
            // zero bits) - mirrors MSVC's lenient interpretation rather than the
            // strict C standard which only allows the unnamed form.
            if (cur.BitWidth == 0)
            {
                i++;
                continue;
            }

            // Open a new storage unit using the underlying type of the first bitfield.
            unsigned storageIdx = (unsigned)out.size();
            unsigned bitOffset = 0;
            DeclTypeAndValue storage = cur;
            storage.VariableName = "__bf" + std::to_string(synthIdx++);
            storage.IsBitfield = false;     // the storage slot itself isn't a bitfield
            storage.IsBitfieldStorage = true;
            storage.BitWidth = 0;
            storage.BitOffset = 0;
            storage.StorageFieldIndex = 0;
            storage.Initializer = nullptr;  // zeroed by default; per-bitfield init handled at field-init time
            storage.BraceInitializer = nullptr;
            storage.Annotations.clear();
            storage.GuardedBy.clear();
            out.push_back(storage);

            // Greedily attach this and subsequent same-type bitfields that fit.
            while (i < in.size() && in[i].IsBitfield && in[i].TypeName == cur.TypeName)
            {
                const auto& bf = in[i];
                if (bf.BitWidth == 0)
                {
                    // Width-0 marker closes the current unit; consume and stop.
                    i++;
                    break;
                }
                if (bf.BitWidth > storageBits)
                {
                    LogError("bitfield '" + bf.VariableName + "' width " + std::to_string(bf.BitWidth)
                           + " exceeds underlying type '" + bf.TypeName + "' width " + std::to_string(storageBits));
                    i++;
                    continue;
                }
                if (bitOffset + bf.BitWidth > storageBits)
                {
                    // Doesn't fit - leave it for the outer loop to start a new unit.
                    break;
                }
                if (!bf.VariableName.empty())
                {
                    BitfieldInfo info;
                    info.Name = bf.VariableName;
                    info.TypeName = bf.TypeName;
                    info.IsUnsigned = (bf.IsUnsignedInteger() != -1) || bf.TypeName == "bool";
                    info.StorageFieldIndex = storageIdx;
                    info.BitOffset = bitOffset;
                    info.BitWidth = bf.BitWidth;
                    info.Annotations = bf.Annotations;
                    outBitfields.push_back(info);
                }
                // Anonymous (no name) bitfield reserves bits but is unreachable.
                bitOffset += bf.BitWidth;
                i++;
            }
            // Outer-loop continue: next iteration handles whatever didn't fit.
        }
        return out;
    }

uint64_t LLVMBackend::GetFieldSlotAlignment(const DeclTypeAndValue& f, llvm::Type* t) const
{
        uint64_t a = 1;
        if (t && t->isSized())
            a = module->getDataLayout().getABITypeAlign(t).value();
        if (f.UserAlignValue > a) a = f.UserAlignValue;
        if (!f.Pointer && !f.ElemPointer && !f.IsInterface && !f.IsInterfacePointer
            && !f.IsFunctionPointer && !f.IsArrayView)
        {
            auto it = dataStructures.find(f.TypeName);
            if (it != dataStructures.end() && it->second.UserRequestedAlignment > a)
                a = it->second.UserRequestedAlignment;
        }
        return a;
    }

bool LLVMBackend::FieldsNeedAlignmentPadding(const std::vector<DeclTypeAndValue>& in) const
{
        for (const auto& f : in)
        {
            if (f.UserAlignValue > 1) return true;
            if (f.Pointer || f.ElemPointer || f.IsInterface || f.IsInterfacePointer
                || f.IsFunctionPointer || f.IsArrayView)
                continue;
            auto it = dataStructures.find(f.TypeName);
            if (it != dataStructures.end() && it->second.UserRequestedAlignment > 1)
                return true;
        }
        return false;
    }

std::vector<LLVMBackend::DeclTypeAndValue> LLVMBackend::PadFieldsForAlignment(
        const std::vector<DeclTypeAndValue>& in,
        uint64_t& outMaxAlign,
        std::vector<BitfieldInfo>* bitfields)
{
        outMaxAlign = 0;
        if (in.empty() || !FieldsNeedAlignmentPadding(in))
            return in;

        const llvm::DataLayout& dl = module->getDataLayout();
        std::vector<llvm::Type*> types;
        types.reserve(in.size());
        for (const auto& f : in)
        {
            llvm::Type* t = GetType(f);
            // An unsized (opaque/incomplete) field has no layout; the struct is already
            // ill-formed and CreateStructType reports it. Leave the field list untouched.
            if (t == nullptr || !t->isSized())
                return in;
            types.push_back(t);
        }

        std::vector<DeclTypeAndValue> out;
        out.reserve(in.size() * 2);
        std::vector<unsigned> oldToNew(in.size(), 0);
        uint64_t offset = 0;
        int padIdx = 0;
        for (size_t i = 0; i < in.size(); i++)
        {
            uint64_t abi  = dl.getABITypeAlign(types[i]).value();
            uint64_t want = GetFieldSlotAlignment(in[i], types[i]);
            if (want > outMaxAlign) outMaxAlign = want;

            uint64_t natural = (offset + abi - 1) / abi * abi;
            uint64_t target  = (offset + want - 1) / want * want;
            if (target > natural)
            {
                // [N x i8] has alignment 1, so it lands exactly at `offset` and the real
                // field that follows starts at `target` (a multiple of its own ABI align).
                DeclTypeAndValue pad;
                pad.TypeName       = "u8";
                pad.VariableName   = "__pad" + std::to_string(padIdx++);
                pad.ConstArraySize = target - offset;
                pad.IsPadding      = true;
                out.push_back(pad);
                offset = target;
            }
            else
            {
                offset = natural;
            }
            oldToNew[i] = (unsigned)out.size();
            out.push_back(in[i]);
            offset += dl.getTypeAllocSize(types[i]);
        }

        if (bitfields != nullptr)
            for (auto& bf : *bitfields)
                if (bf.StorageFieldIndex < oldToNew.size())
                    bf.StorageFieldIndex = oldToNew[bf.StorageFieldIndex];

        return out;
    }

bool LLVMBackend::UniqueChainReaches(const std::string& from, const std::string& target) const
{
        std::unordered_set<std::string> seen;
        std::vector<std::string> pending{ from };
        while (!pending.empty())
        {
            std::string t = std::move(pending.back());
            pending.pop_back();
            if (t == target) return true;
            if (!seen.insert(t).second) continue;
            auto it = dataStructures.find(t);
            if (it == dataStructures.end()) continue;
            for (const auto& f : it->second.StructFields)
                if (f.IsUnique && f.Pointer && !f.ElemPointer)
                    pending.push_back(f.TypeName);
        }
        return false;
    }

void LLVMBackend::RejectUniqueDestructionCycles(const std::string& name, const std::vector<LLVMBackend::DeclTypeAndValue>& fields)
{
        for (const auto& f : fields)
        {
            if (!f.IsUnique || !f.Pointer || f.ElemPointer) continue;
            if (!UniqueChainReaches(f.TypeName, name)) continue;
            LogError("'unique' on field '" + name + "." + f.VariableName + "' forms a destruction cycle back to '"
                     + name + "': the synthesized destructor would recurse without bound. Drop 'unique' and write a destructor with an iterative teardown.");
        }
    }

llvm::StructType* LLVMBackend::CreateStructType(std::string name, std::vector<LLVMBackend::DeclTypeAndValue> typeAndValues, uint64_t userAlign, std::vector<BitfieldInfo>* bitfields)
{

        if (typeAndValues.size() > 0)
        {
            std::vector<llvm::Type*> types;

            for (const auto& typeValue : typeAndValues)
            {
                // A FIELD reaches a vtable read through 'h.f is X' without ever passing
                // CreateLocalVariable. Recorded, not rejected: this loop runs during
                // monomorphization, BEFORE the drain that instantiates this very interface.
                if (!typeValue.Pointer)
                    RecordInterfaceMaterialization(typeValue.TypeName, "the type of a struct field");
                types.emplace_back(GetType(typeValue));
            }

            // alignas(N) on the struct: append a trailing [padBytes x i8] member
            // so getTypeAllocSize matches the padded sizeof. Without this, arrays
            // of the struct stride at the natural size and elements lose alignment.
            // The padding is NOT added to StructFields metadata - user code never
            // sees the synthetic trailing field. Computed BEFORE any setBody call
            // so we only set the body once (LLVM asserts on resetting non-opaque
            // structs).
            if (userAlign > 1)
            {
                auto* tmp = llvm::StructType::get(*context, types);
                uint64_t natural = module->getDataLayout().getTypeAllocSize(tmp);
                uint64_t padded = (natural + userAlign - 1) / userAlign * userAlign;
                if (padded > natural)
                    types.push_back(llvm::ArrayType::get(builder->getInt8Ty(), padded - natural));
            }

            auto mystuct = dataStructures.find(name);
            if (mystuct == dataStructures.end())
            {
                llvm::StructType* myStruct = llvm::StructType::create(types, name);
                dataStructures[name].StructType = myStruct;
                dataStructures[name].StructFields = typeAndValues;
                if (bitfields && !bitfields->empty())
                    dataStructures[name].Bitfields = *bitfields;
                dataStructures[name].typeDescriptor = new llvm::GlobalVariable(
                    *module, builder->getInt8Ty(), true,
                    llvm::GlobalValue::InternalLinkage,
                    builder->getInt8(0), name + "_typedesc");

                if (userAlign > 1)
                    dataStructures[name].UserRequestedAlignment = userAlign;

                RejectUniqueDestructionCycles(name, typeAndValues);
                return myStruct;
            }

            // existing struct;
            auto& structData = mystuct->second;
            structData.StructFields = typeAndValues;
            if (bitfields && !bitfields->empty())
                structData.Bitfields = *bitfields;
            if (structData.StructType->isOpaque())
                structData.StructType->setBody(types);
            if (userAlign > 1)
                structData.UserRequestedAlignment = userAlign;

            RejectUniqueDestructionCycles(name, typeAndValues);
            return structData.StructType;
        }
        else
        {
            auto existing = dataStructures.find(name);
            if (existing != dataStructures.end())
                return existing->second.StructType;

            llvm::StructType* opaqueStruct = llvm::StructType::create(*context, name);
            dataStructures[name].StructType = opaqueStruct;
            dataStructures[name].typeDescriptor = new llvm::GlobalVariable(
                *module, builder->getInt8Ty(), true,
                llvm::GlobalValue::InternalLinkage,
                builder->getInt8(0), name + "_typedesc");
            return opaqueStruct;
        }
    }

llvm::StructType* LLVMBackend::CreateUnionType(std::string name, std::vector<DeclTypeAndValue> typeAndValues, uint64_t userAlign)
{
        uint64_t maxSize = 1;
        llvm::Align maxAlign(1);
        for (const auto& tv : typeAndValues)
        {
            auto* t = GetType(tv);
            // An unsized member (opaque shell / incomplete type) has no layout; sizing it
            // would assert. Callers (RegisterCRecords) abandon such records before reaching
            // here, but guard defensively so this never crashes the compiler.
            if (!t || !t->isSized()) continue;
            uint64_t sz = module->getDataLayout().getTypeAllocSize(t);
            llvm::Align al = module->getDataLayout().getABITypeAlign(t);
            if (sz > maxSize) maxSize = sz;
            if (al > maxAlign) maxAlign = al;
            if (tv.UserAlignValue > userAlign) userAlign = tv.UserAlignValue;
        }
        if (userAlign > 1)
            maxSize = (maxSize + userAlign - 1) / userAlign * userAlign;

        // Pick an integer element type that satisfies maxAlign so the LLVM struct
        // inherits the correct ABI alignment (LLVM sets struct align = max(element aligns)).
        llvm::Type* alignTy;
        switch (maxAlign.value())
        {
            case 8:  alignTy = builder->getInt64Ty(); break;
            case 4:  alignTy = builder->getInt32Ty(); break;
            case 2:  alignTy = builder->getInt16Ty(); break;
            default: alignTy = builder->getInt8Ty();  break;
        }
        uint64_t elemSize = module->getDataLayout().getTypeAllocSize(alignTy);
        uint64_t numElems = (maxSize + elemSize - 1) / elemSize;
        auto* bodyTy = llvm::ArrayType::get(alignTy, numElems);

        auto it = dataStructures.find(name);
        llvm::StructType* unionTy;
        if (it != dataStructures.end() && it->second.StructType != nullptr)
        {
            unionTy = it->second.StructType;
            if (unionTy->isOpaque())
                unionTy->setBody({bodyTy});
        }
        else
        {
            unionTy = llvm::StructType::create(*context, {bodyTy}, name);
        }

        auto& sd = dataStructures[name];
        sd.StructType = unionTy;
        sd.StructFields = typeAndValues;
        sd.IsUnion = true;
        if (userAlign > 1)
            sd.UserRequestedAlignment = userAlign;
        if (sd.typeDescriptor == nullptr)
        {
            sd.typeDescriptor = new llvm::GlobalVariable(
                *module, builder->getInt8Ty(), true,
                llvm::GlobalValue::InternalLinkage,
                builder->getInt8(0), name + "_typedesc");
        }
        return unionTy;
    }

llvm::Value* LLVMBackend::CreateConstant(ConstantVariant constantVariant)
{
        llvm::Value* value = nullptr;

        if (auto* v = std::get_if<bool>(&constantVariant))
        {
            if (*v) { value = builder->getTrue(); }
            else { value = builder->getFalse(); }
        }
        else if (auto* v = std::get_if<char>(&constantVariant))
        {
            value = builder->getInt8(*v);
        }
        else if (auto* v = std::get_if<short>(&constantVariant))
        {
            value = builder->getInt16(*v);
        }
        else if (auto* v = std::get_if<int>(&constantVariant))
        {
            value = builder->getInt32(*v);
        }
        else if (auto* v = std::get_if<int64_t>(&constantVariant))
        {
            value = builder->getInt64(*v);
        }
        else if (auto* v = std::get_if<float>(&constantVariant))
        {
            value = llvm::ConstantFP::get(builder->getFloatTy(), *v);
        }
        else  if (auto* v = std::get_if<double>(&constantVariant))
        {
            double upConvert = *v;
            value = llvm::ConstantFP::get(builder->getDoubleTy(), *v);
        }
        else
        {
            LogError(std::format("CreateConstant encountered unsupported variant type (index {})", constantVariant.index()));
        }

        return value;
    }

llvm::Constant* LLVMBackend::CreateConstant(std::string typeName, std::string initialValue)
{
        llvm::Constant* value = nullptr;

        if (typeName == "char" || typeName == "i8" || typeName == "u8")
        {
            int initValue = 0;
            if (!initialValue.empty())
            {
                initValue = std::stoi(initialValue);
            }

            value = builder->getInt8(initValue);
        }
        else if (typeName == "short" || typeName == "i16" || typeName == "u16")
        {
            int initValue = 0;
            if (!initialValue.empty())
            {
                initValue = std::stoi(initialValue);
            }

            value = builder->getInt16(initValue);
        }
        else if (typeName == "int" || typeName == "i32" || typeName == "u32")
        {
            int initValue = 0;
            if (!initialValue.empty())
            {
                initValue = std::stoi(initialValue);
            }

            value = builder->getInt32(initValue);
        }
        else if (typeName == "long" || typeName == "ulong" || typeName == "i64" || typeName == "u64")
        {
            int initValue = 0;
            if (!initialValue.empty())
            {
                initValue = std::stoi(initialValue);
            }

            // `long`/`ulong` narrow to the target's native width (32-bit on Windows/LLP64).
            if ((typeName == "long" || typeName == "ulong") && longBits_ == 32)
                value = builder->getInt32(initValue);
            else
                value = builder->getInt64(initValue);
        }
        else if (typeName == "float")
        {
            float initValue = 0;
            if (!initialValue.empty())
            {
                initValue = std::stof(initialValue);
            }
            value = llvm::ConstantFP::get(builder->getFloatTy(), initValue);
        }
        else if (typeName == "double")
        {
            double initValue = 0;
            if (!initialValue.empty())
            {
                initValue = std::stoi(initialValue);
            }
            value = llvm::ConstantFP::get(builder->getDoubleTy(), initValue);
        }
        else if (typeName == "bool")
        {
            if (!initialValue.empty() && initialValue == "true")
            {
                value = builder->getTrue();
            }
            else
            {

                value = builder->getFalse();
            }
        }
        else if (typeName == "nullptr")
        {
            // create a i8 null pointer
            value = llvm::ConstantPointerNull::get(builder->getInt8Ty()->getPointerTo());
        }
        else
        {
            LogError(std::format("unknown type '{}'", typeName));
            return nullptr;
        }

        return value;
    }

llvm::Value* LLVMBackend::CreateGlobalString(std::string name, std::string text)
{
        auto it = stringPool.find(text);
        if (it != stringPool.end())
            return it->second;

        // Pass module explicitly so CreateGlobalString doesn't dereference the builder's
        // (possibly null) insertion point to find the module.
        auto* gv = builder->CreateGlobalString(text, name, 0, module.get());
        stringPool[text] = gv;
        stringLiteralLenByPtr[gv] = (int32_t)text.size();
        return gv;
    }

llvm::Value* LLVMBackend::CreateVectorOperation(Operation op, llvm::Value* left, llvm::Value* right,
                                       bool leftIsUnsigned, bool rightIsUnsigned)
{
        // Comparison signedness keeps its long-standing "either operand unsigned" rule; the
        // per-operand flags exist so a splatted scalar widens with its OWN signedness.
        bool isUnsigned = leftIsUnsigned || rightIsUnsigned;
        auto* lvt = llvm::dyn_cast<llvm::FixedVectorType>(left->getType());
        auto* rvt = llvm::dyn_cast<llvm::FixedVectorType>(right->getType());
        llvm::FixedVectorType* vt = lvt ? lvt : rvt;

        auto splat = [&](llvm::Value* scalar, bool srcIsUnsigned) -> llvm::Value*
        {
            scalar = ConvertScalarToType(scalar, vt->getElementType(), srcIsUnsigned);
            return builder->CreateVectorSplat(vt->getElementCount(), scalar);
        };
        if (!lvt) left = splat(left, leftIsUnsigned);
        if (!rvt) right = splat(right, rightIsUnsigned);

        if (left->getType() != right->getType())
        {
            LogError("simd operands must have the same lane count and element type");
            return left;
        }

        bool isFloat = vt->getElementType()->isFloatingPointTy();
        switch (op)
        {
        case Operation::Add:      case Operation::AddAssignment:
            return isFloat ? builder->CreateFAdd(left, right) : builder->CreateAdd(left, right);
        case Operation::Subtract: case Operation::MinusAssignment:
            return isFloat ? builder->CreateFSub(left, right) : builder->CreateSub(left, right);
        case Operation::Multiply: case Operation::MultiplyAssignment:
            return isFloat ? builder->CreateFMul(left, right) : builder->CreateMul(left, right);
        case Operation::Divide:   case Operation::DivideAssignment:
            return isFloat ? builder->CreateFDiv(left, right)
                 : isUnsigned ? builder->CreateUDiv(left, right) : builder->CreateSDiv(left, right);
        // Comparisons -> <N x i1> mask. FP uses ordered predicates (a NaN lane compares false);
        // integers honour the operands' signedness for the relational ops.
        case Operation::Equal:
            return isFloat ? builder->CreateFCmpOEQ(left, right) : builder->CreateICmpEQ(left, right);
        case Operation::NotEqual:
            return isFloat ? builder->CreateFCmpONE(left, right) : builder->CreateICmpNE(left, right);
        case Operation::Greater:
            return isFloat ? builder->CreateFCmpOGT(left, right)
                 : isUnsigned ? builder->CreateICmpUGT(left, right) : builder->CreateICmpSGT(left, right);
        case Operation::GreaterEqual:
            return isFloat ? builder->CreateFCmpOGE(left, right)
                 : isUnsigned ? builder->CreateICmpUGE(left, right) : builder->CreateICmpSGE(left, right);
        case Operation::Less:
            return isFloat ? builder->CreateFCmpOLT(left, right)
                 : isUnsigned ? builder->CreateICmpULT(left, right) : builder->CreateICmpSLT(left, right);
        case Operation::LessEqual:
            return isFloat ? builder->CreateFCmpOLE(left, right)
                 : isUnsigned ? builder->CreateICmpULE(left, right) : builder->CreateICmpSLE(left, right);
        default:
            LogError("simd supports + - * / and comparisons == != < <= > >= (other operators are not yet supported)");
            return left;
        }
    }

llvm::Value* LLVMBackend::SplatToSimd(llvm::Value* scalar, const TypeAndValue& tv, bool srcIsUnsigned)
{
        // BACKSTOP, not the primary guard: the sole caller already proves the slot lowers to a
        // vector. An unchecked cast here aborted the compiler with no diagnostic on `simd<T,N>*`.
        auto* vecTy = llvm::dyn_cast_or_null<llvm::FixedVectorType>(GetType(tv));
        if (vecTy == nullptr)
        {
            // Spell the whole declared shape - pointer AND array - so the message is true of
            // every slot kind that can reach here, not just the pointer one. LogError throws.
            std::string decl = std::format("simd<{},{}>", tv.TypeName, tv.SimdLanes);
            if (tv.Pointer) decl += tv.ElemPointer ? "**" : "*";
            if (tv.ConstArraySize > 0) decl += std::format("[{}]", tv.ConstArraySize);
            LogError(std::format(
                "cannot splat a scalar into '{}' - it does not name simd vector storage", decl));
            return scalar;
        }
        scalar = ConvertScalarToType(scalar, vecTy->getElementType(), srcIsUnsigned);
        return builder->CreateVectorSplat(vecTy->getElementCount(), scalar);
    }

llvm::Value* LLVMBackend::ConvertScalarToType(llvm::Value* scalar, llvm::Type* target, bool srcIsUnsigned)
{
        scalar = Upconvert(scalar, target, srcIsUnsigned);
        if (scalar->getType() != target)
            scalar = CreateCast(scalar, target);
        return scalar;
    }

std::string LLVMBackend::FindFunctionSourceName(const llvm::Function* fn) const
{
        for (const auto& [name, overloads] : functionTable)
            for (const auto& sym : overloads)
                if (sym.Function == fn)
                    return name;
        return fn->getName().str();
    }

void LLVMBackend::RejectBareFunctionValue(llvm::Value* value) const
{
        if (value == nullptr)
            return;
        if (auto* fn = llvm::dyn_cast<llvm::Function>(value))
        {
            // Gated on the instantiation REGISTRY, never on a bare '__': DisplayNameOfMangledType
            // splits on the first '__', so '__error' / 'foo__bar' would be rewritten into nothing.
            std::string name = FindFunctionSourceName(fn);
            bool writable = true;
            if (gts.instantiatedGenericFunctions.count(name) != 0)
                name = DisplayNameOfMangledType(name, &writable);
            if (!writable)
                LogError(std::format(
                    "'{}' is a function used as a value - did you mean to call it? (a bare function name is only valid as a function<T> value)",
                    name));
            else
                LogError(std::format(
                    "'{}' is a function used as a value - did you mean '{}()'? (a bare function name is only valid as a function<T> value)",
                    name, name));
        }
    }

bool LLVMBackend::IsPlainStructOperand(llvm::Value* v)
{
        auto* st = v ? llvm::dyn_cast<llvm::StructType>(v->getType()) : nullptr;
        if (!st) return false;
        if (st->isLiteral() || !st->hasName()) return true;
        std::string n = st->getName().str();
        return n != "__iface_fat_ptr" && n != "__closure_fat_ptr";
    }

bool LLVMBackend::IsProvableNonHeapAddress(llvm::Value* value)
{
        if (value == nullptr || !value->getType()->isPointerTy())
            return false;
        llvm::Value* base = value->stripPointerCasts();
        // Walk GEP bases; stripInBoundsOffsets is not used since a non-constant index still
        // keeps the address inside the same stack/global object.
        while (auto* gep = llvm::dyn_cast<llvm::GEPOperator>(base))
            base = gep->getPointerOperand()->stripPointerCasts();
        return llvm::isa<llvm::AllocaInst>(base) || llvm::isa<llvm::GlobalVariable>(base);
    }

llvm::Value* LLVMBackend::CreateOperation(std::string oper, llvm::Value* left, llvm::Value* right)
{
        if (left == nullptr)
            return right;

        Operation op = ParseOperation(oper);

        return CreateOperation(op, left, right);
    }

llvm::Value* LLVMBackend::CreateOperation(Operation op, llvm::Value* left, llvm::Value* right)
{
        if (left == nullptr)
            return right;

        RejectBareFunctionValue(left);
        RejectBareFunctionValue(right);

        // Pointer operations: must run before Upconvert, which would corrupt pointer/int pairs.
        if (left->getType()->isPointerTy() || right->getType()->isPointerTy())
        {
            auto* i64Ty = builder->getInt64Ty();
            bool leftIsPtr  = left->getType()->isPointerTy();
            bool rightIsPtr = right->getType()->isPointerTy();
            switch (op)
            {
            case Operation::Less:
            case Operation::Greater:
            case Operation::LessEqual:
            case Operation::GreaterEqual:
            {
                // Address comparison: convert both sides to i64 and compare as unsigned integers.
                if (leftIsPtr)  left  = builder->CreatePtrToInt(left,  i64Ty);
                else if (!left->getType()->isIntegerTy(64))  left  = builder->CreateSExt(left,  i64Ty);
                if (rightIsPtr) right = builder->CreatePtrToInt(right, i64Ty);
                else if (!right->getType()->isIntegerTy(64)) right = builder->CreateSExt(right, i64Ty);
                auto pred = op == Operation::Less      ? llvm::ICmpInst::ICMP_ULT :
                            op == Operation::Greater   ? llvm::ICmpInst::ICMP_UGT :
                            op == Operation::LessEqual ? llvm::ICmpInst::ICMP_ULE :
                                                         llvm::ICmpInst::ICMP_UGE;
                return builder->CreateICmp(pred, left, right);
            }
            case Operation::BitwiseAnd:
            case Operation::AndAssignment:
            case Operation::BitwiseOr:
            case Operation::OrAssignment:
            case Operation::BitwiseXor:
            case Operation::XorAssignment:
            {
                // Bitwise masking: ptrtoint, op, inttoptr.  Result type matches the pointer side.
                auto* ptrTy = leftIsPtr ? left->getType() : right->getType();
                if (leftIsPtr)  left  = builder->CreatePtrToInt(left,  i64Ty);
                else if (!left->getType()->isIntegerTy(64))  left  = builder->CreateZExtOrTrunc(left,  i64Ty);
                if (rightIsPtr) right = builder->CreatePtrToInt(right, i64Ty);
                else if (!right->getType()->isIntegerTy(64)) right = builder->CreateZExtOrTrunc(right, i64Ty);
                llvm::Value* res;
                switch (op)
                {
                case Operation::BitwiseAnd: case Operation::AndAssignment: res = builder->CreateAnd(left, right); break;
                case Operation::BitwiseOr:  case Operation::OrAssignment:  res = builder->CreateOr(left,  right); break;
                default:                                                    res = builder->CreateXor(left, right); break;
                }
                return builder->CreateIntToPtr(res, ptrTy);
            }
            case Operation::Add:
            case Operation::AddAssignment:
            case Operation::Subtract:
            case Operation::MinusAssignment:
            {
                if (leftIsPtr && rightIsPtr)
                {
                    // ptr - ptr: byte difference as i64.
                    left  = builder->CreatePtrToInt(left,  i64Ty);
                    right = builder->CreatePtrToInt(right, i64Ty);
                    return builder->CreateSub(left, right, "ptrdiff");
                }
                // ptr +/- int (no element-type context here): byte arithmetic via i8 GEP.
                auto* ptrVal = leftIsPtr  ? left  : right;
                auto* intVal = leftIsPtr  ? right : left;
                if ((op == Operation::Subtract || op == Operation::MinusAssignment) && leftIsPtr)
                    intVal = builder->CreateNeg(intVal, "neg");
                intVal = Upconvert(intVal, i64Ty);
                return builder->CreateGEP(builder->getInt8Ty(), ptrVal, intVal, "ptrarith");
            }
            case Operation::Equal:
            case Operation::NotEqual:
                // Backstop: ptr vs a plain struct value can never form a valid icmp. Without this
                // the integer path below emits mismatched operands and fails module verification.
                // Fat-ptr structs are excluded - they legitimately compare by extracted field.
                if (IsPlainStructOperand(left) || IsPlainStructOperand(right))
                    LogError("cannot compare a pointer with a struct value - declare an 'operator==' whose first "
                             "parameter is the pointer type, or dereference the pointer");
                break;
            default:
                break;  // logical ops, etc. fall through to the integer path below
            }
        }

        // simd<T,N> operands: element-wise vector ops. A <N x float> answers false to the scalar
        // isFloatingPointTy() check below, so vectors must be handled before the scalar paths.
        if (left->getType()->isVectorTy() || right->getType()->isVectorTy())
            return CreateVectorOperation(op, left, right);

        // C integer promotion, scoped to the compile-time fold case (both operands
        // are constants). Widening sub-int constants to i32 stops expressions over
        // small literals (e.g. 1 << 20, 4 * 1024 * 1024) from overflowing in the
        // literals' minimal storage width. Runtime narrow arithmetic is left alone
        // so its result keeps the narrow type and still matches narrow return/storage.
        if (llvm::isa<llvm::ConstantInt>(left) && llvm::isa<llvm::ConstantInt>(right))
        {
            left = PromoteToInt(left);
            right = PromoteToInt(right);
        }

        // Upconvert both
        left = Upconvert(left, right);
        right = Upconvert(right, left);

        // Note: NSW (No Signed Wrap) and NUS(No Unsigned Wrap)
        if (left->getType()->isFloatingPointTy() || right->getType()->isFloatingPointTy())
        {
            switch (op)
            {
            case Operation::AddAssignment:
            case Operation::Add:
            {
                return builder->CreateFAdd(left, right);
            }
            case Operation::MinusAssignment:
            case Operation::Subtract:
            {
                return builder->CreateFSub(left, right);
            }
            case Operation::MultiplyAssignment:
            case Operation::Multiply:
            {
                return builder->CreateFMul(left, right);
            }
            case Operation::DivideAssignment:
            case Operation::Divide:
            {
                return builder->CreateFDiv(left, right);
            }
            case Operation::Modulo:
            {
                return builder->CreateFRem(left, right);
            }
            case Operation::Equal:
            {
                return builder->CreateFCmp(llvm::ICmpInst::FCMP_OEQ, left, right);
            }
            case Operation::NotEqual:
            {
                return builder->CreateFCmp(llvm::ICmpInst::FCMP_ONE, left, right);
            }
            case Operation::Greater:
            {
                return builder->CreateFCmp(llvm::ICmpInst::FCMP_OGT, left, right);
            }
            case Operation::GreaterEqual:
            {
                return builder->CreateFCmp(llvm::ICmpInst::FCMP_OGE, left, right);
            }
            case Operation::Less:
            {
                return builder->CreateFCmp(llvm::ICmpInst::FCMP_OLT, left, right);
            }
            case Operation::LessEqual:
            {
                return builder->CreateFCmp(llvm::ICmpInst::FCMP_OLE, left, right);
            }
            }
        }
        else
        {
            switch (op)
            {
            case Operation::AddAssignment:
            case Operation::Add:
            {
                return builder->CreateAdd(left, right);
            }
            case Operation::MinusAssignment:
            case Operation::Subtract:
            {
                return builder->CreateSub(left, right);
            }
            case Operation::MultiplyAssignment:
            case Operation::Multiply:
            {
                return builder->CreateMul(left, right);
            }
            case Operation::DivideAssignment:
            case Operation::Divide:
            {
                return builder->CreateSDiv(left, right);
            }
            case Operation::Modulo:
            {
                return builder->CreateSRem(left, right);
            }
            case Operation::Equal:
            {
                // Fat-ptr compared to nullptr: ICmp on a distinguishing field.
                // Closure fat ptr: compare fnptr (field 0); interface fat ptr: data ptr (field 1).
                if (left->getType() != right->getType() && left->getType()->isStructTy())
                {
                    auto* st = llvm::cast<llvm::StructType>(left->getType());
                    unsigned field = (st->getName() == "__closure_fat_ptr") ? 0u : 1u;
                    left  = builder->CreateExtractValue(left, {field});
                    right = llvm::Constant::getNullValue(left->getType());
                }
                return builder->CreateICmp(llvm::ICmpInst::ICMP_EQ, left, right);
            }
            case Operation::NotEqual:
            {
                if (left->getType() != right->getType() && left->getType()->isStructTy())
                {
                    auto* st = llvm::cast<llvm::StructType>(left->getType());
                    unsigned field = (st->getName() == "__closure_fat_ptr") ? 0u : 1u;
                    left  = builder->CreateExtractValue(left, {field});
                    right = llvm::Constant::getNullValue(left->getType());
                }
                return builder->CreateICmp(llvm::ICmpInst::ICMP_NE, left, right);
            }
            case Operation::Greater:
            {
                return builder->CreateICmp(llvm::ICmpInst::ICMP_SGT, left, right);
            }
            case Operation::GreaterEqual:
            {
                return builder->CreateICmp(llvm::ICmpInst::ICMP_SGE, left, right);
            }
            case Operation::Less:
            {
                return builder->CreateICmp(llvm::ICmpInst::ICMP_SLT, left, right);
            }
            case Operation::LessEqual:
            {
                return builder->CreateICmp(llvm::ICmpInst::ICMP_SLE, left, right);
            }
            case Operation::LogicalAnd:
            {
                // Normalize to i1 so AND behaves logically. isdigit() returns 4/8, which
                // would AND to 0 against an i1-widened left side (1 & 4 == 0).
                if (!left->getType()->isIntegerTy(1))
                    left = builder->CreateICmpNE(left, llvm::Constant::getNullValue(left->getType()), "tobool");
                if (!right->getType()->isIntegerTy(1))
                    right = builder->CreateICmpNE(right, llvm::Constant::getNullValue(right->getType()), "tobool");
                return builder->CreateAnd(left, right);
            }
            case Operation::BitwiseAnd:
            case Operation::AndAssignment:
            {
                return builder->CreateAnd(left, right);
            }
            case Operation::LogicalOr:
            {
                if (!left->getType()->isIntegerTy(1))
                    left = builder->CreateICmpNE(left, llvm::Constant::getNullValue(left->getType()), "tobool");
                if (!right->getType()->isIntegerTy(1))
                    right = builder->CreateICmpNE(right, llvm::Constant::getNullValue(right->getType()), "tobool");
                return builder->CreateOr(left, right);
            }
            case Operation::BitwiseOr:
            case Operation::OrAssignment:
            {
                return builder->CreateOr(left, right);
            }
            case Operation::BitwiseXor:
            case Operation::XorAssignment:
                return builder->CreateXor(left, right);
            case Operation::ShiftLeft:
            case Operation::LeftShiftAssignment:
                return builder->CreateShl(left, right);
            case Operation::ShiftRight:
            case Operation::RightShiftAssignment:
                return builder->CreateAShr(left, right);
            }
        }

        LogError(std::format("unhandled operation {}", static_cast<int>(op)));
        return right;
    }

llvm::Value* LLVMBackend::CreateOperation(Operation op, llvm::Value* left, llvm::Value* right,
                                  bool leftIsUnsigned, bool rightIsUnsigned)
{
        if (left == nullptr)
            return right;

        // Delegate pointer operations before Upconvert can corrupt pointer/int pairs.
        if (left->getType()->isPointerTy() || right->getType()->isPointerTy())
            return CreateOperation(op, left, right);

        // simd<T,N> operands: element-wise ops. A <N x iX>/<N x float> answers false to the scalar
        // isFloatingPointTy() check below, so vectors must be handled before the integer switch.
        if (left->getType()->isVectorTy() || right->getType()->isVectorTy())
            return CreateVectorOperation(op, left, right, leftIsUnsigned, rightIsUnsigned);

        // C integer promotion for compile-time constants only (see PromoteToInt).
        // Signedness preserved so unsigned narrow constants zero-extend.
        if (llvm::isa<llvm::ConstantInt>(left) && llvm::isa<llvm::ConstantInt>(right))
        {
            left  = PromoteToInt(left,  leftIsUnsigned);
            right = PromoteToInt(right, rightIsUnsigned);
        }

        left  = Upconvert(left,  right, leftIsUnsigned);
        right = Upconvert(right, left,  rightIsUnsigned);

        bool anyUnsigned = leftIsUnsigned || rightIsUnsigned;

        if (left->getType()->isFloatingPointTy() || right->getType()->isFloatingPointTy())
            return CreateOperation(op, left, right);

        switch (op)
        {
        case Operation::DivideAssignment:
        case Operation::Divide:
            return anyUnsigned ? builder->CreateUDiv(left, right) : builder->CreateSDiv(left, right);
        case Operation::Modulo:
            return anyUnsigned ? builder->CreateURem(left, right) : builder->CreateSRem(left, right);
        case Operation::Greater:
            return builder->CreateICmp(anyUnsigned ? llvm::ICmpInst::ICMP_UGT : llvm::ICmpInst::ICMP_SGT, left, right);
        case Operation::GreaterEqual:
            return builder->CreateICmp(anyUnsigned ? llvm::ICmpInst::ICMP_UGE : llvm::ICmpInst::ICMP_SGE, left, right);
        case Operation::Less:
            return builder->CreateICmp(anyUnsigned ? llvm::ICmpInst::ICMP_ULT : llvm::ICmpInst::ICMP_SLT, left, right);
        case Operation::LessEqual:
            return builder->CreateICmp(anyUnsigned ? llvm::ICmpInst::ICMP_ULE : llvm::ICmpInst::ICMP_SLE, left, right);
        case Operation::ShiftRight:
        case Operation::RightShiftAssignment:
            return anyUnsigned ? builder->CreateLShr(left, right) : builder->CreateAShr(left, right);
        default:
            return CreateOperation(op, left, right);
        }
    }

llvm::Value* LLVMBackend::CreateOperation(std::string oper, llvm::Value* left, llvm::Value* right,
                                  bool leftIsUnsigned, bool rightIsUnsigned)
{
        if (left == nullptr)
            return right;
        Operation op = ParseOperation(oper);
        return CreateOperation(op, left, right, leftIsUnsigned, rightIsUnsigned);
    }

llvm::Value* LLVMBackend::CreateNot(llvm::Value* value)
{
        return builder->CreateNot(value);
    }

llvm::Value* LLVMBackend::CreateLogicalNot(llvm::Value* value)
{
        auto* type = value->getType();
        if (type->isIntegerTy(1))
            return builder->CreateNot(value);
        if (type->isPointerTy())
            return builder->CreateIsNull(value);
        if (type->isFloatingPointTy())
            return builder->CreateFCmpOEQ(value, llvm::ConstantFP::get(type, 0.0));
        if (type->isIntegerTy())
            return builder->CreateICmpEQ(value, llvm::ConstantInt::get(type, 0));
        return builder->CreateNot(value);
    }

llvm::Value* LLVMBackend::CreateNeg(llvm::Value* value)
{
        if (value->getType()->isFloatingPointTy())
            return builder->CreateFNeg(value);
        return builder->CreateNeg(value);
    }
