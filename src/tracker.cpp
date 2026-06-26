#include "tracker.hpp"
#include <algorithm>

Tracker::Tracker(std::filesystem::path data_path)
    : path_(std::move(data_path)), activities_(Storage::load(path_)) {}

bool Tracker::add(const std::string& name,
                  std::optional<long long> alarm,
                  std::optional<StreakConfig> streak) {
    if (find(name)) return false;
    Activity a;
    a.name = name;
    a.type = ActivityType::Habit;
    a.logs = {now()};
    a.alert_after = alarm;
    a.streak = streak;
    activities_.push_back(std::move(a));
    save();
    return true;
}

bool Tracker::addtask(const std::string& name) {
    if (find(name)) return false;
    activities_.push_back({name, ActivityType::Task, {now()}, std::nullopt, std::nullopt});
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

bool Tracker::log(const std::string& name, std::optional<std::chrono::system_clock::time_point> when) {
    auto it = std::ranges::find_if(activities_, [&](const Activity& a) { return a.name == name; });
    if (it == activities_.end()) return false;
    it->logs.push_back(when.value_or(now()));
    std::ranges::sort(it->logs);
    save();
    return true;
}

bool Tracker::unlog(const std::string& name) {
    auto it = std::ranges::find_if(activities_, [&](const Activity& a) { return a.name == name; });
    if (it == activities_.end()) return false;
    if (it->logs.size() <= 1) return false;
    it->logs.pop_back();
    save();
    return true;
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
    if (it == activities_.end()) return false;
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

void Tracker::save() const { Storage::save(path_, activities_); }
