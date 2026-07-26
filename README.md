# game-session

A lightweight C++ wrapper that optimises your GPU and monitor settings for gaming
and restores everything when you're done. Designed to sit in Steam's launch options
and chain into CachyOS's `game-performance`.

```
game-session %command%
   ├── save current GPU state (sysfs) + monitor preset (DDC/CI)
   ├── apply gaming profile:
   │   ├── GPU: force performance level, 3D power profile, raised power cap
   │   └── monitor: picture mode, response time, black stabiliser, colour preset
   ├── game-performance <your command>    ← blocks until the game exits
   └── restore everything
```

## Features

- **AMD GPU** — writes `power_dpm_force_performance_level`, `pp_power_profile_mode`
  and `power1_cap` via a minimal privileged helper.
- **Monitor DDC/CI** — switches your display to a gaming picture preset (FPS, RTS,
  Gamer 1, …) and restores it afterwards.
- **CPU / sleep** — delegates to CachyOS's `game-performance`, which toggles the
  performance governor and inhibits suspend.
- **Provider‑based save/restore** — the main binary snapshots the current state
  before touching anything, and a `SIGINT`/`SIGTERM` handler guarantees rollback
  even if the game is killed.
- **Configurable presets** — pick your monitor preset and GPU parameters through
  environment variables or a config file.
- **Steam‑friendly** — drops into `%command%` and inherits whatever extra
  environment variables you set in the launch options.

## Requirements

- Linux with an **AMD GPU** (RDNA 2 / RX 6000 series and later)
- `g++` (GCC 12+), `make` (build only)
- `game-performance` (CachyOS — provides `powerprofilesctl` + `systemd-inhibit`)
- `ddcutil` — DDC/CI monitor control
- LG UltraGear monitor (or any DDC/CI display; you can tweak the VCP codes)

## Install

```bash
git clone https://github.com/caesar96/game-session.git
cd game-session
./install.sh
```

Binaries go to `/usr/local/bin/` (owned by root), sudoers to `/etc/sudoers.d/`.

### Uninstall

```bash
cd game-session
./uninstall.sh
```

### Verify

```bash
game-session echo "it works"
```

## Configuration

### Environment variables

| Variable | Default | Description |
|---|---|---|
| `MONITOR_PRESET` | `RTS` | Monitor picture preset: `FPS`, `RTS`, `Gamer 1`, `Gamer 2`, `Vivid`, `Reader`, `HDR Effect` |
| `MONITOR_MATCH` | `GSM` | String to match your monitor in `ddcutil detect` output |
| `GS_GPU_FORCE_LEVEL` | `high` | `auto`, `low`, `high`, or `manual` |
| `GS_GPU_PROFILE` | `1` | Power profile index (`0` = BOOTUP_DEFAULT, `1` = 3D_FULL_SCREEN, …) |
| `GS_GPU_POWER_CAP` | `120000000` | Power limit in microwatts (`120000000` = 120 W) |

### Config file

Persist your settings in `~/.config/game-session/game-session.conf` (auto‑created on first run):

```ini
# ~/.config/game-session/game-session.conf
[gpu]
force_level = high
profile = 1
power_cap = 120000000
min_clock = 2650
max_clock = 2750
memory_clock = 950
voltage_offset = -5

[monitor]
preset = RTS

[fan]
enabled = true
curve = 40:60,50:100,60:170,65:220,70:255
```

Environment variables override config file values and Steam launch options
override both.

### Default Steam environment variables

The binary automatically exports the following defaults **without overriding**
variables you have already set in your launch options:

- `PROTON_FSR4_UPGRADE=1`
- `ENABLE_LAYER_MESA_ANTI_LAG=1`
- `PROTON_ENABLE_WAYLAND=1`
- `PROTON_ENABLE_HDR=1`
- `MANGOHUD=1`
- `MANGOHUD_CONFIG=cpu_temp,gpu_temp,cpu_stats,fps,frame_timing`

## Usage

### Steam

In a game's **Launch Options**:

```
game-session %command%
```

Override any variable you want:

```
PROTON_FSR4_UPGRADE=0 MONITOR_PRESET=FPS game-session %command%
```

### Command line

```bash
game-session mangohud game-performance lutris battle.net
game-session steam steam://rungameid/12345
game-session your-game
```

## How it works

```
game-session ./mygame
  │
  ├─ load_config()          ← ~/.config/game-session/game-session.conf
  ├─ apply_default_env()    ← exports PROTON_*, MANGOHUD*, etc.
  │
  ├─ save_gpu_state()       ← read sysfs → /tmp/game-session-XXXXX/gpu/
  ├─ save_monitor_state()   ← ddcutil getvcp  → /tmp/…/monitor/
  │
  ├─ apply_gpu()
  │   └─ sudo game-session-helper force-level high
  │   └─ sudo game-session-helper profile 1
  │   └─ sudo game-session-helper power-cap 120000000
  │
  ├─ apply_monitor()
  │   └─ ddcutil setvcp 15 31  (RTS picture mode)
  │   └─ ddcutil setvcp F7 2   (response time)
  │   └─ ddcutil setvcp F9 55  (black stabiliser)
  │   └─ ddcutil setvcp 14 11  (colour preset)
  │
  ├─ fork + exec game-performance ./mygame
  ├─ waitpid (blocks until the game exits)
  │
  ├─ restore_monitor()       ← ddcutil setvcp with saved values
  ├─ restore_gpu()           ← sudo game-session-helper with saved values
  └─ rm -rf /tmp/game-session-XXXXX
```

If the game is killed with `Ctrl+C` / `SIGTERM`, the signal handler forwards
the signal to the child process and then runs the full restore path before
exiting.

## Project structure

```
game-session/
├── game-session.cpp            ← orchestrator (user‑facing binary)
├── game-session-helper.cpp     ← privileged helper (called via sudo)
├── amdgpu_overdrive.cpp/hpp    ← AMDGPU OD class (auto‑detect, sysfs, reset)
├── install.sh                  ← build + install in one step
├── uninstall.sh                ← remove binaries + sudoers
├── CMakeLists.txt
├── cmake/
│   └── uninstall.cmake.in
├── sudo/
│   └── game-session.in
└── README.md
```

The helper is intentionally tiny and compiled separately so nobody can read or
modify the code that runs as root. It validates every argument — no shell
execution, no file traversal, no injection vectors.

## License

MIT
