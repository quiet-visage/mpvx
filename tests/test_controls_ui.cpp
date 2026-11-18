#include "app-window.h"
#include "mpvx/core.hpp"

#include <private/slint_testing_internal.h>

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <vector>

namespace {

void ensure_slint_testing_backend() {
    static const bool initialized = []() {
        slint::cbindgen_private::slint_testing_init_backend();
        return true;
    }();
    (void)initialized;
}

} // namespace

TEST_CASE("MainWindow UI properties and control callbacks", "[ui][controls]") {
    ensure_slint_testing_backend();
    auto ui = MainWindow::create();

    // 1. Verify initial state matches specification
    REQUIRE(ui->get_hud_visible() == true);
    REQUIRE(ui->get_is_buffering() == false);
    REQUIRE(ui->get_loop_mode() == LoopMode::Off);
    REQUIRE(ui->get_ab_loop_state() == AbLoopState::Off);
    REQUIRE(ui->get_active_popover() == ActivePopover::None);
    REQUIRE(ui->get_time_text() == "0:00 | 0:00");
    REQUIRE(ui->get_volume() == 100.0f);
    REQUIRE(ui->get_is_muted() == false);
    REQUIRE(ui->get_playback_speed() == 1.0f);
    REQUIRE(ui->get_hwdec_mode() == "auto-safe");
    REQUIRE(ui->get_aspect_ratio() == "auto");

    // 2. Verify callback wiring (Play/Pause, Prev/Next, Seek, Volume, A/B Loop, Track Selection, Settings, Window Controls)
    bool play_clicked = false;
    bool prev_clicked = false;
    bool next_clicked = false;
    float last_seek = -1.0f;
    float last_rel_seek = 0.0f;
    float last_vol = -1.0f;
    bool mute_toggled = false;
    bool ab_cycled = false;
    bool ab_cleared = false;
    bool loop_cycled = false;
    int selected_sub = -99;
    int selected_audio = -99;
    int selected_chapter = -99;
    float last_speed = 0.0f;
    std::string last_hwdec;
    std::string last_aspect;
    std::string submitted_url;
    bool fullscreen_toggled = false;
    bool maximize_toggled = false;
    bool minimize_clicked = false;
    bool close_clicked = false;

    ui->on_play_pause_clicked([&]() { play_clicked = true; });
    ui->on_prev_clicked([&]() { prev_clicked = true; });
    ui->on_next_clicked([&]() { next_clicked = true; });
    ui->on_seek_requested([&](float pos) { last_seek = pos; });
    ui->on_relative_seek_requested([&](float delta) { last_rel_seek = delta; });
    ui->on_volume_changed([&](float vol) { last_vol = vol; });
    ui->on_mute_toggled([&]() { mute_toggled = true; });
    ui->on_ab_loop_cycled([&]() { ab_cycled = true; });
    ui->on_ab_loop_cleared([&]() { ab_cleared = true; });
    ui->on_loop_mode_cycled([&]() { loop_cycled = true; });
    ui->on_subtitle_track_selected([&](int id) { selected_sub = id; });
    ui->on_audio_track_selected([&](int id) { selected_audio = id; });
    ui->on_chapter_selected([&](int idx) { selected_chapter = idx; });
    ui->on_speed_changed([&](float s) { last_speed = s; });
    ui->on_hwdec_changed([&](slint::SharedString m) { last_hwdec = std::string(m); });
    ui->on_aspect_changed([&](slint::SharedString a) { last_aspect = std::string(a); });
    ui->on_open_url_submitted([&](slint::SharedString u) { submitted_url = std::string(u); });
    ui->on_fullscreen_toggled([&]() { fullscreen_toggled = true; });
    ui->on_maximize_toggled([&]() { maximize_toggled = true; });
    ui->on_minimize_clicked([&]() { minimize_clicked = true; });
    ui->on_close_clicked([&]() { close_clicked = true; });

    ui->invoke_play_pause_clicked();
    ui->invoke_prev_clicked();
    ui->invoke_next_clicked();
    ui->invoke_seek_requested(0.42f);
    ui->invoke_relative_seek_requested(-5.0f);
    ui->invoke_volume_changed(65.0f);
    ui->invoke_mute_toggled();
    ui->invoke_ab_loop_cycled();
    ui->invoke_ab_loop_cleared();
    ui->invoke_loop_mode_cycled();
    ui->invoke_subtitle_track_selected(-1);
    ui->invoke_audio_track_selected(2);
    ui->invoke_chapter_selected(3);
    ui->invoke_speed_changed(1.5f);
    ui->invoke_hwdec_changed(slint::SharedString("vaapi"));
    ui->invoke_aspect_changed(slint::SharedString("16:9"));
    ui->invoke_open_url_submitted(slint::SharedString("https://example.com/stream.mp4"));
    ui->invoke_fullscreen_toggled();
    ui->invoke_maximize_toggled();
    ui->invoke_minimize_clicked();
    ui->invoke_close_clicked();

    REQUIRE(play_clicked);
    REQUIRE(prev_clicked);
    REQUIRE(next_clicked);
    REQUIRE(last_seek == 0.42f);
    REQUIRE(last_rel_seek == -5.0f);
    REQUIRE(last_vol == 65.0f);
    REQUIRE(mute_toggled);
    REQUIRE(ab_cycled);
    REQUIRE(ab_cleared);
    REQUIRE(loop_cycled);
    REQUIRE(selected_sub == -1);
    REQUIRE(selected_audio == 2);
    REQUIRE(selected_chapter == 3);
    REQUIRE(last_speed == 1.5f);
    REQUIRE(last_hwdec == "vaapi");
    REQUIRE(last_aspect == "16:9");
    REQUIRE(submitted_url == "https://example.com/stream.mp4");
    REQUIRE(fullscreen_toggled);
    REQUIRE(maximize_toggled);
    REQUIRE(minimize_clicked);
    REQUIRE(close_clicked);

    // 3. Verify TrackItem & ChapterItem VectorModel population and Multi-Track Subtitle Cycling
    auto subs = std::make_shared<slint::VectorModel<TrackItem>>(std::vector<TrackItem>{
        TrackItem{1, slint::SharedString("English [ass]"), slint::SharedString("en"), slint::SharedString("ass"), false},
        TrackItem{2, slint::SharedString("Japanese [subrip]"), slint::SharedString("ja"), slint::SharedString("subrip"), false}
    });
    ui->set_subtitle_tracks(subs);
    ui->set_has_subtitles_enabled(false);
    ui->set_active_subtitle_index(-1);
    REQUIRE(ui->get_subtitle_tracks()->row_count() == 2);

    // Cycle C shortcut: Off -> Track 1 (id=1) -> Track 2 (id=2) -> Off (id=-1)
    ui->invoke_cycle_subtitles();
    REQUIRE(selected_sub == 1);
    REQUIRE(ui->get_has_subtitles_enabled() == true);
    REQUIRE(ui->get_active_subtitle_index() == 0);

    ui->invoke_cycle_subtitles();
    REQUIRE(selected_sub == 2);
    REQUIRE(ui->get_has_subtitles_enabled() == true);
    REQUIRE(ui->get_active_subtitle_index() == 1);

    ui->invoke_cycle_subtitles();
    REQUIRE(selected_sub == -1);
    REQUIRE(ui->get_has_subtitles_enabled() == false);
    REQUIRE(ui->get_active_subtitle_index() == -1);

    // Verify viewport-resized callback wiring
    int resized_w = 0;
    int resized_h = 0;
    ui->on_viewport_resized([&](int w, int h) {
        resized_w = w;
        resized_h = h;
    });
    ui->invoke_viewport_resized(1920, 1080);
    REQUIRE(resized_w == 1920);
    REQUIRE(resized_h == 1080);

    auto chapters = std::make_shared<slint::VectorModel<ChapterItem>>(std::vector<ChapterItem>{
        ChapterItem{0, slint::SharedString("Intro"), slint::SharedString("0:00"), 0.0f, true},
        ChapterItem{1, slint::SharedString("Main Act"), slint::SharedString("5:30"), 0.25f, false}
    });
    ui->set_chapters(chapters);
    REQUIRE(ui->get_chapters()->row_count() == 2);

    // Verify ActivePopover & A/B loop state bindings
    ui->set_active_popover(ActivePopover::Settings);
    REQUIRE(ui->get_active_popover() == ActivePopover::Settings);

    ui->set_ab_loop_state(AbLoopState::Active);
    ui->set_ab_loop_a_norm(0.2f);
    ui->set_ab_loop_b_norm(0.7f);
    REQUIRE(ui->get_ab_loop_state() == AbLoopState::Active);
    REQUIRE(ui->get_ab_loop_a_norm() == 0.2f);
    REQUIRE(ui->get_ab_loop_b_norm() == 0.7f);
}

