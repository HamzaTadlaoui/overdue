#pragma once
#include <filesystem>

// User-tunable parameters, persisted as JSON alongside the activity data.
struct Config {
    // How long an unlogged entry stays recoverable before it is purged for good.
    long long unlog_grace_secs = 86400; // default: 24h

    static Config load(const std::filesystem::path& path);
    static void save(const std::filesystem::path& path, const Config& cfg);
    static std::filesystem::path default_path();
};
