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

// ---- Definitions moved out of LLVMBackend.h (Interfaces) ----

void LLVMBackend::CreateInterfaceDefinition(const std::string& name, const std::vector<std::string>& parentNames,
                                   std::vector<InterfaceMethod> methods, std::vector<TypeAndValue> fields,
                                   const std::string& definitionSite)
{
        std::string rejectedSiteKey = name + "\n" + definitionSite;
        if (!definitionSite.empty() && rejectedInterfaceDefSites_.count(rejectedSiteKey) != 0)
        {
            auto messageIt = rejectedInterfaceDefMessages_.find(rejectedSiteKey);
            if (messageIt != rejectedInterfaceDefMessages_.end()
                && !expectedError.empty() && messageIt->second.find(expectedError) != std::string::npos)
            {
                if (!diagnosticSink_)
                    std::cout << "PASS: expected error received\n";
                throw ExpectedErrorReceived{};
            }
            return;
        }

        // A second interface definition claiming a registered name used to overwrite it in
        // silence, so every use of the first one dispatched through the second one's contract.
        if (!definitionSite.empty())
        {
            auto siteIt = interfaceDefSites.find(name);
            if (siteIt != interfaceDefSites.end() && siteIt->second != definitionSite
                && !IsSameCoreFileDefSite(siteIt->second, definitionSite))
            {
                rejectedInterfaceDefSites_.insert(rejectedSiteKey);
                bool siteIsCore = coreInterfaceDefs_.count(name) != 0;
                std::string displaySite = ShortenDefSiteForDisplay(siteIt->second, siteIsCore);
                std::string message;
                if (siteIsCore)
                    message = std::format(
                        "interface '{}' collides with the core library interface of the same name "
                        "defined at {} - declare it inside a namespace, or rename it (an interface "
                        "name must be unique within its namespace)", name, displaySite);
                else
                    message = std::format(
                        "interface '{}' is already defined at {} - an interface name must be unique "
                        "within its namespace", name, displaySite);
                rejectedInterfaceDefMessages_[rejectedSiteKey] = message;
                // Keep the normal basename prefix unless this collision has two files with the
                // same basename; then disambiguate the reported occurrence as well as the def-site.
                auto currentPath = std::filesystem::path(currentSourceFilePath_);
                auto priorPath = DefSitePath(siteIt->second);
                if (!currentPath.empty() && !priorPath.empty()
                    && currentPath.filename() == priorPath.filename())
                {
                    std::string currentSite = currentSourceFilePath_
                        + std::format("({},{})", currentLine, currentColumn);
                    std::string displayCurrent = ShortenDefSiteForDisplay(currentSite, currentSourceIsCore_);
                    displayCurrent.resize(displayCurrent.rfind('('));
                    ReportingFileScope reportScope(this, displayCurrent, currentLine, currentColumn);
                    LogError(std::move(message));
                }
                LogError(std::move(message));
                return;
            }
        }

        // A CFlat interface value is a fat pointer; a WinMD interface is a COM struct with an
        // lpVtbl. If a name meant both, the use site would GEP the fat pointer as a COM struct and
        // emit invalid IR. WinMD types are registered fully qualified so they can never take a bare
        // name - the only way back into that state is a `using` alias claiming this one. Reject it.
        std::string aliasedName = ResolveTypeAlias(name);
        if (aliasedName != name && IsWinrtProjectedType(aliasedName))
        {
            LogError(std::format(
                "interface '{}' collides with the WinMD alias 'using {} = {};' - rename one of them "
                "(a WinMD type and a CFlat interface cannot share a name)", name, name, aliasedName));
            return;
        }

        // Recorded only once every rejection above is past, so the site map never claims a name
        // that interfaceTable does not actually hold (LogError throws out of this function).
        if (!definitionSite.empty())
        {
            interfaceDefSites[name] = definitionSite;
            if (currentSourceIsCore_)
                coreInterfaceDefs_.insert(name);
            else
                coreInterfaceDefs_.erase(name);
        }

        // Prepend inherited methods and fields from parent interfaces (in order)
        std::vector<InterfaceMethod> inherited;
        std::vector<TypeAndValue> inheritedFields;
        for (const auto& parentName : parentNames)
        {
            const auto* parentMethods = FindInterface(parentName);
            if (parentMethods == nullptr)
            {
                LogError(std::format("unknown parent interface: '{}'", parentName));
                continue;
            }
            for (const auto& m : *parentMethods)
                inherited.push_back(m);
            if (auto fit = interfaceFields.find(parentName); fit != interfaceFields.end())
                for (const auto& f : fit->second)
                    inheritedFields.push_back(f);
        }
        inherited.insert(inherited.end(), methods.begin(), methods.end());
        inheritedFields.insert(inheritedFields.end(), fields.begin(), fields.end());
        interfaceTable[name] = std::move(inherited);
        interfaceFields[name] = std::move(inheritedFields);
        interfaceParents[name] = parentNames;
    }

const std::vector<LLVMBackend::TypeAndValue>* LLVMBackend::GetInterfaceFields(const std::string& ifaceName) const
{
        auto it = interfaceFields.find(ResolveTypeAlias(ifaceName));
        return it == interfaceFields.end() ? nullptr : &it->second;
    }

int LLVMBackend::InterfaceFieldIndex(const std::string& ifaceName, const std::string& fieldName) const
{
        const auto* fields = GetInterfaceFields(ifaceName);
        if (fields == nullptr) return -1;
        for (int i = 0; i < (int)fields->size(); i++)
            if ((*fields)[i].VariableName == fieldName) return i;
        return -1;
    }

size_t LLVMBackend::InterfaceFieldCount(const std::string& ifaceName) const
{
        const auto* fields = GetInterfaceFields(ifaceName);
        return fields == nullptr ? 0 : fields->size();
    }

bool LLVMBackend::IsPrimitiveTypeName(const std::string& name)
{
        static const std::unordered_set<std::string> primitives = {
            "int", "char", "short", "long", "ulong", "bool", "void",
            "float", "double",
            "i8", "i16", "i32", "i64",
            "u8", "u16", "u32", "u64",
        };
        return primitives.count(name) > 0;
    }

void LLVMBackend::RegisterTypeAlias(const std::string& alias, const std::string& target)
{
        const auto candidates = ScopedNameCandidates(alias);
        typeAliases[candidates.empty() ? alias : candidates.front()] = target;
    }

std::string LLVMBackend::ResolveTypeAlias(const std::string& name) const
{
        for (const auto& candidate : ScopedNameCandidates(name))
        {
            auto it = typeAliases.find(candidate);
            if (it != typeAliases.end()) return it->second;
        }
        return name;
    }

const LLVMBackend::TypeAndValue* LLVMBackend::FindFunctionTypeAlias(const std::string& name) const
{
        for (const auto& candidate : ScopedNameCandidates(name))
        {
            auto it = functionTypeAliases.find(candidate);
            if (it != functionTypeAliases.end()) return &it->second;
        }
        return nullptr;
    }

void LLVMBackend::RegisterManglingAlias(const std::string& alias, const std::string& target)
{
        const auto candidates = ScopedNameCandidates(alias);
        manglingAliases_[candidates.empty() ? alias : candidates.front()] = target;
    }

std::string LLVMBackend::ResolveManglingAlias(const std::string& name) const
{
        std::string cur = name;
        for (int guard = 0; guard < 8; ++guard)
        {
            bool found = false;
            for (const auto& candidate : ScopedNameCandidates(cur))
            {
                auto it = manglingAliases_.find(candidate);
                if (it == manglingAliases_.end()) continue;
                if (it->second == cur) return cur;
                cur = it->second;
                found = true;
                break;
            }
            if (!found) break;
        }
        return cur;
    }

void LLVMBackend::RegisterGenericBaseAlias(const std::string& alias, const std::string& target)
{
        const auto candidates = ScopedNameCandidates(alias);
        genericBaseAliases_[candidates.empty() ? alias : candidates.front()] = target;
    }

bool LLVMBackend::IsGenericBaseAlias(const std::string& name) const
{
        for (const auto& candidate : ScopedNameCandidates(name))
            if (genericBaseAliases_.count(candidate) != 0) return true;
        return false;
    }

std::string LLVMBackend::ResolveGenericBaseAlias(const std::string& base) const
{
        // An alias TARGET is explicit and already resolved at its declaration site: returning it
        // verbatim is what stops a global `using GBox = Box;` naming NS.Box inside `namespace NS`.
        for (const auto& candidate : ScopedNameCandidates(base))
        {
            auto it = genericBaseAliases_.find(candidate);
            if (it != genericBaseAliases_.end()) return it->second;
        }
        return ResolveGenericTemplateBase(base);
    }

bool LLVMBackend::IsGenericTemplateKey(const std::string& key) const
{
        return gts.genericStructTemplates.count(key) != 0
            || gts.genericClassTemplates.count(key) != 0
            || gts.genericInterfaceTemplates.count(key) != 0
            || gts.scannedGenericStructNames.count(key) != 0
            || gts.scannedGenericInterfaceNames.count(key) != 0;
    }

std::string LLVMBackend::ResolveGenericTemplateBase(const std::string& base) const
{
        if (base.empty()) return base;
        if (!currentNamespace_.empty())
        {
            std::string prefix = currentNamespace_;
            while (true)
            {
                if (std::string candidate = prefix + "." + base; IsGenericTemplateKey(candidate))
                    return candidate;
                auto dot = prefix.rfind('.');
                if (dot == std::string::npos) break;
                prefix = prefix.substr(0, dot);
            }
        }
        return base;
    }

