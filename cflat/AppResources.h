#pragma once

// Byte-level serializers for the `application` declaration (internal/plan/resource-embedding.md,
// section 3.2). Pure functions over plain data: no compiler state, no LLVM, no OS API, so they
// are identical on every host and testable from a scratch driver. The declaration walker fills
// AppInfoData; the link step picks the writer for the target.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cflat::appres
{
    // Deployment target every macOS link stamps (triple arm64-apple-macosx11.0.0), reused as
    // LSMinimumSystemVersion so the plist and the load command cannot disagree.
    inline constexpr const char* kMacMinimumSystemVersion = "11.0";

    enum class AppFileType : int { Application = 1, Dll = 2, Driver = 3 };

    struct AppVersionData
    {
        std::string File;      // "0.11.0" as written; validated 1-4 dotted decimals, each < 65536
        std::string Product;   // defaults to File when empty
        bool Prerelease = false;
        bool Debug = false;
    };

    // One icon image as supplied by `icon = { { image = embed("x.png") }, ... }`.
    struct AppIconImage
    {
        std::vector<uint8_t> Bytes;   // raw PNG bytes (or the whole .ico / .icns when Kind says so)
        uint32_t Width = 0;           // from IHDR for PNG; 0 for ico/icns containers
        uint32_t Height = 0;
    };

    enum class AppIconKind { None, PngSet, Ico, Icns };

    struct AppInfoData
    {
        std::string Name;
        std::string Identifier;
        std::string Description;
        std::string Company;
        std::string Copyright;
        AppVersionData Version;
        AppFileType Type = AppFileType::Application;
        int Language = 1033;
        AppIconKind IconKind = AppIconKind::None;
        std::vector<AppIconImage> Icons;   // PngSet: one per size; Ico/Icns: exactly one entry
    };

    // PNG signature + IHDR probe. Returns false when the bytes are not a PNG.
    bool ProbePng(const std::vector<uint8_t>& bytes, uint32_t& width, uint32_t& height);
    bool LooksLikeIco(const std::vector<uint8_t>& bytes);    // ICONDIR reserved=0, type=1
    bool LooksLikeIcns(const std::vector<uint8_t>& bytes);   // 'icns' magic

    // Splits a user .ico into its images (PNG or DIB payloads as stored) so they can become
    // RT_ICON entries. Width/Height come from the ICONDIRENTRY (0 means 256).
    bool SplitIco(const std::vector<uint8_t>& ico, std::vector<AppIconImage>& images, std::string& error);

    // Parses "1.2.3.4" (1-4 components, each < 65536, missing = 0) into four u16.
    bool ParseVersionQuad(const std::string& text, uint16_t out[4], std::string& error);

    // Windows: the full .res image - leading empty entry, RT_ICON 1..N, RT_GROUP_ICON 1,
    // RT_VERSION 1, and RT_MANIFEST 24/1 when manifestXml is non-empty. Layout is the table
    // in the plan. outputFileName feeds "OriginalFilename".
    std::vector<uint8_t> BuildWindowsRes(const AppInfoData& info, const std::string& outputFileName,
        const std::string& manifestXml);

    // Same, reporting a version block that cannot be expressed in its u16 wLength. On overflow
    // the result is empty; the declaration walker is the real gate on over-long fields.
    std::vector<uint8_t> BuildWindowsRes(const AppInfoData& info, const std::string& outputFileName,
        const std::string& manifestXml, bool& overflow);

    // VS_VERSIONINFO payload alone (what --dump-app-info prints in a readable form).
    std::vector<uint8_t> BuildVersionInfo(const AppInfoData& info, const std::string& outputFileName);
    std::vector<uint8_t> BuildVersionInfo(const AppInfoData& info, const std::string& outputFileName,
        bool& overflow);

    // macOS: Info.plist XML from the same leaves. bundleIcon=true adds CFBundleIconFile.
    std::string BuildInfoPlist(const AppInfoData& info, const std::string& executableName,
        const std::string& minimumSystemVersion, bool bundleIcon);

    // macOS: .icns from a PNG set (chunk table in the plan) or the user .icns verbatim.
    std::vector<uint8_t> BuildIcns(const AppInfoData& info);

    // The VS_VERSIONINFO string table as readable "Key = value" lines, in the order
    // BuildVersionInfo emits it. Empty fields are skipped, exactly as in the .res payload.
    std::string DescribeVersionBlock(const AppInfoData& info, const std::string& outputFileName);

    // Human-readable field dump shared by --dump-app-info on every host.
    std::string DescribeAppInfo(const AppInfoData& info);
}
