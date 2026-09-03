#include "AppResources.h"

namespace cflat::appres
{
namespace
{
    void PushU16(std::vector<uint8_t>& bytes, uint16_t value)
    {
        bytes.push_back(static_cast<uint8_t>(value));
        bytes.push_back(static_cast<uint8_t>(value >> 8));
    }

    void PushU32(std::vector<uint8_t>& bytes, uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
            bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
    }

    void PushU32Be(std::vector<uint8_t>& bytes, uint32_t value)
    {
        for (int i = 3; i >= 0; --i)
            bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
    }

    void PadTo4(std::vector<uint8_t>& bytes)
    {
        while ((bytes.size() & 3u) != 0)
            bytes.push_back(0);
    }

    uint32_t ReadU32Be(const std::vector<uint8_t>& bytes, size_t offset)
    {
        return (static_cast<uint32_t>(bytes[offset]) << 24) | (static_cast<uint32_t>(bytes[offset + 1]) << 16)
            | (static_cast<uint32_t>(bytes[offset + 2]) << 8) | static_cast<uint32_t>(bytes[offset + 3]);
    }

    uint16_t ReadU16Le(const std::vector<uint8_t>& bytes, size_t offset)
    {
        return static_cast<uint16_t>(bytes[offset] | (static_cast<uint16_t>(bytes[offset + 1]) << 8));
    }

    uint32_t ReadU32Le(const std::vector<uint8_t>& bytes, size_t offset)
    {
        return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8)
            | (static_cast<uint32_t>(bytes[offset + 2]) << 16) | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
    }

    // Minimal UTF-8 decoder; invalid bytes become U+FFFD so a bad literal cannot
    // desync the UTF-16 stream.
    std::vector<uint16_t> Utf8ToUtf16(const std::string& text)
    {
        std::vector<uint16_t> out;
        size_t i = 0;
        const size_t size = text.size();
        while (i < size)
        {
            const unsigned char lead = static_cast<unsigned char>(text[i]);
            uint32_t code = 0xFFFD;
            size_t extra = 0;
            if (lead < 0x80) { code = lead; extra = 0; }
            else if ((lead & 0xE0) == 0xC0) { code = lead & 0x1Fu; extra = 1; }
            else if ((lead & 0xF0) == 0xE0) { code = lead & 0x0Fu; extra = 2; }
            else if ((lead & 0xF8) == 0xF0) { code = lead & 0x07u; extra = 3; }
            else { code = 0xFFFD; extra = 0; }

            if (extra > 0 && i + extra < size)
            {
                bool ok = true;
                uint32_t value = code;
                for (size_t k = 1; k <= extra; ++k)
                {
                    const unsigned char cont = static_cast<unsigned char>(text[i + k]);
                    if ((cont & 0xC0) != 0x80) { ok = false; break; }
                    value = (value << 6) | (cont & 0x3Fu);
                }
                code = ok ? value : 0xFFFD;
                i += ok ? (extra + 1) : 1;
            }
            else if (extra > 0)
            {
                code = 0xFFFD;
                i += 1;
            }
            else
            {
                i += 1;
            }

            if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
                code = 0xFFFD;

            if (code < 0x10000)
            {
                out.push_back(static_cast<uint16_t>(code));
            }
            else
            {
                const uint32_t adjusted = code - 0x10000;
                out.push_back(static_cast<uint16_t>(0xD800 + (adjusted >> 10)));
                out.push_back(static_cast<uint16_t>(0xDC00 + (adjusted & 0x3FF)));
            }
        }
        return out;
    }

    // Writes a UTF-16LE string plus its terminating NUL, returning the u16 count written.
    uint16_t PushUtf16(std::vector<uint8_t>& bytes, const std::string& text)
    {
        const std::vector<uint16_t> units = Utf8ToUtf16(text);
        for (uint16_t unit : units)
            PushU16(bytes, unit);
        PushU16(bytes, 0);
        return static_cast<uint16_t>(units.size() + 1);
    }

