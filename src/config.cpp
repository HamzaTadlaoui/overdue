#include "config.hpp"
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

using json = nlohmann::json;

static std::string require_home() {
    const char* home = std::getenv("HOME");
    if (!home) throw std::runtime_error("HOME environment variable not set");
    return home;
}

Config Config::load(const std::filesystem::path& path) {
    Config cfg;
    cfg.data_dir = default_data_dir(); // filled first so it survives an absent file
    if (!std::filesystem::exists(path)) return cfg;

    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open " + path.string());

    json j = json::parse(f);
    // Tolerate a partial/old config: only override what is present, keep defaults
    // for anything missing or unknown.
    if (j.contains("data_dir"))
        cfg.data_dir = j["data_dir"].get<std::string>();
    if (j.contains("unlog_grace_secs"))
        cfg.unlog_grace_secs = j["unlog_grace_secs"].get<long long>();
    if (j.contains("web_port"))
        cfg.web_port = j["web_port"].get<int>();
    if (j.contains("date_format"))
        cfg.date_format = j["date_format"].get<std::string>();
    if (j.contains("notify_enabled"))
        cfg.notify_enabled = j["notify_enabled"].get<bool>();
    return cfg;
}

void Config::save(const std::filesystem::path& path, const Config& cfg) {
    std::filesystem::create_directories(path.parent_path());

    json j = {
        {"data_dir", cfg.data_dir.string()},
        {"unlog_grace_secs", cfg.unlog_grace_secs},
        {"web_port", cfg.web_port},
        {"date_format", cfg.date_format},
        {"notify_enabled", cfg.notify_enabled}
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
    // An explicit config file wins — this is how you select a dev/real profile.
    if (const char* c = std::getenv("OVERDUE_CONFIG"); c && *c)
        return c;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "overdue" / "config.json";
    return std::filesystem::path(require_home()) / ".config" / "overdue" / "config.json";
}

std::filesystem::path Config::default_data_dir() {
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "overdue";
    return std::filesystem::path(require_home()) / ".local" / "share" / "overdue";
}
