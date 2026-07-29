# game-session

A lightweight C++ wrapper that applies an AMD GPU and monitor gaming profile,
then restores the monitor and resets the GPU to its default/automatic state
when you're done. It sits in Steam's launch options and chains into CachyOS's
`game-performance`.

```
game-session %command%
   ├── save monitor + HDR + VRR state (DDC/CI / kscreen-doctor)
   ├── apply gaming profile:
   │   ├── GPU: power profile, power cap, overclock (OD), force level
   │   ├── monitor: picture mode, response time, black stabiliser, colour
   │   └── fan: custom temperature‑based curve
   ├── game-performance <your command>    ← blocks until the game exits
   └── restore monitor; reset GPU to defaults/automatic mode
```

## Features

- **AMD GPU OverDrive** — SCLK, MCLK, voltage offset with automatic interface
  detection (RDNA 1/2, legacy). Validates against hardware limits.
- **Monitor DDC/CI** — switches to a gaming picture preset (FPS, RTS, …) and
  restores the original values afterwards. HDR is temporarily disabled before
  restore when necessary, then returned to its pre-session state.
- **Adaptive sync** — switches VRR to `Automatic` while the game runs and
  restores the previous `Never`, `Always` or `Automatic` policy afterwards.
- **Custom fan curve** — temperature‑based PWM interpolation with hysteresis
  and an emergency thermal throttle override.
- **Flicker‑free** — OD changes are applied in `auto` mode, then locked with
  `force_level=high` last. The monitor never sees a frequency transition.
- **CPU / sleep** — delegates to CachyOS's `game-performance` for performance
  governor and suspend inhibition.
- **Restore defaults** — returns GPU OverDrive to VBIOS defaults, force level
  to `auto`, profile to `BOOTUP_DEFAULT`, power cap to the driver default and
  fan control to automatic on exit (SIGINT/SIGTERM/SIGQUIT included). Monitor
  state is snapshot and restored separately.
- **Configurable** — environment variables, config file, or both. Env always
  overrides file.
- **Steam‑friendly** — drops into `%command%` and auto‑exports common Proton
  and MangoHud defaults without overriding user‑set values.

## Requirements

