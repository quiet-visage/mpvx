#pragma once

#include <cmath>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>

namespace mpvx {

inline constexpr std::string_view APP_NAME = "mpvx";
inline constexpr std::string_view APP_VERSION = "0.1.0";

enum class PlaybackState {
    Idle,
    Playing,
    Paused
};

[[nodiscard]] constexpr std::string_view playback_state_to_string(PlaybackState state) noexcept {
    switch (state) {
        case PlaybackState::Idle:
            return "Idle";
        case PlaybackState::Playing:
            return "Playing";
        case PlaybackState::Paused:
            return "Paused";
    }
    return "Unknown";
}

enum class LoopMode : uint8_t {
    Off,
    File,
    Playlist
};

enum class AbLoopState : uint8_t {
    Off,
    SetA,
    Active
};

struct MediaTrack {
    int id{0};
    std::string type;     // "video", "audio", "sub"
    std::string title;
    std::string lang;
    std::string codec;
    bool selected{false};
    bool external{false};

    [[nodiscard]] std::string display_label() const {
        std::string_view codec_str = codec.empty() ? "unknown" : std::string_view(codec);
        if (!title.empty() && !lang.empty()) {
            return std::format("{} ({}) [{}]", title, lang, codec_str);
        }
        if (!title.empty()) {
            return std::format("{} [{}]", title, codec_str);
        }
        if (!lang.empty()) {
            return std::format("Track {} ({}) [{}]", id, lang, codec_str);
        }
        return std::format("Track {} [{}]", id, codec_str);
    }
};

struct Chapter {
    int index{0};
    std::string title;
    double time{0.0};
};

[[nodiscard]] inline std::string format_timestamp(double seconds) {
    if (seconds < 0.0 || std::isnan(seconds) || std::isinf(seconds)) {
        seconds = 0.0;
    }
    auto total_secs = static_cast<int64_t>(seconds);
    int64_t hours = total_secs / 3600;
    int64_t minutes = (total_secs % 3600) / 60;
    int64_t secs = total_secs % 60;

    if (hours > 0) {
        return std::format("{}:{:02d}:{:02d}", hours, minutes, secs);
    }
    return std::format("{}:{:02d}", minutes, secs);
}

[[nodiscard]] inline std::string format_time_readout(double pos_secs, double dur_secs) {
    return std::format("{} | {}", format_timestamp(pos_secs), format_timestamp(dur_secs));
}

struct PlayerError {
    int code{0};
    std::string message;
};

template <typename T>
using Result = std::expected<T, PlayerError>;

} // namespace mpvx
