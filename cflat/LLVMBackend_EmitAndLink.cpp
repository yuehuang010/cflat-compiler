#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/AssemblyAnnotationWriter.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/StructuralHash.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/DiagnosticPrinter.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/StandardInstrumentations.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/Instrumentation/AddressSanitizer.h>
#include <llvm/Object/COFF.h>
#include <llvm/Object/Binary.h>
#include <llvm/Object/Archive.h>
#include <llvm/Object/COFFImportFile.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Object/SymbolSize.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/TimeProfiler.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/FormattedStream.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/JSON.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticHandler.h>
#include <llvm/ADT/DenseMap.h>
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
#include <fstream>
#include <charconv>
#include <cstdint>
#include <tuple>
#include <vector>

namespace {

using RootFilePredicate = std::function<bool(const llvm::DIFile*)>;

struct PromotedSymbol
{
    std::string name;
    llvm::GlobalValue::LinkageTypes linkage;
    llvm::GlobalValue::VisibilityTypes visibility;
    llvm::GlobalValue::UnnamedAddr unnamedAddr;
    bool isFunction;
};

bool IsLocalLinkage(const llvm::GlobalValue& value)
{
    return value.getLinkage() == llvm::GlobalValue::InternalLinkage ||
           value.getLinkage() == llvm::GlobalValue::PrivateLinkage;
}

void PreserveZeroInitializers(llvm::Module& source, const llvm::Module& work,
                              const std::set<std::string>& changedGlobals)
{
    for (llvm::GlobalVariable& sourceGlobal : source.globals())
    {
        if (!sourceGlobal.hasName() || !sourceGlobal.hasInitializer() ||
            !sourceGlobal.getInitializer()->isNullValue())
            continue;
        const llvm::GlobalVariable* workGlobal = work.getNamedGlobal(sourceGlobal.getName());
        if (workGlobal && workGlobal->hasInitializer() &&
            workGlobal->getInitializer()->isNullValue() &&
            !changedGlobals.contains(sourceGlobal.getName().str()))
            sourceGlobal.setInitializer(nullptr);
    }
}

bool RestorePromotedLinkage(llvm::Module& module,
                            const std::vector<PromotedSymbol>& promotedSymbols)
{
    for (const PromotedSymbol& symbol : promotedSymbols)
    {
        llvm::GlobalValue* value = symbol.isFunction
            ? static_cast<llvm::GlobalValue*>(module.getFunction(symbol.name))
            : static_cast<llvm::GlobalValue*>(module.getNamedGlobal(symbol.name));
        if (!value)
            return false;
        value->setLinkage(symbol.linkage);
        value->setVisibility(symbol.visibility);
        value->setUnnamedAddr(symbol.unnamedAddr);
    }
    return true;
}

std::string DebugFilePath(const llvm::DIFile* file)
{
    if (!file) return {};
    std::filesystem::path path(file->getFilename().str());
    if (!path.is_absolute() && !file->getDirectory().empty())
        path = std::filesystem::path(file->getDirectory().str()) / path;
    return path.lexically_normal().string();
}

std::string DebugFileName(const llvm::DIFile* file)
{
    if (!file) return {};
    return std::filesystem::path(file->getFilename().str()).filename().string();
}

// --- Assembly directive parsing, shared by the view and the optimization-info collector ---

std::vector<std::string> ParseQuotedStrings(const std::string& line, size_t pos)
{
    std::vector<std::string> values;
    while (pos < line.size())
    {
        while (pos < line.size() && std::isspace((unsigned char)line[pos])) ++pos;
        if (pos >= line.size() || line[pos] != '"') break;
        ++pos;
        std::string value;
        while (pos < line.size() && line[pos] != '"')
        {
            if (line[pos] == '\\' && pos + 1 < line.size()) ++pos;
            value += line[pos++];
        }
        if (pos < line.size()) ++pos;
        values.push_back(std::move(value));
    }
    return values;
}

// Windows targets emit CodeView (.cv_file/.cv_loc); other targets emit DWARF (.file/.loc).
bool ParseFileDirective(const std::string& line, int& id, std::vector<std::string>& values)
{
    size_t pos = 0;
    while (pos < line.size() && std::isspace((unsigned char)line[pos])) ++pos;
    size_t directiveLength = 0;
    if (line.compare(pos, 5, ".file") == 0)
        directiveLength = 5;
    else if (line.compare(pos, 8, ".cv_file") == 0)
        directiveLength = 8;
    else
        return false;
    pos += directiveLength;
    if (pos < line.size() && !std::isspace((unsigned char)line[pos])) return false;
    while (pos < line.size() && std::isspace((unsigned char)line[pos])) ++pos;
    size_t start = pos;
    while (pos < line.size() && std::isdigit((unsigned char)line[pos])) ++pos;
    if (start == pos) return false;
    id = std::stoi(line.substr(start, pos - start));
    values = ParseQuotedStrings(line, pos);
    return !values.empty();
}

bool ParseLocDirective(const std::string& line, int& id, int& sourceLine, int& column)
{
    size_t pos = 0;
    while (pos < line.size() && std::isspace((unsigned char)line[pos])) ++pos;
    bool codeView = false;
    if (line.compare(pos, 4, ".loc") == 0)
        pos += 4;
    else if (line.compare(pos, 7, ".cv_loc") == 0)
    {
        pos += 7;
        codeView = true;
    }
    else
        return false;
    if (pos < line.size() && !std::isspace((unsigned char)line[pos])) return false;
    auto nextInt = [&](int& value) {
        while (pos < line.size() && std::isspace((unsigned char)line[pos])) ++pos;
        size_t start = pos;
        while (pos < line.size() && std::isdigit((unsigned char)line[pos])) ++pos;
        if (start == pos) return false;
        value = std::stoi(line.substr(start, pos - start));
        return true;
    };
    column = 0;
    int functionId = 0;
    if (codeView && !nextInt(functionId)) return false;
    if (!nextInt(id) || !nextInt(sourceLine)) return false;
    nextInt(column);
    return true;
}

std::string NormalizeFilePath(const std::string& path)
{
    return std::filesystem::path(path).lexically_normal().string();
}

std::string AsmFilePath(const std::vector<std::string>& values)
{
    if (values.empty()) return std::string();
    std::filesystem::path candidate;
    if (values.size() == 1)
        candidate = values[0];
    else
        candidate = std::filesystem::path(values[0]) / values[1];
    return NormalizeFilePath(candidate.string());
}

// File-id -> path table built from the .file/.cv_file directives in an assembly listing.
std::map<int, std::string> BuildAsmFileTable(const std::string& assembly)
{
    std::map<int, std::string> asmFiles;
    size_t lineStart = 0;
    while (lineStart < assembly.size())
    {
        size_t lineEnd = assembly.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = assembly.size();
        int id = 0;
        std::vector<std::string> values;
        std::string line = assembly.substr(lineStart, lineEnd - lineStart);
        if (ParseFileDirective(line, id, values))
            asmFiles[id] = AsmFilePath(values);
        lineStart = lineEnd == assembly.size() ? assembly.size() : lineEnd + 1;
    }
    return asmFiles;
}

// Definitions whose symbol corresponds to the given source-level function name, covering
// plain, namespace-qualified and generic-mangled spellings. Empty name matches nothing.
std::vector<llvm::Function*> MatchingFunctions(const LLVMBackend& compiler, llvm::Module& m,
                                               const std::string& functionName)
{
    std::vector<llvm::Function*> matches;
    if (functionName.empty()) return matches;
    const std::string mangledPrefix = "_" + functionName + "$";
    const std::string qualifiedPrefix = "." + functionName + "$";
    const size_t selectorDot = functionName.rfind('.');
    const bool qualifiedSource = selectorDot != std::string::npos && selectorDot > 0
        && selectorDot + 1 < functionName.size();
    const std::string selectorReceiver = qualifiedSource
        ? functionName.substr(0, selectorDot) : std::string();
    const std::string selectorMethod = qualifiedSource
        ? functionName.substr(selectorDot + 1) : std::string();
    auto normalize = [](std::string text) {
        std::erase_if(text, [](char c) { return std::isspace((unsigned char)c) != 0; });
        return text;
    };
    for (auto& function : m)
    {
        if (function.isDeclaration()) continue;
        llvm::StringRef name = function.getName();
        const std::string sourceSymbol = SpellFunctionSymbol(compiler, name.str());
        const size_t openParen = sourceSymbol.find('(');
        const std::string sourceName = sourceSymbol.substr(0, openParen);
        bool receiverMatch = false;
        if (qualifiedSource && sourceName == selectorMethod && openParen != std::string::npos)
        {
            const size_t firstEnd = sourceSymbol.find(',', openParen + 1);
            const size_t closeParen = sourceSymbol.find(')', openParen + 1);
            const size_t end = firstEnd == std::string::npos ? closeParen : firstEnd;
            if (end != std::string::npos)
            {
                const std::string firstParameter = normalize(
                    sourceSymbol.substr(openParen + 1, end - openParen - 1));
                const std::string receiver = normalize(selectorReceiver);
                receiverMatch = firstParameter == receiver || firstParameter == receiver + "*";
            }
        }
        if (name == functionName
            || name.starts_with(mangledPrefix)
            || name.contains(qualifiedPrefix)
            || sourceName == functionName
            || receiverMatch)
            matches.push_back(&function);
    }
    return matches;
}

// Frontend-expanded return-block bodies retain the call-site location and have no inline chain.
// They cannot be attributed to their source callee.
std::vector<LLVMBackend::LineFrame> BuildLineStack(const llvm::DILocation* location,
                                                   const RootFilePredicate& isRootFile)
{
    std::vector<LLVMBackend::LineFrame> stack;
    for (auto* current = location; current; current = current->getInlinedAt())
    {
        const llvm::DIFile* file = current->getFile();
        std::string functionName;
        if (auto* scope = current->getScope())
            if (auto* subprogram = scope->getSubprogram())
                functionName = subprogram->getName().str();
        stack.push_back({DebugFileName(file), static_cast<int>(current->getLine()),
                         std::move(functionName), isRootFile(file)});
    }
    return stack;
}

LLVMBackend::LineMapping MakeLineMapping(const std::vector<LLVMBackend::LineFrame>& stack)
{
    LLVMBackend::LineMapping mapping;
    for (const auto& frame : stack)
        if (frame.root)
        {
            mapping.srcLine = frame.line;
            break;
        }
    if (stack.size() > 1
        || std::any_of(stack.begin(), stack.end(), [](const auto& frame) { return !frame.root; }))
        mapping.stack = stack;
    return mapping;
}

bool SameLineFrame(const LLVMBackend::LineFrame& left,
                  const LLVMBackend::LineFrame& right)
{
    return left.file == right.file && left.line == right.line
        && left.func == right.func && left.root == right.root;
}

bool SameLineStack(const std::vector<LLVMBackend::LineFrame>& left,
                   const std::vector<LLVMBackend::LineFrame>& right)
{
    if (left.size() != right.size()) return false;
    return std::equal(left.begin(), left.end(), right.begin(), SameLineFrame);
}

class LineMappingAnnotationWriter final : public llvm::AssemblyAnnotationWriter
{
public:
    LineMappingAnnotationWriter(std::vector<LLVMBackend::LineMapping>& mappings,
                                 std::function<LLVMBackend::LineMapping(const llvm::DILocation*)> makeMapping)
        : mappings_(mappings), makeMapping_(std::move(makeMapping)) {}

    void emitInstructionAnnot(const llvm::Instruction* instruction,
                              llvm::formatted_raw_ostream& stream) override
    {
        llvm::DebugLoc debugLoc = instruction->getDebugLoc();
        if (!debugLoc) return;
        auto mapping = makeMapping_(debugLoc.get());
        if (mapping.srcLine <= 0) return;
        int viewLine = baseLine_ + static_cast<int>(stream.getLine()) + 1;
        mapping.viewStart = viewLine;
        mapping.viewEnd = viewLine;
        mappings_.push_back(std::move(mapping));
    }

    // Function::print creates a fresh formatted_raw_ostream (line counter reset to 0)
    // per call; set this to the lines already emitted before each per-function print.
    void SetBaseLine(int baseLine) { baseLine_ = baseLine; }

private:
    std::vector<LLVMBackend::LineMapping>& mappings_;
    std::function<LLVMBackend::LineMapping(const llvm::DILocation*)> makeMapping_;
    int baseLine_ = 0;
};

void ConsolidateLineMappings(std::vector<LLVMBackend::LineMapping>& mappings)
{
    std::vector<LLVMBackend::LineMapping> consolidated;
    for (const auto& mapping : mappings)
    {
        if (!consolidated.empty()
            && consolidated.back().srcLine == mapping.srcLine
            && SameLineStack(consolidated.back().stack, mapping.stack)
            && consolidated.back().viewEnd + 1 == mapping.viewStart)
            consolidated.back().viewEnd = mapping.viewEnd;
        else
            consolidated.push_back(mapping);
    }
    mappings = std::move(consolidated);
}

}

#if defined(__APPLE__)
// Step 3 (macOS self-contained link): harvest libSystem's exported symbols from
// the live dyld shared cache to synthesize a linker stub, so -o needs no SDK.
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <dlfcn.h>
#include <cstring>
#include <sys/sysctl.h>
#endif

#if defined(_WIN32)
struct CflatActCtxW
{
    unsigned long cbSize;
    unsigned long dwFlags;
    const wchar_t* lpSource;
    unsigned short wProcessorArchitecture;
    unsigned short wLangId;
    const wchar_t* lpAssemblyDirectory;
    const wchar_t* lpResourceName;
    const wchar_t* lpApplicationName;
    void* hModule;
};

extern "C" void* __stdcall CreateActCtxW(const CflatActCtxW* actCtx);
extern "C" int __stdcall QueryActCtxSettingsW(unsigned long flags, void* actCtx,
    const wchar_t* settingsNamespace, const wchar_t* settingName, wchar_t* buffer,
    size_t bufferLength, size_t* writtenOrRequired);
extern "C" void __stdcall ReleaseActCtx(void* actCtx);
extern "C" unsigned long __stdcall GetLastError();

static std::wstring ManifestAsciiToWide(const std::string& text)
{
    return std::wstring(text.begin(), text.end());
}
#endif

// ---- Definitions moved out of LLVMBackend.h (EmitAndLink) ----

// Map cflat -O to codegen effort the way clang does: the default -O0 build gets FastISel +
// fast regalloc (CodeGenOptLevel::None); optimized builds keep SelectionDAG quality.
static llvm::CodeGenOptLevel CodeGenLevelFor(int optLevel)
{
    if (optLevel <= 0) return llvm::CodeGenOptLevel::None;
    if (optLevel == 1) return llvm::CodeGenOptLevel::Less;
    if (optLevel == 2) return llvm::CodeGenOptLevel::Default;
    return llvm::CodeGenOptLevel::Aggressive;
}

std::unique_ptr<llvm::TargetMachine> LLVMBackend::CreateOptTargetMachine(int optLevel)
{
        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmPrinters();

        std::string triple = targetMacOS_
            ? std::string("arm64-apple-macosx")
            : targetWindows_ ? (platformValue == 32 ? "i686-pc-windows-msvc" : "x86_64-pc-windows-msvc")
                             : llvm::sys::getProcessTriple();
        std::string cpu = !targetCpu_.empty()
            ? targetCpu_
            : DefaultCpuForPlatform(targetMacOS_ ? "macos"
                                    : targetWindows_ ? (platformValue == 32 ? "win32" : "win64")
                                                     : "linux");

        std::string err;
        const llvm::Target* target = llvm::TargetRegistry::lookupTarget(llvm::Triple(triple), err);
        if (!target)
            return nullptr;

        llvm::TargetOptions opt;
        if (optLevel < 0) optLevel = cOptLevel_;
        return std::unique_ptr<llvm::TargetMachine>(
            target->createTargetMachine(llvm::Triple(triple), cpu, "", opt, llvm::Reloc::PIC_,
                                        std::nullopt, CodeGenLevelFor(optLevel)));
    }

// Materialize the core and clone the module so a view/measurement pass can mutate it
// freely. `purpose` names the caller in the error message.
std::unique_ptr<llvm::Module> LLVMBackend::CloneModuleForView(const std::string& purpose)
{
    llvm::TimeTraceScope cloneScope("ViewCloneModule", purpose);
    if (!module)
        return nullptr;

    // A warm compiler cache leaves the core as a lazily-loaded bitcode module, and
    // CloneModule requires a materialized one. Every other module consumer materializes
    // first; without this the clone walks unread function bodies and faults.
    {
        llvm::TimeTraceScope materializeScope("ViewMaterializeCore", purpose);
        MaterializeCoreIfLazy();
    }
    if (!module->isMaterialized())
    {
        LogError("cannot render " + purpose + ": cached core bitcode is not materialized.");
        return nullptr;
    }
    return llvm::CloneModule(*module);
}

// Point the clone at the target machine and run the O<optLevel> pipeline on it. The
// machine is only created when the caller needs one; codegen always does.
bool LLVMBackend::OptimizeViewModule(llvm::Module& view, int optLevel, bool needTargetMachine,
                                     std::unique_ptr<llvm::TargetMachine>& outMachine)
{
    if (needTargetMachine)
    {
        outMachine = CreateOptTargetMachine(optLevel);
        if (!outMachine)
            return false;
        view.setTargetTriple(outMachine->getTargetTriple());
        view.setDataLayout(outMachine->createDataLayout());
    }

    if (optLevel > 0)
    {
        llvm::TimeTraceScope pipelineScope("ViewOptPipeline",
                                           "O" + std::to_string(optLevel));
        llvm::PipelineTuningOptions pto;
        pto.LoopVectorization = true;
        pto.SLPVectorization = true;
        // OptNone skipping lives in StandardInstrumentations, not the pass manager
        // core; without it the incremental path's optnone-protected kept bodies
        // would be re-optimized by every function pass.
        llvm::PassInstrumentationCallbacks instrumentationCallbacks;
        llvm::StandardInstrumentations instrumentation(view.getContext(),
                                                       /*DebugLogging=*/false);
        llvm::PassBuilder passBuilder(outMachine.get(), pto, std::nullopt,
                                      &instrumentationCallbacks);
        llvm::LoopAnalysisManager loopAnalysis;
        llvm::FunctionAnalysisManager functionAnalysis;
        llvm::CGSCCAnalysisManager cgsccAnalysis;
        llvm::ModuleAnalysisManager moduleAnalysis;
        llvm::TargetLibraryInfoImpl tli = MakeStdioSafeTLII(view.getTargetTriple());
        functionAnalysis.registerPass([&] { return llvm::TargetLibraryAnalysis(tli); });
        passBuilder.registerModuleAnalyses(moduleAnalysis);
        passBuilder.registerCGSCCAnalyses(cgsccAnalysis);
        passBuilder.registerFunctionAnalyses(functionAnalysis);
        passBuilder.registerLoopAnalyses(loopAnalysis);
        passBuilder.crossRegisterProxies(loopAnalysis, functionAnalysis, cgsccAnalysis, moduleAnalysis);
        instrumentation.registerCallbacks(instrumentationCallbacks, &moduleAnalysis);
        auto pipelineLevel = optLevel == 1 ? llvm::OptimizationLevel::O1
                                           : llvm::OptimizationLevel::O2;
        auto passes = passBuilder.buildPerModuleDefaultPipeline(pipelineLevel);
        passes.run(view, moduleAnalysis);
    }
    return true;
}

