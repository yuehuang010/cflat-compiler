// WinRT metadata writer (produce side) - serializes a cflat_winmd::Model to a .winmd file.
//
// The OS metadata dispenser (rometadata.dll) is READ-ONLY: DefineScope returns E_NOTIMPL for
// IMetaDataEmit/IMetaDataEmit2 (probed 2026-06-22). So, like windows-rs and cppwinrt, we write
// the ECMA-335 metadata image ourselves (II.24): the #~ table stream plus the #Strings/#Blob/
// #GUID/#US heaps, wrapped in a minimal PE32 CLI image. Free of Windows/LLVM/ANTLR types so it
// can be included by LLVMBackend.h; all heavy lifting is in WinmdEmit.cpp.
#pragma once

#include <string>
#include "WinmdModel.h"

namespace cflat_winmd
{
    // Write `model` as a .winmd to `path`. `assemblyName` is the winmd assembly + module name
    // (e.g. "Zoo" for Zoo.winmd). Returns false with `err` set on failure.
    bool WriteWinmd(const Model& model, const std::string& assemblyName,
                    const std::string& path, std::string& err);
}
