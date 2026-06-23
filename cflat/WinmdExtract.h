// WinRT metadata reader (consume side) - raises a .winmd file into the pure-data WinrtModel.
//
// Like CClangExtract.h this header is free of any OS metadata COM types; all `IMetaDataImport2`
// usage lives in WinmdExtract.cpp so LLVMBackend.h can include this without dragging the
// Windows metadata headers into that translation unit. The backend consumes only the Model.
#pragma once

#include <string>
#include "WinmdModel.h"

namespace cflat_winmd
{
    // Read every type definition in `path` (a .winmd file) into `out`. Returns false on a hard
    // failure (file missing, not a valid metadata scope) with `err` set to a human-readable
    // reason; a partially-decoded type does not fail the whole read.
    bool ReadWinmd(const std::string& path, Model& out, std::string& err);
}
