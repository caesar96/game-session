#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static const std::string SYSFS_BASE = "/sys/class/drm/card1/device";
static const std::string HWMON = SYSFS_BASE + "/hwmon/hwmon4";

struct Action {
    std::string path;
    bool (*validate)(const std::string &);
};

static bool is_valid_level(const std::string &v) {
    return v == "auto" || v == "low" || v == "high" || v == "manual";
}

static bool is_valid_number(const std::string &v) {
    if (v.empty()) return false;
    for (char c : v) if (c < '0' || c > '9') return false;
    return true;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        std::cerr << "usage: game-session-helper <action> <value>\n";
        return 1;
    }

    std::string action = argv[1];
    std::string value  = argv[2];

    Action a;
    if (action == "force-level") {
        a = { SYSFS_BASE + "/power_dpm_force_performance_level", is_valid_level };
    } else if (action == "profile") {
        a = { SYSFS_BASE + "/pp_power_profile_mode", is_valid_number };
    } else if (action == "power-cap") {
        a = { HWMON + "/power1_cap", is_valid_number };
    } else {
        std::cerr << "unknown action: " << action << "\n";
        return 1;
    }

    if (!a.validate(value)) {
        std::cerr << "invalid value: " << value << "\n";
        return 1;
    }

    std::ofstream f(a.path);
    if (!f.is_open()) {
        std::cerr << "failed to open " << a.path << "\n";
        return 1;
    }
    f << value << "\n";
    return 0;
}
