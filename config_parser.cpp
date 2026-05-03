#include "config_parser.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>

bool ConfigParser::load(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) return false;
    sections_.clear();
    std::string current_section, line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || isComment(line)) continue;
        if (line.front() == '[' && line.back() == ']') {
            current_section = line.substr(1, line.size() - 2);
            sections_.push_back({current_section, {}});
            continue;
        }
        if (!current_section.empty()) {
            size_t eq = line.find('=');
            if (eq != std::string::npos) sections_.back().values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
        }
    }
    return true;
}

std::optional<std::string> ConfigParser::getValue(const std::string& section, const std::string& key, size_t index) const {
    size_t count = 0;
    for (const auto& sec : sections_) {
        if (sec.name == section) {
            if (count == index) { auto it = sec.values.find(key); return (it != sec.values.end()) ? std::optional(it->second) : std::nullopt; }
            count++;
        }
    }
    return std::nullopt;
}

size_t ConfigParser::countSections(const std::string& section) const {
    size_t c = 0; for (const auto& sec : sections_) if (sec.name == section) c++; return c;
}

int ConfigParser::getInt(const std::string& s, const std::string& k, int f, size_t i) const {
    auto v = getValue(s, k, i); return v ? std::stoi(*v) : f;
}
std::string ConfigParser::getString(const std::string& s, const std::string& k, const std::string& f, size_t i) const {
    auto v = getValue(s, k, i); return v ? *v : f;
}
std::string ConfigParser::getIp(const std::string& s, const std::string& k, const std::string& f, size_t i) const {
    auto v = getValue(s, k, i); if (v && !v->empty()) return *v;
    auto rtsp = getValue(s, "rtsp", i);
    if (rtsp && !rtsp->empty()) {
        std::string url = *rtsp;
        size_t p = url.find("://"); if (p != std::string::npos) url = url.substr(p + 3);
        size_t at = url.find('@'); if (at != std::string::npos) url = url.substr(at + 1);
        size_t c = url.find(':'); if (c != std::string::npos) url = url.substr(0, c);
        size_t sl = url.find('/'); if (sl != std::string::npos) url = url.substr(0, sl);
        return url;
    }
    return f;
}
std::string ConfigParser::trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}
bool ConfigParser::isComment(const std::string& l) { return !l.empty() && (l[0] == '#' || l[0] == ';'); }