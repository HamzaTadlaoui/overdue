#pragma once
#include <chrono>
#include <string>
#include <vector>

struct Activity {
    std::string name;
    std::vector<std::chrono::system_clock::time_point> logs;
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
