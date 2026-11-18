#include "mpvx/core.hpp"
#include "mpvx/player.hpp"
#include "mpvx/renderer.hpp"

#include <mpv/client.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace {

mpv_node make_int64_node(int64_t val) {
    mpv_node n{};
    n.format = MPV_FORMAT_INT64;
    n.u.int64 = val;
    return n;
}

mpv_node make_double_node(double val) {
    mpv_node n{};
    n.format = MPV_FORMAT_DOUBLE;
    n.u.double_ = val;
    return n;
}

mpv_node make_flag_node(bool val) {
    mpv_node n{};
    n.format = MPV_FORMAT_FLAG;
    n.u.flag = val ? 1 : 0;
    return n;
}

mpv_node make_string_node(const char* str) {
    mpv_node n{};
    n.format = MPV_FORMAT_STRING;
    n.u.string = const_cast<char*>(str);
    return n;
}

} // namespace

TEST_CASE("PlaybackState string conversions", "[core]") {
    REQUIRE(mpvx::playback_state_to_string(mpvx::PlaybackState::Idle) == "Idle");
    REQUIRE(mpvx::playback_state_to_string(mpvx::PlaybackState::Playing) == "Playing");
    REQUIRE(mpvx::playback_state_to_string(mpvx::PlaybackState::Paused) == "Paused");
}

TEST_CASE("Time readout formatting", "[core][time]") {
    REQUIRE(mpvx::format_timestamp(0.0) == "0:00");
    REQUIRE(mpvx::format_timestamp(65.0) == "1:05");
    REQUIRE(mpvx::format_timestamp(2582.0) == "43:02");
    REQUIRE(mpvx::format_timestamp(3725.0) == "1:02:05");
    REQUIRE(mpvx::format_timestamp(-12.5) == "0:00");
    REQUIRE(mpvx::format_timestamp(std::numeric_limits<double>::quiet_NaN()) == "0:00");
    REQUIRE(mpvx::format_time_readout(0.0, 2582.0) == "0:00 | 43:02");
    REQUIRE(mpvx::format_time_readout(65.0, 3725.0) == "1:05 | 1:02:05");
}

