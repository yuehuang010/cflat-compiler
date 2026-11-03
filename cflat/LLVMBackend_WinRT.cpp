#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include <limits>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Instructions.h>
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

// ---- Definitions moved out of LLVMBackend.h (WinRT) ----

bool LLVMBackend::IsWinrtClass(const std::string& name) const
{ return winrtClasses.count(name) > 0; }

bool LLVMBackend::ParseUuidToBytes(const std::string& text, uint8_t out[16])
{
        uint8_t textBytes[16] = {};
        int count = 0;
        for (char c : text)
        {
            int v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else continue;
            if (count >= 32) break;
            int bi = count / 2;
            if ((count & 1) == 0) textBytes[bi] = (uint8_t)(v << 4);
            else                  textBytes[bi] = (uint8_t)(textBytes[bi] | v);
            count++;
        }
        if (count < 32) return false;
        // Data1 (u32) and Data2/Data3 (u16) are stored little-endian; Data4 is raw byte order.
        out[0] = textBytes[3]; out[1] = textBytes[2]; out[2] = textBytes[1]; out[3] = textBytes[0];
        out[4] = textBytes[5]; out[5] = textBytes[4];
        out[6] = textBytes[7]; out[7] = textBytes[6];
        for (int i = 8; i < 16; i++) out[i] = textBytes[i];
        return true;
    }

llvm::GlobalVariable* LLVMBackend::EmitIidGlobalFor(const std::string& name)
{
        if (auto it = winrtInstanceIid_.find(name); it != winrtInstanceIid_.end())
            return EmitGuidGlobal(it->second.data());
        uint8_t bytes[16];
        // A header-COM interface is often a typedef alias of its real tag (ID3DBlob -> ID3D10Blob);
        // the uuid annotation lives on the tag, so resolve the alias chain before giving up.
        if (std::string u = FindUuidAnnotationResolving(name); !u.empty() && ParseUuidToBytes(u, bytes))
            return EmitGuidGlobal(bytes);
        for (const auto& i : winrtConsumedModel_.interfaces)
            if (i.fullName == name && !i.iid.empty() && ParseUuidToBytes(i.iid, bytes))
                return EmitGuidGlobal(bytes);
        return nullptr;
    }

llvm::GlobalVariable* LLVMBackend::EmitGuidGlobal(const uint8_t bytes[16])
{
        std::string name = "__winrt_iid_";
        for (int i = 0; i < 16; i++)
        {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", bytes[i]);
            name += buf;
        }
        if (auto* existing = module->getNamedGlobal(name)) return existing;

        std::vector<llvm::Constant*> elems;
        for (int i = 0; i < 16; i++) elems.push_back(builder->getInt8(bytes[i]));
        auto* arrTy = llvm::ArrayType::get(builder->getInt8Ty(), 16);
        auto* init = llvm::ConstantArray::get(arrTy, elems);
        return new llvm::GlobalVariable(*module, arrTy, true,
            llvm::GlobalValue::InternalLinkage, init, name);
    }

LLVMBackend::DeclTypeAndValue LLVMBackend::MakeWinrtSlot(const std::string& name, const std::string& retType,
        bool retPtr, std::vector<TypeAndValue::FuncPtrParam> params)
{
        DeclTypeAndValue f;
        f.VariableName = name;
        f.TypeName = "__c_fn_ptr";
        f.IsFunctionPointer = true;
        f.FuncPtrReturnTypeName = retType;
        f.FuncPtrReturnPointer = retPtr;
        f.FuncPtrParams = std::move(params);
        return f;
    }

std::string LLVMBackend::CreateWinrtVtableStruct(const std::string& className, const std::string& ifaceName)
{
        std::string vtblName = className + "__" + ifaceName + "_comvtbl";
        if (!dataStructures.count(vtblName))
        {
            auto vp = []() { TypeAndValue::FuncPtrParam p; p.TypeName = "void"; p.Pointer = true; return p; };
            std::vector<DeclTypeAndValue> slots;
            slots.push_back(MakeWinrtSlot("QueryInterface", "i32", false, { vp(), vp(), vp() }));
            slots.push_back(MakeWinrtSlot("AddRef", "u32", false, { vp() }));
            slots.push_back(MakeWinrtSlot("Release", "u32", false, { vp() }));
            slots.push_back(MakeWinrtSlot("GetIids", "i32", false, { vp(), vp(), vp() }));
            slots.push_back(MakeWinrtSlot("GetRuntimeClassName", "i32", false, { vp(), vp() }));
            slots.push_back(MakeWinrtSlot("GetTrustLevel", "i32", false, { vp(), vp() }));

            const auto* methods = FindInterface(ifaceName);
            if (methods != nullptr)
            {
                for (const auto& m : *methods)
                {
                    std::vector<TypeAndValue::FuncPtrParam> params = { vp() };  // this
                    for (const auto& mp : m.Parameters)
                    {
                        TypeAndValue::FuncPtrParam p;
                        p.TypeName = mp.TypeName;
                        p.Pointer = mp.Pointer;
                        p.PointerDepth = mp.PointerDepth;
                        params.push_back(p);
                    }
                    // WinRT ABI: the slot returns HRESULT (i32); a non-void logical return is passed
                    // back through a trailing [out,retval] pointer (opaque void* in the slot type).
                    bool voidRet = (m.ReturnType.TypeName == "void" && !m.ReturnType.Pointer);
                    if (!voidRet) params.push_back(vp());
                    slots.push_back(MakeWinrtSlot(m.Name, "i32", false, params));
                }
            }

            CreateStructType(vtblName, slots);
        }

        auto* vtblTy = dataStructures.at(vtblName).StructType;
        std::string globalName = "__winrt_" + className + "_vtbl";
        auto* vtblGlobal = module->getNamedGlobal(globalName);
        if (!vtblGlobal)
            vtblGlobal = new llvm::GlobalVariable(*module, vtblTy, true,
                llvm::GlobalValue::InternalLinkage, nullptr, globalName);
        winrtClasses[className] = WinrtClassInfo{ ifaceName, vtblTy, vtblGlobal };
        return vtblName;
    }

void LLVMBackend::PrepareWinrtClass(const std::string& className, const std::string& ifaceName)
{
        CreateWinrtVtableStruct(className, ifaceName);
    }

const LLVMBackend::FunctionSymbol* LLVMBackend::FindWinrtMethod(const std::string& className, const InterfaceMethod& m)
{
        auto it = functionTable.find(m.Name);
        if (it == functionTable.end()) return nullptr;
        size_t expected = 1 + m.Parameters.size();
        for (const auto& sym : it->second)
        {
            if (sym.Parameters.size() != expected) continue;
            if (sym.Parameters[0].TypeName != className || !sym.Parameters[0].Pointer) continue;
            bool ok = true;
            for (size_t pi = 0; pi < m.Parameters.size(); pi++)
                if (sym.Parameters[1 + pi].TypeName != m.Parameters[pi].TypeName
                    || sym.Parameters[1 + pi].ValuePointerDepth()
                        != m.Parameters[pi].ValuePointerDepth()) { ok = false; break; }
            if (ok) return &sym;
        }
        return nullptr;
    }

bool LLVMBackend::IsHResultType(const std::string& typeName)
{
        return MangledBase(typeName) != typeName && MangledBase(typeName) == "HResult";
    }

void LLVMBackend::WireWinrtObject(llvm::Value* objPtr, const std::string& className)
{
        auto wi = winrtClasses.at(className);
        auto* objTy = dataStructures[className].StructType;
        auto* vtblFieldPtr = builder->CreateStructGEP(objTy, objPtr, 0, "lpVtbl");
        auto* vtblPtrTy = objTy->getStructElementType(0);
        builder->CreateStore(builder->CreateBitCast(wi.VtableInstance, vtblPtrTy), vtblFieldPtr);
        auto* rcFieldPtr = builder->CreateStructGEP(objTy, objPtr, 1, "__refcount");
        builder->CreateStore(builder->getInt32(1), rcFieldPtr);
    }

const LLVMBackend::DeclTypeAndValue* LLVMBackend::GetWinrtSlot(const std::string& className, const std::string& slotName) const
{
        auto wi = winrtClasses.find(className);
        if (wi == winrtClasses.end()) return nullptr;
        std::string vtblName = className + "__" + wi->second.InterfaceName + "_comvtbl";
        auto ds = dataStructures.find(vtblName);
        if (ds == dataStructures.end()) return nullptr;
        for (const auto& f : ds->second.StructFields)
            if (f.VariableName == slotName) return &f;
        return nullptr;
    }

llvm::Value* LLVMBackend::EmitWinrtSlotCall(const std::string& className, const std::string& slotName,
        const std::vector<llvm::Value*>& argVals, std::string& outResultType, bool& outResultPtr)
{
        auto wi = winrtClasses.at(className);
        auto* objTy = dataStructures[className].StructType;
        auto* vtblTy = wi.VtableType;
        std::string vtblName = className + "__" + wi.InterfaceName + "_comvtbl";
        auto& vtblSD = dataStructures[vtblName];

        unsigned slotIdx = 0;
        const DeclTypeAndValue* slot = nullptr;
        for (unsigned i = 0; i < vtblSD.StructFields.size(); i++)
            if (vtblSD.StructFields[i].VariableName == slotName) { slotIdx = i; slot = &vtblSD.StructFields[i]; break; }
        if (!slot || argVals.empty())
        {
            LogErrorMessage("EmitWinrtSlotCall: bad slot '{}' on '{}'",
                { slotName, SpellType(*this, TypeAndValue{ .TypeName = className }) });
            return nullptr;
        }

        // A value-returning interface method: build the HResult<T> wrapper around the ABI call.
        const InterfaceMethod* im = nullptr;
        if (slotIdx >= 6)
            if (const auto* methods = FindInterface(wi.InterfaceName))
                for (const auto& m : *methods) if (m.Name == slotName) { im = &m; break; }
        bool nonVoidIface = im && !(im->ReturnType.TypeName == "void" && !im->ReturnType.Pointer);

        llvm::Value* retvalAlloca = nullptr;
        std::string hresultTypeName;
        if (nonVoidIface)
        {
            auto rt = winrtSlotHResultType_.find(className + "::" + slotName);
            if (rt == winrtSlotHResultType_.end() || !dataStructures.count(rt->second))
            {
                LogErrorMessage("[winrt] '{}::{}' sugar needs HResult<{}> instantiated "
                    "(import \"com.cb\")", { SpellType(*this, TypeAndValue{ .TypeName = className }),
                    slotName, SpellType(*this, im->ReturnType) });
                return nullptr;
            }
            hresultTypeName = rt->second;
            auto* retElemTy = GetType(im->ReturnType);
            retvalAlloca = AllocaAtEntry(retElemTy, nullptr, "winrt.retval");
        }

        std::vector<llvm::Value*> callArgs = argVals;
        if (nonVoidIface)
            callArgs.push_back(builder->CreateBitCast(retvalAlloca, cflat_llvm::PointerTo(builder->getInt8Ty())));

        auto* objPtr = builder->CreateBitCast(argVals[0], cflat_llvm::PointerTo(objTy));
        auto* vtblFieldPtr = builder->CreateStructGEP(objTy, objPtr, 0);
        auto* vtblPtr = builder->CreateLoad(cflat_llvm::PointerTo(vtblTy), vtblFieldPtr);
        auto* slotPtr = builder->CreateStructGEP(vtblTy, vtblPtr, slotIdx);
        auto* fnPtr = builder->CreateLoad(BuildThinFnPtrType(*slot), slotPtr);
        llvm::Value* callRes = CreateIndirectCall(*slot, fnPtr, callArgs);

        if (!nonVoidIface)
        {
            // Infra method or void interface method: the raw return (slot's declared type).
            outResultType = slot->FuncPtrReturnTypeName;
            outResultPtr = slot->FuncPtrReturnPointer;
            return callRes;
        }

        // Package {hr = callRes, value = *retval} into the primed HResult<T>.
        auto* hrTy = dataStructures[hresultTypeName].StructType;
        auto* hrAlloca = AllocaAtEntry(hrTy, nullptr, "winrt.hr");
        builder->CreateStore(callRes, builder->CreateStructGEP(hrTy, hrAlloca, 0));
        auto* loadedVal = builder->CreateLoad(GetType(im->ReturnType), retvalAlloca);
        builder->CreateStore(loadedVal, builder->CreateStructGEP(hrTy, hrAlloca, 1));
        outResultType = hresultTypeName;
        outResultPtr = false;
        return builder->CreateLoad(hrTy, hrAlloca);
    }

void LLVMBackend::EmitWinrtRuntime(const std::string& className, const std::string& ifaceName,
        const std::string& vtblName)
{
        auto* objTy = dataStructures[className].StructType;
        auto* vtblTy = dataStructures[vtblName].StructType;
        auto* i8PtrTy = cflat_llvm::PointerTo(builder->getInt8Ty());
        auto* i32Ty = builder->getInt32Ty();

        // GUID constants: IUnknown, IInspectable, IAgileObject (every free-threaded WinRT object
        // answers to it), and this class's interface IID.
        uint8_t unkB[16], inspB[16], agileB[16], ifaceB[16];
        ParseUuidToBytes("00000000-0000-0000-C000-000000000046", unkB);
        ParseUuidToBytes("AF86E2E0-B12D-4C6A-9C5A-D7AA65101E90", inspB);
        ParseUuidToBytes("94EA2B94-E9CC-49E0-C0FF-EE64CA8F5B90", agileB);
        std::string ifaceUuid = GetTypeAnnotationArg(ifaceName, "uuid");
        bool haveIfaceIid = ParseUuidToBytes(ifaceUuid, ifaceB);
        auto* gUnk = EmitGuidGlobal(unkB);
        auto* gInsp = EmitGuidGlobal(inspB);
        auto* gAgile = EmitGuidGlobal(agileB);
        auto* gIface = haveIfaceIid ? EmitGuidGlobal(ifaceB) : nullptr;

        auto makeFn = [&](const std::string& nm, llvm::FunctionType* ty) {
            return llvm::Function::Create(ty, llvm::Function::InternalLinkage,
                "__winrt_" + className + "_" + nm, module.get());
        };
        std::string prefix = "__winrt_" + className + "_";

        // QueryInterface(i8* self, i8* riid, i8* ppv) -> i32
        auto* qiTy = llvm::FunctionType::get(i32Ty, { i8PtrTy, i8PtrTy, i8PtrTy }, false);
        auto* qiFn = makeFn("QueryInterface", qiTy);
        {
            auto* entry = llvm::BasicBlock::Create(*context, "entry", qiFn);
            auto* matchBB = llvm::BasicBlock::Create(*context, "match", qiFn);
            auto* noBB = llvm::BasicBlock::Create(*context, "nomatch", qiFn);
            llvm::IRBuilder<> b(entry);
            auto* self = qiFn->getArg(0);
            auto* riid = qiFn->getArg(1);
            auto* ppv = qiFn->getArg(2);

            // Compare two 8-byte halves of the 16-byte GUID (unaligned loads are fine on x64).
            auto guidEq = [&](llvm::Value* a, llvm::GlobalVariable* g) -> llvm::Value* {
                auto* i64Ty = b.getInt64Ty();
                auto* i64PtrTy = cflat_llvm::PointerTo(i64Ty);
                auto* aLo = b.CreateAlignedLoad(i64Ty, b.CreateBitCast(a, i64PtrTy), llvm::MaybeAlign(1));
                auto* aHiPtr = b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), a, 8);
                auto* aHi = b.CreateAlignedLoad(i64Ty, b.CreateBitCast(aHiPtr, i64PtrTy), llvm::MaybeAlign(1));
                auto* gI8 = b.CreateBitCast(g, i8PtrTy);
                auto* bLo = b.CreateAlignedLoad(i64Ty, b.CreateBitCast(gI8, i64PtrTy), llvm::MaybeAlign(1));
                auto* bHiPtr = b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), gI8, 8);
                auto* bHi = b.CreateAlignedLoad(i64Ty, b.CreateBitCast(bHiPtr, i64PtrTy), llvm::MaybeAlign(1));
                return b.CreateAnd(b.CreateICmpEQ(aLo, bLo), b.CreateICmpEQ(aHi, bHi));
            };

            llvm::Value* match = b.CreateOr(guidEq(riid, gUnk), guidEq(riid, gInsp));
            match = b.CreateOr(match, guidEq(riid, gAgile));
            if (gIface) match = b.CreateOr(match, guidEq(riid, gIface));
            b.CreateCondBr(match, matchBB, noBB);

            b.SetInsertPoint(matchBB);
            auto* objP = b.CreateBitCast(self, cflat_llvm::PointerTo(objTy));
            auto* rcP = b.CreateStructGEP(objTy, objP, 1);
            b.CreateAtomicRMW(llvm::AtomicRMWInst::Add, rcP, b.getInt32(1),
                llvm::MaybeAlign(), llvm::AtomicOrdering::Monotonic);
            b.CreateStore(self, b.CreateBitCast(ppv, cflat_llvm::PointerTo(i8PtrTy)));
            b.CreateRet(b.getInt32(0));

            b.SetInsertPoint(noBB);
            b.CreateStore(llvm::ConstantPointerNull::get(i8PtrTy),
                b.CreateBitCast(ppv, cflat_llvm::PointerTo(i8PtrTy)));
            b.CreateRet(b.getInt32((uint32_t)0x80004002));  // E_NOINTERFACE
        }

        // AddRef(i8* self) -> i32
        auto* refTy = llvm::FunctionType::get(i32Ty, { i8PtrTy }, false);
        auto* addRefFn = makeFn("AddRef", refTy);
        {
            auto* entry = llvm::BasicBlock::Create(*context, "entry", addRefFn);
            llvm::IRBuilder<> b(entry);
            auto* objP = b.CreateBitCast(addRefFn->getArg(0), cflat_llvm::PointerTo(objTy));
            auto* rcP = b.CreateStructGEP(objTy, objP, 1);
            auto* old = b.CreateAtomicRMW(llvm::AtomicRMWInst::Add, rcP, b.getInt32(1),
                llvm::MaybeAlign(), llvm::AtomicOrdering::Monotonic);
            b.CreateRet(b.CreateAdd(old, b.getInt32(1)));
        }

        // Release(i8* self) -> i32: atomic dec; free at zero (full dtor + operator delete).
        auto* releaseFn = makeFn("Release", refTy);
        {
            auto* entry = llvm::BasicBlock::Create(*context, "entry", releaseFn);
            auto* freeBB = llvm::BasicBlock::Create(*context, "free", releaseFn);
            auto* doneBB = llvm::BasicBlock::Create(*context, "done", releaseFn);
            llvm::IRBuilder<> b(entry);
            auto* self = releaseFn->getArg(0);
            auto* objP = b.CreateBitCast(self, cflat_llvm::PointerTo(objTy));
            auto* rcP = b.CreateStructGEP(objTy, objP, 1);
            auto* old = b.CreateAtomicRMW(llvm::AtomicRMWInst::Sub, rcP, b.getInt32(1),
                llvm::MaybeAlign(), llvm::AtomicOrdering::Monotonic);
            auto* nw = b.CreateSub(old, b.getInt32(1));
            b.CreateCondBr(b.CreateICmpEQ(nw, b.getInt32(0)), freeBB, doneBB);

            b.SetInsertPoint(freeBB);
            if (auto* dtor = GetOrCreateFullDestructor(className))
                b.CreateCall(dtor->getFunctionType(), dtor,
                    { b.CreateBitCast(self, cflat_llvm::PointerTo(objTy)) });
            if (auto* del = GetFunction("operator delete"))
                b.CreateCall(del->getFunctionType(), del,
                    { b.CreateBitCast(self, del->getArg(0)->getType()) });
            b.CreateBr(doneBB);

            b.SetInsertPoint(doneBB);
            b.CreateRet(nw);
        }

        // IInspectable stubs: store null/zero out-params and return S_OK.
        auto emitInspStub = [&](const std::string& nm, unsigned outParams) {
            std::vector<llvm::Type*> ps(1 + outParams, i8PtrTy);
            auto* ty = llvm::FunctionType::get(i32Ty, ps, false);
            auto* fn = makeFn(nm, ty);
            auto* entry = llvm::BasicBlock::Create(*context, "entry", fn);
            llvm::IRBuilder<> b(entry);
            b.CreateRet(b.getInt32(0));
            return fn;
        };
        auto* getIidsFn = emitInspStub("GetIids", 2);
        auto* getNameFn = emitInspStub("GetRuntimeClassName", 1);
        auto* getTrustFn = emitInspStub("GetTrustLevel", 1);

        // Per-method thunks: bitcast i8* self to the object pointer and forward to the member fn.
        std::vector<llvm::Function*> thunks;
        const auto* methods = FindInterface(ifaceName);
        if (methods != nullptr)
        {
            for (const auto& m : *methods)
            {
                const FunctionSymbol* implSym = FindWinrtMethod(className, m);
                if (!implSym)
                {
                    LogErrorMessage("[winrt] class '{}' does not implement '{}::{}'",
                        { SpellType(*this, TypeAndValue{ .TypeName = className }),
                          SpellType(*this, TypeAndValue{ .TypeName = ifaceName }), m.Name });
                    thunks.push_back(nullptr);
                    continue;
                }
                llvm::Function* impl = implSym->Function;
                auto* implTy = impl->getFunctionType();
                bool voidRet = (m.ReturnType.TypeName == "void" && !m.ReturnType.Pointer);
                bool implHResult = IsHResultType(implSym->ReturnType.TypeName);

                // A struct returned via the sret ABI (hidden pointer param) is not yet handled
                // by the wrapping thunk; diagnose rather than miscompile.
                if (impl->hasStructRetAttr())
                {
                    LogErrorMessage("[winrt] '{}::{}' returns a struct via the sret ABI, "
                        "which the WinRT thunk does not support yet",
                        { SpellType(*this, TypeAndValue{ .TypeName = className }), m.Name });
                    thunks.push_back(nullptr);
                    continue;
                }

                // WinRT thunk: i32 (i8* this, <in-params>, [RetType* retval]). It forwards to the
                // user method and adapts the result to (HRESULT, *retval).
                std::vector<llvm::Type*> ps;
                ps.push_back(i8PtrTy);  // this
                for (unsigned i = 1; i < implTy->getNumParams(); i++)
                    ps.push_back(implTy->getParamType(i));
                if (!voidRet) ps.push_back(i8PtrTy);  // retval out-pointer
                auto* ty = llvm::FunctionType::get(i32Ty, ps, false);
                auto* fn = makeFn(m.Name, ty);
                auto* entry = llvm::BasicBlock::Create(*context, "entry", fn);
                llvm::IRBuilder<> b(entry);

                std::vector<llvm::Value*> args;
                args.push_back(b.CreateBitCast(fn->getArg(0), implTy->getParamType(0)));
                for (unsigned i = 1; i < implTy->getNumParams(); i++) args.push_back(fn->getArg(i));
                auto* call = b.CreateCall(implTy, impl, args);

                if (voidRet)
                {
                    // void logical return: forward the impl's hr if it is HResult<void>, else S_OK.
                    b.CreateRet(implHResult ? b.CreateExtractValue(call, { 0u }) : b.getInt32(0));
                }
                else
                {
                    llvm::Value* hr;
                    llvm::Value* val;
                    if (implHResult) { hr = b.CreateExtractValue(call, { 0u }); val = b.CreateExtractValue(call, { 1u }); }
                    else             { hr = b.getInt32(0); val = call; }
                    auto* retPtr = b.CreateBitCast(fn->getArg(static_cast<unsigned>(fn->arg_size() - 1)), cflat_llvm::PointerTo(val->getType()));
                    b.CreateStore(val, retPtr);
                    b.CreateRet(hr);
                }
                thunks.push_back(fn);
            }
        }

        // Static vtable instance: a ConstantStruct of the generated functions, each bitcast to
        // the exact thin-fn-ptr type of its slot field.
        std::vector<llvm::Function*> slotFns = {
            qiFn, addRefFn, releaseFn, getIidsFn, getNameFn, getTrustFn };
        for (auto* t : thunks) slotFns.push_back(t);

        const auto& slotFields = dataStructures[vtblName].StructFields;
        std::vector<llvm::Constant*> entries;
        for (size_t i = 0; i < slotFields.size(); i++)
        {
            auto* slotTy = GetType(slotFields[i]);
            llvm::Constant* fnC = slotFns[i]
                ? (llvm::Constant*)llvm::ConstantExpr::getBitCast(slotFns[i], slotTy)
                : llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(slotTy));
            entries.push_back(fnC);
        }
        auto* init = llvm::ConstantStruct::get(vtblTy, entries);
        auto* vtblGlobal = module->getNamedGlobal(prefix + "vtbl");
        if (!vtblGlobal)
            vtblGlobal = new llvm::GlobalVariable(*module, vtblTy, true,
                llvm::GlobalValue::InternalLinkage, nullptr, prefix + "vtbl");
        vtblGlobal->setInitializer(init);

        winrtClasses[className] = WinrtClassInfo{ ifaceName, vtblTy, vtblGlobal };
    }

std::string LLVMBackend::WinrtSimpleName(const std::string& fullName)
{
        auto dot = fullName.rfind('.');
        return dot == std::string::npos ? fullName : fullName.substr(dot + 1);
    }

void LLVMBackend::MapWinrtTypeForSlot(const cflat_winmd::TypeRef& t, std::string& outName, bool& outPtr)
{
        if (t.pointerDepth > 0 || t.isArray || t.isGenericVar || !t.genericArgs.empty())
        {
            outName = "void"; outPtr = true; return;
        }
        std::string fund = cflat_winmd::WinrtFundamentalToCFlat(t.fullName);
        if (!fund.empty())
        {
            if (fund == "string" || fund == "object") { outName = "void"; outPtr = true; return; }
            // Guid maps to core/guid.cb's 16-byte `Guid` by value when that type is in scope;
            // without it (guid.cb not imported) degrade to an opaque pointer (REFGUID-style).
            if (fund == "Guid" && !dataStructures.count("Guid")) { outName = "void"; outPtr = true; return; }
            // The typeMap yields user-facing CFlat spellings; the backend's direct type
            // resolution uses internal names for floats (i32/u32/... already match).
            if (fund == "f32") fund = "float";
            else if (fund == "f64") fund = "double";
            outName = fund; outPtr = false; return;
        }
        if (auto e = winrtEnumUnderlying_.find(t.fullName); e != winrtEnumUnderlying_.end())
        {
            outName = e->second; outPtr = false; return;
        }
        if (winrtValueStructs_.count(t.fullName) && dataStructures.count(t.fullName))
        {
            outName = t.fullName; outPtr = false; return;
        }
        outName = "void"; outPtr = true;   // interface / class / forward -> opaque thin pointer
    }

bool LLVMBackend::WinrtRequiresReaches(const std::string& fullName,
        const std::unordered_map<std::string, const cflat_winmd::Interface*>& byName,
        const char* target, std::set<std::string>& seen)
{
        auto it = byName.find(fullName);
        if (it == byName.end()) return false;
        for (const std::string& req : it->second->requires_)
        {
            if (WinrtSimpleName(req) == target) return true;
            if (seen.insert(req).second && WinrtRequiresReaches(req, byName, target, seen)) return true;
        }
        return false;
    }

void LLVMBackend::CollectComBaseMethods(const cflat_winmd::Interface& iface,
        const std::unordered_map<std::string, const cflat_winmd::Interface*>& byName,
        std::set<std::string>& seen, std::vector<cflat_winmd::Method>& out)
{
        for (const std::string& req : iface.requires_)
        {
            std::string rs = WinrtSimpleName(req);
            if (rs == "IUnknown" || rs == "IInspectable") continue;
            if (!seen.insert(req).second) continue;
            auto it = byName.find(req);
            if (it != byName.end()) CollectComBaseMethods(*it->second, byName, seen, out);
        }
        for (const cflat_winmd::Method& m : iface.methods) out.push_back(m);
    }

bool LLVMBackend::BuildWinrtInterfaceStructs(const std::string& thinName,
        const std::vector<cflat_winmd::Method>& methods, const std::string& lspDesc,
        const std::string& fileForLsp, bool inspectable)
{
        std::string vtblName = thinName + "Vtbl";
        // A forward/opaque shell for the thin pointer may already exist (created by type
        // resolution); fill it rather than bail. Only skip when it is already fully built.
        auto filled = [&](const std::string& n) {
            auto it = dataStructures.find(n);
            return it != dataStructures.end() && !it->second.StructFields.empty();
        };
        if (filled(thinName)) return false;

        auto vp = []() { TypeAndValue::FuncPtrParam p; p.TypeName = "void"; p.Pointer = true; return p; };
        std::vector<DeclTypeAndValue> slots;
        slots.push_back(MakeWinrtSlot("QueryInterface", "i32", false, { vp(), vp(), vp() }));
        slots.push_back(MakeWinrtSlot("AddRef", "u32", false, { vp() }));
        slots.push_back(MakeWinrtSlot("Release", "u32", false, { vp() }));
        if (inspectable)
        {
            slots.push_back(MakeWinrtSlot("GetIids", "i32", false, { vp(), vp(), vp() }));
            slots.push_back(MakeWinrtSlot("GetRuntimeClassName", "i32", false, { vp(), vp() }));
            slots.push_back(MakeWinrtSlot("GetTrustLevel", "i32", false, { vp(), vp() }));
        }
        for (const cflat_winmd::Method& m : methods)
        {
            std::vector<TypeAndValue::FuncPtrParam> params = { vp() };   // this
            for (const cflat_winmd::Param& mp : m.params)
            {
                TypeAndValue::FuncPtrParam p;
                MapWinrtTypeForSlot(mp.type, p.TypeName, p.Pointer);
                params.push_back(p);
            }
            // A non-void logical return is passed back through a trailing [out,retval] pointer
            // ONLY for the WinRT implicit-HRESULT projection. Raw-COM methods (Win32 metadata)
            // return HRESULT explicitly and already list every out-param, so nothing is appended.
            bool voidRet = (m.returnType.fullName == "Void" && m.returnType.pointerDepth == 0);
            if (m.hresultImplicit && !voidRet) params.push_back(vp());
            slots.push_back(MakeWinrtSlot(m.name, "i32", false, params));
        }
        if (!filled(vtblName)) CreateStructType(vtblName, slots);

        DeclTypeAndValue lp;
        lp.VariableName = "lpVtbl";
        lp.TypeName = vtblName;
        lp.Pointer = true;
        CreateStructType(thinName, { lp });
        winrtThinInterfaces_.insert(thinName);
        if (auto* s = GetSymbolSink())
        {
            TypeAndValue thinType;
            thinType.TypeName = thinName;
            const std::string displayThinName = SpellType(*this, thinType);
            s->Register(SymbolKind::Struct, thinName, fileForLsp, 0, 0, lspDesc,
                        {}, {}, displayThinName);
            // Register each real interface method as a "<Interface>.<Method>" member so --symbol
            // and dot-completion expose the WinRT call surface. `methods` is already the flattened
            // vtable-order list with the synthetic IUnknown/IInspectable base slots excluded, so
            // QueryInterface/AddRef/Release never appear here. The signature uses the logical WinRT
            // spelling (declared return + in/out params), matching how the docs describe the call.
            for (const cflat_winmd::Method& m : methods)
            {
                std::string sig = m.returnType.Spelling() + " " + m.name + "(";
                bool first = true;
                for (const cflat_winmd::Param& p : m.params)
                {
                    if (!first) sig += ", ";
                    first = false;
                    if (p.dir == cflat_winmd::ParamDir::Out) sig += "out ";
                    sig += p.type.Spelling();
                    if (!p.name.empty()) sig += " " + p.name;
                }
                sig += ")";
                s->Register(SymbolKind::Function, thinName + "." + m.name, fileForLsp, 0, 0, sig,
                            {}, {}, displayThinName + "." + m.name);
            }
        }
        return true;
    }

bool LLVMBackend::IsWinrtThinInterface(const std::string& name) const
{
        return winrtThinInterfaces_.count(name) != 0;
    }

llvm::Value* LLVMBackend::EmitWinrtThinSlotCall(llvm::Value* objPtr, const std::string& thinName,
        const std::string& slotName, const std::vector<llvm::Value*>& extraArgs)
{
        auto dsIt = dataStructures.find(thinName);
        std::string vtblName = thinName + "Vtbl";
        auto vtblIt = dataStructures.find(vtblName);
        if (dsIt == dataStructures.end() || vtblIt == dataStructures.end())
        {
            LogErrorMessage("EmitWinrtThinSlotCall: '{}' is not a built WinRT interface",
                { SpellType(*this, TypeAndValue{ .TypeName = thinName }) });
            return nullptr;
        }
        auto* objTy = dsIt->second.StructType;     // { <vtbl>* }
        auto* vtblTy = vtblIt->second.StructType;
        const auto& slots = vtblIt->second.StructFields;

        unsigned slotIdx = 0;
        const DeclTypeAndValue* slot = nullptr;
        for (unsigned i = 0; i < slots.size(); i++)
            if (slots[i].VariableName == slotName) { slotIdx = i; slot = &slots[i]; break; }
        if (!slot)
        {
            LogErrorMessage("EmitWinrtThinSlotCall: no slot '{}' on '{}'", { slotName,
                SpellType(*this, TypeAndValue{ .TypeName = thinName }) });
            return nullptr;
        }

        auto* i8PtrTy = cflat_llvm::PointerTo(builder->getInt8Ty());
        auto* typedObj = builder->CreateBitCast(objPtr, cflat_llvm::PointerTo(objTy));
        auto* vtblFieldPtr = builder->CreateStructGEP(objTy, typedObj, 0);
        auto* vtblPtr = builder->CreateLoad(cflat_llvm::PointerTo(vtblTy), vtblFieldPtr);
        auto* slotPtr = builder->CreateStructGEP(vtblTy, vtblPtr, slotIdx);
        auto* fnPtr = builder->CreateLoad(BuildThinFnPtrType(*slot), slotPtr);

        std::vector<llvm::Value*> callArgs;
        callArgs.push_back(builder->CreateBitCast(objPtr, i8PtrTy));
        for (auto* a : extraArgs) callArgs.push_back(a);
        return CreateIndirectCall(*slot, fnPtr, callArgs);
    }

cflat_winmd::TypeRef LLVMBackend::SubstWinrtVar(const cflat_winmd::TypeRef& t,
        const std::vector<cflat_winmd::TypeRef>& args)
{
        if (t.isGenericVar)
        {
            cflat_winmd::TypeRef r =
                (t.genericVarIndex >= 0 && (size_t)t.genericVarIndex < args.size()) ? args[t.genericVarIndex] : t;
            r.pointerDepth += t.pointerDepth;
            r.isArray = r.isArray || t.isArray;
            return r;
        }
        if (!t.genericArgs.empty())
        {
            cflat_winmd::TypeRef r = t;
            for (auto& g : r.genericArgs) g = SubstWinrtVar(g, args);
            return r;
        }
        return t;
    }

bool LLVMBackend::CFlatArgToWinrtTypeRef(const std::string& cflatName, cflat_winmd::TypeRef& out, std::string& err)
{
        // Normalize the few safe C aliases onto the explicit-width family.
        std::string n = cflatName;
        if (n == "int") n = "i32";
        else if (n == "uint") n = "u32";
        else if (n == "float") n = "f32";
        else if (n == "double") n = "f64";

        std::string w = cflat_winmd::CFlatToWinrtFundamental(n);
        if (!w.empty()) { out.fullName = w; return true; }

        // Named imported winmd type (interface / struct / enum / delegate / runtime class). WinMD
        // types are registered under their fully-qualified name, so this is an exact match - after
        // expanding a `using` alias, which is how a type ARGUMENT names one (the mangled
        // instantiation name keeps the alias spelling; only the WinRT signature needs the real one).
        std::string resolved = ResolveTypeAlias(cflatName);
        if (IsWinrtFullName(resolved)) { out.fullName = resolved; return true; }

        err = "cannot map type argument '" + cflatName + "' to a WinRT type (use an explicit-width "
              "scalar like i32/u32/f32/f64/bool/string/object, or the FULLY-QUALIFIED name of an "
              "imported winmd type such as Windows.Foundation.IStringable)";
        return false;
    }

bool LLVMBackend::IsWinrtFullName(const std::string& fullName) const
{
        for (const auto& i : winrtConsumedModel_.interfaces)   if (i.fullName == fullName) return true;
        for (const auto& s : winrtConsumedModel_.structs)      if (s.fullName == fullName) return true;
        for (const auto& e : winrtConsumedModel_.enums)        if (e.fullName == fullName) return true;
        for (const auto& d : winrtConsumedModel_.delegates)    if (d.fullName == fullName) return true;
        for (const auto& rc : winrtConsumedModel_.runtimeClasses) if (rc.fullName == fullName) return true;
        return false;
    }

