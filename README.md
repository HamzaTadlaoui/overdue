# dayssince

A minimal CLI tracker that answers one question: *how long has it been since I last did X?*

Log any habit, chore, or activity. `dayssince` persists the dates locally and tells you the elapsed time on demand.

---

## Requirements

- C++20 compiler (GCC ≥ 11, Clang ≥ 13, MSVC 2022)
- CMake ≥ 3.20
- [nlohmann/json](https://github.com/nlohmann/json) (bundled in `include/nlohmann/`)

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The binary is placed at `build/dayssince`.

Optionally install system-wide:

```bash
sudo cmake --install build
```

---

## Commands

| Command | Description |
|---|---|
| `dayssince add <name>` | Start tracking a new activity (logged as today) |
| `dayssince log <name>` | Record that you just did it (reset counter to 0) |
| `dayssince list` | Show all activities with elapsed days |
| `dayssince show <name>` | Show elapsed time for one activity |
| `dayssince delete <name>` | Stop tracking an activity |

---

## Example session

```
$ dayssince add running
✓ Now tracking "running" (last done: today)

$ dayssince add floss
✓ Now tracking "floss" (last done: today)

$ dayssince list
 Activity   Last done    Days ago
 ─────────────────────────────────
 running    2026-06-25   0
 floss      2026-06-25   0

# Three days later…

$ dayssince list
 Activity   Last done    Days ago
 ─────────────────────────────────
 running    2026-06-25   3
 floss      2026-06-25   3

$ dayssince log running
✓ "running" updated (last done: today)

$ dayssince show floss
floss — last done 2026-06-25 (3 days ago)

$ dayssince delete floss
✓ "floss" removed.
```

---

## Data storage

Activities are saved in `~/.local/share/dayssince/data.json` (Linux/macOS) or `%APPDATA%\dayssince\data.json` (Windows).

---

## License

MIT
