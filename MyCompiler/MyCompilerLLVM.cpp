#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include <llvm\IR\IRBuilder.h>
#include <llvm\IR\LLVMContext.h>
#include <llvm\IR\Module.h>
#include <llvm\IR\Verifier.h>
#include <llvm\Bitcode\BitcodeWriter.h>
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

    // Auto-import the CFlat runtime (provides printf and other builtins).
    // runtime.cb must be loaded first because string.cb uses new/delete.
    if (!runtimeDir.empty())
    {
        auto runtimePath = std::filesystem::path(runtimeDir) / "core" / "runtime.cb";
        if (verbose) std::cout << "[verbose] auto-importing runtime: " << runtimePath.string() << "\n";
        if (std::filesystem::exists(runtimePath))
            CompileImportedFile(runtimePath.string(), "runtime.cb");
        else if (verbose)
            std::cout << "[verbose]   runtime.cb not found, skipping\n";

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
        auto myListener = std::make_unique<MyListener>(&parser, this);
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
        if (!EmitExecutable(*exePath, args.getOption("platform").value_or("x64")))
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
    auto myListener = std::make_unique<MyListener>(parserPtr, this);
    antlr4::tree::ParseTreeWalker().walk(myListener.get(), computeUnit);
    if (verbose) std::cout << "[verbose]   import done: " << importFilename << "\n";

    sourceFileName = savedSourceFileName;
    return true;
}