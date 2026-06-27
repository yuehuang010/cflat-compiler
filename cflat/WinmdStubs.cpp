// Non-Windows stubs for the WinMD/WinRT functions. WinMD is a Windows-only
// feature: the real implementations (WinmdExtract.cpp / WinmdEmit.cpp /
// WinmdSignature.cpp) use the Windows SDK and are excluded from the build on
// other platforms. These stubs let the host link on Linux/macOS and fail
// cleanly with a clear message if a WinMD path is ever exercised.
#include "WinmdExtract.h"
#include "WinmdEmit.h"
#include "WinmdSignature.h"

namespace cflat_winmd {

static const char* kUnsupported = "WinMD/WinRT is only supported on Windows.";

bool ReadWinmd(const std::string&, Model&, std::string& err)
{
    err = kUnsupported;
    return false;
}

bool WriteWinmd(const Model&, const std::string&, const std::string&, std::string& err)
{
    err = kUnsupported;
    return false;
}

bool DerivePiid(const TypeRef&, const Model&, uint8_t[16], std::string& err)
{
    err = kUnsupported;
    return false;
}

std::string FormatGuidImage(const uint8_t[16])
{
    return std::string();
}

bool WinmdSignatureSelfTest(std::string& report)
{
    report = kUnsupported;
    return false;
}

} // namespace cflat_winmd