bool LLVMBackend::AnyGenericTypeTemplateNamed(const std::string& spelledBase) const
{
        if (spelledBase.empty()) return false;
        if (IsGenericTemplateKey(spelledBase)) return true;
        if (IsGenericTemplateKey(ResolveGenericTemplateBase(spelledBase))) return true;
        if (IsGenericBaseAlias(spelledBase)) return true;
        if (gts.scannedGenericStructNamesUncertain.count(spelledBase) != 0) return true;
        // An imported winmd generic is a real template built elsewhere; keep whatever the shell
        // sites did with it rather than turning a Windows-only spelling into `unknown type`.
        if (IsWinrtGenericBase(spelledBase) || IsWinrtFullName(spelledBase)) return true;
        // A bare spelling of a namespaced template: it names no key, but the template exists, and
        // the shell is what carries today's downstream diagnostic for it.
        std::string tail = "." + spelledBase;
        auto endsWithTail = [&](const std::string& n) { return n.ends_with(tail); };
        for (const auto& kv : gts.genericStructTemplates)    if (endsWithTail(kv.first)) return true;
        for (const auto& kv : gts.genericClassTemplates)     if (endsWithTail(kv.first)) return true;
        for (const auto& kv : gts.genericInterfaceTemplates) if (endsWithTail(kv.first)) return true;
        for (const auto& n : gts.scannedGenericStructNames)          if (endsWithTail(n)) return true;
        for (const auto& n : gts.scannedGenericInterfaceNames)       if (endsWithTail(n)) return true;
        for (const auto& n : gts.scannedGenericStructNamesUncertain) if (endsWithTail(n)) return true;
        return false;
    }

bool LLVMBackend::IsGenericFunctionKeyInNamespace(const std::string& key, const std::string& ns) const
{
        if (gts.genericFunctionTemplates.count(key) == 0) return false;
        auto it = gts.genericTemplateNamespace.find(key);
        return it != gts.genericTemplateNamespace.end() && it->second == ns;
    }

std::string LLVMBackend::ResolveGenericFunctionBase(const std::string& base) const
{
        if (base.empty() || currentNamespace_.empty()) return base;
        if (base.find('.') != std::string::npos) return base;
        std::string prefix = currentNamespace_;
        while (true)
        {
            if (std::string candidate = prefix + "." + base;
                IsGenericFunctionKeyInNamespace(candidate, prefix))
                return candidate;
            auto dot = prefix.rfind('.');
            if (dot == std::string::npos) break;
            prefix = prefix.substr(0, dot);
        }
        return base;
    }

bool LLVMBackend::IsTypeArgTypeKey(const std::string& key) const
{
        if (ResolveTypeAlias(key) != key)
            return true;
        return dataStructures.count(key) != 0
            || HasInterface(key)
            || gts.scannedTypeNames.count(key) != 0;
    }

std::string LLVMBackend::ResolveTypeArgBaseName(const std::string& base) const
{
        if (base.empty() || currentNamespace_.empty()) return base;
        if (base.find('.') != std::string::npos) return base;
        std::string prefix = currentNamespace_;
        while (true)
        {
            if (std::string candidate = prefix + "." + base; IsTypeArgTypeKey(candidate))
                return candidate;
            auto dot = prefix.rfind('.');
            if (dot == std::string::npos) break;
            prefix = prefix.substr(0, dot);
        }
        return base;
    }

bool LLVMBackend::IsWinrtGenericTemplate(const std::string& fullName) const
{
        return winrtGenericTemplates_.count(fullName) != 0;
    }

bool LLVMBackend::IsWinrtGenericBase(const std::string& fullName) const
{
        if (IsWinrtGenericTemplate(fullName)) return true;
        for (const auto& d : winrtConsumedModel_.delegates)
            if (d.fullName == fullName) return !d.genericParams.empty();
        return false;
    }

std::string LLVMBackend::FindUuidAnnotationResolving(const std::string& name) const
{
        std::string cur = name;
        for (int guard = 0; guard < 8; ++guard)
        {
            if (std::string u = GetTypeAnnotationArg(cur, "uuid"); !u.empty()) return u;
            std::string next = ResolveTypeAlias(cur);
            if (next == cur) break;
            cur = next;
        }
        return std::string{};
    }

bool LLVMBackend::IsGenericInterfaceTemplateName(const std::string& name) const
{
        if (gts.genericStructTemplates.count(name) != 0
            || gts.genericClassTemplates.count(name) != 0
            || gts.scannedGenericStructNames.count(name) != 0)
            return false;
        return gts.genericInterfaceTemplates.count(name) != 0
            || gts.scannedGenericInterfaceNames.count(name) != 0;
    }

void LLVMBackend::RevokeGenericInterfaceInstances(const std::string& base)
{
        if (gts.genericInterfaceInstances.empty()) return;
        std::string prefix = base + "__";
        for (auto it = gts.genericInterfaceInstances.begin(); it != gts.genericInterfaceInstances.end(); )
        {
            if (it->rfind(prefix, 0) == 0) it = gts.genericInterfaceInstances.erase(it);
            else ++it;
        }
    }

bool LLVMBackend::IsInterfaceType(const std::string& name) const
{
        return HasInterface(name);
    }

bool LLVMBackend::HasInterface(const std::string& name) const
{
        return FindInterface(name) != nullptr;
    }

const std::vector<LLVMBackend::InterfaceMethod>* LLVMBackend::FindInterface(const std::string& name) const
{
        std::string resolved = ResolveInterfaceName(name);
        auto it = interfaceTable.find(resolved);
        return it == interfaceTable.end() ? nullptr : &it->second;
    }

bool LLVMBackend::HasInterfaceMethod(const std::string& ifaceName, const std::string& methodName) const
{
        const auto* methods = FindInterface(ifaceName);
        if (methods == nullptr) return false;
        for (const auto& m : *methods)
            if (m.Name == methodName) return true;
        return false;
    }

const LLVMBackend::InterfaceMethod* LLVMBackend::FindInterfaceMethod(const std::string& ifaceName,
                                               const std::string& methodName,
                                               size_t argCount) const
{
        const auto* methods = FindInterface(ifaceName);
        if (methods == nullptr) return nullptr;
        const InterfaceMethod* firstByName = nullptr;
        for (const auto& m : *methods)
        {
            if (m.Name != methodName) continue;
            if (firstByName == nullptr) firstByName = &m;
            if (argCount != (size_t)-1 && m.Parameters.size() == argCount) return &m;
        }
        return firstByName;
    }

const std::vector<LLVMBackend::TypeAndValue>* LLVMBackend::GetInterfaceMethodParams(const std::string& ifaceName,
                                                             const std::string& methodName,
                                                             size_t argCount) const
{
        const InterfaceMethod* m = FindInterfaceMethod(ifaceName, methodName, argCount);
        return m != nullptr ? &m->Parameters : nullptr;
    }

const LLVMBackend::TypeAndValue* LLVMBackend::GetInterfaceMethodReturnType(const std::string& ifaceName,
                                                    const std::string& methodName) const
{
        const auto* methods = FindInterface(ifaceName);
        if (methods == nullptr) return nullptr;
        for (const auto& m : *methods)
            if (m.Name == methodName) return &m.ReturnType;
        return nullptr;
    }

llvm::StructType* LLVMBackend::GetFatPtrType() const
{
        const char* fatPtrName = "__iface_fat_ptr";
        if (auto* existing = llvm::StructType::getTypeByName(*context, fatPtrName))
            return existing;
        auto ptrTy = builder->getInt8Ty()->getPointerTo();
        return llvm::StructType::create(*context, { ptrTy, ptrTy }, fatPtrName);
    }

llvm::StructType* LLVMBackend::GetClosureFatPtrType() const
{
        const char* name = "__closure_fat_ptr";
        if (auto* existing = llvm::StructType::getTypeByName(*context, name)) return existing;
        auto ptrTy = builder->getInt8Ty()->getPointerTo();
        return llvm::StructType::create(*context, { ptrTy, ptrTy }, name);
    }

llvm::Function* LLVMBackend::GetOrCreateFunctionShim(llvm::Function* original)
{
        std::string shimName = "__shim_" + original->getName().str();
        if (auto* existing = module->getFunction(shimName)) return existing;

        auto* origTy  = original->getFunctionType();
        auto* i8PtrTy = builder->getInt8Ty()->getPointerTo();

        std::vector<llvm::Type*> shimParamTypes;
        for (auto* paramTy : origTy->params())
            shimParamTypes.push_back(paramTy);
        shimParamTypes.push_back(i8PtrTy); // env (trailing, ignored)

        auto* shimTy = llvm::FunctionType::get(origTy->getReturnType(), shimParamTypes, false);
        auto* shim   = llvm::Function::Create(shimTy, llvm::Function::InternalLinkage, shimName, module.get());

        auto* entry = llvm::BasicBlock::Create(*context, "entry", shim);
        llvm::IRBuilder<> b(entry);

        // Forward every param except the trailing env to the original.
        std::vector<llvm::Value*> callArgs;
        for (unsigned i = 0; i + 1 < shim->arg_size(); ++i)
            callArgs.push_back(shim->getArg(i));

        if (origTy->getReturnType()->isVoidTy())
        {
            b.CreateCall(origTy, original, callArgs);
            b.CreateRetVoid();
        }
        else
        {
            b.CreateRet(b.CreateCall(origTy, original, callArgs));
        }
        return shim;
    }

