#include "amdgpu_overdrive.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <filesystem>
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
static volatile sig_atomic_t g_shutdown_signal = 0;
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

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return {};
    }
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

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }
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
    bool vrr = false;
    std::string hdr_output = "DP-1";
    bool fan_enabled = false;
    int fan_start = 50;
    int fan_interval_ms = 250;
    int fan_hysteresis = 2;
    int fan_emergency_temp = 85;
    std::vector<std::pair<int,int>> fan_curve;
};

static Config g_config;

static std::vector<std::pair<int, int>> parse_fan_curve(const std::string &value) {
    std::vector<std::pair<int, int>> curve;
    std::istringstream stream(value);
    std::string token;
    while (std::getline(stream, token, ',')) {
        const auto colon = token.find(':');
        if (colon == std::string::npos) continue;
        curve.emplace_back(safe_stoi(trim(token.substr(0, colon))),
                           safe_stoi(trim(token.substr(colon + 1))));
    }
    return curve;
}

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
        cfg.fan_curve = parse_fan_curve(v);
    }
    if (auto v = env_or("GS_MONITOR_PRESET", ""); !v.empty()) cfg.monitor_preset = v;
    else if (auto v = env_or("MONITOR_PRESET", ""); !v.empty()) cfg.monitor_preset = v;
    if (auto v = env_or("GS_HDR", ""); !v.empty()) cfg.hdr = (v == "true" || v == "1");
    if (auto v = env_or("GS_VRR", ""); !v.empty()) cfg.vrr = (v == "true" || v == "1");
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
        else if (skey == "monitor.vrr" || key == "GS_VRR") cfg.vrr = (val == "true" || val == "1");
        else if (skey == "monitor.hdr_output" || key == "GS_HDR_OUTPUT") cfg.hdr_output = val;
        else if (skey == "fan.curve" || key == "GS_FAN_CURVE") {
            cfg.fan_curve = parse_fan_curve(val);
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
         "# vrr             = true | false — use automatic VRR during the session\n"
         "# hdr_output      = DP-1 (or HDMI-A-1, etc.) — output name for HDR and VRR\n"
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
         "# curve = 40:50,50:58,60:70,65:90,70:100\n"
         "\n"
         "[monitor]\n"
         "# preset = Reader\n"
         "hdr = false\n"
         "vrr = false\n"
         "hdr_output = DP-1\n";
}

static bool validate_config(Config &cfg) {
    if (cfg.force_level != "auto" && cfg.force_level != "low" &&
        cfg.force_level != "high" && cfg.force_level != "manual") {
        std::cerr << "invalid GS_GPU_FORCE_LEVEL: " << cfg.force_level << "\n";
        return false;
    }
    if (cfg.fan_interval_ms < 50 || cfg.fan_interval_ms > 10000 ||
        cfg.fan_hysteresis < 0 || cfg.fan_start < 0 ||
        cfg.fan_emergency_temp <= cfg.fan_start) {
        std::cerr << "invalid fan timing or temperature configuration\n";
        return false;
    }
    std::sort(cfg.fan_curve.begin(), cfg.fan_curve.end());
    for (size_t i = 0; i < cfg.fan_curve.size(); ++i) {
        const auto [temp, pwm] = cfg.fan_curve[i];
        if (temp < 0 || pwm < 0 || pwm > 255 ||
            (i > 0 && temp == cfg.fan_curve[i - 1].first)) {
            std::cerr << "invalid fan curve; temperatures must be unique and PWM must be 0-255\n";
            return false;
        }
    }
    return true;
}

static bool load_config() {
    ensure_config();
    parse_config_file(g_config, config_path());
    parse_env_config(g_config);
    return validate_config(g_config);
}

// ── helper (sudo) ───────────────────────────────────────────────────────────

static std::string helper_path() {
    const char *env = std::getenv("GS_HELPER");
    if (env) return env;
    struct stat st;
    std::vector<std::string> candidates = {
        "/usr/bin/game-session-helper",
        "/usr/local/bin/game-session-helper",
    };
    const char *home = std::getenv("HOME");
    if (home) {
        candidates.push_back(std::string(home) + "/.local/bin/game-session-helper");
    }
    for (const auto &p : candidates) {
        if (stat(p.c_str(), &st) == 0) return p;
    }
    return "game-session-helper";
}