bool LLVMBackend::PrintModuleView(std::string& out, const std::string& kind,
                                  int optLevel, const std::string& functionName,
                                  bool wholeModule,
                                  std::vector<LineMapping>* mappings)
{
    llvm::TimeTraceScope printScope("PrintModuleView", kind);
    if (!module || (kind != "ir" && kind != "asm") || optLevel < 0 || optLevel > 2)
        return false;
    if (mappings) mappings->clear();
    auto view = CloneModuleForView(kind + " view");
    if (!view)
        return false;

    auto matchingFunctions = [&](llvm::Module& m) {
        return MatchingFunctions(*this, m, functionName);
    };

    struct PreOptimizationMatch
    {
        std::string name;
        std::set<std::string> callers;
        bool addressTaken = false;
    };
    std::vector<PreOptimizationMatch> preOptimizationMatches;
    for (auto* function : matchingFunctions(*view))
    {
        PreOptimizationMatch info;
        info.name = function->getName().str();
        std::vector<llvm::User*> pending;
        for (auto* user : function->users()) pending.push_back(user);
        std::set<const llvm::User*> visited;
        while (!pending.empty())
        {
            llvm::User* user = pending.back();
            pending.pop_back();
            if (!visited.insert(user).second) continue;
            if (auto* call = llvm::dyn_cast<llvm::CallBase>(user))
            {
                if (auto* caller = call->getFunction())
                    info.callers.insert(caller->getName().str());
                continue;
            }
            info.addressTaken = true;
            for (auto* nested : user->users()) pending.push_back(nested);
        }
        preOptimizationMatches.push_back(std::move(info));
    }

    auto optimizedAwayBanner = [&](const std::vector<llvm::Function*>& postMatches) {
        if (optLevel == 0 || preOptimizationMatches.empty() || !postMatches.empty())
            return std::string();

        std::string banner;
        for (const auto& info : preOptimizationMatches)
        {
            banner += "; function " + SpellFunctionSymbol(*this, info.name)
                + " was optimized away at O" + std::to_string(optLevel)
                + " (inlined or removed)\n";
            if (info.addressTaken)
                banner += "; its address was also used\n";
        }

        std::set<std::string> survivingCallers;
        for (const auto& info : preOptimizationMatches)
            for (const auto& caller : info.callers)
            {
                auto* function = view->getFunction(caller);
                if (function && !function->isDeclaration())
                    survivingCallers.insert(caller);
            }
        if (!survivingCallers.empty())
        {
            banner += "; its code was inlined into: ";
            size_t count = 0;
            for (const auto& caller : survivingCallers)
            {
                if (count++ != 0) banner += ", ";
                banner += SpellFunctionSymbol(*this, caller);
                if (count == 5) break;
            }
            banner += " - view those functions instead\n";
        }
        return banner;
    };

    std::unique_ptr<llvm::TargetMachine> targetMachine;
    if (optLevel > 0)
    {
        view = GetOrBuildOptimizedView(optLevel);
        if (!view)
            return false;
        if (kind == "asm")
        {
            targetMachine = CreateOptTargetMachine(optLevel);
            if (!targetMachine)
                return false;
            // The cached module was retargeted when built; re-applying is a no-op.
            view->setTargetTriple(targetMachine->getTargetTriple());
            view->setDataLayout(targetMachine->createDataLayout());
        }
    }
    else if (!OptimizeViewModule(*view, optLevel, kind == "asm", targetMachine))
        return false;

    const std::string rootPath = analyzedRootPath_;
    const std::string rootName = std::filesystem::path(rootPath).filename().string();
    std::string cuFileName;
    std::string cuDirectory;
    if (compileUnit && compileUnit->getFile())
    {
        cuFileName = compileUnit->getFile()->getFilename().str();
        cuDirectory = compileUnit->getFile()->getDirectory().str();
    }

    // Kept snapshot bodies reference earlier analyses' temp copies of the same
    // document; treat every recorded alias of the analyzed root as root too.
    std::vector<std::pair<std::string, std::string>> rootAliases;
    if (optLevel > 0 && optimizedViewCache_.optLevel == optLevel && optimizedViewCache_.module)
        for (const std::string& alias : optimizedViewCache_.rootPathAliases)
            rootAliases.emplace_back(alias,
                std::filesystem::path(alias).filename().string());

    auto uncachedIsRootFile = [&](const llvm::DIFile* file) {
        if (!file) return false;
        const std::string filename = file->getFilename().str();
        const std::string directory = file->getDirectory().str();
        if (!cuFileName.empty() && filename == cuFileName && directory == cuDirectory)
            return true;
        if (!rootPath.empty())
        {
            std::filesystem::path candidate = std::filesystem::path(directory) / filename;
            std::error_code ec;
            const std::string canonical =
                std::filesystem::weakly_canonical(candidate, ec).string();
            if (canonical == rootPath)
                return true;
            if (filename == rootName)
                return true;
            for (const auto& [aliasPath, aliasName] : rootAliases)
                if (canonical == aliasPath || filename == aliasName)
                    return true;
        }
        return false;
    };

    struct DebugLocationCache
    {
        RootFilePredicate uncachedIsRootFile;
        llvm::DenseMap<const llvm::DIFile*, bool> rootFiles;
        llvm::DenseMap<const llvm::DILocation*, std::vector<LLVMBackend::LineFrame>> lineStacks;
        llvm::DenseMap<const llvm::DILocation*, int> sourceLines;

        explicit DebugLocationCache(RootFilePredicate rootFile)
            : uncachedIsRootFile(std::move(rootFile)), rootFiles(0), lineStacks(0), sourceLines(0)
        {
        }

        bool IsRootFile(const llvm::DIFile* file)
        {
            auto it = rootFiles.find(file);
            if (it != rootFiles.end()) return it->second;
            const bool result = uncachedIsRootFile(file);
            rootFiles.try_emplace(file, result);
            return result;
        }

        const std::vector<LLVMBackend::LineFrame>& LineStack(const llvm::DILocation* location)
        {
            auto [it, inserted] = lineStacks.try_emplace(location);
            if (inserted)
                it->second = BuildLineStack(
                    location, [this](const llvm::DIFile* file) { return IsRootFile(file); });
            return it->second;
        }

        int SourceLine(const llvm::DILocation* location)
        {
            auto it = sourceLines.find(location);
            if (it != sourceLines.end()) return it->second;
            int result = 0;
            for (const auto& frame : LineStack(location))
                if (frame.root)
                {
                    result = frame.line;
                    break;
                }
            sourceLines.try_emplace(location, result);
            return result;
        }
    } debugLocationCache{RootFilePredicate(uncachedIsRootFile)};

    auto makeLineMapping = [&](const llvm::DILocation* location) {
        auto mapping = MakeLineMapping(debugLocationCache.LineStack(location));
        mapping.srcLine = debugLocationCache.SourceLine(location);
        return mapping;
    };

    const size_t definedFunctionCount = static_cast<size_t>(std::count_if(
        view->begin(), view->end(), [](const llvm::Function& function) {
            return !function.isDeclaration();
        }));
    std::vector<llvm::Function*> rootFunctions;
    if (functionName.empty() && !wholeModule)
        for (auto& function : *view)
        {
            if (function.isDeclaration()) continue;
            auto* subprogram = function.getSubprogram();
            if (subprogram && debugLocationCache.IsRootFile(subprogram->getFile()))
                rootFunctions.push_back(&function);
        }
    const bool rootScopedView = functionName.empty() && !wholeModule && !rootFunctions.empty();
    // The analyzed root is an LSP temp copy; prefer the real document name for the banner.
    const std::string bannerName = sourceDisplayName_.empty() ? rootName : sourceDisplayName_;
    const std::string rootViewBanner = rootScopedView
        ? "; showing " + std::to_string(rootFunctions.size()) + " of "
            + std::to_string(definedFunctionCount) + " defined functions from " + bannerName
            + "; imports hidden (request wholeModule for all)\n"
        : std::string();

    if (kind == "ir")
    {
        out.clear();
        auto matches = functionName.empty() ? std::vector<llvm::Function*>{} : matchingFunctions(*view);
        if (!functionName.empty() && matches.empty())
        {
            out = optimizedAwayBanner(matches);
            if (out.empty())
                out = "; no matching function: " + functionName + "\n";
            return true;
        }

        if (mappings)
        {
            llvm::TimeTraceScope mappingScope("ViewIrMappingPrint");
            for (auto& function : *view)
                for (auto& block : function)
                    for (auto it = block.begin(); it != block.end(); )
                    {
                        auto current = it++;
                        current->dropDbgRecords();
                    }
            std::string mappingText;
            llvm::raw_string_ostream mappingStream(mappingText);
            LineMappingAnnotationWriter writer(*mappings, makeLineMapping);
            // !dbg attachments and the metadata tail are the only mapping/display line differences.
            // Instruction and define lines stay aligned until the unmapped metadata tail.
            if (rootScopedView)
            {
                mappingStream << rootViewBanner;
                for (auto* function : rootFunctions)
                {
                    mappingStream << "; function: "
                                  << SpellFunctionSymbol(*this, function->getName().str()) << "\n";
                    mappingStream.flush();
                    writer.SetBaseLine((int)std::count(mappingText.begin(), mappingText.end(), '\n'));
                    function->print(mappingStream, &writer);
                    mappingStream << "\n";
                }
            }
            else if (functionName.empty())
                view->print(mappingStream, &writer);
            else
                for (auto* function : matchingFunctions(*view))
                {
                    mappingStream << "; function: "
                                  << SpellFunctionSymbol(*this, function->getName().str()) << "\n";
                    mappingStream.flush();
                    writer.SetBaseLine((int)std::count(mappingText.begin(), mappingText.end(), '\n'));
                    function->print(mappingStream, &writer);
                    mappingStream << "\n";
                }
            mappingStream.flush();
            llvm::StripDebugInfo(*view);
        }

        {
            llvm::TimeTraceScope displayScope("ViewIrDisplayPrint");
            llvm::raw_string_ostream stream(out);
            if (rootScopedView)
            {
                stream << rootViewBanner;
                for (auto* function : rootFunctions)
                {
                    stream << "; function: "
                           << SpellFunctionSymbol(*this, function->getName().str()) << "\n";
                    function->print(stream, nullptr);
                    stream << "\n";
                }
            }
            else if (functionName.empty())
                view->print(stream, nullptr);
            else
                for (auto* function : matches)
            {
                stream << "; function: "
                       << SpellFunctionSymbol(*this, function->getName().str()) << "\n";
                function->print(stream, nullptr);
                stream << "\n";
            }
            stream.flush();
        }
        if (mappings) ConsolidateLineMappings(*mappings);
        return true;
    }

    if (rootScopedView)
        for (auto& function : *view)
        {
            if (function.isDeclaration()) continue;
            auto* subprogram = function.getSubprogram();
            if (subprogram && debugLocationCache.IsRootFile(subprogram->getFile())) continue;
            function.setComdat(nullptr);
            function.setLinkage(llvm::GlobalValue::ExternalLinkage);
            function.deleteBody();
        }

    std::string assembly;
    {
        llvm::TimeTraceScope codegenScope("ViewAsmCodegen");
        llvm::legacy::PassManager pass;
        llvm::SmallString<0> buffer;
        llvm::raw_svector_ostream stream(buffer);
        if (targetMachine->addPassesToEmitFile(pass, stream, nullptr,
                                               llvm::CodeGenFileType::AssemblyFile))
            return false;
        pass.run(*view);
        assembly.assign(buffer.data(), buffer.size());
    }

    llvm::TimeTraceScope mappingScope("ViewAsmMappings");
    const std::map<int, std::string> asmFiles = BuildAsmFileTable(assembly);

    auto isRootAsmFile = [&](const std::string& path) {
        if (path.empty() || rootName.empty()) return false;
        if (!rootPath.empty() && NormalizeFilePath(path) == NormalizeFilePath(rootPath))
            return true;
        return std::filesystem::path(path).filename().string() == rootName;
    };

    struct DebugLocationKey
    {
        std::string function;
        std::string file;
        int line = 0;
        int column = 0;

        bool operator<(const DebugLocationKey& other) const
        {
            return std::tie(function, file, line, column)
                < std::tie(other.function, other.file, other.line, other.column);
        }
    };
    std::map<DebugLocationKey, std::vector<std::vector<LineFrame>>> inlineStacks;
    std::map<std::string, std::string> functionDisplayNames;
    for (auto& function : *view)
    {
        if (function.isDeclaration()) continue;
        std::string displayName = function.getName().str();
        if (auto* subprogram = function.getSubprogram())
            if (!subprogram->getName().empty())
                displayName = subprogram->getName().str();
        functionDisplayNames[function.getName().str()] = std::move(displayName);
        for (auto& block : function)
            for (auto& instruction : block)
            {
                llvm::DebugLoc debugLoc = instruction.getDebugLoc();
                if (!debugLoc || debugLoc->getLine() == 0) continue;
                const auto& stack = debugLocationCache.LineStack(debugLoc.get());
                if (stack.empty()) continue;
                DebugLocationKey key{function.getName().str(),
                                     NormalizeFilePath(DebugFilePath(debugLoc->getFile())),
                                     static_cast<int>(debugLoc->getLine()),
                                     static_cast<int>(debugLoc->getColumn())};
                auto& candidates = inlineStacks[key];
                if (std::none_of(candidates.begin(), candidates.end(),
                                 [&](const auto& candidate) { return SameLineStack(candidate, stack); }))
                    candidates.push_back(stack);
            }
    }

    auto resolveAsmStack = [&](const std::string& function, const std::string& file,
                               int line, int column,
                               const std::vector<LineFrame>& fallback) {
        DebugLocationKey key{function, NormalizeFilePath(file), line, column};
        auto it = inlineStacks.find(key);
        if (it == inlineStacks.end() || it->second.empty()) return fallback;
        const auto& candidates = it->second;
        if (candidates.size() == 1) return candidates.front();
        for (const auto& candidate : candidates)
            if (candidate.empty() || !SameLineFrame(candidate.front(), candidates.front().front()))
                return fallback;

        size_t commonSuffix = 0;
        while (commonSuffix < candidates.front().size())
        {
            const auto& frame = candidates.front()[candidates.front().size() - commonSuffix - 1];
            bool same = true;
            for (const auto& candidate : candidates)
                if (candidate.size() <= commonSuffix
                    || !SameLineFrame(candidate[candidate.size() - commonSuffix - 1], frame))
                {
                    same = false;
                    break;
                }
            if (!same) break;
            ++commonSuffix;
        }
        std::vector<LineFrame> result{candidates.front().front()};
        if (commonSuffix > 1)
            result.insert(result.end(), candidates.front().end() - commonSuffix + 1,
                          candidates.front().end());
        return result;
    };

    auto appendAsmMappings = [&](const std::string& text) {
        if (!mappings || asmFiles.empty()) return;
        size_t lineStart = 0;
        int lineNumber = 0;
        int activeStart = 0;
        std::optional<LineMapping> activeMapping;
        std::string currentFunction;
        auto finish = [&](int endLine) {
            if (activeMapping && activeStart <= endLine)
            {
                activeMapping->viewStart = activeStart;
                activeMapping->viewEnd = endLine;
                mappings->push_back(std::move(*activeMapping));
            }
            activeMapping.reset();
            activeStart = 0;
        };
        while (lineStart < text.size())
        {
            size_t lineEnd = text.find('\n', lineStart);
            if (lineEnd == std::string::npos) lineEnd = text.size();
            ++lineNumber;
            std::string line = text.substr(lineStart, lineEnd - lineStart);
            size_t first = 0;
            while (first < line.size() && std::isspace((unsigned char)line[first])) ++first;
            if (first == 0 && !line.empty() && line.ends_with(":"))
            {
                std::string label = line.substr(0, line.size() - 1);
                if (functionDisplayNames.contains(label))
                    currentFunction = label;
                else if (label.starts_with("_")
                         && functionDisplayNames.contains(label.substr(1)))
                    currentFunction = label.substr(1);
            }
            int id = 0;
            int source = 0;
            int column = 0;
            if (ParseLocDirective(line, id, source, column))
            {
                finish(lineNumber - 1);
                auto fileIt = asmFiles.find(id);
                if (fileIt != asmFiles.end() && source > 0)
                {
                    std::string displayName;
                    auto functionIt = functionDisplayNames.find(currentFunction);
                    if (functionIt != functionDisplayNames.end())
                        displayName = functionIt->second;
                    LineFrame fallback{std::filesystem::path(fileIt->second).filename().string(),
                                        source, std::move(displayName),
                                        isRootAsmFile(fileIt->second)};
                    auto stack = resolveAsmStack(currentFunction, fileIt->second,
                                                 source, column, {fallback});
                    auto mapping = MakeLineMapping(stack);
                    if (mapping.srcLine > 0)
                        activeMapping = std::move(mapping);
                    activeStart = lineNumber + 1;
                }
            }
            else if (first < line.size() && line.back() == ':')
                finish(lineNumber - 1);
            lineStart = lineEnd == text.size() ? text.size() : lineEnd + 1;
        }
        finish(lineNumber);
        ConsolidateLineMappings(*mappings);
    };

    if (functionName.empty())
    {
        if (rootScopedView)
            out = rootViewBanner + assembly;
        else
            out = std::move(assembly);
        appendAsmMappings(out);
        return true;
    }

    auto matches = matchingFunctions(*view);
    if (matches.empty())
    {
        out = optimizedAwayBanner(matches);
        if (out.empty())
            out = "; no matching function: " + functionName + "\n";
        return true;
    }

    const bool darwin = targetMachine->getTargetTriple().isOSDarwin();
    std::set<std::string> allLabels;
    std::set<std::string> wantedLabels;
    for (auto& function : *view)
    {
        if (function.isDeclaration()) continue;
        std::string label = (darwin ? "_" : "") + function.getName().str();
        allLabels.insert(label);
        if (std::find(matches.begin(), matches.end(), &function) != matches.end())
            wantedLabels.insert(label);
    }

    struct LabelPos { size_t offset; std::string label; };
    std::vector<LabelPos> labels;
    size_t lineStart = 0;
    while (lineStart < assembly.size())
    {
        size_t lineEnd = assembly.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = assembly.size();
        size_t first = lineStart;
        while (first < lineEnd && (assembly[first] == ' ' || assembly[first] == '\t')) ++first;
        std::string label = assembly.substr(first, lineEnd - first);
        if (!label.empty() && label.back() == ':')
        {
            label.pop_back();
            if (allLabels.contains(label)) labels.push_back({lineStart, std::move(label)});
        }
        lineStart = lineEnd == assembly.size() ? assembly.size() : lineEnd + 1;
    }

    out.clear();
    for (size_t i = 0; i < labels.size(); ++i)
    {
        if (!wantedLabels.contains(labels[i].label)) continue;
        size_t end = i + 1 < labels.size() ? labels[i + 1].offset : assembly.size();
        out.append(assembly, labels[i].offset, end - labels[i].offset);
        if (end == assembly.size() && !out.empty() && out.back() != '\n') out += '\n';
    }
    if (out.empty())
    {
        out = optimizedAwayBanner(matches);
        if (out.empty())
            out = "; no matching function label: " + functionName + "\n";
    }
    appendAsmMappings(out);
    return true;
}

namespace {

// Passes whose remarks are worth an IDE annotation. Deliberately an allowlist: enabling
// everything (instcombine especially) buries the actionable remarks in noise.
bool IsReportedRemarkPass(llvm::StringRef passName)
{
    return passName == "inline" || passName == "loop-vectorize"
        || passName == "slp-vectorizer" || passName == "loop-unroll"
        || passName == "gvn" || passName == "licm" || passName == "sroa"
        || passName == "prologepilog" || passName == "regalloc";
}

// Upper bound on collected remarks, applied PER PASS and counted AFTER the root-file
// filter. Per pass, not overall, because the passes are wildly unequal talkers: a single
// large source emits ~12000 gvn remarks against ~2600 inline ones, so one shared budget is
// spent by gvn long before the inline remarks that the IDE actually reads are collected.
// That was a real defect - past the cut-off every function still reported an inlinedInto
// count with no remarks behind it, and the editor offered "inlined into 2 sites" with no
// sites to navigate to. The caller is told when this clamps.
constexpr size_t kMaxCollectedRemarksPerPass = 5000;

// Collects Tier 3 remarks, and doubles as the Tier 1 source for exact frame size and
// spill counts (prologepilog / regalloc) - scraping the prologue text would be guesswork.
struct RemarkCollector : public llvm::DiagnosticHandler
{
    std::map<std::string, LLVMBackend::FrameRemark>& frames;
    std::vector<LLVMBackend::OptRemark>* remarks;   // null when only frames are wanted
    std::function<bool(const llvm::DiagnosticLocation&)> isRootLocation;
    bool& truncated;   // a reference: the context destroys this handler on restore
    std::map<std::string, size_t> collectedPerPass;

