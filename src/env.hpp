#pragma once
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>

inline void load_dotenv(const std::string& path = ".env") {
    std::ifstream f(path);
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // Don't overwrite vars already set in environment
        if (!getenv(key.c_str()))
            setenv(key.c_str(), val.c_str(), 0);
    }
}

inline std::string require_env(const std::string& key) {
    const char* v = getenv(key.c_str());
    if (!v || !*v)
        throw std::runtime_error("Missing required env var: " + key);
    return v;
}

inline std::string get_env(const std::string& key, const std::string& def = "") {
    const char* v = getenv(key.c_str());
    return (v && *v) ? v : def;
}
