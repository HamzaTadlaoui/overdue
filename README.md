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

- C++23 compiler (GCC ≥ 13, Clang ≥ 16)
- CMake ≥ 3.20

Dependencies are fetched automatically by CMake ([nlohmann/json](https://github.com/nlohmann/json)).

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
| `overdue add <name>` | Start tracking a new activity |
| `overdue log <name>` | Mark as done right now |
| `overdue log <name> --ago <dur>` | Mark as done X time ago |
| `overdue log <name> --at <datetime>` | Mark as done at a specific date/time |
| `overdue unlog <name>` | Cancel the last log |
| `overdue list` | Show all activities with elapsed time and alarms |
| `overdue show <name>` | Show details for one activity |
| `overdue delete <name>` | Stop tracking an activity |
| `overdue setalarm <name> <dur>` | Alert after this much time without logging |
| `overdue delalarm <name>` | Remove the alert |
| `overdue check` | Send desktop notifications for all overdue activities |

Activity names can be multi-word without quotes: `overdue add brush teeth`

### Duration format

Combine units freely: `3d`, `12h`, `30m`, `1d6h`, `1d6h30m`

### Date format for `--at`

- `2026-06-22` — midnight
- `2026-06-22T08:15` — specific time
- `2026-06-22T08:15:30` — with seconds

---

## Data

All logs are stored in `~/.local/share/overdue/data.json`. Every log entry is preserved — full history, never overwritten. This will power stats and streaks in future versions.

---

## License

MIT — made by [Hamza Tadlaoui](https://github.com/HamzaTadlaoui)
