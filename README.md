# dayssince

A minimal CLI tracker that answers one question: *how long has it been since I last did X?*

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
alias ds='dayssince'
```

---

## Commands

| Command | Description |
|---|---|
| `dayssince add <name>` | Start tracking a new activity |
| `dayssince log <name>` | Mark as done right now |
| `dayssince unlog <name>` | Cancel the last log |
| `dayssince list` | Show all activities with elapsed time |
| `dayssince show <name>` | Show one activity |
| `dayssince delete <name>` | Stop tracking an activity |

Activity names can be multi-word without quotes: `dayssince add brush teeth`

---

## Data

All logs are stored in `~/.local/share/dayssince/data.json`. The full history is kept — every log entry is saved, which will be used for stats in future versions.

---

## License

MIT — made by [Hamza Tadlaoui](https://github.com/HamzaTadlaoui)
