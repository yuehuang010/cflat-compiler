#pragma once
#include <filesystem>
#include <string>

// Entry point for the LSP server mode.
// Called from main() when argv[1] == "lsp".
int RunLspServer(int argc, char* argv[]);

// Returns the directory containing the running executable (used to locate runtime.cb).
inline std::string GetExeDir()
{
    char* pgmptr = nullptr;
    _get_pgmptr(&pgmptr);
    return std::filesystem::path(pgmptr ? pgmptr : "").parent_path().string();
}
