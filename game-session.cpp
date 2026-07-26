#include "amdgpu_overdrive.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

// ── globals ─────────────────────────────────────────────────────────────────

static std::string state_dir;
static volatile sig_atomic_t g_child_pid = 0;
static volatile sig_atomic_t g_shutdown = 0;
static std::string hwmon_path;

// ── util ────────────────────────────────────────────────────────────────────

static std::string read_file(const std::string &path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    std::string v, line;
    while (std::getline(f, line)) v += line + "\n";
    return v;
}

static std::string read_file_trim(const std::string &path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    std::string v;
    f >> v;
    return v;
}

static bool write_file(const std::string &path, const std::string &val) {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << val;
    return f.good();
}

static std::string exec_capture_argv(const std::string &cmd,
                                      const std::vector<std::string> &args) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return {};

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return {}; }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        int fdnull = open("/dev/null", O_WRONLY);
        if (fdnull >= 0) dup2(fdnull, STDERR_FILENO);

        std::vector<const char *> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(cmd.c_str());
        for (const auto &a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);

        execvp(cmd.c_str(), const_cast<char *const *>(argv.data()));
        _exit(127);
    }

    close(pipefd[1]);
    std::string result;
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
        result.append(buf, n);
    close(pipefd[0]);

    int status;
    waitpid(pid, &status, 0);
    return result;
}

