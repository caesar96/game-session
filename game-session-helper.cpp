#include "amdgpu_overdrive.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <sys/stat.h>

static std::unique_ptr<AmdgpuOverdrive> g_od;
static std::string HWMON;

// ── sysfs I/O ────────────────────────────────────────────────────────────────

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

// ── entry ────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "usage: game-session-helper <action> [value]\n"
                  << "  force-level    <auto|low|high|manual>\n"
                  << "  profile        <0-6>\n"
                  << "  power-cap      <uwatts>\n"
                  << "  od-sclk-min    <freq>\n"
                  << "  od-sclk-max    <freq>\n"
                  << "  od-mclk-min    <freq>\n"
                  << "  od-mclk-max    <freq>\n"
                  << "  od-voltage     <offset>\n"
                  << "  od-commit\n"
                  << "  od-reset\n"
                  << "  fan-enable     <1|2>\n"
                  << "  fan-pwm        <0-255>\n";
        return 1;
    }

    g_od = AmdgpuOverdrive::create();
    if (g_od && !g_od->hwmon_path().empty()) HWMON = g_od->hwmon_path().string();

    std::string action = argv[1];
    std::string value  = (argc > 2) ? argv[2] : "";

    // ── non-OD actions (direct sysfs) ────────────────────────────────────────

    if (action == "force-level") {
        if (value != "auto" && value != "low" && value != "high" && value != "manual") {
            std::cerr << "invalid force-level: " << value << "\n";
            return 1;
        }
        return write_sysfs(g_od->device_path().string() + "/power_dpm_force_performance_level", value + "\n") ? 0 : 1;
    }

    if (action == "profile") {
        if (!is_number(value)) { std::cerr << "invalid profile\n"; return 1; }
        return write_sysfs(g_od->device_path().string() + "/pp_power_profile_mode", value + "\n") ? 0 : 1;
    }

    if (action == "power-cap") {
        if (!is_number(value)) { std::cerr << "invalid power-cap\n"; return 1; }
        if (HWMON.empty()) { std::cerr << "hwmon not found\n"; return 1; }
        return write_sysfs(HWMON + "/power1_cap", value + "\n") ? 0 : 1;
    }

    // ── OverDrive actions ────────────────────────────────────────────────────

    if (!g_od || !g_od->valid()) {
        std::cerr << "AMD GPU not found\n";
        return 1;
    }

    if (action == "od-sclk-min") {
        if (!is_number(value)) { std::cerr << "invalid sclk min\n"; return 1; }
        int v = std::stoi(value);
        if (!g_od->sclk_min_valid(v)) {
            auto l = g_od->read_limits();
            std::cerr << "sclk min out of range [" << l.sclk_min << ", " << l.sclk_max << "]\n";
            return 1;
        }
        return g_od->set_sclk_min(v) ? 0 : 1;
    }

    if (action == "od-sclk-max") {
        if (!is_number(value)) { std::cerr << "invalid sclk max\n"; return 1; }
        int v = std::stoi(value);
        if (!g_od->sclk_max_valid(v)) {
            auto l = g_od->read_limits();
            std::cerr << "sclk max out of range [" << l.sclk_min << ", " << l.sclk_max << "]\n";
            return 1;
        }
        return g_od->set_sclk_max(v) ? 0 : 1;
    }

    if (action == "od-mclk-min") {
        if (!is_number(value)) { std::cerr << "invalid mclk min\n"; return 1; }
        int v = std::stoi(value);
        if (!g_od->mclk_min_valid(v)) {
            auto l = g_od->read_limits();
            std::cerr << "mclk min out of range [" << l.mclk_min << ", " << l.mclk_max << "]\n";
            return 1;
        }
        return g_od->set_mclk_min(v) ? 0 : 1;
    }

    if (action == "od-mclk-max") {
        if (!is_number(value)) { std::cerr << "invalid mclk max\n"; return 1; }
        int v = std::stoi(value);
        if (!g_od->mclk_max_valid(v)) {
            auto l = g_od->read_limits();
            std::cerr << "mclk max out of range [" << l.mclk_min << ", " << l.mclk_max << "]\n";
            return 1;
        }
        return g_od->set_mclk_max(v) ? 0 : 1;
    }

    if (action == "od-voltage") {
        if (!is_signed_number(value)) { std::cerr << "invalid voltage offset\n"; return 1; }
        int v = std::stoi(value);
        if (!g_od->voltage_offset_valid(v)) {
            auto l = g_od->read_limits();
            std::cerr << "voltage offset out of range [" << l.vddgfx_offset_min << ", " << l.vddgfx_offset_max << "]\n";
            return 1;
        }
        return g_od->set_voltage_offset(v) ? 0 : 1;
    }

    if (action == "od-commit") {
        return g_od->commit() ? 0 : 1;
    }

    if (action == "od-reset") {
        bool ok = g_od->reset();
        if (ok) g_od->commit();
        return ok ? 0 : 1;
    }

    // ── fan actions ──────────────────────────────────────────────────────────

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