bool LLVMBackend::InstantiateWinrtGenericInterface(const std::string& base,
        const std::vector<std::string>& cflatArgs, const std::string& mangledName)
{
        auto it = winrtGenericTemplates_.find(base);
        if (it == winrtGenericTemplates_.end()) return false;
        const cflat_winmd::Interface& tpl = it->second;
        if (auto ds = dataStructures.find(mangledName);
            ds != dataStructures.end() && !ds->second.StructFields.empty())
            return true;   // already fully instantiated
        if (verbose) std::cout << "[winmd] instantiate parameterized interface " << mangledName << "\n";

        if (cflatArgs.size() != tpl.genericParams.size())
        {
            LogErrorMessage("'{}<...>' expects {} type argument(s), got {}",
                { SpellType(*this, TypeAndValue{ .TypeName = base }),
                  std::to_string(tpl.genericParams.size()), std::to_string(cflatArgs.size()) });
            return true;   // handled (it IS a winmd generic), just mis-arity
        }

        std::vector<cflat_winmd::TypeRef> argRefs;
        for (const auto& a : cflatArgs)
        {
            cflat_winmd::TypeRef r;
            std::string err;
            if (!CFlatArgToWinrtTypeRef(a, r, err))
            {
                LogRawError(SpellType(*this, TypeAndValue{ .TypeName = base }) + "<...>: " + err);
                return true;
            }
            argRefs.push_back(r);
        }

        std::vector<cflat_winmd::Method> methods;
        for (const auto& m : tpl.methods)
        {
            cflat_winmd::Method nm = m;
            nm.returnType = SubstWinrtVar(m.returnType, argRefs);
            for (auto& p : nm.params) p.type = SubstWinrtVar(p.type, argRefs);
            methods.push_back(std::move(nm));
        }
        BuildWinrtInterfaceStructs(mangledName, methods, "interface " + tpl.fullName, winrtConsumedLspFile_);

        // Derive and stash the parameterized IID (shared algorithm; see WinmdSignature).
        cflat_winmd::TypeRef inst;
        inst.fullName = tpl.fullName;
        inst.genericArgs = argRefs;
        uint8_t img[16];
        std::string err;
        if (cflat_winmd::DerivePiid(inst, winrtConsumedModel_, img, err))
        {
            std::array<uint8_t, 16> a;
            for (int i = 0; i < 16; i++) a[i] = img[i];
            winrtInstanceIid_[mangledName] = a;
        }
        else
        {
            LogRawError("PIID derivation for '" + SpellType(*this,
                TypeAndValue{ .TypeName = mangledName }) + "': " + err);
        }
        return true;
    }

const cflat_winmd::Delegate* LLVMBackend::FindWinrtDelegate(const std::string& fullName) const
{
        for (const auto& d : winrtConsumedModel_.delegates)
            if (d.fullName == fullName) return &d;
        return nullptr;
    }

std::vector<llvm::Type*> LLVMBackend::WinrtDelegateInvokeParamTypes(const cflat_winmd::Method& invoke)
{
        auto* i8PtrTy = cflat_llvm::PointerTo(builder->getInt8Ty());
        std::vector<llvm::Type*> out;
        for (const cflat_winmd::Param& p : invoke.params)
        {
            std::string nm; bool ptr = false;
            MapWinrtTypeForSlot(p.type, nm, ptr);
            if (ptr) { out.push_back(i8PtrTy); continue; }
            TypeAndValue tv; tv.TypeName = nm; tv.Pointer = false;
            out.push_back(GetType(tv));
        }
        return out;
    }

void LLVMBackend::BuildWinrtDelegateType(const std::string& mangled, const cflat_winmd::Method& invoke,
        const uint8_t iidBytes[16])
{
        EnsureClosureLifetimeRegistered();
        auto* i8PtrTy = cflat_llvm::PointerTo(builder->getInt8Ty());
        auto* i32Ty = builder->getInt32Ty();
        auto* closureTy = GetClosureFatPtrType();

        // vtbl = 4 opaque code pointers (WinRT reinterprets it as the delegate C vtable).
        auto* vtblTy = llvm::StructType::create(*context,
            { i8PtrTy, i8PtrTy, i8PtrTy, i8PtrTy }, "winrtdel_" + mangled + "_vtbl");
        auto* objTy = llvm::StructType::create(*context,
            { cflat_llvm::PointerTo(vtblTy), i32Ty, closureTy }, "winrtdel_" + mangled);

        // GUID globals: IUnknown, IAgileObject, and this delegate's IID.
        uint8_t unkB[16], agileB[16];
        ParseUuidToBytes("00000000-0000-0000-C000-000000000046", unkB);
        ParseUuidToBytes("94EA2B94-E9CC-49E0-C0FF-EE64CA8F5B90", agileB);
        auto* gUnk = EmitGuidGlobal(unkB);
        auto* gAgile = EmitGuidGlobal(agileB);
        auto* gIid = EmitGuidGlobal(iidBytes);

        auto makeFn = [&](const std::string& nm, llvm::FunctionType* ty) {
            return llvm::Function::Create(ty, llvm::Function::InternalLinkage,
                "__winrtdel_" + mangled + "_" + nm, module.get());
        };

        // QueryInterface(i8* self, i8* riid, i8* ppv) -> i32
        auto* qiFn = makeFn("QueryInterface",
            llvm::FunctionType::get(i32Ty, { i8PtrTy, i8PtrTy, i8PtrTy }, false));
        {
            auto* entry = llvm::BasicBlock::Create(*context, "entry", qiFn);
            auto* matchBB = llvm::BasicBlock::Create(*context, "match", qiFn);
            auto* noBB = llvm::BasicBlock::Create(*context, "nomatch", qiFn);
            llvm::IRBuilder<> b(entry);
            auto* self = qiFn->getArg(0);
            auto* riid = qiFn->getArg(1);
            auto* ppv = qiFn->getArg(2);
            auto guidEq = [&](llvm::Value* a, llvm::GlobalVariable* g) -> llvm::Value* {
                auto* i64Ty = b.getInt64Ty();
                auto* i64PtrTy = cflat_llvm::PointerTo(i64Ty);
                auto* aLo = b.CreateAlignedLoad(i64Ty, b.CreateBitCast(a, i64PtrTy), llvm::MaybeAlign(1));
                auto* aHiPtr = b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), a, 8);
                auto* aHi = b.CreateAlignedLoad(i64Ty, b.CreateBitCast(aHiPtr, i64PtrTy), llvm::MaybeAlign(1));
                auto* gI8 = b.CreateBitCast(g, i8PtrTy);
                auto* bLo = b.CreateAlignedLoad(i64Ty, b.CreateBitCast(gI8, i64PtrTy), llvm::MaybeAlign(1));
                auto* bHiPtr = b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), gI8, 8);
                auto* bHi = b.CreateAlignedLoad(i64Ty, b.CreateBitCast(bHiPtr, i64PtrTy), llvm::MaybeAlign(1));
                return b.CreateAnd(b.CreateICmpEQ(aLo, bLo), b.CreateICmpEQ(aHi, bHi));
            };
            llvm::Value* match = b.CreateOr(guidEq(riid, gUnk), guidEq(riid, gAgile));
            match = b.CreateOr(match, guidEq(riid, gIid));
            b.CreateCondBr(match, matchBB, noBB);

            b.SetInsertPoint(matchBB);
            auto* objP = b.CreateBitCast(self, cflat_llvm::PointerTo(objTy));
            auto* rcP = b.CreateStructGEP(objTy, objP, 1);
            b.CreateAtomicRMW(llvm::AtomicRMWInst::Add, rcP, b.getInt32(1),
                llvm::MaybeAlign(), llvm::AtomicOrdering::Monotonic);
            b.CreateStore(self, b.CreateBitCast(ppv, cflat_llvm::PointerTo(i8PtrTy)));
            b.CreateRet(b.getInt32(0));

            b.SetInsertPoint(noBB);
            b.CreateStore(llvm::ConstantPointerNull::get(i8PtrTy),
                b.CreateBitCast(ppv, cflat_llvm::PointerTo(i8PtrTy)));
            b.CreateRet(b.getInt32((uint32_t)0x80004002));   // E_NOINTERFACE
        }

        // AddRef(i8* self) -> i32
        auto* addRefFn = makeFn("AddRef", llvm::FunctionType::get(i32Ty, { i8PtrTy }, false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(*context, "entry", addRefFn));
            auto* objP = b.CreateBitCast(addRefFn->getArg(0), cflat_llvm::PointerTo(objTy));
            auto* rcP = b.CreateStructGEP(objTy, objP, 1);
            auto* old = b.CreateAtomicRMW(llvm::AtomicRMWInst::Add, rcP, b.getInt32(1),
                llvm::MaybeAlign(), llvm::AtomicOrdering::Monotonic);
            b.CreateRet(b.CreateAdd(old, b.getInt32(1)));
        }

        // Release(i8* self) -> i32: destruct the owned closure and free at zero.
        auto* releaseFn = makeFn("Release", llvm::FunctionType::get(i32Ty, { i8PtrTy }, false));
        {
            auto* entry = llvm::BasicBlock::Create(*context, "entry", releaseFn);
            auto* freeBB = llvm::BasicBlock::Create(*context, "free", releaseFn);
            auto* doneBB = llvm::BasicBlock::Create(*context, "done", releaseFn);
            llvm::IRBuilder<> b(entry);
            auto* self = releaseFn->getArg(0);
            auto* objP = b.CreateBitCast(self, cflat_llvm::PointerTo(objTy));
            auto* rcP = b.CreateStructGEP(objTy, objP, 1);
            auto* old = b.CreateAtomicRMW(llvm::AtomicRMWInst::Sub, rcP, b.getInt32(1),
                llvm::MaybeAlign(), llvm::AtomicOrdering::Monotonic);
            auto* nw = b.CreateSub(old, b.getInt32(1));
            b.CreateCondBr(b.CreateICmpEQ(nw, b.getInt32(0)), freeBB, doneBB);

            b.SetInsertPoint(freeBB);
            if (auto* cdtor = module->getFunction("__closure_fat_ptr.dtor"))
                b.CreateCall(cdtor->getFunctionType(), cdtor, { b.CreateStructGEP(objTy, objP, 2) });
            if (auto* del = GetFunction("operator delete"))
                b.CreateCall(del->getFunctionType(), del, { b.CreateBitCast(self, del->getArg(0)->getType()) });
            b.CreateBr(doneBB);

            b.SetInsertPoint(doneBB);
            b.CreateRet(nw);
        }

        // Invoke(i8* self, <mapped invoke params...>) -> i32. Forward to the closure (env-last).
        std::vector<llvm::Type*> paramTys = WinrtDelegateInvokeParamTypes(invoke);
        std::vector<llvm::Type*> invokeSig = { i8PtrTy };
        for (auto* t : paramTys) invokeSig.push_back(t);
        auto* invokeFn = makeFn("Invoke", llvm::FunctionType::get(i32Ty, invokeSig, false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(*context, "entry", invokeFn));
            auto* objP = b.CreateBitCast(invokeFn->getArg(0), cflat_llvm::PointerTo(objTy));
            auto* cloP = b.CreateStructGEP(objTy, objP, 2);
            auto* code = b.CreateLoad(i8PtrTy, b.CreateStructGEP(closureTy, cloP, 0), "clo_code");
            llvm::Value* env = b.CreateLoad(i8PtrTy, b.CreateStructGEP(closureTy, cloP, 1), "clo_env");

            // Strip the OWNED tag (low bit) off the env before invoking - an owning heap env
            // carries the tag (lambda Option A) but the closure code expects a clean pointer.
            auto* envInt = b.CreatePtrToInt(env, b.getInt64Ty());
            auto* masked = b.CreateAnd(envInt, b.getInt64(~(uint64_t)1));
            env = b.CreateIntToPtr(masked, i8PtrTy, "env_untagged");

            std::vector<llvm::Type*> cloSig = paramTys;
            cloSig.push_back(i8PtrTy);   // trailing env
            auto* cloFnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(*context), cloSig, false);
            auto* cloFn = b.CreateBitCast(code, cflat_llvm::PointerTo(cloFnTy), "clo_fn");
            std::vector<llvm::Value*> cloArgs;
            for (size_t i = 0; i < paramTys.size(); i++) cloArgs.push_back(invokeFn->getArg(1 + (unsigned)i));
            cloArgs.push_back(env);
            b.CreateCall(cloFnTy, cloFn, cloArgs);
            b.CreateRet(b.getInt32(0));   // S_OK
        }

        std::vector<llvm::Constant*> entries = {
            llvm::ConstantExpr::getBitCast(qiFn, i8PtrTy),
            llvm::ConstantExpr::getBitCast(addRefFn, i8PtrTy),
            llvm::ConstantExpr::getBitCast(releaseFn, i8PtrTy),
            llvm::ConstantExpr::getBitCast(invokeFn, i8PtrTy) };
        auto* init = llvm::ConstantStruct::get(vtblTy, entries);
        auto* vtblGlobal = new llvm::GlobalVariable(*module, vtblTy, true,
            llvm::GlobalValue::InternalLinkage, init, "__winrtdel_" + mangled + "_vtbl_inst");

        winrtDelegateObjTy_[mangled] = objTy;
        winrtDelegateVtbl_[mangled] = vtblGlobal;
    }

llvm::Value* LLVMBackend::EmitWinrtDelegateObject(const std::string& base,
        const std::vector<std::string>& cflatArgs, llvm::Value* closureFat)
{
        const cflat_winmd::Delegate* tpl = FindWinrtDelegate(base);
        if (!tpl)
        {
            LogErrorMessage("winrtDelegate: '{}' is not an imported WinRT delegate type "
                     "(import the .winmd that declares it)",
                { SpellType(*this, TypeAndValue{ .TypeName = base }) });
            return nullptr;
        }
        if (cflatArgs.size() != tpl->genericParams.size())
        {
            LogErrorMessage("winrtDelegate: '{}' expects {} type argument(s), got {}",
                { SpellType(*this, TypeAndValue{ .TypeName = base }),
                  std::to_string(tpl->genericParams.size()), std::to_string(cflatArgs.size()) });
            return nullptr;
        }
        if (!(tpl->invoke.returnType.fullName == "Void" && tpl->invoke.returnType.pointerDepth == 0))
        {
            LogErrorMessage("winrtDelegate: '{}' has a non-void Invoke return, which the "
                     "projection does not support yet (only void-returning handlers)",
                { SpellType(*this, TypeAndValue{ .TypeName = base }) });
            return nullptr;
        }

        std::string mangled = MangleGenericInstance(*this, base, cflatArgs);

        // Substitute the generic Invoke signature with the concrete type arguments.
        std::vector<cflat_winmd::TypeRef> argRefs;
        for (const auto& a : cflatArgs)
        {
            cflat_winmd::TypeRef r; std::string err;
            if (!CFlatArgToWinrtTypeRef(a, r, err))
            {
                LogRawError("winrtDelegate " + SpellType(*this,
                    TypeAndValue{ .TypeName = base }) + "<...>: " + err);
                return nullptr;
            }
            argRefs.push_back(r);
        }
        cflat_winmd::Method invoke = tpl->invoke;
        for (auto& p : invoke.params) p.type = SubstWinrtVar(p.type, argRefs);

        // Derive the delegate IID: stored (non-generic) or PIID (parameterized).
        uint8_t iidBytes[16];
        if (cflatArgs.empty())
        {
            if (!ParseUuidToBytes(tpl->iid, iidBytes))
            {
                LogErrorMessage("winrtDelegate: delegate '{}' has no IID in metadata",
                    { SpellType(*this, TypeAndValue{ .TypeName = base }) });
                return nullptr;
            }
        }
        else
        {
            cflat_winmd::TypeRef inst; inst.fullName = tpl->fullName; inst.genericArgs = argRefs;
            std::string err;
            if (!cflat_winmd::DerivePiid(inst, winrtConsumedModel_, iidBytes, err))
            {
                LogRawError("winrtDelegate PIID for '" + SpellType(*this,
                    TypeAndValue{ .TypeName = mangled }) + "': " + err);
                return nullptr;
            }
        }

        if (!winrtDelegateObjTy_.count(mangled))
            BuildWinrtDelegateType(mangled, invoke, iidBytes);
        auto* objTy = winrtDelegateObjTy_[mangled];
        auto* vtblGlobal = winrtDelegateVtbl_[mangled];

        if (!GetFunction("operator new"))
        {
            LogErrorMessage("winrtDelegate: 'operator new' unavailable (import a core library first)");
            return nullptr;
        }
        auto* i8PtrTy = cflat_llvm::PointerTo(builder->getInt8Ty());
        auto* closureTy = GetClosureFatPtrType();

        // Allocate the object.
        uint64_t sz = module->getDataLayout().getTypeAllocSize(objTy);
        NamedVariable szArg;
        szArg.Primary = builder->getInt64(sz);
        szArg.BaseType = builder->getInt64Ty();
        auto* raw = CreateOverloadedFunctionCall("operator new", { szArg });
        auto* objP = builder->CreateBitCast(raw, cflat_llvm::PointerTo(objTy), "winrtdel.obj");

        // lpVtbl = &vtbl, refcount = 1.
        builder->CreateStore(builder->CreateBitCast(vtblGlobal, objTy->getStructElementType(0)),
            builder->CreateStructGEP(objTy, objP, 0));
        builder->CreateStore(builder->getInt32(1), builder->CreateStructGEP(objTy, objP, 1));

        // Clone the closure into the object so it owns an independent env (destructed on Release).
        llvm::Value* owned = closureFat;
        if (auto* copyFn = module->getFunction("__closure_fat_ptr.copy"))
            owned = builder->CreateCall(copyFn->getFunctionType(), copyFn, { closureFat }, "clo_clone");
        builder->CreateStore(owned, builder->CreateStructGEP(objTy, objP, 2));

        return builder->CreateBitCast(objP, i8PtrTy, "winrtdel.i8");
    }

void LLVMBackend::RegisterWinrtModel(const cflat_winmd::Model& model, const std::string& fileForLsp)
{
        using namespace cflat_winmd;

        // Retain everything imported so the signature encoder can resolve nested named type args
        // (an enum/struct/interface used as a generic argument) when deriving a PIID later.
        winrtConsumedLspFile_ = fileForLsp;
        for (const auto& i : model.interfaces)    winrtConsumedModel_.interfaces.push_back(i);
        for (const auto& s : model.structs)       winrtConsumedModel_.structs.push_back(s);
        for (const auto& e : model.enums)         winrtConsumedModel_.enums.push_back(e);
        for (const auto& d : model.delegates)     winrtConsumedModel_.delegates.push_back(d);
        for (const auto& rc : model.runtimeClasses) winrtConsumedModel_.runtimeClasses.push_back(rc);

        // Pass A: enums. Record the underlying scalar (for type mapping) and expose members as
        // named constants "<Simple>_<Member>" (first-writer-wins). Deliberately NOT qualified: a
        // member is a VALUE (a global constant, never a type), so it cannot displace a type and
        // cannot reach the bad-IR class that qualification exists to kill; and a dotted name in
        // expression position is member access, so "Ns.Enum_Member" would not even be spellable.
        for (const Enum& e : model.enums)
        {
            std::string under = WinrtFundamentalToCFlat(e.underlying);
            if (under != "i32" && under != "u32" && under != "i64" && under != "u64") under = "i32";
            winrtEnumUnderlying_[e.fullName] = under;
            std::string simple = WinrtSimpleName(e.fullName);
            bool wide = (under == "i64" || under == "u64");
            for (const EnumMember& m : e.members)
            {
                std::string name = simple + "_" + m.name;
                if (globalNamedVariable.count(name)) continue;
                TypeAndValue tv; tv.TypeName = wide ? "i64" : "i32"; tv.VariableName = name;
                llvm::Constant* c = wide
                    ? (llvm::Constant*)builder->getInt64((uint64_t)m.value)
                    : (llvm::Constant*)builder->getInt32((uint32_t)(int32_t)m.value);
                CreateGlobalVariable(tv, c);
                if (auto* s = GetSymbolSink())
                    s->Register(SymbolKind::Variable, name, fileForLsp, 0, 0,
                                SpellType(*this, tv) + " " + name, {}, {}, name);
            }
        }

        // Pass B: value structs, under their full name. Shells first so a field can reference a
        // peer struct by value. Nothing to skip - a fully-qualified name cannot collide with a
        // program's own `struct Rect`, so both types coexist.
        for (const Struct& st : model.structs)
        {
            winrtValueStructs_.insert(st.fullName);
            if (!dataStructures.count(st.fullName)) CreateStructType(st.fullName, {});
        }
        for (const Struct& st : model.structs)
        {
            std::vector<DeclTypeAndValue> fields;
            for (const Field& f : st.fields)
            {
                DeclTypeAndValue d;
                d.VariableName = f.name;
                MapWinrtTypeForSlot(f.type, d.TypeName, d.Pointer);
                fields.push_back(d);
            }
            CreateStructType(st.fullName, fields);   // fills the shell created above
            if (auto* s = GetSymbolSink())
            {
                TypeAndValue structType;
                structType.TypeName = st.fullName;
                const std::string displayStructName = SpellType(*this, structType);
                s->Register(SymbolKind::Struct, st.fullName, fileForLsp, 0, 0,
                            "struct " + displayStructName, {}, {}, displayStructName);
            }
        }

        // Pass C: interfaces -> COM vtable struct (flat IInspectable layout) + thin pointer struct.
        // Generic interfaces (IVector<T>, IMap<K,V>, ...) cannot be lowered until their type args
        // are known, so they are stashed as templates (keyed by full name) and instantiated on
        // demand by InstantiateWinrtGenericInterface.
        std::unordered_map<std::string, const cflat_winmd::Interface*> byName;
        for (const Interface& iface : model.interfaces) byName[iface.fullName] = &iface;

        size_t registered = 0, deferredGeneric = 0;
        for (const Interface& iface : model.interfaces)
        {
            const std::string& regName = iface.fullName;
            if (!iface.genericParams.empty())
            {
                deferredGeneric++;
                if (!winrtGenericTemplates_.count(regName)) winrtGenericTemplates_[regName] = iface;
                continue;
            }
            // Classic-COM (IUnknown-rooted, not IInspectable) interfaces from Win32 metadata use the
            // 3-slot IUnknown header and inline their single-inheritance base methods into the vtable.
            // WinRT interfaces (implicit IInspectable base) keep the 6-slot header + own methods only.
            std::set<std::string> s1, s2;
            bool unknownRooted = WinrtRequiresReaches(iface.fullName, byName, "IUnknown", s1)
                              && !WinrtRequiresReaches(iface.fullName, byName, "IInspectable", s2);
            bool ok;
            if (unknownRooted)
            {
                std::vector<cflat_winmd::Method> flat;
                std::set<std::string> seen;
                CollectComBaseMethods(iface, byName, seen, flat);
                ok = BuildWinrtInterfaceStructs(regName, flat, "interface " + iface.fullName, fileForLsp, false);
            }
            else
            {
                ok = BuildWinrtInterfaceStructs(regName, iface.methods, "interface " + iface.fullName, fileForLsp);
            }
            if (ok) registered++;
        }

        // Deferrals are surfaced under -v, not silently dropped (per the projection plan).
        // Runtime-class activation, delegates, and generic interfaces are not yet projected.
        if (verbose)
            std::cout << std::format(
                "[winmd] {}: registered {} interface(s), {} struct(s), {} enum(s); deferred {} generic interface(s), {} runtime class(es), {} delegate(s)\n",
                fileForLsp, registered, model.structs.size(), model.enums.size(),
                deferredGeneric, model.runtimeClasses.size(), model.delegates.size());
    }

bool LLVMBackend::CompileWinmdFile(const std::string& path)
{
        cflat_winmd::Model model;
        std::string err;
        if (!cflat_winmd::ReadWinmd(path, model, err))
        {
            LogRawError("Failed to read WinRT metadata '" + path + "': " + err);
            return false;
        }
        RegisterWinrtModel(model, path);
        return true;
    }

bool LLVMBackend::WinmdInstantiateSelfTest(const std::string& path, std::string& report)
{
        // The constructor already brought up context/module/builder; no Init() here.
        cflat_winmd::Model model;
        std::string err;
        if (!cflat_winmd::ReadWinmd(path, model, err)) { report = "read failed: " + err + "\n"; return false; }
        RegisterWinrtModel(model, path);

        struct Case { const char* base; std::vector<std::string> args; const char* iid; };
        std::vector<Case> cases = {
            { "Windows.Foundation.IReference",                  { "i32" }, "548cefbd-bc8a-5fa0-8df2-957440fc8bf4" },
            { "Windows.Foundation.Collections.IVector",         { "i32" }, "b939af5b-b45d-5489-9149-61442c1905fe" },
            { "Windows.Foundation.Collections.IVectorView",     { "i32" }, "8d720cdf-3934-5d3f-9a55-40e8063b086a" },
            { "Windows.Foundation.Collections.IIterable",       { "i32" }, "81a643fb-f51c-5565-83c4-f96425777b66" },
        };

        bool allOk = true;
        for (const auto& c : cases)
        {
            if (!winrtGenericTemplates_.count(c.base)) { report += std::string("[skip] ") + c.base + " (not in winmd)\n"; continue; }
            std::string mangled = MangleGenericInstance(*this, c.base, c.args);
            InstantiateWinrtGenericInterface(c.base, c.args, mangled);

            auto iidIt = winrtInstanceIid_.find(mangled);
            std::string got = iidIt != winrtInstanceIid_.end()
                ? cflat_winmd::FormatGuidImage(iidIt->second.data()) : "(none)";
            auto vtbl = dataStructures.find(mangled + "Vtbl");
            size_t slots = vtbl != dataStructures.end() ? vtbl->second.StructFields.size() : 0;
            bool ok = (got == c.iid) && slots >= 6;
            allOk = allOk && ok;
            report += std::format("[{}] {} -> {} ({} vtbl slots){}\n", ok ? " ok " : "FAIL",
                mangled, got, slots, ok ? "" : std::string("  want ") + c.iid);
        }
        report += allOk ? "\nALL PASS\n" : "\nFAILURES PRESENT\n";
        return allOk;
    }

cflat_winmd::TypeRef LLVMBackend::CFlatTypeToWinrt(const TypeAndValue& tv)
{
        cflat_winmd::TypeRef t;
        std::string w = cflat_winmd::CFlatToWinrtFundamental(tv.TypeName);
        if (!w.empty())
        {
            t.fullName = w;
            if (tv.Pointer && w != "string" && w != "object") t.pointerDepth = 1;
        }
        else if (FindTypeAnnotation(tv.TypeName, "uuid") || winrtClasses.count(tv.TypeName))
            t.fullName = tv.TypeName;
        else
            t.fullName = "Object";
        return t;
    }

bool LLVMBackend::EmitWinmd(const std::string& path, const std::string& assemblyName)
{
        cflat_winmd::Model model;

        for (const auto& [name, methods] : interfaceTable)
        {
            std::string iid = GetTypeAnnotationArg(name, "uuid");
            if (iid.empty()) continue;   // only [uuid]-bearing interfaces describe a winmd type
            cflat_winmd::Interface iface;
            iface.fullName = name;
            iface.iid = iid;
            for (const auto& m : methods)
            {
                cflat_winmd::Method wm;
                wm.name = m.Name;
                wm.returnType = CFlatTypeToWinrt(m.ReturnType);
                for (const auto& p : m.Parameters)
                {
                    cflat_winmd::Param wp;
                    wp.type = CFlatTypeToWinrt(p);
                    wm.params.push_back(std::move(wp));
                }
                iface.methods.push_back(std::move(wm));
            }
            model.interfaces.push_back(std::move(iface));
        }

        for (const auto& [cls, info] : winrtClasses)
        {
            cflat_winmd::RuntimeClass rc;
            rc.fullName = cls;
            rc.defaultInterface = info.InterfaceName;
            rc.interfaces.push_back(info.InterfaceName);
            rc.activatable = true;
            model.runtimeClasses.push_back(std::move(rc));
        }

        if (model.interfaces.empty() && model.runtimeClasses.empty())
        {
            LogErrorMessage("--emit-winmd: no [winrt] interfaces or classes found to emit");
            return false;
        }

        std::string err;
        if (!cflat_winmd::WriteWinmd(model, assemblyName, path, err))
        {
            LogRawError("Failed to emit winmd '" + path + "': " + err);
            return false;
        }
        if (verbose)
            std::cout << std::format("[winmd] wrote {} ({} interface(s), {} runtime class(es))\n",
                path, model.interfaces.size(), model.runtimeClasses.size());
        return true;
    }

bool LLVMBackend::CheckWinmd(const std::string& path)
{
        cflat_winmd::Model model;
        std::string err;
        if (!cflat_winmd::ReadWinmd(path, model, err))
        {
            LogRawError("Failed to read WinRT metadata '" + path + "': " + err);
            return false;
        }
        if (verbose)
            std::cout << std::format("[winmd] {}: {} interfaces, {} structs, {} enums, {} delegates, {} runtime classes\n",
                path, model.interfaces.size(), model.structs.size(), model.enums.size(),
                model.delegates.size(), model.runtimeClasses.size());
        return true;
    }

int LLVMBackend::InterfaceDtorSlotIndex(const std::string& ifaceName) const
{
        size_t methodCount = 0;
        if (const auto* methods = FindInterface(ifaceName))
            methodCount = methods->size();
        return (int)(1 + methodCount + InterfaceFieldCount(ifaceName));
    }

llvm::Value* LLVMBackend::EmitInterfaceFieldAddress(llvm::Value* fatVal, const std::string& ifaceName,
                                           const std::string& fieldName, llvm::Type* fieldType)
{
        int fieldIdx = InterfaceFieldIndex(ifaceName, fieldName);
        if (fieldIdx < 0) return nullptr;

        size_t methodCount = 0;
        if (const auto* methods = FindInterface(ifaceName))
            methodCount = methods->size();

        auto ptrTy = cflat_llvm::PointerTo(builder->getInt8Ty());
        llvm::Value* vtablePtr = builder->CreateExtractValue(fatVal, { 0u }, "iface_vtable");
        llvm::Value* dataPtr   = builder->CreateExtractValue(fatVal, { 1u }, "iface_data");

        auto* slot = builder->CreateGEP(ptrTy, vtablePtr,
            builder->getInt32((int)(1 + methodCount + fieldIdx)), "iface_fld_slot");
        auto* offPtr = builder->CreateLoad(ptrTy, slot, "iface_fld_offptr");
        auto* off    = builder->CreatePtrToInt(offPtr, builder->getInt64Ty(), "iface_fld_off");
        auto* bytePtr = builder->CreateGEP(builder->getInt8Ty(), dataPtr, off, "iface_fld_bytes");
        // Re-GEP at the field type (a zero-index no-op) so GetTypeFromStorage - which reads a
        // GEP's element type - sees the field type rather than i8, and loads/stores get it right.
        return builder->CreateGEP(fieldType, bytePtr, builder->getInt32(0), "iface_fld_addr");
    }

void LLVMBackend::DeleteInterfaceValue(llvm::Value* fatVal, const std::string& ifaceName, llvm::Value* fatStorage)
{
        auto fatTy = GetFatPtrType();
        auto ptrTy = cflat_llvm::PointerTo(builder->getInt8Ty());

        llvm::Value* vtablePtr = builder->CreateExtractValue(fatVal, { 0u }, "iface_vtable");
        llvm::Value* dataPtr   = builder->CreateExtractValue(fatVal, { 1u }, "iface_data");

        auto* nullPtr   = llvm::ConstantPointerNull::get(ptrTy);
        auto* dataIsNull = builder->CreateICmpEQ(dataPtr, nullPtr, "iface_data_isnull");
        auto* liveBB  = CreateBasicBlock("iface_del_live");
        auto* afterBB = CreateBasicBlock("iface_del_after");
        builder->CreateCondBr(dataIsNull, afterBB, liveBB);

        // Live: dispatch the destructor through the vtable, guarding a null slot.
        builder->SetInsertPoint(liveBB);
        auto* dtorSlot = builder->CreateGEP(ptrTy, vtablePtr,
            builder->getInt32(InterfaceDtorSlotIndex(ifaceName)), "iface_dtor_slot");
        auto* dtorFn   = builder->CreateLoad(ptrTy, dtorSlot, "iface_dtor");
        auto* dtorIsNull = builder->CreateICmpEQ(dtorFn, nullPtr, "iface_dtor_isnull");
        auto* callBB = CreateBasicBlock("iface_del_dtor");
        auto* freeBB = CreateBasicBlock("iface_del_free");
        builder->CreateCondBr(dtorIsNull, freeBB, callBB);

        builder->SetInsertPoint(callBB);
        auto* dtorTy = llvm::FunctionType::get(builder->getVoidTy(), { ptrTy }, false);
        builder->CreateCall(dtorTy, dtorFn, { dataPtr });
        builder->CreateBr(freeBB);

        builder->SetInsertPoint(freeBB);
        NamedVariable ptrArg;
        ptrArg.Primary  = dataPtr;
        ptrArg.BaseType = ptrTy;
        if (GetFunction("operator delete"))
            CreateOverloadedFunctionCall("operator delete", { ptrArg });
        builder->CreateBr(afterBB);

        builder->SetInsertPoint(afterBB);
        // Null the operand's data field so a second delete sees a null pointer and no-ops.
        if (fatStorage != nullptr)
        {
            auto* dataField = builder->CreateStructGEP(fatTy, fatStorage, 1);
            builder->CreateStore(nullPtr, dataField);
        }
    }

bool LLVMBackend::IsStringLiteralConstant(llvm::Constant* c) const
{
        return stringLiteralLenByPtr.count(c) > 0;
    }

llvm::Value* LLVMBackend::WrapStringLiteralAsString(llvm::Value* strLitPtr)
{
        auto* strTy = llvm::StructType::getTypeByName(*context, "string");
        if (!strTy) return strLitPtr;

        auto* c = llvm::dyn_cast<llvm::Constant>(strLitPtr);
        if (c)
        {
            auto it = stringLiteralLenByPtr.find(c);
            if (it != stringLiteralLenByPtr.end())
            {
                llvm::Value* strVal = llvm::UndefValue::get(strTy);
                strVal = builder->CreateInsertValue(strVal, strLitPtr, { 0u });
                strVal = builder->CreateInsertValue(strVal, builder->getInt32(it->second), { 1u });
                return strVal;
            }
        }

        // Not a known literal: derive the length at runtime. A terminated block is no more
        // usable than no block at all; emitting there would produce invalid IR.
        if (!IsInsertBlockLive())
        {
            llvm::Value* strVal = llvm::UndefValue::get(strTy);
            strVal = builder->CreateInsertValue(strVal, strLitPtr, { 0u });
            strVal = builder->CreateInsertValue(strVal, builder->getInt32(0), { 1u });
            return strVal;
        }

        if (GetFunction("operator string"))
        {
            // Save/restore the post-call flag channel: this injected call must not clobber
            // flags the caller is mid-statement consuming from its own pending call.
            TypeAndValue savedReturnType = lastCallReturnType;
            bool savedReturnsOwned = lastCallReturnsOwned;

            NamedVariable argNV;
            argNV.Primary = strLitPtr;
            argNV.BaseType = strLitPtr->getType();
            argNV.TypeAndValue.TypeName = "char";
            argNV.TypeAndValue.Pointer = true;
            llvm::Value* result = CreateOverloadedFunctionCall("operator string", { argNV });

            lastCallReturnType = savedReturnType;
            lastCallReturnsOwned = savedReturnsOwned;
            return result;
        }

        // No operator string overload registered (string.cb not imported): derive length
        // via a direct strlen call, non-owning.
        auto* strlenFn = GetOrDeclareStrlen();
        auto* len64 = builder->CreateCall(strlenFn, { strLitPtr }, "charptr_strlen");
        auto* len32 = builder->CreateTrunc(len64, builder->getInt32Ty(), "charptr_len32");

        llvm::Value* strVal = llvm::UndefValue::get(strTy);
        strVal = builder->CreateInsertValue(strVal, strLitPtr, { 0u });
        strVal = builder->CreateInsertValue(strVal, len32, { 1u });
        return strVal;
    }

llvm::Value* LLVMBackend::CoerceCharPointerToString(llvm::Value* rawPtr)
{
        if (rawPtr == nullptr) return rawPtr;
        auto* c = llvm::dyn_cast<llvm::Constant>(rawPtr);
        if (c != nullptr && IsStringLiteralConstant(c))
            return WrapStringLiteralAsString(rawPtr);
        if (GetFunction("operator string") != nullptr)
        {
            NamedVariable argNV;
            argNV.Primary = rawPtr;
            argNV.BaseType = rawPtr->getType();
            argNV.TypeAndValue.TypeName = "char";
            argNV.TypeAndValue.Pointer = true;
            return CreateOverloadedFunctionCall("operator string", { argNV });
        }
        return WrapStringLiteralAsString(rawPtr);
    }

llvm::Function* LLVMBackend::GetOrDeclareStrlen()
{
        if (auto* fn = module->getFunction("strlen"))
            return fn;
        auto* i64Ty = builder->getInt64Ty();
        auto* ptrTy = cflat_llvm::PointerTo(builder->getInt8Ty());
        auto* fnTy = llvm::FunctionType::get(i64Ty, { ptrTy }, false);
        return llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage, "strlen", module.get());
    }

