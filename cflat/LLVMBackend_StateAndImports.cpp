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
#include <atomic>
#include <mutex>

static std::mutex gCHeaderDiskCachePublishMutex;
static std::atomic<uint64_t> gCHeaderDiskCacheTempCounter{0};

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
        if (t->isIntegerTy(128))        { tv.TypeName = "i128"; return tv; }
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
                            { "auto", SpellFunctionSymbol(*this, functionName) });
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
                                { "auto", "return;", "return <expr>;", SpellFunctionSymbol(*this, functionName) });
                return oldFn;
            }
            int srcToCur = CompareUpconvert(siteTy, unifiedTy);   // widens to current?
            int curToSrc = CompareUpconvert(unifiedTy, siteTy);   // current widens to new?
            if (srcToCur > 0)      { /* keep unifiedTy, siteTy widens up */ }
            else if (curToSrc > 0) { unifiedTy = siteTy; }
            else
            {
                LogErrorMessage("'{}' return: cannot unify return types in function '{}'",
                                { "auto", SpellFunctionSymbol(*this, functionName) });
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
            // oldFn is a placeholder this compile just created, so every use of it is
            // materialized - use_empty() would assert on a lazily loaded core module.
            if (!oldFn->materialized_use_empty())
                LogErrorMessage("'{}' return: recursive call in function '{}' is not yet supported",
                                { "auto", SpellFunctionSymbol(*this, functionName) });
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
        // Materialized-only: oldFn is this compile's placeholder, and a lazy core module
        // makes the plain use_empty() assert under an assertions-enabled LLVM.
        if (!oldFn->materialized_use_empty())
            LogErrorMessage("'{}' return: recursive call in function '{}' is not yet supported - declare the return type explicitly",
                            { "auto", SpellFunctionSymbol(*this, functionName) });

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
 * Resolve and read an `embed("...")` asset. Containment is decided on the LEXICALLY normalized
 * path (so '..' is rejected as written) and again on the weakly-canonical one (so a symlink
 * cannot smuggle the read outside the source file's directory).
 */
std::optional<std::vector<uint8_t>> LLVMBackend::LoadEmbedFile(const std::string& literalPath,
                                                               const std::string& sourceFile,
                                                               std::string& resolvedPath,
                                                               std::string& error)
{
    resolvedPath.clear();
    error.clear();
    if (literalPath.empty())
    {
        error = "the path is empty";
        return std::nullopt;
    }

    std::filesystem::path relative(literalPath);
    // is_absolute() is false for "/abs/x" on Windows; the root tests catch it on every host.
    if (relative.is_absolute() || relative.has_root_directory() || relative.has_root_name())
    {
        error = "an absolute path is not allowed; embed resolves relative to the source file";
        return std::nullopt;
    }

    std::error_code ec;
    std::filesystem::path base = std::filesystem::path(sourceFile).parent_path();
    // The LSP analyzes a TEMP copy of the root document; its real directory is sourceFileDir_.
    if (!sourceFileDir_.empty() && sourceFile == analyzedRootPath_)
        base = sourceFileDir_;
    if (base.empty()) base = std::filesystem::current_path(ec);

    auto contains = [](const std::filesystem::path& directory,
                       const std::filesystem::path& candidate) {
        auto directoryText = directory.generic_string();
        auto candidateText = candidate.generic_string();
        if (!directoryText.empty() && directoryText.back() == '/') directoryText.pop_back();
        if (directoryText.empty() || candidateText.size() <= directoryText.size()) return false;
        return candidateText.compare(0, directoryText.size(), directoryText) == 0
            && candidateText[directoryText.size()] == '/';
    };

    auto lexicalBase = base.lexically_normal();
    auto lexicalFull = (base / relative).lexically_normal();
    if (!contains(lexicalBase, lexicalFull))
    {
        error = "the path escapes the directory of the source file that names it";
        return std::nullopt;
    }

    auto canonicalBase = std::filesystem::weakly_canonical(lexicalBase, ec);
    if (ec) canonicalBase = lexicalBase;
    auto canonicalFull = std::filesystem::weakly_canonical(lexicalFull, ec);
    if (ec) canonicalFull = lexicalFull;
    if (!contains(canonicalBase, canonicalFull))
    {
        error = "the path escapes the directory of the source file that names it";
        return std::nullopt;
    }

    if (!std::filesystem::exists(canonicalFull, ec) || ec)
    {
        error = "no such file";
        return std::nullopt;
    }
    if (!std::filesystem::is_regular_file(canonicalFull, ec) || ec)
    {
        error = "not a regular file";
        return std::nullopt;
    }

    // The same asset is read by the extent check, the initializer and the global-init arm;
    // serve the later reads from the cache keyed on the resolved path.
    const std::string cacheKey = canonicalFull.string();
    auto cached = embedFileCache_.find(cacheKey);
    if (cached != embedFileCache_.end())
    {
        resolvedPath = cacheKey;
        return cached->second;
    }

    std::ifstream input(canonicalFull, std::ios::binary);
    if (!input)
    {
        error = "the file could not be opened for reading";
        return std::nullopt;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
    if (input.bad())
    {
        error = "the file could not be read";
        return std::nullopt;
    }
    // A zero-byte asset has no 'u8[N]' spelling (N = 0 is not an array), and C23 '#embed'
    // rejects the empty case for the same reason. Reject it once, here, for both shapes.
    if (bytes.empty())
    {
        error = "the file is empty";
        return std::nullopt;
    }
    resolvedPath = cacheKey;
    embedFileCache_.emplace(cacheKey, bytes);
    return bytes;
}

void LLVMBackend::RecordEmbedDependency(const std::string& resolvedPath)
{
    if (resolvedPath.empty()) return;
    if (std::find(embeddedAssets_.begin(), embeddedAssets_.end(), resolvedPath)
        == embeddedAssets_.end())
        embeddedAssets_.push_back(resolvedPath);
    RecordDependency(resolvedPath);
}

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

bool LLVMBackend::RecordApplicationInfo(const std::string& sourceFile, size_t line,
                                        cflat::appres::AppInfoData info)
{
    if (applicationInfo_.has_value()) return false;
    applicationInfo_ = std::move(info);
    applicationSourceFile_ = sourceFile;
    applicationLine_ = line;
    return true;
}

/*
 * Define the constants core/application.cb declares `extern`: four `i8*` strings plus the
 * `__cflat_app_declared` int flag. They are always defined when the file is imported, so a
 * program without an `application` declaration links and reads back empty strings / 0 rather
 * than failing at link time.
 */
void LLVMBackend::MaterializeApplicationConstants()
{
    if (module == nullptr) return;
    const auto& info = applicationInfo_;
    const std::string product = !info ? std::string()
        : (info->Version.Product.empty() ? info->Version.File : info->Version.Product);
    const std::pair<const char*, std::string> constants[] = {
        { "__cflat_app_name",       info ? info->Name : std::string() },
        { "__cflat_app_version",    product },
        { "__cflat_app_identifier", info ? info->Identifier : std::string() },
        { "__cflat_app_copyright",  info ? info->Copyright : std::string() },
    };
    for (const auto& [symbol, text] : constants)
    {
        auto* slot = module->getNamedGlobal(symbol);
        if (slot == nullptr || slot->hasInitializer()) continue;
        auto* bytes = new llvm::GlobalVariable(*module,
            llvm::ArrayType::get(llvm::Type::getInt8Ty(*context), text.size() + 1), true,
            llvm::GlobalValue::PrivateLinkage,
            llvm::ConstantDataArray::getString(*context, text, true),
            std::string(symbol) + ".text");
        bytes->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        slot->setInitializer(bytes);
        slot->setConstant(true);
        slot->setLinkage(llvm::GlobalValue::InternalLinkage);
    }

    // Application.declared(): lets a library (ui_native) skip application-only work when the
    // program carries no declaration, without inspecting the string constants.
    auto* flag = module->getNamedGlobal("__cflat_app_declared");
    if (flag != nullptr && !flag->hasInitializer() && flag->getValueType()->isIntegerTy())
    {
        flag->setInitializer(llvm::ConstantInt::get(flag->getValueType(), info ? 1 : 0));
        flag->setConstant(true);
        flag->setLinkage(llvm::GlobalValue::InternalLinkage);
    }
}

/*
 * An .ico is a Windows container and an .icns a macOS one; each is an error on the other
 * target. Keyed on the TARGET, not the host, so a cross-compile is judged the same way.
 */
bool LLVMBackend::ValidateApplicationForTarget(bool windowsTarget) const
{
    if (!applicationInfo_) return true;
    const char* container = nullptr;
    if (windowsTarget && applicationInfo_->IconKind == cflat::appres::AppIconKind::Icns)
        container = "icns";
    else if (!windowsTarget && applicationInfo_->IconKind == cflat::appres::AppIconKind::Ico)
        container = "ico";
    if (container == nullptr) return true;
    LogError(std::format(
        "application declared at {}:{} uses an .{} icon, which the {} target cannot consume; "
        "supply a PNG set instead", applicationSourceFile_, applicationLine_, container,
        windowsTarget ? "Windows" : "macOS"));
    return false;
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

void LLVMBackend::RegisterScopedEnumType(const std::string& enumName)
{
        if (!enumName.empty()) scopedEnumTypes_.insert(enumName);
    }

bool LLVMBackend::IsScopedEnumTypeName(const std::string& name) const
{
        const std::string key = ResolveEnumTypeName(name);
        return !key.empty() && scopedEnumTypes_.count(key) != 0;
    }

bool LLVMBackend::IsScopedEnumMatch(const TypeAndValue& from, const TypeAndValue& to) const
{
        const bool fromScoped = from.IsScopedEnum || IsScopedEnumTypeName(from.TypeName);
        if (!fromScoped) return true;
        const bool toScoped = to.IsScopedEnum || IsScopedEnumTypeName(to.TypeName);
        if (!toScoped) return false;
        const std::string fromKey = ResolveEnumTypeName(from.TypeName);
        const std::string toKey = ResolveEnumTypeName(to.TypeName);
        if ((!fromKey.empty() && !toKey.empty()) ? fromKey != toKey
                                                 : from.TypeName != to.TypeName)
            return false;
        if (from.Pointer == to.Pointer)
            return from.ElemPointer == to.ElemPointer;
        return !from.Pointer && to.Pointer && to.IsAlias && !to.ElemPointer;
}

std::string LLVMBackend::GetEnumBackingType(const std::string& enumName) const
{
        auto it = enumBackingTypes.find(enumName);
        return it != enumBackingTypes.end() ? it->second : std::string();
    }

namespace {

/*
 * Enumerator values are folded in a wide signed intermediate so a u64 maximum, an i64 minimum
 * and an out-of-range literal are all representable before the backing-type range check runs.
 * llvm::APInt rather than a compiler-specific 128-bit integer type, which MSVC lacks.
 */
constexpr unsigned kEnumEvalBits = 256;

llvm::APInt EnumWide(int64_t v)
{
    return llvm::APInt(kEnumEvalBits, (uint64_t)v, true);
}

// Decimal spelling of the signed intermediate, for the range diagnostic.
std::string EnumWideToString(const llvm::APInt& v)
{
    llvm::SmallString<64> buf;
    v.toString(buf, 10, true);
    return std::string(buf.c_str());
}

/*
 * Magnitude of a plain integer literal token. Decimal / 0x / 0b, with the width suffixes and
 * digit separators stripped. Anything else (char literal, float, string) is rejected so the
 * caller falls back to the codegen folder rather than inventing a value.
 */
bool ParseEnumLiteralMagnitude(const std::string& raw, llvm::APInt& out)
{
    std::string t;
    for (char c : raw) if (c != '_') t += c;
    while (!t.empty() && (t.back() == 'u' || t.back() == 'U' || t.back() == 'l' || t.back() == 'L'))
        t.pop_back();
    if (t.empty()) return false;
    int base = 10;
    size_t i = 0;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) { base = 16; i = 2; }
    else if (t.size() > 2 && t[0] == '0' && (t[1] == 'b' || t[1] == 'B')) { base = 2; i = 2; }
    if (i >= t.size()) return false;
    llvm::APInt v(kEnumEvalBits, 0);
    const llvm::APInt radix(kEnumEvalBits, (uint64_t)base);
    for (; i < t.size(); i++)
    {
        char c = t[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        if (d >= base) return false;
        bool overflow = false;
        v = v.umul_ov(radix, overflow);
        if (overflow) return false;
        v = v.uadd_ov(llvm::APInt(kEnumEvalBits, (uint64_t)d), overflow);
        if (overflow) return false;
    }
    // Leave room for the sign: the caller negates this magnitude in the same width.
    if (v.isSignBitSet()) return false;
    out = v;
    return true;
}

void CollectEnumTerminals(antlr4::tree::ParseTree* node,
                          std::vector<antlr4::tree::TerminalNode*>& out)
{
    if (auto* t = dynamic_cast<antlr4::tree::TerminalNode*>(node)) { out.push_back(t); return; }
    for (auto* c : node->children) CollectEnumTerminals(c, out);
}

/*
 * Fold an enumerator initializer WITHOUT codegen, so the pre-pass can register values. Covers a
 * signed integer literal and a reference to an earlier member of the same enum - the shapes the
 * language actually uses. Everything else returns false and defers to the codegen folder.
 */
bool EvalEnumeratorStatic(CFlatParser::ConditionalExpressionContext* cond,
                          const std::string& enumName,
                          const std::unordered_map<std::string, llvm::APInt>& prior,
                          llvm::APInt& out)
{
    if (cond == nullptr) return false;
    std::vector<antlr4::tree::TerminalNode*> toks;
    CollectEnumTerminals(cond, toks);
    if (toks.empty()) return false;
    bool negate = false;
    size_t start = 0;
    if (toks[0]->getText() == "-" || toks[0]->getText() == "+")
    {
        negate = toks[0]->getText() == "-";
        start = 1;
    }
    size_t n = toks.size() - start;
    auto memberValue = [&](const std::string& name, llvm::APInt& v) {
        auto it = prior.find(name);
        if (it == prior.end()) return false;
        v = it->second;
        return true;
    };
    auto signApply = [&](const llvm::APInt& v) { return negate ? llvm::APInt(-v) : v; };
    if (n == 1)
    {
        auto* tn = toks[start];
        size_t type = tn->getSymbol()->getType();
        if (type == CFlatParser::Constant)
        {
            llvm::APInt mag(kEnumEvalBits, 0);
            if (!ParseEnumLiteralMagnitude(tn->getText(), mag)) return false;
            out = signApply(mag);
            return true;
        }
        if (type == CFlatParser::Identifier)
        {
            llvm::APInt v(kEnumEvalBits, 0);
            if (!memberValue(tn->getText(), v)) return false;
            out = signApply(v);
            return true;
        }
        return false;
    }
    // `EnumName.Member` for a member declared earlier in this same enum.
    if (n == 3 && toks[start]->getText() == enumName && toks[start + 1]->getText() == ".")
    {
        llvm::APInt v(kEnumEvalBits, 0);
        if (!memberValue(toks[start + 2]->getText(), v)) return false;
        out = signApply(v);
        return true;
    }
    return false;
}

}  // namespace

void LLVMBackend::RegisterEnumSpecifier(CFlatParser::EnumSpecifierContext* ctx,
        const std::string& namespaceName,
        std::unordered_set<std::string>* constFoldableGlobals,
        const std::function<bool(CFlatParser::ConditionalExpressionContext*, llvm::APInt&)>& dynamicEval)
{
        if (ctx == nullptr) return;
        auto* id = ctx->Identifier();
        std::string enumName = id ? id->getText() : "";
        auto* typeSpec = ctx->typeSpecifier();
        PrimitiveTypeError backingError;
        std::string backingType = typeSpec ? CanonicalTypeSpecifierText(
            typeSpec, ctx->multiWordTypeSuffix(), false, &backingError) : "int";
        if (HasPrimitiveTypeError(backingError))
        {
            SetSourceLocation(ctx->getStart()->getLine(), ctx->getStart()->getCharPositionInLine());
            LogError(LocalizePrimitiveTypeError(this, backingError));
            return;
        }
        std::string resolvedBacking = ResolveTypeAlias(backingType);
        std::string ns = namespaceName.empty() ? currentNamespace_ : namespaceName;
        std::string scopedName = (enumName.empty() || ns.empty()) ? enumName : ns + "." + enumName;

        // Type facts go in first and unconditionally - a signature and a qualified spelling need
        // them even when a value does not fold. Registry keeps the RESOLVED backing (`enum AB :
        // Byte` must still read as an integer); `backingType` stays the spelling for diagnostics.
        if (!enumName.empty())
        {
            RegisterNamespace(enumName);
            if (scopedName != enumName) RegisterNamespace(scopedName);
            RegisterEnumBackingType(scopedName, resolvedBacking);
        }

        auto* list = ctx->enumeratorList();
        if (list == nullptr) return;
        auto enumerators = list->enumerator();
        if (enumerators.empty()) return;

        std::string prefix = scopedName.empty() ? "" : scopedName + ".";
        // Enum members are compile-time constants, foldable in an `if const` condition and in a
        // case label. Recorded before the early-out: the codegen pass owns this set.
        if (constFoldableGlobals)
            for (auto* e : enumerators)
                constFoldableGlobals->insert(prefix + e->enumerationConstant()->getText());

        // Declaration site, so a second visit of THIS declaration (the codegen pass, a re-import)
        // is the no-op while a second, different declaration of the same enum is a redefinition.
        std::string site = std::to_string(ctx->getStart()->getLine()) + ":"
            + std::to_string(ctx->getStart()->getCharPositionInLine()) + "@"
            + ctx->getStart()->getInputStream()->getSourceName();
        if (!scopedName.empty())
        {
            auto& sites = enumDeclSites_[scopedName];
            if (sites.count(site)) return;
            if (!sites.empty())
            {
                SetSourceLocation(ctx->getStart()->getLine(), ctx->getStart()->getCharPositionInLine());
                LogErrorMessage("enum '{}' is already defined", { enumName });
                return;
            }
        }
        if (globalNamedVariable.count(prefix + enumerators[0]->enumerationConstant()->getText()))
            return;  // members restored from the compiler cache, not from this parse

        TypeAndValue probe;
        probe.TypeName = resolvedBacking;
        const int bits = probe.IsInteger();
        const bool isUnsigned = probe.IsUnsignedInteger() != -1;

        llvm::APInt current = EnumWide(0);
        std::unordered_map<std::string, llvm::APInt> prior;
        std::vector<std::pair<std::string, llvm::APInt>> members;
        for (auto* e : enumerators)
        {
            std::string name = e->enumerationConstant()->getText();
            llvm::APInt value = current;
            if (auto* cexpr = e->constantExpression())
            {
                auto* cond = cexpr->conditionalExpression();
                if (!EvalEnumeratorStatic(cond, enumName, prior, value))
                {
                    // Pre-pass with no folder: leave this whole enum's VALUES to codegen, which
                    // re-visits the declaration and finds no member global registered.
                    if (!dynamicEval) return;
                    SetSourceLocation(e->getStart()->getLine(), e->getStart()->getCharPositionInLine());
                    llvm::APInt folded(kEnumEvalBits, 0);
                    if (!dynamicEval(cond, folded))
                        LogErrorMessage("enum value must be a constant integer expression");
                    // int-typed folds (<= 32 bits) keep their sign so `-1 - 0` under u32 is
                    // rejected as -1; wider folds widen the way the backing type reads them.
                    else if (folded.getBitWidth() <= 32 || !isUnsigned)
                        value = folded.sextOrTrunc(kEnumEvalBits);
                    else
                        value = folded.zextOrTrunc(kEnumEvalBits);
                }
            }
            if (bits > 0)
            {
                llvm::APInt lo = isUnsigned ? llvm::APInt(kEnumEvalBits, 0)
                                            : llvm::APInt::getSignedMinValue(bits).sext(kEnumEvalBits);
                llvm::APInt hi = isUnsigned ? llvm::APInt::getMaxValue(bits).zext(kEnumEvalBits)
                                            : llvm::APInt::getSignedMaxValue(bits).sext(kEnumEvalBits);
                if (value.slt(lo) || value.sgt(hi))
                {
                    SetSourceLocation(e->getStart()->getLine(), e->getStart()->getCharPositionInLine());
                    LogErrorMessage("enum value '{}' does not fit the backing type '{}'",
                                    { EnumWideToString(value), backingType });
                }
            }
            prior[name] = value;
            members.emplace_back(name, value);
            current = value + 1;
        }

        for (const auto& [name, value] : members)
        {
            TypeAndValue tv;
            // The enum's own name is the member's type so field/member lookup and overload
            // resolution keep the enum identity; EnumBacking carries the width and signedness.
            tv.TypeName = scopedName.empty() ? resolvedBacking : scopedName;
            tv.EnumBacking = scopedName.empty() ? std::string() : resolvedBacking;
            tv.VariableName = prefix + name;
            tv.Pointer = false;

            llvm::Constant* c = nullptr;
            if (bits > 0)
                c = llvm::ConstantInt::get(*context, value.trunc(bits));
            else
                c = CreateConstant(resolvedBacking, EnumWideToString(value));
            CreateGlobalVariable(tv, c, false, 0, false, isUnsigned);
        }
        if (!scopedName.empty()) enumDeclSites_[scopedName].insert(site);
    }

std::string LLVMBackend::ResolveEnumTypeName(const std::string& spelled) const
{
        if (spelled.empty()) return {};
        // Two hops at most: the written spelling, then one `using D = Dir;` alias step. Each hop
        // walks the enclosing namespaces so a sibling enum resolves unqualified.
        std::string cur = spelled;
        for (int hop = 0; hop < 2; hop++)
        {
            for (const auto& candidate : ScopedNameCandidates(cur))
                if (enumBackingTypes.count(candidate)) return candidate;
            std::string aliased = ResolveTypeAlias(cur);
            if (aliased == cur) break;
            cur = aliased;
        }
        return {};
    }

bool LLVMBackend::TryGetEnumMemberInt(const std::string& enumSpelling, const std::string& member,
                                      int64_t& out) const
{
        std::string enumKey = ResolveEnumTypeName(enumSpelling);
        if (enumKey.empty()) return false;
        auto it = globalNamedVariable.find(enumKey + "." + member);
        if (it == globalNamedVariable.end())
        {
            const size_t namespaceEnd = enumKey.rfind('.');
            if (namespaceEnd != std::string::npos)
                it = globalNamedVariable.find(enumKey.substr(0, namespaceEnd) + "." + member);
        }
        if (it == globalNamedVariable.end() || it->second == nullptr) return false;
        auto* ci = llvm::dyn_cast_or_null<llvm::ConstantInt>(it->second->getInitializer());
        if (ci == nullptr || ci->getBitWidth() > 64) return false;
        out = ci->getSExtValue();
        return true;
    }

bool LLVMBackend::TryGetScanTimeTypeSize(const std::string& typeName, int64_t& out) const
{
        if (typeName.empty()) return false;
        std::string name = typeName;
        // A pointer of any depth is one target pointer, so strip the suffix before the rest.
        if (name.back() == '*')
        {
            if (module == nullptr) return false;
            out = (int64_t)module->getDataLayout().getPointerSize();
            return true;
        }
        name = ResolveTypeAlias(name);
        if (std::string enumKey = ResolveEnumTypeName(name); !enumKey.empty())
            name = GetEnumBackingType(enumKey);
        TypeAndValue probe;
        probe.TypeName = name;
        if (int bits = probe.IsInteger(); bits > 0) { out = bits / 8; return true; }
        if (int bits = probe.IsFloatingPoint(); bits > 0) { out = bits / 8; return true; }
        if (name == "bool") { out = 1; return true; }
        // Aggregates and anything else are left undecidable: the scan-time layout may not exist
        // yet, and the main pass decides them for real.
        return false;
    }

bool LLVMBackend::TryGetScanTimeIntegerCast(const std::string& typeName, int64_t value,
                                            int64_t& out) const
{
        if (typeName.empty() || typeName.back() == '*') return false;
        std::string name = ResolveTypeAlias(typeName);
        if (std::string enumKey = ResolveEnumTypeName(name); !enumKey.empty())
            name = GetEnumBackingType(enumKey);
        if (name == "bool") { out = value != 0 ? 1 : 0; return true; }
        TypeAndValue probe;
        probe.TypeName = name;
        const int bits = probe.IsInteger();
        if (bits <= 0) return false;
        if (bits >= 64) { out = value; return true; }
        const uint64_t mask = (uint64_t(1) << bits) - 1;
        const uint64_t truncated = (uint64_t)value & mask;
        out = probe.IsUnsignedInteger() != -1
            ? (int64_t)truncated
            : llvm::APInt(bits, truncated).getSExtValue();
        return true;
    }

bool LLVMBackend::IsNamespace(const std::string& name) const
{
        for (const auto& frame : std::ranges::reverse_view(stackNamedVariable))
            if (frame.namespaceAliases.count(name)) return true;
        return namespaceTable.count(name) > 0 || namespaceAliasTable.count(name) > 0
            || ResolveNamespace(name) != name;
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

// Safety net for the using-directive worklists below: a hang with no output is the worst
// failure mode a compiler has, so an unbounded search reports the name instead of spinning.
// Scaled by the directive count so a long but legitimate directive chain never trips it.
static size_t UsingDirectiveWorklistCap(size_t directiveCount)
{
        return 4096 + 8 * directiveCount;
}

std::string LLVMBackend::ResolveNamespaceAliasExact(const std::string& name) const
{
        for (const auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            auto it = frame.namespaceAliases.find(name);
            if (it != frame.namespaceAliases.end()) return it->second;
        }
        auto it = namespaceAliasTable.find(name);
        if (it != namespaceAliasTable.end()) return it->second;
        return name;
    }

std::string LLVMBackend::ResolveNamespace(const std::string& name) const
{
        for (const auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            auto it = frame.namespaceAliases.find(name);
            if (it != frame.namespaceAliases.end()) return it->second;
        }
        auto it = namespaceAliasTable.find(name);
        if (it != namespaceAliasTable.end()) return it->second;

        // A using-directive can expose a nested namespace through a parent namespace, as in
        // `namespace torch { using namespace at; }` followed by `torch::indexing::Slice`.
        if (name.find('.') == std::string::npos) return name;
        // A rewrite may only fire at or past the end of the head the previous rewrite wrote
        // (headEnd). Otherwise a directive that nominates its own child re-matches its own
        // output forever: `namespace N { using namespace detail; }` gives N.x -> N.detail.x -> ...
        std::vector<std::pair<std::string, size_t>> pending{ { name, size_t{0} } };
        std::unordered_set<std::string> visited{ name };
        for (size_t i = 0; i < pending.size(); ++i)
        {
            const std::string current = pending[i].first;  // by value: push_back below may reallocate pending
            const size_t headEnd = pending[i].second;
            for (size_t prefixEnd = current.size(); prefixEnd != std::string::npos && prefixEnd >= headEnd; )
            {
                const std::string prefix = current.substr(0, prefixEnd);
                auto directive = cxxUsingDirectives_.find(prefix);
                if (directive != cxxUsingDirectives_.end())
                    for (const std::string& nominated : directive->second)
                    {
                        const std::string candidate = nominated + current.substr(prefixEnd);
                        if (candidate != current
                            && (namespaceTable.count(candidate) != 0
                                || namespaceAliasTable.count(candidate) != 0))
                            return candidate;
                        if (!visited.insert(candidate).second) continue;
                        if (pending.size() >= UsingDirectiveWorklistCap(cxxUsingDirectives_.size()))
                        {
                            LogError(std::format("could not resolve '{}' through using-directives: "
                                "the candidate list passed {} entries", name, UsingDirectiveWorklistCap(cxxUsingDirectives_.size())));
                            return name;
                        }
                        pending.emplace_back(candidate, nominated.size());
                    }
                if (prefixEnd == 0) break;
                const size_t dot = current.rfind('.', prefixEnd - 1);
                if (dot == std::string::npos) break;
                prefixEnd = dot;
            }
        }
        return name;
    }

std::vector<std::string> LLVMBackend::ScopedNameCandidates(const std::string& name,
        ScopedLookupOptions opts) const
{
        return ScopedNameCandidatesIn(currentNamespace_, name, opts);
    }

std::vector<std::string> LLVMBackend::ScopedNameCandidatesIn(const std::string& fromNamespace,
        const std::string& name, ScopedLookupOptions opts) const
{
        const bool forceRoot = opts.ForceRoot;
        std::vector<std::string> candidates;
        auto add = [&](const std::string& candidate) {
            if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
                candidates.push_back(candidate);
        };

        if (!forceRoot && !fromNamespace.empty()
            && (opts.PrefixDottedNames || name.find('.') == std::string::npos))
        {
            std::string prefix = fromNamespace;
            while (true)
            {
                add(prefix + "." + name);
                auto dot = prefix.rfind('.');
                if (dot == std::string::npos) break;
                prefix = prefix.substr(0, dot);
            }
        }

        // Verbatim spelling outranks the alias hop: a real namespace named like an alias wins,
        // the same order struct lookup uses, so both pre-scans queue one canonical key.
        add(name);
        if (opts.ResolveFirstComponentAlias && name.find('.') != std::string::npos)
        {
            auto dot = name.find('.');
            std::string first = name.substr(0, dot);
            std::string rest = name.substr(dot + 1);
            std::string resolvedFirst = ResolveNamespace(first);
            add(resolvedFirst + "." + rest);
        }
        return candidates;
    }

std::string LLVMBackend::ResolveInterfaceName(const std::string& spelled) const
{
        std::string name = ResolveTypeAlias(spelled);
        if (std::string key = FirstVisibleScopedKey(name,
                [this](const std::string& c) { return interfaceTable.count(c) != 0; });
            !key.empty())
            return key;
        std::string qualified = ResolveTypeAlias(ResolveQualifiedName(name));
        return interfaceTable.count(qualified) ? qualified : name;
    }

std::string LLVMBackend::ResolveQualifiedName(const std::string& name) const
{
        return ResolveQualifiedName(name, false);
    }

std::string LLVMBackend::ResolveQualifiedName(const std::string& name, bool forceRoot) const
{
        const auto isPublishedCxxName = [this](const std::string& candidate) {
            std::string cxxName = candidate;
            for (size_t pos = 0;
                 (pos = cxxName.find('.', pos)) != std::string::npos; pos += 2)
                cxxName.replace(pos, 1, "::");
            return std::any_of(cxxImportGroups_.begin(), cxxImportGroups_.end(),
                [&](const CxxImportGroup& group) {
                    return group.publishedNames.count(cxxName) != 0;
                });
        };
        // Bare name referenced inside a namespace body: prefer an enclosing-namespace
        // sibling (e.g. inside "N", a bare "helper" resolves to "N.helper") before
        // falling back to a top-level/global symbol. Walk outward through parent
        // namespaces so a nested "Outer.Inner" also sees "Outer" members. A more-local
        // match wins; if no qualified sibling exists the bare name resolves below.
        if (!forceRoot && !currentNamespace_.empty() && name.find('.') == std::string::npos)
        {
            if (std::string key = FirstVisibleScopedKey(name, [this, &isPublishedCxxName](const std::string& c) {
                    return dataStructures.count(c) || interfaceTable.count(c)
                        || functionTable.count(c) || globalNamedVariable.count(c)
                        || cxxRecordEntries_.count(c) || cxxClasses_.count(c)
                        || cxxCflatToCxxSpelling_.count(c) || isPublishedCxxName(c); });
                !key.empty())
                return key;
        }

        if (dataStructures.count(name) || interfaceTable.count(name) || functionTable.count(name)
            || cxxRecordEntries_.count(name) || cxxClasses_.count(name)
            || cxxCflatToCxxSpelling_.count(name))
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

        // Walk up from the (possibly expanded) prefix toward the root. The BARE last component is
        // deliberately not a candidate here: a qualified spelling never falls back to a global of
        // the same tail name, it stays unresolved and is reported verbatim.
        std::string key = FirstVisibleScopedKeyIn(nsPrefix, lastName, [&](const std::string& c) {
            if (c == lastName) return false;
            return dataStructures.count(c) != 0 || interfaceTable.count(c) != 0
                || functionTable.count(c) != 0;
        }, ScopedLookupOptions{ .ResolveFirstComponentAlias = false });
        if (!key.empty()) return key;

        const std::string qualified = nsPrefix + "." + lastName;
        const auto isKnownQualified = [this, &isPublishedCxxName](const std::string& candidate) {
            return dataStructures.count(candidate) != 0
                || interfaceTable.count(candidate) != 0
                || functionTable.count(candidate) != 0
                || cxxRecordEntries_.count(candidate) != 0
                || cxxClasses_.count(candidate) != 0
                || cxxCflatToCxxSpelling_.count(candidate) != 0
                || isPublishedCxxName(candidate)
                || cxxFunctionSignatures_.count(candidate) != 0
                || globalNamedVariable.count(candidate) != 0
                || HasCxxFunctionTemplate(candidate)
                || ResolveTypeAlias(candidate) != candidate
                || !ResolveEnumTypeName(candidate).empty();
        };
        const std::string through = ResolveThroughUsingDirectives(qualified, isKnownQualified);
        return through != qualified || isKnownQualified(qualified) ? through : name;
}

std::string LLVMBackend::ResolveThroughUsingDirectives(
        const std::string& qualified,
        const std::function<bool(const std::string&)>& predicate) const
{
        if (qualified.empty() || predicate(qualified)) return qualified;
        const size_t dot = qualified.rfind('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 >= qualified.size()) return qualified;

        const std::string namespaceName = qualified.substr(0, dot);
        const std::string memberName = qualified.substr(dot + 1);
        // Same head-progress rule as ResolveNamespace: a rewrite may only fire at or past the
        // end of the head the previous rewrite wrote, or a directive nominating its own child
        // rewrites its own output forever.
        std::vector<std::pair<std::string, size_t>> pending{ { namespaceName, size_t{0} } };
        std::unordered_set<std::string> visited{ namespaceName };
        std::vector<std::string> hits;
        for (size_t i = 0; i < pending.size(); ++i)
        {
            const std::string current = pending[i].first;  // by value: push_back below may reallocate pending
            const size_t headEnd = pending[i].second;
            for (size_t prefixEnd = current.size(); prefixEnd != std::string::npos && prefixEnd >= headEnd; )
            {
                const std::string prefix = current.substr(0, prefixEnd);
                auto it = cxxUsingDirectives_.find(prefix);
                if (it != cxxUsingDirectives_.end())
                    for (const std::string& nominated : it->second)
                    {
                        const std::string suffix = current.substr(prefixEnd);
                        const std::string nominatedNamespace = nominated + suffix;
                        const std::string candidate = nominatedNamespace + "." + memberName;
                        if (predicate(candidate)
                            && std::find(hits.begin(), hits.end(), candidate) == hits.end())
                            hits.push_back(candidate);
                        if (!visited.insert(nominatedNamespace).second) continue;
                        if (pending.size() >= UsingDirectiveWorklistCap(cxxUsingDirectives_.size()))
                        {
                            LogError(std::format("could not resolve '{}' through using-directives: "
                                "the candidate list passed {} entries", qualified,
                                UsingDirectiveWorklistCap(cxxUsingDirectives_.size())));
                            return qualified;
                        }
                        pending.emplace_back(nominatedNamespace, nominated.size());
                    }
                if (prefixEnd == 0) break;
                const size_t dot = current.rfind('.', prefixEnd - 1);
                if (dot == std::string::npos) break;
                prefixEnd = dot;
            }
        }
        if (hits.size() > 1)
            LogError(std::format(
                "ambiguous qualified name '{}' through using-directives: candidates '{}' and '{}'",
                qualified, hits[0], hits[1]));
        return hits.empty() ? qualified : hits.front();
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


void LLVMBackend::SetCppStrictNoexcept(bool v)
{ cppStrictNoexcept_ = v; }

bool LLVMBackend::SetCppStandard(const std::string& standard)
{
        if (!IsValidCppStandard(standard))
        {
            LogError(std::format("invalid --cpp-std '{}'; accepted values: c++17, c++20, c++23, "
                                 "c++26, gnu++17, gnu++20, gnu++23, gnu++26", standard));
            return false;
        }
        cppStandard_ = standard;
        return true;
}

bool LLVMBackend::IsValidCppStandard(const std::string& standard)
{
        static const std::vector<std::string> accepted = {
            "c++17", "c++20", "c++23", "c++26",
            "gnu++17", "gnu++20", "gnu++23", "gnu++26"
        };
        return std::find(accepted.begin(), accepted.end(), standard) != accepted.end();
}

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
        const std::string cacheDir = base + "/cheaders";
        static std::mutex cleanupMutex;
        static std::set<std::string> cleanedDirs;
        {
            std::lock_guard<std::mutex> lock(cleanupMutex);
            if (cleanedDirs.insert(cacheDir).second)
            {
                std::lock_guard<std::mutex> publishLock(gCHeaderDiskCachePublishMutex);
                namespace fs = std::filesystem;
                std::error_code ec;
                const auto now = fs::file_time_type::clock::now();
                constexpr auto grace = std::chrono::minutes(10);
                if (fs::is_directory(cacheDir, ec))
                {
                    for (const auto& file : fs::directory_iterator(cacheDir, ec))
                    {
                        if (ec) break;
                        const auto path = file.path();
                        if (path.extension() != ".json") continue;
                        const auto originalTime = fs::last_write_time(path, ec);
                        if (ec) { ec.clear(); continue; }
                        if (now - originalTime < grace) continue;
                        nlohmann::json json;
                        bool valid = false;
                        {
                            std::ifstream input(path, std::ios::binary);
                            try { if (input.is_open()) { input >> json; valid = true; } }
                            catch (...) { valid = false; }
                        }
                        bool stale = !valid;
                        if (valid)
                        {
                            try
                            {
                                stale = json.value("version", 0) != kCHeaderCacheVersion;
                                if (!stale && json.contains("cxxRequestKey"))
                                {
                                    auto markerPath = path;
                                    markerPath.replace_extension(".rq");
                                    stale = !fs::exists(markerPath, ec);
                                    ec.clear();
                                }
                            }
                            catch (...) { stale = true; }
                        }
                        if (!stale) continue;
                        const auto latestTime = fs::last_write_time(path, ec);
                        if (ec || latestTime != originalTime) { ec.clear(); continue; }
                        nlohmann::json latest;
                        bool stillStale = false;
                        {
                            std::ifstream input(path, std::ios::binary);
                            try
                            {
                                if (input.is_open())
                                {
                                    input >> latest;
                                    stillStale = latest.value("version", 0) != kCHeaderCacheVersion;
                                    if (!stillStale && latest.contains("cxxRequestKey"))
                                    {
                                        auto markerPath = path;
                                        markerPath.replace_extension(".rq");
                                        stillStale = !fs::exists(markerPath, ec);
                                        ec.clear();
                                    }
                                }
                            }
                            catch (...) { stillStale = true; }
                        }
                        if (!valid) stillStale = true;
                        if (stillStale) { fs::remove(path, ec); ec.clear(); }
                    }
                    std::set<std::string> referencedSidecars;
                    for (const auto& file : fs::directory_iterator(cacheDir, ec))
                    {
                        if (ec) break;
                        if (file.path().extension() != ".json") continue;
                        std::ifstream input(file.path(), std::ios::binary);
                        nlohmann::json json;
                        try
                        {
                            if (input.is_open()) input >> json;
                            if (json.contains("cxxbc"))
                                referencedSidecars.insert(json["cxxbc"].value("file", std::string{}));
                        }
                        catch (...) {}
                    }
                    for (const auto& file : fs::directory_iterator(cacheDir, ec))
                    {
                        if (ec) break;
                        if (file.path().extension() == ".bc")
                        {
                            const std::string name = file.path().filename().string();
                            const auto sidecarTime = fs::last_write_time(file.path(), ec);
                            if (ec) { ec.clear(); continue; }
                            if (now - sidecarTime >= grace && !referencedSidecars.contains(name))
                            { fs::remove(file.path(), ec); ec.clear(); }
                        }
                        else if (file.path().extension() == ".rq")
                        {
                            const auto age = now - fs::last_write_time(file.path(), ec);
                            if (ec) { ec.clear(); continue; }
                            if (age < grace) continue;
                            auto jsonPath = file.path(); jsonPath.replace_extension(".json");
                            if (fs::exists(jsonPath, ec)) { ec.clear(); continue; }
                            ec.clear();
                            fs::remove(file.path(), ec); ec.clear();
                        }
                    }
                }
            }
        }
        return cacheDir;
}

uint64_t LLVMBackend::CHeaderDiskCacheKey(const std::string& fileForLsp,
                                        const std::vector<std::string>& includeDirs,
                                        const std::vector<std::string>& defines,
                                        const std::vector<std::string>& extraDefines,
                                        bool msvcBitfieldPacking,
                                        const std::string& targetTriple)
{
        return CHeaderDiskCacheKey(std::vector<std::string>{ fileForLsp },
                                   includeDirs, defines, extraDefines, msvcBitfieldPacking,
                                   targetTriple);
    }

uint64_t LLVMBackend::CHeaderDiskCacheKey(const std::vector<std::string>& headerPaths,
                                        const std::vector<std::string>& includeDirs,
                                        const std::vector<std::string>& defines,
                                        const std::vector<std::string>& extraDefines,
                                        bool msvcBitfieldPacking,
                                        const std::string& targetTriple,
                                        bool cxxMode, bool cxxDefinitionsEmitted,
                                        const std::string& cppStandard,
                                        const std::string& targetCpu,
                                        const std::string& targetFeatures)
{
        uint64_t h = 14695981039346656037ULL;
        auto fold = [&h](const std::string& s) {
            for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
        };
        for (const auto& hp : headerPaths)   { fold("|H"); fold(hp); }
        for (const auto& inc : includeDirs)  { fold("|I"); fold(inc); }
        for (const auto& def : defines)      { fold("|D"); fold(def); }
        for (const auto& def : extraDefines) { fold("|d"); fold(def); }
        // A record's bitfield layout is stored here, and it packs by the target's rule: an entry
        // written for one packing mode is not reusable under the other.
        fold(msvcBitfieldPacking ? "|BFMS" : "|BFIT");
        // Pointer width, long width and the ABI follow the target: win32 and win64 share every
        // other input above, so an entry is only valid for the triple it was extracted for.
        fold("|T"); fold(targetTriple);
        // A C++-mode binding of the same header is a different result; keep the keys apart.
        if (cxxMode) fold("|CXX");
        if (cxxMode) { fold("|STD"); fold(cppStandard); }
        // A declarations-only C++ bind (LSP: no bodies, empty companion module) is a different
        // result again, and never a substitute for a compile's. Nothing writes such an entry to
        // disk today; keying it apart means an older entry can never be mistaken for one either.
        if (cxxMode) fold(cxxDefinitionsEmitted ? "|EDEF" : "|EDECL");
        // C++ request and header-body bitcode uses O1 with LLVM passes disabled.
        if (cxxMode)
        {
            fold("|O1-NO-LLVM-PASSES-V4|CPU=" + targetCpu + "|FEATURES=" + targetFeatures);
        }
        return h;
    }

size_t LLVMBackend::CCachePathTable::Intern(const std::string& path)
{
        auto it = index.find(path);
        if (it != index.end()) return it->second;
        paths.push_back(path);
        index.emplace(path, paths.size() - 1);
        return paths.size() - 1;
    }

// Stores an entry's source path as a "files" index when the document carries a table, and inline
// otherwise. PathFromJson is the mirror and reads either spelling.
void LLVMBackend::PathToJson(nlohmann::json& j, const std::string& path, CCachePathTable* files)
{
        if (path.empty()) return;
        if (files == nullptr) j["f"] = path;
        else                  j["fi"] = (uint64_t)files->Intern(path);
    }

std::string LLVMBackend::PathFromJson(const SjVal& j, const CCachePaths* files)
{
        if (files != nullptr && j.contains("fi"))
        {
            const auto slot = (size_t)j.value("fi", (uint64_t)0);
            if (slot < files->size()) return (*files)[slot];
        }
        return j.value("f", std::string{});
    }

nlohmann::json LLVMBackend::TvToJson(const TypeAndValue& tv)
{
        auto s = SerializedTav::From(tv);
        nlohmann::json j;
        j["t"] = s.TypeName;
        if (!s.VariableName.empty()) j["n"] = s.VariableName;
        if (!s.EnumBacking.empty()) j["eb"] = s.EnumBacking;
        if (s.IsScopedEnum) j["se"] = true;
        if (s.Pointer)        j["p"]   = true;
        if (s.ElemPointer)    j["ep"]  = true;
        if (s.PointerDepth)   j["pd"]  = s.PointerDepth;
        if (s.IsInterface)    j["if"]  = true;
        if (s.IsInterfacePointer) j["ifp"] = true;
        if (s.IsNullable)     j["nl"]  = true;
        if (s.IsMove)         j["mv"]  = true;
        if (s.IsAdopt)        j["ad"]  = true;
        if (s.IsAlias)        j["al"]  = true;
        if (s.IsRvalueRef)    j["rr"]  = true;
        if (s.IsCxxRefToPointer) j["crp"] = true;
        if (s.IsCxxConstRef) j["ccr"] = true;
        if (s.IsOwningSink)   j["osk"] = true;
        if (s.IsConsumeInferredSink) j["cis"] = true;
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
                if (p.IsRvalueRef) pj["rr"] = true;
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
        s.EnumBacking = j.value("eb", std::string{});
        s.IsScopedEnum = j.value("se", false);
        s.Pointer = j.value("p", false);
        s.ElemPointer = j.value("ep", false);
        s.PointerDepth = j.value("pd", 0);
        s.IsInterface = j.value("if", false);
        s.IsInterfacePointer = j.value("ifp", false);
        s.IsNullable = j.value("nl", false);
        s.IsMove = j.value("mv", false);
        s.IsAdopt = j.value("ad", false);
        s.IsAlias = j.value("al", false);
        s.IsRvalueRef = j.value("rr", false);
        s.IsCxxRefToPointer = j.value("crp", false);
        s.IsCxxConstRef = j.value("ccr", false);
        s.IsOwningSink = j.value("osk", false);
        s.IsConsumeInferredSink = j.value("cis", false);
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
                    p.IsRvalueRef = pj.value("rr", false);
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

nlohmann::json LLVMBackend::AbiSlotToJson(const cflat_cinterop::RawAbiSlot& sl)
{
        nlohmann::json j = {{"k", sl.kind}};
        if (!sl.coerceType.empty())  j["ct"] = sl.coerceType;
        if (!sl.paddingType.empty()) j["pt"] = sl.paddingType;
        if (sl.signExt)         j["se"] = true;
        if (sl.zeroExt)         j["ze"] = true;
        if (sl.inReg)           j["ir"] = true;
        if (sl.canBeFlattened)  j["fl"] = true;
        if (sl.indirectByVal)   j["bv"] = true;
        if (sl.indirectRealign) j["rl"] = true;
        if (sl.sretAfterThis)   j["sa"] = true;
        if (sl.indirectAlign)   j["ia"] = sl.indirectAlign;
        if (sl.directOffset)    j["do"] = sl.directOffset;
        if (sl.llvmArgIndex)    j["ai"] = sl.llvmArgIndex;
        if (sl.llvmArgCount != 1) j["ac"] = sl.llvmArgCount;
        return j;
    }

cflat_cinterop::RawAbiSlot LLVMBackend::AbiSlotFromJson(const SjVal& j)
{
        cflat_cinterop::RawAbiSlot sl;
        sl.kind            = j.value("k", (int)cflat_cinterop::RawAbiSlot::Direct);
        sl.coerceType      = j.value("ct", std::string{});
        sl.paddingType     = j.value("pt", std::string{});
        sl.signExt         = j.value("se", false);
        sl.zeroExt         = j.value("ze", false);
        sl.inReg           = j.value("ir", false);
        sl.canBeFlattened  = j.value("fl", false);
        sl.indirectByVal   = j.value("bv", false);
        sl.indirectRealign = j.value("rl", false);
        sl.sretAfterThis   = j.value("sa", false);
        sl.indirectAlign   = j.value("ia", (uint64_t)0);
        sl.directOffset    = j.value("do", (uint64_t)0);
        sl.llvmArgIndex    = (unsigned)j.value("ai", (uint64_t)0);
        sl.llvmArgCount    = (unsigned)j.value("ac", (uint64_t)1);
        return sl;
    }

nlohmann::json LLVMBackend::AbiToJson(const cflat_cinterop::RawAbi& a)
{
        nlohmann::json ps = nlohmann::json::array();
        for (const auto& p : a.params) ps.push_back(AbiSlotToJson(p));
        return {{"r", AbiSlotToJson(a.ret)}, {"ps", ps},
                {"cc", a.callingConv}, {"ft", a.fnTypeText}};
    }

cflat_cinterop::RawAbi LLVMBackend::AbiFromJson(const SjVal& j)
{
        cflat_cinterop::RawAbi a;
        a.valid = true;
        if (j.contains("r")) a.ret = AbiSlotFromJson(j["r"]);
        if (j.contains("ps")) for (const auto& p : j["ps"]) a.params.push_back(AbiSlotFromJson(p));
        a.callingConv = (unsigned)j.value("cc", (uint64_t)0);
        a.fnTypeText  = j.value("ft", std::string{});
        return a;
    }

nlohmann::json LLVMBackend::SigToJson(const CSigEntry& e, CCachePathTable* files)
{
        nlohmann::json ps = nlohmann::json::array();
        TypeAndValue neutral;
        neutral.TypeName = "void";
        if (e.isCxx)
        {
            // C++ foreign mappings depend on the writer's registered types. Keep only the
            // spelling count in the payload; RegisterCSignatures remaps these slots on replay.
            for (size_t i = 0; i < e.paramSpellings.size(); ++i) ps.push_back(TvToJson(neutral));
        }
        else
            for (const auto& p : e.params) ps.push_back(TvToJson(p));
        nlohmann::json j = {{"n", e.name}, {"r", TvToJson(e.isCxx ? neutral : e.ret)}, {"ps", ps}};
        // Everything below is omitted at its SigFromJson default. A header cache holds hundreds of
        // thousands of signatures, and these four fields sit at their default on nearly all of them.
        if (e.variadic) j["va"] = true;
        if (e.line != 1) j["ln"] = e.line;
        if (e.col != 0)  j["co"] = e.col;
        PathToJson(j, e.file, files);
        const std::string& refusal = e.isCxx ? e.sourceBindRefusal : e.bindRefusal;
        if (!refusal.empty()) j["br"] = refusal;
        // C++ identity must round-trip: without it a warm cache calls the demangled name and
        // silently drops the throwing-call gate.
        if (!e.linkageName.empty()) j["lk"] = e.linkageName;
        if (e.isCxx)      j["cx"] = true;
        if (!e.isNoexcept) j["nx"] = true;
        // Clang's arrangement must round-trip: rebuilding it needs the clang session the warm
        // path deliberately skips, and re-deriving it from CFlat's size heuristic would pass a
        // by-value record in the wrong registers.
        if (e.abi.valid) j["abi"] = AbiToJson(e.abi);
        // Raw parameter spellings: RegisterCSignatures retypes a C++ record-pointer parameter out
        // of void* using these, and a warm cache never sees a clang session to re-derive them.
        if (!e.paramSpellings.empty()) j["pspell"] = e.paramSpellings;
        // paramNames repeats the names already serialized into "ps" for almost every C signature.
        // Compare against the SERIALIZED slot, so "pnq" means exactly what the reader rebuilds.
        bool namesMatchSlots = !e.paramNames.empty() && e.paramNames.size() == ps.size();
        for (size_t i = 0; namesMatchSlots && i < ps.size(); ++i)
            namesMatchSlots = (ps[i].value("n", std::string{}) == e.paramNames[i]);
        if (namesMatchSlots)                j["pnq"]    = true;
        else if (!e.paramNames.empty())     j["pnames"] = e.paramNames;
        if (!e.retSpelling.empty()) j["rspell"] = e.retSpelling;
        if (!e.defaultArgs.empty())
        {
            // The vector is positional and its SIZE is load-bearing (callers gate on
            // defaultArgs.size() == params.size()), so an all-empty one keeps its length as "nd"
            // rather than being dropped.
            bool anyDefault = false;
            for (const auto& d : e.defaultArgs)
                if (!d.kind.empty() || !d.value.empty()) { anyDefault = true; break; }
            if (!anyDefault) j["nd"] = (uint64_t)e.defaultArgs.size();
            else
            {
                nlohmann::json da = nlohmann::json::array();
                for (const auto& d : e.defaultArgs)
                {
                    nlohmann::json one = nlohmann::json::object();
                    if (!d.kind.empty())  one["k"] = d.kind;
                    if (!d.value.empty()) one["v"] = d.value;
                    da.push_back(std::move(one));
                }
                j["defaults"] = da;
            }
        }
        return j;
    }

LLVMBackend::CSigEntry LLVMBackend::SigFromJson(const SjVal& j, const CCachePaths* files)
{
        CSigEntry e;
        e.name     = j.value("n",  std::string{});
        e.ret      = TvFromJson(j.at("r"));
        e.variadic = j.value("va", false);
        e.linkageName = j.value("lk", std::string{});
        e.isCxx    = j.value("cx", false);
        e.isNoexcept = !j.value("nx", false);
        e.file     = PathFromJson(j, files);
        e.bindRefusal = j.value("br", std::string{});
        e.sourceBindRefusal = e.bindRefusal;
        e.needsCxxRebind = e.isCxx;
        e.line     = j.value("ln", 1);
        e.col      = j.value("co", 0);
        if (j.contains("ps")) for (const auto& p : j["ps"]) e.params.push_back(TvFromJson(p));
        if (j.contains("abi")) e.abi = AbiFromJson(j["abi"]);
        if (j.contains("pspell")) e.paramSpellings = j["pspell"].to_string_vector();
        if (j.value("pnq", false))
        {
            if (j.contains("ps"))
                for (const auto& p : j["ps"]) e.paramNames.push_back(p.value("n", std::string{}));
        }
        else if (j.contains("pnames")) e.paramNames = j["pnames"].to_string_vector();
        e.retSpelling = j.value("rspell", std::string{});
        if (j.contains("defaults"))
            for (const auto& d : j["defaults"])
                e.defaultArgs.push_back({ d.value("k", std::string{}), d.value("v", std::string{}) });
        else
            e.defaultArgs.resize((size_t)j.value("nd", (uint64_t)0));
        return e;
    }

nlohmann::json LLVMBackend::FunctionTemplateToJson(
        const cflat_cinterop::RawFunctionTemplate& t, CCachePathTable* files)
{
        nlohmann::json j = { {"n", t.name}, {"o", t.owner}, {"m", t.memberName},
                 {"c", t.cxxSpelling}, {"k", t.kind}, {"mi", t.minArity},
                 {"ma", t.maxArity}, {"tp", t.typeParameterCount}, {"pp", t.hasParameterPack},
                 {"pt", t.parameterTypes},
                 {"pn", t.parameterNames},
                 {"fr", t.forwardingReferenceParameters},
                 {"fpi", t.forwardingReferenceTemplateParameterIndices},
                 {"tk", t.templateParameterKinds},
                 {"cn", t.isConst},
                 {"nx", t.isNoexcept}, {"a", t.access},
                 {"ln", t.line}, {"co", t.col} };
        PathToJson(j, t.file, files);
        return j;
    }

cflat_cinterop::RawFunctionTemplate LLVMBackend::FunctionTemplateFromJson(
        const SjVal& j, const CCachePaths* files)
{
        cflat_cinterop::RawFunctionTemplate t;
        t.name = j.value("n", std::string{});
        t.owner = j.value("o", std::string{});
        t.memberName = j.value("m", std::string{});
        t.cxxSpelling = j.value("c", std::string{});
        t.kind = j.value("k", 0);
        t.minArity = (unsigned)j.value("mi", (uint64_t)0);
        t.maxArity = (unsigned)j.value("ma", (uint64_t)0);
        t.typeParameterCount = (unsigned)j.value("tp", (uint64_t)0);
        t.hasParameterPack = j.value("pp", false);
        if (j.contains("pt")) t.parameterTypes = j["pt"].to_string_vector();
        if (j.contains("pn")) t.parameterNames = j["pn"].to_string_vector();
        if (j.contains("fr"))
            for (uint64_t value : j["fr"].to_u64_vector())
                t.forwardingReferenceParameters.push_back(value != 0 ? 1 : 0);
        if (j.contains("fpi"))
            for (uint64_t value : j["fpi"].to_u64_vector())
                t.forwardingReferenceTemplateParameterIndices.push_back((unsigned)value);
        t.templateParameterKinds = j.value("tk", std::string{});
        t.isConst = j.value("cn", false);
        t.isNoexcept = j.value("nx", false);
        t.access = j.value("a", 0);
        t.file = PathFromJson(j, files);
        t.line = j.value("ln", 1);
        t.col = j.value("co", 0);
        return t;
    }

nlohmann::json LLVMBackend::EnumToJson(const CEnumEntry& e)
{
        nlohmann::json j = {{"n", e.name}, {"v", e.value}, {"ln", e.line}, {"co", e.col}};
        if (!e.enumType.empty()) j["et"] = e.enumType;
        if (!e.underlyingType.empty()) j["ut"] = e.underlyingType;
        if (e.isScoped) j["sc"] = true;
        return j;
    }

LLVMBackend::CEnumEntry LLVMBackend::EnumFromJson(const SjVal& j)
{
        CEnumEntry e;
        e.name = j.value("n", std::string{});
        e.value = j.value("v", 0LL);
        e.line = j.value("ln", 1);
        e.col = j.value("co", 0);
        e.enumType = j.value("et", std::string{});
        e.underlyingType = j.value("ut", std::string{});
        e.isScoped = j.value("sc", false);
        return e;
    }

nlohmann::json LLVMBackend::GlobalToJson(const CGlobalEntry& g)
{
        nlohmann::json j = {{"n", g.name}, {"t", TvToJson(g.type)},
                            {"ln", g.line}, {"co", g.col}};
        if (!g.qualifiedName.empty()) j["qn"] = g.qualifiedName;
        if (!g.linkageName.empty()) j["lk"] = g.linkageName;
        if (g.isConst) j["cq"] = true;
        if (g.isCompileTimeConstant)
        {
            j["cn"] = true;
            if (g.isFloatConstant)
            {
                uint64_t fvbits = 0;
                std::memcpy(&fvbits, &g.floatValue, sizeof(double));
                j["fc"] = true;
                j["fvb"] = fvbits;
            }
            else
                j["cv"] = g.constantValue;
        }
        if (g.isCxxConstexpr) j["cxce"] = true;
        return j;
}

LLVMBackend::CGlobalEntry LLVMBackend::GlobalFromJson(const SjVal& j)
{
        CGlobalEntry g;
        g.name = j.value("n", std::string{});
        g.qualifiedName = j.value("qn", std::string{});
        g.linkageName = j.value("lk", std::string{});
        g.isConst = j.value("cq", false);
        g.type = TvFromJson(j.at("t"));
        g.isCompileTimeConstant = j.value("cn", false);
        g.isFloatConstant = j.value("fc", false);
        if (g.isFloatConstant)
        {
            uint64_t fvbits = j.value("fvb", uint64_t{0});
            std::memcpy(&g.floatValue, &fvbits, sizeof(double));
        }
        g.constantValue = j.value("cv", (int64_t)0);
        g.isCxxConstexpr = j.value("cxce", false);
        g.line = j.value("ln", 1);
        g.col  = j.value("co", 0);
        return g;
    }

nlohmann::json LLVMBackend::FieldToJson(const CRecordFieldEntry& f)
{
        nlohmann::json j = {{"n", f.name}, {"ct", f.ctype}};
        if (f.isBitfield) { j["bf"] = true; j["bw"] = f.bitWidth; }
        if (f.offsetBytes != 0) j["ob"] = f.offsetBytes;
        if (f.sizeBytes != 0) j["sz"] = f.sizeBytes;
        if (f.alignBytes != 0) j["al"] = f.alignBytes;
        if (f.bitOffset != 0) j["bo"] = f.bitOffset;
        if (f.access != 0) j["ac"] = f.access;
        return j;
    }

LLVMBackend::CRecordFieldEntry LLVMBackend::FieldFromJson(const SjVal& j)
{
        CRecordFieldEntry f;
        f.name      = j.value("n",  std::string{});
        f.ctype     = j.value("ct", std::string{});
        f.isBitfield = j.value("bf", false);
        f.bitWidth   = j.value("bw", 0u);
        f.offsetBytes = j.value("ob", (uint64_t)0);
        f.sizeBytes = j.value("sz", (uint64_t)0);
        f.alignBytes = j.value("al", (uint64_t)0);
        f.bitOffset = j.value("bo", (uint64_t)0);
        f.access    = j.value("ac", 0);
        return f;
    }

/*
 * A C++ class member's whole exported description, including clang's ABI arrangement. Every
 * field here is read by an analysis (access control, triviality, deleted/defaulted status, the
 * structor tables), so all of it must round-trip or a warm cache silently loses the class.
 */
static constexpr size_t kCxxRefusalCauseMaxBytes = 4096;

static std::string CxxRefusalCauseForCache(const std::string& cause)
{
        if (cause.size() <= kCxxRefusalCauseMaxBytes) return cause;
        const size_t newline = cause.rfind('\n', kCxxRefusalCauseMaxBytes - 1);
        return newline == std::string::npos ? std::string{} : cause.substr(0, newline);
}

nlohmann::json LLVMBackend::CxxMemberToJson(
        const cflat_cinterop::RawCxxMember& m, CCachePathTable* files)
{
        nlohmann::json j = {{"k", m.kind}, {"n", m.name}, {"rt", m.retType},
                            {"pt", m.paramTypes}, {"pn", m.paramNames},
                            {"ln", m.line}, {"co", m.col}};
        if (!m.linkageName.empty()) j["lk"] = m.linkageName;
        PathToJson(j, m.file, files);
        if (m.variadic)             j["va"] = true;
        if (m.requiresConstructorWrapper) j["cw"] = true;
        if (m.isConst)              j["cn"] = true;
        if (m.refQualifier != cflat_cinterop::CxxRefQualifierNone) j["rq"] = m.refQualifier;
        if (m.isVirtual)            j["vi"] = true;
        if (m.isNoexcept)           j["nx"] = true;
        if (m.isDeleted)            j["dl"] = true;
        if (m.isDefaulted)          j["df"] = true;
        if (m.isImplicit)           j["im"] = true;
        if (m.isTemplateSpecialization) j["ts"] = true;
        if (m.needsLocalDefinition) j["nd"] = true;
        if (!m.bindRefusal.empty()) j["br"] = m.bindRefusal;
        if (!m.refusalCause.empty())
        {
            const std::string cause = CxxRefusalCauseForCache(m.refusalCause);
            if (!cause.empty()) j["rcs"] = cause;
        }
        if (m.returnsThis)          j["rth"] = true;
        if (m.isCopyCtor)           j["cc"] = true;
        if (m.isMoveCtor)           j["mc"] = true;
        if (m.isDefaultCtor)        j["dc"] = true;
        if (m.isCopyAssign)         j["ca"] = true;
        if (m.isMoveAssign)         j["ma"] = true;
        if (m.isPureVirtual)        j["pv"] = true;
        if (m.isOverride)           j["ov"] = true;
        if (m.isFinal)              j["fi"] = true;
        if (m.isConversion)         j["cvn"] = true;
        if (m.isExplicit)           j["xpl"] = true;
        if (m.covariantReturnNeedsAdjust) j["cra"] = true;
        // M6 - the vtable slots. A warm cache that dropped these would re-register a virtual
        // member as a DIRECT call, which silently skips every override.
        if (m.vtableIndex >= 0)         j["vti"] = m.vtableIndex;
        if (m.vtableIndexDeleting >= 0) j["vtd"] = m.vtableIndexDeleting;
        if (m.access != 0)          j["ac"] = m.access;
        if (m.abi.valid)            j["abi"] = AbiToJson(m.abi);
        if (!m.defaultArgs.empty())
        {
            nlohmann::json da = nlohmann::json::array();
            for (const auto& d : m.defaultArgs) da.push_back({{"k", d.kind}, {"v", d.value}});
            j["defaults"] = da;
        }
        return j;
    }

cflat_cinterop::RawCxxMember LLVMBackend::CxxMemberFromJson(
        const SjVal& j, const CCachePaths* files)
{
        cflat_cinterop::RawCxxMember m;
        m.kind        = j.value("k", 0);
        m.name        = j.value("n", std::string{});
        m.retType     = j.value("rt", std::string{});
        m.linkageName = j.value("lk", std::string{});
        m.file        = PathFromJson(j, files);
        m.line        = j.value("ln", 1);
        m.col         = j.value("co", 0);
        if (j.contains("pt")) m.paramTypes = j["pt"].to_string_vector();
        if (j.contains("pn")) m.paramNames = j["pn"].to_string_vector();
        m.variadic             = j.value("va", false);
        m.requiresConstructorWrapper = j.value("cw", false);
        m.isConst              = j.value("cn", false);
        m.refQualifier         = j.value("rq", (int)cflat_cinterop::CxxRefQualifierNone);
        m.isVirtual            = j.value("vi", false);
        m.isNoexcept           = j.value("nx", false);
        m.isDeleted            = j.value("dl", false);
        m.isDefaulted          = j.value("df", false);
        m.isImplicit           = j.value("im", false);
        m.isTemplateSpecialization = j.value("ts", false);
        m.needsLocalDefinition = j.value("nd", false);
        m.bindRefusal = j.value("br", std::string());
        m.refusalCause = j.value("rcs", std::string());
        m.returnsThis          = j.value("rth", false);
        m.isCopyCtor           = j.value("cc", false);
        m.isMoveCtor           = j.value("mc", false);
        m.isDefaultCtor        = j.value("dc", false);
        m.isCopyAssign         = j.value("ca", false);
        m.isMoveAssign         = j.value("ma", false);
        m.isPureVirtual        = j.value("pv", false);
        m.isOverride           = j.value("ov", false);
        m.isFinal              = j.value("fi", false);
        m.isConversion         = j.value("cvn", false);
        m.isExplicit           = j.value("xpl", false);
        m.covariantReturnNeedsAdjust = j.value("cra", false);
        m.vtableIndex          = j.value("vti", -1);
        m.vtableIndexDeleting  = j.value("vtd", -1);
        m.access               = j.value("ac", 0);
        if (j.contains("abi")) m.abi = AbiFromJson(j["abi"]);
        if (j.contains("defaults"))
            for (const auto& d : j["defaults"])
                m.defaultArgs.push_back({ d.value("k", std::string{}), d.value("v", std::string{}) });
        return m;
    }

nlohmann::json LLVMBackend::CxxStaticVarToJson(
        const cflat_cinterop::RawCxxStaticVar& v, CCachePathTable* files)
{
        nlohmann::json j = {{"n", v.name}, {"ct", v.ctype}, {"lk", v.linkageName},
                            {"ln", v.line}, {"co", v.col}};
        if (v.isCompileTimeConstant)
        {
            j["cn"] = true;
            if (v.isFloatConstant)
            {
                uint64_t fvbits = 0;
                std::memcpy(&fvbits, &v.floatValue, sizeof(double));
                j["fc"] = true;
                j["fvb"] = fvbits;
            }
            else
                j["cv"] = v.constantValue;
        }
        PathToJson(j, v.file, files);
        if (v.access != 0)   j["ac"] = v.access;
        return j;
    }

cflat_cinterop::RawCxxStaticVar LLVMBackend::CxxStaticVarFromJson(
        const SjVal& j, const CCachePaths* files)
{
        cflat_cinterop::RawCxxStaticVar v;
        v.name        = j.value("n", std::string{});
        v.ctype       = j.value("ct", std::string{});
        v.linkageName = j.value("lk", std::string{});
        v.isCompileTimeConstant = j.value("cn", false);
        v.isFloatConstant = j.value("fc", false);
        if (v.isFloatConstant)
        {
            uint64_t fvbits = j.value("fvb", uint64_t{0});
            std::memcpy(&v.floatValue, &fvbits, sizeof(double));
        }
        v.constantValue = j.value("cv", (int64_t)0);
        v.file        = PathFromJson(j, files);
        v.line        = j.value("ln", 1);
        v.col         = j.value("co", 0);
        v.access      = j.value("ac", 0);
        return v;
    }

nlohmann::json LLVMBackend::RecordToJson(const CRecordEntry& r, CCachePathTable* files)
{
        nlohmann::json fs = nlohmann::json::array();
        for (const auto& f : r.fields) fs.push_back(FieldToJson(f));
        nlohmann::json j = {{"n", r.name}, {"fs", fs}, {"ln", r.line}, {"co", r.col}};
        if (!r.qualifiedName.empty()) j["qn"] = r.qualifiedName;
        PathToJson(j, r.file, files);
        if (!r.inScope) j["sc"] = false;
        if (r.isUnion) j["u"] = true;
        if (!r.uuid.empty()) j["id"] = r.uuid;
        // C++ layout facts drive CreateStructType's alignment/packing; dropping them on a warm
        // cache would silently re-lay-out the record.
        if (r.isCxx)    j["cx"] = true;
        if (r.isPacked) j["pk"] = true;
        if (r.isTrivial) j["tv"] = true;
        if (r.isTriviallyCopyable) j["tc"] = true;
        if (r.sizeBytes != 0)  j["sz"] = r.sizeBytes;
        if (r.alignBytes != 0) j["al"] = r.alignBytes;
        // M4 class surface. Same rule as the ABI arrangement above: the warm path never rebuilds
        // a clang session, so a dropped triviality bit or member list silently unbinds the class.
        if (r.isPolymorphic)         j["po"] = true;
        if (r.hasBases)              j["hb"] = true;
        if (r.hasTrivialDefaultCtor) j["tdc"] = true;
        if (r.hasTrivialCopyCtor)    j["tcc"] = true;
        if (!r.hasTrivialDtor)       j["ntd"] = true;
        if (r.paramDestroyedInCallee) j["pdc"] = true;
        if (r.hasDeletedDefaultCtor) j["ddc"] = true;
        if (r.hasDeletedCopyCtor)    j["dcc"] = true;
        if (r.hasDefaultCtor)        j["hdc"] = true;
        if (r.hasCopyCtor)           j["hcc"] = true;
        if (r.hasCtorTemplate)       j["hct"] = true;
        if (r.isAggregate)           j["ag"] = true;
        // M6 - inheritance surface. Base offsets drive every derived-to-base adjustment and the
        // abstract/virtual-base gates; a warm cache that lost them would emit unadjusted pointers.
        if (r.hasVirtualBases)       j["hvb"] = true;
        if (r.isAbstract)            j["abs"] = true;
        if (!r.layoutRefusal.empty()) j["lref"] = r.layoutRefusal;
        // The canonical C++ spelling: a template argument naming this class needs it, so a warm
        // cache that dropped it would refuse `cppt.Box<cppi.Tracked>` the second time around.
        if (!r.canonicalCtype.empty()) j["can"] = r.canonicalCtype;
        if (!r.bases.empty())
        {
            nlohmann::json bs = nlohmann::json::array();
            for (const auto& b : r.bases)
            {
                nlohmann::json bj = {{"n", b.name}, {"of", b.offsetBytes}};
                if (!b.canonicalType.empty()) bj["ct"] = b.canonicalType;
                if (b.access != 0) bj["ac"] = b.access;
                if (b.isVirtual)   bj["vi"] = true;
                bs.push_back(std::move(bj));
            }
            j["bs"] = bs;
        }
        if (!r.virtualBases.empty())
        {
            nlohmann::json bs = nlohmann::json::array();
            for (const auto& b : r.virtualBases)
            {
                nlohmann::json bj = {{"n", b.name}, {"of", b.offsetBytes}};
                if (!b.canonicalType.empty()) bj["ct"] = b.canonicalType;
                if (b.access != 0) bj["ac"] = b.access;
                bs.push_back(std::move(bj));
            }
            j["vbs"] = bs;
        }
        if (!r.members.empty())
        {
            nlohmann::json ms = nlohmann::json::array();
            for (const auto& m : r.members) ms.push_back(CxxMemberToJson(m, files));
            j["mb"] = ms;
        }
        if (!r.staticVars.empty())
        {
            nlohmann::json vs = nlohmann::json::array();
            for (const auto& v : r.staticVars) vs.push_back(CxxStaticVarToJson(v, files));
            j["sv"] = vs;
        }
        return j;
    }

LLVMBackend::CRecordEntry LLVMBackend::RecordFromJson(const SjVal& j, const CCachePaths* files)
{
        CRecordEntry r;
        r.name    = j.value("n", std::string{});
        r.isUnion = j.value("u", false);
        r.line    = j.value("ln", 1);
        r.col     = j.value("co", 0);
        r.qualifiedName = j.value("qn", std::string{});
        r.file   = PathFromJson(j, files);
        r.inScope = j.value("sc", true);
        r.uuid    = j.value("id", std::string{});
        r.isCxx    = j.value("cx", false);
        r.isPacked = j.value("pk", false);
        r.isTrivial = j.value("tv", false);
        r.isTriviallyCopyable = j.value("tc", false);
        r.sizeBytes  = j.value("sz", (uint64_t)0);
        r.alignBytes = j.value("al", (uint64_t)0);
        r.isPolymorphic         = j.value("po", false);
        r.hasBases              = j.value("hb", false);
        r.hasTrivialDefaultCtor = j.value("tdc", false);
        r.hasTrivialCopyCtor    = j.value("tcc", false);
        r.hasTrivialDtor        = !j.value("ntd", false);
        r.paramDestroyedInCallee = j.value("pdc", false);
        r.hasDeletedDefaultCtor = j.value("ddc", false);
        r.hasDeletedCopyCtor    = j.value("dcc", false);
        r.hasDefaultCtor        = j.value("hdc", false);
        r.hasCopyCtor           = j.value("hcc", false);
        r.hasCtorTemplate       = j.value("hct", false);
        r.isAggregate           = j.value("ag", false);
        r.hasVirtualBases       = j.value("hvb", false);
        r.isAbstract            = j.value("abs", false);
        r.layoutRefusal         = j.value("lref", std::string{});
        r.canonicalCtype        = j.value("can", std::string{});
        if (j.contains("bs"))
            for (const auto& b : j["bs"])
            {
                cflat_cinterop::RawCxxBase rb;
                rb.name        = b.value("n", std::string{});
                rb.canonicalType = b.value("ct", std::string{});
                rb.offsetBytes = b.value("of", (uint64_t)0);
                rb.access      = b.value("ac", 0);
                rb.isVirtual   = b.value("vi", false);
                r.bases.push_back(std::move(rb));
            }
        if (j.contains("vbs"))
            for (const auto& b : j["vbs"])
            {
                cflat_cinterop::RawCxxBase rb;
                rb.name          = b.value("n", std::string{});
                rb.canonicalType = b.value("ct", std::string{});
                rb.offsetBytes   = b.value("of", (uint64_t)0);
                rb.access        = b.value("ac", 0);
                rb.isVirtual     = true;
                r.virtualBases.push_back(std::move(rb));
            }
        if (j.contains("mb")) for (const auto& m : j["mb"]) r.members.push_back(CxxMemberFromJson(m, files));
        if (j.contains("sv")) for (const auto& v : j["sv"]) r.staticVars.push_back(CxxStaticVarFromJson(v, files));
        if (j.contains("fs")) for (const auto& f : j["fs"]) r.fields.push_back(FieldFromJson(f));
        return r;
    }

nlohmann::json LLVMBackend::MacroToJson(const CMacroEntry& m, CCachePathTable* files)
{
        nlohmann::json j = {{"n", m.name}, {"v", m.value},
                            {"ln", m.line}, {"co", m.col}};
        PathToJson(j, m.file, files);
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

LLVMBackend::CMacroEntry LLVMBackend::MacroFromJson(const SjVal& j, const CCachePaths* files)
{
        CMacroEntry m;
        m.name      = j.value("n",   std::string{});
        m.value     = j.value("v",   0LL);
        m.file      = PathFromJson(j, files);
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

nlohmann::json LLVMBackend::FuncMacroToJson(const CFunctionMacroEntry& m, CCachePathTable* files)
{
        nlohmann::json j = {{"n", m.name}, {"ps", m.params}, {"b", m.body},
                            {"ln", m.line}, {"co", m.col}};
        PathToJson(j, m.file, files);
        return j;
    }

LLVMBackend::CFunctionMacroEntry LLVMBackend::FuncMacroFromJson(const SjVal& j, const CCachePaths* files)
{
        CFunctionMacroEntry m;
        m.name = j.value("n",  std::string{});
        m.body = j.value("b",  std::string{});
        m.file = PathFromJson(j, files);
        m.line = j.value("ln", 1);
        m.col  = j.value("co", 0);
        if (j.contains("ps")) m.params = j["ps"].to_string_vector();
        return m;
    }

nlohmann::json LLVMBackend::TypeAliasToJson(const CTypeAliasEntry& a, CCachePathTable* files)
{
        nlohmann::json j = {{"n", a.name}, {"t", a.target},
                {"ln", a.line}, {"co", a.col}, {"ar", a.isAnonymousRecord},
                {"qn", a.qualifiedName}, {"cs", a.cxxSpecialization},
                {"iat", a.isCxxAliasTemplate}, {"cap", a.cxxAliasPattern},
                {"can", a.cxxAliasParams}, {"catb", a.cxxAliasTargetBase},
                {"caa", a.cxxAliasArgs}, {"cad", a.cxxAliasParamDefaults}};
        PathToJson(j, a.file, files);
        return j;
    }

LLVMBackend::CTypeAliasEntry LLVMBackend::TypeAliasFromJson(const SjVal& j, const CCachePaths* files)
{
        CTypeAliasEntry a;
        a.name = j.value("n", std::string{});
        a.target = j.value("t", std::string{});
        a.file = PathFromJson(j, files);
        a.line = j.value("ln", 1);
        a.col = j.value("co", 0);
        a.isAnonymousRecord = j.value("ar", false);
        a.qualifiedName = j.value("qn", std::string{});
        a.cxxSpecialization = j.value("cs", std::string{});
        a.isCxxAliasTemplate = j.value("iat", false);
        a.cxxAliasPattern = j.value("cap", std::string{});
        if (j.contains("can")) a.cxxAliasParams = j["can"].to_string_vector();
        a.cxxAliasTargetBase = j.value("catb", std::string{});
        if (j.contains("caa")) a.cxxAliasArgs = j["caa"].to_string_vector();
        if (j.contains("cad")) a.cxxAliasParamDefaults = j["cad"].to_string_vector();
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
        CFileSigCacheEntry& out,
        const std::string& expectedRequestKey,
        bool requireBitcode,
        std::string* missReason,
        bool removeOnMiss)
{
        namespace fs = std::filesystem;
        std::error_code ec;
        auto cachePath = cacheDir / std::format("{:016x}.json", diskKey);
        auto cacheMiss = [&](const char* reason) {
            if (missReason != nullptr) *missReason = reason;
            // Cache files are shared by concurrent compilers; a miss must not unlink a writer's entry.
            (void)removeOnMiss;
            return false;
        };
        if (!fs::exists(cachePath, ec)) return cacheMiss("missing entry");

        // parser + jsonBuf own the storage that doc/SjVal reference; keep them alive for the
        // whole function. Reads run through SjVal (simdjson DOM); writes still use nlohmann.
        simdjson::dom::parser parser;
        simdjson::padded_string jsonBuf;
        simdjson::dom::element doc;
        SjVal j;
        {
            llvm::TimeTraceScope parseScope(
                expectedRequestKey.empty() ? "CHeaderJsonParse" : "CxxTypeRequestJsonParse",
                cachePath.string());
            auto loaded = simdjson::padded_string::load(cachePath.string());
            if (loaded.error()) return cacheMiss("unreadable entry");
            jsonBuf = std::move(loaded.value());
            if (parser.parse(jsonBuf).get(doc) != simdjson::SUCCESS)
                return cacheMiss("malformed entry");
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
        // v15 carries clang's own ABI arrangement for each C++ declaration and the record's
        // trivially-copyable flag; a v14 entry has neither, so a warm cache would fall back to
        // the C size heuristic for a by-value C++ record and pass it in the wrong registers.
        // v16 carries the M4 class surface: per-record triviality/polymorphism bits, member
        // access, and the constructor/destructor/method tables with their ABI arrangements. A v15
        // entry has none of it, so a warm cache would leave every imported class methodless.
        // v18 carries the M5 companion module: the LLVM bitcode of the C++ definitions Clang
        // emitted for the group (inline bodies, vtables/RTTI, inline static members). A v17 entry
        // has none, so a warm cache would bind inline members whose symbols were never emitted.
        // v19 carries ABI plans for C++ function-pointer pointee types. v25 carries constexpr
        // C++ static data members as folded values, so declaration-only imports need no symbol.
        // v27 adds the C++ operator++/--, operator*, operator-> and operator bool members to
        // extraction; older entries must be reparsed because their member lists are incomplete.
        // v28 carries the long-double width, format, and target triple from the clang invocation.
        // v34 records a failed generated default wrapper as an unsupported default argument, so
        // a warm cache cannot re-declare a wrapper whose definition CodeGen dropped.
        // v35 stops recording an unnamed enum's placeholder spelling as an enumerator type.
        // v36 carries published C++ function-template declarations for deduction wrappers.
        // v37 invalidates cached C++ member lists after plain-header special-member emission.
        // v38 carries member default arguments and opaque field size/alignment metadata.
        // v39 includes non-member C++ overloaded operators from the header walk.
        // v40 adds the member operators <=>, && and || to the bindable set, so a v39 member
        // list is missing them (and with them the rewritten relational operators).
        // v41 carries constructor-wrapper metadata for inherited and parameter-pack constructors.
        // v42: C++ imports map `long` / `unsigned long` / `char8_t` / `char16_t` / `char32_t` /
        // `wchar_t` by identity to `long` / `ulong` / `c8` / `c16` / `c32` / `wchar`.
        // v43: the companion module no longer emits the vtable of a class whose specialization is
        // an explicit instantiation DECLARATION (`extern template class basic_ios<char>;`) - that
        // vtable is a strong symbol owned by the library, and a v42 entry carries a duplicate
        // definition of it.
        // v45: function templates with trailing parameter packs (unbounded arity) and
        // namespace-scope using-declaration alias signatures.
        // v46: dependent constructor patterns are retained in the member list.
        // v47: C++ namespace-scope using-directives are retained for lookup-time re-export.
        // v48: function templates carry templateParameterKinds (non-type template arguments).
        // v49: namespace-scope C++ constexpr globals retain qualified names and folded values.
        // v50: C++ parameter names survive the header cache for brace-list diagnostics.
        // v51: C++ companion bitcode moves from inline base64 to a validated raw sidecar.
        // v52: C++ constexpr namespace/static floating values are cached as IEEE-754 bits.
        // v53: invalidated entries and no-bitcode rewrites remove stale companion sidecars.
        // v54: extractor and backend share one C++ foreign identity spelling.
        // v55: all canonical multi-word C++ primitive spellings share their CFlat identity.
        // v56: C++ members retain explicit override/final attributes for CFlat-derived classes.
        // v57: C++ function-template cache entries retain parameter types so same-arity overloads
        // of one member template are not collapsed into the first declaration.
        // v58: C++ type-request entries carry their full source/driver identity and are persisted
        // separately from the registration state. v59 preserves the full probe record needed to
        // rebuild an identical stage-2 source from a warm stage-1 hit. v60 preserves enum backing
        // types in cached signatures, so unsigned narrow enum returns keep their signedness.
        // v62 stores a bound global's mangled linkage name and const-ness, so a namespace-scope
        // C++ object survives a warm cache.
        // v63 carries C++ enum scopedness for conversion ranking.
        // v64 binds the constructor of a class with virtual bases to its placement-new thunk,
        // changing that member's cached linkage name and ABI.
        // v65 carries C++ member ref-qualifiers, so a warm cache can distinguish lvalue- and
        // rvalue-qualified overloads.
        // v66 folds a non-constexpr `static const` member initialized in class, so such a member
        // is now present with a constant value where an older cache omitted it entirely.
        // v67 binds a C++ REFERENCE data member as a pointer field: an older cache carries the
        // "reference members are not supported" layout refusal and an empty field list for it.
        // v68 stores the class-template specializations a record holds by value as records of
        // their own, so a field of such a type is laid out instead of embedded as opaque bytes.
        // v69 makes cached C++ signature payloads key-pure by remapping foreign types on replay.
        // v70 maps a std::function return to its std.function specialization: an older cache
        // carries the "return type ... is unsupported" refusal for such a signature.
        // v71 harvests C++ namespace aliases and refuses a 'consteval' function: an older cache
        // has no alias pairs and carries a bindable signature for an immediate function.
        // v72 records `const` on a C++ reference parameter (IsCxxConstRef): an older cache
        // carries the flag as false, so an rvalue would not bind a `const T&` scalar parameter.
        // v74 publishes FREE BINARY OPERATOR templates: an older cache has none, so a binary
        // operator over a class template (every libc++ basic_string operator) finds no candidate.
        // v76 widens `xpl` to a CONVERSION operator as well as a constructor: a v75 cache
        // carries it as false for a conversion, so an explicit one would convert implicitly.
        // v77 shrinks the payload: source paths are interned into a per-document "files" table
        // ("fi"), a signature omits every field sitting at its default, and a C++ type request
        // stores most of its signatures as indices into a shared baseline ("sb"). A v76 entry
        // spells all of it out and has no baseline to resolve against.
        // v78 binds a reference to a T** (crp at two pointer levels): an older cache carries the
        // member's refusal instead of a signature.
        // v79 preserves the pointer level when a const-qualified pointer reference collapses
        // to the CFlat alias surface: an older cache carries the raw shape.
        // v80 records function-template forwarding-reference parameters: an older cache
        // carries none, so an lvalue would not bind a template U&& parameter.
        // v81 records a public constructor TEMPLATE on a C++ record (hct): an older cache
        // carries none, so `T(args)` would never reach clang's constructor overload resolution.
        // v83 keys C++ header and type-request entries by the selected language standard.
        // v82 registers explicit free-function-template wrappers under their unique names.
        // v84 companion bitcode defines the virtual members of a vtable it emits and the
        // out-of-line inline members it uses: an older sidecar declares them (link failure).
        // v84 also records every virtual base's complete-object offset ("vbs"): an older record
        // has none, so a member inherited through a virtual base gets the wrong `this`.
        // v85 omits out-of-line C++ static data members from the bare-global list; they are
        // registered through their class record instead.
        // v86 exports class-scope operator new / delete / new[] / delete[] members: an older
        // record has none, so `new T` of such a class would call the global allocator.
        // v87 recognizes forwarding references in C++ function parameter packs.
        // v88 publishes global class and alias-template names for bare CFlat type requests.
        // v89 stores function-template parameter names and forwarding template-parameter indices.
        // v90 stores companion bitcode under a content-addressed name and validates the bytes
        // read from that file, so concurrent publishers cannot pair another writer's module.
        // v91 stores long double signatures with their distinct longdouble identity.
        // v92 refuses bodies reaching an incremental chunk's emptied (poisoned) specialization
        // and stores the clang diagnostic behind a member refusal (refusalCause).
        // v93 exports non-special user operator= overloads and member templates for assignment calls.
        // v94 caps each serialized C++ member refusal cause at 4 KB on a line boundary.
        // v95 refuses a member whose signature contains clang error nodes instead of mangling it.
        if (version != kCHeaderCacheVersion) return cacheMiss("cache version");

        if (!expectedRequestKey.empty()
            && j.value("cxxRequestKey", std::string{}) != expectedRequestKey)
            return cacheMiss("request key");

        // Accept on mtime match (fast) or content hash match (authoritative on mtime drift).
        auto storedMtime = j.value("mtime", int64_t{-1});
        auto storedHash  = j.value("hash",  uint64_t{0});
        bool mtimeOk = (storedMtime == (int64_t)mtime.time_since_epoch().count());
        bool hashOk  = (storedHash  == contentHash);
        if (!mtimeOk && !hashOk) return cacheMiss("header stamp");

        // Any malformed/incompatible field must degrade to a cache miss (reparse), never abort
        // the compiler: the nlohmann accessors throw on a type mismatch, so guard the whole build.
        CFileSigCacheEntry entry;
        entry.mtime = mtime;
        entry.hash  = contentHash;
        entry.longDoubleWidth = j.value("ldw", (uint64_t)0);
        entry.longDoubleIsIEEEDouble = j.value("ldieee", false);
        entry.targetTriple = j.value("triple", std::string{});
        // DOM walk -> structs (plus deep-mode deps freshness check). Distinct from the parse
        // span above so the allocation-bound conversion cost can be tracked separately.
        llvm::TimeTraceScope convertScope("CHeaderJsonConvert", cachePath.string());
        // Signatures this entry shares with every other request against the same header group are
        // held once in its baseline; the entry stores their indices. A baseline that will not load
        // is a miss, because the indices name nothing.
        std::shared_ptr<CSigBaseline> baseline;
        if (j.contains("sb"))
        {
            baseline = LoadSigBaseline(cacheDir, j.value("sb", std::string{}), /*forWrite*/ false);
            if (baseline == nullptr) return cacheMiss("signature baseline");
        }
        // Source paths are interned into one "files" table per document; a document written
        // before the table existed has none, and every entry then spells its path inline.
        CCachePaths pathTable;
        if (baseline != nullptr) pathTable = baseline->paths;
        if (j.contains("files"))
            for (auto& p : j["files"].to_string_vector()) pathTable.push_back(std::move(p));
        const CCachePaths* files = pathTable.empty() ? nullptr : &pathTable;
        try
        {
            if (j.contains("sigs"))
                for (const auto& s : j["sigs"])
                {
                    size_t slot = 0;
                    if (!s.as_index(slot)) { entry.sigs.push_back(SigFromJson(s, files)); continue; }
                    if (baseline == nullptr || slot >= baseline->sigs.size())
                        return cacheMiss("signature baseline index");
                    entry.sigs.push_back(baseline->sigs[slot]);
                }
            if (j.contains("functionTemplates"))
                for (const auto& t : j["functionTemplates"])
                    entry.functionTemplates.push_back(FunctionTemplateFromJson(t, files));
            if (j.contains("classTemplateNames"))
                entry.classTemplateNames = j["classTemplateNames"].to_string_vector();
            if (j.contains("enums"))      for (const auto& e : j["enums"])      entry.enums.push_back(EnumFromJson(e));
            if (j.contains("records"))    for (const auto& r : j["records"])    entry.records.push_back(RecordFromJson(r, files));
            if (j.contains("macros"))     for (const auto& m : j["macros"])     entry.macros.push_back(MacroFromJson(m, files));
            if (j.contains("funcMacros")) for (const auto& m : j["funcMacros"]) entry.funcMacros.push_back(FuncMacroFromJson(m, files));
            if (j.contains("globals"))    for (const auto& g : j["globals"])    entry.globals.push_back(GlobalFromJson(g));
            if (j.contains("recordAliases"))
                for (const auto& a : j["recordAliases"])
                    entry.recordAliases.emplace_back(a.value("a", std::string{}), a.value("t", std::string{}));
            if (j.contains("typeAliases"))
                for (const auto& a : j["typeAliases"])
                    entry.typeAliases.push_back(TypeAliasFromJson(a, files));
            if (j.contains("usingDirectives"))
                for (const auto& d : j["usingDirectives"])
                    entry.usingDirectives.emplace_back(
                        d.value("from", std::string{}), d.value("to", std::string{}));
            if (j.contains("namespaceAliases"))
                for (const auto& a : j["namespaceAliases"])
                    entry.namespaceAliases.emplace_back(
                        a.value("from", std::string{}), a.value("to", std::string{}));
            if (j.contains("functionPointerAbis"))
                for (const auto& p : j["functionPointerAbis"])
                {
                    cflat_cinterop::RawFunctionPointerAbi plan;
                    plan.signature = p.value("sig", std::string{});
                    plan.retType = p.value("rt", std::string{});
                    if (p.contains("pt")) plan.paramTypes = p["pt"].to_string_vector();
                    if (p.contains("abi")) plan.abi = AbiFromJson(p["abi"]);
                    entry.functionPointerAbis.push_back(std::move(plan));
                }
            // Companion module bitcode lives in a validated raw sidecar next to the JSON entry.
            if (j.contains("cxxbc"))
            {
                const SjVal blob = j["cxxbc"];
                if (!blob.contains("file") || !blob.contains("len") || !blob.contains("hash"))
                    return cacheMiss("missing sidecar metadata");
                const std::string sidecarName = blob.value("file", std::string{});
                const std::string sidecarPrefix = std::format("{:016x}.", diskKey);
                const fs::path sidecarRel(sidecarName);
                if (sidecarRel.filename() != sidecarRel
                    || !sidecarName.starts_with(sidecarPrefix)
                    || !sidecarName.ends_with(".bc"))
                    return cacheMiss("sidecar name");
                const fs::path sidecarPath = cacheDir / sidecarRel;
                const uint64_t expectedLength = blob.value("len", uint64_t{0});
                const uint64_t expectedHash = blob.value("hash", uint64_t{0});
                auto sidecar = [&] {
                    std::optional<llvm::TimeTraceScope> sidecarReadScope;
                    if (!expectedRequestKey.empty())
                        sidecarReadScope.emplace("CxxTypeRequestSidecarRead",
                                                  sidecarPath.string());
                    return llvm::MemoryBuffer::getFile(sidecarPath.string());
                }();
                if (!sidecar || (*sidecar)->getBuffer().size() != expectedLength)
                    return cacheMiss("missing or truncated sidecar");
                uint64_t actualHash = 0;
                bool sidecarHashOk;
                {
                    std::optional<llvm::TimeTraceScope> sidecarHashScope;
                    if (!expectedRequestKey.empty())
                        sidecarHashScope.emplace("CxxTypeRequestSidecarHash",
                                                  sidecarPath.string());
                    actualHash = 14695981039346656037ULL;
                    for (unsigned char byte : (*sidecar)->getBuffer())
                    {
                        actualHash ^= byte;
                        actualHash *= 1099511628211ULL;
                    }
                    sidecarHashOk = true;
                }
                if (!sidecarHashOk || actualHash != expectedHash)
                    return cacheMiss("sidecar hash");
                entry.cxxBitcode.assign((*sidecar)->getBuffer().data(), (*sidecar)->getBuffer().size());
            }
            else if (requireBitcode)
                return cacheMiss("missing sidecar");

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
                    if (!CHeaderDepFresh(dep)) return cacheMiss("dependency stamp");
                    entry.deps.push_back(std::move(dep));
                }
            }
        }
        catch (...) { return cacheMiss("incompatible entry"); }
        out = std::move(entry);
        return true;
    }

void LLVMBackend::LoadCxxTemplateOwnerMemo()
{
        if (cxxTemplateOwnerMemoLoaded_) return;
        cxxTemplateOwnerMemoLoaded_ = true;
        const std::string cacheDir = GetCHeaderCacheDir();
        if (cacheDir.empty()) return;
        std::ifstream input(std::filesystem::path(cacheDir) / "cxx-owner-groups.json");
        if (!input.is_open()) return;
        nlohmann::json doc;
        try { input >> doc; }
        catch (...) { return; }
        if (!doc.is_object() || doc.value("version", 0) != 1
            || doc.value("compilerBuildStamp", std::string{}) != CompilerBuildStamp())
            return;
        auto owners = doc.find("owners");
        if (owners == doc.end() || !owners->is_object()) return;
        for (auto it = owners->begin(); it != owners->end(); ++it)
        {
            const auto& value = it.value();
            if (!value.is_object()) continue;
            auto headers = value.find("headers");
            auto defines = value.find("defines");
            if (headers == value.end() || defines == value.end()
                || !headers->is_array() || !defines->is_array()) continue;
            try
            {
                CxxOwnerGroupMemo memo;
                memo.headers = headers->get<std::vector<std::string>>();
                memo.defines = defines->get<std::vector<std::string>>();
                if (!memo.headers.empty()) cxxTemplateOwnerMemo_.emplace(it.key(), std::move(memo));
            }
            catch (...) {}
        }
    }

void LLVMBackend::StoreCxxTemplateOwnerMemo(const std::string& cxxBase, size_t group)
{
        if (cxxBase.empty() || group >= cxxImportGroups_.size()) return;
        const CxxImportGroup& owner = cxxImportGroups_[group];
        if (owner.headers.empty() || runMode_ || batchMode_
            || retryingTentativeCxxType_ || symbolSink_ != nullptr)
            return;
        const std::string cacheDir = GetCHeaderCacheDir();
        if (cacheDir.empty()) return;
        LoadCxxTemplateOwnerMemo();
        cxxTemplateOwnerMemo_[cxxBase] = { owner.headers, owner.defines };

        nlohmann::json doc = {
            {"version", 1},
            {"compilerBuildStamp", CompilerBuildStamp()},
            {"owners", nlohmann::json::object()}
        };
        for (const auto& [base, memo] : cxxTemplateOwnerMemo_)
            doc["owners"][base] = {{"headers", memo.headers}, {"defines", memo.defines}};

        namespace fs = std::filesystem;
        const fs::path path = fs::path(cacheDir) / "cxx-owner-groups.json";
        const fs::path temp = fs::path(cacheDir)
            / std::format("cxx-owner-groups.{}.tmp", _getpid());
        std::error_code ec;
        fs::create_directories(cacheDir, ec);
        if (ec) return;
        {
            std::ofstream output(temp, std::ios::binary | std::ios::trunc);
            if (!output.is_open()) return;
            output << doc;
            output.close();
            if (!output)
            {
                fs::remove(temp, ec);
                return;
            }
        }
        fs::rename(temp, path, ec);
        if (ec)
        {
            ec.clear();
            fs::remove(path, ec);
            ec.clear();
            fs::rename(temp, path, ec);
        }
        if (ec) fs::remove(temp, ec);
    }

// Guards the process-wide baseline cache below. A baseline is immutable once written, so one
// loaded copy is shared by every entry that names it.
static std::mutex gSigBaselineMutex;

// 64-bit FNV-1a, spelled as the surrounding cache keys already spell it.
static uint64_t SigBaselineHash(const std::string& text)
{
        uint64_t h = 14695981039346656037ULL;
        for (unsigned char byte : text)
        {
            h ^= byte;
            h *= 1099511628211ULL;
        }
        return h;
    }

static std::string CxxRequestGroupMarker(const std::vector<std::string>& headers,
                                         const std::vector<std::string>& defines);

std::string LLVMBackend::SigBaselineGroupKey(uint64_t headerHash,
                                             std::filesystem::file_time_type mtime,
                                             const std::string& triple,
                                             const CxxRequestGroup& group)
{
        std::string key = std::format("{:016x}|{}|{}|{}", headerHash,
                                      (long long)mtime.time_since_epoch().count(), triple,
                                      kCHeaderCacheVersion);
        for (const auto& h : group.headers) key += "|H" + h;
        for (const auto& d : group.defines) key += "|D" + d;
        return std::format("{:016x}", SigBaselineHash(key));
    }

std::shared_ptr<LLVMBackend::CSigBaseline> LLVMBackend::LoadSigBaseline(
        const std::filesystem::path& cacheDir, const std::string& id, bool forWrite)
{
        // Only a successful load is memoized. A failed one must stay re-probeable: this process
        // may be the one that goes on to write that very baseline.
        static std::unordered_map<std::string, std::shared_ptr<CSigBaseline>> loaded;
        if (id.empty()) return nullptr;
        std::lock_guard<std::mutex> lock(gSigBaselineMutex);
        auto it = loaded.find(id);
        if (it == loaded.end())
        {
            std::shared_ptr<CSigBaseline> built;
            const auto path = cacheDir / std::format("sigbase.{}.json", id);
            simdjson::dom::parser parser;
            auto text = simdjson::padded_string::load(path.string());
            simdjson::dom::element doc;
            if (!text.error() && parser.parse(text.value()).get(doc) == simdjson::SUCCESS)
            {
                SjVal j{doc};
                built = std::make_shared<CSigBaseline>();
                built->id = id;
                built->paths = j["files"].to_string_vector();
                const CCachePaths* files = built->paths.empty() ? nullptr : &built->paths;
                try
                {
                    for (const auto& s : j["sigs"]) built->sigs.push_back(SigFromJson(s, files));
                }
                catch (...)
                {
                    built.reset();
                }
            }
            if (built == nullptr) return nullptr;
            it = loaded.emplace(id, std::move(built)).first;
        }
        std::shared_ptr<CSigBaseline> found = it->second;
        if (found != nullptr && forWrite && !found->byTextBuilt)
        {
            // Match by the exact bytes a store would write. Seeding the table from the baseline's
            // own paths is what makes an unchanged signature serialize identically in both.
            CCachePathTable table;
            for (const auto& p : found->paths) table.Intern(p);
            for (size_t i = 0; i < found->sigs.size(); ++i)
                found->byText.emplace(SigToJson(found->sigs[i], &table).dump(), i);
            found->byTextBuilt = true;
        }
        return found;
    }

/*
 * A group's first sizeable entry becomes its baseline and every later one encodes against it. The
 * pointer file is only a hint - a baseline is named by its content, so a stale or racing pointer
 * costs a larger entry, never a wrong signature. A much larger harvest replaces the pointer, which
 * leaves entries already written against the old baseline resolving exactly as before: the group's
 * first entry may have been a small member request, and a baseline that covers little saves little.
 */
std::shared_ptr<LLVMBackend::CSigBaseline> LLVMBackend::AcquireSigBaseline(
        const std::filesystem::path& cacheDir, const std::string& groupKey,
        const std::vector<CSigEntry>& sigs)
{
        namespace fs = std::filesystem;
        // Below this an entry is cheaper to store whole than to describe against a baseline.
        static constexpr size_t kMinBaselineSigs = 64;
        if (groupKey.empty() || sigs.size() < kMinBaselineSigs) return nullptr;

        std::error_code ec;
        const auto pointerPath = cacheDir / std::format("sigbase.{}.ptr", groupKey);
        {
            std::ifstream pointer(pointerPath, std::ios::binary);
            std::string id;
            if (pointer.is_open() && std::getline(pointer, id) && !id.empty())
            {
                auto existing = LoadSigBaseline(cacheDir, id, /*forWrite*/ true);
                // A far richer harvest promotes itself over the one that got here first.
                if (existing != nullptr && sigs.size() <= existing->sigs.size() * 2)
                    return existing;
            }
        }

        CCachePathTable files;
        nlohmann::json body = nlohmann::json::array();
        for (const auto& s : sigs) body.push_back(SigToJson(s, &files));
        nlohmann::json doc;
        doc["files"] = files.paths;
        doc["sigs"]  = body;
        const std::string text = doc.dump();
        const std::string id = std::format("{:016x}", SigBaselineHash(text));
        const auto path = cacheDir / std::format("sigbase.{}.json", id);
        if (!fs::exists(path, ec))
        {
            const auto temp = cacheDir / std::format("sigbase.{}.{}.tmp", id, _getpid());
            {
                std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                if (!out.is_open()) return nullptr;
                out << text;
                out.close();
                if (!out) { fs::remove(temp, ec); return nullptr; }
            }
            ec.clear();
            fs::rename(temp, path, ec);
            if (ec) { ec.clear(); fs::remove(temp, ec); }
        }
        {
            const auto temp = cacheDir / std::format("sigbase.{}.{}.ptr.tmp", groupKey, _getpid());
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (out.is_open())
            {
                out << id;
                out.close();
                ec.clear();
                fs::rename(temp, pointerPath, ec);
                if (ec) { ec.clear(); fs::remove(temp, ec); }
            }
        }
        return LoadSigBaseline(cacheDir, id, /*forWrite*/ true);
    }

void LLVMBackend::WriteCHeaderDiskCache(
        const std::filesystem::path& cacheDir,
        uint64_t diskKey,
        std::filesystem::file_time_type mtime,
        uint64_t contentHash,
        const CFileSigCacheEntry& entry,
        const std::string& requestKey,
        const CxxRequestGroup* requestGroup)
{
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(cacheDir, ec);
        if (ec) return;

        nlohmann::json j;
        // v69 makes cached C++ signature payloads key-pure by remapping foreign types on replay.
        // v70 maps a std::function return to its std.function specialization.
        // v71 harvests C++ namespace aliases and refuses a 'consteval' function.
        // v72 records `const` on a C++ reference parameter (IsCxxConstRef).
        // v73 records an alias template's target base, argument pattern and parameter defaults
        // (catb/caa/cad). v74 publishes FREE BINARY OPERATOR templates.
        // v76 records `explicit` on a C++ conversion operator too (xpl).
        // v77 interns source paths into "files", drops defaulted signature fields, and stores a
        // type request's shared signatures once in a baseline ("sb") instead of per entry.
        // v78 binds a reference to a T** instead of refusing the member.
        // v79 preserves the pointer level of a collapsed const-qualified pointer reference.
        // v80 records function-template forwarding-reference parameters.
        // v81 records a public constructor template on a C++ record (hct).
        // v82 registers explicit free-function-template wrappers under their unique names.
        // v84 defines vtable virtual members + used out-of-line inline members in the companion
        // bitcode, and records virtual-base offsets (vbs).
        // v85 omits out-of-line C++ static data members from the bare-global list.
        // v86 exports class-scope operator new / delete / new[] / delete[] members.
        // v87 recognizes forwarding references in C++ function parameter packs.
        // v88 publishes global class and alias-template names for bare CFlat type requests.
        // v89 stores function-template parameter names and forwarding template-parameter indices.
        // v90 content-addresses companion bitcode and records its byte length with the hash.
        // v91 stores long double signatures with their distinct longdouble identity.
        // v92 refuses bodies reaching an incremental chunk's emptied (poisoned) specialization
        // and stores the clang diagnostic behind a member refusal (refusalCause).
        // v93 exports non-special user operator= overloads and member templates for assignment calls.
        // v94 caps each serialized C++ member refusal cause at 4 KB on a line boundary.
        // v95 refuses a member whose signature contains clang error nodes instead of mangling it.
        j["version"] = kCHeaderCacheVersion;
        j["mtime"]   = (int64_t)mtime.time_since_epoch().count();
        j["hash"]    = contentHash;
        j["ldw"]     = entry.longDoubleWidth;
        j["ldieee"]  = entry.longDoubleIsIEEEDouble;
        j["triple"]  = entry.targetTriple;
        if (!requestKey.empty() && requestGroup != nullptr)
        {
            j["cxxRequestKey"] = requestKey;
            j["cxxRequestHeaders"] = requestGroup->headers;
            j["cxxRequestDefines"] = requestGroup->defines;
        }

        // Every entry below stores its source path as an index into this table rather than
        // repeating the path itself; "files" is written once the last of them has interned.
        // A baseline seeds the table with its own paths so an unchanged signature serializes to
        // the same bytes here as it did there; only the paths beyond them are written out.
        CCachePathTable files;
        std::shared_ptr<CSigBaseline> baseline;
        if (requestGroup != nullptr)
            baseline = AcquireSigBaseline(
                cacheDir, SigBaselineGroupKey(contentHash, mtime, entry.targetTriple, *requestGroup),
                entry.sigs);
        if (baseline != nullptr)
        {
            j["sb"] = baseline->id;
            for (const auto& p : baseline->paths) files.Intern(p);
        }
        const size_t sharedPathCount = files.paths.size();

        // Each signature stores either its index in the baseline or, when the baseline does not
        // have it, the signature itself. Order is preserved exactly: replay depends on it.
        nlohmann::json sigs = nlohmann::json::array();
        for (const auto& s : entry.sigs)
        {
            nlohmann::json one = SigToJson(s, &files);
            if (baseline != nullptr)
            {
                auto hit = baseline->byText.find(one.dump());
                if (hit != baseline->byText.end())
                {
                    sigs.push_back(hit->second);
                    continue;
                }
            }
            sigs.push_back(std::move(one));
        }
        j["sigs"] = sigs;
        nlohmann::json functionTemplates = nlohmann::json::array();
        for (const auto& t : entry.functionTemplates)
            functionTemplates.push_back(FunctionTemplateToJson(t, &files));
        j["functionTemplates"] = functionTemplates;
        j["classTemplateNames"] = entry.classTemplateNames;
        nlohmann::json enums = nlohmann::json::array();
        for (const auto& e : entry.enums) enums.push_back(EnumToJson(e));
        j["enums"] = enums;
        nlohmann::json records = nlohmann::json::array();
        for (const auto& r : entry.records) records.push_back(RecordToJson(r, &files));
        j["records"] = records;
        nlohmann::json macros = nlohmann::json::array();
        for (const auto& m : entry.macros) macros.push_back(MacroToJson(m, &files));
        j["macros"] = macros;
        nlohmann::json funcMacros = nlohmann::json::array();
        for (const auto& m : entry.funcMacros) funcMacros.push_back(FuncMacroToJson(m, &files));
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
            typeAliases.push_back(TypeAliasToJson(a, &files));
        j["typeAliases"] = typeAliases;
        nlohmann::json usingDirectives = nlohmann::json::array();
        for (const auto& d : entry.usingDirectives)
            usingDirectives.push_back({{"from", d.first}, {"to", d.second}});
        j["usingDirectives"] = usingDirectives;
        nlohmann::json namespaceAliases = nlohmann::json::array();
        for (const auto& a : entry.namespaceAliases)
            namespaceAliases.push_back({{"from", a.first}, {"to", a.second}});
        j["namespaceAliases"] = namespaceAliases;
        nlohmann::json functionPointerAbis = nlohmann::json::array();
        for (const auto& p : entry.functionPointerAbis)
            functionPointerAbis.push_back({{"sig", p.signature}, {"rt", p.retType},
                                           {"pt", p.paramTypes}, {"abi", AbiToJson(p.abi)}});
        j["functionPointerAbis"] = functionPointerAbis;
        // Only the paths this entry added: the reader rebuilds the table as the baseline's paths
        // followed by these, which is the order they were interned in.
        j["files"] = std::vector<std::string>(files.paths.begin() + sharedPathCount,
                                              files.paths.end());

        const uint64_t tempId = gCHeaderDiskCacheTempCounter.fetch_add(1, std::memory_order_relaxed);
        auto tmpPath  = cacheDir / std::format("{:016x}.{}.{}.tmp", diskKey, _getpid(), tempId);
        auto destPath = cacheDir / std::format("{:016x}.json", diskKey);
        fs::path sidecarPath;
        fs::path sidecarTmpPath;
        if (!entry.cxxBitcode.empty())
        {
            const uint64_t sidecarHash = SigBaselineHash(entry.cxxBitcode);
            const auto publishedAt = std::chrono::system_clock::now().time_since_epoch().count();
            const std::string sidecarName = std::format("{:016x}.{:016x}.{}.{}.bc",
                                                        diskKey, sidecarHash, publishedAt,
                                                        _getpid(), tempId);
            sidecarPath = cacheDir / sidecarName;
            sidecarTmpPath = cacheDir
                / std::format("{:016x}.{}.{}.{}.bc.tmp", diskKey, _getpid(), tempId, sidecarHash);
            {
                std::ofstream f(sidecarTmpPath, std::ios::binary | std::ios::trunc);
                if (!f.is_open()) return;
                f.write(entry.cxxBitcode.data(), static_cast<std::streamsize>(entry.cxxBitcode.size()));
                if (!f)
                {
                    f.close();
                    fs::remove(sidecarTmpPath, ec);
                    return;
                }
            }
            j["cxxbc"] = {{"file", sidecarPath.filename().string()},
                           {"len", static_cast<uint64_t>(entry.cxxBitcode.size())},
                           {"hash", sidecarHash}};
        }

        // The transitive include set (header imports) for transitive validation.
        if (!entry.deps.empty())
        {
            nlohmann::json deps = nlohmann::json::array();
            for (const auto& d : entry.deps)
                deps.push_back({{"f", d.path}, {"mt", d.mtime}, {"h", d.hash}});
            j["deps"] = deps;
        }

        // The content-addressed sidecar is committed first; this JSON file is the entry index.
        {
            std::ofstream f(tmpPath);
            if (!f.is_open())
            {
                return;
            }
            f << j;
            if (!f)
            {
                f.close();
                fs::remove(tmpPath, ec);
                return;
            }
        }
        {
            std::lock_guard<std::mutex> publishLock(gCHeaderDiskCachePublishMutex);
            if (!sidecarTmpPath.empty())
            {
                ec = llvm::sys::fs::rename(sidecarTmpPath.string(), sidecarPath.string());
                if (ec)
                {
                    fs::remove(sidecarTmpPath, ec);
                    fs::remove(tmpPath, ec);
                    return;
                }
            }
            ec = llvm::sys::fs::rename(tmpPath.string(), destPath.string());
        }
        if (ec)
        {
            fs::remove(tmpPath, ec);
            return;
        }
        if (!requestKey.empty() && requestGroup != nullptr)
        {
            const std::string marker = CxxRequestGroupMarker(requestGroup->ownerHeaders,
                                                              requestGroup->ownerDefines);
            const auto markerPath = cacheDir / std::format("{:016x}.rq", diskKey);
            const auto markerTmpPath = cacheDir
                / std::format("{:016x}.{}.{}.rq.tmp", diskKey, _getpid(), tempId);
            std::ofstream markerFile(markerTmpPath, std::ios::binary | std::ios::trunc);
            if (markerFile.is_open())
            {
                markerFile << marker;
                markerFile.close();
                if (markerFile)
                {
                    {
                        std::lock_guard<std::mutex> publishLock(gCHeaderDiskCachePublishMutex);
                        ec = llvm::sys::fs::rename(markerTmpPath.string(), markerPath.string());
                    }
                    if (ec) { ec.clear(); fs::remove(markerTmpPath, ec); }
                }
                else { fs::remove(markerTmpPath, ec); ec.clear(); }
            }
            std::lock_guard<std::mutex> publishLock(gCHeaderDiskCachePublishMutex);
            PruneCxxTypeRequestDiskCache(cacheDir, *requestGroup);
        }
    }

static std::string CxxRequestGroupMarker(const std::vector<std::string>& headers,
                                         const std::vector<std::string>& defines)
{
        std::string marker = "H" + std::to_string(headers.size()) + ":";
        for (const auto& header : headers)
            marker += std::to_string(header.size()) + ":" + header;
        marker += "D" + std::to_string(defines.size()) + ":";
        for (const auto& define : defines)
            marker += std::to_string(define.size()) + ":" + define;
        return marker;
    }

void LLVMBackend::PruneCxxTypeRequestDiskCache(const std::filesystem::path& cacheDir,
                                                const CxxRequestGroup& group)
{
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::is_directory(cacheDir, ec)) return;
        const std::string owner = CxxRequestGroupMarker(group.ownerHeaders, group.ownerDefines);
        const auto now = fs::file_time_type::clock::now();
        constexpr auto grace = std::chrono::minutes(10);
        for (const auto& file : fs::directory_iterator(cacheDir, ec))
        {
            if (ec) return;
            if (file.path().extension() != ".rq") continue;
            const auto age = now - fs::last_write_time(file.path(), ec);
            if (ec) { ec.clear(); continue; }
            if (age < grace) continue;
            std::ifstream input(file.path(), std::ios::binary);
            std::string marker;
            if (!input.is_open() || !std::getline(input, marker) || marker != owner) continue;
            auto jsonPath = file.path();
            jsonPath.replace_extension(".json");
            fs::remove(jsonPath, ec); ec.clear();
            fs::remove(file.path(), ec); ec.clear();
        }
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
                InsertCFileSigEntry(inMemKey, std::move(diskEntry), verbose);
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
            bool ok = CompileCHeaderGroup(headerCanonicals, extraDefines);
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
                                "'if const (__WINDOWS__) {{ import ...; }}'.",
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
