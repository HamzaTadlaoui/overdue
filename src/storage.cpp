#include "storage.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>

using json = nlohmann::json;

static std::chrono::system_clock::time_point from_unix(long long t) {
    return std::chrono::system_clock::time_point{std::chrono::seconds{t}};
}

static long long to_unix(const std::chrono::system_clock::time_point& tp) {
    return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
}

std::vector<Activity> Storage::load(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return {};

    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open " + path.string());

    std::vector<Activity> result;
    for (const auto& item : json::parse(f)) {
        Activity a;
        a.name = item["name"].get<std::string>();
        a.type = (item.value("type", "habit") == "task") ? ActivityType::Task : ActivityType::Habit;
        for (const auto& l : item["logs"]) {
            // Old format: bare unix number. New format: { "t": unix, "q": amount? }.
            if (l.is_object()) {
                LogEntry e{from_unix(l["t"].get<long long>()), std::nullopt};
                if (l.contains("q")) e.amount = l["q"].get<double>();
                a.logs.push_back(e);
            } else {
                a.logs.push_back({from_unix(l.get<long long>()), std::nullopt});
            }
        }
        if (a.logs.empty())
            throw std::runtime_error("Activity \"" + a.name + "\" has no logs — data may be corrupted");
        if (item.contains("unlogged")) {
            for (const auto& u : item["unlogged"]) {
                LogEntry e{from_unix(u["t"].get<long long>()), std::nullopt};
                if (u.contains("q")) e.amount = u["q"].get<double>();
                a.unlogged.push_back(UnloggedEntry{e, from_unix(u["u"].get<long long>())});
            }
        }
        if (item.contains("completed_at"))
            a.completed_at = from_unix(item["completed_at"].get<long long>());
        if (item.contains("alert_after"))
            a.alert_after = item["alert_after"].get<long long>();
        if (item.contains("unit"))
            a.unit = item["unit"].get<std::string>();
        if (item.contains("target"))
            a.target = item["target"].get<double>();
        if (item.contains("streak")) {
            const auto& s = item["streak"];
            std::string mode = s["mode"].get<std::string>();
            if (mode == "interval") {
                a.streak = StreakConfig{StreakMode::Interval, s["secs"].get<long long>()};
            } else {
                std::string unit = s["unit"].get<std::string>();
                CalendarUnit cu = unit == "week" ? CalendarUnit::Week
                                : unit == "month" ? CalendarUnit::Month
                                : CalendarUnit::Day;
                a.streak = StreakConfig{StreakMode::Calendar, 0, cu};
            }
        }
        result.push_back(std::move(a));
    }
    return result;
}

void Storage::save(const std::filesystem::path& path, const std::vector<Activity>& activities) {
    std::filesystem::create_directories(path.parent_path());

    json j = json::array();
    for (const auto& a : activities) {
        json logs = json::array();
        for (const auto& e : a.logs) {
            json le = {{"t", to_unix(e.when)}};
            if (e.amount) le["q"] = *e.amount;
            logs.push_back(le);
        }
        json entry = {
            {"name", a.name},
            {"type", a.type == ActivityType::Task ? "task" : "habit"},
            {"logs", logs}
        };
        if (!a.unlogged.empty()) {
            json un = json::array();
            for (const auto& u : a.unlogged) {
                json ue = {{"t", to_unix(u.entry.when)}, {"u", to_unix(u.unlogged_at)}};
                if (u.entry.amount) ue["q"] = *u.entry.amount;
                un.push_back(ue);
            }
            entry["unlogged"] = un;
        }
        if (a.completed_at)
            entry["completed_at"] = to_unix(*a.completed_at);
        if (a.alert_after)
            entry["alert_after"] = *a.alert_after;
        if (a.unit)
            entry["unit"] = *a.unit;
        if (a.target)
            entry["target"] = *a.target;
        if (a.streak) {
            if (a.streak->mode == StreakMode::Interval) {
                entry["streak"] = {{"mode", "interval"}, {"secs", a.streak->interval_secs}};
            } else {
                std::string unit = a.streak->unit == CalendarUnit::Week ? "week"
                                 : a.streak->unit == CalendarUnit::Month ? "month"
                                 : "day";
                entry["streak"] = {{"mode", "calendar"}, {"unit", unit}};
            }
        }
        j.push_back(entry);
    }

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

std::filesystem::path Storage::default_path() {
    const char* home = std::getenv("HOME");
    if (!home) throw std::runtime_error("HOME environment variable not set");
    return std::filesystem::path(home) / ".local/share/overdue/data.json";
}
