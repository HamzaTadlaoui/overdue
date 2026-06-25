#pragma once
#include <chrono>
#include <optional>
#include <string>
#include <vector>

struct Activity {
    std::string name;
    std::vector<std::chrono::system_clock::time_point> logs;
    std::optional<long long> alert_after; // seconds
};

inline std::chrono::system_clock::time_point now() {
    return std::chrono::system_clock::now();
}

inline const std::chrono::system_clock::time_point& last_done(const Activity& a) {
    return a.logs.back();
}

inline std::string format_datetime(const std::chrono::system_clock::time_point& tp) {
    auto dp = std::chrono::floor<std::chrono::days>(tp);
    std::chrono::year_month_day ymd{dp};
    std::chrono::hh_mm_ss hms{std::chrono::duration_cast<std::chrono::seconds>(tp - dp)};
    return std::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}",
        static_cast<int>(ymd.year()),
        static_cast<unsigned>(ymd.month()),
        static_cast<unsigned>(ymd.day()),
        hms.hours().count(),
        hms.minutes().count(),
        hms.seconds().count());
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

// Parses "2026-06-22" or "2026-06-22T08:15" or "2026-06-22T08:15:00"
inline std::optional<std::chrono::system_clock::time_point> parse_at(const std::string& s) {
    if (s.size() < 10) return std::nullopt;
    try {
        int y = std::stoi(s.substr(0, 4));
        unsigned mo = std::stoi(s.substr(5, 2));
        unsigned d = std::stoi(s.substr(8, 2));
        std::chrono::year_month_day ymd{std::chrono::year{y}, std::chrono::month{mo}, std::chrono::day{d}};
        if (!ymd.ok()) return std::nullopt;
        std::chrono::system_clock::time_point tp = std::chrono::sys_days{ymd};
        if (s.size() >= 16) {
            long long h = std::stoi(s.substr(11, 2));
            long long m = std::stoi(s.substr(14, 2));
            long long sec = (s.size() >= 19) ? std::stoi(s.substr(17, 2)) : 0;
            tp += std::chrono::hours{h} + std::chrono::minutes{m} + std::chrono::seconds{sec};
        }
        return tp;
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
