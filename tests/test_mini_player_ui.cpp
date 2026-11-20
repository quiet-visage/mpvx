// Headless Slint UI tests for the mini player (docs/mini-player-ui-spec.md §10.2).
// These drive MainWindow directly; the C++ state machine in main.cpp is simulated by
// handlers that assign view-mode / mini-state the way MiniPlayerController does.
#include "app-window.h"

#include <private/slint_testing_internal.h>
// The prebuilt Slint ships the testing backend (SLINT_FEATURE_TESTING) but not the
// experimental flag that gates the header-only ElementHandle wrapper. The wrapper only calls
// slint_testing_* functions that the library exports, so enable it for this file alone.
#ifndef SLINT_FEATURE_EXPERIMENTAL
#    define SLINT_FEATURE_EXPERIMENTAL
#endif
#include <slint-testing.h>

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
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

// Advances Slint's animation clock past any mini player transition and processes timers.
void settle_animations(uint64_t ms = 500) {
    slint::cbindgen_private::slint_mock_elapsed_time(ms);
}

// Animated properties are evaluated lazily: an animation starts when the property is first
// read after its binding changed. The real renderer reads them every frame; the headless
// backend does not render, so read them the way a frame would right after a state change.
void read_morph(const slint::ComponentHandle<MainWindow>& ui) {
    (void)ui->get_mini_t();
    (void)ui->get_collapse_t();
}

void read_tile_pos(const slint::ComponentHandle<MainWindow>& ui) {
    (void)ui->get_mini_tile_x();
    (void)ui->get_mini_tile_y();
}

bool near(float a, float b, float tol = 0.5f) {
    return std::abs(a - b) <= tol;
}

slint::SharedString key(std::u8string_view k) {
    return slint::SharedString(std::string_view(reinterpret_cast<const char*>(k.data()), k.size()));
}

void press(const slint::ComponentHandle<MainWindow>& ui, const slint::SharedString& text) {
    ui->window().dispatch_key_press_event(text);
    ui->window().dispatch_key_release_event(text);
}

std::optional<slint::testing::ElementHandle> find_one(const slint::ComponentHandle<MainWindow>& ui,
                                                      std::string_view label) {
    auto found = slint::testing::ElementHandle::find_by_accessible_label(ui, label);
    if (found.empty()) return std::nullopt;
    return found[0];
}

// A shown 1280x720 window with a playing video, as C++ leaves it after the shrink settles.
slint::ComponentHandle<MainWindow> make_window_in_tile(std::string title = "Episode 04") {
    ensure_slint_testing_backend();
    auto ui = MainWindow::create();
    ui->window().set_size(slint::LogicalSize({1280.0f, 720.0f}));
    ui->show();
    ui->set_has_video(true);
    ui->set_is_playing(true);
    ui->set_media_title(slint::SharedString(title));
    ui->set_view_mode(AppViewMode::Picker);
    ui->set_mini_state(MiniState::Tile);
    read_morph(ui);
    settle_animations();
    return ui;
}

} // namespace

TEST_CASE("Mini player tile geometry follows window size and video aspect", "[ui][mini]") {
    ensure_slint_testing_backend();
    auto ui = MainWindow::create();
    ui->window().set_size(slint::LogicalSize({1280.0f, 720.0f}));
    ui->show();

    SECTION("1280x720 window, 16:9 video") {
        ui->set_video_aspect(16.0f / 9.0f);
        REQUIRE(near(ui->get_mini_w(), 332.8f));
        REQUIRE(near(ui->get_mini_h(), 187.2f));
    }

    SECTION("width clamps to [280, 480]") {
        ui->window().set_size(slint::LogicalSize({640.0f, 360.0f}));
        REQUIRE(near(ui->get_mini_w(), 280.0f));
        ui->window().set_size(slint::LogicalSize({3840.0f, 2160.0f}));
        REQUIRE(near(ui->get_mini_w(), 480.0f));
    }

    SECTION("portrait video uses the height cap") {
        ui->set_video_aspect(9.0f / 16.0f);
        // mini-h = clamp(720 * 0.40, 220, 420) = 288; mini-w = 288 * 9/16 = 162
        REQUIRE(near(ui->get_mini_h(), 288.0f));
        REQUIRE(near(ui->get_mini_w(), 162.0f));
    }

    SECTION("extreme aspects are clamped to [0.5, 3.0]") {
        ui->set_video_aspect(10.0f);
        REQUIRE(near(ui->get_mini_h(), 332.8f / 3.0f));
        ui->set_video_aspect(0.1f);
        REQUIRE(near(ui->get_mini_w(), 288.0f * 0.5f));
    }

    SECTION("geometry changes notify C++") {
        int notified = 0;
        ui->on_mini_geometry_changed([&]() { ++notified; });
        ui->window().set_size(slint::LogicalSize({1600.0f, 900.0f}));
        (void)ui->get_mini_w(); // evaluate the binding so the changed handler runs
        settle_animations(16);
        REQUIRE(notified > 0);
    }
}

