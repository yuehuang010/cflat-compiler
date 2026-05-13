#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include <llvm\IR\IRBuilder.h>
#include <llvm\IR\LLVMContext.h>
#include <llvm\IR\Module.h>
#include <llvm\IR\Verifier.h>
#include <llvm\Bitcode\BitcodeWriter.h>
#include <llvm\Passes\PassBuilder.h>
#include <llvm\Transforms\Utils\Mem2Reg.h>
#include <llvm\Transforms\Scalar\SROA.h>
#include <llvm\Transforms\InstCombine\InstCombine.h>
#include <llvm\Transforms\Scalar\SimplifyCFG.h>
#include <llvm\Support\TimeProfiler.h>
#pragma warning(pop)
#include <antlr4-runtime.h>

#include "CFlatParser.h"
#include "CFlatLexer.h"
#include "CFlatBaseListener.h"
#include "LLVMBackend.h"
#include "MainListener.h"
#include <filesystem>

static std::vector<std::string> ReadFileToLines(std::ifstream& stream)
{
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(stream, line))
        lines.push_back(line);
    stream.clear();
    stream.seekg(0);
    return lines;
}

void LLVMBackend::ReportParseErrors(const std::vector<ParseDiagnostic>& diagnostics,
                                       const std::vector<std::string>& sourceLines)
{
    for (const auto& d : diagnostics)
    {
        if (diagnosticSink_)
        {
            std::string msg = d.message;
            if (!d.hint.empty())
                msg += "\nhint: " + d.hint;
            diagnosticSink_(d.file, static_cast<size_t>(d.line), static_cast<size_t>(d.col), msg);
            // No throw — parse errors are pre-codegen; caller returns false.
        }
        else
        {
            std::cerr << std::format("{}({},{}): error: {}\n", d.file, d.line, d.col, d.message);

            int lineIdx = d.line - 1;
            if (lineIdx >= 0 && lineIdx < static_cast<int>(sourceLines.size()))
            {
                const std::string& srcLine = sourceLines[lineIdx];
                std::cerr << "    " << srcLine << "\n";
                std::string caret(4 + std::min(d.col, static_cast<int>(srcLine.size())), ' ');
                caret += '^';
                std::cerr << caret << "\n";
            }

            if (!d.hint.empty())
                std::cerr << "hint: " << d.hint << "\n";
        }
    }
}