llvm::Value* LLVMBackend::EmitOwnedStringDeepCopy(llvm::Value* value)
{
        auto* strTy = llvm::StructType::getTypeByName(*context, "string");
        if (strTy == nullptr || value == nullptr || value->getType() != strTy) return value;
        if (!GetFunction("operator new")) return value;

        auto* srcPtr = builder->CreateExtractValue(value, { 0u }, "cpyptr");
        // Mask off the OWNED high bit before using _len as a byte count.
        auto* srcLen = builder->CreateAnd(
            builder->CreateExtractValue(value, { 1u }, "cpylenraw"),
            builder->getInt32(0x7FFFFFFF), "cpylen");
        auto* len64 = builder->CreateZExt(srcLen, builder->getInt64Ty());
        auto* allocSize = builder->CreateAdd(len64, builder->getInt64(1), "cpybufsz");

        NamedVariable szArg;
        szArg.Primary  = allocSize;
        szArg.BaseType = builder->getInt64Ty();
        auto* rawPtr  = CreateOverloadedFunctionCall("operator new", { szArg });
        auto* heapPtr = builder->CreateBitCast(rawPtr, cflat_llvm::PointerTo(builder->getInt8Ty()), "cpybuf");
        // Copy exactly _len bytes (0 is a safe no-op for an empty source), then NUL-terminate.
        builder->CreateMemCpy(heapPtr, llvm::MaybeAlign(1), srcPtr, llvm::MaybeAlign(1), len64);
        auto* termPtr = builder->CreateInBoundsGEP(builder->getInt8Ty(), heapPtr, { len64 }, "cpyterm");
        builder->CreateStore(builder->getInt8(0), termPtr);

        auto* ownedLen = builder->CreateOr(srcLen, builder->getInt32(0x80000000), "cpyownedlen");
        llvm::Value* out = llvm::UndefValue::get(strTy);
        out = builder->CreateInsertValue(out, heapPtr, { 0u });
        out = builder->CreateInsertValue(out, ownedLen, { 1u });
        return out;
    }

void LLVMBackend::DiagnoseExplicitMoveToBorrowParam(const std::string& functionName,
        const std::string& paramName, const std::string& paramType, bool paramIsMove,
        const NamedVariable& arg)
{
        if (!arg.IsExplicitMove || paramIsMove) return;
        // Owns a resource? A destructor on the value type is the general signal; the
        // owning-* flags cover the cases (string locals, move params) that lack one.
        // TypeHasDestructor only sees a USER-declared dtor, so a value type that owns via a
        // SYNTHESIZED dtor (e.g. Holder{unique T*}, with no hand-written ~) slips through -
        // IsOwningValueType closes that gap (it forces the memberwise dtor and checks it).
        if (!(arg.IsOwningString || arg.IsOwningStruct || arg.IsOwning
              || TypeHasDestructor(arg.TypeAndValue.TypeName)
              || IsOwningValueType(arg.TypeAndValue.TypeName))) return;
        LogErrorMessage(
            "call to '{}': parameter '{}' BORROWS its argument, so '{} {}' transfers nothing - "
            "the callee never takes ownership and the value would be orphaned. Drop the '{}', "
            "or declare the parameter '{} {}'.",
            { SpellFunctionSymbol(*this, functionName), paramName,
              "move", arg.CallerName.empty() ? SpellType(*this, arg.TypeAndValue) : arg.CallerName,
              "move", "move", SpellType(*this, TypeAndValue{ .TypeName = paramType }) });
    }

void LLVMBackend::DiagnoseExplicitMoveToBorrowParam(const std::string& functionName,
        const TypeAndValue& param, const NamedVariable& arg, bool foreignCxxCallee)
{
        // A `unique`-typed parameter is a sink even without the `move` keyword: the type
        // itself says the callee takes ownership, so an explicit `move` at the call site is fine.
        // An inferred owning-value sink (body unconditionally moves the param, owning concrete
        // type) is likewise a real sink, so `move` into it transfers rather than "nothing".
        bool inferredSinkConsumes = param.IsConsumeInferredSink
            && OwningSinkConsumesConcrete(param);
        bool foreignCxxPointerSink = foreignCxxCallee && param.Pointer && !param.IsAlias
            && !param.IsRvalueRef && !param.IsCxxRefToPointer && !param.CxxConstRef();
        bool paramIsSink = param.IsMove
            || param.IsRvalueRef
            || (!param.Pointer && !param.IsAlias && IsCoreUniqueType(param.TypeName))
            // A foreign C++ class or holder taken by value gets a constructed parameter temp.
            // Explicit `move x` transfers its fields with their C++ move constructors.
            || (!param.Pointer && !param.IsAlias
                && (IsForeignNontrivialCxxClass(param.TypeName)
                    || HasForeignNontrivialCxxField(param.TypeName)))
            || foreignCxxPointerSink
            || inferredSinkConsumes
            || (OwningSinkConsumesConcrete(param) && IsOwningValueOrClosureType(param.TypeName));
        DiagnoseExplicitMoveToBorrowParam(
            functionName, param.VariableName, param.TypeName, paramIsSink, arg);
    }

void LLVMBackend::RejectOwningTempUniqueFieldIntoSinkParam(const std::string& functionName,
        const TypeAndValue& param, const NamedVariable& arg)
{
        bool coreUniqueValueParam = !param.Pointer
            && (IsCoreUniqueType(param.TypeName) || MangledBase(param.TypeName) == "unique");
        bool paramClaimsOwnership = coreUniqueValueParam
            || (param.Pointer && !param.IsAlias
                && (param.IsMove
                    || param.IsUnique));
        if (!paramClaimsOwnership) return;
        if (!JoinCarriesOwningTempUniqueField(arg.Primary))
        {
            // Callee still below its call site: defer this destination like the others.
            RecordDeferredTempUniqueFieldSinkEscape(arg.Primary, functionName, param.VariableName);
            return;
        }
        LogRawError(DescribeTempUniqueFieldSinkEscape(SpellFunctionSymbol(*this, functionName),
                                                     param.VariableName));
    }

LLVMBackend::TypeAndValue LLVMBackend::FuncPtrParamAsTypeAndValue(const TypeAndValue::FuncPtrParam& p,
        size_t index) const
{
        TypeAndValue tv;
        if (p.FunctionTypeSignature != nullptr)
            tv = *p.FunctionTypeSignature;
        else
            tv.TypeName = p.TypeName;
        tv.Pointer  = p.Pointer || p.IsArrayView;
        tv.IsArrayView = p.IsArrayView;
        tv.PointerDepth = p.PointerDepth;
        tv.ElemPointer = tv.Pointer && p.PointerDepth >= 2;
        if (p.ConstMask >= 0) tv.ConstMask = static_cast<uint8_t>(p.ConstMask);
        tv.IsInterface = IsInterfaceType(p.TypeName);
        tv.IsAlias = p.IsAlias;
        tv.AllocAlignValue = p.AllocAlignValue;
        tv.IsOwningSink = p.IsOwningSink;
        tv.IsConsumeInferredSink = p.IsConsumeInferredSink;
        tv.IsReturnInferredSink = p.IsReturnInferredSink;
        tv.IsWriteInferredSink = p.IsWriteInferredSink;
        // Diagnostics print the alias the user wrote; TypeName keeps the resolved base for analysis.
        if (!p.AliasSpelling.empty()) tv.DiagnosticTypeName = FuncPtrAliasParamSpelling(p);
        // A param whose funcptr TYPE spells `move` takes the DECLARED move path, exactly as a
        // direct call does. An INFERRED sink leaves IsMove false - see ApplyFuncPtrSinkTransfer.
        tv.IsMove = p.IsMove;
        // 0-based, matching the indirect call site's own DiagnoseExplicitMoveToBorrowParam.
        tv.VariableName = std::to_string(index);
        return tv;
}

void LLVMBackend::ApplyFuncPtrSinkTransfer(const std::string& functionName,
        const std::vector<TypeAndValue::FuncPtrParam>& params,
        const std::vector<NamedVariable>& args, bool beforeCall)
{
        if (beforeCall) RegisterOwnedStringRvalueArguments(args);
        bool anySink = false;
        for (const auto& p : params)
            anySink = anySink || p.IsOwningSink || p.IsConsumeInferredSink || p.IsMove;
        const bool hasDeferredDiagnostic = beforeCall
            && std::any_of(args.begin(), args.end(), [](const auto& arg) {
                return std::any_of(arg.ConditionalMoveSources.begin(),
                    arg.ConditionalMoveSources.end(), [](const auto& source) {
                        return !source.DeferredDiagnostic.empty();
                    });
            });
        if (!anySink && !hasDeferredDiagnostic) return;
        // A DECLARED `move` param takes the same ApplyMoveParamTransfer path a direct call takes
        // (FuncPtrParamAsTypeAndValue carries IsMove); an INFERRED sink keeps IsMove false.
        std::vector<TypeAndValue> synth;
        for (size_t i = 0; i < params.size(); i++)
        {
            synth.push_back(FuncPtrParamAsTypeAndValue(params[i], i));
            // An unnamed function<> parameter reads as its position in sink diagnostics, not '0'.
            synth.back().VariableName = std::format("parameter {}", i + 1);
        }
        ApplyMoveParamTransfer(functionName, synth, args, /*paramsCarryAllocAlign*/ true,
            /*calleeIsMethod*/ false, beforeCall);
}

void LLVMBackend::LogBorrowedPointerMoveSinkError(const std::string& sourceDescription,
        const std::string& parameterName, const std::string& functionName,
        const std::string& declaredName) const
{
        LogErrorMessage(
            "cannot move {} into move parameter '{}' of '{}' - the caller may still "
            "own this pointer and will free it on scope exit. Move it out of the "
            "owning slot instead (e.g. 'move parent->children[i]'), or declare "
            "'{}' as 'move {}' if this function owns it.",
            { sourceDescription, parameterName, functionName, declaredName, declaredName });
}

void LLVMBackend::LogOwningLocalCopyMoveSinkError(const std::string& sourceName,
        const std::string& ownerName, const std::string& parameterName,
        const std::string& functionName, const llvm::Value* ownerStorage) const
{
        // A `move` parameter owns its pointee exactly as a local does; name the binding kind by
        // storage, so a local shadowing a parameter name still reads "local".
        const bool ownerIsParameter = ownerStorage != nullptr ? IsFunctionParameterStorage(ownerStorage)
                                                              : IsFunctionParameter(ownerName);
        const std::string ownerKind = ownerIsParameter ? "parameter" : "local";
        LogErrorMessage(
            "cannot move borrowed pointer '{}' (copied from owning {} '{}') into move parameter "
            "'{}' of '{}' - '{}' still owns the pointee and frees it on scope exit. Pass '{}' "
            "itself with 'move {}' to transfer ownership, or pass a pointer that is not a copy.",
            { sourceName, ownerKind, ownerName, parameterName, functionName, ownerName,
              ownerName, ownerName });
}

bool LLVMBackend::BindingKeepsOwnershipOfBoxedObject(const NamedVariable* nv) const
{
        if (nv == nullptr) return false;
        if (!nv->BorrowedUniqueField.empty()) return true;
        if (nv->Storage == nullptr) return false;
        bool isCoreUniqueValue = IsCoreUniqueType(nv->TypeAndValue.TypeName)
            && !nv->TypeAndValue.Pointer;
        if (nv->IsUniqueFieldAlias || nv->TypeAndValue.IsUnique || isCoreUniqueValue)
            return true;
        if (nv->Primary != nullptr
            && FindUniqueFieldBorrowResult(nv->Primary) != nullptr
            && !nv->TypeAndValue.IsMove)
            return true;
        if (!llvm::isa<llvm::AllocaInst>(nv->Storage)) return false;
        if (nv->IsOwning) return true;
        if (nv->BorrowsOwnedElement && !nv->CoalesceRebound) return true;
        if (nv->InheritedKeepsOwner) return true;
        if (JoinArmsStillKeepOwner(*nv)) return true;
        // A rebind retires declaration-time borrow/copy proofs before their fallbacks can apply.
        if (nv->PointerRebound) return false;
        if (nv->IsBorrowed && !nv->BorrowedOrigin.empty()
            && !BorrowProofRetiredByRebind(*nv)) return true;
        if (OwningLocalCopyStillAliases(*nv)) return true;
        // Compare storage identity because an inner local can shadow a parameter's name.
        return !nv->TypeAndValue.IsMove && IsFunctionParameterStorage(nv->Storage);
}

bool LLVMBackend::JoinArmsStillKeepOwner(const NamedVariable& nv, int depth) const
{
        if (!nv.JoinKeepsOwner || nv.JoinKeepsOwnerSlots.empty()) return false;
        // The depth bound prevents cycles from being mistaken for a complete owner proof.
        if (depth > 4) return false;
        for (auto* slot : nv.JoinKeepsOwnerSlots)
        {
            if (slot == nullptr || slot == nv.Storage) return false;
            const auto* arm = FindVariableByStorage(slot);
            // Recheck each arm after binding so a later rebind retires its old proof.
            if (arm == nullptr || arm->PointerRebound) return false;
            if (arm->JoinKeepsOwner)
            {
                if (!JoinArmsStillKeepOwner(*arm, depth + 1)) return false;
                continue;
            }
            if (!BindingKeepsOwnershipOfBoxedObject(arm)) return false;
        }
        return true;
}

struct NonLocalViewStoragePath
{
    const llvm::Value* Root = nullptr;
    std::vector<const llvm::Value*> Indices;
};

static bool PointerSlotHasEscaped(const llvm::AllocaInst* slot)
{
    if (slot == nullptr) return true;
    for (const auto* user : slot->users())
    {
        if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(user))
        {
            if (load->getPointerOperand()->stripPointerCasts() == slot) continue;
            return true;
        }
        if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
        {
            if (store->getPointerOperand()->stripPointerCasts() == slot) continue;
            return true;
        }
        if (const auto* call = llvm::dyn_cast<llvm::CallBase>(user))
            if (const llvm::Function* callee = call->getCalledFunction(); callee != nullptr)
            {
                const auto name = callee->getName();
                if (name.starts_with("llvm.dbg.") || name.starts_with("llvm.lifetime.")) continue;
            }
        return true;
    }
    return false;
}

static bool NonLocalFieldPathMatches(const std::string& pattern, const std::string& value)
{
    size_t p = 0;
    size_t v = 0;
    while (p < pattern.size() && v < value.size())
    {
        if (p + 2 < pattern.size() && pattern[p] == '['
            && pattern[p + 1] == '*' && pattern[p + 2] == ']')
        {
            if (value[v] != '[') return false;
            const size_t close = value.find(']', v + 1);
            if (close == std::string::npos || close == v + 1) return false;
            p += 3;
            v = close + 1;
            continue;
        }
        if (pattern[p++] != value[v++]) return false;
    }
    return p == pattern.size() && v == value.size();
}

static bool NonLocalFieldPathsAlias(const std::string& left, const std::string& right)
{
    return left == right || NonLocalFieldPathMatches(left, right)
        || NonLocalFieldPathMatches(right, left);
}

static bool SameProvenNonLocalViewPath(const NonLocalViewStoragePath& left,
        const NonLocalViewStoragePath& right);

static bool CollectNonLocalViewStorageCandidates(const llvm::Value* value,
        const llvm::Function* function, std::unordered_set<const llvm::Value*>& active,
        std::vector<NonLocalViewStoragePath>& candidates, unsigned depth = 0)
{
    if (value == nullptr || depth > 32) return false;
    value = value->stripPointerCasts();
    if (llvm::isa<llvm::ConstantPointerNull>(value)) return true;
    if (!active.insert(value).second) return false;
    bool resolved = true;
    if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(value))
    {
        std::vector<NonLocalViewStoragePath> bases;
        resolved = CollectNonLocalViewStorageCandidates(gep->getPointerOperand(), function,
            active, bases, depth + 1);
        if (resolved)
        {
            for (auto& path : bases)
                for (auto index = gep->idx_begin(); index != gep->idx_end(); ++index)
                    path.Indices.push_back(index->get());
            candidates.insert(candidates.end(), bases.begin(), bases.end());
        }
    }
    else if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(value))
    {
        const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(
            load->getPointerOperand()->stripPointerCasts());
        if (function == nullptr || slot == nullptr || !slot->getAllocatedType()->isPointerTy()
            || PointerSlotHasEscaped(slot))
            resolved = false;
        else
        {
            bool foundStore = false;
            for (const auto& block : *function)
                for (const auto& instruction : block)
                    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
                        store != nullptr
                        && store->getPointerOperand()->stripPointerCasts() == slot)
                    {
                        foundStore = true;
                        if (!CollectNonLocalViewStorageCandidates(store->getValueOperand(), function,
                                active, candidates, depth + 1))
                            resolved = false;
                        if (!resolved || candidates.size() > 8) break;
                    }
            if (!foundStore) resolved = false;
        }
    }
    else if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(value))
    {
        for (const auto& incoming : phi->incoming_values())
        {
            if (!CollectNonLocalViewStorageCandidates(incoming.get(), function, active,
                    candidates, depth + 1) || candidates.size() > 8)
            {
                resolved = false;
                break;
            }
        }
    }
    else if (const auto* select = llvm::dyn_cast<llvm::SelectInst>(value))
    {
        resolved = CollectNonLocalViewStorageCandidates(select->getTrueValue(), function,
                active, candidates, depth + 1)
            && CollectNonLocalViewStorageCandidates(select->getFalseValue(), function,
                active, candidates, depth + 1)
            && candidates.size() <= 8;
    }
    else if (llvm::isa<llvm::AllocaInst>(value))
        candidates.push_back({ value, {} });
    else
        resolved = false;
    active.erase(value);
    return resolved;
}

static void DeduplicateNonLocalViewStorageCandidates(
        std::vector<NonLocalViewStoragePath>& candidates)
{
    std::vector<NonLocalViewStoragePath> unique;
    for (auto& candidate : candidates)
        if (std::none_of(unique.begin(), unique.end(), [&](const auto& prior) {
                return SameProvenNonLocalViewPath(prior, candidate);
            }))
            unique.push_back(std::move(candidate));
    candidates = std::move(unique);
}

static bool SourceViewFieldRootAndPath(antlr4::tree::ParseTree* source,
        std::string& rootName, std::string& fieldPath)
{
    CFlatParser::MoveExpressionContext* move = nullptr;
    unsigned moveCount = 0;
    auto findMove = [&](auto&& self, antlr4::tree::ParseTree* node) -> void {
        if (node == nullptr) return;
        if (node != source
            && (dynamic_cast<CFlatParser::LambdaExpressionContext*>(node) != nullptr
                || dynamic_cast<CFlatParser::FunctionDefinitionContext*>(node) != nullptr)) return;
        if (auto* found = dynamic_cast<CFlatParser::MoveExpressionContext*>(node))
        {
            ++moveCount;
            if (moveCount > 1) { move = nullptr; return; }
            move = found;
            return;
        }
        for (auto* child : node->children) self(self, child);
    };
    findMove(findMove, source);
    if (moveCount != 1 || move == nullptr || move->unaryExpression() == nullptr) return false;
    auto* postfix = SolePostfixExpressionShape(move->unaryExpression());
    auto* primary = postfix != nullptr ? postfix->primaryExpression() : nullptr;
    auto* generic = primary != nullptr ? primary->genericIdentifier() : nullptr;
    if (postfix == nullptr || primary == nullptr || primary->children.size() != 1
        || generic == nullptr || generic->genericTypeParameters() != nullptr
        || generic->Identifier() == nullptr)
        return false;
    rootName = generic->Identifier()->getText();
    bool sawArrow = false;
    for (size_t i = 1; i < postfix->children.size();)
    {
        auto* token = dynamic_cast<antlr4::tree::TerminalNode*>(postfix->children[i]);
        if (token == nullptr) return false;
        const size_t tokenType = token->getSymbol()->getType();
        if (tokenType == CFlatParser::Arrow || tokenType == CFlatParser::Dot)
        {
            if (i + 1 >= postfix->children.size()) return false;
            auto* member = dynamic_cast<CFlatParser::MemberNameTokenContext*>(postfix->children[i + 1]);
            if (member == nullptr) return false;
            if (tokenType == CFlatParser::Arrow)
            {
                if (sawArrow || !fieldPath.empty()) return false;
                sawArrow = true;
                fieldPath = member->getText();
            }
            else
            {
                if (!sawArrow) return false;
                fieldPath += "." + member->getText();
            }
            i += 2;
            continue;
        }
        if (tokenType == CFlatParser::LeftBracket)
        {
            if (!sawArrow || i + 2 >= postfix->children.size()) return false;
            auto* expression = dynamic_cast<CFlatParser::ExpressionContext*>(postfix->children[i + 1]);
            auto* close = dynamic_cast<antlr4::tree::TerminalNode*>(postfix->children[i + 2]);
            auto* indexPostfix = SolePostfixExpressionShape(expression);
            auto* indexPrimary = indexPostfix != nullptr
                ? indexPostfix->primaryExpression() : nullptr;
            auto* literal = indexPrimary != nullptr ? indexPrimary->Constant() : nullptr;
            if (indexPostfix == nullptr || close == nullptr
                || close->getSymbol()->getType() != CFlatParser::RightBracket)
                return false;
            bool constantIndex = false;
            if (indexPostfix->children.size() == 1 && indexPrimary != nullptr
                && indexPrimary->children.size() == 1 && literal != nullptr)
            {
                const std::string digits = literal->getText();
                uint64_t index = 0;
                const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), index);
                if (parsed.ec == std::errc{} && parsed.ptr == digits.data() + digits.size())
                {
                    fieldPath += "[" + std::to_string(index) + "]";
                    constantIndex = true;
                }
            }
            if (!constantIndex) fieldPath += "[*]";
            i += 3;
            continue;
        }
        return false;
    }
    return sawArrow && !fieldPath.empty();
}

static bool CollectSourcePointerRootNames(antlr4::tree::ParseTree* value,
        std::vector<std::string>& names)
{
    if (value == nullptr) return false;
    if (auto* assignment = dynamic_cast<CFlatParser::AssignmentExpressionContext*>(value))
    {
        if (assignment->assignmentOperator() != nullptr) return false;
        return CollectSourcePointerRootNames(assignment->conditionalExpression(), names);
    }
    if (auto* conditional = dynamic_cast<CFlatParser::ConditionalExpressionContext*>(value))
    {
        if (conditional->expression() != nullptr && conditional->conditionalExpression() != nullptr)
            return CollectSourcePointerRootNames(conditional->expression(), names)
                && CollectSourcePointerRootNames(conditional->conditionalExpression(), names);
        if (conditional->QuestionQuestion() != nullptr) return false;
    }
    if (auto* primary = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(value))
    {
        // This is reached only for a well-typed pointer initializer/store; a bare constant there
        // is a null pointer constant, so it contributes no local root candidate.
        if (primary->Constant() != nullptr) return true;
        return false;
    }
    if (auto* unary = dynamic_cast<CFlatParser::UnaryExpressionContext*>(value))
    {
        if (unary->unaryOperator() != nullptr)
        {
            if (unary->unaryOperator()->getText() != "&") return false;
            const std::string name = SimpleIdentifierExpressionName(unary->castExpression());
            if (name.empty()) return false;
            names.push_back(name);
            return names.size() <= 8;
        }
    }
    auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(value);
    if (rule != nullptr && rule->children.size() == 1)
        return CollectSourcePointerRootNames(rule->children.front(), names);
    return false;
}

// Plain decimal literal text (no leading zero, no suffix) that fits int, or nullopt.
static std::optional<long long> CountedLoopLiteralValue(const std::string& text)
{
    if (text.empty() || text.size() > 10 || (text.size() > 1 && text[0] == '0')) return std::nullopt;
    for (char c : text)
        if (c < '0' || c > '9') return std::nullopt;
    const long long value = std::stoll(text);
    if (value > std::numeric_limits<int>::max()) return std::nullopt;
    return value;
}

// True when a break, continue, return or label starts before token anywhere under root.
static bool JumpOrLabelStartsBefore(antlr4::tree::ParseTree* root, size_t token)
{
    if (root == nullptr) return false;
    if (dynamic_cast<CFlatParser::JumpStatementContext*>(root) != nullptr
        || dynamic_cast<CFlatParser::LabeledStatementContext*>(root) != nullptr)
    {
        auto* ctx = dynamic_cast<antlr4::ParserRuleContext*>(root);
        if (ctx != nullptr && ctx->getStart() != nullptr
            && ctx->getStart()->getTokenIndex() < token)
            return true;
    }
    for (auto* child : root->children)
        if (JumpOrLabelStartsBefore(child, token)) return true;
    return false;
}

// `for (int NAME = N; NAME OP M; NAME++ or NAME += K)` with integer literals, OP one of
// <, <=, != and the first test true for N: the body runs at least once.
static bool CountedForRunsBodyOnce(CFlatParser::IterationStatementContext* loop)
{
    auto* condition = loop->forCondition();
    if (condition == nullptr || condition->forDeclaration() == nullptr
        || condition->assignmentExpression() == nullptr || condition->forExpression() == nullptr)
        return false;
    auto* list = condition->forDeclaration()->initDeclaratorList();
    if (list == nullptr || list->initDeclarator().size() != 1) return false;
    // Only a plain int loop variable holds every accepted literal unchanged.
    if (condition->forDeclaration()->declarationSpecifiers() == nullptr
        || condition->forDeclaration()->declarationSpecifiers()->getText() != "int") return false;
    auto* init = list->initDeclarator(0);
    auto* direct = init->declarator() != nullptr ? init->declarator()->directDeclarator() : nullptr;
    if (direct == nullptr || direct->Identifier() == nullptr
        || init->initializer() == nullptr
        || init->initializer()->assignmentExpression() == nullptr)
        return false;
    const std::string name = direct->Identifier()->getText();
    const auto start = CountedLoopLiteralValue(init->initializer()->assignmentExpression()->getText());
    const std::string test = condition->assignmentExpression()->getText();
    const std::string step = condition->forExpression()->getText();
    if (!start.has_value() || test.compare(0, name.size(), name) != 0) return false;
    bool stepOk = step == name + "++";
    if (!stepOk && step.compare(0, name.size() + 2, name + "+=") == 0)
        stepOk = CountedLoopLiteralValue(step.substr(name.size() + 2)).has_value();
    if (!stepOk) return false;
    const std::string rest = test.substr(name.size());
    for (const std::string op : {"<=", "!=", "<"})
    {
        if (rest.compare(0, op.size(), op) != 0) continue;
        const auto bound = CountedLoopLiteralValue(rest.substr(op.size()));
        if (!bound.has_value()) return false;
        if (op == "<=") return *start <= *bound;
        if (op == "!=") return *start != *bound;
        return *start < *bound;
    }
    return false;
}

// True when child is the body of a counted for that runs at least once, and no break,
// continue, return or label starts before node inside that body.
static bool IsRunOnceCountedForBody(CFlatParser::IterationStatementContext* loop,
        antlr4::tree::ParseTree* child, antlr4::tree::ParseTree* node)
{
    if (child == nullptr || child != loop->statement()) return false;
    auto* nodeContext = dynamic_cast<antlr4::ParserRuleContext*>(node);
    if (nodeContext == nullptr || nodeContext->getStart() == nullptr) return false;
    return CountedForRunsBodyOnce(loop)
        && !JumpOrLabelStartsBefore(loop->statement(), nodeContext->getStart()->getTokenIndex());
}

// True when node runs directly in block: no nested block, branch, loop, label, ?:, && or ||
// between them, so every path through block after node passes through node.
// A counted for body that always runs once (see CountedForRunsBodyOnce) does not break this.
static bool IsStraightLineInBlock(antlr4::tree::ParseTree* node,
        CFlatParser::CompoundStatementContext* block)
{
    if (node == nullptr || block == nullptr) return false;
    antlr4::tree::ParseTree* child = nullptr;
    for (auto* parent = node; parent != nullptr; child = parent, parent = parent->parent)
    {
        if (parent == block) return true;
        // A plain nested block runs unconditionally; branch and loop bodies are caught below.
        if (dynamic_cast<CFlatParser::LambdaExpressionContext*>(parent) != nullptr
            || dynamic_cast<CFlatParser::SelectionStatementContext*>(parent) != nullptr
            || dynamic_cast<CFlatParser::LabeledStatementContext*>(parent) != nullptr)
            return false;
        if (auto* loop = dynamic_cast<CFlatParser::IterationStatementContext*>(parent);
            loop != nullptr && !IsRunOnceCountedForBody(loop, child, node))
            return false;
        if (auto* conditional = dynamic_cast<CFlatParser::ConditionalExpressionContext*>(parent);
            conditional != nullptr && conditional->children.size() > 1)
            return false;
        if (auto* logicalAnd = dynamic_cast<CFlatParser::LogicalAndExpressionContext*>(parent);
            logicalAnd != nullptr && logicalAnd->inclusiveOrExpression().size() > 1)
            return false;
        if (auto* logicalOr = dynamic_cast<CFlatParser::LogicalOrExpressionContext*>(parent);
            logicalOr != nullptr && logicalOr->logicalAndExpression().size() > 1)
            return false;
    }
    return false;
}

// A use inside block (any depth of nested statements) that is not in a lambda body, which
// runs later on its own schedule.
static bool IsNestedInBlockWithoutLambda(antlr4::tree::ParseTree* node,
        CFlatParser::CompoundStatementContext* block)
{
    if (node == nullptr || block == nullptr) return false;
    for (auto* parent = node; parent != nullptr; parent = parent->parent)
    {
        if (parent == block) return true;
        if (dynamic_cast<CFlatParser::LambdaExpressionContext*>(parent) != nullptr) return false;
    }
    return false;
}

static bool IsAncestorOf(const antlr4::tree::ParseTree* ancestor, const antlr4::tree::ParseTree* node)
{
    for (auto* parent = node; parent != nullptr; parent = parent->parent)
        if (parent == ancestor) return true;
    return false;
}

// A store after the use reaches it again only through an enclosing loop that holds both but not
// the declaration (each iteration re-declares the pointer otherwise).
static bool StoreReachesUseThroughLoop(const antlr4::tree::ParseTree* store,
        const antlr4::tree::ParseTree* use, const antlr4::tree::ParseTree* init)
{
    for (auto* loop = store->parent; loop != nullptr; loop = loop->parent)
        if (dynamic_cast<const CFlatParser::IterationStatementContext*>(loop) != nullptr
            && IsAncestorOf(loop, use) && !IsAncestorOf(loop, init))
            return true;
    return false;
}

// `q`, `q + 0`, `q - 0`: the value and the candidate roots of q are unchanged.
static bool IsIdentityPointerValue(const std::string& text, const std::string& slotName)
{
    return text == slotName || text == slotName + "+0" || text == slotName + "-0";
}

static antlr4::tree::TerminalNode* FirstIdentifierTerminal(antlr4::tree::ParseTree* node)
{
    if (node == nullptr) return nullptr;
    if (auto* terminal = dynamic_cast<antlr4::tree::TerminalNode*>(node))
        return terminal->getSymbol()->getType() == CFlatParser::Identifier ? terminal : nullptr;
    for (auto* child : node->children)
        if (auto* found = FirstIdentifierTerminal(child)) return found;
    return nullptr;
}

static bool IsMemberNameTerminal(const antlr4::tree::ParseTree* terminal)
{
    for (auto* parent = terminal->parent; parent != nullptr; parent = parent->parent)
    {
        if (dynamic_cast<const CFlatParser::MemberNameTokenContext*>(parent) != nullptr) return true;
        if (dynamic_cast<const CFlatParser::CompoundStatementContext*>(parent) != nullptr) return false;
    }
    return false;
}

// A declaration is visible in its nearest compound or iteration statement.
static antlr4::tree::ParseTree* DeclarationScopeNode(antlr4::tree::ParseTree* declaration)
{
    for (auto* parent = declaration->parent; parent != nullptr; parent = parent->parent)
        if (dynamic_cast<CFlatParser::CompoundStatementContext*>(parent) != nullptr
            || dynamic_cast<CFlatParser::IterationStatementContext*>(parent) != nullptr)
            return parent;
    return nullptr;
}

// True when the reference names the alias binding: it follows the alias inside the alias scope
// and no other declaration of the name visible at the reference shadows it.
static bool ReferenceSeesAlias(antlr4::tree::TerminalNode* reference,
        CFlatParser::InitDeclaratorContext* alias, antlr4::tree::ParseTree* aliasScope,
        const std::vector<CFlatParser::InitDeclaratorContext*>& sameName)
{
    const size_t referenceToken = static_cast<size_t>(reference->getSymbol()->getTokenIndex());
    const size_t aliasToken = static_cast<size_t>(alias->getStart()->getTokenIndex());
    if (referenceToken <= aliasToken || !IsAncestorOf(aliasScope, reference)) return false;
    for (auto* other : sameName)
    {
        if (other == alias) continue;
        auto* scope = DeclarationScopeNode(other);
        const size_t otherToken = static_cast<size_t>(other->getStart()->getTokenIndex());
        if (scope == nullptr || otherToken >= referenceToken || !IsAncestorOf(scope, reference))
            continue;
        if (scope == aliasScope)
        {
            if (otherToken > aliasToken) return false;
        }
        else if (IsAncestorOf(aliasScope, scope))
            return false;
    }
    return true;
}

// Writes that reach an escaped pointer slot through a local alias `T** x = &slot;`. Accepted only
// for a closed alias: every reference that resolves to the alias is a plain `*x = value;` target,
// no lambda is present, and the alias is not a parameter-shadowing redeclaration.
static std::optional<std::vector<CFlatParser::AssignmentExpressionContext*>> CollectIndirectSlotStores(
        CFlatParser::FunctionDefinitionContext* function, const std::string& slotName)
{
    const std::string addressText = "&" + slotName;
    std::vector<CFlatParser::InitDeclaratorContext*> initDecls;
    std::vector<CFlatParser::InitDeclaratorContext*> aliasDecls;
    std::vector<CFlatParser::AssignmentExpressionContext*> assignments;
    std::vector<CFlatParser::UnaryExpressionContext*> stars;
    std::vector<antlr4::tree::TerminalNode*> identifiers;
    std::set<const antlr4::tree::ParseTree*> declIdentifiers;
    size_t addressCount = 0;
    bool hasLambda = false;
    auto walk = [&](auto&& self, antlr4::tree::ParseTree* node) -> void {
        if (node == nullptr) return;
        if (auto* terminal = dynamic_cast<antlr4::tree::TerminalNode*>(node))
        {
            if (terminal->getSymbol()->getType() == CFlatParser::Identifier)
                identifiers.push_back(terminal);
            return;
        }
        if (dynamic_cast<CFlatParser::LambdaExpressionContext*>(node) != nullptr) hasLambda = true;
        if (auto* init = dynamic_cast<CFlatParser::InitDeclaratorContext*>(node))
        {
            initDecls.push_back(init);
            auto* declarator = init->declarator();
            auto* direct = declarator != nullptr ? declarator->directDeclarator() : nullptr;
            if (direct != nullptr && direct->Identifier() != nullptr)
                declIdentifiers.insert(direct->Identifier());
            auto* initializer = init->initializer();
            if (direct != nullptr && direct->Identifier() != nullptr && initializer != nullptr
                && initializer->assignmentExpression() != nullptr
                && initializer->assignmentExpression()->getText() == addressText)
                aliasDecls.push_back(init);
        }
        if (auto* assignment = dynamic_cast<CFlatParser::AssignmentExpressionContext*>(node))
            assignments.push_back(assignment);
        if (auto* unary = dynamic_cast<CFlatParser::UnaryExpressionContext*>(node))
        {
            const std::string text = unary->getText();
            if (text == addressText) ++addressCount;
            if (!text.empty() && text[0] == '*') stars.push_back(unary);
        }
        for (auto* child : node->children) self(self, child);
    };
    walk(walk, function);
    if (hasLambda || aliasDecls.empty() || addressCount != aliasDecls.size()) return std::nullopt;

    // A declaration copying an alias (`T** y = x;`) is an alias too. Its initializer reference is
    // the copy itself, so it does not count as a use of x.
    std::set<const antlr4::tree::ParseTree*> copyTerminals;
    std::set<std::string> aliasNames;
    for (auto* alias : aliasDecls)
        aliasNames.insert(alias->declarator()->directDeclarator()->Identifier()->getText());
    for (bool grown = true; grown;)
    {
        grown = false;
        for (auto* init : initDecls)
        {
            auto* direct = init->declarator() != nullptr ? init->declarator()->directDeclarator() : nullptr;
            auto* initializer = init->initializer();
            if (direct == nullptr || direct->Identifier() == nullptr || initializer == nullptr
                || initializer->assignmentExpression() == nullptr) continue;
            const std::string name = direct->Identifier()->getText();
            auto* copied = FirstIdentifierTerminal(initializer->assignmentExpression());
            if (copied == nullptr || aliasNames.count(copied->getText()) == 0
                || aliasNames.count(name) != 0
                || initializer->assignmentExpression()->getText() != copied->getText()) continue;
            // The copied name must resolve to an alias binding, not a same-named shadow.
            bool seesAlias = false;
            for (auto* alias : aliasDecls)
            {
                const std::string aliasName = alias->declarator()->directDeclarator()->Identifier()->getText();
                if (aliasName != copied->getText()) continue;
                std::vector<CFlatParser::InitDeclaratorContext*> sameName;
                for (auto* decl : initDecls)
                    if (decl->declarator()->directDeclarator()->Identifier()->getText() == aliasName)
                        sameName.push_back(decl);
                auto* scope = DeclarationScopeNode(alias);
                if (scope != nullptr && ReferenceSeesAlias(copied, alias, scope, sameName))
                    seesAlias = true;
            }
            if (!seesAlias) continue;
            aliasDecls.push_back(init);
            aliasNames.insert(name);
            copyTerminals.insert(copied);
            grown = true;
        }
    }

    std::vector<CFlatParser::AssignmentExpressionContext*> aliasStores;
    for (auto* alias : aliasDecls)
    {
        const std::string name = alias->declarator()->directDeclarator()->Identifier()->getText();
        auto* scope = DeclarationScopeNode(alias);
        if (dynamic_cast<CFlatParser::CompoundStatementContext*>(scope) == nullptr) return std::nullopt;
        if (scope == function->compoundStatement())
            for (auto* terminal : identifiers)
                if (terminal->getText() == name && !IsAncestorOf(function->compoundStatement(), terminal))
                    return std::nullopt;
        std::vector<CFlatParser::InitDeclaratorContext*> sameName;
        for (auto* decl : initDecls)
            if (decl->declarator()->directDeclarator()->Identifier()->getText() == name)
                sameName.push_back(decl);
        auto resolves = [&](antlr4::tree::TerminalNode* terminal) {
            return terminal->getText() == name && declIdentifiers.count(terminal) == 0
                && copyTerminals.count(terminal) == 0 && !IsMemberNameTerminal(terminal)
                && ReferenceSeesAlias(terminal, alias, scope, sameName);
        };
        size_t references = 0;
        for (auto* terminal : identifiers)
            if (resolves(terminal)) ++references;
        size_t resolvedStars = 0;
        for (auto* star : stars)
        {
            auto* terminal = FirstIdentifierTerminal(star);
            if (star->getText() == "*" + name && terminal != nullptr && resolves(terminal))
                ++resolvedStars;
        }
        std::vector<CFlatParser::AssignmentExpressionContext*> plain;
        for (auto* assignment : assignments)
        {
            if (assignment->unaryExpression() == nullptr) continue;
            auto* terminal = FirstIdentifierTerminal(assignment->unaryExpression());
            if (assignment->unaryExpression()->getText() != "*" + name
                || terminal == nullptr || !resolves(terminal)) continue;
            if (assignment->assignmentOperator() == nullptr
                || assignment->assignmentOperator()->getText() != "=") return std::nullopt;
            plain.push_back(assignment);
        }
        if (resolvedStars != plain.size() || references != plain.size()) return std::nullopt;
        aliasStores.insert(aliasStores.end(), plain.begin(), plain.end());
    }
    return aliasStores;
}

