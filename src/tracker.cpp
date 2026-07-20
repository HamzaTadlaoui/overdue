#include "tracker.hpp"
#include <algorithm>

namespace {
// A row of an activity's merged history, carrying enough source info for relog
// to map a displayed id back to the exact entry.
struct OrderedRow {
    LogEntry entry;
    std::optional<std::chrono::system_clock::time_point> unlogged_at;
    bool is_unlogged = false;
    std::size_t src_index = 0; // index into Activity::logs or Activity::unlogged
};

// Canonical ordering shared by log_rows() and relog(): oldest-first, with active
// logs before unlogged ones on a tie so ids are deterministic between the two.
std::vector<OrderedRow> build_rows(const Activity& a) {
    std::vector<OrderedRow> rows;
    rows.reserve(a.logs.size() + a.unlogged.size());
    for (std::size_t i = 0; i < a.logs.size(); ++i)
        rows.push_back(OrderedRow{a.logs[i], std::nullopt, false, i});
    for (std::size_t i = 0; i < a.unlogged.size(); ++i)
        rows.push_back(OrderedRow{a.unlogged[i].entry, a.unlogged[i].unlogged_at, true, i});
    std::ranges::sort(rows, [](const OrderedRow& x, const OrderedRow& y) {
        if (x.entry.when != y.entry.when) return x.entry.when < y.entry.when;
        if (x.is_unlogged != y.is_unlogged) return !x.is_unlogged;
        if (x.is_unlogged && *x.unlogged_at != *y.unlogged_at)
            return *x.unlogged_at < *y.unlogged_at;
        return x.src_index < y.src_index;
    });
    return rows;
}

// Normalize, drop empties, sort, and de-duplicate a set of tags so every stored
// tag list has the same canonical shape regardless of entry point.
std::vector<std::string> clean_tags(std::vector<std::string> tags) {
    for (auto& t : tags) t = normalize_tag(t);
    std::erase_if(tags, [](const std::string& t) { return t.empty(); });
    std::ranges::sort(tags);
    tags.erase(std::ranges::unique(tags).begin(), tags.end());
    return tags;
}
} // namespace

Tracker::Tracker(std::filesystem::path data_path, long long unlog_grace_secs)
    : path_(std::move(data_path)), unlog_grace_secs_(unlog_grace_secs) {
    DataFile df = Storage::load(path_);
    activities_ = std::move(df.activities);
    revision_ = df.revision;
    needs_backup_ = df.repaired;
    // Purging expired tombstones is an opportunistic cleanup that also runs on
    // read-only paths (list/show/web render), so a concurrent write must not
    // turn it into a hard error: on conflict, leave the tombstones for the next
    // mutation to purge rather than failing the command or the page render.
    if (purge_expired()) {
        try { save(); }
        catch (const StaleWriteError&) { /* another writer won; purge later */ }
    }
}

bool Tracker::add(const std::string& name,
                  std::optional<long long> alarm,
                  std::optional<StreakConfig> streak,
                  std::optional<std::string> unit,
                  std::optional<double> target,
                  std::vector<std::string> tags) {
    if (find(name)) return false;
    Activity a;
    a.name = name;
    a.type = ActivityType::Habit;
    a.logs = {LogEntry{now(), std::nullopt}};
    a.alert_after = alarm;
    a.streak = streak;
    a.unit = std::move(unit);
    a.target = target;
    a.tags = clean_tags(std::move(tags));
    activities_.push_back(std::move(a));
    save();
    return true;
}

bool Tracker::addtask(const std::string& name,
                      std::optional<std::string> unit,
                      std::optional<double> target,
                      std::vector<std::string> tags) {
    if (find(name)) return false;
    Activity a;
    a.name = name;
    a.type = ActivityType::Task;
    a.logs = {LogEntry{now(), std::nullopt}};
    a.unit = std::move(unit);
    a.target = target;
    a.tags = clean_tags(std::move(tags));
    activities_.push_back(std::move(a));
    save();
    return true;
}

bool Tracker::done(const std::string& name) {
    auto it = std::ranges::find_if(activities_, [&](const Activity& a) { return a.name == name; });
    if (it == activities_.end() || it->type != ActivityType::Task) return false;
    it->completed_at = now();
    save();
    return true;
}

bool Tracker::log(const std::string& name,
                  std::optional<std::chrono::system_clock::time_point> when,
                  std::optional<double> amount) {
    auto it = std::ranges::find_if(activities_, [&](const Activity& a) { return a.name == name; });
    if (it == activities_.end()) return false;
    it->logs.push_back(LogEntry{when.value_or(now()), amount});
    std::ranges::sort(it->logs, {}, &LogEntry::when);
    save();
    return true;
}

bool Tracker::unlog(const std::string& name) {
    auto it = std::ranges::find_if(activities_, [&](const Activity& a) { return a.name == name; });
    if (it == activities_.end()) return false;
    if (it->logs.size() <= 1) return false;
    // logs are kept sorted by when, so back() is the most recent active log.
    // Flag it rather than delete it: recoverable until the grace window elapses.
    it->unlogged.push_back(UnloggedEntry{it->logs.back(), now()});
    it->logs.pop_back();
    save();
    return true;
}

std::vector<LogRow> Tracker::log_rows(const std::string& name) const {
    auto it = std::ranges::find_if(activities_, [&](const Activity& a) { return a.name == name; });
    if (it == activities_.end()) return {};
    std::vector<LogRow> out;
    for (const auto& r : build_rows(*it))
        out.push_back(LogRow{r.entry, r.unlogged_at});
    return out;
}