llvm::Function* LLVMBackend::GetOrCreateCFuncPtrThunk(llvm::FunctionType* cFnTy)
{
        // Build a stable signature key (mangled function type) so we share thunks across
        // identical signatures (e.g. multiple `int(int,int)` callbacks).
        std::string key;
        llvm::raw_string_ostream os(key);
        os << "__c_fnptr_thunk_";
        cFnTy->getReturnType()->print(os);
        for (auto* pt : cFnTy->params()) { os << "_"; pt->print(os); }
        os.flush();
        // LLVM may emit punctuation; sanitize to identifier-friendly chars.
        for (char& c : key) if (!std::isalnum((unsigned char)c)) c = '_';

        if (auto* existing = module->getFunction(key)) return existing;

        auto* i8PtrTy = builder->getInt8Ty()->getPointerTo();
        std::vector<llvm::Type*> thunkParams;
        for (auto* pt : cFnTy->params()) thunkParams.push_back(pt);
        thunkParams.push_back(i8PtrTy); // env (trailing) carries the real C fn ptr at runtime
        auto* thunkTy = llvm::FunctionType::get(cFnTy->getReturnType(), thunkParams, false);

        auto* thunk = llvm::Function::Create(thunkTy, llvm::Function::InternalLinkage, key, *module);

        llvm::IRBuilder<> b(llvm::BasicBlock::Create(*context, "entry", thunk));
        auto* envArg = thunk->getArg((unsigned)thunk->arg_size() - 1);
        auto* fnPtr  = b.CreateBitCast(envArg, cFnTy->getPointerTo(), "cfn");
        std::vector<llvm::Value*> callArgs;
        for (unsigned i = 0; i + 1 < thunk->arg_size(); ++i) callArgs.push_back(thunk->getArg(i));
        if (cFnTy->getReturnType()->isVoidTy())
        {
            b.CreateCall(cFnTy, fnPtr, callArgs);
            b.CreateRetVoid();
        }
        else
        {
            b.CreateRet(b.CreateCall(cFnTy, fnPtr, callArgs));
        }
        return thunk;
    }

llvm::Value* LLVMBackend::WrapCFuncPtrAsFatStruct(llvm::Value* cFnPtrValue, const TypeAndValue& fpTV)
{
        auto* i8PtrTy = builder->getInt8Ty()->getPointerTo();
        // Build the C-side LLVM FunctionType from the TypeAndValue (no env arg).
        std::vector<llvm::Type*> paramTypes;
        for (const auto& p : fpTV.FuncPtrParams)
        {
            TypeAndValue pTV; pTV.TypeName = p.TypeName; pTV.Pointer = p.Pointer;
            paramTypes.push_back(GetType(pTV));
        }
        TypeAndValue retTV;
        retTV.TypeName = fpTV.FuncPtrReturnTypeName;
        retTV.Pointer  = fpTV.FuncPtrReturnPointer;
        auto* cFnTy = llvm::FunctionType::get(GetType(retTV), paramTypes, false);

        auto* thunk    = GetOrCreateCFuncPtrThunk(cFnTy);
        auto* thunkI8  = builder->CreateBitCast(thunk, i8PtrTy, "thunk_i8");
        auto* envI8    = builder->CreateBitCast(cFnPtrValue, i8PtrTy, "cfnret_i8");
        auto* closureTy = GetClosureFatPtrType();
        llvm::Value* fat = llvm::UndefValue::get(closureTy);
        fat = builder->CreateInsertValue(fat, thunkI8, {0u});
        fat = builder->CreateInsertValue(fat, envI8,   {1u});
        return fat;
    }

llvm::Value* LLVMBackend::MakeThinFnPtrValue(llvm::Value* fn, const TypeAndValue& fpTV)
{
        return builder->CreateBitCast(fn, BuildThinFnPtrType(fpTV), "thinfn");
    }

llvm::Value* LLVMBackend::CoerceClosureFatToThin(llvm::Value* fatVal, const TypeAndValue& thinTV)
{
        auto* code = builder->CreateExtractValue(fatVal, {0u}, "clo_code");
        return builder->CreateBitCast(code, BuildThinFnPtrType(thinTV), "thinfn");
    }

bool LLVMBackend::ClosureIsStaticallyNonCapturing(llvm::Value* fatVal)
{
        auto* envField = builder->CreateExtractValue(fatVal, {1u}, "closure_env_probe");
        return llvm::isa<llvm::ConstantPointerNull>(envField);
    }

std::string LLVMBackend::DescribeCapturingClosureToThin(const std::vector<std::string>& captureNames) const
{
        if (captureNames.empty())
            return "a closure that may carry captured state cannot become a C function pointer "
                   "'function<T>'; call .toFunction() instead (it returns the code pointer when the "
                   "closure does not capture, or null when it does), or use Lambda<T> to keep the captures.";
        const size_t count = captureNames.size();
        const size_t shown = count < 5 ? count : 5;
        std::string list;
        for (size_t k = 0; k < shown; k++) { if (k != 0) list += ", "; list += captureNames[k]; }
        std::string more = count > shown ? std::format(", ... (and {} more)", count - shown) : "";
        return std::format(
            "a capturing lambda cannot become a C function pointer 'function<T>': it captured {} {} "
            "[{}{}]. Use Lambda<T> to keep the captures, or call .toFunction() (which returns null when "
            "the closure captures).",
            count, (count == 1 ? "variable" : "variables"), list, more);
    }

llvm::Value* LLVMBackend::EmitFuncToFunctionLowering(llvm::Value* fatVal, const TypeAndValue& thinTV)
{
        auto* code     = builder->CreateExtractValue(fatVal, {0u}, "clo_code");
        auto* env      = builder->CreateExtractValue(fatVal, {1u}, "clo_env");
        auto* thinTy   = llvm::cast<llvm::PointerType>(BuildThinFnPtrType(thinTV));
        auto* codeThin = builder->CreateBitCast(code, thinTy, "thinfn");
        auto* nullThin = llvm::ConstantPointerNull::get(thinTy);
        auto* i8PtrTy  = builder->getInt8Ty()->getPointerTo();
        auto* envNull  = builder->CreateICmpEQ(env, llvm::ConstantPointerNull::get(i8PtrTy), "env_isnull");
        return builder->CreateSelect(envNull, codeThin, nullThin, "tofn");
    }

llvm::Value* LLVMBackend::CoerceToFuncPtrReturn(llvm::Value* val, const TypeAndValue& retTV,
        const NamedVariable& returnNV)
{
        bool valIsStruct = val->getType()->isStructTy();   // fat closure value
        if (retTV.IsThinFnPtr())
        {
            if (auto* fn = llvm::dyn_cast<llvm::Function>(val)) return MakeThinFnPtrValue(fn, retTV);
            if (valIsStruct)
            {
                // Fat closure returned as thin function<T>: only safe when provably non-capturing.
                // A capturing closure has no C-ABI representation, so reject it instead of dropping
                // the env (which yields a thin ptr that reads freed/absent captures when called).
                if (ClosureIsStaticallyNonCapturing(val)) return CoerceClosureFatToThin(val, retTV);
                LogError(DescribeCapturingClosureToThin({}));
                return llvm::UndefValue::get(BuildThinFnPtrType(retTV));
            }
            CheckClosureReturnProvenance(val, returnNV, /*thin=*/true);
            return builder->CreateBitCast(val, BuildThinFnPtrType(retTV), "thinret");
        }
        // Same widening the call paths use, behind the same provenance gate. Fat closure values
        // are not pointer-typed, so the gate below is a no-op for them.
        CheckClosureReturnProvenance(val, returnNV, /*thin=*/false);
        return WidenBareOrThinToClosureFat(val);
    }

llvm::Value* LLVMBackend::WidenThinToFat(llvm::Value* thinPtr)
{
        auto* i8PtrTy   = builder->getInt8Ty()->getPointerTo();
        auto* codeI8    = builder->CreateBitCast(thinPtr, i8PtrTy, "thin_code_i8");
        auto* nullEnv   = llvm::ConstantPointerNull::get(i8PtrTy);
        auto* closureTy = GetClosureFatPtrType();
        llvm::Value* fat = llvm::UndefValue::get(closureTy);
        fat = builder->CreateInsertValue(fat, codeI8,  {0u});
        fat = builder->CreateInsertValue(fat, nullEnv, {1u});
        return fat;
    }

llvm::Value* LLVMBackend::WrapBareValueAsFatStruct(llvm::Function* original)
{
        auto* i8PtrTy  = builder->getInt8Ty()->getPointerTo();
        auto* shim     = GetOrCreateFunctionShim(original);
        auto* shimAsI8 = builder->CreateBitCast(shim, i8PtrTy, "shim_i8");
        auto* nullEnv  = llvm::ConstantPointerNull::get(i8PtrTy);
        auto* closureTy = GetClosureFatPtrType();
        llvm::Value* fat = llvm::UndefValue::get(closureTy);
        fat = builder->CreateInsertValue(fat, shimAsI8, {0u});
        fat = builder->CreateInsertValue(fat, nullEnv,  {1u});
        return fat;
    }

bool LLVMBackend::InterfaceInheritsFrom(const std::string& child, const std::string& parent) const
{
        auto it = interfaceParents.find(child);
        if (it == interfaceParents.end()) return false;
        for (const auto& p : it->second)
        {
            if (p == parent) return true;
            if (InterfaceInheritsFrom(p, parent)) return true;
        }
        return false;
    }