    // A version-resource block: 6-byte prologue patched once the children are known. wLength is
    // a u16, so a block past 0xFFFF cannot be represented; Finish records that instead of wrapping.
    struct VersionBlock
    {
        std::vector<uint8_t>& Bytes;
        bool& Failed;
        size_t Start = 0;

        VersionBlock(std::vector<uint8_t>& bytes, bool& failed, const std::string& key,
            uint32_t valueLength, uint16_t type)
            : Bytes(bytes), Failed(failed)
        {
            if (valueLength > 0xFFFFu)
            {
                Failed = true;
                valueLength = 0;
            }
            PadTo4(Bytes);
            Start = Bytes.size();
            PushU16(Bytes, 0);              // wLength, patched in Finish
            PushU16(Bytes, static_cast<uint16_t>(valueLength));
            PushU16(Bytes, type);
            PushUtf16(Bytes, key);
            PadTo4(Bytes);
        }

        void Finish()
        {
            const uint64_t length = static_cast<uint64_t>(Bytes.size() - Start);
            if (length > 0xFFFFu)
            {
                Failed = true;
                return;
            }
            Bytes[Start] = static_cast<uint8_t>(length);
            Bytes[Start + 1] = static_cast<uint8_t>(length >> 8);
        }
    };

    void PushStringBlock(std::vector<uint8_t>& bytes, bool& failed, const std::string& key,
        const std::string& value)
    {
        if (value.empty())
            return;
        const std::vector<uint16_t> units = Utf8ToUtf16(value);
        VersionBlock block(bytes, failed, key, static_cast<uint32_t>(units.size() + 1), 1);
        PushUtf16(bytes, value);
        block.Finish();
    }

    std::string HexU16(uint32_t value)
    {
        static const char* digits = "0123456789ABCDEF";
        std::string out(4, '0');
        for (int i = 3; i >= 0; --i)
        {
            out[static_cast<size_t>(i)] = digits[value & 0xF];
            value >>= 4;
        }
        return out;
    }

    bool HasVersionContent(const AppInfoData& info)
    {
        return !info.Name.empty() || !info.Identifier.empty() || !info.Description.empty()
            || !info.Company.empty() || !info.Copyright.empty() || !info.Version.File.empty()
            || !info.Version.Product.empty();
    }

    void PushResEntry(std::vector<uint8_t>& bytes, uint16_t type, uint16_t id, uint16_t language,
        const uint8_t* payload, size_t payloadSize)
    {
        PushU32(bytes, static_cast<uint32_t>(payloadSize));
        PushU32(bytes, 32);
        PushU16(bytes, 0xFFFF);
        PushU16(bytes, type);
        PushU16(bytes, 0xFFFF);
        PushU16(bytes, id);
        PushU32(bytes, 0);              // DataVersion
        PushU16(bytes, 0x0030);         // MemoryFlags
        PushU16(bytes, language);
        PushU32(bytes, 0);              // Version
        PushU32(bytes, 0);              // Characteristics
        if (payloadSize != 0)
            bytes.insert(bytes.end(), payload, payload + payloadSize);
        PadTo4(bytes);
    }

    void XmlEscapeInto(std::string& out, const std::string& text)
    {
        for (char c : text)
        {
            switch (c)
            {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c; break;
            }
        }
    }

    void PushPlistEntry(std::string& out, const std::string& key, const std::string& value)
    {
        if (value.empty())
            return;
        out += "\t<key>";
        XmlEscapeInto(out, key);
        out += "</key>\n\t<string>";
        XmlEscapeInto(out, value);
        out += "</string>\n";
    }

    const char* FileTypeName(AppFileType type)
    {
        switch (type)
        {
        case AppFileType::Dll: return "dll";
        case AppFileType::Driver: return "driver";
        case AppFileType::Application:
        default: return "application";
        }
    }
}