- Linux with an **AMD GPU** (RDNA 1/2, legacy GCN/Vega)
- `g++` (GCC 12+), `cmake`, `make` (build only)
- `cachyos-settings` (provides CachyOS's `game-performance` wrapper)
- `ddcutil` — optional, needed only for monitor presets
- `sudo` — required for the privileged helper

## Install

```bash
git clone https://github.com/caesar96/game-session.git
cd game-session
./install.sh
```

Builds a pacman package and installs it via `sudo pacman -U`. Binaries go to
`/usr/bin/`, sudoers to `/etc/sudoers.d/game-session` (permissions `0440`,
grants NOPASSWD to the `wheel` group).

### Uninstall

```bash
cd game-session
./uninstall.sh
```

### Verify

```bash
game-session echo "it works"
game-session dump     # diagnostic — no side effects
```

---

## Configuration

### Environment variables

| Variable | Default | Description |
|---|---|---|
| `GS_GPU_FORCE_LEVEL` | `high` | DPM force level: `auto`, `low`, `high`, `manual` |
| `GS_GPU_PROFILE` | `1` | Power profile index (`0` = BOOTUP_DEFAULT, `1` = 3D_FULL_SCREEN, …) |
| `GS_GPU_POWER_CAP` | `120000000` | Power limit in microwatts (`120000000` = 120 W) |
| `GS_GPU_MIN_CLOCK` | — | GPU core min frequency (MHz). RDNA2 driver ignores point 0. |
| `GS_GPU_MAX_CLOCK` | — | GPU core max frequency (MHz) |
| `GS_GPU_MEMORY_CLOCK` | — | VRAM max frequency (MHz) |
| `GS_GPU_VOLTAGE_OFFSET` | — | Voltage offset in mV (negative = undervolt) |
| `GS_FAN_ENABLED` | `false` | Enable software fan controller (`true` / `1`) |
| `GS_FAN_START` | `50` | Temperature (°C) where the fan starts spinning |
| `GS_FAN_INTERVAL_MS` | `250` | Fan control loop interval (ms) |
| `GS_FAN_HYSTERESIS` | `2` | °C change needed before recalculating PWM |
| `GS_FAN_EMERGENCY_TEMP` | `85` | Above this → fan forced to 100 % |
| `GS_FAN_CURVE` | — | Fan curve: comma‑separated `temp:pwm` pairs |
| `GS_MONITOR_PRESET` | — | Monitor picture preset: `FPS`, `RTS`, `Gamer 1`, `Gamer 2`, `Vivid`, `Reader`, `HDR Effect`. `MONITOR_PRESET` remains supported as an alias. |
| `MONITOR_MATCH` | `GSM` | String to match in `ddcutil detect --brief` output |
| `GS_HDR` | `false` | Enable HDR via kscreen-doctor (`true` / `1` = enable, `0` = skip) |
| `GS_VRR` | `false` | Use automatic VRR during the session and restore the previous policy afterwards |
| `GS_HDR_OUTPUT` | `DP-1` | Display output name for HDR and VRR (e.g. `DP-1`, `HDMI-A-1`) |
| `GS_HELPER` | auto‑detected | Override path to `game-session-helper` binary |

### Config file

`~/.config/game-session/game-session.conf` — auto‑created on first run.

```ini
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
hdr = false
vrr = false
hdr_output = DP-1

[fan]
enabled = true
start = 50
interval_ms = 250
hysteresis = 2
emergency_temp = 85
curve = 40:60,50:100,60:170,65:220,70:255
```

Env vars override config file; Steam launch options override both.

### Default Steam environment variables

Auto‑exported **only if not already set**:

| Variable | Value |
|---|---|
| `PROTON_FSR4_UPGRADE` | `1` |
| `ENABLE_LAYER_MESA_ANTI_LAG` | `1` |
| `PROTON_ENABLE_WAYLAND` | `1` |
| `PROTON_ENABLE_HDR` | `1` |
| `MANGOHUD` | `1` |
| `MANGOHUD_CONFIG` | `cpu_temp,gpu_temp,cpu_stats,fps,frame_timing` |

---

## Usage

### Steam

```
game-session %command%
```

Override any variable:

```
GS_GPU_MAX_CLOCK=2800 GS_GPU_MEMORY_CLOCK=975 game-session %command%
```

### Command line

```bash
game-session mangohud game-performance lutris battle.net
game-session steam steam://rungameid/12345
game-session ./mygame
game-session dump
```

### Helper actions (called internally via sudo)

| Action | Value | Sysfs target |
|---|---|---|
| `force-level` | `auto` / `low` / `high` / `manual` | `power_dpm_force_performance_level` |
| `profile` | 0‑6 | `pp_power_profile_mode` |
| `power-cap` | microwatts | `power1_cap` (hwmon) |
| `od-sclk-min` | MHz | `s 0` → `pp_od_clk_voltage` |
| `od-sclk-max` | MHz | `s 1` → `pp_od_clk_voltage` |
| `od-mclk-min` | MHz | `m 0` → `pp_od_clk_voltage` |
| `od-mclk-max` | MHz | `m 1` → `pp_od_clk_voltage` |
| `od-voltage` | mV (signed) | `vo` → `pp_od_clk_voltage` |
| `od-commit` | (none) | `c` → `pp_od_clk_voltage` |
| `od-reset` | (none) | `r` + `c` → `pp_od_clk_voltage` |
| `fan-enable` | `1` (manual) / `2` (auto) | `pwm1_enable` (hwmon) |
| `fan-pwm` | 0‑255 | `pwm1` (hwmon) |

---

## How it works

```
game-session ./mygame
  │
  ├─ load_config()            ← config file
  ├─ apply_default_env()      ← Proton / MangoHud defaults
  │
  ├─ save_hdr_state()         ← remembers the HDR state before the session
  ├─ save_vrr_policy()        ← remembers Never / Always / Automatic
  ├─ disable HDR temporarily  ← waits until DDC/CI is available
  ├─ save_monitor_state()     ← ddcutil getvcp
  │
  ├─ apply_gpu()
  │   └─ profile              ← sudo helper profile 1
  │   └─ power-cap            ← sudo helper power-cap 120000000
  │   └─ OD (sclk, mclk, vo) ← sudo helper od-* + od-commit
  │   └─ force-level          ← sudo helper force-level high    ← LAST
  │
  ├─ apply_monitor()          ← ddcutil setvcp
  ├─ apply VRR Automatic      ← only while the game is running
  ├─ apply/restore HDR        ← game setting or original desktop state
  ├─ start fan thread
  │
  ├─ fork + exec game-performance ./mygame
  ├─ waitpid
  │
  ├─ stop fan thread
  ├─ disable HDR temporarily  ← waits until DDC/CI is available
  ├─ restore_monitor()
  ├─ restore_hdr()            ← restores the HDR state from before the session
  ├─ restore VRR              ← restores the previous adaptive sync policy
  ├─ restore_gpu_defaults()   ← od-reset, then auto/profile 0/default cap/fan auto
  └─ rm -rf /tmp/game-session-XXXXX
```

OD changes are applied **before** `force_level=high` to avoid flickering.

---

## Monitor presets

Tested on LG UltraGear. Presets are defined in code and set 4 VCP codes:

| Preset | Picture (0x15) | Response (0xF7) | Black Stab (0xF9) | Color (0x14) |
|---|---|---|---|---|
| FPS | 30 | 1 | 70 | 11 |
| RTS | 31 | 2 | 55 | 11 |
| Gamer 1 | 45 | 1 | 0 | 9 |
| Gamer 2 | 46 | 2 | 0 | 5 |
| Vivid | 49 | 2 | 0 | 11 |
| Reader | 1 | 3 | 50 | 11 |
| HDR Effect | 39 | 2 | 50 | 11 |

---

## AMD GPU compatibility

Auto‑detects GPU by scanning `/sys/class/drm/card*/device/vendor` for
`0x1002` and checking for `pp_od_clk_voltage`.

| Interface | Detection | GPUs |
|---|---|---|
| RDNA 2 | `OD_VDDGFX_OFFSET` present | RX 6000 / 7000 series |
| RDNA 1 | `OD_RANGE` present, no offset | RX 5000 series |
| Legacy | `OD_SCLK` present, no range | GCN, Vega |

All OD writes are validated against hardware `OD_RANGE` limits.

---

## Project structure

```
game-session/
├── game-session.cpp            ← orchestrator
├── game-session-helper.cpp     ← privileged helper
├── amdgpu_overdrive.cpp/hpp    ← AMDGPU OD class
├── install.sh / uninstall.sh
├── PKGBUILD                    ← Arch Linux package
├── CMakeLists.txt
├── cmake/uninstall.cmake.in
├── sudo/game-session.in        ← sudoers template
└── README.md
```

## License

MIT
