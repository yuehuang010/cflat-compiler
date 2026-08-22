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
        return typeName.rfind("HResult__", 0) == 0;
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
            LogErrorMessage("EmitWinrtSlotCall: bad slot '{}' on '{}'", { slotName, className });
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
                    "(import \"com.cb\")", { className, slotName, im->ReturnType.TypeName });
                return nullptr;
            }
            hresultTypeName = rt->second;
            auto* retElemTy = GetType(im->ReturnType);
            retvalAlloca = AllocaAtEntry(retElemTy, nullptr, "winrt.retval");
        }

        std::vector<llvm::Value*> callArgs = argVals;
        if (nonVoidIface)
            callArgs.push_back(builder->CreateBitCast(retvalAlloca, builder->getInt8Ty()->getPointerTo()));

        auto* objPtr = builder->CreateBitCast(argVals[0], objTy->getPointerTo());
        auto* vtblFieldPtr = builder->CreateStructGEP(objTy, objPtr, 0);
        auto* vtblPtr = builder->CreateLoad(vtblTy->getPointerTo(), vtblFieldPtr);
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
        auto* i8PtrTy = builder->getInt8Ty()->getPointerTo();
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
                auto* i64PtrTy = i64Ty->getPointerTo();
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
            auto* objP = b.CreateBitCast(self, objTy->getPointerTo());
            auto* rcP = b.CreateStructGEP(objTy, objP, 1);
            b.CreateAtomicRMW(llvm::AtomicRMWInst::Add, rcP, b.getInt32(1),
                llvm::MaybeAlign(), llvm::AtomicOrdering::Monotonic);
            b.CreateStore(self, b.CreateBitCast(ppv, i8PtrTy->getPointerTo()));
            b.CreateRet(b.getInt32(0));

            b.SetInsertPoint(noBB);
            b.CreateStore(llvm::ConstantPointerNull::get(i8PtrTy),
                b.CreateBitCast(ppv, i8PtrTy->getPointerTo()));
            b.CreateRet(b.getInt32((uint32_t)0x80004002));  // E_NOINTERFACE
        }

        // AddRef(i8* self) -> i32
        auto* refTy = llvm::FunctionType::get(i32Ty, { i8PtrTy }, false);
        auto* addRefFn = makeFn("AddRef", refTy);
        {
            auto* entry = llvm::BasicBlock::Create(*context, "entry", addRefFn);
            llvm::IRBuilder<> b(entry);
            auto* objP = b.CreateBitCast(addRefFn->getArg(0), objTy->getPointerTo());
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
            auto* objP = b.CreateBitCast(self, objTy->getPointerTo());
            auto* rcP = b.CreateStructGEP(objTy, objP, 1);
            auto* old = b.CreateAtomicRMW(llvm::AtomicRMWInst::Sub, rcP, b.getInt32(1),
                llvm::MaybeAlign(), llvm::AtomicOrdering::Monotonic);
            auto* nw = b.CreateSub(old, b.getInt32(1));
            b.CreateCondBr(b.CreateICmpEQ(nw, b.getInt32(0)), freeBB, doneBB);

            b.SetInsertPoint(freeBB);
            if (auto* dtor = GetOrCreateFullDestructor(className))
                b.CreateCall(dtor->getFunctionType(), dtor,
                    { b.CreateBitCast(self, objTy->getPointerTo()) });
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
                        { className, ifaceName, m.Name });
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
                        "which the WinRT thunk does not support yet", { className, m.Name });
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
                    auto* retPtr = b.CreateBitCast(fn->getArg(static_cast<unsigned>(fn->arg_size() - 1)), val->getType()->getPointerTo());
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
            s->Register(SymbolKind::Struct, thinName, fileForLsp, 0, 0, lspDesc);
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
                s->Register(SymbolKind::Function, thinName + "." + m.name, fileForLsp, 0, 0, sig);
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
            LogErrorMessage("EmitWinrtThinSlotCall: '{}' is not a built WinRT interface", { thinName });
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
            LogErrorMessage("EmitWinrtThinSlotCall: no slot '{}' on '{}'", { slotName, thinName });
            return nullptr;
        }

        auto* i8PtrTy = builder->getInt8Ty()->getPointerTo();
        auto* typedObj = builder->CreateBitCast(objPtr, objTy->getPointerTo());
        auto* vtblFieldPtr = builder->CreateStructGEP(objTy, typedObj, 0);
        auto* vtblPtr = builder->CreateLoad(vtblTy->getPointerTo(), vtblFieldPtr);
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
                { base, std::to_string(tpl.genericParams.size()), std::to_string(cflatArgs.size()) });
            return true;   // handled (it IS a winmd generic), just mis-arity
        }

        std::vector<cflat_winmd::TypeRef> argRefs;
        for (const auto& a : cflatArgs)
        {
            cflat_winmd::TypeRef r;
            std::string err;
            if (!CFlatArgToWinrtTypeRef(a, r, err)) { LogRawError(base + "<...>: " + err); return true; }
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
            LogRawError("PIID derivation for '" + mangledName + "': " + err);
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
        auto* i8PtrTy = builder->getInt8Ty()->getPointerTo();
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
        auto* i8PtrTy = builder->getInt8Ty()->getPointerTo();
        auto* i32Ty = builder->getInt32Ty();
        auto* closureTy = GetClosureFatPtrType();

        // vtbl = 4 opaque code pointers (WinRT reinterprets it as the delegate C vtable).
        auto* vtblTy = llvm::StructType::create(*context,
            { i8PtrTy, i8PtrTy, i8PtrTy, i8PtrTy }, "winrtdel_" + mangled + "_vtbl");
        auto* objTy = llvm::StructType::create(*context,
            { vtblTy->getPointerTo(), i32Ty, closureTy }, "winrtdel_" + mangled);

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
                auto* i64PtrTy = i64Ty->getPointerTo();
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
            auto* objP = b.CreateBitCast(self, objTy->getPointerTo());
            auto* rcP = b.CreateStructGEP(objTy, objP, 1);
            b.CreateAtomicRMW(llvm::AtomicRMWInst::Add, rcP, b.getInt32(1),
                llvm::MaybeAlign(), llvm::AtomicOrdering::Monotonic);
            b.CreateStore(self, b.CreateBitCast(ppv, i8PtrTy->getPointerTo()));
            b.CreateRet(b.getInt32(0));

            b.SetInsertPoint(noBB);
            b.CreateStore(llvm::ConstantPointerNull::get(i8PtrTy),
                b.CreateBitCast(ppv, i8PtrTy->getPointerTo()));
            b.CreateRet(b.getInt32((uint32_t)0x80004002));   // E_NOINTERFACE
        }

        // AddRef(i8* self) -> i32
        auto* addRefFn = makeFn("AddRef", llvm::FunctionType::get(i32Ty, { i8PtrTy }, false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(*context, "entry", addRefFn));
            auto* objP = b.CreateBitCast(addRefFn->getArg(0), objTy->getPointerTo());
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
            auto* objP = b.CreateBitCast(self, objTy->getPointerTo());
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
            auto* objP = b.CreateBitCast(invokeFn->getArg(0), objTy->getPointerTo());
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
            auto* cloFn = b.CreateBitCast(code, cloFnTy->getPointerTo(), "clo_fn");
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
                     "(import the .winmd that declares it)", { base });
            return nullptr;
        }
        if (cflatArgs.size() != tpl->genericParams.size())
        {
            LogErrorMessage("winrtDelegate: '{}' expects {} type argument(s), got {}",
                { base, std::to_string(tpl->genericParams.size()), std::to_string(cflatArgs.size()) });
            return nullptr;
        }
        if (!(tpl->invoke.returnType.fullName == "Void" && tpl->invoke.returnType.pointerDepth == 0))
        {
            LogErrorMessage("winrtDelegate: '{}' has a non-void Invoke return, which the "
                     "projection does not support yet (only void-returning handlers)", { base });
            return nullptr;
        }

        std::string mangled = base;
        for (const auto& a : cflatArgs) mangled += "__" + a;

        // Substitute the generic Invoke signature with the concrete type arguments.
        std::vector<cflat_winmd::TypeRef> argRefs;
        for (const auto& a : cflatArgs)
        {
            cflat_winmd::TypeRef r; std::string err;
            if (!CFlatArgToWinrtTypeRef(a, r, err)) { LogRawError("winrtDelegate " + base + "<...>: " + err); return nullptr; }
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
                LogErrorMessage("winrtDelegate: delegate '{}' has no IID in metadata", { base });
                return nullptr;
            }
        }
        else
        {
            cflat_winmd::TypeRef inst; inst.fullName = tpl->fullName; inst.genericArgs = argRefs;
            std::string err;
            if (!cflat_winmd::DerivePiid(inst, winrtConsumedModel_, iidBytes, err))
            {
                LogRawError("winrtDelegate PIID for '" + mangled + "': " + err);
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
        auto* i8PtrTy = builder->getInt8Ty()->getPointerTo();
        auto* closureTy = GetClosureFatPtrType();

        // Allocate the object.
        uint64_t sz = module->getDataLayout().getTypeAllocSize(objTy);
        NamedVariable szArg;
        szArg.Primary = builder->getInt64(sz);
        szArg.BaseType = builder->getInt64Ty();
        auto* raw = CreateOverloadedFunctionCall("operator new", { szArg });
        auto* objP = builder->CreateBitCast(raw, objTy->getPointerTo(), "winrtdel.obj");

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
                    s->Register(SymbolKind::Variable, name, fileForLsp, 0, 0, tv.TypeName + " " + name);
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
                s->Register(SymbolKind::Struct, st.fullName, fileForLsp, 0, 0, "struct " + st.fullName);
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
            std::string mangled = c.base;
            for (const auto& a : c.args) mangled += "__" + a;
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

        auto ptrTy = builder->getInt8Ty()->getPointerTo();
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
        auto ptrTy = builder->getInt8Ty()->getPointerTo();

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

llvm::Function* LLVMBackend::GetOrDeclareStrlen()
{
        if (auto* fn = module->getFunction("strlen"))
            return fn;
        auto* i64Ty = builder->getInt64Ty();
        auto* ptrTy = builder->getInt8Ty()->getPointerTo();
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
        auto* heapPtr = builder->CreateBitCast(rawPtr, builder->getInt8Ty()->getPointerTo(), "cpybuf");
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
            { functionName, paramName,
              "move", arg.CallerName.empty() ? arg.TypeAndValue.TypeName : arg.CallerName, "move", "move", paramType });
    }

void LLVMBackend::DiagnoseExplicitMoveToBorrowParam(const std::string& functionName,
        const TypeAndValue& param, const NamedVariable& arg)
{
        // A `unique`-typed parameter is a sink even without the `move` keyword: the type
        // itself says the callee takes ownership, so an explicit `move` at the call site is fine.
        // An inferred owning-value sink (body unconditionally moves the param, owning concrete
        // type) is likewise a real sink, so `move` into it transfers rather than "nothing".
        bool paramIsSink = param.IsMove || param.IsUniqueTypeArg
            || (OwningSinkConsumesConcrete(param) && IsOwningValueOrClosureType(param.TypeName));
        DiagnoseExplicitMoveToBorrowParam(
            functionName, param.VariableName, param.TypeName, paramIsSink, arg);
    }

void LLVMBackend::RejectOwningTempUniqueFieldIntoSinkParam(const std::string& functionName,
        const TypeAndValue& param, const NamedVariable& arg)
{
        bool paramClaimsOwnership = param.Pointer && !param.IsAlias
            && (param.IsMove || param.IsUnique || param.IsUniqueTypeArg);
        if (!paramClaimsOwnership) return;
        if (!JoinCarriesOwningTempUniqueField(arg.Primary))
        {
            // Callee still below its call site: defer this destination like the others.
            RecordDeferredTempUniqueFieldSinkEscape(arg.Primary, functionName, param.VariableName);
            return;
        }
        LogRawError(DescribeTempUniqueFieldSinkEscape(functionName, param.VariableName));
    }

LLVMBackend::TypeAndValue LLVMBackend::FuncPtrParamAsTypeAndValue(const TypeAndValue::FuncPtrParam& p,
        size_t index)
{
        TypeAndValue tv;
        tv.TypeName = p.TypeName;
        tv.Pointer  = p.Pointer;
        tv.PointerDepth = p.PointerDepth;
        tv.AllocAlignValue = p.AllocAlignValue;
        tv.IsOwningSink = p.IsOwningSink;
        tv.IsConsumeInferredSink = p.IsConsumeInferredSink;
        // A param whose funcptr TYPE spells `move` takes the DECLARED move path, exactly as a
        // direct call does. An INFERRED sink leaves IsMove false - see ApplyFuncPtrSinkTransfer.
        tv.IsMove = p.IsMove;
        // 0-based, matching the indirect call site's own DiagnoseExplicitMoveToBorrowParam.
        tv.VariableName = std::to_string(index);
        return tv;
}

void LLVMBackend::ApplyFuncPtrSinkTransfer(const std::string& functionName,
        const std::vector<TypeAndValue::FuncPtrParam>& params, const std::vector<NamedVariable>& args)
{
        bool anySink = false;
        for (const auto& p : params) anySink = anySink || p.IsOwningSink || p.IsMove;
        if (!anySink) return;
        // A DECLARED `move` param takes the same ApplyMoveParamTransfer path a direct call takes
        // (FuncPtrParamAsTypeAndValue carries IsMove); an INFERRED sink keeps IsMove false.
        std::vector<TypeAndValue> synth;
        for (size_t i = 0; i < params.size(); i++)
            synth.push_back(FuncPtrParamAsTypeAndValue(params[i], i));
        ApplyMoveParamTransfer(functionName, synth, args, /*paramsCarryAllocAlign*/ true);
}

void LLVMBackend::ApplyMoveParamTransfer(const std::string& functionName,
        const std::vector<TypeAndValue>& params, const std::vector<NamedVariable>& args,
        bool paramsCarryAllocAlign, bool calleeIsMethod)
{
        for (size_t i = 0; i < params.size() && i < args.size(); i++)
        {
            RejectOwningTempUniqueFieldIntoSinkParam(functionName, params[i], args[i]);
            // A plain by-value parameter the callee body unconditionally moves is a synthesized
            // move sink too - but only when the concrete type owns a resource AND the matched arg
            // is itself an OWNER (not a borrow/alias). A borrow arg has nothing to transfer, so
            // nulling it would orphan a value the caller still needs.
            // A fat closure (Lambda<...>) is an OWNING VALUE like string: it owns its captured env.
            // Admit __closure_fat_ptr and any encoded closure element type; a thin C fn ptr owns
            // nothing and never reaches here as a sink (ParamIsOwningSinkEligible rejects it).
            bool paramOwnsResource = IsOwningValueOrClosureType(params[i].TypeName);
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
                    || IsVariableOwning(args[i].CallerName) || IsVariableOwningString(args[i].CallerName)
                    || IsOwnedClosureTemp(args[i].Primary)
                    || (!args[i].CallerName.empty() && paramOwnsResource));
            bool isOwningSink = OwningSinkConsumesConcrete(params[i]) && argIsOwner && paramOwnsResource;
            // Ownership-laundering guard: the arg is a value this function only BORROWS (a plain
            // by-value owning-value param of the current function) but the callee's param CONSUMES
            // it (a sink / `move` / unique). Transferring would null this function's alias while the
            // TRUE owner (a caller further up) still frees the resource - a silent double-free.
            bool paramConsumesOwningValue = paramOwnsResource
                && (params[i].IsMove || params[i].IsUniqueTypeArg || OwningSinkConsumesConcrete(params[i]));
            if (paramConsumesOwningValue
                && IsVariableBorrowedOwningValue(args[i].CallerName))
            {
                LogErrorMessage(
                    "call to '{}': parameter '{}' takes ownership of a value this function only "
                    "borrows ('{}' is a by-value parameter, so the caller keeps ownership). Accept "
                    "the parameter as a sink ({} it in the body), or pass a copy with '{}'.",
                    { functionName, params[i].VariableName, args[i].CallerName, "move", "copy()" });
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
                && (args[i].IsOwning || IsVariableOwning(args[i].CallerName));
            if (params[i].IsMove || params[i].IsUniqueTypeArg || isOwningSink
                || movesIntoBorrowingElement)
            {
                // A `move` param takes ownership and frees the block itself. The allocation alignment
                // of an over-aligned `new T[n]` is not in the element type, so the callee can recover
                // it only when the PARAMETER declares the same `alignas(_, N)` clause. A matching clause
                // is allowed; a missing/mismatched one would free the block wrong and corrupt the heap.
                // Alignment carried by the TYPE is not tagged and passes through freely.
                if (paramsCarryAllocAlign
                    && args[i].AllocAlignment != params[i].AllocAlignValue
                    && (args[i].AllocAlignment > kDefaultNewAlign
                        || params[i].AllocAlignValue > kDefaultNewAlign))
                {
                    if (params[i].AllocAlignValue == 0)
                        LogErrorMessage(
                            "cannot move the over-aligned buffer '{}' ('{}') into the '{}' "
                            "parameter of '{}': that alignment is a property of the allocation, not of the type, so "
                            "the callee cannot recover it. Declare the parameter '{}' so the block "
                            "alignment is recorded, or over-align the ELEMENT TYPE instead.",
                            { args[i].CallerName.empty() ? args[i].TypeAndValue.TypeName : args[i].CallerName,
                              std::format("new T[n] alignas(0, {})", args[i].AllocAlignment), "move", functionName,
                              std::format("alignas(0, {})", args[i].AllocAlignment) });
                    else
                        LogErrorMessage(
                            "alignment mismatch moving into the 'move' parameter of '{}': the parameter is declared "
                            "'alignas(0, {})' but the argument was allocated 'alignas(0, {})'. The two must agree "
                            "so the callee frees with the correct alignment.",
                            { functionName, std::to_string(params[i].AllocAlignValue),
                              std::to_string(args[i].AllocAlignment) });
                }

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
                UnregisterOwnedPtrTemp(args[i].Primary);

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
                bool isFieldAccess = !args[i].FieldName.empty();
                // The CallerName fallback resolves the BASE variable's storage; do NOT use it
                // for a field access or it would null the base pointer instead of the field.
                // A field access already carries its own (GEP) Storage.
                if (srcStorage == nullptr && !isFieldAccess && !args[i].CallerName.empty())
                {
                    auto ref = FindVariableStorage(args[i].CallerName);
                    srcStorage = ref.Storage;
                    if (srcBaseTy == nullptr) srcBaseTy = ref.BaseType;
                }
                // An OWNING interface VALUE arg (an owning `unique IShape` local) moved into a
                // move interface param transfers ownership: null the source fat-ptr's data field so
                // its scope-exit teardown sees null and no-ops, else callee and source double-free.
                // Raw struct-value boxes are rejected at the call site, so only a real interface
                // value reaches here; the owning check keeps a borrow source untouched.
                if (params[i].IsFatInterfaceValue()
                    && args[i].TypeAndValue.IsFatInterfaceValue()
                    && (args[i].IsOwning || IsVariableOwning(args[i].CallerName))
                    && srcStorage != nullptr)
                {
                    auto* dataField = builder->CreateStructGEP(GetFatPtrType(), srcStorage, 1);
                    builder->CreateStore(
                        llvm::ConstantPointerNull::get(builder->getInt8Ty()->getPointerTo()), dataField);
                    if (!args[i].CallerName.empty() && !isFieldAccess)
                        MarkVariableMoved(args[i].CallerName);
                }
                if (srcStorage != nullptr && !isInterfaceBorrow)
                {
                    // dyn_cast_or_null: a hand-built NamedVariable may carry storage but a null
                    // BaseType. A bare dyn_cast on a null type dereferences null and segfaults
                    // the compiler (the pre-existing list<string>-json crash); _or_null treats a
                    // missing type as "no match" and falls through to the diagnostic below.
                    if (auto* ptrTy = llvm::dyn_cast_or_null<llvm::PointerType>(srcBaseTy))
                    {
                        // Pointer move param: null the caller's storage.
                        builder->CreateStore(llvm::ConstantPointerNull::get(ptrTy), srcStorage);
                        StoreRawArrayLength(args[i], nullptr);
                        // --sanitize=ownership (M1): record this move site for a tracked pointer local.
                        if (!isFieldAccess)
                            SetOwnMoveOrigin(srcStorage, currentLine, currentColumn);
                    }
                    else if (params[i].TypeName == "string" && args[i].IsOwningString)
                    {
                        // String move param: zero out _ptr in the caller's alloca so its destructor is a no-op.
                        // (Does not need srcBaseTy - the string layout is looked up by name.)
                        auto* strTy = llvm::StructType::getTypeByName(*context, "string");
                        if (strTy)
                        {
                            // q11 ruling point 3: a move out of PROGRAM-LIFETIME storage re-initializes
                            // it, so the next entry reads a defined value. Nulling _ptr alone leaves
                            // _len stale, and `string` is { i8* _ptr, i32 _len } - the next run then
                            // reads a stale length off a null pointer and segfaults. A frame local
                            // cannot observe this (its declaration re-runs, and reads after the move
                            // are rejected), so only the outliving storage class is widened here.
                            if (llvm::isa<llvm::GlobalVariable>(srcStorage))
                                builder->CreateStore(llvm::ConstantAggregateZero::get(strTy), srcStorage);
                            else
                            {
                                auto* ptrField = builder->CreateStructGEP(strTy, srcStorage, 0);
                                auto* i8ptrTy = builder->getInt8Ty()->getPointerTo();
                                builder->CreateStore(llvm::ConstantPointerNull::get(i8ptrTy), ptrField);
                            }
                        }
                    }
                    else if (auto* stTy = llvm::dyn_cast_or_null<llvm::StructType>(srcBaseTy))
                    {
                        // Struct move param: zero the caller's entire struct so its destructor is a no-op.
                        builder->CreateStore(llvm::ConstantAggregateZero::get(stTy), srcStorage);
                    }
                    else if (srcBaseTy == nullptr && params[i].TypeName != "string")
                    {
                        // We have caller storage to clear but no type telling us how. This can only
                        // arise from a malformed (hand-built) argument; emit a clear diagnostic
                        // rather than leaving a stale value that a later destructor would double-free.
                        LogErrorMessage(
                            "call to '{}': 'move' argument {} has no resolved type, so its source "
                            "storage cannot be cleared after the move", { functionName, std::to_string(i) });
                    }
                }
                // Compile-time: mark the caller's storage as moved so subsequent reads are rejected.
                // Covers pointer, owning-string, and struct move params - all cases where caller storage was zeroed.
                // Moving a FIELD (`node->left`) marks only that field, not the whole base variable.
                if (!args[i].CallerName.empty() && srcStorage != nullptr &&
                    !isInterfaceBorrow && srcBaseTy != nullptr)
                {
                    bool isPtr = llvm::isa<llvm::PointerType>(srcBaseTy);
                    bool isOwningStr = params[i].TypeName == "string" && args[i].IsOwningString;
                    bool isStruct = llvm::isa<llvm::StructType>(srcBaseTy);
                    if (isPtr || isOwningStr || isStruct)
                    {
                        if (isFieldAccess)
                            MarkVariableFieldMoved(args[i].CallerName, args[i].FieldName);
                        else
                            MarkVariableMoved(args[i].CallerName);
                    }
                }
            }
        }
    }

bool LLVMBackend::ArgumentIsProvablyDataPointer(llvm::Value* value, const NamedVariable& arg) const
{
        if (llvm::isa<llvm::Function>(value)) return false;               // a named function
        if (llvm::isa<llvm::ConstantPointerNull>(value)) return false;    // parity with the direct path
        if (arg.TypeAndValue.IsFunctionPointer) return false;             // a closure value
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
        return WidenBareOrThinToClosureFat(val);
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

llvm::Value* LLVMBackend::WidenBareOrThinToClosureFat(llvm::Value* val)
{
        if (auto* fn = llvm::dyn_cast<llvm::Function>(val))
            return WrapBareValueAsFatStruct(fn);
        if (val && !val->getType()->isStructTy() && val->getType()->isPointerTy())
            return WidenThinToFat(val);   // thin function<T> -> fat Lambda<T> {code, null}
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

llvm::Value* LLVMBackend::LowerByValueArg(llvm::Value* value, const TypeAndValue& param, const NamedVariable& arg)
{
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
            && value && value->getType() == builder->getInt8Ty()->getPointerTo())
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
            // Upconvert to match the declared parameter type (e.g. i16 -> i32; struct identity).
            bool argIsUnsigned = arg.TypeAndValue.IsUnsignedInteger() != -1;
            value = Upconvert(value, GetType(param), argIsUnsigned);
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
                    srcPtr, llvm::ConstantPointerNull::get(builder->getInt8Ty()->getPointerTo()),
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
                auto* heapPtr = builder->CreateBitCast(rawPtr, builder->getInt8Ty()->getPointerTo(), "litbuf");
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

bool LLVMBackend::ArgumentProvablyMismatchesParameter(const NamedVariable& arg, const TypeAndValue& param) const
{
        if (!param.IsFunctionPointer && !IsEncodedClosureType(param.TypeName))
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
            && IsDataStructure(param.TypeName);
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
        if (!arg.TypeAndValue.PointerDepthRefuses(param))
            return "";

        bool writable = true;
        std::string shown = DisplayNameOfMangledType(p.TypeName, &writable);
        std::string argShown = DisplayNameOfMangledType(arg.TypeAndValue.TypeName);
        // A primitive pointer argument carries no CFlat TypeName; report only the depth then.
        if (p.PointerDepth >= 2)
            return std::format("has type '{}**' and the argument has {} - there is no "
                "implicit address-of. Take its address with '&' at the call site", shown,
                argShown.empty() ? std::string("one fewer level of indirection")
                                 : std::format("type '{}*'", argShown));
        return std::format("has type '{}*' and the argument has {} - there is no implicit "
            "dereference. Dereference it with '*' at the call site", shown,
            argShown.empty() ? std::string("one more level of indirection")
                             : std::format("type '{}**'", argShown));
    }

bool LLVMBackend::DiagnoseProvableInterfaceArgMismatch(const std::string& ifaceName,
        const std::string& methodName, const std::vector<NamedVariable>& args,
        const std::vector<TypeAndValue>& params)
{
        for (size_t i = 0; i < args.size() && i < params.size(); i++)
        {
            if (ArgumentProvablyMismatchesParameter(args[i], params[i]))
            {
                std::string why = DescribeFuncPtrSignatureMismatch(args[i].TypeAndValue, params[i]);
                LogErrorMessage("no method of '{}.{}' matches the given arguments{}",
                    { ifaceName, methodName, why.empty() ? "" : " - " + why });
                return true;
            }
            if (args[i].TypeAndValue.PointerDepthRefuses(params[i]))
            {
                bool tooDeep = args[i].TypeAndValue.IsProvenDoublePointer()
                    || args[i].TypeAndValue.IsProvenDecayedDoublePointer();
                std::string shown = DisplayNameOfMangledType(params[i].TypeName);
                std::string argShown = DisplayNameOfMangledType(args[i].TypeAndValue.TypeName);
                if (tooDeep)
                {
                    LogErrorMessage(
                        "call to '{}.{}': parameter {} '{}' has type '{}' and the argument has {} - "
                        "there is no implicit dereference. Dereference it with '{}' at the call site.",
                        { ifaceName, methodName, std::to_string(i), params[i].VariableName,
                          shown + "*", argShown.empty() ? std::string("one more level of indirection")
                                           : std::format("type '{}**'", argShown), "*" });
                }
                else
                {
                    LogErrorMessage(
                        "call to '{}.{}': parameter {} '{}' has type '{}' and the argument has {} - "
                        "there is no implicit address-of. Take its address with '{}' at the call site.",
                        { ifaceName, methodName, std::to_string(i), params[i].VariableName,
                          shown + "**", argShown.empty() ? std::string("one fewer level of indirection")
                                           : std::format("type '{}*'", argShown), "&" });
                }
                return true;
            }
            if (PointerArgIntoByValueParam(args[i], params[i]))
            {
                // Rendered when the rendering is provably writable source; a nested instantiation
                // stays raw and loses the advice clause rather than naming a type that cannot exist.
                bool writable = true;
                std::string shown = DisplayNameOfMangledType(params[i].TypeName, &writable);
                if (writable)
                {
                    LogErrorMessage(
                        "call to '{}.{}': cannot pass a '{}' to by-value parameter '{}' of type '{}' - "
                        "there is no implicit dereference. Write '{}' at the call site to pass the "
                        "pointee, or declare the parameter as '{}'.",
                        { ifaceName, methodName, std::format("{}*", shown), params[i].VariableName,
                          shown, "*", std::format("{}*", shown) });
                }
                else
                {
                    LogErrorMessage(
                        "call to '{}.{}': cannot pass a '{}' to by-value parameter '{}' of type '{}' - "
                        "there is no implicit dereference. Write '{}' at the call site to pass the "
                        "pointee.",
                        { ifaceName, methodName, std::format("{}*", shown), params[i].VariableName,
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
                    { ifaceName, methodName, params[i].VariableName, params[i].TypeName,
                      std::format("{}*", params[i].TypeName), "*",
                      std::format("{}*", params[i].TypeName) });
                return true;
            }
        }
        return false;
    }

int LLVMBackend::ResolveInterfaceMethodSlot(const std::string& ifaceName, const std::string& methodName,
        const std::vector<InterfaceMethod>& methods, const std::vector<NamedVariable>& args,
        std::vector<NamedVariable>& matchedArgs)
{
        matchedArgs = args;

        std::vector<int> byName;
        for (int i = 0; i < (int)methods.size(); i++)
            if (methods[i].Name == methodName) byName.push_back(i);

        if (byName.empty())
        {
            LogErrorMessage("interface '{}' has no method '{}'", { ifaceName, methodName });
            return -1;
        }

        // A variadic interface method is rejected at its declaration (no vtable lowering exists
        // for '...'), so every slot here has a fixed parameter list and arity is exact.
        std::vector<int> byArity;
        for (int idx : byName)
            if (methods[idx].Parameters.size() == args.size()) byArity.push_back(idx);

        if (byArity.empty())
        {
            // Distinct arities only: same-arity overloads differ by TYPE, and listing "1 or 1"
            // would read as nonsense. Sorted so the list is stable across declaration order.
            std::vector<size_t> arities;
            for (int idx : byName) arities.push_back(methods[idx].Parameters.size());
            std::sort(arities.begin(), arities.end());
            arities.erase(std::unique(arities.begin(), arities.end()), arities.end());

            std::string expected;
            for (size_t i = 0; i < arities.size(); i++)
            {
                if (i > 0) expected += (i + 1 == arities.size()) ? " or " : ", ";
                expected += std::to_string(arities[i]);
            }
            LogErrorMessage(
                "no overload of interface method '{}.{}' takes {} argument(s); expected {}",
                { ifaceName, methodName, std::to_string(args.size()), expected });
            return -1;
        }

        if (byArity.size() == 1)
        {
            // Route a single match through MatchFunction anyway so a named argument
            // (`f(count: n)`) still lands in its declared slot.
            if (!args.empty())
            {
                auto matched = MatchFunction(args, methods[byArity[0]].Parameters);
                if (!matched.empty()) matchedArgs = matched;
            }
            // The lone slot was taken on ARITY alone, so an `int` reached a closure slot and
            // was called. Reject only a PROVABLE mismatch (LogError does not return).
            if (DiagnoseProvableInterfaceArgMismatch(ifaceName, methodName, matchedArgs,
                    methods[byArity[0]].Parameters))
                return -1;
            return byArity[0];
        }

        std::vector<std::pair<std::vector<NamedVariable>, FunctionSymbol>> candidates;
        for (int idx : byArity)
        {
            std::vector<NamedVariable> matched;
            if (!args.empty())
            {
                matched = MatchFunction(args, methods[idx].Parameters, false, true);
                if (matched.empty()) continue;
            }
            FunctionSymbol sym;
            sym.UniqueName = std::to_string(idx);   // slot index carrier; also the "matched" marker
            sym.ReturnType = methods[idx].ReturnType;
            sym.Parameters = methods[idx].Parameters;
            candidates.emplace_back(matched, sym);
        }

        const auto& [scored, winner] = ComputeOverloadFunction(candidates);
        if (!winner.UniqueName.empty())
        {
            if (!scored.empty()) matchedArgs = scored;
            return std::stoi(winner.UniqueName);
        }

        // Same arity, no type winner: keep the historical first-slot pick rather than reject a
        // program the scorer cannot rank (an unresolved generic argument lands here).
        int fallbackIdx = byArity[0];
        if (!args.empty())
        {
            // Nothing scored, so re-run only candidates whose parameter names bind. A different
            // slot may fail on names while the slot the caller meant fails on types; replaying the
            // first slot unconditionally would report the wrong cause. An all-positional call
            // binds every slot by construction.
            std::vector<std::string> argumentNames;
            argumentNames.reserve(args.size());
            for (const auto& arg : args)
                argumentNames.push_back(arg.TypeAndValue.VariableName);
            bool replayedNameMatch = false;
            bool permutedMatchedArgs = false;
            for (int idx : byArity)
            {
                auto binding = ComputeArgumentPositions(argumentNames, methods[idx].Parameters, false);
                if (!binding.Ok) continue;
                replayedNameMatch = true;
                if (!permutedMatchedArgs)
                    fallbackIdx = idx;
                MatchFunction(args, methods[idx].Parameters, false, false);
                if (!permutedMatchedArgs)
                {
                    matchedArgs = MatchFunction(args, methods[idx].Parameters);
                    permutedMatchedArgs = true;
                }
            }
            if (!replayedNameMatch)
                MatchFunction(args, methods[byArity[0]].Parameters, false, false);

            // The fallback slot still needs its permutation - matchedArgs is raw call order,
            // so without this a reordered named call would bind its arguments backwards. The
            // first name-valid slot is the one the replay just established as intended.
        }
        // Same provable-mismatch gate as the lone-slot arm above: without it, adding a second
        // same-arity overload turns that arm's clean error back into a silent miscompile.
        if (DiagnoseProvableInterfaceArgMismatch(ifaceName, methodName, matchedArgs,
                methods[fallbackIdx].Parameters))
            return -1;
        return fallbackIdx;
    }

llvm::Value* LLVMBackend::CallInterfaceMethod(llvm::Value* ifacePtr, const std::string& ifaceName,
        const std::string& methodName, const std::vector<NamedVariable>& extraArgNVs,
        const NullIfaceDispatchSite* site)
{
        auto fatTy = GetFatPtrType();
        auto ptrTy = builder->getInt8Ty()->getPointerTo();

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
            LogErrorMessage("unknown interface '{}'", { ifaceName });
            return nullptr;
        }

        std::vector<NamedVariable> callArgNVs;
        int methodIdx = ResolveInterfaceMethodSlot(ifaceName, methodName, *ifaceMethods,
                                                   extraArgNVs, callArgNVs);
        if (methodIdx < 0)
            return nullptr;
        const InterfaceMethod* methodInfo = &(*ifaceMethods)[methodIdx];

        // Method indices start at 1 (slot 0 is type ID)
        auto fnPtrField = builder->CreateGEP(ptrTy, vtablePtr, builder->getInt32(methodIdx + 1));
        auto fnPtr = builder->CreateLoad(ptrTy, fnPtrField);

        // Record the return type so the call site can populate the result's TypeAndValue
        // (needed for chaining a method on the result, e.g. `e.toJson().data()`).
        lastCallReturnType = methodInfo->ReturnType;

        llvm::Type* retTy = GetType(methodInfo->ReturnType);
        std::vector<llvm::Type*> paramTypes = { ptrTy };
        for (const auto& p : methodInfo->Parameters)
        {
            paramTypes.push_back(GetType(p));
            if (ParameterCarriesRawArrayCount(p))
                paramTypes.push_back(builder->getInt64Ty());
        }
        if (ReturnCarriesRawArrayCount(methodInfo->ReturnType))
            paramTypes.push_back(builder->getInt64Ty()->getPointerTo());
        auto fnTy = llvm::FunctionType::get(retTy, paramTypes, false);

        // callArgNVs is arity-matched to Parameters by the resolver, so no clamp here: a
        // mismatched arity was already rejected instead of silently truncating the call.
        std::vector<llvm::Value*> callArgs = { dataPtr };
        for (size_t i = 0; i < callArgNVs.size() && i < methodInfo->Parameters.size(); i++)
        {
            const auto& nv = callArgNVs[i];
            const auto& param = methodInfo->Parameters[i];

            // Same closure SHAPE gate the direct call path applies (RejectFuncPtrShapeMismatch):
            // a vtable slot lowers each argument by bit pattern, so a mismatch is called as code.
            if (!param.IsInterface && RejectFuncPtrShapeMismatch(nv, param))
                return nullptr;

            // Code value into a DATA parameter: the scorer's gate, which this path never runs.
            if (RejectCodeValueIntoDataParam(nv, param, ifaceName, methodName))
                return nullptr;

            if (param.IsInterface && !nv.TypeAndValue.IsInterface)
            {
                // Concrete struct/pointer -> interface fat ptr upconversion.
                // Reject a pointer-shaped source: it is not an instance of its element class.
                std::string argShape = DescribePointerShapedInterfaceSource(nv.TypeAndValue);
                if (!argShape.empty())
                {
                    LogRawError(FormatPointerShapedInterfaceUpcastError(
                        argShape, nv.TypeAndValue.TypeName, param.TypeName));
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
        }

        llvm::Value* rawReturnCountSlot = nullptr;
        if (ReturnCarriesRawArrayCount(methodInfo->ReturnType))
        {
            rawReturnCountSlot = CreateRawArrayReturnCountSlot();
            callArgs.push_back(rawReturnCountSlot);
        }

        auto* callResult = builder->CreateCall(fnTy, fnPtr, callArgs);
        RegisterRawArrayCallResult(callResult, rawReturnCountSlot,
                                   methodInfo->ReturnType.AllocAlignValue);

        // A `move` parameter on an INTERFACE method transfers ownership just as it does on a
        // direct call, so the caller's source must be nulled/marked-moved here too. Without this
        // the source keeps its owning flag and scope exit frees what the callee now owns.
        for (size_t i = 0; i < methodInfo->Parameters.size() && i < callArgNVs.size(); i++)
            DiagnoseExplicitMoveToBorrowParam(ifaceName + "." + methodName,
                methodInfo->Parameters[i], callArgNVs[i]);
        ApplyMoveParamTransfer(ifaceName + "." + methodName, methodInfo->Parameters, callArgNVs);

        // A temp's `unique` field handed to a PLAIN `T*` parameter of a VIRTUAL slot. Same point
        // in the sequence as the direct path's RecordTempUniqueFieldArgs, for the same reason.
        RecordTempUniqueFieldInterfaceArgs(callResult, ifaceName, *methodInfo, callArgNVs);

        // Classify the virtual result's ownership exactly like the direct-call path
        // (CreateOverloadedFunctionCall): a 'move string' / 'move T*' / 'move <interface>'
        // return hands an owned value to the caller. Set the side-channel so a binding site
        // (decl-init / assignment / move-param / return) re-homes it and clears the flag; an
        // inline-used owned STRING result is registered for end-of-full-expression cleanup
        // (mirrors the direct path's RegisterOwnedStringTemp) so e.g. `tree.toJson().data()`
        // does not leak. Without this the virtual path left the result classified as a borrow.
        const auto& rt = methodInfo->ReturnType;
        // Same argument-list retirement as the direct-call path: the result is a different value
        // than anything a `new`/`move` in the argument list produced.
        lastOwningResult = false;
        lastAllocAlignment = 0;
        // A substituted `unique X*` type-arg return owns like `move` (see the direct-call path).
        lastCallReturnsOwned = (rt.IsMove || rt.IsUniqueTypeArg)
            && (rt.TypeName == "string" || rt.Pointer || rt.IsInterface);
        if (lastCallReturnsOwned)
            RegisterOwnedReturnTemp(callResult, ifaceName + "." + methodName, rt);
        lastCallReturnsAllocAlign = rt.AllocAlignValue;
        if (lastCallReturnsOwned
            && callResult->getType() == llvm::StructType::getTypeByName(*context, "string"))
            RegisterOwnedStringTemp(callResult);

        return callResult;
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
            LogErrorMessage("unknown interface{} '{}'", { ":", interfaceName });
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
                            break;
                        }
                    }
                }
            }

            if (!found)
            {
                LogErrorMessage("class '{}' does not implement '{}::{}'",
                    { structName, interfaceName, method.Name });
            }
            else if (!anyConforms)
            {
                // No overload agrees with the contract; report the ownership mismatch against the
                // first signature-matching candidate so the diagnostic names a real overload.
                VerifyInterfaceMethodContract(structName, interfaceName, method, *firstCandidate);
            }
        }

        auto sdIt = dataStructures.find(structName);
        if (sdIt != dataStructures.end())
            VerifyInterfaceFields(structName, interfaceName, sdIt->second.StructFields);
    }

llvm::Function* LLVMBackend::SynthesizeReflectFunction(const std::string& structName)
{
        auto compiler = this;

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
            LogErrorMessage("reflect: unknown struct '{}'", { structName });
            return nullptr;
        }
        auto& sd = it->second;
        if (!sd.StructType)
        {
            LogErrorMessage("reflect: struct '{}' has no LLVM type", { structName });
            return nullptr;
        }
        if (sd.IsUnion)
        {
            LogErrorMessage("reflect is not supported on union type '{}'", { structName });
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
            LogErrorMessage("reflect: failed to create function for '{}'", { structName });
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
            auto* gep = compiler->builder->CreateStructGEP(sd.StructType, objPtr, (unsigned)i,
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
            else if ((typeName == "float" || typeName == "double") && !isPtr && !isInterface)
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
            auto* word = compiler->builder->CreateLoad(storageTy, storagePtr);

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