static bool exec_argv(const std::string &cmd,
                      const std::vector<std::string> &args) {
    pid_t pid = fork();
    if (pid < 0) return false;

    if (pid == 0) {
        std::vector<const char *> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(cmd.c_str());
        for (const auto &a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);

        execvp(cmd.c_str(), const_cast<char *const *>(argv.data()));
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int safe_stoi(const std::string &s, int default_val = 0) {
    if (s.empty()) return default_val;
    char *end = nullptr;
    long val = std::strtol(s.c_str(), &end, 10);
    if (*end != '\0') return default_val;
    return static_cast<int>(val);
}

static std::string trim(const std::string &s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string env_or(const char *key, const std::string &def) {
    const char *v = std::getenv(key);
    return v ? std::string(v) : def;
}

static std::string find_hwmon() {
    auto od = AmdgpuOverdrive::create();
    if (od && !od->hwmon_path().empty()) return od->hwmon_path().string();
    return {};
}

static std::string sysfs_base() {
    auto od = AmdgpuOverdrive::create();
    if (od) return od->device_path().string();
    return "/sys/class/drm/card1/device";
}

// ── TOML-like config parser ─────────────────────────────────────────────────

struct Config {
    std::string force_level = "high";
    std::string profile = "1";
    std::string power_cap = "120000000";
    std::string min_clock;
    std::string max_clock;
    std::string memory_clock;
    std::string voltage_offset;
    std::string monitor_preset;
    bool hdr = false;
    std::string hdr_output = "DP-1";
    bool fan_enabled = false;
    int fan_start = 50;
    int fan_interval_ms = 250;
    int fan_hysteresis = 2;
    int fan_emergency_temp = 85;
    std::vector<std::pair<int,int>> fan_curve;
};

static Config g_config;

static void parse_env_config(Config &cfg) {
    if (auto v = env_or("GS_GPU_FORCE_LEVEL", ""); !v.empty()) cfg.force_level = v;
    if (auto v = env_or("GS_GPU_PROFILE", ""); !v.empty()) cfg.profile = v;
    if (auto v = env_or("GS_GPU_POWER_CAP", ""); !v.empty()) cfg.power_cap = v;
    if (auto v = env_or("GS_GPU_MIN_CLOCK", ""); !v.empty()) cfg.min_clock = v;
    if (auto v = env_or("GS_GPU_MAX_CLOCK", ""); !v.empty()) cfg.max_clock = v;
    if (auto v = env_or("GS_GPU_MEMORY_CLOCK", ""); !v.empty()) cfg.memory_clock = v;
    if (auto v = env_or("GS_GPU_VOLTAGE_OFFSET", ""); !v.empty()) cfg.voltage_offset = v;
    if (auto v = env_or("GS_FAN_ENABLED", ""); !v.empty()) cfg.fan_enabled = (v == "true" || v == "1");
    if (auto v = env_or("GS_FAN_START", ""); !v.empty()) cfg.fan_start = safe_stoi(v, 50);
    if (auto v = env_or("GS_FAN_INTERVAL_MS", ""); !v.empty()) cfg.fan_interval_ms = safe_stoi(v, 250);
    if (auto v = env_or("GS_FAN_HYSTERESIS", ""); !v.empty()) cfg.fan_hysteresis = safe_stoi(v, 2);
    if (auto v = env_or("GS_FAN_EMERGENCY_TEMP", ""); !v.empty()) cfg.fan_emergency_temp = safe_stoi(v, 85);
    if (auto v = env_or("GS_FAN_CURVE", ""); !v.empty()) {
        cfg.fan_curve.clear();
        std::istringstream ss(v);
        std::string token;
        while (std::getline(ss, token, ',')) {
            auto col = token.find(':');
            if (col != std::string::npos)
                cfg.fan_curve.push_back({safe_stoi(token.substr(0, col)),
                                          safe_stoi(token.substr(col + 1))});
        }
    }
    if (auto v = env_or("GS_MONITOR_PRESET", ""); !v.empty()) cfg.monitor_preset = v;
    if (auto v = env_or("GS_HDR", ""); !v.empty()) cfg.hdr = (v == "true" || v == "1");
    if (auto v = env_or("GS_HDR_OUTPUT", ""); !v.empty()) cfg.hdr_output = v;
}

static void parse_config_file(Config &cfg, const std::string &path) {
    std::ifstream f(path);
    if (!f.is_open()) return;
    std::string line, section;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
            val = val.substr(1, val.size() - 2);

        std::string skey = section.empty() ? key : section + "." + key;

        if (skey == "gpu.force_level" || key == "GS_GPU_FORCE_LEVEL") cfg.force_level = val;
        else if (skey == "gpu.profile" || key == "GS_GPU_PROFILE") cfg.profile = val;
        else if (skey == "gpu.power_cap" || key == "GS_GPU_POWER_CAP") cfg.power_cap = val;
        else if (skey == "gpu.min_clock" || key == "GS_GPU_MIN_CLOCK") cfg.min_clock = val;
        else if (skey == "gpu.max_clock" || key == "GS_GPU_MAX_CLOCK") cfg.max_clock = val;
        else if (skey == "gpu.memory_clock" || key == "GS_GPU_MEMORY_CLOCK") cfg.memory_clock = val;
        else if (skey == "gpu.voltage_offset" || key == "GS_GPU_VOLTAGE_OFFSET") cfg.voltage_offset = val;
        else if (skey == "fan.enabled" || key == "GS_FAN_ENABLED") cfg.fan_enabled = (val == "true" || val == "1");
        else if (skey == "fan.fan_start" || skey == "fan.start" || key == "GS_FAN_START") cfg.fan_start = safe_stoi(val, 50);
        else if (skey == "fan.interval_ms" || key == "GS_FAN_INTERVAL_MS") cfg.fan_interval_ms = safe_stoi(val, 250);
        else if (skey == "fan.hysteresis" || key == "GS_FAN_HYSTERESIS") cfg.fan_hysteresis = safe_stoi(val, 2);
        else if (skey == "fan.emergency_temp" || key == "GS_FAN_EMERGENCY_TEMP") cfg.fan_emergency_temp = safe_stoi(val, 85);
        else if (skey == "monitor.preset" || key == "MONITOR_PRESET") cfg.monitor_preset = val;
        else if (skey == "monitor.hdr" || key == "GS_HDR") cfg.hdr = (val == "true" || val == "1");
        else if (skey == "monitor.hdr_output" || key == "GS_HDR_OUTPUT") cfg.hdr_output = val;
        else if (skey == "fan.curve" || key == "GS_FAN_CURVE") {
            cfg.fan_curve.clear();
            std::istringstream vs(val);
            std::string tok;
            while (std::getline(vs, tok, ',')) {
                auto col = tok.find(':');
                if (col != std::string::npos)
                    cfg.fan_curve.push_back({safe_stoi(tok.substr(0, col)),
                                              safe_stoi(tok.substr(col + 1))});
            }
        }
    }
}

static std::string config_path() {
    const char *home = std::getenv("HOME");
    if (!home) return {};
    return std::string(home) + "/.config/game-session/game-session.conf";
}

static void ensure_config() {
    auto path = config_path();
    if (path.empty()) return;
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return;
    auto dir = path.substr(0, path.rfind('/'));
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream f(path);
    if (!f.is_open()) return;
    f << "# game-session configuration\n"
         "# Created automatically. Environment variables override these values.\n"
         "#\n"
         "# [gpu]\n"
         "# force_level     = auto | low | high | manual\n"
         "# profile         = 0=BOOTUP, 1=3D_FULL_SCREEN, 2=POWER_SAVING, ...\n"
         "# power_cap       = power limit in microwatts (120000000 = 120 W)\n"
         "# min_clock       = GPU core min frequency (MHz)\n"
         "# max_clock       = GPU core max frequency (MHz)\n"
         "# memory_clock    = VRAM max frequency (MHz)\n"
         "# voltage_offset  = mV (negative = undervolt, e.g. -5)\n"
         "#\n"
         "# [fan]\n"
         "# enabled         = true | false\n"
         "# start           = temperature to start fan curve at\n"
         "# interval_ms     = PWM update interval\n"
         "# hysteresis      = temp change needed before recalculating PWM\n"
          "# emergency_temp  = above this -> 100 %% fan\n"
          "# curve           = temp:pwm,...  (e.g. 40:50,50:58,60:70,65:90,70:100)\n"
         "#\n"
         "# [monitor]\n"
         "# preset          = monitor picture mode: FPS, RTS, Gamer 1, Gamer 2, Vivid, Reader, HDR Effect\n"
         "#                   (requires ddcutil and a compatible monitor)\n"
         "# hdr             = true | false — enable HDR via kscreen-doctor\n"
         "# hdr_output      = DP-1 (or HDMI-A-1, etc.) — output name for HDR\n"
         "\n"
         "[gpu]\n"
         "force_level = high\n"
         "profile = 1\n"
         "power_cap = 120000000\n"
         "# min_clock = 2313\n"
         "# max_clock = 2700\n"
         "# memory_clock = 852\n"
         "# voltage_offset = -5\n"
         "\n"
         "[fan]\n"
         "enabled = false\n"
         "start = 50\n"
         "interval_ms = 250\n"
         "hysteresis = 2\n"
         "emergency_temp = 85\n"
         "# curve = 40:50,50:58,60:70,65:90,70:100\n";
}

static void load_config() {
    ensure_config();
    parse_config_file(g_config, config_path());
    parse_env_config(g_config);
}

// ── helper (sudo) ───────────────────────────────────────────────────────────

static std::string helper_path() {
    const char *env = std::getenv("GS_HELPER");
    if (env) return env;
    struct stat st;
    if (stat("/usr/local/bin/game-session-helper", &st) == 0)
        return "/usr/local/bin/game-session-helper";
    const char *home = std::getenv("HOME");
    if (home) {
        auto local = std::string(home) + "/.local/bin/game-session-helper";
        if (stat(local.c_str(), &st) == 0) return local;
    }
    return "game-session-helper";
}

static void helper_write(const std::string &action, const std::string &val) {
    auto hp = helper_path();
    std::vector<const char *> args = {"sudo", hp.c_str(), action.c_str()};
    if (!val.empty()) args.push_back(val.c_str());
    args.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) return;

    if (pid == 0) {
        execvp("sudo", const_cast<char *const *>(args.data()));
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);
}

// ── GPU state (non-OD settings only) ────────────────────────────────────────

static std::string detect_od_interface() {
    auto od = AmdgpuOverdrive::create();
    if (!od) return "none";
    switch (od->interface()) {
        case AmdgpuOverdrive::Interface::rdna2: return "rdna2";
        case AmdgpuOverdrive::Interface::rdna1: return "rdna1";
        case AmdgpuOverdrive::Interface::legacy: return "legacy";
        default: return "none";
    }
}

static bool save_gpu_state(const std::string &dir) {
    auto d = dir + "/gpu";
    if (mkdir(d.c_str(), 0755) != 0) {
        if (errno != EEXIST) return false;
    }
    auto base = sysfs_base();
    write_file(d + "/force_level", read_file_trim(base + "/power_dpm_force_performance_level"));
    auto pm = read_file(base + "/pp_power_profile_mode");
    std::string pidx;
    std::istringstream ss(pm);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.find("*:") != std::string::npos) {
            std::istringstream(line) >> pidx;
            break;
        }
    }
    if (pidx.empty()) {
        std::istringstream ss2(pm);
        while (std::getline(ss2, line)) {
            int v;
            if (std::istringstream(line) >> v) { pidx = std::to_string(v); break; }
        }
    }
    write_file(d + "/profile_index", pidx);
    if (!hwmon_path.empty()) {
        write_file(d + "/power_cap", read_file_trim(hwmon_path + "/power1_cap"));
        write_file(d + "/pwm1_enable", read_file_trim(hwmon_path + "/pwm1_enable"));
    }
    return true;
}

