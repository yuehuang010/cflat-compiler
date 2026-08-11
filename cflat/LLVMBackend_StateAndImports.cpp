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

// ---- Definitions moved out of LLVMBackend.h (StateAndImports) ----

std::string LLVMBackend::LlvmTypeToTypeName(llvm::Type* t) const
{
        return LlvmTypeToTypeAndValue(t).TypeName;
    }

LLVMBackend::TypeAndValue LLVMBackend::LlvmTypeToTypeAndValue(llvm::Type* t) const
{
        TypeAndValue tv;
        if (!t) { tv.TypeName = "void"; return tv; }
        if (t->isVoidTy())              { tv.TypeName = "void"; return tv; }
        if (t->isIntegerTy(1))          { tv.TypeName = "bool"; return tv; }
        if (t->isIntegerTy(8))          { tv.TypeName = "i8";   return tv; }
        if (t->isIntegerTy(16))         { tv.TypeName = "i16";  return tv; }
        if (t->isIntegerTy(32))         { tv.TypeName = "int";  return tv; }
        if (t->isIntegerTy(64))         { tv.TypeName = "i64";  return tv; }
        if (t->isFloatTy())             { tv.TypeName = "float";  return tv; }
        if (t->isDoubleTy())            { tv.TypeName = "double"; return tv; }
        if (t->isPointerTy())
        {
            tv.TypeName = "i8";  // opaque pointer in modern LLVM; pointee is unknown here
            tv.Pointer  = true;
            return tv;
        }
        if (t->isStructTy())
        {
            auto* st = llvm::cast<llvm::StructType>(t);
            if (st->hasName())
            {
                tv.TypeName = st->getName().str();
                return tv;
            }
        }
        tv.TypeName = "i64";
        return tv;
    }

llvm::Function* LLVMBackend::FinalizeAutoReturnFunction(
        const std::string& functionName,
        llvm::Function* oldFn,
        std::vector<AutoReturnSite>& sites,
        std::vector<TypeAndValue> arguments,
        bool varargs,
        bool returnsOwned,
        bool isMethod)
{
        if (sites.empty())
        {
            LogError(std::format("'auto' return: function '{}' has no return statement; explicit return required for type inference", functionName));
            return oldFn;
        }

        // Unify return value types. v1 rule: identical types accepted; otherwise
        // pick the strictly-wider one if CompareUpconvert says it widens. No
        // bidirectional promotion (no int+long -> long folding yet); reject with
        // a clear message so the user knows to cast at the source level.
        llvm::Type* unifiedTy = sites[0].Value ? sites[0].Value->getType() : builder->getVoidTy();
        for (size_t i = 1; i < sites.size(); i++)
        {
            llvm::Type* siteTy = sites[i].Value ? sites[i].Value->getType() : builder->getVoidTy();
            if (siteTy == unifiedTy) continue;
            // Both must be non-void (cannot mix 'return;' with 'return expr;').
            if (siteTy->isVoidTy() || unifiedTy->isVoidTy())
            {
                LogError(std::format("'auto' return: cannot mix 'return;' and 'return <expr>;' in function '{}'", functionName));
                return oldFn;
            }
            int srcToCur = CompareUpconvert(siteTy, unifiedTy);   // widens to current?
            int curToSrc = CompareUpconvert(unifiedTy, siteTy);   // current widens to new?
            if (srcToCur > 0)      { /* keep unifiedTy, siteTy widens up */ }
            else if (curToSrc > 0) { unifiedTy = siteTy; }
            else
            {
                LogError(std::format("'auto' return: cannot unify return types in function '{}'", functionName));
                return oldFn;
            }
        }

        // Build the new function type with the same params and the unified return.
        std::vector<llvm::Type*> paramTypes(oldFn->getFunctionType()->params().begin(),
                                            oldFn->getFunctionType()->params().end());
        auto* newFnTy = llvm::FunctionType::get(unifiedTy, paramTypes, varargs);

        TypeAndValue newReturnType = LlvmTypeToTypeAndValue(unifiedTy);
        std::string newMangledName = ComputeMangledName(functionName, newReturnType, arguments, varargs);

        // If a function with the new mangled name already exists (e.g. another
        // instantiation of the same template hit the same inferred type), reuse it.
        if (auto* existing = module->getFunction(newMangledName); existing && !existing->empty())
        {
            // Discard the placeholder; the existing definition wins.
            if (!oldFn->use_empty())
                LogError(std::format("'auto' return: recursive call in function '{}' is not yet supported", functionName));
            ForgetFunctionEscapeMemo(oldFn);
            oldFn->eraseFromParent();
            return existing;
        }

        auto* newFn = llvm::Function::Create(newFnTy, llvm::Function::ExternalLinkage, newMangledName, *module);
        newFn->addFnAttr(llvm::Attribute::NullPointerIsValid);
        newFn->setCallingConv(oldFn->getCallingConv());

        // Splice basic blocks from old to new and remap argument uses.
        std::vector<llvm::BasicBlock*> bbs;
        for (auto& bb : *oldFn) bbs.push_back(&bb);
        for (auto* bb : bbs)
        {
            bb->removeFromParent();
            bb->insertInto(newFn);
        }
        // Remap argument uses by value pointer. Do NOT setName on the new args:
        // the spliced entry block already contains allocas named after the params
        // (e.g. "a"), which would collide and trigger LLVM auto-suffixing in a way
        // that misaligns subsequent load/store operands relative to their displayed
        // names. Arg display becomes %0, %1, ... which is fine for IR validity.
        auto oldArgIt = oldFn->arg_begin();
        auto newArgIt = newFn->arg_begin();
        for (; oldArgIt != oldFn->arg_end() && newArgIt != newFn->arg_end(); ++oldArgIt, ++newArgIt)
        {
            oldArgIt->replaceAllUsesWith(&*newArgIt);
        }

        // Replace each captured 'unreachable' placeholder with the real ret.
        for (auto& site : sites)
        {
            auto* ph = site.Placeholder;
            builder->SetInsertPoint(ph);
            if (site.Value != nullptr)
            {
                llvm::Value* retVal = site.Value;
                if (retVal->getType() != unifiedTy)
                    retVal = Upconvert(retVal, unifiedTy);
                builder->CreateRet(retVal);
            }
            else
            {
                builder->CreateRetVoid();
            }
            ph->eraseFromParent();
        }

        // Transfer the DI subprogram so debug info stays attached.
        if (auto* sp = oldFn->getSubprogram())
        {
            oldFn->setSubprogram(nullptr);
            newFn->setSubprogram(sp);
        }

        // Update the function table entry that pointed at the placeholder. We do
        // this before erasing so the dangling pointer compare is well-defined.
        auto it = functionTable.find(functionName);
        if (it != functionTable.end())
        {
            for (auto& sym : it->second)
            {
                if (sym.Function == oldFn)
                {
                    sym.Function    = newFn;
                    sym.UniqueName  = newMangledName;
                    sym.ReturnType  = newReturnType;
                    sym.ReturnsOwned = returnsOwned;
                    sym.ReturnsAlias = newReturnType.IsAlias; // 'alias' return: caller must not free the interior
                    sym.IsMethod    = isMethod;
                    sym.Parameters  = arguments;
                    sym.Variadic    = varargs;
                    break;
                }
            }
        }

        // Recursion would leave uses behind (call to oldFn from inside its own body).
        // Diagnose explicitly rather than letting LLVM's verifier complain later.
        if (!oldFn->use_empty())
            LogError(std::format("'auto' return: recursive call in function '{}' is not yet supported - declare the return type explicitly", functionName));

        ForgetFunctionEscapeMemo(oldFn);
        oldFn->eraseFromParent();
        return newFn;
    }