    RemarkCollector(std::map<std::string, LLVMBackend::FrameRemark>& frameOut,
                    std::vector<LLVMBackend::OptRemark>* remarkOut,
                    std::function<bool(const llvm::DiagnosticLocation&)> rootFilter,
                    bool& truncatedOut)
        : frames(frameOut), remarks(remarkOut), isRootLocation(std::move(rootFilter)),
          truncated(truncatedOut) {}

    // The remark emitters skip building a remark at all unless some category is enabled.
    bool isAnyRemarkEnabled() const override { return true; }
    bool isAnalysisRemarkEnabled(llvm::StringRef passName) const override { return IsReportedRemarkPass(passName); }
    bool isMissedOptRemarkEnabled(llvm::StringRef passName) const override { return IsReportedRemarkPass(passName); }
    bool isPassedOptRemarkEnabled(llvm::StringRef passName) const override { return IsReportedRemarkPass(passName); }

    static const char* KindOf(const llvm::DiagnosticInfo& di)
    {
        switch (di.getKind())
        {
        case llvm::DK_OptimizationRemark:
        case llvm::DK_MachineOptimizationRemark:
            return "passed";
        case llvm::DK_OptimizationRemarkMissed:
        case llvm::DK_MachineOptimizationRemarkMissed:
            return "missed";
        default:
            return "analysis";
        }
    }

    bool handleDiagnostics(const llvm::DiagnosticInfo& di) override
    {
        const auto* opt = llvm::dyn_cast<llvm::DiagnosticInfoOptimizationBase>(&di);
        if (!opt) return false;

        const llvm::StringRef remarkName = opt->getRemarkName();
        if (remarkName == "StackSize" || remarkName == "SpillReloadCopies")
        {
            LLVMBackend::FrameRemark& entry = frames[opt->getFunction().getName().str()];
            for (const auto& arg : opt->getArgs())
            {
                int value = 0;
                if (llvm::StringRef(arg.Val).getAsInteger(10, value)) continue;
                if (arg.Key == "NumStackBytes") entry.stackBytes = value;
                else if (arg.Key == "NumSpills") entry.spills = value;
                else if (arg.Key == "NumReloads") entry.reloads = value;
            }
            return true;
        }

        if (!remarks) return true;
        // Some emitters call the non-filtering emit() overload, so the isEnabled hooks
        // above are not sufficient on their own - re-check the allowlist here.
        if (!IsReportedRemarkPass(opt->getPassName())) return true;

        const llvm::DiagnosticLocation location = opt->getLocation();
        if (!location.isValid() || !isRootLocation(location))
            return true;   // a remark about the core libraries is not the user's business
        size_t& collected = collectedPerPass[opt->getPassName().str()];
        if (collected >= kMaxCollectedRemarksPerPass)
        {
            truncated = true;
            return true;
        }
        ++collected;

        LLVMBackend::OptRemark entry;
        entry.pass = opt->getPassName().str();
        entry.name = remarkName.str();
        entry.kind = KindOf(di);
        entry.message = opt->getMsg();
        entry.function = opt->getFunction().getName().str();
        entry.file = std::filesystem::path(location.getAbsolutePath()).filename().string();
        entry.srcLine = static_cast<int>(location.getLine());
        entry.srcColumn = static_cast<int>(location.getColumn());
        for (const auto& arg : opt->getArgs())
            if (arg.Key != "String") entry.args.push_back({arg.Key, arg.Val});
        remarks->push_back(std::move(entry));
        return true;  // consumed; do not let the default handler print it
    }
};

// Tier 2: classify a surviving call as a cflat-level cost and merge it into totals keyed
// by (line, kind, detail). Only costs the optimizer did NOT remove are recorded.
void RecordOptCost(const llvm::Instruction& instruction, int rootLine,
                   std::map<std::tuple<int, std::string, std::string>, LLVMBackend::OptCost>& totals)
{
    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
    if (!call || call->isInlineAsm()) return;

    std::string kind;
    std::string detail;
    int bytes = 0;

    const llvm::Function* callee = call->getCalledFunction();
    if (!callee)
    {
        // An unresolved indirect call is a virtual/interface dispatch the optimizer could
        // not devirtualize, or a call through a function pointer.
        if (!call->isIndirectCall()) return;
        kind = "indirect-call";
    }
    else
    {
        const std::string name = callee->getName().str();
        auto starts = [&](const char* prefix) { return name.rfind(prefix, 0) == 0; };
        if (starts("llvm.memcpy") || starts("llvm.memmove") || starts("llvm.memset"))
        {
            kind = "copy";
            detail = starts("llvm.memset") ? "memset"
                   : starts("llvm.memmove") ? "memmove" : "memcpy";
            if (call->arg_size() >= 3)
                if (auto* size = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(2)))
                    bytes = static_cast<int>(size->getZExtValue());
        }
        else if (name == "malloc" || name == "calloc" || name == "realloc")
        {
            kind = "alloc";
            detail = name;
        }
        else if (name == "free")
        {
            kind = "free";
            detail = name;
        }
        else if (name.find(".dtor") != std::string::npos)
        {
            kind = "destructor";
            detail = name.substr(0, name.find(".dtor"));
        }
        else
        {
            return;
        }
    }

    auto key = std::make_tuple(rootLine, kind, detail);
    auto it = totals.find(key);
    if (it == totals.end())
    {
        LLVMBackend::OptCost cost;
        cost.kind = kind;
        cost.detail = detail;
        cost.srcLine = rootLine;
        cost.bytes = bytes;
        cost.count = 1;
        totals.emplace(std::move(key), std::move(cost));
        return;
    }
    it->second.count++;
    // Merged entries share a line and kind; report the largest transfer seen.
    if (bytes > it->second.bytes) it->second.bytes = bytes;
}

// Real instructions in an assembly body: not blank, not a comment, not a directive,
// not a bare label.
bool IsAsmInstructionLine(const std::string& line)
{
    size_t pos = line.find_first_not_of(" \t");
    if (pos == std::string::npos) return false;
    const char first = line[pos];
    if (first == '.' || first == '#' || first == ';') return false;
    if (first == '/' && pos + 1 < line.size() && line[pos + 1] == '/') return false;
    std::string trimmed = line.substr(pos);
    trimmed.erase(trimmed.find_last_not_of(" \t\r") + 1);
    if (trimmed.empty()) return false;
    // A label is a single token ending in ':' with nothing after it.
    if (trimmed.back() == ':' && trimmed.find_first_of(" \t") == std::string::npos)
        return false;
    return true;
}

// The symbol a label line defines, or empty when the line is not a label.
std::string AsmLabelName(const std::string& line)
{
    if (line.empty() || line[0] == ' ' || line[0] == '\t') return {};
    size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0) return {};
    std::string name = line.substr(0, colon);
    if (name.find_first_of(" \t") != std::string::npos) return {};
    return name;
}

bool ViewTypesCompatible(const llvm::Type* first, const llvm::Type* second,
                          std::set<std::pair<const llvm::Type*, const llvm::Type*>>& seen)
{
    if (first == second)
        return true;
    auto pair = std::make_pair(first, second);
    if (!seen.insert(pair).second)
        return true;
    if (first->getTypeID() != second->getTypeID())
        return false;
    if (auto* firstStruct = llvm::dyn_cast<llvm::StructType>(first))
    {
        auto* secondStruct = llvm::cast<llvm::StructType>(second);
        if (firstStruct->isOpaque() || secondStruct->isOpaque())
            return firstStruct->isOpaque() && secondStruct->isOpaque();
        if (firstStruct->isPacked() != secondStruct->isPacked()
            || firstStruct->getNumElements() != secondStruct->getNumElements())
            return false;
        for (unsigned i = 0; i < firstStruct->getNumElements(); ++i)
            if (!ViewTypesCompatible(firstStruct->getElementType(i),
                                     secondStruct->getElementType(i), seen))
                return false;
        return true;
    }
    if (auto* firstPointer = llvm::dyn_cast<llvm::PointerType>(first))
        return firstPointer->getAddressSpace()
            == llvm::cast<llvm::PointerType>(second)->getAddressSpace();
    if (auto* firstArray = llvm::dyn_cast<llvm::ArrayType>(first))
        return firstArray->getNumElements()
            == llvm::cast<llvm::ArrayType>(second)->getNumElements()
            && ViewTypesCompatible(firstArray->getElementType(),
                                   llvm::cast<llvm::ArrayType>(second)->getElementType(),
                                   seen);
    if (auto* firstVector = llvm::dyn_cast<llvm::VectorType>(first))
    {
        auto* secondVector = llvm::cast<llvm::VectorType>(second);
        return firstVector->getElementCount() == secondVector->getElementCount()
            && ViewTypesCompatible(firstVector->getElementType(),
                                   secondVector->getElementType(), seen);
    }
    if (auto* firstFunction = llvm::dyn_cast<llvm::FunctionType>(first))
    {
        auto* secondFunction = llvm::cast<llvm::FunctionType>(second);
        if (firstFunction->isVarArg() != secondFunction->isVarArg()
            || firstFunction->getNumParams() != secondFunction->getNumParams()
            || !ViewTypesCompatible(firstFunction->getReturnType(),
                                     secondFunction->getReturnType(), seen))
            return false;
        for (unsigned i = 0; i < firstFunction->getNumParams(); ++i)
            if (!ViewTypesCompatible(firstFunction->getParamType(i),
                                     secondFunction->getParamType(i), seen))
                return false;
    }
    return true;
}

bool ViewTypesCompatible(const llvm::Type* first, const llvm::Type* second)
{
    std::set<std::pair<const llvm::Type*, const llvm::Type*>> seen;
    return ViewTypesCompatible(first, second, seen);
}

}  // namespace