static void apply_gpu() {
    auto base = sysfs_base();
    helper_write("profile", g_config.profile);
    if (!hwmon_path.empty())
        helper_write("power-cap", g_config.power_cap);

    // Apply OverDrive changes first while still in auto
    auto od_iface = detect_od_interface();
    if (od_iface == "rdna2" || od_iface == "rdna1" || od_iface == "legacy") {
        if (!g_config.min_clock.empty())
            helper_write("od-sclk-min", g_config.min_clock);
        if (!g_config.max_clock.empty())
            helper_write("od-sclk-max", g_config.max_clock);
        if (!g_config.memory_clock.empty())
            helper_write("od-mclk-max", g_config.memory_clock);
        if (!g_config.voltage_offset.empty())
            helper_write("od-voltage", g_config.voltage_offset);
        if (!g_config.min_clock.empty() || !g_config.max_clock.empty() ||
            !g_config.memory_clock.empty() || !g_config.voltage_offset.empty()) {
            helper_write("od-commit", "");
        }
    }

    // Lock frequencies last to avoid flickering from OD transitions
    helper_write("force-level", g_config.force_level);
}

static void restore_gpu() {
    auto d = state_dir + "/gpu";
    std::string v;

    v = read_file_trim(d + "/force_level");
    if (!v.empty()) helper_write("force-level", v);

    v = read_file_trim(d + "/profile_index");
    if (!v.empty()) helper_write("profile", v);

    v = read_file_trim(d + "/power_cap");
    if (!v.empty()) helper_write("power-cap", v);

    // Reset OverDrive to VBIOS defaults — no state was saved, just reset
    auto od = AmdgpuOverdrive::create();
    if (od && od->valid()) {
        if (!g_config.min_clock.empty() || !g_config.max_clock.empty() ||
            !g_config.memory_clock.empty() || !g_config.voltage_offset.empty()) {
            helper_write("od-reset", "");
        }
    }
}