llvm::BasicBlock* LLVMBackend::GetElseBlock()
{
        for (const auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
        {
            auto elseBlock = stackFrame.elseBlock;
            if (elseBlock || stackFrame.isFunction)
            {
                return elseBlock;
            }
        }

        return nullptr;
    }

llvm::BasicBlock* LLVMBackend::ExchangeElseBlock(llvm::BasicBlock* newBlock)
{
        for (auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (stackFrame.elseBlock || stackFrame.isFunction)
            {
                auto old = stackFrame.elseBlock;
                stackFrame.elseBlock = newBlock;
                return old;
            }
        }
        return nullptr;
    }

bool LLVMBackend::IsBlockTerminated()
{
        return !IsInsertBlockLive();
    }

void LLVMBackend::ReopenAfterTerminator()
{
        auto* bb = builder->GetInsertBlock();
        if (bb == nullptr || bb->getParent() == nullptr || bb->getTerminator() == nullptr)
            return;
        builder->SetInsertPoint(CreateBasicBlock("unreachable", bb->getParent()));
    }

bool LLVMBackend::IsConstantTruthy(llvm::Value* v)
{
        if (auto* ci = llvm::dyn_cast_or_null<llvm::ConstantInt>(v))
            return !ci->isZero();
        return false;
    }

bool LLVMBackend::IsCurrentBlockUnreachable()
{
        auto* bb = builder->GetInsertBlock();
        if (bb == nullptr || bb->getParent() == nullptr)
            return false;
        if (bb == &bb->getParent()->getEntryBlock())
            return false;
        return llvm::pred_empty(bb);
    }

void LLVMBackend::CreateBreakCall()
{
        if (!IsInsertBlockLive())
            return;

        for (auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
        {
            EmitDestructorsForScope(stackFrame);
            if (stackFrame.resumeBlock)
            {
                builder->CreateBr(stackFrame.resumeBlock);
                break;
            }
        }
    }

void LLVMBackend::CreateContinueCall()
{
        if (!IsInsertBlockLive())
            return;

        for (auto& stackFrame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (stackFrame.continueBlock)
            {
                builder->CreateBr(stackFrame.continueBlock);
                break;
            }
            EmitDestructorsForScope(stackFrame);
        }
    }

void LLVMBackend::AttachVectorizeHintToCurrentLatch(int sourceLine)
{
        auto* term = builder->GetInsertBlock()->getTerminator();
        auto* br = llvm::dyn_cast_or_null<llvm::BranchInst>(term);
        if (!br)
            return;  // body did not fall through to a back-edge (e.g. ended in return)

        auto* i1True = llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 1);
        auto* enableMD = llvm::MDNode::get(*context, {
            llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
            llvm::ConstantAsMetadata::get(i1True),
        });
        // Stamp the source line into the loop ID so post-optimization enforcement
        // can report the exact `vectorize` loop without relying on debug info or
        // diagnostic-handler correlation.
        auto* lineMD = llvm::MDNode::get(*context, {
            llvm::MDString::get(*context, "cflat.vectorize.line"),
            llvm::ConstantAsMetadata::get(
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), sourceLine)),
        });

        // Loop metadata is a self-referential node: operand 0 points at itself.
        llvm::SmallVector<llvm::Metadata*, 3> ops{ nullptr, enableMD, lineMD };
        auto* loopID = llvm::MDNode::getDistinct(*context, ops);
        loopID->replaceOperandWith(0, loopID);
        br->setMetadata(llvm::LLVMContext::MD_loop, loopID);
    }

std::string LLVMBackend::GetSourceFileName() const
{ return sourceFileName; }

std::string LLVMBackend::GetSourceFilePath() const
{ return currentSourceFilePath_; }

std::string LLVMBackend::DefinitionSitePath() const
{
        if (sourceFileDir_.empty() || sourceDisplayName_.empty()
            || currentSourceFilePath_ != analyzedRootPath_)
            return currentSourceFilePath_;
        std::error_code ec;
        auto real = std::filesystem::weakly_canonical(
            std::filesystem::path(sourceFileDir_) / sourceDisplayName_, ec);
        return ec ? currentSourceFilePath_ : real.string();
    }

std::string LLVMBackend::GetCurrentFunctionName() const
{
        for (const auto& frame : std::ranges::reverse_view(stackNamedVariable))
            if (frame.isFunction) return frame.functionName;
        return "";
    }

void LLVMBackend::RegisterReturnBlock(const std::string& name, CFlatParser::CompoundStatementContext* body, std::vector<DeclTypeAndValue> params, TypeAndValue returnType)
{
        returnBlockTable[name] = { body, std::move(params), returnType };
    }

const LLVMBackend::ReturnBlockEntry* LLVMBackend::GetReturnBlock(const std::string& name) const
{
        auto it = returnBlockTable.find(name);
        return it != returnBlockTable.end() ? &it->second : nullptr;
    }

void LLVMBackend::RegisterNamespace(const std::string& name)
{ namespaceTable.insert(name); }

const std::string& LLVMBackend::GetCurrentNamespace() const
{ return currentNamespace_; }

void LLVMBackend::SetCurrentNamespace(const std::string& name)
{ currentNamespace_ = name; }

void LLVMBackend::RegisterNamespaceAlias(const std::string& alias, const std::string& target)
{ namespaceAliasTable[alias] = target; }

void LLVMBackend::RegisterLocalNamespaceAlias(const std::string& alias, const std::string& target)
{
        if (!stackNamedVariable.empty())
            stackNamedVariable.back().namespaceAliases[alias] = target;
        else
            namespaceAliasTable[alias] = target;
    }

void LLVMBackend::RegisterEnumBackingType(const std::string& enumName, const std::string& backingType)
{
        enumBackingTypes[enumName] = backingType;
    }

std::string LLVMBackend::GetEnumBackingType(const std::string& enumName) const
{
        auto it = enumBackingTypes.find(enumName);
        return it != enumBackingTypes.end() ? it->second : std::string();
    }

bool LLVMBackend::IsNamespace(const std::string& name) const
{
        for (const auto& frame : std::ranges::reverse_view(stackNamedVariable))
            if (frame.namespaceAliases.count(name)) return true;
        return namespaceTable.count(name) > 0 || namespaceAliasTable.count(name) > 0;
    }

bool LLVMBackend::IsImportAlias(const std::string& name) const
{ return importAliasMembers.count(name) > 0; }

bool LLVMBackend::IsImportAliasMember(const std::string& alias, const std::string& member) const
{
        auto it = importAliasMembers.find(alias);
        return it != importAliasMembers.end() && it->second.count(member) > 0;
    }

bool LLVMBackend::IsDataStructure(const std::string& name) const
{ return dataStructures.count(name) > 0; }

std::string LLVMBackend::ResolveNamespace(const std::string& name) const
{
        for (const auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            auto it = frame.namespaceAliases.find(name);
            if (it != frame.namespaceAliases.end()) return it->second;
        }
        auto it = namespaceAliasTable.find(name);
        return it != namespaceAliasTable.end() ? it->second : name;
    }

std::string LLVMBackend::ResolveInterfaceName(const std::string& spelled) const
{
        std::string name = ResolveTypeAlias(spelled);
        if (interfaceTable.count(name)) return name;
        if (!currentNamespace_.empty() && name.find('.') == std::string::npos)
        {
            std::string prefix = currentNamespace_;
            while (true)
            {
                std::string candidate = prefix + "." + name;
                if (interfaceTable.count(candidate)) return candidate;
                auto dot = prefix.rfind('.');
                if (dot == std::string::npos) break;
                prefix = prefix.substr(0, dot);
            }
        }
        std::string qualified = ResolveTypeAlias(ResolveQualifiedName(name));
        return interfaceTable.count(qualified) ? qualified : name;
    }

