#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

// ── state storage ──────────────────────────────────────────────────────────

struct GpuState {
    std::string force_level;
    std::string profile_index;
    std::string power_cap;
};

struct MonitorState {
    std::string bus;
    std::string picture_mode;
    std::string response_time;
    std::string black_stabilizer;
    std::string color_preset;
};

struct Preset {
    std::string dec;  // picture mode
    std::string rt;   // response time
    std::string bs;   // black stabilizer
    std::string color; // color preset
};

static std::string state_dir;
static pid_t child_pid = 0;

// ── helpers ────────────────────────────────────────────────────────────────

static std::string read_file(const std::string &path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    std::string v;
    f >> v;
    return v;
}

static bool write_file(const std::string &path, const std::string &val) {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << val << "\n";
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

// ── config ─────────────────────────────────────────────────────────────────

static std::string env_or(const char *key, const std::string &def) {
    const char *v = std::getenv(key);
    return v ? std::string(v) : def;
}

static void load_config() {
    const char *home = std::getenv("HOME");
    if (!home) return;
    std::string path = std::string(home) + "/.config/game-session/game-session.conf";
    std::ifstream f(path);
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // strip quotes
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
            val = val.substr(1, val.size() - 2);
        setenv(key.c_str(), val.c_str(), 0);
    }
}

// ── monitor via ddcutil ────────────────────────────────────────────────────

static std::string monitor_find_bus() {
    std::string match = env_or("MONITOR_MATCH", "GSM");
    std::string out = exec_capture("ddcutil detect --brief 2>/dev/null");
    std::istringstream ss(out);
    std::string line;
    std::string bus;
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
    std::string cmd = "ddcutil --permit-unknown-feature --bus " + bus
                    + " getvcp " + vcp + " 2>/dev/null";
    std::string out = exec_capture(cmd);
    // match sl=0x... or "current value = <digits>"
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
    std::string cmd = "ddcutil --permit-unknown-feature --bus " + bus
                    + " setvcp " + vcp + " " + val + " 2>/dev/null";
    exec_cmd(cmd);
}

// ── GPU via helper (sudo) ──────────────────────────────────────────────────

static std::string helper_path() {
    const char *home = std::getenv("HOME");
    return std::string(home ? home : "/home/devd")
         + "/.local/bin/game-session-helper";
}

static void gpu_write(const std::string &action, const std::string &val) {
    std::string cmd = "sudo " + helper_path() + " " + action + " " + val;
    exec_cmd(cmd);
}

// ── save / apply / restore ────────────────────────────────────────────────

static bool save_gpu_state(const std::string &dir) {
    std::string d = dir + "/gpu";
    if (mkdir(d.c_str(), 0755) != 0 && errno != EEXIST) return false;
    GpuState s;
    s.force_level   = read_file("/sys/class/drm/card1/device/power_dpm_force_performance_level");
    s.power_cap     = read_file("/sys/class/drm/card1/device/hwmon/hwmon4/power1_cap");
    std::string pm = read_file("/sys/class/drm/card1/device/pp_power_profile_mode");
    std::istringstream ss(pm);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.find("*:") != std::string::npos) {
            std::istringstream ls(line);
            ls >> s.profile_index;
            break;
        }
    }
    if (s.profile_index.empty()) {
        std::istringstream ss2(pm);
        while (std::getline(ss2, line)) {
            int idx;
            if (std::istringstream(line) >> idx) {
                s.profile_index = std::to_string(idx);
                break;
            }
        }
    }

    write_file(d + "/force_level",   s.force_level);
    write_file(d + "/profile_index", s.profile_index);
    write_file(d + "/power_cap",     s.power_cap);
    return true;
}

