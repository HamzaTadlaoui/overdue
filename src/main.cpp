#include "tracker.hpp"
#include "stats.hpp"
#include "config.hpp"
#include "web.hpp"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
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

// Expand a leading "~" or "~/" to $HOME so config paths can be given tersely.
static std::filesystem::path expand_home(const std::string& s) {
    if (s == "~" || s.rfind("~/", 0) == 0) {
        if (const char* home = std::getenv("HOME"))
            return std::filesystem::path(home) / (s.size() > 2 ? s.substr(2) : "");
    }
    return s;
}

static std::optional<bool> parse_bool(const std::string& s) {
    if (s == "on"  || s == "true"  || s == "yes" || s == "1") return true;
    if (s == "off" || s == "false" || s == "no"  || s == "0") return false;
    return std::nullopt;
}

// Friendly names for common full formats. `overdue config set date-format <name>`
// expands one of these; anything else is treated as a raw chrono format string.
static const std::vector<std::pair<std::string, std::string>>& date_format_presets() {
    static const std::vector<std::pair<std::string, std::string>> presets = {
        {"iso",     "%Y-%m-%d %H:%M:%S"},        // 2026-07-07 22:15:03
        {"us",      "%m/%d/%Y %I:%M %p"},        // 07/07/2026 10:15 PM
        {"eu",      "%d/%m/%Y %H:%M"},           // 07/07/2026 22:15
        {"uk",      "%d-%m-%Y %H:%M:%S"},        // 07-07-2026 22:15:03
        {"compact", "%Y%m%d-%H%M"},              // 20260707-2215
        {"long",    "%A, %d %B %Y %H:%M"},       // Tuesday, 07 July 2026 22:15
    };
    return presets;
}

static std::optional<std::string> lookup_date_preset(const std::string& name) {
    for (const auto& [k, v] : date_format_presets())
        if (k == name) return v;
    return std::nullopt;
}

static std::string preset_names_csv() {
    std::string out;
    for (const auto& [k, v] : date_format_presets()) {
        if (!out.empty()) out += ", ";
        out += k;
    }
    return out;
}

// Accepts a separator either literally (- . /) or by name (dash/dot/slash/space).
static std::optional<std::string> parse_date_sep(const std::string& s) {
    if (s == "-" || s == "dash")  return "-";
    if (s == "." || s == "dot")   return ".";
    if (s == "/" || s == "slash") return "/";
    if (s == " " || s == "space") return " ";
    return std::nullopt;
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

// True when the current local time falls inside the nightly quiet window
// [start, end) (hours). start == end disables it; end < start wraps midnight.
static bool in_quiet_hours(int start, int end) {
    if (start == end) return false;
    auto local = std::chrono::floor<std::chrono::seconds>(active_zone()->to_local(now()));
    auto dp = std::chrono::floor<std::chrono::days>(local);
    int hour = std::chrono::hh_mm_ss{local - dp}.hours().count();
    return start < end ? (hour >= start && hour < end)
                       : (hour >= start || hour < end);
}

// Per-activity last-notified epoch seconds, used to honour notify_cooldown_secs
// across separate `overdue check` runs. Stored next to data.json; a
// missing/corrupt file is treated as empty so it can never block a check.
static std::map<std::string, long long> load_notify_state(const std::filesystem::path& p) {
    std::map<std::string, long long> m;
    std::ifstream f(p);
    if (!f) return m;
    try {
        for (auto& [k, v] : nlohmann::json::parse(f).items())
            m[k] = v.get<long long>();
    } catch (...) { /* corrupt state is non-fatal */ }
    return m;
}

static void save_notify_state(const std::filesystem::path& p,
                              const std::map<std::string, long long>& m) {
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [k, v] : m) j[k] = v;
    std::filesystem::create_directories(p.parent_path());
    auto tmp = p; tmp += ".tmp";
    { std::ofstream f(tmp); f << j.dump(2) << '\n'; }
    std::filesystem::rename(tmp, p);
}

