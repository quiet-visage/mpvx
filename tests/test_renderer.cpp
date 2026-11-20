#include "mpvx/core.hpp"
#include "mpvx/player.hpp"
#include "mpvx/renderer.hpp"

#include <mpv/client.h>

#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

// Polls `pred` every 10ms until it returns true or `timeout` elapses.
bool wait_until(const std::function<bool()>& pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return pred();
}

struct FrameLog {
    std::mutex mutex;
    std::vector<std::pair<uint32_t, uint32_t>> sizes;

    void push(const mpvx::VideoRenderer::FrameBuffer& buf) {
        std::lock_guard lock(mutex);
        sizes.emplace_back(buf.width(), buf.height());
    }
    size_t count() {
        std::lock_guard lock(mutex);
        return sizes.size();
    }
    std::vector<std::pair<uint32_t, uint32_t>> snapshot() {
        std::lock_guard lock(mutex);
        return sizes;
    }
};

mpv_node int_node(int64_t v) {
    mpv_node n{};
    n.format = MPV_FORMAT_INT64;
    n.u.int64 = v;
    return n;
}

} // namespace

// ---------------------------------------------------------------------------
// Pure geometry helpers
// ---------------------------------------------------------------------------

TEST_CASE("fit_aspect returns the largest aspect-correct rectangle inside the destination", "[renderer][geometry]") {
    // 16:9 video in a 16:10 window -> height-limited (bars top/bottom handled by the UI)
    static_assert(mpvx::fit_aspect(1920, 1200, 16.0 / 9.0) == std::pair{1920, 1080});
    // 4:3 video in a 16:9 window -> width-limited by height
    static_assert(mpvx::fit_aspect(1920, 1080, 4.0 / 3.0) == std::pair{1440, 1080});
    // Exact match keeps the destination
    REQUIRE(mpvx::fit_aspect(1280, 720, 16.0 / 9.0) == std::pair{1280, 720});
    // Ultra-wide 2.39:1 in 16:9
    REQUIRE(mpvx::fit_aspect(1920, 1080, 2.39) == std::pair{1920, 803});
    // Portrait 9:16 in landscape
    REQUIRE(mpvx::fit_aspect(1920, 1080, 9.0 / 16.0) == std::pair{608, 1080});

    // Unknown aspect -> passthrough
    REQUIRE(mpvx::fit_aspect(1280, 720, 0.0) == std::pair{1280, 720});
    REQUIRE(mpvx::fit_aspect(1280, 720, -1.0) == std::pair{1280, 720});
    REQUIRE(mpvx::fit_aspect(1280, 720, std::nan("")) == std::pair{1280, 720});

    // Never exceeds the destination for a sweep of aspects and sizes
    for (int w : {17, 320, 333, 1280, 1366, 2560}) {
        for (int h : {9, 240, 187, 720, 768, 1440}) {
            for (double a : {0.3, 0.5625, 1.0, 1.3333, 1.7778, 2.39, 3.5}) {
                const auto [fw, fh] = mpvx::fit_aspect(w, h, a);
                REQUIRE(fw >= 1);
                REQUIRE(fh >= 1);
                REQUIRE(fw <= w);
                REQUIRE(fh <= h);
                REQUIRE((fw == w || fh == h)); // touches at least one side
            }
        }
    }
}

TEST_CASE("clamp_render_target preserves aspect when downscaling oversize targets", "[renderer][geometry]") {
    REQUIRE(mpvx::clamp_render_target(1920, 1080) == std::pair{1920, 1080});
    REQUIRE(mpvx::clamp_render_target(3840, 2160) == std::pair{3840, 2160});
    // 8K-wide 32:9 -> scaled uniformly, not squashed per axis
    REQUIRE(mpvx::clamp_render_target(7680, 2160) == std::pair{3840, 1080});
    // 5K 16:9 -> 4K 16:9
    REQUIRE(mpvx::clamp_render_target(5120, 2880) == std::pair{3840, 2160});
    // Tiny and invalid sizes are raised to the minimum side
    REQUIRE(mpvx::clamp_render_target(8, 4) == std::pair{16, 16});
    REQUIRE(mpvx::clamp_render_target(0, 100) == std::pair{16, 16});
}

// ---------------------------------------------------------------------------
// video-out-params parsing
// ---------------------------------------------------------------------------

