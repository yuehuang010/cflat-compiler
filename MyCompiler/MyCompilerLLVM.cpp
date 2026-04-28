#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include <llvm\IR\IRBuilder.h>
#include <llvm\IR\LLVMContext.h>
#include <llvm\IR\Module.h>
#include <llvm\IR\Verifier.h>
#include <llvm\Bitcode\BitcodeWriter.h>
#include <llvm\Passes\PassBuilder.h>
#pragma warning(pop)
#include <antlr4-runtime.h>

#include "CFlatParser.h"
#include "CFlatLexer.h"
#include "CFlatBaseListener.h"
#include "MyCompilerLLVM.h"
#include "MyListener.h"
#include <filesystem>

bool MyCompilerLLVM::Compile(const ArgParser& args)
{
    auto filename = args.getPositional(0).value_or("");
    sourceFileName = std::filesystem::path(filename).filename().string();
    importedFiles.insert(std::filesystem::weakly_canonical(filename).string());
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

        if (verbose) std::cout << "[verbose] macros: __FILE__ = \"" << sourceFileName << "\", __PLATFORM__ = " << platformValue << "\n";
    }

    // Auto-import order: interfaces → program → runtime → string.
    // program.cb declares __active_allocator; runtime.cb references it, so program must come first.
    if (!runtimeDir.empty())
    {
        auto interfacesPath = std::filesystem::path(runtimeDir) / "core" / "interfaces.cb";
        if (verbose) std::cout << "[verbose] auto-importing interfaces: " << interfacesPath.string() << "\n";
        if (std::filesystem::exists(interfacesPath))
            CompileImportedFile(interfacesPath.string(), "interfaces.cb");
        else if (verbose)
            std::cout << "[verbose]   interfaces.cb not found, skipping\n";

        auto programPath = std::filesystem::path(runtimeDir) / "core" / "program.cb";
        if (verbose) std::cout << "[verbose] auto-importing program: " << programPath.string() << "\n";
        if (std::filesystem::exists(programPath))
            CompileImportedFile(programPath.string(), "program.cb");
        else if (verbose)
            std::cout << "[verbose]   program.cb not found, skipping\n";

        if (!skipRuntimeImport) {
            auto runtimePath = std::filesystem::path(runtimeDir) / "core" / "runtime.cb";
            if (verbose) std::cout << "[verbose] auto-importing runtime: " << runtimePath.string() << "\n";
            if (std::filesystem::exists(runtimePath))
                CompileImportedFile(runtimePath.string(), "runtime.cb");
            else if (verbose)
                std::cout << "[verbose]   runtime.cb not found, skipping\n";
        }

        auto stringPath = std::filesystem::path(runtimeDir) / "core" / "string.cb";
        if (verbose) std::cout << "[verbose] auto-importing string: " << stringPath.string() << "\n";
        if (std::filesystem::exists(stringPath))
            CompileImportedFile(stringPath.string(), "string.cb");
        else if (verbose)
            std::cout << "[verbose]   string.cb not found, skipping\n";
    }

    {
        if (verbose) std::cout << "[verbose] parsing " << filename << "\n";
        std::ifstream stream;
        stream.open(filename);
        antlr4::ANTLRInputStream input(stream);

        CFlatLexer lexer(&input);
        antlr4::CommonTokenStream tokens(&lexer);
        CFlatParser parser(&tokens);

        tokens.fill();

        auto computeUnit = parser.compilationUnit();
        if (verbose) std::cout << "[verbose]   parse complete (" << tokens.getTokens().size() << " tokens)\n";

        // Process all top-level imports before scanning the main file so that
        // imported symbols are available to ForwardRefScanner.
        if (auto* tu = computeUnit->translationUnit()) {
            for (auto* decl : tu->externalDeclaration()) {
                if (auto* imp = decl->importDeclaration()) {
                    std::string raw = imp->StringLiteral()->getText();
                    std::string importFilename = raw.substr(1, raw.size() - 2);
                    if (verbose) std::cout << "[verbose] import requested: " << importFilename << "\n";
                    if (!CompileImportedFile(filename, importFilename))
                        return false;
                }
            }
        }

        // Pre-scan: register all function signatures and struct type shells so
        // that forward references resolve during the main code-gen walk.
        {
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
        auto myListener = std::make_unique<MyListener>(&parser, this, sourceFileName);
        auto walker = antlr4::tree::ParseTreeWalker();
        walker.walk(myListener.get(), computeUnit);
        stream.close();
        if (verbose) std::cout << "[verbose]   walk complete\n";
    }

    if (verbose) std::cout << "[verbose] finalizing debug info\n";
    FinalizeDebugInfo();

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
        if (verbose) std::cout << "[verbose] writing IR to " << *lliPath << "\n";
        if (!SaveToFile(*lliPath))
        {
            std::cerr << "Error: failed to save IR to '" << *lliPath << "'.\n";
            return false;
        }
    }

    if (verbose) std::cout << "[verbose] verifying module\n";
    if (!VerifyModule())
    {
        std::cerr << "Error: module verification failed.\n";
        return false;
    }

    int optLevel = args.getOptimizationLevel();
    if (optLevel > 0)
    {
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
        if (verbose) std::cout << "[verbose] emitting executable to " << *exePath << "\n";
        if (!EmitExecutable(*exePath, platformOption))
        {
            std::cerr << "Error: failed to emit executable '" << *exePath << "'.\n";
            return false;
        }
    }

    return true;
}