static bool helper_write(const std::string &action, const std::string &val) {
    auto hp = helper_path();
    std::vector<const char *> args = {"sudo", hp.c_str(), action.c_str()};
    if (!val.empty()) args.push_back(val.c_str());
    args.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) return false;

    if (pid == 0) {
        execvp("sudo", const_cast<char *const *>(args.data()));
        _exit(127);
    }

    int status;
    return waitpid(pid, &status, 0) == pid &&
           WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// ── GPU profile ─────────────────────────────────────────────────────────────

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

static bool apply_gpu() {
    if (!helper_write("profile", g_config.profile)) return false;
    if (!hwmon_path.empty())
        if (!helper_write("power-cap", g_config.power_cap)) return false;

    // Apply OverDrive changes first while still in auto
    auto od_iface = detect_od_interface();
    if (od_iface == "rdna2" || od_iface == "rdna1" || od_iface == "legacy") {
        if (!g_config.min_clock.empty())
            if (!helper_write("od-sclk-min", g_config.min_clock)) return false;
        if (!g_config.max_clock.empty())
            if (!helper_write("od-sclk-max", g_config.max_clock)) return false;
        if (!g_config.memory_clock.empty())
            if (!helper_write("od-mclk-max", g_config.memory_clock)) return false;
        if (!g_config.voltage_offset.empty())
            if (!helper_write("od-voltage", g_config.voltage_offset)) return false;
        if (!g_config.min_clock.empty() || !g_config.max_clock.empty() ||
            !g_config.memory_clock.empty() || !g_config.voltage_offset.empty()) {
            if (!helper_write("od-commit", "")) return false;
        }
    }

    // Lock frequencies last to avoid flickering from OD transitions
    return helper_write("force-level", g_config.force_level);
}

