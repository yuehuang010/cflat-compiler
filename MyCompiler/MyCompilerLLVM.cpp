#include <llvm\IR\IRBuilder.h>
#include <llvm\IR\LLVMContext.h>
#include <llvm\IR\Module.h>
#include <llvm\IR\Verifier.h>
#include <llvm\Bitcode\BitcodeWriter.h>
#include <antlr4-runtime.h>

#include "CParser.h"
#include "CLexer.h"
#include "CBaseListener.h"
#include "MyCompilerLLVM.h"
#include "MyListener.h"
#include <filesystem>

bool MyCompilerLLVM::Compile(const ArgParser& args)
{
    auto filename = args.getPositional(0).value_or("");
    sourceFileName = std::filesystem::path(filename).filename().string();
    importedFiles.insert(std::filesystem::weakly_canonical(filename).string());
    auto outputPath = args.getOption("output").value_or(".\\out.ll");
    auto bitcodePath = args.getOption("bitcode").value_or("");
    bool debugInfo = args.hasFlag("debug-info");
    importSearchDir = args.getOption("import-dir").value_or("");

    auto outputDir = std::filesystem::path(outputPath).parent_path();
    if (!outputDir.empty() && !std::filesystem::exists(outputDir))
    {
        std::cerr << "Error: output directory '" << outputDir.string() << "' does not exist (-o " << outputPath << ").\n";
        return false;
    }

    if (!std::filesystem::exists(filename))
    {
        std::cout << "File doesn't exists.\n";
        return false;
    }

    if (debugInfo)
    {
        std::filesystem::path filePath = std::filesystem::absolute(filename);
        InitDebugInfo(filePath.filename().string(), filePath.parent_path().string());
    }

    {
        std::ifstream stream;
        stream.open(filename);
        antlr4::ANTLRInputStream input(stream);

        CLexer lexer(&input);
        antlr4::CommonTokenStream tokens(&lexer);
        CParser parser(&tokens);

        tokens.fill();
        //for (auto token : tokens.getTokens()) {
        //	std::cout << token->toString() << std::endl;
        //}

        auto computeUnit = parser.compilationUnit();

        // Process all top-level imports before scanning the main file so that
        // imported symbols are available to ForwardRefScanner.
        if (auto* tu = computeUnit->translationUnit()) {
            for (auto* decl : tu->externalDeclaration()) {
                if (auto* imp = decl->importDeclaration()) {
                    std::string raw = imp->StringLiteral()->getText();
                    std::string importFilename = raw.substr(1, raw.size() - 2);
                    CompileImportedFile(filename, importFilename);
                }
            }
        }

        // Pre-scan: register all function signatures and struct type shells so
        // that forward references resolve during the main code-gen walk.
        {
            ForwardRefScanner scanner(this);
            if (auto* tu = computeUnit->translationUnit())
                for (auto* decl : tu->externalDeclaration())
                    scanner.ScanExternalDeclaration(decl);
        }

        auto myListener = std::make_unique<MyListener>(&parser, this);
        auto walker = antlr4::tree::ParseTreeWalker();
        walker.walk(myListener.get(), computeUnit);
        stream.close();
    }

    FinalizeDebugInfo();

    // validate that the stack is empty.
    if (stackNamedVariable.size() > 0)
    {
        for (const auto& stack : stackNamedVariable)
        {
            std::cout << "Stack is not empty:\n";
            for (const auto& funcVariable : stack.functionArgument)
            {
                std::cout << "Function var: " << funcVariable.first << "\n";
            }
            for (const auto& namedVariable : stack.namedVariable)
            {
                std::cout << "namedVar var: " << namedVariable.first << "\n";
            }
        }
    }


    if (!SaveToFile(outputPath))
        return false;

    if (!VerifyModule())
        return false;

    if (!bitcodePath.empty() && !WriteBitcode(bitcodePath))
        return false;

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
    if (ec)
    {
        std::cerr << "Error: imported file not found: " << importFilename << "\n";
        return false;
    }
    auto canonicalStr = canonical.string();
    if (importedFiles.count(canonicalStr))
        return true; // already imported or cycle
    importedFiles.insert(canonicalStr);

    std::ifstream stream;
    stream.open(canonicalStr);
    antlr4::ANTLRInputStream input(stream);
    CLexer lexer(&input);
    antlr4::CommonTokenStream tokens(&lexer);
    CParser parser(&tokens);
    tokens.fill();
    auto computeUnit = parser.compilationUnit();

    // Recursively process nested imports before scanning this file's declarations
    if (auto* tu = computeUnit->translationUnit())
    {
        for (auto* decl : tu->externalDeclaration())
        {
            if (auto* imp = decl->importDeclaration())
            {
                std::string raw = imp->StringLiteral()->getText();
                std::string nested = raw.substr(1, raw.size() - 2);
                if (!CompileImportedFile(canonicalStr, nested))
                    return false;
            }
        }
    }

    // Forward-ref scan the imported file
    {
        ForwardRefScanner scanner(this);
        if (auto* tu = computeUnit->translationUnit())
            for (auto* decl : tu->externalDeclaration())
                scanner.ScanExternalDeclaration(decl);
    }

    // Code-gen walk the imported file
    auto myListener = std::make_unique<MyListener>(&parser, this);
    antlr4::tree::ParseTreeWalker().walk(myListener.get(), computeUnit);
    stream.close();
    return true;
}