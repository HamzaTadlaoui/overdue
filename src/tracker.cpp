#include "tracker.hpp"
#include <algorithm>

Tracker::Tracker(std::filesystem::path data_path)
    : path_(std::move(data_path)), activities_(Storage::load(path_)) {}

bool Tracker::add(const std::string& name) {
    if (find(name)) return false;
    activities_.push_back({name, {now()}});
    save();
    return true;
}

bool Tracker::log(const std::string& name) {
    auto it = std::ranges::find_if(activities_, [&](const Activity& a) { return a.name == name; });
    if (it == activities_.end()) return false;
    it->logs.push_back(now());
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

std::vector<Activity> Tracker::list() const { return activities_; }

std::optional<Activity> Tracker::find(const std::string& name) const {
    auto it = std::ranges::find_if(activities_, [&](const Activity& a) { return a.name == name; });
    return it != activities_.end() ? std::optional<Activity>{*it} : std::nullopt;
}

void Tracker::save() const { Storage::save(path_, activities_); }