static std::optional<std::vector<const llvm::Value*>> CollectSourcePointerRootCandidates(
        const LLVMBackend* compiler, const LLVMBackend::NamedVariable& arg,
        const std::string& rootName)
{
    if (compiler == nullptr || rootName.empty() || arg.SourceExpressionContext == nullptr)
        return std::nullopt;
    const auto rootRef = compiler->FindVariableStorageForViewAnalysis(rootName);
    const auto* pointerSlot = llvm::dyn_cast_or_null<llvm::AllocaInst>(rootRef.Storage);
    if (pointerSlot == nullptr || !pointerSlot->getAllocatedType()->isPointerTy())
        return std::nullopt;
    const bool slotEscaped = PointerSlotHasEscaped(pointerSlot);

    auto* function = dynamic_cast<CFlatParser::FunctionDefinitionContext*>(
        arg.SourceExpressionContext);
    for (auto* parent = arg.SourceExpressionContext->parent;
         function == nullptr && parent != nullptr; parent = parent->parent)
        function = dynamic_cast<CFlatParser::FunctionDefinitionContext*>(parent);
    if (function == nullptr || function->compoundStatement() == nullptr) return std::nullopt;
    const size_t useToken = static_cast<size_t>(arg.SourceExpressionContext->getStart()->getTokenIndex());
    std::vector<CFlatParser::InitDeclaratorContext*> declarations;
    std::vector<CFlatParser::AssignmentExpressionContext*> stores;
    std::vector<size_t> labelTokens;
    bool unsafeStore = false;
    auto scan = [&](auto&& self, antlr4::tree::ParseTree* node) -> void {
        if (node == nullptr) return;
        if (node != function->compoundStatement()
            && (dynamic_cast<CFlatParser::LambdaExpressionContext*>(node) != nullptr
                || dynamic_cast<CFlatParser::FunctionDefinitionContext*>(node) != nullptr)) return;
        if (auto* label = dynamic_cast<CFlatParser::LabeledStatementContext*>(node))
            labelTokens.push_back(static_cast<size_t>(label->getStart()->getTokenIndex()));
        if (auto* init = dynamic_cast<CFlatParser::InitDeclaratorContext*>(node))
        {
            auto* declarator = init->declarator();
            auto* direct = declarator != nullptr ? declarator->directDeclarator() : nullptr;
            if (direct != nullptr && direct->Identifier() != nullptr
                && direct->Identifier()->getText() == rootName)
                declarations.push_back(init);
        }
        if (auto* assignment = dynamic_cast<CFlatParser::AssignmentExpressionContext*>(node))
        {
            if (assignment->unaryExpression() != nullptr
                && SimpleIdentifierExpressionName(assignment->unaryExpression()) == rootName
                && assignment->assignmentOperator() != nullptr)
            {
                if (assignment->assignmentOperator()->getText() != "=") unsafeStore = true;
                stores.push_back(assignment);
            }
        }
        for (auto* child : node->children) self(self, child);
    };
    scan(scan, function->compoundStatement());
    if (slotEscaped)
    {
        auto aliasStores = CollectIndirectSlotStores(function, rootName);
        if (!aliasStores.has_value()) return std::nullopt;
        stores.insert(stores.end(), aliasStores->begin(), aliasStores->end());
        std::sort(stores.begin(), stores.end(), [](auto* left, auto* right) {
            return left->getStart()->getTokenIndex() < right->getStart()->getTokenIndex();
        });
    }
    // An identity store (`q = q + 0`) keeps the value, so it is not a candidate source.
    stores.erase(std::remove_if(stores.begin(), stores.end(), [&](auto* store) {
        return store->assignmentExpression() != nullptr
            && IsIdentityPointerValue(store->assignmentExpression()->getText(), rootName);
    }), stores.end());
    if (unsafeStore || declarations.size() != 1) return std::nullopt;
    auto* init = declarations.front();
    if (static_cast<size_t>(init->getStart()->getTokenIndex()) >= useToken) return std::nullopt;
    for (auto* store : stores)
        if (static_cast<size_t>(store->getStart()->getTokenIndex()) > useToken
            && StoreReachesUseThroughLoop(store, arg.SourceExpressionContext, init))
            return std::nullopt;
    auto* declaration = dynamic_cast<CFlatParser::DeclarationContext*>(init->parent);
    for (auto* parent = init->parent; declaration == nullptr && parent != nullptr;
         parent = parent->parent)
        declaration = dynamic_cast<CFlatParser::DeclarationContext*>(parent);
    auto* specs = declaration != nullptr ? declaration->declarationSpecifiers() : nullptr;
    bool pointerType = false;
    if (specs != nullptr)
        for (auto* spec : specs->declarationSpecifier())
            pointerType = pointerType || (spec->typeSpecifier() != nullptr && spec->pointer() != nullptr);
    auto* initializer = init->initializer();
    if (!pointerType || initializer == nullptr || initializer->assignmentExpression() == nullptr)
        return std::nullopt;

    // The declaring block is the nearest compound statement above the declaration. The last
    // store must run unconditionally in it, and no label may sit between init and use (a case
    // label could be jumped to past the store).
    CFlatParser::CompoundStatementContext* declBlock = nullptr;
    for (auto* parent = init->parent; declBlock == nullptr && parent != nullptr; parent = parent->parent)
        declBlock = dynamic_cast<CFlatParser::CompoundStatementContext*>(parent);
    const size_t initToken = static_cast<size_t>(init->getStart()->getTokenIndex());
    const bool labelBetween = std::any_of(labelTokens.begin(), labelTokens.end(),
        [&](size_t token) { return token > initToken && token < useToken; });
    // Stores after the use cannot reach it (checked above), so the last store is the last one
    // textually before it. A store textually before the declaration writes another variable.
    CFlatParser::AssignmentExpressionContext* lastStore = nullptr;
    for (auto* store : stores)
        if (static_cast<size_t>(store->getStart()->getTokenIndex()) < useToken) lastStore = store;
    const bool storeAfterInit = lastStore != nullptr
        && static_cast<size_t>(lastStore->getStart()->getTokenIndex()) > initToken;
    const bool lastStoreDominatesUse = !labelBetween
        && IsStraightLineInBlock(init, declBlock)
        && IsNestedInBlockWithoutLambda(arg.SourceExpressionContext, declBlock)
        && (!storeAfterInit || IsStraightLineInBlock(lastStore, declBlock));

    std::vector<std::string> rootNames;
    auto appendRoots = [&](antlr4::tree::ParseTree* source) {
        if (!CollectSourcePointerRootNames(source, rootNames)) return false;
        return rootNames.size() <= 8;
    };
    if (lastStoreDominatesUse)
    {
        if (!appendRoots(storeAfterInit
                ? lastStore->assignmentExpression()
                : initializer->assignmentExpression()))
            return std::nullopt;
    }
    else
    {
        if (!appendRoots(initializer->assignmentExpression())) return std::nullopt;
        for (auto* store : stores)
            if (!appendRoots(store->assignmentExpression())) return std::nullopt;
    }

    std::vector<const llvm::Value*> roots;
    for (const auto& name : rootNames)
    {
        const auto candidateRef = compiler->FindVariableStorageForViewAnalysis(name);
        const auto* candidate = llvm::dyn_cast_or_null<llvm::AllocaInst>(candidateRef.Storage);
        if (candidate == nullptr || !candidate->getAllocatedType()->isStructTy())
            return std::nullopt;
        if (std::find(roots.begin(), roots.end(), candidate) == roots.end())
            roots.push_back(candidate);
    }
    return roots;
}

static bool SameProvenNonLocalViewPath(const NonLocalViewStoragePath& left,
        const NonLocalViewStoragePath& right)
{
    if (left.Root != right.Root || left.Indices.size() != right.Indices.size()) return false;
    for (size_t i = 0; i < left.Indices.size(); ++i)
    {
        if (left.Indices[i] == right.Indices[i]) continue;
        const auto* leftConstant = llvm::dyn_cast<llvm::ConstantInt>(left.Indices[i]);
        const auto* rightConstant = llvm::dyn_cast<llvm::ConstantInt>(right.Indices[i]);
        if (leftConstant == nullptr || rightConstant == nullptr
            || leftConstant->getValue() != rightConstant->getValue()) return false;
    }
    return true;
}

static bool NormalizeNonLocalViewStorage(const llvm::Value* value,
        const llvm::Function* function, std::unordered_set<const llvm::Value*>& active,
        NonLocalViewStoragePath& path, unsigned depth = 0)
{
    if (value == nullptr || depth > 32) return false;
    value = value->stripPointerCasts();
    if (!active.insert(value).second)
    {
        path.Root = value;
        path.Indices.clear();
        return true;
    }
    bool resolved = true;
    if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(value))
    {
        resolved = NormalizeNonLocalViewStorage(gep->getPointerOperand(), function,
            active, path, depth + 1);
        if (resolved)
            for (auto index = gep->idx_begin(); index != gep->idx_end(); ++index)
                path.Indices.push_back(index->get());
    }
    else if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(value))
    {
        const llvm::Value* slot = load->getPointerOperand()->stripPointerCasts();
        const auto* localPointer = llvm::dyn_cast<llvm::AllocaInst>(slot);
        if (function != nullptr && localPointer != nullptr
            && localPointer->getAllocatedType()->isPointerTy()
            && !PointerSlotHasEscaped(localPointer))
        {
            bool foundStore = false;
            bool everyStoreAgrees = true;
            NonLocalViewStoragePath storedPath;
            for (const auto& block : *function)
                for (const auto& instruction : block)
                    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
                        store != nullptr && store->getPointerOperand()->stripPointerCasts() == slot)
                    {
                        NonLocalViewStoragePath candidate;
                        std::unordered_set<const llvm::Value*> nestedActive = active;
                        if (!NormalizeNonLocalViewStorage(store->getValueOperand(), function,
                                nestedActive, candidate, depth + 1))
                        {
                            everyStoreAgrees = false;
                            break;
                        }
                        if (!foundStore) storedPath = std::move(candidate);
                        else if (!SameProvenNonLocalViewPath(storedPath, candidate))
                            everyStoreAgrees = false;
                        foundStore = true;
                    }
            if (foundStore && everyStoreAgrees) path = std::move(storedPath);
            else
            {
                path.Root = slot;
                path.Indices.clear();
            }
        }
        else
            resolved = NormalizeNonLocalViewStorage(slot, function, active, path, depth + 1);
    }
    else
        path.Root = value;
    active.erase(value);
    return resolved;
}

bool LLVMBackend::SameNonLocalViewStorage(const llvm::Value* left,
        const llvm::Value* right) const
{
        if (left == right) return true;
        if (left == nullptr || right == nullptr) return false;
        const llvm::Function* function = builder != nullptr && builder->GetInsertBlock() != nullptr
            ? builder->GetInsertBlock()->getParent() : currentFunction;
        NonLocalViewStoragePath leftPath;
        NonLocalViewStoragePath rightPath;
        std::unordered_set<const llvm::Value*> leftActive;
        std::unordered_set<const llvm::Value*> rightActive;
        if (!NormalizeNonLocalViewStorage(left, function, leftActive, leftPath)
            || !NormalizeNonLocalViewStorage(right, function, rightActive, rightPath))
            return false;
        if (leftPath.Root != rightPath.Root
            || leftPath.Indices.size() != rightPath.Indices.size())
            return false;
        for (size_t i = 0; i < leftPath.Indices.size(); ++i)
        {
            const auto* leftIndex = leftPath.Indices[i];
            const auto* rightIndex = rightPath.Indices[i];
            if (leftIndex == rightIndex) continue;
            const auto* leftConstant = llvm::dyn_cast<llvm::ConstantInt>(leftIndex);
            const auto* rightConstant = llvm::dyn_cast<llvm::ConstantInt>(rightIndex);
            if (leftConstant != nullptr && rightConstant != nullptr
                && leftConstant->getValue() != rightConstant->getValue())
                return false;
        }
        return true;
}

void LLVMBackend::DisarmNonLocalViewFieldsForEscape(const llvm::Value* storage,
                                                     bool includeDescendants)
{
        if (storage == nullptr || builder == nullptr || !IsInsertBlockLive()) return;
        struct StoragePath
        {
            const llvm::Value* Root = nullptr;
            std::vector<const llvm::Value*> Indices;
        };
        auto normalize = [](const llvm::Value* value) {
            StoragePath path;
            while (value != nullptr)
            {
                value = value->stripPointerCasts();
                if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(value))
                {
                    value = load->getPointerOperand();
                    continue;
                }
                if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(value))
                {
                    std::vector<const llvm::Value*> segment;
                    for (auto index = gep->idx_begin(); index != gep->idx_end(); ++index)
                        segment.push_back(*index);
                    path.Indices.insert(path.Indices.begin(), segment.begin(), segment.end());
                    value = gep->getPointerOperand();
                    continue;
                }
                break;
            }
            path.Root = value != nullptr ? value->stripPointerCasts() : value;
            return path;
        };
        const StoragePath escapePath = normalize(storage);
        auto* function = builder->GetInsertBlock()->getParent();
        for (auto& state : nonLocalViewOwnershipStates_)
        {
            if (state.Function != function || state.RootStorage != escapePath.Root) continue;
            const StoragePath fieldPath = normalize(state.Storage);
            if (fieldPath.Indices.size() < escapePath.Indices.size()
                || (!includeDescendants && fieldPath.Indices.size() != escapePath.Indices.size()))
                continue;
            bool matches = true;
            for (size_t i = 0; i < escapePath.Indices.size(); ++i)
            {
                const auto* escapeIndex = escapePath.Indices[i];
                const auto* fieldIndex = fieldPath.Indices[i];
                if (escapeIndex == fieldIndex) continue;
                const auto* escapeConstant = llvm::dyn_cast<llvm::ConstantInt>(escapeIndex);
                const auto* fieldConstant = llvm::dyn_cast<llvm::ConstantInt>(fieldIndex);
                if (escapeConstant != nullptr && fieldConstant != nullptr
                    && escapeConstant->getValue() != fieldConstant->getValue())
                {
                    matches = false;
                    break;
                }
            }
            if (!matches) continue;
            if (state.OwnFlag != nullptr)
                builder->CreateStore(builder->getInt1(false), state.OwnFlag);
            if (state.RawArrayLengthStorage != nullptr)
                builder->CreateStore(builder->getInt64(-1), state.RawArrayLengthStorage);
        }
}

llvm::Value* LLVMBackend::RecomputeNonLocalViewFieldStorage(
        const NonLocalViewOwnershipState& state)
{
        if (state.RootStorage == nullptr || builder == nullptr) return nullptr;
        llvm::Value* storage = state.RootStorage;
        for (const auto& step : state.AddressPath)
        {
            if (step.IsLoad)
            {
                if (step.LoadType == nullptr) return nullptr;
                storage = builder->CreateLoad(step.LoadType, storage, "field.view.ptr");
                continue;
            }
            if (step.SourceType == nullptr) return nullptr;
            std::vector<llvm::Value*> indices;
            indices.reserve(step.Indices.size());
            for (auto* index : step.Indices)
            {
                if (auto* slot = llvm::dyn_cast_or_null<llvm::AllocaInst>(index))
                    indices.push_back(builder->CreateLoad(slot->getAllocatedType(), slot,
                                                          "field.view.index"));
                else
                    indices.push_back(index);
            }
            storage = builder->CreateGEP(step.SourceType, storage, indices, "field.view.addr");
        }
        return storage;
}

// Two element-path keys ("root|path") alias when the roots and paths match, or a dynamic index
// ('*') on either side names the same leaf field: a dynamic element may be any element.
static bool NonLocalPathKeysAlias(const std::string& left, const std::string& right)
{
        const size_t leftBar = left.find('|');
        const size_t rightBar = right.find('|');
        if (leftBar == std::string::npos || rightBar == std::string::npos) return false;
        if (left.compare(0, leftBar, right, 0, rightBar) != 0) return false;
        const std::string leftPath = left.substr(leftBar + 1);
        const std::string rightPath = right.substr(rightBar + 1);
        if (leftPath == rightPath) return true;
        if (leftPath.empty() || rightPath.empty()
            || (leftPath[0] != '*' && rightPath[0] != '*')) return false;
        auto leaf = [](const std::string& path) {
            const size_t dot = path.rfind('.');
            return dot == std::string::npos ? path : path.substr(dot + 1);
        };
        return leaf(leftPath) == leaf(rightPath);
}

std::string LLVMBackend::NonLocalElementPathKey(const NamedVariable& nv) const
{
        // Only element-field paths need a name key; every other slot matches by storage.
        if (nv.FieldName.empty() || nv.FieldPathRoot.empty()) return {};
        if (!nv.MovePathHasDynamicIndex && nv.MovePathKey.find('[') == std::string::npos)
            return {};
        // Key the root by its resolved binding: same-named locals in nested scopes are distinct.
    const llvm::Value* rootStorage = FindVariableStorage(nv.FieldPathRoot).Storage;
    const auto* function = builder != nullptr && builder->GetInsertBlock() != nullptr
        ? builder->GetInsertBlock()->getParent() : currentFunction;
    const auto* pointerSlot = llvm::dyn_cast_or_null<llvm::AllocaInst>(rootStorage);
    if (function != nullptr && pointerSlot != nullptr && !PointerSlotHasEscaped(pointerSlot)
        && pointerSlot->getAllocatedType()->isPointerTy())
    {
        const llvm::Value* resolvedRoot = nullptr;
        bool foundStore = false;
        bool everyStoreAgrees = true;
        for (const auto& block : *function)
            for (const auto& instruction : block)
                if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
                    store != nullptr
                    && store->getPointerOperand()->stripPointerCasts() == pointerSlot)
                {
                    NonLocalViewStoragePath target;
                    std::unordered_set<const llvm::Value*> active;
                    if (!NormalizeNonLocalViewStorage(store->getValueOperand(), function,
                            active, target)
                        || !llvm::isa_and_nonnull<llvm::AllocaInst>(target.Root))
                    {
                        everyStoreAgrees = false;
                        break;
                    }
                    for (const auto* index : target.Indices)
                    {
                        const auto* constant = llvm::dyn_cast<llvm::ConstantInt>(index);
                        if (constant == nullptr || !constant->isZero())
                            everyStoreAgrees = false;
                    }
                    if (!everyStoreAgrees) break;
                    if (resolvedRoot == nullptr) resolvedRoot = target.Root;
                    else if (resolvedRoot != target.Root) everyStoreAgrees = false;
                    foundStore = true;
                }
        if (foundStore && everyStoreAgrees) rootStorage = resolvedRoot;
    }
        const std::string root = rootStorage != nullptr
            ? std::to_string(reinterpret_cast<uintptr_t>(rootStorage))
            : "name:" + nv.FieldPathRoot;
        if (nv.MovePathHasDynamicIndex)
            return root + "|*." + nv.FieldName;
        return root + "|" + nv.MovePathKey;
}

void LLVMBackend::TrackNonLocalViewAssignment(llvm::Value* storage, llvm::Value* value,
        const llvm::Value* sourceStorage, bool sourceTransfersOwnership,
        const std::string& pathKey, bool sourceKnownBorrowed,
        const NamedVariable* destination, const NamedVariable* source)
{
        if (storage == nullptr || builder == nullptr || builder->GetInsertBlock() == nullptr)
            return;
        if (sourceStorage != nullptr && SameNonLocalViewStorage(storage, sourceStorage))
            return;
        bool mayBeBorrowed = !sourceTransfersOwnership;
        if (const auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(value);
            constant != nullptr && constant->isNullValue())
            mayBeBorrowed = false;
        // A tracked borrowed source is proof; the moved value's raw flags must not override it.
        else if (sourceKnownBorrowed)
            mayBeBorrowed = true;
        else if (sourceTransfersOwnership)
        {
            if (const auto* raw = FindRawArrayResult(value))
            {
                if (raw->Owns) mayBeBorrowed = false;
                // A tracked move source proves the transfer; a moved global's raw flags read as not
                // owned. A mixed join (no tracked source) keeps the raw borrowed signal.
                else if (sourceStorage != nullptr) mayBeBorrowed = false;
                else if (const auto* owns = llvm::dyn_cast_or_null<llvm::ConstantInt>(raw->RuntimeOwns))
                    mayBeBorrowed = owns->isZero();
                else mayBeBorrowed = true;
            }
        }
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        auto stateIt = std::ranges::find_if(nonLocalViewOwnershipStates_, [&](const auto& state) {
            return state.Function == function && SameNonLocalViewStorage(state.Storage, storage);
        });
        if (stateIt == nonLocalViewOwnershipStates_.end())
        {
            nonLocalViewOwnershipStates_.push_back({ storage, function, mayBeBorrowed, pathKey });
            stateIt = std::prev(nonLocalViewOwnershipStates_.end());
        }
        stateIt->MayBeBorrowed = mayBeBorrowed;

        if (destination == nullptr || !destination->TypeAndValue.IsArrayView
            || destination->FieldPathRoot.empty()) return;
        const auto rootRef = FindVariableStorage(destination->FieldPathRoot);
        const llvm::Value* rootStorage = rootRef.Storage;
        auto* root = rootStorage != nullptr
            ? FindStackVariableByStorage(rootStorage) : nullptr;
        if (root == nullptr || root->BaseType == nullptr || !root->BaseType->isStructTy()
            || !llvm::isa<llvm::AllocaInst>(root->Storage)) return;

        stateIt->RootStorage = root->Storage;
        stateIt->FieldMovePath = FieldMoveKey(*destination);
        stateIt->FieldType = destination->TypeAndValue;
        stateIt->BaseType = destination->BaseType;
        std::vector<NonLocalViewAddressStep> sourceAddressPath;
        const llvm::Value* pathValue = storage->stripPointerCasts();
        bool completeAddressPath = true;
        while (pathValue != root->Storage)
        {
            if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(pathValue))
            {
                NonLocalViewAddressStep step;
                step.SourceType = gep->getSourceElementType();
                for (auto index = gep->idx_begin(); index != gep->idx_end(); ++index)
                    step.Indices.push_back(index->get());
                sourceAddressPath.push_back(std::move(step));
                pathValue = gep->getPointerOperand()->stripPointerCasts();
            }
            else if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(pathValue))
            {
                NonLocalViewAddressStep step;
                step.IsLoad = true;
                step.LoadType = load->getType();
                sourceAddressPath.push_back(std::move(step));
                pathValue = load->getPointerOperand()->stripPointerCasts();
            }
            else
            {
                completeAddressPath = false;
                break;
            }
        }
        if (completeAddressPath)
        {
            std::ranges::reverse(sourceAddressPath);
            if (stateIt->AddressPath.size() != sourceAddressPath.size())
                stateIt->AddressPath.clear();
            if (stateIt->AddressPath.empty())
            {
                stateIt->AddressPath = sourceAddressPath;
                for (auto& step : stateIt->AddressPath)
                    if (!step.IsLoad)
                        for (auto& index : step.Indices)
                            if (!llvm::isa<llvm::Constant>(index))
                            {
                                auto* indexStorage = AllocaAtEntry(index->getType(), nullptr,
                                                                   "field.view.path.index");
                                builder->CreateStore(index, indexStorage);
                                index = indexStorage;
                            }
            }
            else
            {
                for (size_t i = 0; i < sourceAddressPath.size(); ++i)
                {
                    if (sourceAddressPath[i].IsLoad || stateIt->AddressPath[i].IsLoad) continue;
                    auto& storedIndices = stateIt->AddressPath[i].Indices;
                    const auto& currentIndices = sourceAddressPath[i].Indices;
                    if (storedIndices.size() != currentIndices.size()) continue;
                    for (size_t j = 0; j < storedIndices.size(); ++j)
                        if (llvm::isa<llvm::AllocaInst>(storedIndices[j]))
                            builder->CreateStore(currentIndices[j], storedIndices[j]);
                }
            }
        }
        if (stateIt->OwnFlag == nullptr)
        {
            stateIt->OwnFlag = AllocaAtEntry(builder->getInt1Ty(), nullptr, "field.view.owns");
            stateIt->RawArrayLengthStorage = AllocaAtEntry(
                builder->getInt64Ty(), nullptr, "field.view.raw_array_count");
            llvm::IRBuilderBase::InsertPointGuard guard(*builder);
            auto* lastAlloca = stateIt->OwnFlag->comesBefore(stateIt->RawArrayLengthStorage)
                ? stateIt->RawArrayLengthStorage : stateIt->OwnFlag;
            builder->SetInsertPoint(lastAlloca->getParent(),
                                    std::next(lastAlloca->getIterator()));
            builder->CreateStore(builder->getInt1(false), stateIt->OwnFlag);
            builder->CreateStore(builder->getInt64(-1), stateIt->RawArrayLengthStorage);
        }

        llvm::Value* owns = builder->getInt1(false);
        llvm::Value* count = builder->getInt64(-1);
        llvm::Value* refCount = nullptr;
        uint64_t allocAlignment = 0;
        if (const auto* raw = FindRawArrayResult(value))
        {
            if (sourceTransfersOwnership)
            {
                owns = raw->RuntimeOwns != nullptr
                    ? raw->RuntimeOwns : builder->getInt1(raw->Owns);
                if (raw->Count != nullptr) count = Upconvert(raw->Count, builder->getInt64Ty());
                allocAlignment = raw->AllocAlign;
            }
        }
        else if (sourceTransfersOwnership && sourceStorage != nullptr)
        {
            auto sourceField = std::ranges::find_if(nonLocalViewOwnershipStates_, [&](const auto& state) {
                return state.Function == function && state.Storage != storage
                    && SameNonLocalViewStorage(state.Storage, sourceStorage)
                    && state.OwnFlag != nullptr;
            });
            if (sourceField != nonLocalViewOwnershipStates_.end())
            {
                owns = builder->CreateLoad(builder->getInt1Ty(), sourceField->OwnFlag);
                count = builder->CreateLoad(builder->getInt64Ty(), sourceField->RawArrayLengthStorage);
                refCount = sourceField->RefCountStorage;
                allocAlignment = sourceField->AllocAlignment;
                builder->CreateStore(builder->getInt1(false), sourceField->OwnFlag);
            }
        }
        if (source != nullptr && source->Storage != nullptr)
        {
            if (const auto* live = FindVariableByStorage(source->Storage))
            {
                if (sourceTransfersOwnership && live->RefCountStorage != nullptr)
                {
                    owns = builder->getInt1(true);
                    refCount = live->RefCountStorage;
                }
                if (sourceTransfersOwnership && live->RawArrayLengthStorage != nullptr)
                    count = LoadRawArrayLength(*live);
                if (sourceTransfersOwnership) allocAlignment = live->AllocAlignment;
            }
        }
        stateIt->RefCountStorage = refCount;
        stateIt->AllocAlignment = allocAlignment;
        builder->CreateStore(owns, stateIt->OwnFlag);
        builder->CreateStore(count, stateIt->RawArrayLengthStorage);
}

std::optional<bool> LLVMBackend::NonLocalViewMayBeBorrowed(const llvm::Value* storage,
        const std::string& pathKey) const
{
        if ((storage == nullptr && pathKey.empty())
            || builder == nullptr || builder->GetInsertBlock() == nullptr)
            return std::nullopt;
        const auto* function = builder->GetInsertBlock()->getParent();
        std::vector<NonLocalViewStoragePath> candidates;
        std::unordered_set<const llvm::Value*> active;
        const bool provenCandidates = storage != nullptr
            && CollectNonLocalViewStorageCandidates(storage, function, active, candidates);
        if (provenCandidates) DeduplicateNonLocalViewStorageCandidates(candidates);
        // A dynamic index may name any aliased element: every candidate counts, and one
        // may-borrowed candidate outweighs any number of owned ones.
        std::optional<bool> result;
        for (const auto& state : nonLocalViewOwnershipStates_)
        {
            if (state.Function != function) continue;
            bool sameSlot = storage != nullptr && SameNonLocalViewStorage(state.Storage, storage);
            if (!sameSlot && provenCandidates)
            {
                NonLocalViewStoragePath statePath;
                std::unordered_set<const llvm::Value*> stateActive;
                if (NormalizeNonLocalViewStorage(state.Storage, function, stateActive, statePath))
                    sameSlot = std::any_of(candidates.begin(), candidates.end(), [&](const auto& candidate) {
                        return SameProvenNonLocalViewPath(candidate, statePath);
                    });
            }
            const bool samePath = !pathKey.empty() && !state.PathKey.empty()
                && NonLocalPathKeysAlias(state.PathKey, pathKey);
            if (!sameSlot && !samePath) continue;
            if (state.MayBeBorrowed) return true;
            result = false;
        }
        return result;
}

std::optional<bool> LLVMBackend::NonLocalViewFieldMayBeBorrowed(
        const llvm::Value* rootStorage, const std::string& fieldPath) const
{
        if (rootStorage == nullptr || fieldPath.empty()
            || builder == nullptr || builder->GetInsertBlock() == nullptr)
            return std::nullopt;
        const auto* function = builder->GetInsertBlock()->getParent();
        std::vector<NonLocalViewStoragePath> roots;
        std::unordered_set<const llvm::Value*> active;
        if (!CollectNonLocalViewStorageCandidates(rootStorage, function, active, roots))
            return std::nullopt;
        DeduplicateNonLocalViewStorageCandidates(roots);
        if (roots.empty() || std::any_of(roots.begin(), roots.end(), [](const auto& root) {
                return root.Root == nullptr || !root.Indices.empty()
                    || !llvm::isa<llvm::AllocaInst>(root.Root);
            })) return std::nullopt;
        std::optional<bool> result;
        for (const auto& state : nonLocalViewOwnershipStates_)
        {
            if (state.Function != function
                || !NonLocalFieldPathMatches(fieldPath, state.FieldMovePath)
                || std::none_of(roots.begin(), roots.end(), [&](const auto& root) {
                    return state.RootStorage == root.Root;
                }))
                continue;
            if (state.MayBeBorrowed) return true;
            result = false;
        }
        return result;
}

std::optional<bool> LLVMBackend::NonLocalViewFieldCandidatesMayBeBorrowed(
        const std::vector<const llvm::Value*>& rootStorages,
        const std::string& fieldPath) const
{
        if (rootStorages.empty() || fieldPath.empty()
            || builder == nullptr || builder->GetInsertBlock() == nullptr)
            return std::nullopt;
        const auto* function = builder->GetInsertBlock()->getParent();
        std::optional<bool> result;
        for (const auto& state : nonLocalViewOwnershipStates_)
        {
            if (state.Function != function
                || !NonLocalFieldPathMatches(fieldPath, state.FieldMovePath)
                || std::find(rootStorages.begin(), rootStorages.end(), state.RootStorage)
                    == rootStorages.end())
                continue;
            if (state.MayBeBorrowed) return true;
            result = false;
        }
        return result;
}

std::optional<bool> LLVMBackend::NonLocalViewFieldAnyRootMayBeBorrowed(
        const std::string& fieldPath) const
{
        if (fieldPath.empty() || builder == nullptr || builder->GetInsertBlock() == nullptr)
            return std::nullopt;
        const auto* function = builder->GetInsertBlock()->getParent();
        std::optional<bool> result;
        for (const auto& state : nonLocalViewOwnershipStates_)
        {
            if (state.Function != function
                || !NonLocalFieldPathMatches(fieldPath, state.FieldMovePath))
                continue;
            if (state.MayBeBorrowed) return true;
            result = false;
        }
        return result;
}

std::optional<bool> LLVMBackend::NonLocalViewFieldStorageMayBeBorrowed(
        const llvm::Value* fieldStorage, const std::string& fieldPath) const
{
        if (fieldStorage == nullptr || fieldPath.empty()
            || builder == nullptr || builder->GetInsertBlock() == nullptr)
            return std::nullopt;
        const auto* function = builder->GetInsertBlock()->getParent();
        std::vector<NonLocalViewStoragePath> fieldAddressPaths;
        std::unordered_set<const llvm::Value*> active;
        if (!CollectNonLocalViewStorageCandidates(fieldStorage, function, active, fieldAddressPaths))
            return std::nullopt;
        DeduplicateNonLocalViewStorageCandidates(fieldAddressPaths);
        if (fieldAddressPaths.empty() || std::any_of(fieldAddressPaths.begin(), fieldAddressPaths.end(),
                [](const auto& path) {
                    return path.Root == nullptr || !llvm::isa<llvm::AllocaInst>(path.Root);
                })) return std::nullopt;
        std::optional<bool> result;
        for (const auto& state : nonLocalViewOwnershipStates_)
        {
            if (state.Function != function
                || !NonLocalFieldPathMatches(fieldPath, state.FieldMovePath)
                || std::none_of(fieldAddressPaths.begin(), fieldAddressPaths.end(), [&](const auto& path) {
                    return state.RootStorage == path.Root;
                }))
                continue;
            if (state.MayBeBorrowed) return true;
            result = false;
        }
        return result;
}

void LLVMBackend::CheckOwnFlagLoopBackEdge()
{
        if (ownFlagLoopReownSnapshots_.empty()
            || !IsInsertBlockLive() || IsCurrentBlockUnreachable())
            return;
        const auto& frame = ownFlagLoopReownSnapshots_.back();
        for (const auto& sink : frame.uniqueOwnerSinks)
        {
            const auto* live = FindVariableByStorage(sink.Storage);
            if (live == nullptr) continue;
            bool owningLocalCopyWasRebound = false;
            if (live->BorrowsOwningLocal && live->OwningLocalStorage != nullptr)
            {
                auto* owner = FindVariableByStorage(live->OwningLocalStorage);
                owningLocalCopyWasRebound = live->PointerRebound
                    || (owner != nullptr && owner->PointerRebound);
            }
            const bool localMayBeBorrowed = live->IsBorrowed || live->IsAliasBorrow
                || live->TypeAndValue.IsAlias || live->BorrowsOwnedElement
                || (live->BorrowsOwningLocal && !owningLocalCopyWasRebound);
            if (!localMayBeBorrowed) continue;
            ReportingFileScope report(this, sink.File, sink.Line, sink.Column);
            const std::string sourceName = BorrowedSourceName(*live);
            const std::string action = sink.IsReset ? "reset" : "initialize";
            if (sourceName.empty())
                LogError(std::format("cannot {} {} '{}' from a borrowed value - the source still owns it; "
                    "use 'new', a 'move' expression, or a move-returning call",
                    action, sink.UniqueType, sink.Destination));
            else
                LogError(std::format("cannot {} {} '{}' from borrowed value '{}' - the source still owns it; "
                    "use 'new', a 'move' expression, or a move-returning call",
                    action, sink.UniqueType, sink.Destination, sourceName));
            return;
        }
        for (const auto& sink : frame.moveOwnerSinks)
        {
            const auto* live = FindVariableByStorage(sink.Storage);
            if (live == nullptr) continue;
            bool owningLocalCopyWasRebound = false;
            if (live->BorrowsOwningLocal && live->OwningLocalStorage != nullptr)
            {
                auto* owner = FindVariableByStorage(live->OwningLocalStorage);
                owningLocalCopyWasRebound = live->PointerRebound
                    || (owner != nullptr && owner->PointerRebound);
            }
            const bool localMayBeBorrowed = live->IsBorrowed || live->IsAliasBorrow
                || live->TypeAndValue.IsAlias || live->BorrowsOwnedElement
                || (live->BorrowsOwningLocal && !owningLocalCopyWasRebound);
            if (!localMayBeBorrowed) continue;
            ReportingFileScope report(this, sink.File, sink.Line, sink.Column);
            const std::string sourceName = FindVariableNameByStorage(sink.Storage);
            const std::string borrowedSource = BorrowedSourceName(*live);
            LogError(std::format(
                "cannot pass borrowed pointer '{}' to move parameter '{}' of '{}' - it may "
                "refer to borrowed storage '{}', which the sink does not own",
                sourceName.empty() ? borrowedSource : sourceName, sink.Parameter,
                SpellFunctionSymbol(*this, sink.Function), borrowedSource));
            return;
        }
        for (const auto& sink : frame.moveSinkViews)
        {
            const auto* live = FindVariableByStorage(sink.Storage);
            const bool localMayBeBorrowed = live != nullptr
                && live->TypeAndValue.IsArrayView && live->OwnFlagMayBeFalse;
            const bool nonLocalMayBeBorrowed = NonLocalViewMayBeBorrowed(
                sink.Storage, sink.PathKey).value_or(false);
            if (!localMayBeBorrowed && !nonLocalMayBeBorrowed) continue;
            ReportingFileScope report(this, sink.File, sink.Line, sink.Column);
            LogError(sink.Diagnostic);
            return;
        }
}