TEST_CASE("mpv_node track-list parser", "[player][tracks]") {
    // Build synthetic mpv_node tree: 1 video, 2 audio (en, ja), 2 sub (en selected, external srt)
    const char* keys_v[] = {"id", "type", "codec", "selected"};
    mpv_node vals_v[] = {
        make_int64_node(1),
        make_string_node("video"),
        make_string_node("hevc"),
        make_flag_node(true)
    };
    mpv_node_list map_v{4, vals_v, const_cast<char**>(keys_v)};

    const char* keys_a1[] = {"id", "type", "title", "lang", "codec", "selected"};
    mpv_node vals_a1[] = {
        make_int64_node(1),
        make_string_node("audio"),
        make_string_node("Surround 5.1"),
        make_string_node("en"),
        make_string_node("eac3"),
        make_flag_node(true)
    };
    mpv_node_list map_a1{6, vals_a1, const_cast<char**>(keys_a1)};

    const char* keys_a2[] = {"id", "type", "lang", "codec", "selected"};
    mpv_node vals_a2[] = {
        make_int64_node(2),
        make_string_node("audio"),
        make_string_node("ja"),
        make_string_node("flac"),
        make_flag_node(false)
    };
    mpv_node_list map_a2{5, vals_a2, const_cast<char**>(keys_a2)};

    const char* keys_s1[] = {"id", "type", "title", "lang", "codec", "selected", "external"};
    mpv_node vals_s1[] = {
        make_int64_node(1),
        make_string_node("sub"),
        make_string_node("Dialogue"),
        make_string_node("en"),
        make_string_node("ass"),
        make_flag_node(true),
        make_flag_node(false)
    };
    mpv_node_list map_s1{7, vals_s1, const_cast<char**>(keys_s1)};

    const char* keys_s2[] = {"id", "type", "codec", "selected", "external"};
    mpv_node vals_s2[] = {
        make_int64_node(2),
        make_string_node("sub"),
        make_string_node("subrip"),
        make_flag_node(false),
        make_flag_node(true)
    };
    mpv_node_list map_s2{5, vals_s2, const_cast<char**>(keys_s2)};

    mpv_node entries[5]{};
    entries[0].format = MPV_FORMAT_NODE_MAP;
    entries[0].u.list = &map_v;
    entries[1].format = MPV_FORMAT_NODE_MAP;
    entries[1].u.list = &map_a1;
    entries[2].format = MPV_FORMAT_NODE_MAP;
    entries[2].u.list = &map_a2;
    entries[3].format = MPV_FORMAT_NODE_MAP;
    entries[3].u.list = &map_s1;
    entries[4].format = MPV_FORMAT_NODE_MAP;
    entries[4].u.list = &map_s2;

    mpv_node_list root_list{5, entries, nullptr};
    mpv_node root_node{};
    root_node.format = MPV_FORMAT_NODE_ARRAY;
    root_node.u.list = &root_list;

    auto tracks = mpvx::parse_track_list_node(root_node);
    REQUIRE(tracks.size() == 5);

    REQUIRE(tracks[1].type == "audio");
    REQUIRE(tracks[1].id == 1);
    REQUIRE(tracks[1].selected == true);
    REQUIRE(tracks[1].display_label() == "Surround 5.1 (en) [eac3]");

    REQUIRE(tracks[2].type == "audio");
    REQUIRE(tracks[2].id == 2);
    REQUIRE(tracks[2].selected == false);
    REQUIRE(tracks[2].display_label() == "Track 2 (ja) [flac]");

    REQUIRE(tracks[3].type == "sub");
    REQUIRE(tracks[3].selected == true);
    REQUIRE(tracks[3].external == false);
    REQUIRE(tracks[3].display_label() == "Dialogue (en) [ass]");

    REQUIRE(tracks[4].type == "sub");
    REQUIRE(tracks[4].selected == false);
    REQUIRE(tracks[4].external == true);
    REQUIRE(tracks[4].display_label() == "Track 2 [subrip]");
}

TEST_CASE("mpv_node chapter-list parser", "[player][chapters]") {
    const char* keys_c1[] = {"title", "time"};
    mpv_node vals_c1[] = {
        make_string_node("Prologue"),
        make_double_node(0.0)
    };
    mpv_node_list map_c1{2, vals_c1, const_cast<char**>(keys_c1)};

    const char* keys_c2[] = {"title", "time"};
    mpv_node vals_c2[] = {
        make_string_node(""),
        make_double_node(142.5)
    };
    mpv_node_list map_c2{2, vals_c2, const_cast<char**>(keys_c2)};

    mpv_node entries[2]{};
    entries[0].format = MPV_FORMAT_NODE_MAP;
    entries[0].u.list = &map_c1;
    entries[1].format = MPV_FORMAT_NODE_MAP;
    entries[1].u.list = &map_c2;

    mpv_node_list root_list{2, entries, nullptr};
    mpv_node root_node{};
    root_node.format = MPV_FORMAT_NODE_ARRAY;
    root_node.u.list = &root_list;

    auto chapters = mpvx::parse_chapter_list_node(root_node);
    REQUIRE(chapters.size() == 2);
    REQUIRE(chapters[0].index == 0);
    REQUIRE(chapters[0].title == "Prologue");
    REQUIRE(chapters[0].time == 0.0);

    REQUIRE(chapters[1].index == 1);
    REQUIRE(chapters[1].title == "Chapter 2");
    REQUIRE(chapters[1].time == 142.5);
}