TEST_CASE("Mini player picker inset per state", "[ui][mini]") {
    auto ui = make_window_in_tile();
    REQUIRE(near(ui->get_picker_bottom_inset(), ui->get_mini_h() + 32.0f));

    ui->set_mini_state(MiniState::Pill);
    REQUIRE(near(ui->get_picker_bottom_inset(), 72.0f));

    ui->set_mini_state(MiniState::Hidden);
    REQUIRE(near(ui->get_picker_bottom_inset(), 0.0f));
}

TEST_CASE("Return to picker with video enters the tile", "[ui][mini]") {
    ensure_slint_testing_backend();
    auto ui = MainWindow::create();
    ui->set_view_mode(AppViewMode::Player);
    ui->set_has_video(true);
    ui->on_return_to_picker_clicked([&]() {
        ui->set_mini_state(MiniState::Tile);
        ui->set_view_mode(AppViewMode::Picker);
    });

    ui->invoke_return_to_picker_clicked();
    REQUIRE(ui->get_mini_state() == MiniState::Tile);
    REQUIRE(ui->get_view_mode() == AppViewMode::Picker);
    REQUIRE(ui->get_mini_active());

    settle_animations();
    REQUIRE(near(ui->get_mini_t(), 1.0f, 0.001f));
    REQUIRE(near(ui->get_collapse_t(), 0.0f, 0.001f));
}

TEST_CASE("Mini player tile chrome invokes expand, play/pause and minimize", "[ui][mini]") {
    auto ui = make_window_in_tile("Episode 04");

    int expanded = 0;
    int minimized = 0;
    int play_pause = 0;
    ui->on_mini_expand_clicked([&]() { ++expanded; });
    ui->on_mini_minimize_clicked([&]() {
        ++minimized;
        ui->set_mini_state(MiniState::Pill);
    });
    ui->on_play_pause_clicked([&]() { ++play_pause; });

    auto tile = find_one(ui, "Mini player: Episode 04. Activate to expand.");
    REQUIRE(tile.has_value());
    tile->invoke_accessible_default_action();
    REQUIRE(expanded == 1);

    // Buttons appear on hover: move the pointer over the tile centre.
    const float cx = 1280.0f - 16.0f - ui->get_mini_w() / 2.0f;
    const float cy = 720.0f - 16.0f - ui->get_mini_h() / 2.0f;
    ui->window().dispatch_pointer_move_event(slint::LogicalPosition({cx, cy}));
    settle_animations(200);

    auto pause_btn = find_one(ui, "Pause");
    REQUIRE(pause_btn.has_value());
    pause_btn->invoke_accessible_default_action();
    REQUIRE(play_pause == 1);
    REQUIRE(ui->get_mini_state() == MiniState::Tile); // play/pause does not expand

    auto close_btn = find_one(ui, "Pause and minimize");
    REQUIRE(close_btn.has_value());
    close_btn->invoke_accessible_default_action();
    REQUIRE(minimized == 1);

    settle_animations();
    REQUIRE(near(ui->get_collapse_t(), 1.0f, 0.001f));
    REQUIRE(near(ui->get_picker_bottom_inset(), 72.0f));
    REQUIRE(find_one(ui, "Mini player minimized: Episode 04. Activate to restore.").has_value());
}

TEST_CASE("Mini player pill invokes restore, resume and expand", "[ui][mini]") {
    auto ui = make_window_in_tile("Episode 04");
    ui->set_is_playing(false);
    ui->set_mini_state(MiniState::Pill);
    settle_animations();

    std::vector<bool> restores;
    int expanded = 0;
    ui->on_mini_restore_clicked([&](bool resume) { restores.push_back(resume); });
    ui->on_mini_expand_clicked([&]() { ++expanded; });

    auto pill = find_one(ui, "Mini player minimized: Episode 04. Activate to restore.");
    REQUIRE(pill.has_value());
    pill->invoke_accessible_default_action();

    auto resume = find_one(ui, "Resume");
    REQUIRE(resume.has_value());
    resume->invoke_accessible_default_action();

    auto expand = find_one(ui, "Expand");
    REQUIRE(expand.has_value());
    expand->invoke_accessible_default_action();

    REQUIRE(restores == std::vector<bool>{false, true});
    REQUIRE(expanded == 1);
}

