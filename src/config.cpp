#include "config.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>

using json = nlohmann::json;

Config Config::load(const std::filesystem::path& path) {
    Config cfg;
    if (!std::filesystem::exists(path)) return cfg;

    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open " + path.string());

    json j = json::parse(f);
    // Tolerate a partial/old config: only override what is present, keep defaults
    // for anything missing or unknown.
    if (j.contains("unlog_grace_secs"))
        cfg.unlog_grace_secs = j["unlog_grace_secs"].get<long long>();
    return cfg;
}

void Config::save(const std::filesystem::path& path, const Config& cfg) {
    std::filesystem::create_directories(path.parent_path());

    json j = {
        {"unlog_grace_secs", cfg.unlog_grace_secs}
    };

    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) throw std::runtime_error("Cannot write " + tmp.string());
        f << j.dump(2) << '\n';
        if (!f) throw std::runtime_error("Write failed: " + tmp.string());
    }
    std::filesystem::rename(tmp, path);
}

std::filesystem::path Config::default_path() {
    const char* home = std::getenv("HOME");
    if (!home) throw std::runtime_error("HOME environment variable not set");
    return std::filesystem::path(home) / ".local/share/overdue/config.json";
}
