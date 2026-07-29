#define main game_session_program_main
#include "../game-session.cpp"
#undef main

#include <filesystem>
#include <stdexcept>

static void expect(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

static void write_test_file(const std::string &path, const std::string &value) {
    std::ofstream file(path);
    file << value;
}

static void write_test_command(const std::string &path, const std::string &body) {
    write_test_file(path, "#!/bin/sh\n" + body);
    chmod(path.c_str(), 0755);
}

static std::vector<std::string> read_test_log(const std::string &path) {
    std::ifstream file(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) lines.push_back(line);
    return lines;
}

static void test_display_session(const std::string &initial_hdr,
                                 bool request_hdr,
                                 const std::vector<std::string> &expected_log,
                                 bool fail_restore = false) {
    char root_template[] = "/tmp/game-session-test-XXXXXX";
    const std::string root = mkdtemp(root_template);
    const auto bin = root + "/bin";
    const auto session = root + "/session";
    const auto monitor = session + "/monitor";
    const auto hdr_state = root + "/current-hdr";
    const auto vrr_state = root + "/current-vrr";
    const auto log = root + "/commands";
    std::filesystem::create_directories(bin);
    std::filesystem::create_directories(monitor);

    write_test_command(bin + "/kscreen-doctor",
        "if [ \"$1\" = \"-o\" ]; then\n"
        "  hdr=$(tr -d '\\n' < \"$GS_TEST_HDR_STATE\")\n"
        "  vrr=$(tr -d '\\n' < \"$GS_TEST_VRR_STATE\")\n"
        "  printf 'Output: 1 DP-1 uuid\\n\\tVrr: %s\\n\\tHDR: %s\\n' \"$vrr\" \"$hdr\"\n"
        "  exit 0\n"
        "fi\n"
        "case \"$1\" in\n"
        "  *.hdr.enable) state=enabled; file=$GS_TEST_HDR_STATE; feature=hdr ;;\n"
        "  *.hdr.disable) state=disabled; file=$GS_TEST_HDR_STATE; feature=hdr ;;\n"
        "  *.vrrpolicy.never) state=never; file=$GS_TEST_VRR_STATE; feature=vrr ;;\n"
        "  *.vrrpolicy.always) state=always; file=$GS_TEST_VRR_STATE; feature=vrr ;;\n"
        "  *.vrrpolicy.automatic) state=automatic; file=$GS_TEST_VRR_STATE; feature=vrr ;;\n"
        "  *) exit 1 ;;\n"
        "esac\n"
        "printf '%s' \"$state\" > \"$file\"\n"
        "printf '%s:%s\\n' \"$feature\" \"$state\" >> \"$GS_TEST_LOG\"\n");
    write_test_command(bin + "/ddcutil",
        "if [ \"$1\" = \"detect\" ]; then\n"
        "  printf 'Display 1\\n   I2C bus: /dev/i2c-6\\n   Monitor: GSM:LG ULTRAGEAR:test\\n'\n"
        "  exit 0\n"
        "fi\n"
        "if [ \"$4\" = \"getvcp\" ]; then\n"
        "  state=$(tr -d '\\n' < \"$GS_TEST_HDR_STATE\")\n"
        "  printf 'read:%s:%s\\n' \"$state\" \"$5\" >> \"$GS_TEST_LOG\"\n"
        "  case \"$5\" in\n"
        "    15) if [ -n \"$GS_TEST_DDC_MISMATCH\" ]; then value=0x1f; else value=0x2e; fi ;;\n"
        "    F7) value=0x02 ;;\n"
        "    F9) value=0x00 ;;\n"
        "    14) value=0x05 ;;\n"
        "    *) exit 1 ;;\n"
        "  esac\n"
        "  printf 'VCP code: mh=0x00, ml=0xff, sh=0x00, sl=%s\\n' \"$value\"\n"
        "  exit 0\n"
        "fi\n"
        "if [ \"$5\" = \"setvcp\" ]; then\n"
        "  printf 'ddc:%s=%s\\n' \"$6\" \"$7\" >> \"$GS_TEST_LOG\"\n"
        "fi\n"
        "exit 0\n");
    write_test_command(bin + "/sudo", "exit 0\n");

    const auto old_path = env_or("PATH", "");
    setenv("PATH", (bin + ":" + old_path).c_str(), 1);
    setenv("GS_TEST_HDR_STATE", hdr_state.c_str(), 1);
    setenv("GS_TEST_VRR_STATE", vrr_state.c_str(), 1);
    setenv("GS_TEST_LOG", log.c_str(), 1);

    write_test_file(hdr_state, initial_hdr);
    write_test_file(vrr_state, "never");
    write_test_file(monitor + "/hdr_state", initial_hdr);
    write_test_file(monitor + "/vrr_policy", "never");

    state_dir = session;
    hwmon_path.clear();
    g_config = Config{};
    g_config.monitor_preset = "RTS";
    g_config.hdr = request_hdr;
    g_config.vrr = true;
    g_config.hdr_output = "DP-1";

    expect(set_hdr_state(HdrState::disabled), "HDR should be disabled before capture");
    expect(save_monitor_state(session), "complete monitor state should be captured");
    expect(apply_display_settings(), "display settings should apply");
    if (fail_restore) setenv("GS_TEST_DDC_MISMATCH", "1", 1);
    expect(cleanup() != fail_restore, "cleanup result should report restoration failures");
    expect(read_file_trim(hdr_state) == initial_hdr, "original HDR state should be restored");
    expect(read_file_trim(vrr_state) == "never", "original VRR policy should be restored");
    if (!fail_restore)
        expect(read_test_log(log) == expected_log, "display operations ran in the wrong order");
    expect(std::filesystem::exists(session) == fail_restore,
           "recovery state should only remain after a restoration failure");

    setenv("PATH", old_path.c_str(), 1);
    unsetenv("GS_TEST_HDR_STATE");
    unsetenv("GS_TEST_VRR_STATE");
    unsetenv("GS_TEST_LOG");
    unsetenv("GS_TEST_DDC_MISMATCH");
    std::filesystem::remove_all(root);
}

int main() {
    const std::string outputs =
        "\x1b[01;32mOutput: \x1b[0;0m1 DP-10 uuid-a\n"
        "\t\x1b[01;33mVrr: \x1b[0;0mNever\n"
        "\t\x1b[01;33mHDR: \x1b[0;0mdisabled\n"
        "\x1b[01;32mOutput: \x1b[0;0m2 DP-1 uuid-b\n"
        "\t\x1b[01;33mVrr: \x1b[0;0mAutomatic\n"
        "\t\x1b[01;33mHDR: \x1b[0;0menabled\n";

    expect(parse_hdr_state(outputs, "DP-1") == HdrState::enabled,
           "DP-1 HDR state should be enabled");
    expect(parse_hdr_state(outputs, "DP-10") == HdrState::disabled,
           "DP-10 HDR state should be disabled");
    expect(parse_hdr_state(outputs, "HDMI-A-1") == HdrState::unknown,
           "missing output should have unknown HDR state");
    expect(parse_hdr_state("Output: 1 HDMI-A-1 uuid\n\tHDR: incapable\n",
                           "HDMI-A-1") == HdrState::disabled,
           "HDR-incapable output should not require a transition");
    expect(parse_vrr_policy(outputs, "DP-1") == VrrPolicy::automatic,
           "DP-1 VRR policy should be automatic");
    expect(parse_vrr_policy(outputs, "DP-10") == VrrPolicy::never,
           "DP-10 VRR policy should be never");
    expect(parse_vrr_policy(outputs, "HDMI-A-1") == VrrPolicy::unknown,
           "missing output should have unknown VRR policy");

    test_display_session("disabled", true, {
        "read:disabled:15", "read:disabled:F7", "read:disabled:F9", "read:disabled:14",
        "ddc:15=31", "ddc:F7=2", "ddc:F9=55", "ddc:14=11",
        "vrr:automatic", "hdr:enabled", "hdr:disabled",
        "ddc:15=0x2e", "ddc:F7=0x02", "ddc:F9=0x00", "ddc:14=0x05",
        "read:disabled:15", "read:disabled:F7", "read:disabled:F9", "read:disabled:14",
        "vrr:never",
    });
    test_display_session("enabled", false, {
        "hdr:disabled",
        "read:disabled:15", "read:disabled:F7", "read:disabled:F9", "read:disabled:14",
        "ddc:15=31", "ddc:F7=2", "ddc:F9=55", "ddc:14=11",
        "vrr:automatic", "hdr:enabled", "hdr:disabled",
        "ddc:15=0x2e", "ddc:F7=0x02", "ddc:F9=0x00", "ddc:14=0x05",
        "read:disabled:15", "read:disabled:F7", "read:disabled:F9", "read:disabled:14",
        "hdr:enabled", "vrr:never",
    });
    test_display_session("disabled", true, {}, true);
    return 0;
}