// ── monitor ─────────────────────────────────────────────────────────────────

static std::string monitor_find_bus() {
    auto match = env_or("MONITOR_MATCH", "GSM");
    auto out = exec_capture_argv("ddcutil", {"detect", "--brief"});
    std::istringstream ss(out);
    std::string line, bus;
    while (std::getline(ss, line)) {
        auto p = line.find("/dev/i2c-");
        if (p != std::string::npos) {
            bus = line.substr(p + 9);
            auto sp = bus.find_first_of(" \t");
            if (sp != std::string::npos) bus = bus.substr(0, sp);
        }
        p = line.find(match);
        if (p != std::string::npos && !bus.empty()) return bus;
    }
    return {};
}

static std::string monitor_read_vcp(const std::string &bus, const std::string &vcp) {
    auto out = exec_capture_argv("ddcutil",
        {"--permit-unknown-feature", "--bus", bus, "getvcp", vcp});
    auto sl = out.find("sl=");
    if (sl != std::string::npos) {
        auto val = out.substr(sl + 3);
        std::string hex;
        for (char c : val) {
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                (c >= 'A' && c <= 'F') || c == 'x') hex += c;
            else break;
        }
        return hex;
    }
    auto cv = out.find("current value = ");
    if (cv != std::string::npos) {
        auto val = out.substr(cv + 16);
        auto end = val.find_first_of(" \t\n\r");
        if (end != std::string::npos) val = val.substr(0, end);
        return val;
    }
    return {};
}