TEST_CASE("parse_video_out_params_node extracts display size", "[player][video-size]") {
    SECTION("plain dw/dh") {
        char* keys[] = {const_cast<char*>("w"), const_cast<char*>("h"), const_cast<char*>("dw"), const_cast<char*>("dh")};
        mpv_node vals[] = {int_node(1440), int_node(1080), int_node(1920), int_node(1080)};
        mpv_node_list list{4, vals, keys};
        mpv_node root{};
        root.format = MPV_FORMAT_NODE_MAP;
        root.u.list = &list;

        const auto size = mpvx::parse_video_out_params_node(root);
        REQUIRE(size == mpvx::VideoDisplaySize{1920, 1080});
        REQUIRE(std::abs(size.aspect() - 16.0 / 9.0) < 1e-9);
    }

    SECTION("90 and 270 degree rotation swap the display size") {
        for (int64_t rot : {90, 270, -90}) {
            char* keys[] = {const_cast<char*>("dw"), const_cast<char*>("dh"), const_cast<char*>("rotate")};
            mpv_node vals[] = {int_node(1920), int_node(1080), int_node(rot)};
            mpv_node_list list{3, vals, keys};
            mpv_node root{};
            root.format = MPV_FORMAT_NODE_MAP;
            root.u.list = &list;
            REQUIRE(mpvx::parse_video_out_params_node(root) == mpvx::VideoDisplaySize{1080, 1920});
        }
    }

    SECTION("180 degree rotation keeps the display size") {
        char* keys[] = {const_cast<char*>("dw"), const_cast<char*>("dh"), const_cast<char*>("rotate")};
        mpv_node vals[] = {int_node(1920), int_node(1080), int_node(180)};
        mpv_node_list list{3, vals, keys};
        mpv_node root{};
        root.format = MPV_FORMAT_NODE_MAP;
        root.u.list = &list;
        REQUIRE(mpvx::parse_video_out_params_node(root) == mpvx::VideoDisplaySize{1920, 1080});
    }

    SECTION("missing, zero, oversized or non-map input yields {0,0}") {
        char* keys[] = {const_cast<char*>("dw")};
        mpv_node vals[] = {int_node(1920)};
        mpv_node_list list{1, vals, keys};
        mpv_node root{};
        root.format = MPV_FORMAT_NODE_MAP;
        root.u.list = &list;
        REQUIRE_FALSE(mpvx::parse_video_out_params_node(root).valid());

        char* keys2[] = {const_cast<char*>("dw"), const_cast<char*>("dh")};
        mpv_node vals2[] = {int_node(100000), int_node(1080)};
        mpv_node_list list2{2, vals2, keys2};
        root.u.list = &list2;
        REQUIRE_FALSE(mpvx::parse_video_out_params_node(root).valid());

        mpv_node none{};
        none.format = MPV_FORMAT_NONE;
        REQUIRE_FALSE(mpvx::parse_video_out_params_node(none).valid());
        REQUIRE(mpvx::VideoDisplaySize{}.aspect() == 0.0);
    }
}

TEST_CASE("Player reports video display size, including aspect override", "[player][video-size]") {
    mpvx::Player player;
    REQUIRE(player.initialize().has_value());
    mpvx::VideoRenderer renderer; // vo=libmpv needs a render context to configure video
    REQUIRE(renderer.initialize(player.handle(), 320, 240).has_value());

    std::mutex mutex;
    std::vector<mpvx::VideoDisplaySize> reported;
    mpvx::PlayerCallbacks cbs;
    cbs.on_video_size = [&](mpvx::VideoDisplaySize size) {
        std::lock_guard lock(mutex);
        reported.push_back(size);
    };
    player.set_callbacks(std::move(cbs));

    auto last_reported = [&]() {
        std::lock_guard lock(mutex);
        return reported.empty() ? mpvx::VideoDisplaySize{} : reported.back();
    };

    REQUIRE(player.load_file("av://lavfi:testsrc=size=320x240:rate=30").has_value());
    REQUIRE(wait_until([&] { return last_reported() == mpvx::VideoDisplaySize{320, 240}; }, 3000ms));
    REQUIRE(player.video_display_size() == mpvx::VideoDisplaySize{320, 240});

    REQUIRE(player.set_aspect_override("16:9").has_value());
    REQUIRE(wait_until([&] { return std::abs(last_reported().aspect() - 16.0 / 9.0) < 0.01; }, 3000ms));

    REQUIRE(player.stop().has_value());
    REQUIRE(wait_until([&] { return !last_reported().valid(); }, 3000ms));

    renderer.shutdown();
    player.shutdown();
}

// ---------------------------------------------------------------------------
// VideoRenderer: forced redraw, idle safety, buffer ring
// ---------------------------------------------------------------------------