TEST_CASE("MainWindow HUD auto-hide timer and lock predicate", "[ui][hud]") {
    ensure_slint_testing_backend();
    auto ui = MainWindow::create();

    // While idle / paused, HUD remains locked visible even as time elapses
    REQUIRE(ui->get_hud_visible() == true);
    slint::cbindgen_private::slint_mock_elapsed_time(3000);
    REQUIRE(ui->get_hud_visible() == true);

    // Start video playback with no popovers open -> HUD should auto-hide after 2500ms
    ui->set_has_video(true);
    ui->set_is_playing(true);
    ui->set_active_popover(ActivePopover::None);

    for (int i = 0; i < 12; ++i) {
        slint::cbindgen_private::slint_mock_elapsed_time(250);
    }
    REQUIRE(ui->get_pointer_idle_ms() >= 2500);
    REQUIRE(ui->get_hud_visible() == false);

    // Opening a popover locks HUD back to visible
    ui->set_active_popover(ActivePopover::Settings);
    slint::cbindgen_private::slint_mock_elapsed_time(250);
    REQUIRE(ui->get_hud_visible() == true);
    REQUIRE(ui->get_pointer_idle_ms() == 0);

    // Closing popover and entering buffering state also locks HUD visible
    ui->set_active_popover(ActivePopover::None);
    ui->set_is_buffering(true);
    for (int i = 0; i < 12; ++i) {
        slint::cbindgen_private::slint_mock_elapsed_time(250);
    }
    REQUIRE(ui->get_hud_visible() == true);
    REQUIRE(ui->get_pointer_idle_ms() == 0);
}