static void restore_gpu_defaults() {
    // Reset OverDrive first. force-level=auto must be written afterwards so the
    // driver is not left locked while it returns to the VBIOS defaults.
    auto od = AmdgpuOverdrive::create();
    if (od && od->valid()) {
        helper_write("od-reset", "");
    }

    helper_write("force-level", "auto");
    helper_write("profile", "0"); // BOOTUP_DEFAULT

    if (!hwmon_path.empty()) {
        const auto default_power_cap = read_file_trim(hwmon_path + "/power1_cap_default");
        if (!default_power_cap.empty()) helper_write("power-cap", default_power_cap);
        helper_write("fan-enable", "2"); // automatic fan control
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
    for (int attempt = 0; attempt < 3; ++attempt) {
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
            if (!hex.empty()) return hex;
        }
        auto cv = out.find("current value = ");
        if (cv != std::string::npos) {
            auto val = out.substr(cv + 16);
            // ddcutil prints decimal or hexadecimal values followed by punctuation.
            size_t end = 0;
            while (end < val.size() &&
                   (std::isdigit(static_cast<unsigned char>(val[end])) ||
                    (val[end] >= 'a' && val[end] <= 'f') ||
                    (val[end] >= 'A' && val[end] <= 'F') ||
                    val[end] == 'x' || val[end] == 'X')) ++end;
            if (end > 0) return val.substr(0, end);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    std::cerr << "game-session: failed to read monitor VCP " << vcp << "\n";
    return {};
}

static bool monitor_write_vcp(const std::string &bus, const std::string &vcp,
                              const std::string &val) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (exec_argv("ddcutil",
                {"--noverify", "--permit-unknown-feature", "--bus", bus,
                 "setvcp", vcp, val}))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    std::cerr << "game-session: failed to set monitor VCP " << vcp << "=" << val << "\n";
    return false;
}

static bool monitor_vcp_equal(const std::string &actual, const std::string &wanted) {
    char *actual_end = nullptr;
    char *wanted_end = nullptr;
    const auto actual_value = std::strtol(actual.c_str(), &actual_end, 0);
    const auto wanted_value = std::strtol(wanted.c_str(), &wanted_end, 0);
    return actual_end != actual.c_str() && *actual_end == '\0' &&
           wanted_end != wanted.c_str() && *wanted_end == '\0' &&
           actual_value == wanted_value;
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
    return !g_config.monitor_preset.empty();
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

enum class HdrState { unknown, disabled, enabled };

static HdrState parse_hdr_state(const std::string &raw, const std::string &output) {
    std::istringstream lines(strip_ansi(raw));
    std::string line;
    bool matching_output = false;

    while (std::getline(lines, line)) {
        auto value = trim(line);
        if (value.rfind("Output:", 0) == 0) {
            std::istringstream header(value);
            std::string label, id, name;
            header >> label >> id >> name;
            matching_output = (name == output);
            continue;
        }
        if (!matching_output || value.rfind("HDR:", 0) != 0) continue;

        value = trim(value.substr(4));
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (value == "enabled") return HdrState::enabled;
        if (value == "disabled" || value == "incapable") return HdrState::disabled;
        return HdrState::unknown;
    }
    return HdrState::unknown;
}

static HdrState hdr_current_state(const std::string &output) {
    return parse_hdr_state(exec_capture_argv("kscreen-doctor", {"-o"}), output);
}

static const char *hdr_state_name(HdrState state) {
    switch (state) {
        case HdrState::disabled: return "disabled";
        case HdrState::enabled: return "enabled";
        default: return "unknown";
    }
}

static HdrState saved_hdr_state() {
    auto state = read_file_trim(state_dir + "/monitor/hdr_state");
    std::transform(state.begin(), state.end(), state.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (state == "enabled") return HdrState::enabled;
    if (state == "disabled") return HdrState::disabled;
    return HdrState::unknown;
}

static bool set_hdr_state(HdrState wanted) {
    if (wanted == HdrState::unknown) return false;
    auto current = hdr_current_state(g_config.hdr_output);
    if (current == wanted) return true;

    const auto action = wanted == HdrState::enabled ? "enable" : "disable";
    if (!exec_argv("kscreen-doctor",
                   {"output." + g_config.hdr_output + ".hdr." + action})) {
        std::cerr << "game-session: failed to " << action << " HDR on "
                  << g_config.hdr_output << "\n";
        return false;
    }

    for (int attempt = 0; attempt < 30; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (hdr_current_state(g_config.hdr_output) == wanted) {
            // The compositor state changes before some monitors accept DDC/CI again.
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            return true;
        }
    }
    std::cerr << "game-session: timed out waiting for HDR to become "
              << hdr_state_name(wanted) << " on " << g_config.hdr_output << "\n";
    return false;
}

static bool save_hdr_state(const std::string &dir) {
    // A monitor preset may need HDR disabled during restoration even when this
    // session did not enable HDR itself.  Save the pre-session state whenever
    // either feature is in use so cleanup can restore the picture mode first.
    if (!g_config.hdr && !monitor_enabled()) return true;
    auto d = dir + "/monitor";
    mkdir(d.c_str(), 0755);
    auto state = hdr_current_state(g_config.hdr_output);
    if (state == HdrState::unknown) {
        std::cerr << "game-session: could not read HDR state for "
                  << g_config.hdr_output << "; display settings were not changed\n";
        return false;
    }
    return write_file(d + "/hdr_state", hdr_state_name(state));
}

enum class VrrPolicy { unknown, never, always, automatic };

static VrrPolicy parse_vrr_policy(const std::string &raw, const std::string &output) {
    std::istringstream lines(strip_ansi(raw));
    std::string line;
    bool matching_output = false;

    while (std::getline(lines, line)) {
        auto value = trim(line);
        if (value.rfind("Output:", 0) == 0) {
            std::istringstream header(value);
            std::string label, id, name;
            header >> label >> id >> name;
            matching_output = (name == output);
            continue;
        }
        if (!matching_output || value.rfind("Vrr:", 0) != 0) continue;

        value = trim(value.substr(4));
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (value == "never") return VrrPolicy::never;
        if (value == "always") return VrrPolicy::always;
        if (value == "automatic") return VrrPolicy::automatic;
        return VrrPolicy::unknown;
    }
    return VrrPolicy::unknown;
}

static VrrPolicy vrr_current_policy(const std::string &output) {
    return parse_vrr_policy(exec_capture_argv("kscreen-doctor", {"-o"}), output);
}

static const char *vrr_policy_name(VrrPolicy policy) {
    switch (policy) {
        case VrrPolicy::never: return "never";
        case VrrPolicy::always: return "always";
        case VrrPolicy::automatic: return "automatic";
        default: return "unknown";
    }
}

static VrrPolicy saved_vrr_policy() {
    auto policy = read_file_trim(state_dir + "/monitor/vrr_policy");
    std::transform(policy.begin(), policy.end(), policy.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (policy == "never") return VrrPolicy::never;
    if (policy == "always") return VrrPolicy::always;
    if (policy == "automatic") return VrrPolicy::automatic;
    return VrrPolicy::unknown;
}

static bool set_vrr_policy(VrrPolicy wanted) {
    if (wanted == VrrPolicy::unknown) return false;
    if (vrr_current_policy(g_config.hdr_output) == wanted) return true;

    const auto policy = vrr_policy_name(wanted);
    if (!exec_argv("kscreen-doctor",
                   {"output." + g_config.hdr_output + ".vrrpolicy." + policy})) {
        std::cerr << "game-session: failed to set VRR " << policy << " on "
                  << g_config.hdr_output << "\n";
        return false;
    }

    for (int attempt = 0; attempt < 30; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (vrr_current_policy(g_config.hdr_output) == wanted) return true;
    }
    std::cerr << "game-session: timed out waiting for VRR " << policy
              << " on " << g_config.hdr_output << "\n";
    return false;
}

static bool save_vrr_policy(const std::string &dir) {
    if (!g_config.vrr) return true;
    auto d = dir + "/monitor";
    mkdir(d.c_str(), 0755);
    const auto policy = vrr_current_policy(g_config.hdr_output);
    if (policy == VrrPolicy::unknown) {
        std::cerr << "game-session: could not read VRR policy for "
                  << g_config.hdr_output << "; display settings were not changed\n";
        return false;
    }
    return write_file(d + "/vrr_policy", vrr_policy_name(policy));
}

// ── monitor DDC/CI ────────────────────────────────────────────────────────

static bool save_monitor_state(const std::string &dir) {
    if (!monitor_enabled()) return true;
    auto bus = monitor_find_bus();
    if (bus.empty()) { std::cerr << "game-session: monitor not found\n"; return false; }
    std::cerr << "game-session: save: found bus=" << bus << "\n";
    const auto pm = monitor_read_vcp(bus, "15");
    if (g_shutdown_signal) return false;
    const auto rt = monitor_read_vcp(bus, "F7");
    if (g_shutdown_signal) return false;
    const auto bs = monitor_read_vcp(bus, "F9");
    if (g_shutdown_signal) return false;
    const auto color = monitor_read_vcp(bus, "14");
    std::cerr << "game-session: save: picture_mode=" << pm << "\n";
    if (pm.empty() || rt.empty() || bs.empty() || color.empty()) {
        std::cerr << "game-session: monitor state is incomplete; preset was not changed\n";
        return false;
    }

    auto d = dir + "/monitor";
    mkdir(d.c_str(), 0755);
    const bool saved =
        write_file(d + "/picture_mode", pm) &&
        write_file(d + "/response_time", rt) &&
        write_file(d + "/black_stabilizer", bs) &&
        write_file(d + "/color_preset", color) &&
        write_file(d + "/bus", bus);
    if (!saved) std::cerr << "game-session: failed to save monitor state\n";
    return saved;
}

static bool apply_monitor() {
    if (g_config.monitor_preset.empty()) return true;
    auto bus = read_file_trim(state_dir + "/monitor/bus");
    if (bus.empty()) { std::cerr << "game-session: apply: bus empty\n"; return false; }
    auto p = get_preset(g_config.monitor_preset);
    std::cerr << "game-session: apply: bus=" << bus << " preset=" << g_config.monitor_preset
              << " 15=" << p.dec << " F7=" << p.rt << " F9=" << p.bs << " 14=" << p.color << "\n";
    bool ok = monitor_write_vcp(bus, "15", p.dec);
    ok = monitor_write_vcp(bus, "F7", p.rt) && ok;
    ok = monitor_write_vcp(bus, "F9", p.bs) && ok;
    ok = monitor_write_vcp(bus, "14", p.color) && ok;
    return ok;
}

static bool restore_monitor() {
    auto d = state_dir + "/monitor";
    auto bus = read_file_trim(d + "/bus");
    if (bus.empty()) return true;
    std::string v;
    v = read_file_trim(d + "/picture_mode");
    std::cerr << "game-session: restore: bus=" << bus << " 15=" << v;
    v = read_file_trim(d + "/response_time");    std::cerr << " F7=" << v;
    v = read_file_trim(d + "/black_stabilizer"); std::cerr << " F9=" << v;
    v = read_file_trim(d + "/color_preset");     std::cerr << " 14=" << v;
    std::cerr << "\n";

    const auto picture_mode = read_file_trim(d + "/picture_mode");
    const auto response_time = read_file_trim(d + "/response_time");
    const auto black_stabilizer = read_file_trim(d + "/black_stabilizer");
    const auto color_preset = read_file_trim(d + "/color_preset");

    for (int attempt = 0; attempt < 3; ++attempt) {
        if (!picture_mode.empty()) monitor_write_vcp(bus, "15", picture_mode);
        if (!response_time.empty()) monitor_write_vcp(bus, "F7", response_time);
        if (!black_stabilizer.empty()) monitor_write_vcp(bus, "F9", black_stabilizer);
        if (!color_preset.empty()) monitor_write_vcp(bus, "14", color_preset);

        // HDR transitions can acknowledge before the monitor finishes changing
        // modes. Verify after it settles and repeat if that transition won.
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const bool restored =
            monitor_vcp_equal(monitor_read_vcp(bus, "15"), picture_mode) &&
            monitor_vcp_equal(monitor_read_vcp(bus, "F7"), response_time) &&
            monitor_vcp_equal(monitor_read_vcp(bus, "F9"), black_stabilizer) &&
            monitor_vcp_equal(monitor_read_vcp(bus, "14"), color_preset);
        if (restored) return true;
        std::cerr << "game-session: monitor restore verification failed; retrying\n";
    }
    return false;
}

static bool apply_display_settings() {
    const auto original_hdr = saved_hdr_state();
    const bool has_monitor_state =
        !read_file_trim(state_dir + "/monitor/bus").empty();

    if (has_monitor_state && !set_hdr_state(HdrState::disabled)) return false;
    if (!apply_monitor()) return false;
    if (g_config.vrr && !set_vrr_policy(VrrPolicy::automatic)) return false;

    const auto game_hdr = g_config.hdr ? HdrState::enabled : original_hdr;
    return game_hdr == HdrState::unknown || set_hdr_state(game_hdr);
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
            return std::clamp(static_cast<int>(std::round(lo.pwm + t * (hi.pwm - lo.pwm))), 0, 255);
        }
    }
    return curve.back().pwm;
}

static void fan_loop(std::atomic<bool> &running, const Config &cfg) {
    if (hwmon_path.empty() || !cfg.fan_enabled) return;

    if (!helper_write("fan-enable", "1")) return;

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

        if (pwm != last_pwm) {
            if (!helper_write("fan-pwm", std::to_string(pwm))) break;
            last_pwm = pwm;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(cfg.fan_interval_ms));
    }

}

// ── restore / cleanup ──────────────────────────────────────────────────────

static bool cleanup() {
    const auto original_hdr = saved_hdr_state();
    const auto original_vrr = saved_vrr_policy();
    const bool has_monitor_state =
        !read_file_trim(state_dir + "/monitor/bus").empty();

    // GPU resets can retrain the display link, so finish them before restoring
    // the monitor. Otherwise the link transition may undo the final VCP write.
    restore_gpu_defaults();

    // Some monitor picture modes cannot be selected while HDR is enabled.
    // Wait for HDR to be disabled, restore DDC/CI, then restore the exact state.
    const bool hdr_disabled =
        !has_monitor_state || set_hdr_state(HdrState::disabled);
    const bool monitor_restored = hdr_disabled && restore_monitor();
    const bool hdr_restored = original_hdr == HdrState::unknown ||
        set_hdr_state(original_hdr);
    const bool vrr_restored = original_vrr == VrrPolicy::unknown ||
        set_vrr_policy(original_vrr);

    const bool display_restored = monitor_restored && hdr_restored && vrr_restored;
    if (display_restored && !state_dir.empty()) {
        std::error_code ec;
        fs::remove_all(state_dir, ec);
    } else if (!display_restored) {
        std::cerr << "game-session: display restoration failed; recovery state kept at "
                  << state_dir << "\n";
    }
    return display_restored;
}

// ── signal ──────────────────────────────────────────────────────────────────

static void handle_signal(int sig) {
    g_shutdown_signal = sig;
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
    std::cout << "  vrr              = " << (g_config.vrr ? "true" : "false") << "\n";
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
    if (!load_config()) return 1;
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

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGQUIT, handle_signal);

    if (!save_hdr_state(state_dir) || !save_vrr_policy(state_dir)) {
        std::error_code ec;
        fs::remove_all(state_dir, ec);
        return 1;
    }
    if (g_shutdown_signal) {
        std::error_code ec;
        fs::remove_all(state_dir, ec);
        return 128 + g_shutdown_signal;
    }
    const auto original_hdr = saved_hdr_state();
    if (monitor_enabled() && !set_hdr_state(HdrState::disabled)) {
        cleanup();
        return 1;
    }
    if (g_shutdown_signal || !save_monitor_state(state_dir)) {
        const bool hdr_restored = original_hdr == HdrState::unknown ||
            set_hdr_state(original_hdr);
        if (hdr_restored) {
            std::error_code ec;
            fs::remove_all(state_dir, ec);
        } else {
            std::cerr << "game-session: HDR restoration failed; recovery state kept at "
                      << state_dir << "\n";
        }
        return g_shutdown_signal ? 128 + g_shutdown_signal : 1;
    }
    if (!apply_gpu()) {
        std::cerr << "failed to apply GPU profile; restoring saved state\n";
        cleanup();
        return 1;
    }
    const bool display_applied = !g_shutdown_signal && apply_display_settings();
    if (g_shutdown_signal || !display_applied) {
        std::cerr << "failed to apply display settings; restoring saved state\n";
        cleanup();
        return g_shutdown_signal ? 128 + g_shutdown_signal : 1;
    }

    std::vector<const char *> cmd_vec = {"game-performance"};
    for (int i = 1; i < argc; i++) cmd_vec.push_back(argv[i]);
    cmd_vec.push_back(nullptr);

    sigset_t blocked_signals, previous_mask;
    sigemptyset(&blocked_signals);
    sigaddset(&blocked_signals, SIGINT);
    sigaddset(&blocked_signals, SIGTERM);
    sigaddset(&blocked_signals, SIGQUIT);
    if (sigprocmask(SIG_BLOCK, &blocked_signals, &previous_mask) < 0) {
        std::cerr << "failed to block signals before fork: " << strerror(errno) << "\n";
        cleanup();
        return 1;
    }
    if (g_shutdown_signal) {
        sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
        cleanup();
        return 128 + g_shutdown_signal;
    }

    g_child_pid = fork();
    if (g_child_pid < 0) {
        sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
        std::cerr << "fork failed\n";
        cleanup();
        return 1;
    }

    if (g_child_pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
        execvp("game-performance", const_cast<char *const *>(cmd_vec.data()));
        std::cerr << "failed to execute game-performance: " << strerror(errno) << "\n";
        _exit(127);
    }
    sigprocmask(SIG_SETMASK, &previous_mask, nullptr);

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

    if (!cleanup()) return 1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
