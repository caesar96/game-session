#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

// ── globals ─────────────────────────────────────────────────────────────────

static std::string state_dir;
static pid_t child_pid = 0;
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

static std::string exec_capture(const std::string &cmd) {
    std::array<char, 4096> buf{};
    std::string result;
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {};
    while (fgets(buf.data(), buf.size(), pipe)) result += buf.data();
    pclose(pipe);
    return result;
}

static bool exec_cmd(const std::string &cmd) {
    return std::system(cmd.c_str()) == 0;
}

static std::string trim(const std::string &s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static bool starts_with(const std::string &s, const std::string &pfx) {
    return s.size() >= pfx.size() && s.compare(0, pfx.size(), pfx) == 0;
}

static std::string env_or(const char *key, const std::string &def) {
    const char *v = std::getenv(key);
    return v ? std::string(v) : def;
}

static std::string find_hwmon() {
    const char *base = "/sys/class/drm/card1/device/hwmon";
    for (int i = 0; i < 16; i++) {
        std::string d = std::string(base) + "/hwmon" + std::to_string(i);
        std::string n = read_file_trim(d + "/name");
        if (n.find("amdgpu") != std::string::npos) return d;
    }
    return {};
}

static std::string sysfs_base() {
    return "/sys/class/drm/card1/device";
}

// ── TOML-like config parser ─────────────────────────────────────────────────

struct Config {
    // gpu
    std::string force_level = "high";
    std::string profile = "1";
    std::string power_cap = "120000000";
    std::string min_clock;
    std::string max_clock;
    std::string memory_clock;
    std::string voltage_offset;
    // fan
    bool fan_enabled = false;
    int fan_start = 50;
    int fan_interval_ms = 250;
    int fan_hysteresis = 2;
    int fan_emergency_temp = 85;
    std::vector<std::pair<int,int>> fan_curve; // temp:pwm
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
    if (auto v = env_or("GS_FAN_START", ""); !v.empty()) cfg.fan_start = std::stoi(v);
    if (auto v = env_or("GS_FAN_INTERVAL_MS", ""); !v.empty()) cfg.fan_interval_ms = std::stoi(v);
    if (auto v = env_or("GS_FAN_HYSTERESIS", ""); !v.empty()) cfg.fan_hysteresis = std::stoi(v);
    if (auto v = env_or("GS_FAN_EMERGENCY_TEMP", ""); !v.empty()) cfg.fan_emergency_temp = std::stoi(v);
    if (auto v = env_or("GS_FAN_CURVE", ""); !v.empty()) {
        cfg.fan_curve.clear();
        std::istringstream ss(v);
        std::string token;
        while (std::getline(ss, token, ',')) {
            auto col = token.find(':');
            if (col != std::string::npos)
                cfg.fan_curve.push_back({std::stoi(token.substr(0, col)),
                                          std::stoi(token.substr(col + 1))});
        }
    }
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
        else if (skey == "fan.fan_start" || skey == "fan.start" || key == "GS_FAN_START") cfg.fan_start = std::stoi(val);
        else if (skey == "fan.interval_ms" || key == "GS_FAN_INTERVAL_MS") cfg.fan_interval_ms = std::stoi(val);
        else if (skey == "fan.hysteresis" || key == "GS_FAN_HYSTERESIS") cfg.fan_hysteresis = std::stoi(val);
        else if (skey == "fan.emergency_temp" || key == "GS_FAN_EMERGENCY_TEMP") cfg.fan_emergency_temp = std::stoi(val);
        else if (skey == "fan.curve" || key == "GS_FAN_CURVE") {
            cfg.fan_curve.clear();
            std::istringstream vs(val);
            std::string tok;
            while (std::getline(vs, tok, ',')) {
                auto col = tok.find(':');
                if (col != std::string::npos)
                    cfg.fan_curve.push_back({std::stoi(tok.substr(0, col)),
                                              std::stoi(tok.substr(col + 1))});
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
    mkdir(dir.c_str(), 0755);
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
    auto cmd = "sudo " + helper_path() + " " + action + " " + val;
    exec_cmd(cmd);
}

// ── GPU state ───────────────────────────────────────────────────────────────

struct GpuState {
    std::string force_level;
    std::string profile_index;
    std::string power_cap;
    std::string od_full;         // raw pp_od_clk_voltage content
    std::string pwm1_enable;
};

static bool file_exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

static std::string detect_od_interface() {
    auto p = sysfs_base() + "/pp_od_clk_voltage";
    if (!file_exists(p)) return "none";
    auto content = read_file(p);
    if (content.find("OD_VDDGFX_OFFSET") != std::string::npos) return "rdna2";
    if (content.find("OD_RANGE") != std::string::npos) return "rdna1";
    if (content.find("OD_SCLK") != std::string::npos) return "legacy";
    return "unknown";
}

static bool save_gpu_state(const std::string &dir) {
    auto d = dir + "/gpu";
    if (mkdir(d.c_str(), 0755) != 0 && errno != EEXIST) return false;
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
    auto od_path = base + "/pp_od_clk_voltage";
    if (file_exists(od_path)) write_file(d + "/od", read_file(od_path));
    return true;
}

static void apply_gpu() {
    auto base = sysfs_base();
    helper_write("force-level", g_config.force_level);
    helper_write("profile", g_config.profile);
    if (!hwmon_path.empty())
        helper_write("power-cap", g_config.power_cap);

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
        // commit if any OD value was set
        if (!g_config.min_clock.empty() || !g_config.max_clock.empty() ||
            !g_config.memory_clock.empty() || !g_config.voltage_offset.empty()) {
            helper_write("od-commit", "");
        }
    }
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

    // restore OD: reset to kernel defaults (this undoes any overclock)
    auto od_path = sysfs_base() + "/pp_od_clk_voltage";
    if (file_exists(od_path)) {
        // if we applied OD overrides, reset them
        if (!g_config.min_clock.empty() || !g_config.max_clock.empty() ||
            !g_config.memory_clock.empty() || !g_config.voltage_offset.empty()) {
            helper_write("od-reset", "");
        }
    }

}

// ── monitor ─────────────────────────────────────────────────────────────────

static std::string monitor_find_bus() {
    auto match = env_or("MONITOR_MATCH", "GSM");
    auto out = exec_capture("ddcutil detect --brief 2>/dev/null");
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
    auto cmd = "ddcutil --permit-unknown-feature --bus " + bus + " getvcp " + vcp + " 2>/dev/null";
    auto out = exec_capture(cmd);
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
    auto cmd = "ddcutil --permit-unknown-feature --bus " + bus
             + " setvcp " + vcp + " " + val + " 2>/dev/null";
    exec_cmd(cmd);
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

static void save_monitor_state(const std::string &dir) {
    if (!std::getenv("MONITOR_PRESET")) return;
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
    if (!std::getenv("MONITOR_PRESET")) return;
    auto bus = read_file_trim(state_dir + "/monitor/bus");
    if (bus.empty()) return;
    auto pname = std::getenv("MONITOR_PRESET");
    auto p = get_preset(pname);
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

    // enable manual fan mode via helper (needs root)
    helper_write("fan-enable", "1");

    std::vector<FanPoint> curve;
    for (auto &p : cfg.fan_curve) curve.push_back({p.first, p.second});

    int last_pwm = -1;
    int last_temp = -999;

    while (running) {
        auto temp_str = read_file_trim(hwmon_path + "/temp1_input");
        int temp = 0;
        if (temp_str.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(cfg.fan_interval_ms)); continue; }
        temp = std::stoi(temp_str) / 1000;

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

    // restore auto fan mode via helper
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
    restore_gpu();
    restore_fan();
    if (!state_dir.empty())
        exec_cmd("rm -rf " + state_dir);
}

// ── signal ──────────────────────────────────────────────────────────────────

static void handle_signal(int sig) {
    if (child_pid > 0) {
        kill(child_pid, sig);
        int status;
        waitpid(child_pid, &status, 0);
    }
    cleanup();
    _exit(128 + sig);
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
    auto base = sysfs_base();
    hwmon_path = find_hwmon();

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
        std::cout << "  power_cap:  " << read_file_trim(hwmon_path + "/power1_cap") << " uW ("
                  << std::stod(read_file_trim(hwmon_path + "/power1_cap")) / 1000000 << " W)\n";
        std::cout << "  temp:       " << read_file_trim(hwmon_path + "/temp1_input") << " millideg\n";
    }

    auto od_path = base + "/pp_od_clk_voltage";
    if (file_exists(od_path)) {
        std::cout << "\npp_od_clk_voltage:\n";
        std::cout << read_file(od_path);
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

    // dump command
    if (std::string(argv[1]) == "dump") {
        cmd_dump();
        return 0;
    }

    // create temp state dir
    char buf[64];
    std::strcpy(buf, "/tmp/game-session-XXXXXX");
    if (!mkdtemp(buf)) {
        std::cerr << "failed to create temp dir\n";
        return 1;
    }
    state_dir = buf;

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGQUIT, handle_signal);

    save_gpu_state(state_dir);
    save_monitor_state(state_dir);
    apply_gpu();
    apply_monitor();

    // build command: game-performance <user args...>
    std::vector<const char *> cmd_vec = {"game-performance"};
    for (int i = 1; i < argc; i++) cmd_vec.push_back(argv[i]);
    cmd_vec.push_back(nullptr);

    child_pid = fork();
    if (child_pid < 0) {
        std::cerr << "fork failed\n";
        cleanup();
        return 1;
    }

    if (child_pid == 0) {
        execvp("game-performance", const_cast<char *const *>(cmd_vec.data()));
        std::cerr << "failed to execute game-performance: " << strerror(errno) << "\n";
        _exit(127);
    }

    // fan controller thread
    std::atomic<bool> fan_running{true};
    std::thread fan_thread;
    if (g_config.fan_enabled && !hwmon_path.empty()) {
        fan_thread = std::thread(fan_loop, std::ref(fan_running), std::ref(g_config));
    }

    int status;
    waitpid(child_pid, &status, 0);
    child_pid = 0;

    fan_running = false;
    if (fan_thread.joinable()) fan_thread.join();

    cleanup();
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