std::string LLVMBackend::ResolveQualifiedName(const std::string& name) const
{
        return ResolveQualifiedName(name, false);
    }

std::string LLVMBackend::ResolveQualifiedName(const std::string& name, bool forceRoot) const
{
        // Bare name referenced inside a namespace body: prefer an enclosing-namespace
        // sibling (e.g. inside "N", a bare "helper" resolves to "N.helper") before
        // falling back to a top-level/global symbol. Walk outward through parent
        // namespaces so a nested "Outer.Inner" also sees "Outer" members. A more-local
        // match wins; if no qualified sibling exists the bare name resolves below.
        if (!forceRoot && !currentNamespace_.empty() && name.find('.') == std::string::npos)
        {
            std::string prefix = currentNamespace_;
            while (true)
            {
                std::string candidate = prefix + "." + name;
                if (dataStructures.count(candidate) || interfaceTable.count(candidate)
                    || functionTable.count(candidate) || globalNamedVariable.count(candidate))
                    return candidate;
                auto parentDot = prefix.rfind('.');
                if (parentDot == std::string::npos)
                    break;
                prefix = prefix.substr(0, parentDot);
            }
        }

        if (dataStructures.count(name) || interfaceTable.count(name) || functionTable.count(name))
            return name;

        auto dotPos = name.rfind('.');
        if (dotPos == std::string::npos)
            return name;

        std::string lastName = name.substr(dotPos + 1);
        std::string nsPrefix = name.substr(0, dotPos);

        // Resolve an alias on the first namespace component
        {
            auto firstDot = nsPrefix.find('.');
            std::string firstComp = firstDot == std::string::npos ? nsPrefix : nsPrefix.substr(0, firstDot);
            std::string restComp  = firstDot == std::string::npos ? std::string{} : nsPrefix.substr(firstDot + 1);
            std::string resolvedFirst = ResolveNamespace(firstComp);
            if (resolvedFirst != firstComp)
                nsPrefix = restComp.empty() ? resolvedFirst : resolvedFirst + "." + restComp;
        }

        // "$global$:<alias>" sentinel: file-scoped import alias (import "x.cb" as Alias).
        // Resolve to unqualified lastName only if it was contributed by that file.
        if (nsPrefix.starts_with("$global$:"))
        {
            std::string aliasName = nsPrefix.substr(9);
            if (IsImportAliasMember(aliasName, lastName))
                return lastName;
            return name;
        }

        // Walk up from the (possibly expanded) prefix toward the root
        std::string prefix = nsPrefix;
        while (true)
        {
            std::string candidate = prefix + "." + lastName;
            if (dataStructures.count(candidate) || interfaceTable.count(candidate) || functionTable.count(candidate))
                return candidate;
            auto parentDot = prefix.rfind('.');
            if (parentDot == std::string::npos)
                break;
            prefix = prefix.substr(0, parentDot);
        }

        return name;
    }

std::string LLVMBackend::GetNameOfCurrentInsertionBlock()
{
        llvm::BasicBlock* currentBlock = builder->GetInsertBlock();
        return std::format("{}::{}", currentBlock->getParent()->getName().str(), currentBlock->getName().str());
    }

void LLVMBackend::DumpCurrentInsertionPoint(std::string prefix)
{
        llvm::BasicBlock* currentBlock = builder->GetInsertBlock();
        llvm::outs() << prefix << "Current insertion block: " << currentBlock->getParent()->getName() << "::" << currentBlock->getName() << "\n";
    }

void LLVMBackend::SetSkipRuntimeImport(bool v)
{ skipRuntimeImport = v; }

void LLVMBackend::SetRuntimeDir(const std::string& dir)
{ runtimeDir = dir; }

void LLVMBackend::SetSourceFileDir(const std::string& dir)
{ sourceFileDir_ = dir; }

void LLVMBackend::SetSourceDisplayName(const std::string& name)
{ sourceDisplayName_ = name; }

void LLVMBackend::SetVerbose(bool v)
{ verbose = v; }

bool LLVMBackend::IsVerbose() const
{ return verbose; }

void LLVMBackend::SetAsan(bool v)
{ asan_ = v; }

void LLVMBackend::SetSanitizeOwnership(bool v)
{ sanitizeOwnership_ = v; }

bool LLVMBackend::IsSanitizeOwnership() const
{ return sanitizeOwnership_; }

void LLVMBackend::SetHeapAudit(bool v)
{ heapAudit_ = v; }

void LLVMBackend::SetRunMode(bool v)
{ runMode_ = v; }

bool LLVMBackend::IsRunMode() const
{ return runMode_; }

void LLVMBackend::SetRunArgs(std::vector<std::string> a)
{ runArgs_ = std::move(a); }

int  LLVMBackend::GetJitExitCode() const
{ return jitExitCode_; }

void LLVMBackend::SetBatchMode(bool v)
{ batchMode_ = v; }

void LLVMBackend::SetNoCache(bool v)
{ noCache_ = v; }

void LLVMBackend::SetCHeaderCacheDeep(bool v)
{ cHeaderCacheDeep_ = v; }

void LLVMBackend::SetXthreadScanLevel(int n)
{ xthreadScanLevel_ = n; }

int  LLVMBackend::GetXthreadScanLevel() const
{ return xthreadScanLevel_; }

bool LLVMBackend::IsXthreadEscapedType(const std::string& typeName) const
{
        return threadSharedTypes_.find(typeName) != threadSharedTypes_.end();
    }

void LLVMBackend::AddXthreadEscapedType(const std::string& typeName)
{
        if (!typeName.empty())
            threadSharedTypes_.insert(typeName);
    }

void LLVMBackend::ReportXthreadFieldAccess(const std::string& varName, const std::string& fieldName,
                                  const std::string& structType, const TypeAndValue& field)
{
        if (xthreadScanLevel_ <= 0)
            return;
        if (!IsXthreadEscapedType(structType))
            return;
        if (FieldSatisfiesThreadDiscipline(field))
            return;                         // atomic wrapper or GuardedBy lock -> safe
        std::string line = std::format(
            "[xthread] field '{}.{}' ({}) shared across spawn, not atomic/guarded",
            varName.empty() ? "?" : varName, fieldName, structType);
        if (xthreadReported_.insert(line).second)
            std::cout << line << "\n";
    }

void LLVMBackend::SetDiagnosticSink(DiagnosticSink sink)
{ diagnosticSink_ = std::move(sink); }

void LLVMBackend::SetSymbolSink(LspSymbolIndex* sink)
{ symbolSink_ = sink; }

LspSymbolIndex* LLVMBackend::GetSymbolSink() const
{ return symbolSink_; }

void LLVMBackend::SetHintRegionSink(HintRegionSink sink)
{ hintRegionSink_ = std::move(sink); }

void LLVMBackend::ReportHintRegion(int startLine, int startCol, int endLine, int endCol, const std::string& msg)
{
        if (hintRegionSink_)
            hintRegionSink_(startLine, startCol, endLine, endCol, msg);
    }

bool LLVMBackend::HasHintRegionSink() const
{ return (bool)hintRegionSink_; }

