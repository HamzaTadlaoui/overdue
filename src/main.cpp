#include "tracker.hpp"
#include "stats.hpp"
#include "web.hpp"
#include <cstdio>
#include <iostream>
#include <print>
#include <span>
#include <sys/wait.h>
#include <unistd.h>

static std::string join_args(std::span<char*> args) {
    std::string result;
    for (const auto& arg : args) {
        if (!result.empty()) result += ' ';
        result += arg;
    }
    return result;
}

static void notify(const std::string& title, const std::string& body) {
    pid_t pid = fork();
    if (pid == 0) {
        execlp("notify-send", "notify-send", "--urgency=normal",
               title.c_str(), body.c_str(), nullptr);
        _exit(127);
    } else if (pid > 0) {
        waitpid(pid, nullptr, 0);
    }
}

static void print_usage() {
    std::println("Usage:");
    std::println("  overdue add <name>              Track a recurring habit ([--unit u] [--target n])");
    std::println("  overdue addtask <name>          Add a one-time task ([--unit u] [--target n])");
    std::println("  overdue log <name>              Mark habit as done now");
    std::println("  overdue log <name> --ago <dur>  Mark as done X time ago (2h, 1d6h...)");
    std::println("  overdue log <name> --at <date>  Mark as done at date (2026-06-22T08:15)");
    std::println("  overdue log <name> --amount <n> Record a quantity with the log");
    std::println("  overdue unlog <name>            Cancel the last log");
    std::println("  overdue done <name>             Mark a task as completed");
    std::println("  overdue list                    Show habits and active tasks");
    std::println("  overdue list --done             Also show completed tasks");
    std::println("  overdue show <name>             Show details for one entry");
    std::println("  overdue delete <name>           Remove an entry");
    std::println("  overdue setalarm <name> <dur>   Alert after this long without logging");
    std::println("  overdue delalarm <name>         Remove alert");
    std::println("  overdue setunit <name> <unit>   Label amounts for an entry (e.g. km)");
    std::println("  overdue delunit <name>          Remove the unit label");
    std::println("  overdue settarget <name> <n>    Set a goal for accumulated amount");
    std::println("  overdue deltarget <name>        Remove the target");
    std::println("  overdue check                   Send notifications for overdue habits");
    std::println("  overdue web [--port <n>]        Open a dashboard in your browser (default :8080)");
    std::println("  overdue setstreak <name> <s>    Set streak (daily/weekly/monthly/3d...)");
    std::println("  overdue delstreak <name>        Remove streak tracking");
    std::println("  overdue stats [name]            Global stats, or detail for one entry");
}