TEST_CASE("VideoRenderer re-renders the current frame on resize while paused", "[renderer][redraw]") {
    mpvx::Player player;
    REQUIRE(player.initialize().has_value());
    mpvx::VideoRenderer renderer;
    REQUIRE(renderer.initialize(player.handle(), 320, 240).has_value());

    FrameLog log;
    renderer.set_frame_callback([&](const mpvx::VideoRenderer::FrameBuffer& buf) { log.push(buf); });

    REQUIRE(player.load_file("av://lavfi:testsrc=size=320x240:rate=30").has_value());
    REQUIRE(wait_until([&] { return log.count() >= 3; }, 3000ms));

    REQUIRE(player.pause().has_value());
    REQUIRE(wait_until([&] { return player.state() == mpvx::PlaybackState::Paused; }, 2000ms));
    std::this_thread::sleep_for(300ms); // let in-flight frames drain
    const size_t before = log.count();

    renderer.set_target_size(160, 120);
    REQUIRE(wait_until([&] { return log.count() > before; }, 1000ms));
    std::this_thread::sleep_for(300ms);

    const auto frames = log.snapshot();
    REQUIRE(frames.size() == before + 1); // exactly one forced redraw, no extra frames while paused
    REQUIRE(frames.back() == std::pair<uint32_t, uint32_t>{160, 120});

    // Setting the same size again is a no-op
    renderer.set_target_size(160, 120);
    std::this_thread::sleep_for(200ms);
    REQUIRE(log.count() == before + 1);

    renderer.shutdown();
    player.shutdown();
}

TEST_CASE("VideoRenderer never emits a frame on resize or redraw before video has played", "[renderer][redraw]") {
    mpvx::Player player;
    REQUIRE(player.initialize().has_value());
    mpvx::VideoRenderer renderer;
    REQUIRE(renderer.initialize(player.handle(), 320, 240).has_value());

    FrameLog log;
    renderer.set_frame_callback([&](const mpvx::VideoRenderer::FrameBuffer& buf) { log.push(buf); });

    renderer.set_target_size(200, 100);
    renderer.request_redraw();
    std::this_thread::sleep_for(300ms);
    REQUIRE(log.count() == 0);

    renderer.shutdown();
    player.shutdown();
}

TEST_CASE("VideoRenderer output suspended across stop: no clear frame, no resize redraw, resumes on next video",
          "[renderer][redraw]") {
    mpvx::Player player;
    REQUIRE(player.initialize().has_value());
    mpvx::VideoRenderer renderer;
    REQUIRE(renderer.initialize(player.handle(), 320, 240).has_value());

    FrameLog log;
    renderer.set_frame_callback([&](const mpvx::VideoRenderer::FrameBuffer& buf) { log.push(buf); });

    // Mirror main.cpp: suspend on idle, resume on FILE_LOADED and on a valid video size.
    mpvx::PlayerCallbacks cbs;
    cbs.on_state_changed = [&](mpvx::PlaybackState state) {
        if (state == mpvx::PlaybackState::Idle) renderer.suspend_output();
    };
    cbs.on_file_loaded = [&](const std::string&) { renderer.resume_output(mpvx::VideoRenderer::ResumeFrom::NextFrame); };
    cbs.on_video_size = [&](mpvx::VideoDisplaySize size) {
        if (size.valid()) renderer.resume_output();
    };
    player.set_callbacks(std::move(cbs));

    // Repeat to cover the timing window in which libmpv emits its post-stop clear frame.
    for (int round = 0; round < 5; ++round) {
        INFO("round " << round);
        const size_t start = log.count();
        REQUIRE(player.load_file("av://lavfi:testsrc=size=320x240:rate=30").has_value());
        REQUIRE(wait_until([&] { return log.count() >= start + 2; }, 3000ms));

        // Mirror main.cpp's stop handler: suspend *before* stopping.
        renderer.suspend_output();
        const size_t at_stop = log.count();
        REQUIRE(player.stop().has_value());
        REQUIRE(wait_until([&] { return player.state() == mpvx::PlaybackState::Idle; }, 2000ms));
        std::this_thread::sleep_for(150ms);
        REQUIRE(log.count() == at_stop); // libmpv's clear frame never reached the callback

        renderer.set_target_size(round % 2 ? 640 : 320, round % 2 ? 360 : 240);
        renderer.request_redraw();
        std::this_thread::sleep_for(150ms);
        REQUIRE(log.count() == at_stop); // nor did resize/redraw requests while idle
    }

    renderer.shutdown();
    player.shutdown();
}