RelogResult Tracker::relog(const std::string& name, int id) {
    auto it = std::ranges::find_if(activities_, [&](const Activity& a) { return a.name == name; });
    if (it == activities_.end()) return RelogResult::NotFound;
    auto rows = build_rows(*it);
    if (id < 1 || static_cast<std::size_t>(id) > rows.size()) return RelogResult::BadId;
    const auto& row = rows[id - 1];
    if (!row.is_unlogged) return RelogResult::NotUnlogged;
    // Move the entry back into active logs and drop the tombstone.
    it->logs.push_back(it->unlogged[row.src_index].entry);
    it->unlogged.erase(it->unlogged.begin() + row.src_index);
    std::ranges::sort(it->logs, {}, &LogEntry::when);
    save();
    return RelogResult::Ok;
}

bool Tracker::purge_expired() {
    bool changed = false;
    auto cutoff = now();
    for (auto& a : activities_) {
        auto before = a.unlogged.size();
        std::erase_if(a.unlogged, [&](const UnloggedEntry& u) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(cutoff - u.unlogged_at).count();
            return age >= unlog_grace_secs_;
        });
        if (a.unlogged.size() != before) changed = true;
    }
    return changed;
}

bool Tracker::remove(const std::string& name) {
    auto it = std::ranges::find_if(activities_, [&](const Activity& a) { return a.name == name; });
    if (it == activities_.end()) return false;
    activities_.erase(it);
    save();
    return true;
}

bool Tracker::setstreak(const std::string& name, StreakConfig sc) {
    auto it = std::ranges::find_if(activities_, [&](const Activity& a) { return a.name == name; });
    if (it == activities_.end() || it->type != ActivityType::Habit) return false;
    it->streak = sc;
    save();
    return true;
}

bool Tracker::delstreak(const std::string& name) {
    auto it = std::ranges::find_if(activities_, [&](const Activity& a) { return a.name == name; });
    if (it == activities_.end()) return false;
    it->streak = std::nullopt;
    save();
    return true;
}

bool Tracker::setalarm(const std::string& name, long long seconds) {
    auto it = std::ranges::find_if(activities_, [&](const Activity& a) { return a.name == name; });
    if (it == activities_.end() || it->type != ActivityType::Habit) return false;
    it->alert_after = seconds;
    save();
    return true;
}

bool Tracker::delalarm(const std::string& name) {
    auto it = std::ranges::find_if(activities_, [&](const Activity& a) { return a.name == name; });
    if (it == activities_.end()) return false;
    it->alert_after = std::nullopt;
    save();
    return true;
}

bool Tracker::setunit(const std::string& name, const std::string& unit) {
    auto it = std::ranges::find_if(activities_, [&](const Activity& a) { return a.name == name; });
    if (it == activities_.end()) return false;
    it->unit = unit;
    save();
    return true;
}

bool Tracker::delunit(const std::string& name) {
    auto it = std::ranges::find_if(activities_, [&](const Activity& a) { return a.name == name; });
    if (it == activities_.end()) return false;
    it->unit = std::nullopt;
    save();
    return true;
}

bool Tracker::settarget(const std::string& name, double target) {
    auto it = std::ranges::find_if(activities_, [&](const Activity& a) { return a.name == name; });
    if (it == activities_.end()) return false;
    it->target = target;
    save();
    return true;
}

bool Tracker::deltarget(const std::string& name) {
    auto it = std::ranges::find_if(activities_, [&](const Activity& a) { return a.name == name; });
    if (it == activities_.end()) return false;
    it->target = std::nullopt;
    save();
    return true;
}

bool Tracker::addtag(const std::string& name, const std::string& tag) {
    auto it = std::ranges::find_if(activities_, [&](const Activity& a) { return a.name == name; });
    if (it == activities_.end()) return false;
    std::string t = normalize_tag(tag);
    if (t.empty()) return false;
    // Idempotent: re-adding an existing tag is a no-op success (kept sorted+unique).
    if (std::ranges::find(it->tags, t) == it->tags.end()) {
        it->tags.push_back(t);
        std::ranges::sort(it->tags);
        save();
    }
    return true;
}

bool Tracker::deltag(const std::string& name, const std::string& tag) {
    auto it = std::ranges::find_if(activities_, [&](const Activity& a) { return a.name == name; });
    if (it == activities_.end()) return false;
    std::string t = normalize_tag(tag);
    auto removed = std::erase(it->tags, t);
    if (removed == 0) return false;
    save();
    return true;
}

const std::vector<Activity>& Tracker::all() const { return activities_; }

std::vector<Activity> Tracker::habits() const {
    std::vector<Activity> result;
    for (const auto& a : activities_)
        if (a.type == ActivityType::Habit) result.push_back(a);
    return result;
}

std::vector<Activity> Tracker::tasks(bool include_done) const {
    std::vector<Activity> result;
    for (const auto& a : activities_)
        if (a.type == ActivityType::Task && (include_done || !a.completed_at))
            result.push_back(a);
    return result;
}

std::vector<Activity> Tracker::overdue_activities() const {
    std::vector<Activity> result;
    for (const auto& a : activities_) {
        if (!a.alert_after || a.type != ActivityType::Habit) continue;
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now() - last_done(a)).count();
        if (elapsed >= *a.alert_after)
            result.push_back(a);
    }
    return result;
}

std::optional<Activity> Tracker::find(const std::string& name) const {
    auto it = std::ranges::find_if(activities_, [&](const Activity& a) { return a.name == name; });
    return it != activities_.end() ? std::optional<Activity>{*it} : std::nullopt;
}

void Tracker::save() {
    revision_ = Storage::save(path_, activities_, revision_, needs_backup_);
    needs_backup_ = false; // original preserved once; later saves write normally
}