static void monitor_write_vcp(const std::string &bus, const std::string &vcp,
                               const std::string &val) {
    exec_argv("ddcutil",
        {"--permit-unknown-feature", "--bus", bus, "setvcp", vcp, val});
}

struct Preset {
    std::string dec, rt, bs, color;
};

static Preset get_preset(const std::string &name) {
    if (name == "FPS")   return {"30", "1", "70", "11"};
    if (name == "RTS")   return {"31", "2", "55", "11"};
    if (name == "Gamer 1") return {"45", "1", "0",  "9"};
    if (name == "Gamer 2") return {"46", "2", "0",  "5"};
    if (name == "Vivid") return {"49", "2", "0",  "11"};
    if (name == "Reader") return {"1",  "3", "50", "11"};
    if (name == "HDR Effect") return {"39", "2", "50", "11"};
    std::cerr << "game-session: unknown preset '" << name << "', using RTS\n";
    return {"31", "2", "55", "11"};
}

static bool monitor_enabled() {
    if (!g_config.monitor_preset.empty()) return true;
    if (auto e = std::getenv("MONITOR_PRESET")) {
        g_config.monitor_preset = e;
        return true;
    }
    return false;
}

// ── HDR (kscreen-doctor) ──────────────────────────────────────────────────

static std::string strip_ansi(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && s[i] != 'm') i++;
        } else {
            out += s[i];
        }
    }
    return out;
}

static std::string hdr_current_state(const std::string &output) {
    auto raw = exec_capture_argv("kscreen-doctor", {"-o"});
    auto out = strip_ansi(raw);
    // locate the output section and read HDR line
    auto pos = out.find("Output: ");
    while (pos != std::string::npos) {
        auto nl = out.find('\n', pos);
        if (nl == std::string::npos) break;
        auto line = out.substr(pos, nl - pos);
        if (line.find(output) != std::string::npos) {
            auto section_end = out.find("Output: ", nl + 1);
            auto sec = (section_end == std::string::npos)
                ? out.substr(nl + 1)
                : out.substr(nl + 1, section_end - nl - 1);
            auto hpos = sec.find("\n\tHDR: ");
            if (hpos != std::string::npos) {
                auto val_start = hpos + 7;
                auto val_end = sec.find('\n', val_start);
                return trim(sec.substr(val_start, val_end - val_start));
            }
        }
        pos = out.find("Output: ", nl + 1);
    }
    return {};
}

static void save_hdr_state(const std::string &dir) {
    if (!g_config.hdr) return;
    auto d = dir + "/monitor";
    mkdir(d.c_str(), 0755);
    auto state = hdr_current_state(g_config.hdr_output);
    if (!state.empty())
        write_file(d + "/hdr_state", state);
}

static void apply_hdr() {
    if (!g_config.hdr) return;
    exec_argv("kscreen-doctor",
              {"output." + g_config.hdr_output + ".hdr.enable"});
}

static void restore_hdr() {
    auto d = state_dir + "/monitor";
    auto state = read_file_trim(d + "/hdr_state");
    if (state.empty()) return;
    if (state == "disabled")
        exec_argv("kscreen-doctor",
                  {"output." + g_config.hdr_output + ".hdr.disable"});
    // if it was enabled, leave it enabled
}