TEST_CASE("VideoRenderer keeps delivering frames when a new file replaces the current one",
          "[renderer][redraw][replace]") {
    mpvx::Player player;
    REQUIRE(player.initialize().has_value());
    mpvx::VideoRenderer renderer;
    REQUIRE(renderer.initialize(player.handle(), 320, 240).has_value());

    FrameLog log;
    renderer.set_frame_callback([&](const mpvx::VideoRenderer::FrameBuffer& buf) { log.push(buf); });

    // Mirror main.cpp: suspend when playback goes idle (END_FILE of the replaced file fires
    // Idle), resume on FILE_LOADED and when a valid video size is reported.
    mpvx::PlayerCallbacks cbs;
    cbs.on_state_changed = [&](mpvx::PlaybackState state) {
        if (state == mpvx::PlaybackState::Idle) renderer.suspend_output();
    };
    cbs.on_file_loaded = [&](const std::string&) { renderer.resume_output(mpvx::VideoRenderer::ResumeFrom::NextFrame); };
    cbs.on_video_size = [&](mpvx::VideoDisplaySize size) {
        if (size.valid()) renderer.resume_output();
    };
    player.set_callbacks(std::move(cbs));

    // Same display size every time: the case where mpv's change-deduplicated
    // video-out-params observation may report nothing for the new file.
    const char* sources[] = {
        "av://lavfi:testsrc=size=320x240:rate=30",
        "av://lavfi:testsrc2=size=320x240:rate=30",
        "av://lavfi:smptebars=size=320x240:rate=30",
        "av://lavfi:testsrc=size=320x240:rate=30",
        "av://lavfi:testsrc2=size=320x240:rate=30",
    };
    for (const char* src : sources) {
        INFO("source " << src);
        const size_t before = log.count();
        REQUIRE(player.load_file(src).has_value());
        REQUIRE(wait_until([&] { return log.count() >= before + 10; }, 3000ms));
    }

    renderer.shutdown();
    player.shutdown();
}

TEST_CASE("VideoRenderer reuses ring buffers when the consumer drops frames promptly", "[renderer][ring]") {
    mpvx::Player player;
    REQUIRE(player.initialize().has_value());
    mpvx::VideoRenderer renderer;
    REQUIRE(renderer.initialize(player.handle(), 320, 240).has_value());

    renderer.set_frame_callback([](const mpvx::VideoRenderer::FrameBuffer&) {});

    REQUIRE(player.load_file("av://lavfi:testsrc=size=320x240:rate=60").has_value());
    REQUIRE(wait_until([&] { return renderer.stats().frames_rendered >= 40; }, 5000ms));

    const auto stats = renderer.stats();
    INFO("frames=" << stats.frames_rendered << " allocations=" << stats.buffer_allocations);
    REQUIRE(stats.buffer_allocations <= 3); // one per ring slot, then steady-state reuse

    renderer.shutdown();
    player.shutdown();
}

TEST_CASE("VideoRenderer never overwrites a frame the consumer still holds", "[renderer][ring]") {
    mpvx::Player player;
    REQUIRE(player.initialize().has_value());
    mpvx::VideoRenderer renderer;
    REQUIRE(renderer.initialize(player.handle(), 64, 48).has_value());

    // Retain the first few frames (as the GUI would hold an Image) together with a
    // byte-for-byte snapshot taken at delivery time.
    std::mutex mutex;
    std::vector<mpvx::VideoRenderer::FrameBuffer> retained;
    std::vector<std::vector<slint::Rgba8Pixel>> snapshots;
    renderer.set_frame_callback([&](const mpvx::VideoRenderer::FrameBuffer& buf) {
        std::lock_guard lock(mutex);
        if (retained.size() < 6) {
            retained.push_back(buf);
            snapshots.emplace_back(buf.begin(), buf.end());
        }
    });

    REQUIRE(player.load_file("av://lavfi:testsrc=size=64x48:rate=60").has_value());
    REQUIRE(wait_until([&] { return renderer.stats().frames_rendered >= 30; }, 5000ms));
    renderer.shutdown(); // stop rendering before inspecting

    std::lock_guard lock(mutex);
    REQUIRE(retained.size() == 6);
    for (size_t i = 0; i < retained.size(); ++i) {
        const auto& buf = retained[i];
        const auto& snap = snapshots[i];
        REQUIRE(static_cast<size_t>(buf.end() - buf.begin()) == snap.size());
        bool identical = true;
        for (size_t p = 0; p < snap.size(); ++p) {
            const auto& a = buf.begin()[p];
            const auto& b = snap[p];
            if (a.r != b.r || a.g != b.g || a.b != b.b || a.a != b.a) {
                identical = false;
                break;
            }
        }
        INFO("retained frame " << i);
        REQUIRE(identical);
    }
    // Holding slots forced copy-on-write detaches instead of in-place reuse.
    REQUIRE(renderer.stats().buffer_allocations > 3);

    player.shutdown();
}