static void print_usage() {
    std::println("Usage:");
    std::println("  overdue add <name>              Track a recurring habit ([--unit u] [--target n])");
    std::println("  overdue addtask <name>          Add a one-time task ([--unit u] [--target n])");
    std::println("  overdue log <name>              Mark habit as done now");
    std::println("  overdue log <name> --ago <dur>  Mark as done X time ago (2h, 1d6h...)");
    std::println("  overdue log <name> --at <date>  Mark as done at date (2026-06-22T08:15)");
    std::println("  overdue log <name> --amount <n> Record a quantity with the log");
    std::println("  overdue unlog <name>            Cancel the last log (recoverable for a grace period)");
    std::println("  overdue logs <name>             List all logs with ids and unlogged status");
    std::println("  overdue relog <name> <id>       Restore an unlogged entry by its id");
    std::println("  overdue done <name>             Mark a task as completed");
    std::println("  overdue list                    Show habits and active tasks");
    std::println("  overdue list --done             Also show completed tasks");
    std::println("  overdue list --type habit|task  Show only one kind");
    std::println("  overdue list --tag <t>          Show only entries with tag <t> (repeatable)");
    std::println("  overdue show <name>             Show details for one entry");
    std::println("  overdue delete <name>           Remove an entry");
    std::println("  overdue setalarm <name> <dur>   Alert after this long without logging");
    std::println("  overdue delalarm <name>         Remove alert");
    std::println("  overdue setunit <name> <unit>   Label amounts for an entry (e.g. km)");
    std::println("  overdue delunit <name>          Remove the unit label");
    std::println("  overdue settarget <name> <n>    Set a goal for accumulated amount");
    std::println("  overdue deltarget <name>        Remove the target");
    std::println("  overdue tag <name> <tag>        Add a category/tag to an entry");
    std::println("  overdue untag <name> <tag>      Remove a tag from an entry");
    std::println("  overdue check                   Send notifications for overdue habits");
    std::println("  overdue web [--port <n>]        Open a dashboard in your browser (default :8080)");
    std::println("  overdue setstreak <name> <s>    Set streak (daily/weekly/monthly/3d...)");
    std::println("  overdue delstreak <name>        Remove streak tracking");
    std::println("  overdue stats [name]            Global stats, or detail for one entry");
    std::println("  overdue config                  Show settings and config file path");
    std::println("  overdue config set <k> <v>      data-dir|unlog-grace|web-port|date-format|date-order|");
    std::println("                                  date-sep|clock|show-seconds|timezone|week-start|");
    std::println("                                  notify|notify-cooldown|quiet-hours");
}

