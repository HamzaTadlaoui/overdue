#pragma once
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <vector>

enum class ActivityType { Habit, Task };

enum class StreakMode { Interval, Calendar };
enum class CalendarUnit { Day, Week, Month };

struct StreakConfig {
    StreakMode mode;
    long long interval_secs = 0;
    CalendarUnit unit = CalendarUnit::Day;
};

struct LogEntry {
    std::chrono::system_clock::time_point when;
    std::optional<double> amount; // nullopt = presence-only log
};

// A log that was unlogged but not yet purged: kept out of Activity::logs so all
// stats/streak/web read paths see only active logs, but recoverable until the
// grace window (Config::unlog_grace_secs) elapses.
struct UnloggedEntry {
    LogEntry entry;                                    // original when + amount
    std::chrono::system_clock::time_point unlogged_at; // when unlog happened
};

struct Activity {
    std::string name;
    ActivityType type = ActivityType::Habit;
    std::vector<LogEntry> logs;
    std::vector<UnloggedEntry> unlogged; // soft-deleted logs pending purge
    std::optional<std::chrono::system_clock::time_point> completed_at;
    std::optional<long long> alert_after;
    std::optional<StreakConfig> streak;
    std::optional<std::string> unit;   // display label for amounts, e.g. "km"
    std::optional<double> target;      // optional goal for accumulated amount
    std::vector<std::string> tags;     // free-form categories, kept sorted+unique
};

inline std::chrono::system_clock::time_point now() {
    return std::chrono::system_clock::now();
}

inline const std::chrono::system_clock::time_point& last_done(const Activity& a) {
    return a.logs.back().when;
}

// chrono/strftime-style format applied to timestamps. Set once at startup from
// Config::date_format; a single definition is shared across translation units.
inline std::string& datetime_format() {
    static std::string fmt = "%Y-%m-%d %H:%M:%S";
    return fmt;
}

// Configured IANA time-zone name (e.g. "Europe/Paris"); empty = follow the
// system zone. Set once at startup from Config::timezone, shared across
// translation units (same pattern as datetime_format()).
inline std::string& timezone_name() {
    static std::string tz;
    return tz;
}

// The zone all timestamps are rendered and day/week-bucketed in: the configured
// zone when set and valid, otherwise the system zone. An unknown name silently
// falls back to the system zone so a bad config never breaks a read path.
inline const std::chrono::time_zone* active_zone() {
    const std::string& tz = timezone_name();
    if (!tz.empty()) {
        try { return std::chrono::locate_zone(tz); }
        catch (...) { /* unknown zone — fall through */ }
    }
    return std::chrono::current_zone();
}

// True if `tz` names a zone the system knows about (used to validate config).
inline bool is_valid_timezone(const std::string& tz) {
    try { (void)std::chrono::locate_zone(tz); return true; }
    catch (...) { return false; }
}

// Fixed ISO rendering used when no/invalid custom format is in play.
inline std::string format_datetime_iso(const std::chrono::local_seconds& local) {
    auto dp = std::chrono::floor<std::chrono::days>(local);
    std::chrono::year_month_day ymd{dp};
    std::chrono::hh_mm_ss hms{local - dp};
    return std::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}",
        static_cast<int>(ymd.year()),
        static_cast<unsigned>(ymd.month()),
        static_cast<unsigned>(ymd.day()),
        hms.hours().count(),
        hms.minutes().count(),
        hms.seconds().count());
}

inline std::string format_datetime(const std::chrono::system_clock::time_point& tp) {
    auto local = std::chrono::floor<std::chrono::seconds>(
        active_zone()->to_local(tp));
    try {
        return std::vformat("{:" + datetime_format() + "}", std::make_format_args(local));
    } catch (...) {
        // A malformed user format must never crash a read path — fall back to ISO.
        return format_datetime_iso(local);
    }
}

// True if `fmt` is a usable chrono format spec (used to validate `config set`).
inline bool is_valid_datetime_format(const std::string& fmt) {
    std::chrono::local_seconds local = std::chrono::floor<std::chrono::seconds>(
        active_zone()->to_local(std::chrono::system_clock::now()));
    try {
        (void)std::vformat("{:" + fmt + "}", std::make_format_args(local));
        return true;
    } catch (...) { return false; }
}

inline std::string format_elapsed(const std::chrono::system_clock::time_point& last) {
    auto total = std::chrono::duration_cast<std::chrono::seconds>(now() - last).count();

    long long d = total / 86400;
    long long h = (total % 86400) / 3600;
    long long m = (total % 3600) / 60;
    long long s = total % 60;

    if (d > 0) return std::format("{}d {}h {}m {}s", d, h, m, s);
    if (h > 0) return std::format("{}h {}m {}s", h, m, s);
    if (m > 0) return std::format("{}m {}s", m, s);
    return std::format("{}s", s);
}

