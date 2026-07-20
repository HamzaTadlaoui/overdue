#pragma once
#include "activity.hpp"
#include "storage.hpp"
#include <optional>
#include <vector>

// One row of an activity's full log history, as shown by `overdue logs`.
struct LogRow {
    LogEntry entry;
    std::optional<std::chrono::system_clock::time_point> unlogged_at; // set => soft-deleted
};

enum class RelogResult { Ok, NotFound, BadId, NotUnlogged };

class Tracker {
public:
    explicit Tracker(std::filesystem::path data_path, long long unlog_grace_secs = 86400);

    bool add(const std::string& name,
             std::optional<long long> alarm = std::nullopt,
             std::optional<StreakConfig> streak = std::nullopt,
             std::optional<std::string> unit = std::nullopt,
             std::optional<double> target = std::nullopt,
             std::vector<std::string> tags = {});
    bool addtask(const std::string& name,
                 std::optional<std::string> unit = std::nullopt,
                 std::optional<double> target = std::nullopt,
                 std::vector<std::string> tags = {});
    bool setstreak(const std::string& name, StreakConfig sc);
    bool delstreak(const std::string& name);
    bool log(const std::string& name,
             std::optional<std::chrono::system_clock::time_point> when = std::nullopt,
             std::optional<double> amount = std::nullopt);
    bool unlog(const std::string& name);
    // Full merged history (active + unlogged), sorted oldest-first; row index + 1
    // is the id used by `relog`. Empty if the activity is not found.
    std::vector<LogRow> log_rows(const std::string& name) const;
    RelogResult relog(const std::string& name, int id);
    bool done(const std::string& name);
    bool remove(const std::string& name);
    bool setalarm(const std::string& name, long long seconds);
    bool delalarm(const std::string& name);
    bool setunit(const std::string& name, const std::string& unit);
    bool delunit(const std::string& name);
    bool settarget(const std::string& name, double target);
    bool deltarget(const std::string& name);
    // Add/remove a single tag. addtag returns false if the activity is missing;
    // deltag returns false if the activity is missing or lacks the tag.
    bool addtag(const std::string& name, const std::string& tag);
    bool deltag(const std::string& name, const std::string& tag);

    std::vector<Activity> habits() const;
    std::vector<Activity> tasks(bool include_done = false) const;
    const std::vector<Activity>& all() const;
    std::vector<Activity> overdue_activities() const;
    std::optional<Activity> find(const std::string& name) const;

private:
    std::filesystem::path path_;
    long long unlog_grace_secs_;
    std::vector<Activity> activities_;
    // Revision the in-memory state is based on; save() commits revision_+1 and
    // fails with StaleWriteError if the on-disk revision has moved on since.
    long long revision_ = 0;

    // Drop unlogged entries past the grace window. Returns true if any were purged.
    bool purge_expired();
    // Commit activities_ at revision_+1 under the cross-process lock, advancing
    // revision_. Throws StaleWriteError on a concurrent-write conflict.
    void save();
};
