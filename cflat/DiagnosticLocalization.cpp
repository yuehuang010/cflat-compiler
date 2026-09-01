#include "pch.h"
#include "DiagnosticLocalization.h"

#include <nlohmann/json.hpp>

namespace
{
using Json = nlohmann::json;

bool IsAsciiAlphaNumeric(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

std::string NormalizeKeyFull(std::string_view englishTemplate)
{
    std::string key;
    size_t argument = 0;
    for (size_t i = 0; i < englishTemplate.size();)
    {
        if (englishTemplate[i] == '{' && i + 1 < englishTemplate.size() && englishTemplate[i + 1] == '}')
        {
            key += "arg" + std::to_string(argument++);
            i += 2;
            continue;
        }

        char c = englishTemplate[i++];
        if (IsAsciiAlphaNumeric(c))
            key += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    return key;
}

bool IsHex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

bool IsCompactedKey(std::string_view key)
{
    if (key.size() != 59 || key.substr(20, 3) != "...")
        return false;
    for (size_t i = 43; i < key.size(); ++i)
        if (!IsHex(key[i]))
            return false;
    return true;
}

std::string CompactKey(std::string_view fullKey)
{
    if (fullKey.size() <= 40 || IsCompactedKey(fullKey))
        return std::string(fullKey);

    uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : fullKey)
    {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return std::format("{}...{}{:016x}", fullKey.substr(0, 20),
                       fullKey.substr(fullKey.size() - 20), hash);
}

std::string SourceTemplateWithNumberedArguments(std::string_view text)
{
    std::string result;
    result.reserve(text.size());
    size_t argument = 0;
    for (size_t i = 0; i < text.size();)
    {
        if (text[i] == '{' && i + 1 < text.size() && text[i + 1] == '}')
        {
            result += "{" + std::to_string(argument++) + "}";
            i += 2;
        }
        else
            result += text[i++];
    }
    return result;
}

bool HasValidNumberedPlaceholders(std::string_view text, size_t argumentCount)
{
    for (size_t i = 0; i < text.size();)
    {
        if (text[i] != '{')
        {
            ++i;
            continue;
        }

        size_t close = text.find('}', i + 1);
        if (close == std::string_view::npos || close == i + 1)
            return false;

        size_t index = 0;
        for (size_t j = i + 1; j < close; ++j)
        {
            char c = text[j];
            if (c < '0' || c > '9')
                return false;
            index = index * 10 + static_cast<size_t>(c - '0');
        }
        if (index >= argumentCount)
            return false;
        i = close + 1;
    }
    return true;
}

std::string FormatNumberedTemplate(std::string_view text,
                                   const std::vector<std::string>& arguments)
{
    std::string result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size();)
    {
        if (text[i] == '{')
        {
            size_t close = text.find('}', i + 1);
            if (close != std::string_view::npos && close > i + 1)
            {
                size_t index = 0;
                bool numeric = true;
                for (size_t j = i + 1; j < close; ++j)
                {
                    char c = text[j];
                    if (c < '0' || c > '9')
                    {
                        numeric = false;
                        break;
                    }
                    index = index * 10 + static_cast<size_t>(c - '0');
                }
                if (numeric && index < arguments.size())
                {
                    result += arguments[index];
                    i = close + 1;
                    continue;
                }
            }
        }
        result += text[i++];
    }
    return result;
}

bool ReadJson(const std::filesystem::path& path, Json& root, bool verbose)
{
    std::ifstream input(path);
    if (!input)
    {
        if (verbose)
            std::cerr << std::format("[verbose] locale file not found: {}\n", path.string());
        return false;
    }

    try
    {
        root = Json::parse(input);
        return true;
    }
    catch (const std::exception& e)
    {
        if (verbose)
            std::cerr << std::format("[verbose] could not parse locale file '{}': {}\n",
                                     path.string(), e.what());
        return false;
    }
}
}

void DiagnosticLocalization::SetLocale(std::string locale)
{
    if (!locale.empty())
        locale_ = std::move(locale);
}

void DiagnosticLocalization::SetLocaleDirectory(std::string directory)
{
    localeDirectory_ = std::move(directory);
}

void DiagnosticLocalization::SetCollectTemplates(bool enabled)
{
    collectTemplates_ = enabled;
    if (enabled)
    {
        collectedTemplates_.clear();
        collectedArgumentExamples_.clear();
    }
}

bool DiagnosticLocalization::LoadCatalog(
    const std::filesystem::path& path,
    std::unordered_map<std::string, std::string>& messages,
    bool verbose,
    std::unordered_map<std::string, std::vector<std::string>>* argumentExamples) const
{
    llvm::TimeTraceScope localeScope("LocaleJsonLoad", path.string());
    Json root;
    if (!ReadJson(path, root, verbose) || !root.is_object() ||
        !root.contains("messages") || !root["messages"].is_object())
        return false;

    messages.clear();
    if (argumentExamples)
        argumentExamples->clear();
    for (const auto& [key, value] : root["messages"].items())
    {
        if (!value.is_string())
        {
            if (verbose)
                std::cerr << std::format("[verbose] ignoring non-string locale entry '{}' in {}\n",
                                         key, path.string());
            continue;
        }
        messages.emplace(key, value.get<std::string>());
    }
    if (argumentExamples && root.contains("argumentExamples") && root["argumentExamples"].is_object())
    {
        for (const auto& [key, value] : root["argumentExamples"].items())
        {
            if (!value.is_array())
                continue;
            std::vector<std::string> examples;
            for (const auto& example : value)
            {
                if (!example.is_string())
                {
                    examples.clear();
                    break;
                }
                examples.push_back(example.get<std::string>());
            }
            if (!examples.empty() || value.empty())
                (*argumentExamples)[key] = std::move(examples);
        }
    }
    return true;
}

bool DiagnosticLocalization::Load(bool verbose)
{
    messages_.clear();
    enMessages_.clear();
    pseudoReportedKeys_.clear();
    collectedTemplates_.clear();
    collectedArgumentExamples_.clear();

    bool enLoaded = LoadCatalog(localeDirectory_ / "en.json",
                                      enMessages_, verbose);

    if (locale_ == "pseudo")
        return true;

    if (locale_ == "en")
    {
        messages_ = enMessages_;
        return enLoaded;
    }

    bool selectedLoaded = LoadCatalog(localeDirectory_ / (locale_ + ".json"), messages_, verbose);
    return enLoaded && selectedLoaded;
}

std::string DiagnosticLocalization::NormalizeKey(std::string_view englishTemplate)
{
    return CompactKey(NormalizeKeyFull(englishTemplate));
}

std::string DiagnosticLocalization::ResolveClientLocale(std::string_view clientTag,
                                                        const std::filesystem::path& localeDirectory)
{
    std::string tag;
    for (char c : clientTag)
        tag += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    if (tag.empty() || tag == "pseudo")
        return "en";

    auto catalogExists = [&localeDirectory](const std::string& name)
    {
        std::error_code ec;
        return std::filesystem::exists(localeDirectory / (name + ".json"), ec);
    };

    const std::string primary = tag.substr(0, tag.find('-'));
    if (primary == "en")
        return "en";

    // Chinese needs script, not region: the catalogs are zh-Hans / zh-Hant.
    if (primary == "zh")
    {
        const bool traditional = tag.find("hant") != std::string::npos
            || tag.find("-tw") != std::string::npos
            || tag.find("-hk") != std::string::npos
            || tag.find("-mo") != std::string::npos;
        const std::string candidate = traditional ? "zh-Hant" : "zh-Hans";
        if (catalogExists(candidate))
            return candidate;
        return "en";
    }

    // Exact tag first (fr-ca.json if someone ships one), then the primary subtag.
    if (catalogExists(tag))
        return tag;
    if (catalogExists(primary))
        return primary;
    return "en";
}

std::string DiagnosticLocalization::FormatSourceTemplate(
    std::string_view englishTemplate,
    const std::vector<std::string>& arguments)
{
    return FormatNumberedTemplate(SourceTemplateWithNumberedArguments(englishTemplate), arguments);
}

std::string DiagnosticLocalization::Localize(
    std::string_view englishTemplate,
    const std::vector<std::string>& arguments) const
{
    const std::string fullKey = NormalizeKeyFull(englishTemplate);
    const std::string key = CompactKey(fullKey);
    if (collectTemplates_)
    {
        collectedTemplates_.emplace(key, std::string(englishTemplate));
        collectedArgumentExamples_.emplace(key, arguments);
    }
    if (locale_ == "pseudo")
    {
        if (pseudoReportedKeys_.insert(key).second)
        {
            auto it = enMessages_.find(key);
            if (it == enMessages_.end() && fullKey != key)
                it = enMessages_.find(fullKey);
            bool present = it != enMessages_.end() && !it->second.empty();
            std::cerr << std::format("[pseudo] warning: en.json entry {}: {}\n",
                                     present ? "present" : "missing", key);
        }
        return FormatSourceTemplate(englishTemplate, arguments);
    }

    auto it = messages_.find(key);
    if (it == messages_.end() && fullKey != key)
        it = messages_.find(fullKey);
    if (it != messages_.end() && !it->second.empty() &&
        HasValidNumberedPlaceholders(it->second, arguments.size()))
        return FormatNumberedTemplate(it->second, arguments);

    return FormatSourceTemplate(englishTemplate, arguments);
}

bool DiagnosticLocalization::WriteCollectedCatalog(const std::string& locale,
                                                     bool verbose) const
{
    if (locale.empty() || locale == "pseudo")
    {
        std::cerr << "Error: --update-locale requires a real locale name.\n";
        return false;
    }

    std::unordered_map<std::string, std::string> messages;
    std::unordered_map<std::string, std::vector<std::string>> argumentExamples;
    const auto& localeDirectory = localeDirectory_;
    const auto outputPath = localeDirectory / (locale + ".json");
    if (std::filesystem::exists(outputPath))
    {
        if (!LoadCatalog(outputPath, messages, verbose,
                         locale == "en-pseudo" ? &argumentExamples : nullptr))
        {
            std::cerr << std::format("Error: could not read existing locale '{}'.\n",
                                     outputPath.string());
            return false;
        }
    }

    std::unordered_map<std::string, std::string> compactMessages;
    for (const auto& [key, value] : messages)
    {
        std::string compactKey = CompactKey(key);
        auto it = compactMessages.find(compactKey);
        if (it == compactMessages.end() || it->second.empty())
            compactMessages[std::move(compactKey)] = value;
    }
    messages = std::move(compactMessages);

    std::unordered_map<std::string, std::vector<std::string>> compactArgumentExamples;
    for (const auto& [key, examples] : argumentExamples)
    {
        std::string compactKey = CompactKey(key);
        if (!compactArgumentExamples.contains(compactKey))
            compactArgumentExamples[std::move(compactKey)] = examples;
    }

    for (const auto& [key, sourceTemplate] : collectedTemplates_)
    {
        if (!messages.contains(key) || messages[key].empty())
            messages[key] = SourceTemplateWithNumberedArguments(sourceTemplate);
    }

    if (locale == "en-pseudo")
    {
        for (const auto& [key, examples] : collectedArgumentExamples_)
        {
            if (!compactArgumentExamples.contains(key) || compactArgumentExamples[key].empty())
                compactArgumentExamples[key] = examples;
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(localeDirectory, ec);
    if (ec)
    {
        std::cerr << std::format("Error: could not create locale directory '{}': {}\n",
                                 localeDirectory.string(), ec.message());
        return false;
    }

    Json root;
    root["locale"] = locale;
    root["messages"] = Json::object();
    for (const auto& [key, value] : messages)
        root["messages"][key] = value;
    if (locale == "en-pseudo")
    {
        root["argumentExamples"] = Json::object();
        for (const auto& [key, examples] : compactArgumentExamples)
            root["argumentExamples"][key] = examples;
    }

    std::ofstream output(outputPath, std::ios::trunc);
    if (!output)
    {
        std::cerr << std::format("Error: could not write locale '{}'.\n", outputPath.string());
        return false;
    }
    output << root.dump(2) << '\n';
    return output.good();
}
