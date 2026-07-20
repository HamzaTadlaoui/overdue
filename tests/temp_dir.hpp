#pragma once
// RAII temporary directory under the system temp path, so tests never read or
// write the user's real config or data. Each instance gets a unique directory
// that is removed on destruction.

#include <atomic>
#include <filesystem>
#include <string>
#include <unistd.h> // getpid

class TempDir {
public:
    TempDir() {
        static std::atomic<unsigned> counter{0};
        auto base = std::filesystem::temp_directory_path();
        for (;;) {
            auto candidate = base / ("overdue-test-" +
                std::to_string(::getpid()) + "-" +
                std::to_string(counter.fetch_add(1)));
            std::error_code ec;
            if (std::filesystem::create_directory(candidate, ec)) {
                path_ = candidate;
                return;
            }
        }
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }
    std::filesystem::path file(const std::string& name) const { return path_ / name; }
    std::filesystem::path data() const { return path_ / "data.json"; }

private:
    std::filesystem::path path_;
};
