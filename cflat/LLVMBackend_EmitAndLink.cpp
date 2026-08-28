#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/AssemblyAnnotationWriter.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>
#include <llvm/Transforms/Utils/Cloning.h>
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
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/FormattedStream.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/IR/LegacyPassManager.h>
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
#include <fstream>
#include <cstdint>
#include <vector>

namespace {

class LineMappingAnnotationWriter final : public llvm::AssemblyAnnotationWriter
{
public:
    LineMappingAnnotationWriter(std::vector<LLVMBackend::LineMapping>& mappings,
                                 const std::function<int(const llvm::DILocation*)>& sourceLine)
        : mappings_(mappings), sourceLine_(sourceLine) {}

    void emitInstructionAnnot(const llvm::Instruction* instruction,
                              llvm::formatted_raw_ostream& stream) override
    {
        llvm::DebugLoc debugLoc = instruction->getDebugLoc();
        if (!debugLoc) return;
        int sourceLine = sourceLine_(debugLoc.get());
        if (sourceLine <= 0) return;
        int viewLine = baseLine_ + static_cast<int>(stream.getLine()) + 1;
        mappings_.push_back({sourceLine, viewLine, viewLine});
    }

    // Function::print creates a fresh formatted_raw_ostream (line counter reset to 0)
    // per call; set this to the lines already emitted before each per-function print.
    void SetBaseLine(int baseLine) { baseLine_ = baseLine; }

private:
    std::vector<LLVMBackend::LineMapping>& mappings_;
    const std::function<int(const llvm::DILocation*)>& sourceLine_;
    int baseLine_ = 0;
};

void ConsolidateLineMappings(std::vector<LLVMBackend::LineMapping>& mappings)
{
    std::vector<LLVMBackend::LineMapping> consolidated;
    for (const auto& mapping : mappings)
    {
        if (!consolidated.empty()
            && consolidated.back().srcLine == mapping.srcLine
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

static void AppendManifestResU16(std::vector<uint8_t>& bytes, uint16_t value)
{
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
}

static void AppendManifestResU32(std::vector<uint8_t>& bytes, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

static bool WriteManifestResource(const std::string& path, const std::string& xml)
{
    std::vector<uint8_t> bytes;
    auto appendHeader = [&](uint32_t dataSize, uint16_t type, uint16_t id) {
        AppendManifestResU32(bytes, dataSize);
        AppendManifestResU32(bytes, 32);
        AppendManifestResU16(bytes, 0xffff);
        AppendManifestResU16(bytes, type);
        AppendManifestResU16(bytes, 0xffff);
        AppendManifestResU16(bytes, id);
        AppendManifestResU32(bytes, 0);
        AppendManifestResU16(bytes, 0x0030);
        AppendManifestResU16(bytes, 1033);
        AppendManifestResU32(bytes, 0);
        AppendManifestResU32(bytes, 0);
    };

    appendHeader(0, 0, 0);
    appendHeader(static_cast<uint32_t>(xml.size()), 24, 1);
    bytes.insert(bytes.end(), xml.begin(), xml.end());
    while ((bytes.size() & 3u) != 0) bytes.push_back(0);

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

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

std::unique_ptr<llvm::TargetMachine> LLVMBackend::CreateOptTargetMachine()
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
        const llvm::Target* target = cflat_llvm_compat::LookupTarget(triple, err);
        if (!target)
            return nullptr;

        llvm::TargetOptions opt;
        return std::unique_ptr<llvm::TargetMachine>(
            cflat_llvm_compat::CreateTargetMachine(*target, triple, cpu, "", opt, llvm::Reloc::PIC_));
    }

bool LLVMBackend::PrintModuleView(std::string& out, const std::string& kind,
                                  bool optimized, const std::string& functionName,
                                  std::vector<LineMapping>* mappings)
{
    if (!module || (kind != "ir" && kind != "asm"))
        return false;
    if (mappings) mappings->clear();

    auto view = llvm::CloneModule(*module);
    if (!view)
        return false;

    auto matchingFunctions = [&](llvm::Module& m) {
        std::vector<llvm::Function*> matches;
        if (functionName.empty()) return matches;
        for (auto& function : m)
        {
            if (function.isDeclaration()) continue;
            llvm::StringRef name = function.getName();
            const std::string mangledPrefix = "_" + functionName + "_";
            const std::string qualifiedPrefix = "." + functionName + "_";
            if (name == functionName
                || name.starts_with(mangledPrefix)
                || name.starts_with(functionName + "__")
                || name.contains(qualifiedPrefix))
                matches.push_back(&function);
        }
        return matches;
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
        if (!optimized || preOptimizationMatches.empty() || !postMatches.empty())
            return std::string();

        std::string banner;
        for (const auto& info : preOptimizationMatches)
        {
            banner += "; function " + info.name
                + " was optimized away at O2 (inlined or removed)\n";
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
                banner += caller;
                if (count == 5) break;
            }
            banner += " - view those functions instead\n";
        }
        return banner;
    };

    std::unique_ptr<llvm::TargetMachine> targetMachine;
    if (optimized || kind == "asm")
    {
        targetMachine = CreateOptTargetMachine();
        if (!targetMachine)
            return false;
        cflat_llvm_compat::SetModuleTriple(*view, targetMachine->getTargetTriple().str());
        view->setDataLayout(targetMachine->createDataLayout());
    }

    if (optimized)
    {
        llvm::PassBuilder passBuilder(targetMachine.get());
        llvm::LoopAnalysisManager loopAnalysis;
        llvm::FunctionAnalysisManager functionAnalysis;
        llvm::CGSCCAnalysisManager cgsccAnalysis;
        llvm::ModuleAnalysisManager moduleAnalysis;
        llvm::TargetLibraryInfoImpl tli(llvm::Triple(cflat_llvm_compat::GetModuleTripleStr(*view)));
        functionAnalysis.registerPass([&] { return llvm::TargetLibraryAnalysis(tli); });
        passBuilder.registerModuleAnalyses(moduleAnalysis);
        passBuilder.registerCGSCCAnalyses(cgsccAnalysis);
        passBuilder.registerFunctionAnalyses(functionAnalysis);
        passBuilder.registerLoopAnalyses(loopAnalysis);
        passBuilder.crossRegisterProxies(loopAnalysis, functionAnalysis, cgsccAnalysis, moduleAnalysis);
        auto passes = passBuilder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
        passes.run(*view, moduleAnalysis);
    }

    const std::string rootPath = analyzedRootPath_;
    const std::string rootName = std::filesystem::path(rootPath).filename().string();
    std::string cuFileName;
    std::string cuDirectory;
    if (compileUnit && compileUnit->getFile())
    {
        cuFileName = compileUnit->getFile()->getFilename().str();
        cuDirectory = compileUnit->getFile()->getDirectory().str();
    }

    auto isRootFile = [&](const llvm::DIFile* file) {
        if (!file) return false;
        const std::string filename = file->getFilename().str();
        const std::string directory = file->getDirectory().str();
        if (!cuFileName.empty() && filename == cuFileName && directory == cuDirectory)
            return true;
        if (!rootPath.empty())
        {
            std::filesystem::path candidate = std::filesystem::path(directory) / filename;
            std::error_code ec;
            if (std::filesystem::weakly_canonical(candidate, ec).string() == rootPath)
                return true;
            if (filename == rootName)
                return true;
        }
        return false;
    };

    std::function<int(const llvm::DILocation*)> sourceLine = [&](const llvm::DILocation* location) {
        for (auto* current = location; current; current = current->getInlinedAt())
            if (isRootFile(current->getFile()))
                return static_cast<int>(current->getLine());
        return 0;
    };

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
            auto mappingView = llvm::CloneModule(*view);
            if (mappingView)
            {
                for (auto& function : *mappingView)
                    for (auto& block : function)
                        for (auto it = block.begin(); it != block.end(); )
                        {
                            auto current = it++;
                            if (llvm::isa<llvm::DbgInfoIntrinsic>(&*current))
                                current->eraseFromParent();
                        }
                std::string mappingText;
                llvm::raw_string_ostream mappingStream(mappingText);
                LineMappingAnnotationWriter writer(*mappings, sourceLine);
                if (functionName.empty())
                    mappingView->print(mappingStream, &writer);
                else
                    for (auto* function : matchingFunctions(*mappingView))
                    {
                        mappingStream << "; function: " << function->getName() << "\n";
                        mappingStream.flush();
                        writer.SetBaseLine((int)std::count(mappingText.begin(), mappingText.end(), '\n'));
                        function->print(mappingStream, &writer);
                        mappingStream << "\n";
                    }
                mappingStream.flush();
            }
            llvm::StripDebugInfo(*view);
        }

        llvm::raw_string_ostream stream(out);
        if (functionName.empty())
            view->print(stream, nullptr);
        else
            for (auto* function : matches)
            {
                stream << "; function: " << function->getName() << "\n";
                function->print(stream, nullptr);
                stream << "\n";
            }
        stream.flush();
        if (mappings) ConsolidateLineMappings(*mappings);
        return true;
    }

    llvm::legacy::PassManager pass;
    llvm::SmallString<0> buffer;
    llvm::raw_svector_ostream stream(buffer);
    if (targetMachine->addPassesToEmitFile(pass, stream, nullptr,
                                           llvm::CodeGenFileType::AssemblyFile))
        return false;
    pass.run(*view);
    std::string assembly(buffer.data(), buffer.size());

    auto parseQuotedStrings = [](const std::string& line, size_t pos) {
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
    };
    auto parseFileDirective = [&](const std::string& line, int& id,
                                  std::vector<std::string>& values) {
        size_t pos = 0;
        while (pos < line.size() && std::isspace((unsigned char)line[pos])) ++pos;
        if (line.compare(pos, 5, ".file") != 0) return false;
        pos += 5;
        if (pos < line.size() && !std::isspace((unsigned char)line[pos])) return false;
        while (pos < line.size() && std::isspace((unsigned char)line[pos])) ++pos;
        size_t start = pos;
        while (pos < line.size() && std::isdigit((unsigned char)line[pos])) ++pos;
        if (start == pos) return false;
        id = std::stoi(line.substr(start, pos - start));
        values = parseQuotedStrings(line, pos);
        return !values.empty();
    };
    auto parseLocDirective = [&](const std::string& line, int& id, int& sourceLine) {
        size_t pos = 0;
        while (pos < line.size() && std::isspace((unsigned char)line[pos])) ++pos;
        if (line.compare(pos, 4, ".loc") != 0) return false;
        pos += 4;
        if (pos < line.size() && !std::isspace((unsigned char)line[pos])) return false;
        auto nextInt = [&](int& value) {
            while (pos < line.size() && std::isspace((unsigned char)line[pos])) ++pos;
            size_t start = pos;
            while (pos < line.size() && std::isdigit((unsigned char)line[pos])) ++pos;
            if (start == pos) return false;
            value = std::stoi(line.substr(start, pos - start));
            return true;
        };
        return nextInt(id) && nextInt(sourceLine);
    };
    auto isRootAsmFile = [&](const std::vector<std::string>& values) {
        if (values.empty() || rootName.empty()) return false;
        std::filesystem::path candidate;
        if (values.size() == 1)
            candidate = values[0];
        else
            candidate = std::filesystem::path(values[0]) / values[1];
        std::error_code ec;
        if (!rootPath.empty() && std::filesystem::weakly_canonical(candidate, ec).string() == rootPath)
            return true;
        return candidate.filename().string() == rootName;
    };
    std::set<int> rootAsmFileIds;
    {
        size_t lineStart = 0;
        while (lineStart < assembly.size())
        {
            size_t lineEnd = assembly.find('\n', lineStart);
            if (lineEnd == std::string::npos) lineEnd = assembly.size();
            int id = 0;
            std::vector<std::string> values;
            std::string line = assembly.substr(lineStart, lineEnd - lineStart);
            if (parseFileDirective(line, id, values) && isRootAsmFile(values))
                rootAsmFileIds.insert(id);
            lineStart = lineEnd == assembly.size() ? assembly.size() : lineEnd + 1;
        }
    }

    auto appendAsmMappings = [&](const std::string& text) {
        if (!mappings || rootAsmFileIds.empty()) return;
        size_t lineStart = 0;
        int lineNumber = 0;
        int activeSourceLine = 0;
        int activeStart = 0;
        auto finish = [&](int endLine) {
            if (activeSourceLine > 0 && activeStart <= endLine)
                mappings->push_back({activeSourceLine, activeStart, endLine});
            activeSourceLine = 0;
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
            int id = 0;
            int source = 0;
            if (parseLocDirective(line, id, source))
            {
                finish(lineNumber - 1);
                if (rootAsmFileIds.contains(id) && source > 0)
                {
                    activeSourceLine = source;
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


bool LLVMBackend::EmitExecutableElf(const std::string& exePath, bool debugInfo,
                           const std::optional<std::string>& lliPath)
{
        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmPrinters();

        const std::string triple = llvm::sys::getProcessTriple(); // e.g. x86_64-unknown-linux-gnu
        cflat_llvm_compat::SetModuleTriple(*module, triple);

        std::string err;
        const llvm::Target* target = cflat_llvm_compat::LookupTarget(triple, err);
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
            cflat_llvm_compat::CreateTargetMachine(*target, triple, cpu, "", opt, llvm::Reloc::PIC_));
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
        cflat_llvm_compat::SetModuleTriple(*module, triple);

        std::string err;
        const llvm::Target* target = cflat_llvm_compat::LookupTarget(triple, err);
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
            cflat_llvm_compat::CreateTargetMachine(*target, triple, cpu, "", opt, llvm::Reloc::PIC_));
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
        // macOS arm64 cross-target: Mach-O object emission, independent of host OS
        // (handled before the host-specific COFF/ELF split below).
        if (targetMacOS_)
            return EmitExecutableMachO(exePath, debugInfo, lliPath);
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

        cflat_llvm_compat::SetModuleTriple(*module, triple);

        std::string err;
        const llvm::Target* target = cflat_llvm_compat::LookupTarget(triple, err);
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
            cflat_llvm_compat::CreateTargetMachine(*target, triple, cpu, "", opt, llvm::Reloc::PIC_));
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

        std::vector<std::string> manifestTempFiles;
        auto cleanupManifestFiles = [&]() {
            for (const auto& path : manifestTempFiles)
                llvm::sys::fs::remove(path);
            manifestTempFiles.clear();
        };
        if (!manifestFragments_.empty())
        {
            auto merged = MergeManifestFragments();
            if (!merged || !ValidateManifestActivationContext(*merged))
            {
                llvm::sys::fs::remove(objPath);
                for (auto& cObj : cObjectFiles_) llvm::sys::fs::remove(cObj);
                return false;
            }

            llvm::SmallString<256> resourcePath;
            if (auto ec = llvm::sys::fs::createTemporaryFile("cflat_manifest", "res", resourcePath))
            {
                LogError(std::format("could not create temporary manifest resource: {}", ec.message()));
                llvm::sys::fs::remove(objPath);
                for (auto& cObj : cObjectFiles_) llvm::sys::fs::remove(cObj);
                return false;
            }
            std::string path = resourcePath.str().str();
            std::string document = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                + *merged;
            if (!WriteManifestResource(path, document))
            {
                llvm::sys::fs::remove(path);
                LogError(std::format("could not write temporary manifest resource '{}'", path));
                llvm::sys::fs::remove(objPath);
                for (auto& cObj : cObjectFiles_) llvm::sys::fs::remove(cObj);
                return false;
            }
            manifestTempFiles.push_back(path);
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
            cleanupManifestFiles();

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
            auto* ret = llvm::dyn_cast_or_null<llvm::ReturnInst>(cflat_llvm_compat::GetTerminatorOrNull(&bb));
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
            // LLVM 21 dropped the Triple parameter from ObjectLinkingLayerCreator; the
            // trailing pack makes the lambda convertible to either signature.
            [](llvm::orc::ExecutionSession& ES, auto&&...)
                -> llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>> {
                auto ol = std::make_unique<llvm::orc::ObjectLinkingLayer>(ES);
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
        cflat_llvm_compat::SetModuleTriple(*module, jit->getTargetTriple().str());

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
