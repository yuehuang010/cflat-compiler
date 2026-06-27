#pragma once
// Low-level I/O shim for the LSP transport (JsonRpcLoop, LspServer). The
// underscore-prefixed CRT fd functions (_read/_write/_close/_dup/_fileno/
// _setmode) and _O_BINARY are provided by PlatformCompat.h on POSIX; on Windows
// they come from <io.h>/<fcntl.h>.
#include "PlatformCompat.h"
#include <fcntl.h>
#if defined(_WIN32)
#  include <io.h>
#endif