std::string LLVMBackend::ResolveCLinkLib(const std::string& lib, const std::string& importingFilePath)
{
        std::filesystem::path lp(lib);
        if (lp.is_absolute())
            return lp.string();
        std::filesystem::path beside = (std::filesystem::path(importingFilePath).parent_path() / lp).lexically_normal();
        std::error_code ec;
        if (std::filesystem::exists(beside, ec))
            return beside.string();
        // A relative path that names a subdirectory is a real (possibly mistyped) location -
        // keep it normalized so lld-link's "not found" points at the resolved path. A bare
        // filename is a system lib - pass it through for the linker's lib-path search.
        if (lp.has_parent_path())
            return beside.string();
        return lib;
    }

void LLVMBackend::SetVcpkgExe(const std::string& path)
{ vcpkg_.SetExeOverride(path); }

void LLVMBackend::SetVcpkgManifest(const std::string& path)
{ vcpkg_.SetManifestOverride(path); }

void LLVMBackend::SetVcpkgTriplet(const std::string& triplet)
{ vcpkg_.SetTripletOverride(triplet); }

std::string LLVMBackend::RootVcpkgImportPath(const std::string& analyzedPath) const
{
        if (sourceFileDir_.empty())
            return analyzedPath;
        return (std::filesystem::path(sourceFileDir_) /
                std::filesystem::path(analyzedPath).filename()).string();
    }

uint64_t LLVMBackend::VcpkgDiskCacheKey(const std::string& fileForLsp,
                                       const std::vector<std::string>& defines)
{
        uint64_t h = 14695981039346656037ULL;
        for (unsigned char c : fileForLsp) { h ^= c; h *= 1099511628211ULL; }
        for (const auto& d : defines) for (unsigned char c : d) { h ^= c; h *= 1099511628211ULL; }
        return h;
    }

std::string LLVMBackend::CompilerBuildStamp()
{
        static const std::string stamp = []() -> std::string {
            std::string exe = PlatformExePath();
            if (exe.empty()) return "unknown";
            std::error_code ec;
            auto size = std::filesystem::file_size(exe, ec);
            if (ec) return "unknown";
            auto mtime = std::filesystem::last_write_time(exe, ec);
            if (ec) return "unknown";
            long long ticks = (long long)mtime.time_since_epoch().count();
            unsigned long long bytes = (unsigned long long)size;
            return std::to_string(ticks) + "-" + std::to_string(bytes);
        }();
        return stamp;
    }

std::string LLVMBackend::GetCHeaderCacheDir()
{
        std::string base = GetCflatCacheDir();
        if (base.empty()) return {};
        // Forward slash: Win32 accepts it, and a backslash would otherwise become part of
        // the directory NAME on POSIX (a literal "~/.cflat\cheaders" entry).
        return base + "/cheaders";
    }

uint64_t LLVMBackend::CHeaderDiskCacheKey(const std::string& fileForLsp,
                                        const std::vector<std::string>& includeDirs,
                                        const std::vector<std::string>& defines,
                                        const std::vector<std::string>& extraDefines)
{
        return CHeaderDiskCacheKey(std::vector<std::string>{ fileForLsp },
                                   includeDirs, defines, extraDefines);
    }

uint64_t LLVMBackend::CHeaderDiskCacheKey(const std::vector<std::string>& headerPaths,
                                        const std::vector<std::string>& includeDirs,
                                        const std::vector<std::string>& defines,
                                        const std::vector<std::string>& extraDefines)
{
        uint64_t h = 14695981039346656037ULL;
        auto fold = [&h](const std::string& s) {
            for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
        };
        for (const auto& hp : headerPaths)   { fold("|H"); fold(hp); }
        for (const auto& inc : includeDirs)  { fold("|I"); fold(inc); }
        for (const auto& def : defines)      { fold("|D"); fold(def); }
        for (const auto& def : extraDefines) { fold("|d"); fold(def); }
        return h;
    }

nlohmann::json LLVMBackend::TvToJson(const TypeAndValue& tv)
{
        nlohmann::json j;
        j["t"] = tv.TypeName;
        if (tv.Pointer)        j["p"]   = true;
        if (tv.ElemPointer)    j["ep"]  = true;
        if (tv.PointerDepth)   j["pd"]  = tv.PointerDepth;
        if (tv.IsMove)         j["mv"]  = true;
        if (tv.CallConv != CallingConv::Default) j["cc"] = static_cast<int>(tv.CallConv);
        if (tv.ConstArraySize) j["arr"] = tv.ConstArraySize;
        if (!tv.ConstInnerDimensions.empty()) j["idims"] = tv.ConstInnerDimensions;
        if (tv.IsSimd) { j["sd"] = true; j["sdl"] = tv.SimdLanes; }
        if (tv.IsArrayView) j["av"] = true;
        if (tv.IsFunctionPointer)
        {
            j["fp"]  = true;
            j["fpr"] = tv.FuncPtrReturnTypeName;
            if (tv.FuncPtrReturnPointer) j["fprp"] = true;
            nlohmann::json fps = nlohmann::json::array();
            for (const auto& p : tv.FuncPtrParams)
            {
                nlohmann::json pj;
                pj["t"] = p.TypeName;
                if (p.Pointer) pj["p"]  = true;
                if (p.IsMove)  pj["mv"] = true;
                if (p.IsOwningSink) pj["osk"] = true;
                if (p.IsConsumeInferredSink) pj["cis"] = true;
                fps.push_back(pj);
            }
            j["fps"] = fps;
        }
        return j;
    }

LLVMBackend::TypeAndValue LLVMBackend::TvFromJson(const SjVal& j)
{
        TypeAndValue tv;
        tv.TypeName       = j.value("t",   std::string{});
        tv.Pointer        = j.value("p",   false);
        tv.ElemPointer    = j.value("ep",  false);
        tv.PointerDepth   = j.value("pd",  0);
        tv.IsMove         = j.value("mv",  false);
        tv.CallConv = static_cast<CallingConv>(j.value("cc", 0));
        tv.ConstArraySize = j.value("arr", uint64_t{0});
        if (j.contains("idims")) tv.ConstInnerDimensions = j["idims"].to_u64_vector();
        tv.IsSimd   = j.value("sd",  false);
        tv.SimdLanes = j.value("sdl", uint64_t{0});
        tv.IsArrayView = j.value("av", false);
        tv.IsFunctionPointer = j.value("fp", false);
        if (tv.IsFunctionPointer)
        {
            tv.FuncPtrReturnTypeName = j.value("fpr",  std::string{});
            tv.FuncPtrReturnPointer  = j.value("fprp", false);
            if (j.contains("fps"))
                for (const auto& pj : j["fps"])
                {
                    TypeAndValue::FuncPtrParam p;
                    p.TypeName = pj.value("t",  std::string{});
                    p.Pointer  = pj.value("p",  false);
                    p.IsMove   = pj.value("mv", false);
                    p.IsOwningSink = pj.value("osk", false);
                    p.IsConsumeInferredSink = pj.value("cis", false);
                    tv.FuncPtrParams.push_back(p);
                }
        }
        return tv;
    }

nlohmann::json LLVMBackend::SigToJson(const CSigEntry& e)
{
        nlohmann::json ps = nlohmann::json::array();
        for (const auto& p : e.params) ps.push_back(TvToJson(p));
        nlohmann::json j = {{"n", e.name}, {"r", TvToJson(e.ret)}, {"ps", ps},
                            {"va", e.variadic}, {"ln", e.line}, {"co", e.col}};
        if (!e.file.empty()) j["f"] = e.file;
        return j;
    }