bool LLVMBackend::StructImplementsInterface(const std::string& structName, const std::string& ifaceName) const
{
        // Programs exist in both dataStructures (empty shell) and programTable (real data).
        // Check programTable first so program-declared interfaces are found correctly.
        auto progIt = programTable.find(structName);
        if (progIt != programTable.end())
        {
            for (const auto& iface : progIt->second.Interfaces)
            {
                if (iface == ifaceName) return true;
                if (InterfaceInheritsFrom(iface, ifaceName)) return true;
            }
            return false;
        }
        auto structIt = dataStructures.find(structName);
        if (structIt != dataStructures.end())
        {
            for (const auto& iface : structIt->second.Interfaces)
            {
                if (iface == ifaceName) return true;
                if (InterfaceInheritsFrom(iface, ifaceName)) return true;
            }
        }
        return false;
    }

std::string LLVMBackend::DescribePointerShapedInterfaceSource(const TypeAndValue& src) const
{
        if (src.TypeName.empty()) return "";
        if (src.IsInterface || src.IsInterfacePointer) return "";
        // A simd element keeps its own spelling: TypeName is only the LANE type, so without this
        // a simd<float,4>[2] source reads as 'float[2]'. Wording only - the arms are unchanged.
        std::string elem = src.IsSimd ? std::format("simd<{},{}>", src.TypeName, src.SimdLanes)
                                      : src.TypeName;
        if (src.ElemPointer) return elem + "**";
        if (src.IsArrayView) return elem + "[]";
        if (src.ConstArraySize != 0) return std::format("{}[{}]", elem, src.ConstArraySize);
        if (src.IsSimd) return src.TypeName + " simd vector";
        return "";
    }

std::string LLVMBackend::FormatPointerShapedInterfaceUpcastError(const std::string& shape,
                                                        const std::string& typeName,
                                                        const std::string& interfaceName) const
{
        // A PRIMITIVE element gets its own wording: "index or dereference it first" is useless
        // advice when no value of that element type could ever implement the interface.
        if (IsPrimitiveTypeName(ResolveTypeAlias(typeName)))
            return std::format(
                "cannot convert '{}' to interface '{}' - '{}' is a primitive type and can never "
                "implement an interface",
                shape, interfaceName, typeName);
        return std::format(
            "cannot convert '{}' to interface '{}' - only a single instance pointer '{}*' or a "
            "'{}' value can be boxed into an interface fat pointer; index or dereference it first",
            shape, interfaceName, typeName, typeName);
    }

void LLVMBackend::AppendInterfaceFieldOffsetSlots(const std::string& structName, const std::string& ifaceName,
                                         llvm::StructType* structTy,
                                         const std::vector<DeclTypeAndValue>& structFields,
                                         std::vector<llvm::Constant*>& entries)
{
        const auto* ifields = GetInterfaceFields(ifaceName);
        if (ifields == nullptr || ifields->empty()) return;

        auto ptrTy = builder->getInt8Ty()->getPointerTo();
        const llvm::StructLayout* layout = (structTy != nullptr && !structTy->isOpaque())
            ? module->getDataLayout().getStructLayout(structTy) : nullptr;

        for (const auto& f : *ifields)
        {
            size_t idx = 0;
            for (; idx < structFields.size(); idx++)
                if (structFields[idx].VariableName == f.VariableName) break;

            if (idx >= structFields.size() || layout == nullptr)
            {
                entries.push_back(llvm::ConstantPointerNull::get(ptrTy));
                continue;
            }

            uint64_t offset = layout->getElementOffset((unsigned)idx);
            entries.push_back(llvm::ConstantExpr::getIntToPtr(builder->getInt64(offset), ptrTy));
        }
    }

std::string LLVMBackend::InterfaceFieldTypeText(const TypeAndValue& f)
{
        std::string s = f.TypeName;
        if (f.Pointer) s += "*";
        return s;
    }

std::string LLVMBackend::InterfaceMethodTypeText(const TypeAndValue& tv, const std::string& name)
{
        std::string s;
        if (tv.IsMove)  s += "move ";
        if (tv.IsAlias) s += "alias ";
        if (tv.IsBond)  s += "bond ";
        s += InterfaceFieldTypeText(tv);
        if (!name.empty()) s += " " + name;
        return s;
    }

void LLVMBackend::VerifyInterfaceFields(const std::string& implName, const std::string& ifaceName,
                               const std::vector<DeclTypeAndValue>& implFields)
{
        const auto* ifields = GetInterfaceFields(ifaceName);
        if (ifields == nullptr) return;

        for (const auto& f : *ifields)
        {
            const DeclTypeAndValue* impl = nullptr;
            for (const auto& sf : implFields)
                if (sf.VariableName == f.VariableName) { impl = &sf; break; }

            if (impl == nullptr)
            {
                LogError(std::format(
                    "class '{}' does not implement interface field '{}::{}' (expected type '{}')",
                    implName, ifaceName, f.VariableName, InterfaceFieldTypeText(f)));
                continue;
            }
            if (impl->TypeName != f.TypeName || impl->Pointer != f.Pointer
                || impl->IsInterface != f.IsInterface)
            {
                LogError(std::format(
                    "class '{}' field '{}' has type '{}' but interface '{}' declares it as '{}'",
                    implName, f.VariableName, InterfaceFieldTypeText(*impl), ifaceName,
                    InterfaceFieldTypeText(f)));
            }
            // 'unique' is part of the field contract, not of the field's type: a store through the
            // interface's byte-offset slot reads ownership from the INTERFACE, because dynamic
            // dispatch leaves the concrete type unknown. Both directions of disagreement are heap
            // bugs, so both are rejected - the same uniformity 'push(move T item)' already imposes.
            if (impl->IsUnique != f.IsUnique)
            {
                if (impl->IsUnique)
                    LogError(std::format(
                        "class '{}' field '{}' is declared 'unique' but interface '{}' does not declare "
                        "it 'unique' - a store through the interface's field slot would neither free the "
                        "old pointee (it leaks) nor reject a borrow, which the class's synthesized "
                        "destructor then frees. Declare the field 'unique {} {}' on interface '{}', or "
                        "drop 'unique' from class '{}'.",
                        implName, f.VariableName, ifaceName,
                        InterfaceFieldTypeText(f), f.VariableName, ifaceName, implName));
                else
                    LogError(std::format(
                        "class '{}' field '{}' is not declared 'unique' but interface '{}' declares it "
                        "'unique' - a store through the interface's field slot would free the old pointee, "
                        "which class '{}' does not own. Declare the field 'unique {} {}' on class '{}', or "
                        "drop 'unique' from interface '{}'.",
                        implName, f.VariableName, ifaceName, implName,
                        InterfaceFieldTypeText(f), f.VariableName, implName, ifaceName));
            }
        }
    }

bool LLVMBackend::InterfaceMethodContractConforms(const InterfaceMethod& method, const FunctionSymbol& sym) const
{
        for (size_t i = 0; i < method.Parameters.size(); i++)
        {
            const auto& ip = method.Parameters[i];
            const auto& cp = sym.Parameters[i + 1];   // Parameters[0] is the implicit 'this'
            bool cpMove = cp.IsMove || cp.IsUniqueTypeArg;
            bool ipMove = ip.IsMove || ip.IsUniqueTypeArg;
            if (cpMove != ipMove) return false;
            if (cp.IsBond != ip.IsBond) return false;
            if (cp.IsAlias != ip.IsAlias) return false;
        }
        const auto& ir = method.ReturnType;
        const auto& cr = sym.ReturnType;
        bool crMove = cr.IsMove || cr.IsUniqueTypeArg;
        bool irMove = ir.IsMove || ir.IsUniqueTypeArg;
        if (crMove != irMove) return false;
        if (cr.IsAlias != ir.IsAlias) return false;
        if (cr.IsBond != ir.IsBond) return false;
        return true;
    }