void LLVMBackend::DiagnoseBorrowedMoveSinkArg(const std::string& functionName,
        const std::string& paramName, const NamedVariable& arg)
{
        // The argument NV is rebuilt by the expression walk, so the borrow bits live on the
        // binding itself - ask the live variable, not the copy. Unknown provenance ACCEPTS.
        const llvm::Value* moveSourceStorage = arg.MoveSourceStorage != nullptr
            ? arg.MoveSourceStorage : arg.Storage;
        const bool storageIsLocalSlot = moveSourceStorage != nullptr
            && llvm::isa<llvm::AllocaInst>(moveSourceStorage);
        const NamedVariable* live = FindVariableByStorage(moveSourceStorage);
        const bool allJoinArmsKeepOwner = live != nullptr && !live->IsOwning
            && JoinArmsStillKeepOwner(*live) && !live->JoinKeepsOwnerSource.empty();

        // Non-local views have no runtime own flag; preserve the last assignment's proof instead.
        const auto nonLocalFieldMayBeBorrowed = arg.TypeAndValue.IsArrayView
            ? NonLocalViewFieldStorageMayBeBorrowed(moveSourceStorage,
                arg.MoveSourceFieldPath.empty() ? FieldMoveKey(arg) : arg.MoveSourceFieldPath)
            : std::optional<bool>{};
        std::string sourceRootName;
        std::string sourceFieldPath;
        const bool hasSourceFieldPath = arg.TypeAndValue.IsArrayView
            && SourceViewFieldRootAndPath(arg.SourceExpressionContext,
                sourceRootName, sourceFieldPath);
        const auto sourceRootCandidates = hasSourceFieldPath
            ? CollectSourcePointerRootCandidates(this, arg, sourceRootName)
            : std::optional<std::vector<const llvm::Value*>>{};
        const std::string candidateFieldPath = hasSourceFieldPath ? sourceFieldPath
            : (!arg.MoveSourceFieldPath.empty() ? arg.MoveSourceFieldPath
                : (!arg.MovePathKey.empty() ? arg.MovePathKey : FieldMoveKey(arg)));
        // Undecidable provenance may reach any root: refuse when any mixed root of this function
        // has the moved path. Only a fully decided candidate set narrows the check.
        const auto sourceRootCandidatesMayBeBorrowed = sourceRootCandidates.has_value()
            ? NonLocalViewFieldCandidatesMayBeBorrowed(*sourceRootCandidates, candidateFieldPath)
            : (hasSourceFieldPath
                ? NonLocalViewFieldAnyRootMayBeBorrowed(candidateFieldPath)
                : std::optional<bool>{});
        const bool nonLocalMayBeBorrowed = !storageIsLocalSlot
            && arg.TypeAndValue.IsArrayView
            && NonLocalViewMayBeBorrowed(moveSourceStorage, arg.MoveSourcePathKey.empty()
                ? NonLocalElementPathKey(arg) : arg.MoveSourcePathKey).value_or(false);
        const bool nonLocalFieldMayBeBorrowedValue = nonLocalFieldMayBeBorrowed.value_or(false);
        const bool candidateFieldMayBeBorrowedValue = sourceRootCandidatesMayBeBorrowed.value_or(false);
        const bool localMayBeBorrowed = live != nullptr
            && (live->OwnFlagMayBeFalse || allJoinArmsKeepOwner);
        auto makeDiagnostic = [&]() {
            const std::string sourceName = !arg.FieldPathText.empty()
                ? arg.FieldPathText
                : (arg.CallerName.empty() ? FindVariableNameByStorage(arg.Storage)
                                          : arg.CallerName);
            return std::format(
                "cannot move array view '{}' into move parameter '{}' of '{}' - the view may be "
                "borrowed on a runtime path, so the sink could free storage the real owner still "
                "holds. Reassign it from an owned source before passing it to a move parameter.",
                sourceName.empty() ? SpellType(*this, arg.TypeAndValue) : sourceName,
                paramName, SpellFunctionSymbol(*this, functionName));
        };
        if (arg.TypeAndValue.IsArrayView
            && (localMayBeBorrowed || nonLocalMayBeBorrowed || nonLocalFieldMayBeBorrowedValue
                || candidateFieldMayBeBorrowedValue))
        {
            LogError(makeDiagnostic());
            return;
        }
        if (arg.TypeAndValue.IsArrayView && !ownFlagLoopReownSnapshots_.empty())
        {
            const std::string rootName = !arg.FieldPathRoot.empty()
                ? arg.FieldPathRoot : arg.CallerName;
            const llvm::Value* scopeStorage = moveSourceStorage;
            if (!rootName.empty())
                if (auto* resolved = FindVariableStorage(rootName).Storage) scopeStorage = resolved;

            bool hasScope = false;
            size_t scopeIndex = 0;
            for (size_t i = 0; i < stackNamedVariable.size() && !hasScope; ++i)
            {
                const auto& scope = stackNamedVariable[i];
                for (const auto& [name, variable] : scope.functionArgument)
                    if (variable.Storage == scopeStorage) { scopeIndex = i; hasScope = true; break; }
                if (hasScope) break;
                for (const auto& [name, variable] : scope.namedVariable)
                    if (variable.Storage == scopeStorage) { scopeIndex = i; hasScope = true; break; }
            }
            const bool isGlobal = llvm::isa_and_nonnull<llvm::GlobalVariable>(scopeStorage);
            if (hasScope || isGlobal)
            {
                const std::string pathKey = arg.MoveSourcePathKey.empty()
                    ? NonLocalElementPathKey(arg) : arg.MoveSourcePathKey;
                const std::string diagnostic = makeDiagnostic();
                for (auto& frame : ownFlagLoopReownSnapshots_)
                    if (isGlobal || scopeIndex < frame.scopeDepth)
                        frame.moveSinkViews.push_back({ moveSourceStorage, pathKey, diagnostic,
                            sourceFileName, currentLine, currentColumn });
            }
        }
        if (!storageIsLocalSlot || !arg.FieldName.empty()) return;
        const bool argOwns = arg.IsOwning || arg.IsOwningStruct || arg.IsOwningString
            || arg.OwnsInterfaceBox
            || (live != nullptr && (live->IsOwning || live->IsOwningStruct
                                    || live->IsOwningString || live->IsNewAllocated))
            || (!arg.CallerName.empty() && IsVariableOwning(arg.CallerName));
        if (argOwns || BorrowProofRetiredByRebind(arg)
            || (live != nullptr && BorrowProofRetiredByRebind(*live)))
            return;
        const bool directBorrowParam = !arg.CallerName.empty()
            && IsFunctionParameter(arg.CallerName);
        // A local read out of a FIELD of the borrowed parameter copies an OWNING SLOT, not the
        // parameter - that is exactly the legal idiom, so it is not reached by this rule.
        const bool throughField = arg.BorrowedThroughField
            || (live != nullptr && live->BorrowedThroughField);
        const std::string borrowOrigin = throughField ? std::string()
            : (!arg.BorrowedOrigin.empty()
               ? arg.BorrowedOrigin
               : (live != nullptr && live->IsBorrowed ? live->BorrowedOrigin : std::string()));
        const bool copiedFromBorrowParam = !directBorrowParam
            && !borrowOrigin.empty() && IsFunctionParameter(borrowOrigin);
        if (directBorrowParam || copiedFromBorrowParam)
        {
            const std::string sourceName = directBorrowParam
                ? std::format("borrowed parameter '{}'", arg.CallerName)
                : std::format("borrowed pointer '{}' (copied from parameter '{}')",
                              arg.CallerName.empty() ? borrowOrigin : arg.CallerName,
                              borrowOrigin);
            const std::string declName = directBorrowParam ? arg.CallerName : borrowOrigin;
            LogBorrowedPointerMoveSinkError(sourceName, paramName,
                SpellFunctionSymbol(*this, functionName), declName);
            return;
        }
        // A plain copy of a live OWNING local (`P* view = n;`) is the same double free.
        const NamedVariable& copy = live != nullptr ? *live : arg;
        if (!directBorrowParam && !throughField && copy.BorrowsOwningLocal
            && OwningLocalCopyStillAliases(copy))
            LogOwningLocalCopyMoveSinkError(
                arg.CallerName.empty() ? FindVariableNameByStorage(arg.Storage) : arg.CallerName,
                copy.OwningLocalOrigin, paramName, SpellFunctionSymbol(*this, functionName),
                copy.OwningLocalStorage);
}

void LLVMBackend::DiagnoseBorrowedViewFieldMoveArg(const std::string& functionName,
        const std::string& paramName, const NamedVariable& arg,
        const std::vector<std::string>& fieldPaths, bool isMethodReceiver)
{
        if ((!arg.TypeAndValue.Pointer && !arg.TypeAndValue.ElemPointer)
            && !isMethodReceiver) return;
        if (fieldPaths.empty()) return;
        const llvm::Value* rootStorage = arg.Primary != nullptr
            && arg.Primary->getType()->isPointerTy() ? arg.Primary : arg.Storage;
        if (rootStorage == nullptr || !rootStorage->getType()->isPointerTy()) return;
        // A summarized helper may move through any root the pointer argument can hold, so the
        // candidate roots of a plain pointer identifier are checked as well.
        const std::string argName = arg.SourceExpressionContext != nullptr
            ? std::string(SimpleIdentifierExpressionName(arg.SourceExpressionContext)) : std::string{};
        const auto argRoots = !argName.empty()
            ? CollectSourcePointerRootCandidates(this, arg, argName)
            : std::optional<std::vector<const llvm::Value*>>{};
        for (const auto& fieldPath : fieldPaths)
        {
            const bool candidateBorrowed = !argName.empty() && (argRoots.has_value()
                ? NonLocalViewFieldCandidatesMayBeBorrowed(*argRoots, fieldPath)
                : NonLocalViewFieldAnyRootMayBeBorrowed(fieldPath)).value_or(false);
            // Decided candidates are exact; the every-stored-value fallback is for undecided ones.
            const bool storageBorrowed = !argRoots.has_value()
                && NonLocalViewFieldMayBeBorrowed(rootStorage, fieldPath).value_or(false);
            if (!candidateBorrowed && !storageBorrowed) continue;
            const std::string sourceName = !arg.CallerName.empty()
                ? arg.CallerName
                : (!arg.FieldPathText.empty() ? arg.FieldPathText
                    : FindVariableNameByStorage(arg.Storage));
            LogError(std::format(
                "cannot call '{}' through '{}' for parameter '{}' - array view field '{}': the view "
                "may be borrowed on a runtime path, so the callee's move sink could free storage the "
                "real owner still holds. Reassign the field from an owned source before calling.",
                SpellFunctionSymbol(*this, functionName), sourceName.empty() ? "<pointer>" : sourceName,
                paramName, fieldPath));
            return;
        }
}

std::string LLVMBackend::ImplicitOutlivingOwnerConsumeError(const NamedVariable& srcNV,
        bool srcIsMove) const
{
        if (srcIsMove || srcNV.TypeAndValue.IsMove || srcNV.IsExplicitMove
            || (srcNV.Storage == nullptr && !srcNV.IsStaticLocal
                && !srcNV.FieldPathHasPointerDereference))
            return {};
        // Field and element Storage is a GEP; ask about its root allocation.
        const llvm::Value* obj = srcNV.Storage;
        for (int hop = 0; obj != nullptr && hop < 8; hop++)
        {
            if (auto* gep = llvm::dyn_cast<llvm::GEPOperator>(obj))
            { obj = gep->getPointerOperand(); continue; }
            if (auto* bc = llvm::dyn_cast<llvm::BitCastOperator>(obj))
            { obj = bc->getOperand(0); continue; }
            break;
        }
        std::string globalName;
        if (auto* global = llvm::dyn_cast_or_null<llvm::GlobalVariable>(obj))
        {
            globalName = global->getName().str();
            // User globals are recorded here; compiler globals (literal pools, vtables) are not.
            if (globalVariableTypes.find(globalName) == globalVariableTypes.end())
                globalName.clear();
        }
        const bool outlivingGlobal = !globalName.empty();
        const bool alreadyCheckedBorrow = srcNV.RootIsBorrowedByValueParam
            || srcNV.RootIsAliasBorrowLocal || srcNV.PointsToBorrowedByValueParam
            || (srcNV.IsBorrowed && !srcNV.BorrowedOrigin.empty()
                && IsFunctionParameter(srcNV.BorrowedOrigin));
        const bool outlivingPointer = srcNV.FieldPathHasPointerDereference
            && (srcNV.TypeAndValue.IsUnique || srcNV.IsUniqueFieldAlias)
            && !srcNV.FieldPathPointerFrameLocal && !alreadyCheckedBorrow;
        if (!srcNV.IsStaticLocal && !outlivingGlobal && !outlivingPointer) return {};

        std::string spelling = outlivingPointer && !srcNV.FieldPathRoot.empty()
            ? srcNV.FieldPathRoot : srcNV.CallerName;
        if (spelling.empty())
            spelling = !globalName.empty() ? globalName
                : !srcNV.FieldPathRoot.empty() ? srcNV.FieldPathRoot : srcNV.FieldName;
        std::string movePath = FieldMoveKey(srcNV);
        if (movePath.empty() && !srcNV.FieldName.empty()) movePath = srcNV.FieldName;
        if (!movePath.empty())
            spelling += movePath.front() == '[' ? movePath : "." + movePath;
        if (outlivingPointer && !srcNV.IsStaticLocal && !outlivingGlobal)
            return std::format(
                "cannot consume owning value '{}' out of storage that outlives this function - "
                "the pointer target is not proven local to this frame, so the owning field may "
                "still be responsible for cleanup when this function returns. Write 'move {}' to "
                "take the value and re-initialize the storage, or give '{}' a 'copy()' method "
                "and copy it.",
                spelling, spelling, SpellType(*this, srcNV.TypeAndValue));
        return std::format(
            "cannot consume owning value '{}' out of storage that outlives this function - a global "
            "or 'static' local is never re-initialized between entries, so this store empties it and "
            "the next execution reads an empty value. Write 'move {}' to take the value and "
            "re-initialize the storage, or give '{}' a 'copy()' method and copy it.",
            spelling, spelling, SpellType(*this, srcNV.TypeAndValue));
}

std::string LLVMBackend::ImplicitThisFieldConsumeError(const NamedVariable& srcNV,
        bool srcIsMove) const
{
        if (srcIsMove || srcNV.IsExplicitMove) return {};
        std::string fieldPath;
        if (srcNV.RootIsBorrowedThisField)
            fieldPath = srcNV.ThisFieldPath;
        if (fieldPath.empty())
            for (const auto& source : srcNV.ConditionalMoveSources)
                if (source.RootIsBorrowedThisField && !source.ThisFieldPath.empty())
                {
                    fieldPath = source.ThisFieldPath;
                    break;
                }
        if (fieldPath.empty()) return {};
        return std::format(
            "cannot consume owning field '{}' out of the borrowed receiver 'this' - write "
            "'move {}' to take the value and leave the field empty",
            fieldPath, fieldPath);
}

bool LLVMBackend::IsFunctionValueType(const TypeAndValue& type) const
{
        return type.IsFunctionPointer || type.TypeName == "__c_fn_ptr"
            || type.TypeName == "__closure_fat_ptr" || IsEncodedClosureType(type.TypeName);
}

/*
 * Explicit `move f` of a function value (thin or fat) into a BY-VALUE parameter: store null into
 * the source and mark it readable-as-null. A reference sink (C++ T&& / T&, or a slot the callee
 * binds, IsCxxReferenceSink) keeps the source. Returns true when the source was nulled.
 */
bool LLVMBackend::NullExplicitFunctionMoveToValue(const TypeAndValue& param, const NamedVariable& arg)
{
        const TypeAndValue& movedType = arg.HasOriginalArgumentType
            ? arg.OriginalArgumentType : arg.TypeAndValue;
        if (!arg.IsExplicitMove || !IsFunctionValueType(movedType)) return false;
        if (param.Pointer || param.IsAlias || param.IsRvalueRef || param.CxxRefLevel >= 0
            || arg.IsCxxReferenceSink)
            return false;
        const std::string name = arg.CallerName.empty() ? arg.TypeAndValue.VariableName : arg.CallerName;
        const std::string path = FieldMoveKey(arg);
        llvm::Value* storage = arg.Storage;
        if (storage == nullptr && path.empty() && !arg.IsElementAccess && !name.empty())
            storage = FindVariableStorage(name).Storage;
        llvm::Type* valueType = arg.Primary != nullptr ? arg.Primary->getType() : GetType(movedType);
        if (storage != nullptr && valueType != nullptr)
            builder->CreateStore(llvm::Constant::getNullValue(valueType), storage);
        if (path.empty() && !arg.IsElementAccess && arg.FieldName.empty()
            && arg.OwningStructName.empty() && !name.empty())
            MarkVariableExplicitlyMovedNull(name);
        return true;
}

void LLVMBackend::ApplyMoveParamTransfer(const std::string& functionName,
        const std::vector<TypeAndValue>& params, const std::vector<NamedVariable>& args,
        bool paramsCarryAllocAlign, bool calleeIsMethod, bool beforeCall, bool calleeIsCxx)
{
        if (beforeCall) RegisterOwnedStringRvalueArguments(args);
        auto argumentType = [&](const NamedVariable& arg) -> const TypeAndValue& {
            return arg.HasOriginalArgumentType ? arg.OriginalArgumentType : arg.TypeAndValue;
        };
        auto isFunctionValue = [&](const TypeAndValue& type) { return IsFunctionValueType(type); };
        auto isThinFunctionValue = [&](const TypeAndValue& type) {
            if (type.IsFunctionPointer || type.TypeName == "__c_fn_ptr") return type.IsThinFnPtr();
            const auto* encoded = GetEncodedClosureType(type.TypeName);
            return encoded != nullptr && encoded->IsThinFnPtr();
        };
        auto isPointeeMove = [](const NamedVariable& arg) {
            return arg.PointeeMove;
        };
        // An implicit consume of a field of the borrowed receiver `this` is refused at the point
        // the source slot would be cleared (T447). True when reported.
        auto reportThisFieldConsume = [&](const NamedVariable& arg) {
            const std::string error = ImplicitThisFieldConsumeError(arg, false);
            if (error.empty()) return false;
            if (arg.SourceExpressionContext != nullptr && contextualErrorReporter)
                contextualErrorReporter(arg.SourceExpressionContext, error);
            else
                LogError(error);
            return true;
        };
        for (size_t i = 0; i < params.size() && i < args.size(); i++)
        {
            const auto& movedArgType = argumentType(args[i]);
            const bool explicitFunctionMove = args[i].IsExplicitMove
                && isFunctionValue(movedArgType);
            // Only a by-value sink takes the value. A reference sink (C++ T&&/T&, or a forwarded
            // function-pointer slot) keeps the source: the callee binds the slot, and may consume it.
            if (explicitFunctionMove)
            {
                // Null once, before the call: a store after it would overwrite a callee's reseat.
                if (beforeCall) NullExplicitFunctionMoveToValue(params[i], args[i]);
                if (isThinFunctionValue(movedArgType)) continue;
            }
            // C++ class by-value transfer constructs a separate parameter object. The source
            // remains a live moved-from object, so never zero its storage like a CFlat sink.
            if (!params[i].Pointer && !params[i].IsAlias
                && (IsForeignNontrivialCxxClass(params[i].TypeName)
                    || HasForeignNontrivialCxxField(params[i].TypeName)))
            {
                if (!beforeCall && (params[i].IsMove || args[i].IsExplicitMove))
                {
                    std::string movedName = args[i].CallerName.empty()
                        ? args[i].TypeAndValue.VariableName : args[i].CallerName;
                    const std::string movePath = FieldMoveKey(args[i]);
                    // A lowered CFlat struct is marked moved only by a `move` parameter, like the plain
                    // struct sink (diagnostic only: storage not zeroed); a copy param keeps it readable.
                    const bool loweredStructMoveSink = !IsForeignNontrivialCxxClass(params[i].TypeName)
                        && params[i].IsMove && !args[i].DeferMovedMark;
                    if ((IsForeignNontrivialCxxClass(params[i].TypeName) || loweredStructMoveSink)
                        && !movedName.empty())
                    {
                        if (!movePath.empty()) MarkVariableFieldMoved(movedName, movePath);
                        else if (!args[i].IsElementAccess)
                            MarkVariableMoved(movedName, isPointeeMove(args[i]));
                    }
                }
                continue;
            }
            std::string sourceName = args[i].CallerName;
            const std::string movePath = FieldMoveKey(args[i]);
            const std::string fieldRootName = sourceName.empty()
                ? !args[i].FieldPathRoot.empty() ? args[i].FieldPathRoot
                    : args[i].TypeAndValue.ParentVariableName
                : sourceName;
            const bool wholeBindingSource = !args[i].IsElementAccess
                && args[i].FieldName.empty() && args[i].OwningStructName.empty();
            if (sourceName.empty() && movePath.empty())
                sourceName = args[i].TypeAndValue.VariableName;
            const bool paramIsMoveSink = params[i].IsMove
                || (!params[i].Pointer && !params[i].IsAlias
                    && IsCoreUniqueType(params[i].TypeName));
            if (beforeCall)
            {
                const auto borrowedArm = std::find_if(args[i].ConditionalMoveSources.begin(),
                    args[i].ConditionalMoveSources.end(), [](const auto& source) {
                        return source.MoveParamSinkArgument && source.BorrowedPointer;
                    });
                if (paramIsMoveSink && borrowedArm != args[i].ConditionalMoveSources.end())
                {
                    const bool directBorrowParam = IsFunctionParameter(borrowedArm->CallerName);
                    const std::string borrowOrigin = borrowedArm->BorrowedOrigin;
                    const bool copiedBorrow = !directBorrowParam && !borrowOrigin.empty()
                        && IsFunctionParameter(borrowOrigin);
                    if (borrowedArm->BorrowedFromOwningLocal)
                        LogOwningLocalCopyMoveSinkError(borrowedArm->CallerName, borrowOrigin,
                            params[i].VariableName, SpellFunctionSymbol(*this, functionName));
                    else if (directBorrowParam || copiedBorrow)
                    {
                        const std::string sourceDescription = directBorrowParam
                            ? std::format("borrowed parameter '{}'", borrowedArm->CallerName)
                            : std::format("borrowed pointer '{}' (copied from parameter '{}')",
                                borrowedArm->CallerName.empty() ? borrowOrigin : borrowedArm->CallerName,
                                borrowOrigin);
                        const std::string declaredName = directBorrowParam
                            ? borrowedArm->CallerName : borrowOrigin;
                        LogBorrowedPointerMoveSinkError(sourceDescription,
                            params[i].VariableName, SpellFunctionSymbol(*this, functionName),
                            declaredName);
                    }
                }
                const auto deferred = std::find_if(args[i].ConditionalMoveSources.begin(),
                    args[i].ConditionalMoveSources.end(), [](const auto& source) {
                        return !source.DeferredDiagnostic.empty();
                    });
                if (deferred != args[i].ConditionalMoveSources.end())
                    LogErrorMessage("{}", { deferred->DeferredDiagnostic });
            }
            if (params[i].IsRvalueRef && args[i].IsExplicitMove)
            {
                // C++ move construction consumes the source only at compile time. The moved-from
                // object remains alive, so do not zero its storage or retire its destructor.
                if (!beforeCall)
                {
                    if (!sourceName.empty() && !movePath.empty())
                    {
                        if (!explicitFunctionMove)
                            MarkVariableFieldMoved(sourceName, movePath);
                    }
                    else if (!sourceName.empty() && !args[i].IsElementAccess
                        && !explicitFunctionMove)
                        MarkVariableMoved(sourceName, isPointeeMove(args[i]));
                }
                continue;
            }
            if (!beforeCall)
                RejectOwningTempUniqueFieldIntoSinkParam(functionName, params[i], args[i]);
            // A plain by-value parameter the callee body unconditionally moves is a synthesized
            // move sink too - but only when the concrete type owns a resource AND the matched arg
            // is itself an OWNER (not a borrow/alias). A borrow arg has nothing to transfer, so
            // nulling it would orphan a value the caller still needs.
            // A fat closure (Lambda<...>) is an OWNING VALUE like string: it owns its captured env.
            // Admit __closure_fat_ptr and any encoded closure element type; a thin C fn ptr owns
            // nothing and never reaches here as a sink (ParamIsOwningSinkEligible rejects it).
            bool paramOwnsResource = IsOwningValueOrClosureType(params[i].TypeName)
                || IsCoreUniqueType(params[i].TypeName);
            bool foreignCxxPointerSink = calleeIsCxx && args[i].IsExplicitMove
                && params[i].Pointer && !params[i].IsAlias
                && !params[i].IsRvalueRef && !params[i].IsCxxRefToPointer
                && !params[i].CxxConstRef();
            bool inferredSinkConsumes = params[i].IsConsumeInferredSink
                && OwningSinkConsumesConcrete(params[i]);
            // A borrow/alias arg has no ownership to transfer - nulling it would orphan a value
            // the caller still relies on.
            bool argIsBorrow = args[i].IsBorrowed || args[i].IsAliasBorrow
                || args[i].BorrowsOwnedString || args[i].BorrowsOwnedElement
                || args[i].TypeAndValue.IsAlias;
            // A named local of an owning VALUE type is itself an owner (a by-value borrow is
            // excluded above, and a moved-from read is caught by the use-after-move machinery).
            // A closure arg is an owner either as a named closure local (CallerName + owning param)
            // or as a freshly-lowered lambda RVALUE TEMP (no CallerName, tracked in the pending
            // closure-temp set); consuming the temp transfers its env into the slot.
            bool argIsOwner = !argIsBorrow
                && (args[i].IsOwning || args[i].IsOwningString || args[i].IsOwningStruct
                    || IsVariableOwning(sourceName) || IsVariableOwningString(sourceName)
                    || IsOwnedClosureTemp(args[i].Primary)
                    || IsPendingOwnedStringTemp(args[i].Primary)
                    || std::any_of(args[i].ConditionalMoveSources.begin(),
                        args[i].ConditionalMoveSources.end(), [](const auto& source) {
                            return !source.BorrowedPointer;
                        })
                    || IsCoreUniqueType(args[i].TypeAndValue.TypeName)
                    || (!sourceName.empty() && paramOwnsResource));
            bool forcedOperatorConsume = args[i].ForceOperatorConsume && argIsOwner && paramOwnsResource;
            bool sinkConsumes = OwningSinkConsumesConcrete(params[i]) || inferredSinkConsumes;
            bool isOwningSink = (sinkConsumes || foreignCxxPointerSink || forcedOperatorConsume
                    || (!params[i].Pointer && !params[i].IsAlias
                        && IsCoreUniqueType(params[i].TypeName)))
                && argIsOwner && (paramOwnsResource || foreignCxxPointerSink);
            const bool sourceIsOwning = args[i].TypeAndValue.IsUnique
                || IsCoreUniqueType(args[i].TypeAndValue.TypeName)
                || (!args[i].TypeAndValue.Pointer && IsOwningValueOrClosureType(args[i].TypeAndValue.TypeName))
                || args[i].IsOwning || args[i].IsOwningString || args[i].IsOwningStruct;
            const bool sourceBindingOwns = sourceIsOwning
                || (!sourceName.empty() && IsVariableOwning(sourceName));
            const bool paramConsumesSource = params[i].IsMove
                || (!params[i].Pointer && !params[i].IsAlias
                    && IsCoreUniqueType(params[i].TypeName))
                || params[i].IsOwningSink || sinkConsumes || forcedOperatorConsume;
            // Dynamic-index moves stay untracked and keep their pre-T367 call behavior.
            if (beforeCall && paramConsumesSource && sourceIsOwning
                && !args[i].MovePathHasDynamicIndex)
                if (std::string error = ImplicitOutlivingOwnerConsumeError(args[i], false);
                    !error.empty())
                    LogError(error);
            // Ownership-laundering guard: the arg is a value this function only BORROWS (a plain
            // by-value owning-value param of the current function) but the callee's param CONSUMES
            // it (a sink / `move` / unique). Transferring would null this function's alias while the
            // TRUE owner (a caller further up) still frees the resource - a silent double-free.
            bool paramConsumesOwningValue = paramOwnsResource
                && (params[i].IsMove
                || (!params[i].Pointer && !params[i].IsAlias
                        && IsCoreUniqueType(params[i].TypeName))
                || sinkConsumes || forcedOperatorConsume);
            if (paramConsumesOwningValue
                && IsVariableBorrowedOwningValue(sourceName))
            {
                LogErrorMessage(
                    "call to '{}': parameter '{}' takes ownership of a value this function only "
                    "borrows ('{}' is a by-value parameter, so the caller keeps ownership). Accept "
                    "the parameter as a sink ({} it in the body), or pass a copy with '{}'.",
                    { SpellFunctionSymbol(*this, functionName), params[i].VariableName, sourceName,
                      "move", "copy()" });
                continue;
            }
            // A `unique`-typed parameter is a synthesized move sink: the callee owns and frees it,
            // so the caller's source must be nulled exactly as for an explicit `move` param.
            // Without this, `list<unique T*>::add(T value)` leaves the caller owning too.
            // An explicit `move` into the element slot of a BORROWING container (bare
            // `list<T*>` and friends) is a real transfer: the container becomes the only handle,
            // so the caller's local must be nulled exactly as a declared `move` param nulls it.
            // Without this the source would still free at scope exit and the element dangle -
            // which is the very thing the `move` was written to prevent.
            bool movesIntoBorrowingElement = args[i].IsExplicitMove && !argIsBorrow
                && args[i].TypeAndValue.Pointer
                && IsBorrowingContainerElementSink(functionName, params, i, calleeIsMethod)
                && (args[i].IsOwning || IsVariableOwning(sourceName));
            if (params[i].IsMove
                || (!params[i].Pointer && !params[i].IsAlias
                    && IsCoreUniqueType(params[i].TypeName))
                || isOwningSink
                || inferredSinkConsumes
                || movesIntoBorrowingElement)
            {
                // A `move` param takes ownership and frees the block itself. The allocation alignment
                // of an over-aligned `new T[n]` is not in the element type, so the callee can recover
                // it only when the PARAMETER declares the same `alignas(_, N)` clause. A matching clause
                // is allowed; a missing/mismatched one would free the block wrong and corrupt the heap.
                // Alignment carried by the TYPE is not tagged and passes through freely.
                // A `unique<T, N>` parameter carries its allocation alignment in the TYPE, so
                // type identity already enforces agreement and the wrapper VALUE has no tracked
                // block alignment to compare against.
                const bool coreUniqueWrapperParam = !params[i].Pointer
                    && IsCoreUniqueType(params[i].TypeName);
                const bool argIsNull = args[i].Primary != nullptr
                    && llvm::isa<llvm::ConstantPointerNull>(args[i].Primary);
                uint64_t argTypeAlignment = 0;
                if (args[i].TypeAndValue.Pointer && !args[i].TypeAndValue.ElemPointer
                    && !args[i].TypeAndValue.TypeName.empty()
                    && !args[i].TypeAndValue.IsFunctionPointer
                    && args[i].TypeAndValue.TypeName != "__c_fn_ptr"
                    && args[i].TypeAndValue.TypeName != "__closure_fat_ptr")
                {
                    TypeAndValue argType{ .TypeName = args[i].TypeAndValue.TypeName };
                    llvm::Type* type = GetType(argType);
                    if (type != nullptr && type->isSized())
                        argTypeAlignment = GetEffectiveAlignmentForType(argType.TypeName, type);
                }
                const uint64_t sourceAllocAlignment = std::max({ args[i].AllocAlignment,
                    args[i].TypeAndValue.AllocAlignValue,
                    args[i].TypeAndValue.NewAllocAlignment });
                const uint64_t effectiveArgAlignment = std::max(argTypeAlignment,
                    sourceAllocAlignment);
                const bool hasTypeAlignment = argTypeAlignment > kDefaultNewAlign;
                const bool alignmentMismatch = params[i].AllocAlignValue > kDefaultNewAlign
                    ? effectiveArgAlignment < params[i].AllocAlignValue
                    : sourceAllocAlignment > kDefaultNewAlign && !hasTypeAlignment
                        && sourceAllocAlignment != params[i].AllocAlignValue;
                if (!beforeCall && paramsCarryAllocAlign
                    && !coreUniqueWrapperParam
                    && !argIsNull
                    && alignmentMismatch)
                {
                    if (params[i].AllocAlignValue == 0)
                        LogErrorMessage(
                            "cannot move the over-aligned buffer '{}' ('{}') into the '{}' "
                            "parameter of '{}': that alignment is a property of the allocation, not of the type, so "
                            "the callee cannot recover it. Declare the parameter '{}' so the block "
                            "alignment is recorded, or over-align the element TYPE instead.",
                            { sourceName.empty() ? SpellType(*this, args[i].TypeAndValue) : sourceName,
                              std::format("new T[n] alignas(0, {})", sourceAllocAlignment), "move",
                              SpellFunctionSymbol(*this, functionName),
                              std::format("alignas(0, {})", sourceAllocAlignment) });
                    else
                    {
                        // A core `unique<T, N>` receiver reaches this check through its own
                        // constructor, so naming a 'move' parameter the user never wrote is wrong.
                        std::string calleeSpelling = SpellFunctionSymbol(*this, functionName);
                        if (IsCoreUniqueType(functionName))
                        {
                            const std::string allocationDescription =
                                argTypeAlignment > kDefaultNewAlign
                                    && effectiveArgAlignment == argTypeAlignment
                                    ? std::format("{}, the alignment of its type '{}'",
                                    argTypeAlignment,
                                    args[i].TypeAndValue.TypeName)
                                : effectiveArgAlignment == 0
                                    ? std::string("ordinary alignment")
                                    : std::format("'alignas(0, {})'", effectiveArgAlignment);
                            LogErrorMessage(
                                "alignment mismatch initializing '{}': the declared type allocates at {}, but the "
                                "value was allocated {}. "
                                "Allocate the value with the matching 'alignas(0, {})' "
                                "clause, or declare the receiver with the alignment the value already has.",
                                { calleeSpelling, std::to_string(params[i].AllocAlignValue),
                                  allocationDescription,
                                  std::to_string(params[i].AllocAlignValue) });
                        }
                        else
                            LogErrorMessage(
                                "alignment mismatch moving into the 'move' parameter of '{}': the parameter is declared "
                                "'alignas(0, {})' but the argument was allocated 'alignas(0, {})'. The two must agree "
                                "so the callee frees with the correct alignment.",
                                { calleeSpelling, std::to_string(params[i].AllocAlignValue),
                                  std::to_string(args[i].AllocAlignment) });
                    }
                }

                // The pass after the call drops these from the temp lists; an unwind out of
                // the call itself must already treat them as the callee's.
                if (beforeCall && args[i].Primary != nullptr)
                    moveTransferConsumedTemps_.push_back(args[i].Primary);
                if (!beforeCall)
                {
                    // A `move string` argument transfers ownership to the callee, which frees
                    // it on return. If the argument is an unnamed owned-string temporary (e.g.
                    // a chained-concat result passed directly), drop it from the end-of-
                    // expression cleanup list so it is not also freed by the caller.
                    if (params[i].TypeName == "string")
                        UnregisterOwnedStringTemp(args[i].Primary);
                    // A `move` closure argument (fat Lambda<T> or an encoded closure element type, gap a)
                    // transfers env ownership to the callee. Drop the unnamed lambda temp from the end-
                    // of-expression closure cleanup so it is not also freed by the caller (double-free).
                    else if (params[i].TypeName == "__closure_fat_ptr"
                             || IsEncodedClosureType(params[i].TypeName))
                        UnregisterOwnedClosureTemp(args[i].Primary);

                    // Invariant guard: a sink parameter owns its argument, so no caller-side
                    // end-of-expression free may remain registered for it.
                    UnregisterOwnedPtrTemp(args[i].Primary, /*throughPhis*/ true);
                }

                // An interface fat-ptr param built from a caller STRUCT VALUE points AT the caller's
                // alloca, so zeroing that alloca would corrupt the data the callee sees - keep it a
                // borrow. A POINTER argument is different: the fat ptr carries the pointer VALUE, so
                // boxing an owning pointer into a `move` interface param transfers ownership (like
                // `IFace x = ptr`). Its source must be nulled, or the local is freed at scope exit
                // while the callee (e.g. list<IFace>) still holds it - a double free.
                bool isInterfaceBorrow = params[i].IsFatInterfaceValue()
                    && !args[i].TypeAndValue.Pointer;
                // When the arg expression went through a cast (or similar), args[i].Storage
                // may be cleared even though CallerName still names the original owning variable.
                // Look up the source by name so we still null its alloca and prevent a double-free.
                llvm::Value* srcStorage = args[i].Storage;
                llvm::Type*  srcBaseTy  = args[i].BaseType;
                bool isFieldAccess = !movePath.empty();
                // The CallerName fallback resolves the BASE variable's storage; do NOT use it
                // for a field access or it would null the base pointer instead of the field.
                // A field access already carries its own (GEP) Storage.
                if (srcStorage == nullptr && !isFieldAccess && wholeBindingSource
                    && !sourceName.empty())
                {
                    auto ref = FindVariableStorage(sourceName);
                    srcStorage = ref.Storage;
                    if (srcBaseTy == nullptr) srcBaseTy = ref.BaseType;
                }

                // Only sources collected from a call argument feed the declared move-param path.
                // Assignment joins keep their existing source-ledger behavior.
                bool anyMoveParamCallArm = false;
                for (const auto& source : args[i].ConditionalMoveSources)
                    anyMoveParamCallArm = anyMoveParamCallArm
                        || (source.MoveParamSinkArgument && !source.BorrowedPointer);
                if (!args[i].ConditionalMoveSources.empty()
                    && (isOwningSink || (params[i].IsMove && anyMoveParamCallArm)))
                {
                    const bool nullsThisFieldArm = std::any_of(
                        args[i].ConditionalMoveSources.begin(), args[i].ConditionalMoveSources.end(),
                        [&](const auto& source) {
                            return source.RootIsBorrowedThisField
                                && (isOwningSink
                                    || (source.MoveParamSinkArgument && !source.BorrowedPointer));
                        });
                    if (beforeCall && nullsThisFieldArm && reportThisFieldConsume(args[i]))
                        continue;
                    for (const auto& source : args[i].ConditionalMoveSources)
                    {
                        if (!isOwningSink
                            && (!source.MoveParamSinkArgument || source.BorrowedPointer))
                            continue;
                        if (beforeCall)
                        {
                            if (source.Storage == nullptr || source.ValueType == nullptr
                                || source.ArmBlock == nullptr
                                || cflat_llvm::GetTerminatorOrNull(source.ArmBlock) == nullptr)
                                continue;
                            auto savedIP = builder->saveIP();
                            builder->SetInsertPoint(source.ArmBlock->getTerminator());
                            builder->CreateStore(llvm::Constant::getNullValue(source.ValueType),
                                                 source.Storage);
                            if (!source.NullKey.empty()) RecordNullSet(source.NullKey);
                            if (source.PointeeMove)
                                RecordNullSet(source.CallerName, true);
                            builder->restoreIP(savedIP);
                        }
                        else if (!source.CallerName.empty())
                        {
                            if (explicitFunctionMove)
                                continue;
                            if (!source.MovePathHasDynamicIndex && !source.MovePathKey.empty())
                                MarkVariableFieldMoved(source.CallerName, source.MovePathKey);
                            else if (!source.FieldName.empty())
                                MarkVariableFieldMoved(source.CallerName, source.FieldName);
                            else if (!source.IsElementAccess)
                                MarkVariableMoved(source.CallerName, source.PointeeMove);
                        }
                    }
                    continue;
                }

                if (beforeCall)
                {
                    // A unique<T> wrapper releases its pointee while lowering the raw-pointer
                    // argument. Do not clear the wrapper before that release call runs.
                    if (foreignCxxPointerSink && !args[i].TypeAndValue.Pointer)
                        continue;
                    // The source slot is cleared below: that is the consume a this-field refuses.
                    const bool nullsFatData = params[i].IsFatInterfaceValue()
                        && args[i].TypeAndValue.IsFatInterfaceValue()
                        && (args[i].IsOwning || IsVariableOwning(sourceName))
                        && srcStorage != nullptr;
                    const bool nullsSlot = srcStorage != nullptr && !isInterfaceBorrow
                        && (llvm::isa_and_nonnull<llvm::PointerType>(srcBaseTy)
                            || (params[i].TypeName == "string" && args[i].IsOwningString)
                            || llvm::isa_and_nonnull<llvm::StructType>(srcBaseTy));
                    if ((nullsFatData || nullsSlot) && sourceIsOwning
                        && reportThisFieldConsume(args[i]))
                        continue;
                    // Clear the source before the callee can observe or reseat an aliased slot.
                    if (params[i].IsFatInterfaceValue()
                        && args[i].TypeAndValue.IsFatInterfaceValue()
                        && (args[i].IsOwning || IsVariableOwning(sourceName))
                        && srcStorage != nullptr)
                    {
                        auto* dataField = builder->CreateStructGEP(GetFatPtrType(), srcStorage, 1);
                        builder->CreateStore(
                            llvm::ConstantPointerNull::get(cflat_llvm::PointerTo(builder->getInt8Ty())), dataField);
                    }
                    if (srcStorage != nullptr && !isInterfaceBorrow)
                    {
                        if (auto* ptrTy = llvm::dyn_cast_or_null<llvm::PointerType>(srcBaseTy))
                        {
                            if (params[i].IsMove && sourceBindingOwns)
                                RecordMoveOwnerLoopSink(args[i], functionName,
                                    params[i].VariableName);
                            builder->CreateStore(llvm::ConstantPointerNull::get(ptrTy), srcStorage);
                            if (sourceBindingOwns && wholeBindingSource
                                && !sourceName.empty())
                                ClearOwnFlag(sourceName);
                            if (auto* sourceLoad = llvm::dyn_cast_or_null<llvm::LoadInst>(
                                    args[i].Primary);
                                sourceLoad != nullptr && sourceLoad->getPointerOperand() == srcStorage)
                                NoteOwnSlotLeavingLoad(sourceLoad);
                            // A field or element slot holds one scalar; only a whole binding owns a count.
                            if (args[i].FieldName.empty() && args[i].OwningStructName.empty()
                                && !args[i].IsElementAccess)
                                StoreRawArrayLength(args[i], nullptr);
                            if (!isFieldAccess && wholeBindingSource)
                                SetOwnMoveOrigin(srcStorage, currentLine, currentColumn);
                        }
                        else if (params[i].TypeName == "string" && args[i].IsOwningString)
                        {
                            auto* strTy = llvm::StructType::getTypeByName(*context, "string");
                            if (strTy)
                            {
                                if (llvm::isa<llvm::GlobalVariable>(srcStorage))
                                    builder->CreateStore(llvm::ConstantAggregateZero::get(strTy), srcStorage);
                                else
                                {
                                    auto* ptrField = builder->CreateStructGEP(strTy, srcStorage, 0);
                                    auto* i8ptrTy = cflat_llvm::PointerTo(builder->getInt8Ty());
                                    builder->CreateStore(llvm::ConstantPointerNull::get(i8ptrTy), ptrField);
                                }
                            }
                        }
                        else if (auto* stTy = llvm::dyn_cast_or_null<llvm::StructType>(srcBaseTy))
                        {
                            builder->CreateStore(llvm::ConstantAggregateZero::get(stTy), srcStorage);
                        }
                        else if (srcBaseTy == nullptr && params[i].TypeName != "string")
                        {
                            LogErrorMessage(
                                "call to '{}': 'move' argument {} has no resolved type, so its source "
                                "storage cannot be cleared",
                                { SpellFunctionSymbol(*this, functionName), std::to_string(i) });
                        }
                    }
                    continue;
                }

                // An OWNING interface VALUE arg (an owning `unique IShape` local) moved into a
                // move interface param transfers ownership: null the source fat-ptr's data field so
                // its scope-exit teardown sees null and no-ops, else callee and source double-free.
                // Raw struct-value boxes are rejected at the call site, so only a real interface
                // value reaches here; the owning check keeps a borrow source untouched.
                if (params[i].IsFatInterfaceValue()
                    && args[i].TypeAndValue.IsFatInterfaceValue()
                    && (args[i].IsOwning || IsVariableOwning(sourceName))
                    && srcStorage != nullptr)
                {
                    if (!args[i].DeferMovedMark && !sourceName.empty() && !isFieldAccess
                        && wholeBindingSource)
                    {
                        if (!explicitFunctionMove)
                            MarkVariableMoved(sourceName, isPointeeMove(args[i]));
                    }
                }
                // Compile-time: mark the caller's storage as moved so subsequent reads are rejected.
                // Covers pointer, owning-string, and struct move params - all cases where caller storage was zeroed.
                // Moving a FIELD (`node->left`) marks only that field, not the whole base variable.
                if (!args[i].DeferMovedMark && !fieldRootName.empty() && srcStorage != nullptr &&
                    !isInterfaceBorrow && srcBaseTy != nullptr)
                {
                    bool isPtr = llvm::isa<llvm::PointerType>(srcBaseTy);
                    bool isOwningStr = params[i].TypeName == "string" && args[i].IsOwningString;
                    bool isStruct = llvm::isa<llvm::StructType>(srcBaseTy);
                    if (isPtr || isOwningStr || isStruct)
                    {
                        if (explicitFunctionMove)
                            continue;
                        if (isFieldAccess)
                        {
                            if (!sourceName.empty())
                                MarkVariableFieldMoved(fieldRootName, movePath);
                        }
                        else if (wholeBindingSource && !sourceName.empty())
                            MarkVariableMoved(sourceName, isPointeeMove(args[i]));
                    }
                }
            }

            // The unique<T> raw-pointer adapter already released and nulled the holder before
            // this call. Retire its source here so later reads use the normal moved-value error.
            if (!beforeCall && foreignCxxPointerSink && !args[i].TypeAndValue.Pointer
                && IsCoreUniqueType(args[i].TypeAndValue.TypeName)
                && args[i].IsExplicitMove && !sourceName.empty())
            {
                if (!movePath.empty()) MarkVariableFieldMoved(sourceName, movePath);
                else if (wholeBindingSource)
                    MarkVariableMoved(sourceName, isPointeeMove(args[i]));
            }
        }
    }