bool MyCompilerLLVM::CompileImportedFile(const std::string& importingFilePath, const std::string& importFilename)
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
    if (importedFiles.count(canonicalStr))
    {
        if (verbose) std::cout << "[verbose]   already imported: " << canonicalStr << "\n";
        return true; // already imported or cycle
    }
    importedFiles.insert(canonicalStr);

    if (verbose) std::cout << "[verbose] importing: " << canonicalStr << "\n";

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
    state.input  = std::make_unique<antlr4::ANTLRInputStream>(*state.stream);
    state.lexer  = std::make_unique<CFlatLexer>(state.input.get());
    state.tokens = std::make_unique<antlr4::CommonTokenStream>(state.lexer.get());
    state.parser = std::make_unique<CFlatParser>(state.tokens.get());
    state.tokens->fill();
    auto computeUnit = state.parser->compilationUnit();
    if (verbose) std::cout << "[verbose]   parse complete (" << state.tokens->getTokens().size() << " tokens)\n";
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
                if (verbose) std::cout << "[verbose]   nested import: " << nested << "\n";
                if (!CompileImportedFile(canonicalStr, nested))
                    return false;
            }
        }
    }

    auto savedSourceFileName = sourceFileName;
    sourceFileName = std::filesystem::path(canonicalStr).filename().string();

    // Forward-ref scan the imported file
    {
        if (verbose) std::cout << "[verbose]   forward-ref scan: " << importFilename << "\n";
        ForwardRefScanner scanner(this);
        if (auto* tu = computeUnit->translationUnit())
            for (auto* decl : tu->externalDeclaration())
                scanner.ScanExternalDeclaration(decl);
    }

    // Code-gen walk the imported file
    if (verbose) std::cout << "[verbose]   code-gen walk: " << importFilename << "\n";
    auto myListener = std::make_unique<MyListener>(parserPtr, this, sourceFileName);
    antlr4::tree::ParseTreeWalker().walk(myListener.get(), computeUnit);
    if (verbose) std::cout << "[verbose]   import done: " << importFilename << "\n";

    sourceFileName = savedSourceFileName;
    return true;
}

void MyCompilerLLVM::OptimizeModule(int optimizationLevel)
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

bool MyCompilerLLVM::Analyze(const std::string& filePath,
                              const std::string& importDir,
                              const std::string& runtimeDirPath)
{
    MyListener::ClearGenericCaches();

    sourceFileName = std::filesystem::path(filePath).filename().string();
    importedFiles.insert(std::filesystem::weakly_canonical(filePath).string());
    importSearchDir = importDir;
    runtimeDir = runtimeDirPath;
    verbose = false;
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
    }

    // Auto-import core files in the same order as Compile():
    // interfaces → program (pulls in block_allocator) → runtime → string.
    // runtime.cb references BlockAllocator, which program.cb imports, so program must come first.
    if (!runtimeDir.empty())
    {
        auto interfacesPath = std::filesystem::path(runtimeDir) / "core" / "interfaces.cb";
        if (std::filesystem::exists(interfacesPath))
            CompileImportedFile(interfacesPath.string(), "interfaces.cb");

        auto programPath = std::filesystem::path(runtimeDir) / "core" / "program.cb";
        if (std::filesystem::exists(programPath))
            CompileImportedFile(programPath.string(), "program.cb");

        if (!skipRuntimeImport) {
            auto runtimePath = std::filesystem::path(runtimeDir) / "core" / "runtime.cb";
            if (std::filesystem::exists(runtimePath))
                CompileImportedFile(runtimePath.string(), "runtime.cb");
        }

        auto stringPath = std::filesystem::path(runtimeDir) / "core" / "string.cb";
        if (std::filesystem::exists(stringPath))
            CompileImportedFile(stringPath.string(), "string.cb");
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
        antlr4::ANTLRInputStream input(stream);
        CFlatLexer lexer(&input);
        antlr4::CommonTokenStream tokens(&lexer);
        CFlatParser parser(&tokens);
        tokens.fill();

        auto computeUnit = parser.compilationUnit();

        // Process top-level imports before scanning
        if (auto* tu = computeUnit->translationUnit()) {
            for (auto* decl : tu->externalDeclaration()) {
                if (auto* imp = decl->importDeclaration()) {
                    std::string raw = imp->StringLiteral()->getText();
                    std::string importFilename = raw.substr(1, raw.size() - 2);
                    if (!CompileImportedFile(filePath, importFilename))
                        return false;
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
        auto myListener = std::make_unique<MyListener>(&parser, this, sourceFileName);
        auto walker = antlr4::tree::ParseTreeWalker();
        walker.walk(myListener.get(), computeUnit);
        stream.close();
    }
    catch (CompilerAbortException&) { return false; }
    catch (ExpectedErrorReceived&)  { return false; }

    return true;
}

void MyCompilerLLVM::ResetForReanalysis()
{
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
    importedParseStates.clear();

    // Re-register the built-in string type that was wiped by the clears above.
    // string.cb references 'string' as a return type before the struct is parsed,
    // so it must exist before any core file is compiled.
    RegisterBuiltinString();
}