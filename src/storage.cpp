#include "storage.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>

using json = nlohmann::json;

static std::chrono::system_clock::time_point from_unix(long long t) {
    return std::chrono::system_clock::time_point{std::chrono::seconds{t}};
}

static long long to_unix(const std::chrono::system_clock::time_point& tp) {
    return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
}

std::vector<Activity> Storage::load(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return {};

    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open " + path.string());

    std::vector<Activity> result;
    for (const auto& item : json::parse(f)) {
        Activity a;
        a.name = item["name"].get<std::string>();
        for (auto unix : item["logs"])
            a.logs.push_back(from_unix(unix.get<long long>()));
        result.push_back(std::move(a));
    }
    return result;
}

void Storage::save(const std::filesystem::path& path, const std::vector<Activity>& activities) {
    std::filesystem::create_directories(path.parent_path());

    json j = json::array();
    for (const auto& a : activities) {
        json logs = json::array();
        for (const auto& tp : a.logs)
            logs.push_back(to_unix(tp));
        j.push_back({{"name", a.name}, {"logs", logs}});
    }

    std::ofstream f(path);
    if (!f) throw std::runtime_error("Cannot write " + path.string());
    f << j.dump(2) << '\n';
}

std::filesystem::path Storage::default_path() {
    const char* home = std::getenv("HOME");
    if (!home) throw std::runtime_error("HOME environment variable not set");
    return std::filesystem::path(home) / ".local/share/overdue/data.json";
}
