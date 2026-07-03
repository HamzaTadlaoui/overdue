#pragma once
#include <filesystem>
#include <string>

// User-tunable parameters, persisted as JSON at Config::default_path()
// (~/.config/overdue/config.json by default; overridable with $OVERDUE_CONFIG).
struct Config {
    // Directory holding data.json. Kept separate from the config file so the two
    // can live in standard XDG locations and so a throwaway dev profile can point
    // at its own data without touching your real history.
    std::filesystem::path data_dir;

    // How long an unlogged entry stays recoverable before it is purged for good.
    long long unlog_grace_secs = 86400; // default: 24h

    // Default port for `overdue web` (still overridable per-run with --port).
    int web_port = 8080;

    // chrono/strftime-style format used to render timestamps (see format_datetime).
    std::string date_format = "%Y-%m-%d %H:%M:%S";

    // Whether `overdue check` sends desktop notifications.
    bool notify_enabled = true;

    static Config load(const std::filesystem::path& path);
    static void save(const std::filesystem::path& path, const Config& cfg);

    // Location of config.json: $OVERDUE_CONFIG, else $XDG_CONFIG_HOME/overdue,
    // else ~/.config/overdue.
    static std::filesystem::path default_path();
    // Default data directory: $XDG_DATA_HOME/overdue, else ~/.local/share/overdue.
    static std::filesystem::path default_data_dir();

    // Full path to the activity data file inside data_dir.
    std::filesystem::path data_path() const { return data_dir / "data.json"; }
};