int main(int argc, char* argv[]) {
    if (argc < 2) { print_usage(); return 0; }

    try {
        Tracker tracker{Storage::default_path()};
        std::string cmd = argv[1];

        if (cmd == "add") {
            if (argc < 3) { std::println(stderr, "Usage: overdue add <name> [--alarm <dur>] [--streak daily|weekly|monthly|<dur>] [--unit <u>] [--target <n>]"); return 1; }

            std::optional<long long> alarm;
            std::optional<StreakConfig> streak;
            std::optional<std::string> unit;
            std::optional<double> target;
            std::vector<std::string> name_parts;

            for (int i = 2; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--alarm" && i + 1 < argc) {
                    auto dur = parse_duration(argv[++i]);
                    if (!dur) { std::println(stderr, "Invalid alarm duration."); return 1; }
                    alarm = dur;
                } else if (arg == "--streak" && i + 1 < argc) {
                    streak = parse_streak(argv[++i]);
                    if (!streak) { std::println(stderr, "Invalid streak. Examples: daily, weekly, monthly, 3d, 12h"); return 1; }
                } else if (arg == "--unit" && i + 1 < argc) {
                    unit = argv[++i];
                } else if (arg == "--target" && i + 1 < argc) {
                    target = parse_amount(argv[++i]);
                    if (!target) { std::println(stderr, "Invalid target. Expected a non-negative number."); return 1; }
                } else {
                    name_parts.push_back(arg);
                }
            }
            if (name_parts.empty()) { std::println(stderr, "Missing activity name."); return 1; }
            std::string name;
            for (const auto& p : name_parts) { if (!name.empty()) name += ' '; name += p; }

            if (!tracker.add(name, alarm, streak, unit, target))
                std::println(stderr, "\"{}\" is already being tracked.", name);
            else
                std::println("✓ Now tracking \"{}\"", name);
        }
        else if (cmd == "addtask") {
            if (argc < 3) { std::println(stderr, "Usage: overdue addtask <name> [--unit <u>] [--target <n>]"); return 1; }

            std::optional<std::string> unit;
            std::optional<double> target;
            std::vector<std::string> name_parts;

            for (int i = 2; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--unit" && i + 1 < argc) {
                    unit = argv[++i];
                } else if (arg == "--target" && i + 1 < argc) {
                    target = parse_amount(argv[++i]);
                    if (!target) { std::println(stderr, "Invalid target. Expected a non-negative number."); return 1; }
                } else {
                    name_parts.push_back(arg);
                }
            }
            if (name_parts.empty()) { std::println(stderr, "Missing task name."); return 1; }
            std::string name;
            for (const auto& p : name_parts) { if (!name.empty()) name += ' '; name += p; }

            if (!tracker.addtask(name, unit, target))
                std::println(stderr, "\"{}\" already exists.", name);
            else
                std::println("✓ Task \"{}\" added.", name);
        }
        else if (cmd == "log") {
            if (argc < 3) { std::println(stderr, "Usage: overdue log <name> [--ago <dur> | --at <date>] [--amount <n>]"); return 1; }

            std::optional<std::chrono::system_clock::time_point> when;
            std::optional<double> amount;
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
                } else if (arg == "--amount" && i + 1 < argc) {
                    std::string val = argv[++i];
                    amount = parse_amount(val);
                    if (!amount) { std::println(stderr, "Invalid amount \"{}\". Expected a non-negative number.", val); return 1; }
                } else {
                    name_parts.push_back(arg);
                }
            }

            if (name_parts.empty()) { std::println(stderr, "Missing activity name."); return 1; }
            if (when && *when > now()) {
                std::println(stderr, "Cannot log a future date."); return 1;
            }
            std::string name;
            for (const auto& p : name_parts) { if (!name.empty()) name += ' '; name += p; }

            if (!tracker.log(name, when, amount))
                std::println(stderr, "\"{}\" not found.", name);
            else {
                auto label = when ? std::format("logged at {}", format_datetime(*when)) : "logged now";
                auto qty = amount ? std::format(" (+{})", format_amount(*amount)) : "";
                std::println("✓ \"{}\" {}{}.", name, label, qty);
            }
        }
        else if (cmd == "unlog") {
            if (argc < 3) { std::println(stderr, "Usage: overdue unlog <name>"); return 1; }
            auto name = join_args(std::span(argv + 2, argc - 2));

            // Guard against wiping out an older log by accident: unlog removes the
            // most recent log (logs.back()); if that one is over an hour old, confirm.
            if (auto a = tracker.find(name); a && a->logs.size() > 1) {
                auto when = a->logs.back().when;
                auto age = std::chrono::duration_cast<std::chrono::seconds>(now() - when).count();
                if (age > 3600) {
                    std::print("Last log for \"{}\" was {} ago ({}). Unlog it? [y/N] ",
                        name, format_elapsed(when), format_datetime(when));
                    std::fflush(stdout);
                    std::string answer;
                    std::getline(std::cin, answer);
                    if (answer != "y" && answer != "Y" && answer != "yes") {
                        std::println("Cancelled.");
                        return 0;
                    }
                }
            }

            if (!tracker.unlog(name))
                std::println(stderr, "\"{}\" not found or no previous log to restore.", name);
            else
                std::println("✓ Last log for \"{}\" cancelled.", name);
        }
        else if (cmd == "done") {
            if (argc < 3) { std::println(stderr, "Usage: overdue done <name>"); return 1; }
            auto name = join_args(std::span(argv + 2, argc - 2));
            if (!tracker.done(name))
                std::println(stderr, "\"{}\" not found or is not a task.", name);
            else
                std::println("✓ \"{}\" completed.", name);
        }
        else if (cmd == "setstreak") {
            if (argc < 4) { std::println(stderr, "Usage: overdue setstreak <name> <daily|weekly|monthly|dur>"); return 1; }
            std::string s_str = argv[argc - 1];
            auto sc = parse_streak(s_str);
            if (!sc) { std::println(stderr, "Invalid streak \"{}\". Examples: daily, weekly, monthly, 3d", s_str); return 1; }
            auto name = join_args(std::span(argv + 2, argc - 3));
            if (!tracker.setstreak(name, *sc))
                std::println(stderr, "\"{}\" not found.", name);
            else
                std::println("✓ Streak set for \"{}\" ({})", name, format_streak_label(*sc));
        }
        else if (cmd == "delstreak") {
            if (argc < 3) { std::println(stderr, "Usage: overdue delstreak <name>"); return 1; }
            auto name = join_args(std::span(argv + 2, argc - 2));
            if (!tracker.delstreak(name))
                std::println(stderr, "\"{}\" not found.", name);
            else
                std::println("✓ Streak removed for \"{}\"", name);
        }
        else if (cmd == "list") {
            bool show_done = (argc >= 3 && std::string(argv[2]) == "--done");

            auto habits = tracker.habits();
            auto tasks  = tracker.tasks(show_done);

            if (habits.empty() && tasks.empty()) {
                std::println("Nothing tracked. Use 'overdue add <name>' or 'overdue addtask <name>'.");
                return 0;
            }

            if (!habits.empty()) {
                std::println("Habits");
                std::println("{:<22} {:<21} {:<16} {:<8} {}", "Activity", "Last done", "Elapsed", "Alarm", "Streak");
                std::println("{}", std::string(76, '-'));
                for (const auto& a : habits) {
                    auto alarm = a.alert_after ? format_duration(*a.alert_after) : "-";
                    std::string streak_col = "-";
                    if (a.streak) {
                        int s = compute_streak(a);
                        streak_col = std::format("{} ({})", s, format_streak_label(*a.streak));
                    }
                    std::println("{:<22} {:<21} {:<16} {:<8} {}", a.name,
                        format_datetime(last_done(a)), format_elapsed(last_done(a)), alarm, streak_col);
                }
            }

            if (!tasks.empty()) {
                if (!habits.empty()) std::println("");
                std::println("Tasks");
                std::println("{:<22} {:<21} {}", "Task", "Added", "Pending for");
                std::println("{}", std::string(60, '-'));
                for (const auto& a : tasks) {
                    if (a.completed_at) {
                        std::println("{:<22} {:<21} ✓ done ({})", a.name,
                            format_datetime(a.logs.front().when),
                            format_datetime(*a.completed_at));
                    } else {
                        std::println("{:<22} {:<21} {}", a.name,
                            format_datetime(a.logs.front().when),
                            format_elapsed(a.logs.front().when));
                    }
                }
            }
        }
        else if (cmd == "show") {
            if (argc < 3) { std::println(stderr, "Usage: overdue show <name>"); return 1; }
            auto name = join_args(std::span(argv + 2, argc - 2));
            auto a = tracker.find(name);
            if (!a) { std::println(stderr, "\"{}\" not found.", name); return 1; }
            if (a->type == ActivityType::Task) {
                std::println("{} [task] — added {}", a->name, format_datetime(a->logs.front().when));
                if (a->completed_at)
                    std::println("  completed: {}", format_datetime(*a->completed_at));
                else
                    std::println("  pending for: {}", format_elapsed(a->logs.front().when));
            } else {
                std::println("{} — last done {} ({})", a->name,
                    format_datetime(last_done(*a)), format_elapsed(last_done(*a)));
                if (a->alert_after)
                    std::println("  alarm:  after {}", format_duration(*a->alert_after));
                if (a->streak)
                    std::println("  streak: {} {} ({})", compute_streak(*a),
                        compute_streak(*a) > 1 ? "in a row" : "",
                        format_streak_label(*a->streak));
                std::println("  total logs: {}", a->logs.size());
            }
            if (auto qs = quantity_stats(*a)) {
                std::string u = a->unit ? " " + *a->unit : "";
                std::println("  amount: {}{} total over {} logs (avg {}, max {})",
                    format_amount(qs->total), u, qs->count,
                    format_amount(qs->avg_per_log), format_amount(qs->max_single));
                if (a->target)
                    std::println("  target: {} / {}{} ({:.0f}%)",
                        format_amount(qs->total), format_amount(*a->target), u,
                        *a->target > 0 ? 100.0 * qs->total / *a->target : 0.0);
            } else if (a->target) {
                std::println("  target: 0 / {}{} (0%)", format_amount(*a->target),
                    a->unit ? " " + *a->unit : "");
            }
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
        else if (cmd == "setunit") {
            if (argc < 4) { std::println(stderr, "Usage: overdue setunit <name> <unit>"); return 1; }
            std::string unit = argv[argc - 1];
            auto name = join_args(std::span(argv + 2, argc - 3));
            if (!tracker.setunit(name, unit))
                std::println(stderr, "\"{}\" not found.", name);
            else
                std::println("✓ Unit for \"{}\" set to {}", name, unit);
        }
        else if (cmd == "delunit") {
            if (argc < 3) { std::println(stderr, "Usage: overdue delunit <name>"); return 1; }
            auto name = join_args(std::span(argv + 2, argc - 2));
            if (!tracker.delunit(name))
                std::println(stderr, "\"{}\" not found.", name);
            else
                std::println("✓ Unit removed for \"{}\"", name);
        }
        else if (cmd == "settarget") {
            if (argc < 4) { std::println(stderr, "Usage: overdue settarget <name> <n>"); return 1; }
            std::string t_str = argv[argc - 1];
            auto target = parse_amount(t_str);
            if (!target) { std::println(stderr, "Invalid target \"{}\". Expected a non-negative number.", t_str); return 1; }
            auto name = join_args(std::span(argv + 2, argc - 3));
            if (!tracker.settarget(name, *target))
                std::println(stderr, "\"{}\" not found.", name);
            else
                std::println("✓ Target for \"{}\" set to {}", name, format_amount(*target));
        }
        else if (cmd == "deltarget") {
            if (argc < 3) { std::println(stderr, "Usage: overdue deltarget <name>"); return 1; }
            auto name = join_args(std::span(argv + 2, argc - 2));
            if (!tracker.deltarget(name))
                std::println(stderr, "\"{}\" not found.", name);
            else
                std::println("✓ Target removed for \"{}\"", name);
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
        else if (cmd == "web") {
            int port = 8080;
            for (int i = 2; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--port" && i + 1 < argc) {
                    auto p = parse_amount(argv[++i]);
                    if (!p || *p < 1 || *p > 65535 || *p != std::floor(*p)) {
                        std::println(stderr, "Invalid port. Expected an integer 1-65535."); return 1;
                    }
                    port = static_cast<int>(*p);
                } else {
                    std::println(stderr, "Usage: overdue web [--port <n>]"); return 1;
                }
            }
            run_web(Storage::default_path(), port);
        }
        else if (cmd == "stats" && argc >= 3) {
            auto name = join_args(std::span(argv + 2, argc - 2));
            auto a = tracker.find(name);
            if (!a) { std::println(stderr, "\"{}\" not found.", name); return 1; }

            std::println("{}{}", a->name, a->type == ActivityType::Task ? " [task]" : "");
            std::println("{}", std::string(40, '-'));
            std::println("  logs:       {}", a->logs.size());
            std::println("  last done:  {} ({})",
                format_datetime(last_done(*a)), format_elapsed(last_done(*a)));
            if (a->streak)
                std::println("  streak:     {} ({})", compute_streak(*a), format_streak_label(*a->streak));

            if (auto qs = quantity_stats(*a)) {
                std::string u = a->unit ? " " + *a->unit : "";
                std::println("  total:      {}{}   avg/log: {}{}",
                    format_amount(qs->total), u, format_amount(qs->avg_per_log), u);
                std::println("  avg/day:    {}{}   best day: {}{}",
                    format_amount(qs->avg_per_day), u, format_amount(qs->best_day), u);
                std::println("  max single: {}{}", format_amount(qs->max_single), u);
                std::println("  last 7d:    {}{}   prev 7d: {}{}",
                    format_amount(qs->last7), u, format_amount(qs->prev7), u);
                if (a->target)
                    std::println("  target:     {} / {}{} ({:.0f}%)",
                        format_amount(qs->total), format_amount(*a->target), u,
                        *a->target > 0 ? 100.0 * qs->total / *a->target : 0.0);
            } else {
                std::println("  (no amounts logged — use 'log {} --amount <n>')", a->name);
            }
        }
        else if (cmd == "stats") {
            auto gs = compute_global(tracker.all());

            std::println("Global stats");
            std::println("{}", std::string(40, '-'));

            if (gs.habit_count > 0)
                std::println("  Habits:  {}    {} logs total", gs.habit_count, gs.total_logs);
            else
                std::println("  Habits:  none");

            if (gs.task_total > 0)
                std::println("  Tasks:   {} done / {} total", gs.task_done, gs.task_total);
            else
                std::println("  Tasks:   none");

            if (gs.most_consistent) {
                auto& h = *gs.most_consistent;
                auto avg = h.avg_interval ? std::format("avg every {}", format_duration(*h.avg_interval)) : "only 1 log";
                std::println("  Most consistent:  {:<20} {}", h.name, avg);
            }
            if (gs.most_neglected) {
                auto& h = *gs.most_neglected;
                auto avg = h.avg_interval ? std::format("avg every {}", format_duration(*h.avg_interval)) : "only 1 log";
                auto alarm = h.alert_after ? std::format("  ← alarm: {}", format_duration(*h.alert_after)) : "";
                std::println("  Most neglected:   {:<20} {}{}", h.name, avg, alarm);
            }
            if (gs.most_logged) {
                auto& h = *gs.most_logged;
                std::println("  Most logged:      {:<20} {} logs", h.name, h.log_count);
            }
            if (gs.best_streak && gs.best_streak->streak > 0) {
                auto& h = *gs.best_streak;
                std::println("  Best streak:      {:<20} {} ({})", h.name, h.streak,
                    format_streak_label(*h.streak_config));
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