bool ProbePng(const std::vector<uint8_t>& bytes, uint32_t& width, uint32_t& height)
{
    static const uint8_t signature[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    if (bytes.size() < 24)
        return false;
    for (size_t i = 0; i < 8; ++i)
        if (bytes[i] != signature[i])
            return false;
    if (ReadU32Be(bytes, 8) != 13)
        return false;
    if (bytes[12] != 'I' || bytes[13] != 'H' || bytes[14] != 'D' || bytes[15] != 'R')
        return false;
    width = ReadU32Be(bytes, 16);
    height = ReadU32Be(bytes, 20);
    return true;
}

bool LooksLikeIco(const std::vector<uint8_t>& bytes)
{
    if (bytes.size() < 6)
        return false;
    return ReadU16Le(bytes, 0) == 0 && ReadU16Le(bytes, 2) == 1 && ReadU16Le(bytes, 4) != 0;
}

bool LooksLikeIcns(const std::vector<uint8_t>& bytes)
{
    return bytes.size() >= 8 && bytes[0] == 'i' && bytes[1] == 'c' && bytes[2] == 'n' && bytes[3] == 's';
}

bool SplitIco(const std::vector<uint8_t>& ico, std::vector<AppIconImage>& images, std::string& error)
{
    images.clear();
    error.clear();
    if (!LooksLikeIco(ico))
    {
        error = "not a valid .ico file: ICONDIR header is missing or malformed.";
        return false;
    }

    const size_t count = ReadU16Le(ico, 4);
    if (ico.size() < 6 + count * 16)
    {
        error = "malformed .ico file: the directory claims more images than the file holds.";
        return false;
    }

    for (size_t i = 0; i < count; ++i)
    {
        const size_t entry = 6 + i * 16;
        uint32_t width = ico[entry];
        uint32_t height = ico[entry + 1];
        if (width == 0) width = 256;
        if (height == 0) height = 256;
        const uint32_t size = ReadU32Le(ico, entry + 8);
        const uint32_t offset = ReadU32Le(ico, entry + 12);
        if (static_cast<uint64_t>(offset) + size > ico.size())
        {
            error = "malformed .ico file: image " + std::to_string(i + 1) + " runs past the end of the file.";
            return false;
        }
        AppIconImage image;
        image.Bytes.assign(ico.begin() + offset, ico.begin() + offset + size);
        image.Width = width;
        image.Height = height;
        images.push_back(std::move(image));
    }
    return true;
}

bool ParseVersionQuad(const std::string& text, uint16_t out[4], std::string& error)
{
    for (int i = 0; i < 4; ++i)
        out[i] = 0;
    error.clear();

    if (text.empty())
    {
        error = "version '" + text + "' is empty; expected one to four dotted decimals.";
        return false;
    }

    size_t start = 0;
    int index = 0;
    while (true)
    {
        const size_t dot = text.find('.', start);
        const std::string part = text.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (index >= 4)
        {
            error = "version '" + text + "' has more than four components.";
            return false;
        }
        if (part.empty())
        {
            error = "version '" + text + "' has an empty component.";
            return false;
        }
        uint32_t value = 0;
        for (char c : part)
        {
            if (c < '0' || c > '9')
            {
                error = "version '" + text + "' must be one to four dotted decimals.";
                return false;
            }
            value = value * 10 + static_cast<uint32_t>(c - '0');
            if (value > 65535)
            {
                error = "version '" + text + "' has a component that is not less than 65536.";
                return false;
            }
        }
        out[index++] = static_cast<uint16_t>(value);
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    return true;
}

std::vector<uint8_t> BuildVersionInfo(const AppInfoData& info, const std::string& outputFileName)
{
    bool ignoredOverflow = false;
    return BuildVersionInfo(info, outputFileName, ignoredOverflow);
}

std::vector<uint8_t> BuildVersionInfo(const AppInfoData& info, const std::string& outputFileName,
    bool& overflow)
{
    overflow = false;
    uint16_t fileQuad[4] = { 0, 0, 0, 0 };
    uint16_t productQuad[4] = { 0, 0, 0, 0 };
    std::string ignored;
    if (!info.Version.File.empty())
        ParseVersionQuad(info.Version.File, fileQuad, ignored);
    const std::string productText = info.Version.Product.empty() ? info.Version.File : info.Version.Product;
    if (!productText.empty())
        ParseVersionQuad(productText, productQuad, ignored);

    uint32_t flags = 0;
    if (info.Version.Debug) flags |= 0x1;
    if (info.Version.Prerelease) flags |= 0x2;

    std::vector<uint8_t> bytes;
    VersionBlock root(bytes, overflow, "VS_VERSION_INFO", 52, 0);
    PushU32(bytes, 0xFEEF04BD);                                     // dwSignature
    PushU32(bytes, 0x00010000);                                     // dwStrucVersion
    PushU32(bytes, (static_cast<uint32_t>(fileQuad[0]) << 16) | fileQuad[1]);
    PushU32(bytes, (static_cast<uint32_t>(fileQuad[2]) << 16) | fileQuad[3]);
    PushU32(bytes, (static_cast<uint32_t>(productQuad[0]) << 16) | productQuad[1]);
    PushU32(bytes, (static_cast<uint32_t>(productQuad[2]) << 16) | productQuad[3]);
    PushU32(bytes, 0x3F);                                           // dwFileFlagsMask
    PushU32(bytes, flags);
    PushU32(bytes, 0x00040004);                                     // VOS_NT_WINDOWS32
    PushU32(bytes, static_cast<uint32_t>(info.Type));
    PushU32(bytes, 0);                                              // dwFileSubtype
    PushU32(bytes, 0);                                              // dwFileDateMS
    PushU32(bytes, 0);                                              // dwFileDateLS

    {
        VersionBlock stringFileInfo(bytes, overflow, "StringFileInfo", 0, 1);
        {
            const std::string langKey = HexU16(static_cast<uint32_t>(info.Language) & 0xFFFFu) + "04B0";
            VersionBlock table(bytes, overflow, langKey, 0, 1);
            PushStringBlock(bytes, overflow, "CompanyName", info.Company);
            PushStringBlock(bytes, overflow, "FileDescription", info.Description);
            PushStringBlock(bytes, overflow, "FileVersion", info.Version.File);
            PushStringBlock(bytes, overflow, "InternalName", info.Identifier);
            PushStringBlock(bytes, overflow, "LegalCopyright", info.Copyright);
            PushStringBlock(bytes, overflow, "OriginalFilename", outputFileName);
            PushStringBlock(bytes, overflow, "ProductName", info.Name);
            PushStringBlock(bytes, overflow, "ProductVersion", productText);
            table.Finish();
        }
        stringFileInfo.Finish();
    }

    {
        VersionBlock varFileInfo(bytes, overflow, "VarFileInfo", 0, 1);
        {
            VersionBlock translation(bytes, overflow, "Translation", 4, 0);
            PushU16(bytes, static_cast<uint16_t>(info.Language));
            PushU16(bytes, 0x04B0);
            translation.Finish();
        }
        varFileInfo.Finish();
    }

    root.Finish();
    if (overflow)
        return std::vector<uint8_t>();
    return bytes;
}

std::vector<uint8_t> BuildWindowsRes(const AppInfoData& info, const std::string& outputFileName,
    const std::string& manifestXml)
{
    bool ignoredOverflow = false;
    return BuildWindowsRes(info, outputFileName, manifestXml, ignoredOverflow);
}

std::vector<uint8_t> BuildWindowsRes(const AppInfoData& info, const std::string& outputFileName,
    const std::string& manifestXml, bool& overflow)
{
    overflow = false;
    std::vector<uint8_t> bytes;
    const uint16_t language = static_cast<uint16_t>(info.Language);

    // Mandatory empty leading entry: 32-byte header, zero payload, zero flags.
    PushU32(bytes, 0);
    PushU32(bytes, 32);
    PushU16(bytes, 0xFFFF); PushU16(bytes, 0);
    PushU16(bytes, 0xFFFF); PushU16(bytes, 0);
    PushU32(bytes, 0);
    PushU16(bytes, 0); PushU16(bytes, 0);
    PushU32(bytes, 0);
    PushU32(bytes, 0);

    const std::vector<AppIconImage>& icons = info.Icons;
    if (!icons.empty() && info.IconKind != AppIconKind::None)
    {
        for (size_t i = 0; i < icons.size(); ++i)
            PushResEntry(bytes, 3, static_cast<uint16_t>(i + 1), language,
                icons[i].Bytes.data(), icons[i].Bytes.size());

        std::vector<uint8_t> group;
        PushU16(group, 0);                                      // idReserved
        PushU16(group, 1);                                      // idType = icon
        PushU16(group, static_cast<uint16_t>(icons.size()));
        for (size_t i = 0; i < icons.size(); ++i)
        {
            group.push_back(icons[i].Width >= 256 ? 0 : static_cast<uint8_t>(icons[i].Width));
            group.push_back(icons[i].Height >= 256 ? 0 : static_cast<uint8_t>(icons[i].Height));
            group.push_back(0);                                 // bColorCount
            group.push_back(0);                                 // bReserved
            PushU16(group, 1);                                  // wPlanes
            PushU16(group, 32);                                 // wBitCount
            PushU32(group, static_cast<uint32_t>(icons[i].Bytes.size()));
            PushU16(group, static_cast<uint16_t>(i + 1));       // nId
        }
        PushResEntry(bytes, 14, 1, language, group.data(), group.size());
    }

    if (HasVersionContent(info))
    {
        const std::vector<uint8_t> version = BuildVersionInfo(info, outputFileName, overflow);
        if (overflow)
            return std::vector<uint8_t>();
        PushResEntry(bytes, 16, 1, language, version.data(), version.size());
    }

    if (!manifestXml.empty())
        PushResEntry(bytes, 24, 1, language,
            reinterpret_cast<const uint8_t*>(manifestXml.data()), manifestXml.size());

    return bytes;
}

std::string BuildInfoPlist(const AppInfoData& info, const std::string& executableName,
    const std::string& minimumSystemVersion, bool bundleIcon)
{
    const std::string productText = info.Version.Product.empty() ? info.Version.File : info.Version.Product;

    std::string out;
    out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out += "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
    out += "<plist version=\"1.0\">\n<dict>\n";

    // Keys in a stable sorted order so the output is byte-identical run to run.
    PushPlistEntry(out, "CFBundleExecutable", executableName);
    if (bundleIcon)
        PushPlistEntry(out, "CFBundleIconFile", executableName + ".icns");
    PushPlistEntry(out, "CFBundleIdentifier", info.Identifier);
    PushPlistEntry(out, "CFBundleName", info.Name);
    PushPlistEntry(out, "CFBundlePackageType", "APPL");
    PushPlistEntry(out, "CFBundleShortVersionString", productText);
    PushPlistEntry(out, "CFBundleVersion", info.Version.File);
    PushPlistEntry(out, "LSMinimumSystemVersion", minimumSystemVersion);
    PushPlistEntry(out, "NSHumanReadableCopyright", info.Copyright);

    out += "</dict>\n</plist>\n";
    return out;
}

std::vector<uint8_t> BuildIcns(const AppInfoData& info)
{
    if (info.IconKind == AppIconKind::Icns)
        return info.Icons.empty() ? std::vector<uint8_t>() : info.Icons.front().Bytes;
    if (info.IconKind == AppIconKind::Ico || info.IconKind == AppIconKind::None)
        return std::vector<uint8_t>();

    // Size -> {chunk, @2x chunk}; an absent size is simply not emitted.
    struct SlotEntry { uint32_t Size; const char* Primary; const char* Retina; };
    static const SlotEntry slots[] = {
        { 16,   "icp4", nullptr },
        { 32,   "icp5", "ic11" },
        { 64,   nullptr, "ic12" },
        { 128,  "ic07", nullptr },
        { 256,  "ic08", "ic13" },
        { 512,  "ic09", "ic14" },
        { 1024, "ic10", nullptr },
    };

    std::vector<uint8_t> body;
    for (const SlotEntry& slot : slots)
    {
        const AppIconImage* image = nullptr;
        for (const AppIconImage& candidate : info.Icons)
            if (candidate.Width == slot.Size)
            {
                image = &candidate;
                break;
            }
        if (image == nullptr)
            continue;

        const char* types[2] = { slot.Primary, slot.Retina };
        for (const char* type : types)
        {
            if (type == nullptr)
                continue;
            body.push_back(static_cast<uint8_t>(type[0]));
            body.push_back(static_cast<uint8_t>(type[1]));
            body.push_back(static_cast<uint8_t>(type[2]));
            body.push_back(static_cast<uint8_t>(type[3]));
            PushU32Be(body, static_cast<uint32_t>(image->Bytes.size() + 8));
            body.insert(body.end(), image->Bytes.begin(), image->Bytes.end());
        }
    }

    if (body.empty())
        return std::vector<uint8_t>();

    std::vector<uint8_t> out;
    out.push_back('i'); out.push_back('c'); out.push_back('n'); out.push_back('s');
    PushU32Be(out, static_cast<uint32_t>(body.size() + 8));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::string DescribeVersionBlock(const AppInfoData& info, const std::string& outputFileName)
{
    const std::string productText = info.Version.Product.empty() ? info.Version.File : info.Version.Product;
    const std::pair<const char*, std::string> entries[] = {
        { "CompanyName", info.Company },
        { "FileDescription", info.Description },
        { "FileVersion", info.Version.File },
        { "InternalName", info.Identifier },
        { "LegalCopyright", info.Copyright },
        { "OriginalFilename", outputFileName },
        { "ProductName", info.Name },
        { "ProductVersion", productText },
    };
    std::string out = "VS_VERSIONINFO string table (language " + HexU16(
        static_cast<uint32_t>(info.Language) & 0xFFFFu) + "04B0):\n";
    for (const auto& [key, value] : entries)
        if (!value.empty()) out += std::string("  ") + key + " = " + value + "\n";
    return out;
}

std::string DescribeAppInfo(const AppInfoData& info)
{
    const std::string productText = info.Version.Product.empty() ? info.Version.File : info.Version.Product;

    std::string out;
    out += "name: " + info.Name + "\n";
    out += "identifier: " + info.Identifier + "\n";
    out += "description: " + info.Description + "\n";
    out += "company: " + info.Company + "\n";
    out += "copyright: " + info.Copyright + "\n";
    out += "version file: " + info.Version.File + "\n";
    out += "version product: " + productText + "\n";
    std::string flags;
    if (info.Version.Prerelease) flags += "prerelease";
    if (info.Version.Debug) { if (!flags.empty()) flags += " "; flags += "debug"; }
    if (flags.empty()) flags = "none";
    out += "version flags: " + flags + "\n";
    out += "type: " + std::string(FileTypeName(info.Type)) + "\n";
    out += "language: " + std::to_string(info.Language) + "\n";

    for (const AppIconImage& icon : info.Icons)
    {
        const std::string size = std::to_string(icon.Bytes.size()) + " bytes";
        if (info.IconKind == AppIconKind::Ico)
            out += "icon: " + std::to_string(icon.Width) + "x" + std::to_string(icon.Height)
                + " ico (" + size + ")\n";
        else if (info.IconKind == AppIconKind::Icns)
            out += "icon: icns (" + size + ")\n";
        else
            out += "icon: " + std::to_string(icon.Width) + "x" + std::to_string(icon.Height)
                + " png (" + size + ")\n";
    }
    return out;
}
}
