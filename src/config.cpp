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
    if (j.contains("date_order"))
        cfg.date_order = j["date_order"].get<std::string>();
    if (j.contains("date_sep"))
        cfg.date_sep = j["date_sep"].get<std::string>();
    if (j.contains("clock"))
        cfg.clock = j["clock"].get<std::string>();
    if (j.contains("show_seconds"))
        cfg.show_seconds = j["show_seconds"].get<bool>();
    if (j.contains("timezone"))
        cfg.timezone = j["timezone"].get<std::string>();
    if (j.contains("week_start"))
        cfg.week_start = j["week_start"].get<std::string>();
    if (j.contains("notify_enabled"))
        cfg.notify_enabled = j["notify_enabled"].get<bool>();
    if (j.contains("notify_cooldown_secs"))
        cfg.notify_cooldown_secs = j["notify_cooldown_secs"].get<long long>();
    if (j.contains("notify_quiet_start"))
        cfg.notify_quiet_start = j["notify_quiet_start"].get<int>();
    if (j.contains("notify_quiet_end"))
        cfg.notify_quiet_end = j["notify_quiet_end"].get<int>();
    return cfg;
}

void Config::save(const std::filesystem::path& path, const Config& cfg) {
    std::filesystem::create_directories(path.parent_path());

    json j = {
        {"data_dir", cfg.data_dir.string()},
        {"unlog_grace_secs", cfg.unlog_grace_secs},
        {"web_port", cfg.web_port},
        {"date_format", cfg.date_format},
        {"date_order", cfg.date_order},
        {"date_sep", cfg.date_sep},
        {"clock", cfg.clock},
        {"show_seconds", cfg.show_seconds},
        {"timezone", cfg.timezone},
        {"week_start", cfg.week_start},
        {"notify_enabled", cfg.notify_enabled},
        {"notify_cooldown_secs", cfg.notify_cooldown_secs},
        {"notify_quiet_start", cfg.notify_quiet_start},
        {"notify_quiet_end", cfg.notify_quiet_end}
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

std::string Config::effective_date_format() const {
    // A raw/preset string wins; the knobs are only consulted in structured mode.
    if (!date_format.empty()) return date_format;

    const std::string& s = date_sep;
    std::string date;
    if (date_order == "dmy")      date = "%d" + s + "%m" + s + "%Y";
    else if (date_order == "mdy") date = "%m" + s + "%d" + s + "%Y";
    else                          date = "%Y" + s + "%m" + s + "%d"; // ymd default

    std::string time = (clock == "12h")
        ? (show_seconds ? "%I:%M:%S %p" : "%I:%M %p")
        : (show_seconds ? "%H:%M:%S"    : "%H:%M");

    return date + " " + time;
}

std::filesystem::path Config::default_data_dir() {
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "overdue";
    return std::filesystem::path(require_home()) / ".local" / "share" / "overdue";
}