bool LLVMBackend::Compile(const ArgParser& args)
{
    auto filename = args.getPositional(0).value_or("");
    sourceFileName = std::filesystem::path(filename).filename().string();
    llvm::TimeTraceScope compileScope("Compilation", sourceFileName);
    auto rootCanonical = std::filesystem::weakly_canonical(filename).string();
    currentSourceFilePath_ = rootCanonical;
    importedFiles.insert(rootCanonical);
    importStack.push_back(rootCanonical);
    struct RootGuard {
        std::vector<std::string>& stack;
        ~RootGuard() { stack.pop_back(); }
    } rootGuard{importStack};
    auto exePath = args.getOption("output");
    auto lliPath = args.getOption("out-lli");
    auto bitcodePath = args.getOption("bitcode").value_or("");
    bool debugInfo = args.hasFlag("debug-info");
    importSearchDir = args.getOption("import-dir").value_or("");
    auto platformOption = args.getOption("platform").value_or("win64");

    if (verbose)
    {
        std::cout << "[verbose] input:        " << filename << "\n";
        std::cout << "[verbose] output exe:   " << (exePath ? *exePath : "(none)") << "\n";
        std::cout << "[verbose] output lli:   " << (lliPath ? *lliPath : "(none)") << "\n";
        std::cout << "[verbose] runtime dir:  " << runtimeDir << "\n";
        std::cout << "[verbose] import dir:   " << (importSearchDir.empty() ? "(none)" : importSearchDir) << "\n";
        std::cout << "[verbose] debug info:   " << (debugInfo ? "yes" : "no") << "\n";
    }

    if (exePath)
    {
        auto exeDir = std::filesystem::path(*exePath).parent_path();
        if (!exeDir.empty() && !std::filesystem::exists(exeDir))
        {
            std::cerr << "Error: output directory '" << exeDir.string() << "' does not exist (-o " << *exePath << ").\n";
            return false;
        }
    }

    if (lliPath)
    {
        auto lliDir = std::filesystem::path(*lliPath).parent_path();
        if (!lliDir.empty() && !std::filesystem::exists(lliDir))
        {
            std::cerr << "Error: output directory '" << lliDir.string() << "' does not exist (--out-lli " << *lliPath << ").\n";
            return false;
        }
    }

    if (!std::filesystem::exists(filename))
    {
        std::cerr << "Error: input file '" << filename << "' does not exist.\n";
        return false;
    }

    if (debugInfo)
    {
        std::filesystem::path filePath = std::filesystem::absolute(filename);
        InitDebugInfo(filePath.filename().string(), filePath.parent_path().string());
    }

    // Set the platform constant (__PLATFORM__) based on the target platform.
    // This is a compile-time constant available in all compiled files.
    platformValue = (platformOption == "win32") ? 32 : 64;
    if (verbose) std::cout << "[verbose] __PLATFORM__ = " << platformValue << "\n";

    // Set the module data layout now so sizeof/alignof produce correct sizes during
    // codegen (the final target-machine layout is set again in EmitExecutable).
    {
        // Win32: 32-bit pointers; Win64: 64-bit pointers. Both little-endian MSVC.
        const char* dl = (platformValue == 32)
            ? "e-m:x-p:32:32-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:32-n8:16:32-S32"
            : "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128";
        module->setDataLayout(llvm::DataLayout(dl));
    }

    // Pre-populate compile-time macros (constants throughout compilation)
    {
        // __FILE__: source filename (create global string directly without BasicBlock)
        auto* fileGlobalStr = module->getOrInsertGlobal("__FILE__",
            llvm::ArrayType::get(llvm::Type::getInt8Ty(*context), sourceFileName.size() + 1));
        auto* fileConst = llvm::ConstantDataArray::getString(*context, sourceFileName, true);
        if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(fileGlobalStr))
        {
            gv->setInitializer(fileConst);
            gv->setConstant(true);
            SetCompileTimeMacro("__FILE__", gv, "string");
        }

        // __PLATFORM__: target platform (64 or 32)
        auto platformConst = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), platformValue);
        SetCompileTimeMacro("__PLATFORM__", platformConst, "int");

        auto win64Const = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), platformValue == 64 ? 1 : 0);
        SetCompileTimeMacro("__WIN64__", win64Const, "int");
        auto win32Const = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), platformValue == 32 ? 1 : 0);
        SetCompileTimeMacro("__WIN32__", win32Const, "int");
        auto windowsConst = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 1);
        SetCompileTimeMacro("__WINDOWS__", windowsConst, "int");

        if (verbose) std::cout << "[verbose] macros: __FILE__ = \"" << sourceFileName << "\", __PLATFORM__ = " << platformValue << "\n";
    }

    if (!runtimeDir.empty() && !skipRuntimeImport)
    {
        llvm::TimeTraceScope runtimeScope("RuntimeImport", "runtime.cb");
        auto runtimePath = std::filesystem::path(runtimeDir) / "core" / "runtime.cb";
        if (verbose) std::cout << "[verbose] auto-importing runtime: " << runtimePath.string() << "\n";
        if (std::filesystem::exists(runtimePath))
            CompileImportedFile(runtimePath.string(), "runtime.cb");
        else if (verbose)
            std::cout << "[verbose]   runtime.cb not found, skipping\n";
    }

    {
        if (verbose) std::cout << "[verbose] parsing " << filename << "\n";
        std::ifstream stream;
        stream.open(filename);
        auto sourceLines = ReadFileToLines(stream);

        antlr4::ANTLRInputStream input(stream);
        CFlatLexer lexer(&input);
        antlr4::CommonTokenStream tokens(&lexer);
        CFlatParser parser(&tokens);

        CFlatErrorListener errorListener(filename, sourceLines);
        lexer.removeErrorListeners();
        lexer.addErrorListener(&errorListener);
        parser.removeErrorListeners();
        parser.addErrorListener(&errorListener);

        CFlatParser::CompilationUnitContext* computeUnit;
        {
            llvm::TimeTraceScope parseScope("Parse", sourceFileName);
            tokens.fill();
            computeUnit = parser.compilationUnit();
        }
        if (verbose) std::cout << "[verbose]   parse complete (" << tokens.getTokens().size() << " tokens)\n";

        if (errorListener.hasErrors())
        {
            ReportParseErrors(errorListener.getDiagnostics(), sourceLines);
            return false;
        }

        // Process all top-level imports before scanning the main file so that
        // imported symbols are available to ForwardRefScanner.
        {
            llvm::TimeTraceScope importsScope("ProcessImports", sourceFileName);
            if (auto* tu = computeUnit->translationUnit()) {
                for (auto* decl : tu->externalDeclaration()) {
                    if (auto* imp = decl->importDeclaration()) {
                        std::string raw = imp->StringLiteral()->getText();
                        std::string importFilename = raw.substr(1, raw.size() - 2);
                        std::string ns = imp->Identifier() ? imp->Identifier()->getText() : "";
                        if (verbose) std::cout << "[verbose] import requested: " << importFilename << (ns.empty() ? "" : " as " + ns) << "\n";
                        if (!CompileImportedFile(filename, importFilename, ns))
                            return false;

                        // For 'import program "file.cb" as Name', rename @main to __imported_main_Name
                        if (imp->children.size() >= 2 && imp->children[1]->getText() == "program")
                        {
                            std::string alias = imp->Identifier()->getText();
                            auto* mainFn = module->getFunction("main");
                            if (!mainFn)
                            {
                                LogError(std::format("import program '{}': '{}' has no 'main' function", alias, importFilename));
                                return false;
                            }
                            mainFn->setName("__imported_main_" + alias);
                            programTable[alias].IsImportedProgram = true;
                            programTable[alias].MainFunction = mainFn;
                        }
                    }
                }
            }
        }

        // Pre-scan: register all function signatures and struct type shells so
        // that forward references resolve during the main code-gen walk.
        {
            llvm::TimeTraceScope scanScope("ForwardRefScan", sourceFileName);
            if (verbose) std::cout << "[verbose] forward-ref scan (" << sourceFileName << ")\n";
            ForwardRefScanner scanner(this);
            // First pass: pre-declare opaque types and constructors for every
            // generic instantiation found anywhere in the file (including inside
            // function bodies), so uses like Box<MyType> b = Box__MyType() resolve.
            if (auto* tu = computeUnit->translationUnit())
                for (auto* decl : tu->externalDeclaration())
                    scanner.ScanGenericTypeUses(decl);
            // Second pass: register non-generic struct shells and function signatures.
            if (auto* tu = computeUnit->translationUnit())
                for (auto* decl : tu->externalDeclaration())
                    scanner.ScanExternalDeclaration(decl);
        }

        if (verbose) std::cout << "[verbose] code-gen walk (" << sourceFileName << ")\n";
        {
            llvm::TimeTraceScope codegenScope("CodeGeneration", sourceFileName);
            auto myListener = std::make_unique<MainListener>(&parser, this, sourceFileName);
            auto walker = antlr4::tree::ParseTreeWalker();
            walker.walk(myListener.get(), computeUnit);
        }
        stream.close();
        if (verbose) std::cout << "[verbose]   walk complete\n";
    }

    {
        llvm::TimeTraceScope debugScope("FinalizeDebugInfo");
        if (verbose) std::cout << "[verbose] finalizing debug info\n";
        FinalizeDebugInfo();
    }

    // validate that the stack is empty.
    if (stackNamedVariable.size() > 0)
    {
        for (const auto& stack : stackNamedVariable)
        {
            std::cerr << "Warning: scope stack is not empty at end of compilation:\n";
            for (const auto& funcVariable : stack.functionArgument)
            {
                std::cerr << "  Function var: " << funcVariable.first << "\n";
            }
            for (const auto& namedVariable : stack.namedVariable)
            {
                std::cerr << "  namedVar var: " << namedVariable.first << "\n";
            }
        }
    }

    if (lliPath)
    {
        llvm::TimeTraceScope irScope("WriteIR", *lliPath);
        if (verbose) std::cout << "[verbose] writing IR to " << *lliPath << "\n";
        if (!SaveToFile(*lliPath))
        {
            std::cerr << "Error: failed to save IR to '" << *lliPath << "'.\n";
            return false;
        }
    }

    {
        llvm::TimeTraceScope verifyScope("VerifyModule");
        if (verbose) std::cout << "[verbose] verifying module\n";
        if (!VerifyModule())
        {
            std::cerr << "Error: module verification failed.\n";
            return false;
        }
    }

    if (!args.hasFlag("no-opt"))
    {
        llvm::TimeTraceScope baselineScope("BaselinePasses");
        if (verbose) std::cout << "[verbose] running baseline passes (sroa, mem2reg, instcombine, simplifycfg)\n";
        RunBaselinePasses();
    }

    int optLevel = args.getOptimizationLevel();
    if (optLevel > 0)
    {
        llvm::TimeTraceScope optScope("OptimizePasses", std::format("O{}", optLevel));
        if (verbose) std::cout << "[verbose] running optimizations (O" << optLevel << ")\n";
        OptimizeModule(optLevel);
    }

    if (!bitcodePath.empty())
    {
        if (verbose) std::cout << "[verbose] writing bitcode to " << bitcodePath << "\n";
        if (!WriteBitcode(bitcodePath))
        {
            std::cerr << "Error: failed to write bitcode to '" << bitcodePath << "'.\n";
            return false;
        }
    }

    if (exePath)
    {
        llvm::TimeTraceScope emitScope("EmitExecutable", *exePath);
        if (verbose) std::cout << "[verbose] emitting executable to " << *exePath << "\n";
        if (!EmitExecutable(*exePath, platformOption))
        {
            std::cerr << "Error: failed to emit executable '" << *exePath << "'.\n";
            return false;
        }
    }

    return true;
}