LLVMBackend::CSigEntry LLVMBackend::SigFromJson(const SjVal& j)
{
        CSigEntry e;
        e.name     = j.value("n",  std::string{});
        e.ret      = TvFromJson(j.at("r"));
        e.variadic = j.value("va", false);
        e.file     = j.value("f",  std::string{});
        e.line     = j.value("ln", 1);
        e.col      = j.value("co", 0);
        if (j.contains("ps")) for (const auto& p : j["ps"]) e.params.push_back(TvFromJson(p));
        return e;
    }

nlohmann::json LLVMBackend::EnumToJson(const CEnumEntry& e)
{
        return {{"n", e.name}, {"v", e.value}, {"ln", e.line}, {"co", e.col}};
    }

LLVMBackend::CEnumEntry LLVMBackend::EnumFromJson(const SjVal& j)
{
        return {j.value("n", std::string{}), j.value("v", 0LL), j.value("ln", 1), j.value("co", 0)};
    }

nlohmann::json LLVMBackend::GlobalToJson(const CGlobalEntry& g)
{
        return {{"n", g.name}, {"t", TvToJson(g.type)}, {"ln", g.line}, {"co", g.col}};
    }

LLVMBackend::CGlobalEntry LLVMBackend::GlobalFromJson(const SjVal& j)
{
        CGlobalEntry g;
        g.name = j.value("n", std::string{});
        g.type = TvFromJson(j.at("t"));
        g.line = j.value("ln", 1);
        g.col  = j.value("co", 0);
        return g;
    }

nlohmann::json LLVMBackend::FieldToJson(const CRecordFieldEntry& f)
{
        nlohmann::json j = {{"n", f.name}, {"ct", f.ctype}};
        if (f.isBitfield) { j["bf"] = true; j["bw"] = f.bitWidth; }
        return j;
    }

LLVMBackend::CRecordFieldEntry LLVMBackend::FieldFromJson(const SjVal& j)
{
        CRecordFieldEntry f;
        f.name      = j.value("n",  std::string{});
        f.ctype     = j.value("ct", std::string{});
        f.isBitfield = j.value("bf", false);
        f.bitWidth   = j.value("bw", 0u);
        return f;
    }

nlohmann::json LLVMBackend::RecordToJson(const CRecordEntry& r)
{
        nlohmann::json fs = nlohmann::json::array();
        for (const auto& f : r.fields) fs.push_back(FieldToJson(f));
        nlohmann::json j = {{"n", r.name}, {"fs", fs}, {"ln", r.line}, {"co", r.col}};
        if (r.isUnion) j["u"] = true;
        if (!r.uuid.empty()) j["id"] = r.uuid;
        return j;
    }

LLVMBackend::CRecordEntry LLVMBackend::RecordFromJson(const SjVal& j)
{
        CRecordEntry r;
        r.name    = j.value("n", std::string{});
        r.isUnion = j.value("u", false);
        r.line    = j.value("ln", 1);
        r.col     = j.value("co", 0);
        r.uuid    = j.value("id", std::string{});
        if (j.contains("fs")) for (const auto& f : j["fs"]) r.fields.push_back(FieldFromJson(f));
        return r;
    }

nlohmann::json LLVMBackend::MacroToJson(const CMacroEntry& m)
{
        nlohmann::json j = {{"n", m.name}, {"v", m.value}, {"f", m.file},
                            {"ln", m.line}, {"co", m.col}};
        if (m.isPointer) j["isp"]  = true;
        // Store the float as its raw IEEE-754 bit pattern, not as a JSON number:
        // nlohmann serializes inf/NaN as JSON `null` (math.h's INFINITY/NAN/HUGE_VAL),
        // which then throws type_error.302 on reload. Bits round-trip every value exactly.
        if (m.isFloat)
        {
            uint64_t fvbits = 0;
            std::memcpy(&fvbits, &m.floatValue, sizeof(double));
            j["isf"] = true; j["fvb"] = fvbits;
        }
        if (m.isString) { j["iss"]  = true; j["sv"]   = m.stringValue; }
        if (m.isFuncPtr){ j["isfp"] = true; j["fptv"] = TvToJson(m.funcPtrTV); }
        if (!m.intTypeName.empty()) j["ity"] = m.intTypeName;
        return j;
    }

LLVMBackend::CMacroEntry LLVMBackend::MacroFromJson(const SjVal& j)
{
        CMacroEntry m;
        m.name      = j.value("n",   std::string{});
        m.value     = j.value("v",   0LL);
        m.file      = j.value("f",   std::string{});
        m.line      = j.value("ln",  1);
        m.col       = j.value("co",  0);
        m.isPointer = j.value("isp", false);
        m.isFloat   = j.value("isf", false);
        if (m.isFloat)
        {
            uint64_t fvbits = j.value("fvb", uint64_t{0});
            std::memcpy(&m.floatValue, &fvbits, sizeof(double));
        }
        m.isString  = j.value("iss",  false);
        if (m.isString) m.stringValue = j.value("sv", std::string{});
        m.isFuncPtr = j.value("isfp", false);
        if (m.isFuncPtr && j.contains("fptv")) m.funcPtrTV = TvFromJson(j["fptv"]);
        m.intTypeName = j.value("ity", std::string{});
        return m;
    }

nlohmann::json LLVMBackend::FuncMacroToJson(const CFunctionMacroEntry& m)
{
        return {{"n", m.name}, {"ps", m.params}, {"b", m.body},
                {"f", m.file}, {"ln", m.line},   {"co", m.col}};
    }

LLVMBackend::CFunctionMacroEntry LLVMBackend::FuncMacroFromJson(const SjVal& j)
{
        CFunctionMacroEntry m;
        m.name = j.value("n",  std::string{});
        m.body = j.value("b",  std::string{});
        m.file = j.value("f",  std::string{});
        m.line = j.value("ln", 1);
        m.col  = j.value("co", 0);
        if (j.contains("ps")) m.params = j["ps"].to_string_vector();
        return m;
    }

bool LLVMBackend::CHeaderDepFresh(const CHeaderDep& dep)
{
        std::error_code ec;
        auto mt = std::filesystem::last_write_time(dep.path, ec);
        if (ec) return false;
        if ((int64_t)mt.time_since_epoch().count() == dep.mtime) return true;
        uint64_t h = 0;
        return HashFileFnv1a(dep.path, h) && h == dep.hash;
    }

