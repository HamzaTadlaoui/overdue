#include "storage.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <print>
#include <stdexcept>

using json = nlohmann::json;

static std::chrono::system_clock::time_point from_unix(long long t) {
    return std::chrono::system_clock::time_point{std::chrono::seconds{t}};
}

static long long to_unix(const std::chrono::system_clock::time_point& tp) {
    return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
}

// Tolerant field readers: return nullopt rather than throwing when a value is
// missing or has an unexpected type, so data written by an older/newer version
// (or hand-edited into a slightly wrong shape) still loads. Numbers stored as
// strings are also accepted, since that is a common cross-tool mangling.
static std::optional<long long> read_ll(const json& j) {
    if (j.is_number_integer() || j.is_number_unsigned()) return j.get<long long>();
    if (j.is_number_float()) return static_cast<long long>(j.get<double>());
    if (j.is_boolean()) return j.get<bool>() ? 1 : 0;
    if (j.is_string()) { try { return std::stoll(j.get<std::string>()); } catch (...) {} }
    return std::nullopt;
}

static std::optional<double> read_double(const json& j) {
    if (j.is_number()) return j.get<double>();
    if (j.is_string()) { try { return std::stod(j.get<std::string>()); } catch (...) {} }
    return std::nullopt;
}

static std::optional<std::string> read_string(const json& j) {
    if (j.is_string()) return j.get<std::string>();
    return std::nullopt;
}

// Looks up `key` in `obj` and applies a tolerant reader, returning nullopt when
// the key is absent, null, or unreadable. `obj` need not be an object.
template <class Reader>
static auto read_field(const json& obj, const char* key, Reader reader)
    -> decltype(reader(obj)) {
    if (!obj.is_object() || !obj.contains(key)) return std::nullopt;
    const json& v = obj.at(key);
    if (v.is_null()) return std::nullopt;
    return reader(v);
}

// Reads one entry from a "logs"/"unlogged" array element, tolerating both the
// old bare-unix-number format and the { "t": …, "q": … } object format. Returns
// nullopt only when no usable timestamp can be recovered.
static std::optional<LogEntry> read_log_entry(const json& l) {
    if (l.is_object()) {
        auto t = read_field(l, "t", read_ll);
        if (!t) return std::nullopt;
        LogEntry e{from_unix(*t), std::nullopt};
        e.amount = read_field(l, "q", read_double);
        return e;
    }
    if (auto t = read_ll(l)) return LogEntry{from_unix(*t), std::nullopt};
    return std::nullopt;
}

std::vector<Activity> Storage::load(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return {};

    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open " + path.string());

    // Parse without exceptions so a syntactically broken file does not crash the
    // program. If the whole document is unrecoverable we deliberately throw
    // rather than returning {}: the caller's atomic save would otherwise
    // overwrite the original file and turn a recoverable corruption into total
    // data loss. The bad file is left untouched for manual inspection.
    json root = json::parse(f, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded())
        throw std::runtime_error(
            "Cannot parse " + path.string() +
            " — the file is not valid JSON and was left unchanged. "
            "Fix or remove it to continue.");

    // A single object (rather than an array of them) is tolerated by wrapping it.
    if (root.is_object()) root = json::array({root});
    if (!root.is_array()) return {};

    std::vector<Activity> result;
    int skipped_activities = 0;
    int skipped_logs = 0;
    for (const auto& item : root) {
        if (!item.is_object()) { ++skipped_activities; continue; }

        Activity a;
        auto name = read_field(item, "name", read_string);
        // Without a name an activity cannot be addressed by any command; drop it.
        if (!name || name->empty()) { ++skipped_activities; continue; }
        a.name = *name;
        a.type = (read_field(item, "type", read_string).value_or("habit") == "task")
                     ? ActivityType::Task : ActivityType::Habit;

        if (item.contains("logs") && item["logs"].is_array()) {
            for (const auto& l : item["logs"]) {
                if (auto e = read_log_entry(l)) a.logs.push_back(*e);
                else ++skipped_logs;
            }
        }
        // Every read path (stats, streaks, web, last_done) assumes at least one
        // log, so an activity with no recoverable logs is dropped rather than
        // kept in an invalid state.
        if (a.logs.empty()) { ++skipped_activities; continue; }

        if (item.contains("unlogged") && item["unlogged"].is_array()) {
            for (const auto& u : item["unlogged"]) {
                auto e = read_log_entry(u);
                auto uat = read_field(u, "u", read_ll);
                // An unlogged entry needs both its original time and the unlog
                // time; if either is missing it is silently discarded.
                if (e && uat) a.unlogged.push_back(UnloggedEntry{*e, from_unix(*uat)});
            }
        }
        a.completed_at = read_field(item, "completed_at", read_ll)
                             .transform(from_unix);
        a.alert_after = read_field(item, "alert_after", read_ll);
        a.unit = read_field(item, "unit", read_string);
        a.target = read_field(item, "target", read_double);
        if (auto mode = read_field(item, "streak", [](const json& s) {
                return read_field(s, "mode", read_string);
            })) {
            const json& s = item["streak"];
            if (*mode == "interval") {
                a.streak = StreakConfig{StreakMode::Interval,
                                        read_field(s, "secs", read_ll).value_or(0)};
            } else {
                std::string unit = read_field(s, "unit", read_string).value_or("day");
                CalendarUnit cu = unit == "week" ? CalendarUnit::Week
                                : unit == "month" ? CalendarUnit::Month
                                : CalendarUnit::Day;
                a.streak = StreakConfig{StreakMode::Calendar, 0, cu};
            }
        }
        result.push_back(std::move(a));
    }

    if (skipped_activities > 0 || skipped_logs > 0)
        std::println(stderr,
            "Warning: recovered data from {} — skipped {} unreadable "
            "activit{} and {} unreadable log entr{}.",
            path.string(), skipped_activities,
            skipped_activities == 1 ? "y" : "ies",
            skipped_logs, skipped_logs == 1 ? "y" : "ies");

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