std::unique_ptr<llvm::Module> LLVMBackend::GetOrBuildOptimizedView(int optLevel)
{
    optimizedViewWasIncremental_ = false;
    const bool viewTraceEnabled = viewTraceEnabled_ || std::getenv("CFLAT_VIEW_INC_TRACE");
    if (optLevel <= 0 || !context)
        return nullptr;

    int calleeDepth = 2;
    std::string calleeDepthText = "2";
    if (const char* depthEnv = std::getenv("CFLAT_VIEW_INC_DEPTH"))
    {
        const std::string value(depthEnv);
        if (!value.empty())
        {
            if (value == "full" || value == "-1")
            {
                calleeDepth = -1;
                calleeDepthText = "full";
            }
            else
            {
                const char* begin = value.data();
                const char* end = begin + value.size();
                while (begin != end && std::isspace((unsigned char)*begin))
                    ++begin;
                if (begin != end && *begin == '+')
                    ++begin;
                int parsed = 0;
                const auto [consumed, error] = std::from_chars(begin, end, parsed);
                if (error == std::errc{} && consumed == end && parsed >= 0)
                {
                    calleeDepth = parsed;
                    calleeDepthText = std::to_string(parsed);
                }
            }
        }
    }
    if (optimizedViewCache_.optLevel == optLevel && optimizedViewCache_.module)
    {
        optimizedViewWasIncremental_ = true;
        if (viewTraceEnabled)
            std::cerr << "[view-incremental] hit reopt=0/"
                      << optimizedViewCache_.funcHashes.size() << " seeds=0 depth="
                      << calleeDepthText << "\n";
        return llvm::CloneModule(*optimizedViewCache_.module);
    }

    MaterializeCoreIfLazy();
    if (!module || !module->isMaterialized())
        return nullptr;

    auto hashGlobal = [](const llvm::GlobalVariable& global) {
        std::string text = std::to_string(static_cast<unsigned>(global.getLinkage()));
        std::string type;
        llvm::raw_string_ostream typeOut(type);
        global.getValueType()->print(typeOut);
        text += "|" + type;
        if (global.hasInitializer())
        {
            std::string initializer;
            llvm::raw_string_ostream initializerOut(initializer);
            global.getInitializer()->print(initializerOut);
            text += "|" + initializer;
        }
        return static_cast<uint64_t>(std::hash<std::string>{}(text));
    };
    OptimizedViewCache metadata;
    metadata.preOptMetadataRecorded = true;
    for (const llvm::Function& function : *module)
    {
        if (function.isDeclaration())
            continue;
        const std::string name = function.getName().str();
        metadata.funcHashes[name] = static_cast<uint64_t>(
            llvm::StructuralHash(function, /*DetailedHash=*/true));
        if (function.hasAddressTaken())
            metadata.addressTaken.insert(name);

        auto& callees = metadata.callees[name];
        for (const llvm::BasicBlock& block : function)
            for (const llvm::Instruction& instruction : block)
                if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction))
                {
                    if (const llvm::Function* callee = call->getCalledFunction())
                        if (!callee->isDeclaration())
                            callees.insert(callee->getName().str());
                }
    }
    for (const llvm::GlobalVariable& global : module->globals())
        metadata.globalHashes[global.getName().str()] = hashGlobal(global);
    const size_t definedFunctionCount = metadata.funcHashes.size();
    const std::string rootFile = sourceDisplayName_.empty() ? analyzedRootPath_ : sourceDisplayName_;
    auto makeCache = [&] {
        OptimizedViewCache next;
        next.optLevel = optLevel;
        next.preOptMetadataRecorded = true;
        next.funcHashes = metadata.funcHashes;
        next.globalHashes = metadata.globalHashes;
        next.callees = metadata.callees;
        next.addressTaken = metadata.addressTaken;
        return next;
    };

    auto runFullBuild = [&](const std::string& reason) -> std::unique_ptr<llvm::Module> {
        if (viewTraceEnabled)
            std::cerr << "[view-incremental] miss reason=" << reason << "\n";
        OptimizedViewCache next = makeCache();
        auto view = CloneModuleForView("optimized view");
        if (!view)
            return nullptr;

        {
            auto previousHandler = context->getDiagnosticHandler();
            context->setDiagnosticHandler(
                std::make_unique<RemarkCollector>(
                    next.frameRemarks, &next.remarks,
                    [](const llvm::DiagnosticLocation& location) { return location.isValid(); },
                    next.remarksTruncated),
                /*RespectFilters=*/false);
            struct HandlerRestore
            {
                llvm::LLVMContext* context;
                std::unique_ptr<llvm::DiagnosticHandler> previous;
                ~HandlerRestore() { context->setDiagnosticHandler(std::move(previous), false); }
            } restore{context.get(), std::move(previousHandler)};

            std::unique_ptr<llvm::TargetMachine> targetMachine;
            llvm::TimeTraceScope pipelineScope("OptViewCachePipeline",
                                               "O" + std::to_string(optLevel));
            if (!OptimizeViewModule(*view, optLevel, /*needTargetMachine=*/true, targetMachine))
                return nullptr;
        }

        next.rootPathAliases = {analyzedRootPath_};
        next.module = std::move(view);
        optimizedViewCache_ = std::move(next);
        return llvm::CloneModule(*optimizedViewCache_.module);
    };
    std::string initialMissReason;
    if (!incrementalViewSnapshot_)
        initialMissReason = "no-snapshot";
    if (initialMissReason.empty() && incrementalViewSnapshot_->optLevel != optLevel)
        initialMissReason = "opt-level";
    if (initialMissReason.empty() && incrementalViewSnapshot_->rootFile != rootFile)
        initialMissReason = "root-file";
    if (initialMissReason.empty() && std::getenv("CFLAT_VIEW_NO_INCREMENTAL"))
        initialMissReason = "env-disabled";
    if (!initialMissReason.empty())
        return runFullBuild(initialMissReason);

    const IncrementalViewSnapshot& snapshot = *incrementalViewSnapshot_;
    std::set<std::string> changedFuncs;
    std::set<std::string> removedFuncs;
    for (const auto& [name, hash] : metadata.funcHashes)
    {
        auto old = snapshot.funcHashes.find(name);
        if (old == snapshot.funcHashes.end() || old->second != hash)
            changedFuncs.insert(name);
    }
    for (const auto& [name, hash] : snapshot.funcHashes)
        if (!metadata.funcHashes.contains(name))
            removedFuncs.insert(name);
    std::set<std::string> changedGlobals;
    for (const auto& [name, hash] : metadata.globalHashes)
    {
        auto old = snapshot.globalHashes.find(name);
        if (old == snapshot.globalHashes.end() || old->second != hash)
            changedGlobals.insert(name);
    }
    std::set<std::string> seeds = changedFuncs;
    for (const std::string& name : changedGlobals)
    {
        llvm::GlobalVariable* global = module->getNamedGlobal(name);
        if (!global)
            continue;
        std::vector<llvm::User*> pending;
        for (llvm::User* user : global->users())
            pending.push_back(user);
        std::set<const llvm::User*> visited;
        while (!pending.empty())
        {
            llvm::User* user = pending.back();
            pending.pop_back();
            if (!visited.insert(user).second)
                continue;
            if (auto* instruction = llvm::dyn_cast<llvm::Instruction>(user))
            {
                if (llvm::Function* function = instruction->getFunction())
                    if (!function->isDeclaration())
                        seeds.insert(function->getName().str());
                continue;
            }
            for (llvm::User* nested : user->users())
                pending.push_back(nested);
        }
    }
    for (const std::string& removed : removedFuncs)
        for (const auto& [caller, callees] : snapshot.callees)
            if (callees.contains(removed))
                seeds.insert(caller);
    auto addCallers = [&](const std::set<std::string>& initial) {
        std::set<std::string> result = initial;
        std::vector<std::string> pending(result.begin(), result.end());
        while (!pending.empty())
        {
            const std::string target = std::move(pending.back());
            pending.pop_back();
            auto addFrom = [&](const auto& graph) {
                for (const auto& [caller, callees] : graph)
                    if (callees.contains(target) && result.insert(caller).second)
                        pending.push_back(caller);
            };
            addFrom(snapshot.callees);
            addFrom(metadata.callees);
        }
        return result;
    };
    // An address-taken SEED can have callers invisible to the name-keyed graph
    // (vtable/function-pointer dispatch, possibly devirtualized and inlined by
    // the old pipeline), so fall back. Callers/callees merely entering reopt via
    // explicit graph edges are safe to re-optimize regardless of address-taken.
    for (const std::string& seed : seeds)
        if (snapshot.addressTaken.contains(seed) || metadata.addressTaken.contains(seed))
            return runFullBuild("addr-taken");
    const std::set<std::string> callerClosure = addCallers(seeds);
    std::set<std::string> reopt = callerClosure;
    std::vector<std::pair<std::string, size_t>> calleePending;
    calleePending.reserve(reopt.size());
    for (const std::string& name : reopt)
        calleePending.emplace_back(name, 0);
    for (size_t pendingIndex = 0; pendingIndex < calleePending.size(); ++pendingIndex)
    {
        const auto& [caller, depth] = calleePending[pendingIndex];
        if (calleeDepth >= 0 && depth >= static_cast<size_t>(calleeDepth))
            continue;
        auto it = metadata.callees.find(caller);
        if (it == metadata.callees.end())
            continue;
        for (const std::string& callee : it->second)
            if (reopt.insert(callee).second)
                calleePending.emplace_back(callee, depth + 1);
    }
    if (reopt.size() * 2 > definedFunctionCount)
    {
        return runFullBuild("too-wide");
    }
    std::unique_ptr<llvm::Module> snapshotModule;
    {
        llvm::TimeTraceScope parseScope("ViewSnapshotParse");
        auto bitcodeBuffer = llvm::MemoryBuffer::getMemBuffer(snapshot.bitcode,
                                                              "incremental-view", false);
        auto parsed = llvm::parseBitcodeFile(bitcodeBuffer->getMemBufferRef(), *context);
        if (!parsed)
        {
            llvm::consumeError(parsed.takeError());
            return runFullBuild("transplant-conflict");
        }
        snapshotModule = std::move(*parsed);
    }
    if (reopt.empty() && changedGlobals.empty() && removedFuncs.empty())
    {
        snapshotModule->setModuleIdentifier(module->getModuleIdentifier());
        snapshotModule->setSourceFileName(module->getSourceFileName());
        OptimizedViewCache next = makeCache();
        next.rootPathAliases = snapshot.rootPathAliases;
        next.rootPathAliases.insert(analyzedRootPath_);
        next.frameRemarks = snapshot.frameRemarks;
        next.remarks = snapshot.remarks;
        next.remarksTruncated = snapshot.remarksTruncated;
        next.module = std::move(snapshotModule);
        optimizedViewCache_ = std::move(next);
        optimizedViewWasIncremental_ = true;
        if (viewTraceEnabled)
            std::cerr << "[view-incremental] hit reopt=0/" << definedFunctionCount
                      << " seeds=0 depth=" << calleeDepthText << "\n";
        return llvm::CloneModule(*optimizedViewCache_.module);
    }
    std::unique_ptr<llvm::Module> stomp = llvm::CloneModule(*snapshotModule);
    snapshotModule->setModuleIdentifier(module->getModuleIdentifier());
    snapshotModule->setSourceFileName(module->getSourceFileName());
    // Link direction: dest is a full clone of the NEW analyzed module; src is the
    // old optimized snapshot reduced to the bodies we keep. OverrideFromSrc then
    // swaps the old optimized body over the new pre-opt one for every function we
    // are not re-optimizing, while reopt/added functions keep their fresh bodies
    // (src holds only declarations for them). Changed globals are erased from src
    // so the new content under the original name wins; unchanged globals are
    // byte-identical in both, so the override is a no-op.
    std::unique_ptr<llvm::Module> work = CloneModule(*module);
    if (!work)
        return runFullBuild("transplant-conflict");
    std::unique_ptr<llvm::Module> source = std::move(snapshotModule);
    // The snapshot went through OptimizeViewModule, which normalizes triple and
    // datalayout from the target machine; the fresh clone has not yet. Align them
    // before linking so IRMover does not see mismatched layouts.
    work->setTargetTriple(source->getTargetTriple());
    work->setDataLayout(source->getDataLayout());

    for (llvm::Function& function : *source)
        if (!function.isDeclaration() && reopt.contains(function.getName().str()))
        {
            function.deleteBody();
            // A local-linkage declaration is invalid IR; the work body wins anyway.
            function.setLinkage(llvm::GlobalValue::ExternalLinkage);
        }

    // Functions with no body in the optimized snapshot (fully inlined + DCE'd by
    // the old pipeline) cannot keep an old body. Those reachable as callees of the
    // reopt set must be freshly optimized (a reopt caller may keep a direct call);
    // the rest keep their pre-opt body protected and die in the final GlobalDCE.
    std::set<std::string> srcMissing;
    for (const llvm::Function& function : *work)
        if (!function.isDeclaration() && !reopt.contains(function.getName().str()))
        {
            const llvm::Function* sourceFunction = source->getFunction(function.getName());
            if (!sourceFunction || sourceFunction->isDeclaration())
                srcMissing.insert(function.getName().str());
        }
    std::vector<std::string> missingPending(reopt.begin(), reopt.end());
    while (!missingPending.empty())
    {
        const std::string caller = std::move(missingPending.back());
        missingPending.pop_back();
        auto calleeIt = metadata.callees.find(caller);
        if (calleeIt == metadata.callees.end())
            continue;
        for (const std::string& callee : calleeIt->second)
            if (srcMissing.contains(callee) && reopt.insert(callee).second)
                missingPending.push_back(callee);
    }
    if (reopt.size() * 2 > definedFunctionCount)
        return runFullBuild("too-wide");
    bool transplantConflict = false;
    std::string transplantReason = "transplant-conflict";
    for (const std::string& name : removedFuncs)
    {
        llvm::Function* removed = source->getFunction(name);
        if (!removed)
            continue;
        removed->deleteBody();
        if (!removed->use_empty())
        {
            transplantConflict = true;
            transplantReason = "removed-func-uses";
            break;
        }
        removed->eraseFromParent();
    }
    for (auto it = source->global_begin(); !transplantConflict && it != source->global_end();)
    {
        llvm::GlobalVariable& global = *it++;
        if (!changedGlobals.contains(global.getName().str()))
            continue;
        // A kept (old) body still reading the old content means the function hash
        // did not see the global change - do not guess, rebuild.
        if (!global.use_empty())
        {
            transplantConflict = true;
            break;
        }
        global.eraseFromParent();
    }

    std::vector<PromotedSymbol> promotedSymbols;
    auto markTransplantConflict = [&](const char* reason) {
        transplantConflict = true;
        transplantReason = reason;
    };
    if (!transplantConflict)
    {
        for (llvm::Function& workFunction : *work)
        {
            const std::string name = workFunction.getName().str();
            if (name.empty())
                continue;
            llvm::Function* sourceFunction = source->getFunction(name);
            const bool workLocal = IsLocalLinkage(workFunction);
            const bool sourceLocal = sourceFunction && IsLocalLinkage(*sourceFunction);
            if (!workLocal && !sourceLocal)
                continue;
            // No src counterpart: no collision - srcMissing functions stay
            // protected in work and die in the final GlobalDCE if unused.
            if (!sourceFunction)
                continue;
            // Linkage may legitimately differ between analyses (core bitcode cache
            // vs re-analysis); the final module must carry the NEW (work) linkage.
            if (workFunction.isDeclaration())
                continue;
            if (reopt.contains(name))
            {
                // src holds only an external declaration; promote a local work
                // definition too, or the linker renames it away from kept callers.
                if (workLocal)
                {
                    promotedSymbols.push_back({name, workFunction.getLinkage(),
                                               workFunction.getVisibility(),
                                               workFunction.getUnnamedAddr(), true});
                    workFunction.setLinkage(llvm::GlobalValue::ExternalLinkage);
                }
                continue;
            }
            if (sourceFunction->isDeclaration())
            {
                markTransplantConflict("src-decl-only");
                break;
            }
            promotedSymbols.push_back({name, workFunction.getLinkage(),
                                       workFunction.getVisibility(),
                                       workFunction.getUnnamedAddr(), true});
            workFunction.setLinkage(llvm::GlobalValue::ExternalLinkage);
            sourceFunction->setLinkage(llvm::GlobalValue::ExternalLinkage);
        }
    }
    // One-sided named locals are safe: no name collision means the linker keeps
    // or copies them under their own name (e.g. pipeline-created snapshot locals).
    if (!transplantConflict)
    {
        for (llvm::GlobalVariable& workGlobal : work->globals())
        {
            const std::string name = workGlobal.getName().str();
            if (name.empty())
                continue;
            llvm::GlobalVariable* sourceGlobal = source->getNamedGlobal(name);
            const bool workLocal = IsLocalLinkage(workGlobal);
            const bool sourceLocal = sourceGlobal && IsLocalLinkage(*sourceGlobal);
            if (!workLocal && !sourceLocal)
                continue;
            if (sourceGlobal && !changedGlobals.contains(name))
            {
                promotedSymbols.push_back({name, workGlobal.getLinkage(),
                                           workGlobal.getVisibility(),
                                           workGlobal.getUnnamedAddr(), false});
                workGlobal.setLinkage(llvm::GlobalValue::ExternalLinkage);
                sourceGlobal->setLinkage(llvm::GlobalValue::ExternalLinkage);
            }
        }
    }
    // Letting src override an unchanged global remaps the initializer's named
    // struct types to arbitrary isomorphic ones. For zero-initialized pairs the
    // content is trivially identical (the old pipeline cannot have folded
    // anything into an all-zero initializer we would lose), so reduce src to a
    // declaration and let the work definition keep its type names. Runs after
    // promotion - a local-linkage declaration would be invalid IR.
    if (!transplantConflict)
    {
        PreserveZeroInitializers(*source, *work, changedGlobals);
    }
    if (!transplantConflict)
    {
        struct LinkDiagPrinter : llvm::DiagnosticHandler
        {
            explicit LinkDiagPrinter(bool enabled) : enabled(enabled) {}

            bool handleDiagnostics(const llvm::DiagnosticInfo& info) override
            {
                std::string text;
                llvm::raw_string_ostream stream(text);
                llvm::DiagnosticPrinterRawOStream printer(stream);
                info.print(printer);
                if (enabled)
                    std::cerr << "[view-incremental-diag] " << text << "\n";
                return true;
            }

            bool enabled;
        };
        auto previousHandler = context->getDiagnosticHandler();
        context->setDiagnosticHandler(std::make_unique<LinkDiagPrinter>(viewTraceEnabled), false);
        transplantConflict = llvm::Linker::linkModules(*work, std::move(source),
                                                        llvm::Linker::OverrideFromSrc);
        if (transplantConflict)
            transplantReason = "link-failed";
        context->setDiagnosticHandler(std::move(previousHandler), false);
    }
    if (!transplantConflict)
    {
        if (!RestorePromotedLinkage(*work, promotedSymbols))
        {
            transplantConflict = true;
            transplantReason = "restore-miss";
        }
    }
    if (transplantConflict)
    {
        return runFullBuild(transplantReason);
    }

    std::set<std::string> protectedNames;
    for (llvm::Function& function : *work)
        if (!function.isDeclaration() && !reopt.contains(function.getName().str()))
        {
            function.addFnAttr(llvm::Attribute::OptimizeNone);
            function.addFnAttr(llvm::Attribute::NoInline);
            protectedNames.insert(function.getName().str());
        }

    OptimizedViewCache next = makeCache();
    next.rootPathAliases = snapshot.rootPathAliases;
    next.rootPathAliases.insert(analyzedRootPath_);
    next.remarksTruncated = snapshot.remarksTruncated;
    for (const auto& [name, frame] : snapshot.frameRemarks)
        if (!reopt.contains(name))
            next.frameRemarks.emplace(name, frame);
    for (const auto& remark : snapshot.remarks)
        if (!reopt.contains(remark.function))
            next.remarks.push_back(remark);

    {
        auto previousHandler = context->getDiagnosticHandler();
        context->setDiagnosticHandler(
            std::make_unique<RemarkCollector>(
                next.frameRemarks, &next.remarks,
                [](const llvm::DiagnosticLocation& location) { return location.isValid(); },
                next.remarksTruncated),
            /*RespectFilters=*/false);
        struct HandlerRestore
        {
            llvm::LLVMContext* context;
            std::unique_ptr<llvm::DiagnosticHandler> previous;
            ~HandlerRestore() { context->setDiagnosticHandler(std::move(previous), false); }
        } restore{context.get(), std::move(previousHandler)};

        std::unique_ptr<llvm::TargetMachine> targetMachine;
        llvm::TimeTraceScope pipelineScope("ViewIncrementalPipeline",
                                           "O" + std::to_string(optLevel));
        if (!OptimizeViewModule(*work, optLevel, /*needTargetMachine=*/true, targetMachine))
            return nullptr;
    }

    for (const std::string& name : protectedNames)
        if (llvm::Function* function = work->getFunction(name))
        {
            function->removeFnAttr(llvm::Attribute::OptimizeNone);
            function->removeFnAttr(llvm::Attribute::NoInline);
        }

    // Module-level IPO passes (IPSCCP, GlobalOpt) refine even optnone-protected
    // bodies, so kept functions would drift from the snapshot on every rebuild.
    // Stomp the snapshot bodies back over every kept function that survived the
    // pipeline: a kept function always displays exactly as the last full build
    // produced it. Reopt/added functions keep their freshly optimized bodies.
    {
        for (llvm::Function& function : *stomp)
        {
            if (function.isDeclaration())
                continue;
            const std::string name = function.getName().str();
            llvm::Function* workFunction = work->getFunction(name);
            if (!workFunction || workFunction->isDeclaration() || reopt.contains(name))
            {
                function.deleteBody();
                function.setLinkage(llvm::GlobalValue::ExternalLinkage);
            }
        }
        for (auto it = stomp->begin(); it != stomp->end();)
        {
            llvm::Function& function = *it++;
            if (function.isDeclaration() && function.use_empty() &&
                !work->getFunction(function.getName()))
                function.eraseFromParent();
        }
        for (auto it = stomp->global_begin(); it != stomp->global_end();)
        {
            llvm::GlobalVariable& global = *it++;
            if (global.hasName() && global.use_empty() &&
                (changedGlobals.contains(global.getName().str()) ||
                 !work->getNamedGlobal(global.getName())))
                global.eraseFromParent();
        }
        std::vector<PromotedSymbol> stompPromoted;
        for (llvm::Function& workFunction : *work)
        {
            const std::string name = workFunction.getName().str();
            if (name.empty() || workFunction.isDeclaration())
                continue;
            llvm::Function* stompFunction = stomp->getFunction(name);
            if (!stompFunction)
                continue;
            if (!IsLocalLinkage(workFunction) && !IsLocalLinkage(*stompFunction))
                continue;
            stompPromoted.push_back({name, workFunction.getLinkage(),
                                     workFunction.getVisibility(),
                                     workFunction.getUnnamedAddr(), true});
            workFunction.setLinkage(llvm::GlobalValue::ExternalLinkage);
            stompFunction->setLinkage(llvm::GlobalValue::ExternalLinkage);
        }
        for (llvm::GlobalVariable& workGlobal : work->globals())
        {
            const std::string name = workGlobal.getName().str();
            if (name.empty())
                continue;
            llvm::GlobalVariable* stompGlobal = stomp->getNamedGlobal(name);
            if (!stompGlobal || changedGlobals.contains(name))
                continue;
            if (!IsLocalLinkage(workGlobal) && !IsLocalLinkage(*stompGlobal))
                continue;
            stompPromoted.push_back({name, workGlobal.getLinkage(),
                                     workGlobal.getVisibility(),
                                     workGlobal.getUnnamedAddr(), false});
            workGlobal.setLinkage(llvm::GlobalValue::ExternalLinkage);
            stompGlobal->setLinkage(llvm::GlobalValue::ExternalLinkage);
        }
        PreserveZeroInitializers(*stomp, *work, changedGlobals);
        // Linking unions declaration attributes, and the snapshot's set is
        // degraded by the bitcode round-trip (intrinsic attrs re-canonicalized).
        // Work's post-pipeline inference matches the full build - keep it exact.
        std::map<std::string, llvm::AttributeList> declarationAttrs;
        for (const llvm::Function& function : *work)
            if (function.isDeclaration() && function.hasName())
                declarationAttrs.emplace(function.getName().str(),
                                         function.getAttributes());
        if (llvm::Linker::linkModules(*work, std::move(stomp),
                                      llvm::Linker::OverrideFromSrc))
            return runFullBuild("restomp-link");
        for (const auto& [name, attrs] : declarationAttrs)
            if (llvm::Function* function = work->getFunction(name))
                if (function->isDeclaration())
                    function->setAttributes(attrs);
        if (!RestorePromotedLinkage(*work, stompPromoted))
            return runFullBuild("restomp-restore");
    }

    llvm::PassBuilder dceBuilder;
    llvm::LoopAnalysisManager dceLoops;
    llvm::FunctionAnalysisManager dceFunctions;
    llvm::CGSCCAnalysisManager dceCgscc;
    llvm::ModuleAnalysisManager dceModule;
    llvm::TargetLibraryInfoImpl dceTli = MakeStdioSafeTLII(work->getTargetTriple());
    dceFunctions.registerPass([&] { return llvm::TargetLibraryAnalysis(dceTli); });
    dceBuilder.registerModuleAnalyses(dceModule);
    dceBuilder.registerCGSCCAnalyses(dceCgscc);
    dceBuilder.registerFunctionAnalyses(dceFunctions);
    dceBuilder.registerLoopAnalyses(dceLoops);
    dceBuilder.crossRegisterProxies(dceLoops, dceFunctions, dceCgscc, dceModule);

    llvm::ModulePassManager dcePasses;
    dcePasses.addPass(llvm::GlobalDCEPass());
    dcePasses.run(*work, dceModule);

    if (llvm::verifyModule(*work, &llvm::errs()))
        return runFullBuild("verify-failed");

    next.module = std::move(work);
    optimizedViewCache_ = std::move(next);
    optimizedViewWasIncremental_ = true;
    if (viewTraceEnabled)
        std::cerr << "[view-incremental] hit reopt=" << reopt.size() << "/"
                  << definedFunctionCount << " seeds=" << seeds.size()
                  << " depth=" << calleeDepthText << "\n";
    if (viewTraceEnabled && std::getenv("CFLAT_VIEW_INC_DUMP"))
        for (const std::string& name : reopt)
            std::cerr << "[view-incremental-reopt] " << name << "\n";
    return llvm::CloneModule(*optimizedViewCache_.module);
}

