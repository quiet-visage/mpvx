#pragma once

#include "mpvx/core.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mpvx {

struct PlayerCallbacks {
    std::function<void(PlaybackState)> on_state_changed;
    std::function<void(double time_pos)> on_time_pos;
    std::function<void(double duration)> on_duration;
    std::function<void(const std::string& title)> on_media_title;
    std::function<void(double volume)> on_volume;
    std::function<void(bool muted)> on_mute;
    std::function<void(const std::string& filename)> on_file_loaded;
    std::function<void(const std::string& error_msg)> on_error;

    // HUD & Media Control Callbacks
    std::function<void(bool buffering)> on_buffering_changed;
    std::function<void(const std::vector<MediaTrack>&)> on_tracks_changed;
    std::function<void(const std::vector<Chapter>&)> on_chapters_changed;
    std::function<void(int chapter_index)> on_chapter_changed;
    std::function<void(AbLoopState, std::optional<double> a, std::optional<double> b)> on_ab_loop_changed;
    std::function<void(LoopMode)> on_loop_mode_changed;
    std::function<void(double speed)> on_speed_changed;
};

} // namespace mpvx