bool LLVMBackend::TryLoadCHeaderDiskCache(
        const std::filesystem::path& cacheDir,
        uint64_t diskKey,
        std::filesystem::file_time_type mtime,
        uint64_t contentHash,
        CFileSigCacheEntry& out)
{
        namespace fs = std::filesystem;
        std::error_code ec;
        auto cachePath = cacheDir / std::format("{:016x}.json", diskKey);
        if (!fs::exists(cachePath, ec)) return false;

        // parser + jsonBuf own the storage that doc/SjVal reference; keep them alive for the
        // whole function. Reads run through SjVal (simdjson DOM); writes still use nlohmann.
        simdjson::dom::parser parser;
        simdjson::padded_string jsonBuf;
        simdjson::dom::element doc;
        SjVal j;
        {
            llvm::TimeTraceScope parseScope("CHeaderJsonParse", cachePath.string());
            auto loaded = simdjson::padded_string::load(cachePath.string());
            if (loaded.error()) return false;
            jsonBuf = std::move(loaded.value());
            if (parser.parse(jsonBuf).get(doc) != simdjson::SUCCESS) return false;
            j = SjVal{doc};
        }

        int version = j.value("version", 0);
        // v1/v2 stored float macro values as JSON numbers, which encode inf/NaN as `null`
        // and throw on reload; v3 used the raw-bits float encoding but cached only in-scope
        // records (so an entry written before the by-value dependency closure was added would
        // still be missing the dependency structs). v4 caches the in-scope + dependency set.
        // v5 widened the in-scope filter to the Windows SDK shared/ sibling dir (ERROR_*,
        // MAX_PATH, ...), so a v4 entry would silently lack those constants.
        // v6 synthesizes a tag/record for a named field of an unnamed record type (the
        // _LARGE_INTEGER `u` shape), so a v5 entry would still carry the unmappable
        // "struct X::(unnamed at ...)" ctype that abandons the whole record.
        // v7 extends that synthesis to an *array* of an unnamed record element
        // (RETRIEVAL_POINTERS_BUFFER.Extents[1]); a v6 entry would still have dropped that
        // field and left the enclosing record an incomplete shell.
        // v8 records each C function's own declaring header (CSigEntry.file) so
        // go-to-definition lands on the real prototype, not the imported umbrella header.
        // v9 adopts pointer-to-record typedefs as handle aliases (CGColorSpaceRef ->
        // CGColorSpace*); a v8 entry's recordAliases dropped every one of them.
        // v10 also seeds the needed-record closure from in-scope function-signature and
        // global-variable by-value types, not just in-scope record fields; a v9 entry cached
        // the narrower record set, so CGPoint/CGRect-style signature-only dependency records
        // (defined in a sibling out-of-scope header, named only by a function's params/return)
        // would still be missing.
        if (version != 10) return false;

        // Accept on mtime match (fast) or content hash match (authoritative on mtime drift).
        auto storedMtime = j.value("mtime", int64_t{-1});
        auto storedHash  = j.value("hash",  uint64_t{0});
        bool mtimeOk = (storedMtime == (int64_t)mtime.time_since_epoch().count());
        bool hashOk  = (storedHash  == contentHash);
        if (!mtimeOk && !hashOk) return false;

        // Any malformed/incompatible field must degrade to a cache miss (reparse), never abort
        // the compiler: the nlohmann accessors throw on a type mismatch, so guard the whole build.
        CFileSigCacheEntry entry;
        entry.mtime = mtime;
        entry.hash  = contentHash;
        // DOM walk -> structs (plus deep-mode deps freshness check). Distinct from the parse
        // span above so the allocation-bound conversion cost can be tracked separately.
        llvm::TimeTraceScope convertScope("CHeaderJsonConvert", cachePath.string());
        try
        {
            if (j.contains("sigs"))       for (const auto& s : j["sigs"])       entry.sigs.push_back(SigFromJson(s));
            if (j.contains("enums"))      for (const auto& e : j["enums"])      entry.enums.push_back(EnumFromJson(e));
            if (j.contains("records"))    for (const auto& r : j["records"])    entry.records.push_back(RecordFromJson(r));
            if (j.contains("macros"))     for (const auto& m : j["macros"])     entry.macros.push_back(MacroFromJson(m));
            if (j.contains("funcMacros")) for (const auto& m : j["funcMacros"]) entry.funcMacros.push_back(FuncMacroFromJson(m));
            if (j.contains("globals"))    for (const auto& g : j["globals"])    entry.globals.push_back(GlobalFromJson(g));
            if (j.contains("recordAliases"))
                for (const auto& a : j["recordAliases"])
                    entry.recordAliases.emplace_back(a.value("a", std::string{}), a.value("t", std::string{}));

            // A deep (transitive) entry is only fresh if every recorded include is unchanged.
            // Shallow entries (no "deps") skip this and rely on the top-header check above.
            if (j.contains("deps"))
            {
                for (const auto& dj : j["deps"])
                {
                    CHeaderDep dep;
                    dep.path  = dj.value("f", std::string{});
                    dep.mtime = dj.value("mt", int64_t{0});
                    dep.hash  = dj.value("h",  uint64_t{0});
                    if (!CHeaderDepFresh(dep)) return false;
                    entry.deps.push_back(std::move(dep));
                }
            }
        }
        catch (...) { return false; }
        out = std::move(entry);
        return true;
    }

void LLVMBackend::WriteCHeaderDiskCache(
        const std::filesystem::path& cacheDir,
        uint64_t diskKey,
        std::filesystem::file_time_type mtime,
        uint64_t contentHash,
        const CFileSigCacheEntry& entry)
{
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(cacheDir, ec);
        if (ec) return;

        nlohmann::json j;
        j["version"] = 10;
        j["mtime"]   = (int64_t)mtime.time_since_epoch().count();
        j["hash"]    = contentHash;

        nlohmann::json sigs = nlohmann::json::array();
        for (const auto& s : entry.sigs) sigs.push_back(SigToJson(s));
        j["sigs"] = sigs;
        nlohmann::json enums = nlohmann::json::array();
        for (const auto& e : entry.enums) enums.push_back(EnumToJson(e));
        j["enums"] = enums;
        nlohmann::json records = nlohmann::json::array();
        for (const auto& r : entry.records) records.push_back(RecordToJson(r));
        j["records"] = records;
        nlohmann::json macros = nlohmann::json::array();
        for (const auto& m : entry.macros) macros.push_back(MacroToJson(m));
        j["macros"] = macros;
        nlohmann::json funcMacros = nlohmann::json::array();
        for (const auto& m : entry.funcMacros) funcMacros.push_back(FuncMacroToJson(m));
        j["funcMacros"] = funcMacros;
        nlohmann::json globals = nlohmann::json::array();
        for (const auto& g : entry.globals) globals.push_back(GlobalToJson(g));
        j["globals"] = globals;
        nlohmann::json recordAliases = nlohmann::json::array();
        for (const auto& a : entry.recordAliases)
            recordAliases.push_back({{"a", a.first}, {"t", a.second}});
        j["recordAliases"] = recordAliases;

        // Deep mode only: the transitive include set for strict (transitive) validation.
        if (!entry.deps.empty())
        {
            nlohmann::json deps = nlohmann::json::array();
            for (const auto& d : entry.deps)
                deps.push_back({{"f", d.path}, {"mt", d.mtime}, {"h", d.hash}});
            j["deps"] = deps;
        }

        // Atomic write: PID-stamped temp file renamed over the target.
        auto tmpPath  = cacheDir / std::format("{:016x}.{}.tmp", diskKey, _getpid());
        auto destPath = cacheDir / std::format("{:016x}.json", diskKey);
        {
            std::ofstream f(tmpPath);
            if (!f.is_open()) return;
            f << j;
        }
        fs::rename(tmpPath, destPath, ec);
        if (ec) fs::remove(tmpPath, ec);
    }

