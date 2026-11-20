#include "mpvx/player.hpp"

#include <mpv/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <print>
#include <utility>

namespace mpvx {

std::vector<MediaTrack> parse_track_list_node(const mpv_node& root) {
    std::vector<MediaTrack> result;
    if (root.format != MPV_FORMAT_NODE_ARRAY || !root.u.list) {
        return result;
    }

    const mpv_node_list* list = root.u.list;
    result.reserve(static_cast<size_t>(std::max(0, list->num)));

    for (int i = 0; i < list->num; ++i) {
        const mpv_node& entry = list->values[i];
        if (entry.format != MPV_FORMAT_NODE_MAP || !entry.u.list) {
            continue;
        }

        MediaTrack track;
        const mpv_node_list* map = entry.u.list;
        for (int k = 0; k < map->num; ++k) {
            if (!map->keys || !map->keys[k]) continue;
            std::string_view key(map->keys[k]);
            const mpv_node& val = map->values[k];

            if (key == "id" && val.format == MPV_FORMAT_INT64) {
                track.id = static_cast<int>(val.u.int64);
            } else if (key == "type" && val.format == MPV_FORMAT_STRING && val.u.string) {
                track.type = val.u.string;
            } else if (key == "title" && val.format == MPV_FORMAT_STRING && val.u.string) {
                track.title = val.u.string;
            } else if (key == "lang" && val.format == MPV_FORMAT_STRING && val.u.string) {
                track.lang = val.u.string;
            } else if ((key == "codec" || key == "decoder-desc") && val.format == MPV_FORMAT_STRING && val.u.string) {
                if (track.codec.empty() || key == "codec") {
                    track.codec = val.u.string;
                }
            } else if (key == "selected" && val.format == MPV_FORMAT_FLAG) {
                track.selected = (val.u.flag != 0);
            } else if (key == "external" && val.format == MPV_FORMAT_FLAG) {
                track.external = (val.u.flag != 0);
            }
        }

        if (!track.type.empty()) {
            result.push_back(std::move(track));
        }
    }

    return result;
}

VideoDisplaySize parse_video_out_params_node(const mpv_node& root) {
    if (root.format != MPV_FORMAT_NODE_MAP || !root.u.list) {
        return {};
    }

    int64_t dw = 0;
    int64_t dh = 0;
    int64_t rotate = 0;
    const mpv_node_list* map = root.u.list;
    for (int k = 0; k < map->num; ++k) {
        if (!map->keys || !map->keys[k]) continue;
        std::string_view key(map->keys[k]);
        const mpv_node& val = map->values[k];
        if (val.format != MPV_FORMAT_INT64) continue;
        if (key == "dw") {
            dw = val.u.int64;
        } else if (key == "dh") {
            dh = val.u.int64;
        } else if (key == "rotate") {
            rotate = val.u.int64;
        }
    }

    constexpr int64_t kMaxSide = 1 << 16;
    if (dw <= 0 || dh <= 0 || dw > kMaxSide || dh > kMaxSide) {
        return {};
    }
    if (((rotate % 360) + 360) % 180 == 90) {
        std::swap(dw, dh);
    }
    return VideoDisplaySize{static_cast<int>(dw), static_cast<int>(dh)};
}

std::vector<Chapter> parse_chapter_list_node(const mpv_node& root) {
    std::vector<Chapter> result;
    if (root.format != MPV_FORMAT_NODE_ARRAY || !root.u.list) {
        return result;
    }

    const mpv_node_list* list = root.u.list;
    result.reserve(static_cast<size_t>(std::max(0, list->num)));

    for (int i = 0; i < list->num; ++i) {
        const mpv_node& entry = list->values[i];
        if (entry.format != MPV_FORMAT_NODE_MAP || !entry.u.list) {
            continue;
        }

        Chapter ch;
        ch.index = i;
        const mpv_node_list* map = entry.u.list;
        for (int k = 0; k < map->num; ++k) {
            if (!map->keys || !map->keys[k]) continue;
            std::string_view key(map->keys[k]);
            const mpv_node& val = map->values[k];

            if (key == "title" && val.format == MPV_FORMAT_STRING && val.u.string) {
                ch.title = val.u.string;
            } else if (key == "time" && val.format == MPV_FORMAT_DOUBLE) {
                ch.time = val.u.double_;
            } else if (key == "time" && val.format == MPV_FORMAT_INT64) {
                ch.time = static_cast<double>(val.u.int64);
            }
        }

        if (ch.title.empty()) {
            ch.title = std::format("Chapter {}", i + 1);
        }
        result.push_back(std::move(ch));
    }

    return result;
}

Player::Player() = default;

Player::~Player() {
    shutdown();
}

Player::Player(Player&& other) noexcept {
    *this = std::move(other);
}

Player& Player::operator=(Player&& other) noexcept {
    if (this != &other) {
        shutdown();
        mpv_ = other.mpv_;
        other.mpv_ = nullptr;
        running_.store(other.running_.load());
        other.running_.store(false);
        if (other.event_thread_.joinable()) {
            event_thread_ = std::move(other.event_thread_);
        }
        state_.store(other.state_.load());
        paused_.store(other.paused_.load());
        idle_active_.store(other.idle_active_.load());
        file_loaded_.store(other.file_loaded_.load());
        eof_reached_.store(other.eof_reached_.load());
        duration_.store(other.duration_.load());
        time_pos_.store(other.time_pos_.load());
        volume_.store(other.volume_.load());
        muted_.store(other.muted_.load());
        buffering_.store(other.buffering_.load());
        opening_.store(other.opening_.load());
        seeking_.store(other.seeking_.load());
        core_idle_.store(other.core_idle_.load());
        paused_for_cache_.store(other.paused_for_cache_.load());
        speed_.store(other.speed_.load());
        current_chapter_.store(other.current_chapter_.load());
        loop_mode_.store(other.loop_mode_.load());
        ab_loop_state_.store(other.ab_loop_state_.load());
        video_size_.store(other.video_size_.load());
        sub_visibility_.store(other.sub_visibility_.load());
        osd_level_.store(other.osd_level_.load());
        mini_profile_.store(other.mini_profile_.load());
        saved_sub_visibility_.store(other.saved_sub_visibility_.load());
        saved_osd_level_.store(other.saved_osd_level_.load());
        {
            std::scoped_lock lock(mutex_, other.mutex_);
            callbacks_ = std::move(other.callbacks_);
            media_title_ = std::move(other.media_title_);
            ab_loop_a_ = other.ab_loop_a_;
            ab_loop_b_ = other.ab_loop_b_;
            tracks_ = std::move(other.tracks_);
            chapters_ = std::move(other.chapters_);
        }
    }
    return *this;
}

Result<void> Player::initialize() {
    if (mpv_ != nullptr) {
        return {};
    }

    mpv_ = mpv_create();
    if (!mpv_) {
        return std::unexpected(PlayerError{-1, "Failed to create mpv instance"});
    }

    // Configure sensible default options for frontend integration
    mpv_set_option_string(mpv_, "vo", "libmpv");
    mpv_set_option_string(mpv_, "terminal", "yes");
    mpv_set_option_string(mpv_, "msg-level", "all=v");
    mpv_set_option_string(mpv_, "ytdl", "yes");
    mpv_set_option_string(mpv_, "keep-open", "yes");
    mpv_set_option_string(mpv_, "idle", "yes");
    mpv_set_option_string(mpv_, "hwdec", "auto-safe");

    int status = mpv_initialize(mpv_);
    if (status < 0) {
        std::string err = mpv_error_string(status);
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
        return std::unexpected(PlayerError{status, std::format("Failed to initialize mpv: {}", err)});
    }

    observe_properties();

    running_.store(true);
    event_thread_ = std::thread(&Player::event_loop, this);

    std::println("[mpvx] libmpv initialized successfully (API version: {})", mpv_client_api_version());
    return {};
}

void Player::observe_properties() {
    if (!mpv_) return;
    mpv_observe_property(mpv_, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, 0, "eof-reached", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, 0, "volume", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 0, "mute", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, 0, "media-title", MPV_FORMAT_STRING);
    mpv_observe_property(mpv_, 0, "idle-active", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, 0, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, 0, "seeking", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, 0, "core-idle", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, 0, "track-list", MPV_FORMAT_NODE);
    mpv_observe_property(mpv_, 0, "chapter-list", MPV_FORMAT_NODE);
    mpv_observe_property(mpv_, 0, "chapter", MPV_FORMAT_INT64);
    mpv_observe_property(mpv_, 0, "ab-loop-a", MPV_FORMAT_NODE);
    mpv_observe_property(mpv_, 0, "ab-loop-b", MPV_FORMAT_NODE);
    mpv_observe_property(mpv_, 0, "loop-file", MPV_FORMAT_NODE);
    mpv_observe_property(mpv_, 0, "loop-playlist", MPV_FORMAT_NODE);
    mpv_observe_property(mpv_, 0, "speed", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 0, "playlist-pos", MPV_FORMAT_INT64);
    mpv_observe_property(mpv_, 0, "video-out-params", MPV_FORMAT_NODE);
    mpv_observe_property(mpv_, 0, "sub-visibility", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, 0, "osd-level", MPV_FORMAT_INT64);
}

void Player::shutdown() {
    if (running_.exchange(false)) {
        if (mpv_) {
            mpv_wakeup(mpv_);
        }
        if (event_thread_.joinable()) {
            event_thread_.join();
        }
    }

    if (mpv_) {
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
    }
}

Result<void> Player::load_file(std::string_view path) {
    if (!mpv_) {
        return std::unexpected(PlayerError{-1, "Player not initialized"});
    }

    (void)clear_ab_loop();
    file_loaded_.store(false, std::memory_order_release);
    eof_reached_.store(false, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
    paused_for_cache_.store(false);
    seeking_.store(false);
    opening_.store(true);
    recompute_buffering_state();

    int pause_val = 0;
    (void)mpv_set_property_async(mpv_, 0, "pause", MPV_FORMAT_FLAG, &pause_val);

    std::string path_str(path);
    const char* cmd[] = {"loadfile", path_str.c_str(), "replace", nullptr};
    int status = mpv_command_async(mpv_, 0, cmd);
    if (status < 0) {
        opening_.store(false);
        recompute_buffering_state();
        return std::unexpected(PlayerError{status, mpv_error_string(status)});
    }
    return {};
}

Result<void> Player::play() {
    if (!mpv_) return std::unexpected(PlayerError{-1, "Player not initialized"});
    if (file_loaded_.load(std::memory_order_acquire)
        && eof_reached_.load(std::memory_order_acquire)
        && paused_.load(std::memory_order_acquire)) {
        eof_reached_.store(false, std::memory_order_release);
        (void)seek(0.0, true);
    }
    paused_.store(false, std::memory_order_release);
    int pause_val = 0;
    int status = mpv_set_property_async(mpv_, 0, "pause", MPV_FORMAT_FLAG, &pause_val);
    if (status < 0) return std::unexpected(PlayerError{status, mpv_error_string(status)});
    return {};
}

Result<void> Player::pause() {
    if (!mpv_) return std::unexpected(PlayerError{-1, "Player not initialized"});
    paused_.store(true, std::memory_order_release);
    int pause_val = 1;
    int status = mpv_set_property_async(mpv_, 0, "pause", MPV_FORMAT_FLAG, &pause_val);
    if (status < 0) return std::unexpected(PlayerError{status, mpv_error_string(status)});
    return {};
}

Result<void> Player::toggle_pause() {
    if (!mpv_) return std::unexpected(PlayerError{-1, "Player not initialized"});
    if (paused_.load(std::memory_order_acquire)) {
        return play();
    }
    return pause();
}

Result<void> Player::stop() {
    if (!mpv_) return std::unexpected(PlayerError{-1, "Player not initialized"});
    file_loaded_.store(false, std::memory_order_release);
    eof_reached_.store(false, std::memory_order_release);
    opening_.store(false);
    seeking_.store(false);
    core_idle_.store(false);
    paused_for_cache_.store(false);
    update_playback_state(PlaybackState::Idle);
    set_buffering_state(false);
    const char* cmd[] = {"stop", nullptr};
    int status = mpv_command_async(mpv_, 0, cmd);
    if (status < 0) return std::unexpected(PlayerError{status, mpv_error_string(status)});
    return {};
}

Result<void> Player::seek(double seconds, bool exact) {
    if (!mpv_) return std::unexpected(PlayerError{-1, "Player not initialized"});
    if (state_.load() != PlaybackState::Idle) {
        seeking_.store(true);
        recompute_buffering_state();
    }
    std::string sec_str = std::format("{:.2f}", std::max(0.0, seconds));
    const char* mode = exact ? "absolute+exact" : "absolute";
    const char* cmd[] = {"seek", sec_str.c_str(), mode, nullptr};
    int status = mpv_command_async(mpv_, 0, cmd);
    if (status < 0) {
        seeking_.store(false);
        recompute_buffering_state();
        return std::unexpected(PlayerError{status, mpv_error_string(status)});
    }
    return {};
}

Result<void> Player::seek_relative(double delta_seconds) {
    if (!mpv_) return std::unexpected(PlayerError{-1, "Player not initialized"});
    if (state_.load() != PlaybackState::Idle) {
        seeking_.store(true);
        recompute_buffering_state();
    }
    std::string sec_str = std::format("{:.2f}", delta_seconds);
    const char* cmd[] = {"seek", sec_str.c_str(), "relative", nullptr};
    int status = mpv_command_async(mpv_, 0, cmd);
    if (status < 0) {
        seeking_.store(false);
        recompute_buffering_state();
        return std::unexpected(PlayerError{status, mpv_error_string(status)});
    }
    return {};
}

Result<void> Player::playlist_prev() {
    if (!mpv_) return std::unexpected(PlayerError{-1, "Player not initialized"});
    int64_t playlist_pos = playlist_pos_.load(std::memory_order_acquire);
    if (playlist_pos <= 0 || time_pos_.load(std::memory_order_acquire) > 3.0) {
        return seek(0.0, true);
    }
    const char* cmd[] = {"playlist-prev", "weak", nullptr};
    int status = mpv_command_async(mpv_, 0, cmd);
    if (status < 0) return std::unexpected(PlayerError{status, mpv_error_string(status)});
    return {};
}

Result<void> Player::playlist_next() {
    if (!mpv_) return std::unexpected(PlayerError{-1, "Player not initialized"});
    const char* cmd[] = {"playlist-next", "weak", nullptr};
    int status = mpv_command_async(mpv_, 0, cmd);
    if (status < 0) return std::unexpected(PlayerError{status, mpv_error_string(status)});
    return {};
}

Result<void> Player::set_volume(double volume) {
    if (!mpv_) return std::unexpected(PlayerError{-1, "Player not initialized"});
    double clamped = std::clamp(volume, 0.0, 150.0);
    int status = mpv_set_property_async(mpv_, 0, "volume", MPV_FORMAT_DOUBLE, &clamped);
    if (status < 0) return std::unexpected(PlayerError{status, mpv_error_string(status)});
    return {};
}

Result<void> Player::set_mute(bool muted) {
    if (!mpv_) return std::unexpected(PlayerError{-1, "Player not initialized"});
    int mute_val = muted ? 1 : 0;
    int status = mpv_set_property_async(mpv_, 0, "mute", MPV_FORMAT_FLAG, &mute_val);
    if (status < 0) return std::unexpected(PlayerError{status, mpv_error_string(status)});
    return {};
}

Result<void> Player::set_subtitle_track(int sid) {
    if (!mpv_) return std::unexpected(PlayerError{-1, "Player not initialized"});
    std::string val = (sid <= 0) ? "no" : std::to_string(sid);
    const char* cstr = val.c_str();
    int status = mpv_set_property_async(mpv_, 0, "sid", MPV_FORMAT_STRING, &cstr);
    if (status < 0) return std::unexpected(PlayerError{status, mpv_error_string(status)});
    return {};
}

Result<void> Player::add_subtitle_file(std::string_view path) {
    if (!mpv_) return std::unexpected(PlayerError{-1, "Player not initialized"});
    std::string path_str(path);
    const char* cmd[] = {"sub-add", path_str.c_str(), "select", nullptr};
    int status = mpv_command_async(mpv_, 0, cmd);
    if (status < 0) return std::unexpected(PlayerError{status, mpv_error_string(status)});
    return {};
}

Result<void> Player::set_audio_track(int aid) {
    if (!mpv_) return std::unexpected(PlayerError{-1, "Player not initialized"});
    std::string val = (aid <= 0) ? "no" : std::to_string(aid);
    const char* cstr = val.c_str();
    int status = mpv_set_property_async(mpv_, 0, "aid", MPV_FORMAT_STRING, &cstr);
    if (status < 0) return std::unexpected(PlayerError{status, mpv_error_string(status)});
    return {};
}

std::vector<MediaTrack> Player::tracks() const {
    std::lock_guard lock(mutex_);
    return tracks_;
}

Result<void> Player::set_chapter(int chapter_index) {
    if (!mpv_) return std::unexpected(PlayerError{-1, "Player not initialized"});
    current_chapter_.store(chapter_index);
    double target_time = -1.0;
    {
        std::lock_guard lock(mutex_);
        for (const auto& ch : chapters_) {
            if (ch.index == chapter_index) {
                target_time = ch.time;
                break;
            }
        }
    }
    if (target_time >= 0.0) {
        return seek(target_time, true);
    }
    int64_t idx = static_cast<int64_t>(chapter_index);
    int status = mpv_set_property_async(mpv_, 0, "chapter", MPV_FORMAT_INT64, &idx);
    if (status < 0) return std::unexpected(PlayerError{status, mpv_error_string(status)});
    return {};
}

std::vector<Chapter> Player::chapters() const {
    std::lock_guard lock(mutex_);
    return chapters_;
}

int Player::current_chapter() const noexcept {
    return current_chapter_.load();
}

Result<void> Player::set_loop_mode(LoopMode mode) {
    if (!mpv_) return std::unexpected(PlayerError{-1, "Player not initialized"});
    const char* loop_file_val = "no";
    const char* loop_playlist_val = "no";
    switch (mode) {
        case LoopMode::Off:
            break;
        case LoopMode::File:
            loop_file_val = "inf";
            break;
        case LoopMode::Playlist:
            loop_playlist_val = "inf";
            break;
    }
    int s1 = mpv_set_property_async(mpv_, 0, "loop-file", MPV_FORMAT_STRING, &loop_file_val);
    int s2 = mpv_set_property_async(mpv_, 0, "loop-playlist", MPV_FORMAT_STRING, &loop_playlist_val);
    if (s1 < 0) return std::unexpected(PlayerError{s1, mpv_error_string(s1)});
    if (s2 < 0) return std::unexpected(PlayerError{s2, mpv_error_string(s2)});

    loop_mode_.store(mode);
    std::function<void(LoopMode)> cb;
    {
        std::lock_guard lock(mutex_);
        cb = callbacks_.on_loop_mode_changed;
    }
    if (cb) {
        cb(mode);
    }
    return {};
}

LoopMode Player::loop_mode() const noexcept {
    return loop_mode_.load();
}

void Player::update_ab_loop_state_locked() {
    AbLoopState next_state = AbLoopState::Off;
    if (ab_loop_a_.has_value() && ab_loop_b_.has_value()) {
        next_state = AbLoopState::Active;
    } else if (ab_loop_a_.has_value()) {
        next_state = AbLoopState::SetA;
    }
    ab_loop_state_.store(next_state);
}

Result<void> Player::cycle_ab_loop() {
    if (!mpv_) return std::unexpected(PlayerError{-1, "Player not initialized"});

    AbLoopState current = ab_loop_state_.load();
    double now = std::max(0.0, time_pos_.load());

    if (current == AbLoopState::Off) {
        const char* no_str = "no";
        int s1 = mpv_set_property_async(mpv_, 0, "ab-loop-a", MPV_FORMAT_DOUBLE, &now);
        int s2 = mpv_set_property_async(mpv_, 0, "ab-loop-b", MPV_FORMAT_STRING, &no_str);
        if (s1 < 0) return std::unexpected(PlayerError{s1, mpv_error_string(s1)});
        if (s2 < 0) return std::unexpected(PlayerError{s2, mpv_error_string(s2)});

        std::function<void(AbLoopState, std::optional<double>, std::optional<double>)> cb;
        AbLoopState st;
        std::optional<double> a_opt;
        std::optional<double> b_opt;
        {
            std::lock_guard lock(mutex_);
            ab_loop_a_ = now;
            ab_loop_b_ = std::nullopt;
            update_ab_loop_state_locked();
            st = ab_loop_state_.load();
            a_opt = ab_loop_a_;
            b_opt = ab_loop_b_;
            cb = callbacks_.on_ab_loop_changed;
        }
        if (cb) cb(st, a_opt, b_opt);
        return {};
    }

    if (current == AbLoopState::SetA) {
        double a_val = 0.0;
        {
            std::lock_guard lock(mutex_);
            a_val = ab_loop_a_.value_or(0.0);
        }

        double new_a = a_val;
        double new_b = now;
        if (new_b < new_a) {
            std::swap(new_a, new_b);
        } else if (std::abs(new_b - new_a) < 0.05) {
            double dur = duration_.load();
            new_b = (dur > new_a + 0.1) ? std::min(dur, new_a + 5.0) : (new_a + 5.0);
        }

        int s1 = mpv_set_property_async(mpv_, 0, "ab-loop-a", MPV_FORMAT_DOUBLE, &new_a);
        int s2 = mpv_set_property_async(mpv_, 0, "ab-loop-b", MPV_FORMAT_DOUBLE, &new_b);
        if (s1 < 0) return std::unexpected(PlayerError{s1, mpv_error_string(s1)});
        if (s2 < 0) return std::unexpected(PlayerError{s2, mpv_error_string(s2)});

        std::function<void(AbLoopState, std::optional<double>, std::optional<double>)> cb;
        AbLoopState st;
        std::optional<double> a_opt;
        std::optional<double> b_opt;
        {
            std::lock_guard lock(mutex_);
            ab_loop_a_ = new_a;
            ab_loop_b_ = new_b;
            update_ab_loop_state_locked();
            st = ab_loop_state_.load();
            a_opt = ab_loop_a_;
            b_opt = ab_loop_b_;
            cb = callbacks_.on_ab_loop_changed;
        }
        if (cb) cb(st, a_opt, b_opt);
        return {};
    }

    return clear_ab_loop();
}

Result<void> Player::clear_ab_loop() {
    if (!mpv_) return std::unexpected(PlayerError{-1, "Player not initialized"});
    const char* no_str = "no";
    int s1 = mpv_set_property_async(mpv_, 0, "ab-loop-a", MPV_FORMAT_STRING, &no_str);
    int s2 = mpv_set_property_async(mpv_, 0, "ab-loop-b", MPV_FORMAT_STRING, &no_str);
    if (s1 < 0) return std::unexpected(PlayerError{s1, mpv_error_string(s1)});
    if (s2 < 0) return std::unexpected(PlayerError{s2, mpv_error_string(s2)});

    std::function<void(AbLoopState, std::optional<double>, std::optional<double>)> cb;
    AbLoopState st;
    std::optional<double> a_opt;
    std::optional<double> b_opt;
    {
        std::lock_guard lock(mutex_);
        ab_loop_a_ = std::nullopt;
        ab_loop_b_ = std::nullopt;
        update_ab_loop_state_locked();
        st = ab_loop_state_.load();
        a_opt = ab_loop_a_;
        b_opt = ab_loop_b_;
        cb = callbacks_.on_ab_loop_changed;
    }
    if (cb) cb(st, a_opt, b_opt);
    return {};
}

AbLoopState Player::ab_loop_state() const noexcept {
    return ab_loop_state_.load();
}

std::optional<double> Player::ab_loop_a() const noexcept {
    std::lock_guard lock(mutex_);
    return ab_loop_a_;
}

std::optional<double> Player::ab_loop_b() const noexcept {
    std::lock_guard lock(mutex_);
    return ab_loop_b_;
}

Result<void> Player::set_speed(double speed) {
    if (!mpv_) return std::unexpected(PlayerError{-1, "Player not initialized"});
    double clamped = std::clamp(speed, 0.1, 4.0);
    int status = mpv_set_property_async(mpv_, 0, "speed", MPV_FORMAT_DOUBLE, &clamped);
    if (status < 0) return std::unexpected(PlayerError{status, mpv_error_string(status)});
    speed_.store(clamped);
    std::function<void(double)> cb;
    {
        std::lock_guard lock(mutex_);
        cb = callbacks_.on_speed_changed;
    }
    if (cb) {
        cb(clamped);
    }
    return {};
}

double Player::speed() const noexcept {
    return speed_.load();
}

Result<void> Player::set_hwdec(std::string_view mode) {
    if (!mpv_) return std::unexpected(PlayerError{-1, "Player not initialized"});
    std::string mode_str(mode.empty() ? "auto-safe" : mode);
    const char* cstr = mode_str.c_str();
    int status = mpv_set_property_async(mpv_, 0, "hwdec", MPV_FORMAT_STRING, &cstr);
    if (status < 0) return std::unexpected(PlayerError{status, mpv_error_string(status)});
    return {};
}

Result<void> Player::set_aspect_override(std::string_view aspect) {
    if (!mpv_) return std::unexpected(PlayerError{-1, "Player not initialized"});
    std::string val = (aspect.empty() || aspect == "auto") ? "-1" : std::string(aspect);
    const char* cstr = val.c_str();
    int status = mpv_set_property_async(mpv_, 0, "video-aspect-override", MPV_FORMAT_STRING, &cstr);
    if (status < 0) return std::unexpected(PlayerError{status, mpv_error_string(status)});
    return {};
}

Result<void> Player::set_mini_profile(bool mini) {
    if (!mpv_) return std::unexpected(PlayerError{-1, "Player not initialized"});
    if (mini_profile_.exchange(mini, std::memory_order_acq_rel) == mini) {
        return {}; // already in the requested profile; keep the saved values intact
    }

    int sub_vis = 0;
    int64_t osd = 0;
    if (mini) {
        saved_sub_visibility_.store(sub_visibility_.load(std::memory_order_acquire), std::memory_order_release);
        saved_osd_level_.store(osd_level_.load(std::memory_order_acquire), std::memory_order_release);
    } else {
        sub_vis = saved_sub_visibility_.load(std::memory_order_acquire) ? 1 : 0;
        osd = saved_osd_level_.load(std::memory_order_acquire);
    }

    const int s1 = mpv_set_property_async(mpv_, 0, "sub-visibility", MPV_FORMAT_FLAG, &sub_vis);
    const int s2 = mpv_set_property_async(mpv_, 0, "osd-level", MPV_FORMAT_INT64, &osd);
    const int status = s1 < 0 ? s1 : s2;
    if (status < 0) return std::unexpected(PlayerError{status, mpv_error_string(status)});
    return {};
}

void Player::set_pending_resume(double seconds) noexcept {
    pending_resume_sec_.store(seconds, std::memory_order_release);
}

void Player::clear_pending_resume() noexcept {
    pending_resume_sec_.store(-1.0, std::memory_order_release);
}

double Player::duration() const noexcept {
    return duration_.load();
}

double Player::time_pos() const noexcept {
    return time_pos_.load();
}

double Player::volume() const noexcept {
    return volume_.load();
}

bool Player::is_muted() const noexcept {
    return muted_.load();
}

bool Player::is_buffering() const noexcept {
    return buffering_.load();
}

void Player::update_playback_state(PlaybackState next_state, bool force_notify) {
    const PlaybackState prev = state_.exchange(next_state, std::memory_order_acq_rel);
    recompute_buffering_state();
    if (prev == next_state && !force_notify) {
        return;
    }
    std::function<void(PlaybackState)> cb;
    {
        std::lock_guard lock(mutex_);
        cb = callbacks_.on_state_changed;
    }
    if (cb) {
        cb(next_state);
    }
}

void Player::set_buffering_state(bool buffering) {
    if (buffering_.exchange(buffering) == buffering) {
        return;
    }
    std::function<void(bool)> cb;
    {
        std::lock_guard lock(mutex_);
        cb = callbacks_.on_buffering_changed;
    }
    if (cb) {
        cb(buffering);
    }
}

void Player::recompute_buffering_state() {
    PlaybackState st = state_.load();
    bool next = opening_.load()
             || (st != PlaybackState::Idle && (
                    seeking_.load()
                 || paused_for_cache_.load()
                 || (st == PlaybackState::Playing && core_idle_.load())
                ));
    set_buffering_state(next);
}

PlaybackState Player::state() const noexcept {
    return state_.load();
}

bool Player::is_paused() const noexcept {
    return paused_.load(std::memory_order_acquire);
}

bool Player::is_idle_active() const noexcept {
    return idle_active_.load(std::memory_order_acquire);
}

std::string Player::media_title() const {
    std::lock_guard lock(mutex_);
    return media_title_;
}

VideoDisplaySize Player::video_display_size() const noexcept {
    const uint64_t packed = video_size_.load(std::memory_order_acquire);
    return VideoDisplaySize{static_cast<int>(packed >> 32), static_cast<int>(packed & 0xFFFFFFFFu)};
}

void Player::publish_video_size(VideoDisplaySize size, bool notify_if_unchanged) {
    const uint64_t packed = (static_cast<uint64_t>(static_cast<uint32_t>(size.width)) << 32) |
                            static_cast<uint32_t>(size.height);
    const bool changed = video_size_.exchange(packed, std::memory_order_acq_rel) != packed;
    if (!changed && !notify_if_unchanged) return;
    std::function<void(VideoDisplaySize)> cb;
    {
        std::lock_guard lock(mutex_);
        cb = callbacks_.on_video_size;
    }
    if (cb) cb(size);
}

void Player::set_callbacks(PlayerCallbacks callbacks) {
    std::lock_guard lock(mutex_);
    callbacks_ = std::move(callbacks);
}

void Player::event_loop() {
    while (running_.load()) {
        mpv_event* event = mpv_wait_event(mpv_, 0.05);
        if (!event || event->event_id == MPV_EVENT_NONE) {
            continue;
        }

        if (event->event_id == MPV_EVENT_SHUTDOWN) {
            break;
        }

        handle_mpv_event(event);
    }
}

void Player::handle_mpv_event(void* event_ptr) {
    auto* event = static_cast<mpv_event*>(event_ptr);
    switch (event->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE: {
            auto* prop = static_cast<mpv_event_property*>(event->data);
            if (!prop || !prop->name) break;

            std::string_view name(prop->name);

            if (name == "time-pos") {
                if (prop->format == MPV_FORMAT_DOUBLE && prop->data) {
                    double val = *static_cast<double*>(prop->data);
                    time_pos_.store(val);
                    std::function<void(double)> cb;
                    {
                        std::lock_guard lock(mutex_);
                        cb = callbacks_.on_time_pos;
                    }
                    if (cb) cb(val);
                }
            } else if (name == "duration") {
                if (prop->format == MPV_FORMAT_DOUBLE && prop->data) {
                    double val = *static_cast<double*>(prop->data);
                    duration_.store(val);
                    std::function<void(double)> cb;
                    {
                        std::lock_guard lock(mutex_);
                        cb = callbacks_.on_duration;
                    }
                    if (cb) cb(val);
                }
            } else if (name == "pause") {
                if (prop->format == MPV_FORMAT_FLAG && prop->data) {
                    bool is_paused = *static_cast<int*>(prop->data) != 0;
                    paused_.store(is_paused, std::memory_order_release);
                    if (file_loaded_.load(std::memory_order_acquire)) {
                        update_playback_state(is_paused ? PlaybackState::Paused : PlaybackState::Playing);
                    }
                }
            } else if (name == "eof-reached") {
                if (prop->format == MPV_FORMAT_FLAG && prop->data) {
                    bool eof = *static_cast<int*>(prop->data) != 0;
                    eof_reached_.store(eof, std::memory_order_release);
                }
            } else if (name == "volume") {
                if (prop->format == MPV_FORMAT_DOUBLE && prop->data) {
                    double val = *static_cast<double*>(prop->data);
                    volume_.store(val);
                    std::function<void(double)> cb;
                    {
                        std::lock_guard lock(mutex_);
                        cb = callbacks_.on_volume;
                    }
                    if (cb) cb(val);
                }
            } else if (name == "mute") {
                if (prop->format == MPV_FORMAT_FLAG && prop->data) {
                    bool val = *static_cast<int*>(prop->data) != 0;
                    muted_.store(val);
                    std::function<void(bool)> cb;
                    {
                        std::lock_guard lock(mutex_);
                        cb = callbacks_.on_mute;
                    }
                    if (cb) cb(val);
                }
            } else if (name == "media-title") {
                if (prop->format == MPV_FORMAT_STRING && prop->data) {
                    std::string title = *static_cast<char**>(prop->data);
                    std::function<void(const std::string&)> cb;
                    {
                        std::lock_guard lock(mutex_);
                        media_title_ = title;
                        cb = callbacks_.on_media_title;
                    }
                    if (cb) cb(title);
                }
            } else if (name == "idle-active") {
                if (prop->format == MPV_FORMAT_FLAG && prop->data) {
                    bool is_idle = *static_cast<int*>(prop->data) != 0;
                    idle_active_.store(is_idle, std::memory_order_release);
                    if (is_idle) {
                        file_loaded_.store(false, std::memory_order_release);
                        eof_reached_.store(false, std::memory_order_release);
                        opening_.store(false);
                        seeking_.store(false);
                        core_idle_.store(false);
                        paused_for_cache_.store(false);
                        // Force a notification: END_FILE usually reported Idle already, and
                        // listeners need this second one to tell "playlist over" (is_idle_active())
                        // from "between playlist entries".
                        update_playback_state(PlaybackState::Idle, /*force_notify=*/true);
                        set_buffering_state(false);
                    }
                }
            } else if (name == "paused-for-cache") {
                if (prop->format == MPV_FORMAT_FLAG && prop->data) {
                    bool caching = *static_cast<int*>(prop->data) != 0;
                    paused_for_cache_.store(caching);
                    recompute_buffering_state();
                }
            } else if (name == "seeking") {
                if (prop->format == MPV_FORMAT_FLAG && prop->data) {
                    bool is_seeking = *static_cast<int*>(prop->data) != 0;
                    seeking_.store(is_seeking);
                    recompute_buffering_state();
                }
            } else if (name == "core-idle") {
                if (prop->format == MPV_FORMAT_FLAG && prop->data) {
                    bool idle = *static_cast<int*>(prop->data) != 0;
                    core_idle_.store(idle);
                    recompute_buffering_state();
                }
            } else if (name == "track-list") {
                if (prop->format == MPV_FORMAT_NODE && prop->data) {
                    auto parsed = parse_track_list_node(*static_cast<mpv_node*>(prop->data));
                    std::function<void(const std::vector<MediaTrack>&)> cb;
                    {
                        std::lock_guard lock(mutex_);
                        tracks_ = parsed;
                        cb = callbacks_.on_tracks_changed;
                    }
                    if (cb) cb(parsed);
                }
            } else if (name == "chapter-list") {
                if (prop->format == MPV_FORMAT_NODE && prop->data) {
                    auto parsed = parse_chapter_list_node(*static_cast<mpv_node*>(prop->data));
                    std::function<void(const std::vector<Chapter>&)> cb;
                    {
                        std::lock_guard lock(mutex_);
                        chapters_ = parsed;
                        cb = callbacks_.on_chapters_changed;
                    }
                    if (cb) cb(parsed);
                }
            } else if (name == "chapter") {
                if (prop->format == MPV_FORMAT_INT64 && prop->data) {
                    int idx = static_cast<int>(*static_cast<int64_t*>(prop->data));
                    current_chapter_.store(idx);
                    std::function<void(int)> cb;
                    {
                        std::lock_guard lock(mutex_);
                        cb = callbacks_.on_chapter_changed;
                    }
                    if (cb) cb(idx);
                }
            } else if (name == "loop-file") {
                if (prop->format == MPV_FORMAT_NODE && prop->data) {
                    const auto* node = static_cast<mpv_node*>(prop->data);
                    bool active = (node->format == MPV_FORMAT_STRING && node->u.string && std::string_view(node->u.string) == "inf")
                               || (node->format == MPV_FORMAT_FLAG && node->u.flag != 0);
                    if (active && loop_mode_.load() != LoopMode::File) {
                        loop_mode_.store(LoopMode::File);
                        std::function<void(LoopMode)> cb;
                        {
                            std::lock_guard lock(mutex_);
                            cb = callbacks_.on_loop_mode_changed;
                        }
                        if (cb) cb(LoopMode::File);
                    }
                }
            } else if (name == "loop-playlist") {
                if (prop->format == MPV_FORMAT_NODE && prop->data) {
                    const auto* node = static_cast<mpv_node*>(prop->data);
                    bool active = (node->format == MPV_FORMAT_STRING && node->u.string && std::string_view(node->u.string) == "inf")
                               || (node->format == MPV_FORMAT_FLAG && node->u.flag != 0);
                    if (active && loop_mode_.load() != LoopMode::Playlist) {
                        loop_mode_.store(LoopMode::Playlist);
                        std::function<void(LoopMode)> cb;
                        {
                            std::lock_guard lock(mutex_);
                            cb = callbacks_.on_loop_mode_changed;
                        }
                        if (cb) cb(LoopMode::Playlist);
                    }
                }
            } else if (name == "ab-loop-a") {
                if (prop->format == MPV_FORMAT_NODE && prop->data) {
                    const auto* node = static_cast<mpv_node*>(prop->data);
                    std::function<void(AbLoopState, std::optional<double>, std::optional<double>)> cb;
                    AbLoopState st;
                    std::optional<double> a_opt;
                    std::optional<double> b_opt;
                    {
                        std::lock_guard lock(mutex_);
                        if (node->format == MPV_FORMAT_DOUBLE) {
                            ab_loop_a_ = node->u.double_;
                        } else {
                            ab_loop_a_ = std::nullopt;
                        }
                        update_ab_loop_state_locked();
                        st = ab_loop_state_.load();
                        a_opt = ab_loop_a_;
                        b_opt = ab_loop_b_;
                        cb = callbacks_.on_ab_loop_changed;
                    }
                    if (cb) cb(st, a_opt, b_opt);
                }
            } else if (name == "ab-loop-b") {
                if (prop->format == MPV_FORMAT_NODE && prop->data) {
                    const auto* node = static_cast<mpv_node*>(prop->data);
                    std::function<void(AbLoopState, std::optional<double>, std::optional<double>)> cb;
                    AbLoopState st;
                    std::optional<double> a_opt;
                    std::optional<double> b_opt;
                    {
                        std::lock_guard lock(mutex_);
                        if (node->format == MPV_FORMAT_DOUBLE) {
                            ab_loop_b_ = node->u.double_;
                        } else {
                            ab_loop_b_ = std::nullopt;
                        }
                        update_ab_loop_state_locked();
                        st = ab_loop_state_.load();
                        a_opt = ab_loop_a_;
                        b_opt = ab_loop_b_;
                        cb = callbacks_.on_ab_loop_changed;
                    }
                    if (cb) cb(st, a_opt, b_opt);
                }
            } else if (name == "sub-visibility") {
                if (prop->format == MPV_FORMAT_FLAG && prop->data) {
                    sub_visibility_.store(*static_cast<int*>(prop->data) != 0, std::memory_order_release);
                }
            } else if (name == "osd-level") {
                if (prop->format == MPV_FORMAT_INT64 && prop->data) {
                    osd_level_.store(*static_cast<int64_t*>(prop->data), std::memory_order_release);
                }
            } else if (name == "speed") {
                if (prop->format == MPV_FORMAT_DOUBLE && prop->data) {
                    double s = *static_cast<double*>(prop->data);
                    speed_.store(s);
                    std::function<void(double)> cb;
                    {
                        std::lock_guard lock(mutex_);
                        cb = callbacks_.on_speed_changed;
                    }
                    if (cb) cb(s);
                }
            } else if (name == "playlist-pos") {
                if (prop->format == MPV_FORMAT_INT64 && prop->data) {
                    playlist_pos_.store(*static_cast<int64_t*>(prop->data), std::memory_order_release);
                }
            } else if (name == "video-out-params") {
                // MPV_FORMAT_NONE (no video) maps to {0, 0}.
                VideoDisplaySize size{};
                if (prop->format == MPV_FORMAT_NODE && prop->data) {
                    size = parse_video_out_params_node(*static_cast<mpv_node*>(prop->data));
                }
                publish_video_size(size, /*notify_if_unchanged=*/false);
            }
            break;
        }

        case MPV_EVENT_START_FILE: {
            file_loaded_.store(false, std::memory_order_release);
            eof_reached_.store(false, std::memory_order_release);
            paused_for_cache_.store(false);
            seeking_.store(false);
            opening_.store(true);
            recompute_buffering_state();
            break;
        }

        case MPV_EVENT_FILE_LOADED: {
            const double resume_target = pending_resume_sec_.exchange(-1.0, std::memory_order_acq_rel);
            if (resume_target > 0.0) {
                (void)seek(resume_target, true);
            }
            file_loaded_.store(true, std::memory_order_release);
            eof_reached_.store(false, std::memory_order_release);
            const PlaybackState loaded_state =
                paused_.load(std::memory_order_acquire) ? PlaybackState::Paused : PlaybackState::Playing;
            update_playback_state(loaded_state, /*force_notify=*/true);
            std::function<void(const std::string&)> loaded_cb;
            std::string title;
            {
                std::lock_guard lock(mutex_);
                loaded_cb = callbacks_.on_file_loaded;
                title = media_title_;
            }
            if (loaded_cb) loaded_cb(title);
            break;
        }

        case MPV_EVENT_SEEK: {
            seeking_.store(true);
            recompute_buffering_state();
            break;
        }

        case MPV_EVENT_PLAYBACK_RESTART: {
            opening_.store(false);
            seeking_.store(false);
            recompute_buffering_state();
            break;
        }

        case MPV_EVENT_END_FILE: {
            file_loaded_.store(false, std::memory_order_release);
            eof_reached_.store(false, std::memory_order_release);
            time_pos_.store(0.0);
            opening_.store(false);
            seeking_.store(false);
            core_idle_.store(false);
            paused_for_cache_.store(false);
            update_playback_state(PlaybackState::Idle);
            set_buffering_state(false);
            std::function<void(double)> pos_cb;
            {
                std::lock_guard lock(mutex_);
                pos_cb = callbacks_.on_time_pos;
            }
            if (pos_cb) pos_cb(0.0);
            break;
        }

        default:
            break;
    }
}

} // namespace mpvx
