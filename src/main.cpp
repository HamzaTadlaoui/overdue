#include "tracker.hpp"
#include <cstdlib>
#include <iostream>
#include <print>
#include <span>

static std::string join_args(std::span<char*> args) {
    std::string result;
    for (const auto& arg : args) {
        if (!result.empty()) result += ' ';
        result += arg;
    }
    return result;
}

static void notify(const std::string& title, const std::string& body) {
    auto cmd = std::format("notify-send --urgency=normal '{}' '{}'", title, body);
    std::system(cmd.c_str());
}

static void print_usage() {
    std::println("Usage:");
    std::println("  overdue add <name>              Start tracking an activity");
    std::println("  overdue log <name>              Mark as done now");
    std::println("  overdue unlog <name>            Cancel the last log");
    std::println("  overdue list                    Show all activities");
    std::println("  overdue show <name>             Show one activity");
    std::println("  overdue delete <name>           Stop tracking an activity");
    std::println("  overdue setalarm <name> <dur>   Set alert threshold (e.g. 3d, 12h, 1d6h)");
    std::println("  overdue delalarm <name>         Remove alert");
    std::println("  overdue check                   Send notifications for overdue activities");
}

int main(int argc, char* argv[]) {
    if (argc < 2) { print_usage(); return 0; }

    try {
        Tracker tracker{Storage::default_path()};
        std::string cmd = argv[1];

        if (cmd == "add") {
            if (argc < 3) { std::println(stderr, "Usage: overdue add <name>"); return 1; }
            auto name = join_args(std::span(argv + 2, argc - 2));
            if (!tracker.add(name))
                std::println(stderr, "\"{}\" is already being tracked.", name);
            else
                std::println("✓ Now tracking \"{}\"", name);
        }
        else if (cmd == "log") {
            if (argc < 3) { std::println(stderr, "Usage: overdue log <name> [--ago <dur> | --at <date>]"); return 1; }

            std::optional<std::chrono::system_clock::time_point> when;
            std::vector<std::string> name_parts;

            for (int i = 2; i < argc; ++i) {
                std::string arg = argv[i];
                if ((arg == "--ago" || arg == "--at") && i + 1 < argc) {
                    std::string val = argv[++i];
                    if (arg == "--ago") {
                        auto dur = parse_duration(val);
                        if (!dur) { std::println(stderr, "Invalid duration \"{}\". Examples: 2h, 3d, 30m", val); return 1; }
                        when = now() - std::chrono::seconds{*dur};
                    } else {
                        when = parse_at(val);
                        if (!when) { std::println(stderr, "Invalid date \"{}\". Expected: 2026-06-22 or 2026-06-22T08:15", val); return 1; }
                    }
                } else {
                    name_parts.push_back(arg);
                }
            }

            if (name_parts.empty()) { std::println(stderr, "Missing activity name."); return 1; }
            std::string name;
            for (const auto& p : name_parts) { if (!name.empty()) name += ' '; name += p; }

            if (!tracker.log(name, when))
                std::println(stderr, "\"{}\" not found.", name);
            else {
                auto label = when ? std::format("logged at {}", format_datetime(*when)) : "logged now";
                std::println("✓ \"{}\" {}.", name, label);
            }
        }
        else if (cmd == "unlog") {
            if (argc < 3) { std::println(stderr, "Usage: overdue unlog <name>"); return 1; }
            auto name = join_args(std::span(argv + 2, argc - 2));
            if (!tracker.unlog(name))
                std::println(stderr, "\"{}\" not found or no previous log to restore.", name);
            else
                std::println("✓ Last log for \"{}\" cancelled.", name);
        }
        else if (cmd == "list") {
            auto activities = tracker.list();
            if (activities.empty()) {
                std::println("No activities tracked. Use 'overdue add <name>' to start.");
                return 0;
            }
            std::println("{:<22} {:<21} {:<16} {}", "Activity", "Last done", "Elapsed", "Alarm");
            std::println("{}", std::string(68, '-'));
            for (const auto& a : activities) {
                auto alarm = a.alert_after ? format_duration(*a.alert_after) : "-";
                std::println("{:<22} {:<21} {:<16} {}", a.name,
                    format_datetime(last_done(a)), format_elapsed(last_done(a)), alarm);
            }
        }
        else if (cmd == "show") {
            if (argc < 3) { std::println(stderr, "Usage: overdue show <name>"); return 1; }
            auto name = join_args(std::span(argv + 2, argc - 2));
            auto a = tracker.find(name);
            if (!a) { std::println(stderr, "\"{}\" not found.", name); return 1; }
            std::println("{} — last done {} ({})", a->name,
                format_datetime(last_done(*a)), format_elapsed(last_done(*a)));
            if (a->alert_after)
                std::println("  alarm: after {}", format_duration(*a->alert_after));
            std::println("  total logs: {}", a->logs.size());
        }
        else if (cmd == "delete") {
            if (argc < 3) { std::println(stderr, "Usage: overdue delete <name>"); return 1; }
            auto name = join_args(std::span(argv + 2, argc - 2));
            if (!tracker.remove(name))
                std::println(stderr, "\"{}\" not found.", name);
            else
                std::println("✓ \"{}\" removed.", name);
        }
        else if (cmd == "setalarm") {
            // last arg is the duration, everything in between is the name
            if (argc < 4) { std::println(stderr, "Usage: overdue setalarm <name> <duration>"); return 1; }
            std::string dur_str = argv[argc - 1];
            auto dur = parse_duration(dur_str);
            if (!dur) {
                std::println(stderr, "Invalid duration \"{}\". Examples: 3d, 12h, 30m, 1d6h", dur_str);
                return 1;
            }
            auto name = join_args(std::span(argv + 2, argc - 3));
            if (!tracker.setalarm(name, *dur))
                std::println(stderr, "\"{}\" not found.", name);
            else
                std::println("✓ Alarm set for \"{}\" after {}", name, format_duration(*dur));
        }
        else if (cmd == "delalarm") {
            if (argc < 3) { std::println(stderr, "Usage: overdue delalarm <name>"); return 1; }
            auto name = join_args(std::span(argv + 2, argc - 2));
            if (!tracker.delalarm(name))
                std::println(stderr, "\"{}\" not found.", name);
            else
                std::println("✓ Alarm removed for \"{}\"", name);
        }
        else if (cmd == "check") {
            auto overdue = tracker.overdue_activities();
            if (overdue.empty()) return 0;
            for (const auto& a : overdue) {
                auto elapsed = format_elapsed(last_done(a));
                auto threshold = format_duration(*a.alert_after);
                notify(std::format("overdue: {}", a.name),
                       std::format("{} since last done (alarm: {})", elapsed, threshold));
            }
        }
        else {
            std::println(stderr, "Unknown command: {}", cmd);
            print_usage();
            return 1;
        }
    } catch (const std::exception& e) {
        std::println(stderr, "Error: {}", e.what());
        return 1;
    }

    return 0;
}
