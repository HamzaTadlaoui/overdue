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

---

## Commands

| Command | Description |
|---|---|
| `overdue add <name>` | Track a recurring habit (`[--unit u] [--target n]`) |
| `overdue addtask <name>` | Add a one-time task (`[--unit u] [--target n]`) |
| `overdue log <name>` | Mark habit as done right now |
| `overdue log <name> --ago <dur>` | Mark as done X time ago |
| `overdue log <name> --at <datetime>` | Mark as done at a specific date/time |
| `overdue log <name> --amount <n>` | Record a quantity with the log |
| `overdue unlog <name>` | Cancel the last log |
| `overdue done <name>` | Mark a task as completed (archives it) |
| `overdue list` | Show habits and active tasks |
| `overdue list --done` | Also show completed tasks |
| `overdue show <name>` | Show details for one entry |
| `overdue stats [name]` | Global stats, or quantity detail for one entry |
| `overdue delete <name>` | Remove an entry |
| `overdue setalarm <name> <dur>` | Alert after this much time without logging |
| `overdue delalarm <name>` | Remove the alert |
| `overdue setunit <name> <unit>` | Label amounts for an entry (e.g. `km`) |
| `overdue delunit <name>` | Remove the unit label |
| `overdue settarget <name> <n>` | Set a goal for accumulated amount |
| `overdue deltarget <name>` | Remove the target |
| `overdue check` | Send desktop notifications for all overdue habits |
| `overdue web [--port <n>]` | Open an interactive dashboard in your browser (default `:8080`) |

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

### Duration format

Combine units freely: `3d`, `12h`, `30m`, `1d6h`, `1d6h30m`

### Date format for `--at`

- `2026-06-22` — midnight
- `2026-06-22T08:15` — specific time
- `2026-06-22T08:15:30` — with seconds

---

## Data

All logs are stored in `~/.local/share/overdue/data.json`, written atomically (temp file + rename) so a crash can't corrupt it. Every log entry is preserved — full history, never overwritten — which is what powers streaks and stats. Each log is `{ "t": <unix>, "q": <amount?> }`; older files that stored bare timestamps are upgraded automatically on the next write.

---

## Inspiration

Inspired by [Better Counter](https://f-droid.org/packages/org.kde.bettercounter/) — an open source Android app by KDE.

---

## License

MIT — made by [Hamza Tadlaoui](https://github.com/HamzaTadlaoui)