bool LLVMBackend::CollectOptimizationInfo(int optLevel,
                                          const std::vector<SourceFunction>& sourceFunctions,
                                          OptimizationInfo& info,
                                          bool withRemarks)
{
    llvm::TimeTraceScope collectScope("CollectOptimizationInfo");
    info = OptimizationInfo{};
    std::vector<FunctionOptInfo>& out = info.functions;
    if (!module || optLevel < 0 || optLevel > 2)
        return false;

    const std::string rootName = std::filesystem::path(analyzedRootPath_).filename().string();

    // cflat emits ONE DIFile for the whole module, so imported core-library code carries
    // the root file's name with its own line numbers. Filtering on the file therefore
    // cannot separate user code from core code; the analyzed function ranges can.
    std::vector<std::pair<int, int>> userRanges;
    for (const auto& source : sourceFunctions)
        if (source.second.first > 0 && source.second.second >= source.second.first)
            userRanges.push_back(source.second);
    auto lineInUserRange = [userRanges](int line) {
        for (const auto& range : userRanges)
            if (line >= range.first && line <= range.second) return true;
        return false;
    };
    auto isRootDiagLocation = [lineInUserRange](const llvm::DiagnosticLocation& location) {
        return location.isValid() && lineInUserRange(static_cast<int>(location.getLine()));
    };

    std::unique_ptr<llvm::Module> view;
    std::map<std::string, FrameRemark> frameRemarks;
    if (optLevel > 0)
    {
        view = GetOrBuildOptimizedView(optLevel);
        if (!view)
            return false;
        frameRemarks = optimizedViewCache_.frameRemarks;
        if (withRemarks)
        {
            info.remarks = optimizedViewCache_.remarks;
            info.remarksTruncated = optimizedViewCache_.remarksTruncated;
        }
    }
    else
    {
        view = CloneModuleForView("optimization info");
        if (!view)
            return false;
    }

    // The handler stays installed across codegen (prologepilog / regalloc), which emits
    // separately from the cached IR pipeline.
    auto previousHandler = context->getDiagnosticHandler();
    context->setDiagnosticHandler(
        std::make_unique<RemarkCollector>(frameRemarks,
                                          withRemarks ? &info.remarks : nullptr,
                                          isRootDiagLocation, info.remarksTruncated),
        /*RespectFilters=*/false);
    struct HandlerRestore
    {
        llvm::LLVMContext* context;
        std::unique_ptr<llvm::DiagnosticHandler> previous;
        ~HandlerRestore() { context->setDiagnosticHandler(std::move(previous), false); }
    } restore{context.get(), std::move(previousHandler)};

    std::unique_ptr<llvm::TargetMachine> targetMachine;
    if (optLevel == 0)
    {
        llvm::TimeTraceScope pipelineScope("OptInfoPipeline",
                                           "O" + std::to_string(optLevel));
        if (!OptimizeViewModule(*view, optLevel, /*needTargetMachine=*/true, targetMachine))
            return false;
    }
    else
    {
        targetMachine = CreateOptTargetMachine(optLevel);
        if (!targetMachine)
            return false;
        // The cached module was retargeted when built; re-applying is a no-op.
        view->setTargetTriple(targetMachine->getTargetTriple());
        view->setDataLayout(targetMachine->createDataLayout());
    }

    // Definitions of one source function in a module. Name matching alone cannot separate
    // overloads, which share a source name and differ only in mangling, so prefer the
    // definitions whose debug info places them in this file at this function's own lines.
    auto locateDefinitions = [this, &rootName](llvm::Module& searched, const std::string& name,
                                         int startLine, int endLine) {
        auto matches = MatchingFunctions(*this, searched, name);
        std::vector<llvm::Function*> located;
        for (auto* function : matches)
        {
            auto* subprogram = function->getSubprogram();
            if (!subprogram) continue;
            const int line = static_cast<int>(subprogram->getLine());
            if (line < startLine || line > endLine) continue;
            if (!rootName.empty() && DebugFileName(subprogram->getFile()) != rootName) continue;
            located.push_back(function);
        }
        if (!located.empty()) matches = std::move(located);
        return matches;
    };

    // Seed one entry per source function and index every surviving symbol back to it,
    // so generic instantiations aggregate into the function the user wrote.
    std::map<std::string, size_t> entryBySymbol;
    out.reserve(sourceFunctions.size());
    for (const auto& source : sourceFunctions)
    {
        FunctionOptInfo info;
        info.name = source.first;
        info.startLine = source.second.first;
        info.endLine = source.second.second;

        auto matches = locateDefinitions(*view, info.name, info.startLine, info.endLine);

        info.eliminated = matches.empty();
        for (auto* function : matches)
        {
            info.irInstructions += static_cast<int>(function->getInstructionCount());
            if (info.symbol.empty()) info.symbol = function->getName().str();
            entryBySymbol[function->getName().str()] = out.size();
        }
        out.push_back(std::move(info));
    }

    // The same index over the PRE-OPTIMIZATION module. A function the optimizer erased has
    // no symbol left in the view, so the view-side index cannot name it - yet that is
    // exactly the function an "inlined away" annotation is about. Kept separate from
    // entryBySymbol so the line-attribution walk below keeps using the view's own view of
    // which symbols are user code.
    std::map<std::string, size_t> preoptEntryBySymbol;
    for (size_t index = 0; index < out.size(); ++index)
    {
        FunctionOptInfo& info = out[index];
        for (auto* function : locateDefinitions(*module, info.name, info.startLine, info.endLine))
        {
            preoptEntryBySymbol.emplace(function->getName().str(), index);
            if (info.symbol.empty()) info.symbol = function->getName().str();
        }
    }

    // One walk serves two purposes: which functions absorbed an inlined body, and how
    // much IR each source line produced wherever that code ended up.
    std::map<std::string, std::set<std::string>> absorbedBy;
    std::map<int, LineOptInfo> lineTotals;
    std::map<std::tuple<int, std::string, std::string>, OptCost> costTotals;
    for (auto& function : *view)
    {
        if (function.isDeclaration()) continue;
        const std::string container = function.getName().str();
        const bool userFunction = entryBySymbol.count(container) != 0;
        for (auto& block : function)
            for (auto& instruction : block)
            {
                const llvm::DILocation* location = instruction.getDebugLoc().get();
                if (!location) continue;
                for (auto* frame = location; frame && frame->getInlinedAt();
                     frame = frame->getInlinedAt())
                {
                    auto* scope = frame->getScope();
                    auto* subprogram = scope ? scope->getSubprogram() : nullptr;
                    if (subprogram) absorbedBy[subprogram->getName().str()].insert(container);
                }

                // Line attribution only makes sense inside the user's own functions.
                if (!userFunction) continue;

                // Walk outward and take the first frame that lands in this file's code. A
                // same-file callee keeps its own line; a core-library callee inlined here
                // has no line of its own to claim, so it falls back to the call site.
                int rootLine = 0;
                for (auto* frame = location; frame; frame = frame->getInlinedAt())
                {
                    const int line = static_cast<int>(frame->getLine());
                    if (!lineInUserRange(line)) continue;
                    rootLine = line;
                    LineOptInfo& entry = lineTotals[rootLine];
                    entry.srcLine = rootLine;
                    entry.irInstructions++;
                    if (frame != location || location->getInlinedAt()) entry.inlined = true;
                    break;
                }

                if (rootLine > 0)
                    RecordOptCost(instruction, rootLine, costTotals);
            }
    }

    // A remark's enclosing function is the one physically holding the code, so anything
    // reported against a non-user function is core-library noise that shares our line span.
    {
        llvm::TimeTraceScope remarksScope("OptInfoRemarks");
        if (!info.remarks.empty())
        {
            std::vector<OptRemark> kept;
            for (auto& remark : info.remarks)
            {
                // A caller the optimizer erased is still a user function that made real
                // decisions at real user lines, so the pre-opt index has to count too.
                if (!entryBySymbol.count(remark.function)
                    && !preoptEntryBySymbol.count(remark.function))
                    continue;
                if (!lineInUserRange(remark.srcLine)) continue;
                for (const auto& arg : remark.args)
                {
                    if (arg.first != "Callee") continue;
                    auto callee = preoptEntryBySymbol.find(arg.second);
                    if (callee != preoptEntryBySymbol.end())
                    {
                        remark.calleeName = out[callee->second].name;
                        remark.calleeLine = out[callee->second].startLine;
                    }
                    else
                    {
                        // Not defined in this file (printf, a core-library routine): the
                        // symbol IS the best name available, and there is no line to point at.
                        remark.calleeName = arg.second;
                    }
                    break;
                }
                kept.push_back(std::move(remark));
            }
            info.remarks = std::move(kept);
        }
    }

    for (auto& [key, cost] : costTotals) info.costs.push_back(cost);
    std::sort(info.costs.begin(), info.costs.end(),
              [](const OptCost& a, const OptCost& b) {
                  if (a.srcLine != b.srcLine) return a.srcLine < b.srcLine;
                  if (a.kind != b.kind) return a.kind < b.kind;
                  return a.detail < b.detail;
              });
    for (auto& info : out)
    {
        auto it = absorbedBy.find(info.name);
        if (it == absorbedBy.end()) continue;
        int count = 0;
        for (const auto& container : it->second)
        {
            // A function inlined into itself (or into its own instantiation) is not fan-out.
            auto owner = entryBySymbol.find(container);
            if (owner != entryBySymbol.end() && out[owner->second].name == info.name) continue;
            ++count;
        }
        info.inlinedInto = count;
    }

    // Exact byte sizes need the object file: COFF symbols carry no size field, so sizes
    // come from address-sorted deltas. Best effort - a failure just leaves bytes at 0.
    std::map<std::string, int> symbolBytes;
    {
        llvm::TimeTraceScope objectCodegenScope("OptInfoObjectCodegen");
        if (auto objectView = llvm::CloneModule(*view))
    {
        llvm::SmallString<0> objectBuffer;
        llvm::raw_svector_ostream objectStream(objectBuffer);
        llvm::legacy::PassManager objectPass;
        if (!targetMachine->addPassesToEmitFile(objectPass, objectStream, nullptr,
                                                llvm::CodeGenFileType::ObjectFile))
        {
            objectPass.run(*objectView);
            auto binary = llvm::object::ObjectFile::createObjectFile(
                llvm::MemoryBufferRef(llvm::StringRef(objectBuffer.data(), objectBuffer.size()),
                                      "optinfo"));
            if (binary)
            {
                for (const auto& sized : llvm::object::computeSymbolSizes(**binary))
                {
                    auto name = sized.first.getName();
                    if (!name) { llvm::consumeError(name.takeError()); continue; }
                    llvm::StringRef symbol = *name;
                    auto entry = entryBySymbol.find(symbol.str());
                    if (entry == entryBySymbol.end() && symbol.starts_with("_"))
                        entry = entryBySymbol.find(symbol.substr(1).str());
                    if (entry != entryBySymbol.end())
                        out[entry->second].bytes += static_cast<int>(sized.second);
                    symbolBytes[symbol.str()] = static_cast<int>(sized.second);
                }
            }
            else
            {
                llvm::consumeError(binary.takeError());
            }
        }
        }
    }

    // Monomorphization bloat, taken from the compiler's own instantiation registry rather
    // than guessed from mangled symbols. Each entry is a full instantiation name (list$int);
    // group them by the generic they came from and charge every function that mentions one.
    {
        std::map<std::string, OptInstantiation> byBase;
        for (const auto& instantiation : gts.instantiatedGenerics)
        {
            std::string_view base = MangledBase(instantiation);
            if (base == instantiation || base.empty()) continue;
            OptInstantiation& group = byBase[std::string(base)];
            group.base = std::string(base);
            group.count++;
            if (group.symbols.size() < 12) group.symbols.push_back(instantiation);
        }
        for (auto& function : *view)
        {
            if (function.isDeclaration()) continue;
            const std::string symbol = function.getName().str();
            auto size = symbolBytes.find(symbol);
            if (size == symbolBytes.end()) continue;
            // Charge the longest matching instantiation so list$list$int does not also
            // count against list$int.
            const OptInstantiation* best = nullptr;
            size_t bestLength = 0;
            for (const auto& [base, group] : byBase)
                for (const auto& name : group.symbols)
                    if (name.size() > bestLength && symbol.find(name) != std::string::npos)
                    {
                        best = &group;
                        bestLength = name.size();
                    }
            if (best) byBase[best->base].bytes += size->second;
        }
        for (auto& [base, group] : byBase)
            if (group.count > 0) info.instantiations.push_back(std::move(group));
        // Biggest first: this list answers "where is my binary going".
        std::sort(info.instantiations.begin(), info.instantiations.end(),
                  [](const OptInstantiation& a, const OptInstantiation& b) {
                      if (a.bytes != b.bytes) return a.bytes > b.bytes;
                      if (a.count != b.count) return a.count > b.count;
                      return a.base < b.base;
                  });
        if (info.instantiations.size() > 50) info.instantiations.resize(50);
    }

    // Assembly gives the machine instruction count; the codegen remarks emitted during
    // this same run give the frame size and spill counts.
    llvm::SmallString<0> asmBuffer;
    {
        llvm::TimeTraceScope asmCodegenScope("OptInfoAsmCodegen");
        llvm::raw_svector_ostream asmStream(asmBuffer);
        llvm::legacy::PassManager asmPass;
        if (targetMachine->addPassesToEmitFile(asmPass, asmStream, nullptr,
                                               llvm::CodeGenFileType::AssemblyFile))
            return false;
        asmPass.run(*view);
    }

    const std::string assembly(asmBuffer.data(), asmBuffer.size());
    const std::map<int, std::string> asmFiles = BuildAsmFileTable(assembly);
    const std::string rootPath = analyzedRootPath_;
    auto isRootAsmFile = [&](const std::string& path) {
        if (path.empty() || rootName.empty()) return false;
        if (!rootPath.empty() && NormalizeFilePath(path) == NormalizeFilePath(rootPath))
            return true;
        return std::filesystem::path(path).filename().string() == rootName;
    };

    size_t lineStart = 0;
    size_t current = out.size();
    int currentSourceLine = 0;   // from the most recent root-file .loc / .cv_loc
    while (lineStart < assembly.size())
    {
        size_t lineEnd = assembly.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = assembly.size();
        std::string line = assembly.substr(lineStart, lineEnd - lineStart);
        lineStart = lineEnd == assembly.size() ? assembly.size() : lineEnd + 1;

        int locFile = 0, locLine = 0, locColumn = 0;
        if (ParseLocDirective(line, locFile, locLine, locColumn))
        {
            auto file = asmFiles.find(locFile);
            // Same single-DIFile caveat as the IR walk: the file id alone cannot rule out
            // core-library code, so the line must also fall inside a function of this file.
            currentSourceLine = (file != asmFiles.end() && isRootAsmFile(file->second)
                                 && lineInUserRange(locLine)) ? locLine : 0;
            continue;
        }

        const std::string label = AsmLabelName(line);
        if (!label.empty())
        {
            // Local labels (.Lfunc_begin, .Ltmp, block labels) sit inside a function body
            // and must not end it; only .Lfunc_end does. A named label switches function.
            if (label[0] == '.')
            {
                if (label.rfind(".Lfunc_end", 0) == 0) current = out.size();
                continue;
            }
            auto entry = entryBySymbol.find(label);
            current = entry == entryBySymbol.end() ? out.size() : entry->second;
            currentSourceLine = 0;
            continue;
        }
        if (line.find(".seh_endproc") != std::string::npos
            || line.find(".cfi_endproc") != std::string::npos)
        {
            current = out.size();
            currentSourceLine = 0;
            continue;
        }
        if (!IsAsmInstructionLine(line)) continue;
        if (current < out.size()) out[current].machineInstructions++;
        // Counted even outside a matched function: the code may be an inlined body that
        // still belongs to the source line that wrote it.
        if (currentSourceLine > 0)
        {
            LineOptInfo& entry = lineTotals[currentSourceLine];
            entry.srcLine = currentSourceLine;
            entry.machineInstructions++;
        }
    }

    // Hand each line to the function whose source range contains it. Lines outside every
    // range (file-scope initializers, for one) have no lens to hang on and are dropped.
    for (const auto& [srcLine, totals] : lineTotals)
        for (auto& info : out)
            if (srcLine >= info.startLine && srcLine <= info.endLine)
            {
                info.lines.push_back(totals);
                break;
            }

    for (auto& info : out)
    {
        auto remark = frameRemarks.find(info.symbol);
        if (info.symbol.empty() || remark == frameRemarks.end()) continue;
        info.stackBytes = remark->second.stackBytes;
        info.spills = remark->second.spills;
        info.reloads = remark->second.reloads;
    }
    return true;
}

bool LLVMBackend::EmitExecutableElf(const std::string& exePath, bool debugInfo,
                           const std::optional<std::string>& lliPath)
{
        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmPrinters();

        const std::string triple = llvm::sys::getProcessTriple(); // e.g. x86_64-unknown-linux-gnu
        module->setTargetTriple(llvm::Triple(triple));

        std::string err;
        const llvm::Target* target = llvm::TargetRegistry::lookupTarget(llvm::Triple(triple), err);
        if (!target)
        {
            std::cout << std::format("Error: no target for triple '{}': {}\n", triple, err);
            return false;
        }

        std::string cpu = targetCpu_.empty() ? std::string("x86-64") : targetCpu_;
        llvm::TargetOptions opt;
        opt.FunctionSections = true;
        opt.DataSections     = true;
        // PIC so the object links into a position-independent executable (the
        // default on modern Linux toolchains).
        auto TM = std::unique_ptr<llvm::TargetMachine>(
            target->createTargetMachine(llvm::Triple(triple), cpu, "", opt, llvm::Reloc::PIC_,
                                        std::nullopt, CodeGenLevelFor(cOptLevel_)));
        module->setDataLayout(TM->createDataLayout());

        // Keep cflat's runtime DEFINITIONS out of the dynamic symbol table so they do
        // not interpose libc. cflat reuses libc names (printf/vsnprintf/malloc/...);
        // an executable that defines a symbol libc also exports would preempt libc's
        // OWN internal calls to it (e.g. glibc __vsnprintf_chk -> vsnprintf -> cflat's
        // vsnprintf), recursing forever. Hidden visibility makes each definition local
        // to the image while still callable from cflat code; external declarations
        // (the libc imports) are left untouched so they still bind to libc.
        for (llvm::Function& F : module->functions())
            if (!F.isDeclaration())
                F.setVisibility(llvm::GlobalValue::HiddenVisibility);
        for (llvm::GlobalVariable& G : module->globals())
            if (!G.isDeclaration())
                G.setVisibility(llvm::GlobalValue::HiddenVisibility);

        if (lliPath)
        {
            if (verbose) std::cout << std::format("[verbose] writing IR to {}\n", *lliPath);
            if (!SaveToFile(*lliPath))
            {
                std::cout << std::format("Error: failed to save IR to '{}'.\n", *lliPath);
                return false;
            }
        }

        const std::string objPath = exePath + ".o";
        std::error_code EC;
        llvm::raw_fd_ostream dest(objPath, EC, llvm::sys::fs::OF_None);
        if (EC)
        {
            std::cout << std::format("Error: could not write object file '{}': {}\n", objPath, EC.message());
            return false;
        }
        {
            llvm::legacy::PassManager pass;
            // Codegen runs its own libcall simplification with a default TargetLibraryInfo
            // (where vsnprintf/vfprintf are "available"), which would re-fold our
            // __vsnprintf_chk/__vfprintf_chk calls back into cflat's own vsnprintf/vfprintf
            // and recurse forever - even though the IR-level opt pipeline already kept them
            // intact. Seed the codegen PM with the same stdio-safe TLI so the fold stays off.
            pass.add(new llvm::TargetLibraryInfoWrapperPass(MakeStdioSafeTLII(llvm::Triple(triple))));
            if (TM->addPassesToEmitFile(pass, dest, nullptr, llvm::CodeGenFileType::ObjectFile))
            {
                std::cout << "Error: target does not support object file emission\n";
                return false;
            }
            pass.run(*module);
            dest.flush();
            dest.close();
        }

        // Link with the system C compiler driver: it pulls in the C runtime
        // startup and libc and resolves cflat's `main`.
        std::string cc;
        for (const char* cand : { "cc", "gcc", "clang", "clang-18" })
        {
            if (auto p = llvm::sys::findProgramByName(cand)) { cc = *p; break; }
        }
        if (cc.empty())
        {
            llvm::sys::fs::remove(objPath);
            std::cout << "Error: no C compiler driver (cc/gcc/clang) found to link the executable\n";
            return false;
        }

        std::vector<std::string> argStrs = { cc, objPath, "-o", exePath };
        // GC unreferenced sections: the auto-imported core runtime defines many
        // functions (stdio/stdout shims) that still reference Windows CRT symbols
        // until the core libs are ported. Emitting one section per function (set
        // above) lets the linker drop the ones this program does not call.
        argStrs.push_back("-Wl,--gc-sections");
        // -no-pie keeps global symbols out of .dynsym so --gc-sections can drop
        // unreferenced runtime functions (otherwise PIE exports them as GC roots).
        argStrs.push_back("-no-pie");
        // Merge any C objects compiled from .c inputs.
        for (auto& cObj : cObjectFiles_) argStrs.push_back(cObj);
        // Prebuilt C libraries (--c-lib).
        for (const auto& lib : cLinkLibs_) argStrs.push_back(lib);
        // Common runtime deps for cflat programs (math, threads, dlopen).
        argStrs.push_back("-lm");
        argStrs.push_back("-lpthread");
        argStrs.push_back("-ldl");
        if (debugInfo) argStrs.push_back("-g");

        std::vector<llvm::StringRef> args;
        for (auto& s : argStrs) args.push_back(s);

        std::cout << std::format("Linking (elf): {}\n", exePath);
        std::string linkErr;
        int rc = llvm::sys::ExecuteAndWait(cc, args, std::nullopt, {}, 0, 0, &linkErr);
        llvm::sys::fs::remove(objPath);
        for (auto& cObj : cObjectFiles_) llvm::sys::fs::remove(cObj);
        if (rc != 0)
        {
            std::cout << std::format("Error: linking failed (exit {}): {}\n", rc, linkErr);
            return false;
        }
        return true;
    }

bool LLVMBackend::AddFrameworkImport(const std::string& name)
{
        if (!targetMacOS_ && symbolSink_ == nullptr)
        {
            LogErrorMessage("import framework '{}' is only supported when targeting macOS", { name });
            return false;
        }
        for (const auto& f : cFrameworks_) if (f == name) return true;
        cFrameworks_.push_back(name);
        return true;
    }

void LLVMBackend::EmitMacInfoPlistSection(const std::string& executableName, bool bundleIcon)
{
    if (!applicationInfo_ || module == nullptr) return;
    const std::string xml = cflat::appres::BuildInfoPlist(*applicationInfo_, executableName,
        cflat::appres::kMacMinimumSystemVersion, bundleIcon);
    auto* bytes = llvm::ConstantDataArray::getString(*context, xml, false);
    auto* global = new llvm::GlobalVariable(*module, bytes->getType(), true,
        llvm::GlobalValue::PrivateLinkage, bytes, "__cflat_info_plist");
    global->setSection("__TEXT,__info_plist");
    global->setAlignment(llvm::Align(1));
    // Dead-strip would drop a private constant nothing references; llvm.used pins it.
    llvm::appendToUsed(*module, { global });
}

