#include "settings.h"
#include "airplay/live_settings.h"
#include "log.h"

#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <shlobj.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ap {
namespace {

#if defined(_WIN32)
std::filesystem::path app_data_path(REFKNOWNFOLDERID folder) {
    PWSTR wide_path = nullptr;
    if (FAILED(SHGetKnownFolderPath(folder, 0, nullptr, &wide_path))) {
        CoTaskMemFree(wide_path);
        return {};
    }
    std::filesystem::path path(wide_path);
    CoTaskMemFree(wide_path);
    return path;
}
#endif

bool read_file(const std::filesystem::path& path, std::string& contents) {
    if (path.empty()) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    contents.assign(std::istreambuf_iterator<char>(in), {});
    return !in.bad();
}

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

// Validate the whole update before publishing any fields. Missing keys retain
// their last accepted values; unknown keys belong to the external application.
bool parse_settings(std::string contents, PersistentSettings& settings) {
    if (contents.compare(0, 3, "\xEF\xBB\xBF") == 0) contents.erase(0, 3);
    std::istringstream in(contents);
    std::string line;
    bool recognized = false;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) return false;
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        int* number = nullptr;
        bool* flag = nullptr;
        int minimum = 1;
        int maximum = 60;
        if (key == "mirror_width") {
            number = &settings.mirror_width; minimum = 320; maximum = 7680;
        } else if (key == "mirror_height") {
            number = &settings.mirror_height; minimum = 240; maximum = 4320;
        } else if (key == "max_fps")       number = &settings.max_fps;
        else if (key == "refresh_rate")   number = &settings.refresh_rate;
        else if (key == "hevc_enabled")   flag = &settings.hevc_enabled;
        else if (key == "mirror_hwaccel") flag = &settings.mirror_hwaccel;
        else if (key == "vsync_enabled")  flag = &settings.vsync_enabled;
        else if (key == "fullscreen")     flag = &settings.fullscreen;
        else if (key == "chromeless")     flag = &settings.chromeless;
        else continue;

        try {
            std::size_t used = 0;
            const int n = std::stoi(value, &used);
            if (used != value.size()) return false;
            if (flag) {
                if (n != 0 && n != 1) return false;
                *flag = n != 0;
            } else {
                if (n < minimum || n > maximum) return false;
                *number = n;
            }
            recognized = true;
        } catch (const std::exception&) {
            return false;
        }
    }
    return recognized;
}

std::string serialize_settings(const PersistentSettings& s) {
    std::ostringstream out;
    out << "mirror_width="   << s.mirror_width   << '\n'
        << "mirror_height="  << s.mirror_height  << '\n'
        << "hevc_enabled="   << s.hevc_enabled   << '\n'
        << "max_fps="        << s.max_fps        << '\n'
        << "refresh_rate="   << s.refresh_rate   << '\n'
        << "mirror_hwaccel=" << s.mirror_hwaccel << '\n'
        << "vsync_enabled="  << s.vsync_enabled  << '\n'
        << "fullscreen="     << s.fullscreen     << '\n'
        << "chromeless="     << s.chromeless     << '\n';
    return out.str();
}

} // namespace

std::filesystem::path settings_path() {
#if defined(_WIN32)
    const auto base = app_data_path(FOLDERID_LocalAppData);
    if (!base.empty()) return base / L"FastXR" / L"settings.ini";
#endif
    return {};
}

SettingsFile::SettingsFile(std::filesystem::path path) : path_(std::move(path)) {}

PersistentSettings SettingsFile::load() {
    std::string contents;
    if (!read_file(path_, contents)) {
#if defined(_WIN32)
        // Preserve existing preferences on the first launch after relocation.
        std::error_code ec;
        if (path_ == settings_path() && !std::filesystem::exists(path_, ec) && !ec) {
            const auto legacy_base = app_data_path(FOLDERID_RoamingAppData);
            if (!legacy_base.empty()) {
                read_file(legacy_base / L"AirPlay-Windows" / L"settings.ini", contents);
            }
        }
#endif
    } else {
        applied_contents_ = contents;
    }
    auto candidate = settings_;
    if (parse_settings(contents, candidate)) settings_ = candidate;
    return settings_;
}

void SettingsFile::create_if_missing(const PersistentSettings& defaults) {
    settings_ = defaults;
    if (path_.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    if (ec) {
        LOG_WARN << "settings: could not create directory: " << ec.message();
        return;
    }
    const std::string contents = serialize_settings(defaults);
#if defined(_WIN32)
    // CREATE_NEW prevents clobbering a file created concurrently by FastXR.
    const HANDLE file = CreateFileW(path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                   nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
            LOG_WARN << "settings: could not create " << path_.u8string();
        return;
    }
    DWORD written = 0;
    const bool ok = WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()),
                              &written, nullptr) && written == contents.size();
    CloseHandle(file);
#else
    const int file = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (file < 0) return;
    const bool ok = ::write(file, contents.data(), contents.size()) ==
                    static_cast<ssize_t>(contents.size());
    ::close(file);
#endif
    if (ok) {
        applied_contents_ = contents;
        LOG_INFO << "settings: created " << path_.u8string();
    } else {
        LOG_WARN << "settings: could not initialize " << path_.u8string();
    }
}

bool SettingsFile::poll(ap::airplay::LiveSettings& live) {
    std::string contents;
    if (!read_file(path_, contents) || contents.empty() || contents == applied_contents_) {
        have_pending_ = false;
        return false;
    }
    // Require identical bytes on two polls to skip the usual truncate/write
    // interval of editors. Content comparison also supports atomic replacement.
    if (!have_pending_ || contents != pending_contents_) {
        pending_contents_ = std::move(contents);
        have_pending_ = true;
        return false;
    }
    have_pending_ = false;
    applied_contents_ = contents;
    auto candidate = settings_;
    if (!parse_settings(contents, candidate)) {
        LOG_WARN << "settings: ignoring incomplete or invalid update in " << path_.u8string();
        return false;
    }
    settings_ = candidate;
    apply_settings(settings_, live);
    LOG_INFO << "settings: reloaded " << path_.u8string();
    return true;
}

void apply_settings(const PersistentSettings& s, ap::airplay::LiveSettings& live) {
    live.mirror_width.store(s.mirror_width, std::memory_order_relaxed);
    live.mirror_height.store(s.mirror_height, std::memory_order_relaxed);
    live.hevc_enabled.store(s.hevc_enabled, std::memory_order_relaxed);
    live.max_fps.store(s.max_fps, std::memory_order_relaxed);
    live.refresh_rate.store(s.refresh_rate, std::memory_order_relaxed);
    live.mirror_hwaccel.store(s.mirror_hwaccel, std::memory_order_relaxed);
    live.vsync_enabled.store(s.vsync_enabled, std::memory_order_relaxed);
    live.fullscreen.store(s.fullscreen, std::memory_order_relaxed);
    live.chromeless.store(s.chromeless, std::memory_order_relaxed);
}

} // namespace ap