TEST_CASE("Player Lifecycle and Initial State", "[player]") {
    mpvx::Player player;
    REQUIRE(player.state() == mpvx::PlaybackState::Idle);
    REQUIRE(player.time_pos() == 0.0);
    REQUIRE(player.duration() == 0.0);

    auto init_res = player.initialize();
    REQUIRE(init_res.has_value());
    REQUIRE(player.handle() != nullptr);

    // Initial state after init should remain Idle
    REQUIRE(player.state() == mpvx::PlaybackState::Idle);

    // Multiple initialize calls should be idempotent
    auto second_init = player.initialize();
    REQUIRE(second_init.has_value());

    player.shutdown();
    REQUIRE(player.handle() == nullptr);
}

TEST_CASE("Player Volume and Mute Controls", "[player]") {
    mpvx::Player player;
    auto init_res = player.initialize();
    REQUIRE(init_res.has_value());

    auto vol_res = player.set_volume(75.0);
    REQUIRE(vol_res.has_value());

    auto mute_res = player.set_mute(true);
    REQUIRE(mute_res.has_value());

    player.shutdown();
}

TEST_CASE("Player A/B Loop state machine", "[player][ab-loop]") {
    mpvx::Player player;
    REQUIRE(player.initialize().has_value());

    REQUIRE(player.ab_loop_state() == mpvx::AbLoopState::Off);
    REQUIRE(!player.ab_loop_a().has_value());
    REQUIRE(!player.ab_loop_b().has_value());

    // Step 1: Off -> SetA
    REQUIRE(player.cycle_ab_loop().has_value());
    REQUIRE(player.ab_loop_state() == mpvx::AbLoopState::SetA);
    REQUIRE(player.ab_loop_a().has_value());
    REQUIRE(!player.ab_loop_b().has_value());

    // Step 2: SetA -> Active
    REQUIRE(player.cycle_ab_loop().has_value());
    REQUIRE(player.ab_loop_state() == mpvx::AbLoopState::Active);
    REQUIRE(player.ab_loop_a().has_value());
    REQUIRE(player.ab_loop_b().has_value());
    REQUIRE(*player.ab_loop_b() > *player.ab_loop_a());

    // Step 3: Active -> Off
    REQUIRE(player.cycle_ab_loop().has_value());
    REQUIRE(player.ab_loop_state() == mpvx::AbLoopState::Off);
    REQUIRE(!player.ab_loop_a().has_value());
    REQUIRE(!player.ab_loop_b().has_value());

    // Right-click clear from SetA
    REQUIRE(player.cycle_ab_loop().has_value());
    REQUIRE(player.ab_loop_state() == mpvx::AbLoopState::SetA);
    REQUIRE(player.clear_ab_loop().has_value());
    REQUIRE(player.ab_loop_state() == mpvx::AbLoopState::Off);
    REQUIRE(!player.ab_loop_a().has_value());
    REQUIRE(!player.ab_loop_b().has_value());

    char* ab_a_str = mpv_get_property_string(player.handle(), "ab-loop-a");
    REQUIRE(ab_a_str != nullptr);
    REQUIRE(std::string(ab_a_str) == "no");
    mpv_free(ab_a_str);

    player.shutdown();
}

TEST_CASE("Player LoopMode transitions", "[player][loop]") {
    mpvx::Player player;
    REQUIRE(player.initialize().has_value());
    REQUIRE(player.loop_mode() == mpvx::LoopMode::Off);

    // Off -> File
    REQUIRE(player.set_loop_mode(mpvx::LoopMode::File).has_value());
    REQUIRE(player.loop_mode() == mpvx::LoopMode::File);
    {
        char* lf = mpv_get_property_string(player.handle(), "loop-file");
        REQUIRE(lf != nullptr);
        REQUIRE(std::string(lf) == "inf");
        mpv_free(lf);
    }

    // File -> Playlist
    REQUIRE(player.set_loop_mode(mpvx::LoopMode::Playlist).has_value());
    REQUIRE(player.loop_mode() == mpvx::LoopMode::Playlist);
    {
        char* lp = mpv_get_property_string(player.handle(), "loop-playlist");
        REQUIRE(lp != nullptr);
        REQUIRE(std::string(lp) == "inf");
        mpv_free(lp);
    }

    // Playlist -> Off
    REQUIRE(player.set_loop_mode(mpvx::LoopMode::Off).has_value());
    REQUIRE(player.loop_mode() == mpvx::LoopMode::Off);
    {
        char* lf = mpv_get_property_string(player.handle(), "loop-file");
        char* lp = mpv_get_property_string(player.handle(), "loop-playlist");
        REQUIRE(lf != nullptr);
        REQUIRE(lp != nullptr);
        REQUIRE(std::string(lf) == "no");
        REQUIRE(std::string(lp) == "no");
        mpv_free(lf);
        mpv_free(lp);
    }

    player.shutdown();
}