// ── monitor DDC/CI ────────────────────────────────────────────────────────

static void save_monitor_state(const std::string &dir) {
    if (!monitor_enabled()) return;
    auto bus = monitor_find_bus();
    if (bus.empty()) { std::cerr << "game-session: monitor not found, skipping\n"; return; }
    auto d = dir + "/monitor";
    mkdir(d.c_str(), 0755);
    write_file(d + "/bus", bus);
    write_file(d + "/picture_mode",     monitor_read_vcp(bus, "15"));
    write_file(d + "/response_time",    monitor_read_vcp(bus, "F7"));
    write_file(d + "/black_stabilizer", monitor_read_vcp(bus, "F9"));
    write_file(d + "/color_preset",     monitor_read_vcp(bus, "14"));
}

static void apply_monitor() {
    if (g_config.monitor_preset.empty()) return;
    auto bus = read_file_trim(state_dir + "/monitor/bus");
    if (bus.empty()) return;
    auto p = get_preset(g_config.monitor_preset);
    monitor_write_vcp(bus, "15", p.dec);
    monitor_write_vcp(bus, "F7", p.rt);
    monitor_write_vcp(bus, "F9", p.bs);
    monitor_write_vcp(bus, "14", p.color);
}

static void restore_monitor() {
    auto d = state_dir + "/monitor";
    auto bus = read_file_trim(d + "/bus");
    if (bus.empty()) return;
    std::string v;
    v = read_file_trim(d + "/picture_mode");     if (!v.empty()) monitor_write_vcp(bus, "15", v);
    v = read_file_trim(d + "/response_time");    if (!v.empty()) monitor_write_vcp(bus, "F7", v);
    v = read_file_trim(d + "/black_stabilizer"); if (!v.empty()) monitor_write_vcp(bus, "F9", v);
    v = read_file_trim(d + "/color_preset");     if (!v.empty()) monitor_write_vcp(bus, "14", v);
}

// ── fan controller ─────────────────────────────────────────────────────────

struct FanPoint { int temp; int pwm; };

static int interpolate_pwm(int temp, const std::vector<FanPoint> &curve, int fan_start) {
    if (temp <= fan_start) return 0;
    if (curve.empty()) return 0;
    if (temp <= curve.front().temp) return curve.front().pwm;
    if (temp >= curve.back().temp) return curve.back().pwm;
    for (size_t i = 1; i < curve.size(); i++) {
        if (temp <= curve[i].temp) {
            auto &lo = curve[i-1];
            auto &hi = curve[i];
            double t = double(temp - lo.temp) / double(hi.temp - lo.temp);
            return (int)std::round(lo.pwm + t * (hi.pwm - lo.pwm));
        }
    }
    return curve.back().pwm;
}

static void fan_loop(std::atomic<bool> &running, const Config &cfg) {
    if (hwmon_path.empty() || !cfg.fan_enabled) return;

    helper_write("fan-enable", "1");

    std::vector<FanPoint> curve;
    for (auto &p : cfg.fan_curve) curve.push_back({p.first, p.second});

    int last_pwm = -1;
    int last_temp = -999;

    while (running) {
        auto temp_str = read_file_trim(hwmon_path + "/temp1_input");
        int temp = 0;
        if (temp_str.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(cfg.fan_interval_ms)); continue; }
        temp = safe_stoi(temp_str) / 1000;

        int pwm = 0;
        if (temp >= cfg.fan_emergency_temp) {
            pwm = 255;
        } else if (std::abs(temp - last_temp) >= cfg.fan_hysteresis) {
            pwm = interpolate_pwm(temp, curve, cfg.fan_start);
            last_temp = temp;
        } else if (last_pwm >= 0) {
            pwm = last_pwm;
        }

        if (pwm != last_pwm && pwm > 0) {
            helper_write("fan-pwm", std::to_string(pwm));
            last_pwm = pwm;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(cfg.fan_interval_ms));
    }

    helper_write("fan-enable", "2");
}

