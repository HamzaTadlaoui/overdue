#pragma once
#include "activity.hpp"
#include "storage.hpp"
#include <optional>
#include <vector>

class Tracker {
public:
    explicit Tracker(std::filesystem::path data_path);

    bool add(const std::string& name,
             std::optional<long long> alarm = std::nullopt,
             std::optional<StreakConfig> streak = std::nullopt,
             std::optional<std::string> unit = std::nullopt,
             std::optional<double> target = std::nullopt);
    bool addtask(const std::string& name,
                 std::optional<std::string> unit = std::nullopt,
                 std::optional<double> target = std::nullopt);
    bool setstreak(const std::string& name, StreakConfig sc);
    bool delstreak(const std::string& name);
    bool log(const std::string& name,
             std::optional<std::chrono::system_clock::time_point> when = std::nullopt,
             std::optional<double> amount = std::nullopt);
    bool unlog(const std::string& name);
    bool done(const std::string& name);
    bool remove(const std::string& name);
    bool setalarm(const std::string& name, long long seconds);
    bool delalarm(const std::string& name);
    bool setunit(const std::string& name, const std::string& unit);
    bool delunit(const std::string& name);
    bool settarget(const std::string& name, double target);
    bool deltarget(const std::string& name);

    std::vector<Activity> habits() const;
    std::vector<Activity> tasks(bool include_done = false) const;
    const std::vector<Activity>& all() const;
    std::vector<Activity> overdue_activities() const;
    std::optional<Activity> find(const std::string& name) const;

private:
    std::filesystem::path path_;
    std::vector<Activity> activities_;

    void save() const;
};