TEST_CASE("Player Speed, Track, and Aspect controls", "[player][settings]") {
    mpvx::Player player;
    REQUIRE(player.initialize().has_value());

    REQUIRE(player.set_speed(1.5).has_value());
    REQUIRE(std::abs(player.speed() - 1.5) < 0.001);

    REQUIRE(player.set_subtitle_track(-1).has_value());
    {
        char* sid = mpv_get_property_string(player.handle(), "sid");
        REQUIRE(sid != nullptr);
        REQUIRE(std::string(sid) == "no");
        mpv_free(sid);
    }

    REQUIRE(player.set_audio_track(1).has_value());
    REQUIRE(player.set_hwdec("auto-safe").has_value());
    REQUIRE(player.set_aspect_override("16:9").has_value());
    REQUIRE(player.set_aspect_override("auto").has_value());

    REQUIRE(player.current_chapter() == -1);
    REQUIRE(player.set_chapter(2).has_value());
    REQUIRE(player.current_chapter() == 2);

    player.shutdown();
}

TEST_CASE("Player absolute seek forward and backward", "[player][seek]") {
    // Generate a 10-second seekable PCM WAV file in /tmp (8000 Hz, mono, 8-bit unsigned PCM)
    const std::string wav_path = "/tmp/mpvx_test_seek_10s.wav";
    {
        constexpr uint32_t sample_rate = 8000;
        constexpr uint32_t num_samples = sample_rate * 10; // 10.0 seconds
        constexpr uint32_t data_size = num_samples;
        constexpr uint32_t riff_size = 36 + data_size;
        constexpr uint32_t fmt_size = 16;
        constexpr uint16_t audio_format = 1; // PCM
        constexpr uint16_t num_channels = 1;
        constexpr uint32_t byte_rate = sample_rate;
        constexpr uint16_t block_align = 1;
        constexpr uint16_t bits_per_sample = 8;

        std::ofstream out(wav_path, std::ios::binary | std::ios::trunc);
        out.write("RIFF", 4);
        out.write(reinterpret_cast<const char*>(&riff_size), 4);
        out.write("WAVEfmt ", 8);
        out.write(reinterpret_cast<const char*>(&fmt_size), 4);
        out.write(reinterpret_cast<const char*>(&audio_format), 2);
        out.write(reinterpret_cast<const char*>(&num_channels), 2);
        out.write(reinterpret_cast<const char*>(&sample_rate), 4);
        out.write(reinterpret_cast<const char*>(&byte_rate), 4);
        out.write(reinterpret_cast<const char*>(&block_align), 2);
        out.write(reinterpret_cast<const char*>(&bits_per_sample), 2);
        out.write("data", 4);
        out.write(reinterpret_cast<const char*>(&data_size), 4);
        std::vector<char> silence(data_size, static_cast<char>(0x80));
        out.write(silence.data(), static_cast<std::streamsize>(silence.size()));
    }

    mpvx::Player player;
    REQUIRE(player.initialize().has_value());
    mpv_set_property_string(player.handle(), "ao", "null");

    std::atomic<int> seek_buffering_events{0};
    mpvx::PlayerCallbacks cbs;
    cbs.on_buffering_changed = [&](bool buffering) {
        if (buffering) {
            seek_buffering_events.fetch_add(1, std::memory_order_relaxed);
        }
    };
    player.set_callbacks(std::move(cbs));

    REQUIRE(player.load_file(wav_path).has_value());

    // Wait for file to load and duration to become available
    for (int i = 0; i < 40 && (player.duration() <= 0.0 || player.is_buffering()); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    REQUIRE(player.duration() >= 9.5);
    REQUIRE(player.pause().has_value());

    int before_seek = seek_buffering_events.load();

    // 1. Seek forward to 6.0s (absolute+exact) -> must enter buffering during seek and clear on PLAYBACK_RESTART
    REQUIRE(player.seek(6.0, true).has_value());
    REQUIRE(seek_buffering_events.load() > before_seek);
    for (int i = 0; i < 40 && (std::abs(player.time_pos() - 6.0) > 0.5 || player.is_buffering()); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    REQUIRE(player.time_pos() >= 5.5);
    REQUIRE(player.time_pos() <= 6.8);
    REQUIRE(player.is_buffering() == false);

    // 2. Seek backward to 1.5s (must move backwards to ~1.5s, NOT advance by +1.5s to 7.5s)
    REQUIRE(player.seek(1.5, true).has_value());
    for (int i = 0; i < 40 && std::abs(player.time_pos() - 1.5) > 0.5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    REQUIRE(player.time_pos() >= 1.0);
    REQUIRE(player.time_pos() <= 2.2);

    player.shutdown();
}

TEST_CASE("Player Callbacks Registration and Re-entrant Query Safety", "[player][callbacks]") {
    mpvx::Player player;
    auto init_res = player.initialize();
    REQUIRE(init_res.has_value());

    int ab_callback_count = 0;
    int buffering_true_count = 0;
    int buffering_false_count = 0;
    mpvx::PlayerCallbacks cbs;
    cbs.on_buffering_changed = [&](bool buffering) {
        (void)player.is_buffering();
        if (buffering) {
            ++buffering_true_count;
        } else {
            ++buffering_false_count;
        }
    };
    cbs.on_ab_loop_changed = [&](mpvx::AbLoopState, std::optional<double>, std::optional<double>) {
        // Must not deadlock when querying Player getters inside callbacks
        (void)player.ab_loop_a();
        (void)player.ab_loop_b();
        (void)player.ab_loop_state();
        (void)player.duration();
        ++ab_callback_count;
    };
    cbs.on_loop_mode_changed = [&](mpvx::LoopMode) {
        (void)player.loop_mode();
    };
    cbs.on_speed_changed = [&](double) {
        (void)player.speed();
    };

    player.set_callbacks(std::move(cbs));
    REQUIRE(player.handle() != nullptr);

    // Trigger load_file (which invokes clear_ab_loop -> on_ab_loop_changed and sets buffering=true) and stop
    REQUIRE(player.load_file("av://lavfi:testsrc=duration=1:size=320x240:rate=15").has_value());
    REQUIRE(buffering_true_count >= 1);
    REQUIRE(player.cycle_ab_loop().has_value());
    REQUIRE(player.clear_ab_loop().has_value());
    REQUIRE(player.stop().has_value());
    REQUIRE(buffering_false_count >= 1);
    REQUIRE(ab_callback_count >= 3);

    player.shutdown();
}

TEST_CASE("VideoRenderer renders frames from vo=libmpv without blocking", "[player][renderer]") {
    mpvx::Player player;
    REQUIRE(player.initialize().has_value());

    mpvx::VideoRenderer renderer;
    REQUIRE(renderer.initialize(player.handle(), 320, 240).has_value());

    std::atomic<int> frames_rendered{0};
    renderer.set_frame_callback([&](const mpvx::VideoRenderer::FrameBuffer& buf) {
        if (buf.width() == 320 && buf.height() == 240) {
            frames_rendered.fetch_add(1, std::memory_order_relaxed);
        }
    });

    REQUIRE(player.load_file("av://lavfi:testsrc=duration=1:size=320x240:rate=15").has_value());

    for (int i = 0; i < 40 && frames_rendered.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    REQUIRE(frames_rendered.load() > 0);

    renderer.shutdown();
    player.shutdown();
}


