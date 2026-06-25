#include "tracker.hpp"
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

static void print_usage() {
    std::println("Usage:");
    std::println("  dayssince add <name>     Start tracking an activity");
    std::println("  dayssince log <name>     Mark as done now");
    std::println("  dayssince unlog <name>   Cancel the last log");
    std::println("  dayssince list           Show all activities");
    std::println("  dayssince show <name>    Show one activity");
    std::println("  dayssince delete <name>  Stop tracking an activity");
}

int main(int argc, char* argv[]) {
    if (argc < 2) { print_usage(); return 0; }

    try {
        Tracker tracker{Storage::default_path()};
        std::string cmd = argv[1];

        if (cmd == "add") {
            if (argc < 3) { std::println(stderr, "Usage: dayssince add <name>"); return 1; }
            auto name = join_args(std::span(argv + 2, argc - 2));
            if (!tracker.add(name))
                std::println(stderr, "\"{}\" is already being tracked.", name);
            else
                std::println("✓ Now tracking \"{}\"", name);
        }
        else if (cmd == "log") {
            if (argc < 3) { std::println(stderr, "Usage: dayssince log <name>"); return 1; }
            auto name = join_args(std::span(argv + 2, argc - 2));
            if (!tracker.log(name))
                std::println(stderr, "\"{}\" not found.", name);
            else
                std::println("✓ \"{}\" logged now.", name);
        }
        else if (cmd == "unlog") {
            if (argc < 3) { std::println(stderr, "Usage: dayssince unlog <name>"); return 1; }
            auto name = join_args(std::span(argv + 2, argc - 2));
            if (!tracker.unlog(name))
                std::println(stderr, "\"{}\" not found or no previous log to restore.", name);
            else
                std::println("✓ Last log for \"{}\" cancelled.", name);
        }
        else if (cmd == "list") {
            auto activities = tracker.list();
            if (activities.empty()) {
                std::println("No activities tracked. Use 'dayssince add <name>' to start.");
                return 0;
            }
            std::println("{:<22} {:<21} {}", "Activity", "Last done", "Elapsed");
            std::println("{}", std::string(55, '-'));
            for (const auto& a : activities)
                std::println("{:<22} {:<21} {}", a.name, format_datetime(last_done(a)), format_elapsed(last_done(a)));
        }
        else if (cmd == "show") {
            if (argc < 3) { std::println(stderr, "Usage: dayssince show <name>"); return 1; }
            auto name = join_args(std::span(argv + 2, argc - 2));
            auto a = tracker.find(name);
            if (!a) { std::println(stderr, "\"{}\" not found.", name); return 1; }
            std::println("{} — last done {} ({})", a->name, format_datetime(last_done(*a)), format_elapsed(last_done(*a)));
        }
        else if (cmd == "delete") {
            if (argc < 3) { std::println(stderr, "Usage: dayssince delete <name>"); return 1; }
            auto name = join_args(std::span(argv + 2, argc - 2));
            if (!tracker.remove(name))
                std::println(stderr, "\"{}\" not found.", name);
            else
                std::println("✓ \"{}\" removed.", name);
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
