#pragma once
#include "platform/PlatformCompat.h"
#include <filesystem>
#include <string>

// Entry point for the LSP server mode.
// Called from main() when argv[1] == "lsp".
int RunLspServer(int argc, char* argv[]);

// Returns the directory containing the running executable (used to locate runtime.cb).
inline std::string GetExeDir()
{
    return std::filesystem::path(PlatformExePath()).parent_path().string();
}
