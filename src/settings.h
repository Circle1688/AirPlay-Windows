#pragma once

#include <filesystem>
#include <string>

namespace ap::airplay { struct LiveSettings; }

namespace ap {

struct PersistentSettings {
    int  mirror_width   = 0;
    int  mirror_height  = 0;
    bool hevc_enabled   = true;
    int  max_fps        = 60;
    int  refresh_rate   = 60;
    bool mirror_hwaccel = false;
    bool vsync_enabled  = true;
    bool fullscreen     = false;
    bool chromeless     = false;
};

std::filesystem::path settings_path();
void apply_settings(const PersistentSettings& s, ap::airplay::LiveSettings& live);

// FastXR owns this file. Create it only when missing; never overwrite it on exit.
class SettingsFile {
public:
    explicit SettingsFile(std::filesystem::path path = settings_path());
    PersistentSettings load();
    void create_if_missing(const PersistentSettings& defaults);
    bool poll(ap::airplay::LiveSettings& live);

private:
    std::filesystem::path path_;
    PersistentSettings settings_;
    std::string applied_contents_;
    std::string pending_contents_;
    bool have_pending_ = false;
};

} // namespace ap