void LLVMBackend::VerifyInterfaceMethodContract(const std::string& implName, const std::string& ifaceName,
                                       const InterfaceMethod& method, const FunctionSymbol& sym)
{
        for (size_t i = 0; i < method.Parameters.size(); i++)
        {
            const auto& ip = method.Parameters[i];
            const auto& cp = sym.Parameters[i + 1];   // Parameters[0] is the implicit 'this'
            std::string pname = !cp.VariableName.empty() ? cp.VariableName
                              : (!ip.VariableName.empty() ? ip.VariableName
                                                          : std::format("#{}", i + 1));
            // Compare sink-ness: a `unique` type argument makes a by-value parameter a move sink
            // just like `move` (ApplyMoveParamTransfer), so the two spellings must agree.
            bool cpMove = cp.IsMove || cp.IsUniqueTypeArg;
            bool ipMove = ip.IsMove || ip.IsUniqueTypeArg;
            if (cpMove != ipMove)
            {
                if (ipMove)
                    LogError(std::format(
                        "class '{}' method '{}': parameter '{}' is not declared 'move' but interface "
                        "'{}' declares it 'move' - a call through the interface transfers ownership of "
                        "the argument, so the caller stops freeing it while class '{}' never takes it "
                        "over (the argument leaks). Declare the parameter '{}' on class '{}', or drop "
                        "'move' from interface '{}'.",
                        implName, method.Name, pname, ifaceName, implName,
                        InterfaceMethodTypeText(ip, pname), implName, ifaceName));
                else
                    LogError(std::format(
                        "class '{}' method '{}': parameter '{}' is declared 'move' but interface '{}' "
                        "does not declare it 'move' - a call through the interface does not transfer "
                        "ownership, so the caller still frees the argument that class '{}' has already "
                        "taken over (a double free). Drop 'move' from class '{}', or declare the "
                        "parameter '{}' on interface '{}'.",
                        implName, method.Name, pname, ifaceName, implName, implName,
                        InterfaceMethodTypeText(cp, pname), ifaceName));
            }
            // 'bond' and 'move' are mutually exclusive, so report only the more severe disagreement.
            else if (cp.IsBond != ip.IsBond)
            {
                LogError(std::format(
                    "class '{}' method '{}': parameter '{}' is declared '{}' but interface '{}' "
                    "declares it '{}' - 'bond' is part of the method's contract, and a call through "
                    "the interface reads the contract from the interface, so the borrow the two "
                    "disagree about would go untracked. Make the two declarations agree.",
                    implName, method.Name, pname, InterfaceMethodTypeText(cp), ifaceName,
                    InterfaceMethodTypeText(ip)));
            }
            else if (cp.IsAlias != ip.IsAlias)
            {
                LogError(std::format(
                    "class '{}' method '{}': parameter '{}' is declared '{}' but interface '{}' "
                    "declares it '{}' - 'alias' makes the parameter a borrow whose destructor is "
                    "suppressed, and a call through the interface reads the contract from the "
                    "interface, so the borrow the two disagree about would go untracked. Make the "
                    "two declarations agree.",
                    implName, method.Name, pname, InterfaceMethodTypeText(cp), ifaceName,
                    InterfaceMethodTypeText(ip)));
            }
        }

        const auto& ir = method.ReturnType;
        const auto& cr = sym.ReturnType;
        // Sink-ness, matching the parameter check: a `unique` type-argument return owns like `move`.
        bool crMove = cr.IsMove || cr.IsUniqueTypeArg;
        bool irMove = ir.IsMove || ir.IsUniqueTypeArg;
        if (crMove != irMove)
        {
            if (irMove)
                LogError(std::format(
                    "class '{}' method '{}': the return type is not declared 'move' but interface "
                    "'{}' declares it '{}' - a call through the interface hands the caller an owned "
                    "value to free, while class '{}' returns one it still owns (a double free). "
                    "Declare the return '{}' on class '{}', or drop 'move' from interface '{}'.",
                    implName, method.Name, ifaceName, InterfaceMethodTypeText(ir), implName,
                    InterfaceMethodTypeText(ir), implName, ifaceName));
            else
                LogError(std::format(
                    "class '{}' method '{}': the return type is declared 'move' but interface '{}' "
                    "declares it '{}' - a call through the interface treats the result as a borrow "
                    "and never frees it, while class '{}' hands over ownership (the result leaks). "
                    "Drop 'move' from class '{}', or declare the return '{}' on interface '{}'.",
                    implName, method.Name, ifaceName, InterfaceMethodTypeText(ir), implName,
                    implName, InterfaceMethodTypeText(cr), ifaceName));
        }
        if (cr.IsAlias != ir.IsAlias)
        {
            if (ir.IsAlias)
                LogError(std::format(
                    "class '{}' method '{}': the return type is not declared 'alias' but interface "
                    "'{}' declares it '{}' - a call through the interface treats the result as a "
                    "borrow and suppresses its destructor, while class '{}' returns a value the "
                    "caller must destroy (the result leaks). Declare the return '{}' on class '{}', "
                    "or drop 'alias' from interface '{}'.",
                    implName, method.Name, ifaceName, InterfaceMethodTypeText(ir), implName,
                    InterfaceMethodTypeText(ir), implName, ifaceName));
            else
                LogError(std::format(
                    "class '{}' method '{}': the return type is declared 'alias' but interface '{}' "
                    "declares it '{}' - a call through the interface destroys the result at scope "
                    "exit, while class '{}' returns a borrow of storage it still owns (a double "
                    "free). Drop 'alias' from class '{}', or declare the return '{}' on interface "
                    "'{}'.",
                    implName, method.Name, ifaceName, InterfaceMethodTypeText(ir), implName,
                    implName, InterfaceMethodTypeText(cr), ifaceName));
        }
        if (cr.IsBond != ir.IsBond)
        {
            LogError(std::format(
                "class '{}' method '{}': the return type is declared '{}' but interface '{}' "
                "declares it '{}' - 'bond' is part of the return contract, and a call through the "
                "interface reads the contract from the interface, so the borrow the two disagree "
                "about would go untracked. Make the two declarations agree.",
                implName, method.Name, InterfaceMethodTypeText(cr), ifaceName,
                InterfaceMethodTypeText(ir)));
        }
    }

/*
 * The vtable slot's implementing function for one concrete type. Shared by both vtable builders
 * and by the interface-dispatch ownership walks, so the "which body does this slot call" answer
 * can never drift between what is emitted and what is analysed.
 */
llvm::Function* LLVMBackend::LookupInterfaceMethodImpl(const std::string& structName,
                                                      const InterfaceMethod& method) const
{
        auto funcIt = functionTable.find(method.Name);
        if (funcIt == functionTable.end()) return nullptr;
        // method.Parameters excludes 'this'; sym.Parameters[0] is 'this'.
        // Match by struct this-pointer and remaining params to distinguish overloads.
        size_t expectedParamCount = 1 + method.Parameters.size();
        llvm::Function* fallback = nullptr;
        for (const auto& sym : funcIt->second)
        {
            if (sym.Parameters.size() != expectedParamCount) continue;
            if (sym.Parameters[0].TypeName != structName || !sym.Parameters[0].Pointer) continue;
            bool paramsMatch = true;
            for (size_t pi = 0; pi < method.Parameters.size(); pi++)
            {
                if (sym.Parameters[1 + pi].TypeName != method.Parameters[pi].TypeName
                    || sym.Parameters[1 + pi].ValuePointerDepth()
                        != method.Parameters[pi].ValuePointerDepth())
                { paramsMatch = false; break; }
            }
            if (!paramsMatch) continue;
            // Bind the overload whose ownership contract matches the interface method, so dispatch
            // honors the interface's move/alias/bond spelling regardless of declaration order.
            if (!fallback) fallback = sym.Function;
            if (InterfaceMethodContractConforms(method, sym)) return sym.Function;
        }
        return fallback;
    }

llvm::GlobalVariable* LLVMBackend::GetOrCreateProgramVTable(ProgramData& pd, const std::string& structName, const std::string& ifaceName)
{
        auto it = pd.VTables.find(ifaceName);
        if (it != pd.VTables.end()) return it->second;

        const auto* ifaceMethods = FindInterface(ifaceName);
        if (ifaceMethods == nullptr)
        {
            LogError(std::format("GetOrCreateProgramVTable: unknown interface '{}'", ifaceName));
            return nullptr;
        }

        auto ptrTy = builder->getInt8Ty()->getPointerTo();
        std::vector<llvm::Constant*> entries;

        if (pd.typeDescriptor == nullptr)
        {
            pd.typeDescriptor = new llvm::GlobalVariable(
                *module, builder->getInt8Ty(), true,
                llvm::GlobalValue::InternalLinkage,
                builder->getInt8(0), structName + "_typedesc");
        }
        entries.push_back(pd.typeDescriptor);

        for (const auto& method : *ifaceMethods)
        {
            llvm::Function* fn = LookupInterfaceMethodImpl(structName, method);
            if (fn == nullptr)
            {
                LogError(std::format("GetOrCreateProgramVTable: '{}' does not implement '{}::{}'", structName, ifaceName, method.Name));
                entries.push_back(llvm::ConstantPointerNull::get(ptrTy));
            }
            else
            {
                entries.push_back(fn);
            }
        }

        AppendInterfaceFieldOffsetSlots(structName, ifaceName, pd.StructType, pd.ConfigFields, entries);

        // Trailing slot: full concrete destructor (or null) for delete-through-interface.
        // Layout is [typedesc, method0..N-1, fieldOff0..M-1, fullDtor].
        if (auto* dtor = GetOrCreateFullDestructor(structName))
            entries.push_back(llvm::ConstantExpr::getBitCast(dtor, ptrTy));
        else
            entries.push_back(llvm::ConstantPointerNull::get(ptrTy));

        auto arrTy = llvm::ArrayType::get(ptrTy, entries.size());
        auto vtableConst = llvm::ConstantArray::get(arrTy, entries);
        auto vtableGlobal = new llvm::GlobalVariable(
            *module, arrTy, true,
            llvm::GlobalValue::InternalLinkage,
            vtableConst, structName + "_" + ifaceName + "_vtable");

        pd.VTables[ifaceName] = vtableGlobal;
        return vtableGlobal;
    }

llvm::GlobalVariable* LLVMBackend::GetOrCreateVTable(const std::string& structName, const std::string& ifaceName)
{
        // Programs have their own VTable cache separate from dataStructures.
        auto progIt = programTable.find(structName);
        if (progIt != programTable.end())
            return GetOrCreateProgramVTable(progIt->second, structName, ifaceName);

        auto& sd = dataStructures[structName];
        auto it = sd.VTables.find(ifaceName);
        if (it != sd.VTables.end()) return it->second;

        const auto* ifaceMethods = FindInterface(ifaceName);
        if (ifaceMethods == nullptr)
        {
            LogError(std::format("GetOrCreateVTable: unknown interface '{}'", ifaceName));
            return nullptr;
        }

        auto ptrTy = builder->getInt8Ty()->getPointerTo();
        std::vector<llvm::Constant*> entries;

        // First entry: pointer to unique per-struct type descriptor global (for 'is'/'as' checks)
        // Lazily create the descriptor here if CreateStructType wasn't called first.
        if (sd.typeDescriptor == nullptr)
        {
            sd.typeDescriptor = new llvm::GlobalVariable(
                *module, builder->getInt8Ty(), true,
                llvm::GlobalValue::InternalLinkage,
                builder->getInt8(0), structName + "_typedesc");
        }
        entries.push_back(sd.typeDescriptor);

        for (const auto& method : *ifaceMethods)
        {
            llvm::Function* fn = LookupInterfaceMethodImpl(structName, method);
            if (fn == nullptr)
            {
                LogError(std::format("GetOrCreateVTable: '{}' does not implement '{}::{}'", structName, ifaceName, method.Name));
                entries.push_back(llvm::ConstantPointerNull::get(ptrTy));
            }
            else
            {
                entries.push_back(fn);
            }
        }

        AppendInterfaceFieldOffsetSlots(structName, ifaceName, sd.StructType, sd.StructFields, entries);

        // Trailing slot: full concrete destructor (or null) for delete-through-interface.
        // Layout is [typedesc, method0..N-1, fieldOff0..M-1, fullDtor].
        if (auto* dtor = GetOrCreateFullDestructor(structName))
            entries.push_back(llvm::ConstantExpr::getBitCast(dtor, ptrTy));
        else
            entries.push_back(llvm::ConstantPointerNull::get(ptrTy));

        auto arrTy = llvm::ArrayType::get(ptrTy, entries.size());
        auto vtableConst = llvm::ConstantArray::get(arrTy, entries);
        auto vtableGlobal = new llvm::GlobalVariable(
            *module,
            arrTy,
            true,
            llvm::GlobalValue::InternalLinkage,
            vtableConst,
            structName + "_" + ifaceName + "_vtable"
        );

        sd.VTables[ifaceName] = vtableGlobal;
        return vtableGlobal;
    }