TEST_CASE("Mini player keyboard: Space, Ctrl+W and Esc in the picker", "[ui][mini]") {
    auto ui = make_window_in_tile();

    int play_pause = 0;
    int minimized = 0;
    int returned = 0;
    std::vector<bool> restores;
    ui->on_play_pause_clicked([&]() { ++play_pause; });
    ui->on_mini_minimize_clicked([&]() { ++minimized; });
    ui->on_mini_restore_clicked([&](bool resume) { restores.push_back(resume); });
    ui->on_return_to_player_clicked([&]() { ++returned; });

    SECTION("Space toggles the tile, resumes from the pill") {
        press(ui, " ");
        REQUIRE(play_pause == 1);

        ui->set_mini_state(MiniState::Pill);
        press(ui, " ");
        REQUIRE(restores == std::vector<bool>{true});
        REQUIRE(play_pause == 1);
    }

    SECTION("Space is not handled while the search field is focused") {
        ui->set_mini_state(MiniState::Pill);
        press(ui, "/"); // focuses search
        press(ui, " ");
        REQUIRE(restores.empty());
        REQUIRE(play_pause == 0);
    }

    SECTION("Ctrl+W minimizes the tile") {
        const auto ctrl = key(slint::platform::key_codes::Control);
        ui->window().dispatch_key_press_event(ctrl);
        press(ui, "w");
        ui->window().dispatch_key_release_event(ctrl);
        REQUIRE(minimized == 1);
    }

    SECTION("Esc clears a search first, then expands") {
        ui->set_picker_search_query("dune");
        std::string last_search = "unset";
        ui->on_picker_search_changed([&](slint::SharedString q) { last_search = std::string(q); });

        const auto esc = key(slint::platform::key_codes::Escape);
        press(ui, esc);
        REQUIRE(ui->get_picker_search_query() == "");
        REQUIRE(last_search.empty());
        REQUIRE(returned == 0);
        REQUIRE(ui->get_mini_state() == MiniState::Tile);

        press(ui, esc);
        REQUIRE(returned == 1);
    }

    SECTION("Esc from the pill expands") {
        ui->set_mini_state(MiniState::Pill);
        press(ui, key(slint::platform::key_codes::Escape));
        REQUIRE(returned == 1);
    }
}

TEST_CASE("Mini player persists across picker navigation, search and sort", "[ui][mini]") {
    for (MiniState state : {MiniState::Tile, MiniState::Pill}) {
        auto ui = make_window_in_tile();
        ui->set_mini_state(state);

        ui->invoke_picker_nav_selected(PickerNavKind::Folder, slint::SharedString("/videos/Anime"));
        ui->invoke_picker_breadcrumb_clicked(0);
        ui->invoke_picker_navigate_up();
        ui->invoke_picker_search_changed(slint::SharedString("dune"));
        ui->invoke_picker_sort_changed(PickerSortMode::Recent);
        ui->set_sidebar_open(false);

        // Replace every picker model, as a library snapshot does.
        ui->set_picker_folders(std::make_shared<slint::VectorModel<FolderNode>>());
        ui->set_picker_breadcrumbs(std::make_shared<slint::VectorModel<slint::SharedString>>());
        ui->set_picker_continue_watching(std::make_shared<slint::VectorModel<VideoEntry>>());
        ui->set_picker_direct_video_rows(std::make_shared<slint::VectorModel<VideoGridRow>>());
        ui->set_picker_shelves(std::make_shared<slint::VectorModel<ShelfSection>>());
        settle_animations();

        REQUIRE(ui->get_mini_state() == state);
        REQUIRE(ui->get_view_mode() == AppViewMode::Picker);
    }
}

TEST_CASE("Expand path clears mini state and morphs back to full", "[ui][mini]") {
    auto ui = make_window_in_tile();
    ui->on_mini_expand_clicked([&]() {
        ui->set_mini_state(MiniState::Hidden);
        ui->set_view_mode(AppViewMode::Player);
    });

    ui->invoke_mini_expand_clicked();
    REQUIRE_FALSE(ui->get_mini_active());
    REQUIRE(ui->get_view_mode() == AppViewMode::Player);
    settle_animations();
    REQUIRE(near(ui->get_mini_t(), 0.0f, 0.001f));
}

TEST_CASE("Pill fades out when expanding straight to the player", "[ui][mini]") {
    auto ui = make_window_in_tile("Episode 04");
    ui->set_is_playing(false);
    ui->set_mini_state(MiniState::Pill);
    read_morph(ui);
    settle_animations();
    const std::string pill_label = "Mini player minimized: Episode 04. Activate to restore.";
    REQUIRE(find_one(ui, pill_label).has_value());

    // Expand path (§6.3): the pill stays drawn while it fades over the combined morph.
    ui->set_mini_state(MiniState::Hidden);
    ui->set_view_mode(AppViewMode::Player);
    read_morph(ui);
    settle_animations(16);
    REQUIRE(ui->get_collapse_t() > 0.5f);
    REQUIRE(find_one(ui, pill_label).has_value());

    settle_animations();
    REQUIRE_FALSE(find_one(ui, pill_label).has_value());
}

TEST_CASE("Pill does not reappear after the idle close", "[ui][mini]") {
    auto ui = make_window_in_tile("Episode 04");
    ui->set_is_playing(false);
    ui->set_mini_state(MiniState::Pill);
    read_morph(ui);
    settle_animations();
    const std::string pill_label = "Mini player minimized: Episode 04. Activate to restore.";
    REQUIRE(find_one(ui, pill_label).has_value());

    // MiniPlayerController::close_on_idle(): fade out in place, then clear the state.
    ui->set_mini_closing(true);
    settle_animations(200);
    ui->set_mini_state(MiniState::Hidden);
    ui->set_mini_closing(false);
    read_morph(ui);
    settle_animations(16);
    REQUIRE(ui->get_collapse_t() > 0.5f);
    REQUIRE_FALSE(find_one(ui, pill_label).has_value());
}

