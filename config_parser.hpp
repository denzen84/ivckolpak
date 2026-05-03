#pragma once
#include <string>
#include <vector>
#include <map>
#include <optional>

class ConfigParser {
public:
    bool load(const std::string& filepath);
    std::optional<std::string> getValue(const std::string& section, const std::string& key, size_t index = 0) const;
    size_t countSections(const std::string& section) const;
    int getInt(const std::string& section, const std::string& key, int fallback, size_t index = 0) const;
    std::string getString(const std::string& section, const std::string& key, const std::string& fallback, size_t index = 0) const;
    std::string getIp(const std::string& section, const std::string& key, const std::string& fallback, size_t index = 0) const;

private:
    struct SectionData { std::string name; std::map<std::string, std::string> values; };
    std::vector<SectionData> sections_;
    static std::string trim(const std::string& str);
    static bool isComment(const std::string& line);
};