bool LLVMBackend::WriteMacBundleMetadata(const std::string& bundlePath, const std::string& stem)
{
    namespace fs = std::filesystem;
    const fs::path contents = fs::path(bundlePath) / "Contents";

    cflat::appres::AppInfoData info;
    if (applicationInfo_) info = *applicationInfo_;
    // Build the icns first: CFBundleIconFile may only be claimed when a file is actually written.
    const std::vector<uint8_t> icns = applicationInfo_
        ? cflat::appres::BuildIcns(*applicationInfo_) : std::vector<uint8_t>();
    const bool hasIcon = !icns.empty();
    const std::string plist = cflat::appres::BuildInfoPlist(info, stem,
        cflat::appres::kMacMinimumSystemVersion, hasIcon);

    auto writeFile = [&](const fs::path& path, const char* data, size_t size) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (out) out.write(data, static_cast<std::streamsize>(size));
        if (out) return true;
        LogError(std::format("could not write bundle file '{}'", path.string()));
        return false;
    };

    if (!writeFile(contents / "Info.plist", plist.data(), plist.size())) return false;
    static const char kPkgInfo[] = "APPL????";
    if (!writeFile(contents / "PkgInfo", kPkgInfo, sizeof(kPkgInfo) - 1)) return false;
    const fs::path icnsPath = contents / "Resources" / (stem + ".icns");
    if (hasIcon)
    {
        if (!writeFile(icnsPath, reinterpret_cast<const char*>(icns.data()), icns.size()))
            return false;
    }
    else
    {
        // A rebuild that drops the icon must not leave the previous bundle's icns behind.
        std::error_code removeError;
        fs::remove(icnsPath, removeError);
    }
    return true;
}

/*
 * `-o App.app` is the one bundle-emitting spelling: the executable lands in
 * Contents/MacOS/<stem> and the compiler writes the rest of the layout around it. Any other
 * output path is a bare Mach-O, which can carry the plist but never an icon.
 */
bool LLVMBackend::EmitMacApplication(const std::string& outputPath, bool debugInfo,
                                     const std::optional<std::string>& lliPath)
{
    namespace fs = std::filesystem;
    if (!ValidateApplicationForTarget(false)) return false;

    const bool bundle = fs::path(outputPath).extension() == ".app";
    if (!bundle)
    {
        EmitMacInfoPlistSection(fs::path(outputPath).filename().string(), false);
        return EmitExecutableMachO(outputPath, debugInfo, lliPath);
    }

    const std::string stem = fs::path(outputPath).stem().string();
    std::error_code ec;
    fs::create_directories(fs::path(outputPath) / "Contents" / "MacOS", ec);
    if (!ec) fs::create_directories(fs::path(outputPath) / "Contents" / "Resources", ec);
    if (ec)
    {
        LogError(std::format("could not create application bundle '{}': {}",
            outputPath, ec.message()));
        return false;
    }

    // Same rule as the on-disk plist: claim the icon only when an icns actually results.
    EmitMacInfoPlistSection(stem, applicationInfo_
        && !cflat::appres::BuildIcns(*applicationInfo_).empty());
    const std::string innerPath = (fs::path(outputPath) / "Contents" / "MacOS" / stem).string();
    if (!EmitExecutableMachO(innerPath, debugInfo, lliPath)) return false;
    return WriteMacBundleMetadata(outputPath, stem);
}

bool LLVMBackend::EmitExecutableMachO(const std::string& exePath, bool debugInfo,
                             const std::optional<std::string>& lliPath)
{
        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmPrinters();

        // Versioned triple (min macOS 11.0, the Apple Silicon baseline) so the
        // emitted Mach-O carries an LC_BUILD_VERSION load command; without a
        // version ld64 warns "no platform load command found" on every link.
        const std::string triple = "arm64-apple-macosx11.0.0";
        module->setTargetTriple(llvm::Triple(triple));

        std::string err;
        const llvm::Target* target = llvm::TargetRegistry::lookupTarget(llvm::Triple(triple), err);
        if (!target)
        {
            std::cout << std::format("Error: no target for triple '{}': {}. The macOS arm64 "
                                     "target needs an LLVM built with the AArch64 backend "
                                     "(apt llvm-18 on Linux/WSL has it).\n", triple, err);
            return false;
        }

        // Apple Silicon baseline. --cpu overrides (e.g. apple-m2).
        std::string cpu = targetCpu_.empty() ? std::string("apple-m1") : targetCpu_;
        llvm::TargetOptions opt;
        opt.FunctionSections = true;
        opt.DataSections     = true;
        // Darwin code is always PIC.
        auto TM = std::unique_ptr<llvm::TargetMachine>(
            target->createTargetMachine(llvm::Triple(triple), cpu, "", opt, llvm::Reloc::PIC_,
                                        std::nullopt, CodeGenLevelFor(cOptLevel_)));
        module->setDataLayout(TM->createDataLayout());

        if (lliPath)
        {
            if (verbose) std::cout << std::format("[verbose] writing IR to {}\n", *lliPath);
            if (!SaveToFile(*lliPath))
            {
                std::cout << std::format("Error: failed to save IR to '{}'.\n", *lliPath);
                return false;
            }
        }

        const std::string objPath = exePath + ".o";
        std::error_code EC;
        llvm::raw_fd_ostream dest(objPath, EC, llvm::sys::fs::OF_None);
        if (EC)
        {
            std::cout << std::format("Error: could not write object file '{}': {}\n", objPath, EC.message());
            return false;
        }
        {
            llvm::legacy::PassManager pass;
            // Same stdio-safe TLI as EmitExecutableElf: codegen runs its own libcall
            // simplification, which would fortify-fold our __vsnprintf_chk call back
            // into a bare vsnprintf - and cflat DEFINES vsnprintf, so that recurses
            // forever. Mark the format libcalls unavailable so the fold stays off.
            pass.add(new llvm::TargetLibraryInfoWrapperPass(MakeStdioSafeTLII(llvm::Triple(triple))));
            if (TM->addPassesToEmitFile(pass, dest, nullptr, llvm::CodeGenFileType::ObjectFile))
            {
                std::cout << "Error: target does not support object file emission\n";
                return false;
            }
            pass.run(*module);
            dest.flush();
            dest.close();
        }

        // Link only if a Darwin-capable linker exists (a real Mac, or osxcross).
        // ld64 / the macOS SDK is absent on the WSL cross-host, so the object IS
        // the deliverable there; report it and stop without claiming an exe.
        const bool darwinHost =
            llvm::Triple(llvm::sys::getProcessTriple()).isOSDarwin();

#if defined(__APPLE__)
        // Prefer the bundled ld64.lld (deployed next to cflat), invoked directly -
        // mirroring the Windows lld-link path. The SDK still supplies libSystem via
        // -syslibroot; harvesting our own libSystem.tbd (step 3) is what drops that.
        if (darwinHost)
        {
            const std::string ld64 = FindBundledLd64Lld();
            // Prefer the SDK-free stub harvested by `cflat --init` (no CLT needed);
            // fall back to $SDKROOT / xcrun when it hasn't been generated.
            const std::string stubRoot = MacStubSyslibroot();
            std::string sdk = stubRoot;
            // sdkVer is resolved the same way regardless of cache state, so a warm vs
            // cold `--init` cache never changes what gets stamped: stub present -> its
            // own harvest-time provenance file; stub absent -> xcrun. Either path falls
            // back to the live running-OS version, then "11.0" as the last resort.
            std::string sdkVer;
            if (!sdk.empty())
            {
                sdkVer = MacStubSdkVersion();
                if (sdkVer.empty()) sdkVer = MacHostOsProductVersion();
            }
            else
            {
                if (const char* env = std::getenv("SDKROOT")) sdk = env;
                if (sdk.empty()) sdk = CaptureToolLine("xcrun --show-sdk-path 2>/dev/null");
                sdkVer = CaptureToolLine("xcrun --show-sdk-version 2>/dev/null");
                if (sdkVer.empty()) sdkVer = MacHostOsProductVersion();
            }
            sdkVer = TwoComponentVersion(sdkVer);
            if (sdkVer.empty()) sdkVer = "11.0";
            if (!ld64.empty() && !sdk.empty())
            {
                std::vector<std::string> argStrs = {
                    ld64, "-arch", "arm64",
                    "-platform_version", "macos", "11.0.0", sdkVer,
                    "-syslibroot", sdk, "-o", exePath, objPath };
                for (auto& cObj : cObjectFiles_) argStrs.push_back(cObj);
                for (const auto& lib : cLinkLibs_) argStrs.push_back(lib);
                // macOS frameworks (`import framework`). When the SDK-free harvested
                // root is in use, point -F at its Frameworks dir so ld64 finds the
                // harvested stubs; the SDK/xcrun syslibroot already carries the default path.
                if (!cFrameworks_.empty() && !stubRoot.empty())
                    argStrs.push_back("-F"), argStrs.push_back(stubRoot + "/System/Library/Frameworks");
                // Frameworks not in the harvested root (e.g. CoreGraphics from a header
                // bind) resolve from the real SDK: add its Frameworks dir as an extra -F.
                // ld64 tries syslibroot+path then path, so this coexists with the stub root.
                if (!cFrameworks_.empty())
                {
                    const std::string& fwSdk = MacSdkPathCached();
                    if (!fwSdk.empty() && fwSdk != stubRoot)
                        argStrs.push_back("-F"), argStrs.push_back(fwSdk + "/System/Library/Frameworks");
                }
                for (const auto& fw : cFrameworks_)
                    argStrs.push_back("-framework"), argStrs.push_back(fw);
                if (cLinkObjC_) argStrs.push_back("-lobjc");
                // The AArch64 backend can emit compiler-rt libcalls (e.g. __multi3);
                // clang always links libclang_rt.osx.a, so mirror it when locatable.
                // Also the source of the asan runtime dylib below (--asan) - hoisted
                // so we only shell out to `clang -print-resource-dir` once.
                const std::string rtDir = CaptureToolLine("clang -print-resource-dir 2>/dev/null");
                if (asan_)
                {
                    const std::string asanDylib = rtDir + "/lib/darwin/libclang_rt.asan_osx_dynamic.dylib";
                    if (rtDir.empty() || !llvm::sys::fs::exists(asanDylib))
                    {
                        LogErrorMessage("{} on macOS requires the AddressSanitizer runtime "
                                        "from Xcode or the Command Line Tools (the SDK-free "
                                        "harvested-stub link cannot supply it); install one and retry.",
                                        { "--asan" });
                        return false;
                    }
                    // Install name is @rpath/libclang_rt.asan_osx_dynamic.dylib.
                    // Listed before -lSystem to mirror clang's own
                    // -fsanitize=address link order (link order does not by
                    // itself affect interception - see the __asan_default_options
                    // override in OptimizeModule for the real fix needed here).
                    argStrs.push_back(asanDylib);
                    argStrs.push_back("-rpath");
                    argStrs.push_back(rtDir + "/lib/darwin");
                }
                argStrs.push_back("-lSystem");
                if (!rtDir.empty())
                {
                    const std::string rt = rtDir + "/lib/darwin/libclang_rt.osx.a";
                    if (llvm::sys::fs::exists(rt)) argStrs.push_back(rt);
                }
                std::vector<llvm::StringRef> args;
                for (auto& s : argStrs) args.push_back(s);
                if (verbose)
                {
                    std::string joined;
                    for (auto& s : argStrs) joined += (joined.empty() ? "" : " ") + s;
                    std::cout << std::format("[verbose] ld64.lld link line: {}\n", joined);
                }
                std::cout << std::format("Linking (ld64.lld{}): {}\n",
                                         stubRoot.empty() ? "" : ", SDK-free", exePath);
                std::string linkErr;
                int rc = llvm::sys::ExecuteAndWait(ld64, args, std::nullopt, {}, 0, 0, &linkErr);
                llvm::sys::fs::remove(objPath);
                for (auto& cObj : cObjectFiles_) llvm::sys::fs::remove(cObj);
                if (rc != 0)
                {
                    std::cout << std::format("Error: linking failed (exit {}): {}\n", rc, linkErr);
                    return false;
                }
                return true;
            }
        }
#endif

        // Fallback: host clang driver (bundled ld64.lld/SDK missing, or an osxcross
        // cross-link from a non-Darwin host). Same behavior as before this change.
        std::string cc;
        for (const char* cand : { "o64-clang", "oa64-clang", "clang" })
        {
            if (auto p = llvm::sys::findProgramByName(cand)) { cc = *p; break; }
        }
        if (cc.empty() || !darwinHost)
        {
            (void)debugInfo;
            std::cout << std::format("Emitted Mach-O arm64 object (no Darwin linker on this host; "
                                     "link on macOS): {}\n", objPath);
            return true;
        }

        std::vector<std::string> argStrs = { cc, "-target", "arm64-apple-macosx11.0.0",
                                             objPath, "-o", exePath };
        for (auto& cObj : cObjectFiles_) argStrs.push_back(cObj);
        for (const auto& lib : cLinkLibs_) argStrs.push_back(lib);
        // macOS frameworks (`import framework`); the clang driver accepts -framework.
        for (const auto& fw : cFrameworks_)
            argStrs.push_back("-framework"), argStrs.push_back(fw);
        if (cLinkObjC_) argStrs.push_back("-lobjc");
        if (debugInfo) argStrs.push_back("-g");
        if (asan_) argStrs.push_back("-fsanitize=address");

        std::vector<llvm::StringRef> args;
        for (auto& s : argStrs) args.push_back(s);

        std::cout << std::format("Linking (mach-o): {}\n", exePath);
        std::string linkErr;
        int rc = llvm::sys::ExecuteAndWait(cc, args, std::nullopt, {}, 0, 0, &linkErr);
        llvm::sys::fs::remove(objPath);
        for (auto& cObj : cObjectFiles_) llvm::sys::fs::remove(cObj);
        if (rc != 0)
        {
            std::cout << std::format("Error: linking failed (exit {}): {}\n", rc, linkErr);
            return false;
        }
    return true;
    }

bool LLVMBackend::ValidateManifestActivationContext(const std::string& xml) const
{
#if !defined(_WIN32)
    (void)xml;
    return true;
#else
    llvm::SmallString<256> manifestPath;
    if (auto ec = llvm::sys::fs::createTemporaryFile("cflat_manifest_check", "manifest", manifestPath))
    {
        LogError(std::format("could not create manifest backstop input: {}", ec.message()));
        return false;
    }
    std::string path = manifestPath.str().str();
    std::string document = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n" + xml;
    {
        std::ofstream out(path, std::ios::binary);
        out.write(document.data(), static_cast<std::streamsize>(document.size()));
        if (!out)
        {
            out.close();
            llvm::sys::fs::remove(path);
            LogError(std::format("could not write manifest backstop input '{}'", path));
            return false;
        }
    }

    std::wstring widePath = std::filesystem::path(path).wstring();
    CflatActCtxW actCtx = {};
    actCtx.cbSize = sizeof(actCtx);
    actCtx.lpSource = widePath.c_str();
    void* handle = CreateActCtxW(&actCtx);
    if (handle == reinterpret_cast<void*>(static_cast<intptr_t>(-1)))
    {
        unsigned long error = GetLastError();
        std::string locations;
        for (const auto& fragment : manifestFragments_)
        {
            if (!locations.empty()) locations += ", ";
            locations += std::format("{}:{}", fragment.SourceFile, fragment.Line);
        }
        llvm::sys::fs::remove(path);
        LogError(std::format(
            "manifest activation context failed with Win32 error {} for declaration(s) at {}",
            error, locations));
        return false;
    }

    for (const auto& fragment : manifestFragments_)
        for (const auto& leaf : fragment.Leaves)
        {
            if (leaf.Namespace.empty()) continue;
            std::wstring wideNamespace = ManifestAsciiToWide(leaf.Namespace);
            std::wstring wideName = ManifestAsciiToWide(leaf.LocalName);
            std::vector<wchar_t> buffer(1024);
            size_t written = 0;
            int ok = QueryActCtxSettingsW(0, handle, wideNamespace.c_str(), wideName.c_str(),
                buffer.data(), buffer.size(), &written);
            size_t length = written > 0 ? written - 1 : 0;
            std::wstring returned(buffer.data(), std::min(length, buffer.size()));
            std::string returnedText;
            for (wchar_t c : returned) returnedText.push_back(static_cast<char>(c));
            if (!ok || returnedText != leaf.Text)
            {
                unsigned long error = ok ? 0 : GetLastError();
                ReleaseActCtx(handle);
                llvm::sys::fs::remove(path);
                LogError(std::format(
                    "manifest [JsonText] backstop failed for '{}:{}' declared at {}:{}: "
                    "Win32 error {}; OS returned '{}', expected '{}'",
                    leaf.Namespace, leaf.LocalName, leaf.SourceFile, leaf.Line,
                    error, returnedText, leaf.Text));
                return false;
            }
        }

    ReleaseActCtx(handle);
    llvm::sys::fs::remove(path);
    return true;
#endif
}