llvm::Value* LLVMBackend::BuildInterfaceFatValue(llvm::GlobalVariable* vtable, llvm::Value* dataPtr)
{
        auto fatTy = GetFatPtrType();
        auto ptrTy = builder->getInt8Ty()->getPointerTo();
        llvm::Value* v = llvm::UndefValue::get(fatTy);
        v = builder->CreateInsertValue(v, builder->CreateBitCast(vtable, ptrTy), { 0u });
        v = builder->CreateInsertValue(v, builder->CreateBitCast(dataPtr, ptrTy), { 1u });
        return v;
    }

llvm::Value* LLVMBackend::CoerceArgToInterface(const NamedVariable& arg, llvm::Value* val,
                                      const std::string& ifaceName, const std::string& calleeDesc)
{
        if (val == nullptr || ifaceName.empty()) return val;
        // Same hole as ReboxInterfaceIfNeeded: an unrouted name as the PARAMETER type would skip
        // boxing entirely and drop a raw struct into a fat-pointer slot.
        RecordInterfaceMaterialization(ifaceName, "an argument type");
        if (!IsInterfaceType(ifaceName)) return val;
        if (arg.TypeAndValue.IsInterface)
            return ReboxInterfaceIfNeeded(val, arg.TypeAndValue.TypeName, ifaceName);
        if (val->getType() == GetFatPtrType()) return val;   // already a fat pointer

        std::string structName = arg.TypeAndValue.TypeName;
        if (structName.empty() && arg.BaseType)
            if (auto* st = llvm::dyn_cast<llvm::StructType>(arg.BaseType))
                structName = st->getName().str();

        if (structName.empty() || !StructImplementsInterface(structName, ifaceName))
        {
            LogError(std::format(
                "cannot pass '{}' as interface parameter '{}' of {} - it does not implement '{}'",
                structName.empty() ? "<unknown>" : structName, ifaceName, calleeDesc, ifaceName));
            return val;
        }

        auto* vtable = GetOrCreateVTable(structName, ifaceName);
        llvm::Value* dataPtr = nullptr;
        if (arg.TypeAndValue.Pointer)
            dataPtr = arg.Primary != nullptr ? arg.Primary : LoadArgStorage(arg);
        else if (arg.Storage != nullptr)
            dataPtr = arg.Storage;
        else
        {
            auto* structTy = arg.BaseType ? arg.BaseType : GetType(arg.TypeAndValue);
            if (structTy == nullptr || structTy->isVoidTy() || arg.Primary == nullptr)
            {
                LogError(std::format(
                    "argument for interface parameter '{}' of {} has no resolved storage",
                    ifaceName, calleeDesc));
                return val;
            }
            auto* tempAlloca = AllocaAtEntry(structTy, nullptr);
            builder->CreateStore(arg.Primary, tempAlloca);
            dataPtr = tempAlloca;
        }
        return BuildInterfaceFatValue(vtable, dataPtr);
    }

void LLVMBackend::RecordScannedStructInterfaces(const std::string& structName, std::vector<std::string> ifaceNames)
{
        if (structName.empty() || ifaceNames.empty()) return;
        scannedInterfaceImpls[structName] = std::move(ifaceNames);
    }

void LLVMBackend::RecordUncertainInterfaceImpl(const std::string& ifaceName)
{
        if (!ifaceName.empty()) uncertainInterfaceImpls.insert(ifaceName);
    }

void LLVMBackend::RecordIfConstGuardedInterfaceImpl(const std::string& className,
                                           std::vector<std::string> guardChain, const void* node,
                                           std::vector<std::vector<std::string>> ifaceCandidates)
{
        if (className.empty() || ifaceCandidates.empty()) return;
        ifConstGuardedImpls_.push_back(IfConstGuardedImpl{
            className, std::move(guardChain), node, std::move(ifaceCandidates) });
    }

void LLVMBackend::RetractIfConstGuardedInterfaceImpl(const void* node)
{
        if (node == nullptr) return;
        std::erase_if(ifConstGuardedImpls_,
                      [node](const IfConstGuardedImpl& g) { return g.Node == node; });
    }

void LLVMBackend::PeelIfConstGuardedInterfaceImpl(const void* node)
{
        if (node == nullptr) return;
        for (auto& g : ifConstGuardedImpls_)
            if (g.Node == node && !g.GuardChain.empty())
                g.GuardChain.erase(g.GuardChain.begin());
    }

std::string LLVMBackend::ResolveGuardedBaseCandidate(const std::vector<std::string>& candidates) const
{
        for (const auto& raw : candidates)
        {
            std::string resolved = ResolveTypeAlias(raw);
            if (HasInterface(resolved)) return ResolveInterfaceName(resolved);
        }
        return {};
    }

bool LLVMBackend::FindIfConstGuardedImplementor(const std::string& ifaceName, std::string& classOut,
                                       std::vector<std::string>& guardChainOut,
                                       size_t& countOut) const
{
        std::string want = ResolveTypeAlias(ifaceName);
        std::unordered_set<std::string> seen;
        countOut = 0;
        for (const auto& guarded : ifConstGuardedImpls_)
        {
            bool provides = false;
            for (const auto& candidates : guarded.InterfaceCandidates)
            {
                // Blame rests on a FACT, so only the resolved name may be tested. A surplus
                // candidate here would fabricate an implements-claim the class never made.
                std::string declared = ResolveGuardedBaseCandidate(candidates);
                if (declared.empty()) continue;
                if (declared == want || InterfaceInheritsFrom(declared, want)) { provides = true; break; }
            }
            if (!provides) continue;
            if (!seen.insert(guarded.ClassName).second) continue;
            if (countOut == 0) { classOut = guarded.ClassName; guardChainOut = guarded.GuardChain; }
            countOut++;
        }
        return countOut > 0;
    }

bool LLVMBackend::InterfaceImplementorSetIsUncertain(const std::string& ifaceName) const
{
        if (importCompileDepth_ > 0) return true;
        if (uncertainInterfaceImpls.count(ifaceName) > 0) return true;
        // A monomorphized name (IFoo__int) inherits its template's uncertainty.
        if (size_t p = ifaceName.find("__"); p != std::string::npos)
            return uncertainInterfaceImpls.count(ifaceName.substr(0, p)) > 0;
        return false;
    }

bool LLVMBackend::TypeMayProvideInterface(const std::string& typeName, const std::string& ifaceName) const
{
        if (StructImplementsInterface(typeName, ifaceName)) return true;
        auto it = scannedInterfaceImpls.find(typeName);
        if (it == scannedInterfaceImpls.end()) return false;
        // Exact names only - no trailing-component heuristic. A base clause is a bare Identifier in
        // the grammar and a namespaced interface still registers unqualified, so both sides are
        // already bare; a `using` alias is resolved, which the scanner could not do yet.
        std::string want = ResolveTypeAlias(ifaceName);
        for (const auto& declaredRaw : it->second)
        {
            std::string declared = ResolveTypeAlias(declaredRaw);
            if (declared == want || InterfaceInheritsFrom(declared, want)) return true;
        }
        return false;
    }

bool LLVMBackend::AnyTypeMayProvideInterface(const std::string& ifaceName) const
{
        for (const auto& [name, sd] : dataStructures)
            if (TypeMayProvideInterface(name, ifaceName)) return true;
        for (const auto& [name, pd] : programTable)
            if (TypeMayProvideInterface(name, ifaceName)) return true;
        for (const auto& [name, ifaces] : scannedInterfaceImpls)
            if (TypeMayProvideInterface(name, ifaceName)) return true;
        return false;
    }

/*
 * The CLOSED WORLD of an interface: every registered type that may provide it. False means the
 * set cannot be trusted (still inside an import, or an uncertain template), which every caller
 * must read as "no proof" - the unknown-accepts polarity the ownership guards are built on.
 */
bool LLVMBackend::EnumerateInterfaceImplementors(const std::string& ifaceNameIn,
                                                 std::vector<std::string>& out) const
{
        std::string ifaceName = ResolveTypeAlias(ifaceNameIn);
        if (ifaceName.empty() || !IsInterfaceType(ifaceName)) return false;
        if (InterfaceImplementorSetIsUncertain(ifaceName)) return false;
        std::unordered_set<std::string> seen;
        auto consider = [&](const std::string& name)
        {
            if (!TypeMayProvideInterface(name, ifaceName)) return;
            if (seen.insert(name).second) out.push_back(name);
        };
        for (const auto& [name, sd] : dataStructures)            consider(name);
        for (const auto& [name, pd] : programTable)              consider(name);
        for (const auto& [name, ifaces] : scannedInterfaceImpls) consider(name);
        // Deterministic order: the diagnostic below names implementors, and a map walk is not.
        std::sort(out.begin(), out.end());
        return !out.empty();
    }

