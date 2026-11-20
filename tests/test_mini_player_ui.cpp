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