bool LLVMBackend::EmitExecutable(const std::string& exePath, const std::string& platform, bool debugInfo,
                        const std::optional<std::string>& lliPath)
{
        MaterializeCoreIfLazy();
        // macOS arm64 cross-target: Mach-O object emission, independent of host OS
        // (handled before the host-specific COFF/ELF split below).
        if (targetMacOS_)
            return EmitMacApplication(exePath, debugInfo, lliPath);
#if !defined(_WIN32)
        // Linux/Unix host: emit a native ELF executable. The Windows/COFF path
        // below is unreachable here (but still compiles).
        (void)platform;
        return EmitExecutableElf(exePath, debugInfo, lliPath);
#endif
        std::string triple;
        std::string clangTarget;
        std::string cpu;
        std::string clangBits;
        if (platform == "win32")
        {
            triple = "i686-pc-windows-msvc";
            clangTarget = "--target=i686-pc-windows-msvc";
            clangBits = "-m32";
            cpu = "i686";
        }
        else // win64 (default)
        {
            triple = "x86_64-pc-windows-msvc";
            clangTarget = "--target=x86_64-pc-windows-msvc";
            clangBits = "-m64";
            cpu = "x86-64";
        }

        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmPrinters();

        module->setTargetTriple(llvm::Triple(triple));

        std::string err;
        const llvm::Target* target = llvm::TargetRegistry::lookupTarget(llvm::Triple(triple), err);
        if (!target)
        {
            std::cout << std::format("Error: no target for triple '{}': {}\n", triple, err);
            return false;
        }

        // --cpu overrides the platform default. The value was already resolved ("native"
        // -> host CPU) and validated in Compile, so it can be used verbatim here.
        if (!targetCpu_.empty())
            cpu = targetCpu_;

        llvm::TargetOptions opt;
        // Emit one section per function/global so lld-link's /OPT:REF can garbage-collect
        // unreferenced symbols. This is pure object layout - it adds no optimization and does
        // not touch the IR, so it has no effect on the --out-lli dump written just below.
        opt.FunctionSections = true;
        opt.DataSections     = true;
        auto TM = std::unique_ptr<llvm::TargetMachine>(
            target->createTargetMachine(llvm::Triple(triple), cpu, "", opt, llvm::Reloc::PIC_,
                                        std::nullopt, CodeGenLevelFor(cOptLevel_)));
        module->setDataLayout(TM->createDataLayout());

        // --out-lli for an -o build: dump the IR here, after the target triple and data layout
        // are finalized and immediately before codegen, so the .ll is exactly what gets
        // instruction-selected into the object. (Standalone --out-lli without -o is written in
        // Compile, since EmitExecutable does not run there.)
        if (lliPath)
        {
            llvm::TimeTraceScope irScope("WriteIR", *lliPath);
            if (verbose) std::cout << std::format("[verbose] writing IR to {}\n", *lliPath);
            if (!SaveToFile(*lliPath))
            {
                std::cout << std::format("Error: failed to save IR to '{}'.\n", *lliPath);
                return false;
            }
        }

        auto objPath = exePath + ".obj";
        std::error_code EC;
        llvm::raw_fd_ostream dest(objPath, EC, llvm::sys::fs::OF_None);
        if (EC)
        {
            std::cout << std::format("Error: could not write object file '{}': {}\n", objPath, EC.message());
            return false;
        }

        {
            llvm::TimeTraceScope codegenScope("ObjectCodegen", exePath);
            llvm::legacy::PassManager pass;
            if (TM->addPassesToEmitFile(pass, dest, nullptr, llvm::CodeGenFileType::ObjectFile))
            {
                std::cout << "Error: target does not support object file emission\n";
                return false;
            }
            pass.run(*module);
            dest.flush();
            dest.close();
        }

        const std::string arch = (platform == "win32") ? "x86" : "x64";

        // Under -g, compile and link the in-process crash handler (DbgHelp symbolizes CFlat frames).
        // If CompileCrashHandlerObject fails, skip handler link args but still produce the PDB.
        bool crashHandlerLinked = debugInfo && CompileCrashHandlerObject(arch);

        // Keep the stock VC CRT (msvcrt.lib + vcruntime140.dll + mainCRTStartup) when:
        //   - ASan: its runtime expects the stock CRT and would clash with our mem*; or
        //   - x86 (win32): freestanding x86 additionally needs the x86 64-bit compiler-runtime
        //     intrinsics (__alldiv/__aulldiv/__allrem/__aullrem/__allmul/shifts), __tls_array,
        //     and __fltused - normally from the VC CRT and not available to link as compiler-rt
        //     here. Until those are ported, x86 stays on the proven stock path. The x86 import
        //     libs are already synthesized (--init) and the cflat_builtins.c decoration guards
        //     are in place, so finishing x86 is just that intrinsic set. See Phase D of
        //     internal/plan/remove-vcruntime-dependency.md.
        // Otherwise we drop vcruntime entirely: cflat_builtins.c supplies our own CRT entry, the
        // mem*/str* intrinsics and (on x64) a real __C_specific_handler, so even the `program`
        // construct's SEH crash isolation (catchpad personality, MainListener.h) works without it.
        const bool keepVcRuntime = asan_ || arch == "x86";

        if (!keepVcRuntime)
            CompileBuiltinsObject(arch);

        LinkerPaths linkerPaths_;
        {
            llvm::TimeTraceScope pathScope("FindLinkerPaths", exePath);
            linkerPaths_ = FindLinkerPaths(arch, runtimeDir, verbose);
        }
        const std::string& lldLinkPath = linkerPaths_.lldLink;
        const std::string& msvcLibPath = linkerPaths_.msvcLib;
        const std::string& ucrtLibPath = linkerPaths_.ucrtLib;
        const std::string& umLibPath   = linkerPaths_.umLib;

        if (lldLinkPath.empty())
        {
            llvm::sys::fs::remove(objPath);
            std::cout << "Error: lld-link.exe not found\n";
            return false;
        }

        std::vector<std::string> linkArgStrs = {
            lldLinkPath,
            "/out:" + exePath,
            "/subsystem:" + windowsSubsystem_,
        };
        // /subsystem:windows makes the linker look for WinMain. CFlat programs only ever have
        // `main`, so name the entry point explicitly rather than making users write WinMain.
        if (windowsSubsystem_ == "windows")
            linkArgStrs.push_back("/entry:mainCRTStartup");
        if (debugInfo)
        {
            // /DEBUG embeds CodeView from the object into a PDB next to the EXE.
            // Without this, the DI metadata we emitted is dropped at link time.
            linkArgStrs.push_back("/DEBUG");
            // Derive <exe>.pdb from the exe path by swapping the extension.
            std::string pdbPath = exePath;
            auto dot = pdbPath.find_last_of('.');
            auto sep = pdbPath.find_last_of("/\\");
            if (dot != std::string::npos && (sep == std::string::npos || dot > sep))
                pdbPath.replace(dot, std::string::npos, ".pdb");
            else
                pdbPath += ".pdb";
            linkArgStrs.push_back("/PDB:" + pdbPath);
        }
        if (crashHandlerLinked)
        {
            // DbgHelp for the in-process crash handler. Gated on crashHandlerLinked so a failed
            // handler compile doesn't /INCLUDE a symbol that was never emitted.
            // (heap_audit.c pulls in dbghelp.lib itself via #pragma comment(lib) so its leak-site
            // symbolization works whether or not the crash handler is linked.)
            linkArgStrs.push_back("dbghelp.lib");
            // Force-retain the crash handler's .CRT$XCU initializer - lld-link's /OPT:REF would
            // garbage-collect it since it has no external references. x86 uses a leading underscore.
            linkArgStrs.push_back(arch == "x86"
                ? "/INCLUDE:_cflat_crash_init_"
                : "/INCLUDE:cflat_crash_init_");
        }
        // SDK-free path: if --init synthesized the system import libs (from the OS-resident
        // DLLs) and this is a freestanding x64 build, link against those instead of the Windows
        // SDK / VS lib directories - so the user needs neither installed. ASan still needs the
        // VS libs (clang_rt.asan + the stock CRT), so it keeps the SDK paths. /nodefaultlib:
        // oldnames suppresses the /defaultlib:oldnames directive the clang-cl (/MD) objects emit;
        // oldnames.lib lives in the VS lib dir we are deliberately not adding, and nothing here
        // needs its POSIX-name aliases.
        // x64 only for now: x86 also reaches here as keepVcRuntime (stock path), and finishing
        // x86 freestanding is gated on the x86 compiler-runtime intrinsics (see keepVcRuntime).
        std::string syntheticLibDir = GetSyntheticLibDir(arch);
        const bool useSyntheticLibs = !keepVcRuntime && arch == "x64"
            && !syntheticLibDir.empty()
            && std::filesystem::exists(std::filesystem::path(syntheticLibDir) / "ucrt.lib")
            && std::filesystem::exists(std::filesystem::path(syntheticLibDir) / "kernel32.lib");

        if (useSyntheticLibs)
        {
            // Synthetic dir first: ucrt/kernel32/ws2_32/ntdll/dbghelp resolve here (first match
            // wins), so basic code links with no Windows SDK present. The SDK dirs are added
            // after as a fallback chain for advanced cases that genuinely still need the SDK:
            //   - 'um':       long-tail system import libs we do not synthesize (user32/gdi32/
            //                 uuid/... pulled by system-header `#pragma comment(lib,...)`).
            //   - 'msvc-lib': libcmt/msvcrt - reached only by a prebuilt user lib built /MT, a
            //                 user .c using /GS (__security_cookie/check), or a CRT header-inline
            //                 we have not shimmed. A basic build requests none of these, so it
            //                 never consults this dir and stays SDK-free; lld pulls only the
            //                 specific archive members an advanced build leaves undefined.
            linkArgStrs.push_back("/libpath:" + syntheticLibDir);
            linkArgStrs.push_back("/nodefaultlib:oldnames");
            if (!umLibPath.empty())   linkArgStrs.push_back("/libpath:" + umLibPath);
            if (!msvcLibPath.empty()) linkArgStrs.push_back("/libpath:" + msvcLibPath);
        }
        else
        {
            if (!msvcLibPath.empty()) linkArgStrs.push_back("/libpath:" + msvcLibPath);
            if (!ucrtLibPath.empty()) linkArgStrs.push_back("/libpath:" + ucrtLibPath);
            if (!umLibPath.empty())   linkArgStrs.push_back("/libpath:" + umLibPath);
        }

        // No source for the OS CRT import libs: neither the --init cache (synthetic libs)
        // nor a Windows SDK ucrt.lib is present. Fail early with an actionable message
        // instead of letting lld-link emit a raw "could not open ucrt.lib".
        if (!useSyntheticLibs && ucrtLibPath.empty())
        {
            llvm::sys::fs::remove(objPath);
            for (auto& cObj : cObjectFiles_) llvm::sys::fs::remove(cObj);
            if (!keepVcRuntime && arch == "x64")
                LogErrorMessage("Run cflat --init for the first time to finish setting up.");
            else
                LogErrorMessage("Windows SDK / Visual Studio build tools are required for this build "
                                "configuration but were not found.");
            return false;
        }

        // AddressSanitizer: link the dynamic asan runtime (matches /MD-style CRT), force-include
        // the thunk via /wholearchive to retain CRT$XI* interceptors. DLL copied next to exe.
        std::string asanDllToCopy;
        if (asan_)
        {
            if (!ResolveAsanRuntime(arch, msvcLibPath, linkArgStrs, asanDllToCopy))
            {
                llvm::sys::fs::remove(objPath);
                for (auto& cObj : cObjectFiles_) llvm::sys::fs::remove(cObj);
                return false;
            }
        }

        // ucrt.lib is always needed - it is the import lib for the OS-resident Universal CRT.
        linkArgStrs.push_back("ucrt.lib");
        if (keepVcRuntime)
        {
            // ASan path: keep the stock VC CRT. msvcrt.lib provides mainCRTStartup (the default
            // entry) and vcruntime140.dll provides the mem*/EH symbols ASan's runtime expects.
            linkArgStrs.push_back("msvcrt.lib");
            linkArgStrs.push_back("vcruntime.lib");
        }
        else
        {
            // Freestanding path: cflat_builtins.c supplies our own entry (cflat_start) plus the
            // mem*/str* intrinsics and a real x64 __C_specific_handler. So we need neither the VC
            // startup lib (msvcrt.lib, a Visual Studio component) nor VCRUNTIME140.dll (the VC++
            // redistributable). /nodefaultlib suppresses the /defaultlib directives the clang-cl
            // (/MD) builtins object emits that would otherwise re-pull both.
            linkArgStrs.push_back("/entry:cflat_start");
            linkArgStrs.push_back("/nodefaultlib:msvcrt.lib");
            linkArgStrs.push_back("/nodefaultlib:vcruntime.lib");
            // _fltused (the MSVC FP-usage marker) is dropped with the VC CRT. The synthetic
            // ntdll.lib re-exports it, but the Windows SDK's ntdll.lib does not - so on the
            // SDK-fallback path (no --init) it is unresolved. Redirect it to our private
            // definition only when nothing else supplies it; /alternatename is a no-op when
            // _fltused already resolves (synthetic path), avoiding a duplicate symbol.
            linkArgStrs.push_back("/alternatename:_fltused=__cflat_fltused");
            // __chkstk (the MSVC stack-probe stub, emitted for >4KB frames) lives in the static
            // CRT, which the freestanding link drops. kernel32.dll exports it (forwarded to ntdll),
            // so the synthetic-lib path resolves it and this redirect is a no-op; on the SDK
            // fallback (whose kernel32.lib omits __chkstk) it supplies our private definition.
            linkArgStrs.push_back("/alternatename:__chkstk=__cflat_chkstk");
        }
        linkArgStrs.push_back("kernel32.lib");
        linkArgStrs.push_back("ws2_32.lib");
        // ntdll.lib provides RtlUnwindEx, used by cflat_builtins.c's __C_specific_handler.
        // Harmless when unreferenced (no DLL import is added unless a symbol is pulled).
        linkArgStrs.push_back("ntdll.lib");
        // advapi32.lib provides OpenProcessToken/LookupPrivilegeValueA/AdjustTokenPrivileges
        // (core/os.windows.cb's huge-page privilege dance). Harmless when unreferenced.
        linkArgStrs.push_back("advapi32.lib");
        linkArgStrs.push_back(objPath);
        // Merge any C objects compiled by clang-cl from .c inputs.
        for (auto& cObj : cObjectFiles_) linkArgStrs.push_back(cObj);
        // Prebuilt C import libraries (--c-lib): add each lib's dir as libpath, then name.
        // Keeps behavior uniform with system libs above.
        for (const auto& lib : cLinkLibs_)
        {
            auto libDir = std::filesystem::path(lib).parent_path().string();
            if (!libDir.empty()) linkArgStrs.push_back("/libpath:" + libDir);
            linkArgStrs.push_back(std::filesystem::path(lib).filename().string());
        }

        // ONE .res carries every Win32 resource: the merged manifest, the version block and the
        // icon group. Either declaration alone still produces it; neither produces none.
        std::vector<std::string> resourceTempFiles;
        auto cleanupResourceFiles = [&]() {
            for (const auto& path : resourceTempFiles)
                llvm::sys::fs::remove(path);
            resourceTempFiles.clear();
        };
        if (!manifestFragments_.empty() || applicationInfo_)
        {
            std::string mergedManifestXml;
            if (!manifestFragments_.empty())
            {
                auto merged = MergeManifestFragments();
                if (!merged || !ValidateManifestActivationContext(*merged))
                {
                    llvm::sys::fs::remove(objPath);
                    for (auto& cObj : cObjectFiles_) llvm::sys::fs::remove(cObj);
                    return false;
                }
                mergedManifestXml =
                    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n" + *merged;
            }
            if (!ValidateApplicationForTarget(true))
            {
                llvm::sys::fs::remove(objPath);
                for (auto& cObj : cObjectFiles_) llvm::sys::fs::remove(cObj);
                return false;
            }

            llvm::SmallString<256> resourcePath;
            if (auto ec = llvm::sys::fs::createTemporaryFile("cflat_resource", "res", resourcePath))
            {
                LogError(std::format("could not create temporary resource file: {}", ec.message()));
                llvm::sys::fs::remove(objPath);
                for (auto& cObj : cObjectFiles_) llvm::sys::fs::remove(cObj);
                return false;
            }
            std::string path = resourcePath.str().str();
            cflat::appres::AppInfoData info;
            if (applicationInfo_) info = *applicationInfo_;
            bool resOverflow = false;
            auto resBytes = cflat::appres::BuildWindowsRes(info,
                std::filesystem::path(exePath).filename().string(), mergedManifestXml, resOverflow);
            if (resOverflow)
            {
                llvm::sys::fs::remove(path);
                LogError("the application version resource is too large to encode; "
                    "shorten the application text fields");
                llvm::sys::fs::remove(objPath);
                for (auto& cObj : cObjectFiles_) llvm::sys::fs::remove(cObj);
                return false;
            }
            bool written = false;
            {
                std::ofstream out(path, std::ios::binary | std::ios::trunc);
                if (out)
                {
                    out.write(reinterpret_cast<const char*>(resBytes.data()),
                        static_cast<std::streamsize>(resBytes.size()));
                    written = static_cast<bool>(out);
                }
            }
            if (!written)
            {
                llvm::sys::fs::remove(path);
                LogError(std::format("could not write temporary resource file '{}'", path));
                llvm::sys::fs::remove(objPath);
                for (auto& cObj : cObjectFiles_) llvm::sys::fs::remove(cObj);
                return false;
            }
            resourceTempFiles.push_back(path);
            linkArgStrs.push_back(path);
        }

        std::vector<llvm::StringRef> linkArgs;
        for (auto& s : linkArgStrs) linkArgs.push_back(s);

        std::cout << std::format("Linking ({}): {}\n", arch, exePath);

        {
            llvm::TimeTraceScope linkScope("Link", exePath);
            std::string linkErr;
            int rc = llvm::sys::ExecuteAndWait(lldLinkPath, linkArgs, std::nullopt, {}, 0, 0, &linkErr);
            llvm::sys::fs::remove(objPath);
            for (auto& cObj : cObjectFiles_) llvm::sys::fs::remove(cObj);
            cleanupResourceFiles();

            if (rc != 0)
            {
                std::cout << std::format("Error: linking failed (exit {}): {}\n", rc, linkErr);
                return false;
            }
        }

        // Copy each --c-lib's sibling runtime DLL next to the exe for self-contained launch.
        // Conan puts the DLL beside the import lib or in ../bin - check both. Best-effort.
        {
            llvm::TimeTraceScope dllScope("CopyCRuntimeDlls", exePath);
            CopyCRuntimeDlls(exePath);
        }

        // The asan runtime is a DLL, so it must sit next to the exe to launch (the
        // instrumented program imports __asan_* from it). Copy it after a successful link.
        if (asan_ && !asanDllToCopy.empty())
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            fs::path dest = fs::path(exePath).parent_path() / fs::path(asanDllToCopy).filename();
            fs::copy_file(asanDllToCopy, dest, fs::copy_options::overwrite_existing, ec);
            if (ec)
                LogErrorMessage("{}: failed to copy asan runtime DLL '{}' next to '{}': {}",
                                { "--asan", asanDllToCopy, exePath, ec.message() });
            else if (verbose)
                std::cout << std::format("[verbose]   copied asan runtime DLL: {} -> {}\n", asanDllToCopy, dest.string());
        }

        // Normal builds are vcruntime-free, but an --asan build links the VC++ runtime (and its
        // asan runtime DLL needs it too). If it is not installed the program fails to start with
        // a cryptic loader error, so point the user at the redistributable - they may not know.
        if (keepVcRuntime && !VcRuntimeInstalled())
        {
            const char* redist = (arch == "x86") ? "vc_redist.x86.exe" : "vc_redist.x64.exe";
            std::cout << std::format(
                "Note: this --asan build depends on the Microsoft Visual C++ runtime "
                "(vcruntime140.dll),\n"
                "      which was not found on this system. The program will not start until the\n"
                "      Microsoft Visual C++ Redistributable is installed:\n"
                "        https://aka.ms/vs/17/release/{}\n"
                "      (Normal builds do not depend on it; only --asan does.)\n", redist);
        }
        return true;
    }

bool LLVMBackend::ReachesThreadSpawn()
{
        auto isSpawnPrimitive = [](llvm::StringRef name) {
            return name == "CreateThread" || name == "_beginthreadex" || name == "_beginthread";
        };

        llvm::Function* mainFn = module->getFunction("main");
        if (!mainFn) return false;

        llvm::SmallVector<llvm::Function*, 32> worklist{mainFn};
        llvm::SmallPtrSet<llvm::Function*, 32> visited{mainFn};

        while (!worklist.empty())
        {
            llvm::Function* fn = worklist.pop_back_val();
            for (auto& bb : *fn)
            {
                for (auto& inst : bb)
                {
                    auto* call = llvm::dyn_cast<llvm::CallBase>(&inst);
                    if (!call) continue;
                    llvm::Function* callee = call->getCalledFunction();
                    if (!callee) continue; // indirect/virtual call - not followed
                    if (isSpawnPrimitive(callee->getName()))
                        return true;
                    if (callee->isDeclaration()) continue; // no body to walk into
                    if (visited.insert(callee).second)
                        worklist.push_back(callee);
                }
            }
        }
        return false;
    }

void LLVMBackend::InjectHeapAuditIntoMain()
{
        llvm::Function* mainFn = module->getFunction("main");
        if (!mainFn || mainFn->isDeclaration())
            return;

        llvm::Function* enableFn = module->getFunction("_HeapAudit.enable_void__");
        llvm::Function* reportFn = module->getFunction("_HeapAudit.reportLeaks_u64__");
        if (!enableFn || !reportFn)
        {
            LogErrorMessage("--heap-audit: HeapAudit.enable/reportLeaks were not linked - "
                            "diagnostic/heap_audit.cb failed to import; cannot instrument main.");
            return;
        }

        // Leave a self-auditing program untouched: if it already calls enable()/reportLeaks()
        // it audits at its own quiescent points, and injecting an earlier enable() would widen
        // the tracked set and break its own reportLeaks()==0 assertions. The flag is for
        // programs that do not instrument themselves.
        if (enableFn->getNumUses() > 0 || reportFn->getNumUses() > 0)
        {
            if (verbose)
                std::cout << "[verbose] --heap-audit: program already audits itself; "
                             "leaving main uninstrumented\n";
            return;
        }

        // Under -g every call in a function with debug info needs a !dbg location or the
        // verifier rejects it. Reuse a nearby instruction's location; fall back to the
        // subprogram scope (the prologue) when none is available. Empty when not -g.
        llvm::DISubprogram* sp = mainFn->getSubprogram();
        auto debugLocNear = [&](llvm::Instruction* anchor) -> llvm::DebugLoc {
            if (anchor && anchor->getDebugLoc())
                return anchor->getDebugLoc();
            if (sp)
                return llvm::DILocation::get(*context, sp->getLine(), 0, sp);
            return llvm::DebugLoc();
        };

        // enable() before the first real instruction of the entry block.
        llvm::BasicBlock& entry = mainFn->getEntryBlock();
        llvm::BasicBlock::iterator entryIp = entry.getFirstInsertionPt();
        {
            llvm::IRBuilder<> b(&entry, entryIp);
            b.SetCurrentDebugLocation(debugLocNear(entryIp != entry.end() ? &*entryIp : nullptr));
            b.CreateCall(enableFn);
        }

        // reportLeaks() right before each return - the leak count is intentionally ignored
        // (report-only); reportLeaks itself prints every live allocation to stderr.
        for (llvm::BasicBlock& bb : *mainFn)
        {
            auto* ret = llvm::dyn_cast_or_null<llvm::ReturnInst>(cflat_llvm::GetTerminatorOrNull(&bb));
            if (!ret) continue;
            llvm::IRBuilder<> b(ret);
            b.SetCurrentDebugLocation(debugLocNear(ret));
            b.CreateCall(reportFn);
        }
    }

