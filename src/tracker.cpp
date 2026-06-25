#include "tracker.hpp"
#include <algorithm>

Tracker::Tracker(std::filesystem::path data_path)
    : path_(std::move(data_path)), activities_(Storage::load(path_)) {}

bool Tracker::add(const std::string& name) {
    if (find(name)) return false;
    activities_.push_back({name, {now()}, std::nullopt});
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

bool Tracker::setalarm(const std::string& name, long long seconds) {
    auto it = std::ranges::find_if(activities_, [&](const Activity& a) { return a.name == name; });
    if (it == activities_.end()) return false;
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

std::vector<Activity> Tracker::list() const { return activities_; }

std::vector<Activity> Tracker::overdue_activities() const {
    std::vector<Activity> result;
    for (const auto& a : activities_) {
        if (!a.alert_after) continue;
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
