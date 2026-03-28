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
    auto outputPath = args.getOption("output").value_or(".\\out.ll");
    auto bitcodePath = args.getOption("bitcode").value_or("");
    bool debugInfo = args.hasFlag("debug-info");

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


    if (!VerifyModule())
        return false;

    if (!SaveToFile(outputPath))
        return false;

    if (!bitcodePath.empty() && !WriteBitcode(bitcodePath))
        return false;

    return true;
}