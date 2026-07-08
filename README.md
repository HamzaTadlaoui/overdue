# overdue

A minimal CLI habit tracker that tells you exactly how long it's been since you last did something — and makes you feel it.

```
Activity               Last done             Elapsed           Alarm
---------------------------------------------------------------------
running                2026-06-22 08:15:00   3d 6h 16m 54s     5d
floss                  2026-06-18 21:00:00   6d 17h 31m 54s    2d  ← overdue
meditation             2026-06-25 09:00:00   5h 47m 12s        -
```

---

## Requirements

- C++23 compiler (GCC ≥ 14, Clang ≥ 17)
- CMake ≥ 3.20

Dependencies are fetched automatically by CMake ([nlohmann/json](https://github.com/nlohmann/json) and [cpp-httplib](https://github.com/yhirose/cpp-httplib) for the web dashboard).

## Build & install

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix ~/.local
```

Optional short alias — add to your `~/.bashrc` or `~/.zshrc`:
```bash
alias od='overdue'
```

## Auto-notifications (Linux)

Install the systemd timer to get desktop notifications automatically every hour:

```bash
mkdir -p ~/.config/systemd/user
cp systemd/* ~/.config/systemd/user/
systemctl --user enable --now overdue-check.timer
```

Requires `notify-send` (included in most GNOME/KDE setups).

The timer can run as often as you like — `notify-cooldown` keeps it from re-alerting the same
habit too frequently, and `quiet-hours` silences notifications overnight (see [Settings](#settings)).

---

## Commands

| Command | Description |
|---|---|
| `overdue add <name>` | Track a recurring habit (`[--unit u] [--target n] [--tag t]...`) |
| `overdue addtask <name>` | Add a one-time task (`[--unit u] [--target n] [--tag t]...`) |
| `overdue log <name>` | Mark habit as done right now |
| `overdue log <name> --ago <dur>` | Mark as done X time ago |
| `overdue log <name> --at <datetime>` | Mark as done at a specific date/time |
| `overdue log <name> --amount <n>` | Record a quantity with the log |
| `overdue unlog <name>` | Cancel the last log (recoverable for a grace period) |
| `overdue logs <name>` | List all logs for an entry with ids and unlogged status |
| `overdue relog <name> <id>` | Restore an unlogged entry by its id |
| `overdue done <name>` | Mark a task as completed (archives it) |
| `overdue list` | Show habits and active tasks |
| `overdue list --done` | Also show completed tasks |
| `overdue list --type habit\|task` | Show only habits or only tasks |
| `overdue list --tag <t>` | Show only entries with tag `<t>` (repeatable, matches any) |
| `overdue show <name>` | Show details for one entry |
| `overdue stats [name]` | Global stats, or quantity detail for one entry |
| `overdue delete <name>` | Remove an entry |
| `overdue setalarm <name> <dur>` | Alert after this much time without logging |
| `overdue delalarm <name>` | Remove the alert |
| `overdue setunit <name> <unit>` | Label amounts for an entry (e.g. `km`) |
| `overdue delunit <name>` | Remove the unit label |
| `overdue settarget <name> <n>` | Set a goal for accumulated amount |
| `overdue deltarget <name>` | Remove the target |
| `overdue tag <name> <tag>` | Add a category/tag to an entry |
| `overdue untag <name> <tag>` | Remove a tag from an entry |
| `overdue check` | Send desktop notifications for all overdue habits |
| `overdue web [--port <n>]` | Open an interactive dashboard in your browser (default `:8080`) |
| `overdue config` | Show settings and the active config file path |
| `overdue config set <key> <value>` | Change a setting (see [Settings](#settings)) |

Activity names can be multi-word without quotes: `overdue add brush teeth`

### Web dashboard

`overdue web` starts a tiny local server and opens a dashboard in your browser. It exposes
**every action the CLI has** — no terminal needed:

- summary cards: habits, total logs, tasks done, overdue
- add habits or tasks (with optional unit, target, alarm, streak)
- per-entry buttons: log (with optional amount and "ago"), unlog, mark task done, delete
- per-entry **Manage** panel: set/remove alarm, streak, unit, and target
- **live-ticking "since last done" timers** (updated every second in the browser), progress
  bars toward targets, streak/alarm badges, and overdue cards highlighted in red
- a **Stats** tab with a 14-day activity chart, global highlights (most consistent / neglected /
  logged, best streak), a per-habit breakdown (logs, streak, average interval, total, 7-day
  trend), and a task summary
- click any entry name for a **detail page**: a GitHub-style calendar heatmap (last 26 weeks),
  key stat cards, a quantity breakdown (total, avg/log, avg/day, best day, max, 7-day trend),
  the full log history, and the same log/manage controls inline

The page also refreshes its data every 30 seconds, pausing automatically while you're typing
in a field or have a panel open so it never interrupts you.

```bash
overdue web              # serve on http://127.0.0.1:8080 and open the browser
overdue web --port 9000  # pick a different port
```

Every change is written straight to the same `data.json` the CLI uses, so the two stay in
sync. Press `Ctrl+C` to stop the server.

**Safety:** the server binds to loopback (`127.0.0.1`) only, so it is never exposed on the
network. Each session also mints a random token that every form must include, which stops
other sites open in your browser from driving the dashboard behind your back. Concurrent
edits are serialized so two quick actions can't clobber each other.

### Undoing an unlog

`unlog` doesn't delete anything — it *flags* the most recent log as unlogged and removes it
from your active history (so streaks, stats, and the dashboard immediately ignore it). The
entry stays recoverable for a grace period (default **24h**), then it's purged for good.

`overdue logs <name>` shows every log with an id and its status, so you can spot and recover a
mistake:

```
id   When                  Amount    Status
--------------------------------------------------------------
1    2026-06-28 07:30:00   5 km      active
2    2026-06-29 07:45:00   6 km      active
3    2026-06-30 08:10:00   7 km      unlogged 4m ago · restorable for 23h
```

```bash
overdue unlog running        # flag the last log (recoverable)
overdue logs running         # find the id of the unlogged entry
overdue relog running 3      # restore it
```

The grace period is a setting — make it longer if you want a bigger safety net, or near-zero
to keep unlog effectively permanent:

```bash
overdue config                          # show current settings
overdue config set unlog-grace 7d       # a week to catch mistakes
```

### Quantities

Logs can carry an optional amount, so a habit or task becomes a counter as well as a
timestamp — useful for "5.2 km run" or "30 pushups". `--amount` combines with `--ago`/`--at`.

```bash
overdue add running --unit km --target 100   # optional unit label + goal
overdue log running --amount 5.2
overdue log running --amount 8 --ago 1d
overdue stats running                         # total, avg/log, avg/day, best day, 7-day trend
```

Amounts also work on tasks as **partial progress** — logging some doesn't complete the
task, it just records that you did something; `done` still marks it finished:

```bash
overdue addtask "write report" --unit pages --target 10
overdue log "write report" --amount 3        # 3 / 10 pages, still pending
```

Amounts are only meaningful within a single activity (km and pushups aren't comparable),
so quantity stats are always per-activity; the global `stats` view ignores them.

### Tags & filtering

Any habit or task can carry free-form tags (categories). Add them at creation with a
repeatable `--tag`, or later with `tag`/`untag`:

```bash
overdue add "morning run" --tag health --tag morning
overdue addtask "file taxes" --tag finance --tag admin
overdue tag reading learning        # add a tag to an existing entry
overdue untag reading learning      # remove it
```

Tags are stored lowercased, trimmed, de-duplicated and sorted, so `Work`, ` work ` and
`work` are the same tag. They show up as a column in `list` and on the `show` page.

`list` accepts filters that combine freely:

```bash
overdue list --tag health           # only entries tagged 'health'
overdue list --tag work --tag admin # entries tagged 'work' OR 'admin' (matches any)
overdue list --type task            # only tasks
overdue list --type habit --tag health --done
```

`--type` restricts to `habit` or `task`; `--tag` (repeatable) keeps entries carrying **any**
of the given tags; `--done` still includes completed tasks. All three can be used together.

### Duration format

Combine units freely: `3d`, `12h`, `30m`, `1d6h`, `1d6h30m`

### Date format for `--at`

- `2026-06-22` — midnight
- `2026-06-22T08:15` — specific time
- `2026-06-22T08:15:30` — with seconds

---

## Settings

Settings live in a JSON config file, separate from your data so they can point at it.
Show the current settings and where they're stored with:

```bash
overdue config
```

| Key | Default | Meaning |
|---|---|---|
| `data-dir` | `~/.local/share/overdue` | Directory holding `data.json` |
| `unlog-grace` | `24h` | Window to restore an unlogged entry (`30m`, `7d`, …) |
| `web-port` | `8080` | Default port for `overdue web` |
| `date-format` | *(structured)* | Timestamp format — a preset, a raw [strftime-style](https://en.cppreference.com/w/cpp/chrono/system_clock/formatter) string, or built from the knobs below |
| `date-order` | `ymd` | Component order: `ymd`, `dmy`, or `mdy` |
| `date-sep` | `-` | Separator between date parts: `-` `.` `/` (or `dash`/`dot`/`slash`/`space`) |
| `clock` | `24h` | `24h` (`22:15`) or `12h` (`10:15 PM`) |
| `show-seconds` | `on` | Include `:SS` in the time |
| `timezone` | *(system)* | IANA zone for rendering/bucketing all times (e.g. `Europe/Paris`); `system` follows the OS |
| `week-start` | `monday` | First day of the week for weekly streaks and the heatmap (`monday`/`sunday`) |
| `notify` | `on` | Whether `overdue check` sends desktop notifications |
| `notify-cooldown` | `1h` | Minimum gap between repeat alerts for the same overdue habit (`30m`, `off`) |
| `quiet-hours` | `off` | Nightly window where `overdue check` stays silent, e.g. `22-7` (wraps midnight) |

```bash
overdue config set unlog-grace 7d
overdue config set web-port 9000
overdue config set timezone Europe/Paris
overdue config set week-start sunday
overdue config set notify-cooldown 30m       # don't re-alert the same habit within 30m
overdue config set quiet-hours 22-7          # silence 22:00–06:59
overdue config set notify off
```

Invalid values (a bad port, an unknown timezone, an unparseable date format, …) are rejected without changing anything.

### Date & time format

There are three ways to control how timestamps look, from easiest to most flexible:

**Presets** — one word that expands to a full format:

| Preset | Example |
|---|---|
| `iso` | `2026-07-07 22:15:03` |
| `us` | `07/07/2026 10:15 PM` |
| `eu` | `07/07/2026 22:15` |
| `uk` | `07-07-2026 22:15:03` |
| `compact` | `20260707-2215` |
| `long` | `Tuesday, 07 July 2026 22:15` |

```bash
overdue config set date-format eu
```

**Structured knobs** — mix `date-order`, `date-sep`, `clock`, and `show-seconds` and the app
builds the format for you (no `%` codes to remember):

```bash
overdue config set date-order dmy
overdue config set date-sep slash
overdue config set clock 12h
overdue config set show-seconds off      # → 07/07/2026 10:15 PM
```

**Raw string** — a full [chrono/strftime](https://en.cppreference.com/w/cpp/chrono/system_clock/formatter)
format for anything the above can't express (names may contain spaces, no quotes needed):

```bash
overdue config set date-format %A %d %b, %I:%M %p
```

Setting `date-format` (preset or raw) switches to a *custom* format used verbatim; setting any
structured knob switches back to *structured* mode. `overdue config` shows which mode is active
along with a live example. This format also drives the clock style (12h vs 24h) and applies
everywhere times are shown, including the web dashboard.

### Config location & separate profiles

By default the config file is `~/.config/overdue/config.json` (honoring `$XDG_CONFIG_HOME`).
Set the `OVERDUE_CONFIG` environment variable to use a different one — this is how you keep a
throwaway **dev/testing** profile completely separate from your **real** habit data:

```bash
# real usage — the default config and data
overdue list

# development — its own config pointing at throwaway data, your real history untouched
export OVERDUE_CONFIG=~/.config/overdue/dev.json
overdue config set data-dir /tmp/overdue-dev
overdue list        # this shell now only sees the dev data
```

Because `data-dir` is itself a setting, each config file can point at its own data directory,
so switching profiles switches both settings and data together.

## Data

All logs are stored in `data.json` inside `data-dir` (default `~/.local/share/overdue/`, honoring `$XDG_DATA_HOME`), written atomically (temp file + rename) so a crash can't corrupt it. Every log entry is preserved — full history, never overwritten — which is what powers streaks and stats. Each log is `{ "t": <unix>, "q": <amount?> }`; older files that stored bare timestamps are upgraded automatically on the next write. Unlogged-but-not-yet-purged entries are kept in a per-activity `"unlogged"` array (each carrying its original log plus the unlog time) until their grace period elapses.

---

## Inspiration

Inspired by [Better Counter](https://f-droid.org/packages/org.kde.bettercounter/) — an open source Android app by KDE.

---

## License

MIT — made by [Hamza Tadlaoui](https://github.com/HamzaTadlaoui)
