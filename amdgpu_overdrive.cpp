#include "amdgpu_overdrive.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <vector>

// ── helpers ──────────────────────────────────────────────────────────────────

static std::string trim(const std::string &s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string read_file_trim(const std::string &path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    std::string v;
    f >> v;
    return v;
}

static int parse_mhz(const std::string &s) {
    std::string val;
    for (char c : s) {
        if (c >= '0' && c <= '9') val += c;
        else if (c == '-') val += c;
    }
    return val.empty() ? 0 : std::stoi(val);
}

// ── construct / detect ───────────────────────────────────────────────────────

AmdgpuOverdrive::AmdgpuOverdrive(fs::path dev, fs::path od, fs::path hwmon)
    : device_path_(std::move(dev))
    , od_path_(std::move(od))
    , hwmon_path_(std::move(hwmon)) {}

std::unique_ptr<AmdgpuOverdrive> AmdgpuOverdrive::create() {
    for (int i = 0; i < 16; i++) {
        auto dev = fs::path("/sys/class/drm") / ("card" + std::to_string(i)) / "device";
        auto od  = dev / "pp_od_clk_voltage";

        struct stat st;
        if (stat(od.c_str(), &st) != 0) continue;

        auto vendor = read_file_trim(dev / "vendor");
        if (vendor != "0x1002") continue;

        auto hwmon = find_hwmon(dev);
        return std::unique_ptr<AmdgpuOverdrive>(
            new AmdgpuOverdrive(std::move(dev), std::move(od), std::move(hwmon)));
    }
    return nullptr;
}

fs::path AmdgpuOverdrive::find_hwmon(const fs::path &device) {
    for (int i = 0; i < 16; i++) {
        auto d = device / "hwmon" / ("hwmon" + std::to_string(i));
        struct stat st;
        if (stat(d.c_str(), &st) != 0) continue;
        auto name = read_file_trim(d / "name");
        if (name.find("amdgpu") != std::string::npos) return d;
    }
    return {};
}

// ── I/O ──────────────────────────────────────────────────────────────────────

bool AmdgpuOverdrive::write_od(const std::string &data) const {
    if (od_path_.empty()) return false;
    std::ofstream f(od_path_);
    if (!f.is_open()) {
        std::cerr << "error: cannot open " << od_path_ << "\n";
        return false;
    }
    f << data;
    if (!f.good()) {
        std::cerr << "error: write failed " << od_path_ << "\n";
        return false;
    }
    return true;
}

std::string AmdgpuOverdrive::read_od() const {
    if (od_path_.empty()) return {};
    std::ifstream f(od_path_);
    if (!f.is_open()) return {};
    std::string result, line;
    while (std::getline(f, line)) result += line + "\n";
    return result;
}

// ── interface detection ──────────────────────────────────────────────────────

AmdgpuOverdrive::Interface AmdgpuOverdrive::interface() const {
    auto content = read_od();
    if (content.find("OD_VDDGFX_OFFSET") != std::string::npos) return Interface::rdna2;
    if (content.find("OD_RANGE") != std::string::npos)         return Interface::rdna1;
    if (content.find("OD_SCLK") != std::string::npos)          return Interface::legacy;
    return Interface::none;
}

// ── parse ────────────────────────────────────────────────────────────────────

AmdgpuOverdrive::State AmdgpuOverdrive::read_state() const {
    State s;
    auto content = read_od();
    std::istringstream ss(content);
    std::string line;
    int sclk_count = 0, mclk_count = 0;

    while (std::getline(ss, line)) {
        line = trim(line);
        if (line.find("OD_SCLK:") != std::string::npos) continue;
        if (line.find("OD_MCLK:") != std::string::npos) continue;
        if (line.find("OD_RANGE:") != std::string::npos) break;
        if (line.find("OD_VDDGFX_OFFSET:") != std::string::npos) continue;

        auto colon = line.find(':');
        if (colon == std::string::npos) continue;

        int idx = 0;
        try { idx = std::stoi(line.substr(0, colon)); } catch (...) { continue; }
        auto val_str = line.substr(colon + 1);
        int val = parse_mhz(val_str);

        if (line.find("mV") != std::string::npos) {
            s.vddgfx_offset = val;
        } else if (sclk_count < 2) {
            if (idx == 0) s.sclk_min = val;
            else if (idx == 1) s.sclk_max = val;
            sclk_count++;
        } else if (mclk_count < 2) {
            if (idx == 0) s.mclk_min = val;
            else if (idx == 1) s.mclk_max = val;
            mclk_count++;
        }
    }
    return s;
}

AmdgpuOverdrive::Limits AmdgpuOverdrive::read_limits() const {
    if (cached_limits_) return *cached_limits_;

    Limits l;
    auto content = read_od();
    std::istringstream ss(content);
    std::string line;

    while (std::getline(ss, line)) {
        line = trim(line);
        if (line.find("OD_RANGE:") != std::string::npos) continue;

        // Parse limit lines from OD_RANGE section: "SCLK:     500Mhz       2900Mhz"
        if (line.find("SCLK:") != std::string::npos && line.find("Mhz") != std::string::npos) {
            std::istringstream ls(line.substr(line.find(':') + 1));
            std::string a, b;
            ls >> a >> b;
            l.sclk_min = parse_mhz(a);
            l.sclk_max = parse_mhz(b);
        } else if (line.find("MCLK:") != std::string::npos && line.find("Mhz") != std::string::npos) {
            std::istringstream ls(line.substr(line.find(':') + 1));
            std::string a, b;
            ls >> a >> b;
            l.mclk_min = parse_mhz(a);
            l.mclk_max = parse_mhz(b);
        } else if (line.find("VDDGFX") != std::string::npos && line.find("mV") != std::string::npos) {
            std::istringstream ls(line.substr(line.find(':') + 1));
            std::string a, b;
            ls >> a >> b;
            l.vddgfx_offset_min = parse_mhz(a);
            l.vddgfx_offset_max = parse_mhz(b);
        }
    }

    cached_limits_ = l;
    return l;
}

// ── apply ────────────────────────────────────────────────────────────────────

bool AmdgpuOverdrive::set_sclk_min(int mhz) {
    if (!sclk_min_valid(mhz)) return false;
    return write_od("s 0 " + std::to_string(mhz) + "\n");
}

bool AmdgpuOverdrive::set_sclk_max(int mhz) {
    if (!sclk_max_valid(mhz)) return false;
    return write_od("s 1 " + std::to_string(mhz) + "\n");
}

bool AmdgpuOverdrive::set_mclk_min(int mhz) {
    if (!mclk_min_valid(mhz)) return false;
    return write_od("m 0 " + std::to_string(mhz) + "\n");
}

bool AmdgpuOverdrive::set_mclk_max(int mhz) {
    if (!mclk_max_valid(mhz)) return false;
    return write_od("m 1 " + std::to_string(mhz) + "\n");
}

bool AmdgpuOverdrive::set_voltage_offset(int mv) {
    if (!voltage_offset_valid(mv)) return false;
    return write_od("vo " + std::to_string(mv) + "\n");
}

bool AmdgpuOverdrive::commit() {
    return write_od("c\n");
}

bool AmdgpuOverdrive::reset() {
    return write_od("r\n");
}

// ── validation ───────────────────────────────────────────────────────────────

bool AmdgpuOverdrive::sclk_min_valid(int mhz) const {
    auto l = read_limits();
    return mhz >= l.sclk_min && mhz <= l.sclk_max;
}

bool AmdgpuOverdrive::sclk_max_valid(int mhz) const {
    auto l = read_limits();
    return mhz >= l.sclk_min && mhz <= l.sclk_max;
}

bool AmdgpuOverdrive::mclk_min_valid(int mhz) const {
    auto l = read_limits();
    return mhz >= l.mclk_min && mhz <= l.mclk_max;
}

bool AmdgpuOverdrive::mclk_max_valid(int mhz) const {
    auto l = read_limits();
    return mhz >= l.mclk_min && mhz <= l.mclk_max;
}

bool AmdgpuOverdrive::voltage_offset_valid(int mv) const {
    auto l = read_limits();
    return mv >= l.vddgfx_offset_min && mv <= l.vddgfx_offset_max;
}
