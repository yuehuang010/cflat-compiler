#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class DiagnosticLocalization
{
public:
    void SetLocale(std::string locale);
    void SetLocaleDirectory(std::string directory);
    void SetCollectTemplates(bool enabled);

    // Load the default English catalog and the selected locale.
    // Failure leaves the source-template fallback available.
    bool Load(bool verbose = false);

    std::string Localize(std::string_view englishTemplate,
                         const std::vector<std::string>& arguments) const;

    // Format the canonical source-language diagnostic for test matching.
    static std::string FormatSourceTemplate(std::string_view englishTemplate,
                                             const std::vector<std::string>& arguments);

    // Add encountered source templates to <localeDirectory>/<locale>.json.
    bool WriteCollectedCatalog(const std::string& locale, bool verbose = false) const;

    static std::string NormalizeKey(std::string_view englishTemplate);

private:
    bool LoadCatalog(const std::filesystem::path& path,
                     std::unordered_map<std::string, std::string>& messages,
                     bool verbose,
                     std::unordered_map<std::string, std::vector<std::string>>* argumentExamples = nullptr) const;

    std::string locale_ = "en-simple";
    std::filesystem::path localeDirectory_;
    std::unordered_map<std::string, std::string> messages_;
    std::unordered_map<std::string, std::string> enSimpleMessages_;
    mutable std::unordered_map<std::string, std::string> collectedTemplates_;
    mutable std::unordered_map<std::string, std::vector<std::string>> collectedArgumentExamples_;
    bool collectTemplates_ = false;
    mutable std::unordered_set<std::string> pseudoReportedKeys_;
};
