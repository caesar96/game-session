#pragma once

#include <filesystem>
#include <string>
#include <memory>

namespace fs = std::filesystem;

class AmdgpuOverdrive {
public:
    struct State {
        int sclk_min = 0;
        int sclk_max = 0;
        int mclk_min = 0;
        int mclk_max = 0;
        int vddgfx_offset = 0;
    };

    struct Limits {
        int sclk_min = 0;
        int sclk_max = 0;
        int mclk_min = 0;
        int mclk_max = 0;
        int vddgfx_offset_min = 0;
        int vddgfx_offset_max = 0;
    };

    enum class Interface { none, legacy, rdna1, rdna2 };

    static std::unique_ptr<AmdgpuOverdrive> create();

    bool valid() const { return !od_path_.empty(); }

    State read_state() const;
    Limits read_limits() const;
    Interface interface() const;

    bool set_sclk_min(int mhz);
    bool set_sclk_max(int mhz);
    bool set_mclk_min(int mhz);
    bool set_mclk_max(int mhz);
    bool set_voltage_offset(int mv);
    bool commit();
    bool reset();

    bool sclk_min_valid(int mhz) const;
    bool sclk_max_valid(int mhz) const;
    bool mclk_min_valid(int mhz) const;
    bool mclk_max_valid(int mhz) const;
    bool voltage_offset_valid(int mv) const;

    const fs::path &device_path() const { return device_path_; }
    const fs::path &hwmon_path() const { return hwmon_path_; }

private:
    AmdgpuOverdrive(fs::path dev, fs::path od, fs::path hwmon);

    bool write_od(const std::string &data) const;
    std::string read_od() const;

    static fs::path find_hwmon(const fs::path &device);

    fs::path device_path_;
    fs::path od_path_;
    fs::path hwmon_path_;
};