TEST_CASE("Double-clicking the tile only expands", "[ui][mini]") {
    auto ui = make_window_in_tile("Episode 04");
    int expanded = 0;
    int play_pause = 0;
    int fullscreen = 0;
    ui->on_mini_expand_clicked([&]() {
        ++expanded;
        ui->set_mini_state(MiniState::Hidden);
        ui->set_view_mode(AppViewMode::Player);
        read_morph(ui);
    });
    ui->on_play_pause_clicked([&]() { ++play_pause; });
    ui->on_fullscreen_toggled([&]() { ++fullscreen; });

    // Tile body, left of the centre play/pause button and below the corner buttons.
    const slint::LogicalPosition body({1280.0f - 16.0f - ui->get_mini_w() + 40.0f,
                                       720.0f - 16.0f - ui->get_mini_h() / 2.0f});
    auto click = [&]() {
        ui->window().dispatch_pointer_move_event(body);
        ui->window().dispatch_pointer_press_event(body, slint::PointerEventButton::Left);
        ui->window().dispatch_pointer_release_event(body, slint::PointerEventButton::Left);
    };

    click();
    REQUIRE(expanded == 1);
    settle_animations(120); // mini-mode is already off: the second click hits the player
    REQUIRE_FALSE(ui->get_mini_active());
    click();
    REQUIRE(play_pause == 0);
    REQUIRE(fullscreen == 0);

    // Past the guard, a click on the full player toggles pause again.
    settle_animations(1000);
    click();
    REQUIRE(play_pause == 1);
    REQUIRE(fullscreen == 0);
}

TEST_CASE("Settings popover 30 fps option", "[ui][mini]") {
    ensure_slint_testing_backend();
    auto ui = MainWindow::create();
    ui->window().set_size(slint::LogicalSize({1280.0f, 720.0f}));
    ui->show();
    REQUIRE(ui->get_mini_fps_cap() == false);

    std::vector<bool> changes;
    ui->on_mini_fps_cap_changed([&](bool enabled) { changes.push_back(enabled); });

    ui->set_view_mode(AppViewMode::Player);
    ui->set_active_popover(ActivePopover::Settings);
    settle_animations();

    auto capped = find_one(ui, "30 fps");
    REQUIRE(capped.has_value());
    capped->invoke_accessible_default_action();
    REQUIRE(ui->get_mini_fps_cap() == true);

    auto full = find_one(ui, "Full rate");
    REQUIRE(full.has_value());
    full->invoke_accessible_default_action();
    REQUIRE(ui->get_mini_fps_cap() == false);

    REQUIRE(changes == std::vector<bool>{true, false});
}

TEST_CASE("Mini player drag below threshold clicks to expand; past threshold drags without expanding", "[ui][mini]") {
    auto ui = make_window_in_tile("Episode 04");
    REQUIRE(ui->get_mini_corner() == MiniCorner::BottomRight);

    int expanded = 0;
    ui->on_mini_expand_clicked([&]() { ++expanded; });

    // Body point away from overlay buttons.
    const float start_x = ui->get_mini_tile_x() + 40.0f;
    const float start_y = ui->get_mini_tile_y() + ui->get_mini_h() / 2.0f;
    const slint::LogicalPosition p0({start_x, start_y});
    const slint::LogicalPosition p_jitter({start_x + 3.0f, start_y + 2.0f});

    // Sub-threshold movement (< 6px) expands normally.
    ui->window().dispatch_pointer_move_event(p0);
    ui->window().dispatch_pointer_press_event(p0, slint::PointerEventButton::Left);
    ui->window().dispatch_pointer_move_event(p_jitter);
    REQUIRE_FALSE(ui->get_mini_dragging());
    ui->window().dispatch_pointer_release_event(p_jitter, slint::PointerEventButton::Left);
    REQUIRE(expanded == 1);

    // Dragging past threshold (>= 6px) enters drag mode and does NOT expand on release.
    const slint::LogicalPosition p_drag({start_x - 40.0f, start_y - 40.0f});
    ui->window().dispatch_pointer_move_event(p0);
    ui->window().dispatch_pointer_press_event(p0, slint::PointerEventButton::Left);
    ui->window().dispatch_pointer_move_event(p_drag);
    REQUIRE(ui->get_mini_dragging());
    ui->window().dispatch_pointer_release_event(p_drag, slint::PointerEventButton::Left);
    REQUIRE_FALSE(ui->get_mini_dragging());
    REQUIRE(expanded == 1);
}