bool LLVMBackend::JitRun(int& runExitCode)
{
        runExitCode = 0;

        // ORC needs the native target + asm printer registered (EmitExecutable registers all
        // targets for cross-codegen; for in-process JIT only the host target is required).
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();

        if (!module->getFunction("main"))
        {
            LogErrorMessage("{}: no '{}' function to execute.", { "--run", "main" });
            return false;
        }

        // Determine entry signature before handing the module to the JIT (which consumes it).
        // Two prototypes: int main() and int main(int argc, char** argv).
        llvm::FunctionType* mainTy = module->getFunction("main")->getFunctionType();
        unsigned mainParamCount = mainTy->getNumParams();
        bool mainTakesArgv = false;
        if (mainParamCount == 0)
        {
            // int main()
        }
        else if (mainParamCount == 2 && mainTy->getParamType(0)->isIntegerTy() &&
                 mainTy->getParamType(1)->isPointerTy())
        {
            mainTakesArgv = true; // int main(int argc, char** argv)
        }
        else
        {
            LogErrorMessage("{}: '{}' must be '{}' or '{}' to be executed in-process.",
                            { "--run", "main", "int main()", "int main(int argc, char** argv)" });
            return false;
        }
        if (!mainTakesArgv && !runArgs_.empty())
        {
            LogErrorMessage("{}: program arguments were supplied after '{}', but '{}' takes no "
                            "parameters. Declare '{}' to receive them.",
                            { "--run", "--", "main", "int main(int argc, char** argv)" });
            return false;
        }

        // Multi-threaded --run is supported: the JITLink object-linking layer plus
        // SehRegistrationPlugin (see the LLJITBuilder setup below) register .pdata unwind tables
        // for the JIT'd image, so hardware faults on worker threads dispatch correctly (e.g. the
        // 'program' construct's SEH crash isolation). ReachesThreadSpawn() is retained for
        // diagnostics but no longer gates execution.

        // Build an LLJIT for the host. JITTargetMachineBuilder::detectHost picks up the host
        // triple, CPU, and features so the JIT'd code matches this machine.
        auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
        if (!jtmb)
        {
            LogErrorMessage("{}: could not detect host machine: {}",
                            { "--run", llvm::toString(jtmb.takeError()) });
            return false;
        }

#if defined(__APPLE__)
        // Force emulated TLS for the in-process JIT on Darwin. The host arm64-apple target
        // otherwise lowers `thread_local` to a native Mach-O TLV descriptor whose resolver thunk
        // is only bootstrapped by dyld (or an ORC MachOPlatform + orc_rt, which we do not link).
        // In the bare LLJIT that thunk is unresolved, so the first thread-local access (e.g.
        // __prog_tls inside printf's format path) recurses through the stub and overflows the
        // stack. Emulated TLS routes every access through __emutls_get_address instead, which we
        // define below - matching how the Windows --run path resolves thread-locals.
        jtmb->getOptions().EmulatedTLS = true;
#endif

        // On Windows, force the JITLink object-linking layer (instead of the default RuntimeDyld)
        // and attach SehRegistrationPlugin. RuntimeDyld does not register .pdata in a way the OS
        // exception dispatcher finds, so a hardware fault on a JIT'd WORKER thread (e.g. the
        // 'program' construct's SEH crash-isolation catchpad) is never dispatched and the host
        // process hard-crashes. JITLink gives the plugin the LinkGraph's .pdata so we can call
        // RtlAddFunctionTable ourselves - this is what makes multi-threaded --run unwind-safe.
        //
        // These object-flag overrides and the SEH plugin are COFF-specific. On macOS (Mach-O)
        // setAutoClaimResponsibilityForObjectSymbols makes JITLink claim the object's *undefined*
        // reference to __emutls_get_address as if the object defined it, binding its GOT slot to a
        // self-referential stub cycle instead of our absolute host hook - the emutls call then
        // recurses until the stack overflows. Mach-O/ELF need none of this, so use the default
        // LLJIT object layer there (which is already JITLink on macOS).
        llvm::orc::LLJITBuilder jitBuilder;
        jitBuilder.setJITTargetMachineBuilder(std::move(*jtmb));
#if defined(_WIN32)
        jitBuilder.setObjectLinkingLayerCreator(
            // LLVM 21 dropped the Triple parameter from ObjectLinkingLayerCreator; LLVM 23
            // added a JITLinkMemoryManager& and removed the ES-only ObjectLinkingLayer ctor,
            // so pass through the memory manager LLJIT owns instead of making our own.
            [](llvm::orc::ExecutionSession& ES,
               llvm::jitlink::JITLinkMemoryManager& memMgr)
                -> llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>> {
                auto ol = std::make_unique<llvm::orc::ObjectLinkingLayer>(ES, memMgr);
                // Under LLJIT, the IR layer pre-computes symbol flags from the IR; JITLink
                // recomputes them from the linked COFF object and they can disagree
                // (weak/COMDAT constants), tripping ORC's "Resolving symbol with incorrect
                // flags" assert. Defer to the promised flags and claim any object-only symbols.
                ol->setOverrideObjectFlagsWithResponsibilityFlags(true);
                ol->setAutoClaimResponsibilityForObjectSymbols(true);
                ol->addPlugin(std::make_unique<cflat_jit::SehRegistrationPlugin>());
                return ol;
            });
#endif
        auto jitOrErr = jitBuilder.create();
        if (!jitOrErr)
        {
            LogErrorMessage("{}: failed to create JIT: {}",
                            { "--run", llvm::toString(jitOrErr.takeError()) });
            return false;
        }
        std::unique_ptr<llvm::orc::LLJIT> jit = std::move(*jitOrErr);

        // The module's data layout must match the JIT's. Set both layout and triple to the
        // JIT's host values before handing the module over.
        module->setDataLayout(jit->getDataLayout());
        module->setTargetTriple(jit->getTargetTriple());

        // Disable builtin libcall recognition for the in-process JIT (equivalent to -fno-builtin).
        // cflat DEFINES its own hook-aware libc functions (printf/vsnprintf/memcpy/...). LLVM's
        // libcall optimizer otherwise treats those names as the standard builtins and applies
        // folds that assume standard semantics - most damagingly the _FORTIFY fold that rewrites
        // __vsnprintf_chk(buf,size,0,size,fmt,ap) into vsnprintf(buf,size,fmt,ap). Since cflat's
        // vsnprintf routes back through __vsnprintf_chk, that fold makes vsnprintf_libc recurse
        // infinitely and overflow the stack under --run. The linked -o path does not fold these,
        // so match that behavior by marking every function "no-builtins".
        for (llvm::Function& fn : *module)
            fn.addFnAttr("no-builtins");

        // Resolve external symbols (CRT, kernel32, ws2_32) from already-loaded process symbols.
        // GlobalPrefix from the data layout keeps name mangling consistent (no prefix on win64).
        auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            jit->getDataLayout().getGlobalPrefix());
        if (!gen)
        {
            LogErrorMessage("{}: failed to install process symbol resolver: {}",
                            { "--run", llvm::toString(gen.takeError()) });
            return false;
        }
        jit->getMainJITDylib().addGenerator(std::move(*gen));

        // Supply __emutls_get_address - emulated TLS is how the host target lowers thread-locals.
        // runtime.cb allocator hooks are thread-local, so essentially every program needs this.
        {
            llvm::orc::SymbolMap syms;
            syms[jit->mangleAndIntern("__emutls_get_address")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&cflat_jit::CflatEmutlsGetAddress),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            if (auto err = jit->getMainJITDylib().define(
                    llvm::orc::absoluteSymbols(std::move(syms))))
            {
                LogErrorMessage("{}: failed to define {}: {}",
                                { "--run", "__emutls_get_address", llvm::toString(std::move(err)) });
                return false;
            }
        }

        // Imported C sources are compiled to native objects during the import walk. The AOT
        // linker consumes cObjectFiles_, while --run must add the same objects to the ORC
        // JITDylib explicitly. Prebuilt libraries are rejected before reaching this path.
        for (const auto& cObj : cObjectFiles_)
        {
            auto objOrErr = llvm::MemoryBuffer::getFile(cObj);
            if (!objOrErr)
            {
                LogErrorMessage("{}: could not read imported C object '{}': {}",
                                { "--run", cObj, objOrErr.getError().message() });
                for (const auto& path : cObjectFiles_)
                    llvm::sys::fs::remove(path);
                return false;
            }
            if (auto err = jit->addObjectFile(std::move(*objOrErr)))
            {
                LogErrorMessage("{}: could not load imported C object '{}': {}",
                                { "--run", cObj, llvm::toString(std::move(err)) });
                for (const auto& path : cObjectFiles_)
                    llvm::sys::fs::remove(path);
                return false;
            }
        }
        // addObjectFile owns the object bytes after this point, so the compiler's temporary
        // files are no longer needed. The JIT keeps its in-memory copies alive for execution.
        for (const auto& cObj : cObjectFiles_)
            llvm::sys::fs::remove(cObj);

        // Hand the module (and its context) to the JIT. After this the backend's module is
        // consumed - fine, since --run executes and the process exits.
        llvm::orc::ThreadSafeContext tsc(std::move(context));
        if (auto err = jit->addIRModule(
                llvm::orc::ThreadSafeModule(std::move(module), std::move(tsc))))
        {
            LogErrorMessage("{}: failed to add module to JIT: {}",
                            { "--run", llvm::toString(std::move(err)) });
            return false;
        }

        // Run static initializers (llvm.global_ctors / .CRT$XCU) before main.
        if (auto err = jit->initialize(jit->getMainJITDylib()))
        {
            LogErrorMessage("{}: static initializer execution failed: {}",
                            { "--run", llvm::toString(std::move(err)) });
            return false;
        }

        auto mainSym = jit->lookup("main");
        if (!mainSym)
        {
            LogErrorMessage("{}: could not find '{}': {}",
                            { "--run", "main", llvm::toString(mainSym.takeError()) });
            return false;
        }

        char jitDiagBuf[16]; size_t jitDiagLen = 0;
        if (getenv_s(&jitDiagLen, jitDiagBuf, sizeof(jitDiagBuf), "CFLAT_JIT_DIAG") == 0 && jitDiagLen > 0)
            fprintf(stderr, "[jitdiag] invoking JIT main @%p\n", (void*)mainSym->getValue());

        // Dispatch on the entry signature detected above.
        if (mainTakesArgv)
        {
            // Build a C-style argv: argv[0] = source file name, argv[1..] = user args, argv[argc] = NULL.
            // std::string::data() is NUL-terminated (C++11+), so each element is a valid C string.
            std::vector<std::string> argvStorage;
            argvStorage.reserve(runArgs_.size() + 1);
            argvStorage.push_back(sourceFileName.empty() ? std::string("program") : sourceFileName);
            for (const auto& a : runArgs_)
                argvStorage.push_back(a);

            std::vector<char*> argv;
            argv.reserve(argvStorage.size() + 1);
            for (auto& s : argvStorage)
                argv.push_back(s.data());
            argv.push_back(nullptr);

            auto mainPtr = mainSym->toPtr<int (*)(int, char**)>();
            runExitCode = mainPtr(static_cast<int>(argvStorage.size()), argv.data());
        }
        else
        {
            auto mainPtr = mainSym->toPtr<int (*)()>();
            runExitCode = mainPtr();
        }

        // Run static destructors (atexit-style) registered via the JIT.
        if (auto err = jit->deinitialize(jit->getMainJITDylib()))
            llvm::consumeError(std::move(err)); // best-effort; program already ran

        return true;
    }

bool LLVMBackend::ResolveAsanRuntime(const std::string& arch, const std::string& msvcLibDir,
                            std::vector<std::string>& linkArgStrs, std::string& dllSrcOut)
{
        namespace fs = std::filesystem;
        if (msvcLibDir.empty())
        {
            LogErrorMessage("{}: MSVC lib directory not found, cannot locate the asan runtime "
                            "(clang_rt.asan_dynamic). Ensure Visual Studio with the C++ "
                            "AddressSanitizer component is installed.", { "--asan" });
            return false;
        }

        const std::string suffix = (arch == "x86") ? "i386" : "x86_64";
        const std::string dynLib   = "clang_rt.asan_dynamic-" + suffix + ".lib";
        const std::string thunkLib = "clang_rt.asan_dynamic_runtime_thunk-" + suffix + ".lib";

        fs::path libDir(msvcLibDir);
        for (const std::string& lib : { dynLib, thunkLib })
        {
            std::error_code ec;
            if (!fs::exists(libDir / lib, ec))
            {
                LogErrorMessage("{}: asan runtime import library '{}' not found in "
                                     "'{}'. Install the 'C++ AddressSanitizer' component for "
                                     "this MSVC toolset.", { "--asan", lib, msvcLibDir });
                return false;
            }
        }

        // libDir is already on the /libpath, so reference the libs by name. The thunk must be
        // pulled in whole so its interceptor init records are not GC'd by /OPT:REF.
        linkArgStrs.push_back(dynLib);
        linkArgStrs.push_back(thunkLib);
        linkArgStrs.push_back("/wholearchive:" + thunkLib);

        // The DLL lives under the MSVC version root: msvcLibDir is <ver>/lib/<arch>, so the
        // version root is two levels up; the DLL is at <ver>/bin/Host{x64,x86}/<arch>/.
        const std::string dllName = "clang_rt.asan_dynamic-" + suffix + ".dll";
        fs::path verRoot = libDir.parent_path().parent_path(); // <ver>/lib/<arch> -> <ver>
        for (const char* host : { "Hostx64", "Hostx86" })
        {
            fs::path candidate = verRoot / "bin" / host / arch / dllName;
            std::error_code ec;
            if (fs::exists(candidate, ec))
            {
                dllSrcOut = candidate.string();
                return true;
            }
        }

        LogErrorMessage("{}: asan runtime DLL '{}' not found under '{}\\bin'. The "
                             "import library was present but the matching runtime DLL is "
                             "missing; reinstall the 'C++ AddressSanitizer' component.",
                             { "--asan", dllName, verRoot.string() });
        return false;
    }

void LLVMBackend::CopyCRuntimeDlls(const std::string& exePath)
{
        namespace fs = std::filesystem;
        auto exeDir = fs::path(exePath).parent_path();
        // Track dest filenames to avoid duplicate copies when the legacy probe and vcpkg
        // authoritative list resolve to the same DLL for a given --c-lib.
        std::unordered_set<std::string> copiedDestNames;
        for (const auto& lib : cLinkLibs_)
        {
            fs::path libPath(lib);
            std::string stem = libPath.stem().string(); // e.g. "libcurl" or "libcurl_imp"
            // Strip a trailing "_imp" (MSVC import-lib convention) to recover the DLL stem.
            if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, "_imp") == 0)
                stem = stem.substr(0, stem.size() - 4);

            std::vector<fs::path> dirs = { libPath.parent_path() };
            auto binSibling = libPath.parent_path().parent_path() / "bin";
            dirs.push_back(binSibling);

            std::vector<std::string> stems = { stem };
            if (stem.rfind("lib", 0) == 0) stems.push_back(stem.substr(3)); // libcurl -> curl

            bool copied = false;
            for (const auto& dir : dirs)
            {
                for (const auto& s : stems)
                {
                    fs::path dll = dir / (s + ".dll");
                    std::error_code ec;
                    if (fs::exists(dll, ec))
                    {
                        fs::path dest = exeDir / dll.filename();
                        fs::copy_file(dll, dest, fs::copy_options::overwrite_existing, ec);
                        if (!ec)
                        {
                            copiedDestNames.insert(dest.filename().string());
                            if (verbose) std::cout << std::format("[verbose]   copied runtime DLL: {} -> {}\n", dll.string(), dest.string());
                            copied = true;
                        }
                        break;
                    }
                }
                if (copied) break;
            }
            if (!copied && verbose)
                std::cout << std::format("[verbose]   no runtime DLL found for {} (static lib?)\n", lib);
        }

        // vcpkg-resolved DLL paths are authoritative (from vcpkg_installed/<triplet>/bin).
        // Skip ones the legacy probe already copied.
        for (const auto& dll : vcpkgRuntimeDlls_)
        {
            fs::path src(dll);
            std::error_code ec;
            if (!fs::exists(src, ec)) continue;
            fs::path dest = exeDir / src.filename();
            if (copiedDestNames.count(dest.filename().string())) continue;
            fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
            if (!ec && verbose)
                std::cout << std::format("[verbose]   copied vcpkg DLL: {} -> {}\n", src.string(), dest.string());
        }

        // Deploy a package-nuget `pri "..."` file as <exe>.pri (WinUI MRT probes resources.pri
        // and <exe>.pri beside an unpackaged exe; <exe>.pri lets several exes share a folder).
        if (!deployPriPath_.empty())
        {
            std::error_code ec;
            fs::path src(deployPriPath_);
            fs::path dest = exeDir / (fs::path(exePath).stem().string() + ".pri");
            if (!fs::exists(src, ec))
                LogErrorMessage("failed to deploy pri: source '{}' no longer exists", { deployPriPath_ });
            else
            {
                fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
                if (ec)
                    LogErrorMessage("failed to deploy pri '{}' to '{}': {}",
                                    { deployPriPath_, dest.string(), ec.message() });
                else if (verbose)
                    std::cout << std::format("[verbose]   copied pri: {} -> {}\n", src.string(), dest.string());
            }
        }
    }

LLVMBackend::Operation LLVMBackend::ParseOperation(const std::string& operationText)
{
        if (operationText == "+") { return Operation::Add; }
        else if (operationText == "*") { return Operation::Multiply; }
        else if (operationText == "-") { return Operation::Subtract; }
        else if (operationText == "/") { return Operation::Divide; }
        else if (operationText == "%") { return Operation::Modulo; }
        else if (operationText == "==") { return Operation::Equal; }
        else if (operationText == "!=") { return Operation::NotEqual; }
        else if (operationText == ">") { return Operation::Greater; }
        else if (operationText == ">=") { return Operation::GreaterEqual; }
        else if (operationText == "<") { return Operation::Less; }
        else if (operationText == "<=") { return Operation::LessEqual; }
        else if (operationText == "*=") { return Operation::MultiplyAssignment; }
        else if (operationText == "/=") { return Operation::DivideAssignment; }
        else if (operationText == "%=") { return Operation::ModAssignment; }
        else if (operationText == "+=") { return Operation::AddAssignment; }
        else if (operationText == "-=") { return Operation::MinusAssignment; }
        else if (operationText == "<<") { return Operation::ShiftLeft; }
        else if (operationText == ">>") { return Operation::ShiftRight; }
        else if (operationText == "&") { return Operation::BitwiseAnd; }
        else if (operationText == "^") { return Operation::BitwiseXor; }
        else if (operationText == "|") { return Operation::BitwiseOr; }
        else if (operationText == "<<=") { return Operation::LeftShiftAssignment; }
        else if (operationText == ">>=") { return Operation::RightShiftAssignment; }
        else if (operationText == "&=") { return Operation::AndAssignment; }
        else if (operationText == "^=") { return Operation::XorAssignment; }
        else if (operationText == "|=") { return Operation::OrAssignment; }

        LogErrorMessage("unknown operation '{}'", { operationText });
        return Operation::None;
    }