bool LLVMBackend::CompileImportedFile(const std::string& importingFilePath, const std::string& importFilename, const std::string& namespaceName)
{
    auto importingDir = std::filesystem::path(importingFilePath).parent_path();
    auto importPath = (importingDir / importFilename).lexically_normal();

    std::error_code ec;
    auto canonical = std::filesystem::canonical(importPath, ec);
    // For LSP analysis: the "importing" file is a temp file; try the real source directory first.
    if (ec && !sourceFileDir_.empty())
    {
        auto sourcePath = (std::filesystem::path(sourceFileDir_) / importFilename).lexically_normal();
        canonical = std::filesystem::canonical(sourcePath, ec);
    }
    if (ec && !importSearchDir.empty())
    {
        auto searchPath = (std::filesystem::path(importSearchDir) / importFilename).lexically_normal();
        canonical = std::filesystem::canonical(searchPath, ec);
    }
    // Implicit fallback: look in the "core" directory beside the runtime.
    if (ec && !runtimeDir.empty())
    {
        auto corePath = (std::filesystem::path(runtimeDir) / "core" / importFilename).lexically_normal();
        canonical = std::filesystem::canonical(corePath, ec);
    }
    if (ec)
    {
        std::cerr << "Error: imported file not found: " << importFilename
                  << " (searched relative to '" << importingDir.string() << "'"
                  << (sourceFileDir_.empty() ? "" : ", source dir '" + sourceFileDir_ + "'")
                  << (importSearchDir.empty() ? "" : ", import dir '" + importSearchDir + "'")
                  << (runtimeDir.empty() ? "" : ", runtime core '" + runtimeDir + "/core'")
                  << ").\n";
        return false;
    }
    auto canonicalStr = canonical.string();
    // Check circular import first (file currently being processed).
    auto cycleIt = std::find(importStack.begin(), importStack.end(), canonicalStr);
    if (cycleIt != importStack.end())
    {
        std::string chain;
        for (auto it = cycleIt; it != importStack.end(); ++it)
            chain += std::filesystem::path(*it).filename().string() + " -> ";
        chain += std::filesystem::path(canonicalStr).filename().string();
        LogError(std::format("Circular import detected: {}", chain));
        return false;
    }
    // Check dedup (file already fully processed or eagerly registered).
    if (importedFiles.count(canonicalStr))
    {
        if (verbose) std::cout << "[verbose]   already imported: " << canonicalStr << "\n";
        return true;
    }
    // Cross-path dedup: the same filename may resolve to different canonical paths
    // depending on which search directory finds it first (e.g. runtimeDir/core vs -i dir).
    // Check all alternate search directories so duplicates are caught across paths.
    {
        auto tryAlt = [&](const std::filesystem::path& dir) -> std::string {
            std::error_code altEc;
            auto alt = std::filesystem::canonical((dir / importFilename).lexically_normal(), altEc);
            return altEc ? "" : alt.string();
        };
        std::string altPaths[] = {
            importSearchDir.empty() ? "" : tryAlt(importSearchDir),
            runtimeDir.empty()      ? "" : tryAlt(std::filesystem::path(runtimeDir) / "core"),
        };
        for (auto& alt : altPaths)
        {
            if (!alt.empty() && alt != canonicalStr && importedFiles.count(alt))
            {
                if (verbose) std::cout << "[verbose]   already imported (alt path): " << canonicalStr << "\n";
                importedFiles.insert(canonicalStr);
                return true;
            }
        }
    }
    // Register eagerly so any re-entrant or duplicate import is caught above.
    importedFiles.insert(canonicalStr);
    importStack.push_back(canonicalStr);
    struct ImportGuard {
        std::vector<std::string>& stack;
        ~ImportGuard() { stack.pop_back(); }
    } importGuard{importStack};

    if (verbose) std::cout << "[verbose] importing: " << canonicalStr << "\n";
    llvm::TimeTraceScope importScope("ImportFile", importFilename);

    // Persist parse state for the lifetime of the compiler — generic template
    // ctx pointers from imported files must remain valid when instantiated later
    // during the main-file walk.
    ImportedParseState state;
    state.stream = std::make_unique<std::ifstream>();
    state.stream->open(canonicalStr);
    if (!state.stream->is_open())
    {
        std::cerr << "Error: failed to open imported file '" << canonicalStr << "'.\n";
        return false;
    }
    auto importSourceLines = ReadFileToLines(*state.stream);
    state.input  = std::make_unique<antlr4::ANTLRInputStream>(*state.stream);
    state.lexer  = std::make_unique<CFlatLexer>(state.input.get());
    state.tokens = std::make_unique<antlr4::CommonTokenStream>(state.lexer.get());
    state.parser = std::make_unique<CFlatParser>(state.tokens.get());

    CFlatErrorListener importErrorListener(canonicalStr, importSourceLines);
    state.lexer->removeErrorListeners();
    state.lexer->addErrorListener(&importErrorListener);
    state.parser->removeErrorListeners();
    state.parser->addErrorListener(&importErrorListener);

    CFlatParser::CompilationUnitContext* computeUnit;
    {
        llvm::TimeTraceScope parseScope("Parse", importFilename);
        state.tokens->fill();
        computeUnit = state.parser->compilationUnit();
    }
    if (verbose) std::cout << "[verbose]   parse complete (" << state.tokens->getTokens().size() << " tokens)\n";

    if (importErrorListener.hasErrors())
    {
        ReportParseErrors(importErrorListener.getDiagnostics(), importSourceLines);
        return false;
    }

    auto* parserPtr = state.parser.get();
    state.canonicalPath = canonicalStr;
    importedParseStates.push_back(std::move(state));

    // Recursively process nested imports before scanning this file's declarations
    if (auto* tu = computeUnit->translationUnit())
    {
        for (auto* decl : tu->externalDeclaration())
        {
            if (auto* imp = decl->importDeclaration())
            {
                std::string raw = imp->StringLiteral()->getText();
                std::string nested = raw.substr(1, raw.size() - 2);
                std::string nestedNs = imp->Identifier() ? imp->Identifier()->getText() : "";
                if (verbose) std::cout << "[verbose]   nested import: " << nested << (nestedNs.empty() ? "" : " as " + nestedNs) << "\n";
                if (!CompileImportedFile(canonicalStr, nested, nestedNs))
                    return false;
            }
        }
    }

    // Snapshot tables after nested imports but before this file's scan+codegen.
    // The diff after codegen identifies symbols this file (and only this file) contributed.
    // Track functions by unique mangled name so that a new overload of an existing call-name
    // (e.g. "add") is still detected as new even if a different overload already existed.
    std::unordered_set<std::string> funcUniqBefore, structsBefore, ifacesBefore, nsBefore;
    if (!namespaceName.empty())
    {
        for (auto& [_, syms] : functionTable) for (auto& s : syms) funcUniqBefore.insert(s.UniqueName);
        for (auto& [n, _] : dataStructures) structsBefore.insert(n);
        for (auto& [n, _] : interfaceTable) ifacesBefore.insert(n);
        nsBefore = namespaceTable;
    }

    auto savedSourceFileName = sourceFileName;
    auto savedSourceFilePath = currentSourceFilePath_;
    sourceFileName = std::filesystem::path(canonicalStr).filename().string();
    currentSourceFilePath_ = canonicalStr;

    // Forward-ref scan the imported file
    {
        llvm::TimeTraceScope scanScope("ForwardRefScan", importFilename);
        if (verbose) std::cout << "[verbose]   forward-ref scan: " << importFilename << "\n";
        ForwardRefScanner scanner(this);
        // Pre-declare opaque types for all generic instantiations found in the file
        // (including inside function/program bodies) before scanning declarations.
        // Without this pass, program definitions that pre-declare run(Name*, list__string)
        // fail because the opaque shell for list__string hasn't been created yet.
        if (auto* tu = computeUnit->translationUnit())
            for (auto* decl : tu->externalDeclaration())
                scanner.ScanGenericTypeUses(decl);
        if (auto* tu = computeUnit->translationUnit())
            for (auto* decl : tu->externalDeclaration())
                scanner.ScanExternalDeclaration(decl);
    }

    // Code-gen walk the imported file
    if (verbose) std::cout << "[verbose]   code-gen walk: " << importFilename << "\n";
    {
        llvm::TimeTraceScope codegenScope("CodeGeneration", importFilename);
        auto myListener = std::make_unique<MainListener>(parserPtr, this, sourceFileName);
        antlr4::tree::ParseTreeWalker().walk(myListener.get(), computeUnit);
    }
    if (verbose) std::cout << "[verbose]   import done: " << importFilename << "\n";

    // Register the file-scoped import alias.  The "$global$:<alias>" sentinel tells
    // ResolveQualifiedName and ParsePostfixExpression to resolve Alias.X → X only
    // when X was contributed by this file (membership-checked via importAliasMembers).
    if (!namespaceName.empty())
    {
        std::unordered_set<std::string> members;
        for (auto& [callName, syms] : functionTable)
            for (auto& s : syms)
                if (!funcUniqBefore.count(s.UniqueName)) { members.insert(callName); break; }
        for (auto& [n, _] : dataStructures) if (!structsBefore.count(n)) members.insert(n);
        for (auto& [n, _] : interfaceTable) if (!ifacesBefore.count(n)) members.insert(n);
        for (auto& n : namespaceTable)      if (!nsBefore.count(n))      members.insert(n);
        importAliasMembers[namespaceName] = std::move(members);
        RegisterNamespaceAlias(namespaceName, "$global$:" + namespaceName);
    }

    sourceFileName = savedSourceFileName;
    currentSourceFilePath_ = savedSourceFilePath;
    return true;
}