bool LLVMBackend::ArgumentIsProvablyDataPointer(llvm::Value* value, const NamedVariable& arg) const
{
        if (llvm::isa<llvm::Function>(value)) return false;               // a named function
        if (llvm::isa<llvm::ConstantPointerNull>(value)) return false;    // parity with the direct path
        // A C NULL macro reads as a load of its backing global; the ilz macro flags make it the
        // null pointer constant nullptr is here.
        if (arg.IsCxxNullIntegerMacro && arg.IsCxxMacroGlobal)
            if (auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(value))
                if (auto* global = llvm::dyn_cast_or_null<llvm::GlobalVariable>(load->getPointerOperand());
                    global != nullptr && cxxZeroIntegerMacroNames_.count(global->getName().str()) != 0)
                    return false;
        if (arg.TypeAndValue.IsFunctionPointer) return false;             // a closure value
        if (auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(value);
            constant != nullptr && IsStringLiteralConstant(constant)) return true;
        // The only declared evidence available here: the type is a pointer. A call result with no
        // recorded shape, or a bare identifier, leaves this false.
        if (arg.TypeAndValue.Pointer) return true;
        // An explicit cast to a code type is the user's own assertion - honour it here, scoped to
        // THIS argument's own occurrence (see codeValueDataCasts_'s comment on the collision).
        if (IsDataValueCodeCast(value, arg.CastOccurrenceId)) return false;
        // A JOIN carries no declared facts at all - resolve it through the per-value DATA ledger,
        // which answers yes when an arm is proven data and none is unproven (JoinDeliversDataValue).
        return JoinDeliversDataValue(value, arg.CastOccurrenceId);
    }

std::string LLVMBackend::DescribeNonFunctionArgument(const NamedVariable& arg) const
{
        const auto& tn = arg.TypeAndValue.TypeName;
        if (tn.empty())
            return "a non-function pointer value";
        return std::format("a '{}{}' value", tn, arg.TypeAndValue.Pointer ? "*" : "");
    }

llvm::Value* LLVMBackend::WidenToClosureFatChecked(llvm::Value* val, const NamedVariable& arg,
        const std::string& paramName, const std::string& fieldDesc)
{
        if (val && val->getType()->isPointerTy() && ArgumentIsProvablyDataPointer(val, arg))
        {
            // A field STORE reaches the same gate as an argument pass; name the real
            // destination so the diagnostic does not call a struct field a parameter.
            std::string action = fieldDesc.empty()
                ? std::format("pass {} to closure parameter '{}'",
                    DescribeNonFunctionArgument(arg), paramName)
                : std::format("store {} into closure field {}",
                    DescribeNonFunctionArgument(arg), fieldDesc);
            LogErrorMessage(
                "cannot {}: only a named function, a '{}' value or a lambda converts to a closure - a data pointer "
                "would be called as code. If the value really holds a code address, assert "
                "it with an explicit cast: '{}'.",
                { action, "function<>", "(function<...>)value" });
        }
        return WidenBareOrThinToClosureFat(val, arg.TypeAndValue);
    }

void LLVMBackend::CheckClosureReturnProvenance(llvm::Value* val, const NamedVariable& returnNV,
        bool thin) const
{
        if (val && val->getType()->isPointerTy() && ArgumentIsProvablyDataPointer(val, returnNV))
        {
            if (thin)
                LogErrorMessage(
                    "cannot return {} as a '{}' value: only a named function, a "
                    "'{}' value or a non-capturing lambda converts to a function pointer - "
                    "a data pointer would be called as code. If the value really holds a code "
                    "address, assert it with an explicit cast: '{}'.",
                    { DescribeNonFunctionArgument(returnNV), "function<>", "function<...>",
                      "(function<...>)value" });
            else
                LogErrorMessage(
                    "cannot return {} as a closure: only a named function, a "
                    "'{}' value or a lambda converts to a closure - a data pointer "
                    "would be called as code. If the value really holds a code address, assert "
                    "it with an explicit cast: '{}'.",
                    { DescribeNonFunctionArgument(returnNV), "function<>",
                      "(function<...>)value" });
        }
    }

void LLVMBackend::CheckThinFnPtrArgProvenance(llvm::Value* val, const NamedVariable& arg,
        const std::string& paramName) const
{
        if (val && val->getType()->isPointerTy() && ArgumentIsProvablyDataPointer(val, arg))
        {
            LogErrorMessage(
                "cannot pass {} to '{}' parameter '{}': only a named function, a "
                "'{}' value or a non-capturing lambda converts to a function pointer - "
                "a data pointer would be called as code. If the value really holds a code "
                "address, assert it with an explicit cast: '{}'.",
                { DescribeNonFunctionArgument(arg), "function<>", paramName, "function<>",
                  "(function<...>)value" });
        }
    }

void LLVMBackend::CheckThinFnPtrAssignProvenance(llvm::Value* val, const NamedVariable& arg,
        const std::string& destDesc) const
{
        if (val && val->getType()->isPointerTy() && ArgumentIsProvablyDataPointer(val, arg))
        {
            LogErrorMessage(
                "cannot assign {} to '{}' destination {}: only a named function, a "
                "'{}' value or a non-capturing lambda converts to a function pointer - "
                "a data pointer would be called as code. If the value really holds a code "
                "address, assert it with an explicit cast: '{}'.",
                { DescribeNonFunctionArgument(arg), "function<>", destDesc, "function<>",
                  "(function<...>)value" });
        }
    }

void LLVMBackend::CheckFatClosureAssignProvenance(llvm::Value* val, const NamedVariable& arg,
        const std::string& destDesc) const
{
        if (val && val->getType()->isPointerTy() && ArgumentIsProvablyDataPointer(val, arg))
        {
            LogErrorMessage(
                "cannot assign {} to closure destination {}: only a named function, a "
                "'{}' value or a lambda converts to a closure - a data pointer "
                "would be called as code. If the value really holds a code "
                "address, assert it with an explicit cast: '{}'.",
                { DescribeNonFunctionArgument(arg), destDesc, "function<>",
                  "(function<...>)value" });
        }
    }

llvm::Value* LLVMBackend::WidenBareOrThinToClosureFat(llvm::Value* val, const TypeAndValue& sigTV)
{
        if (auto* fn = llvm::dyn_cast<llvm::Function>(val))
            return WrapBareValueAsFatStruct(fn);
        if (val && !val->getType()->isStructTy() && val->getType()->isPointerTy())
            return WidenThinToFat(val, sigTV);   // thin function<T> -> fat Lambda<T> {code, null}
        return val;
    }

llvm::Value* LLVMBackend::LowerClosureFatToThinFnPtr(llvm::Value* val, llvm::Type* targetTy,
        const std::string& paramName, const std::vector<std::string>& captureNames)
{
        auto* envField = builder->CreateExtractValue(val, {1u}, "closure_env");
        if (llvm::isa<llvm::ConstantPointerNull>(envField))
        {
            auto* codeField = builder->CreateExtractValue(val, {0u}, "closure_code");
            return builder->CreateBitCast(codeField, targetTy, "noncap_lambda_cfn");
        }
        // A lowered-signature forwarder closure (a non-capturing literal or named body): its env
        // is the C-ABI entry itself, so that entry is the C pointer the callee needs.
        if (ClosureForwarderOf(val) != nullptr)
            return builder->CreateBitCast(envField, targetTy, "forwarder_cfn");
        if (!captureNames.empty())
        {
            // A capturing lambda literal - name what it captures so the user can see
            // exactly which variables to drop (or replace with a named function).
            // Names are already de-duplicated; show at most 5, then summarize the rest.
            const size_t count = captureNames.size();
            const size_t shown = count < 5 ? count : 5;
            std::string list;
            for (size_t k = 0; k < shown; k++)
            {
                if (k != 0) list += ", ";
                list += captureNames[k];
            }
            if (count > shown)
                LogErrorMessage(
                    "cannot pass to C function-pointer parameter '{}': this lambda captured {} {} "
                    "[{}, ... (and {} more)]. A C callback is a bare code pointer and cannot carry "
                    "captured state - pass a non-capturing lambda or a named function.",
                    { paramName, std::to_string(count), count == 1 ? "variable" : "variables", list,
                      std::to_string(count - shown) });
            else
                LogErrorMessage(
                    "cannot pass to C function-pointer parameter '{}': this lambda captured {} {} [{}]. "
                    "A C callback is a bare code pointer and cannot carry captured state - "
                    "pass a non-capturing lambda or a named function.",
                    { paramName, std::to_string(count), count == 1 ? "variable" : "variables", list });
            return nullptr;   // unreachable: LogError above does not return
        }
        // A stored function<> value - its captures are not known at the call site.
        LogErrorMessage(
            "cannot pass to C function-pointer parameter '{}': this 'function<>' value may store captured state. "
            "A C callback is a bare code pointer - pass a non-capturing lambda or a named function.",
            { paramName });
        return nullptr;   // unreachable: LogError above does not return
    }

llvm::Value* LLVMBackend::LowerAliasByPointerArg(const NamedVariable& arg,
                                                 const TypeAndValue& param,
                                                 bool strictCxxScalarType)
{
        auto* paramTy = GetType(param);
        // A named nullptr_t lvalue already has the object slot a C++ reference must borrow.
        if (param.IsCxxNullptrT && arg.TypeAndValue.IsCxxNullptrT
            && arg.Storage != nullptr)
            return arg.Storage;
        std::string sourceSpelling;
        std::string paramSpelling;
        auto cxxValueSpelling = [&](std::string typeName, std::string& spelling) {
            auto removePrefix = [&](std::string_view prefix) {
                if (!typeName.starts_with(prefix)) return;
                typeName.erase(0, prefix.size());
                while (!typeName.empty() && typeName.front() == ' ') typeName.erase(0, 1);
            };
            removePrefix("const");
            removePrefix("volatile");
            for (std::string_view suffix : { std::string_view(" const"),
                                             std::string_view(" volatile") })
                if (typeName.ends_with(suffix)) typeName.erase(typeName.size() - suffix.size());
            return CxxSpellingForCflatType(typeName, spelling);
        };
        const bool sameReferentType = !arg.TypeAndValue.Pointer && !param.Pointer
            && (strictCxxScalarType
                ? (cxxValueSpelling(arg.TypeAndValue.TypeName, sourceSpelling)
                    && cxxValueSpelling(param.TypeName, paramSpelling)
                    && SqueezeCxxSpelling(sourceSpelling) == SqueezeCxxSpelling(paramSpelling))
                : arg.BaseType == paramTy);
        if (param.IsCxxRefToPointer)
        {
            // C++ T*& borrows the caller's T* slot. A plain pointer local already
            // has exactly that slot; expressions and call results need a temporary.
            if (arg.Storage != nullptr && arg.TypeAndValue.Pointer && arg.BaseType == paramTy
                && !arg.IsRvalue)
                return arg.Storage;
            // A `?:` join with a string-literal arm (also through a nested `?:`) is a pointer
            // prvalue with no slot: clang refuses it. Incoming PHIs are followed, each once.
            llvm::SmallPtrSet<llvm::PHINode*, 4> visitedJoins;
            auto reachesLiteralArm = [&](auto&& self, llvm::Value* value) -> bool {
                if (value == nullptr) return false;
                value = value->stripPointerCasts();
                if (auto* armConst = llvm::dyn_cast<llvm::Constant>(value))
                    return IsStringLiteralConstant(armConst);
                auto* join = llvm::dyn_cast<llvm::PHINode>(value);
                if (join == nullptr || !visitedJoins.insert(join).second) return false;
                for (llvm::Value* incoming : join->incoming_values())
                    if (self(self, incoming)) return true;
                return false;
            };
            const bool literalArmJoin = llvm::isa<llvm::PHINode>(arg.Primary)
                && reachesLiteralArm(reachesLiteralArm, arg.Primary);
            // A pointer join with a prvalue arm (`x + 1`, a call) is marked by the ternary builder.
            const auto* armJoin = llvm::dyn_cast<llvm::PHINode>(arg.Primary);
            const bool prvalueArmJoin = armJoin != nullptr
                && armJoin->getMetadata("cflat.ptr.prvalue.join") != nullptr;
            if ((literalArmJoin || prvalueArmJoin) && arg.Storage == nullptr && !param.CxxConstRef()
                && !param.IsRvalueRef)
            {
                LogError(std::format("the parameter '{}' is a non-const reference to a pointer and "
                    "cannot bind a pointer rvalue; pass an lvalue", param.VariableName));
                return llvm::ConstantPointerNull::get(cflat_llvm::PointerTo(paramTy));
            }
            llvm::Value* value = arg.Primary != nullptr ? arg.Primary : LoadArgStorage(arg);
            value = LowerByValueArg(value, param, arg);
            if (value == nullptr)
                return llvm::ConstantPointerNull::get(cflat_llvm::PointerTo(paramTy));
            auto* temp = AllocaAtEntry(paramTy, nullptr, "cxx.refptr.arg");
            CreateAssignment(value, temp, arg.TypeAndValue.IsUnsignedInteger() != -1);
            return temp;
        }
        // The caller's own slot is the borrow. Only an exact type match may be handed over -
        // anything else (a coercion, a literal, a call result) is materialized into a temp,
        // which is a copy, exactly as binding a C++ const-reference to a converted value is.
        if (arg.Storage != nullptr && !arg.TypeAndValue.Pointer
            && !arg.TypeAndValue.IsInterface
            && IsCxxDerivedToBaseValue(arg.TypeAndValue, param, nullptr))
        {
            llvm::Value* address = arg.Storage;
            if (LowerCxxReferenceToBase(arg.TypeAndValue.TypeName, param.TypeName, arg, address))
                return address;
        }
        if (arg.Storage != nullptr && arg.BaseType == paramTy && sameReferentType
            && !arg.TypeAndValue.IsInterface)
            return arg.Storage;
        llvm::Value* value = arg.Primary != nullptr ? arg.Primary : LoadArgStorage(arg);
        value = LowerByValueArg(value, param, arg);
        if (value == nullptr)
            return llvm::ConstantPointerNull::get(cflat_llvm::PointerTo(paramTy));
        auto* temp = AllocaAtEntry(paramTy, nullptr, "aliasarg");
        CreateAssignment(value, temp, arg.TypeAndValue.IsUnsignedInteger() != -1);
        // The materialized slot holds an rvalue nothing owns; an `alias` param borrows and never
        // frees it, so without this it leaks. Registering THIS slot (not a fresh copy) keeps the
        // destruction count at one. Owned string / closure temps are registered upstream at the
        // argument-evaluation site and are not re-registered here.
        bool ternaryJoinNeedsRegistration = arg.Primary != nullptr
            && llvm::isa<llvm::PHINode>(arg.Primary)
            && !arg.TernaryTempAlreadyRegistered;
        RegisterBorrowedOwningStructTempAt(arg, temp, ternaryJoinNeedsRegistration);
        return temp;
    }

llvm::Value* LLVMBackend::LowerRvalueRefArg(const NamedVariable& arg, const TypeAndValue& param, bool cxxCallee)
{
        // nullptr_t itself is a pointer-sized value. A reference needs an address to a slot
        // containing that value; the null pointer value is never the reference address.
        if (cxxCallee && param.IsCxxNullptrT && IsCxxNullTypedArgument(arg))
        {
            if (arg.Storage != nullptr && IsCxxRvalueReferenceArgument(arg))
                return arg.Storage;
            llvm::Type* valueType = GetType(param);
            llvm::Value* value = arg.Primary != nullptr ? arg.Primary : LoadArgStorage(arg);
            if (valueType != nullptr && value != nullptr && value->getType() == valueType)
            {
                auto* temporary = AllocaAtEntry(valueType, nullptr, "cxx.nullptr.rrefarg");
                builder->CreateStore(value, temporary);
                return temporary;
            }
        }
        // An explicit move of a pointer names the pointer object's slot. Bind T*&& to that slot
        // directly; only pointer prvalues need the temporary below.
        if (cxxCallee && param.ElemPointer && arg.TypeAndValue.Pointer && arg.IsExplicitMove)
        {
            llvm::Value* slot = arg.Storage;
            if (slot == nullptr && !arg.CallerName.empty())
                slot = FindVariableStorage(arg.CallerName).Storage;
            if (slot != nullptr && slot->getType()->isPointerTy()) return slot;
        }
        // A pointer prvalue needs a T* temporary slot; passing its value would be a T** mismatch.
        if (cxxCallee && param.ElemPointer && (arg.TypeAndValue.Pointer
                                  || IsNullPointerConstantArgument(arg, true)
                                  || (arg.Primary != nullptr && arg.Primary->getType()->isPointerTy())
                                  || llvm::isa_and_nonnull<llvm::ConstantPointerNull>(arg.Primary)))
        {
            TypeAndValue referent = param;
            referent.ElemPointer = false;
            referent.Pointer = true;
            referent.PointerDepth = 1;
            referent.IsAlias = false;
            referent.IsRvalueRef = false;
            if (llvm::Type* refTy = GetType(referent); refTy != nullptr && refTy->isPointerTy())
            {
                llvm::Value* value = IsNullPointerConstantArgument(arg, true)
                    ? static_cast<llvm::Value*>(llvm::ConstantPointerNull::get(
                        llvm::cast<llvm::PointerType>(refTy)))
                    : (arg.Primary != nullptr ? arg.Primary : LoadArgStorage(arg));
                auto* temp = AllocaAtEntry(refTy, nullptr, "cxx.rrefarg");
                builder->CreateStore(value, temp);
                return temp;
            }
        }
        // A pointer argument (`move p` on a T*) already IS the object's address: pass its value,
        // never the slot holding it. Pointers are opaque, so no type test can tell the two apart.
        if (arg.TypeAndValue.Pointer)
            return arg.Primary != nullptr ? arg.Primary : LoadArgStorage(arg);

        if (cxxCallee && param.IsRvalueRef && !param.ElemPointer)
        {
            TypeAndValue referent = param;
            referent.Pointer = referent.IsAlias = referent.IsRvalueRef = false;
            referent.PointerDepth = 0;
            if (referent.IsPrimitive() && arg.TypeAndValue.IsPrimitive()
                && ClassifyScalarArgConversion(arg, referent) == ScalarArgConversion::Widen
                && IsProvenCxxScalarLvalue(arg))
            {
                llvm::Value* value = arg.Primary != nullptr ? arg.Primary : LoadArgStorage(arg);
                llvm::Type* valueType = GetType(referent);
                if (value != nullptr && valueType != nullptr)
                {
                    value = CreateCast(value, valueType, arg.TypeAndValue.IsUnsignedInteger() != -1,
                                       referent.IsUnsignedInteger() != -1);
                    auto* temp = AllocaAtEntry(valueType, nullptr, "cxx.rref.converted");
                    builder->CreateStore(value, temp);
                    return temp;
                }
            }
        }

        // An opaque C++ class reference is still represented by the constructor's pointer result,
        // even when its CFlat argument metadata is a by-value foreign record.
        if (cxxCallee && param.TypeName == "void")
        {
            if (arg.Storage != nullptr && arg.Storage->getType()->isPointerTy())
                return arg.Storage;
            if (arg.Primary != nullptr && arg.Primary->getType()->isPointerTy())
                return arg.Primary;
        }

        auto aliasParam = param;
        aliasParam.Pointer = false;
        aliasParam.ElemPointer = false;
        aliasParam.IsAlias = false;

        // A loaded value from an addressable object still borrows that object's slot.
        if (auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(arg.Primary))
            if (load->getType() == GetType(aliasParam)
                && (arg.Storage == load->getPointerOperand() || !arg.IsRvalue
                    || arg.TypeAndValue.IsAlias))
                return load->getPointerOperand();

        return LowerAliasByPointerArg(arg, aliasParam);
    }

llvm::Value* LLVMBackend::LowerByValueArg(llvm::Value* value, const TypeAndValue& param, const NamedVariable& arg)
{
        if (arg.IsNullConditionalOwningValue && !param.Pointer && !param.IsAlias
            && (param.IsMove || OwningSinkConsumesConcrete(param)))
        {
            if (!IsCopyableType(arg.TypeAndValue.TypeName)
                || IsLoweredCFlatOnlyStruct(arg.TypeAndValue.TypeName))
                LogError(
                    "cannot pass an owning struct read from a null-conditional '?.' access to a "
                    "consuming parameter; use an explicit null check and move or copy the field");
            NamedVariable source;
            source.Primary = value;
            source.BaseType = value != nullptr ? value->getType() : nullptr;
            source.TypeAndValue.TypeName = arg.TypeAndValue.TypeName;
            value = CreateOverloadedFunctionCall("copy", { source });
            if (value == nullptr)
                LogError(
                    "cannot safely copy an owning struct from a null-conditional '?.' access");
        }
        // Encoded closure param (list<function<...>>::add's `T value`, a substituted generic field).
        // The encoded element has the SAME machine repr as the spelling it encodes - bare code
        // pointer (thin) or fat struct - so convert exactly as an IsFunctionPointer param would.
        if (const TypeAndValue* enc = GetEncodedClosureType(param.TypeName);
            enc && value && !param.Pointer)
        {
            if (enc->IsThinFnPtr() && value->getType()->isStructTy())
                return LowerClosureFatToThinFnPtr(value, GetType(param),
                    param.VariableName, arg.LambdaCaptureNames);
            if (enc->IsThinFnPtr() && value->getType()->isPointerTy())
            {
                CheckThinFnPtrArgProvenance(value, arg, param.VariableName);
                return builder->CreateBitCast(value, GetType(param), "thinfn");
            }
            if (!enc->IsThinFnPtr() && value->getType()->isPointerTy())
                return WidenToClosureFatChecked(value, arg, param.VariableName);
        }
        // Function-pointer parameter fed the other flavour's representation, both directions.
        if (param.IsFunctionPointer && value)
        {
            // Thin `function<>` slot fed a closure fat struct (a lambda literal or a Lambda<> value).
            if (param.IsThinFnPtr() && value->getType()->isStructTy())
            {
                return LowerClosureFatToThinFnPtr(value, GetType(param),
                    param.VariableName, arg.LambdaCaptureNames);
            }
            // Fat `Lambda<>` slot - shared provenance gate, identical to the direct call path.
            if (!param.IsThinFnPtr() && !value->getType()->isStructTy()
                && value->getType()->isPointerTy())
            {
                return WidenToClosureFatChecked(value, arg, param.VariableName);
            }
            // Thin `function<>` slot fed a raw pointer - same provenance gate, thin wording.
            // Check only: the lowering below (Upconvert/bitcast) is unchanged.
            if (param.IsThinFnPtr() && value->getType()->isPointerTy())
                CheckThinFnPtrArgProvenance(value, arg, param.VariableName);
        }
        if (param.TypeName == "string" && !param.Pointer
            && value && value->getType() == cflat_llvm::PointerTo(builder->getInt8Ty()))
        {
            // Implicit char* -> string coercion: string literal or char* passed to a string param.
            auto* c = llvm::dyn_cast<llvm::Constant>(value);
            if (c && IsStringLiteralConstant(c))
                value = WrapStringLiteralAsString(value);
            else if (GetFunction("operator string"))
            {
                NamedVariable argNV2;
                argNV2.Primary = value;
                argNV2.BaseType = value->getType();
                argNV2.TypeAndValue.TypeName = "char";
                argNV2.TypeAndValue.Pointer = true;
                value = CreateOverloadedFunctionCall("operator string", { argNV2 });
            }
            else
                value = WrapStringLiteralAsString(value);
        }
        else
        {
            // Convert to match the declared parameter type (e.g. i16 -> i32; struct identity).
            // Integer -> bool converts, narrowing is refused - the paths that bind without
            // per-argument scoring (interface dispatch, a variadic candidate's declared
            // parameters) reach the check only here.
            bool argIsUnsigned = arg.TypeAndValue.IsUnsignedInteger() != -1;
            std::string what = param.VariableName.empty()
                ? std::string("a by-value parameter")
                : std::format("parameter '{}'", param.VariableName);
            value = ConvertIntegerCallArgument(value, GetType(param), argIsUnsigned,
                SpellType(*this, param), what, arg.ArithNarrowWidth);
        }

        // A string not statically known to own its buffer, passed to a move parameter.
        // It may still carry the runtime OWNED bit (a plain-'string' call result, or an
        // owned local read bare) - transferring that as-is lets the callee/list adopt it.
        // A literal or view (owned bit clear) must instead be heap-copied so the callee
        // receives an independent, freeable buffer that cannot dangle or leak. The static
        // flag cannot tell these apart, so branch on the owned bit at runtime.
        if (param.IsMove && param.TypeName == "string" && !arg.IsOwningString)
        {
            auto* strTy = llvm::StructType::getTypeByName(*context, "string");
            if (strTy && GetFunction("operator new"))
            {
                auto* srcPtr = builder->CreateExtractValue(value, { 0u }, "litptr");
                auto* rawLen = builder->CreateExtractValue(value, { 1u }, "litlenraw");
                // Owned bit is _len's high bit - already-owned strings transfer without a copy.
                auto* alreadyOwned = builder->CreateICmpSLT(rawLen, builder->getInt32(0), "movearg.owned");
                // An EMPTY/default string ({ null, 0 }) has the owned bit clear but no buffer to
                // copy from: the copy path would memcpy 1 byte off a null _ptr and segfault. It
                // owns nothing, so transfer it as-is and let the callee's destructor no-op on the
                // null buffer. Reachable without any global (`string s = default; sink(move s);`)
                // and also as the re-initialized state q11 leaves in program-lifetime storage.
                auto* srcIsNull = builder->CreateICmpEQ(
                    srcPtr, llvm::ConstantPointerNull::get(cflat_llvm::PointerTo(builder->getInt8Ty())),
                    "movearg.empty");
                auto* skipCopy = builder->CreateOr(alreadyOwned, srcIsNull, "movearg.nocopy");
                auto* transferBB = builder->GetInsertBlock();
                auto* fn = transferBB->getParent();
                auto* copyBB  = llvm::BasicBlock::Create(*context, "movearg.copy",  fn);
                auto* mergeBB = llvm::BasicBlock::Create(*context, "movearg.merge", fn);
                builder->CreateCondBr(skipCopy, mergeBB, copyBB);

                // Non-owning source: heap-copy the bytes (plus the null terminator) and set
                // the OWNED bit. Mask off _len's high bit before using it as a byte count -
                // it would otherwise inflate allocSize to ~2GB and AV the memcpy.
                builder->SetInsertPoint(copyBB);
                auto* srcLen = builder->CreateAnd(rawLen, builder->getInt32(0x7FFFFFFF), "litlen");
                auto* allocSize = builder->CreateAdd(
                    builder->CreateZExt(srcLen, builder->getInt64Ty()),
                    builder->getInt64(1), "litbufsz");
                NamedVariable szArg;
                szArg.Primary  = allocSize;
                szArg.BaseType = builder->getInt64Ty();
                auto* rawPtr  = CreateOverloadedFunctionCall("operator new", { szArg });
                auto* heapPtr = builder->CreateBitCast(rawPtr, cflat_llvm::PointerTo(builder->getInt8Ty()), "litbuf");
                builder->CreateMemCpy(heapPtr, llvm::MaybeAlign(1), srcPtr, llvm::MaybeAlign(1), allocSize);
                auto* ownedLen = builder->CreateOr(srcLen, builder->getInt32(0x80000000), "ownedlen");
                llvm::Value* copyVal = llvm::UndefValue::get(strTy);
                copyVal = builder->CreateInsertValue(copyVal, heapPtr, { 0u });
                copyVal = builder->CreateInsertValue(copyVal, ownedLen, { 1u });
                auto* copyEndBB = builder->GetInsertBlock();
                builder->CreateBr(mergeBB);

                // Merge: the original owned value, or the fresh owned copy.
                builder->SetInsertPoint(mergeBB);
                auto* phi = builder->CreatePHI(strTy, 2, "movearg.val");
                phi->addIncoming(value, transferBB);
                phi->addIncoming(copyVal, copyEndBB);
                value = phi;
            }
        }

        return value;
    }