// ---------------------------------------------------------------------------
// Mini player: 30 fps hand-off cap and mpv profile
// ---------------------------------------------------------------------------

namespace {

// Plays `source` and returns the frame hand-off rate measured over `window` after a 1 s warm-up.
double measure_handoff_fps(const char* source, bool cap, std::chrono::milliseconds window) {
    mpvx::Player player;
    REQUIRE(player.initialize().has_value());
    mpvx::VideoRenderer renderer;
    REQUIRE(renderer.initialize(player.handle(), 160, 120).has_value());
    std::atomic<uint64_t> delivered{0};
    renderer.set_frame_callback([&](const mpvx::VideoRenderer::FrameBuffer&) { delivered.fetch_add(1); });
    renderer.set_frame_cap(cap);

    REQUIRE(player.load_file(source).has_value());
    REQUIRE(wait_until([&] { return delivered.load() >= 5; }, 5000ms));
    std::this_thread::sleep_for(1000ms);

    const auto start_count = delivered.load();
    const auto start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(window);
    const auto end_count = delivered.load();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    renderer.shutdown();
    player.shutdown();
    return static_cast<double>(end_count - start_count) / secs;
}

} // namespace

TEST_CASE("VideoRenderer 30 fps cap halves 60 fps hand-off and keeps 30 fps sources intact",
          "[renderer][frame-cap]") {
    const double capped60 = measure_handoff_fps("av://lavfi:testsrc2=size=160x120:rate=60", true, 3000ms);
    INFO("capped 60 fps source: " << capped60);
    REQUIRE(std::abs(capped60 - 30.0) <= 1.5);

    const double capped30 = measure_handoff_fps("av://lavfi:testsrc2=size=160x120:rate=30", true, 3000ms);
    INFO("capped 30 fps source: " << capped30);
    REQUIRE(std::abs(capped30 - 30.0) <= 1.5);

    const double uncapped60 = measure_handoff_fps("av://lavfi:testsrc2=size=160x120:rate=60", false, 3000ms);
    INFO("uncapped 60 fps source: " << uncapped60);
    REQUIRE(std::abs(uncapped60 - 60.0) <= 1.5);
}

TEST_CASE("Player mini profile hides subs/OSD and restores the user's prior values", "[player][mini]") {
    mpvx::Player player;
    REQUIRE(player.initialize().has_value());
    mpv_handle* mpv = player.handle();

    auto get_flag = [&](const char* name) {
        int v = -1;
        REQUIRE(mpv_get_property(mpv, name, MPV_FORMAT_FLAG, &v) >= 0);
        return v;
    };
    auto get_int = [&](const char* name) {
        int64_t v = -1;
        REQUIRE(mpv_get_property(mpv, name, MPV_FORMAT_INT64, &v) >= 0);
        return v;
    };

    // User preference: subtitles hidden, OSD level 2.
    REQUIRE(mpv_set_property_string(mpv, "sub-visibility", "no") >= 0);
    REQUIRE(mpv_set_property_string(mpv, "osd-level", "2") >= 0);
    std::this_thread::sleep_for(200ms); // let the property observers catch up

    REQUIRE(player.set_mini_profile(true).has_value());
    REQUIRE(wait_until([&] { return get_int("osd-level") == 0; }, 2000ms));
    REQUIRE(get_flag("sub-visibility") == 0);

    // Re-entering must not overwrite the saved values with the mini ones.
    REQUIRE(player.set_mini_profile(true).has_value());
    std::this_thread::sleep_for(200ms);

    REQUIRE(player.set_mini_profile(false).has_value());
    REQUIRE(wait_until([&] { return get_int("osd-level") == 2; }, 2000ms));
    REQUIRE(get_flag("sub-visibility") == 0);

    // And a visible-subtitles user gets them back.
    REQUIRE(mpv_set_property_string(mpv, "sub-visibility", "yes") >= 0);
    std::this_thread::sleep_for(200ms);
    REQUIRE(player.set_mini_profile(true).has_value());
    REQUIRE(wait_until([&] { return get_flag("sub-visibility") == 0; }, 2000ms));
    REQUIRE(player.set_mini_profile(false).has_value());
    REQUIRE(wait_until([&] { return get_flag("sub-visibility") == 1; }, 2000ms));

    player.shutdown();
}