void LLVMBackend::RunBaselinePasses()
{
    llvm::PassBuilder PB;
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;

    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    llvm::FunctionPassManager FPM;
    FPM.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
    FPM.addPass(llvm::PromotePass());
    FPM.addPass(llvm::InstCombinePass());
    FPM.addPass(llvm::SimplifyCFGPass());

    llvm::ModulePassManager MPM;
    MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(FPM)));
    MPM.run(*module, MAM);
}

void LLVMBackend::OptimizeModule(int optimizationLevel)
{
    if (optimizationLevel == 0)
        return;

    llvm::PassBuilder PB;
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;

    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    llvm::ModulePassManager MPM;
    if (optimizationLevel == 1)
        MPM = PB.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O1);
    else if (optimizationLevel == 2)
        MPM = PB.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);

    MPM.run(*module, MAM);
}

bool LLVMBackend::Analyze(const std::string& filePath,
                              const std::string& importDir,
                              const std::string& runtimeDirPath)
{
    gts.Clear();

    sourceFileName = std::filesystem::path(filePath).filename().string();
    auto rootCanonical = std::filesystem::weakly_canonical(filePath).string();
    currentSourceFilePath_ = rootCanonical;
    importedFiles.insert(rootCanonical);
    importStack.push_back(rootCanonical);
    struct RootGuard {
        std::vector<std::string>& stack;
        ~RootGuard() { stack.pop_back(); }
    } rootGuard{importStack};
    importSearchDir = importDir;
    runtimeDir = runtimeDirPath;
    // verbose retains the value set by the LSP via SetVerbose() — left unmodified.
    bool debugInfo = false;

    platformValue = 64;

    // Pre-populate compile-time macros
    {
        auto* fileGlobalStr = module->getOrInsertGlobal("__FILE__",
            llvm::ArrayType::get(llvm::Type::getInt8Ty(*context), sourceFileName.size() + 1));
        auto* fileConst = llvm::ConstantDataArray::getString(*context, sourceFileName, true);
        if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(fileGlobalStr))
        {
            gv->setInitializer(fileConst);
            gv->setConstant(true);
            SetCompileTimeMacro("__FILE__", gv, "string");
        }
        auto platformConst = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), platformValue);
        SetCompileTimeMacro("__PLATFORM__", platformConst, "int");
        auto win64Const = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), platformValue == 64 ? 1 : 0);
        SetCompileTimeMacro("__WIN64__", win64Const, "int");
        auto win32Const = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), platformValue == 32 ? 1 : 0);
        SetCompileTimeMacro("__WIN32__", win32Const, "int");
        auto windowsConst = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 1);
        SetCompileTimeMacro("__WINDOWS__", windowsConst, "int");
    }

    if (!runtimeDir.empty())
    if (!skipRuntimeImport)
    {
        auto runtimePath = std::filesystem::path(runtimeDir) / "core" / "runtime.cb";
        if (std::filesystem::exists(runtimePath))
            CompileImportedFile(runtimePath.string(), "runtime.cb");
    }

    if (!std::filesystem::exists(filePath))
    {
        std::cerr << "Error: input file '" << filePath << "' does not exist.\n";
        return false;
    }

    try
    {
        std::ifstream stream;
        stream.open(filePath);
        auto analyzeSourceLines = ReadFileToLines(stream);

        antlr4::ANTLRInputStream input(stream);
        CFlatLexer lexer(&input);
        antlr4::CommonTokenStream tokens(&lexer);
        CFlatParser parser(&tokens);

        CFlatErrorListener analyzeErrorListener(filePath, analyzeSourceLines);
        lexer.removeErrorListeners();
        lexer.addErrorListener(&analyzeErrorListener);
        parser.removeErrorListeners();
        parser.addErrorListener(&analyzeErrorListener);

        tokens.fill();

        auto computeUnit = parser.compilationUnit();

        if (analyzeErrorListener.hasErrors())
        {
            ReportParseErrors(analyzeErrorListener.getDiagnostics(), analyzeSourceLines);
            return false;
        }

        // Process top-level imports before scanning
        if (auto* tu = computeUnit->translationUnit()) {
            for (auto* decl : tu->externalDeclaration()) {
                if (auto* imp = decl->importDeclaration()) {
                    std::string raw = imp->StringLiteral()->getText();
                    std::string importFilename = raw.substr(1, raw.size() - 2);
                    std::string ns = imp->Identifier() ? imp->Identifier()->getText() : "";
                    if (!CompileImportedFile(filePath, importFilename, ns))
                        return false;

                    // Mirror Compile()'s 'import program "file.cb" as Name' handling:
                    // rename the imported 'main' so the EmitProgramRunWrapper helper
                    // path can find it under programTable[alias].MainFunction.
                    if (imp->children.size() >= 2 && imp->children[1]->getText() == "program")
                    {
                        std::string alias = imp->Identifier() ? imp->Identifier()->getText() : "";
                        if (auto* mainFn = module->getFunction("main"))
                        {
                            mainFn->setName("__imported_main_" + alias);
                            programTable[alias].IsImportedProgram = true;
                            programTable[alias].MainFunction = mainFn;
                        }
                    }
                }
            }
        }

        // Forward-ref scan
        {
            ForwardRefScanner scanner(this);
            if (auto* tu = computeUnit->translationUnit())
                for (auto* decl : tu->externalDeclaration())
                    scanner.ScanGenericTypeUses(decl);
            if (auto* tu = computeUnit->translationUnit())
                for (auto* decl : tu->externalDeclaration())
                    scanner.ScanExternalDeclaration(decl);
        }

        // Code-gen walk
        auto myListener = std::make_unique<MainListener>(&parser, this, sourceFileName);
        auto walker = antlr4::tree::ParseTreeWalker();
        walker.walk(myListener.get(), computeUnit);
        stream.close();
    }
    catch (CompilerAbortException&) { return false; }
    catch (ExpectedErrorReceived&)  { return false; }

    return true;
}