int main(int argc, char* argv[]) {
    if (argc < 2) { print_usage(); return 0; }

    try {
        Config cfg = Config::load(Config::default_path());
        datetime_format() = cfg.effective_date_format();
        timezone_name()   = cfg.timezone;
        week_start_day()  = (cfg.week_start == "sunday") ? std::chrono::Sunday
                                                         : std::chrono::Monday;
        Tracker tracker{cfg.data_path(), cfg.unlog_grace_secs};
        std::string cmd = argv[1];

        if (cmd == "add") {
            if (argc < 3) { std::println(stderr, "Usage: overdue add <name> [--alarm <dur>] [--streak daily|weekly|monthly|<dur>] [--unit <u>] [--target <n>] [--tag <t>]..."); return 1; }

            std::optional<long long> alarm;
            std::optional<StreakConfig> streak;
            std::optional<std::string> unit;
            std::optional<double> target;
            std::vector<std::string> tags;
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
                } else if ((arg == "--tag" || arg == "--tags") && i + 1 < argc) {
                    tags.push_back(argv[++i]);
                } else {
                    name_parts.push_back(arg);
                }
            }
            if (name_parts.empty()) { std::println(stderr, "Missing activity name."); return 1; }
            std::string name;
            for (const auto& p : name_parts) { if (!name.empty()) name += ' '; name += p; }

            if (!tracker.add(name, alarm, streak, unit, target, tags))
                std::println(stderr, "\"{}\" is already being tracked.", name);
            else
                std::println("✓ Now tracking \"{}\"", name);
        }
        else if (cmd == "addtask") {
            if (argc < 3) { std::println(stderr, "Usage: overdue addtask <name> [--unit <u>] [--target <n>] [--tag <t>]..."); return 1; }

            std::optional<std::string> unit;
            std::optional<double> target;
            std::vector<std::string> tags;
            std::vector<std::string> name_parts;

            for (int i = 2; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--unit" && i + 1 < argc) {
                    unit = argv[++i];
                } else if (arg == "--target" && i + 1 < argc) {
                    target = parse_amount(argv[++i]);
                    if (!target) { std::println(stderr, "Invalid target. Expected a non-negative number."); return 1; }
                } else if ((arg == "--tag" || arg == "--tags") && i + 1 < argc) {
                    tags.push_back(argv[++i]);
                } else {
                    name_parts.push_back(arg);
                }
            }
            if (name_parts.empty()) { std::println(stderr, "Missing task name."); return 1; }
            std::string name;
            for (const auto& p : name_parts) { if (!name.empty()) name += ' '; name += p; }

            if (!tracker.addtask(name, unit, target, tags))
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
                std::println("✓ Last log for \"{}\" unlogged — restore within {} via 'overdue logs/relog'.",
                    name, format_duration(cfg.unlog_grace_secs));
        }
        else if (cmd == "logs") {
            if (argc < 3) { std::println(stderr, "Usage: overdue logs <name>"); return 1; }
            auto name = join_args(std::span(argv + 2, argc - 2));
            auto a = tracker.find(name);
            if (!a) { std::println(stderr, "\"{}\" not found.", name); return 1; }

            auto rows = tracker.log_rows(name);
            std::string u = a->unit ? " " + *a->unit : "";
            std::println("{:<4} {:<21} {:<12} {}", "id", "When", "Amount", "Status");
            std::println("{}", std::string(72, '-'));
            int id = 1;
            for (const auto& r : rows) {
                std::string amt = r.entry.amount ? format_amount(*r.entry.amount) + u : "-";
                std::string status = "active";
                if (r.unlogged_at) {
                    auto age = std::chrono::duration_cast<std::chrono::seconds>(now() - *r.unlogged_at).count();
                    long long remaining = cfg.unlog_grace_secs - age;
                    status = std::format("unlogged {} ago · restorable for {}",
                        format_elapsed(*r.unlogged_at), format_duration(remaining > 0 ? remaining : 0));
                }
                std::println("{:<4} {:<21} {:<12} {}", id++, format_datetime(r.entry.when), amt, status);
            }
        }
        else if (cmd == "relog") {
            if (argc < 4) { std::println(stderr, "Usage: overdue relog <name> <id>"); return 1; }
            std::string id_str = argv[argc - 1];
            int id = 0;
            try {
                std::size_t pos = 0;
                id = std::stoi(id_str, &pos);
                if (pos != id_str.size()) throw std::invalid_argument("trailing");
            } catch (...) {
                std::println(stderr, "Invalid id \"{}\". Use the id from 'overdue logs <name>'.", id_str);
                return 1;
            }
            auto name = join_args(std::span(argv + 2, argc - 3));
            switch (tracker.relog(name, id)) {
                case RelogResult::Ok:
                    std::println("✓ Restored log #{} for \"{}\".", id, name); break;
                case RelogResult::NotFound:
                    std::println(stderr, "\"{}\" not found.", name); return 1;
                case RelogResult::BadId:
                    std::println(stderr, "No log with id {} for \"{}\". See 'overdue logs {}'.", id, name, name); return 1;
                case RelogResult::NotUnlogged:
                    std::println(stderr, "Log #{} for \"{}\" is active, not unlogged — nothing to restore.", id, name); return 1;
            }
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
            // Filters: --done (include completed tasks), --tag <t> (repeatable,
            // match-any), --type habit|task (restrict to one section).
            bool show_done = false;
            bool show_habits = true, show_tasks = true;
            std::vector<std::string> filter_tags;
            for (int i = 2; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--done") {
                    show_done = true;
                } else if ((arg == "--tag" || arg == "--tags") && i + 1 < argc) {
                    filter_tags.push_back(argv[++i]);
                } else if (arg == "--type" && i + 1 < argc) {
                    std::string t = argv[++i];
                    if (t == "habit" || t == "habits") { show_habits = true; show_tasks = false; }
                    else if (t == "task" || t == "tasks") { show_habits = false; show_tasks = true; }
                    else { std::println(stderr, "Invalid --type \"{}\". Use habit or task.", t); return 1; }
                } else {
                    std::println(stderr, "Usage: overdue list [--done] [--type habit|task] [--tag <t>]..."); return 1;
                }
            }

            // An activity passes when it carries any of the requested tags (or no
            // tag filter was given).
            auto matches = [&](const Activity& a) {
                if (filter_tags.empty()) return true;
                for (const auto& t : filter_tags)
                    if (has_tag(a, t)) return true;
                return false;
            };

            std::vector<Activity> habits, tasks;
            if (show_habits)
                for (auto& a : tracker.habits()) if (matches(a)) habits.push_back(std::move(a));
            if (show_tasks)
                for (auto& a : tracker.tasks(show_done)) if (matches(a)) tasks.push_back(std::move(a));

            if (habits.empty() && tasks.empty()) {
                if (!filter_tags.empty())
                    std::println("Nothing matches tag{} {}.",
                        filter_tags.size() > 1 ? "s" : "", format_tags(filter_tags));
                else
                    std::println("Nothing tracked. Use 'overdue add <name>' or 'overdue addtask <name>'.");
                return 0;
            }

            if (!habits.empty()) {
                std::println("Habits");
                std::println("{:<22} {:<21} {:<16} {:<8} {:<14} {}", "Activity", "Last done", "Elapsed", "Alarm", "Streak", "Tags");
                std::println("{}", std::string(90, '-'));
                for (const auto& a : habits) {
                    auto alarm = a.alert_after ? format_duration(*a.alert_after) : "-";
                    std::string streak_col = "-";
                    if (a.streak) {
                        int s = compute_streak(a);
                        streak_col = std::format("{} ({})", s, format_streak_label(*a.streak));
                    }
                    std::println("{:<22} {:<21} {:<16} {:<8} {:<14} {}", a.name,
                        format_datetime(last_done(a)), format_elapsed(last_done(a)), alarm,
                        streak_col, format_tags(a.tags));
                }
            }

            if (!tasks.empty()) {
                if (!habits.empty()) std::println("");
                std::println("Tasks");
                std::println("{:<22} {:<21} {:<22} {}", "Task", "Added", "Status", "Tags");
                std::println("{}", std::string(80, '-'));
                for (const auto& a : tasks) {
                    std::string status = a.completed_at
                        ? std::format("✓ done ({})", format_datetime(*a.completed_at))
                        : format_elapsed(a.logs.front().when);
                    std::println("{:<22} {:<21} {:<22} {}", a.name,
                        format_datetime(a.logs.front().when), status, format_tags(a.tags));
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
            if (!a->tags.empty())
                std::println("  tags:   {}", format_tags(a->tags));
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
        else if (cmd == "tag") {
            if (argc < 4) { std::println(stderr, "Usage: overdue tag <name> <tag>"); return 1; }
            std::string tag = argv[argc - 1];
            auto name = join_args(std::span(argv + 2, argc - 3));
            if (normalize_tag(tag).empty()) { std::println(stderr, "Empty tag."); return 1; }
            if (!tracker.addtag(name, tag))
                std::println(stderr, "\"{}\" not found.", name);
            else
                std::println("✓ Tagged \"{}\" with '{}'", name, normalize_tag(tag));
        }
        else if (cmd == "untag") {
            if (argc < 4) { std::println(stderr, "Usage: overdue untag <name> <tag>"); return 1; }
            std::string tag = argv[argc - 1];
            auto name = join_args(std::span(argv + 2, argc - 3));
            if (!tracker.deltag(name, tag))
                std::println(stderr, "\"{}\" not found or not tagged '{}'.", name, normalize_tag(tag));
            else
                std::println("✓ Removed tag '{}' from \"{}\"", normalize_tag(tag), name);
        }
        else if (cmd == "check") {
            if (!cfg.notify_enabled) return 0;
            if (in_quiet_hours(cfg.notify_quiet_start, cfg.notify_quiet_end)) return 0;
            auto overdue = tracker.overdue_activities();
            if (overdue.empty()) return 0;

            auto state_path = cfg.data_dir / "notify_state.json";
            auto state = load_notify_state(state_path);
            long long now_s = std::chrono::duration_cast<std::chrono::seconds>(
                now().time_since_epoch()).count();
            bool changed = false;
            for (const auto& a : overdue) {
                if (cfg.notify_cooldown_secs > 0) {
                    auto it = state.find(a.name);
                    if (it != state.end() && now_s - it->second < cfg.notify_cooldown_secs)
                        continue; // still cooling down for this activity
                }
                auto elapsed = format_elapsed(last_done(a));
                auto threshold = format_duration(*a.alert_after);
                notify(std::format("overdue: {}", a.name),
                       std::format("{} since last done (alarm: {})", elapsed, threshold));
                state[a.name] = now_s;
                changed = true;
            }
            if (changed && cfg.notify_cooldown_secs > 0)
                save_notify_state(state_path, state);
        }
        else if (cmd == "web") {
            int port = cfg.web_port;
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
            run_web(cfg.data_path(), port);
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
        else if (cmd == "config") {
            auto cfg_path = Config::default_path();
            if (argc >= 4 && std::string(argv[2]) == "set") {
                std::string key = argv[3];
                if (argc < 5) {
                    std::println(stderr, "Usage: overdue config set {} <value>", key);
                    return 1;
                }
                if (key == "data-dir") {
                    cfg.data_dir = expand_home(argv[4]);
                    Config::save(cfg_path, cfg);
                    std::println("✓ data-dir set to {}", cfg.data_dir.string());
                    std::println("  (existing data is not moved — copy data.json there if you want to keep it)");
                } else if (key == "unlog-grace") {
                    auto dur = parse_duration(argv[4]);
                    if (!dur) { std::println(stderr, "Invalid duration \"{}\". Examples: 24h, 1d, 30m", argv[4]); return 1; }
                    cfg.unlog_grace_secs = *dur;
                    Config::save(cfg_path, cfg);
                    std::println("✓ unlog-grace set to {}", format_duration(*dur));
                } else if (key == "web-port") {
                    auto p = parse_amount(argv[4]);
                    if (!p || *p < 1 || *p > 65535 || *p != std::floor(*p)) {
                        std::println(stderr, "Invalid port \"{}\". Expected an integer 1-65535.", argv[4]); return 1;
                    }
                    cfg.web_port = static_cast<int>(*p);
                    Config::save(cfg_path, cfg);
                    std::println("✓ web-port set to {}", cfg.web_port);
                } else if (key == "date-format") {
                    // The format may contain spaces (e.g. "%Y-%m-%d %H:%M"), so take
                    // everything after the key as one value.
                    std::string arg = join_args(std::span(argv + 4, argc - 4));
                    // A bare preset name expands to its format; otherwise the arg
                    // is a raw chrono format string.
                    std::string fmt = lookup_date_preset(arg).value_or(arg);
                    if (!is_valid_datetime_format(fmt)) {
                        std::println(stderr, "Invalid date format \"{}\". Try a preset ({}) or a chrono string like %Y-%m-%d %H:%M:%S", arg, preset_names_csv()); return 1;
                    }
                    cfg.date_format = fmt; // switch to custom mode
                    Config::save(cfg_path, cfg);
                    datetime_format() = fmt;
                    std::println("✓ date-format set to \"{}\"  (e.g. {})", fmt, format_datetime(now()));
                } else if (key == "date-order") {
                    std::string v = argv[4];
                    if (v != "ymd" && v != "dmy" && v != "mdy") {
                        std::println(stderr, "Invalid date-order \"{}\". Use ymd, dmy, or mdy.", v); return 1;
                    }
                    cfg.date_order = v;
                    cfg.date_format.clear(); // switch to structured mode
                    Config::save(cfg_path, cfg);
                    datetime_format() = cfg.effective_date_format();
                    std::println("✓ date-order set to {}  (e.g. {})", v, format_datetime(now()));
                } else if (key == "date-sep") {
                    auto sep = parse_date_sep(argv[4]);
                    if (!sep) {
                        std::println(stderr, "Invalid date-sep \"{}\". Use - . / or the words dash/dot/slash/space.", argv[4]); return 1;
                    }
                    cfg.date_sep = *sep;
                    cfg.date_format.clear();
                    Config::save(cfg_path, cfg);
                    datetime_format() = cfg.effective_date_format();
                    std::println("✓ date-sep set to '{}'  (e.g. {})", *sep, format_datetime(now()));
                } else if (key == "clock") {
                    std::string v = argv[4];
                    if (v != "12h" && v != "24h") {
                        std::println(stderr, "Invalid clock \"{}\". Use 12h or 24h.", v); return 1;
                    }
                    cfg.clock = v;
                    cfg.date_format.clear();
                    Config::save(cfg_path, cfg);
                    datetime_format() = cfg.effective_date_format();
                    std::println("✓ clock set to {}  (e.g. {})", v, format_datetime(now()));
                } else if (key == "show-seconds") {
                    auto b = parse_bool(argv[4]);
                    if (!b) { std::println(stderr, "Invalid value \"{}\". Use on/off.", argv[4]); return 1; }
                    cfg.show_seconds = *b;
                    cfg.date_format.clear();
                    Config::save(cfg_path, cfg);
                    datetime_format() = cfg.effective_date_format();
                    std::println("✓ show-seconds {}  (e.g. {})", *b ? "on" : "off", format_datetime(now()));
                } else if (key == "timezone") {
                    std::string tz = argv[4];
                    // "" / "system" / "auto" clears the override and follows the OS.
                    if (tz == "system" || tz == "auto" || tz == "\"\"") tz.clear();
                    if (!tz.empty() && !is_valid_timezone(tz)) {
                        std::println(stderr, "Unknown timezone \"{}\". Use an IANA name like Europe/Paris, or \"system\".", tz); return 1;
                    }
                    cfg.timezone = tz;
                    Config::save(cfg_path, cfg);
                    timezone_name() = tz;
                    std::println("✓ timezone set to {}  (now: {})",
                        tz.empty() ? "system" : tz, format_datetime(now()));
                } else if (key == "week-start") {
                    std::string ws = argv[4];
                    if (ws != "monday" && ws != "sunday") {
                        std::println(stderr, "Invalid week-start \"{}\". Use monday or sunday.", ws); return 1;
                    }
                    cfg.week_start = ws;
                    Config::save(cfg_path, cfg);
                    std::println("✓ week-start set to {}", ws);
                } else if (key == "notify") {
                    auto b = parse_bool(argv[4]);
                    if (!b) { std::println(stderr, "Invalid value \"{}\". Use on/off (or true/false).", argv[4]); return 1; }
                    cfg.notify_enabled = *b;
                    Config::save(cfg_path, cfg);
                    std::println("✓ notify {}", *b ? "enabled" : "disabled");
                } else if (key == "notify-cooldown") {
                    // "0"/"off" disables throttling; otherwise a duration like 30m, 2h.
                    std::string v = argv[4];
                    long long secs;
                    if (v == "0" || v == "off" || v == "none") secs = 0;
                    else {
                        auto dur = parse_duration(v);
                        if (!dur) { std::println(stderr, "Invalid duration \"{}\". Examples: 30m, 1h, 0 (off).", v); return 1; }
                        secs = *dur;
                    }
                    cfg.notify_cooldown_secs = secs;
                    Config::save(cfg_path, cfg);
                    std::println("✓ notify-cooldown set to {}", secs ? format_duration(secs) : "off");
                } else if (key == "quiet-hours") {
                    // "22-7" (local hours) or "off"/"none" to disable.
                    std::string v = argv[4];
                    int qs = 0, qe = 0;
                    if (v != "off" && v != "none") {
                        auto dash = v.find('-');
                        auto ps = (dash != std::string::npos) ? parse_amount(v.substr(0, dash)) : std::nullopt;
                        auto pe = (dash != std::string::npos) ? parse_amount(v.substr(dash + 1)) : std::nullopt;
                        if (!ps || !pe || *ps != std::floor(*ps) || *pe != std::floor(*pe) ||
                            *ps < 0 || *ps > 23 || *pe < 0 || *pe > 23) {
                            std::println(stderr, "Invalid quiet-hours \"{}\". Use START-END in 0-23 (e.g. 22-7), or off.", v); return 1;
                        }
                        qs = static_cast<int>(*ps); qe = static_cast<int>(*pe);
                    }
                    cfg.notify_quiet_start = qs;
                    cfg.notify_quiet_end = qe;
                    Config::save(cfg_path, cfg);
                    if (qs == qe) std::println("✓ quiet-hours off");
                    else std::println("✓ quiet-hours set to {:02d}:00–{:02d}:00", qs, qe);
                } else {
                    std::println(stderr, "Unknown setting \"{}\". Known: data-dir, unlog-grace, web-port, date-format, date-order, date-sep, clock, show-seconds, timezone, week-start, notify, notify-cooldown, quiet-hours", key);
                    return 1;
                }
            } else if (argc == 2) {
                std::println("Configuration ({})", cfg_path.string());
                std::println("{}", std::string(50, '-'));
                std::println("  data-dir:     {}", cfg.data_dir.string());
                std::println("  unlog-grace:  {}   (window to restore an unlogged entry)",
                    format_duration(cfg.unlog_grace_secs));
                std::println("  web-port:     {}", cfg.web_port);
                if (cfg.date_format.empty())
                    std::println("  date/time:    {} · sep '{}' · {} · seconds {}   (e.g. {})",
                        cfg.date_order, cfg.date_sep, cfg.clock,
                        cfg.show_seconds ? "on" : "off", format_datetime(now()));
                else
                    std::println("  date/time:    {}   (custom · e.g. {})",
                        cfg.date_format, format_datetime(now()));
                std::println("  timezone:     {}", cfg.timezone.empty() ? "system" : cfg.timezone);
                std::println("  week-start:   {}", cfg.week_start);
                std::println("  notify:       {}", cfg.notify_enabled ? "on" : "off");
                std::println("  notify-cooldown: {}   (min gap between repeat alerts)",
                    cfg.notify_cooldown_secs ? format_duration(cfg.notify_cooldown_secs) : "off");
                if (cfg.notify_quiet_start == cfg.notify_quiet_end)
                    std::println("  quiet-hours:  off");
                else
                    std::println("  quiet-hours:  {:02d}:00–{:02d}:00   (no notifications)",
                        cfg.notify_quiet_start, cfg.notify_quiet_end);
            } else {
                std::println(stderr, "Usage: overdue config | overdue config set <key> <value>");
                std::println(stderr, "Keys: data-dir, unlog-grace, web-port, date-format, date-order, date-sep, clock, show-seconds, timezone, week-start, notify, notify-cooldown, quiet-hours");
                std::println(stderr, "date-format presets: {}", preset_names_csv());
                return 1;
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
