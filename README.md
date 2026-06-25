# overdue

A minimal CLI tracker that tells you how long it's been since you last did something — and makes you feel it.

```
Activity               Last done             Elapsed
-------------------------------------------------------
running                2026-06-22 08:15:00   3d 6h 16m 54s
floss                  2026-06-18 21:00:00   6d 17h 31m 54s
meditation             2026-06-25 09:00:00   5h 47m 12s
```

---

## Requirements

- C++23 compiler (GCC ≥ 13, Clang ≥ 16)
- CMake ≥ 3.20

Dependencies are fetched automatically by CMake (nlohmann/json).

## Build & install

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix ~/.local
```

Optional short alias — add to your `~/.bashrc`:
```bash
alias od='overdue'
```

---

## Commands

| Command | Description |
|---|---|
| `overdue add <name>` | Start tracking a new activity |
| `overdue log <name>` | Mark as done right now |
| `overdue unlog <name>` | Cancel the last log |
| `overdue list` | Show all activities with elapsed time |
| `overdue show <name>` | Show one activity |
| `overdue delete <name>` | Stop tracking an activity |

Activity names can be multi-word without quotes: `overdue add brush teeth`

---

## Data

All logs are stored in `~/.local/share/overdue/data.json`. The full history is kept — every log entry is saved, which will be used for stats and alerts in future versions.

---

## License

MIT — made by [Hamza Tadlaoui](https://github.com/HamzaTadlaoui)