bool LLVMBackend::InterfaceConversionIsProvablyImpossible(const std::string& srcIfaceIn,
                                                 const std::string& dstIfaceIn) const
{
        std::string srcIface = ResolveTypeAlias(srcIfaceIn);
        std::string dstIface = ResolveTypeAlias(dstIfaceIn);
        if (srcIface.empty() || dstIface.empty() || srcIface == dstIface) return false;
        if (!IsInterfaceType(srcIface) || !IsInterfaceType(dstIface)) return false;
        if (InterfaceInheritsFrom(srcIface, dstIface) || InterfaceInheritsFrom(dstIface, srcIface))
            return false;
        if (InterfaceImplementorSetIsUncertain(srcIface) || InterfaceImplementorSetIsUncertain(dstIface))
            return false;

        bool sawSourceImplementor = false;
        auto consider = [&](const std::string& name) -> bool
        {
            if (!TypeMayProvideInterface(name, srcIface)) return true;
            sawSourceImplementor = true;
            return !TypeMayProvideInterface(name, dstIface);
        };
        for (const auto& [name, sd] : dataStructures)         if (!consider(name)) return false;
        for (const auto& [name, pd] : programTable)           if (!consider(name)) return false;
        for (const auto& [name, ifaces] : scannedInterfaceImpls) if (!consider(name)) return false;
        // No implementor at all means the registry cannot answer the question - stay silent.
        return sawSourceImplementor;
    }

void LLVMBackend::RecordInterfaceMaterialization(const std::string& nameIn, const std::string& role)
{
        // Cheapest possible early-out first: no routed instance anywhere means nothing to record,
        // and this runs on every local / global / field / parameter / argument.
        if (gts.genericInterfaceInstances.empty() || nameIn.empty()) return;
        std::string name = ResolveTypeAlias(nameIn);
        // Only a name the routing actually treats as a fat pointer can be the bug state.
        if (name.empty() || gts.genericInterfaceInstances.count(name) == 0) return;
        gts.materializedInterfaceUses.push_back(
            { name, sourceFileName, currentLine, currentColumn, role });
    }

void LLVMBackend::ResolveMaterializedInterfaceUses()
{
        if (gts.materializedInterfaceUses.empty()) return;
        auto uses = std::move(gts.materializedInterfaceUses);
        gts.materializedInterfaceUses.clear();
        // Deterministic order. NOTE: this is file/line/column of the RECORDING site, which for a
        // struct field is the field's declaration - so several fields of one template share a line
        // and the order among them is the order they were laid out, not textual "source order".
        std::stable_sort(uses.begin(), uses.end(),
            [](const GenericTemplateState::MaterializedInterfaceUse& a,
               const GenericTemplateState::MaterializedInterfaceUse& b)
            {
                if (a.File != b.File) return a.File < b.File;
                if (a.Line != b.Line) return a.Line < b.Line;
                return a.Column < b.Column;
            });

        std::vector<const GenericTemplateState::MaterializedInterfaceUse*> offenders;
        std::unordered_set<std::string> seen;
        bool anyIfConst = false;
        for (const auto& u : uses)
        {
            if (HasInterface(u.MangledName)) continue;
            std::string key = u.MangledName + "\x1f" + u.Role + "\x1f" + u.File
                            + "\x1f" + std::to_string(u.Line) + ":" + std::to_string(u.Column);
            if (!seen.insert(key).second) continue;
            offenders.push_back(&u);
            if (gts.ifConstUncertainInterfaceNames.count(TemplateBaseOfMangledName(u.MangledName)))
                anyIfConst = true;
        }
        if (offenders.empty()) return;

        // Name every offender in the body; the caret goes on the first one.
        const auto& first = *offenders.front();
        std::string body = std::format(
            "generic interface '{}' was never instantiated, so it has no method table and cannot "
            "carry a value - it is used here as {}.", first.MangledName, first.Role);
        for (size_t i = 1; i < offenders.size(); i++)
        {
            const auto& u = *offenders[i];
            body += std::format("\n  also: '{}' as {} at {}({},{})",
                                u.MangledName, u.Role, u.File, u.Line, u.Column);
        }
        if (anyIfConst)
            body += "\n  One or more of these interfaces is declared inside an 'if const' whose "
                    "condition this pass could not evaluate; if that branch is not taken for this "
                    "target, move the declaration outside the 'if const' or guard the uses with the "
                    "same condition.";

        // Point the caret at the first offender. LogError does not return (exit(1) or throw), so
        // this location is never restored - nothing runs after it.
        sourceFileName = first.File;
        currentLine = first.Line;
        currentColumn = first.Column;
        LogError(body);
    }

std::string LLVMBackend::TemplateBaseOfMangledName(const std::string& mangled)
{
        size_t pos = mangled.find("__");
        return pos == std::string::npos ? mangled : mangled.substr(0, pos);
    }

bool LLVMBackend::MangledGenericNameIsAmbiguous(const std::string& mangled) const
{
        size_t d = mangled.find("__");
        if (d == std::string::npos) return false;

        auto segmentNamesATemplate = [&](const std::string& seg)
            {
                if (seg.empty()) return false;
                if (IsGenericTemplateKey(seg)) return true;
                std::string tail = "." + seg;
                for (const auto& kv : gts.genericStructTemplates)
                    if (kv.first.ends_with(tail)) return true;
                for (const auto& kv : gts.genericClassTemplates)
                    if (kv.first.ends_with(tail)) return true;
                for (const auto& kv : gts.genericInterfaceTemplates)
                    if (kv.first.ends_with(tail)) return true;
                for (const auto& n : gts.scannedGenericStructNames)
                    if (n.ends_with(tail)) return true;
                for (const auto& n : gts.scannedGenericInterfaceNames)
                    if (n.ends_with(tail)) return true;
                return false;
            };

        std::string args = mangled.substr(d + 2);
        for (size_t pos = 0; pos <= args.size(); )
        {
            size_t sep = args.find("__", pos);
            std::string seg = (sep == std::string::npos) ? args.substr(pos) : args.substr(pos, sep - pos);
            if (segmentNamesATemplate(seg)) return true;
            if (sep == std::string::npos) break;
            pos = sep + 2;
        }
        return false;
    }

std::string LLVMBackend::DisplayNameOfMangledType(const std::string& mangled, bool* writable) const
{
        if (writable) *writable = true;

        if (auto it = mangledTypeDisplayNames.find(mangled); it != mangledTypeDisplayNames.end())
            return it->second;

        size_t d = mangled.find("__");
        if (d == std::string::npos)
            return mangled;

        if (MangledGenericNameIsAmbiguous(mangled))
        {
            if (writable) *writable = false;
            return mangled;
        }

        std::string args = mangled.substr(d + 2);
        std::string joined;
        for (size_t pos = 0; pos <= args.size(); )
        {
            size_t sep = args.find("__", pos);
            std::string seg = (sep == std::string::npos) ? args.substr(pos) : args.substr(pos, sep - pos);
            // An EMPTY segment means the argument itself began with "__" - an encoded closure type
            // ("Box____fatfn_1_3_i32_3_i32"). Splitting it gives "Box<, fatfn_...>", a hybrid that
            // names no type. A SINGLE such argument can be spelled back exactly; anything else is
            // not writable source and the raw mangled name is handed back.
            if (seg.empty())
            {
                if (joined.empty())
                    if (const TypeAndValue* enc = GetEncodedClosureType(args))
                        return mangled.substr(0, d) + "<" + SpellEncodedClosureType(*enc) + ">";
                if (writable) *writable = false;
                return mangled;
            }
            if (sep == std::string::npos) { joined += seg; break; }
            joined += seg + ", ";
            pos = sep + 2;
        }
        return mangled.substr(0, d) + "<" + joined + ">";
    }

llvm::Value* LLVMBackend::ReboxInterfaceIfNeeded(llvm::Value* fatVal, const std::string& srcIface,
                                        const std::string& dstIface)
{
        if (fatVal == nullptr || fatVal->getType() != GetFatPtrType() || dstIface.empty()) return fatVal;
        if (srcIface == kAmbiguousFatInterface) return RebuildInterfaceFatValue(fatVal, dstIface);
        if (srcIface.empty() || srcIface == dstIface) return fatVal;
        // Recorded before the IsInterfaceType early-out below silently passes the value through
        // unconverted: an unrouted name on either side is the vtable-laundering state.
        RecordInterfaceMaterialization(srcIface, "the source of an interface conversion");
        RecordInterfaceMaterialization(dstIface, "the target of an interface conversion");
        if (!IsInterfaceType(srcIface) || !IsInterfaceType(dstIface)) return fatVal;
        // A rebuild with no possible typedesc match would silently yield a null vtable that the
        // next method call dispatches through. Reject it here, where both names are still known.
        if (InterfaceConversionIsProvablyImpossible(srcIface, dstIface))
        {
            LogError(std::format(
                "cannot convert interface '{}' to interface '{}' - no class implementing '{}' "
                "implements '{}'", srcIface, dstIface, srcIface, dstIface));
            return fatVal;
        }
        return RebuildInterfaceFatValue(fatVal, dstIface);
    }