void LLVMBackend::ResetForReanalysis()
{
    // Reset the LLVM context so named struct types from a previous analysis do
    // not survive into the next one.  Without this, two fixtures that define a
    // struct with the same name but different field types (e.g. Point{int,int}
    // then Point{float,float}) would share the stale type object, causing an
    // LLVM assertion when inserting a float constant into an int-typed slot.
    //
    // Destroy module and builder BEFORE the context: both hold internal
    // references to the context, and their destructors must not run against
    // an already-freed context object.
    module.reset();
    builder.reset();
    context = std::make_unique<llvm::LLVMContext>();
    module = std::make_unique<llvm::Module>("cflat", *context);
    builder = std::make_unique<llvm::IRBuilder<>>(*context);

    functionTable.clear();
    dataStructures.clear();
    programTable.clear();
    enumBackingTypes.clear();
    typeAliases.clear();
    interfaceTable.clear();
    interfaceParents.clear();
    globalNamedVariable.clear();
    globalVariableTypes.clear();
    namespaceTable.clear();
    stringPool.clear();
    stackNamedVariable.clear();
    namespaceAliasTable.clear();
    returnBlockTable.clear();
    compileTimeMacros.clear();
    stringLiteralLenByPtr.clear();
    strConcatRegistered = false;
    stringDtorRegistered = false;
    lambdaCounter = 0;
    expectedError.clear();
    expectedErrorScopeDepth = SIZE_MAX;
    currentFunction = nullptr;
    autoVaListAlloca = nullptr;
    returnCapture = std::nullopt;

    // Clear all import state so core files (string, runtime, etc.) are fully re-imported
    // on the next Analyze() call.
    importedFiles.clear();
    importStack.clear();
    importedParseStates.clear();

    // Generic-template state must also be cleared so prior-analysis ANTLR contexts
    // (which point into a discarded parse tree) don't survive into the next run.
    gts.Clear();

    // Re-register the built-in string type that was wiped by the clears above.
    // string.cb references 'string' as a return type before the struct is parsed,
    // so it must exist before any core file is compiled.
    RegisterBuiltinString();
}