bool LLVMBackend::CompileVcpkgImport(const std::string& importingFilePath,
                            const std::string& header,
                            const std::string& portSpec,
                            const std::vector<std::string>& extraDefines)
{
        // Mirror LSP/non-LSP mode and verbosity into the resolver.
        vcpkg_.SetVerbose(verbose);
        vcpkg_.SetLspMode(symbolSink_ != nullptr);
        vcpkg_.SetPlatform(platformValue == 32 ? "win32" : "win64");

        VcpkgResolver::Resolution res;
        std::string err;
        if (!vcpkg_.Resolve(importingFilePath, portSpec, res, err))
        {
            LogError(err);
            return false;
        }

        // Push the resolved paths into the existing accumulators. Idempotent: a second
        // package-vcpkg import re-pushes the same include dir, which is harmless (clang-cl
        // dedupes -I, lld-link dedupes libs).
        if (!res.includeDir.empty())
        {
            bool dup = false;
            for (const auto& d : cIncludeDirs_) if (d == res.includeDir) { dup = true; break; }
            if (!dup) cIncludeDirs_.push_back(res.includeDir);
        }
        for (const auto& lib : res.libs)
        {
            bool dup = false;
            for (const auto& l : cLinkLibs_) if (l == lib) { dup = true; break; }
            if (!dup) cLinkLibs_.push_back(lib);
        }
        for (const auto& dll : res.dlls)
        {
            bool dup = false;
            for (const auto& d : vcpkgRuntimeDlls_) if (d == dll) { dup = true; break; }
            if (!dup) vcpkgRuntimeDlls_.push_back(dll);
        }

        // The header path in source is relative to the include dir (e.g. "curl/curl.h").
        // Resolve it against the vcpkg include dir explicitly so CompileCHeader sees an
        // absolute path; the source-location filter in clang's AST dump already keeps
        // only decls under our --c-include roots, so unrelated SDK/CRT decls stay out.
        std::filesystem::path headerAbs = std::filesystem::path(res.includeDir) / header;
        std::error_code ec;
        auto headerCanon = std::filesystem::canonical(headerAbs, ec);
        if (ec)
        {
            // In LSP mode `vcpkg install` is skipped (RunVcpkgInstall is a no-op), so if the
            // package has not been built yet the whole vcpkg_installed/<triplet>/include tree
            // is absent. Flagging the import line then would put a spurious error on a file
            // that compiles cleanly once the user runs a build. Degrade to a silent skip: the
            // C symbols just stay unindexed until the package is installed. The CLI build
            // (which actually ran the install) still reports the precise error. A header
            // missing *under an existing* include dir is a real mistake (typo / wrong port)
            // and is surfaced even in LSP mode.
            const bool lspMode = symbolSink_ != nullptr;
            std::error_code dirEc;
            const bool includeDirPresent =
                !res.includeDir.empty() && std::filesystem::exists(res.includeDir, dirEc);
            if (lspMode && !includeDirPresent)
            {
                if (verbose)
                    std::cout << std::format("[verbose] vcpkg: package not installed (no '{}'); skipping header '{}' for LSP analysis\n",
                        res.includeDir, header);
                return true;
            }
            // CLI: a missing header is a hard error. If `vcpkg install` was suppressed
            // (--vcpkg-no-install) and the include tree is absent, say so precisely instead
            // of the generic "install is incomplete" hint.
            if (vcpkg_.InstallSuppressed() && !includeDirPresent)
            {
                LogError(std::format(
                    "import package-vcpkg: port for header '{}' is not installed (no '{}'), "
                    "and 'vcpkg install' is disabled (--vcpkg-no-install).\n"
                    "  Run 'vcpkg install' yourself, or drop --vcpkg-no-install to let cflat install it.",
                    header, res.includeDir));
                return false;
            }
            LogError(std::format(
                "import package-vcpkg: header '{}' not found under '{}'.\n"
                "  The port may not own this header, or the install is incomplete.",
                header, res.includeDir));
            return false;
        }
        return BindCanonicalCHeader(headerCanon, res.includeDir, extraDefines);
    }

bool LLVMBackend::BindCanonicalCHeader(const std::filesystem::path& headerCanon,
                              const std::string& includeDir,
                              const std::vector<std::string>& extraDefines)
{
        std::filesystem::path pkgCacheDir =
            std::filesystem::path(includeDir).parent_path().parent_path() / ".cflat-cache";

        // Derive the same in-memory key CompileCHeader builds internally.
        llvm::SmallString<256> realPathBuf;
        std::string fileForLsp = headerCanon.string();
        if (!llvm::sys::fs::real_path(fileForLsp, realPathBuf))
            fileForLsp = realPathBuf.str().str();
        // Mirror the in-memory cache key CompileCHeaderGroup builds (single-header form).
        std::string inMemKey = "|H" + fileForLsp;
        for (const auto& inc : cIncludeDirs_)  inMemKey += "|I" + inc;
        for (const auto& def : cDefines_)      inMemKey += "|D" + def;
        for (const auto& def : extraDefines)   inMemKey += "|d" + def;

        // Fold the inline `define` clauses into the disk key so a build with a different
        // set of defines does not load a stale cached header bind.
        std::vector<std::string> diskKeyDefines = cDefines_;
        diskKeyDefines.insert(diskKeyDefines.end(), extraDefines.begin(), extraDefines.end());
        uint64_t diskKey     = VcpkgDiskCacheKey(fileForLsp, diskKeyDefines);
        uint64_t contentHash = 0;
        bool haveHash        = HashFileContents(fileForLsp, contentHash);
        std::error_code mtEc;
        auto headerMtime     = std::filesystem::last_write_time(fileForLsp, mtEc);

        bool diskHit = false;
        if (haveHash && !mtEc)
        {
            CFileSigCacheEntry diskEntry;
            if (TryLoadCHeaderDiskCache(pkgCacheDir, diskKey, headerMtime, contentHash, diskEntry))
            {
                std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
                cFileSigCache_[inMemKey] = std::move(diskEntry);
                diskHit = true;
                if (verbose)
                    std::cout << std::format("[verbose] package header disk cache hit: {}\n", fileForLsp);
            }
        }

        bool ok = CompileCHeader(headerCanon.string(), extraDefines);

        // --run is read-only: skip persisting the header cache to disk under run mode
        // (the in-memory cache entry from CompileCHeader still serves this compile).
        if (ok && !diskHit && !runMode_ && haveHash && !mtEc)
        {
            CFileSigCacheEntry entryToWrite;
            bool haveEntry = false;
            {
                std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
                auto it = cFileSigCache_.find(inMemKey);
                if (it != cFileSigCache_.end()) { entryToWrite = it->second; haveEntry = true; }
            }
            if (haveEntry)
                WriteCHeaderDiskCache(pkgCacheDir, diskKey, headerMtime, contentHash, entryToWrite);
        }

        if (ok) ProcessPendingMacroSources();
        return ok;
    }