TEST_CASE("Mini player drags and snaps to nearest of four corners with spring settle", "[ui][mini]") {
    auto ui = make_window_in_tile("Episode 04");
    int expanded = 0;
    ui->on_mini_expand_clicked([&]() { ++expanded; });

    const float w = ui->get_mini_w();
    const float h = ui->get_mini_h();

    auto drag_body_to = [&](float target_cx, float target_cy) {
        // Grab the tile body left of the 44px centre play/pause button.
        const float grab_off_x = 40.0f;
        const float grab_off_y = h / 2.0f;
        const float gx0 = ui->get_mini_tile_x() + grab_off_x;
        const float gy0 = ui->get_mini_tile_y() + grab_off_y;
        const float gx1 = target_cx - w / 2.0f + grab_off_x;
        const float gy1 = target_cy - h / 2.0f + grab_off_y;
        const slint::LogicalPosition p0({gx0, gy0});
        const slint::LogicalPosition p_mid({(gx0 + gx1) / 2.0f, (gy0 + gy1) / 2.0f});
        const slint::LogicalPosition p1({gx1, gy1});
        ui->window().dispatch_pointer_move_event(p0);
        ui->window().dispatch_pointer_press_event(p0, slint::PointerEventButton::Left);
        ui->window().dispatch_pointer_move_event(p_mid);
        ui->window().dispatch_pointer_move_event(p1);
        REQUIRE(ui->get_mini_dragging());
        ui->window().dispatch_pointer_release_event(p1, slint::PointerEventButton::Left);
        read_tile_pos(ui);
    };

    // Drag to top-left quadrant -> snaps to TopLeft (16, 72)
    drag_body_to(220.0f, 180.0f);
    REQUIRE(ui->get_mini_corner() == MiniCorner::TopLeft);
    REQUIRE(ui->get_mini_snapping());
    settle_animations(300);
    REQUIRE_FALSE(ui->get_mini_snapping());
    REQUIRE(near(ui->get_mini_tile_x(), 16.0f));
    REQUIRE(near(ui->get_mini_tile_y(), 72.0f));

    // Drag to top-right quadrant -> snaps to TopRight (1280 - w - 16, 72)
    drag_body_to(1080.0f, 190.0f);
    REQUIRE(ui->get_mini_corner() == MiniCorner::TopRight);
    settle_animations(300);
    REQUIRE(near(ui->get_mini_tile_x(), 1280.0f - w - 16.0f));
    REQUIRE(near(ui->get_mini_tile_y(), 72.0f));

    // Drag to bottom-left quadrant -> snaps to BottomLeft (16, 720 - h - 16)
    drag_body_to(220.0f, 580.0f);
    REQUIRE(ui->get_mini_corner() == MiniCorner::BottomLeft);
    settle_animations(300);
    REQUIRE(near(ui->get_mini_tile_x(), 16.0f));
    REQUIRE(near(ui->get_mini_tile_y(), 720.0f - h - 16.0f));

    // Drag to bottom-right quadrant -> snaps to BottomRight (1280 - w - 16, 720 - h - 16)
    drag_body_to(1060.0f, 580.0f);
    REQUIRE(ui->get_mini_corner() == MiniCorner::BottomRight);
    settle_animations(300);
    REQUIRE(near(ui->get_mini_tile_x(), 1280.0f - w - 16.0f));
    REQUIRE(near(ui->get_mini_tile_y(), 720.0f - h - 16.0f));

    // Dragging past top-left window bounds clamps cleanly inside safe padding (8px left, 64px top).
    const float gx0 = ui->get_mini_tile_x() + 40.0f;
    const float gy0 = ui->get_mini_tile_y() + h / 2.0f;
    ui->window().dispatch_pointer_move_event(slint::LogicalPosition({gx0, gy0}));
    ui->window().dispatch_pointer_press_event(slint::LogicalPosition({gx0, gy0}), slint::PointerEventButton::Left);
    ui->window().dispatch_pointer_move_event(slint::LogicalPosition({-200.0f, -200.0f}));
    REQUIRE(near(ui->get_mini_tile_x(), 8.0f));
    REQUIRE(near(ui->get_mini_tile_y(), 64.0f));
    ui->window().dispatch_pointer_release_event(slint::LogicalPosition({-200.0f, -200.0f}), slint::PointerEventButton::Left);
    read_tile_pos(ui);
    settle_animations(300);
    REQUIRE(ui->get_mini_corner() == MiniCorner::TopLeft);
    REQUIRE(near(ui->get_mini_tile_x(), 16.0f));
    REQUIRE(near(ui->get_mini_tile_y(), 72.0f));
    REQUIRE(expanded == 0);
}

