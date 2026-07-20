#include "storage.hpp"
#include "filelock.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <print>
#include <stdexcept>

using json = nlohmann::json;

// On-disk format (version 2): a JSON envelope
//   { "version": 2, "revision": <n>, "activities": [ <activity>, ... ] }
// The per-activity object shape is unchanged from earlier versions, so only the
// wrapper is new. Two legacy shapes are still accepted on load:
//   * a bare array of activities (the original format), and
//   * a single activity object,
// both of which carry no revision and so load as revision 0 and are upgraded to
// the envelope on the next save.
static constexpr int FORMAT_VERSION = 2;

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

// Parse the file at `path` into a JSON document, throwing a clear error (rather
// than returning empty) on unparseable content so the caller's atomic save can
// never overwrite a merely-corrupt file and cause total data loss.
static json parse_file(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open " + path.string());
    json root = json::parse(f, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded())
        throw std::runtime_error(
            "Cannot parse " + path.string() +
            " — the file is not valid JSON and was left unchanged. "
            "Fix or remove it to continue.");
    return root;
}

// True if `root` looks like a versioned envelope — i.e. it carries any of the
// envelope-only marker keys. Presence of a marker commits us to the envelope
// format: we then validate it strictly rather than silently reinterpreting a
// broken envelope as a legacy single-activity object.
static bool looks_like_envelope(const json& root) {
    return root.is_object() &&
           (root.contains("version") || root.contains("revision") ||
            root.contains("activities"));
}

// Raised for a structurally invalid envelope (valid JSON, wrong shape) or an
// unsupported version. Distinct message so the caller can leave the file
// untouched instead of overwriting it.
static std::runtime_error envelope_error(const std::filesystem::path& path,
                                         const std::string& why) {
    return std::runtime_error(
        "Cannot load " + path.string() + " — " + why +
        ". The file was left unchanged; fix or remove it to continue.");
}

// Split a parsed document into (revision, activities-array). A document with any
// envelope marker is validated strictly as a v2 envelope (throwing on anything
// malformed or from an unsupported future version); otherwise the two legacy
// shapes — a bare array, or a single activity object — are accepted. `holder`
// backs the returned view when a single object has to be wrapped into an array.
static void split_document(const std::filesystem::path& path, const json& root,
                           long long& revision, const json*& activities,
                           json& holder) {
    if (looks_like_envelope(root)) {
        // version: if present it must be a plain integer we understand. A newer
        // version than we know how to read is rejected rather than misparsed.
        if (root.contains("version") && !root["version"].is_null()) {
            const json& v = root["version"];
            if (!(v.is_number_integer() || v.is_number_unsigned()))
                throw envelope_error(path, "\"version\" is not an integer");
            long long ver = v.get<long long>();
            if (ver < 1 || ver > FORMAT_VERSION)
                throw envelope_error(path, "unsupported data format version " +
                                               std::to_string(ver));
        }
        // activities: mandatory and must be an array.
        if (!root.contains("activities") || !root["activities"].is_array())
            throw envelope_error(path, "\"activities\" is missing or not an array");
        // revision: optional, but if present must be a non-negative integer.
        revision = 0;
        if (root.contains("revision") && !root["revision"].is_null()) {
            const json& r = root["revision"];
            if (!(r.is_number_integer() || r.is_number_unsigned()))
                throw envelope_error(path, "\"revision\" is not an integer");
            long long rev = r.get<long long>();
            if (rev < 0)
                throw envelope_error(path, "\"revision\" is negative");
            revision = rev;
        }
        activities = &root["activities"];
    } else if (root.is_array()) {
        // Legacy: bare array of activities.
        revision = 0;
        activities = &root;
    } else if (root.is_object()) {
        // Legacy: a single activity object (no envelope markers) — wrap it.
        revision = 0;
        holder = json::array({root});
        activities = &holder;
    } else {
        revision = 0;
        holder = json::array();
        activities = &holder;
    }
}

DataFile Storage::load(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return {};

    json root = parse_file(path);
    long long revision = 0;
    const json* items = nullptr;
    json holder;
    split_document(path, root, revision, items, holder);

    DataFile out;
    out.revision = revision;
    int skipped_activities = 0;
    int skipped_logs = 0;
    for (const auto& item : *items) {
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
        if (item.contains("tags") && item["tags"].is_array()) {
            for (const auto& t : item["tags"]) {
                // Skip non-string or empty tags; normalize + de-dupe so loaded
                // data matches what the add/tag commands would have produced.
                auto s = read_string(t);
                if (!s) continue;
                std::string norm = normalize_tag(*s);
                if (!norm.empty() &&
                    std::find(a.tags.begin(), a.tags.end(), norm) == a.tags.end())
                    a.tags.push_back(norm);
            }
            std::sort(a.tags.begin(), a.tags.end());
        }
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
        out.activities.push_back(std::move(a));
    }

    if (skipped_activities > 0 || skipped_logs > 0)
        std::println(stderr,
            "Warning: recovered data from {} — skipped {} unreadable "
            "activit{} and {} unreadable log entr{}.",
            path.string(), skipped_activities,
            skipped_activities == 1 ? "y" : "ies",
            skipped_logs, skipped_logs == 1 ? "y" : "ies");

    return out;
}

long long Storage::current_revision(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return 0;
    json root = parse_file(path);
    long long revision = 0;
    const json* items = nullptr;
    json holder;
    split_document(path, root, revision, items, holder);
    return revision;
}

long long Storage::save(const std::filesystem::path& path,
                        const std::vector<Activity>& activities,
                        long long base_revision) {
    std::filesystem::create_directories(path.parent_path());

    // Hold the advisory lock for the whole check-and-write so no other process
    // can slip a write between our revision check and our rename.
    FileLock lock(FileLock::lock_path_for(path));

    long long disk_revision = current_revision(path);
    if (disk_revision != base_revision)
        throw StaleWriteError(base_revision, disk_revision);
    long long new_revision = base_revision + 1;

    json items = json::array();
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
        if (!a.tags.empty())
            entry["tags"] = a.tags;
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
        items.push_back(entry);
    }

    json j = {
        {"version", FORMAT_VERSION},
        {"revision", new_revision},
        {"activities", std::move(items)},
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
    return new_revision;
}
