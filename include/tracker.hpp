#pragma once
#include "activity.hpp"
#include "storage.hpp"
#include <optional>
#include <vector>

class Tracker {
public:
    explicit Tracker(std::filesystem::path data_path);

    bool add(const std::string& name);
    bool log(const std::string& name);
    bool unlog(const std::string& name);
    bool remove(const std::string& name);

    std::vector<Activity> list() const;
    std::optional<Activity> find(const std::string& name) const;

private:
    std::filesystem::path path_;
    std::vector<Activity> activities_;

    void save() const;
};