// ── restore / cleanup ──────────────────────────────────────────────────────

static void restore_fan() {
    auto d = state_dir + "/gpu";
    auto v = read_file_trim(d + "/pwm1_enable");
    if (!v.empty())
        helper_write("fan-enable", v);
}

static void cleanup() {
    restore_monitor();
    restore_hdr();
    restore_gpu();
    restore_fan();
    if (!state_dir.empty()) {
        std::error_code ec;
        fs::remove_all(state_dir, ec);
    }
}

// ── signal ──────────────────────────────────────────────────────────────────

static void handle_signal(int sig) {
    g_shutdown = 1;
    pid_t pid = g_child_pid;
    if (pid > 0) kill(pid, sig);
}

// ── defaults ────────────────────────────────────────────────────────────────

static void set_default_env(const char *k, const char *v) {
    if (!std::getenv(k)) setenv(k, v, 0);
}

static void apply_default_env() {
    set_default_env("PROTON_FSR4_UPGRADE",        "1");
    set_default_env("ENABLE_LAYER_MESA_ANTI_LAG", "1");
    set_default_env("PROTON_ENABLE_WAYLAND",      "1");
    set_default_env("PROTON_ENABLE_HDR",          "1");
    set_default_env("MANGOHUD",                   "1");
    set_default_env("MANGOHUD_CONFIG",            "cpu_temp,gpu_temp,cpu_stats,fps,frame_timing");
}

// ── dump ────────────────────────────────────────────────────────────────────

static void cmd_dump() {
    auto od = AmdgpuOverdrive::create();
    if (!od) {
        std::cerr << "AMD GPU not found\n";
        return;
    }

    auto base = od->device_path().string();
    hwmon_path = od->hwmon_path().string();

    std::cout << "GPU\n";
    std::cout << "  device:   " << base << "\n";
    std::cout << "  hwmon:    " << (hwmon_path.empty() ? "not found" : hwmon_path) << "\n";
    std::cout << "  OD interface: " << detect_od_interface() << "\n";
    std::cout << "  force_level:  " << read_file_trim(base + "/power_dpm_force_performance_level") << "\n";
    std::cout << "  profile:\n";
    auto pm = read_file(base + "/pp_power_profile_mode");
    std::istringstream pms(pm);
    std::string line;
    while (std::getline(pms, line)) {
        if (line.find("*:") != std::string::npos)
            std::cout << "    (active) " << trim(line) << "\n";
    }

    if (!hwmon_path.empty()) {
        auto cap_str = read_file_trim(hwmon_path + "/power1_cap");
        double cap_w = 0;
        try { cap_w = std::stod(cap_str) / 1000000; } catch (...) {}
        std::cout << "  power_cap:  " << cap_str << " uW (" << cap_w << " W)\n";
        std::cout << "  temp:       " << read_file_trim(hwmon_path + "/temp1_input") << " millideg\n";
    }

    if (od->valid()) {
        std::cout << "\npp_od_clk_voltage:\n";
        auto s = od->read_state();
        auto l = od->read_limits();
        std::cout << "OD_SCLK:\n";
        std::cout << "0: " << s.sclk_min << "Mhz\n";
        std::cout << "1: " << s.sclk_max << "Mhz\n";
        std::cout << "OD_MCLK:\n";
        std::cout << "0: " << s.mclk_min << "Mhz\n";
        std::cout << "1: " << s.mclk_max << "MHz\n";
        std::cout << "OD_RANGE:\n";
        std::cout << "SCLK:     " << l.sclk_min << "Mhz       " << l.sclk_max << "Mhz\n";
        std::cout << "MCLK:     " << l.mclk_min << "Mhz        " << l.mclk_max << "Mhz\n";
        std::cout << "OD_VDDGFX_OFFSET:\n";
        std::cout << s.vddgfx_offset << "mV\n";
    }

    if (!hwmon_path.empty()) {
        std::cout << "\nFan\n";
        std::cout << "  mode:       " << read_file_trim(hwmon_path + "/pwm1_enable")
                  << " (1=manual, 2=auto)\n";
        std::cout << "  pwm:        " << read_file_trim(hwmon_path + "/pwm1") << "\n";
        std::cout << "  rpm:        " << read_file_trim(hwmon_path + "/fan1_input") << "\n";
    }

    std::cout << "\nConfig\n";
    std::cout << "  force_level      = " << g_config.force_level << "\n";
    std::cout << "  profile          = " << g_config.profile << "\n";
    std::cout << "  power_cap        = " << g_config.power_cap << "\n";
    std::cout << "  min_clock        = " << (g_config.min_clock.empty() ? "(not set)" : g_config.min_clock) << "\n";
    std::cout << "  max_clock        = " << (g_config.max_clock.empty() ? "(not set)" : g_config.max_clock) << "\n";
    std::cout << "  memory_clock     = " << (g_config.memory_clock.empty() ? "(not set)" : g_config.memory_clock) << "\n";
    std::cout << "  voltage_offset   = " << (g_config.voltage_offset.empty() ? "(not set)" : g_config.voltage_offset) << "\n";
    std::cout << "  monitor_preset   = " << (g_config.monitor_preset.empty() ? "(not set)" : g_config.monitor_preset) << "\n";
    std::cout << "  hdr              = " << (g_config.hdr ? "true" : "false") << "\n";
    std::cout << "  hdr_output       = " << g_config.hdr_output << "\n";
    std::cout << "  fan_enabled      = " << (g_config.fan_enabled ? "true" : "false") << "\n";
    std::cout << "  fan_curve        = ";
    for (auto &p : g_config.fan_curve)
        std::cout << p.first << ":" << p.second << " ";
    std::cout << "\n";

    std::cout << "\nHelper\n";
    std::cout << "  path: " << helper_path() << "\n";
}