llvm::Value* LLVMBackend::LowerPointerParamArg(const NamedVariable& arg)
{
        if (!arg.TypeAndValue.Pointer
            && (arg.Storage != nullptr || arg.NullConditionalBorrowStorage != nullptr)
            && !(arg.Primary != nullptr && arg.Primary->getType()->isPointerTy()))
        {
            // `p?.f`: borrow the field in place (null-or-field-address), never a temporary copy.
            return arg.NullConditionalBorrowStorage != nullptr ? arg.NullConditionalBorrowStorage : arg.Storage;
        }

        if (!arg.TypeAndValue.Pointer && arg.Storage == nullptr
            && arg.Primary != nullptr && arg.Primary->getType()->isStructTy())
        {
            auto* temporary = AllocaAtEntry(arg.Primary->getType(), nullptr);
            builder->CreateStore(arg.Primary, temporary);
            return temporary;
        }

        if (arg.Primary == nullptr && arg.Storage != nullptr
            && llvm::isa<llvm::AllocaInst>(arg.Storage))
            return LoadArgStorage(arg);

        return arg.GetValue();
    }

bool LLVMBackend::ArgumentProvablyMismatchesParameter(const NamedVariable& arg, const TypeAndValue& param) const
{
        if (!param.IsFunctionPointer && !IsEncodedClosureType(param.TypeName))
            return false;
        if (ClosureValueCanAdaptToSlot(arg, param))
            return false;
        // Second proof: two signatures that provably name different function types, at the same
        // indirection shape. Precedes the closure early-out, which such a pointer would satisfy.
        if (param.IsFunctionPointer
            && FunctionPointerShapeOf(arg.TypeAndValue, &arg) == FunctionPointerShapeOf(param, nullptr)
            && FuncPtrSignaturesProvablyDiffer(arg.TypeAndValue, param))
            return true;
        // Same proof for a NAMED function, whose signature lives in the function table rather than
        // on the argument. Shape is not consulted: a bare name is always a plain value.
        if (NamedFunctionArgMismatches(arg, param))
            return true;
        // Any recorded closure evidence disproves the mismatch.
        if (arg.TypeAndValue.IsFunctionPointer || arg.TypeAndValue.TypeName == "__closure_fat_ptr"
            || IsEncodedClosureType(arg.TypeAndValue.TypeName))
            return false;
        if (arg.Primary && llvm::isa<llvm::Function>(arg.Primary))
            return false;
        // Positive evidence only: a pointer, a struct, or an unknown shape is NOT proof.
        return arg.BaseType != nullptr
            && (arg.BaseType->isIntegerTy() || arg.BaseType->isFloatingPointTy());
    }

bool LLVMBackend::PointerArgIntoByValueParam(const NamedVariable& arg, const TypeAndValue& param) const
{
        return arg.TypeAndValue.Pointer && !param.Pointer && !param.IsInterface
            && !param.IsFunctionPointer && !param.TypeName.empty()
            && arg.TypeAndValue.TypeName == param.TypeName
            && IsDataStructure(param.TypeName)
            && !IsRawPointerToCoreUnique(arg, param);
    }

bool LLVMBackend::PointerArgIntoByValuePrimitiveParam(const NamedVariable& arg, const TypeAndValue& param) const
{
        // A bare `nullptr` carries no type flag at all, so it is proven from the CONSTANT instead.
        // Both proofs are confined to a by-value primitive slot, where no pointer is ever legal.
        bool argIsPointer = (arg.TypeAndValue.Pointer && arg.BaseType && arg.BaseType->isPointerTy())
            || (arg.Primary && llvm::isa<llvm::ConstantPointerNull>(arg.Primary));
        return argIsPointer
            && !param.Pointer && !param.IsInterface && !param.IsFunctionPointer
            && !param.IsArrayView && param.ConstArraySize == 0
            && param.IsPrimitive() && param.TypeName != "void";
    }

bool LLVMBackend::IsAddressedUniqueSlotToRawSlot(const NamedVariable& arg,
        const TypeAndValue& source, const TypeAndValue& param) const
{
        return arg.AddressedUniqueSlot != nullptr
            && source.IsProvenSinglePointerDepth()
            && IsCoreUniqueType(source.TypeName)
            && param.IsProvenDoublePointer()
            && !param.IsInterface && !param.IsFunctionPointer
            && MangledGenericArgument(*this, source.TypeName) == param.TypeName
            && !IsInterfaceType(param.TypeName);
}

std::string LLVMBackend::FuncPtrArgDepthMismatch(const NamedVariable& arg,
        const TypeAndValue::FuncPtrParam& p) const
{
        if (!p.Pointer || p.PointerDepth < 1 || p.TypeName == "void" || p.TypeName.empty())
            return "";

        // Rebuild the parameter as a judgeable value: depth 2 is what ElemPointer would say.
        TypeAndValue param;
        param.TypeName = p.TypeName;
        param.Pointer = true;
        param.PointerDepth = p.PointerDepth;
        param.ElemPointer = p.PointerDepth >= 2;
        if (IsAddressedUniqueSlotToRawSlot(arg, arg.TypeAndValue, param)
            || !arg.TypeAndValue.PointerDepthRefuses(param))
            return "";

        TypeAndValue parameterBase;
        parameterBase.TypeName = p.TypeName;
        std::string shown = SpellType(*this, parameterBase);
        std::string argShown = SpellType(*this, arg.TypeAndValue);
        // A primitive pointer argument carries no CFlat TypeName; report only the depth then.
        if (p.PointerDepth >= 2)
            return std::format("has type '{}**' and the argument has {} - there is no "
                "implicit address-of. Take its address with '&' at the call site", shown,
                argShown.empty() ? std::string("one fewer level of indirection")
                                 : std::format("type '{}'", argShown));
        return std::format("has type '{}*' and the argument has {} - there is no implicit "
            "dereference. Dereference it with '*' at the call site", shown,
            argShown.empty() ? std::string("one more level of indirection")
                             : std::format("type '{}'", argShown));
    }

bool LLVMBackend::DiagnoseProvableInterfaceArgMismatch(const std::string& ifaceName,
        const std::string& methodName, const std::vector<NamedVariable>& args,
        const std::vector<TypeAndValue>& params)
{
        const std::string displayIface = SpellType(*this, TypeAndValue{ .TypeName = ifaceName });
        for (size_t i = 0; i < args.size() && i < params.size(); i++)
        {
            if (IsAddressedUniqueSlotToRawSlot(args[i], args[i].TypeAndValue, params[i]))
                continue;
            if (ArgumentProvablyMismatchesParameter(args[i], params[i]))
            {
                std::string why = DescribeFuncPtrSignatureMismatch(args[i].TypeAndValue, params[i]);
                LogErrorMessage("no method of '{}.{}' matches the given arguments{}",
                    { displayIface, methodName, why.empty() ? "" : " - " + why });
                return true;
            }
            if (args[i].TypeAndValue.PointerDepthRefuses(params[i]))
            {
                bool tooDeep = args[i].TypeAndValue.IsProvenDoublePointer()
                    || args[i].TypeAndValue.IsProvenDecayedDoublePointer();
                std::string shown = SpellType(*this, params[i]);
                std::string argShown = SpellType(*this, args[i].TypeAndValue);
                if (tooDeep)
                {
                    LogErrorMessage(
                        "call to '{}.{}': parameter {} '{}' has type '{}' and the argument has {} - "
                        "there is no implicit dereference. Dereference it with '{}' at the call site.",
                        { displayIface, methodName, std::to_string(i), params[i].VariableName,
                          shown, argShown.empty() ? std::string("one more level of indirection")
                                           : std::format("type '{}'", argShown), "*" });
                }
                else
                {
                    LogErrorMessage(
                        "call to '{}.{}': parameter {} '{}' has type '{}' and the argument has {} - "
                        "there is no implicit address-of. Take its address with '{}' at the call site.",
                        { displayIface, methodName, std::to_string(i), params[i].VariableName,
                          shown, argShown.empty() ? std::string("one fewer level of indirection")
                                           : std::format("type '{}'", argShown), "&" });
                }
                return true;
            }
            if (PointerArgIntoByValueParam(args[i], params[i]))
            {
                // Rendered when the rendering is provably writable source; a nested instantiation
                // stays raw and loses the advice clause rather than naming a type that cannot exist.
                std::string shown = SpellType(*this, params[i]);
                if (!shown.empty())
                {
                    LogErrorMessage(
                        "call to '{}.{}': cannot pass a '{}' to by-value parameter '{}' of type '{}' - "
                        "there is no implicit dereference. Write '{}' at the call site to pass the "
                        "pointee, or declare the parameter as '{}'.",
                        { displayIface, methodName, std::format("{}*", shown), params[i].VariableName,
                          shown, "*", std::format("{}*", shown) });
                }
                else
                {
                    LogErrorMessage(
                        "call to '{}.{}': cannot pass a '{}' to by-value parameter '{}' of type '{}' - "
                        "there is no implicit dereference. Write '{}' at the call site to pass the "
                        "pointee.",
                        { displayIface, methodName, std::format("{}*", shown), params[i].VariableName,
                          shown, "*" });
                }
                return true;
            }
            if (PointerArgIntoByValuePrimitiveParam(args[i], params[i]))
            {
                LogErrorMessage(
                    "call to '{}.{}': cannot pass a pointer to by-value parameter '{}' of type '{}' - "
                    "an address is not of type '{}'. Dereference it with '{}' at the call site, or "
                    "declare the parameter as '{}'.",
                    { displayIface, methodName, params[i].VariableName, SpellType(*this, params[i]),
                      std::format("{}*", SpellType(*this, params[i])), "*",
                      std::format("{}*", SpellType(*this, params[i])) });
                return true;
            }
        }
        return false;
    }

int LLVMBackend::ResolveInterfaceMethodSlot(const std::string& ifaceName, const std::string& methodName,
        const std::vector<InterfaceMethod>& methods, const std::vector<NamedVariable>& args,
        std::vector<NamedVariable>& matchedArgs, int& fallbackDefaultSlot, bool constReceiver)
{
        matchedArgs = args;
        fallbackDefaultSlot = -1;
        const std::string displayIface = SpellType(*this, TypeAndValue{ .TypeName = ifaceName });
        auto sameParameterShape = [](const TypeAndValue& left, const TypeAndValue& right) {
            return left.TypeName == right.TypeName && left.Pointer == right.Pointer
                && left.ElemPointer == right.ElemPointer && left.PointerDepth == right.PointerDepth
                && left.ConstMask == right.ConstMask && left.IsAlias == right.IsAlias
                && left.IsRvalueRef == right.IsRvalueRef && left.IsMove == right.IsMove
                && left.IsArrayView == right.IsArrayView && left.IsInterface == right.IsInterface
                && left.IsFunctionPointer == right.IsFunctionPointer;
        };
        auto sameMethodSignature = [&](const InterfaceMethod& left, const InterfaceMethod& right) {
            if (left.Parameters.size() != right.Parameters.size()) return false;
            for (size_t i = 0; i < left.Parameters.size(); ++i)
                if (!sameParameterShape(left.Parameters[i], right.Parameters[i])) return false;
            return true;
        };

        NamedVariable receiverArg;
        receiverArg.TypeAndValue.TypeName = ifaceName;
        receiverArg.TypeAndValue.Pointer = true;
        receiverArg.TypeAndValue.IsInterface = true;
        receiverArg.TypeAndValue.IsInterfacePointer = true;
        receiverArg.TypeAndValue.VariableName = "this";
        receiverArg.IsThisReceiver = true;
        TypeAndValue receiverParam = receiverArg.TypeAndValue;

        size_t inheritedCount = 0;
        if (auto parents = interfaceParents.find(ResolveInterfaceName(ifaceName));
            parents != interfaceParents.end())
            for (const auto& parentName : parents->second)
                if (const auto* parentMethods = FindInterface(parentName))
                    inheritedCount += parentMethods->size();

        std::vector<int> allByName;
        std::vector<int> ownByName;
        for (int i = 0; i < static_cast<int>(methods.size()); ++i)
            if (methods[i].Name == methodName)
            {
                allByName.push_back(i);
                if (static_cast<size_t>(i) >= inheritedCount)
                    ownByName.push_back(i);
            }
        std::vector<int> byName;
        for (int inheritedIdx : allByName)
        {
            const bool redeclared = static_cast<size_t>(inheritedIdx) < inheritedCount
                && std::any_of(ownByName.begin(), ownByName.end(), [&](int ownIdx) {
                    return sameMethodSignature(methods[inheritedIdx], methods[ownIdx]);
                });
            if (!redeclared) byName.push_back(inheritedIdx);
        }
        const bool hasConstOverload = constReceiver
            && std::any_of(byName.begin(), byName.end(), [&](int idx) {
                return methods[idx].IsConst;
            });
        if (hasConstOverload)
        {
            receiverArg.TypeAndValue.ConstMask = 1u;
            std::erase_if(byName, [&](int idx) { return !methods[idx].IsConst; });
        }
        if (byName.empty())
        {
            LogErrorMessage("interface '{}' has no method '{}'", { displayIface, methodName });
            return -1;
        }

        auto helperSet = [&](const InterfaceMethod& method) -> const std::vector<FunctionSymbol>* {
            if (method.DefaultWrapperName.empty()) return nullptr;
            auto it = functionTable.find(method.DefaultWrapperName);
            return it == functionTable.end() ? nullptr : &it->second;
        };
        auto helperMatchesMethod = [&](const FunctionSymbol& helper, const InterfaceMethod& method) {
            if (helper.IsMethod || helper.Parameters.empty()
                || helper.Parameters.size() > method.Parameters.size() + 1)
                return false;
            for (size_t i = 1; i < helper.Parameters.size(); ++i)
                if (!sameParameterShape(helper.Parameters[i], method.Parameters[i - 1]))
                    return false;
            return true;
        };

        std::vector<size_t> availableArities;
        std::vector<int> exactSlots;
        std::vector<std::pair<std::vector<NamedVariable>, FunctionSymbol>> candidates;
        for (int idx : byName)
        {
            const InterfaceMethod& method = methods[idx];
            availableArities.push_back(method.Parameters.size());
            if (method.Parameters.size() == args.size())
            {
                exactSlots.push_back(idx);
                std::vector<NamedVariable> candidateArgs{ receiverArg };
                candidateArgs.insert(candidateArgs.end(), args.begin(), args.end());
                std::vector<TypeAndValue> methodParams{ receiverParam };
                methodParams.insert(methodParams.end(), method.Parameters.begin(), method.Parameters.end());
                std::vector<NamedVariable> matched = MatchFunction(candidateArgs, methodParams, false, true);
                if (matched.empty()) continue;

                FunctionSymbol sym;
                sym.UniqueName = std::to_string(idx);
                sym.SourceName = displayIface + "." + methodName;
                sym.ReturnType = method.ReturnType;
                sym.Parameters = std::move(methodParams);
                sym.IsMethod = true;
                candidates.emplace_back(std::move(matched), std::move(sym));
                continue;
            }

            const auto* helpers = helperSet(method);
            if (helpers == nullptr || args.size() >= method.Parameters.size()) continue;
            for (const FunctionSymbol& helper : *helpers)
            {
                if (!helperMatchesMethod(helper, method)) continue;
                const size_t helperArity = helper.Parameters.size() - 1;
                availableArities.push_back(helperArity);
                if (helperArity != args.size()) continue;
                if (fallbackDefaultSlot < 0) fallbackDefaultSlot = idx;

                std::vector<TypeAndValue> helperParams(helper.Parameters.begin() + 1,
                                                       helper.Parameters.end());
                std::vector<NamedVariable> candidateArgs{ receiverArg };
                candidateArgs.insert(candidateArgs.end(), args.begin(), args.end());
                std::vector<TypeAndValue> candidateParams{ receiverParam };
                candidateParams.insert(candidateParams.end(), helperParams.begin(), helperParams.end());
                std::vector<NamedVariable> matched = MatchFunction(candidateArgs, candidateParams, false, true);
                if (matched.empty()) continue;

                FunctionSymbol sym;
                sym.UniqueName = std::to_string(idx);
                sym.SourceName = displayIface + "." + methodName;
                sym.ReturnType = method.ReturnType;
                sym.Parameters = std::move(candidateParams);
                sym.IsMethod = true;
                candidates.emplace_back(std::move(matched), std::move(sym));
            }
        }

        if (candidates.empty())
        {
            if (fallbackDefaultSlot >= 0)
                return -1;
            if (!exactSlots.empty())
            {
                const int idx = exactSlots.front();
                if (!args.empty())
                {
                    auto matched = MatchFunction(args, methods[idx].Parameters);
                    if (!matched.empty()) matchedArgs = matched;
                }
                if (DiagnoseProvableInterfaceArgMismatch(ifaceName, methodName, matchedArgs,
                                                          methods[idx].Parameters))
                    return -1;
                LogError(std::format("no overload of '{}' matches the given arguments.", methodName));
                return -1;
            }

            std::sort(availableArities.begin(), availableArities.end());
            availableArities.erase(std::unique(availableArities.begin(), availableArities.end()),
                                   availableArities.end());
            std::string expected;
            for (size_t i = 0; i < availableArities.size(); ++i)
            {
                if (i > 0) expected += (i + 1 == availableArities.size()) ? " or " : ", ";
                expected += std::to_string(availableArities[i]);
            }
            LogErrorMessage(
                "no overload of interface method '{}.{}' takes {} argument(s); expected {}",
                { displayIface, methodName, std::to_string(args.size()), expected });
            return -1;
        }

        std::vector<FunctionSymbol> tiedCandidates;
        const auto& [scored, winner] = ComputeOverloadFunction(candidates, &tiedCandidates);
        std::vector<int> ambiguousSlots;
        for (const auto& tied : tiedCandidates)
        {
            const int idx = std::stoi(tied.UniqueName);
            if (std::find(ambiguousSlots.begin(), ambiguousSlots.end(), idx) == ambiguousSlots.end())
                ambiguousSlots.push_back(idx);
        }
        const int winnerIdx = winner.UniqueName.empty() ? -1 : std::stoi(winner.UniqueName);
        for (size_t i = 0; winnerIdx >= 0 && i < candidates.size(); ++i)
            for (size_t j = i + 1; j < candidates.size(); ++j)
            {
                const int leftIdx = std::stoi(candidates[i].second.UniqueName);
                const int rightIdx = std::stoi(candidates[j].second.UniqueName);
                if (winnerIdx != leftIdx && winnerIdx != rightIdx) continue;
                if (methods[leftIdx].Parameters.size() == args.size()
                    && methods[rightIdx].Parameters.size() == args.size())
                    continue;
                bool sameCallTypes = true;
                for (size_t argIdx = 0; argIdx < args.size(); ++argIdx)
                    if (!sameParameterShape(methods[leftIdx].Parameters[argIdx],
                                            methods[rightIdx].Parameters[argIdx]))
                    {
                        sameCallTypes = false;
                        break;
                    }
                if (!sameCallTypes) continue;
                for (int idx : { leftIdx, rightIdx })
                    if (std::find(ambiguousSlots.begin(), ambiguousSlots.end(), idx)
                        == ambiguousSlots.end())
                        ambiguousSlots.push_back(idx);
            }

        if (!ambiguousSlots.empty())
        {
            std::string candidateList;
            for (int idx : ambiguousSlots)
            {
                std::string params;
                for (const auto& param : methods[idx].Parameters)
                    params += (params.empty() ? "" : ", ") + SpellType(*this, param);
                candidateList += (candidateList.empty() ? "" : ", ")
                    + displayIface + "." + methodName + "(" + params + ")";
            }
            LogErrorMessage(
                "ambiguous call to '{}.{}': no candidate ranks better than the others: {}. "
                "Cast the argument to the parameter type you mean.",
                { displayIface, methodName, candidateList });
            return -1;
        }

        if (winner.UniqueName.empty())
        {
            if (fallbackDefaultSlot >= 0)
                return -1;
            if (!exactSlots.empty())
            {
                const int firstSlot = candidates.empty()
                    ? exactSlots.front() : std::stoi(candidates.front().second.UniqueName);
                const auto& firstMethod = methods[firstSlot];
                std::vector<NamedVariable> diagnosticArgs = args;
                if (!candidates.empty() && !candidates.front().first.empty())
                {
                    diagnosticArgs = candidates.front().first;
                    diagnosticArgs.erase(diagnosticArgs.begin());
                }
                if (!candidates.empty())
                {
                    if (DiagnoseProvableInterfaceArgMismatch(ifaceName, methodName, diagnosticArgs,
                                                              firstMethod.Parameters))
                        return -1;
                    for (size_t i = 0; i < diagnosticArgs.size()
                         && i < firstMethod.Parameters.size(); ++i)
                    {
                        if (RejectFuncPtrShapeMismatch(diagnosticArgs[i], firstMethod.Parameters[i])
                            || RejectCodeValueIntoDataParam(diagnosticArgs[i], firstMethod.Parameters[i],
                                                           ifaceName, methodName))
                            return -1;
                        const auto& arg = diagnosticArgs[i];
                        const auto& param = firstMethod.Parameters[i];
                        auto* paramType = GetType(param);
                        if (ClassifyScalarArgConversion(arg, param) == ScalarArgConversion::Narrow
                            && arg.BaseType != nullptr && arg.BaseType->isIntegerTy()
                            && paramType != nullptr && paramType->isIntegerTy())
                        {
                            const std::string parameter = param.VariableName.empty()
                                ? std::format("parameter {}", i)
                                : std::format("parameter '{}'", param.VariableName);
                            const std::string target = SpellType(*this, param);
                            const std::string what = parameter + " of '" + displayIface + "."
                                + methodName + "'";
                            LogErrorMessage(
                                "cannot pass a {}-bit integer to {}: implicit integer narrowing "
                                "to '{}' is not allowed at a call argument - cast it explicitly "
                                "with '({})'",
                                { std::to_string(arg.BaseType->getIntegerBitWidth()), what,
                                  target, target });
                        }
                    }
                }
                auto isObjectPointer = [&](const TypeAndValue& type) {
                    return type.Pointer && !type.IsInterface && !type.IsFunctionPointer
                        && !type.IsArrayView && type.ValuePointerDepth() == 1
                        && !type.ElemPointer && !type.TypeName.empty()
                        && IsDataStructure(type.TypeName);
                };
                const bool unrankedObjectPointers = !args.empty()
                    && std::all_of(args.begin(), args.end(), [&](const NamedVariable& arg) {
                        return isObjectPointer(arg.TypeAndValue);
                    })
                    && std::all_of(byName.begin(), byName.end(), [&](int idx) {
                        const auto& params = methods[idx].Parameters;
                        return params.size() == args.size()
                            && std::all_of(params.begin(), params.end(), [&](const TypeAndValue& param) {
                                return isObjectPointer(param);
                            });
                    });
                if (unrankedObjectPointers)
                {
                    int pointerSlot = -1;
                    size_t bestDerivedCount = SIZE_MAX;
                    bool tiedPointerSlots = false;
                    for (int idx : exactSlots)
                    {
                        const auto& params = methods[idx].Parameters;
                        bool related = params.size() == args.size();
                        size_t derivedCount = 0;
                        for (size_t i = 0; related && i < args.size(); ++i)
                        {
                            const TypeAndValue& actual = args[i].TypeAndValue;
                            const TypeAndValue& param = params[i];
                            if (actual.ValuePointerDepth() != param.ValuePointerDepth()
                                || (actual.ConstMask & ~param.ConstMask) != 0)
                            {
                                related = false;
                                break;
                            }
                            const bool exactPointee = ResolveTypeAlias(actual.TypeName)
                                == ResolveTypeAlias(param.TypeName);
                            const bool derivedPointee = !exactPointee
                                && IsCxxRecord(actual.TypeName) && IsCxxRecord(param.TypeName)
                                && IsCxxBaseOf(param.TypeName, actual.TypeName);
                            if (derivedPointee) ++derivedCount;
                            else if (!exactPointee) related = false;
                        }
                        if (!related) continue;
                        if (derivedCount < bestDerivedCount)
                        {
                            pointerSlot = idx;
                            bestDerivedCount = derivedCount;
                            tiedPointerSlots = false;
                        }
                        else if (derivedCount == bestDerivedCount)
                            tiedPointerSlots = true;
                    }
                    if (tiedPointerSlots)
                    {
                        LogError(std::format("ambiguous call to '{}.{}' with object pointer arguments",
                                             displayIface, methodName));
                        return -1;
                    }
                    if (pointerSlot >= 0)
                    {
                        auto matched = MatchFunction(args, methods[pointerSlot].Parameters);
                        if (!matched.empty()) matchedArgs = std::move(matched);
                        return pointerSlot;
                    }
                }
                // Keep exact calls and safely lowerable scalar widenings.
                bool safelyLowerableScalars = diagnosticArgs.size() == firstMethod.Parameters.size();
                for (size_t i = 0; safelyLowerableScalars && i < diagnosticArgs.size(); ++i)
                {
                    const auto conversion = ClassifyScalarArgConversion(
                        diagnosticArgs[i], firstMethod.Parameters[i]);
                    safelyLowerableScalars = conversion == ScalarArgConversion::Exact
                        || conversion == ScalarArgConversion::Widen;
                    auto* targetType = GetType(firstMethod.Parameters[i]);
                    if (safelyLowerableScalars && diagnosticArgs[i].BaseType != nullptr
                        && diagnosticArgs[i].BaseType->isFloatingPointTy())
                    {
                        if (targetType != nullptr && targetType->isFloatingPointTy()
                            && diagnosticArgs[i].BaseType->getPrimitiveSizeInBits().getFixedValue()
                                > targetType->getPrimitiveSizeInBits().getFixedValue())
                            safelyLowerableScalars = false;
                    }
                }
                if (safelyLowerableScalars)
                {
                    matchedArgs = std::move(diagnosticArgs);
                    return firstSlot;
                }
            }
            LogError(std::format("no overload of '{}' matches the given arguments.", methodName));
            return -1;
        }

        int selectedIdx = std::stoi(winner.UniqueName);
        if (!scored.empty())
        {
            matchedArgs = scored;
            matchedArgs.erase(matchedArgs.begin());
        }
        else if (!candidates.front().first.empty())
        {
            matchedArgs = candidates.front().first;
            matchedArgs.erase(matchedArgs.begin());
        }

        const InterfaceMethod& selected = methods[selectedIdx];
        std::vector<NamedVariable> diagnosticArgs = args;
        for (const auto& candidate : candidates)
            if (candidate.second.UniqueName == std::to_string(selectedIdx))
            {
                diagnosticArgs = candidate.first;
                if (!diagnosticArgs.empty()) diagnosticArgs.erase(diagnosticArgs.begin());
                break;
            }
        if (args.size() == selected.Parameters.size()
            && DiagnoseProvableInterfaceArgMismatch(ifaceName, methodName, diagnosticArgs,
                                                     selected.Parameters))
            return -1;
        return selectedIdx;
    }

