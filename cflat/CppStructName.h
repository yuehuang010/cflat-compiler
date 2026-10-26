#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace cflat_cinterop
{
inline std::string GeneratedCflatStructIdentifier(std::string_view name)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string out = "cf";
    std::size_t start = 0;
    bool first = true;
    while (true)
    {
        const std::size_t end = name.find('.', start);
        const std::string_view component = name.substr(
            start, end == std::string_view::npos ? name.size() - start : end - start);
        if (!first) out.push_back('z');
        first = false;
        out += std::to_string(component.size());
        out.push_back('x');
        for (unsigned char byte : component)
        {
            out.push_back(hex[byte >> 4]);
            out.push_back(hex[byte & 0x0f]);
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return out;
}

inline std::string GeneratedCflatStructCxxName(std::string_view name)
{
    return "__cflat_user::" + GeneratedCflatStructIdentifier(name);
}

inline int GeneratedCflatHexValue(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

inline bool DecodeGeneratedCflatStructIdentifier(std::string_view identifier,
                                                  std::string& name)
{
    if (!identifier.starts_with("cf")) return false;
    std::string decoded;
    std::size_t pos = 2;
    bool first = true;
    while (pos < identifier.size())
    {
        const std::size_t lengthStart = pos;
        std::size_t length = 0;
        while (pos < identifier.size() && identifier[pos] >= '0' && identifier[pos] <= '9')
        {
            const unsigned digit = static_cast<unsigned>(identifier[pos] - '0');
            if (length > (static_cast<std::size_t>(-1) - digit) / 10) return false;
            length = length * 10 + digit;
            ++pos;
        }
        if (pos == lengthStart || pos >= identifier.size() || identifier[pos] != 'x')
            return false;
        ++pos;
        if (length > (identifier.size() - pos) / 2) return false;
        if (!first) decoded.push_back('.');
        first = false;
        for (std::size_t i = 0; i < length; ++i)
        {
            const int high = GeneratedCflatHexValue(identifier[pos++]);
            const int low = GeneratedCflatHexValue(identifier[pos++]);
            if (high < 0 || low < 0) return false;
            decoded.push_back(static_cast<char>((high << 4) | low));
        }
        if (pos == identifier.size())
        {
            name = std::move(decoded);
            return true;
        }
        if (identifier[pos++] != 'z' || pos == identifier.size()) return false;
    }
    return false;
}

inline std::string DemangleGeneratedCflatStructNames(std::string_view text)
{
    static constexpr std::string_view prefix = "__cflat_user::";
    if (text.find(prefix) == std::string_view::npos) return std::string(text);
    std::string out;
    std::size_t pos = 0;
    while (pos < text.size())
    {
        const std::size_t found = text.find(prefix, pos);
        if (found == std::string_view::npos)
        {
            out.append(text.substr(pos));
            break;
        }
        out.append(text.substr(pos, found - pos));
        out.append(prefix);
        const std::size_t start = found + prefix.size();
        std::size_t end = start;
        while (end < text.size())
        {
            const char value = text[end];
            if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z')
                  || (value >= '0' && value <= '9') || value == '_'))
                break;
            ++end;
        }
        std::string decoded;
        if (DecodeGeneratedCflatStructIdentifier(text.substr(start, end - start), decoded))
            out += decoded;
        else
            out.append(text.substr(start, end - start));
        pos = end;
    }
    return out;
}
}