// ── entry ──────────────────────────────────────────────────────────────────

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "usage: game-session <command> [args...]\n"
                  << "       game-session dump\n"
                  << "  game-session steam steam://rungameid/12345\n"
                  << "  game-session ./mygame\n";
        return 1;
    }

    hwmon_path = find_hwmon();
    load_config();
    apply_default_env();

    if (std::string(argv[1]) == "dump") {
        cmd_dump();
        return 0;
    }

    char buf[64];
    std::snprintf(buf, sizeof(buf), "/tmp/game-session-XXXXXX");
    if (!mkdtemp(buf)) {
        std::cerr << "failed to create temp dir\n";
        return 1;
    }
    state_dir = buf;

    save_gpu_state(state_dir);
    save_monitor_state(state_dir);
    save_hdr_state(state_dir);
    apply_gpu();
    apply_monitor();
    apply_hdr();

    std::vector<const char *> cmd_vec = {"game-performance"};
    for (int i = 1; i < argc; i++) cmd_vec.push_back(argv[i]);
    cmd_vec.push_back(nullptr);

    g_child_pid = fork();
    if (g_child_pid < 0) {
        std::cerr << "fork failed\n";
        cleanup();
        return 1;
    }

    if (g_child_pid == 0) {
        execvp("game-performance", const_cast<char *const *>(cmd_vec.data()));
        std::cerr << "failed to execute game-performance: " << strerror(errno) << "\n";
        _exit(127);
    }

    // Install signal handlers only after fork so g_child_pid is always valid
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGQUIT, handle_signal);

    std::atomic<bool> fan_running{true};
    std::thread fan_thread;
    if (g_config.fan_enabled && !hwmon_path.empty()) {
        fan_thread = std::thread(fan_loop, std::ref(fan_running), std::ref(g_config));
    }

    int status;
    while (waitpid(g_child_pid, &status, 0) < 0 && errno == EINTR) {}
    g_child_pid = 0;

    fan_running = false;
    if (fan_thread.joinable()) fan_thread.join();

    cleanup();
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