TEST_CASE("Mini player can be grabbed mid-snap and redirected to another corner", "[ui][mini]") {
    auto ui = make_window_in_tile("Episode 04");
    const float w = ui->get_mini_w();
    const float h = ui->get_mini_h();

    // Start in BottomRight, drag toward TopLeft and release.
    const slint::LogicalPosition p0({ui->get_mini_tile_x() + 40.0f, ui->get_mini_tile_y() + h / 2.0f});
    const slint::LogicalPosition p_tl({120.0f, 200.0f});
    ui->window().dispatch_pointer_move_event(p0);
    ui->window().dispatch_pointer_press_event(p0, slint::PointerEventButton::Left);
    ui->window().dispatch_pointer_move_event(p_tl);
    ui->window().dispatch_pointer_release_event(p_tl, slint::PointerEventButton::Left);
    read_tile_pos(ui);
    REQUIRE(ui->get_mini_snapping());

    // Mid-flight at 90ms: grab the tile body (away from centre play button) and drag to TopRight.
    settle_animations(90);
    const slint::LogicalPosition p_grab({ui->get_mini_tile_x() + 40.0f, ui->get_mini_tile_y() + h / 2.0f});
    const slint::LogicalPosition p_tr({1000.0f, 190.0f});
    ui->window().dispatch_pointer_move_event(p_grab);
    ui->window().dispatch_pointer_press_event(p_grab, slint::PointerEventButton::Left);
    ui->window().dispatch_pointer_move_event(p_tr);
    REQUIRE(ui->get_mini_dragging());
    REQUIRE_FALSE(ui->get_mini_snapping());
    ui->window().dispatch_pointer_release_event(p_tr, slint::PointerEventButton::Left);
    read_tile_pos(ui);

    REQUIRE(ui->get_mini_corner() == MiniCorner::TopRight);
    settle_animations(300);
    REQUIRE(near(ui->get_mini_tile_x(), 1280.0f - w - 16.0f));
    REQUIRE(near(ui->get_mini_tile_y(), 72.0f));
}

TEST_CASE("Mini pill, picker inset, and window resize honor the snapped corner", "[ui][mini]") {
    auto ui = make_window_in_tile("Episode 04");

    SECTION("TopLeft corner has zero bottom inset and collapses pill to TopLeft") {
        ui->set_mini_corner(MiniCorner::TopLeft);
        REQUIRE(near(ui->get_picker_bottom_inset(), 0.0f));
        REQUIRE(near(ui->get_picker_top_clearance(), 72.0f + ui->get_mini_h() + 16.0f));
        REQUIRE(near(ui->get_mini_tile_x(), 16.0f));
        REQUIRE(near(ui->get_mini_tile_y(), 72.0f));

        ui->set_is_playing(false);
        ui->set_mini_state(MiniState::Pill);
        read_morph(ui);
        settle_animations();
        REQUIRE(near(ui->get_picker_bottom_inset(), 0.0f));
        REQUIRE(near(ui->get_picker_top_clearance(), 72.0f + 40.0f + 16.0f));
        REQUIRE(near(ui->get_mini_pill_x(), 16.0f));
        REQUIRE(near(ui->get_mini_pill_y(), 72.0f));

        ui->set_mini_state(MiniState::Tile);
        read_morph(ui);
        settle_animations();
        REQUIRE(near(ui->get_mini_tile_x(), 16.0f));
        REQUIRE(near(ui->get_mini_tile_y(), 72.0f));
    }

    SECTION("BottomLeft corner keeps bottom inset and collapses pill to BottomLeft") {
        ui->set_mini_corner(MiniCorner::BottomLeft);
        REQUIRE(near(ui->get_picker_bottom_inset(), ui->get_mini_h() + 32.0f));
        REQUIRE(near(ui->get_picker_top_clearance(), 0.0f));

        ui->set_is_playing(false);
        ui->set_mini_state(MiniState::Pill);
        read_morph(ui);
        settle_animations();
        REQUIRE(near(ui->get_picker_bottom_inset(), 72.0f));
        REQUIRE(near(ui->get_mini_pill_x(), 16.0f));
        REQUIRE(near(ui->get_mini_pill_y(), 720.0f - 40.0f - 16.0f));
    }

    SECTION("TopRight corner stays anchored on window resize") {
        ui->set_mini_corner(MiniCorner::TopRight);
        ui->window().set_size(slint::LogicalSize({1600.0f, 900.0f}));
        REQUIRE(near(ui->get_mini_tile_x(), 1600.0f - ui->get_mini_w() - 16.0f));
        REQUIRE(near(ui->get_mini_tile_y(), 72.0f));
    }
}

