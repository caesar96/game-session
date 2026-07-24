#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>

static const std::string SYSFS_BASE = "/sys/class/drm/card1/device";
static std::string HWMON;

static std::string find_hwmon() {
    for (int i = 0; i < 16; i++) {
        auto d = std::string("/sys/class/drm/card1/device/hwmon/hwmon") + std::to_string(i);
        struct stat st;
        if (stat(d.c_str(), &st) != 0) continue;
        std::ifstream f(d + "/name");
        std::string name;
        f >> name;
        if (name.find("amdgpu") != std::string::npos) return d;
    }
    return {};
}

static bool write_sysfs(const std::string &path, const std::string &val) {
    std::ofstream f(path);
    if (!f.is_open()) {
        std::cerr << "error: cannot write " << path << "\n";
        return false;
    }
    f << val;
    return f.good();
}

static bool is_number(const std::string &v) {
    if (v.empty()) return false;
    for (char c : v) if (c < '0' || c > '9') return false;
    return true;
}

static bool is_signed_number(const std::string &v) {
    if (v.empty()) return false;
    size_t i = (v[0] == '-') ? 1 : 0;
    if (i == v.size()) return false;
    for (; i < v.size(); i++) if (v[i] < '0' || v[i] > '9') return false;
    return true;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "usage: game-session-helper <action> [value]\n"
                  << "  force-level    <auto|low|high|manual>\n"
                  << "  profile        <0-6>\n"
                  << "  power-cap      <uwatts>\n"
                  << "  od-sclk-min    <freq>\n"
                  << "  od-sclk-max    <freq>\n"
                  << "  od-mclk-max    <freq>\n"
                  << "  od-voltage     <offset>\n"
                  << "  od-commit\n"
                  << "  od-reset\n"
                  << "  fan-enable     <1|2>\n"
                  << "  fan-pwm        <0-255>\n";
        return 1;
    }

    HWMON = find_hwmon();

    std::string action = argv[1];
    std::string value  = (argc > 2) ? argv[2] : "";
    auto od = SYSFS_BASE + "/pp_od_clk_voltage";

    if (action == "force-level") {
        if (value != "auto" && value != "low" && value != "high" && value != "manual") {
            std::cerr << "invalid force-level: " << value << "\n";
            return 1;
        }
        return write_sysfs(SYSFS_BASE + "/power_dpm_force_performance_level", value + "\n") ? 0 : 1;
    }

    if (action == "profile") {
        if (!is_number(value)) { std::cerr << "invalid profile\n"; return 1; }
        return write_sysfs(SYSFS_BASE + "/pp_power_profile_mode", value + "\n") ? 0 : 1;
    }

    if (action == "power-cap") {
        if (!is_number(value)) { std::cerr << "invalid power-cap\n"; return 1; }
        if (HWMON.empty()) { std::cerr << "hwmon not found\n"; return 1; }
        return write_sysfs(HWMON + "/power1_cap", value + "\n") ? 0 : 1;
    }

    if (action == "od-sclk-min") {
        if (!is_number(value)) { std::cerr << "invalid sclk min\n"; return 1; }
        return write_sysfs(od, "s 0 " + value + "\n") ? 0 : 1;
    }

    if (action == "od-sclk-max") {
        if (!is_number(value)) { std::cerr << "invalid sclk max\n"; return 1; }
        return write_sysfs(od, "s 1 " + value + "\n") ? 0 : 1;
    }

    if (action == "od-mclk-max") {
        if (!is_number(value)) { std::cerr << "invalid mclk max\n"; return 1; }
        return write_sysfs(od, "m 1 " + value + "\n") ? 0 : 1;
    }

    if (action == "od-voltage") {
        if (!is_signed_number(value)) { std::cerr << "invalid voltage offset\n"; return 1; }
        return write_sysfs(od, "vo " + value + "\n") ? 0 : 1;
    }

    if (action == "od-commit") {
        return write_sysfs(od, "c\n") ? 0 : 1;
    }

    if (action == "od-reset") {
        return write_sysfs(od, "r\n") ? 0 : 1;
    }

    if (action == "fan-enable") {
        if (value != "1" && value != "2") { std::cerr << "invalid fan-enable (1=manual, 2=auto)\n"; return 1; }
        if (HWMON.empty()) { std::cerr << "hwmon not found\n"; return 1; }
        return write_sysfs(HWMON + "/pwm1_enable", value + "\n") ? 0 : 1;
    }

    if (action == "fan-pwm") {
        if (!is_number(value)) { std::cerr << "invalid pwm\n"; return 1; }
        if (HWMON.empty()) { std::cerr << "hwmon not found\n"; return 1; }
        return write_sysfs(HWMON + "/pwm1", value + "\n") ? 0 : 1;
    }

    std::cerr << "unknown action: " << action << "\n";
    return 1;
}