llvm::Value* LLVMBackend::CallInterfaceMethod(llvm::Value* ifacePtr, const std::string& ifaceName,
        const std::string& methodName, const std::vector<NamedVariable>& extraArgNVs,
        const NullIfaceDispatchSite* site, const NamedVariable* userReceiver)
{
        auto fatTy = GetFatPtrType();
        auto ptrTy = cflat_llvm::PointerTo(builder->getInt8Ty());

        auto vtablePtrField = builder->CreateStructGEP(fatTy, ifacePtr, 0);
        auto vtablePtr = builder->CreateLoad(ptrTy, vtablePtrField);

        // Record only - the definitely-null proof needs the finished block (see
        // RunNullIfaceDispatchCheck). The vtable load is the anchor: it is used by the call
        // below, so it cannot be erased out from under the deferred walk.
        if (site != nullptr)
            RecordPendingNullIfaceDispatch(*site, ifacePtr, vtablePtr, ifaceName);

        auto dataPtrField = builder->CreateStructGEP(fatTy, ifacePtr, 1);
        auto dataPtr = builder->CreateLoad(ptrTy, dataPtrField);

        const auto* ifaceMethods = FindInterface(ifaceName);
        if (ifaceMethods == nullptr)
        {
            LogErrorMessage("unknown interface '{}'",
                { SpellType(*this, TypeAndValue{ .TypeName = ifaceName }) });
            return nullptr;
        }

        const std::string displayMethod = SpellType(*this, TypeAndValue{ .TypeName = ifaceName })
            + "." + methodName;
        auto callDefaultWrapper = [&](const InterfaceMethod& wrapperMethod) {
            NamedVariable ifaceArg = userReceiver != nullptr ? *userReceiver : NamedVariable{};
            ifaceArg.TypeAndValue.TypeName = ifaceName;
            ifaceArg.TypeAndValue.Pointer = false;
            ifaceArg.TypeAndValue.ElemPointer = false;
            ifaceArg.TypeAndValue.IsInterface = true;
            ifaceArg.TypeAndValue.IsInterfacePointer = false;
            ifaceArg.TypeAndValue.VariableName.clear();
            ifaceArg.IsThisReceiver = true;
            ifaceArg.Primary = builder->CreateLoad(fatTy, ifacePtr, "iface_default_receiver");
            ifaceArg.BaseType = fatTy;
            ifaceArg.Storage = ifacePtr;

            std::vector<NamedVariable> helperArgs;
            helperArgs.reserve(extraArgNVs.size() + 1);
            helperArgs.push_back(std::move(ifaceArg));
            helperArgs.insert(helperArgs.end(), extraArgNVs.begin(), extraArgNVs.end());
            return CreateOverloadedFunctionCall(wrapperMethod.DefaultWrapperName, helperArgs,
                                                false, displayMethod);
        };
        const bool constReceiver = userReceiver != nullptr
            && (userReceiver->TypeAndValue.ConstMask & 1u) != 0;
        if (constReceiver)
        {
            std::string constObject, constReason;
            if (ConstReceiverReason(*userReceiver, constObject, constReason))
            {
                bool hasConstBindable = false;
                std::vector<int> bindableNonConstSlots;
                for (int i = 0; i < static_cast<int>(ifaceMethods->size()); ++i)
                {
                    const auto& method = (*ifaceMethods)[i];
                    if (method.Name != methodName) continue;
                    if (method.Parameters.size() != extraArgNVs.size()) continue;
                    auto bound = MatchFunction(extraArgNVs, method.Parameters, false, true);
                    bool methodBinds = bound.size() == extraArgNVs.size();
                    for (size_t argIndex = 0; methodBinds && argIndex < extraArgNVs.size(); ++argIndex)
                    {
                        auto* targetType = GetType(method.Parameters[argIndex]);
                        const auto conversion = ClassifyScalarArgConversion(
                            extraArgNVs[argIndex], method.Parameters[argIndex]);
                        if (conversion == ScalarArgConversion::Widen
                            && extraArgNVs[argIndex].BaseType != nullptr
                            && extraArgNVs[argIndex].BaseType->isIntegerTy()
                            && targetType != nullptr && targetType->isFloatingPointTy())
                            methodBinds = false;
                    }
                    if (!methodBinds) continue;
                    if (method.IsConst) hasConstBindable = true;
                    else bindableNonConstSlots.push_back(i);
                }
                if (!hasConstBindable && bindableNonConstSlots.size() == 1)
                {
                    const auto& method = (*ifaceMethods)[bindableNonConstSlots.front()];
                    RefuseConstInterfaceReceiverCall(*userReceiver, ifaceName, methodName, &method);
                    return nullptr;
                }
            }
        }
        std::vector<NamedVariable> callArgNVs;
        int fallbackDefaultSlot = -1;
        int methodIdx = ResolveInterfaceMethodSlot(ifaceName, methodName, *ifaceMethods,
                                                   extraArgNVs, callArgNVs, fallbackDefaultSlot,
                                                   constReceiver);
        if (methodIdx < 0)
        {
            if (fallbackDefaultSlot >= 0)
                return callDefaultWrapper((*ifaceMethods)[fallbackDefaultSlot]);
            return nullptr;
        }
        const InterfaceMethod* methodInfo = &(*ifaceMethods)[methodIdx];
        if (extraArgNVs.size() < methodInfo->Parameters.size())
            return callDefaultWrapper(*methodInfo);
        if (userReceiver != nullptr && !methodInfo->IsConst)
            RefuseConstInterfaceReceiverCall(*userReceiver, ifaceName, methodName);

        bool receiverResolved = false;
        if (userReceiver != nullptr && userReceiver->Storage != nullptr)
        {
            auto* block = builder->GetInsertBlock();
            auto it = builder->GetInsertPoint();
            while (it != block->begin())
            {
                --it;
                auto* store = llvm::dyn_cast<llvm::StoreInst>(&*it);
                if (store == nullptr || store->getPointerOperand() != userReceiver->Storage)
                    continue;
                if (const auto* box = FindInterfaceBoxByFatValue(store->getValueOperand());
                    box != nullptr && box->InterfaceName == ifaceName)
                {
                    NoteThisClosureMemberCall(LookupInterfaceMethodImpl(box->SourceClassName,
                                                                        *methodInfo));
                    receiverResolved = true;
                }
                break;
            }
        }
        // For a parameter or an indirect interface value, reject if a registered implementation
        // of this slot contains the deferred copy. Unknown implementor sets remain inconclusive.
        if (userReceiver != nullptr && !receiverResolved)
        {
            std::vector<std::string> implementors;
            if (EnumerateInterfaceImplementors(ifaceName, implementors, true))
                for (const auto& structName : implementors)
                {
                    const auto* callee = LookupInterfaceMethodImpl(structName, *methodInfo);
                    const size_t siteCount = thisClosureCopySites_.size();
                    NoteThisClosureMemberCall(callee);
                    if (thisClosureCopySites_.size() != siteCount) break;
                }
        }

        std::vector<TypeAndValue> transferParams = methodInfo->Parameters;
        for (auto& param : transferParams)
        {
            if (!InterfaceParamConsumesOwnership(param)) continue;
            param.IsOwningSink = true;
            param.IsInterfaceSink = true;
        }

        for (size_t i = 0; i < methodInfo->Parameters.size() && i < callArgNVs.size(); i++)
        {
            if (!callArgNVs[i].IsBonded) continue;
            if (!methodInfo->Parameters[i].IsMove
                && !InterfaceCallMayRetainBondedArg(ifaceName, methodName,
                                                     methodInfo->Parameters.size(), (unsigned)i))
                continue;
            LogErrorMessage(
                "cannot pass bonded value to interface method '{}.{}' parameter '{}' - bonded values cannot be transferred out of their source's scope",
                { SpellType(*this, TypeAndValue{ .TypeName = ifaceName }), methodName,
                  methodInfo->Parameters[i].VariableName });
        }

        // Method indices start at 1 (slot 0 is type ID)
        auto fnPtrField = builder->CreateGEP(ptrTy, vtablePtr, builder->getInt32(methodIdx + 1));
        auto fnPtr = builder->CreateLoad(ptrTy, fnPtrField);

        // Record the return type so the call site can populate the result's TypeAndValue
        // (needed for chaining a method on the result, e.g. `e.toJson().data()`).
        lastCallReturnType = methodInfo->ReturnType;

        lastCxxRetTemp_ = nullptr;
        lastCxxRetValue_ = nullptr;
        lastLoweredRetTemp_ = nullptr;
        lastLoweredRetValue_ = nullptr;
        const bool cxxSretReturn = !methodInfo->ReturnType.Pointer
            && !methodInfo->ReturnType.IsAlias
            && !methodInfo->ReturnType.IsArrayView
            && ReturnsViaInPlaceSret(methodInfo->ReturnType.TypeName);
        std::vector<TypeAndValue> interfaceRecipeParams;
        TypeAndValue receiver;
        receiver.TypeName = ifaceName;
        receiver.Pointer = true;
        interfaceRecipeParams.push_back(receiver);
        interfaceRecipeParams.insert(interfaceRecipeParams.end(), methodInfo->Parameters.begin(),
                                     methodInfo->Parameters.end());
        AbiRecipe cxxSretRecipe = ComputeCxxReturnAbiRecipe(methodInfo->ReturnType,
                                                            interfaceRecipeParams);
        llvm::Value* cxxSretDest = nullptr;
        if (cxxSretReturn)
        {
            auto* structTy = GetType(methodInfo->ReturnType);
            if (structTy == nullptr || !structTy->isStructTy())
                return nullptr;
            if (pendingCxxSretDest_ != nullptr
                && pendingCxxSretTypeName_ == methodInfo->ReturnType.TypeName)
            {
                cxxSretDest = pendingCxxSretDest_;
                pendingCxxSretDest_ = nullptr;
                pendingCxxSretTypeName_.clear();
            }
            else
            {
                const uint64_t align = module->getDataLayout().getABITypeAlign(structTy).value();
                cxxSretDest = AllocaAtEntry(structTy, nullptr, "cxx.interface.rettemp", align);
                // A lowered CFlat-only result keeps CFlat by-value ownership: not ledger-owned.
                if (IsLoweredCFlatOnlyStruct(methodInfo->ReturnType.TypeName))
                    lastLoweredRetTemp_ = cxxSretDest;
                else
                {
                    RegisterOwnedStructTemp(cxxSretDest, methodInfo->ReturnType.TypeName);
                    lastCxxRetTemp_ = cxxSretDest;
                }
            }
        }

        llvm::Type* retTy = cxxSretReturn ? builder->getVoidTy()
                                          : GetFunctionReturnABIType(methodInfo->ReturnType);
        std::vector<llvm::Type*> paramTypes;
        if (cxxSretReturn)
            paramTypes.push_back(cflat_llvm::PointerTo(GetType(methodInfo->ReturnType)));
        paramTypes.push_back(ptrTy);
        for (size_t i = 0; i < methodInfo->Parameters.size(); ++i)
        {
            const auto& p = methodInfo->Parameters[i];
            const size_t recipeIndex = i + 1;
            const bool byVal = recipeIndex < cxxSretRecipe.paramSlots.size()
                && cxxSretRecipe.paramSlots[recipeIndex].kind == AbiSlot::ByVal;
            // Match the definition's ByVal recipe and alias-borrow lowering.
            paramTypes.push_back(byVal
                ? cflat_llvm::PointerTo(GetType(p))
                : (ParameterIsAliasByPointer(p) ? cflat_llvm::PointerTo(GetType(p))
                                                : GetType(p)));
            if (ParameterCarriesRawArrayCount(p))
                paramTypes.push_back(builder->getInt64Ty());
        }
        if (ReturnCarriesRawArrayCount(methodInfo->ReturnType))
            paramTypes.push_back(cflat_llvm::PointerTo(builder->getInt64Ty()));
        auto fnTy = llvm::FunctionType::get(retTy, paramTypes, false);

        // callArgNVs is arity-matched to Parameters by the resolver, so no clamp here: a
        // mismatched arity was already rejected instead of silently truncating the call.
        std::vector<llvm::Value*> callArgs;
        std::vector<size_t> paramCallArgIndexes;
        if (cxxSretReturn)
            callArgs.push_back(cxxSretDest);
        callArgs.push_back(dataPtr);
        for (size_t i = 0; i < callArgNVs.size() && i < methodInfo->Parameters.size(); i++)
        {
            const auto& nv = callArgNVs[i];
            const auto& param = methodInfo->Parameters[i];
            const size_t firstCallArg = callArgs.size();

            const size_t recipeIndex = i + 1; // slot zero is the interface receiver
            if (recipeIndex < cxxSretRecipe.paramSlots.size()
                && cxxSretRecipe.paramSlots[recipeIndex].kind == AbiSlot::ByVal)
            {
                if (nv.Storage == nullptr)
                {
                    LogError(std::format(
                        "cannot pass C++ class '{}' through interface method '{}.{}': the argument must "
                        "have addressable storage so its copy or move constructor can run",
                        DisplayCxxClassName(param.TypeName), ifaceName, methodName));
                    continue;
                }
                const bool forwardedDefaultParam = nv.IsDefaultParamForward
                    && nv.Storage->getType()->isPointerTy()
                    && !nv.TypeAndValue.Pointer && !nv.TypeAndValue.IsAlias
                    && !nv.TypeAndValue.IsRvalueRef
                    && ResolveTypeAlias(nv.TypeAndValue.TypeName)
                        == ResolveTypeAlias(param.TypeName);
                const bool cxxObject = IsForeignNontrivialCxxClass(param.TypeName);
                if (cxxObject && forwardedDefaultParam)
                {
                    if (IsCxxParamDestroyedInCallee(param.TypeName))
                    {
                        if (auto* storageJoin = llvm::dyn_cast_or_null<llvm::PHINode>(nv.Storage))
                            for (unsigned arm = 0; arm < storageJoin->getNumIncomingValues(); ++arm)
                                UnregisterOwnedStructTemp(storageJoin->getIncomingValue(arm));
                        else
                            UnregisterOwnedStructTemp(nv.Storage);
                    }
                    callArgs.push_back(nv.Storage);
                    if (ParameterCarriesRawArrayCount(param))
                        callArgs.push_back(RawArrayCountArgument(nv));
                    paramCallArgIndexes.push_back(firstCallArg);
                    continue;
                }
                auto* temp = AllocaAtEntry(cxxSretRecipe.paramSlots[recipeIndex].structTy,
                                           nullptr, "cxx.interface.argtemp",
                                           cxxSretRecipe.paramSlots[recipeIndex].align);
                const bool cflatHolder = !cxxObject
                    && HasForeignNontrivialCxxField(param.TypeName);
                const bool interfaceHolderCopy = transferParams[i].IsInterfaceSink
                    && param.IsMove && cflatHolder && IsCopyableType(param.TypeName);
                const bool useMove = (param.IsMove && !interfaceHolderCopy) || nv.IsExplicitMove
                    || nv.CxxParamLastUse || nv.IsRvalue;
                if (!EmitCxxByValueParamConstruct(param.TypeName, temp, nv.Storage, useMove,
                                                  "into an interface by-value parameter",
                                                  CxxConstReceiverKind(nv) != 0))
                    continue;
                if (cxxObject && !IsCxxParamDestroyedInCallee(param.TypeName))
                    RegisterOwnedStructTemp(temp, param.TypeName);
                if (nv.CxxParamLastUse && !cflatHolder
                    && !nv.IsElementAccess && nv.FieldName.empty())
                {
                    const std::string sourceName = nv.CallerName.empty()
                        ? nv.TypeAndValue.VariableName : nv.CallerName;
                    MarkVariableMoved(sourceName);
                }
                callArgs.push_back(temp);
                if (ParameterCarriesRawArrayCount(param))
                    callArgs.push_back(RawArrayCountArgument(nv));
                paramCallArgIndexes.push_back(firstCallArg);
                continue;
            }

            // Same closure SHAPE gate the direct call path applies (RejectFuncPtrShapeMismatch):
            // a vtable slot lowers each argument by bit pattern, so a mismatch is called as code.
            if (!param.IsInterface && RejectFuncPtrShapeMismatch(nv, param))
                return nullptr;

            // Code value into a DATA parameter: the scorer's gate, which this path never runs.
            if (RejectCodeValueIntoDataParam(nv, param, ifaceName, methodName))
                return nullptr;

            // Array-view parameter gate, shared with the direct-call door: a vtable slot lowers
            // by the same ABI, so a forged view is called as one and cannot be lowered here.
            if (param.IsArrayView
                && RejectArrayViewParamBinding(nv, param, ifaceName + "." + methodName))
                return nullptr;

            if (IsImplicitPrimitiveToPointer(param, nv, nv.Primary))
            {
                LogError(DescribeImplicitPrimitiveToPointer(
                    param, nv, nv.Primary, "pass",
                    std::format("parameter '{}' of interface method '{}.{}'",
                                param.VariableName, ifaceName, methodName)));
                return nullptr;
            }

            // A blessed unique<IFace> wrapper is not an implementor: borrow the fat value it holds
            // through get(), the same lowering the direct call path applies.
            if (param.IsInterface && !param.IsArrayView && !nv.TypeAndValue.IsInterface
                && IsCoreUniqueToRawPointer(nv, param))
            {
                callArgs.push_back(CreateCoreUniqueRawPointerCall(nv, param));
            }
            // An 'IA[]' parameter is a THIN view over fat elements, never a fat value itself, so
            // nothing binds to it by boxing - the direct-call door excludes views the same way.
            else if (param.IsInterface && !param.IsArrayView && !nv.TypeAndValue.IsInterface)
            {
                // Concrete struct/pointer -> interface fat ptr upconversion.
                // Reject a pointer-shaped source: it is not an instance of its element class.
                std::string argShape = DescribePointerShapedInterfaceSource(nv.TypeAndValue);
                if (!argShape.empty())
                {
                    LogRawError(FormatPointerShapedInterfaceUpcastError(
                        argShape, SpellType(*this, nv.TypeAndValue), SpellType(*this, param)));
                    return nullptr;
                }
                std::string structName = nv.TypeAndValue.TypeName;
                if (structName.empty() && nv.BaseType)
                {
                    if (auto* st = llvm::dyn_cast<llvm::StructType>(nv.BaseType))
                        structName = st->getName().str();
                }
                auto vtable = GetOrCreateVTable(structName, param.TypeName);
                llvm::Value* argDataPtr = nullptr;
                if (nv.TypeAndValue.Pointer)
                {
                    argDataPtr = nv.Primary != nullptr ? nv.Primary : CreateLoad(nv.Storage);
                }
                else if (nv.Storage != nullptr)
                {
                    argDataPtr = nv.Storage;
                }
                else
                {
                    auto structTy = nv.BaseType ? nv.BaseType : GetType(nv.TypeAndValue);
                    auto tempAlloca = AllocaAtEntry(structTy, nullptr);
                    builder->CreateStore(nv.Primary, tempAlloca);
                    argDataPtr = tempAlloca;
                }
                callArgs.push_back(BuildInterfaceFatValue(vtable, argDataPtr));
            }
            else if (param.IsInterface && nv.TypeAndValue.IsInterface)
            {
                // Interface -> interface: pass the fat struct by value, re-boxing on an upcast.
                llvm::Value* val = nv.Primary ? nv.Primary : CreateLoad(nv.Storage);
                callArgs.push_back(ReboxInterfaceIfNeeded(val, nv.TypeAndValue.TypeName, param.TypeName));
            }
            else if (ParameterIsAliasByPointer(param))
            {
                // Same alias-borrow ABI as the direct call path (CreateOverloadedFunctionCall).
                callArgs.push_back(LowerAliasByPointerArg(nv, param));
            }
            else if (!param.Pointer && IsRawPointerToCoreUnique(nv, param))
            {
                // Interface vtable slots use the same by-value unique<T> ABI as direct calls;
                // construct the wrapper before passing the raw owning pointer to the slot.
                callArgs.push_back(CreateCoreUniqueFromRawPointerCall(nv, param));
            }
            else if (param.Pointer && IsCoreUniqueToRawPointer(nv, param))
            {
                // The mirror direction: a blessed unique<X> argument borrows to a raw X* slot
                // through get(), exactly as the direct call path lowers it.
                callArgs.push_back(CreateCoreUniqueRawPointerCall(nv, param));
            }
            else if (param.Pointer)
            {
                // Storage may be a promoted-param alloca holding the pointer; load to get the value.
                if (nv.Primary == nullptr && nv.Storage != nullptr && llvm::isa<llvm::AllocaInst>(nv.Storage))
                    callArgs.push_back(CreateLoad(nv.Storage));
                else
                    callArgs.push_back(nv.GetValue());
            }
            else
            {
                // Scalar or by-value struct (string / user value struct). Reuse the normal
                // call path's lowering so a string literal becomes a %string by value rather
                // than a bare ptr. See LowerByValueArg (canonical path: CreateOverloadedFunctionCall).
                auto val = nv.Primary != nullptr ? nv.Primary : CreateLoad(nv.Storage);
                val = LowerByValueArg(val, param, nv);
                callArgs.push_back(val);
            }
            if (ParameterCarriesRawArrayCount(param))
                callArgs.push_back(RawArrayCountArgument(nv));
            paramCallArgIndexes.push_back(firstCallArg);
        }

        // Restore the virtual method result side-channel after any nested core constructor call.
        lastCallReturnType = methodInfo->ReturnType;
        lastCallReturnsOwned = false;
        lastCallReturnsAllocAlign = 0;

        llvm::Value* rawReturnCountSlot = nullptr;
        if (ReturnCarriesRawArrayCount(methodInfo->ReturnType))
        {
            rawReturnCountSlot = CreateRawArrayReturnCountSlot();
            callArgs.push_back(rawReturnCountSlot);
        }

        moveTransferConsumedTemps_.clear();
        ApplyMoveParamTransfer(ifaceName + "." + methodName, transferParams, callArgNVs,
            true, false, true);
        // The implementation is a CFlat method, so it may throw through: invoke when this frame
        // owes cleanup. Its pad leaves the sink arguments to the callee.
        unwindCallConsumedTemps_ = std::move(moveTransferConsumedTemps_);
        moveTransferConsumedTemps_.clear();
        auto* callResult = CreateCallOrInvoke(fnTy, fnPtr, callArgs, /*mayUnwind=*/true);
        if (methodInfo->ReturnType.Pointer && !methodInfo->ReturnType.IsMove)
            callResult->setMetadata("cflat.borrowed-pointer-return", llvm::MDNode::get(*context, {}));
        unwindCallConsumedTemps_.clear();
        // Dispatch has no body to prove the callee borrows; keep retained join-arm temps.
        DropRetainedJoinArmPtrTemps(callResult);
        if (cxxSretRecipe.hasLowering)
            ApplyAbiCallAttributes(callResult, cxxSretRecipe);
        llvm::Value* resultValue = callResult;
        const auto& rt = methodInfo->ReturnType;
        if (cxxSretReturn)
        {
            resultValue = CreateLoad(cxxSretDest);
            if (IsLoweredCFlatOnlyStruct(methodInfo->ReturnType.TypeName))
            {
                lastLoweredRetValue_ = resultValue;
                nullConditionalTempResults_.push_back(resultValue);
            }
            else
                lastCxxRetValue_ = resultValue;
        }
        RegisterRawArrayCallResult(callResult, rawReturnCountSlot,
                                   methodInfo->ReturnType.AllocAlignValue);
        std::vector<llvm::Value*> aliasCallArgs;
        aliasCallArgs.reserve(methodInfo->Parameters.size());
        for (size_t i = 0; i < methodInfo->Parameters.size(); ++i)
        {
            const size_t argIndex = i < paramCallArgIndexes.size()
                ? paramCallArgIndexes[i] : callArgs.size();
            aliasCallArgs.push_back(argIndex < callArgs.size() ? callArgs[argIndex] : nullptr);
        }
        bool aliasStringTempMayOwn = TryAdoptAliasReturnStringTemp(resultValue, rt,
            rt.IsMove, rt.IsAlias, transferParams, aliasCallArgs, &callArgNVs);

        // An `adopt` parameter publishes an owned interface box into callee-managed storage.
        // The source remains readable, but its local teardown ownership is retired.
        for (size_t i = 0; i < methodInfo->Parameters.size() && i < callArgNVs.size(); i++)
        {
            if (!methodInfo->Parameters[i].IsAdopt) continue;
            const auto& arg = callArgNVs[i];
            bool ownedMoveInterface = arg.TypeAndValue.IsFatInterfaceValue()
                && arg.TypeAndValue.IsMove && arg.Storage == nullptr && arg.FieldName.empty();
            bool ownedInterfaceLocal = arg.OwnsInterfaceBox
                || arg.IsAdoptable
                || (arg.TypeAndValue.IsFatInterfaceValue() && arg.IsOwning && arg.TypeAndValue.IsMove);
            if (arg.TypeAndValue.IsUnique
                || (!ownedInterfaceLocal && !ownedMoveInterface)
                || (!arg.CallerName.empty() && !arg.FieldName.empty()))
                LogErrorMessage(
                    "call to '{}.{}': adopt parameter '{}' requires an owned interface local or "
                    "a move-returning interface result; "
                    "a unique local or borrowed interface cannot be adopted",
                    { SpellType(*this, TypeAndValue{ .TypeName = ifaceName }), methodName,
                      methodInfo->Parameters[i].VariableName });
            if (!arg.CallerName.empty())
            {
                SetVariableOwning(arg.CallerName, false);
                SetVariableOwnsInterfaceBox(arg.CallerName, false);
            }
        }

        // A `move` parameter on an INTERFACE method transfers ownership just as it does on a
        // direct call, so the caller's source must be nulled/marked-moved here too. Without this
        // the source keeps its owning flag and scope exit frees what the callee now owns.
        for (size_t i = 0; i < methodInfo->Parameters.size() && i < callArgNVs.size(); i++)
            DiagnoseExplicitMoveToBorrowParam(SpellType(*this, TypeAndValue{ .TypeName = ifaceName })
                + "." + methodName,
                transferParams[i], callArgNVs[i]);
        ApplyMoveParamTransfer(ifaceName + "." + methodName, transferParams, callArgNVs,
            true, false, false);

        // A temp's `unique` field handed to a PLAIN `T*` parameter of a VIRTUAL slot. Same point
        // in the sequence as the direct path's RecordTempUniqueFieldArgs, for the same reason.
        RecordTempUniqueFieldInterfaceArgs(resultValue, ifaceName, *methodInfo, callArgNVs);

        // Classify the virtual result's ownership exactly like the direct-call path
        // (CreateOverloadedFunctionCall): a 'move string' / 'move T*' / 'move <interface>'
        // return hands an owned value to the caller. Set the side-channel so a binding site
        // (decl-init / assignment / move-param / return) re-homes it and clears the flag; an
        // inline-used owned STRING result is registered for end-of-full-expression cleanup
        // (mirrors the direct path's RegisterOwnedStringTemp) so e.g. `tree.toJson().data()`
        // does not leak. Without this the virtual path left the result classified as a borrow.
        // Same argument-list retirement as the direct-call path: the result is a different value
        // than anything a `new`/`move` in the argument list produced.
        lastOwningResult = false;
        lastAllocAlignment = 0;
        // A core unique return owns like `move` (see the direct-call path).
        lastCallReturnsOwned = (rt.IsMove
            || aliasStringTempMayOwn
            || (!rt.IsAlias && IsCoreUniqueType(rt.TypeName)))
            && (rt.TypeName == "string" || rt.Pointer || rt.IsInterface
                || IsCoreUniqueType(rt.TypeName));
        if (lastCallReturnsOwned)
            RegisterOwnedReturnTemp(resultValue, ifaceName + "." + methodName, rt,
                                    !aliasStringTempMayOwn);
        lastCallReturnsAllocAlign = rt.AllocAlignValue;
        if (lastCallReturnsOwned
            && resultValue->getType() == llvm::StructType::getTypeByName(*context, "string"))
            RegisterOwnedStringTemp(resultValue);

        return resultValue;
    }

LLVMBackend::NamedVariable LLVMBackend::MakeStringLiteralNV(const std::string& text)
{
        NamedVariable nv;
        auto* rawPtr = CreateGlobalString("reflect_str", text);
        nv.Primary = WrapStringLiteralAsString(rawPtr);
        nv.TypeAndValue.TypeName = "string";
        return nv;
    }

void LLVMBackend::RegisterDestructor(const std::string& structName, llvm::Function* fn)
{
        dataStructures[structName].Destructor = fn;
    }

bool LLVMBackend::TypeHasDestructor(const std::string& structName) const
{
        auto it = dataStructures.find(structName);
        return it != dataStructures.end() && it->second.Destructor != nullptr;
    }

llvm::Value* LLVMBackend::GetTypeSizeBytes(llvm::Type* type)
{
        if (!type)
        {
            LogErrorMessage("GetTypeSizeBytes: null type pointer");
            return nullptr;
        }
        const llvm::DataLayout& dl = module->getDataLayout();
        uint64_t size = dl.getTypeAllocSize(type);
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), size);
    }

llvm::Value* LLVMBackend::GetTypeAlignBytes(llvm::Type* type)
{
        if (!type)
        {
            LogErrorMessage("GetTypeAlignBytes: null type pointer");
            return nullptr;
        }
        const llvm::DataLayout& dl = module->getDataLayout();
        uint64_t align = dl.getABITypeAlign(type).value();
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), align);
    }

uint64_t LLVMBackend::GetEffectiveAlignment(const DeclTypeAndValue& decl, llvm::Type* type)
{
        uint64_t a = 1;
        if (type && type->isSized())
            a = module->getDataLayout().getABITypeAlign(type).value();
        if (decl.UserAlignValue > a) a = decl.UserAlignValue;
        auto it = dataStructures.find(decl.TypeName);
        if (it != dataStructures.end() && it->second.UserRequestedAlignment > a)
            a = it->second.UserRequestedAlignment;
        return a;
    }

uint64_t LLVMBackend::GetEffectiveAlignmentForType(const std::string& typeName, llvm::Type* type)
{
        uint64_t a = 1;
        if (type && type->isSized())
            a = module->getDataLayout().getABITypeAlign(type).value();
        auto it = dataStructures.find(typeName);
        if (it != dataStructures.end() && it->second.UserRequestedAlignment > a)
            a = it->second.UserRequestedAlignment;
        return a;
    }

uint64_t LLVMBackend::GetEffectiveAllocSize(llvm::Type* type, uint64_t effAlign)
{
        if (!type || !type->isSized()) return 0;
        uint64_t sz = module->getDataLayout().getTypeAllocSize(type);
        if (effAlign <= 1) return sz;
        return (sz + effAlign - 1) / effAlign * effAlign;
    }

void LLVMBackend::RegisterStructInterfaces(const std::string& structName, const std::vector<std::string>& interfaces)
{
        dataStructures[structName].Interfaces = interfaces;
    }

std::vector<std::string> LLVMBackend::GetStructInterfaces(const std::string& structName) const
{
        auto it = dataStructures.find(structName);
        if (it == dataStructures.end()) return {};
        return it->second.Interfaces;
    }

void LLVMBackend::RegisterStructStaticInterfaces(const std::string& structName, const std::vector<std::string>& interfaces)
{
        dataStructures[structName].StaticInterfaces = interfaces;
    }

std::vector<std::string> LLVMBackend::GetStructStaticInterfaces(const std::string& structName) const
{
        auto it = dataStructures.find(structName);
        if (it == dataStructures.end()) return {};
        return it->second.StaticInterfaces;
    }

bool LLVMBackend::TypeHasCapability(const std::string& typeName, const std::string& ifaceName) const
{
        auto it = dataStructures.find(typeName);
        if (it == dataStructures.end()) return false;
        auto declares = [&](const std::vector<std::string>& list)
        {
            for (const auto& i : list)
                if (i == ifaceName || InterfaceInheritsFrom(i, ifaceName)) return true;
            return false;
        };
        return declares(it->second.StaticInterfaces) || declares(it->second.Interfaces);
    }

bool LLVMBackend::TypeImplementsInterface(const std::string& typeName, const std::string& ifaceName) const
{
        // An interface trivially satisfies a constraint to itself (e.g. arena_channel<IMessage>
        // uses IMessage as its payload type when constrained to IMessage).
        if (typeName == ifaceName && HasInterface(typeName)) return true;

        auto it = dataStructures.find(typeName);
        if (it == dataStructures.end()) return false;
        auto has = [&](const std::vector<std::string>& list)
        {
            return std::find(list.begin(), list.end(), ifaceName) != list.end();
        };
        // Static ([Capability]) conformance satisfies a `where T : I` constraint too; only the
        // fat-pointer CONVERSION sites are restricted to the nominal Interfaces list.
        return has(it->second.Interfaces) || has(it->second.StaticInterfaces);
    }

void LLVMBackend::VerifyInterfaceImplementation(const std::string& structName, const std::string& interfaceName)
{
        const auto* ifaceMethods = FindInterface(interfaceName);
        if (ifaceMethods == nullptr)
        {
            TypeAndValue displayInterface;
            displayInterface.TypeName = interfaceName;
            LogErrorMessage("unknown interface{} '{}'", { ":", SpellType(*this, displayInterface) });
            return;
        }

        for (const auto& method : *ifaceMethods)
        {
            bool found = false;
            // A class may declare several overloads of the contract method that share the interface
            // method's name/type/arity but differ in ownership qualifiers (e.g. a `move` and a plain
            // 'set'). The class conforms if ANY of them satisfies the contract, so remember the first
            // signature-matching candidate to blame only if none conform.
            const FunctionSymbol* firstCandidate = nullptr;
            const FunctionSymbol* conforming = nullptr;
            bool anyConforms = false;
            auto funcIt = functionTable.find(method.Name);
            if (funcIt != functionTable.end())
            {
                for (const auto& sym : funcIt->second)
                {
                    // Expect: first param = structName*, remaining params match the interface method's params
                    if (sym.Parameters.size() != method.Parameters.size() + 1)
                        continue;
                    if (sym.Parameters[0].TypeName != structName || !sym.Parameters[0].Pointer)
                        continue;

                    bool paramsMatch = true;
                    for (int i = 0; i < (int)method.Parameters.size(); i++)
                    {
                        if (sym.Parameters[i + 1].TypeName != method.Parameters[i].TypeName ||
                            sym.Parameters[i + 1].ValuePointerDepth()
                                != method.Parameters[i].ValuePointerDepth())
                        {
                            paramsMatch = false;
                            break;
                        }
                    }

                    // Selection matches on type alone; the ownership qualifiers are contract and are
                    // validated across all matching overloads below, accepting if any conforms.
                    if (paramsMatch)
                    {
                        found = true;
                        if (!firstCandidate) firstCandidate = &sym;
                        if (InterfaceMethodContractConforms(method, sym))
                        {
                            anyConforms = true;
                            conforming = &sym;
                            break;
                        }
                    }
                }
            }

            if (!found)
            {
                LogErrorMessage("class '{}' does not implement '{}::{}'",
                    { SpellType(*this, TypeAndValue{ .TypeName = structName }),
                      SpellType(*this, TypeAndValue{ .TypeName = interfaceName }), method.Name });
            }
            else if (!anyConforms)
            {
                // No overload agrees with the contract; report the ownership mismatch against the
                // first signature-matching candidate so the diagnostic names a real overload.
                VerifyInterfaceMethodContract(structName, interfaceName, method, *firstCandidate);
            }
            // #10: the implementer of a const interface method must be read-only.
            if (found && method.IsConst)
                RecordConstInterfaceImplementer(conforming ? *conforming : *firstCandidate,
                                                structName, interfaceName, method.Name);
        }

        auto sdIt = dataStructures.find(structName);
        if (sdIt != dataStructures.end())
            VerifyInterfaceFields(structName, interfaceName, sdIt->second.StructFields);
    }

llvm::Function* LLVMBackend::SynthesizeReflectFunction(const std::string& structName)
{
        auto compiler = this;
        TypeAndValue displayStruct;
        displayStruct.TypeName = structName;

        // Guard: if already synthesized, return existing function
        if (compiler->synthesizedReflectFunctions.count(structName))
        {
            if (auto fn = compiler->module->getFunction("__reflect_" + structName))
                return fn;
        }

        // Insert into set before emitting body (handles A->B->A cycles)
        compiler->synthesizedReflectFunctions.insert(structName);

        // Lookup struct
        auto it = compiler->dataStructures.find(structName);
        if (it == compiler->dataStructures.end())
        {
            LogErrorMessage("reflect: unknown struct '{}'", { SpellType(*compiler, displayStruct) });
            return nullptr;
        }
        auto& sd = it->second;
        if (!sd.StructType)
        {
            LogErrorMessage("reflect: struct '{}' has no LLVM type", { SpellType(*compiler, displayStruct) });
            return nullptr;
        }
        if (sd.IsUnion)
        {
            LogErrorMessage("reflect is not supported on union type '{}'", { SpellType(*compiler, displayStruct) });
            return nullptr;
        }

        // Save builder state (includes currentFunction, currentSubprogram); the guard restores
        // on every exit, including a thrown LogError.
        BuilderStateGuard savedState(compiler);

        // Build parameter types
        TypeAndValue objParam;
        objParam.TypeName = structName;
        objParam.Pointer = true;
        objParam.VariableName = "obj";

        TypeAndValue visitorParam;
        visitorParam.TypeName = "IReflector";
        visitorParam.IsInterface = true;
        visitorParam.IsInterfacePointer = false;
        visitorParam.Pointer = true;
        visitorParam.VariableName = "visitor";

        TypeAndValue voidReturn;
        voidReturn.TypeName = "void";

        // Create function definition
        std::string fnName = "__reflect_" + structName;
        auto* fn = compiler->CreateFunctionDefinition(fnName, voidReturn, {objParam, visitorParam});
        if (!fn)
        {
            LogErrorMessage("reflect: failed to create function for '{}'", { SpellType(*compiler, displayStruct) });
            return nullptr;
        }
        fn->setLinkage(llvm::Function::InternalLinkage);

        // Retrieve parameters
        // obj is a pointer argument
        llvm::Value* objPtr = fn->getArg(0);

        // visitor fat-ptr alloca (created by createFunctionBlock)
        llvm::Value* visitorAlloca = nullptr;
        if (!compiler->stackNamedVariable.empty())
        {
            auto& topFrame = compiler->stackNamedVariable.back();
            auto frameIt = topFrame.functionArgument.find("visitor");
            if (frameIt != topFrame.functionArgument.end())
                visitorAlloca = frameIt->second.Storage;
        }
        if (!visitorAlloca)
        {
            LogErrorMessage("reflect: failed to retrieve visitor alloca");
            return fn;
        }

        // Emit per-field code
        for (size_t i = 0; i < sd.StructFields.size(); i++)
        {
            const auto& field = sd.StructFields[i];

            // Synthetic bitfield storage slots (`__bfN`) are not user-visible;
            // the named bitfields they pack are emitted from sd.Bitfields below.
            // Synthetic `__padN` alignment slots are not user-visible either.
            if (field.IsBitfieldStorage || field.IsPadding) continue;

            // Skip [Private] fields
            bool isPrivate = false;
            for (const auto& ann : field.Annotations)
            {
                if (ann.Name == "Private")
                {
                    isPrivate = true;
                    break;
                }
            }
            if (isPrivate) continue;

            std::string displayName = field.VariableName;

            // GEP to field
            auto* gep = compiler->CreateCxxFieldGEP(sd, objPtr, (unsigned)i,
                field.VariableName + "_ptr");

            // Dispatch on field type
            std::string typeName = field.TypeName;
            bool isPtr = field.Pointer;
            bool isInterface = field.IsInterface;

            // Integer types
            if (typeName == "int" || typeName == "i8" || typeName == "i16" || typeName == "i32"
                || typeName == "i64" || typeName == "u8" || typeName == "u16" || typeName == "u32"
                || typeName == "u64")
            {
                if (!isPtr && !isInterface)
                {
                    auto* raw = compiler->builder->CreateLoad(compiler->GetType(field), gep);
                    auto* widened = compiler->Upconvert(raw, compiler->builder->getInt64Ty(),
                        field.IsUnsignedInteger() != -1);
                    auto nameNV = compiler->MakeStringLiteralNV(displayName);
                    NamedVariable intNV;
                    intNV.Primary = widened;
                    intNV.TypeAndValue.TypeName = "i64";
                    compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitInt",
                        {nameNV, intNV});
                }
            }
            // Bool type
            else if (typeName == "bool" && !isPtr && !isInterface)
            {
                auto* raw = compiler->builder->CreateLoad(compiler->GetType(field), gep);
                auto nameNV = compiler->MakeStringLiteralNV(displayName);
                NamedVariable boolNV;
                boolNV.Primary = raw;
                boolNV.TypeAndValue.TypeName = "bool";
                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitBool",
                    {nameNV, boolNV});
            }
            // Float / double types
            else if ((typeName == "float" || typeName == "double" || typeName == "longdouble") && !isPtr && !isInterface)
            {
                llvm::Value* raw = compiler->builder->CreateLoad(compiler->GetType(field), gep);
                // visitFloat takes double, so widen a 'float' field instead of narrowing.
                if (typeName == "float")
                    raw = compiler->builder->CreateFPCast(raw, compiler->builder->getDoubleTy());
                auto nameNV = compiler->MakeStringLiteralNV(displayName);
                NamedVariable floatNV;
                floatNV.Primary = raw;
                floatNV.TypeAndValue.TypeName = "double";
                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitFloat",
                    {nameNV, floatNV});
            }
            // String type
            else if (typeName == "string" && !isPtr && !isInterface)
            {
                auto nameNV = compiler->MakeStringLiteralNV(displayName);
                NamedVariable strNV;
                strNV.Storage = gep;
                strNV.TypeAndValue.TypeName = "string";
                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitString",
                    {nameNV, strNV});
            }
            // Struct value type (nested struct)
            else if (!isPtr && !isInterface && compiler->dataStructures.count(typeName))
            {
                compiler->SynthesizeReflectFunction(typeName);
                auto nameNV = compiler->MakeStringLiteralNV(displayName);
                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "beginObject",
                    {nameNV});

                // Call __reflect_FieldType(gep, visitor)
                auto* nestedFn = compiler->module->getFunction("__reflect_" + typeName);
                if (nestedFn)
                {
                    compiler->builder->CreateCall(nestedFn->getFunctionType(), nestedFn, {gep, visitorAlloca});
                }

                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "endObject", {});
            }
            // Struct pointer (nested struct reference)
            else if (isPtr && !isInterface && compiler->dataStructures.count(typeName))
            {
                compiler->SynthesizeReflectFunction(typeName);
                auto* ptrVal = compiler->builder->CreateLoad(compiler->GetType(field), gep);
                auto* ptrType = llvm::cast<llvm::PointerType>(ptrVal->getType());
                auto* isNull = compiler->builder->CreateICmpEQ(ptrVal,
                    llvm::ConstantPointerNull::get(ptrType));

                auto* thenBB = compiler->CreateBasicBlock("reflect_null", fn);
                auto* elseBB = compiler->CreateBasicBlock("reflect_obj", fn);
                auto* mergeBB = compiler->CreateBasicBlock("reflect_merge", fn);
                compiler->builder->CreateCondBr(isNull, thenBB, elseBB);

                // null branch: visitNull
                compiler->builder->SetInsertPoint(thenBB);
                auto nameNV = compiler->MakeStringLiteralNV(displayName);
                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitNull", {nameNV});
                compiler->builder->CreateBr(mergeBB);

                // non-null branch: beginObject + recurse + endObject
                compiler->builder->SetInsertPoint(elseBB);
                nameNV = compiler->MakeStringLiteralNV(displayName);
                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "beginObject", {nameNV});
                auto* nestedFn2 = compiler->module->getFunction("__reflect_" + typeName);
                if (nestedFn2)
                {
                    auto* visitorVal = compiler->builder->CreateLoad(compiler->GetFatPtrType(),
                        visitorAlloca);
                    compiler->builder->CreateCall(nestedFn2->getFunctionType(), nestedFn2,
                        {ptrVal, visitorVal});
                }
                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "endObject", {});
                compiler->builder->CreateBr(mergeBB);

                compiler->builder->SetInsertPoint(mergeBB);
            }
            // list<T>*, dictionary<K,V>*, void*, function, interface => skip (Phase 2+)
        }

        // Emit named bitfields from the side-table. These are not in StructFields
        // (only their __bfN storage slots are), so they need their own pass.
        for (const auto& bf : sd.Bitfields)
        {
            // Skip [Private] bitfields
            bool isPrivate = false;
            for (const auto& ann : bf.Annotations)
            {
                if (ann.Name == "Private") { isPrivate = true; break; }
            }
            if (isPrivate) continue;

            const auto& storageField = sd.StructFields[bf.StorageFieldIndex];
            auto* storageTy = compiler->GetType(storageField);
            auto* storagePtr = compiler->builder->CreateStructGEP(sd.StructType, objPtr,
                bf.StorageFieldIndex, bf.Name + "_bf_ptr");
            auto* word = compiler->LoadBitfieldWord(storagePtr, storageTy);

            unsigned w = bf.BitWidth;
            unsigned off = bf.BitOffset;
            unsigned storageBits = (unsigned)word->getType()->getIntegerBitWidth();

            llvm::Value* extracted;
            if (bf.IsUnsigned || bf.TypeName == "bool")
            {
                auto* shr = compiler->builder->CreateLShr(word,
                    llvm::ConstantInt::get(word->getType(), off));
                uint64_t mask = (w == 64) ? ~uint64_t(0) : ((uint64_t(1) << w) - 1);
                extracted = compiler->builder->CreateAnd(shr,
                    llvm::ConstantInt::get(word->getType(), mask));
            }
            else
            {
                unsigned leftShift = storageBits - w - off;
                auto* shl = compiler->builder->CreateShl(word,
                    llvm::ConstantInt::get(word->getType(), leftShift));
                extracted = compiler->builder->CreateAShr(shl,
                    llvm::ConstantInt::get(word->getType(), storageBits - w));
            }

            auto nameNV = compiler->MakeStringLiteralNV(bf.Name);

            if (bf.TypeName == "bool")
            {
                // Truncate to i1 for visitBool.
                auto* asBool = compiler->builder->CreateICmpNE(extracted,
                    llvm::ConstantInt::get(extracted->getType(), 0));
                NamedVariable boolNV;
                boolNV.Primary = asBool;
                boolNV.TypeAndValue.TypeName = "bool";
                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitBool",
                    {nameNV, boolNV});
            }
            else
            {
                auto* widened = compiler->Upconvert(extracted,
                    compiler->builder->getInt64Ty(), bf.IsUnsigned);
                NamedVariable intNV;
                intNV.Primary = widened;
                intNV.TypeAndValue.TypeName = "i64";
                compiler->CallInterfaceMethod(visitorAlloca, "IReflector", "visitInt",
                    {nameNV, intNV});
            }
        }

        // Emit return
        compiler->builder->CreateRetVoid();

        // Pop stack frame
        if (!compiler->stackNamedVariable.empty())
            compiler->stackNamedVariable.pop_back();

        return fn;
    }