TEST_CASE("Interrupting a drag with expand or minimize resets drag state cleanly", "[ui][mini]") {
    auto ui = make_window_in_tile("Episode 04");
    const float h = ui->get_mini_h();

    const slint::LogicalPosition p0({ui->get_mini_tile_x() + 40.0f, ui->get_mini_tile_y() + h / 2.0f});
    const slint::LogicalPosition p_tr({1000.0f, 190.0f});
    ui->window().dispatch_pointer_move_event(p0);
    ui->window().dispatch_pointer_press_event(p0, slint::PointerEventButton::Left);
    ui->window().dispatch_pointer_move_event(p_tr);
    REQUIRE(ui->get_mini_dragging());

    SECTION("Minimize mid-drag snaps pill to the hovered corner") {
        ui->set_is_playing(false);
        ui->set_mini_state(MiniState::Pill);
        read_morph(ui);
        read_tile_pos(ui);
        settle_animations(16); // flush deferred Slint `changed mini-state` callback
        REQUIRE_FALSE(ui->get_mini_dragging());
        REQUIRE(ui->get_mini_corner() == MiniCorner::TopRight);
        settle_animations();
        REQUIRE(near(ui->get_mini_pill_x(), 1280.0f - ui->get_pill_w() - 16.0f));
        REQUIRE(near(ui->get_mini_pill_y(), 72.0f));
    }

    SECTION("Expand mid-drag clears drag state and returns to Player") {
        const float drag_x = ui->get_mini_tile_x();
        const float drag_y = ui->get_mini_tile_y();
        ui->set_mini_state(MiniState::Hidden);
        ui->set_view_mode(AppViewMode::Player);
        read_morph(ui);
        settle_animations(16); // flush deferred Slint `changed mini-state` callback
        REQUIRE_FALSE(ui->get_mini_dragging());
        // The morph starts from the dragged position: no corner commit, no snap (§12.4C).
        REQUIRE_FALSE(ui->get_mini_snapping());
        REQUIRE(ui->get_mini_corner() == MiniCorner::BottomRight);
        read_tile_pos(ui);
        settle_animations(100);
        REQUIRE(near(ui->get_mini_tile_x(), drag_x));
        REQUIRE(near(ui->get_mini_tile_y(), drag_y));
        settle_animations();
        REQUIRE(near(ui->get_mini_t(), 0.0f, 0.001f));
    }
}

TEST_CASE("Other mouse buttons mid-drag never leave the tile stuck or expand", "[ui][mini]") {
    auto ui = make_window_in_tile("Episode 04");
    int expanded = 0;
    ui->on_mini_expand_clicked([&]() { ++expanded; });
    const float h = ui->get_mini_h();

    const slint::LogicalPosition p0({ui->get_mini_tile_x() + 40.0f, ui->get_mini_tile_y() + h / 2.0f});
    const slint::LogicalPosition p1({p0.x - 200.0f, p0.y - 100.0f});
    ui->window().dispatch_pointer_move_event(p0);
    ui->window().dispatch_pointer_press_event(p0, slint::PointerEventButton::Left);
    ui->window().dispatch_pointer_move_event(p1);
    REQUIRE(ui->get_mini_dragging());
    const float x1 = ui->get_mini_tile_x();

    // Pressing another button keeps the drag going: the tile still tracks 1:1.
    ui->window().dispatch_pointer_press_event(p1, slint::PointerEventButton::Right);
    REQUIRE(ui->get_mini_dragging());
    const slint::LogicalPosition p2({p1.x - 50.0f, p1.y});
    ui->window().dispatch_pointer_move_event(p2);
    REQUIRE(near(ui->get_mini_tile_x(), x1 - 50.0f));

    // Releasing it ends Slint's pointer grab, so the drag ends there and snaps.
    ui->window().dispatch_pointer_release_event(p2, slint::PointerEventButton::Right);
    REQUIRE_FALSE(ui->get_mini_dragging());
    REQUIRE(ui->get_mini_snapping());
    REQUIRE(ui->get_mini_corner() == MiniCorner::BottomRight);

    // The left release afterwards is neither a click nor a new drag.
    ui->window().dispatch_pointer_release_event(p2, slint::PointerEventButton::Left);
    REQUIRE(expanded == 0);
    read_tile_pos(ui);
    settle_animations(300);
    REQUIRE_FALSE(ui->get_mini_snapping());
    REQUIRE(near(ui->get_mini_tile_x(), 1280.0f - ui->get_mini_w() - 16.0f));
}

