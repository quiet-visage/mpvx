#pragma once

#include "mpvx/core.hpp"
#include "mpvx/events.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Forward declarations from libmpv
struct mpv_handle;
struct mpv_node;

namespace mpvx {

[[nodiscard]] std::vector<MediaTrack> parse_track_list_node(const mpv_node& root);
[[nodiscard]] std::vector<Chapter> parse_chapter_list_node(const mpv_node& root);

class Player {
public:
    Player();
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    Player(Player&&) noexcept;
    Player& operator=(Player&&) noexcept;

    [[nodiscard]] Result<void> initialize();
    void shutdown();

    [[nodiscard]] Result<void> load_file(std::string_view path);
    [[nodiscard]] Result<void> play();
    [[nodiscard]] Result<void> pause();
    [[nodiscard]] Result<void> toggle_pause();
    [[nodiscard]] Result<void> stop();
    [[nodiscard]] Result<void> seek(double seconds, bool exact = false);
    [[nodiscard]] Result<void> set_volume(double volume);
    [[nodiscard]] Result<void> set_mute(bool muted);

    // Playlist & Relative Navigation
    [[nodiscard]] Result<void> playlist_prev();
    [[nodiscard]] Result<void> playlist_next();
    [[nodiscard]] Result<void> seek_relative(double delta_seconds);

    // Track Selection & External Subtitles
    [[nodiscard]] Result<void> set_subtitle_track(int sid); // sid <= 0 sets "no"
    [[nodiscard]] Result<void> add_subtitle_file(std::string_view path);
    [[nodiscard]] Result<void> set_audio_track(int aid);
    [[nodiscard]] std::vector<MediaTrack> tracks() const;

    // Chapters
    [[nodiscard]] Result<void> set_chapter(int chapter_index);
    [[nodiscard]] std::vector<Chapter> chapters() const;
    [[nodiscard]] int current_chapter() const noexcept;

    // Loop & A/B Loop
    [[nodiscard]] Result<void> set_loop_mode(LoopMode mode);
    [[nodiscard]] LoopMode loop_mode() const noexcept;
    [[nodiscard]] Result<void> cycle_ab_loop();
    [[nodiscard]] Result<void> clear_ab_loop();
    [[nodiscard]] AbLoopState ab_loop_state() const noexcept;
    [[nodiscard]] std::optional<double> ab_loop_a() const noexcept;
    [[nodiscard]] std::optional<double> ab_loop_b() const noexcept;

    // Playback Settings
    [[nodiscard]] Result<void> set_speed(double speed);
    [[nodiscard]] double speed() const noexcept;
    [[nodiscard]] Result<void> set_hwdec(std::string_view mode);
    [[nodiscard]] Result<void> set_aspect_override(std::string_view aspect);

    [[nodiscard]] double duration() const noexcept;
    [[nodiscard]] double time_pos() const noexcept;
    [[nodiscard]] double volume() const noexcept;
    [[nodiscard]] bool is_muted() const noexcept;
    [[nodiscard]] bool is_buffering() const noexcept;
    [[nodiscard]] PlaybackState state() const noexcept;
    [[nodiscard]] std::string media_title() const;

    void set_callbacks(PlayerCallbacks callbacks);

    [[nodiscard]] mpv_handle* handle() const noexcept { return mpv_; }

private:
    void event_loop();
    void observe_properties();
    void handle_mpv_event(void* event_ptr);
    void update_ab_loop_state_locked();
    void set_buffering_state(bool buffering);
    void recompute_buffering_state();

    mpv_handle* mpv_{nullptr};
    std::atomic<bool> running_{false};
    std::thread event_thread_;

    mutable std::mutex mutex_;
    PlayerCallbacks callbacks_;

    std::atomic<PlaybackState> state_{PlaybackState::Idle};
    std::atomic<double> duration_{0.0};
    std::atomic<double> time_pos_{0.0};
    std::atomic<double> volume_{100.0};
    std::atomic<bool> muted_{false};
    std::atomic<bool> buffering_{false};
    std::atomic<bool> opening_{false};
    std::atomic<bool> seeking_{false};
    std::atomic<bool> core_idle_{false};
    std::atomic<bool> paused_for_cache_{false};
    std::atomic<double> speed_{1.0};
    std::atomic<int> current_chapter_{-1};
    std::atomic<LoopMode> loop_mode_{LoopMode::Off};
    std::atomic<AbLoopState> ab_loop_state_{AbLoopState::Off};

    std::optional<double> ab_loop_a_;
    std::optional<double> ab_loop_b_;
    std::vector<MediaTrack> tracks_;
    std::vector<Chapter> chapters_;
    std::string media_title_;
};

} // namespace mpvx