llvm::Value* LLVMBackend::RebuildInterfaceFatValue(llvm::Value* fatVal, const std::string& dstIface)
{
        llvm::Function* thunk = GetOrCreateInterfaceReboxThunk(dstIface);
        return builder->CreateCall(thunk->getFunctionType(), thunk, { fatVal }, "up_rebox");
    }

llvm::Function* LLVMBackend::GetOrCreateInterfaceReboxThunk(const std::string& dstIface)
{
        // Capture the site's context now: at finalization importCompileDepth_ is 0 and the
        // scanner's uncertainty registry no longer describes what was knowable here.
        InterfaceReboxSite site{ sourceFileName, currentLine, currentColumn,
                                 importCompileDepth_ > 0,
                                 InterfaceImplementorSetIsUncertain(dstIface) };

        if (auto it = deferredIfaceReboxIndex_.find(dstIface); it != deferredIfaceReboxIndex_.end())
        {
            deferredIfaceRebox_[it->second].Sites.push_back(std::move(site));
            return deferredIfaceRebox_[it->second].Thunk;
        }
        auto* fatTy = GetFatPtrType();
        auto* fnTy = llvm::FunctionType::get(fatTy, { fatTy }, false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                          std::string(kInterfaceReboxPrefix) + dstIface, *module);
        fn->arg_begin()->setName("src");
        deferredIfaceReboxIndex_[dstIface] = deferredIfaceRebox_.size();
        deferredIfaceRebox_.push_back(DeferredInterfaceRebox{ dstIface, fn, { std::move(site) } });
        // Backstop: a bodyless internal function fails the verifier, so a late thunk is filled
        // in on the spot. It fires MID-BODY, so it needs the full builder bracket.
        if (deferredIfaceReboxDrained_)
        {
            FullBuilderStateScope outer(this);
            InterfaceReboxEmitScope park(this);
            EmitInterfaceReboxBody(deferredIfaceRebox_.back());
        }
        return fn;
    }

void LLVMBackend::AdoptInterfaceReboxThunksFromModule()
{
        for (llvm::Function& fn : module->functions())
        {
            if (!fn.empty()) continue;
            llvm::StringRef name = fn.getName();
            llvm::StringRef prefix(kInterfaceReboxPrefix.data(), kInterfaceReboxPrefix.size());
            if (!name.starts_with(prefix)) continue;
            std::string dstIface = name.substr(prefix.size()).str();
            if (deferredIfaceReboxIndex_.count(dstIface) > 0) continue;
            // The real site is in the core library this cache was built from, and is long gone.
            // Record it as an import site so the diagnostic never reports an empty location.
            InterfaceReboxSite site{ std::string(kCoreBitcodeCacheOrigin), 0, 0, true, true };
            deferredIfaceReboxIndex_[dstIface] = deferredIfaceRebox_.size();
            deferredIfaceRebox_.push_back(
                DeferredInterfaceRebox{ dstIface, &fn, { std::move(site) } });
        }
    }

void LLVMBackend::EmitDeferredInterfaceReboxBodies()
{
        // Set BEFORE the early return: from here on any new thunk must be filled in eagerly.
        deferredIfaceReboxDrained_ = true;
        if (deferredIfaceRebox_.empty()) return;
        // The bodies drive the member builder (CreateBasicBlock/SwitchToBlock/AllocaAtEntry all
        // key off it), so park the walk's insert point, function and debug scope around them.
        InterfaceReboxEmitScope park(this);
        // Index-based and re-reading size(): a nested get-or-create may append while this runs,
        // and draining to a fixed point is required - a bodyless internal function fails verify.
        for (size_t i = 0; i < deferredIfaceRebox_.size(); i++)
            EmitInterfaceReboxBody(deferredIfaceRebox_[i]);
    }

void LLVMBackend::EmitInterfaceReboxBody(DeferredInterfaceRebox site)
{
        llvm::Function* fn = site.Thunk;
        if (!fn->empty()) return;
        const std::string& dstIface = site.DstInterface;

        auto fatTy = GetFatPtrType();
        auto ptrTy = builder->getInt8Ty()->getPointerTo();

        currentFunction = fn;
        SwitchToBlock(llvm::BasicBlock::Create(*context, "entry", fn));

        llvm::Value* src = &*fn->arg_begin();
        llvm::Value* dataPtr   = builder->CreateExtractValue(src, { 1u }, "up_data");
        llvm::Value* vtablePtr = builder->CreateExtractValue(src, { 0u }, "up_vtable");

        auto* resultAlloca = CreateAlloca(fatTy);
        builder->CreateStore(llvm::ConstantAggregateZero::get(fatTy), resultAlloca);
        auto* afterBlock = CreateBasicBlock("upcast_after");

        // A null (failed-cast) source has no vtable to read a typedesc from: stay zeroed.
        auto* liveBlock = CreateBasicBlock("upcast_live");
        builder->CreateCondBr(
            builder->CreateICmpEQ(vtablePtr, llvm::ConstantPointerNull::get(ptrTy)), afterBlock, liveBlock);
        SwitchToBlock(liveBlock);

        llvm::Value* loadedDesc = builder->CreateLoad(ptrTy,
            builder->CreateGEP(ptrTy, vtablePtr, builder->getInt32(0)), "up_typedesc");

        auto emitCase = [&](const std::string& implName, llvm::GlobalVariable* typeDesc)
        {
            auto* matchBlock = CreateBasicBlock("upcast_match");
            auto* nextBlock  = CreateBasicBlock("upcast_next");
            builder->CreateCondBr(builder->CreateICmpEQ(loadedDesc, typeDesc), matchBlock, nextBlock);
            SwitchToBlock(matchBlock);
            builder->CreateStore(BuildInterfaceFatValue(GetOrCreateVTable(implName, dstIface), dataPtr), resultAlloca);
            builder->CreateBr(afterBlock);
            SwitchToBlock(nextBlock);
        };

        // Snapshot the implementor names first: GetOrCreateVTable inside emitCase can register
        // further types and invalidate an iterator over dataStructures / programTable.
        std::vector<std::pair<std::string, llvm::GlobalVariable*>> impls;
        for (auto& [sName, sd] : dataStructures)
            if (sd.typeDescriptor && StructImplementsInterface(sName, dstIface))
                impls.emplace_back(sName, sd.typeDescriptor);
        for (auto& [pName, pd] : programTable)
            if (pd.typeDescriptor && StructImplementsInterface(pName, dstIface))
                impls.emplace_back(pName, pd.typeDescriptor);
        for (const auto& [implName, typeDesc] : impls)
            emitCase(implName, typeDesc);

        builder->CreateBr(afterBlock);
        SwitchToBlock(afterBlock);
        builder->CreateRet(builder->CreateLoad(fatTy, resultAlloca));

        ReportInterfaceReboxHasNoImplementor(site, impls.empty());
    }

void LLVMBackend::ReportInterfaceReboxHasNoImplementor(const DeferredInterfaceRebox& site, bool noCases)
{
        if (!noCases) return;
        const InterfaceReboxSite* reportAt = nullptr;
        for (const auto& s : site.Sites)
        {
            // A site inside an import is never blamed: it is dead code in a library the user
            // does not own, and the location would point into a file they cannot edit.
            if (s.InImport) continue;
            // A site that could not settle the implementor set - a generic implementor that was
            // never monomorphized, an `if const` base clause - proves nothing.
            if (s.Uncertain) return;
            if (reportAt == nullptr) reportAt = &s;
        }
        // Every site was inside an import (or the thunk was adopted from the bitcode cache):
        // stay silent, matching the importCompileDepth_ suppression this check used to have.
        if (reportAt == nullptr) return;
        // The scanner registry may still know a type that supplies the interface even though
        // nothing was monomorphized from it.
        if (AnyTypeMayProvideInterface(site.DstInterface)) return;

        // Caller parks these (InterfaceReboxEmitScope); LogError has no parse context here.
        sourceFileName = reportAt->File;
        currentLine = reportAt->Line;
        currentColumn = reportAt->Column;

        // Same rejection either way - only the wording differs. A class guarded by an `if const`
        // arm this build did not take is absent for a reason the user can act on, so name it.
        std::string guardedClass;
        std::vector<std::string> guardChain;
        size_t guardedCount = 0;
        if (FindIfConstGuardedImplementor(site.DstInterface, guardedClass, guardChain, guardedCount))
        {
            // Chain[0] is the arm this build did not take; the rest is where the class sits INSIDE
            // it - stated as position, never as another takenness claim, since those are unknown.
            std::string guard = guardChain.empty() ? "an 'if const' branch" : guardChain.front();
            std::string nest;
            for (size_t i = 1; i < guardChain.size(); i++)
                nest += (i == 1 ? "" : ", then ") + guardChain[i];
            guard = TruncateDiagnosticText(guard, kIfConstConditionTextLimit);
            nest = TruncateDiagnosticText(nest, kIfConstConditionTextLimit);
            if (guardedCount == 1)
                LogError(std::format(
                    "cannot convert to interface '{}' - the only class implementing it, '{}', is "
                    "declared inside {} that is not taken in this build{}",
                    site.DstInterface, guardedClass, guard,
                    nest.empty() ? "" : std::format(", further nested inside {}", nest)));
            else
                LogError(std::format(
                    "cannot convert to interface '{}' - all {} classes implementing it are declared "
                    "inside 'if const' arms that are not taken in this build (for example '{}', "
                    "declared inside {}{})",
                    site.DstInterface, guardedCount, guardedClass, guard,
                    nest.empty() ? "" : std::format(", further nested inside {}", nest)));
            return;
        }
        LogError(std::format(
            "cannot convert to interface '{}' - no class implements it",
            site.DstInterface));
    }