TEST_CASE("Pressing the tile mid-snap freezes it without a jump", "[ui][mini]") {
    auto ui = make_window_in_tile("Episode 04");
    int expanded = 0;
    ui->on_mini_expand_clicked([&]() { ++expanded; });
    const float h = ui->get_mini_h();

    // Throw the tile from BottomRight toward TopLeft.
    const slint::LogicalPosition p0({ui->get_mini_tile_x() + 40.0f, ui->get_mini_tile_y() + h / 2.0f});
    const slint::LogicalPosition p_rel({500.0f, 300.0f});
    ui->window().dispatch_pointer_move_event(p0);
    ui->window().dispatch_pointer_press_event(p0, slint::PointerEventButton::Left);
    ui->window().dispatch_pointer_move_event(p_rel);
    ui->window().dispatch_pointer_release_event(p_rel, slint::PointerEventButton::Left);
    read_tile_pos(ui);
    REQUIRE(ui->get_mini_corner() == MiniCorner::TopLeft);

    settle_animations(60);
    const float hx = ui->get_mini_tile_x();
    const float hy = ui->get_mini_tile_y();
    REQUIRE_FALSE(near(hx, 16.0f, 20.0f)); // still well short of the corner

    const slint::LogicalPosition g({hx + 40.0f, hy + h / 2.0f});
    ui->window().dispatch_pointer_move_event(g);
    ui->window().dispatch_pointer_press_event(g, slint::PointerEventButton::Left);
    REQUIRE_FALSE(ui->get_mini_snapping());

    SECTION("held still: no drift, and jitter below the threshold does not start a drag") {
        settle_animations(60);
        REQUIRE(near(ui->get_mini_tile_x(), hx));
        REQUIRE(near(ui->get_mini_tile_y(), hy));
        ui->window().dispatch_pointer_move_event(slint::LogicalPosition({g.x + 1.0f, g.y}));
        REQUIRE_FALSE(ui->get_mini_dragging());
        REQUIRE(near(ui->get_mini_tile_x(), hx));

        // Releasing is a click (expand) and lets the snap finish.
        ui->window().dispatch_pointer_release_event(slint::LogicalPosition({g.x + 1.0f, g.y}),
                                                    slint::PointerEventButton::Left);
        REQUIRE(expanded == 1);
        REQUIRE(ui->get_mini_snapping());
        read_tile_pos(ui);
        settle_animations(300);
        REQUIRE(near(ui->get_mini_tile_x(), 16.0f));
        REQUIRE(near(ui->get_mini_tile_y(), 72.0f));
    }

    SECTION("dragging after a pause moves the tile by exactly the pointer delta") {
        settle_animations(60);
        ui->window().dispatch_pointer_move_event(slint::LogicalPosition({g.x + 40.0f, g.y + 30.0f}));
        REQUIRE(ui->get_mini_dragging());
        REQUIRE(near(ui->get_mini_tile_x(), hx + 40.0f));
        REQUIRE(near(ui->get_mini_tile_y(), hy + 30.0f));
        ui->window().dispatch_pointer_release_event(slint::LogicalPosition({g.x + 40.0f, g.y + 30.0f}),
                                                    slint::PointerEventButton::Left);
        REQUIRE(expanded == 0);
    }
}

TEST_CASE("The tile cannot be dragged until the shrink morph finishes", "[ui][mini]") {
    ensure_slint_testing_backend();
    auto ui = MainWindow::create();
    ui->window().set_size(slint::LogicalSize({1280.0f, 720.0f}));
    ui->show();
    ui->set_has_video(true);
    ui->set_is_playing(true);
    ui->set_view_mode(AppViewMode::Picker);
    ui->set_mini_state(MiniState::Tile);
    read_morph(ui);
    settle_animations(150);

    const float t = ui->get_mini_t();
    REQUIRE(t >= 0.95f);
    REQUIRE(t < 1.0f);
    const float w = ui->get_mini_w();
    const float h = ui->get_mini_h();
    const float x = (1280.0f - w - 16.0f) * t;
    const float y = (720.0f - h - 16.0f) * t;
    const slint::LogicalPosition p0({x + 40.0f, y + h / 2.0f});
    ui->window().dispatch_pointer_move_event(p0);
    ui->window().dispatch_pointer_press_event(p0, slint::PointerEventButton::Left);
    ui->window().dispatch_pointer_move_event(slint::LogicalPosition({p0.x - 80.0f, p0.y - 80.0f}));
    REQUIRE_FALSE(ui->get_mini_dragging());
    ui->window().dispatch_pointer_release_event(slint::LogicalPosition({p0.x - 80.0f, p0.y - 80.0f}),
                                                slint::PointerEventButton::Left);
    REQUIRE_FALSE(ui->get_mini_snapping());
    REQUIRE(ui->get_mini_corner() == MiniCorner::BottomRight);
}

TEST_CASE("The snap overshoot stays inside the drag pads on a large window", "[ui][mini]") {
    auto ui = make_window_in_tile("Episode 04");
    ui->window().set_size(slint::LogicalSize({2560.0f, 1440.0f}));
    ui->set_mini_corner(MiniCorner::BottomLeft);
    const float w = ui->get_mini_w();
    const float h = ui->get_mini_h();
    REQUIRE(near(ui->get_mini_tile_x(), 16.0f));

    // Release just right of centre: about 1000px of travel to the right corner.
    const slint::LogicalPosition p0({ui->get_mini_tile_x() + 40.0f, ui->get_mini_tile_y() + h / 2.0f});
    const slint::LogicalPosition p1({1060.0f + 40.0f, p0.y});
    ui->window().dispatch_pointer_move_event(p0);
    ui->window().dispatch_pointer_press_event(p0, slint::PointerEventButton::Left);
    ui->window().dispatch_pointer_move_event(p1);
    ui->window().dispatch_pointer_release_event(p1, slint::PointerEventButton::Left);
    read_tile_pos(ui);
    REQUIRE(ui->get_mini_corner() == MiniCorner::BottomRight);

    float max_x = 0.0f;
    for (int i = 0; i < 20; ++i) {
        settle_animations(16);
        max_x = std::max(max_x, ui->get_mini_tile_x());
    }
    REQUIRE(max_x <= 2560.0f - w - 8.0f + 0.5f);
    settle_animations(300);
    REQUIRE(near(ui->get_mini_tile_x(), 2560.0f - w - 16.0f));
}
