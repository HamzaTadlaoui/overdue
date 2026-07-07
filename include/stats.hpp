#pragma once
#include "activity.hpp"
#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

struct HabitStat {
    std::string name;
    size_t log_count;
    std::optional<long long> avg_interval;
    std::optional<long long> alert_after;
    std::optional<StreakConfig> streak_config;
    int streak = 0;
};

struct GlobalStats {
    size_t habit_count;
    size_t total_logs;
    size_t task_total;
    size_t task_done;
    std::optional<HabitStat> most_consistent;
    std::optional<HabitStat> most_neglected;
    std::optional<HabitStat> most_logged;
    std::optional<HabitStat> best_streak;
};

inline std::optional<long long> avg_interval(const Activity& a) {
    if (a.logs.size() < 2) return std::nullopt;
    auto span = std::chrono::duration_cast<std::chrono::seconds>(
        a.logs.back().when - a.logs.front().when).count();
    return span / static_cast<long long>(a.logs.size() - 1);
}

inline std::chrono::local_days to_local_days(const std::chrono::system_clock::time_point& tp) {
    return std::chrono::floor<std::chrono::days>(
        active_zone()->to_local(tp));
}

// First day of the week for weekly streaks and the calendar heatmap. Set once
// at startup from Config::week_start (defaults to Monday, the ISO week start).
inline std::chrono::weekday& week_start_day() {
    static std::chrono::weekday d = std::chrono::Monday;
    return d;
}

// Snaps `d` back to the most recent configured week-start day (inclusive).
inline std::chrono::local_days week_start(std::chrono::local_days d) {
    unsigned cur   = std::chrono::weekday{d}.c_encoding();   // Sun=0 .. Sat=6
    unsigned first = week_start_day().c_encoding();
    unsigned back  = (cur + 7 - first) % 7;
    return d - std::chrono::days{back};
}

using YM = std::pair<int, unsigned>;
inline YM to_year_month(std::chrono::local_days d) {
    std::chrono::year_month_day ymd{d};
    return {static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month())};
}
inline YM prev_ym(YM ym) {
    return ym.second == 1 ? YM{ym.first - 1, 12} : YM{ym.first, ym.second - 1};
}

inline int compute_streak(const Activity& a) {
    if (!a.streak || a.logs.empty()) return 0;

    if (a.streak->mode == StreakMode::Interval) {
        long long interval = a.streak->interval_secs;
        auto since_last = std::chrono::duration_cast<std::chrono::seconds>(
            now() - a.logs.back().when).count();
        if (since_last > interval) return 0;
        int s = 1;
        for (int i = static_cast<int>(a.logs.size()) - 1; i > 0; --i) {
            auto gap = std::chrono::duration_cast<std::chrono::seconds>(
                a.logs[i].when - a.logs[i - 1].when).count();
            if (gap <= interval) ++s; else break;
        }
        return s;
    }

    // Calendar mode — all comparisons in local time so midnight = user's midnight
    auto today = to_local_days(now());

    if (a.streak->unit == CalendarUnit::Day) {
        std::set<std::chrono::local_days> days;
        for (const auto& e : a.logs) days.insert(to_local_days(e.when));
        auto start = days.contains(today) ? today : today - std::chrono::days{1};
        if (!days.contains(start)) return 0;
        int s = 0;
        for (auto d = start; days.contains(d); d -= std::chrono::days{1}) ++s;
        return s;
    }

    if (a.streak->unit == CalendarUnit::Week) {
        std::set<std::chrono::local_days> weeks;
        for (const auto& e : a.logs) weeks.insert(week_start(to_local_days(e.when)));
        auto this_week = week_start(today);
        auto start = weeks.contains(this_week) ? this_week : this_week - std::chrono::days{7};
        if (!weeks.contains(start)) return 0;
        int s = 0;
        for (auto d = start; weeks.contains(d); d -= std::chrono::days{7}) ++s;
        return s;
    }

    // Month
    std::set<YM> months;
    for (const auto& e : a.logs) months.insert(to_year_month(to_local_days(e.when)));
    auto this_month = to_year_month(today);
    auto start = months.contains(this_month) ? this_month : prev_ym(this_month);
    if (!months.contains(start)) return 0;
    int s = 0;
    for (auto m = start; months.contains(m); m = prev_ym(m)) ++s;
    return s;
}

// Aggregate stats over the logs that carry an amount. All quantities are only
// comparable within one activity (km vs pushups), so this is strictly per-activity.
struct QuantityStats {
    size_t count = 0;       // logs that carry an amount
    double total = 0;
    double avg_per_log = 0;
    double avg_per_day = 0; // total / distinct days with amounts
    double max_single = 0;
    double best_day = 0;
    double last7 = 0;       // sum over the last 7 days
    double prev7 = 0;       // sum over the 7 days before that
};

inline std::optional<QuantityStats> quantity_stats(const Activity& a) {
    QuantityStats qs;
    std::map<std::chrono::local_days, double> per_day;
    auto today = to_local_days(now());

    for (const auto& e : a.logs) {
        if (!e.amount) continue;
        ++qs.count;
        qs.total += *e.amount;
        qs.max_single = std::max(qs.max_single, *e.amount);
        auto day = to_local_days(e.when);
        per_day[day] += *e.amount;
        auto age = (today - day).count();
        if (age >= 0 && age < 7)       qs.last7 += *e.amount;
        else if (age >= 7 && age < 14) qs.prev7 += *e.amount;
    }
    if (qs.count == 0) return std::nullopt;

    qs.avg_per_log = qs.total / static_cast<double>(qs.count);
    qs.avg_per_day = qs.total / static_cast<double>(per_day.size());
    for (const auto& [d, v] : per_day) qs.best_day = std::max(qs.best_day, v);
    return qs;
}

inline GlobalStats compute_global(const std::vector<Activity>& activities) {
    GlobalStats gs{};
    std::vector<HabitStat> habit_stats;

    for (const auto& a : activities) {
        if (a.type == ActivityType::Task) {
            ++gs.task_total;
            if (a.completed_at) ++gs.task_done;
            continue;
        }
        ++gs.habit_count;
        gs.total_logs += a.logs.size();

        HabitStat hs;
        hs.name = a.name;
        hs.log_count = a.logs.size();
        hs.avg_interval = avg_interval(a);
        hs.alert_after = a.alert_after;
        hs.streak_config = a.streak;
        hs.streak = compute_streak(a);
        habit_stats.push_back(hs);
    }

    for (const auto& hs : habit_stats) {
        if (!gs.most_logged || hs.log_count > gs.most_logged->log_count)
            gs.most_logged = hs;

        if (hs.avg_interval) {
            if (!gs.most_consistent || *hs.avg_interval < *gs.most_consistent->avg_interval)
                gs.most_consistent = hs;
            if (!gs.most_neglected || *hs.avg_interval > *gs.most_neglected->avg_interval)
                gs.most_neglected = hs;
        }

        if (hs.streak_config) {
            if (!gs.best_streak || hs.streak > gs.best_streak->streak)
                gs.best_streak = hs;
        }
    }

    if (gs.most_consistent && gs.most_neglected &&
        gs.most_consistent->name == gs.most_neglected->name)
        gs.most_neglected = std::nullopt;

    return gs;
}
