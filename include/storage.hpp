#pragma once
#include "activity.hpp"
#include <filesystem>
#include <vector>

class Storage {
public:
    static std::vector<Activity> load(const std::filesystem::path& path);
    static void save(const std::filesystem::path& path, const std::vector<Activity>& activities);
};