static bool save_monitor_state(const std::string &dir) {
    std::string bus = monitor_find_bus();
    if (bus.empty()) {
        std::cerr << "game-session: monitor not found, skipping\n";
        return false;
    }
    std::string d = dir + "/monitor";
    if (mkdir(d.c_str(), 0755) != 0 && errno != EEXIST) return false;
    write_file(d + "/bus",              bus);
    write_file(d + "/picture_mode",     monitor_read_vcp(bus, "15"));
    write_file(d + "/response_time",    monitor_read_vcp(bus, "F7"));
    write_file(d + "/black_stabilizer", monitor_read_vcp(bus, "F9"));
    write_file(d + "/color_preset",     monitor_read_vcp(bus, "14"));
    return true;
}

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

static void apply_gpu() {
    std::string level = env_or("GS_GPU_FORCE_LEVEL", "high");
    std::string prof  = env_or("GS_GPU_PROFILE", "1");
    std::string cap   = env_or("GS_GPU_POWER_CAP", "120000000");
    gpu_write("force-level", level);
    gpu_write("profile", prof);
    gpu_write("power-cap", cap);
}

static void apply_monitor() {
    std::string bus = read_file(state_dir + "/monitor/bus");
    if (bus.empty()) return;
    std::string pname = env_or("MONITOR_PRESET", "RTS");
    Preset p = get_preset(pname);
    monitor_write_vcp(bus, "15", p.dec);
    monitor_write_vcp(bus, "F7", p.rt);
    monitor_write_vcp(bus, "F9", p.bs);
    monitor_write_vcp(bus, "14", p.color);
}

static void restore_gpu() {
    std::string d = state_dir + "/gpu";
    std::string v;
    v = read_file(d + "/force_level");   if (!v.empty()) gpu_write("force-level", v);
    v = read_file(d + "/profile_index"); if (!v.empty()) gpu_write("profile", v);
    v = read_file(d + "/power_cap");     if (!v.empty()) gpu_write("power-cap", v);
}

static void restore_monitor() {
    std::string d = state_dir + "/monitor";
    std::string bus = read_file(d + "/bus");
    if (bus.empty()) return;
    std::string v;
    v = read_file(d + "/picture_mode");     if (!v.empty()) monitor_write_vcp(bus, "15", v);
    v = read_file(d + "/response_time");    if (!v.empty()) monitor_write_vcp(bus, "F7", v);
    v = read_file(d + "/black_stabilizer"); if (!v.empty()) monitor_write_vcp(bus, "F9", v);
    v = read_file(d + "/color_preset");     if (!v.empty()) monitor_write_vcp(bus, "14", v);
}

static void cleanup() {
    restore_gpu();
    restore_monitor();
    if (!state_dir.empty())
        exec_cmd("rm -rf " + state_dir);
}

// ── signal handler ─────────────────────────────────────────────────────────

static void handle_signal(int sig) {
    if (child_pid > 0) {
        kill(child_pid, sig);
        int status;
        waitpid(child_pid, &status, 0);
    }
    cleanup();
    _exit(128 + sig);
}

// ── default env vars (don't override if already set) ───────────────────────

static void set_default_env(const char *k, const char *v) {
    if (!std::getenv(k)) setenv(k, v, 0);
}

static void apply_default_env() {
    set_default_env("PROTON_FSR4_UPGRADE",       "1");
    set_default_env("ENABLE_LAYER_MESA_ANTI_LAG","1");
    set_default_env("PROTON_ENABLE_WAYLAND",     "1");
    set_default_env("PROTON_ENABLE_HDR",         "1");
    set_default_env("MANGOHUD",                  "1");
    set_default_env("MANGOHUD_CONFIG",           "cpu_temp,gpu_temp,cpu_stats,fps,frame_timing");
}

// ── entry ──────────────────────────────────────────────────────────────────

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "usage: game-session <command> [args...]\n"
                  << "  game-session steam steam://rungameid/12345\n"
                  << "  game-session mangohud game-performance lutris ...\n";
        return 1;
    }

    load_config();
    apply_default_env();

    // create temp state dir
    const char *tmpl = "/tmp/game-session-XXXXXX";
    char buf[64];
    std::strcpy(buf, tmpl);
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

    int status;
    waitpid(child_pid, &status, 0);
    child_pid = 0;

    cleanup();
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
