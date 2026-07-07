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

    // How timestamps are rendered. Two modes coexist:
    //  * "custom": date_format holds a raw chrono/strftime string (set directly
    //    or via a named preset like "us"); it is used verbatim.
    //  * "structured": date_format is empty and the format is composed from the
    //    knobs below (order/separator/clock/seconds) by effective_date_format().
    // Setting date_format switches to custom; setting any knob clears it back to
    // structured. Empty default => structured, which yields "%Y-%m-%d %H:%M:%S".
    std::string date_format;          // raw override; empty => use the knobs
    std::string date_order = "ymd";   // ymd | dmy | mdy
    std::string date_sep   = "-";     // separator between date parts: - . / or space
    std::string clock      = "24h";   // 24h | 12h
    bool show_seconds      = true;    // include :SS in the time

    // The chrono format actually applied: date_format if set, else built from
    // date_order/date_sep/clock/show_seconds.
    std::string effective_date_format() const;

    // IANA time zone (e.g. "Europe/Paris") used to render and day/week-bucket
    // every timestamp. Empty follows the system zone. See active_zone().
    std::string timezone;

    // First day of the week for weekly streaks and the calendar heatmap:
    // "monday" (ISO default) or "sunday".
    std::string week_start = "monday";

    // Whether `overdue check` sends desktop notifications.
    bool notify_enabled = true;

    // Minimum time between repeat notifications for the same overdue activity,
    // so a frequently-scheduled `overdue check` (e.g. cron) doesn't spam. 0
    // disables throttling. Last-sent times live in <data_dir>/notify_state.json.
    long long notify_cooldown_secs = 3600; // default: 1h

    // Optional nightly quiet window, as local hours [start, end), during which
    // `overdue check` stays silent. Equal values disable it; the window may wrap
    // past midnight (e.g. 22 → 7 means 22:00 through 06:59).
    int notify_quiet_start = 0;
    int notify_quiet_end = 0;

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
