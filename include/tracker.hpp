#pragma once
#include "activity.hpp"
#include "storage.hpp"
#include <optional>
#include <vector>

class Tracker {
public:
    explicit Tracker(std::filesystem::path data_path);

    bool add(const std::string& name);
    bool addtask(const std::string& name);
    bool log(const std::string& name,
             std::optional<std::chrono::system_clock::time_point> when = std::nullopt);
    bool unlog(const std::string& name);
    bool done(const std::string& name);
    bool remove(const std::string& name);
    bool setalarm(const std::string& name, long long seconds);
    bool delalarm(const std::string& name);

    std::vector<Activity> habits() const;
    std::vector<Activity> tasks(bool include_done = false) const;
    std::vector<Activity> overdue_activities() const;
    std::optional<Activity> find(const std::string& name) const;

private:
    std::filesystem::path path_;
    std::vector<Activity> activities_;

    void save() const;
};