bool LLVMBackend::ResolveNugetPri(const std::string& priName,
                         const std::string& packageFolder,
                         const std::string& packageSpec,
                         bool lspMode)
{
        namespace fs = std::filesystem;
        std::error_code ec;
        std::string arch = (platformValue == 32) ? "x86" : "x64";

        fs::path primary = fs::path(packageFolder) / "runtimes-framework" /
                           ("win-" + arch) / "native" / priName;
        fs::path found;
        if (fs::exists(primary, ec))
            found = primary;
        else
        {
            for (auto it = fs::recursive_directory_iterator(packageFolder, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                if (it->is_regular_file(ec) && it->path().filename().string() == priName)
                {
                    found = it->path();
                    break;
                }
            }
        }

        if (found.empty())
        {
            // LSP never deploys; a missing pri must not paint an error on a clean file.
            if (lspMode) return true;
            LogError(std::format(
                "import package-nuget: pri file '{}' was not found in package '{}' (probed '{}' "
                "and a recursive search of '{}').",
                priName, packageSpec, primary.string(), packageFolder));
            return false;
        }

        std::string abs = fs::absolute(found, ec).string();
        if (!deployPriPath_.empty() && deployPriPath_ != abs)
        {
            LogError(std::format(
                "import package-nuget: conflicting pri deployment - both '{}' and '{}' were "
                "requested as <exe>.pri. Only one pri may be deployed per output.",
                deployPriPath_, abs));
            return false;
        }
        deployPriPath_ = abs;
        if (verbose)
            std::cout << std::format("[verbose] nuget: pri '{}' -> deploy as <exe>.pri from {}\n", priName, abs);
        return true;
    }

bool LLVMBackend::CompileNugetImport(const std::vector<std::string>& files,
                            const std::string& packageSpec,
                            const std::vector<std::string>& extraDefines,
                            const std::string& priName)
{
        if (files.empty()) return true;
        const bool multi = files.size() > 1;
        const bool lspMode = symbolSink_ != nullptr;

        // Lowercased extension of a path (leading '.' included, e.g. ".winmd").
        auto lowerExt = [](const std::string& f) {
            std::string e = std::filesystem::path(f).extension().string();
            std::transform(e.begin(), e.end(), e.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return e;
        };

        // Shape check BEFORE package resolution (needs no network/cache): a multi-entry group is
        // one C translation unit, so it may hold only .h/.hpp/.hh headers. A .winmd must be
        // imported on its own line; anything else is unsupported inside a group.
        if (multi)
        {
            for (const auto& f : files)
            {
                std::string ext = lowerExt(f);
                if (ext == ".winmd")
                {
                    LogError(std::format(
                        "import package-nuget: '{}' - a .winmd cannot appear in a multi-entry group; "
                        "group a .winmd import on its own line.", f));
                    return false;
                }
                if (ext != ".h" && ext != ".hpp" && ext != ".hh")
                {
                    LogError(std::format(
                        "import package-nuget: '{}' - a multi-entry group may contain only "
                        ".h/.hpp/.hh headers.", f));
                    return false;
                }
            }
        }

        // Mirror LSP/non-LSP mode and verbosity into the resolver.
        nuget_.SetVerbose(verbose);
        nuget_.SetLspMode(symbolSink_ != nullptr);
        nuget_.SetPlatform(platformValue == 32 ? "win32" : "win64");

        NugetResolver::Resolution res;
        std::string err;
        if (!nuget_.Resolve(packageSpec, res, err))
        {
            // LSP mode never downloads; an unresolved package should not paint an error on a
            // file that compiles cleanly once the package is restored. Degrade to a silent skip.
            if (lspMode)
            {
                if (verbose)
                    std::cout << std::format("[verbose] nuget: package '{}' not resolved; skipping for LSP analysis\n",
                        packageSpec);
                return true;
            }
            LogError(err);
            return false;
        }

        // Push the resolved paths into the existing accumulators (deduped, same as vcpkg).
        // vcpkgRuntimeDlls_ is the generic "runtime DLLs to copy next to the exe" list; reuse it.
        for (const auto& inc : res.includeDirs)
        {
            if (inc.empty()) continue;
            bool dup = false;
            for (const auto& d : cIncludeDirs_) if (d == inc) { dup = true; break; }
            if (!dup) cIncludeDirs_.push_back(inc);
        }
        for (const auto& lib : res.libs)
        {
            bool dup = false;
            for (const auto& l : cLinkLibs_) if (l == lib) { dup = true; break; }
            if (!dup) cLinkLibs_.push_back(lib);
        }
        for (const auto& dll : res.dlls)
        {
            bool dup = false;
            for (const auto& d : vcpkgRuntimeDlls_) if (d == dll) { dup = true; break; }
            if (!dup) vcpkgRuntimeDlls_.push_back(dll);
        }

        // Optional `pri "..."` clause: locate the named .pri inside the resolved package and
        // record it for deployment as <exe>.pri. Probe the arch-specific framework runtimes
        // dir first, then fall back to a recursive filename match over the package folder.
        if (!priName.empty() && !ResolveNugetPri(priName, res.packageFolder, packageSpec, lspMode))
            return false;

        // Multi-entry group: STRICT package-only. Resolve every header under the package
        // include dirs (a header not found there is an error - system headers may not ride in a
        // package-nuget group), then bind them all as one TU / one disk-cache entry.
        if (multi)
        {
            std::vector<std::string> headerCanonicals;
            for (const auto& f : files)
            {
                bool found = false;
                for (const auto& inc : res.includeDirs)
                {
                    std::error_code ec;
                    auto headerCanon = std::filesystem::canonical(std::filesystem::path(inc) / f, ec);
                    if (!ec) { headerCanonicals.push_back(headerCanon.string()); found = true; break; }
                }
                if (found) continue;
                // Not found under any resolved include dir. In LSP mode degrade to a silent skip.
                if (lspMode)
                {
                    if (verbose)
                        std::cout << std::format("[verbose] nuget: header '{}' not found in package '{}'; skipping for LSP analysis\n",
                            f, packageSpec);
                    return true;
                }
                LogError(std::format(
                    "import package-nuget: header '{}' was not found in the include dirs of package '{}'.\n"
                    "  Only package-owned headers may appear in a package-nuget group; a system header "
                    "(e.g. windows.h) may not ride in a package-nuget group.",
                    f, packageSpec));
                return false;
            }
            bool ok = CompileCHeaderGroup(headerCanonicals, extraDefines, /*diskCache=*/true);
            if (ok) ProcessPendingMacroSources();
            return ok;
        }

        // Single entry: route by extension of the imported file.
        const std::string& file = files[0];
        std::string ext = std::filesystem::path(file).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (ext == ".h" || ext == ".hpp" || ext == ".hh")
        {
            // The source header path is relative to a package include dir (e.g. "WebView2.h").
            // Find the first include dir under which it exists, canonicalize, then reuse the
            // shared disk-cache + CompileCHeader flow.
            for (const auto& inc : res.includeDirs)
            {
                std::error_code ec;
                std::filesystem::path headerAbs = std::filesystem::path(inc) / file;
                auto headerCanon = std::filesystem::canonical(headerAbs, ec);
                if (!ec)
                    return BindCanonicalCHeader(headerCanon, inc, extraDefines);
            }
            // Not found under any resolved include dir. In LSP mode the package may not be
            // restored yet - degrade to a silent skip rather than flagging the import line.
            if (lspMode)
            {
                if (verbose)
                    std::cout << std::format("[verbose] nuget: header '{}' not found in package '{}'; skipping for LSP analysis\n",
                        file, packageSpec);
                return true;
            }
            LogError(std::format(
                "import package-nuget: header '{}' not found in the include dirs of package '{}'.\n"
                "  The package may not own this header, or the layout is unexpected.",
                file, packageSpec));
            return false;
        }

        if (ext == ".winmd")
        {
            // WinRT metadata is a Windows-only feature - reject early off-Windows with a
            // guarded-import hint, mirroring CompileImportedFile's .winmd guard.
            if (!targetWindows_)
            {
                LogError(std::format("import package-nuget '{}': WinRT metadata (.winmd) is only supported when "
                                     "targeting Windows; guard the import with "
                                     "'if const (__WINDOWS__) {{ import ...; }}'.", file));
                return false;
            }
            // Search the resolved metadata dirs for the exact filename and route the absolute
            // path through the existing .winmd import pipeline.
            for (const auto& dir : res.winmdDirs)
            {
                std::error_code ec;
                std::filesystem::path cand = std::filesystem::path(dir) / file;
                if (std::filesystem::exists(cand, ec) && !ec)
                {
                    auto canon = std::filesystem::canonical(cand, ec);
                    return CompileWinmdFile(ec ? cand.string() : canon.string());
                }
            }
            if (lspMode)
            {
                if (verbose)
                    std::cout << std::format("[verbose] nuget: winmd '{}' not found in package '{}'; skipping for LSP analysis\n",
                        file, packageSpec);
                return true;
            }
            LogError(std::format(
                "import package-nuget: metadata '{}' not found in the winmd dirs of package '{}'.",
                file, packageSpec));
            return false;
        }

        LogError(std::format(
            "import package-nuget: '{}': only .h/.hpp/.hh headers and .winmd metadata are supported.",
            file));
        return false;
    }

