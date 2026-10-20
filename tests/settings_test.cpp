#include "settings.h"
#include "airplay/live_settings.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h>
#endif

namespace fs = std::filesystem;

namespace {

int checks = 0;

void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
    ++checks;
}

void write(const fs::path& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
    if (!out.good()) throw std::runtime_error("Could not write test settings");
}

std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void run(const fs::path& dir) {
#if defined(_WIN32)
    PWSTR local = nullptr;
    check(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)),
          "LocalAppData should be available");
    const auto expected = fs::path(local) / L"FastXR" / L"settings.ini";
    CoTaskMemFree(local);
    check(ap::settings_path() == expected, "Use LocalAppData/FastXR/settings.ini");
#endif
    const auto path = dir / "settings.ini";
    ap::PersistentSettings defaults;
    defaults.mirror_width = 1920;
    defaults.mirror_height = 1080;
    ap::airplay::LiveSettings live;
    {
        ap::SettingsFile file(path);
        check(file.load().mirror_width == 0, "Missing file should use defaults");
        file.create_if_missing(defaults);
        check(fs::exists(path), "First launch should create settings");
        const auto original = read(path);
        ap::apply_settings(defaults, live);
        check(!file.poll(live), "Initial file should not override CLI or hotkeys");
        auto other = defaults;
        other.mirror_width = 3840;
        file.create_if_missing(other);
        check(read(path) == original, "Initialization must not overwrite an existing file");
    }

    ap::SettingsFile file(path);
    auto loaded = file.load();
    check(loaded.mirror_width == 1920 && loaded.mirror_height == 1080,
          "Load existing resolution");
    ap::apply_settings(loaded, live);
    live.fullscreen.store(true);
    check(!file.poll(live) && live.fullscreen.load(),
          "Unchanged disk contents must preserve a window hotkey");

    write(path, "\xEF\xBB\xBF; external update\r\n mirror_width = 2560 \r\n"
                "mirror_height=1440\r\nhevc_enabled=0\r\nmax_fps=30\r\n"
                "refresh_rate=30\r\nmirror_hwaccel=1\r\nvsync_enabled=0\r\n"
                "fullscreen=0\r\nchromeless=1\r\nfastxr_extra=value\r\n");
    check(!file.poll(live) && live.mirror_width.load() == 1920,
          "A changed file must stabilize before applying");
    check(file.poll(live), "Stable external update should load");
    check(live.mirror_width.load() == 2560 && live.mirror_height.load() == 1440 &&
          !live.hevc_enabled.load() && live.max_fps.load() == 30 &&
          live.refresh_rate.load() == 30 && live.mirror_hwaccel.load(),
          "Apply all protocol settings with UTF-8 BOM, spaces, CRLF and unknown keys");
    check(!live.vsync_enabled.load() && !live.fullscreen.load() && live.chromeless.load(),
          "Publish runtime window and VSYNC settings");

    write(path, "");
    check(!file.poll(live) && !file.poll(live) && live.mirror_width.load() == 2560,
          "Temporary truncation must not reset settings");
    write(path, "mirror_width=3840\nmax_fps=0\n");
    check(!file.poll(live) && !file.poll(live) && live.mirror_width.load() == 2560,
          "Reject the whole update when one known value is invalid");
    write(path, "hevc_enabled=2\n");
    check(!file.poll(live) && !file.poll(live) && !live.hevc_enabled.load(),
          "Reject boolean values other than zero and one");
    write(path, "max_fps=60junk\n");
    check(!file.poll(live) && !file.poll(live) && live.max_fps.load() == 30,
          "Reject numeric prefixes followed by junk");
    write(path, "mirror_width=3840\n");
    check(!file.poll(live), "First partial-file snapshot should wait");
    write(path, "mirror_width=3840\nmirror_height=2160\n");
    check(!file.poll(live) && live.mirror_width.load() == 2560,
          "Changes during an external write restart stabilization");
    check(file.poll(live) && live.mirror_width.load() == 3840 &&
          live.mirror_height.load() == 2160 && live.max_fps.load() == 30,
          "Apply completed edit while retaining omitted settings");

    const auto before_replace = fs::last_write_time(path);
    const auto replacement = dir / "settings.tmp";
    write(replacement, "mirror_width=1280\nmirror_height=720\n");
    fs::last_write_time(replacement, before_replace);
#if defined(_WIN32)
    check(MoveFileExW(replacement.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0,
          "Replace settings atomically");
#else
    fs::rename(replacement, path);
#endif
    check(!file.poll(live) && file.poll(live) && live.mirror_width.load() == 1280 &&
          live.mirror_height.load() == 720,
          "Detect replacement by content even when the timestamp does not change");

#if defined(_WIN32)
    const HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(locked != INVALID_HANDLE_VALUE, "Lock the settings file");
    const bool ignored_locked = !file.poll(live);
    CloseHandle(locked);
    check(ignored_locked && live.mirror_width.load() == 1280,
          "Keep last values while the external writer locks the file");
#endif
    fs::remove(path);
    check(!file.poll(live) && live.mirror_width.load() == 1280,
          "Keep last values if the file temporarily disappears");
    write(path, "fullscreen=1\nchromeless=0\nvsync_enabled=1\n");
    check(!file.poll(live) && file.poll(live) && live.fullscreen.load() &&
          !live.chromeless.load() && live.vsync_enabled.load(),
          "Resume loading after deletion and recreation");
    const auto owned_contents = read(path);
    {
        ap::SettingsFile reader(path);
        reader.load();
        reader.create_if_missing(defaults);
    }
    check(read(path) == owned_contents, "Never write back external settings on destruction");
}

} // namespace

int main(int argc, char** argv) {
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path dir = (argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path()) / unique;
    try {
        fs::create_directories(dir);
        run(dir);
        fs::remove_all(dir);
        std::cout << checks << " settings checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::error_code ec;
        fs::remove_all(dir, ec);
        std::cerr << "Settings check failed: " << e.what() << '\n';
        return 1;
    }
}