// Parses "2026-06-22" or "2026-06-22T08:15" or "2026-06-22T08:15:00" as local time
inline std::optional<std::chrono::system_clock::time_point> parse_at(const std::string& s) {
    if (s.size() < 10) return std::nullopt;
    try {
        int y = std::stoi(s.substr(0, 4));
        // system_clock can't represent dates outside ~1678–2262; out-of-range
        // years would silently wrap into the past and bypass the future-date guard.
        if (y < 1678 || y > 2261) return std::nullopt;
        unsigned mo = std::stoi(s.substr(5, 2));
        unsigned d = std::stoi(s.substr(8, 2));
        std::chrono::year_month_day ymd{std::chrono::year{y}, std::chrono::month{mo}, std::chrono::day{d}};
        if (!ymd.ok()) return std::nullopt;
        auto local = std::chrono::local_days{ymd};
        std::chrono::local_seconds local_sec{local};
        if (s.size() >= 16) {
            long long h   = std::stoi(s.substr(11, 2));
            long long m   = std::stoi(s.substr(14, 2));
            long long sec = (s.size() >= 19) ? std::stoi(s.substr(17, 2)) : 0;
            local_sec += std::chrono::hours{h} + std::chrono::minutes{m} + std::chrono::seconds{sec};
        }
        return active_zone()->to_sys(local_sec);
    } catch (...) { return std::nullopt; }
}

// Parses durations like "3d", "12h", "30m", "1d12h", "1d6h30m"
inline std::optional<long long> parse_duration(const std::string& s) {
    long long total = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return std::nullopt;
        long long val = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
            val = val * 10 + (s[i++] - '0');
        if (i >= s.size()) return std::nullopt;
        switch (s[i++]) {
            case 'd': total += val * 86400; break;
            case 'h': total += val * 3600;  break;
            case 'm': total += val * 60;    break;
            case 's': total += val;         break;
            default:  return std::nullopt;
        }
    }
    return total > 0 ? std::optional<long long>{total} : std::nullopt;
}

inline std::string format_duration(long long seconds) {
    long long d = seconds / 86400;
    long long h = (seconds % 86400) / 3600;
    long long m = (seconds % 3600) / 60;
    long long s = seconds % 60;
    if (d > 0 && h > 0) return std::format("{}d {}h", d, h);
    if (d > 0)           return std::format("{}d", d);
    if (h > 0 && m > 0) return std::format("{}h {}m", h, m);
    if (h > 0)           return std::format("{}h", h);
    if (m > 0)           return std::format("{}m", m);
    return std::format("{}s", s);
}

// "daily"/"weekly"/"monthly" → calendar; "3d"/"12h"/… → interval
inline std::optional<StreakConfig> parse_streak(const std::string& s) {
    if (s == "daily")   return StreakConfig{StreakMode::Calendar, 0, CalendarUnit::Day};
    if (s == "weekly")  return StreakConfig{StreakMode::Calendar, 0, CalendarUnit::Week};
    if (s == "monthly") return StreakConfig{StreakMode::Calendar, 0, CalendarUnit::Month};
    auto dur = parse_duration(s);
    if (!dur) return std::nullopt;
    return StreakConfig{StreakMode::Interval, *dur, CalendarUnit::Day};
}

inline std::string format_streak_label(const StreakConfig& sc) {
    if (sc.mode == StreakMode::Calendar) {
        switch (sc.unit) {
            case CalendarUnit::Day:   return "daily";
            case CalendarUnit::Week:  return "weekly";
            case CalendarUnit::Month: return "monthly";
        }
    }
    return format_duration(sc.interval_secs);
}

// Renders a quantity without noisy trailing zeros: 30.0 -> "30", 5.20 -> "5.2"
inline std::string format_amount(double v) {
    std::string s = std::format("{:.4f}", v);
    auto dot = s.find('.');
    if (dot != std::string::npos) {
        auto last = s.find_last_not_of('0');
        if (last == dot) --last; // drop a now-bare decimal point
        s.erase(last + 1);
    }
    return s;
}

// Trims surrounding whitespace and lowercases a tag so lookups and de-duplication
// are case- and padding-insensitive ("Work", " work " and "work" are one tag).
inline std::string normalize_tag(const std::string& s) {
    auto begin = s.find_first_not_of(" \t");
    if (begin == std::string::npos) return {};
    auto end = s.find_last_not_of(" \t");
    std::string out = s.substr(begin, end - begin + 1);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// True if `a` carries `tag` (compared after normalization).
inline bool has_tag(const Activity& a, const std::string& tag) {
    std::string t = normalize_tag(tag);
    return std::find(a.tags.begin(), a.tags.end(), t) != a.tags.end();
}

// Comma-space joined tag list for display, e.g. "health, morning".
inline std::string format_tags(const std::vector<std::string>& tags) {
    std::string out;
    for (const auto& t : tags) {
        if (!out.empty()) out += ", ";
        out += t;
    }
    return out;
}

// Parses a non-negative quantity; rejects trailing junk and non-finite values.
inline std::optional<double> parse_amount(const std::string& s) {
    try {
        std::size_t pos = 0;
        double v = std::stod(s, &pos);
        if (pos != s.size() || !std::isfinite(v) || v < 0) return std::nullopt;
        return v;
    } catch (...) { return std::nullopt; }
}
