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
            LogErrorMessage("'{}' return: function '{}' has no return statement; explicit return required for type inference",
                            { "auto", functionName });
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
                LogErrorMessage("'{}' return: cannot mix '{}' and '{}' in function '{}'",
                                { "auto", "return;", "return <expr>;", functionName });
                return oldFn;
            }
            int srcToCur = CompareUpconvert(siteTy, unifiedTy);   // widens to current?
            int curToSrc = CompareUpconvert(unifiedTy, siteTy);   // current widens to new?
            if (srcToCur > 0)      { /* keep unifiedTy, siteTy widens up */ }
            else if (curToSrc > 0) { unifiedTy = siteTy; }
            else
            {
                LogErrorMessage("'{}' return: cannot unify return types in function '{}'",
                                { "auto", functionName });
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
        if (auto* existing = module->getFunction(newMangledName);
            existing && FunctionHasDefinition(existing))
        {
            // Discard the placeholder; the existing definition wins.
            if (!oldFn->use_empty())
                LogErrorMessage("'{}' return: recursive call in function '{}' is not yet supported",
                                { "auto", functionName });
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
            LogErrorMessage("'{}' return: recursive call in function '{}' is not yet supported - declare the return type explicitly",
                            { "auto", functionName });

        MigrateUniqueFieldBorrowReturn(oldFn, newFn);
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
        if (bb == nullptr || bb->getParent() == nullptr || cflat_llvm::GetTerminatorOrNull(bb) == nullptr)
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
        auto* term = cflat_llvm::GetTerminatorOrNull(builder->GetInsertBlock());
        if (!llvm::isa_and_nonnull<llvm::UncondBrInst, llvm::CondBrInst>(term))
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
        term->setMetadata(llvm::LLVMContext::MD_loop, loopID);
    }

std::string LLVMBackend::GetSourceFileName() const
{ return sourceFileName; }

std::string LLVMBackend::GetSourceFilePath() const
{ return currentSourceFilePath_; }

/*
 * The ROOT <assemblyIdentity> of a manifest is the one Windows uses to build the process
 * activation context, and a malformed one fails at PROCESS LAUNCH with Win32 error 14001
 * (ERROR_SXS_CANT_GEN_ACTCTX) - outside the compiler, naming no field. These checks are pure
 * string shape over the folded XML, so they are decidable on any host and fire under --check.
 * Nested (dependentAssembly) identities are deliberately not checked here: they name a foreign,
 * signed assembly whose token vocabulary the compiler cannot enumerate.
 */
bool LLVMBackend::ValidateManifestIdentity(const std::string& xml, const std::string& sourceFile,
                                           size_t line) const
{
    // Locate the root <assemblyIdentity>: the direct child of <assembly>, i.e. depth 1.
    std::string rootTag;
    int depth = 0;
    for (size_t pos = 0; (pos = xml.find('<', pos)) != std::string::npos; )
    {
        size_t end = xml.find('>', pos);
        if (end == std::string::npos) break;
        std::string_view tag(xml.data() + pos, end - pos + 1);
        pos = end + 1;
        if (tag.size() < 3 || tag[1] == '?' || tag[1] == '!') continue;
        if (tag[1] == '/') { --depth; continue; }
        if (depth == 1 && tag.starts_with("<assemblyIdentity"))
        {
            rootTag.assign(tag);
            break;
        }
        if (tag[tag.size() - 2] != '/') ++depth;
    }
    if (rootTag.empty()) return true;

    auto attribute = [&rootTag](std::string_view name) -> std::optional<std::string> {
        std::string needle = std::string(name) + "=\"";
        size_t at = rootTag.find(needle);
        if (at == std::string::npos || at == 0) return std::nullopt;
        char before = rootTag[at - 1];
        if (before != ' ') return std::nullopt;   // never match a suffix of a longer attribute
        size_t start = at + needle.size();
        size_t end = rootTag.find('"', start);
        if (end == std::string::npos) return std::nullopt;
        return rootTag.substr(start, end - start);
    };
    // LogError already stamps the manifest declaration's file/line, so the detail must not
    // repeat it; sourceFile/line only back the fallback when there is no ambient location.
    auto fail = [&](const std::string& detail) {
        if (sourceFileName.empty())
            LogError(std::format("manifest at {}:{}: {}",
                                 sourceFile.empty() ? "<unknown>" : sourceFile, line, detail));
        else
            LogError("manifest: " + detail);
        return false;
    };

    auto name = attribute("name");
    if (name && name->empty())
        return fail("the root <assemblyIdentity> has an empty 'name'. An assemblyIdentity must "
                    "name the assembly, e.g. name = \"Contoso.MyApp\", or omit the identity "
                    "entirely - a root identity is optional and most desktop apps do not need one.");

    auto version = attribute("version");
    if (version)
    {
        size_t parts = 1, digits = 0;
        bool wellFormed = !version->empty();
        for (char c : *version)
        {
            if (c == '.') { wellFormed = wellFormed && digits > 0; ++parts; digits = 0; }
            else if (c >= '0' && c <= '9') ++digits;
            else wellFormed = false;
        }
        if (!wellFormed || digits == 0 || parts != 4)
            return fail(std::format(
                "the root <assemblyIdentity> declares version = \"{}\", which is not a four-part "
                "version. It must be four dot-separated decimal numbers, e.g. \"1.0.0.0\".",
                *version));
    }

    auto token = attribute("publicKeyToken");
    if (token && token->empty())
        return fail("the root <assemblyIdentity> declares an empty 'publicKeyToken'. Windows "
                    "cannot build an activation context from it and the program fails at launch "
                    "with Win32 error 14001 (ERROR_SXS_CANT_GEN_ACTCTX), naming no field. Drop "
                    "'publicKeyToken' from the ROOT identity - only a dependentAssembly needs one "
                    "- or give the signing key's 16 hex digit token.");
    if (token && !token->empty())
    {
        bool hex = token->size() == 16;
        for (char c : *token)
            hex = hex && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
        if (!hex)
            return fail(std::format(
                "the root <assemblyIdentity> declares publicKeyToken = \"{}\", which is not a "
                "16 hex digit token. Give the signing key's token (e.g. \"6595b64144ccf1df\") or "
                "drop 'publicKeyToken' from the root identity.", *token));
    }

    auto type = attribute("type");
    if (type && !type->empty() && *type != "win32")
        return fail(std::format(
            "the root <assemblyIdentity> declares type = \"{}\". The only type a desktop "
            "application manifest may declare is \"win32\".", *type));

    auto arch = attribute("processorArchitecture");
    if (arch && !arch->empty())
    {
        static constexpr std::string_view kArches[] = {
            "*", "x86", "amd64", "ia64", "arm", "arm64", "msil" };
        bool known = false;
        for (auto candidate : kArches) known = known || *arch == candidate;
        if (!known)
            return fail(std::format(
                "the root <assemblyIdentity> declares processorArchitecture = \"{}\". It must be "
                "one of \"*\", \"x86\", \"amd64\", \"ia64\", \"arm\", \"arm64\", \"msil\".",
                *arch));
    }
    return true;
}

void LLVMBackend::RecordManifestFragment(const std::string& sourceFile, size_t line,
                                         const std::string& xml,
                                         std::vector<ManifestFragment::Leaf> leaves)
{
    for (const auto& fragment : manifestFragments_)
        if (fragment.Xml == xml)
            return;

    if (!ValidateManifestIdentity(xml, sourceFile, line)) return;

    auto location = [](const ManifestFragment::Leaf& leaf) {
        return std::format("{}:{}", leaf.SourceFile, leaf.Line);
    };
    std::unordered_map<std::string, const ManifestFragment::Leaf*> seen;
    for (const auto& fragment : manifestFragments_)
        for (const auto& leaf : fragment.Leaves)
            seen.emplace(leaf.Namespace + "\n" + leaf.LocalName, &leaf);
    std::vector<ManifestFragment::Leaf> uniqueLeaves;
    for (const auto& leaf : leaves)
    {
        std::string key = leaf.Namespace + "\n" + leaf.LocalName;
        auto [it, inserted] = seen.emplace(key, &leaf);
        if (!inserted && it->second->Text != leaf.Text)
        {
            LogError(std::format(
                "manifest [JsonText] conflict for '{}:{}': '{}' at {} versus '{}' at {}",
                leaf.Namespace, leaf.LocalName, it->second->Text, location(*it->second),
                leaf.Text, location(leaf)));
            return;
        }
        if (inserted || it->second == &leaf)
            uniqueLeaves.push_back(leaf);
    }
    manifestFragments_.push_back({ sourceFile, line, xml, std::move(uniqueLeaves) });
}

const std::vector<LLVMBackend::ManifestFragment>& LLVMBackend::GetManifestFragments() const
{ return manifestFragments_; }

std::optional<std::string> LLVMBackend::MergeManifestFragments() const
{
    if (manifestFragments_.empty()) return std::nullopt;

    auto location = [](const std::string& file, size_t line) {
        return std::format("{}:{}", file.empty() ? "<unknown>" : file, line);
    };
    auto parse = [](const std::string& xml, std::string& rootAttributes,
                    std::string& childContent) {
        constexpr std::string_view prefix = "<assembly";
        constexpr std::string_view suffix = "</assembly>";
        if (!xml.starts_with(prefix)) return false;
        size_t openEnd = xml.find('>');
        if (openEnd == std::string::npos) return false;
        rootAttributes = xml.substr(prefix.size(), openEnd - prefix.size());
        if (!rootAttributes.empty() && rootAttributes.back() == '/')
            rootAttributes.pop_back();
        if (openEnd > 0 && xml[openEnd - 1] == '/')
        {
            childContent.clear();
            return true;
        }
        if (xml.size() < suffix.size() || !xml.ends_with(suffix)) return false;
        size_t closeStart = xml.size() - suffix.size();
        childContent = xml.substr(openEnd + 1, closeStart - openEnd - 1);
        return true;
    };

    std::string rootAttributes;
    std::string children;
    bool haveRoot = false;
    for (const auto& fragment : manifestFragments_)
    {
        std::string fragmentAttributes;
        std::string fragmentChildren;
        if (!parse(fragment.Xml, fragmentAttributes, fragmentChildren))
        {
            LogError(std::format("manifest fragment from {} is not a valid assembly document",
                                 location(fragment.SourceFile, fragment.Line)));
            return std::nullopt;
        }
        if (!haveRoot)
        {
            rootAttributes = fragmentAttributes;
            haveRoot = true;
        }
        else if (rootAttributes != fragmentAttributes)
        {
            const auto& first = manifestFragments_.front();
            LogError(std::format(
                "manifest root attributes conflict between {} and {}",
                location(first.SourceFile, first.Line),
                location(fragment.SourceFile, fragment.Line)));
            return std::nullopt;
        }
        children += fragmentChildren;
    }

    std::unordered_map<std::string, const ManifestFragment::Leaf*> seen;
    for (const auto& fragment : manifestFragments_)
        for (const auto& leaf : fragment.Leaves)
        {
            std::string key = leaf.Namespace + "\n" + leaf.LocalName;
            auto [it, inserted] = seen.emplace(key, &leaf);
            if (!inserted && it->second->Text != leaf.Text)
            {
                LogError(std::format(
                    "manifest [JsonText] conflict for '{}:{}': '{}' at {} versus '{}' at {}",
                    leaf.Namespace, leaf.LocalName, it->second->Text,
                    location(it->second->SourceFile, it->second->Line), leaf.Text,
                    location(leaf.SourceFile, leaf.Line)));
                return std::nullopt;
            }
        }

    return "<assembly" + rootAttributes + ">" + children + "</assembly>";
}

void LLVMBackend::RecordCompileTimeStringConstant(const std::string& name, const std::string& value)
{ compileTimeStringConstants_[name] = value; }

std::optional<std::string> LLVMBackend::GetCompileTimeStringConstant(const std::string& name) const
{
    auto it = compileTimeStringConstants_.find(name);
    return it == compileTimeStringConstants_.end()
        ? std::nullopt : std::optional<std::string>(it->second);
}

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

std::vector<std::string> LLVMBackend::ScopedNameCandidates(const std::string& name, bool forceRoot) const
{
        std::vector<std::string> candidates;
        auto add = [&](const std::string& candidate) {
            if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
                candidates.push_back(candidate);
        };

        if (!forceRoot && !currentNamespace_.empty() && name.find('.') == std::string::npos)
        {
            std::string prefix = currentNamespace_;
            while (true)
            {
                add(prefix + "." + name);
                auto dot = prefix.rfind('.');
                if (dot == std::string::npos) break;
                prefix = prefix.substr(0, dot);
            }
        }

        if (name.find('.') != std::string::npos)
        {
            auto dot = name.find('.');
            std::string first = name.substr(0, dot);
            std::string rest = name.substr(dot + 1);
            std::string resolvedFirst = ResolveNamespace(first);
            add(resolvedFirst + "." + rest);
        }
        add(name);
        return candidates;
    }

std::string LLVMBackend::ResolveInterfaceName(const std::string& spelled) const
{
        std::string name = ResolveTypeAlias(spelled);
        for (const auto& candidate : ScopedNameCandidates(name))
            if (interfaceTable.count(candidate)) return candidate;
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

void LLVMBackend::SetAnalyzeDebugInfo(bool enabled)
{ analyzeDebugInfo_ = enabled; }

void LLVMBackend::SetLocale(const std::string& locale)
{ diagnosticLocalization_.SetLocale(locale); }

void LLVMBackend::SetLocaleDirectory(const std::string& directory)
{ diagnosticLocalization_.SetLocaleDirectory(directory); }

bool LLVMBackend::LoadLocale(bool verbose)
{
        llvm::TimeTraceScope localeScope("LocaleLoad");
        return diagnosticLocalization_.Load(verbose);
}

void LLVMBackend::SetLocaleTemplateCollection(bool enabled)
{ diagnosticLocalization_.SetCollectTemplates(enabled); }

bool LLVMBackend::WriteCollectedLocale(const std::string& locale, bool verbose) const
{ return diagnosticLocalization_.WriteCollectedCatalog(locale, verbose); }

std::string LLVMBackend::LocalizeMessage(std::string englishTemplate,
                                         std::vector<std::string> arguments) const
{ return diagnosticLocalization_.Localize(englishTemplate, arguments); }

std::function<std::string(std::string, std::vector<std::string>)>
LLVMBackend::MakeDiagnosticLocalizer() const
{ return [this](std::string t, std::vector<std::string> a) { return LocalizeMessage(std::move(t), std::move(a)); }; }

void LLVMBackend::SetVerbose(bool v)
{ verbose = v; }

void LLVMBackend::SetViewTraceEnabled(bool enabled)
{ viewTraceEnabled_ = enabled; }

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

void LLVMBackend::SetWindowsSubsystem(const std::string& v)
{ windowsSubsystem_ = v; }

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
        auto s = SerializedTav::From(tv);
        nlohmann::json j;
        j["t"] = s.TypeName;
        if (!s.VariableName.empty()) j["n"] = s.VariableName;
        if (s.Pointer)        j["p"]   = true;
        if (s.ElemPointer)    j["ep"]  = true;
        if (s.PointerDepth)   j["pd"]  = s.PointerDepth;
        if (s.IsInterface)    j["if"]  = true;
        if (s.IsInterfacePointer) j["ifp"] = true;
        if (s.IsNullable)     j["nl"]  = true;
        if (s.IsMove)         j["mv"]  = true;
        if (s.IsAdopt)        j["ad"]  = true;
        if (s.IsAlias)        j["al"]  = true;
        if (s.IsUniqueTypeArg) j["unt"] = true;
        if (s.ElementOwningUnique) j["eou"] = true;
        if (s.IsOwningSink)   j["osk"] = true;
        if (s.IsConsumeInferredSink) j["cis"] = true;
        if (s.IsBorrowOfUniqueElement) j["bue"] = true;
        if (s.IsBorrowOfAliasElement) j["bae"] = true;
        if (s.IsBond)          j["bd"] = true;
        if (s.IsUnique)        j["uq"] = true;
        if (s.CallConv != CallingConv::Default) j["cc"] = static_cast<int>(s.CallConv);
        if (s.LockThis)        j["lt"] = true;
        if (s.LockThisMode != LockMode::Exclusive) j["ltm"] = static_cast<int>(s.LockThisMode);
        if (!s.GuardedBy.empty()) j["gb"] = s.GuardedBy;
        if (s.IsFunctionPointer)
        {
            j["fp"]  = true;
            j["fpr"] = s.FuncPtrReturnTypeName;
            if (s.FuncPtrReturnPointer) j["fprp"] = true;
            if (s.FuncPtrReturnOwned) j["fpro"] = true;
            if (s.FuncPtrReturnAlias) j["fpra"] = true;
            if (s.FuncPtrReturnPointerDepth > 1) j["fprd"] = s.FuncPtrReturnPointerDepth;
            if (!s.FuncPtrReturnResolvedKey.empty()) j["fprk"] = s.FuncPtrReturnResolvedKey;
            nlohmann::json fps = nlohmann::json::array();
            for (const auto& p : s.FuncPtrParams)
            {
                nlohmann::json pj;
                pj["t"] = p.TypeName;
                if (p.Pointer) pj["p"]  = true;
                if (p.AllocAlignValue > 0) pj["aav"] = static_cast<int64_t>(p.AllocAlignValue);
                if (p.IsMove)  pj["mv"] = true;
                if (p.IsOwningSink) pj["osk"] = true;
                if (p.IsConsumeInferredSink) pj["cis"] = true;
                if (p.PointerDepth > 1) pj["pd"] = p.PointerDepth;
                if (!p.ResolvedTypeKey.empty()) pj["rk"] = p.ResolvedTypeKey;
                fps.push_back(pj);
            }
            j["fpp"] = fps;
        }
        if (s.ConstArraySize > 0) j["as"] = s.ConstArraySize;
        if (!s.ConstInnerDimensions.empty()) j["aid"] = s.ConstInnerDimensions;
        if (s.IsSimd) { j["sd"] = true; j["sdl"] = s.SimdLanes; }
        if (s.IsArrayView) j["av"] = true;
        if (s.AllocAlignValue > 0) j["aa"] = s.AllocAlignValue;
        return j;
    }

LLVMBackend::TypeAndValue LLVMBackend::TvFromJson(const SjVal& j)
{
        SerializedTav s;
        s.TypeName = j.value("t", std::string{});
        s.VariableName = j.value("n", std::string{});
        s.Pointer = j.value("p", false);
        s.ElemPointer = j.value("ep", false);
        s.PointerDepth = j.value("pd", 0);
        s.IsInterface = j.value("if", false);
        s.IsInterfacePointer = j.value("ifp", false);
        s.IsNullable = j.value("nl", false);
        s.IsMove = j.value("mv", false);
        s.IsAdopt = j.value("ad", false);
        s.IsAlias = j.value("al", false);
        s.IsUniqueTypeArg = j.value("unt", false);
        s.ElementOwningUnique = j.value("eou", false);
        s.IsOwningSink = j.value("osk", false);
        s.IsConsumeInferredSink = j.value("cis", false);
        s.IsBorrowOfUniqueElement = j.value("bue", false);
        s.IsBorrowOfAliasElement = j.value("bae", false);
        s.IsBond = j.value("bd", false);
        s.IsUnique = j.value("uq", false);
        s.CallConv = static_cast<CallingConv>(j.value("cc", 0));
        s.LockThis = j.value("lt", false);
        s.LockThisMode = static_cast<LockMode>(j.value("ltm", 0));
        s.GuardedBy = j.value("gb", std::string{});
        s.IsFunctionPointer = j.value("fp", false);
        if (s.IsFunctionPointer)
        {
            s.FuncPtrReturnTypeName = j.value("fpr", std::string{});
            s.FuncPtrReturnPointer = j.value("fprp", false);
            s.FuncPtrReturnOwned = j.value("fpro", false);
            s.FuncPtrReturnAlias = j.value("fpra", false);
            s.FuncPtrReturnPointerDepth = j.value("fprd", s.FuncPtrReturnPointerDepth);
            s.FuncPtrReturnResolvedKey = j.value("fprk", std::string{});
            if (j.contains("fpp"))
                for (const auto& pj : j["fpp"])
                {
                    SerializedTav::FuncPtrParam p;
                    p.TypeName = pj.value("t", std::string{});
                    p.Pointer = pj.value("p", false);
                    p.AllocAlignValue = pj.value("aav", uint64_t{0});
                    p.IsMove = pj.value("mv", false);
                    p.IsOwningSink = pj.value("osk", false);
                    p.IsConsumeInferredSink = pj.value("cis", false);
                    p.PointerDepth = pj.value("pd", p.PointerDepth);
                    p.ResolvedTypeKey = pj.value("rk", std::string{});
                    s.FuncPtrParams.push_back(std::move(p));
                }
        }
        s.ConstArraySize = j.value("as", uint64_t{0});
        if (j.contains("aid")) s.ConstInnerDimensions = j["aid"].to_u64_vector();
        s.IsSimd = j.value("sd", false);
        s.SimdLanes = j.value("sdl", uint64_t{0});
        s.IsArrayView = j.value("av", false);
        s.AllocAlignValue = j.value("aa", uint64_t{0});
        return s.ToTypeAndValue();
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
        if (!m.aliasTarget.empty()) j["al"] = m.aliasTarget;
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
        m.aliasTarget = j.value("al", std::string{});
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

nlohmann::json LLVMBackend::TypeAliasToJson(const CTypeAliasEntry& a)
{
        return {{"n", a.name}, {"t", a.target}, {"f", a.file},
                {"ln", a.line}, {"co", a.col}, {"ar", a.isAnonymousRecord}};
    }

LLVMBackend::CTypeAliasEntry LLVMBackend::TypeAliasFromJson(const SjVal& j)
{
        CTypeAliasEntry a;
        a.name = j.value("n", std::string{});
        a.target = j.value("t", std::string{});
        a.file = j.value("f", std::string{});
        a.line = j.value("ln", 1);
        a.col = j.value("co", 0);
        a.isAnonymousRecord = j.value("ar", false);
        return a;
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
        // v11 carries alias macros (`#define A B`); a v10 entry dropped every one of them.
        // v12 carries typedef aliases for the LSP symbol sink.
        // v13 records anonymous-struct typedef identity in that alias cache. v14 adopts the
        // canonical SerializedTav field set and key spellings, including the core fpp/as/aid keys.
        if (version != 14) return false;

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
            if (j.contains("typeAliases"))
                for (const auto& a : j["typeAliases"])
                    entry.typeAliases.push_back(TypeAliasFromJson(a));

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
        j["version"] = 14;
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
        nlohmann::json typeAliases = nlohmann::json::array();
        for (const auto& a : entry.typeAliases)
            typeAliases.push_back(TypeAliasToJson(a));
        j["typeAliases"] = typeAliases;

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
            LogRawError(err);
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
                LogErrorMessage(
                    "import package-vcpkg: port for header '{}' is not installed (no '{}'), "
                    "and 'vcpkg install' is disabled (--vcpkg-no-install).\n"
                    "  Run 'vcpkg install' yourself, or drop --vcpkg-no-install to let cflat install it.",
                    { header, res.includeDir });
                return false;
            }
            LogErrorMessage(
                "import package-vcpkg: header '{}' not found under '{}'.\n"
                "  The port may not own this header, or the install is incomplete.",
                { header, res.includeDir });
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
            LogErrorMessage(
                "import package-nuget: pri file '{}' was not found in package '{}' (probed '{}' "
                "and a recursive search of '{}').",
                { priName, packageSpec, primary.string(), packageFolder });
            return false;
        }

        std::string abs = fs::absolute(found, ec).string();
        if (!deployPriPath_.empty() && deployPriPath_ != abs)
        {
            LogErrorMessage(
                "import package-nuget: conflicting pri deployment - both '{}' and '{}' were "
                "requested as <exe>.pri. Only one pri may be deployed per output.",
                { deployPriPath_, abs });
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
                    LogErrorMessage(
                        "import package-nuget: '{}' - a .winmd cannot appear in a multi-entry group; "
                        "group a .winmd import on its own line.",
                        { f });
                    return false;
                }
                if (ext != ".h" && ext != ".hpp" && ext != ".hh")
                {
                    LogErrorMessage(
                        "import package-nuget: '{}' - a multi-entry group may contain only "
                        ".h/.hpp/.hh headers.",
                        { f });
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
            LogRawError(err);
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
                LogErrorMessage(
                    "import package-nuget: header '{}' was not found in the include dirs of package '{}'.\n"
                    "  Only package-owned headers may appear in a package-nuget group; a system header "
                    "(e.g. windows.h) may not ride in a package-nuget group.",
                    { f, packageSpec });
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
            LogErrorMessage(
                "import package-nuget: header '{}' not found in the include dirs of package '{}'.\n"
                "  The package may not own this header, or the layout is unexpected.",
                { file, packageSpec });
            return false;
        }

        if (ext == ".winmd")
        {
            // WinRT metadata is a Windows-only feature - reject early off-Windows with a
            // guarded-import hint, mirroring CompileImportedFile's .winmd guard.
            if (!targetWindows_)
            {
                LogErrorMessage("import package-nuget '{}': WinRT metadata (.winmd) is only supported when "
                                "targeting Windows; guard the import with "
                                "'if const (__WINDOWS__) { import ...; }'.",
                                { file });
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
            LogErrorMessage(
                "import package-nuget: metadata '{}' not found in the winmd dirs of package '{}'.",
                { file, packageSpec });
            return false;
        }

        LogErrorMessage(
            "import package-nuget: '{}': only .h/.hpp/.hh headers and .winmd metadata are supported.",
            { file });
        return false;
    }
